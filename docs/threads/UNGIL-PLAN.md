# UNGIL-PLAN.md — Ground-truth GIL-dependency inventory (pre-design)

Status: INVENTORY + CLASSIFICATION ONLY. The post-GIL execution-model design
itself is the next phase's deliverable (SPEC-ungil.md); nothing here is a
design decision except where a MANDATED SHAPE is recorded from THREAD.md or
an orchestrator directive. Verified against the tree on 2026-06-05, branch
jarred/threads.

Authorities consulted: THREAD.md; docs/threads/SPEC-{heap,vmstate,objectmodel,
jit,api}.md (+ annexes/history); docs/threads/INTEGRATE-*.md (landed
deviations D1-D13 in INTEGRATE-api.md "Landed deviations").

Classification key:
- **DESIGNED-FOR** — the landed protocol is already N-mutator-sound as
  written; GIL removal needs no redesign here (cite spec section).
- **CHARTERED** — the five frozen specs explicitly deferred this to a
  post-GIL charter ("re-frozen at GIL removal"); the charter text is the
  binding scope (cite the charter).
- **GAP** — serialization the GIL provides that no spec clause designs OR
  charters precisely enough to implement from; SPEC-ungil must design it.

---

## Part I — Code inventory: every useThreadGIL / JSLock serialization dependency

