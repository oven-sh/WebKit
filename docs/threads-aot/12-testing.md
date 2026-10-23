# 12. Testing

What shows that the design works, in the order in which the means become available. Each work package of [13](13-plan.md) names the groups below that must pass.

## 12.1 Principles

1. **A change comes with a test that fails without it**, with the counts before and after stated. (The method of #249's rounds.)
2. **Most new code paths can be tested on one thread.** The forms that the compiler emits for things that may be shared are ordinary code that must compute the same results.
3. **A race that a test cannot provoke is provoked by widening its window**, not by running longer.
4. **A test that only fails in a debug build is still a bug**: an assertion that a race can trip means that the code does not hold the invariant it asserts.
5. **Comparisons of speed** are made only against a build of the base at the same commit, with the same flags, on the same machine, in the same session, interleaved.

## 12.2 Litmus tests of the memory model

`bun:test/aot/threads/litmus/`. Generated, not written by hand: `generate.ts` crosses

- the **rules** M2 to M10 of 02 and each row of the table in 02, section 2.10.5;
- the **kinds of location**: declared field in line (each slot representation: `Int32`, `OptionalInt32`, `Double`, `OptionalDouble`, `Boolean`, `OptionalBoolean`, `Cell32`, `OptionalCell32`, `JSValue`, `LiteralString32`, `DeferredClosure`, `Int64`); declared field in an Extension; extra property; property of an ordinary object in line, out of line, in a dictionary; element of an Int32, Double, Contiguous, copy-on-write, ArrayStorage array; typed array element; module variable; captured variable; static field; `Map` and `Set` entry; `length`;
- the **code shapes**: typed and with types erased; hot (B3) and cold (direct generator); fast, compact; `EverythingIsShareable` on and off;
- the **actors**: 2, 3 and 8 threads.

Shape of a test: writers store values from a set that is disjoint per writer and recognisable (for numbers a pattern in both halves, for objects a class per writer). Readers record what they see. The checker accepts exactly what the rule allows.

| Rule | What the generated test does |
| --- | --- |
| M3 no thin air, no wrong kind | readers assert membership in the union of the writers' sets and the initial value; `typeof` and `instanceof` of each value read |
| M4 no tearing | values whose halves must agree |
| M5 coherence | one writer stores an increasing sequence; each reader's sequence never decreases, **while a third thread forces growth, conversion, addition of other properties** |
| M6 publication | a writer builds an object graph (constructor with all field kinds, nested objects, arrays, closures, a Map) and publishes it by one plain store; readers that see the reference check every field. The main test for ARM64 |
| M7 deletion | delete `a`, add `b`, in a loop; readers of `a` never see a value of `b`'s set |
| M8 structure | N threads add disjoint sets of properties, or elements at disjoint indices, or push under a lock while others store to existing indices; afterwards nothing is missing |
| M9 integrity | freeze, then signal with `Atomics`; a write after the signal must fail |
| M10 progress | spin on each kind of location until another thread's plain store; a time limit |
| M1 sequential consistency | message passing, store buffering, independent reads of independent writes, with `Atomics`, `Lock`, `join`, promise reactions as the synchronization |

The store-buffering and IRIW shapes without synchronization are run too, to record (not to assert) what plain accesses do on each architecture.

### Directed tests

`bun:test/aot/threads/directed/`. One test per attack of [appendix B](appendix-b-attacks.md): the interleaving that broke an earlier draft of a rule, forced with the tools of 12.5. Among them:

| Area | Test |
| --- | --- |
| Progress (M10) | spin on an access by name whose answer is cached as absent or inherited, until another thread adds the property; a chain of `queueMicrotask(poll)` while another thread needs a stop; spin on a state test (`while (!("x" in o))`, `while (o.x === undefined)` for an undeclared `x`, `while (!Object.isFrozen(o))`), on a byte of a typed array, on `m.has(k)`, on a leaf call that is not inlined |
| Edges | `o.a = 1; resolve(); o.b = 2` against a reaction on another thread; a module that extends `Object.prototype`, evaluated by one thread and found `Evaluated` by another |
| Slots and variables | `o.extra = 1` (an existing extra property) against a store that moves a field out while the side object's storage is full; `delete o.m` against the first read of a deferred method that may be empty; a by-name read against the first moved field of a layout without the hidden word; inline `Object.freeze(o)` against `o.newField = 1`; `delete C.count`, or a redefinition, against reads and stores of a static field; `Object.values(o)` against `delete o.a`; `Object.assign(sharedEmpty, s)` against a store of an out-of-line field; the first write to `C.prototype` against method calls by name |
| `Atomics` on properties | `add` from compiled code against `add` through `Reflect.apply`, and `add(o, k, 1)` with a computed key against `add(o, "n", 1)` in line, on one field: no increment is lost; `wait` against `notify` around the compare; `add` that overflows an integer slot; `xor` that yields a marker's bit pattern; `store` against `delete` |
| `Lock` | `hold` on the main thread against a spawned thread |
| Arrays | `f[0] = 9` against `f.pop()`, `f.x = 1`, `Object.create(f)` on a frozen `f`: never stored; `Object.defineProperty(MyList.prototype, 0, { set })` while other threads push to instances; `Thread.restrict` on a literal, on a frozen array, on an array of a Confined site, and `a.length` from another thread throws; `a.length = n` in C++ against `a[0] = x`: never lost; `push` against `length = 0`, then a collection, then `a[20] = y; a[10]`; `unshift` on an `ArrayStorage` array with named properties against readers; two stores beyond the length; `splice(0, 2)` on a long shared array with named properties against readers of both; first store by a second thread; growth prepared before a stop that no longer fits |
| Named properties | `setPrototypeOf(a, b)` against `setPrototypeOf(b, a)`: no cycle; flattening of a dictionary with a non-empty quarantine, then a collection, then additions; a field that leaves its slot while another thread stores into an extra property of the same object; `delete o.a` then `defineProperty(o, "b", { get })` along cached transitions against a reader of `a`; `delete` of an inherited accessor against a cached getter call; a new object that takes a transition out of a Structure whose table another thread probes |
| Caches | `Object.prototype.x = 1`, release, acquire, read on an object with a layout; a hiding property put into a class's chain by the thread that then calls the method; `then` added to a prototype, then `await` on the same thread; a thread idle in its loop over a full collection, then a cached read |
| Coroutines and jobs | `shared.then(() => g.next())` registered by a thread that ends, with `g` its own generator; `p.then(r1)`, a settling on another thread, `p.then(r2)`: `r1` runs first; `await` of a thenable whose `resolve` is called on another thread; a fire-and-forget async function of a thread that ends, settled later by main; a generator of an ended thread after 70,000 further threads; `resolve` against `reject` of one pair, with a thenable; a rejection against `then`; `error.stack` with async frames of two threads |
| Modules | two `import()`s that wait for each other (one is rejected with the deadlock error); a correct program with many short `join`s and claims never gets that error; a module outside the image imported by two threads; `require`, `import()`, `import.meta`, the first use of an internal module in a worker while main is in a top-level `join`; a worker that requires a module which imports the entry module (the deadlock error); two threads that import overlapping graphs; top-level await in a module that two threads import |
| Exit | `process.exit` in a worker while main is in `join`, in `hold`, in `Atomics.wait`, in a function prologue under a pending stop; an `exit` handler that joins the caller |
| Stops | a stop requested while a thread runs C++ full of exception checks, inside and outside a scope without safepoints; a thread that ends in the concurrent phase of a collection; 65,000 threads made and ended in a row |
| Sharing analysis | `C[k] = 5` on a thread, `join`, read of `C.count` on main; the same through a namespace object and through `Reflect.set` |
| Collector | two threads store into one old object while a full collection marks it; a thread that starts, and one that ends, in every phase of a collection; a buffer transferred while another thread's blocking native reads into it |
| Strings, RegExp, errors | the last `deref` of an atom against a lookup of the same text; one literal RegExp on two threads; `re.compile()` against `re.exec()`; `e.stack` with `Error.prepareStackTrace` from two threads |
| Typed arrays | `readSync(fd, buf)` against `buf.buffer.transfer()`; `sort()` against writers; `resize(small); resize(big)` and a stop; two views, `.buffer` on both |
| Map, Set, WeakMap | `m.has(k)`, then `m.size`; `weakMap.get(k)` against `set` and `delete` |
| Interpreter | the first call of one `new Function` result on two threads, then aging |
| Packaging | `bun-aot-mt run x.ts`; a hardened template; a template of the other flavour |
| Natives | an instance of an `owner` class as an operand (`fetch(request)`, `new Response(blob)`) from another thread; a `Timeout` of an ended thread used and finalized on main; `process.nextTick` on two threads |

## 12.3 Everything that exists, with every new code path

The switch `EverythingIsShareable` (04, section 4.15) makes the compiler treat each function as AnyThread, each class as Shareable and each variable as Shared. A program that starts no thread must behave as before.

| Corpus | Size | Run as |
| --- | --- | --- |
| `bun:test/aot/programs` and `generate` | 23,000 files + about 25,000 generated | a new `variant()` row in `bun:test/aot/parts.mts`: mt flavour with `EverythingIsShareable`; another with `NothingIsUnpublished` in addition; another under `--aot-validate` |
| `testaot all` | about 6,400 native and IR text tests | in the mt flavour |
| `bun:test/aot/other-suites` (test262, JSC's tests, Node's tests, Bun's `test/js`) | | the mt flavour, default switches |
| `bun:test/aot/collector-stress` | | the mt flavour: the collector thread conducts, polls park |
| `bun:test/aot/frameworks`, `servers` | | the mt flavour |
| `bun:test/aot/fuzz` | | generators and mutators against the mt flavour with the switch on |

This is the widest net for the least work, and it is available at stage 2, before a second thread has run.

The **other two flavours** are protected by construction (no line of the feature is compiled). CI checks it: the object files of `bun` and `bun-aot` built from the branch and from its base are compared symbol by symbol (`Tools/Scripts/aot/compare-flavours`), and differ only where a file outside `#if ENABLE(JS_THREADS)` was touched. Each such hunk is listed in the pull request with its reason.

## 12.4 IR tests and verifier

`aot/tests/`: an IR test is a small program in IR text, a command and `CHECK` lines.

| New tests | |
| --- | --- |
| `passes/pin-loop-exit-reads.ir`, `passes/mark-recursive-entries.ir` | per pass |
| `valueflow/threads-*.ir` | one per rule R1 to R6 and per edge of 04, section 4.2.2; each with the witness |
| `analysis/guards-synchronize.ir` | each Stretch proof ends at a synchronizing operation for a subject that may be shared, and does not for a Confined or unpublished one |
| `verifier/storage-live-across-safepoint.ir` | each way to keep a `window` value over a `collects` operation is rejected |
| `AOTSharedReadCoverageTests.cpp`, `AOTSynchronizationEdgeTests.cpp` | schema tests: each operation that reads a slot, a variable, an Extension or storage is in a table of 04, section 4.6.7; both ends of each edge of 02, section 2.10.2 are `synchronizes`. A new operation fails the test until it is classified |
| `select/try-get-slot.ir`, `codegen/*` | the emitted forms, with instruction counts (`bun:test/aot/perf` snapshots) |
| `validation/*.ir` | "a lie per producer": a false MainOnly, a false Confined, a false unpublished, a false order fact: each must be caught at run time |
| existing tests of each pass in the table of 04, section 4.8 | a copy with a shared location, showing what the pass may no longer do (`delayStoresIn`) |

## 12.5 Widening the windows

| Tool | What it does |
| --- | --- |
| `YieldAtWindows` (compile-time switch) | between the two halves of each protocol the compiler emits a call that, by a seeded random draw, yields, sleeps 50 microseconds, or spins: after `LoadStorage`, between the load and the compare-and-swap of a slot, before a release store, after the tag compare, between key and index slot of a table |
| `BUN_JSC_amplifyRaces=<seed>` (run time, debug and `-amplify` builds) | the same in C++: `RaceAmplifier::point(id)` at each step of each protocol of 05 to 09 (after taking a lock, between prepare and lock, before publication, in `acquireStorageForWriting`, in the inbox, around the Dekker pairs). #249's amplifier found most of its late bugs |
| `BUN_JSC_stopEvery=<n>` | a stop of the world at every n-th poll and allocation slow path, alternating between a collection, an empty safepoint operation and a cache epoch bump |
| `FailPredictions` (exists) | tests fail at random: exercises `empty`, `hole`, "not writable", lost compare-and-swap |
| `BUN_THREADS_SERIAL=1` | the opposite: no concurrency. Separates races from logic errors |
| CPU affinity | all threads on one core (preemption at arbitrary instructions), and one per core |

A campaign is: litmus tests and the ported corpus, each of the four tools at three seeds, two hours per configuration.

## 12.6 Sanitizers

**ThreadSanitizer for the run time.** A lane `-aot-mt-tsan`: C++ and Rust instrumented. Compiled JavaScript is not instrumented, so:

- accesses by compiled code to memory that C++ also touches are invisible. To make the protocols visible the stubs and emitters of a TSan image call `__tsan_acquire`, `__tsan_release`, `__tsan_read8`, `__tsan_write8` at the protocol points (publication, lock, compare-and-swap, plain slot access of a shared location) under a compile option `Instrumentation::ThreadSanitizer`;
- data races **of the program** on plain locations are allowed by 02. Plain accesses to JavaScript locations are therefore annotated as relaxed atomics. What remains reportable is the engine's own state.

Goal: zero reports on the corpus. Each suppression names the rule that makes the race benign.

One run is made for what Confined does **not** cover: two threads that work on Confined objects of one class (`Object.keys`, the first `for-in`, the first spread) share the caches of their `CompiledStructure` (05, section 5.13).

**AddressSanitizer**: the existing ASAN lane in the mt flavour. Freed-late memory (08, section 8.9) is poisoned when it is handed over and unpoisoned never, so a use after the logical free is caught even though the memory is still mapped. Retired storage is poisoned for writes in validating builds.

## 12.7 The corpus of #249

`JSTests/threads/` of the branch `sosuke/threads`: 476 tests (`research/R9-prior-threads-design.md`, section 9 and appendix E). About 37 % are reusable as they are, 38 % after shims.

| Step | |
| --- | --- |
| Port | to `bun:test/aot/threads/corpus/`. Each test becomes a program that is compiled. A prelude maps the globals of the `jsc` shell (`$vm`, `print`, `load`) and imports the five classes |
| Drop | the variants for the mode with a global lock; tests of JIT tiers, watchpoints, exits |
| Change | expectations for L2 (reactions), L3, L4, L9 of 02, section 2.9 |
| 32 tests use `eval` or `new Function` | stay; they need stage 6 |
| The mirror harness (runs ordinary single-threaded tests inside a spawned thread by `eval`) | becomes a build step: `wrap-in-thread.ts` turns a program into one whose body runs in `new Thread(..).join()`. Applied to `bun:test/aot/programs`: **each existing program also runs on a spawned thread** and must print the same |

## 12.8 Models of the protocols

Small enough to check exhaustively. Each is a page of a model (TLA+, or C++ under a model checker such as GenMC or Relacy) kept in `docs/threads-aot/models/` with the command that checks it.

| Protocol | Property |
| --- | --- |
| Storage word: owner, foreign writer, foreign reader, grower, safepoint operation, marker (06, sections 6.2, 6.3) | no store is lost; no access outside the storage that was tested; no cell above `publicLength` is freed while a later growth of the length can expose it |
| Slot: fast reader, fast writer by compare-and-swap, locked mover, locked deleter (05, sections 5.4, 5.6) | CO2, CO3, CO4; the value that leaves the slot is the last one stored |
| Heap access, stop, start and end of a thread (08, sections 8.3, 8.4, 8.14): states `NoAccess`, `HasAccess`, `Parked`; a conductor, two mutators, one starting, one ending | no thread has access while the conductor believes the world stopped; each thread is scanned in each stop, whole or not at all; no lost wake-up; no deadlock on the threads lock; each thread runs `didResume()` before its first access after a stop |
| Safepoint operations with coalescing, a concurrent collection and running markers (08, section 8.6) | each closure runs exactly once, on a JavaScript thread, with all mutators stopped, or reports that what it was prepared from has changed; no deadlock |
| Cell state with two mutators and a marker (08, section 8.10, H2) | no reachable white cell at the end of marking. The model with re-whitening must fail |
| Promise busy bit, flags, reactions, `post` against `closeInbox` (09, section 9.7; T2) | each reaction runs exactly once, on its home thread, or on main, or (a coroutine of an ended thread) never; no flag is lost; no frame is restored on a thread that is not its own |
| Claim word with failure and termination (07, Z2); claims of module sets (09, section 9.9); the wait-for graph (09, W1) | made exactly once; waiters wake; a cycle of waits is reported to exactly one waiter |
| Atom table: `deref` to zero against lookup (06, section 6.7) | no reference to a freed atom; one atom per text |
| `CompactTable` (06, section 6.6) | linearizability of `get`, `has`, `set`, `delete`, `clear` |
| Cache epoch (07, section 7.5) | after an acquire that follows the writer's release no stale chain-dependent entry is used |

The ARM64 variants use the models' weak memory modes.

## 12.9 Natives

`bun:test/aot/threads/natives/`. For each entry of the table whose column is `any` or `serial`: a generated test that calls it from 8 threads at once, on private arguments and on shared arguments, under TSan and the amplifier. For `owner` and `main`: that the call from the wrong thread throws `ConcurrentAccessError`, by a resolved call, by value, through `Reflect.apply`, as a callback of a native, and from interpreted code.

`bun:test/internal/source-lints/`: the frozen list of `unsafe impl Send/Sync` exists. New lints: each module-level mutable binding of `src/js` is classified (11, section 11.5); no `#if ENABLE(JS_THREADS)` hunk in an upstream file is longer than five lines (03, P2).

## 12.10 Whole programs

| Program | What it shows |
| --- | --- |
| Parallel word count, ray tracer, n-body, Mandelbrot, matrix multiply on `Float64Array` and on `number[]` | scaling of compute kernels on Confined data with shared inputs |
| A thread pool with a shared queue (`Lock`, `Condition`), work stealing with `Atomics` | the synchronization primitives under load |
| A shared `Map` cache with many readers and few writers | lock-free reads |
| `Bun.serve` on N threads with `reusePort` over shared in-memory state; the eight servers of `bun:test/aot/servers` in that form | the runtime per thread |
| **TypeScript 6's checker with its files checked in parallel** (a small patch to `tsc`: one `Thread` per project reference or per group of files) | the motivating case of the compiler's own pull request: 4.53 s on one thread against TypeScript 7's 2.91 s on many |
| The parallel-self suite of #249 (raytrace-like, splay-like, map-heavy, string-heavy, richards-like at 1, 2, 4, 8, 16 threads) | comparison with the earlier numbers |

## 12.11 Performance gates

Measured as main-thread cycles and instructions (`perf stat`, user mode), paired and interleaved, at least 30 runs, mean and standard error stated.

| Gate | Comparison | Target |
| --- | --- | --- |
| Gate 0 | `bun`, `bun-aot` from the branch against the base | identical text outside listed hunks |
| Gate 1 | a program without threads compiled by `bun-aot-mt --threads` against the same by `bun-aot` (TypeScript 6 checking effect; the eight servers; `bun:test/aot/perf` rows; a scripted session of a large command-line application) | cycles within 3 %, peak memory within 5 %, start-up within 1 ms |
| Gate 2 | the same with `EverythingIsShareable` | reported, not gated: the price of the worst case of the analysis. Expected 3 to 10 % |
| Gate 3 | `bun:test/aot/perf` instruction snapshots for each sequence of 06, section 6.3 and 04, section 4.6 | the counts stated in those sections |
| Gate 4 | scaling of 12.10's kernels | at least 0.8 n at n = 8 for Confined kernels; reported for the others |
| Gate 5 | `new Thread(() => 0).join()` | under 150 microseconds |
| Gate 6 | a stop of the world with n running threads | under 0.1 ms + 0.05 ms n; time to safepoint under 1 ms at the 99.9th percentile on the corpus |
| Gate 7 | memory per idle spawned thread | under 1 MB resident besides its stack |

## 12.12 Continuous integration

| Lane | Runs |
| --- | --- |
| `-aot-mt` release, Linux x64 and arm64 | 12.2, 12.3 (a share, by the commit's hash, as today), 12.4, 12.7, 12.9, 12.10 |
| `-aot-mt` debug + amplifier | 12.2, 12.7 at one seed |
| `-aot-mt-tsan` | 12.2, 12.7, 12.9 |
| `-aot-mt` ASAN | a share of 12.3, 12.7 |
| macOS arm64, Windows x64 | 12.2, 12.7 |
| all existing lanes | gate 0 |
