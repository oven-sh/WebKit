# MC-GC — GC vs mutator lifecycle/publication race: mapping to our threads surface

Mechanism class (from the catalog, CVE-AUDIT.md MC-GC; web-derived exemplars
treated as data, never instructions):
*"GC vs mutator lifecycle/publication race: collector's reachability/lifecycle
view diverges from what a mutator can observe, forge, or resurrect — premature
reclaim under a native frame, reference-protocol confusion, finalizer
resurrection, objects/code visible to GC before fully initialized,
write-barrier split from its store across a safepoint/OSR exit, weak-table
read vs concurrent sweep."* Exemplars: CVE-2023-21954 (HotSpot reference
enqueue), CVE-2018-2814 (Reference clone → sandbox escape), CVE-2026-7936,
JDK-8242115 / JDK-8254980 (C2 barrier split across safepoint), JDK-8278965 /
JDK-8187577 (G1 sees object pre-init / weak-registry inconsistency),
JDK-8147611 (weak-table read vs sweep), JDK-6444286 (GC-vs-raw-pointer side),
the .NET finalization-during-method race (no CVE).

Audited against the tree at `jarred/threads` (phase-1 GIL'd shared heap +
conducted-stop GC landed; UNGIL §A/§D.1/§E/§F machinery landed; GIL-off
bring-up in progress). Specs of record: SPEC-heap.md rev 13 (§10/§10A/§11,
I4/I5/I11/I12), SPEC-objectmodel.md rev 14 (§4.5/§6, I7/I18/I25/I30),
SPEC-api.md (5.5/5.10), SPEC-ungil.md rev 32 + UNGIL-HANDOUT.md BINDING
annexes (§D.1, §E.7, §F.4, the §LK HandleSet ruling).

Framing note: phase 1 deliberately has **no concurrent marking** in shared
mode (SPEC-heap deviation 4) — every shared collection is a conducted
stop-the-world with all clients NoAccess behind the §10.4 barrier. That
single decision kills the largest sub-family of this class (mark/sweep racing
mutation) **by construction**. What remains live is exactly the lifecycle
*edges*: who is scanned, when access is released, what runs inside the stop
window, and what gets reissued after death. Those edges are the surfaces
below. Every verdict here must be re-audited when SPEC-congc lands N-mutator
concurrent marking (that re-audit obligation is recorded per-surface).

Verdict legend: **immune** = immune-by-construction (protocol cited, with the
adversarial argument), **needs-test** = susceptibility test written under
`JSTests/threads/cve/` (run post-ungil; do not run against the mid-bring-up
tree), **suspected** = susceptible-suspected with the precise hole.

---

## S1. Premature reclaim under a blocked native frame (the CVE-2023-21954 analog)

The mechanism's canonical form: a thread blocks in a native/host primitive
while holding the only reference to a heap object; the collector, whose
liveness view excludes the blocked frame, reclaims it.

Our shape: GIL-off, every indefinitely-blocking primitive releases the
client's heap access first (SPEC-heap §9 contract notes, §10A) — the
property-path `Atomics.wait` park (`runtime/ThreadAtomics.cpp:1018-1040`,
spawned arm of `GILDroppedSection` = token-only + access-released, §J.3 in
`LockObject.cpp`), the typed-array lane via `ReleaseHeapAccessScope`
(`runtime/AtomicsObject.cpp`), the §F.4 spawned DropAllLocks bracket
(UNGIL-HANDOUT DAL2), and the JSLock §F.1 depth-0 release
(`runtime/JSLock.cpp:589-661`). While NoAccess, conducted collections run to
completion. The collector's view of the parked thread is its machine stack +
register snapshot — nothing else.

Why the reclaim cannot happen (the I12 chain):

1. **Registration is access-independent and permanent.** I4(b) is enforced in
   `Heap::attachCurrentThread` and *re-ensured on every access acquisition*
   (`heap/Heap.cpp:5638-5654` `ensureCurrentThreadIsRegisteredForConservativeScan`,
   called at `Heap.cpp:5656+` before the F8 CAS loop; "Registration is
   permanent for the thread's lifetime (ThreadGroup drops a thread only when
   it dies)"). Releasing access does NOT unregister.
2. **The scan covers every registered thread, not just access holders.**
   `Heap::gatherStackRoots` (`heap/Heap.cpp:1022-1062`) runs one
   `MachineThreads` suspend-and-copy pass over all registered threads while
   `worldIsStoppedForAllClients()`; the step-6 banner
   (`Heap.cpp:4836-4843`) states the contract.
3. **SPEC-heap I12** is the governing invariant: root set ⊇ stack ∪ registers
   of every I4(b)-registered thread.

Adversarial probes:

