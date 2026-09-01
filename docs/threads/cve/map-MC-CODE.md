# map-MC-CODE — Code publication/invalidation vs concurrent execution

Mechanism class (CVE-AUDIT.md "MC-CODE", merged JVM-6 + JVM-7 + nmethod-sweeper family):
instruction bytes or code-entry state mutated while another core may fetch/execute them —
IC/call-site patching, deopt racing the executing thread, freeing compiled code a stack
still returns into. Known-good fix shape: ONE funnel (safepoint/handshake + entry
barriers) plus i-cache ordering barriers (ISB-class on arm64).

Audit date: **2026-06-18 re-audit (B5)**, tree `jarred/threads` (post thread-closeout +
TSAN-TRIAGE §17 closure + AB18-D + B5 precondition-10 mechanism); supersedes the
2026-06-15 audit. Governing design: SPEC-jit.md §5.1/§5.3/§5.6/§5.8, invariants
I2/I3/I7/I8/I16/I21, F5/F6, R1/R2; UNGIL-HANDOUT §A.3 + ANNEX ISB1; SPEC-heap §10/§11
(epoch, scan); TSAN-TRIAGE.md §17.2 (retired-artifact dealloc enumeration).

Our architecture deliberately *retires* most cross-modifying code instead of fencing it:
flag-on, property ICs and call links become data-only records (no code patching at all),
and the residual code-mutating operations (jettison, Class-A watchpoint fires, jump
replacements) are funneled through exactly one primitive
(`JSThreadsSafepoint::stopTheWorldAndRun`, SPEC-jit R1). That is precisely the HotSpot
fix shape. The audit below is therefore mostly "verify the funnel has no bypass".

