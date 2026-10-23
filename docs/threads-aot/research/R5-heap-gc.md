# R5: the garbage collector and allocation as an AOT executable uses them

Scope: `heap/`, the parts of `runtime/` that tie a thread to a heap, and the heap-facing parts of `aot/`, at
oven-sh/WebKit#757 head 5c2923e566cb. Read only; nothing was built or run.

Conventions
- Paths are relative to `Source/JavaScriptCore/` unless they begin with `bun:`
  (`<bun>/`) or `WTF:` (`.../Source/WTF/wtf/`).
- "Mutator" = a thread that runs JavaScript or C++ on behalf of a VM and touches its heap. "Conductor" = the thread that
  steps the collector's phase machine (upstream's word). "The conn" = the right to be the conductor.
- Sentences that begin with **Inference:** are my conclusions. Everything else was read in the source at the cited line.

## 0. The ten answers in short

1. Upstream is building "one `Collector` collects several `Heap`s, each `Heap` still has exactly one mutator (its VM)". In
   this tree the data structures are there (`Vector<Heap*, 1> m_heaps`, join/leave, per-heap accounting, summed scheduler),
   behind `useGlobalGC` (default false). It cannot collect two heaps: `RELEASE_ASSERT(m_heaps.size() <= 1)` stands where the
   mutators are stopped (`heap/Collector.cpp:825-826`). It is **not** "one heap, N mutators", which is what we need.
2. One `Atomic<unsigned> m_worldState` per heap holds one `hasAccessBit`, one `stoppedBit`, one `mutatorHasConnBit`. About
   forty plain fields of `Heap`/`MarkedSpace` are written by "the" mutator without any lock (table in 2.6).
3. The bump pointers live in `LocalAllocator::m_freeList`. The `LocalAllocator`s are per VM (in `CompleteSubspace` of
   `JSC::Heap`, and in `GCClient::IsoSubspace` of `VM::clientHeap`). Compiled code reaches them through **pointers stored in
   the Instance** (per realm) and in per-Instance `AllocationSiteRecord`s. The fast path is load, compare, add-to-memory:
   not atomic. A `BlockDirectory` already supports a *list* of `LocalAllocator`s and claims blocks atomically.
4. The cage is per `Heap` (one 32 GB-aligned, 32 GB reservation each, pooled per process). `Cell32 = address >> 3`
   truncated to 32 bits; decode is `base + (index << 3)` with the base in a pinned register. All JSCells, precise
   allocations of cells, and Extensions are caged. Butterflies and typed-array storage are not.
5. Barrier: after the store, `cellState(owner) <= instance.barrierThreshold` goes to a stub that fences if marking, and
   appends the owner to the heap's one `m_mutatorMarkStack` (in line on x86-64, unsynchronized). Barrier removal rests on
   "no safepoint of *this function* between A and B", plus "not yet retained". It survives N mutators **iff** a thread can
   only be stopped at its own `collects` operations.
6. The marker already runs beside one mutator. AOT objects are easy for it: the layout never changes, references are read
   one aligned word at a time. But several paths switch to *in-place, non-atomic* updates when `mutatorShouldBeFenced` is
   false ("no marker reads this now"). Those are safe against a marker and unsafe against a second mutator.
7. Stacks are scanned conservatively, whole, with each 32-bit half also tried as a `Cell32`. `MachineThreads` already
   suspends and copies every registered thread's stack and registers.
8. The collector never forces the mutator to stop. It sets a bit and waits for the next `stopIfNecessary()` (allocation
   slow paths, some C++ calls, the run loop). Compiled loops do have a poll (`LoopHint`, a faulting load on Linux x86-64, a
   compare elsewhere), its IR effect is already `collects`, but the GC does not arm it and the poll's slow path does not
   call `stopIfNecessary()`.
9. Weak reconciliation runs on the conductor with the world stopped. Destructors, `WeakHandleOwner::finalize` and the
   collection epilogue run on "the" mutator: in allocation slow paths (lazy sweep), in the run-loop sweeper, and in
   `handleNeedCollectionEpilogue()`.
10. There are no JSCells in the image. "Immortal cells" are ordinary heap cells rooted by a per-realm list.

---

## 1. Upstream's Collector for several heaps

### 1.1 The series

`git log -- heap/Collector.{h,cpp}` plus `--author='Dan Hecht'`. All are upstream WebKit commits already in this tree's base.

| Commit | Date | Title (short) |
| --- | --- | --- |
| 2e80a204eb24 | 2025-10-05 | (Mark Lam) Implement a Stop the World mechanism for JSC (`VMManager`) |
| c2fa19276047 | 2026-09-24 | Split the GC's collection-cycle state and machinery into a Collector class |
| 6730df1f8510 | 2026-09-25 | Prepare visitChildren and marking constraints for a SlotVisitor that marks more than one heap |
| 101392f63ca9 | 2026-09-27 | Remove SlotVisitor's Heap |
| 5e53548c666f | 2026-09-28 | Move the marking constraint set to the Collector |
| 5297538c7daa | 2026-10-01 | Move the request queue checks and the mutator's phase loop from the Heap into the Collector |
| 729712aa495c | 2026-10-05 | Group the per-heap work of the Collector's Begin and End phases |
| f488eac0e00c | 2026-10-07 | Replace the Collector's Heap& with a vector of heaps |
| 5fa36a13d44e | 2026-10-07 | Assert that Heap methods are only passed cells from that Heap |
| ef6ad2a4ee36, b68bb7b7c02b, 34832d663f81, 75a1e77857c2 | 2026-10-08 | compiler threads, log sums, own visitor, GC verifier for several heaps |
| 192fd94f17a5 | 2026-10-09 | Make the GC's mutator scheduler work for collections of several heaps |
| 5e4ff7aa732e | 2026-10-12 | Support heaps joining and leaving a Collector, for global collections |
| 47697fd0eb3e, ddc9c7fbc680 | 2026-10-12 | scope decision and visitor checks; heap analysis |
| 94bf25067137 | 2026-10-13 | Make the GC's memory accounting work for collections of several heaps |
| 1d86f8086971 | 2026-10-14 | Remove optimizeParallelSlotVisitorsForStoppedMutator and its protocol |
| b8c79b443812 | 2026-10-14 | Clarify what MarkedBlock's versioned mark and newly-allocated bits record |
| 847b1d1f5e7f | 2026-10-15 | (Keith Miller) BlockDirectory's bits should be guarded by a mutate-grow lock |
| 9d236ecf922e | 2026-10-16 | Heaps that join global collections late adopt the others' marking version |
| 9960cb034c09 | 2026-10-21 | Move immutableButterflyToStringCache from Heap to VM |
| eaf0efa57b26 | 2026-10-22 | Stop a heap's local Collector threads when the heap shuts down |

### 1.2 The model

- One `Heap` per `VM`, by value (`runtime/VM.h:500` `Heap heap;`, `heap/HeapInlines.h:41-44` computes the VM from the heap's
  address). That does not change in the series.
- `class Collector`: "State belonging to a collection cycle." (`heap/Collector.h:59-60`). It owns the heaps it covers
  (`Vector<Heap*, 1> m_heaps`, `Collector.h:227-228`), the shared mark stacks, the race mark stack, the opaque roots, the
  collector's own `SlotVisitor` and the pool of parallel ones, the `MarkingConstraintSet`, the phase machine, the
  `MutatorScheduler`, the collector thread and the request queue (`Collector.h:230-314`).
- What stays per heap, and is reached by `forEachHeap`: `m_worldState`, `m_mutatorSlotVisitor`, `m_mutatorMarkStack`,
  allocation accounting, `MarkedSpace`, weak sets, `MachineThreads` (`heap/Heap.h:971-1088`).
- Local and global: `Heap::Heap` (`heap/Heap.cpp:309-311`)
  ```cpp
  , m_localCollector(Options::useGlobalGC() ? nullptr : makeUnique<Collector>())
  , m_collector(m_localCollector ? m_localCollector.get() : &Collector::globalCollector())
  , m_mutatorSlotVisitor(makeUnique<SlotVisitor>(*m_collector, "M"_s))
  ```
  `Collector::globalCollector()` is a process singleton that is never destroyed (`heap/Collector.cpp:154-164`).
  5e4ff7aa732e: "By default, each Heap owns its own Collector, which collects only that heap. ... With useGlobalGC enabled,
  all heaps are instead collected by one shared Collector, which is created once and never destroyed. ... Local and global
  Collectors may eventually coexist, but while global collection is being developed, the flag picks one for the whole process."

### 1.3 What the series says the goal is

- c2fa19276047: "The motivation is to eventually allow the Collector to drive a collection across multiple heaps in a single cycle."
- 5fa36a13d44e: "**Once VMs can share objects**, a VM's code can hold a cell that its own heap did not allocate, and may pass it to
  its own heap's methods." It then lists what is wrong for such a cell: `isMarked()` reads this heap's marking version;
  `addToRememberedSet()` "puts the cell in this heap's remembered set. That is where a shared cell should go, so that no other
  heap's set needs synchronizing, but the code there assumes the cell is marked only by this heap's collections and that no other
  thread writes to it"; `reportExtraMemoryAllocatedSlowCase()`, `addReference()`, `addFinalizer()`, `protect()`.
  "Nothing passes another heap's cell today."
- 94bf25067137: "...when it marks another heap's cell, which will be rare since a heap's cells will mostly reference their own
  heap's cells".
- 847b1d1f5e7f (Keith Miller): the one-mutator assumption of `BlockDirectory` "stands in the way of sweeping concurrently with the
  mutator, and of **a global GC with multiple mutators allocating from the same directory**. This patch starts making those cases
  possible."
- 2e80a204eb24 (Mark Lam): the VMManager stop-the-world "is needed by Global GC, the Wasm Debugger, and the Memory Debugger".

**Inference:** upstream's target is several VMs (each on its own thread, each with its own heap and allocators) that may hold
references into each other's heaps, collected together. Keith Miller's commit message is the only place that speaks of several
mutators on one directory.

### 1.4 What it can do today, step by step

Options: `useGlobalGC`, default false (`runtime/OptionsList.h:523`). Its only readers are `Heap.cpp:309`, `Collector.cpp:156`
and the test `API/tests/testapi.cpp:765`. Nothing in `aot/` or in Bun's half mentions it.

Join, leave, add a constraint (`Collector::MembershipChange`, `Collector.h:107-116`):
1. `Heap::notifyIsSafeToCollect()` requests `Join` and does not wait (`Heap.cpp:3404-3418`).
2. `Heap::shutDown()` requests `Leave` and waits (`Heap.cpp:495-498`), then stops a local collector's threads (`:500-503`).
3. `Heap::addMarkingConstraint()` requests `AddConstraint` and waits (`Heap.cpp:3395-3402`).
4. `Collector::requestMembershipChange` (`Collector.cpp:897-914`): under `m_threadLock`; if the collector is idle
   (`!m_threadIsWorking && !hasWork && !anyMutatorHasConn`, `:916-919`) the requester applies it itself, otherwise it is queued and
   "whichever thread conducts next applies it" in `runNotRunningPhase` (`:389-411`).
5. Applying `Join` adds the heap's core constraints and appends to `m_heaps` (`:934-954`). Each constraint records its heap so
   that `Leave` can remove them.

