# AUDIT-heapcontainers — Heap/GC-infrastructure members under shared-heap N-mutator threads

Scope: every mutable member of `Heap`, `MarkedSpace`, `BlockDirectory` (+friends), SlotVisitor-shared
state, WeakSet/WeakBlock plumbing, HandleSet/HandleStack, MachineThreads, the
MarkedVector/MarkedArgumentBuffer machinery, DeferGC/DisallowGC counters, and other heap/-resident
containers reachable from MUTATOR paths when N mutator threads share one Heap
(`--useSharedGCHeap=1`, GIL-off per docs/threads/SPEC-ungil.md).

Motivation: DW-2 (Tools/threads/bughunt/deepwater/LEDGER.md) — `Heap::m_markListSet` is an
unsynchronized HashSet shared by all threads' MarkedVector spill paths; observed UAF. K4 audited VM
members, N7 audited cell classes; this audit closes the Heap/GC-infrastructure class gap.

Risk enum: `confirmed-race` > `likely-race` > `needs-trace` > `safe-synchronized` /
`safe-collector-only` / `safe-main-only`.

Sources merged and deduped: heap-core sweep (30 rows), marking/weak sweep (14 rows), mutator-entry
sweep (20 rows). Where two sweeps covered the same member, evidence is merged under one row; where
they disagreed (HC-06), the disagreement is recorded and the more pessimistic classification kept.

---

## Summary table

| Row | Member | Risk | Ruling (short) |
|---|---|---|---|
| HC-01 | `Heap::m_markListSet` + `MarkedVectorBase::m_markSet` (DW-2) | confirmed-race | per-client sharded set (preferred) or per-heap leaf Lock; NOT the limp global static |
| HC-02 | `Heap::immutableButterflyToStringCache` | confirmed-race | per-client cache (preferred) or gilOff skip; move behind accessor |
| HC-03 | `Heap::m_protectedValues` | likely-race | per-heap leaf Lock on protect/unprotect gilOff arm (HandleSet §F.3 pattern) |
| HC-04 | `Heap::m_weakGCHashTables` | likely-race | per-heap leaf Lock on register/unregister (cold) |
| HC-05 | `Heap::m_deprecatedExtraMemorySize` | likely-race | atomic (saturating CAS, F3 pattern) |
| HC-06 | GC pacing fields read by `VM::performOpportunisticallyScheduledTasks` | likely-race | relaxed atomics (single conductor writer, many readers) |
| HC-07 | `Heap::m_isInOpportunisticTask` | likely-race | per-client flag or atomic counter (SetForScope bool breaks under N clients) |
| HC-08 | `Heap::m_overCriticalMemoryThreshold` + cached-call counter | likely-race | relaxed atomics (Darwin-only path) |
| HC-09 | dynamic subspace registries (server `m_<name>` ptrs + `MarkedSpace::m_subspaces`) | needs-trace | if any direct server-accessor path is reachable GIL-off, lock the server Slow paths |
| HC-10 | `Heap::m_observers` / `m_heapFinalizerCallbacks` | needs-trace | Bun-side call-site check; leaf lock if off-main |
| HC-11 | `m_collectionScope` family + `m_shouldDoOpportunisticFullCollection` | needs-trace | relaxed atomic bool if off-main scheduling exists; document otherwise |
| HC-12 | `MarkedBlockSet` read by SamplingProfiler | needs-trace | snapshot under lock or thread-granular stop if HashSet reads confirmed |
| HC-13 | `MarkedVectorBase::m_storageForOutOfBoundsAccess` | needs-trace (benign UB) | `thread_local` one-liner |
| S-01..S-25 | see Safe section | safe-* | none / documented residuals |

---

## CONFIRMED RACES

### HC-01 — `Heap::m_markListSet` (UncheckedKeyHashSet<MarkedVectorBase*>) + `MarkedVectorBase::m_markSet` — DW-2 ground zero
*(merges heap-core-01, marking-weak-001, mutator-entry-marklistset)*

**Risk: confirmed-race** — observed UAF (LEDGER DW-2).

**State in this tree.** Heap.h:1209 declares `UncheckedKeyHashSet<MarkedVectorBase*> m_markListSet;`
with no lock; Heap.h:684 hands out the raw set via `markListSet()`. VERIFIED: the limp paper
(`MarkedVectorBase::s_limpMarkListSetLock`) exists ONLY in the /root/WebKit-limp worktree —
`grep -rn s_limpMarkListSetLock Source/` matches nothing here; the only hits are
Tools/threads/bughunt/deepwater/LIMP.md:67 and LEDGER.md:16. The unsynchronized HashSet ships on
jarred/threads.

**Mutator writers, every entered thread, no lock anywhere:**
- runtime/MarkedVector.cpp:31-42 `addMarkSet(JSValue)` → `m_markSet = &heap->markListSet(); m_markSet->add(this)` — reached from slowAppend (cpp:198-212) and expandCapacity (cpp:152-188) on every spill past inline capacity; ADDRESS32 twin at cpp:93-103.
- runtime/MarkedVector.h:447-452 and 476-481 `fill`/`fillWith` → `vm.heap.markListSet(); m_markSet->add(this)` — the exact DW-2 frame: `MarkedVector::fill` ← `sortImpl` ← `arrayProtoFuncSort`.
- runtime/MarkedVector.h:201-207 `~MarkedVectorBase` → `removeFromMarkSetAndDeallocateBuffer` → `m_markSet->remove(this)`.
- runtime/MarkedVector.h:209-246 `adopt` / move ctor: remove+add at h:223-224.

Every Thread's MarkedArgumentBuffer overflow path (sort, Function.apply, Reflect.apply, spread)
hits these. Under `useSharedGCHeap=1` all client VMs resolve to the ONE server set, and GIL-off N
lites share one VM outright. HashTable add/remove from two threads = rehash UAF — DW-2 observed
SEGV on a freed table (0xf5f5 scribble) in `HashTable::removeIterator`/`add` via
`MarkedVector::fill` ← `arrayProtoFuncSort` on a spawned Thread.

**Collector reader:** Heap.cpp:4000-4002 — `MarkedVectorBase::markLists` iterates the set inside
the Msr constraint, executed in the I5 stop window (world-stopped → collector-vs-mutator excluded).
The live race is mutator-vs-mutator add/remove.

**Ruling.** Two acceptable shapes; the LEDGER explicitly rejects the global limp static Lock.
1. **Preferred: per-client (HeapClient) sharded sets** — registration becomes thread-local with
   zero contention on the hot spill path (this sits on every big sort/apply spill per LEDGER §2);
   the collector unions/iterates all clients' sets at the world-stopped constraint (it already
   iterates per-heap state there). **Destructor-ordering caveat:** a MarkedVector can be destroyed
   on a different thread than it registered on (Thread teardown), so per-client storage must key
   off the *registering* client (store the owning set/struct pointer in `m_markSet`), not
   `currentThreadClient()` at removal time.
