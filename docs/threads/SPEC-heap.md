# SPEC-heap.md - FROZEN implementation spec (rev 13)
## Heap server & per-thread allocators (N mutators over one `JSC::Heap`)

FROZEN; implement without redesigning. Branch `jarred/threads`; design doc `THREAD.md`. Rev 13 = rev 12+r4 fixes (L4/9b lock-order split, deviation-7 gate split, §14 prep; history §24); r2/r3: history §22-23. `SPEC-heap-history.md` ("history") is NON-NORMATIVE; binding = this file + `SPEC-heap-annex.md` (manifest-5 hunks; itself <=cap).

Owned paths: `Source/JavaScriptCore/heap/**`, `JSTests/threads/heap-*.js`, `docs/threads/INTEGRATE-heap.md`. NOT editable (§13 manifest instead): `runtime/{OptionsList.h, VM.h/.cpp, JSGlobalObject.*, VMManager.*, JSCConfig.h}`, `tools/JSDollarVM.cpp`, `wasm/js/JSWebAssemblyInstance.cpp`, `Sources.txt`, `CMakeLists.txt`.

Notation: WSAC=`worldIsStoppedForAllClients()`, MSPL=`m_mutatorSlowPathLock`, BVL=`m_bitvectorLock`, GCL=`m_gcConductorLock`, GBL=`m_gcBarrierLock`, GBC=`m_gcBarrierCondition`, GSP=`m_gcStopPending`, GCA/GEC=`m_gcConductorActive`/`m_gcElectionCondition`, ISS=`isSharedServer()`, AHA/RHA=`acquire/releaseHeapAccess()`, SINFAC=`stopIfNecessaryForAllClients()`, CSAC/RCAC=`collectSyncAllClients`/`requestCollectionAllClients`, CIND=`collectIfNecessaryOrDefer`, ACT/DCT=`attach/detachCurrentThread()`, BD=`BlockDirectory`, LA=`LocalAllocator`, TLC=`GCClient::GCThreadLocalCache`, HCS=`HeapClientSet`, GCH=`GCClient::Heap`, VMM=`VMManager`, NVS=`notifyVMStop`, SAL=`structureAllocationLock`.

## 1. Scope

Deliverables = §§5-12. Non-goals: object model, VM-lite split, atom-table sharding, Handler-IC/FTL, TLC-aware emission, `Thread()` API, GIL stub.

## 2. Ground truth

Inventory 2.1-2.9: history App. A; T3b/T5b/T8/T9 re-verify. Retained (2.6): VMM GC stop unimplemented (`VMManager.h:200-211`); park/callback only inside NVS; **conductor = §10.2 election winner**; VMM = trap+park (§13.5).

## 3. Deviations from THREAD.md (this section wins on conflict)

1. Per-thread non-iso allocators are new (§5.3); precise allocation unsynchronized (§5.6).
2. "Safepoints via existing VMManager machinery" - insufficient (§2.6): VMM only traps/parks; conductor = requester.
3. "Split exists so N clients share one server" - server is by-value in VM with `OBJECT_OFFSETOF` back-pointers=>phase 1: the shared server **is the main VM's `vm.heap`**, one per process (I13); `Heap::vm()` = "the main mutator VM" (T9).
4. "Concurrent-GC protocol extends to N mutators" - deferred. Shared mode is synchronous, conductor-driven (§10); disabled: concurrent marking, collector-conn, incremental assist, activity-callback collection, mutator-concurrent sweeping; parallel marking inside the stop stays. Option off=>today's protocol (I10).
5. Cross-spec (normative): soundness is `Thread()`-definition-independent (predicates over registered clients+heap access, §10A); secondary-context creators MUST ACT/DCT (§9; §12.1).
6. Shared-mode JIT allocation is slow-path-only (§5.5); cost confined to the option (I10).
7. Flag-on perf carve-outs, phase 1 (GIL-masked): §5.5 slow-path alloc, §3.4 sync GC, §5.2 single-MSPL (in-lock sweeps). Chartered WS gating GIL removal: TLC-aware inline emission — its addressing contract MUST be per-THREAD (VMLite/TLS-relative, §3.8); the §5.3 vm-relative chain is GIL-phase-only (deviation 6) — per-directory handout+out-of-lock sweep, concurrent marking/incremental sweep. Budget (r13 SPLIT): jit Task-13 composite GATES {useJSThreads=1, useSharedGCHeap=0} at <=5%; {1,1} is MEASURED+RECORDED, NOT gated phase 1 (§5.5 slow path dominates; budget set at GIL-removal chartering with the TLC-emission charter); {1,0} miss=>jit §4.3 LLInt-cache revival REQUIRED pre-ship; coupling decision=orchestrator, INTEGRATE records.
8. **Post-GIL execution model (r12, normative cross-spec; charter listed in api §2).** N mutators=ONE GCClient::Heap PER Thread sharing the server: §10A access state, §5.3 TLC, §5.4 deferral, §11 epoch all stay per-CLIENT exactly as specced, instantiated per thread; Thread attach/detach=ACT/DCT on that thread's OWN client (api §5.2). Client lifecycle inside one VM (creation/teardown wiring)=chartered with deviation 7+vmstate Dev 10 Phase B; owner recorded in INTEGRATE. GIL phase: one client, sequential JSLock migration (I2) — sound. Note: the §10 GC stop barrier is already thread-granular (per-client access state, deviation 5); only VMM trap delivery/jit R1 arbitration are VM-granular (Phase-B charter, jit R1 freeze scope).

## 4. Architecture

