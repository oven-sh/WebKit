# INTEGRATE-jit — shared-file needs + cross-spec dispositions (SPEC-jit §10)

Integrator-applied manifest for the jit workstream. Implementers do NOT edit the
target files listed here; every hunk below is ready-to-paste. Updated per task
(Tasks 1, 1b, 2–13 each have a section below, in order). **HANDOFF COMPLETE:
the final consolidated M1–M6 + CS1–CS6 record is the "Task 14 — Manifest
handoff (FINAL)" section at the END of this file; on any divergence it
supersedes the per-task sections, which remain as rationale/history.**

Status legend: [REQUIRED-NOW] = needed for the tree to link once Task 1 is in;
[PREP] = SPEC-jit §10 prep precondition observed missing from the shared tree;
[DEFERRED] = integration-time (M4/CS2/CS6), not needed yet.

---

## M3 — Sources.txt entries [REQUIRED-NOW]

Task 1 adds three new TUs. Without these entries the tree fails to LINK (the
flag-gated asserts in CodeBlock.cpp / DFGCommonData.cpp / DFGJumpReplacement.cpp /
PropertyInlineCache.cpp / CallLinkInfo.cpp reference `JSThreadsSafepoint::*`
symbols unconditionally; the guard is runtime, not compile-time).

File: `Source/JavaScriptCore/Sources.txt`

1. Insert after the line `bytecode/JumpTable.cpp` (keeping the bytecode/ block's
   alphabetical order; currently line 272):

```
bytecode/JSThreadsSafepoint.cpp
```

2. Insert after the line `bytecode/RecordedStatuses.cpp` (currently line 291;
   before `bytecode/ReduceWhitespace.cpp`):

```
bytecode/RetiredJITArtifacts.cpp
```

3. Insert after the line `jit/CallFrameShuffler64.cpp` (currently line 649;
   before `jit/ExecutableAllocationFuzz.cpp`):

```
jit/ConcurrentButterflyOperations.cpp
```

`CMakeLists.txt`: NO change needed — JavaScriptCore's CMake consumes Sources.txt
for these directories (the explicit runtime/ list in CMakeLists.txt is
`JavaScriptCore_OBJECT_LUT_SOURCES`, lookup-table generation only). If a port
file enumerates bytecode/jit TUs separately, mirror the three entries there.

---

## M1 — runtime/OptionsList.h remainder [PREP]

Observed in-tree: `useJSThreads` (line 681) and the `useThreads` alias (line 685)
are present. The remaining M1 entries are NOT yet present and are prerequisites
for Tasks 2/8-13 (not for Task 1). Insert alongside `useJSThreads`:

```
    v(Bool, useThreadedLLIntICs, true, Normal, "kill switch: threaded LLInt metadata caches under useJSThreads"_s) \
    v(Bool, useThreadedBaselineICs, true, Normal, "kill switch: threaded Baseline/stub ICs under useJSThreads"_s) \
    v(Bool, useThreadedDFG, true, Normal, "kill switch: threaded DFG support under useJSThreads"_s) \
    v(Bool, useThreadedFTL, true, Normal, "kill switch: threaded FTL support under useJSThreads"_s) \
    v(Bool, validateButterflyTagDiscipline, false, Normal, "validate that every generated butterfly access masks or proves the tag (SPEC-jit I14)"_s) \
    v(Bool, useJSThreadsUnlockHandlerICInFTL, false, Normal, "unlock the FTL handler-IC force-disable for bring-up (SPEC-jit M2a)"_s) \
```

## M2 — runtime/Options.cpp [PREP M2a; DEFERRED M2b]

M2a (prep precondition, needed before Task 2 can be smoke-tested): gate the
force-disable at `runtime/Options.cpp:814`. Replace:

```cpp
    Options::useHandlerICInFTL() = false; // Currently, it is not completed. Disable forcefully.
```

with:

```cpp
    if (!Options::useJSThreadsUnlockHandlerICInFTL())
        Options::useHandlerICInFTL() = false; // Currently, it is not completed. Disable forcefully.
```

M2b (handoff, applied at Task 14): in the same finalization function, after the
M2a hunk:

```cpp
    if (Options::useJSThreads()) {
        Options::useHandlerICInFTL() = true;
        Options::usePollingTraps() = true; // SPEC-jit I21: cooperative polls only; async breakpoint patching = I2 violation.
    }
```

plus a startup error if `useJSThreads && !useHandlerICInFTL`.

## M2c — runtime/Options.cpp main-thread P5 init [PREP, REQUIRED for EVERY flag-on JIT leg — review round 2, R2-6]

**ALL PLATFORMS, prep-phase (apply with M1/M2a, NOT at M2b).** Review round 2
confirmed `initializeButterflyTIDTagForCurrentThread()` has ZERO call sites in
the tree: the api workstream's spawn-path diff (its 9.2-8) is unapplied and in
any case covers spawned threads only, never the main thread. Meanwhile
`CCallHelpers::loadButterflyTIDTag` (jit/CCallHelpers.cpp) is emitted flag-on
by every Baseline/DFG/FTL property-write leg and calls
`butterflyTIDTagELFTLSOffset()` (Linux) / `butterflyTIDTagTLSKey()` (Darwin),
both of which RELEASE_ASSERT that P5 init has already run. The earlier framing
of this requirement as a Darwin-only Config::finalize ordering note (M4a below)
was wrong on classification: on ELF/Linux the TLS-offset computation has
exactly the same first-call requirement, and the flag-on JIT legs run at
apply-order step 2 — without this hunk, any `--useJSThreads=1` run that
JIT-compiles a property write RELEASE_ASSERTs at first emission. Secondary
effect of the missing call: the CS3 `setVMLiteTIDTagHook` registration lives
inside the first init call, so until something calls init,
`g_jscButterflyTIDTag` silently goes stale across VM-lite switches (the I19
incoherence the hook exists to prevent).

File: `Source/JavaScriptCore/runtime/Options.cpp` — in the options
finalization function (same function as the M2a hunk), AFTER option values are
final and BEFORE `Config::finalize()` runs (on Darwin this ordering is
load-bearing for the M4a config-slot store; on Linux only the
before-first-JIT-emission ordering matters):

```cpp
    if (Options::useJSThreads()) {
        // SPEC-jit P5/App. R5 (review round 2, R2-6): the main thread must run
        // butterfly-TID-tag init before any flag-on JIT leg emits
        // loadButterflyTIDTag (both per-platform offset/key queries
        // RELEASE_ASSERT prior init), and before Config::finalize on Darwin
        // (the pthread key is mirrored into g_jscConfig). Idempotent;
        // main-thread tag is 0. Spawned threads run the same call on their
        // spawn path (api manifest 9.2-8).
        JSC::initializeButterflyTIDTagForCurrentThread();
    }
```

with `#include "ConcurrentButterflyOperations.h"` added to Options.cpp's
include block. (If the integrator prefers `jscFinalizeOptions` /
`initializeThreading`, any site works that satisfies: options final, before
Config::finalize, before first JIT emission, on the main thread.)

## M4a — runtime/JSCConfig.h gate byte + Darwin TLS-key slot [PREP]

Not yet present in-tree. Needed BEFORE Tasks 6/8 (LLInt gate branch shape is
frozen by Task-13 golden diffs; no interim substitute permitted). Add to the
`JSC::Config` POD (next to the existing option mirror fields):

```cpp
    uint8_t useJSThreads; // mirrored from Options::useJSThreads() at options-finalize (SPEC-jit M4a/§5.4)
    uint32_t butterflyTIDTagTLSKey; // Darwin only: pthread key for g_jscButterflyTIDTag (SPEC-jit-annex App. R5)
#define JSC_CONFIG_HAS_BUTTERFLY_TID_TAG_TLS_KEY 1 // consumed by jit/ConcurrentButterflyOperations.cpp (Task 1b)
```

(The `#define` is load-bearing: Task 1b's
`jit/ConcurrentButterflyOperations.cpp` stores the Darwin pthread key into
`g_jscConfig.butterflyTIDTagTLSKey` ONLY when that macro is defined, so the
owned TU compiles unchanged before and after this hunk lands.)

and at options-finalize (where other config mirrors are stored, before
`Config::finalize`): `g_jscConfig.useJSThreads = Options::useJSThreads();`

