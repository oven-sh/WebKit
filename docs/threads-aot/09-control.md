# 9. Control: threads, stacks, exceptions, coroutines, jobs, modules

The present state: `research/R6-calls-frames-async.md` (`R6 6.4`, hazards `H-1` to `H-29`), and `research/R4-runtime-state.md`, section 6, for modules.

## 9.1 What is already right

| Fact | Source |
| --- | --- |
| A compiled frame has no `CodeBlock`, callee, count or `this` slot. It is recognised by its return address through read-only tables of the image | R6 1.4 |
| Compiled-to-compiled calls check nothing. Most operations return the exception in a register | R6 3.3 |
| The fault handler is process-wide and stateless: program counter, fault table, registers of the faulting context | R6 4.6 |
| Only the adapters set the pinned registers | R6 2.2 |
| The collector's stack scan handles several threads | R5 7 |
| Workers already run the same image, stubs and C++ on other threads, each with its own VM | R6 8.2 |

## 9.2 Life of a thread

How a thread becomes a mutator and ceases to be one, with all locking and the order against a stop of the world, are rules **T3** and **T4** of 08, section 8.14 (with H3 and H4). Here is what happens around them.

```
new Thread(fn, ...args)                                       on the creating thread
  1  check fn; allocate the JSThread cell: { state, result, native handle, id }
  2  the first thread of the realm: Instance::prepareForThreads()        (07, section 7.6; 04, S5)
  3  allocate a thread ID (16 bits, 0 = main; T1) from the free list under VM::m_threadTableLock.
     If the list is empty and IDs are quarantined: request a full collection, wait for it as Parked, try again. RangeError if none then
  4  root fn, args, the cell and the current async context in the native ThreadStart record
  5  keepAlive(main thread, +1)                                (9.8, 9.11)
  6  create the OS thread (stack size: BUN_JSC_jsThreadStackSizeKB, default 4 MB); RangeError if the OS refuses (undo 3 to 5)
  7  return the cell                                           (the call `synchronizes`: release)

thread main                                                    on the new thread
  8  WTF::Thread registration
  9  register as a mutator: T3                                  (context, allocators, view, ThreadState, registry, heap access)
 10  embedder hook  threadDidStart(context)                    Bun: VirtualMachine, event loop (11, section 11.4)
 11  enter the VM (9.3); install the async context; release the ThreadStart record
 12  result = call fn(...args)                                  through the adapter, like any call from C++
 13  if it threw: state Failed, result = the thrown value.  Else result = the value; if it is a promise of this realm: keepAlive until it settles
 14  run the event loop until it has nothing to do and nothing keeps it alive (9.8), and closeInbox() succeeds (T2).
     From here on the thread counts as ended wherever a job looks for its home (T2)
 15  T4, first step: a last sweep of the thread's affine blocks (08, H10); embedder hook  threadWillEnd(context): close handles, run exit hooks of natives
 16  publish: store result; state.store(Finished | Failed, release); wake joiners; post the settle tasks of asyncJoin
 17  leave the VM
 18  T4, the rest: unregister. The collector knows nothing of the thread any more. The ID is quarantined (T1)
 19  keepAlive(main thread, -1)
```

The handles of the `ThreadStart` record are made by the parent and released by the child. `Strong<>` tolerates that (08, section 8.2: a slot that is released on another thread goes to a return list of the owning set).

Step 3: `new Thread` is declared `waits` (04, S5). The collection that it asks for begins after the request, hence after the T4 of every ID that was quarantined then, and frees them all (T1). So `RangeError` means what 02, section 2.2 says: 65,534 threads are alive. A program that starts short threads one after the other and allocates little pays one full collection per 65,534 threads.

`join()`: load-acquire `state`; if running, enter the wait-for graph (W1, 9.14), `ReleaseHeapAccessScope`, and wait on the cell's condition: the state is tested again under the condition's lock, and step 16 notifies under it. After it returns the joiner does not drain its jobs (no job runs inside a blocking call; they run when the current job ends, as always).

The OS thread is detached and holds a reference to the VM.

`Thread.current`: a word of the view. `thread.id`: a field of the cell.

