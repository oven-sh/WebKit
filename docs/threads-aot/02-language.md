# 2. Language: the API and the memory model

This document is normative. It says what a JavaScript program can rely on. The API is the one of oven-sh/WebKit#249 as it stood with its global lock removed;
`research/R9-prior-threads-design.md`, sections 1 to 4, records every corner with the test that pins it. Differences from it are marked **[changed]** and collected in 2.9.

## 2.1 Where the API comes from

```ts
import { Thread, Lock, Condition, ThreadLocal, ConcurrentAccessError } from "bun:threads";
```

**[changed]** The five classes are exports of the built-in module `bun:threads`, not globals. Reasons: the build has to know early that it needs the mt flavour
(03, section 3.3); the compiler goes "by what an identifier is bound to, never by how it's spelled"; and text passed to `eval` cannot reach a module that the program does not
import, so a program without the import stays free of threads even if it evaluates text.

`Atomics.*` on `(object, key)` (2.5) is available in mt executables only. Elsewhere `Atomics.load({}, "x")` throws the `TypeError` it throws today.

Under `bun run`, `bun test`, `bun build` without `--aot`, and in a `Worker` of an executable that was not built by the mt flavour, the import fails with
`No such built-in module: bun:threads`.

All five require `new`. Instances have ordinary prototypes and a `Symbol.toStringTag`. Subclassing works. There are no options objects.

## 2.2 `Thread`

| Member | Semantics |
| --- | --- |
| `new Thread(fn, ...args)` | `fn` must be callable, else `TypeError("Thread constructor requires a callable argument")`. Starts an OS thread at once; it calls `fn(...args)` with `this` undefined. `fn` and `args` are the same objects, not copies. `RangeError("too many live Threads (or thread-ID space exhausted)")` over the limit (65,534 live threads; threads that have completed do not count, however many there were), `RangeError("could not create a native thread for Thread")` if the OS refuses |
| `thread.join()` | Blocks until the thread has completed (2.6). Returns what `fn` returned, or throws the same value that `fn` threw (identity). Any thread may join, any number of times. Joining oneself: `Error("Thread cannot join itself")`. If the wait would close a cycle of threads that wait for each other, through `join` or through the evaluation of modules (2.6): `Error("deadlock: ..")`. On a thread that may not block, if the target still runs: `TypeError("Thread.prototype.join cannot block the current thread")` |
| `thread.asyncJoin()` | A promise of the same. Never blocks, never settles inside the call. Each call returns a new promise |
| `thread.id` | An integer. 0 for the main thread. Unique among live threads |
| `Thread.current` | The caller's `Thread`. On a spawned thread it is the object `new Thread` returned. On the main thread it is made at first access. `asyncJoin()` of the main thread's object never settles |
| `Thread.restrict(o)` | 2.7 |

No detach, cancel, terminate, name or priority.

## 2.3 `Lock` and `Condition`

`Lock` is a mutex that is not recursive.

| Member | Semantics |
| --- | --- |
| `lock.hold(fn)` | `fn` callable, else `TypeError`. If the caller already holds the lock: `Error("Lock is not recursive")`. Tries first, so an uncontended hold never blocks and is allowed on every thread. Contended on a thread that may not block: `TypeError("Lock.prototype.hold cannot block the current thread")`. Calls `fn()`, releases as `finally` would, returns or rethrows |
| `lock.asyncHold(fn)` | A promise. Once granted, `fn()` runs in a job of the calling thread with the lock held; the lock is released when `fn` returns, not when a promise that it returns settles |
| `lock.asyncHold()` | A promise of a `release` function, to be called exactly once (`Error("Lock release function called more than once")`). Any thread may call it |
| `lock.locked` | Racy. For tests |

Order between synchronous and asynchronous acquirers is unspecified, barging is allowed, asynchronous acquirers are served first in, first out.

| Member | Semantics |
| --- | --- |
| `cond.wait(lock)` | `lock` must be a `Lock` that the caller holds synchronously, else `TypeError`. Atomically enqueues the caller, releases the lock and blocks; reacquires before it returns. Spurious wake-ups are allowed. No timeout |
| `cond.asyncWait(lock)` | Consumes the caller's hold. A promise that, after a notify and once the lock is granted again, resolves with a new `release` function |
| `cond.notify()`, `cond.notifyAll()` | Wake one / all waiters of both kinds, first in, first out. Return the number woken. Holding the lock is optional |

