# R8. The Bun half of the AOT compiler and Bun's native runtime, as far as a `Thread` API touches them

Trees read (read only):

- Bun half: `<bun>/` (oven-sh/bun#45631, head 8bb8d41bffe). Paths below that begin with `src/`,
  `docs/`, `scripts/`, `packages/`, `test/` are relative to it.
- Engine half: `Source/JavaScriptCore/`. Paths that begin with `aot/`, `ffi/`, `heap/` are
  relative to it. `WTF/` is `Source/WTF/wtf/`.
- Earlier threads work: `<bun-threads-branch>/` (commit 3fd4c405c1b2) and
  `<threads-branch>/docs/threads/` ("threads docs").

Words used here:

- **front end**: the Rust crate `bun_aot` (`src/aot`) plus `src/sema`, everything before the call `Bun__AOT__compile`.
- **engine**: JavaScriptCore's `aot/`, behind that call at build time and behind `Bun__AOT__useImage` at run time.
- **table**: the table of built-ins (6,330 entries, 271 classes by the PR text).
- **Bun VM**: the Rust struct `VirtualMachine` (`src/jsc/VirtualMachine.rs:159`). **JSC VM**: `JSC::VM`.
- **owner thread**: the one OS thread on which a Bun VM was made by `VirtualMachine::init*` and whose thread-local slot names it.
- **second mutator**: a hypothetical OS thread that runs JavaScript in the same JSC VM, realm and `AOT::Instance` as the owner thread.

One correction to the task text first: **this tree has no `.classes.ts` files.** `find src -name '*.classes.ts'` finds 0.
Native classes are declared in 79 `*.bind.ts` files (`src/codegen/bindgen*.ts` is the generator). The r17 commit patched
`src/codegen/generate-classes.ts`, which no longer exists here. Its counterpart is `src/codegen/bindgen-emit.ts`.

---

## 1. The build pipeline, stage by stage

### 1.1 From the command line to the front end

| Step | Where | What goes in, what comes out |
| --- | --- | --- |
| `bun` hands over to `bun-aot` | `src/runtime/cli/compile_command/hand_over.rs:42-83` | A `bun` without the compiler looks for `bun-aot` beside itself (`find_beside`, :9), else in the `--target` cache or npm (`find_or_fetch`, :21), and runs it with the same argv by `spawn_sync_inherit` (:70). It exits with the child's code |
| Flags to options | `src/runtime/cli/compile_options.rs` | `AotOptions` (`src/options_types/aot_options.rs:57-79`): `heap`, `explain: ExplainSet`, `trim: TrimRequest`, `reports_type_mismatches`, `forbids_text_evaluation`, `admits_inspector`, `runs_in_place`, `checks_only`, `runs_tests`, `thread_count`, `option_words` (developer flags passed to the engine as words) |
| Request | `src/runtime/cli/compile_command.rs:60-99` | `CompileRequest { origin, product, options, entry_points, bundler, executable, log }`. `CompileProduct` is `Executable{outfile}`, `GraphBytes`, `GraphBytesIfSourceCannotRun`, `ReportsOnly` |
| Driver of one build | `compile_to_executable`, `src/runtime/cli/compile_command/build.rs:39-92` | `has_compiler()` -> `check_request` (no cross compile, template identity) -> `bun_jsc::initialize{is_compiler:true}` -> `build_program` -> `used_features(&build.engine)` -> `decide_trimming` -> `collect_reports` -> `write_product` -> `write_build_report` |
| Bundler services | `src/bundler/aot/mod.rs:79-` (`build`) | Turns the bundler's options into `bun_aot::Options` (`src/aot/options.rs:10-21`: `aot`, `executable_identity`, `image_form`, `entry_points`, `code`, `typescript_libs`, `load`, `paths`, `removable`), supplies plugins, macros, assets, loaders as a `SourceProvider` |

### 1.2 The front end (`src/aot/compile.rs`)

`compile()` (`src/aot/compile.rs:278-340`) in order:

1. `EngineOptions::new(&EngineRequest{options, identity, image_form, thread_count})` (:284-306). **The engine's option record is made
   before anything of the program has been read.** It stays `mut`, and one field is changed later (step 4).
2. `Build::run_through_emit` -> `run_front_end` (:403-423):

   | Stage (`Stage` enum, `aot/ir/schema/limits.def:1-4`) | Function | In | Out |
   | --- | --- | --- | --- |
   | `Load` | `with_loaded_projects_before_assets` -> `load_projects_once`, `src/aot/compile/load_stage.rs:257-352` | `Options`, `SourceProvider` | `LoadedBuild` (`compile.rs:141-151`): two projects, **Application** and **Runtime** (Bun's own `src/js`), each a `LoadedProgram` (files, HIR, bindings) plus `Sources`. Also `builtin_names`, `typed_packages`, `can_load_code_at_run_time` |
   | `BindModules` | `bind_modules`, `src/aot/compile/bind_stage.rs:51-87` | `LoadedBuild` | `BoundBuild`: per project `ModuleGraph`, `LeftOutCode`, `KnownHelpers`, `DeferredData`, `BuiltinTypeSymbols` |
   | `Check` + `Lower` | `check_and_lower`, `src/aot/compile/lower_stage.rs:114-197` | both of the above | `Vec<LinkUnit>`: one IR unit per file, made on all threads. `find_reached_runtime_code` (:155) drops the files and functions of `src/js` that nothing reaches |
   | `LinkAndEmit` | `link`, `src/aot/compile/link_stage.rs:104-177`, then `emit_*` (`compile.rs:438-482`) | units | `FrontEndOutput { program, body_remap, names, texts, entry_points, folders, validates_by_type }` (`compile.rs:171-179`), then `ProgramBytes` in the wire format of `aot/ir/AOTSchema.def` |

   The Application is loaded first. Then `collect_runtime_roots(application.files(), ...)` (`load_stage.rs:279-280`) finds which
   built-in modules and functions the program names, and only then is the Runtime project loaded, with **facts about the program
   folded into Bun's own sources as constants** (1.4 below).
3. `build.write_program_file` (`--aot-out`).
4. `if options.aot.checks_only && !output.validates_by_type { engine_options.check_only(); }` (`compile.rs:320-322`). **This is the one
   place today where a fact found in the program changes the engine's options.**
5. `run_engine` (:498-529) -> `engine::compile(bytes, options)` -> FFI `Bun__AOT__compile(program, external_bodies, release_program,
   owner, &CompileOptions, &HostThreadPool)` (`src/aot/engine.rs:221-246`, declaration :453-460). Result: `EngineResult`, an opaque C++ object read
   section by section (`ResultSection`, `aot/ir/schema/compile-call.def:61-65`): `Image Text UserDiagnostics InternalErrors EntryModuleIds
   UnseenCode BuiltinsUsed FeaturesUsed LocalesUsed Accesses AccessCounts TypeTests RemainingChecks Declines AllocationRemarks Losses
   Functions OperationCounts PassTimes Statistics LimitValues Symbols ObfuscatedSites Arrival*`.
6. `diagnostics.add_engine_records(&engine, names, texts)` (:516-517) turns the engine's records into messages.

### 1.3 After the engine

`src/runtime/cli/compile_command/build.rs`:

- `used_features(&build.engine)` (`src/aot/engine/used_features.rs:12-22`) reads `FeaturesUsed`, `UnseenCode`, `LocalesUsed` into
  `bun_trim::UsedFeatures`.
- `trimming::decide_trimming` -> `bun_trim::decide` (`src/trim/decide.rs:53-`) -> `TrimPlan`.
- `write_product` (:95-182): `GraphRequest{ compiled_program: Some(CompiledProgramInput{ image, image_alignment, entry_points,
  package_names, preserves_source_paths, symbols }) }`, then either `to_graph_bytes` (no file: `bun run --aot`) or `write_executable`
  with `ExecutableRequest{ graph, outfile, executable_path, trim: plan }`.
- The template is **this process's own executable** unless `--compile-executable-path` names one:
  `target_executable`, `src/standalone_graph/StandaloneModuleGraph.rs:2998-3015`. `check_template` (`build.rs:237-266`) refuses a template whose
  Git revision, stamp of built-in modules, engine description or table description differs (`find_difference`, :268-303).

### 1.4 Where "this program uses Thread" could be detected

There are four existing mechanisms. Each has a precedent that can be copied line for line.

**(D1) At load, by import of a module.** `collect_runtime_roots`, `src/aot/load/runtime_roots.rs:60-83`:

```rust
let imports_cluster = imported_modules.contains(&RuntimeModule::NodeCluster);
```

`imported_modules` are the host modules that any compiled file of the application resolves an import site to, plus modules named in
`require(flag ? "a" : "b")` (`RootUses`, :17-58). The result becomes `ProgramFacts` (`src/aot/load/fold_inputs.rs:24-29`):

```rust
// What the build tells Bun's own sources about the program. They are read after the program.
pub(crate) struct ProgramFacts { pub can_load_code_at_run_time: bool, pub can_be_cluster_worker: bool }
```

and three folded constants (`fold_inputs.rs:79-87`): `CAN_LOAD_CODE_AT_RUN_TIME`, `CAN_BE_CLUSTER_WORKER`, `IS_COMPILED_AHEAD_OF_TIME`. They are declared
for `src/js` in `src/js/builtins.d.ts:44-48`, get their JIT-build values in `src/codegen/replacements.ts:140-144`, and are used for example in
`src/js/internal/cluster/isPrimary.ts:6`, `src/js/node/_http_server.ts:1029`, `src/js/node/async_hooks.ts:110,233,391`. Note the widening:
`can_be_cluster_worker: roots.imports_cluster || can_load_code_at_run_time` (`load_stage.rs:294`): code the build does not see may name the module too.
Properties: earliest possible (before Bun's own sources are read, before the check), an over-approximation (an import that nothing calls counts), and it
only works if `Thread` comes from a module of its own (`bun:threads`), not from a global or from `"bun"`.

**(D2) At link, from the IR.** `validates_by_type` (`src/aot/compile/link_stage.rs:166-167`):

```rust
let validates_by_type = expected_results.any(|expected| !expected.validation.is_empty());
```

It travels `FrontEndOutput` -> `ProgramSummary.validates_by_type` (`compile.rs:66`) -> `bun_aot_interface::compile` (`src/aot/interface/compile.rs:19`) ->
`AotBuild.validates_by_type` (`src/bundler/aot/mod.rs:69`, `output.rs:54,67`) -> `build.rs:75`. It is computed after dead files and functions have been left
out (`LeftOutCode`, `find_reached_runtime_code`), and before the engine runs. So it can still change `EngineOptions`.

**(D3) In the engine's flow analysis, per built-in.** After flow, the engine returns `BuiltinUseRecord { builtin, uses: BuiltinUse, witness }` with
`flags BuiltinUse { Tested Read Called Constructed Written Escaped }` (`aot/ir/schema/compile-call.def:16,79`) and `FeatureUseRecord { feature, builtin?, witness }`
(:80). This is the precise answer ("some reached call constructs `Thread`, here is the source position"), found "by following the functions as values"
like `eval`. A program-wide fact derived the same way inside the engine: `countProgramInstances`,
`aot/valueflow/AOTValueFlowResultsBuiltins.cpp:1184-1206`. It walks every built-in with `BuiltinFlag::StartsInstance` and answers `ProgramInstances::Unknown` if
unseen code can find every built-in, if the built-in has a use that enters code, or if its object has escaped or is lost. Otherwise `AtMostOne`. Its comment
matters for this design: *"The instances of one VM are counted: no object of another VM gets here, so a worker, which runs the program once more in a VM of
its own, is none of them."* The closed-program decision (`ImageFlag::AdmitsUnknownJavaScript`, `aot/image/AOTImageFormat.h:118-124`) is made the same way and
recorded in the image header's `flags`.

**(D4) By an explicit flag.** `AotOptions` field + `fill_record` (`src/aot/engine.rs:125-166`), as `forbids_text_evaluation` -> `ExcludedCode::EVALUATED_TEXT`.

What each one can and cannot feed:

| | known before Bun's `src/js` is read | known before `CompileOptions` is final | precise (dead code ignored, values followed) | has a source position |
| --- | --- | --- | --- | --- |
| D1 import | yes | yes | no | the import site |
| D2 IR at link | no | yes | partly (left-out code is gone, values are not followed) | in the IR |
| D3 flow | no | no: the engine must act on it itself | yes | `Witness` |
| D4 flag | yes | yes | n/a | n/a |

A program with `eval`, `--compile-inspector` or externals is not closed (`Options::can_load_code_at_run_time`, `src/aot/options.rs:118-120`). For such a program D3
must answer "may use Thread" unless `Thread` is unreachable from text (see 4.6).

### 1.5 Turning it into a build option for the engine

`record CompileOptions abi` (`aot/ir/schema/compile-call.def:139-144`) is generated into both halves from the schema. Existing shapes that fit:

- a bit in an existing flags word: `flags AdmittedCode { Inspector }`, `flags ExcludedCode { EvaluatedText FunctionSource }`, `flags OutputOptions { Obfuscate }` (:128-130);
- a field of `TargetOptions { features, architecture, referenceWidth, imageForm, reserved }` (:138), as `--aot-huge-heap` is `ReferenceWidth::Wide` (`engine.rs:135-138`);
- there is a spare `reserved: UInt32` in both records.

`CompileOptions.size` and the schema hash guard against two halves built from different schemas (`InternalErrorKind::SchemaMismatch`, `diagnostics.def:44`).
What the image was compiled with is written into `ImageHeader.options` (`RecordedOptions`) and `ImageHeader.flags`, and `useImage` refuses a process whose options
differ (`ImageVerdict::OptionsConflict`, sentence in `src/jsc/compiled_program.rs:475`). So "compiled for several mutators" has a natural home in
`RecordedOptions` or `ImageFlag`, and the run time can read it before the first VM exists: `admitted_code_of` and `admits_unknown_javascript`
(`compiled_program.rs:81-93`) already read the header "before `use_image` has run".

Developer flags go another way: `--aot-<key>` words (`DEVELOPER_FLAGS`, `src/runtime/cli/compile_options.rs:59-82`) are handed to
`Bun__AOT__applyOptionWord` (`engine.rs:71-93`), which the engine parses. A testing switch such as "compile as if threads were used" costs no Bun change beyond one
line in that list.

### 1.6 Reporting it

**`--explain=<topic>`.** To add a topic:

1. a bit in `ExplainSet` (`src/options_types/aot_options.rs:14-30`; `u16`, 12 of 16 bits used), and the test list `MEMBERS` (:111);
2. a name in `EXPLAIN_NAMES` and the sentence `EXPLAIN_ALLOWED` (`src/runtime/cli/compile_options.rs:25-44`), the help text in `src/runtime/cli/Arguments.rs:671`;
3. which engine reports it needs: `reports_of` (`src/aot/engine.rs:95-123`) maps topics to `flags ReportSet { Flow Declines Symbols Allocations Functions Arrivals PassCodeBytes }`;
4. a collector module under `src/aot/report/` and a line in `collect_entries` (`src/aot/report/mod.rs:61-97`);
5. how its lines print: `src/aot/report/message.rs:12-27` (JSON, message with position, or prefixed `aot:` line), table in `src/aot/report/entry.rs:114-125`;
6. `Bun.build`: `compile.aotExplain` (`COMPILE_AOT_KEYS`, `compile_options.rs:24`);
7. the docs page `docs/bundler/aot-explain.mdx:981-1013` ("More reports"). `test/cli/aot-docs.test.ts` runs every documented command (PR text).

`src/aot/report/runtime_use.rs` (`--explain=trim`) is the closest model: it prints `uses <builtin>` for each `BuiltinUseRecord` whose uses are more than
`Tested` (:100-122), and "blocked by ..." lines with the witness position (:78-98).

**Build report** (`--aot-report <path>`, `--aot-report-detail`): `src/aot/report/build_report.rs`. Top-level keys: `schemaHash`, `buildId`, `summary`, `statistics`,
`limits`, `passes`, `stages`, `retainedFeatures`, `unseenCode`, `functions`, `accessSites`, `typeTestSites` (:46-238). A boolean or a list of witnesses is added there.
`--explain=statistics` and the report's `statistics` object print every `StatisticId` of `aot/ir/schema/statistics.def`. A counter added to the schema shows up
in both with no further code. `src/aot/report/registry.rs` writes the list of all limits, statistics, flow results, passes, operations, features.

### 1.7 Rejecting with a diagnostic

Two registries, which share one number space (TS1000xx):

- **Front end**: the macro table in `src/aot/diagnostic_codes.rs:32-108`. One line per code: `Name = number, Severity, "template with {0} {1}";` inside a section
  with a number range (`Build 100050..=100099`, `Fold`, `Resolve`, `Link`, `Lower 100300..=100699`, `Builtins 100700..=100749 {}` which is empty). Rule (:31): *"A new code
  takes the next free number of its section. The number of a deleted line goes into `RETIRED_NUMBERS`, and no code takes a number of `UserDiagnosticKind::code`."* Unit
  tests enforce uniqueness, ranges and placeholders (:162-231). It is raised with `Diagnostic::new(Code::X, position, &arguments)` + `diagnostics.add(..)`
  (`compile.rs:493-494`). `build.stop_if_errors(stage)` ends the build after the stage.
  The `validate()` family is the model for misuse of a compile-only API: `ValidateIsNotAValue = 100509, "validate() from \"bun:bundle\" can only be called. It can't be
  assigned, passed as an argument or exported"`, `ValidateNeedsTypeScript`, `ValidateOptionalCall`, `JsonFromBunIsExported` (:94-104), raised in `src/aot/lower/validate/misuse.rs`.
  `UnsupportedOption = 100050, "--aot does not support {0}"` is the model for a flag conflict.
- **Engine**: `enum UserDiagnosticKind (severity, stage, code, message)` in `aot/ir/schema/diagnostics.def:4-34`. The message takes `{limit} {count} {function}`. The record
  carries function, block, value, layout, location, origin (`UserDiagnosticRecord`, `compile-call.def:70-73`). Flow-stage examples:
  `LockedBuiltinWriteAlwaysRuns(Error, AnalyzeFlow, 100014, ...)`, `LockedBuiltinWriteMayRun(Warning, ...)`, `CodeOutsideImageIsRefused(Warning, AnalyzeFlow, 100064, ...)`. That trio shows the house
  style for a fact flow can only half prove: an **Error** where it always happens, a **Warning** where it may. `src/aot/diagnostics/engine_records.rs` turns records into messages.

Both kinds are reported by `bun check --aot` (`src/runtime/cli/check_command/compile_check.rs:60`, `CompileProduct::ReportsOnly`) and by `bun run --aot`, because the
check "ends where the compiler would start to generate code" (`docs/bundler/aot-debugging.mdx:56`): `CompileAction::CheckOnly` still runs flow.
A diagnostic of stage `GenerateCode` or `BuildImage` is not seen there.

---

## 2. Workers under `--aot`

### 2.1 Build time: a Worker's file is an entry point only if the user passes it

- Docs: *"Pass the worker as an additional entry point, like in any executable. It's compiled with the rest."* (`docs/bundler/aot-compatibility.mdx:661`), and
  `bun compile ./spawn.ts ./worker.ts --outfile spawn` (:678).
- I found **no code that turns `new Worker(new URL("./w.ts", import.meta.url))` or `new Worker("./w.ts")` into an entry point**. `grep -i worker` over `src/aot`, `src/bundler/aot`,
  `src/standalone_graph`, `src/runtime/cli/compile_command` finds only: the host-read contexts (below), test names, comments. `implied_answers` (`src/aot/load/builtin_uses.rs:131-151`), which the
  comment in `src/aot/compile/program_reach.rs:1043` describes as "A file that the host loads for a call in this one, as for a worker", is filled with built-in modules, files of built-in
  functions, evaluated texts and React declarations, never with a user file named by a Worker call.
- The table row (`src/codegen/builtins/host/handwritten-classes.builtins.ts:20-25,141-152`): `Worker` is `constructorOnly`, `extends: "EventTarget"`, two `construct` overloads, `capability: "js"`,
  `throws: true`, `implementation: b.hostCall`, `uses: { url: [b.loadedAsModule] }` (role `ModuleSpecifier`), and for the general overload `url` and `options` are `b.everything`.
- `--trim`: `new Worker()` counts as `blocked by dynamic-import`: *"It finds its file by name. Only an entry point of the build can run"* (`docs/bundler/aot-size.mdx:178`).
- Unseen code: `Worker` is listed under `seenCodeEntries` with the reason *"A worker has a heap of its own and starts with an entry point of the build: the loader of its realm refuses
  everything else."* (`src/codegen/builtins/host/unseen-code.builtins.ts:41-51`). So a program with Workers can still be closed.
- Each entry point gets an IR `Entry { module, host_reads }` (`src/aot/link/entries.rs:33-56`). `EntryPointKind::Main` gets the host reads of context `EntryPoint`. `Additional` gets `EntryPoint` and
  `Worker` (:10-14). The rows are `src/codegen/builtins/host/host-reads.builtins.ts:6-15`: native code reads `default` (passes it to `Bun.serve`) and `then`. The contexts are
  `hostReadContexts = ["EntryPoint","Worker","Preload","Plugin","TestFile"]` (`aot/builtins/generator/vocabulary.ts:221`).
- The whole program is **one** image with one set of layouts, shapes, functions. All entry points share it. `import.meta.main` is folded at build time in a one-entry executable
  (`aot-compatibility.mdx:687`).

### 2.2 Run time: how a name finds its entry point

`resolve_entry_point_specifier` (`src/jsc/web_worker.rs:1278-1374`), called twice: on the parent thread for preloads (`create`, :359) and on the worker thread for the specifier (`spin`, :840):

1. `graph.resolve(base_public_path, str)` (:1288-1292): a name of the embedded graph.
2. `transpiler.resolver.resolve_embedded_module(root, str)` then `compiled_program::find_entry_point_by_module_path` -> `module_key(entry)` (:1295-1304).
3. `data:` and `blob:` pass through (and then fail to load, `aot-compatibility.mdx:685`).
4. Otherwise the ordinary resolver, then `find_entry_point_by_source_path` (:1358-1363), for `--compile-in-place` and `bun run --aot`.

### 2.3 Run time: how a Worker starts (order of events)

Parent thread, `WebWorker::create` (`src/jsc/web_worker.rs:301-525`):

1. Copies what the child needs **by value**: `transform_options.clone()` (:374), `env_loader().clone_for_worker()` under the `proxy_env_storage` lock (:407-417), argv pointers borrowed from the C++ proxy.
2. `parent_ref.ticket()` (:476): a counted `Ticket` on the parent VM, held by the thread for its whole life.
3. `std::thread::Builder::new().stack_size(DEFAULT_THREAD_STACK_SIZE).spawn(..)` (:496-501). `parent.child_workers.push(worker)` (:508).

Worker thread, `thread_main` (:640-683):

4. `start_vm` (:689-800): a new `bun_alloc::Arena`, `VirtualMachine::init_worker(self, Options{ args, env_loader, graph: standalone_module_graph(), .. })` (:718).
5. Inside `init` (`src/jsc/VirtualMachine.rs:3425-3480`): `hooks.init_runtime_state(vm, &mut opts)` makes the per-VM `RuntimeState` (timers, DNS, SQL, ...) **before** the global object; `regular_event_loop.ensure_waker()`;
   then

   ```rust
   if opts.is_main_thread {
       if let Some(graph) = standalone_module_graph() { crate::compiled_program::use_image(graph); }
   }
   let global = Zig__GlobalObject__create(console, context_id, mini_mode, eval_mode, worker_ptr);
   ```

   So `use_image` runs **only on the main thread**, under a `std::sync::Once` (`src/jsc/compiled_program.rs:333-386`).
6. `Zig__GlobalObject__create` (`src/jsc/bindings/ZigGlobalObject.cpp:597-`): `JSC::VM::tryCreate(heapSize)` (:600), **a new JSC VM, so a new heap**; `vm.heap.acquireAccess()`; `JSLockHolder`;
   `JSVMClientData::create(&vm, Bun__getVM(), worker_ptr)` (:639), where `Bun__getVM()` reads the thread-local; `Bun::requireCellCage()` (:671); a new `Zig::GlobalObject` with
   `methodTableOfProgram(executionContextId)` (:582-595), which in a standalone executable is `StandaloneGlobalObject::globalObjectMethodTable()`, whose `moduleHost` is `&Bun::compiledModuleHost` (:1273).
7. The engine sees a realm whose method table has a module host: `AOT::RuntimeHooks::didCreateRealm` -> `makeBuiltinsImmutable`, `Instance::create(realm)` (`aot/runtime/AOTRuntimeHooks.cpp:80-94`).
   `Instance::create` (`aot/runtime/AOTInstance.cpp:387-424`) fills the realm words, collector words, stub words, makes `State`, registers a stack mirror and appends itself to
   `vm.aotState().instances` under `instancesLock`. Bun then installs its globals and calls `JSC::AOT::hostDidCreateRealm(this)` (`ZigGlobalObject.cpp:3035`) -> `didInstallBuiltins`.
8. Back in `thread_main`: `JSC__VM__getAPILock(global.vm())` (:681): the worker thread takes the API lock of its own VM and never gives it back.
9. `spin` (:802-): `workerThreadStarted`, resolve the specifier, for `node:worker_threads` load that module, `vm.load_entry_point_for_web_worker(path)` (:890), then the loop
   `while !stopped_by_entry && vm.is_event_loop_alive() { vm.tick(); ...; vm.auto_tick_active(); }` (:978-).
10. `shutdown` (:1033-): exit handlers, join own children, JSC VM teardown, free the per-thread state, `workerGlobalScopeDestroyed`.

**Answer: new OS thread, new Bun VM, new JSC VM and heap (with a 32 GB cell cage of its own), new realm, new `AOT::Instance`; one image, mapped once.**

### 2.4 What is shared between Workers today

| Shared | Kind | Evidence |
| --- | --- | --- |
| The image: code + 27 read-only sections | read only, mapped once | `use_image` under `Once`; `ImageVerdict::AlreadyInUse` = "this process has already loaded another compiled program" (`compiled_program.rs:470`); `JSC::AOT::Image::current()` is a process singleton (`BunAOTRuntime.cpp:69,135`) |
| `StaticStringImpl`s of the image (identifiers, literals) | read only, but see atoms below | PR text. `test/aot/the-atoms-a-worker-makes-of-the-programs-texts-end-with-the-worker.test.ts:6-7`: *"The atom that native code makes of a string constant of compiled code is held by the atom table of its thread ... A Worker's thread has a table of its own"* |
| `StandaloneModuleGraph` singleton | immutable after start | `src/standalone_graph/StandaloneModuleGraph.rs:132,356` ("populated once at startup before any worker threads", "shared by the main VM, Workers and resolver threads") |
| `find_module_path`'s set of leaked paths | `Guarded<BTreeSet>` (mutex) | `compiled_program.rs:157-182` |
| The table of built-ins and host bindings | read only | `Bun__AOT__registerHostBindings` once (`BunAOTRuntime.cpp:105-106`) |
| FFI callback entry pool (256 entries) | process-wide, `WTF::Lock` | `ffi/FFICallbackEntryPool.h:52-57` |
| Type-mismatch log descriptor | process-wide; counts per Worker | `Bun__AOT__setMismatchLog`; *"Each `Worker` counts for itself"* (`aot-debugging.mdx:150`) |
| JSC options, `g_jscConfig` | frozen after the first VM | `JSCInitialize` once (`ZigGlobalObject.cpp:321-323`) |
| Thread pool, HTTP thread, process env store, `BroadcastChannel`, `MessagePort`, `SharedArrayBuffer` contents | as in plain Bun | not AOT specific |

Not shared: every JS cell, `Structure`, atom table, property cache (`vm.aotState().propertyCaches`), `Instance`, module variable, event loop, timer heap, uws loop.

### 2.5 Tests that cover it (top level of `test/aot`, the only part in the sparse checkout)

- `workers-named-by-relative-texts.test.ts`: eight spellings of a name, own file and another entry point, run in the source folder and elsewhere, against plain Bun.
- `worker-entry-throw-origin.test.ts`: `node:worker_threads`, CommonJS and ESM entries that throw; as an executable and `--compile-in-place`.
- `the-atoms-a-worker-makes-of-the-programs-texts-end-with-the-worker.test.ts`: 300 Workers, 4,000 texts, bound on anonymous memory. Uses `bun run --debug-aot`.
- `no-thread-can-be-started.test.ts`: under `ulimit -u`: collector helpers, thread pool, `new Worker(new URL(import.meta.url))`, `fetch`. *"A compiled program starts its threads late"*.
- `thread-sanitizer.test.ts` + `thread-sanitizer-known.json`: ThreadSanitizer over the **compiler's** threads (`testaot` built with `-fsanitize=thread`), not over compiled programs.
- `same-program-at-any-thread-count.test.ts`, `type-errors-on-every-thread-count.test.ts`: build determinism.
- `test/aot/README.md` mentions neither Workers nor threads as a group (grep: lines 33 and 91 only, unrelated). Programs under `test/aot/programs/` are outside the sparse checkout.

---

## 3. How a native class or function gets into the table

### 3.1 Sources of declarations

`src/codegen/builtins/index.ts`. Most files of `src/codegen/builtins/*.ts` are 9-line shims that re-export the engine's generator (`aot/builtins/generator/*.ts`) found by
`src/codegen/engine-directory.ts`. `generate()` (:239-277):

1. `loadEngine` (:143-150): the engine's declarations, from `aot/builtins/*.builtins.ts` of a checkout or from the serialized list in a prebuilt tarball.
2. `collectHost` (:115-141), concatenating:
   - `written`: the 31 hand-written files `src/codegen/builtins/host/*.ts`;
   - `collectStaticTables`: C++ `HashTableValue` / `.lut.h` tables named by those files;
   - `collectModules`: `src/codegen/builtins/data/module-exports.txt` (76 modules) and `-absent-on-windows.txt`;
   - `collectOwnProperties`: `data/own-properties.txt` (215 lines);
   - `collectNativeBindings`: every `$cpp(..)`, `$rust(..)`, `$newCppFunction`, `$newRustFunction`, `$bindgenFn` call site in `src/js` (`nativeCalls`);
   - `collectBufferAccessors`, `collectNativeLoads`;
   - `collectBindgen(...)`: **the `.bind.ts` schemas** (`--sources=<crate>=<a.bind.ts>,..`).
3. `collectRuntimeFunctions`, `collectHostFunctions`: functions of `src/js/builtins` and of the front end's prelude (`src/aot/prelude/*.ts`).
4. `buildModel({declarations, schema, features, engineFiles, syntaxUses})`, then emitters. Output into `build/<target>/codegen`: `AOTBuiltinTable*.cpp`, `AOTBuiltinTableArrays.h`, `BunAOTHostBindings.cpp`,
   `aot_builtin_ids.rs`, `aot_builtin_syntax.rs`, `aot_builtin_table.rs`, `builtin-uses.json` (for trim), `aot_builtin_table.json` (the dump), `aot_builtin_undescribed_results.txt`.

A change to an entry "that the engine declares or that `aot/builtins/host-roles.builtins.ts` lists needs a build of WebKit" (`index.ts:299-302`). Everything else is Bun-only. `host-roles.builtins.ts` is the list of
host entries that engine C++ names (`%CompilerHelpers%.*`, `%ModuleObject%`, `URL`, `Buffer.byteLength`, ...).

### 3.2 From a `.bind.ts` member to a row (`src/codegen/builtins/collect-bindgen.ts`)

What an author writes (`src/codegen/bindgen-lib-internal.ts`):

- per function or member (`SignatureOptions`, :2293-2300; `FunctionField`, :2524-2530): `args`, `returns`, `capability: "none" | "heap" | "js"` (default `"none"`, :2388), `throws`, `mutates`, `passThis`, `invalidThis`, plus every
  `SignatureColumns` and `EntryColumns` of the table ("What it extends is for the table of built-ins").
- per class (`ClassOptions`, :2584-2621): `rustPath`, `refCounted`, `finalize`, `final`, `forBind`, `memoryCost`, `estimatedSize`, `construct`, `call`, `internalFields`, `proto`, `static`, plus `ClassTableColumns` (`types`, `lockedObjects`, ...).
- `capability` is a Rust-level fact too: the body is handed nothing, `&JsHeap` or `&mut JsScope` (`src/jsc/bindgen.rs:26-34`), so "cannot run JavaScript" is proven by construction. Both types carry `NotSend = PhantomData<*mut ()>` (:20).

What the collector derives (`createField`, :376-416):

| Column | Derived value when the author says nothing |
| --- | --- |
| `args`, `returns` | from the schema's types (exact: the generated wrapper converts and refuses) |
| `capability` | the schema's, raised from `none` to `heap` if any operand or the result is a cell that is not read in place (`capabilityOf`, :369-373) |
| `throws` | `signature.throws` or any argument type that can refuse |
| `uses[arg]` | `computeBodyUses` (:294-301): an argument that holds a JS value: `handedOver` (= `[retained]`) if the member says what it returns, else `[everything]` for `capability: "js"`, else `usesWithoutJavaScript` (retained, returned, reads and writes contents, reachable values stored in native memory and in the result, :95-102). A borrowed buffer: `readsContents`. Plus `computeConversionUses` (:277-289) |
| `uses.this` | `computeReceiverUses` (:320-331): `handedOver` if the result is said and the receiver is `&self`; `[everything]` for `js`; else reads and writes contents |
| `reads`, `writes` | `["ExternalState"]`, or nothing said (= everything) for a class member with `capability: "js"` (:412-413) |
| `implementation` | `b.hostCall` |
| `directEntry` | the C thunk that compiled code calls **without the function object**: `(JSGlobalObject*, JSCell* receiver, operands...)`, at most 6 parameters (`createFields`, :488-530; `realmParameter`, `receiverParameter`) |
| proof | `"ByConstruction"` for a member with signatures, else `"Structure"` (:114-124) |

A member "of which the schema says nothing but that it is there" (`isUnread`, :549-552) also gets `b.otherwiseBy(b.hostCall)`: an **undeclared overload**.

### 3.3 The defaults when nothing is declared

`undeclaredSignature` (`aot/builtins/generator/lib.ts:891-902`):

```ts
{ mode, receiver: null, parameters: [{ name: "operands", type: any.rest.parameter }], result: anythingSpec,
  capability: "js", throws: true, isUndeclared: true, implementation: field.implementation }
```

and `collectSignatures` (`aot/builtins/generator/build-overloads.ts:569-581`) **adds one such overload for every call mode that an entry has and nobody declared**. So the default is the most conservative row:
takes anything, answers anything, runs any JavaScript, throws.

How flow treats it (`aot/valueflow/AOTValueFlowBuiltins.cpp:741-777`):

- no declared receiver + undeclared: `handToUndescribedNativeCode(BuiltinOperand::receiver())`;
- every operand goes through `applyParameter` on an `any.rest` parameter with no uses said, i.e. it escapes;
- result = `valueOfType(anything)`: cause `UndescribedNativeResult(UnknownCode)` in `--explain` (`compile-call.def:10`, docs cause `builtin-result`);
- `capability == JS`: `throwValue(unknownValue(ThrownByRunTime))`;
- `overload.isUndeclared() && origin == Host && !IsCompilerHelper`: `letUndeclaredNativeCodeRun(context)` (`aot/valueflow/AOTValueFlowRulesHost.cpp:229-237`), which records a use of the pseudo-entry
  `Builtins::UndeclaredNativeCode` and raises `HostChoosesOperands`: native code may call the library functions that only native code leads to, with operands of its choice.

What an undeclared native does **not** do by default: it does not make the program open. That is a separate, mandatory classification (3.4).

### 3.4 The existing model for "every native entry must be classified": `runsUnseenCode`

`classifyUnseenCode` (`aot/builtins/generator/build-attachments.ts:1080-1098`):

```ts
const [written] = writtenColumn(builder, builtin, "runsUnseenCode");
if (written !== undefined) applyUnseenCodeColumn(builder, builtin, written);
else if (!runsOnlySeenCode(builder, seen, builtin))
  builder.problems.add(builder.sourceOf(builtin),
    `${builtin.text} runs native code nobody has classified: declare it, or list its holder or file in host/unseen-code.builtins.ts`);
```

`runsOnlySeenCode` (:1043-1054) answers yes without a word from anybody if the entry runs no native code (`Namespace`, `Prototype`, `Constant`, a `DataProperty` that is not lazily created), if all its overloads are declared, or if its
origin is `Engine`. Otherwise some holder up the chain, or the native file of the binding, has to be in a `b.seenCodeOnly({ holders, nativeFiles })` list. `src/codegen/builtins/host/unseen-code.builtins.ts` holds those lists:
5 host objects, 31 classes with a constructor, 54 prototypes without one, 66 module namespaces, 84 native files, plus explicit `runsUnseenCode: true` (17 entries) and `false` (29 entries).
**A new native function that nobody classified fails Bun's build.** That is exactly the ratchet a `threads` attribute wants.

### 3.5 What a per-entry attribute `threads: "any" | "owner" | "main"` would need

Engine side (`aot/builtins/generator/`, so a WebKit change):

1. `vocabulary.ts`: a type and a field in `EntryColumns` (:548-597), beside `runsUnseenCode` (:574). For a class-wide default, a field in `ClassColumns` (:612). `lib.ts:794-809` lists the accepted column names.
2. `model.ts`: storage. **`builtinFlags` is full**: 16 flags (:44-61) in `enum class BuiltinFlag : uint16_t` (`aot/builtins/AOTBuiltinTable.h:153-170`). Three states need two bits, so either widen the word or use the other mechanism, **marks**:
   `builtinMarks` (`vocabulary.ts:531-546`, "What code of the engine asks about an entry, and few entries have. The table lists the entries of each mark, so the engine names none of them"). A mark is a list, good for the rare value
   (`any`), bad for a per-call test in emitted code.
3. `build-attachments.ts`: a `classifyThreads` pass after the model of `classifyUnseenCode`, with the conservative default and a build error or a default for unclassified host natives. Bulk lists by holder and native file as in `SeenCodeOnly`.
4. `emit-cpp-*.ts`, `emit-rust-table.ts`, `emit-dump.ts`, `AOTBuiltinTableValidation.cpp`, `AOTBuiltinTableDescription.cpp` (the JS-visible description that `JSTests/stress/bun-aot-builtins-table.js` checks).
5. Consumers: the call emitters (`aot/builtins/AOTBuiltinCallPlan.cpp`, `AOTBuiltinCallEmitter.cpp`), the adapter stubs for host calls, and flow if a diagnostic is wanted.

Bun side: values in `.bind.ts` (they pass through `columns`, `createField` spreads `...columns`), a file `host/threads.builtins.ts` with `b.extend(path, { threads: .. })` lists, nothing else.

Where the check can be enforced, and where it cannot:

- A call that the compiler resolved (`directEntry`, `hostCall` with a known `BuiltinID`) can get a compile-time decision or an inline test.
- A call by value (`const f = fs.readSync; f()`, `Reflect.apply`, a callback handed to native code, interpreted code from `eval`) reaches the native through JSC's ordinary host-call path, where there is no `BuiltinID`. The earlier
  threads work put the bit on the function identity for this reason: *"The bit lives on `NativeExecutable` ... NOT on a property descriptor ... the same callable is reachable via `.call`/`.apply`, bind products, cross-thread-published
  `JSFunction`s"* (threads docs `SPEC-nativeaffinity.md` §1.1). It also records the holes: `InternalFunction` has no `NativeExecutable` (§2.5), custom getters and setters, `GlobalObjectMethodTable` hooks get no coverage (§0.3, NA-I24), std::function-backed
  natives share one pointer (NA-I25).
- Getters: `customGetter` "takes its receiver for an instance without a test: the engine tests that before it enters a getter of the table" (`collect-bindgen.ts:672`). So a table-driven guard in front of getters already exists in shape.

The conservative default that the existing code suggests: **host origin + runs native code + not classified = `owner`** (or a build error), **engine origin = whatever the engine's own audit says**. `capability: "none"` members of a non-`mutates`, non-`refCounted`
class that touch only immediates are the cheap first candidates for `any`, because bindgen proves that they allocate nothing and run nothing. It does not prove that the Rust body reads no thread-local: `VirtualMachine::get()` needs no capability.

---

## 4. How types and values from `bun-types` are recognised

"The compiler goes by what an identifier is bound to, never by how it's spelled" is implemented by four separate mechanisms.

### 4.1 The compiler's own declaration file

`src/aot/prelude/bun-intrinsics.d.ts` (57 lines) is compiled into the binary (`src/aot/load/compiler_declarations.rs:3`) and served at the virtual path `/$bun/types/compiler.d.ts`
(`COMPILER_DECLARATIONS_PATH`, `src/sema/check/aot/program_symbols.rs:8`; `src/aot/load/host.rs:203-204,510-511`). **Every build has it as a root file of role `TypesOnly`**, whether or not `@types/bun` is installed
(test `every_build_has_them_as_types_only`). It declares, in `declare module "bun"`: `type int32 = number & { readonly "bun.int32"?: true }`, `namespace Schema { ... }`, `const JSON: globalThis.JSON`, `global { export import Bun = BunModule }`; in
`declare module "bun:bundle"`: `feature()`, `validate<T>(value: unknown): T`; and globally `interface JSON { parse<T = any>(...) : T }`. `packages/bun-types/bun.d.ts:16-33` repeats the same text for editors and `tsc`, and a unit test compares the aliases
(`TAGGED_ALIAS_COUNT = 17`, `compiler_declarations.rs:15`).

### 4.2 Intrinsic type aliases: by symbol, in that file

`ProgramSymbols::resolve` (`program_symbols.rs:76-103`): look up the ambient module `bun`, then `find_type_alias(files, bun_module, b"int32", declared_in)`. `find_export` (:127-142):

```rust
// `declared_in` is the compiler's own declaration file: a name means what that file declares, whatever another file declares under it.
let part = parts.iter().find(|part| part.file == file)?;
```

The result is a `Sym`. Every later question is `Some(alias) == self.int32` (`is_compiler_known_alias`, :109-111). So `import type { int32 as i } from "bun"` works, `type int32 = number` of the user's is nothing, and a hostile `declare module "bun" { type int32 = string }` in another
file does not take the name over (test `names_stay_theirs_where_other_files_declare_them`). The tag literal `{ readonly "bun.int32"?: true }` is also recognised by identity of the type node (`int32_tag: (FileId, TypeNodeId)`, :116-118), never by the property's name.
The 16 `Schema.*` aliases are an array of `Option<Sym>` indexed by an enum (`SchemaAlias`, :10-67).

**To add a marker type** such as `shared` or `Shared<T>`: a line in `bun-intrinsics.d.ts` and in `bun.d.ts`; a field in `ProgramSymbols` and a lookup in `resolve`; then the place where types become shapes. For `int32`: `src/sema/check/aot/inferred_from_use.rs:1018,1129,1732`,
`schema_constraints.rs:78`, then `src/aot/lower/convert/*` (`int32_flows.rs`, `narrowed.rs:338`). For `Schema.*`: `ShapeKind::Constrained { base, constraints }` (`src/sema/check/aot/type_shape.rs:95-98`). The IR's declared types are in `aot/ir/schema/types.def`. A new kind of declared type is a schema change, so both repos.
An alias from another module (`bun:threads`) only changes the module atom that `resolve` looks up.

### 4.3 Built-in classes as types: paths in the table, resolved to symbols once

Each class row has `types: TypeNameSpec[] = { path, isView, typeArguments }` (`vocabulary.ts`), written as `b.exactType("$Node.Timeout")`, `b.viewType(name)` (`src/runtime/timer/Timer.bind.ts:21`, `handwritten-classes.builtins.ts:9`).
`builtin_type_paths()` (`src/aot/builtins/type_paths.rs:29-46`) lists `(class, type_name, module?, names, denotation)`. The first component is a module if `table::module_by_specifier` knows it (`split_module`, :13-20).
`BuiltinTypeSymbols::compute(files, &types, &values)` runs once per project in `bind_modules` (`bind_stage.rs:63-66`): `resolve_type_classes` (`src/sema/check/aot/builtin_types/resolve.rs:17-43`) walks global or ambient-module scope by name **once**, through aliases (`resolve_alias_as`, `canonical`), to a `Sym`, and skips a symbol that the program itself
declares with a body (`has_runtime_declaration`, :105-128). Afterwards `class_of(symbol)` (`builtin_types.rs:80`) is a hash lookup. In the type-to-shape step: `ShapeKind::Builtin { class, arguments }` (`src/sema/check/aot/type_shape/objects.rs:32-38`).

`Denotation::Exact` (the type's values are the instances of the class and nothing else) needs `has_instance_test && !is_view` (`type_paths.rs:22-27`), and is **degraded to `View`** if any declaration file outside TypeScript's own lib declares a subtype the table does not know
(`degrade_types_with_unknown_subtypes`, `resolve.rs:154-181`). The comment in `bun-intrinsics.d.ts:14` shows the consequence for authors of declarations: *"Not an interface that extends `TypeError`: a subtype in a declaration file makes a built-in class's type expected, not proven."*

### 4.4 Built-in values: by binding, per file

`src/aot/builtins/resolve/` (`resolver.rs`, `imports.rs`, `aliases.rs`, `paths.rs`, `mentions.rs`). `collect_import_aliases` (`imports.rs:67-94`): for each import site whose resolution is a host module **without source** (`has_source` false), the imported bindings become `ResolvedValue`s that name table entries; `const { x } = require("node:y")`, `import * as`,
`import x = require()` likewise. A global name resolves only if no binding of the file shadows it. The answer for an expression is `Resolved::Builtin(BuiltinPath { id, mutability })`, `DynamicGlobal`, or `Intrinsic` (`resolve.rs:52-56`). `locked_builtin()` (:44-48) says whether the reference may be folded to the entry (`Mutability::AlwaysLocked`).
`find_module_export` (`imports.rs:37-62`) distinguishes `Found`, `HeldByNamespace`, `PropertyOfNamespace` (open namespaces: `"bun"`), `Absent` (module lists all exports: a build error `ExportNotFound`), `Unknown`.

`bun:bundle` is the model of a **module that exists only for the compiler**: `collect_bundle_import_aliases` (`imports.rs:96-107`) matches the import's specifier atom `known::bun_bundle`, binds to `BuiltinId::BunBundle`; unknown names give `UnknownBundleExport = 100105`. `validate` is then found as `BuiltinId::BunBundleValidate`
(`src/aot/lower/validate/expected_result.rs:30`), and every mention that is not a direct call is an error (`misuse.rs:16`). Under plain `bun run`, *"The import is an error"* (`docs/bundler/validate.mdx:247`).

### 4.5 Built-in functions by their call signature

`JSON.parse<T>` is recognised even through a value the resolver cannot follow: `collect_call_signatures` (`resolve.rs:44-62`) maps every call signature declared for a published value path to its `BuiltinValueIndex`. At a call, `find_callee_by_signature` (`src/aot/lower/call.rs:409-423`) asks the checker which declaration the call resolved to.
If only the type says so, a warning: `TypedParseCalleeIsNotResolved = 100514`. `remove_compiler_signatures` (`program_symbols.rs:176-192`) keeps the compiler's extra overloads from changing what the program is checked against.

### 4.6 Checklist for `Thread`, `Lock`, ... as new built-ins

From a module `bun:threads` (native, no JS source):

1. Runtime: `src/resolve_builtins/HardcodedModule.rs` (enum, :12-24; parse, :210-217; alias table, :712-), the native module (`src/jsc/modules/_NativeModule.h`) or a `src/js/bun/threads.ts`.
2. Table: a line in `data/module-exports.txt`; class rows (`.bind.ts` or `host/*.builtins.ts`, or the engine's `*.builtins.ts` if engine code must name them: then `b.hostRoles` is not needed, they are engine entries); `types: [b.exactType("bun:threads", "Thread")]`-style paths; `usesFeature`; `runsUnseenCode: false`; `unseen-code` lists.
3. Types: `packages/bun-types/*.d.ts` + (if the build must not depend on `@types/bun`) `bun-intrinsics.d.ts`.
4. Whether text from `eval` can reach it: a module is reached by `require`/`import()`/`process.getBuiltinModule`, which flow models as `LoadsModuleByName`. A **global** `Thread` is reachable from any text through `globalThis`.
5. Callback semantics for flow: `UseSpec { use: "Calls", times: "Later" | "Once" | ..., thisValue, arguments, result }` (`CallbackSpec`, `vocabulary.ts:305-314`). `callbackTimes = ["Later","Repeatedly","PerElement","Once","PerEvent"]` (:170): **there is no word for "at the same time as the caller"**. Every effect summary, `Proof` that "holds for a stretch of code in which nothing can undo it" (engine PR text) and `reads`/`writes` heap list assumes that a callback runs either inside the call or after the caller has returned to the event loop.

---

## 5. Bun's runtime per VM

### 5.1 The rule, as the code states it

- `src/jsc/VirtualMachine.rs:943-960`: *"`VirtualMachine` is deliberately `!Send + !Sync` ... a `&VirtualMachine`, a pointer to it or to its loops cannot be carried to another thread. Other threads hold a `bun_jsc::Ticket` / `VmHandle` and reach the VM only by posting to it."* A `const _` block fails to compile if it ever becomes `Send` or `Sync`.
- `src/ptr/js_cell.rs:1-36`: *"Bun runs **one** `VirtualMachine` per JS thread. JavaScript is single-threaded and reentrant ... There is no true concurrent aliasing — only stacked, same-thread reentrancy. `JsCell` is the Rust spelling of that contract."* and *"the `unsafe impl Sync` below is a lie to the type system that we discharge by the thread-affinity invariant"*.
- `src/jsc/VmHandle.rs:1-33`: the "door": *"a VM is destroyed only after everything that left its thread has come back."* `test/internal/source-lints/vm-thread-door.test.ts` "freezes the set of call sites and of `unsafe impl Send/Sync` in the VM crates" (file outside the sparse checkout). Counted here: 39 `unsafe impl .. Send`, 15 `.. Sync` in `src/runtime` + `src/jsc`.

### 5.2 Thread-local roots

| Slot | Where | Read by |
| --- | --- | --- |
| `VM: Cell<Option<*mut VirtualMachine>>` (`#[thread_local]`) | `VirtualMachine.rs:695-696` | `VirtualMachine::get()`, `get_mut()`, `get_mut_ptr()`, `as_mut()`, `get_or_null()` (:968-1040), C `Bun__getVM()`, `Bun__thisThreadHasVM` |
| `CACHED_GLOBAL_OBJECT` | :697-698 | C++ `defaultGlobalObject()` via `Bun__getDefaultGlobalObject` (:723-734) |
| `IS_MAIN_THREAD_VM` | :765-766 | `Bun.isMainThread` etc. |
| `RUNTIME_STATE: Cell<*mut RuntimeState>` | `src/runtime/jsc_hooks.rs:113-118` | `runtime_state()`, `timer_all()`, `timer_all_mut()`, `cron_jobs_mut()` ... |
| `SOURCE_CODE_PRINTER`, `SPECIFIER_CACHE_RESOLVER_BUF` | `VirtualMachine.rs:4203,4235`; `RuntimeTranspilerStore.rs:462` | module loader |
| `CHANNEL` (IPC instance) | `src/runtime/ipc_host.rs:404` | `process.send` |
| `CORKED_H2`, h2 shared buffer | `src/runtime/api/bun/h2_frame_parser.rs:844-864` | `node:http2` |
| `STDIN`/`STDOUT`/(stderr) blob stores | `src/runtime/api/BunObject.rs:2241-2242` | `Bun.stdin` ... |
| request-context `POOL`s | `src/runtime/server/mod.rs:3517,3535` | `Bun.serve` |
| `PENDING_TIME_LOGS`, `STD*_LOCK_COUNT` | `src/jsc/ConsoleObject.rs:277-278,5804-5805` | `console.time`, console writers |
| `SOURCE` (output writer), `PRNG`, `MOCKED_TIME_NS`, `SHARED_TEMP_BUFFER_PTR` | `src/bun_core/output.rs:220`, `util.rs:3544,5155`, `fmt.rs:527` | everything that prints, `Math.random`-like natives, fake timers |
| `AST_ALLOC` | `src/bun_alloc/ast_alloc.rs:160` | transpiler |
| the uws/usockets loop (`uws::Loop::get()` = `uws_get_loop()`) | `src/uws_sys/Loop.rs:213-215` | sockets, servers, polls. `ensure_waker`'s comment: "The VM's embedded loops run on **the thread's loop**" (`src/jsc/event_loop.rs:1107-1110`) |
| WTF: atom string table, `WTF::Thread::current()`, `RunLoop::current()` | engine | every `Identifier::fromString`, `AtomString` |

Totals outside `src/aot` and `src/sema`: 37 `#[thread_local]` in 13 files, 85 `thread_local!` in 60 files.

**The decisive detail**: `get_mut_ptr` is `unsafe { Self::get_or_null().unwrap_unchecked() }` (`VirtualMachine.rs:988-998`): *"`unwrap_unchecked` collapses to one TLS load. The "no VM on this thread" case is a programmer error"*. And `JSGlobalObject::bun_vm()` **does not read the global object**:

```rust
/// Reads the thread-local directly instead of calling `JSC__JSGlobalObject__bunVM` — cross-language LTO does not inline the
/// C++ shim, and the two are address-equal by construction (asserted in debug builds). JS thread only
pub fn bun_vm(&self) -> &'static VirtualMachine { ...; VirtualMachine::get() }      // src/jsc/JSGlobalObject.rs:1084-1099
```

Call sites outside the compiler: `VirtualMachine::get()` 253 in 89 files, `.bun_vm()` 392 in 114 files, `get_mut_ptr()` 31, `get_mut()` 11, `js_vm_ctx()` 73 in 22 files, C++ `defaultGlobalObject()` 15 in 10 files, C++ `bunVM(..)` 54 in 22 files (that one does go through `clientData(vm)->bunVM`).

On a second mutator whose slot is `None`, a release build treats `None` as a pointer: undefined behaviour, in practice a null dereference. If the slot were instead filled with the owner's pointer, every one of those sites would hand out `&mut VirtualMachine` (`as_mut`, `get_mut`) on two threads at once.

### 5.3 What `VirtualMachine` holds (`VirtualMachine.rs:159-430`, about 130 fields)

Grouped, with the fields that are written while JS runs:

- **Identity**: `global`, `jsc_vm`, `console`, `worker`, `is_main_thread`, `initial_script_execution_context_identifier`, `handle: ManuallyDrop<VmHandle>`.
- **Module loading**: `transpiler: Transpiler<'static>` (resolver with its caches, options, `log` pointer: `WebWorker::create` swaps `transpiler.log` for the duration of a resolve, `web_worker.rs:329-340`), `transpiler_store`, `module_loader`, `modules: async_module::Queue`, `source_mappings`, `ref_strings` + mutex, `resolved_path_dups`,
  `commonjs_custom_extensions`, `has_mutated_built_in_extensions`, `main*`, `entry_point`, `preload`, `macros`, `macro_entry_points`, `pending_internal_promise`, `standalone_module_graph`.
- **Event loops**: `regular_event_loop`, `macro_event_loop`, `event_loop: *mut EventLoop` (points at one of the two), `event_loop_handle`, `active_tasks`, `pending_unref_counter: AtomicI32`, `after_event_loop_callback*`, `is_inside_deferred_task_queue: Cell<bool>`, `suppress_microtask_drain: Cell<bool>`, `jobs: JsCell<JobList>`.
- **Errors**: `on_unhandled_rejection` (fn pointer, replaced per Worker, `web_worker.rs:744`), `on_unhandled_rejection_ctx`, `unhandled_error_counter`, `unhandled_pending_rejection_to_capture`, `is_handling_uncaught_exception`, `exit_on_uncaught_exception`, `had_errors`, `exit_handler`.
- **GC**: `gc_controller`, `aggressive_garbage_collection`, `arena: Option<NonNull<Arena>>` (a mimalloc heap; `MimallocArena` allocation is same-thread only, `mi_free` any thread, `src/bun_alloc/MimallocArena.rs:28-34,75-77`).
- **Rare**: `rare_data: Option<Box<RareData>>`, `proxy_env_storage` (locked), `node_fs`, `debugger`, `auto_killer`, `child_workers`, `pending_ipc`, `channel_ref`, `counters`, `html_rewriter_active_sink`.
- **Contexts**: `root_context`, `dead_context`, `graph_contexts`, `entered_context: Cell<Option<ContextId>>`.
- `runtime_state: *mut c_void` -> `RuntimeState` (`src/runtime/jsc_hooks.rs:64-111`): `timer: timer::All` (the heap of `setTimeout`/`setInterval`/`setImmediate` and of JSC's own `WTFTimer`s), `sql_rare`, `ssl_ctx_cache`, `global_dns_data` (c-ares channel), `entry_point`, `transpiler_arena`, `body_value_pool` (hive allocator for `Request`/`Response` bodies), `cron_jobs`, `uv_loop` (the `uv_loop_t` that addons get).

Only four things in the struct are built for other threads: `pending_unref_counter` (atomic), `remap_stack_frames_mutex`, `ref_strings_mutex`, `proxy_env_storage`, plus the `VmHandle`.

### 5.4 The event loop (`src/jsc/event_loop.rs:48-119`)

| Field | Touched by |
| --- | --- |
| `tasks: Queue`, `immediate_tasks`, `next_immediate_tasks`, `yield_tasks`, `plugin_requests`, `deferred_tasks` | owner only, plain `Vec`/FIFO. `enqueue_task(&mut self)` (:893) |
| `concurrent_tasks: ConcurrentQueue`, `concurrent_plugin_requests` | any thread pushes (`UnboundedQueue<ConcurrentTask>`, `src/event_loop/ConcurrentTask.rs:286,336`), owner pops in `tick_concurrent_with_count` (:620) |
| `waker`, `concurrent_ref: AtomicI32`, `imminent_gc_timer: AtomicPtr`, `task_drains: AtomicU64` | cross-thread by design |
| `entered_event_loop_count: isize` | owner only. `enter()` increments (:313-316). `exit()` (:324-333): `if count == 1 && !is_inside_deferred_task_queue { drain_microtasks() }` then decrements. **This plain counter is what decides when microtasks run after a native-to-JS call** |
| `uws_loop`, `virtual_machine`, `global` | set once |

Posting from another thread: `Ticket::post` (`VmHandle.rs:236`, cannot fail) and `VmHandle::post(kind, task) -> Posted` (:337, deliver or refuse). C++: `ScriptExecutionContext::postTaskTo(contextId, BunLoopKind, lambda)`. The state machine `Open -> Stopping -> Draining -> Closed` is atomic (:48-66).

### 5.5 Microtasks, `process.nextTick`, rejections, AsyncLocalStorage

Order in `GlobalObject::drainMicrotasks()` (`ZigGlobalObject.cpp:3478-3551`):

1. stopped VM: return.
2. `if (!vm.entryScope) m_asyncContextData.get()->putInternalField(vm, 0, ...undefined)` (:3514-3515): with no script on **the** stack the ALS cursor is reset. `vm.entryScope` is one field per JSC VM.
3. `m_nextTickQueue` not empty: `nextTickQueue->drain(vm, this)`.
4. `vm.drainMicrotasks()`.
5. the tick queue again.

Then, in Rust (`event_loop.rs:412-457`): `release_weak_refs`, `deferred_tasks.run()` between `is_inside_deferred_task_queue.set(true/false)`, `uws_loop.drain_quic_if_necessary()`.

- **nextTick is JavaScript in this branch** (PR: "It's a JavaScript function"): `src/js/internal/process/next_tick_queue.ts`. State is module-level: `const ring = { read, write, size, pushWithInitHooks }`, `let isSetUp`, `let nextTickQueue`. A push is `tick = ring.write; next = tick.next; if (next === ring.read) next = growRing(tick); tick.callback = ..; tick.frame = ..; ring.write = next; ...; $putInternalField(nextTickQueue, 0, 1)` (:31-74). Under AOT this is compiled code on typed layouts, in the one `Instance`.
- `vm.setOnEachMicrotaskTick(&checkIfNextTickWasCalledDuringMicrotask)` (:561-564), which starts with `auto* globalObject = defaultGlobalObject();` (thread-local, :512-519) and dereferences it unchecked.
- **AsyncLocalStorage**: one cursor per realm: `$asyncContext` = `m_asyncContextData`, an `InternalFieldTuple` whose field 0 is the current frame. `get()`/`set()` are `$getInternalField($asyncContext, 0)` / `$putInternalField` (`src/js/node/async_hooks.ts:97-116`). `next_tick_queue.ts:19-20`: *"The realm has one for good. A constant of the module is read without a look at the global object."* The threads docs ruled it per-thread: "The CURSOR m_asyncContextData = GIL-off per-lite" (`SPEC-ungil.md` §E.1 item 5).
  Under AOT, tracking is switched on lazily by the first `set(frame)` (`async_hooks.ts:103-113`, `isTrackingEnabled`, a module-level `let`), which changes how the engine queues microtasks (`RuntimeHooks::didChangeMicrotaskQueueing`).
- **Unhandled rejections**: `GlobalObject::promiseRejectionTracker` (:1339) appends to `Bun::RejectedPromiseQueue m_aboutToBeNotifiedRejectedPromises` (`ZigGlobalObject.h`, last member), drained by `handle_rejected_promises` from the loop (`JSGlobalObject.rs:1103`). No lock.

### 5.6 `Zig::GlobalObject` members (`src/jsc/bindings/ZigGlobalObject.h`)

- 153 GC members in the `FOR_EACH_GLOBALOBJECT_GC_MEMBER` list, most of them `LazyPropertyOfGlobalObject<T>` or `LazyClassStructure`, read with `.getInitializedOnMainThread(this)` or `.get(this)`: **initialised on first use** (check, run an initializer that allocates, store).
- `m_domStructures[DOMStructureSlot::Count]` write barriers; `m_gcLock` guards only `m_guardedObjects`.
- Plain C++ state: `m_pendingNapiModules`, `m_pendingV8Modules`, `napiModuleRegisterCallCount`, `m_napiEnvs`, `onLoadPlugins`, `onResolvePlugins`, `sourceProviderMap` (hash map), `reloadCount`, five `hasOverridden*`/`hasWarned*` bools, `m_secureContextCache`, `m_moduleGraphs`, `m_gcProfilerObserver`, `m_subtleCrypto`, `m_performance`, `m_moduleWrapperStart/End`, `m_aboutToBeNotifiedRejectedPromises`.
- `JSVMClientData` per JSC VM: `bunVM`, `defaultGlobalObject`, `vmHandle`, the iso subspaces created on first use (`subspaceForImplSlow`), the `StrongRootBlock` list with a cursor.
  `setDefaultGlobalObject` (`ZigGlobalObject.cpp:499-505`): *"The thread-local default (what defaultGlobalObject() returns on this thread) and the VM's (what defaultGlobalObject(VM&) returns on any thread) change together."* So `defaultGlobalObject(JSC::VM&)` (:574-579) already works from any thread. The argument-less one does not.

### 5.7 What assumes "the thread that runs JS for this VM is this one OS thread": summary list

1. Every thread-local of 5.2, above all `VM`, `RUNTIME_STATE`, the uws loop, the atom table.
2. `&mut VirtualMachine` from `&self` (`as_mut`) and `JsCell::with_mut` (435 uses of `JsCell<` in 77 files).
3. Non-atomic counts and flags: `entered_event_loop_count`, `active_tasks`, `is_inside_deferred_task_queue`, `macro_mode`, `is_handling_uncaught_exception`, `RefCount` (6.2).
4. The API lock: the worker thread takes it once and keeps it (`web_worker.rs:681`); the main thread likewise holds it while it runs. `JSLockHolder` in natives is recursive on the owner.
5. `vm.entryScope`, `vm.topCallFrame` (used by `recordRefusedChange`, `BunAOTRuntime.cpp:161-175`), `vm.exceptionForInspection()`: one per JSC VM.
6. `StackCheck::configure_thread()` (`VirtualMachine.rs:3473`): the stack bound is per thread and set at VM creation.
7. `WTFTimer__create` (`src/runtime/timer/WTFTimer.rs:247-250`): a `RunLoop` timer made on a thread without a Bun VM returns null, "the timer then never fires". JSC's GC timers are such timers.
8. mimalloc arenas: allocation from the owning thread only.
9. Teardown: `close_and_wait`, `join_child_workers`, `destroyVM` assume that script has stopped once the owner thread stops.

---

## 6. Native objects of Bun

### 6.1 Shape of a generated class (`src/codegen/bindgen-emit.ts`)

*"The cell: JSDestructibleObject | the internal fields | the pointer to the struct of a refCounted class, **or the struct itself**"* (:831). Two kinds:

- **`refCounted: true`** (37 of 90 `b.class(` sites): the cell holds `void* m_wrapped` and owns one ref. "Every member takes `&self`" (`bindgen-lib-internal.ts:2587-2588`). `destroy` -> `host_fn_finalize_ref_counted(wrapped, T::finalize)` (:1913-1917).
- **payload in the cell** (the other 53): the Rust struct lives **inside the JS cell** at `offsetOfPayload`, at most `maxCellSize` bytes (:1890-1893). `destroy` -> `core::ptr::drop_in_place(payload)` (:1894-1898). A member with `mutates: true` takes `&mut self` on that memory (4 sites). The generator guards that against *reentrancy only*:
  `refuseReentrancy` (:2629-2633): *"JavaScript could call a mutates member of the same object while this one runs"*, so no `capability: "js"` in such a class; and `estimatedSize` is refused with *"a marking thread would read the struct while a mutates member writes it"* (:2857-2860).
- `internalFields`: `WriteBarrier<Unknown>` slots in the cell, set through `Bun__Bindgen__setInternalField`.
- Each class has an `IsoSubspace` made on first allocation: `Bun::Bindgen::subspaceForClass(vm, classIndex, allocationSize())` (:852-858).
- `destroy` runs wherever the sweeper runs. With a trim guard it is `guardedCall(.., "Abort", finalize, ..)` (:935).

Besides bindgen: 68 `JS*.cpp` in `src/jsc/bindings/webcore/` (WebCore-style wrappers holding `Ref<Impl>`), 209 `.cpp` directly in `src/jsc/bindings/`, the `JSSink` family (`generate-jssink.ts`), N-API and V8 shims.

### 6.2 `Send`/`Sync`, refcounts

- The capability tokens are `!Send` (`NotSend`, `src/jsc/bindgen.rs:20,28`), `JsWrapper<'a,T>` holds `PhantomData<*const T>` (:975-979). Nothing requires `T: Sync` of a wrapped struct. The structs use `Cell`, `JsCell`, `RefCell`, raw pointers freely.
- `bun_ptr::RefCount<T>` (`src/ptr/ref_count.rs:139-145`): `raw_count: Cell<u32>`, `thread: ThreadLock`. `ref_`: `assert_single_threaded(); raw_count.set(raw_count.get() + 1)` (:180-181). `deref`: the same with `- 1`, then `if raw_count.get() == 0 { T::destructor(self_) }` (:197-207).
  `ThreadLock` has **fields only under `debug_assertions`** (`src/bun_core/util.rs:2018-2023`): a release build checks nothing.
- `CellRefCounted`: a bare `Cell<u32>` ("Single-threaded only", :396-413).
- `ThreadSafeRefCount<T>`: `AtomicU32` (:269-285).
- Derive sites in `src/`: 16 `ThreadSafeRefCounted`, about 110 `RefCounted`/`CellRefCounted`.
- `WTF::StringImpl::ref/deref` **are atomic in this tree** (`WTF/text/StringImpl.h:1211,1224`: `fetch_add/fetch_sub(.., relaxed)`), so `bun_core::String` may be dropped anywhere. Atomization is still per-thread table.
- `Strong` (`src/jsc/Strong.rs`): a slot index in a `StrongRootBlock` cell. `destroy` (:184-226) asks `VirtualMachine::get_or_null()`: `None` = "Off the JS thread ... the slot (and its rooted value) leak until that VM's teardown. The block bitset/count are not thread-safe to touch here", with a `debug_assert!(false)`.

### 6.3 What would happen if a wrapper made on thread A had a method called on thread B (same JSC VM)

Step by step for a compiled call through `directEntry` (3.2): compiled code passes `(realm, receiver cell, operands)`. Nothing in the thunk looks at the thread.

1. The thunk casts the cell to `&T` (or `&mut T`). Two threads in a `mutates` member, or one in `mutates` and one in a reader: two live `&mut`/`&` to the same bytes. **Memory safety** (a `Vec` reallocated under a reader, an enum whose tag and payload disagree).
2. `&self` members mutate through `Cell`/`JsCell`/`RefCell`. `RefCell`'s borrow flag is not atomic, so even its panics are unreliable. **Memory safety.**
3. Almost every body then asks for the VM: `global.bun_vm()` = thread B's TLS. Empty slot: **UB / null dereference** (5.2).
4. It registers with "the" loop: `bun_io::js_vm_ctx()`, `KeepAlive::ref_`, `uws::Loop::get()`, `timer_all_mut()`: thread B's (absent) structures, or with a shared pointer the owner's unlocked heaps. A poll registered on the wrong epoll set never fires, or fires on A while B frees it. **Memory safety** for timers (an intrusive heap) and sockets; a **hang** at best.
5. It takes a ref for the async part: `RefCount::ref_` non-atomic: lost increments -> premature `destructor` -> **use after free**.
6. It creates `Strong`s: `Bun__StrongRef__new(global, value)` edits the block list, cursor, bitset and count of `JSVMClientData` without a lock. **Memory safety** (a GC root lost or a slot handed out twice).
7. It allocates a wrapper of another class: `subspaceForClass` lazily creates the `IsoSubspace` (check then create), and the `GCClient::IsoSubspace`'s `LocalAllocator` is single-threaded. r17's comment: *"These subspaces are per VM, so they belong to the VM's main client; only that thread may allocate Bun cell types through them."*
8. It reads a `LazyProperty` structure of the global object: double initialisation.
9. It enters JS through `event_loop.run_callback` -> `enter()`/`exit()`: the counter races, so microtasks drain at the wrong depth or on the wrong thread. **Wrong JavaScript order**, then (through 5.5) corrupted tick ring.

So: no generated class is safe to call from a second thread today, and none is protected by a check in release builds.

### 6.4 Counts

- PR text: 6,330 entries, 271 classes. **I could not split them by origin**: the split is `origin: "Engine" | "Host"` on every entry of `aot_builtin_table.json`, a build product. No build directory with that file was at hand, and the generator cannot run in the sparse checkout (`scripts/glob-sources.ts` and `vendor/` are absent).
- Proxies counted from sources: 79 `.bind.ts` files, 4,648 lines, 90 `b.class(`, 177 `b.fn(`, `capability: "js"` 219 times, `"heap"` 88 times. In `unseen-code.builtins.ts`: 31 + 54 hidden host classes (`%Name%`). 76 modules in `module-exports.txt`. Engine side: 26 `*.builtins.ts` files.
- With a build at hand: `jq '[.entries[] | .origin] | group_by(.) | map({(.[0]): length})' build/<target>/codegen/aot_builtin_table.json` (field names by `aot/builtins/generator/dump.ts`, not checked).

---

## 7. `bun` and `bun-aot`; `ENABLE_AOT`; trim

### 7.1 One switch, `cfg.aot`

`scripts/build/config.ts:136-140`: *"The ahead-of-time compiler and what its programs need at run time. Off: JavaScriptCore and Bun's bindings are compiled with ENABLE_AOT=0, the crate `bun_aot` stays out of the link, and `--aot` answers that this build has no compiler."* Default `partial.aot ?? true` (:1397). CLI `--aot=off`. It fans out to:

| Layer | Effect | Where |
| --- | --- | --- |
| C/C++ flags, Bun's sources **and** vendored deps | `-DENABLE_AOT=0` when `!c.aot` | `scripts/build/flags.ts:461-465` ("wtf/PlatformEnable.h turns it on wherever the FTL is on": `WTF/PlatformEnable.h:759-763`) |
| Prebuilt WebKit | suffix `-aot`: *"What ships (release, lto, lto-hardened) is built twice: without the ahead-of-time compiler and the run time of its programs, and with both under "-aot". Every other variant has both"*. No twin + `--aot=off` is a `BuildError` | `scripts/build/deps/webkit.ts:73-82` (variants in oven-sh/WebKit's `.github/scripts/lanes.mjs`) |
| Rust | `cargo ... --no-default-features --features without_aot` | `scripts/build/rust.ts:322`; `src/executable/Cargo.toml:16`: `without_aot = ["dep:bun_aot_absent"]` |
| Rust stand-in | `src/aot/absent/lib.rs`: the ~20 functions that `bun_aot_interface` declares, each `absent()` = panic, behind `has_compiler() == false` | |
| C++ stand-in | `src/jsc/bindings/BunWithoutAOT.h` (included by `root.h`): inline no-ops for `JSC::AOT::*`, "so a call site reads the same in both builds". `#else` halves in `BunAOTRuntime.cpp:216-237`, `BunCompiledModules.cpp:532-564` | |
| mimalloc | two extra patches only with the compiler | `scripts/build/deps/mimalloc.ts:29-36` |
| Artifact name | `bun-aot-<os>-<arch>` vs `bun-...` | `scripts/build/ci.ts:294-303` |
| Feature list | `no-aot` | `config.ts:1799` |
| Generated code | the schema and table generators run in both (`emitAotCodegen`, `scripts/build/aot.ts:347-351`) | |

A further compile-time switch "only in the `-aot` flavour" therefore has a well-trodden path: a `Config` boolean (or simply `#if ENABLE(AOT)` implying it in `PlatformEnable.h`), one entry in `flags.ts`, a Cargo feature or `cfg` (compare `bun_hardened`, used as `#[cfg(bun_hardened)]` in `compiled_program.rs:231,259,285`, and `-DBUN_HARDENED=1 -DENABLE_AOT_POSITION_INDEPENDENT_IMAGE=1` in `flags.ts:122-124`), and, if it changes the engine's ABI or size, either riding on the existing `-aot` twin (no new lane) or a new suffix in `prebuiltSuffix` + `lanes.mjs` (a new lane per platform).
`--hardened` is the example of a third axis: it has its own suffix and only an LTO twin.

**Consequence worth stating**: `bun-aot` is at once (a) the compiler, (b) a complete JIT Bun (`bun run --aot` runs programs in it from source), and (c) the template that executables are cut from. A C++ `#if` in the `-aot` flavour is compiled into all three roles. What differs between roles at run time is `Options::useAOT()` (`ZigGlobalObject.cpp:377`: `= runsCompiledCode`) and which ranges are mapped.

### 7.2 What "the run-time half, 4.3 MB" is

From the PR text only: of `bun-aot`'s +29.1 MB, "the build-time half of the engine's `aot/` 16.0, the Rust front end and what only it reaches 4.9, the run-time half 4.3". By the engine PR's table the run-time folders are `aot/object/`, `aot/runtime/` (objects, closures, modules, coroutines, caches, slow paths), plus `aot/image` loading and the fault handler, `aot/stubs`, `llint/LowLevelInterpreterAOT.asm`, `yarr/YarrFlatProgram*`, `ffi/FFIStubs`, `heap/MarkedBlockCage`. On the Bun side: `BunAOTRuntime.cpp`, `BunCompiledModules.cpp`, `BunAOTHostBindings.cpp` + the table, `src/jsc/compiled_program.rs`, `src/standalone_graph/compiled_program.rs`.
The line between the halves is drawn by **trim features**, not by folders: `feature ImageCompiler "the ahead-of-time compiler's code generator" entered-from command-line` (`src/trim/features/engine.def:11-12`) and `feature AotCompiler "the ahead-of-time compiler" entered-from command-line minimum 9000000` (`tools.def:29-31`). I did not find a file that lists the 4.3 MB.

### 7.3 How an executable is cut from the template

At Bun's own link (`cfg.trimmable`, `config.ts:130-134`; `scripts/build/trim/link.ts`): link once; read the reference graph from relocations (`elf-graph.ts`, `macho-graph.ts`, `coff-graph.ts`); `src/trim/features/*.def` name the features and "the functions you have to pass through to reach each one"; whatever is reachable only through those is removable; link again with removable sections grouped in page-aligned ranges (`partition.ts`, `ranges.ts`) and write a table into `bun` (`table.ts`). At `bun compile`: `plan_trimming` (`src/runtime/cli/compile_command/trimming.rs:71-93`) reads the table from the template, `bun_trim::decide` chooses, and `write_executable` "just doesn't map those ranges" (PR).

- `.def` syntax (`engine.def`, `web.def`, ...): `feature Name "words for a message"`, optional `entered-from startup|command-line`, `minimum <bytes>` (a check that the feature really removes that much), `c-function a b c` (entry symbols). `trim-features.def`: `trim-feature public-name : Feature Feature ...`, `about`, `also`, `saves <MB> <MB>`.
- Engine features are **numbered by the engine**: `b.engineFeatures("JIT", "Interpreter", ..., "Atomics", ...)` in `aot/builtins/features.builtins.ts`, and `checkEngineFeatures` (`src/codegen/builtins/index.ts:216-235`) fails the build unless `engine.def` lists the same names in the same order.
- A table entry names its features: `usesFeature: "Atomics"` (`aot/builtins/buffer.builtins.ts:341`), `extendAll(["WorkerConstructor"], "node:worker_threads")` (`host/features-web.builtins.ts:30,75-82`: `workerFeatures = ["WorkerConstructor","Worker","MessageChannel","StructuredClone","Events","Buffer"]`). Flow reports `FeatureUseRecord`s for reached uses.
- Guards: the generated wrappers of a removable entry test `is_removed(feature)` and throw `ERR_FEATURE_REMOVED` (`src/codegen/builtins/guards.ts`, `RemovedFeature.cpp`, `RemovableFeatures.h`). "`--trim` removes code, not names."
- **Always** left out of a compiled executable, `--trim` or not: the tools and the JIT (`Trimming::NotAsked { without_tools }`, `trimming.rs:96-110`; `decide.rs:63-66`: `if request.removes_unused || input.is_program_compiled() { unused = all - retained }`).
- `removed_features` is also an **input to the engine** (`CompileOptions.removedFeatures: UInt64[8]`, `engine.rs:158-165`) for explicit `--trim-without` requests.
- Run time can ask: `bun_removable_features::is_removed(Feature::Ffi)` (`compiled_program.rs:286-301` uses it to tighten the seccomp filter).

### 7.4 Could "threads support" be a trim feature?

**Code that is only reached through `Thread`: yes**, and cheaply. A `feature Threads` (engine-numbered if the code is in JSC), `usesFeature: "Threads"` on the `Thread`/`Lock`/... entries, `trim-feature threads : Threads`. Existing near-equals: `Worker` + `WorkerConstructor` (saves "0.0 0.0" MB) and `Atomics`. The thread spawn path, park/unpark, the stop-the-world conductor, per-thread registries would go when nothing constructs a `Thread`. As with `eval`, an open program keeps it (`blocked by runtime-code`).

**Behaviour inside shared code: no.** Trim removes whole functions that become unreachable. It cannot specialise a function that both kinds of program run (allocation slow paths, property-cache fills, `RefCount::ref_`, the event loop's `enter/exit`). For those the choices visible in this tree are: (i) a run-time flag read from the image header at start (like `useWideReferences()`, `BunAOTRuntime.cpp:70`), (ii) two copies of a function, each behind its own feature so that one is always trimmed, or (iii) for **compiled** code and image stubs, a `CompileOptions` bit, since those are generated per program. (iii) is the one that costs a thread-free program nothing.

---

## 8. `bun run --aot`, `bun test --aot`, `bun check --aot`

### 8.1 Mechanism (two processes)

`apply_runtime_flags` (`src/runtime/cli/compile_options.rs:901-944`):

```rust
let compiles = args.flag(b"--debug-aot") || reports_type_mismatches;
ctx.bundler_options.aot = Some(Box::new(AotOptions { ..., runs_in_place: true, checks_only: !compiles, runs_tests: cmd == TestCommand, .. }));
```

`run_command.rs:1044-1047` -> `run_compiled::start(ctx, entry_points, 0)` (`src/runtime/cli/run_compiled.rs:82-160`):

1. **First process**: `product = if checks_only { GraphBytesIfSourceCannotRun } else { GraphBytes }` (:108-111); `compile_from_context`. The front end runs in full. The engine runs with `CompileAction::CheckOnly` **unless `validates_by_type`** (`compile.rs:320-322`).
2. `build.rs:73-78`: `GraphBytesIfSourceCannotRun if !build.validates_by_type => None` (no product), else graph bytes with the image in them.
3. **Handover** (:147-159): with graph bytes the flag is `--aot-graph-fd=<fd>`, without `--aot-checked=<fd or ->`. The bytes go into an unnamed file (`write_to_unnamed_file`, :364), the mode flag in argv is replaced (`second_process_arguments`, :379-399) and the process **`exec`s itself** (`bun_core::exec_self`, :431; on Windows a child process, :511).
4. **Second process**:
   - `--aot-checked`: `use_check_result` sets `IS_CHECKED`; `bun_jsc::initialize{ locks_builtins: is_checked(), runs_compiled_code: false }` (`run_command.rs:1056-1062`). `JSCInitialize`: `if (locksBuiltins) JSC::AOT::lockBuiltinsWithoutImage()` (`ZigGlobalObject.cpp:326-328`), and the method table gets a `moduleHost` so the realm's built-ins are locked (:585-593). **JIT on, source files, ordinary module loader.**
   - `--aot-graph-fd`: `use_graph_descriptor` -> `Graph::from_descriptor` (:565-589); `runs_compiled_code() == true` -> `Options::useAOT() = true`, cell cage, `graph: run_compiled::graph()` given to `VirtualMachine::init`, then `use_image`. **The same run time as an executable**, with `preserves_source_paths` (files keep their disk paths). `process.execArgv` shows `--debug-aot` in place of the fd flag (`src/runtime/node/node_process.rs:306-308`).

`--debug-aot` (hidden flag, `Arguments.rs:343,548`) and `--aot-log-type-mismatches` always compile. Many tests use `bun run --debug-aot file.ts`.

What is lost when the program "is compiled after all" (`docs/bundler/aot-debugging.mdx:60-65`, `check_runtime_conflicts`, `compile_options.rs:946-969`): `mock.module()` and `spyOn()` on module exports and read-only built-ins throw; `--inspect*` is an error; `--watch` and `--hot` are errors ("the program is compiled once, before it starts"); coverage is off (`compile_options.rs:910`). `bun test --aot` compiles or checks per test file (`refused_entry_points`, `list_refused_files`).

### 8.2 What a program that uses `Thread` would do there

Nothing exists today, so this is what the mechanisms allow:

- **`bun check --aot`**: works unchanged. All front-end and flow-stage diagnostics about threads are reported (`ReportsOnly`).
- **`bun run --aot`, `bun test --aot`**: the `validates_by_type` road fits exactly: *"Only compiled code has the validators, so Bun compiles such a program and runs the machine code, without writing an executable."* Add `uses_threads` beside it in `FrontEndOutput`/`ProgramSummary`/`AotBuild`, widen the two conditions (`compile.rs:320`, `build.rs:75`). Detection must be available **before** the engine call (D1 or D2 of 1.4), because it decides `CheckOnly` vs `Compile`. Cost to the developer: a full build at every start (no cache between builds: 8 s for `tsc`, 36 s for Claude Code), no breakpoints, no mocks of modules, no `--watch`/`--hot`.
- **plain `bun run`, `bun test`, `bun build`** (and `bun` without `bun-aot`): the two precedents are `validate` = *"The import is an error"* and `JSON.parse<T>` = silently unchecked (`docs/bundler/validate.mdx:244-249`). For `Thread` only the first is honest. With a `bun:threads` module the error falls out of module resolution (`BuiltinModuleNotFound = 100152, "No such built-in module: {0}"` at build; the resolver's error at run time).
- **Text from `eval` / `new Function` / `node:vm` inside an executable** runs in the interpreter (`LowLevelInterpreterAOT.asm`) "in the same process, on the same objects as the compiled code". If a `Thread`'s function, or anything it calls, can be interpreted code, the interpreter and everything it touches (bytecode generation, `CodeBlock` linking, LLInt caches "through the same tables as the stubs") is inside the threaded world. `--disallow-code-generation-from-strings` (`forbids_text_evaluation`) or a closed program removes that. That is a fact the build knows (`AdmitsUnknownJavaScript`).
- **Editor/tsc story**: types come from `packages/bun-types`. Nothing else is needed.

---

## 9. What the earlier threads work changed in Bun (commit 3fd4c405c1b2, 14 files, +175 -22)

| # | Hunk | Purpose | Under "threads only in AOT executables" |
| --- | --- | --- | --- |
| 1 | `src/codegen/generate-classes.ts` +5: in `~Class()`, `if (Bun::isOnSpawnedJSThread()) { Bun::postToVMThread(vm(), [ctx = m_ctx] { finalize(ctx); }); return; }` | A dead wrapper may be swept by whichever thread allocates. The native finalizer belongs to the owner thread | **Remains**, if any mutator can sweep. New home: `cppSourceForCell`'s `destroy` (`src/codegen/bindgen-emit.ts:932-936`). **Harder now**: for the 53 payload-in-cell classes the Rust struct lies in the dying cell, so "the posted work receives only the native pointer, never the dead cell" (threads docs `SPEC-heap.md` §10G) cannot hold: `drop_in_place(payload)` must run before the cell's memory is reused. Also the hand-written C++ classes, `JSSink`, WebCore wrappers (`Ref<Impl>` deref on a foreign thread), N-API finalizers |
| 2 | `src/jsc/Strong.rs` +19, `StrongRef.cpp` +29, `BunClientData.h` +lock and vector: `Bun__StrongRef__deleteFromOtherThread` queues the slot under `m_strongRefsReleasedElsewhereLock`, the first one posts a drain task | `Strong` slots released by a finalizer on a spawned thread | **Remains** with #1. `Strong.rs:209-221` in the AOT tree still has the old "slot leaks" arm |
| 3 | `BunClientData.cpp`: `isOnSpawnedJSThreadSlow()` = `ThreadManager::isJSThreadCurrent()`; `postToVMThread(vm, fn)` = `Bun__VmHandle__postAndRelease(retainRef(data->vmHandle), new EventLoopTask(fn), Regular)` | The two primitives | **Remain**. `VmHandle`/`EventLoopTask` exist in the AOT tree. The predicate needs an engine API |
| 4 | `BunClientData.cpp` ×2: `registerExternalAllocator(&...localAllocator())` for the DOM constructor/namespace spaces and in `subspaceForImplSlow` | With a shared heap every `LocalAllocator` must belong to a client's thread-local cache. "only that thread may allocate Bun cell types through them" | **Depends on the heap design.** The restriction it documents (Bun cell types are allocated on the owner thread only) is the same as `threads: "owner"` for every native that returns a wrapper. Also `Bun::Bindgen::subspaceForClass` now |
| 5 | `ZigGlobalObject.cpp` ×2: `if (!globalObject) return;` in `checkIfNextTickWasCalledDuringMicrotask` and the `setOnEachMicrotaskTick` lambda | A spawned thread drains its own microtask queue through a VM-wide hook and has no thread-local default global | **Remains** in some form: `ZigGlobalObject.cpp:512-519` is unchanged and unchecked. Deeper: the tick queue is now JS module state (5.5), so "a spawned thread never touches the tick queue" has to be a rule about `process.nextTick` itself |
| 6 | `ZigGlobalObject.cpp` `destroyVM`: `DropAllLocks` + `ThreadManager::singleton().waitForCompletedThreadsToReleaseVM(vm)` | A joined Thread may still hold its `Ref<VM>` | **Remains.** Fits the `Ticket` idea: a running Thread is "something that left the VM's thread" and `close_and_wait` already waits for tickets. `process.exit()` from a non-owner thread is a new case (`endsProcessUnlessInWorker`, `process.builtins.ts:164`) |
| 7 | `JSEnvironmentVariableMap.cpp`, `JSInspectorProfiler.cpp`: `vm.heap.runWithOtherClientsStopped([&]{ HeapIterationScope ... })`; `JSLockHolder` in `resetDateCachesAfterTimeZoneChange` | Heap walks are legal only with the other mutators stopped | **Remains**: same two sites (`JSEnvironmentVariableMap.cpp:51`, `JSInspectorProfiler.cpp:105`), and `bindings.cpp:3775` discusses a third. `process.env.TZ = ..` is reachable from any program |
| 8 | `NodeVMRunTermination.cpp`: `notifyNeedTerminationForCurrentThread()` | A `node:vm` timeout stops its own thread only | Remains if `node:vm` may run on a Thread; otherwise `owner` |
| 9 | `JSONRowsToJS.cpp`: `JSONCache::live(m_vm)` | Per-thread JSON atom cache with the GIL off | Engine API shape. Needed only if SQL row decoding may run on a Thread |
| 10 | `URLPatternComponent.cpp`: `regExp->ovectorSpan(getVM(globalObject))` | The RegExp match buffer became per thread | Engine API shape. In AOT, RegExp literals are compiled code with their own buffers (not studied here) |
| 11 | `TopExceptionScope.rs` / `TopExceptionScopeBinding.cpp`: 56 -> 72 bytes | ABI of a debug-only struct | Only if the engine changes it again |
| 12 | `test/js/bun/net/socket.test.ts` +34 | `Bun.listen` wrappers swept by `new Thread(...)`, with `BUN_JSC_useJSThreads=1` | A test like it, as an executable |

What r17 did **not** do, by its own docs: "Standing embedder items that matter only with the flag on (**Bun natives on engine-spawned threads, the marshalling predicate, the accept loop**)" (`FLAG-OFF-LANDING.md` §2.5). The design for natives was `SPEC-nativeaffinity.md` (DRAFT rev 11): a `concurrent-ok` byte on `NativeExecutable`, default Locked, one per-VM native serial lock. And the embedder contract of `SPEC-ungil.md` §F.6: (a) the API lock excludes only embedder threads; (b) promise reactions registered by the embedder and settled by a spawned thread **run on the settler**, off the loop; (c) blocking native sections on spawned threads must release heap access; (d) the first VM constructed is the only one that can spawn; (e) native code on a spawned Thread never enters or creates another VM.
(b) is the sharpest for Bun: every native that resolves a promise assumes that its reactions run on the loop's thread.

What AOT removes from that list: everything about JIT code, inline caches patched in place, watchpoints, tiering, `CodeBlock` jettison (other reports). What it adds: payload-in-cell classes, `directEntry` thunks that bypass `NativeExecutable` (so a bit there would not be consulted), nextTick and streams as compiled JS with module-level state, `Instance` words.

---

## 10. N-API, FFI, native addons under AOT

### 10.1 What AOT changes

- `require("./addon.node")` embeds the addon; `process.dlopen(module, path)` loads from disk (`aot-compatibility.mdx:691`). Both are `machineCodeLoaders` with `runsUnseenCode: false` but `loadsMachineCodeAs` marks (`unseen-code.builtins.ts:4-13`, `marks.builtins.ts:47-51`), reported as `UnseenCodeCause::RunsMachineCode`. *"An addon is native code. It can keep, call and change whatever your program hands it, right away or later, and it can read, call and change globals by name."* (:693). For `--trim`: "Native code can call anything".
- `napi_run_script` throws the `EvalError` in a closed program.
- `PR_SET_MDWE` after the image is mapped (`compiled_program.rs:307-318,366`): no new executable memory, so `cc()` and JIT-ing addons fail. It is process-wide, so thread-neutral. The hardened seccomp filter is installed with `ALL_THREADS` (`install`, :260-283).
- `bun:ffi`: call stubs and 256 callback entries are in the image (`ffi/FFIStubs.*`, `ffi/FFICallbackEntryPool.*`).

### 10.2 What pins them to a thread

**N-API** (`src/jsc/bindings/napi.h`, `napi.cpp`, `src/runtime/napi/`):

1. `napi_env` = `NapiEnv`, one per (module, global object), kept in `GlobalObject::m_napiEnvs`. Node's contract, which addons rely on: every `napi_*` call except the `napi_threadsafe_function` family is made on the env's thread.
2. `napi_module_register` (`napi.cpp:790-800`): `Zig::GlobalObject* globalObject = defaultGlobalObject();` (thread-local), then unlocked `napiModuleRegisterCallCount++` and `m_pendingNapiModules.append`. It runs inside `dlopen()` from a static constructor. `process.dlopen` brackets it with `std::exchange` of those fields (`BunProcess.cpp:574-580,658-726`).
3. Finalizers: *"A collection's end phase can run on the collector thread; an addon's finalizer only ever runs on the JS thread."* (`napi.h:461-467`): `if (inGC() && (mustDeferFinalizers() || WTF::Thread::mayBeGCThread())) Bun__napi_enqueue_finalizer(...) else finalize_cb(this, ...)`. A second mutator that sweeps is neither "in GC on a GC thread" nor the env's thread: the `else` arm would run the addon's finalizer on it.
4. `napi_threadsafe_function`: "hold a raw pointer to this env's event loop" (:222-224); registry under `m_threadSafeFunctionsLock`, "added and removed on the JS thread only" (:298-300).
5. Async work: `m_asyncWorkOnThreadPool` atomic; `complete` runs on the loop.
6. `napi_get_uv_event_loop`: `RuntimeState.uv_loop`, "The `uv_loop_t` that addons get for this VM. Stored inline because addons keep its address" (`jsc_hooks.rs:107-110`), found through the thread-local.
7. Handle scopes, `m_pendingException`, `napi_set_instance_data`: per env, unlocked.
8. `env->vmHandle()` (:198-201): "how a thread holding an env ref (a finalizer fired off the JS thread) posts work to the VM": the marshalling primitive is already there.
9. Workers: each Worker that loads the addon gets its own env; `allow_addons` is inherited and can only be narrowed (`web_worker.rs:396-399`).

**FFI**:

1. A plain `JSCallback` must be called on the thread that runs the realm: `ffiCallbackDispatch` enters JS directly. `threadsafe: true` posts instead: `ffiCallbackDispatchThreadsafe` -> `FFIContext::threadsafeDispatch()` = `Bun__jscFFIThreadsafeDispatch` -> `ScriptExecutionContext::postTaskTo(contextId, Regular, ...)` (`src/jsc/bindings/JSCFFIBridge.cpp:66-72,85-90,109-117`). `m_threadsafeState` is atomic (`ffi/JSFFICallback.h:79-93,109`). The context id is that of the creating realm, so the callback always runs on the owner thread.
2. The entry pool is process-wide under a lock (`ffi/FFICallbackEntryPool.h:52-57`): 256 for all threads and Workers together.
3. A foreign function call blocks its thread holding heap access (no release seen in Bun's bridge; not traced in `ffi/FFICallHost.cpp`). With a stop-the-world collector a long C call on one mutator stalls all. The threads docs demanded brackets for "spawned-thread blocking native sections" (§F.6 (c)).
4. `dlopen`ed libraries may have their own thread affinity. Nothing in Bun knows.

**V8 shim** (`src/jsc/bindings/v8`, `m_pendingV8Modules`): `v8::Isolate::GetCurrent()`-style lookups go through the thread-local global (not read in detail).

Conservative reading: everything in this section is `owner`, and a `Thread` that needs an addon goes through `napi_threadsafe_function`-like posting. Nothing under AOT makes addons more thread-neutral than under the JIT.

---

## Hazards with several mutators

State written at run time that a second JavaScript thread in the same realm could see. "MS" = memory safety (type confusion, wild pointer, use after free). "JS" = only a wrong JavaScript-visible value or order. "Hang" = lost wake-up or deadlock.

### H-A. Thread-local lookups that stand for "the VM"

| # | State | Sequence | Class |
| --- | --- | --- | --- |
| A1 | `VM` slot (`VirtualMachine.rs:696`) | native body calls `VirtualMachine::get()` / `global.bun_vm()`; slot empty on thread B; `unwrap_unchecked` | MS (UB, null deref). 253 + 392 sites |
| A2 | the same slot, if it were filled with the owner's pointer | `as_mut()`/`get_mut()` give `&mut VirtualMachine` on two threads | MS (aliasing UB; every `Vec`, map, `Option<Box>` in 5.3) |
| A3 | `RUNTIME_STATE` (`jsc_hooks.rs:117`) | `timer_all_mut()`: `debug_assert!(!null)` then `&mut (*state).timer` | MS |
| A4 | `CACHED_GLOBAL_OBJECT` -> `defaultGlobalObject()` | `checkIfNextTickWasCalledDuringMicrotask`, `napi_module_register`, 13 more | MS (null deref) |
| A5 | `uws::Loop::get()` | a socket, poll or timer made on B registers with B's loop, which nobody runs | Hang, then MS at teardown |
| A6 | WTF atom table of the thread | `Identifier::fromString`, `AtomString` on B use B's table: two atoms for one text, so pointer comparison of property names fails. The image's static strings are atomized per thread (test in 2.5) | JS at least. MS if compiled code or caches assume one `UniquedStringImpl*` per name (engine reports) |
| A7 | `Output::SOURCE`, console lock counts, `PENDING_TIME_LOGS` | `console.log` on B uses an unconfigured writer (`configure_named_thread` is called per Worker thread, `web_worker.rs:643-649`) | JS / lost output; not checked whether the zeroed `Source` is safe to write through |
| A8 | `StackCheck` bound | set by `configure_thread()` at VM init on the owner | MS (stack overflow undetected) if natives on B consult an unset bound |
| A9 | `IS_MAIN_THREAD_VM`, `MOCKED_TIME_NS`, `PRNG` | default values on B | JS |

### H-B. Multi-word updates and check-then-act in Bun's runtime

| # | State | Sequence | Class |
| --- | --- | --- | --- |
| B1 | `EventLoop.entered_event_loop_count` (`event_loop.rs:99,313-333`) | read, compare with 1, maybe drain, write back | JS (microtask order), feeds B2 |
| B2 | tick ring (`next_tick_queue.ts:11,31-74`) | `tick = ring.write; next = tick.next; fill tick; ring.write = next; flag = 1` | JS (lost or doubled ticks). Whether a torn ring is also MS depends on the engine's guarantees for racing field stores |
| B3 | `$asyncContext` field 0 | set on `run()` entry, restored on exit; captured by every `nextTick`, promise reaction, timer | JS, **security relevant**: one request's ALS store seen by another thread's continuation |
| B4 | `async_hooks.ts` `isTrackingEnabled`, `next_tick_queue.ts` `isSetUp` | `if (!flag) { flag = true; setUp() }` | JS (double set-up), then B2 |
| B5 | `tasks`, `immediate_tasks`, `deferred_tasks`, `yield_tasks` | `Vec` push from B while A drains | MS |
| B6 | `timer::All` heap | `setTimeout`/`clearTimeout` from B | MS (intrusive pairing heap) |
| B7 | `m_aboutToBeNotifiedRejectedPromises` | `promiseRejectionTracker` runs on the thread that rejects | MS (vector) |
| B8 | `LazyProperty` / `LazyClassStructure` (153 in the global, more in JSC's) | test the tag bit, run the initializer, store | MS or assertion (JSC marks "initializing" in the same word; two initializers give two Structures for one class, which breaks `pristineTest: "InstanceStructure"` and `instanceStructureCppExpression` in the table) |
| B9 | `subspaceForImplSlow`, `subspaceForClass` | `if (!slot) slot = make()`; then allocation through one `LocalAllocator` | MS |
| B10 | `StrongRootBlock` list, cursor, bitset, count | `Strong::create` / `destroy` | MS (lost GC root) |
| B11 | `RefCount` / `CellRefCounted` | `set(get() + 1)`; `set(get() - 1); if 0 destroy` | MS (UAF, double free). Unchecked in release |
| B12 | payload-in-cell structs with `mutates` | `&mut self` beside `&self` | MS |
| B13 | `JsCell`, `Cell`, `RefCell` in wrapped structs | any | MS |
| B14 | `transpiler.log` swap (`web_worker.rs:329-340`), resolver caches, `ref_strings`, `source_mappings` | `require()`/`import()`/`new Worker` of a computed path from B | MS |
| B15 | `sourceProviderMap`, `onLoadPlugins`, `commonjs_custom_extensions`, `requireMap` | hash maps | MS |
| B16 | `m_pendingNapiModules` + count | `exchange`, `dlopen`, read back | MS / wrong module |
| B17 | `body_value_pool` (hive allocator), server request-context `POOL` | bitset allocate and free | MS |
| B18 | `process.env` (`proxy_env_storage` is locked, the `env_loader.map` is not), `process.chdir`, `process.umask`, `TZ` change + `HeapIterationScope` | process-wide state behind per-VM caches | JS; MS for the heap walk (r17 #7) |
| B19 | `vm.entryScope` test in `drainMicrotasks` (:3514) | "no script on the stack" is judged per JSC VM while B runs script | JS (ALS cursor wiped under B) |
| B20 | `exit_handler`, `is_shutting_down`, `process.exit()` on B | teardown runs on B while A runs script. `Strong::destroy` and others test `is_shutting_down` unsynchronised | MS |
| B21 | `REFUSED_CHANGES`, `RUNNING_TEST_FILE`, `PATHS` | `Guarded` (mutex) | safe |

### H-C. "This holds until the next call" assumptions at the native border

| # | Assumption | Where | Class |
| --- | --- | --- | --- |
| C1 | A borrowed buffer or string stays as it is for the length of a `capability: "none"|"heap"` call: `type.borrowsHeap`, `isReadInPlace`, "a rope stays a rope" (`bindgen-lib-internal.ts:329,2416`; `collect-bindgen.ts:364-367`) | every native that takes `&[u8]` of an `ArrayBuffer`/`Uint8Array` or a string view | MS: another thread can `transfer()`, detach, resize or (for ropes) resolve it meanwhile. Work-pool jobs pin buffers (`ThreadIsolatedArg`, `src/jsc/thread_isolated.rs`); synchronous calls pin nothing |
| C2 | The table's `reads`/`writes` heaps, `deterministic`, `runsOnlyOperandCode`, `freshness: "Unaliased"|"Yes"`, `pristine` describe **all** that can change during a call | every effect summary the optimizer uses around a native call | engine reports; the table has no notion of a concurrent writer |
| C3 | `directEntry`: "A C function that takes the operands untested" (`vocabulary.ts:407-408`). The compiler proved the types before the call | a proof that is not "forever" (shape, slot filled, not frozen) can be undone by B between test and call | MS (type confusion inside native code) if any direct form relies on a non-permanent proof. `Instance` class tests on host cells are permanent (a cell never changes class) |
| C4 | `customGetter` "takes its receiver for an instance without a test" | same | permanent proof, fine |
| C5 | `CallbackSpec.times` has no "concurrently" | flow, escape analysis (`AllocateInFrames`: an object that "doesn't escape can lie in the frame") | MS if a closure handed to `Thread` is modelled as `Once`/`Later` and captures frame-allocated objects. `retained in nativeMemory` + `everything` (the undeclared default) is safe: it escapes everything |
| C6 | `ProgramInstances::AtMostOne` | `countProgramInstances` | unaffected by threads in one Instance, but "at most one Instance" lets the compiler treat module variables as process-unique storage: they become shared mutable words |
| C7 | `host_reads`: native code reads `default`/`then` of an entry module once, on the thread that loaded it | `entries.rs` | a `Thread` entry that is a function, not a module, needs no host read |

### H-D. Lifetime and teardown

| # | Hazard | Class |
| --- | --- | --- |
| D1 | Destructors and finalizers on the sweeping thread (r17 #1, #2; `napi.h:461`) | MS |
| D2 | Payload-in-cell `drop_in_place` cannot be posted: the memory is the cell | MS, or a design constraint: such cells are swept by the owner only, or their `Drop` must be thread-neutral |
| D3 | `close_and_wait` counts tickets, not Threads. `destroyVM` loops on the `Ref<VM>` count (r17 #6) | MS |
| D4 | Worker + Thread: `terminate()` uses `vm_handle.request_termination()` -> one VM trap. Threads of that Worker's VM must all stop before `shutdown()` frees the arena and `RuntimeState` | MS |
| D5 | `WebWorker__endForExhaustedHeap(Bun__getVM())` as the cage's last-reserve handler (`ZigGlobalObject.cpp:346-347`): thread-local again, called on whichever thread exhausts the cage | MS |
| D6 | Crash handler and fault handler (`AOTFaultHandler`: null tests are faults): signal handlers run on the faulting thread and look up "the" VM / Instance | engine reports. Bun side: `bun_crash_handler::report_aborts()`, `panic(main thread)` naming |

### H-E. Build-time

| # | Hazard | Class |
| --- | --- | --- |
| E1 | `CompileOptions` is fixed before flow. A thread-aware code shape chosen from D3 needs a decision inside the engine | design |
| E2 | An open program (eval, inspector, externals) cannot prove "no Thread" | over-approximation, costs speed only |
| E3 | `find_reached_runtime_code` and `--trim` drop library functions nothing reaches. A function reached only from a Thread's callback is reached like any other (flow follows the closure), provided the `Thread` row says `Calls` | JS (`was not compiled into this executable` error) if the row is wrong |
| E4 | `check_template`: an executable template must match revision, module stamp, engine and table descriptions. A threads-capable run time is part of that identity for free | none |

---

## What helps

1. **The build knows.** Whole-program flow already answers "is this built-in constructed anywhere, and where" with a witness (`BuiltinUseRecord`), and closed programs are the default. A program that never reaches `Thread` can be compiled exactly as today, so the cost of threads can be zero for everybody else, in compiled code, stubs and image tables (all generated per program).
2. **A precedent for every plumbing step**: fact from imports (`imports_cluster`), fact from IR (`validates_by_type`), fact from flow (`ProgramInstances`, `AdmitsUnknownJavaScript`), constants folded into Bun's own JS (`CAN_BE_CLUSTER_WORKER`), option bits (`AdmittedCode`), image flags read before the first VM, `OptionsConflict` at load, compile-only module (`bun:bundle`), "compiled after all" in `bun run --aot`.
3. **One table for all natives, with a build-breaking "nobody has classified this" rule** (`classifyUnseenCode`). The same ratchet gives a total, reviewed `threads` classification with a safe default, and Bun can change it without an engine release.
4. **The default row is maximally conservative** (undeclared = takes, keeps, calls and returns anything), so handing a closure to an undescribed `Thread` constructor would already defeat frame allocation, proven layouts of escaped objects, and so on. Soundness of *sequential* reasoning about escaped objects is not at risk from a missing description.
5. **bindgen's capabilities**: `none`/`heap`/`js` are enforced by Rust types. Members with `capability: "none"` and immediate operands are machine-checkably free of allocation and re-entry: a small audited `any` set can start there.
6. **Built-ins are read-only.** Prototype objects, constructors, `Math`, `JSON`, seven Bun classes cannot be changed at run time, so the largest body of shared mutable state in a JS realm is immutable by rule, and calls to them are resolved at build time.
7. **No JIT, no code patching, image read-only.** Nothing in Bun's half writes code or image data after `use_image`. MDWE makes that a kernel guarantee.
8. **"The door" exists** (`Ticket`, `VmHandle`, `ConcurrentTask`, `postTaskTo`, `EventLoopTask`, `ThreadIsolated`): a tested, lint-guarded way for a non-owner thread to hand work to the owner, including teardown that waits for outstanding tickets. `owner` natives called from a Thread can be expressed as "post and wait" on it; r17's `postToVMThread` was five lines on top of it.
9. **`VirtualMachine` is `!Send + !Sync` by a compile-time assertion** and the set of `unsafe impl Send/Sync` is frozen by a lint. A refactor that makes parts shareable will be told by the compiler where the borrow rules bite.
10. **`defaultGlobalObject(JSC::VM&)` and `bunVM(JSC::VM&)` already resolve through the client data**, not TLS, so C++ has a thread-neutral path where a `VM&` or global is at hand. `directEntry` thunks always receive the realm.
11. **`StringImpl` refcounts are atomic** in this WTF.
12. **Workers already prove the process-wide parts**: one image under N VMs, thread pool, HTTP thread, FFI entry pool, mismatch log, crash reports with several JS threads.
13. **Debug builds detect much of H-B**: `ThreadLock` in every `RefCount`, `MimallocArena::assert_owning_thread`, `bun_vm()`'s "This thread lacks a Bun VM" panic, `bun_vm_ptr called off the JS thread`. A debug `bun-aot` run of a threaded test suite is a cheap census of which natives touch owner state.
14. **Trim** gives "pay only if reached" for the code that only `Thread` leads to, with reporting (`--explain=trim`) for free.
15. **A ThreadSanitizer lane exists** for `testaot` with a known-races list that "only gets shorter". The harness could be pointed at compiled programs.

---

## Not found / not sure

1. **Engine vs Bun split of the 6,330 entries / 271 classes**: not determined (6.4). Needs `aot_builtin_table.json` from a build.
2. **Automatic discovery of Worker files**: I found none in the AOT path and the docs say to pass them. `src/runtime/cli/compile_command/entry_points.rs` (679 lines) has neither "worker" nor "new URL" in it. I did not read the non-AOT bundler (`src/bundler/*` outside `aot/`) for `new Worker(new URL(...))` handling.
3. **`startsInstance`**: the column (`vocabulary.ts:552-553`: "Makes a second realm that runs the compiled program in the VM of its caller: one whose method table has a module host"), the flag and the flow rule exist, but **no declaration sets it**: a text search of `src/codegen`, `src/runtime`, `src/jsc` and `aot/builtins` finds the word only in the generator. So `countProgramInstances` answers `AtMostOne` for every program whose reached functions are known. Whether a test host under `aot/tests/host/` sets it was not checked.
4. **`load_entry_point_for_web_worker` under AOT**: I did not trace from there to `importCompiledModule` / the engine's module evaluation.
5. **The uws loop being thread-local**: `uws_get_loop()` is in `packages/bun-uws` / `bun-usockets`, outside the sparse checkout. One comment says "the thread's loop" (`event_loop.rs:1107`), two say "process-global" (:1132, :1470). Workers having their own loops implies thread-local.
6. **The 4.3 MB run-time half**: no file lists it. The description in 7.2 is inferred from folder roles and trim features.
7. **What a zero-initialised `Output::Source` does** when a thread that never called `configure_named_thread` prints.
8. **Whether FFI calls release heap access** around the foreign call (`ffi/FFICallHost.cpp` not read).
9. **V8 shim** thread assumptions: not read.
10. **`test/internal/source-lints/vm-thread-door.test.ts`**, `test/aot/programs/**`, `test/aot/README.md` groups about Workers: outside the sparse checkout or absent.
11. **`hostReadContexts` "TestFile", `bun test --aot --parallel`**: how several test files share a process under compiled mode was not studied.
12. **How `--compile-in-place` and several Instances in one VM** (`Zig__GlobalObject__createForTestIsolation`, `ZigGlobalObject.cpp:768`) interact with `ProgramInstances`: not studied.
13. **`src/codegen/aot-schema/` and `src/codegen/known-helpers/`** named in the task do not exist in this checkout under those names (`ls src/codegen` shows neither). The schema generator is the engine's `aot/ir/generator/`, driven by `scripts/build/aot.ts:161-188`. Known helpers are `src/aot/known_helpers/*.rs`.