One *server* heap per thread group (phase 1: the main VM's heap; gate `Options::useSharedGCHeap()`); each client (GCH) gains a TLC (§5.3)+access state (§10A); directories/blocks/bitvectors stay shared server-side.

## 5. Data structures

### 5.1 `JSC::HeapClientSet` (new; §12)

```cpp
namespace JSC {
class HeapClientSet {
public:
    void add(GCClient::Heap&); // sticky-shared on size()>1 ever; blocks until quiescent (§10B.4)
    void remove(GCClient::Heap&); // blocks during stop..resume (I13)
    template<typename F> void forEach(const F&); // m_lock or world-stopped
    unsigned size() const;
private:
    Lock m_lock; // rank 6
    SentinelLinkedList<GCClient::Heap, BasicRawSentinelNode<GCClient::Heap>> m_clients;
};
}
```

GCH gains a `BasicRawSentinelNode` base+§5.3/§10A/§11/§12.1 members; ctor/dtor register with `server().clientSet()` (`VM.cpp:260`). Server additions (`heap/Heap.h`; accessors §9): `HeapClientSet m_clientSet`; `Lock`s MSPL, GCL, GBL; `Condition` GBC; `bool m_gcConductorActive`+`Condition m_gcElectionCondition` (under `*m_threadLock`, rank 5; §10.2); GSP=`Atomic<bool>`, seq_cst (F8); ISS **sticky**: option && size() EVER >1, set under stop-quiescence (I15); cleared ONLY via §10D.

### 5.2 Synchronized block handout

Per-server MSPL in `allocateSlowCase`, held across `tryAllocateWithoutCollecting`/`tryAllocateBlock`/`addBlock`/steal (`LocalAllocator.cpp:133-156`); serializes steals, accounting, `tryAllocateLowerTierPrecise`, `addBlock` resizes (I5b), precise registration (§5.6). Owned edits: (1) `:116` assert->`ASSERT(heap.currentThreadIsAllocatorOwner(this))` (I2; predicate §10A.1); lock after CIND returns (L2), never across a collection request or stop; (2) eden-bit store `:250` under BVL; `didAllocateInBlock` (weak-set splice, `MarkedSpace.cpp:553`) unchanged — mutator call `:251` is inside this MSPL section, other `m_newActiveWeakSets` access conductor-side WSAC (I5); debug assert `MSPL ∨ WSAC ∨ !ISS`; T8 audits; (3) `tryAllocateBlock` `WTF_REQUIRES_LOCK(MSPL)` via `AbstractLocker&`; (4) stop/resume/prepare assert `WSAC ∨ (MSPL: §5.3 teardown) ∨ !ISS`; (5) `addBlock` callers assert MSPL when ISS.

### 5.3 TLC (new; §12)

Index assignment (server-side): monotonic `m_nextTlcIndexBase` under `MarkedSpace::m_directoryLock` (rank 7b, NOT MSPL — directoryLock-only, JIT-thread-safe; `CompleteSubspace.cpp:68`); each `CompleteSubspace` reserves `MarkedSpace::numSizeClasses` contiguous slots (`m_tlcIndexBase`); size class `i`'s slot=`tlcIndexBase() + i`; non-iso BD records `m_tlcIndex` (assigned in `allocatorForSlow`; `BlockDirectory(size_t)`->`(Heap&, size_t)`); iso=`UINT_MAX`; aliased entries share the LA*. Each `GCClient::IsoSubspace` LA also enters its client's `m_perDirectory` at materialization (lookup-only, not in `m_table`; covers iso for §10A.1).

```cpp
namespace JSC { namespace GCClient {
class GCThreadLocalCache {
    WTF_MAKE_NONCOPYABLE(GCThreadLocalCache);
public:
    explicit GCThreadLocalCache(JSC::Heap& server);
    ~GCThreadLocalCache(); // per-slot stopAllocatingForGood() under MSPL

    // fast: bounds check + indexed load; null => slow: materialize LA (dedup I3)
    Allocator allocatorFor(BlockDirectory&); // by directory->tlcIndex()
    Allocator allocatorForSizeStep(CompleteSubspace&, size_t sizeClassIndex);

    void stopAllocating();
    void resumeAllocating();
    void prepareForAllocation();
    void stopAllocatingForGood();

    // PROVISIONAL (Status): slot = tlcIndexBase + sizeClassIndex;
    //   slot < *offsetOfTableBound() ? table[slot] : null
    static constexpr ptrdiff_t offsetOfTable();
    static constexpr ptrdiff_t offsetOfTableBound();
private:
    JSC::Heap& m_server;
    Allocator* m_table { nullptr }; // flat; LocalAllocator* or null
    unsigned m_tableBound { 0 }; // grow-only
    Vector<std::unique_ptr<LocalAllocator>> m_ownedAllocators;
    HashMap<BlockDirectory*, LocalAllocator*> m_perDirectory; // cold; I3
};
}}
```

**Status.** Layout+indexing FROZEN; JIT addressing contract (offset exports+the `vmGPR + OBJECT_OFFSETOF(VM, clientHeap) + offsetOfThreadLocalCache()` chain) **provisional** (deviation 6) - offsets exported, layout-stable. Option on: `CompleteSubspace::allocatorFor`/`Subspace::allocate` route through the caller's TLC; server allocator array/vector **never populated** (§5.5); `allocatorForSlow` still creates directories (directoryLock, no MSPL), no server LA; `MustAlreadyHaveAllocator` forbidden when shared (T4); option off=>byte-for-byte today (I10). GCH gains `m_threadLocalCache`+accessors; `lastChanceToFinalize()` per FIXME `Heap.h:1279-1281`. Teardown (world running): MSPL across all slots' `stopAllocatingForGood()` (flips directory bits — I5b writer; `LocalAllocator.cpp:107`), unlink under `m_localAllocatorsLock` (7→8).

### 5.4 Accounting; DeferGC; assist/activity

`m_nonOversizedBytesAllocatedThisCycle`, `m_oversizedBytesAllocatedThisCycle`, `m_lastOversidedAllocationThisCycle`, `m_bytesAbandonedSinceLastFullCollect`, `m_blockBytesAllocated`, `MarkedSpace::m_capacity`->`std::atomic<size_t>`, relaxed both sides. `performIncrement` early-returns when ISS (deviation 4; `m_incrementBalance` plain; debug-assert mutator `SlotVisitor` world-stopped when shared). Activity callbacks never fire collections when shared (`didAllocate` skips eden-activity dispatch); triggering = mutator-driven (CIND+CSAC; I15, T5).

**DeferGC per-client when shared (I17).** Server `Heap::m_deferralDepth` is plain `++/--` (`HeapInlines.h:166-176`). Rule: GCH gains `unsigned m_deferralDepth`; `incrementDeferralDepth`/`decrementDeferralDepth*` (owned `HeapInlines.h`) route to `currentThreadClient()`'s counter when ISS else the server field (I10); `isDeferred()`/CIND/`decrementDeferralDepthAndGCIfNeeded` consult the CALLING client's depth when ISS. `GCDeferralContext` stays stack-local, unchanged.

### 5.5 JIT inline-allocation gating (owned files)

Rule: **option on, no server-side non-iso `Allocator` is ever materialized** - `m_allocatorForSizeStep` stays null, `m_localAllocators` empty=>baked constants/FTL runtime loads null=>every JS-tier emitter slow-paths=>`CompleteSubspace::allocate` (owned)=>caller's TLC. RELEASE_ASSERT `!m_allocatorForSizeStep[i]` ∀i at second-client attach; §12.1 runs JIT-on; slow paths stay a C++ FreeList pop. **Wasm-GC unsupported when shared:** ctor memcpys server LA*s for BBQ/OMG inline alloc (`prepareAllAllocators()` sole caller, `JSWebAssemblyInstance.cpp:135-141`); emitters assume non-null (`AssemblyHelpers.cpp:962`)=>`prepareAllAllocators()` (owned) RELEASE_ASSERTs `!Options::useSharedGCHeap()`; manifest 11 rejects wasm-GC instantiation.

### 5.6 Precise-allocation synchronization

`tryAllocateSlow` (`CompleteSubspace.cpp:115-146`), `registerPreciseAllocation` (`MarkedSpace.cpp:231-241`), `reallocatePreciseAllocationNonVirtual` (`:148-202`) mutate the precise vectors/set unlocked. Shared-mode rule: under `Locker { MSPL }` - `tryAllocateSlow` `:138-145` one critical section (lock after CIND `:132`, L2); `reallocatePreciseAllocationNonVirtual` `:173-199` (after `:171`); all other mutator-path mutations found by T3b. Readers world-stopped (I5) or today's single-mutator rules (T3b vs I16); standalone clients skip VM-coupled preludes (§12.1).

## 6. Lock ordering (total; lower rank = outermost)

Process-wide **master order** (vmstate §7 defers here).

| Rank | Lock | Where |
|---|---|---|
| 1 | per-client API lock (`JSLock`) / heap access (§10A) | `JSLock.h:40-50`; held entering NVS |
| 2 | GCL | §10; before any VMM call |
| 3 | `VMManager::m_worldLock` | `VMManager.h:340` |
| 4 | GBL | §10A; rank 3 optional before it, never after |
| 5 | `Heap::m_threadLock` (GC tickets) | existing |
| 6 | `HeapClientSet::m_lock` | §5.1 |
| 6b | JIT worklist locks, `CodeBlock::m_lock` (jit §7 orders within 6b) | outer to 7a-10 (§13.10f) |
| 7a | SAL (`SharedVMState`) | vmstate §5; across Structure allocation (I14) |
| 7 | MSPL | §5.2/§5.6; §5.3 teardown |
| 7b | `MarkedSpace::m_directoryLock` | `MarkedSpace.h:223`; §5.3; JIT threads take it alone |
| 8 | `BlockDirectory::m_localAllocatorsLock` | `BlockDirectory.h:178` |
| 9 | BVL | `BlockDirectory.h:177` |
| 9b | `MarkedBlock` internals | §5.2 in-lock sweeps; under MSPL/BVL |
| 10a | per-cell lock (JSCellLock) | `IndexingType.h:230`; L4 back-edge |
| 10b | `Structure::m_lock` | inside 10a (OM §6; flatten) |
| leaf | `GCSafepointEpoch::m_retireLock` (§11); AtomString shards | OK under 10a/10b, never 7-9b (§13.10f) |

- L1. Never two same-rank locks (exception: 10a<10b); never two BVLs (steal releases each first).
- L2. `requestStopAll(GC)` / §10.2 election entered with no rank ≥ 4 lock and no SAL; GC initiation from CIND precedes MSPL.
- L3. The conductor takes ranks 4-10 freely while world-stopped; parked mutators hold none ≥ 4 (I6).
- L4 (r13). 9b never wraps allocation (Riptide). 10a/10b holders allocate ONLY under pre-lock DeferGC/GCDeferralContext (OM O1; JSArray.cpp:1805-1806); that allocation may take ranks 7-9b — the SOLE sanctioned back-edge, acyclic: rank 7-9b sections NEVER acquire 10a/10b (debug-assert, §5.2/§5.6).
- L5. A SAL holder must not initiate/join/wait for STW, park, or block in SINFAC; its allocations use `GCDeferralContext` (I14).
- L6. GBL while holding `m_worldLock` forbidden in heap code (the §13.5 hooks sit outside it).

## 7. Memory fences

- F1. Block handout publication only via BVL-protected `inUse`/`canAllocate` flips; the lock fences.
- F2. FreeList access-confined (I2); non-owner access only by the conductor world-stopped (F8).
- F3. Accounting counters relaxed (exact sums re-establish at safepoints, I7).
- F4. Epoch: conductor `store(release)` while stopped; readers `load(acquire)`; retire list under `m_retireLock`.
- F5. `m_indexingTypeAndMisc`: always CAS (`JSCell.h:297`).
- F6. `m_accessState` transitions seq_cst RMWs (F8); RHA publishes prior heap writes; conductor reads seq_cst under GBL.
- F7. WSAC: written only by the conductor under GBL (set after barrier, cleared pre-resume); reads acquire.
- F8. **Stop-pending vs. access-acquire (Dekker pair, normative).** GSP; sole writer the conductor: seq_cst `true` at §10.3, `false` at §10.8. Client AHA: (0) already HasAccess (`m_accessOwner`)=>return (**idempotent, no CAS-spin**: JSLock recursion/ACT/hook re-entry); (1) seq_cst CAS NoAccess->HasAccess; (2) seq_cst load GSP; (3) if pending, **mandatory revert** - seq_cst exchange->NoAccess, signal GBC under GBL, block until `!GSP`, retry from (1). Conductor: seq_cst samples each client under GBL until all NoAccess. **seq_cst mandatory** - acq/rel misses both store-load pairs (proof: history).

## 8. Invariants (each needs a test/assert; sketches: history App. D)

- **I1**: a handle with `isInUse` set is referenced (`m_currentBlock`/`m_lastActiveBlock`/in-sweep) by ≤1 thread; transfer only under its directory's BVL.
- **I2**: an LA/`FreeList` is mutated only by the thread holding its owning client's heap access, except the conductor world-stopped; access-based, not thread-pinned (JSLock migration transfers).
- **I3** (one allocator per (client, directory)): dedup via `m_perDirectory`; aliased slots share the pointer.
- **I4**: allocation only for a client with completed ACT: (a) in HCS; (b) `machineThreads().addCurrentThread()` ran on this thread (enforced in AHA); (c) access held.
- **I5**: shared mode runs marking-start, stop/prepare iteration, conservative scan, constraint solving, sweep scheduling, precise-vector iteration only on the conductor (or its parallel helpers) while WSAC; excluded: I5b, I16.
- **I5b**: `m_bits` reallocated only in `addBlock` holding BVL+(shared) MSPL; every bitvector access holds that directory's BVL, MSPL (incl. §5.3 TLC teardown), is world-stopped, or runs `!ISS` under today's rule; T8 audits.
- **I6**: a mutator parked in NVS, blocked in AHA, or waiting in SINFAC holds no rank ≥ 4 lock, no SAL.
- **I7**: at every safepoint counters equal the true allocation sum since the last; monotone between.
- **I8**: a stolen empty block is swept/removed/re-added under MSPL; never allocatable in two directories.
- **I9**: after `lastChanceToFinalize()` no allocator of the client is in any `m_localAllocators` list; every held block has `inUse == false`.
- **I10**: option off=>fast/slow paths execute the same code as `main` (branches gated; TLC bypassed; server allocators populated; legacy protocol incl. concurrent marking+`performIncrement`).
- **I11**: an item retired at epoch E is destroyed only after (a) every client published `localEpoch > E` (or detached) AND (b) world-stopped AND compiler threads suspended **by the reclaimer's own suspend/resume pair** (NOT the conducted cycle's); `bumpAndReclaim()` RELEASE_ASSERTs this; legal contexts: §10 step 7 (ISS)+legacy `runEndPhase` (`!ISS`, §9 note) — never a JSThreads stop (CS4 refused); marker helpers run only inside the stop window.
- **I12**: root set ⊇ stack ∪ registers of every I4(b)-registered thread (suspend-and-copy, §10.6) ∪ `CLoopStack` if `!ENABLE(JIT)` (`Heap.cpp:910`) ∪ the **window-witness set**, closed under tracing INSIDE the marking fixpoint (the "Wlr" core marking constraint, normative): (a) every version-current `newlyAllocated` cell of every stopped block — `stopAllocating` NA-stamps every non-free cell of every local allocator's block, not just `m_currentBlock`; (b) every cell of every directory-`isAllocated` (free-list-consumed) block; (c) every NA precise allocation. Mark-without-trace is FORBIDDEN (mark-keyed registries, e.g. transition `WeakGCHashTable` pruning, must only observe consistent retained objects — history §25). Witness bits are read only under the `MarkedBlock` header-lock protocol (snapshot per block; appends after unlock). Soundness: L1 — every pre-park heap store happens-before conductor constraint execution (RHA seq_cst access exchange + §10.4 barrier); L2 — no client releases heap access mid-allocation/initialization. Deliberate over-retention is part of the contract: eden-cycle retentions are sticky until the next full collection; in a full collection leg (b) retains every cell of every window-consumed block, so a dead cohort plus closure can ride ≤ 2 full collections.
- **I13**: `add`/`remove` cannot complete between stop and resume; `remove` of a stopped client defers to resume; the size()>1 `add` RELEASE_ASSERTs no other sticky-shared server, sets sticky ISS, blocks until legacy-quiescent (§10B.4), inserts; ISS reverts only via §10D.
- **I14**: a SAL holder must not call `requestStopAll`/CSAC/SINFAC or park; allocations pass `GCDeferralContext`; debug STW-forbidden counter checked at those entries; §9 scope pair+N7 macro for vmstate.
- **I15**: one collection protocol at a time - legacy while `!ISS`, §10 once set; switches only at `add` (I13) or §10D revert, both under quiescence; triggers re-route after.
- **I16**: shared mode mutates both `m_preciseAllocations` vectors, `preciseAllocationSet`, `indexInSpace` stamps only under MSPL or world-stopped; readers likewise (or `!ISS`).
- **I17**: when ISS, deferral depth is per-client (§5.4): touched only by its access-holding thread (debug-asserted); one client's decrement never closes another's `DeferGC` scope; CIND defers iff the calling client's depth is nonzero. `!ISS`=>today's server counter.