| # | Rule |
| --- | --- |
| **T1 thread IDs** | The ID of a thread that has ended is **quarantined**. It returns to the free list of `VM::m_threadTable` at the end of the first full collection that began after T4. In that collection every carrier of a thread's ID that the collector reaches gets a quarantined ID replaced by `NoThread` (0xFFFF), which equals no thread's ID. The carriers: `SavedFrame`, `AOT::Generator`, `JSAsyncGenerator`, `JSGenerator`, `JSPromiseReaction`, `JSPromiseCombinatorsGlobalContext`, `JSFinalizationRegistry`, a module record and a `ModuleRegistryEntry` (MD10), each rewritten by its visitor; the Structure of a restricted object, which names the owner (`Restricted(owner)`: 05, section 5.11; 06, section 6.4), rewritten by the Structure's visitor, so that after its owner has ended a restricted object is usable by nobody, and never by a later thread with the same ID (02, section 2.7); the creating thread of a `Weak<>` whose `WeakHandleOwner` is `owner` (08, H9), rewritten by `WeakBlock::reap`, in impls that are `Live` or `Dead`. So no live cell names by its ID a thread other than the one that wrote it. (The store is 16 bits wide and races only with compares.) A thread that has read an ID out of a cell uses it before its own next safepoint, so no ID is held in a register or a C++ local over the collection that frees it. Native records name a thread by a reference to its `ThreadContext`, not by its ID. Words that are released when their thread ends need no visitor: claim words (07, Z2), owner words of modules (MD9), lock words (9.13). The storage word's `Owned(t)` may outlive `t` (06, section 6.2); the storage of a restricted object is `Shared` and names no thread (06, section 6.4) |
| **T2 inbox and context** | `VM::m_threadTable` maps an ID to a reference-counted `ThreadContext`, under the leaf lock `VM::m_threadTableLock` (08, section 8.14). `post(id, task)` takes the reference under that lock, releases it, then appends under the inbox lock, also a leaf, and wakes the loop. (Not `m_threadsLock`: the conductor holds that for the whole of a stop, waiting for it is a safepoint, and `post` is called where a thread may not park, and by the conductor itself.) A thread leaves its loop only by `closeInbox()`: under the inbox lock, if the inbox is empty set `closed`, else go on with the loop. `post` to an ID that is not in the table, or to a closed inbox, returns false, and the caller does what G2 says. So a task is run by its thread, or its sender learns that it will not be. A successful `closeInbox()` also stores `NoThread` into `homeID`, the word of the view with which `enqueueAtHome`, the settling paths and `then` compare a home, and which `then` writes into a new reaction (until then it equals the thread's ID). Nobody drains the thread's queue any more. So when the hooks of step 15 close handles and thereby settle promises, "home is me" fails everywhere, `post` returns false, and J1 decides: a handler function goes to the main thread |
| **T5 memory of a thread that is published** | Nothing that a thread allocates outside the garbage-collected heap is named by shared state, unless it is never freed or is reference counted. The one case in the run time of compiled programs: the header (`StringImpl`) of a string constant that is made at run time, which a shared string cell names for the life of the program. Header arenas are chunks of one process-wide reservation (today one arena, sized by "a site makes at most one header", for the first Instance of the process: `aot/runtime/AOTProgramStrings.cpp:61-68`). A thread takes a chunk by `fetch_add`, bumps inside it with today's stub, and at its end returns the unused rest to a free list. Used headers are never freed. A thread without a chunk takes the path that exists for a full arena |

## 9.3 Entering and leaving the VM; `JSLock`

`JSLock` is what makes "the mutator" unique today. `didAcquireLock` (`runtime/JSLock.cpp:118-160`) installs the VM's atom table in the thread, sets `lastStackTop`, acquires heap access, sets the stack pointer at entry, registers the thread for scanning, tells the traps.

In mt builds:

| Today | mt |
| --- | --- |
| a recursive mutex with an owner thread | for a registered JavaScript thread `lock()` and `unlock()` count in `ThreadContext::m_lockDepth`. There is no mutual exclusion between JavaScript threads |
| `currentThreadIsHoldingAPILock()` | `ThreadContext::current() && m_lockDepth` |
| the steps of `didAcquireLock` | done once, at steps 9 to 11 above, and for the main thread at VM creation. **Except `machineThreads().addCurrentThread()`**, which is done only for an embedder's thread that is no JavaScript thread. A JavaScript thread is not in `MachineThreads`: its stack is scanned in place from its parked state (08, section 8.13), and nothing suspends it by a signal |
| `willReleaseLock` drains microtasks | drains the thread's own queue |
| `DropAllLocks` | `ReleaseHeapAccessScope` |
| a thread that is not registered (an embedder's helper thread that calls into the API) | takes the real mutex `JSLock::m_embedderLock`, which is outermost in the order of 08, section 8.15 and is waited for by a thread that has no context yet, and becomes a temporary JavaScript thread: context, view and mutator are made at the first entry (T3) and kept in the OS thread's TLS. Bun never does this: other threads post to a loop |

About 60 consumers of "is the API lock held" exist (#249's count). All are assertions or the two rows above.

`VMEntryScope`, `VM::entryScope`, `topEntryFrame`, `VMEntryRecord`: per thread (07, section 7.2). There is no AOT `vmEntry`; LLInt's `doVMEntry` is used. It reads and writes `VM::topCallFrame`, `topEntryFrame` by offset from the `vm` argument: in mt builds its first argument is the `ThreadContext*`, whose layout begins with the words that `doVMEntry` touches (07, V8), and `VMEntryRecord::m_vm` is read from it.

## 9.4 Stacks

- Each thread has a `StackManager` with both limits. The view's `stackMirror` is its only mirror. Prologue checks read `[instanceGPR + stackMirror..]`: unchanged code, the thread's own limit.
- Bounds: the main thread's from `AT_EXECFN` as today; a spawned thread's from `pthread_getattr_np` / `pthread_get_stackaddr_np` / `GetCurrentThreadStackLimits`.
- `ElideStackChecks`' reasoning (at most 8 nested unchecked functions, 4096 bytes, inside the reserved zone) is about one stack. Unchanged.
- The soft reserved zone and the red zone for the fault handler's pushed frame are per stack.
- `RangeError` on overflow is thrown in the thread that overflowed.

## 9.5 Exceptions and faults

| State (R6 3, R4 2.2) | mt |
| --- | --- |
| `VM::m_exception` | per thread. The pending exception lies **in the view**, as a new word `runtime.exceptions.pending`, and `ThreadContext::exception()` refers to that word. Stubs that load `instance->vm->m_exception` today (after host calls, in polls, in the job loop) load `[instanceGPR + constant]`: one load instead of two |
| `topCallFrame`, `topEntryFrame`, unwind targets, as stubs address them | `[instance->thread + constant]` (07, V8) |
| `runtime.exceptions.thrownValue`, `stackPointerOfHandler`, `nameOfUncallableMethod` | private part of the view; roots of the thread's registry |
| The unwinder writes callee saves into the `VMEntryRecord` found through `topEntryFrame` | per thread |
| `CallSiteCache` | per thread |
| Handler tables, call site tables, position tables | image, read-only |
| Exception scope verification (`validateExceptionChecks`) | per thread |
| Termination exception | 9.10 |

**Fault handler** (`aot/image/AOTFaultHandler.cpp`): installed once per process. Concurrent faults on several threads are independent: each handler invocation works on its own context. Loop poll faults compare the address with the poll page of the view in `r13` of the faulting context: the thread's own.
- `s_scratchRegisterBeforeFault` is `thread_local` already.
- Darwin: the handler runs on the Mach exception thread, so a `thread_local` there is the wrong thread's (R6, N-4). The scratch word moves into the faulting thread's view, which the handler finds through the saved register.
- An alternate signal stack per thread is not needed: the handler runs on the faulting thread's stack, above the red zone, as today.

**A fault at a site that is not registered** ends the process today. 04, 05, 06 and 07 (E4) remove the ways in which a race could lead there (a marker is never decoded as a reference; a slot that a handler dereferences as a `GetterSetter` holds one). In validating builds such a fault prints the thread and the site.

## 9.6 Generators and other coroutines

To suspend, a frame's words are copied into a `SavedFrame` cell; to resume, they are copied onto the current stack and control jumps to the saved address (R6 6.1). The words are "whatever compiled code keeps over the suspension".

That includes things of the thread: B3 may keep across the suspension a value that it loaded from an immutable heap of the view (an allocator's address, a table base of a per-thread cache) or an address computed from `instanceGPR`. Resumed on another thread, the frame would bump the first thread's free list. Resumed after its thread has ended, it would use memory that is unmapped.

| # | Rule |
| --- | --- |
| **G1** | **A coroutine belongs to the thread that created it.** `SavedFrame` and `AOT::Generator` get `uint16_t m_thread` (in `Generator` bytes 30 and 31 are free); `JSAsyncGenerator` and the interpreter's `JSGenerator` (a generator, or the frame of an interpreted async function) get an internal field, which the interpreter's resume entries compare (10, section 10.4) |
| **G2** | **A coroutine whose thread has ended is dead.** `next`, `return`, `throw` on it throw `ConcurrentAccessError`. A job that would resume it is dropped, as a pending `await` is dropped at the end of a process. Only reactions whose handler is a function outlive their thread: they run on the main thread (J1) |
| **G3** | **Each of the three places that restore a frame tests the owner**: `StepGenerator`, `RunAsyncGeneratorStep` and the adapter `EnterSavedFrame` (with their C++ twins `Generator::resume`, `resumeAsyncFunction`, `willEnterAsyncGeneratorBody`) compare `m_thread` with the view's ID before they copy a word. Two instructions per resumption. The test is left out only where the coroutine's site is Confined **and** the function that steps it is `MainOnly`. In an `AnyThread` function it stays, also for a Confined site: such a function can run on the main thread as a handler of a thread that has ended (J1), with that thread's Confined objects in its closure, and G2 is enforced by this test. (An `AnyThread` function cannot hand such an object to a `MainOnly` one: what it calls is `AnyThread`, and a variable that it stores to is shared.) 04, section 4.2.3 rests "no frame carries such an object on" on this |
| **G4** | **Fields of a `SavedFrame`, `Generator` or `JSAsyncGenerator` other than `m_thread` are read only by its thread and by the collector.** An async stack trace (`appendDirectAsyncCallers`, `promisesSettledBy`, `aot/runtime/AOTAsyncStackTrace.cpp:75-143`) walks from promise to awaiting frame to that frame's waiter, and reads `m_promiseOrWaiter` by the byte `m_continuation`, which the owner writes in two stores (`aot/runtime/AOTSavedFrame.cpp:256-257`). It stops at a frame whose `m_thread` is not the running thread and appends one frame `at async <thread n>` |

What a failing test of G3 does:

| Kind | Who can get there | Result |
| --- | --- | --- |
| Sync generators | any thread that holds the object | `ConcurrentAccessError("a generator can be resumed only by the thread that created it")` from `next`, `return`, `throw` |
| Async generators | the same, at `StepAsyncGenerator` and the C++ entry points | the same error |
| Async functions, bodies of async generators | nobody, if J2 and J3 hold: they are resumed only by jobs and by hand-overs, which go by the frame's home | the job is sent to the frame's home (J2), or dropped (G2). Validating builds fail with `FrameRestoredOnOtherThread` |

With G1 to G3 the state word of a generator is read and written by one thread, so the plain load, sign test and store stay, and the order "publish `SuspendedAtYield`, then save the frame" (R6 6.5, point 5) harms nobody. The `SavedFrame`'s version lock has one writer again.

The generator **object** is an ordinary object: any thread may hold it, read its properties, pass it on.

This is stricter than #249, which let any thread resume (a claim by compare-and-swap). Lifting it needs one compiler rule, "no value derived from the view is live across an operation that suspends", checked by the verifier as `window` values are, plus the claim. 13, section 13.5.

`FuseGeneratorLoops`, `StartGeneratorsAtFirstStep`, `UnmodifiedGenerator`: unaffected.

## 9.7 Promises and jobs

### The promise's two words

`m_packed` (payload cell in 48 bits, 16 bits of flags) and `m_slot`. What both mean depends on the flags. Each transition is "store `m_slot`, then store `m_packed`", plain, in C++ and in eight stubs (R6 7.1). Races give lost reactions, double settlement, and a resume reaction whose context is no `SavedFrame`: type confusion (R6 7.2).

A promise's allocation site may be Confined (flow: per site for `new Promise`, one abstract object for all promises of an async function). Code whose operand's type admits Confined sites only stays as it is. All other code:

| # | Rule |
| --- | --- |
| **PR1 busy bit** | Flag bit 15 of `m_packed` is free. It is the promise's lock. A transition is: `old = load(m_packed)`; if busy, spin; `compareExchange(m_packed, old, old \| busy)` (acquire); write `m_slot`; `store(m_packed, new)` with busy clear (release). One atomic instruction per transition. Whatever can fail or leave for C++ (reserving a job, a barrier that is needed) comes before the compare-and-swap |
| **PR2** | Each stub tests `m_packed` as one 64-bit word against an expected pattern already. A busy word matches no pattern, so the fast path falls to the slow one, which spins. The stubs' plain first store becomes the compare-and-swap |
| **PR3 settled is final** | Once the status is not `Pending`, `m_slot` never changes. A reader does: load-acquire `m_packed`; if settled, load `m_slot`. No lock. `fulfillPromise` and `rejectPromise` return without effect if the word that they lock is not `Pending` (today an `ASSERT`) |
| **PR4** | `isHandled` (stub `emitLoadSettledValue`, `markAsHandled()`) is set by a compare-and-swap loop that does not proceed while the busy bit is set: PR1 ends in a plain store of a word that was computed before, which would undo an `or` that came in between. On a settled promise no transition can be under way, and an atomic `or` will do |
| **PR5** | `spillInlineReaction` allocates first, then takes the busy bit and checks that the inline reaction is still the one it copied |
| **PR6** | The resolving functions' "already resolved" record is **one word**: the `ResolvingOther` field of the *resolve* function of the pair. Both functions claim by a compare-and-swap on that word. (Today each nulls its own field first, `runtime/JSPromise.cpp:891, 910`: two words, on which two threads would both win.) The same for the pair with an internal microtask |
| **PR7** | No JavaScript runs and nothing parks while the bit is set. The only locks taken under it are `VM::m_threadTableLock` and an inbox lock (08, section 8.15: they rank below the bit). A settling **appends the jobs of reactions whose home is another thread to the inboxes before it clears the bit**; the wake-ups of the loops come after |
| **PR8** | A reader of a promise that is not settled (stack traces, `asyncStackTraceContext`, inspection) takes the busy bit, copies both words, and releases it. There is no lock-free reader of a pending promise |
| **PR9** | `isFirstResolvingFunctionCalled` (`resolve`, `reject`, `fulfill`, `pipeFrom`: test, set, then code that can call `then`, `runtime/JSPromise.cpp:184-215`) is claimed as a transition of PR1. Whoever finds it set returns |
| **PR10** | No bit of the cell tells a Confined promise from another. So C++ (`performPromiseThen`, natives), and every stub whose operand's type admits a Shareable site, follow PR1 to PR9 on every promise and never *write* an inline form that has no home (J1). They can *meet* one: such a promise is Confined, so its home is the running thread |

`Promise.all` and its kin: all `PromiseAllResolveJob`s of one call have one home (J2), which covers the stub that counts down and stores into the values array (`aot/stubs/AOTStubsCoroutinePromises.cpp:215-261`). The resolve-element *functions*, which a foreign thenable's `then` receives, can be called by any thread: `JSPromiseCombinatorsGlobalContext` and `JSPromiseCombinatorsContext` follow D1 of 06, section 6.10.

### A job's home

| # | Rule |
| --- | --- |
| **J1** | **A reaction job runs on the thread that registered the reaction.** If that thread has ended: a reaction whose handler is a function runs on the main thread; a reaction that resumes a coroutine is dropped (G2). **A thread that finds a promise settled**, in `then`, in `await`, or in any stub that reads a settled value, **first moves the reaction jobs of its inbox to the end of its microtask queue**, and only then queues its own job or goes on in place |
| **J2** | **The home of a job that resumes something is a property of its context, not of a promise.** `homeOf(task, context)`: `m_thread` of a `SavedFrame`; the thread field of a `JSAsyncGenerator` or `JSGenerator`; the owner of a module record (9.9); the home field of a combinator's context. Every enqueue of an `InternalMicrotask` of the resuming set goes through `enqueueAtHome(task, context, ..)`, which compares with the running thread and posts otherwise. `JSGlobalObject::queueMicrotask` asserts that it is not given one of them |
| **J3** | **Every place that takes a waiter out of a promise instead of queueing a job** (`emitHandOverToWaiterOfPromise`, `aot/stubs/AOTStubsCoroutinePromises.cpp:350`; `tryScheduleImmediateResume`, `aot/runtime/AOTJobs.cpp:81`; `tryFulfillTakingInlineJob`) first compares `waiter->m_thread` with the running thread, and declines otherwise. The hand-over between a callee and the caller that awaits the call itself (`Continuation::Waiter`) needs no test: a call is synchronous, so both frames have one thread |

The resuming set: `CompiledAsyncFunctionResume`, `CompiledAsyncFunctionResumeAfterTurn`, `AsyncGeneratorBodyCall*`, `AsyncGeneratorYieldAwaited`, `AsyncGeneratorAwaitReturn`, `AsyncGeneratorDriverResume`, `AsyncFunctionResume`, `AsyncModuleExecutionResume`, `PromiseResolveThenableJobWithInternalMicrotask`, its `Fast` form, `PromiseAllResolveJob`.

**Why J2 is about the context.** Not every resume job comes from a promise. `await` of a thenable makes two resolving functions that hold the task and the frame, and hands them to the program's `then`. Whichever thread calls one goes `resolveWithInternalMicrotask`, `fulfillWithInternalMicrotask`, `queueMicrotask(task, .., frame)` (`runtime/JSPromise.cpp:940, 1207`), with no promise in between.

Where the home of a reaction on a promise is found:

| Form of the reaction | Home |
| --- | --- |
| inline `InternalMicrotask` of the resuming set | J2. For three tasks the stubs read it themselves: `SavedFrame::m_thread` for `CompiledAsyncFunctionResume`, the generator's field for `AsyncGeneratorBodyCallNormal` and `AsyncGeneratorYieldAwaited`, whose context is the `JSAsyncGenerator` (`emitRunGeneratorJobWhenSettled`, `aot/stubs/AOTStubsCoroutineSuspension.cpp:710`) |
| `JSPromiseReaction` cell | a new internal field |
| inline handler (derived promise + handler function), other inline microtasks | has no room. Written only by code that PR10 exempts. Elsewhere `then` allocates a `JSPromiseReaction` |

Settling (`fulfillPromise`, `rejectPromise`, stubs `emitFulfillPromise`, `generateReturnAsyncResult`): for each reaction, if its home is the current thread, enqueue on the thread's own queue, as today. Otherwise **post** it, while the busy bit is still set (PR7).

```cpp
bool VM::post(ThreadID, Task&&);        // any thread may call it. T2
```

The inbox is two vectors, reaction jobs and other tasks, under one leaf lock (08, H15), and a root set of the collector. The owner's loop takes both, puts each reaction job at the end of its microtask queue, and drains. It has heap access before it takes the inbox lock, or it swaps the vectors out under the lock, unlocks, and takes access before it looks at a task: `acquireAccess()` asserts that no leaf lock is held (08, section 8.3). After it has taken anything from the inbox, in the loop or by J1, a thread runs E2 of 07, once per batch: the start of a job that another thread sent is the acquiring end of an edge of 02, section 2.10.2, and it is neither a call of the run time nor an inline sequence. `post` returns false if the home has ended (T2); then J1 decides.

`inboxIsNotEmpty` is a byte of the owner's view. `post` sets it under the inbox lock when it appends a reaction job; the owner clears it under the lock when it takes the jobs. It is the one word of a view that other threads store to (they find the view through the `ThreadContext` that `post` holds a reference to).

The stubs test "home is me" with one compare against the view's ID and leave for C++ otherwise.

**Ordering.** Reactions of one promise that one thread registered run in the order of registration (02, section 2.6), also when another thread settles the promise between two registrations.

- Those registered before the settling are appended in list order to one inbox.
- One registered after the settling found the promise settled. It read the settled word with acquire; the settler stored that word with release, after it had appended the earlier jobs and set `inboxIsNotEmpty` (PR7). So the byte is seen, and J1 moves the earlier jobs into the microtask queue in front of the new one.
- Without J1's second half the earlier job would lie in the inbox, which the *loop* empties, while the later one lay in the microtask queue, which is drained first.

Cost: one load and a branch on each path that has found a promise settled. In the stubs: `emitLoadSettledValue`, the test "the queue is empty, so running the continuation now is unobservable" (`emitBranchUnlessFrameMayContinue`), and the reservation of a job for a settled promise test the byte and leave for C++ if it is set.

The settle happens before the job (lock of the inbox).

### The queue

One `MicrotaskQueue` per thread (`ThreadContext`). The view's `realm.microtaskQueue` points at it. `MicrotaskQueue::drainImpl` calls `handleSafepointRequests()` after each job (08, section 8.4), so a long chain of jobs holds up no stop and no exit. So the stubs that reserve, fill, publish and dequeue jobs (`aot/stubs/AOTStubsCoroutineJobs.cpp:42-210`), the job loop inside the adapter `EnterSavedFrame`, and the test "the queue is empty, so running the continuation now is unobservable" work on one thread's queue and stay as they are, but for G3.
`JSGlobalObject::m_microtaskQueue` becomes an accessor. All queues are in `VM::m_microtaskQueues`. A thread's queue is added and removed in T3 and T4. The collector visits the queues while it holds the lock as conductor (08, H3).

`awaitMustSuspend`, `frameThatMayContinue`, `nextEntryEndsJob`, `waiterToResume` and the other hand-over words: private part of the view. C++ that sets them in front of an entry of the adapter (`aot/runtime/AOTAsyncResumption.cpp:115, 157`, `aot/runtime/AOTJobs.cpp:193`) reaches them through the running thread's view (07, V4). `didChangeMicrotaskQueueing` recomputes the calling thread's view; a change that concerns all threads (a debugger) is applied to each view while parked.

`queueMicrotask(fn)`: the caller's queue.

### Async context

One slot per realm today (`JSGlobalObject::m_asyncContextData`, field 0), swapped around each job by `AsyncContextSwapScope` and by stubs. In mt builds each thread has its own tuple cell. The view's private `realm` words and the link-time constant `$asyncContext` (the view's `linkTimeConstants[]` are private) name the thread's. A reaction carries the context captured at registration, as today, so the store follows the reaction; with J1 it also stays on its thread.

The context crosses threads at `new Thread` (step 4) without being an operand of the call. To flow `$asyncContext` is therefore **one** abstract location for all threads, shared as soon as thread code reads it (04, section 4.2.3), although at run time there is one per thread.

### Unhandled rejections

The tracker (`promiseRejectionTracker`) is called on the thread that rejects or handles. Bun's list of pending rejections is per Bun VM, hence per thread. The user's handlers are functions of the main thread's `process` object: the report is posted to the main thread.

## 9.8 The event loop of a thread

The engine needs four things from the embedder (`GlobalObjectMethodTable`-style hooks, or `ThreadClient`):

| Hook | Meaning |
| --- | --- |
| `threadDidStart(ThreadContext&)`, `threadWillEnd(ThreadContext&)` | make and destroy the embedder's per-thread state |
| `runLoopUntilIdle(ThreadContext&)` | run tasks, timers, I/O and jobs until nothing is left and nothing keeps the loop alive. Waits with heap access released |
| `wake(ThreadContext&)` | callable from any thread: the inbox has something |
| `keepAlive(ThreadContext&, ±1)` | callable from any thread (an atomic count, and `wake` when it reaches 0). The engine's own reasons to stay: the promise that `fn` returned, `asyncHold`, `asyncWait`, `waitAsync` on a property, a module that the thread evaluates asynchronously (MD9), on the main thread each running thread. Not `asyncJoin` (two threads that `asyncJoin` each other must not keep each other alive) |

The `jsc` shell and `testaot`'s stand-in host get a minimal loop: inbox, timers, jobs.

## 9.9 Modules

Today (R4 6.4): one status byte per module, `Unlinked < Linked < InRecord < Evaluated < Evaluating..`. An importer's compiled body contains, per dependency, `load8; compare; store8` as the claim to evaluate it. `Evaluating` is read as "my own cycle: go on".

Everything that the compiler derives from "a module of the image is evaluated at most once" hangs on this (04, section 4.7).

A module is evaluated once. Several evaluations may be under way at once, on disjoint sets of modules.

| # | Rule |
| --- | --- |
| **MD1** | The fast path stays in line: **load-acquire** of the status byte; `Evaluated` is final; go on. (x86-64: the same `cmp byte`.) Then the compare of 07, E2a |
| **MD2** | Every other value calls `operationAOTBeginModuleEvaluation(m)`, which is declared `waits` (04, S5). The inline claim is gone: it ran once per module per program |
| **MD3'** | `Modules::m_lock` is a leaf lock, a table lock of 07, section 7.7. It is a reference to the lock of the realm's loader, `JSModuleLoader::m_lock`, and the claims live in a record `ModuleClaims` of the loader, because a realm of `node:vm` has a loader and no `Modules` (10, section 10.3). No JavaScript runs under it. It protects the containers (MD7), the status bytes other than `Evaluated`, an **owner word** per module (thread ID plus one; 0 = nobody; where it lies: MD10), and the claims with their tickets |
| **MD3''** | **A claim takes all that it can see, or nothing. Then it grows.** A **claim** is a reference-counted record: its owner, its modules, a state word (`Open`, `Ended`), a list of tickets. `beginEvaluation(root)` (from MD2, `require`, the job of `import()`, the loading of one of Bun's own modules), under the lock, walks the modules that the image's table shows as reachable from `root` and that are not `Evaluated`. If one of them is owned by another thread it takes nothing and waits for that claim (below), then starts again. Otherwise it makes itself the owner of all of them, as one claim, and releases the lock. Link, load and evaluation then work on the thread's own set. |
| **MD3'' growth** | The set is not always known in advance: a `require(x)` in a body names its target when the statement runs, and a module that is not compiled shows its imports after it has been fetched and parsed (MD10). Whenever load, link or a nested `require` finds a module that is not `Evaluated` and not in the claim: under `m_lock`, if nobody owns it, it joins the claim; if another thread owns it, the claimant waits for that claim, **with its own claim held**, before it links against the module. So no module becomes a pending parent of a module with another owner. |
| **MD3'' waiting** | Under `m_lock`, in the critical section in which it saw the owner, the waiter takes a reference to the claim. A synchronous caller releases the lock, enters the graph (W1) and sleeps by `ParkingLot::compareAndPark` on the claim's state word, with the value `Open`, heap access released. The job of `import()` leaves a ticket in the claim, under `m_lock`, unless the claim has ended by then; a ticket of a job that holds a claim itself is an edge of the graph too (W1). |
| **MD3'' end** | A claim ends when the call that made it returns, or, with top-level `await`, when its root has settled (MD6). Its modules are then `Evaluated` or failed. Under `m_lock` the owner words are cleared, the tickets are taken out and the state word is set to `Ended` (release). After the unlock `unparkAll` is called on the word, always, and each ticket's job is posted to its importer's inbox again. So a waiter that saw the owner either finds `Ended` in its `compareAndPark`, or when it comes to leave its ticket, or it is found by the one who ends the claim. The state word lies in the record, so it has one address for all threads |
| **MD4** | On its own set a thread runs the single-threaded algorithm: `Evaluating` with owner "me" is the thread's own cycle. `evaluatingMark` is a word of the thread's view; `m_innermostEvaluation`, `m_isAskingHost` are members of the `ThreadContext`; `isBeingRequired` is read together with the owner. The scan of `failModulesLeftEvaluating` (all bytes equal to a mark, `aot/runtime/AOTModuleEvaluation.cpp:572-582`) looks only at modules that the running thread owns: two threads may use one mark |
| **MD5** | `FinishModuleEvaluation`: **store-release** of `Evaluated`. It is declared `synchronizes` (04, section 4.4) |
| **MD6 top-level await** | A module in `EvaluatingAsync` keeps its owner, and its claim stays open, until the cycle root's capability settles. `import()` from another thread gets a promise, as always; its job waits by the ticket of MD3''. A **synchronous** need (`require`, a body called where it is first needed) of a module that is evaluating asynchronously on another thread blocks until the claim has ended |
| **MD7** | Containers of `Modules` (`m_records`, `m_loaded`, `m_storedReferences` ..) are touched under `m_lock` only, for the length of the touch |
| **MD7'** | Namespace objects, module objects, `require` functions, `import.meta`: made aside, then, under `m_lock`, looked for again and published by a release store into the shared statics. No claim is needed, so a thread gets them for an evaluated module whatever other threads evaluate |
| **MD8** | Every step of the async half (`executeAsync`, `asyncExecutionFulfilled`, `asyncExecutionRejected`) runs on the owner: the home of `AsyncModuleExecutionResume` is the record's owner (J2). `asyncExecutionFulfilled` executes the bodies of waiting parents inside the child's completion (`runtime/CyclicModuleRecord.cpp:868-899`). It compares each parent's owner with the running thread. By MD3'' they are equal. A parent with another owner is not executed there: its step is posted to its owner (`enqueueAtHome`, J2), and a parent whose owner has ended fails as an evaluation that was abandoned. So no body runs on a thread that did not ask for it, which `MainOnly` rests on (04, section 4.2.2), and nothing here ends the process (02, M11) |
| **MD9** | A module in `EvaluatingAsync` holds a keep-alive on its owner's loop (9.8). So a claim does not outlive its thread |
| **MD10 modules outside the image** | The engine's `JSModuleLoader` takes part in the claims. (a) `m_moduleMap`, and the loader's other containers (`m_resolutionFailures`, `m_loadedModules`, `m_prelinkedRecords`), are touched under the loader's lock only (MD3'). (b) The owner word of a module that has a record lies in the record (`AbstractModuleRecord`), for compiled and uncompiled modules alike; before there is a record, in its `ModuleRegistryEntry`; for a module of the image without a record, in a table beside the status bytes. (c) `ensureRegistered` is a point of growth: an entry that it makes joins the running thread's claim; an entry that it finds, which another thread owns and whose record is not evaluated, is waited for as MD3'' says. An `import()` of a name that the image does not know opens a claim with that one entry. (d) The place that makes a module a pending parent (`runtime/AbstractModuleRecord.cpp:2003-2008`) is reached only with a required module that is evaluated or has the running thread as owner. (e) The loader's steps (fetch, parse, link, evaluate) are reactions that the owner registered, so they run on the owner (J1). (f) A claim stays in one realm; no thread holds two loaders' locks at once; a wait across realms is an edge of W1 like any other |

**What this gives.** The entry module's body *is* the program, and may block for its whole length:

```ts
const workers = files.map(file => new Thread(() => { .. }));
for (const w of workers) w.join();          // top level of the entry module
```

When that body runs, its static imports are `Evaluated`. The main thread's claim holds the entry module (and the members of its cycle, if any). A worker that calls `require`, `import()`, `import.meta`, or a native or library function that is the first to need one of Bun's own modules (`evaluateBuiltinModule` is `Modules::require`, `aot/runtime/AOTBuiltinModules.cpp:48-67`) claims a set that does not meet the main thread's, and evaluates it itself.

**Where the records of uncompiled modules live.** Not in `AOT::Modules`:

| Case | In the source |
| --- | --- |
| a module that the build has listed as not compiled (`ExternalKind::UncompiledCode`) | It has a `ModuleID` in the image's table. Its record is made, linked and evaluated by the engine's `JSModuleLoader`. `Modules::uncompiledRecord` only looks it up in the loader's registry by the resolved key, throws if it is not loaded, and caches it in `LoadedModule::uncompiledRecord` (`aot/runtime/AOTExternalModules.cpp:123-139`) |
| a compiled module that reaches such a module (`ReachesUncompiledCode`) and is still `Unlinked` | `Modules` does not evaluate it. `registerWithLoader` hands its `AOT::ModuleRecord` to the registry (`provideModule`, `:92-102`), with the uncompiled modules as its requests (`appendUncompiledRequests`, `:52-76`). `importThroughLoader` and `evaluateThroughLoaderSynchronously` (`:104-121`; called from `aot/runtime/AOTModuleImports.cpp:92` and `aot/runtime/AOTModuleEvaluation.cpp:804`) let the loader run the standard's algorithm over a graph of both kinds of record |
| `import(x)` of a name that the table does not hold | `importDynamic`, `importUncompiled`, `requestImportModuleForKey` (`aot/runtime/AOTModuleImports.cpp:171-203`). No `ModuleID` at all |
| the registry | `JSModuleLoader::m_moduleMap`, a hash map of a cell, without a lock (`runtime/JSModuleLoader.h:315`) |

So status bytes and a table by `ModuleID` cannot carry the claims of these modules. That is why MD10 puts the owner into the record and the registry entry, and the point of growth into `ensureRegistered`.

**Deadlock.** A first claim takes nothing while it waits. A claim that grows, a body that waits for a set that a nested `require` needs, and a body that calls `join` wait with a claim held. Cycles can then form: a worker needs a module that imports the entry module, while the entry module's body joins the worker; two threads load uncompiled modules that import each other. W1 finds them. The thread whose wait would close the cycle gets `Error("deadlock: ..")`; an `import()` whose ticket would close it is rejected with that error. A body that blocks in `Lock.hold`, `Condition.wait` or `Atomics.wait` for a thread that needs its modules is the program's own deadlock.

**CommonJS**: `evaluateCommonJS` runs under a claim like any evaluation, which grows at each nested `require`. A cycle sees the partial `module.exports`, as today, only on the owner. The price: two threads that enter one CommonJS cycle from its two ends at the same time get `Error("deadlock: ..")` in one of them, where a single thread would have seen a partial `exports`. The other thread completes the cycle, and a second `require` in the first thread succeeds.

**Rejected:** one recursive lock that is held while module bodies run. It is simple, and it makes the program above fail as soon as a worker needs a module.

## 9.10 Traps, termination, watchdog, profiler

| Mechanism | mt |
| --- | --- |
| `VMTraps` | one per thread. A request names a thread or all threads |
| Safepoint requests | `NeedStopForHeap`, `NeedExit`, `NeedYield` are no events. They are bits of `m_trapBits` above `NumberOfEvents`, in none of the masks of `runtime/VMTraps.h:183-186`, so `handleTraps` and `RETURN_IF_EXCEPTION` never see them. `handleSafepointRequests()` answers them. Its callers: `operationAOTPollSlow`, the interpreter's `slow_path_handle_traps`, the job loop. `operationAOTPollAtPrologue` does not ask (08, section 8.4) |
| A stop for the collector or a safepoint operation | the safepoint request `NeedStopForHeap`, set and cleared by the conductor only; nobody takes it (08, section 8.4) |
| Termination of the VM (a Worker's `terminate()`, embedder) | fanned to each thread's trap word under `m_threadsLock` (a parking lock, 08, H15: the caller may lose heap access while it waits). The request is also stored in the `VM`, under the same lock, and a thread that registers afterwards finds it in the critical section of T3 and starts with its trap set: an OS thread that `new Thread` has made and that has not registered yet is in no list. Blocked threads are woken: each blocking primitive looks at the termination event and at `NeedExit` in its thread's `m_trapBits` at each wake-up (9.13). A thread that is terminated unwinds with the termination exception, which releases its claims (07, Z2; MD3'') and the `Lock`s that it holds synchronously; its `Thread` is `Failed` with a new `Error("Thread terminated")` |
| `process.exit` | the safepoint request `NeedExit` on the main thread: X1 (9.11). It is answered only where a thread may park. No thread is unwound |
| A time limit on one call (`node:vm` `timeout`) | the calling thread only |
| Watchdog | per thread |
| "Needs OutOfMemoryError" | per thread |
| Sampling profiler | samples each thread in `VM::m_threads` in turn: suspend, read the stack by return addresses, resume. Suspension by signal is allowed here because the profiler touches no heap state of the suspended thread beyond what `StackVisitor` reads from read-only tables. A thread unregisters from the profiler before it ends |
| Inspector | not available in an mt executable in the first version (03, section 3.8) |
| Signal-based traps | off without the JIT, as today |

## 9.11 Exit

| Event | Behaviour |
| --- | --- |
| The main thread's loop is empty | it waits for spawned threads (each thread holds a keep-alive on the main loop from step 5 to step 19 of 9.2, as a Worker does) |
| `process.exit(code)` | X1 |
| Uncaught exception in a job or timer of a spawned thread | posted to the main thread's `uncaughtException` path |
| The cage's last reserve is exhausted on a spawned thread | the handler gets the engine `VM&` and ends the VM's main Bun VM through its `VmHandle` (11, section 11.4) |
| VM teardown (a Worker with threads ends) | terminate its threads (9.10), join them natively, then destroy the VM. `~VM` asserts that `m_threads` holds only the caller |

**Rule X1.**

1. On the main thread `process.exit(code)` runs the `exit` handlers where it stands, as today, and goes to step 4.
2. On a spawned thread it stores `VM::m_exitRequested = code`, sets the safepoint request `NeedExit` on the main thread, and blocks with heap access released, for good. It enters the wait-for graph with an edge to the main thread (W1), so an `exit` handler that joins it gets the deadlock error instead of hanging. It posts nothing: the main thread may be inside `join`, `hold` or `wait`, where no task runs.
3. `NeedExit` is answered **only where a thread may park**: by `handleSafepointRequests()` (08, section 8.4), which `operationAOTPollSlow` (loop polls, prologues with `EntryIsSafepoint`), the interpreter's `slow_path_handle_traps` and the job loop call, and which does nothing inside a `NoSafepointScope`; and by a blocking primitive, each of which looks at the bit itself at every wake-up (9.13). It is in none of the masks of `handleTraps`. So `operationAOTPollAtPrologue`, which runs under `DeferGCForAWhile` at a place that the caller's code holds to be no safepoint (`aot/runtime/AOTExceptionOperations.cpp:60-72`), and `RETURN_IF_EXCEPTION` do not answer it, and they leave the polls armed. The main thread runs the `exit` handlers where it answers the request, on top of whatever it was doing, and does not return to it. The handlers are ordinary JavaScript at a safepoint: they may allocate, block and be stopped.
4. It then becomes conductor (08, section 8.6) and requests a stop that is never cancelled, so that every other thread is parked or without heap access and the collector is quiet. It waits for the stop at most `BUN_JSC_exitGraceMs`. Then it ends the process without running `atexit` handlers or static destructors. Other threads are not unwound and their native state is not torn down.
5. If the main thread does not reach step 3 within the same time (it is in native code that does not poll), the caller does step 4 itself, without handlers.

## 9.12 `ThreadLocal`

| Case | Storage |
| --- | --- |
| The object is the value of a module-level constant, created once, and flow sees all its uses | a per-thread variable in the private statics (03, section 3.6). `value` is one load or store |
| Otherwise | `ThreadContext::m_threadLocals`: a hash map from the object's key (a 64-bit number unique in the process) to a value. No lock. Values are roots until overwritten or until the thread ends. A `ThreadLocal` that dies leaves its slots in live threads until they end |

## 9.13 `Lock`, `Condition`, `Atomics.wait`

- `JSLockObject`: one word `{ holder: thread ID + 1 (0 = free), waiter bit, async-held bit }` and a queue of async acquirers under an internal lock. The main thread's ID is 0, so the ID itself cannot stand for "held". Fast path in line (04, section 4.13): compare-and-swap from 0 to "my ID + 1" (acquire), followed by the compare of 07, E2a. The release in line is a compare-and-swap back from "my ID + 1" to 0 (release), which fails if a waiter has set its bit meanwhile, and then calls the operation. (Not "test the waiter bit, then store 0": a waiter that sets its bit between the two would never be woken.) The slow path parks in `WTF::ParkingLot` on the word's address, with heap access released. At each wake-up, and every 10 ms, it looks at the termination event and at the safepoint request `NeedExit` in the thread's `m_trapBits`. (`NeedStopForHeap` does not concern it: it has no heap access.)
- "Lock is not recursive" and `Condition.wait`'s test compare the holder with the caller only while the async-held bit is clear. A hold that `asyncHold()` granted belongs to no thread.
- `JSConditionObject`: a FIFO of waiters (sync and async) under an internal lock. `Condition.wait` enqueues itself under the condition's lock and releases the `Lock` while it still holds it, so that no `notify` falls between the two. The slow path of that release takes the lock object's internal lock. The order of the two, which have one rank in 08, section 8.15: the condition's, then the lock object's. Nothing takes them the other way round. The waiter sleeps on a flag of its entry, which `notify` sets under the condition's lock before it wakes.
- The internal locks are leaf locks (08, H15).
- Waiters of `Atomics.wait(o, k)` are keyed by `(cell, property uid)`, not by the slot's address. A waited-on object is kept alive by the list.
- `Atomics.wait(o, k, expected)`: under the list's lock, enqueue. Unlock. Read the property as `Atomics.load` does. If it differs: dequeue, `"not-equal"`. Else sleep on the entry's flag, which `notify` sets under the list's lock before it wakes. (With the read in front of the enqueue, a store and a `notify` between the two would find no waiter, and the waiter would sleep for ever. With the read under the list's lock, a property that needs the object lock would be read against the order of 08, section 8.15.)
- Tickets of `asyncJoin`, `asyncHold`, `asyncWait`, `waitAsync`: a native record with the promise (rooted by a `Strong<>`, which another thread may release: 08, section 8.2), a reference to the registering thread's `ThreadContext` (T2) and a keep-alive. Settling posts a task to that thread. If its inbox is closed the sender settles the promise itself: any thread may settle a promise (PR1), and each reaction goes to its own home (J1).

## 9.14 Waiting for another thread

**Rule W1 (the wait-for graph).** `join`, a wait for a module claim (MD3'', MD6) and a wait for a claim of 07, Z2 are waits for something that *one known thread* will do. Such a wait is blocking in the sense of 08, H1: heap access is released, `VM::m_nativeSerialLock` is released (11, section 11.1), and it is forbidden inside a `NoSafepointScope` (08, S4).

**Edges.** All are written and read under one leaf lock, `VM::m_waitGraphLock`.

| Edge | Stored in | Names | Live while |
| --- | --- | --- | --- |
| a thread that blocks; at most one per thread | `ThreadContext::m_waitsFor` | the thread waited for, and what it is to do: complete (`join`); end a module claim (a reference to the claim); end a claim of Z2 (the canonical address of the claim word and the value that the waiter read). The caller of `process.exit` on a spawned thread has an edge of the first kind to the main thread (X1) | the target's state is still running; the claim's state word is `Open`; the claim word still holds that value |
| a module claim whose job of `import()` has left a ticket while the claim is held (growth) | the claim | the claim waited for | that claim's state word is `Open` |
| a module claim needs its owner's loop | implicit | the owner | always |

**Before it blocks**, or leaves such a ticket, a thread takes the lock, stores its edge, and follows the edges from its target: from a thread along `m_waitsFor`; from a claim to the claim that it waits for, and to its owner. **Only live edges are followed**, and liveness is tested there, under the lock. If the walk comes back to the thread itself, or to a claim that it owns, it removes its edge and does not block: it throws `Error("deadlock: <what waits for what>")`, or rejects the promise of the `import()` with it. (For the engine's own lazy words: 07, Z2, part 3.) Otherwise it releases the lock and blocks.

**The waiter removes its edge**, under the lock, before it returns from the wait.

**Why liveness.** Between the wake-up and the removal the edge is still there, and says nothing any more. The holder of a claim lives on after the claim, unlike the target of a `join`: T waits for a module that U is loading; U ends the claim, wakes T, and calls `t.join()` before T has run. A walk that trusted every edge would go U, T, U and throw in a correct program, and this is the common pattern (9.9). U has set the claim's state word to `Ended` before it woke T, hence before its own walk, so the walk finds T's edge dead and stops.
An edge is held to be dead only if what it waits for has happened, and then its waiter is awake or about to be. So no real cycle is missed. All edges of a cycle are stored under one lock, so the thread that stores the last one sees the others.

`Lock.hold`, `Condition.wait` and `Atomics.wait` are not in the graph: who will release or notify is not known. A cycle through them is the program's own deadlock, and a walk ends at a thread that blocks in one of them.
