# 1. Overview

## 1.1 Goal

A program compiled with `bun compile` can start threads that run JavaScript at the same time, in one realm, on the same
objects, with one garbage-collected heap:

```ts
import { Thread, Lock } from "bun:threads";

const totals = new Map<string, number>();
const lock = new Lock();

const workers = files.map(file => new Thread(() => {
  const counts = countWords(readFileSync(file, "utf8"));       // plain JavaScript, on its own core
  lock.hold(() => { for (const [w, n] of counts) totals.set(w, (totals.get(w) ?? 0) + n); });
}));
for (const w of workers) w.join();
```

No copying, no message passing, no separate module graph per thread. Any object can be reached by any thread.

The three properties the project has asked for since the beginning stay the measure:

- **Behaviour.** A program that starts no thread behaves exactly as it does today. A program without data races behaves as if
  its threads' steps were interleaved in some order (sequential consistency for data-race-free programs).
- **Safety.** No program, racy or not, reads or writes memory outside the object it names, uses freed memory, or takes a value of
  one kind for another. A refusal is a JavaScript exception or a build error, never an abort.
- **Speed.** A program that does not import `bun:threads` pays nothing: not one instruction, not one byte. A program that does
  pays little on one thread (target: within 3 % in cycles of the same program compiled without threads, section 1.6) and scales.

## 1.2 Non-goals

- Threads under `bun run`, `bun test` or any process with a JIT. `import "bun:threads"` is an error there
  ([03](03-packaging-and-build.md), section 3.7 for the development loop).
- A mode with a global lock. #249 had one as a semantic oracle; its semantics are now written down ([02](02-language.md)).
- Running one compiled executable both with and without thread support. The decision is made at build time.
- Race detection, deterministic replay, transactional memory.
- Making npm packages thread safe. A library that assumes run-to-completion is safe to *call* from several threads in the sense of
  1.1 (no corruption of the engine), not in the sense of its own invariants. That is the author's business, as in every language
  with threads.
- First version: WebAssembly on spawned threads, `bun:ffi` on spawned threads, the inspector, 32-bit or C-loop builds.
  [13](13-plan.md) lists what each would need.

## 1.3 Why ahead-of-time compilation changes the problem

What #249 had to fight, and what is left of it here. Details and measurements: `research/R9-prior-threads-design.md`, sections 5 and 6.

| In the engine with a JIT | In a compiled executable |
| --- | --- |
| Inline caches patch code or publish several words that code reads | No code is patched. An access by name probes tables, which are data (`runtime/AOTPropertyCaches*`). Data can be made private to a thread |
| Watchpoints: other threads are inside code that dropped a check, so firing one needs a stop, a jettison, an OSR exit | No watchpoint reaches code. Firing clears words. A test that fails continues in generic code of the same frame through an ordinary SSA edge (`passes/AOTAddGenericVersion.cpp`) |
| Tier-up, profiles, execution counters written by all threads; every bimodal benchmark of that project | None |
| A Structure transition races with a cached offset: the central problem | "A compiled object keeps its shape, and with it its instance, for life" (`analysis/AOTGuardFacts.cpp:48`). The kind, width and offset of a slot are functions of that constant |
| Built-ins can be replaced at any time, so every fast path rests on a watchpoint | Existing members of built-ins are read-only |
| What an optimization assumes about memory is spread over `clobberize`, `doesGC`, OSR | One schema: abstract heaps, `reads`/`writes` per operation, proofs with a lifetime (`ir/AOTSchema.def`), checked by a verifier and by validating builds |
| The engine cannot know whether a program will start a thread | The build knows |
| The feature is in every process: 17 rounds to bring the unused feature to +0.35 % cycles, +6.7 % text, +0.87 MB resident | The feature is in one flavour of the binary, which only programs that ask for it are copied from |

What is **not** easier, and takes most of these documents:

1. Arrays and ordinary objects still have butterflies ([06](06-ordinary-objects.md)).
2. The compiler's proofs that "hold for a stretch of code in which nothing can undo it" assume one thread ([04](04-compiler.md)).
3. The run time uses the `Instance`, which is per realm, as if it were thread-local storage ([07](07-runtime-state.md)).
4. The collector has one mutator ([08](08-heap.md)).
5. The C++ run time and Bun's natives are what they were ([11](11-native.md)).
6. There is no OSR exit to fall back on, so an unexpected state needs a real code path from the first day. (The generic version is that path.)

## 1.4 The architecture

### A. A build-time feature, in a flavour of its own

