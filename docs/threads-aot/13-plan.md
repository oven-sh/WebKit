# 13. Plan

Stages, work packages, what can be cut, what comes later, and the decisions that need an owner.

## 13.1 Order

The order is chosen so that each milestone can be tested with what exists at that point, and so that the largest unknown (what the mt flavour costs a program on one thread) is measured before most of the work is done.

```
stage 0  flavour and scaffolding
stage 1  per-thread state, one thread          -> first measurement of gate 1
stage 2  compiler rules, one thread            -> measurement of gates 2, 3
stage 3  a second thread: compute
stage 4  jobs, modules, event loop
stage 5  natives      |  stage 6  interpreter  |  stage 7  ARM64, macOS, Windows      (in any order, or at once)
stage 8  hardening
```

| Stage | A program can | Proven by (12) |
| --- | --- | --- |
| **Stage 0** | be built by `bun-aot-mt` and behave as before | existing tests in the mt flavour; gate 0 |
| **Stage 1** | the same, with every piece of per-thread state where it will be | existing tests; collector stress; **gate 1** |
| **Stage 2** | the same, compiled as if everything were shared | 12.3 with `EverythingIsShareable`; 12.4; gates 2, 3 |
| **Stage 3** | start threads that compute on shared objects and arrays, synchronize with `join`, `Lock`, `Condition`, `Atomics`, and print | 12.2; 12.5; 12.8; the parts `api`, `sync`, `invariants`, `shared-objects` of 12.7; kernels of 12.10; gates 4 to 7 |
| **Stage 4** | use `async`/`await`, promises, timers, `import()` on threads | the rest of 12.7; every program of `test/aot/programs` wrapped in a thread |
| **Stage 5** | use Bun's APIs on threads | 12.9; servers on N threads |
| **Stage 6** | run text on threads | the 32 tests with `eval`; zod, ajv on threads |
| **Stage 7** | do all of it on Linux arm64, macOS, Windows | 12.2 (M6 publication above all), 12.7 on those lanes |
| **Stage 8** | be trusted | campaigns of 12.5; TSan at zero; fuzzers; all gates |

A first public preview is possible after stage 4 with stage 5's refusals in place: threads that compute, await and print, on Linux x64.

## 13.2 Work packages

Size: S under 500 lines, M under 2,000, L under 6,000, XL more. "Lines" are changed or new lines outside tests.

### Stage 0: Flavour and scaffolding

| # | Package | Where | Size | Done when |
| --- | --- | --- | --- | --- |
| W-0.1 | `ENABLE(JS_THREADS)`, `cfg.threads`, flags, Cargo cfg, artifact name, prebuilt suffix | 03, section 3.1 | S | `bun run build --threads=on` links |
| W-0.2 | `AdmittedCode::Threads`, `ImageFlag::RunsThreads`, recorded option, template check | 03, section 3.3 | S | an image of one flavour is refused by the other with a sentence |
| W-0.3 | `bun:threads` resolves; empty classes; types; table rows; feature `Threads` | 03, sections 3.4, 3.5.2 | M | `import { Thread } from "bun:threads"` type checks and builds; is an error elsewhere |
| W-0.4 | the name `bun:threads` known in all flavours; detection at load, hand-over both ways, `--threads`, `bun run --aot` compiles after all | 03, sections 3.3, 3.7 | M | tests after the model of `test/aot/*hand-over*` |
| W-0.5 | vocabulary of the table: `on`, `thrownTo`, `synchronizes`, `blocks`, `threads`; `BuiltinFlag` 32 bits; `classifyThreads` with everything `main` | 03, section 3.5.1 | M | the mt build fails for an unclassified native |
| W-0.6 | the changes of 13.7 offered to the compiler's branch | | M | merged there |
| W-0.7 | an mt engine never generates code at run time (P3); commands that run source hand over to `bun-aot` or interpret; `flavour` byte and run-time facts in the template's description; layout hash | 03, sections 3.1, 3.3 | S | `bun-aot-mt run x.ts` works; a template of the other flavour is named in words |

### Stage 1: Per-thread state, one thread

