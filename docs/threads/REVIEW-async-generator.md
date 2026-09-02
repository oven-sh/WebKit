# Review: the async generator driver with the GIL off

Landing plan section 1.4. Reviewed commit 4fa55c249e62 ("Serialize async
generator requests across threads when the GIL is off") on `sosuke/threads`
at d69451e8b3ba, by someone who had not seen it before. Line numbers are at
that commit.

## Summary

The locking protocol in the commit is correct as far as I can tell. I found
no interleaving in which two threads resume the body through the request
queue, or in which a request is queued and never run. The stress test passes
in every mode.

Three problems on the paths the driver uses are not in the commit, and were
there before it:

1. A `for await` loop queues its own body as the settlement target before
   that body has suspended. GIL-off, another thread can resume the body in
   that gap. Found by reading. Not reproduced.
2. `VM::syncResumeCallCache()` is one cache per VM, used from every thread
   with no lock. Reproduced: a Debug assertion in 5 to 6 of 20 GIL-off runs.
3. GIL-off, a thread can run code compiled while a function had one
   activation after a second activation exists, and read the first
   activation's variables. Reproduced every time with a short script. This
   is not generator code, but it made my first version of the test hang.

## The protocol

Spec terms (ECMA-262, AsyncGenerator objects):

- The generator has a state (suspended-start, suspended-yield, executing,
  draining-queue, completed) and a FIFO queue of requests. A request is a
  completion (normal, return or throw) and a promise capability.
- AsyncGeneratorEnqueue appends a request. `next()`, `return()` and
  `throw()` then decide from the state alone whether to resume the
  generator now (suspended-start or suspended-yield) or to leave the request
  for whoever is running it (executing or draining-queue).
- AsyncGeneratorCompleteStep removes the head request and settles it.
  AsyncGeneratorYield does that, then resumes the body with the next head
  request if the queue is not empty, or stores suspended-yield if it is.
  AsyncGeneratorDrainQueue does the same after the body has finished, and
  stores completed when the queue is empty.

JSC's encoding: the head request is kept in three fields of the generator
(`ResumeValue`, `ResumeMode`, `ResumePromise`), and the rest in a circular
list (`Queue`). The queue is empty when `ResumeMode` is `Empty`. The state is
one int32: `Init` (0) is suspended-start; a positive value is a suspend point
with a reason in the low two bits. The reason `Yield` means suspended-yield.
The reason `Await` means executing.

With the GIL off, the spec's "decide from the state" is not enough. Two
threads can both see suspended-yield and both resume the body. Or a thread
can see executing, queue its request, and return, while the driver has
already checked the queue and is about to store suspended-yield. Then the
request waits forever.

The commit adds a role, "the driver": the one thread that runs the
generator's steps at a time. The role has two rules, and both use the
generator's cell lock:

- Take the role (`JSAsyncGenerator::enqueueGILOff`,
  JSAsyncGenerator.cpp:105). Under the lock: if the queue is not empty, or
  the state is not settled (suspended-start, suspended-yield or completed),
  append the request and return `None`, and someone else is the driver.
  Otherwise this thread is the driver. It queues the request and returns
  `Resume` (or `AwaitReturn` for `return()` on a suspended-start or completed
  generator, having stored draining-queue). Or it settles the request at
  once (`SettleCompleted`) and never holds the role.
- Give up the role (`retireIfQueueEmptyGILOff`, JSAsyncGenerator.cpp:155).
  Under the lock: if the queue is empty, store the settled state and return
  true, and the caller stops touching the generator. If it is not empty,
  return false, and the caller keeps going with the head request.

These are the only points where the driver checks whether the queue is
empty: AsyncGeneratorYield, JSMicrotask.cpp:784, and the loop head in
AsyncGeneratorDrainQueue, JSMicrotask.cpp:602. `dequeueGILOff`
(JSAsyncGenerator.cpp:145) takes the lock too, because it edits the same
fields as a concurrent append.

Between taking the role and giving it up, the driver does not hold the lock.
It runs the body, settles promises, and may suspend at an `await`. In that
case a microtask holds the role, and whichever thread runs the microtask
carries on. The head request stays in the queue for that whole time, so the
queue is not empty, and every other thread's enqueue appends and leaves.

## Checks, question by question

### Can two threads both resume the body?

Not through the queue. The role goes to whichever thread finds the queue
empty and the state settled. The first thing it does is append its request,
so the queue is not empty until CompleteStep removes that request. CompleteStep
runs in one of three states:

- AsyncGeneratorYield stores an `Await` state first. JSMicrotask.cpp:775.
- The body-completion path stores `DrainingQueue`. JSMicrotask.cpp:665.
- AsyncGeneratorAwaitReturn's continuation runs in `DrainingQueue`.

None of these is settled. So another thread cannot take the role between
that CompleteStep and the next retire check, even when the queue is empty in
that gap.

The `for await` path, the first problem in the summary, can resume a body
on two threads. It goes around the queue, not through it. See finding 1.

### Can a request be queued and never run?

No. A thread that does not take the role appends under the lock. The driver
gives up the role only by `retireIfQueueEmptyGILOff`, and that check is under
the same lock. So an append that comes first is seen by the retire check, and
an append that comes after the retire takes the role itself.

The one exit that skips the retire check is an exception after CompleteStep
(`RETURN_IF_EXCEPTION` at JSMicrotask.cpp:779 and 620). CompleteStep settles
a promise, and that does not throw, so only a termination reaches these
exits. Upstream behaves the same on that path.

### Can the queue be edited without the lock?

No, not with the GIL off. `enqueue` is called only from `enqueueGILOff`, and
from the GIL-on branches after each `if (vm.gilOff())` arm returns.
`dequeue` is called only from `dequeueGILOff` and from the GIL-on branch of
CompleteStep. The driver reads the head fields (`resumeValue()`,
`resumeMode()`) without the lock. That is safe: it does so only after a lock
acquire that saw the queue non-empty, and appends never touch the head.

### Is every store of a settled state made under the lock?

Nearly. The exception is safe, but the comment at JSAsyncGenerator.cpp:85
should say so.

Under the lock: the retire stores (suspended-yield, completed), and
`throw()` on suspended-start (completed, JSAsyncGenerator.cpp:134).

Not under the lock: the body's own bytecode stores the suspend state at every
`yield` with a plain `put_internal_field`
(`BytecodeGenerator::emitYieldPoint`, BytecodeGenerator.cpp:5350). For a
plain `yield`, that state has the `Yield` reason, which is suspended-yield.
`asyncGeneratorDispatchSuspend` changes it to `Await` a moment later
(JSMicrotask.cpp:840). In between, an enqueuer that reads the state sees a
settled state. It does not take the role, because the queue is not empty:
the body runs only while it serves the head request. I checked every entry
into `asyncGeneratorBodyCall` for that. The entries are
`asyncGeneratorUnwrapYieldResumption`, the await continuations
(`asyncGeneratorBodyCallNormal`, `asyncGeneratorBodyCallReturn`),
`asyncGeneratorYieldAwaited`'s rejection arm, and `asyncGeneratorDriverResume`.

Proposed change: in the protocol comment, add "the body runs only while its
request is at the head of the queue, so the body's own unlocked store of a
suspend state never lets another thread take the role."

### Is every load that decides "I drive" made under the same lock?

Yes. The only such loads are in `enqueueGILOff` and
`retireIfQueueEmptyGILOff`. The unlocked state loads left in
`asyncGeneratorResume` and `asyncGeneratorAwaitReturn` are asserts. The
driver reads the state there after taking the role, and no other thread can
change it at that point.

### Memory order

The driver's unlocked state stores (`Executing`, or an `Await` state) come
before its next lock release. That release is in `dequeueGILOff` or in the
retire, so an enqueuer that takes the lock later sees them. The head fields
are written under the lock, and read after a lock acquire. I found nothing
that needs a fence beyond the lock.

### return() and throw() on a suspended-start generator

`enqueueGILOff` follows the spec:

- `throw()` stores completed and rejects.
- `return()` queues the request, stores draining-queue, and awaits the value.

If the queue is not empty, the generator is not really suspended-start. A
`next()` has taken the role and has not yet stored `Executing`. The request
is then appended, and it runs after that `next()`. That matches the spec,
because the `next()` is ordered first. Section 4 of the test covers this
race, and both outcomes occur.

### An await in the body that resumes on another thread

GIL-off, a microtask is queued on the thread that queues it
(`JSGlobalObject::queueMicrotaskGILOff`, JSGlobalObject.cpp:4462). So the
body resumes on the thread that resolved the awaited promise. That is fine.
The reaction is registered by `asyncGeneratorDispatchSuspend`, after the body
function has returned. So the body has suspended before any other thread can
resume it. The test does this with `crossThreadTick`.

### A body that throws

`asyncGeneratorBodyCall` stores `DrainingQueue`, rejects the head request, and
drains. The drain loop checks the queue under the lock each time round. Every
request queued in the meantime is settled (`done: true`, a rejection, or an
await for `return()`), and then the state becomes completed. Section 2 of the
test covers this.

### next() called from inside the body, on the same thread

The state is `Executing`, so the request is appended, and the driver reaches
it at the next yield. A `next()` from a `then` getter that runs inside
CompleteStep works the same way. The state is `Await` there, and the retire
check that follows finds the request. Both match the spec.

### Is the cell lock held across JS, an allocation, or a park?

- JS: no. The three functions in JSAsyncGenerator.cpp release the lock
  before they return. The callers settle promises and resume the body after
  that.
- Allocation: yes, in `enqueueGILOff`, which allocates a queue entry
  (`JSSlimPromiseReaction`). A `DeferGC` is taken before the lock. This is
  the pattern SPEC-heap.md section 6 (L4) allows: a cell-lock holder may
  allocate under a `DeferGC` taken before the lock. With the deferral,
  `Heap::collectIfNecessaryOrDefer` records a hint and does not call
  `stopIfNecessary`. So the allocation cannot park for a stop-the-world.
  The `DeferGC` is declared first, so its destructor runs after the lock is
  released. The queue entry is a plain cell, so no second cell lock is taken.
  The counter `t_cellLocksHeldByConcurrentButterfly` does not see this lock,
  because it counts only locks taken in ConcurrentButterfly.cpp.
- Park: no. `dequeueGILOff` and the retire only store fields.

### Are the flag-off and GIL-on paths the same as before?

They behave the same, but they are not byte for byte. Each GIL-off change is
an `if (vm.gilOff())` arm that returns, so the GIL-on code after it is the
old code. The exceptions:

- `asyncGeneratorDrainQueue` (JSMicrotask.cpp:600) is now a `for (;;)` loop
  with a `break`. It does the same thing as the old `while`.
- `asyncGeneratorYield` (JSMicrotask.cpp:791) tests
  `vm.gilOff() || !isQueueEmpty()`.
- `settleDriverWithIteratorResult` has the old code inside an `else`.

The cost with the flag off is one load of `VM::m_gilOff`, and a branch, at
eight sites.

## Findings

Most severe first.

### 1. High. Not in the commit. A for-await body can be resumed on a second thread before it suspends. Not reproduced.

`for await (x of gen)` uses the fused driver when `gen` is a plain async
generator. The loop's body enqueues itself as the settlement target of a
`next` request, and then suspends. Bytecode: `emitAsyncIteratorNext`, then
`emitAwait` (BytecodeGenerator.cpp:5007, 5008). The enqueue is in the LLInt
slow path (LLIntSlowPaths.cpp:2247), in the baseline JIT
(JITCall.cpp:603), and in the DFG/FTL operation. All of them call
`enqueueAsyncGeneratorDriver` (JSMicrotask.cpp:711).

Interleaving, GIL off. Thread A runs an async function or async generator
`outer`, which has `for await (x of g)`. Thread B is driving `g`.

1. A: the slow path calls `enqueueGILOff(g, ..., outer)`. The queue is not
   empty, so it appends and returns `None`.
2. B: `g`'s body yields. `asyncGeneratorYield` calls CompleteStep, which
   dequeues A's request. `settleDriverWithIteratorResult` queues
   `AsyncGeneratorDriverResume(outer)` on B's own microtask queue.
3. B: goes back to its drain loop and runs that job.
   `asyncGeneratorBodyCall(outer)` (or `asyncFunctionGeneratorBodyCall`)
   stores `Executing` and calls `outer`'s body function with `outer`'s frame.
4. A: still inside `outer`'s body. It is between the slow path and
   `op_yield`, and has not saved its registers to the frame. It now runs
   `op_yield`, and returns to its own `asyncGeneratorBodyCall`, which reads
   `outer`'s state.

Now two threads run `outer`, and B has resumed from a frame that A has not
finished writing. The gap is short, so B must run steps 2 and 3 in that time.
In my runs it did not happen. Thread A being preempted in that gap would be
enough.

`for await` over a sync iterable has the same gap, through
`driveAsyncFromSyncIteratorWithDriver`. There, another thread must settle the
promise inside the sync iterator's result.

A plain `await` does not have this gap, because the reaction is registered
after the body function has returned.

Proposed fix, smallest first:

- (a) GIL-off, do not pick the fused path. In `slow_path_iterator_open` for
  async iteration (CommonSlowPaths.cpp:1014 and 1005), keep
  `IterationMode::Generic` when `vm.gilOff()`, and store the real `next`
  function, not `fastAsyncGeneratorSentinel`. The DFG and FTL choose the
  fast path from `seenModes`, so they would not emit
  `EnqueueAsyncGeneratorDriver`. GIL-off, `for await` then costs one promise
  per step.
- (b) Keep the fused path, but enqueue after the suspend. The slow path
  stores `(iterator, resumeValue)` on the driver and returns the sentinel.
  `asyncGeneratorDispatchSuspend` (JSMicrotask.cpp:831) and the async
  function's version do the enqueue when they see the sentinel, after the
  body function has returned.

### 2. High. Not in the commit. VM::syncResumeCallCache() is shared by all threads. Reproduced.

`VM::m_syncResumeCallCache` (VM.h:1543) is one `MicrotaskCallCache` for the
VM. Every request that runs a generator body on the calling thread passes it
to `callMicrotask`:

- AsyncGeneratorPrototype.cpp:124, 149, 172, 207, 240
- LLIntSlowPaths.cpp:2247
- JITCall.cpp:603
- DFGSpeculativeJIT.cpp:8757
- FTLLowerDFGToB3.cpp:21709

The cache has no lock. `find` and `nextEntryToReplace`
(MicrotaskCall.h:99, 114) read and write `m_entries` and `m_nextEntryIndex`.
`callMicrotask` (JSMicrotask.cpp:150-154) then calls `initialize` on the
entry it got. `MicrotaskCall`'s own comments assume that only one mutator
thread writes an entry.

The commit's protocol lets only one thread drive a given generator. So two
threads use the cache at once only when they drive different generators.
With one body function per generator, that happens only on a cache miss.

Interleaving, GIL off:

1. Thread A: `find(X)` returns entry E, which was set up for body X.
2. Thread B: `find(Y)` misses. `nextEntryToReplace()` returns E, and B calls
   `E.initialize(Y)`. That stores Y's executable, code block and entry
   address.
3. Thread A: `E.tryCallWithArguments(X, ...)` loads `m_codeBlock`, which is
   now Y's, and enters Y's code with X as the callee.

Two threads that miss at once can also get the same entry and both call
`relink`. That is the failure I reproduced. In
`async-generator-multithread-for-await.js`, Debug build, GIL off, it failed
in 6 of 20 runs and then in 5 of 20 runs, always with:

```
ASSERTION FAILED: !node->prev()
SentinelLinkedList<CallLinkInfoBase>::push
  CodeBlock::linkIncomingCall            CodeBlock.cpp:2477
  Interpreter::prepareForMicrotaskCall   Interpreter.cpp:1553
  MicrotaskCall::relink                  MicrotaskCall.cpp:60
  MicrotaskCall::initialize
  callMicrotask
  asyncGeneratorBodyCall                 JSMicrotask.cpp:644
  asyncGeneratorUnwrapYieldResumption
  asyncGeneratorResume
  enqueueAsyncGeneratorDriver            JSMicrotask.cpp:726
  asyncIteratorNextWithDriver
  llint_slow_path_async_iterator_next_with_driver
```

When I filled the cache on main before starting the threads, the same file
passed 40 of 40 runs on Debug and 40 of 40 on Release.

Proposed fix: GIL-off, do not use the shared cache. `callMicrotask` accepts
a null cache, and then takes the uncached path. Either

- add `MicrotaskCallCache* VM::syncResumeCallCacheIfSafe()`, which returns
  null when `m_gilOff`, and use it at the nine sites. The JIT sites take the
  pointer as a constant, so pass null when the VM is GIL-off at compile
  time. Or
- keep one cache per thread (on the VMLite) and pass that instead.

The microtask drain already uses a cache on the stack
(MicrotaskQueue.cpp:280), so it is not affected.

### 3. High. Not generator code. GIL-off, a thread reads the variables of an earlier activation. Reproduced.

A function's code is compiled while it has had one activation. Then another
thread creates a second activation. A thread that starts after that reads
the first activation's variables. The values look constant-folded, and the
code that has them still runs. Every run of this script shows it:

```js
// Each call of phase(tag) makes a new closure over tag. A thread must only
// ever read its own tag.
function phase(tag) {
    const ts = [];
    for (let t = 0; t < 3; ++t) ts.push(new Thread(() => {
        let wrong = 0, firstWrong;
        for (let i = 0; i < 300000; ++i) {
            const v = tag + t;
            if (v !== tag + t || v - t !== EXPECT[0]) {
                if (!wrong) firstWrong = v - t;
                wrong++;
            }
        }
        return [wrong, firstWrong];
    }));
    return ts.map(t => t.join());
}
var EXPECT = [0];
let badThreads = 0;
for (let round = 0; round < 6; ++round) {
    EXPECT[0] = round * 10;
    for (const [wrong, firstWrong] of phase(round * 10))
        if (wrong) { badThreads++; print("round", round, "thread read tag", firstWrong, "in", wrong, "iterations"); }
}
print("threads that read another call's tag:", badThreads);
```

Release build, `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1
JSC_useThreadGIL=0 jsc --useJSThreads=1`: in round 1, all three threads read
`tag` as 0, once each. Three runs, same result. GIL-on: 0. With
`--useDFGJIT=0`: 0. On main, with no threads, calling the closure directly,
through `Reflect.apply`, or through `Lock.prototype.hold`: 0. It still
happens when the thread's start function calls the closure with an ordinary
JS call. My guess is that it is the singleton-scope watchpoint, which fires
when `phase` runs the second time. I have not checked that.

How it hit the generator test: the first version did not prewarm the bodies.
`countingBody(s, n)` ran hot in section 1 with one activation. In section 2,
a thread that ran the new generator's body read section 1's `s`. It stored
its await resolver in section 1's object, which nobody pokes, so the body
never resumed. That was 7 hangs in 60 Release GIL-off runs. In a failing run,
section 1's object had `yields = 202` (200, plus 2 from section 2's body) and
a pending resolver. Prewarming the bodies on main (section "Test" below)
removed the hang, 0 in 60 runs. So did `--useDFGJIT=0`, 0 in 60 runs.