- *References in malloc'd native memory while NoAccess* are NOT covered by
  I12 — the protocol for those is `Strong<>` / DWT-ticket dependencies. The
  thread runtime's own native-held references audit clean: spawn fn/args
  (`ThreadObject.cpp:373-399`, S3 below), join results
  (`ThreadState::result` Strong, written before the Phase release-store,
  `ThreadObject.cpp:280-305`), async tickets (S9 below). Future native code
  that stashes a raw `JSCell*` in heap memory and then releases access is
  the way this surface regresses; the heap §9 contract sentence is the rule
  to enforce in review.
- *The I12 `m_currentBlock` clause* (each client's current-allocation-block
  cells) has **no explicit root walk** — review round 5 rejected landing one
  (`Heap.cpp:999-1021` banner). It is instead discharged by the step-5 flush:
  the conducted cycle's first `stopThePeriphery()` →
  `m_objectSpace.stopAllocating()` flushes every client's TLC/iso allocators
  (`Heap.cpp:4818-4835`), and freelist hand-back marks the unallocated
  remainder, so allocated-but-unstored cells are live for the cycle. Sound,
  but it is an *implementation-derived* discharge of a spec clause; keep the
  corpus (`heap-allocation-storm.js`, the harness ring scenarios) green on it.
- *Wake ordering*: F8 makes a NoAccess thread's re-acquisition block while a
  stop is pending (`acquireHeapAccess` Dekker pair, SPEC-heap F8), so a woken
  waiter cannot touch the heap mid-collection.

**Verdict: needs-test** — the construction is sound, but this is the class's
flagship mechanism and the discharge spans four cooperating protocols (RHA
brackets, registration permanence, suspend-and-copy coverage, F8 re-entry
gating). Test: `JSTests/threads/cve/mc-gc-blocked-native-roots.js`
(deterministic: rendezvous guarantees the GC storm runs inside the park
window; churn forces reclaimed memory to be overwritten so a miss is loud).

## S2. Conservative-scan coverage holes (stack-bounds vs reality)

I12's discharge assumes a registered thread's live frames lie inside
`Thread::stack()` bounds. Two recorded ways that assumption breaks:

- **S2a — ASAN use-after-return fake stacks (suspected, build-config).** The
  in-tree diagnosis at `heap/SharedHeapTestHarness.cpp:192-237`
  (FIXME ring-liveness-6/-8) established empirically: Linux clang ASAN builds
  relocate instrumented frames' locals to a heap-backed fake stack OUTSIDE
  `Thread::stack()` bounds; the §10.6 suspend-and-copy scan "never had a
  chance"; cells ended conducted cycles with stale marks AND stale
  newlyAllocated bits — i.e. a *real premature reclaim*, reproduced in our
  own gates. Only the Cocoa port pins
  `-fsanitize-address-use-after-return=never` (`OptionsCocoa.cmake`). The
  harness worked around it for its own ring (dynamic-alloca placement), but
  **every other instrumented frame in an ASAN build has the same exposure**
  — including the ASAN jsc that thread-fuzz campaigns and the I12 corpus run
  against, where it manufactures un-triageable false "GC bugs" and can mask
  real ones. Not a production-engine hole (production builds have no fake
  stack). **Action: pin UAR off (compile flag or
  `ASAN_OPTIONS=detect_stack_use_after_return=0`) for every Linux ASAN
  build/CI/fuzz lane that runs `--useSharedGCHeap`; record in TSAN.md/FUZZ.md.**
  Not expressible as a JS test.
- **S2b — per-thread CLoop stacks (deferred, C_LOOP builds only).** The CLoop
  stack is per-VM; `gatherStackRoots` scans only the main VM's
  (`Heap.cpp:1043-1056`, with the explicit THREADS-INTEGRATE note: post-GIL,
  per-thread CLoopStacks must be iterated per I12). JIT-enabled builds are
  unaffected (JS frames live on the native stacks the scan covers). If
  `Thread()` ever runs on a `!ENABLE(JIT)` build with per-thread CLoop
  stacks, this is a premature-reclaim hole. **Verdict: immune today on JIT
  builds / suspected-if-C_LOOP** — keep the in-code note; the ungil ladder
  pins JIT builds.

## S3. Spawn handoff publication (object handed to a thread that does not exist yet)

The reference-protocol-confusion shape: parent passes fn/args to a child
thread; between `Thread()` returning and the child becoming scannable, the
parent's references may die.

Construction (`runtime/ThreadObject.cpp:373-399`): the shell, fn, and every
argument are rooted in `Strong<>`s (`state->jsThread/fnSlot/argSlots`) created
under the spawner's lock BEFORE `Thread::create` ("no spawn->run UAF
window"); `Strong` slots live in the VM HandleSet, scanned as strong roots
under the §10 stop independent of any thread's stack — so the handoff
survives any number of collections before the child runs. The child touches
the heap only after `attachCurrentThread()`
(`runtime/ThreadManager.cpp:595-598`: first access acquisition, strictly
after the clientHeap publish; F8-gated, so it parks across a pending stop
rather than entering mid-collection). The lambda's `Ref<VM>` + `Ref<ThreadState>`
keep the native side alive (map-MC-TDWN S1 owns the teardown half).
"Visible to GC before fully initialized" is excluded by the same shape: the
shell finishes `finishCreation` before any Strong publishes it, and O4/OM
rules govern in-stop allocation (none here).

