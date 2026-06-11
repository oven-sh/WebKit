# AOT JSC — Ground-Truth Survey of This Tree

**Provenance note.** This is a CLEAN-ROOM survey. Filip Pizlo is known to have an
unpublished AOT-JSC design; we have NOT seen it, and nothing here is derived from it or
pretends to be. Everything below comes from (a) reading this tree
(`/root/WebKit`, branch `jarred/threads`, Bun JSC fork) and (b) public prior art cited in
§4. File:line references are against this tree as of 2026-06-10.

**Role of this document.** Uncapped inventory feeding `docs/aot/AOT-DESIGN.md` (capped).
The design doc must not contradict a measurement or citation here without recording a
correction.

Measurement environment: `WebKitBuild/Release` is a `RelWithDebInfo` (`-O2 -g`) x86-64
Linux build with `ENABLE_JIT=ON, ENABLE_DFG_JIT=ON, ENABLE_FTL_JIT=ON,
ENABLE_WEBASSEMBLY=ON (BBQ+OMG), ENABLE_STATIC_JSC=ON` (WebKitBuild/Release/CMakeCache.txt).
All sizes below are from that build; a shipping build would add `-Os`/LTO/`--gc-sections`
and strip, so these numbers are conservative *upper* bounds for code that is kept and
honest bounds for code that is dropped. Per-function code sizes in §3.5 were measured
dynamically by running the existing `WebKitBuild/Release/bin/jsc` with `JSC_logJIT=1`
(`assembler/LinkBuffer.cpp:139-169` prints one `… N bytes` line per finalized
compilation) over eight JetStream3/Octane workloads — no rebuild was performed.

---

## 1. Reusable machinery already in this tree

### 1.1 LLInt jitless mode — the always-correct fallback tier

JSC already runs full ECMAScript with the JIT completely absent:

- `Options::useJIT()` gates every entrypoint choice. `llint/LLIntEntrypoint.cpp:60,116,143,170`
  (`setFunctionEntrypoint`, `setEvalEntrypoint`, …): when `useJIT()` is false the CodeBlock's
  "JITCode" is a shared `DirectJITCode` whose entry is the LLInt prologue label
  (`llint_function_for_call_prologue` etc., `llint/LLIntEntrypoint.cpp:96-110`), i.e. the
  interpreter is plugged in through the *same* `setJITCode` interface every tier uses.
- With `useJIT=0`, the executable allocator is never created:
  `jit/ExecutableAllocator.cpp:160` (`RELEASE_ASSERT(!Options::useJIT())` in the
  jitless path) and `jit/ExecutableAllocator.h:74` — the jitless
  `ExecutableAllocatorBase::allocate()` returns `nullptr`. So "no RWX/JIT memory at
  runtime" is an already-supported configuration, not a new mode. This is the
  configuration Apple ships for lockdown-mode / non-entitled iOS processes.
- The LLInt itself is tiny: the offlineasm-generated interpreter blob is
  **0.38 MB** (measured: `llintPCRangeStart`=0x11b4204 to `llintPCRangeEnd`=0x1216028 in
  `bin/jsc`), plus ~0.33 MB of named LLInt/slow-path C++ helpers (`llint/LLIntSlowPaths.cpp`,
  `runtime/CommonSlowPaths.cpp`). Yarr regexps fall back to `yarr/YarrInterpreter.cpp`;
  Wasm falls back to the in-place interpreter IPInt (`llint/InPlaceInterpreter64.asm`,
  ~0.10 MB of `ipint_*` text).

**Consequence for AOT.** The product runtime can be "LLInt + runtime + GC + precompiled
code", with LLInt as the semantic backstop for everything the offline compiler did not or
could not compile (`eval`, `new Function`, dynamically loaded code, deopt residue). No
semantics are lost: anything is *allowed* to stay on LLInt.

### 1.2 CachedBytecode / CodeCache — bytecode serialization, and its limits