I also saw an ASAN SEGV inside JIT code, in 9 of 10 Debug GIL-off runs of a
scratch copy of the test. That copy reassigns a global array in each section,
and calls a closure that reads it through `Lock.prototype.hold`. With
`--useDFGJIT=0` it did not crash, 0 in 10 runs. I could not make a smaller
crash repro. I think it is the same bug, but I have not shown that.

Proposed fix: I did not find the cause. Places to look are how a new thread
picks a callee's code (`JSC::call` at ThreadObject.cpp:229, then
`Interpreter::executeCall`), and whether the watchpoint fire jettisons code
that the other threads see. LANDING-PLAN.md section 1.1 does not list this.

### 4. Low. Known. Two threads make the first generator object of a function at once.

`slow_path_create_async_generator`, then
`InternalFunctionAllocationProfile::createAllocationStructureFromBase`. The
assertion fires when two threads call an async generator function for the
first time at once. LANDING-PLAN.md section 1.1 lists it for another test.
It also fires from ordinary code, such as the `relay(g)` calls in the
for-await test. In Release, the race costs an extra structure, or a
"rotated" watchpoint fire. The for-await test makes one `relay` on main
first, so it does not hit this.

### 5. Low. The protocol comment leaves out one invariant.

See "Is every store of a settled state made under the lock?" above. Add a
sentence to JSAsyncGenerator.cpp:85.

