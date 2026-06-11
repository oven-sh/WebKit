# SPEC-heap-history.md — review-resolution log for SPEC-heap.md

**ENTIRELY NON-NORMATIVE as of rev 8** (round-5 finding: the rev-7
"normative-by-reference" appendix mechanism evaded the spec's 40000-byte cap).
SPEC-heap.md rev 8 is self-sufficient and alone binding; everything here is
audit trail, rationale, archived prose from earlier revs, and test sketches.
Where an appendix below disagrees with SPEC-heap.md rev 8, rev 8 wins.

---
## 15. Review notes — round-1 findings: dispositions & refutations (retained from rev 2)

Accepted and resolved in rev 2 (summary): entered-VMs-only stop count and
blocked-in-native deadlock → §10A access protocol (round 2 showed rev 2's fix was
itself incomplete; superseded by rev 3's §10); missing Riptide bridge → §10B +
deviation 4; I5 vs running-world bitvector resize → I5b; HashMap TLC layout vs JIT
offset → flat-array layout (§5.3); epoch vs compiler threads → I11 suspension
precondition (round 2 corrected the *placement*; §10 step 7); unimplementable
per-mutator park hooks → rev 2 claimed none were needed, rev 3 reverses this with
evidence (§2.6, §13 entry 5); JIT-baked `LocalAllocator*` in shared code → §5.5
null-allocator gating; unreachable `isSharedServer()` → §12.1 harness; racy protocol
switch → I13/I15 + §10B.4; orphaned cross-spec architecture → deviation 5; structure-
lock STW deadlock → rank 7a + L5 + I14; GIL stub vs owning-thread asserts →
access-based I2/I4.

Still-valid round-1 refutations (so later rounds don't re-trip):

- "Bitvector resizing happens without the bitvector lock" — false; the resize was
  always under `m_bitvectorLock` (`BlockDirectory.cpp:141,148-153`); the real hazard
  was the single-mutator lock-free reader privilege, governed by I5b.
- "LLInt caches allocators per code site (asm fast path)" — false in this tree; LLInt
  has no assembly inline-allocation path (sole "Allocator" hit is
  `ExecutableAllocatorConfig`, `LowLevelInterpreter.asm:464`); the real shared
  artifact is `ObjectAllocationProfile::m_allocator`
  (`bytecode/ObjectAllocationProfile.h:75`), covered by §5.5.
- "Emitters cannot be gated without editing jit/**" — false; every emitter already
  branches to the slow path on a null `Allocator`
  (`AssemblyHelpers.cpp:1088-1106`, `FTLLowerDFGToB3.cpp:22452-22477`), and allocator
  values originate from heap-owned `CompleteSubspace` state (§5.5).
- "Idle VMs need a GC dispatchStopHandler path" — still false under rev 3, a fortiori:
  idle VMs hold no heap access, and the §10 barrier is access-based.

---

## 16. Review notes — round-2 findings: dispositions

Every round-2 blocker/major, with disposition and where it landed:

1. **"VMManager conductor-selection barrier never completes / no conductor with zero
   entered VMs" (blocker, filed three times in overlapping forms).** ACCEPTED —
   verified against `VMManager.cpp:223-276,260-272,345-489` (§2.6 documents the
   negative result). Rev 2's design is replaced wholesale: requester-conducts (§10),
   access-based barrier (§10 step 4, F8), park hooks + keep-parked condition +
   reason-latch exclusion in VMManager via manifest (§13 entry 5; the "no other
   VMManager change" claim is retracted there). Regression scenarios:
   `blockedInNativeVsGC`, `syncRequesterStorm`, `noEnteredVMsGC` (§12.1).
2. **"Steal precondition asserted as invariant fires on a legal race" (major).**
   ACCEPTED — verified at `Heap.cpp:2343-2365` (§2.5 note): granted-unserved tickets
   are legal at request time. §10B.1 now: `exchangeOr` idempotent conn-bit, assert
   only `!m_collectorThreadIsRunning`, conductor drains until
   `m_lastServedTicket == m_lastGrantedTicket`.
3. **"I11 precondition unsatisfiable at the specified call site —
   resumeThePeriphery runs before the cycle returns" (major).** ACCEPTED — verified
   (`Heap.cpp:1965` clears the flag; ticket served at `:1807`; §2.5). Fixed by the
   conductor's dedicated post-cycle `suspendCompilerThreads()` window, frozen as §10
   step 7 / §10B.6 / I11; T7 adds a regression test.
4. **"Non-conductor collectSyncAllClients caller has no path into the stop; literal
   implementation deadlocks" (major).** ACCEPTED — subsumed by the §10 redesign: the
   synchronous caller is the conductor or a follower that releases access before
   waiting (§10 step 2); no caller ever needs `notifyVMStop`.
5. **"Precise allocation path completely unsynchronized and unmentioned" (blocker).**
   ACCEPTED — verified at `CompleteSubspace.cpp:115-146,148-202`,
   `MarkedSpace.cpp:231-241`. New §5.6, I16, T3b, `preciseAllocationStorm`,
   `MarkedSpace::m_capacity` atomicity (§5.4.1).
6. **"acquireHeapAccess vs barrier race unspecified/unsound; NoAccessBlockedForGC
   declared never used" (blocker).** ACCEPTED — F8 now specifies storage
   (`m_gcStopPending`), writers, the seq_cst Dekker argument, and the mandatory
   revert-before-block; the unused third enum value is deleted (§10A).
7. **"Stale vmstate erratum; live rank-10-before-JSLock contradiction" (major).**
   PARTIALLY ACCEPTED / PARTIALLY REFUTED with evidence: the finding is right that
   rev 2's erratum target was stale (vmstate fixed 30/40 → rank 25,
   `SPEC-vmstate.md:282-284`); but vmstate's round-2 disposition *already* defers to
   this spec's §6 as master and records the inverted JSLock/VMManager rows as deleted
   (`SPEC-vmstate.md:364-372`) — the contradiction survives only as residual
   un-edited table text at `SPEC-vmstate.md:1159-1172`. §6 preamble + §13 entry 9
   re-target the erratum at exactly that residue and give the interim reading rule.
8. **"didAllocate's performIncrement path raced by N clients; eden-callback deferral
   mechanism missing" (major).** ACCEPTED — verified (`Heap.cpp:2642,3324-3351`,
   `Heap.h:871`). §5.4.2 gates `performIncrement` off in shared mode (with the
   deviation-4 consistency argument and a mutator-SlotVisitor assert); §5.4.3
   replaces callback-driven collection with mutator-driven triggering, removing the
   unspecified deferral.
9. **"Harness cannot be implemented: GCClient::Heap::vm() UB on stack clients;
   allocate requires VM&" (major).** ACCEPTED — verified (`HeapInlines.h:239-242`,
   `CompleteSubspace.cpp:115-120`). §12.1 adds `markStandalone()` + `vm()` poison
   assert, the `allocateForClient` seam with the enumerated skipped VM-coupled steps,
   and extends T9 to the client-side `vm()`.
10. **"bumpAndReclaim contradicts SPEC-jit R4/CS4" (blocker).** ACCEPTED — CS4 is
    explicitly REFUSED with the three-part rationale in §11; §9's frozen comment now
    states conductor-only; cross-spec CR filed (§13 entry 10a).
11. **"releaseQuarantinedSlots has no caller in SPEC-heap" (major).** ACCEPTED —
    verified (`SPEC-objectmodel.md:772,780,889,1091`). §9 adds
    `addStopTheWorldSafepointHook`; §10 step 7 invokes hooks; legacy-mode invocation
    point specified (§9); CR to objectmodel for the adapter (§13 entry 10d).
12. **"§5.3 JIT addressing contract has no consumer — dead frozen surface" (major).**
    ACCEPTED — SPEC-jit scopes per-thread allocators out (§2.9). §5.3 carries an
    explicit status note splitting frozen (layout, used by our own C++) from
    provisional (JIT offsets); deviation 6 states the slow-path-only consequence
    against THREAD.md's perf goal; CR filed (§13 entry 10c).
13. **"Multi-stop-reason arbitration unspecified; M4 edits the same VMManager hunks;
    JSThreads stops bypass the access barrier" (major).** ACCEPTED — §10C defines the
    phase-1 boundary: GC never latches into the reason machinery; debugger overlap is
    documented-unsupported (stall-not-corrupt analysis given); JSThreads stops
    serialize via `m_gcConductorLock` (answer to CS2, replacing R1.h's overlap);
    same-file coordination note in §13 entry 5; CR filed (§13 entry 10b).

## 17. Review notes — round-3 findings (rev 6): dispositions

Every round-3 blocker/major, with disposition and where it landed in SPEC-heap.md rev 6:

1. **"GC stop racing any other VMManager stop reason leaves the world permanently
   parked" (major) + "§10C 'not corrupting' unproven for RunOne" (major).** ACCEPTED
   (in part) — verified at `VMManager.cpp:305-316` (`requestResumeAllInternal` clears
   the bit then early-returns when `hasPendingStopRequests()`, with NO notify) and
   `:319-330` (`resumeTheWorld` early-returns when `m_useRunOneMode`, also without
   notify). With the §13.5b keep-parked clause, VMs parked solely by the GC bit wait
   in `m_worldConditionVariable.wait` (`:432-433`) and nothing would ever wake them:
   the second requester's `requestStopAllInternal` early-returned at `:231-233`
   because `m_worldMode >= Stopping`. Additionally the `m_currentStopReason` latch
   sits BEFORE the wait loop (`:404-407` vs `:432-433`), so even a woken VM would
   break out without latching the pending non-GC reason (the reason would be dropped
   and the world resumed with its bit still set). Fix: new manifest entries
   **§13.5e** (when clearing the GC bit, ALWAYS notifyAll under `m_worldLock`, even
   when other bits remain pending or RunOne suppresses `resumeTheWorld`) and
   **§13.5f** (move the latch inside the wait loop: re-fetch when `None` after every
   wake). Full shape analysis now in §10C(a)-(d); regression
   `debuggerStopDuringSharedGC` added (§12.1, §10 ordering note, §14 done-criterion
   "four §10 deadlock regressions").
   **REFUTED sub-claim:** "GC during RunOne dissolves the debugger session because
   `requestResumeAll` calls `resumeTheWorld()` which sets `Mode::RunAll`" — false:
   `resumeTheWorld` early-returns when `m_useRunOneMode` is set
   (`VMManager.cpp:328-330`), so `m_targetVM`/RunOne survive; the only real defect in
   that shape was the missing wakeup, fixed by §13.5e. The targetVM, once woken,
   passes `shouldStop` (condition 1: it IS the target) and resumes its session;
   non-target VMs remain parked.
   Shape (d) analysis (GC requested while `m_worldMode >= Stopping`): the early
   return at `:231-233` installs no new traps, but the in-flight request's
   `requestStopAllInternal` already called `vm.requestStop()` on every entered VM,
   and `notifyVMConstruction`/entry services trap newcomers while
   `m_worldMode != RunAll`; the heap-side access barrier therefore still completes
   once those VMs park (or stalls in the disclosed, watchdog-visible shape (a) if
   the debugger holds a VM in JS with heap access).

2. **"Conductor-election follower has no specified wakeup" (blocker + major,
   filed twice).** ACCEPTED — the rev-5 pseudocode's "wait until (T served ||
   conductor inactive)" named no mechanism: tickets granted after the conductor's
   final drain check (legal under `m_threadLock`) would strand a follower forever;
   the existing serve path wakes via `mutatorWaitingBit` + `ParkingLot::unparkAll`
   (`Heap.cpp:1805-1810`), not a `m_threadLock` condition, and `m_threadCondition`
   is mutator-wait-forbidden (`Heap.h:1018`). Fix (§10.2 + §5.1): new
   `bool m_gcConductorActive` + `Condition m_gcElectionCondition`, both guarded by
   `*m_threadLock` (rank 5). Conductor sets the flag under `m_threadLock` after
   winning `tryLock(GCL)`, and after releasing GCL clears it + notifyAll; the serve
   path also notifyAlls the election condition; the follower waits on the election
   condition with predicate `(m_lastServedTicket >= T || !m_gcConductorActive)`.
   Lock-order note made explicit: the GCL `tryLock` is attempted holding NO lock
   (the served check releases `m_threadLock` first), so there is no rank-5→2
   inversion. `syncRequesterStorm` now explicitly covers the late-granted-ticket
   interleaving.

3. **"No current-thread→GCClient::Heap mapping, but frozen server-side signatures
   depend on one" (major).** ACCEPTED — fixed without changing the frozen CSAC/RCAC/
   SINFAC signatures: new normative §10A.1 specifies one
   `WTF::ThreadSpecific<GCClient::Heap*>` slot (defined in `GCThreadLocalCache.cpp`),
   set by `attachCurrentThread`, cleared by `detachCurrentThread`, re-stamped by
   `JSLock::didAcquireLock`'s shared-mode forwarding before AHA, NOT cleared by
   `releaseHeapAccess` (re-acquire after a blocking call needs identity). §9 gains
   `GCClient::Heap::currentThreadClient()` and
   `JSC::Heap::currentThreadIsAllocatorOwner(const LocalAllocator*)` (resolution:
   current client non-null and `la` present in its TLC's `m_perDirectory`; debug
   cross-check against `m_accessOwner`). No LA→client back-pointer; the frozen §5.3
   layout is unchanged. T2 carries the implementation.

4. **"GCL bracketing API required by SPEC-jit CS2 absent from frozen §9" (blocker).**
   ACCEPTED — §9 now exports `class JSC::Heap::JSThreadsStopScope` (RAII over
   `m_gcConductorLock`, rank 2; preconditions: caller has released its heap access —
   preserves jit R1.g — and never calls `bumpAndReclaim` inside, per I11/§13.10b;
   no-op when `!isSharedServer()`). §10C and §13 entry 10b now read
   RESOLVED-AS-PROVIDED; T5 builds it. SPEC-jit R1.i's "until exported,
   RELEASE_ASSERT" clause is thereby dischargeable at integration.

5. **"Cross-spec line citations rotted in both directions; §13.9 erratum
   unexecutable" (major, filed twice).** ACCEPTED — all verified: SPEC-objectmodel
   is 461 lines (`releaseQuarantinedSlots` at :221/:386, i.e. §9.4 — the contract
   itself, `releaseQuarantinedSlots(uint64_t currentEpoch)` with the adapter passing
   `safepointEpoch().current()`, still matches: citation rot only, noted in §1.8 as
   "contract unchanged"); SPEC-vmstate is 682 lines, the deferral table is its §7
   (:565-580) and already defers to heap §6 (the old `:1159-1172` rows are gone, and
   vmstate §7 itself flags heap's stale cites). Fixes: §1.8 → "SPEC-objectmodel
   §9.4"; §6 → "SPEC-vmstate §7"; §13 entry 9 rewritten: erratum RETIRED, no
   integrator action; new §13 entry 10e: cite-refresh CR — siblings re-anchor their
   `SPEC-heap.md:<line>` cites to section anchors at integration. Mapping for the
   dangling inbound cites (SPEC-heap rev 6 targets): jit `:199` → §9 (frozen
   interface), `:342` and `:420b` → §10C + §13.10b (GCL serialization / CS2),
   `:380-415` and `:415` → §13.5 (NVS park hooks; M4-before-heap-hook resume-tail
   order); api `:314-315` → §10C, `:872-874` → §11/§9 contract notes. SPEC-heap
   itself now cites siblings by section anchor only.

6. **"§12 owned-file list incomplete relative to T3b/T8" (major).** ACCEPTED — §12
   re-titled "MINIMUM set, not exhaustive" with an explicit audit rule (any
   `Source/JavaScriptCore/heap/**` file may be modified where T3b/T8/T9 require) and
   the expected candidates listed (`IncrementalSweeper.cpp`, `MarkedBlock.h/.cpp`,
   `MarkedBlockInlines.h`, `PreciseAllocation.h/.cpp`, `HeapVerifier.cpp`).

7. **"Ownership overlap: SPEC-vmstate M9 edits heap/StructureAlignedMemoryAllocator.cpp"
   (major).** ACCEPTED — verified (`SPEC-vmstate.md:9,18,238,634,646`: M9 ships the
   §5.1/N3 mimalloc thread-affine structure-handout ctor hunk into that file and the
   integrator checklist carries "M9 rebase"). §12 now carves the file out of this
   workstream's writes: the heap implementer does not touch it; the integrator
   applies M9 per vmstate's manifest, mirroring the §13 entry-5 VMManager
   coordination.

Archived rev-5 §2 pivotal-facts bullets (digested out of the spec in rev 6; the
long-form ground truth remains Appendix A below):

- 2.2 FIXMEs `LocalAllocator.cpp:116` (APILock assert), `:138` (handout sync; guards
  `:140-156`), `:170-183` (bitvector-cursor + steal races; proposes "a single
  per-Heap lock" — adopted §5.2), `:249-251` (unlocked `setIsEden`). Find/steal/add
  already under BVL (`BlockDirectory.cpp:100-173`; `inUse` = single-allocator
  handout); the rest of the slow path is NOT. `addBlock` reallocates `m_bits` under
  BVL mid-run (`:141,148-153`); the mutator reads bitvectors lock-free
  (`BlockDirectory.h:174-176`) — I5b basis.
- 2.3 Split is 1:1 today (GCH `heap/Heap.h:1268-1323`, ctor takes any server
  `:1273`); iso already per-client (`IsoSubspace.h:79-100`); non-iso allocators
  server-side (`CompleteSubspace.h:49,62-64`); every non-iso entry needs a `VM&`
  (`:115-120`, .cpp) ⇒ §12.1 seam; both `vm()`s are `OBJECT_OFFSETOF` arithmetic
  (`HeapInlines.h:40-43,239-242`; client one garbage off-VM).
- 2.8 Every JIT emitter already slow-paths on a null allocator
  (`AssemblyHelpers.cpp:964-967,1088-1106`; `FTLLowerDFGToB3.cpp:22452-22477`);
  tiers bake `Allocator` constants (`JITAllocator.h:44-49`);
  `ObjectAllocationProfile::m_allocator` thread-shared; LLInt has no asm
  inline-alloc path (`LowLevelInterpreter.asm:464`); Baseline shared/unlinked
  (`OptionsList.h:640`) ⇒ data-driven gating (§5.5).
- 2.9 Cell lock `IndexingType.h:53,97-98,230`/`JSCell.h:297`; `useHandlerICInFTL`
  off (`OptionsList.h:638`); `AtomStringTableLocker` real only under
  `USE(WEB_THREAD)` (`AtomStringImpl.cpp:42-63`); libpas TLC
  `pas_thread_local_cache.h:79-81,188,225,265`; N-thread conservative scan
  primitives `MachineStackMarker.h:42-65`; GIL stub = shared `JSLock`
  (SPEC-api §5.2); SPEC-jit M4 edits VMM (§13.5); R4/CS4 refused (§11).

---

## 18. Review notes — round-4 findings (rev 7): dispositions

All six round-4 blocker/major findings verified against the tree; all six are REAL
(findings 1 and 3 describe the same defect). Dispositions and where each landed in
SPEC-heap.md rev 7:

1+3. **Park/resume hook contract internally contradictory; AHA undefined when the
   caller already holds access (blocker + major).** ACCEPTED. Rev-6 §13.5a's frozen
   text ("no-op unless ISS and (willPark) stop pending") admitted two readings and
   both were unsound: Reading A (gate applies to both hooks) makes `didResume`
   always no-op because §10.8 clears GSP strictly before §10.9's
   `requestResumeAll`, so a VM whose `willPark` released access exits NVS running
   JS with NO access — violating §10A's core rule and silently breaking the next
   conductor's step-4 barrier. Reading B (gate is willPark-only; didResume
   unconditional when ISS) makes `didResume` call AHA on a client already
   HasAccess after a debugger-only stop (§10C(a): willPark no-op'd because GSP was
   false at park time); rev-6 F8 defined AHA only as a CAS-NoAccess→HasAccess
   retry loop, which can never succeed against an already-HasAccess state — spin
   forever or assert. Fix (rev 7): (a) per-client `bool m_releasedByGCPark`
   (GCClient::Heap member, written only by the owning thread inside NVS):
   `willPark` sets it iff it actually released (ISS ∧ GSP ∧ access held);
   `didResume` re-acquires iff the flag is set, then clears it, blocking via the
   normal F8 path if a NEW stop is pending; both hooks idempotent. (b) F8 gains
   step (0): AHA when the calling client is already HasAccess with
   `m_accessOwner == ` current thread debug-asserts and returns (idempotent) —
   never CAS-spins; needed independently for JSLock recursion and ACT. §10.9's
   "re-acquires each VM's client's access" reworded to "iff `m_releasedByGCPark`".

2. **Wasm GC-object allocation violates §5.5's never-populate rule (major).**
   ACCEPTED. Verified: `JSWebAssemblyInstance.cpp:135-141` calls
   `subspace->prepareAllAllocators()` (`CompleteSubspace.cpp:204-211` —
   force-populates every `m_allocatorForSizeStep` slot) then memcpys the
   server-side LA* array into per-instance data consumed by BBQ/OMG inline
   GC-array/struct allocation (`WasmBBQJIT64.cpp:1453,1992`;
   `WasmOMGIRGenerator.cpp:3683+`). Under the option this would (a) fire the §5.5
   attach-time RELEASE_ASSERT if a wasm-GC module is instantiated before client
   #2, or (b) hand one shared FreeList to N threads if after. Reviewer's
   option (a) — "verify nulls are benign" — is FALSE: those emitters use
   `JITAllocator::variableNonNull` and `emitAllocateWithNonNullAllocator`
   (`AssemblyHelpers.cpp:962`) dereferences the LA* with NO null check (loads
   FreeList fields at a near-null address ⇒ fault, not slow path). So rev 7 takes
   option (b): shared mode + wasm-GC objects explicitly unsupported phase 1.
   Owned backstop: `prepareAllAllocators()` RELEASE_ASSERTs
   `!Options::useSharedGCHeap()` (sole caller is that ctor — verified by grep).
   New manifest entry 11: option-gated rejection in the
   `hasGCObjectTypes()` ctor block before `prepareAllAllocators()`. §5.5 and the
   attach assert stand unweakened; Appendix A §2.8's "every emitter slow-paths on
   null" claim is hereby scoped to JS tiers only (wasm is the exception, handled
   by manifest 11).

4. **§10C(a)/(c) were permanent deadlocks, not "stalls until debugger resume"
   (major).** ACCEPTED. Verified: the rev-6 hooks fire exactly once at NVS
   entry/exit; a VM parked for a debugger reason before GSP was set holds access
   with no release point inside the wait loop (`VMManager.cpp:432-433`), and on
   debugger resume it re-parks via §13.5b still holding access ⇒ the step-4
   barrier never completes, the conductor holds GCL forever, every mutator
   eventually blocks in AHA. Also `requestStopAllInternal` early-returns at
   `:231-233` when `m_worldMode >= Stopping` without trapping or notifying, so
   nothing even wakes the parked VMs to re-evaluate. Fix (rev 7, the reviewer's
   "5-line" option, made mechanical): new manifest **5g** — (i) inside the NVS
   wait loop, when the GC bit is pending, before each wait the VM drops
   `m_worldLock`, calls `gcWillParkInStopTheWorld` (idempotent; releases access +
   sets the flag only if access held; L6 respected — no VMM lock held), re-takes
   the lock and re-evaluates; (ii) `requestStopAllInternal`, `reason == GC`,
   `m_worldMode >= Stopping`: before the early return, `vm.requestStop()` on
   entered VMs (traps a RunOne targetVM) + `m_worldConditionVariable.notifyAll()`
   under the already-held `m_worldLock`. §10C(a)/(c)/(d) rewritten to the now-true
   recovery behavior; new scenario `gcDuringDebuggerPark` asserts it. Appendix D's
   §10C "unsupported phase 1" framing and rev-6 §10C(a)'s "stalls until debugger
   resume" are BOTH superseded by rev-7 §10C (main file wins on conflict).

5. **GC election vs `JSThreadsStopScope` mutual deadlock (major).** ACCEPTED.
   Verified: when GCL is held by a JSThreads stop (not a GC conductor),
   `m_gcConductorActive` is false, so a §10.2 requester busy-loops
   tryLock/AHA/RHA without ever parking; the JSThreads requester waits for all
   entered VMs to park in NVS (`m_numberOfStoppedVMs` counting — releasing heap
   access is irrelevant to it) ⇒ mutual deadlock whenever useSharedGCHeap + jit
   M4 multi-mutator stops compose. Fix (rev 7): §10.2 GCL-busy rule — when
   `tryLock` fails and no GC conductor is active, the requester (access released)
   waits TIMED (≤1ms) on `m_gcElectionCondition` and, if VM-backed, polls VMTraps
   each iteration before retrying — the poll parks it in NVS for the pending
   JSThreads stop, the scope holder completes, releases GCL, and the requester's
   next `tryLock` wins. Timed wait (not scope-dtor notification) because the
   JSThreads trap request can land after a poll and before an untimed wait —
   no notification edge exists that closes that race. New §12.1 scenario
   `jsThreadsStopVsGCRequester`; §10C(e) records the disposition; manifest 10b
   notes it for SPEC-jit G13.

6. **"Hunks disjoint except the resume tail" false for VMManager.cpp (major).**
   ACCEPTED. Verified: jit R1.c (SPEC-jit.md:234) edits the `:413-460`
   arbitration region, which textually contains heap 5b's `shouldStop()` hunk
   (`:413-430`) and the wait loop heap 5f restructures (`:404-407` latch moved
   into `:432-433`); jit M4 (SPEC-jit.md:257,260) repeats the stale disjointness
   claim. Fix (rev 7): manifest entry 5's coordination paragraph replaced with a
   normative merge order — heap 5b/5c/5f/5g(i) apply first; jit R1.c is then
   expressed as a requirement against the post-heap shape (conductor-pin runs
   where `m_currentStopReason` has just latched JSThreads — inside the wait loop
   after 5f — and all active VMs are stopped; heap 5b's GC-bit condition stays
   FIRST in `shouldStop()`); resume tail keeps M4-fence-then-didResume-hook; the
   integrator runs the combined diff past both specs and treats jit M4's
   "disjoint" sentence as superseded.

---

# Appendix A — rev 3/4 long-form ground truth (archived from SPEC-heap.md rev 4 compression)

## 2. Ground truth (verified at the cited lines; more cites inline in §5/§10/§13)

### 2.1 Split exists, 1:1
`GCClient::Heap` `heap/Heap.h:1268-1323` (`m_server` `:1284`; ctor takes any server
heap `:1273`); `VM` embeds both by value (`VM.h:468-469`; `clientHeap(heap)`
`VM.cpp:260`); `GCClient::Heap::vm()` = `OBJECT_OFFSETOF(VM, clientHeap)` arithmetic
(`HeapInlines.h:239-242`) — garbage for non-VM-embedded clients (§12.1 poisons);
iso is already per-client (`IsoSubspace.h:79-100`); WebCore shares server heap data
under `useGlobalGC` (`WebCoreJSClientData.cpp:111-117`; `OptionsList.h:429`).

### 2.2 The FIXMEs (`heap/LocalAllocator.cpp`)
`:116` single-mutator APILock assert; `:138` "FIXME GlobalGC: Need to synchronize
here to when allocating from the BlockDirectory in the server" (guards `:140-156`);
`:170-183` FIXME naming (a) N allocators cursor-searching one directory's bitvectors
and (b) the steal path (`Subspace.cpp:80-83`), proposing "a single per-Heap lock"
(adopted, §5.2); `:249` FIXME on the unlocked `setIsEden` `:250-251`. Already under
`m_bitvectorLock`: `findBlockForAllocation` (`BlockDirectory.cpp:111-126`),
`findEmptyBlockToSteal` (`:100-109`), `addBlock` (`:139-173`; `inUse` bit hands a
block to exactly one allocator). NOT synchronized: eden-bit/`didAllocateInBlock`;
accounting + `performIncrement`; `m_lowerTierPreciseFreeList` (`IsoSubspace.h:72`);
`tryCreate`+`didAddBlock` (`BlockDirectory.cpp:128-137`); precise registration
(§5.6). I5b fact: `addBlock` reallocates `m_bits` under `m_bitvectorLock`
(`:141,148-153`) on the running-world path; the single mutator reads bitvectors
lock-free today (`BlockDirectory.h:174-176`).

### 2.3 Non-iso allocation is NOT per-client
`CompleteSubspace` owns `m_allocatorForSizeStep` + `m_localAllocators` server-side
(`CompleteSubspace.h:62-64`); `Allocator` wraps `LocalAllocator*`
(`Allocator.h:38-67`); `offsetOfAllocatorForSizeStep()` (`CompleteSubspace.h:49`);
`tryAllocateSlow(VM&,...)` runs `vm.verifyCanGC()` + `sanitizeStackForVM(vm)`
(`CompleteSubspace.cpp:115-120`) — every non-iso entry needs a `VM&` (§12.1 seam).

### 2.4 Shapes
`LocalAllocator` fields `LocalAllocator.h:71-80`; registers under
`m_localAllocatorsLock` (`LocalAllocator.cpp:39-45`); per-directory allocator list
exists (`BlockDirectory.h:196`); `stopAllocatingForGood` iterates all
(`BlockDirectory.cpp:237-249`); `FreeList` scrambled/interval-based, JIT offsets
exported (`FreeList.h:82-120`), single-owner (I2);
`assertIsMutatorOrMutatorIsStopped` (`BlockDirectory.h:93-99`, `.cpp:516`).

### 2.5 Server Heap hard-wired to one VM; collection = two-party conn protocol
`Heap::vm()` = `OBJECT_OFFSETOF(VM, heap)` (`HeapInlines.h:40-43`), used widely
(`Heap.cpp` 546, 634, 750, 906-910, 935, 1130, 1395); world-state bits
`Heap.h:985-989`. `requestCollection` (`Heap.cpp:2343`) steals the conn only when
`m_lastServedTicket == m_lastGrantedTicket && !m_collectorThreadIsRunning`
(`:2355-2358`) — granted-unserved tickets **legal** at request time (§10B.1); conn
mutator-held ⇒ phases run on the mutator's thread (`:2073-2090`); ticket-wait under
`m_threadLock` (`:2367-2373`); served at `:1807`. `resumeThePeriphery` resumes
compiler threads and clears the flag **before** the ticket is served (`:1965` vs
`:1807`) ⇒ `bumpAndReclaim` needs its own suspension window (§10.7, I11);
`suspendCompilerThreads()` callable (`:2385`). `checkConn` `:1390-1401`;
single-mutator asserts `:2346-2348`. `didAllocate` → `performIncrement` (`:2642`,
`:3324-3351`) RMWs plain double `m_incrementBalance` (`Heap.h:871`) and can drain
the mutator `SlotVisitor` — unsound with N callers (§5.4).

### 2.6 VMManager STW and the negative result
Reasons `VMManager.h:200-211`; `requestStopAll/requestResumeAll` `:279-286`; GC
reason unimplemented (`VMManager.h:154`; `RELEASE_ASSERT_NOT_REACHED`
`VMManager.cpp:462-463`). `requestStopAllInternal` (`:223-276`) is async, traps
every VM (`:254`), counts *active* only entered VMs (`:260-272`); mutators park in
`notifyVMStop` (`:432-433`); `shouldStop()` (`:413-430`) parks while
`stopped != active` (`:429`); the callback runs only inside `notifyVMStop` on the
thread observing `stopped == active` (`:444,456-477`); reason latch
`:405-411`/`:479-490`; nudge WasmDebugger-only (`:249,257-258,282-303`); VMs
constructed during a stop park first (`:532-548`); resume `:305-343`.

**"Conductor = last VM to park" is impossible for GC**: (1) an entered VM blocked
in a host call stays counted active but never runs a traps check ⇒
`stopped == active` never holds; (2) a synchronous requester blocked on its ticket
never parks; (3) zero entered VMs (the §12.1 config) ⇒ no thread inside
`notifyVMStop` to run the callback. **Definition: the *conductor* = the GC
requester that won the heap-side election (§10.2), never VMManager-selected.**
VMManager's GC role: trap + keep entered mutators parked while the GC bit is set.

### 2.7 Conservative scanning supports N registered threads
`MachineThreads::addCurrentThread()`; `tryCopyOtherThreadStacks` suspends+copies
each registered stack (`MachineStackMarker.h:42-65`); `gatherStackRoots` passes one
`m_currentThreadState` today (`Heap.cpp:906-910`). Suspend-and-copy works whatever
the thread executes ⇒ no mutator snapshot hooks needed.

### 2.8 JIT/LLInt inline allocation (drives §5.5)
Tiers bake `Allocator` constants (`JITAllocator.h:44-49`) via
`allocatorForConcurrently<T>()` (`JSCellInlines.h:148-153`); sites
`AssemblyHelpers.h:2078-2079`, `DFGSpeculativeJIT.cpp:435-437,16467,17744`,
`FTLLowerDFGToB3.cpp:11666,18843-18845,22608,22691`; FTL also loads the server
array at runtime (`:22655-22660`). **Every emitter already slow-paths on a null
allocator** (`AssemblyHelpers.cpp:1088-1106`; `FTLLowerDFGToB3.cpp:22452-22477`;
`forceGCSlowPaths` `AssemblyHelpers.cpp:964-967`).
`ObjectAllocationProfile::m_allocator` (`ObjectAllocationProfile.h:41,61,75`) is
thread-shared — also nulled. LLInt has no asm inline-allocation path (only
"Allocator" hit `LowLevelInterpreter.asm:464`). Baseline code is shared/unlinked
(`useBaselineJITCodeSharing`, `OptionsList.h:640`) ⇒ gating must be data-driven.

### 2.9 Adjacent verified facts
2-bit cell lock (`IndexingType.h:53,97-98,230`; `JSCell.h:151-152`; "Always CAS"
`JSCell.h:297`); `useHandlerICInFTL` default false (`OptionsList.h:638`);
`AtomStringTableLocker` real lock only under `USE(WEB_THREAD)`
(`AtomStringImpl.cpp:42-63`); libpas TLC flat slot array/global index/bound
(`pas_thread_local_cache.h:79-81,188,225,265`); `structureAllocationLock`
(SPEC-vmstate §5, rank 25; vmstate defers to this §6, `SPEC-vmstate.md:364-372`);
GIL stub = shared `JSLock` handed between threads (SPEC-api §5.2;
`JSLock.h:40-50,73`); SPEC-jit scopes per-thread allocators out, M4 edits VMManager
(§13.5), R4/CS4 refused (§11).


---

# Appendix B — rev 3/4 long-form stop protocol incl. rationale (archived from SPEC-heap.md rev 4 compression)

## 10. Shared-mode collection: requester-conducted stop protocol

Steps (normative):

1. **Trigger & ticketing.** `collectSyncAllClients`/`requestCollectionAllClients`
   (or `collectIfNecessaryOrDefer` when shared) enqueues a `GCRequest` ticket
   under `m_threadLock` per §10B.1 (subject to L2/I14); caller notes ticket T;
   both run the election loop. No fire-and-forget collection in shared mode.
2. **Conduction election.**
   ```
   while (T not served /* m_lastServedTicket < T under m_threadLock */) {
       if (m_gcConductorLock.tryLock()) {
           if (T already served) { unlock; break; }
           conduct();            // steps 3-9
           m_gcConductorLock.unlock();
       } else {                  // follower
           releaseHeapAccess();  // REQUIRED: else the step-4 barrier waits on us
           wait under m_threadLock until (T served || conductor inactive);
           acquireHeapAccess();  // blocks until resume if a stop is pending (F8)
       }
   }
   ```
   The conductor drains all granted tickets (§10B.1); a follower waking unserved
   loops and conducts. The synchronous caller never waits on a ticket while
   holding access and never needs `notifyVMStop`.
3. **Stop request.** Conductor: (a) seq_cst `m_gcStopPending = true`; (b) releases
   its own client's access (uniform step-4 predicate; conductor operates under
   I2's stopped-world exception from step 5); (c)
   `VMManager::requestStopAll(StopReason::GC)` (async) — traps every entered VM.
   The conductor's own trap bit is harmless (no JS/traps checks until after
   resume, debug-asserted). Idle VMs need no nudge (can't mutate without access;
   `dispatchStopHandler` NOT extended to GC); VMs constructed during the stop
   park first.
