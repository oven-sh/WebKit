# 8. One heap, N mutators

The collector and allocation. The present state: `research/R5-heap-gc.md` (`R5 2.6`, hazards `A1` to `E12` of its section 11). The earlier project's specification of the same layer for the engine with a JIT is summarized in `research/R9-prior-threads-design.md`, section 5.3 and appendix C; several protocols below are its, simplified by the absence of compiler threads and of a second mode.

The protocol of heap access, stops, waits, and the start and end of a thread is written once, here: 8.3, 8.4, 8.6, 8.14. Other documents cite it.

## 8.1 The model

- One `JSC::Heap`, the VM's. One cage, so one value of `cellCageBaseGPR` for all threads and `cageBaseOf(holder)` right for every holder. The limit of 32 GB of cells is a limit for all threads of the VM together.
- Each thread has a `MutatorContext` (a member of `ThreadContext`).
- Marking stays concurrent and parallel. Nothing on the marker's side needs to know how many mutators there are, except two uses of `m_collector.heap()`: `m_stopIfNecessaryTimer->scheduleSoon()` (`heap/SlotVisitor.cpp:893`), which is compiled out because the collector thread conducts (8.10), and `drainInParallelPassively` (`:966`), whose test of one `m_worldState` becomes "no stop is in force and some thread has `HasAccess`".
- Cells do not move. Butterfly evacuation (`Heap::evacuateSparseAuxiliaryBlocks`) refuses when more than one thread is registered; in mt builds it is compiled out, and `m_auxiliaryEvacuationIsDue` with it.

**Upstream's `Collector` for several heaps is not this.** It is N heaps with one mutator each, "once VMs can share objects", behind `useGlobalGC`, and cannot yet stop two mutators (`RELEASE_ASSERT(m_heaps.size() <= 1)`, `heap/Collector.cpp:826`). Per-heap cages do not compose with narrow references across heaps. What is taken from it: the cut between cycle state (`Collector`) and what is reached through `forEachHeap`, which is almost item
for item what must exist per thread; and `BlockDirectory`'s list of `LocalAllocator`s, atomic block claim and grow lock.

**Rule H0.** The stop of N mutators lives in and below `Heap::stopTheMutator()` / `resumeTheMutator()`. `Collector::finishChangingPhase` keeps calling those two. When upstream writes "stop the mutators of several heaps" there, the conflict is small.

## 8.2 `MutatorContext`

| Member | Replaces (R5 2.6) |
| --- | --- |
| `Atomic<uint8_t> m_access`: `NoAccess`, `HasAccess`, `Parked` | `hasAccessBit`, `stoppedBit` of `m_worldState` |
| `CurrentThreadState m_parkedState`: registers, stack top, stack origin | `m_conductingMutatorState` for one thread |
| `uint64_t m_stopCountSeen`, `m_collectionCountSeen` | `needCollectionEpilogueBit` (H12) |
| `GCClient::Heap m_clientHeap`: the iso subspaces' `LocalAllocator`s | `VM::clientHeap` |
| `ThreadSubspaceAllocators` per `CompleteSubspace`: `m_allocatorForSizeStep[]` and the allocators | the arrays inside `CompleteSubspace` |
| `MarkStackArray m_markStack` | `Heap::m_mutatorMarkStack` |
| `StoredCellLog m_storedCellLog` | `Heap::m_storedCellLog` |
| `uint64_t m_barriersExecuted` | the same in `Heap` |
| `unsigned m_deferralDepth`, `bool m_didDeferGCWork`, `unsigned m_noSafepointDepth`, `unsigned m_liveStorageRefs` | the first two: the same in `Heap`. The others are new (8.4, 8.9) |
| `MutatorState m_mutatorState` | `Heap::m_mutatorState` |
| `size_t m_bytesAllocatedSinceFlush`, extra memory since the last check | plain counters of `Heap`; flushed to atomics in slow paths |
| `Vector<..> m_replacedStringBuffers`, `m_deferredFrees`, `bool m_flushIsDue` | `m_possiblyAccessedStringsFrom..`; new (8.9) |
| `Vector<JSCell*> m_keptAlive` | `VM::m_currentWeakRefVersion` (8.11) |
| `MarkedVectorBase` set | `m_markListSet`: per thread, merged by the visiting constraint. A `MarkedVectorBase` lives in a frame, so it is added and removed by one thread |
| `StrongSet`, with a return list per block | `m_strongSet`: per thread, merged by the visiting constraint (below) |
| `void* m_lastStackTop` | `VM::m_lastStackTop` |

The view's `heap` words (`aot/runtime/AOTHeapWords.h:19-23`) point at this thread's mark stack, log and counter. So the stub that appends to the mark stack in line (`aot/stubs/AOTStubsBarriers.cpp:40-70`) and `RememberElement` (`:118-156`) stay as they are.

**`Strong<>` handles cross threads.** A handle is made by one thread and may be released by another: the tickets of `asyncJoin`, `asyncHold`, `waitAsync` (09, section 9.13), the `ThreadStart` record (09, section 9.2). `StrongSet::deallocate(slot)` finds the set through the slot's block and pushes on that set's free list without a lock (`heap/StrongSet.h:160-179`). In mt builds it compares the block's set with the running thread's own. If they differ it stores an empty value in the slot and pushes the slot on a lock-free return list, **one atomic head per `StrongBlock`**. The list belongs to the block, not to the set: a handle is released wherever its holder dies, also on a thread without heap access or on no JavaScript thread, so neither `m_threadsLock` nor a stop orders the release against the end of the thread that made it. Blocks survive that end: T4 gives them to the heap's set (8.14). A set drains the lists of its blocks in `allocateSlow()`; the End phase drains those of the heap's set and frees blocks that are empty. `Strong::set()` on another thread's handle is one store of a value; the set keeps no second list by kind of value.

## 8.3 Heap access

A thread touches the heap (reads or writes a cell, holds a reference in a register that the collector must see) only while `m_access == HasAccess`.

### State

```
Heap     Atomic<uint32_t> m_stopRequested                        0 or 1; written by the conductor
         Atomic<uint64_t> m_stopCount, m_collectionCount         written by the conductor, while stopped (H12)
         Lock             m_stopLock                             leaf (8.15)
         Condition        m_stopCondition                        the conductor sleeps here
         Condition        m_conductorCondition                   who wants to conduct sleeps here
         bool             m_hasConductor                         under m_stopLock
         Deque<PendingOperation*> m_pendingOperations            under m_stopLock (8.6)
VM       Lock             m_threadsLock                          parking (8.15); list m_threads
         Lock             m_threadTableLock                      leaf; m_threadTable[id], free and quarantined IDs (8.14)
```

`Heap::m_conductorLock` is the pair (`m_hasConductor`, `m_conductorCondition`) under `m_stopLock`. It is no `WTF::Lock`, because a thread that waits for it must also be able to leave when somebody else has done its work (8.6).

### The primitives

```
capture():            callee-saved registers, stack top and stack origin into m_parkedState

notifyConductor():    { Locker l(heap.m_stopLock); }  heap.m_stopCondition.notifyAll()

releaseAccess():      ASSERT(!m_noSafepointDepth && !m_liveStorageRefs)                         // S4, S1'
                      capture()
                      m_access.store(NoAccess, seq_cst)
                      if (heap.m_stopRequested.load(seq_cst)) notifyConductor()

becomeParked():       as releaseAccess(), with Parked in place of NoAccess

acquireAccess():      ASSERT(!m_noSafepointDepth)                                               // no leaf lock is held: below
                      for (;;) {
                          m_access.store(HasAccess, seq_cst)
                          if (!heap.m_stopRequested.load(seq_cst)) break
                          m_access.store(NoAccess, seq_cst)
                          notifyConductor()
                          ParkingLot::compareAndPark(&heap.m_stopRequested, 1)                  // returns at once if it is 0 by now
                      }
                      if (m_stopCountSeen != heap.m_stopCount.load(acquire)) didResume()        // H12

park():               becomeParked()                                                            // only at a safepoint (8.4)
                      ParkingLot::compareAndPark(&heap.m_stopRequested, 1)
                      acquireAccess()

stopIfNecessary():    if (m_noSafepointDepth) return
                      if (heap.m_stopRequested.load(relaxed)) park()
                      if (m_flushIsDue) { m_flushIsDue = false; heap.runSafepointOperation(nothing) }   // 8.9

lockParking(lock):    if (lock.tryLock()) return                                                // H15, 8.15
                      releaseAccess(); lock.lock(); acquireAccess()
```