```
bun            ENABLE_AOT=0                      no compiler, no run time of compiled programs
bun-aot        ENABLE_AOT=1  ENABLE_JS_THREADS=0 compiler + run time; template of executables
bun-aot-mt     ENABLE_AOT=1  ENABLE_JS_THREADS=1 compiler + run time with threads; template of executables that import bun:threads
```

The front end sees the import of `bun:threads` while it loads the program, before anything else is decided, and hands the build
over to `bun-aot-mt` exactly as `bun` hands a build over to `bun-aot` today. All C++ changes are under `#if ENABLE(JS_THREADS)`.
**There is no run-time test "are threads on".** One binary has one object model, and for the same reason an engine with
`ENABLE(JS_THREADS)` never generates code at run time: its JIT tiers are off for good (03, P3). This is the main lesson taken from #249, which
ended with 26,150 static tests of its mode flag in the binary and named "memory unsafety as the failure mode of a missed site" as
the reason not to have two object models alive at once.

### B. Three classes of state

Every piece of state that is written at run time falls in one of three classes, and each class has one treatment.

| Class | Examples | Treatment |
| --- | --- | --- |
| **Of the running thread** | stack limit, pending exception, top call frame, allocator free lists, barrier buffers, scratch words, coroutine hand-over words, microtask queue | Moves to the thread: `ThreadContext` (C++) and the private regions of the thread's `InstanceView` (compiled code) |
| **Cache** | property caches, site caches, allocation site records, key order caches, prototype transitions, numeric string caches | One per thread. Filled from the truth. Nobody else reads or writes it, so the stubs that fill caches stay as they are. Invalidation across threads by one epoch word (07, section 7.5) |
| **Truth** | objects, module variables, Structures, class tables, the atom table, module states | Shared. Immutable after publication, or published by one atomic word, or changed under a lock with lock-free readers that tolerate every intermediate state, or (rare and dangerous) changed in a safepoint operation |

### C. The per-thread view of the Instance

Compiled code addresses everything as `[instanceGPR + constant]`. The `Instance` is one mapping whose regions lie on 16 KB and 64 KB
boundaries and whose offsets are constants of the program (`runtime/AOTInstanceLayout.h`). In an mt build its memory is a memory
object, and each thread maps a **view** of it: regions that hold truth (module variables, guard bits, cells of constants, the method
table) are the same physical pages in every view; regions that hold thread state and caches are the thread's own zero pages.
`instanceGPR` holds the address of the running thread's view.

So no instruction of compiled code or of a stub changes its addressing of the Instance, no register is given up, and no access costs
an extra load. Stubs that address members of `VM` which move to the thread go through a word of the view (07, V8). Only three places
make or recover the register: the adapters at the border from JSC's calling convention, the unwinder, and the fault handler.

C++ finds the Instance by itself in several hundred places (through the realm, through `State`). So the mapping that the realm names, the
**canonical view**, is no thread's view: its private regions are read-only, and C++ has two types, `SharedInstance&` for what all threads
share and `Instance&` for the running thread's view (07, V3 to V7). A store to a thread's word through the wrong one does not compile, or faults in the first test.

### D. Safety by construction, analysis for speed

The whole-program flow analysis has one abstract object per class (`research/R3-value-flow.md`, section 1). If one `Vec` is handed to a thread
every `Vec` counts as possibly shared. A design in which "possibly shared" is expensive would therefore be expensive everywhere. So:

- **The fast paths of possibly shared things are made race-tolerant, not guarded.** For an object with a layout this costs almost nothing,
  because the shape is constant, each slot is one aligned word with its empty state in band, and what a slot may hold is a fact about all
  stores of the program, not about their order.
- The analysis yields three facts, **Confined** (allocation class), **MainOnly** (function, variable) and **unpublished** (object in a function). Where
  one holds, today's code is generated unchanged. Where none holds, the rules of [04](04-compiler.md) apply.
- A wrong answer of the analysis is caught in validating builds, which check each of the three facts at run time.

### E. Windows and safepoints

A thread parks only at an operation whose declared effects say `collects`. The compiler already rests on that rule for the collector
(barrier removal, narrow references in registers, cell initialization). Loop polls exist, cost one load, and are already declared
`collects` (`ir/schema/operations.def:150`). The design adds one rule and gets three things from it:

> **S1.** A pointer to replaceable storage of a Shareable object (butterfly, Extension, table, buffer) does not live across a safepoint.