**Darwin ordering (Task 1b; RE-CLASSIFIED by review round 2 — see M2c):** the
key creation + config store run inside the first
`JSC::initializeButterflyTIDTagForCurrentThread()` call. On Darwin the
integrator MUST ensure that first call happens before `Config::finalize`
freezes the page. The M2c hunk above IS that call — apply it with M1/M2a, all
platforms. (The original text here said "On ELF/Linux there is no config
store, so no ordering constraint" — true for the CONFIG ordering, but wrong as
a classification: ELF has the same first-call requirement via
`butterflyTIDTagELFTLSOffset()`'s RELEASE_ASSERT. M2c supersedes.)

## M4 rest — runtime/VMManager.h/.cpp [DEFERRED]

Exactly R1.a-d per SPEC-jit §9.2 (stop reason `v(JSThreads)`,
`JSC_CONFIG_METHOD(jsThreadsStopTheWorld)` slot + `VMManager::setJSThreadsCallback`,
`requestStopAllWithConductor`, resume-path ISB after JSThreads/GC stops).
INTEGRATION-DEFERRED (OM manifest 6 concurs): Tasks 5/11/13 land against the
Task-1 interim stub in `bytecode/JSThreadsSafepoint.cpp`. At M4, replace the stub
body of `stopTheWorldAndRun` per the comment block inside it (R1.a-i sequence,
GCL bracket = `Heap::JSThreadsStopScope`, CS2) and delete `s_stubWorldStoppedDepth`
plus the §5.6 disjunct-4 witness read.

## M5 — runtime/VM.h: none (epoch state is heap-owned, §4.4)

## M6 — runtime/** deferred-fire conversions: empty at Task 1 (populated, if at all, by Task 11's audit)

---

## Cross-spec dispositions touched by Task 1

* **CS6 / §5.6 disjunct 4 (OM stub witness).** `JSThreadsSafepoint.cpp` reads
  `g_jsThreadsStubWorldStopped` ONLY when `__has_include("ConcurrentButterfly.h")`
  AND that header defines `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS` next to the
  witness declaration. Integrator action when OM lands: EITHER have
  ConcurrentButterfly.h declare the witness + that macro, OR (preferred, CS6)
  have OM's §10.6 veneer (`jsThreadsStopTheWorldAndRun`) delegate to
  `JSThreadsSafepoint::stopTheWorldAndRun`, which makes the disjunct redundant
  (the jit stub's own depth counter then witnesses OM stop windows). Record the
  M4-time deletion of the disjunct here.
* **CS3 / R5 / P5.** `jit/ConcurrentButterflyOperations.cpp` registers
  `setVMLiteTIDTagHook` only when `__has_include("VMLite.h")` (vmstate §6.7).
  Until vmstate W3 lands, an owned interim `currentButterflyTID()` shim returns 0
  (main-thread-only; same pattern as OM §9.1). Swap = delete the shim; nothing
  else changes.
* **N6 ordering (SPEC-jit §2).** `bytecode/RetiredJITArtifacts.cpp` compiles its
  real bodies iff `__has_include("GCSafepointEpoch.h")` (heap §11); otherwise it
  is a no-op leak-until-integration stub (sound: GIL stub ⇒ no concurrent
  retirement). HEAP MUST LAND BEFORE jit TASK 13's epoch tests run
  (retire→safepoint→free ordering, retire→legacy-GC→free variant).
* **Shared-heap disjunct.** `JSThreadsSafepoint::worldIsStopped(VM&)` consults
  `vm.heap.worldIsStoppedForAllClients()` gated on
  `__has_include("HeapClientSet.h")` (proxy for "heap workstream landed",
  SPEC-heap F7). If heap lands the header without that Heap method in the same
  change, fix the gate here rather than in heap files.

## Task-1b — I19 VM-entry assert wiring [DEFERRED until useJSThreads is functional; runtime/** hunk]

SPEC-jit I19 requires a VM-entry debug RELEASE_ASSERT that
`g_jscButterflyTIDTag == uint64_t(currentButterflyTID()) << 48`. The check
itself is the owned export `JSC::assertButterflyTIDTagCoherent()`
(`jit/ConcurrentButterflyOperations.h`); the call site is non-ownable.

File: `Source/JavaScriptCore/runtime/VMEntryScope.cpp`

1. Add to the include block (after `#include "Options.h"`):

```cpp
#include "ConcurrentButterflyOperations.h"
```

2. Insert at the top of `VMEntryScope::setUpSlow()` (immediately after
   `m_vm.entryScope = this;`):

```cpp
#if ASSERT_ENABLED
    // SPEC-jit I19: the per-thread butterfly TID tag must be coherent before
    // any JS runs on this thread (CS3; zero-init is correct only for the
    // main thread).
    if (Options::useJSThreads()) [[unlikely]]
        assertButterflyTIDTagCoherent();
#endif
```

This runs once per top-level VM entry (not per call), so debug-build cost is
negligible. Until vmstate's `VMLite.h` lands, `currentButterflyTID()` is the
owned interim shim returning 0, so the assert is trivially green single-VM.

## Cross-spec notes added by Task 1b

* **R5 emitter surface (additive-only assembler files).** Landed per App. R5:
  `X86Assembler::fs()` (+ `PRE_FS = 0x64`),
  `MacroAssemblerX86_64::loadFromELFTLS64(intptr_t, RegisterID)` (OS(LINUX)),
  `ARM64Assembler::mrs_TPIDR_EL0(RegisterID)` (OS(LINUX)),
  `MacroAssemblerARM64::loadFromELFTLS64(intptr_t, RegisterID)` (OS(LINUX);
  needs the macro scratch register convention, mirrors `loadFromTLS64`).
  Emission-side consumers (Tasks 8-10) take the immediate from
  `JSC::butterflyTIDTagELFTLSOffset()` (ELF) or
  `WTF::fastTLSOffsetForKey(JSC::butterflyTIDTagTLSKey())` (Darwin).
* **LLInt `loadButterflyTIDTag` macro is NOT part of Task 1b** (file list
  excludes `llint/**`); it lands with Task 8 using link-time initial-exec
  relocations (ELF) / the M4a config key (Darwin) per App. R5.
* **CS3 hook timing.** `updateButterflyTIDTag` (the registered
  `setVMLiteTIDTagHook` body) tolerates running before the Darwin key exists
  (guarded), so vmstate's `VMLite::setCurrent` may fire it in any order
  relative to P5 init; the next P5 init re-syncs both copies.
* **D8 enforcement.** On platforms with no JIT-visible TLS mechanism
  (Windows; Linux on CPUs other than x86-64/arm64), P5 init RELEASE_ASSERTs
  `!Options::useJSThreads()` at second-thread startup, per App. R5.

## Task-2 — FTL handler ICs completed (D1, §5.2) [owned-path; M2a still PREP]

Design of the completion (all changes in owned paths:
`ftl/{FTLLowerDFGToB3.cpp,FTLState.cpp,FTLJITCode.{h,cpp},FTLJITFinalizer.cpp}`,
`jit/JITInlineCacheGenerator.cpp`,
`bytecode/{InlineCacheCompiler.cpp,PropertyInlineCache.{h,cpp}}`):

* Handler ICs are register-convention-fixed: every shared handler stub/thunk
  (`VM::m_sharedJITStubs`, `CommonJITThunkID::*Handler`, the slow-path handler
  thunks) is compiled against the Baseline data-IC registers and reads per-tier
  data via `GPRInfo::jitDataRegister` + `BaselineJITData` offsets. The FTL
  therefore adopts the DFG's protocol at every IC patchpoint when
  `useHandlerICInFTL` is on:
  1. the patchpoint late-clobbers `registersToSaveForJSCall(allScalarRegisters)`
     plus `jitDataRegister` (the FTL analogue of the DFG's `flushRegisters()`),
     pins value-producing results to `returnValueGPR` (= each op's Baseline
     `resultJSR`), and drops the previous gpScratch-based cache register;
  2. the generator shuffles operands into the op's Baseline IC registers
     (`CCallHelpers::shuffleRegisters`), materializes
     `FTL::JITCode::handlerICJITData()` into `jitDataRegister` and the
     `PropertyInlineCache*` into the op's Baseline `propertyCacheGPR`, then
     emits `generateDataICFastPath` instead of `generateFastPath`;
  3. IC `usedRegisters` = `RegisterSet::stubUnavailableRegisters()` exactly as
     Baseline/DFG record, keeping cross-code-block stub sharing sound.
* New `FTL::JITData` (FTLJITCode.h) mirrors the `BaselineJITData`/`DFG::JITData`
  leading layout (static_asserts in FTLJITCode.cpp); filled in by
  `FTL::JITFinalizer::finalize()` after `setJITCode()` (stackPointerOffset needs
  the installed jitType) and before `installCode()`. It also carries a dummy
  `ArrayProfile` (the by-val slow-path handler thunks pass profileGPR to the
  optimize operations; by-val FTL sites point profileGPR at the dummy, exactly
  like the DFG's `JITData::offsetOfDummyArrayProfile()` sites), cleared from
  `CodeBlock::finalizeUnconditionally` (owned bytecode/ file) alongside the
  DFG's.
* Initial handler installation: `HandlerPropertyInlineCache::
  initializeHandlerForOptimizingJIT(CodeBlock*)` (new, PropertyInlineCache.h/cpp)
  installs the shared slow-path handler; called per IC from
  `FTL::JITFinalizer::finalize()` (main thread, pre-installCode).
  `JITInlineCacheGenerator::finalize` now dispatches on the IC kind and records
  doneLocation/slowPathStartLocation for handler ICs (no inline slab).
* Exceptions: with the full volatile clobber, the unwind exit for an FTL
  handler-IC call site must report no live registers; `compileHandler()` bails
  (`GaveUp`) for FTL if that should-be-impossible case arises, keeping the
  pre-existing `ASSERT(!useHandlerIC())` in
  `calculateLiveRegistersForCallAndExceptionHandling` valid.
* `rewireStubAsJumpInAccess` now `RELEASE_ASSERT(!Options::useHandlerICInFTL())`
  — FTL's last in-place code rewrite for property ICs is structurally
  unreachable once the FTL allocates handler ICs exclusively
  (`FTL::State::addPropertyInlineCache` also RELEASE_ASSERTs `!useJSThreads()`
  on the repatching branch, I3/§5.2 acceptance).

Integration notes:

* **M2a is still a PREP precondition and is NOT in the tree** —
  `runtime/Options.cpp:814` still force-disables `useHandlerICInFTL`
  unconditionally. The M2a hunk above must land before any flag-on smoke test.
  Task 2 did NOT add the temporary owned env-var hatch (the spec's slip-hatch is
  optional and no build/test runs were possible in this pass); first validation
  should go through M2a + `--useJSThreadsUnlockHandlerICInFTL=1
  --useHandlerICInFTL=1`.
* M2b at handoff additionally needs the startup error
  `useJSThreads && !useHandlerICInFTL`.
* No new Sources.txt entries: Task 2 touched only existing TUs.
* DFGStrengthReductionPhase.cpp:1758's DirectCall bailout under
  `useHandlerICInFTL` left untouched per §5.2 (DirectCall is Task 7 / §5.8).

## Task-1 notes for later jit tasks (informational, owned-path)

* `GCAwareJITStubRoutine::isGCAware()` accessor added (owned jit/ header) for
  the §4.4 RELEASE_ASSERT.
* I2 asserts wired at: `DFG::CommonData::invalidateLinkedCode`,
  `DFG::JumpReplacement::fire` (VM-less `worldIsStopped()` overload),
  `PropertyInlineCache::rewireStubAsJumpInAccess`,
  `DirectCallLinkInfo::{initialize,setCallTarget non-data branch,repatchSpeculatively}`.
* I3 asserts wired at: `RepatchingPropertyInlineCache` constructors,
  `DirectCallLinkInfo` constructor (UseDataIC::Yes required flag-on).
* I8 assert wired at: `CodeBlock::jettison` (old-age exempt).
* Flag-on is NOT yet functional end-to-end: until Task 11 routes Class-A
  watchpoint fires through `stopTheWorldAndRun`, a flag-on jettison reached from
  a plain fire will (intentionally) trip I8. Fail-fast is the specified behavior.

---

# Task 3 — Handler publish fences + epoch retirement (§5.1/§4.4/§4.5, I5/I9/I15/I17)

**No new shared-file hunks.** Task 3 touched only owned paths; M1–M6 are
unchanged by this task. Sources.txt already carries
`bytecode/RetiredJITArtifacts.cpp` (Task-1 M3 entry above).

## What landed (owned paths, informational)

* **F1/I5 fences** (`bytecode/PropertyInlineCache.cpp`): `WTF::storeStoreFence()`
  between payload init and the publishing `m_handler` store in
  `prependHandler`, `initializeWithUnitHandler` (both branches), and
  `resetStubAsJumpInAccess`; plus a payload-before-field-publish fence in
  `HandlerPropertyInlineCache::setInlinedHandler` (the single-word atomicity of
  the `{byIdSelfOffset, m_inlineAccessBaseStructureID}` pair itself is §4.2 =
  Task 4). Fences are UNCONDITIONAL (C++-side; I1 is about emitted code).
* **I9 rerouting** (flag-gated on `Options::useJSThreads()`):
  - `resetStubAsJumpInAccess` (handler-IC branch): fenced slow-path-head
    publish, then `RetiredJITArtifacts::retireHandlerChain` for the displaced
    chain AND the displaced inlined unit handler; never frees inline.
    `removeOwner()` walk preserved (§4.1).
  - `initializeWithUnitHandler` (megamorphic upgrade): same treatment for the
    displaced head/inlined handler.
  - `PropertyInlineCache::deref()` is now `deref(VM&)` (callers: the two
    CodeBlock.cpp sites — destructor and jettison — and
    `PropertyInlineCache::reset`). Flag-on, a Repatching IC's
    `unique_ptr<PolymorphicAccess>` is routed through
    `RetiredJITArtifacts::retire` (defensive: I3 makes that branch unreachable
    flag-on); flag-off byte-for-byte today's `m_stub.reset()`.
  - Jettison-time handler CHAINS are deliberately left installed (resumed
    mutators may dispatch through them until their next invalidation point,
    I21); they die with the CodeBlock after R2's scan.
* **§4.5 atomic refcounts (UNCONDITIONAL)**:
  - `InlineCacheHandler` (and via inheritance `InlineCacheHandlerWithJSCall`)
    now derives `ThreadSafeRefCounted<InlineCacheHandler>`
    (`bytecode/InlineCacheHandler.h`).
  - `JITStubRoutine::m_refCount` is `std::atomic<unsigned>`: relaxed inc,
    release dec, acquire fence before `observeZeroRefCount()`
    (`jit/JITStubRoutine.h`; covers all subclasses).
* **I15 Ref-ified slow paths** (`jit/JITOperations.cpp`):
  `operationReallocateButterflyAndTransition` — the only native slow path
  found taking a raw `InlineCacheHandler*` from JIT'd code — now takes
  `Ref<const InlineCacheHandler>` before its allocation (= possible safepoint).

## I17 audit table (shared counters/lists reachable through handler chains)

| Datum | Cross-thread mutation path | Guard after Task 3 |
|---|---|---|
| `InlineCacheHandler` refcount | I15 slow-path Refs (any mutator); epoch-expiry deref of `RetiredHandlerChain` (GC conductor); chain `m_next` RefPtr teardown | `ThreadSafeRefCounted` (atomic) |
| `JITStubRoutine::m_refCount` (all subclasses) | node destruction at epoch expiry drops `Ref<PolymorphicAccessJITStubRoutine>`; `vm.m_sharedJITStubs` handler cache | `std::atomic<unsigned>`, release-dec/acquire-pre-zero |
| `InlineCacheHandler::m_next` | set once at prepend, immutable until reset (§4.1); displaced chains immutable post-retire | publish fence (F1) + reader address dependency (F2); no further sync needed |
| Handler payload fields (`m_structureID`, `m_offset`, `u.*`, targets) | none after publish (I4) | frozen at publish; fence F1 |
| owner registration (`addOwner`/`removeOwner` → `GCAwareJITStubRoutine` owner sets) | mutators under `CodeBlock::m_lock` (§5.1 writers unchanged) | stays under `m_lock` (§4.5) |
| `RepatchingPropertyInlineCache::m_stub` (`unique_ptr<PolymorphicAccess>`) | flag-on: none (I3); flag-off: single mutator | unchanged; flag-on defensive retire in `deref(VM&)` |

## Integration checks for other workstreams

1. **heap**: `PropertyInlineCache::reset` → `resetStubAsJumpInAccess` can run
   from `visitWeak`/finalization (legacy GC, world stopped). Flag-on this calls
   `RetiredJITArtifacts::retireHandlerChain` ⇒ the heap's
   `GCSafepointEpoch::retire` leaf lock must be acquirable from that GC-side
   context (heap §11 legacy-end reclamation implies it is; P1 forbids only heap
   ranks 7–9). Flag verify at M4/CS2 integration.
2. **heap (ordering, restated from Task 1)**: `GCSafepointEpoch.h` must land
   before Task-13 epoch tests; until then the N6 shim leaks retired items
   (sound under the GIL stub).

## Caveats / untested (no build allowed this pass)

* `ThreadSafeRefCounted<T>::deref()` deletes via `delete static_cast<const T*>(this)`
  while `InlineCacheHandler` declares a destroying `operator delete`
  (`InlineCacheHandler*, std::destroying_delete_t`). Destroying delete of a
  const-qualified pointer is standard-conformant (the implementation applies
  the const_cast), but if a toolchain objects, the fix is a thin
  `void deref() const` on `InlineCacheHandler` that calls `derefBase()` and
  `delete const_cast<InlineCacheHandler*>(this)`.
* `RELEASE_ASSERT(!m_refCount)` sites (JITStubRoutine.cpp:38,
  GCAwareJITStubRoutine.cpp:69/77) rely on `std::atomic`'s implicit load —
  intentional, no edits there.

# Task 4 — Inlined fast-path repack (§4.2, I6) [owned-path only; NO new shared-file needs]

No new OptionsList/Options/Sources/VM/VMManager hunks. Everything below is
informational for the integrator and for Tasks 9/10/13.

## What landed (owned paths)

* `bytecode/PropertyInlineCache.h`
  - UNCONDITIONAL repack (D7): `{byIdSelfOffset (PropertyOffset),
    m_inlineAccessBaseStructureID (WriteBarrierStructureID)}` are now an
    anonymous `union alignas(8)` with `std::atomic<uint64_t> m_packedSelfWord`.
    Per-field names keep their exact pre-repack offsets (offset at +0, id at
    +4; static_asserts after the class prove it), so flag-off C++ and emitted
    code are unchanged (I1). Ctor zero-inits the word (all-zero = invalid; the
    id half previously zero-initialized via WriteBarrierStructureID's default
    ctor, the offset half was uninitialized-but-unreachable — now both zero).
  - New: `offsetOfPackedInlineAccessSelfWord()`,
    `packedInlineAccessSelfWord(StructureID, PropertyOffset)` (endian-correct),
    `setInlineAccessSelfState(VM&, CodeBlock*, Structure*, PropertyOffset)`,
    `clearInlineAccessSelfState()`.
* `bytecode/PropertyInlineCache.cpp`
  - All writers of the pair (`initGetByIdSelf`/`initPutByIdReplace`/
    `initInByIdSelf`, `HandlerPropertyInlineCache::setInlinedHandler`) route
    through `setInlineAccessSelfState`: flag-off = today's
    `WriteBarrierStructureID::set` + field store; flag-on = build word -> one
    relaxed 64-bit store -> `vm.writeBarrier(codeBlock)` (GC barrier preserved
    per §4.2). All invalidations (`reset`, `addAccessCase` repatching path,
    `clearInlinedHandler`) route through `clearInlineAccessSelfState`:
    flag-off = `.clear()` of the id half as today; flag-on = one all-zero
    64-bit store (ABA-safe, barrier-free).
  - Holder-bearing inlined form (CacheType::GetByIdPrototype, the only
    cacheType that writes `m_inlineHolder`) is DISABLED flag-on:
    `prependHandler` no longer calls `setInlinedHandler` for it under
    `useJSThreads` (the handler is prepended to the chain instead — correct,
    just without the call-site inlined fast path); `setInlinedHandler`
    RELEASE_ASSERTs `!Options::useJSThreads()` in that case and debug-asserts
    holder-free self-access in the packable cases.
* `jit/JITInlineCacheGenerator.cpp` (Baseline + DFG + FTL-handler-IC sites all
  funnel through these emitters; DFG/FTL pass their CCallHelpers)
  - New `emitPackedInlineAccessCheckThreaded` (compiled under
    `USE(JSVALUE64) && CPU(LITTLE_ENDIAN)` only, matching D8's flag-on
    platform support): ONE relaxed 64-bit load of the packed word, id-half
    compare + offset-half use from the SAME load (xor/shift sequence, 2 regs:
    scratch1 ends with the zero-extended offset, the word register is
    clobbered). Flag-off emission is byte-identical to today (I1).
  - `generateGetByIdInlineAccessBaselineDataIC` gained a `scratch2GPR`
    parameter (GetById/GetByIdWithThis namespaces already define it; it may
    alias resultJSR which may alias baseJSR, hence a true scratch is required
    for the word). Flag-on GetByIdSelf uses the single-load form;
    flag-on GetByIdPrototype emits NO inline fast path (chain dispatch only).
  - `generatePutByIdInlineAccessBaselineDataIC` gained a `scratch3GPR`
    parameter (= `BaselineJITRegisters::PutById::scratch2GPR` on JSVALUE64,
    InvalidGPRReg otherwise; dead at the IC site per the "Required for
    HandlerIC" noOverlap asserts). Flag-on uses the single-load form.
  - `generateInByIdInlineAccessBaselineDataIC` unchanged by design: it reads
    only the id half (packed word + 4) and uses no offset — no pair to tear;
    commented at the site.

## Notes for later tasks

* Task 13 (I6 stress): "flip an IC between two structures under readers" — the
  packed word is the only JIT-observable state; the test should hammer
  `setInlineAccessSelfState`/`clearInlineAccessSelfState` against Baseline
  GetByIdSelf/PutByIdReplace readers.
* Tasks 8–10: the inlined fast paths still load butterflies via
  `loadProperty`/`storeProperty`; TID/SW mask+check insertion at those choke
  points is §5.5 work, NOT done here.
* I1 caveat: flag-off instruction sequences are identical; the only layout
  delta is `byIdSelfOffset` now always zero-initialized (was indeterminate
  until first publish, unreachable either way).
* Flag-on on big-endian or JSVALUE32_64: the single-load emitters are compiled
  out; C++ writers still take the packed path, which is endian-correct via
  `packedInlineAccessSelfWord`. Those configs are unsupported flag-on (D8)
  and the flag-off emitters remain in place there.

# Task 5 — Jettison under STW (§5.3): world-stopped gates + F5 barriers + STWR routing (incl. R1.i)

**No new shared-file hunks.** Task 5 touched only owned paths
(`bytecode/CodeBlock.cpp`, `bytecode/JSThreadsSafepoint.{h,cpp}`); M1–M6 are
unchanged. Existing M3 entries already cover `JSThreadsSafepoint.cpp`.

## What landed (owned-path summary for the integrator)

* `bytecode/CodeBlock.cpp` — `CodeBlock::jettison` is now the §5.3 CHOKE
  POINT. The entire former body lives in a by-reference closure
  (`doJettison`); flag-on, every jettison with
  `reason != Profiler::JettisonDueToOldAge` routes through
  `JSThreadsSafepoint::stopTheWorldAndRun(vm, scopedLambda<void()>(doJettison))`.
  Consequences:
  - Reoptimization jettisons (`triggerReoptimizationNow`/OSR-exit paths,
    `operationOptimize`), watchpoint-fire-driven jettisons (pre-Task-11), VM
    traps and debugger jettisons all get STW semantics with NO call-site
    changes anywhere else in the tree.
  - GC-driven jettisons (weak-reference/end-phase finalizers) and Task-11
    fire-closure jettisons enter `stopTheWorldAndRun`'s R1.h already-stopped
    path: inline run, no re-request, witness raised.
  - The I8 RELEASE_ASSERT (`!useJSThreads || reason==OldAge ||
    worldIsStopped(vm)`) stays as the first statement of the body —
    now satisfied by construction.
  - Flag-off: `doJettison()` runs directly; behavior byte-identical (I1).
* `bytecode/JSThreadsSafepoint.cpp` — `stopTheWorldAndRun` upgrades:
  - **R1.h first**: `worldIsStopped(vm)` => run inline, bumping the stub
    depth so the VM-LESS `worldIsStopped()` witness (used by the asserts in
    `DFG::CommonData::invalidateLinkedCode` and `DFG::JumpReplacement::fire`,
    which have no VM in scope) holds across the closure even when the
    "already stopped" evidence is per-heap state. No API-lock/GIL assert on
    this path (GC-end finalizers are covered by the collector's stop).
  - **R1.i bracket LIVE** (heap workstream has landed in this tree:
    `Heap.h` defines `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` and provides
    `Heap::JSThreadsStopScope` per CS2/heap manifest 10b): on the requesting
    path, resolve the SERVER via `vm.clientHeap.server()` (R4-1 — under
    useSharedGCHeap a client VM's `vm.heap` is NOT the shared server; keying
    on it skipped the bracket for every client, the round-4 blocker) and iff
    `server.isSharedServer()`, release THIS client's heap access
    (`GCClient::Heap::releaseHeapAccess` via the local
    `ClientHeapAccessReleaseScope` — server-level `releaseAccess()` forwards
    to the MAIN client, wrong for a non-main requester) THEN hold
    `Heap::JSThreadsStopScope(server)` (rank-2 GCL) across `work`;
    destruction order = spec resume order (unlock GCL, re-acquire access).
    Never calls `bumpAndReclaim` (CS4 refused). Non-shared heap: no bracket
    (R1.i "no-op"; legacy concurrent GC already tolerates patch-with-access
    exactly as tip-of-tree). Gate interplay (R4-1):
    `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` (from Heap.h) and
    `JSC_JIT_HAS_SHARED_HEAP_SERVER` (HeapClientSet.h presence) are
    independent; the former-without-latter fallback keys on `vm.heap`, sound
    because no foreign-client shared server can exist without the client-set
    machinery.
  - **F5 stub barrier**: `WTF::crossModifyingCodeFence()` on the closing edge
    of both paths — the single-mutator stand-in for the per-mutator ISB.

## M4 reminders sharpened by Task 5 (all still INTEGRATION-DEFERRED)

* R1.d: the NVS resume tail must execute the per-mutator ISB when the
  serviced stop included `JSThreads` OR `GC` (F5), BEFORE the heap's
  `gcDidResumeFromStopTheWorld` hook (manifest 5a ordering). At M4 the stub
  `crossModifyingCodeFence()` calls in `JSThreadsSafepoint.cpp` become
  redundant on the requesting path (keep-or-drop is the integrator's call;
  keeping them is harmless).
* At M4, `stopTheWorldAndRun`'s real stop replaces only steps 3/5 of the
  commented sequence; the R1.i bracket and R1.h early-inline branch written
  here are the FINAL shapes and carry over verbatim.
* I21 dependency (recorded for M2b, Task 14): flag-on soundness of the
  jettison choke point assumes `usePollingTraps` is forced true (async
  VMTraps breakpoint patching would violate I2); M2b's hunk already covers
  this.

## Notes for later tasks

* Task 11 (Class-A fires): fire closures may simply call
  `CodeBlock::jettison` from inside their STWR closure; the R1.h path makes
  that a natural nest (§5.6 step 5). No special jettison entry point needed.
* Task 13: the jettison-vs-execute integration-gate stress exercises this
  routing; pre-integration (stub) the flag-on single-thread suite now runs
  jettisons through the closure path — any jettison caller holding a §7 lock
  will deadlock/assert at the STWR boundary by design (that is the audit
  signal, mirroring App. 5.6's watchdog rationale).

# Task 6 — LLInt metadata repack/disable (§4.3/§5.4, I13) [owned-path only; M4a/M1 notes below]

**No new shared-file hunks.** Task 6 touched only owned paths
(`bytecode/{GetByIdMetadata.h,BytecodeList.rb,CodeBlock.cpp,GetByStatus.cpp}`,
`llint/{LLIntSlowPaths.cpp,LowLevelInterpreter64.asm,LowLevelInterpreter32_64.asm}`).
M1–M6 hunks are unchanged, but two existing PREP items have Task-6 consequences:

## M4a disposition (gate byte) — ACTION FOR INTEGRATOR

SPEC-jit §5.4 freezes the LLInt gate as "one `_g_config` byte-load + branch".
Observed in-tree: `JSC::Config` already contains `OptionsStorage options`
(JSCConfig.h:104), i.e. `Options::useJSThreads()`'s backing byte ALREADY lives
inside the frozen config page, and the LLInt already has the load shape
(`loadBoolJSCOption`, LowLevelInterpreter.asm:469-472). Task 6's gate macro
(`ifJSThreadsBranch(scratch, label)`, LowLevelInterpreter64.asm) therefore loads
`JSCConfigOffset + JSC::Config::options + OptionsStorage::useJSThreads` — the
SAME instruction shape (leap + byte-compare-branch) as M4a's dedicated byte,
differing only in the offset immediate.

Integrator choice (record either way):
1. (Recommended) Declare M4a's gate byte SATISFIED by the existing
   `Config::options.useJSThreads` storage; M4a then shrinks to the Darwin
   `butterflyTIDTagTLSKey` slot + the `JSC_CONFIG_HAS_BUTTERFLY_TID_TAG_TLS_KEY`
   define (still needed by Task 1b). No asm change.
2. Land M4a's dedicated `uint8_t useJSThreads` byte as written: then change ONE
   line — the field expression inside `ifJSThreadsBranch` — to
   `JSCConfigOffset + JSC::Config::useJSThreads[scratch]` and re-bless the
   Task-13 golden diffs (instruction shape identical; only the immediate moves).

## M1 reminder (kill switch wiring point)

`useThreadedLLIntICs` is still absent from OptionsList.h (M1 [PREP]). Until it
lands, `useThreadedLLIntPropertyCaches()` (LLIntSlowPaths.cpp, top of file) is
keyed on `Options::useJSThreads()` alone. When M1 lands, change its body to
`Options::useJSThreads() && Options::useThreadedLLIntICs()`. The kill switch is
publication-side only: the asm gate stays keyed on useJSThreads (with the
switch off, the threaded readers see never-published words and always miss —
sound, slow-path-only).

## What landed (owned paths)

* `bytecode/GetByIdMetadata.h`
  - NEW `struct alignas(8) LLIntCachedIdAndOffset { StructureID structureID; int32_t offset; }`
    (§4.3 frozen survivor form) + `clear()` (one all-zero relaxed 64-bit store,
    F3) + `setConcurrently(StructureID, int32_t)` (one-word publish) +
    `encode()` + static_asserts (size 8, align 8, offsets 0/4).
  - Free helpers `clearLLIntIdAndOffsetPairConcurrently(void*)` /
    `publishLLIntIdAndOffsetPairConcurrently(void*, StructureID, int32_t)` for
    pairs embedded in generated metadata (put_by_id replace cache).
  - `GetByIdModeMetadata` (64-bit union form): `defaultModeCacheWord()` +
    `setDefaultModeCacheConcurrently(StructureID, PropertyOffset)`;
    static_asserts pinning word 1 = {structureID@0, cachedOffset@4}, align 8.
  - Flag-aware store discipline: `clearToDefaultModeWithoutCache()` and
    `setArrayLengthMode()` do single-word invalidation + 1-byte relaxed
    mode/hitCount stores under `Options::useJSThreads()`; flag-off bodies
    byte-identical to before. `setUnsetMode`/`setProtoLoadMode` now
    `ASSERT(!Options::useJSThreads())` (I18).
* `bytecode/BytecodeList.rb`
  - `:LLIntCachedIdAndOffset` added to `types`.
  - `try_get_by_id` / `get_by_id_direct` metadata repacked to a single
    `cache: LLIntCachedIdAndOffset` field, and BOTH OPS MOVED into the
    "Alignment: 8" region (before the `# Alignment: 4` comment) —
    `UnlinkedMetadataTable::finalize()` debug-asserts decreasing-alignment
    layout (UnlinkedMetadataTable.cpp:126), so the move is load-bearing.
    CONSEQUENCE: opcode IDs after `get_private_name` shift; everything is
    regenerated, but any serialized bytecode caches are invalidated (CachedTypes
    versioning handles this at build granularity).
* `llint/LLIntSlowPaths.cpp`
  - `useThreadedLLIntPropertyCaches()` / `useUnthreadedLLIntPropertyCaches()`
    gates (see M1 note); per-op I13 static_asserts (OpTryGetById/OpGetByIdDirect
    metadata = one aligned u64; OpPutById {m_oldStructureID,m_offset} word 0,
    align 8).
  - try_get_by_id / get_by_id_direct: clears via `m_cache.clear()`; flag-off
    publish unchanged (locked two-field store); flag-on publish =
    `m_cache.setConcurrently(...)`, no lock, + `vm.writeBarrier(codeBlock)`.
  - performLLIntGetByID: flag-on Default publish via
    `setDefaultModeCacheConcurrently` (word 1, one store); prototype-cache
    branch (`hitCountForLLIntCaching` decrement + `setupGetByIdPrototypeCache`)
    disabled wholesale flag-on; `setupGetByIdPrototypeCache` itself
    `RELEASE_ASSERT(!Options::useJSThreads())` (sole ProtoLoad/Unset installer).
  - put_by_id: flag-on transition cache disabled (never published; fields null
    forever); replace cache cleared/published as ONE u64 via the pair helpers;
    flag-off byte-identical.
  - get_private_name / put_private_name / set_private_brand /
    check_private_brand: caching disabled flag-on
    (`useUnthreadedLLIntPropertyCaches()` added to the outer gate).
* `llint/LowLevelInterpreter64.asm`
  - NEW `ifJSThreadsBranch(scratch, label)` (§5.4 gate; see M4a note).
  - Threaded single-load fast paths (gated, ONE gate branch per fast path):
    op_try_get_by_id, op_get_by_id_direct, performGetByIDHelper (covers
    op_get_by_id, op_get_length, op_instanceof ×2, op_iterator_open getNext,
    op_iterator_next getDone/getValue), op_put_by_id (replace-only; the
    flag-off transition branch is unreachable flag-on). Pattern:
    `loadq` the word → 32-bit `bineq` on the id half → `rshiftq 32` for the
    offset half → property access. ArrayLength reuses the flag-off block
    (never touches word 1; self-validates via the indexing byte).
  - Field renames m_structureID/m_offset → m_cache.structureID/m_cache.offset.
  - Flag-off delta vs today: exactly one not-taken `leap+bbneq` per affected
    fast path (the §5.4 gate) + field-offset immediates moved by the repack
    (I1 carve-outs; Task-13 golden diffs must bless both).
* `llint/LowLevelInterpreter32_64.asm` — field renames only (no gate; flag-on
  unsupported on 32-bit, D8).
* `bytecode/CodeBlock.cpp` (finalizeLLIntInlineCaches) — try/direct clears via
  `m_cache.clear()`; put_by_id replace-pair clear via
  `clearLLIntIdAndOffsetPairConcurrently` flag-on.
* `bytecode/GetByStatus.cpp` — reads `m_cache.structureID` (id-half-only read;
  no pair coherence needed).

## Size/alignment deltas (§4.3 "size deltas at Task 6")

| Metadata | before | after |
|---|---|---|
| OpTryGetById::Metadata | 8B, align 4 | 8B, **align 8** |
| OpGetByIdDirect::Metadata | 8B, align 4 | 8B, **align 8** |
| GetByIdModeMetadata (get_by_id/get_length/instanceof/iterator_*) | 16B, align 8 | unchanged (word-1 discipline only) |
| OpPutById::Metadata | 24B, align 8 | unchanged (store discipline only) |
| private name/brand ops | unchanged | unchanged (disabled flag-on) |

Net metadata-table size change: zero bytes per entry; at most +4B padding per
CodeBlock from the two alignment bumps (avoided by the BytecodeList reorder).

## I13 grep lint (inventory; re-run at Task 13)

`grep -n "metadata\.m_.*[Ss]tructureID\s*=" Source/JavaScriptCore/llint/` —
every hit is in LLIntSlowPaths.cpp and is either (a) in the §4.3 table's
surviving single-word publication path, or (b) inside a
`useUnthreadedLLIntPropertyCaches()` (flag-off-only) block:
try_get_by_id / get_by_id_direct (flag-off branch only; threaded path uses
`setConcurrently`), put_by_id transition+replace (flag-off branches; threaded
replace uses the pair helper), get_private_name / put_private_name /
set_private_brand / check_private_brand (all flag-off-only).
GetByIdModeMetadata writers (`defaultMode.structureID =`, `setUnsetMode`,
`setProtoLoadMode`, `setArrayLengthMode`) are all in LLIntSlowPaths.cpp and are
flag-off-only or flag-aware per §4.3 (I18 asserts in GetByIdMetadata.h).

## Notes for later tasks

* Task 8 (TID/SW LLInt emission): the threaded fast paths added here still
  reach the butterfly via `loadPropertyAtVariableOffset` /
  `storePropertyAtVariableOffset`; converting those to the
  `loadButterflyForRead/ForWrite` choke points (mask + checks) is §5.5/Task-8
  work and was deliberately NOT done here. `ifJSThreadsBranch` is reusable for
  Task 8's gated paths.
* Task 13 (golden diffs / `--useJIT=0` bench gate): flag-off instruction deltas
  are exactly (1) the §5.4 gate branch per affected op and (2) repack-moved
  metadata offsets — both are I1 carve-outs; bless them in the baseline.
* §4.3 charter: if Task 13's {useJSThreads=1, useSharedGCHeap=0} bench misses
  the <=5% budget, the disabled proto/transition caches return as immutable
  single-pointer records (§5.8 pattern) — REQUIRED pre-ship in that case.

# Task 7 - Call-link records (SPEC-jit section 5.8)

Owned-path changes only; NO new shared-file (M1-M6) hunks are introduced by
this task. Files touched: `bytecode/CallLinkInfo.{h,cpp}`,
`bytecode/Repatch.cpp` (one-line caller update for the new `setStub(VM&, ...)`
signature), `dfg/DFGSpeculativeJIT64.cpp`, `ftl/FTLLowerDFGToB3.cpp` (the three
G10 `UseDataIC::No` flips), this file.

## What landed

* `struct CallLinkRecord { uintptr_t comparand; CodePtr<JSEntryPtrTag> target;
  CodeBlock* codeBlockToTransfer; }` (frozen field order; static_asserts on
  offsets 0/8/16, size 24 on ADDRESS64) in `CallLinkInfo.h`.
* `m_record` appended (unconditional, D7: +8B per call op for
  `DataOnlyCallLinkInfo` bytecode metadata): literally the LAST member of the
  `CallLinkInfo` data and of `DirectCallLinkInfo`. PLACEMENT NOTE vs the spec's
  "each gain m_record as LAST member": for DataOnly/Optimizing the field lives
  at the end of the shared `CallLinkInfo` base so ONE `offsetOfRecord()` serves
  the shared data-IC fast path (`emitFastPathImpl` machine code is common to
  both subclasses); `OptimizingCallLinkInfo`'s private, C++-only
  `m_callLocation` (NO_UNIQUE_ADDRESS) therefore follows it. No JIT-visible
  offset is affected; no pre-existing field moved.
* F6 publish/unlink: `CallLinkInfo::publishRecord/clearRecord(VM&)` and
  `DirectCallLinkInfo::publishRecord/clearRecord` (init record ->
  `storeStoreFence` -> single pointer store; unlink = single null store).
  Writers converted: `setMonomorphicCallee`, the `unlinkOrUpgradeImpl`
  monomorphic in-place upgrade (now also publishes a NEW record),
  `setVirtualCall`, `setStub`, `reset`, `DirectCallLinkInfo::setCallTarget`,
  `DirectCallLinkInfo::reset`. All publication/retirement is gated on
  `Options::useJSThreads()` (I1: flag-off, no records exist; asserted).
* Retirement: replaced/unlinked records -> `RetiredJITArtifacts::retire`
  (`RetiredCallLinkRecord` callback). Destructor-time records are deleted
  inline (owning code already unreachable post-R2; destructors can run in heap
  contexts where retire() is forbidden).
* Frozen fast-path sequence, record form, emitted flag-on (JSVALUE64):
  - `emitFastPathImpl` (LLInt/Baseline shared data-IC thunks, Optimizing
    DFG/FTL calls): load r; null/comparand miss -> a process-lifetime immutable
    "empty record" sentinel whose target is `LLInt::defaultCall()` (so hit and
    miss run the SAME frozen tail: transfer `r->codeBlockToTransfer`, load
    `r->target` ONCE, call). No legacy field is read. `callLinkInfoGPR` is
    preserved for the default-call/virtual/polymorphic thunks; r rides
    `callTargetGPR` (survives `prepareForTailCall`, as today's preloaded
    target does). RISCV64 keeps its memory-operand comparand compare.
  - `emitDirectFastPath`/`emitDirectTailCallFastPath` (data-IC flavor): load r
    via macro scratch, null -> slow path (callLinkInfoGPR still = `this`),
    then r replaces callLinkInfoGPR; no comparand check; both accesses flow
    through r (F2). I16 holds: no poll between the m_record load and the call.
* UseDataIC flips (G10's three 64-bit sites): `DFGSpeculativeJIT64.cpp` direct
  call construction and both FTL `m_directCallLinkInfos.add` sites now pass
  `UseDataIC::Yes` under the flag. The FTL callers previously DISCARDED the
  data-IC fast path's slow-case JumpList (FTL never built data-IC direct calls
  before); both now capture it and link it into their slow paths (tail: before
  the shuffle, same entry convention as the patched-jump slow path; non-tail:
  into the late path, where the x86-32 return-address pop is skipped for
  branch-entered data-IC slow cases). `DFGSpeculativeJIT32_64.cpp` keeps
  `UseDataIC::No`: useJSThreads is unsupported on 32-bit (D8); the
  DirectCallLinkInfo constructor RELEASE_ASSERT enforces I3 there.
* `repatchSpeculatively` forbidden + I2 asserts at the section 5.8 patching
  sites: already landed in Task 1; verified present, unchanged.
* Section 4.5 atomic refcounts: ALREADY LANDED (Task 3) -
  `InlineCacheHandler : ThreadSafeRefCounted`, `JITStubRoutine::m_refCount`
  is `std::atomic<unsigned>`. No action this task.
* Polymorphic-stub mirror hazard fixed: the shared polymorphic-call thunk
  reloads `m_stub` through the CallLinkInfo (ThunkGenerators.cpp:369), so
  flag-on (a) `clearStub()` does `unlinkForcefully` bookkeeping but KEEPS the
  pointer published (no null window for racing readers; ref released at
  replacement or CallLinkInfo death), (b) `setStub()` replaces the routine
  with ONE raw-pointer store (fence-before-publish) and derefs the displaced
  routine - safe because GCAware stub routines are reclaimed only after R2's
  conservative scan even at refcount zero, and (c) the `stub()` accessor
  returns null flag-on unless `mode() == Polymorphic`, so C++ logic
  (visitWeak/forEachDependentCell/reset) sees today's semantics.

## Deferred / integration-gate items (THREADS-INTEGRATE(jit))

* LLINT ASM RECORD FORM NOT YET EMITTED: `LowLevelInterpreter64.asm`'s
  monomorphic call fast path (~:2573-2683) still reads the legacy
  `m_callee`/`m_monomorphicCallDestination` mirror pair. That word-pair read is
  exactly the protocol section 5.8 retires; it stays sound ONLY under the
  phase-1 GIL. The record-form LLInt fast path (one `loadp
  CallLinkInfo::m_record`, null test, comparand test, transfer+call through
  the record) must land behind section 5.4's `ifJSThreadsBranch` gate together
  with Task 8's asm pass (M4a is already a prep precondition). Until then the
  C++ writers keep the mirrors in sync, so flag-off AND GIL'd flag-on behavior
  is unchanged. HARD precondition for the Task 13 integration gate
  (true-concurrency call-relink stress).
* Call link/unlink slow paths (incoming-call list insert/remove,
  `unlinkForcefully`, `linkMonomorphicCall`/`linkPolymorphicCall` themselves)
  are serialized today only by the GIL; N-mutator serialization of those slow
  paths is owned by the integration phase (they are C++ slow paths, not
  emitted code; records make the FAST paths safe regardless).
* `PolymorphicCallStubRoutine::upgradeIfPossible` mutates CallSlots in place;
  it is reached only from `unlinkOrUpgrade` (GC/STW per Task 5), asserted
  nowhere new here - covered by the Task 11/13 audits.

---

# Task 8 — TID/SW emission, LLInt + Baseline (§5.5; choke points, frozen predicates, R3 slow-path routing, I14 inventory + lint)

## What landed (owned paths)

* `jit/CCallHelpers.h` / `jit/CCallHelpers.cpp` — the §5.5 Baseline/stub choke
  points (the spec's `CCallHelpers::loadButterflyForRead/ForWrite` by name):
  - `ConcurrentButterflyShape { KnownNonArrayStorage, KnownArrayStorage,
    MaybeArrayStorage }` (AS-rule clause (c) = Known* shapes).
  - `loadButterflyForRead(base, dest, shape, indexingScratch=Invalid,
    structureID=Invalid) -> JumpList`: flag-off ONE `loadPtr` (I1); flag-on
    frozen READ predicate — segmented (`tagged >= 0xffff<<48`, unsigned
    compare = top16==0xffff) => slow; SW=1∧AS => slow (indexing byte loaded
    iff a scratch was supplied, else conservative SW=1 => slow, a sound
    superset); mask ALWAYS emitted (`and64 0x0000ffffffffffff`, I14(a)/D6).
  - `loadButterflyForWrite(base, dest, tidScratch, shape, ...) -> JumpList`:
    (1) segmented => slow; (2) owner via `tidScratch = tagged ^
    g_jscButterflyTIDTag; < 2^48` (fused TID compare, NEVER elided, D9);
    (3) SW=1∧notAS => mask+store; (4) else slow. KnownArrayStorage: any
    non-owner => slow (I20). MaybeArrayStorage w/o indexing scratch: any
    non-owner => slow (conservative).
  - `loadButterflyTIDTag(dest)`: ELF `loadFromELFTLS64(butterflyTIDTagELFTLSOffset())`;
    Darwin `loadFromTLS64(fastTLSOffsetForKey(butterflyTIDTagTLSKey()))` under
    `ENABLE(FAST_TLS_JIT)`; otherwise RELEASE_ASSERT (D8).
  - `maskButterflyTag(dest)`.
  - R7/F7: optional `structureIDGPR` — ARM64 emits `eor sid,sid -> 0; add
    base` so the butterfly load is address-dependent on the compared
    structureID (no-op x86-64); skipped when dest==base or the sid register
    was reused (gaps listed below).
  - Threaded property accessors `loadProperty(object, offset, result,
    storageScratch, slowCases, structureID=Invalid)` and
    `storeProperty(value, object, offset, scratch, tidScratch, slowCases,
    structureID=Invalid)` — the out-of-line branch goes through the choke
    points; inline branch is cell-internal (never checked/masked, §5.5).
    `storageScratch` exists because resultJSR may alias baseJSR (ARM64
    GetById: x0), and the butterfly word must not clobber base on the
    slow-case path. JSVALUE32_64 stubs RELEASE_ASSERT(!useJSThreads) (D8).
* `jit/AssemblyHelpers.cpp`:
  - LEGACY `loadProperty`/`storeProperty` (no slow-path list): flag-on they
    emit a tag guard — `tagged >= 2^48 => breakpoint()` — so any unconverted
    caller traps loudly instead of misreading a tagged/segmented word (I14
    enforcement-by-construction; flag-off emits nothing). Reachable flag-on
    legacy users are only: gated transition/delete thunk bodies (dead),
    megamorphic emitters (gated, below), DFG/FTL emitters (Tasks 9/10 convert
    or keep them trapped until then).
  - `loadMegamorphicProperty` / `storeMegamorphicProperty`: flag-on emit an
    unconditional slow-case jump (megamorphic fast paths read the VM-global
    MegamorphicCache unsynchronized AND deref the butterfly without the
    predicate). One choke gates all Baseline+DFG megamorphic emission.
    Revisit with vmstate's shared-cache story.
  - Typed-array wasteful-mode butterfly loads (`loadTypedArrayByteLength*`,
    2 sites): flag-on mask-only (never segmented/SW; the butterfly routes to
    the ArrayBuffer) — I14(a).
* `bytecode/InlineCacheCompiler.cpp` — every reachable butterfly access
  converted or gated (per-site inventory below); shared handler thunks
  (`getByIdLoadOwn/Prototype`, getter/setter, `putByIdReplaceHandler`,
  getByVal load variants) carry the predicates via the new overloads; PutByVal
  replace/setter shared thunks defer flag-on (register-file limit);
  transition/delete thunk bodies gated/trapped.
* `bytecode/Repatch.cpp` — flag-on `GiveUpOnCache` gates: put TRANSITION
  caching (tryCachePutBy NewProperty branch), DELETE caching
  (tryCacheDeleteBy), SET-PRIVATE-BRAND caching (tryCacheSetPrivateBrand).
  Rationale: §5.5 transition emission is legal only with valid+watched
  transitionThreadLocal/writeThreadLocal sets (OM E4) which do not exist
  in-tree yet => "else R3 slow paths" = generic locked OM path.
* `jit/JITInlineCacheGenerator.cpp` — threaded packed GetByIdSelf inline path
  now routes its out-of-line load through the choke (storage scratch =
  scratch2); threaded packed PutById inline path stores INLINE offsets only
  (cell-internal) and dispatches out-of-line offsets to the replace handler
  (no spare GPR pair with base preserved on miss); inline ArrayLength path
  flag-on uses the conservative READ choke.
* `jit/JITPropertyAccess.cpp` — get/put_to_scope GlobalProperty fast paths
  (3 sites incl. the thunk) and enumerator get/put fast paths converted
  (enumerator put OOL defers to the generic IC route flag-on).
* `llint/LLIntOfflineAsmConfig.h` — new `OFFLINE_ASM_LINUX` setting.
* `llint/LowLevelInterpreter64.asm` — LLInt choke points + frozen predicates:
  - `loadButterflyTIDTagToT4(slow)`: Linux x86-64 `movq
    %fs:g_jscButterflyTIDTag@TPOFF, %r8`; Linux arm64 `mrs/add
    :tprel_hi12:/ldr :tprel_lo12_nc:` into x4 (App. R5 verbatim); ALL other
    configs (Darwin until M4a's key slot, Windows, C_LOOP) jump to the slow
    label — i.e. LLInt threaded WRITE fast paths are Linux-only for now;
    reads are unaffected.
  - `threadedButterflyReadPredicate` / `threadedButterflyWritePredicate`
    (frozen §5.5 forms; the SW branch loads the indexing byte per the
    generic-path rule; AS => locked slow path; mask always).
  - `loadPropertyAtVariableOffsetThreaded` / `storePropertyAtVariableOffsetThreaded`
    (out-of-line branch through the predicates; inline branch untouched).
  - `butterflyLoadDependsOnStructureID` (R7, ARM64-only eor+add).
  - Wired into: try_get_by_id / get_by_id_direct / get_by_id+get_length
    threaded Default (these previously used the RAW macro — a Task 6 hole,
    now closed), NEW `.opGetByIdThreadedArrayLength` block (full predicate;
    AS arrays), put_by_id threaded replace (write predicate + R7),
    get_by_val + put_by_val(+_direct) (gate + threaded blocks rejoining the
    shared shape dispatch), enumerator_get/put_by_val out-of-line (R7 dep
    from the cell-sid register added at review round 3, R3-7 — these accesses
    are structure-bounded, so the threaded twins carry the same
    cell-sid-in-register + butterflyLoadDependsOnStructureID form as
    put_by_id's R1-1 fix), get_from_scope/put_to_scope GlobalProperty (gate
    inside getProperty()/putProperty() + R7 dep after
    loadScopeWithStructureCheck).

## I14 site inventory (every generated-code butterfly deref, this tier set)

Disposition codes: CHOKE-R/W = routed through a read/write choke point;
FLAG-OFF = unreachable flag-on (gated emission or gated cache publication);
MASK = mask-only (proven never segmented/SW); TRAP = legacy guard traps if
tagged; GATED = creation gated in Repatch/compiler.

| Site | Disposition |
|---|---|
| LLInt64 loadPropertyAtVariableOffset (1584) / storePropertyAtVariableOffset (1597) | FLAG-OFF (all callers gated; private-name callers rely on Task 6 never publishing their caches flag-on) |
| LLInt64 threaded macros (choke points) | CHOKE-R/W |
| LLInt64 .opGetByIdArrayLength loadCagedJSValue | FLAG-OFF (threaded mode dispatch targets .opGetByIdThreadedArrayLength) |
| LLInt64 get_by_val / put_by_val loadCagedJSValue | FLAG-OFF (gated; threaded blocks CHOKE-R/W) |
| LLInt64 enumerator get/put .outOfLine | FLAG-OFF + CHOKE-R/W threaded twins (+ R7 cell-sid dependency, R3-7) |
| LLInt64 get/put_to_scope getProperty/putProperty | FLAG-OFF + CHOKE-R/W threaded twins |
| LLInt32_64 (8 sites) | FLAG-OFF (useJSThreads unsupported on 32-bit, D8 — integrator: M2b startup error must also reject !ADDRESS64) |
| InlineCacheCompiler ArrayLengthStore (2063) | CHOKE-W KnownNonArrayStorage |
| InlineCacheCompiler IndexedArrayStorageLoad/InHit (2528) | CHOKE-R KnownArrayStorage |
| InlineCacheCompiler IndexedInt32/Double/ContiguousLoad/InHit (2574) | CHOKE-R KnownNonArrayStorage |
| InlineCacheCompiler IndexedArrayStorageStore (2648) | CHOKE-W KnownArrayStorage |
| InlineCacheCompiler IndexedInt32/Double/ContiguousStore (2683) | CHOKE-W KnownNonArrayStorage |
| InlineCacheCompiler Load/GetGetter out-of-line (3373/3549) | CHOKE-R MaybeArrayStorage (conservative SW=1=>generic) |
| InlineCacheCompiler Replace + Indexed*KeyReplace OOL (3711) | flag-on emits m_failAndIgnore (no spare reg; hot Replace goes through putByIdReplaceHandler) |
| InlineCacheCompiler Transition (3793/3821/3897) | GATED (Repatch) + RELEASE_ASSERT(!useJSThreads) |
| InlineCacheCompiler Delete (3941) / SetPrivateBrand | GATED + RELEASE_ASSERT |
| InlineCacheCompiler ArrayLength (3954) | CHOKE-R MaybeArrayStorage |
| Shared thunks loadHandlerImpl own/proto (getById + getByVal variants) | CHOKE-R via threaded loadProperty (storage scratch3) |
| Shared thunks getter/setterHandlerImpl (ById + getByVal getter) | CHOKE-R |
| putByIdReplaceHandler | CHOKE-W (scratch2 storage, scratch3 TID) |
| putByVal replace (string/symbol + NonStringPrimitiveKey) / putByVal setter thunks | flag-on ALWAYS defer to next handler/generic (PutByVal register file has no third scratch with profileGPR preserved) |
| deleteById/deleteByVal delete thunks | GATED + flag-on defer (defense in depth) |
| transitionHandlerImpl (5802/5857/5881) | GATED; flag-on thunk body = breakpoint() |
| JITInlineCacheGenerator packed GetByIdSelf threaded | CHOKE-R |
| JITInlineCacheGenerator packed PutById threaded | inline-only store; OOL => handler |
| JITInlineCacheGenerator inline ArrayLength | CHOKE-R flag-on |
| JITPropertyAccess get_from_scope GlobalProperty (1294/1441) | CHOKE-R (dest==base, conservative) |
| JITPropertyAccess put_to_scope GlobalProperty (1613) | CHOKE-W MaybeArrayStorage conservative |
| JITPropertyAccess enumerator get OOL (2089) | CHOKE-R (failures -> mismatch/generic route) |
| JITPropertyAccess enumerator put OOL (2187) | flag-on defers to mismatch/generic route |
| bytecode/InlineAccess.cpp generate* (5 butterfly loads) | FLAG-OFF: every generator early-returns unless the cache dynamicDowncasts to RepatchingPropertyInlineCache, which I3 (Tasks 2/4) forbids constructing flag-on |
| AssemblyHelpers legacy loadProperty/storeProperty | TRAP flag-on |
| AssemblyHelpers loadMegamorphicProperty/storeMegamorphicProperty | flag-on unconditional slow (gates all megamorphic emission) |
| AssemblyHelpers typed-array byteLength butterfly loads (2 sites) | MASK |
| AssemblyHelpers.h butterfly INSTALL stores (nukeStructureAndStoreButterfly 1920-1931; emitAllocate* 2070/2106) | INSTALL sites, not accesses: 1920-1931 reachable only from gated transition stubs (dead flag-on); 2106 stores null (tag 0 legal); 2070 (DFG array/object allocation with storage) stores a RAW pointer = tag (0,0) — claims main-thread ownership. SOUND under phase-1 GIL only. Tasks 9/10 + OM allocation tagging MUST or-in the R5 tag at install. HARD precondition for GIL removal. |
| DFG/FTL emitters (DFGSpeculativeJIT GetButterfly :11311, FTLLowerDFGToB3 compileGetButterfly :5823, AssemblyHelpers.cpp callers in dfg/ftl) | Tasks 9/10 (legacy TRAP protects loadProperty/storeProperty paths meanwhile; raw GetButterfly lowerings remain UNCONVERTED — flag-on DFG/FTL is unsound until Task 9/10, as scoped) |

## Grep lint (Task 8 form of the I14 choke-point lint; re-run at Task 13)

```sh
# LLInt: every m_butterfly use must be a choke macro or in a flag-off-gated block
grep -n "m_butterfly" Source/JavaScriptCore/llint/LowLevelInterpreter64.asm
#   expected: 2 legacy macros (flag-off), 2 threaded macros, and per-op pairs
#   {flag-off site, threaded twin}; any NEW bare site = violation.
# Baseline/IC: no raw butterfly loads outside choke points / gated blocks
grep -rn "butterflyOffset())" Source/JavaScriptCore/jit Source/JavaScriptCore/bytecode \
  | grep -v "loadButterflyForRead\|loadButterflyForWrite\|nukeStructureAndStoreButterfly\|storePtr\|store"
# AssemblyHelpers legacy accessors must keep the flag-on trap:
grep -n "emitLegacyButterflyTagTrap" Source/JavaScriptCore/jit/AssemblyHelpers.cpp
```

## Known gaps / follow-ups (MUST-FIX list for Tasks 9/10/13 + integrator)

1. **R7/F7 ARM64 dependency gaps — CLOSED at the choke point (review round 1)**:
   `CCallHelpers.cpp loadButterflyWithStructureDependency` now emits the
   dependency UNCONDITIONALLY on ARM64 whenever dest != base: if no live
   structureID register is supplied it RE-LOADS the cell's structureID through
   destGPR and builds the eor/add dependency from the re-load
   (coherence-sound: the re-load can never return an older value than the
   guard's compared load). This automatically covers every CCallHelpers-routed
   site that previously passed InvalidGPRReg: getByVal load handlers,
   putByIdReplaceHandler, generateImpl Load/Replace, Baseline enumerator
   emitters. Additionally wired directly (review round 1): the Baseline packed
   GetByIdSelf inline path (JITInlineCacheGenerator.cpp — it now passes the
   sid-dependent decoded-offset register as structureIDGPR; it was previously
   marked CHOKE-R in the I14 table while silently passing InvalidGPRReg) and
   the LLInt op_put_by_id threaded replace path (the dependency was previously
   built from the METADATA cache word, not the cell structureID load — fixed
   to mirror op_try_get_by_id). REMAINING residue (recorded, GIL-removal
   precondition 6): sites where dest == base (no temp register exists), i.e.
   the Baseline get_from_scope chokes at JITPropertyAccess.cpp (now moot —
   GlobalProperty scope caches are never armed flag-on, see the scope-metadata
   freeze below). x86-64 unaffected (TSO).
2. **Conservative slow-routing** (perf, not soundness): SW=1 reads through
   IC named-property paths go generic; foreign writes via shape-unknown
   paths go generic; putByVal replace/setter + enumerator-put OOL +
   megamorphic always generic flag-on. Revisit if the Task 13 {1,0} budget
   misses.
3. **LLInt threaded writes are Linux-only** until the M4a
   `butterflyTIDTagTLSKey` JSCConfig slot lands (Darwin needs register-form
   tls_loadp; App. R5). Non-Linux: writes slow-path, reads full speed.
4. **`@TPOFF` link caveat**: App. R5's spelling is the local-exec relocation.
   Static links (Bun) are fine; if a `-shared` libJSC link rejects
   R_X86_64_TPOFF32 / TLSLE on arm64, switch the two emits to the
   initial-exec GOTTPOFF / `:gottprel:` forms (semantics identical here).
5. **Caging**: LLInt threaded paths skip `loadCagedJSValue`'s cage step
   (mask replaces it). SUPERSEDED by the Task-14 M2b note (review round 1):
   the JSValue gigacage does not exist in this tree (Gigacage::Kind has only
   `Primitive`), so the originally proposed assert does not compile and is
   not needed — the concern is vacuously satisfied.
6. **M2b addition**: startup error must also reject `useJSThreads` on
   !CPU(ADDRESS64) and on platforms without a JIT-visible TLS mechanism
   (matches D8 and the LLInt32_64/JSVALUE32_64 stubs).
7. **Task 7 leftover (unchanged)**: the LLInt monomorphic CALL fast path
   record form is still pending (see the Task 7 deferred items above); it is
   a §5.8 item, sound under the GIL, and remains a hard precondition for the
   Task 13 integration gate.
8. **Private-name/brand LLInt fast paths** rely on Task 6's
   LLIntSlowPaths.cpp never publishing their caches flag-on (structureID
   compare can never match, so the raw butterfly macros stay unreachable).
   The Task 13 lint must keep asserting that property.
9. **Transition fast paths return later**: when OM lands
   `transitionThreadLocal`/`writeThreadLocal` sets +
   `Structure::m_transitionThreadLocalTID`, revisit the three Repatch gates
   and emit the §5.5 Transition predicate (E4: TTL sets valid+watched + PA
   bit test `cell & 8` + owner-tag compare) instead of GiveUpOnCache.

## Integrator actions (shared files; ready-to-paste already covered by earlier sections)

* No NEW shared-file hunks beyond what Tasks 1-7 recorded. M4a remains the
  blocker for Darwin LLInt writes (item 3). M2b text gains items 5/6 above.

---

# Task 9 — TID/SW + TTL elision, DFG (§5.5 sites) + DFGDesiredWatchpoints

**No new shared-file (M1–M6) hunks.** Owned-path changes only. Files touched:
`dfg/DFGDesiredWatchpoints.{h,cpp}`, `dfg/DFGSpeculativeJIT.{h,cpp}` (the two
spec-listed sites plus this file's auxiliary butterfly loads),
`dfg/DFGMayExit.cpp`, `dfg/DFGClobberize.h`, `dfg/DFGByteCodeParser.cpp`,
`dfg/DFGConstantFoldingPhase.cpp`.

## What landed

* **DFGDesiredWatchpoints (E1/E2 registration)**:
  `DesiredWatchpoints::considerButterflyTransitionThreadLocal(Structure*)` (E1)
  and `considerButterflyWriteThreadLocal(Structure*)` (E2). Each returns true
  and `addLazily`s the structure's InlineWatchpointSet iff it is currently
  valid+watched (`state() == IsWatched`, a racy compile-thread read). The sets
  ride the existing `m_inlineSets` machinery: a `CodeBlockJettisoningWatchpoint`
  is installed at `reallyAdd` (fire => jettison, §5.3; Task 11 makes fires STW)
  and `hasBeenInvalidated()` is revalidated there, so a set fired between
  compilation and linking fails the compilation. Flag-off the sets start
  `ClearWatchpoint` (OM ctor) and the helpers return false.
* **SpeculativeJIT plan/emission helpers** (JSVALUE64 only, D8):
  - `ThreadedButterflyPlan planThreadedButterflyAccess(Edge base)`: from
    `m_state.forNode(base).m_structure` (finite => per-structure scan):
    AS-rule clause (c) shape classification (KnownNonArrayStorage /
    KnownArrayStorage / MaybeArrayStorage via `hasAnyArrayStorage(indexingMode)`)
    plus E1/E2 elision decisions, registered through the new
    DesiredWatchpoints helpers (any registration failure voids the elision).
    Infinite/clobbered structure sets (typical for arrays, whose speculation
    is CheckArray/indexing-shape-based, not structure-based) => no elision,
    MaybeArrayStorage.
  - `emitThreadedButterflyLoadForRead/ForWrite(...)`: the frozen §5.5
    predicates, elision-aware (the Task-8 CCallHelpers chokes are not — these
    DFG twins mirror them exactly): E1 omits the segmented compare; E2 omits
    the SW branch (3) + AS SW test; the WRITE fused owner-TID compare and the
    case-(4) fallback are ALWAYS emitted (D9); the mask is ALWAYS emitted
    (E3/D6/I14(a)). R7/F7: ARM64 re-loads the structureID into a scratch and
    makes the butterfly load address-dependent on it (eor+add; re-load is
    sound by structure/butterfly coherence; no-op x86-64) — INCLUDING on the
    fully-elided E1+E2 paths, per §5.5 "incl. elided E1+E2".
* **compileGetButterfly (`:11311`)**: flag-on, full read predicate via the
  helpers. Slow-case dispatch = **OSR exit** (`BadIndexingType`, per the task's
  "OSR-exit where profitable else slow path"): at GetButterfly OSR exit is the
  only sound choice — no storage pointer a slow-path call could return is
  legally usable by downstream direct loads (segmented spines need a dependent
  fragment load per access; SW=1 AS needs the cell lock), and BadIndexingType
  reprofiles consumers toward generic/R3 on recompile. Flag-off byte-identical
  (original Reuse-base path kept verbatim).
* **compilePutByOffset**: flag-on + out-of-line offset, the store re-loads the
  TAGGED butterfly from the base (child2) and runs the write predicate in the
  same poll-free window as the store (I16). The storage child (GetButterfly's
  masked result) is deliberately NOT the store target: polls may sit between a
  (possibly LICM-hoisted) GetButterfly and the store, and OM §4.6's per-event
  STW argument needs predicate+store on one freshly loaded word. Slow cases =
  OSR exit (`BadCache`): the store has not happened, baseline re-executes the
  put through the OM's regime-aware C++ access (R3); the case-(4)
  first-foreign-write exit is one-time (the generic path sets SW, F1) and
  BadCache exit-site profiling reroutes to the generic IC on recompile.
  Inline offsets keep today's path (cell-internal, never checked/masked).
  Both the JSValue and DoubleRep forms are covered.
* **Auxiliary DFGSpeculativeJIT.cpp butterfly loads** (this file's share of the
  I14 inventory):
  - compileSpread fast path (3 loads): CHOKE-R `loadButterflyForRead`
    KnownNonArrayStorage, slow => existing `operationSpreadFastArray` path.
  - compileArraySortCompact: CHOKE-R KnownNonArrayStorage, slow => OSR exit.
  - compileArraySortCommit: CHOKE-**W** (the commit loop stores into the
    butterfly; tid scratch allocated flag-on only), slow => OSR exit BEFORE
    any store.
  - compileEnumeratorGetByVal `!storageEdge` load: CHOKE-R MaybeArrayStorage
    conservative, slow => existing generic/recover operation route.
  - compileCreateRest / compileNewArrayWithSpread / compileArraySlice result
    loads: FRESH-ALLOCATION (raw install + raw load are consistent; commented;
    see the Task-8 allocation-tagging item).
* **Transition machinery unreachable flag-on** (§5.5: "the JIT never
  implements transition semantics"; E4 not emitted by this tier):
  - `DFGByteCodeParser.cpp`: `handlePutById` Transition case and the
    private-name define Transition case bail to the generic op flag-on;
    both MultiPutByOffset-creation branches bail when any variant is a
    Transition (also protects FTL/Task 10).
  - `DFGConstantFoldingPhase.cpp`: `tryFoldAsPutByOffset` refuses Transition
    variants flag-on; the MultiPutByOffset folding case keeps the node
    instead of folding a Transition variant.
  - `compileAllocatePropertyStorage` / `compileReallocatePropertyStorage` /
    `compileNukeStructureAndSetButterfly`: emission-time
    `RELEASE_ASSERT(!Options::useJSThreads())` (fail-fast; the raw butterfly
    install would claim tag (0,0) and bypass the OM transition protocol).
    Transition fast paths return with E4 (same trigger as the Task-8
    Repatch-gate item 9).
* **DFGMayExit.cpp**: `GetButterfly`/`PutByOffset` moved out of the
  unconditional DoesNotExit list; flag-on they report `Exits` (they emit
  speculation checks now). Flag-off unchanged.
* **DFGClobberize.h**: flag-on, `GetButterfly` and `PutByOffset` additionally
  `read(JSCell_structureID)` + `read(JSCell_indexingType)` (R7 dependency +
  AS test), and `PutByOffset` `read(JSObject_butterfly)` (the re-load).
  Flag-off unchanged.

## I14 site inventory delta (DFG tier; extends the Task-8 table)

| Site | Disposition |
|---|---|
| compileGetButterfly | CHOKE-R (DFG twin), E1/E2-aware, slow => OSR exit |
| compilePutByOffset OOL | CHOKE-W (DFG twin), E1/E2-aware, TID compare never elided, slow => OSR exit |
| compilePutByOffset inline | cell-internal (never checked/masked) |
| compileSpread ×3 | CHOKE-R KnownNonArrayStorage => operationSpreadFastArray |
| compileArraySortCompact | CHOKE-R => OSR exit |
| compileArraySortCommit | CHOKE-W => OSR exit (pre-store) |
| compileEnumeratorGetByVal (!storageEdge) | CHOKE-R MaybeArrayStorage conservative => generic route |
| compileCreateRest / NewArrayWithSpread / ArraySlice result loads | FRESH-ALLOC (raw/raw consistent; OM allocation tagging follow-up) |
| compileAllocate/Reallocate/NukeStructureAndSetButterfly | UNREACHABLE flag-on (gated at creation + RELEASE_ASSERT) |

## Known gaps / MUST-FIX follow-ups (Task 10 / Task 13 / integrator)

1. **DFG64 array-element WRITES lack the per-store TID compare (D9)**:
   `DFGSpeculativeJIT64.cpp` PutByVal (Int32/Double/Contiguous/ArrayStorage),
   ArrayPush/Pop and friends store through the GetButterfly-masked storage
   operand with no write predicate at the store. GetButterfly's predicate
   makes their READS sound, but a foreign-thread store through these paths
   would not set SW => GIL-SOUND ONLY. This mirrors the Task-8 "DFG/FTL
   unsound until Task 9/10, as scoped" row; the remaining DFG64 share is a
   HARD precondition for GIL removal (same bucket as the LLInt call-record
   item). Cheapest shape: re-load+predicate at the store via
   emitThreadedButterflyLoadForWrite, exactly like compilePutByOffset.
2. `DFGSpeculativeJIT64.cpp:8506` (enumerator-put OOL raw butterfly load):
   unconverted, same bucket as item 1.
3. **F7/R7 ARM64 dependency gaps**: the spread/sort/enumerator choke calls
   pass no structureIDGPR (their guards are indexing-shape, not structure,
   checks). Same closing recipe as Task-8 gap 1.
4. **Allocation tagging** (restated from Task 8): MaterializeNewObject /
   NewObject-with-storage etc. install RAW butterflies via
   `AssemblyHelpers::emitAllocate*`; flag-on executed by a non-main thread
   this claims tag (0,0). GIL-sound; OM allocation tagging + or-in of the R5
   tag at install is the fix (Tasks 9/10 emission can then drop the
   FRESH-ALLOC carve-outs).
5. **M1 kill switch**: `useThreadedDFG` is still absent (PREP). All Task-9
   flag tests key on `Options::useJSThreads()` alone; when M1 lands, AND the
   three THREADS-INTEGRATE-marked sites (compileGetButterfly,
   compilePutByOffset, and the DFGMayExit/DFGClobberize tests may stay on the
   master flag — conservative either way).
6. **Exit-origin audit (Task 13)**: GetButterfly/PutByOffset historically
   never exited; any phase that placed one at an `exitOK == false` origin was
   previously legal and will now assert flag-on at emission. The flag-on
   single-threaded suite run (Task 13) is the audit; expected clean (the only
   known `withInvalidExit()` placements are the transition-sequence nodes,
   which are unreachable flag-on).
7. **R3 dedicated shims still unreferenced**: the DFG routes every predicate
   failure to OSR exit / existing generic operations, so the
   `operationSegmentedButterfly*` / `operationSharedArrayStorage*` shims in
   `jit/ConcurrentButterflyOperations.h` remain unreferenced after Task 9
   (their forwarding bodies stay a THREADS-INTEGRATE item; first expected
   consumer is the FTL, Task 10, and the DFG64 item-1 fix may tail-call them).

---

# Task 10 — TID/SW + TTL elision, FTL (§5.5 FTL sites + patchpoints, after Task 2)

**No new shared-file (M1–M6) hunks.** Owned-path changes only. Files touched:
`ftl/FTLLowerDFGToB3.cpp` (the spec-listed site + this file's auxiliary
butterfly loads), `dfg/DFGClobberize.h` (flag-on read additions for the
FTL-only nodes, mirroring Task 9's GetButterfly/PutByOffset hunk).

## What landed

* **FTL plan/emission helpers** (LowerDFGToB3 members, twins of Task 9's
  SpeculativeJIT helpers and Task 8's CCallHelpers chokes):
  - `ThreadedButterflyPlan planThreadedButterflyAccess(Edge)` (abstract-value
    based, `m_interpreter.forNode(...).m_structure`),
    `planThreadedButterflyAccessForStructureSet(RegisteredStructureSet|StructureSet)`
    (MultiByOffset per-case/per-variant), both over one core
    (`planThreadedButterflyAccessForStructures`): AS-rule clause (c) shape
    classification + E1/E2 elision registered through Task 9's
    `DesiredWatchpoints::considerButterflyTransitionThreadLocal/WriteThreadLocal`
    (registration failure voids the elision; fire => jettison §5.3).
  - `threadedButterflyLoadForRead/ForWrite(LValue base, plan)`: the frozen
    §5.5 predicates evaluated BRANCHLESSLY in B3 (compares OR-folded into one
    `slowCondition`; decision set proven equal to the DFG branchy twins):
    E1 omits the segmented compare; E2 omits SW branch (3) + AS SW test; the
    WRITE fused owner-TID compare (`(tagged ^ tidTag) >= 2^48` => slow side)
    and case-(4) fallback are ALWAYS emitted (D9); the mask is ALWAYS emitted
    (E3/D6/I14(a)). Under E1 a segmented word still routes slow via the
    never-elided owner compare (its TID field is the notTTLTID sentinel).
  - **R5 tag load** = pure `PatchpointValue` (Int64, `Effects::none()`,
    generator = Task 8's `CCallHelpers::loadButterflyTIDTag`): loop-invariant /
    hoistable as R5 requires; per-platform mechanics inherited from Task 1b.
  - **R7/F7** = `B3::Depend` (ARM64 only; x86-64 TSO no-op): structureID
    re-load -> `Depend` -> folded into the butterfly load's address, INSIDE
    the choke helper, so EVERY converted FTL site (including the
    indexing-shape-guarded sort/spread/enumerator sites that are an open F7
    gap in Tasks 8/9) carries the dependency. No FTL F7 gap.
* **compileGetButterfly (spec site, was :5823/:5883)**: flag-on full read
  predicate; slow = OSR exit (`BadIndexingType`) — Task 9's rationale holds
  verbatim (no materializable storage pointer; segmented spine needs a
  dependent fragment load per access, SW=1 AS needs the cell lock).
  Flag-off byte-identical.
* **compilePutByOffset**: flag-on + out-of-line offset re-loads the TAGGED
  butterfly from the base (child2) and runs the write predicate in the same
  poll-free window as the store (I16: FTL polls are explicit
  CheckTraps/invalidation patchpoints from other nodes; the predicate loads
  cannot sink below the exiting Check, the store sits after it). Storage
  child deliberately NOT the store target (LICM-hoisted GetButterfly + OM
  §4.6 per-event STW argument, as in Task 9). Slow = OSR exit (`BadCache`).
  Inline offsets keep today's path. JSValue + DoubleRep forms covered.
* **Property patchpoints (spec site)**: with Task 2 the FTL's
  GetById/PutById/InById/DelBy/PrivateName patchpoints are handler ICs whose
  stub/handler code is emitted by `InlineCacheCompiler` /
  `JITInlineCacheGenerator` through the Task 8 CCallHelpers choke points —
  already predicate-correct; nothing to emit in the lowering. The
  megamorphic patchpoints (`loadMegamorphicProperty`/`storeMegamorphicProperty`)
  are force-slow flag-on at the AssemblyHelpers choke (Task 8 row). Recorded
  here as the "patchpoints" half of the Task-10 site list.
* **MultiGetByOffset** (FTL-only node): self OOL loads plan against the
  case's `RegisteredStructureSet` (E1/E2-eligible); prototype-base OOL loads
  (constant cell, unspeculated structure) use the conservative
  MaybeArrayStorage plan. Slow = OSR exit (`BadCache`).
* **MultiPutByOffset** (FTL-only node): Replace OOL storage = write choke
  planned per-variant (`variant.oldStructure()`); slow = OSR exit
  (`BadCache`). Transition arm: `RELEASE_ASSERT(!Options::useJSThreads())`
  (unreachable flag-on via Task 9's ByteCodeParser/ConstantFolding gates,
  which protect the FTL too).
* **Transition machinery fail-fast** (mirrors Task 9):
  `compileAllocatePropertyStorage` / `compileReallocatePropertyStorage` /
  `compileNukeStructureAndSetButterfly` / `storageForTransition` each
  `RELEASE_ASSERT(!Options::useJSThreads())` (raw install would claim tag
  (0,0) and bypass the OM transition protocol; E4 never emitted by this tier).
* **Auxiliary FTLLowerDFGToB3.cpp butterfly loads** (this file's share of the
  I14 inventory):
  - multi-mode GetByVal `tryLoadJSArray`: CHOKE-R KnownNonArrayStorage
    (Int32/Double/Contiguous dispatch = AS-rule (c)), slow => OSR exit.
  - multi-mode PutByVal `tryStoreJSArray`: CHOKE-W KnownNonArrayStorage,
    slow => OSR exit, pre-store.
  - compileSpread fast path: CHOKE-R KnownNonArrayStorage, slow => existing
    `operationSpread` slow block.
  - compileArraySortCompact: CHOKE-R, slow => OSR exit.
  - compileArraySortCommit: CHOKE-W pre-store, slow => OSR exit.
  - compileEnumeratorGetByVal (!storageEdge): CHOKE-R MaybeArrayStorage
    conservative, slow => existing genericOrRecover block.
  - compileEnumeratorPutByVal OOL: CHOKE-W MaybeArrayStorage conservative,
    slow => genericOrRecover.
  - compileMultiDeleteByOffset hit-variant OOL: CHOKE-W per-variant (slot
    zeroing protected) — but see known gap 3 below.
  - compileMaterializeNewObject / allocateJSArray slow-path butterfly
    re-loads: FRESH-FROM-OPERATION (just allocated on this thread:
    current-owner, SW=0, never segmented) — no predicate, mask ALWAYS
    emitted flag-on (no-op until OM allocation tagging lands, then load-
    bearing).
  - `allocateObject`/array fast-path butterfly INSTALL stores + ArrayValues
    fast butterflies: FRESH-ALLOC carve-out (raw install/raw use consistent;
    Task-8/9 allocation-tagging item restated, gap 2).
* **DFGClobberize.h**: flag-on, MultiGetByOffset/MultiPutByOffset/
  MultiDeleteByOffset additionally `read(JSCell_indexingType)` (AS-rule arm),
  ArraySortCompact/Commit additionally `read(JSCell_structureID)` (ARM64
  R7/F7 dependency; covers the DFG twins as well). Flag-off unchanged.
  GetByVal/PutByVal/Spread/Enumerator* already read indexingType or
  clobberTop. DFGMayExit: Task 9's GetButterfly/PutByOffset hunk suffices —
  every other converted node was already outside the DoesNotExit list.

## I14 site inventory delta (FTL tier; extends the Task-8/9 tables)

| Site | Disposition |
|---|---|
| compileGetButterfly | CHOKE-R (FTL twin), E1/E2-aware, slow => OSR exit |
| compilePutByOffset OOL | CHOKE-W (FTL twin), TID compare never elided, slow => OSR exit |
| compilePutByOffset inline | cell-internal (never checked/masked) |
| GetById/PutById/etc. patchpoints | handler ICs (Task 2) via Task-8 emitter chokes |
| megamorphic patchpoints | force-slow flag-on (Task-8 AssemblyHelpers choke) |
| MultiGetByOffset self / proto | CHOKE-R per-case / conservative => OSR exit |
| MultiPutByOffset Replace OOL | CHOKE-W per-variant => OSR exit; Transition RELEASE_ASSERT |
| MultiDeleteByOffset OOL | CHOKE-W per-variant => OSR exit (protocol gap: item 3) |
| tryLoadJSArray / tryStoreJSArray | CHOKE-R/W KnownNonArrayStorage => OSR exit |
| compileSpread / sort compact+commit / enumerator get+put | CHOKE-R/W => slow block or OSR exit |
| Materialize/allocate slow-path re-loads | FRESH-FROM-OPERATION, mask-only |
| allocateObject installs, ArrayValues fast butterflies | FRESH-ALLOC carve-out (gap 2) |
| storageForTransition / allocate+reallocatePropertyStorage / nukeStructureAndSetButterfly | UNREACHABLE flag-on (RELEASE_ASSERT) |

## Known gaps / MUST-FIX follow-ups (Task 13 / integrator)

1. **FTL array-element writes through the storage child lack the per-store
   TID compare (D9)**: PutByVal Int32/Double/Contiguous (`lowStorage`-fed),
   ArrayPush/Pop/ArraySplice and friends store through GetButterfly's masked
   result with no write predicate at the store. EXACT mirror of Task 9 known
   gap 1 (DFG64) — GIL-SOUND ONLY; HARD precondition for GIL removal. Same
   closing recipe: re-load + `threadedButterflyLoadForWrite` at the store.
2. **Allocation tagging** (restated from Tasks 8/9): FTL inline allocations
   install RAW butterflies (tag (0,0)); OM allocation tagging + or-in of the
   R5 tag at install lets the FRESH-ALLOC carve-outs drop.
3. **MultiDeleteByOffset delete protocol**: the inline hit-variant fast path
   (slot zero + structureID store) does not implement the OM's locked
   delete/quarantine protocol; the Task-10 write choke protects the slot
   store's I14 discipline but NOT racing foreign stale readers. GIL-SOUND
   ONLY. Fix = flag-on bail to the generic DeleteById op at graph
   construction (DFGByteCodeParser handleDeleteById), same shape as the
   Task-9 Transition gates; or E4-style gating chartered with OM. MUST land
   before GIL removal.
4. **M1 kill switch**: `useThreadedFTL` still absent (PREP). All Task-10
   flag tests key on `Options::useJSThreads()` alone; AND the
   THREADS-INTEGRATE-marked sites (compileGetButterfly, compilePutByOffset)
   when M1 lands.
5. **R3 dedicated shims remain unreferenced** (Task-9 item 7 updated): the
   FTL routes every predicate failure to OSR exit or an existing generic
   block, so `operationSegmentedButterfly*`/`operationSharedArrayStorage*`
   stay unconsumed after Task 10; expected first consumers are the gap-1
   fixes (both tiers) and LLInt/Baseline generic paths.
6. **Exit-origin audit (Task 13)** now also covers the FTL: GetButterfly /
   PutByOffset emit Checks flag-on; the flag-on single-threaded suite run is
   the audit (same expectation as Task 9 item 6).
7. **CoW stores in `tryStoreJSArray`**: CoW butterflies are JSCellButterfly
   cells; until OM decides their tag policy they carry tag (0,0), so flag-on
   non-main-thread owners exit and reroute generic (recorded; benign).

## Integrator actions (shared files)

* None beyond what Tasks 1-9 recorded. CS5/D9 honored (write TID compare
  never elided); CS6 unaffected; no new options, no Sources.txt entries
  (no new files).

# Task 11 — Watchpoint classification + central Class-A stop (§5.6)

Owned-path changes only: `bytecode/Watchpoint.{h,cpp}` (the §5.6 intercept) and
`bytecode/JSThreadsSafepoint.{h,cpp}` (the App. 5.6(d) watchdog surface). NO new
M1–M5 hunks. M6 is now POPULATED (one entry, below) — per App. 5.6(c),
"populated M6 = specified fallback".

## What landed (owned paths)

* **Classification (I10/P2)**: `enum class WatchpointSetClassification
  { InvalidatesCode /*A*/, DataOnly /*B*/ }` (Watchpoint.h).
  `WatchpointSet`/`WatchpointSet::create` and `InlineWatchpointSet` take a
  classification parameter DEFAULTING to Class A, so every set in the tree —
  including every non-owned `runtime/**` construction — is classified at
  construction with zero call-site edits; the bit is `int8_t
  m_invalidatesCode`, immutable after construction. Thin
  `InlineWatchpointSet`s carry the bit as `ClassBFlag = 8` in `m_data`
  (consulted only when thin; every thin state store goes through
  `setThinState`, which preserves it); `inflateSlow` transfers it to the fat
  set; `WatchpointSet::take` transfers it to a deferred-fire holder.
* **Rare-site per-fire override**: `virtual bool FireDetail::fireIsDataOnly()
  const { return false; }` — a Class-A set can take a no-stop fire for a
  detail that proves the specific fire data-only.
* **Central Class-A protocol** (`WatchpointSet::fireAllSlow(VM&, const
  FireDetail&)`): flag-on, `m_invalidatesCode && !detail.fireIsDataOnly()` ⇒
  `fireAllUnderClassAStop`: (1) `JSThreadsSafepoint::worldIsStopped(vm)` ⇒
  fire inline (I11 state re-check first); (2) else enqueue a stack-allocated
  record on the owned intrusive coalescing queue and call
  `stopTheWorldAndRun`; (3) the draining closure re-checks
  `state() == IsWatched` per entry (I11); (4) runs today's fire body
  (`fireAllNow`, F4 fence pair preserved); (5) watchpoint-driven jettisons run
  in the SAME closure via `CodeBlock::jettison`'s R1.h already-stopped path
  (Task 5); (6) synchronous completion RELEASE_ASSERTed (`serviced` +
  `hasBeenInvalidated`). NO ">1 mutator" gate (G7 addendum). Flag-off:
  byte-identical behavior to today (`fireAllNow` is the old body; one
  not-taken `Options::useJSThreads()` test on the C++ slow path only — no
  emitted-code change, I1).
* **Coalescing (REQUIRED, r10)**: queue entries are enqueued BEFORE the stop
  request; whichever requester stops first drains the WHOLE queue in one stop;
  a loser's entry is serviced inside the winner's stop (loser parks per
  R1.g), and its own closure then drains an empty queue. Drain reads
  `entry->next` before publishing `serviced` (the loser's stack record dies
  at resume). The drain fires each entry with the ENQUEUER's `VM&`. Closure is
  allocation-free (OM O4): intrusive stack nodes, leaf `Lock` held only
  around pointer swaps, never across a fire.
* **Deferral (App. 5.6(a))**: the `fireAllSlow(VM&, DeferredWatchpointFire*)`
  transfer overload is UNCHANGED (invalidate now, transfer watchpoints;
  callers may hold locks). The scope-exit fire
  (`DeferredStructureTransitionWatchpointFire::fireAllSlow` →
  `watchpointsToFire().fireAll(...)`) funnels into the intercepted
  `fireAllSlow` and gets steps 2–6, lock-free by construction.
* **Watchdog (App. 5.6(d))**: `JSThreadsSafepoint::ClassAStopWatchdogContext`
  (RAII, thread-local, nests) published around every Class-A stop request, and
  `JSThreadsSafepoint::watchdogAssertStopProgress(MonotonicTime requestStart)`
  — RELEASE_ASSERT-crashes naming the escaped set (context pointer +
  description) after a generous 30s without a stopped world.
  **THREADS-INTEGRATE(jit) / M4 action**: the real requester-side wait loop
  (replacing the Task-1 stub) MUST call `watchdogAssertStopProgress` on every
  wait iteration; pre-M4 the stub never waits, so the watchdog is dormant by
  construction. Lock-rank counters NOT instrumented (uninstrumentable from
  owned paths, per App. 5.6(d)).

## Class-B opt-in inventory (I10 table)

EMPTY this pass — deliberately conservative. No owned-path construction site
was provably data-only (every candidate set can carry code-invalidating
watchpoints via adaptive/jettisoning watchpoint types). All sets therefore
default Class A, which is sound (worst case: an unnecessary stop). Future
opt-ins go through the constructor parameter and must be recorded here.

## Constructor lint (I10)

Structural: classification is a constructor parameter with default A — a set
CANNOT be constructed unclassified. Grep lint for reviews:
`grep -rn "WatchpointSetClassification::DataOnly" Source/JavaScriptCore/`
must yield only sites listed in the inventory above (currently: none outside
Watchpoint.{h,cpp} plumbing). Per-FIRE overrides:
`grep -rn "fireIsDataOnly" Source/JavaScriptCore/` must yield only
Watchpoint.{h,cpp} (currently true).

## Direct-fire audit (App. 5.6(c)); buckets: (i) lock-free, (ii) world-already-stopped, (iii) §7/cell-lock-holding ⇒ M6

Methodology: grep-enumeration of every direct `fireAll`/`fireAllSlow`/
`invalidate`/`touch` caller on WatchpointSet/InlineWatchpointSet (the
`isWatchableWhenValid(EnsureWatchability)` chain included), then caller-context
inspection. "Lock-free" = w.r.t. every SPEC-jit §7 lock and every cell lock
(non-§7 locks like `SymbolTable::m_lock` are permitted; ConcurrentJSLock
holders never span poll sites).

| Fire site (file:line) | Path | Bucket |
|---|---|---|
| runtime/JSGlobalObject.cpp:2518 (varReadOnly), 2604 (arrayBufferDetach), 2888 (havingABadTime), 2897 (structureCacheCleared) | mutator slow paths / global reconfiguration | (i) |
| runtime/JSGlobalObject.cpp:3374, 3442, 3563, 3600, 3605, 3673, 3741 (species/iterator-protocol/propertyDescriptor setup `invalidate`/`touch`) | main-thread lazy setup | (i) |
| dfg/DFGDesiredGlobalProperties.cpp:52 | compile finalize, main thread; fires AFTER the inner `symbolTable->m_lock` scope closes | (i) |
| jit/GCAwareJITStubRoutine.cpp:140 (`PolymorphicAccessJITStubRoutine::invalidate`) | no live in-tree caller found this pass; any future caller must be lock-free or world-stopped | (i)/(ii) note |
| bytecode/InternalFunctionAllocationProfile.h:69; bytecode/ObjectAllocationProfileInlines.h:141; runtime/FunctionRareData.cpp:137 + .h:105 | allocation-profile clears, mutator | (i) |
| interpreter/Interpreter.cpp:1511 (varInjection on eval) | mutator | (i) |
| runtime/ProgramExecutable.cpp:272 | program initialization, mutator | (i) |
| runtime/VM.cpp:1371 (`addImpureProperty`) | mutator | (i) |
| runtime/VM.cpp:680, 1780 (`m_primitiveGigacageEnabled.fireAll`) | :680 VM-teardown path; :1780 `primitiveGigacageDisabledCallback` may run on a NON-MUTATOR thread | (i) + REVIEW note: flag-on, :1780 would trip the stub's API-lock RELEASE_ASSERT (fail-fast, correct). Expected unreachable in Bun (gigacage disable is startup-time, pre-threads). If ever reachable, convert to a deferred per-VM fire — candidate M6 entry, NOT included now |
| runtime/ArrayBuffer.cpp:538 (detach) | mutator | (i) |
| runtime/Structure.cpp:1536-1538 (TTL sets, `fireThreadLocalSetsWithChainUnderStop`) | `ASSERT(butterflyWorldIsStopped(vm))` — fires take protocol branch (1) inline, per "§5.6 TTL fires assert branch 1" | (ii) |
| runtime/Structure.cpp:1929 (transition set, deferred form) | deferred transfer (lock-holding OK by design); scope-exit fire at Structure.cpp:2317 is lock-free. **Review round 1: lock-freedom of the FIRE is not sufficient — the deferring caller publishes the watched-fact change BEFORE the stop. GIL-sound only; see GIL-removal precondition 10 + the caveat at Watchpoint.cpp's deferred overload. This audit gains a mandatory "fact published before fire?" column before GIL removal.** | (i) via deferral — ORDERING-CAVEATED |
| runtime/Structure.cpp:1931 (transition set, direct form) | non-deferred `didTransitionFromThisStructure(nullptr)` callers (e.g. dictionary flatten paths) assumed lock-free today; the watchdog names this set if one escapes holding a cell lock | (i) + watchdog backstop |
| runtime/Structure.cpp:2317 (`DeferredStructureTransitionWatchpointFire::fireAllSlow`) | scope-exit, lock-free by construction (App. 5.6(a)) | (i) |
| runtime/InternalFunction.cpp:185; runtime/RegExpPrototype.cpp:246 | mutator | (i) |
| runtime/ScopedArguments.h:115; bytecode/VariableWriteFireDetail.cpp:46; dfg/DFGOperations.cpp:4804; jit/JITOperations.cpp:4767; llint/LLIntSlowPaths.cpp:2528 (notifyWrite `touch`) | mutator write slow paths | (i) |
| llint/LLIntSlowPaths.cpp:807, 861, 1001, 1235-1236, 1432, 1569-1570 (sharedPolyProtoWatchpoint `invalidate`) | LLInt slow paths | (i) |
| bytecode/CodeBlock.cpp:657 (ResolvedClosureVar `invalidate` at link) | CodeBlock setup, mutator, no m_lock | (i) |
| runtime/SymbolTable.h:293 (`disableWatching`) | callers hold at most `SymbolTable::m_lock` (non-§7) | (i) note |
| runtime/InferredValue.h notifyWriteSlow → `invalidate` | mutator writes | (i) |
| bytecode/InlineCacheCompiler.h:101 (`AccessGenerationResult::fireWatchpoints`) | called from Repatch.cpp `fireWatchpointsAndClearStubIfNeeded` OUTSIDE the GCSafeConcurrentJSLocker (verified Repatch.cpp:367-377) | (i) |
| bytecode/{LLIntPrototypeLoadAdaptiveStructureWatchpoint.cpp:76, AdaptiveInferredPropertyValueWatchpointBase.cpp:76, PropertyInlineCacheClearingWatchpoint.cpp:65}; dfg/DFGAdaptiveStructureWatchpoint.cpp:72 (re-watch `EnsureWatchability` from `fireInternal`) | reached only DURING a fire ⇒ world already stopped under the Class-A protocol; nested fires take branch (1) | (ii) |
| **runtime/Structure.cpp:1660 (`firePropertyReplacementWatchpointSet`) reached from bytecode/InlineCacheCompiler.cpp:3271 (`collectConditions`, EnsureWatchability) and bytecode/AccessCase.cpp:1154 (`couldStillSucceed`, EnsureWatchability)** | **runs UNDER `CodeBlock::m_lock`**: both run inside `PropertyInlineCache::addAccessCase(const GCSafeConcurrentJSLocker&, ...)` (PropertyInlineCache.cpp:216 → compileHandler/compile under the locker) | **(iii) ⇒ M6 (entry below)** — this EnsureWatchability/addAccessCase reacher remains on the M6 deferral plan; it is NOT closed by the Repatch.cpp hoist below |
| runtime/StructureInlines.h:265 (`didCachePropertyReplacement`) reached from bytecode/Repatch.cpp `tryCachePutBy` ExistingProperty branch — **row added by the STW-watchdog bughunt (H1-lockheld-classA-fire-repatch-1360): this reacher was MISSING from the audit** | WAS bucket (iii): ran under `CodeBlock::m_lock` via the :1238 `GCSafeConcurrentJSLocker`, wedging the stop protocol (requester parks holding m_lock; lock-waiters hold heap access; conductor predicate starves; 30s watchdog abort). NOW hoisted BEFORE the locker (flag-on only); fire completes before the Replace IC is published; the in-lock residue check refuses to cache against a still-watched set. M6.1 ordering-caveat class: N/A — site remains in the synchronous fire-precedes-publication regime (the Experiment-A deferral shape was rejected for landing: it publishes the IC under the lock before the scope-exit fire, failing caveat classes (a), (b) and (c) under N mutators). Sibling callers re-audited lock-free this round: LLIntSlowPaths.cpp:1384/1398/1705, CommonSlowPathsInlines.h:105, JITOperations.cpp:1277/2135. | (i) |

| dfg/DFGCommonData.cpp (`CommonData::installVMTrapBreakpoints`) → dfg/DFGJumpReplacement.cpp (`installVMTrapBreakpoint`) — **row added review round 4 (R4-3)**: not a watchpoint fire, but the only OTHER writer of reachable invalidation points; runs on the VMTraps SIGNAL-SENDER thread (async cross-thread patching = I2 violation flag-on) | guarded in tree: `RELEASE_ASSERT(!Options::useJSThreads() OR !signal-traps)` — i.e. requires `usePollingTraps` — at BOTH entry points; structurally closed when M2b's `usePollingTraps` force-set lands | fail-fast until M2b |

Also audited per §5.7 rule 6 boundary: `computeFor*` Status snapshot lockers are
Task 12's audit, not repeated here.

## M6 — runtime/** deferred-fire conversion [NOW POPULATED: 1 entry]

**M6.1 — `Structure::firePropertyReplacementWatchpointSet` must accept a
deferred fire** (bucket-(iii) site above; without this, a flag-on IC
regeneration that needs to fire a property-replacement set would request a
stop while holding `CodeBlock::m_lock` — forbidden by §7; pre-M4 the interim
stub runs it inline so single-threaded flag-on still works, post-M4 the
watchdog would crash naming the set, by design).

File: `Source/JavaScriptCore/runtime/Structure.h` — replace the declaration

```cpp
    WatchpointSet* firePropertyReplacementWatchpointSet(VM&, PropertyOffset, const char* reason);
```

with

```cpp
    // SPEC-jit §5.6 / M6.1: when `deferred` is non-null the set is invalidated
    // immediately but its watchpoints FIRE at the deferred holder's scope exit
    // (lock-free), where the Class-A stop protocol runs. Pass a deferred fire
    // from any caller that may hold CodeBlock::m_lock or a cell lock.
    WatchpointSet* firePropertyReplacementWatchpointSet(VM&, PropertyOffset, const char* reason, DeferredWatchpointFire* deferred = nullptr);
```

File: `Source/JavaScriptCore/runtime/Structure.cpp` — in the body at :1653-1666,
replace

```cpp
    if (watchpointSet && watchpointSet->state() == IsWatched) {
        StructureRareData* rareData = structure->rareData();
        watchpointSet->fireAll(vm, reason);
```

with

```cpp
    if (watchpointSet && watchpointSet->state() == IsWatched) {
        StructureRareData* rareData = structure->rareData();
        if (deferred)
            watchpointSet->fireAllSlow(vm, deferred); // Invalidates now; fires at scope exit (SPEC-jit §5.6 deferral).
        else
            watchpointSet->fireAll(vm, reason);
```

Owned-side consumers (jit applies in `bytecode/` WHEN M6.1 lands; recorded here
so the change is one atomic step): thread a `DeferredWatchpointFire*` through
`PropertyCondition::isWatchableWhenValid/isWatchable*(EnsureWatchability)`
(bytecode/PropertyCondition.{h,cpp}, owned) into the :414 call; carriers are
one `DeferredWatchpointFire` per fired set owned by `AccessGenerationResult`
(a `DeferredWatchpointFire` holder takes exactly ONE set —
`takeWatchpointsToFire` asserts ClearWatchpoint — so the carrier is a small
inline Vector of holders), destroyed (= fired) in Repatch.cpp
`fireWatchpointsAndClearStubIfNeeded` after the locker scope, alongside the
existing `result.fireWatchpoints(vm)`. Interim owned mitigation if M6.1 is
rejected: those two owned call sites pass `MakeNoChanges` under
`Options::useJSThreads()` (conditions become unwatchable ⇒ IC gives up;
sound, slower).

## Notes for later tasks

* Task 13 (fires/sec + N-thread warmup stop-budget bench): stop counts are
  bounded by the coalescing queue (one stop drains all queued fires) + OM F4
  chain-fires (Structure fires whole chains in one stop).
* Task 13 fire-vs-execute integration-gate stress: hammer
  `$vm`-triggered Class-A fires against running mutators; assert the
  RELEASE_ASSERTs in `fireAllUnderClassAStop` (serviced + invalidated) and the
  watchdog stays quiet.
* Task 14 / M4: delete nothing here; wire `watchdogAssertStopProgress` into
  the real wait loop (see THREADS-INTEGRATE(jit) marker in
  JSThreadsSafepoint.h) and re-confirm the M6.1 conversion landed before
  enabling multi-mutator runs.

# Task 12 — Racy profiling + tier-up serialization (§5.7)

## What landed (owned paths only)

* **§5.7.1 — execution/exit counters, relaxed-atomic C++ accesses**
  - `bytecode/ExecutionCounter.h`: new `counterValueConcurrently()` /
    `storeCounterValueConcurrently()` (relaxed `WTF::atomicLoad/atomicStore` on the
    JIT-shared `m_counter` word; layout untouched — `int32_t m_counter` stays public at
    the same offset so JIT'd/LLInt fast-path adds remain plain, per §5.7.1).
    `count()` reads through the helper.
  - `bytecode/ExecutionCounter.cpp`: every C++ touch of `m_counter`
    (`forceSlowPathConcurrently`, `deferIndefinitely`, `hasCrossedThreshold`,
    `setThreshold`, `reset`, `dump`) routed through the helpers; `m_totalCount` /
    `m_activeThreshold` stay plain per §5.7.7 (word-sized, advisory) with
    `SUPPRESS_TSAN` on the writers. Covers G8's LLInt tier-up counter on the shared
    `UnlinkedCodeBlock` too (all flows go through `ExecutionCounter` methods).
  - `bytecode/CodeBlock.h`: `countOSRExit()` is now a relaxed
    `WTF::atomicExchangeAdd` (C++ OSR-exit paths on any of N mutators; JIT'd bumps via
    `offsetOfOSRExitCounter` stay plain adds).

* **§5.7.2 — tier-up CAS (unconditional; single-mutator CAS always wins, so flag-off
  behavior is unchanged; I1 concerns emitted code only)**
  - `bytecode/CodeBlock.h`: `enum class TierUpEdge : uint8_t { LLIntToBaseline,
    BaselineToDFG, DFGToFTL, DFGToFTLForOSREntry }` + `tryBeginTierUp()` (0->1
    `compare_exchange_strong`, acq_rel) / `endTierUp()` (release 0 store) + RAII
    `CodeBlock::TierUpEdgeLocker`. Backing store: `std::atomic<uint8_t>
    m_tierUpInFlight[4]`, placed in the 32-bit member cluster right after `m_hash`
    (packs into padding; the `sizeof(CodeBlock) <= 224` assert at CodeBlock.h:~1116
    is the canary — if a future member shuffle makes this overflow, move the array,
    do not bump the assert).
  - Latch discipline: the latch is scoped to the threshold slow path's
    create/enqueue window (won -> newReplacement()+compile()/enqueue -> released at
    scope exit). Losers defer (`CompilationDeferred` threshold reset / `jitSoon()`)
    and stay in tier. Re-entries after release see the worklist `Compiling` state and
    defer as today; the §5.7.3 dedup backstop covers the latch-free window.
  - Sites: `jit/JITOperations.cpp` `operationOptimize` (BaselineToDFG, guarding the
    `newReplacement()+DFG::compile` else-branch); `llint/LLIntSlowPaths.cpp`
    `jitCompileAndSetHeuristics` (LLIntToBaseline, guarding the BaselineJITPlan
    enqueue); `dfg/DFGOperations.cpp` `triggerFTLReplacementCompile` (DFGToFTL,
    guarding the FTL replacement `compile()`; covers `operationTriggerTierUpNow`,
    `operationTriggerTierUpNowInLoop`, and `tierUpCommon`'s replacement path) and
    `tierUpCommon` (DFGToFTLForOSREntry, guarding the
    `reconstruct()+newReplacement()+compile(FTLForOSREntry)` tail). Nested
    DFGToFTLForOSREntry->DFGToFTL acquisition is fine (distinct bytes, CAS not a lock).

* **§5.7.3 — worklist dedup backstop (NOT flag-gated)**
  - `jit/JITWorklist.cpp` `enqueue`: the `ASSERT(m_plans.find(plan->key()) ==
    m_plans.end())` (old :187) is replaced by: key present under `*m_lock` =>
    `plan->cancel()` (which ends the signpost and rebalances
    `numberOfActiveJITPlans`) => return `CompilationResult::CompilationDeferred`.
    Header doc on `enqueue` in `jit/JITWorklist.h`. All plan admission funnels
    through this (`dfg/DFGDriver.cpp:95`, LLInt baseline enqueue), so `m_totalLoad`
    / queue accounting can no longer be corrupted by racy duplicate keys.

* **§5.7.4 — ValueProfile bucket tolerance**
  - `bytecode/ValueProfile.h`: `loadBucketConcurrently()` / `storeBucketConcurrently()`
    on `ValueProfileBase` — JSVALUE64: relaxed atomic 64-bit ops (TSAN-clean);
    JSVALUE32_64: the existing JSCJSValue.h tag/payload protocol. All in-class bucket
    accesses (`clearBuckets`, `classInfo`, `numberOfSamples`,
    `computeUpdatedPrediction`, `dump`) converted. `static_assert`s pin 8-byte size +
    alignment of the bucket array on JSVALUE64 (I12 word-atomicity basis).
  - C++ bucket writers converted to `storeBucketConcurrently`:
    `llint/LLIntSlowPaths.cpp` (LLINT_PROFILE_VALUE macro + iterator_open/iterator_next
    profiles + instanceof profiles + op_catch buffer refill) and
    `jit/JITOperations.cpp` (`operationTryOSREnterAtCatchAndValueProfile`).
  - JIT'd/LLInt-asm bucket stores stay plain per §5.7.1/§5.7.4 (aligned 8B stores,
    word-atomic at ISA level; invisible to the TSAN no-JIT config).

* **§5.7.5 — ArrayProfile flag/mode merges**
  - `bytecode/ArrayProfile.h`: `observeArrayMode` = relaxed `WTF::atomicExchangeOr`;
    new `addArrayProfileFlagsConcurrently()` (relaxed OR on the `OptionSet` storage,
    `static_assert`ed 4 bytes) used by `setOutOfBounds`, `setMayBeLargeTypedArray`;
    `UnlinkedArrayProfile::update` (shared UnlinkedCodeBlock!) rewritten as relaxed
    monotone ORs both directions + word-atomic last-writer-wins unlinked-flag
    snapshot. Racy plain readers (`observedArrayModes(locker)`, the `contains`-based
    getters) annotated `SUPPRESS_TSAN` with the tolerance rationale.
  - `bytecode/ArrayProfile.cpp`: `computeUpdatedPrediction` merges via relaxed ORs;
    first-run pruning overwrite is a relaxed word store (heuristic, benign);
    structure-ID words stay plain per §5.7.7 (4B aligned, advisory; racing
    `std::exchange` at worst re-merges/drops one observation).

## §5.7.6 Status locker audit (verify only — NO code changes; all entries PASS)

Every multi-word IC/Status snapshot entry point takes `ConcurrentJSLocker` on the
owning CodeBlock's `m_lock` before walking IC state; IC-state writers also hold
`m_lock` (§5.1, landed Task 3/4):

| computeFor* entry | Locker | Verdict |
|---|---|---|
| `CallLinkStatus.cpp:59,103,327` (`computeFor`/`computeExitSiteData`/DFG-block loop) | `m_lock` of profiled/optimized block | PASS |
| `GetByStatus.cpp:190-197` (+ DFG-block variant :440 ff., locker on `context->optimizedCodeBlock`) | yes | PASS |
| `PutByStatus.cpp:143-145,354` | yes | PASS |
| `InByStatus.cpp:54-56,101` | yes | PASS |
| `DeleteByStatus.cpp:51-53,176` | yes | PASS |
| `CheckPrivateBrandStatus.cpp:52-54,156` | yes | PASS |
| `SetPrivateBrandStatus.cpp:52-54,161` | yes | PASS |
| `*::computeFromLLInt` (GetByStatus.cpp:56, PutByStatus.cpp:55) | none needed: post-Task-6 LLInt metadata reads are single aligned u64 (§4.3/I13) | PASS |

DFG->FTL trigger-map tolerance (verified, no change needed): `DFGJITCode`'s
`tierUpEntryTriggers` / `tierUpInLoopHierarchy` hash maps are fully key-populated at
compile finalization (`dfg/DFGJITCompiler.cpp:62-64`); every runtime mutation
(`DFGOperations.cpp` trigger sets, `DFGJITCode.cpp:418`,
`ToFTLForOSREntryDeferredCompilationCallback.cpp:72`) is a value overwrite on an
existing key — no rehash, word-sized enum value, last-writer-wins benign (I12).

## Known gaps / notes

* `Options::useConcurrentJIT()==false` + N mutators: `JITWorklist::enqueue` compiles
  synchronously and bypasses both `m_plans` and the dedup backstop. The per-linked-
  CodeBlock CAS still serializes per-CodeBlock, but two *distinct* linked CodeBlocks
  sharing one UnlinkedCodeBlock could sync-compile Baseline twice and race the
  `m_unlinkedBaselineCode` install. Flag-on therefore requires `useConcurrentJIT`
  (default-on); Task 13 should assert `Options::useJSThreads() =>
  Options::useConcurrentJIT()` in the validation pass, or M2b may force it
  (integrator's pick; recorded here, NOT applied).
* `ExecutionCounter` layout is unchanged (m_counter offset 0): no I1 golden-diff
  impact; all changes are C++-side only.
* `CodeBlock` gains 4 bytes of atomics packed after `m_hash`; if
  `static_assert(sizeof(CodeBlock) <= 224)` ever fires after a rebase, repack —
  do not raise the cap.

## Integrator action (shared file; ready-to-paste, OPTIONAL but recommended)

`runtime/CommonSlowPaths.cpp:153-155` — the one non-owned C++ ValueProfile bucket
writer. Plain aligned 64-bit store is already word-atomic (sound under I12); convert
for TSAN cleanliness. Replace:

```cpp
#define PROFILE_VALUE_IN(value, profileName) do { \
        codeBlock->valueProfileForOffset(bytecode.profileName).m_buckets[0] = JSValue::encode(value); \
    } while (false)
```

with:

```cpp
#define PROFILE_VALUE_IN(value, profileName) do { \
        codeBlock->valueProfileForOffset(bytecode.profileName).storeBucketConcurrently(0, JSValue::encode(value)); /* THREADS §5.7.4 */ \
    } while (false)
```

# Task 13 — Validation + tests (I1/I6/I14/I16/I19/I20), owned JSTests/threads/jit/**

**No new shared-file (M1–M6) hunks.** Everything landed under
`JSTests/threads/jit/**` (owned). This section records the gate matrix, the
prep preconditions each gate depends on, the budget-miss consequences, and
two REQUESTED (not required) integration-time hooks.

## What landed (owned paths)

* `run-jit-tests.sh` — driver (lints → functional flag-on → tag-discipline →
  integration-gate stresses (smoke|FULL) → bench gates → golden disasm). It
  PROBES jsc for prep-precondition options and prints loud SKIP lines when
  one is missing rather than failing the suite.
* `lint.sh` — static lints, all green on this tree at handoff:
  - I13 (LLInt metadata publication confined + gate-count floor),
  - I18 (Unset/ProtoLoad asserts present),
  - I14 choke-point lint (LLInt `m_butterfly` count pinned at 15; raw
    `butterflyOffset()` loads confined to chokes + the two AUDITED flag-off
    residues: `bytecode/InlineAccess.cpp` (I3-unreachable flag-on, count<=5)
    and `jit/JITPropertyAccess.cpp` enumerator-put flag-off else-branch
    (count<=1); legacy-trap presence),
  - §5.5 PA-transition lint (current form: transition machinery fail-fast
    asserts + Repatch GiveUpOnCache gates present. WHEN OM's E4 lands and the
    Repatch gates are replaced by real transition emission, REPLACE this lint
    with one requiring the PA bit-test (`cell & 8`) adjacent to every emitted
    transition predicate — marked in the script),
  - Task-8 gap-8 private-name/brand publication gates,
  - I10 (Class-B opt-ins + fireIsDataOnly overrides confined to the recorded
    inventory — currently none),
  - I21/I16 structural checks (CheckTraps/InvalidationPoint lowerings present;
    IC emitters poll-free).
* `golden-disasm.sh` + `golden-disasm-corpus.js` — I1 golden disassembly
  (flag-off, deterministic config, normalized; `--record` blessing flow).
  I1 carve-outs that REQUIRE re-recording in the same change: D7 repack
  offset-immediate moves (§4.2/§4.3/§5.8). The §5.4 LLInt gate branch is NOT
  visible to --dumpDisassembly (asm interpreter); its flag-off cost is gated
  by the bench gate below.
* `bench-gates.sh` — the r12 split:
  - G1 LLINT-I1: flag-off `--useJIT=0` vs recorded baseline
    (`jit/baselines/llint-bench-<arch>.txt`, `--record-llint` on the blessed
    pre-change build; 1% per-bench threshold, mirroring Tools/threads/bench-gate.sh).
  - G2 FLAGON-1-0: geomean(flag-on/flag-off) over `JSTests/threads/bench/`
    **GATED <= 1.05** in {useJSThreads=1, useSharedGCHeap=0}. **Miss ⇒ the
    §4.3 LLInt-cache revival is REQUIRED pre-ship** (proto/transition caches
    as immutable single-pointer records, §5.8 pattern) — the script prints
    this consequence on failure.
  - R1 FLAGON-1-1: MEASURED+RECORDED, never gated; SKIPs loudly until heap's
    `--useSharedGCHeap` option exists (heap manifest 2).
  - R2/R3: `fires-per-sec.js` (Class-A fire throughput) and
    `construction-shared-constructor.js` (shared-constructor microbench,
    pre-share vs post-share; flag-on/flag-off recorded both — the OM 8h
    Task-14 promotion input, DECIDED PRE-INT) recorded in both configs.
* Functional tests (flag-on, GIL-interleaved today):
  - `ic-publish-reset-loops.js` — I6 packed-word flip under readers (two
    shapes, `f` at different offsets, poison neighbors so a torn id/offset
    surfaces as a wrong value) + §5.1/§4.4 publish/reset churn
    (toCacheableDictionary ⇒ resetStubAsJumpInAccess ⇒ retireHandlerChain).
  - `spawned-thread-butterfly-stress.js` — I14 owner transitions on spawned
    threads (non-zero R5 tags), foreign reads, foreign writes (SW path),
    checksummed.
  - `shared-arraystorage-stress.js` — I20: SW=1 AS butterflies + owner
    shift/unshift relayout (AS-COPY) + sparse holes under N writers;
    disjoint-stripe no-lost-elements oracle.
  - `tag-discipline.js` — drives every §5.5 emission family through all
    tiers under `--validateButterflyTagDiscipline=1` (+ poll placement, I21)
    and doubles as the Tasks-9/10 exit-origin audit vehicle.
* Integration-gate stresses (smoke by default, FULL with `-- int-gate` /
  `run-jit-tests.sh --int-gate`; **re-run FULL at M4/CS2**; they validate the
  N-separate-VMs config ONLY — N threads in ONE VM is the Phase-B charter):
  - `int-gate-jettison-vs-execute.js` (OSR-exit storms + haveABadTime +
    delete-all-code vs hot workers; self-checking results),
  - `int-gate-fire-vs-execute.js` (replacement/transition/badtime Class-A
    fires vs prototype-load workers; monotonic-generation oracle pins the
    "fire completes synchronously" semantics, history §13.5),
  - `int-gate-direct-call-relink.js` (mono upgrade/poly republish/virtual/
    GC-unlink churn vs callers; callee-identity decode catches torn records),
  - `int-gate-epoch-reclaim.js` (PRE-INT: retire→legacy-GC→free variant runs
    by default — live iff heap's GCSafepointEpoch landed (N6), leak-checks
    the stub otherwise; FULL: retire→safepoint→refcount/free incl.
    parked-in-slow-path dispatchers, §4.4(b)),
  - `int-gate-stop-budget.js` (N-thread warmup wall-time proxy + the $vm
    counter hooks when present; STOPBUDGET lines for INTEGRATE sign-off).

## Prep/integration dependencies consumed by this task (no new hunks; restated)

1. **M1 options** (`validateButterflyTagDiscipline`,
   `useJSThreadsUnlockHandlerICInFTL`, kill switches): absent in-tree ⇒
   run-jit-tests.sh SKIPs the tag-discipline and FTL legs. Apply the M1 hunk
   (top of this file) to un-skip.
2. **M2a** (FTL force-disable gate): required before the FTL handler-IC leg
   runs flag-on.
3. **heap `--useSharedGCHeap`** (heap manifest 2): required for the {1,1}
   recorded leg; G2 stays runnable without it (the config is implicitly
   {*, 0}).
4. **heap `GCSafepointEpoch.h` BEFORE the epoch tests mean anything** (N6
   ordering, restated from Tasks 1/3): until then `int-gate-epoch-reclaim.js`
   exercises the leak-until-INT stub (sound; ASan run recommended after the
   heap lands to re-validate frees).
5. **Task-12 invariant**: `useJSThreads ⇒ useConcurrentJIT`. The driver
   probes for the unsound combination and prints a NOTE; the integrator
   should fold the rejection into M2b (recorded at Task 12).

## REQUESTED $vm hooks (integration-time, NON-blocking; tools/JSDollarVM.cpp is not jit-owned)

For the stop-budget sign-off numbers (stop COUNT + total STOPPED-TIME ceiling,
§5.6 coalescing evidence) the integrator is asked to add, at M4:

```cpp
// tools/JSDollarVM.cpp (with the usual functionPrologue/epilogue):
// $vm.jsThreadsStopCount()    -> number of JSThreads STW stops since startup
// $vm.jsThreadsStoppedMillis() -> cumulative world-stopped milliseconds
```

backed by two relaxed `std::atomic<uint64_t>` counters bumped in
`JSThreadsSafepoint::stopTheWorldAndRun`'s real (post-M4) stop path — the
counters themselves live in owned `bytecode/JSThreadsSafepoint.{h,cpp}` and
will be added when M4 lands (exported accessors
`JSThreadsSafepoint::stopCountForTesting()` / `stoppedMillisForTesting()`).
`int-gate-stop-budget.js` already consumes the hooks when present and prints
`STOPBUDGET stop-count UNAVAILABLE` otherwise.

## Gate→milestone map (for the integrator's checklist)

| Gate | Phase | Blocking? |
|---|---|---|
| lint.sh | every PR touching owned paths | yes |
| golden-disasm (flag-off) | pre-INT, after any emission change | yes (re-record only for D7 carve-outs) |
| G1 LLINT-I1 (`--useJIT=0`) | pre-INT | yes |
| G2 FLAGON-1-0 geomean <= 5% | pre-INT | yes; miss ⇒ §4.3 revival REQUIRED pre-ship |
| R1 FLAGON-1-1 | pre-INT once heap option exists | record only |
| R2 fires/sec, R3 construction | pre-INT | record only (R3 feeds OM 8h decision PRE-INT) |
| int-gate-* FULL | M4/CS2 | yes (integration gate; GIL-removal precondition list also includes the Task-7 LLInt call record, Task-9/10 gap-1 array-store predicates, Task-10 MultiDeleteByOffset gate — all recorded above) |
| stop-budget ceiling | INTEGRATE sign-off | yes (set the ceiling when first FULL numbers exist) |

---

# Task 14 — Manifest handoff (FINAL)

This section is the AUTHORITATIVE consolidated manifest. Where a hunk below
differs from an earlier per-task section, THIS section wins (earlier sections
remain as rationale/history). Every hunk was re-verified against the shared
tree at handoff (grep evidence noted inline). The jit workstream's Tasks 1–13
touched ONLY the owned paths (`Source/JavaScriptCore/{jit,dfg,ftl,bytecode,llint}/**`,
`JSTests/threads/jit/**`, this file) plus the spec-permitted ADDITIVE-ONLY R5
emitters in `assembler/{X86Assembler.h,MacroAssemblerX86_64.h,ARM64Assembler.h,
MacroAssemblerARM64.h}` (SPEC-jit "Owned" clause, App. R5 list only). No other
shared file was modified by this workstream; everything it needs from shared
files is below.

## Shared-tree state observed at handoff (2026-06-05)

| Item | In tree? | Evidence |
|---|---|---|
| `useJSThreads` + `useThreads` alias | YES | OptionsList.h:681,685 |
| M1 remainder (kill switches, `validateButterflyTagDiscipline`, `useJSThreadsUnlockHandlerICInFTL`) | NO | grep empty |
| M2a gate on the FTL force-disable | NO | Options.cpp:814 still unconditional |
| M3 Sources.txt entries (3 TUs) | NO | grep empty — **tree does not link with jit workstream applied until M3 lands** |
| M4a (Darwin TLS-key slot / gate byte) | NO | JSCConfig.h has `OptionsStorage options` (line 104) but no key slot |
| heap `Heap::JSThreadsStopScope` + `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` (CS2) | YES | Heap.h:77,483 — R1.i bracket is LIVE (Task 5) |
| heap `GCSafepointEpoch.h` (R4/N6) | YES | file present — RetiredJITArtifacts real bodies compile; epoch tests meaningful |
| heap `useSharedGCHeap` option | NO (referenced by Heap.h, absent from OptionsList.h) | heap manifest 2 still pending; Task-13 {1,1} leg SKIPs |
| OM `ConcurrentButterfly.h` + TTL sets + `Structure::m_transitionThreadLocalTID` | YES | Structure.h:797,863 — Task-8 item 9 / Task-9 E4 revisit is now UNBLOCKED (post-handoff work) |
| OM stub witness + macro (CS6) | YES | ConcurrentButterfly.h defines `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS` and delegates its veneer to STWR |
| vmstate `VMLite.h` (CS3 hook) | YES | file present — P5 init registers `setVMLiteTIDTagHook` for real |

## M1 — runtime/OptionsList.h [FINAL; PREP, still absent]

Insert directly after the existing `useJSThreads` entry (line 681), keeping the
`useThreads` alias untouched:

```
    v(Bool, useThreadedLLIntICs, true, Normal, "kill switch: threaded LLInt metadata caches under useJSThreads"_s) \
    v(Bool, useThreadedBaselineICs, true, Normal, "kill switch: threaded Baseline/stub ICs under useJSThreads"_s) \
    v(Bool, useThreadedDFG, true, Normal, "kill switch: threaded DFG support under useJSThreads"_s) \
    v(Bool, useThreadedFTL, true, Normal, "kill switch: threaded FTL support under useJSThreads"_s) \
    v(Bool, validateButterflyTagDiscipline, false, Normal, "validate that every generated butterfly access masks or proves the tag (SPEC-jit I14)"_s) \
    v(Bool, useJSThreadsUnlockHandlerICInFTL, false, Normal, "unlock the FTL handler-IC force-disable for bring-up (SPEC-jit M2a)"_s) \
```

Kill-switch wiring points already in owned code (AND-in when M1 lands; all
marked `THREADS-INTEGRATE(jit)` at the site):
* `useThreadedLLIntICs` → `useThreadedLLIntPropertyCaches()` body
  (llint/LLIntSlowPaths.cpp, top of file) becomes
  `Options::useJSThreads() && Options::useThreadedLLIntICs()`.
* `useThreadedDFG` → `compileGetButterfly` / `compilePutByOffset` flag tests
  (dfg/DFGSpeculativeJIT.cpp).
* `useThreadedFTL` → same two sites in ftl/FTLLowerDFGToB3.cpp.
* `useThreadedBaselineICs` → CCallHelpers choke-point flag test
  (jit/CCallHelpers.cpp).
Conservative either way: leaving any site on the master flag only forfeits the
kill switch, never soundness.

## M2 — runtime/Options.cpp [FINAL]

**M2a [PREP]** — gate the force-disable at Options.cpp:814. Replace:

```cpp
    Options::useHandlerICInFTL() = false; // Currently, it is not completed. Disable forcefully.
```

with:

```cpp
    if (!Options::useJSThreadsUnlockHandlerICInFTL())
        Options::useHandlerICInFTL() = false; // Currently, it is not completed. Disable forcefully.
```

**M2b [HANDOFF — apply when flag-on is first enabled; FINAL consolidated text,
supersedes the Task-1 M2b draft].** In the same options-finalization function,
AFTER the M2a hunk (order matters: M2b's force-set must win), before
`Config::finalize()`:

```cpp
    if (Options::useJSThreads()) {
        // SPEC-jit M2b.
        Options::useHandlerICInFTL() = true;   // §5.2/D1: FTL must not patch property-IC code in place.
        Options::usePollingTraps() = true;     // I21: cooperative polls only; async breakpoint patching = I2 violation.
        Options::useConcurrentJIT() = true;    // Task 12: sync-compile bypasses the JITWorklist dedup backstop (§5.7.3).

        // D8 / Task-8 item 6: flag-on requires 64-bit pointers and a JIT-visible TLS
        // mechanism for the R5 tag (ELF initial-exec TLS on Linux x86-64/arm64, or
        // Darwin FAST_TLS via the M4a key slot).
#if !CPU(ADDRESS64) || !(OS(LINUX) && (CPU(X86_64) || CPU(ARM64))) && !(OS(DARWIN) && ENABLE(FAST_TLS_JIT))
        { dataLogLn("useJSThreads is unsupported on this platform (SPEC-jit D8)"); CRASH(); }
#endif
        // Task-8 item 5: LLInt/Baseline threaded paths replace the JSValue-cage step
        // with the tag mask. NOTE (review round 1): the JSValue gigacage DOES NOT
        // EXIST in this tree (Gigacage::Kind has only Primitive,
        // Source/bmalloc/bmalloc/Gigacage.h) - it was removed upstream years ago -
        // so there is nothing to assert against and the original
        // `RELEASE_ASSERT(!Gigacage::isEnabled(Gigacage::JSValue))` hunk would not
        // compile. The Task-8 item-5 concern is vacuously satisfied: butterflies
        // are not Primitive-caged, and the LLInt threaded paths never feed
        // Primitive-caged storage (typed-array vectors) through the tag-mask path
        // (typed-array accesses keep their own cage discipline; see the Task-8
        // MASK rows). No assert is emitted here on purpose.
    }
    // Spec'd invariant check (defense against a later pass re-disabling it):
    if (Options::useJSThreads() && !Options::useHandlerICInFTL()) {
        dataLogLn("FATAL: useJSThreads requires useHandlerICInFTL (SPEC-jit M2b).");
        CRASH();
    }
```

## M3 — Source/JavaScriptCore/Sources.txt [FINAL; REQUIRED-NOW]

Unchanged from Task 1; re-verified absent. Three insertions, alphabetical
within their blocks:

```
bytecode/JSThreadsSafepoint.cpp      (after bytecode/JumpTable.cpp)
bytecode/RetiredJITArtifacts.cpp     (after bytecode/RecordedStatuses.cpp)
jit/ConcurrentButterflyOperations.cpp (after jit/CallFrameShuffler64.cpp)
```

`CMakeLists.txt`: no change (Sources.txt is consumed for these directories).
Without M3 the tree DOES NOT LINK once the jit workstream is merged: the I2/I3/I8
asserts in CodeBlock.cpp / DFGCommonData.cpp / DFGJumpReplacement.cpp /
PropertyInlineCache.cpp / CallLinkInfo.cpp reference `JSThreadsSafepoint::*`
unconditionally (runtime-gated, not compile-gated).

**LINK COUPLING (review round 3, R3-10): M3 must be applied TOGETHER WITH
INTEGRATE-objectmodel.md entry 2 (Sources.txt: `runtime/ConcurrentButterfly.cpp`)
in the same step.** The two TUs reference each other's symbols:
`bytecode/JSThreadsSafepoint.cpp` reads `g_jsThreadsStubWorldStopped`
(compiled in because runtime/ConcurrentButterfly.h, present in the tree,
defines `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS`), and that global is DEFINED
only in runtime/ConcurrentButterfly.cpp — which is itself unbuilt until OM
entry 2 lands; conversely ConcurrentButterfly.cpp's veneer calls
`JSThreadsSafepoint::stopTheWorldAndRun`/`worldIsStopped`, defined only in the
M3-added JSThreadsSafepoint.cpp. M3 alone => undefined
`g_jsThreadsStubWorldStopped`; OM entry 2 alone => undefined
`JSThreadsSafepoint::*`. Neither manifest's Sources.txt change links without
the other.

**LINK COUPLING ADDENDUM (review round 4, R4-10): step 1 additionally
requires INTEGRATE-vmstate.md M2 (Sources.txt: `runtime/VMLite.cpp` +
`runtime/VMLiteShared.cpp`) in the SAME step.** Because `runtime/VMLite.h` IS
present in the tree, `jit/ConcurrentButterflyOperations.cpp` compiles with
`JSC_JIT_HAS_VMLITE` defined: its local `currentButterflyTID()` shim is
compiled out (the `#if !defined(JSC_JIT_HAS_VMLITE) &&
!defined(JSC_JIT_HAS_CONCURRENT_BUTTERFLY)` guard) and the TU emits calls to
`currentButterflyTID()` and `setVMLiteTIDTagHook(&updateButterflyTIDTag)` —
JS_EXPORT_PRIVATE symbols DEFINED only in `runtime/VMLite.cpp` (vmstate
manifest; not in Sources.txt — verified round 4: grep VMLite Sources.txt is
empty), which is added only by INTEGRATE-vmstate.md M2. Applying M3 + OM
entry 2 without vmstate M2 therefore still fails to link, with undefined
references from ConcurrentButterflyOperations.cpp. Net: apply-order step 1 =
{jit M3, INTEGRATE-objectmodel.md entry 2, INTEGRATE-vmstate.md M2} as ONE
atomic Sources.txt step. (Cross-recorded in the CS3 disposition, whose
"P5 init registers the hook for real" note is exactly the source of this
link dependency.)

## M4a — runtime/JSCConfig.h [FINAL; PREP] — disposition DECIDED

Per the Task-6 analysis, **Option 1 is the handoff recommendation and the shape
the owned code is written against**: the §5.4 LLInt gate byte is SATISFIED by
the existing `Config::options` storage (`Options::useJSThreads()`'s backing
byte already lives in the frozen config page; `ifJSThreadsBranch` in
LowLevelInterpreter64.asm already loads it — same instruction shape as a
dedicated byte). M4a therefore SHRINKS to the Darwin TLS-key slot only. Add to
`JSC::Config` (JSCConfig.h, inside `struct Config`):

```cpp
    uint32_t butterflyTIDTagTLSKey; // Darwin only: pthread key for g_jscButterflyTIDTag (SPEC-jit App. R5)
#define JSC_CONFIG_HAS_BUTTERFLY_TID_TAG_TLS_KEY 1 // consumed by jit/ConcurrentButterflyOperations.cpp (Task 1b)
```

No options-finalize mirror store is needed under Option 1 (the options byte is
mirrored by the existing OptionsStorage copy). Darwin ordering constraint
(unchanged from Task 1b): the first
`JSC::initializeButterflyTIDTagForCurrentThread()` call must precede
`Config::finalize()` — simplest is to call it from options-finalize right
after M2b (idempotent; correct on the main thread, tag 0). If the integrator
instead picks Option 2 (dedicated `uint8_t useJSThreads` byte), change ONE
line — the field expression in `ifJSThreadsBranch` — and re-bless the Task-13
golden diffs (instruction shape identical; only the immediate moves).
Consequence of M4a landing: Darwin LLInt threaded WRITE fast paths go live
(Task-8 item 3); until then they slow-path (reads unaffected).

## M4 rest — runtime/VMManager.{h,cpp} + config slot [FINAL scope; INTEGRATION-DEFERRED]

Exactly R1.a–d (SPEC-jit §9.2 / annex App. R1), plus the integration steps the
owned code has pre-wired:

1. R1.a: stop reason `v(JSThreads)` in `FOR_EACH_STOP_THE_WORLD_REASON`
   (VMManager.h:200-212).
2. R1.b: `notifyVMStop` dispatch `case StopReason::JSThreads:` →
   `JSC_CONFIG_METHOD(jsThreadsStopTheWorld)` slot + `VMManager::setJSThreadsCallback`
   (pattern VMManager.h:272-277); registered from owned
   `bytecode/JSThreadsSafepoint.cpp` at first flagged VM.
3. R1.c: `static void VMManager::requestStopAllWithConductor(StopReason, VM*)` —
   stores `m_jsThreadsConductor` under `m_worldLock`, then `requestStopAll`;
   arbitration: reason==JSThreads ∧ all parked ⇒ `m_targetVM = m_jsThreadsConductor`
   (NOT the last parker, G7). The pending-job slot + requester-vs-requester
   park-aware mutex are OWNED (already in JSThreadsSafepoint.cpp) — VMManager
   needs no job state. FREEZE SCOPE (r11): this VM-counting arbitration is final
   only for the N-separate-VMs config; N threads in ONE VM is the Phase-B
   charter (vmstate Dev-10, api §2) and re-freezes R1.c there.
4. R1.d: per-mutator ISB on leaving `notifyVMStop` when the serviced stop
   included `JSThreads` OR `GC` (F5), ordered BEFORE heap's
   `gcDidResumeFromStopTheWorld` hook (heap manifest 5a). The stub
   `crossModifyingCodeFence()` calls in JSThreadsSafepoint.cpp become redundant
   on the requesting path (keep-or-drop; keeping is harmless).
5. Replace the stub body of `JSThreadsSafepoint::stopTheWorldAndRun` per the
   commented R1.a-i sequence inside it. The R1.i GCL bracket
   (`Heap::JSThreadsStopScope`, CS2; as amended by R4-1 — server resolved via
   `vm.clientHeap.server()`, client-scoped access release) and the R1.h
   already-stopped inline path written at Task 5 are FINAL shapes — carry
   over verbatim; only the stop/resume steps change. NEVER calls
   `bumpAndReclaim` (CS4).
   **AMENDMENT (review round 3, R3-5): "verbatim" applies to the
   stop/witness mechanics only, NOT to locks acquired INSIDE conducted
   closures.** `CodeBlock::jettison`'s doJettison closure takes
   `ConcurrentJSLocker(m_lock)` while world-stopped; under M4's real parking
   that deadlocks the conductor — silently, past the stop watchdog (which
   only guards reaching Mode::Stopped) — if a remote mutator parked holding
   the same CodeBlock's m_lock (GCSafeConcurrentJSLocker regions such as
   addAccessCase tolerate GC stops today, so this is possible by design
   flag-off). CHOSEN RULE: flag-on, ConcurrentJSLock critical sections must
   be PARK-FREE — M4's park hook (notifyVMStop entry) MUST RELEASE_ASSERT
   that the parking thread holds no ConcurrentJSLock (thread-local lock
   census in ConcurrentJSLock, useJSThreads-gated). This is a NEW M4
   obligation, item 5a; the alternative (timed tryLock + watchdog coverage
   for in-closure acquisitions) was rejected as masking rather than
   preventing the inversion. Comment at the disputed site:
   bytecode/CodeBlock.cpp doJettison IC-deref block.
6. Wire `JSThreadsSafepoint::watchdogAssertStopProgress(requestStart)` into
   every iteration of the real requester wait loop (Task 11; marker in
   JSThreadsSafepoint.h). Pre-M4 the stub never waits, so the watchdog is
   dormant by construction.
7. Delete `s_stubWorldStoppedDepth` and §5.6 disjunct 4 (see CS6 below; the OM
   witness + `jsThreadsStopTheWorldAndRun` veneer body are deleted in the same
   change — both sides are marked).
8. Add the two relaxed `std::atomic<uint64_t>` stop counters to the real stop
   path (owned side: `JSThreadsSafepoint::stopCountForTesting()` /
   `stoppedMillisForTesting()`) and the REQUESTED `$vm.jsThreadsStopCount()` /
   `$vm.jsThreadsStoppedMillis()` hooks (tools/JSDollarVM.cpp, non-owned,
   non-blocking; Task-13 stop-budget gate consumes them when present).
9. Re-run the Task-13 integration gate FULL (`run-jit-tests.sh --int-gate`).

## M5 — runtime/VM.h: NONE (final; epoch state is heap-owned, §4.4)

## M6 — runtime/** deferred-fire conversions [FINAL: exactly ONE entry]

Task 11's direct-fire audit (full table in the Task-11 section) found exactly
one bucket-(iii) site. **M6.1**: `Structure::firePropertyReplacementWatchpointSet`
gains a `DeferredWatchpointFire* deferred = nullptr` parameter (Structure.h
declaration + Structure.cpp:1653-1666 body; ready-to-paste hunks in the Task-11
section). The owned-side consumer step (threading the deferred holder through
`PropertyCondition::isWatchable*` and `AccessGenerationResult`) is applied by
the jit side in the SAME change; interim mitigation if M6.1 is rejected: the
two owned call sites pass `MakeNoChanges` under `Options::useJSThreads()`
(sound, slower). All other audited fire sites are bucket (i)/(ii); the
runtime/VM.cpp:1780 gigacage-callback site is recorded as a REVIEW note (fail-fast
by stub assert; expected unreachable in Bun), NOT an M6 entry.

## M7 — runtime/VMEntryScope.cpp entered-VM tripwire [NEW at review round 3 (R3-4); AMENDED at review round 4 (R4-12: shared-server clients excluded, matching R3-11); REQUIRED before any flag-on config in which a second VM could be entered; DELETED at M4]

The pre-M4 stub's soundness premise — at most one entered VM — is only
SPOT-CHECKED by the sampled entered-VM counts in
`JSThreadsSafepoint::stopTheWorldAndRun` / `AlreadyStoppedWorldWitnessScope`
(check-then-act: VM entry does not consult the stub, so a thread can enter
another VM between the count and the end of the closure). M7 makes the premise
STRUCTURAL by enforcing it at the entry point itself: a second concurrent
top-level VM entry crashes deterministically on the ENTERING thread, regardless
of interleaving. Until M7 is applied, `useJSThreads=1` with more than one
concurrently-enterable VM in the process (Workers) is an UNSUPPORTED
configuration whose violation is only probabilistically caught.

**ENVELOPE AMENDMENT (review round 4, R4-12).** The round-3 draft of M7
RELEASE_ASSERTed sole entry UNCONDITIONALLY, which contradicts the R3-11 fix:
in the {useJSThreads=1, useSharedGCHeap=1, N clients} config — which R3-11
was made precisely to keep legal, and whose pre-INT bench leg Task 13 keeps —
the SECOND client VM would crash at ENTRY, making R3-11's shared-server
carve-out dead code. Resolution: pick envelope (a) — M7 mirrors R3-11.
N concurrently-entered VMs are permitted iff they are all clients of ONE
shared GC server (their stops are conducted by that server, and pre-M4 JS
execution among them is serialized by the phase-1 GIL); any entry outside
that single server, or any second entry in a non-shared config, still
crashes deterministically. (Envelope (b) — declaring the shared-server
N-client config unsupported pre-M4 and deleting the R3-11 carve-out — was
rejected because Task 13 keeps the pre-INT shared-heap leg.)

Ready-to-paste, `runtime/VMEntryScope.cpp`:

1. Add to the includes: `#include "JSThreadsSafepoint.h"`, `#include "Heap.h"`
   and `#include <atomic>`.
2. Add inside `namespace JSC`, above `VMEntryScope::setUpSlow`:
```cpp
// Pre-M4 structural entered-VM tripwire (docs/threads/INTEGRATE-jit.md M7,
// review rounds 3+4, R3-4/R4-12). Counts VMs with a live top-level entry
// scope, permitting N clients of ONE shared GC server (the R3-11-legal
// config) and crashing every other concurrent-entry shape deterministically
// on the ENTERING thread. The shared-server slot is STICKY (mirrors the
// heap's sticky ISS bit): it is never cleared, so the exit path needs no
// counter/slot coordination and there is no clear-vs-install race; the
// pointer is compared for identity only, never dereferenced. One shared
// server per process is the supported pre-M4 envelope.
// DELETE at integration manifest M4: real parking makes entry during a stop
// park in notifyVMStop instead of crash, and the GIL-removal change replaces
// the premise wholesale.
static std::atomic<unsigned> s_jsThreadsEnteredLegacyVMs { 0 };
static std::atomic<unsigned> s_jsThreadsEnteredSharedClients { 0 };
static std::atomic<JSC::Heap*> s_jsThreadsEnteredSharedServer { nullptr };
```
3. At the TOP of `VMEntryScope::setUpSlow()` (before `m_vm.entryScope = this;`):
```cpp
    if (Options::useJSThreads()) [[unlikely]] {
        // M7/R3-4: the phase-1 stub runs stop-the-world closures inline on
        // the premise that the requesting caller is the only RUNNING mutator.
        // Enforce at entry: no entering while a stub stop window is open, and
        // no second concurrently-entered VM — except clients of ONE shared GC
        // server (R3-11/R4-12; their cross-client stops are conducted by the
        // server and the phase-1 GIL serializes their execution). Two
        // counters so concurrent same-server client entries (legal) cannot
        // false-positive a single-counter "!previous" check, while each side
        // still trips on the other (mixed legacy+shared is unsupported).
        RELEASE_ASSERT(!JSThreadsSafepoint::worldIsStopped());
        JSC::Heap& server = m_vm.clientHeap.server();
        if (server.isSharedServer()) {
            s_jsThreadsEnteredSharedClients.fetch_add(1, std::memory_order_acq_rel);
            JSC::Heap* expected = nullptr;
            if (!s_jsThreadsEnteredSharedServer.compare_exchange_strong(expected, &server, std::memory_order_acq_rel)) {
                // One shared server per process pre-M4 (sticky slot,
                // identity-compared only).
                RELEASE_ASSERT(expected == &server);
            }
            RELEASE_ASSERT(!s_jsThreadsEnteredLegacyVMs.load(std::memory_order_acquire));
        } else {
            unsigned previouslyEntered = s_jsThreadsEnteredLegacyVMs.fetch_add(1, std::memory_order_acq_rel);
            RELEASE_ASSERT(!previouslyEntered); // sole legacy entry, as in round 3
            RELEASE_ASSERT(!s_jsThreadsEnteredSharedClients.load(std::memory_order_acquire));
        }
    }
```
4. At the TOP of `VMEntryScope::tearDownSlow()`:
```cpp
    if (Options::useJSThreads()) [[unlikely]] {
        JSC::Heap& server = m_vm.clientHeap.server();
        if (server.isSharedServer())
            s_jsThreadsEnteredSharedClients.fetch_sub(1, std::memory_order_acq_rel);
        else
            s_jsThreadsEnteredLegacyVMs.fetch_sub(1, std::memory_order_acq_rel);
    }
```
   (ISS is sticky and set only while no legacy holder exists — heap §10B.4 —
   so a VM cannot be counted on one side at setUp and the other at tearDown
   except via §10D reversion, which is out of the pre-M4 envelope.)

Residual envelope notes (R4-12): (i) the sticky server slot means a process
that tears down its shared server and starts a fresh one pre-M4 will compare
against a stale identity and may crash — acceptable for a tripwire whose
supported envelope is one shared server per process; (ii) N entered
shared-server clients are mutually serialized by the phase-1 GIL, and the
stub's requesting-path `enteredVMs <= 1` count in
`bytecode/JSThreadsSafepoint.cpp` is intentionally UNCHANGED: a
requesting-path (non-already-stopped) stop from one of N entered clients
still RELEASE_ASSERTs, because the inline-run stub cannot park the other
clients — in the shared-server config, pre-M4 stops must arrive via
already-stopped GC windows (the R1.h path), which is how the Task-13
shared-heap leg exercises them.

Owned-side counterparts already in tree (round 3): the R3-4 comments at
both sampled-count sites in `bytecode/JSThreadsSafepoint.cpp` point here.

## Auxiliary non-owned hunks (recorded; not part of M1–M7)

| Hunk | Phase | Blocking? |
|---|---|---|
| VMEntryScope.cpp I19 assert (Task-1b section; include + 6-line debug block) | when flag-on is functional | no (debug-only) |
| CommonSlowPaths.cpp `PROFILE_VALUE_IN` → `storeBucketConcurrently` (Task-12 section) | any time | no (TSAN cleanliness; plain store already word-atomic) |
| `$vm` stop-budget hooks (M4-rest item 8) | M4 | no (Task-13 prints UNAVAILABLE otherwise) |

## CS1–CS6 dispositions (FINAL record)

* **CS1 (one option, api/OM alias).** SATISFIED. Canonical entry =
  `useJSThreads` (OptionsList.h:681, api 9.2-1); in-tree alias `useThreads`
  (:685). `useConcurrentJS` is not in the tree; if api/OM introduce it, alias
  it to the same storage. All jit code reads `Options::useJSThreads()`
  exclusively — no jit action on any aliasing outcome.
* **CS2 (heap GCL bracket).** RESOLVED-AS-PROVIDED and **CONSUMED LIVE**:
  heap landed `Heap::JSThreadsStopScope` + `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE`
  (Heap.h:77,483); Task 5's R1.i bracket uses it by exact name on the
  requesting path (release THIS client's heap access → scope over GCL → work
  → unwind). No-op when the resolved server is `!isSharedServer()`.
  **R4-1 amendment:** the bracket is keyed on `vm.clientHeap.server()`, not
  `vm.heap` (a client's vm.heap is not the shared server), and the access
  release is client-scoped (`GCClient::Heap::releaseHeapAccess`, since
  server-level release forwards to the MAIN client). THIS shape — not the
  round-3 `vm.heap` one — is what M4 carries over verbatim.
* **CS3 (MANDATORY tag hook).** IMPLEMENTED AND LIVE: `VMLite.h` is in the
  tree, so P5 init registers the `setVMLiteTIDTagHook` body for real (stores
  `uint64_t(tid) << 48` to the R5 slot); vmstate's `VMLite::setCurrent` fires
  it post-TLS-write. The interim `currentButterflyTID()`-returns-0 shim is
  compiled out by the `__has_include` switch — verify at integration that the
  real provider is selected (one grep: `JSC_JIT_HAS_VMLITE_TID_PROVIDER` in
  jit/ConcurrentButterflyOperations.cpp). I19's runtime witness = the
  VMEntryScope hunk above + the Task-1b 3-thread test. **LINK DEPENDENCY
  (review round 4, R4-10): the real-provider selection makes
  jit/ConcurrentButterflyOperations.cpp reference `currentButterflyTID` /
  `setVMLiteTIDTagHook`, defined only in runtime/VMLite.cpp — so apply-order
  step 1 must include INTEGRATE-vmstate.md M2 (see the M3 LINK COUPLING
  ADDENDUM).**
* **CS4 (epoch-bump cadence).** REFUSED-AND-HONORED: `stopTheWorldAndRun`
  never calls `bumpAndReclaim` (Task 5; structural — there is no call in any
  owned TU, grep `bumpAndReclaim` over owned paths = 0 hits). Bumps happen
  only at heap's two GC-side contexts; JSThreads stops enqueue a GC request
  (heap 13.10a). §4.4's adapter only ever calls `safepointEpoch().retire()`.
* **CS5 (D9 write-elision correction).** ADOPTED BY OM, HONORED BY EMISSION:
  every tier's write path emits the fused owner-TID compare unconditionally —
  Task 8 (CCallHelpers/LLInt chokes), Task 9 (DFG twins), Task 10 (FTL twins,
  "TID compare never elided" rows in each I14 table). E2 elision removes ONLY
  the SW branch + AS SW test; E3 mask-only applies to READS only.
* **CS6 (pre-M4 world-stopped witness).** BOTH options are live in this tree
  (orchestrator picked "both", redundant-but-harmless, per OM's comment at
  ConcurrentButterfly.h:638): OM defines
  `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS` (jit's disjunct 4 reads
  `g_jsThreadsStubWorldStopped`) AND OM's `jsThreadsStopTheWorldAndRun` veneer
  delegates to `JSThreadsSafepoint::stopTheWorldAndRun`. **M4 deletion step
  (recorded here as CS6 requires):** delete, in ONE change — jit side:
  §5.6 disjunct 4 + `s_stubWorldStoppedDepth` (bytecode/JSThreadsSafepoint.cpp);
  OM side: the witness global + macro + veneer stub body (ConcurrentButterfly.{h,cpp},
  both marked THREADS-INTEGRATE). `worldIsStopped` then has three disjuncts
  (VMM Stopped / heap all-clients / legacy heap), per §5.6.

## Integrator apply order (checklist)

1. **M3** (link requirement — first) **together with INTEGRATE-objectmodel.md
   entry 2** (`runtime/ConcurrentButterfly.cpp` in Sources.txt) **AND
   INTEGRATE-vmstate.md M2** (`runtime/VMLite.cpp` + `runtime/VMLiteShared.cpp`
   in Sources.txt) — the TUs reference each other's symbols (witness global /
   STWR delegation / `currentButterflyTID` + `setVMLiteTIDTagHook` consumed by
   jit/ConcurrentButterflyOperations.cpp), so no proper subset links (R3-10 +
   R4-10; details in the M3 section's LINK COUPLING paragraphs).
2. **M1 + M2a + M2c + M4a** (prep; un-skips the Task-13 tag-discipline/FTL
   legs and Darwin LLInt writes; M2c is a hard precondition of EVERY flag-on
   JIT leg — without it the first JIT'd property-write emission
   RELEASE_ASSERTs, all platforms; review round 2 R2-6).
3. **M6.1** + owned-side consumer (one atomic step).
4. Optional hunks (VMEntryScope I19, CommonSlowPaths, any time).
4a. **M7** (VMEntryScope sole-entry tripwire, R3-4) — REQUIRED before any
   flag-on configuration in which a second VM could be entered (Workers);
   harmless to apply with step 2. Deleted again at M4.
5. **M4 rest** at integration (items 1–9 above), then re-run
   `JSTests/threads/jit/run-jit-tests.sh --int-gate` FULL + ASan epoch re-validation.
6. **M2b** when enabling flag-on by default for threads.

## GIL-removal preconditions contributed by this workstream (consolidated; all recorded in their task sections)

1. LLInt monomorphic CALL fast path in record form (Task 7 deferred item).
2. DFG64 + FTL array-element store predicates (Task 9 gap 1 / Task 10 gap 1).
3. Task 10 gap 3: MultiDeleteByOffset flag-on bail (or OM-chartered E4-style gate).
4. Allocation tagging (Tasks 8/9/10): or-in the R5 tag at butterfly install
   (with OM), then drop the FRESH-ALLOC carve-outs.
5. Transition fast-path revival per §5.5 E4 now that OM's TTL sets are in-tree
   (replace the three Repatch GiveUpOnCache gates; flip the Task-13 PA lint to
   its E4 form — marked in lint.sh).
6. ARM64 R7/F7 dependency gap closure (Task 8 gap 1 — now closed at the
   CCallHelpers choke by the re-load fallback, review round 1; Task 9 gap 3
   remains for the DFG spread/sort/enumerator sites' indexing-shape guards;
   FTL has no gap).
7. Task-13 integration gate FULL green at M4/CS2 + stop-budget ceiling set.
8. Phase-B charter (N threads in ONE VM) — R1.c re-freeze, api §2; this
   manifest's M4 covers the N-separate-VMs config ONLY.
9. **Segmented-butterfly (regime 2) fast paths — perf-gated work item (review
   round 1).** NO tier currently emits the design's regime-2 dependent-load
   fast path: LLInt/CCallHelpers/DFG/FTL all route top16==0xffff to generic
   C++ slow paths or OSR exit, and the seven
   `operationSegmentedButterfly*`/`operationSharedArrayStorage*` R3 shims in
   jit/ConcurrentButterflyOperations.cpp are RELEASE_ASSERT_NOT_REACHED stubs
   referenced by no emitter (they cannot be filled until OM's spine accessor
   DEFINITIONS land — the header declares them only). Consequence: a
   persistently shared-and-reshaped object pays a C++ call (or an
   exit/recompile cycle) per out-of-line access — a silent scope reduction
   from THREAD.md's "one extra dependent load" / ~2x cost model for non-TTL
   objects, and a risk to the Task-13 flag-on bench budget. Before GIL
   removal (or before shipping flag-on if the G2 gate misses): wire the
   segmented dependent-load fast path or at minimum the R3 shim tail-calls
   into LLInt/Baseline/DFG slow-case routing, delete the
   RELEASE_ASSERT_NOT_REACHED bodies, and re-run G2. **Bench-coverage
   addendum (review round 2, R2-5): the Task-13 G2 gate currently measures
   only the no-sharing {1,0} config, so this regression is UNMEASURED. The
   bench plan must add a shared-and-reshaped-object workload (persistently
   non-TTL objects, out-of-line reads+writes) and that leg must be green
   before declaring THREAD.md's ~2x non-TTL cost model met. Scheduling owner:
   orchestrator (jointly with OM, whose spine-accessor definitions gate the
   shim fill).**
10. **Deferred Class-A fire ordering (review round 1).** The deferral overload
   (`WatchpointSet::fireAllSlow(VM&, DeferredWatchpointFire*)`) lets the
   lock-holding caller COMPLETE its watched-fact mutation (e.g. publish a new
   structureID) before the scope-exit fire's stop lands; under N mutators,
   other threads' optimized code runs against the already-false fact in that
   window — forbidden by THREAD.md. Sound today only under the phase-1 GIL
   (single mutator) or when the whole mutation+fire already runs world-stopped
   (the OM TTL-set pattern, Structure::fireThreadLocalSetsWithChainUnderStop).
   Before GIL removal, every deferred Class-A site (Task-11 audit rows
   Structure.cpp:1929 "(i) via deferral" and any future ones) must be
   classified in a new audit column — **"fact published before fire?"** with
   verdict (a) single-mutator-only / (b) published-inside-stop / (c) all
   compiled consumers re-check dynamically — or restructured onto the TTL-set
   pattern. The full caveat is written at the overload in
   bytecode/Watchpoint.cpp.
11. **Concurrent slow-path call/direct-call linking serialization (review
   round 3, R3-3).** `CallLinkInfo::publishRecord` /
   `DirectCallLinkInfo::publishRecord` use a NON-ATOMIC `std::exchange` on the
   plain `m_record`, and the writers — `linkMonomorphicCall`,
   `setVirtualCall`, `setStub`, `linkDirectCall` (jit/Repatch.cpp slow paths)
   — take no lock. Under N mutators, two threads entering the same unlinked
   call site's slow path can both observe the SAME oldRecord and retire it
   twice => double-delete at epoch expiry (heap corruption); the same window
   tears the unsynchronized `m_callee`/`m_codeBlock`/`m_mode` mirror writes
   and `setLastSeenCallee`. The F6 publish protocol defends READERS only.
   Before GIL removal: serialize flag-on linking per CallLinkInfo (e.g. the
   owner `CodeBlock::m_lock` around the set*/link* entry points) AND make the
   `m_record` swap a CAS so a losing linker retires its OWN new record, never
   the same old one twice. Caveat comments at both publishRecord definitions
   in bytecode/CallLinkInfo.cpp.

**Mechanical tripwire (review round 1):**
`JSThreadsSafepoint::gilRemovalPreconditionsMet()` (bytecode/JSThreadsSafepoint.h)
is a constexpr FALSE. The change that removes the GIL MUST gate second-mutator
attach on `RELEASE_ASSERT(JSThreadsSafepoint::gilRemovalPreconditionsMet())`
and may flip the constant to true only in the same commit that closes (or
consciously re-classifies) every precondition above. This makes it impossible
to ship GIL removal ahead of these fixes silently.

---

# Review round 1 — adversarial-review dispositions (post-handoff fixes)

Every blocker/major finding from adversarial-review round 1 was verified
against the tree; this section records the fixes (all in owned paths), the
refutations, and the two items that need orchestrator/integrator action.
Where this section conflicts with earlier sections, THIS section wins.

## R1-1 (blocker, FIXED): LLInt op_put_by_id R7 dependency was wired to the metadata word

CONFIRMED and fixed in `llint/LowLevelInterpreter64.asm` (.opPutByIdThreaded):
the cell's structureID is now loaded into a register (t2), compared 32-bit
against the cache word's low half, and t2 — not the relaxed-published metadata
word — feeds `butterflyLoadDependsOnStructureID`, mirroring
op_try_get_by_id/op_get_by_id_direct. (A control dependency from the old
folded compare does not order load->load on ARM64; the old form could pair a
fresh {sid,offset} cache word with a stale butterfly => OOB write.)

## R1-2 (major, FIXED): dictionary structures must never be served by threaded one-word caches or handler fast paths

CONFIRMED: `propertyAccessesAreCacheable()` admits CachedDictionaryKind, and a
dictionary keeps its structureID while the owner grows the butterfly, so the
whole R7 "new sid => new butterfly" argument proves nothing for dictionaries
(THREAD.md: dictionary reads take the structure lock). **FROZEN RULE: flag-on,
no generated fast path (one-word LLInt cache, packed self word, handler
AccessCase) may be keyed on a dictionary base structure.** Enforced at:

* `llint/LLIntSlowPaths.cpp` — all four threaded publishes
  (try_get_by_id, get_by_id_direct, get_by_id Default, put_by_id replace) now
  also require `!structure->isDictionary()`.
* `bytecode/PropertyInlineCache.cpp addAccessCase` — single funnel for every
  repatch-driven IC: flag-on GaveUp for `accessCase->structure()->isDictionary()`.
  This also covers the inlined packed self word (`setInlinedHandler` is fed
  only from admitted handlers); `setInlineAccessSelfState` additionally
  RELEASE_ASSERTs `!structure->isDictionary()` flag-on as a tripwire.
  Prototype-chain dictionary structures inside condition sets remain governed
  by the existing watchability machinery (unwatchable conditions => GiveUp).

## R1-3 (major, FIXED): GlobalProperty scope metadata is now FROZEN post-link flag-on

CONFIRMED: `runtime/CommonSlowPathsInlines.h tryCacheGetFromScopeGlobal/
tryCachePutToScopeGlobal` rewrite the {m_getPutInfo, m_structureID, m_operand}
triple as three plain stores under `codeBlock->m_lock`, which no fast path
takes — a racing reader could pair a fresh structureID with a stale operand,
or a stale GlobalProperty resolveType with an operand that is now a raw
pointer. Fix (option (a) of the finding, conservative): **flag-on the triple
is immutable after CodeBlock linking and the GlobalProperty structure cache is
never armed**:

* `bytecode/CodeBlock.cpp` (link): op_get_from_scope / op_put_to_scope skip
  the `metadata.m_structureID.set(...)` publication flag-on (the global
  object's structure is routinely a cacheable dictionary — R1-2 — so arming
  even the immutable link-time snapshot is not worth the audit burden).
  `loadScopeWithStructureCheck` therefore always misses; GlobalProperty
  accesses stay on the C++ slow path. GlobalVar/GlobalLexicalVar/ClosureVar
  fast paths are unaffected (their link-time metadata is immutable).
* `llint/LLIntSlowPaths.cpp` + `jit/JITOperations.cpp`: the four owned
  tryCache* call sites are gated `!Options::useJSThreads()`.
* The LLInt threaded GlobalProperty blocks are now structurally unreachable
  flag-on and kept as defense in depth (commented).
* Side benefit: DFG/Baseline compile-time snapshots of scope metadata read an
  immutable triple, so no compile-thread coherence argument is needed.
* Perf note: GlobalProperty (non-var implicit globals) goes generic flag-on.
  If the Task-13 G2 budget misses on global-heavy code, revive as an immutable
  single-pointer record (§5.8 pattern) — same charter as the §4.3 LLInt-cache
  revival.

**Defense-in-depth shared-file hunk (NEW, optional but recommended — also
covers the non-owned `lol/LOLJITOperations.cpp` callers and any future
caller).** File: `Source/JavaScriptCore/runtime/CommonSlowPathsInlines.h` —
insert as the FIRST statement of BOTH `tryCachePutToScopeGlobal` (line ~44)
and `tryCacheGetFromScopeGlobal` (line ~112):

```cpp
    // THREADS (SPEC-jit §5.5, review round 1): flag-on, scope metadata is
    // frozen after CodeBlock linking; rewriting {getPutInfo, structureID,
    // operand} as separate plain stores races the LLInt/Baseline fast-path
    // readers. The owned llint/jit callers are already gated; this gate
    // covers lol/ and future callers.
    if (Options::useJSThreads()) [[unlikely]]
        return;
```

(`Options.h` is already transitively included there; verify on apply.)

## R1-4 (major, FIXED): ARM64 R7 closure at the CCallHelpers choke + Baseline packed GetByIdSelf

See the rewritten Task-8 "Known gaps" item 1. Summary: the choke-point helper
`loadButterflyWithStructureDependency` now ALWAYS emits the ARM64 dependency
when dest != base, re-loading the structureID through destGPR when the caller
has no live sid register (coherence-sound). The Baseline packed GetByIdSelf
inline path passes its sid-dependent decoded-offset register explicitly.
putByIdReplaceHandler (write side) and all getByVal/generateImpl/enumerator
sites are covered by the choke fallback. The I14 table row
"JITInlineCacheGenerator packed GetByIdSelf threaded | CHOKE-R" is accurate
again.

## R1-5 (major, RECORDED + code comment): deferred Class-A fires complete the fact mutation before the stop

CONFIRMED as a design-record gap (code is GIL-sound today). Disposition:
GIL-removal precondition 10 (above) + the full ordering caveat written at
`WatchpointSet::fireAllSlow(VM&, DeferredWatchpointFire*)` in
bytecode/Watchpoint.cpp + the new mandatory "fact published before fire?"
column for the Task-11 audit. No behavior change now: pre-M4 there is exactly
one mutator, and the OM's TTL-set fires already use the publish-inside-stop
pattern (Structure.cpp:1536).

## R1-6 (major, REFUTED with evidence + premise FROZEN): get/put_by_val shape-byte/butterfly pairing

The uncited OM invariant EXISTS and is enforced in this tree: flag-on, every
indexing-shape conversion that touches Double or rewrites lanes runs under a
per-event stop-the-world — `JSObject::convertInt32ToDouble` (JSObject.cpp:1993)
routes through `relabelIndexingShapeConcurrent` (JSObject.cpp:2399, body runs
inside `jsThreadsStopTheWorldAndRun`), as do convertUndecidedTo*/
convertInt32ToContiguous; `convert*ToArrayStorage` routes through
`convertToArrayStorageConcurrent` (§4.6 stops); and R-DOUBLE (§4.7, OM
ConcurrentButterfly.cpp:1421) forbids reboxing at sharing onset. There is NO
in-place rebox outside a stop flag-on. Since the {shape-byte load, butterfly
load, lane access} sequence of each rejoin site contains no safepoint poll, a
stop can never split it, so the non-atomic pair is always mutually consistent.
**FROZEN PREMISE: "flag-on, no indexing-shape conversion of a reachable object
happens outside a stop-the-world window, and never as an in-place rebox"
(OM §4.7/I28 + §4.6) is the soundness premise of EVERY shape-dispatch rejoin
(LLInt get/put_by_val, Baseline/DFG/FTL CheckArray-then-GetButterfly paths).**
If OM ever relaxes it, every rejoin must re-validate the shape from data
derived after the butterfly load. Comments added at the LLInt rejoin sites.

## R1-7 (major, FIXED): FTL I16 was not enforced against B3 load-CSE

CONFIRMED: B3's InvalidationPoint lowers with `writes = HeapRange()` +
exitsSideways and CheckTraps' fast path is a plain load+branch, so B3
load-CSE across a poll was legal and could resurrect the stale predicate word.
Fixed in `ftl/FTLLowerDFGToB3.cpp`: `threadedButterflyLoadForWrite` now feeds
the predicate from `loadTaggedButterflyPinnedForWrite` — an effectful
PatchpointValue (reads top, controlDependent; ARM64 R7 dependency emitted
inside) that B3 never CSEs/hoists/sinks — and the MaybeArrayStorage AS arm
uses a pinned indexing-byte twin (a stale "not AS" byte + fresh SW=1 word
would route an AS store down the unlocked path, I20). All FTL write-predicate
sites (compilePutByOffset, MultiPutByOffset, MultiDeleteByOffset,
tryStoreJSArray, sort commit, enumerator put) inherit the pin through the
shared helper. READ predicates intentionally keep plain loads: a stale read
word is snapshot-sound (fragment aliasing / AS-COPY). The DFG needs no
analogue (its re-load is emitted inside the node's own code; the DFG backend
performs no machine-level CSE).

## R1-8 (major, DEVIATION RECORDED): THREAD.md's IC-update buffering was substituted, not implemented

CONFIRMED absent — and recorded here as an ARGUED DEVIATION, not an omission.
THREAD.md's "Inline Caches" section prescribes per-CodeBlock thread-locality
tracking + globally buffered IC updates flushed at ~1ms safepoints. That
design predates the Handler-IC architecture this tree already has. The
delivered substitution:

* IC UPDATE = append an immutable handler node under `CodeBlock::m_lock` +
  one fenced atomic publish of the chain head (F1/I5, Task 3). Racing
  executors never observe a partially-built case — the buffering design's
  goal — without any flush latency.
* IC RESET / code invalidation = stop-the-world (Tasks 5/11), exactly the
  design's "rely on safepoints if an inline cache needs to be reset eagerly".

Why this dominates buffering: (1) buffering existed to avoid synchronizing
with concurrent executors on update — the fenced-publish chain already
achieves wait-free reads (readers take no lock; the F2 address dependency is
the only read-side cost); (2) contention on `CodeBlock::m_lock` under N
mutators is bounded by IC-MISS frequency, not access frequency — misses are
already slow paths that take the same lock today single-threaded, and
steady-state ICs stop missing; (3) buffering would ADD a flush-latency window
in which all threads keep missing, costing more slow-path traffic than the
lock it saves. If Task-13 N-thread runs ever show `m_lock` contention from IC
regeneration storms, the recorded fallback is per-CodeBlock buffered merges
flushed at the existing once-per-ms safepoint — the design's mechanism,
adopted only if measurement demands it. THREAD.md's per-CodeBlock
thread-locality BIT is likewise subsumed: Baseline code is shared/unlinked
and handler dispatch is pure data, so there is no patch-in-place fast path
left for a thread-local bit to unlock (FTL handler ICs, Task 2, close the
last one).

## R1-9/R1-12 (blockers, ORCHESTRATOR ADJUDICATION STILL REQUESTED — re-confirmed OPEN at review rounds 2 AND 3): writes outside the declared owned paths

**Round-3 status (R3-6/R3-9):** unchanged again — still in tree, still
unadjudicated, hunks below re-verified verbatim-accurate a third time. The
jit workstream repeats that it can neither self-ratify nor revert non-owned
files; the either/or below remains the orchestrator's to execute before any
flag-on merge.

**Round-2 status (R2-3/R2-7):** unchanged — the four assembler/ edits and
JSTests/threads/jit/** remain in tree, the hunks below remain verbatim-accurate
(re-verified this round), and adjudication has not been granted. This is an
orchestrator action by construction: the jit workstream can neither ratify its
own carve-out nor revert non-owned files (reverting is itself a non-owned
write). Round 2 re-verified the cross-workstream claim: no other
INTEGRATE-*.md mentions assembler/. Do not merge to a flag-on configuration
before either (a) ratifying both carve-outs (amend the jit owned-path list
with `assembler/{X86Assembler.h,MacroAssemblerX86_64.h,ARM64Assembler.h,MacroAssemblerARM64.h}`
additive-only + `JSTests/threads/jit/**`) or (b) reverting the four files and
applying the hunks below as integrator-applied M-entries — Tasks 8-10 emission
depends on the emitters either way.

Two write sets exist outside `Source/JavaScriptCore/{jit,dfg,ftl,bytecode,llint}/**`
+ this file:

1. **assembler/** R5 emitters (4 files, genuinely additive, consumed by owned
   `jit/CCallHelpers.cpp:130-141`; no other workstream's INTEGRATE-*.md
   touches assembler/ — cross-checked, zero conflict).
2. **JSTests/threads/jit/**** (Task-13 test tree; no other manifest claims it).

Task 14 claimed a "SPEC-jit Owned clause, App. R5 additive-only" carve-out for
(1). Per the run charter, the orchestrator must either RATIFY both carve-outs
(amend the owned-path list) or treat the hunks below as integrator-applied and
revert the in-tree edits (reverting is itself a non-owned write, so the jit
workstream cannot do it — and doing so without applying the hunks breaks
Tasks 8-10). **The exact in-tree assembler hunks, verbatim, so adjudication
needs no diffing:**

* `assembler/X86Assembler.h` — in `enum OneByteOpcodeID` (after
  `OP_MOVSXD_GvEv = 0x63,`):
```cpp
        PRE_FS                          = 0x64,
```
  and after the existing `gs()` method:
```cpp
    // Causes the memory access in the next instruction to be offset by %fs. On ELF/Linux
    // x86-64, %fs is the thread pointer, so pairing this with a 32-bit absolute address
    // load yields an initial-exec TLS load: the "address" is the (sign-extended, typically
    // negative) TPOFF of the thread_local. Used for g_jscButterflyTIDTag (SPEC-jit-annex
    // App. R5, Task 1b).
    void fs()
    {
        m_formatter.prefix(PRE_FS);
    }
```

* `assembler/MacroAssemblerX86_64.h` — new `#if OS(LINUX)` block (after the
  Darwin `loadFromTLS64`/`storeToTLS64` group):
```cpp
#if OS(LINUX)
    // ELF initial-exec TLS load: one %fs-prefixed 64-bit load at a constant
    // (typically negative) offset from the thread pointer, baked as an
    // immediate at emission. The offset comes from
    // JSC::butterflyTIDTagELFTLSOffset() (jit/ConcurrentButterflyOperations.h),
    // which RELEASE_ASSERTs it is thread-invariant (SPEC-jit-annex App. R5;
    // SPEC-jit R5/Task 1b). The disp32 is sign-extended by the hardware, so
    // negative TPOFFs encode correctly.
    void loadFromELFTLS64(intptr_t offset, RegisterID dst)
    {
        RELEASE_ASSERT(offset == static_cast<intptr_t>(static_cast<int32_t>(offset)));
        m_assembler.fs();
        m_assembler.movq_mr(static_cast<uint32_t>(static_cast<int32_t>(offset)), dst);
    }

    static bool loadFromELFTLS64NeedsMacroScratchRegister()
    {
        return false;
    }
#endif
```

* `assembler/ARM64Assembler.h` — new `#if OS(LINUX)` block (after
  `mrs_TPIDRRO_EL0`):
```cpp
#if OS(LINUX)
    // MRS dst, TPIDR_EL0 (S3_3_C13_C0_2): the ELF thread pointer. Same encoding
    // shape as TPIDRRO_EL0 above with op2 = 2 instead of 3. Used for the
    // initial-exec TLS load of g_jscButterflyTIDTag (SPEC-jit-annex App. R5;
    // SPEC-jit R5/Task 1b).
    void mrs_TPIDR_EL0(RegisterID dst)
    {
        insn(0xd53bd040 | dst);
    }
#endif
```

* `assembler/MacroAssemblerARM64.h` — new `#if OS(LINUX)` block (after the
  Darwin `loadFromTLS64` group):
```cpp
#if OS(LINUX)
    // ELF initial-exec TLS load: TPIDR_EL0 + ldr at a constant offset, baked
    // as an immediate at emission. The offset comes from
    // JSC::butterflyTIDTagELFTLSOffset() (jit/ConcurrentButterflyOperations.h),
    // RELEASE_ASSERTed thread-invariant (SPEC-jit-annex App. R5; SPEC-jit
    // R5/Task 1b). The offset is materialized through dst itself, so no
    // macro scratch register is needed for encodable offsets; load64 falls
    // back to the data temp for unencodable ones, hence dst must not be the
    // data temp (mirrors loadFromTLS64 above).
    void loadFromELFTLS64(intptr_t offset, RegisterID dst)
    {
        RELEASE_ASSERT(offset == static_cast<intptr_t>(static_cast<int32_t>(offset)));
        m_assembler.mrs_TPIDR_EL0(dst);
        load64(Address(dst, static_cast<int32_t>(offset)), dst);
    }

    static bool loadFromELFTLS64NeedsMacroScratchRegister()
    {
        return true;
    }
#endif // OS(LINUX)
```

## R1-10 (major, ALREADY RECORDED — tripwire added): DFG64/FTL array stores + LLInt call record are GIL-sound only

The finding itself acknowledges these are recorded (GIL-removal preconditions
1-3). The missing MECHANICAL guard is now the
`JSThreadsSafepoint::gilRemovalPreconditionsMet()` constexpr tripwire (see the
preconditions section above): GIL removal cannot ship ahead of these fixes
without a deliberate, greppable constant flip in the same commit.

## R1-13 (major, FIXED): M2b Gigacage::JSValue hunk did not compile

Fixed in the Task-14 M2b text above (and the Task-8 item-5 note): the JSValue
gigacage does not exist in this tree (`Gigacage::Kind` has only `Primitive`);
the assert is replaced by a comment recording the vacuous satisfaction.

---

# Review round 2 — adversarial-review dispositions

Same rules as round 1: every blocker/major finding verified against the tree;
fixes are in owned paths; on divergence with earlier sections THIS section
wins (it post-dates the round-1 section too).

## R2-1 (blocker, FIXED): m_handler null window at all three publication sites

CONFIRMED. The flag-on dispatch fast path emitted for every Baseline/DFG/FTL
handler IC (`JITInlineCacheGenerator.cpp`) is
`loadPtr [propertyCache+offsetOfHandler]; call [handlerGPR+offsetOfCallTarget]`
with no null check, and `WTF::RefPtr` move construction/assignment nulls the
SOURCE slot — so `setNext(WTF::move(m_handler))` (prependHandler) and
`displacedHead = WTF::move(m_handler)` (initializeWithUnitHandler,
resetStubAsJumpInAccess) each opened a window in which a racing JIT'd reader
calls through address null. prependHandler is the NORMAL concurrent IC-update
path (under CodeBlock::m_lock, readers lock-free), so this was a real phase-B
crasher exactly contradicting the file's own one-fenced-publish comments and
the R1-8 deviation argument. Fixed in `bytecode/PropertyInlineCache.cpp` with
the same idiom already proven at `CallLinkInfo::setStub`:

* New static helper `publishHandlerChainHead(RefPtr<InlineCacheHandler>&,
  Ref<InlineCacheHandler>&&)`: build fully, ONE storeStoreFence (F1/I5), ONE
  raw pointer store into the bit_cast slot, deref the slot's old reference
  only AFTER publication. Contract: callers hold a copy of anything displaced
  they need alive.
* `prependHandler` now COPIES the head into the new node's m_next
  (`setNext(RefPtr { m_handler })`), then publishes via the helper. Readers
  observe old head or new head, both fully initialized; the old head's
  refcount is handed from the slot to the new node's m_next.
* `initializeWithUnitHandler` (flag-on arm) copies into `displacedHead`,
  publishes via the helper, then retires the copy through
  RetiredJITArtifacts. Flag-off arm unchanged (single mutator; transient null
  unobservable) with a comment saying why.
* `resetStubAsJumpInAccess` (flag-on arm) likewise — latent today (callers
  world-stopped) but no longer shares the broken move-from-member idiom.

## R2-2 (major, FIXED): shared stub-routine owner bookkeeping + SharedJITStubSet were unsynchronized cross-CodeBlock

CONFIRMED on both counts; both are now locked in code (NOT merely listed as
preconditions — the structures are owned-path, so fixing beats tripwiring):

* `jit/GCAwareJITStubRoutine.h/.cpp`: `PolymorphicAccessJITStubRoutine` gains
  `Lock m_ownersLock`; `addOwner`/`removeOwner`/`removeDeadOwners` all mutate
  `m_owners` under it. Rationale comment at addOwner: shared routines are
  reachable from many CodeBlocks, so no single CodeBlock's m_lock serializes
  two mutators missing in ICs of different CodeBlocks that resolve to the
  same shared routine; an unsynchronized HashCountedSet rehash is heap
  corruption. Lock taken unconditionally (IC-miss/reset slow paths only).
* `bytecode/SharedJITStubSet.h/.cpp` (the pointee of `VM::m_sharedJITStubs` —
  the CLASS is owned even though the VM.h member is not): internal
  `mutable Lock m_lock` guarding every container; all seven accessors
  (add/remove/find + the stateless/DOMJIT/slow-path-handler get/setters) take
  it. `setSlowPathHandler` drops any displaced ref OUTSIDE the lock because a
  final handler deref can re-enter `remove()` via
  `observeZeroRefCountImpl` (m_lock is not recursive). No VM.h/VM.cpp change
  needed, so nothing new for the integrator.

## R2-3 / R2-7 (blocker + major, ORCHESTRATOR ACTION — status updated in place): out-of-owned-path writes still unadjudicated

CONFIRMED-as-described and not actionable from inside the jit workstream (the
workstream can neither self-ratify nor revert non-owned files). The R1-9/R1-12
section above now carries the round-2 re-confirmation, the re-verified
cross-workstream zero-conflict check, and the explicit either/or the
orchestrator must execute before any flag-on merge.

## R2-4 (major, FIXED): pre-M4 stub witness could go process-global on per-VM evidence

CONFIRMED: the requesting path of `stopTheWorldAndRun` RELEASE_ASSERTed
`enteredVMs <= 1` but the R1.h already-stopped path bumped the process-global
`s_stubWorldStoppedDepth` on the strength of `vm.heap.worldIsStopped()` —
per-VM evidence — with no such check, so flag-on + Workers (multiple entered
VMs pre-M4) could spuriously satisfy `worldIsStopped()` on unrelated threads
(spurious VM-less patching asserts; worse, `fireAllUnderClassAStop` branch (1)
firing a Class-A set inline while a foreign mutator runs). Fixed in
`bytecode/JSThreadsSafepoint.cpp` per the finding's option (a): the R1.h path
now replicates the entered-VM count and RELEASE_ASSERTs `<= 1` BEFORE the
fetch_add — but only when no PROCESS-GLOBAL witness already holds
(`!worldIsStopped()`), because under a genuine all-VM stop (VMManager
Stopped mode, e.g. the wasm debugger, or an outer stopTheWorldAndRun closure
that already passed this check) multiple entered-but-parked VMs are
legitimate and the global witness is truthfully process-wide. This makes the
assert the startup-configuration tripwire for the bring-up window between
enabling useJSThreads and M4; the whole counter (and this check) is deleted
at M4 as already recorded.

## R2-5 (major, RECORDED — no code action required by the finding itself): regime-2 fast paths absent in every tier

CONFIRMED as the known state: the finding explicitly verifies these are
honestly recorded (GIL-removal preconditions 1, 2, 9) and tripwired by the
constexpr-false `gilRemovalPreconditionsMet()`. Round-2 additions: the
bench-coverage addendum inside precondition 9 above (shared-and-reshaped
workload must be measured before declaring the THREAD.md ~2x cost model met;
the current G2 gate's {1,0} config does not see this regression) and the
explicit note that preconditions 1, 2, 3, 9 are scheduled work items needing
orchestrator-assigned owners, jointly with OM for the shim fill.

## R2-6 (major, FIXED in manifest): main-thread P5 init had no call site and was misclassified Darwin-only

CONFIRMED: `initializeButterflyTIDTagForCurrentThread()` had zero call sites
tree-wide, the api spawn-path diff is unapplied (and covers spawned threads
only), and the ELF/Linux offset query has the same first-call RELEASE_ASSERT
as Darwin's key query — so every flag-on JIT leg would crash at first
property-write emission, four apply-order steps before the old M2b-adjacent
note would have fired. Fixed by the new **M2c** section (ready-to-paste
Options.cpp hunk, all platforms, REQUIRED for every flag-on JIT leg), the
corrected M4a Darwin-ordering note (the wrong "no ordering constraint on
Linux" classification is retracted in place), and the apply-order checklist
step 2 now reading M1 + M2a + **M2c** + M4a. Deliberately NOT "fixed" by
weakening the RELEASE_ASSERTs to lazy init: the offset query could lazily
self-init on the JIT thread, but mutator threads' TLS tag words would then be
silently stale (read as TID 0 = main-thread ownership) — exactly the unsound
default the loud assert exists to prevent.

---

# Review round 3 — adversarial-review dispositions

Same rules as rounds 1-2: every blocker/major finding verified against the
tree; fixes are in owned paths; on divergence with earlier sections THIS
section wins (it post-dates rounds 1 and 2).

## R3-1 (major, FIXED): fireAllUnderClassAStop branch (1) fired inline on per-VM evidence without the R2-4 tripwire or the stub witness

CONFIRMED: the R2-4 fix covered only stopTheWorldAndRun's R1.h path; branch
(1) of `WatchpointSet::fireAllUnderClassAStop` (Watchpoint.cpp) checks
`worldIsStopped(vm)` — whose disjuncts include the PER-VM legacy GC stop —
and called `fireAllNow` directly, with (a) no entered-VMs tripwire and (b) no
`s_stubWorldStoppedDepth` raise, so a fire that invalidates without
jettisoning completed invisible to the VM-less patching asserts. Fixed
exactly per the finding's suggestion: the R1.h-path logic (scoped tripwire +
witness raise + F5 fence on exit) is now the RAII
`JSThreadsSafepoint::AlreadyStoppedWorldWitnessScope`
(bytecode/JSThreadsSafepoint.{h,cpp}), used by BOTH stopTheWorldAndRun's R1.h
path (refactored onto it; behavior identical modulo R3-11 below) and branch
(1), which wraps its `fireAllNow` in the scope. Nesting is free (depth
counter). Deleted at M4 with the rest of the stub counter.

## R3-2 (major, FIXED): PropertyInlineCache::reset dropped the inlined unit handler inline, bypassing I9 retirement

CONFIRMED: `reset()` called `clearInlinedHandler` directly (inline Ref drop)
even flag-on, inconsistent with initializeWithUnitHandler /
resetStubAsJumpInAccess — and because reset() nulls `m_inlinedHandler` BEFORE
the per-accessType reset functions reach resetStubAsJumpInAccess, that
function's displacedInlinedHandler retire arm was dead on the reset() path
while reset() is reachable flag-on OUTSIDE any stop
(fireWatchpointsAndClearStubIfNeeded in bytecode/Repatch.cpp;
PropertyInlineCacheClearingWatchpoint). Fixed in
bytecode/PropertyInlineCache.cpp: reset() now copies the inlined handler into
a RefPtr flag-on and routes it through `RetiredJITArtifacts::retireHandlerChain`
(same pattern as the other displacement sites), with a comment recording that
the resetStubAsJumpInAccess arm stays for its non-reset() callers. All four
displacement sites now agree: displaced inlined handlers are ALWAYS retired,
never inline-dropped, flag-on.

## R3-3 (major, RECORDED as GIL-removal precondition 11 + code caveats): concurrent slow-path call linking is unserialized

CONFIRMED as a real phase-B (post-GIL) double-retire/double-delete and
mirror-field-tearing hazard; NOT a bug under the phase-1 GIL (all the writers
are mutator slow paths; exactly one mutator exists, and the publish protocol
is reader-safe). Disposition: the previously-unrecorded gap is now **GIL-removal
precondition 11** (consolidated list above), covered by the
`gilRemovalPreconditionsMet()` constexpr tripwire, with caveat comments at
both `publishRecord` definitions in bytecode/CallLinkInfo.cpp naming the
unserialized writers and the required fix shape (owner CodeBlock::m_lock
serialization + CAS swap so a losing linker retires its OWN record).
Deliberately NOT "fixed" by adding the lock now: flag-off it is pure cost on
hot linking paths, and the CAS form interacts with the M4-deferred LLInt call
record (precondition 1) — the two should land together.

## R3-4 (major, FIXED via comments + new manifest M7): the entered-VM premise checks are check-then-act

CONFIRMED: both sampled counts (requesting path and the R1.h/witness-scope
path) are TOCTOU — VM entry does not consult the stub, so a second VM can be
entered during `work()` with every assert passing. Disposition per the
finding's suggested fix: the premise is made STRUCTURAL at the enforcement
point that actually exists pre-M4 — VM entry. New manifest section **M7**
(runtime/VMEntryScope.cpp, ready-to-paste): a process-global atomic
entered-VM counter check-and-incremented at top-level VM entry,
RELEASE_ASSERTing sole entry and no-open-stop-window under useJSThreads, so a
second concurrent entry crashes deterministically on the ENTERING thread
regardless of interleaving; apply-order step 4a; deleted at M4. Both sampled
count sites in bytecode/JSThreadsSafepoint.{h,cpp} now carry R3-4 comments
stating explicitly that they are probabilistic tripwires, that M7 is the
structural guard, and that flag-on + >1 concurrently-enterable VM is an
UNSUPPORTED configuration until M7 is applied. (The counts are kept: they
catch the misconfiguration at the firing site too, with better diagnostics.)

## R3-5 (major, FIXED via recorded rule + code comment): doJettison acquires CodeBlock::m_lock inside the stop closure

CONFIRMED as an M4 parked-holder deadlock hazard (harmless pre-M4: single
mutator, lock always free). The caller contract indeed constrains only the
REQUESTER; locks acquired INSIDE conducted closures that a remote parked
mutator may hold were uncovered, and the old "carries over verbatim" claim
baked the hazard in. Disposition: rule (a) chosen and recorded — flag-on,
ConcurrentJSLock critical sections must be PARK-FREE, enforced by a
RELEASE_ASSERT (no ConcurrentJSLock held) in M4's park hook — as new M4-rest
item 5a (amending item 5's verbatim claim in place), plus a caveat comment at
the doJettison m_lock site in bytecode/CodeBlock.cpp. Hoisting the lock above
the stop request was rejected: it would violate the requester contract
("caller holds no section-7 lock") and invert the lock/stop order for every
OTHER jettison trigger.

## R3-6 / R3-9 (blockers, ORCHESTRATOR ACTION — re-confirmed OPEN a third time): out-of-owned-path writes (assembler x4 + JSTests/threads/jit/**)

CONFIRMED-as-described and still not actionable from inside the jit
workstream. The R1-9/R1-12 section above now carries the round-3
re-confirmation; hunks re-verified verbatim-accurate; the
either/or (ratify carve-outs vs revert + apply hunks as integrator M-entries)
is unchanged and remains a hard pre-flag-on-merge gate.

## R3-7 (major, FIXED): LLInt enumerator get/put threaded out-of-line paths lacked the R7/F7 structureID->butterfly dependency

CONFIRMED — same bug class as R1-1, and the finding is right that the I14
row presented the twins as fully converted while precondition 6 did not list
them: these accesses are STRUCTURE-bounded (index validated against the
enumerator's cached structure), the cell-SID compare was a memory-operand
bineq (no register held the cell's structureID, and a control dependency does
not order load->load on ARM64), so a stale flat butterfly could be paired
with a structure promising more out-of-line slots => OOB read (get) / write
(put), uncaught by the TID/SW predicates (the stale butterfly carries the
owner tag). Fixed in llint/LowLevelInterpreter64.asm mirroring the R1-1
op_put_by_id form: `op_enumerator_get_by_val` loads the cell SID into t3
(register-register compare; ifJSThreadsBranch scratch moved to t7 so t3
survives) and `.outOfLineThreaded` calls
`butterflyLoadDependsOnStructureID(t3, t0, t7)` before the m_butterfly load;
`op_enumerator_put_by_val` likewise via t6 (free across the intervening
sequence; noted in the comment) with the dependency emitted before its
threaded butterfly load. Flag-off codegen shape unchanged except the
register-form compare. The R7 wiring list and I14 row above are updated in
place. (The lint.sh R7-inventory extension is NOT done here: lint.sh lives
under JSTests/threads/jit/**, whose ownership is the open R3-6 adjudication —
extending it is recorded as part of that resolution.)

## R3-8 (major, ALREADY RECORDED — re-affirmed, no code action): regime-2 fast paths absent in every tier

CONFIRMED as the known, loudly-recorded state — the finding itself verifies
the recording (GIL-removal precondition 9 incl. the R2-5 bench addendum) and
the unreachability of the seven RELEASE_ASSERT_NOT_REACHED R3 shims (Task 8
routes every predicate failure to existing generic paths; the shims are
referenced by no emitter and cannot be filled until OM's spine-accessor
DEFINITIONS land — runtime/ConcurrentButterfly.h declares them only).
Standing disposition unchanged: precondition 9 + the constexpr-false
tripwire gate GIL removal; the shim fill + bench leg are
orchestrator-scheduled joint work with OM. Nothing new to fix in owned code;
this entry exists so round 4 sees the re-affirmation.

## R3-10 (major, FIXED in manifest): M3 apply-order step 1 omitted the mutual link dependency with INTEGRATE-objectmodel entry 2

CONFIRMED: JSThreadsSafepoint.cpp reads `g_jsThreadsStubWorldStopped`
(defined only in runtime/ConcurrentButterfly.cpp, not yet in Sources.txt) and
ConcurrentButterfly.cpp calls `JSThreadsSafepoint::*` (defined only in the
M3-added TU) — either Sources.txt change alone is an undefined-symbol link
failure. Fixed in place: the M3 section now carries a LINK COUPLING paragraph
and apply-order step 1 says "M3 together with INTEGRATE-objectmodel.md
entry 2".

## R3-11 (major, FIXED): the R2-4 tripwire false-positived under a shared-server all-clients stop

CONFIRMED: the R2-4 count treated the shared-server all-clients stop as
per-VM evidence, but it is a GENUINE stop of every mutator of that server —
in the {useJSThreads=1, useSharedGCHeap=1, N clients} pre-M4 config a
GC-end-finalizer jettison would count N entered-but-parked clients and
RELEASE_ASSERT on a perfectly legal fire. Fixed per the finding's first
option (scoped count, not the SKIP-the-leg alternative — the Task-13 bench
plan keeps its pre-INT shared-heap leg) in
`assertAlreadyStoppedEvidenceCoversEveryMutator`
(bytecode/JSThreadsSafepoint.cpp): when `vm.clientHeap.server()` is a shared
server currently `worldIsStoppedForAllClients()`, entered VMs whose client
heap attaches to THAT server are excluded (they are parked by the stop), and
the remaining count must be ZERO (any other-heap entered VM is genuinely
concurrent — raising the global witness would be unsound, exactly the R2-4
scenario). Without such a stop the original `<= 1` (the caller itself)
stands. Note the fix also corrects the evidence disjunct itself:
`worldIsStopped(VM&)` now consults `vm.clientHeap.server()` rather than the
VM's own (possibly idle under sharing) `vm.heap` member for the all-clients
disjunct — for the 1:1 case the two are the same object.

---

# Review round 4 — adversarial-review dispositions

Same rules as rounds 1–3: every blocker/major finding verified against the
tree; fixes are in owned paths; on divergence with earlier sections THIS
section wins (it post-dates rounds 1–3).

## R4-1 (blocker, FIXED — also filed as a duplicate major): R1.i GC-serialization bracket keyed on vm.heap, skipped for clients of a shared server

CONFIRMED — a fix-introduced asymmetry of R3-11, exactly as filed: the round-3
fix corrected the read side (`worldIsStopped(VM&)`,
`assertAlreadyStoppedEvidenceCoversEveryMutator` both resolve
`vm.clientHeap.server()`) but the write-side bracket in
`stopTheWorldAndRun`'s requesting path still tested `vm.heap.isSharedServer()`
and constructed both scopes on `vm.heap`. Since `m_isSharedServer` is set on
the SERVER Heap only, every requesting-path stop (reoptimization jettison,
Class-A fire) initiated from a CLIENT VM of a shared server skipped the
bracket: no heap-access release, no rank-2 GC conductor lock, `work` patching
code while a shared-mode GC could start or be mid-cycle — the CS2 corruption
window, in the {useJSThreads=1, useSharedGCHeap=1, N clients} config R3-11
itself legitimized. Fixed in `bytecode/JSThreadsSafepoint.cpp`:

* The server is resolved as `vm.clientHeap.server()` (under the same
  `__has_include("HeapClientSet.h")` gate as `worldIsStopped(VM&)`, with a
  `vm.heap` fallback that is sound because without the client-set machinery
  no foreign-client shared server can exist); the bracket tests
  `server.isSharedServer()` and `JSThreadsStopScope` is constructed on the
  resolved server (which also self-gates internally, so the 1:1 case is
  unchanged).
* Heap-access release is now CLIENT-scoped: a new local
  `ClientHeapAccessReleaseScope` calls
  `vm.clientHeap.releaseHeapAccess()/acquireHeapAccess()` instead of
  `ReleaseHeapAccessScope(vm.heap)`. Rationale (confirmed against heap §10A
  in Heap.h): once ISS, server-level `releaseAccess()` FORWARDS TO THE MAIN
  CLIENT — correct only when the requester is the main client; the
  GCClient::Heap entry points act on the requester's own client in every
  case. Declaration order keeps the spec resume order (stop scope destroyed
  before access reacquired).
* Gate interplay recorded at the site and in the Task-5/CS2 sections:
  `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` (defined by Heap.h itself) and
  `JSC_JIT_HAS_SHARED_HEAP_SERVER` (HeapClientSet.h presence) are independent
  #ifdefs; the code is correct under all four combinations.
* The Task-5 R1.i bullet, M4-rest item 5, and the CS2 disposition were
  amended in place: the shape M4 carries over verbatim is THIS one, not the
  round-3 `vm.heap` one.

## R4-2 (major, FIXED): epoch retirement routed to vm.heap's safepointEpoch — wrong heap for clients of a shared server

CONFIRMED, same root cause as R4-1 on the §4.4 axis: every
`RetiredJITArtifacts` call site passed `vm.heap`/`codeBlock->vm().heap`, but
the §4.4 soundness argument requires the epoch to be advanced by the
safepoints of the mutator population that can hold pointers into the retired
data — under useSharedGCHeap that is the SERVER's epoch, not the client's
idle local heap's (whose epoch semantics would give either premature free =
use-after-free for a server-tracked reader mid-dispatch, or never-free =
unbounded leak under IC churn). Fixed per the finding's second (stronger)
option so call sites cannot get it wrong:

* `RetiredJITArtifacts::retire` / `retireHandlerChain` now take `VM&` and
  resolve the epoch heap internally via a private `epochHeapFor(VM&)` =
  `vm.clientHeap.server()` (same HeapClientSet.h gate + vm.heap fallback as
  JSThreadsSafepoint.cpp; 1:1 case identical to before). Contract documented
  in RetiredJITArtifacts.h (R4-2 paragraph).
* All call sites updated: PropertyInlineCache.cpp (deref,
  resetStubAsJumpInAccess displaced-handler arm, initializeWithUnitHandler,
  reset slow-path republish), CallLinkInfo.cpp (`retireCallLinkRecord` now
  takes VM&; publishRecord/clearRecord pass `vm`;
  `DirectCallLinkInfo::retireRecord` passes `m_owner->vm()` instead of
  `*Heap::heap(m_owner)`). CodeBlock.cpp's jettison-time path goes through
  `PropertyInlineCache::deref(VM&)` and is covered transitively.

## R4-3 (major, FIXED): installVMTrapBreakpoint patched reachable code asynchronously with no useJSThreads tripwire

CONFIRMED — the Task-11 audit covered `JumpReplacement::fire()` but left the
sibling entry point unguarded; M2b (which forces `usePollingTraps` flag-on)
is deferred to Task 14 and not in tree, so a flag-on run with signal-based
traps would silently violate I2 from the VMTraps signal-sender thread. Fixed
with `RELEASE_ASSERT(!Options::useJSThreads() || Options::usePollingTraps())`
at BOTH `DFG::CommonData::installVMTrapBreakpoints` (dfg/DFGCommonData.cpp)
and `DFG::JumpReplacement::installVMTrapBreakpoint`
(dfg/DFGJumpReplacement.cpp), with comments cross-referencing M2b. The
assert form (rather than `!useJSThreads()` flat) is chosen so the site goes
quiet automatically once M2b lands. This closes the I2 hole fail-fast; the
structural fix remains M2b's force-set (unchanged, still Task-14).

## R4-4 (blocker, ORCHESTRATOR ACTION — re-confirmed OPEN a FOURTH time): out-of-owned-path writes (assembler x4 + JSTests/threads/jit/**)

Round-4 re-verification: the four assembler/ files
(X86Assembler.h PRE_FS/fs(), MacroAssemblerX86_64.h loadFromELFTLS64,
ARM64Assembler.h mrs_TPIDR_EL0, MacroAssemblerARM64.h loadFromELFTLS64) and
the JSTests/threads/jit/** tree are still in tree and still unratified. Not
actionable from inside the jit workstream (cannot self-ratify, cannot revert
non-owned files, git is off-limits to subagents this round). The verbatim
hunks recorded in the R1-9/R1-12 section remain accurate. The either/or is
unchanged and remains a hard pre-flag-on-merge gate: ratify both carve-outs
(amend the owned-path list) OR revert the four files and apply the recorded
hunks as integrator M-entries. Tasks 8–10 emission depends on the emitters
either way.

## R4-5 (major, FIXED — also filed as a duplicate major): LLInt loadButterflyTIDTagToT4 emitted LOCAL-EXEC TLS relocations

CONFIRMED: `@TPOFF` is R_X86_64_TPOFF32 and `:tprel_hi12:`/`:tprel_lo12_nc:`
are R_AARCH64_TLSLE_* — the LOCAL-EXEC sequences, rejected by the linker for
-fPIC shared-object builds ("relocation ... cannot be used when making a
shared object"), i.e. they linked only under ENABLE_STATIC_JSC (Bun's
default) while the macro comment, this manifest, and the
`tls_model("initial-exec")` contract in jit/ConcurrentButterflyOperations.h
all promised initial-exec. Fixed in llint/LowLevelInterpreter64.asm to the
true IE sequences (first suggested option; the config-slot alternative was
rejected because JSCConfig.h is non-owned and the Darwin M4a slot is the
charter for that shape):

* x86-64: `movq g_jscButterflyTIDTag@GOTTPOFF(%rip), %r8` +
  `movq %fs:(%r8), %r8` (one extra load; r8 == t4 throughout).
* arm64: `adrp x16, :gottprel:...` + `ldr x16, [x16, #:gottprel_lo12:...]` +
  `mrs x4, tpidr_el0` + `ldr x4, [x4, x16]`. The extra scratch is x16 —
  offlineasm's own transient temp (arm64.rb ARM64_EXTRA_GPRS); nothing is
  live in it between pseudo-instructions, noted in the macro comment.
* Comments at the macro now match the shipped model; the
  ConcurrentButterflyOperations.h IE contract needed no change (it was
  already correct — the asm was the deviation). The JIT-tier emitters
  (baked runtime-computed offset) were already model-agnostic and are
  untouched.
* Task-13 note: the macro sits behind `ifJSThreadsBranch`, so FLAG-OFF
  golden disassembly is unchanged (no re-record of the I1 baseline); the
  flag-on tag-discipline legs that disassemble LLInt write fast paths must
  re-bless the two/four-instruction IE shape when they next run.

## R4-6 (major, RE-AFFIRMED — no code action, matches the finding's own disposition): mandated TID/SW coverage incomplete under N mutators

The finding's round-4 verification matches ours: DFG64/FTL array-element
store predicates, the LLInt monomorphic call record, and all regime-2 fast
paths are absent, honestly recorded (GIL-removal preconditions 1/2/3/9/11 +
R2-5/R3-8), the seven R3 shims are unreachable today, and
`gilRemovalPreconditionsMet()` is the constexpr-false hard gate on
second-mutator attach. Scheduling owners (preconditions 1, 2, 3, 9 jointly
with OM) remain an orchestrator action. Nothing new recorded beyond this
re-affirmation entry.

## R4-7 (major, RE-AFFIRMED — recorded ordering hole, gated): deferred watchpoint fires publish the watched fact before the stop

As the finding itself verifies: the direct `fireAllSlow` path is correctly
stop-conducted and the R3-1 witness scope covers the inline branch; the
DEFERRED form's fact-before-fire ordering is the one remaining hole, loudly
recorded at the ORDERING CAVEAT in bytecode/Watchpoint.cpp and as
GIL-removal precondition 10 (with the mandatory Task-11
"fact published before fire?" column / TTL-set restructure options). It is
sound under the phase-1 GIL and remains gated by
`gilRemovalPreconditionsMet()`. No code action in this round; this entry
exists so round 5 sees the re-affirmation.

## R4-8 (major, RE-AFFIRMED — integration dependency, enforce the checklist): the stop primitive is an inline-run stub until M2c/M4/M4a/M7 land

Accurate description of the agreed phasing, all of it already recorded: the
stub never parks anyone, the entered-VM counts are sampled tripwires (R3-4),
M7 is the structural pre-M4 guard, M4/M4a are the real stop and Darwin key
slot, and M2c prevents the deterministic first-emission RELEASE_ASSERT. The
apply-order checklist already sequences these (M2c at step 2 before any
flag-on leg; M7 at step 4a before any multi-VM flag-on; M4/M4a before GIL
removal). Round-4 action: none in code; the checklist's step-2 text already
carries the R2-6 "hard precondition of EVERY flag-on JIT leg" language. The
integrator MUST treat steps 1–2 as preconditions of the FIRST flag-on run,
not of GIL removal.

## R4-10 (major, FIXED in manifest): M3 LINK COUPLING was still incomplete — step 1 also needs INTEGRATE-vmstate.md M2

CONFIRMED: with runtime/VMLite.h present, jit/ConcurrentButterflyOperations.cpp
compiles `JSC_JIT_HAS_VMLITE`, drops its local shim, and references
`currentButterflyTID` + `setVMLiteTIDTagHook` — defined only in
runtime/VMLite.cpp, which is NOT in Sources.txt (verified: grep is empty) and
is added only by INTEGRATE-vmstate.md M2. So the R3-10 pairing (M3 + OM
entry 2) still does not link. Fixed in place: the M3 section gains a LINK
COUPLING ADDENDUM, apply-order step 1 now reads {M3 + OM entry 2 + vmstate
M2} as one atomic Sources.txt step, and the CS3 disposition cross-records the
dependency its "P5 init registers the hook for real" note creates.

## R4-12 (major, FIXED in manifest): M7's unconditional sole-entry assert contradicted the R3-11 shared-server carve-out

CONFIRMED: as drafted, M7 crashed the second client of a shared server at
ENTRY, making R3-11's carve-out dead code and the Task-13 pre-INT
shared-heap N-client leg unreachable. Fixed by choosing envelope (a) — M7
now mirrors R3-11: the M7 hunk keeps a strict sole-entry assert for legacy
(non-shared) VMs, but permits N concurrently-entered clients of ONE shared
GC server (sticky identity slot, two counters so legal concurrent
same-server entries cannot false-positive, mixed legacy+shared crashes from
both sides). Envelope (b) (declare the config unsupported, delete the
carve-out) was rejected because Task 13 keeps the shared-heap leg. The M7
section also records the residual envelope notes: one shared server per
process pre-M4, and requesting-path (non-already-stopped) stops from one of
N entered clients still trip the stub's `enteredVMs <= 1` count by design —
pre-M4 shared-server stops must arrive via already-stopped GC windows (the
R1.h path).

## Round-4 cross-cutting note

R4-1/R4-2 together close the two write-side consumers that R3-11's read-side
fix had left keyed on `vm.heap`; after this round, every shared-server-aware
site in owned code resolves `vm.clientHeap.server()` under the single
`JSC_JIT_HAS_SHARED_HEAP_SERVER` gate (code sites: JSThreadsSafepoint.cpp x3
— tripwire, evidence disjunct, R1.i bracket — plus RetiredJITArtifacts.cpp's
epochHeapFor), and M7 is the only remaining place (manifest
text, non-owned target) that performs the same resolution — keep them in
lockstep if the heap workstream renames the accessor.

---

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

(Applied by the integrator per INTEGRATE-objectmodel.md §60; recorded here so
the two manifests agree. No transition fast path is emitted yet — the three
Repatch GiveUpOnCache gates stand — so this is a constraint on the future
E4 emission, GIL-removal precondition 5.)

## AB18-E / S1 (major, FIXED — landing record; recorded one round late, flagged by the AB18 verify review): DirectCallLinkInfo::retireRecord derived the retire VM from a dead owner cell (MarkedBlock::vm() UAF)

* **Signature (sig-1, standalone capture):** ASAN heap-use-after-free in
  `MarkedBlock::vm()` reached via `m_owner->vm()` inside
  `DirectCallLinkInfo::retireRecord`, reproduced ~1/10 standalone by
  `JSTests/threads/jit/ic-publish-reset-loops.js` under the pinned GIL-off
  flags. (The full symbolized stack lives in the AB18-E implementer report;
  it was not copied into this ledger when the fix landed — that omission is
  what this entry repairs.)
* **Mechanism:** `RetiredJITArtifacts::retireOptimizedJITCode` flag-on
  DELIBERATELY leaks optimized JITCode (N6 leak-until-integration,
  `RetiredJITArtifacts.h` ~96-117), which keeps `DirectCallLinkInfo` nodes
  alive — and validly enqueued on callees' `m_incomingCalls` lists — past
  their owner CodeBlock's death. A drain
  (`CodeBlock::unlinkOrUpgradeIncomingCalls` under a jettison stop) then
  legitimately reaches `retireRecord` on a node whose `m_owner` is a swept
  cell in a freed MarkedBlock; the pre-fix `m_owner->vm()` dereferenced that
  freed block header. The owner-derived form was introduced by R4-2 (epoch
  heap resolution), which is why no earlier round saw it.
* **Fix delta:** `retireRecord` (CallLinkInfo.cpp:801-815) now takes the
  retiring mutator's `VM&` from the caller; plumbed through `clearRecord`,
  `Repatch.h:91` / `linkDirectCall` (Repatch.cpp:2362-2371, AB18-D writer
  set), and the DFG operation call site (DFGOperations.cpp,
  `linkDirectCall(vm, ...)`). Rule (AB18-E): a retire-path VM must come from
  the operation/caller, NEVER be re-derived from a cell.
* **Epoch semantics unchanged:** flag-on `epochCoversEveryJSThread()` is
  constant-false, so retirement still leaks (the free side is untouched);
  the fix removes only the dead-owner dereference on the drain path. The
  drain path was audited for remaining dead-owner dereferences
  (unlinkOrUpgradeImpl / reset / clearRecord / setCallTarget / visitWeak /
  PolymorphicCallNode): none remain.
* **Family closures landed with this record (AB18 verify round):**
  1. `PropertyInlineCache::reset` was the one surviving cell-derived retire
     VM (`codeBlock->vm()` at PropertyInlineCache.cpp); it now takes `VM&`
     from every caller (Repatch.cpp `fireWatchpointsAndClearStubIfNeeded`,
     `PropertyInlineCacheClearingWatchpoint::fireInternal` — which previously
     RECEIVED a VM& and discarded it — and the visitWeak-driven GC reset).
  2. `createPreCompiledICJITStubRoutine` now calls `makeGCAware` at creation
     flag-on, retiring the unserialized lazy `makeGCAware` promotion in
     `RetiredJITArtifacts::retireHandlerChain` (old FIXME (a)): two mutators
     retiring chains sharing one stateless precompiled stub could race the
     `!isGCAware()/makeGCAware()` pair and double-append to
     JITStubRoutineSet — a double-jettison/double-free in this same family.
     The lazy promotion is now flag-off-only with a flag-on fail-stop.
* **OPEN — evidence gap on the second half of sig-1:**
  `jit/shared-arraystorage-stress.js` (30/30 standalone, fails under
  whole-corpus load) is attributed to this same root cause by signature
  family ONLY. No ASAN capture or symbolized stack from a corpus-load
  failure of THAT test is on record. Per the flaky-bug closure rule, sig-1
  must NOT be declared fully closed until either (a) one corpus-load failure
  of shared-arraystorage-stress is captured on an ASAN binary and the stack
  matches retireRecord/MarkedBlock::vm(), or (b) the corpus-load
  configuration passes a pinned-seed pre-fix-vs-post-fix A/B. If its stack
  turns out to be the AB17f item-6 F4 baseline-IC-repatch family (NO landed
  fix), V3 stays red. The pinned V3 rung must be re-run on the post-fix
  binary either way (AB17f item-8 stale-baselines rule).

## AB18-F (review-round repair of the AB18-E family closure): four residuals fixed; sig-1 remains HALF-CLOSED

Adversarial review of the AB18-E landing found the family closure overstated.
Verified against the tree and fixed in this round:

1. **(major, FIXED) `PropertyInlineCache::reset` tail still derived the retire
   VM from the owner cell.** The function took `VM&` per AB18-E and used it
   for the inlined-handler retire, but ended with `deref(codeBlock->vm())`
   (PropertyInlineCache.cpp:505) — the exact MarkedBlock-header read the
   in-function comment bans. Now `deref(vm)`. Note the deeper repatch helpers
   (`resetGetBy`/`resetPutBy` -> `resetStubAsJumpInAccess`) still derive
   `codeBlock->vm()`; that is acceptable ONLY because item 3 below closes the
   dead-owner reachability of reset() — if a new reset path through
   retired/leaked state ever appears, those helpers need the same plumbing.

2. **(blocker, FIXED) `JITStubRoutineSet::add` was an unlocked Vector append
   reachable from N mutators.** Under useVMLite all threads share one
   Heap/JITStubRoutineSet; `makeGCAware` runs on IC-miss slow paths holding
   only per-CodeBlock locks, and the AB18-E FIXME(a) closure
   (`createPreCompiledICJITStubRoutine` -> makeGCAware at creation) put it on
   the dominant flag-on data-IC path. The pre-existing
   `createICJITStubRoutine` sites had the same exposure. add() now takes a
   JITStubRoutineSet-internal Lock (GC-side members stay lock-free: they run
   with mutators stopped). This was a candidate mechanism for the
   shared-arraystorage corpus-load failure that the family-signature
   attribution could not exclude.

3. **(blocker, FIXED) Displaced handler chains kept
   `PropertyInlineCacheClearingWatchpoint` armed past owner CodeBlock death.**
   Flag-on, `RetiredJITArtifacts::retireHandlerChain` leaks (or epoch-defers)
   displaced chains; each leaked handler's watchpoint stayed INSTALLED on a
   live WatchpointSet with `m_owner` = the CodeBlock. `~CodeBlock`'s
   aboutToDie() walk covers only ATTACHED chains, so a post-sweep fire read
   the dead cell (fireInternal's guard dereferences are themselves the UAF)
   and then ran the full reset/repatch chain against it — an uncovered,
   still-live instance of the sig-1 UAF family. retireHandlerChain now
   disarms every node's watchpoint (new
   `InlineCacheHandler::disarmClearingWatchpointOnRetire`) on BOTH the epoch
   and leak arms, matching flag-off inline-destruction semantics at the same
   program points.

4. **(major, FIXED) `linkDirectCall` gilOff loser path could strand the node
   on the WRONG callee's incoming-calls list.** The isOnList() skip assumed
   both racers resolved the same calleeCodeBlock; a tier-up install between
   the racers' resolutions breaks that, leaving the published record pointing
   at CodeBlock B while the node sits on A's m_incomingCalls — jettisoning B
   then cannot unlink this caller (jettison/int-gate regression class). Under
   s_callLinkSerializationLock, isOnList() implies the node is on
   `codeBlock()`'s list, so the linker now delists on mismatch before
   republishing and relinks against the resolved callee.

**Status correction (gate enforcement):** AB18-E's "ROOT-CAUSED AND FIXED"
claim for sig-1 applies to the standalone ic-publish-reset-loops capture
ONLY. Per the OPEN clause above, sig-1 stays HALF-CLOSED until (a) an ASAN
corpus-load capture of shared-arraystorage-stress matches a landed fix's
stack, or (b) the pinned-seed pre/post corpus-load A/B passes. Items 2 and 3
above are additional candidate mechanisms for that second half, so the A/B
must be run on a binary containing THIS round's fixes, and the pinned V3
rung must be re-run regardless (AB17f item-8 stale-baselines rule).

## AB18-G (adversarial review of AB18-F): watchpoint MEMBERSHIP lock; flag-off lock gating; retire-VM plumbing completed. sig-1 REMAINS HALF-CLOSED.

Review of the AB18-F landing found the item-3 disarm fix itself racy, plus a
broader uncovered instance of the same mechanism class, plus a rule violation
and an unfinished plumbing. All verified against the tree and fixed:

1. **(blocker, FIXED) `disarmClearingWatchpointOnRetire` performed an
   unsynchronized SentinelLinkedList remove() on a WatchpointSet shared
   across CodeBlocks.** Stubs are shared cross-thread via the per-VM
   SharedJITStubSet (one VM under useVMLite => one m_sharedJITStubs for all
   threads; getStatelessStub at InlineCacheCompiler.cpp:7518 and find at
   :8247 both hand an existing stub to a NEW CodeBlock's IC, which then
   add()s its own clearing watchpoint to the SAME stub watchpointSet at
   :5114/:7472/:7494). The AB18-F disarm (remove) on a retiring mutator
   therefore raced add() on a compiling mutator — trading the item-3 UAF for
   list corruption in the same family. The in-code claim that the disarm had
   "the same serialization constraints" as flag-off inline destruction was
   wrong flag-on (flag-off's constraint is the single mutator) and has been
   rewritten.

2. **(blocker, FIXED) WatchpointSet membership was unserialized across N
   mutators generally** — `WatchpointSet::add` (bare m_set.push),
   `Watchpoint::~Watchpoint` (bare remove()), `~WatchpointSet`'s drain
   (reachable from lazy sweep on a LIVE mutator per AB18-C),
   `WatchpointSet::take`, the unlink step of `fireAllWatchpoints` (Class-B
   fires run un-stopped), `AdaptiveInferredPropertyValueWatchpointBase::
   fire`'s direct removes, and the racy thin->fat
   `InlineWatchpointSet::inflateSlow` (two losers => one thread's installs
   land on a leaked losing set = silently disarmed watchpoint). Installs are
   reached flag-on from IC-miss/handler-compile slow paths holding only
   per-CodeBlock locks (ensureReferenceAndInstallWatchpoint ->
   Structure::addTransitionWatchpoint on the SHARED object model;
   stub->watchpointSet().add on shared stubs), matching the handout's own
   lock-class charter for installs (K4.III.18/VI.2). Fix: a single global
   leaf lock `g_watchpointMembershipLock` (Watchpoint.h), taken ONLY under
   Options::useJSThreads(), covering every membership link/unlink and the
   inflation double-check. FIRING never runs under it (Class-A fires are
   stop-conducted; fireAllWatchpoints releases it before fire()). Class-B
   fire-vs-destroy watchpoint LIFETIME (not membership) is pre-existing and
   out of scope here.

3. **(major, FIXED — rule compliance) `JITStubRoutineSet::add`'s AB18-F lock
   was unconditional**, i.e. new flag-off work without a V5b re-run (the
   standing ab17c rule). The Locker is now gated on Options::useJSThreads(),
   matching the makeGCAware-at-creation gating one frame up; flag-off takes
   no atomic. The "GC phases touching JITStubRoutineSet stay STW" assumption
   the lock-free GC-side members rely on is now recorded in the header as a
   SPEC-congc precondition.

4. **(major, FIXED) AB18-E retire-VM rule applied structurally.** The
   surviving `codeBlock->vm()` derivations on retire paths
   (initializeWithUnitHandler, resetStubAsJumpInAccess x2, and the
   resetGetBy/resetPutBy/... helpers AB18-F item 1 accepted conditionally)
   are gone: VM& is now plumbed from the operation/caller through
   initializeWithUnitHandler / prependHandler / rewireStubAsJumpInAccess /
   resetStubAsJumpInAccess and the whole Repatch reset*/repatch*SlowPathCall
   family (JITOperations call sites pass the operation's vm). The only
   remaining derivations feeding these paths are LINK-time
   (initializeFromDFGUnlinkedPropertyInlineCache /
   initializeHandlerForOptimizingJIT), where the codeBlock is being
   installed and provably live; commented as such. The closure no longer
   rests on the reachability argument alone.

**Status (gate enforcement, unchanged from AB18-F):** sig-1 remains
HALF-CLOSED and V3 stays RED. The shared-arraystorage-stress corpus-load
half is attributed by signature family only; items 1-2 above are additional
candidate mechanisms that no prior binary contained. Before any green claim:
(a) rebuild ASAN with THIS round's fixes and either capture one corpus-load
shared-arraystorage failure whose stack matches a landed fix, or pass the
pinned-seed pre/post corpus-load A/B on the post-AB18-G binary; (b) re-run
the pinned V3 rung (AB17f item-8 stale-baselines rule); (c) V5b bench gate
needs no re-run for this round (the only flag-off-reachable change is the
now-GATED JITStubRoutineSet lock and a single predicted-not-taken branch in
the watchpoint slow paths), but any future unconditional lock re-triggers
the rule. The TSAN binary (WebKitBuild/TSan) still predates all ungil work
and must be rebuilt before its rung means anything.
