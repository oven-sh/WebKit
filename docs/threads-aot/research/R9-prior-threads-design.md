# R9. Digest of the earlier shared-memory threads work (JIT engine, oven-sh/WebKit#249, branch `sosuke/threads`)

Purpose: let a new design (threads only in AOT-compiled executables) keep the JavaScript-level API, its semantics and the
lessons of the earlier work, without keeping its implementation.

**Contents.** 0 how to read; 1 the JavaScript API; 2 what happens to exceptions, promises, exit, modules ...; 3 the memory model; 4 built-ins; 5 the hard problems, their mechanisms, costs and causes; 6 where the GIL-off time goes;
7 the owner's decisions and the open defects; 8 the GIL-on mode; 9 testing assets; 10 hazards with several mutators; 11 what helps; 12 not found / not sure. Sections 0 to 12 (about 740 lines) are the report.
Appendices A to E (about 2,500 lines) are the detailed digests behind sections 5, 6, 9 and 10: A object model, B JIT / inline caches / watchpoints / stops / polls, C heap and GC, D performance and decisions, E testing assets. Each appendix has its own citation legend and its own "not found" list.

## 0. How to read this report

### 0.1 Path shorthand
Everything cited is under `<threads-branch>/` (checked out at `2b14c39e3638`, branch `sosuke/threads`,
the seventeenth round). Shorthand:

| Prefix | Means |
|---|---|
| `D/` | `docs/threads/` |
| `S/` | `Source/JavaScriptCore/` |
| `T/` | `JSTests/threads/` |
| `THREAD.md` | the design of record at the repository root (`THREAD.md:1-23`; the rest of that file is a copy of Filip Pizlo's 2017 blog post, marked "Outdated Blogpost") |

### 0.2 The documents are layered, and later layers overrule earlier ones
1. `THREAD.md` (23 lines of design): the goal and the contract.
2. Five "frozen" specs written before implementation for a **GIL'd** first phase: `D/SPEC-api.md`, `SPEC-heap.md`,
   `SPEC-vmstate.md`, `SPEC-objectmodel.md`, `SPEC-jit.md`. Each has a `-history.md` (reasons, review rounds, later
   amendments) and some an `-annex.md`.
3. `D/SPEC-ungil.md` (rev 35): GIL removal, "N-mutator execution model". It supersedes parts of the five ("SUPERSESSION ...
   both sides"). Its two executed audits are the best inventory of shared C++ state:
   `D/SPEC-ungil-audit-K4.md` (VM and JSGlobalObject members) and `D/SPEC-ungil-audit-N7.md` (cell-internal state of builtins).
4. `D/SPEC-congc.md` (concurrent marking with N mutators), `D/SPEC-nativeaffinity.md` (a DRAFT that was **never implemented**:
   `grep -r "NativeSerialLock\|m_concurrentOk" Source/JavaScriptCore` finds nothing; `D/PARITY-PLAN.md:943-946` "is a draft:
   nothing of it is in the tree").
5. `D/LANDING-PLAN.md`: seventeen "rounds" (2026-09-02 to 2026-10-14) of fixing, measuring and rebasing. Rounds amended the
   specs in place ("tenth round", "fifteenth round" paragraphs inside the SPEC files). "Current state" `:14-55`, "Open items"
   `:5029-5269`, "Results, seventeenth round" `:4790-5027`.
6. The eleventh session (2026-09-17, no code) produced `D/DESIGN-PROPOSALS.md` (what each remaining cost is and what would fix
   it), `D/PARITY-PLAN.md` (what it takes to reach 0.90 of `main` GIL off, and what "as safe as `main`" means) and
   `D/FLAG-OFF-LANDING.md` (land with the flag off first).

So: a statement in a SPEC file is what was intended; LANDING-PLAN / AUDIT / PARITY-PLAN say what is true of the tree.

### 0.3 The three modes
| Name | Options | Meaning |
|---|---|---|
| flag off | none | the branch built, `useJSThreads` not set. Must equal `main`. |
| GIL on | `--useJSThreads=1` | the API exists, threads are real OS threads, but only the holder of the VM's `JSLock` runs JavaScript. |
| GIL off | `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0` (plus `useVMLite`, `useSharedAtomStringTable`) | N threads run JavaScript at once in one VM (`D/LANDING-PLAN.md:48-49`, `D/SPEC-ungil.md:15-20`). |

GIL off was never declared safe: `gilRemovalPreconditionsMetValue` is a `constexpr false`, and the mode needs an option with
"Unsafe" in its name (`D/PARITY-PLAN.md:955-962`; `S/runtime/OptionsList.h:794`).

### 0.4 Vocabulary used by the documents
- **lite / VMLite**: the per-thread block of execution state split out of `VM` (section 5, P33).
- **carrier**: the main thread or an embedder thread that enters the VM through the real `JSLock`; **spawned**: a thread made by `new Thread`.
- **token**: what `JSLockHolder` means on a spawned thread GIL off: not a mutex, an "entered" record plus heap access (`D/SPEC-ungil.md:632-644`).
- **heap access**: the right to touch the GC heap; released around every blocking wait so that a stop need not wait for the sleeper.
- **conductor**: the thread that runs a stop-the-world (a collection or a "JSThreads stop").
- **ticket / AsyncTicket**: the native record behind a promise returned by `asyncJoin` / `asyncHold` / `asyncWait` / property `waitAsync`.
- **SD n**: numbered "semantic deltas" of GIL off against GIL on (`D/SPEC-ungil.md:1020-1044`).
- **cell lock**: the 2-bit lock in the `IndexingType` byte of every `JSCell` header (`THREAD.md:11`).

---

## 1. The JavaScript API (question 1, first half)

Five global constructors, `DontEnum`, installed eagerly in `JSGlobalObject::init()` only when the flag is on: `Thread`, `Lock`,
`Condition`, `ThreadLocal`, `ConcurrentAccessError` (`D/SPEC-api.md:43`, `:405-410`). All require `new` (TypeError
`"calling X constructor without new is invalid"`: `S/runtime/ThreadObject.cpp:341`, `LockObject.cpp:737`, `ConditionObject.cpp:74`,
`ThreadLocalObject.cpp:66`). Instances are `JSDestructibleObject` subclasses with ordinary prototypes and a `Symbol.toStringTag`
each; subclassing works (`newTarget != callee` goes through `createSubclassStructure`, `ThreadObject.cpp:363-369`;
`T/api/threadlocal-subclass-structure.js`). Plus: every `Atomics.*` function accepts `(object, propertyKey, ...)`.

There are **no options objects anywhere** in the API. There is no `detach`, no `cancel`, no `terminate`, no thread name, no
priority, no message passing: "Lifecycle:Running->Finished(result)|Failed(exc);no detach/cancel" (`D/SPEC-api.md:54`); "NO
Thread.prototype.terminate" (`D/SPEC-ungil.md:166-167`).

### 1.1 `Thread`

| Member | Signature | Semantics | Source |
|---|---|---|---|
| constructor | `new Thread(fn, ...args)` | `fn` must be callable, else TypeError `"Thread constructor requires a callable argument"`. Spawns an OS thread **immediately**; it runs `fn(...args)` with `this === undefined` (a sloppy function sees `globalThis`). `fn` and `args` are the same objects, not copies. | `D/SPEC-api.md:47`; `ThreadObject.cpp:344-431`; `T/api/thread-basic.js:37-60` |
| | errors | RangeError `"too many live Threads (or thread-ID space exhausted)"` (over `maxJSThreads`, default 16383 in the tree, 32766 in the spec; or, GIL off, spawn from a VM other than the one VM that may have threads); RangeError `"could not create a native thread for Thread"` (OS refusal). | `ThreadObject.cpp:379`, `:426`; `OptionsList.h:783`; `D/SPEC-ungil.md:42-49` |
| `join` | `thr.join()` | Blocks until completion; returns `fn`'s value or **rethrows the same exception value** (identity, not a copy). Any thread may join, any number of times, at any time; all agree. Self-join: `Error("Thread cannot join itself")`. On a thread that may not block: TypeError `"Thread.prototype.join cannot block the current thread"` (only if the target is still running). | `D/SPEC-api.md:48`; `ThreadObject.cpp:435-564`; `T/api/thread-exc.js:10-24` |
| `asyncJoin` | `thr.asyncJoin()` | Promise of the result / rejection with the exception. Never blocks, never settles synchronously in the call. Repeated calls return distinct promises with the same settlement. | `D/SPEC-api.md:49`; `ThreadObject.cpp:566-608`; `T/api/thread-lifecycle.js:52-79` |
| `id` | getter on the prototype | The engine thread ID. Main is 0. Spawned ids are unique, in `[1, 0x3fff]` in the tree ("the spawned half of the TID space; carrier lites take the upper half"). | `D/SPEC-api.md:50`; `T/api/thread-id-bounds.js:2-5` |
| `Thread.current` | static getter | The caller's `Thread` object. Inside a spawned thread it is `===` the object `new Thread` returned to the parent. On main/embedder threads it is created lazily at first access and is stable. `asyncJoin()` on the main thread's object never settles and keeps the process alive. | `ThreadObject.cpp:644-656`, `:579-583`; `T/api/thread-basic.js:62-75` |
| `Thread.restrict` | `Thread.restrict(o)` returns `o` | See 1.6. | |

Examples (`T/api/thread-basic.js:17-35`): `new Thread(() => obj).join() === obj`; `-0`, `NaN`, symbols and BigInts round-trip.

### 1.2 `Lock`

Non-recursive mutex built on `WTF::Lock` plus a holder word and a FIFO of async acquirers (`D/SPEC-api.md:153-169`).

| Member | Semantics | Source |
|---|---|---|
| `new Lock()` | no arguments | `D/SPEC-api.md:58` |
| `lock.hold(fn)` | `fn` callable else TypeError `"Lock.prototype.hold requires a callable argument"`. If the caller already holds it: `Error("Lock is not recursive")`, outer hold kept. `tryLock` first, so an **uncontended hold never blocks and is always allowed**, even on a thread that may not block; contended on such a thread: TypeError `"Lock.prototype.hold cannot block the current thread"`. Runs `fn()`, releases as `finally` would, returns or rethrows. | `D/SPEC-api.md:59`; `LockObject.cpp:766-878`; `T/api/lock-basic.js:14-47` |
| `lock.asyncHold(fn)` | Promise. Once granted, `fn()` runs on an event-loop turn holding the lock; the lock is released after `fn` returns (not after a promise it returns settles); the promise settles with `fn`'s result or exception. | `D/SPEC-api.md:60`, `:212`; `T/api/lock-async-hold.js:42-61` |
| `lock.asyncHold()` | Promise of a `release` function, to be called exactly once: twice is `Error("Lock release function called more than once")`; never means later acquirers stall for ever (documented). The release function is a transferable capability: any thread may call it. | `D/SPEC-api.md:60`, `:211`; `LockObject.cpp:723`; `T/api/lock-async-hold.js:20-37`; `D/INTEGRATE-api.md:1146-1150` |
| | If the caller **sync**-holds: `Error("Lock is not recursive")`. An async hold is not recursion: callers queue. A sync `hold` inside a function delivered by `asyncHold(fn)` throws "Lock is not recursive" instead of deadlocking (landed deviation D10). | `D/INTEGRATE-api.md:1095-1115` |
| `lock.locked` | getter; racy; "tests only" | `D/SPEC-api.md:61` |
| ordering | Sync against async order unspecified, barging allowed; async tickets FIFO; async starvation possible under perpetual sync contention. | `D/SPEC-api.md:62`, `:210` |

### 1.3 `Condition`

| Member | Semantics | Source |
|---|---|---|
| `cond.wait(lock)` | `lock` must be a `Lock` (TypeError `"...requires a Lock argument"`) **sync-held by the caller** (TypeError `"...requires the lock to be held by the caller"`); on a thread that may not block TypeError `"...cannot block the current thread"`. Atomically enqueues, releases the lock, blocks; on wake-up reacquires, returns `undefined`. **Spurious wake-ups are allowed; predicate loops are mandatory.** No timeout parameter. | `D/SPEC-api.md:66`, `:183-192`; `ConditionObject.cpp:96-118`; `T/api/condition-basic.js:25` |
| `cond.asyncWait(lock)` | `lock` sync-held by the caller or async-held (by a delivered grant; for a with-fn grant only by the thread running `fn`: D6, D12), else TypeError `"...requires the lock to be held"`. **Consumes the hold**: releases the lock now. Promise: on notify the ticket joins the lock's async queue; when granted, resolves with a fresh `release` function. | `D/SPEC-api.md:67`; `D/INTEGRATE-api.md:1028-1040`, `:1136-1152` |
| `cond.notify()` / `notifyAll()` | Wake one / all, sync and async waiters uniformly, FIFO across kinds. Return the count woken. Holding the lock is optional. | `D/SPEC-api.md:68`; `T/api/condition-basic.js:15-17` |

### 1.4 `ThreadLocal`
`new ThreadLocal()`; one accessor `value` on the prototype; a per-thread slot, initially `undefined` on every thread, any JS value.
Storage is a map in the thread's native state keyed by a process-unique 64-bit key, so get and set take no lock. Values are GC roots
until overwritten or the thread exits. A dead `ThreadLocal` object leaks its slots in live threads until they exit (documented).
`D/SPEC-api.md:70-72`, `:254-256`; `T/api/threadlocal-basic.js:9-38`.

### 1.5 `Atomics` on ordinary objects
Dispatch (`D/SPEC-api.md:74-96`): argument 0 a typed array view: today's path, unchanged. Else an object: property path, argument 1
through `ToPropertyKey`. Else TypeError as today. `Atomics.isLockFree` and `pause` unchanged. "Prop semantics (all SeqCst; one atomic step each)".

| Op | Semantics (own **data** property `k`; prototype chain never consulted) |
|---|---|
| `load(o,k)` | absent, accessor or inherited: TypeError `"Atomics.load: object has no own property"`. A non-writable property is loadable. |
| `store(o,k,v)` | sets; **creates the property if absent and `o` is extensible**; TypeError for accessor / not writable / non-extensible and absent. Returns `v` unconverted (`store(o,"x",7.9)` is 7.9). |
| `exchange(o,k,v)` | as store but `k` must exist; returns the prior value |
| `compareExchange(o,k,exp,rep)` | compares with **SameValueZero** ("=== breaks NaN CAS loops"); returns the value read |
| `add sub and or xor` | the stored value must be a Number (else TypeError `"Atomics RMW: stored value is not a number"`; a BigInt slot throws too); operand through `ToNumber`; `and/or/xor` use `ToInt32` on both and give an int32; `add/sub` are double arithmetic; returns the old value |
| `wait(o,k,exp,timeout?)` | `"not-equal"` / `"ok"` / `"timed-out"`; blocks (subject to the blocking gate); termination surfaces as the termination exception |
| `waitAsync(o,k,exp,timeout?)` | `{async:false,value:"not-equal"}` or `{async:true,value:Promise}` |
| `notify(o,k,count?)` | wakes at most `count` waiters of exactly `(o,k)`; returns the count; 0 is valid even if `o` lacks `k` |

Details that were learnt in implementation and belong to the contract:
- Waiters are keyed by `(cell, property uid)`, not by slot address ("slots move": Dev 3, `D/SPEC-api.md:13`). Property and typed-array
  waiters never wake each other; `(o,"k")` is unaffected by `(o,"j")` (I11, `:307`). A waited-on object is kept alive by the waiter list (`:224`).
- Receivers refused with TypeError (landed deviation D3, `D/INTEGRATE-api.md:984-997`): `Proxy` and the global proxy
  (`"Atomics property operations cannot be performed on a Proxy"`: a trap would run JS inside the "one atomic step"), and own data
  properties that are not plain structure/butterfly storage (`"...require a plain data property"`): array `length`, `RegExp` `lastIndex`,
  `String` object characters, sparse-map indices, global `var` bindings. Dictionary-mode properties and indexed elements are fine.
  `S/runtime/ThreadAtomics.cpp:122-183`.
- Every writing op throws on a read-only property, CAS and RMW included, before the comparison (D7, `D/INTEGRATE-api.md:1041-1051`;
  `T/atomics/property-errors.js:48-59`).
- GIL off the ops are lock-free seq_cst on flat storage and cell-locked on dictionaries and ArrayStorage (`D/SPEC-ungil.md:409-415`).
- Open defect O17-3: a property `Atomics` store can land after `Object.freeze` has returned on another thread (section 7.4).
- Cost to conformance: 34 results of the ordinary JSC suites differ with the flag on only because `Atomics.load({}, ...)` no longer
  throws; proposed an own option `useAtomicsOnObjects` (`D/LANDING-PLAN.md:5211-5213`).

### 1.6 `Thread.restrict(o)` and `ConcurrentAccessError`
`ConcurrentAccessError`: global constructor, `Error` subclass, `name` "ConcurrentAccessError", empty default message
(`D/SPEC-api.md:52`; `ThreadObject.cpp:963-964`; `T/api/concurrent-access-error-shape.js`).

`Thread.restrict(o)` marks `o` as usable only by the calling thread and returns `o`. Idempotent from the owner; from another thread it
throws `ConcurrentAccessError("Thread.restrict called from a non-owning thread")`. There is no un-restrict.
- **Enforced** from other threads (throws CAE): get, set, has, delete, defineProperty, ownKeys, setPrototypeOf, isExtensible,
  preventExtensions, indexed set / delete / define. **Not enforced** (documented): getPrototypeOf, call/construct, indexed **get**
  (`D/SPEC-api.md:18`).
- **Refused receivers** (TypeError `"cannot restrict this object"`): non-objects, global object, global proxy, Proxy, scope objects, the
  species-protected builtin prototype/constructor pairs (Array, Promise, RegExp, ArrayBuffer, SharedArrayBuffer, each typed array pair),
  structures that hijack the indexing header, Wasm GC objects, every ArrayBufferView including DataView, and (D13) **every class whose
  method table overrides any enforced entry point**. What is left is an allowlist: plain objects and `JSArray`
  (`ThreadObject.cpp:720-818`; `D/INTEGRATE-api.md:1153-1174`).
- Mechanism: no new object-model machinery. The object is converted to SlowPutArrayStorage plus an uncacheable dictionary and pinned
  against flattening, so that no inline cache ever covers it and each enforced operation reaches a generic C++ entry point, which begins
  with `if (useJSThreads && structure->isUncacheableDictionary() && !threadRestrictCheck(...))`; ownership is in a process-wide side table
  with a "count is zero" fast path (`D/SPEC-api.md:242-252`). So restricting makes the object slow for its owner too.
- Open question of scope O17-7: a put on an object that **inherits** a setter or read-only property from a restricted object is not refused.
- Lesson: the enforcement was twice found to rest on accidents of the JIT object model (`D/SPEC-api.md:252`: two fast paths "which
  per-thread butterfly tags used to cover by accident").

### 1.7 The blocking gate
One predicate decides whether a thread may park synchronously (`join`, contended `hold`, `cond.wait`, `Atomics.wait` of both kinds):
a spawned thread always may (GIL off); main/embedder threads follow the embedder's `isAtomicsWaitAllowedOnCurrentThread()`
(`D/SPEC-ungil.md:706-716`; `ThreadObject.cpp:90-97`; `T/api/blocking-gate.js`, `T/api/spawned-thread-may-block-gil-off.js`). Async variants and
uncontended `hold` never consult it (I18, `D/SPEC-api.md:314`). GIL on only: a **typed-array** `Atomics.wait` on a spawned thread throws
TypeError (I21), lifted GIL off (SD4).

### 1.8 Engine options that are part of the surface
`useJSThreads`, `maxJSThreads` (16383), `jsThreadStackSizeKB` (0 = platform default), `jsThreadGILTimeSliceMs` (reserved, inert),
`jsThreadsStopWatchdogMs` (30000: a stop that does not complete aborts the process), `maxJSThreadHeapBytes` ("INERT STUB")
(`S/runtime/OptionsList.h:769-797`). The text of `useJSThreads` records a security ruling: enabling it "is a high-resolution-timer
capability grant" (`OptionsList.h:769`; `D/cve/map-MC-SPEC.md`).

---

## 2. What happens to ... (question 1, second half)

Where GIL on and GIL off differ, the GIL-off rule is the one a design without a GIL inherits.

| Situation | Behaviour | Source |
|---|---|---|
| **Exception thrown by `fn`** | Caught at the thread's top. The thread becomes `Failed`; the thrown **value** (not the `Exception` wrapper) is stored; every `join()` rethrows it, every `asyncJoin()` rejects with it. Nothing is printed and the process does not die. A thread nobody joins fails silently. | `ThreadObject.cpp:229-243`, `:556-563`; I3 `D/SPEC-api.md:299` |
| **Unhandled rejection on a spawned thread** | The host's `promiseRejectionTracker` is not called on the spawned thread: the event is appended to a hand-off queue and the tracker runs later on the carrier, at its next drain (SD15). | `D/SPEC-ungil.md:504-510` |
| **When is a thread complete?** | GIL on: when `fn` returns, after one drain of the (shared) microtask queue. **GIL off: only when `fn` has returned AND its own microtask and task queues are empty AND its keep-alive count is 0** (SD1, U7). So `join()` observes the effects of the thread's `await`s and async lock grants. | `D/SPEC-ungil.md:519-541`; `D/SPEC-ungil-history.md:1356-1364`, `:1408-1410`; `ThreadObject.cpp:245-258` |
| **Event loop on a spawned thread** | Every spawned thread has its own: after `fn`, loop { drain own microtasks; release heap access; under the inbox lock: termination => close, else take a task, else keep-alive 0 => close, else wait }; expire `waitAsync` deadlines; run the task. Tasks are only the settle tasks of AsyncTickets. "Park sites inside fn do NOT service the task queue." | `D/SPEC-ungil.md:519-541` |
| **Keep-alive** | Count of outstanding registrations that may still post a task to this thread: `asyncHold`, `asyncWait`, property `waitAsync`. **Not** `asyncJoin` (SD12: two threads that `asyncJoin` each other must not keep each other alive) and not typed-array `waitAsync` (SD11, settles on main). A never-released lock or never-notified wait keeps the thread (or, on main, the process) alive for ever: "Intentional leak". | `D/SPEC-ungil.md:543-552`; `D/SPEC-api.md:101-102` |
| **Microtasks** | One queue per thread, "enqueued/drained ONLY by its owner" (I11). `queueMicrotask` and every drain go to the current thread's queue. No microtask runs inside a blocking call (D11; `T/api/park-no-microtask-drain.js`). After a blocking `join()` returns, the joiner drains its own queue before returning to JS. | `D/SPEC-ungil.md:473-476`; `ThreadObject.cpp:517-545` |
| **Which thread runs a promise reaction?** | **The thread that settles the promise** (SD10): the resolver enqueues the reaction jobs on its own queue; there is "no per-reaction registrant hop v1". So `await p` in thread A continues **on thread B** if B resolves `p`. An async function can therefore migrate between threads at each `await`. | `D/SPEC-ungil.md:488-503`; `D/PARITY-PLAN.md:1099` "an async function's continuation runs on the settling thread in both flag-on modes" |
| **...except the API's own promises** | `asyncJoin` / `asyncHold` / `asyncWait` / property `waitAsync` promises settle on the thread that **registered** them (a task hop through its inbox); if that thread's inbox is closed (it finished), on main (SD3, U22). | `D/SPEC-ungil.md:554-578` |
| **`await` on a thread** | `await thr.asyncJoin()`. There is no thenable on `Thread` itself. | `T/lifecycle/async-join.js` |
| **Concurrent `then()` / `resolve()` on one promise** | Promise state transitions take the promise's cell lock GIL off; the reaction list walk copies under it. | `D/SPEC-ungil.md:496-501`; `T/shared-objects/promise-reaction-walk-vs-then-gil-off.js` |
| **`AsyncLocalStorage`** | Preserved across migration. The context is captured **per reaction at registration** into the reaction's own tuple and swapped in and out around the job by whichever thread runs it. The "current context" cursor, one per global object on `main`, had to become per thread. A continuation that runs on B sees the store of its registration on A; B's own value is restored afterwards. | `D/SPEC-ungil.md:511-517`; `D/SPEC-ungil-history.md:5211-5285` |
| **Timers** | Not an engine concept. The `jsc` shell's `setTimeout` on a spawned thread "posts to that thread's run loop" (a shell fix). In Bun timers do not work on a spawned thread (next row). Finite `waitAsync` timeouts of a spawned registrant are deadlines on its own state and fire only when that thread drains or closes (SD16). | `D/LANDING-PLAN.md:1634-1638`; `D/SPEC-ungil.md:617-625` |
| **Bun natives on a spawned thread** | **Unusable.** "Bun's Rust host functions find their `VirtualMachine` through a thread-local that only Bun's own threads set, so essentially every Bun native (`console.log`, timers, `require`, `fetch`, `process.env`, `path.*`, `Bun.*`) is unusable from a JSC-spawned thread today (a null dereference in Release)"; behind that the per-VM singletons are unsynchronized. Left as "a policy decision for Bun (refuse ... or route them)". | `D/LANDING-PLAN.md:1936-1946`; open item `:5244-5245` |
| **`process.exit`, exit with threads running** | Not specified at Bun level (not found). Engine: "No implicit join; mid-fn teardown may terminate abruptly, no invariant covers it". The native thread is detached and holds a `Ref<VM>`, so the VM outlives it. `~VM` blocks until no other thread's lite points at the VM. An embedder that destroys a VM must join every Thread, then call `ThreadManager::waitForCompletedThreadsToReleaseVM(vm)` without the API lock, then release. Destroying a VM under a running Thread "stays a contract violation and keeps the fail-stop". | `D/SPEC-api.md:102-103`; `ThreadObject.cpp:397-416`; `D/SPEC-ungil.md:373-382`; `T/cve/mc-tdwn-vm-teardown-unjoined.js` |
| **What keeps the process alive** | A pending ticket registered on main (as an un-notified `waitAsync` does today). Whether a running, unjoined spawned thread does is not stated beyond "No implicit join" (section 12). | `D/SPEC-api.md:101-102`, I20 `:316` |
| **Termination** | VM-wide only in the spec: a termination request (watchdog, embedder) is fanned to every thread's trap word; each thread takes its own close; parked threads notice within a 10 ms quantum (D9: every park site polls). A terminated thread is `Failed` with a **fresh ordinary `Error("Thread terminated")`**, never the sticky termination exception; its undrained microtasks are dropped (SD17); its tickets settle later on main. The VM survives. Later (audit row VM-9) a time limit on one call (`node:vm` timeout) terminates only the calling thread. | `D/SPEC-ungil.md:164-171`, `:580-593`; `D/INTEGRATE-api.md:1070-1094`; `T/giloff-time-limit-terminates-one-thread.js` |
| **Stack overflow** | Per thread: each thread has its own limits and gets its own RangeError. | `D/SPEC-ungil.md:155-157`; `T/semantics/stack-overflow-per-thread.js` |
| **Out of memory on one thread** | Not tested: `T/semantics/oom-one-thread.js` is skipped, "the heap cap options are inert". | `D/PARITY-PLAN.md:1108` |
| **`import()` on a spawned thread** | GIL off: **rejects with TypeError** before the loader hook is consulted. `new ShadowRealm()` throws TypeError. Main keeps both. | `T/api/import-and-shadowrealm-refused-gil-off.js:1-7` |
| **Using modules from threads** | Bindings and namespace objects of already loaded modules are ordinary shared objects (one namespace object is published: `T/api/module-namespace-identity-gil-off.js`). **Module evaluation still reaches spawned threads** (first read of a deferred namespace; a top-level-await continuation on the settling thread) and has no claim: measured, 4 threads on 200 deferred modules, "121-139 TypeErrors and 4-11 TDZ ReferenceErrors per thread, 12-17 modules evaluated twice". Open; proposed one module-evaluation lock. | `D/LANDING-PLAN.md:5200-5203`; `D/SPEC-ungil-history.md:5539-5553` |
| **`eval` / `new Function` on threads** | Allowed. The code caches are locked, parse outside the lock and publish under it. Global declaration instantiation of two scripts must not interleave (`T/semantics/global-declaration-race.js`). Open: concurrent indirect evals declaring one global function can show a caller `undefined` for a moment; "not decided whether declaration instantiation racing a reader is permitted". | `D/SPEC-ungil-audit-K4.md:341`; `D/PARITY-PLAN.md:1003`, `:1106` |
| **First call of a function from two threads** | Both may compile; publication is a release-CAS of the executable's CodeBlock slot, the loser discards. | `D/SPEC-ungil.md:914-920` |
| **WebAssembly** | GIL on: refused on spawned threads (TypeError, SD7); Wasm GC refused with LinkError. GIL off: "WebAssembly does not exist", forced off for the whole process. The GIL-on refusal had holes (W4: exports reached through `JSON.parse` revivers, `replace` callbacks, promise reactions). | `D/SPEC-ungil.md:726-768`; `D/DESIGN-PROPOSALS.md:159` |
| **`bun:ffi`** | GIL off only the main thread can create or call an FFI function; a spawned thread gets TypeError. "Out of scope by standing decision". | `D/LANDING-PLAN.md:238-241`, `:5259-5264` |
| **Debugger, profiler** | Breakpoints on spawned threads are ignored (SD13); the sampling profiler samples only the main thread (SD18); the watchdog's budget is the carrier's (SD14). | `D/SPEC-ungil.md:184-197`, `:1033-1039` |
| **`RegExp.$1` etc.** | Per thread GIL off (SD19). | `D/SPEC-ungil-audit-N7.md:164-179` |
| **`Math.random`** | Per-thread streams, independently seeded. | `D/SPEC-ungil-audit-K4.md:442` |
| **Destructors and finalizers of native objects** | Run on **whichever thread conducts the collection or sweeps the block**. Bun panicked on this (F31: a Rust `RefPtr` locked to the main thread). The Bun patch posts native finalizers to the VM's event loop when a spawned thread runs the destructor. `FinalizationRegistry` callbacks are scheduled through the deferred-work timer, so they run on the carrier, never on a spawned registering thread. | `D/LANDING-PLAN.md:2462-2471`; oven-sh/bun branch commit `6e39e39c579` message; `D/PARITY-PLAN.md:1092` |

---

## 3. The memory model promised to JavaScript programs (question 2)

### 3.1 The one-line contract
`THREAD.md:5`: "Memory model = SharedArrayBuffer's; the engine guarantees object-model invariants under races (no lost properties, no torn
shapes, no corruption), not race-free programs."

The most careful restatement is property S5 of the safety program (`D/PARITY-PLAN.md:925-937`):

> A program with data races observes, for every read, a value that some thread wrote to that location (or the initial value), of the kind
> the location holds; per-location order is respected by one thread's successive reads; a write becomes visible to a polling reader within
> a stated bound. Nothing else is promised, and nothing less. [...] the performance plan may relax *when* a write is seen, never *what* is seen.

and S1 (memory safety, `:811-816`): no program, racy or not, makes the process "read or write memory outside the object it names, use freed
memory, or treat a value of one kind as another (a cell where a value is expected, a raw double where a boxed value is expected, a butterfly
of one shape read as another)".

### 3.2 The seven rules, as a conformance list
`D/PARITY-PLAN.md:1146-1167` proposes (it was never written as tests: "there is no test set that enumerates them per tier", `:935-936`) a litmus
set "For locations of each kind (inline property, out-of-line property flat and segmented, array element of each indexing shape, typed-array
element, global variable, closure variable, `Map` entry)":

1. **No thin air, no wrong kind**: only values some writer stored; "a location that only ever held int32s never yields a double, a cell, or a
   hole; a property that was only ever data never yields a `GetterSetter`".
2. **No tearing**: "a JSValue is read whole; a Double lane is one of the written doubles bit for bit".
3. **Per-location coherence**: two reads by one thread in program order do not go backwards against one writer's increasing sequence.
4. **Message passing through `Atomics` and `Lock`**: a plain write before a release is seen after the matching acquire.
5. **Bounded visibility**: a spinning reader with no synchronization sees a plain write (subject to decision 1, never ruled; sections 7.1 and 7.3).
6. **Publication**: "an object built by one thread and published by a plain store is seen with its constructor's fields or not at all".
7. **Deletion and reuse**: "a property deleted and another added never makes an old reader see the new property's value under the old name".

This list is implementation independent and is the best starting point for the new design's memory-model section.

### 3.3 What a race may and may not produce, by location
| Location | May | May never | Evidence |
|---|---|---|---|
| existing named property, read vs write | old or new value | torn value, another property's value, going backwards ("time travel") even while a third thread grows the storage | `T/invariants/no-time-travel.js:1-9` (I21) |
| `x++` / read-modify-write from two threads | lost updates, "as on a SharedArrayBuffer" | anything else | `D/LANDING-PLAN.md:164-166` (a test that counted with `++` was ruled wrong) |
| two threads add different properties to one object | both orders of `Object.keys` | a lost property; `o.f` holding `g`'s value; a key visible without its value | `T/invariants/no-torn-shapes.js:1-30` (I9), `no-lost-properties.js` |
| two threads add the same property | one of the two values | two properties | `T/invariants/no-lost-properties-same-name.js` |
| delete vs read | the pre-delete value or `undefined` | the value of a property added later that reuses the slot (slots are quarantined until a GC epoch) | `T/invariants/delete-quarantine.js:1-12` (I18, I30) |
| `Object.freeze/seal` vs delete/add | either order | a deleted property coming back with no value (this happened: `D/LANDING-PLAN.md:143-152`) | `T/semantics/frozen-seal-race.js` |
| `setPrototypeOf` a->b vs b->a | exactly one succeeds, the other gets the cycle TypeError | a cycle | `T/semantics/proto-cycle-race.js` |
| array elements, `push` vs read, growth | old or new element, hole/`undefined` beyond the length seen | lost elements written by their only writer, out-of-bounds access | `T/invariants/no-lost-elements.js`; section 5 P1, P2, P11 |
| array `length` read then copy (`TypedArray.prototype.set(array)`) | a spec-legal exception (RangeError) if another thread changed the length in between | memory unsafety | `D/LANDING-PLAN.md:128` |
| typed array vs `transfer`/detach/`resize` | spec-legal TypeError; stale length 0 | "a racing reader never pairs a passing length with an unmapped-or-short base" | `D/SPEC-ungil.md:882-889` |
| typed array elements | exactly SharedArrayBuffer's races (for any buffer, since any view can be reached by any thread) | | `D/PARITY-PLAN.md:1020` |
| `re.lastIndex` on a shared RegExp | any value some `exec` legitimately wrote | torn number, out of range; a match that is not a real match | `T/semantics/regexp-lastindex-shared.js` |
| closure variable, global variable | treated as locations like properties (listed in 3.2); spin loops on them must terminate (`T/jit/poll-visibility-control-reads-gil-off.js`: "a spin on a plain field, element, closure or global variable, a cancel flag, a loop bound") | | `D/LANDING-PLAN.md:3625-3633` |
| `Map`/`Set` | each operation linearizable: "each operation must see the table whole" | lost entry, foreign value, crash | `T/shared-objects/map-set-shared-writers.js`; section 4 |
| strings | immutable to JS; rope resolution and atomization are internal and race-free | | `D/SPEC-ungil.md:858-862`; `T/semantics/atom-rope-torture.js` |
| `Symbol.for(k)` from N threads | one symbol | two | `T/semantics/symbol-registry-cross-thread.js` (U16) |
| `WeakRef.deref()` | one thread's job end can end another thread's keep-alive (one VM-wide version): an admitted deviation | | `D/PARITY-PLAN.md:1070-1072` |

### 3.4 Happens-before edges
- **Thread start**: everything the parent did before `new Thread` is visible to `fn` (roots are created under the lock before `Thread::create`; `ThreadObject.cpp:383-391`).
- **Thread end / `join`**: the result is written, then `Phase` is release-stored; `join` load-acquires `Phase` first (F1, `D/SPEC-api.md:136`).
- **`Lock`**: `WTF::Lock` acquire/release (F2, `:169`). **`Condition`**: enqueue before the lock is released, so no lost wake-up (F3, I9).
- **`Atomics` on properties and typed arrays**: sequentially consistent, "one atomic step each" (`:83`). `wait`/`notify`: no lost store-then-notify window (I10, F4).
- **Promise settlement across threads**: the enqueue edges "carry the needed release/acquire ordering" (`D/SPEC-ungil-history.md:5259-5262`).
- **Plain accesses to different locations**: decision 3 assumed "(A) portable model", i.e. no ordering promised between plain accesses to different
  locations beyond publication safety (rule 6); x86-64 gives more for free, "arm64 cost only" (`D/PARITY-PLAN.md:1354`).

### 3.5 The visibility rule (the one rule that costs compiled code)
SPEC-jit I21. In the form the owner's interim default tests (`D/PARITY-PLAN.md:1352`): "a read that can decide a loop's exit is performed again every
iteration". Without it `while (!o.done) {}` may never end, because the optimizer hoists the load. `main` itself makes no such promise for plain
SharedArrayBuffer reads, "which FTL code on `main` already hoists out of loops" (`D/LANDING-PLAN.md:5075-5076`). Details, cost and alternatives: section 5 P22, section 6, and appendix B section 5.

(The object-model invariants behind 3.3, with their I-numbers, a longer case table and the fences per architecture, are in appendix A, section 1.)

---

## 4. Which built-ins are thread safe, and how (question 3)

### 4.1 The default rule
`D/SPEC-ungil.md:832-840`: property storage is the object model's business; VM and global-object members are section K's; everything else,
"Multi-word C++/internal-field state INSIDE other shareable cells", follows one default: "mutations AND structure-traversing reads under the
cell's JSCellLock [...] allocate OUTSIDE, re-validate under, never allocate/park holding it". Nothing is restricted to its creating thread
except by explicit `Thread.restrict`, Wasm and FFI. The vocabulary of dispositions (`D/SPEC-ungil-audit-N7.md:16-18`): CELL-LOCK, CAS-PUBLISH,
RACY-TOLERATED, GIL-OFF TypeError, per-thread copy.

### 4.2 By builtin
| Builtin | Mechanism GIL off | Source |
|---|---|---|
| **Array methods** (C++ and builtin JS) | No per-array lock. They use the object model's concurrent accessors and must tolerate shape, length and storage changing between any two steps. Each fast path that reads `length` then touches `butterfly` was a bug class: `Object.values` "stores past its result array" (R10-5), vectorized `indexOf`/`includes` "re-reads the length" (R10-6), `butterfly()` on a word that can be segmented (PRE-7). Fast paths that cannot be made safe are off: `Array.prototype.join`'s two-pass joiner (6.4 % of UniPoker), the copy-on-write `fastToString` cache. ArrayStorage shapes are fully cell-locked, reads included. Sort scratch is per thread. | `D/AUDIT-upstream-since-rebase.md:1098`, `:1690`, `:1719`; `D/LANDING-PLAN.md:5098-5099`; `D/PARITY-PLAN.md:1059`; `ThreadAtomics.cpp:95`; K4.II.8 |
| **Map / Set** | Every mutation, iteration and `getOrInsert` under the table's cell lock. A rehash or `clear` publishes a **new** table and never writes the old one. `has/get/size` are validated lock-free reads: a seqlock word on the owner that writers bracket odd/even; the reader trusts nothing it reads (bounds-checks every index, bounds the chain), then re-checks the version, retries, then falls back to the lock. DFG/FTL intrinsics off. History: the audit row claimed coverage and "was wrong: nothing was locked" until 2026-09-02. | `D/SPEC-ungil.md:841-857`; `D/SPEC-ungil-audit-N7.md:192` |
| **WeakMap / WeakSet** | All operations cell-locked, reads too. | `D/SPEC-ungil.md:841-843` |
| **Map/Set/Array/String iterators, iterator helpers** | Internal fields; single-word claim/publish as generators; table reads under the table's lock. | N7 R3, R8 |
| **Promise** | State transitions under the promise's cell lock; reactions run on the settling thread. | `D/SPEC-ungil.md:496-501` |
| **Generators, async functions, async generators** | A single-word **resume claim**: CAS `SuspendedX -> Running` on the state field; the loser sees "already running" (TypeError, as for re-entrancy). Stores to the frame while claimed are plain. Two new intrinsics `@atomicInternalFieldClaim/Publish`, emitted in all modes. Async generators serialize their request queue (`S/runtime/JSAsyncGenerator.cpp`, `D/REVIEW-async-generator.md`). `for await` never uses the fused driver GIL off. Cost: "the generator resume's two compare-and-swaps" measured, not designed away. | `D/SPEC-ungil.md:869-881`; `D/LANDING-PLAN.md:254-269`, `:5248-5249` |
| **RegExp** | Compile state cell-locked (already so on `main`, for the compiler threads). The per-match output vector, a member of the cell on `main` ("racing resize = realloc UAF"), moved to a per-thread buffer. `lastIndex` is one racy word. Legacy statics per thread (SD19). RegExpCache already locked. Cost per call over GIL on: `exec` 1.15, `test` 1.26, `replace` 1.29, regexp `split` 1.55. | N7 RESOLVED-2, R13, R14; `D/LANDING-PLAN.md:5094-5098` |
| **Date** | The per-instance `GregorianDateTime` cache is bypassed; `m_data` CAS-published; the VM's `DateCache` per thread, time-zone change fanned out. One TSan report remains accepted ("the `DateInstance` pair"). | `D/SPEC-ungil.md:863-865`; K4.II.11; `D/LANDING-PLAN.md:4854` |
| **Typed arrays / ArrayBuffer** | Elements race as SharedArrayBuffer. Detach: length 0 seq_cst, then the contents are **quarantined until a GC stop**. Transfer = copy plus detach of the source. Shrink defers freeing the tail. Grow never moves the base: commit, then release-publish the length. `sort` works on a private copy. Open: O17-2 (store lost when `.buffer` makes a small array "waste memory"), F30 (unexplained FTL trap during transfer), PRE-13 (C++ that loads the base after the detach check). | `D/SPEC-ungil.md:882-907`; section 7.4 |
| **Proxy** | No lock. Target/handler are single words; revoke is a seq_cst null store; readers re-validate. Refused as `Atomics` receiver and `restrict` receiver. Invariant checks against a concurrently mutated target were never audited. | N7 R20; `D/PARITY-PLAN.md:1096` |
| **WeakRef / FinalizationRegistry** | Registry maps under the cell lock (upstream's own discipline). WeakRef: two racy single words; the referent is kept alive by the conservative scan. Callbacks run on the carrier. CVE audit row "MC-GC S6 (FinalizationRegistry across threads)" never re-adjudicated. | N7 R18, R19; `D/PARITY-PLAN.md:1050`, `:1092` |
| **Intl** | Members that mutate after construction under the cell lock (lazy strings computed outside, published under it); break iterators locked; ICU handles frozen at construction used only through calls verified `const`, else cloned per use; `IntlCache` under a leaf lock. "done well"; but ICU is invisible to TSan. The unimplemented draft would have serialized all Intl behind one lock. | N7 RESOLVED-6; K4.III.3; `D/PARITY-PLAN.md:1093`; `D/SPEC-nativeaffinity.md:218-225` |
| **JSON** | No shared state but per-thread atom caches (`jsonAtomStringCache`); parse builds fresh objects; stringify walks with the concurrent accessors. Cost: the transition map and property walk under the structure's lock are 42 % of json-parse-inspector's excess and 80 % of JSON.stringify's. | K4.II.4; `D/LANDING-PLAN.md:5112-5113` |
| **Error `.stack`** | Lazy materialization under the cell lock / a claim: "One thread does the work now and the others wait". Before: crash on every run. Open: O17-4. VM members `m_onComputeErrorInfoJSValue`, `m_nativeStackTraceOfLastThrow` "in no audit". | N7 R15; `T/semantics/error-stack-first-access-race.js`; `D/PARITY-PLAN.md:1104` |
| **Functions** | Rare data lazily published; lazy `name`/`length`/`prototype` reification raced (tests `T/semantics/lazy-*-first-use-race.js`); allocation profiles filled under the cell lock. Bound function: immutable plus idempotent lazy words. | N7 R6, R21; `D/LANDING-PLAN.md:120` |
| **arguments objects** | Lazy override storage CAS-published; "overrode" flags release-stored **after** the property puts, else a reader misses `length`/`callee` (lost-property class). | N7 RESOLVED-3, -4 |
| **Strings** | Rope resolution lock-free: compute into a fresh buffer, publish by one release-CAS of the `fiber0`/flags word; losers discard. Atomization through the shared sharded table. `StringImpl::m_hashAndFlags` became atomic. | `D/SPEC-ungil.md:858-862`; `D/SPEC-vmstate.md:197-205` |
| **Symbols** | `SymbolRegistry` gains one lock. | `D/SPEC-ungil.md:718-724` |
| **Module records** | Maps under the cell lock; the resolution cache was an unlocked HashMap ("cross-thread UAF"). Evaluation: open. | N7 RESOLVED-1, R16 |
| **for-in / `Object.keys` caches** (`StructureRareData`) | Installed under `Structure::m_lock`, each word release-published last. Enumerator validity against a published-but-not-fired transition: never examined, rated H. | N7 RESOLVED-5; `D/PARITY-PLAN.md:1101` |
| **Lazy global-object properties** (`LazyProperty`, `LazyClassStructure`) | Atomic lazy publication: acquire-load fast path; a CAS records the owner; the winner initializes without a lock and release-stores; foreign threads wait in a way that can park for a stop. Plain "if null, create, store" members have no protocol (fifteen promise executables). | `D/SPEC-ungil.md:804-811`; `D/PARITY-PLAN.md:880-885`, `:1086` |

### 4.3 What was never covered
"Host function bodies at large (1,180 host functions, 167 custom accessors, 702 JIT operations, 189 slow paths)": coverage "is whatever the mirror
harness, the corpus and the fifth round's directory sweep reached"; risk rated **H** (`D/PARITY-PLAN.md:1085`). The draft answer
(`D/SPEC-nativeaffinity.md:32-55`, "Jarred's proposal") is a ratchet worth keeping as an idea: every native function carries a *concurrent-ok* bit;
clear means calls serialize on one per-VM "native serial lock" that is released around re-entry into JS and around blocking; a small audited hot core
(object, array, string, Math, JSON, Atomics) ships concurrent; bits flip only with TSan and fuzzer evidence. Its cheap form: "default refusal
(TypeError) on a spawned thread for natives not on an audited list" (`D/PARITY-PLAN.md:1000`).

---

## 5. The hard problems: mechanism, cost, and where the cause lies (questions 4 and 5)

Cause classes: **J** = JIT / speculation / profiling / code patching (disappears in an engine that generates no code at run time and speculates on nothing that can be
invalidated); **O** = the object model (stays for ordinary objects and arrays whoever accesses them: compiled code or C++); **R** = the C++ runtime and heap (stays);
**M** = inherent to any runtime with several mutators and one collector (stays). The class column is this report's judgement, not the documents'. Details, more numbers
and every citation behind a row are in appendices A (object model), B (JIT, stops, polls), C (heap), D (performance). All numbers are Release, Linux x86-64; nothing was ever run on arm64.

### 5.1 Object model

| # | Problem | Mechanism chosen (final tree, GIL off) | Cost | Class |
|---|---|---|---|---|
| P1 | **Butterfly growth is copy-then-swing**: a store by another thread into the old copy vanishes ("time travel") | **Tagged butterfly word**: "bit 63=SW; 62..48=TID (installer/owner); 47..0=payload" (`D/SPEC-objectmodel.md:15`). Every object is born with its allocator's TID. Reads: test for segmented, mask, no TID check. Writes to an existing out-of-line slot: owner, or SW ("shared write") already set, else set SW by a 128-bit CAS of {header, word}, then store. The owner's growth CASes the word expecting `(me, SW=0)`, so it fails if a foreign writer came first. SW is never cleared. A published flat `vectorLength` is immutable, so stale storage "can only be stale, never short", and is kept alive by the conservative scan. Inline (in-cell) slots need nothing. | read +1 instruction; out-of-line write 9 instructions against 2 (`D/DESIGN-PROPOSALS.md:6209-6222`). Tags as a whole: 1.05 in instructions, 1.4 % of cycles (3.5 % at most) (`D/PARITY-PLAN.md:210-212`). One growth event 960 C++ instructions against 360, "re-dispatch around the copy, not the copy" (`D/LANDING-PLAN.md:5086-5090`). | **O** |
| P2 | Growth once the storage is shared | **Segmented butterfly**: an immutable spine of pointers to 32-byte fragments that never move; conversion is zero-copy (the spine aliases slices of the live flat butterfly). Correct, and nearly unusable by fast code: the project kept adding ways not to segment (`StayFlatShared`). | 243.7 against 19.0 instructions per element read; DFG/FTL exit on a segmented word; intrinsic estimate 5-7 instructions per access and no hoisted base (`D/DESIGN-PROPOSALS.md:2022`, `:2175-2178`). A lazily reified builtin property touched first by a worker made a 16-thread phase 3x slower (`D/MAP-BIMODAL-EVIDENCE.md:255-268`). | layout **O**; the 13x is **J** |
| P3 | **Structure transition = two words** (StructureID in the header, slot or butterfly) must change as one against another writer and against lock-free readers | **Claim first**: CAS StructureID `S -> nuked(S)`; then value; then word; then the new StructureID. A loser RESTARTs the whole operation from dispatch. Five legs from "owner, no sync" to "cell lock + claim + DCAS". Readers that see the new StructureID see the value (I9). Cells in precise allocations cannot take a 16-byte CAS (address 8 mod 16): always locked. Header bytes are shared with the GC and the lock bits, so each header CAS must merge them. | one locked instruction per added property; 70 % of delta-blue's locked loads, about 7 % of its time (`D/PARITY-PLAN.md:402-409`). Locked instructions overall: 0.48 per 1,000 against 0.25 on `main` = 4.7 % of `main`'s cycles, moved by no experiment (`:217-219`). | **O** (avoidable for objects that provably have not escaped: R-1, an AOT-friendly analysis) |
| P4 | **Ownership keyed on shared metadata** | First design keyed thread-locality on the Structure: two threads building the same shapes collapsed. Fixed by instance-keyed ownership (TID in the unused word at birth). | `{}` + 4 adds: 8 / 1,920 / 2,100 / 4,206 ms at 1/2/4/8 threads before, 6.0 / 6.6 / 8.2 / 9.6 after (`D/PERF-RESULTS.md:369-376`) | **O** |
| P5 | **Structure-only transitions** (seal, freeze, preventExtensions, setPrototype, brand, dictionary conversion): read structure, plan, store | One helper `publishStructureOnlyTransitionConcurrently`: publish only if the planning-time structure and its table are unchanged, under the cell lock; else re-plan. `setPrototypeOf`'s cycle walk under one process-wide lock. Twelve call sites were found broken late. | not isolated | **O/R** |
| P6 | **Shared Structure metadata**: transition table, property table | `Structure::m_lock`, later a two-sided seqlock for table probes; replaced index vectors quarantined. The per-object lock cannot cover per-Structure state (found only in the fourth design review). The open I-2 defect is here. | 3.3 % of time over 18 tests; json-stringify 26 %; json-parse 42 % of its excess (`D/PARITY-PLAN.md:269-272`) | **O**; "hotter without inline caches" |
| P7 | **Delete and slot reuse** | Cell-locked; `undefined` release-stored before the table edit; the offset is quarantined until a collection of the owning heap has stopped the world. Needs I34: no poll, allocation or park between obtaining an offset and using it. | not isolated | **O** |
| P8 | **A slot changes kind** (data <-> accessor) under an unchanged offset | one stop-the-world per redefinition (L4-K). Still open against getter caches (O17-1). | rare | **O** |
| P9 | **Dictionaries** | writers: cell lock + structure lock, value before name. Readers are *not* locked in the tree, whatever the spec says. Flatten = transition to a fresh clone, never renumber under one StructureID. ICs refuse dictionaries. | get 5.9x, put 7.5x (`D/DESIGN-PROPOSALS.md:110`) | protocol **O**, the cliff **J** |
| P10 | **Indexing-shape changes; Double arrays**: lanes are untyped 8 bytes whose meaning is in another word; a reader can pair an old shape check with new storage and read a raw double as a pointer | Never solved. Int32->Contiguous in place, readers verify each Int32 lane (I41). **Int32->Double never happens** (served as Contiguous). Copies of Double sources are boxed. Double->Contiguous = one stop per array. Designed, not built (C-D3): encoding-changing transitions are copies and a raw-double bit travels in the butterfly word, "the bit and the base arrive in one load"; needs an audit of 140 C++ lane decoders. | 2.9 % of time; navier-stokes 82 instructions per kernel iteration against 31; sha256 0.40, ML 0.56 of GIL on; earlier 1.5 M stops in one aes run (`D/LANDING-PLAN.md:5077-5085`) | **O**, amplified by **J** |
| P11 | `{base, length}` from different moments | every bound is `min(publicLength, vectorLength of the storage actually loaded)` ("section 39"); length raises are CAS-max, lowers plain | 2-3 instructions per element access; CAS-max 1.06-1.11x on push loops | **O** + compiler |
| P12 | **Copy-on-write arrays** | any write first materializes a private flat butterfly: cell lock + claim + DCAS even for the owner | four stores into a literal 798 instructions against 426 on `main` (`D/DESIGN-PROPOSALS.md:1982-1990`) | **O/R** |
| P13 | **ArrayStorage / sparse maps** | never segments; every runtime access cell-locked, reads included (a sparse-map rehash frees the table under a reader) | `shift` drain O(n): 27.3 ms against 1.5 | **O** |
| P14 | **TID space** | 15 bits; rebias (restamp dead TIDs to 0) inside a full collection; RangeError at exhaustion | - | **O** (consequence of P1's encoding) |

### 5.2 Compiled code, caches, stops

| # | Problem | Mechanism | Cost | Class |
|---|---|---|---|---|
| P15 | **Inline caches** that patch code or publish several words | No patching outside a stop (I2). Handler (data) ICs in every tier; a chain node is frozen at publication; `{structureID, offset}` in **one aligned word**; "no multi-store seqlocks in JIT-readable state". LLInt caches survive only if they fit one word, else disabled. Retired nodes freed by GC epoch, legal only because no poll sits inside an IC window (I16). | FTL handler ICs: WSL +4 to +9 %; disabled LLInt caches +627 to +690 instructions per operation; no transition cache at all (rounds 1-4): 40x-1400x on object creation (`D/DESIGN-PROPOSALS.md:104`, `:137`; `D/SPEC-jit-history.md:455-460`) | patching **J**; *publication of mutable per-site data to lock-free readers* **M**: stays if AOT code has any cache |
| P16 | **Call linking** | one pointer to an immutable `CallLinkRecord`; relink = new record | +5 instructions per non-inlined call | **J** in form |
| P17 | **Profiling** | "tolerate, don't synchronize"; "profiles select, guards validate" | shared counters: "Baseline 50 -> 260 cycles per back-edge from one thread to two" (`D/DESIGN-PROPOSALS.md:149`) | **J**; lesson **M**: a word written per execution by all threads costs about 5x |
| P18 | **Watchpoints**: other threads are inside code that dropped the check | stop the world, fire, jettison, resume, synchronously; concurrent fires coalesce into one stop; a fire under a lock is deferred; deferred fires open "claim" windows in which "invalidated" is not yet "fired". The data-only class was defined and never used. The two new per-Structure "thread-local" sets, the heart of THREAD.md's "making the checks free", were progressively emptied; full elision of the write check was **unsound** (shared code runs on every thread). | stops themselves cheap: about 36 per run, 1.6 ms in all (`D/STW-WATCHDOG-CLOSURE.md:158-166`). Indirect cost large: one fire knocks every parked thread to Baseline. | **J**. What stays (**R**): sets that C++ caches consult (enumerators, species/iterator protocol), `haveABadTime` |
| P19 | **Code invalidation, OSR exit, tier-up with N threads** | jettison only world-stopped; an invalidation point after each poll; "heap-fact epoch" makes parked threads jettison their own frames; exit thresholds x (1 + live threads) | a stale loop: 2 exits on `main`, 903 GIL off; 1.5 points of score for one parked thread; every bimodality of the project | **J** |
| P20 | **Stop-the-world requests** (non-GC) | requester conducts on its own stack; unit = entered thread; predicate "every other entered thread has released heap access or is not entered"; sound only with a **re-acquisition gate**, a Dekker pair: "ALL seq_cst; acq/rel UNSOUND"; entry during a stop parks; serialized with GC on one conductor lock; a 30 s watchdog aborts a stop that does not complete | 0.04 ms per running mutator per stop (`D/DESIGN-PROPOSALS.md:5167-5172`) | **M** for GC; the non-GC requesters shrink to object-model stops (P8, P10), `haveABadTime`, heap walks, debugger |
| P21 | **Polls / safepoints** | cooperative only; signals off. `op_check_traps` at loop back edges and function entries: load, test, branch on a trap word (the VM-level one in the tree). Every allocation slow path, lock spin and blocking call is also a park site. Poll-free C++ must be bounded (Yarr's match limit). | 1.06 in instructions; tight loops 1.65-2.0x; **in time under 1 %** once polls at inlined callee entries are gone (136 of delta-blue's 182 in-loop polls were those) (`D/PARITY-PLAN.md:210-212`) | **M** |
| P22 | **The visibility rule** (I21) | a poll is modelled as writing the value heaps that the loop's branch conditions read, so "a read that can decide control flow [...] is performed again after every poll, and a read that only feeds data [...] may be hoisted" (`D/SPEC-jit.md:214`) | before refinement 40 % of delta-blue's gap; after D-B1..B4 0.8 % of time (`D/PARITY-PLAN.md:1352`) | **M** + compiler: "about compiler motion, not hardware ordering" |
| P23 | **Facts held across a park site that a stop may rewrite** (shape, butterfly layout, property kind) | allowed to cross polls only because a stop that rewrites them forces an OSR exit at the poll | - | **M**, and **harder without OSR exit**: either each park site clobbers them, or the poll's slow path re-enters through a "refresh pre-header" (D-B5), which needs no code invalidation |

### 5.3 Heap

| # | Problem | Mechanism | Cost | Class |
|---|---|---|---|---|
| P24 | **One heap, N mutators** | one server `JSC::Heap` (the main VM's), one `GCClient::Heap` per thread; a per-thread table of `LocalAllocator`s over shared block directories ("TLC"); slow path under a lock that ended as a reader/writer facade with per-directory stripes; counters relaxed atomics; DeferGC depth per client; Eden allowance grows with the number of *allocating* threads | single thread: from +122 % (June) to "allocation and collector 1.4 %" of cycles; per thread about one nursery (35-70 MB) (`D/PARITY-PLAN.md:277`; `D/DESIGN-PROPOSALS.md:5142-5148`) | **R** |
| P25 | **How compiled code finds its allocator** | constant slot index + per-thread table: 6 instructions. The first phase (null allocator, always slow path) cost 46.6 M thunk runs in one benchmark. "A 'real TLAB' [...] targets the wrong lever": refills were 0.33 % of allocations; the cost was the lookup. | 18 of the 50 extra instructions of a constructor loop | **R**; AOT needs it from day one |
| P26 | **Who collects, and the handshake** | the requester conducts. Per-client access byte; AHA = seq_cst CAS, then seq_cst load of stop-pending, then mandatory revert. "no thread reads or writes the shared JS heap except holding some client's heap access"; every indefinitely blocking primitive is bracketed. | stops 3-5 % of a parallel leg | **M** |
| P27 | **Concurrent marking** | barrier's cellState CAS was already N-safe; the one unsound piece was the append to a single mutator mark stack. Final: at most one concurrent window per cycle; the conductor runs no JS for the whole cycle; assist, incremental sweep, collector-thread conduct off | splay 0.67 (out of JS 265 ms against 69); forced fencing cost 1.4-2.7x on barrier-heavy loops until fixed | **R** |
| P28 | **Conservative scan of N stacks** | upstream's `MachineThreads` suspend-and-copy already handled N threads; parked threads later publish their own register snapshot. Per-thread C-loop stacks were never done. | 2 % of mutator time at 16 threads before the snapshots | **R** |
| P29 | **Destructors, finalizers, weak handles** | run on whichever thread sweeps; `addFinalizer` lambdas queued to after the stop; sweeps that run beside mutators skip "weak-bearing" blocks (every Structure block) | 478 MB against 88 MB on one workload, open; three successive bugs | **R** + embedder |
| P30 | **Heap's own containers** (mark-list set, Strong set, protect set, weak tables) | shards or leaf locks; a "destructor-leaf" lock class | observed UAF before | **R** |
| P31 | **Heap walks** (`HeapIterationScope`, snapshots, statistics) | `runWithOtherClientsStopped` | - | **R** |
| P32 | thread-local nursery | **rejected**: "any plain store publishes an object", needs a publication barrier on each pointer store | - | - |

### 5.4 VM state and runtime

| # | Problem | Mechanism | Cost | Class |
|---|---|---|---|---|
| P33 | **VM-wide execution state** | `VMLite`, one per thread, found through `thread_local` (no reserved register): `topCallFrame`, `topEntryFrame`, exception and unwind words, stack bookkeeping, `VMTraps`, entry scope, scratch buffers, microtask queue, RegExp stack and match buffers, GC client pointer, allocator table (`D/SPEC-vmstate.md:376-433`; `D/SPEC-ungil.md:65-147`). Registry of lites = the set of entered threads and a GC root source. | the out-of-line selector "executes 0.97 % of all instructions" GIL off; 19,000 expansion sites (`D/LANDING-PLAN.md:4532-4535`, `:4364-4365`) | **R** |
| P34 | **Exception state** | per lite. `RETURN_IF_EXCEPTION` polls one VM-level word that carries a count of threads with a pending exception; the slow path reads the thread's own word (`D/SPEC-ungil.md:198-218`) | 23 such tests in one regexp `split` | **R** |
| P35 | **Stack limits** | per lite, set at entry; Parser and BytecodeGenerator keep a copy | - | **R** |
| P36 | **Termination, watchdog** | bit fan-out to every lite under the registry lock + the VM word; 10 ms quanta at every park site; later one-thread deadlines; `cancelTermination` must withdraw from both words | - | **M** |
| P37 | **Microtask queues, DeferredWorkTimer, rejection tracker** | section 2 | generators/promises: async-fs 15.8 % | **R** |
| P38 | **Per-op VM caches** (numeric strings, last atomized identifier, JSON/key atom caches, split/replace caches, sort scratch, BigInt divisor, recursion check, date cache, `Math.random`, HasOwnProperty and megamorphic caches) | per-thread duplicates (K4 section II, 19 rows); shared keyed caches get leaf locks (K4 section III, 18 rows: code cache, Intl cache, symbol and atom-to-JSString weak maps, compact variable map ...) | megamorphic-access 1.77 -> 1.13 when made per thread | **R** |
| P39 | **Lazy initialization** | atomic lazy publication with an owner CAS and a park-capable wait (section 4.2, last row) | - | **R** |
| P40 | **Atom string table** | process-global, 128 shards each with a lock. The hard part was the **refcount life cycle**: "refcount 0 is final", `tryRefAtom` under the shard lock, `removeDeadAtom` by pointer identity; `m_hashAndFlags` atomic; pre-latch atoms migrated (`D/SPEC-vmstate.md:90-250`) | every `StringImpl::deref` a locked instruction: map-set-get 1.19, regexp-exec 1.13 at identical instruction counts; strings 1.9 % of time | **R** |
| P41 | **Ropes** | lock-free: resolve into a fresh buffer, one release-CAS of the `fiber0`/flags word, losers discard (`D/SPEC-ungil.md:858-862`) | +0.5 % (7th round) | **R/O** |
| P42 | **StructureID allocation** | IDs are addresses in one reservation, no table; a lock only around Structure cell allocation; mimalloc's thread-affine heaps were unsafe for block hand-out (`D/SPEC-vmstate.md:17-18`, `:252-260`) | - | **R** |
| P43 | **Sampling profiler** | main thread only (SD18); an exiting thread must unbind (a hang found by the amplifier) | - | **R** |
| P44 | **WebAssembly** | GIL off `useWasm` is forced off for the whole process; GIL on refused on spawned threads; the refusal leaked through C++ call paths | 117 suite results, 47 Bun cases | scope decision |
| P45 | **FFI** | carrier only. Lifting needs "a per-thread string arena and UTF-8 cache, a per-thread callback return buffer, a rule for which thread a callback runs on, heap-access release around long native calls" (`D/LANDING-PLAN.md:5259-5262`) | FFI call 541 against 118 instructions GIL off | **R** + embedder |
| P46 | **API lock** | `JSLock` is a real mutex only among embedder threads; for a spawned thread `JSLockHolder` is an entry token; about 60 consumers of "is the API lock held" had to be classified; `DropAllLocks` on a spawned thread is a heap-access bracket (`D/SPEC-ungil.md:632-674`) | - | **R** |
| P47 | **Lock order** | one merged process table, heap access treated as the outermost lock (appendix C section 10) | deadlocks were the commonest late failure | **M** |
| P48 | **Host functions at large, embedder natives** | nothing (section 4.3) | unknown risk | **R** |

### 5.5 Summary of question 5
- **Disappears with the JIT**: code patching and its fences; jettison, OSR exit, invalidation points; tier-up, profiles, execution counters and all bimodal behaviour; Class-A watchpoint stops and deferred claims; the
  two per-Structure thread-local sets and the elision they licensed; retirement of JIT artifacts; compiler-thread suspension; baked per-thread immediates; scratch buffers as roots.
- **Stays, object model**: P1-P14. In one list (appendix A, end of section 3+4): copy-growth against a foreign store; header and word must change together; storage-then-meaning consistency for readers (shape against lanes,
  structure against offset, data against accessor, length against base); slot reuse after delete; shared Structure tables; old storage must stay readable and alive; header bytes shared with GC and lock.
- **Stays, runtime and heap**: P24-P48, nearly unchanged.
- **Stays, any compiled code**: polls, the visibility rule, poll-free protocol windows, per-thread allocator lookup, "one snapshot" bounds.
- **Gets worse without a JIT**: (1) checks that the JIT elided under watchpoints are always paid; (2) there is no OSR exit to fall back on, so a segmented or otherwise unexpected word needs a real inline or out-of-line arm from day one,
  and facts cannot cross park sites for free (P23); (3) whatever inline caches were hiding, above all property-table and transition-table lookups under locks (P6).

---

## 6. Where the GIL-off time goes (question 6)

### 6.1 A correction to the question
"1.06 x 1.05 x 1.16" is **not** a decomposition of the 0.76 score. It factors **GIL off / GIL on = 1.30, in whole-test instructions, over fifteen JetStream 2 tests** (`D/DESIGN-PROPOSALS.md:120-123`; `D/LANDING-PLAN.md:5057-5062`),
measured by option ablation on one binary: polls = GIL on + `--usePollingTraps=1`; tag = GIL on + `--useJSThreadsSingleOwnerWithGIL=0`. 0.76 is the JetStream 2 *score* of GIL off against `main` (265.9 / 347.3, tenth round,
`D/LANDING-PLAN.md:3552`; 0.758 in the eleventh session, 0.743 with one parked thread, `D/PARITY-PLAN.md:158-165`). JetStream never spawns a thread: every "GIL off" score is a process that has the mode on and has not used it.

The last factor, 1.16, is "what only a GIL-off process does": "the poll's heap writes (the visibility rule), the concurrency protocols in generated code (claim compare-and-swap in transitions, compare-and-swap length raises,
seqlock-validated Map reads, per-thread allocator resolution, the second bounds check against the vector length), operations that are inline GIL on and calls GIL off (MakeAtomString, Map/Set mutation and iteration, WeakMap reads, array
join, generators, promises), and the `gilOff`-keyed C++ bodies (RegExp, per-thread scratch, locked structure tables)" (`D/DESIGN-PROPOSALS.md:6162-6166`).

### 6.2 The decomposition in time (the one to trust): `D/PARITY-PLAN.md:176-222`
Main-thread cycles against `main`, 36 tests, an experiment build with nineteen switches, cumulative:

| configuration | instructions | **cycles** | locked loads / 1,000 instr |
|---|---|---|---|
| flag off | 1.015 | 1.011 | 0.29 |
| GIL on | 1.037 | **1.050** | 0.34 |
| GIL on + polls + tagged words ("the floor") | 1.163 | **1.132** | 0.35 |
| **GIL off** | 1.349 | **1.291** | 0.48 |
| + design set (poll placement D-B1..B4, RegExp entry, one-pass append, lone conductor, MakeAtomString, join, fused predicate, fresh owner) | 1.289 | 1.251 | 0.47 |
| + Double family as GIL on, no lane verification, no second bound | 1.212 | 1.216 | 0.50 |
| + every FTL poll writes nothing (visibility rule gone) | 1.198 | 1.206 | |
| + no poll in optimized code | 1.172 | 1.195 | |
| + untagged words everywhere | 1.123 | **1.178** | |

Its reading: "**Cycles, not instructions** [...] polls, masks and compares retire beside the useful work"; "Every remaining poll gone: 0.9 % of cycles. Every poll writing nothing: 0.8 %. Untagged words everywhere: 1.4 %"; design set 3.4 %; Double family 2.9 %;
"Twelve points, a third of the original distance, are in none of the switches"; locked instructions 4.7 % of `main`'s cycles, "No switch moves it"; "Both flag-on modes pay 5 % of time over `main`". "A score of 0.90 is a main-thread cycle ratio of 1.09" (`:145-147`).
"No set of changes known today guarantees 0.90"; central estimate 0.87 with threads alive (`:20-23`).

What is left after the prototypes, by family, percent of time over the 18 worst tests (`:239-285`): generated code 10.9 (of which polls and tags about 3), property tables and structures 3.3, strings 1.9, object model in C++ 1.6, allocation and collector 1.4,
Map/Set/WeakMap 1.3, promises and generators 1.1, RegExp 0.9, compiler on the main thread 0.6, calls and entry 0.4.

### 6.3 Per tier: the nearest thing to "GIL off without a JIT"
- Score ratio GIL off / GIL on with tiers removed (seventh round's ledger, the 15 worst tests): full 0.65, no FTL 0.69, Baseline only 0.84, **LLInt only 0.89** (geometric means computed by the sub-report from `D/PERF-RESULTS.md:612-634`). "the rest keep most of their deficit down to
  baseline-only and much of it in LLInt-only, i.e. it is in C++ paths the interpreter also reaches" (`:640-644`). An old tree; no later measurement exists. GIL off has never run in a build without a JIT: it "is further forced back to GIL on [...] on every non-Linux, CLoop or 32-bit build" (`D/FLAG-OFF-LANDING.md:1042-1043`).
- Instructions per loop iteration, GIL on -> GIL off, FTL column (`D/DESIGN-PROPOSALS.md:6176-6200`): empty loop 6.7 -> 8.2; array read 11.8 -> 18.5; array write 10.5 -> 13.8; push 35.7 -> 50.9; inline read 18.5 -> 22.1; out-of-line read 20.1 -> 24.0; out-of-line write 17.4 -> 29.3; seven adds 104.9 -> 136.3; `new` array 34.4 -> 57.6; `Map.get` 43.0 -> 80.3 (DFG 59.9 -> 308.9).
- `for (i < a.length) s += a[i]`: 8.4 -> 18.3: "the poll 2; `a.length` re-read 1 (visibility rule); bounds check against the re-read length 2; bounds check against the hoisted storage's vectorLength 2-3; the Int32 lane verification 3" (`:721`).

### 6.4 Which parts would exist in AOT-compiled code (this report's judgement; reasoning per row in appendix D, section A3)
| Part | In AOT code? |
|---|---|
| poll as instructions | **Yes**, but perhaps not as an increment: AOT text cannot be patched, so single-threaded AOT code must already have some way to be interrupted. Placement is the compiler's own choice: one per loop header and per non-inlined entry gives D-B1/D-B3 for free. |
| visibility rule | **Yes** for any backend that hoists loads. If heap loads are plain loads to a generic optimizer and the poll's slow path is not an opaque call, the default is silently "full release" (c2); if it is an opaque call in the loop, everything escaped is clobbered, which is stricter and dearer than the rule. The contract must be chosen first. |
| butterfly tag, write predicate, owner check | **Yes, probably dearer**: the FTL numbers include elision under watchpoints. |
| segmented arm | **Yes, and it must be a real code path**, not an exit. |
| second bound, lane verification, Double rule | **Yes**, wherever a shape or storage fact is kept across a point where another thread can act; with true parallelism a check-then-load pair is never atomic. |
| claim CAS per add, CAS-max length | **Yes**; escape analysis can remove the claim on fresh objects. |
| per-thread allocator resolution | **Yes**; a pinned register or TLS base shortens it. |
| handler ICs, call records, unpatched slow paths, math ICs, LLInt caches | Not as an increment (AOT code never patches); but any multi-word data cache has the publication problem. |
| tier-up, exits, thread multiplier, shared profiles | **No.** Removes every bimodality and the 1.5 points a live thread cost. |
| Map/Set/WeakMap, structure tables, strings' refcounts, RegExp per-thread context, generators/promises, per-thread VM state, VM caches | **Yes, in full** (C++), and nearer to the DFG/Baseline rows than to the FTL row, since AOT code calls C++ where the FTL had intrinsics. |
| collector (conductor sits out the cycle, retention, weak-bearing blocks), barrier fencing | **Yes, in full.** |
| arm64 load ordering | **Yes** (acquire on guard loads). |

### 6.5 Scaling, in one paragraph
Parallel-self suite, tenth round, speed-up at 2/4/8 threads: raytrace-like 1.63/3.33/4.84, splay-like 1.95/3.59/5.74, map-heavy 1.84/3.26/4.30, string-heavy 1.49/0.93/3.01 (bimodal), richards-like 0.54/1.15/1.71 (`D/PERF-RESULTS.md:1395-1401`). The failures were tier-up recovery (J), not the collector:
in the big benchmark "~2/3 of the W=16 gap is **single-thread throughput**, not scaling" (`D/FLAT-GAP-EVIDENCE.md:21-24`), and stops are 3-5 % of a parallel leg. Non-JIT causes worth remembering: refcount bouncing on one `RefPtr` returned by value (14.4 % of CPU at 16 threads), a fresh Structure per `Lock`/`Thread` instance, a shared report word (8.2 % of cycles),
one nursery budget for k threads, atom-table and structure creation under contention, `Lock.prototype.hold(fn)` being a native between caller and closure (15.9 % self time at 16 threads). Appendix D, section A4.

### 6.6 What the *unused* feature cost (a lesson for packaging)
658 source files, +87,000 lines, "No build flag compiles the feature out". Flag off began at 0.966 of `main` with text +15.6 %; six rounds (12 to 17) were spent to reach cycles 1.0035, text +6.7 % (thin LTO), start-up RSS +868 KB (the one target missed). The cure was to compile functions once per mode and keep the threaded copies in a
section of their own. An AOT-only feature can avoid this whole class if the threaded runtime is selected at build time of the executable.

---

## 7. Decisions of the project owner, and the open defects (question 7)

A caution first. The documents distinguish **rulings the owner made** from **decisions put to the owner with a recommendation**. Most of the
numbered "decisions" (nineteen in `D/DESIGN-PROPOSALS.md:265-423`, three more in `D/PARITY-PLAN.md:1371-1379`, ten in
`D/FLAG-OFF-LANDING.md:1349-1415`) are of the second kind: I found no text that records the owner's answer to them. `PARITY-PLAN` says so itself:
"A plan cannot wait for nineteen answers; it assumes the recommendation" (`:1346-1348`).

### 7.1 Rulings that are recorded as made
| Ruling | Text | Where |
|---|---|---|
| **Order of goals** (2026-09-17) | "first the branch built with the flag not set equals `main` in safety and speed, so that it can be merged and shipped as an experimental feature that is off by default"; the flag-off work packages "come before everything below" | `D/LANDING-PLAN.md:5037-5040` |
| **The contract** | any object shared, "sound semantics, no crashes from races, and ~zero perf cost for single-threaded code"; "Memory model = SharedArrayBuffer's; the engine guarantees object-model invariants under races [...] not race-free programs" | `THREAD.md:3-5` |
| **GIL first** | "ship a GIL'd `Thread()` first (semantic oracle + makes the full test corpus runnable), then remove the GIL layer-by-layer" | `THREAD.md:23` |
| **`bun:ffi` on spawned threads** | "Out of scope by standing decision": "stays refused" | `D/LANDING-PLAN.md:5258-5264` |
| **WebAssembly on spawned threads** | out of scope by standing decision; refused in v1 | `D/LANDING-PLAN.md:5265`; `D/SPEC-ungil.md:726` |
| **Fuzzers** | out of scope for the landing rounds ("none ran in the ninth, tenth or this session"), "with the instruction that the documents list what each would need" | `D/LANDING-PLAN.md:5266-5267`; `D/FLAG-OFF-LANDING.md:1399-1400` |
| **arm64 and non-Linux hardware** | out of scope for the landing rounds; reading pass only | `D/LANDING-PLAN.md:5268-5269` |
| **GIL off is not supported** | needs `useThreadGILOffUnsafe`; the constant `gilRemovalPreconditionsMetValue` stays `false` until the safety program is discharged | `D/PARITY-PLAN.md:955-962` |
| **One VM with threads per process** | the first VM constructed wins; `new Thread` in another VM throws RangeError; a spawned thread never enters another VM (fail-stop) | `D/SPEC-ungil.md:42-56`, `:696-701` |
| **No termination of one thread from JS; no detach/cancel** | "no detach/cancel"; "NO Thread.prototype.terminate" | `D/SPEC-api.md:54`; `D/SPEC-ungil.md:164-168` |
| **Reactions run on the settling thread** (SD10) | declared a hard precondition that the embedder had to sign off | `D/SPEC-ungil.md:702-704` |
| **Method rules of the rounds** | "Every change went into the specifications first, then C++, then every tier, with a JSTests/threads test that fails or shows the old count before and passes after" | `D/LANDING-PLAN.md:3549-3551` |
| | "A test that only fails in Debug is still a bug: an assertion that a race can trip means the code does not hold the invariant it asserts." | `D/LANDING-PLAN.md:61-64` |
| | comparisons only against `main` "built with the same flags on the same machine in the same session" | `D/FLAG-OFF-LANDING.md:120-122` |
| **Threaded modes may pay for flag off** (taken by the working sessions on the strength of the order of goals, not attributed to the owner: the fifteenth round's form was "decided without asking", `D/LANDING-PLAN.md:4537-4538`) | the seventeenth round took 245,616 bytes out of every process at the price of 0.7 % more instructions GIL off: "That was decided for the configuration without the flag" | `D/LANDING-PLAN.md:4981-4985` |


**Explicitly not made**: the visibility ruling. "That ruling belongs to the owner of the threads API and has not been made; this round does not make it" (`D/SPEC-jit-history.md:1444-1446`).

### 7.2 What may never be traded (as the documents state it)
1. **Memory safety for racy programs** (S1). "the performance plan may relax *when* a write is seen, never *what* is seen" (`D/PARITY-PLAN.md:932-933`).
2. **A process-wide fast mode whose failure mode is memory unsafety is refused.** Running untagged until the first spawn "means classifying about
   980 `gilOff` tests [...] and keeping two copies of the object model alive GIL off, with memory unsafety as the failure mode of a missed site. Not
   recommended" (`D/LANDING-PLAN.md:5115-5118`).
3. **Correctness must not depend on which tier runs a loop**: the argument against full release of the visibility rule (`D/DESIGN-PROPOSALS.md:285-292`).
   `PARITY-PLAN` then measured the remaining price at 0.8 % of time once the cheap refinements are in, and assumed the promise stays (`:1352`).
4. **Soundness over surface**: `Thread.restrict` refuses what it cannot enforce ("an object that cannot be enforced is never accepted", `ThreadObject.cpp:753-754`); property
   `Atomics` refuse receivers on which "one atomic step" cannot be kept.
5. **No heap state that a sequential program cannot create** ("THREAD.md's indistinguishable-heap requirement", `D/INTEGRATE-api.md:989-994`, `:1045-1048`).
6. **A refusal is a JS exception, not an abort**; every added fail-stop must be an internal invariant or a documented embedder misuse, never reachable from
   script (S2, `D/PARITY-PLAN.md:834-838`).

### 7.3 The decisions that shape the promise to programs (put to the owner; recommendation in brackets)
1. Visibility rule: (c0) keep "every read that can decide a loop's exit is performed again every iteration" plus four free refinements; (c1) bounded staleness with a
   1 ms tick; (c2) full release as for SharedArrayBuffer. [DESIGN-PROPOSALS: c0 now, c1 as ruling; PARITY-PLAN after measuring: c0 only.]
2. `Atomics` on ordinary objects behind an own option that follows the flag. [yes] "under (c2) it is the only lock-free way to signal through an object".
3. Racy plain accesses to **different** locations: (A) unordered, the portable model, engine safety by acquire guard loads only; (B) TSO everywhere. [A]
4. x86-64-only rules allowed if the portable fallback is exercised by a test on x86-64. [yes]
5. arm64 GIL off not a landing requirement; refused at option validation. [yes]
9. Speed of a GIL-off process before its first spawn is not a goal. [A: no; PARITY-PLAN: a single-mutator phase for optimized code only, "C++ never has two copies of the object model"]
11. Module evaluation: one evaluation lock. [yes]
F1. The flag must be an act of the program's author (a switch or config key), not of whoever controls the environment. [B]

### 7.4 Open defects: `D/AUDIT-upstream-since-rebase.md:2355-2366`, eight rows, "All are with the GIL off", most serious first
| Row | What | Evidence | Cause class for a new design |
|---|---|---|---|
| O17-1 | A **getter inline cache** against another thread's delete and re-add, or data/accessor redefinition, of the property: "a garbage callee, a fault in `op_call`, an internal cell reaching `toString`" | ordinary object, Baseline cap, Release: 18 of 20 runs (delete and re-add), 9 of 20 (data to getter and back), 0 of 20 (getter to getter). Mechanism by reading only | The IC is JIT machinery, but the hazard is the object model's: *a slot changes kind (value / `GetterSetter`) while a reader holds a shape-derived fact*. Any compiled fast path "structure check, then load slot, then treat as X" has it. |
| O17-2 | A store into a small typed array is lost when another thread reads its `.buffer`: `slowDownAndWasteMemory()` copies the elements without a stop | the creator failed to read back its own store in 608 (interpreter), 1,192 (Baseline), 8 (all tiers) of 4,000 rounds | C++ runtime / object model: copy-then-swing of storage that others write. Stays. |
| O17-3 | A property `Atomics` store lands after `freeze()` has returned on another thread | 22 of 3,000 rounds | the API's lock-free arm against a structure-only transition. Stays if the API is kept lock-free. |
| O17-4 | `ErrorInstance` writes a bit-field in the byte the materializing thread writes, outside the claim | by reading; worst case: lazy properties put twice | C++ runtime (bit-fields sharing a byte). Stays. |
| O17-5 | structured clone tests `isDetached()`, then `byteLength()`, then `span()` which re-reads base and length, against `transfer()`/`resize()` | 1.8 M detaches against 2.1 M clones: no crash, 6 threw. "The same holds for any C++ caller of `span()` on a buffer another thread can detach" | C++ runtime and embedder. Stays. |
| O17-6 | `loadLoadFence`/`loadStoreFence` became `dmb ishld` on ARM64; the branch adds 45 uses, none checked | by reading | architecture. Stays for arm64. |
| O17-7 | A put on an object that inherits a setter or read-only property from an object under `Thread.restrict` is not refused | cold, without caches: "a question of the API's scope" | API definition. |
| O17-8 | With the FTL on, an indexed loop over a segmented array is compiled again and again | compile counts | JIT only. |

Other known-open items with memory-safety relevance (`D/PARITY-PLAN.md:827-832`, `:976-1006`): I-1a (deleted slot reused inside a deferred watchpoint-claim window),
I-2 (a persistent wrong value, about 1 in 100,000 amplified runs; candidate: `Structure::materializePropertyTable` publishes a table assembled without the lock over one another
thread pinned and edited, "check-then-act across two lock holds"), W4 (Wasm reachable from spawned threads), H5 (module evaluation), F30 (unexplained FTL trap during a typed-array
transfer), N2-N4 (arm64: a structure check does not order the slot read after it).

---

## 8. What "GIL on" is, why it existed, and whether a design without it loses anything (question 8)

### 8.1 What it is
`--useJSThreads=1` alone. Threads are real OS threads with their own stacks, but "GIL=JSLock (G7) of single shared VM. No new lock." (`D/SPEC-api.md:145`). The thread body takes a
`JSLockHolder`; the lock changes hands **only inside blocking primitives** (`join`, contended `hold`, `cond.wait`, property `Atomics.wait`, and, as landed deviation D2, `notify`):
"Cooperative-only" (`:150-151`, Dev 9 `:19`). `jsThreadGILTimeSliceMs` is "reserved, inert". So "With the GIL on, a thread runs until it blocks, so the spinning thread never lets the other
one run" (`D/LANDING-PLAN.md:126`): 22 tests that spin on shared state had to be marked `//@ threadsRequireGILOff`.

### 8.2 Why it existed
1. **Semantic oracle and test enabler**: `THREAD.md:23`. The API, its corner cases (D1-D13) and about 150 tests were settled before any concurrency existed in the engine.
2. **Fallback and bisection oracle**: "Every GIL-on path stays compiled, the fallback/bisection oracle (section J); GIL-on observable behavior unchanged" (`D/SPEC-ungil.md:15-18`); invariant U19:
   the GIL-on corpus must stay green after every GIL-removal task.
3. **The shippable intermediate**: gate B of three (flag off; GIL on supported; GIL off supported; `D/FLAG-OFF-LANDING.md:1419-1422`).
4. After the tenth round, **cheap**: in a GIL-on process every thread's butterfly TID is 0, no word is tagged, the JIT tiers emit `main`'s accesses, there are no polls. Sound because "the GIL changes
   hands only inside blocking calls, and nothing holds a storage pointer, a shape check or an open fast-path window across a call" (`D/LANDING-PLAN.md:3574-3589`). Score 0.958 of flag off.

### 8.3 What it cost
- A third mode in every gate: "about 980 `gilOff` tests"; 26,150 static tests of `gilOffProcess` and 4,164 of `useTaggedButterflies` in the binary (`D/LANDING-PLAN.md:5116`, `:5174`); later, functions compiled once per mode.
- Semantics of its own that are **not** those of real concurrency, so the corpus carries per-mode expectations for SD1-SD5 and SD8-SD19 (`D/SPEC-ungil.md:1040-1042`): one shared microtask queue drained by whoever holds the lock;
  completion at `fn` return; `notify` as a yield point through which "foreign JS can run inside a notify() call" (`D/INTEGRATE-api.md:976-983`); typed-array `Atomics.wait` refused on spawned threads.
- Machinery only it needs: `GILDroppedSection` (because `DropAllLocks`' LIFO unwinding livelocks with N waiters, D1), saving and restoring per-thread execution state at each hand-over, a bounded "yield for pending park resumptions".
- Starvation as a feature: two `sync/` tests are skipped in every mode because "a spinning thread starves the others GIL on" (`D/PARITY-PLAN.md:903-905`).

### 8.4 Does a design without it lose anything?
- **An oracle for the API semantics**: no longer needed as a mode, because the semantics are now written down and the corpus exists. But the new design must pick the **GIL-off** column wherever the two differ (section 2), and the
  tests' `GILOn` variants must be dropped, not ported.
- **A bisection tool** ("is this bug a race or a logic error?"). This is a real loss. A cheap substitute that needs no mode in the engine: run with one spawned thread at a time under a test-harness lock, or serialize at the safepoints.
- **A fallback to ship** if full concurrency is late. In the JIT engine that mattered because GIL off cost 24 % single-threaded and was unsafe. An AOT-only design has no flag-off configuration to protect inside the same binary
  if threaded executables are compiled as such; whether it needs a fallback is a planning question, not a semantic one.
- **Nothing in the JavaScript API** depends on the GIL. The clauses marked GPO ("GIL-phase-only") in SPEC-api are exactly the ones SPEC-ungil replaced.
- One lesson runs the other way: several GIL-on shortcuts rest on "nothing holds a storage pointer across a call". That is a property of generated code that an AOT compiler could also state and check, and it is what a
  *safepoint-only hand-over* design (threads switch only at calls/polls) would rest on.

---

## 9. Testing assets that do not depend on the implementation (question 9)

Full inventory with counts, directives and per-directory notes: appendix E. Summary:

### 9.1 The corpus `JSTests/threads/`
500 files, 476 tests, about 41,900 lines. Conventions (frozen, `D/SPEC-api-annex.md:18`): self-checking, failure = throw; every spawned thread joined or awaited; no reliance on preemption; race tests bound their blocking operations.
Latest result: "Threads corpus, Release, four modes | 62 / 48 / 403 / 376 passed, 0 failed" (CVE suite GIL off / GIL on, default corpus GIL off / GIL on, by comparison with the eighth round's "329 + 346 + 48 + 62", `D/LANDING-PLAN.md:3103-3104`; `:4852`).

| Directory | Tests | What | Reuse |
|---|---|---|---|
| `api` | 38 | constructors, errors, async variants, termination, blocking policy | the 12 original files map to invariants API-I1..I24 (`D/SPEC-api.md:293-320`, "the most compact implementation-independent statement of what the API must do"); later files pin engine fixes |
| `atomics` | 19 | `Atomics` on properties; typed-array path unchanged | high |
| `races`, `sync`, `lifecycle`, `invariants` | 7, 13, 10, 7 | counters under Lock and Atomics, transition against read/write, futex from property Atomics, Condition, join semantics, the object-model guarantees as observable properties | high: nearly all use only the API |
| `semantics`, `shared-objects`, `arrays` | 26, 17, 14 | language features under threads: prototype cycles, freeze/seal, symbol registry, stack overflow, private fields, RegExp, async generators, global declarations, first-use races; Map/Set/WeakMap writers; array shapes | high |
| `objectmodel`, `vmstate` | 81, 39 | headers name old mechanisms, about half the bodies are plain JS races with value oracles; `vmstate` "is effectively a checklist of hidden shared runtime state" | medium |
| `cve` | 65 | susceptibility tests for 20 mechanism classes | 58 of 65 need only shims |
| `gc-stress` | 14 | conservative scan of a parked thread's registers, destructors once on any thread, zombies | ideas; assertions often use `$vm` |
| `scaling`, `bench` | 7, 8 | parallel-self workloads with checksums; serial micro gate | workloads portable |
| `jit`, top level `heap-*`, `congc-*`, `giloff-*` | 75, 36 | ICs, OSR, watchpoints, C++ heap harness | mostly not |

Mechanical split by visible dependency: 175 tests use nothing but `load`, standard JS and the API; 153 also use shell helpers with an obvious replacement (`print`, `noInline`, `gc`, `asyncTestStart`, `preciseTime`, `drainMicrotasks`); 148 use internal `$vm` hooks or
engine options (about 20 of these only name a checker option). Estimate: 37 % reusable as is, 38 % after a shim file, 25 % implementation specific. Points to watch for an AOT compiler: 417 files use the shell's `load()`; **32 use `eval` or `new Function`**; 46 spin on shared state and so
depend on the visibility rule; some assert old *policies* (Wasm and `import()` refusal, typed-array wait gate, id range, exact error messages); many early tests are weaker than they could be because they were written around the cooperative GIL.

`harness.js` + `resources/assert.js` (170 lines): `shouldBe` (SameValue-like), `shouldThrow(Type, fn, msg)`, `shouldNotThrow`, `spawnN(n, fn)`, `joinAll`, `withTimeout`, `sleepMs` (a timed property `Atomics.wait`), `waitUntil(cond, maxMs, stepMs)`.

### 9.2 Runner policies worth copying (`Tools/threads/run-tests.sh`)
Never turn an engine crash into a skip; **ask the binary for its effective mode** (earlier "GIL-off" runs of Bun and of zsh scripts were silently GIL on); a skip-heavy or empty run is a failure; a test can report that its premise does not hold; expected failures fail when they pass; a hang is a failure with a bounded timeout;
a coverage grep requires each API invariant to be cited by a non-skipped test.

### 9.3 Race amplifier
An in-engine hook, `RaceAmplifier::perturb()` (250 lines, engine neutral): per-thread PRNG seeded from a global seed and a thread-creation ordinal; a countdown re-armed at random; on firing 3/4 `sched_yield`, 1/4 a sleep of 1..100 microseconds; three options (`randomYieldPeriod`, `randomYieldSeed`, `randomYieldMaxMicroseconds`,
`S/runtime/OptionsList.h:582-584`). Driver `Tools/threads/amplify.sh`: N runs with rotating seeds, the first run is the reference; classifies CRASH / HANG / DIVERGENCE and prints a replay command. Rule for a site: "*inside* the racy window you want to widen (after the read, before the publish)" (`D/AMPLIFIER.md:148-158`).
**Its weakness**: all 24-25 call sites are in the heap and in teardown; none in the object model, so "Clean under the amplifier [...] says nothing about transition windows" (`D/PARITY-PLAN.md:1030-1035`). An AOT compiler could emit `perturb()` at compiled safepoints and shared-access slow paths in a test build, which the old rule "never JIT-emitted code" excluded.

### 9.4 ThreadSanitizer
Two configurations: a C-loop, no-JIT build (`D/TSAN.md`) and the one the campaigns used, full JIT ("TSanJIT"). Reports fell from 10,515 (32 families) to 0; one open (`DateInstance`). Real bugs it found: shared RegExp state, VM string caches, date cache, microtask queue, symbol registry, unserialized rare-data creation.
Standing rules: each suppression justified in place, narrowest matcher, "NEVER suppress a race in code touched by the threads workstreams", check names after a rebase because "a stale name fails silently". Its limit: "**any race whose one side is generated code.** This is the largest blind spot" (`D/PARITY-PLAN.md:1017`); also blind to check-then-act across two lock holds (I-2) and to ICU.
For AOT: if compiled JS can be instrumented, that blind spot goes; but JS-level races are legal, so heap accesses must be relaxed atomics or annotated.

### 9.5 The mirror harness (`Tools/threads/mirror/`)
It compares nothing: it "runs one test file's top-level code concurrently on two JS threads that share the shell's global object. Wrong output and uncaught exceptions are expected and ignored; the harness exists to surface crashes, assertion failures, sanitizer reports and hangs." Modes: `eval` (threads race on the shared global) and `func` (one `new Function`, shared code, private locals).
"5,752 files written to test everything else in the engine become 5,752 two-thread races through every subsystem, with no idea in advance of what should break" (`D/LANDING-PLAN.md:1628-1630`). It found eleven engine bugs "of kinds the written tests had not imagined". Its driver distinguishes a deadline while burning CPU (124) from a deadline while blocked (125, with a backtrace of every thread).
For AOT the mechanism (run-time `eval`) must become a build step that wraps each source in a function and calls it on N threads.

### 9.6 CVE audit
`D/CVE-AUDIT.md` + three catalogs of concurrency CVEs in other runtimes (`D/cve/jsengine-sab.md`, `jvm.md`, `runtime-general.md`), merged "by *failing invariant*, not by runtime" into 20 classes: MC-DF (double fetch), MC-TEAR, MC-GROW, MC-JIT, MC-REENT, MC-VAL, MC-INT, MC-LIFE, MC-TDWN, MC-WAIT, MC-LOCK, MC-GC, MC-SAFE, MC-CODE, MC-INIT, MC-PRIM, MC-HAND, MC-AINT, MC-SPEC, MC-DOS.
Independent of the implementation. Rule worth keeping: "No finding may be downgraded on 'window too small' grounds, only on 'no writer can exist' grounds". Six findings had executed crash evidence; **none needs a JIT**. Without a JIT only MC-CODE mostly goes; MC-JIT turns into "soundness of ahead-of-time optimizations across points where another thread can write".

### 9.7 Others
- **Fuzzilli profile** (`Tools/threads/fuzz/JSCThreadsProfile.swift`): 13-16 API-level generators (spawn/join, shared property storm, array resize race, dictionary flip, restrict, property Atomics, wait/notify, lock contention, condition, ThreadLocal, shared Proxy getter). Crash oracles only; never run GIL off; last run in June.
- **Scaling suite** with per-thread checksums equal to a serial reference (also a correctness test) and `scalebench`: one program in JS, Go and Java with bit-identical checksums across languages and thread counts.
- **GC stress matrix**: the corpus under scribble-on-free, zombie mode, continuous collection, every allocation on the slow path.
- **Ordinary suites with the feature on and no thread spawned**: "every difference from the flag-off run is a bug" (`D/LANDING-PLAN.md:189-193`). It caught a one-thread abort that broke every Bun test.
- **Bun's tests with a second JS thread kept alive** by a preload (a timed `Atomics.wait` loop with a little allocation): found F31 and the finalizer-thread class.
- **Exact event counters as a test oracle** (`$vm.jsThreadsCounter`: "this path took no stop").
- **The stop watchdog** as an oracle for "a poll was deleted" or "a lock is held across a stop".
- **Lints**: eight of nine serve the flag-off landing or JSC layout; only the lock-order lint (`lint-lockorder-u20.sh`) is a general idea. Debug assertions "cell-lock depth is 0" at each park site, exception check and heap-access transition found 22 more files (`D/LANDING-PLAN.md:1366-1377`).
- **Never built, and wanted**: the per-location litmus set (section 3.2); test262 on a spawned thread with verdicts diffed against main; a differential fuzzer (two-thread program against the same program run sequentially); amplifier sites in the object model; a census of natives never entered by two threads at once (V-7).

---

## 10. Hazards with several mutators (what the earlier work teaches to look for)

This report's subject is not the AOT engine, so this section lists hazard **classes**, each with the instance that proved it and the worst consequence seen. "MS" = memory safety (type confusion, wild pointer, use after free); "V" = only a wrong JavaScript value, exception or hang.

| # | Class | Proven by | Worst |
|---|---|---|---|
| H1 | **Copy, then swing the pointer**, while others write the old copy | butterfly growth (P1); `slowDownAndWasteMemory` (O17-2); CoW materialization; table rehash | V (lost store); MS if the old copy is freed |
| H2 | **Two words that mean something together**, read or written separately: {StructureID, butterfly}, {shape, lanes}, {base, length}, {publicLength, vectorLength}, {allocator, structure}, {callee, CodeBlock}, {flag, payload} | P3, P10, P11; typed-array detach cleared the base: "it wrote at 4095 bytes past the start of the primitive cage" | MS |
| H3 | **A slot changes meaning under a reader that checked earlier**: data/accessor, Int32/Double/Contiguous, deleted then reused, dictionary add in place | O17-1; a `GetterSetter` reached `jsAdd`, 10 of 10 SIGSEGV; raw double read as pointer | MS, also address disclosure |
| H4 | **Check, plan, act** on shared metadata without re-validation | seal against delete + eleven siblings; global declarations; `materializePropertyTable` (two lock holds; TSan silent) | V to MS |
| H5 | **Name before value** (publication order) | dictionary in-place add: `JSON.stringify` dereferenced an empty value; arguments objects' flags | MS (null cell) |
| H6 | **One validation covering several reads** | spread, `Object.assign`, `Object.values` wrote past its result array | MS |
| H7 | **Lazy first-use initialization** on a read path | Error `.stack` (crash on every run), function `name/length/prototype`, module namespace, `LazyProperty`, promise executables, watchpoint installs, RegExp compile | MS (UAF) |
| H8 | **Scratch state stored in a shared cell or in the VM** | `RegExp::m_ovector` (realloc UAF), module resolution cache (HashMap rehash UAF), sort scratch, string caches, `m_asyncContextData` cursor, `syncResumeCallCache` | MS |
| H9 | **Unlocked C++ containers** reachable from two threads | Map/Set/WeakMap tables ("Two threads writing one `WeakMap` crashed every time"), sparse maps, `Heap::m_markListSet`, shell's `PropertyFilter` | MS |
| H10 | **Refcounts and bit-fields** | plain refcounts on handlers; `StringImpl` flags; `ErrorInstance` bit-field sharing a byte (O17-4); atom resurrection at refcount 0 | MS |
| H11 | **Blocking while holding heap access** (heap access is a lock) | any raw `lock()` whose holder can park or request a stop; native waits without a bracket | V (deadlock, 30 s abort) |
| H12 | **A safepoint, allocation or exception check under a cell lock** | `RETURN_IF_EXCEPTION` under a cell lock parked the holder; `reportExtraMemoryAllocated` under a lock started a collection; a `DeferGC` scope *ending* under the lock | V (deadlock) |
| H13 | **The conductor runs foreign code inside its own stop** (destructors, finalizers, trap polls) | three self-deadlocks | V |
| H14 | **"World is stopped" evidence that is not yours** | acting on a GC stop somebody else conducts: it "can evaporate mid-`work`"; five notions of "stopped" coexisted | MS |
| H15 | **A state flip that precedes its effect** | "invalidated" is not "fired"; a flag written after the release that publishes it (`m_releasedByGCPark`) | MS |
| H16 | **Thread exit and VM teardown** | exit made the thread's young cells look dead (`StopAllocatingMode::ForGood`); last `Ref<VM>` dropped on a spawned thread; sampling profiler bound to a dead thread | MS / hang |
| H17 | **Destructors and finalizers on another thread** | F31 | embedder panic |
| H18 | **Compiler hoisting in C++** | "The compiler moved the load of a nuked StructureID out of a spin loop" (Release only) | V (hang) |
| H19 | **Compile-time facts taken from mutable heap state** | F30 (unexplained) | MS |
| H20 | **Facts kept across a park site** in compiled code | the whole of AUDIT-checktraps | MS |
| H21 | **Assertions that encode single-thread truths** | "a structure cannot change mid-operation": aborts on legal programs; 596 fail-stop sites added | V (abort) |
| H22 | **Words written per operation by all threads** | execution counters, profile words, a report word, one `RefPtr` by value | speed only (3.5x to 5x) |
| H23 | **Constant budgets and client-count heuristics** in the collector | 32 weak-bearing blocks per cycle; five consumers of `clientSet().size()` | memory growth |
| H24 | **Weak ordering** | arm64: a structure check does not order the slot read after it; relaxed pointer load then dereference | MS on arm64 |
| H25 | **Merges** | "No file conflicted, and the merge was wrong twice all the same"; an upstream `indexOf` "merged without a textual conflict and the combination undoes" a clamp; 280 blocks compiled out by a removed macro | any |
| H26 | **Run-to-completion assumptions in JS itself** | continuations migrate between threads (SD10); module evaluation twice | V |

---

## 11. What helps: why threads should be easier without a JIT (and where they are not)

Derived from sections 5 and 6; judgement, not measurement.
1. **No code ever changes.** I2 and everything under it (stops for fires and jettison, ISB generations, retirement of code, compiler threads, the compilation lock) has no counterpart. The non-GC reasons to stop the world shrink to a short list.
2. **No speculation that can be invalidated**, hence no deferred-claim windows, no "loser returns into its own stale code", no I-1a, no heap-fact epoch.
3. **No tiers and no profiles**: no bimodal performance, no thread multiplier, no shared hot counters; performance is the same on the first and the millionth iteration, and a thread body entered once is not at a disadvantage.
4. **One tier means one semantics**: the argument against full release of the visibility rule ("works in the LLInt, Baseline and DFG tiers and hangs once the FTL compiles the loop") loses its tier half. The choice is still needed.
5. **The compiler owns poll placement and the poll's clobber set** from the start (D-B1..D-B4 for free) and can do escape analysis to drop claims and predicates on fresh objects (R-1), which the JIT project only designed.
6. **Claim-first can be the only transition protocol**: "exclusion between writers comes from the claim on the StructureID, 'the lane, not the sets'" (appendix A, take-away 3). The watchpoint sets need never exist.
7. **Compiled code can be instrumented** (TSan, amplifier sites, validators) because it comes from an ordinary toolchain.
8. **No flag-off problem** if a threaded executable links a runtime compiled for threads: no mode bytes, no per-mode copies, no 26,150 gate tests.
9. **The API, its 24 invariants, 13 landed deviations and 19 semantic deltas are already debugged**, with about 350 portable tests.
10. **The two audits (K4, N7) and the heap-container audit are a ready checklist** of shared C++ state, with a disposition for each row.

Where it is **not** easier: no watchpoint elision (each check is always paid); no OSR exit (each unexpected state needs a real path, and facts cannot cross park sites for free); more time in C++ under locks (structure tables, Map/Set, strings); every heap and runtime problem unchanged; the embedder (Bun natives, finalizers, event loop) untouched by the earlier work.

---

## 12. Not found / not sure

- **`process.exit`, `beforeExit`, signals, `console`, timers on spawned threads in Bun**: nothing is specified. The documents say Bun natives are unusable on spawned threads and leave the policy open (`D/LANDING-PLAN.md:1936-1946`). Bun was built against the branch in the early rounds (`:786`, `:2854`, `:3142`); from the twelfth round on its patch is only carried along, "It was not built" (`:4198-4199`, `:5005-5006`).
- **Does a running, unjoined spawned thread keep the process alive?** Spec: "No implicit join; mid-fn teardown may terminate abruptly". `~VM` blocks until other lites are gone, but the shell normally exits without destroying the VM. I did not read `jsc.cpp`'s exit path.
- **Owner's answers to decisions 1-22**: none found in the repository.
- **Closure variables**: no rule in the object-model spec; the scope sources contain no thread code. Inferred (not read anywhere): environment slots are fixed in-cell slots, "atomic for free" (`THREAD.md:13`). TDZ reads racing initialization: not found.
- **Publication of a fresh object by a plain store** (rule 6 of 3.2): no invariant states it; on arm64 the fenced barrier is what orders it.
- **Which thread runs a `FinalizationRegistry` callback**: `D/PARITY-PLAN.md:1092` says the carrier ("deferred-work timer: carrier only"); the code path was not read.
- **Thread id range**: spec `[1, 0x7ffe]`, test header and `maxJSThreads` 16383 say `[1, 0x3fff]`; I take the tree.
- **Per-thread or VM-level trap word in generated code**: documents and comments disagree (appendix B, section 9).
- **Barrier always fenced GIL off on x86?** Documents and code disagree (appendix C, section 14).
- **No measurement of GIL off without a JIT after the seventh round**; the 0.89 is arithmetic on an old table.
- **Row R of the ledger (eleven to twelve points of time) was never prototyped**; its families are ranked by profile difference only.
- **None of the eleventh session's GIL-off designs was implemented**; rounds 12-17 worked on flag off.
- **arm64**: every statement is by reading.
- **History files**: `SPEC-ungil-history.md` (610 KB) was read only at the annexes cited (INV, SD, ALS1, AUD1.K3); `SPEC-api-history.md`, `SPEC-nativeaffinity-history.md`, `SPEC-vmstate-history.md`, `UNGIL-HANDOUT.md`, `CONGC-HANDOUT.md`, most `INTEGRATE-*.md`, `TSAN-TRIAGE.md` beyond its head and the `map-MC-*.md` files were not read. Each appendix ends with its own list.
- **Method**: sections 0-4, 7, 8 are from my own reading. Sections 5, 6, 9, 10 condense five sub-reports (appendices A-E) written from the sources; all five were read in full and spot-checked about thirty of their citations against the files (all matched). Class columns and "in AOT?" columns are judgement everywhere.

---

# Appendices

Written from the sources named in each, read in full and spot-checked. Text marked [inference] or [mine], and every "class" or "in AOT?" column, is the helper's judgement.

---

## Appendix A. Object model

(Original title: Object model of the earlier JS-threads work (JSC with JIT): digest for a no-JIT design)

Read-only research over `<threads-branch>/` (branch `sosuke/threads`). Every fact carries `file:line`.

**Citation aliases** (all paths relative to the WebKit root):

| alias | file | alias | file |
|---|---|---|---|
| OM | docs/threads/SPEC-objectmodel.md | DP | docs/threads/DESIGN-PROPOSALS.md |
| OMH | docs/threads/SPEC-objectmodel-history.md | PP | docs/threads/PARITY-PLAN.md |
| OMA | docs/threads/SPEC-objectmodel-annex.md | PR | docs/threads/PERF-RESULTS.md |
| LP | docs/threads/LANDING-PLAN.md | JIT | docs/threads/SPEC-jit.md |
| UG | docs/threads/SPEC-ungil.md | UGH | docs/threads/SPEC-ungil-history.md |
| AUD | docs/threads/AUDIT-upstream-since-rebase.md | SB | docs/threads/SCALEBENCH.md |
| CB.h / CB.cpp | Source/JavaScriptCore/runtime/ConcurrentButterfly.h / .cpp | T/ | JSTests/threads/ |

**Terms.** *Flag off*: `useJSThreads` not set. *GIL on*: threads exist, one runs at a time. *GIL off*: threads really run in
parallel; this is the only mode that matters for us. *Word*: the 64-bit butterfly field of the cell. *Owner*: the thread whose
ID (TID) is in the word's tag. *Foreign*: any other thread. *Nuke*: set a bit in the 32-bit StructureID so that it matches no
structure. *Claim*: compare-and-swap StructureID `S -> nuked(S)` before writing anything; whoever wins has the exclusive right
to transition the object. *Stop*: a stop-the-world of all JS threads. *TTL sets*: two watchpoint sets per Structure ("no
instance was ever transitioned by a foreign thread", "no instance was ever written by a foreign thread"). *Lane*: one 8-byte
array element slot. *AS*: ArrayStorage shapes. *CoW*: CopyOnWrite array shapes. *PA cell*: cell in a PreciseAllocation (large
object; address is 8 mod 16).

---

### A: 0. The ten things to take away

1. **The promise**: SharedArrayBuffer's memory model for values, plus "object-model invariants hold under races (no lost
   properties, no torn shapes, no corruption), not race-free programs" (THREAD.md:5). One invariant carries most of it, I21:
   "No lost property adds ..., torn JSValues, structure/butterfly mismatch, or read of f returning g's value, under any race"
   (OM:232).
2. **The final design has two object models, chosen per process.** GIL off: tagged words, unchanged encoding (bit 63 SW,
   62..48 TID, 47..0 payload, OM:15). GIL on and flag off: no tag at all, `main`'s code (rule G1, tenth round, OM:27). What
   SPEC-api's "used to" refers to is G1, not a change to GIL off (section 2.0).
3. **The per-Structure watchpoint sets and their stops were progressively emptied.** They began as the core of the design
   ("making the checks free", THREAD.md:15). By rev 17 exclusion between writers comes from the claim on the StructureID,
   "the lane, not the sets" (OMH:1468), and for inline caches "the stub watches NO thread-local set" (JIT:102). Only DFG/FTL inlined code
   still needs them. A no-JIT engine can start from claim-first and never have the sets.
4. **Keying ownership on shared metadata was the most expensive mistake**: ownership of butterfly-less objects keyed on the
   Structure's creator made two threads of independent object creation 230x-400x slower (OM:33); fixed by stamping the
   allocating thread's TID in the unused word of every object at birth.
5. **Readers check the shape, then load the storage, then load the slot, and do nothing after that** (OMH:1240-1243). Every hard problem (Double arrays,
   data<->accessor redefinition, delete + reuse, dictionary in-place add) is a writer changing the *meaning* of a slot under
   such a reader. The three remedies used: never change meaning in place (copy or fresh offset), a stop per event, or an
   after-read re-check. The recommended end state for arrays puts the encoding in the same word as the pointer (DP:1562).
6. **Segmented butterflies were correct and nearly unused by fast code**: 243.7 vs 19.0 instructions per element read
   (DP:2022); the project kept adding ways *not* to segment (StayFlatShared, CB.cpp:1154).
7. **Measured floor of tags alone**: +4.7 % instructions (PP:229), factor 1.05 of a total GIL-off factor 1.30 over GIL on
   (LP:5059). In *time*, locked instructions matter more than tags: 0.48 locked loads per 1000 instructions vs 0.25 on
   `main`, 4.7 % of `main`'s cycles (PP:217).
8. **Shared Structure metadata (transition table, property table) needs its own protocol**; the per-object lock does not
   cover it (OMH:893-917), and the one open memory-safety defect with a confirmed mechanism is there (PP:1433-1442).
9. **Most late bugs were C++ check-then-act**, found by widening the test corpus, Release builds, a race amplifier and a
   "mirror" harness running ordinary single-thread stress tests on two threads, not by review (section 5).
10. **arm64 was never built or run** (LP:5234); two orderings are known unsafe there (DP:7462, DP:7508).

---

### A: 1. Memory model promised to JS for plain (non-Atomics) racy accesses

#### A: 1.1 The general statement

- "Memory model = SharedArrayBuffer's; the engine guarantees object-model invariants under races" (THREAD.md:5).
- Atomic operations: add, delete, get, set a property, change its configuration, snapshot the names (THREAD.md:156-163).
  `o.f` through a prototype or a getter is *not* one atomic step (THREAD.md:164-168).
- "For any heap created by a concurrent JS program, it should be possible to write a sequential JS program that creates an
  indistinguishable heap" (THREAD.md:169). "No race should ever lead to a VM crash, lost writes, or the value of a property
  experiencing time travel" (THREAD.md:200).
- The most precise list is a *planned* conformance set, not an existing test directory (PP:1146-1169): (1) no thin air, no wrong
  kind: "a location that only ever held int32s never yields a double, a cell, or a hole; a property that was only ever data
  never yields a `GetterSetter`"; (2) no tearing: "a Double lane is one of the written doubles bit for bit"; (3) per-location
  coherence; (4) message passing through `Atomics` and `Lock`; (5) bounded visibility, undecided; (6) publication: an object
  "published by a plain store is seen with its constructor's fields or not at all"; (7) delete + add never shows the new
  property's value under the old name. It names the location kinds: "inline property, out-of-line property flat and segmented,
  array element of each indexing shape, typed-array element, global variable, closure variable, `Map` entry" (PP:1149-1151).
- **Visibility (does a spinning reader ever see the write?) was never ruled.** Current rule: a read that can decide control flow
  ("a spin on a plain field, element, closure or global variable, a cancel flag ..., a loop bound") is redone after every
  poll; a read that only feeds data "may be hoisted out of the loop and is then as stale as the loop is long" (JIT:214).
  The two candidate contracts are bounded staleness (1 ms tick) and full release, `main`'s contract for plain
  SharedArrayBuffer reads (DP:1004-1008, LP:5070-5076).

#### A: 1.2 Case by case

| access | a race MAY produce | a race may NEVER produce | source |
|---|---|---|---|
| Read/write of an existing named property | a stale value; a lost update of a compound op (`++` "can lose an update with the GIL off, as on a SharedArrayBuffer", LP:164-166); final value = one of the written values (T/races/transition-vs-write.js:10-11) | a torn value ("one atomic EncodedJSValue", OM:56); a value going backwards against one monotone writer, "including across transitions that reallocate the storage" (T/invariants/no-time-travel.js:7-9); another property's value (I21, OM:232) | |
| Add vs read | absent, or present with its value | the name without its value. I9: "A reader seeing a transition's new StructureID sees the new property's value (release-stored first)" (OM:218). Known hole on arm64 for inline slots (OM:205) | T/invariants/no-torn-shapes.js:9-12 |
| Add vs add (different names) | either order | a lost add; "o.f = 1 racing o.g = 2 must never produce o.f == 2, "g" in o == false" | T/invariants/no-torn-shapes.js:5-7; T/invariants/no-lost-properties.js:4-9 |
| Add vs add (same name) | the value of either writer | "never another property's value, never a torn/garbage value, never missing" | T/invariants/no-lost-properties-same-name.js:5-8 |
| Delete vs read | "either its pre-delete value or undefined" | a later-added property's value through slot reuse | T/invariants/delete-quarantine.js:6-11; D1/I30 OM:186, OM:242; I18 OM:229 |
| Delete + re-add of the same name vs read | old value, `undefined`, new value | anything else | T/invariants/delete-quarantine.js:60-62 |
| Array element read/write | stale value; "old or new JSValue per slot, never torn" (OMA:55-56) | a value nobody wrote; for an Int32-shaped array GIL off a stale reader may load "a NON-Int32 JSValue ..., never a non-JSValue" (I41, OM:253) | |
| Stores to disjoint indices while the array grows | nothing is lost: "after join, every element holds its writer's value and length is exact" | a dropped store | T/invariants/no-lost-elements.js:5-9 |
| `push` vs `push`, unsynchronized | not atomic: "two threads can read the same length, so an element can be overwritten, and a thread that read an old length can shrink the array under another one, which leaves holes" | more elements than pushes; an element that was not pushed, or appears twice | T/arrays/push-resize-multithread.js:50-55 |
| Growth vs read of a high index | "EITHER undefined (hole/stale spine) or the writer's exact value" | "never garbage, never a crash, never another slot's value" | T/objectmodel/i03-stale-spine-reader-vs-grow.js:11-15; I33 OM:245 |
| `length` | raises are monotone (compare-and-swap maximum); lowers (pop, shift, `length =`) are plain stores: "a shrink racing a grow is a program-level race" (JIT:116). On arm64 only: `a.length === n + 1` and then `a[n] === undefined` (DP:7521-7530, "a memory-model statement, not a defect") | a raise that hides another thread's element (I21 "in every tier", OM:232); `a[0]` or `a.length` regressing under a grower (T/invariants/no-time-travel.js:61-63) | |
| Holes | a hole where an element is being stored or deleted | a truncated-then-regrown slot resurrecting a stale value (T/arrays/push-resize-multithread.js:126) | |
| Double arrays | a written double, or `undefined` (hole = PNaN) | a torn double ("aligned 8B slots: tear-free, SAB semantics", OM:140); "a "double reinterpretation" of a pointer or vice versa" (T/objectmodel/i03-shared-double.js:11-13). In fresh copies "a hole or any NaN read from a racing lane becomes a hole" (OMH:1567-1568) | I28 OM:240 |
| Data <-> accessor redefinition vs read | the data value or the getter's result | "never the engine's internal getter/setter cell under the data property's structure, never a call of a plain value" | T/objectmodel/define-property-kind-change-vs-readers.js:4-7; I39 OM:254. **Open**: AUD O17-1 (section 5) |
| Dictionary objects | same as ordinary objects | same; "survivors keep exact values through arbitrary churn" | T/invariants/delete-quarantine-dictionary.js:7-11 |
| Multi-property reads (`{...o}`, `Object.assign`, `Object.values/entries`, `JSON.stringify`) | a snapshot "linearized before the delete", or per-property reads; `undefined` for a slot being deleted | an empty (internal) value, an out-of-bounds store | I42 OM:249; OMH:1775-1776 |
| Prototype change | old or new chain; stale optimized code "keeps the old prototype chain" for a while (DP:4586) | a prototype cycle: "exactly one succeeds and the other MUST get the cycle-check TypeError" (T/semantics/proto-cycle-race.js:3-5) | |
| freeze / seal / preventExtensions vs add / delete | exactly one side wins per object | "Half-added states (name without value, value at a wrong offset, isFrozen true but still extensible ...)" | T/semantics/frozen-seal-race.js:4-13. Weaker in optimized code: stale code "can still store to a property that is now read-only" (DP:4584). **Open**: a property `Atomics` store lands after `freeze()` returned, 22 of 3,000 rounds (AUD:2361) |
| Closure variables (JSLexicalEnvironment) | not specified by the object-model spec; only the visibility rule (JIT:214) and the planned conformance set (PP:1151) name them | | see section 6 |
| Global `let`/`const`/`class`/`var`/function declarations | "exactly one declaration of each name must win" | duplicate bindings | T/semantics/global-declaration-race.js:2-8; LP:1662-1667. **Open**: concurrent indirect evals, a call "can find the binding `undefined` for a moment" (PP:1003) |
| Typed array / ArrayBuffer detach, transfer, shrink, grow | an access that pairs the old length with the old, still mapped, base ("stale-but-safe"); spec exceptions (`TypeError` after a transfer, `RangeError` from `set`: LP:128-129) | "a racing reader never pairs a passing length with an unmapped-or-short base" | UG:884-886; UGH:2352-2361, torn-pair table UGH:2412-2419. **Open**: a store into a small typed array is lost when another thread reads `.buffer` (AUD:2360) |

#### A: 1.3 Fences and orderings (OM section 7)

| rule | content | x86-64 | arm64 |
|---|---|---|---|
| M1 (OM:199) | word -> spine -> fragment -> slot | plain loads | address dependency, no barrier |
| M2 (OM:200) | the new property's value is stored before its type | plain store | release store / store-store fence |
| M3, M4 (OM:201-202) | 128-bit CAS of {header, word}, 64-bit CAS of the word, first install, SW flip | `lock cmpxchg(16b)`, seq_cst | `casp` or `ldxp/stxp`, seq_cst; libatomic forbidden (OM:296) |
| M5 (OM:203) | transitions that change layout first CAS the StructureID to nuked; slow-path readers spin while nuked; `structure()` masks the bit, `structureID()` never does | | |
| M7 (OM:205) | a structure check and a *separate* word load must be ordered ID first, "else stale-smaller-storage OOB" | "no-ops" | one of: dependency, load-acquire, re-check after, load-load fence |
| M8 (OM:206) | the fenced nuke order is the only publication order; no in-place realloc of a butterfly GIL off | fence is free; write barrier follows marking | fenced barrier forced "for the heap's life" in both flag-on modes |

Known arm64 gaps, all by reading: an *inline* slot read is not ordered after the structure check in handler caches and DFG/FTL, so
a reader "can then see the new structure ID and the slot's zero fill" (OM:205; DP:7462-7489); residual ID-to-word gaps (DP:7508-7519);
length-then-element (DP:7521). The proposed single rule: "every guard load of the structure ID or indexing byte is a load-acquire
on weakly ordered targets GIL off, in every tier" (OM:205). Mixed-size atomics on the first 16 bytes of the cell "are outside the C++
memory model; we rely on x86-64/arm64 same-address coherence" (OMA:67-70).

---

### A: 2. Mechanisms, in the order things happen

#### A: 2.0 What the FINAL design is, and what was dropped

**Three process-wide configurations, fixed when options are final** (`Options::useTaggedButterflies()`, OM:27):

| | word | object-model code |
|---|---|---|
| flag off | raw pointer | `main`'s |
| GIL on (default `useJSThreadsSingleOwnerWithGIL`) | raw pointer: "every thread's butterfly TID is 0" (OMH:1959) | `main`'s bodies in C++ and all four tiers (OM:27; JIT:110) |
| GIL off, or GIL on with `--useJSThreadsSingleOwnerWithGIL=0`, or a stress option | tagged | the protocols below |

- **Dropped in the tenth round (G1): tags, SW bit, segmented butterflies, TTL fires, owner checks, in a process with a GIL.** Why:
  "The tag ... records which thread may write an object without synchronizing, and GIL on every thread may" (OMH:1956-1957).
  Soundness: "the GIL is handed over only inside blocking primitives, which are calls, and no object-model window or piece of
  generated code holds a storage pointer across a call" (OM:27). Gain: JetStream GIL on / flag off 0.907 -> 0.958 (LP:3552).
- SPEC-api's "per-thread butterfly tags used to cover by accident" (docs/threads/SPEC-api.md:252) is a side effect of G1: two of
  `main`'s fast paths store into a SlowPut array without passing the `Thread.restrict` hook, and with tags "every store from a
  thread other than the allocator failed the write predicate" (same line; LP:3590-3593).
- **GIL off keeps the frozen encoding.** Rounds 12-17 worked only on the flag-off configuration; for threaded modes "Everything the tenth
  round's "Open items" lists ...: unchanged" (LP:4210). Source agrees: CB.h:71-77.
- Other things dropped or reversed on the way (details below): shape-keyed ownership (r16); F4 chain-fire (r16); value-before-claim
  order in locked writers (r17); T5 in-place vectorLength growth (withdrawn GIL off); boxing of shared Double arrays (r12); Int32->Double
  in place GIL off (r17); in-place dictionary flatten under a stop GIL off (r19); per-thread structure splitting (Task 14, never
  promoted: UG:456-460).

**GIL-off read path** (JIT:100; OM:50): load the word; top 16 bits all ones -> segmented read; else mask the top 16 bits and read as
today. "No TID check." AS shapes go to a locked slow path in C++ (OM:48). Int32-mode element reads verify `isInt32` (I41).
**GIL-off write path to an existing slot** (JIT:101; OM:51-52): (1) segmented -> segmented write; (2) tag == my tag -> mask, store;
(3) SW set and not AS -> mask, store; (4) else `ensureSharedWriteBit` (fire the write set in a stop if still valid, then flip SW with
a 128-bit CAS), then store. The TID compare on writes is never elided: the original claim that it could be was "UNSOUND" (JIT:19).
**Inline (in-cell) slots**: "never checked/masked" (JIT:118).

#### A: 2.1 Butterfly word encoding and regimes

- "bit 63=SW; 62..48=TID (installer/owner); 47..0=payload" (OM:15); `notTTLTID = 0x7fff` is reserved (OM:22).
- Decode (OM:25; CB.h:86-91, `butterflyRegimeForWord` CB.h:236-249): payload 0 = **None** (tag names the allocating thread); SW=0 =
  **Flat** ("owner t transitions lock-free"); SW=1 = **Flat shared-written** ("any transition => regime 2"); TID = notTTLTID and SW=1 =
  **Segmented** (payload is a spine). A fourth regime is not in the word: dictionary structures and AS shapes use the **cell lock**.
- **SW (shared-write) bit** means: some foreign thread has stored, or is about to store, into this flat storage. It exists for exactly one reason: the owner's
  growth is copy-then-swing, and a foreign store landing in the old copy would vanish. With SW=0 the owner's CAS of the word
  (expected tag `(me,0)`) fails if a foreign writer flipped the bit first (T1, OM:122). "SW monotonic per object; never cleared" (I4, OM:213).
- Every object is born with its allocator's tag, butterfly or not (I40, OM:252; OM:29).

#### A: 2.2 First butterfly install (N3)

CAS the loaded None word to `(b, myTID, 0)`; "64-bit CAS if header unchanged, else DCAS. Failure = race lost: re-dispatch on the winner's tag"
(OM:35). The value lives in the not-yet-published butterfly, which is why this needed no claim (OMH:931-933).

#### A: 2.3 Property add (structure transition, with or without growth)

Order of writes, all forms: **(claim or nuke the StructureID) -> value -> [word] -> new StructureID.** Five legs, fastest first:

| leg | who | sequence | sync cost | source |
|---|---|---|---|---|
| E4 | owner, both TTL sets of the source valid, not PA | "today's lock-free code: allocate, re-validate E4, store value, nuke, store butterfly (t,0), store StructureID" | none | OM:53, OM:163 |
| N2-LF | owner, butterfly-less, inline slot | CAS ID `old->nuke(old)`; release-store value; fence; store new ID. "a loser never writes a slot another transition owns" | 1 CAS | OM:34 |
| E4-C | owner, sets dead | allocate grown copy; re-read; CAS `S->nuked(S)` (lost => RESTART); re-check word; store value (or copy + CAS word); fence; store `S'` | 1-2 CAS | OM:163; OMH:1431-1447 |
| locked (section 4.3) | owner with E4 failing, foreign, segmented | allocate; cell lock; re-read header + word, "structureID != planning-time source -> RESTART"; claim; value; 128-bit CAS {new header, new word}; unlock | lock + CAS + DCAS | OM:105-109 |
| StayFlatShared (GIL off only, not in the frozen spec) | foreign or SW=1, no indexing header, capacity unchanged | locked; claim; store into the existing slot; DCAS `{newStructure, (installerTID, SW=1)}`; stays flat | same | CB.h:621-628; CB.cpp:1140-1158 |

- **RESTART** is the universal failure rule: "unlock if locked, discard allocations, re-enter the WHOLE operation from dispatch on fresh tag +
  structureID" (OM:91). A target structure is planned *from* a source; after a racing transition it is wrong (CB.h:638-640).
- **DCAS failure taxonomy** (OM:111-116): (a) only GC/lock bytes changed: merge, retry; (b1) SW flipped, payload shared: merge; (b2) SW
  flipped and the desired payload is a *fresh copy*: "merge-and-retry FORBIDDEN (lost write)" -> RESTART; (c) payload changed: re-plan; (d)
  anything else: crash.
- **The header is not privately owned.** GC's cell state, the two lock bits and a per-cell flag bit are CASed at any time, "even under the
  cell lock", so every header CAS copies them from the freshest read, "else a lost unpark deadlocks, GC marking state is clobbered"
  (OM:42; CB.h:267-280).
- **PA cells** cannot take a 16-byte CAS (it faults at 8 mod 16): always cell-locked, published in the fenced order (OM:44, I36 OM:248). The
  global object is one (OMH:342-343).
- Rejected alternatives: a value-first header CAS without a claim ("the CAS loser has already clobbered the winner's slot value", OMH:929-932);
  a per-structure transition lock ("strictly WORSE contended: N threads constructing DISTINCT objects through one shared shape all
  serialize on S", OMH:935-938).

#### A: 2.4 Foreign transition, TTL sets, stops

- A transition by a foreign thread, or by the owner with SW=1, first fires both sets of source and target in a stop, *before* taking the cell
  lock (F2, OM:169; I10b OM:221), then converts to segmented (OM:55) unless StayFlatShared applies.
- What the fire is for: the claim-free owner window (E4; DFG `PutStructure`) "writes the new slot and then the StructureID WITHOUT claiming the
  lane"; the stop retires such code. "This is a MODE SWITCH for S" (OMH:1397-1408). After it, owners claim and shared instances lock:
  its lasting effect "shrinks from "S is locked for everyone" to "S's owners claim instead of storing plain"" (OMH:1451-1454).
- Sets are monotonic, fire only in a stop (I13, OM:224), and a target created from a fired structure is born fired (OM:171).

#### A: 2.5 Flat -> segmented conversion, and the segmented layout

- Layout: an immutable **spine** {fragment counts, vectorLength, aliased allocation base/size, fragment pointers} and mutable 32-byte
  **fragments** of 4 slots that never move (OM:63-77). Growth = new spine + appended fragments (I6, OM:215).
- Conversion is zero-copy: "spine aliases 32B slices" of the live flat butterfly (OM:89), so old flat pointers and the spine reach the same memory
  (I8, OM:217). Steps: fire sets; allocate spine; cell lock; re-read; claim; store the trigger's value; DCAS to `(notTTLTID,1)`; unlock (OM:93-99).
- `publicLength` lives in fragment 0 and is shared by all spines of the object, `vectorLength` is per spine, so every access bounds by
  "min(publicLength, loadedSpine->vectorLength)" and the gap "reads as holes" (C4, OM:87).
- To make slices line up GIL off, every fresh contiguous vector length is rounded to 4k-1 (JIT:114).

#### A: 2.6 Array growth, length

- Element-storage resizes change only the word: "allocate, ONE 64-bit CAS on the tagged word; failure => re-dispatch on the fresh tag, never
  blind-retry" (OM:120). **T1**: copying resize legal only with expected tag `(me,0)`. **T2**: every other resize grows segmented; "SW=1 flat
  resizes NEVER copy element storage outside STW" (OM:122-123, I27 OM:238).
- **A published flat vectorLength is immutable GIL off** (T5 withdrawn, OM:126). Consequence: a hoisted storage pointer "can only be stale, never
  short: old storage is abandoned, not shrunk" (LP:2981-2982), and old storage is kept alive by conservative stack scan (I7, OM:216).
- Length raises are CAS-max (Source/JavaScriptCore/runtime/Butterfly.h:221-227; JIT:116); release on success so the element is visible with the length.

#### A: 2.7 Indexing-shape changes; Double arrays

The reader model: "check the shape ..., load the butterfly word, bounds-check ..., load the lane, with nothing after the lane load"; a reader "can
therefore pair a shape it validated arbitrarily long ago with the butterfly word that is current now" (OMH:1240-1248). Hence (OM:125, OM:140):

| change | GIL-off rule |
|---|---|
| Undecided -> anything, by the owner | claim, fill, publish; no stop ("no tier loads an element lane from an Undecided-shaped object") |
| Int32 -> Contiguous, by the owner | claim, relabel in place; no stop (lanes keep their representation); readers tolerate non-int32 (I41) |
| Int32 -> Double | **never happens**: "EXECUTED AS Int32->Contiguous by every thread" (T4-O) |
| Double -> Contiguous | one stop per array |
| fresh copy (`slice`, `concat`) of a Double source | created Contiguous, boxed during the copy (T4-C) |
| anything on a foreign / SW=1 / segmented object touching Double | one stop per event (I28) |
| shared Double array, no shape change | stays Double; raw-double fragments (R-DOUBLE, r12) |

A copy-convert does not help: "even if both are published by one 128-bit DCAS, a reader that checked Int32 before the publication and loads the
word after it holds (Int32, B') and decodes raw doubles as JSValues" (OMH:1255-1259). Misreads are not only crashes: a cell's low bits as an
integer is "an address leak" (OMH:1308-1310); a pointer read as a subnormal double is "an address disclosure to script under a race" (OMH:1556-1558).
Allocation-profile feedback (T4-P, promotion, a 256-entry address table) was then needed so that numeric sites are *born* Double
(OMH:1577-1590, OMH:1654-1676, OMH:2007-2016).

**Designed, not built (C-D3)**: encoding-changing transitions are copies and "Published indexed storage never changes lane encoding" (DP:1513);
readers validate the storage they loaded. Recommended variant W: a raw-double bit in bit 0 of the payload, so "the bit and the base arrive in
one load" and "No ordering between the shape load and the word load is needed for type safety on either architecture" (DP:1562, DP:1618-1621).
It needs a 140-function audit of C++ lane decoders (LP:5084-5085); typed views make a missed site "a compile error rather than an audit finding" (DP:1594-1596).

#### A: 2.8 ArrayStorage, CopyOnWrite

- **AS** never segments. "flag-on, EVERY runtime/interpreter access (reads INCLUDED, any SW) cell-locked" (OM:134, I31 OM:243). Reason: a sparse-map
  rehash "frees the table under a racing getOwnPropertySlotByIndex - UAF" (OMA:275-278). Anything a stale lock-free reader decodes is never relaid
  out in place (AS-COPY); the owner may move *elements* in place under the lock (AS-INPLACE, r16) (OM:136). First foreign write = one stop.
- **CoW** words point at a shared immutable GC cell; any write first materializes a private flat butterfly tagged `(me,0)` (OM:144, I35 OM:247). GIL off
  this is cell lock + claim + DCAS even for the owner (DP:1982-1989).

#### A: 2.9 Deletion, quarantine

- Delete is cell-locked (L4, OM:180). It release-stores `undefined` (not the empty value) into the slot **before** the table edit (D1, OM:186):
  otherwise "a tardy lock-free reader surfacing encoded-0 = null-cell deref" (OMA:74-75).
- Every deleted out-of-line offset is quarantined and reused only after a collection *of the owning heap* has stopped the world (OM:184, OM:188). "a
  crossed epoch proves every mutator passed a safepoint after the deletion" (OMA:76-77).
- Soundness needs I34: no poll, allocation or park "between obtaining a PropertyOffset/slot pointer and the access, unless it re-validates
  structureID after" (OM:246), which required an audit of about 45 unowned files (OMH:878).

#### A: 2.10 Dictionaries, kind changes, structure-only transitions

- Spec: dictionary storage access holds the cell lock, table edits also the structure's lock (L3, OM:179). **Reality**: "§6's "dictionary readers
  are cell-locked (L3)" describes no reader in the tree" (OMH:2079-2080); readers resolve the name under the structure's lock or a lock-free
  seqlock probe and then read the slot. What makes that sound is the writer's order: value before the table entry (I42 amendment, OM:249).
- Flatten GIL off "is a structure-only transition to a fresh non-dictionary clone with the same table and offsets" (OMH:1683-1685): never renumber
  offsets under an unchanged StructureID.
- **L4-K**: data <-> accessor redefinition of an existing property keeps the offset and changes the slot's *kind*; done inside one stop (OM:180).
  Options weighed: fresh offset; a type check in every get; a stop (OMH:1096-1101).
- **Structure-only transitions** (seal, freeze, preventExtensions, setPrototype, private brand, dictionary conversion ...): planned from a structure,
  "published only if the structure it was planned from is still the object's, with its property table unchanged. Otherwise it is planned again"
  (Source/JavaScriptCore/runtime/JSObject.cpp:5999-6006; callers DP:4586).
- `setPrototypeOf`: cycle walk + install under one *process-wide* lock, acquired by a polling try-lock because the holder can request a stop
  (Source/JavaScriptCore/runtime/JSObject.cpp:4546-4590, 4669-4672).

#### A: 2.11 The per-object cell lock

- The existing 2-bit `JSCellLock`: bits 0x40 (held) and 0x80 (parked) of the indexing-type byte in the header (OMA:24, GT#6). "flat/segmented slot
  access NEVER takes it (except AS)" (OM:175).
- Takers: non-elided transitions, conversion, dictionary writers, delete and attribute change, all AS access (L1-L5, OM:177-181); the marker for AS
  butterflies (OMA:25); and, outside this spec, Map/Set writers, WeakMap, generators' multi-word cases, FunctionRareData (UG:840-868).
- Rules: order `SAL < JSCellLock < Structure::m_lock`; never allocate under it except with GC deferred *before* locking (O1); never reach a
  safepoint under it (O2); at most one per thread (O3); closures run inside a stop never allocate (O4) (OM:190-195).

#### A: 2.12 Structure transition table and property table (L6)

- Found only in the fourth whole-design review: with per-object ownership two threads run `addPropertyTransition` on one shared Structure lock-free,
  "racing unlocked lookup vs m_lock-held insert/rehash ... = UB, and steal + rehash vs unlocked `Structure::get` walk = UB"; "JSCellLock is
  per-INSTANCE and cannot serialize per-Structure state" (OMH:893-906).
- L6 (OM:182): (i) transition lookups through the locked `Concurrently` variant (single-slot case lock-free, DP:6341-6342); (ii) table steal / clone /
  materialize under the source's lock; (iii) uncached table walks hold the lock, later a two-sided seqlock: an edit count odd during an edit,
  readers probe with relaxed loads and the probe "never faults on torn data", then re-check; replaced index vectors are quarantined, not freed
  (Source/JavaScriptCore/runtime/PropertyTable.h:204-229, 244-255).

#### A: 2.13 TID allocation and rebias

- 15-bit TIDs, main = 0, `RangeError` at exhaustion (OM:29; SD9, UG:1030). Reuse without restamping = "false-owner E4/T1 (unsound)" (OMA:175-176).
- Rebias: "world-stopped INSIDE the next FULL shared collection"; restamp dead TIDs' tags to 0; trigger at 75 % of 2^15 (UG:447-455; UGH:3126-3140). A
  review found that compiled code with a baked `tid<<48` immediate survives reissue, so the same stop also fires sets and jettisons (UGH:3575-3612).

#### A: 2.14 Atomic slot operations (`Atomics.*` on properties)

Lock-free seq_cst 64-bit CAS on the slot for inline, flat and fragment slots; on a flat foreign word **first** set SW and re-validate, because "flat GROW =
butterfly-CAS + copy, NO nuke - an old-butterfly CAS is silently lost"; dictionaries and AS under the cell lock; Int32/Double arrays convert to
Contiguous on first atomic access ("raw-word CAS REJECTED") (UGH:1854-1888).

#### A: 2.15 GC visiting while mutators run

- The stock concurrent-marker protocol is reused: load raw StructureID (nuked -> `didRace`, revisit), load the word by dependency, visit, re-load and
  compare StructureID and maxOffset (OM:130). Never re-derive the shape from a fresh load inside the visit: it "could pair a NEWER structure ... with
  the caller's OLDER spine and value-visit raw double or uninitialized lanes" (CB.h:587-592).
- Segmented: mark spine, the aliased flat allocation's *base* (its only references are interior pointers, invisible to conservative scan, OMA:49-52),
  and every fragment; visit indexed slots up to the spine's vectorLength, not the length (CB.h:596-601).
- GIL off the marker value-visits Int32 lanes as defence: a missed copy site becomes "a mislabel, never an unmarked cell" (I41, OM:253).
- Quarantined slots stay visited (OM:186).

#### A: 2.16 Typed arrays and ArrayBuffers (UG section N.6)

Readers load length, check, then load base, unordered. So: detach publishes length 0 and a flag, keeps the base, and the memory goes to a quarantine retired
at the next stop; transfer = copy + detach of the source; shrink defers the page release; grow never moves the base (commit, then release-publish
the length) (UGH:2365-2407). The view keeps its vector on detach too (LP:466-470). In-place algorithms (`sort`) work on a private copy GIL off (UG:895-898).

---

### A: 3 + 4. Cost and root cause, mechanism by mechanism

Root cause classes: **(a)** JIT / speculation / profiling / inline-cache machinery; **(b)** the object model itself (remains for any compiled code and C++
runtime that share Structure + butterfly objects); **(c)** C++ runtime code. The classification and its reasoning are mine; the numbers are the documents'.
All numbers are Release, Linux x86-64.

| mechanism | measured cost | class | reasoning |
|---|---|---|---|
| Tag in the word: mask on read | +1 instruction per out-of-line read in FTL (`mov mask,r; and 8(obj),r`) (DP:6210-6211). Per tier, out-of-line read: LLInt +19, Baseline +12, DFG +7.2, FTL +3.9 per loop iteration, polls included (DP:6187) | (b) | The pointer shares its word with ownership state, so every consumer must strip it. AOT code pays it the same way. |
| Write predicate (owner or SW) | 9 instructions vs 2 per out-of-line replace in FTL (DP:6212-6215); out-of-line write: DFG +15.4, FTL +11.9 (DP:6190). In C++ the owner test is 6 instructions + 2 config-byte tests, up to 3 times per store (DP:2216-2220) | (b) | Needed because copy-growth loses a foreign store. The *elision* of part of it (E1/E2) is (a) and unavailable without code invalidation. |
| Tags in total (predicates + flag-on C++ bodies) | 1.047 of GIL on in instructions, whole process (PP:229); geometric mean 1.042-1.060, worst ai-astar 1.19 (DP:6142-6157); factor 1.05 of the 1.30 (LP:5059). Before G1, GIL on: JetStream 0.908 of flag off, code size delta-blue FTL 105 KB vs 87, cycles 1.33x (OMH:1947-1953) | (b) | This is the price of the ownership encoding with no thread ever spawned. |
| Claim (CAS on the StructureID) per add | most of class-ctor-4's 2.6x GIL on (OMH:1825-1826); 70 % of delta-blue's locked loads are in generated code, "about 7 % of delta-blue's GIL-off time" (PP:402-409); Baseline +20 instructions per add (DP:6204-6205) | (b) | Two words (header, slot/word) must change as one against another writer. Avoidable for objects that provably have not escaped (R-1, PP:400-425), an analysis an AOT compiler can do. |
| Locked instructions overall | 0.48 per 1000 instructions GIL off, 0.34 GIL on, 0.25 `main`; "At twenty cycles each" 4.7 % of `main`'s cycles (PP:217-219) | (b)+(c) | "instruction counts weigh [them] at one and the processor at twenty" (PP:284-285). Budget in cycles. |
| No cached transition at all (every add in C++) | "40x on object-creation loops" (OMH:1011-1012); LLInt +646 instructions per add with its cache disabled (DP:6202-6204) | (a) | The caches were multi-word metadata read without a lock. The lesson for AOT: whatever replaces an inline cache must be published as one word or be immutable. |
| Shape-keyed ownership (r12-r15) | 2M iterations: 8 ms on one thread, 1.9 s on two; literals 6.3 s (OMH:1119-1122); "230x-400x" (OM:33). After: 6.0 / 6.6 / 8.2 / 9.6 ms at 1/2/4/8 threads (PR:369-376) | (b) | Ownership state in metadata shared by all threads. Independent of any JIT. |
| Per-structure fire with no owner leg afterwards (r12-r16) | one foreign add made the main thread's own loop 20x slower, 31 -> 619 ms; with E4-C 88 ms (OMH:1387-1390, OMH:1454) | (a) mostly | The set exists so that claim-free inlined code is legal. Without such code there is nothing to fire. |
| F4 chain-fire | "the retirement of lock-free/cached transitions for the entire shape family (every descendant of `{}`)" (OM:171); no number | (a) | An optimization of stop counts. |
| Stop per shape relabel | pbkdf2/sha256 at 0.37-0.40 of flag off with one thread (OMH:1228-1230); a stress file 25x slower (OMH:1199); aes: 1.5 M stops a run, 6.5-6.7 s in the conductor, 56-77 -> 215-218 after T4-C (OMH:1598-1601); Basic: 840,000 one-element arrays, 3.7 s of stops in 5 s (LP:2958-2961) | (b); amplified by (a) | The unsafe pair (old shape, new storage) exists for any check-then-load reader. DFG hoisting of the check across polls and `Arrayify` conversions multiplied the events. |
| Double family (boxed doubles instead of raw) | navier-stokes 2.45x-2.68x instructions (OMH:2001; DP:130); 82 vs 31 instructions per kernel iteration (LP:5080-5081); ML 1.89x (OMH:2003); 2.9 points of time (PP:31) | (b) | Untyped 8-byte lanes whose meaning is in another word. |
| I41 lane check, second bounds check | array read FTL +6.7 per iteration (DP:6183); array-element-read 19 vs about 8 instructions (DP:2016-2017) | (b) | Consequence of in-place Int32->Contiguous and of bounding by the loaded storage's own vectorLength. |
| Array growth by copy | one event 978 vs 498 instructions; C++ part 960 vs 360; `trySetIndexQuicklyConcurrent` runs "3.00 per growth event" (DP:1918-1931); "none of that is the copy" (DP:1892). gbemu copies 1.1 GB vs 30 MB a run, 23 ms (LP:3090-3092) | (c) | Re-dispatch around one CAS. `main` copies block-allocated butterflies too (DP:1896-1899). |
| Append inside the vector | 27 vs 16 (index), 24 vs 15 (push) instructions per element (DP:1966) | (b) | Owner compare, `min(publicLength, vectorLength)`, poll. |
| Length CAS-max | 1.06-1.09x on a hole/push/pop loop after the sets fired; routing to the runtime instead cost 3.2-3.9x (LP:3275-3279) | (b) | Length and element are two locations. |
| Segmented arrays | 243.7 vs 19.0 instructions per element read, 555.9 vs 34.0 read+write (DP:2022-2023); intrinsic estimate "five to seven instructions more per access, and no hoisting of a base pointer" (DP:2175-2176); 15.9 % of a 16-thread benchmark in segmented push/length (docs/threads/FLAT-GAP-EVIDENCE.md:869-873); 1.63 M BadIndexingType exits in a slow mode (FLAT-GAP-EVIDENCE.md:1067-1070) | layout (b); the 13x is (a) | Optimizing tiers exit on a segmented word and the Baseline cache never settles (DP:2024-2030). "the break-even is two to three reads per element" (DP:2178). |
| Segmenting a property-only object | a worker reifying `String.fromCharCode` first made a 16-thread phase 3x slower, bimodal 1512-4746 ms (docs/threads/MAP-BIMODAL-EVIDENCE.md:255-268; SB:3555-3557) | (a) symptom, (b) lesson | "segmentation only buys concurrent INDEXED growth" (SB:3476-3477). Lazy reification of builtins by whichever thread comes first is a foreign transition on a hot shared object. |
| CoW materialization | 798 (GIL off) vs 486 (GIL on) vs 426 (`main`) instructions for four stores into a literal; four atomic RMWs (DP:1982-1990). GIL on before the owner leg: 1,518 M vs 607 M cycles (OMH:1921-1923) | (b)+(c) | Two materializers must not both publish. A claim-first owner leg would save about 150 instructions (DP:2206-2207). |
| ArrayStorage | shift-drain of 20,000: 1.5 ms flag off, 227.4 with AS-COPY, 27.3 with AS-INPLACE (PR:291-294); still O(n) vs O(1) | (b) | The O(1) form moves the header a stale reader is decoding. |
| Property-table walk under lock / seqlock | GIL on 3.4-4.3 % of FlightPlanner's instructions (OMH:1879-1881); GIL off 3.3 % of time over 18 tests, json-stringify-inspector 26 % (PP:269-272); `forEachProperty` 96 -> 573 samples (DP:6344-6347) | (b) | Shared mutable metadata. **Hotter without inline caches**: the original cost argument was "IC hits ... do not walk tables" (OMH:914-915). |
| Transition-table lookup under lock | json-parse-inspector 199 vs 49 samples, 42 % of its excess (DP:6341-6344) | (b) | Same. |
| Dictionary objects uncached | get 5.9x, put 7.5x with the flag on (DP:6992-6994) | (a) | Caches refuse structures whose table changes under one ID. |
| Global property put, implicit global, late `let` | 14x, 27x, 11.6x (DP:6975-6979) | (a) | Scope-resolution metadata is three fields read without a lock, so it is frozen after link (DP:6969-6973). |
| Generic out-of-line access helper | 3 calls = 111 of 127 extra instructions per `RegExp.exec` (OMH:1786-1789) | (c) | A fresh, unpublished object does not need the dispatch. |
| Fenced write barrier forced on | class-ctor loop 157 -> 86 ms when removed GIL on (OMH:1189-1191); 1.4-2.7x on barrier-heavy micros (DP:7540); put loop 33.6 vs 60.7 ms (PR:272-275) | heap | One flag meant both "publication fences" and "every barrier slow" (DP:7534-7538). |
| Allocation-profile report word | 4.6 % + 3.5 % of cycles at four threads, JIT code 31 % more cycles per unit (OMH:1797-1801) | (a) | A shared advisory word written per allocation bounces a cache line. |
| Flag-off residue of the object model | marker +13 instructions per visited cell, 8 from `visitButterflyImpl`'s mode flag (LP:5175-5179; OMH:2105-2109); `JSArray::tryCreate` 109 -> 124 (LP:5179-5180); 4,164 static tests of `useTaggedButterflies` (DP:75); `Structure` 128 bytes vs 112 (LP:4207) | (c) | "a hot leaf gains one to twenty tests of a Config-page byte ... and callers stop inlining it" (LP:5172-5174). |
| Quarantine, DCAS, cell lock on transitions, TID rebias, GC visit of spines | no isolated number found | | see section 6 |

Overall, for scale: JetStream medians `main` 347.3, flag off 345.4, GIL on 330.9, GIL off 265.9 (LP:3552). GIL off = 0.758 of `main`; "Every prototype and every
ceiling together (no polls in optimized code, untagged words) reach 0.832" (PP:21-22); "eleven points are in none of the switches" (PP:33). Scaling on
independent work: 3.1-3.3x at 4 threads, 5.1-5.7x at 8 (PR:316-318).

**What (b) leaves for a no-JIT engine, in one list**: (1) copy-growth vs foreign store (SW bit or an equivalent); (2) header and word must change together (claim /
nuke, DCAS, RESTART); (3) storage-then-meaning consistency for readers (shape vs lanes, structure vs offset, data vs accessor, length vs base); (4) slot reuse after
delete; (5) shared Structure tables; (6) old storage must stay readable and alive after replacement; (7) header bytes shared with GC and the lock. **What
disappears**: TTL sets and their stops, F1-F4, E1-E3, jettison of baked TIDs, allocation-profile feedback, deferred watchpoint claims (section 5, I-1a), frozen
metadata caches. **What gets worse**: everything inline caches were hiding, above all property-table and transition-table lookups.

---

### A: 5. Bugs found late that teach a lesson

| # | bug | how found | lesson | source |
|---|---|---|---|---|
| 1 | **seal vs delete**: "`Object.seal` read the structure, planned the sealed one, and stored it with no check, so it could undo a delete"; a deleted property came back with no value. "The audit of every such store found eleven more (a prototype change, a dictionary conversion, a private brand, `Object.assign`, and others)" | the runner had run only some test directories; 1 run in 7 | every "read structure, plan, store structure" is check-then-act; funnel all of them through one publish-if-unchanged helper | LP:142-152 |
| 2 | **Second delete from a cacheable dictionary aborted, with one thread**: the path "assumed that a cacheable dictionary's property table is always pinned" | Bun's test preload; "The threads corpus had no object with more than 64 properties and two deletes" | run the ordinary single-thread suites with the flag on | LP:153-163 |
| 3 | **Hang in Release only**: "The compiler moved the load of a nuked StructureID out of a spin loop" | first Release run | every field another thread can change is an atomic (relaxed) load in C++ | LP:133-136 |
| 4 | **Dictionary in-place add published the name before the value**; `JSON.stringify` dereferenced an empty value; on a reused slot a reader saw another property's value (`w1_180 -> "w0_206!"`). "the readers were the symptom; the writer was the defect" | amplifier: 1 crash in 410 runs; 16 in 2,400 pinned to one core, 0 in 12,000 on an idle machine | I9 (value before name) must hold for *every* add path; a spec sentence ("readers take the cell lock") that "describes no reader in the tree" hid it; refusing dictionaries in fast paths made it *worse* (13 of 60 vs 2 of 60) | OMH:2032-2089; LP:3652-3675 |
| 5 | **Multi-slot readers** (`{...o}`, `Object.assign`, rest, clone) stored an empty value read under one structure check; `Object.values` wrote past the end of its result after a racing add | Debug assertion dismissed as noise for a round; amplifier | one validation does not cover several reads; re-validate after the reads, write nothing before; a sweep that stops at the first family misses siblings (section 30 vs 41) | OMH:1760-1778, OMH:1849-1871 |
| 6 | **Data -> accessor redefinition**: a `GetterSetter` cell reached `jsAdd`, 10 of 10 SIGSEGV | mirror harness (a stock stress test on two threads) | do not change a slot's kind under an unchanged offset | LP:1668-1675; OMH:1085-1095 |
| 7 | **Still open**: a getter cache vs another thread's delete + re-add or data<->getter redefinition: "a garbage callee, a fault in `op_call`", 18 of 20 runs | reading, seventeenth round | the accessor read is two dependent loads (slot, then getter) after one check | AUD:2359 |
| 8 | **E4-C first cut, two aliasing holes**: growth re-loaded the word instead of using its snapshot; the converter stored into a fragment that aliases the live flat butterfly *before* its claim: `o.g` read the foreign thread's `o.f` value | Release mirror pass, 6 of 6 | once one party is lock-free, *every* writer must claim before touching shared storage; zero-copy aliasing makes "private" storage shared | OMH:1482-1501 |
| 9 | **Delete leg not updated for claim-first**: a release assertion that under the cell lock only GC/lock bytes move; owner adds + foreign delete aborted | mirror harness, 3 in 100 | changing who may write lock-free invalidates assertions in all other legs; a lost claim under the lock is a RESTART | OMH:1625-1647 |
| 10 | **Undecided relabel claimed without consulting the sets**: a foreign locked writer could lose a CAS it asserted | re-reading for r17; never observed | same | OMH:1342-1358 |
| 11 | **DFG/FTL element stores had no write predicate**: a foreign store did not set SW, "The owner then kept its lock-free copying resize ... so the store was lost" | audit row; 20 of 20 | the SW protocol is only as good as its least careful writer | LP:542-551 |
| 12 | **Length raised with a plain store** in generated code and in `pushInline`'s owner leg: lowered a racing raise and hid an element. "the owner is no exception" | end-of-round amplifier; 947 of 1,500 rounds in the new test | | LP:3267-3284 |
| 13 | **`butterfly()` on a segmented word**: the length "came from the memory just before the spine"; `Object.assign` copied no indexed property, `Array.from` returned an empty array | corpus with the verifier on | an accessor whose contract is "flat only" will be called anyway; more appeared in round 17 (`setLength`, for-of) | LP:625-655; LP:4987-4990 |
| 14 | **Collection under a cell lock**: the sparse-map add reports memory, which can conduct a collection, whose marker waits for that lock. Second shape: a nested locker's `DeferGC` *ended* under the cell lock | GIL-off JSC suite, 1 of 200 | "no allocation under the lock" must include anything that can trigger or release a deferred GC | LP:599-624 |
| 15 | **Flatten vs in-place kind change**: a flatten cloned the table between the two halves of a `defineProperty`; every reader then spun. Three rules, the third found only "after a plain corpus run under load" | amplifier, 1 in 20; 6 hangs in 720 | two locks (cell, structure) guarding halves of one logical edit | LP:2933-2945 |
| 16 | **Property-table rebuild race (open, mechanism confirmed)**: `materializePropertyTable` walks without the lock, then locks and publishes, and "never re-checks that the slot is still empty and the structure still unpinned": a thread does not see its own add; one offset issued twice. "ThreadSanitizer is silent because every access is under some lock: it is check-then-act across two lock holds" | 314,000 amplified runs, then a checked build: the window entered in 256 of 619 runs | | PP:1421-1458 |
| 17 | **Deleted slot reused inside a deferred watchpoint-fire window** (derived, not reproduced): optimized code that elided its check "survives stops", so the quarantine argument fails and an accessor can land in the slot | reading | (a)-only: quarantine by "one stop" assumes offsets are held only across poll-free windows | DP:4598-4622 |
| 18 | **Quarantine epoch was process-global**: an unrelated heap's collection released the slot | third whole-design review | an epoch proves something only about the threads that stopped | OMH:885 |
| 19 | **"Sibling's value" that was not an object-model bug**: a key-atom cache "verified the slot's atom ... and then RE-LOADED the slot on return" | 1 in 120 under load | wrong key + correct lookup looks exactly like slot confusion; take one snapshot, verify it, return it | OMH:961-1007 |
| 20 | **Typed-array detach cleared the base**: an access paired the old length with a null base; with the Gigacage on "it wrote at 4095 bytes past the start of the primitive cage" | CVE test with the cage off, 20 of 20 | | LP:465-483 |
| 21 | **N1-I follow-up**: an assertion that the replaced word is "empty or owner-tagged" fired because a butterfly-less word is no longer empty | mirror, 16 of 30 | | LP:1785-1794 |
| 22 | **Global declarations**: "checks for a clash, then adds; both threads passed the check" | fifth-round sweep, 5 of 20 aborts | | LP:1662-1667 |
| 23 | **Symbol-table lock held while allocating an error object** -> three-way deadlock with a stop | | no allocation under any lock another thread waits on without a safepoint | T/semantics/const-assign-throw-vs-scope-access-under-stops.js:2-10 |
| 24 | **Assertions that a structure cannot change mid-operation** (`JSON.stringify` fast path, IC condition just validated, watchpoint install) | Debug/Release mirror, amplifier | single-thread truths encoded as release assertions become aborts on legal programs | LP:1795-1814; LP:653-669 |
| 25 | **Lost store into a small typed array** when another thread reads `.buffer` (`slowDownAndWasteMemory()` copies without a stop): open | reading; 608 of 4,000 rounds in the interpreter | the same copy-then-swing hazard as butterfly growth, outside the butterfly | AUD:2360 |
| 26 | **Persistent wrong value** (about 1 in 100,000 amplified runs): not explained; every add and delete leg "cleared by reading" (a useful table of all legs and their store/claim order) | | | DP:4647-4678; LP:5225-5228 |

Design-review finds worth remembering (before code): 16-byte CAS faults on PA cells (OMH:336-353); no StructureID re-validation under the lock silently drops a racer's
property (OMH:355-365); foreign *readers* of AS never set SW, so "read-only sharing" is invisible to the writer (OMA:273-281); CoW had no rule at all (OMA:290-301).

---

### A: 6. Not found / not sure

- **Closure variables / JSLexicalEnvironment**: no rule in OM, OMA or OMH. Only: the N7 audit row "SymbolTable ... JSSegmentedVariableObject (own m_lock) ... PHASE-1-IN-TREE
  (pre-existing concurrent locks)" (docs/threads/SPEC-ungil-audit-N7.md:216), the visibility rule (JIT:214), and the planned conformance set (PP:1151). A grep for
  `useJSThreads|gilOff|isGILOff` in `runtime/JSLexicalEnvironment.*`, `JSSymbolTableObject.*`, `JSSegmentedVariableObject.*`, `JSGlobalLexicalEnvironment.cpp`, `JSScope.cpp`,
  `SymbolTable.*` returned nothing. I infer, and did not read anywhere, that environment slots were treated as fixed-size in-cell slots ("atomic for free", THREAD.md:13). TDZ
  (empty value) reads racing initialization: not found.
- **Publication of a fresh object by a plain store** (conformance item 6): called "the construct-then-publish class TSan cannot check" (PP:1162-1163). I found no invariant in OM
  that states it for x86-64 or arm64 beyond I9/I40.
- **The conformance (litmus) set** of PP:1146-1169 is a plan; I did not find such a directory in JSTests/threads.
- **No isolated measured cost found** for: the 128-bit CAS, the cell lock on transitions (only a 5 % item for all cell locks in one benchmark heading,
  docs/threads/GILOFF-TAX-EVIDENCE.md:131, body not read), the delete quarantine (memory or time), TID rebias, GC marking of spines and fragments, the seqlock probe versus a plain
  probe per lookup.
- **Whether rebias (Task 13) is fully implemented**: OMH:1145 mentions "the dead-TID restamp (Heap.cpp)" and a CVE test name `mc-val-tid-reissue-false-owner.js` exists (LP:485); I did not read the
  code.
- **Whether P1.8 (the fix for bug 16) landed**: LP:4031 only repeats the finding; rounds 12-17 say the threaded modes' open items are unchanged (LP:4210). I did not diff `Structure.cpp`.
- **F1/F2 stops in the final GIL-off C++**: taken from OM (rev 18/19 text) and CB.h comments; I did not trace `ensureSharedWriteBit` or `trySegmentedTransition` bodies beyond CB.cpp:1140-1185.
- **NaN canonicalization on racy Double stores**: only the hole = PNaN statements (OM:140) and "an impure NaN" as the hazard of misreading boxed lanes (OMH:1253-1254). No statement about user-visible
  NaN payloads.
- **Frozen text vs tree**: OM is "FROZEN rev 18" with later rounds patched in; at least three sentences were found false against the tree (T5, OM:126; L3 readers, OMH:2079; M8 on arm64, OM:206). CB.h:28-29
  still says "frozen rev 14". Treat OM as intent, DP section I's table (DP:4661-4676) as the closest thing to a description of the code.
- **FLAT-GAP-EVIDENCE.md, MAP-BIMODAL-EVIDENCE.md, SCALEBENCH.md, PERF-RESULTS.md** were read only in the cited parts. INTEGRATE-objectmodel.md, CVE-AUDIT*.md, TSAN-*.md, FUZZ.md were not read.
- **arm64**: every arm64 statement is the documents' reading; "nothing was built or run" (LP:5234).
- **Test headers**: about 30 of the 152 files in the six directories were sampled, header comments only.

---

## Appendix B. JIT, inline caches, watchpoints, stop-the-world, polls and the visibility rule

(Original title: Digest: JIT / inline caches / watchpoints / stop-the-world / safepoints in the earlier JS-threads work)

Source tree: `<threads-branch>/` (branch `sosuke/threads`). All paths below are relative to that root.
Every fact carries `path:line`. Text marked **[inference]** is my own conclusion for the no-JIT engine; it is not in the sources.

### B: 0. Vocabulary and status (needed to read the citations)

- Three configurations. *Flag off* = `useJSThreads` not set. *GIL on* = flag set, one thread of a VM runs JS at a time. *GIL off* =
  `useJSThreads() && !useThreadGIL()`, N threads run at once; "GIL-off is a MODE" and GIL on stays as the "fallback/bisection oracle"
  (`docs/threads/SPEC-ungil.md:15-20`).
- *Lite* (`VMLite`) = per-thread execution state. An *entered thread* "holds a VM entry token ...: registered VMLite + unique TID ..., GCClient::Heap
  ..., VMThreadContext/VMTraps ..., microtask + task queues" (`docs/threads/SPEC-ungil.md:24-28`).
- *Heap access* = per-thread (per GC client) flag: a thread that holds it may touch the heap; a thread that released it counts as stopped.
- *TID/SW tag* = owner thread id + "shared write" bit in the top 16 bits of the butterfly word (object-model work; consumed by the JIT spec, `docs/threads/SPEC-jit.md:95-101`).
- *TTL sets* = two new per-Structure watchpoint sets, `transitionThreadLocal` and `writeThreadLocal` ("all instances still thread-local", "no shared writes yet", `THREAD.md:15`).
- *STWR* = `JSThreadsSafepoint::stopTheWorldAndRun`; *NVS* = `notifyVMStop`; *GCL* = the heap's GC conductor lock (`docs/threads/SPEC-jit.md:3`).
- *Class A / Class B* = watchpoint sets whose fire may / may not invalidate machine code (`Source/JavaScriptCore/bytecode/Watchpoint.h:196-217`).
- Status: GIL off is **not** declared safe. `constexpr bool gilRemovalPreconditionsMetValue = false;` (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:299`); it runs only with `useThreadGILOffUnsafe`.
  The owner's first goal became "the branch built with the flag not set equals `main`" (`docs/threads/LANDING-PLAN.md:5037-5040`). x86-64 Linux only; arm64 does not compile in two places and has two unsafe orderings (`docs/threads/LANDING-PLAN.md:5234-5241`).
- Overall price (JetStream 2 score / `main`, quiet pass): GIL on 0.947, GIL off 0.758, GIL off with one parked Thread 0.743; every prototype and every ceiling
  (no polls in optimized code, untagged words) 0.832 (`docs/threads/PARITY-PLAN.md:158-165`). Instructions GIL off / GIL on 1.30 = "1.06 (the poll as an instruction sequence) x 1.05 (the butterfly tag) x 1.16 (what only a GIL-off process does)" (`docs/threads/DESIGN-PROPOSALS.md:120-123`).
- The original design note thought the JIT was a "smaller problem than it sounds" (`THREAD.md:17`). SPEC-jit then needed 12 frozen revisions and 59 history sections.

---

### B: 1. Inline caches, call linking, profiling, per-tier checks

#### B: 1.1 What was wrong

| Problem in stock JSC | Evidence |
|---|---|
| FTL property ICs repatch machine code (`RepatchingPropertyInlineCache`); `useHandlerICInFTL` was "FORCE-DISABLED 'not completed'" | `docs/threads/SPEC-jit-annex.md:20,23` |
| Handler chain publish is unfenced; JIT readers "load `m_handler` raw" | `docs/threads/SPEC-jit-annex.md:22` |
| Inlined fast-path state is two independent fields {offset, structureID}: "compare-then-load unsound, ARM64" | `docs/threads/SPEC-jit.md:36` |
| LLInt caches are multi-word metadata written in place; `try_get_by_id` "re-publishes id *before* offset — not a seqlock"; the mode byte is read before the word | `docs/threads/SPEC-jit-annex.md:28`, `docs/threads/SPEC-jit.md:58` |
| `DirectCallLinkInfo` with `UseDataIC::No` "still patches code", possibly from a compiler thread | `docs/threads/SPEC-jit-annex.md:29` |
| Guard/payload word pairs in call links: `reset()` nulls payload from running slow paths; direct fast path loads the target twice | `docs/threads/SPEC-jit-history.md:97-106` |
| Refcounts of handlers and stub routines are plain integers | `docs/threads/SPEC-jit-annex.md:30` |
| Tier-up triggering not serialized, the worklist does not dedup ("races corrupt `m_totalLoad`/queues in release") | `docs/threads/SPEC-jit-annex.md:27` |
| A retired handler/record may still be in a register of a thread that is about to use it | `docs/threads/SPEC-jit-history.md:41-45` |

#### B: 1.2 Mechanisms chosen

1. **No code patching outside a stop (I2).** "No tier modifies reachable machine code while >1 mutator may execute JS, except inside R1 STW" (`docs/threads/SPEC-jit.md:195`), release-asserted at every patch site (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:151-165`). Flag on: no `RepatchingPropertyInlineCache`, no `UseDataIC::No` direct call (I3, `docs/threads/SPEC-jit.md:196`).
2. **Handler (data) ICs in every tier**, FTL lowering completed (`docs/threads/SPEC-jit.md:80-83`). Node fields "frozen at publish"; `m_next` set once; reset "installs a fresh slow-path-only head; old chain retired ..., never inline" (`docs/threads/SPEC-jit.md:32`). Publish = `storeStoreFence()` then one pointer store; readers "address-depend through the head" (`docs/threads/SPEC-jit.md:76`, F1/F2 `:166-167`). Writers keep `CodeBlock::m_lock`.
3. **Single-word publication for inlined state.** `{byIdSelfOffset, structureID}` = one 8-byte-aligned atomic word; "readers ONE relaxed 64-bit load (compare id half, use offset half)"; invalidation = one all-zero store, "ABA-safe"; the GC write barrier is kept after the store; holder-bearing inlined forms disabled because they cannot pack (`docs/threads/SPEC-jit.md:37-41`). Rule F3: "no multi-store seqlocks in JIT-readable state" (`docs/threads/SPEC-jit.md:168`).
4. **LLInt metadata caches: one word or disabled.** "Survives IFF threaded form is exactly one such word; wider=>disabled (slow path, never caches)" (`docs/threads/SPEC-jit.md:45`). Disabled: prototype-load and unset `get_by_id`, `put_by_id` transition cache, private name/brand caches (table `docs/threads/SPEC-jit.md:47-56`). Charter for their return: "immutable single-ptr records" (`docs/threads/SPEC-jit.md:58`); never built.
5. **Call linking through one immutable record.** `CallLinkRecord {comparand, target, codeBlockToTransfer}`, "immutable after publish" (`docs/threads/SPEC-jit.md:144-150`). Fast path: load `m_record`, compare through it, store the CodeBlock, load target once, call (`docs/threads/SPEC-jit.md:152`). Every relink "publishes a NEW record, never mutates; stale read=complete OLD record, benign" (`:154`). Unlink = single null store (`:156`). Virtual calls: mirror / CodeBlock / mirror re-read recompare (`:158`). Polymorphic stub upgrade on callee tier-up = republish a copy, not an in-place slot rewrite (`:159`, `docs/threads/SPEC-jit-history.md:1739-1769`).
6. **Retired-artifact (epoch) reclamation.** `RetiredJITArtifacts` forwards to the heap's `safepointEpoch().retire()` (`docs/threads/SPEC-jit.md:62`). "**Hard rule: epoch expiry frees only non-executable data whose every JIT-side access is in a safepoint-free window** ... never machine code" (`:64`). Two obligations: (a) generated code holds no retired pointer across a safepoint: cooperative stops only and "no poll in an IC fast-path/handler-field window" (I16, `:66,209`); (b) native slow paths take a `Ref` first (I15, `:208`). Machine code is freed "only after R2's scan" = conservative scan of all N stacks incl. parked threads' registers (I7 `:200`, R2 `:249`). The epoch is bumped by GC stops only; a bump at a JSThreads stop was **refused** ("a non-GC bump reclaims against stale `m_localEpoch`s", `docs/threads/SPEC-jit-history.md:223`; CS4 `docs/threads/SPEC-jit.md:273`).
7. **Atomic refcounts**, later made mode-dependent ("Atomic when the flag is on, `main`'s plain count when it is off", `docs/threads/SPEC-jit.md:70`).
8. **Racy profiling: "tolerate, don't synchronize" except tier-up** (`docs/threads/SPEC-jit.md:131`). Value-profile buckets rely on aligned 64-bit word atomicity; array-profile merges are relaxed load + relaxed store, "lost bit benign"; multi-word Status snapshots stay under `m_lock`; invariant I12 "profiles select, guards validate" (`:135-138,205`). Tier-up: per-CodeBlock `m_tierUpInFlight` 0->1 CAS, worklist dedup (`:133-134`).
9. **Allocation profiles** are a two-word {allocator, structure} pair: readers do "structure, allocator, structure; slow path unless the two structure reads are equal and non-null"; the allocator is stored as a thread-independent table slot (`docs/threads/SPEC-jit-history.md:512-542`). All inline allocation GIL off resolves through the thread's allocator table (`docs/threads/SPEC-jit.md:112`).
10. **Megamorphic cache**: one cache per thread + one process-wide invalidation epoch (`docs/threads/SPEC-jit-history.md:943-977`).
11. **Global scope caches**: metadata frozen after link; later the GlobalProperty read cache came back with an ordered two-store publish, x86-64 only (`docs/threads/SPEC-jit-history.md:572-600`).
12. **Per-tier TID/SW predicates** through per-tier choke points (`docs/threads/SPEC-jit.md:106-108`). Read: segmented test, mask, no TID check. Write: segmented / owner compare / SW bit / slow path (`:100-101`). Transition: owner test + claim `lock cmpxchg` S -> nuked(S) in ICs; `CheckTransitionOwner` + four watched sets + `InvalidationPoint` in DFG/FTL (`:102`). Elision E1-E3 by watching the TTL sets; **writes never elide the owner compare** (D9: THREAD.md's full write elision "is UNSOUND", `:19,118`; argument: the mask "silently strips" a foreign tag so a foreign thread running shared compiled code would neither fire nor fault, `docs/threads/SPEC-jit-history.md:271`). "watchpoint validity is NOT a substitute for the runtime TID check" because "Baseline machine code is executed by every thread" (`docs/threads/SPEC-jit-history.md:249`).
13. GIL on with one owner later dropped all tag code (`useTaggedButterflies`): "every tier emits the flag-off form of every butterfly access" (`docs/threads/SPEC-jit.md:110`).

#### B: 1.3 Costs

| Item | Cost | Citation |
|---|---|---|
| FTL handler ICs instead of one patched stub per site ("walking a handler chain and clobbering every volatile register") | WSL +4..+9 %, Air +3.4 %, typescript +1.5 % instructions | `docs/threads/DESIGN-PROPOSALS.md:104` |
| Call-link records | "+5 instructions per non-inlined call (FTL), +6 LLInt, +8 `new`" | `docs/threads/DESIGN-PROPOSALS.md:107` |
| FTL loses DirectCall/CallFFI/CallWasm strength reduction | +27.7 instructions per call; FFI call 90 vs 19 | `docs/threads/DESIGN-PROPOSALS.md:108` |
| Disabled LLInt caches | "+690 / +627 / +646 instructions per operation in the LLInt"; first iterations GIL on scored 0.83-0.92 before they were given back there | `docs/threads/DESIGN-PROPOSALS.md:137`, `docs/threads/SPEC-jit-history.md:1531-1539` |
| Disabled cache read by the DFG as "never ran" -> forced exits | InadequateCoverage exits OfflineAssembler 82 -> 1,554 | `docs/threads/DESIGN-PROPOSALS.md:136` |
| Frozen scope metadata | put to a global property 14x, implicit global 27x; before §28 a global read was 50x | `docs/threads/DESIGN-PROPOSALS.md:109`, `docs/threads/SPEC-jit-history.md:576-580` |
| ICs refuse dictionary structures | 5.9x read, 7.5x replace | `docs/threads/DESIGN-PROPOSALS.md:110` |
| No transition IC at all (rounds 1-4) | "40x-1400x object-creation costs" | `docs/threads/SPEC-jit-history.md:455-460` |
| Per-access sequences GIL off | out-of-line write 9 instructions vs 2; constructor + six adds 77 -> 127 | `docs/threads/DESIGN-PROPOSALS.md:144` |
| Shared mutable profile/counter words under N threads | "equal instructions per thread at 1/2/4 threads, 3.5 times the cycles; Baseline 50 -> 260 cycles per back-edge from one thread to two" | `docs/threads/DESIGN-PROPOSALS.md:149`, detail `:5600-5635` |
| Locked instructions | GIL-off main thread 0.48 locked loads / 1,000 instructions vs 0.25 on `main` = 4.7 % of `main`'s cycles; "70 % of delta-blue's are in generated code" (claim CAS, length CAS) | `docs/threads/PARITY-PLAN.md:217-219,265-268` |
| Tags | 1.05 instructions; untagged everywhere = 1.4 % of cycles (3.5 % at most) | `docs/threads/DESIGN-PROPOSALS.md:121-123`, `docs/threads/PARITY-PLAN.md:210-212` |
| Flag-off residue of gates | 26,150 static tests of one Config byte, text 35.26 MB vs 30.50 MB | `docs/threads/DESIGN-PROPOSALS.md:73-76` |

---

### B: 2. Watchpoints

#### B: 2.1 The problem

Stock firing is "single-mutator-shaped ... no cross-thread sync", fire sites span ~20 files (`docs/threads/SPEC-jit-annex.md:25`). With N threads a fire means: other threads are *inside* optimized code that dropped the check. Two distinct hazards:
- the fire's handlers patch code other threads execute (I2);
- between "the fact became false" and "the code is invalidated" some thread runs the stale code. A queued/deferred jettison was evaluated and "REJECTED as unsound: the firing thread would proceed past the guarded mutation while its own speculatively-folded code is still installed" (`docs/threads/SPEC-jit-history.md:133-136`).

#### B: 2.2 Protocol (SPEC-jit §5.6)

- Interception is central, inside `WatchpointSet::fireAllSlow` / `InlineWatchpointSet::fireAll`, so no fire site needs an edit (`docs/threads/SPEC-jit.md:122`, `Source/JavaScriptCore/bytecode/Watchpoint.cpp:254-262`).
- **Class A (default)**: "(1) `worldIsStopped(vm)`->fire inline; (2) else `STWR(vm, closure)` (lock-free callers only); (3) re-check `state()==IsWatched`; (4) existing fire body; (5) jettisons in the same closure; (6) resume->fire COMPLETE. Synchronous completion load-bearing" (`docs/threads/SPEC-jit.md:124`). So the order is: stop the world, fire, jettison, resume.
- **No ">1 mutator" shortcut**: an inline fire with count==1 "races an attaching thread" (`docs/threads/SPEC-jit-history.md:225`, `Source/JavaScriptCore/bytecode/Watchpoint.cpp:459-462`).
- **Coalescing is required**: concurrent fires queue stack records; "the winner drains ALL queued fires in ONE stop" (`docs/threads/SPEC-jit.md:124`, `Source/JavaScriptCore/bytecode/Watchpoint.cpp:264-275,327-379`).
- **Class B (data only)**: the mechanism exists, the inventory is "EMPTY this pass — deliberately conservative ... every candidate set can carry code-invalidating watchpoints via adaptive/jettisoning watchpoint types" (`docs/threads/INTEGRATE-jit.md:1431-1437`). My grep of `Source/JavaScriptCore` found no `DataOnly` / `fireIsDataOnly` user outside `Watchpoint.{h,cpp}`.
- **Watcher-less Class A fire (r15)**: a set with no members flips state under the membership lock, no stop; `add()` takes the same lock and refuses an invalidated set (`docs/threads/SPEC-jit-history.md:914-924`, `Source/JavaScriptCore/bytecode/Watchpoint.cpp:463-486`).
- **Callers must hold no lock**: direct `fireAll` callers "REQUIRED lock-free w.r.t. every §7/cell lock"; a fire under a lock goes through `DeferredWatchpointFire` and fires at scope exit (`docs/threads/SPEC-jit.md:126`, `docs/threads/SPEC-jit-annex.md:35`).
- **Deferred fires** (structure transition sets): the state is claimed by CAS `IsWatched`->`IsInvalidated` before the membership transfer, because two mutators can reach the deferred overload unserialized (`docs/threads/SPEC-jit.md:126`, `docs/threads/SPEC-jit-history.md:409-443`).
- **Deferred claims in flight (tenth round)**: between claim and fire "the set reads `IsInvalidated` while code that elided checks on it still runs". (a) "*The loser is not benign*"; (b) "*The winner publishes before it fires*". Three memory-unsafe transition kinds (relabel out of Double, conversion to ArrayStorage, data<->accessor kind change) now: wait until no foreign claim is in flight, derive, and "in ONE stop: re-verify, publish, then fire" (`docs/threads/SPEC-jit.md:127`). The count is process-wide (`Source/JavaScriptCore/bytecode/Watchpoint.cpp:290-323`).
- Fire must come **after** publication for adaptive watchpoints: firing before publishing made the adaptive watchpoint "find the old, just-invalidated structure" and give the iterator protocol up (`docs/threads/SPEC-jit-history.md:1646-1653`).

#### B: 2.3 Kinds of set

| Set | Treatment | Citation |
|---|---|---|
| Structure transition set | the only set fired through a deferred object; consumers: DFG/FTL constants, DFG adaptive structure watchpoints, IC conditions, runtime caches (enumerators, cached `toString`/`valueOf`, realm protocol sets) | `docs/threads/DESIGN-PROPOSALS.md:4562-4572` |
| All 12 deferring sites with what stale code can read | every one "value-safe" or already one-stop, except a delete whose slot is reused | `docs/threads/DESIGN-PROPOSALS.md:4580-4596` |
| Property replacement (constant folding of a property value) | armed by ICs and scope caches on every cached structure, usually with no member -> watcher-less path; the source of the stop storms and of the watchdog deadlock | `docs/threads/SPEC-jit-history.md:903-912`, `docs/threads/STW-WATCHDOG-CLOSURE.md:14-21` |
| TTL sets (new) | fired under a per-event stop by the first foreign write/transition; "TTL fires assert branch 1" (already stopped); chain-fire F4 = one stop per transition chain | `docs/threads/SPEC-jit.md:125`, `docs/threads/SPEC-jit-history.md:338` |
| `havingABadTime` | not just a fire: "Class 4 - requires-stop ... writers that iterate/rewrite OTHER threads' objects"; "whole body under ONE §A.3 stop; conductor = caller"; allocation legal in that window | `docs/threads/SPEC-ungil.md:816-824,323-332` |
| Adaptive watchpoints | `install()` may find the structure changed by another thread between check and install: returns false flag on instead of asserting | `docs/threads/LANDING-PLAN.md:653-665,1695-1698,2653-2662` |
| InferredValue / single-instance inference | no special rule found; default Class A. Seen only as a cost ("nine `UnprofiledWatchpoint` jettisons - the single-instance inferences 'Allocated a scope' / 'Allocating a function'") and in one bug (fire in a GC end phase allocating a rare data) | `docs/threads/DESIGN-PROPOSALS.md:5508-5510`, `docs/threads/LANDING-PLAN.md:2656-2659` |
| First-use watchpoint installs in C++ | two first `Object.defineProperty` calls raced the install: "The install now runs inside a stop with a re-check" | `docs/threads/LANDING-PLAN.md:1653-1661` |

#### B: 2.4 How often, and what a stop costs

- richards-like GIL off, N=2: "total time in all 54 Class-A/§A.3 stop windows is 1.6 ms (~0.003% of wall); stop-fire count is constant (~36/run, warmup-phase one-shots ...) regardless of N and scale". The "watchpoint storm" hypothesis was "REFUTED" (`docs/threads/STW-WATCHDOG-CLOSURE.md:158-166`). Same later: `stwMs` 0.95 for a whole run (`docs/threads/DESIGN-PROPOSALS.md:5604-5605`).
- Before the watcher-less rule: string-heavy at four threads 376 stops per run -> 2; typescript GIL on 3,460 stop requests -> 171; the test 6,445 -> 0 (`docs/threads/SPEC-jit-history.md:907-929`).
- Object-model stops requested by JIT-planted `Arrayify`: Basic "up to 840,000 stops and 3.7 s of a 5 s run", sjcl tests 2,000-4,000 per run; fixed by making such sites generic (`docs/threads/SPEC-jit-history.md:1200-1214`).
- Raw rendezvous: `edenGC()` on an empty heap 0.27 ms alone, 0.38/0.41/0.50/0.57/0.93 ms with 1/2/4/8/16 spinning threads: "about 0.04 ms per running mutator per stop (trap, park, cooperative root snapshot, wake)" (`docs/threads/DESIGN-PROPOSALS.md:5167-5172`).
- **The indirect cost dominates**: one watched fire's stop bumps the heap-fact epoch, each parked thread jettisons its on-stack optimized code (3 per fire with four threads), and tier-up defects then keep all four threads in Baseline: string-heavy 2.7 s vs 3.4-6.7 s (`docs/threads/DESIGN-PROPOSALS.md:5569-5591`).

---

### B: 3. Code invalidation, OSR exit, code deletion, compiler threads

- **Jettison only world-stopped.** `CodeBlock::jettison` "runs only (a) on the JSThreads conductor inside an STWR closure, or (b) during GC, world stopped"; ditto `invalidateLinkedCode` and every `JumpReplacement::fire` (`docs/threads/SPEC-jit.md:87`, I8 `:201`). The function is the single choke point: "we become the conductor HERE" (`Source/JavaScriptCore/bytecode/CodeBlock.cpp:3073-3092`). So each reoptimization with N threads "is a stop each" (`docs/threads/SPEC-jit-history.md:1714-1715`).
- **Threads on the stack of the jettisoned block.** `invalidateLinkedCode` patches only invalidation points; "a mutator parked cooperatively at a trap-check poll inside DFG/FTL code resumes at that PC and runs the original stream until the NEXT invalidation point"; "N mutators make every park site a potential invalidation site" (`docs/threads/SPEC-jit-history.md:313`). Fix = I21(b): every DFG/FTL poll is immediately followed by an invalidation point (`docs/threads/SPEC-jit.md:214`, `Source/JavaScriptCore/dfg/DFGByteCodeParser.cpp:7994-8015`).
- **Cross-modifying code**: patcher flush, then ISB on each mutator leaving the park (F5, `docs/threads/SPEC-jit.md:170`); widened to threads that slept through a stop outside NVS via a process-wide stop-generation counter compared before JIT entry (`docs/threads/SPEC-ungil.md:257-265`).
- **Precise jettison (heap-fact rewrite epoch).** A stop may rewrite facts that parked code hoisted (haveABadTime, OM transition stops, debugger). Each such window bumps a process-global epoch *inside* the window; a mutator whose park overlapped a bump "jettisons its own on-stack optimizing-JIT code on resume", so it OSR-exits at the poll (`docs/threads/AUDIT-checktraps.md:33-44`, `Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:218-241`). Pure code windows (jettison, delete-all-code, heap walk) suppress the bump, else "every reoptimization would cascade into a process-wide on-stack jettison" (`docs/threads/AUDIT-checktraps.md:160`; users `Source/JavaScriptCore/bytecode/CodeBlock.cpp:3308`, `runtime/VM.cpp:1795`, `heap/Heap.cpp:956`).
- **Every other park site inside optimized code** (allocation slow paths, lock spins, tier-up checks) is made a whole-heap clobber by one predicate shared by clobberize and the abstract interpreter (`docs/threads/AUDIT-checktraps.md:201-203`); its `default:` arm takes every `doesGC`-true node (`Source/JavaScriptCore/dfg/DFGClobberize.h:194-199`).
- **OSR exit ramps**: flag off the first exit repatches its jump; GIL off nothing may repatch, so every exit went through the generation thunk (gbemu ~25,000 exits a run). Now a published exit vector / published ramp pointer (`docs/threads/SPEC-jit-history.md:1147-1170`).
- **Exit counting with N threads**: thresholds are multiplied by (1 + live threads) for every block (`docs/threads/DESIGN-PROPOSALS.md:5637-5645`); with one parked thread alive this alone costs 1.5 points of score (`docs/threads/PARITY-PLAN.md:168-172`).
- **A thread body is entered once**, so it regains optimized code only by loop OSR entry; after one jettison the way back costs `5,000 x s x (1 + N) x 4^r` Baseline back-edges (`docs/threads/DESIGN-PROPOSALS.md:5479-5521`). Private code scales 3.9x at four threads; one shared warmed copy does not (table `:5649-5662`).
- **deleteAllCode** GIL off: `whenIdle` decides under the registry lock, then decides again inside a stop ("a stopped thread that is entered is stopped inside JS, so the work waits for the next time the VM goes idle"), with collection prevented and JIT plans completed first (`Source/JavaScriptCore/runtime/VM.cpp:1774-1802,1833-1862`). RegExp code deletion has its own stop (`Source/JavaScriptCore/runtime/RegExpCache.cpp:159`).
- **Compiler threads.** Stops do not wait for them (`docs/threads/SPEC-jit-history.md:1425-1428`). They must not patch inline on seeing somebody else's window: inline execution "is licensed solely for the CONDUCTOR thread" (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:278-292`). GIL off a *synchronous* compile on a mutator is forbidden: it "holds heap access and reaches no poll until it is done, so every other thread's stop-the-world waits out the whole compilation" (`docs/threads/SPEC-jit-history.md:1425-1428`); a requested synchronous JIT became a parked wait on the concurrent one (`:1680-1695`). Epoch reclamation needs compiler threads suspended "by the reclaimer's own suspend/resume pair" (`docs/threads/SPEC-heap.md:163`).
- **One process-wide recursive "GIL-off compilation lock"** serializes CodeBlock creation, installation, CodeCache lookups and `Plan::finalize`; its contended path spins on `tryLock` and parks for stops, because "A thread blocked in a raw lock() is invisible to the gilOff stop protocol" (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:304-325`).

---

### B: 4. Stop-the-world

#### B: 4.1 Who can request

Any entered mutator, as *requester = conductor*: `work` runs "ON THE CALLER'S OWN STACK" (`docs/threads/SPEC-jit.md:228-238`). Requesters found in the source (watchdog context strings): Class-A fire (`bytecode/Watchpoint.cpp:422`), jettison (`bytecode/CodeBlock.cpp:3309`), OM transition stop (`runtime/ConcurrentButterfly.cpp:238`), delete all code (`runtime/VM.cpp:1796`), RegExp code deletion (`runtime/RegExpCache.cpp:159`), debugger walk (`debugger/Debugger.cpp:91`), heap walk (`heap/Heap.cpp:957`), wasm relocating grow (`wasm/WasmMemory.cpp:430`) (all under `Source/JavaScriptCore/`). The GC has its own stop, which "NEVER enters VMM's reason latch/dispatch" (`docs/threads/SPEC-jit.md:11`).

#### B: 4.2 Counting and the stop predicate

- First design counted VMs; that is wrong for N threads in one VM ("a JSThreads conductor could proceed while sibling threads run elided code", `docs/threads/SPEC-jit-history.md:336`). Re-frozen: "Counting unit = entered thread"; the entered set *is* the lite registry, re-walked under its lock on every sample, pointers "NEVER cached across samples" (`docs/threads/SPEC-ungil.md:226-236`, `Source/JavaScriptCore/runtime/VMManager.cpp:210-220,397-428`).
- Predicate: "every entered thread of the target VM — other than the conductor itself — is access-released (which subsumes parked ...) or not-entered" (`Source/JavaScriptCore/runtime/VMManager.cpp:430-452`).
- That is sound only with a **re-acquisition gate**: `acquireHeapAccess()`/attach "polls the lite's stop bit; set => park"; every park site polls after waking, before re-acquiring. Ordering: "fan-out stores, conductor samples + polls ALL seq_cst; acq/rel UNSOUND" (a Dekker pair) (`docs/threads/SPEC-ungil.md:243-256`, `Source/JavaScriptCore/runtime/VMManager.cpp:245-252`).
- "Entry during a stop parks" (`docs/threads/SPEC-ungil.md:285-286`).

#### B: 4.3 Conductor sequence

"release access -> arbitration on the park-aware pending-job-slot mutex -> (WINNER ONLY) Heap::JSThreadsStopScope (GCL) -> fan stop bits -> stop -> work -> resume -> drop scope -> re-acquire access" (`Source/JavaScriptCore/runtime/VMManager.cpp:222-230`). Details:
- Losers park access-released, so they count as stopped for the winner, then retry (`docs/threads/SPEC-ungil.md:266-273`). The earlier order "GCL then arbitrate" deadlocks (`Source/JavaScriptCore/runtime/VMManager.cpp:755-762`).
- A non-GC stop holds the GC conductor lock for its whole window, so a GC "can neither start nor be mid-cycle while the closure patches code" (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:59-65`); no GC may start inside a window (`docs/threads/SPEC-ungil.md:329-332`).
- Nested request on the conductor, or a request under a GC stop *that this thread conducts*, runs inline (R1.h). A caller whose only evidence is a GC stop conducted by someone else must queue for its own stop, because "the 'stopped world' can evaporate mid-`work`" (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:293-322`).
- The closure was specified allocation-free; the conductor later had to re-acquire its own heap access in the window because fire bodies take `DeferGC` and run write barriers (`Source/JavaScriptCore/runtime/VMManager.cpp:816-836`).
- Resume order: code fence, stop-generation bump, clear stop word (seq_cst), wake tickets, retire the trap bits with clear-recheck-restore (`Source/JavaScriptCore/runtime/VMManager.cpp:858-887`).

#### B: 4.4 How a thread parks

- **Cooperative only**: "park at poll sites, never async; load-bearing for §4.4a" (`docs/threads/SPEC-jit.md:245`). Signal delivery is off GIL off: "SignalSender never started; delivery = bit fan-out + poll sites + D9 quanta" (`docs/threads/SPEC-ungil.md:172-175`).
- Running JS: trap bit `NeedStopTheWorld` -> poll -> `operationHandleTraps` -> `VMTraps::handleTraps` -> release own heap access -> wait on a striped {lock, condition, generation} ticket, untimed, lost-wakeup-proof (`Source/JavaScriptCore/runtime/VMManager.cpp:271-312,479-507`). Parked threads publish their own stack/register snapshot for the conservative scan (`docs/threads/DESIGN-PROPOSALS.md:5171-5172`).
- As built the polls of every tier read the **VM-level** word, not a per-thread one (`Source/JavaScriptCore/ftl/FTLLowerDFGToB3.cpp:21461`, `llint/LowLevelInterpreter.asm:3014-3028`, `docs/threads/LANDING-PLAN.md:552-553`); the first thread to trap consumes the bit, so the conductor re-fires it on every non-quiescent sample (`Source/JavaScriptCore/runtime/VMManager.cpp:798-807`).
- **Blocked in Lock/Condition/join/Atomics.wait**: `GILDroppedSection` = "access release + §A.3 park cooperation + post-wake poll" (`docs/threads/SPEC-ungil.md:778-784`). These waits run in 10 ms quanta polling termination (D9, `docs/threads/INTEGRATE-api.md:1070-1086`) and call `parkSitePollAndParkForStopTheWorld` per quantum (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:208-216`).
- **Native code that keeps heap access** (spins on a nuked structure, lock waits, first-use initialization): must poll the stop word itself (`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:318-324,368-377`). "A spawned thread blocked in native code that holds heap access without a release/acquire bracket counts against this budget" (`Source/JavaScriptCore/runtime/OptionsList.h:787`).
- **Poll-free C++ regions must be bounded**: the Yarr interpreter has "zero CheckTraps/safepoint sites"; bound pinned by `static_assert(matchLimit <= 100000000)`; measured worst case ~1.8 s (`docs/threads/STW-WATCHDOG-CLOSURE.md:52-61`).
- C++ polls through `RETURN_IF_EXCEPTION`: one VM-level word in every mode, with a count field of lites that have a pending exception (`docs/threads/SPEC-ungil.md:198-218`).

#### B: 4.5 Deadlock rules

1. No stop request while holding any lock of the order `[GCL] > [STW ownership] > JIT worklist > CodeBlock::m_lock > IC state / cell lock > epoch retire lock, stub-routine/allocator locks` (`docs/threads/SPEC-jit.md:176-188`).
2. A thread blocked on an ordinary lock still holds heap access and is "counted non-quiescent ... forever" (`docs/threads/STW-WATCHDOG-CLOSURE.md:14-21`). Any lock whose holder can park or request a stop needs a try-lock + stop-poll acquisition (§3 last bullet).
3. Do not wait for another thread's progress from inside a stop: the claim wait is skipped in a stop since "a claimant can be parked by that very stop" (`docs/threads/SPEC-jit.md:127`).
4. Collect under a shared lock, drop it, then jettison: conducting a window while holding the codeBlockSet lock "deadlocks against siblings that take the same lock on their way to their park" (`docs/threads/AUDIT-checktraps.md:163`).
5. Two threads that each hold a claim and wait for the other: latent (`docs/threads/DESIGN-PROPOSALS.md:4638-4645`).

#### B: 4.6 Stop watchdog

`jsThreadsStopWatchdogMs` = 30,000 ms: a stop that has not completed "fail-stops and names the non-quiescent participants" (`Source/JavaScriptCore/runtime/OptionsList.h:787`, `Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:167-206`). It replaced an "unimplementable lock-rank RELEASE_ASSERT" (`docs/threads/SPEC-jit-history.md:130-132`). The budget became progress-aware (re-armed when another conductor completes a window; fresh budget per leg), because a live fire-storm queue had tripped it (`Source/JavaScriptCore/runtime/VMManager.cpp:663-700`). It is also the test oracle for "a poll was deleted" (`JSTests/threads/checktraps-invalidation.js:15-20`).

#### B: 4.7 Termination, script watchdog, time limits

- Spec: "Termination is VM-WIDE ONLY v1", no `Thread.prototype.terminate`; the bit is fanned to all lites under the registry lock plus the VM word, which late joiners OR in (`docs/threads/SPEC-ungil.md:158-171`). "EVERY entered thread takes its OWN close; the VM SURVIVES" (`:588-592`).
- Amended in the second round: a deadline (`VM::addTerminationDeadline`, `node:vm` timeout) terminates only the thread that added it. Because every tier polls the VM word, "The bit goes into both words, and the other threads ignore the VM word's bit while no VM-wide termination is raised" (`docs/threads/LANDING-PLAN.md:551-566`, `Source/JavaScriptCore/runtime/VMTraps.h:310-320`). Watchdog, SIGINT and worker `terminate()` stay VM-wide (`docs/threads/LANDING-PLAN.md:1062-1071`).
- `cancelTermination` must withdraw from every lite's word too (`docs/threads/LANDING-PLAN.md:352-355`, `JSTests/threads/giloff-cancel-termination.js:3-6`).
- Script watchdog and debugger bits are carrier-only: spawned threads never service them (`docs/threads/SPEC-ungil.md:184-197`, `Source/JavaScriptCore/runtime/VMTraps.h:199-209`).
- Whether a thread may block synchronously is a per-thread predicate (`docs/threads/SPEC-ungil.md:709-716`).

---

### B: 5. Polls and the visibility rule

#### B: 5.1 Where polls are, and their form

- Bytecode: `op_check_traps` after every `op_loop_hint` (`Source/JavaScriptCore/bytecompiler/BytecodeGenerator.cpp:1516-1520`). DFG/FTL also plant one at `op_enter` (`Source/JavaScriptCore/dfg/DFGByteCodeParser.cpp:8643`; table rows P1/P2 `docs/threads/AUDIT-checktraps.md:191-192`). `LoopHint` itself is not a poll (`:193`). The LLInt prologue polls through a trap-aware stack limit (`docs/threads/PARITY-PLAN.md:480-485`).
- Form: a 32-bit load of the trap word, test against `AsyncEvents`, branch: "on x86-64 `test dword [reg], 31; jnz`", invalidation point "as a zero-byte patchpoint at the rejoin" (`docs/threads/DESIGN-PROPOSALS.md:702-704`); arm64 three instructions (`:1140-1143`). A relaxed load is enough; "the handshake that matters ... is in the slow path under locks" (`:1140-1142`).
- `main`, flag off and GIL on have **no** polls in optimized code: traps arrive by signal + breakpoint patching, which with more than one running mutator is an I2 violation (`docs/threads/SPEC-jit.md:214`, `docs/threads/SPEC-jit-history.md:1558-1569`).
- A protected-page poll (one instruction) was rejected: "needs a fault handler that re-enters the engine at arbitrary prologue states and competes with the embedder's signal handlers"; kept as a contingency (`docs/threads/PARITY-PLAN.md:532-534`). A per-thread countdown was rejected: "5 cycles per iteration" (`docs/threads/DESIGN-PROPOSALS.md:1101-1102`). Removing polls from counted loops was rejected: "a 10^9-iteration loop would hold a collection for seconds" (`docs/threads/PARITY-PLAN.md:616-617`).
- The compiler must not delete polls: without the ordered `write(Watchpoint_fire)` "a second CheckTraps ... could be CSE'd away — a deleted poll is a missed safepoint" (`docs/threads/AUDIT-checktraps.md:91-102`).

#### B: 5.2 Cost of polls

| Measurement | Number | Citation |
|---|---|---|
| `main` with `--usePollingTraps=1` vs `main` | delta-blue -9 % score / +15 % cycles; richards -5 % / +13 %; ai-astar -6 % / +2 % | `docs/threads/SPEC-jit-history.md:1571-1574` |
| Per iteration, tight loops | +2.0 instructions on 4.7 (int loop, inline/out-of-line property read); time ratios 1.65-2.01 because "the poll adds a second branch and a load" to a one-cycle iteration | `docs/threads/DESIGN-PROPOSALS.md:714-726` |
| Most executed polls are not loop polls | 136 of delta-blue's 182 in-loop polls are entry polls of inlined callees; eight polls per iteration in `Plan.prototype.execute` | `docs/threads/DESIGN-PROPOSALS.md:732-736` |
| Share | 11.8 % of delta-blue's FTL instructions, 10.2 % of hash-map's | `docs/threads/DESIGN-PROPOSALS.md:738` |
| In **time**, once inlined-entry polls are gone | "Every remaining poll gone: 0.9 % of cycles. Every poll writing nothing: 0.8 %. ... No machine-frame entry poll: nothing measurable" | `docs/threads/PARITY-PLAN.md:210-212` |
| Lesson | "Cycles, not instructions ... polls, masks and compares retire beside the useful work" | `docs/threads/PARITY-PLAN.md:207-209` |
| Poll as a *barrier* | navier-stokes 31 -> 43 instructions per inner iteration, crypto `am3` +45 %, float-mm +10 % | `docs/threads/PARITY-PLAN.md:575-579` |

#### B: 5.3 The visibility rule, exactly

SPEC-jit I21 as amended in the tenth round (`docs/threads/SPEC-jit.md:214`):

> "what a GIL-off poll keeps from being hoisted or reused across it. In DFG plans and wherever no analysis is attached, the interim set of AUDIT-checktraps §7.1 (every user-visible value heap). In FTL plans a pre-pass attaches to each poll inside a loop the set of value heaps read by the backward slices of every Branch/Switch condition in that loop (through Phis, and through in-loop stores whose heap overlaps a collected read); the poll writes exactly that set, so a read that can decide control flow - a spin on a plain field, element, closure or global variable, a cancel flag tested with `if`, a loop bound - is performed again after every poll, and a read that only feeds data ... may be hoisted out of the loop and is then as stale as the loop is long."

Fallbacks to the interim set: a slice that reads the whole heap, a loop containing a whole-heap writer, a loop no Branch/Switch leaves, a poll outside any loop (same line). One-sentence forms: "every read that can decide a loop's exit is performed again every iteration" (`docs/threads/DESIGN-PROPOSALS.md:272`); guarantee kept: "no loop can be made non-terminating, and no `if` inside a loop can stop seeing another thread's plain write" (`docs/threads/SPEC-jit-history.md:1455-1456`).

The *interim set* = `NamedProperties`, `IndexedProperties` (incl. typed arrays, scope, arguments), `Butterfly_publicLength`, `Absolute`, Map/Set/WeakMap/WeakSet/internal/Date fields, `RegExpObject_lastIndex` (`docs/threads/AUDIT-checktraps.md:53-65,224-231`). Value **stores** also cannot sink below a poll (`:75-77`). **Shape facts** (structure, indexing type, type info; since §39 the butterfly in FTL) may cross polls because a stop that rewrites them forces an OSR exit at the poll (`:67-71`).

**Why it exists.** Once `CheckTraps` stopped being `read(World)/write(Heap)`, LICM could hoist a loop-invariant plain load, "so a spin-on-plain-flag loop (no Atomics) hangs — a user-visible semantic regression the old ... model de-facto prevented" (`Source/JavaScriptCore/dfg/DFGClobberize.h:896-905`). Plain `ArrayBuffer`s and ordinary objects are shareable here and "get no ECMA SAB memory-model cover" (`docs/threads/AUDIT-checktraps.md:72-75`). It "is about compiler motion, not hardware ordering: a hoisted read is stale by the compiler's doing on every architecture" (`docs/threads/DESIGN-PROPOSALS.md:1144-1146`). An experiment build with no rule: "the ping-pong test hangs, as it should" (`docs/threads/LANDING-PLAN.md:3820-3821`).

Precedent the other way: on stock `main` an FTL loop over a SharedArrayBuffer view "never sees another agent's plain store"; with `--useFTLJIT=0` it does. So `main`'s contract is "may be hoisted for the loop's lifetime; use `Atomics`", tier-dependent, and ECMA-262 allows it (`docs/threads/DESIGN-PROPOSALS.md:846-855`).

**What it costs.** Three builds, GIL off, instructions (G): delta-blue 7.196 (interim) / 7.222 (§50) / 6.101 (polls write nothing) = 0.845; hash-map 0.860; gbemu 0.939; Box2D 0.949; richards 0.960 (`docs/threads/DESIGN-PROPOSALS.md:782-794`). float-mm under the interim set: 174.3 G vs 116.5 G GIL on (`docs/threads/SPEC-jit-history.md:1438-1442`). A re-read pointer is a new SSA value, so its `CheckStructure`, `GetButterfly`, `GetArrayLength`, `CheckInBounds` run again (`docs/threads/DESIGN-PROPOSALS.md:749-752`). The §50 analysis gave nothing on object-heavy code because `PutStack` counted as a whole-heap writer (`:807-832`). In time: 0.8 % after D-B1..D-B4 (`docs/threads/PARITY-PLAN.md:1352`).

**Alternatives** (`docs/threads/DESIGN-PROPOSALS.md:271-292`, designs `:871-1122`):
- (c0) keep the promise + four refinements: D-B1 no poll at an inlined callee's entry; D-B2 only the loop-header poll refreshes; D-B3 one poll per trip of an unrolled loop; D-B4 `PutStack` is not a park site.
- (c1) D-B5 bounded staleness: the poll's fast path keeps nothing fresh; its slow path jumps to a *refresh pre-header* that re-executes the hoisted control reads; a ticker thread raises a no-op trap every 1 ms.
- (c2) D-B6 full release: plain reads may be hoisted for the loop's life; signalling needs `Atomics` or a lock; "works in the LLInt, Baseline and DFG tiers and hangs once the FTL compiles the loop".

**Decision.** No ruling by the API owner is recorded: "That ruling belongs to the owner of the threads API and has not been made" (`docs/threads/SPEC-jit-history.md:1444-1446`; "what remains for the spec owner (Jarred) is to RATIFY", `docs/threads/AUDIT-checktraps.md:231-235`). DESIGN-PROPOSALS recommended (c0) then (c1) (`:289-292`). PARITY-PLAN changed that after measuring: "**(c0) only, changed.** ... D-B5's phase, per-thread trap delivery and ticker are not worth that. The promise stays as tested since the interim default" (`docs/threads/PARITY-PLAN.md:1352`); "Decision 1 stays (c0): nothing to implement" (`:759`); (c2) is kept as a contingency if a gate is missed (`:1302-1304`). The owner "took this document's conclusion and set the order of goals" (`:9-13`). D-B1..D-B4 existed only as switches of a throw-away build (`:123-124`); the tree's `handleCheckTraps` still plants a poll for every `op_enter`/`op_check_traps` (`Source/JavaScriptCore/dfg/DFGByteCodeParser.cpp:7994-8015`).

Test: `JSTests/threads/jit/poll-visibility-control-reads-gil-off.js:3-32` (note `:16-22`: a function called once and spinning passes even with everything hoistable, since loop OSR entry has no pre-header).

---

### B: 6. §39, I41, and the storage-then-shape re-check

| Check | Protects against | Form | Cost | Citation |
|---|---|---|---|---|
| **§39: bound from the same butterfly's `vectorLength`** | A foreign thread converts a flat butterfly to segmented and grows it "with no stop and no epoch bump"; the segmented array's `publicLength` lives in the old flat header's slot, "so a hoisted flat base paired with a re-loaded publicLength could index past the flat allocation" (heap OOB) | FTL: every bound derived from a storage edge is `min(publicLength, vectorLength)` of that storage: extra `CheckInBounds(index, GetVectorLength(storage))`; pop/shift go to the runtime when the two disagree. Sound because "a flat butterfly's vectorLength never changes in place GIL off" and the conservative scan keeps superseded storage alive | 2-3 instructions per element access. It *replaced* re-loading {base, length, vectorLength} after each poll ("about 25 instructions on a 50-instruction loop body"); crypto 0.73 -> 0.89 of GIL on | `docs/threads/SPEC-jit-history.md:1095-1141`, `docs/threads/DESIGN-PROPOSALS.md:721`, `docs/threads/LANDING-PLAN.md:2975-2988` |
| §47: same clamp in C++ operations handed a storage pointer | An upstream vectorized `indexOf` re-read `publicLength()` in C++ and undid the clamp | `searchableLengthOfStorageFromJIT` | - | `docs/threads/SPEC-jit-history.md:1357-1379` |
| **I41: Int32 lane verification** | The owner may relabel Int32->Contiguous without a stop; a reader with a stale (separately loaded, possibly hoisted) Int32 shape check loads a non-Int32 JSValue; "a cell there would surface its low 32 bits as an integer" | DFG/FTL Int32-mode `GetByVal`/`ArrayPop` test the lane and exit (BadType); Int32-mode `ArraySlice` allocates a Contiguous result; C++ readers test `isInt32`; the marker value-visits Int32 lanes | one unsigned compare + branch (`(lane - 1) < NumberTag - 1`), earlier 7 instructions; "three tenths" of array-element-read's 1.75 | `docs/threads/SPEC-jit-history.md:602-621,1143-1145`, `docs/threads/SPEC-objectmodel.md:253`, `docs/threads/DESIGN-PROPOSALS.md:721-726` |
| **I16: pinned write predicate** | Predicate and store must see one word loaded after the last poll; a stop between them could flip ownership | tagged word re-loaded in a pinned patchpoint for each store | 9 instructions per out-of-line store vs 2 (6 fused) | `docs/threads/SPEC-jit.md:209`, `docs/threads/PARITY-PLAN.md:541-545` |
| **CAS-max length raise** | A plain length store in generated code lowers a length another thread raised; the element becomes a hole | load, done if already >=, 32-bit CAS, retry; lowers stay plain | 1.06-1.11x on hole/push loops (routing to the runtime was 3.2-3.9x) | `docs/threads/SPEC-jit.md:116`, `docs/threads/SPEC-jit-history.md:1295-1354` |
| **Storage-then-shape re-check (designed, NOT adopted)** | Encoding-changing transitions (Int32->Double, Double->Contiguous): a reader keyed on one shape must never decode storage encoded for another (raw double read as pointer) | Writer: copy, nuke, CAS word, publish header with one 64-bit CAS. Reader: "check, load, re-check: a sequence lock whose sequence is the header"; DFG node `CheckArrayAfterStorage(array, storage)` hoisted together with the butterfly | per butterfly load: a load, compare, branch, on arm64 a `dmb ishld` | `docs/threads/SPEC-objectmodel-history.md:1688-1745`, `docs/threads/DESIGN-PROPOSALS.md:1547-1557` |
| why not adopted | "the reader side is an audit of every lane decoder in the runtime ..., four JIT tiers, and a new DFG node - with the failure mode of a missed site being a forged pointer read" | preferred later variant W: a raw-double bit in the butterfly word ("No second header load, no fence") | what exists instead: Int32->Double served as Int32->Contiguous, copies of Double boxed, Double->Contiguous = a stop. "The Double family with its lane checks 2.9 %" of cycles | `docs/threads/SPEC-objectmodel-history.md:1747-1756`, `docs/threads/DESIGN-PROPOSALS.md:1559-1573`, `docs/threads/PARITY-PLAN.md:213` |

All of these are consequences of one fact: shape and storage are two separately loaded words, and a compiler may keep either across a point where another thread changes the pair without a stop.

---

### B: 7. Root cause per problem, and what survives without a JIT

Classes: (a) JIT / speculation / profiling machinery; (b) object model; (c) C++ runtime; (d) inherent to any multi-mutator GC'd runtime. The class column is **[inference]** throughout; the sources do not classify.

| # | Problem | Class | In a no-JIT, no-profiling, no-watchpoint-speculation engine |
|---|---|---|---|
| 1 | Patching reachable code: repatching ICs, direct-call patching, jump replacement, trap breakpoints (I2/I3) | a | Vanishes. |
| 2 | Cross-modifying-code fences, ISB at resume, stop-generation compare | a | Vanishes. |
| 3 | Freeing machine code only after an N-stack conservative scan (I7/R2) | a | Vanishes for code. The N-stack scan itself stays (d). |
| 4 | Jettison under a stop, invalidation point after each poll, OSR exit ramps | a | Vanishes as mechanism. **But it was the mechanism that let shape facts cross polls** (row 20). |
| 5 | Tier-up CAS, worklist dedup, compilation lock, sync-compile ban, loop OSR entry, 4^r back-off, thread multiplier | a | Vanishes. Lazy per-function linking state, if any, is (c). |
| 6 | Racy value/array/arith profiles, execution counters, call-slot counts (correctness and false sharing) | a | Vanishes with profiling. General lesson stays: any word written per execution by all threads costs ~5x (`DESIGN-PROPOSALS.md:5633-5635`). |
| 7 | Allocation-profile torn pair; allocators baked into code | a+d | The profile vanishes. Per-thread allocator lookup from compiled code stays (6 extra instructions, `DESIGN-PROPOSALS.md:144`). |
| 8 | Class-A fires, coalescing, watcher-less fires, deferred claims / precondition 10, adaptive re-install races | a | Vanish **for compiled-code consumers**. |
| 9 | Watchpoint sets consulted by C++ caches and fast paths (cached enumerators, special-property caches, iterator/species protocol sets; `DESIGN-PROPOSALS.md:4572`); first-use installs (`LANDING-PLAN.md:1653`) | c | Stay if the C++ runtime keeps them. They become data-only validity flags; check-then-use against a concurrent invalidation needs its own argument. The earlier work never exercised Class B, so there is no precedent. |
| 10 | TTL sets + E1-E3 elision + lock-free owner transitions licensed by them | a on b | The elision vanishes: AOT code pays the full predicate always, i.e. the "floor" (tags alone 1.047 of GIL on, `PARITY-PLAN.md:226-229`). Whatever told foreign threads "fire first" must be replaced by a run-time test; D9 already made the owner compare the sole detector for writes. |
| 11 | Handler chain publication, single-word {structureID, offset}, call-link records, epoch retirement of IC data, atomic refcounts, I15/I16 | b/d, not a | **Stays if AOT code has any mutable per-site cache.** These rules are about publishing mutable data to lock-free readers, not about code generation. §4.3 (one aligned word or nothing) and §5.8 (one pointer to an immutable record) are the reusable patterns. With no per-site cache they vanish, at the measured price of the disabled LLInt caches. |
| 12 | IC case validity vs another thread's delete/re-add or data<->accessor change (O17-1, open) | b | Stays for any cache keyed on structureID whose payload is used after the check. |
| 13 | Megamorphic / global-scope / concat-key caches | c | Stay: per-thread copy + process epoch, or ordered publication. |
| 14 | TID/SW predicates, segmented dispatch, transition claim, AS-COPY, slot quarantine | b | Stay unchanged. |
| 15 | Structure->butterfly load ordering (R7), inline-slot ordering on arm64 | b | Stays; the AOT compiler must emit the dependency or acquire. |
| 16 | §39 same-snapshot bound, §47 clamp in C++ | b + compiler | Stays wherever base and length can come from different moments (hoisting, CSE, passing a storage pointer to C++). |
| 17 | I41 lane check, Double family, storage-then-shape | b | Stays whenever compiled code or C++ decodes lanes by a shape read separately. An engine that never types a lane from the shape alone (as LLInt/Baseline, `SPEC-jit-history.md:615-617`) needs no I41; raw-double lanes remain the hard case. |
| 18 | CAS-max length raise | b | Stays in compiled code and C++. |
| 19 | `haveABadTime` | b/c | The watchpoint half vanishes; "rewrite OTHER threads' objects" under one stop stays, and compiled code must tolerate it after each park site (row 20). |
| 20 | Facts held across a park site that a stop may rewrite (indexing type, butterfly layout, property kind) | d + compiler | **Stays, and gets harder**: with no OSR exit there is no "exit at the poll". Options seen in the sources: treat each poll and each parkable call as a clobber of those facts (the original `read(World)/write(Heap)`: int loop 1.7x, closure-call loop 2.6x instructions, `SPEC-jit-history.md:495-501`), or D-B5's shape, where only the poll's *slow path* re-enters through a refresh pre-header. D-B5 needs no code invalidation. |
| 21 | Safepoint polls in compiled code | d | **Stay**: loop back edges + function entries (or the stack check, T-1), bounded poll-free paths. Signal + patch delivery is not available with N mutators. |
| 22 | Visibility rule | d + compiler | **Stays**: it is a rule about LICM/CSE, and an AOT compiler does both. A choice among (c0)/(c1)/(c2) is still needed. One argument changes: with a single tier, (c2)'s "hangs once the FTL compiles the loop" tier dependence disappears (behaviour is uniform, though still a hang). |
| 23 | Poll-free windows (I16): predicate -> store, record load -> call, claim -> publish | b/d | **Stay**: the object-model proofs lean on "no poll/allocation between the word load and the last store" (`SPEC-jit.md:102`). The AOT compiler must not schedule a poll, call or allocation into them, nor reuse a predicate across a poll. |
| 24 | Parkable slow paths (allocation, lock spins) = park sites | d | **Stay**: each call that can release heap access is a poll for the purposes of rows 20, 22, 23. |
| 25 | Stop protocol: entered-thread count, access release, seq_cst re-acquire gate, entry parks, conductor arbitration, GC/non-GC serialization, nested and evaporating evidence | d | Stays for the GC. Non-GC requesters shrink to OM transition stops, haveABadTime, heap walks, debugger. |
| 26 | Stop request under a lock; blocked lock waiters hold access; bounded poll-free C++ (Yarr) | c/d | Stay. |
| 27 | Stop watchdog | d | Stays; cheap and it found real bugs. |
| 28 | Termination VM-wide vs one thread, cancel, quantum polling in blocking waits, carrier-only watchdog/debugger | d | Stay. |
| 29 | Per-thread vs VM-level trap word | d | Stays as a design choice; a per-thread word avoids the re-fire and "ignore the VM bit" workarounds (§4.4, §4.7). |
| 30 | Flag-off cost of mode gates | build | Depends on whether one binary must serve both modes. |

Rules that apply to AOT-compiled code, in one list **[inference]**:
1. Poll on each loop back edge and on each non-inlined entry; never let CSE, unrolling or DCE remove the last poll of a cycle. Inlined-entry polls and per-unrolled-copy polls are pure waste (D-B1, D-B3).
2. The poll is load + test + branch on a word; its time cost is under 1 % once redundant polls are gone. What costs is what the poll is modelled to clobber.
3. Decide the visibility contract first; it fixes the poll's clobber set. If the slow path is an ordinary opaque call inside the loop, a generic compiler will treat all escaped memory as clobbered: that is the pre-de-jank model, stricter than the interim set.
4. Each fact a stop may rewrite is clobbered by each park site unless a slow-path refresh exists.
5. {base, length} must come from one snapshot, or the bound from the base's own immutable extent.
6. A shape check does not type a lane read later unless no encoding-changing transition can intervene without a stop.
7. Owner/SW predicates are valid only until the next park site and must sit in the same poll-free window as their store.
8. No stop request, allocation or blocking lock inside a poll-free protocol window.

---

### B: 8. Late bugs that teach something

| # | Bug | Found | Lesson | Citation |
|---|---|---|---|---|
| 1 | `tryCachePutBy` fired a property-replacement set while holding `CodeBlock::m_lock`; siblings blocked on the lock held heap access: 2-4 party deadlock, 30 s abort. 8-84 % of runs; "two threads suffice" | scaling bench | The one call site "the Task-11 audit table missed". A hand audit of fire sites is not enough; the watchdog made it diagnosable. Not JIT-specific in shape | `docs/threads/STW-WATCHDOG-CLOSURE.md:14-48,86-99` |
| 2 | Heap-fact epoch bumped at publication only: victims parked *by* the window sampled after the bump and "compared equal on resume, deterministically"; a third conductor entry point was missed one round later | review | Sample/compare epochs need the bump inside the stopped window; enumerate every entry into the conductor | `docs/threads/AUDIT-checktraps.md:157` |
| 3 | The "benign loser" of a deferred claim: thread B finds the set already invalidated, publishes, returns into its own not-yet-invalidated code: a GetterSetter read as data, a raw double stored into a Contiguous array | tenth round, only by a harness that runs each file on two threads started together | "Invalidated" is not "fired". A state flip that precedes its effect is a window for *every* thread, the one that lost the race included | `docs/threads/SPEC-jit-history.md:1608-1644` |
| 4 | First fix of #3 (fire before publish) broke adaptive watchpoints; second fix hung three `having-a-bad-time` files (waiting inside a stop for a parked claimant) | own battery | Order constraints come from both sides; never wait for a mutator from inside a stop | `docs/threads/SPEC-jit-history.md:1646-1653`, `docs/threads/LANDING-PLAN.md:3701-3706` |
| 5 | Generated length stores stayed plain after the C++ writers became CAS-max: an array ends one element short. The first fix dropped hole profiling and made FTL code lose the length in ~55 % of rounds; the third amendment found the runtime's own `pushInline` wrong too | ninth round, amplifier (11 of 8,000 runs) | A protocol change in C++ must be applied in every tier and vice versa | `docs/threads/SPEC-jit-history.md:1295-1346` |
| 6 | An upstream vectorized `indexOf` "merged without a textual conflict and the combination undoes §39's clamp" | audit after a rebase | Invariants living in emitted code are invisible to merges; C++ helpers taking raw storage pointers inherit compiled-code staleness | `docs/threads/SPEC-jit-history.md:1357-1371` |
| 7 | `create_this` read {allocator, structure} torn once the GIL-off allocator became non-null: SIGSEGV; "a size mismatch would have been silent corruption" | amplifier | A comment ("a torn pair just takes the slow path") was true of one field only | `docs/threads/SPEC-jit-history.md:525-542` |
| 8 | FTL direct tail call with a data IC clobbered an argument register: completed generators "silently resurrect"; the wild jump had been "misattributed to a §4.4 epoch race (it reproduces with zero threads)" | review | Turning on a code path upstream never runs brings single-threaded bugs; suspect them before the concurrency protocol. Same class: FTL handler IC for `delete`/`instanceof` had no unwind entry | `docs/threads/SPEC-jit-history.md:373-407`, `docs/threads/LANDING-PLAN.md:316-318` |
| 9 | A record's `codeBlockToTransfer` named a dead CodeBlock after resume (unlink ran in a lazy sweep). First fix: pin as GC root -> retention, idle code aging never fired. Final: unlink dead blocks' incoming calls in the End phase, world stopped | W=16 scaling run; later an upstream test | Close the window where it opens rather than extending lifetimes | `docs/threads/SPEC-jit-history.md:1219-1268` |
| 10 | Polymorphic stub republication ran on a thread doing collector work and "pushed a dead CodeBlock onto the mark stack" | GIL-off suite, deterministic | Publication helpers need to know whether they run inside a collection | `docs/threads/SPEC-jit-history.md:1759-1767` |
| 11 | F28: a jettison from the GC conductor spun on the compilation lock whose holder was parked for that very stop (the exemption tested the wrong "stopped" flag) | Debug corpus, 1 in 8 | Several notions of "world is stopped" coexisted (five disjuncts); each exemption must use the right one | `docs/threads/LANDING-PLAN.md:2328-2339`, `Source/JavaScriptCore/bytecode/JSThreadsSafepoint.h:102-109` |
| 12 | Acting on a GC stop somebody else conducts: the stop "can evaporate mid-`work`" | CVE corpus | "Stopped" evidence is valid only for the thread that ends the stop | `Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:293-322` |
| 13 | A disabled LLInt cache is read by the DFG as "never ran" -> forced exits; the heuristic fix made richards-like 5.6 s -> 56 s at four threads and was withdrawn; the real cause was a loop-entry accounting defect | tenth/eleventh | Switching a cache off also switches off whatever used it as a profile; a slowdown after a fix may expose an older defect | `docs/threads/SPEC-jit-history.md:1698-1736` |
| 14 | Value-profile write avoidance was compiled out by `#if USE(JSVALUE64)`, a macro upstream had removed: 67 unconditional shared stores | eleventh, annotated disassembly | Verify an optimization in the emitted code, not the source | `docs/threads/DESIGN-PROPOSALS.md:5617-5623` |
| 15 | `PutStack` (a frame store) wrote the whole heap through a conservative `default:` arm, so the §50 analysis gave up on 126 of delta-blue's 182 in-loop polls; docs claimed the opposite | eleventh, graph dumps | A conservative default silently defeats a precise analysis; count how often the fallback is taken | `docs/threads/DESIGN-PROPOSALS.md:807-835`, `docs/threads/AUDIT-checktraps.md:203` |
| 16 | §41: "no test could show a before/after difference", idea withdrawn; later measurement contradicted it (3 on-stack jettisons per fire) | eighth vs eleventh | A null result needs a positive control | `docs/threads/SPEC-jit-history.md:1172-1193` |
| 17 | The LLInt prologue's tier-up took the executable's *current* CodeBlock (already the optimized one) and gave it Baseline code | 32-thread test | "Current X of the shared object" is not "my frame's X" | `JSTests/threads/giloff-prologue-tiers-up-frame-code-block.js:2-8` |
| 18 | Linking a call while trap handling jettisons on-stack code: "the link finds the CodeBlock it read jettisoned" | test | The on-stack jettison must honour the same deferral as traps | `JSTests/threads/giloff-link-call-defers-on-stack-jettison.js:2-7` |
| 19 | A `node:vm` timeout terminated every thread; `cancelTermination` cleared one word of two; "requested" and "handled" shared one flag | Bun, audit | One-thread termination needs request and handled state per thread | `docs/threads/LANDING-PLAN.md:352-355,551-566` |
| 20 | Watchdog false positive: try-lock arbitration has no queue position, so a starved loser's budget expired "with the system fully live" | tier-forced storm | Distinguish queue from wedge: progress-aware budgets | `Source/JavaScriptCore/runtime/VMManager.cpp:683-700` |
| 21 | VMManager counters: a VM constructed during a stop is counted stopped but not active | Bun workers under load | Census by "entered" while participation is not keyed on it | `docs/threads/DESIGN-PROPOSALS.md:4712-4742` |
| 22 | I-1a: a deleted slot's quarantine can expire inside a claim window, since the claimant parks for a GC before its own stop | by reading, not reproduced | "One stop flushes every holder" is false for holders that survive stops (elided checks) | `docs/threads/DESIGN-PROPOSALS.md:4598-4622` |
| 23 | **Open** O17-1: a getter IC vs another thread's delete + re-add or data<->getter redefinition: garbage callee, fault in `op_call`; 18 of 20 runs at the Baseline cap | seventeenth round | The data-IC layer is still not safe against kind changes; not a code-patching problem | `docs/threads/AUDIT-upstream-since-rebase.md:2359` |
| 24 | **Open** F30: FTL reached a block emitted as unreachable, three threads at once, during a buffer transfer; hypothesis: "An abstract-interpreter proof taken from concurrently mutable state ... without a watchpoint" | amplifier, 2 in ~3,000 | Compile-time proofs from mutable heap state need a guard. An AOT compiler proving facts from a heap snapshot would share this | `docs/threads/LANDING-PLAN.md:2353-2364` |
| 25 | D9: THREAD.md's "elide TID/SW checks entirely" was unsound for writes | spec review round 3 | Shared compiled code runs on every thread; a watchpoint says nothing about *which* thread executes | `docs/threads/SPEC-jit-history.md:271` |
| 26 | Budgeting in whole-process instructions overstated polls and tags (a fifth of the distance) vs time (three points) and hid locked instructions | eleventh | Measure main-thread cycles; "instruction counts weigh [locked instructions] at one and the processor at twenty" | `docs/threads/PARITY-PLAN.md:27-38,282-285` |
| 27 | Blind spots: "ThreadSanitizer cannot see generated code, no amplifier site exists in the object model or the code life cycle, no fuzzer has run since June" | plan | AOT code compiled by a normal toolchain could be TSan-instrumented **[inference]** | `docs/threads/PARITY-PLAN.md:61-64` |

---

### B: 9. Not found / not sure

- **No recorded owner ruling on the visibility rule.** I found only "has not been made", a recommendation, and the plan's assumption (c0). The owner's acceptance is of the plan's conclusion and goal order (`docs/threads/PARITY-PLAN.md:9-13`), not of decision 1 by name.
- **Per-thread trap word.** SPEC-ungil A.2.1 says generated code reaches `lite->traps.m_trapBits` (`docs/threads/SPEC-ungil.md:152-154`); the FTL and LLInt polls I read test the VM word. I did not read the Baseline or DFG emitters. `VMManager.cpp:768-771` says the per-lite bits "ALIAS the single VM-wide trap word", while `SPEC-ungil.md:198-218` and `LANDING-PLAN.md:352-353` describe lites with words of their own. I did not resolve which comment is current.
- **Doc conflict**: `SPEC-ungil.md:164` and `VMTraps.h:293-297` say termination is VM-wide only; `VMTraps.h:310-316` and `LANDING-PLAN.md:551-566` describe targeted termination. I take the latter as current.
- **Function-entry poll**: `DESIGN-PROPOSALS.md:705-707` says the bytecode generator emits a poll at each function entry; in `BytecodeGenerator.cpp` I saw `emitCheckTraps()` called only from `emitLoopHint` (`:1519`); the DFG parser plants the entry poll at `op_enter`. How Baseline polls at entry I did not read (PARITY-PLAN says "Baseline keeps its explicit `op_check_traps`-free prologue and its loop polls", `:490-491`).
- **Latency from stop request to last park**: no distribution found. PARITY-PLAN lists measuring it as a *first act* of a package not carried out (`:595-598`). J12's 0.04 ms per mutator is labelled "indicative".
- **`fires-per-sec.js` and `int-gate-stop-budget.js` results**: I read the headers only; the stop-count hooks were "requested" there (`JSTests/threads/jit/int-gate-stop-budget.js:16-19`). I did not look for recorded numbers.
- **InferredValue, `InferredType`-like sets, symbol-table (`varInjection`, `varReadOnly`) sets**: only listed as fire sites (`docs/threads/SPEC-jit-annex.md:13-16`, `docs/threads/INTEGRATE-jit.md` direct-fire table after `:1449`); no dedicated protocol found.
- **Which watchers the fired property-replacement sets have** in string-heavy's slow mode: open in the source itself (`docs/threads/DESIGN-PROPOSALS.md:5593-5595`).
- **History annexes** cited as BINDING by SPEC-ungil (SB1, EXIT1, HBT2-4, ISB1, TERM1, W) live in `SPEC-ungil-history.md`; I did not read them. My statements on those protocols come from the compressed spec text and the `VMManager.cpp` comments.
- **Not read**: `SPEC-jit-history.md` §30-§35 and §38 beyond their headings or first lines (megamorphic GIL on, reallocating transitions, virtual calls, dictionary flattening, Map/Set probes); `VMTraps.cpp` beyond a grep of `handleTraps`; `RetiredJITArtifacts.{h,cpp}`; `jit/` emitters; DESIGN-PROPOSALS sections A, L, M bodies (only their summary rows); most of `JSTests/threads/jit/` (12 headers sampled).
- **arm64**: nothing was built or run there by the earlier project; every number above is x86-64 Linux Release.
- **Rounds 12-17** worked mainly on flag off. I did not check whether any GIL-off JIT rule changed after the tenth round beyond `SPEC-jit-history.md` §59 and AUDIT section 14.
- The other open GIL-off rows of the last audit (O17-2..O17-8, `docs/threads/AUDIT-upstream-since-rebase.md:2360-2366`) are object-model / typed-array / API items; only O17-1 and O17-8 (the FTL has no segmented-aware forms and recompiles again and again) touch this digest's area.

---

## Appendix C. Heap and garbage collection

(Original title: Digest: the GC / heap part of the earlier JS-threads project (branch `sosuke/threads`, HEAD `2b14c39e3638`))

Read-only digest. Every citation is `path:line` relative to the WebKit root (`<threads-branch>/`).
Path prefixes used to keep lines short:

- `D/` = `docs/threads/`
- `S/` = `Source/JavaScriptCore/`
- `T/` = `JSTests/threads/`

Caveats on the sources. (1) `D/SPEC-heap.md` and `D/SPEC-congc.md` cite `Heap.cpp` by line numbers that are stale
("the numbers are stale ... read the citations by function name", `D/SPEC-congc.md:3-6`). I cite the spec's own line, and
the current source line only where I opened the source. (2) The specs describe a *phase-1* design; later rounds changed
it. Where the final tree differs I say "final tree". (3) Sentences marked **[inference]** are my reading for the AOT
design, not something the documents state.

Modes: *flag off* (no threads), *GIL on* (threads serialized by one lock; the heap is not a shared server), *GIL off*
(`useSharedGCHeap=1 useThreadGIL=0 useThreadGILOffUnsafe=1`, `D/LANDING-PLAN.md:48-49`): N mutators, one heap.

Spec notation (`D/SPEC-heap.md:8`): ISS = `isSharedServer()`, WSAC = `worldIsStoppedForAllClients()`, MSPL = mutator
slow-path lock, BVL = directory bit-vector lock, GCL = GC conductor lock, GBL/GBC = GC barrier lock/condition, GSP =
`m_gcStopPending`, GCA/GEC = conductor-active flag / election condition, AHA/RHA = acquire/release heap access, SINFAC =
`stopIfNecessaryForAllClients()`, CIND = `collectIfNecessaryOrDefer`, ACT/DCT = attach/detach current thread, TLC =
`GCClient::GCThreadLocalCache`, HCS = `HeapClientSet`, LA = `LocalAllocator`, NVS = `VMManager::notifyVMStop`.

---

### C: 1. Architecture: one heap server, one client per thread

**Server and clients.** "One *server* heap per thread group (phase 1: the main VM's heap; gate
`Options::useSharedGCHeap()`); each client (GCH) gains a TLC (§5.3)+access state (§10A); directories/blocks/bitvectors
stay shared server-side" (`D/SPEC-heap.md:31`). After GIL removal: "N mutators=ONE GCClient::Heap PER Thread sharing the
server: §10A access state, §5.3 TLC, §5.4 deferral, §11 epoch all stay per-CLIENT" (`D/SPEC-heap.md:27`). The upstream
`GCClient::Heap`/`JSC::Heap` split already existed 1:1, and IsoSubspaces were already per client
(`D/SPEC-heap-history.md:360-366`); non-iso allocators were not (`D/SPEC-heap-history.md:383-388`).

- The server is *by value inside the VM*, so "the shared server **is the main VM's `vm.heap`**, one per process"
  (`D/SPEC-heap.md:22`). `Heap::vm()` means "the main mutator VM"; every `vm()` use in `heap/**` had to be classified
  (task T9, `D/SPEC-heap.md:381`). `GCClient::Heap::vm()` is pointer arithmetic and "GARBAGE for the per-thread
  heap-allocated clients" (`S/heap/Heap.cpp:7992-7997`).
- Registry: `HeapClientSet` (`S/heap/HeapClientSet.h:40-48`), lock rank 6. `add`/`remove` cannot complete inside a stop
  window: "the registry is frozen inside a stop window on both the add and the remove side"
  (`S/heap/HeapClientSet.h:59-66`).
- *Sticky shared mode.* ISS was "option && size() EVER >1" (`D/SPEC-heap.md:52`) with a quiescent protocol switch at the
  second attach (§10B.4, `D/SPEC-heap.md:288`) and a reversion (§10D, `D/SPEC-heap.md:321`). **Reversal:** GIL off "is a
  shared server from VM construction ... also with one thread" (`D/DESIGN-PROPOSALS.md:4981-4982`) and the reversion arm
  no-ops (`T/heap-iss-revert.js:13-18`). Running the legacy protocol while single-threaded was rejected because it
  "reintroduces the protocol flip at the first spawn" (`D/DESIGN-PROPOSALS.md:5284-5286`).

**TLC = thread-local cache of allocators** (not a bump region). "Per-client (per-thread, post-GIL) allocator cache over
the shared server BlockDirectories (... design template: libpas pas_thread_local_cache)"
(`S/heap/GCThreadLocalCache.h:43-53`). A flat table `Allocator* m_table` indexed by
`tlcIndexBase(subspace) + sizeClassIndex`; a null slot lazily materializes this client's `LocalAllocator`
(`S/heap/GCThreadLocalCache.h:60-65`, `D/SPEC-heap.md:60`). One allocator per (client, directory), invariant I3
(`D/SPEC-heap.md:154`). Final tree: the table "is allocated at its lifetime capacity in the ctor and never reallocated"
so cached pointers stay valid (`S/heap/GCThreadLocalCache.h:47-51`).

**Ownership is by heap access, not by thread id**: "an LA/`FreeList` is mutated only by the thread holding its owning
client's heap access, except the conductor world-stopped" (I2, `D/SPEC-heap.md:153`).

**Block handout.**
- Phase 1: one server-wide lock around the whole slow path (`allocateSlowCase`: cursor search, steal, `tryAllocateBlock`,
  `addBlock`, in-lock sweep), the lock that upstream's own FIXME proposed (`D/SPEC-heap.md:56`,
  `D/SPEC-heap-history.md:368-381`). It is taken only *after* CIND returns: "never across a collection request or stop"
  (L2, `D/SPEC-heap.md:133`, `S/heap/Heap.h:2258-2260`).
- A block belongs to at most one allocator through the `inUse` bit, transferred only under BVL (I1,
  `D/SPEC-heap.md:152`). A stolen empty block is swept/removed/re-added under MSPL (I8, `D/SPEC-heap.md:160`).
- The real hazard was not the resize but "the single-mutator lock-free reader privilege" on directory bit vectors
  (`D/SPEC-heap-history.md:27-29`); rule I5b: `m_bits` reallocated only under BVL+MSPL (`D/SPEC-heap.md:157`).
- **Final tree: MSPL became a writer-preferring reader/writer facade with per-directory stripes**
  (`S/heap/Heap.h:455-500`). STRIPE = shared side + `BlockDirectory::m_refillLock` (rank 7a) for own-directory refill;
  EXCLUSIVE for cross-directory steal, mutator-concurrent full sweeps, `WeakSet::allocate/sweep/shrink`, TLC teardown.
  Cross-directory `MarkedSpace` state on the stripe leg is under a leaf `m_markedSpaceRegistryLock` (rank 7r)
  (`S/heap/Heap.h:484-487`). Reason: "~75% of contended refills being JSBigInt ... made every JSRopeString / JSArray ...
  refill queue behind BigInt block creation" (`S/heap/LocalAllocator.cpp:250-258`).
- Slow-path order when shared: stripe #1 own directory -> `tryLock` exclusive for a cross-subspace steal -> stripe #2
  fresh block -> exclusive fallback (`S/heap/LocalAllocator.cpp:287-365`). The `tryLock` steal was added back because
  skipping it cost "~6.4x more first-touch faults" at one thread (`S/heap/LocalAllocator.cpp:303-316`).

**Precise (large) allocations.** `tryAllocateSlow`, `registerPreciseAllocation`,
`reallocatePreciseAllocationNonVirtual` "mutate the precise vectors/set unlocked"; shared mode puts them under MSPL
(`D/SPEC-heap.md:107`, I16 `D/SPEC-heap.md:168`). Final tree: exclusive facade side, except the iso lower-tier path, which
takes the registry leaf lock under a stripe (`S/heap/IsoSubspace.cpp:83-110`). Precise allocations "cannot be"
reallocated in place GIL off (`D/PERF-RESULTS.md:951`), which feeds array-growth garbage (gbemu: 1.13 GB,
`D/DESIGN-PROPOSALS.md:5128-5133`).

**IsoSubspaces.** Each `GCClient::IsoSubspace`'s LA is registered lookup-only in the client's `m_perDirectory` map
(`D/SPEC-heap.md:60`, `S/heap/GCThreadLocalCache.h:67-74`); later static iso subspaces got a stamped table slot so that
compiled code can reach them (`D/SCALEBENCH.md:3217-3221`). Dynamic subspace creation on the server side was left
"needs-trace" (HC-09, `D/AUDIT-heapcontainers.md:34`).

**Accounting.** Byte counters and `MarkedSpace::m_capacity` became relaxed atomics, "exact sums re-establish at
safepoints" (`D/SPEC-heap.md:97`, F3 `D/SPEC-heap.md:143`, I7 `D/SPEC-heap.md:159`). The non-shared arm keeps plain
load+store with a written proof that the two arms are never concurrent (`S/heap/Heap.cpp:3895-3924`). `didAllocate` is
reached only per free-list refill / precise / extra memory, never per cell (`D/SPEC-heap.md:396`).

**Eden allowance (§10F).** One nursery budget with k allocating threads made stops per second grow with k: "raytrace-like
at four threads spending 47 ms of a 275 ms run in 63 eden pauses". Rule: allowance += `(k-1) * min(m_maxEdenSize,
largeHeapSize)`, k = clients that allocated at least a quarter of a fair eden share this cycle. "Cost: up to one capped
nursery of additional floating garbage per additionally allocating thread" (`D/SPEC-heap.md:304-306`; counting code
`S/heap/Heap.cpp:3925-3954`). Test: 51 -> 13 collections (`D/PERF-RESULTS.md:331-333`,
`T/heap-eden-allowance-scales-with-threads.js:2-13`).

**N-mutator floating garbage.** "every eden cycle roots N live stacks, so sibling mutators' in-flight temporaries get
promoted"; the max-heap ratchet climbed without a Full ever firing (`D/SCALEBENCH.md:1115-1131`). Fixed by a shared-only
Full trigger, `sharedGCEdenSurvivalFullTriggerRatio` = 3.0 (`S/runtime/OptionsList.h:508`).

**DeferGC per client (I17).** The server's `m_deferralDepth` is a plain `++/--`; when ISS the depth, `didDeferGCWork` and
`mutatorState` route to the calling thread's client (`D/SPEC-heap.md:99`, `D/SPEC-heap.md:169`,
`D/AUDIT-heapcontainers.md:477-488`). "a deferred client still parks for a pending stop"
(`T/heap-deferral-storm.js:5-10`). `GCDeferralContext` stays stack-local.

**Block retention (§10E).** Physical freeing of empty blocks is world-stopped only, because "sibling mutators' lock-free
reads may touch dead-verdicted cells" (`D/SPEC-heap.md:296`). So the end of a conducted cycle is the only reclamation
point, and its policy went through five amendments (section 11 and 13 below).

---

### C: 2. How compiled code reaches its allocator, and what that cost

**Upstream shape.** Tiers bake a `LocalAllocator*` constant (`allocatorForConcurrently<T>()`); the FTL also loads the
server's size-step array at run time; "**Every emitter already slow-paths on a null allocator**"; the LLInt has no inline
allocation; `ObjectAllocationProfile::m_allocator` is shared between threads (`D/SPEC-heap-history.md:438-449`).

**Phase 1 gating (§5.5).** "**option on, no server-side non-iso `Allocator` is ever materialized**" so baked constants
and run-time loads are null and "every JS-tier emitter slow-paths" into C++, which uses the caller's TLC
(`D/SPEC-heap.md:103`). Data-driven, no JIT edits. Wasm GC is refused under the shared heap because its emitters assume
non-null allocators (`D/SPEC-heap.md:103`, `D/SPEC-heap.md:367`).

**What that cost.**
- The review already said a <=5 % budget "in that config is not credible" (`D/SPEC-heap-history.md:1043-1055`) and that
  the addressing contract "MUST be per-THREAD (VMLite/TLS-relative)"; the vm-relative chain is wrong with one client per
  thread (`D/SPEC-heap.md:26`).
- Measured (one benchmark, one thread): lazy-slow-path traversals 53 (GIL on) / 60 (shared heap, GIL on) /
  **46,631,032** (GIL off) (`D/GILOFF-TAX-EVIDENCE.md:60-62`). GIL off the slow-path jump could not be repatched
  (cross-modifying code), so every allocation ran a register-dumping thunk: "+1155ms ≈ 46.6M × ~25ns/traversal ...
  essentially the *entire* gilOff-only tax is this one mechanism" (`D/GILOFF-TAX-EVIDENCE.md:64-81`).
- C++ side: a per-cell 3-hop lookup (`allocationClientForCurrentThread` -> `allocatorForSizeStep` ->
  `allocateForClient`), "172ms / 27.8M = 6.2ns/cell" out of line, ~550-650 ms in all
  (`D/GILOFF-TAX-EVIDENCE.md:89-105`, table `:200-211`). "#1 and #2 share a root cause"
  (`D/GILOFF-TAX-EVIDENCE.md:213-216`).
- The refill path was *not* the problem: 70.86 M cells, 232,402 refills, "0.33% of allocs hit the slow path", ~1.5 µs
  per refill, refill count unchanged by the shared heap (`D/SHAREDHEAP-ALLOC-EVIDENCE.md:90-91`, `:119-125`).
  "**A "real TLAB" (per-thread fresh-block cache) targets the wrong lever**"
  (`D/SHAREDHEAP-ALLOC-EVIDENCE.md:165-171`).
- TLS details mattered: `ThreadSpecific` (call_once + `pthread_getspecific`) per allocation was "~45% of the
  GIL-off/GIL-on heap-BigInt regression"; default-visibility `thread_local` forced `__tls_get_addr` and un-inlined the
  resolver (+1.067 G cycles); fixed with initial-exec TLS (`S/heap/Heap.h:2400-2414`, `S/heap/Heap.h:2456-2470`).

**Final mechanism.** Compile time bakes a *process-constant slot index*; run time loads the allocator from the running
thread's table: `loadVMLite -> bound>slot? -> [tlcTable + slot*ptr]` (`S/jit/AssemblyHelpers.h:1851-1860`). The per-thread
record (`VMLite`) mirrors `{tlcTable, tlcTableBound}`, "the only form generated code reads, one lite-relative hop"
(`S/runtime/VMLite.h:347-363`). Allocation profiles hold an encoded slot instead of a pointer
(`S/jit/AssemblyHelpers.h:1869-1875`). Rule: "Every inline allocation in DFG and FTL therefore resolves its allocator
through the thread's allocator table GIL off"; a new upstream inline-allocation site silently becomes an operation call
until given that arm (`D/SPEC-jit.md:112`). C++ non-iso path: two TLS loads + one indexed load
(`S/heap/Heap.h:2416-2434`).

**Residual cost.** Six instructions per inline allocation ("`cmpl $slot,bound(lite); jbe slow; mov table(lite),r; mov
slot*8(r),r; test r,r; je slow` (six) where GIL on bakes one `movabs`", `D/DESIGN-PROPOSALS.md:6218-6219`); 18 of the 50
extra instructions of an object-construction loop; B3 redoes the loads after every store because they are typed on the
root abstract heap (`D/DESIGN-PROPOSALS.md:6252-6257`). Recovery: shared-heap tax 1912 -> 1214 -> 453 ms, "cumulative
§42+§43 = **1459 ms = 76.3%**" (`D/SCALEBENCH.md:3110-3112`, `D/SCALEBENCH.md:3244-3246`). `obj-literal` micro row 1.00
(`D/PERF-RESULTS.md:796-798`).

**A size-class trap.** Generated code and C++ must pick the *same* size class: with different vector-length rules "the
inline path draws from a size class the C++ slow path never allocates from and so never refills, and every such
allocation runs the operation" (`D/SPEC-jit.md:114`).

---

### C: 3. Collection with N mutators

**Who conducts: the requester.** "*Conductor = last VM to park*" is impossible: (1) a mutator blocked in a host call
never reaches a trap check; (2) a synchronous requester blocked on its ticket never parks; (3) with zero entered VMs
nobody is there to run the callback. "the *conductor* = the GC requester that won the heap-side election"
(`D/SPEC-heap-history.md:424-430`). VMManager only traps and keeps threads parked.

**Protocol (§10, `D/SPEC-heap.md:246-277`):**
1. Ticket under `m_threadLock`; "no fire-and-forget collections when shared".
2. Election: `GCL.tryLock()`; the winner conducts and drains all granted tickets; a loser **must release heap access**
   and wait on the election condition, then re-acquire (`D/SPEC-heap.md:251-267`). If GCL is held by a non-GC stop:
   "**timed** wait on GEC (≤1ms; never untimed)" and poll traps.
3. seq_cst `GSP = true`; release own access; ask VMManager to trap everyone.
4. Barrier: under GBL wait "until **every client is NoAccess**"; set WSAC.
5. Flush every client's allocators (`stopAllocating`). 6. Gather stacks. 7. Collect; then, still stopped, safepoint
   hooks + epoch reclaim + block retention. 8. Resume allocators, clear WSAC, `GSP = false`, broadcast. 9. VMM resume.
   "Heap resume (8) strictly precedes VMM resume (9)" (`D/SPEC-heap.md:277`).

The conductor runs the Riptide phase loop "**as the mutator**" and may be VM-less; the collector thread is quiesced
(`D/SPEC-heap.md:286`). An allocation-triggered ticket "is conducted by whichever mutator's poll finds it"
(`D/DESIGN-PROPOSALS.md:4983-4985`).

**Heap-access protocol (§10A).** Per-client `Atomic<uint8_t> m_accessState` in {HasAccess, NoAccess}. "**Rule: no thread
reads or writes the shared JS heap except holding some client's heap access.**" (`D/SPEC-heap.md:281`). "ALL
indefinitely-blocking primitives RHA/AHA-bracketed" (`D/SPEC-heap.md:244`); embedder natives on spawned threads too
(`D/SPEC-ungil.md:694-696`). `DropAllLocks` on a spawned thread degenerates to a heap-access bracket
(`D/SPEC-ungil.md:666-670`).

**The Dekker pair (F8).** AHA: (0) already has access -> return (idempotent); (1) seq_cst CAS NoAccess->HasAccess; (2)
seq_cst load GSP; (3) if pending, "**mandatory revert**", signal, block, retry. "**seq_cst mandatory** - acq/rel misses
both store-load pairs" (`D/SPEC-heap.md:148`; proof `D/SPEC-heap-history.md:778-792`). A transient HasAccess flicker inside a
window is therefore legal, and a verifier must wait it out rather than sample once (`S/heap/Heap.cpp:1031-1051`).

**Poll points.** Trap checks in JS, every allocation slow path (CIND -> SINFAC), every RHA/AHA. A parked/blocked thread
"holds no rank ≥ 4 lock" (I6, `D/SPEC-heap.md:158`).

**Composition with the other stop-the-world** (watchpoint fires, jettison, `haveABadTime`, heap walks: "§A.3"
thread-granular stop, `D/SPEC-ungil.md:220-360`):
- Both serialize on GCL: `Heap::JSThreadsStopScope` is "GCL (rank 2) RAII for a JSThreads/debugger stop"
  (`D/SPEC-heap.md:200-205`). "at most one of {GC window, §A.3 window} open at any instant — single-owner GCL is the
  proof" (`D/SPEC-congc.md:588-592`).
- Order pin for every conductor: "release access -> arbitration -> GCL; only the WINNER touches GCL, access-released"
  (`D/SPEC-ungil.md:270-273`). GCL-before-release was rejected as a deadlock (`D/SPEC-congc.md:123-124`).
- The two stops differ in what a waking thread sees: "GC stops set client-visible state (GSP/F8); §A.3 stops set none";
  AHA has three gates (GSP, the §A.3 stop word, the VMM mode machine), each re-looped (`D/SPEC-congc.md:612-616`,
  `D/SPEC-ungil.md:243-256`).
- No GC inside a §A.3 window: slow paths enqueue a request for after the resume (`D/SPEC-ungil.md:329-332`). No epoch
  reclaim from a §A.3 stop (`D/SPEC-heap.md:325`).
- The §A.3 conductor was first modelled as allocation-free; that "was FALSE": it is "a FULL CLIENT during its window"
  (re-acquires its own access, allocates, runs barriers, may weak-sweep) (`D/SPEC-congc.md:632-644`).
- Fairness: the §A.3 side acquires GCL by an unqueued 1 ms `tryLock` poll, which a GC conductor's blocking re-acquire can
  starve into the 30 s watchdog; fix = a foreign-waiter counter that the GC defers to (`D/SPEC-congc.md:577-587`).

**Attach / detach.**
- Spawn: construct the client and attach "BEFORE any allocation" (`D/SPEC-ungil.md:367-372`). ACT = stamp TLS client,
  register for conservative scan, acquire access (`S/heap/Heap.cpp:7879-7908`).
- Exit: "release access (seq_cst RHA) -> TEARDOWN mark ... -> DCT/destroy client -> unregisterLite/free lite LAST";
  `~VM` blocks until no other thread record names it (`D/SPEC-ungil.md:373-382`).
- Mid-cycle attach/detach is legal *between* windows, blocked *inside* them (`D/SPEC-congc.md:646-717`). Requiring
  quiescence for a mid-cycle attach was rejected (`D/SPEC-congc.md:695`). The exiting thread is never the live conductor
  (release assert, `D/SPEC-congc.md:677-679`).
- Attach deliberately does not stamp the epoch: a delayed store could land between a later stop's stamping loop and its
  min scan (`S/heap/Heap.cpp:7889-7905`).
- "A spawned thread's exit sweeps nothing (its blocks go back unswept)" (`D/SPEC-heap.md:315`).

---

### C: 4. Concurrent marking with N mutators

**Phase 1: off.** "Shared mode is synchronous, conductor-driven (§10); disabled: concurrent marking, collector-conn,
incremental assist, activity-callback collection, mutator-concurrent sweeping; parallel marking inside the stop stays"
(`D/SPEC-heap.md:23`).

**Design for re-enabling: the window model.** "A shared collection becomes ONE GCA TENURE containing a SEQUENCE of stop
windows" (`D/SPEC-congc.md:110-111`). GCL is released between windows so other stops can interleave
(`D/SPEC-congc.md:155-160`). "Conducting is a CLOSED LOOP": no heap access for the whole tenure, no JS; between windows
the conductor only does `donateAll()` + `waitForTermination()` (`D/SPEC-congc.md:189-207`). Why it may neither return to
JS nor drain: UAF, deadlock and wake-up races against a foreign stop (`D/SPEC-congc-history.md:1680-1713`). What stays
in-window: constraint solving, conservative scan, begin/endMarking, finalize, epoch reclaim (`D/SPEC-congc.md:492-504`).

**What already carried concurrency** (reused unchanged): versioned liveness / newlyAllocated, the race mark stack, the
re-whiten CAS, multi-window phases (`D/SPEC-congc.md:86-100`).

**Write barrier from N threads.**
- The inline barrier is unchanged in shape (`D/SPEC-congc.md:259-264`). "The cellState CAS protocol is already N-safe
  (single-word CAS; mutator-count-independent)" (`D/SPEC-congc.md:297-299`).
- The one unsound piece was the slow path's append to the single `m_mutatorMarkStack` (`postIncTop` is a non-atomic RMW):
  fixed with a lock on that instance (`D/AUDIT-heapcontainers.md:500-505`; final tree: `m_serverMutatorMarkStackLock`,
  `S/heap/Heap.cpp:1745-1748`).
- **Per-client mutator mark stacks (CMS)** are "a CONTENTION/SCALING + window-drain-ACCOUNTING design, not a soundness
  fix" (`D/SPEC-congc.md:266-274`); drained at every window open, before the window's first constraint pass
  (`D/SPEC-congc.md:281-308`). Only under `useConcurrentSharedGCMarking` (default false, `S/runtime/OptionsList.h:502`).
- Write-barrier slow path cost measured flat: 88 ms against 82 ms GIL on (`D/SHAREDHEAP-ALLOC-EVIDENCE.md:57`).

**`mutatorShouldBeFenced` / barrier threshold.**
- Phase 1: forced "always-fenced once shared". Design: server master mutated in-window only + fence epoch + per-client
  copies republished before the window closes; raises are window-complete, lowers only in the final window; "over-fenced
  is always sound" (`D/SPEC-congc.md:310-344`).
- Compiled code bakes the *server's* addresses, so per-client copies need a code-generation change; until then the master
  stays pinned under C1 GIL off (F19, `D/SPEC-congc.md:328-336`).
- **Cost and reversal:** forcing it "made every write barrier take the slow path with a store-load fence (measured:
  1.4-2.7x on barrier-heavy micro-benchmarks GIL-off)" (`S/heap/Heap.cpp:5327-5341`). Even after that, the sticky flip
  raised it at start-up and nothing lowered it before the first GC: "2.2-2.5x on a barrier-heavy put loop that never
  collects" (`D/SPEC-heap-history.md:1119-1136`). Final code: kept raised on non-x86 (it also orders a new cell's
  initialization before publication to *another mutator*) and under C1 GIL off; otherwise it follows marking
  (`S/heap/Heap.cpp:5340-5344`, `D/DESIGN-PROPOSALS.md:5388-5392`).

**Black allocation.** "The mechanism is already allocator-local" (versioned newlyAllocated keyed on `isMarking`). N-client
rules: `m_isMarking` flips only in-window and the F8 resume edge publishes it; every sweep-to-free-list and steal holds
MSPL and access, so none straddles a window; every client's allocators are flushed exactly once per window open and
resumed once per close (`D/SPEC-congc.md:346-379`).

**Marking vs live mutators.** `visitChildren` races mutators; the object-model protocol is consumed as is; the spec adds an
audit obligation with classes {IN-WINDOW-ONLY, OM-PROTOCOL, N-PROTOCOL, CELL-LOCKED, RACY-TOLERATED}. Markers use
"tryLock + defer/re-visit, NEVER block"; and "a JSCellLock (10a) holder must not release heap access, pass a stop poll,
or enter a conducting path" (`D/SPEC-congc.md:506-527`, seed rows `D/SPEC-congc-history.md:1371-1404`).

**Stages and final state.**

| Stage | Content | Final state |
|---|---|---|
| C1 | concurrent marking, per-client mark stacks and fences (`D/SPEC-congc.md:387-399`) | implemented behind `useConcurrentSharedGCMarking`, default **false**. First A/B: "it buys ~nothing" (within ±2.4 %) and "8 SIGSEGVs across 30 congc reps" (`D/SCALEBENCH.md:821-832`) |
| 7.1a | "gilOff single-handoff": **at most one** concurrent window per cycle, no flag, per-client routing off (`D/SPEC-congc.md:401-425`) | **live in every GIL-off process** (`S/heap/Heap.cpp:179-215`) |
| C2 | collector-thread conductor, activity callbacks (`D/SPEC-congc.md:427-448`) | option "reserved": "nothing beyond the `sharedGCWindowedConductActive()` predicate reads them" (`D/SPEC-congc.md:4-6`). Final tree did re-enable eden-activity dispatch on the main client's thread only, routed into tickets (`S/heap/Heap.cpp:3963-3971`) |
| C3 | incremental + mutator-concurrent sweeping (`D/SPEC-congc.md:450-469`) | option reserved; but the IncrementalSweeper does run on the main VM's run loop in a shared form: exclusive MSPL per step, never frees a block, skips weak-bearing blocks (`S/heap/IncrementalSweeper.cpp:140-183`) |
| C4 | incremental mutator assist (`D/SPEC-congc.md:471-488`) | off: `performIncrement` returns at once when shared (`S/heap/Heap.cpp:5360-5370`) |

Also tried and parked: parked siblings joining the marking (`D/SCALEBENCH.md:1064-1065`), now capped at 0 by default
(`S/runtime/OptionsList.h:509`); "measured twice, a wash" (`D/DESIGN-PROPOSALS.md:5288-5289`).

**Consequence of the closed loop** (largest open GC item): with one mutator "the "concurrent" window has nobody to be
concurrent with: seven helpers mark, the conductor sleeps, and the program loses the whole cycle"
(`D/DESIGN-PROPOSALS.md:4993-4996`). With N threads one mutator sits out each window (`D/DESIGN-PROPOSALS.md:5094-5100`).
Proposed, not built: a service conductor thread for cycles predicted >= 5 ms, and "a lone conductor does not schedule the
concurrent window" (`D/DESIGN-PROPOSALS.md:5176-5215`).

**Thread-local nursery collection: rejected.** "In a shared-memory object model any plain store publishes an object, so the
invariant needs a publication barrier on every pointer store in every tier ... an unbounded transitive walk on the
publishing thread ... The pauses this would remove cost 3-5 % of a parallel leg today. Out of proportion"
(`D/DESIGN-PROPOSALS.md:5363-5373`). PARITY-PLAN still names it as "the known next design" if scaling gates are missed
(`D/PARITY-PLAN.md:1317-1322`).

---

### C: 5. Conservative scan of N threads

- Root set: "stack ∪ registers of every I4(b)-registered thread (suspend-and-copy, §10.6) ∪ `CLoopStack` if
  `!ENABLE(JIT)`" (I12, `D/SPEC-heap.md:164`). Upstream's `MachineThreads` already handled N registered threads:
  "Suspend-and-copy works whatever the thread executes ⇒ no mutator snapshot hooks needed"
  (`D/SPEC-heap-history.md:432-436`).
- Registration is enforced, not assumed: AHA "ensure — not assert" `addCurrentThread()` before the CAS, with a per-client
  uid cache to skip the thread-group lock (`S/heap/Heap.cpp:7941-7959`, `S/heap/Heap.cpp:7973-7985`).
- The conductor's own stack goes through its `CurrentThreadState`; others through `tryCopyOtherThreadStacks`
  (`D/SPEC-heap.md:271`). Runs only in-window (`D/SPEC-congc.md:494-497`), and again at each fixpoint window.
- **Later optimization: cooperative snapshots.** A thread about to park (access released) spills callee-saves into a
  `CurrentThreadState` in the parking caller's frame and seq_cst-publishes a pointer; the conductor copies it and skips
  the signal round trip (`S/heap/Heap.h:2358-2390`, `S/heap/MachineStackMarker.h:49-60`). Motivation: "2.07% of W=16
  parallel-phase mutator-time was the SIGUSR2 ... sem_wait, and 25/52 of those victims were ALREADY cooperatively parked"
  (`S/heap/MachineStackMarker.cpp:288-293`).
- Hazards that optimization met: the snapshot pointer can dangle (bounded waits expire and clear), so the conductor caches
  the *client*, re-loads at use time, latches each field once, re-validates against the thread's real stack bounds and
  falls back to suspend (`S/heap/Heap.cpp:1077-1104`, `S/heap/MachineStackMarker.cpp:192-213`). The snapshot must live in
  the frame that stays live across the park, not in an out-of-line RAII constructor's frame
  (`S/heap/MachineStackMarker.cpp:179-182`).
- Running (not parked) threads still use signals on Linux, `thread_suspend` on Darwin, `SuspendThread` on Windows
  (`D/DESIGN-PROPOSALS.md:5399-5402`).
- **C_LOOP:** "the CLoop stack is per-VM, not per-thread ... post-GIL, if Thread() ever gets per-thread CLoopStacks ...
  iterate them here" (`S/heap/Heap.cpp:1106-1117`): never done.
- **VM entry state:** `entryScope`, `isEntered()`, `stackPointerAtVMEntry`/`lastStackTop` moved into the per-thread record;
  "isEntered() = '§A.3.1 set non-empty'" (`D/SPEC-ungil.md:115-127`). `sanitizeStackForVM`, shadow chicken and
  `topCallFrame` are listed as VM-singular, deferred (`D/SPEC-congc-history.md:1329-1330`). Scratch buffers and side state
  are JIT-only roots (`S/heap/Heap.cpp:1132-1150`).
- Test: `T/gc-stress/conservative-scan-register.js:2-20` (a reference that lives only in a parked thread's frame).

**The window-witness set ("Wlr"): a workaround that was later removed.** June: live cells were swept with N mutators
(`D/SCALEBENCH.md:289-315`). The lost edge "was never identified"; instead a marking constraint rooted *and traced*
everything any parked client had allocated since the last marking (`D/SPEC-heap.md:164`,
`D/SPEC-heap-history.md:1071-1088`). "Mark-without-trace is FORBIDDEN" (a retained, untraced Structure stayed findable in a
weak table with a swept PropertyTable). Costs: section 11. Switched off in the tenth round after the object model's
publication order and barriers were rewritten and "five rounds closed ways for a thread to keep heap access, or to run,
across a stop"; validated with `--verifyGC=1` (`D/SPEC-heap-history.md:1397-1430`).

---

### C: 6. Sweeping, destructors, finalizers, weak things

**Which thread (§10G, `D/SPEC-heap.md:308-317`).** "A dead cell's destructor, a `WeakHandleOwner::finalize`, and a
`Heap::addFinalizer` callback run on the thread that sweeps the cell's block or precise allocation." GIL off that is: any
thread that lazily sweeps while allocating, the conductor inside the stop (epilogue's eager sweep of lower-tier precise
cells, requested-Full sweep, weak finalizers), the conductor after the stop (deferred lambda finalizers), or any client's
synchronous-collection tail. "Every destructor runs exactly once, whichever thread that is."

**Embedder contract.** "A destructor or finalizer must not assume which thread runs it ... must not take a lock that
another thread may hold across a safepoint, allocate a cell, or enter JS. It may take the VM's API lock"
(`D/SPEC-heap.md:317`). Alternatives rejected: owner-only sweeping of embedder subspaces (an idle owner keeps dead cells
for ever), and a deferred-destruction queue inside JSC ("the sweep frees the cell's memory, so the queue would need to
copy out whatever the owner's teardown reads") (`D/SPEC-heap-history.md:1285`). Bun posts native finalizers and strong-slot
releases to the VM's event loop when run on another thread; side effect: memory freed a loop turn late, unmeasured
(`D/DESIGN-PROPOSALS.md:5160-5165`).

**`addFinalizer` lambdas** may clear Strongs and take API-rank locks, so GIL off they are queued inside the stop and run at
the conduct tail, world resumed, access held, with a **null cell pointer** (`S/heap/Heap.cpp:3995-4047`;
`D/SPEC-ungil.md:662-664`). "no JS finalizers in the stop window" (`D/SPEC-heap.md:286`).

**Weak handles (`Weak<>`, WeakSet/WeakBlock): "the weak-mutation protocol".**
- Allocation: under exclusive MSPL, including the WeakImpl's construction (`D/AUDIT-heapcontainers.md:423-431`).
- Collector side (sweep, shrink, reap, visit): world stopped. Any sweep that runs alongside mutators **skips blocks whose
  WeakSet has WeakBlocks** ("weak-bearing"), "because MSPL does not exclude the finalizer-vs-Weak-owner lifetime race"
  (`S/heap/WeakSet.cpp:98-120`; carve-out `S/heap/LocalAllocator.cpp:531`).
- Clear/deallocate: first deliberately lock-free (reachable from destructors inside in-lock sweeps, where re-taking MSPL
  self-deadlocks) (`D/AUDIT-heapcontainers.md:441-448`). After an upstream redesign made `WeakImpl::clear()` edit the
  block's free list, it goes through a new leaf lock `Heap::m_weakHandleLock` (`S/heap/Heap.h:2225-2228`,
  `D/AUDIT-upstream-since-rebase.md:2050`).
- Weak *creation* is forbidden under API-rank locks and leaf locks (hoist construction before the lock); a
  `WeakHandleOwner::finalize` may take only a closed list of locks in-window (`D/SPEC-ungil.md:1001-1008`).
- Consequence: "Every Structure block is weak-bearing" and cannot be reused by the allocation path
  (`D/DESIGN-PROPOSALS.md:5107-5117`). See 13.4-13.6. Proposed fix, not built: run the *weak half* of the sweep inside the
  stop and the *cell half* outside (`D/DESIGN-PROPOSALS.md:5291-5322`).

**WeakMap / WeakSet (JS).** "were not locked, GIL off": `set`/`delete` rehash and free the buffer under another thread's
probe; "Two threads writing one `WeakMap` crashed every time". Now every operation holds the map's cell lock and the
intrinsics stay calls (`D/LANDING-PLAN.md:1230-1241`). Marker side: cell-locked with tryLock + revisit
(`D/SPEC-congc-history.md:1386-1397`). Cost: WeakMap get/has under the cell lock "are 30 % of Basic's locked loads"
(`D/PARITY-PLAN.md:277-278`).

**WeakRef.** `m_currentWeakRefVersion` is one VM-wide atomic bumped by whichever thread ends synchronous execution
(`S/runtime/VM.h:1822-1831`; audit row `D/SPEC-ungil-audit-K4.md:427`). JSC releases `deref`-kept objects when the
microtask queue drains (`D/LANDING-PLAN.md:862-866`).

**FinalizationRegistry.** Registration maps are under the cell lock (`D/SPEC-ungil-audit-N7.md:209`). The GC-end pass takes the
cell lock on the conducting thread and schedules the cleanup through `vm.deferredWorkTimer`
(`S/runtime/JSFinalizationRegistry.cpp:100-167`); the timer's ticket set got a leaf lock and carrier/spawned routing rules
(`D/SPEC-ungil.md:595-627`). Which thread finally runs the callback: see section 14.

**`WeakGCHashTable` registry, `m_protectedValues`, mark-list set:** section 7.

---

### C: 7. Strong handles, MarkedArgumentBuffer, protect()

The heap-container audit (`D/AUDIT-heapcontainers.md:22-40`) was started by an observed UAF, after audits of VM members
and cell classes had missed the heap's own containers.

| Container | Problem | Final tree |
|---|---|---|
| `Heap::m_markListSet` (spilled `MarkedVector`/`MarkedArgumentBuffer`) | "unsynchronized HashSet shared by all threads' MarkedVector spill paths; observed UAF"; hit by every big `sort`/`apply`/spread (`D/AUDIT-heapcontainers.md:9-11`, `:45-68`). Caveat: a vector can be destroyed on another thread than it registered on (`:77-80`) | 32 shards, each with a lock, chosen by hashing the vector's address; collector iterates world-stopped (`S/heap/Heap.h:939-978`) |
| `Strong<>` set | "ONE shared HandleSet per VM, new leaf `m_strongLock` inside Strong allocate/free/set-slot only"; GC scans it lock-free under the GC stop; frees are legal from in-lock sweeps ("destructor-leaf") (`D/SPEC-ungil.md:656-665`, `:983-988`) | upstream replaced HandleSet by `StrongSet`; GIL off allocate/deallocate take `m_gilOffLock` (`S/heap/StrongSet.h:72-76`, `:136-137`) |
| `protect`/`unprotect` | plain `HashCountedSet` (HC-03) | `m_protectedValuesLock` GIL off; the caller's heap access is what excludes the collector's lock-free walk (`S/heap/Heap.cpp:809-842`) |
| `m_weakGCHashTables` | HC-04 | leaf lock (`S/heap/Heap.h:1805`, `S/heap/Heap.cpp:4440-4467`) |
| `immutableButterflyToStringCache` | unlocked HashMap on `join()`'s fast path, "confirmed-race" (HC-02, `D/AUDIT-heapcontainers.md:27`, `:99-110`) | ruling: per-client cache; I did not check the final code |
| pacing fields, `m_isInOpportunisticTask`, deprecated extra memory | heuristic-only races (HC-05..08, `D/AUDIT-heapcontainers.md:30-33`) | rulings: relaxed atomics / per-client flag |
| `m_arrayBuffers`, `CodeBlockSet`, `JITStubRoutineSet`, `m_possiblyAccessedStringsFromConcurrentThreads` | own locks (`D/AUDIT-heapcontainers.md:557-598`) | `traceMarkedStubRoutines` stays lock-free, sound only while every phase touching it is in a stop (`:571-574`) |

---

### C: 8. Epoch reclamation

**Mechanism** (`D/SPEC-heap.md:323-325`, `S/heap/GCSafepointEpoch.h:43-60`): global epoch, per-client `m_localEpoch`,
`retire(ptr, destroy)` from any thread. An item retired at E is destroyed only when (a) every client published a later
epoch or detached, (b) the world is stopped, (c) compiler threads are suspended "by the reclaimer's OWN suspend/resume
pair" (the cycle's own periphery suspension has already been resumed by then, `D/SPEC-heap-history.md:404-407`). Bumps happen
only at the end of a GC cycle (final window), never from another kind of stop: "a non-GC bump reclaims against stale
epochs; JSThreads stops enqueue a GC request instead". Empty cycles skip the bracket (`D/SPEC-congc.md:501-504`). Also runs in
the legacy protocol, "sole option-off delta" (`D/SPEC-heap.md:275`).

**What is actually retired** (call sites found by grep):
- non-executable JIT data: handler-IC chain nodes, replaced call-link records, polymorphic-access stubs
  (`S/bytecode/RetiredJITArtifacts.h:41-49`, `S/bytecode/PropertyInlineCache.cpp:196-211`,
  `S/bytecode/CallLinkInfo.cpp:78`). "epoch expiry frees only data whose every JIT-side access sits in a safepoint-free
  window - NEVER machine code"; code waits for the conservative scan (`S/bytecode/RetiredJITArtifacts.h:63-67`).
- one runtime case: a displaced dead `Weak<JSArrayBuffer>` whose WeakImpl another thread may still be inspecting
  (`S/runtime/SimpleTypedArrayController.cpp:100-117`).
- Must use the *server* heap's epoch, not `vm.heap` (`S/bytecode/RetiredJITArtifacts.h:50-60`).

**Sibling mechanism on the same hook:** `addStopTheWorldSafepointHook`, once per collection; the object model's quarantine of
deleted property slots is released by it (`D/SPEC-heap.md:191-192`, `D/SPEC-heap.md:365`). An open safety residue sits here: a
collection between a claimant's publication and its fire moves the quarantine epoch, and a deleted slot can be reused while
stale optimized code survives (`D/LANDING-PLAN.md:5216-5221`).

**Physical block freeing** follows the same rule ("§11's rule for freed memory"), world-stopped only (`D/SPEC-heap.md:296`).

Lock rule: `retire()` is legal under cell/Structure locks, "never while holding ranks 7-9b"
(`S/heap/GCSafepointEpoch.h:58-60`).

---

### C: 9. Heap walks, snapshots, deleteAllCode, quotas

- **`HeapIterationScope` stops nothing by itself.** On a shared heap its `stopAllocating()` "flushes the other clients' free
  lists out from under their lock-free fast paths" (`D/SPEC-heap-history.md:1258-1260`). Three of the four GIL-off corpus
  failures at the start of the landing work were this one class (`D/LANDING-PLAN.md:69-78`).
- **Rule:** `Heap::runWithOtherClientsStopped(func)`: inline when not shared or already stopped; otherwise the caller
  conducts a §A.3 stop window; it must hold the JSLock and no cell/Structure/heap lock, and `func` must not enter JS
  (`S/heap/Heap.h:988-1005`, `S/heap/Heap.cpp:944-959`). "Every JSC user of `HeapIterationScope` uses it";
  `MarkedSpace::size()`/`objectCount()` take the exclusive slow-path lock (`D/LANDING-PLAN.md:119`). Users: heap statistics,
  snapshot builder, heap profiler, sampling profiler, inspector (`S/heap/Heap.cpp:1457-1610`,
  `S/heap/HeapSnapshotBuilder.cpp:359`, `S/heap/HeapProfiler.cpp:75`, `S/runtime/SamplingProfiler.cpp:565-588`).
- The assertion in `MarkedSpace::stopAllocating` is "the right tripwire" for walks added later
  (`D/AUDIT-heapcontainers.md:624-633`); it caught one a rebase brought in: 15 of 20 Release runs crashed
  (`D/SPEC-heap-history.md:1250-1279`).
- **`deleteAllCode` / `deleteAllLinkedCode`** "run their work in a stop window, after the JIT plans finish, and requeue while
  a thread is inside JS" (`D/LANDING-PLAN.md:119`); upstream's later `deleteAllCodeToGenerateItAgain` got the same
  (`D/LANDING-PLAN.md:4099-4100`).
- **Heap snapshots.** Two threads snapshotting deadlocked, then crashed: "`preventCollection` parked in a plain lock with heap
  access held while the holder requested a stop; then two `HeapSnapshotBuilder`s appended to one profiler". Fix: polling
  `tryLock`, builders serialized (`D/LANDING-PLAN.md:1676-1681`). A `PreventCollectionScope` holder must still be able to run
  its own `collectNow(Sync)` (`T/gc-stress/heap-snapshot-prevent-collection-shared.js:2-9`). `waitForCollector`'s loop must also
  poll the *other* stop kind (`D/SPEC-heap-history.md:1096-1101`).
- **`haveABadTime`** walks every live cell from a mutator, inside a §A.3 stop (`D/AUDIT-heapcontainers.md:624-627`).
- **Quotas (Annex B).** Problem: "a single spawned thread in a tight allocation loop drives the shared heap to its limit, and
  the resulting OOM lands on whichever client allocates at the threshold" (`D/SPEC-heap.md:394`). Design: per-client gross
  counter charged on slow paths, OOM attribution, soft quota = gross bytes since the last cycle. Per-client *live* accounting
  rejected (needs per-client mark attribution). Assert-mode allocations must still succeed; the throw is delivered at the next
  trap checkpoint (`D/SPEC-heap.md:396-402`). **Not implemented:** the option is an "INERT STUB"
  (`S/runtime/OptionsList.h:784`); grep finds no `m_grossBytesAllocated` in `S/heap/`.

---

### C: 10. Lock order and deadlock lessons

**Master table** (`D/SPEC-heap.md:113-130`; merged process table `D/SPEC-ungil.md:930-1010`). Outermost first:

| Rank | Lock |
|---|---|
| 1 | API lock / entry token / **heap access** (treated as a lock) |
| (api 1-3, 4b) | thread manager, wait tables, queue locks; the §A.3 pending-job slot mutex, outer to GCL |
| 2 | GCL (conductor lock) |
| 3 | `VMManager::m_worldLock` |
| 4 | GBL (access barrier) |
| 5 | `Heap::m_threadLock` (tickets, election) |
| 6 | `HeapClientSet::m_lock` |
| 6b | JIT worklist locks, `CodeBlock::m_lock` |
| 7a(spec) | structure allocation lock |
| 7 | MSPL (final tree: facade; stripe + `m_refillLock` 7a, registry leaf 7r) |
| 7b | `MarkedSpace::m_directoryLock` |
| 8 | `BlockDirectory::m_localAllocatorsLock` |
| 9 | BVL |
| 9b | MarkedBlock internals |
| 9c/9d | per-client mark-stack lock (terminal leaf); marking-internal group, inside GCL/GBL |
| 10a < 10b | JSCellLock < `Structure::m_lock` |
| leaves | epoch retire lock, atom-table shards; "destructor-leaf class" (atom shards, SymbolRegistry, strong-handle lock), acquirable *under* MSPL/BVL/9b because in-lock sweeps run destructors (`D/SPEC-ungil.md:983-988`) |

Rules L1-L6 (`D/SPEC-heap.md:132-137`): a stop/election is entered with no rank >= 4 lock; the conductor takes 4-10 freely
while stopped because parked mutators hold none; cell/Structure-lock holders allocate only under a *pre-lock* DeferGC, "the
SOLE sanctioned back-edge"; rank 7-9b sections never take cell locks. Negative edges: "no heap 2-9b holder acquires ANY api
lock ... GC/§A.3 conductors acquire NO api lock" (`D/SPEC-ungil.md:996-1000`).

**Deadlock lessons (each was hit or derived):**
1. *Heap access is a lock.* Blocking on anything while holding it, when the holder of that thing may request or wait for a
   stop, is a deadlock (snapshots `D/LANDING-PLAN.md:1676-1679`; a follower must release access before waiting
   `D/SPEC-heap.md:260`).
2. *A lock whose holder can park must not be waited on without a safepoint, and a lock waited on without a safepoint must never
   be held at a poll.* A trap check (`RETURN_IF_EXCEPTION`) under a cell lock parked the holder; the main thread blocked on that
   lock; 30 s watchdog (`D/LANDING-PLAN.md:969-985`). Debug asserts (cell-lock depth == 0) were then added at every park site,
   every `RETURN_IF_EXCEPTION`, `handleTraps`, the collection poll, both AHA park legs and RHA, and found 22 more files
   (`D/LANDING-PLAN.md:1366-1377`, `S/heap/GCThreadLocalCache.h:125-135`).
3. *A collection point under a cell lock deadlocks against markers.* `reportExtraMemoryAllocated` under the lock started a
   collection; "a deferral scope that ends under the lock is a collection point too"
   (`D/AUDIT-upstream-since-rebase.md:1059-1080`).
4. *The conductor runs foreign code inside its own stop.* (a) It polled traps in C++ and parked in its own stop
   (`D/LANDING-PLAN.md:1770-1779`, `D/SPEC-heap-history.md:1101-1107`). (b) A destructor took the API lock, which ran the gated AHA,
   which reverted on the conductor's own GSP: self-deadlock; the first fix exempted one gate of two and it parked in the other
   (`D/SPEC-heap-history.md:1358-1394`). (c) A jettison from the conductor's end phase spun on the compilation lock, whose holder
   was parked for that very stop; the "world is stopped" test read the per-VM flag, which is false in a shared stop
   (`D/LANDING-PLAN.md:2328-2339`).
5. *States that used to last a few instructions become steady states.* "GCL free && GCA set && phase != NotRunning" allowed a
   second conductor (`D/SPEC-congc-history.md:1629-1648`) and a permanent revert-poll deadlock
   (`D/SPEC-congc-history.md:1660-1678`).
6. *Never wait untimed where the waker may be parked*: <= 1 ms timed waits in the GCL-busy rule and in attach quiescence
   (`D/SPEC-heap.md:267`, `D/SPEC-heap.md:288`).
7. *A C++ loop with no poll is a hang for everybody*: a `Set` spread that never ended held access until the watchdog
   (`D/LANDING-PLAN.md:838-848`); a zero pause budget made the shared fixpoint loop for ever
   (`T/gc-stress/zero-pause-budget-fixpoint.js:2-6`).
8. *Rank tables must be split by who takes the lock under what*: "cellLock never wraps allocation" contradicted in-tree code;
   rank 10a was split into 9b and 10a (`D/SPEC-heap-history.md:1028-1041`).

---

### C: 11. Measured costs

**Single-thread cost of the shared-heap mode, over time** (SCALEBENCH big program unless noted):

| When | Number | Cite |
|---|---|---|
| June, run 2 (with the over-retention workaround) | map-heavy T(1): flag off 1373 ms, GIL on 1467, GIL off 2710 ("**+97% vs flag-off**"), all attributed to `useSharedGCHeap`; RSS 3.72 GB against 155 MB | `D/SCALEBENCH.md:676-690` |
| June, run 3 | "+122.5% wall time and 32.9x RSS on single-threaded execution" | `D/SCALEBENCH.md:881-884` |
| run 3.1 | tax 2.20x -> 1.66x; W=1 RSS 13.9 GB -> 411 MB | `D/SCALEBENCH.md:1016-1029` |
| §41 decomposition | shared heap + GIL off = +1912 ms (25 %): ~737 ms shared heap with the GIL, ~1175 ms GIL-off only | `D/SHAREDHEAP-ALLOC-EVIDENCE.md:33-47` |
| §42/§43 (allocator table in compiled code) | 1912 -> 453 ms | `D/SCALEBENCH.md:3244-3246` |
| Sept, 18 JetStream tests, main-thread cycles, GIL off vs GIL on | "allocation and collector **1.4 %**" mean (raytrace 3.7, json-parse 2.4) | `D/PARITY-PLAN.md:239-259`, `:277` |
| Sept, scaling suite T(1) vs flag off | raytrace-like -6 %, splay-like +30 %, string-heavy +14 %, map-heavy +77 % | `D/PERF-RESULTS.md:1191-1199` |
| splay (JetStream), GIL off vs GIL on | 0.67; main thread out of JS 265 ms against 69 ms | `D/DESIGN-PROPOSALS.md:4977`, `:5001-5007` |

**Flag-off cost of carrying the code.** Marking "+6.4 % instructions per collection", +13 instructions per visited cell: 5 from
counters and the mark stack's `m_top` made relaxed atomics ("the compiler may not merge the loads"), 8 from a mode flag captured
by reference and a reshaped switch (`D/DESIGN-PROPOSALS.md:2363-2395`, `D/SPEC-congc-history.md:2816-2825`). After fixes: marking
-0.003 % of the run, sweeping/slow paths +0.072 % (`D/PERF-RESULTS.md:1878-1882`). `isSharedServer()` (236 uses) is decided on the
frozen config page first (`D/SPEC-heap-history.md:1433-1439`).

**Allocation scaling.**
- June, before any work: best 1.59x at 8 threads, raytrace-like 0.65x, richards-like 0.23x (`D/SCALEBENCH.md:67-73`).
- Striped MSPL + retention fixes: W=16 wall -46 %, cpu_util 0.26 -> 0.43, RSS halved (`D/SCALEBENCH.md:1100-1114`).
- Object creation alone, N threads doing the same per-thread work (ideal flat): adds 6.0 / 6.6 / 8.2 / 9.6 ms at 1/2/4/8,
  literals 8.8 / 11.3 / 15.7 / 18.5 ms; before an *object-model* fix: 8 / 1920 / 2100 / 4206 ms (`D/PERF-RESULTS.md:369-376`).
- Scaling gate, tenth round (2/4/8 threads): raytrace-like 1.63/3.33/4.84, splay-like 1.95/3.59/5.74, map-heavy 1.84/3.26/4.30
  (`D/PERF-RESULTS.md:1393-1401`).
- Per-thread allocator state "bounded at ~6.7MB/thread" (`D/SCALEBENCH.md:1123-1125`).

**Pauses.**
- A stop besides marking: `edenGC()` on an almost empty heap 0.27 ms alone; 0.38/0.41/0.50/0.57/0.93 ms with 1/2/4/8/16 spinning
  threads, "about 0.04 ms per running mutator per stop" (`D/DESIGN-PROPOSALS.md:5167-5172`). In June a re-entry cost ~9 ms at 16
  threads, which is why the one-window cap exists (`D/DESIGN-PROPOSALS.md:5353-5354`).
- Eden cycle, splay-like at 1/4/8 threads: 15-17 / 27-35 / 38-56 ms, windows 1.0+0.7 / 1.1-3.6+2.4-5.0 / 1.7-2.5+5.7-8.6 ms. "The
  number of stops no longer grows with the thread count ... What grows is the second window". Share of a parallel leg spent
  stopped: 3-5 %. "The remaining distance from linear scaling ... is not in the stops" (`D/DESIGN-PROPOSALS.md:5075-5092`).
- More markers: a wash (`D/DESIGN-PROPOSALS.md:5083-5087`, `D/SPEC-heap-history.md:1291-1304`).
- A requested sync GC costs the same in every mode (0.42/5.1 ms against 0.47/5.3 ms) (`D/DESIGN-PROPOSALS.md:5152-5154`).
- Service-conductor experiment: break-even at a cycle of about 3-4 ms; below it, two real rendezvous cost more than conducting
  yourself (`D/DESIGN-PROPOSALS.md:5026-5046`).

**RSS.**
- Final scaling suite, MB, GIL off at 1/2/4/8 (flag off serial; GIL on at 4): splay-like 322/290/403/639 (242; 503); map-heavy
  167/256/351/548 (165; 168); raytrace-like 71/105/170/303 (70; 70); string-heavy 478/526/637/721 (88; 100). "a thread adds
  about one nursery (35-70 MB)" (`D/DESIGN-PROPOSALS.md:5142-5148`).
- Block churn: shrinking after every cycle minted "5x-56x" the blocks (raytrace 65,545 against 2,161), 100-400 ms per benchmark,
  "present in 20 of 24", est. +5-8 % geomean (`D/PERF-RESULTS.md:700-718`, `D/SPEC-heap-history.md:1138-1159`). Holding the pre-Full
  peak instead: 932 MB against 254 MB; no Full-cycle sweep: Bun's server at 3.0 GB against 0.8 GB
  (`D/SPEC-heap-history.md:1181-1204`).
- Window-liveness retention: JetStream with one extra *parked* thread 2.4-2.8 GB and 57-62 Fulls against 1.2-1.35 GB and 19-25
  (`D/SPEC-heap-history.md:1399-1405`); map-heavy 1,140/2,188/4,281/8,463 MB at 1/2/4/8 (`D/SPEC-heap-history.md:1308-1309`).
- Weak-bearing blocks: string-heavy at ONE thread 478-498 MB against 88 MB flag off (`D/DESIGN-PROPOSALS.md:5104-5105`). "The 478
  MB ... went unseen because the gate measures time only" (`D/DESIGN-PROPOSALS.md:5415-5417`).

---

### C: 12. Root-cause classification: (a) JIT-only, (b) object model, (c) C++ runtime / heap

(c) = stays in any engine that puts several mutators on this heap. The last column is **[inference]**.

| # | Problem | Class | AOT-only design |
|---|---|---|---|
| 1 | Non-iso allocators are per server; directory bits read lock-free by "the" mutator; steal; precise registry; eden bit; weak-set splice (§5.2/§5.6) | c | needed unchanged: per-thread LAs, striped slow-path lock, I1/I5b/I8/I16 |
| 2 | Accounting, DeferGC depth, mutatorState, didDeferGCWork singular | c | unchanged |
| 3 | One nursery budget for k threads; N-stack floating garbage; cycle-end block retention | c | unchanged (policy; take the final form directly) |
| 4 | Compiled code bakes `LocalAllocator*` | a in form, c in substance | AOT code cannot bake a pointer at all: needs "constant slot index + per-thread table" from day one. A pinned thread register or TLS base removes the `loadVMLite` hop; a fixed-capacity table makes the bound check removable |
| 5 | Unpatchable lazy slow path GIL off (46.6 M thunk runs) | a | does not exist without patching; the lesson "a null allocator silently turns every allocation into a call" stays |
| 6 | Inline path and C++ path choosing different size classes | a/b | applies to AOT inline allocation too |
| 7 | Two-party `m_worldState` handshake; who conducts; access protocol; F8 | c | unchanged |
| 8 | Trap delivery / park tickets per thread | c (VMManager, VMTraps) | needed; AOT code must poll (no signal-based invalidation points). Poll *cost* is a code-generation subject, not the heap's |
| 9 | Composition with non-GC stops via GCL | mixed: the stops exist for watchpoint fires and jettison (a), for `haveABadTime`/kind changes/flatten (b), for heap walks (c) | fewer reasons to stop, but heap walks and object-model stops remain, so the GCL bracket stays |
| 10 | Compiler threads suspended for epoch reclaim; `completeAllJITPlans`; ISB generation bump at each close; scratch buffers/side state as roots | a | drop |
| 11 | Epoch reclamation of IC handler chains / call-link records | a | drop those users; keep the primitive if any C++ side table is read lock-free (the `Weak<JSArrayBuffer>` case; the slot quarantine hook is b) |
| 12 | Barrier slow path's append to one mark stack | c | unchanged (lock, or per-client stacks) |
| 13 | `mutatorShouldBeFenced`/threshold addresses baked in code | a in form | AOT code loads them from memory anyway; per-thread copies are cheap to adopt. On arm64 the flag also orders cell initialization before publication to another *mutator* (b/c) |
| 14 | Black allocation, versioned liveness | c, already N-safe | unchanged: flips in-window only, flush once per window |
| 15 | `visitChildren` racing N mutators | b + c | audit every visitor of multi-word mutable state; cell-lock no-park rule |
| 16 | Conservative scan of N stacks; registration; parked snapshots | c | unchanged. If AOT frames or an interpreter use a separate stack (C_LOOP-like), per-thread stacks must be iterated: the earlier project never did that |
| 17 | Destructors / finalizers on any thread; conductor re-entry | c + embedder | unchanged; the embedder contract is the expensive part |
| 18 | Weak handle protocol; weak-bearing blocks | c (made acute by b: a Structure's single transition is a `Weak<>`) | unchanged; adopt the two-half sweep early |
| 19 | WeakMap/WeakSet/FinalizationRegistry tables | c (cell-internal C++ state) | unchanged |
| 20 | Strong set, protect set, mark-list set, weak-table registry, memo caches in `Heap` | c | unchanged |
| 21 | `HeapIterationScope` users, snapshots, statistics | c | unchanged (`deleteAllCode` itself is mostly a) |
| 22 | Under-marking of in-flight cells (June) | never identified; went away after b rewrites and c stop-gap fixes | keep `--verifyGC` and scribble/zombie lanes from the start |
| 23 | Growth-by-copy garbage raising the GC count (gbemu 45 against 17 collections) | b | depends on the new object model |
| 24 | Closed-loop conductor loses the whole cycle | c | unchanged; decide on a service conductor / lone-conductor rule up front |
| 25 | Per-thread quota / OOM attribution | c | unimplemented there too |
| 26 | Flag-off cost of atomics and mode tests in marker and allocator leaves | c (compiler effects) | applies if one binary serves both modes |

**Needed unchanged [inference]:** sections 1, 3, 5, 6, 7, 9, 10 in full; of section 4 the window model, the barrier-append
fix, black-allocation rules and the cell-lock no-park rule; of section 8 only the primitive. **Droppable:** compiler-thread
suspension, code-patching sync, JIT stub/handler retirement, scratch-buffer roots, the null-allocator gating phase.

---

### C: 13. Late bugs that teach a lesson

1. **A thread's exit made its young cells look dead.** Teardown used `StopAllocatingMode::ForGood`, which "does not record the
   cells allocated since the last collection in the newly-allocated bitmap, so those cells looked dead to the heap that kept
   running"; seen as optimized code reading a stale closure variable. Fix: a resumable stop for client teardown
   (`T/heap-client-exit-closure-reads.js:5-12`, `S/heap/LocalAllocator.cpp:189-193`). *Lesson: "for good" paths were written for heap
   death, not for a client leaving a live heap.*
2. **A workaround for an unidentified bug cost more than the bug's class.** The window-witness constraint (section 5): +37 % time
   and 28x RSS when introduced (`D/SCALEBENCH.md:562-570`); retained the conductor's own fresh garbage
   (`D/AUDIT-upstream-since-rebase.md:1398-1414`); made tests that expect `gc()` to collect fail (`D/LANDING-PLAN.md:866-877`); 10x
   slower continuous collection in Debug (`D/LANDING-PLAN.md:1964-1968`); blocked a service conductor, whose client "makes two"
   (`D/SPEC-heap-history.md:1405-1408`).
3. **One bit with two meanings.** Its precise-allocation leg used `isNewlyAllocated`, which `PreciseAllocation::flip()` sets for
   everything marked in the previous cycle: "a precise cell ... that was ever marked was never freed again while two clients were
   attached" (`D/SPEC-heap-history.md:1306-1331`). One parked thread was enough.
4. **"Left for the next world-stopped sweep" with no such sweep.** Weak-bearing blocks were never swept in steady state: "12,350 of
   50,000 destructible cells never destroyed after twenty synchronous collections" (`D/SPEC-heap.md:302`,
   `D/SPEC-heap-history.md:1287-1289`). Found only by a test that counts destructor runs.
5. **The fix of 4 was quadratic under back-to-back Fulls** (800 blocks re-swept at each of 20,000 cycle ends a minute; timeouts "first
   read as load") (`D/SPEC-heap-history.md:1333-1356`); **the budget that fixed 5 is unbounded growth** for programs making more than 32
   such blocks per cycle (26 -> 372 MB with under 10 MB live) (`D/DESIGN-PROPOSALS.md:5102-5122`). *Lesson: any constant budget is
   below some program's production rate.*
6. **Freeing an unswept block.** The eden retention arm freed an empty weak-bearing block the budget had not reached; `~WeakSet`
   crashed; "a loop that only makes `ArrayBuffer`s crashed a GIL-off process after half a second" (`D/LANDING-PLAN.md:4394-4396`,
   `D/SPEC-heap.md:300`, `T/heap-shared-shrink-frees-unswept-weak-bearing-blocks.js:2-7`).
7. **Fixes unmask older hazards.** The conductor self-deadlock (10.4b) "is as old as the in-stop sweeps, but this round's fixes made
   it live": before fix 3 no dead precise cell was ever destroyed with two clients (`D/SPEC-heap-history.md:1374-1379`).
8. **Retention policy needs the program's period, not a threshold.** Five forms in three rounds (`D/SPEC-heap-history.md:1138-1247`);
   the last bug: "half the excess" was shed from the first directories, exactly the ones the churn refills, so 12,600 blocks were
   minted where 150 were needed; fix = never free a block the just-ended cycle allocated into.
9. **Segmented-array drain underflow at thread exit**: `removeLast()` does not refill across segments; an exiting thread with more than
   one segment (~500 cells) of barrier appends crashed (`S/heap/HeapClientSet.h:128-143`, `D/SCALEBENCH.md:824-832`).
10. **A flag written after the release that publishes it**: `m_releasedByGCPark` was set after RHA and read by the collector once all
    clients had released; "a real race in every build" (`D/LANDING-PLAN.md:677-684`).
11. **End-phase work is not a mutator context**: republishing a call stub from the conductor's End phase put "a dead CodeBlock on the
    mark stack" (`D/LANDING-PLAN.md:3774-3778`).
12. **Rebases add heap walks and change lock-free assumptions**: snapshot start (section 9) and `WeakImpl::clear()` no longer a state
    flip (`D/AUDIT-upstream-since-rebase.md:2050`). The audit checklist has "new `HeapIterationScope` users" (`D/LANDING-PLAN.md:224`).
13. **TSAN cannot see initialization done by generated code**: a report that read as "a reachable cell in a dead block" took two rounds
    to prove benign with the GC verifier (`D/LANDING-PLAN.md:2678-2687`, `:2893-2904`). The June under-marking was the opposite:
    "Masked by instrumentation", Release-timing only (`D/SCALEBENCH.md:315-319`).
14. **Client-count heuristics**: five consumers of `clientSet().size()` are moved by any non-allocating client
    (`D/DESIGN-PROPOSALS.md:5052-5068`). Key them on *allocating* clients.
15. **"Relaxed atomics compile to the same code" was false** (section 11).
16. **Embedder liveness queries**: `MarkedBlock::Handle::isLive(cell)` from a plain mutator needs the BVL GIL off
    (`D/SPEC-heap-history.md:1107-1111`).

---

### C: 14. Not found / not sure

- **Root cause of the June under-marking**: the documents say it was never identified (`D/SPEC-heap-history.md:1410-1416`). I did not open
  `Tools/threads/bughunt/prev-gc-undermark-20260610/EVIDENCE.md`.
- **Root cause of the "GC-independent" shared-heap crash** (`D/SCALEBENCH.md:589-618`): run 3's tree is called "fully green"
  (`D/SCALEBENCH.md:750`) but I did not find the fix's description.
- **"The persistent wrong value under heavy foreign churn"**: open; the allocator hypothesis (one free cell handed to two allocators) was
  read and not confirmed (`D/LANDING-PLAN.md:5225-5228`, `D/PARITY-PLAN.md:1429-1432`).
- **Which thread runs a FinalizationRegistry callback GIL off**: I read the scheduling call and the timer's routing rules, not the code
  path that decides carrier or registrant. No test named for it was read.
- **WeakRef kept-alive semantics per thread**: I saw only the single VM-wide version counter. Whether one thread's end of a job can release
  another thread's `deref`-kept object is not stated in what I read.
- **Is the barrier always fenced GIL off on x86?** Documents disagree with the code. `D/DESIGN-PROPOSALS.md:5217-5218` and the banner at
  `S/heap/Heap.cpp:191-193` say always fenced GIL off ("`|| isGILOffProcess()` arm"); the function body keeps it raised only for non-x86 or
  `GILOff && useConcurrentSharedGCMarking` (`S/heap/Heap.cpp:5340-5344`), the latter false by default. I take the body as the truth:
  on x86 it follows marking, and is therefore raised during the concurrent window.
- **HC-02, HC-05..HC-13 fixes**: I verified HC-01, HC-03 and HC-04 in the source; the others only as rulings.
- **`D/SPEC-congc-history.md` rev log (lines 8-1303), `D/SPEC-heap-history.md` §§16-23, `D/INTEGRATE-heap.md`, `D/INTEGRATE-congc.md`,
  `D/CONGC-HANDOUT.md`, `D/SPEC-ungil-history.md`**: not read (annexes CGA1, CGB1, CGN1, CGD1, CGD9 were).
- **`D/SCALEBENCH.md` §§26-29, 31, 33-40, 44-48**: headings only. **`S/heap/Collector.{h,cpp}`** (an upstream refactor that now owns the phase
  loop): only grepped.
- **arm64, macOS, Windows**: "nothing was built or run" (`D/LANDING-PLAN.md:5234`); every number above is Linux x86-64.
- **Multi-VM shared heap**: out of scope there ("one VM per shared server", `S/heap/Heap.cpp:7996-7997`); VMManager's counters with Workers
  under a shared stop have a designed, unlanded fix (`D/LANDING-PLAN.md:5229-5232`).
- **Tests**: I read header comments of 8 `heap-*.js`, 3 `congc-*.js` and 6 `gc-stress/*.js`; I ran nothing.

---

## Appendix D. Performance decomposition, scaling, flag-off cost, decisions

(Original title: r9/perf: where the GIL-off time went in the JIT threads project, and what its owner decided)

Read-only research over `vendor/WebKit/docs/threads` (branch `sosuke/threads`, oven-sh/WebKit#249). Nothing was built or run.

**Citations.** `XX:n` = file and line, relative to the WebKit root `<threads-branch>/`:

| tag | path | tag | path |
|---|---|---|---|
| PR | `docs/threads/PERF-RESULTS.md` | SB | `docs/threads/SCALEBENCH.md` |
| PP | `docs/threads/PARITY-PLAN.md` | GT | `docs/threads/GILOFF-TAX-EVIDENCE.md` |
| DP | `docs/threads/DESIGN-PROPOSALS.md` | SH | `docs/threads/SHAREDHEAP-ALLOC-EVIDENCE.md` |
| FO | `docs/threads/FLAG-OFF-LANDING.md` | MB | `docs/threads/MAP-BIMODAL-EVIDENCE.md` |
| LP | `docs/threads/LANDING-PLAN.md` | FG | `docs/threads/FLAT-GAP-EVIDENCE.md` |
| BE | `docs/threads/BENCH.md` | TH | `THREAD.md` |

"[mine]" marks arithmetic or reasoning that is mine and is in no document.

### D: 0. Four corrections to the brief, before anything else

1. **"1.06 x 1.05 x 1.16" is not the decomposition of 1/0.76.** It factors **GIL off / GIL on = 1.30 in whole-test
   *instructions*, over 15 JetStream tests** (DP:120-123, DP:6160-6168, PR:1425-1426). 0.76 is the *score* against `main`.
   The product is 1.291 and GIL off's main-thread *cycles* against `main` are also 1.291 (PP:182): a coincidence [mine].
   The decomposition **in time, against `main`** is the ladder of PP 2.3 / PR 6.14 (A2.2 below) and it reads very
   differently: polls and tags are about 3 points, "eleven points are in none of the switches" (PP:29-37).
2. The suite is **JetStream 2**, 36 tests in the `jsc` shell: the default list "minus the four WebAssembly tests,
   `bomb-workers` and `segmentation`" (PR:380-384).
3. **JetStream never spawns a thread**, so every "GIL off" score is a process that has the configuration on and has not
   used it (PP:49-52). A second gate with one parked thread was added only in the eleventh session.
4. `SDn` in the documents is "**S**emantic **d**elta vs phase 1", not "standing decision" (`docs/threads/SPEC-ungil.md:1020-1044`).

---

### D: A1. Headline scores

#### D: Configurations, machine, builds

- "Flag off" = branch binary, no option. "GIL on" = `--useJSThreads=1`. "GIL off" = that plus `JSC_useSharedGCHeap=1
  JSC_useThreadGIL=0 JSC_useThreadGILOffUnsafe=1` (PR:13-16).
- "x86-64, Intel Xeon Platinum 8488C, 32 cores / 64 threads, 247 GB, Linux 6.12" (PR:20-21); later described as "a virtual
  machine on a Sapphire Rapids core" (PR:1609). clang 21.1, `-O2 -g`, JSCOnly port, static JSC, `USE_BUN_JSC_ADDITIONS`, `jsc`
  shell (PR:22-23). `main` is always the merge base, same compiler and flags, same machine, same session (FO:120-122).
- **Linux x86-64 only, ever**: "Nothing has been built or run on macOS, Windows, musl or arm64 in any round" (FO:1052).

#### D: JetStream 2 totals by round (geomean of 36; quiet pass)

| round (date) | `main` | flag off | GIL on | GIL off | off/main | on/off | GILoff/GILon | GIL off/main | cite |
|---|---|---|---|---|---|---|---|---|---|
| 6th | | | | | | | 0.77 (`aes` at 0.19) | | PR:571-572 |
| 7th | | | | | 0.975 | 0.865 | 0.830 | | PR:391-398 |
| 8th (09-09) | 355.8 | 340.9 | 306.0 | 262.2 | 0.958 (0.971 over ten runs) | 0.898 | 0.857 | 0.737 [mine] | PR:388-399 |
| 9th (09-11) | 353.5 | 339.8 | 308.4 | 257.0 | 0.961 | 0.908 | 0.833 | 0.727 [mine] | PR:1138-1143 |
| 10th (09-15) | 347.3 | 345.4 | 330.9 | 265.9 | 0.994; 0.97-0.98 without bimodal earley-boyer | 0.958 | 0.804 | 0.766 | PR:1297-1309, PP:167 |
| 11th session, medians of 3 | | | 0.947 of main | **0.758 of main** | | | | **never spawned 0.758; one parked Thread 0.743** | PP:158-165 |
| 13th | | 0.989 | | | | | | | FO:44 |
| 15th | | 0.991 +- 0.006 (thin LTO 0.996) | cycles 1.010 of before | cycles 1.015 of before | | | | | FO:19-29 |
| 17th (10-14) | | 0.9881 +- 0.0050 (thin LTO 1.0076 +- 0.0082) | unchanged | unchanged (thin LTO +0.7 % instr) | | | | | LP:4901, LP:4964, LP:4971-4985 |

The threaded modes were **not optimized after the tenth round**: rounds 12-17 "worked on one goal only ... The threaded modes
were not optimized; they were kept from getting worse" (LP:4091-4092). So 0.76 is the tenth round's state, minus about one
percent paid in round 15 (FO:28-29).

Sub-scores (11th session, /`main`): GIL off Startup 0.794, Worst Case 0.735, Average 0.757; with a parked Thread Worst Case
0.697 (PP:160-162). The 1.5 points of a live thread are "all in the Worst Case sub-score": "the exit-count thresholds are
doubled ... and a stop has a second client" (PP:168-172), with identical main-thread instructions.

Per-test table of the last full pass: PR:1315-1352. GIL off / GIL on below 0.80 in thirteen tests: sha256 0.40 (bimodal), ML 0.56,
splay 0.67, aes 0.67, delta-blue 0.71, Basic 0.71, OfflineAssembler 0.71, hash-map 0.74, FlightPlanner 0.77,
json-parse-inspector 0.78, UniPoker 0.78, WSL 0.79, regexp 0.79 (PR:1310-1313).

Micro set (26 rows, ms, five configurations incl. `main --usePollingTraps=1`): PR:40-68 (8th), PR:1356-1383 (10th). Other suite:
the big threaded program against Go and Java (SCALEBENCH, June; A4).

#### D: How noise was handled (this changed three times; the lessons are reusable)

- Rounds 5-10: medians of >= 5 runs, configurations interleaved, "nothing else running" (PR:28-30, PR:384-387). Bimodal tests named
  per pass (earley-boyer is bimodal on `main`: 926 or 545, PR:1303-1306; FlightPlanner "bimodal on every build", FO:244).
- Round 9 on: judged "by instruction counts before any timing" (`perf stat -e instructions:u`, `--useConcurrentJIT=0
  --useConcurrentGC=0 --numberOfGCMarkers=1`) (PR:1010-1016).
- 11th session: **whole-process instruction counts are "useless as a budget"** (they include compiler and collector threads and
  "weigh a locked instruction at one"); the working measure became **main-thread cycles**, `perf stat --no-inherit` (PP:27-28,
  PP:134-141). Relation used for all budgeting: "score times the main-thread cycle ratio is 0.98 to 0.99 in all four
  configurations"; "**A score of 0.90 is a main-thread cycle ratio of 1.09**" (PP:145-147).
- Two silent traps: `perf record -c 1-2M` was throttled and dropped 25-50 % of samples "where the instruction rate is highest,
  which is poll-dense GIL-off code"; an unquoted zsh variable made runs "silently GIL on" (PR:1416-1422).
- Round 13 on: exact per-function counts under callgrind, because sampled profiles "skid across calls" (PR:1547-1551). Round 15 on:
  mean of per-round paired ratios with a standard error (FO:1247-1249); "passes differ by more than their errors say and only
  binaries of one pass can be compared" (LP:4908-4910).
- The score is not time: it "weighs the first iteration and the four worst as much as the other 115" (FO:65-66).

---

### D: A2. The decomposition of the GIL-off cost

#### D: A2.1 In instructions, against GIL on (L-0): the 1.06 x 1.05 x 1.16

Method: **run-time option ablation on one binary, one run per cell, whole JetStream test, `perf stat -e instructions:u`**
(DP:6136-6138, command DP:6170-6172). A = GIL on. B = A + `--usePollingTraps=1` ("the poll as GIL on models it: three instructions, no
heap write"). C = A + `--useJSThreadsSingleOwnerWithGIL=0` ("tagged butterfly words, every tag predicate in every tier, the
flag-on object-model bodies in C++"). D = both. E = GIL off.

| factor | definition | geomean of 15 (14 without FlightPlanner) | range | cite |
|---|---|---|---|---|
| polls B/A | the poll as an instruction sequence | 1.058 (1.065) | json-parse 1.000 ... delta-blue 1.193 | DP:6140-6157 |
| tags C/A | tag predicates in generated code + flag-on object-model bodies in C++ | 1.042 (1.060) | richards 0.996 ... ai-astar 1.190 | same |
| both D/A | "the floor" | 1.114 (1.121) | | same |
| rest E/D | "what only a GIL-off process does" | 1.163 (1.173) | richards 1.088 ... crypto 1.311 | same |
| total E/A | | 1.296 (1.315) | | same |

What "the rest 1.16" contains, verbatim list: "the poll's heap writes (the visibility rule), the concurrency protocols in
generated code (claim compare-and-swap in transitions, compare-and-swap length raises, seqlock-validated Map reads, per-thread
allocator resolution, the second bounds check against the vector length), operations that are inline GIL on and calls GIL off
(MakeAtomString, Map/Set mutation and iteration, WeakMap reads, array join, generators, promises), and the `gilOff`-keyed C++
bodies (RegExp, per-thread scratch, locked structure tables)" (DP:6162-6166). Consequence drawn: "a process that ran untagged
until its first spawn would win back the 1.05, not the 1.30" (DP:6167-6168).

#### D: A2.2 In time, against `main`: the ladder (the table to trust)

Method: **one throw-away Release build of the branch head with nineteen run-time switches** ("prototypes of the designs that
could be prototyped cheaply, ceilings of the others; not in the tree", PR:1502-1504); main-thread counters, one test per process,
two runs on an idle machine, per-test minima, geomean of ratios to `main` over 36 tests; scores from one quiet pass, medians of
three (PP:132-141). About 1,900 changed lines, some switches "knowingly unsound with a second thread" (PP:1385-1391).

| cumulative configuration | wp instr | main-thread instr | **main-thread cycles** | score (quiet pass) | cite |
|---|---|---|---|---|---|
| flag off | 1.018 | 1.015 | 1.011 | | PR:1512 |
| GIL on | 1.032 | 1.037 | **1.050** | 0.947 | PR:1513 |
| GIL on + polls in optimized code + tagged words ("the floor") | 1.141 | 1.163 | **1.132** | (single-test 0.906) | PR:1514, PP:181 |
| **GIL off, branch head** | 1.318 | 1.349 | **1.291** | **0.758** | PR:1515 |
| same + one parked spawned Thread | 1.318 | 1.349 | 1.294 | 0.743 | PR:1516 |
| + design set (D-B1..B4 polls, RegExp entry, one-pass append, lone conductor, MakeAtomString, join, fused predicate, fresh owner) | 1.266 | 1.289 | 1.251 | | PR:1517 |
| + no machine-frame entry poll | 1.256 | 1.285 | 1.252 | | PR:1518 |
| + Double family as GIL on before the spawn, no lane verification, no second bound | 1.187 | 1.212 | 1.216 | 0.804 | PR:1519 |
| + every FTL poll writes nothing | 1.174 | 1.198 | 1.206 | | PR:1520 |
| + no poll in optimized code | 1.152 | 1.172 | 1.195 | | PR:1521 |
| + untagged words in every tier and C++ | 1.104 | 1.123 | **1.178** | **0.832** | PR:1522 |

Reading, quoted (PP:207-222):
1. "**Cycles, not instructions.** ... The extra instructions are cheap: polls, masks and compares retire beside the useful work."
2. "**Polls and tags are worth little in time once D-B1 is in.** Every remaining poll gone: 0.9 % of cycles. Every poll writing
   nothing: 0.8 %. Untagged words everywhere: 1.4 % (3.5 % without the artefact at most). No machine-frame entry poll: nothing
   measurable."
3. "The design set is 3.4 % of cycles, the Double family with its lane checks 2.9 %."
4. "With everything prototyped and every ceiling, GIL off is at 1.178 of `main`'s cycles; GIL on is at 1.050. Twelve points, a
   third of the original distance, are in none of the switches."
5. "**Locked instructions.** ... 0.48 locked loads per thousand instructions against 0.25 on `main` and 0.34 GIL on. At twenty cycles
   each the difference to `main` is 4.7 % of `main`'s cycles (GIL on: 1.0 %). No switch moves it."
6. "**Both flag-on modes pay 5 % of time over `main`**."

The floor: "A GIL-off process in which everything else cost nothing would sit at about 0.87 of `main`" (PP:229-231).

#### D: A2.3 The ledger 1.29 -> 1.09: factor, fix, what the fix recovers (PP:356-370)

Multiplicative steps in main-thread cycles.

| row | what | fix (package) | measured step | kind | pess./central/opt. |
|---|---|---|---|---|---|
| A | design set | D-B1..B4, F-D1/F-D7, D-AG-1, lone conductor, L-D4, L-D10, L-D3(a)(c) | 0.966 | prototypes | 0.966 |
| C+D | Double family with threads alive; lane verification withdrawn; second bound hoisted | C-D3 | 0.971 | C-D2 prototype + two ceilings | 0.975 / 0.972 / 0.971 |
| | **after A-D** | | **1.216, score 0.804** | measured | |
| K | thread multiplier, the stop's second client (threads alive only) | K D6 | 1.020 | measured G-U/G-T difference | 1.015 / 1.010 / 1.000 |
| E | polls that write nothing (visibility rule) | not planned | 0.992 | ceiling | 1 / 1 / 0.996 |
| P | polls that remain in optimized code | T-3 (contingency) | 0.991 | ceiling | 1 / 1 / 0.994 |
| T | tag sequences | T-4, T-2 | 0.986 to 0.965 | ceiling with an artefact | 0.997 / 0.993 / 0.985 |
| **R** | **the remainder: locked instructions, generated code beyond polls and tags, the C++ families** | R-1..R-4, P4.5/4.6 | 0.895 to reach GIL on | **not prototyped** | 0.966 / 0.945 / 0.927 |
| F | what both flag-on modes pay over `main` | P6 (D-M1..M6, E, F) | 0.952 to reach `main` | derivations | 0.986 / 0.976 / 0.971 |
| | threads alive: cycles; score | | | | 1.18 / 1.12 / 1.07; **0.835 / 0.87 / 0.915** |
| | never spawned with the U track | U-1 | | | **0.865 / 0.90 / 0.925** |

"**Nothing prototyped gets near 0.90.** Every switch and every ceiling together: 0.832" (PP:376). "The plan is therefore **not a
plan that guarantees 0.90**" (PP:385).

#### D: A2.4 What is left after rows A-D, by family (PP:239-285)

Method: `perf record --no-inherit -e cycles:u`, GIL on against the `real` configuration, 18 tests with the largest remainder,
symbol differences grouped; cells are % of the test's GIL-on samples. **Means of 18 tests:**

| generated code | property tables, structures | strings | object model in C++ | allocation, collector | Map/Set/WeakMap | promises, generators | other | RegExp | compiler on main thread | calls, entry |
|---|---|---|---|---|---|---|---|---|---|---|
| **10.9** | **3.3** | 1.9 | 1.6 | 1.4 | 1.3 | 1.1 | 1.1 | 0.9 | 0.6 | 0.4 |

- Generated code 10.9: "poll and tag ceilings account for about 3 points of it. The rest is ... claim compare-and-swaps in
  transitions, compare-and-swap length raises, allocation through per-thread allocator resolution, validated Map reads, inline
  caches through handler chains, calls where GIL on is inline" (PP:261-265). Locked loads by address: "70 % of delta-blue's are in
  generated code, 47 % of Basic's, 29 % of aes's; delta-blue retires 6.9 M against `main`'s 0.8 M, about a tenth of its GIL-off
  time" (PP:265-268).
- Peaks: json-stringify-inspector 26.2 (property walk under the structure's lock), async-fs 15.8 (cell-locked async generator
  queues), Basic 10.8 (WeakMap under the cell lock), json-parse 10.1, FlightPlanner 10.6 + 8.9 strings, OfflineAssembler 8.5 RegExp
  (PP:241-258, PP:269-280).
- Sum: "13.6 % of these eighteen tests' time in the C++ families and 8 % in generated code beyond polls and tags, against 3.4 %
  of the suite for the whole design set" (PP:282-285).

#### D: A2.5 Per tier

**(a) Score ratio GIL off / GIL on with tiers removed** (7th round's ledger, on the 6th round's binary, 15 worst tests, 3 runs;
PR:612-634). "a ratio that rises toward 1.0 as tiers are removed says the deficit is in the code the removed tier generates; one
that stays low says it is in the runtime."

| | full | no FTL | Baseline only | **LLInt only** |
|---|---|---|---|---|
| geomean of the 15 rows [mine] | 0.65 | 0.69 | 0.84 | **0.89** |
| range, LLInt only | | | | splay 0.79, json-parse 0.84 ... crypto 0.98 |

"Only `crypto` is a pure optimizing-tier case ...; the rest keep most of their deficit down to baseline-only and much of it in
LLInt-only, i.e. it is in C++ paths the interpreter also reaches" (PR:640-644). **This is the closest thing in the documents to "GIL off
with no JIT": about 11 % on the worst tests, on an old tree** (block churn, RegExp lock etc. were fixed later; LLInt caches were
not). No later no-JIT GIL-off measurement was found.

**(b) Steady-state instructions per loop iteration, GIL on -> GIL off, per tier** (10th round final; difference of runs at N and 3N;
DP:6176-6200). Selected rows:

| loop body | LLInt | Baseline | DFG | FTL |
|---|---|---|---|---|
| empty | 132 -> 132 | 39 -> 40 | 13.1 -> 16.0 | 6.7 -> 8.2 |
| arrayread | 238 -> 247 (+9) | 80 -> 84 | 19.9 -> 26.9 | 11.8 -> 18.5 (+6.7) |
| arraywrite | 196 -> 210 (+14) | 73 -> 87 | 15.4 -> 24.7 | 10.5 -> 13.8 |
| arraypush | 478 -> 1332 (+854) | 221 -> 297 | 34.8 -> 55.6 | 35.7 -> 50.9 (+15.2) |
| readinline | 293 -> 303 | 98 -> 104 | 28.8 -> 34.1 | 18.5 -> 22.1 |
| readool (out of line) | 296 -> 315 (+19) | 102 -> 114 | 29.8 -> 37.0 | 20.1 -> 24.0 |
| protoread | 296 -> 986 (+690) | 103 -> 115 | 25.5 -> 30.7 | 17.3 -> 19.8 |
| writeool | 251 -> 272 (+21) | 98 -> 103 | 30.0 -> 45.4 | 17.4 -> 29.3 (+11.9) |
| newobj | 854 -> 2331 (+1477) | 264 -> 306 | 38.4 -> 57.5 | 6.5 -> 9.7 |
| addprop (seven adds) | 1818 -> 6342 (+4523) | 486 -> 626 (+140) | 128.1 -> 156.9 | 104.9 -> 136.3 (+31.4) |
| newarray | 697 -> 794 | 472 -> 564 | 47.9 -> 70.8 | 34.4 -> 57.6 (+23.2) |
| strconcat | 970 -> 1038 | 268 -> 295 | 65.0 -> 77.1 | 53.8 -> 71.2 |
| mapget | 440 -> 1245 (+805) | 230 -> 449 (+218) | 59.9 -> 308.9 (+249) | 43.0 -> 80.3 (+37.3) |

"in the LLInt the large rows are the disabled metadata caches ... in Baseline the tax is small except adds (+20 per add: the
claim-first handler) and array growth; in DFG and FTL the per-access tax is a few instructions and it is the number of accesses
that makes the total" (DP:6202-6207).

**(c) First-iteration instruction ratios GIL off / GIL on**: WSL 1.256, Basic 1.254, OfflineAssembler 1.206 ... ai-astar 0.984 (DP:6367-6370).

#### D: A2.6 Micro rows and the exact sequences

FTL micro loops, instructions per iteration, GIL on -> GIL off (`perf stat` over 1e8-3e8 iterations + Air listing; DP:711-721):

| loop | GIL on | GIL off | extra | what |
|---|---|---|---|---|
| `s = (s + i) \| 0` | 4.7 | 6.7 | +2.0 | the poll; every unrolled copy keeps its own |
| `s += o.x` inline | 4.7 | 6.7 | +2.0 | poll only; `o.x` is hoisted |
| `s += o.p9` out of line | 4.7 | 6.9 | +2.2 | poll only |
| `s = a.f(s)` inlined method | 4.7 | 8.8 | +4.1 | two polls: the loop's and the inlined callee's entry poll |
| `s += f(i)` inlined closure | 12.7 | 20.3 | +7.6 | two polls (4), closure variable re-read + check (3), `PutStack` |
| **`for (i < a.length) s += a[i]`** | **8.4** | **18.3** | **+9.8** | "the poll 2; `a.length` re-read 1 (visibility rule, B-2); bounds check against the re-read length 2 ...; bounds check against the hoisted storage's vectorLength 2-3 (history §39); the Int32 lane verification 3 (I41)" |

So the time rows int-loop 1.65, inline-property-read 2.00, flat-butterfly-read 2.01, proto-method-calls 1.70 "are polls and nothing
else"; `array-element-read` 1.75 "is one fifth polls, three tenths the visibility rule, two tenths §39, three tenths I41" (DP:723-726).

x86-64 sequences from FTL disassembly (DP:6209-6222):
- out-of-line **read**: `mov 8(obj),r` becomes `mov mask,r; and 8(obj),r` ("one instruction more; the segmented-dispatch test is
  **elided under the watched thread-local set, E1**").
- out-of-line **write (replace)**: 2 instructions -> 9 (`mov 8(obj),w; mov tag(spill),t; xor w,t; movabs mask; and; lea; cmp; jae slow; mov v,..`).
- **array read**: second `CheckInBounds` against vectorLength (2) + the mask.
- **poll**: `movabs $trapWord,r; testb $0x1f,(r); jne` (3; 2 with the address in a register).
- **inline allocation**: 6 (`cmpl $slot,bound(lite); jbe; mov table(lite),r; mov slot*8(r),r; test; je`) where GIL on bakes one `movabs`.
- **CheckTransitionOwner**: 7, once per object per run of transitions. **Butterfly install**: +2.
- constructor with 3 inline properties + 6 adds: 77 -> 127 straight-line instructions: "three polls 9, three allocator resolutions
  18, the owner check 7, two tagged installs 4, one more store barrier ... 6, a store-to-load forwarding the poll prevented 5" (DP:6224-6228).
- `am3` (crypto's inner loop), 7th round: FTL loop block 35 -> 54 Air instructions; 260 / 290 / 527 ms flag off / GIL on / GIL off (PR:812-830).
- kernels: navier-stokes `lin_solve` 31.0 / 43.1 / 82.0 instructions per inner iteration (GIL on / GIL off with Double arrays / GIL off as
  it is); ML `mmul` 14.7 against 43.7 (PR:1435-1438). "1.39x is the general GIL-off cost of an array loop (polls, the vector-length
  bound) and 1.90x on top of that is boxing" (DP:1295-1296).
- array micro (G instr per 2 M calls; main / flag off / GIL on / GIL off): append by index 0.83/0.84/0.85/1.28; push 0.77/0.78/0.79/1.12; first
  write to a literal 0.88/1.00/1.01/1.63; `slice` 0.32/0.32/0.33/0.50; `concat` 0.75/0.83/0.84/1.25 (PR:1243-1245).
- RegExp per call (main / flag off / GIL on / GIL off): `exec` 1,463/1,536/1,535/1,767; `test` 905/903/908/1,148; `replace`
  2,189/2,430/2,435/3,131; regexp `split` 2,316/2,647/2,649/4,105 (PR:1448-1450).
- length raises once the array structure's writeThreadLocal set has fired ("the state of any GIL-off program that shares an
  array"): routed to the runtime 3.2-3.9x; inline CAS-max 1.06-1.11x, "the `lock cmpxchg` per raise" (PR:1107-1128).

#### D: A2.7 The gap factors GF-1..GF-23 (DP:125-153), condensed

| id | mechanism | worth | proposed fix -> expected |
|---|---|---|---|
| GF-1 | polls at the entries of **inlined** callees; unroller clones header polls | 136 of delta-blue's 182 in-loop polls; raytrace 11 %, delta-blue 10 % of FTL instr | D-B1, D-B3 |
| GF-2 | visibility rule proper: control-deciding reads redone after each poll, with dependent checks | releasing every read: delta-blue -15.5 %, hash-map -14 % instr (40 % / 35 % of their gaps), 2-6 % on seven | D-B2; then D-B5 or D-B6 (decision 1) |
| GF-3 | `PutStack` writes the whole heap (parkable-slow-path predicate's default arm) | 126 of 182 in-loop polls keep the interim set | D-B4. D-B1..4 together -13 to -16 % instr on delta-blue, hash-map (DP:221) |
| GF-4 | Double family: Int32->Double served as Int32->Contiguous (T4-O), copies of Double sources boxed (T4-C); failed check on a merged local reports to no profile | sha256 0.40/0.72, ML 0.56, navier-stokes 2.68x instr; "about +4 % of the GIL-off total" | C-D1, C-D2 (pre-spawn only), C-D3 |
| GF-5 | array growth: store re-dispatched 3x around one copy, 960 vs 360 C++ instr per event; garbage -> gbemu 45 vs 17 GCs | aes -8 % | D-AG-1 |
| GF-6 | RegExp/strings: per-thread scratch, statics, stack limit looked up per use; `RegExpTestInline`, folding, cached result off | regexp 0.79, OfflineAssembler 0.71 | F-D1, F-D2, F-D3, F-D7 |
| GF-7 | `Array.prototype.join` two-pass joiner off | UniPoker 6.4 % | L-D10 |
| GF-8 | the conductor sits out every GC cycle | splay 0.67 (out of JS 265 ms vs 69 ms) | D-J1 -> splay 0.9-1.0 |
| GF-9, 10, 11 | stale optimized code re-entered from loops; forced exits for LLInt-profiled puts; LLInt caches disabled | 2 exits vs 903; +690/+627/+646 instr per op in LLInt | K D2, D-A1, D-A2 |
| GF-12 | `MakeAtomString` with a concat-key cache is a locked call | 22 % of WSL's excess | L-D4 |
| GF-13 | Map, Set, WeakMap: mutation/iteration as calls, WeakMap reads as host calls | WSL 13 % of excess; map-set-get 1.27 | L-D8, R-2 |
| GF-14 | `CreateThis` on a poly-proto function looks the prototype up by name | class-ctor-4 2.67 (mono-proto 1.5-1.8) | L-D6 |
| GF-15, 16 | reallocating transitions leave generated code; math ICs never regenerate | megamorphic-put-transition 2.0 | L-D7, L-D5 |
| GF-17 | structure tables under the structure's lock | json-parse 42 % of excess; JSON.stringify 80 % | L-D9, R-4 |
| GF-18 | per-access sequences (A2.6) | 77 -> 127 | L-D3, T-4 |
| GF-19 | generators (2 CAS per `next()`), async generators, promises (locked queues) | Basic, async-fs 1.21 | "not designed"; R-2 |
| GF-20..20e | tier-up under N threads: 4^r recovery, entry-exit loops, fire stops knock all threads to Baseline, shared profiles/counters, live-thread multiplier | richards-like, string-heavy (A4) | K D1-D6 |
| GF-21 | Eden pauses with N threads | 3-5 % of a parallel leg | D-J3 (measure first) |
| GF-22 | segmented arrays: DFG/FTL exit on a segmented word | **243.7 vs 19.0 instr per element read** | D-AG-3's note |
| GF-23 | a process that has not spawned runs tagged code | 1.05 of the 1.30 | L-D2 (not recommended) |

#### D: A2.8 Earlier ledgers (history; most items fixed, but they show what a first implementation gets wrong)

- **June, `intcs` W=1** (perf record + uprobe exact counts; GT:12-19): GIL on 5,857 ms, + shared heap 6,676 (+819), full GIL off 7,831
  (+1,974). Ranking GT:200-211: (1) unpatched FTL lazy-slow-path thunk x 46.6 M, ~950-1,100 ms; (2) "per-cell 3-hop C++ alloc path x
  70.9M", ~550-650 ms; (3) `Lock.hold` path ~145; (4) JSCellLock x 9.27 M ~80-120 (8.8 ns per release); (6) trap check +1.1 ns x 42.5 M.
  Zero: CheckTraps clobber, segmented access at W=1, write-barrier slow path. Root cause of 1+2: "`m_allocatorForSizeStep` stays null
  under sharedGCHeap -> JIT inline allocation never works" (GT:213-216). Fixed: tax 1,912 -> 453 ms (SB:3244-3246).
- SH:155-171: fast path hit rate already 99.67 %; "The visible sharedGCHeap per-cell tax is allocator LOOKUP, not bump"; "A 'real TLAB' ...
  targets the wrong lever".
- **7th round ledger** (counters `--reportJSThreadsCounters=1` + six unsound knobs `--jsThreadsExp*`; PR:591-608), ranked PR:700-799: (1)
  marked-block churn, +5-8 %; (2) virtual calls never on the thunk's fast path +3-4 %; (3) dictionary / megamorphic ICs to generic C++
  +3-4 %; (4) Double arrays +4 % (`aes`: 0.75-1.5 M stop-the-world relabels a run); (5) Map/Set as calls +1.5 %; (6) RegExp compile
  check under the cell lock +1-1.5 %; (7) array codegen in loops +1 %; (8) rope resolution +0.5 %; (9) OSR exits via the operation
  +0.5 %; (10) generator claims. **Not** on the list though expected: parkable-slow-path clobber, stop frequency single-threaded,
  per-thread cache hit rates (PR:801-808). Afterwards: "none moved the geometric mean by more than 2 % alone" (PR:883-884).
- A fence bug worth remembering: the shared heap "raised the 'mutator must fence' flag at its first collection and never lowered it",
  a put loop 33.6 -> 60.7 ms (PR:270-275).

---

### D: A3. Which factors exist in AOT-compiled code with no JIT

Classes as in the brief: (i) loss of JIT speculation/patching; (ii) per-access checks on the object model; (iii) polls + visibility;
(iv) shared-heap allocation, barriers, collector; (v) C++ runtime locks and per-thread state. **The "applies" column and the
reasoning are mine**; the sizes and mechanisms are cited above.

| factor | class | in AOT, no JIT? | reasoning |
|---|---|---|---|
| poll as instructions (1.06 instr; <= 0.9 % cycles after D-B1) | iii | **applies, but maybe not as an increment** | GIL off polls because "asynchronous breakpoint patching is I2's violation with more than one running mutator" (DP:704). AOT text cannot be patched either, so single-threaded AOT code presumably already polls or has another stop mechanism; then threads add only the poll's *meaning*. `main --usePollingTraps=1` costs 1.055 instr / 1.005 cycles over the suite (LP:3563-3564), but tight loops 1.65-2.0x in time (PR:50-54) |
| polls at inlined callee entries (GF-1), cloned by the unroller | iii, artefact | **avoidable by construction** | it comes from `op_check_traps` at every `op_enter` being parsed into the caller (DP:705-708). An AOT compiler decides poll placement itself: one per loop header and per non-inlined entry is D-B1/D-B3 for free |
| visibility rule (GF-2; 0.8 % cycles after D-B1..4; 40 % of delta-blue's instr gap before) | iii | **applies to any optimizing backend** | the cost is lost LICM/CSE and the re-done dependent checks (DP:749-752). An AOT backend that hoists loads has the same choice c0/c1/c2. If AOT code is less optimized than FTL (hoists less), the rule costs less; if heap loads are plain loads for an LLVM-like backend, the default is silently (c2) |
| parkable nodes clobber heap facts (GF-3, B-4) | iii | **PutStack part n/a; allocation part applies weakly** | `PutStack` is a DFG classification accident (DP:811-820). "an allocation slow path can park for a collection" (DP:839-841) holds anywhere, but an out-of-line call is already opaque to an AOT optimizer |
| butterfly tag mask, owner test on writes, CheckTransitionOwner (1.05 instr; 1.4-3.5 % cycles) | ii | **applies, and probably costs more** | sequences are per access in any compiled code (A2.6). The FTL figures *include elision*: the segmented test is "elided under the watched thread-local set, E1" (DP:6210-6211), and TH:15 rests the whole "~zero" claim on watchpoints that let "JIT'd code elide the TID/SW checks entirely". Without invalidatable code there is no elision: every read needs mask + segmented test, every write the 6-9 instruction predicate. The C++ half (flag-on object-model bodies) applies unchanged |
| segmented butterfly path (GF-22) | ii | **applies; must be inline** | DFG/FTL *exit* on a segmented word: 243.7 vs 19.0 instr per read, and exit storms (MB:261-270, FG:1067-1071). AOT has no exit, so it needs a real in-line or out-of-line segmented arm from day one; forcing all arrays segmented "halves `gbemu`" (PR:964-966), reads "13 times slower today" (DP:341) |
| second bound against vectorLength (§39), Int32 lane verification (I41) | ii | **applies wherever a shape or storage fact is kept across a point where another thread can act** | "what makes a hoisted or stale butterfly memory-safe against a fresh publicLength" (DP:774-776). With true parallelism a check-then-load pair is never atomic, so AOT code that checks the indexing type and then loads a lane has the same hazard. Part of the 2.9 % |
| Double family, the object-model rule (T4-O, T4-C) | ii | **applies** | it is a C++ rule (`relabelIndexingShapeConcurrent`, DP:1282-1283): "raw-double lanes may not appear under a stale Int32-keyed reader" (PR:260-262). Boxed lanes cost 43 -> 82 instr per kernel iteration. The way out (C-D3: encoding-changing transitions as copies + a raw-double bit in the word) is tier-independent; "38 C++ reader functions behind 7 choke points" (DP:333-334) remain in a no-JIT engine |
| Double family, the amplifiers (exit batches 101/201/401/801, BadIndexingType storms, MultiGetByVal) | i | **n/a** | profile and reoptimization mechanics (DP:1258-1265) |
| claim CAS per property add; CAS length raise on push (locked instr; ~7 % of delta-blue) | ii | **applies** | "one locked instruction per added property on an object that no other thread can have a pointer to" (PP:405-409). R-1 (no claim on an unescaped object) is an ordinary compile-time escape analysis and is *more* natural in AOT |
| array growth re-dispatch, COW materialization (GF-5) | ii/v | **applies** (C++), fix D-AG-1 is C++ only | |
| per-thread allocator resolution (6 instr per allocation) | iv | **applies** | AOT cannot bake an allocator address per thread either; a pinned register or TLS base for the thread's table is the analogue of L-D3(b). SH:160-171 says the lookup, not the bump, is the cost |
| unpatched lazy slow paths, OSR exits never repatched, math ICs, virtual-call mirror, call-link records, handler ICs in FTL (GO-1, GO-4, GF-15/16) | i | **n/a as an increment; "different"** | all follow from "patching live code with several mutators is forbidden, SPEC-jit I2" (DP:6330-6331). AOT code never patches, so its ICs are data ICs already; what remains is making their publication and reclamation concurrent (append under a lock + one atomic publish, TH:17) |
| LLInt metadata caches disabled (GF-10/11: +627..+690 per op) | i/v | **applies in kind** to any multi-word inline cache in writable data | the cure proposed is "immutable single-pointer record" (DP:430). Any AOT data IC wider than one word has this problem |
| tier-up, exit thresholds x live threads, jettison storms, shared profiles/counters (GF-9, GF-20*, K) | i | **n/a** | no tiers, no profiles. This removes the *bimodality* class entirely (sha256, string-heavy, intcs, W=32), which was most of the scaling pain. It also removes row K (1.5 points with threads alive) |
| VM-global caches (megamorphic, HasOwnProperty, JSON atom, numeric string, split, concat-key) | v | **applies** | disabled -> generic C++ (ML 24.4 M GaveUps, PR:739-741) until made per thread (megamorphic-access 1.77 -> 1.13, PR:88-90). A no-JIT engine leans on these caches more |
| Map/Set/WeakMap (cell lock on mutation, seqlock-validated reads) | v | **applies, near the DFG/Baseline row, not the FTL row** | AOT calls C++: +218..+250 per `get` (DP:6199, DP:6206-6207); WeakMap 30 % of Basic's locked loads (PP:277-278). R-2 "owner-first cell lock" is C++ |
| structure tables, transition map, property walk under `m_lock` (3.3 %) | v | **applies**, likely larger | a no-JIT engine reaches `getConcurrently`, `forEachProperty`, transition lookup more often |
| strings: atomic refcounts in shared-atom-table mode, rope resolution under the cell lock, atom table shards (1.9 %) | v | **applies**; paid GIL on as well | "every `StringImpl::ref`/`deref` is a locked add" (PP:451); 94 M extra locked ops in one micro row (PR:1464-1466) |
| RegExp per-thread context, legacy statics, scratch, stack limit | v | **applies** | C++ entry points; Yarr JIT aside. `softStackLimitForCurrentThreadGilOffSlow` etc. (DP:6391) |
| generators, async generators, promises, microtask queue | v/ii | **applies** | "locked GIL off by design (SPEC-ungil section E)" (DP:6361-6362) |
| per-thread VM state through the lite (exception checks, `topCallFrame`, traps) | v | **applies** | the out-of-line selector "executes 0.97 % of all instructions" GIL off (LP:4532-4535); 19,000 expansion sites (LP:4364-4365) |
| collector: conductor sits out the cycle, block churn, Eden allowance, weak-bearing blocks, retention bugs | iv | **applies in full** | nothing in it is JIT-related (DP:4981-4996, PR:700-718, PR:1081-1093) |
| write barrier / mutator fence | iv | **applies**; on arm64 potentially "the largest single flag-on cost" | N6: fenced for the heap's life on non-x86; "1.4-2.7x on barrier-heavy microbenchmarks" when forced on x86-64 (DP:7532-7545) |
| load ordering on arm64 (structure check then inline slot; length then element) | ii | **applies to any compiled code on arm64** | N2: "Worth: a GIL-off crash class on arm64" (DP:7462-7491); fix is load-acquire guard loads (N-D2) |
| watchpoint fires as stop-the-world, jettison | i | **mostly n/a** | no code to invalidate. Runtime facts that C++ fast paths and AOT code test as data (species, having-a-bad-time) still need a publication rule. Not read in detail; see C |
| compile time (L-15), compiler on the main thread 0.6 % | i | n/a | |

**Rough re-weighting [mine, not a measurement].** Of the 1.291 cycles: removed with the JIT are row K, the exit/tier defects, part of row F
(handler ICs, call records) and the "calls where GIL on is inline" part of generated code. Kept are the C++ families (13.6 % on the 18
worst tests), locked instructions (4.7 % of `main`'s cycles), the collector, the Double rule, tags. Tags and segmented tests get
*worse* without watchpoint elision. The one direct datum is A2.5(a): LLInt-only 0.89 on the 15 worst tests of an old tree.

---

### D: A4. Scaling

#### D: Parallel-self suite (N threads of identical independent work; `Tools/threads/scaling-gate.sh`; medians of 5)

| workload | June run 1, 4 / 8 thr (SB:67-73) | 8th round 2/4/8 (PR:314-320) | 10th round final 2/4/8 (PR:1395-1401) | T(1) vs flag off, 9th (PR:1193-1199) |
|---|---|---|---|---|
| raytrace-like (small objects, doubles) | 0.67 / 0.65 | 1.67 / 3.27 / 5.71 | 1.63 / 3.33 / 4.84 | -6 % |
| splay-like (GC pressure) | 1.47 / 1.59 | 1.75 / 3.11 / 5.23 | 1.95 / 3.59 / 5.74 | +30 % |
| map-heavy | 1.11 / 1.13 | 1.88 / 3.34 / 5.13 | 1.84 / 3.26 / 4.30 | +77 % |
| string-heavy (computed keys, ropes) | 1.15 / 1.14 | 1.09 / 2.24 / 2.66 bimodal | 1.49 / 0.93 / 3.01 bimodal | +14 % |
| richards-like | 0.19 / 0.23 | not meaningful | 0.54 / 1.15 / 1.71 | +88 % |

GIL on: "speedup is 1.0 at every N by construction" (PR:344-345). Targets: 3.2x at four threads on clean workloads (PR:338-342); PP's G-S:
T(4) >= 3.2 x T(1) non-allocating, >= 2.4 x allocating (PP:85).

Why each did or did not scale:
- **Object creation** before instance-keyed ownership: `{}` + four adds 8 / 1,920 / 2,100 / 4,206 ms at 1/2/4/8 threads; after: 6.0 / 6.6 /
  8.2 / 9.6 ms (PR:369-376). Structure-keyed thread-locality collapses as soon as two threads build the same shapes.
- **raytrace-like**: "What keeps it under 8x is still the stop-the-world shared-heap collection" (PR:348-352).
- **splay-like**: the four-thread loss "was a shared cache line, not the collector": 8.2 % of cycles on two loads of one report word;
  written only on change: 3.11x -> 3.62x (PR:1074-1077). Eden allowance growing with allocating threads: 51 -> 13 collections (PR:331-333).
- **map-heavy**: scales from a bad serial base. Memory: 1,140 / 2,188 / 4,281 / 8,463 MB at 1/2/4/8 threads from a retention bug in the
  window-liveness constraint; 352 MB at one thread after the fix (PR:1081-1088); 10th round 167 / 256 / 351 / 548 MB (PR:1495-1498).
- **string-heavy**: first diagnosis (7th): 3.3 M structure transitions per thread, "structure creation (property-table materialization
  and copies, the transition table, structure allocation) and atomization under contention, plus collecting 13 M structures";
  `addToStringTable` 7.6x samples for 4x work, `lockSlow` 11x; stops and GC together only 7 % (PR:843-859). Later: bimodal because of
  tiering: a watched fire's stop makes "every parked thread jettison its on-stack optimized frames" and two tier-up defects keep all
  four threads in Baseline (DP:148; LP:5133-5137). Memory 478 MB vs 88 MB at one thread: weak-bearing (Structure) blocks recycled 32
  per cycle end (LP:5205-5207).
- **richards-like**: a benchmark artefact on `main` too (365 -> 2,498 ms over five serial invocations); "a fresh private copy per thread
  scales 3.9x at four threads" (PR:1488-1491).
- **Shared mutable state in lower-tier code**: "a two-instruction DFG loop 5.6x slower at two threads" from the shared execution counter
  (PR:276-283); "Baseline 50 -> 260 cycles per back-edge from one thread to two" (DP:149).
- **Eden pauses**: "a rendezvous costs about 0.04 ms per running mutator"; 3-5 % of a parallel leg (DP:151).

#### D: The big program against Go and Java (SCALEBENCH, June 2026, 64 HW threads, 3-rep medians)

| stage | JS W=1 | JS W=16 | speedup | note | cite |
|---|---|---|---|---|---|
| run 1 | ok | 41 of 42 runs at W>=2 died | | GC under-marking | SB:56-62 |
| run 3 | 33.3 s | 47.6 s | **0.70** ("negative at every W") | RSS 11.8-14.7 GB; GIL off W=1 = +122.5 % time, 32.9x RSS over GIL on | SB:779-795, SB:868-883 |
| v4, `intcs` arm | 16.7 s | 9.2 s | 1.81 (Java 2.02, Go 4.35) | GIL off/on W=1 1.445 | SB:2136-2149, SB:2158-2166 |
| §39b-§45 `intcs` | 8.2 -> 6.3 s | 6.4 s (slow mode) -> 3.3 s | ~1.9 [mine] | after TLC slots and the segmentation fix | SB:2879-2883, SB:3443, SB:3559 |
| §38 `flat` arm (typed arrays, no `[].push`) | 1.68 s | 0.485 s | 3.5 [mine] | "BEATS Java at every W ... and BEATS Go at W=1 and W=8" | SB:2639-2655 |

Causes found, in the order they fell:
- **GC was not the ceiling**: STW-GC "is 5.4% not the ceiling"; concurrent marking "buys ~nothing" (+-2.4 %) and added ~30 rendezvous
  windows at ~9 ms each; 52 % of W=16 wall was thread-0 serial work paying the GIL-off tax (SB:1326-1339, SB:1407-1447, SB:819-823).
- "~2/3 of the W=16 gap is **single-thread throughput**, not scaling" (FG:21-24).
- **Refcount bouncing**: `CodeBlock::jitCode()` returning a `RefPtr` by value on a hot GIL-off path: "~14.4 % of W=16 CPU" on one
  `lock incl/decl` pair (FG:507-514). A process-global OSR-exit generation lock: 22 % of threads spin-yielding (FG:515-520).
- **Per-instance Structures** for `Lock`/`Condition`/`Thread`: 128 StructureIDs at one site, "megamorphic is disabled under
  `useJSThreads`", permanent `operationGetByIdGaveUp` (FG:1041-1049).
- **Segmented `[].push`** on arrays pushed from several threads: 9.8-15.9 % of W=16; W=32 slow mode = 1.63 M `BadIndexingType` exits vs 818
  (FG:1053-1071).
- **Map bimodal** (phase A 1,600 or 4,600 ms, "zero intermediate values", MB:22-27): *not* Map, GC, rehash or cell locks (MB:171-176,
  MB:274-282). Root cause: a worker first touching the lazy `String.fromCharCode` = "foreign-TID structure transition on a Flat butterfly ->
  `convertToSegmentedButterfly` ... Segmented for life -> every D@40 GetButterfly takes the segmented branch -> BadIndexingType exit
  storm" and the recompile never learns (MB:261-270). Engine fix: foreign property-only transition without capacity growth stays flat and
  shared, plus an exit-site backstop (SB:3502-3522); 0/30 slow afterwards (SB:3557).
- `Lock.prototype.hold(fn)`: a native between caller and closure prevents inlining; 15.9 % self at W=16 even at the end (FG:246-251, SB:2667).

---

### D: A5. What flag off cost, and why it mattered

**Why.** TH:3: "~zero perf cost for single-threaded code". BE:3-4: "a hard constraint". LP:5273-5275: "about zero cost ... when the flag is
off". There is **no build flag**: "No build flag compiles the feature out ... every difference between flag off and `main` is a run-time test
or an unconditional change" (FO:676-677), so "the flag-on code is in every binary" (FO:111-112). After PARITY-PLAN it became *the* gate: "the
only one that gates shipping: **the branch built, the flag not set, is `main`**" (FO:5-6). Size mattered because "The embedder has worked
to take hundreds of kilobytes out of its binary" (FO:1384-1385); the interpreter because "the embedder's short-lived programs would" see it (FO:69-70).

| metric (flag off vs `main`) | 11th session (before the work) | 17th round (after six rounds) | target | cite |
|---|---|---|---|---|
| JetStream score | 0.966 (Startup 0.950, Worst 0.957, Avg 0.978) | 0.9881 +- 0.0050; thin LTO 1.0076 +- 0.0082 | 0.99, then >= 0.985 | FO:64-67, LP:4901, LP:4964 |
| whole run, main-thread cycles | 1.013 | 1.0035 +- 0.0006; LTO 1.0007 | 1.005 | FO:1205, LP:4893, LP:4957 |
| first iteration, cycles | 1.039 | 1.0084 +- 0.0009 (150 rounds) | 1.010 | FO:1202, LP:4894 |
| interpreter only (`--useJIT=0`) | **1.041** (+5.2 % instr; 2-8 % on 35 of 36 tests) | 1.0002 | 1.010 | FO:1203, FO:1294, LP:4897 |
| Baseline-capped / DFG-capped | 1.010 / 1.025 | 1.0050 / 1.0044 | 1.005 / 1.010 | FO:1204, LP:4899-4900 |
| locked loads, run / first iteration | 1.11 / 1.29 | 0.96 / 1.02 (13th) | 1.02 / 1.03 | FO:1206, FO:1220 |
| start-up instructions, empty script | +2.7 % | -9.5 % (+1.8 % with a `JSC_` variable) | +1 % | FO:258, LP:4902 |
| **start-up RSS** | +1.4 MB | **+868 +- 48 KB: the one metric not met** | +0.5 MB | FO:258, LP:4903 |
| peak RSS, suite in one process | +8 to +25 % | 1,898 vs 1,894 MB, no difference | +2 % | FO:1209, LP:4904 |
| text, plain build | +15.6 % (35.26 vs 30.50 MB); +19.1 % after per-mode copies | +9.344 % (+2,892,001 B) | | FO:1062, FO:59, LP:4994-4996 |
| text, thin LTO (what Bun ships) | +8.5 % (+3.7 MB, 5th round) | +6.661 % (+2,580,847 B) | +6.93 % | PR:494-496, LP:4966 |

Source size: 658 files, +87,069 / -6,320; "7,484 other lines in 1,638 functions of 473 files" run ungated, plus 1,972 gate tests; 26,150 static
tests of `gilOffProcess` (FO:675, FO:703-707, DP:75-76). 478 added members; "`JSCell`, `JSObject`, `Butterfly` and `JSString` layouts unchanged" (LP:4052).

**Mechanisms of the flag-off cost** (all C++; generated code +0.4 %, FO:67):
- "a hot leaf function gains one to twenty tests of a Config-page byte, each test's cold arm is inlined behind it, the function grows 1.2 to 4
  times in bytes, the register allocator spills around the cold arms, and callers that used to inline the leaf now call it" (DP:72-75).
- relaxed atomics on hot words: "the compiler may not combine, hoist or reorder it": 22 % of `operationMapSet`'s samples (PR:99-111); marking +6.4 %
  instr per collection, +13 per visited cell (PR:1476-1480). `thread_local` without `constinit` (PR:122-126).
- "fifteen locks taken unconditionally, sixty locked instructions `main` does not execute" (FO:131-132); locked `ArrayProfile` updates (FO:1281).
- "the gate in front of every property and call opcode" in the interpreter (LP:4075-4076).
- layout: `Structure` 128 vs 112 bytes made a hash set 1.6-2.0x dearer (PR:1592-1594); new source files shifted unified bundles and with them the
  layout of the whole library (LP:4835-4840).
- start-up RSS "All of it is the executable's text, counted in 64 KB windows" (LP:4835).

**What fixed it**: the mode as "one byte that the compiler treats as constant; functions are compiled once per mode and the copy without threads has
no gate in it" (FO:49-51; 192 slow paths, 109 JIT operations..., LP:4114-4118); two LLInt bodies for 27 opcodes (LP:4102-4103); threaded arms out of line
in `.text_jsc_threaded`, "which a process without threads never maps" (FO:1072-1073); `noinline` on arms because thin LTO inlined them back (LP:4826-4828).
Key experiment: "with every test of the process-wide byte compiled as false the whole run is 1.002" (LP:4108-4110). **Price**: threaded modes about one
percent slower (FO:28-29), thin-LTO GIL off +0.7 % instr more (LP:4981-4985).

---

### D: B1. Decisions and rulings of the project owner

Very few owner decisions are written in the repository. The documents mostly list *decisions asked of* the owner with a recommendation. Three
levels are kept apart below.

#### D: B1.1 Recorded in the repository as made

| # | decision | quote | cite |
|---|---|---|---|
| 1 | **Order of goals (2026-09-17): flag off first** | "the project owner set the order: first, the branch built with the flag not set must have `main`'s safety and speed, so that the work can be merged and shipped in the embedder's ordinary binary as an experimental feature that is off by default; the threaded modes mature behind the flag afterwards" | LP:4036-4040; LP:5037-5040; PP:9-13 |
| 2 | 0.90 is no longer a merge condition | "The gates G-U, G-T and G-S below are what comes after it and are reported as measured, not as conditions of the first merge" | PP:12-13 |
| 3 | **`bun:ffi` on GIL-off spawned threads: refused** | "Out of scope by standing decision"; "stays refused"; "The refusal on spawned threads stays either way (standing decision)" | LP:5258-5264; DP:363-364; DP:4086, DP:4242 |
| 4 | **WebAssembly on spawned Threads: out of scope** | listed under the same heading; only "Carrier-only execution GIL off is proposed (D1)". SD7: "spawned wasm TypeError, both modes" | LP:5265; `docs/threads/SPEC-ungil.md:1027` |
| 5 | **Fuzzers: out of scope for the landing rounds** | "The fuzzers and arm64 were ruled out of scope for the landing rounds, with the instruction that the documents list what each would need" | FO:1399-1400; LP:5266-5267 |
| 6 | **arm64 and non-Linux: out of scope** (support of the flag) | same sentence; "nothing was built or run" | FO:1399-1403; LP:5268-5269 |
| 7 | Spawned threads are not sampled by the profiler | "Spawned threads stay unsampled by ruling" (SD18) | LP:3747; DP:4112 |
| 8 | Legacy RegExp statics per thread GIL off (SD19) | "Legacy statics stay per thread GIL off (SD19 ...; recommended to keep)" | DP:386-387 |
| 9 | Flag-off generated code identical to `main` ("the LAW") | "byte-identical codegen LAW; every new branch gated on `Options::useJSThreads()`"; un-gating "not chartered without an explicit reviewer ruling" | `docs/threads/AUDIT-checktraps.md:218`; SB:2612; rule text FO:176-178 |
| 10 | Pushes are the owner's call | "a push to a CI-visible branch, which is the project owner's call each time" | FO:1151 |
| 11 | Concurrent shared-GC marking stays default off (June campaign decision; not attributed to the owner) | "`useConcurrentSharedGCMarking` stays **default `false`**" (later default-on via `3be6a06a7a65`, SB:2104-2105) | SB:1409-1413 |
| 12 | The round targets | 8th: GIL off >= 0.90 of GIL on, none under 0.75; GIL on >= 0.89; flag off >= 0.97 (PR:484-487). 9th/10th: 0.99 / 0.95 / 0.90, no test below 0.80 (PR:1138-1143, LP:3545-3546) | |

**Explicitly not made**: the visibility ruling. "That ruling belongs to the owner of the threads API and has not been made; this round does not make it"
(`docs/threads/SPEC-jit-history.md:1444-1446`); DP:690 calls it "the ruling that was never made". What is shipped is the "interim default" narrowed by history
§50: "a read that can decide a loop's exit is performed again every iteration" (PP:1352).

One decision taken by the agent **without** asking, recorded as such: outlining the threaded arms at ~1 % cost to the threaded modes, "decided without asking,
against the letter of the round's goal, on the ground that the goal's order puts flag off first" (LP:4537-4538).

#### D: B1.3 What may never be traded (stated as rules of the plans; authored by the sessions, accepted with the plans)

- Memory safety (S1): no program "racy or not ... makes a GIL-off process read or write memory outside the object it names, use freed memory, or treat a value of one
  kind as another" (PP:813-816).
- S5: "the performance plan may relax *when* a write is seen, never *what* is seen" (PP:932-933).
- "defects before speed" (DP:196); "No performance package is counted toward a gate until Part 6's campaign has run on the tree that contains it" (PP:1326).
- Two copies of the object model in C++ are refused: "two live copies of the object model in C++ is the failure mode decision 9 rejected" (PP:1314-1315).
- After the merge: "**a change to the threads work may not add an ungated hunk without a ledger row**" (FO:1427-1428).
- GIL off stays behind a second option named unsafe; `gilRemovalPreconditionsMetValue` is `constexpr false` until one commit cites the evidence (PP:957-963).
- The flag is "a high-resolution-timer capability grant; embedders must not enable it for semi-trusted or multi-tenant code in-process" (FO:148-150).
- Spec first, then C++, then every tier, with a test that fails before (DP:199-201, PP:682-683).

#### D: B1.4 The nineteen + three + ten decisions *asked* of the owner (recommendation; what PARITY-PLAN assumed)

DP:271-423; assumed values PP:1350-1369. No answer to 1-22 was found in the repository.

| # | question | recommendation / assumption |
|---|---|---|
| 1 | visibility rule: (c0) keep, (c1) bounded staleness with a 1 ms tick, (c2) full release | DP: (c0) now, (c1) as ruling. **PP changed to "(c0) only"**: worth 0.8 % of time |
| 2 | `Atomics` on ordinary objects | (B) own option following the flag |
| 3 | racy plain accesses to different locations | (A) portable model: unordered, as Java non-volatile; not TSO everywhere |
| 4 | x86-64-only rules | keep, each fallback exercised on x86-64 |
| 5 | arm64 GIL off a landing requirement? | no: refused at option validation until run on hardware |
| 6 | Double family | DP: (b) C-D1+C-D2 now, (c) C-D3 as its own project, not (d). **PP: C-D3 in the base plan** |
| 7 | vectorLength growth | (C) keep the copy, remove the re-dispatch (D-AG-1) |
| 8 | WebAssembly | carrier-only stage 1; the two defects first; uniform refusal on spawned threads |
| 9 | speed before the first spawn a goal? | DP: (A) no. **PP: new option (C') U-1, optimized code only** |
| 10, 11 | FFI for the carrier; sampling; second-VM message; module evaluation lock | as recommended |
| 12 | precondition 10 | (E)+(F) |
| 13 | collector | service conductor in full with the lone-conductor rule; RSS gate in the scaling suite |
| 14, 18 | RegExp/strings and flag off per-mode instantiation | F-D4 (done in another form in round 13, DP:3134) |
| 15 | inlined callee entries stop polling | accept |
| 16, 17, 19 | LLInt caches; micro set; tier-up under N threads | D-A1 after the loop-entry fix; add mono-proto rows; K D2-D3 first |
| 20-22 | both gates must pass; report all three numbers on a miss; first carrier's TID is 0 for good | PP:1373-1379 |
| F1-F10 | FO:1351-1415 | answers in B1.2 |

---

### D: B2. What the eleventh design session (2026-09-17) concluded, and what it rejected

Three parts, no code changed (LP:3942-4087).

**Part 1, DESIGN-PROPOSALS** (reading + cheap measurements on existing binaries). Conclusions one line each: LP:3955-3981. The ones that matter here: delta-blue's doubled
FTL instructions are inlined-entry polls + `PutStack` writing the heap, "the visibility rule as such is about 40 % of the gap"; sha256's bimodality is an exit-feedback
defect; aes' growth cost is "re-dispatch around the copy, not the copy"; flag off is "one shape, Config-byte gates whose inline cold arms bloat leaves"; scaling failures are
tier-up recovery. Expected from groups 0+1: GIL off / GIL on 0.804 -> 0.86-0.87, "of which about four points exist only before the first spawn" (DP:229-232).

**Part 2, PARITY-PLAN** (experiment build allowed). "**The honest answer first.** No set of changes known today guarantees 0.90" (PP:20). Measured 0.758 -> 0.804; all
ceilings 0.832; central estimate "**0.87 with threads alive and 0.90 for a process that never spawns**, in a range of 0.835 to 0.925" (PP:22-23). Time is main-thread cycles; polls
and tags 3 points; Double family 2.9 and "C-D3 is no longer optional"; eleven points in no switch; five points paid by both flag-on modes (PP:27-38). Two gates G-U / G-T plus
G-S (PP:81-85). Safety = six properties + one constant (PP:99-105). Rules for a miss fixed in advance, the last being "**Accept and state**" the measured cost (PP:1305-1308).
New designs R-1..R-4 (no claim on an unescaped object; owner-first cell locks; unshared strings' refcounts; lock-free structure tables) and T-1..T-4, U-1.

**Part 3**: the owner's reordering (B1.1 #1) and FLAG-OFF-LANDING: 12 criteria, 10 packages, 10 decisions.

**Rejected, demoted or not recommended:**

| option | verdict | reason | cite |
|---|---|---|---|
| L-D2 scope A/B: untagged and unsynchronized until the first spawn, C++ included | not recommended; PP refuses to reopen | "about 980 gates to classify, two live copies of the object model GIL off"; "A process-wide mode whose failure mode is memory unsafety"; C++ frames that span the switch; "a JetStream number that stops measuring what threaded code costs" | DP:354-360, DP:6539-6550, DP:6580-6582, PP:1312-1315 |
| start GIL on and convert the VM at the first spawn | rejected | "`vm.m_gilOff` is immutable by design ... the heap would have to change from one client to server/clients under a live program" | DP:6584-6586 |
| D-AG-2 in-place vectorLength raise by the owner | "proposed for rejection" | "at most three lanes per butterfly, reopens the converter race the withdrawn T5 had" | DP:339-341, DP:2135 |
| D-AG-3 segmented growth for owned arrays | "proposed for rejection" | "O(1) growth, 13 times slower reads today" | DP:341, DP:2173 |
| C-D3 variant (d): header re-check of history §29 ("storage-then-shape re-check") | not (d) | "same gains, dearer readers, an arm64 barrier"; the raw-double bit in the word is preferred | DP:334-336 |
| (c1) D-B5 bounded staleness, (c2) D-B6 full release | dropped from the base plan | 0.8 % of time; "D-B5's phase, per-thread trap delivery and ticker are not worth that"; (c2) makes a plain-flag spin work in lower tiers and hang in the FTL | PP:1352, DP:283-288 |
| T-1 entry poll as the stack check | designed, NOT in the base plan | "Measured: nothing in time (... 1.251 -> 1.252)" | PP:471-473 |
| T-3 counted loops poll once per strip | contingency | ceiling 0.9 %; "a new CFG transformation" | PP:571-573 |
| removing polls from counted loops outright | rejected | "a 10^9-iteration loop would hold a collection for seconds" | PP:616-617 |
| poll as one load from a protected page | rejected, kept as contingency | "needs a fault handler that re-enters the engine at arbitrary prologue states and competes with the embedder's signal handlers" | PP:532-534 |
| TSO everywhere on arm64 | not recommended | "probably double-digit percent there" | DP:309-310 |
| thread-local prescaler for tier-up counters (6th round) | not landed | "cost single-threaded loops 2x on this hardware" | PR:280-283 |
| per-thread Eden collection | known next design, out of the plan's scope | | PP:1319-1322 |
| service conductor for every cycle (10th-round experiment) | lost on four tests | "two real rendezvous per short cycle"; D-J1 uses it only for cycles predicted >= 5 ms | PR:1456-1457, LP:5103-5106 |
| "real TLAB" per-thread fresh-block cache (June) | wrong lever | SH:165-171 | |
| per-(worker x shard) owner-local chunks in the benchmark | "measured and REJECTED" | FG:553-559 | |
| dictionary flattening GIL off, first form (7th) | built and withdrawn | "the amplifier tied crashes to it"; redone as a transition in the 8th | PR:888-891, PR:913 |

---

### D: B3. Exit criteria for landing

**Original (LP:5334-5346), never rewritten:** `JSTests/threads` and the CVE suite pass in Debug and Release, GIL on and off, "on Linux x86-64 and on one arm64 platform"; JSC suites same as
`main` flag off and no new failures flag on; TSAN reports nothing unexplained; Bun's suite same as with `main`; "No benchmark in 2.2 is slower than `main` with the flag off, beyond
noise"; "Every flag-off difference in generated code has a written reason."

**In force since 2026-09-17: three gates (PP:1227-1239).**
- *Gate A, merge with the flag off by default*: flag-off suites incl. test262 and one Debug pass, the identity compare, Bun flag off, builds on every platform the embedder ships. "Nothing
  else in this document gates it."
- *Gate B, GIL on supported*: W4/W5 fixed, extension list published, GIL-on lanes.
- *Gate C, GIL off supported*: everything; no open memory-safety row; "48 consecutive amplifier hours; 30 CPU-days of GIL-off fuzzing with zero new signatures; the conformance set per
  tier on x86-64 and arm64" (PP:1274-1277); the constant flips in one commit.

**Gate A's definition of done, D1-D12 (FO:1102-1123).** The claim: "a process that does not set `useJSThreads` cannot tell this engine from `main`: not by a result, not by a crash, not by a
measurable amount of time or memory; and the code that such a process executes has been accounted for line by line." D1 results equal on every suite; D2 generated code equals `main`'s
(golden compare); D3 ledger of every ungated hunk; D4 no added fail-stop reachable; D5 sanitizers; D6 fuzzing (subject to F9); D7 speed (L5's table); D8 start-up and memory; D9 size
stated and bounded; D10 builds and runs flag off on Linux x64/arm64 glibc+musl, macOS x64/arm64, Windows x64; D11 Bun builds, full suite, Release benchmarks; D12 the flag's exposure.
"Deliberately not in the definition": flag-on correctness or speed, arm64/non-Linux support of the flag, wasm or FFI on spawned threads (FO:1125-1127).

**State at the seventeenth round (LP:5037-5049, LP:5008-5027):** safety identical to `main` with every golden-compare difference explained; all speed metrics at target (first iteration only in
the mean); **start-up RSS +0.87 MB vs +0.5 MB is "the one metric outside its target"**; not done: arm64, macOS, Windows, musl "are not built; Bun with the branch is not built (clang 23.1)"; ledger
2,127 rows, 1,227 without a verdict at round 15 (FO:39); eight open GIL-off defect rows. Flag-on stress failures: GIL on 61, GIL off 255 of ~89,780 (LP:4862).

**No-JIT facts.** The C loop (`ENABLE_C_LOOP`, no JIT) "builds and runs" flag off and GIL on (LP:4880); GIL off "is further forced back to GIL on (with a log line) on every non-Linux, CLoop
or 32-bit build" (FO:1042-1043). So **GIL off has never run in a no-JIT build**, only with `--useJIT=0` on a JIT build.

---

### D: C. Not found / not sure

- **No decomposition of 1/0.76 as a product of named factors** exists. The nearest are A2.1 (instructions, vs GIL on) and A2.2/A2.3 (cycles, vs `main`; cumulative ablation, so each step depends on order).
- **Row R (eleven points) was never prototyped**; its families are ranked by profile difference only. "Nobody has a census of them by node kind" (PP:268). P0.7 was not carried out as far as I read.
- **None of the eleventh session's GIL-off designs was implemented** afterwards, except F-D4/F-D5 for flag off (DP:3134, DP:3212), N-D1 compile fixes (DP:7605) and the two ride-along fixes (not verified by me in
  LP's rounds 12-17). I did not read rounds 12-16 in full.
- **No GIL-off measurement without a JIT after the 7th round.** A2.5(a) is an old tree, 15 worst tests, runs "made eight at a time" (PR:636). My 0.89 geomean is my arithmetic.
- **Per-tier time** (cycles) GIL off is not tabulated anywhere I read; per-tier tables are scores (old) or instructions.
- **No multi-thread JetStream-like score**; "with threads" means one parked thread (G-T). Scaling tables were last taken in the 10th round; SCALEBENCH's last full matrix is June.
- **Owner's answers to decisions 1-22**: none found in the repository.
  personally is not stated; the documents say "by ruling", "LAW", "standing decision" without naming who ruled.
- **Watchpoints in a no-JIT design** (A3 row): I did not read SPEC-jit/SPEC-objectmodel on Class-A fires, F1/F2 or `haveABadTime`; the row is reasoning only.
- **Atom table and structure-ID table as scaling limits**: atom-table contention is shown once (7th round, string-heavy, PR:851-857); ablation in June found `useSharedAtomStringTable` neutral on map-heavy T(1)
  (SB:679-681). No measurement of a StructureID-allocation lock was found; TH:19 says StructureID needs no table.
- **arm64 costs are all by reading**: "unknown until measured on hardware" (DP:90, DP:7544).
- **Bun-level numbers**: "The embedder's binary, start-up, memory and benchmarks: never measured" (FO:1093-1094); still not built at round 17.
- Unexplained in the documents themselves: navier-stokes GIL on (0.87 of the instructions in 1.28 of the cycles, PP:336-338), earley-boyer +5 % GIL on, F30, typescript's +13 to +40 MB (LP:4948-4949).
- Not read: DP sections D-K and M designs in full, SB §25-§33 and §46-§47, FG rounds 1-4 bodies, FO 2.2 "Where the score goes" body and 2.3-2.5, LP rounds 1-9.

---

## Appendix E. Testing assets

(Original title: r9/test: testing assets of the old JS-threads implementation, and what can be reused)

Paths are relative to `<threads-branch>/` (branch `sosuke/threads`, HEAD `2b14c39e3638`) unless they start with `bun:`
. Counts marked *measured* come from `grep -l`/`find` run for this report on that checkout.
Counts marked *estimate* are my judgement. Nothing was built or run.

Two terms of the old implementation appear everywhere:
- **GIL on**: `--useJSThreads=1` alone. One JS thread runs at a time; threads yield only at blocking calls (`Tools/threads/run-tests.sh:61-66`).
- **GIL off**: real parallelism. `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0` in the environment (`docs/threads/LANDING-PLAN.md:47-48`).

### E: 0. Summary of what is reusable

| Asset | Tied to old engine? | Verdict for the new implementation |
|---|---|---|
| `JSTests/threads/{api,atomics,races,sync,lifecycle,invariants,semantics,shared-objects,arrays}` (151 tests) | mostly not | the core reusable corpus; black-box, self-checking |
| `JSTests/threads/cve` (65 tests) + `docs/threads/CVE-AUDIT.md` taxonomy + `docs/threads/cve/*.md` | tests: partly; taxonomy and CVE catalogs: no | taxonomy reusable as is; per-class surface maps must be redone |
| `JSTests/threads/{objectmodel,vmstate}` (120 tests) | headers name internals, but about half the bodies are plain JS races | reuse as black-box races; drop the counter/`$vm` ones |
| `JSTests/threads/jit`, top-level `heap-*`, `congc-*`, `giloff-*`, `bench/` | yes | mostly not reusable |
| `harness.js` + `resources/assert.js` | only `load()`; `sleepMs` comment is GIL-specific | reusable |
| `run-tests.sh` | probes jsc options and `$vm.useThreadGIL()` | policy reusable (see 3.3), script needs a port |
| `amplify.sh` (driver) | only 3 option names | reusable; `RaceAmplifier.{h,cpp}` is 250 lines and engine-neutral; the 24 call sites are not |
| TSan method (rules, dedup tools) | suppression list is JSC symbols | rules and tools reusable; with no JIT the main blind spot of the old campaign goes away |
| Mirror harness | needs `eval`/`new Function` + `readFile` at run time | idea reusable; mechanism conflicts with ahead-of-time compilation |
| Fuzzilli profile | generators are API level; flag rotation is JSC | generators reusable |
| `scaling/` + `scaling-gate.sh`, `scalebench/` (JS, Go, Java) | only `preciseTime`, `print`, `-e` injection | reusable |
| `lint-*` | 8 of 9 are flag-off landing or JSC-layout lints | only the lock-order lint idea carries over |

### E: 1. Corpus inventory (`JSTests/threads/`)

500 files *measured*: 491 `.js` and 9 others. Of the `.js`, 476 are tests; 15 are libraries or fixtures (`harness.js`, `bench/harness.js`,
`scaling/harness.js`, `resources/{assert,segmented-out-of-bounds-read}.js`, `vmstate/resources/workload.js`, 9 module fixtures in
`api/resources/ns-identity/`, eight of them 2,001 generated lines each). About 41,900 lines of test code.
Non-JS: `jit/{README.md,run-jit-tests.sh,lint.sh,golden-disasm.sh,bench-gates.sh}`, `vmstate/README.md`,
`cve/mc-jit-delete-reuse-stale-offset.DIAGNOSIS.md`, `cve/mc-gc-weakgcmap-registry-vs-prune.stw-variant.txt`, `lifecycle/restrict-foreign-access.js.skip`.

`JSTests/threads.yaml` exists and registers only six directories with upstream's runner (`threads/api`, `atomics`, `races`, `jit`, `vmstate`,
`objectmodel`, each `cmd: runDefault unless parseRunCommands`; `JSTests/threads.yaml:30-48`). The real runner is `Tools/threads/run-tests.sh`.

Conventions, frozen in `docs/threads/SPEC-api-annex.md:18` (§T2): "self-checking,failure=throw;join/await every spawned thr (4.6.3);no preemptive-GIL
reliance (5.2);race tests bound blocking ops;headers list API-I<n>;CI greps API-I1..I24". The 24 API invariants are `docs/threads/SPEC-api.md:293-320`;
the file-to-invariant map is `docs/threads/SPEC-api-annex.md:10-14`. These 24 lines are the most compact implementation-independent statement of what the API must do.

#### E: 1.1 Dependency counts, whole corpus (*measured*, 481 files = top level + one directory level)

| Pattern | Files |
|---|---|
| `load(...harness.js / assert.js)` | 407 (any `load(`: 417) |
| `//@ requireOptions` | 456 (`--useJSThreads` named in 445) |
| `//@ runDefault` | 12 |
| `//@ threadsRequireGILOff` / `threadsRequireGILOn` / `threadsExpectFail` | 46 / 3 / 0 |
| `//@ threadsEnv` / `threadsForbidOutput` / `threadsNoAmplify` / `skip` | 2 / 1 / 3 / 7 |
| `THREADS-PREMISE-SKIP` self-report | 6 |
| `$vm` | 114 (`--useDollarVM` in 126 headers) |
| `noInline(` | 115; `numberOfDFGCompiles` 11; `optimizeNextInvocation` 0 |
| `print(` | 113 |
| `gc()`/`fullGC()`/`edenGC()` shell globals | 45 (plus `$vm.gc` 18, `$vm.edenGC` 3) |
| `asyncTestStart`/`asyncTestPassed` | 22 |
| `preciseTime`/`performance.now` | 22 |
| `drainMicrotasks` 7; `setTimeout` 2; `quit(` 11; `WebAssembly` 11; `$.agent`/`$262` 4; `createGlobalObject`/`runString` family 14 | |
| `eval(` / `new Function(` | 32 (matters for an ahead-of-time compiler) |
| comments or code mention "segmented" or "butterfly" | 120 |
| file name contains `gil-off`/`giloff` 61; `gil-on` 8; `refused` 5 | |
| API use: `new Lock` 44, `new Condition` 16, `new ThreadLocal` 5, `Thread.restrict` 11, `asyncJoin` 17, Atomics on a named property (heuristic) 196, no `Thread` at all 20 | |

`$vm` methods by number of files: `useThreadGIL` 23, `jsThreadsCounter` 20, `sharedHeapTest` 19, `gc` 18, `indexingMode` 12, `toCacheableDictionary` 7,
`flattenDictionaryObject` 6, `assertEnabled` 6, `toUncacheableDictionary` 5, `createGlobalObject` 5, `ensureArrayStorage` 4, `useJIT`/`haveABadTime`/`edenGC`/`deleteAllCodeWhenIdle` 3 each,
then 35 more in 1-2 files (`butterflyOwnerTID`, `currentButterflyTID`, `structureThreadLocalSetsValid`, `jsThreadsStopCount`, `requestTermination`, `callWithTimeLimit`, ...).

Options in headers beyond the mode flags: `useSharedGCHeap` 60, `useVMLite` 42, `useThreadGILOffUnsafe` 41, `useSharedAtomStringTable` 41, JIT thresholds 23+22+8+6+4+3,
`useThreadGIL` 20, `countJSThreadsCounters` 20, `verifyConcurrentButterfly` 15, `useFTLJIT` 9, `watchdog` 8, `useConcurrentSharedGCMarking` 8, `useConcurrentJIT` 6, `numberOfGCMarkers` 6, `useJIT` 5,
`collectContinuously` 4, `validateExceptionChecks` 3, `forceButterflySWBit` 3, `forceSegmentedButterflies` 2, `destroy-vm` 2, `can-block-is-false` 2, about 40 more once each.

#### E: 1.2 Mechanical classification (*measured*, 476 tests)

- **A**: nothing but `load`, standard JS, the threads API, and mode flags in the header.
- **B**: also shell helpers with an obvious replacement (`print`, `noInline`, `gc`, `asyncTestStart`, `preciseTime`, `drainMicrotasks`, `quit`, `setTimeout`, `$vm.{useThreadGIL,gc,edenGC,assertEnabled,createGlobalObject}`, `WebAssembly`) or JIT threshold/tier options (a no-op without a JIT).
- **C**: any other `$vm.*`, a tier query (`numberOfDFGCompiles`, `$vm.useJIT`...), or any other option.

| Directory | Tests | A | B | C | Lines |
|---|---|---|---|---|---|
| (top level) | 36 | 3 | 10 | 23 | 2,862 |
| api | 38 | 5 | 15 | 18 | 2,637 |
| arrays | 14 | 7 | 3 | 4 | 1,118 |
| atomics | 19 | 13 | 3 | 3 | 1,698 |
| bench | 8 | 0 | 8 | 0 | 350 |
| cve | 65 | 23 | 35 | 7 | 7,122 |
| gc-stress | 14 | 2 | 6 | 6 | 1,351 |
| invariants | 7 | 7 | 0 | 0 | 618 |
| jit | 75 | 4 | 35 | 36 | 5,499 |
| lifecycle | 10 | 7 | 2 | 1 | 721 |
| objectmodel | 81 | 33 | 13 | 35 | 6,921 |
| races | 7 | 6 | 1 | 0 | 501 |
| scaling | 7 | 5 | 2 | 0 | 1,363 |
| semantics | 26 | 15 | 10 | 1 | 3,618 |
| shared-objects | 17 | 11 | 2 | 4 | 1,979 |
| sync | 13 | 11 | 2 | 0 | 1,306 |
| vmstate | 39 | 23 | 6 | 10 | 2,192 |
| **Total** | **476** | **175** | **153** | **148** | |

Inside B (a file can count several times): `noInline` 66, `print` 49 (14 use only `print`), `gc` 36, `asyncTest*` 19, tier options 19, wasm 10, `preciseTime` 9, `drainMicrotasks` 7, GIL probe 7, `quit` 6.
Inside C: internal `$vm` hook 94, non-mode option 84, tier query 13. 46 files are C **only** because of an option. Of those 46:
about 20 name only a checker or forcing option (`verifyConcurrentButterfly`, `forceSegmentedButterflies`, `forceButterflySWBit`, `verifyGC`, `verifyHeap`, `validateExceptionChecks`), and the body is an ordinary race, e.g. `objectmodel/json-stringify-array-race.js`, `objectmodel/string-raw-race.js`, `arrays/segmented-out-of-bounds-read.js`;
7 need a termination source (`--watchdog --watchdog-exception-ok`); 2 need `--can-block-is-false`; 2 need `--destroy-vm`; the rest are GC or JIT tuning.

**Estimate of reuse** (mine; not verified by running anything):

| Class | Tests | What it takes |
|---|---|---|
| Reusable as is | about 175 (37 %) | replace `load` (link or concatenate at build time), map `--useJSThreads=1` |
| Reusable after shims | about 180 (38 %): B (153) + about 20 "drop the checker option" + about 10 termination / may-block policy | a shim file defining `print`, `noInline = () => {}`, `gc`, `asyncTestStart/Passed`, `preciseTime`, `drainMicrotasks`, `quit`; a termination hook. About 43 of these (`jit/` B, `bench/`) run but lose their purpose without a JIT |
| Implementation specific | about 120 (25 %) | counters, `$vm.sharedHeapTest`, TID/butterfly probes, tier kill switches, GC tuning |

Caveats on "as is":
1. Some tests assert **policies of the old implementation**, not the API: wasm refused on spawned threads (`api/wasm-refused-sd7.js`: "NORMATIVE in BOTH GIL modes"), `import()`/ShadowRealm refused (`api/import-and-shadowrealm-refused-gil-off.js`), typed-array `Atomics.wait` throwing on a spawned thread GIL on (`atomics/ta-wait-thread-gate.js`, API-I21 "deleted by re-freeze"), thread id range (`api/thread-id-bounds.js`: header says `[1, 0x3fff]`, `SPEC-api.md` I17 says `[1,0x7ffe]`).
2. `api/thread-ctor-errors.js` and `atomics/property-errors.js` check "exact messages".
3. 46 tests spin on shared state and need real parallelism (`threadsRequireGILOff`); fine for a parallel engine, but they need loops to observe other threads' plain writes.
4. Many early tests are written around the cooperative GIL ("Stub-scheduling note (phase-1 GIL)", `sync/atomics-futex-lock.js:6`), so they are weaker than they could be: bounded 5 ms waits, main thread only spawns and joins.
5. Stale header: `api/thread-restrict.js:4` says "SKIPPED until the 9.2-6 choke-point hook", but the file has no `//@ skip` line now.

#### E: 1.3 Per directory

**api (38)**: API surface, error cases, async variants, termination, embedder policy. Mixed: the 12 original spec files are API level, the later ones pin engine fixes.
- `thread-basic.js:2` "API-I2: for any value v (objects, NaN, -0), new Thread(() => v).join() is SameValue-equal to v" (uses `asyncTestStart`)
- `lock-basic.js` "API-I6 (small-N, main thread contending): N x M lock.hold(() => counter++) ... yields exactly N*M increments"
- `thread-restrict.js:2` "API-I14: Thread.restrict + ConcurrentAccessError"; `concurrent-access-error-shape.js` "an Error subclass in the NativeError shape"
- engine specific: `dollarvm-shared-heap-test-fuzzer-gated.js`, `options-environment-order-gil-off.js`, `lazy-decoding-options-gil-off.js`, `gil-on-*` (3)

**atomics (19)**: Atomics on `(object, propertyName)` and proof that the typed-array path is unchanged. 13 A.
- `property-cas-samevaluezero.js` "compares with SameValueZero — NaN matches NaN and +0 matches -0"
- `property-wait-notify.js` "API-I10: ... there is no lost store+notify window"
- `property-wtr-isolation.js` "API-I11: waiters on (o,"k") are unaffected by notify on a typed array, on (o,"j"), or on another object's "k""
- the `*-u5*`, `*-u28-flat` storms target lock-free "arms" of the old store path, but their oracle is an exact count

**races (7)**: the "GI" set ("must hold post-GIL"). 6 A, 1 B. `counter-lock.js:2` "N=8 threads x M=1e5 lock.hold(() => counter.n++)"; `counter-atomics.js`; `transition-vs-read.js` / `transition-vs-write.js` (API-I16); `wait-notify-storm.js`; `join-storm.js`; `forin-enumerator-cache.js`.

**sync (13)**: Lock, Condition, ThreadLocal, futex built from property Atomics. 11 A. `atomics-futex-lock.js:2` "Build a working mutex out of Atomics.compareExchange / wait / notify on a plain object property"; `condition-notify-all-shared-lock.js` "N waiters share ONE Lock and a single notifyAll releases them all"; `thread-local-isolation.js`. Two are `//@ skip` with FIXME for the GIL stub (`condition-notify-all-multi-waiter.js`, `condition-worker-waiter.js`): worth enabling on a parallel engine.

**lifecycle (10 + 1 `.skip`)**: constructor shape, `Thread.current`/`id`, join repeatability, nested spawns, exception identity, return value fidelity, `restrict`. 7 A. Engine specific: `exit-flush-multi-segment-remembered-set.js`, `vm-destroy-collect-continuously.js`.

**invariants (7)**: all A. The object-model guarantees as observable properties: `no-lost-properties.js` "N threads concurrently add DISJOINT sets of named properties to one shared" object; `no-torn-shapes.js:5` "o.f = 1 racing o.g = 2 must never produce o.f == 2"; `no-time-travel.js`; `no-lost-elements.js`; `delete-quarantine*.js` (named after a mechanism, asserted through values).

**semantics (26)**: language features under threads. 15 A, 10 B. `proto-cycle-race.js` "two threads race Object.setPrototypeOf in opposite directions"; `frozen-seal-race.js`; `symbol-registry-cross-thread.js`; `stack-overflow-per-thread.js`; `private-fields-shared.js`; `regexp-lastindex-shared.js`; `async-generator-multithread-*.js`; `global-declaration-race.js`; first-use races (`lazy-prototype-first-use-race.js`, `error-stack-first-access-race.js`, `intl-lazy-fields-race.js`). Seven `ic-<op>-vs-transition.js` drive an access site "uninit -> mono -> poly -> megamorphic" against a transitioning thread; without ICs they are plain access-versus-transition races. Skipped: `oom-one-thread.js`, `termination-storm.js`.

**shared-objects (17)**: property add/delete/read-write, dictionary mode, accessors, prototype chains, frozen/sealed, shared `Map`/`Set`/`WeakMap`/`WeakSet` writers, spread/`Object.values`/`Object.assign` against a deleting thread. 11 A. `map-set-shared-writers.js` "Several threads write one shared Map and one shared Set at once".

**arrays (14)**: element read/write over int32/double/contiguous, holes, copy-on-write literals, push/pop/length, typed arrays + SAB, detach races. 7 A. `push-resize-multithread.js` "Lock-guarded sections must stay exact in any implementation".

**objectmodel (81)**: the largest set. Headers cite SPEC-objectmodel invariants and mechanisms (segmented butterflies, TID tags, SW bit, ArrayStorage copy, quarantine). 33 A + 13 B have plain JS bodies with value oracles: `i03-n2-inline-add-races.js`, `i03-i37-same-shape-add-storm.js`, `define-property-kind-change-vs-readers.js`, `seal-freeze-vs-delete-add-race.js`, `property-table-rehash-vs-lookup.js`, `concat-multiple-arrays-race.js`, `join-two-pass-vs-foreign-writer.js`, `json-stringify-*`. C examples: `i03-selftest.js` ("C++ self-test driver"), `double-allocation-profile-*`, `typed-owner-relabel-no-stop.js` (counts stops), `n1i-instance-keyed-ownership.js`. `i03-single-threaded-no-change.js` / `i03-single-threaded-flag-on.js` are a flag-off/flag-on twin of single-threaded semantics pins.

**vmstate (39)**: per-VM caches and state that had to become per-thread or locked. 23 A. This is effectively a checklist of hidden shared runtime state: `date-cache-per-thread.js`, `json-parse-key-cache-per-thread.js`, `split-cache-per-thread.js`, `bigint-divisor-cache-per-thread.js`, `intl-*-cache-per-thread.js`, `string-recursion-check-per-thread.js`, `exception-state-per-thread.js`, `stack-limits-per-thread.js`, `eval-callee-per-thread.js`, `microtask-ordering.js`, `regexp-churn-threads.js`. Three identity files assert one hard-coded digest of `resources/workload.js` under different flags (`vmstate/README.md`: "All three identity files assert the SAME hard-coded digest"). C: sampling profiler (4), `dump-registers-*`, `heap-epoch-reclaim-second-vm.js`.

**jit (75)**: ICs, call linking, OSR, watchpoint fires, tier kill switches, tag discipline, handler ICs in FTL. 52 use `noInline`-family helpers, 29 `$vm`, 72 `requireOptions`. `jit/README.md` lists the original Task-13 suite and its own driver `run-jit-tests.sh`. Implementation specific almost by definition. A few have an engine-neutral race in the body: `length-update-races-gil-off.js`, `create-this-profile-torn-pair.js`, `instanceof-nuked-structure-id.js`, `known-atom-bit-vs-rope-resolve-gil-off.js`, `shared-arraystorage-stress.js`, `spawned-thread-butterfly-stress.js`.

**gc-stress (14)**: `conservative-scan-register.js` "the conservative scanner must see references that live ONLY in a parked spawned thread's machine" registers; `zombie-uaf-canary.js`; `destructors-run-once-on-any-thread.js`; heap snapshots from several threads; reclamation with a parked thread. 2 A, 6 B, 6 C. Ideas carry over to any collector with N mutators; the assertions often use `$vm` counters.

**cve (65)**: see section 8. 23 A, 35 B, 7 C; all load the harness; 17 `threadsRequireGILOff`.

**scaling (7 + harness)** and **bench (8 + harness)**: section 10.

**top level (36 + harness.js)**: `heap-*.js` (15; most drive the C++ harness `$vm.sharedHeapTest`, e.g. `heap-allocation-storm.js:1` needs only `--useSharedGCHeap=1 --useDollarVM=1` and spawns no JS `Thread`), `congc-t*.js` (8, concurrent marking charters), `giloff-*.js` (6), `checktraps-*.js` (2), `dw1-*`/`dw2-*` (4, regressions from the "deepwater" hunt), `w16-c1-prevent-collection.js`, `smoke.js`. Reusable: `smoke.js`, `heap-client-exit-closure-reads.js`, `giloff-time-limit-terminates-one-thread.js` (as a semantic: a time limit terminates one thread).

### E: 2. Harness

#### E: 2.1 `JSTests/threads/harness.js` (62 lines; loads `resources/assert.js`)

| Helper | Meaning |
|---|---|
| `sleepMs(ms)` (`:36`) | sleeps with `Atomics.wait(lane, "v", 0, ms)` on a private plain object; without `Thread`, on a private `Int32Array(SharedArrayBuffer(4))`. The property path was chosen because it "parks with the GIL DROPPED" (`:13`) |
| `waitUntil(cond, maxMs = 30000, stepMs = 5)` (`:53`) | polls `cond()` with `sleepMs(stepMs)`; throws `"waitUntil: condition not reached within ..."` after `maxMs` |

#### E: 2.2 `JSTests/threads/resources/assert.js` (110 lines)

| Helper | Meaning |
|---|---|
| `describe(value)` (`:7`) | printable form; quotes strings, distinguishes `NaN` and `-0` |
| `shouldBe(actual, expected, message?)` (`:21`) | SameValue-like: `NaN` equals `NaN`, `+0` differs from `-0`, otherwise `===` |
| `shouldBeTrue`, `shouldBeFalse(actual, message?)` (`:33`, `:37`) | |
| `shouldNotThrow(func, message?)` (`:41`) | returns `func()` |
| `shouldThrow(fn)`, `(Type, fn)`, `(fn, "msg")`, `(Type, fn, "msg")` (`:56`) | type matches by `instanceof`, `constructor.name` or `error.name`; string matches `String(error)` or `error.message`; returns the error |
| `spawnN(n, fn)` (`:91`) | `new Thread(fn, i)` for `i` in `0..n-1`; returns the array |
| `joinAll(threads)` (`:98`) | `threads.map(t => t.join())` |
| `withTimeout(ms, fn)` (`:104`) | runs `fn`, throws afterwards if it took longer: "a coarse watchdog, not a scheduler" |

Only dependency on the shell: `load(path, "caller relative")`. Others: `bench/harness.js` `reportBench(name, fn, expected, warmup = 20, measured = 50)` prints `BENCH <name> <ms>`;
`scaling/harness.js` `runScalingWorkload(name, workFn)`, `scalingThreadCount()`, `scalingWorkScale()`, `runSelfTripwire(...)` prints `SCALING <name> <threads> <ms>`.

#### E: 2.3 Directives (`Tools/threads/run-tests.sh:19-69`; only the leading block of `//@` lines is read, `:360`)

| Directive | Effect |
|---|---|
| `//@ skip` | file skipped |
| `//@ requireOptions("a", ...)` | appended to every run |
| `//@ runDefault("a", ...)` | one run per directive |
| `//@ threadsExpectFail("gilOff")` | verdict inverted when the effective mode is GIL off: nonzero exit = XFAIL, exit 0 = XPASS = failure. No file uses it now |
| `//@ threadsEnv("NAME=value", ...)` | environment for the run (e.g. `GIGACAGE_ENABLED=0`) |
| `//@ threadsForbidOutput("text", ...)` | fails if stdout+stderr contains the text, even with exit 0 |
| `//@ threadsNoAmplify` | timing-ratio tests run plain under `--amplify` |
| `//@ threadsRequireGILOn` / `threadsRequireGILOff` | skipped in the other effective mode; unknown mode runs normally |
| output line `THREADS-PREMISE-SKIP: ...` | the test reports that ambient options invert its premise: counted SKIP, never PASS (`:731-739`) |
| by name | `blocking-gate.js` gets `--can-block-is-false` appended (`:691`) |

### E: 3. `Tools/threads/run-tests.sh` (840 lines)

#### E: 3.1 Modes
- Usage: `[--filter=SUBSTR] [--amplify] [--list] [--cve]`, or `--gates` alone (`:86-87`). There is **no repeat flag and no GIL flag**.
- **GIL mode is chosen by the caller's environment** (`JSC_*` variables). The runner warns about every ambient `JSC_*` and does not scrub them (`:188-208`: "detect and WARN, never scrub"). "Four modes" in the results means {default, `--cve`} x {GIL on, GIL off}.
- Default corpus: every directory except `bench/`, `cve/`, `resources/` (`:333-346`). `--cve` runs `cve/*.js` **instead** (`:325-328`).
- `--amplify` wraps each run in `amplify.sh`; `AMPLIFY_RUNS=M` is the repeat count (`:617-625`). Missing `amplify.sh`: one warning, plain runs.
- `--gates`: (1) `races/` amplified, (2) `races/` under `WebKitBuild/TSan/bin/jsc` if present with `halt_on_error=1 exitcode=66`, (3) bench self-gate: record then gate on the same build (`:98-113`, `:225-316`).
- Timeout: `THREADS_TEST_TIMEOUT_SECS`, default 120, `timeout -k 10`; three times that with `--collectContinuously=1` (`:609-637`). A timeout is a FAIL.
- Serial; parallelism was obtained outside (several runner processes).
- Exports `ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0` in front of the caller's (`:155`).
- Repetition tools: `load6.sh` (6 workers x 20 runs with rotating seeds), `cve-verify-loop.sh`.

#### E: 3.2 Verdicts
- PASS = exit 0, no premise-skip line, no forbidden output, not XFAIL-marked. FAIL = nonzero exit (including 124/137).
- **Option probe** (`:406-482`): each distinct option set is first run with `-e ''`. SKIP only on jsc's `ERROR: invalid option`; any other nonzero probe exit is a "PROBE CRASH" and fails all runs needing that set.
- **GIL probe** (`:567-603`): asks the binary, `print("THREADS-GIL-MODE:" + ($vm.useThreadGIL() ? "on" : "off"))`, "never a guess from flag spelling".
- **Vacuity guards**: more than 50 % probe skips, or nothing executed, exits nonzero (`:829-838`).
- **Coverage grep**: every `API-I1`..`API-I24` must be cited by a non-skipped file of `api/`, `atomics/`, `races/` (`:772-813`).
- Exit 0 only if no failure, coverage complete, no vacuity.

#### E: 3.3 Policies worth copying
Never turn an engine crash into a skip; ask the binary for its effective mode; a skip-heavy run is a failure; tests declare a broken premise; expected failures fail when they pass; a hang is a failure with a bounded timeout.

#### E: 3.4 Latest recorded result
Seventeenth round (`docs/threads/LANDING-PLAN.md:4852-4853`): "Threads corpus, Release, four modes | 62 / 48 / 403 / 376 passed, 0 failed", same for Debug+ASan. The row does not name the four modes; I read them as CVE GIL off / CVE GIL on / default GIL off / default GIL on (65 CVE tests minus the 17 marked `threadsRequireGILOff` is 48).

### E: 4. Race amplifier

**What it is**: an in-engine hook plus a driver. Not `LD_PRELOAD`, no scheduler control.
- Hook: `Source/JavaScriptCore/runtime/RaceAmplifier.{h,cpp}` (103 + 147 lines). `RaceAmplifier::perturb()` is `if (s_period) [[unlikely]] perturbSlow();` (`RaceAmplifier.h:87-91`).
- `perturbSlow()` (`RaceAmplifier.cpp`): per-thread `WeakRandom` seeded from the global seed mixed (SplitMix64) with a **thread-creation ordinal**, "NOT the OS thread id"; a countdown re-armed uniformly in `[1, 2*period)` because "fixed periods can resonate with loop trip counts and hide races"; on firing, 3/4 `sched_yield()`, 1/4 sleep of 1..`maxSleepUs` microseconds.
- Options (`runtime/OptionsList.h:582-584`): `--randomYieldPeriod=N` (0 = off), `--randomYieldSeed=S` (0 = random, logged), `--randomYieldMaxMicroseconds=U` (100). One banner line `[RaceAmplifier] enabled: period=.. seed=.. maxSleepUs=..`.
- Compiled into release builds; off cost "one load of a global word plus a never-taken predicted branch" (`docs/threads/AMPLIFIER.md:59`).

**Where it perturbs** (*measured*: 24 call sites in 7 files): `heap/Heap.cpp` 6 (heap access acquire/release/detach, TID rebias inside a stop), `runtime/ThreadManager.cpp` 5 (thread exit tail, TID retire), `runtime/VM.cpp` 3 and `runtime/JSLock.cpp` 3 (teardown), `heap/LocalAllocator.cpp` 3 and `heap/CompleteSubspace.cpp` 2 (allocation slow path, block steal), `heap/GCSafepointEpoch.cpp` 2. This matches the list in `RaceAmplifier.h:44-61`.
Discrepancies: `AMPLIFIER.md:130-146` lists *intended* sites in the object model, watchpoints, ICs and atom table, and I found none there. `LANDING-PLAN.md:2118` describes it as "random yields at every poll, park and lock site", which the call sites do not show. So the landed amplifier perturbs allocation, heap access, epoch and teardown paths only; other windows were widened indirectly, by timing and machine load.

**Rules for a site** (`AMPLIFIER.md:148-158`): slow paths only; "*inside* the racy window you want to widen (after the read, before the publish)"; not under a pause-critical lock unless that is the point.

**Driver** `Tools/threads/amplify.sh [--runs 100] [--period 64] [--seed-base S] [--max-sleep-us 100] [--timeout 60] [--jsc-arg A]... [--keep-logs] jsc script.js`. The first non-crashing run is the reference. Each run is CRASH (signal), HANG (timeout), DIVERGENCE (exit status or output differs, banner lines stripped) or, for the reference, REFERENCE FAILED (`amplify.sh:140-162`). It prints the seed and a replay command. It requires deterministic test output (`AMPLIFIER.md:91-94`).

**Results**: fifth round: "ten seeds per test per pass, four modes, four hours, about 341,000 runs, core dumps on): no crash, hang or unexpected exit", and "Two intermediate campaigns ... each found one real defect within their first hour" (`LANDING-PLAN.md:2118-2126`). Typical hit rates: 22 of 960, 35 of 3,000, 1 of about 1,300 runs (`LANDING-PLAN.md:855, 973, 987`).

**Tie to the engine**: the class and the driver are engine-neutral (they need three options, a yield and a sleep). The value is in the call sites, which must be chosen anew. An ahead-of-time compiler could also emit `perturb()` at compiled safepoints or shared-access slow paths in a test build, which the old rule "never JIT-emitted code" ruled out.

### E: 5. ThreadSanitizer

**Two configurations exist in the tree, and the documents disagree about which is current.**
1. `tsan.sh` + `docs/threads/TSAN.md`: **no JIT, C loop**. `-DENABLE_SANITIZERS=thread -DENABLE_C_LOOP=ON -DENABLE_JIT=OFF` (DFG, FTL, WebAssembly and sampling profiler off) `-DUSE_SYSTEM_MALLOC=ON`, RelWithDebInfo, `-fno-omit-frame-pointer -g`, output `WebKitBuild/TSan`. Reason (`TSAN.md:11-16`): JIT code and the assembly interpreter are invisible to TSan, giving false negatives and false positives. Known limit: one shared `CLoopStack` broke parked threads (`TSAN.md:117-133`).
2. The campaigns used **full JIT + assembly LLInt** ("TSanJIT", `WebKitBuild/TSanJIT`): the C loop directory was "wiped and reconfigured", "Per the standing ruling (CLoop is not used in production and is fake work)" (`docs/threads/TSAN-TRIAGE.md:15-19`). Accepted trade-off: "TSAN cannot see races inside JIT-generated code. That coverage belongs to the object-model protocol tests and the race amplifier" (`TSAN-TRIAGE.md:36-38`). A separate C-loop "tsan-deep" scan had 14 GIL-off / 12 GIL-on families, 1,813 / 1,932 reports at r0 (`docs/threads/SCAN-RESULTS.md:42-100`).

**Run**: `Tools/threads/tsan/run-corpus-tsan.sh [outdir]` (`MODE=gilon` for GIL on): every corpus file, 12 in parallel, 420 s each, `TSAN_OPTIONS="suppressions=Tools/tsan/suppressions.txt halt_on_error=0 history_size=7 second_deadlock_stack=1"`. Tools: `tsan/dedup.py` (reports to stack-pair families), `classify.py`, `mkreports.sh`, `family-keys.txt`, `families-r{0..11,23..26}.txt`, `mirror/tsan-dedupe.py`.

**Results** (`docs/threads/TSAN-RESULTS.md:30-50`), unsuppressed reports per round: r0 10,515; r5 763; r6 246; r7 182; r8 74; r10 55; r11 32; r16/r17 0; after the corpus grew to 252 files r23 3,692; r24-r26 66/32/40; r27 0.
- r0: 502 deduplicated stack pairs in **32 families** (`TSAN-TRIAGE.md:66-110`). By ruling: relaxed-atomic (profiling counters, value/array/arith profiles, watchpoint state, JSValue slots), concurrent-accessor / publication (cell header, butterfly word, structure fields, code lifecycle, call links), lock (IC stub info), **real-bug, "NOT blessed"**: `regexp-shared` 137, `vm-string-caches` 96, `gc-incoming-ref` 59, `date-cache` 51, `microtask-queue` 41, `symbol-registry` 17; suppress: `misc-tail` 90.
- Real bugs named: `FunctionExecutable::ensureRareDataSlow` unserialized creation (use-after-free shape) (`TSAN-RESULTS.md:104-109`); r23-r26: 15 families, 13 closed in code, 3 real bugs (`dfg-weakrefs-reallyadd`, `growarrayright-vs-sparsemap-atomics`, `cachedcall-protoframe-crossthread`; `:555-567`).
- Post-rebase rounds (from 2026-09-02): three kinds: upstream races present flag off (suppressed), accesses TSan cannot pair (JIT/LLInt side or address dependency), and "Real ordering bugs, fixed in all builds. A pointer loaded relaxed and then dereferenced"; "on arm64 they can read uninitialized memory" (`:642-662`).
- Latest (seventeenth round, `:1174-1215`): 470 tests; GIL on 0 reports; GIL off 1, open since the thirteenth round: `cve/mc-tear-date-cache.js`, `DateInstance`'s cached fields read against another thread's `setTime`.
- Suppressions: 40 active entries at r27 (6 upstream parallel GC, 23 "atomic probe vs allocator re-hand-out", 1 `recordParse`, 9 "JIT-one-sider allocator hand-out", 1 parked-stack snapshot; `:513-527`). The file has **115 active lines now** (*measured*; 777 lines with justifications).

**Standing rules** (`Tools/tsan/suppressions.txt:6-19`, `TSAN.md:86-95`): only known-benign races that exist upstream; every entry has a justification above it; "NEVER suppress a race in code touched by the threads workstreams"; narrowest matcher. Prefer `SUPPRESS_TSAN` or TSan-only acquire/annotation in source. Learned later: "a stale name fails silently, so check the names after a rebase" (`TSAN-RESULTS.md:647-648`); a suppression by function name "would hide a real race" in that function (`:1207`); TSan defers asynchronous signals, so signal-delivered traps spin (tenth round); a sanitizer build compiles nothing per threads mode, so it does not test those copies.

**For a no-JIT ahead-of-time engine**: if compiled JS goes through a TSan-instrumenting backend, or the runtime does the shared accesses in instrumented C++/Rust, the old blind spot disappears, and so do two of the five suppression classes. JS-level races on plain properties are allowed by the API, so those accesses must be relaxed atomics (or annotated), otherwise every racy test reports.

### E: 6. Mirror harness (`Tools/threads/mirror/`)

**It does not compare anything.** `mirror.js:1-4`: "runs one test file's top-level code concurrently on two JS threads that share the shell's global object. Wrong output and uncaught exceptions are expected and ignored; the harness exists to surface crashes, assertion failures, sanitizer reports and hangs."
- Modes (`mirror.js:8-13`): `eval`: each thread evaluates the source by indirect eval, so `var`s and functions land on the shared global and the threads race on them; `func`: the source is compiled once with `new Function` and called on every thread, sharing code, caches and profiles but not locals.
- A start gate with `Atomics.add/load(gate, "arrived")` makes the threads overlap (`:49-53`). `$vm.crash`/`$vm.abort`/`crash` become exceptions through a Proxy, so only engine aborts count (`:25-39`).
- Driver: `run-mirror.sh <jsc> <outdir> <eval|func> [dir = JSTests/stress] [parallel = 32] [timeout = 300]`; sets the GIL-off environment; `MIRROR_THREADS` (2), `MIRROR_JSC_OPTIONS`, `MIRROR_RSS_LIMIT_MB` (6144). Status: 0 ran, 3 threw, 124 deadline while burning CPU (expected noise), **125 deadline while blocked = deadlock candidate, with a gdb backtrace of all threads**, 126 memory cap; anything else is a finding (`run-mirror.sh:20-28, 76-98`).
- Results (fifth round, `LANDING-PLAN.md:1617-1650, 1852-1876, 2126-2137`): 5,752 stress files. First passes: Release 42 findings (eval) and 32 (func), "most were the shell"; Debug 34 crashes and 26 assertion failures; TSan reports in 448 files, 75 signatures, "14 code changes". "the first passes found eleven engine bugs (F3-F15 and the TSAN fixes), of kinds the written tests had not imagined — first-use races in watchpoint installs, a publication order in `defineProperty`, a parser cache outside its lock". Final eval pass: 3,603 completed, 2,096 threw, 35 spun, 18 others, all artefacts. Later rounds: 5,831 files, 19 findings, "all of the standing" artefact classes (`:3851`).
- Stated limit: "it shares everything, so it says nothing about objects that stay thread-local" (`:2135-2136`).
- **Reuse**: needs `readFile` + `eval`/`new Function`. For an ahead-of-time compiler the same effect needs a build step that wraps each test source in a function and emits an executable that calls it on N threads (the `func` mode); `eval` mode needs top-level declarations compiled as properties of a shared global. The driver's classification (spin against blocked, RSS cap, backtraces) is engine-neutral.
- Related and **not built**: "test262-on-a-Thread", which would diff verdicts between main thread and spawned thread (`docs/threads/CORPUS2-INTEGRATE.md:46-60`). `Tools/threads/test262-on-thread.sh` does not exist.
- Related and done: the ordinary JSC suites with the flag set and no thread spawned ("every difference from the flag-off run is a bug", `LANDING-PLAN.md:189-193`); seventeenth round: `JSTests/stress` GIL on 89,723 passed / 61 failed, GIL off 89,529 / 255 (`:4862`).

### E: 7. Fuzzing (`Tools/threads/fuzz/`, `docs/threads/FUZZ.md`)

- **Real Fuzzilli** with a custom profile `JSCThreadsProfile.swift` (683 lines) + `fuzzilli-profile-registration.patch`; target built by `build-jsc-fuzz.sh` (REPRL + ASan + `-fsanitize-coverage=trace-pc-guard`, FTL on; `FUZZ.md:34-46`); `run-fuzzilli.sh [--smoke]`.
- **Generators** (`FUZZ.md:52-80`): type information for `Thread`, `Lock`, `Condition`, `ThreadLocal`, `ConcurrentAccessError`; 13 code generators, "all join their threads, all cross-thread ops guarded": ThreadSpawn/Join, SharedObjectPropertyStorm, SharedArrayResizeRace, DictionaryFlip, ThreadRestrict, PropertyAtomics, PropertyAtomicsWaitNotify (5-50 ms timeouts), LockContention, ConditionWaitNotify, ThreadLocal, SharedProxyGetter, CrossThreadJITWarmup.
- Rotating flags: tier toggles, threaded-IC kill switches, `forceSegmentedButterflies`, `forceButterflySWBit`, `verifyConcurrentButterfly`, ... (`:82-87`). `timeout=1000ms`, `maxExecsBeforeRespawn=100`. Startup tests assert that the API is exposed and a spawn/join round-trips.
- **Oracles: crash, ASan report, assertion only.** No differential oracle; `LANDING-PLAN.md:2154-2156` lists "a fuzzer that generates two-thread programs (differential against the same program run sequentially)" as a method *not used*.
- "NOT enabled by default: the GIL-off configuration. The profile fuzzes phase-1 semantics" (`FUZZ.md:94-95`). I found no record of a GIL-off campaign.
- **Results**: smoke 2026-06-07: 17 crashes in 10 minutes, about 64 execs/s, 5.58 % edge coverage. r3b (4 h, 4 jobs): 128 crashes, 2 unique signatures (125 were one single-threaded assert in `trySegmentedTransition`). r47 (2 h, 423,374 execs, 11.58 %): 9 crashes, 2 signatures, one root family. r48 (310,666 execs): 2 flaky, neither reproduced. Each closure added tests: `objectmodel/array-storage-property-transition.js`, `cow-named-property-transition.js`, `r47-*.js`, `r48-*.js`. Saved repros: `Tools/threads/fuzz/crashes/{r3,r3b,r47}/`.
- The found bugs were flag-on **single-threaded** assertion failures of the new object model, not races.
- Reuse: the generator list and typing are API level. REPRL and edge coverage need an engine that executes generated programs quickly; with ahead-of-time compilation each sample costs a compile, unless an interpreter configuration of the same runtime exists.

### E: 8. CVE audit

**What it is**: a defensive audit by *mechanism class*. Concurrency CVEs and bug families from other runtimes were catalogued (`docs/threads/cve/jsengine-sab.md` 343 lines: V8/JSC/SpiderMonkey SAB, Atomics, workers, wasm; `jvm.md` 499: HotSpot; `runtime-general.md` 226: CLR, Go, BEAM, classic race exploitation), merged "by *failing invariant*, not by runtime" into **20 classes** (`docs/threads/CVE-AUDIT.md:19`). "Classes are the unit of testing, not individual CVEs" (`:14`). For each class: `docs/threads/cve/map-MC-*.md` (242-579 lines) lists the old engine's surfaces (S1..Sn) with verdicts, and `JSTests/threads/cve/mc-<class>-*.js` are susceptibility tests.

| Class | Tests | Meaning (`CVE-AUDIT.md`) | Rollup 2026-06-15 (`CVE-AUDIT-RESULTS.md:192-356`) | Without a JIT |
|---|---|---|---|---|
| MC-DF | 6 | double fetch / TOCTOU: validate on one read, act on another | core immune; 3 susceptible (A2, B1, B2) | **stays** (runtime fast paths, sort, copy, typed arrays) |
| MC-TEAR | 4 | multi-word value (type+data, base+length, shape+storage) published by several stores | core immune; 1 susceptible (async generator) | **stays**, highest severity ("user-code RCE precedent (Go)") |
| MC-GROW | 3 | resize/detach/transfer against a cached base+length | 2 susceptible (A1 confirmed SEGV, B4) | **stays** |
| MC-JIT | 4 | compiler proves an invariant once, a second mutator falsifies it | 1 central susceptible (B3) | changes form: **applies to ahead-of-time optimizations** (hoisting, CSE, bounds-check or shape-check elimination across points where another thread can write) |
| MC-REENT | 2 | user JS runs inside an operation that pre-validated state; used as a *finder* | immune (8 surfaces) | **stays** |
| MC-VAL | 5 | consumer of "validated" input assumes more than the validator enforced | 0 susceptible, 5 needs-test | **stays** (caches, atom table, thread-id reuse); LLInt cache part goes |
| MC-INT | 1 | integer arithmetic on sizes/limits of shared or growable storage | immune | **stays** |
| MC-LIFE | 4 | refcount/ownership of shared backing stores | 3 susceptible (B4, B10, B11; two embedder only) | **stays** |
| MC-TDWN | 3 | thread/VM teardown against in-flight work | 1 susceptible (B12) | **stays** |
| MC-WAIT | 1 | waiter-list node lifetime, timeout against notify, lost wake-up | immune after the S3a fix (was 13/20 lost wake-ups) | **stays** |
| MC-LOCK | 3 | lock word / state machine transition race (CoW, safepoint states) | immune; 3 needs-test | **stays** |
| MC-GC | 4 | collector's view against the mutator's: premature reclaim, weak tables, finalizers, barrier split from its store | 4 susceptible (A3 confirmed SEGV, B7-B9) | **stays** |
| MC-SAFE | 5 | thread cannot be stopped (poll elision), or stops in a misdescribed state | mostly immune; 1 residual (B16) | **stays** (poll placement in compiled loops, long native regions such as regexp) |
| MC-CODE | 3 | code bytes or entry state changed under a concurrent executor | 2 susceptible (B5, B6) | **mostly goes** (no patching, jettison or i-cache ordering); remains only if dispatch tables or lazy linking are mutated |
| MC-INIT | 5 | reachable before initialized; lazy materialization; metadata swap against use | immune (11 surfaces) after fixes | **stays** (lazy globals, ropes, arguments objects) |
| MC-PRIM | 5 | an "atomic"/privileged primitive trusts an invariant others can break | 1 susceptible (A4) | **stays** (property Atomics, generator resume claim) |
| MC-HAND | 2 | cancel/completion/ownership hand-off race | immune | **stays** (join, async tickets, `Thread.restrict` claim) |
| MC-AINT | 2 | asynchronous interruption at an unsafe point | immune | **stays** as a design rule: termination "MUST be safepoint-polled" |
| MC-SPEC | 1 | shared memory as a high-resolution timer for speculative side channels | structural, embedder obligation (B15) | **stays** |
| MC-DOS | 2 | unbounded shared-resource growth | 2 susceptible, availability only | **stays**; the retired-JIT-artifact part goes |

Cross-cutting rules (`CVE-AUDIT.md:297-317`): "No finding may be downgraded on "window too small" grounds — only on "no writer can exist" grounds"; fail-stop is acceptable containment; a GC turns UAF into disclosure but tearing stays memory-unsafe; "Sharing concentrates bugs"; the second mutator includes the engine's own compiler and marker threads. Priority order: `:321-340`.

**Results**: 2026-06-15: 6 Tier-A items with executed crash evidence (A1 typed-array detach null vector; A2 `TypedArray.prototype.sort` in place, ASan heap-buffer-overflow; A3 weak-table registry against prune; A4 async generator resume, 15/15; A5 structure transition replay, "Reproduces with `--useJIT=0`"; A6 pre-existing `waitAsync` GC UAF reachable flag off) and 16 Tier-B (`CVE-AUDIT-RESULTS.md:32-146`). **None of A1-A6 needs a JIT.** Closure 2026-06-19 (`:502-595`): `--cve` Debug GIL off 60 pass / 3 fail (test-side) / 2 skip; Tier-B: 12 fixed, 1 partial (B3), 2 documented, 1 deferred by design (B13). Latest: 62 / 48 passed, 0 failed (see 3.4 for my reading of the modes).
The crash logs that RESULTS cites (`cve/*.CRASH.log`, `*.crash.txt`) are **not in this checkout**; only one `.DIAGNOSIS.md` and one `.stw-variant.txt` remain.

**Reuse**: `CVE-AUDIT.md` and the three catalogs are independent of the implementation. The `map-MC-*.md` files are file:line arguments about the old engine; their *surface lists* are a good prompt for a new audit. Tests: 58 of 65 are A or B.

### E: 9. Lints

| Lint | Enforces | Kind |
|---|---|---|
| `Tools/threads/lint-lockorder-u20.sh` | grep rules R1-R5 for GC locks: every `m_gcConductorLock` tryLock site carries a disposition marker; `m_markingMutex` acquired only in allow-listed marking files; the mutator mark-stack lock is a terminal leaf (no acquisition within the next 8 lines); chain `NL > GCL > m_markingMutex > CMS` has no reverse edge. Runtime companion `JSTests/threads/congc-t2-lockorder-lint.js` | **general thread safety** (lock order), encoded for JSC files |
| `JSTests/threads/jit/lint.sh` | grep lints: LLInt metadata publication only through single-word choke points (I13), count-pinned raw butterfly loads (I14), watchpoint classification (I10), poll placement (I21), private-name caches | old design's choke points |
| `lint-threads-mode.py` | 6 rules for functions compiled once per threads mode (macro pairing, no mode read before the mode is latched, ...) | flag-off landing |
| `lint-mode-test-form.sh` | every mode test addresses the mode page directly, not through the GOT (x86-64 ELF disassembly) | flag-off landing (cost) |
| `lint-llint-flagoff-bodies.py` | flag-off interpreter bodies contain no test of the flag | flag-off landing |
| `lint-llint-threaded-entries.sh` | every threaded twin of an opcode is installed, and nothing else | flag-off landing (also correctness of dispatch) |
| `lint-flagoff-call-forms.sh` | two code forms on the call path (needs the JIT disassembler) | flag-off landing (cost) |
| `lint-flagoff-build-forms.sh` | four build/layout forms, also under thin LTO | flag-off landing (cost) |
| `lint-upstream-removed-lines.py` | lines the branch re-adds that upstream removed; run after each rebase | rebase hygiene |

Related checkers: `flagoff-ledger.py` (classifies every added line as gated or ungated; `--check` fails when an existing function gains ungated lines without a reviewed row in `docs/threads/FLAG-OFF-LEDGER.tsv`), `golden-compare.py` (JIT code of `main` against the branch, flag off), `v5a-identity.sh` (40 stress tests, `--useJSThreads=false` against no flag, byte-compare), `arm64-llint-check.sh`, `arm64-syntax-check.sh`, `record-layout.sh`.
Other static scans (`docs/threads/SCAN-RESULTS.md`): UBSan (54 in-scope reports, 53 pre-existing), clang-tidy/analyzer, semgrep custom concurrency rules (33 hits, all false positives), CodeQL skipped; JSC validation-option sweeps over the corpus (`validateDFGClobberize`, `validateExceptionChecks` 44/50/4 then 95/0/4 after fixes, zombie+scribble, `verifyGC`). The directory `Tools/threads/scan/` that it cites is **not in this checkout**.
In-engine verifiers used as options: `--verifyConcurrentButterfly`, `--validateButterflyTagDiscipline`, `--countJSThreadsCounters` + `$vm.jsThreadsCounter` (20 tests assert "this path took no stop / no slow path" by counter). The *pattern* of exact event counters as a test oracle is reusable.

### E: 10. Benchmarks

**Serial gate** (`JSTests/threads/bench/`, `Tools/threads/bench-gate.sh`, `docs/threads/BENCH.md`): 8 microbenchmarks (inline/out-of-line property read/write, array element read/write, megamorphic access, transition-heavy constructor). Fresh process each, 20 warm-up + 50 measured iterations, checksum checked every iteration, median of 9 runs, **fails above 1 %** over `Tools/threads/baseline.json`. No `Thread`. Purpose: zero cost for single-threaded code. Workloads portable; thresholds and warm-up are JIT oriented.

**Parallel-self scaling** (`JSTests/threads/scaling/`, `Tools/threads/scaling-gate.sh`, `Tools/threads/INTEGRATE-scaling.md`): criterion "near-linear scalability running a program in parallel with itself, with NO deliberate sharing" (`scaling-gate.sh:5-6`).
- Workloads: `richards-like` (control flow + properties, low allocation), `raytrace-like` (doubles + small short-lived objects), `splay-like` (allocation, pointer churn, GC), `string-heavy` (ropes, flattening, atomization), `map-heavy` (Map/Set churn); `lock-fairness.js` (N threads on one Lock; hard asserts on exclusion and lost updates, soft starvation verdict); `write-barrier-idle-fence.js` (engine specific).
- `speedup(N) = N * T(1) / T(N)`, N in {1,2,4,8}, median of 3. `--gate` floors: speedup(4) >= 2.8 and speedup(8) >= 4.5; `splay-like` 2.0 / 3.0; serial identity: T(1) flag on within 5 % of flag off. Report-only by default; a wrong checksum is fatal in either mode; `--gate` refuses a sweep without 4 and 8.
- Each thread's checksum must equal a serial reference, so the suite is also a correctness test ("cross-thread interference"). Optional in-process tripwire `SCALING_SELF_TRIPWIRE=1`: best-of-3 T(2) < 1.75 x T(1), workload amplified until T(1) >= 250 ms.
- Results: run 2 (June) violated every floor, e.g. richards-like 0.19x at 4 (`docs/threads/SCALEBENCH.md:628-634`); eighth round, GIL off at 2/4/8: raytrace-like 1.67/3.27/5.71, splay-like 1.75/3.11/5.23, map-heavy 1.88/3.34/5.13, string-heavy 1.09/2.24/2.66 (bimodal) (`docs/threads/PERF-RESULTS.md:314-320`).
- Dependencies: `preciseTime` (falls back to `Date.now`), `print`, globals injected by `jsc -e`.

**Big program** (`Tools/threads/scalebench/`, `SPEC.md`, `docs/threads/SCALEBENCH.md` 4,041 lines): "Concurrent Document Index & Query Engine": inverted index in three phases (A ingest, B 90/10 query/write, C analytics), 128 shards, hand-rolled barrier from `Lock`+`Condition`, splitmix64 + FNV-1a. **Identical program in JS (`js/bench.js`, 2,347 lines), Go (`go/main.go`, 1,660) and Java (`Bench.java`, 1,295) with bit-identical checksums across languages and thread counts.** W in 1..64, medians of 5. First results: JS total 33,373 ms at W=1 against Go 1,737 and Java 1,893; W >= 4 did not complete (`scalebench/RESULTS.md`). Later used as a stability gate (10/10 at W=16, 5/5 at W=32, identical checksums; `docs/threads/MEGA-RUN-RESULTS.md:27-28`). JS needs `print`, `preciseTime`, shell `arguments`, `readFile("/proc/self/environ")`. The cross-W checksum makes it a strong correctness oracle.

**Others**: `Tools/threads/perf/` (`run-jetstream.sh`, `run-micro.sh`, `scale-objlit.js`, `scale-objadd.js`) and `perf/flagoff/` (about 60 scripts comparing flag off with `main`: main-thread cycles, RSS, text size, callgrind parity). The measurement rules in `perf/flagoff/README.md` are general; the scripts are specific to the flag-off landing.

### E: 11. GC stress matrix (`Tools/threads/gc-stress-matrix.sh`, `Tools/threads/INTEGRATE-gc-stress.md`)

Re-runs the existing corpus under four GC regimes, with the runner's discipline (directives, option probe, premise skip, timeout):

| Mode | Options | Timeout factor |
|---|---|---|
| `scribble` | `--scribbleFreeCells=1` (freed cells poisoned at sweep) | x1 |
| `zombie` | `--useZombieMode=1` (dead objects scribbled `0xbadbeef0`) | x1 |
| `contgc` | `--collectContinuously=1` | x2 |
| `eden` | `--collectContinuously=1 --collectContinuouslyPeriodMS=1 --forceGCSlowPaths=1` | x4 |

Mode options are appended after the test's own. Scope must be explicit: `--quick` (`races/`, `jit/`, `gc-stress/` x scribble+contgc), `--full`, or `--mode=`; a bare call prints the plan and the worst-case time and exits 2. TIMEOUT is a separate column, still a nonzero exit. Its corpus globs are the older subset (`api, atomics, races, heap-*, objectmodel, vmstate, gc-stress, jit`).
Recorded outcome: `contgc`, GIL off, Debug: 30 s stop-watchdog wedge, `races/counter-lock.js` deterministic (`docs/threads/MEGA-RUN-RESULTS.md:84-87`, 2026-06-12). I did not find a later full-matrix table.
Reuse: the four regimes (poison on free, continuous collection, every allocation on the slow path) are generic collector test modes; the new runtime needs equivalents.

### E: 12. Bun level

- **One Bun test** uses the API (*measured*: `grep -rl "new Thread("` under `bun:test/{js,cli,regression,integration}`): `bun:test/js/bun/net/socket.test.ts:4723`. It spawns `bun -e` with `BUN_JSC_useJSThreads: "1"`, creates and stops listeners, then runs an allocating `new Thread(...).join()` so that a collection is conducted from a spawned thread. There is no threads test directory in Bun.
- **Embedder changes**: commit `6e39e39c579` "Embedder changes for the JS threads WebKit branch, refitted to current main": 14 files, +176/-22: `src/jsc/bindings/{BunClientData.cpp,BunClientData.h,StrongRef.cpp,ZigGlobalObject.cpp,JSEnvironmentVariableMap.cpp,JSInspectorProfiler.cpp,JSONRowsToJS.cpp,TopExceptionScopeBinding.cpp,vm/NodeVMRunTermination.cpp,webcore/URLPatternComponent.cpp}`, `src/jsc/{Strong.rs,TopExceptionScope.rs}`, `src/codegen/generate-classes.ts`. Direct references: `ZigGlobalObject.cpp:418` "A spawned JS thread (useJSThreads) has no thread-local default global", `:582`, `:4700`; `BunClientData.h:471` `isOnSpawnedJSThreadSlow()`.
- **How Bun was used as a test load** (`LANDING-PLAN.md:1920-1960, 2142-2152`): twelve test directories in three modes (flag off / GIL on / GIL off), the flag-on modes "with a preload that keeps a second JS thread alive for the whole process (`Thread` running a timed `Atomics.wait` loop with a little allocation)". Example: `web/fetch` 11417-4 / 11417-4 / 11381-40. I did not find the preload file in either tree.
- Stated weakness: "Bun's Rust host functions find their `VirtualMachine` through a thread-local that only Bun's own threads set, so essentially every Bun native — `console.log`, timers, `require`, `fetch`, `process.env` ... — is unusable from a JSC-spawned thread today (a null dereference in Release)". "This is the weakest leg".
- Pitfall recorded there: earlier Bun "GIL-off" runs passed the options as `JSC_*` instead of `BUN_JSC_*`: "those runs were GIL-on runs". This is the reason for the runner's effective-mode probe.

### E: 13. Other evidence directories

`Tools/threads/bughunt/`: case files of long hunts (`EVIDENCE.md` 61 KB, `prev-butterfly-stress-20260609/`, `prev-gc-undermark-20260610/`, `stw-watchdog/`, `varargs-fix/`, `w16/`, `deepwater/{DIVE,LEDGER,LIMP}.md`) with repro scripts, instrumentation patches and campaign scripts. "Limp build" = Debug+ASan with `-fsanitize-recover=address`, so a run continues past the first report (`deepwater/DIVE.md:3-6`). Specific to the old engine; useful as method examples.
The five methods the project relied on, with their yields: `LANDING-PLAN.md:2105-2152` (written tests, amplifier, mirror, reading, an application).

### E: 14. Not found / not sure

- `Tools/threads/scan/`, `Tools/threads/tsan/reports-r*.log` and per-test TSan logs, `JSTests/threads/cve/*.CRASH.log` / `*.crash.txt`, `Tools/threads/test262-on-thread.sh`, `Tools/threads/fuzz/campaign-*.log`: cited by documents, absent from this checkout.
- How the `TSanJIT` build directory is configured as a script: not found. `tsan.sh` still configures the C loop build; the flags are only in `TSAN-TRIAGE.md:20-26`.
- Whether Fuzzilli was ever run GIL off: no record found.
- Whether the full gc-stress matrix was run after June: not found.
- The Bun keep-alive preload and the "patch notes" with the fifteen entry points: not found.
- `TSAN-TRIAGE.md` (4,057 lines): I read only sections 0-2 (`:1-110`). `SCALEBENCH.md`: only headings and two tables. `map-MC-*.md`, `cve/{jvm,jsengine-sab,runtime-general}.md`: sizes only, contents not read. `LANDING-PLAN.md`: grep hits and the cited ranges only. `JSCThreadsProfile.swift`: not read (generator list taken from `FUZZ.md`).
- The A/B/C split is syntactic. I read header comments of all tests (first two comment lines) but the bodies of only a handful, so "reusable as is" means "no visible dependency", not "passes on another engine". Tests in A may still encode old-implementation outcomes (error messages, refusal policies, id ranges, allowed result sets).
- Thread id range: `api/thread-id-bounds.js` header says `[1, 0x3fff]`, `SPEC-api.md` I17 says `[1,0x7ffe]`. I did not resolve which is current.