### 6. Low, test tooling. run-tests.sh did not run JSTests/threads/semantics.

CORPUS2-INTEGRATE.md:33 says the directory joins the default corpus, but the
glob was not added. I added it. It is two lines in `Tools/threads/run-tests.sh`,
in the corpus loop and in the header comment. The other hunks in that file's
working-tree diff (`threadsRequireGILOff`) are someone else's. On Release,
every `semantics/` test passed in both modes, with two skipped by their own
headers. I did not run the whole directory on Debug.

## Test

### JSTests/threads/semantics/async-generator-multithread-requests.js

Four threads, one async generator per section.

1. `next()` only. 240 requests and 200 values. Some requests get a value,
   and the rest get `done: true`.
2. `next()`, `return()` and `throw()` mixed, on a body that does not catch.
   The first `return()` or `throw()` to reach a yield ends the body.
3. `next()` and `throw()` on a body that catches. A caught `throw()` gets the
   next yielded value.
4. One request per thread on a generator that has not started, all made at
   once. This is 30 rounds, with one `next()`, one `return()` and one
   `throw()` in each.

The bodies await a resolved value, and a promise that another thread may
resolve (`crossThreadTick`). So the body moves between threads at awaits,
and not only when the driver role changes hands.

Thread 0 sends its requests in bursts, so the queue grows long. Threads 1 to
3 wait for each of their requests to settle before they send the next one.
So a request often finds the generator idle, and the driver changes threads.
I counted these changes with a scratch copy: 5 to 59 per section on Release
GIL-off, and 54 to 78 on Debug GIL-off. GIL-on, they change only at blocking
calls, which gives 1 to 5 per section.