## 9. Public interface (FROZEN)

```cpp
namespace JSC {

class Heap { // server (heap/Heap.h)
public:
    HeapClientSet& clientSet();
    Lock& mutatorSlowPathLock();
    bool isSharedServer() const; // sticky (§5.1/I13)
    bool worldIsStoppedForAllClients() const; // F7
    bool gcStopPendingForAllClients() const; // F8, read-only

    // §10 pre: access held; no rank>=4/7a lock; not in stop window
    JS_EXPORT_PRIVATE void collectSyncAllClients(CollectionScope);
    JS_EXPORT_PRIVATE void requestCollectionAllClients(GCRequest);
    JS_EXPORT_PRIVATE void stopIfNecessaryForAllClients(); // §10A poll, from CIND

    GCSafepointEpoch& safepointEpoch();

    // once per collection, BOTH protocols (contract notes); §13.10d
    JS_EXPORT_PRIVATE void addStopTheWorldSafepointHook(void (*)(JSC::Heap&));

    JS_EXPORT_PRIVATE void incrementSTWForbiddenScope(); // I14; debug-only impl
    JS_EXPORT_PRIVATE void decrementSTWForbiddenScope();
    // + #define JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE 1 here (vmstate N7 shim macro)

    bool currentThreadIsAllocatorOwner(const LocalAllocator*) const; // §10A.1

    // §10C/CS2: GCL (rank 2) RAII for a JSThreads/debugger stop; pre: access released; no bumpAndReclaim; !ISS: no-op.
    class JSThreadsStopScope {
    public:
        JS_EXPORT_PRIVATE explicit JSThreadsStopScope(JSC::Heap&);
        JS_EXPORT_PRIVATE ~JSThreadsStopScope();
    };
};

namespace GCClient {
class Heap {
public:
    explicit Heap(JSC::Heap&); // UNCHANGED
    JSC::Heap& server(); // UNCHANGED
    GCThreadLocalCache& threadLocalCache();
    static ptrdiff_t offsetOfThreadLocalCache();
    void lastChanceToFinalize(); // implements FIXME Heap.h:1279

    // I4 lifecycle: call on the using thread
    JS_EXPORT_PRIVATE void attachCurrentThread(); // I4(a)-(c) + acquires access
    JS_EXPORT_PRIVATE void detachCurrentThread(); // releases access, epoch=MAX
    JS_EXPORT_PRIVATE void markStandalone(); // non-VM client; arms vm() assert

    // §10A/F8; REQUIRED around blocking native calls
    JS_EXPORT_PRIVATE void acquireHeapAccess();
    JS_EXPORT_PRIVATE void releaseHeapAccess();
    bool hasHeapAccess() const;

    static Heap* currentThreadClient(); // §10A.1 TLS slot; null off-client threads
};
} // namespace GCClient

class GCSafepointEpoch { // heap/GCSafepointEpoch.h
public:
    uint64_t current() const; // acquire load
    // defer destruction past the next all-client safepoint
    JS_EXPORT_PRIVATE void retire(void* pointer, void (*destroy)(void*));
    template<typename T> void retire(std::unique_ptr<T>);
    // §10.7 conductor ONLY, after its OWN compiler-thread suspend; asserts I11 (§11)
    JS_EXPORT_PRIVATE void bumpAndReclaim();
};

} // namespace JSC
```