4. **Access barrier.** Under `m_gcBarrierLock`, wait on `m_gcBarrierCondition`
   until **every client has `m_accessState == NoAccess`** (seq_cst, F8). Entered
   mutators running JS reach NoAccess via traps → `notifyVMStop` → the §13.5
   `gcWillParkInStopTheWorld` hook (releases access, parks; the §13.5
   `shouldStop()` condition keeps them parked while the GC bit is set,
   independent of VM counts); blocked-in-native mutators already released access
   (§9); non-entered holders release at their next `releaseHeapAccess()` or
   `stopIfNecessaryForAllClients()` poll (every allocation slow path); acquirers
   revert-and-block (F8). Then set `worldIsStoppedForAllClients` (F7); no client
   regains access until step 9.
5. **Conductor-side flush.** For every client:
   `threadLocalCache().stopAllocating()` + iso equivalent (FreeLists flushed into
   blocks — what `stopThePeriphery` achieves today). Legal under I2's exception;
   F2/F8 fence.
6. **Stacks.** `gatherStackRoots` (`Heap.cpp:906-910`) generalized: conductor's
   own `CurrentThreadState` (`MachineStackMarker.h:67-73`) +
   `tryCopyOtherThreadStacks` for every other registered thread (`:60-62`);
   parked/accessless/blocked-in-native all handled (§2.7).