## 2.4 `ThreadLocal`

`new ThreadLocal()`. One accessor `value` on the prototype: a slot per thread, `undefined` at first on every thread. Values are kept alive until overwritten or until
the thread ends.

## 2.5 `Atomics` on properties

If the first argument of an `Atomics` function is a typed array the function does what it does today. If it is any other object the second argument is converted with
`ToPropertyKey` and the operation applies to the **own data property** of that name. The prototype chain is never consulted. Every operation is one atomic step and all of them are
sequentially consistent.

| Operation | Semantics |
| --- | --- |
| `load(o, k)` | Absent, accessor or inherited: `TypeError("Atomics.load: object has no own property")` |
| `store(o, k, v)` | Sets. Creates the property if it is absent and `o` is extensible. `TypeError` for an accessor, a read-only property, or an absent one on a non-extensible object. Returns `v` |
| `exchange(o, k, v)` | As `store`, but `k` must exist. Returns the previous value |
| `compareExchange(o, k, expected, replacement)` | Compares with SameValueZero. Returns the value read |
| `add sub and or xor` | The stored value must be a Number, else `TypeError("Atomics RMW: stored value is not a number")`. `and or xor` work on `ToInt32` of both; `add sub` are double arithmetic. Return the old value |
| `wait(o, k, expected, timeout?)` | `"not-equal"`, `"ok"`, `"timed-out"`. Blocks, subject to 2.8 |
| `waitAsync(o, k, expected, timeout?)` | `{ async: false, value: "not-equal" }` or `{ async: true, value: promise }` |
| `notify(o, k, count?)` | Wakes at most `count` waiters of exactly `(o, k)`. Returns the number |

Refused with `TypeError`: a `Proxy`, the global proxy, and own properties that are not plain storage (array `length`, `lastIndex` of a RegExp, characters of a String object, indices in a
sparse map, global `var` bindings, module namespace exports). Every writing operation throws on a read-only property before it compares.