AOT already copes with the order: its two constraints are added before the heap joins, so
`shouldVisitRoots()` tests `visitor.collector().hasHeap(vm.heap)` (`aot/runtime/AOTHeapHooks.cpp:37-41`: "Until then other heaps'
collections run them too.").

Per-heap loops that exist: `beginCollectionInEachHeap` (`Collector.cpp:481-508`), `setUpVisitors` (`:510-529`),
`endCollectionInEachHeap` (`:763-785`), `stopThePeriphery`/`resumeThePeriphery` (`:846-873`), forgiving `m_incrementBalance`
(`:652-654`), waking waiting mutators and setting the epilogue bit (`:740-759`), `forEachSlotVisitor` including each heap's
mutator visitor (`heap/CollectorInlines.h:34-44`).

Scope: Full if any heap needs Full (`Collector.cpp:273-287`, with a FIXME that the policy "probably needs rethinking").

Marking version: one per collection, cached in every visitor; a late joiner adopts the others' version at Begin
(`Collector.cpp:483-491`, `heap/MarkedSpace.cpp:496-504`). 9d236ecf922e: "This works while each heap is collected only locally or only
globally... If a heap can later be collected both ways, this will have to change".

Scheduler (space-time balance): `SpaceTimeMutatorScheduler` reads `m_collector.totalBytesAllocatedThisCycle()` and
`totalMaxEdenSize()` (`heap/SpaceTimeMutatorScheduler.cpp:81,160`), which sum over heaps (`Collector.cpp:986-1002`).
192fd94f17a5: "Pooling the heaps' budgets is the simplest choice for several heaps. Whether to combine them differently ... is left until
collections of several heaps can be measured."

Cross-heap references: a visitor *can* mark a cell of any member heap. It asserts `m_collector.hasHeap(*heapCell->heap())`
(`heap/SlotVisitor.cpp:153,322`), charges bytes to the cell's own heap (`:304,354`, `flushBytesMarked` `:358-365`), and hands heap-analysis
nodes to the cell's own VM with a FIXME that "A cross-heap edge appears in neither VM's snapshot" (`:232-237`). But every mutator-side
`Heap` method asserts `cell->heap() == this` (`HeapInlines.h:65`, `Heap.cpp:626,695,707,718,1354,2703,2709`). So: traced yes, created no.

### 1.5 What is missing

- **Stopping and resuming several mutators.** `Collector::finishChangingPhase` (`Collector.cpp:799-844`):
  ```cpp
  // FIXME: Stopping the mutators of several heaps is not implemented yet.
  RELEASE_ASSERT(m_heaps.size() <= 1);
  ```
  and it calls `heap().stopTheMutator()`, `heap().resumeTheMutator()`, `heap().handleNeedCollectionEpilogue()`,
  `heap().waitWhileNeedCollectionEpilogue()`, where `heap()` asserts one heap (`Collector.h:70-75`, "FIXME: remove all uses of this").
  5e4ff7aa732e: "Global collection isn't usable yet, even for heaps with no references to each other: stopping the mutators of several
  heaps isn't implemented, so a collection can still cover only one heap."
- Two more single-heap uses in the marker: `SlotVisitor.cpp:893` (`m_collector.heap().m_stopIfNecessaryTimer->scheduleSoon()`) and `:966`
  (`drainInParallelPassively` reads one heap's `m_worldState`).
- The VMManager's GC stop handler: `case StopReason::GC: RELEASE_ASSERT_NOT_REACHED();` (`runtime/VMManager.cpp:556-557`).
- Teardown with the global collector (eaf0efa57b26: "This will need to be fixed for the --useGlobalGC=true case too, but that depends on
  how stopping the mutators of several heaps will work").
- Remembering a shared cell (`Heap.cpp:1352-1354` FIXME).
- `LocalAllocator` slow path: `// FIXME GlobalGC: Need to synchronize here to when allocating from the BlockDirectory in the server.`
  (`heap/LocalAllocator.cpp:183`) and the long FIXME at `:231-244` ("One way to make this work is to have a single per-Heap lock that
  protects all mutator lock allocation slow paths.").
- `GCClient::Heap::lastChanceToFinalize()` (`Heap.h:1336-1338` FIXME).

### 1.6 What it means for "one heap, N JavaScript threads"

**Inference.** The series does not give us N mutators on one `Heap`. Two things in it are directly useful:
1. The cut between cycle state (`Collector`) and per-mutator state. What the Collector reaches through `forEachHeap` is, almost item for
   item, what has to exist once per JavaScript thread: world-state word, mutator `SlotVisitor`, mutator mark stack, increment
   balance, waiting bit, epilogue bit. What it does *not* separate is `MarkedSpace`, accounting and weak sets, which we want shared.
2. `BlockDirectory`'s mutate-grow lock and `inUse` claim (section 3.5).

A conflict to know about: in an AOT process a narrow reference is decoded against the **holder's** cage
(`cageBaseOf(holder)`, `heap/MarkedBlockCage.h:112-122`) or against the pinned base register. Each heap has its own cage. So an AOT object
cannot name a cell of another heap by `Cell32` (except where heaps share a range, section 4.6). Upstream's "VMs share objects"
model and the per-heap cage do not compose; one shared heap does.

### 1.7 VMManager stop-the-world (already in the tree)

`VMManager` treats "VM (and its VMThreadContext)" as a thread (2e80a204eb24's message, also `runtime/VMManager.h`).
`requestStopAllInternal` (`runtime/VMManager.cpp:212-264`): sets `Mode::Stopping`, and for every VM calls `vm.requestStop()`, which sets an
entry-scope service bit and fires the trap `NeedStopTheWorld` (`runtime/VM.h:1288-1292`). A running VM reaches
`VMTraps::handleTraps` and parks in `VMManager::notifyVMStop` (`runtime/VMTraps.cpp:515-518`). An idle VM is not counted; it stops when it
next enters (`VM.cpp:2162-2168`). It is VM-granular: one trap word, one `isEntered()` per VM. Bun added `StopReason::JSDebugger`
(`VMManager.cpp:565`).

---

## 2. How a mutator thread relates to a Heap today

### 2.1 The world-state word

`heap/Heap.h:1070-1076`
```cpp
static constexpr unsigned mutatorHasConnBit = 1u << 0u; // Must also be protected by threadLock.
static constexpr unsigned stoppedBit = 1u << 1u; // Only set when !hasAccessBit
static constexpr unsigned hasAccessBit = 1u << 2u;
static constexpr unsigned needCollectionEpilogueBit = 1u << 3u;
static constexpr unsigned mutatorWaitingBit = 1u << 4u; // Allows the mutator to use this as a condition variable.
Atomic<unsigned> m_worldState;
bool m_worldIsStopped { false };
```
- `acquireAccess()`: CAS `0 -> hasAccessBit`, else slow (`HeapInlines.h:211-219`). Slow: parks while `stoppedBit`, then sets the bit, runs
  a pending epilogue, `stopIfNecessary()` (`Heap.cpp:2169-2194`). `RELEASE_ASSERT(!(oldState & hasAccessBit))`: access is not counted, so
  a second thread that acquires it crashes.
- `releaseAccess()`: CAS `hasAccessBit -> 0`, else slow (`Heap.h:674-679`, `Heap.cpp:2196-2229`). If the mutator had the conn in the
  middle of a phase change it sets `stoppedBit` for itself and wakes the collector thread.
- `stopIfNecessary()`: `if (m_worldState.loadRelaxed() != hasAccessBit) stopIfNecessarySlow()` (`Heap.h:704`, `HeapInlines.h:221-228`).
  "All allocations logically call this." (`Heap.h:689`).

### 2.2 The conn, in the order things happen

A collection that the mutator asks for (the usual case):
1. Allocation slow path: `LocalAllocator::allocateSlowCase` calls `heap.didAllocate(m_freeList.originalSize())`, then
   `heap.collectIfNecessaryOrDefer()` (`heap/LocalAllocator.cpp:144,171`).
2. `collectIfNecessaryOrDefer` (`Heap.cpp:2965-3053`): returns if `m_mutatorState` is `Sweeping`/`Collecting`; polls
   `stopIfNecessary()`; compares `totalBytesAllocatedThisCycle()` with `m_maxHeapSize - m_sizeAfterLastCollect`; if over:
   `collectAsync(); stopIfNecessary();`. If deferred it only sets `m_didDeferGCWork` or `deferralContext->m_shouldGC`.
3. `Heap::collect` (`Heap.cpp:1892-1915`) asserts the API lock, `RELEASE_ASSERT(vm().atomStringTable() == Thread::currentSingleton().atomStringTable())`,
   then `Collector::requestCollection(*this, request)`.
4. `requestCollection` (`Collector.cpp:242-264`): under `m_threadLock`; if nothing is queued and the collector thread is not working,
   "Taking the conn": `requester.m_worldState.exchangeOr(mutatorHasConnBit)`. The collector thread is notified only if no mutator has the conn.
   "This is an optimization that prevents the collector thread from ever starting in most cases."
5. The `stopIfNecessary()` of step 2 sees the bit: `stopIfNecessarySlow` -> `collectInMutatorThread` (`Heap.cpp:2098-2126`), under a
   `CollectingScope` (`m_mutatorState = Collecting`).
6. `Collector::collectInMutatorThread` loops `runCurrentPhase(GCConductor::Mutator, ...)` (`Collector.cpp:305-337`). For Fixpoint it first
   captures its own registers and stack bounds with `callWithCurrentThreadState` (`:317-334`, `heap/MachineStackMarker.h:66-71`).
7. Phases (`heap/CollectorPhase.cpp:32-48`): world suspended in Begin, Fixpoint, Reloop, End; running in NotRunning, Concurrent.
   With `conn == Mutator`, "stopping the world" is only `sanitizeStackForVM`, the pending epilogue, and `stopThePeriphery()`
   (`Collector.cpp:834-838`): **the mutator is stopped because it is the one running the collector.**
8. Fixpoint runs constraints and drains until `m_scheduler->shouldResume()`, then `changePhase(Concurrent)` (`:576-657`).
   `runConcurrentPhase(Mutator)` returns false, i.e. the mutator goes back to JavaScript while the helper threads mark (`:659-676`):
   "When the mutator has the conn, we poll runConcurrentPhase() on every time someone says stopIfNecessary(), so on every allocation slow path."
9. At some later poll: `visitor.didReachTermination() || m_scheduler->shouldStop()` -> Reloop -> Fixpoint -> ... -> End.
10. End (`:701-761`): helpers finish, per-heap end steps, tickets served, `setNeedCollectionEpilogue()` per heap. On the way to NotRunning the
    conducting mutator runs `handleNeedCollectionEpilogue()` itself (`:817-821`).

When the collector thread conducts (`conn == Collector`): `Heap::stopTheMutator()` (`Heap.cpp:2018-2055`)
- mutator has no access: CAS in `stoppedBit`, "We can stop the world instantly."
- mutator has access: **it is not stopped**. "Transfer the conn to the mutator and bail": sets `mutatorHasConnBit`, schedules the
  `StopIfNecessaryTimer`, unparks, returns false. The collector thread's `runCurrentPhase` returns `Finished`.

AOT-only shortcuts in this protocol: `stopIfNecessarySlow` skips `collectInMutatorThread` when the collector `isNotRunning()`
(`Heap.cpp:2116`, `Collector.cpp:531-534`); helpers are started lazily (`Collector.h:201`, `Collector.cpp:441-456`); an Eden collection with an
empty eden skips marking (`Heap.cpp:1006-1009`, `Collector.cpp:536-545,599`); `m_hasAllocatedSinceLastCollection` is cleared at the end of a
collection so that `stopAllocating()`/`resumeAllocating()` do nothing if nothing was allocated (`Heap.cpp:2601`, `MarkedSpace.cpp:341-351,377-388`).

### 2.3 JSLock

`JSLock::lock` (`runtime/JSLock.cpp:91-116`) is a recursive mutex with `m_ownerThread`. `didAcquireLock` (`:118-160`), in order: installs
the VM's `AtomStringTable` as the thread's current one; `m_vm->setLastStackTop(thread)`; acquires heap access unless the VM already has it
(`m_shouldReleaseHeapAccess`); `setStackPointerAtVMEntry`; if the thread differs from the last owner,
`m_vm->heap.machineThreads().addCurrentThread()`; `traps().notifyGrabAllLocks()`. `willReleaseLock` (`:301-331`) drains microtasks, releases
delayed objects, clears the entry stack pointer, releases access if it took it, restores the atom table.

The lock is what makes "the mutator" unique. The heap only *asserts* it: `ASSERT(heap.vm().currentThreadIsHoldingAPILock())`
(`LocalAllocator.cpp:140`), `BlockDirectory.cpp:161`, `Heap.cpp:690,702,1907`, `WeakSetInlines.h:38`, and
`Heap::assertIsMutatorOrMutatorIsStopped()` (`Heap.cpp:526-536`, with "FIXME: It feels like heap access should be tied to holding the API lock.").

### 2.4 MachineThreads

One `MachineThreads` per heap (`Heap.h:980`), a `ThreadGroup`. A thread is added the first time it takes the JSLock and stays until it
exits. "Only needs to be called by clients that can use the same heap from multiple threads." (`heap/MachineStackMarker.h:51-52`). Section 7.

### 2.5 What Bun does

- `vm.heap.acquireAccess()` once at VM creation, before the `JSLockHolder` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:607`), so the JSLock never
  releases it. The `StopIfNecessaryTimer` is disabled unless `--smol` (`:611-627`).
- Before the event loop waits: `vm->heap.stopIfNecessary()` for a few idle rounds (`bun:src/jsc/bindings/BunJSCEventLoop.cpp:108`). It releases
  access for the wait only while an idle collection is pending (`:133-141`): "a requested collection only advances at this thread's safepoints
  while this thread holds the collector's conn, and a parked thread has none; without access, the conn goes to the collector thread".

**Inference:** in a Bun process the JavaScript thread is nearly always the conductor, every stop-the-world phase runs on it, and the
`JSC Heap Collector Thread` rarely exists. Only the marking helpers run beside JavaScript.

### 2.6 What in `Heap` assumes exactly one mutator

All in `heap/Heap.h` unless noted. "Plain" = no lock, no atomic.

| State | Line | Who writes it, when | Notes |
| --- | --- | --- | --- |
| `m_worldState` | 1075 | mutator, collector | one access bit, one stopped bit, one conn bit, one waiting bit |
| `m_worldIsStopped` | 1076 | conductor | read by asserts and by `drainInParallelPassively` |
| `m_mutatorState` | 916 | plain, `AllocatingScope`/`SweepingScope`/`CollectingScope`/`RunningScope` | `AllocatingScope` has `RELEASE_ASSERT(m_mutatorState == Running)` (`heap/AllocatingScope.h`): two threads in a slow path crash |
| `m_hasAllocatedSinceLastCollection` | 917 | plain, slow paths | only ever set true by mutators |
| `m_nonOversizedBytesAllocatedThisCycle`, `m_oversizedBytesAllocatedThisCycle`, `m_lastOversizedAllocationThisCycle` | 943-946 | plain `+=` in `didAllocate` (`Heap.cpp:2683-2698`), `reportExtraMemoryAllocatedSlowCase` (`:609-632`) | also read by the scheduler on the conductor |
| `m_extraMemoryBytesSinceLastCollectionCheck`, `m_deprecatedExtraMemorySize` | 974-975 | plain | |
| `m_extraMemorySize` | 973 | CAS loop (`Heap.cpp:2934-2950`) | already safe: markers write it |
| `m_bytesMarkedThisCycle` | 957 | atomic | already safe |
| `m_bytesAbandonedSinceLastFullCollect` | 947 | plain | |
| `m_incrementBalance` | 959 | plain double, `performIncrement` (`Heap.cpp:3469-3497`) | the mutator's marking tax |
| `m_mutatorSlotVisitor` | 986 | the mutator drains with it in `performIncrement` | one visitor, not reentrant |
| `m_mutatorMarkStack` | 987 | plain append in `addToRememberedSet` (`Heap.cpp:1403`) **and by AOT stubs in line** | section 5 |
| `m_storedCellLog` | 988 | plain cursor bump, C++ and AOT stub | section 5 |
| `m_barriersExecuted` | 1088 | plain `++`, C++ and AOT stub | statistics only |
| `m_deferralDepth` | 1049 | plain `++/--` (`HeapInlines.h:168-184`) | `DeferGC`, `DeferGCForAWhile` |
| `m_didDeferGCWork` | 1079 | plain | |
| `m_mutatorDidRun`, `m_mutatorExecutionVersion` | 1078,1081 | plain | |
| `m_protectedValues` | 977 | plain `HashCountedSet` | read by the "Msr" constraint while stopped |
| `m_markListSet` | 978 | plain hash set (`MarkedVectorBase` spill) | |
| `m_strongSet` | 990 | `Strong<>` handle allocation, unlocked | |
| `m_arrayBuffers` | 972 | plain vectors (`GCIncomingRefCountedSetInlines.h:47-64`) | |
| `m_weakGCHashTables`, `m_dirtyWeakGCHashTables` | 1051-1052 | plain | |
| `m_detachedWeakBlocks`, `m_pooledWeakBlocks`, counts | 1011-1014 | plain | |
| `m_possiblyAccessedStringsFromConcurrentThreadsOrGCOwnedDataScope` | 1027 | plain append when a string's buffer is replaced | |
| `m_observers`, `m_gcCompletionCallbacks` | 1035-1037 | plain | |
| `m_sweeper`, `m_stopIfNecessaryTimer`, activity callbacks | 1030-1033 | `JSRunLoopTimer`s of the VM's run loop | run under the JSLock on the VM's thread |
| `m_isCollectionPrevented`, `m_auxiliaryEvacuationIsDue` | 933-934 | plain | |
| lazily made subspaces `m_##name` | 1201-1226, 1309-1321 | `name##Slow()`: `ASSERT(!m_##name)`, make, `storeStoreFence`, publish (`Heap.cpp:3514-3568`) | no lock on the server side; two threads would make two |
| `MarkedSpace::m_capacity` | `MarkedSpace.h:219` | plain `+=` | |
| `MarkedSpace::m_blocks` (hash set + bloom filter) | `:227` | plain in `didAddBlock`/`freeBlock` (`MarkedSpace.cpp:405-411,562-569`) | read by the conservative scan |
| `MarkedSpace::m_preciseAllocations`, `m_preciseAllocationSet` | `:210-211` | plain (`MarkedSpace.cpp:261-277`, `CompleteSubspace.cpp:175-183,213-237`) | |
| `MarkedSpace::m_newActiveWeakSets` | `:230` | plain list splice in `didAllocateInBlock` (`MarkedSpace.cpp:571-577`) | on every block a free list is built for |
| `MarkedSpace::m_isIterating` + `HeapIterationScope` | `:223`, `MarkedSpace.cpp:506-518` | **calls `stopAllocating()` on the calling mutator without stopping anybody** | |
| `VM::m_lastStackTop` | `runtime/VM.h:1434` | `sanitizeStackForVM` (`runtime/VM.cpp:1847-1863`) release-asserts that it lies in the current thread's stack | |
| `VM::m_currentWeakRefVersion` | `runtime/VM.h:1507` | plain | section 9 |

Locks that do exist: `Heap::m_lock` (client iso subspace creation, `Heap.cpp:3669`), `MarkedSpace::m_directoryLock` (directory creation,
`CompleteSubspace.cpp:68`), `BlockDirectory::m_bitvectorLock`, `m_localAllocatorsLock`, `MarkedBlock::Header::m_lock`,
`Collector::m_threadLock`, `m_markingMutex`, the cage's `ReservedRangeHeap::m_lock`.

---

## 3. Allocation

### 3.1 Where the bump pointers live

`FreeList` (`heap/FreeList.h:114-122`): `m_intervalStart`, `m_intervalEnd`, `m_nextInterval`, `m_secret`, `m_originalSize`, `m_cellSize`.
It is a member of `LocalAllocator` (`heap/LocalAllocator.h:74-82`) with `m_directory`, `m_currentBlock`, `m_lastActiveBlock`, `m_allocationCursor`.

Who owns the `LocalAllocator`s:
- Size-class spaces: `CompleteSubspace::m_localAllocators` and the lookup array `m_allocatorForSizeStep[numSizeClasses]`
  (`heap/CompleteSubspace.h:66-68`), **inside `JSC::Heap`** ("server"). One per size class per subspace, made on demand under `m_directoryLock`
  (`CompleteSubspace.cpp:53-104`).
- Iso spaces: `GCClient::IsoSubspace::m_localAllocator` (`heap/IsoSubspace.h:78-93`), inside `GCClient::Heap` = `VM::clientHeap`
  (`runtime/VM.h:501`, `Heap.h:1325-1380`). The `BlockDirectory` is in the server's `JSC::IsoSubspace` (`IsoSubspace.h:70`).

So the client/server split exists **for iso subspaces only**, and there is exactly one client per VM, by value, found by offset
(`HeapInlines.h:242-245`). There is no `ThreadLocalCache`, no `isSharedServer`, no `GCThreadLocalData` in this tree (grep finds none).
The earlier JIT-threads branch invented those (`<threads-branch>/docs/threads/SPEC-heap.md` 5.1-5.6: `HeapClientSet`,
`GCThreadLocalCache`, one `GCClient::Heap` per thread, one slow-path lock); none of it is here.

### 3.2 How compiled code and stubs find an allocator

Every route starts at the pinned `instanceGPR` (`aot/stubs/AOTStubRegisters.h:20`). The Instance is per realm. The pointers:

| Word | Where | Filled by |
| --- | --- | --- |
| `prefix.arrayAllocator`, `prefix.ropeStringAllocator` | `aot/runtime/AOTInstance.h:76,94` | `AOTInstance.cpp:144-147` |
| `allocation.auxiliarySpace`, `cagedAuxiliarySpace`, `cellSpace`, `primitiveAuxiliarySpace` (`CompleteSubspace*`; code then indexes `m_allocatorForSizeStep`) | `AOTInstance.h:180-183` | `AOTInstance.cpp:218-225` |
| `allocation.inlineAllocations[9]` {allocator, header}: Promise, Map, BoundFunction, BigInt, DefaultFinalObject, RegExpObject, NullPrototypeObject, RegExpMatchesArray, Set | `AOTInstance.h:153-160,184` | lazily, `prepareInlineAllocation` (`aot/runtime/AOTOperationScope.h:89-97`) |
| `allocation.typedArrayAllocations[16]` | `AOTInstance.h:162-167,185` | `aot/runtime/AOTTypedArrayOperations.cpp:41` |
| `runtime.constants.stringAllocator` | `aot/runtime/AOTRuntimeWords.h:70-71` | `aot/runtime/AOTProgramStrings.cpp:59` |
| `closures.slimClosureAllocators[3]` | `aot/object/AOTObjectWords.h:146-155` | before any compiled code runs |
| `coroutines.asyncGeneratorAllocator` | `aot/runtime/AOTCoroutineWords.h:45` | |
| `AllocationSiteRecord::allocator`, one record per (shape, order word, size, constructor) | `aot/object/AOTAllocationSiteRecord.h:16-27` | `StructureTable::fillAllocationSite` (`aot/object/AOTAllocationSites.cpp:121-160`) |

An `AllocationSiteRecord` is 32 bytes: `header`, `orderAndShapeWords`, `allocator`, unused. "The allocator is never null. Where there is none yet
it is one without room, so that the test for room, which every allocation makes, is the only one." Records lie in a table below
`objects.emptyAllocationSite()`, found through `objects.distancesToAllocationSites[allocationID]` (int32 distances,
`aot/stubs/AOTStubsAllocation.cpp:62-76`).

Compiled code treats several of these words as immutable: `loadImmutablePointer(AOT_INSTANCE_OFFSET(allocation.cellSpace))`
(`aot/codegen/AOTElementEmitterAllocation.cpp:239`, `AOTSavedFrameEmitterObjects.cpp:52`), `prefix.arrayAllocator` (`AOTElementEmitterAllocation.cpp:281`),
`objects.distancesToAllocationSites` (`AOTCellAllocationEmitter.cpp:173`). B3 may hoist and share such loads.

### 3.3 The fast path, instruction by instruction

`AssemblyHelpers::emitAllocateFromFreeList` (`jit/AssemblyHelpers.cpp:1070-1124`), used in line by `CellAllocationEmitter::allocateHeapCell`
(`aot/codegen/AOTCellAllocationEmitter.cpp:75-134`) and by the stubs (`aot/stubs/AOTStubHelpersAllocation.cpp:36-60`).

x86-64:
```
mov   result, [allocator + freeList.intervalStart]
cmp   result, [allocator + freeList.intervalEnd]
jae   refill
hasRoom:
prefetch [result + 64]                      ; only for constant cell sizes >= 48
add   qword [allocator + freeList.intervalStart], cellSize   ; immediate, or a register loaded from allocator.cellSize
```
ARM64: `ldp result, scratch, [allocator + intervalStart]`; `cmp`/`b.hs refill`; `add scratch, result, #cellSize`; `str scratch, [allocator + intervalStart]`.

Refill from the next interval of the same block (`emitFreeListRefill`, `:1126-1180`): load `nextInterval`; if bit 0 is set go to the slow
path; descramble `FreeCell::scrambledBits` with `m_secret`; write `nextInterval`, `intervalStart`, `intervalEnd` (three stores on x86-64, a pair
on ARM64); jump back to `hasRoom`.

Nothing here is atomic. Load, compare and add are three separate accesses to shared words.

The stub `AllocateCell` (`AOTStubsAllocation.cpp:143-185`): operand = allocation number; load the record; load its allocator; bump; optionally zero
the body; copy header (8 bytes) or header + order and shape words (16 bytes, one `movups`/`ldp+stp`) from the record into the cell; return.

The cell size is a compile-time constant only for sizes `<= MarkedSpace::preciseCutoff (80)` that are multiples of 16
(`knownCellSizeFor`, `AOTCellAllocationEmitter.cpp:57-62`); otherwise it is read from the allocator. The size-class progression is 1.1 in
an AOT process (`runtime/Options.h:137`, `Options.cpp:800-818`) and is recorded in the image's options (`aot/support/AOTCompileOptionWords.cpp:706`).

### 3.4 Which spaces and cell kinds AOT uses

`Heap.h:229-236`, `aot/runtime/AOTHeapCellClasses.h`:

| Space | Kind | What lies there | Memory |
| --- | --- | --- | --- |
| `cellSpace` (CompleteSubspace, `cellHeapCellType`) | JSCell, no destructor | `AOT::Object` (`AOTAllocationSites.cpp:110-113`), `SavedFrame`, `CompactTable`, generators, arrays made together with their first storage (`runtime/JSObjectVisitInlines.h:36-48`) | cage |
| `aotOuterCellSpace` (`outerCellHeapCellType`, `hasInnerCells = true`, `Heap.cpp:327`) | JSCell | objects allocated with room for their inner objects | cage |
| `aotClosureSpace` | JSCell | `Closure`, `SlimClosure`, `ReifiedSlimClosure`, `Environment`, coroutine frames | cage |
| `cagedAuxiliarySpace` (`auxiliaryHeapCellType`) | Auxiliary | `Extension` | cage |
| `auxiliarySpace` | Auxiliary | butterflies | **fastMalloc** (`Heap.cpp:391`) |
| `primitiveGigacageAuxiliarySpace` | Auxiliary | typed-array vectors | Gigacage |
| iso: `aotModuleRecordSpace`, `aotStringBuilderStorageSpace` (destructible), `compiledStructureSpace` (structure heap) | JSCell | | cage / structure heap |
| the engine's iso spaces (`arraySpace`, `stringSpace`, `ropeStringSpace`, `promiseSpace`, `mapSpace`...) | JSCell | | cage |

Inline-visited classes: `CompiledObject`, `CompiledEnvironment`, `CompactTable`, `SlimClosure`, `CompiledClosure` (`AOTHeapCellClasses.h:23-28`).

### 3.5 The slow path

`operationAOTAllocateSlow` (`aot/runtime/AOTAllocationOperations.cpp:31-44`) -> `StructureTable::allocateAsCompiledCodeDoes`
(`AOTAllocationSites.cpp:236-250`) -> `CompleteSubspace::allocate` -> `LocalAllocator::allocateSlowCase` (`heap/LocalAllocator.cpp:137-218`). In order:
1. `heap.didAllocate(m_freeList.originalSize())`: the whole consumed free list is accounted at once. (Fast-path allocation touches no counter.)
2. `didConsumeFreeList()`: the block gets its `allocated` bit and releases `inUse` (`MarkedBlock.cpp:491-502`).
3. AOT only: a directory with no block yet may hand out a cell of a larger size class (`tryAllocateInLargerCell`, `CompleteSubspace.cpp:122-135`).
4. `AllocatingScope`; `heap.collectIfNecessaryOrDefer(deferralContext)`: **the safepoint**.
5. `tryAllocateWithoutCollecting` (`:229-279`): `m_directory->findBlockForAllocation(*this)`, then `tryAllocateIn(block)` = `block->sweep(&m_freeList)`:
   **lazy sweeping, destructors included, runs here on the allocating thread**. Else steal an empty block from another directory of the same
   `AlignedMemoryAllocator`.
6. Iso only: lower-tier precise cells.
7. `tryAllocateBlock` -> `MarkedBlock::tryCreate` -> cage; `addBlock`.
8. Cage full: section 4.5.

What 847b1d1f5e7f made safe (comment at `heap/BlockDirectory.h:243-259`):
- `m_bitvectorLock` is a read-write lock: `grow()` exclusive in `addBlock`/`removeBlock` only, `mutate()` shared for everything else.
- "A block's inUse bit is a per-block lock: claimInUse() (acquire) and releaseInUse() (release)." `findBlockForAllocation`
  (`BlockDirectory.cpp:120-145`) scans `canAllocate & ~inUse`, claims, rechecks, clears `canAllocate`. `findEmptyBlockToSteal` likewise (`:86-118`).
- Single-bit accessors are relaxed atomics. Cursors are `VersionedField`s.
- "Exception: growth only happens on the mutator, so the mutator, or anyone while the world is stopped, may read the bits without mutate()".
- A directory has a **list** of allocators: `SentinelLinkedList<LocalAllocator> m_localAllocators` under `m_localAllocatorsLock` (`:263,285`;
  `LocalAllocator::LocalAllocator` appends itself, `LocalAllocator.cpp:49-55`). `stopAllocating`, `resumeAllocating`, `prepareForAllocation`,
  `stopAllocatingForGood` iterate over all of them (`BlockDirectory.cpp:232-305`). The allocation cursor is already per allocator.

### 3.6 What "one allocator set per thread" needs

**Inference**, from the above:
1. A per-thread table of `LocalAllocator`s for every `(subspace, size class)` and every iso space that a thread allocates from. The directory
   side is ready for it. The creation side is not: `CompleteSubspace` has one allocator per size class and publishes it in
   `m_allocatorForSizeStep`, which compiled code indexes directly.
2. A per-thread base from which compiled code reaches that table. Today every allocator pointer hangs off the per-realm Instance, and some of the
   loads are declared immutable. The choices are a third pinned register / TLS for a thread block, or one Instance *view* per thread. The
   `AllocationSiteRecord` mixes per-realm data (header, words) with a per-thread pointer (allocator), in a table whose records are also rewritten at
   run time (`predictStructure`, `forgetStructuresOf`, `AOTAllocationSites.cpp:162-202`).
3. Locks or atomics for what the slow path still does unlocked: `Heap::didAllocate` counters, `m_mutatorState`, `MarkedSpace::didAddBlock`
   (`m_blocks`, `m_capacity`), `didAllocateInBlock` (`m_newActiveWeakSets`), `tryAllocateBlock`'s unlocked `m_blocks.size()`
   (`BlockDirectory.cpp:149`), `m_numberOfLargerCells`, precise allocation registration, `IsoSubspace::m_lowerTierPreciseFreeList`,
   `CagedAlignedMemoryAllocator::m_numberOfBlocksToFillRequested`/`m_residentBlocks` (`heap/CagedAlignedMemoryAllocator.cpp:178-189`),
   `MarkedBlockCage::m_reserve` (`MarkedBlockCage.cpp:228-260`).
4. `stopThePeriphery()` runs on the conductor and rewrites every allocator's free list (`LocalAllocator::stopAllocating`, `LocalAllocator.cpp:102-114`).
   That is only sound if no thread is between the load and the add of 3.3, i.e. if threads stop at safepoints only.
5. Thread exit: `stopAllocatingForGood()` for each of its allocators; `~LocalAllocator` release-asserts an empty free list (`:79-100`).
6. C++ allocation (`allocateCell<T>(vm)`, `tryAllocateCell`, `Butterfly::create...`) goes `vm.xxxSpace()` -> the VM's one client/server allocator.
   Every runtime function that allocates needs the per-thread route as well.

---

## 4. The cage

### 4.1 Reservation

`MarkedBlockCage::initialize()` (`heap/MarkedBlockCage.cpp:117-152`), once per process, only if `Options::runsCompiledImage()` and
`markedBlockCageSizeInMB` (default `32 * 1024`, `runtime/OptionsList.h:153`: "the address space reserved for the cells of each heap, a power of two, at most
32768"). It reserves the first range and stores its size in `g_jscConfig.markedBlockCageSize`.

`tryCreateRange` (`:82-115`): `ReservedRangeHeap::tryCreate` with `alignment = maxSize` (32 GB), `InitialAccess::None`, random placement, and an unused
prefix of `minIndex << shift` = 16 MB plus a random multiple of 64 KB. `RELEASE_ASSERT(!(range->start() & (maxSize - 1)))`.
One page at `base + 8 MB` is made readable and stays zero: the "empty Extension" (`:62-74`, `aot/object/AOTCellFormat.h:90-91`).

### 4.2 Per heap or per process

Per `Heap`: `Heap::cagedAllocator` is a `unique_ptr<CagedAlignedMemoryAllocator>` (`Heap.h:1166`, `Heap.cpp:387`) that holds a `MarkedBlockCage`.
The constructor (`MarkedBlockCage.cpp:154-175`) takes a range from the process-wide pool `s_freeRanges`, or reserves a new one, or, if that fails,
**shares the first range**: `m_range = s_firstRange; m_sharesRange = true;`. The destructor returns the range to the pool (never unmapped;
`ReservedRangeHeap::~ReservedRangeHeap() = delete`).

Workers: each Worker is a VM, so a heap, so a cage of its own: N+1 reservations of 32 GB of address space.
`bun:docs/bundler/aot-compatibility.mdx:937`: "Each thread can have 32 GB of objects... Each `Worker` has a limit of its own."

`AddressSpaceBudget.h`: only under `RLIMIT_AS` on Linux. Half of the free address space may be reserved (`:48-57`), a quarter of that for the structure
heap, "All cages of the process together get the rest" (`:59-67`). Then ranges are growable: a small first segment (at most 1 GB) and more on demand
(`:69-79`, `MarkedBlockCage.cpp:136-146,165-166`).

`ReservedRangeHeap` has its own lock and a malloc-like back end; `tryAllocateAligned`, `tryAllocate`, `free` are callable from any thread (the warm-up helper
thread does: `CagedAlignedMemoryAllocator.cpp:133-162`, "The cage has a lock of its own"). `m_lowestAllocation`/`m_endOfHighestAllocation` are atomics.

### 4.3 Cell32

`heap/MarkedBlockCage.h:40-50`
```cpp
static constexpr unsigned shift = 3;
static constexpr uint64_t maxSize = 1ull << (32 + shift);
static constexpr uint32_t minIndex = 1 << 21;
static uintptr_t baseOf(const void* pointerInCage) { return bits & ~(maxSize - 1); }
static uint32_t encode(uintptr_t base, const void* cell) { ... return static_cast<uint32_t>(bits >> shift); }
static JSCell* decode(uintptr_t base, uint32_t index) { return base + (uintptr_t(index) << shift); }
```
- Encode needs no base: shift right by 3 and keep the low 32 bits (`encodeCell32`, `aot/stubs/AOTStubHelpersValues.cpp:147-150`).
- Decode: `lea result, [r14 + word*8]` / `add result, x25, word, lsl #3` (`:138-145`). `cellCageBaseGPR` = r14 / x25 (`AOTStubRegisters.h:22,26`), pinned in every
  procedure (`aot/codegen/AOTPrimitiveEmitter.cpp:180-183`), loaded from `instance.cellCageBase` (`AOTInstance.h:114`, `AOTHeapHooks.cpp:104`).
- C++ and the marker use the holder's address instead of a register: `cageBaseOf(holder)`.
- The shift is 3, not 4, because a precise allocation's cell is 8 mod 16 (`heap/PreciseAllocation.h:68-70,159`).
- Words below `minIndex` (2^21) are not references: markers, literal-string words, deferred-closure words (`aot/object/AOTSlotRepresentation.h:69-87`).
  The word of an Extension is biased by 8 MB so that 0 decodes to the page of zeros.

### 4.4 What is caged

`Heap::cellAllocator()` returns the caged allocator when the cage is enabled (`Heap.cpp:443-446`). It serves `cellSpace`, `destructibleObjectSpace`,
`immutableButterflyAuxiliarySpace` (`:392-394`), every iso subspace without an allocator of its own (`heap/IsoSubspace.cpp:47`), and the AOT spaces
(`Heap.cpp:3557-3568`). Structures come from the process-wide structure heap (`:299,386`).

Precise allocations take memory from their subspace's allocator (`heap/PreciseAllocation.cpp:90`), so a large *cell* is in the cage and a large *butterfly* is
in malloc. `Collector::verifyGC` checks all three (`Collector.cpp:1110-1124`).

### 4.5 When it is full

`LocalAllocator::allocateSlowCase` (`LocalAllocator.cpp:193-211`), `CompleteSubspace::allocateSlow` (`CompleteSubspace.cpp:138-149`):
1. `heap.tryCollectForExhaustedCage()`: a synchronous Full collection, unless deferred (`Heap.cpp:2895-2901`).
2. `heap.tryUseReserveOfExhaustedCage()` (`:2903-2922`): frees three quarters of a reserve kept since the cage was half full
   (4 to 32 MB, `MarkedBlockCage.cpp:228-250`) and sets `vm.traps().setNeedsOutOfMemoryError(true)`, which is `m_stack.setStopsAtFunctionEntries(true)`
   (`runtime/VMTraps.h:300`, `runtime/StackManager.cpp:73-83`): the next function entry that checks the stack throws the `RangeError`.
3. The last quarter, with `willUseLastReserve(vm)`: "A host that can end this thread alone asks for its termination here."
4. `logExhaustion` and a crash.

The owed error is **per VM** (one flag, one stack-limit word mirrored into each Instance).

### 4.6 `--aot-huge-heap`

`RecordedOptions::useWideReferences()` (`aot/support/AOTCompileOptions.h:104`). Slots of kind `Cell32` become 64-bit; about thirty stubs branch on
`jit.options().useWideReferences`. `Bun::requireCellCage()` does not require the cage then (`bun:src/jsc/bindings/BunAOTRuntime.cpp:63-72`).
`MarkedBlockCage::isRequired()` stays false, so a failed cage allocation falls back to malloc (`CagedAlignedMemoryAllocator.cpp:191-199`).

**Inference for threads:** one shared heap = one cage = one base value for all threads, which is what the pinned register and `cageBaseOf(holder)` want.
The 32 GB limit becomes a limit for all threads of the realm together.

---

## 5. Write barriers

### 5.1 Cell states

`heap/CellState.h:31-47`: `PossiblyBlack = 0`, `DefinitelyWhite = 1`, `PossiblyGrey = 2`; `blackThreshold = 0`, `tautologicalThreshold = 100`.
A new cell's header carries `DefinitelyWhite` (from `structure->typeInfoBlob()`, `aot/object/AOTObjectWords.h:26-30`). A cell in a frame carries
`frameCellState = 255`: "No barrier takes it for the state of a marked cell." (`AOTCellFormat.h:54-55`).

`Heap::setMutatorShouldBeFenced(value)` sets `m_mutatorShouldBeFenced` and `m_barrierThreshold = value ? tautological : black`, and copies both into **every
Instance** (`Heap.cpp:3459-3467`, `AOTHeapHooks.cpp:149-157`). It is called from `beginMarking()` and `endMarking()` only (`Heap.cpp:882,982`), both with the world stopped.

### 5.2 What compiled code emits

After the store (`aot/codegen/AOTBarrierEmitter.cpp:157-192`):
- fast code shape: `load8 state, [owner+7]`; `load32 threshold, [instance.barrierThreshold]`; `if (state > threshold) continue; else call stub WriteBarrier`.
- other shapes: always call the stub, which makes the same test first (`aot/stubs/AOTStubsBarriers.cpp:23-27,90-108`).

B3 sees both loads in the abstract heap `collectorState`, which only operations with `mayCollect` (or barrier stubs) write
(`aot/codegen/AOTRuntimeCallEffects.cpp:91-98`, `AOTHeaps.cpp:442-447`). So the threshold may be kept in a register between two `collects` operations.

The stub (`returnUnlessOperationRemembers`, `AOTStubsBarriers.cpp:40-70`):
1. If `instance.mutatorShouldBeFenced`: `memoryFence`; reload the state; return unless it is `PossiblyBlack`; else go to the C++ operation.
2. Not fenced, x86-64 release builds only, in line ("What Heap::addToRememberedSet() does while no collector runs beside the program"):
   ```
   stack = instance.heap.mutatorMarkStack
   top   = stack->m_top
   if (top >= segmentCapacity) goto operation
   stack->m_top += 1                      ; add to memory
   seg   = stack->m_segments.head
   seg->data[top] = owner
   owner->cellState = PossiblyGrey        ; store8
   *instance.heap.barriersExecuted += 1
   ```
   Layout from `aot/runtime/AOTMutatorMarkStackLayout.h`; the three pointers are `Instance::heap` (`aot/runtime/AOTHeapWords.h:19-23`), filled by
   `initializeCollectorWords` (`AOTHeapHooks.cpp:100-110`).
3. Otherwise `operationAOTBarrierSlow` -> `vm->writeBarrierSlowPath(cell)` (`AOTAllocationOperations.cpp:147-150`) -> `Heap::writeBarrierSlowPath` (fence, recheck) ->
   `addToRememberedSet` (`Heap.cpp:1348-1404`): while marking, an unmarked black cell is re-whitened by CAS with a correction; otherwise
   `setCellState(PossiblyGrey); m_mutatorMarkStack->append(cell)`.

The mark stack is merged into the collector's visitor by `MarkStackMergingConstraint` while stopped (`heap/MarkStackMergingConstraint.cpp:60-75`).

The collector is a retreating-wavefront (re-scan the owner) collector: the visitor sets the cell black, `storeLoadFence`, then reads the fields
(`SlotVisitor.cpp:455-458`); the mutator stores the field, fences, then reads the state. No snapshot-at-the-beginning barrier, no read barrier.

### 5.3 Other kinds of barrier

- **Statics (module variables).** No owner cell. A store of a possible cell marks a card byte: `store8 1, [statics + cardsOffset + (offset >> 8)]`
  (`markCardOfStatics`, `AOTBarrierEmitter.cpp:179-185`; `InstanceLayout::cardShift = 8`).
- **String-constant cells**: a stub allocates the `JSString`, stores its `Cell32` into `instance.stringCell(n)`, marks a card (`aot/stubs/AOTStubsConstants.cpp:92-114`).
- **Elements of big arrays**: `elementBarrierAfterStore` -> stub `RememberElement` (5.5).

### 5.4 RememberedSlot / RememberedSlotSet (`heap/RememberedSlot.h`)

A root table outside the heap (site caches, string cells, closure cells, property caches, statics) with a byte per region ("card"):
`neverWrittenCard = 0`, `writtenCard = 1`, `scannedCard = 2`. `RememberedSlot<T>::set(set, value)` stores the value, then `set.record(this)` (`:125-129`).
`record` (`:47-59`) sets the card and appends its number to `m_writtenCards` (a `Vector`), up to a limit. "Compiled code stores into the cards of such a set
itself, so only the cards say what was written." (`setCardsAreWrittenWithoutRecord`, `:61-66`).

Readers, world stopped (asserted, `:216-220`): `forEachRegionToVisit` visits written cards (all non-zero cards on the first visit of a Full collection) and sets them
`scannedCard`; `forEachRegionToSweep`/`forEachRegionToClear` at finalization. `tryDeferFullVisit` (`:237-270`) turns the first full visit into
`DeferredRootRegion`s that any marker thread takes later (`SlotVisitor.cpp:828-834,921-944`), **possibly while the mutator runs**; what the mutator writes meanwhile is
caught by its card at the next stop.

Writers: C++ run time (`record`, not thread safe: check-then-append on a `Vector`), compiled code and stubs (a plain byte store, idempotent).

The registry: `CacheRegistry` per VM and per realm (`aot/runtime/AOTHeapCacheRegistry.h`), kinds `StrongRoots`, `WeakByKey`, `ClearedEachCollection`, `Observer`.
Two constraints drive it (`AOTHeapHooks.cpp:74-98`): "Ao" (GreyedByMarking) and "At" (GreyedByExecution, "Static Variables of Compiled Code"). `visitRoots` takes the
registry's lock; `finalize` (from `Heap::reconcileWeakReferencesAtGCEnd`, `Heap.cpp:743-746`) does not.

### 5.5 StoredCellLog (`heap/StoredCellLog.{h,cpp}`)

"An old array may hold a young cell without being remembered if the cell is in the log. The log's cells are roots of the next Eden collection." (`StoredCellLog.cpp:22`).
It avoids re-scanning a huge old array for one stored young cell.
- Used only if the owner's `vectorLength >= 1024` and no marking is in progress (`Heap::writeBarrierSlowPath(from, to)`, `Heap.cpp:2881-2886`).
- Stub `RememberElement` (`AOTStubsBarriers.cpp:118-156`): not fenced; vector length test; skip if the value is no cell or is black;
  `cursor = log->m_cursor; if (cursor == log->m_end) slow; *cursor = value; log->m_cursor = cursor + 8`.
- C++ `tryRemember` (`StoredCellLog.cpp:44-60`): the same; `m_lock` is taken **only around `grow()`** (a `fastRealloc`).
- Reader: the "Msr" constraint, `visitAndReset` under the lock, world stopped (`Heap.cpp:3216-3219`, `StoredCellLog.cpp:71-91`).

### 5.6 Which barriers `represent/` removes, and why that is sound

`aot/represent/AOTRemoveRedundantBarriers.cpp`. Three decisions, carried as flags on the store.

"Safepoint" for this pass (`:77-90`): `effects.mayCollect || effects.mayRunOtherCode()`, on the normal or the handler edge.

**(a) `OwnerCannotBeMarked`, frame cell** (`:353-356,368-373`; also decided in `BarrierEmitter::ownerIsOnlyInFrames`, `AOTBarrierEmitter.cpp:69-92`: "Such a cell may have no
header, so no code may read its cell state."). The owner lies in a machine frame; the collector never visits it; its fields are found by the stack scan.

**(b) `OwnerCannotBeMarked`, young heap allocation** (`YoungOwnersProblem`, `:113-159`; `isStoreToYoungOwner`, `:404-418`). All of:
1. the owner is defined in this function by an operation with `allocatesAfterLastSafepoint` (`NewObject`, `NewPromise`, `CreatePromise`:
   `aot/ir/schema/operations.def:47`, `intrinsic-operations.def:31-32`), and for `NewObject` it `allocatesUnmarkedCell` (`:382-388`: not into a free cell of an outer object, no
   prototype operand, not taking an inner object's cell);
2. forward dataflow: on **every** path from the allocation to the store no value or terminator reaches a safepoint, and no handler edge is taken ("allocatesThrownError");
3. the store itself reaches no safepoint before it stores;
4. escape analysis knows the object as an `Allocation`, and `retentionHistory().occurredBefore(object, store) == HasNotOccurred`: **nothing has retained it yet**.

The argument: a cell that no heap word and no table names can be reached by a marker only through the stack scan, and the stack is scanned only when this thread is stopped,
which happens only at a safepoint. So it is still white.

**(c) `LaterBarrierCovers`** (`findCoveredStores`, `:431-510`). Backward dataflow: on every path from the store, a later store to the *same owner value* always runs an owner barrier
(`alwaysRunsBarrier`), before any operation that `endsCoverage`: a safepoint, a sideways exit, `Return`, `ReturnCall`, `Throw`, `Rethrow`, `Unreachable`.

The argument: marking cannot *terminate* without stopping this thread, so a white cell stored into a black owner is safe as long as the owner is re-greyed before this
thread's next safepoint.

`--aot-validate` checks both at run time: (b) fails if the owner's state is black at the store; (c) parks the owner in `instance.scratch.pendingBarrierOwner` and fails if it is still there before any
`mayCollect` call or `LoopHint` (`AOTBarrierEmitter.cpp:215-273`, `AOTHintEmitter.cpp:146-147`).

### 5.7 Would a second mutator or another thread's collection break it?

- (a): a frame cell is thread-private as long as it cannot escape to another thread. That is `AllocateInFrames`' proof, not the collector's (R2/R3).
- (b): condition 4 makes the owner invisible to other mutators too. A collection that another thread starts must stop this thread for Begin, and again for every Fixpoint. **If this
  thread can be stopped only at its own `collects` operations, the argument holds unchanged.** If it could be suspended at an arbitrary instruction (signal), the owner in a register
  would be marked and blackened concurrently, and the barrier-free store after the resume would hide a white cell: use after free.
- (c): the same. Also sound if another mutator stores into the same owner in between: each thread covers its own stores.
- The threshold cached in a register (5.2) changes only while every mutator is stopped at a `collects` operation. Same condition.

**Inference:** the design rule is "a JavaScript thread parks for the collector only at an operation whose IR effects say `collects`". Any new poll must be an IR operation with that effect
before `represent/` runs. `LoopHint` already is one (section 8).

---

## 6. Concurrent marking versus the mutator

### 6.1 The general protocol

`SlotVisitor::visitChildren<mutatorsAreStopped>` (`heap/SlotVisitor.cpp:447-468`): set black; `storeLoadFence` unless stopped; `structure = cell->structureID().decontaminate().decode()`;
`structure->cellVisitKind()`; inline visit or method table. Only the collector's own visitor ever reports `mutatorsAreStopped()` (1d86f8086971; `Collector.cpp:852,863`).

A visit that loses a race calls `visitor.didRace(cell, reason)`: the cell goes on `Collector::m_raceMarkStack` (`SlotVisitor.cpp:1033-1040`) and is visited again at the next stop.

With `useAOT`, `drainStack` visits common kinds in line (`heap/CellVisitKind.h:28-46`, `InlineCellVisits.h`, `FastMarkAppender.h`); `CellVisitValidator` compares with the method table under `verifyGC`.

### 6.2 AOT objects

`InlineCellVisit<CompiledObject>` (`aot/runtime/AOTHeapCellVisits.h:19-31`): append the structure; `layout = object->layout()` = `compiledStructure().layout()` (`aot/object/AOTObject.h:135`);
`visitReferences(object, layout, layout.hasExtension(), appender)`.

`forEachReference` (`aot/object/AOTObjectVisiting.h:82-120`) walks a **read-only table in the image**: three counts, then byte offsets of 64-bit value slots and of `Cell32` slots. Each slot is read once, as one
aligned word. A `Cell32` word below `minCell32` is skipped. No cell lock, no double read of the structure.

Why that is enough: every structure an `AOT::Object` can have shares its layout (the shape never changes: "A compiled object stays one", PR text), so a stale structure gives the same offsets. Each slot has one fixed
representation for the life of the object. A torn view across slots does not matter to a marker.

### 6.3 Extensions (out-of-line slots)

Format (`AOTCellFormat.h:83-100`): a 64-bit word of granule bits, then the held 8-byte granules **packed in ascending order**. The position of a slot is `popcount(bits & (bit - 1))`.

The marker: read the owner's hidden word once; mark the auxiliary cell; "Read once: the mutator may set bits of granules that come after these, and those hold nothing that this visit could miss."
(`AOTObjectVisiting.h:100-119`).

The mutator (`Object::ensureExtensionGranules`, `aot/object/AOTObjectStorage.cpp:182-207`; stub `growExtensionAndReturn`, `AOTStubsAllocation.cpp:391-515`):
1. `tryAppend`: new granules above all held ones, room left: just OR the bits ("It is zero since the Extension was created, so the collector may see the bits at any time", `AOTObject.h:355`).
2. `tryInsert`: granules between held ones: **the granules behind them are moved up in place**, then the bits are stored. "Only while no thread of the collector visits cells." (`AOTObject.h:357`). Taken
   only if `!vm.heap.mutatorShouldBeFenced()`, justified by "Marking begins and ends while the mutator is stopped, so it does neither before the caller has returned." (`AOTObjectStorage.cpp:182`).
   The stub tests `instance.mutatorShouldBeFenced` (`AOTStubsAllocation.cpp:433`).
3. Otherwise a copy: allocate, copy, `storeStoreFence`, store the owner's word, `writeBarrier(owner)` (`setExtension`, `:209-221`). "the old Extension stays as it is."

### 6.4 Extra properties, environments, closures, tables, saved frames

- Extra properties: an ordinary `JSFinalObject` named by a slot or by the structure. `setExtraProperties` (`aot/object/AOTObjectExtraProperties.cpp:644-655`): write the word; `mutatorFence`; set
  `orderWordHasExtraProperties`; `mutatorFence`; `setStructure`; barrier. The side object is visited by JSC's ordinary protocol.
- `Environment`: like an object, without Extension (`aot/object/AOTEnvironment.h:54-57`).
- `SlimClosure` can change class in place (`becomeReified`, `aot/object/AOTSlimClosure.cpp:178-195`): nuke the structure ID; fence; overwrite `m_word` with a butterfly pointer; fence; set the new structure; barrier. The marker's
  `tryVisitFields` (`AOTSlimClosure.h:94-110`) reads ID, type, word, ID again with `loadLoadFence`s, and gives up (falls back to the method table) on any mismatch. "Not uncheckedDowncast, which asserts the class: the mutator may be reifying this cell right now." (`AOTHeapCellVisits.h:106`)
- `CompactTable` (storage of compiled `Map`/`Set`): the marker holds the **cell lock**, reads `m_used`, fence, visits entries (`aot/runtime/AOTCompactTable.h:528-543`). The C++ mutator takes the cell lock where it restructures (`AOTCompactTable.cpp:396,690`).
  The stub that grows a table gives up while marking: "No marker reads a table meanwhile." (`aot/stubs/AOTStubsCollections.cpp:621-622`).
- `SavedFrame` (a suspended coroutine's copied machine frame): precise, by a live-slot map from the image, under a **sequence lock**: `m_version` odd = being written; read version, fence, collect cells into local vectors, fence, compare;
  on a mismatch `didRace(saved, "a SavedFrame is being written")` (`aot/runtime/AOTSavedFrame.h:271-316`).
- `StringBuilderStorage`, direct-eval caches: cell lock.
- Ordinary objects: structure / butterfly / structure with nuking (`runtime/JSObjectVisitInlines.h:100-...`, the long proof at `:122-200`). Note `JSObject::setButterfly` and `nukeStructureAndSetButterfly` skip the nuke and the fences on ARM64
  when `!mutatorShouldBeFenced()` (`runtime/JSObjectInlines.h:110-133`).

### 6.5 Publication of new cells

Compiled code initializes a cell inside an `InitializationScope`, within which no call may collect or throw: `RELEASE_ASSERT(!generator().isInitializingCell() || (!effects.mayCollect && !effects.mayThrow))`
(`aot/codegen/AOTRuntimeCallEffects.cpp:74`, `AOTRuntimeCallEmitter.cpp:655`). Then `fenceAfterInitialization()` = `mutatorFence()` (`aot/codegen/AOTPrimitiveEmitter.cpp:154-168`):
- x86-64: a compiler-only fence for B3.
- ARM64: `if (instance.mutatorShouldBeFenced) dmb`. **No fence when no collection is marking.** The same in stubs (`fenceAfterAllocation`, `AOTStubHelpersAllocation.cpp:67-76`) and in C++ (`Heap::mutatorFence`, `HeapInlines.h:123-134`).

### 6.6 Inner cells

An object in `aotOuterCellSpace` holds other cells inside its own cell (`FlattenInnerObjects`). An inner cell has its own header and its own mark bit; marking it marks the outer cell (`SlotVisitor.cpp:297-315`). The outer visit marks and visits named inner
cells in place (`AOTHeapCellVisits.h:33-98`). Inner cells with a header and no mark are noted under a lock (`noteInnerCellsWithoutMark`, `AOTHeapHooks.cpp:185-196`), and at the end of marking, world stopped, those still unmarked **lose their header**:
`*reinterpret_cast<uint64_t*>(inner) = 0` (`didFinishMarking`, `:198-207`), "so that no later scan of a stack takes it for a cell".

### 6.7 The pattern to take away

`mutatorShouldBeFenced` is used as "is anybody else reading this memory right now?". Every use in the tree (grep):

| Where | What is skipped or allowed when it is false |
| --- | --- |
| `AOTObjectStorage.cpp:185,197`, `AOTStubsAllocation.cpp:433` | granules move in place inside an Extension |
| `AOTStubsCollections.cpp:622` | a stub grows and re-indexes a `CompactTable` without the cell lock |
| `AOTStubsBarriers.cpp:43,128` | in-line append to the mark stack; the stored-cell log |
| `AOTPrimitiveEmitter.cpp:161`, `AOTStubHelpersAllocation.cpp:72`, `HeapInlines.h:132` | ARM64: no store-store fence after initializing a cell |
| `runtime/JSObjectInlines.h:112,124`, `JSObject.cpp:1514` | ARM64: no nuke, no fence around a butterfly change |
| `runtime/ButterflyInlines.h:240`, `runtime/JSArray.cpp:68` | a large butterfly is **`realloc`ed in place**, i.e. the old memory may be freed at once |

---

## 7. Conservative scanning

### 7.1 When, and by whom

The "Cs" constraint (`Heap.cpp:3135-3185`), volatility `GreyedByExecution`: it runs once per stop (`lastVersion == m_collector->m_phaseVersion` skips repeats), `ASSERT(worldIsStopped())`. It builds a `ConservativeRoots`, calls `gatherStackRoots` and
`gatherVMRoots` (JIT scratch buffers only), then `visitor.append(conservativeRoots)`.

`Heap::gatherStackRoots` (`:847-854`) -> `MachineThreads::gatherConservativeRoots(roots, ..., m_collector->m_conductingMutatorState, m_collector->m_conductorThread)` (`heap/MachineStackMarker.cpp:227-244`):
1. If a mutator conducts: scan its saved `RegisterState` and its stack from `stackTop` to `stackOrigin` (`gatherFromConductor`, `:45-54`). The state was captured in `callWithCurrentThreadState` (2.2 step 6).
2. For **every other thread in the group** except the current thread and the conductor: `thread->suspend()`, copy registers and the stack from its stack pointer (minus the red zone) to the origin into a malloc buffer, `resume()`
   (`tryCopyOtherThreadStacks`, `:159-216`). A process-wide `suspendLock` serializes VMs. On Linux `suspend` is `pthread_kill(sigThreadSuspendResume)` + a semaphore (`WTF:posix/ThreadingPOSIX.cpp:697-730`); on Darwin `thread_suspend`.
3. Scan the buffer.

So N stacks are already handled, *for threads that are not running JavaScript at that moment*. Today the others are threads that once held the JSLock.

### 7.2 What a word can be

In an AOT process `genericAddSpan` goes to `genericAddSpanOfCompiledProcess` (`heap/ConservativeRoots.cpp:231-352`). For each aligned 64-bit word:
1. `addNarrowReferences` (`:291-299`), if `useNarrowReferenceScan` (default true, `OptionsList.h:154`) and the cage is required (`:58-59`): the low half and the high half are each tried as an index within `cage.usedIndices()`; it must name exactly the start of a live cell (or of an inner cell, or a
   known precise allocation) (`:260-286`). Also: "Compiled code may keep a reference live as (index << shift) plus an offset into the cell, before it adds the cage base": a word below 32 GB is tried as `base + word`.
2. `addStructureOfFrameCell` (`:301-313`): a word whose type byte is `CompiledObjectType` and whose state byte is `frameCellState` is the header of an object in a frame; its `StructureID` is decoded and added. ("The collector visits no object that lies in a frame, so such an object keeps its Structure alive
   only if the scan of the stack takes its header for a reference.", `aot/object/AOTStructureTable.cpp:42-43`.)
3. The ordinary test (`genericAddPointer`, `:92-229`): bloom filter, block set, `isLiveCell(markingVersion, newlyAllocatedVersion, isMarking, p)`; interior pointers are aligned down to the cell; butterfly end pointers; precise allocations by binary search. In a block with inner cells the inner cell around the
   address is added too (`:214-220`).

"This function must be run after stopThePeriphery() is called and before liveness data is cleared to be accurate." (`:90-91`): liveness of a cell in a block that is being allocated from is known only after `stopAllocating()` has turned the free list into newly-allocated bits.

### 7.3 Compiled frames, frame objects, saved frames

- A compiled frame has no `CodeBlock` and no map for the collector. It is just stack words. `codegen/AOTFrameMaps.*` serve `SavedFrame`s only (roots at suspension sites).
- Objects in frames are scanned as the words they are; their `Cell32` fields are found by the half-word test. That makes `useNarrowReferenceScan` load-bearing for them.
- In SSA values the compiler does not rely on the half-word test: "The collector does not see a narrow reference: every use has to come before it can run." (`aot/represent/AOTChooseRepresentations.cpp:409`); "A narrow reference lives from a load to its Decode." (`AOTRepresentationWebs.cpp:521`,
  `isReachedBeforeCollectorCanRun` `:496-515`, first `mayCollect` operation per block `:434-445`). The option's text: "Tests turn it off to show that compiled code keeps a pointer next to each such reference."
- A narrow reference cannot name a frame cell: "a cell of a frame lies outside of the cage" (`AOTRepresentationWebs.cpp:391`).
- Saved coroutine frames are heap cells, visited precisely (6.4).
- Values parked in Instance scratch words are deliberately not roots ("Not a root: the stub ReturnMisfit leaves the value here, and the landing takes it before anything else runs", `aot/codegen/AOTCodegenScratchWords.h:27-35`).

### 7.4 What scanning N mutator stacks needs

**Inference:**
1. Every JavaScript thread registered in the heap's `MachineThreads` (one call, `addCurrentThread()`), and removed at exit (the `ThreadGroup` does that).
2. All of them parked at safepoints before "Cs" runs. Then the existing suspend-copy-resume loop works as it is: a parked thread's callee-saved registers are in the signal context or already spilled. Cost: two signals per thread per stop, under a process-wide lock.
   The cheaper variant is what the conductor does for itself: each thread captures `CurrentThreadState` as it parks and publishes it; the conductor scans in place.
3. `sanitizeStackForVM` (zeroing the dead stack below the deepest point) uses one `VM::m_lastStackTop` and release-asserts that it lies in the current stack (`runtime/VM.cpp:1847-1863`). It must become per thread. `Thread::savedLastStackTop()` already exists per thread (`VM.cpp:799-803`).
4. `m_conductingMutatorState`/`m_conductorThread`/`m_conductingHeap` are single, which is right: one conductor.

---

## 8. Safepoints

### 8.1 How the mutator is stopped today

Never by force. The collector changes `m_worldState`; the mutator notices at its next poll:
- every allocation slow path (`collectIfNecessaryOrDefer`), every `reportExtraMemoryAllocated` that crosses a block's worth (`Heap.cpp:617-631`);
- `DeferGC` scope exits when work was deferred (`HeapInlines.h:180-209`);
- explicit `stopIfNecessary()` calls: the sweeper (`IncrementalSweeper.cpp:97`), Bun's event loop, `acquireAccess`;
- the `StopIfNecessaryTimer` on the run loop (disabled in Bun).

The only signals in the collector are those of 7.1 step 2, for threads that are not the mutator.

### 8.2 A compiled loop that never allocates or calls

Nothing stops it for the collector. With one mutator that is harmless: the thread that would have to stop is the only one that could want memory, and marking helpers just wait (`SlotVisitor.cpp:878-894` lists who will notice termination).
**With N mutators it is a liveness hole:** thread A needs a collection, thread B spins; A waits forever at the stop barrier.

### 8.3 The polls that compiled code already has

`LoopHint` (`aot/ir/schema/operations.def:150`):
```
operation LoopHint () [| engine isVersioned] -> none ; UsesRuntime terminates collects ; runtime = PollSlow ; flow = TableProves
```
Its declared effects include **`collects`**, so barrier removal, narrow references, cell initialization and B3's `collectorState` heap already treat every loop poll as a safepoint.
`HintEmitter::emitPoll` says so: "Handling a trap stops the thread for the collector or ends it. It looks at no object of the program." (`aot/codegen/AOTHintEmitter.cpp:165-189`).

Two forms:
- Linux x86-64 (`hasFaultingLoopPolls`, `aot/image/AOTCodeSites.h:46-51`): one 7-byte load `mov r11d, [r13 + offsetOfLoopPollPage (+4 if frameless)]` (`AOTHintEmitter.cpp:34-45`). The page lies 1 MB below the Instance (`aot/runtime/AOTInstanceLayout.h:27-28,45`). To arm: `mprotect(page, PROT_NONE)` for every Instance of the VM
  (`setLoopPollsEnabled`, `aot/runtime/AOTRuntimeHooks.cpp:141-152`). The SIGSEGV handler looks the pc up in the image's fault table, pushes a return address (and a frame if the function has none) and redirects to the stub `pollAtLoop` (`aot/image/AOTFaultHandler.cpp:148-172,193-228`).
- elsewhere: `test64 [instance.stackMirror.trapAwareSoftStackLimit], sign` and a call on a late path (`AOTHintEmitter.cpp:47-59`).

Function prologues compare the stack pointer with `instance.stackMirror.trapAwareSoftStackLimit`, which a stop request sets to a marker that is above every stack (`runtime/StackManager.cpp:36-49`); stub `PollAtPrologue` (`aot/stubs/AOTStubsPolls.cpp:55-100`).

Who arms them: `StackManager::requestStop()`, from `VMTraps::requestThreadStopIfNeeded` (`runtime/VMTraps.cpp:396-419`), i.e. any async trap: termination, watchdog, debugger break, `NeedStopTheWorld`. **Not the collector.**

What the slow paths do:
- `operationAOTPollSlow`: `vm.traps().handleTraps(VMTraps::AsyncEvents); vm.throwTerminationExceptionIfNeeded();` (`aot/runtime/AOTExceptionOperations.cpp:52-58`). No `heap.stopIfNecessary()`.
- `operationAOTPollAtPrologue` (`:60-72`) holds a `DeferGCForAWhile`: "What a caller knows about a callee that collects nothing holds for the callee's prologue too." **A prologue poll is deliberately not a safepoint.**

Loops without a poll: `generateLoopHint` omits it when `endsWithinLengthOfData(block)`: an innermost loop whose counter rises by a constant up to an array, string or typed-array length and whose likely paths always complete (`AOTHintEmitter.cpp:102-150`). Bounded by the size of data, not by a constant.

Granularity: the poll words (`stackMirror`, the poll page) are per Instance, the request is per VM (`StackManager::m_mirrors`, `VMState::loopPollPages`). Arming them stops whoever runs in that VM, which for a collector stop is what one wants. But the same `Mirror` also holds `m_softStackLimit`, the **stack limit of the one
thread** (`StackManager.cpp:86-102`): per-thread data in a per-realm structure (R6's subject).

### 8.4 Why a thread must not be suspended at an arbitrary instruction

Read facts that each forbid it:
1. Between the bump of 3.3 and the store of the header a cell holds old bytes. After `stopAllocating()` it counts as newly allocated, so a stack word makes it a root, and `appendJSCellOrAuxiliary` dies on "GC scan found corrupt object: structureID is zero!" or decodes garbage (`SlotVisitor.cpp:188-198`).
   "We must not GC while an object is not fully initialized."
2. `stopAllocating()` on the conductor clears a free list that the suspended thread has half-updated.
3. Barrier removal (5.6), the cached threshold (5.2), narrow references in SSA values (7.3), `tryInsert` (6.3), values in Instance scratch words (7.3).
4. C++ run-time code holds raw pointers and half-built cells everywhere between its own allocation points.

### 8.5 What a stop of N mutators needs

**Inference:**
1. Per-thread access state (has access / parked / in native code without access), replacing the three bits of 2.1.
2. The stop request must arm the existing polls (`requestStop()`-like: write the mirrors, `mprotect` the pages), and `operationAOTPollSlow` must park for the collector.
3. Threads blocked in native code (I/O, `Atomics.wait`, locks, the event loop) must have released access, or they block every collection. Today only Bun's event loop does, and only sometimes.
4. Long-running C++ without allocation (a big `memcpy`, a RegExp match, `Array.prototype.sort` on numbers, JSON of a huge string) delays every other thread's collection. Compiled RegExp code and the omitted polls of 8.3 are in the same class.
5. The conn: exactly one mutator conducts; the others park. `Collector::assertConn` already speaks of "the conducting heap's mutator" (`Collector.cpp:956-963`).
6. The Concurrent phase's polling ("on every allocation slow path" of the conn holder) needs an owner: if the conn-holding thread blocks or exits, nobody re-enters Fixpoint.

---

## 9. Finalizers, weak references, sweeping, extra memory

### 9.1 End of marking, on the conductor, world stopped

`Collector::endCollectionInEachHeap` (`Collector.cpp:763-785`): `endMarking()`; verifier; then `pruneDeadReferences()` (`Heap.cpp:1011-1046`), which first sets the thread's atom table to null ("This doesn't necessarily run on this heap's mutator thread."), then:
`cancelDeferredWorkIfNeeded`, `reapWeakHandles` (`WeakSet::reap`: dead `WeakImpl`s change state, no callbacks), `reconcileWeakGCHashTables`, `sweepArrayBuffers`, `snapshotUnswept`, `reconcileWeakReferencesAtGCEnd`, compiled-code cleanup. Then `prepareForAllocation()` (`:1048-1070`): `m_sweeper->startSweeping`,
reset every `LocalAllocator`, `updateAllocationLimits()`.

`reconcileWeakReferencesAtGCEnd` (`Heap.cpp:739-802`): first `AOT::HeapHooks::finalize(vm, scope)` (drop dead Instances, `removeDeadEntries` of weak caches, zero the caches that are cleared each collection; `AOTHeapHooks.cpp:130-147`, `AOTHeapCacheRegistry.cpp:132-160`), then executables, symbol tables, code blocks, structures (Full only, `CompiledStructure`
included), `JSWeakSet`, `JSWeakMap`, `JSWeakObjectRef`, `ErrorInstance`, `JSFinalizationRegistry`, the VM, the embedder.

- **WeakMap/WeakSet**: ephemerons through the "O" output constraint over `m_weakMapSpace` (`Heap.cpp:3306-3309`); dead keys pruned here.
- **WeakRef** (`runtime/JSWeakObjectRef.{h,cpp}`): strong while `vm.currentWeakRefVersion() == m_lastAccessVersion` (`.cpp:50-54`: "This doesn't need to be atomic because if we are out of date we will get write barriered and revisit ourselves."); `deref()` updates the version and barriers (`.h:49-50`). The version is one counter per VM, bumped at the end
  of a job (`runtime/VM.h:1217`). Cleared here if unmarked (`.cpp:59-63`).
- **FinalizationRegistry** (`runtime/JSFinalizationRegistry.cpp:103-172`): under the cell lock moves dead registrations to lists and schedules `runFinalizationCleanup` on `vm.deferredWorkTimer`. "FinalizationRegistries currently rely on serial finalization because they can post tasks to the deferredWorkTimer, which normally expects
  tasks to only be posted by the API lock holder." (`Heap.cpp:789`). The callbacks run later as a task of the VM's run loop.

### 9.2 The epilogue, on "the" mutator

`setNeedCollectionEpilogue()` sets a bit (`Heap.cpp:2287-2292`); whichever thread next runs `handleNeedCollectionEpilogue` with access clears it by CAS and runs `runCollectionEpilogue()` (`:2265-2285,2356-2382`): `sweepPreciseAllocations()` (destructors of large cells), Wasm memories, clearing VM caches
(`hasOwnPropertyCache`, megamorphic cache, JSON, string caches...), dropping retained old string buffers not found on a stack, `GCCompletionCallback`s, optional synchronous sweep. The collector thread waits for it before the next stop (`waitWhileNeedCollectionEpilogue`, `:2294-2306`).

With N mutators the CAS picks one thread. But the epilogue touches VM-wide caches that other running mutators use, without locks.

### 9.3 Sweeping

`MarkedBlock::Handle::sweep(FreeList*)` (`heap/MarkedBlock.cpp:824-...`): `assertIsMutatorOrMutatorIsStopped()`, `SweepingScope` (writes `m_mutatorState`), `m_weakSet.sweep()` (**runs `WeakHandleOwner::finalize` callbacks**, `heap/WeakSet.cpp:90-128`, `WeakSetInlines.h:50-63`), then destructors of dead cells in destructible blocks, then builds the free list.
It requires the block's `inUse` bit. During marking it takes the block's header lock (`:889-890`).

Called from: (1) the allocation slow path of whichever thread claimed the block; (2) `IncrementalSweeper::sweepNextBlock` (`heap/IncrementalSweeper.cpp:95-127`), a `JSRunLoopTimer` on the VM's run loop, 10 ms slices, which may also free empty blocks (`freeBlock`: `m_blocks.remove`, unlocked);
(3) `sweepSynchronously`; (4) stealing; (5) shutdown.

So a destructor or a weak finalizer of an object runs on **any thread that allocates from that size class**, at an allocation point. Most AOT cells need none (`cellHeapCellType`: `DoesNotNeedDestruction`). Destructible: strings (they deref a `StringImpl`), `ModuleRecord`, `StringBuilderStorage`, structures, and Bun's native wrapper classes, whose
finalizers are Rust/C++ code written for the VM's thread (R8).

`WeakSet::allocate` (`WeakSetInlines.h:35-48`): the weak set belongs to the **block of the referent**; `m_currentBlock`, the `WeakBlock` free list and `heap.didAllocate` are unsynchronized. Two threads that make a `Weak<>` to cells of one block race.

### 9.4 Extra memory and ArrayBuffers

- `reportExtraMemoryAllocated(cell, size)` for `size > 256` (`Heap.h:507-516`) -> slow case (`Heap.cpp:609-632`): plain counters; `performIncrement`; if the cell may be black already, `reportExtraMemoryVisited` (CAS); a safepoint once per 16 KB reported.
- `reportExtraMemoryVisited` from visitors: per visitor, flushed per heap (94bf25067137).
- `addReference(cell, ArrayBuffer*)` (`Heap.cpp:716-724`): `GCIncomingRefCountedSet::addReference` pushes on plain vectors and `m_bytes +=`; `sweep` filters by `isMarked` while stopped (`heap/GCIncomingRefCountedSetInlines.h:47-116`).
- `extraMemorySize()` = `m_extraMemorySize + m_deprecatedExtraMemorySize + m_arrayBuffers.size()`, used by `updateAllocationLimits` while stopped.

### 9.5 Butterfly evacuation (Bun addition)

`Heap::evacuateSparseAuxiliaryBlocks` moves butterflies of sparse blocks while the program rests (`Heap.h:584-609`, `Heap.cpp:1577-...`). Contract: "No thread may read a butterfly without holding the JSLock." It looks at one stack only and **refuses** when `m_machineThreads->threads().size() > 1`: "more than one thread is registered with the VM" (`Heap.cpp:1570-1575`).
So with JavaScript threads it switches itself off.

---

## 10. Immortal cells and cells in the image

- **No JSCell lies in the image.** The image's 27 sections are read-only data and code (`aot/image/AOTImageFormat.h:53-85`). What heap cells *point to* in the image:
  - `StringImpl` headers and text (`StaticStringImpl`s; a `JSString` cell made at run time points at one; `AOTStubsConstants.cpp:77-125`);
  - copy-on-write array storage without references: "Copy-on-write storage without references may lie in read-only memory of the process. No cell stands in front of it, and the collector has nothing to do with it." (`runtime/JSCellButterfly.h:168-171`; the marker tests `isOutsideHeap`, `JSObjectVisitInlines.h:59-62`);
  - entries of constant `Map`s/`Set`s: the `CompactTable` is a heap cell with `isInImage()`; its visit returns at once (`aot/runtime/AOTCompactTableCreation.cpp:29-42`, `AOTCompactTable.h:537-538`);
  - layout, reference and live-slot tables that the marker reads (6.2, 6.4).
- **`ImmortalCells`** (`aot/runtime/AOTImmortalCells.{h,cpp}`), one per realm `State`: a `SegmentedVector<JSCell*, 512>` of ordinary heap cells that must live as long as the realm: structures, prototypes, literal strings, built-in cells, getter-setters (about 30 callers). `add()` appends under the registry's lock. It is a `StrongRoots` entry with
  `GreyedByExecution`. `visitRoots` visits everything on the first visit of a Full collection (as deferred regions, one per segment: "Needs no lock: a segment never moves, and these cells were added before the visit that deferred them.") and otherwise only what was added since the last visit (`m_numberOfCellsVisited`, `m_lastVisitVersion`).
  An Eden collection need not revisit old ones: their marks are sticky.
- Order of publication: "The collector does not read these tables, so the cell has its root before compiled code can find it." (`aot/runtime/AOTProgramStrings.cpp:241-250`: `immortalCells->add(cell)`, then the table word.)
- They are marked like any cell; nothing is pre-marked or exempt from sweeping logic.
- Instances themselves are malloc/mmap memory, kept alive by a `livenessToken` cell; dead ones are queued at finalization (`AOTHeapHooks.cpp:130-141`).

---

## 11. Hazards with several mutators

Severity: **M** = memory safety (wild pointer, use after free, type confusion, out-of-bounds write). **C** = crash by assertion. **L** = liveness (deadlock, unbounded wait). **V** = wrong value or wrong accounting only.

### 11.1 Allocation

| # | State / sequence | What goes wrong | Sev |
| --- | --- | --- | --- |
| A1 | `FreeList::m_intervalStart`: load, compare, add (3.3) on an allocator shared by two threads | both get the same cell: two objects in one memory | M |
| A2 | Refill: three separate stores of `nextInterval`, `intervalStart`, `intervalEnd` | a mixed interval: allocation over live cells | M |
| A3 | `AllocationSiteRecord` read as header+words (16 bytes) and allocator while `fillAllocationSite`, `predictStructure`, `forgetStructuresOf` rewrite it (struct assignment, 32 bytes, not atomic) | header of one structure with words of another; or `header == 0` with a real allocator | M |
| A4 | Lazy filling: `ensureRecord` (`++m_numberOfAllocationSiteRecords`, write distance), `prepareInlineAllocation` (`header` then `allocator`; readers test `allocator`), `typedArrayAllocations` | double fill is benign if idempotent; on ARM64 a reader can see `allocator` before `header` | M |
| A5 | `AllocatingScope` `RELEASE_ASSERT(m_mutatorState == Running)` | two threads in slow paths | C |
| A6 | `didAllocate` counters, `m_capacity`, `m_extraMemoryBytesSinceLastCollectionCheck` | lost updates: collections too late or too early | V |
| A7 | `MarkedSpace::m_blocks` (hash set + filter) in `didAddBlock`/`freeBlock` | corrupt hash table; the conservative scan misses a block | M |
| A8 | `m_newActiveWeakSets` splice in `didAllocateInBlock` | corrupt list | M |
| A9 | `m_preciseAllocations`, `m_preciseAllocationSet`, `Subspace::m_preciseAllocations`, `indexInSpace` | corrupt vector; lost large cell | M |
| A10 | `reallocatePreciseAllocationNonVirtual` (`CompleteSubspace.cpp:186-240`), taken when no marking is on | frees or moves a large butterfly that another thread is reading | M |
| A11 | `Heap::name##Slow()` lazy server subspaces; `CompleteSubspace::allocatorForSlow` is locked, the `unique_ptr` publication of subspaces is not | two subspaces, one leaked with live blocks | M |
| A12 | `HeapIterationScope` / `willStartIterating()` calls `stopAllocating()` from a running mutator | wipes other threads' free lists under them | M |
| A13 | `tryAllocateInLargerCell`: `m_numberOfLargerCells`; `CagedAlignedMemoryAllocator::m_residentBlocks` creation; `MarkedBlockCage::m_reserve` | double creation, vector race | M/V |
| A14 | Owed out-of-memory error is a per-VM flag read at any thread's next function entry | the `RangeError` is thrown in a thread that did not allocate | V |
| A15 | `tryCollectForExhaustedCage` = synchronous Full collection from inside a slow path, needs all threads stopped | L if another thread waits on something this one holds | L |
| A16 | ARM64: no fence between initializing a cell and publishing it when no marking is on (6.5) | another thread loads the reference and reads a stale header, shape word or field | M |
| A17 | x86-64 and ARM64: B3 and the stubs order initialization against *collector* visibility only; `InitializationScope` exempts those stores from other checks | a store that publishes the cell must not be moved above the initializing stores; today only `mutatorFence()` (a B3 fence) guarantees it | M |

### 11.2 Barriers and remembered sets

| # | State / sequence | What goes wrong | Sev |
| --- | --- | --- | --- |
| B1 | In-line append to `m_mutatorMarkStack` (5.2): read `m_top`, test, `add [m_top], 1`, store at the old index | two threads use one index: one owner is never re-scanned, an Eden collection frees what it points to | M |
| B2 | the same, at `top == capacity - 1` | `m_top` jumps over `s_segmentCapacity`; C++ `append` tests `m_top == s_segmentCapacity` (`heap/GCSegmentedArrayInlines.h:189-194`) and writes past the 4 KB segment | M |
| B3 | `StoredCellLog`: cursor read, store, cursor write; `grow()` reallocs under a lock that writers do not take | lost root (use after free after Eden), or a store into freed memory | M |
| B4 | `RememberedSlotSet::record`: test card, set card, `m_writtenCards.append` | vector corruption | M |
| B5 | Card bytes written by compiled code | idempotent byte store; safe. Needs the value store to be visible before the stop, which a stop barrier gives | ok |
| B6 | `cell->setCellState(PossiblyGrey)` by two threads, or against a marker | designed to be racy (comments at `Heap.cpp:1397-1402`) | ok |
| B7 | Re-whitening CAS in `addToRememberedSet` during a Full collection | written for one mutator against markers ("assumes ... that no other thread writes to it", FIXME `:1352`). Not analysed for two mutators | ? |
| B8 | `m_barriersExecuted` | statistics | V |
| B9 | Barrier removal (b), (c); cached `barrierThreshold` | sound only with cooperative stops at `collects` operations (5.7) | M if violated |
| B10 | `instance.scratch.pendingBarrierOwner` (validating builds) | one word per realm: false validation failures | C (validate only) |
| B11 | `didChangeBarrierThreshold` writes each Instance | fine while stopped; any per-thread copy must be updated too | M if missed |

### 11.3 "No marker is reading" used as "nobody is reading" (6.7)

| # | Sequence | What goes wrong | Sev |
| --- | --- | --- | --- |
| C1 | `Extension::tryInsert` / the stub: move granules up one by one, then store the bits | a reader computes `popcount(bits & ...)` with old bits over moved granules: reads a neighbour slot of another representation (a raw double as a `JSValue`, an `Int32` pair as a `Cell32`) | M |
| C2 | `tryAppend`/`appendGranule`: load bits, OR, store | two threads append different granules: one bit lost, its value with it | V, then M if a later proof says "slot filled" |
| C3 | Extension replacement: copy, publish | a store by another thread into the old Extension after the copy is lost | V |
| C4 | `CompactTable` growth in a stub without the cell lock | reader sees a half-built index | M |
| C5 | `SlimClosure::becomeReified`: the word at offset 8 changes from closure word to butterfly pointer | the marker's protocol is read-retry; compiled callers read the word once and jump through it | M |
| C6 | `SavedFrame` sequence lock has one writer | two threads resuming or saving one coroutine | M |
| C7 | ARM64: butterfly changes without nuke and fences | R7's subject | M |

These belong to the object-model reports; they are listed because the switch they hang on is the collector's.

### 11.4 Stopping, scanning, phases

| # | State / assumption | What goes wrong | Sev |
| --- | --- | --- | --- |
| D1 | one `hasAccessBit`; `acquireAccessSlow` `RELEASE_ASSERT(!(oldState & hasAccessBit))` | second thread crashes | C |
| D2 | "the conductor mutator is the stopped mutator" (2.2 step 7) | other mutators keep running through Begin/Fixpoint/End | M |
| D3 | no collector-driven poll in compiled loops; prologue polls defer GC; polls omitted in data-bounded loops; RegExp and long C++ loops | a stop waits without bound | L |
| D4 | threads blocked in native code with access held | every collection blocks | L |
| D5 | conn holder leaves (blocks, exits, waits for a lock held by a parked thread) during Concurrent | marking never terminates; or deadlock | L |
| D6 | `VM::m_lastStackTop`, `sanitizeStackForVM` release asserts | crash on the second thread | C |
| D7 | `Heap::collect`: `RELEASE_ASSERT(vm().atomStringTable() == Thread::currentSingleton().atomStringTable())` | every JS thread must have the VM's atom table current (and that table is unsynchronized: R7) | C |
| D8 | `m_deferralDepth`, `m_didDeferGCWork` shared | thread A's `DeferGC` blocks B's collections; unbalanced under races: GC inside a `DeferGC` scope | M |
| D9 | `DeferGC` means "no collection can *happen* here" | with N threads it must also mean "this thread does not park"; otherwise another thread's collection runs inside the scope. And then a long `DeferGC` scope delays every thread (L) | M |
| D10 | `m_mutatorSlotVisitor` and `m_incrementBalance` in `performIncrement` | two threads drain with one visitor | M |
| D11 | `needCollectionEpilogueBit`: one thread runs the epilogue while others already run | clears VM caches under running readers; `sweepPreciseAllocations` compacts `m_preciseAllocations` while others append | M |
| D12 | `mutatorWaitingBit` + `ParkingLot::compareAndPark(&m_worldState, ...)` as a condition variable for one waiter | lost wake-ups or spurious ones with several | L |
| D13 | suspension by signal of a *running* mutator in `tryCopyOtherThreadStacks` (it suspends every registered thread that is not the conductor) | if a registered thread is not parked, its stack is copied mid-instruction: 8.4 | M |
| D14 | `preventCollection()`/`allowCollection()` "do not nest", one flag and one held lock (`Heap.cpp:3428-3457`) | two threads that prevent collection at once | C/L |
| D15 | `m_protectedValues`, `m_markListSet`, `m_strongSet`, `m_weakGCHashTables`, `m_observers` | container corruption | M |
| D16 | Instance scratch words that hold cells and are "not a root" | clobbered by another thread; also invisible to a collection if the owner parks while one is live (it cannot today) | M |

### 11.5 Weak things and finalizers

| # | State | What goes wrong | Sev |
| --- | --- | --- | --- |
| E1 | `WeakSet::allocate`/`deallocate` per block, unsynchronized | corrupt free list of `WeakImpl`s | M |
| E2 | Destructors and `WeakHandleOwner::finalize` run on whichever thread sweeps | native finalizers with thread affinity run on the wrong thread; they may also run concurrently with each other | M/V |
| E3 | `SweepingScope` writes `m_mutatorState`; `collectIfNecessaryOrDefer` returns early when it reads `Sweeping` | another thread's sweep suppresses this thread's collection | V |
| E4 | `IncrementalSweeper::m_currentDirectory`, `freeBlock` from the run-loop thread | races with allocators that scan `m_blocks`; block claim is safe, `MarkedSpace::m_blocks` is not | M |
| E5 | `vm.m_currentWeakRefVersion` per VM, bumped at the end of *a* job | "kept alive until the end of the current job" is per agent in the standard; with one counter a WeakRef target may die in the middle of another thread's job | V |
| E6 | `JSWeakObjectRef::deref()`: test, write version, barrier | benign double write | ok |
| E7 | `JSFinalizationRegistry`: cell lock protects the lists; `m_hasAlreadyScheduledWork` and `deferredWorkTimer` assume the API lock holder | which thread runs cleanup callbacks is undefined | V |
| E8 | `GCIncomingRefCountedSet::addReference`, `ArrayBuffer::addIncomingReference` | vector corruption | M |
| E9 | `m_possiblyAccessedStringsFromConcurrentThreadsOrGCOwnedDataScope`: keeps replaced string buffers alive until no stack names the string; appended unlocked; cleared in the epilogue by "strings found on stack" | needs all stacks (it gets them from "Cs") and a lock for the append | M |
| E10 | `CacheRegistry::finalize` and `ClearedEachCollection` caches are zeroed while stopped | fine; but compiled code must not carry a value read from such a cache across a `collects` operation. It already may not (`weakTableStorage`, `weakTableEntries`, `collectorState` are "written by collection") | ok |
| E11 | `ImmortalCells::add` is locked; `m_numberOfCellsVisited` is written by the visit only | nothing | ok |
| E12 | `VMState::instances` is locked for visits, iterated unlocked in `finalize`/`willAnalyzeHeap` (`AOTHeapHooks.cpp:145-146,163-164`) | fine while stopped | ok |

---

## 12. What helps

1. **The collector is already concurrent and parallel.** Mark bits, `aboutToMark`/`resetStaleMarks`, block header locks, opaque roots, shared mark stacks, race stack, extra-memory counters are multi-thread safe. Nothing on the marker side needs to know how many mutators there are, except the two `heap()` uses in `SlotVisitor.cpp:893,966`.
2. **No moving.** Cells never move; butterflies move only in the evacuation that disables itself with more than one registered thread. So no read barrier, no forwarding, no pointer fix-up in N stacks.
3. **Owner re-scan barriers are per store and stateless**: two mutators that store into one owner need no coordination beyond private mark stacks.
4. **Conservative stacks.** No stack maps for compiled frames, so a thread may park at any `collects` operation without per-site metadata. `MachineThreads` scans any number of registered threads today.
5. **`collects` is an explicit IR effect**, declared in one schema, checked at run time by `--aot-validate` (operations that collect against their declaration, barriers that were needed, `OperationValidationScope`, `aot/runtime/AOTRuntimeValidation.cpp:435-464`). Every compiler assumption about the collector that I found is phrased in terms of it. In a JIT engine the same facts are spread over `doesGC`, clobberize and OSR.
6. **Loop polls exist, cost one load, and are already `collects`.** The arming machinery (mirrors, `mprotect`, fault table, `pollAtLoop` stub) is there and is VM-wide. No code patching, no invalidation points, no jettison.
7. **No JIT threads.** `useJIT = false` (`Options.cpp:815`): `suspendCompilerThreads` returns false, `JITWorklist`, `CodeBlockSet` marking, scratch buffers, `JITStubRoutineSet` drop out of every stop.
8. **AOT object layout is immutable and described by read-only tables**, so the marker needs no lock and no retry for objects and environments, and a racing mutator cannot make the *marker* misread a slot's type.
9. **`BlockDirectory` is prepared**: a list of `LocalAllocator`s per directory, per-allocator cursors, atomic block claim, read-write lock for growth, stop/resume over all allocators.
10. **The cage and the structure heap are thread safe** and the cage base is one constant per heap; `Cell32` encode needs no base; `StructureID`s are process-wide.
11. **The `Collector`/`Heap` split** marks the per-mutator state (1.6), and upstream is about to write "stop several mutators" in `finishChangingPhase`. Our stop barrier should be shaped so that a later merge of theirs is a small conflict: keep it in `Heap::stopTheMutator`/`resumeTheMutator` and below.
12. **Fast-path allocation touches no shared counter**: accounting happens per consumed free list in the slow path, so per-thread allocators scale without atomics on the fast path.
13. **All roots outside the heap go through one registry** (`CacheRegistry`) with declared kinds and volatility, and cards that tolerate racing writers.
14. **Most AOT cells need no destructor**, so lazy sweeping on any thread is harmless for them.
15. **The earlier JIT-threads branch has a frozen spec** for exactly this layer (`docs/threads/SPEC-heap.md`, `SPEC-congc.md`: client set, per-thread cache, per-client deferral depth and access state, conductor election, slow-path lock, lock order). Its biggest cost there, "shared-mode JIT allocation is slow-path-only", came from allocator addresses baked into JIT code. AOT code loads them from memory,
    so an inline per-thread fast path is a matter of which base register.

---

## 13. Not found / not sure

- **No support for several mutators on one heap** exists in this tree under any name I searched (`isSharedServer`, `ThreadLocalCache`, `GCThreadLocal`, `HeapClientSet`, `useSharedGCHeap`, `useJSThreads`): none.
- I did not find where upstream intends to stop several mutators (VMManager traps, or per-heap `m_worldState`). The commit messages only say it is not implemented. `VMManager`'s `StopReason::GC` is declared and unreachable.
- `Heap::addToRememberedSet`'s re-whitening path (B7): I read it, I did not work out whether two mutators racing on one owner during a Full collection can leave it white and marked. Upstream's FIXME says the code assumes one writer.
- `MarkedBlock::Handle::sweep` and `specializedSweep`: I read the head of `sweep` only. Whether every field of `MarkedBlock::Handle` (`m_isFreeListed`, `m_markingVersionAtLastSweep`, `m_decommittedPages`, `m_zeroPagesDuringSweep`) is covered by the `inUse` claim was not checked.
- `AlignedMemoryAllocator::findEmptyBlockToSteal` / `addDirectoryWithEmptyBlocks`: Keith Miller's commit covers them (fences, `VersionedField`); I did not verify the list `m_nextDirectoryWithEmptyBlocks`.
- `IsoCellSet`, `IsoSubspace::tryAllocateLowerTierPrecise`: has its own lock since 847b1d1f5e7f for the bits; the lower-tier free list looked unlocked, not read in full.
- `StrongSet`/`HandleSet` and `MarkedVectorBase`: taken as unsynchronized from their use under the API lock; their sources were not read.
- `WeakMapImpl`: no `cellLock` hit in my grep of `WeakMapImpl*`; how its buffer is protected against the marker during a rehash was not read.
- `GCActivityCallback` (Eden/Full timers) and Bun's `GarbageCollectionController`: only their call sites in `Heap` were read.
- Whether B3/Air can move a *publishing* store above initializing stores on x86-64 when `mutatorFence()` is only a B3 fence: the fence is `m_out.fence(&m_heaps.root, nullptr)`; I did not read how B3 lowers or orders it, nor the direct code generator's treatment.
- Windows and macOS: the faulting poll is Linux x86-64 only (`CPU(X86_64) && OS(LINUX)`); other platforms use compare-and-call. `Thread::suspend` on Windows was not read.
- How much stack a parked thread's scan costs, how long `mprotect` of the poll pages takes with many Instances, and how the space-time scheduler behaves with N allocating threads: no measurements exist.
- The pieces of `aot/passes/` that consult `mayCollect` for reasons of their own (`AllocateInnerObjects.cpp:440`, `FlattenInnerObjects.cpp:1078`, `InlineHeads.cpp:87`, `TypeTestElimination.cpp:1080`, `TypeTestConditions.cpp:41`) were only glanced at. In the first two, "collects" stands for "may allocate / may run the run time", not for a collector invariant.
- Whether `operationAOT*` functions that are declared not to collect can nevertheless block (locks, I/O). If one can, it is a place where a thread holds access without polling (D4).