Checks, in any interleaving:

- Every promise settles exactly once. Each request has a slot in a shared
  `Int32Array`.
- The values delivered are exactly 0 to k-1, each once, where k is the
  number of yields the body ran.
- A request made after this thread saw the generator completed gets
  `{value: undefined, done: true}`, or a rejection for `throw()`.
- `return(v)` resolves `{value: v, done: true}`, and `throw(e)` rejects with
  `e`, or in section 3 gets the next value.
- In section 3, the number of caught throws equals the number of fulfilled
  `throw()` requests.
- In section 4, the body started at most once, and it started only if one
  `next()` got the value 0.
- The body never runs on two threads at once. `enter()` and `leave()` wrap
  each stretch of body code between suspensions, with `Atomics.add`.
- A lost request shows up as the settled count stuck below the total, and
  the test throws after 60 seconds.

Every thread, main included, drains its own microtask queue until all
requests have settled. This is because, GIL-off, a reaction runs on the
thread that settled the promise. The drain loop sleeps 1 ms when nothing
moved, so that GIL-on threads can switch. The test uses no sleep to wait for
a condition.

The bodies are prewarmed on main, because of finding 3.

### JSTests/threads/semantics/async-generator-multithread-for-await.js

Four threads each run a `for await` loop over one generator, so the fused
driver path is used. Odd threads loop over an async generator of their own,
which relays the values. So both kinds of driver are used. Checks: every
value reaches exactly one loop, every loop ends, and the body never runs on
two threads at once. Debug GIL-off, this file fails because of finding 2,
and it should pass once that is fixed.