| # | Package | Where | Size | Done when |
| --- | --- | --- | --- | --- |
| W-1.1 | `ThreadContext`; `VM` members become accessors | 07, section 7.2 | L | no data member of the list is left in `VM` in mt builds |
| W-1.2 | Instance layout: 64 KB regions, private statics chunk, shared and private regions; memory object; canonical view and one thread's view | 07, section 7.3 | L | the main thread runs on a view of its own; the canonical view's private regions are read-only |
| W-1.2a | `SharedInstance` / `Instance` / `ThreadState` split (V3 to V7): 65 uses of `Instance::of(realm)`, 177 of `State::instance`, 253 of `m_instance` members | 07, section 7.3.1 | XL | no `Instance&` is reachable from an object that two threads can reach; the whole test suite runs without a fault on the canonical view |
| W-1.2b | a mark on each member of `Instance` and `InstancePrefix`; the generator of view set-up (V9); `VMState::views` | 07, section 7.3.3 | M | the build fails for a member without a mark |
| W-1.2c | the `thread` word of the view and the fixed head of `ThreadContext` (V8) | 07, section 7.3.4 | M | `VM::offsetOfTopCallFrame()` and its like do not exist in mt builds |
| W-1.3 | adapters read the view from thread-local storage; runtime fact; fallback operation | 07, section 7.3.4 | M | Linux x64 |
| W-1.4 | caches of `VMState`, `State`, `VM` move to the thread | 07, section 7.4 | L | K1: no cache is reachable from two contexts |
| W-1.5 | K2: no value caches for writable locations | 07, section 7.4 | S | perf rows for global reads recorded |
| W-1.6 | `MutatorContext`; allocators per thread; `ThreadSubspaceAllocators`; client heap per thread | 08, sections 8.2, 8.8 | L | collector stress passes |
| W-1.7 | access protocol, `requestStop`, safepoint requests beside the trap events, polls park, `EntryIsSafepoint` operation, the collector thread conducts | 08, sections 8.3, 8.4, 8.10 | L | every collection of the test suite stops the one mutator at a `collects` operation |
| W-1.8 | `runSafepointOperation`, `freeAfterSafepoint`, `NoSafepointScope`, lock kinds and ranks | 08, sections 8.6, 8.9, 8.15 | M | unit tests in `testaot` |
| W-1.8a | `StorageRef` / `RevalidatedRef` over the run time (S1'); pins on buffers (H11) | 08, section 8.9 | L | debug builds assert a count of zero at each safepoint over the test suite |
| W-1.9 | `IndexingHeader` of 16 bytes, storage word written by every creator of a butterfly (C++, stubs, emitters, image writer), `offsetInButterfly` | 06, section 6.2 | L | debug builds assert kind and state at each `Butterfly` accessor |
| W-10.3 | the interpreter's handlers under `JS_THREADS` and its ways to the thread (I4): the layout at this milestone (the interpreter is the only way an mt engine runs source, P3), the protocols at stage 3 | 10, section 10.4 | L | JSC's tests pass in the mt flavour's interpreter |
| W-1.10 | `JSLock` as a counter; `doVMEntry` on the context | 09, section 9.3 | M | |
| W-1.11 | **Measure gate 1** | 12.11 | | a number. If over 3 %: find out why before stage 3 |

### Stage 2: Compiler rules, one thread

| # | Package | Where | Size | Done when |
| --- | --- | --- | --- | --- |
| W-2.1 | `ThreadEntries`, `HandedToThreads`, `ThreadsOfFunctions`, `SharedObjects`, witnesses | 04, section 4.2 | L | `valueflow/threads-*.ir` |
| W-2.2 | `--explain=threads`, statistics, diagnostics | 03, sections 3.8, 3.9 | M | docs test |
| W-2.3 | `synchronizes`, on both ends of each edge of 02, section 2.10.2; the epoch compare in inline acquires (E2a) | 04, section 4.4; 07, section 7.5 | M | `guards-synchronize.ir`; message passing shapes in IR |
| W-2.3a | the schema flag for operations that may wait for another thread (S4, S5); `Instance::prepareForThreads()` | 04, section 4.11; 07, section 7.6; 08, section 8.4 | L | a validating build fails when an operation without the flag parks |
| W-2.4 | `TryGetSlot` and the other terminators that read once and test the value (C3, with the verifier's structural check and the coverage test of 04, section 4.6.7), with their plans; `TryPutSlot`, `TryPutVariable`; no birth-flag proofs for Shareable layouts; the stubs that store by name, with a compare-and-swap per width | 04, sections 4.5, 4.6.1, 4.6.2 | L | |
| W-2.4a | static fields and other variables with markers (CO11 to CO13) | 05; 04, section 4.6 | M | |
| W-2.5 | `Storage`, `window` types, verifier, `AcquireStorage`, element plans, inline `push`/`pop`/`shift`/`length`; the array intrinsics take the kind from the storage; inline `Object.freeze` only on unpublished objects | 04, section 4.6.3; 06, section 6.3 | XL | gate 3 |
| W-2.6 | typed array views, Extensions (unpacked), `AtomicSlot` | 04, sections 4.6.4 to 4.6.6; 05, section 5.5 | L | |
| W-2.7 | order facts: `Concurrent` calls, O1, O2, O3 | 04, section 4.7 | M | lies caught by validation |
| W-2.8 | the table of passes; `PinLoopExitReads`, with the epoch compare in loops whose exit depends on an access by name; `MarkRecursiveEntries`; polls never elided | 04, sections 4.8, 4.11, 4.12 | L | one IR test per row |
| W-2.9 | B3: what `collects` patchpoints declare to write (04, C4); direct generator's new cases | 04, section 4.9 | M | `testb3` |
| W-2.10 | validating builds; testing switches | 04, sections 4.14, 4.15 | M | 12.3 passes |
| W-2.11 | per-thread variables, `$threadLocal` | 03, section 3.6 | M | |

### Stage 3: A second thread

| # | Package | Where | Size | Done when |
| --- | --- | --- | --- | --- |
| W-3.1 | `Thread`, `join`, IDs, start and end, minimal loop in `jsc` and `testaot` | 09, section 9.2 | L | |
| W-3.2 | views for spawned threads; smaller tables | 07, sections 7.3, 7.4 | M | gates 5, 7 |
| W-3.3 | N mutators: stop of all, scan of parked states, merge of mark stacks and logs, nursery scaling, exit | 08 | L | model 12.8 |
| W-8.4 | no re-whitening (H2): measure what it costs | 08, section 8.10 | S | model; a number |
| W-3.3a | start and end of a thread against stops (H3, H4, T3, T4); ID quarantine and its visitors (T1); the conductor's context (H14) | 08, section 8.14; 09, section 9.2 | L | model with a conductor, two mutators, one starting, one ending |
| W-3.3b | the wait-for graph (W1) | 09, section 9.14 | M | each deadlock of 12.7 throws |
| W-3.4 | `ObjectLocker`; every mutation of 05, section 5.6; CO10; copies; closures; class prototype | 05 | XL | models; litmus |
| W-3.5 | storage protocol in C++ and stubs: `acquireStorageForWriting`, growth, shrinking by the owner only, conversions, copy-on-write, the frozen form in one replacement, flattening with prepared storage; rules A1 to A5 over the run time's lane decoders | 06, sections 6.2, 6.3 | XL | litmus M5, M8 |
| W-3.6 | named properties, N1 to N3 and N2' in `PropertyTable` and in transitions, dictionaries, Structures; `Thread.restrict` on arrays and ordinary objects (if D12 says yes) | 06, sections 6.4, 6.5 | XL | |
| W-3.7 | `Map`, `Set`, `WeakMap` | 06, section 6.6 | L | linearizability test |
| W-3.8 | strings, atom table | 06, section 6.7 | L | |
| W-3.9 | typed arrays, ArrayBuffer; measure TA5 on programs that make many small `Buffer`s | 06, section 6.8 | L | a number |
| W-3.10 | RegExp; other cells (D1); the list of about 80 `isStillValid()` sites that C++ consults (WP1) | 06, sections 6.5, 6.9, 6.10 | L | |
| W-3.11 | cache epoch; `SharedChainDependents` (E5); shared name sets (K4); guard bits atomic; method table alias | 07, section 7.5 | M | model |
| W-7.4 | a stale pass of each kind of member guard is harmless to memory; guard sets; the arming logic | 07, section 7.5 | S | a written argument per kind |
| W-3.12 | lazily made things, Z1 and Z2, table locks | 07, sections 7.6, 7.7 | L | first-use races of 12.7 |
| W-3.13 | `Lock`, `Condition`, `ThreadLocal`, `Atomics` on properties, `wait`/`notify`; inline forms | 09, section 9.13; 04, section 4.13 | L | |
| W-3.14 | exceptions, faults, stacks, traps, termination per thread | 09, sections 9.4, 9.5, 9.10 | M | |
| W-3.15 | generators: owner test | 09, section 9.6 | S | |
| W-8.6 | polls in long C++ loops and RegExp; scopes without safepoints that are not short, each with the way it is split | 08, section 8.5 | M | gate 6 |
| W-11.2 | audit of `aot/runtime/*.cpp`, `aot/object/*.cpp` against 11.2 | 11, section 11.6 | XL (reading) | a state per file |
| W-3.16 | serial mode, amplifier, `stopEvery` | 08, section 8.7; 12.5 | M | |

### Stage 4: Jobs, modules, event loop

| # | Package | Where | Size |
| --- | --- | --- | --- |
| W-4.1 | microtask queue per thread, with a safepoint per job; hand-over words; async context; the order of reactions across a settling on another thread | 09, section 9.7 | M |
| W-4.2 | promise busy bit in C++ and eight stubs | 09, section 9.7 | L |
| W-4.3 | reaction homes; inbox, `post`, `closeInbox` (T2); `enqueueAtHome` over every enqueue of the resuming set (J2, J3); owner tests at the three places that restore a frame (G3) | 09, sections 9.6, 9.7 | L |
| W-4.4 | tickets: `asyncJoin`, `asyncHold`, `asyncWait`, `waitAsync`; keep-alive; async thread bodies | 09, sections 9.8, 9.13 | M |
| W-4.5 | modules: leaf lock, claims that take a whole set and can grow, tickets of `import()`, acquire and release of the status, top-level await, CommonJS; modules outside the image, which the engine's loader evaluates (MD10): the loader's lock replaces its cell lock, `ModuleClaims` in the loader | 09, section 9.9 | L |
| W-4.6 | Bun: `VirtualMachine::init_for_thread`, hooks; exit (X1) | 11, section 11.4; 09, section 9.11 | L |
| W-4.7 | `WeakRef`, `FinalizationRegistry`, `DeferredWorkTimer` per thread | 08, section 8.11 | M |

### Stage 5: Natives

| # | Package | Where | Size |
| --- | --- | --- | --- |
| W-5.1 | checks at resolved calls, on `NativeExecutable`, `InternalFunction`, getters; the owner test at every cast to a native struct, with its lint | 11, section 11.3 | L |
| W-5.2 | thread-affine blocks, orphans (H6 to H10); `owner_has_ended` in finalizers; the counted life of what a Bun VM leaves behind | 08, section 8.11; 11, section 11.4 | L |
| W-11.3 | natives that share one function pointer; method table hooks | 11, section 11.3 | S |
| W-11.5 | `Zig::GlobalObject` and `JSVMClientData`, member by member (BV1); a `ScriptExecutionContext` per thread; the `static_assert` on the size | 11, section 11.4 | L |
| W-11.6 | module-level state of `src/js`; the lint | 11, section 11.5 | L |
| W-5.3 | first classification; generated tests; the first hundred `any` | 11, sections 11.4, 11.6; 12.9 | XL (reading) |

### Stages 6, 7, 8

| # | Package | Where | Size |
| --- | --- | --- | --- |
| W-6.1 | per-thread `CodeBlock`s, `m_thread`, prologue; affine code block spaces (I3) | 10, section 10.2 | L |
| W-6.2 | the unlinked half, code caches, declaration lock, eval callee | 10, section 10.3 | L |
| W-7.1 | ARM64: F1, F3 in emitters, stubs, direct generator, C++ (`WTF::Atomic` orders) | 04, section 4.10 | L |
| W-7.2 | views and thread-local access on Darwin and Windows; fault handler's scratch word | 07, sections 7.3.1, 7.3.4; 09, section 9.5 | M |
| W-8.1 | TSan lane and annotations | 12.6 | L |
| W-8.2 | campaigns, fuzzers, gates | 12.5, 12.11 | |
| W-8.3 | user documentation | 03, section 3.10 | M |

## 13.3 Size of the whole

Roughly 60,000 to 90,000 changed or new lines outside tests, of which about a third in `aot/`, a third in `runtime/`, `heap/`, `llint/`, `wtf/`, and a third in Bun. #249 was +87,000 lines in 658 files for the engine alone, with three modes and all JIT tiers.

## 13.4 What can be cut from a first version

| Cut | Effect |
| --- | --- |
| Stage 6 | text does not run on spawned threads (an `EvalError`) |
| Stage 7 | Linux x64 only, as the compiler itself began |
| `Thread.restrict` (05, section 5.11) | the export is absent |
| `AtomicSlot` in line, `LockHold` in line | calls of the run time: slower, same meaning |
| `PinLoopExitReads` | if the owner rules that plain reads carry no promise (13.6, D5) |
| Per-thread variables for user `ThreadLocal` | the map |
| `serial` | everything not audited is `main` or `owner` |
| Smaller tables for spawned threads | more address space per thread |
| Witnesses in `--explain=threads` | counts only |

What cannot be cut: stage 1, stage 2's safety rules (4.5 to 4.7, 4.11), stage 3's protocols. They are one argument.

## 13.4a Before any code

The first full draft of these documents was reviewed against the sources, hazard by hazard, and about a third of the 954 rows were not closed (about 110 findings). The rewritten protocols were then attacked by readers who had not written them: 37 more findings, 8 of them memory unsafe. The present text answers all of them (appendix B has both rounds), and appendix A lists what is open.
The numbers fall, and they have not reached zero: **a third round would find more.** What held in the second round without a finding: the storage word with its sequences (06, sections 6.2, 6.3), heap access and concurrent marking (08, sections 8.3, 8.10), order facts, fences and what B3 is told (04, sections 4.7, 4.9, 4.10).
Two things follow for the plan. A protocol is implemented only after its model (12, section 12.8) passes. And the review is repeated, by the method of appendix A, on the documents of a milestone when that milestone begins, because the compiler will have moved.

## 13.5 Later

| Item | Gain | Needs |
| --- | --- | --- |
| Allocation classes per site; function versions per thread set | more Confined and MainOnly code | 04, section 4.2.5 |
| Strip-mined loops: a poll every n turns, the inner loop without | versioned loops with clamped bounds on arrays that may be shared | a pass; `window` values live inside the inner loop |
| A poll's slow path continues in the generic version | guards and storage may cross polls on the fast path | `AddGenericVersion` with the poll as a failing test; re-entry in front of the pre-header |
| Coroutines that are not bound to a thread | L9 of 02 goes: any thread may resume a generator, and an `await` of a thread that has ended can continue on the main thread | "no value derived from the view is live across a suspension", verified; a claim on the state word; the frame saved before the state is published |
| Header words that read as numbers | `shift()` in constant time on shared arrays | 06, section 6.3 |
| Marking assist by mutators | less throttling by stops | a visitor per thread |
| Mutators conduct collections | no hand-off to the collector thread | a protocol for the conn among N |
| Concurrent collections in `bun:threads` (`Queue`, `ConcurrentMap`) | what most programs want instead of racing on arrays | library work |
| WebAssembly, FFI, inspector on threads | 11, sections 11.8, 11.9 | |
| A cache between builds | the development loop (03, section 3.7) | the compiler's own plan |

## 13.6 Decisions that need an owner

Each has a recommendation, which the documents assume.

| # | Question | Alternatives | Recommendation |
| --- | --- | --- | --- |
| **D1** | How is the feature packaged? | (a) a third flavour; (b) a run-time flag in `bun-aot`; (c) always on in `bun-aot` | **(a)**. 03, section 3.2. It decides nearly everything else |
| **D2** | Where do the classes come from? | (a) `bun:threads`; (b) globals, as in #249; (c) `"bun"` | **(a)**: the import is what selects the flavour |
| **D3** | Which thread runs a promise reaction? | (a) the registering one; (b) the settling one, as in #249 | **(a)**. Needed for MainOnly; an `await` stays on its thread |
| **D3a** | And after the registering thread has ended? | (a) handler functions run on the main thread, an `await` never continues; (b) a thread with a pending `await` does not end; (c) both continue on main (needs coroutines that are not bound to a thread, 13.5) | **(a)** now, (c) later. (b) turns a forgotten promise into a thread that never ends |
| **D4** | Does a thread whose function returns a promise wait for it? | yes / no | **yes**, given D3 |
| **D5** | What is promised about plain reads in loops (never ruled in #249)? | (c0) a read that decides a loop's exit is performed in every turn; (c1) bounded staleness; (c2) nothing, as for SharedArrayBuffer | **(c0)**: 0.8 % there, less here, and `while (!done) {}` is what people write first |
| **D6** | May a generator be resumed by another thread? | (a) no, `ConcurrentAccessError`; (b) yes, with a claim | **(a)** now, (b) later |
| **D7** | Two threads `push` to one array without a lock | (a) may lose an element; (b) `push` is atomic on arrays that may be shared (a compare-and-swap on the length) | **(a)**. It is a race on `length`. (b) costs each push about 20 cycles |
| **D8** | `shift()` on long arrays that may be shared | (a) falls back to `ArrayStorage`; (b) encoded headers now | **(a)**, and a `Queue` class |
| **D9** | What may the main thread do? | block in `join`, `hold`, `wait` / not | **may block**, as Bun allows `Atomics.wait` there |
| **D10** | Default for a native that nobody has audited | (a) refusal (`main`, `owner`); (b) `serial` | **(a)** for Bun's natives, (b) for engine built-ins whose only problem is hidden state |
| **D11** | The process when the main loop is empty and threads run | waits / exits | **waits**, as for Workers |
| **D12** | Is `Thread.restrict` in the first version? | yes / no | **no**. It costs a flow mark and markers on every field of each class that reaches it |
| **D13** | What happens to #249's branch? | (a) frozen as a record, its tests and specifications reused; (b) continued in parallel | **(a)** |
| **D14** | Names | `bun-aot-mt`, `--threads`, `bun:threads`, `ENABLE(JS_THREADS)` | as written |
| **D15** | Does the mt flavour ship with Bun's releases, or on demand? | with / on demand | **on demand**, as `bun-aot` |
| **D16** | A wait that can never end because of the run time's own claims (`join` inside a module body against a thread that needs that module) | (a) an `Error` in the waiter, found by a wait-for graph; (b) hang | **(a)**. A program's own deadlock on `Lock` or `Condition` is not detected |
| **D17** | Native objects of a thread that has ended | (a) every method throws; (b) they pass to the main thread | **(a)**. Their structs point into a Bun `VirtualMachine` that is gone |

## 13.7 What is asked of the compiler's branch

Small changes that are harmless without threads and that keep this work from conflicting with a branch that moves fast. Each can go into oven-sh/WebKit#757 or oven-sh/bun#45631 by itself.

| # | Change | Why |
| --- | --- | --- |
| U1 | `Shape`, `OwnInstance` declared `Forever` in `ir/AOTSchema.def` | they are (`proofsThatOutlastWrites`) |
| U2 | All regions of the `Instance` on 64 KB boundaries; heuristics and free cells (`regionSteps` 9 to 11) in a chunk of their own | 07, section 7.3 |
| U3 | Words of the running thread grouped in one part of the `Instance` struct (`stackMirror`, `exceptions`, `scratch`, hand-over words, `json`, `stringBuilder`, allocators) | the same; also better locality |
| U4 | One order of stores in all array emitters: element, then length | 06, section 6.3; today two orders exist |
| U5 | `addToExtraProperties`: value, then the StructureIDs | 05, section 5.6 e |
| U6 | Dictionary add: grow the storage, then insert the table entry | 06, section 6.4 |
| U7 | `GlobalBinding` fill: location first, guard word last | R4 5.4 |
| U8 | `BuiltinFlag` 32 bits; `impossibleMutationBits` 12; `EffectTraits` 16 bits | room |
| U9 | Readers of `SlimClosure` in C++ derive everything from one load of the word | 05, section 5.8 |
| U10 | `SlotEmitter::holdsDeclaredType` on a loaded value where the value is loaded anyway | one load less today too |
| U11 | The generator state is published after the frame is saved | 09, section 9.6; right for the marker as well |
| U12 | `mutatorShouldBeFenced` is not used to mean "nobody else reads": a named predicate `storageMayBeRearrangedInPlace()` at the six sites | 08, section 8.10 |
| U13 | `VM::topCallFrame` and the other members of 07, section 7.2 behind accessors | W-1.1 becomes a change of the accessors |
| U14 | A callback's thrown value has a place in `CallbackSpec` | 03, section 3.5.2 |

## 13.8 What stays out of scope, and what each would need

| Item | Would need |
| --- | --- |
| Threads under the JIT (`bun run`) | #249 |
| WebAssembly on spawned threads | 11, section 11.8 |
| `bun:ffi` on spawned threads | 11, section 11.9 |
| The inspector | breakpoints do not work in compiled code at all today. A protocol for several threads per realm in the inspector's agents |
| `--aot-huge-heap` with threads | nothing known: wide references change slot widths, not protocols. Untested |
| 32-bit, C loop | not planned |
| Fuzzing of threaded programs | a generator of programs with threads whose allowed outcomes can be computed (data-race-free by construction, compared with a sequential run), plus the amplifier |
| Cross compilation | the compiler's own limitation. The thread-local offset (07, section 7.3.4) becomes a fact of the target template |