Contract notes (normative): object-model cells readable under I5b; `runStopTheWorldSafepointHooks()` fires once per collection (not per stop window) in BOTH protocols: legacy (`!ISS`, incl. option-off) in `runEndPhase` just before `didFinishCollection()` (`Heap.cpp:1789`; mutator suspended; assert `worldIsStopped()`); shared at §10 step 7 (=OM §6's bar). **Legacy epoch reclamation (r11):** the same legacy site then runs the §11 reclaim sequence (`!ISS`) — phase-1 frees retired items; no-op when none (I10 exemption); adapter passes `safepointEpoch().current()`; `retire`: any thread, 10a/10b locks OK, ranks 7-9b never (§13.10f), async-signal-unsafe; secondary-context creators: `GCClient::Heap(sharedServer)`+ACT/DCT; ALL indefinitely-blocking primitives RHA/AHA-bracketed; vmstate `StructureAllocationLocker`=STW-forbidden scope+`GCDeferralContext` (I14/L5). Options per §13.2.

## 10. Shared-mode collection: requester-conducted stop

Binding (derivations: history App. B):

1. **Trigger & ticketing** - CSAC/RCAC (or CIND when shared) enqueues a `GCRequest` ticket under `m_threadLock` (§10B.1; L2/I14); no fire-and-forget collections when shared.
2. **Election:**
   ```
   while (T not served) { // check under m_threadLock
     if (GCL.tryLock()) { // no lock held (no 5->2 inversion)
       under m_threadLock: if (T served) { unlock GCL; break; } GCA = true;
       conduct(); // steps 3-9
       GCL.unlock();
       under m_threadLock: GCA = false; GEC.notifyAll();
     } else { // follower
       releaseHeapAccess(); // REQUIRED for the step-4 barrier
       under m_threadLock:
         while (m_lastServedTicket < T && GCA) GEC.wait(*m_threadLock);
       acquireHeapAccess(); // F8: blocks if stop pending
     }
   }
   ```
   Normative: the serve path (`m_lastServedTicket++`, `Heap.cpp:1805-1810`) also `GEC.notifyAll()` (followers never wait on `m_threadCondition`); late-granted tickets re-loop and win `tryLock`; sync callers never wait on a ticket holding access, never need NVS. **GCL-busy rule:** `tryLock` failed∧GCA false (a `JSThreadsStopScope` holds GCL, §10C(e))=>no spinning: release access; **timed** wait on GEC (≤1ms; never untimed); VM-backed requesters poll VMTraps each iteration (park in NVS if a VMM stop pends).
3. **Stop request** - seq_cst `GSP = true`; release own access; `VMManager::requestStopAll(StopReason::GC)` (async; own trap bit harmless).
4. **Access barrier** - under GBL wait on GBC until **every client is NoAccess** (F8; entered mutators park: traps->NVS->§13.5a/g, kept parked §13.5b; others release at next RHA/SINFAC poll; acquirers revert-and-block). Set WSAC (F7).
5. **Flush** - every client's `threadLocalCache().stopAllocating()`+iso equivalent (I2 exception).
6. **Stacks** - `gatherStackRoots` (`Heap.cpp:906-910`) generalized: conductor's `CurrentThreadState`+`tryCopyOtherThreadStacks` per other registered thread (`MachineStackMarker.h:60-73`).
7. **Collection+safepoint work** - full synchronous collection per §10B; then, still stopped, the §11 reclaim sequence (I11).
8. **Resume (heap)** - `resumeAllocating()` on all client caches; clear WSAC (under GBL); seq_cst `GSP = false`; broadcast GBC; re-acquire own access.
9. **Resume (VMM)** - `requestResumeAll(StopReason::GC)` (§13.5e); parked mutators exit (GC never latched, §13.5c) or re-latch (§13.5f); didResume per §13.5a. Conductor releases GCL, re-checks its ticket.
10. One client / option off - existing `m_worldState` protocol; never concurrent with §10 (I10/I13/I15). Legacy collections still run the hooks **+ §9 legacy epoch reclamation** at the `runEndPhase` site (T5/T7; sole option-off delta, I10 exemption).

Heap resume (8) strictly precedes VMM resume (9) (normative; regressions: §12.1).

### 10A. Heap-access protocol

Replaces `hasAccessBit` (`Heap.h:986-987`)+`Heap::acquireAccess/releaseAccess/stopIfNecessary` (`Heap.cpp:2143-2237`) with per-client `Atomic<uint8_t> m_accessState` ∈ {HasAccess, NoAccess} (+debug `m_accessOwner`). Wiring: `JSLock::didAcquireLock/willReleaseLock` call the server pair today; once ISS they forward to the *main client's* AHA/RHA (owned `Heap.cpp`); pre-ISS kept; secondary clients wired by creator (§9). **Rule: no thread reads or writes the shared JS heap except holding some client's heap access.** Exemption (jit R1.i): a JSThreads conductor inside its `JSThreadsStopScope` stopped window may WRITE heap memory without access (world stopped, GCL held); allocation stays forbidden (I4(c); OM O4 pre-allocates). AHA=F8+re-stamp `m_accessOwner`+ensure `addCurrentThread()` (I4(b)); RHA=seq_cst exchange->NoAccess, signal barrier only if GSP; SINFAC=release->wait->re-acquire when shared+pending. Per-client `bool m_releasedByGCPark` (GCH; written only inside NVS) pairs the §13.5a hooks. Single-client mode unchanged: never blocks.

**10A.1 Current-client TLS (server->client seam).** One `WTF::ThreadSpecific<GCClient::Heap*>` slot (`GCThreadLocalCache.cpp`), read via `currentThreadClient()`. Set by ACT; cleared by DCT; once ISS, `JSLock::didAcquireLock`'s forwarding re-stamps it before AHA (migration-safe; keyed on sticky ISS, not the option); RHA does NOT clear it. Resolves: CSAC/RCAC/SINFAC find the caller's client; CIND-when-shared from `allocateSlowCase(JSC::Heap&,..)`. `currentThreadIsAllocatorOwner(la)`: **`!ISS`** (option off, or on pre-sticky)=>today's predicate `vm().currentThreadIsHoldingAPILock()` (TLS may be unset; I10); **ISS**=>`currentThreadClient()` non-null∧`la` in its TLC `m_perDirectory` (non-iso AND iso, §5.3; debug cross-check `m_accessOwner`).
### 10B. Riptide bridge

Rules (1)-(7) NORMATIVE: (1) RCAC ticketing like `requestCollection` (`Heap.cpp:2343-2364`) minus legacy `stopIfNecessary()`; conn-bit set idempotently via `exchangeOr`, asserting only `!m_collectorThreadIsRunning`; never assert served==granted mid-drain; conductor drains all granted tickets; `:2346-2348` asserts->access-holder-or-conductor; (2) conductor runs **as the mutator** (`GCConductor::Mutator`; phase loop per `Heap.cpp:2073-2090`), may be VM-less; phase-loop `vm()` asserts gain `|| WSAC` (T5b/T9); (3) collector thread quiesced once shared (I15; `shouldCollectInCollectorThread` `:1360-1366` stays false; `stopTheMutator`/`resumeTheMutator` `:1969,2008` unreachable, asserted); (4) attach quiescence: §10B.4; (5) T5b audit, sites gaining `|| WSAC`: `checkConn` `:1390-1401` (conn always Mutator); `:2346-2348`; periphery fence bookkeeping `:1883-1968`+`setMutatorShouldBeFenced` `:935,1024` (always-fenced once ISS); `handleNeedFinalize` `:2240`; `vm()` uses (T9); `stoppedBit`/`hasAccessBit`=main-client only, superseded by WSAC; `mutatorWaitingBit`/`needFinalizeBit` unchanged; no JS finalizers in the stop window; (6) compiler threads §10.7/I11; (7) deviation-4 features disabled (§3.4; T8 vs I5b/I16).

**10B.4 Attach quiescence (I13/I15).** `add` making size()>1, BEFORE taking HCS `m_lock` (rank 6; 5 outer): under `*m_threadLock`, **timed re-check loop** (≤1ms waits on GEC; never `m_threadCondition`) until `m_lastServedTicket == m_lastGrantedTicket && !m_collectorThreadIsRunning && m_currentPhase == NotRunning`; then, same critical section, set sticky ISS (new triggers re-route, I15); release; insert under `m_lock`. **Liveness (cross-spec):** never block indefinitely on an attaching client while your VM has granted-unserved tickets - creators poll CIND/SINFAC until attach completes. Regression: `attachWithPendingTicket`.

### 10C. Other stop reasons

GC never enters VMM's latch/dispatch (§13.5c; `:462-463` assert stays). Shapes (regressions: §12.1): (a) debugger-first (parked VMs hold access): §13.5g(i) re-fires willPark in the wait loop; barrier completes during the debugger stop; didResume re-acquires at NVS exit. (b) non-GC stop mid-GC-stop REQUIRES §13.5e+f (else `:312-313`/`:328-329` hang). (c) GC in RunOne: §13.5g(ii) traps targetVM; parks via §13.5b (GC-bit check precedes `m_targetVM` shortcut); §13.5e wakes; session preserved. (d) `m_worldMode >= Stopping`: §13.5g(ii) covers `:231-233`; reduces to (a). (e) GC requester vs `JSThreadsStopScope`: §10.2 GCL-busy rule parks the requester (jit G13; JSThreads stops hold `JSThreadsStopScope` - §9, §13.10b).

### 10D. ISS reversion (r11)

A short-lived secondary client must not downgrade GC forever: `remove()` leaving size()==1 (survivor=main client) sets `m_issRevertPending`. Main client's thread at a later CIND/SINFAC poll (never inside `remove`/a stop): (1) under `*m_threadLock`, §10B.4-style timed loop until ticket-quiescent∧`m_currentPhase==NotRunning`∧GCL `tryLock` succeeds∧size()==1; (2) same section: clear ISS+flag; release GCL. Deviation-4 features+server deferral re-enable (survivor depth MUST be 0, asserted); residual `m_retired` drains via §11's legacy site; §10A.1 TLS stays stamped. Later `add` re-runs §10B.4 (I13 assert keys on current ISS). Regression: `issRevertChurn`.

## 11. Epoch reclamation (new; §12)

Global `Atomic<uint64_t> m_epoch { 1 }`; per-client `Atomic<uint64_t> m_localEpoch` (in GCH); `Lock m_retireLock; Vector<RetiredItem> m_retired;`, `RetiredItem { void* ptr; void (*destroy)(void*); uint64_t epoch; }`. `retire(p,d)` appends `{p, d, m_epoch.load(acquire)}` under `m_retireLock`. **Reclaim sequence** (sole publication/bump contexts: §10 step 7 (shared)+legacy `runEndPhase` (§9 note, `!ISS`); no mutator hook): `runStopTheWorldSafepointHooks()`->`suspendCompilerThreads()` (reclaimer's OWN pair; `Heap.cpp:2385`)->per client `m_localEpoch = m_epoch` (exact)->`bumpAndReclaim()` (release-asserts I11; skipped when `m_retired` empty)->`resumeCompilerThreads()`; destroys items with `epoch < min(m_localEpoch)`, then `m_epoch.store(old+1, release)`. DCT sets the client's epoch to `UINT64_MAX`. Compiler/GC-helper threads aren't participants. **SPEC-jit R4/CS4 refused (normative):** a non-GC bump reclaims against stale epochs; JSThreads stops enqueue a GC request instead (§13.10a). Only `void*`+destroy thunks.

## 12. Files (MINIMUM set)

Modified (`heap/`): Heap(+Inlines), BlockDirectory(+Inlines), LocalAllocator(+Inlines), CompleteSubspace, IsoSubspace, Subspace, MarkedSpace, MachineStackMarker, Allocator(+Inlines).
New: `heap/{HeapClientSet,GCThreadLocalCache,GCSafepointEpoch,SharedHeapTestHarness}.{h,cpp}`.

T3b/T8/T9 fixes may touch any heap/** file; expected: IncrementalSweeper, MarkedBlock(+Inlines), PreciseAllocation, HeapVerifier. **Carve-out 1:** `heap/StructureAlignedMemoryAllocator.cpp` gets vmstate's M9 hunk (integrator-applied); untouched here. **Carve-out 2 (§13.10g):** OM manifest-7 `mayBeSegmentedButterfly()` guards into heap/** hits (HeapVerifier, visitor/snapshot `butterfly()` readers), integrator-applied on heap's final tree (rule as M9); T8/T9 keep `butterfly()` sites greppable.

### 12.1 Multi-client harness

`SharedHeapTestHarness`: K raw `WTF::Thread`s, each a standalone `GCClient::Heap(server)` on its stack (`markStandalone()`+ACT), running C-level allocation/steal/detach/epoch loops - no JS/VM entry (zero-entered-VMs stop path); clients real=>ISS reachable. Seam: owned overloads `CompleteSubspace::allocateForClient(GCClient::Heap&, size_t, GCDeferralContext*, AllocationFailureMode)` (+ iso): client-TLC routing, no VM-coupled preludes; CIND/SINFAC ARE called; VM-taking overloads delegate. `markStandalone()` arms `RELEASE_ASSERT(!m_isStandalone)` in `vm()` (`HeapInlines.h:239-242`; T9). Scenarios: `allocationStorm`, `preciseAllocationStorm` (I16), `stealRace`, `clientChurnVsGC`, `epochReclaim`, `structureLockVsSTW` (I14), `blockedInNativeVsGC`, `syncRequesterStorm` (§10.2), `noEnteredVMsGC`, `attachWithPendingTicket` (§10B.4), `deferralVsAllocationStorm` (I17), `debuggerStopDuringSharedGC` (§10C(b)/(c)), `gcDuringDebuggerPark` (§10C(a)), `jsThreadsStopVsGCRequester` (§10C(e); real VMs via `$vm`), `issRevertChurn` (§10D). JS exposure: manifest 8 (corpus per T10).

## 13. Manifest for `INTEGRATE-heap.md` (written verbatim)

1. **`Sources.txt`** - add in `heap/` (alphabetical):
   ```
   heap/GCSafepointEpoch.cpp
   heap/GCThreadLocalCache.cpp
   heap/HeapClientSet.cpp
   heap/SharedHeapTestHarness.cpp
   ```
2. **`runtime/OptionsList.h`** - near `useGlobalGC` (`:429`):
   ```
   v(Bool, useSharedGCHeap, false, Normal, "Multiple GCClient::Heaps (threads) share one server JSC::Heap"_s) \
   v(Bool, verboseSharedGCHeap, false, Normal, nullptr) \
   ```
3. **`runtime/JSCConfig.h`** - alongside `wasmDebuggerOnStop`:
   ```cpp
   void (*gcWillParkInStopTheWorld)(VM&); // heap-owned impl; may be null
   void (*gcDidResumeFromStopTheWorld)(VM&); // heap-owned impl; may be null
   ```
4. **`runtime/VMManager.h`** - next to `setMemoryDebuggerCallback` (`:274`):
   ```cpp
   JS_EXPORT_PRIVATE static void setGCParkCallbacks(void (*willPark)(VM&), void (*didResume)(VM&));
   ```
5. **`runtime/VMManager.cpp`** - inert when callbacks null / GC bit never set. Hunks a-g+the jit-M4 merge-order/Coordination note = `SPEC-heap-annex.md` §A5, FROZEN NORMATIVE, applied verbatim; `§13.5x` cites herein = annex §A5 hunk x (summary: a park/resume hooks; b GC-bit keep-parked first; c latch exclusion; d `setGCParkCallbacks`; e resume notifyAll; f re-latch; g re-check while parked; merge heap-then-jit-R1.c; resume tail M4 fence then didResume).
6. **`runtime/VM.cpp`** - none (registration in GCH ctor/dtor); only permitted build-fix: `#include "HeapClientSet.h"`.
7. **`CMakeLists.txt`** - none (sources derive from `Sources.txt`).
8. **`tools/JSDollarVM.cpp`** - `$vm.sharedHeapTest(name, threads, iters)`->`SharedHeapTestHarness::run(vm.heap, ...)`; guarded by the option; logic in `heap/**`.
9.-10. **Informational**: 10. CRs: a. jit R4/CS4 REFUSED for JSThreads stops (bumps only at §10.7/legacy `runEndPhase`, §9 note; JSThreads stops enqueue a GC request); b. jit CS2 RESOLVED: `JSThreadsStopScope` (§9) = the GCL bracket (overlap=>§10.2 GCL-busy rule, G13); c. TLC-aware emission: deviation-7 charter; d. OM `releaseQuarantinedSlots` via `addStopTheWorldSafepointHook` (init adapter; fires per §9 note; OM r13: adapter bumps OM's PER-SERVER-HEAP quarantine epoch for the hook's Heap, OM §6); e. siblings cite anchors; f. lock-order reconciliation (jit §7/OM §6)=§6 rows 6b/9b/10a<10b/leaf (jit leaf-note superseded; `retire()` under 10a/10b OK, 7-9b never; OM 8e closed); g. OM manifest-7 heap/** guards applied by the integrator on heap's final tree (§12 carve-out 2).

11. **`wasm/js/JSWebAssemblyInstance.cpp`** - ctor `hasGCObjectTypes()` block (`:135-141`), before `prepareAllAllocators()`: `RELEASE_ASSERT(!Options::useSharedGCHeap())` - wasm-GC+shared heap unsupported phase 1 (§5.5).

No `VM.h`/`JSGlobalObject.*` edits.

## 14. Ordered task list (one agent)

1. **T1 - Scaffolding.** Four new file pairs (compiling stubs); §5.1 server members, hook registry, accessors, sticky ISS (one-shared-server assert, I13), STW-forbidden counters; write §13 into `INTEGRATE-heap.md` FIRST.
2. **T2 - Client registration & access.** Ctor/dtor registration (sticky switch §10B.4); ACT/DCT/`markStandalone`; AHA/RHA (F8 incl. step 0+revert); SINFAC; access forwarding; §10A.1 TLS+`currentThreadClient`/`currentThreadIsAllocatorOwner`; `vm()` standalone assert; `lastChanceToFinalize()`.
3. **T3 - Block handout.** §5.2; resolve the `:138`/`:170-183` FIXMEs; atomic accounting; per-client deferral depth (§5.4/I17); gate `performIncrement`+activity callbacks (§5.4). **T3b:** §5.6 locking; audit precise-registry readers vs I16.
4. **T4 - TLC.** §5.3 indices/flat table/lazy growth/dedup; re-pointed `allocatorFor`/`allocate`; `allocateForClient` seam; §5.5 never-populate rule+`prepareAllAllocators` assert (+`MustAlreadyHaveAllocator` audit); teardown (I9); verify I10 via option-off codegen diff.
5. **T5 - Stop protocol.** §10 steps 1-9 in `heap/Heap.cpp`: election (§10.2 flag+condition+GCL-busy rule), barrier, flush, hooks **+ the legacy `runEndPhase` hook+reclamation site (§9 note; assert `worldIsStopped()`)**, §10D revert, `gcWillPark`/`gcDidResume` impls (via manifests 3-5); `JSThreadsStopScope`; CSAC/RCAC (§10B.1; I15 re-routing); WSAC. **T5b:** §10B.5 audit, patches tagged `// SharedGC:`.
6. **T6 - N-stack scan.** `gatherStackRoots` per §10.6; `addCurrentThread` enforcement in AHA (I4(b)).
7. **T7 - Epoch reclamation.** `GCSafepointEpoch` per §11 incl. the legacy `runEndPhase` reclamation; unit tests: conducted cycle's own periphery suspension does NOT satisfy I11; retire->legacy-GC->free in the 1-client `!ISS` config.
8. **T8 - Stop/resume, sweeper & bitvector audit.** N-client stop/resume/prepare/sweeper interplay (conductor iterates all caches; I5 asserts); audit every `BlockDirectoryBits` reader/writer+`assertIsMutatorOrMutatorIsStopped` site (`BlockDirectory.cpp:516`) vs I5b (`IncrementalSweeper`, marker helpers, `MarkedBlock::sweep`, `didConsumeFreeList`, §5.3 TLC teardown)+`m_newActiveWeakSets` (§5.2(2)); fix or lock each.
9. **T9 - `vm()` audit.** Classify every `vm()` use in `heap/**`: main-VM-only / per-client iteration (`clientSet().forEach`) / conductor-context OK (incl. VM-less); audit GCH `vm()`+`GCClient::IsoSubspace` uses vs the standalone assert. Tag `// SharedGC:`.
10. **T10 - Tests.** §12.1 scenarios+`JSTests/threads/heap-*.js` corpus (I1, I8, I12-I14, I16); epoch unit test (I11); race-amplifier hooks at handout, steal, detach, access release/acquire (F8 window), precise registration, epoch bump; corpus no-JIT (TSAN)+JIT-on (§5.5); bench gate option-off (I10). Under the GIL stub I2 holds (JSLock hand-off re-stamps `m_accessOwner`).

Done = every I1-I17 has a test/assert; `LocalAllocator.cpp` FIXMEs gone; option-off perf-/behavior-identical to `main` (sole delta: §10.10 hook+reclaim call); harness reaches ISS; `INTEGRATE-heap.md` = exactly §13. **Gating (normative).** Prep (r13): ALL five specs' OptionsList.h entries (incl. manifest 2)+jit M2a/M4a are orchestrator-PRE-APPLIED to the shared tree before fan-out (jit §10). Other §13 symbols exist only via the manifest: build/test in a throwaway overlay (manifests 3-5, 8, 11 applied verbatim, written first, T1) in a private worktree; overlay hunks NEVER committed (commits = owned paths only; no off-spec shims). Overlay convention extends to vmstate/OM/api non-Options hunks. In-overlay bar = §12.1 scenarios+I11 unit test; at integration: §10C scenarios, JS corpus, T5b.

§§15-24 review logs: history (non-normative); outcomes folded into §§2-14.
