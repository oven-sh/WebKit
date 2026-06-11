# map-MC-CODE — Code publication/invalidation vs concurrent execution

Mechanism class (CVE-AUDIT.md "MC-CODE", merged JVM-6 + JVM-7 + nmethod-sweeper family):
instruction bytes or code-entry state mutated while another core may fetch/execute them —
IC/call-site patching, deopt racing the executing thread, freeing compiled code a stack
still returns into. Known-good fix shape: ONE funnel (safepoint/handshake + entry
barriers) plus i-cache ordering barriers (ISB-class on arm64).

Audit date: 2026-06-07, tree `jarred/threads` (UNGIL-HANDOUT rev 32 era; gilOff
bring-up machinery live, `gilRemovalPreconditionsMet()` still constexpr false).
Governing design: SPEC-jit.md §5.1/§5.3/§5.6/§5.8, invariants I2/I3/I7/I8/I16/I21,
F5/F6, R1/R2; UNGIL-HANDOUT §A.3 + ANNEX ISB1; SPEC-heap §10/§11 (epoch, scan).

Our architecture deliberately *retires* most cross-modifying code instead of fencing it:
flag-on, property ICs and call links become data-only records (no code patching at all),
and the residual code-mutating operations (jettison, Class-A watchpoint fires, jump
replacements) are funneled through exactly one primitive
(`JSThreadsSafepoint::stopTheWorldAndRun`, SPEC-jit R1). That is precisely the HotSpot
fix shape. The audit below is therefore mostly "verify the funnel has no bypass".

---

## S1 — Jettison/deopt of code another thread executes (JVM-7, "not_entrant races")

Surface:
- `Source/JavaScriptCore/bytecode/CodeBlock.cpp:2388` —
  `RELEASE_ASSERT(!Options::useJSThreads() || reason == Profiler::JettisonDueToOldAge || JSThreadsSafepoint::worldIsStopped(vm))`
  at the top of the jettison body; `CodeBlock.cpp:2548` routes every flag-on
  jettison with `reason != JettisonDueToOldAge` through `stopTheWorldAndRun` so
  callers never need their own stop (SPEC-jit §5.3 choke point).
- Parked-mutator resume safety: `Options.cpp:920` forces `usePollingTraps` under
  `useJSThreads` (SPEC-jit I21 — async breakpoint patching would be an I2
  violation), and every DFG/FTL poll is followed by an invalidation point, so a
  mutator parked at a poll resumes into the patched exit, never across
  jettisoned elided code.

Governing invariants: SPEC-jit §5.3, I2, I8, I21, R1.

Verdict: **immune-by-construction** (GIL-on today; post-ungil the same asserts hold
against the real §A.3 stop), with one adversarial caveat examined:
- The `JettisonDueToOldAge` exemption (I8) runs un-stopped. Why this cannot be the
  HotSpot not_entrant race: the old-age sweep only retires cold code the GC has
  already proved unreachable (no frame returns into it — conservative scan R2 ran
  first) and never rewrites still-reachable optimized code; the actual *free* is
  still gated by S4 below.
- `worldIsStopped(vm)` has a VM-less weaker form used by the assert at the two
  DFG patch sites (`dfg/DFGCommonData.cpp:81` `invalidateLinkedCode`,
  `dfg/DFGJumpReplacement.cpp:40` `JumpReplacement::fire`,
  via `JSThreadsSafepoint::assertPatchingIsSafe()`,
  `bytecode/JSThreadsSafepoint.h:141-155`). It is assert-only by design
  (header :96-101); the *safety* comes from the callers being reached only from
  inside §5.3/§5.6 closures. Existing coverage:
  `JSTests/threads/jit/int-gate-jettison-vs-execute.js`,
  `int-gate-fire-vs-execute.js` (chartered SPEC-jit Task 13 integration gate,
  re-run at M4/CS2).

## S2 — Class-A watchpoint fire funnel (deopt racing the executing thread)

Surface:
- `Source/JavaScriptCore/bytecode/Watchpoint.cpp:215-247` — `fireAllSlow` branch
  (1) fires inline iff `worldIsStopped(vm)` (with the
  `AlreadyStoppedWorldWitnessScope` tripwire + closing-edge
  `crossModifyingCodeFence`), else branch (2) requests
  `stopTheWorldAndRun` under a `ClassAStopWatchdogContext` (SPEC-jit §5.6;
  fires synchronous-complete, I10/I11 idempotence re-check inside the stop).
