# JavaScript threads for ahead-of-time compiled programs

Design documents for shared-memory threads (`Thread`, `Lock`, `Condition`, `ThreadLocal`, `Atomics` on properties) that
exist **only in executables made by `bun compile`** (`bun build --compile --aot`).

Status: design only. Nothing here is implemented. The documents are written so that the work can be done from them.

Written against the engine half of the compiler at `5c2923e566cb` (oven-sh/WebKit#757) and the Bun half at `8bb8d41bffe`
(oven-sh/bun#45631). Every `path:line` in these documents and in `research/` is a line of one of those two commits. Paths
without a prefix are relative to `Source/JavaScriptCore/`, or, where they begin with a directory of the compiler (`analysis/`,
`builtins/`, `codegen/`, `image/`, `ir/`, `object/`, `passes/`, `pipeline/`, `stubs/`, `valueflow/`, and `runtime/AOT*`), to
`Source/JavaScriptCore/aot/`. Paths that begin with `bun:` are relative to the root of oven-sh/bun; paths that begin with
`wtf:` are relative to `Source/WTF/wtf/`.

## The idea in five sentences

1. An earlier design (oven-sh/WebKit#249) put threads into the engine that has a JIT, behind a run-time flag. Most of its
   difficulty came from the JIT (patched caches, watchpoints, invalidation, profiles), and most of the cost of landing it
   came from the rule that the engine with the flag off must equal the engine without the feature.
2. An ahead-of-time compiled program has no JIT, patches nothing, has objects whose shape never changes, has read-only
   built-ins, and is compiled by a compiler that has seen the whole program and that declares every effect of every
   operation in one schema.
3. So threads become a property of a **build**: a program that imports `bun:threads` is compiled by, and copied from, a
   third flavour of Bun (`bun-aot-mt`) whose engine is compiled with `ENABLE(JS_THREADS)`. `bun` and `bun-aot` contain no
   line of it.
4. Inside such a build, memory safety under races holds **by construction** of the object model and the run time. The
   whole-program analysis is used only to keep today's code where a thing provably never meets a second thread.
5. Everything that is a cache becomes private to a thread, everything that is the truth is either immutable, published by
   one atomic word, or changed under a lock, and the few rare and dangerous transitions run while every thread is parked.

## How to read

| Document | What it settles |
| --- | --- |
| [01-overview.md](01-overview.md) | Goals, non-goals, the architecture on three pages, what each kind of program pays, comparison with #249, risks |
| [02-language.md](02-language.md) | **Normative.** The JavaScript API, what happens on a thread, the memory model, what a race may and may not produce |
| [03-packaging-and-build.md](03-packaging-and-build.md) | The third flavour, how a build finds out that it needs it, front end, table of built-ins, diagnostics, reports, the development loop |
| [04-compiler.md](04-compiler.md) | Sharing analysis in the flow stage; new effect, proofs, operations; a rule for every pass, B3 switch and emitter; fences; validating builds |
| [05-compiled-objects.md](05-compiled-objects.md) | Objects with a layout, Extensions, extra properties, closures, environments, classes: invariants and a protocol for every mutation |
| [06-ordinary-objects.md](06-ordinary-objects.md) | Butterflies and the storage word, arrays, Structures, dictionaries, Map and Set, strings, typed arrays, RegExp |
| [07-runtime-state.md](07-runtime-state.md) | `ThreadContext`, the per-thread view of the `Instance`, caches and their invalidation, lazily made things, tables, modules |
| [08-heap.md](08-heap.md) | One heap with N mutators: access, safepoints, safepoint operations, allocation, barriers, marking, sweeping, finalizers |
| [09-control.md](09-control.md) | Life of a thread, entry and exit, stacks, exceptions, faults, generators, promises, jobs, event loop, termination, exit |
| [10-interpreter.md](10-interpreter.md) | `eval`, `new Function`, `node:vm` on threads |
| [11-native.md](11-native.md) | The `threads` column of the table of built-ins, the audit of the C++ run time, Bun's runtime per thread, N-API, FFI, WebAssembly |
| [12-testing.md](12-testing.md) | What proves that it works: litmus tests, the old corpus, validating builds, sanitizers, schedule perturbation, performance gates |
| [13-plan.md](13-plan.md) | Stages, work packages with acceptance lines, order, what can be cut, **decisions that need an owner** |
| [appendix-a-inventory.md](appendix-a-inventory.md) | Every hazard found in the survey (954 rows), with the rule that covers it, and the index of rules |
| [appendix-b-attacks.md](appendix-b-attacks.md) | The interleavings by which the first draft was broken, and those that the rules withstand. The reason behind many rules |
| `research/` | The survey of both repositories that the design rests on (nine reports, about 12,000 lines, not normative) |

A reader who wants to judge the idea reads 01 and 13. A reader who implements a part reads 02, then the document of the part,
then the rows of the appendix that name its files.

## Vocabulary

| Word | Meaning |
| --- | --- |
| **mt build**, **mt image** | An engine compiled with `ENABLE(JS_THREADS)`; an image compiled by it with `ImageFlag::RunsThreads` |
| **thread** | A JavaScript thread: the main thread, or one made by `new Thread`. Collector, compiler and I/O threads are never meant |
| **main thread** | The thread that entered the program. **Spawned thread**: any other |
| **mutator** | A thread while it may touch the heap |
| **`ThreadContext`** | The C++ object, one per thread, that holds what `VM` holds today for "the" running thread. Found through thread-local storage |
| **`InstanceView`**, **view** | One thread's mapping of the `Instance`. Shared regions are the same pages in every view, private regions are the thread's own. The **canonical view** (`realm->m_aotInstance`) is no thread's view: it names the shared regions and holds the constants |
| **`ThreadState`** | The per-thread half of the run time of compiled programs: every object that keeps an `Instance&` |
| **claim** | A word or record by which one thread owns the making of a thing (07, Z2) or the evaluation of a set of modules (09, MD3'') |
| **safepoint** | A place where a thread may park: an IR operation whose effects say `collects`, a C++ call of `stopIfNecessary()`, or a wait with heap access released |
| **window** | A stretch of one thread's execution that contains no safepoint |
| **safepoint operation** | A closure that runs while every other thread is parked at a safepoint or has released heap access |
| **conductor** | The thread that runs a collection's stop-the-world phases or a safepoint operation |
| **storage** | Memory behind an object that can be replaced: a butterfly, an Extension, a table of a Map, the bytes of an ArrayBuffer |
| **storage word** | The 64-bit word in front of a butterfly's indexing header that holds its owner, state and element kind (mt builds only) |
| **Confined** | Of an allocation class: no object of it is reachable by a thread other than the one that allocated it, while that thread lives. Proven by flow |
| **Shareable** | Not proven Confined |
| **MainOnly** | Of a function: it never runs on a spawned thread. Of a variable: only MainOnly functions touch it. Proven by flow |
| **unpublished** | Of an object in a function: allocated here, and nothing has retained it yet |
| **truth** / **cache** | State that decides what a program observes / state that can be thrown away at any time and filled again from the truth |
| **layout, shape, slot, marker, Extension, extra properties, stub, operation, Instance, State** | As in the compiler's own sources; defined at the top of `research/R1-object-model.md` |

## Rules of these documents

- English, no machine-specific paths, nothing that is not public.
- A rule that code must follow has a number (`S1`, `O3`, `H7` ..) and is stated once, in the document that owns it. The index of rules is at the end of appendix A.
  Three numbered lists are not rules and share letters with rules: the differences from #249 (`L1` .. in 02, section 2.9; the lock rules `L1` .. are in 05), the survey's reports (`R1` to `R9`; the sharing rules `R1` .. are in 04), and the survey's own hazard numbers, which are always cited with their report (`R4 H-7`).
- Where the design chooses between alternatives the losing ones are named with the reason, so that nobody proposes them again
  without new evidence.
- Where a fact was not verified in the sources the text says "not verified".