2. **Fallback: per-heap leaf Lock** — change `m_markListSet` to a small
   `struct { Lock lock; UncheckedKeyHashSet<MarkedVectorBase*> set; }` returned by `markListSet()`,
   and point `MarkedVectorBase::m_markSet` at that struct so destructor/adopt paths (which have no
   `Heap*`) can find the lock. Take it in: `addMarkSet` (both overloads, MarkedVector.cpp:31, :93),
   `removeFromMarkSetAndDeallocateBuffer` (h:201), `adopt` (h:221-226), and the inline registration
   in `fill`/`fillWith` (h:448-451, 477-480). Collector iteration at Heap.cpp:4002 runs
   world-stopped so the lock is uncontended there; take it anyway (belt-and-suspenders, turns a
   straggler add into a bounded wait — same rationale as MarkStack.h:65-70). Registration only
   happens when the vector has already left its inline buffer (FastMalloc slow path), so a leaf
   Lock is two uncontended atomics on an already-cold-ish path — acceptable, but sharded is better.
   Buffer-content marking needs NO lock (`markLists` reads `m_size`/`m_buffer` only in the stop
   window per the h:456-458 invariant, which still holds under either shape).

**Files:** Source/JavaScriptCore/heap/Heap.h, heap/Heap.cpp, runtime/MarkedVector.h,
runtime/MarkedVector.cpp, (per-client shape: heap/HeapClientSet.h).

---

### HC-02 — `Heap::immutableButterflyToStringCache` (public UncheckedKeyHashMap<JSCellButterfly*, JSString*>)
*(merges heap-core-02, mutator-entry-immutablebutterfly-tostring-cache)*

**Risk: confirmed-race** (by inspection; no special interleaving required).

**Evidence.** Heap.h:865 — public member, no lock, no gilOff gate. Mutator hot path on every
thread: runtime/JSArray.cpp:1242-1244 (unlocked `find`/`end` on the CoW fast path of
`Array.prototype.join`/`toString`) and JSArray.cpp:1254 (unlocked `add` after fastArrayJoin —
HashMap insertion, can rehash). Any two threads calling `join()`/`toString()` on copy-on-write
arrays concurrently perform unsynchronized HashMap mutation on the one shared Heap: add-vs-add
corrupts the table; find-vs-rehash is the same freed-table read DW-2 SEGV'd on. This is exactly the
workload class (array-heavy benchmarks) the deepwater dive ran — structurally identical mechanism
to DW-2 on a hotter path. Collector side clears at Heap.cpp:2925 (`didFinishCollection`,
world-stopped — that edge is fine once the mutator side is fenced). No K4/N7 row covers it; not in
LEDGER — precisely the class gap this audit exists for.

**Ruling.** Per-client cache is cleanest: it is a pure memo (per-thread duplication only costs
memory; join still works), cache locality preserved, no lock; cleared by the conductor at GC end
via the client registry (the GC clear already iterates heap state at a stop). Cheapest correct
alternative: skip the cache entirely when `vm.gilOff()`. A per-heap lock works but serializes a
benchmark-visible fast path; if locked, hold across `find` and across `add` separately (a lost add
is harmless). Either way: do NOT leave it public on Heap — move behind an accessor so the fix is
enforceable.

**Files:** heap/Heap.h, heap/Heap.cpp, runtime/JSArray.cpp, (per-client: heap/HeapClientSet.h).

---

## LIKELY RACES

### HC-03 — `Heap::m_protectedValues` (ProtectCountSet = HashCountedSet<JSCell*>)
*(merges heap-core-03, marking-weak-014)*

**Risk: likely-race** (unconditioned; not yet observed — API protect traffic is rare in the
threads corpus).

**Evidence.** Heap.h:1208 declares it; Heap.cpp:828 (`protect`: `m_protectedValues.add(k.asCell())`)
and Heap.cpp:840 (`unprotect`: `.remove(...)`) are guarded ONLY by
`ASSERT(vm().currentThreadIsHoldingAPILock())` (Heap.cpp:823, :835). GIL-off that predicate is
REDEFINED as "current thread holds an entry token" (VM.cpp:283-298) — satisfied by N
concurrently-running threads simultaneously, so it no longer implies mutual exclusion. The in-tree
comment (Heap.cpp:817-826) concedes "Post-GIL this becomes an access-held predicate... the set
itself stays one-per-server" — i.e. the GIL-off ruling is documented but NOT landed; no
protect-related lock exists in Heap.h. Reachable from any thread via C API:
API/JSValueRef.cpp:810 (`JSValueProtect` → `gcProtect`), API/JSContextRef.cpp:171
(global-context retain/release → `gcProtect(globalObject)`), plus internal `gcProtect`. Two entered
threads protecting concurrently = unlocked HashCountedSet add/remove — the exact DW-2 mechanism
class (shared per-Heap hash container, mutator/API path, rehash UAF). Readers are safe:
Heap.cpp:3995-3997 (`forEachProtectedCell` root walk in the Msr constraint) runs in the I5 stop
window; `protectedObjectCount`/`TypeCounts` (Heap.cpp:~1240+) are API-side statistics walks.

**Ruling.** Mirror the HandleSet §F.3 pattern: per-heap leaf Lock (e.g.
`Heap::m_protectedValuesLock`) taken in `protect()`/`unprotect()` on the gilOff arm only (GIL-on
stays bit-identical). Cold path — protect traffic is API-rate, not allocation-rate. Root scan stays
lock-free under the §10 stop; the API-statistics walks may take the leaf like
`HandleSet::protectedGlobalObjectCount` does (HandleSet.cpp:227-247). Lock is leaf-class: nothing
acquired under it, never held across user JS. Replace the API-lock ASSERT with a lock-held or
access-held predicate.

**Files:** heap/Heap.cpp, heap/Heap.h.

### HC-04 — `Heap::m_weakGCHashTables` (UncheckedKeyHashSet<WeakGCHashTable*>)
*(merges heap-core-04, mutator-entry-weakgchashtables)*

**Risk: likely-race.**

**Evidence.** Heap.h:1278 declares it; Heap.cpp:3690-3697 `registerWeakGCHashTable` /
`unregisterWeakGCHashTable` do a bare HashSet add/remove — no lock, no main-only guard, no assert
at all. Mutator-entry callers: runtime/WeakGCMapInlines.h:40 (WeakGCMap ctor) and :46 (dtor);
runtime/WeakGCSetInlines.h:38/:44. Every spawned Thread's lite-VM/JSGlobalObject bring-up
constructs WeakGCMaps (string caches, structure caches, custom-getter-setter caches, etc.) and
registers them into the SHARED server heap; thread teardown unregisters. A thread-spawn or
thread-exit storm (the corpus does both) gives concurrent HashSet mutation → rehash corruption,
same container class as DW-2. Not observed in the dive (registration is cold relative to spill
paths) but unconditioned. Collector consumer `pruneStaleEntriesFromWeakGCHashTables`
(Heap.cpp:3070) iterates at collection time (world-stopped once shared) — must be covered by, or
asserted against, the same fix.