`Options::useThreadGIL` exists in exactly two places
(`runtime/OptionsList.h:696` definition, default true, "always on in phase 1;
reserved"; `runtime/JSLock.cpp:151` consumption). Everything else depends on
the GIL *structurally*: JSLock mutual exclusion is the soundness argument,
cited in comments as "phase-1 GIL" / "GPO" (GIL-phase-only). Inventory:

### I.1 JSLock.cpp — the GIL itself

| # | Site | What the GIL provides | Class |
|---|------|----------------------|-------|
| L1 | `runtime/JSLock.cpp:151` `RELEASE_ASSERT(!Options::useJSThreads() \|\| Options::useThreadGIL())` inside `didAcquireLock()` main-carrier VMLite install | Fail-stop backstop: no GIL-off config may reach the tid-0 main-carrier install path (two threads sharing tid 0 would race unlocked flat-butterfly transitions, comment at :136-148). | GAP (item J end-state + item A per-thread carriers; the assert's *replacement* is vmstate Dev 10 Phase B CHARTERED, see A) |
| L2 | `JSLock.cpp:118-188` `didAcquireLock()` acquisition migration: atom table swap (`:126`, `m_entryAtomStringTable`), main-carrier VMLite install (`:129-155`), `setLastStackTop` (`:157`), heap-access acquire forwarding (`:159-164`), `stackPointerAtVMEntry` (`:166-168`), `machineThreads().addCurrentThread()` (`:172`), `traps().notifyGrabAllLocks()` (`:179`) | Per-thread execution state is *moved into* the one VM at each handoff. SPEC-vmstate §6.1 calls this swap **load-bearing** and frozen for the GIL phase ("M4's only change=§6.4.4", SPEC-vmstate.md:342). Post-GIL, consumption must become per-thread (Phase B) and heap-access forwarding becomes per-thread GCClient (heap Dev 8). | CHARTERED (vmstate Dev 10 Phase B, SPEC-vmstate.md:37-48, 335-349; heap §3.8/Dev 7+8, SPEC-heap.md:26-27) |
| L3 | `JSLock.cpp:329-374` `willReleaseLock()`: microtask drain guarded on `!m_lockDropDepth` (`:342-343`), `clearLastException` (`:345-346`), `releaseDelayedReleasedObjects` (`:348`), `stackPointerAtVMEntry` clear (`:349`), heap-access release (`:351-352`), VMLite restore (`:356-368`), atom-table restore (`:370-373`) | Lock release = the only "between turns" point; the drain-on-release is the GIL-phase microtask scheduling model. Post-GIL the drain re-homes to per-thread queues (item E); the rest of the teardown becomes per-thread state that never needs restoring. | GAP (item E for the drain; rest follows L2's charter) |
| L4 | `JSLock.cpp:389-408` `unlockAllForThreadParking()` (the applied INTEGRATE-api 9.2-9 hunk, D11 closed): bumps/restores `m_lockDropDepth` around `willReleaseLock()` to suppress the drain at park sites | Pure GIL machinery — exists only so a park site doesn't run user JS mid-host-call. | GAP (item J end state: dead code in GIL-off mode, kept for fallback) |
| L5 | `JSLock.cpp:421-460` `dropAllLocks()`/`grabAllLocks()` + `m_lockDropDepth` strict-LIFO loop (`:449-453`); `DropAllLocks` ctor/dtor `:462-495` | Embedder re-entrancy protocol. D1 (INTEGRATE-api) bans embedder DropAllLocks on the shared VM while Threads are live; coexistence is D1's **open rev-15 question**. | GAP (item F; D1 explicitly left it open) |
| L6 | `JSLock.cpp:84-116` `lock()`: single `m_lock` mutex + `m_ownerThread`/`m_lockCount`; `currentThreadIsHoldingLock()` consumed by RELEASE_ASSERTs across the runtime (e.g. `unlock()` :308-311, `stopTheWorldAndRun` R1 contract) | THE global mutex. Post-GIL contract is item F's mandated shape (per-thread entered-token; asserts reinterpreted, never deleted). | GAP (item F mandated shape) |
| L7 | `JSLock.cpp:166` `RELEASE_ASSERT(!m_vm->stackPointerAtVMEntry())` | Only one thread may have a live VM entry SP — definitionally false with N entered threads; field must become VMLite/thread-local. | CHARTERED (vmstate Phase B: `stackPointerAtVMEntry` is in the Group-3 per-thread set, SPEC-vmstate.md:37, THREAD.md:19) |

### I.2 ThreadObject.cpp — threadMain spawn/run/completion

| # | Site | Dependency | Class |
|---|------|-----------|-------|
| T1 | `runtime/ThreadObject.cpp:170` `JSLockHolder locker(vm)` — "The GIL: all JS execution is serialized by the shared VM's JSLock" | The whole-thread-body lock hold. Post-GIL this degrades to the item-F entered-token + per-thread GCClient attach (heap Dev 8 ACT). | CHARTERED (api §5.2 + heap Dev 8) for the token/client; GAP for the exact JSLockHolder degradation semantics (item F) |
| T2 | `ThreadObject.cpp:175` `GILParkSavedExecutionState::resetForFreshThread(vm)` | Scrubs the previous GIL holder's stack pointers out of the shared VM. Exists ONLY because execution state lives in the VM; with Phase B per-thread state it is meaningless. | CHARTERED (vmstate Phase B); deletion is item J |
| T3 | `ThreadObject.cpp:205-208` completion sequence `vm.drainMicrotasks()` — comment: "GIL-phase rule; post-GIL: own queue until empty" (mirrors SPEC-api 4.6.1, SPEC-api.md:100) | GIL-phase: one shared queue, drained once. Post-GIL: per-thread queue drained to empty + task queue + keepalive (item E mandated shape). | GAP (item E; api 4.6.1 marks the clause GPO and names the post-GIL rule but does not design the queues/keepalive) |
| T4 | `ThreadObject.cpp:212-231` F1/F5 completion protocol: result Strong before Phase release-store ("redundant under the GIL, **load-bearing post-GIL**"), joinLock + notifyAll + asyncJoiners swap, settle moved tickets | Written for N mutators already; the release/acquire pairing and never-wait rule survive GIL removal as-is. | DESIGNED-FOR (api F1/F5, 4.6.1) — except join *settlement timing* changes under item E (join settles at queues-empty+keepalive==0, not fn-return) |
| T5 | `ThreadObject.cpp:233-248` Strong clears, `unregisterThread`, `unregisterLite`, `setCurrent(nullptr)`, TID tag clear, lite destroyed after lock release | 5.2/5.10/N8 sequence is per-thread and lock-ordered; survives. TID retired forever = Deviation 10 (`:243`), lifted by OM Task 13 rebias. | DESIGNED-FOR (api 5.2/5.10, vmstate N8); TID retirement CHARTERED (OM Task 13/api Dev 10, item D) |
| T6 | `ThreadObject.cpp:333-365` join park: `GILDroppedSection droppedSection(vm)` at `:337`, 10ms quanta termination poll (D9) | Park-with-GIL-dropped + poll. Post-GIL there is no GIL to drop; the park becomes a wait on the thread's own runloop/inbox (item E) and the blocking-policy gate is item G. | GAP (items E/G); the D9 poll *predicate* (trap bits, LockObject.h:144-156) is DESIGNED-FOR and survives |
| T7 | `ThreadObject.cpp:92-136` spawn ("spawner under the GIL pre Thread::create"), `:396-404` asyncJoin ticket settle "on a run-loop turn, never synchronously" (I12) | I12 is GI (GIL-independent) per api; but *which* runloop settles is GPO ("settling thr unspecified; post-GIL=registering thr, dead=>main", SPEC-api.md:306). | DESIGNED-FOR (I12 invariant) + GAP (item E routing: registering thread's task queue + wake) |

### I.3 LockObject.h / LockObject.cpp — GIL machinery proper

| # | Site | Dependency | Class |
|---|------|-----------|-------|
| K1 | `runtime/LockObject.h:186-208` + `LockObject.cpp:67-99` `GILParkSavedExecutionState`: saves/restores topCallFrame, exception scope chain, lastStackTop, stackPointerAtVMEntry around parks | Exists only because execution state is VM-resident. Phase B makes it per-thread; class deleted (kept compilable under useThreadGIL fallback, item J). | CHARTERED (vmstate Phase B) |
| K2 | `LockObject.h:249-258` + `LockObject.cpp:101-125` `GILDroppedSection` (D1 replacement for DropAllLocks; depth-free; routes `unlockAllForThreadParking`) | The canonical park-site GIL release. Post-GIL park sites hold no global lock to release; the section degrades to (a) heap-access release/safepoint-park cooperation with shared GC stops and (b) nothing else. | GAP (item J end state; the GC-stop cooperation half is DESIGNED-FOR via heap §10/F8 per-client access state, SPEC-heap.md:27 "already thread-granular") |
| K3 | `LockObject.h:159-174` + `LockObject.cpp:127-150` `jsThreadGILHandoffYield` — "Phase-1 GIL stub fairness primitive"; consumed by `ConditionObject.cpp:364` (notifyImpl, deviation D2 notify-as-yield-point) | Pure GIL fairness; meaningless post-GIL (notify just wakes waiters that run in parallel). D2's semantic consequence (foreign JS runs inside notify()) must be re-derived. | GAP (item C: D2 re-derivation is named in the api Dev 12 re-freeze bundle) |
| K4 | `LockObject.h:144-156` D9 park-poll predicate (trap-bit poll, watchdog deferral note) | Per-thread trap polling while parked; written against VMTraps bits, which become per-thread VMThreadContext in Phase B but the *predicate shape* is thread-local already. | DESIGNED-FOR (api D9 normative-amendment request) with consumption re-pointed by Phase B (item A) |
| K5 | `LockObject.cpp:334-380` contended `lock.hold`: GILDroppedSection + 10ms `tryLockWithTimeout` quanta; `:448-465` release settles next grant via `settleLockGrant` | NativeLockState's m_lock/m_queueLock protocol (api 5.3) is rank-ordered and thread-correct on its own; only the GIL drop wrapper and the "release under the GIL" happens-before (api 5.6 GPO, SPEC-api.md:225) change. | DESIGNED-FOR (api 5.3/I6-I8) + the 5.6 happens-before re-base is CHARTERED (api Dev 12) |
| K6 | `LockObject.cpp:155-171` run-loop pump task routing — comment: "routing is what survives GIL removal (the pump must run on the run loop)"; D5-adjacent Ref<VM> + cancelled-bailout | Explicitly marked GI. | DESIGNED-FOR (api 5.5a P; INTEGRATE-api D5/D6) |

### I.4 DeferredWorkTimer / runloop settlement paths

| # | Site | Dependency | Class |
|---|------|-----------|-------|
| S1 | `runtime/ThreadManager.cpp:41-100` `AsyncTicket`: `addPendingWork` (`:72`), `settle()` CAS + `scheduleWorkSoon` (`:78-88`), Strong cleared under API lock (`:50-53`) | All settlement funnels through the ONE VM's `vm.deferredWorkTimer` and hence ONE runloop. Post-GIL mandated shape: enqueue to the REGISTERING thread's task queue + wake its inbox; dead-thread fallback to main (api 5.5 post-GIL surface, SPEC-api.md:200: per-TS ticket inbox + RL-wakeup hook, "settler never enqueues into another's MicrotaskQueue (vmstate I11); u/owner's inboxLock, inboxOpen=>append+wake owner RL; else append to main TS inbox"). | CHARTERED (api 5.5 post-GIL surface text exists and the per-TS `inboxLock`/`inbox`/`inboxOpen` fields are landed-but-inert, SPEC-api.md:126) — but the runloop/task-queue substrate they enqueue into is GAP (item E) |
| S2 | `ThreadManager.cpp:235` Thread.restrict Weak "created under the GIL (host call)" + restrict affinity table | Table is its own lock; GIL only guarantees host-call context. Survives with item-F token. | DESIGNED-FOR (api 5.7, D13) |
| S3 | `runtime/LockObject.cpp:228-271` `settleLockGrant` settle tasks "run on a run-loop turn" | Same routing as S1. | CHARTERED/GAP as S1 |
| S4 | `runtime/ThreadAtomics.cpp:635-760` `waitAsync`: ticket settle via runloop (`:652-692` Ref<VM> capture, D5 cancelled-bailout; `:737-746` notify collects under list lock, settles later) | Same routing as S1; the list-lock-then-settle split is N-mutator-sound. | DESIGNED-FOR (api 5.6 list protocol, I10) + S1's routing gap |
| S5 | `ThreadAtomics.cpp:402-430` infinite-timeout waitAsync per-cell heap finalizer sweep (D5 round-4 companion) | Teardown path; per-cell, lock-held; survives. | DESIGNED-FOR (D5 companion, requested 5.6/5.10 amendment) |

### I.5 ThreadAtomics.cpp — property atomics

| # | Site | Dependency | Class |
|---|------|-----------|-------|
| P1 | `ThreadAtomics.cpp:109-133` "SPEC-api 4.5: every property op is 'one atomic step'. **Under the phase-1 GIL**" — atomicity of the read-probe/CAS/RMW bodies is the GIL itself; comment: "Post-GIL these bodies re-home" onto OM §9.5 atomic slot accessors | The central GPO of the whole API surface. | CHARTERED (api Dev 12 / OM 8g: "atomic slot CAS/RMW added to OM §9.5 then", SPEC-api.md:22; item C) |
| P2 | `ThreadAtomics.cpp:536-537` G11 gate guards the *block* (4.5 step 1a TA sync-wait gate, I21: spawned Thread sync `Atomics.wait` on a view throws TypeError) | GPO by spec text ("lifted post-GIL", SPEC-api.md:79; I21 "deleted by re-freeze", :315). | CHARTERED (api Dev 12 bundle lifts 4.5-1a; item C) — but the *replacement* per-thread blocking policy is GAP (item G) |
| P3 | `ThreadAtomics.cpp:552-605` property `Atomics.wait` park: GILDroppedSection (`:569`), 10ms quanta + termination poll, "store+notify window (I10); no list lock held across the GIL drop" | List protocol DESIGNED-FOR (I10); the GIL-drop wrapper goes per K2; waiter *arming* re-homes to owner inboxes per api Dev 12. | DESIGNED-FOR (I10) + CHARTERED (Dev 12 PWT re-home; item C) |
| P4 | `runtime/AtomicsObject.cpp:530` `isAtomicsWaitAllowedOnCurrentThread()` (per-VM `m_typedArrayController` gate, G11) | Per-VM gate is the wrong granularity once N threads share the VM: main/embedder threads of the one VM need different answers than spawned Threads. | GAP (item G) |
| P5 | `AtomicsObject.cpp:506-579` D4 GIL-dropped main-thread TA sync wait + D8 per-VM single-flight gate (`syncTAWaitGateLock`, `vmsWithSyncTAWaitInFlight`, second waiter throws) | D8 exists only because D4 removed the API-lock guarantee under the GIL while `vm.syncWaiter()` is a single per-VM node. Post-GIL re-freeze (Dev 12) specifies per-wait waiter nodes; D4+D8 "lifted together". | CHARTERED (api Dev 12; INTEGRATE-api D4/D8 "Lifted together with D4") |

### I.6 JSThreadsSafepoint.cpp — the enteredVMs<=1 stub

| # | Site | Dependency | Class |
|---|------|-----------|-------|
| W1 | `bytecode/JSThreadsSafepoint.cpp:244-250` `stopTheWorldAndRun`: count entered VMs, `RELEASE_ASSERT(enteredVMs <= 1)` at `:250`; comment :227-231 "Phase-1 GIL: ... 'the world' is exactly the calling thread" | The load-bearing soundness argument for running Class-A stop closures inline. Post-GIL: real stop/resume (R1.a-i steps 3+5, listed at `:208-221`) with **THREAD-granular** arbitration — count entered THREADS per VM, not VMs. | CHARTERED (jit R1 freeze scope, SPEC-jit.md:233: "N threads in ONE VM ... = thread-granular STW (vmstate Dev-10 Phase-B charter, api §2); **R1.c re-frozen there**") — item A |
| W2 | `JSThreadsSafepoint.cpp:126-154` `assertAlreadyStoppedEvidenceCoversEveryMutator` (`:153` `RELEASE_ASSERT(enteredVMsNotCoveredByStop <= (stoppedServer ? 0u : 1u))`) | Sampled tripwire on the same premise; R3-4 notes it is not the soundness mechanism. Deleted at M4. | CHARTERED (same as W1; explicitly "Deleted at M4", `:242-243`) |
| W3 | `JSThreadsSafepoint.cpp:97` `s_stubWorldStoppedDepth` process-global (non-TLS) witness — "correct for the interim stub: the phase-1 GIL guarantees at most one entered mutator" | Witness must become real per-mutator NVS parking at M4. | CHARTERED (jit R1.d/M4) |
| W4 | `runtime/VMEntryScope.cpp:44-100` M7 structural entered-VM tripwire (`s_jsThreadsEnteredLegacyVMs` etc.) — "DELETE at integration manifest M4: ... the GIL-removal change replaces the premise wholesale" | Companion enforcement; self-documents its deletion. | CHARTERED (INTEGRATE-jit M7 note; replaced by item A's entered-THREAD counting) |
| W5 | `JSThreadsSafepoint.cpp:252-304` R1.i shared-server GC bracket (client-scoped heap-access release + JSThreadsStopScope) | Already keyed on the requesting *client* (R4-1), i.e. per-thread-correct once clients are per-thread. | DESIGNED-FOR (heap §9/§10.2 G13, jit CS2-RESOLVED) |

### I.7 Other GIL-relying state (confirmed by spec text, code shared-VM-wide)

| # | Site | Dependency | Class |
|---|------|-----------|-------|
| X1 | Atom table: process/VM `AtomStringTable` swapped per JSLock handoff (L2); THREAD.md:19 says sharded/concurrent table is the design; the sharded atom table is landed (per workflow context) but *currentAtomStringTable routing* still rides the JSLock swap | Post-GIL: every entered thread points at the shared table permanently; swap deleted. | DESIGNED-FOR (table itself) + Phase-B consumption (item A) |
| X2 | `SymbolRegistry.cpp:58` (+ teardown `:40-43`): `Symbol.for` registry accessed under the JSLock only (SPEC-vmstate.md:40: "post-GIL=§2 non-goal (history)"; vmstate Dev 8 "post-GIL Symbol.for/SymbolRegistry sync (UNOWNED)", SPEC-vmstate.md:57) | No spec owns the post-GIL locking. | GAP (item H; vmstate names it UNOWNED — confirmed) |
| X3 | `VM.cpp` microtask queue + `drainMicrotasks` (single `m_microtaskQueue`); vmstate I11 "settler never enqueues into another's MicrotaskQueue"; VMLite carries a queue slot ("Phase B routes to the current thread's queue", SPEC-vmstate.md:529) | The queue *slot* exists per-lite; the drain loop, host hooks, and ownership rules do not. | GAP (item E) layered on CHARTERED plumbing (vmstate §6.6) |
| X4 | Scratch buffers, regexp stack, exception state, topCallFrame: VM-resident, migrated by L2 | Phase B Group-3 set (THREAD.md:19 "Per-thread 'VM-lite' split: top call frame, exception state, stack limits, scratch buffers, microtask queue, lazy regexp stack"). | CHARTERED (vmstate Phase B) |
| X5 | Wasm on spawned threads: no gate exists in ThreadObject.cpp (spawned thread runs arbitrary `fn`, which can reach WebAssembly.*); no spec clause addresses Wasm-vs-Thread at all (grep: zero wasm hits in SPEC-api.md) | Unspecified behavior post-GIL (per-VM wasm state, signal handling, IPInt/OMG tiers never audited for N threads in one VM). | GAP (item I) |

---

## Part II — What the five frozen specs say

### SPEC-api (rev 14)
- **GPO marker is formal**: "semantics final except 'GPO'=GIL-phase-only
  clauses" (SPEC-api.md:3). GPO clauses: 4.5-1a TA gate (:79, I21 :315),
  4.6.1 completion drain ("post-GIL: own queue till empty", :100), 5.6
  happens-before=JSL (:225), I12 settling-thread unspecified (:306), TID-0
  embedder note (:361).
- **The §2 composed deliverable** (:26) is the master charter list binding
  all five specs: GIL removal GATED on — heap Dev 7 (per-THREAD TLC
  addressing) + heap §3.8 per-thread-client model; vmstate Dev 10 Phase B
  incl. thread-granular STW (VMM counts entered THREADS per VM; jit R1.c
  re-frozen there) — **HARD precondition**; OM Tasks 13-14 (14 decided
  PRE-INT on the GIL-stub construction bench); jit §4.3 revival; Dev 12/OM
  8g (atomic slot CAS/RMW + PWT re-home + 4.5-1a lift). All chartered
  (owner+frozen interface+budget) by the orchestrator BEFORE GIL removal.
- **Already N-mutator-sound as written** (DESIGNED-FOR): F1/F5 completion
  ordering ("load-bearing post-GIL"); 5.3 NativeLockState rank protocol; 5.6
  list protocol I10; D9 poll predicate; 5.9 lock-rank table; 5.10 Strong
  discipline; restrict affinity table; per-TS inbox fields (landed inert,
  :126) and the 5.5 post-GIL settlement routing *text* (:200).
- **Landed deviations needing post-GIL re-freeze** (INTEGRATE-api
  :826-1064): D1 (GILDroppedSection normative + DropAllLocks coexistence —
  open), D2 (notify-yield — re-derive corpus interleavings), D4/D8 (lifted
  together at Dev 12), D9 (poll normative), D11 (closed by 9.2-9), D12
  (grant-runner rule), D13 (restrict allowlist).

### SPEC-vmstate
- Phase A frozen / **Phase B UNOWNED** (SPEC-vmstate.md:42-48): "'Phase B'
  refs here=frozen contract for a FUTURE chartered WS ... r12: Phase B ALSO
  covers thread-granular STW — per-thread parking, VMM counts entered
  THREADS per VM (per-thread NVS tickets); jit R1.c re-frozen there."
- Phase B scope (:335-349, 418, 443, 490, 529, 551, 572): pinned
  register/TLS base; `VM::field` accesses become VMLite-relative (per-field
  offsets frozen at :418); accessor signatures frozen, impl replaceable
  (:443); main-thread carrier decision deferred to Phase B (:490, §6.4.4);
  microtask routing to current thread's queue (:529); embedder-thread
  post-GIL lazy lite (:551); per-thread JSLock-handoff replacement (:572).
- §6.1 GIL handoff is **load-bearing and frozen** for phase 1 (:23, :342).
- Dev 8: SymbolRegistry post-GIL sync UNOWNED (:40, :57).

### SPEC-heap
- Dev 7 (SPEC-heap.md:26): GIL-masked carve-outs — §5.5 slow-path alloc,
  §3.4 sync GC, §5.2 single-MSPL. "Chartered WS gating GIL removal:
  TLC-aware inline emission — addressing contract MUST be per-THREAD
  (VMLite/TLS-relative, §3.8); the §5.3 vm-relative chain is GIL-phase-only
  (deviation 6)". Budget: {1,0} gated <=5%; {1,1} budget "set at GIL-removal
  chartering with the TLC-emission charter".
- Dev 8 (:27): **the post-GIL heap execution model is already normative**:
  "N mutators=ONE GCClient::Heap PER Thread sharing the server; §10A access
  state, §5.3 TLC, §5.4 deferral, §11 epoch all stay per-CLIENT exactly as
  specced, instantiated per thread; Thread attach/detach=ACT/DCT on that
  thread's OWN client. Client lifecycle inside one VM (creation/teardown
  wiring)=chartered with deviation 7+vmstate Dev 10 Phase B." And: "the §10
  GC stop barrier is **already thread-granular** (per-client access state,
  deviation 5); only VMM trap delivery/jit R1 arbitration are VM-granular
  (Phase-B charter)."

### SPEC-objectmodel
- TID/SW dispatch, segmented butterflies, M5 nuking+DCAS, M7 ordering, I33
  bounds, §9.5 accessor contract: DESIGNED-FOR N mutators (that was the
  point); GIL only masks their cost, not their correctness.
- Task 13 (SPEC-objectmodel.md:377): "(post-GIL, chartered-owned w/ api)
  GC-time TID rebias/reissue per 8c". Task 14 (:378): "(post-GIL,
  chartered-owned) per-thread structure splitting per 8h"; :359 — cell-locked
  N2 stands pending Task 14, "promotion DECIDED PRE-INT on jit Task-13's
  GIL-stub construction bench".
- 8g: atomic property-slot CAS/RMW added to §9.5 at the api Dev-12
  re-freeze (api :22).

### SPEC-jit
- R1 freeze scope (SPEC-jit.md:233): "VM-counting arbitration=final only for
  the N-separate-VMs config; N threads in ONE VM (api §5.2, post-GIL)=
  thread-granular STW (vmstate Dev-10 Phase-B charter, api §2); **R1.c
  re-frozen there**."
- Task 13 (:278): INTEGRATION-GATE "validates the N-separate-VMs config
  ONLY — N threads in ONE VM=Phase-B charter (R1 freeze scope), a **HARD
  GIL-removal precondition**, api §2; green != one-VM coverage."
- Per-tier TTL watchpoint checks, Class-A stop protocol, epoch
  retire/reclaim, IC publish discipline: DESIGNED-FOR N mutators, contingent
  only on a real stop (W1) replacing the stub.

---

## Part III — Gap list (confirmed/extended), A-J

### A. vmstate Phase B: per-thread execution-state CONSUMPTION — **CONFIRMED, the big one. CHARTERED (charter exists: vmstate Dev 10 / api §2 / jit R1 freeze scope) but UNDESIGNED (Phase B is explicitly UNOWNED, SPEC-vmstate.md:42)**
Sub-items, each currently GIL-carried:
1. Pinned TLS/register base for the current lite; `VM::field` access in
   LLInt asm / Baseline / DFG / FTL becomes VMLitePrimitives-relative
   (frozen offsets, SPEC-vmstate.md:418; "no interpreter/JIT/runtime path"
   touched in Phase A, :344).
2. Per-thread VMThreadContext/VMTraps: stack limits, trap bits, termination
   delivery per entered thread (today `traps().notifyGrabAllLocks()` at
   JSLock.cpp:179 re-targets the one trap set at each handoff).
3. Scratch-buffer/regexp-stack/exception-state rerouting (X4).
4. Main-thread carrier choice: §6.4.4 install (JSLock.cpp:129-155) vs
   §6.4(3) view — explicitly "Phase B decides" (SPEC-vmstate.md:490).
   Lifting it deletes backstop L1.
5. THREAD-granular VMManager stop arbitration: count entered THREADS, not
   VMs; per-thread NVS park tickets; re-freeze jit R1.c; replaces the
   in-tree `RELEASE_ASSERT(enteredVMs <= 1)` stub
   (JSThreadsSafepoint.cpp:250) and the M7 tripwire (VMEntryScope.cpp:44,
   self-marked DELETE-at-M4).

### B. Per-thread GCClient lifecycle in one VM — **CONFIRMED. CHARTERED (heap Dev 8 is normative on the model; lifecycle wiring chartered "with deviation 7 + vmstate Dev 10 Phase B; owner recorded in INTEGRATE", SPEC-heap.md:27)**
- Client create at spawn (before first allocation, alongside the
  ThreadObject.cpp:162-166 lite handshake) / teardown at exit (T5 sequence);
  ACT/DCT on the thread's OWN client.
- Replaces JSLock heap-access forwarding (JSLock.cpp:159-164, 351-352) —
  per-client access state already exists (heap deviation 5; W5's
  ClientHeapAccessReleaseScope is the in-tree pattern).
- TLC-aware per-thread inline-allocation emission (heap Dev 7, §3.8,
  VMLite/TLS-relative addressing) + its perf budget — budget explicitly
  deferred to "GIL-removal chartering" (heap Dev 7); {1,0} miss => jit §4.3
  LLInt-cache revival REQUIRED pre-ship.

### C. api Dev 12 / OM 8g re-freeze — **CONFIRMED. CHARTERED (api §2 lists it; SPEC-api.md:22 "UNOWNED chartered WS")**
- Atomic property-slot CAS/RMW added to OM §9.5 (replaces P1's GIL-step
  atomicity; D3's exotic-receiver exclusions and D7's writability rule must
  be carried into the atomic bodies).
- Property-waiter arming re-homed to owner inboxes (api 5.5 post-GIL
  surface text at :200; PWT re-home).
- 4.5-1a TA-gate lift (P2/I21 deleted) — interacts with item G.
- D2 notify-yield re-derivation: post-GIL notify() is not a yield point
  (there is nothing to yield); the corpus tests that green-light BECAUSE of
  D2 (condition-notify-all*, wait-notify-storm — INTEGRATE-api D2) must
  have their interleaving assumptions re-derived.
- D4/D8 lifted together (per-wait waiter nodes).
- D1 coexistence ruling and D12 asymmetry ruling fold into the same rev-15.

### D. OM Task 13 / Task 14 — **CONFIRMED. CHARTERED (SPEC-objectmodel.md:377-378)**
- Task 13 (TID rebias/reissue at shared-GC stops, 8c, co-owned with api
  Task 15): required to lift Deviation 10 (TID retired forever,
  ThreadObject.cpp:243) and the 2^15 spawn-count RangeError ceiling
  (api :20). Part of the ungil milestone.
- **Task 14 (per-thread structure splitting) STAYS DEFERRED** unless the
  bench gate forces it: the decision rule is already frozen — "promotion
  DECIDED PRE-INT on jit Task-13's GIL-stub construction bench"
  (SPEC-objectmodel.md:359). Until then concurrent prop adds on shared
  shapes remain cell-locked + structure-table-locked (OM 8h/L6/I37, api §2).
  SPEC-ungil must record the bench verdict, not redesign 8h.

### E. Per-thread event loop — **CONFIRMED GAP. MANDATED SHAPE (THREAD.md:98 "each thread gets its own runloop"; this section records the mandate, design in SPEC-ungil)**
No spec designs it: api 4.6.1/5.5/I12 name the post-GIL *rules* (own queue
till empty; settle on registering thread, dead=>main; inbox fields landed
inert) and vmstate §6.6 reserves the per-lite microtask-queue slot, but
nothing specifies the task (macrotask) queue, the drain loop, or thread
lifetime. Mandated shape to design against:
- Every Thread owns BOTH an independent microtask queue AND an independent
  task (macrotask) queue.
- Lifecycle: run fn -> drain own microtasks -> service own task queue
  (settled async tickets, condition/waitAsync wakeups, cross-thread promise
  reactions), draining microtasks after each task.
- Thread completes ONLY when: fn has returned AND both queues are empty AND
  a pending-registration **keepalive count** is zero. Keepalive = number of
  outstanding registrations that can still enqueue to this thread:
  asyncWait, asyncHold, waitAsync, inbox-armed promises. **join settles
  then, not at fn-return.**
- Cross-thread settlement = enqueue to the REGISTERING thread's task queue
  + wake it (park/unpark on the inbox); dead-thread fallback to main
  (matches the api :200 inboxOpen protocol).
- The keepalive accounting must be specified EXACTLY in SPEC-ungil
  (increment at registration, decrement at settle OR cancel OR
  inbox-close-residue-to-main; it decides thread lifetime and is the
  easiest place to leak a thread or hang a join).
- **Semantic delta vs phase-1 stub**: today join settles at fn-return
  (ThreadObject.cpp:218-231 runs immediately after the single completion
  drain at :208; "Never waits for tickets (4.6.1)"). Post-GIL a thread with
  a live asyncHold/waitAsync registration stays alive past fn-return.
  Corpus tests that must change: lifecycle/join-semantics.js (join timing),
  every test that assumes the completion drain happens exactly once on the
  shared queue (api 4.6.1 GPO), condition-notify-all* / wait-notify-storm
  (D2, item C), and any test relying on I12's "settling thread unspecified"
  (now: registering thread). Exact list to be enumerated by the SPEC-ungil
  corpus audit.

### F. Post-GIL API-lock contract — **CONFIRMED GAP. MANDATED SHAPE**
No spec clause designs JSLock's GIL-off behavior; D1 leaves DropAllLocks
coexistence explicitly open. Mandated shape:
- JSLock learns a GIL-off mode. Spawned threads' JSLockHolder (T1) degrades
  to a per-thread "entered the VM" token + per-thread GCClient heap access
  — near-no-op, no global mutex.
- `currentThreadIsHoldingAPILock()`-style asserts are REINTERPRETED as the
  token (entered-thread check), **never deleted** (they guard host-call
  context everywhere: L6 consumers, ~AsyncTicket's D5 assert,
  stopTheWorldAndRun's R1 contract at JSThreadsSafepoint.cpp:225).
- Embedder/main thread KEEPS real lock semantics (Bun is a non-thread
  client; multi-embedder-thread mutual exclusion on the main carrier stays).
- DropAllLocks coexistence rule must be ruled (INTEGRATE-api D1's open
  rev-15 question): with the embedder on a real lock and spawned threads on
  tokens, DAL scopes only the embedder side.
- Strong-handle discipline under N entered threads: HandleSet writes
  currently API-lock-serialized (ThreadManager.cpp:50-53; api 5.10);
  SPEC-ungil must pick per-thread HandleSets or a locked shared set.

### G. Per-thread blocking policy — **CONFIRMED GAP**
Replaces the per-VM G11 `isAtomicsWaitAllowedOnCurrentThread` gate
(AtomicsObject.cpp:530, P4) and the GPO 4.5-1a spawn-thread TA gate
(P2, lifted by item C). Post-GIL the question "may this thread block
synchronously?" is per-THREAD (main/embedder: embedder policy; spawned
Threads: allowed), and must also govern join()/lock.hold()/cond.wait() on
the main thread (today implicitly allowed via D4-style GIL drops). No spec
owns it.

### H. SymbolRegistry / Symbol.for — **CONFIRMED GAP (vmstate Dev 8, UNOWNED)**
`SymbolRegistry.cpp:58` mutation is JSLock-serialized today; one shared VM
post-GIL needs a concurrent or locked registry (and teardown `:40-43`
ordering vs dying threads). vmstate §2 declares it a non-goal of that WS.

### I. Wasm on spawned threads — **CONFIRMED GAP (extension; no spec mentions it)**
No gate exists (X5). Recommendation to carry into SPEC-ungil: **refuse in
v1** — instantiating/calling WebAssembly entry points from a spawned Thread
throws TypeError; document the restriction. Rationale: per-VM wasm
machinery (memory/table registries, signal-based bounds handling, OMG/BBQ
tier-up state) was never audited under the N-threads-one-VM model and is
outside every charter. Lift requires its own charter.

### J. GIL-machinery end state — **CONFIRMED GAP (disposition table; no spec owns deletion)**
- `useThreadGIL` (OptionsList.h:696): KEPT as a supported fallback mode
  (GIL-on remains the debugging/bisection oracle); default flips to false
  at the ungil milestone gate.
- `GILDroppedSection` (K2): GIL-on path kept; GIL-off path compiles to the
  heap-access/safepoint cooperation only.
- `GILParkSavedExecutionState` (K1) + `resetForFreshThread` (T2): dead once
  Phase B lands; kept compiled for the GIL-on fallback, deleted when the
  fallback is retired.
- `jsThreadGILHandoffYield` (K3) and D2's notify-yield: GIL-on only.
- `unlockAllForThreadParking` (L4): GIL-on only.
- JSLock.cpp:151 backstop (L1): REPLACED, not deleted — the GIL-off branch
  asserts the Phase-B invariant instead (current thread has a registered
  per-thread carrier with a unique TID; never two installs of tid 0), per
  the cross-WS item-13 note at JSLock.cpp:146-148.
- Stub deletions already self-scheduled: W2/W3 (M4), W4 (M7 note), OM stub
  witness (JSThreadsSafepoint.cpp:40-52, "Deleted at M4").

---

## Part IV — Summary classification table

| Item | Classification | Binding citation |
|------|---------------|------------------|
| A (Phase B exec-state + thread-granular STW) | CHARTERED, undesigned | vmstate Dev 10 / SPEC-vmstate.md:42-48; api §2 (:26); jit R1 freeze (:233) |
| B (per-thread GCClient) | CHARTERED (model normative, wiring chartered) | heap Dev 8 (SPEC-heap.md:27), Dev 7 (:26), §3.8 |
| C (Dev 12 / OM 8g re-freeze) | CHARTERED | api :22, :26, :225; OM 8g; INTEGRATE-api D1/D2/D4/D8/D12 |
| D (OM Task 13; Task 14 deferred) | CHARTERED; 14 stays deferred unless bench forces | SPEC-objectmodel.md:377-378, :359 |
| E (per-thread event loop + keepalive) | GAP, mandated shape | THREAD.md:98; api 4.6.1/:100, 5.5/:200, I12/:306 (rules only) |
| F (GIL-off JSLock contract) | GAP, mandated shape | INTEGRATE-api D1 (open question) |
| G (per-thread blocking policy) | GAP | AtomicsObject.cpp:530; api 4.5-1a GPO |
| H (SymbolRegistry) | GAP (UNOWNED) | SPEC-vmstate.md:40, :57 (Dev 8) |
| I (Wasm on spawned threads) | GAP (recommend refuse-v1) | no spec coverage |
| J (GIL machinery end state) | GAP (disposition) | JSLock.cpp:151 note; OptionsList.h:696 |

Already DESIGNED-FOR (no SPEC-ungil work beyond consuming items A-D): OM
TID/SW dispatch + segmented butterflies + M5/M7/I33 + §9.5 contract; jit
TTL/Class-A/epoch/IC protocols (pending only the real stop); heap per-client
access state + RCAC + shared-server stop barrier (Dev 5/§10); api F1/F5,
5.3/5.6 native protocols, I10/I12 invariants, D9 poll predicate, 5.9 rank
table, inbox fields.

Byte budget note: this file is an inventory; SPEC-ungil.md (next phase)
carries the design, SPEC-ungil-history.md the change log.