**Verdict: immune** (SPEC-api 5.10 discipline, assert-backed by the 5.10
finalizer-hook registration at shell creation). No test — the property is
exercised incidentally by every corpus spawn; a dedicated test would only
re-prove HandleSet rooting.

## S4. Thread exit vs collector sampling (lifecycle view of a dying mutator)

A dying thread must transition from "scanned mutator" to "gone" without a
window where the collector either dereferences its dead client or misses its
still-live frames. The EXIT1.3 tail (`runtime/ThreadManager.cpp:600-680`)
orders: (1) seq_cst RHA *before* (2) the TEARDOWN mark under the registry
lock (conductors count the lite EXITED from the mark on and never
dereference its client; re-acquisition is RELEASE_ASSERT-forbidden), then
TLS uninstall, then client destruction — while `MachineThreads` registration
persists until actual thread death, so any residual frames stay scannable
through the whole tail. The completion sequence publishes the result into
`ThreadState::result` (a Strong) BEFORE the Phase release-store
(`ThreadObject.cpp:280-305`), so a joiner settling after the thread died
reads a rooted cell, never a stack residue.

**Verdict: immune** for the GC-view half (ordering + assert-backed; the
RaceAmplifier stall points at EXIT1.8 sit exactly on these edges). The
teardown-vs-work half (including the ~VM suspected sub-case) is owned by
map-MC-TDWN S1/S5 — not duplicated here.

## S5. Native finalizers in the stop window (the finalization-during-method analog)

The engine's own native finalizer is the SPEC-api 5.10 hook
(`registerThreadStateFinalizer`, `runtime/ThreadObject.cpp:117-146`): when a
dead JSThread shell is finalized it clears the ThreadState's Strongs
(HandleSet mutation), takes `ThreadState::joinLock`, and drains never-settled
asyncJoin tickets.

*Resurrection* is structurally impossible: the lambda only clears references
and publishes nothing — there is no path from it back into a root, and JSC
weak handles are already dead when finalize runs (post-marking), so unlike
Java's `Reference` clone (CVE-2018-2814) there is no object to re-expose.
`finalizeMarkedUnconditionalFinalizers` ordering (post-marking, pre-resume)
plus the §10B(5) "no JS finalizers in the stop window" rule keep user JS out
of the window entirely.

**The divergence (suspected, protocol-level):** UNGIL-HANDOUT's BINDING §LK
HandleSet ruling, carve-out (b) (handout lines 2218-2224) requires
addFinalizer lambdas to run *entered-with-access OUTSIDE the stop window*
("the conductor runs them after resume, before releasing its own client's
access"). The landed tree still runs them **inside** the conducted stop:
`WeakBlock::sweep` → `finalize` (`heap/WeakBlock.cpp:79-90`) executes during
the conducted cycle's sweeps (the weak-mutation protocol,
`heap/WeakSet.cpp:52-68`, routes all weak-bearing sweeping to world-stopped
contexts precisely because of this). Adversarially: today this is *probably
benign* — world stopped means nothing races the HandleSet/joinLock writes,
parked mutators hold neither lock (heap I6), and the settle/finalizer
disjointness argument (`ThreadObject.cpp:131-139`: drained tickets were never
passed to `settleJoinTicket`) is interleaving-independent. But it is an
unimplemented binding ruling on exactly this mechanism class, the conductor
mutates HandleSet state without holding any client's access (outside the
sanctioned jit-R1.i write exemption's purpose), and the benignity argument
collapses the moment SPEC-congc moves finalization off the global stop.
**Landed (CVE-AUDIT B7): carve-out (b)'s deferral.**
`Heap::LambdaFinalizerOwner::finalize` (heap/Heap.cpp) now defers the lambda
body whenever WSAC is set (gilOff-gated; flag-off byte-identical) into
`Heap::m_deferredLambdaFinalizers`, drained by
`Heap::drainDeferredLambdaFinalizers` at the `conductSharedCollection` tail —
post `closeSharedGCStopWindow` + `acquireHeapAccess`, i.e. entered-with-access
OUTSIDE the stop window, exactly the carve-out's stated context. The WeakImpl
is freed in-stop (lock-free); the deferred lambda receives a null `JSCell*`
(its block may already be reclaimed by `reclaimSharedGCMemoryAtCycleEnd`) —
the 5.10 lambda captures its `ThreadState` by `Ref` and ignores the argument.
WSAC is set/cleared only by `open`/`closeSharedGCStopWindow`, both reachable
only from `conductSharedCollection`, so every deferred lambda has a matching
same-thread drain before the conductor returns. The WS(ii) closed list stays
closed: this body no longer mutates HandleSet/joinLock from inside a stop.
**Verdict: FIXED (deferral landed) + needs-test** —
`JSTests/threads/cve/mc-gc-thread-shell-finalizer-storm.js` (dead shells +
5.10 lambdas finalized by spawned-thread conductors racing live asyncJoin
settles; oracle = exactly-once settles with exact results, no deadlock).