**Ruling.** Per-heap leaf Lock on add/remove (registration-rate, cold; per-client sharding is
overkill). Either lock the collector's iteration too, or assert
`worldIsStoppedForAllClients()` on the iteration side and lock only the mutator edges.

**Files:** heap/Heap.cpp, heap/Heap.h, runtime/WeakGCMapInlines.h, runtime/WeakGCSetInlines.h.

### HC-05 — `Heap::m_deprecatedExtraMemorySize` (plain size_t)
*(merges heap-core-05, mutator-entry-deprecated-extramemory)*

**Risk: likely-race** (accounting-only severity — lost updates skew GC pacing; no memory
unsafety — but a plain data race, UB, TSAN-visible).

**Evidence.** Heap.h:1206; Heap.cpp:770-779 `deprecatedReportExtraMemorySlowCase` does an
unsynchronized read-modify-write: `CheckedSize checkedNewSize = m_deprecatedExtraMemorySize; ...
m_deprecatedExtraMemorySize = newSize` (lines 772-777). Mutator-reachable from any client thread
via the C API (`JSReportExtraMemoryCost`, API/JSBase.cpp — sole caller; API threads are first-class
mutators under the threads model). Contrast: the sibling `m_extraMemorySize` is handled correctly —
saturating CAS loop in `reportExtraMemoryVisited` (Heap.cpp:3509-3520,
`atomicCompareExchangeWeakRelaxed`) and world-stopped reset (Heap.cpp:3029-3030).

**Ruling.** Atomic: convert to `std::atomic<size_t>` with the same saturating-CAS loop shape as
`reportExtraMemoryVisited` (F3 precedent). Reads in the `extraMemorySize()` total (Heap.cpp:1300)
and the world-stopped reset (Heap.cpp:3030) become relaxed loads/stores. One-file fix.

**Files:** heap/Heap.cpp, heap/Heap.h.

### HC-06 — GC pacing fields read by `VM::performOpportunisticallyScheduledTasks`
*(mutator-entry-gc-pacing-reads; supersedes the safe-collector-only call in heap-core-21 for these
specific fields — the heap-core sweep assumed readers only consume them at safepoints; the
mutator-entry sweep found a concurrent reader)*

Fields: `m_shouldDoOpportunisticFullCollection` (Heap.h:1196), `m_totalBytesVisited` (:1192),
`m_totalBytesVisitedAfterLastFullCollect` (:1191), `m_bytesAllocatedBeforeLastEdenCollect` (:1173),
`m_lastFullGCLength`/`m_lastEdenGCLength` (:1244-1248), `m_lastGCEndTime` (:1372),
`m_currentGCStartTime` (:1373).

**Risk: likely-race** (bogus opportunistic-GC decisions, not corruption; UB + V7 TSAN rung noise).

**Evidence.** Mutator-entry reader: runtime/VM.cpp:3307-3331 (`performOpportunisticallyScheduledTasks`,
under JSLockHolder — a per-VM lock, NOT a heap lock; with N lite VMs sharing one Heap, N threads
read these concurrently). All fields are plain size_t/bool/Seconds/MonotonicTime written by the
collecting context at cycle boundaries (`updateAllocationLimits` / `runEndPhase` /
`increaseLastFullGCLength` Heap.h:697) — the collector runs concurrently with mutators outside
stops, so these are unsynchronized cross-thread reads of 64-bit values; torn values feed the
`estimatedGCDuration` math (VM.cpp:3308, 3318).

**Ruling.** Relaxed atomics: make each field `std::atomic` (wrap MonotonicTime/Seconds as
`atomic<double>`/`atomic<int64_t>`) with relaxed load/store; exactness at safepoints follows the F3
precedent already applied to the allocation counters (Heap.h:966). No lock — single writer
(conductor), many readers.

**Files:** heap/Heap.h, heap/Heap.cpp, runtime/VM.cpp.

### HC-07 — `Heap::m_isInOpportunisticTask` (plain bool, Heap.h:1197)
*(mutator-entry-isinopportunistictask)*

**Risk: likely-race** (write/write race with broken nesting semantics under N clients).

**Evidence.** runtime/VM.cpp:3291: `SetForScope insideOpportunisticTaskScope {
heap.m_isInOpportunisticTask, true }` — executed under JSLockHolder (per-VM, VM.cpp:3286), so two
clients of the shared Heap can enter `performOpportunisticallyScheduledTasks` simultaneously:
client A's SetForScope destructor restores the value it captured, clobbering client B's `true` (or
vice versa); the write itself also races collector-side readers consulting the flag for pacing.

**Ruling.** Per-client: move the flag to HeapClient (like `m_deferralDepth`, Heap.h:1761) and have
heap-side readers OR across entered clients at the safepoint; or make it an atomic counter
(increment/decrement instead of SetForScope bool) if the heap-wide aggregate is what readers want.

**Files:** heap/Heap.h, runtime/VM.cpp, (per-client: heap/HeapClientSet.h).

### HC-08 — `Heap::m_overCriticalMemoryThreshold` + `m_percentAvailableMemoryCachedCallCount` (Heap.h:1383-1384)
*(mutator-entry-overcritical-threshold)*

**Risk: likely-race** (heuristic-only consequence; UB/TSAN noise). `USE(MEMORY_FOOTPRINT_API)`
only — invisible on the Linux dive box but real for the macOS CI matrix.

**Evidence.** Heap.cpp:781-795 `overCriticalMemoryThreshold`:
`++m_percentAvailableMemoryCachedCallCount >= 100` is an unlocked RMW on a plain unsigned, and
`m_overCriticalMemoryThreshold` is a plain bool written on the same path. Called from
`collectIfNecessaryOrDefer` / `shouldDoFullCollection` heuristics on every client's allocation slow
path — N threads increment concurrently; lost increments / stale threshold.

**Ruling.** Relaxed atomics for both fields (`fetch_add` on the counter, relaxed store/load on the
bool). Trivial, no lock.

**Files:** heap/Heap.cpp, heap/Heap.h.

---

## NEEDS-TRACE

### HC-09 — Dynamic subspace registries: server-side `m_<name>` unique_ptrs (FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE) + `MarkedSpace::m_subspaces` Vector
*(heap-core-06)*

**Evidence.** Server Slow paths Heap.cpp:4456-4480 are LOCKLESS (`ASSERT(!m_##name)`; makeUnique;
storeStoreFence; publish) — sound only because GIL-off materialization is supposed to funnel
through the GCClient wrappers, which DO lock: Heap.cpp:6302-6313 (`Locker locker {
server().m_lock }` then `server().name<OnMainThread>()`). Inside that lock, the Subspace ctor →
`MarkedSpace::initializeSubspace` appends to `m_subspaces` (MarkedSpace.h:213). Residual hazard:
any direct mode-blind `vm.heap.name<OnMainThread>()` call on a lite (the VM.h static iso accessor
rewiring is explicitly OPEN per the Heap.h:526-538 IT-9 note) would race both the `m_##name`
publication and the `m_subspaces` append against a sibling client's locked materialization.

**Trace needed.** Call-site trace of VM.h `subspaceFor`/`name<mode>()` consumers under gilOff.