7. **Collection + safepoint work.** Run the full synchronous collection per §10B
   (conducted-as-mutator phase loop, drain all tickets). Then, still stopped —
   the cycle already resumed compiler threads (§2.5) — the ONLY sanctioned
   `bumpAndReclaim` context (I11):
   ```
   runStopTheWorldSafepointHooks();                          // §9 registry
   m_isCompilerThreadsSuspended = suspendCompilerThreads();  // Heap.cpp:2385
   for each client: client.m_localEpoch = m_epoch;           // §11 (exact)
   safepointEpoch().bumpAndReclaim();                        // asserts I11
   resumeCompilerThreads(); m_isCompilerThreadsSuspended = false;
   ```
8. **Resume (heap).** `resumeAllocating()` on every client's caches; clear
   `worldIsStoppedForAllClients` (under `m_gcBarrierLock`); seq_cst
   `m_gcStopPending = false`; broadcast `m_gcBarrierCondition`; re-acquire own
   client's access.
9. **Resume (VMManager).** `requestResumeAll(StopReason::GC)` →
   `resumeTheWorld()`. Parked mutators re-evaluate the §13.5 `shouldStop()` (GC
   bit clear), exit via the existing `m_currentStopReason == None` path — GC was
   never latched (§13.5c) — and the `gcDidResumeFromStopTheWorld` hook
   re-acquires each VM's client's access (blocking if a NEW stop is pending —
   correct). Conductor releases `m_gcConductorLock`, re-checks its ticket.