### Results

The binaries are `WebKitBuild/{Debug,Release}/bin/jsc`, built on
2026-09-02. Debug is an ASAN build. Both include another engineer's
uncommitted changes to `Source/`. The Debug binary was relinked at 04:29
while my loops ran, so the Debug rows mix two binaries: the one from 04:02
and the one from 04:29. The last 20 runs of each Debug row are on the 04:29
binary.

GIL off is `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1
JSC_useThreadGIL=0`. Each run also has `--useJSThreads=1` and
`ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0`, as
run-tests.sh sets it.

| File | Build | GIL | Runs | Failures |
|---|---|---|---|---|
| requests | Release | on | 20 | 0 |
| requests | Release | off | 80 | 0 |
| requests | Debug | on | 40 | 0 |
| requests | Debug | off | 80 | 0 |
| for-await | Release | on | 20 | 0 |
| for-await | Release | off | 20 | 0 |
| for-await | Debug | on | 40 | 0 |
| for-await | Debug | off | 40 | 11, all finding 2 |

`Tools/threads/run-tests.sh --filter=async-generator-multithread` passes
both files in all four configurations, except for-await on Debug GIL-off,
which failed with finding 2.

A run of the requests file takes about 1 second on Debug ASAN, and about
0.1 seconds on Release.

### What I did not check

- That the requests test fails without commit 4fa55c249e62. I was not allowed
  to build. Without the commit, two threads could resume the body, which the
  overlap check catches. Or a request could be lost, which the deadline
  catches. This is from reading the code only.
- Finding 1. The for-await test did not trip it.
- The whole `semantics/` directory on Debug.
