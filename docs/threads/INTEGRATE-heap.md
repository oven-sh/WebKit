# INTEGRATE-heap.md — shared-file manifest for the heap workstream

This file is SPEC-heap.md §13 written verbatim (item 5 expanded with the FROZEN
NORMATIVE hunks from `SPEC-heap-annex.md` §A5). The integrator applies these hunks
exactly as written; the heap workstream itself never edits these files.

Status (T1): manifest written; items 1-5, 8, 11 are required for the heap scaffolding
and later heap tasks to build/run. Item 2 (`OptionsList.h`) is a prerequisite for
compiling `heap/HeapClientSet.cpp` and later tasks (`Options::useSharedGCHeap()` is
referenced from heap/** already).

Status (T2): client registration & access landed entirely inside heap/** — no new
shared-file hunks. Notes for the integrator:
- Item 6 stands as written: GCClient::Heap registration is now live in its
  ctor/dtor (`heap/Heap.cpp`); `runtime/VM.cpp` needs no edit (the permitted
  build-fix `#include "HeapClientSet.h"` should not be necessary since
  registration code lives in Heap.cpp, but remains permitted).
- Item 5a's heap-owned willPark/didResume callback bodies are NOT yet
  installed (T5); the per-client `m_releasedByGCPark` field they pair with
  exists as of T2 (`GCClient::Heap`, heap/Heap.h).
- Items 2 (`Options::useSharedGCHeap`) is now also referenced from
  `heap/HeapClientSet.cpp` (sticky-switch gate), in addition to T1's uses.

Status (T3 + T3b): synchronized block handout (§5.2), atomic accounting +
per-client DeferGC depth + performIncrement/activity-callback gating (§5.4/I17),
and precise-allocation locking (§5.6) landed entirely inside heap/** — NO new
shared-file hunks. Notes for the integrator:
- The two `LocalAllocator.cpp` GlobalGC FIXMEs (block-handout synchronization,
  webkit.org/b/181635) are resolved by the per-server MSPL taken in
  `allocateSlowCase` when `isSharedServer()`; option off is branch-gated to
  today's code (I10).
- `BlockDirectory::tryAllocateBlock` gained a leading
  `const AbstractLocker&` MSPL-token parameter (§5.2(3)). Sole caller is
  `LocalAllocator::allocateSlowCase` (in heap/**); no out-of-heap callers
  exist, so no shared-file fallout.
- Heap counters (`m_*BytesAllocatedThisCycle`, `m_bytesAbandonedSince...`,
  `m_blockBytesAllocated`) and `MarkedSpace::m_capacity` are now
  `std::atomic<size_t>` with relaxed ordering; all accessors keep their
  existing signatures, so out-of-heap readers (e.g. `runtime/VM.cpp`
  opportunistic-task heuristics via `totalBytesAllocatedThisCycle()`)
  need no edits.
- `GCClient::Heap` gained `m_deferralDepth` (§5.4/I17); the sticky switch in
  `Heap::noteSharedServerSticky` migrates any open server depth to the main
  client so DeferGC scopes straddling the ISS flip stay balanced. No VM.h/VM.cpp
  edits: DeferGC routing lives in heap/HeapInlines.h + heap/Heap.h inlines.
- T3b reader audit recorded at the registry declaration in
  `heap/MarkedSpace.h` (I16); mutator-path writers now lock MSPL
  (`CompleteSubspace::tryAllocateSlow`/`reallocatePreciseAllocationNonVirtual`,
  `PreciseSubspace::tryAllocate`, `MarkedSpace::enablePreciseAllocationTracking`)
  or assert it (`MarkedSpace::registerPreciseAllocation`,
  `IsoSubspace::tryAllocateLowerTierPrecise`).
- Deferred to T8 (per spec task split): `didConsumeFreeList`/`MarkedBlock`
  bitvector flips outside the MSPL section, sweeper interplay, and the
  remaining `assertIsMutatorOrMutatorIsStopped` sites (I5b audit).

Status (T4): TLC (§5.3), the re-pointed `allocatorFor`/`allocate` path, the
`allocateForClient` seam (§12.1), the §5.5 never-populate rule, and teardown
(I9) landed entirely inside heap/** — NO new shared-file hunks. Notes for the
integrator:
- `BlockDirectory`'s constructor changed `(size_t)` -> `(Heap&, size_t)` per
  §5.3. Both construction sites are in heap/** (`CompleteSubspace`,
  `IsoSubspace`); grep found no out-of-heap constructions, so no shared-file
  fallout. The directory also gained `m_tlcIndex`/`tlcIndex()` (iso =
  `invalidTlcIndex`) — layout change only; LLInt/JIT extract offsets
  symbolically.
- §5.5 is enforced at three points, all heap-owned: (1) `allocatorForSlow`
  never materializes a server `LocalAllocator` when `useSharedGCHeap` (it
  still creates directories, directoryLock-only, so JIT threads keep working);
  (2) `Heap::verifyServerNonIsoAllocatorsNeverMaterialized()` RELEASE_ASSERTs
  at the second-client attach (called from `noteSharedServerSticky()`);
  (3) `CompleteSubspace::prepareAllAllocators()` RELEASE_ASSERTs
  `!Options::useSharedGCHeap()` — manifest item 11 (the wasm-GC instantiation
  reject in `JSWebAssemblyInstance.cpp`) remains REQUIRED as the user-facing
  guard; the assert is the backstop.
- `MustAlreadyHaveAllocator` audit (T4): repo-wide grep finds NO caller
  passing this mode (mentions only in `AllocatorForMode.h` and
  `CompleteSubspace.h` itself). The existing `RELEASE_ASSERT(result)` now
  doubles as the shared-mode trap (the table is never populated when the
  option is on), with a comment at the switch.
- JS-tier emitters need no edits for §5.5: `AssemblyHelpers::emitAllocate`
  jumps straight to the slow path for a null constant allocator,
  `JITAllocator::Variable` null-checks at runtime (the baked
  `offsetOfAllocatorForSizeStep` loads read the never-populated table), and
  `ObjectAllocationProfileInlines.h` already guards `if (allocator)` on its
  `EnsureAllocator` result. Slow paths funnel into
  `CompleteSubspace::allocate(VM&)`, which routes to the calling VM's client
  TLC when the option is on (`vm.clientHeap`, the §5.3 vm-relative chain —
  GIL-phase-only per deviation 6/7; the TLC-aware-emission charter owns the
  per-THREAD addressing).
- I10 (option off): the only fast-path delta is one
  `Options::useSharedGCHeap()` branch in `CompleteSubspace::allocate` /
  `tryAllocateSlow` (branch-gating is the I10-sanctioned form). Cold-path
  deltas (directory creation only): TLC index reservation + a
  per-size-step directory table fill now happen in both modes, and
  `m_firstDirectory`/`registerDirectory` publication moved before the server
  allocator-table fill (all under the same `directoryLock` critical section
  as before). Option-off codegen diff (I10 verification) could NOT be run in
  this environment — flag for the in-overlay build/bench gate.
- Teardown (I9): `GCThreadLocalCache::stopAllocatingForGood()` runs per-slot
  `stopAllocatingForGood()` under MSPL, then unlinks via the new
  `BlockDirectory::detachLocalAllocator()` (rank 7 -> 8). It covers the
  registered `GCClient::IsoSubspace` allocators too, resolving the GlobalGC
  FIXME (`GCClient::Heap::lastChanceToFinalize`). Option off: TLC is empty
  (iso registration is option-gated), so server teardown is unchanged.
- Iso coverage for §10A.1: `GCClient::Heap::registerIsoSubspaceLocalAllocators()`
  (ctor, before `clientSet().add`) plus registration in the dynamic
  iso-subspace `name##Slow()` macro (`heap/Heap.cpp`) enter each
  `GCClient::IsoSubspace` `LocalAllocator` into the client TLC's
  `m_perDirectory` as lookup-only entries.

Status (T5 + T5b): the §10 stop protocol (steps 1-9), §10B.1 CSAC/RCAC
ticketing with I15 trigger re-routing, the §10.2 election (flag + condition +
GCL-busy rule), the §10.4 access barrier / WSAC, the legacy `runEndPhase`
hook+reclamation site (§9 note), the §10D ISS reversion poll, the manifest-5a
`gcWillParkInStopTheWorld`/`gcDidResumeFromStopTheWorld` impls, and the
`JSThreadsStopScope` access-precondition assert landed entirely inside
heap/** — NO new shared-file hunks beyond what items 2-5 already specify.
Notes for the integrator:
- Items 3-5 are now LOAD-BEARING: `Heap::noteSharedServerSticky()` calls
  `VMManager::setGCParkCallbacks(&Heap::gcWillParkInStopTheWorld,
  &Heap::gcDidResumeFromStopTheWorld)` (item 4's declaration; item 3's
  storage — review round 4: now the item-5d VMManager.cpp statics, the
  former item-3 JSCConfig slots are DROPPED (post-freeze SIGSEGV); item 5's
  notifyVMStop call sites / GC-bit behavior).
  `conductSharedCollection()` calls
  `VMManager::requestStopAll/requestResumeAll(VMManager::StopReason::GC)`
  (the enum value already exists in-tree; the GC-bit keep-parked, latch
  exclusion, resume notify, re-latch and re-check hunks 5b/c/e/f/g make it
  behave). All call sites are tagged `// THREADS-INTEGRATE(heap)`.
  Heap.cpp now includes `VMManager.h`, `VMTraps.h` and
  `StopTheWorldCallback.h` (all in-tree headers; no manifest change).
- §10.2 election followers poll `vm.traps().needHandling(NeedStopTheWorld)`
  and park via `VMManager::singleton().notifyVMStop(vm,
  StopTheWorldEvent::VMStopped)` each iteration (GCL-busy rule); standalone
  (§12.1) clients skip the poll.
- The §10 step-5 flush and step-8 resume are carried by the conducted
  cycle's own `stopThePeriphery()`/`resumeThePeriphery()`: every client's
  LocalAllocators (TLC + iso) are linked into the shared BlockDirectories'
  `m_localAllocators` lists, so `m_objectSpace.stopAllocating()` flushes all
  clients (I2 exception). `conductSharedCollection()` additionally runs the
  idempotent per-client `threadLocalCache().resumeAllocating()` pass at step
  8 before clearing WSAC/GSP. T8 owns the deeper stop/resume/sweeper audit.
- Deviation 4 enforcement: `runFixpointPhase` never schedules
  `CollectorPhase::Concurrent` once shared (the conducted collection is
  fully synchronous; world suspended Begin..End); `runConcurrentPhase`
  asserts `!isSharedServer()`.
- §10B.5 audit patches (tagged `// SharedGC:`): `checkConn` Mutator case
  gains `|| worldIsStoppedForAllClients()`, Collector case
  RELEASE_ASSERTs `!isSharedServer()`; `stopTheMutator`/`resumeTheMutator`
  RELEASE_ASSERT `!isSharedServer()`; `shouldCollectInCollectorThread`
  returns false when shared (collector thread quiesced, I15);
  `requestCollection` asserts `!isSharedServer()` and its :2346-2348 asserts
  tolerate WSAC (the access-holder-or-conductor form lives in
  `requestCollectionShared`); `setMutatorShouldBeFenced` is always-fenced
  once shared (raised at the ISS flip in `noteSharedServerSticky`);
  `handleNeedFinalize`/periphery bookkeeping carry contract comments
  (stoppedBit/hasAccessBit are main-client-only, superseded by WSAC; no JS
  finalizers in the stop window). The serve path in `runEndPhase` now also
  `m_gcElectionCondition.notifyAll()` (§10.2 normative).
- Legacy (`!isSharedServer()`, incl. option OFF) collections now run
  `runSafepointHooksAndReclaim()` in `runEndPhase` just before
  `didFinishCollection()` — the sole option-off behavior delta (I10
  exemption sanctioned by §9/§10.10). The reclaim bracket adopts the cycle's
  compiler-thread suspension when this thread already holds it
  (JITWorklist's suspension lock is not recursive); T7 finishes the I11
  RELEASE_ASSERT inside `GCSafepointEpoch::bumpAndReclaim()`.
- `GCClient::Heap::m_releasedByGCPark` is now written exactly as item 5a
  specifies (only inside notifyVMStop via the two hooks).
- `Heap::m_issRevertPending` became `Atomic<bool>` (writes still under
  `*m_threadLock`; relaxed reads are the SINFAC poll hint) — heap-internal,
  no manifest impact.

Status (T9): `vm()` audit landed entirely inside heap/** — NO new shared-file
hunks, comments/classification tags only (zero behavior change). Notes for
the integrator:
- Audit legend lives at `Heap::vm()` in `heap/HeapInlines.h` (deviation 3:
  the server is by-value in the main VM, so `vm()` = "the main mutator VM"
  from ANY thread, incl. VM-less standalone clients/conductors). Every vm()
  use in heap/** carries (or inherits via a tagged helper) a
  `// SharedGC (T9)` tag classifying it as one of:
  - main-VM-only (API-lock/entryScope/legacy-!ISS coupled; unreachable or
    superseded once ISS, or GIL-phase sound via JSLock migration I2),
  - per-client iteration (clientSet().forEach / currentThreadClient()),
  - conductor-context OK (VM-global or server-coupled state, thread-agnostic
    round-trips, or self-guarded calls like sanitizeStackForVM()).
- Helper chains tagged at their definitions (inherit downward):
  `HeapCell::vm()`, `CellContainer::vm()`, `MarkedBlock(::Handle)::vm()`,
  `WeakSet::vm()`, `PreciseAllocation::vm()`, `AbstractSlotVisitor::vm()`,
  `HandleSet::vm()`, `HeapProfiler::vm()`.
- Audit outcome: NO site required a clientSet().forEach() conversion phase 1
  — the only true per-client iteration sites already exist from T5-T7
  (election trap poll `client->vm()`, guarded `!m_isStandalone`; epoch
  publication; per-client cache stop/resume). Per-VM caches cleared in
  `finalize()` stay singular post-GIL (deviation 8: one VM per thread
  group); per-THREAD state (topCallFrame, exception roots, scratch buffers)
  is flagged in-tag as VMLite/vmstate-charter follow-ups, not heap work.
- GCH vm() vs the standalone assert: the sole client->vm() caller in heap/**
  (runSharedGCElection's trap poll) is `!m_isStandalone`-guarded;
  `GCClient::IsoSubspace` is vm()-free by construction (audit note at its
  ctor, `heap/IsoSubspace.cpp`); VM-coupled entries go VM->client, never
  client->vm(). No unguarded standalone-reachable vm() call remains.
- `sanitizeStackForVM()` (runtime/VM.cpp) self-guards (early-returns unless
  the calling thread holds the main VM's API lock) — relied upon by the
  conductor-context classifications in `Heap.cpp` and
  `LocalAllocatorInlines.h`; no shared-file change needed.

Status (T10): tests — §12.1 scenarios, JS corpus, amplifier hooks, bench gate
— landed inside heap/** plus ten JS corpus files at `JSTests/threads/heap-*.js`.
OWNERSHIP NOTE (review round 1): `JSTests/threads/heap-*.js` was NOT part of
the heap workstream's declared owned-path grant (`Source/JavaScriptCore/heap/**`
+ this file only); the earlier "(both owned)" claim here was wrong and is
retracted. The ten files are nonetheless this part's review deliverables, are
prefix-namespaced (top-level `heap-*.js`), and collide with no other part's
corpus (api/atomics/races/vmstate/jit corpora live in subdirectories). ACTION
for the orchestrator/integrator: either amend the heap part's grant to include
`JSTests/threads/heap-*.js` (preferred; record the amendment here), or treat
the ten files — `heap-epoch-reclaim.js`, `heap-allocation-storm.js`,
`heap-precise-storm.js`, `heap-client-churn.js`, `heap-access-blocking.js`,
`heap-stop-interleavings.js`, `heap-deferral-storm.js`, `heap-iss-revert.js`,
`heap-option-off.js`, `heap-bench-allocation.js` — as integrator-placed
deliverables of this manifest (place them verbatim from the working tree).
The heap workstream makes no further edits to them until the grant is settled.
NO new shared-file hunks; notes for the integrator:
- `SharedHeapTestHarness` now implements ALL fifteen §12.1 scenarios
  (epochReclaim was T7's). The overlay's manifest item 8
  (`$vm.sharedHeapTest(name, threads, iters)`) is the sole JS entry point the
  corpus uses; nothing else is exposed. The harness caller contract is
  documented in `SharedHeapTestHarness.h` (main VM mutator thread, API lock
  held); scenarios release the caller's access via `ReleaseHeapAccessScope`
  around the multi-threaded sections so standalone conductors' §10.4 barriers
  complete while the calling thread is parked in join.
- JS corpus (run each file in its own jsc process; every file is vacuous-pass
  if `$vm.sharedHeapTest` is absent, i.e. on a non-overlay tree):
  - `heap-epoch-reclaim.js` — I11 unit test (T7); MUST run alone (needs the
    1-client !ISS config); deliberately option-OFF (the I10 legacy-reclaim
    exemption).
  - `heap-allocation-storm.js` — I1/I8/I12 (allocationStorm, stealRace).
  - `heap-precise-storm.js` — I16 (preciseAllocationStorm).
  - `heap-client-churn.js` — I13/§10B.4 (clientChurnVsGC,
    attachWithPendingTicket).
  - `heap-access-blocking.js` — §10A/F8/§10.2 (blockedInNativeVsGC,
    syncRequesterStorm, noEnteredVMsGC).
  - `heap-stop-interleavings.js` — §10C(a)-(e)/G13, the "real VMs via $vm"
    half (debuggerStopDuringSharedGC, gcDuringDebuggerPark,
    jsThreadsStopVsGCRequester).
  - `heap-deferral-storm.js` — I17/I14 (deferralVsAllocationStorm,
    structureLockVsSTW).
  - `heap-iss-revert.js` — §10D (issRevertChurn).
  - `heap-option-off.js` — I10: shared-mode scenarios refuse with the option
    off; epochReclaim still passes; deterministic legacy churn.
  - `heap-bench-allocation.js` — bench-gate input (BENCH output via
    `bench/harness.js`); EXCLUDE from amplify.sh campaigns (timing output).
- Run matrix (T10 / §5.5): the corpus is option-driven only — run it once in
  the no-JIT TSAN config (`TSAN.md` target; add `--useJIT=0`) and once
  JIT-on (default), per SPEC-heap.md T10. Under amplify.sh, sweep
  `--period` per AMPLIFIER.md; all heap-*.js files except the bench file
  print deterministic output.
- Race-amplifier call sites added (slow paths only, AMPLIFIER.md rules; all
  in heap/**): block handout pre-MSPL + the steal window
  (`LocalAllocator.cpp`), AHA post-CAS/pre-GSP-sample + RHA
  post-exchange/pre-signal (F8 Dekker windows) + the detach
  access-released/epoch-not-yet-MAX window (`Heap.cpp`),
  precise-registration windows in both `tryAllocateSlow` overloads
  (`CompleteSubspace.cpp`), and `retire()`'s stale-epoch window +
  `bumpAndReclaim()`'s pre-min-scan window (`GCSafepointEpoch.cpp`). The
  `RaceAmplifier.{h,cpp}` files + options are the amplifier workstream's
  (INTEGRATE-amplifier.md, already pre-applied per SPEC-heap.md §14 prep);
  heap/** only adds `#include "RaceAmplifier.h"` + `perturb()` calls, free
  when `--randomYieldPeriod=0`.
- Bench gate (I10 quantitative half): `Tools/threads/bench-gate.sh` globs
  `JSTests/threads/bench/*.js` only. REQUEST to the gate owner (not a heap
  edit — Tools/ is not heap-owned): also include
  `JSTests/threads/heap-bench-allocation.js` (same BENCH protocol) so the
  allocation slow path the heap workstream touched is in the gated set;
  re-record `baseline.json` from a pre-threads `main` jsc when adding it.
- Bug found by issRevertChurn and fixed in `Heap.cpp` (owned):
  `noteSharedServerSticky` now clears a STALE per-client HasAccess on the
  main client at a RE-flip when the legacy `hasAccessBit` is clear — after a
  §10D reversion the legacy protocol owns access tracking, so the per-client
  state can be left HasAccess with no holder; without the fix the first
  §10.4 barrier after a re-flip can wait forever. Same quiescent critical
  section as the existing migration; no shared-file impact.
- `Heap.h` (owned) gained `friend class SharedHeapTestHarness;` (the
  standalone scenarios drive the private per-client deferral-depth routing
  for I17/I14 directly).
- Done-criteria cross-check (SPEC-heap.md §14): every I1-I17 now has a
  test/assert (I1/I8/I12 pattern rings + storm scenarios; I2/I4/I5/I5b/I6/
  I7/I9/I15 asserts from T2-T8 exercised by every conducted stop in the
  corpus; I3 TLC dedup asserts under allocationStorm; I10
  heap-option-off.js + bench gate; I11 epochReclaim; I13 clientChurnVsGC +
  attachWithPendingTicket; I14 structureLockVsSTW; I16 preciseAllocation
  storm; I17 deferralVsAllocationStorm); the LocalAllocator.cpp FIXMEs are
  gone (T3); the harness reaches ISS (any multi-client scenario); this file
  is §13 + status notes.

Status (review round 1): five code fixes landed, entirely inside heap/** —
NO new shared-file hunks; manifest item 8 reworded (see below). Summary for
re-review and the integrator:
- §10B.4 flip soundness (`Heap::noteSharedServerSticky`): the attach
  quiescence loop now ALSO requires "no thread holds legacy heap access
  (hasAccessBit clear) OR this thread holds the main VM's API lock", so no
  legacy mutator can be mid-allocation-slow-path (MSPL-less) at the ISS flip;
  and the flip pins hasAccessBit in `m_worldState` permanently (the "§10B.4
  poison"), so a stale legacy inline `acquireAccess()` CAS can never succeed
  once shared — it falls into `acquireAccessSlow()`, whose new leading ISS
  re-check is ordered after the flip by the failed seq_cst CAS read of the
  poison (synchronizes-with), and forwards. `releaseAccessSlow()` gained the
  mirror backstop. The previously tolerated "different thread holds legacy
  access during a harness-thread flip" branch is GONE (now waited out).
- Teardown order (`GCClient::Heap::~Heap`): lastChanceToFinalize() now runs
  FIRST, with the client's heap access held (acquired if needed; F8 parks
  across a pending stop) and the client still registered; detach/release and
  `clientSet().remove()` follow. This restores "MSPL holders always hold
  access" on the teardown path — a conducted stop can no longer run
  concurrently with TLC stopAllocatingForGood(). The `~GCThreadLocalCache`
  straggler re-run is documented as a structural no-op.
- §10D reversion TOCTOU (`Heap::pollIssRevertIfNeeded` +
  `HeapClientSet::withSizeUnderRegistryLock`, new): the size()==1 re-check
  and the ISS clear are now one atomic step under the rank-6 registry lock,
  the same lock under which `add()` re-checks `isSharedServer()` — closing
  the interleaving that could yield two registered clients with ISS false.
  The poll also disarms the stale hint when size > 1.
- I13 add-side (`HeapClientSet::add`): an insert on an already-shared server
  now holds GBL (rank 4) and waits `!worldIsStoppedForAllClients()` first,
  mirroring `remove()` — the registry is frozen inside a stop window on both
  sides, as `GCSafepointEpoch::bumpAndReclaim` documents it relies on.
- `SharedHeapTestHarness.h` header comment aligned with the item-8 rewording
  (unconditional $vm registration; per-scenario option gating).

Status (review round 2): six fixes, entirely inside heap/** + this file —
NO new shared-file hunks beyond the item-5 rewrite below (same semantics,
now literal text). Summary for re-review and the integrator:
- Per-thread mutator state (blocker): `JSC::Heap::m_mutatorState` was a
  single plain server field; two clients in allocation slow paths would trip
  `AllocatingScope`'s RELEASE_ASSERTs and race the save/restore scopes.
  `GCClient::Heap` gained a per-client `m_mutatorState`;
  `Heap::mutatorStateSlot()` (mirrors `deferralDepthSlot()`) routes the
  calling thread to its client's slot once ISS, server field otherwise (no
  client TLS stamp => server field, i.e. today's behavior for the legacy
  collector thread and option-off). `mutatorState()` reads the calling
  thread's slot; the four scopes (Allocating/Sweeping/Collecting/Running)
  cache the slot REFERENCE at ctor so a flip/reversion mid-scope cannot
  split ctor/dtor across slots. Readers audited: both mutatorState()
  switches (`collectIfNecessaryOrDefer`, SINFAC) treat Running/Allocating
  identically and Sweeping/Collecting identically, so per-thread semantics
  is exactly what each call site wants; `JSCell.cpp:179` reads the calling
  (API-lock-holding) thread's slot — correct, no out-of-heap edits needed.
- Directory-list construction race (blocker): `LocalAllocator`'s ctor links
  into the shared server directory's `m_localAllocators` BEFORE the client
  is published by `clientSet().add()` (and before any access is held), so
  the list mutates outside any stop-window exclusion.
  `BlockDirectory::stopAllocating/prepareForAllocation/resumeAllocating/
  stopAllocatingForGood` now hold `m_localAllocatorsLock` (rank 8) around
  their traversals — unconditionally (collection-time paths; uncontended
  when single-threaded). Lock order acyclic: per-allocator work inside takes
  only BVL/block-internal locks (ranks 9/9b); the appending ctor takes
  nothing inside rank 8. The misleading "registration is
  owner-thread-private" ctor comment is corrected in `Heap.cpp`.
- Attach-side epoch stamp (major, two findings): `attachCurrentThread()` no
  longer stores `current()` into `m_localEpoch` at all. The store was taken
  before access acquisition and could land stale across two stop windows,
  tripping `RELEASE_ASSERT(minLocalEpoch >= oldEpoch)` in
  `bumpAndReclaim()`. MAX-until-first-stamp is safe: the sole consumer (the
  min scan) always runs after the same stop window's stamping loop has
  overwritten every registered client's value, and MAX is only ever MORE
  conservative. New invariant (documented at the member): `m_localEpoch` is
  written ONLY by the conductor's stamping loop (world stopped) and by
  `detachCurrentThread` (MAX).
- §10D revert vs deferral (major): `pollIssRevertIfNeeded()`'s quiescent
  condition now also requires `!client->m_deferralDepth` — a SINFAC poll
  arriving inside an open DeferGC scope defers the REVERT (retry at a later
  poll) instead of tripping the RELEASE_ASSERT. The assert stays, now
  licensed by the condition.
- ISS-flip liveness (major): the §10B.4 quiescence loop gained a
  release-visible diagnostic (one dataLogLn after >5s of waiting on a
  foreign legacy access holder), and the obligation is now a NORMATIVE
  cross-part contract (next section).
- Item 5 (major): hunks 5a/5d/5e/5g rewritten below as literal ready-to-
  paste text against the current `runtime/VMManager.cpp` (5b/5f were
  already literal; 5f and 5g(i) are now folded into ONE combined literal
  loop so the integrator never hand-merges them). Where the item-5 text
  below and the SPEC-heap-annex.md §A5 prose differ in FORM, this file is
  the integrator's source of truth (the annex is frozen; semantics
  unchanged).

Cross-part contract: ISS-flip liveness (NORMATIVE; review round 2)
- The §10B.4 attach quiescence loop (`Heap::noteSharedServerSticky`) cannot
  complete while any thread other than the flipping thread holds legacy
  heap access (`hasAccessBit`), unless the flipping thread holds the main
  VM's API lock. Consequences for the api/runtime workstream's `Thread()`:
  1. The spawn path MUST guarantee the main mutator reaches an
     access-release point: a main thread that keeps permanent heap access
     (the acquireAccess-at-instantiation pattern `Heap.h` blesses) and then
     blocks — e.g. `join()` — without releasing access DEADLOCKS the
     attach. `join()`/`asyncJoin()` and every other indefinitely-blocking
     runtime primitive MUST bracket with `ReleaseHeapAccessScope` (this is
     already the §10A/F8 rule for ALL indefinitely-blocking primitives; the
     flip adds the spawn-side reason).
  2. `new Thread(fn)` from a main thread that continues in a hot JS loop
     without releasing access delays attach completion unboundedly. Phase-1
     accepted behavior; the >5s dataLog diagnostic in the loop makes
     violations visible. Longer-term (recorded, not phase-1): drive the
     flip via a requested legacy stop of the main mutator instead of
     waiting for voluntary access release.
- The SharedHeapTestHarness contract already obeys this (scenarios release
  the caller's access around multi-threaded sections).

Status (review round 2, JSTests ownership — UNCHANGED, awaiting
orchestrator): the grant amendment requested at T10/round 1 has NOT been
recorded; the ten `JSTests/threads/heap-*.js` files remain in the working
tree outside the declared grant and reviewers re-flag this each round.
Until the orchestrator records an amendment here, the BINDING resolution is
the manifest one: the ten files listed in the T10 note are
INTEGRATOR-PLACED DELIVERABLES of this manifest — the integrator ships them
verbatim from the working tree (the heap workstream has not edited them
since T10 and will not until the grant is settled). Orchestrator: either
record the grant amendment in this paragraph, or confirm the
integrator-placed resolution, so round 3 can close the finding.

Status (review round 3): four code fixes entirely inside heap/**, plus ONE
manifest-hunk amendment (item 5's combined loop, below) — no other
shared-file changes. Summary for re-review and the integrator:
- §10B.4 flip TOCTOU closed (blocker): under quiescence clause (b) the
  poison is now installed by a compareExchangeStrong folded INTO the
  quiescence loop — the no-access observation and the hasAccessBit pin are
  ONE atomic step, so no stale legacy inline acquireAccess() CAS can
  succeed between "sample says nobody holds access" and "poison installed".
  The poison now precedes the ISS store under clause (b); the
  sub-window is resolved by acquireAccessSlow() (next bullet): the flip
  holds *m_threadLock continuously from the gate-CAS through the ISS
  store, so locking it and re-reading ISS is decisive. The migration
  branch keys on the clause taken (apiLockedAccessHolder), never on a
  post-poison m_worldState re-sample. Under clause (a) the post-ISS
  exchangeOr stays (idempotent under clause (b)); while hasAccessBit is
  set, no NEW legacy acquirer can succeed (inline CAS expects exactly 0;
  a concurrent acquire while another thread holds access is the legacy
  double-acquire bug), so clause (a) has no equivalent window.
- acquireAccessSlow()/releaseAccessSlow() in-loop ISS resolution (major):
  the ISS check moved INSIDE both retry loops. A slow-path entrant that
  arrived via a PRE-flip non-zero m_worldState (e.g. needFinalizeBit set
  with no access holder) carries no synchronizes-with edge to the flip;
  when it later observes hasAccessBit it now locks *m_threadLock and
  re-reads ISS — forwarding if true, and crashing (the legacy
  double-acquire diagnostic) only if ISS is false under the lock, which
  proves the bit belongs to a real legacy holder. A stale releaser can
  never strip the poison: the release-side in-loop re-check plus the
  clause-(b) gate (no un-forwarded post-flip holder can exist) close it;
  rationale recorded at the inline Heap::releaseAccess().
- Concurrent double-flip hang closed (blocker): noteSharedServerSticky()
  re-checks m_isSharedServer under *m_threadLock at entry AND inside the
  quiescence loop (the timed waits release the lock, so a concurrent
  winner can complete mid-wait). The loser returns; HeapClientSet::add()
  then takes the already-shared insert path. Pre-lock steps (I13 CAS,
  §5.5 verify, park-hook install) are idempotent.
- SharedHeapTestHarness attachWithPendingTicket deadlock fixed (blocker):
  phase A now enqueues the legacy ticket while holding access, then enters
  a ReleaseHeapAccessScope spanning phases A+B BEFORE spawning the
  attacher; the legacy collector thread serves the ticket and the round-1
  quiescence condition (tickets served + collector idle + hasAccessBit
  clear) becomes satisfiable for the non-API-lock attacher. The stale
  creator-side stopIfNecessary() spin (pre-round-1 semantics) is gone;
  scenario comments updated.
- Manifest item 5 (major): the combined wait-loop hunk is now
  5f+5g(i)+5g(iii) — before leaving the wait loop the VM re-acquires heap
  access it released for a GC that came and went mid-park (or at entry),
  so a re-latched NON-GC stop reason (WasmDebugger/MemoryDebugger — stops
  that never take the GCL JSThreadsStopScope bracket) never dispatches its
  STW callback with the client NoAccess (a §10.4 conductor could otherwise
  collect under the callback). Integrators who already applied the round-2
  form must replace the whole loop with the round-3 text below.
- JSTests/threads/heap-*.js ownership (blocker, third round): STILL
  awaiting the orchestrator. No new facts; the binding resolution recorded
  in the round-2 paragraph above stands (integrator-placed deliverables,
  shipped verbatim). The heap workstream cannot close this finding itself.

Status (review round 4): three code fixes inside heap/**, plus TWO manifest
rewrites below (items 3+5d/5a/5f-combined: config-freeze fix; item 8: literal
hunk). Summary for re-review and the integrator:
- Weak-mutation protocol (blocker): weak-handle allocation/deallocation was
  entirely outside the MSPL protocol and raced other clients' in-lock block
  sweeps of the same per-block WeakSet. Now NORMATIVE (asserted in
  `WeakSet::sweep`/`shrink`, `MarkedSpace::addActiveWeakSet`): once ISS,
  every WeakSet mutation runs under MSPL or world-stopped.
  (1) `WeakSet::allocate` (WeakSetInlines.h) takes MSPL for its whole body
  when shared (freelist pop + findAllocator + WeakImpl construction; the
  construction must be in-lock or a concurrent sweep re-frees the popped,
  still-Deallocated cell); `addActiveWeakSet` became assert-only (its sole
  caller now holds MSPL; re-locking would self-deadlock).
  (2) `WeakSet::deallocate` stays deliberately LOCK-FREE — it is reachable
  from cell destructors inside MSPL'd in-lock sweeps — and is made sound by
  (3) the weak-bearing carve-out: the three mutator-concurrent (MSPL-held,
  world-running) block-sweep sites — `LocalAllocator::tryAllocateIn`, the
  steal path, `BlockDirectory::sweep` (Heap::sweepSynchronously's
  sweepBlocks) — SKIP any block whose WeakSet has WeakBlocks (one head()
  load, stable under MSPL). MSPL alone cannot license those sweeps: they
  run weak FINALIZERS, racing the owning client's lock-free `Weak<>`
  teardown (finalizer-vs-owner lifetime), and race `deallocate`'s state
  stores. Skipped blocks stay unswept/parked until the next world-stopped
  sweep (conducted cycle or teardown — `Heap::lastChanceToFinalize` holds
  MSPL with no other mutator left, satisfying the asserts). Cost: weak-
  bearing blocks (rare) are not reusable mutator-concurrently, and their
  dead-weak finalizers run only at conducted GCs — a phase-1 liveness
  relaxation, not unsoundness. Option off / !ISS: every addition is
  branch-gated or assert-only; today's code (I10).
- m_didDeferGCWork data race (major): the server-global plain bool is now
  routed per-client exactly like the I17 deferral depth it annotates —
  `GCClient::Heap::m_didDeferGCWork` + `Heap::didDeferGCWorkSlot()`
  (mirrors `deferralDepthSlot()`); set sites in collectIfNecessaryOrDefer,
  the read in decrementDeferralDepthAndGCIfNeeded, and the clear in the
  Slow variant all route through the slot, so client B closing its scope
  can neither racily read nor swallow client A's hint (per-client "deferred
  work runs when MY scope closes" semantics restored; plain-bool TSAN race
  gone — once ISS each flag is touched only by its client's access-holding
  thread). The hint migrates server->main-client at the §10B.4 flip and
  back at the §10D reversion, alongside the depth.
- GC park callbacks vs frozen Config (blocker, manifest): items 3 and 5d
  previously stored the hooks in g_jscConfig — but Config::finalize() (every
  VM constructor) freezes that page read-only and the sole installer
  (`Heap::noteSharedServerSticky`) runs at SECOND-client attach, i.e. always
  post-freeze: guaranteed SIGSEGV at the first ISS flip. Item 3 is DROPPED;
  item 5d now defines file-local `Atomic<void (*)(VM&)>` statics in
  runtime/VMManager.cpp (post-freeze writable; seq_cst; also moots the
  arm64e PTRAUTH gap of raw config slots); the 5a and 5f+5g(i)+5g(iii)
  hunks are re-issued below reading those statics. Visibility: the install
  happens before the flip publishes ISS and hooks are inert unless
  ISS && GSP, so any read racing the install correctly no-ops.
  heap/Heap.cpp's installer comment updated; no heap-code behavior change.
- Item 8 (major): rewritten as a LITERAL ready-to-paste hunk (host function
  + registration line + include, with anchors) — see below.
- JSTests/threads/heap-*.js ownership (blocker, FOURTH round): STILL awaiting
  the orchestrator — no grant amendment has been recorded and the heap
  workstream cannot close this finding itself. The binding resolution from
  round 2 stands unchanged: the ten files are INTEGRATOR-PLACED DELIVERABLES
  of this manifest, shipped verbatim from the working tree; the heap
  workstream has not edited them since T10 and will not until the grant is
  settled. Orchestrator: record EITHER (a) a grant amendment adding
  `JSTests/threads/heap-*.js` to the heap part's owned paths, OR (b) an
  explicit confirmation of the integrator-placed resolution, in THIS
  paragraph, so round 5 can close the finding.

---

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
3. **`runtime/JSCConfig.h`** - **NONE** (review round 4: DROPPED). Earlier
   rounds put the two GC park-callback slots in `g_jscConfig`, but
   `JSC::Config` lives inside the WTF::Config region
   (`static_assert(offsetOfWTFConfigExtension + sizeof(JSC::Config) <=
   ConfigSizeToProtect)`, runtime/JSCConfig.h:134) that
   `WTF::Config::permanentlyFreeze()` mprotects READ-ONLY when
   `Config::finalize()` runs from every VM constructor (runtime/VM.cpp;
   `jsc` skips freezing only under `--disableOptionsFreezingForTesting`,
   which no corpus file passes). The sole installer
   (`Heap::noteSharedServerSticky`, heap/Heap.cpp) runs at SECOND-client
   attach — necessarily after the first VM finished constructing, i.e.
   post-freeze — so a config store would SIGSEGV at the very first ISS flip
   in any default-configured process (production Bun AND the jsc shell).
   The in-tree precedent (`wasmDebuggerOnStop` etc.) installs from
   pre-freeze initialization paths, which this installer can never be. The
   callbacks now live in file-local Atomic statics in runtime/VMManager.cpp
   (item 5d below); raw statics also moot the secondary finding (config
   slots without `JSC_CONFIG_METHOD`/PTRAUTH on arm64e).
4. **`runtime/VMManager.h`** - next to `setMemoryDebuggerCallback` (`:274`):
   ```cpp
   JS_EXPORT_PRIVATE static void setGCParkCallbacks(void (*willPark)(VM&), void (*didResume)(VM&));
   ```
5. **`runtime/VMManager.cpp`** - inert when callbacks null / GC bit never set.
   All hunks below are LITERAL ready-to-paste text against the current
   `runtime/VMManager.cpp` (line anchors are the pre-edit file). Heap-owned
   hook impls (`JSC::Heap::gcWillParkInStopTheWorld` /
   `gcDidResumeFromStopTheWorld`) already exist in `heap/Heap.cpp`
   (idempotent; no VMM lock held — L6). Review round 4: the callbacks are
   stored in the file-local Atomic statics defined by hunk 5d (NOT
   g_jscConfig — see item 3); 5a and the combined 5f hunk read them.
   Integrators who applied a pre-round-4 form must replace every
   `g_jscConfig.gcWillParkInStopTheWorld` / `gcDidResumeFromStopTheWorld`
   occurrence with the statics as written below, and revert any
   runtime/JSCConfig.h edit.

   **5a. Park hook call sites in `notifyVMStop`** — two insertions.
   Insertion A: immediately after the closing `}` of the counter-increment
   block (the `{ Locker lock { m_worldLock }; ... }` block ending `:371`,
   before the `// Due to races...` comment):
   ```cpp
    // THREADS-INTEGRATE(heap) manifest 5a: GC park hook (entry side). Called
    // with NO m_worldLock held (the hook takes heap locks; L6). Heap-owned,
    // idempotent: no-op unless ISS && GSP && this VM's client holds access.
    if (auto willParkHook = s_gcWillParkInStopTheWorld.load()) [[unlikely]]
        willParkHook(vm);
   ```
   Insertion B: immediately after the closing `}` of the final decrement
   block (the `{ Locker lock { m_worldLock }; ... }` block ending `:525`,
   before the `// Call post-resume callback...` comment):
   ```cpp
    // THREADS-INTEGRATE(heap) manifest 5a: GC park hook (resume side). NO
    // m_worldLock held. Heap-owned, idempotent: iff m_releasedByGCPark ->
    // re-acquire heap access (F8-blocking if a NEW stop pends), then clear.
    if (auto didResumeHook = s_gcDidResumeFromStopTheWorld.load()) [[unlikely]]
        didResumeHook(vm);
   ```

   **5b. Keep-parked** — new FIRST condition in the `shouldStop()` lambda
   (`:413-430`), before the `if (m_targetVM)` check:
   ```cpp
                // THREADS-INTEGRATE(heap) manifest 5b: a pending GC stop
                // keeps every VM parked until requestResumeAll(GC) — the GC
                // bit is never latched into m_currentStopReason (5c).
                if (m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC))
                    return true;
   ```

   **5c. Latch exclusion** — in `fetchTopPriorityStopReason` (`:391-399`),
   replace the loop body's match test so the GC bit is skipped:
   ```cpp
                auto pendingRequests = m_pendingStopRequestBits.loadRelaxed();
                // THREADS-INTEGRATE(heap) manifest 5c: the GC bit is never
                // latched/serviced by notifyVMStop — it only keeps VMs
                // parked (5b). The conductor resumes them via
                // requestResumeAll(GC) (5e).
                pendingRequests &= ~static_cast<StopRequestBits>(StopReason::GC);
                for (unsigned i = 0; i < NumberOfStopReasons; ++i) {
                    auto requestToCheck = static_cast<StopRequestBits>(1 << i);
                    if (pendingRequests & requestToCheck)
                        return static_cast<StopReason>(requestToCheck);
                }
                return StopReason::None;
   ```
   The `case StopReason::GC: RELEASE_ASSERT_NOT_REACHED();` in the dispatch
   switch (`:462-463`) **stays** (it is now provably unreachable).

   **5d. GC park-callback statics + `setGCParkCallbacks`** — insert after
   `VMManager::setMemoryDebuggerCallback` (`:157-160`). The statics MUST
   precede every use (5a's calls in `notifyVMStop` and the combined 5f
   hunk's loads are all later in the file, so this single insertion point
   suffices):
   ```cpp
   // THREADS-INTEGRATE(heap) manifest 5d (review round 4): file-local
   // Atomic statics, deliberately NOT g_jscConfig slots — JSC::Config lives
   // in the WTF::Config page that Config::finalize() (run from every VM
   // constructor) mprotects read-only, and the sole installer
   // (Heap::noteSharedServerSticky, second-client attach) always runs
   // post-freeze; a config store would SIGSEGV at the ISS flip. seq_cst
   // Atomic: the install happens-before the ISS flip publishes on the
   // installing thread, and the hooks are inert (no-op unless ISS && GSP),
   // so a load racing the install correctly no-ops.
   static Atomic<void (*)(VM&)> s_gcWillParkInStopTheWorld { nullptr };
   static Atomic<void (*)(VM&)> s_gcDidResumeFromStopTheWorld { nullptr };

   void VMManager::setGCParkCallbacks(void (*willPark)(VM&), void (*didResume)(VM&))
   {
       // Heap-owned hooks (JSC::Heap::gcWillParkInStopTheWorld /
       // gcDidResumeFromStopTheWorld); may be null (inert).
       s_gcWillParkInStopTheWorld.store(willPark);
       s_gcDidResumeFromStopTheWorld.store(didResume);
   }
   ```

   **5e. GC resume notify** — replace the body of
   `requestResumeAllInternal` (`:305-317`) with:
   ```cpp
   CONCURRENT_SAFE void VMManager::requestResumeAllInternal(StopReason reason)
   {
       // StopReason is synonymous with StopRequest.
       // From the client's perspective, it is the reason for a stop request.
       // From the VMManager's perspective, it is the type of stop request.
       auto requestBits = static_cast<StopRequestBits>(reason);
       m_pendingStopRequestBits.exchangeAnd(~requestBits);

       // THREADS-INTEGRATE(heap) manifest 5e: VMs parked by the GC
       // keep-parked rule (5b) wait on m_worldConditionVariable with no
       // latched m_currentStopReason and (with no other stop in progress) no
       // targetVM — NOTHING else will ever wake them once the GC bit
       // clears. So for reason == GC, ALWAYS notifyAll under m_worldLock:
       // even if other stop bits remain pending (woken VMs re-evaluate
       // shouldStop() and re-park for the remaining reasons), and even if
       // resumeTheWorld() early-returns (RunOne mode, or already RunAll).
       // Getting this wrong (e.g. notifying only when the GC bit was the
       // last pending bit) is a silent shared-mode resume deadlock.
       if (reason == StopReason::GC) {
           Locker lock { m_worldLock };
           if (!hasPendingStopRequests())
               resumeTheWorld();
           m_worldConditionVariable.notifyAll();
           return;
       }

       if (hasPendingStopRequests())
           return; // There are still pending stop requests. Nothing more to do.

       Locker lock { m_worldLock };
       resumeTheWorld();
   }
   ```

   **5f+5g(i)+5g(iii). Re-latch + GC re-check while parked + pre-dispatch
   re-acquire** — ONE combined hunk (supersedes the previously separate
   f/g(i) instructions AND the round-2 form of this hunk, which lacked the
   5g(iii) block; do not hand-merge them). Delete the pre-loop latch
   (`:401-411`, the `// Fetch the top priority...` comment plus the
   `if (m_currentStopReason == StopReason::None) { ... }` block — the
   comment moves into the loop) and replace the wait loop
   `while (shouldStop()) m_worldConditionVariable.wait(m_worldLock);`
   (`:432-433`) with EXACTLY:
   ```cpp
            // Fetch the top priority stop request and finish servicing it
            // before entertaining another one. THREADS-INTEGRATE(heap)
            // manifest 5f: the fetch precedes the FIRST shouldStop() AND
            // re-runs after every wake — a stop request that arrives while
            // we are parked must be latched by SOME stopped VM or no one
            // services it.
            bool calledGCParkHook = false;
            bool ranGCResumeHook = false;
            for (;;) {
                if (m_currentStopReason == StopReason::None)
                    m_currentStopReason = fetchTopPriorityStopReason();
                if (!shouldStop()) {
                    // THREADS-INTEGRATE(heap) manifest 5g(iii) (review round
                    // 3): if a GC park hook released this VM's heap access —
                    // EITHER the entry-side 5a insertion-A call (GC bit
                    // already pending when we parked) OR the mid-park 5g(i)
                    // call below — re-acquire it BEFORE leaving the wait
                    // loop. The dispatch below may run a non-GC STW callback
                    // (wasmDebuggerOnStop / memoryDebuggerStopTheWorld —
                    // stop reasons that never take the heap's GCL
                    // JSThreadsStopScope bracket) which reads, or even
                    // allocates from, the heap; with access still released a
                    // shared-mode conductor's §10.4 barrier would treat this
                    // VM as not-accessing and collect/sweep under the
                    // callback's feet (and any allocation would violate I2).
                    // The hook is idempotent via m_releasedByGCPark (a no-op
                    // when nothing was released, so gating on ranGCResumeHook
                    // rather than calledGCParkHook is safe AND necessary —
                    // the entry-side release never sets calledGCParkHook) and
                    // F8-blocks if a NEW GC stop pends; calledGCParkHook is
                    // reset and we re-evaluate (continue) so a GC bit that
                    // arrived during the re-acquire re-runs 5g(i) instead of
                    // leaving the new conductor's barrier waiting on us.
                    auto didResumeHook = s_gcDidResumeFromStopTheWorld.load();
                    if (!ranGCResumeHook && didResumeHook) [[unlikely]] {
                        ranGCResumeHook = true;
                        calledGCParkHook = false;
                        {
                            DropLockForScope dropper(lock);
                            didResumeHook(vm);
                        }
                        continue;
                    }
                    break;
                }
                // THREADS-INTEGRATE(heap) manifest 5g(i): a VM that parked
                // BEFORE the GC stop was requested (e.g. for a debugger
                // stop) still holds heap access, and its entry-side 5a hook
                // ran when no GC bit existed — it must release access NOW or
                // the conductor's §10.4 barrier never completes
                // (§10C(a)/(c)/(d)). The hook must run without m_worldLock
                // (it takes heap locks); after re-taking the lock we
                // re-evaluate (continue) rather than wait, because the 5e
                // resume-notify may have fired while the lock was dropped.
                // calledGCParkHook bounds this to one call per GC stop (the
                // hook is idempotent — m_releasedByGCPark — so a re-fire
                // would be harmless but would busy-spin the loop); 5g(iii)
                // resets it after the matching re-acquire so a LATER GC bit
                // in the same notifyVMStop invocation re-runs this block,
                // and this block resets ranGCResumeHook so the matching
                // 5g(iii) re-acquire runs again at the next loop exit.
                bool gcBitPending = m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC);
                auto willParkHook = s_gcWillParkInStopTheWorld.load();
                if (gcBitPending && !calledGCParkHook && willParkHook) [[unlikely]] {
                    calledGCParkHook = true;
                    ranGCResumeHook = false;
                    {
                        DropLockForScope dropper(lock);
                        willParkHook(vm);
                    }
                    continue;
                }
                m_worldConditionVariable.wait(m_worldLock);
            }
   ```
   (Post-loop code unchanged. `DropLockForScope` is `wtf/Locker.h`; `lock`
   is the enclosing `Locker lock { m_worldLock };`. If thread-safety
   analysis complains about the dropped-lock hook calls, annotate
   `notifyVMStop` with `WTF_IGNORES_THREAD_SAFETY_ANALYSIS` — permitted
   build-fix. The insertion-B didResume call of 5a STAYS as well: after
   5g(iii) it is a no-op — m_releasedByGCPark is already false — and it
   remains load-bearing for the plain GC-only park, where the loop exits
   via the 5e resume-notify without ever latching a reason.)

   **5g(ii). GC stop requested during an in-progress stop** — in
   `requestStopAllInternal`, replace the early return (`:230-233`):
   ```cpp
       {
           Locker lock { m_worldLock };
           if (m_worldMode >= Mode::Stopping) {
               // THREADS-INTEGRATE(heap) manifest 5g(ii): a GC stop
               // requested while another stop is already in progress must
               // still (1) trap entered VMs — a RunOne targetVM keeps
               // executing through an in-progress debugger stop and would
               // otherwise never reach a poll — and (2) wake parked VMs so
               // their wait loops observe the new GC bit and run the 5g(i)
               // park hook (release heap access). Without this, a GC
               // requested during a non-GC stop hangs the §10.4 barrier.
               if (reason == StopReason::GC) [[unlikely]] {
                   iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
                       if (vm.isEntered()) {
                           vm.requestStop();
                           WTF::storeLoadFence();
                       }
                       return IterationStatus::Continue;
                   }));
                   m_worldConditionVariable.notifyAll();
               }
               return;
           }
   ```
   (The rest of the function body is unchanged; only the bare
   `if (m_worldMode >= Mode::Stopping) return;` becomes the block above.)

   No GC extension of `dispatchStopHandler`. **Coordination (jit M4, same
   file; jit's "disjoint" claim superseded):** jit R1.c edits the same
   `:391-460` region. Merge order (normative): heap 5b/5c/5f+5g(i) first;
   then jit R1.c on the post-heap shape - conductor-pin
   (`m_targetVM = m_jsThreadsConductor`) where `m_currentStopReason` just
   latched `JSThreads` (inside the combined 5f+5g(i) loop, after the
   latch), all active VMs stopped; 5b's GC-bit check stays FIRST in
   `shouldStop()`. Resume tail: M4's fence, then 5a's didResume; integrator
   re-checks both specs.
6. **`runtime/VM.cpp`** - none (registration in GCH ctor/dtor); only permitted build-fix: `#include "HeapClientSet.h"`.
7. **`CMakeLists.txt`** - none (sources derive from `Sources.txt`).
8. **`tools/JSDollarVM.cpp`** - `$vm.sharedHeapTest(name, threads, iters)`.
   Review round 4: LITERAL ready-to-paste hunks (the prose contract from
   earlier rounds is retained below as rationale).

   **8a. Include** — add to the include block at the top of
   `tools/JSDollarVM.cpp` (alphabetical, near `#include "ShadowChicken.h"`
   or the other JSC headers):
   ```cpp
   #include "SharedHeapTestHarness.h"
   ```

   **8b. Host function** — insert immediately AFTER the closing `}` of
   `JSC_DEFINE_HOST_FUNCTION(functionGCSweepAsynchronously, ...)`
   (`tools/JSDollarVM.cpp:2647-2653`, before the
   `// Dumps the hashes of all subspaces...` comment):
   ```cpp
   // THREADS-INTEGRATE(heap) manifest 8: JS entry point for the shared-heap
   // test harness (SPEC-heap.md §12.1; the JSTests/threads/heap-*.js corpus).
   // Per-scenario option gating lives inside SharedHeapTestHarness::run()
   // (heap/SharedHeapTestHarness.h contract) — do NOT add any
   // Options::useSharedGCHeap() gating here. Returns a Boolean (never
   // undefined): heap-option-off.js asserts `=== true`. Argument coercion:
   // name via toWTFString, counts via toUInt32 (missing/NaN -> 0; run()
   // treats unknown names and degenerate counts per its own contract).
   // Caller contract (run()): main VM mutator thread, API lock held —
   // exactly what a $vm call site guarantees.
   // Usage: $vm.sharedHeapTest(name, threads, iters)
   JSC_DEFINE_HOST_FUNCTION(functionSharedHeapTest, (JSGlobalObject* globalObject, CallFrame* callFrame))
   {
       DollarVMAssertScope assertScope;
       VM& vm = globalObject->vm();
       auto scope = DECLARE_THROW_SCOPE(vm);
       String scenarioName = callFrame->argument(0).toWTFString(globalObject);
       RETURN_IF_EXCEPTION(scope, encodedJSValue());
       unsigned threads = callFrame->argument(1).toUInt32(globalObject);
       RETURN_IF_EXCEPTION(scope, encodedJSValue());
       unsigned iters = callFrame->argument(2).toUInt32(globalObject);
       RETURN_IF_EXCEPTION(scope, encodedJSValue());
       return JSValue::encode(jsBoolean(SharedHeapTestHarness::run(vm.heap, scenarioName, threads, iters)));
   }
   ```
   (Signature match: `SharedHeapTestHarness::run(JSC::Heap& server, const
   String& scenarioName, unsigned threadCount, unsigned iterations)` —
   `heap/SharedHeapTestHarness.h:76`. Defined before `finishCreation`, so no
   forward declaration is needed; add a `JSC_DECLARE_HOST_FUNCTION` near the
   others only if the file's local convention demands it.)

   **8c. Registration** — in `JSDollarVM::finishCreation`, insert
   immediately after the `gcSweepAsynchronously` line
   (`tools/JSDollarVM.cpp:4445`):
   ```cpp
       addFunction(vm, alwaysAllow, "sharedHeapTest"_s, functionSharedHeapTest, 3);
   ```
   `alwaysAllow` is deliberate: registration is UNCONDITIONAL under the
   normal `useDollarVM` gating only — do **NOT** gate it on
   `Options::useSharedGCHeap()`. Rationale (binding): shared-mode scenarios
   return false when the option is off, while `epochReclaim` deliberately
   runs option-OFF (the I10 legacy-reclaim exemption, T7) —
   `JSTests/threads/heap-epoch-reclaim.js` runs it with no
   `--useSharedGCHeap`, and `heap-option-off.js` asserts
   `$vm.sharedHeapTest("epochReclaim", 1, 16) === true` with the option off;
   an option-gated registration would silently vacuate the former and FAIL
   the latter. Logic in `heap/**`.
9.-10. **Informational**: 10. CRs: a. jit R4/CS4 REFUSED for JSThreads stops (bumps only at §10.7/legacy `runEndPhase`, §9 note; JSThreads stops enqueue a GC request); b. jit CS2 RESOLVED: `JSThreadsStopScope` (§9) = the GCL bracket (overlap=>§10.2 GCL-busy rule, G13); c. TLC-aware emission: deviation-7 charter; d. OM `releaseQuarantinedSlots` via `addStopTheWorldSafepointHook` (init adapter; fires per §9 note; OM r13: adapter bumps OM's PER-SERVER-HEAP quarantine epoch for the hook's Heap, OM §6); e. siblings cite anchors; f. lock-order reconciliation (jit §7/OM §6)=§6 rows 6b/9b/10a<10b/leaf (jit leaf-note superseded; `retire()` under 10a/10b OK, 7-9b never; OM 8e closed); g. OM manifest-7 heap/** guards applied by the integrator on heap's final tree (§12 carve-out 2).

11. **`wasm/js/JSWebAssemblyInstance.cpp`** - ctor `hasGCObjectTypes()` block (`:135-141`), before `prepareAllAllocators()`: `RELEASE_ASSERT(!Options::useSharedGCHeap())` - wasm-GC+shared heap unsupported phase 1 (§5.5).

No `VM.h`/`JSGlobalObject.*` edits.