10. One client / option off: existing `m_worldState` protocol (I10, I15); the two
    protocols never run concurrently (I13/I15).

Heap resume (8) strictly precedes VMManager resume (9) — normative. The three
§2.6 failure modes are deadlock-free here (blocked-in-native = accessless, never
waited on; the sync requester is the conductor or an access-released follower;
zero entered VMs ⇒ barrier completes on access state alone) — full derivation in
the history file. Regressions: `blockedInNativeVsGC`, `syncRequesterStorm`,
`noEnteredVMsGC` (§12.1).

### 10A. Per-client heap-access protocol

Replaces the single `hasAccessBit` (`Heap.h:986-987`) and
`Heap::acquireAccess/releaseAccess/stopIfNecessary` (`Heap.cpp:2143-2237`):

- `Atomic<uint8_t> m_accessState` ∈ {HasAccess, NoAccess} + debug `m_accessOwner`.
- **Rule: no thread reads or writes the shared JS heap (allocate, mutate cells,
  touch butterflies) except while holding some client's heap access.** Covers
  C-API/host threads with no `VMEntryScope` (invisible to VMManager counts; the
  barrier catches them). Wiring: `JSLock::didAcquireLock/willReleaseLock` already
  call the server's `acquireAccess/releaseAccess`; in shared mode those forward
  to the *main client's* `acquireHeapAccess/releaseHeapAccess` (owned
  `Heap.cpp`); secondary clients wired by their creating workstream (§9).
- `acquireHeapAccess()`: F8 sequence; on success re-stamp `m_accessOwner`, ensure
  `addCurrentThread()` ran (I4(b)).
- `releaseHeapAccess()`: flush nothing (conductor flushes at stop); seq_cst
  exchange →NoAccess; signal the barrier only if `m_gcStopPending` observed true.
- `stopIfNecessaryForAllClients()`: if shared and pending, release → wait →
  re-acquire. Called from `collectIfNecessaryOrDefer`.
- Entered threads need no manual calls (§13.5 hooks); native blocking bracketed
  per §9. Single-client mode: maintained but never blocks (legacy bit
  authoritative).

### 10B. Bridge to the Riptide machinery (the step-7 "collection")

1. **Ticketing.** `requestCollectionAllClients` appends to
   `m_requests`/`m_lastGrantedTicket` under `m_threadLock` like
   `requestCollection` (`Heap.cpp:2343-2364`) but: (a) no legacy
   `stopIfNecessary()`; (b) sets `mutatorHasConnBit` **idempotently via
   `exchangeOr`**, asserting only `!m_collectorThreadIsRunning`; must NOT assert
   `m_lastServedTicket == m_lastGrantedTicket` (granted-unserved is legal, §2.5);
   (c) the conductor **drains until `m_lastServedTicket ==
   m_lastGrantedTicket`**. The `:2346-2348` asserts generalize to "caller holds
   heap access for a registered client, or is the conductor inside the stop
   window".
2. **Conductor conducts as the mutator**: phase loop as `waitForCollector` with
   the conn mutator-held (`runCurrentPhase(GCConductor::Mutator,...)`,
   `Heap.cpp:2073-2090`) until drained; sound because every client is accessless.
   The conductor may be a VM-less harness thread; `Heap::vm()` still denotes the
   main VM (deviation 3); `vm()`-mediated thread-identity assumptions reached
   from the phase loop gain `|| worldIsStoppedForAllClients()` (T5b/T9).
3. **Collector thread quiesced.** All triggers re-route once shared (I15) ⇒ the
   collector `AutomaticThread` (`Heap.h:1019`) never observes work
   (`shouldCollectInCollectorThread`, `Heap.cpp:1360-1366`, stays false).
   `stopTheMutator`/`resumeTheMutator` (`Heap.cpp:1969,2008`) are collector-conn
   paths, unreachable in shared mode (asserted).
4. **Switch quiescence (I13/I15).** `add` of client #2 waits under `m_threadLock`
   for `m_lastServedTicket == m_lastGrantedTicket && !m_collectorThreadIsRunning
   && m_currentPhase == NotRunning` before setting sticky `isSharedServer()`.
5. **Assertion/bit audit (T5b).** Sites gaining
   `|| worldIsStoppedForAllClients()` (or generalization): `checkConn`
   (`:1390-1401`; conn always Mutator); `requestCollection` asserts
   (`:2346-2348`); periphery mutator-fence bookkeeping (`:1883-1968`;
   `setMutatorShouldBeFenced` `:935,1024` — shared mode keeps barriers
   always-fenced once `isSharedServer()`; revisit with concurrent marking);
   `handleNeedFinalize` (`:2240`); `vm()` uses (T9). Bits: `mutatorHasConnBit`
   always set during a cycle; `stoppedBit`/`hasAccessBit` keep main-client
   meaning, superseded by `worldIsStoppedForAllClients()` in generalized asserts;
   `mutatorWaitingBit`/`needFinalizeBit` unchanged. No JS-executing finalizers
   inside the stop window (existing deferral; T5b verifies).