**Ruling if confirmed.** Take `Heap::m_lock` inside the server `name##Slow()` too (cold,
one-time-per-subspace) — makes the funnel assumption unnecessary.

**Files:** heap/Heap.cpp, heap/MarkedSpace.cpp, runtime/VM.h.

### HC-10 — `Heap::m_observers` (Vector<HeapObserver*>) and `m_heapFinalizerCallbacks`
*(heap-core-07)*

**Evidence.** `addObserver`/`removeObserver` are unlocked Vector ops (Heap.h:398-399);
add/removeHeapFinalizerCallback likewise (Heap.cpp:4348-4355). Iterated by the collecting context
at collection boundaries (Heap.cpp:3045, 3233 willStartCollection/didFinishCollection observers;
Heap.cpp:2930 finalizer callbacks). Embedder-API surface — in WebKit these are main-thread-only.

**Trace needed.** Whether Bun registers observers/finalizer callbacks from spawned Threads. If
yes: mutator append racing conductor iteration outside the stop window (willStartCollection runs
pre-stop in the legacy protocol).

**Ruling if confirmed.** Leaf lock on registration + iterate under it (boundary-rate, cold).
Otherwise document main-only guard.

**Files:** heap/Heap.h, heap/Heap.cpp.

### HC-11 — `Heap::m_collectionScope` / `m_lastCollectionScope` + `m_shouldDoOpportunisticFullCollection` write side
*(heap-core-08; the READ side of `m_shouldDoOpportunisticFullCollection` is already covered by HC-06)*

**Evidence.** Plain fields written by the collecting context (willStartCollection /
didFinishCollection); `collectionScope()` (Heap.h:406) is a public unlocked read used from
sweep/finalize paths and API. Once shared, the whole conduct runs inside the stop window so writes
are mutator-excluded — but the legacy (!ISS) concurrent-collector path writes them with mutators
running (pre-existing benign single-byte race), and ISS-off N-thread states shouldn't exist.
`scheduleOpportunisticFullCollection()` (Heap.cpp:4451-4454) sets the bool with no lock — fine if
Bun only calls it from the main/event-loop thread.

**Trace needed.** Bun-side call-site check for off-main `scheduleOpportunisticFullCollection`.

**Ruling.** If off-main scheduling exists: relaxed atomic bool (heuristic-grade; folds into the
HC-06 atomicization). `collectionScope` reads are byte-sized and stop-window-ordered once shared —
document, no code change.

**Files:** heap/Heap.cpp, heap/Heap.h.

### HC-12 — `MarkedSpace::m_blocks` / `MarkedBlockSet::m_filter` + `m_set` read by SamplingProfiler
*(mutator-entry-samplingprofiler-blocks-filter)*

**Evidence.** runtime/SamplingProfiler.cpp:503 copies `m_vm.heap.objectSpace().blocks().filter()`
(MarkedBlockSet.h:73 returns `m_filter` by value, no lock) and the profiler subsequently consults
the set for cell liveness; `MarkedBlockSet::add` (MarkedBlockSet.h:51-55) mutates `m_filter` and
the HashSet when ANY mutator thread allocates a fresh block. Pre-threads this was tolerated because
the profiled VM's mutator was suspended; under shared heap the profiler suspends ONE thread while
N-1 others keep adding blocks to the same MarkedSpace. The bloom-filter word read is torn-tolerant
by design, but HashSet reads (recordJSFrame liveness checks) against a rehashing table are not.
Adjacent: SamplingProfiler.cpp:331 `enablePreciseAllocationTracking` flips MarkedSpace state
mid-run. (The add/remove side of `m_blocks` itself is synchronized — see S-05; the gap is this
lock-free profiler READ path.)

**Trace needed.** Exactly which SamplingProfiler paths touch `m_set` vs only the filter copy, and
whether block adds are serialized at a point the profiler could also take.

**Ruling if confirmed.** Snapshot blocks under a per-heap lock taken by both
`MarkedSpace::didAddBlock` and the profiler, or take a §A.3 thread-granular stop for the sample.
Filter-only consumers can stay racy (bloom filter is false-positive-tolerant) with the filter word
made a relaxed atomic.

**Files:** runtime/SamplingProfiler.cpp, heap/MarkedBlockSet.h, heap/MarkedSpace.cpp.

### HC-13 — `MarkedVectorBase::m_storageForOutOfBoundsAccess` (process-global static scribble slot)
*(marking-weak-002)*

**Evidence.** MarkedVector.cpp:29 defines one process-wide
`EncodedJSValue MarkedVectorBase::m_storageForOutOfBoundsAccess;` written from `at()` on the
RecordOverflow OOB-hardening arm (MarkedVector.h:344-348, 356-359) by ANY mutator thread, no
synchronization. By design it is a junk sink (comment h:268-276) — a torn/raced value is never
trusted — so not a correctness bug, but it is a C++ data race (UB) and a guaranteed TSAN report
once two threads overflow RecordOverflow vectors concurrently.

**Ruling.** Cheapest: make the slot `thread_local` (still satisfies the hardening contract,
removes the race entirely); alternative: relaxed atomic store/load. One-line fix, no behavior
change on the non-OOB path. Fold into the HC-01 MarkedVector change (same files).

**Files:** runtime/MarkedVector.h, runtime/MarkedVector.cpp.

---

## SAFE ROWS (synchronized / collector-only / N-A) — condensed evidence

### S-01 — BlockDirectory shared state (`m_blocks`, `m_freeBlockIndices`, `m_bits`+BVL, cursors, `m_localAllocators`, link words, `m_tlcIndex`) — safe-synchronized
Cursor/bit paths take the bitvector lock: findEmptyBlockToSteal (BlockDirectory.cpp:128-138),
findBlockForAllocation (:141-156), findBlockToSweep (:~380), sweep/shrink in-lock scans with
DropLockForScope. addBlock's m_blocks/m_bits resize requires MSPL when shared (asserted,
BlockDirectory.cpp:~170-180 `heap.mutatorSlowPathLock().isHeld()`) so the I5b resize cannot race
transient BVL readers; every lock-free bits access funnels through
assertIsMutatorOrMutatorIsStopped (BlockDirectory.cpp:~520-560: WSAC | MSPL-held | §A.3 conductor).
m_localAllocators traversals hold m_localAllocatorsLock (stopAllocating/prepareForAllocation/
resumeAllocating/stopAllocatingForGood). Link words are relaxed-atomic with storeStoreFence
publication, acquire/release under TSAN (BlockDirectory.h:138-165). m_tlcIndex assigned once under
MarkedSpace::m_directoryLock (BlockDirectory.h:183-196). Sole unlocked walk:
updatePercentageOfPagedOutPages iterates m_blocks (BlockDirectory.cpp:95-120), reached only via
Heap::isPagedOut which has no caller outside heap/ — dead in JSCOnly/Bun. *Residual: if isPagedOut
ever gets a caller, gate it world-stopped or take BVL.*