`runtime/CachedTypes.cpp` (2,795 lines) is a complete serializer for **unlinked**
bytecode. What it covers (class list at `runtime/CachedTypes.cpp:392-1575`):
`CachedCodeBlock` for Program/Module/Eval/Function (`CachedFunctionCodeBlock` etc.),
instruction streams (`CachedInstructionStream:1511`), metadata tables
(`CachedMetadataTable:1529`), identifiers/strings (`CachedUniquedStringImpl:826`),
jump tables (`:914,:939`), symbol tables and TDZ environments (`:1254,:1125`),
compiled-out constants including RegExp objects (`CachedRegExp:1333`), BigInts
(`CachedBigInt:1376`), template objects (`:1352`), and immutable butterflies
(`CachedImmutableButterfly:1297`). Entry points:
`serializeBytecode` (`runtime/CodeCache.cpp:423`, decl `runtime/CodeCache.h:270`),
`encodeFunctionCodeBlock` (used by the shell's disk cache, `jsc.cpp:1324`), and decode
validation via `GenericCacheEntry::isUpToDate` (`runtime/CachedTypes.cpp:2586`, checked at
`:2651,:2669,:2687` — cache version + tag, so stale caches fail safe to reparse).
The shell already wires a transparent disk cache (`jsc.cpp:1309-1417`,
`ShellSourceProvider::cacheBytecode/commitCachedBytecode`).

**How complete?** Complete for the *unlinked* level only. Not covered: linked
`CodeBlock`s, profiling data (`ValueProfile`s, exit-site data), any JIT artifact,
`Structure`s, or anything holding a runtime pointer. That is by design — unlinked
bytecode is VM-instance-independent, which is exactly the property an AOT image needs.
**Consequence:** the AOT image format can *contain* a CachedBytecode payload verbatim as
its "level 0" (every function gets bytecode in the image; compiled machine code is an
optional acceleration attached per-function), and the encode/decode infrastructure
(Decoder/Encoder, offset-based `CachedPtr`, dedup) is reusable for the new sections.

### 1.3 The DFG/FTL/B3 pipeline — where machine code is born, and what "linking" already means

The offline compiler is mandated to be the existing pipeline. Where the bytes come from:

- B3→Air: `b3/air/AirGenerate.h:46` — `JS_EXPORT_PRIVATE void generate(Code&, CCallHelpers&);`
  Air's output is *not* memory; it is emission into a `CCallHelpers` (a `MacroAssembler`),
  i.e. an in-RAM instruction buffer plus a list of unresolved label/call records.
- DFG backend: `dfg/DFGSpeculativeJIT.cpp` emits through the same `MacroAssembler`
  (`JITCompiler`), FTL through `ftl/FTLCompile.cpp` / `FTLLowerDFGToB3.cpp`.
- The buffer becomes executable only in `LinkBuffer`:
  `assembler/LinkBuffer.cpp:290` `copyCompactAndLinkCode()` performs branch compaction
  and resolves the assembler's link records while *copying* the buffer into memory
  obtained from `ExecutableAllocator::allocate` (`jit/ExecutableAllocator.h:346`);
  `performFinalization()` at `:559`.

**Key observation.** Everything upstream of `LinkBuffer` is already
position-independent-by-construction: code addresses do not exist until copy time, and
every cross-reference is a link record. "Emit a relocatable object instead of JIT
memory" is therefore a *backend swap at the LinkBuffer seam*, not a compiler rewrite:
an alternative LinkBuffer that (a) copies into a plain file section, (b) writes the
unresolved external references (operation calls, thunk calls, baked pointers — §2) as a
relocation table instead of resolving them. The compactor already proves the code
tolerates being re-addressed at copy time.

Caveat to record: `MacroAssembler` *immediates* are resolved before LinkBuffer (e.g.
`move(TrustedImmPtr(ptr), reg)` bakes the pointer into the instruction stream, §2). Those
are reachable because every such site goes through typed wrappers
(`weakPointer`, `TrustedImmPtr(structure)`, `callOperation`), which is where the
relocation-recording hook belongs.

### 1.4 offlineasm — "generate code at build time" is already a JSC-native concept

The entire LLInt is built *ahead of time* by a Ruby assembler-generator:
`offlineasm/asm.rb`, `offlineasm/arm64.rb`, `offlineasm/cloop.rb`, …
(listed `Source/JavaScriptCore/CMakeLists.txt:238-245`), producing `LLIntAssembly.h`
(custom command around `CMakeLists.txt:466-471`, including mtime-handling comments — this
is mature, load-bearing build machinery). Build-time offsets are validated by the
`LLIntOffsetsExtractor` binary (present in `WebKitBuild/Release/...`). So the project
already maintains a toolchain step that compiles JSC's lowest tier to platform assembly
offline, including a portable C fallback (CLoop). The AOT object emitter is culturally
and mechanically the same kind of artifact.

### 1.5 OSR exit — what exits need if the runtime has no JIT

How an exit works today (DFG; FTL is analogous via `ftl/FTLOSRExit.h`):

- **Compile time** records per-exit *data*: an `OSRExit` descriptor + `Operands<ValueRecovery>`
  (how to rebuild each bytecode-visible value from registers/stack/constants), stored in
  the code block's DFG data.
- **First-exit time** the exit *ramp* is lazily compiled with the JIT:
  `dfg/DFGOSRExit.cpp:270` → `OSRExit::compileExit(...)` (`:391`). Comments at `:909-912`
  confirm the ramp is compiled once on first hit, then reused.
- **Where the ramp lands**: `dfg/DFGOSRExitCompilerCommon.cpp:426` `adjustAndJumpToTarget`
  decides at `:461`:
  `bool exitToLLInt = Options::forceOSRExitToLLInt() || codeBlockForExit->jitType() == JITType::InterpreterThunk;`
  and jumps to `LLInt::normalOSRExitTrampolineThunk()` / `checkpointOSRExitTrampolineThunk()`.
  Same for inlined-frame return PCs: `:148-183` `callerReturnPC` selects
  `op_call_return_location` LLInt labels when `callerIsLLInt`. The option
  `forceOSRExitToLLInt` (`runtime/OptionsList.h:615`) makes *every* exit target LLInt and
  is exercised by CI.

**So exits targeting LLInt frames is an existing, tested mode** — exactly what an AOT
runtime (which has no baseline JIT) requires. The gap: the ramp itself is generated at
runtime by the JIT. Two known-shape fixes, for the design doc to choose between:
(a) precompile every exit ramp offline into the image (space: ramps are straight-line
materialization code; dedupable since `compileExit` is driven by the recovery stream); or
(b) interpret the `ValueRecovery` stream in C++ at exit time (slow path only; the data is
already self-describing). Either way, *all inputs the ramp needs are compile-time data*
(operands stream, inline-call-frame tree `DFGCommonData::inlineCallFrames`,
`dfg/DFGCommonData.h:120`), plus runtime-resolved LLInt label addresses, which are link
symbols.

### 1.6 Watchpoints / structures — what a precompiled speculation must validate at load time

The DFG/FTL already separates "speculations I made" from "code I emitted", because
compilation happens on a concurrent thread and the world may change before installation:

- `dfg/DFGDesiredWatchpoints.h:243` `class DesiredWatchpoints` — the full catalog of
  watchable assumptions: `WatchpointSet`/`InlineWatchpointSet`, `SymbolTable`,
  `FunctionExecutable`, `JSArrayBufferView` (detach), `ObjectPropertyCondition`,
  `DesiredGlobalProperty` (`:248-259`), plus this fork's butterfly-TTL sets (`:285-287`).
- Installation = revalidate-then-subscribe: `DesiredWatchpoints::reallyAdd`
  (`dfg/DFGDesiredWatchpoints.h:208,289`), driven from `DFG::Plan::reallyAdd`
  (`dfg/DFGPlan.cpp:602-611`) which also installs `m_identifiers`, `m_weakReferences`,
  `m_transitions`; `Plan::isStillValidCodeBlock` (`:587`) and the
  install-or-discard dance at `:660-681`.
- What survives into the installed code as *data*: `dfg/DFGCommonData.h:120-135` —
  `inlineCallFrames`, `m_dfgIdentifiers`, `m_transitions`, `m_weakReferences`,
  `m_weakStructureReferences` (note: `FixedVector<StructureID>` — 32-bit IDs, not
  pointers), `m_catchEntrypoints`, `m_watchpoints` (`CodeBlockJettisoningWatchpoint`),
  adaptive structure/inferred-value watchpoints, `m_jumpReplacements`.

**Consequence.** "Load a precompiled CodeBlock" is structurally the same transaction as
"install a concurrently-compiled Plan": deserialize the DesiredX sets, run
`isStillValid`-style revalidation against the live heap, subscribe watchpoints, and on
failure fall back to LLInt for that function. The machinery's *shape* is reusable even
where the serialized representation must be new (a `Structure*` in a watchpoint must be
re-derived at load time from a structure *recipe*, not a pointer — §2).

### 1.7 Baseline "data ICs" / shared baseline code — proof that JSC code can be CodeBlock-independent

This tree already generates baseline machine code that is **shared across CodeBlocks**:
`jit/JIT.cpp:1020` sets `jitCode->m_isShareable`, and `bytecode/CodeBlock.cpp:935`
(`Options::useBaselineJITCodeSharing()`) installs it on the *UnlinkedCodeBlock*
(`installUnlinkedBaselineCodeIfAbsent`), to be reused by every future linked instance.
This works because baseline code reaches per-instance state through `BaselineJITData`
(`jit/BaselineJITCode.h:124` — a constant-pool/IC-table object addressed by offsets,
`offsetOfGlobalObject():136`, `offsetOfStackOffset():137`) rather than baking pointers.
**This is the strongest existing proof that JSC machine code can be made
instance-independent and parameterized by a side table** — the exact discipline an AOT
image needs, already shipped for one tier.

### 1.8 Wasm BBQ/OMG — JSC already AOT-compiles modules, with a homegrown relocation list

`WebAssembly.compile` runs `Wasm::BBQPlan` over every function *before execution*
(`wasm/WasmBBQPlan.h:53`, `compileFunction` at `:74` / `wasm/WasmBBQPlan.cpp:150`).
Cross-function calls are emitted unresolved and recorded as
`Vector<UnlinkedWasmToWasmCall>` (`wasm/WasmBBQPlan.cpp:94`) — function-index + call
location — then *linked later* by patching:
`wasm/WasmCalleeGroup.cpp:323,:435` `MacroAssembler::repatchNearCall(call.callLocation, entrypoint)`.
That is a relocation table and a link step, in production, inside this tree. The wasm
tiers also demonstrate instance-independence: compiled wasm code reaches all
per-instance state through the pinned instance register, so one module's code serves
many instances. Shared infrastructure with JS tiers: same `MacroAssembler`/`LinkBuffer`,
same B3/Air backend for OMG, same `Plan` threading. What wasm does *not* have here is
on-disk persistence of the compiled module — but it proves every mechanical ingredient
(batch compile-before-run, unlinked call records, late binding) on the JS engine's own
backend.

### 1.9 What supports speculation WITHOUT a profile — the static-mode inventory

The primary AOT mode must compile from static analysis alone (no training runs). What
the tree already provides for that:

- **Generic-but-direct paths exist in the optimizing tiers.** The DFG/FTL are not
  profile-or-nothing: every value-consuming node has an `UntypedUse` lowering that
  compiles a correct generic path with no speculation (e.g.
  `dfg/DFGSpeculativeJIT.cpp:2925,4121,4197,4253` — `UntypedUse` cases throughout the
  backend). Where static analysis proves nothing, the offline compiler emits these —
  still direct machine code with inlined fast checks, not interpreter dispatch. No
  profile is ever *required* for the pipeline to produce code.
- **Universal cheap assumptions are watchpoints, not profiles.** The global object
  exposes the assumption set every JIT tier leans on regardless of profiling:
  `isHavingABadTime()` (`runtime/JSGlobalObject.h:1202`),
  `m_arraySpeciesWatchpointSet` (`:550`),
  `m_objectPrototypeChainIsSaneWatchpointSet` (`:557`), and the rest of the
  intact-builtin-prototype family. These are validated by *inspection at load time*
  (§1.6), no execution needed — they are the JS analog of Java AOT's "standard library
  not redefined" assumptions.
- **Constructor/object-shape inference has bytecode-level plumbing already.**
  `op_create_this` and `op_new_object` carry a static `inlineCapacity` operand
  (`bytecode/BytecodeList.rb:541-558`), consumed by
  `ObjectAllocationProfileBase::initializeProfile`
  (`bytecode/ObjectAllocationProfileInlines.h:35-90`), which combines the
  statically-supplied capacity with a prototype scan
  (`possibleDefaultPropertyCount`, `:151`) to pick the allocation structure. Notably,
  this fork's generator currently emits capacity 0 for `create_this`
  (`bytecompiler/BytecodeGenerator.cpp:3225`) — i.e. the operand is a *vacant slot the
  offline compiler can fill* from constructor-body analysis (`this.x=...; this.y=...`
  sequences statically determine shape and capacity). Object/array *literals* determine
  their shapes syntactically; `CachedImmutableButterfly`
  (`runtime/CachedTypes.cpp:1297`) shows literal-derived heap data already serializes.
- **Whole-program visibility replaces type feedback for call graphs (the CHA analog).** Bundled apps
  expose the full module graph at build time; module-graph-constant propagation
  (exports that are assigned once) plus closed call-graph analysis gives the offline
  DFG concrete callees and constants where today's pipeline would consult
  `CallLinkInfo` profiles. This is an analysis the design doc must specify; the survey
  point is that nothing in the pipeline *consumes* profiles in a way that lacks a
  static substitute — predictions enter via `Graph`/`SpeculatedType`, which any
  front-end analysis may populate.

**Consequence.** "No profile" degrades speculation quality, not correctness or
compilability: the spectrum is statically-proven speculation → watchpoint-validated
assumption → UntypedUse generic code → (worst case) leave the function on LLInt.
Profiles can only ever *upgrade* positions on this spectrum, which is why they are
safely confinable to an optional appendix.

### 1.10 Data-driven ICs: how far "IC as data + patchable constants" is already real

The handler-IC machinery is the strongest in-tree precedent for "speculation constants
bound by a data write, code immutable" — the mechanism an ARCH-L-style
selection/linking runtime needs:

- **The IC is a linked list of data records.** `bytecode/InlineCacheHandler.h:61`
  `class InlineCacheHandler`: each handler carries `m_structureID` (32-bit),
  `m_offset`, `m_uid`, a union of case payloads (holder/globalObject/customAccessor,
  newStructureID/sizes for transitions, module-namespace slot — `:171-186`), a
  `m_next` chain pointer (`:189`), and two code pointers `m_callTarget`/`m_jumpTarget`
  (`:166-167`). The full `offsetOfStructureID()/offsetOfOffset()/offsetOfNext()/…`
  accessor block (`:130-143`) exists because *shared stub code reads every speculation
  constant out of the handler record at runtime* instead of having it baked into
  instructions. CacheType enumeration (self-load, proto-load, replace, in-by-id,
  array/string length) at `:45-54`.
- **Binding a newly observed shape is a data write.** Installing/upgrading an IC means
  prepending a handler to the chain and publishing the head pointer:
  `bytecode/PropertyInlineCache.cpp:1149` `publishHandlerChainHead(headSlot, newHead)`
  (used at `:1210`). No instruction bytes change. This is exactly the
  "late-bound speculation constant resolved through a patchable (data) slot" pattern:
  the hot structure ID enters via `m_structureID` in the record, and dispatch reaches
  the right specialized body via `m_jumpTarget`. The W^X-clean rebinding primitive
  already exists and ships.
- **Pre-compiled, shared stub bodies already exist.** `InlineCacheHandler::createPreCompiled`
  (`bytecode/InlineCacheHandler.h:68`; called at `bytecode/InlineCacheCompiler.cpp:7475`)
  pairs a handler record with stubs from `createPreCompiledICJITStubRoutine`
  (`bytecode/InlineCacheCompiler.cpp:7553,7566,7604,7623`) — stub code generated once
  per *access shape*, not per site, parameterized entirely by handler data;
  `jit/GCAwareJITStubRoutine.h:97-110` shows the `SharedJITStubSet` sharing machinery.
  Handler ICs reach the top tier under `Options::useHandlerICInFTL`
  (`runtime/OptionsList.h:643`).
- **Calls have the same data-driven shape.** `bytecode/CallLinkInfo.h:568`
  `class DataOnlyCallLinkInfo` lives in `UnlinkedMetadataTable` storage (comment
  `:181`) and serves LLInt/Baseline call sites: the cached callee/target is metadata,
  consulted by shared dispatch, with C-ABI slow paths `llint_default_call` /
  `llint_virtual_call` / `llint_polymorphic_call` (`bytecode/CallLinkInfo.h:716,731,747`).
  Monomorphic call caching therefore already works as pure data even at the
  interpreter tier.

**The honest gap:** `InlineCacheHandler.h:28` is `#if ENABLE(JIT)` — the records are
data, but today their *stub bodies* are emitted by the runtime JIT, so a jitless build
compiles the whole subsystem out. For AOT, the finite catalog of shared stub bodies
becomes precompiled symbols in the image/runtime (the same way thunks would ship;
measured thunk pools are ~23-35 KB/process, §3.5), and the handler records keep doing
what they do now. Nothing about the *architecture* of handler ICs assumes runtime
codegen; only the current packaging does.

### 1.11 LLInt profiling — what a jitless runtime observes for free

Every profiling sink below is written by the *interpreter itself* (offlineasm or its
slow paths), is plain data in the metadata table, and none of the relevant headers are
`ENABLE(JIT)`-guarded (`bytecode/ValueProfile.h`, `bytecode/ArrayProfile.h` — verified
no `ENABLE(JIT)` conditionals). A no-JIT product build keeps paying for and producing
all of it:

- **Value profiles.** `llint/LowLevelInterpreter64.asm:77-82` `valueProfile(...)`
  stores the last-seen `JSValue` into `ValueProfile::m_buckets` in the metadata table;
  invoked by every profiled op and after every call via `dispatchAfterRegularCall`
  (`:84-95`). This is the raw feed DFG prediction propagation consumes — in AOT terms,
  the *selection signal* for which precompiled variant matches reality.
- **Array/shape profiles.** `bytecode/ArrayProfile.h:215` — `m_lastSeenStructureID`
  and `m_observedArrayModes` updated with relaxed atomic stores
  (`:225-229,:270-271`); LLInt array ops feed these.
- **Arithmetic observation bits.** `updateUnaryArithProfile` / `updateBinaryArithProfile`
  (`llint/LowLevelInterpreter64.asm:1235,1306,1331-1344`) record int/number/negative-zero
  outcomes per arith site.
- **Structure-resolved property ICs that double as shape profiles.**
  LLInt get_by_id self-caches via `GetByIdModeMetadata`
  (`bytecode/GetByIdMetadata.h:98-125`: Default {structureID, cachedOffset},
  ProtoLoad {structureID, cachedOffset, cachedSlot}, ArrayLength) — written by
  `LLIntSlowPaths.cpp:1111` and `setupGetByIdPrototypeCache` (`:966`), the latter with
  adaptive structure watchpoints (`:1001`). So a jitless runtime already *records the
  observed hot structure ID per access site, as data* — precisely the input an ARCH-L
  selector needs to bind a variant's symbolic structure constant.
- **Callee observation.** `DataOnlyCallLinkInfo` caching (§1.10) records the observed
  callee per call site at LLInt tier.
- **Tier-up triggers.** `llint/LowLevelInterpreter.asm:1830-1836` `checkSwitchToJIT`:
  one `baddis` (add-and-branch) on
  `UnlinkedCodeBlock::m_llintExecuteCounter.m_counter` at prologues, epilogues
  (`:1838`), and loop back-edges (`LowLevelInterpreter64.asm:2946`). The counter ticks
  regardless of JIT availability; with `useJIT=0` the triggered slow path declines.
  Repointing that trigger from "compile" to "select + link a shipped variant" reuses
  the counter, the threshold machinery, and the existing OSR-entry plumbing.

**Consequence for the ARCH-S vs ARCH-L cost model.** The marginal runtime cost of
ARCH-L's "runtime remains a profiler" premise is ~zero: all the above ships in the
jitless configuration today (it is the configuration's *normal state*, since upstream
uses it on iOS). The selector consumes profiles that exist; no training run and no new
instrumentation is required.

---

## 2. What the JITs bake into code — the relocation/materialization catalog

Each category below is an immediate the DFG/FTL writes into instruction bytes today
(rows 10-11, added r5: heap-side/table-side bakes that escape that framing).
For AOT each needs one of: **(R)** classic relocation against a link-time symbol,
**(M)** load-time materialization via an image-local table (the `BaselineJITData`
pattern, §1.7), or **(V)** revalidate-or-reject at load (the `DesiredWatchpoints`
pattern, §1.6).

| # | Category | Example emission site | AOT story |
|---|----------|----------------------|-----------|
| 1 | **Structure IDs** (32-bit, not pointers — `runtime/StructureID.h:38`, `bits():60`) compared as immediates in type checks, and stored on allocation | `dfg/DFGJITCompiler.h:261` `branchWeakStructure`; `dfg/DFGSpeculativeJIT.cpp:446` `emitAllocateJSObject(..., TrustedImmPtr(structure), ...)`, guard at `:1261` | M+V: IDs are runtime-assigned table indices; image stores a *structure recipe* (shape: prototype path, property names, indexing type), load resolves/creates and patches or table-loads the ID. The fact they are already 32-bit indices (post-StructureID refactor) and arrive in DFG data as `FixedVector<StructureID>` (`dfg/DFGCommonData.h:126`) makes a per-image ID-translation table natural. |
| 2 | **Cell pointers (weak references)**: frozen JS values — functions, prototypes, singletons — baked via `Graph::freeze` | `dfg/DFGGraph.cpp:1634` `FrozenValue* Graph::freeze(JSValue)`; registered at install by `DesiredWeakReferences::reallyAdd` (`dfg/DFGDesiredWeakReferences.cpp:87`) into `m_weakReferences` (`dfg/DFGCommonData.h:125`) | M+V: cannot survive serialization as pointers. Image stores a *path* to the value (global slot, builtin index, constant-pool entry); load materializes and either patches the immediate or the code loads from an image constant table. Speculations on identity become load-time checks. |
| 3 | **JSGlobalObject / VM pointers** passed as the first argument to nearly every operation call | `ftl/FTLLowerDFGToB3.cpp:2838,2880,2916,...` — `vmCall(..., weakPointer(globalObject), ...)` (hundreds of sites) | M: one well-known slot. Baseline already does exactly this: `BaselineJITData::offsetOfGlobalObject()` (`jit/BaselineJITCode.h:136`). AOT code should take globalObject from its data table, never as an immediate. |
| 4 | **Host/operation function pointers**: `operationXxx` C++ entry points (~0.36 MB / 736 `operation*` symbols measured in the binary), thunks, LLInt trampolines | every `vmCall`/`callOperation` site, e.g. `ftl/FTLLowerDFGToB3.cpp:2838`; OSR ramp targets `LLInt::normalOSRExitTrampolineThunk` (`dfg/DFGOSRExitCompilerCommon.cpp:461ff`) | R: these are symbols in the shipped runtime binary — textbook relocations (PLT-style or direct PC-rel). The easiest category. |
| 5 | **String atoms / identifiers**: `UniquedStringImpl*` baked for by-id ops and string constants | `dfg/DFGSpeculativeJIT.cpp:7851` `move(TrustedImmPtr(data), implGPR)` (string impl); identifiers carried in `DFGCommonData::m_dfgIdentifiers` (`dfg/DFGCommonData.h:123`) and `DesiredIdentifiers::reallyAdd` (`dfg/DFGPlan.cpp:608`) | M: `CachedTypes` already serializes/dedups identifiers (`CachedUniquedStringImpl`, `runtime/CachedTypes.cpp:826`); load interns them and fills the table. |
| 6 | **VM-global singletons**: small-strings table, sentinel values, scratch buffers | `dfg/DFGSpeculativeJIT.cpp:2947` `TrustedImmPtr(vm().smallStrings.singleCharacterStrings())`; `:7046` scratch buffer | M: per-process addresses; either relocate against runtime-exported symbols where static (rare) or route through the data table. Scratch buffers (`:7046`) are compile-thread allocations that must become image-owned buffers. |
| 7 | **Inline cache data**: IC fast paths bake structure IDs + offsets; repatching rewrites code at runtime | [CORRECTED r5/C30 — the original row named `StructureStubInfo` and `useDataIC`, NEITHER of which exists in this tree (the string `StructureStubInfo` survives only in a stale comment, `runtime/RaceAmplifier.h:77`; the only relevant option is `useHandlerICInFTL`, `runtime/OptionsList.h:643`, default false).] The IC subsystem here is `PropertyInlineCache`/`InlineCacheHandler` (`bytecode/PropertyInlineCache.h`, `bytecode/InlineCacheHandler.h`). The data-dispatch (handler) form — load handler from the cache, indirect call through `InlineCacheHandler::offsetOfCallTarget` (`jit/JITInlineCacheGenerator.cpp:105-106`, comment `:124`) — is emitted only for FTL sites under `useHandlerICInFTL`; DFG sites instantiate `RepatchingPropertyInlineCache` (`:94-97`), whose runtime behavior is code repatching | M: AOT code must use the handler-dispatch form only — IC state in a writable table, handler chain published by data write (`publishHandlerChainHead`, `bytecode/PropertyInlineCache.cpp:1149`), code immutable; tool-side lowering must emit that form for DFG-grade bodies too (design §3.1 row 7). Cold ICs go to the generic slow path instead of runtime repatching. |
| 8 | **Jump replacements / patchpoints**: invalidation points overwritten on jettison | `dfg/DFGCommonData.h:133` `m_jumpReplacements` | M/V: with immutable code, invalidation flips a per-CodeBlock "valid" bit checked at entries/loop headers (cheap load+branch) instead of code patching; watchpoint fire ⇒ bit clear ⇒ exits funnel to LLInt. Design doc must cost this (~1 load/branch per invalidation point). |
| 9 | **Wasm cross-function calls** | `UnlinkedWasmToWasmCall` (`wasm/WasmBBQPlan.cpp:94`), patched at `wasm/WasmCalleeGroup.cpp:323,435` | R: already a relocation list; serialize it as-is. |

| 10 | **Switch jump tables** [added r5/C31]: side-table ABSOLUTE code addresses, not instruction-byte immediates — they escape this catalog's framing | `dfg/DFGJITCode.h:312-313` (`FixedVector<SimpleJumpTable>`, `FixedVector<StringJumpTable>`); entries `CodeLocationLabel<JSSwitchPtrTag>` (`bytecode/JumpTable.h:44,66-68`), populated at LinkBuffer finalization | M/R: serialize per-function (case → function-relative code offset) arrays; rebase at load (BUNDLE) or emit data-section absolute relocations (LINKED). StringJumpTable keys on `StringImpl*` ⇒ atom table (category 5). |
| 11 | **MathIC arithmetic ICs** [added r5/C29]: DFG/FTL untyped arith bakes `TrustedImmPtr(mathIC)` — a compile-VM C++ heap pointer, not a JSCell — and the `*Optimize` slow path regenerates code at runtime | `dfg/DFGSpeculativeJIT.cpp:4891` (addJITAddIC), `:5068,5742` (`TrustedImmPtr(mathIC)`); `ftl/FTLLowerDFGToB3.cpp:2926-3116` (compileBinary/UnaryMathIC); `JITMathIC::generateOutOfLine` = LinkBuffer codegen (`jit/JITMathIC.h:127`) | Disable for AOT: the non-repatching operation path already exists (`dfg/DFGSpeculativeJIT.cpp:5097` `nonRepatchingFunction`); drop the `*Optimize` family from kept operations. |
| 12 | **Per-VM mutable fast-path state via `AbsoluteAddress`** [added r6/C36]: raw tool-VM addresses of GC-phase-mutable state baked as instruction operands in MAIN-BODY DFG/AssemblyHelpers emission — a constructor none of the typed choke points sees | barrier threshold `jit/AssemblyHelpers.h:2088,2095` (every barriered store); mutator fencing `:2159`; soft stack limit `:195-205` non-VMLite arm (every prologue); trap bits `dfg/DFGSpeculativeJIT.cpp:2581` (back-edge safepoint poll); `&vm().didEnterVM` `:2348,2388` (validateDFGClobberize-only) | M: offset loads off the table-loaded VM pointer (category 3 slot) — values mutate during concurrent GC, so never snapshot; validation-only sites disabled tool-side. Failure mode if missed is SILENT (skipped barriers, missed termination), not exit-class — design Annex I.2. |
| 13 | **FTL LazySlowPath** [added r6/C35]: ~28 patchpoints (object/activation/butterfly allocation slow cases) jump to a generation thunk; FIRST HIT runs CCallHelpers+LinkBuffer+FINALIZE_CODE at runtime | `ftl/FTLLowerDFGToB3.cpp:9983,10060,10435,11935,19916-19925,21232-21239,23856,24882` (sites), `:25352` (thunk jump), `:25361-25375` (generator lambdas constructed at compile time); `ftl/FTLOperations.cpp:958`; `ftl/FTLLazySlowPath.cpp:57-72` | Run generators EAGERLY at tool time (only `->run()` is deferred in-tree); stubs into CODE, jump resolved offline; drop `operationCompileFTLLazySlowPath` + thunk from kept set — design I.1. |

Categories 1, 2, 7 are the substance of the design problem; 3-6, 9 are mechanical; 8 is a
policy choice. Note the unifying fact: for every category, the *upper tiers already
maintain a side list of what they baked* (DesiredX / CommonData / UnlinkedWasmToWasmCall)
because concurrent compilation forced them to. AOT serialization piggybacks on lists
that exist.

---

## 3. Size ground truth

### 3.1 Raw artifacts (RelWithDebInfo, x86-64 Linux; debug info dominates file sizes)

| Artifact | On-disk | Relevant content |
|---|---|---|
| `WebKitBuild/Release/lib/libJavaScriptCore.a` | 603 MB | mostly DWARF |
| `WebKitBuild/Release/lib/libWTF.a` | 48 MB | mostly DWARF |
| `WebKitBuild/Release/lib/libbmalloc.a` | 7.8 MB | mostly DWARF |
| `WebKitBuild/Release/bin/jsc` (static, unstripped) | 361 MB | see sections below |

`jsc` ELF sections (readelf): **`.text` 23.62 MB**, **`.rodata` 31.37 MB**,
`.eh_frame` 2.24 MB, debug sections ≈ 294 MB (gone when stripped). The shipping-size
question is `.text + .rodata + .data/.eh_frame` ≈ **57 MB unstripped-of-features**.

### 3.2 `.text` by subsystem (nm symbol-size bucketing of `bin/jsc`, 49,898 text symbols, sum 23.72 MB ≈ section size)

| Subsystem | MB | Keep in AOT product runtime? |
|---|---:|---|
| JSC runtime + heap + parser + bytecompiler (`JSC::` residue) | 9.49 | KEEP (parser/bytecompiler needed for eval/Function; could be lazy-linked but full semantics says keep) |
| DFG (`JSC::DFG::`) | 3.37 | DROP (offline only). Exception: OSR-exit *data* readers, small |
| Wasm (`JSC::Wasm::` + ipint) | 2.05 | MOSTLY DROP; keep IPInt (~0.10) + runtime glue if wasm support required |
| B3 + Air (`JSC::B3::`) | 1.72 | DROP (offline only) |
| ICU code (statically linked icu_75 functions) | 1.11 | KEEP (Intl) — see ICU note |
| FTL (`JSC::FTL::`) | 0.99 | DROP |
| Baseline JIT + IC compiler + `operation*` | 0.94 | SPLIT: drop JIT/IC *compilers*; keep `operation*` slow paths (~0.36) |
| WTF + bmalloc + libpas (out-of-line; much WTF inlines into JSC syms) | 0.90 | KEEP |
| Assembler + disassembler (MacroAssembler, Zydis) | 0.73 | DROP (no runtime codegen; Zydis is dev-only already) |
| Yarr (`JSC::Yarr::`) | 0.56 | KEEP interpreter, drop YarrJIT (~half) |
| Inspector + profiling (`Inspector::`, sampling profiler, heap snapshot) | 0.51 | DROP for min product (Bun may want it; budget as optional) |
| LLInt asm blob + slow paths | 0.33 + 0.38 blob | KEEP — this is the fallback tier |
| jsc shell + misc + libstdc++ residue | ~1.0 | partly drop (shell), partly keep |

### 3.3 `.rodata`: the elephant is ICU data

`icudt75_dat` = **30.73 MB** of the 31.37 MB `.rodata` (nm, `R` symbols). Everything
else is small: `JSC::s_JSCCombinedCode` (all builtin JS source, kept as text and
compiled at runtime) = **152 KB**; Yarr/ICU property tables ≈ 0.3 MB; options metadata
22 KB.

**ICU note:** the <10 MB product budget is only meaningful *excluding* ICU data, on two
grounds: (1) Bun already ships ICU for its runtime regardless of this project, so AOT
adds zero marginal ICU bytes; (2) if a standalone budget is demanded, ICU offers
`--with-data-packaging` / data filtering ("small-icu" as used by Node) bringing data to
~6-10 MB for English-only, or Intl can be configured out. The design doc's budget table
must state which stance it takes.

### 3.4 Honest jitless-runtime estimate

Starting at 23.72 MB `.text` and dropping per the table (DFG 3.37, B3/Air 1.72, FTL 0.99,
baseline/IC compilers ~0.58, assembler/disasm 0.73, YarrJIT ~0.28, wasm JITs ~1.7
keeping IPInt+glue, inspector 0.51, shell ~0.3) ⇒ **≈ 13.5 MB of -O2 text remains**, plus
~0.7 MB non-ICU rodata, ~2 MB eh_frame/data. That is *over* 10 MB before toolchain work,
within it after: this is -O2 without `-Os`, without LTO/`--gc-sections` (a static
`jsc` currently links e.g. all four MacroAssembler backends — RISCV/ARMv7 objects are in
the build), and with `functionJSCOptions` alone at 149 KB and ~22 KB options metadata
showing how much dev-only surface is linked. Public precedent (Apple's jitless
"mini-mode" JSC, V8-lite) and the bucket math say a **6-9 MB product runtime excluding
ICU data** is the defensible estimate; **<10 MB is feasible but only with
ICU-data-excluded accounting and a -Os/LTO/dead-strip shipping configuration, and it has
no room for keeping DFG/FTL in-process** — the offline compiler must genuinely be a
separate full-fat binary. The AOT *image* (precompiled code) is additional per-app
payload, not runtime budget.

The design doc's budget table should reproduce §3.2 with a KEEP column summing to the
claimed product size, and must carry these caveats: measured at -O2 (not -Os), x86-64
(arm64 text typically ~10-15% larger), and the kept "JSC runtime" bucket includes some
JIT-conditional code that `#if !ENABLE(JIT)` would remove (estimate, not measured).

### 3.5 Measured per-function compiled-code sizes — grounding bytes-per-variant

Methodology: ran the existing `WebKitBuild/Release/bin/jsc` (no rebuild) with
`JSC_logJIT=1` over eight JetStream3/Octane workloads (richards, deltablue, crypto,
raytrace, splay, earley-boyer, navier-stokes ×25 iterations; typescript ×4 — the
"big real program": 1.2 MB source, ~1,800 function definitions). `LinkBuffer`
prints exact finalized size per compilation (`assembler/LinkBuffer.cpp:169`).
x86-64; arm64 typically ~10-15% larger.

**Aggregate across all 8 workloads** (per-function machine-code bytes; "bc" =
bytecode instructions in the CodeBlock, parsed from the same log lines):

| Tier | n compiled | total | median | mean | p90 | max | bytes/bc |
|---|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 846 | 4,192,896 | 2,048 | 4,956 | 10,240 | 235,008 | 14.6 |
| DFG | 906 | 2,641,088 | 1,168 | 2,915 | 6,400 | 51,712 | 6.5 |
| FTL (B3) | 338 | 906,528 | 1,536 | 2,682 | 5,888 | 39,680 | 7.9 |
| OSR-exit ramps (lazily compiled, §1.5) | 407 | 555,232 | 1,168 | 1,364 | 3,072 | 6,912 | — |
| Thunk pool (per process, one-time) | 164-373 | 23-35 KB | 96 | — | — | 448 | — |

Per-workload FTL medians range 0.7 KB (deltablue) to 5.5 KB (splay); the typescript
run (the most app-like) compiled 568 functions to baseline, 637 to DFG, 220 to FTL —
i.e. **the FTL-hot set of a large real program is a few hundred functions, ~12% of its
~1,800 defined functions**, totaling just 672 KB of FTL text (DFG 2.2 MB, exits 502 KB).

Cost-model consequences the design doc must use:

1. **Bytes-per-variant ≈ 1.5 KB median / 2.7 KB mean (FTL), 1.2/2.9 KB (DFG).**
   A speculation-lattice artifact shipping V variants per hot function costs roughly
   `V × 2.7 KB × hotCount` before dedup/compression; e.g. typescript-scale
   (220 FTL-hot) at V=3 ≈ 1.8 MB — artifact-size, not runtime-binary-size.
2. **Per-bytecode density is the stable predictor**: ~6.5-8 B/bc for optimized tiers
   across all workloads (range 4.2-29); estimate variant size from bytecode length,
   which the offline compiler knows exactly.
3. **Baseline is not the cheap tier in bytes** (14.6 B/bc, ~2x DFG/FTL density): a
   "ship baseline-shaped generic code" fallback strategy costs *more* image bytes per
   function than shipping DFG-shaped generic (UntypedUse) code. Generic-but-direct
   ARCH-S code should be DFG-pipeline output, not baseline-template output.
4. **Exit ramps are material**: lazily-compiled ramps (only exits actually *hit*)
   already total 61% of FTL text bytes on these runs at ~1.2 KB median each.
   Precompiling a ramp per static exit *site* (sites vastly outnumber hit exits)
   would multiply this; the survey's options (§1.5) of deduped ramps or a C++
   recovery-stream interpreter are the only size-sane choices — this is now a
   measured argument, not a hunch.
5. **The shared-stub/thunk catalog is tiny** (~25-35 KB/process covers every thunk a
   full 4-tier run needed): shipping all shared IC stub bodies + thunks precompiled
   (§1.10 gap) is noise in the size budget.

---

## 4. Prior art (public only) — what each forked/subset/gave up, what's stealable

**Hermes / Static Hermes (Meta).** Hermes AOT-compiles JS to bytecode for fast startup
(no source parse at runtime); Static Hermes compiles *typed* JS (Flow/TS annotations,
soundness-checked subset) to native code, with an interpreter fallback for untyped code.
Gives up: full dynamic semantics in the native path — types are load-bearing, and classic
Hermes drops `with`/some `eval` forms entirely. Stealable without forking syntax: the
*shape* of "native code where provable, interpreter everywhere else, in one image", and
bytecode-in-image as the universal level-0 (our §1.2 equivalent). Their lesson: the
fallback tier must be a first-class citizen, not an apology.

**Porffor.** AOT JS→native (via C/wasm) compiler that intentionally supports a growing
*subset* — no full `eval`, semantics gaps accepted as known-failing test262. Gives up:
ECMAScript completeness as a temporary-but-structural matter; types are inferred and
specialized statically with bailouts limited. Stealable: evidence that whole-program
static analysis of real JS specializes far more than folklore suggests; test262-coverage-%
as the honesty metric an AOT JSC should also publish (ours must be 100% by
construction since LLInt backstops).

**Moddable XS preload.** XS runs the module graph at *build* time on the build machine,
then snapshots the resulting heap (closures, prototypes, structures) into ROM; runtime
aliases ROM objects copy-on-write. No syntax fork; gives up: preload-phase code must be
side-effect-confined (no host I/O at preload), and the snapshot is engine-version-locked.
Stealable: heap snapshotting as the answer to §2's category-1/2 problem — if the
*startup* heap is built offline, structure recipes and frozen values resolve against a
deterministic snapshot instead of a dynamic heap, collapsing most load-time validation.
This composes with, rather than replaces, our watchpoint revalidation.

**GraalVM Native Image (closed-world Java).** AOT under a closed-world assumption:
all classes known at build; reflection/dynamic loading must be declared in config; heap
built at build time via image-heap snapshotting. Gives up: openness — exactly what
constraint 1 forbids us to give up. Stealable: the *image heap* + "build-time
initialization" discipline, and the precedent that deopt-to-interpreter can be the only
escape hatch (Native Image's runtime has no compiler unless you opt into JIT); also their
honest split of "hosted" (build-time) vs "runtime" code, which maps to our offline-jsc vs
product-runtime split.

**Manuel Serrano — Hop/Hopc (Scheme-pedigree AOT JS).** Hopc compiles full
(ES5-era) JavaScript ahead of time to native code via Scheme/Bigloo, no type
annotations, using occurrence typing + hidden-class-style static analysis; `eval`
supported via an embedded interpreter. Gives up: peak performance on megamorphic dynamic
code (no runtime recompilation) and modern-spec completeness lagged. Stealable: the
proof that *annotation-free* AOT JS with an interpreter for `eval` is viable and can
reach within ~1.5-2x of JITs on much code; and the analysis insight that most property
accesses are statically monomorphic given whole-program view — the same lever our
primary (no-profile) mode pulls: 2026 JS apps ship as bundles, so the offline compiler
sees the whole program (minus eval/dynamic import), which is the JS analog of Java
class-hierarchy analysis. Hopc is the existence proof that annotation-free, profile-free
AOT JS works.

**iOS JIT-less JSC deployments.** Every non-Safari iOS app embedding JSC (and lockdown
mode) runs LLInt-only today: full semantics, zero executable-memory allocation —
this tree's `useJIT=0` path (§1.1). Gives up: throughput (~2-6x slower than baseline+,
workload-dependent per public benchmarks). Stealable: it *is* our product-runtime
baseline; the AOT image is precisely "give the jitless configuration back its upper
tiers, compiled elsewhere". Also proves the security story regulators/platforms accept:
W^X with no runtime codegen.

**V8 snapshots / custom startup snapshots.** V8 serializes the heap after running
embedder script at build time (startup + context snapshots, code cache for bytecode).
Sparkplug/Maglev/TurboFan code is *not* shipped in snapshots (only bytecode + a fixed set
of builtins compiled into the binary); custom snapshots forbid certain dynamic state
(no open handles). Gives up: nothing semantically — it is startup-only, which is why V8
still JITs at runtime for peak performance. Stealable: snapshot-blob versioning/checksum
discipline (engine build hash must match — our `isUpToDate` analog, §1.2), the
embedded-builtins precedent (V8 moved all builtin code into the *binary* `.text`,
PC-relative, instance-shared — the same instance-independence discipline as §1.7), and
the explicit lesson that bytecode caching alone leaves 10-100x on the table for compute,
motivating shipping optimized code, which none of V8's mechanisms do — that is the gap
this project fills.

---

## 5. Summary: what the design doc inherits

1. **Fallback tier exists and is shipped** (§1.1): jitless LLInt, full semantics,
   `ExecutableAllocator` absent. Nothing about constraint 1 (no syntax fork) is at risk;
   the design only decides *how much* gets accelerated.
2. **Level-0 serialization exists** (§1.2): CachedBytecode covers unlinked bytecode
   end-to-end with versioned validation; reuse, don't reinvent.
3. **The pipeline has a natural object-emission seam** (§1.3): everything is link
   records until `LinkBuffer::copyCompactAndLinkCode` (`assembler/LinkBuffer.cpp:290`);
   wasm already maintains explicit unlinked-call relocation lists (§1.8).
4. **Exit-to-LLInt is a tested mode** (`forceOSRExitToLLInt`, §1.5); the only new work is
   moving exit-ramp generation offline or interpreting recoveries.
5. **Every baked constant already has a side-list** (§2): DesiredWatchpoints /
   DesiredWeakReferences / DesiredIdentifiers / CommonData are the serialization schema
   in embryo; structure IDs are already 32-bit table indices.
6. **Instance-independent machine code is already shipped for one tier**
   (shared baseline + `BaselineJITData`, §1.7): the data-IC/data-table discipline is the
   template for AOT'd DFG/FTL output.
7. **Static-only compilation is viable in-pipeline** (§1.9): UntypedUse generic
   lowering, load-validatable universal watchpoints, and bytecode-level shape-inference
   plumbing (`create_this`/`new_object` inlineCapacity operands) mean no training run is
   required for the pipeline to emit direct code; profiles are an optional upgrade only.
8. **Size**: <10 MB product runtime is feasible **iff** ICU data is excluded from the
   budget (Bun ships it anyway / small-icu), the compilers are offline-only, and the
   shipping build is -Os/LTO/dead-stripped. Best estimate 6-9 MB text+data ex-ICU-data
   (§3.4). The budget table in the design doc must be derived from §3.2.
9. **"IC as data + patchable constants" is mostly shipped already** (§1.10):
   handler records carry every speculation constant as data
   (`InlineCacheHandler.h:130-143`), rebinding is one published pointer/word write
   (`PropertyInlineCache.cpp:1149`), pre-compiled shared stub bodies exist
   (`InlineCacheCompiler.cpp:7475,7553`); the only gap is that stub bodies are
   currently JIT-emitted (`#if ENABLE(JIT)`), and they are tiny to ship precompiled.
10. **A jitless runtime is already a profiler** (§1.11): value/array/arith profiles,
   structure-resolved LLInt property caches, callee caches, and tier-up counters all
   tick with `useJIT=0` at zero marginal cost — the ARCH-L selection signal is free.
11. **Bytes-per-variant is measured** (§3.5): FTL ≈ 1.5 KB median / 2.7 KB mean per
   function, ~6.5-8 B per bytecode instruction; large-app FTL-hot set ≈ low hundreds
   of functions; exit-ramp bytes rival function bytes (61% of FTL text), forcing
   deduped-ramp or interpreted-recovery exits; baseline-density code is the *worst*
   bytes-per-function choice for shipped generic code.

Open questions the survey *cannot* settle (design decisions, to be recorded with
rationale in AOT-DESIGN.md): exit-ramp strategy (precompiled vs interpreted recoveries);
invalidation via valid-bit vs load-time-only watchpoints; whether to adopt
XS/Graal-style build-time heap snapshotting for category-1/2 constants or pure load-time
materialization; how far static shape/call-graph inference (§1.9) can drive speculation
before falling back to generic-but-direct code (note: the PRIMARY mode is static-only —
no training runs; an *optional* CI-test-run profile feed is appendix-grade at most);
arm64e PAC signing of image code at load.