An operation that writes observes `freeze`, `seal` and `preventExtensions` atomically: once `Object.freeze(o)` has returned on any thread no `Atomics` write to `o` takes effect.
(This was open defect O17-3 of #249. Here writing `Atomics` operations take the object lock, which integrity changes take too: 05, section 5.6.)

## 2.6 What happens on a thread

| Situation | Behaviour |
| --- | --- |
| `fn` throws | The thread has failed. Every `join()` throws the value, every `asyncJoin()` rejects with it. Nothing is printed. The process goes on |
| **When a thread is complete** | When `fn` has returned or thrown, **and** its job and task queues are empty, **and** nothing keeps its event loop alive, **and**, if `fn` returned a promise, that promise has settled **[changed]** |
| `fn` returns a promise | **[changed]** The thread stays alive until the promise settles. `join()` still returns the promise object itself (it is `fn`'s value); by then it is settled. A rejection does not make the thread failed |
| Event loop | Every thread has its own: jobs (microtasks), `process.nextTick`, timers, I/O. `setTimeout` on a thread fires on that thread. What keeps Node's loop alive keeps a thread's loop alive |
| **Which thread runs a promise reaction** | **[changed]** The thread that registered it (`then`, `catch`, `finally`, `await`). If another thread settles the promise, the reaction is sent to its thread as a task. So an async function runs on one thread from beginning to end. The same holds where no promise is in between: an `await` of a thenable continues on the awaiting thread, whichever thread calls the function that `then` was given |
| **Reactions of a thread that has completed** | **[changed]** A handler function (`then`, `catch`, `finally`) runs on the main thread, after everything that its thread did. There it sees the main thread's `ThreadLocal` values. An `await` of that thread never continues, as at the end of a process: its async function, async generator or module stays suspended for ever |
| Order of reactions | Reactions of one promise that were registered by one thread run in the order of registration, also if another thread settles the promise between two registrations. Nothing is promised between threads |
| A pending `await` | Does not keep a thread alive, as it does not keep a process alive. Two exceptions: the promise that `fn` returned, and a module with a top-level `await` that the thread is evaluating |
| Generators, async generators | **[changed]** The object can be held and passed on by any thread. `next`, `return` and `throw` work only on the thread that created it; elsewhere they throw `ConcurrentAccessError("a generator can be resumed only by the thread that created it")`. So `for..of` over a generator made by another thread throws. Once the creating thread has completed they throw on every thread |
| Jobs and blocking | No job runs inside `join`, `hold`, `wait` or `Atomics.wait` |
| Unhandled rejection | Reported on the thread on whose job queue the check runs, with the process-wide handlers of `process.on("unhandledRejection")`, which run on the main thread |
| `AsyncLocalStorage` | The store follows the reaction. The current store is per thread. A new thread starts with the store that was current at `new Thread` |
| Modules | A module of the executable is evaluated at most once in the process, by the thread that first needs it. That thread evaluates the module and all that it imports and that is not evaluated yet. A thread that needs one of these meanwhile waits until the first has finished: `require` and static imports block, the promise of `import()` settles later. Sets of modules that do not meet are evaluated at the same time, so a thread can load a module while the entry module's body is still running or is blocked in `join`. What a thread evaluates is not always known when it begins: a `require` in a body, and the imports of a module that is not compiled, are found on the way, and the thread then waits for a module that another thread is evaluating while others may wait for its own. A wait that would close a cycle of waiting threads throws `Error("deadlock: ..")` in the thread that would close it; an `import()` that would close one is rejected with it (09, sections 9.9, 9.14). So two threads that enter one cycle of CommonJS modules from its two ends at the same time do not both see a partial `exports`, as one thread would: one of them gets the error. `import()` and `require()` work on every thread |
| `eval`, `new Function`, `node:vm` | Work on every thread, in the interpreter, if the program admits them at all (10) |
| Stack overflow | Per thread: each thread has its own limit and gets its own `RangeError` |
| Out of memory | The `RangeError` is thrown in the thread whose allocation failed |
| `process.exit(code)` | From any thread: ends the process. The `exit` handlers run on the main thread, also if it is blocked in `join`, `hold` or `wait` at that moment: it does not go back to what it was doing. No thread is unwound and no `finally` block runs, as for Workers. A thread that has called `process.exit` never completes: an `exit` handler that joins it gets `Error("deadlock: ..")` |
| The main thread's loop is empty | The process waits for spawned threads that are still running, as it waits for Workers. A thread that is blocked for ever keeps the process alive |
| Uncaught exception in a job or timer of a spawned thread | As on the main thread: `uncaughtException` handlers, which run on the main thread; without one the process ends |
| `RegExp.$1` and friends, `Math.random` streams | Per thread |
| `WeakRef` | A target that `deref()` returned stays alive until the end of the current job **of the thread that called it** |
| `FinalizationRegistry` | The callback runs on the thread that created the registry, or on the main thread if that one has completed |
| Native objects (`Bun.file`, sockets, `Database` ..) | Belong to the thread that created them unless documented as shared. A method call from another thread throws `ConcurrentAccessError` (11). Once the creating thread has completed every call throws `ConcurrentAccessError("the thread that created this object has ended")` |
| Listeners of `process` | `process` is one object. Its listeners are registered on the main thread and run there. On a spawned thread `process.on`, `once`, `off`, `emit` and the rest of the emitter are refused like every call that only the main thread may make (11, section 11.4) |
| Stack traces | The async part of `error.stack` ends at the first frame that belongs to another thread, with the line `at async <thread n>` |
| Workers | A `Worker` is a separate realm with a separate heap, as today. A Worker of an mt executable may start threads of its own |
| Debugger, sampling profiler | The inspector is not available in the first version. The sampling profiler samples every thread |

## 2.7 `Thread.restrict` and `ConcurrentAccessError`

`ConcurrentAccessError` is a subclass of `Error`.

`Thread.restrict(o)` marks `o` as usable only by the calling thread and returns it. Idempotent from the owner. From another thread it throws
`ConcurrentAccessError("Thread.restrict called from a non-owning thread")`. There is no way back: once the owner has completed, nobody can use the object, whatever `id` a later thread gets.

From other threads these throw `ConcurrentAccessError`: get, set, has, delete, defineProperty, ownKeys, setPrototypeOf, isExtensible, preventExtensions, by name and by index.
Not enforced: getPrototypeOf, call, construct.

Accepted receivers: plain objects (with or without a layout), instances of the program's own classes, arrays. Everything else: `TypeError("cannot restrict this object")`. An object that cannot be
enforced is never accepted. **[changed]** indexed get is enforced too; the exception of #249 was an artefact of its implementation.

How it is enforced without a test on fast paths: 05, section 5.11; for arrays and ordinary objects 06, section 6.4.

## 2.8 Which threads may block

A spawned thread may always block. The main thread may block if the embedder allows `Atomics.wait` on it. Bun allows it. Asynchronous variants and an uncontended `hold` never ask.

## 2.9 Differences from #249

| # | #249 (global lock off) | Here | Why |
| --- | --- | --- | --- |
| L1 | five globals | exports of `bun:threads` | 2.1 |
| L2 | a reaction runs on the thread that settles the promise; async functions migrate | on the thread that registered it; after that thread has completed, handler functions on the main thread, an `await` never | A function that only the main thread calls would otherwise run partly on other threads, which makes "MainOnly" unprovable (`research/R3-value-flow.md`, section 10) and surprises every program. #249 itself made this exception for the API's own promises |
| L3 | a thread is complete when its queues are empty | also waits for the promise that `fn` returned | With L2 an async `fn` that awaits something another thread settles would otherwise end at its first `await` |
| L4 | `import()` refused on spawned threads, module evaluation racy | modules work on every thread, evaluated once, disjoint sets at the same time | 09, section 9.9 |
| L5 | Bun's natives unusable on spawned threads | each thread has an event loop; natives are classified | 11 |
| L6 | indexed get on a restricted object not enforced | enforced | 2.7 |
| L7 | WebAssembly off in the whole process | available on the main thread; refused on spawned threads | 11, section 11.7 |
| L8 | `Atomics` store may land after `freeze` returned (defect) | never | 2.5 |
| L9 | any thread may resume a generator; a second one gets "already running" | only the creating thread | A suspended frame can hold values that belong to its thread (09, section 9.6). Can be lifted later (13, section 13.5) |

## 2.10 The memory model

### 2.10.1 Locations

A **location** is one of: an own property of an object (named or indexed, declared field or not); a variable (module level, or captured by a closure); an internal slot that the standard gives an
object (the `[[Prototype]]`, the extensible flag, `length` of an array, an entry of a Map or Set, the state of a promise or generator, `lastIndex`); a byte of an ArrayBuffer.

Two accesses **conflict** if they are to the same location and at least one writes. *Adding* or *deleting* a property writes that property's location and the object's key list, and nothing else.
In particular these pairs do **not** conflict, however the engine stores things:

- `o.a = 1` and `o.b = 2`, whether or not either property exists yet;
- `a[0] = x` and `a.push(y)` on an array that has an element 0.

One pair conflicts although the keys differ: `m.set(k1, v)` and `m.get(k2)`. The table of a Map or Set is one location. Each of its operations is atomic all the same (2.10.5).

### 2.10.2 Synchronization

These pairs are ordered (**happens-before**), in addition to program order within a thread. The operation at either end of an edge is a **synchronization operation**. The compiler declares exactly these operations `synchronizes` (04, section 4.4), the releasing ends as well as the acquiring ones.

| Edge | From | To |
| --- | --- | --- |
| Start | everything before `new Thread(fn)` in the creating thread | the start of `fn` |
| End | everything a thread did | the return of `join()`, the reaction of `asyncJoin()`, each of its handlers that runs on the main thread afterwards (2.6) |
| Lock | the release of a `Lock` (end of `hold`, call of `release`, entry into `wait`) | the next acquisition of it |
| Condition | nothing of its own: it synchronizes through its lock | |
| Atomics | an `Atomics` write | an `Atomics` read that reads it. All `Atomics` operations are in one total order consistent with happens-before |
| Wait | `Atomics.notify` | the return of the `wait` it wakes |
| Reaction | the settling of a promise: a call of a resolving function, `Promise.resolve` and its kin, the return or throw of an async function, a step of an async generator, the last element of a combinator | the start of each of its reactions, the continuation of each `await` of it |
| Task | a call that schedules work for another thread (a `release`, a native that posts) | that work |
| Module | the end of a module's evaluation, normal or by an exception | every use of the module by a thread that waited for or found it evaluated |

Some accesses are **atomic**:

- the `Atomics` operations;
- every access to the state of a promise: `then`, `catch`, `finally`, `await`, a settling, the test for "handled";
- every operation of `Thread`, `Lock` and `Condition` on the state of that object (`join` against completion, `hold` against `release`);
- every access to the state of a module (whether it is evaluated, and by whom).

All atomic accesses to one location are in one total order, which is consistent with happens-before.

A **data race** is a pair of conflicting accesses that are not ordered by happens-before and are **not both atomic**.

So `await p` on one thread and `resolve(p)` on another are no data race, although the Reaction edge orders the settling with the *reaction* and not with the *registration*; nor are `asyncJoin`, `asyncHold`, `waitAsync`. These words are synchronization, not data. What such a pair does is in 2.10.5.
The operations of `Map` and `Set` are not in the list. `m.get(k)` against `m.set(k, v)` is a data race, one whose outcomes are all good (2.10.5), and it orders nothing.

### 2.10.3 The guarantee for programs without data races

**M1.** An execution of a program without data races is sequentially consistent: it is an interleaving of the threads' steps.

### 2.10.4 The guarantees for all programs

These hold whatever a program does.

| # | Guarantee |
| --- | --- |
| **M2. Memory safety** | The process never reads or writes outside the object it names, never uses freed memory, never takes a value of one kind for another |
| **M3. No thin air, no wrong kind** | A read returns a value that some thread wrote to that location, or its initial value. A property that was only ever a data property never yields an accessor's internals. A location that only ever held numbers never yields an object |
| **M4. No tearing** | A value is read whole. A number is one of the numbers written, bit for bit |
| **M5. Coherence** | A thread sees its own last write to a location unless another thread has written since. Reads of one location **through one reference** (the same variable; the same object value and key), in program order, do not go backwards in the order of the writes. Two reads through references that the compiler cannot prove equal may be satisfied out of order: `r0 = o.x; r1 = p.x; r2 = o.x` with `p === o` may yield new, then old, because `r2` may reuse `r0` (2.10.6) |
| **M6. Publication** | An object that one thread creates and makes reachable by a plain store is seen by other threads with everything that was stored into it before that store. (No half-built object: not its header, not its fields, not the objects they point to.) |
| **M7. Deletion** | After `delete o.a` and the addition of `o.b` no reader gets `b`'s value under the name `a` |
| **M8. Structure** | The engine's invariants hold: no lost property, no key without a value, no duplicate key, no prototype cycle, `length` never below the index of an element that a reader can still see as present by `in` on the same snapshot |
| **M9. Integrity** | After `Object.freeze(o)` has returned, a write to `o` that **happens after** it fails. A plain write that races with it may still take effect, at the latest before the writing thread's next synchronization operation |
| **M10. Progress of a spinning reader** | A read, or a test of an object's state (whether a property exists, whether the object is frozen, which prototype it has), that can decide whether a loop is left is performed again in every turn of the loop. "Read" means a read of any location of 2.10.1, in the loop itself or in a function that it calls. A read by name is answered anew as well: not from what the thread remembers about the object's prototype chain (07, E2). So `while (!o.done) {}`, also where `done` appears on a prototype of `o`, `while (!("x" in o)) {}`, `while (!m.has(k)) {}` and `while (!isDone(o)) {}` end some time after another thread has made the change. Other reads may be performed once for many uses (2.10.6) |
| **M11. Refusals** | What the engine cannot do on a thread it refuses with a JavaScript exception or at build time. It does not abort. One exception: when the cage of cells is exhausted and both reserves are used up the process ends with a message (08, section 8.8) |

### 2.10.5 What a race may produce, by location

| Location | A racing read may see | Racing writes may | Never |
| --- | --- | --- | --- |
| Existing property, variable, array element | the old or a new value | leave either value | M2 to M5 violated |
| `x++`, `o.n += 1` from two threads | | lose an update | anything else |
| Two threads add different properties to one object | either key order | | a lost property, one property's value under the other's name |
| Two threads add the same property | | leave one of the values | two properties |
| `delete` against read | the old value or `undefined` | | M7 violated |
| `a.push(x)` from two threads | | lose one element: both may take the same index. `length` is a location that both read and write | a hole that neither wrote, an out-of-bounds access |
| `a[i] = x` (existing index) against `a.push(y)`, `a.length = n` with `n > i` | | not a race: both take effect | a lost store |
| `a.length = n` with `n <= i`, `pop`, against `a.push(x)`, `a[i] = x` | | leave the array with or without the element. An element that a racing `push` or store put above the new length may reappear when the length grows again | a value that nobody stored at that index, a reference to a dead object |
| `a.length = n`, `pop`, `shift`, `splice`, `sort`, `reverse`, `fill` against reads of the elements they move | any value that the location held before, during or after the operation, or a hole | | a value that was never in the array |
| `freeze`, `seal`, `preventExtensions` against writes | | M9 | a property that comes back without a value |
| `setPrototypeOf(a, b)` against `setPrototypeOf(b, a)` | | exactly one succeeds | a cycle |
| Change of a prototype's property, of a class's method | the old or the new member, until the reader's next synchronization operation, or the next turn of a loop whose exit depends on it (M10) | | |
| `Map`, `Set`, `WeakMap`, `WeakSet` | `get`, `has`, `set`, `add`, `delete`, `clear` see the table whole: they are linearizable. `size` and iteration may lag behind `has` and `get` by operations that are still in progress; `size` is exact with respect to operations that happen before it | | a lost entry, a foreign value, a key without its value |
| Iteration of a Map, Set or array against writers | what a single-threaded program sees that modifies the collection during iteration, with the modifications placed anywhere | | |
| Strings | immutable | | |
| Typed array elements | as for a SharedArrayBuffer today, for every buffer | | |
| Typed array against `transfer`, `resize`, detach | a length of 0, a `TypeError` that the standard allows, or the old bytes | writes may go to the old bytes | an access to unmapped or foreign memory |
| A generator or async generator resumed by a thread other than its creator | | that thread gets a `ConcurrentAccessError` (2.6) | two activations of one frame, an activation on another thread than its first |
| A promise settled by two threads | | one wins, as for two calls in one thread. That holds for `resolve` against `reject` of one pair too, also when the winner resolves with a thenable and the promise stays pending for a while | two settlements |
| `then` or `await` against the settling of the promise | | the reaction runs, once | a lost reaction, a reaction that runs twice |
| `d.setTime(t)` against `d.getHours()` | the hours of the old or of the new time | leave an old breakdown cached, which `getHours()` returns until the next `setTime` | anything but a breakdown of a time that the date has held |
| `re.lastIndex` on a shared RegExp | any value some `exec` wrote | | a match that is none |
| `Symbol.for(k)` from N threads | one symbol | | two |
| First use of a lazily made thing (a method closure, a class object, a template object, `fn.prototype`, a namespace object, `import.meta`, `error.stack`) from N threads | one identity, complete | | two identities, a thing that is half made |

### 2.10.6 What the compiler may do with plain accesses

For accesses that are not `Atomics`, between two synchronization operations of one thread the compiler may: perform a read once for several uses; move a read earlier or later; forward a stored value to a later read
of the same location; keep the outcome of a test of an object's state (frozen, has extra properties, prototype unchanged, key order). It may **not**:

- invent a write: store to a location on a path on which the program does not (**M3**, **M5**);
- invent a read whose value is then written back;
- split or merge accesses so that a value tears (**M4**);
- move an access across a synchronization operation in the direction that the operation forbids (all of 2.10.2 are full barriers for the compiler; 04, section 4.4);
- hoist a read that decides a loop's exit out of the loop (**M10**); the same holds for a **test of an object's state** that decides a loop's exit (`"x" in o`, `o.x === undefined` for a property that does not exist yet, `Object.isFrozen(o)`) and for a call that only reads;
- keep a fact whose staleness breaks **M2** (04, section 4.5 lists which facts those are and what replaces them).

Plain accesses to *different* locations are not ordered with each other, except by M6. x86-64 gives more; programs must not rely on it.

### 2.10.7 Relation to the standard's memory model

For bytes of a SharedArrayBuffer nothing changes. For all other locations the model above is the standard's model for `Unordered` accesses with four strengthenings: no tearing at the granularity of a value (M4), no
values out of thin air (M3), coherence of the reads through one reference (M5), and M10. `Atomics` on properties are `SeqCst` events on locations that are values instead of bytes.

## 2.11 Conformance list

[12](12-testing.md), section 12.2, turns M1 to M11 and the table of 2.10.5 into tests: each rule, for each kind of location (declared field in line, declared field in an Extension, extra property, property of an ordinary object in line
and out of line, dictionary property, element of each kind, typed array element, module variable, captured variable, static field, Map entry), typed and untyped, hot and cold code shapes.