### S-02 — LocalAllocator (`m_freeList`, `m_currentBlock`, `m_lastActiveBlock`, `m_allocationCursor`) — safe-synchronized
Ownership is access-based and asserted: allocateSlowCase
`ASSERT(heap.currentThreadIsAllocatorOwner(this))` (LocalAllocator.cpp:~195); conductor-side
mutation funnels through assertSharedAllocatorMutationIsSafe (LocalAllocator.cpp:55-80). Slow path
takes MutatorSlowPathLocker after collectIfNecessaryOrDefer (L2; :~215-235), covering cursor
searches, steals (with weak-bearing carve-out declines), tryAllocateBlock+addBlock; eden-bit flip is
BVL-published when shared. Per-thread routing via Heap::allocationClientForCurrentThread
(Heap.h:1867-1919) with the access-owner tripwire; the JIT-baked-allocator hole is closed at the
allocatorForConcurrently funnel (empty Allocator GIL-off, Heap.h:529-538 AB17c F4 note —
re-enabling inline JIT allocation (U-T7) tracked open there, not a present race).

### S-03 — Allocation/extra-memory accounting counters — safe-synchronized
`m_nonOversizedBytesAllocatedThisCycle` / `m_oversizedBytesAllocatedThisCycle` /
`m_bytesAbandonedSinceLastFullCollect` / `m_blockBytesAllocated` / `MarkedSpace::m_capacity` /
`m_extraMemorySize` / `m_externalMemorySize`: F3 relaxed atomics declared Heap.h:1183-1187 and
MarkedSpace.h:238 ("exact at safepoints, I7"); didAllocateBlock/didFreeBlock fetch_add/sub
(Heap.cpp:3700-3716); didAllocate's plain-RMW arm gated `!isSharedServer()` with a written
single-writer + ISS-flip-ordering proof (Heap.cpp:3263-3290); m_extraMemorySize/m_externalMemorySize
use saturating CAS loops (Heap.cpp:3509-3536) and world-stopped resets (Heap.cpp:3029-3030);
m_capacity fetch_add/sub at MarkedSpace.cpp:273/:482. reportExtraMemoryVisited callers in runtime/
(IntlDurationFormat.cpp:90, Exception.cpp:69, IntlDateTimeFormat.cpp:137-139) are visitor-context,
batched via SlotVisitor::m_extraMemorySize and flushed at SlotVisitor.cpp:425-429.
`totalBytesAllocatedThisCycle()` readers (e.g. VM.cpp:3317-3318) get relaxed loads (Heap.h:966).
(The deprecated twin is NOT covered — see HC-05.)

### S-04 — MarkedSpace precise registry (`m_preciseAllocations`, `m_preciseAllocationSet`, nursery offsets, snapshots) — safe-synchronized
Writer discipline asserted at registerPreciseAllocation (MarkedSpace.cpp:249-259: MSPL-held | WSAC;
T3b audit of all mutator callers in the comment — CompleteSubspace::tryAllocateSlow,
PreciseSubspace::tryAllocate, IsoSubspace::tryAllocateLowerTierPrecise inside allocateSlowCase's
MSPL section); sweepPreciseAllocations asserts WSAC (:277-282); full reader audit at the member
declarations (MarkedSpace.h:215-229) — forEach*/objectCount/size only inside HeapIterationScope,
world-stopped-only once shared per willStartIterating's banner (MarkedSpace.cpp:581-592).

### S-05 — `MarkedSpace::m_blocks` (MarkedBlockSet) add/remove side — safe-synchronized
Adds serialized by MSPL (didAddBlock, MarkedSpace.cpp:656-666, called from tryAllocateBlock which
asserts MSPL when shared, BlockDirectory.cpp:158-165); removes via freeBlock (MarkedSpace.cpp:480-485)
reached from shrink — world-stopped-only once shared (asserted, :487-498) — or
lastChanceToFinalize. Conservative-scan readers run conductor-side world-stopped per the
MarkedSpace.h:215 reader audit. *The SamplingProfiler lock-free READ path is the open question —
HC-12.*

### S-06 — WeakSet allocation plumbing + MarkedSpace active weak-set lists — safe-synchronized
Weak<T> creation → WeakSet::allocate (WeakSetInlines.h:35-76) takes MutatorSlowPathLocker around
freelist pop, findAllocator walk, m_blocks append, AND WeakImpl construction (comment
WeakSetInlines.h:47-66 explains why construction must be inside). The weak-mutation protocol
(every WeakSet mutation under MSPL or world-stopped) is asserted at WeakSet::sweep (WeakSet.cpp:84)
and shrink (:107), incl. the §A.3 conductor disjunct. addActiveWeakSet asserts MSPL|WSAC
(MarkedSpace.cpp:635-648); didAllocateInBlock same (:668-677); takeFrom splice and visits are
conductor-side world-stopped (:310-316, 346-349). Soundness dependency VERIFIED PRESENT: the
weak-bearing carve-out exists at LocalAllocator.cpp:293, :341 and BlockDirectory.cpp:450, :531
(mutator-concurrent sweeps skip blocks whose WeakSet has WeakBlocks). Heap side:
addLogicallyEmptyWeakBlock asserts MSPL|WSAC (Heap.cpp:3404-3410); sweepAllLogicallyEmptyWeakBlocks
takes MutatorSlowPathLocker (:3413-3424); sweepNextLogicallyEmptyWeakBlock asserts
MSPL|WSAC|§A.3-conductor (:3429-3434). Mutator-entry addFinalizer sites (ThreadObject.cpp:122,
JSArrayBufferView.cpp:186, ThreadAtomics.cpp:1157) route through this. *Perf note: all weak
allocation serializes on the single heap-wide MSPL — sharding candidate if Weak-heavy
multithreaded workloads appear. Assert-only caveat: the `currentThreadIsHoldingAPILock()` ASSERT
atop WeakSet::allocate is the GIL-phase predicate, slated to become access-held.*

### S-07 — `WeakSet::deallocate` / WeakImpl state words (lock-free path) — safe-synchronized
Deliberately lock-free even when shared (WeakSet.h:121-135): reachable from cell destructors inside
MSPL'd in-lock block sweeps where re-taking MSPL would self-deadlock. Sound because deallocate only
writes the impl's OWN state word; the only concurrent reader/rewriter of WeakImpl states is a weak
sweep, and sweeps are excluded by construction (conducted sweeps run world-stopped;
mutator-concurrent MSPL sweeps skip weak-bearing blocks — S-06 carve-out). Disjoint words for
different impls. *If TSAN flags the plain state-word store, make WeakImpl::setState relaxed
atomic — protocol unchanged.*

### S-08 — WeakBlock sweep/visit/reap + `m_sweepResult` — safe-collector-only
All run from WeakSet::sweep/visit/reap under the weak-mutation protocol asserts (WeakSet.cpp:84,
:107). takeSweepResult on the mutator allocate path is inside the MSPL section
(WeakSetInlines.h:66-72). addLogicallyEmptyWeakBlock/sweepNext called only from those protected
contexts (WeakSet.cpp:86-101).