1. Storage that has been replaced can be freed at the next collection, without reference counts or hazard pointers.
2. Memory that must not be unmapped under a reader (detached buffers, replaced tables) is freed once every thread has passed a safepoint and no pin is held on it (08, H11).
3. A closure that runs while all threads are parked sees no half-done access. That is the **safepoint operation**, used for the transitions that
   are rare and cannot be made tolerant: growing storage that several threads have written, converting a class prototype's stand-in,
   `haveABadTime`, heap walks.

S1 is checkable: storage pointers become IR values of an opaque type whose liveness the verifier tests, and C++ holds them in a counted wrapper that debug builds check at each safepoint (08, S1').

The reverse rule matters as much. Many operations are declared **not** to collect, and the run time keeps that promise by scopes in which it may not park. Inside such a scope a thread never
waits for another thread (S4). What could make it wait is either made before the second thread exists (`Instance::prepareForThreads()`, run by the first `new Thread`) or declared in the schema (S5).

### F. Butterflies: an owner word instead of tags and segments

#249 tagged the butterfly pointer with a thread ID (every read masks, every C++ accessor changes) and replaced shared storage by segmented
storage (243 instructions per element read against 19). Here the butterfly's header grows by one word, in mt builds only:

```
 b-16  storage word: state (Confined | Owned(thread) | Shared | Retired | ReadOnly), element kind, start offset
 b-8   publicLength, vectorLength
 b     elements
```

- A **read** loads the pointer once and takes bounds and element kind from the storage it loaded. Same number of instructions as today's unhoisted read. `vectorLength` is a constant of a storage.
- A **write** also compares the storage word with the thread's own (two instructions). A thread that is not the owner first turns `Owned` into `Shared` by a
  compare-and-swap.
- The **owner grows** by copy, `Owned -> Retired` (compare-and-swap), publish, owner barrier. The copy is exact and no store of another thread can be lost, because no other thread had the right to store while the word said `Owned`.
- **Shared storage grows** in a safepoint operation. That happens a logarithmic number of times per array that several threads really write.
- A change of element kind always makes new storage, so kind and contents arrive by one load. Double arrays stay.
- Headers never move inside an allocation, and the collector visits every slot below `vectorLength` of storage that may be shared, so a length that goes backwards in a race exposes no dead cell.

### G. One heap, N mutators

One `JSC::Heap`, one cage, one base register value. Each thread has its own allocators over the shared block directories (which already keep a list of
allocators and claim blocks atomically), its own barrier buffers and its own access word. The collector thread conducts; the others park at safepoints. Marking stays
concurrent. Upstream's `Collector` for several heaps is a different model (N heaps with one mutator each) and is not used, but its split between cycle state and
per-mutator state is followed so that later merges stay small.

### H. A thread is a Worker's run-time half on a shared realm

Bun's natives find their `VirtualMachine` through thread-local storage (about 650 call sites). Each spawned thread gets its own Bun `VirtualMachine`, event
loop and timers, as a Worker does, but they point at the one `JSGlobalObject`. Functions without a receiver (`setTimeout`, `fetch`, `console.log`) run on the
caller's loop. Native objects belong to the thread that made them unless their class is declared thread safe; every way from a value to the native struct tests the owner, so any
use from another thread, as receiver or as operand, throws. After their thread has ended nobody can use them. The table of built-ins gets a column `threads`, and Bun's build fails for a native that nobody classified.

A promise reaction runs on the thread that registered it, so an `await` continues on its own thread. Coroutines belong to their thread: after it has ended its handler functions
run on the main thread, and its `await`s never continue.

## 1.5 What each kind of program pays

| Program | Binary it is copied from | Cost of this feature |
| --- | --- | --- |
| Anything under `bun`, `bun run`, `bun test` | `bun` | none: no line of the feature is compiled in |
| `bun compile`, no import of `bun:threads` | `bun-aot` | none |
| `bun compile`, imports `bun:threads`, code that is MainOnly on Confined data | `bun-aot-mt` | the run time's: 8 bytes per butterfly with elements and 16 per butterfly that has only out-of-line properties, atomic reference counts where they were plain, locks on slow paths, polls in all loops |
| the same, code that any thread may run on Shareable data | `bun-aot-mt` | section 1.6 |

## 1.6 Expected cost on one thread

An estimate, to be replaced by measurement at stage 3 ([13](13-plan.md)). The basis is #249's decomposition in main-thread cycles
(`research/R9-prior-threads-design.md`, section 6.2): with all threads machinery on, 1.291 of the engine without it.

| Part of #249's 29 points | There | Here |
| --- | ---: | --- |
| JIT-only effects (exits, tier-up, handler caches, call records) | about 5 | gone |
| Polls | 0.9 | already paid by every compiled program; +0 to 0.3 for polls that are no longer elided |
| Visibility rule | 0.8 | the same rule, on Shareable locations only: at most 0.8 |
| Tagged butterfly words | 1.4 | no tags. Owner compare on stores: estimated 0.3 to 0.6 |
| Double arrays disabled, lane verification, second bound | 2.9 | Double arrays stay; kind and bound come from one load: estimated under 0.5 |
| Locked instructions (one per property added, reference counts of strings) | 4.7 | objects with a layout add no property by transition; unpublished objects need no atomics; strings as there: estimated 1 to 2 |
| C++ locks and protocols never prototyped (structure tables, Map/Set, RegExp, generators, promises) | about 12 | per-thread caches in front of the tables; fewer trips to C++ in typed code, more in untyped code: estimated 1 to 6, the widest uncertainty |

Not in #249's list: small typed arrays at sites that are not Confined keep their bytes outside the cell (06, TA5), which costs an allocation per small `Buffer`; a store into an old object that a full collection has not reached yet takes
the barrier's slow path each time (08, H2); the first `new Thread` makes every constant cell of the image (07, section 7.6). All three are to be measured at stage 3.

The sum is 3 to 10 % for code that is neither MainOnly nor on Confined data, and near zero for code that is. The target of 3 % for whole programs assumes that most of a
real program is one or the other. If stage 3 measures otherwise the first lever is the precision of the analysis (allocation classes per site instead of per layout).

## 1.7 Comparison with #249 in one table

| | #249 | This design |
| --- | --- | --- |
| Where threads exist | every process, behind `useJSThreads` | executables copied from `bun-aot-mt` |
| Modes in one binary | flag off, GIL on, GIL off | one |
| Per-thread state | `VMLite` through TLS, selected at 19,000 sites | `ThreadContext` through TLS in C++; pinned register in compiled code |
| Caches | shared, one-word entries, frozen nodes, epoch reclamation | private to a thread |
| Butterfly | tag in the pointer, segmented when shared | owner word in the storage, safepoint operation when shared |
| Element kinds | Int32 to Double never happens | all kinds; conversions copy |
| Transitions of objects | claim by compare-and-swap on each added property | none for declared fields; object lock on slow paths |
| Stops outside the collector | watchpoints, kind changes, conversions | rare list of 08, section 8.6 |
| Promise reactions | on the settling thread (async functions migrate) | on the registering thread; after it has ended, handler functions on the main thread, an `await` never |
| Generators | any thread may resume | only the creating thread |
| Module evaluation | one lock | a thread claims the whole set of modules it needs, or waits owning nothing |
| Bun natives on threads | unusable | per-thread Bun VM, owner affinity, classified table |
| arm64 | out of scope | in scope: fences are part of the rules |
| What an unused feature costs | +0.35 % cycles, +6.7 % text, +0.87 MB | nothing |

## 1.8 Risks

| # | Risk | What limits it |
| --- | --- | --- |
| Risk 1 | The compiler is a draft and moves fast ("Fuzzers still find miscompiles"). A design pinned to line numbers rots | Rules are stated in terms of the schema's vocabulary (heaps, proofs, effects). The appendix is regenerated by the survey's method. Stage 0 lands hooks upstream early |
| Risk 2 | A third flavour costs CI lanes and download size | It is built only for platforms where compiled programs run. It can start as a local build option (13, stage 0). The alternative, a run-time flag in `bun-aot`, is described and rejected in 03, section 3.2 |
| Risk 3 | The C++ run time is large and was never written for this (1,180 host functions) | The `threads` column defaults to refusal. The set of functions that may run concurrently grows only with evidence (11) |
| Risk 4 | The analysis is too coarse, so that everything is Shareable | By construction Shareable is cheap for objects with a layout. Arrays and Maps are per allocation site already |
| Risk 5 | Mirrored mappings misbehave on some platform | A fallback with one more load per access to a module variable is specified (07, section 7.3.5) |
| Risk 6 | Time to safepoint: long C++ loops, RegExp matches | A list with bounds, a watchdog in debug builds, heap access released around blocking calls (08, section 8.5) |
| Risk 7 | Semantics differ from #249 in several places (reactions, `bun:threads` module, async thread bodies, generators) | 02, section 2.9. Listed as decisions for the owner (13, section 13.6) |
| Risk 8 | The design is unbuilt. Its first full draft failed a review against the sources in about a third of the surveyed hazards | The review is part of the method: each hazard of the survey has a row with its rule ([appendix A](appendix-a-inventory.md)), the protocols have models (12, section 12.8), and the review is repeated when the compiler has moved |