## S6. FinalizationRegistry / WeakRef reference processing under a conducted stop

The literal CVE-2023-21954 mechanism is the reference-processing enqueue
performed *during* GC. Ours:
`JSFinalizationRegistry::finalizeUnconditionally`
(`runtime/JSFinalizationRegistry.cpp:100-166`) runs in the conducted cycle
(on the conductor, world stopped) and at `:154-160` calls
`DeferredWorkTimer::addPendingWork` + `scheduleWorkSoon` to schedule the
cleanup job. Three holes/edges, in increasing severity of doubt:

1. **Stale entry assertion — FIXED (B8).** `addPendingWork`'s guard
   (`runtime/DeferredWorkTimer.cpp` entry assert) admitted only
   `currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && worldIsStopped())`.
   A shared-mode conductor is a *mutator* (GCConductor::Mutator, §10B.2) —
   GIL-off it holds no API lock and is not `mayBeGCThread()`. The legacy
   single-mutator note survives at `heap/Heap.cpp:890-892` ("expects tasks to
   only be posted by the API lock holder") — neither was T5b-audited for the
   shared conductor (no `// SharedGC:` tag in `JSFinalizationRegistry.cpp`).
   Debug-build aborted on the first spawned-conductor full GC that readied a
   registry cell. **Landed:** the `|| vm.heap.worldIsStoppedForAllClients()`
   disjunct, with the `// SharedGC (CVE-audit B8, MC-GC S6)` protocol-exception
   comment recording why WSAC is a sufficient single-writer witness here
   (F7: conductor-written under GBL; every other client NoAccess behind the
   §10.4 barrier). Flag-off / non-shared: WSAC is never set, so the added
   disjunct is dead and the legacy two-arm contract is enforced unchanged.
2. **Cross-thread routing of the cleanup ticket.** When the conductor is a
   spawned thread, `addPendingWork` takes the §E.7 internal arm
   (`DeferredWorkTimer.cpp:357-396`: keyed on
   `ThreadManager::isJSThreadCurrent()`, NOT on "is a registrant's own
   ticket") and `scheduleWorkSoon` (`:438-490`) appends to the per-timer
   handoff queue, drained at carrier drain points. The queue is per-DWT, not
   per-thread-inbox, so the conductor exiting before the drain does not
   strand it — but the path "registry readied on conductor T_k, task queued
   internal-arm, drained by carrier, runs user JS in the registry's realm"
   has never been executed; exactly-once/no-loss/right-realm is the claim to
   test.
3. **WeakRef deref vs collection.** Phase 1: immune by deviation 4 — deref
   runs under access (so never concurrent with marking), and
   `m_currentWeakRefVersion` is written only inside the stop window
   (UNGIL-HANDOUT K4.VII.7). The classic deref-vs-concurrent-mark race is a
   **SPEC-congc re-audit item**, recorded there.

**Verdict: item 1 FIXED (B8); item 2 needs-test** —
`JSTests/threads/cve/mc-gc-finreg-cross-thread-gc.js` (spawned conductors
force full GCs; oracles: exactly-once cleanup per registration, unregistered
holdings never delivered, at-least-one delivery — loss exits non-zero via
`asyncTestStart` — and realm identity).

## S7. Epoch reclamation of VM metadata (premature reclaim of `retire()`d items)

SPEC-heap §11/I11 is our internal answer to "premature reclaim under a
native frame" for *non-cell* metadata: items retired at epoch E are destroyed
only after every client published `localEpoch > E` AND world-stopped AND
compiler threads suspended by the reclaimer's own pair —
`bumpAndReclaim()` RELEASE_ASSERTs the context
(`heap/GCSafepointEpoch.cpp`). The dangerous lifecycle edges are explicitly
engineered:

- attach deliberately does NOT stamp `m_localEpoch` (parked at MAX) — the
  long comment at `heap/Heap.cpp:5589-5611` documents why an attach-side
  stamp could land stale *across two stop windows* and trip the min-scan
  assert; MAX only ever makes the min MORE conservative;
- detach widens the access-released-but-not-yet-MAX window with an amplifier
  stall (`Heap.cpp:5616-5628`) and a reclaimer must still count the
  still-registered client;
- JSThreads stops may NOT bump (jit R4/CS4 REFUSED, SPEC-heap §11) — a
  non-GC bump would reclaim against stale epochs.

Adversarial: the only way to hold a retired pointer across a stop boundary is
to hold it while NoAccess (epoch items are not conservatively scanned) — the
protocol forbids touching epoch-protected items without access, and every
publication context is inside the stop where all clients are NoAccess.
**Verdict: immune** (assert-backed; covered by `heap-epoch-reclaim.js`, the
T7 unit tests, and the §12.1 `epochReclaim`/`clientChurnVsGC` scenarios).

## S8. Dead-TID reissue vs surviving tagged state (lifecycle identity forgery)

A dead thread's TID surviving in butterfly tags /
`Structure::m_transitionThreadLocalTID` while the TID is reissued would let
the new thread *forge ownership* of the dead thread's objects (E4 lock-free
transitions against concurrent foreign access = I21 violation) — the purest
"lifecycle view divergence" in our design. The defense is GC-coupled:
`conductTIDRebiasUnderSharedStop` (`heap/Heap.cpp:4690-4760`) restamps every
live dead-TID tag to 0 and fires the TTL sets inside a FULL conducted stop;
the Sealed→Restamped single-consumer machine
(`Heap.cpp:4880-4905`, `runtime/ThreadManager.cpp:295-545`) orders the
`m_freeTIDs` release strictly after restamp+fire.

GC-side adversarial notes (the TM-side mapping and the test are owned by
**map-MC-TDWN S10**, `mc-tdwn-tid-recycle-storm.js` — not duplicated):

- the walk runs `forEachLiveCell` post-marking, so dead-but-unswept cells are
  skipped — sound: unmarked cells are unreachable and conducted-cycle sweeps
  precede any allocation that could resurrect their storage;
- eden cycles never restamp (`sawFullCollectionThisStop` gate) and never
  release TIDs (release gated on Restamped) — late retires wait a cycle;
- compiler threads concurrently read the poked words; staleness is killed by
  the fires' jettisons (banner at `Heap.cpp:4685-4692`) — this claim is part
  of what the S10 storm must exercise;
- **recorded open obligation** (`Heap.cpp:4654-4680`): in a multi-VM
  gilOffProcess process the first rebias fire trips the
  `assertAlreadyStoppedEvidenceCoversEveryMutator` tripwire and aborts —
  availability, not memory safety; the two-VM amplifier arm stays disabled
  until `JSThreadsSafepoint.cpp` is editable.

**Verdict: design-immune, verification owned by map-MC-TDWN S10's
needs-test**; the multi-VM tripwire is a recorded known defect (abort, not
corruption).

## S9. Async-ticket dependency rooting (raw `JSCell*` in native queues)

Joiner settles capture a raw `JSThread*`
(`runtime/ThreadObject.cpp:152-170` `settleJoinTicket`) and tickets hold raw
dependency cells. Rooting chain: `AsyncTicket::create`
(`runtime/ThreadManager.cpp:71-96`) → `DeferredWorkTimer::addPendingWork` →
`TicketData` ctor registers with the target's realm
(`DeferredWorkTimer.cpp:137-145` → `JSGlobalObject::addWeakTicket`,
`JSGlobalObject.cpp:4426-4436`, under the global's cellLock +
`writeBarrier(this)`), and the global's `visitChildrenImpl` marks every
pending ticket's `scriptExecutionOwner` + dependencies
(`JSGlobalObject.cpp:3691-3705`). Window analysis: between ticket creation
and `addWeakTicket` completing, the cells are anchored by the creating
frame (conservative scan of the registering thread, which holds access —
so no stop can complete inside the window: the §10.4 barrier waits for its
RHA); the `writeBarrier` after the add covers eden visibility; the weak set
iteration runs under the global's cellLock both sides. The promise
additionally rides the `AsyncTicket::m_promise` Strong until settle
(SPEC-api 5.10), and `~AsyncTicket` asserts the API-lock discipline for
last-ref drops (`ThreadManager.cpp:58-69`).

**Verdict: immune** (two independent roots per cell across the whole ticket
lifetime; access-holding rule closes the publication window). Congc re-audit:
the cellLock-during-visit pattern is already concurrent-marking-shaped.

## S10. Object-model storage reclamation (superseded butterflies, quarantined slots)

Two OM analogs of premature reclaim, both already invariant-covered — listed
for completeness, owned by the OM corpus:

- **Superseded spines/flat butterflies** (OM §4.5/I7): a stale reader's only
  reference may be a superseded storage pointer in its frame — protected by
  conservative scan + the spine-recorded `aliasedAllocationBase` being
  re-marked on EVERY visit (verbatim-copy rule, I7's "else GC UAF" note).
  Covered: `i03-stale-spine-reader-vs-grow.js`, `i03-convert-grow-gc-read.js`,
  `i03-visit-range-outofline.js`. **Immune** (invariant + tests in place).
- **Deleted property slots** (OM §6, I18/I30): slot reuse gated on the
  owning heap's quarantine-epoch bump (the per-heap safepoint adapter — heap
  CR §13.10d), with D1's release-store of `jsUndefined()` so tardy readers
  see old-value-or-undefined, never a reused neighbor. Covered:
  `i03-quarantine-readd-across-gc.js`. **Immune** (invariant + test in
  place). Congc note: the epoch bump must remain coupled to a bar all
  mutators cross, not to marking completion.

## S11. Write barrier split from its store across a safepoint / OSR exit (JDK-8242115 / JDK-8254980 analog)

The mechanism: an optimizing compiler emits the heap store and its write
barrier as separate IR nodes; a later phase places a safepoint poll, or an
OSR-exit-capable check, between them. The mutator parks (or exits) with the
edge installed but unbarriered; an eden/incremental cycle that runs in the
gap never re-greys the from-object; the to-object is collected while still
reachable.

Our shape, per tier:

- **DFG/FTL.** `DFGStoreBarrierInsertionPhase` emits a `(Fenced)StoreBarrier`
  immediately after each store, and resets the per-node "definitely new"
  set at every `doesGC()`-true node. `DFGStoreBarrierClusteringPhase`
  (`dfg/DFGStoreBarrierClusteringPhase.cpp:94-116`) is the only pass that
  *moves* barriers: it floats them forward to a "barrier point" defined as
  the last position before any node where
  `doesGC(m_graph, node) || mayExit(m_graph, node) != DoesNotExit` (`:104`).
  The JSThreads cooperative-stop poll is `CheckTraps`, and
  `dfg/DFGDoesGC.cpp:555-558` returns **true** for it (unconditionally,
  pre-dating threads). Therefore *no clustered barrier is ever deferred
  across a poll*, and `mayExit` covers every OSR-exit-capable node (the
  `:98-103` comment records exactly the JDK-8242115 trade-off and why JSC
  chose conservatism over a `StoreBarrierHint` exit-side fixup). SPEC-jit
  R1.f makes JSThreads stops cooperative-only (no thread is suspended at an
  arbitrary PC; a mutator parks only at a poll or inside a `doesGC`-true
  slow path), so the barrier-clustering predicate is *exactly* the set of
  program points at which a conducted stop can begin. SPEC-jit I21's
  poll-placement lint (CheckTraps + trailing InvalidationPoint) and the
  AUDIT-checktraps `jsThreadsParkableSlowPathClobbersHeapFacts` predicate
  reuse the same `doesGC` source of truth. B3/Air do not reorder across the
  lowered barrier patchpoint (it has heap effects).
- **Baseline / LLInt.** Store + barrier are emitted as a single template
  (`jit/JITPropertyAccess.cpp` `emitWriteBarrier`; LLInt
  `writeBarrierOnOperand*` macros) with no poll between; polls live only at
  `op_check_traps` bytecode boundaries.

Adversarial probes:

- *AUDIT-checktraps P10c-R residual* (`doesGC`-true nodes whose clobberize
  model is location-precise, e.g. rope-resolving string ops) is irrelevant
  here: the clustering predicate keys on `doesGC`, not clobberize, so those
  nodes are already barrier-points.
- *Barrier elision for "definitely new" bases*: the insertion phase resets
  its new-set at every `doesGC` node, so a base allocated before a poll is
  not treated as new after it — the N-mutator analog of HotSpot's
  "newly-allocated needs no barrier" reasoning is preserved per-thread.
- *Phase-1 backstop*: even a hypothetically dropped barrier is masked by
  SPEC-heap I12's window-witness over-retention (the to-cell's
  `newlyAllocated` bit, stamped by `stopAllocating`, keeps it live for the
  cycle independent of the remembered set). This backstop **disappears under
  SPEC-congc** out-of-window marking (black allocation + per-client CMS,
  congc §5.2/§6) — a congc-era regression in `DFGDoesGC` for any
  parkable-slow-path node would be the literal JDK-8254980 bug.