### S-09 — Directory/TLC creation plumbing + allocator fast path — safe-synchronized
Directory creation and TLC slot reservation are m_directoryLock-only (MarkedSpace.h:176-185
reserveThreadLocalCacheIndices, rank 7b; m_nextTlcIndexBase per MarkedSpace.h:246);
addBlockDirectory publishes with storeStoreFence before linking (MarkedSpace.cpp:733-740);
CompleteSubspace::m_directoryForSizeStep entries are release-published std::atomic with acquire
fast-path loads; the server m_allocatorForSizeStep table is never populated when shared (§5.5,
verifyNoAllocatorsMaterialized RELEASE_ASSERT at second-client attach); GCThreadLocalCache is
owner-thread-only by contract with conductor exceptions asserted per-slot
(GCThreadLocalCache.h:64-101), teardown under MSPL→rank-8 (stopAllocatingForGood). From the
mutator-entry direction (allocateCell paths), the FreeList fast paths are lock-free per-client and
every shared directory-bit/steal mutation crossing point asserts MSPL-or-stop
(assertSharedAllocatorMutationIsSafe). *DW-4's allocation-heavy anti-scaling (LEDGER §1) marks the
MSPL/steal path as a contention hot spot — perf, not a race.*

### S-10 — Heap stop/handshake state (`m_worldState`, `m_isSharedServer`, `m_gcStopPending`, WSAC, `m_issRevertPending`, `m_gcConductorActive`, `m_mainClient`, `m_clientSet`, `m_safepointEpoch`, MSPL/GCL/GBL) — safe-synchronized
All atomics or lock-guarded with documented protocols at the declarations (Heap.h:1309-1333: GSP
sole-writer seq_cst F8; WSAC conductor-written under GBL F7; GCA under *m_threadLock; m_mainClient
under HeapClientSet::m_lock). Relaxed-ISS soundness proof at Heap.h:432-451; stale-release
impossibility at Heap.h:786-800; access forwarding via GCClient m_accessState seq_cst RMWs
(Heap.h:1755-1758).

### S-11 — Per-thread routed mutator state: DeferGC/DisallowGC counters (`m_deferralDepth`, `m_didDeferGCWork`, `m_mutatorState`) — safe-synchronized (per-lite ruling already implemented)
Once ISS, all three route to the calling thread's client slot via
deferralDepthSlot()/didDeferGCWorkSlot()/mutatorStateSlot() (Heap.h:1774-1840) through
GCClient::Heap::currentThreadClient() — ThreadSpecific TLS (GCThreadLocalCache.cpp:53-71) — to
per-client fields (Heap.h:1761-1763), each touched only by its access-holding thread (asserted
hasHeapAccess() || WSAC). Server slots serve only unstamped threads with matching read/write
pairing (Heap.h:1166-1170, 1353-1358); scopes cache the returned reference so an ISS flip mid-scope
cannot split ctor/dtor across slots (Heap.h:1111-1127); collectIfNecessaryOrDefer's clear hits only
the calling thread's flag. Each VMLite owns its own GCClient::Heap (VMLite.h:264).
AssertNoGC/DisallowGC s_scopeReentryCount is ThreadSpecific (DeferGC.h:83, DeferGC.cpp:32).
Mutator-entry ASSERT readers (JSCellInlines.h:210, WeakGCMapInlines.h:99, ScriptExecutable.cpp:496,
CachedTypes.cpp:2788) and VM.cpp:1125 incrementDeferralDepth hit their own client's counter.

### S-12 — GC request/ticket machinery (`m_requests`, tickets, `m_collectorThreadIsRunning`, `m_threadShouldStop`, `m_mutatorDidRun`, phase fields) — safe-synchronized
Legacy requestCollection mutates m_requests/m_lastGrantedTicket under Locker{*m_threadLock}
(Heap.cpp:2942-2967) and is ASSERT-unreachable once shared (rerouted to requestCollectionShared per
§10B.1, precondition access-holder-or-conductor); ticket invariants checked under the same lock
(Heap.cpp:622-640, 1703-1704). Mutator-entry collectAsync/collectSync (ArrayBuffer.cpp:131/:138,
ThreadManager.cpp:412) scan/mutate m_requests only under Locker{*m_threadLock}. Phase fields
written by the conn-holding context under the established handshake; shared-mode conduct is
single-conductor inside GCL.

### S-13 — Marking infrastructure — safe-synchronized / safe-collector-only
- `m_mutatorMarkStack`: flipped to multi-producer when useSharedGCHeap (Heap.cpp:485-497 banner —
  N-mutator write-barrier append hazard; setMultiProducerAccess() at :497) feeding
  addToRememberedSet's append (Heap.cpp:1518); MarkStackArray::append/clear take m_appendLock on
  flagged instances (MarkStack.h:53-79, lock :85; rationale :44-52 — postIncTop is a non-atomic
  RMW). Drains/clear run world-stopped per MarkStack.h:65-70. Per-instance opt-in keeps
  per-SlotVisitor stacks lock-free on the parallel-marking hot path.
- `m_raceMarkStack`: sole producer SlotVisitor::didRace under m_raceMarkStackLock
  (SlotVisitor.cpp:796-804; Heap.h:1201).
- `m_sharedCollectorMarkStack`/`m_sharedMutatorMarkStack`: parallel-marker-only
  donate/steal/correspondingGlobalStack (SlotVisitor.cpp:811-817) serialized by m_markingMutex
  (Heap.h:1335); no mutator-path access.
- Per-SlotVisitor state: each parallel marker owns its visitor (Heap.cpp:499-501);
  single-producer stacks; m_rightToRun (SlotVisitor.h:169, :240) is the existing concurrent-marking
  stop-protocol lock; m_opaqueRoots is ConcurrentPtrHashSet (Heap.h:1296); VerifierSlotVisitor runs
  only inside the verifyGC stop window.
- `m_availableParallelSlotVisitors` WTF_GUARDED_BY_LOCK(m_parallelSlotVisitorLock) (Heap.h:1225);
  m_barriersExecuted relaxed-atomic stats (Heap.cpp:1467-1472, reset :2377); m_constraintSet
  populated at init.

### S-14 — Write-barrier bits (`m_mutatorShouldBeFenced`, `m_barrierThreshold`) — safe-synchronized (designed benign race)
Single writer = the collecting context at phase changes (setMutatorShouldBeFenced); N-mutator
readers do plain word-sized loads via addressOfMutatorShouldBeFenced()/addressOfBarrierThreshold()
(Heap.h:747-751) including JIT-baked addresses — the pre-existing, designed mutator-vs-collector
benign race of the concurrent-GC barrier protocol (mutatorFence/storeLoadFence discipline,
writeBarrierSlowPath Heap.cpp:3489-3497; "OK for us to race with the collector here"
Heap.cpp:746-756). GIL-off only multiplies readers, not writers; once shared, phase changes happen
world-stopped. Formally a C++ data race on plain bool/unsigned — TSAN-suppression territory, not a
correctness gap.