- D1R rebias fires (UNGIL ANNEX D1R) ride heap §10 stops; jit §5.6's
  `worldIsStopped` already includes the `worldIsStoppedForAllClients()` disjunct.

Verdict: **immune-by-construction** for the non-deferred path: every
code-invalidating fire reaches the one funnel, P2 makes non-owned sets default
Class A, and coalescing (§5.6 r10) keeps concurrent fires single-stop.

**EXCEPT** the deferred overload — see S6 (susceptible-suspected, recorded).

## S3 — i-cache / cross-modifying-code ordering (hotspot-cmc, AArch64 deopt-trap analog)

Surface:
- Patcher side: `crossModifyingCodeFence` before resume —
  `bytecode/JSThreadsSafepoint.cpp:199,348` (stub form),
  `runtime/VMManager.cpp:534` (nested patch), `:646` (conductor, before the
  ISB1.1 bump and the stop-word clear). SPEC-jit F5.
- Consumer side, NVS exits: unconditional per-mutator ISB on every
  notifyVMStop/ticket-park exit — `runtime/VMManager.cpp:427,493` (R1.d/ISB1).
- Consumer side, NON-NVS re-entries (the hole the AArch64 exemplar lives in:
  a thread sleeping *access-released* through the stop never executes the NVS
  exit ISB): UNGIL ANNEX ISB1 (§A.3.2c) — process-wide seq_cst stop-generation
  counter bumped inside every patching window
  (`runtime/VMManager.cpp:642`; shared-GC conductor likewise), compared on every
  may-execute-JIT transition (AHA re-acquisition, token acquisition, ACT, DAL2
  dtor, LIFO restore — all funneled through `acquireHeapAccess`'s success path),
  with `crossModifyingCodeFence` (ISB on arm64, serializing instruction on
  x86-64) on mismatch BEFORE any JIT entry. Implementation:
  `runtime/VMLite.cpp:771-840`
  (`jsThreadsBumpStopGeneration` / `jsThreadsSyncToStopGenerationBeforeJITEntry`;
  recorded deviation: per-thread `thread_local` copy instead of per-lite — a
  strict refinement, since an ISB synchronizes the executing PE).
  Visibility argument: the bump is sequenced before the conductor's seq_cst
  stop-word clear, which the re-acquirer's §A.3.2b seq_cst load must observe
  before it can reach JIT code.

Verdict: **needs-test**. The protocol is sound on paper and adversarial review
already found-and-closed the sleeper hole (ISB1 supersedes jit F5's
NVS-exit-only delivery), but the chartered exercise (ISB1 item 6, "U-T5 arm:
conductor jettisons during a stop while a thread sleeps access-released through
it; the sleeper re-enters via AHA and executes the patched region") has **no
test in JSTests/threads** (grep for sleep-through/ISB/stop-generation: none).
Test written: `JSTests/threads/cve/mc-code-sleep-through-jettison-isb.js`
(deterministic rendezvous; the failure mode itself is arm64-hardware +
amplifier territory, so the test is also amplifier-ready).

## S4 — Freeing compiled code a stack still returns into (nmethod sweeper UAF)

Surface:
- `heap/Heap.cpp:1424` — `deleteUnmarkedJettisonedStubRoutines` runs only after
  `gatherStackRoots` (`Heap.cpp:1022-1045`), whose
  `MachineThreads::gatherConservativeRoots`
  (`heap/MachineStackMarker.cpp:239`) scans every registered thread's stack and
  registers — including parked threads (heap §10.2/§10.4; the entered set is the
  VMLiteRegistry walk, UNGIL ANNEX EXIT1, which also covers access-released
  lites whose stacks still bear JIT return addresses). SPEC-jit R2/I7/G1.
- Epoch reclamation can NEVER free machine code: SPEC-jit §4.4 hard rule;
  `bytecode/RetiredJITArtifacts.cpp:144-159` `RELEASE_ASSERT(routine->isGCAware())`
  — expired handler chains drop their `Ref<GCAwareJITStubRoutine>`s into the
  jettison machinery, whose `ExecutableMemoryHandle`s are released only on the
  GC sweep after R2's scan.
- Retired *data* (handler nodes, IC metadata) is freed by epoch expiry, made
  sound by I16 (no safepoint poll inside an IC fast-path window — lint'd) and
  I15 (native slow paths take `Ref<InlineCacheHandler>` across potential
  safepoints).

Verdict: **immune-by-construction**. Adversarial pushback considered: (a) a
thread that exits/detaches mid-window — EXIT1's registry lock owns sampled-set
membership for every open window, per-sample re-walks, no pointer caching;
(b) parked-but-access-released threads — kept tokens make them registry-visible
(§A.3.2/§A.3.4). Existing coverage:
`JSTests/threads/jit/int-gate-epoch-reclaim.js`,
`JSTests/threads/heap-epoch-reclaim.js`.

## S5 — IC publish/reset (the classic "IC patching" leg)

Surface:
- Flag-on, property ICs are data-only: `RepatchingPropertyInlineCache`
  construction is a release assert (SPEC-jit I3), and the one residual
  code-patching path `rewireStubAsJumpInAccess` is gated by
  `assertPatchingIsSafe` (`bytecode/PropertyInlineCache.cpp:1200`).
- Inline self-access publish = one packed 64-bit `m_packedSelfWord` store
  (`PropertyInlineCache.cpp:66`); invalidation = all-zero store (`:81`),
  ABA-safe (SPEC-jit §5.1). Single-word ⇒ no torn structureID/offset pair.
- Handler-chain publish: `storeStoreFence` before head store
  (`PropertyInlineCache.cpp:1023,1095,1138,1145,1246`); readers
  address-depend through the head (F2); reset replaces the head with the
  slow-path handler (fenced) then `retireHandlerChain` — never an inline free.
- Writers serialized: `addAccessCase`/`InlineCacheCompiler` under `m_lock`
  (`GCSafeConcurrentJSLocker`), unchanged locking.

Verdict: **immune-by-construction** (readers race only against single-word or
fenced+address-dependent publications; frees go through S4's epoch/scan
gates). Existing coverage: `JSTests/threads/jit/ic-publish-reset-loops.js`.
Residual dependence flagged: F2's reader side relies on address dependency
(consume) ordering — fine on arm64/x86 for a dependent load through the head
pointer; any future compiler transformation breaking the dependency chain is
the standard consume-ordering caveat, not a protocol hole.

## S6 — Deferred Class-A fire: watched fact published BEFORE invalidation lands

Surface:
- `bytecode/Watchpoint.cpp:285-320` — `fireAllSlow(VM&, DeferredWatchpointFire*)`:
  a lock-holding caller flips state and transfers the watchpoint list now; the
  code-invalidating fire runs at scope exit. The caller COMPLETES its
  watched-fact mutation (e.g. publishes a new structureID into objects) before
  the scope-exit stop lands. Deferring sites: Task-11 audit rows, e.g.
  `runtime/Structure.cpp:1929` (transition set, deferred form; scope-exit fire
  at `:2317`).

Mechanism match: this is exactly "deopt racing the executing thread" —
under N mutators, another thread's optimized code that elided a check on this
set executes against the already-false fact until the stop lands. THREAD.md
forbids precisely that window.

Verdict: **susceptible-suspected** (known, honestly recorded: GIL-removal
precondition 10, `docs/threads/INTEGRATE-jit.md:2334-2352`; full caveat at the
overload). Sound today only because (a) the phase-1 GIL admits a single
mutator, or (b) the mutation+fire already runs world-stopped (the OM TTL-set
pattern publishes INSIDE the stop). The required fix is chartered: classify
every deferred site (a)/(b)/(c) in the "fact published before fire?" column or
restructure onto `Structure::fireThreadLocalSetsWithChainUnderStop`.
Test written (amplifier-ready stale-window detector):
`JSTests/threads/cve/mc-code-deferred-fire-stale-window.js`.

## S7 — Call-site publication: readers vs writers (call-site patching leg)

Reader side:
- Flag-on, call links are data records (SPEC-jit §5.8/F6): fast path loads
  `m_record` once and reads comparand/target THROUGH it (F2); publish is
  `new CallLinkRecord` → `storeStoreFence` → pointer store
  (`bytecode/CallLinkInfo.cpp:111-119` `publishRecord`); unlink is a single
  monotone nullptr store (`:121-133`); the three `UseDataIC::No`
  direct-call sites are flipped and `DirectCallLinkInfo::repatchSpeculatively`
  is `RELEASE_ASSERT(!Options::useJSThreads())` (`CallLinkInfo.cpp:878`).
  Retired records go through epoch (S4). I16 forbids a poll between the
  `m_record` load and the call.
- Verdict (readers): **immune-by-construction**; existing coverage
  `JSTests/threads/jit/int-gate-direct-call-relink.js`.

Writer side:
- `CallLinkInfo::publishRecord` / `DirectCallLinkInfo::publishRecord` use a
  NON-ATOMIC `std::exchange` on the plain `m_record`, and the slow-path linkers
  (`linkMonomorphicCall`, `setVirtualCall`, `setStub`, `linkDirectCall` in
  `bytecode/Repatch.cpp:84,81,2163`) take no lock. Two threads entering the
  SAME unlinked call site's slow path can observe the SAME oldRecord and retire
  it twice ⇒ **double-delete at epoch expiry** (heap corruption); the same
  window tears the `m_callee`/`m_codeBlock`/`m_mode` mirror writes and
  `setLastSeenCallee`. Caveat comment at `CallLinkInfo.cpp:96-110`.
- Verdict (writers): **susceptible-suspected** (known, recorded: GIL-removal
  precondition 11, `INTEGRATE-jit.md:2353-2367`; chartered fix = owner
  `CodeBlock::m_lock` around the set*/link* entry points + CAS on `m_record`
  so a losing linker retires its OWN record). Test written (targets the
  simultaneous-first-link window the existing relink gate does not isolate;
  ASAN-amplifier-ready): `JSTests/threads/cve/mc-code-calllink-writer-writer.js`.

Related open item in the same leg: GIL-removal precondition 1 — the LLInt
monomorphic CALL fast path is not yet in record form (Task 7 deferred). The
writer-writer test hammers cold (LLInt-tier) call sites as well, so it
exercises that surface too once executed post-ungil.

## S8 — Meta-finding: the mechanical tripwire is not wired

`JSThreadsSafepoint::gilRemovalPreconditionsMet()`
(`bytecode/JSThreadsSafepoint.h:202-222`) is constexpr false and
`INTEGRATE-jit.md:2362-2367` requires the GIL-removal change to gate
second-mutator attach on `RELEASE_ASSERT(gilRemovalPreconditionsMet())`. As of
this audit the symbol is referenced **only in comments**
(`Watchpoint.cpp:313`, `CallLinkInfo.cpp:109`,
`ConcurrentButterflyOperations.cpp:269`) — there is no assert at any
spawn/attach/registration point (`runtime/ThreadManager.cpp`,
`runtime/ThreadObject.cpp`, VMLite registration), while the gilOff §A.3 stop
machinery is live in `VMManager.cpp`/`VMLite.cpp` and the bring-up ladder runs
real second mutators. Bring-up intentionally runs ahead of the preconditions,
but the recorded contract ("impossible to ship GIL removal ahead of these
fixes silently") is currently not mechanically enforced.

Verdict: **susceptible-suspected** (process hole, not a code race per se).
Recommendation for the bring-up loop (no Source edits from this audit): wire
the RELEASE_ASSERT at the gilOff second-mutator attach point behind a
bring-up-only override flag, so the tripwire exists the day the ladder goes
green; flip `gilRemovalPreconditionsMetValue` only in the commit that closes
preconditions 1, 2, 3, 9, 10, 11.

---

## Summary table

| # | Surface | Spec anchor | Verdict |
|---|---------|------------|---------|
| S1 | CodeBlock::jettison / DFG invalidate+jump-replacement | SPEC-jit §5.3, I2/I8/I21 | immune-by-construction (existing int-gate tests re-run at M4/CS2) |
| S2 | Class-A watchpoint fire funnel | SPEC-jit §5.6, P2, I10/I11 | immune-by-construction (non-deferred path) |
| S3 | i-cache ordering incl. access-released sleepers | F5/R1.d + UNGIL ANNEX ISB1 | needs-test → `mc-code-sleep-through-jettison-isb.js` |
| S4 | Freeing jettisoned code (nmethod sweeper) | R2/I7/G1, §4.4 hard rule | immune-by-construction |
| S5 | Property-IC publish/reset | §5.1/§4.4, F2, I3/I16 | immune-by-construction |
| S6 | Deferred Class-A fire fact ordering | §5.6 + precondition 10 | susceptible-suspected (recorded) → `mc-code-deferred-fire-stale-window.js` |
| S7 | Call-link writers (publishRecord double-retire) | §5.8/F6 + precondition 11 | susceptible-suspected (recorded) → `mc-code-calllink-writer-writer.js` |
| S8 | Unwired gilRemovalPreconditionsMet tripwire | INTEGRATE-jit.md tripwire contract | susceptible-suspected (process) |

All three tests are written but NOT executed (tree owned by the bring-up loop);
they carry `//@` headers and are designed to run post-ungil, where S3/S6/S7's
windows become real. S6/S7 detection is strongest under ASAN (double-delete /
UAF) and the race amplifier.