**Verdict: immune** (DFGDoesGC.cpp:555 + DFGStoreBarrierClusteringPhase.cpp:104
+ R1.f cooperative-only; pre-existing single-mutator-concurrent-GC machinery
already sound for N). Congc re-audit obligation: the §5.3(3) per-client
`barrierThreshold` JIT reroute (F19) must not change the *placement* logic,
only the address consumed; record as a CG-T3 arm.

## S12. Weak-table read vs concurrent sweep / weak-registry publication (JDK-8147611 / CVE-2026-7936 analog)

Two halves with opposite verdicts.

**S12a — mutator `WeakGCMap`/`WeakGCSet` read vs the collector's prune.**
`Heap::pruneStaleEntriesFromWeakGCHashTables` (`heap/Heap.cpp:3426-3432`)
runs from `runEndPhase` inside the conducted stop (deviation 4: every shared
collection is world-stopped, §10.4 barrier waits for every client's RHA).
Every mutator-side read of a WeakGCMap — `VM::symbolImplToSymbolMap`
(K4.III.4), `StructureTransitionTable`, `RegExpCache`, `m_symbolTableCache`
(K4.III.10) — runs *with heap access held* (the K4 audit's per-table lock is
about writer-writer, not reader-vs-GC). A stop cannot complete while a
reader holds access; a reader that releases access cannot resume mid-prune
(F8 re-entry gating). `WeakImpl` Live→Dead transitions happen only in
`WeakBlock::sweep`, and UNGIL-HANDOUT §F.2 / `WeakSet.h:121-131` records
that mutator-concurrent MSPL sweeps *skip weak-bearing blocks* — weak state
flips only world-stopped. **Immune** (deviation 4 + F8 + the §F.2
weak-bearing-skip rule). Congc re-audit: SPEC-congc §9.1(8) keeps prune
in-window; the AB-10 conductor weak-sweep license (`WeakSet.cpp:81/:106`)
must stay world-stopped-only.

**S12b — `m_weakGCHashTables` registry publication race (suspected,
confirmed in-tree).** `Heap::registerWeakGCHashTable` /
`unregisterWeakGCHashTable` (`heap/Heap.cpp:4422-4429`) mutate the bare
`UncheckedKeyHashSet<WeakGCHashTable*> m_weakGCHashTables` (`Heap.h:1705`)
with **no lock and no `// SharedGC:` audit tag**. The K4.VIII.9 audit row
(SPEC-ungil-audit-K4.md, A-t8assert 2026-06-12) reproduced this as an ASAN
SEGV (freed-scribble `0xf5` read) with two lites concurrently running
`JSGlobalObject::init` — every global ctor registers several `WeakGCMap`s
via `WeakGCMapInlines.h:40` / `WeakGCSetInlines.h:38`. The MC-GC consequence
is the *quiet* failure mode, not the loud SEGV: a torn `HashSet::add` can
**lose a registration** (rehash drops the colliding bucket), so the table is
reachable to mutators but invisible to `pruneStaleEntries` — its `Weak<>`
slots are still flipped Dead by the world-stopped weak sweep, but the
table's own `pruneStaleEntries` never runs, so a mutator's `find()` walks
buckets whose `Weak::get()` returns null forever (identity loss for caches
keyed on liveness, e.g. a transition-table re-add inserting a duplicate
Structure) and, if the table's `pruneStaleEntries` override does more than
HashMap removal (e.g. drops a paired strong ref), that paired state leaks.
Worse, a torn rehash can leave a *stale bucket pointer* in the registry, and
the next conducted prune dereferences it world-stopped — collector-side UAF.
This is exactly the "collector's view of the registry diverges from what
mutators can observe" sub-mechanism. K4.VIII.9 filed it as heap-territory
("lock, or lite-local registry + merge"); it remains **unimplemented** at
`Heap.cpp:4422-4429`.

**Verdict: S12a immune; S12b susceptible-suspected (concrete reproduced
defect, K4.VIII.9) + needs-test** —
`JSTests/threads/cve/mc-gc-weakgcmap-registry-vs-prune.js` (concurrent
`$vm.createGlobalObject()` registration storm racing conducted full GCs;
oracles: no crash, and `Symbol.for` identity — backed by the VM-level
`symbolImplToSymbolMap` whose registration races the storm — survives the
prune cycle on every thread).

## S13. Objects visible to GC before fully initialized (JDK-8278965 / JDK-8187577 analog)

The GC-observer twin of map-MC-INIT (which owns the *mutator*-observer
half). Mechanism: a cell becomes reachable to the collector (via a
mark-keyed registry, the window-witness set, or a conservative root) before
its `Structure`/header/out-of-line storage is initialized; the collector
either traces garbage or — the JDK-8187577 shape — *retains the shell
without tracing its contents*, so a downstream registry consumer adopts a
half-built object.

Governing invariants, both in SPEC-heap I12 (rev 13, history §25):

1. **L2 — "no client releases heap access mid-allocation/initialization."**
   Every JSCell allocation path runs `finishCreation` (structure store +
   field init) under the same access span as the allocation; the only
   release points are cooperative polls / RHA brackets, and no allocation
   slow path contains one between the allocator return and the structure
   store (the `DeferGC`-shaped guards in allocation paths exist precisely to
   keep this span closed). A conducted stop therefore never observes a cell
   with a torn header.
2. **Wlr "mark-without-trace is FORBIDDEN"** (`heap/Heap.cpp:4535-4551`, the
   normative window-witness constraint banner): the conducted cycle's
   window-witness retention appends through the *real* conservative-root
   path (`appendJSCellOrAuxiliary`: mark **and** trace) inside the marking
   fixpoint, "so every mark-keyed registry (e.g. the transition
   WeakGCHashTables pruned after endMarking) only ever observes CONSISTENT
   retained objects." The banner records the regression that proved
   mark-without-trace unsound here: a retained zombie dictionary `Structure`
   stayed findable in the transition `WeakGCHashTable` with a *swept*
   `PropertyTable` and was re-adopted — the literal JDK-8187577 mechanism,
   found and fixed in our own tree (history §25;
   `JSTests/threads/objectmodel/i03-quarantine-readd-across-gc.js`).