### S-15 — GC pacing/statistics written by the collecting context — safe-collector-only, EXCEPT the HC-06 subset
`m_sizeAfterLastCollect` family, `m_maxEdenSize`/`m_maxEdenSizeWhenCritical`/`m_maxHeapSize`,
`m_incrementBalance`, `m_shouldDoFullCollection`, `m_totalGCTime`, version counters
(`m_mutatorExecutionVersion`/`m_phaseVersion`/`m_gcVersion`), `m_currentThreadState`/
`m_currentThread` (documented "OK if this becomes a dangling pointer", Heap.h:1380),
`m_signpostMessage`: all written by the collecting context (willStartCollection /
updateAllocationLimits / didFinishCollection — world-stopped once shared, deviation-4 conduct);
mutator-side consumers (shouldCollectHeuristic) read word-sized heuristics whose staleness
self-corrects at the next safepoint (I7 contract). No pointer-bearing container among them is
mutator-mutated. **Carve-out:** the specific fields read concurrently by
`VM::performOpportunisticallyScheduledTasks` are reclassified likely-race — see HC-06.

### S-16 — `Heap::m_handleSet` (HandleSet: Strong<> machinery) — safe-synchronized
GIL-off Strong allocate/free/set-slot route through
strongHandle{Allocate,Deallocate,WriteBarrier}Slow (HandleSet.h:117-144); ALWAYS_INLINE wrappers
(HandleSet.h:223-244) dispatch on the ctor-stamped immutable m_gilOff byte (HandleSet.h:68-109,
with the AB17c F4 stamping-order fix / owner-VM designation noteOwnerVMDesignatedGILOff,
VM.cpp:437); the slow arms take the per-set m_strongLock from the eager-registered side table
(HandleSet.cpp:44-170, ctor registration :178-187, never-allocates lookup for the §LK.8
in-lock-sweep carve-out). Call-site wiring VERIFIED: Strong.h:71/:80/:129/:132/:145 and
StrongInlines.h:39/:46/:55 all go through strongHandle* — no remaining direct
m_freeList/m_strongList mutation from Strong. visitStrongHandles deliberately lock-free
(HandleSet.cpp:205-218 — scans under the heap §10 stop; taking the leaf would invert the carve-out
rank proof). protectedGlobalObjectCount takes the lock GIL-off (HandleSet.cpp:227-247). GIL-on arms
bit-identical to stock. *Documented residual cleanup (HandleSet.cpp:78-100): fold the side-table
lock into the class as `Lock m_strongLock` when HandleSet.h enters an owned file set — pure
tidy-up.*

### S-17 — `Heap::m_arrayBuffers` (GCIncomingRefCountedSet<ArrayBuffer>) — safe-synchronized
All access (vector AND every member's incoming-reference storage) under m_arrayBuffers.lock() per
the invariant banner (GCIncomingRefCountedSet.h:53-80, leaf rank, never safepoints), surfaced as
Heap::arrayBufferIncomingReferencesLock() (Heap.h:714-718); internal Lockers at
GCIncomingRefCountedSetInlines.h:42/:55/:74. Mutator-entry: JSArrayBuffer.cpp:46,
JSArrayBufferView.cpp:193/:201 (addReference from any thread); sweep at Heap.cpp:3079 is
conductor-side.

### S-18 — `Heap::m_codeBlocks` (CodeBlockSet) and `m_jitStubRoutines` (JITStubRoutineSet) — safe-synchronized (with a forward dependency)
Both carry their own locks: CodeBlockSet.h:58/:80 (getLock(), Lock m_lock; the lockerless-looking
iterate(Functor) self-locks, CodeBlockSetInlines.h:50-54, covering JSGlobalObject.cpp:4393;
explicit lockers at VMTraps.cpp:197/:265/:337/:679, SamplingProfiler.cpp:388) and
JITStubRoutineSet.h:82-92 (Lock m_lock, added for the concurrent IC-stub-install case,
GCAwareJITStubRoutine.cpp:52). Deep per-call-site verification of JIT-side add paths is
boundary-owned by the jit/code-lifecycle slice (SPEC-jit). **Forward dependency (recorded at
JITStubRoutineSet.h:88-91):** traceMarkedStubRoutines stays lock-free and is only sound while every
GC phase touching the set runs inside a stop — SPEC-congc (N-mutator concurrent marking) must
re-validate or extend m_lock coverage when marking goes concurrent.

### S-19 — `Heap::m_machineThreads` (MachineThreads) + per-client conservative-scan registration — safe-synchronized
Shared state is the Ref<ThreadGroup> (MachineStackMarker.h:69); addCurrentThread (h:52) delegates
to ThreadGroup::addCurrentThread, internally serialized by the group's WordLock (getLock h:60),
idempotent. Mutator callers: GCClient attach path Heap.cpp:6066-6101 (:6080, with the relaxed
m_lastConservativeScanRegisteredUid cache, Heap.h:1760 — staleness only re-runs the idempotent
add; per-client I4(b) enforcement Heap.h:1705-1710) and JSLock::didAcquireLock (JSLock.cpp:1322).
Conductor's conservative scan reads the list under the group lock world-stopped (signature h:67
takes AbstractLocker); SamplingProfiler readers take getLock() (SamplingProfiler.cpp:209/:387).

### S-20 — Wasm pending-destruction family — safe-synchronized
m_wasmCalleesPendingDestruction WTF_GUARDED_BY_LOCK(m_wasmCalleesPendingDestructionLock)
(Heap.h:1281, lock :1239); the snapshot pattern exists precisely for mutator-concurrent
registration during scanning (Heap.h:1282-1287); snapshot + discovered sets touched only in
prepare/finalizeWasmCalleeCleanup (collection phases); m_boxedWasmCalleeFilter additions under the
lock, reads in conservative-scan/marking context within the stop window.

### S-21 — Misc locked leaves — safe-synchronized
`m_possiblyAccessedStringsFromConcurrentThreads`: append under the dedicated leaf lock with an
explicit GIL-off N-mutator rationale (Heap.h:872-882; WTF_GUARDED_BY_LOCK :1254-1255; GC-end clear
conductor-side); mutator-entry JSString.h:1008 (swapToAtomString on first atomization, any thread).
`m_stopTheWorldSafepointHooks` WTF_GUARDED_BY_LOCK(m_stopTheWorldSafepointHookLock)
(Heap.h:1331-1332). `m_delayedReleaseObjects` is FOUNDATION/GLIB-only (not compiled on this Linux
target), historically under Heap::m_lock.

### S-22 — MarkedSpace phase/version state (`m_markingVersion`, `m_newlyAllocatedVersion`, `m_edenVersion`, `m_isIterating`, `m_isMarking`, `m_conservativeScanIsPrepared`) — safe-collector-only
Versions advance only in beginMarking/endMarking/clearNewlyAllocated — conductor-side world-stopped
once shared (asserts at MarkedSpace.cpp:277-282, 487-498, 690-696); mutators read them unlocked on
every isMarked/isNewlyAllocated check, safe because every write is inside a stop window (version
reads happen-after the stop release). m_isIterating flips only inside
willStartIterating/didFinishIterating, world-stopped-only once shared (MarkedSpace.cpp:581-599).