**Delta vs 2026-06-15:** S6 (precondition 10) MECHANISM is LANDED — the claim CAS
is the release-publish of the deferred-fire fact, and
`DeferredWatchpointFire::fireEarlyForGILOff` is the eager-fire entry point that
closes the publication-before-fire window; the three named consumer reference sites
(Watchpoint.cpp drain re-check, CallLinkInfo.cpp publishRecord,
ConcurrentButterflyOperations.cpp R3 shims) were audited in-tree and none requires
a separate acquire-load (all ride the §A.3 stop barrier or are not deferred-fire-fact
consumers). S8 spawned-arm CLOSED — tripwire wired at `attachSpawnedThreadGCClient`
(ThreadManager.cpp) behind `useThreadGILOffUnsafe` (CVE-B6); carrier non-main arm
(JSLock.cpp `perThreadClientForCarrierEntry`) is the recorded second wiring site,
residual. (Prior delta: S4 falsified-then-fixed by §17.2 rows
16/17/18; S7/precondition 11 landed by AB18-D; S1's I21(b) cite struck.)

---

## S1 — Jettison/deopt of code another thread executes (JVM-7, "not_entrant races")

Surface:
- `Source/JavaScriptCore/bytecode/CodeBlock.cpp:2649` —
  `RELEASE_ASSERT(!Options::useJSThreads() || reason == Profiler::JettisonDueToOldAge || JSThreadsSafepoint::worldIsStopped(vm))`
  at the top of the jettison body; `CodeBlock.cpp:2852-2885` routes every flag-on
  gilOff jettison with `reason != JettisonDueToOldAge` through
  `parkSitePollAndParkForStopTheWorld` + `stopTheWorldAndRun` (under a
  `PureCodeLifecycleStopWindowScope` + `ClassAStopWatchdogContext`) so callers
  never need their own stop (SPEC-jit §5.3 choke point).
- Parked-mutator resume safety: `Options.cpp:920` forces `usePollingTraps` under
  `useJSThreads` (SPEC-jit I21(a) — async breakpoint patching would be an I2
  violation; gates verified independently in map-MC-AINT S1).

Governing invariants: SPEC-jit §5.3, I2, I8, I21, R1.

Verdict: **immune-by-construction** (the funnel is intact and asserted). Adversarial
caveats examined:
- The `JettisonDueToOldAge` exemption (I8) runs un-stopped. Why this cannot be the
  HotSpot not_entrant race: the old-age sweep only retires cold code the GC has
  already proved unreachable (no frame returns into it — conservative scan R2 ran
  first) and never rewrites still-reachable optimized code; the actual *free* is
  still gated by S4 below.
- `worldIsStopped(vm)` has a VM-less weaker form used by the assert at the two
  DFG patch sites (`dfg/DFGCommonData.cpp:81` `invalidateLinkedCode`,
  `dfg/DFGJumpReplacement.cpp:40` `JumpReplacement::fire`,
  via `JSThreadsSafepoint::assertPatchingIsSafe()`). Assert-only by design;
  *safety* comes from callers being reached only from inside §5.3/§5.6 closures.
  Existing coverage: `JSTests/threads/jit/int-gate-jettison-vs-execute.js`,
  `int-gate-fire-vs-execute.js` (Task-13 integration gate, re-run at M4/CS2).
- **CORRECTION (carried from CVE-AUDIT-STATUS item 13 / map-MC-AINT S4):** the
  2026-06-07 audit cited I21(b) — "every DFG/FTL poll is followed by an
  invalidation point, so a parked mutator resumes into the patched exit, never
  across jettisoned elided code" — as part of S1's immunity. That premise is
  presently **unverified by lint** (`validateButterflyTagDiscipline` / the
  Task-13 poll-placement check is a stub, `runtime/OptionsList.h:688`). The
  resume-past-elided-fact concern is tracked under MC-AINT S4 / MC-JIT S2, not
  MC-CODE; S1's own claim (no machine-code patching outside STW) stands on
  `CodeBlock.cpp:2649` + `DFGCommonData.cpp:81` + `DFGJumpReplacement.cpp:40`
  alone and does not depend on I21(b).

## S2 — Class-A watchpoint fire funnel (deopt racing the executing thread)

Surface:
- `Source/JavaScriptCore/bytecode/Watchpoint.cpp:396` — `fireAllSlow(VM&, FireDetail)`
  branch (1) fires inline iff `worldIsStopped(vm)` (with the
  `AlreadyStoppedWorldWitnessScope` tripwire + closing-edge
  `crossModifyingCodeFence`), else branch (2) requests `stopTheWorldAndRun` under
  a `ClassAStopWatchdogContext` (SPEC-jit §5.6; fires synchronous-complete,
  I10/I11 idempotence re-check inside the stop).
- D1R rebias fires (UNGIL ANNEX D1R) ride heap §10 stops; jit §5.6's
  `worldIsStopped` already includes the `worldIsStoppedForAllClients()` disjunct.

Verdict: **immune-by-construction** for the non-deferred path: every
code-invalidating fire reaches the one funnel, P2 makes non-owned sets default
Class A, and coalescing (§5.6 r10) keeps concurrent fires single-stop.

**EXCEPT** the deferred overload — see S6.

## S3 — i-cache / cross-modifying-code ordering (hotspot-cmc, AArch64 deopt-trap analog)

Surface:
- Patcher side: `crossModifyingCodeFence` before resume —
  `bytecode/JSThreadsSafepoint.cpp` (stub form), `runtime/VMManager.cpp:614`
  (nested patch), `:824` (conductor, before the ISB1.1 bump and the stop-word
  clear). SPEC-jit F5.
- Consumer side, NVS exits: unconditional per-mutator ISB on every
  notifyVMStop/ticket-park exit (R1.d/ISB1).
- Consumer side, NON-NVS re-entries (the hole the AArch64 exemplar lives in:
  a thread sleeping *access-released* through the stop never executes the NVS
  exit ISB): UNGIL ANNEX ISB1 (§A.3.2c) — process-wide seq_cst stop-generation
  counter bumped inside every patching window
  (`runtime/VMManager.cpp:825` `jsThreadsBumpStopGeneration`; shared-GC
  conductor likewise), compared on every may-execute-JIT transition (AHA
  re-acquisition, token acquisition, ACT, DAL2 dtor, LIFO restore — all
  funneled through `acquireHeapAccess`'s success path), with
  `crossModifyingCodeFence` on mismatch BEFORE any JIT entry. Implementation:
  `runtime/VMLite.cpp:797-824`
  (`jsThreadsBumpStopGeneration` / `jsThreadsSyncToStopGenerationBeforeJITEntry`;
  recorded deviation: per-thread `thread_local` copy instead of per-lite — a
  strict refinement, since an ISB synchronizes the executing PE).
  Visibility argument: the bump is sequenced before the conductor's seq_cst
  stop-word clear, which the re-acquirer's §A.3.2b seq_cst load must observe
  before it can reach JIT code.

Verdict: **needs-test**. The protocol is sound on paper and adversarial review
already found-and-closed the sleeper hole (ISB1 supersedes jit F5's
NVS-exit-only delivery), but the chartered exercise (ISB1 item 6, "U-T5 arm")
has its only coverage in `JSTests/threads/cve/mc-code-sleep-through-jettison-isb.js`
(deterministic rendezvous; the failure mode itself is arm64-hardware +
amplifier territory, so the test is also amplifier-ready). Unchanged from
2026-06-07.

## S4 — Freeing compiled code a stack still returns into (nmethod sweeper UAF)

The 2026-06-07 verdict was **FALSIFIED** by the TSAN-TRIAGE §17.2 retired-artifact
dealloc-path enumeration (closeout review): rows 16, 17 and 18 were real bypasses
of the §4.4/R2 funnel. All three landed fixes; the verdict is **re-established as
immune-by-construction for the CURRENT (fixed) tree**, with the §17.2 18-row table
now the governing proof, not the 2026-06-07 prose.

Surface (load-bearing facts in the fixed tree):
- **Leak invariant (the strong proof).** `bytecode/RetiredJITArtifacts.cpp:97`
  `epochCoversEveryJSThread` returns false flag-on, so EVERY
  `RetiredJITArtifacts` path (`retireHandlerChain`, `retire`,
  `retireOptimizedJITCode` — `RetiredJITArtifacts.cpp:214,260`) takes the
  leak-until-integration arm. Quarantine = "never freed", not "freed after
  epoch+1". Any genuine premature-free must therefore BYPASS RetiredJITArtifacts.
- **R2 scan + machine-code rule.** `heap/Heap.cpp` —
  `deleteUnmarkedJettisonedStubRoutines` runs only after `gatherStackRoots`,
  whose `MachineThreads::gatherConservativeRoots` (`heap/MachineStackMarker.cpp`)
  scans every registered thread's stack and registers including parked /
  access-released lites (heap §10.2/§10.4; UNGIL ANNEX EXIT1 registry walk).
  SPEC-jit R2/I7/G1 + §4.4 hard rule: epoch expiry NEVER frees machine code
  (`RetiredJITArtifacts.cpp` `RELEASE_ASSERT(routine->isGCAware())`).
- **Row 16 fix (MetadataTable bypass — was a real UAF window).**
  `bytecode/CodeBlock.cpp:991,1149` — flag-on `~CodeBlock` ref-escapes
  `m_metadata` (leak), keeping the MetadataTable, its embedded
  `DataOnlyCallLinkInfo`s, and their §5.8 records alive for straggler prologue
  reloads. Before this fix the destructor freed published records inline on
  exactly the straggler window the AB18-B `m_jitData` leak in the SAME
  destructor exists for — a direct nmethod-sweeper analog.
- **Row 17 fix (CodeBlock cell recycling — the IsoSubspace re-hand-out race).**
  `heap/CodeBlockSet.cpp:51-82`
  `clearCurrentlyExecutingAndRemoveDeadCodeBlocks` now unlinks every dead
  block's incoming calls flag-on, in the End phase, world-stopped. Before the
  fix a sibling could call through a still-linked CallLinkInfo AFTER resume,
  acquiring the dead cell pointer AFTER the conservative scan (hence unpinned),
  then race lazy-sweep + IsoSubspace re-hand-out and bind the prologue
  `loadPairPtr(offsetOfJITData, offsetOfMetadataTable)` to the NEW occupant's
  fields (silent wrong-metadata). With the CLI acquisition window closed inside
  the stop, any thread still able to enter a dead block post-resume must have
  held the pointer at scan time → conservatively marked → not dead
  (contradiction proof).
- **Row 18 fix (OpCatch ValueProfileBuffer bypass).** `bytecode/CodeBlock.cpp` —
  the `ValueProfileAndVirtualRegisterBuffer::destroy` in `~CodeBlock`'s
  baseline arm is skipped flag-on (buffer leaks with the row-16 ref-escaped
  metadata); previously a straggler that threw and landed in `op_catch` read
  the freed buffer.
- Row 7 correction (DFG `m_jitData` was nulled before the flag split → near-null
  store/load on the kept-alive JITData) is also landed.

Verdict: **immune-by-construction (fixed tree).** The 18-row enumeration
(TSAN-TRIAGE §17.2) is exhaustive: every deallocation of a *published*
artifact routes through RetiredJITArtifacts (leak) or is leaked in
`~CodeBlock` (rows 7/8/11/16/18) or is unreachable flag-on; the row-17
End-phase unlink closes the only post-scan acquisition vector. **Re-audit
obligation:** all 18 rows must be re-walked when `epochCoversEveryJSThread`
starts returning true flag-on — the leak invariant is the load-bearing fact,
and turning the epoch on REMOVES it. Existing coverage:
`JSTests/threads/jit/int-gate-epoch-reclaim.js`,
`JSTests/threads/heap-epoch-reclaim.js`.

## S5 — IC publish/reset (the classic "IC patching" leg)

Surface:
- Flag-on, property ICs are data-only: `RepatchingPropertyInlineCache`
  construction is a release assert (SPEC-jit I3), and the one residual
  code-patching path `rewireStubAsJumpInAccess` is gated by
  `assertPatchingIsSafe` (`bytecode/PropertyInlineCache.cpp`).
- Inline self-access publish = one packed 64-bit `m_packedSelfWord` store;
  invalidation = all-zero store, ABA-safe (SPEC-jit §5.1). Single-word ⇒ no
  torn structureID/offset pair.
- Handler-chain publish: `storeStoreFence` before head store; readers
  address-depend through the head (F2); reset replaces the head with the
  slow-path handler (fenced) then `retireHandlerChain` — never an inline free.
- Writers serialized: `addAccessCase`/`InlineCacheCompiler` under `m_lock`
  (`GCSafeConcurrentJSLocker`).

Verdict: **immune-by-construction** (readers race only against single-word or
fenced+address-dependent publications; frees go through S4's leak/scan gates).
Existing coverage: `JSTests/threads/jit/ic-publish-reset-loops.js`. Residual
dependence flagged: F2's reader side relies on address-dependency (consume)
ordering — fine on arm64/x86 for a dependent load through the head pointer.

## S6 — Deferred Class-A fire: watched fact published BEFORE invalidation lands

Surface:
- `bytecode/Watchpoint.cpp:412-485` — `fireAllSlow(VM&, DeferredWatchpointFire*)`.
  The B-relabelrace claim CAS (lines 463-476: `m_state.compareExchangeStrong(
  IsWatched, IsInvalidated)`) LANDED — exactly one racer claims the membership
  transfer, losers return with `ClearWatchpoint` (no scope-exit fire). That
  closes writer-writer on the deferral overload itself.
- **The ORDERING window remains open** (caveat at `Watchpoint.cpp:424-440`):
  a deferring caller COMPLETES its watched-fact mutation (publishes a new
  structureID into objects) BEFORE the scope-exit fire's stop lands. Under N
  mutators, another thread's optimized code that elided a check on this set
  executes against the already-false fact until the stop. THREAD.md forbids
  this. Deferring sites: Task-11 audit rows, e.g. `runtime/Structure.cpp`
  transition-set deferred form.

Mechanism match: this is exactly "deopt racing the executing thread".

Verdict: **mechanism LANDED (B5); per-site adoption is the residual.**
Precondition 10's protocol is now fixed in `bytecode/Watchpoint.{h,cpp}`:
- **(1) Release-publish of the deferred-fire fact.** The claim CAS at
  `Watchpoint.cpp` `fireAllSlow(VM&, DeferredWatchpointFire*)` flag-on arm
  (`m_state.compareExchangeStrong(IsWatched, IsInvalidated)`, seq_cst hence
  release) is the single release point at which the SOURCE set becomes
  observably invalidated, and runs strictly BEFORE any caller publishes its
  watched-fact mutation.
- **(2) Eager-fire entry point.**
  `DeferredWatchpointFire::fireEarlyForGILOff(VM&, const FireDetail&)`
  (`bytecode/Watchpoint.h` / `.cpp`): gilOff callers invoke it AFTER dropping
  the locks that motivated deferral but BEFORE publishing (the
  setStructure/structureID store). gilOff Class-A pending → runs the full
  Class-A stop+fire NOW, jettisoning every CodeBlock that watched the source
  set BEFORE another mutator can act on the about-to-be-published fact; the
  dtor's scope-exit fire is then a no-op. Flag-off / GIL-on / Class-B: no-op,
  so the adapt-after-publish ordering stays byte-identical. Adaptive
  watchpoints fire pre-publish gilOff and take their conservative path
  (cannot re-install on the still-old structure) — recorded gilOff cost,
  correctness > re-adapt.
- **(3) Consumer audit (the three named reference sites).** All three were
  checked in-tree with B5 audit comments landed at each:
  `Watchpoint.cpp` `drainClassAFireQueue` state() re-check — runs
  world-stopped, the §A.3 stop barrier is the ordering edge, no separate
  acquire needed. `CallLinkInfo.cpp` `publishRecord` — NOT a deferred-fire-fact
  consumer (F2 address-dependent reader; jettison-time unlink reaches it under
  the §A.3 stop); references gilRemovalPreconditionsMet for precondition 11.
  `ConcurrentButterflyOperations.cpp` R3 shims — NOT a deferred-fire-fact
  consumer (regime-2 OM forwarders; references gilRemovalPreconditionsMet for
  precondition 9). No site requires an explicit acquire-load of the
  deferred-fire fact.

The Task-11 audit's "fact published before fire?" column now records, per
deferring site, which of (b) published-inside-stop / (c) re-checked
dynamically / **(d) eager-fire-via-fireEarlyForGILOff** applies; (a)
single-mutator-only is no longer admissible gilOff. Gate test
(amplifier-ready stale-window detector, ASAN strongest):
`JSTests/threads/cve/mc-code-deferred-fire-stale-window.js` — flips XFAIL→PASS
once the (d) call is wired at every deferring site that publishes after the
SAL scope (the JSObject/JSObjectInlines transition writers).

## S7 — Call-site publication: readers vs writers (call-site patching leg)

Reader side (unchanged):
- Flag-on, call links are data records (SPEC-jit §5.8/F6): fast path loads
  `m_record` once and reads comparand/target THROUGH it (F2); publish is
  `new CallLinkRecord` → `storeStoreFence` → pointer store
  (`bytecode/CallLinkInfo.cpp:120` `publishRecord`); the three `UseDataIC::No`
  direct-call sites are flipped and `DirectCallLinkInfo::repatchSpeculatively`
  is `RELEASE_ASSERT(!Options::useJSThreads())`. I16 forbids a poll between the
  `m_record` load and the call.
- Verdict (readers): **immune-by-construction**; existing coverage
  `JSTests/threads/jit/int-gate-direct-call-relink.js`.

Writer side — **AB18-D LANDED (precondition 11 closed)**:
- `m_record` is now atomic: every swap is `m_record.exchange()`
  (`bytecode/CallLinkInfo.cpp:132,147,219,228,862,873`) — no two writers can
  observe the same oldRecord; a losing linker retires its OWN record.
- Every transition writer serializes on the single process-wide
  `CallLinkInfo::s_callLinkSerializationLock` (RecursiveLock,
  `bytecode/CallLinkInfo.h:153`, `CallLinkInfo.cpp:92`): `linkMonomorphicCall`
  (`bytecode/Repatch.cpp:107`, with `isLinked() || stub()` lost-race early
  return at `:108-109`), `linkPolymorphicCall` (`Repatch.cpp:366`),
  `linkDirectCall` (`Repatch.cpp:2526`), `setVirtualCall` self-locks for
  unlocked callers (`CallLinkInfo.cpp:574-576` — covers `RepatchInlines.h
  linkFor` and `linkSlowFor`), `unlinkOrUpgradeImpl` (`CallLinkInfo.cpp:266`),
  `~CallLinkInfo` (`CallLinkInfo.cpp:210`), DirectCallLinkInfo destruction
  (`CallLinkInfo.cpp:915`). The same lock serializes the SentinelLinkedList
  `linkIncomingCall` push/remove (the prev/next-corruption signature) and the
  `m_callee`/`m_codeBlock`/`m_mode`/`setLastSeenCallee` mirror writes.
  Per-CodeBlock locking was REJECTED in review (covered one writer pair out of
  six; risked caller/callee ABBA between mutually-recursive CodeBlocks).
- Publication ordering inside `setVirtualCall` (`CallLinkInfo.cpp:578-607`):
  payload (codeBlock, destination) stored before `storeStoreFence` before the
  `polymorphicCalleeMask` mirror store (relaxed-atomic) before `publishRecord`.

Verdict (writers): **immune-by-construction.** Adversarial residue examined:
- The lock is process-wide → scalability concern only, not a safety hole.
- Lock rank: §7 forbids holding any lock below GCL across STWR. The locked
  bodies (`linkMonomorphicCall`/`linkPolymorphicCall`/`linkDirectCall`/
  `setVirtualCall`) make no STWR/safepoint call (DeferGC at
  `linkPolymorphicCall` entry); `unlinkOrUpgradeImpl` runs inside the
  End-phase / jettison stop (row 17) or under the same lock from a slow path
  with no inner stop. No deadlock vector found.
- `Repatch.cpp:263` documents the one cross-domain read NOT covered by the
  lock (`ScriptExecutable` retract-first mirror) and re-derives the
  authoritative entrypoint through the per-variant CodeBlock snapshot — a
  read-side accommodation, not a writer race.
- Test `JSTests/threads/cve/mc-code-calllink-writer-writer.js` is now a
  REGRESSION test (was EXPECTED-FAIL pre-AB18-D; CVE-AUDIT-STATUS row should
  drop the EXPECTED-FAIL tag).

Adjacent FIXED item (CVE-AUDIT-STATUS 2026-06-10 #2): the FTL DirectTailCall
data-IC register clobber was initially mistriaged as an MC-CODE epoch race;
root-caused as a single-threaded codegen bug (B3 assigned callee/tail-args to
`callLinkInfoGPR`; record pointer not live across the CallFrameShuffler).
Mechanism is NOT cross-modifying code; recorded here only because the symptom
(wild `farJump` through a clobbered record pointer) impersonated S7. Fix +
pinned regression test
`JSTests/threads/jit/ftl-direct-tailcall-dataic-arg-clobber.js`.

## S8 — Meta-finding: the mechanical tripwire — WIRED, spawned arm (CVE-B6)

`JSThreadsSafepoint::gilRemovalPreconditionsMet()`
(`bytecode/JSThreadsSafepoint.h`) is constexpr false and
`INTEGRATE-jit.md:2363-2369` requires the GIL-removal change to gate
second-mutator attach on `RELEASE_ASSERT(gilRemovalPreconditionsMet())`. The
tripwire is now **WIRED** at the gilOff **spawned-thread** second-mutator
attach point:

    runtime/ThreadManager.cpp attachSpawnedThreadGCClient():
    RELEASE_ASSERT(JSThreadsSafepoint::gilRemovalPreconditionsMet()
                   || Options::useThreadGILOffUnsafe());

`attachSpawnedThreadGCClient` is the §B.1 point where a spawned thread
constructs its OWN `GCClient::Heap` and acquires first heap access — i.e. the
exact instant a second concurrent JS mutator becomes live. The bring-up
override is `useThreadGILOffUnsafe` (the same flag U0 option validation already
requires to admit gilOff at all), so the ladder keeps running unchanged today;
a production build that flips gilOff WITHOUT the override fail-stops here
rather than running the documented GIL-sound-only gaps with N mutators.
Unreachable flag-off (`vm.gilOff()` asserted immediately above; spawned threads
are U0b-gated to the m_gilOff winner VM only).

**RESIDUAL — second wiring site (out of B6 file scope):** the §F.1/§B.2
carrier non-main arm at `runtime/JSLock.cpp perThreadClientForCarrierEntry`
(the `new GCClient::Heap(vm.heap)` for `!isMainThread`, reached from
`ensureCarrierLiteForCurrentThread` under `ASSERT(vm.gilOff())`) is ALSO a
gilOff concurrent-mutator admission point per the ANNEX A36C / §F.1 design and
MUST carry the identical assert. With only the spawned arm wired, a future
commit that relaxes the U0 option-validation gate without flipping the
predicate would fail-stop spawned attach but silently admit a non-main carrier
as a concurrent mutator. The carrier wiring lands with the JSLock owner or the
GIL-removal commit; recorded here so S8 is not read as fully closed.

**Tautology note (intentional):** because `vm.gilOff()` is reachable today only
when U0 option validation (`Options.cpp`) passed — which itself requires
`useThreadGILOffUnsafe` — the wired assert cannot fire under any present
configuration. The tripwire's teeth depend on the U0 gate and the override flag
being retired TOGETHER with the predicate flip; it is NOT independently
load-bearing today. This is by design (future-tripwire).

The three comment-only references (`Watchpoint.cpp` precondition-10 ORDERING,
`CallLinkInfo.cpp` AB18-D, `ConcurrentButterflyOperations.cpp` precondition 9)
remain as precondition trackers. Outstanding preconditions the predicate must
guard before flipping true: 1, 2, 3, 9 plus the precondition-10 per-site
Task-11 column. The GIL-removal change flips the constant true, retires the
override flag, AND wires the carrier non-main site in the SAME commit.

Verdict: **CLOSED — spawned arm** (process hole; tripwire wired behind bring-up
override). **Carrier non-main arm: residual wiring site** (JSLock.cpp).

---

## Summary table

| # | Surface | Spec anchor | Verdict (2026-06-15) | Δ vs 06-07 |
|---|---------|------------|---------|---|
| S1 | CodeBlock::jettison / DFG invalidate+jump-replacement | SPEC-jit §5.3, I2/I8/I21 | immune-by-construction | I21(b) cite struck (→ MC-AINT S4) |
| S2 | Class-A watchpoint fire funnel | SPEC-jit §5.6, P2, I10/I11 | immune-by-construction (non-deferred) | none |
| S3 | i-cache ordering incl. access-released sleepers | F5/R1.d + UNGIL ANNEX ISB1 | needs-test → `mc-code-sleep-through-jettison-isb.js` | none |
| S4 | Freeing jettisoned code/data (nmethod sweeper) | R2/I7/G1, §4.4 + TSAN-TRIAGE §17.2 | immune-by-construction (fixed tree) | **was falsified; rows 16/17/18 fixed** |
| S5 | Property-IC publish/reset | §5.1/§4.4, F2, I3/I16 | immune-by-construction | none |
| S6 | Deferred Class-A fire fact ordering | §5.6 + precondition 10 | **mechanism LANDED (B5)** — release claim-CAS + `fireEarlyForGILOff`; consumer sites audited (ride §A.3 stop barrier) → `mc-code-deferred-fire-stale-window.js` | **claim CAS + eager-fire mechanism + 3-site consumer audit landed**; per-site (d) wiring is the residual |
| S7 | Call-link writers (publishRecord double-retire) | §5.8/F6 + precondition 11 | **immune-by-construction (AB18-D)** → `mc-code-calllink-writer-writer.js` (regression) | **was suspected; LANDED** |
| S8 | gilRemovalPreconditionsMet tripwire | INTEGRATE-jit.md tripwire contract | **CLOSED spawned arm (CVE-B6)** — wired at `attachSpawnedThreadGCClient` behind `useThreadGILOffUnsafe`; **residual**: carrier non-main arm (`JSLock.cpp perThreadClientForCarrierEntry`) second wiring site | **spawned wired; carrier residual** |

All three tests carry `//@` headers; S3 detection is arm64-hardware strongest;
S6 detection is strongest under ASAN + the race amplifier. S4's re-audit is a
**hard obligation** the day `epochCoversEveryJSThread` flips true.