**Why no stop is missed.** The pair (store own state, load the request) against (store the request, load each state: `requestStop()`, 8.4) is a Dekker pair. All four accesses are sequentially consistent. (#249: "acq/rel UNSOUND".) If a thread's load of the request returns 0, that load precedes the conductor's store of 1 in the single total order, so the thread's store of its state does too, and the conductor reads it.

**Why no wake-up is lost.** The conductor reads all `m_access` words and goes to sleep in one critical section of `m_stopLock` (8.4). A thread stores its state first and passes through `m_stopLock` afterwards. So either the conductor's read sees the new state, or the thread gets the lock only once the conductor is inside `wait()`, and its notification finds a sleeper. A thread that waits for the end of a stop uses `compareAndPark`, which compares and enqueues under the queue's lock; `cancelStop()` stores 0 and then calls `unparkAll`.

**Heap access changes in neither direction inside a `NoSafepointScope`.** A thread without access may take a leaf lock: `notifyConductor()`, `post` and `wake` from a loop that waits, the clearing of an edge of the wait-for graph after a wake-up. That is harmless while the holder runs. If it asked for access with the lock held, it would sleep through a stop with the lock, and the conductor, which takes leaf locks during a stop (it posts to inboxes: 8.11), would wait for it. A leaf lock opens its scope whether or not the thread has access, so the assertion of `acquireAccess()` finds this. The places that sleep or take things out under a leaf lock and go on with access afterwards release the lock first:

| Place | Order |
| --- | --- |
| `join`, `Condition.wait`, `Atomics.wait`, a wait of 09, W1 | the `ReleaseHeapAccessScope` encloses the `Locker` of the condition's or list's lock, not the other way round |
| waiting for a collection (8.10), `lockConductor()` (8.6) | `Collector::m_threadLock`, `m_stopLock` are released before `acquireAccess()` |
| a loop that takes tasks out of its inbox (09, section 9.7) | it takes access first and the inbox lock second; or it swaps the vector out under the lock, unlocks, and takes access before it looks at a task |
| T3, step 8; `lockParking(m_threadsLock)` | `m_threadsLock` is a parking lock and opens no scope by itself. `instancesLock` and `m_threadTableLock` are released before |

**`m_parkedState` is valid** from the store of `NoAccess` or `Parked` until `acquireAccess()` returns. Until then the thread pops no frame above the captured stack top. The transient `HasAccess` in the loop of `acquireAccess()` means nothing: the thread touches no cell before the loop has ended. Nobody but the waiting loop of `requestStop()` decides anything by the value of another thread's `m_access`.

**Rule H12 (stop count).** The conductor increments `m_stopCount` at the end of every stop, and `m_collectionCount` at the end of every End phase, before `cancelStop()` lets anybody go. `acquireAccess()`, and with it `park()`, ends with the comparison. So the first thing that a thread does with heap access after any stop, whether it was parked during the stop or had no access and never noticed it, is `didResume()`.

`didResume()`: store the new `m_stopCountSeen`; compare the cache epoch (07, E2); if `m_collectionCountSeen` differs, clear the per-thread caches that the collection epilogue clears today (8.12) and store the new value. It takes no lock. It reloads nothing else, since compiled code treats the operation it parked in as `collects`.

The commonest state of a thread is `NoAccess`: it sleeps in its event loop. A timer that wakes it is no synchronizing operation. Without H12 it would go on with caches that name StructureIDs, offsets and strings which a collection has recycled meanwhile.

**Rule H1.** Every primitive that can block for an unbounded time is bracketed by `ReleaseHeapAccessScope` (`releaseAccess()`, `acquireAccess()`): `join`, a contended `Lock`, `Condition.wait`, `Atomics.wait`, the event loop's wait, blocking system calls of natives marked `blocks`, every wait that the wait-for graph covers (a module claim, a claim of 07, Z2: 09, W1), every wait for a parking lock (`lockParking`), waiting to become conductor, waiting for a collection. Inside the scope the thread touches no cell, and it uses the bytes of a buffer only under a pin (H11). Each such wait is a safepoint (8.4).

Bun takes access once per VM at creation and keeps it (`bun:src/jsc/bindings/ZigGlobalObject.cpp:607`), releasing it around the loop's wait only while an idle collection is pending. In mt builds the loop's wait always releases.

## 8.4 Safepoints

**Rule S2** (04): a thread parks only at an IR operation whose effects say `collects`, or, in C++, in `Heap::stopIfNecessary()` or in a wait of H1.

| Safepoint | Today | mt |
| --- | --- | --- |
| Allocation slow path (`collectIfNecessaryOrDefer`) | polls `m_worldState` | `stopIfNecessary()` |
| Loop poll | handles VM traps only | `operationAOTPollSlow`: traps, then `handleSafepointRequests()` |
| Prologue poll of a function with `EntryIsSafepoint` (04, section 4.11) | | the same operation |
| Other prologue polls | `DeferGCForAWhile`: not a safepoint | unchanged. The thread goes on to its next real safepoint |
| Between two jobs | none: `MicrotaskQueue::drainImpl` is `while (!m_queue.isEmpty())` without a poll (`runtime/MicrotaskQueue.cpp:182-253`) | `drainImpl` calls `handleSafepointRequests()` after each job. The job loop of the adapter `EnterSavedFrame` leaves for C++ when the trap-aware limit is the marker, as today. (A job that queues itself again is a cycle that neither a control flow graph nor the call graph shows: 04, S3) |
| `DeferGC` scope exit with deferred work; `reportExtraMemoryAllocated` past a threshold; explicit `stopIfNecessary()` | | unchanged |
| Entering and leaving the VM; `acquireAccess()`, `releaseAccess()`, `lockParking()`, `runSafepointOperation()`, waiting for a collection | | a stop may begin and end while the thread is inside |

**Not safepoints**: anything inside a `NoSafepointScope`. Leaf locks (8.15: object locks, table locks), `DeferGC` and `DeferGCForAWhile` open one. With N threads "no collection can happen here" must also mean "this thread does not park here and does not give up access here", else another thread's collection or safepoint operation runs inside the scope (R5 D9). Such a scope is also how the run time keeps the promise of an IR operation that is **not** declared `collects` (`CreatesConstant`, `AllocatesWithoutCollecting`, `RuntimeCreatesConstant`, `RuntimeCreatesField`: `aot/ir/AOTSchema.def:83-85`, `aot/ir/schema/runtime-operations.def:1-5`): `Storage` values are live across such an operation, barriers were removed over it, narrow references are in registers.

**Rule S4.** A thread inside a `NoSafepointScope` never waits for a claim, for a parking lock, for `m_conductorLock`, for a collection, or for anything else of H1, and it does not spin for them either. (Spinning deadlocks: the owner of a claim may park for a stop, the stop waits for the spinner, the spinner for the owner.) Code that finds that it would have to wait undoes nothing that others can see, and returns `MustRetryAtSafepoint` to the frame that opened the outermost scope. That frame closes the scope, waits, and starts again from the generic dispatch. It may do so only if its own caller may park: in compiled code, if the IR operation is declared `waits` (04, S5). The operations that are not so declared never get there, because what they would wait for is settled before the second thread exists: made, or recorded as unavailable (`Instance::prepareForThreads()`: 07, section 7.6; 04, S5). `releaseAccess()`, `becomeParked()` and `lockParking()` assert a depth of zero; a validating build fails with `OperationParked` or `OperationWaited` (04, section 4.14).

What S4 means for the rules that ask for a wait or a stop:

| Rule | Reached inside a scope |
| --- | --- |
| 07, Z2: a loser waits for the claimant | `MustRetryAtSafepoint` |
| 09, MD3'': a module that another thread owns | the same |
| 06, section 6.4; 05, section 5.6: storage in state `Shared` must be replaced | decided before the first store that a lock-free reader can see (05, L6); the locks are released and the whole mutation becomes the closure of a safepoint operation; inside somebody else's scope, `MustRetryAtSafepoint` |
| `flattenDictionaryStructure` as an optimization (cache fills, enumerator creation) | skipped: the caller goes on with the dictionary |
| 8.9: the deferred bytes cross the threshold | sets `m_flushIsDue`; the flush runs in the thread's next `stopIfNecessary()` |

**Why a running thread is never suspended by a signal** (R5 8.4): between the bump of a free list and the store of the header a cell holds old bytes; `stopAllocating()` rewrites free lists; barrier removal, the cached barrier threshold, narrow references in registers and values in scratch words all assume cooperative stops.

### How a stop is requested

The caller is the conductor: it holds `m_conductorLock` (8.6, 8.10), and it is `Parked` or is the collector thread.

```
requestStop():        vm.m_threadsLock.lock()                                                   // H3
                      m_stopRequested.store(1, seq_cst)
                      for (T : vm.m_threads) if (T != me) T.traps.setStopForHeap()              // arms T's polls, below
                      { Locker l(m_stopLock)
                        while (some T != me in vm.m_threads has T.m_access.load(seq_cst) == HasAccess)
                            m_stopCondition.wait(m_stopLock) }                                  // with the watchdog of 8.5

cancelStop():         m_stopCount.store(m_stopCount + 1, release)                               // H12
                      m_mutatorDidRun = true
                      for (T : vm.m_threads) if (T != me) T.traps.clearStopForHeap()
                      m_stopRequested.store(0, seq_cst)
                      ParkingLot::unparkAll(&m_stopRequested)
                      vm.m_threadsLock.unlock()
```

**Rule H3 (the threads lock spans the stop).** The conductor takes `VM::m_threadsLock` **before** it stores `m_stopRequested` and keeps it until after `cancelStop()` has stored 0. Three things follow.
1. While any other thread holds `m_threadsLock`, no stop is requested and none can begin. So `lockParking(m_threadsLock)` cannot deadlock: the `acquireAccess()` at its end, made with the lock held, finds the request at 0.
2. A thread is added to or removed from `m_threads` only under the lock, together with all its parts (8.14). So a stop sees a thread with all of its parts or not at all, and no part appears or vanishes during a stop.
3. Code that runs during a stop (phases of the collector, closures, the constraints that visit every thread's queue, inbox, registry) uses `m_threads` without taking the lock again.

A JavaScript thread other than the conductor holds the lock only for short stretches that allocate nothing in the heap and wait for nothing: T3, T4 (8.14), the fan-out of a termination (09, section 9.10), attaching the profiler.

`m_mutatorDidRun` is today stored by the mutator after each resume (`heap/Heap.cpp:2095,2151,2189`), and `stopThePeriphery()` derives from it whether `m_mutatorExecutionVersion` moves, that is, whether the constraints that are greyed by execution ("Cs", "Msr", "At") must run again (`:1987-1990`). In mt builds the conductor stores it.

### The request is a bit that belongs to the conductor, and is no event

The trap word stays the one way to make compiled code leave its fast path. Each thread has its own `VMTraps`, so no thread consumes another's bit (`research/R6-calls-frames-async.md`, hazard on traps). The stop marker in the `StackManager` and its mirror, and the protection of the poll page, have several writers: every handled trap ends in `updateThreadStopRequestIfNeeded()`, which calls `StackManager::cancelStop()` when no bit is left (`runtime/VMTraps.cpp:425-443,484`), and the owed `RangeError` writes the same word (`runtime/StackManager.cpp:73-83`).

A request that can be answered only where a thread may park must not be an *event*. Events are looked at all over the run time: `RETURN_IF_EXCEPTION` is `if (traps().maybeNeedHandling()) if (vm.hasExceptionsAfterHandlingTraps()) return`, which calls `handleTraps(NonDebuggerAsyncEvents)` (`runtime/ExceptionScope.h:131-138`, `runtime/VM.cpp:1300-1305`), and `handleTraps` loops `while (needHandling(mask))` (`runtime/VMTraps.cpp:488-528`). As an event the request would make each of the several thousand `RETURN_IF_EXCEPTION` a safepoint, and inside a `NoSafepointScope`, where the thread cannot park and the bit stays, the loop would never end.

**Safepoint requests.** Three bits of `m_trapBits` above the events:

```
FOR_EACH_VMTRAPS_EVENTS      unchanged (runtime/VMTraps.h:150-156). So NumberOfEvents, AllEvents, AsyncEvents, NonDebuggerEvents,
                             NonDebuggerAsyncEvents (:183-186) are unchanged, and none of them contains a safepoint request
NeedStopForHeap              1 << NumberOfEvents            a stop (this section)
NeedExit                     1 << (NumberOfEvents + 1)      09, X1
NeedYield                    1 << (NumberOfEvents + 2)      serial mode (8.7)
SafepointRequests            the three
maybeNeedHandling()          m_trapBits.loadRelaxed() & AllEvents                                (today: any bit, :209)
handleSafepointRequests()    if (m_noSafepointDepth) return; heap.stopIfNecessary(); then NeedExit (09, X1); then NeedYield (8.7). It does not loop
```

| # | Rule |
| --- | --- |
| 1 | `NeedStopForHeap` is set (`setStopForHeap`) and cleared (`clearStopForHeap`) under the thread's `m_trapSignalingLock`, **by the conductor only**. Nobody takes it |
| 2 | `handleTraps`, `takeTopPriorityTrap`, `needHandling(mask)` with any of the masks above, `maybeNeedHandling()` and with it `RETURN_IF_EXCEPTION` never see a safepoint request. While a stop is pending `RETURN_IF_EXCEPTION` stays on its fast path. It is no safepoint, so the safepoints that C++ has to reckon with stay "allocation, call of JavaScript, blocking" (11, N-d; 06, A3) |
| 3 | The callers that are safepoints ask for themselves, behind `handleTraps(AsyncEvents)`: `operationAOTPollSlow`, the interpreter's `slow_path_handle_traps` (`llint/LLIntSlowPaths.cpp:2620-2627`), the job loop (table above) call `handleSafepointRequests()`. A blocking primitive has no heap access, so a stop does not concern it; at each wake-up it looks at `NeedExit` and at the termination event (09, section 9.13). The interpreter's `checkTraps` tests `AsyncEvents \| SafepointRequests` (`llint/LowLevelInterpreter.asm:2516-2521`): the same three instructions. `operationAOTPollAtPrologue`, which is no safepoint, does not ask (`aot/runtime/AOTExceptionOperations.cpp:60-72`) |
| 4 | The marker, the mirror and the poll page (on Linux x86-64 `mprotect(pollPage, PROT_NONE)`) are written by `updateThreadStopRequestIfNeeded()` alone, whose `shouldStop` is `needHandling(AsyncEvents \| SafepointRequests)` (with today's addition for a pending termination). Neither `Heap` nor a handler calls `StackManager::requestStop()` or `cancelStop()` directly |
| 5 | `handleTraps` takes the lock of the code block set and walks all code blocks at each call, for breakpoints that the JIT installs (`runtime/VMTraps.cpp:459-467`). An mt build generates no code at run time (03, P3): the walk is compiled out |

So a prologue poll, or a watchdog trap that is handled while a stop is pending, leaves the polls armed, and the end of a stop leaves a pending termination armed.

## 8.5 Time to safepoint

A stop takes as long as the slowest thread needs to reach a safepoint.

| Source of delay | Bound |
| --- | --- |
| Compiled loops, recursion | S3, S3' of 04: a poll in every cycle of a control flow graph and of the final call graph. The bound is the longest path without a cycle, which only generated code makes long |
| Function entries while a stop is pending | each entry of a thread that has not parked yet fails its stack check and takes the C++ slow path. A constant per call |
| Stubs | straight-line or bounded by a small constant (element-wise copies up to `maxLengthForElementwiseCopy`, `aot/stubs/AOTStubHelpersStorage.cpp:209`) |
| C++ that runs long without allocating: `memcpy`, `memmove` of large arrays, `sort` of numbers, `indexOf`, `JSON.stringify` of a huge string, hashing, `TextDecoder` | by data size. Functions that are linear in a size above 1 MB poll every 64 KB of work: `heap.stopIfNecessary()` in the loop, after which rule A3 of 06 applies (load storage again). List: W-8.6, first half |
| Growth and conversion of storage by its owner (06, sections 6.3.5, 6.3.6) | the copy is made **before** the retirement. A copy of more than 1 MB polls every 64 KB and starts again after a poll that parked. Between the retirement and the publication, for which other writers wait (8.15), lie a handful of instructions |
| C++ that calls compiled code once per element (`sort` with a comparator, the C++ forms of `forEach`, `replace` with a function) | the callee need not contain a safepoint. The loop calls `heap.stopIfNecessary()` once per 1,024 calls, and then loads storage again (06, A3) |
| RegExp: code in the image, the flat matcher, the bytecode interpreter | a poll every 2^16 backtracks. Image code calls a stub that saves the matcher's registers. The subject string's characters are immutable, so nothing has to be reloaded |
| `NoSafepointScope` under a leaf lock | short: a debug build logs scopes over 1 ms |
| `NoSafepointScope` from `DeferGC`, `DeferGCForAWhile` | **not all are short.** `ScriptExecutable::prepareForExecutionImpl` holds one over parsing, bytecode generation and linking (`runtime/ScriptExecutable.cpp:405-408`): linear in the text given to `eval` or `new Function`, and with 10, I1 once per thread. `ErrorInstance::computeErrorInfo` holds one over the embedder's source map hook (`runtime/ErrorInstance.cpp:495-515`). List: W-8.6, second half, each with the way it is split (parse and generate outside, link inside) |
| Natives that block | H1 |
| A thread that the OS does not schedule | not bounded |

**Cost of a stop.** #249 measured 0.04 ms per running thread and stop. On Linux x86-64 a stop here adds two `mprotect` calls per thread, each with a TLB shootdown, and a concurrent collection has several stops. To be measured at stage 3; the fallback is the compare-and-branch poll of the other platforms.

`BUN_JSC_stopWatchdogMs` (default 0 in release, 30,000 in debug builds): a stop that does not complete dumps every thread's state and aborts.

## 8.6 Safepoint operations

```cpp
template<typename Functor> bool Heap::runSafepointOperation(const Functor&);      // heap/threads/SafepointOperation.h
```

Runs the closure while every JavaScript thread but the one that runs it is `Parked` or `NoAccess`. Returns what the closure returns: `false` means "what I was prepared from has changed; start again".

```
lockConductor(op):    Locker l(m_stopLock)                                                      // the caller is Parked, or is the collector thread (op == null)
                      while (m_hasConductor && !(op && op->done)) m_conductorCondition.wait(m_stopLock)
                      if (op && op->done) return false
                      m_hasConductor = true; return true

unlockConductor():    { Locker l(m_stopLock); m_hasConductor = false }  m_conductorCondition.notifyAll()

runSafepointOperation(closure):
                      ASSERT(!m_noSafepointDepth && !m_liveStorageRefs)                         // point 1
                      PendingOperation op { closure, done = false }
                      { Locker l(m_stopLock); m_pendingOperations.append(&op) }
                      becomeParked()
                      if (lockConductor(&op)) {
                          requestStop()
                          for (;;) {
                              { Locker l(m_stopLock); batch = take all of m_pendingOperations }
                              if (batch.isEmpty()) break
                              { NoSafepointScope scope; for (p : batch) p->result = p->closure() }
                              { Locker l(m_stopLock); for (p : batch) p->done = true }
                          }
                          releaseDeferredFrees()                                                // 8.9
                          cancelStop()
                          unlockConductor()
                      }
                      acquireAccess()                                                           // waits for the end of whatever stop is in force; H12
                      return op.result
```

1. **The caller is at a safepoint.** It holds no leaf lock, is in no `NoSafepointScope`, has no `window` value and no `StorageRef` (S4, S1'). In compiled code the IR operation is declared `waits` (04, S5); the empty operation of 8.9 runs at any safepoint. The caller may hold parking locks and claims.
2. **While it waits it is `Parked`**, registers captured: another conductor may run a collection, or this very closure.
3. **Concurrent requests coalesce into one stop.** The JavaScript thread that becomes conductor runs all pending closures. A requester whose closure has been run leaves the queue for the lock. An operation that is appended after the conductor's last look finds `m_hasConductor` set, sleeps, and is woken by `unlockConductor()`: the test and the sleep are one critical section of `m_stopLock`.
4. **A closure runs beside markers.** `m_conductorLock` is free during the Concurrent phase, and a stop stops mutators only. Towards the markers a closure is a mutator: it keeps every step that today's code takes against them (nuke of the StructureID, fences, the cell lock, `Structure::m_lock`, the owner barrier). A marker holds the cell lock of an array with ArrayStorage or with an elements start offset (`runtime/JSObjectVisitInlines.h:355-369`), of a `CompactTable`, of a sparse map (`runtime/SparseArrayValueMap.cpp:243`), `Structure::m_lock` (`runtime/StructureInlines.h:642-644`), and the lock of a `JSModuleLoader` (10, section 10.3); `flattenDictionaryStructure` takes two of these and nukes because of the marker (`runtime/Structure.cpp:1799-1825`), and goes on doing so. What a closure never waits for is a lock that a *mutator* holds: by L1 (05) and S4 no parked thread holds a leaf lock, a thread without access that holds one is running and does not sleep with it (8.3), and a closure takes no parking lock.
5. **Closures run on JavaScript threads only.** The collector thread has no allocators, no mark stack and no thread ID. When it conducts it leaves `m_pendingOperations` alone; the requesters are `Parked`, hinder nothing, and compete for the lock when the collector's stop is over. A closure that another JavaScript thread runs uses that thread's allocators and mark stack. It does not refer to "the current thread": what it needs of its requester (thread ID for `Owned`, prepared storage) it captures by value.
6. **A closure validates everything it was prepared from, by contents**: it loads the storage afresh and compares state, kind, `vectorLength` (against the capacity that it brought) and the StructureID with what its requester read (06, section 6.3.7). Not the pointer's identity: the requester holds no storage pointer across its wait (S1'), and storage that was freed and allocated again at the same address must not pass for the old one. While the requester waited, another closure of the same batch may have grown the array further, changed its kind, made it ArrayStorage, frozen or restricted it. If anything differs the closure changes nothing and returns `false`.
7. **A closure runs inside a `NoSafepointScope` of the conducting thread.** During the operation `m_stopRequested` is 1, and the conductor runs ordinary C++: without the scope the first `stopIfNecessary()` on its way (an allocation slow path, `reportExtraMemoryAllocated` past its threshold, the exit of a `DeferGC` scope with deferred work) would park it for its own stop, which only it can cancel. With the scope S4 holds for the closure: it waits for nothing but leaf locks, runs no JavaScript, does not block.
8. **A closure allocates nothing in the heap.** What it needs its requester has allocated before, at a safepoint, and holds on its parked stack (06, sections 6.3.7, 6.4; 05, sections 5.6, 5.10). That holds for code that allocates in the middle today, such as the smaller butterfly of `flattenDictionaryStructure` (`shiftButterflyAfterFlattening`, `runtime/JSObject.cpp:4464-4494`): its size follows from what the requester reads and the closure validates. Nor does a closure open a `DeferGC` scope of its own. The one exception is `haveABadTime` in both its forms, whose need (new storage for each array that it finds) is known only from a walk of the heap while all threads stand still: walk and conversions are one closure, which allocates under the scope, hence without collecting and without parking (05, L2).
9. A closure may read and write any cell.

The collector's stop-the-world phases take the same lock (8.10). So the stop of a collection and a safepoint operation never overlap. A safepoint operation does not set `m_worldIsStopped`, which switches visitors to unfenced visits (`visitor.mutatorsAreStopped()`): only collections set it. `assertIsMutatorOrMutatorIsStopped()` accepts the conductor.

**Who uses it.** This list is closed: an addition needs the argument that the event is rare per process or per long-lived object, not per operation.

| User | Frequency |
| --- | --- |
| Replacing storage in state `Shared` (growth, change of kind, freezing in place: 06, section 6.3.7). Copy-on-write to writable and the first storage of an object are no users: they run under the object lock (06, sections 6.3.8, 6.3.9) | logarithmic in the size of an array that several threads write |
| A mutation of 05, section 5.6, or an added property of 06, section 6.4, whose side object or object has out-of-line storage that must be replaced and is not `Owned` by the mutating thread: the whole mutation is the closure (05, L6) | logarithmic in the number of properties that several threads add to one object |
| The conversion of a class prototype's stand-in **together with the definition of all its members** (05, section 5.10). Everything that allocates is made before | once per class |
| `flattenDictionaryStructure` (06, section 6.4) | at most once per object |
| `haveABadTime(vm)`. Its closure rewrites the array headers in the canonical view and in every listed view (07, V9) | once per realm |
| `haveABadTime(vm, interceptor)` (`runtime/JSGlobalObject.cpp:3139-3174`), which is made for the main realm of a compiled process: it walks the heap for the objects that have the interceptor in their chain and converts their storage. The caller holds nothing: it runs behind the change of the chain, after the prototype-chain lock and the object lock are released (06, section 6.3.11) | once per object that begins to intercept indexed accesses: a prototype that gets an indexed accessor, a chain that gets a `Proxy` (`runtime/JSObject.cpp:1065,1988-1991`) |
| Heap walks: `HeapIterationScope`, statistics (`willStartIterating()` calls `stopAllocating()` from a running mutator today, R5 A12). Heap snapshots: inside a `PreventCollectionScope` (H13) | tools |
| Flushing deferred frees when they exceed a threshold (8.9) | by volume |
| `VM::whenIdle` callbacks (H16) | rare API |
| `Thread.restrict` on an array or ordinary object (06, section 6.4) | per call of a rare API |
| Attaching the sampling profiler | tools |
| The last stop of a process (8.14) | once |

## 8.7 Serial mode

`BUN_THREADS_SERIAL=1`. One process-wide baton (`WTF::Lock`, a parking lock without a rank: it is taken and given up only inside the primitives of 8.3). `acquireAccess()` takes it before its loop, while the thread is still `NoAccess` or `Parked`, and gives it up before it sleeps in the loop. `releaseAccess()` and `becomeParked()` give it up after the store of the state. A conductor runs closures without the baton: nobody else has access. A timer sets the safepoint request `NeedYield` of the holder each millisecond (8.4); `handleSafepointRequests()` answers it with `releaseAccess(); acquireAccess()`, so the baton rotates. All code, all layouts and all protocols are the same; only one thread at a time has heap access. A bug that disappears in this mode is a race.

## 8.8 Allocation

### Fast path

Unchanged: load, compare, add on `LocalAllocator::m_freeList` (`jit/AssemblyHelpers.cpp:1070-1124`), not atomic, because the allocator belongs to the thread. Every route to an allocator starts at `instanceGPR` (R5 3.2), and the words on the way are in private regions of the view:

| Word | mt |
| --- | --- |
| `prefix.arrayAllocator`, `ropeStringAllocator`, `constants.stringAllocator`, `closures.slimClosureAllocators[]`, `coroutines.asyncGeneratorAllocator`, `allocation.inlineAllocations[]`, `typedArrayAllocations[]` | the thread's allocator for that space |
| `allocation.cellSpace`, `auxiliarySpace`, `cagedAuxiliarySpace`, `primitiveAuxiliarySpace` (`CompleteSubspace*`, which code indexes at `m_allocatorForSizeStep`) | point at the thread's `ThreadSubspaceAllocators`, which has the array at the same offset |
| `AllocationSiteRecord::allocator` | per-thread records (07, section 7.4) |

Loads of these words that are declared immutable stay right.

### C++

`allocateCell<T>(vm)`, `tryAllocateCell`, `Butterfly::create..` go `vm.xxxSpace()`. In mt builds `VM::clientHeap` is an accessor for the current thread's `GCClient::Heap`, and `CompleteSubspace::allocatorFor(size)` looks in the thread's table.

### Slow path (`LocalAllocator::allocateSlowCase`, R5 3.5)

| Step | mt |
| --- | --- |
| `didAllocate`: accounting per consumed free list | atomic adds on `Heap`'s counters. `m_hasAllocatedSinceLastCollection`: a relaxed store of `true`, read while stopped. `m_bytesAbandonedSinceLastFullCollect`, `m_deprecatedExtraMemorySize`, `m_lastOversizedAllocationThisCycle`: relaxed atomics |
| `m_edenActivityCallback->didAllocate()` (`heap/Heap.cpp:2694-2695`), which arms a timer of the VM's run loop | the timers live on the main loop (11, section 11.4). A spawned thread posts "allocation happened" to the main thread's inbox, at most once per cycle (an atomic flag that the End phase clears) |
| `didConsumeFreeList`: block bits | atomic already (847b1d1f5e7f) |
| `tryAllocateInLargerCell`: `m_numberOfLargerCells` | relaxed atomic |
| `AllocatingScope` (`RELEASE_ASSERT(m_mutatorState == Running)`) | per thread |
| `collectIfNecessaryOrDefer` | the safepoint; 8.10 |
| `findBlockForAllocation`: scan, claim `inUse`, recheck | safe already. In a thread-affine directory the recheck under the claim includes the block's owner (8.11) |
| `block->sweep(&m_freeList)`: lazy sweep, destructors | 8.11 |
| steal an empty block (`heap/LocalAllocator.cpp:260-275`) | covered by 847b1d1f5e7f (to be verified, R5 13). Never from a thread-affine directory (H6) |
| lower-tier precise cells of an iso subspace: `m_lowerTierPreciseFreeList`, `m_remainingLowerTierPreciseCount` (`heap/IsoSubspace.cpp:81-113`) | `MarkedSpace::m_lock`. None in thread-affine subspaces (H6) |
| `tryAllocateBlock`, `addBlock`, `MarkedSpace::didAddBlock` (`m_blocks`, `m_capacity`), `didAllocateInBlock` (`m_newActiveWeakSets`) | `MarkedSpace::m_lock`, a new leaf lock |
| precise allocations: `m_preciseAllocations`, the set, `indexInSpace` | the same lock |
| `reallocatePreciseAllocationNonVirtual` | only for `Confined` storage (06, SW8) |
| lazily made subspaces (`Heap::name##Slow()`) | `Heap::m_lock`, look again |
| cage: `CagedAlignedMemoryAllocator` counters, `m_residentBlocks`, `MarkedBlockCage::m_reserve` | the cage's lock |

"Growth only happens on the mutator, so the mutator .. may read the bits without `mutate()`" (`heap/BlockDirectory.h:243-259`) no longer holds: a mutator takes `mutate()` (shared) as any other reader.

### A thread's allocators

Made on demand, with heap access, appended to their directory's list under `m_localAllocatorsLock`. `stopAllocating`, `resumeAllocating`, `prepareForAllocation` iterate over all allocators of a directory **without that lock** (`heap/BlockDirectory.cpp:232-305`). That is right because they run during a stop: every thread that could append is parked or without access, no thread is between the load and the add of the fast path, and an ending thread removes its allocators under `m_threadsLock`, which the conductor holds (H3, T4).

### Size of the nursery

`m_maxEdenSize` is scaled by the number of threads that consumed a free list in the last cycle. (#249: one nursery budget for k threads made collections k times too frequent.)

### Out of memory

The cage is full (R5 4.5). In order:

1. `tryCollectForExhaustedCage()`: a synchronous full collection from an allocation slow path, unless the thread is in a `NoSafepointScope`. The requester waits as `Parked` (8.10). The call is `collectNow(Sync, Full)`, which afterwards runs `sweepSynchronously()` on the requester (`heap/Heap.cpp:1930-1945`): `sweepBlocks()` over every directory, then `shrink()`. That sweep runs beside running mutators, under the `inUse` claims, and skips other threads' affine blocks (H7).
2. `tryUseReserveOfExhaustedCage()` frees three quarters of the reserve. The owed `RangeError` (`setNeedsOutOfMemoryError`) is a flag of the **thread** whose allocation failed.
3. The reserve is the heap's, not the thread's: while one thread unwinds with its error the others go on allocating from it. Each thread whose allocation then finds the cage full owes its own error and takes from what is left.
4. The last quarter, with `willUseLastReserve(vm)`. The handler gets the engine's `VM&`, not the running thread's embedder state, and ends the VM as a whole (11).
5. Nothing is left: the process ends with a message. This is the one place where the engine ends a process that has done nothing but allocate (02, M11 names the exception).

## 8.9 Memory that is freed late

Some memory must not be unmapped or reused while a thread may hold a pointer to it inside a window: replaced index vectors of property tables, `WeakMap` buffers, `ProgramSymbols` blocks, contents of detached or shrunk ArrayBuffers, sparse map tables, compiled forms of a `RegExp` that `deleteCode` drops (H16).

```cpp
void Heap::freeAfterSafepoint(void* memory, size_t size, FreeFunction);      // appends to the thread's m_deferredFrees
```

`releaseDeferredFrees()` empties the lists of all threads at the end of each stop, on the conductor, after all closures and before `cancelStop()`. At that moment every thread is at a safepoint or has no access. By S1 and S1' none holds such a pointer, with two exceptions. Bytes that a native uses without access are pinned and never get on a list (H11). A compiled form of a `RegExp` whose cell some thread has recorded in `m_executingRegExp`, because its matcher has parked at a poll, stays on the list for this time (H16).

If the deferred bytes of the process exceed `max(16 MB, 1/16 of the heap)`, the thread that crosses the line sets its `m_flushIsDue`. It may be under the lock of the table whose vector it has just replaced, so it cannot stop anybody there (S4). Its next `stopIfNecessary()` runs an empty safepoint operation.

**Who appends where.** A thread appends to its own `m_deferredFrees` only while it has heap access. For a thread without access the conductor counts it as stopped and empties its list: two writers on one vector. So every other caller (a JavaScript thread that unpins before it has taken access back, a thread of Bun's work pool, which has no `MutatorContext`) appends to `Heap::m_deferredFreesOfOthers`, under the leaf lock `Heap::m_deferredFreesLock`, which `releaseDeferredFrees()` takes too. An entry that is on a list at the end of a stop was put there by somebody who no longer uses the memory, and every JavaScript thread is at a safepoint or without access: it may be freed.

No epochs, no hazard pointers, no reference counts.

**Rule S1' (S1 in C++).** The verifier checks S1 for IR values (04, section 4.6.3). In C++ a pointer to replaceable storage of an object that may be shared is held in a `StorageRef<T>`: the constructor increments `m_liveStorageRefs`, the destructor decrements it. `releaseAccess()`, `becomeParked()` and `stopIfNecessary()` assert that the count is zero, in debug and in validating builds (`StorageRefAcrossSafepoint`). Existing code holds storage over safepoints as a matter of course, for example both butterflies, lengths and indexing types over `auxiliarySpace().allocate()` in `runtime/ArrayPrototype.cpp:1612-1668`: such code loads again (06, A3).
A pointer that has to outlive a safepoint is a `RevalidatedRef<T>`. Its `get(test)` takes the test that makes it valid in this window and yields a `StorageRef`. The model is `runtime/JSGenericTypedArrayViewPrototypeFunctions.h:240-247`, which caches `typedVector()` across calls of JavaScript and tests `isDetached()` before each use: test and use lie in one window, and memory that was freed at a stop was detached before that stop, which the test then sees.
Storage from the garbage-collected heap that a parked stack names stays alive through the conservative scan, so a violation there costs a lost store. Memory from `freeAfterSafepoint` has no such net.

**Rule H11 (pins).** A thread without access counts as being at a safepoint, and H1 demands that blocking system calls run without access. The bytes of an ArrayBuffer are no cell. So a span that is used while heap access is released (`readSync(fd, buffer)`), or that is handed to another OS thread (Bun's work pool), is **pinned**: the native holds `ArrayBuffer::pin()` for that time. Contents with a pin are not put on the deferred list: `transfer`, `resize` and detach do their work on the views' words and leave the old bytes to the last `unpin` (06, TA9). `pin` and these three exclude each other under the buffer's lock. So contents that reach `freeAfterSafepoint` have no pin. **The last `unpin`** of contents that have been detached, transferred away or shrunk meanwhile does not free them: it hands them to `freeAfterSafepoint`. Another thread may have loaded vector and length before the detach and be in the middle of its window. In the table of built-ins `blocks: true` with an operand of a buffer type makes the call emitter pin.

**Rule H5 (markers).** A stop does not stop markers, so at the end of a safepoint operation's stop a marker may be in the middle of a visit. Memory that is handed to `freeAfterSafepoint` is read by markers only under the lock that its replacing writer held (the cell lock for `WeakMap` buffers and sparse maps, `Structure::m_lock` for property tables). A marker that has released the lock holds no pointer to the old memory. Contents of ArrayBuffers, `ProgramSymbols` blocks and RegExp code are not read by markers.

## 8.10 Collections

### Who conducts

Today the mutator that asks for a collection takes "the conn" and runs each stop-the-world phase itself, at its allocation slow paths; the collector thread rarely exists in a Bun process. With N mutators the holder of the conn can block, exit, or wait for a lock that a parked thread holds (R5 D5).

**In mt builds the collector thread always conducts.** `mutatorHasConnBit` is never set. `requestCollection` queues the request and notifies the thread.

```
Heap::stopTheMutator():      lockConductor(null); requestStop()
Heap::resumeTheMutator():    releaseDeferredFrees(); cancelStop(); unlockConductor()
```

The collector thread holds no lock, `Collector::m_threadLock` included, when it calls `lockConductor()`. The branch "the mutator has access: transfer the conn and bail" (`heap/Heap.cpp:2018-2055`) is compiled out.

Cost: a thread hand-off at each phase change, and a thread that exists in every mt process. In return every liveness question about the conn is gone.

**A thread that needs the collection to finish** (`collectSync`, an exhausted cage, `new Thread` below): `ticket = requestCollection()`; `becomeParked()`; wait on the condition of `Collector::m_threadLock` until `hasServedTicketWithLock(ticket)`; `acquireAccess()`. The test and the sleep are one critical section of that lock, and the collector thread serves tickets under it (`heap/Heap.cpp:2392-2398`). `mutatorWaitingBit` is gone. The caller is at a safepoint (S4); inside a scope the request is deferred as today.

`new Thread` that finds the free list of thread IDs empty and the quarantine not (09, section 9.2, step 3; 09, T1) requests a **full** collection and waits for it in this way. An ID leaves quarantine at the end of the first full collection that *began* after its thread's T4, so a collection that is under way does not count: the ticket is for a collection that begins after the request.

**Rule H14 (the conductor's context).** In mt builds `VM::topCallFrame`, the exception, the traps and the deferral depth are accessors that forward to `ThreadContext::current()` (07, section 7.2). Code of the End phase uses them: `ErrorInstance::reconcileWeakReferencesAtGCEnd` opens `DeferGCForAWhile` and calls the embedder's hook (`runtime/ErrorInstance.cpp:470-515`), which opens `DeferTerminationForAWhile`, `SuspendExceptionScope` and a throw scope (`bun:src/jsc/bindings/FormatStackTraceForJS.cpp:647-672`); `vm().reconcileWeakReferencesAtGCEnd()` and the client data's (`heap/Heap.cpp:797-800`). In a Bun process this code has nearly always run on the JavaScript thread. So the collector thread owns a `ThreadContext` of kind `Conductor`: exception, trap and deferral words; no allocators, no view, no thread ID; not in `m_threads`. Embedder hooks that can run in the End phase are listed (11, section 11.4). Each takes what it needs as an argument, or is declared to tolerate that all threads are parked, and then gets the main thread's embedder state.

**Rule H13 (`PreventCollectionScope`).** Today: take `m_collectContinuouslyLock`, wait for all collections, then "a collection can only start if this thread starts it" (`heap/Heap.cpp:3428-3444`), which is false with a second mutator; it does not nest. In mt builds it is a count in `Heap` under `Collector::m_threadLock`. To open the scope a thread increments the count and waits, as `Parked`, until no collection is active. While the count is above zero the collector thread serves no request, and `collectSync` and `tryCollectForExhaustedCage()` return without collecting, as they do under `DeferGC`: nobody waits for a collection that cannot come. The scope nests and may be held by several threads. `m_isCollectionPrevented` is "the count is above zero". It does not stop mutators: a user that needs them stopped (both heap snapshot builders, `deleteAllCodeBlocks`: `heap/Heap.cpp:1176,1228,1628`) runs safepoint operations inside it, one per step that needs a still heap.

### Phases

| Phase | Mutators |
| --- | --- |
| Begin, Fixpoint, Reloop, End | all parked or without access |
| Concurrent | running |
| NotRunning | running |

`SpaceTimeMutatorScheduler` decides when to stop and resume; it reads the summed allocation counters.

### What mutators no longer do

| Today | mt |
| --- | --- |
| `performIncrement`: the mutator drains with `m_mutatorSlotVisitor` as a tax on allocation | off in the first version. The scheduler's stops throttle allocation: its share for the collector rises to the whole as the headroom is used up. (One visitor per thread is possible later.) |
| `collectInMutatorThread` | off |
| AOT's shortcuts in the conn protocol (`stopIfNecessarySlow` skips when not running; an Eden collection with an empty eden skips marking) | the second stays |

### Barriers

The collector is a retreating-wavefront collector: the visitor blackens the cell, fences, reads the fields; the mutator stores, fences (while marking), reads the state. Owner re-scan barriers are per store and stateless, so two mutators that store into one owner need no coordination beyond private mark stacks. Marking cannot end without a stop of every thread, at which all mark stacks are merged and all stacks scanned.

| Item (R5 11.2) | mt |
| --- | --- |
| B1, B2 inline append to the mark stack | per thread |
| B3 stored-cell log | per thread |
| B4 `RememberedSlotSet::record` | sets over per-thread tables belong to per-thread registries. A set over a shared table has no list: `record()` is the byte store alone (07, K3). In the source `record()` appends to `m_writtenCards` whatever `m_cardsAreWrittenWithoutRecord` says (`heap/RememberedSlot.h:47-66`) |
| B5 card bytes | idempotent stores; visible at the stop |
| B6 `setCellState(PossiblyGrey)` by two threads | designed to be racy |
| **B7** the re-whitening compare-and-swap in `addToRememberedSet` during a full collection | **H2**, below |
| B9 barrier removal, cached threshold | sound by S2, S4, S5 |
| B11 `didChangeBarrierThreshold` | writes every view in `VMState::views`, during a stop. A view enters that list together with its copy of the words, under `m_threadsLock` (T3) |

`MarkStackMergingConstraint` merges every thread's mark stack and the heap's stack of ended threads (T4). The "Msr" constraint visits every thread's log.

**Rule H2 (no re-whitening).** In mt builds `addToRememberedSet` reads: `if (!isMarked(cell)) return;`. (The source: "We could return here", `heap/Heap.cpp:1358-1392`.)
Today an old cell that a full collection has not reached yet (unmarked, `PossiblyBlack`) is made `DefinitelyWhite` by a compare-and-swap, and made black again if it turns out to have been marked meanwhile. Between the two stores the cell is marked, scanned and white. With one mutator the only thread that could run a barrier in that window is the one inside the function. A second mutator that stores into the cell there reads white, skips its barrier, and what it stored is never marked.
The early return is sound for any number of mutators, each for itself: the store precedes a full fence (`writeBarrierSlowPath`, `:2868-2879`), which precedes the load of the mark bit that returned 0, which precedes the marker's atomic set of that bit, which precedes the marker's fence and its reads of the fields. So the visit that follows sees the store.
Cost: a store into such a cell takes the slow path each time, not once, while a full collection marks. W-8.4 measures it.

### What the marker reads of replaceable storage

| # | Rule |
| --- | --- |
| 1 | **The owner barrier follows every publication of storage.** A new butterfly (06, SW7), an Extension that is published by compare-and-swap (05, CO6), a table of a Map: after the publishing store, and before the publishing thread's next safepoint, the owner's barrier runs. Otherwise an old, black owner names young storage that the next Eden collection neither visits nor marks. (Today: `AuxiliaryBarrier::set`, `setExtension`; the stub `growVector` leaves for C++ when "the collector will not visit [the array] again", `aot/stubs/AOTStubHelpersStorage.cpp:210-211`.) A loser of the compare-and-swap needs no barrier of its own for the storage |
| 2 | **Kind, state and pre-capacity come from the storage word** of the butterfly that the visitor loaded (06, section 6.2, SW11) |
| 3 | **A pair that does not agree is not visited** (06, SW11). The visitor reads StructureID, butterfly, StructureID again (`runtime/JSObjectVisitInlines.h:347-380`). From the Structure, before it loads the butterfly, it has decided whether to take the cell lock (ArrayStorage), whether the butterfly is a `JSCellButterfly` to be appended as a cell, and how to find the base of the allocation (`:51-80`). A replacement together with which the Structure changes is installed between the nuke of the StructureID and the store of the new one (06, SW12), so the visitor gets exact pairs, as today. As a second line of defence, and the first for `Confined` storage that converts in place: if the kind of the storage word is not the kind of the Structure's indexing type, or its state is `ReadOnly` without the type being copy-on-write, the visit gives up (`didRace`), and the cell is visited again at the next stop |
| 4 | **All slots are visited** (06, SW10). For Contiguous storage the visitor reads `vectorLength` (constant for the life of the storage, 06, SW2) and visits that many slots, not `publicLength` (`:103`). Writers of `Shared` storage can leave a value above `publicLength` for good; a later growth of the length shows it again. It must not have been freed |
| 5 | Retired storage that a reader still holds in its window is not freed before that reader's next safepoint. A value that the reader takes from it is in a register or frame at that safepoint, and is found by the scan |

### "No marker is reading" is not "nobody is reading"

`mutatorShouldBeFenced == false` licenses six things today (R5 6.7). In mt builds:

| Use | mt |
| --- | --- |
| Extension granules move in place | unpacked Extensions; packed ones only for Confined layouts |
| a stub grows a `CompactTable` without the lock | 06, MS1, MS5 |
| in-line append to the mark stack; stored-cell log | fine: per thread |
| ARM64: no fence after initializing a cell | fine: 04, F1 and F1' cover other mutators |
| ARM64: no nuke, no fences around a butterfly change | always nuked and fenced for storage that is not `Confined` |
| large butterflies are `realloc`ed | only `Confined` |

## 8.11 Sweeping, destructors, weak things

`MarkedBlock::Handle::sweep` runs on whichever thread claimed the block: in an allocation slow path, in the incremental sweeper, in a steal, in a synchronous sweep, at shutdown. It runs destructors and `WeakHandleOwner::finalize`.

| Item | mt |
| --- | --- |
| Cells without destructors (most AOT cells) | any thread sweeps |
| Strings (deref a `StringImpl`), Structures, `ModuleRecord`, `StringBuilderStorage`, engine classes | destructors are thread safe (atomic counts, sharded atom table); any thread sweeps |
| **Native classes with `threads: "owner"`**; linked `CodeBlock`s (10, I3) | **thread-affine blocks**, below |
| `SweepingScope` writes `m_mutatorState` | per thread |
| `IncrementalSweeper` (a timer on the VM's run loop) | runs on the main thread's loop for directories that are not affine; each thread's loop sweeps its own affine blocks when idle. `freeBlock` under `MarkedSpace::m_lock` |
| `WeakSet::allocate` / `deallocate` (per block of the referent, unsynchronized) | the block's header lock. The lists of the heap that they take from and return to (`m_detachedWeakBlocks`, `m_pooledWeakBlocks`, their counts: `heap/Heap.cpp:2799-2850`): a leaf lock `Heap::m_weakBlockPoolLock`, below the header locks |
| `Heap::addFinalizer`, `protect`, `m_weakGCHashTables`, `m_dirtyWeakGCHashTables`, `m_observers`, `m_gcCompletionCallbacks`, `m_arrayBuffers` | `Heap::m_lock` or a leaf lock each; the appendix has the row for each member of R5 2.6 |
| `WeakGCMap`s | the owning table's lock (07) |
| **`WeakRef`** | the standard's own device. `MutatorContext::m_keptAlive`: `deref()` and the constructor append the target; the vector is a root of its thread; it is cleared where `m_currentWeakRefVersion` is bumped today, at the end of a job. The `WeakRef` has no version field, and the visitor treats the target as weak. (One record per `WeakRef` would not do: a second thread's `deref()` overwrites the first's, and the target dies in the middle of the first thread's job) |
| **`FinalizationRegistry`** | dead registrations are moved to lists under the cell lock, as today, at the end of marking. The conductor posts the cleanup task to the inbox of the thread that created the registry. That home is a thread ID in the cell, and the registry's visitor is one of those of 09, T1: the ID of an ended thread becomes `NoThread`, and the task then goes to the main thread |
| `DeferredWorkTimer` | one per thread |

### Thread-affine blocks

An iso subspace whose class is `owner` keeps, in the header of each block, the ID of the owner thread. Only the owner's allocator claims such a block and only the owner sweeps it. So a destructor or finalizer runs on the thread that made the object, and never at the same time as a method of it. 53 of Bun's classes keep the Rust struct inside the cell, so "post the finalizer to the owner" is not possible for them; sweeping by the owner is.

The source sweeps in more places than the allocation slow path. Each needs a rule.

| # | Rule |
| --- | --- |
| **H6** | A thread-affine subspace has `numberOfLowerTierPreciseCells = 0`. (Default 8, `runtime/JSCell.h:107`; Bun passes `T::numberOfLowerTierPreciseCells`, `bun:src/jsc/bindings/BunClientData.cpp:221`. Lower-tier cells are handed out before the first block is made, `heap/LocalAllocator.cpp:187`; for a class with a handful of instances all of them would be precise.) Its directory is **not** in the list of directories that its `AlignedMemoryAllocator` steals from: in a compiled process every iso subspace uses `heap.cellAllocator()` (`heap/IsoSubspace.cpp:47`), so `findEmptyBlockToSteal()` crosses all of them, and the thief sweeps (`heap/LocalAllocator.cpp:260-275`); "empty" means no marked cell, destructors are still owed. `shrink()` frees a block of it only if the block is empty **and swept**. A swept, empty block loses its owner and may be claimed by any thread's allocator of that directory, which writes its own ID under the `inUse` claim. Cost: 16 KB per class and thread that makes an instance |
| **H7** | `sweepAll`, `sweepSynchronously` (after `collectNow(Sync)`, in the epilogue under `shouldSweepSynchronously()`, `Bun.gc(true)`) and the incremental sweeper skip affine blocks of other threads. `assertNoUnswept()` excludes them. A thread that is asked to sweep everything posts "sweep your affine blocks" to each owner's inbox |
| **H8** | No instance of an `owner` class is a precise allocation. Such a class lives in an `IsoSubspace` (one fixed cell size, at most `MarkedSpace::largeCutoff`), never in a `PreciseSubspace` or a `CompleteSubspace`: a static assertion in `subspaceForClass` and `subspaceForImpl`. An `IsoSubspace` makes precise allocations only as lower-tier cells. So with H6 the owner test of 11, section 11.3 has one form (the block's header), and `sweepPreciseAllocations()`, which the conductor runs (8.12), meets no destructor with thread affinity |
| **H9** | `WeakHandleOwner::finalize` belongs to the block of the **referent**, which is an ordinary block for Bun's `Weak` (`bun:src/jsc/bindings/Weak.cpp:27-44`), the wrapper cache and N-API's weak references. A `WeakHandleOwner` is classified like a native, by a virtual `threads()`: `any` (runs on the sweeping thread; all engine owners) or `owner`. For `owner`, `Weak<>` records the creating thread's ID beside the context, and `WeakSet::sweep` does not call `finalize` but posts `(owner, context)` to that thread's inbox; if it has ended, to the main thread's, with `owner_has_ended`. The ID follows 09, T1. A `WeakImpl` is no cell and has no visitor, so in a full collection `reapWeakHandles` does the work: `WeakBlock::reap` (`heap/WeakBlock.cpp:201-223`) replaces a quarantined ID by `NoThread` in every `WeakImpl` that is `Live` **or `Dead`** (a dead one still owes its `finalize` until its block is swept), also in a block without a live one |
| **H10** | **Blocks of an ended thread get the owner `Orphan` (0xFFFF)**, which no thread's ID equals. Every owner test fails on them with `ConcurrentAccessError("the thread that created this object has ended")`. `threadWillEnd` runs after a last sweep of the thread's affine blocks (T4). What native structs can point into (the Bun `VirtualMachine` struct, `RuntimeState`, the `Strong` list, the arena) is reference counted by the thread's affine blocks and freed when the last of them is empty. An orphan block is not allocated from while it holds a cell. The main thread sweeps orphan blocks; destructors run there with the flag `owner_has_ended`, under which they release memory and descriptors and touch no loop. An orphan block that has been swept empty gives up its reference to the ended thread's native state and is an empty block like any other: it has no owner, and may be claimed or freed (H6). A class whose instances are meant to outlive their thread is `any` |

A thread that blocks for ever, or computes without allocating that class and without an idle loop, does not finalize its dead objects: descriptors stay open until it sweeps or ends. That is documented with the `owner` classes (11).

`lastChanceToFinalize` runs when the VM is torn down, after all spawned threads have ended (09, section 9.11): every affine block is then the caller's or an orphan.

## 8.12 The end of a collection

`runCollectionEpilogue()` runs today on whichever thread next has access, beside running mutators (`heap/Heap.cpp:2319-2382`). In mt builds `needCollectionEpilogueBit` is gone and each part has its place.

| Part | mt |
| --- | --- |
| `sweepEagerlyInEpilogue()`: `sweepPreciseAllocations()`, the WebAssembly memory space | inside the End phase, by the conductor (H8: no destructor with affinity) |
| `pruneUnusedSourceProviderCaches()` | inside the End phase, by the conductor |
| clearing VM caches (`hasOwnPropertyCache`, megamorphic, JSON, `keyAtomStringCache`, `stringSplitCache`, `stringReplaceCache`, `immutableButterflyToStringCache`) | they are per thread: each thread clears its own in `didResume()`, before it touches the heap again (H12) |
| dropping replaced string buffers that no stack names | by the conductor, with all stacks scanned and all threads' lists merged |
| `GCCompletionCallback`s, Bun's `GarbageCollectionController` | posted to the main thread's loop |
| `shouldSweepSynchronously()`: `sweepSynchronously()` | posted to the main thread's loop; H7 |
| `AOT::HeapHooks::finalize`: dead Instances, `removeDeadEntries`, caches "cleared each collection" | while stopped: `VMState::instances` for each realm's registry, `m_threads` for each thread's registry and view |
| quarantined property offsets become free (06, N2). Tables with a non-empty quarantine are registered in `Heap::m_tablesWithQuarantine`, under `Heap::m_lock`. Only dictionaries have one; other Structures never reuse an offset (06, N2') | while stopped |
| deferred frees (8.9) | while stopped, as at the end of every stop |
| thread IDs in quarantine become free, at the end of the first full collection that began after their thread's T4 (09, T1) | while stopped |
| views and `ThreadState`s of threads that ended during the collection (H4) | freed at the end of the End phase |
| `m_collectionCount` (H12) | incremented last |

**Rule H16 ("no JavaScript is running" does not exist).** `entryScope` is per thread (07, section 7.2). Its consumers read it as a fact about the VM, and free things on its strength. `VM::isIdle()` is compiled out in mt builds, and each consumer becomes one of two things.

| Consumer | mt |
| --- | --- |
| `clearConcurrentRetainedDataIfPossible` (`heap/Heap.cpp:1406-1434`, called from the sweeper's timer, `heap/IncrementalSweeper.cpp:77`): frees every parked `StringImpl` | empty. Only the pruning of the table above remains, which looks at all stacks |
| `RegExp::deleteCode`, pruning of the RegExp cache | the compiled forms go to `freeAfterSafepoint`. A matcher holds them inside one window. One that parks at its poll first records its cell in `ThreadContext::m_executingRegExp`, and a form whose cell some thread has recorded stays on the list (06, section 6.9): `releaseDeferredFrees()` runs at the end of every stop, and only collections scan stacks, so the stacks cannot be asked |
| `VM::whenIdle` | the callback runs in a safepoint operation |
| `deleteAllCode`, `shrinkFootprintNow` (`runtime/VM.cpp:1175-1253`) | `main`, and refused while `m_threads` has more than one member |

## 8.13 Conservative scanning

The "Cs" constraint scans **every thread in `m_threads`**, whatever its `m_access` says: the registers and the stack range in `m_parkedState`, in place. No suspend, no copy, no signal. (A thread that returns from a native during the stop shows `HasAccess` for a moment, inside `acquireAccess()`. Its parked state is valid all the same, 8.3.)

- A JavaScript thread is not in `MachineThreads`: of the steps of `JSLock::didAcquireLock`, `addCurrentThread()` is not taken for it (09, section 9.3). `MachineThreads::tryCopyOtherThreadStacks`, which suspends by signal, stays for embedder threads that once held the API lock and are not JavaScript threads.
- `capture()` saves callee-saved registers too: a thread that blocks in a native may have cells only there.
- Each 32-bit half of a word is tried as a narrow reference, headers of frame objects keep their Structure alive: unchanged.
- `sanitizeStackForVM` uses the thread's `m_lastStackTop`.
- "This function must be run after `stopThePeriphery()`": unchanged; `stopAllocating()` covers all threads' allocators.
- A conducting JavaScript thread runs no collection, so its own stack is never scanned while it conducts.

## 8.14 Life of a mutator

`VM::m_threads` is the set of mutators, under `m_threadsLock` (H3). `VM::m_threadTable` maps a thread ID to a reference-counted `ThreadContext`, and holds the free and the quarantined IDs, under the leaf lock `m_threadTableLock`. Whoever only needs a context for an ID (`post`, 09, T2; the parent that allocates an ID, 09, section 9.2) takes the leaf lock. It can do so inside a `NoSafepointScope`, and during a stop.

**Rule T3 (start).** On the new OS thread:

```
register():   1  make the ThreadContext; stack bounds; m_access = NoAccess                      // nothing of it is known to anybody
              2  reserve and map the view; copy and rebase its constants (07, V7); make the ThreadState (07, V4), the registry, the queue, the inbox
              3  vm.m_threadsLock.lock()                                                        // a plain wait: this is no mutator yet
              4     under instancesLock: copy the pushed words into the view (07, V9: barrierThreshold and mutatorShouldBeFenced from Heap,
                            the others from the canonical view); append the view to VMState::views
              5     append: poll page to LoopPollPages, mirror to the StackManager, queue to m_microtaskQueues, registry to the collector's list
              6     m_threads.append(this); { Locker l(m_threadTableLock); m_threadTable[id] = this }
              7     m_stopCountSeen = heap.m_stopCount; m_collectionCountSeen = heap.m_collectionCount
                    if the VM has a termination request: set this thread's termination trap (09, section 9.10)
              8     acquireAccess()                                                             // returns at once: no stop is requested while the lock is held (H3)
              9  vm.m_threadsLock.unlock()
             10  with access: the allocators that the view needs before compiled code runs (8.8)
```

There is no "if a stop is requested, wait and try again": the conductor holds the lock for the whole stop, so step 3 is that wait. The collector's two words change only during a stop, hence not while this thread holds the lock. Other pushed words are written outside stops, under `instancesLock`: their writer either finds the view listed, or the view finds the word written. A registry that is added between Begin and End counts as never visited in this cycle. An OS thread that `new Thread` has made and that has not reached step 6 is in no list, so the fan-out of a termination misses it; the request is stored in the `VM` under the same lock, and step 7 looks at it.

**Rule T4 (end).** On the ending thread, after 09, section 9.2 has drained the jobs and closed the inbox (09, T2). `closeInbox()` has set the view's `homeID` to `NoThread`, so a promise that a hook of `threadWillEnd` settles in step 1 queues nothing on this thread's queue, which nobody would drain:

```
unregister(): 1  with access: sweep the thread's affine blocks a last time; embedder hook threadWillEnd; owner of the blocks = Orphan (H10)
              2  with access: stopAllocatingForGood() for each allocator                         // returns the rest of each free list to its block
              3  lockParking(vm.m_threadsLock)
              4     merge into the heap's: mark stack, stored-cell log, deferred frees, replaced string buffers, the blocks of the StrongSet, counters
              5     destroy the allocators                                                      // removes them from the directories' lists
              6     remove: view (under instancesLock), poll page, mirror, queue, registry from the lists of T3, steps 4 and 5
              7     m_threads.remove(this); { Locker l(m_threadTableLock); m_threadTable[id] = null; quarantine id (09, T1) }
              8     m_access.store(NoAccess, seq_cst)
              9     if a collection is between Begin and End: heap.m_retiredThreads.append(view, ThreadState)   // H4
             10  vm.m_threadsLock.unlock()
             11  unmap the view and destroy the ThreadState, unless step 9 took them; drop the thread's reference to the context
```

09, section 9.2 takes step 1 before the thread publishes its result and wakes its joiners, and the rest after. After step 8 the thread touches no cell, and the collector knows nothing of it. Its kept-alive list, `ThreadLocal` values and inbox are no roots any more.

**The stack is dropped from the scan by step 7.** That step runs under the lock, so no stop is in force, and a later stop does not find the thread. The OS thread may return, and its stack may be unmapped, at once.

**Rule H4 (retired threads).** Markers read root tables outside stops: the first visit of a full collection turns them into `DeferredRootRegion`s "that any marker thread takes later" (`heap/RememberedSlot.h:237-270`). A region is `{ visit, owner, beginOffset, endOffset }`, and a marker calls `visit(*this, owner, ..)` at any time of the Concurrent phase (`heap/SlotVisitor.cpp:921-944`). `owner` is the C++ object that registered the set, and the table is whatever that object points at: the thread's front of `StructureTable` defers its allocation site records, which lie in the thread's table arena (`aot/object/AOTStructureTable.cpp:274,342`); the twin of `ProgramStatics` for the private statics defers regions of the view (`aot/runtime/AOTProgramStatics.cpp:173`). So what is kept of a thread that ends between Begin and End is its **view** and its **`ThreadState` with everything that it owns** (07, section 7.3.1): the registry, the objects that are registered with it, the table arena, the reserved tables of 07, section 7.4. They go to `Heap::m_retiredThreads`, and the conductor frees them at the end of the End phase. Whether a collection is between Begin and End is stable under `m_threadsLock`, since both phases are stops. What a marker reads there are references that were valid when the thread ended; nothing is swept before End; marking them keeps some garbage for one cycle.

The `ThreadContext` itself is reference counted (09, T2) and holds nothing that a marker reads. Nothing that shared state names lies in memory that dies with the thread (09, T5).

| Event | Steps |
| --- | --- |
| Thread starts | T3, then 09, section 9.2 |
| Thread ends | 09, section 9.2, then T4 |
| An embedder's thread enters the API for the first time (09, section 9.3) | `JSLock::m_embedderLock`, then T3 |
| Process exit (09, X1) | after the `exit` handlers the main thread becomes conductor (`becomeParked()`, `lockConductor(null)`, `requestStop()` with a limit of `BUN_JSC_exitGraceMs`) and never cancels. It ends the process without static destructors and without `atexit` handlers. Unless the limit ran out, no thread runs compiled code or holds a leaf lock at that moment |
| VM teardown (a Worker with threads ends) | 09, section 9.11: terminate and join its threads, then destroy the VM |

## 8.15 Locks and waits

**Rule H15 (two kinds of lock).** Each lock of an mt build is of one of two kinds, and its declaration names the kind.

| Kind | The holder | Who waits for it |
| --- | --- | --- |
| **parking** | may reach a safepoint, park, allocate with collection, in some cases run JavaScript | has released heap access (`lockParking`). The wait is a safepoint, so it is forbidden in a `NoSafepointScope` (S4) |
| **leaf** | is in a `NoSafepointScope` from lock to unlock: it does not park, does not give up access, does not run JavaScript, allocates only without collecting, and waits for nothing but leaf locks further down | keeps heap access. The holder is running, so the wait is short, and a stop that is requested meanwhile completes after both have gone on to their next safepoint (05, L5) |

A lock under which a thread may reach a safepoint and whose waiters keep access deadlocks with a stop: the holder parks, the waiter never does.
"Release access, take the lock, take access back with the lock held" deadlocks if a conductor needs that lock. So: **no conductor, no closure and no phase of the collector takes a parking lock, except `m_conductorLock` and `m_threadsLock`**, for which H3 gives the order.

From outer to inner. A thread that holds or owns an entry takes, or waits for, only entries further down. The one exception: ranks 2 and 3 are owned while JavaScript runs, so a thread that owns some of them may wait for others of either rank.

| Rank | Lock or wait | Kind | Notes |
| --- | --- | --- | --- |
| 1 | `JSLock::m_embedderLock` (09, section 9.3) | parking | taken by a thread that is no JavaScript thread yet |
| 2 | Claims in the wait-for graph (09, W1): `join`; a module claim (09, MD3''); a claim word of 07, Z2 | wait, no lock | owned while JavaScript runs. The waiter has released access and `m_nativeSerialLock`. There is no order among them: a wait that would close a cycle throws in the waiter. A module claim is waited for by `compareAndPark` on the state word of the claim record (09, MD3''), a claim word of Z2 on its **canonical** address, which is the same in every view (07, Z2, part 3). A module claim that grows waits with the claim held (09, MD3'') |
| 3 | The program's own waits: `Lock`, `Condition.wait`, `Atomics.wait` | wait, no lock | access released. Not in the graph: a cycle that goes through one of them (a module body that takes a `Lock` whose holder needs that module) is the program's deadlock. Each wake-up checks the thread's trap word (09, section 9.13) |
| 4 | `VM::m_nativeSerialLock` (11, section 11.1) | parking | the holder allocates and may park. Released around calls into JavaScript and around every wait of ranks 2 and 3 |
| 5 | `JSGlobalObject::m_declarationLock` (10, section 10.3) | parking | held over a check phase and an act phase that allocate. The check phase asks the global object for each name (`runtime/ProgramExecutable.cpp:75`, `runtime/JSGlobalObjectInlines.h:551,562`), and a name of its static table can be a lazy word (`runtime/Lookup.h:63-64`), whose first read makes it (07, Z2). **Under the lock no lazy word is made or waited for**: a name whose word is not made yet makes the holder release the lock, make the thing or wait for its claimant (09, W1), and start the check phase again. The check phase has changed nothing. So no JavaScript runs under the lock, and its holder waits at none of the ranks 2 to 4 |
| 6 | `Heap::m_conductorLock`; waiting for a collection; opening a `PreventCollectionScope` | parking | the waiter is `Parked` |
| 7 | `VM::m_threadsLock` | parking for its waiters | H3. A holder that is not the conductor behaves as under a leaf lock |
| | | | **from here on: `NoSafepointScope`** |
| 8 | `VM::prototypeChainLock` (05, section 5.7) | leaf | |
| 9 | object lock: the outer object, then its side object (05, L3) | leaf | also taken by markers, for some classes (8.6, point 4) |
| 10 | `Retired` storage (06, section 6.3) | wait, no lock | a writer waits until the owner has published the new storage. The owner has copied before it retires; between its two stores it takes no lock, allocates nothing, calls nothing. Compiled code leaves on the `miss` edge of `AcquireStorage` for an operation that calls `Thread::yield()` and starts again; C++ that holds an object lock yields and tries again in place, with access kept |
| 11 | the busy bit of a promise (09, PR1) | leaf (spin) | no lock above rank 18 is taken under it (09, PR7) |
| 12 | `Structure::m_lock` | leaf | also taken by markers |
| 13 | table locks of 07, section 7.7, one at a time; among them `Modules::m_lock` (09, MD3'), which is the lock of the realm's `JSModuleLoader` and covers its containers (09, MD10), and the lock of `SharedChainDependents` (07, E5). The loader's lock is also taken by markers: the loader's visitor takes it in place of the cell lock (10, section 10.3), which is rank 9 and could not be taken inside this rank | leaf | an append to a container that a registry visits takes `CacheRegistry::lock()` (rank 21) inside |
| 14 | `CodeCache`, `sourceProviderCacheMap`, a `SourceProviderCache`, `CompactTDZEnvironmentMap`, the allocator of `ScriptExecutable::m_index` (10, sections 10.2, 10.3) | leaf | parse and generate outside |
| 15 | `StructureCache::m_lock`, `RegExpCache::m_lock`; one of the 64 shards of the atom table; symbol registry; `WeakGCMap` owners | leaf | |
| 16 | an `ArrayBuffer`'s lock (06, TA7, TA9); a watchpoint set's lock (06, section 6.5) | leaf | |
| 17 | internal locks of `JSLockObject` and `JSConditionObject`, the waiter lists of `Atomics.wait` (09, section 9.13); `VM::m_waitGraphLock` (09, W1) | leaf | an edge is entered, and the cycle search made, before access is released. `Condition.wait` holds the condition's internal lock while it releases the `Lock`, whose slow path takes the lock object's internal lock. Order inside the rank: the condition's, then the lock object's (09, section 9.13) |
| 18 | `VM::m_threadTableLock`, then released; an inbox lock (09, T2) | leaf | also taken by the conductor and by threads that are no JavaScript threads |
| 19 | a thread's `VMTraps::m_trapSignalingLock`, `StackManager::m_mirrorLock`; the lock of `VMState::instances` and `views`; `LoopPollPages` | leaf | |
| 20 | `Heap::m_lock`, `MarkedSpace::m_lock`, directory locks (`m_bitvectorLock`, `m_localAllocatorsLock`), block header locks, `Heap::m_weakBlockPoolLock`, `Heap::m_deferredFreesLock`, cage lock | leaf | in this order |
| 21 | `CacheRegistry::lock()`, `ImmortalCells` | leaf | between a mutator and the collector |
| 22 | `Collector::m_threadLock`, `Heap::m_stopLock` | leaf | the conditions of 8.3, 8.6, 8.10 hang on them. A thread that sleeps on one of those conditions is `Parked`, `NoAccess`, or the collector thread |
| 23 | `ParkingLot`, `malloc` | leaf | |

Heap access itself is treated as a lock that lies above rank 1: whoever waits at ranks 1 to 7 has given it up.

Lock-free protocols (publication by compare-and-swap of 07, Z1; `Owned` to `Shared`; stores into slots; the return list of a `StrongSet`) wait for nobody and have no rank.

**Why a stop always completes.** The conductor waits for threads with `HasAccess`. Such a thread is running, or waits at ranks 8 to 23. What it waits for there is held by a thread that is inside a `NoSafepointScope`, hence neither parked nor without access, or by a marker, or by a helper thread in a short critical section; none of those waits for the conductor, and ranks only descend. So it goes on, and reaches a safepoint within the bounds of 8.5. A thread that waits at ranks 1 to 7 has no access and is not waited for.

**Why a stop always ends.** During a stop the conductor takes `m_threadsLock` (held by a thread that behaves as under a leaf lock, or free) and leaf locks. No thread that is parked holds a leaf lock (S4). A thread without access may hold one, but then it is running and releases it before it asks for access (8.3), so it does not sleep with it.

Deadlocks were the commonest late failure of #249. A debug build checks the order with a per-thread stack of ranks. The waits that are no locks (ranks 2, 3, 10, 11) push their rank too.