### S-23 — Subspace residuals (sentinel lists, lower-tier free lists, `m_cellSets`) — safe-synchronized
Precise-allocation linking and lower-tier free-list pops are inside the MSPL-serialized slow paths:
IsoSubspace::tryAllocateLowerTierPrecise inside allocateSlowCase's MSPL section
(LocalAllocator.cpp:~240-247, §5.2/§5.6/I16); CompleteSubspace::tryAllocateSlow /
PreciseSubspace::tryAllocate take MSPL per the T3b audit (MarkedSpace.cpp:249-259); steal-path
directory walks run under MSPL; m_cellSets mutates only at IsoCellSet construction (heap init /
dynamic-subspace creation, under server m_lock per HC-09's funnel); sweeping/removal sides are
world-stopped (deviation 4).

### S-24 — Activity callbacks (`m_fullActivityCallback`, `m_edenActivityCallback`, `m_sweeper`, `m_stopIfNecessaryTimer`) — safe-synchronized (neutered when shared)
didAllocate skips eden-activity dispatch and reportAbandonedObjectGraph skips full-activity when
isSharedServer() ("activity callbacks never fire collections when shared — triggering is
mutator-driven", Heap.cpp:3263-3270, :~810); the IncrementalSweeper is disabled entirely once
shared (deviation 4, BlockDirectory.cpp assertSweeperIsSuspended banner + assertNoUnswept FIX-3
note). Setters are embedder API on the owning VM's thread; RefPtr reads on mutator slow paths are
gated behind !isSharedServer().

### S-25 — Mutator-side heap iteration: haveABadTime forEachLiveCell — safe-synchronized
JSGlobalObject.cpp:3543/:3554/:3576 iterate every live cell of the SHARED MarkedSpace from a
mutator thread. HeapIterationScope itself stops nothing (HeapIterationScope.h:44-58, T9 note), but
the branch routes haveABadTime through a §A.3 thread-granular stop (JSGlobalObject.cpp:3432) and
MarkedSpace::stopAllocating enforces it: `ASSERT(!heap().isSharedServer() ||
worldIsStoppedForAllClients() || mutatorSlowPathLock().isHeld() ||
(jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()))`
(MarkedSpace.cpp, citing UNGIL §K.5 class-4/AB-10). The residual
"havebadtime-vs-indexed-fastpath" closeout item is a semantic ordering bug, not a missing lock.
Any OTHER mutator-side HeapIterationScope added later inherits the assert — the right tripwire.

### N/A — HandleStack — safe-main-only (does not exist)
No HandleStack.{h,cpp} under Source/ (heap/ contains only Handle.h, HandleBlock*, HandleSet*,
HandleTypes.h, HandleForward.h). Removed upstream years ago (LocalScope era). Chartered for
completeness — nothing to audit or fix.

---

## Fix wave plan (confirmed + likely rows, grouped by file ownership)

Nearly every confirmed/likely row touches `heap/Heap.h` + `heap/Heap.cpp`, so the waves serialize
on those two files; parallelism comes from the satellite files.

### Wave 1 — DW-2 class closure (single fixer; owns Heap.{h,cpp}, MarkedVector.{h,cpp}, JSArray.cpp, HeapClientSet.h)
1. **HC-01 `m_markListSet`** (row 1, the DW-2 fix) — per-client sharded sets keyed by registering
   client (preferred) or the per-heap struct{Lock,set} fallback. Files: Heap.h, Heap.cpp,
   MarkedVector.h, MarkedVector.cpp, HeapClientSet.h.
2. **HC-13 `m_storageForOutOfBoundsAccess` → thread_local** — folded in (same MarkedVector files,
   one line).
3. **HC-02 `immutableButterflyToStringCache`** — per-client cache behind an accessor (or gilOff
   skip as the stopgap); GC-end clear via client registry. Files: Heap.h, Heap.cpp, JSArray.cpp,
   HeapClientSet.h.

Rationale for bundling: identical mechanism class, overlapping files, and HC-01/HC-02 are the two
rows with a demonstrated (or trivially demonstrable) UAF — they gate everything else.

### Wave 2 — Heap leaf locks (single fixer; owns Heap.{h,cpp} only; starts after Wave 1 lands)
4. **HC-03 `m_protectedValues`** — `m_protectedValuesLock`, gilOff arm only; assert swap to
   lock/access-held predicate.
5. **HC-04 `m_weakGCHashTables`** — leaf lock on register/unregister (+ assert or lock the prune
   iteration).
6. **HC-05 `m_deprecatedExtraMemorySize`** — saturating-CAS atomic (F3 shape).
7. **HC-08 `m_overCriticalMemoryThreshold` + counter** — relaxed atomics.

All four are cold-path, Heap.{h,cpp}-only changes; one reviewer pass covers them.

### Wave 3 — pacing atomicization (parallel-eligible with Wave 2 EXCEPT for the shared Heap.h declarations — coordinate or serialize on Heap.h)
8. **HC-06 pacing fields** — relaxed atomics for the 8 fields read by
   `performOpportunisticallyScheduledTasks`; reader updates in VM.cpp:3307-3331. Files: Heap.h,
   Heap.cpp, VM.cpp.
9. **HC-07 `m_isInOpportunisticTask`** — per-client flag or atomic counter; replaces the
   SetForScope at VM.cpp:3291. Files: Heap.h, VM.cpp, HeapClientSet.h.
10. **HC-11 write side** — if the Bun trace (below) finds off-main
    `scheduleOpportunisticFullCollection`, fold the relaxed-atomic bool into this wave.

### Wave 4 — trace resolutions (no engine code until traced)
- **HC-09**: trace VM.h `subspaceFor`/`name<mode>()` consumers under gilOff; if any direct
  server-accessor path is reachable, add `Heap::m_lock` to the server `name##Slow()` bodies
  (Heap.cpp only).
- **HC-10 / HC-11**: Bun-side call-site check (observers, finalizer callbacks,
  scheduleOpportunisticFullCollection off-main). Outcome is either a one-line doc note
  ("main-only guard") in this file or a leaf lock in Heap.{h,cpp}.
- **HC-12**: trace SamplingProfiler `m_set`-vs-filter usage; fix shape depends on outcome
  (snapshot-under-lock in MarkedSpace.cpp + SamplingProfiler.cpp, or §A.3 stop for the sample).

### Recorded forward dependencies (not fixes here)
- S-18: SPEC-congc must re-validate JITStubRoutineSet::traceMarkedStubRoutines lock-free-ness when
  marking goes N-mutator-concurrent (JITStubRoutineSet.h:88-91).
- S-16: HandleSet side-table → member-lock collapse at milestone cleanup (HandleSet.cpp:78-100).
- S-02: U-T7 re-enable of inline JIT allocation must re-audit the allocatorForConcurrently funnel
  (Heap.h:529-538).
- S-01: any future caller of Heap::isPagedOut must gate world-stopped or take BVL.