6. **Compiler threads**: see §10.7 / §2.5 / I11.
7. **Disabled in shared mode (deviation 4):** collector-conn conducting;
   concurrent marking; incremental assist (`performIncrement` early-returns;
   `stopIfNecessarySlow`'s `collectInMutatorThread` entry, `Heap.cpp:2035-2090`,
   reached only by the conductor world-stopped); activity-callback collection;
   incremental sweeping concurrent with mutators (sweeper runs on the main
   client's thread between collections — T8 audits against I5b/I16). Parallel
   marking inside the stop stays on.

### 10C. Other stop reasons (phase-1 boundary)

- GC never enters VMManager's latch/dispatch (§13.5c); the assert stays.
- Debugger reasons concurrent with shared-mode GC: **unsupported phase 1** (a
  debugger stop latched before a GC request leaves parked VMs holding access ⇒
  step-4 barrier stalls — watchdog-visible, not corrupting). Lifting requires
  debugger handlers to join the access protocol (out of scope).
- SPEC-jit JSThreads stops serialize with shared-mode GC by holding
  `Heap::m_gcConductorLock` (rank 2) for their stopped window; must not call
  `bumpAndReclaim`. Answers CS2's R1.g/R1.h (§13.10b): R1.h overlap is unsound
  against the access barrier; serialization preserves R1.g progress (a JSThreads
  requester blocked on the rank-2 lock holds no heap access).



---
# Appendix C — rev-5 full text of sections digested in SPEC-heap.md (NON-NORMATIVE since rev 8; archived prose)

SPEC-heap.md rev 5 digests the following sections to fit its size cap; the binding full text is here, verbatim. On conflict with the digests, the digests win on rules, this appendix wins on detail.

### 5.2 Synchronized block handout

One per-server MSPL (the FIXME's own proposal) taken in `allocateSlowCase` before `tryAllocateWithoutCollecting`, held through `tryAllocateBlock`/`addBlock`/steal (`LocalAllocator.cpp:133-156` + all of `tryAllocateWithoutCollecting`). Serializes: cross-directory steals; `didAddBlock`/`didAllocateInBlock` accounting; `tryAllocateLowerTierPrecise` + its free list; `addBlock` bitvector resizes (I5b); precise registration (§5.6). Single-thread cost: one uncontended CAS on a block-sweep path.

Edits (owned): (1) `allocateSlowCase` `:113` — the `:116` assert becomes `ASSERT(heap.currentThreadIsAllocatorOwner(this))` (access-based ⇒ JSLock hand-off legal, I2); counters relaxed atomic (§5.4); CIND is the stop-participation point (§10A); take `Locker { MSPL }` after it returns, covering `:133-156`; never hold across collection request / stop participation (L2). (2) `tryAllocateIn` — eden-bit store `:250` under BVL; `didAllocateInBlock` atomic. (3) `tryAllocateBlock` (`BlockDirectory.cpp:128-137`) — `WTF_REQUIRES_LOCK(MSPL)` via `AbstractLocker&`. (4) stop/resume/prepare — `ASSERT(!ISS || WSAC)`. (5) `addBlock` callers assert MSPL when ISS.

### 5.6 Precise (oversized) allocation synchronization

Racy today: `tryAllocateSlow` (`CompleteSubspace.cpp:115-146`) reads size `:139`, appends `:143`, `registerPreciseAllocation` `:144` → re-stamps index, appends to `MarkedSpace::m_preciseAllocations` + `preciseAllocationSet` (`MarkedSpace.cpp:231-241`); concurrent appends reallocate Vectors under readers ⇒ UAF. Same class: `reallocatePreciseAllocationNonVirtual` `:148-202` (writes `:182,189-199`).

Rule (shared mode): under `Locker { MSPL }` — `tryAllocateSlow` `:138-145` as one critical section (lock after CIND `:132`, per L2); `reallocatePreciseAllocationNonVirtual` `:173-199` (after `:171`); every other mutator-path mutation found by T3b. Readers run world-stopped in shared mode (I5) or under today's single-mutator rules — T3b audits each against I16. VM-coupled preludes skipped for standalone clients (§12.1).

## 11. Epoch-based reclamation (`heap/GCSafepointEpoch.h/.cpp`, new)

State: global `Atomic<uint64_t> m_epoch { 1 }`; per-client `Atomic<uint64_t> m_localEpoch` (in GCH); `Lock m_retireLock; Vector<RetiredItem> m_retired;`, `RetiredItem { void* ptr; void (*destroy)(void*); uint64_t epoch; }`.

- `retire(p, d)`: append `{p, d, m_epoch.load(acquire)}` under `m_retireLock`.
- Publication is **conductor-side**: §10.7 stores `m_localEpoch = m_epoch` for every client (all accessless — exact); no mutator-side hook.
- `bumpAndReclaim()` (conductor, §10.7 only; release-asserts I11): `minEpoch = min(m_localEpoch)`; destroy items with `epoch < minEpoch`; `m_epoch.store(old+1, release)`.
- DCT sets the client's local epoch to `UINT64_MAX`.
- Compiler/GC-helper threads are not participants (excluded by the suspension precondition). Non-GC-stop reclamation would need compiler-thread publication — follow-up, not frozen.
- **SPEC-jit R4/CS4 refused (normative):** WSAC is conductor-only; compiler threads aren't suspended by a bare VMM stop; `m_localEpoch` is published only at GC stops ⇒ a non-GC bump reclaims against stale epochs. JSThreads stops needing reclamation enqueue a GC request (CR §13.10a).

Only `void*` + destroy thunks — no JIT dependency.

### 12.1 Multi-client harness

- `SharedHeapTestHarness`: given a server `JSC::Heap&`, spawn K raw `WTF::Thread`s, each constructing a standalone `GCClient::Heap(server)` on its stack (`markStandalone()` + ACT), running C-level allocation/steal/detach/epoch loops — no JS, no VM entry (exercises the zero-entered-VMs stop path). Harness threads ARE real clients (HCS, `MachineThreads`, access protocol) — ISS becomes reachable.
- **Allocation seam.** Harness threads must NOT call `CompleteSubspace::allocate(VM&,...)` (§2.3). Owned overloads `CompleteSubspace::allocateForClient(GCClient::Heap&, size_t, GCDeferralContext*, AllocationFailureMode)` (+ iso equivalent) route through the client's TLC, skipping the VM-coupled preludes (harness stacks covered by suspend-and-copy, I12); CIND/SINFAC ARE called. VM-taking overloads delegate to client-taking ones after the prelude.
- **`vm()` poisoned for standalone clients**: `markStandalone()` sets a flag; `vm()` (`HeapInlines.h:239-242`, owned) gains `RELEASE_ASSERT(!m_isStandalone)`; T9 audits.
- Scenarios (pass/fail + counters; descriptions: history rev-4 §12.1): `allocationStorm`, `preciseAllocationStorm` (I16), `stealRace`, `clientChurnVsGC`, `epochReclaim`, `structureLockVsSTW` (I14), `blockedInNativeVsGC`, `syncRequesterStorm`, `noEnteredVMsGC`. The `$vm.sharedHeapTest` shim's own join is bracketed RHA/AHA inside the harness (owned).
- JS exposure: manifest entry 8; `JSTests/threads/heap-*.js` drive it under `--useSharedGCHeap=true`, no-JIT and JIT-on (JIT-on validates §5.5).



## (Appendix C cont.) rev-5 full text of §5.3-§5.4

### 5.3 TLC (new `heap/GCThreadLocalCache.h/.cpp`)

Index assignment (server-side, owned): monotonic `m_nextTlcIndexBase` under MSPL; each CSS reserves a contiguous `MarkedSpace::numSizeClasses` range at construction (`m_tlcIndexBase`); slot for size class `i` = `tlcIndexBase() + i`. Each non-iso BD records `m_tlcIndex`, assigned in `allocatorForSlow` (`BlockDirectory(size_t)` becomes `(Heap&, size_t)`; all construction sites in `heap/**`). Iso: `UINT_MAX`. Aliased size-class entries share the per-thread LA*.

```cpp
namespace JSC { namespace GCClient {
class GCThreadLocalCache {
    WTF_MAKE_NONCOPYABLE(GCThreadLocalCache);
public:
    explicit GCThreadLocalCache(JSC::Heap& server);
    ~GCThreadLocalCache();    // stopAllocatingForGood() on every slot

    // Fast path: bounds check + one indexed load
    // (m_table[subspace.tlcIndexBase() + sizeClassIndex], null => slow path).
    // Slow path materializes the LocalAllocator (deduped per directory, I3),
    // grows m_table.
    Allocator allocatorFor(BlockDirectory&);              // by directory->tlcIndex()
    Allocator allocatorForSizeStep(CompleteSubspace&, size_t sizeClassIndex);

    void stopAllocating();
    void resumeAllocating();
    void prepareForAllocation();
    void stopAllocatingForGood();

    // PROVISIONAL JIT addressing contract (status note below):
    //   slot = tlcIndexBase + sizeClassIndex;
    //   slot < *offsetOfTableBound() ? table[slot] : null; null => slow path
    static constexpr ptrdiff_t offsetOfTable();
    static constexpr ptrdiff_t offsetOfTableBound();
private:
    JSC::Heap& m_server;
    Allocator* m_table { nullptr };  // flat; entry = per-thread LocalAllocator* or null
    unsigned m_tableBound { 0 };     // grows, never shrinks
    Vector<std::unique_ptr<LocalAllocator>> m_ownedAllocators;
    HashMap<BlockDirectory*, LocalAllocator*> m_perDirectory; // cold slow path; I3
};
}}
```

**Status.** Layout + indexing FROZEN. The JIT addressing contract (offset exports + chain `vmGPR + OBJECT_OFFSETOF(VM, clientHeap) + offsetOfThreadLocalCache()`) is **provisional**: no frozen sibling consumes it (deviation 6); offsets stay exported and layout-stable for §13.10c.

Supporting changes (owned): under the option, `CompleteSubspace::allocatorFor`/`Subspace::allocate` route through the caller's `threadLocalCache()`; server-side allocator array/vector **never populated** (the JIT gate, §5.5); `allocatorForSlow` still creates directories (under MSPL) but no server LA; `MustAlreadyHaveAllocator` forbidden in shared mode (RELEASE_ASSERT; audit T4). Option off ⇒ byte-for-byte today (I10). GCH gains `m_threadLocalCache` + accessors + `offsetOfThreadLocalCache()`; implements `lastChanceToFinalize()` per the `Heap.h:1279-1281` FIXME. Teardown per allocator: `stopAllocatingForGood()` (returns `m_currentBlock`, clears `inUse`), then unlink under `m_localAllocatorsLock` (`LocalAllocator.cpp:55-60`).

### 5.4 Accounting atomic; assist/activity gated off in shared mode

1. `m_nonOversizedBytesAllocatedThisCycle`, `m_oversizedBytesAllocatedThisCycle`, `m_lastOversidedAllocationThisCycle`, `m_bytesAbandonedSinceLastFullCollect`, `m_blockBytesAllocated`, `MarkedSpace::m_capacity` (`CompleteSubspace.cpp:197`) → `std::atomic<size_t>`, relaxed both sides.
2. **`performIncrement` unreachable in shared mode**: early-return when ISS (incremental marking assists concurrent marking — disabled by deviation 4). Debug assert: mutator `SlotVisitor` used only world-stopped when shared. `m_incrementBalance` stays a plain double.
3. **Activity callbacks don't fire collections in shared mode**: `didAllocate` skips `m_edenActivityCallback->didAllocate`; triggering is mutator-driven only (CIND + CSAC). Re-routing is I15 (T5).



---
# Appendix D — rev-5 full text of further digested sections (NON-NORMATIVE since rev 8; archived prose + test sketches)

## 8. Invariants (numbered, testable; "T:" = test)

- **I1** (block ownership): a handle with `isInUse` set is referenced (`m_currentBlock`/`m_lastActiveBlock`/in-sweep) by ≤1 thread; transfer only under that directory's BVL. T: debug owner field; TSAN.
- **I2** (allocator access-confinement): an LA/`FreeList` is mutated only by the thread holding its owning client's heap access, except the conductor world-stopped; access-based, not thread-pinned (JSLock migration transfers ownership). T: debug `m_accessOwner`.
- **I3** (one allocator per (client, directory)): dedup via `m_perDirectory`; aliased slots share the pointer. T: slow-path assert.
- **I4** (registration before allocation): allocation only with access held for a client with completed ACT: (a) in HCS; (b) `machineThreads().addCurrentThread()` ran on this thread (re-required per migrated thread; enforced in AHA); (c) access held. T: release-assert in first `allocateSlowCase`.
- **I5** (STW collection): shared mode runs marking-start, stop/prepare-allocation iteration, conservative scan, constraint solving, sweep scheduling, and precise-vector iteration only on the conductor (or its parallel helpers) while WSAC; excluded: I5b, I16. T: entry asserts; stop watchdog.
- **I5b** (bitvector storage): `m_bits` reallocated only in `addBlock` holding BVL + (shared) MSPL; every bitvector access holds that directory's BVL, MSPL, is world-stopped, or runs `!ISS` under today's rule; T8 audits every reader. T: debug accessor; TSAN.
- **I6** (no heap locks while parked): a mutator parked in NVS, blocked in AHA, or waiting in SINFAC holds no rank ≥ 4 lock and no SAL. T: debug lock-rank counter zero at park.
- **I7** (accounting): at every safepoint counters equal the true allocation sum since the last; monotone between. T: per-client shadow counters (debug).
- **I8** (steal safety): a stolen empty block is swept/removed/re-added under MSPL; never allocatable in two directories at once. T: assert `block->directory()` transitions under the lock; TSAN.
- **I9** (client teardown): after `lastChanceToFinalize()` no allocator of the client is in any `m_localAllocators` list and every held block has `inUse == false`. T: directory walk (debug).
- **I10** (single-thread zero-cost): option off ⇒ fast/slow paths execute the same code as `main` (branches gated; TLC bypassed; server allocators populated; legacy protocol incl. concurrent marking + `performIncrement`). T: bench gate + fast-path disassembly diff.
- **I11** (epoch reclamation): an item retired at epoch E is destroyed only after (a) every client published `localEpoch > E` (or detached) AND (b) inside a window with world-stopped true AND compiler threads suspended **by the conductor's own §10.7 suspend/resume pair** (NOT the conducted cycle's — undone before it returns); `bumpAndReclaim()` RELEASE_ASSERTs this; GC-conductor-only (CS4 refused); marker helpers run only inside the stop window. T: harness + fake compiler-reader; TSAN.
- **I12** (conservative completeness): root set ⊇ stack ∪ registers of every I4(b)-registered thread (suspend-and-copy, §10.6) ∪ `CLoopStack` if `!ENABLE(JIT)` (`Heap.cpp:910`) ∪ each client's `m_currentBlock` new cells. T: N-thread allocation, GC from thread 0, `--gcAtEnd=true`, ASAN.
- **I13** (no client churn during GC; sticky; one shared server): `add`/`remove` cannot complete between stop and resume; `remove` of a stopped client defers to resume; the `add` exceeding one client (i) RELEASE_ASSERTs no other sticky-shared server in the process, (ii) sets sticky ISS, (iii) blocks until legacy-quiescent (§10B.4), then inserts; never reverts. T: spawn/exit vs forced-GC loop; attach during legacy collection.
- **I14** (no STW under rank-7a locks): a SAL holder must not call `requestStopAll`/CSAC/SINFAC or park; allocations pass `GCDeferralContext`; debug STW-forbidden counter checked at those entries; `incrementSTWForbiddenScope()/decrement...` exposed for vmstate (§9). T: debug assert + `structureLockVsSTW`.
- **I15** (protocol exclusivity): one collection protocol at a time — legacy while `!ISS`, §10 once set; switch only inside `HeapClientSet::add` under I13 quiescence; triggers re-route thereafter. T: assert legacy `requestCollection` unreached when ISS.
- **I16** (precise registry): shared mode mutates both `m_preciseAllocations` vectors, `preciseAllocationSet`, and `indexInSpace` stamps only under MSPL or world-stopped; readers likewise (or `!ISS`). T: debug accessor; `preciseAllocationStorm` (TSAN).

Contract notes:
- Object-model: cells from any client lie in blocks readable under I5b; `releaseQuarantinedSlots` runs at every stopped window once registered (adapter passes `safepointEpoch().current()`).
- JIT: `retire` callable from any thread with no rank ≥ 7 lock; async-signal-unsafe; timing per I11.
- Secondary-context creators construct `GCClient::Heap(sharedServer)`, bracket thread lifetime with ACT/DCT. **Blocking-primitive obligation** (Thread-API): `Atomics.wait`, `Lock`/`Condition` parking, `Thread.join`, any indefinitely-blocking host call MUST be bracketed RHA/AHA (SPEC-api §5.2 wraps parks). Blocked-with-released-access = stopped; blocking while *holding* access stalls the group's GC (watchdog-visible violation).
- vmstate's `StructureAllocationLocker`: `incrementSTWForbiddenScope()/decrement...` + `GCDeferralContext` (I14/L5).
- Debugger STW overlapping shared-mode GC: unsupported phase 1 (§10C).



## §13 entries 9-10 (informational, full text)

9. **Erratum for `SPEC-vmstate.md` §7** (informational): `:1159-1172` still ranks VMM STW (10) before JSLock (20), contradicting its own round-2 disposition (`:364-372`, `:282-284`) and the real call pattern (`VMManager.cpp:353,387`); delete those rows (interim rule: §6).
10. **Cross-spec change requests** (informational): a. SPEC-jit R4/CS4 REFUSAL — `bumpAndReclaim()` is GC-conductor-only; JSThreads stops enqueue a GC request. b. SPEC-jit CS2/M4 — R1.h overlap → serialization via GCL (§10C); same-file coordination per entry 5. c. SPEC-jit (scope) — adopt TLC-aware emission against §5.3's provisional contract later. d. SPEC-objectmodel — `releaseQuarantinedSlots` via `addStopTheWorldSafepointHook` (§9/§10.7); adapter registered at init.



### 5.4 Accounting atomic; assist/activity gated off in shared mode

1. `m_nonOversizedBytesAllocatedThisCycle`, `m_oversizedBytesAllocatedThisCycle`, `m_lastOversidedAllocationThisCycle`, `m_bytesAbandonedSinceLastFullCollect`, `m_blockBytesAllocated`, `MarkedSpace::m_capacity` (`CompleteSubspace.cpp:197`) → `std::atomic<size_t>`, relaxed both sides.
2. **`performIncrement` unreachable in shared mode**: early-return when ISS (incremental marking assists concurrent marking — disabled by deviation 4). Debug assert: mutator `SlotVisitor` used only world-stopped when shared. `m_incrementBalance` stays a plain double.
3. **Activity callbacks don't fire collections in shared mode**: `didAllocate` skips `m_edenActivityCallback->didAllocate`; triggering is mutator-driven only (CIND + CSAC); re-routing is I15 (T5).



### 10C. Other stop reasons (phase-1 boundary)

GC never enters VMM's latch/dispatch (§13.5c; `:462-463` assert stays). Debugger stops concurrent with shared-mode GC: **unsupported phase 1** (barrier stalls, watchdog-visible, not corrupting; lifting = debugger handlers join the access protocol, out of scope). SPEC-jit JSThreads stops serialize with shared-mode GC by holding GCL (rank 2) for their stopped window; must not call `bumpAndReclaim` (§13.10b: R1.h overlap unsound; R1.g preserved — a requester blocked on GCL holds no heap access).


## F8 soundness proof (archived from SPEC-heap.md rev 4 §7/F8)

F8 (stop-pending vs. access-acquire, Dekker pair). `Heap::m_gcStopPending`; sole
writer the conductor: seq_cst store `true` at §10.3 before sampling client states;
seq_cst store `false` at §10.8. Client `acquireHeapAccess()`: (1) seq_cst CAS
NoAccess→HasAccess; (2) seq_cst load `m_gcStopPending`; (3) if pending: mandatory
revert — seq_cst exchange →NoAccess, signal `m_gcBarrierCondition` under
`m_gcBarrierLock`, block until `!m_gcStopPending`, retry from (1). Conductor:
seq_cst samples each client under `m_gcBarrierLock`, waits until all NoAccess.
Soundness: in the seq_cst total order, a conductor load returning NoAccess either
precedes the client's CAS — then the earlier pending-store precedes the client's
step-2 load, which sees `true` and reverts before touching the heap — or follows
the revert; either way the conductor proceeds only when no client can mutate.
Acq/rel alone lets both store-load pairs miss (silent corruption); seq_cst is
mandatory and debug-asserted via a fence-checking wrapper.


---
## 19. Review notes — round-5 findings (rev 7 → rev 8): dispositions

All nine round-5 findings were verified against the tree and accepted (no
false positives this round). Resolutions, with the evidence:

1. **NBR cap evasion (blocker) — ACCEPTED, structural fix.** Rev 7 kept
   SPEC-heap.md at 39,996 bytes only by declaring this history file's
   appendices "normative-by-reference". Rev 8 demotes this entire file to
   non-normative and makes the spec self-sufficient: the §5.4 counter list,
   the §10A JSLock wiring, the §10B.1 drain rule and §10B.5 audit-site list,
   and the §10B.4 quiescence predicate were folded into the spec; ground
   truth (App. A), invariant test sketches (App. D), the F8 proof, and all
   rationale remain here as background. The spec was compressed (~5KB of
   derivational/duplicative prose removed; no layout, signature, invariant,
   lock-order, manifest, or task content cut) to 39,997 bytes.

2. **Manifest 5f breaks non-GC stops as literally written (blocker) —
   ACCEPTED.** Verified `VMManager.cpp`: today the latch (`:404-407`) runs
   BEFORE the first `shouldStop()` evaluation; `shouldStop()` returns false
   for reason `None` with no targetVM, and the post-loop `None` path breaks
   out without dispatching. Rev 7's "move the latch inside the wait loop:
   re-fetch after every wake" would therefore strand every fresh
   WasmDebugger/MemoryDebugger/JSDebugger stop. Rev 8's 5f gives the exact
   post-edit loop: `for (;;) { if (m_currentStopReason == StopReason::None)
   m_currentStopReason = fetchTopPriorityStopReason(); if (!shouldStop())
   break; m_worldConditionVariable.wait(m_worldLock); }` — fetch precedes
   the FIRST `shouldStop()` and re-runs after every wake.

3+7. **`currentThreadIsAllocatorOwner` false when option off / pre-ISS /
   iso (major + blocker; two findings, same root) — ACCEPTED.** Verified
   `LocalAllocator.cpp:116` (`ASSERT(heap.vm().currentThreadIsHoldingAPILock())`)
   and that `allocateSlowCase` is shared by iso and non-iso LAs while rev 7
   only populated `m_perDirectory` for non-iso. Rev 8 §10A.1 defines the
   predicate total over all quadrants: `!ISS` (option off OR option-on
   pre-sticky) => today's predicate `vm().currentThreadIsHoldingAPILock()`;
   ISS => TLS client non-null ∧ `la` ∈ `m_perDirectory`, and §5.3 now
   registers `GCClient::IsoSubspace` LAs in `m_perDirectory` at
   materialization (lookup-only). §10A.1 also pins TLS stamping and JSLock
   forwarding to sticky ISS (not the bare option).

4. **Done bar depends on un-applied manifest hunks (major) — ACCEPTED.**
   Rev 8 §14 adds a normative Gating paragraph: the implementer never
   applies §13 hunks to the shared tree; in-workstream Done bar = the §12.1
   scenarios reachable through owned code (all but the three stop-overlap
   scenarios) + the I11 unit test; integration-gated (evaluated only after
   the integrator applies manifest 3-5, 8, 11): `debuggerStopDuringSharedGC`,
   `gcDuringDebuggerPark`, `jsThreadsStopVsGCRequester`, the
   `JSTests/threads/heap-*.js` corpus, T5b end-to-end.

5. **§10B.4 wait mechanism unspecified / liveness unargued (major) —
   ACCEPTED.** Verified `Heap.h:1018` ("The mutator must not wait on this").
   Rev 8 §10B.4 specifies: the quiescence wait runs under `*m_threadLock`
   BEFORE taking HCS `m_lock` (rank order 5 then 6) as a timed (≤1ms)
   re-check loop on `m_gcElectionCondition` (the new §10.2 condition; the
   serve-path notify at `Heap.cpp:1805-1810`, an owned edit, is an
   accelerator — the timed loop alone guarantees liveness with no notify
   edge); sticky ISS is set in the same critical section once quiescent,
   then the insert happens under `m_lock`. Granted-unserved + blocked
   requester: new cross-spec liveness obligation (never block indefinitely
   on a freshly attaching client while your VM has granted-unserved
   tickets; creators poll CIND/SINFAC) + new regression scenario
   `attachWithPendingTicket`.

6. **Shared `Heap::m_deferralDepth` race (blocker) — ACCEPTED.** Verified
   `HeapInlines.h:166-176` plain `++/--` and `Heap.cpp:2938` consulting it;
   `heap/HeapInlines.h` is heap-owned so no other workstream would fix it.
   Rev 8 §5.4 makes deferral depth per-client (counter in GCClient::Heap;
   increment/decrement route via `currentThreadClient()` when ISS;
   `isDeferred()`/CIND consult the calling client's depth; option off
   byte-identical), adds invariant **I17**, and adds the
   `deferralVsAllocationStorm` harness scenario. Per-client (not atomic
   shared) is the correct shape: one client's deferral must not be consumed
   by another's decrement.

8. **§6 contradicts SPEC-jit §7 on `CodeBlock::m_lock` and the retire lock
   (major) — ACCEPTED.** Verified SPEC-jit.md §7 chart (CodeBlock::m_lock
   outside the Structure/cell lock; retire lock INSIDE the cell lock, with
   a self-contradictory "no heap lock rank >= 3" parenthetical) and
   SPEC-objectmodel.md §6 ("CodeBlock::m_lock and all jit-§7 locks are
   OUTER"). Rev 8 §6: new rank **6b** for `CodeBlock::m_lock` + JIT
   worklist locks (jit §7 orders within 6b; outer to 7a-10); the leaf row
   for `GCSafepointEpoch::m_retireLock` now explicitly permits acquisition
   under rank-10 cell/Structure locks while forbidding 7-9, and the §9
   `retire()` contract note matches. Recorded as CR §13.10f (jit's
   parenthetical declared superseded by its own chart).

9. **No carve-out for objectmodel's flatness-guard hunks in heap/** (major)
   — ACCEPTED.** Verified SPEC-objectmodel manifest 7 greps heap/ and names
   HeapVerifier. Rev 8 §12 adds Carve-out 2 mirroring the M9 precedent:
   the integrator applies objectmodel's guards ON TOP of heap's final
   heap/** tree; heap's T8/T9 rewrites must keep `butterfly()` call sites
   textually greppable. Recorded as CR §13.10g.

# 20. Review notes — round-6 findings (rev 8 → rev 9): dispositions

All seven filed findings were verified against the tree; 5 and 6 are the same
defect (no legacy fire point for the STW safepoint hooks). All ACCEPTED; none
refuted. Spec deltas are folded into rev 9; full reasoning below.

1. **TLC/client teardown vs §5.2(4) assert (blocker) — ACCEPTED.** Verified
   `LocalAllocator::stopAllocatingForGood()` (`LocalAllocator.cpp:107-111`)
   calls `stopAllocating()`, which hands `m_currentBlock` back and flips
   directory-bit state — while rev-8 §5.2(4) asserted `!ISS || WSAC` on the
   stop/resume/prepare family and §5.3 ran client teardown between stops
   (HeapClientSet::remove only blocks DURING a stop, I13). Under ISS the
   `clientChurnVsGC` scenario would fire the assert every time in debug, and
   in release the bit flips would race other clients' cursor scans with only
   `m_localAllocatorsLock` (rank 8) held — which protects the list unlink,
   not the bitvectors (I5b). Rev 9: §5.3 teardown now holds the server MSPL
   (rank 7, outer to rank 8 — consistent with §6) across every slot's
   `stopAllocatingForGood()`; §5.2(4) assert is rescoped to
   `WSAC ∨ (MSPL: §5.3 teardown) ∨ !ISS`; I5b's writer list and the T8 audit
   explicitly include the teardown path. Alternative considered (defer
   teardown to the next stop window) rejected: it would make detach latency
   unbounded when no GC is pending.

2. **In-workstream Done bar unsatisfiable (blocker) — ACCEPTED.** Verified by
   grep: `useSharedGCHeap` appears nowhere under Source/JavaScriptCore/
   (it is created only by manifest 2 in non-editable OptionsList.h);
   `gcWillParkInStopTheWorld`/`setGCParkCallbacks` absent from JSCConfig.h /
   VMManager.h; the four new .cpp files enter the build only via the
   manifest-1 Sources.txt entry; the only §12.1 entry point is the gated
   manifest-8 `$vm.sharedHeapTest`. So rev-8's "never apply §13 hunks" +
   "run §12.1 scenarios in-workstream" was contradictory. Rev 9 adopts the
   reviewer's option (c)-shaped fix with (a)'s ergonomics: a normative
   **throwaway local overlay** — the implementer applies manifests 1-5, 8, 11
   verbatim from INTEGRATE-heap.md (written first, T1) to a private
   worktree, builds and runs everything there, and never commits overlay
   hunks (commits = owned paths only; off-spec shims, e.g. a local
   env-var option stub, are expressly forbidden — they would diverge from
   the integrated gate). In-overlay Done bar = all §12.1 scenarios + the
   I11 unit test; the three §10C stop scenarios, the JS corpus, and T5b are
   re-verified at integration on the integrator-applied tree. Option (b)
   (heap-owned env-var gate) was rejected: it forks the gating predicate
   the other four specs key on (`Options::useSharedGCHeap()`), and dollar-VM
   exposure would still be manifest-gated anyway. T5's stale footnote
   ("until then behind the option") removed.

3. **§6 omits `MarkedSpace::m_directoryLock` (major) — ACCEPTED.** Verified
   `MarkedSpace.h:175,223`, `CompleteSubspace::allocatorForSlow` holding it
   (`CompleteSubspace.cpp:68`) with the in-tree comment (`:60-66`) that JIT
   compiler threads take this path, and `IsoSubspace.cpp:50`. Rev-8 §5.3's
   "m_nextTlcIndexBase under MSPL" silently conflicted: allocatorForSlow
   holds directoryLock today, and binding it to MSPL would put MSPL on JIT
   compiler threads. Rev 9: `m_nextTlcIndexBase` moves UNDER
   `m_directoryLock` (index assignment happens exactly where directories
   are created, so no second lock is needed); allocatorForSlow keeps
   today's directoryLock-only locking; §6 gains rank **7b** for
   m_directoryLock (inner to MSPL 7, outer to m_localAllocatorsLock 8;
   JIT threads take it alone). No path needs MSPL and directoryLock in the
   other order: MSPL sections (§5.2/§5.6) never create directories.

4. **"didAllocateInBlock atomic" unimplementable (major) — ACCEPTED.**
   Verified `MarkedSpace::didAllocateInBlock` (`MarkedSpace.cpp:553-559`)
   is a SentinelLinkedList splice (WeakSet moved onto m_newActiveWeakSets),
   not a counter. Rev 9 replaces the directive with the reviewer's
   suggested rule verbatim in substance: no change needed — the sole
   mutator call site (`LocalAllocator.cpp:251`, inside tryAllocateIn) is
   within the §5.2 MSPL critical section, so splices are serialized across
   mutators; all other m_newActiveWeakSets accesses are conductor-side
   while WSAC (I5) with mutator-concurrent sweeping disabled (deviation 4).
   Added: debug assert `MSPL ∨ WSAC ∨ !ISS` inside didAllocateInBlock and
   m_newActiveWeakSets in the T8 audit list.

5./6. **No hook fire point in legacy collections (blocker + major, same
   defect) — ACCEPTED.** Verified SPEC-objectmodel.md:187 requires the
   quarantine adapter to run world-stopped in EVERY collection, legacy AND
   shared (`useJSThreads !=> useSharedGCHeap`; phase 1 runs legacy GC
   only), while rev-8 specified exactly one invocation site (§10 step 7,
   shared-conducted only) and the §9 contract note self-contradicted on
   cadence ("every stopped window" vs "once per collection"). Rev 9 fixes
   both: hooks fire ONCE PER COLLECTION (not per stop window) in BOTH
   protocols; the legacy call site is named — `Heap::runEndPhase`,
   immediately before `didFinishCollection()` (`Heap.cpp:1789`), where the
   mutator is provably suspended (in-tree comment at `:1766-1768`: "mutator
   is suspended so there is no race condition"); assert `worldIsStopped()`
   at the call. Wired into §9 contract notes (canonical), §10 item 10, T5,
   and CR §13.10d. The unconditional (no-op-when-unregistered) call is
   declared the sole option-off code delta — an explicit I10 exemption.
   Cadence choice: once per collection suffices for OM's epoch proof (a
   crossed bump proves a full stop after deletion); per-stop-window firing
   would over-promise under legacy concurrent GC.

7. **JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE has no provider (major) — ACCEPTED.**
   Verified SPEC-vmstate.md N7 (:276-279) compiles its
   StructureAllocationLocker STW-forbidden calls only under
   `#if defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)` and says "INTEGRATE
   defines the macro once heap lands", while rev-8 heap defined no such
   macro anywhere (no §13 entry, nothing in §9) — leaving I14/L5
   enforcement permanently inert. Rev 9: heap/Heap.h (owned) defines
   `#define JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE 1` next to the
   increment/decrementSTWForbiddenScope declarations; noted in §9 and in
   I14 ("§9 scope pair + N7 macro for vmstate"). No manifest entry needed
   since Heap.h is heap-owned and vmstate's VMLiteShared.cpp necessarily
   includes Heap.h to call the pair.

Editorial (rev 9, no semantic change): heavy compression to stay under the
40,000-byte cap — notation gains GCA/GEC; §13.3's "(rev-2 `gcStopTheWorld`
callback stays DELETED)" parenthetical moved here (it remains true: the
rev-2 single-callback design stays retired; manifest 3 adds only the
willPark/didResume pair); assorted wording tightened. All layouts,
signatures, invariants I1-I17, lock ranks, fences F1-F8, manifest hunks,
and the task list are intact.

## §21. Whole-design adversarial review round 1 dispositions (rev 9 -> rev 10)

1. BLOCKER (x2, cross-cutting + heap): "Master lock order forbids JSCellLock->Structure::m_lock nesting; OM §10.8e OPEN-BLOCKING never landed." ACCEPTED. Verified: objectmodel §6/L3 and in-tree flattenDictionaryStructure (Structure.cpp:1047 cellLock(), then :1049 GCSafeConcurrentJSLocker{m_lock}) require holding both; rev-9 §6 had a single rank 10 and L1 banned same-rank pairs. Fix landed in rev 10: §6 rank 10 split into 10a = MarkedBlock internals / per-cell lock (JSCellLock) and 10b = Structure::m_lock, with 10a < 10b; L1 amended ("exception: 10a<10b"); §13.10f records the split and cites OM ledger 8e as closed; leaf-row and §9 retire() contract wording updated to "10a/10b". OM rev 11 updates its §6 line and ledger 8e to RESOLVED. No other rank moved.

2. BLOCKER (heap): "Shared-heap mode permanently disables JIT inline allocation and concurrent GC; no workstream restores either; cannot meet the near-baseline contract; no useJSThreads<=>useSharedGCHeap coupling decision." ACCEPTED AS A CHARTERING GAP, not a protocol bug. The §5.5 never-populate rule and §3.4 sync-GC mode are deliberate phase-1 simplifications and remain frozen; what was missing is an owner for recovering the perf. Rev 10 adds deviation 7 (normative charter): flag-on phase 1 accepts slow-path-only JS-tier allocation, synchronous conductor GC, and single-MSPL handout (all GIL-masked for JS mutators in phase 1); TLC-aware inline allocation (the §5.3 offsetOfTable/offsetOfTableBound chain exists precisely for it), per-directory handout locks + sweep-outside-MSPL, and restored concurrent marking/incremental sweeping are a chartered follow-up workstream GATING GIL removal and any default-on shipping of useSharedGCHeap; the useJSThreads<=>useSharedGCHeap coupling decision is assigned to INTEGRATE-heap.md. Note the reviewer's "applies the moment the flag is on with zero threads" is correct but flag-on is opt-in this milestone and the flag-off bar (I10) is unaffected.

3. MAJOR (heap): "One global MSPL serializes all block handout incl. sweeping under the lock, stealing, accounting, and every precise allocation." ACCEPTED AS PHASE-1 CARVE-OUT (same deviation 7). Analysis: under the phase-1 GIL at most one JS mutator runs, so MSPL contention from JS is nil; the §12.1 C-level harness (allocationStorm/preciseAllocationStorm) is the only multi-threaded allocator client this milestone and exists to measure exactly this. Redesigning to per-directory handout locks now would ripple through I5b/I8/I16 and the frozen §5.2/§5.6 text for no phase-1 benefit; it is chartered (deviation 7) for the GIL-removal WS, including moving block sweeping outside the lock (claim under lock, sweep after release, libpas-style) as the reviewer suggests.

4. Editorial: rank-10 references throughout (§9 contract notes, §13.10f) updated to 10a/10b; manifest entry 9 (retired vmstate erratum note) moved here: "rev-5 vmstate erratum RETIRED (vmstate §7 defers to §6)". Sundry compressions for the byte cap; no normative content removed.


## §22. Whole-design adversarial review round 2 — resolutions (rev 10 -> rev 11)

1. **Retired JIT artifacts never reclaimed in any phase-1 configuration (major, filed twice; cross-cutting) — ACCEPTED, option (a).** The legacy `runEndPhase` hook site (world stopped, mutator suspended, <=1 mutator client when `!ISS`) now also runs the epoch reclaim sequence: suspendCompilerThreads (reclaimer's OWN pair) -> publish every registered client's localEpoch -> bumpAndReclaim -> resume; no-op when nothing retired, so option-off stays behavior-identical (I10 exemption widened from "hook call" to "hook+reclaim call"). §11 now defines ONE reclaim sequence with exactly two legal contexts (shared §10 step 7; legacy runEndPhase); I11's "GC-conductor-only" relaxed to "reclaimer's own suspension at either context — never a JSThreads stop" (CS4 still refused for non-GC bumps, which would reclaim against stale epochs). jit Task-13's epoch ordering test becomes runnable in the phase-1 1-client legacy configuration (T7 grows that unit test). Soundness with one client is trivial: the sole mutator is suspended at the publish point; compiler threads are suspended by the reclaimer's own pair.
2. **Sticky ISS permanently downgrades GC (major) — ACCEPTED.** New §10D: `remove()` leaving size()==1 arms `m_issRevertPending`; the surviving main client clears ISS at a CIND/SINFAC poll under a §10B.4-style `*m_threadLock` timed quiescence loop with GCL tryLock (excludes conducted cycles and JSThreads stops); deviation-4 features+server deferral re-enable; residual retired items drain via item-1's legacy site, so no pre-revert epoch flush is needed. I13 ("never reverts") and I15 amended; new `issRevertChurn` harness scenario. The §10A access forwarding and 10A.1 TLS stamping deliberately stay keyed on "ever-shared" (harmless when reverted, avoids un-wiring races).
3. **STWR closures write heap without access (major, cross-cutting) — ACCEPTED.** §10A gains the explicit exemption: a JSThreads conductor inside its JSThreadsStopScope stopped window may WRITE heap memory without access (world stopped, GCL held — no GC can run); allocation stays forbidden (I4(c)); OM O4 makes its stop closures allocation-free by pre-allocating.
4. **Flag-on 1-thread tax (blocker, cross-cutting) — ADDRESSED via budgets:** deviation 7 now binds §5.5's slow-path-only allocation to jit Task-13's <=5% composite flag-on 1-thread gate; a miss promotes TLC-aware inline emission from charter to REQUIRED pre-ship.
5. **Cap compliance:** rev 11's additions were paid for by (i) moving manifest item 5's `runtime/VMManager.cpp` hunks VERBATIM to the new sub-cap `SPEC-heap-annex.md` §A5 (FROZEN NORMATIVE; the in-spec pointer indexes hunks a-g, and `§13.5x` cites resolve to annex hunk x) and (ii) wording-only compression logged here: operator spacing squeezed outside code spans; §10 step 7/§9 note/§11 de-duplicated into the single §11 reclaim sequence; §13.10f now points at the §6 rows instead of restating them; 10C(e)/10D/12.1 parentheticals tightened. Every layout, signature, invariant (I1-I17), fence (F1-F8), lock-order row, manifest entry and task survives verbatim in meaning; no normative content dropped.

## §23. Whole-design adversarial review round 3 — resolutions (rev 11 -> rev 12)

1. **Per-CLIENT mutator model vs api's N-threads-in-ONE-VM (major, cross-cutting) — ACCEPTED.** The composition was GIL-sound only: §10A/I2 forbid two threads of one client holding access concurrently, and one client owns one TLC — so the post-GIL product shape (api §5.2: all Threads in the shared VM = today one client) had no bridge to N-mutator parallelism. New §3.8 fixes the post-GIL execution model normatively: ONE GCClient::Heap PER Thread sharing the server. This choice reuses every per-client mechanism unchanged (access state, TLC, deferral depth, local epoch — all already keyed per client and exercised N-way by the §12.1 harness); what remains chartered is client lifecycle wiring inside a single VM (creation at Thread attach, teardown at detach, JSLock interplay) — bundled with deviation 7 and vmstate Dev 10 Phase B, owner recorded in INTEGRATE, and listed in api §2's binding charter enumeration. The alternative (per-thread access states inside one client) was rejected: it redesigns §10A/§5.3 instead of instantiating them.
2. **Thread-granularity STW (major, cross-cutting) — PARTIALLY REFUTED for heap.** The §10 GC stop barrier is already thread-granular: it waits until every CLIENT is NoAccess (F8 Dekker pair), independent of how many threads a VM hosts — deviation 5 made soundness Thread()-definition-independent on purpose. Liveness relies on threads releasing access at RHA/SINFAC polls, also thread-granular. Only VMM trap DELIVERY and jit R1 conductor arbitration are VM-granular; those are vmstate-Phase-B/jit-owned (jit R1 freeze scope), noted in §3.8.
3. **Perf gate blind to §5.5 (major) — ACCEPTED.** §3.7 now cites the two-config matrix (jit Task 13): the composite ALSO runs {useJSThreads=1, useSharedGCHeap=1}, so the never-populate allocation slow path is measured; miss in either config makes TLC-aware inline emission REQUIRED pre-ship; flag-coupling decision owner=orchestrator at GIL removal.
4. **OM quarantine epoch (blocker, OM-owned) — CR mirror updated.** §13.10d now records that the hook adapter bumps OM's PER-SERVER-HEAP quarantine epoch for the hook's Heap (OM §6 r13); the hook signature (takes `JSC::Heap&`) already supports this; no heap interface change.

## §24. Round-4 COMPOSED-design review — rev 13 resolutions

### 24.1 CONFIRMED: L4 ("cellLock never wraps allocation") contradicted OM O1/§4.6
OM O1 and AS-COPY (and the in-tree pattern at JSArray.cpp:1805-1806: DeferGC → Locker{cellLock()}
→ unshiftCountSlowCase allocating) allocate under JSCellLock with a pre-lock DeferGC. DeferGC
suppresses collection, not lock acquisition: under ISS the slow path takes MSPL (rank 7) and
in-lock sweeps touch MarkedBlock internals — formerly lumped into rank 10a with JSCellLock,
making the declared total order carry both 7→10a and 10a→7. Fix (r13): rank 10a split —
MarkedBlock internals become rank 9b (the side taken under MSPL/BVL; NEVER wraps allocation,
Riptide rule preserved); JSCellLock stays 10a (preserving OM/jit "10a"/"10b" cites). New L4:
10a/10b holders allocate only under pre-lock DeferGC/GCDeferralContext (OM O1); that
allocation may take ranks 7-9b — the sole sanctioned back-edge, acyclic because rank 7-9b
critical sections never acquire any 10a/10b lock, now debug-asserted in the §5.2/§5.6 MSPL
sections. Not a runtime deadlock today (MSPL sections never touch cell locks) — the fix makes
the assertion regimes of the heap and OM implementers compatible instead of contradictory.
§13.10f/leaf-row mentions updated 7-9 → 7-9b.

### 24.2 CONFIRMED: two-config <=5% composite gate unachievable as specced
With {useJSThreads=1, useSharedGCHeap=1}, §5.5 never-populate forces EVERY JS-tier allocation
to the C++ slow path; stacked with LLInt cache disables, forced polling traps, forced
m_mutatorShouldBeFenced and single-MSPL, a <=5% geomean in that config is not credible, and
the prescribed remediation (TLC-aware inline emission) rested on the PROVISIONAL §5.3
vm-relative addressing chain, which is wrong post-GIL (one client per THREAD per §3.8 ⇒ the
TLC base must be per-thread). Fix (r13 deviation-7 SPLIT, mirrored in jit Task 13, vmstate
§10, api §2): the composite GATES only {1,0} at <=5%; {1,1} is measured+recorded, its budget
set by the orchestrator at GIL-removal chartering together with the TLC-emission charter;
{1,0} miss ⇒ jit §4.3 LLInt-cache revival required pre-ship; the TLC-emission charter's
addressing contract is REQUIRED to be per-thread (VMLite/TLS-relative) — recorded now so the
charter cannot bake the GIL-phase vm-relative chain. §5.3's frozen layout/indexing and
exported offsets are unchanged.

### 24.3 Options bootstrap (cross-spec, CONFIRMED)
§14 gating now records: all five specs' OptionsList.h entries (incl. our manifest 2)+jit
M2a/M4a are orchestrator-pre-applied before fan-out; heap's private-overlay convention is
extended by name to vmstate/OM/api for their non-Options hunks. Our overlay scope narrows to
manifests 3-5, 8, 11.

### 24.4 Phase-B / perf-charter findings (ACKNOWLEDGED, no heap change)
Thread-granular STW (one VM, N threads) remains the vmstate Dev-10 Phase-B charter; heap §3.8
already states the §10 barrier is thread-granular and only VMM trap delivery/jit R1
arbitration are VM-granular. jit Task-13's integration gate now labels which config it
validates; api §2 records the charter as a hard GIL-removal precondition. Deviation 4/7
disclosures (sync GC, single-MSPL) stand as the phase-1 contract; api §2 re-scopes the
composed deliverable accordingly.

### 25. Round-7 amendment: I12 redefined around the window-witness root set (Wlr made normative)
The previous I12 clause "∪ each client's `m_currentBlock` cells" was strictly NARROWER than
the load-bearing mechanism the tree depends on: the §10 under-marking corruption cohort
lives in free-list-consumed blocks and precise allocations, not in `m_currentBlock` alone
(EVIDENCE.md §10 Experiment B, §11-§12). An implementation satisfying the old I12 literally
would still corrupt. I12 now defines the conducted-cycle root set as scan-roots ∪ the
window-witness set — (a) version-current NA cells of ALL stopped blocks, (b) all cells of
directory-allocated blocks, (c) NA precise allocations — closed under tracing inside the
marking fixpoint as the "Wlr" core marking constraint, with mark-without-trace explicitly
forbidden (the i03 zombie-dictionary-Structure regression proved it unsound: a retained but
untraced Structure stayed findable in the transition WeakGCHashTable with a swept
PropertyTable). The L1/L2 publication/initialization lemmas (round-6 F2 documentation debt)
and the over-retention semantics (eden-sticky marks; full-collection whole-consumed-block
retention; ≤ 2-full-collection float) are absorbed as normative. The header-lock witness
read protocol (round-7 F1; MarkedBlock::sharedGCWindowWitnessSnapshot) is required: the
prior lock-free reads raced aboutToMarkSlow's clear/fold/version writes. Gate note: the
Wlr gate keys on sticky ISS (NOT gilOff) — ruled correct, the window hole is a property of
shared conduction (round-7 F5, EVIDENCE.md §14).