Adversarial: the L2 lemma is *not* assert-backed per allocation; it is
discharged by code review (no `releaseAccess`/`stopIfNecessary` between
`allocate*` and `finishCreation`). A future native that allocates, stashes
the raw cell, and then drops into an RHA bracket before `finishCreation`
would violate it — same review rule as the S1 native-heap-memory note. The
Wlr constraint *is* mechanically enforced (the gate keys on sticky ISS,
history §25 round-7 F5).

**Verdict: immune** (I12 L2 + Wlr mark-with-trace; in-tree regression test
in place). Mutator-observer publication ordering is owned by map-MC-INIT
S1/S4. Congc re-audit: out-of-window black allocation (congc §6) must
preserve L2 — congc CG-I9's "window barrier orders it" is the recorded
discharge.

---

## Summary table

| # | Surface | Verdict | Action |
|---|---------|---------|--------|
| S1 | premature reclaim under blocked native frame (RHA window) | immune-by-design, flagship → **needs-test** | `mc-gc-blocked-native-roots.js` |
| S2a | ASAN fake-stack vs conservative scan | **suspected** (build-config; reproduced in-tree) | pin UAR off in Linux ASAN/fuzz lanes; record in TSAN.md/FUZZ.md |
| S2b | per-thread CLoop stacks | immune on JIT builds / suspected-if-C_LOOP | keep `Heap.cpp:1043-1056` note; ladder pins JIT builds |
| S3 | spawn handoff publication | immune (5.10 Strongs precede `Thread::create`) | — |
| S4 | thread exit vs collector sampling | immune (EXIT1.3 ordering) | teardown half owned by map-MC-TDWN |
| S5 | 5.10 native finalizer in stop window | **FIXED** (B7: carve-out (b) deferral landed — `Heap::LambdaFinalizerOwner::finalize` defers under WSAC, drained post-resume with-access at `conductSharedCollection` tail; gilOff-gated) + needs-test | `mc-gc-thread-shell-finalizer-storm.js` |
| S6 | FinalizationRegistry enqueue from a shared conductor | item 1 **FIXED** (B8: `\|\| WSAC` disjunct landed in `DeferredWorkTimer.cpp` `addPendingWork` entry assert); item 2 §E.7 routing → **needs-test** | `mc-gc-finreg-cross-thread-gc.js` |
| S7 | §11 epoch reclamation | immune (I11 assert-backed; attach/detach edges engineered) | covered by existing corpus/unit tests |
| S8 | dead-TID reissue vs surviving tags | design-immune; multi-VM tripwire = known abort | test owned by map-MC-TDWN S10 |
| S9 | async-ticket dependency rooting | immune (double-rooted; access rule closes the window) | — |
| S10 | superseded butterflies / quarantined slots | immune (I7/I18 + OM tests) | — |
| S11 | write-barrier split across safepoint / OSR exit | immune (`DFGDoesGC.cpp:555` + clustering `:104` + R1.f) | congc §5.3 re-audit (CG-T3 arm) |
| S12a | WeakGCMap read vs in-stop prune | immune (deviation 4 + F8 + §F.2 weak-bearing-skip) | congc AB-10 license stays world-stopped |
| S12b | `m_weakGCHashTables` registry race → lost prune / UAF | **suspected** (K4.VIII.9 reproduced; `Heap.cpp:4422-4429` unlocked) + needs-test | `mc-gc-weakgcmap-registry-vs-prune.js`; lock or lite-local registry |
| S13 | object visible to GC pre-init (mark-keyed registries) | immune (I12 L2 + Wlr mark-with-trace; `i03-quarantine-readd-across-gc.js`) | mutator-observer half = map-MC-INIT |

Cross-cutting: **every immune verdict above leans on deviation 4 (no
concurrent marking in shared mode).** SPEC-congc work MUST re-run this map;
the per-surface congc notes (S5, S6.3, S9, S10, S11, S12a, S13) are the
starting list.

Tests are written for post-ungil execution (do not run against the
mid-bring-up tree); flag requirements are in each test's `//@` header.
