# MC-SAFE — Safepoint reachability / state-at-poll mismatch

Mechanism class (web-derived, treated as data): a thread can never be brought
to a stop (poll elision, unbounded time-to-safepoint, watchdog DoS), or stops
with frame/oop-map state misdescribing reality so the stopped-world operation
walks wrong state, or handshake-ack does not equal quiescence. Exemplars:
JDK-8161147 (counted-loop safepoint elision family), JDK-8226705 / JDK-8343607
(handshake operation runs against a thread whose ack does not imply the state
the operation needs), the JBS handshake-vs-exiting-thread races.

Our stop-the-world machinery: the §A.3 thread-granular conductor
(`Source/JavaScriptCore/runtime/VMManager.cpp:383-873`,
`jsThreadsThreadGranularStopTheWorldAndRun` at :595) consumed by
`JSThreadsSafepoint::stopTheWorldAndRun`
(`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp`), used by code
jettison (SPEC-jit §5.3) and Class-A watchpoint fires (SPEC-jit §5.6). The
conductor predicate is access-based (§A.3.2: every entered thread of the
target VM other than the conductor is access-released or not-entered,
`allEnteredThreadsAreQuiescent` at VMManager.cpp:432-448), with
re-acquisition gated at `GCClient::Heap::acquireHeapAccess`
(`Source/JavaScriptCore/heap/Heap.cpp:7918-8090`, SPEC-ungil §A.3.2b(i)).
Governing spec sections: SPEC-ungil §A.2 (trap fan-out, D9 quanta), §A.3
(thread-granular STW, ANNEXES SB1/HBT2-4/EXIT1/ISB1), SPEC-jit §5.3/§5.6 +
annex App. 5.6(d) (watchdog), I2/I21.

Verdict legend: immune-by-construction / needs-test / susceptible-suspected.

Re-audited 2026-06-15 against the post-STW-WATCHDOG-CLOSURE / post-FIX-2-wiring
tree; line anchors refreshed; S3 downgraded (ratchet + wasm gate landed); S4
discrepancy resolved (helper now wired); S9 added (ack-≠-quiescence).

---

## S1. Poll emission and retention across tiers (poll-elision analog)

**Surface.** A mutator is stopped only at cooperative polls. Poll sites:
- Bytecode: every loop back-edge emits `OpLoopHint` + `OpCheckTraps`
  unconditionally (`bytecompiler/BytecodeGenerator.cpp:1505-1518`,
  `emitCheckTraps` at :1516); function prologues likewise.
- LLInt: `llintOp(op_check_traps, ...)`
  (`llint/LowLevelInterpreter.asm`).
- Baseline: `JIT::emit_op_check_traps` + slow path + handler thunk
  (`jit/JITOpcodes.cpp`).
- DFG: `ByteCodeParser::handleCheckTraps`
  (`dfg/DFGByteCodeParser.cpp:7037-7046`) emits a real `CheckTraps` node —
  not an `InvalidationPoint` — whenever `Options::usePollingTraps()` holds.
- FTL lowers the same node (`ftl/FTLLowerDFGToB3.cpp`).

`useJSThreads` force-sets `usePollingTraps=1` at option finalization
(`runtime/Options.cpp:1010-1012`, SPEC-jit M2b / I21: "cooperative polls
only; async breakpoint patching = I2 violation"), so the `InvalidationPoint`
substitution (the no-poll form) is unreachable flag-on, in every tier, for
the process lifetime (options are frozen before any JIT compile).

**Adversarial check.** The JDK-8161147 mechanism is the optimizer *removing*
polls from counted loops. JSC has no counted-loop poll-elision pass:
`CheckTraps` is `NodeMustGenerate` and clobbers world state
(`dfg/DFGClobberize.h:47-150` explicitly: "must clobber heap facts"), so DFG
LICM/DCE/CSE cannot hoist or delete it; B3 sees it as an effectful
patchpoint. There is no "loop strip mining"-style compensation needed
because no elision exists. The residual risk is a *future* pass that
special-cases CheckTraps for loop performance — the I21 wording in
Options.cpp:1012 is the recorded guard.

**Verdict: immune-by-construction** (cite: Options.cpp:1010-1012 forcing,
the unconditional bytecode emission, DFGByteCodeParser.cpp:7039,
DFGClobberize.h clobber rule). Liveness is exercised by the S2 test below
(a spinning sibling has *only* loop-hint polls available).

---

## S2. Trap-bit consumption race (poll reached, bit already gone)

**Surface.** Under the §A.2.1 interim seam the per-lite stop bits ALIAS the
single VM-wide trap word, and VMTraps' take rule clears `NeedStopTheWorld`
when the FIRST trapping thread latches it. A sibling still spinning in JS
would then poll a clear word and never trap — the conductor predicate hangs
until the 30s watchdog fail-stop. This is a state-at-poll mismatch in the
delivery channel itself.

**Mitigation in tree.** The conductor re-fires `vm.requestStop()` on every
non-quiescent predicate sample (1ms cadence), explicitly for this race:
`runtime/VMManager.cpp:759-770` ("Re-fire on every non-quiescent sample
(idempotent seq_cst RMW) ... RETIRED when the per-lite trap words land").
The fan/SB1 ordering (seq_cst stop-word store at :734, `requestStop` at
:735, storeLoadFence, fenced registry-walk samples at :395-448) puts the
fan and the clients' access transitions in one SC total order (ANNEX SB1).
The shared-GC §10.4 barrier loop now carries the SAME re-fire (S4 close-out
item 3; `Heap::conductSharedCollection` step 4).

**Adversarial check.** Soundness depends on the re-fire staying in the loop
until the per-lite trap words land. If the per-lite migration lands and the
re-fire is deleted in the same change, the test below is the regression
guard; if the re-fire is deleted FIRST, a 2-sibling spin reproduces the hang
deterministically.

**Verdict: needs-test** (regression guard across the per-lite trap-word
migration). Test: `JSTests/threads/cve/mc-safe-spin-vs-classa-stop.js`
(also covers S1 liveness: stop must converge with siblings whose only polls
are loop hints).

---

## S3. Unbounded time-to-safepoint in poll-free native regions (watchdog-DoS analog)

**Surface.** The conductor predicate needs every sibling access-released.
A sibling executing a long *native* operation while holding heap access has
no poll until it returns to bytecode/JIT code. Candidate poll-free regions
reachable from attacker-controlled JS:
- **Yarr** (regexp interpreter and JIT): zero VMTraps references in
  `Source/JavaScriptCore/yarr/`.
- Long built-ins without JS callbacks (huge `JSON.parse`, BigInt arithmetic
  on enormous operands, giant-string `replace`). Sub-second on realistic
  inputs at the sizes a single allocation admits.
- **Carrier wasm loops** (no `op_check_traps`; no `NeedStopTheWorld` poll in
  `Source/JavaScriptCore/wasm/`).

**Mitigations now in tree (CLOSES the original susceptible-suspected
finding).**
- `yarr/Yarr.h:71-73`: `static constexpr unsigned matchLimit = 100000000`
  with a `static_assert(matchLimit <= 100000000, ...)` ratchet whose comment
  cites "the only poll-free heap-access region reachable under" GIL-off; the
  Yarr interpreter decrements `remainingMatchCount` per `matchDisjunction`
  entry (`YarrInterpreter.cpp` ErrorHitLimit return) and the Yarr JIT
  enforces the same bound. Measured worst-case wall at the cap: ~1.8s
  interpreter / ~0.9s JIT (`docs/threads/STW-WATCHDOG-CLOSURE.md` §"The
  change landed THIS round"). 1.8s << 30s watchdog: bounded jank, not
  fail-stop. The static_assert makes raising the cap a conscious revisit.
- `runtime/Options.cpp:843-846`: `useWasm` is FORCED OFF under GIL-off
  ("wasm glue still reads the raw VM-block exception word; not yet audited
  for UNGIL §A.1.3"). Carrier wasm loops are therefore unreachable in the
  GIL-off configuration where the §A.3 predicate is the stop machinery.

**Adversarial check.** Residuals: (a) ~1.8s of stop latency is still
attacker-controllable jank; a `CheckTraps`-style poll inside
`matchDisjunction` would close it (handed to the perf charter,
STW-WATCHDOG-CLOSURE.md §"Perf facet"). (b) The wasm gate is a refusal,
not a fix — when wasm is re-enabled GIL-off, wasm loop polling is a HARD
precondition. (c) The "long built-in" family has no formal bound; none
audited above ~a few seconds at allocation-admissible sizes, but no
static_assert-style ratchet exists for them.

**Verdict: needs-test** (was susceptible-suspected; downgraded by the Yarr
ratchet + wasm refusal). Test:
`JSTests/threads/cve/mc-safe-regexp-tts-watchdog.js` — now PASSES on the
current tree (the calibrated regexp hits ErrorHitLimit at ~1.8s, the
Class-A stop converges in <25s); it is the runtime regression guard for the
compile-time ratchet. The pass criterion is unchanged: stop converges <25s.

---

## S4. Park sites: parked-while-holding-access, and the FIX-2 helper wiring

**Surface.** A thread parked in a native wait that still holds heap access
stalls the predicate exactly like S3, but forever (its waker may itself be
parked by the same stop) — the deadlock the FIX-2 banner in
`bytecode/JSThreadsSafepoint.cpp:74` describes.

**What the tree does.** The audited D9 park sites park access-released
(`GILDroppedSection` family — `Atomics.wait`, property-wait, `Lock`/
`Condition`/`Thread.join`, NVS ticket parks; full enumeration in the
2026-06-05 revision of this section, unchanged). GC-completion waits hold
access but are shielded by conductor ORDER: HBT4 takes
`Heap::JSThreadsStopScope` (rank-2 GCL) BEFORE publishing the stop word
(`runtime/VMManager.cpp:724-734`), and the winner queues behind any
in-progress shared GC (§10C(b)/(e)).

**FIX-2 helper wiring — RESOLVED.** The earlier audit found
`JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld`
(`bytecode/JSThreadsSafepoint.cpp:785-820`) with ZERO callers. It is now
wired at every tryLock-poll loop whose holder may allocate / fire / park
inside the critical section:
- `runtime/JSObject.cpp:4227` (`GILOffProtoCycleLocker`)
- `runtime/FunctionRareData.cpp:174` (allocation-profile init claim)
- `runtime/CodeCache.cpp:100`
- `bytecompiler/BytecodeGenerator.cpp:103`
- `bytecode/UnlinkedFunctionExecutable.cpp:120`
- `dfg/DFGPlan.cpp:132` (`GILOffCompilationLocker` finalize park)
- `dfg/DFGOSRExit.cpp:221`
The helper now ALSO quiesces for a pending shared-GC stop when no §A.3 word
is published yet (the counter-lock contgc wedge fix,
JSThreadsSafepoint.cpp:790-820: "an §A.3 requester that already owns
gilOffCompilationLock queues on the GCL BEHIND a shared-GC conductor; the
§A.3 stop word is still UNPUBLISHED"), breaking the
GCL→client-access→gilOffCompilationLock→GCL cycle.

**Verdict: needs-test** for the GCL-ordering shield (deterministic liveness:
GC storm on sibling threads concurrent with Class-A stop storm must never
trip the 30s watchdog): `JSTests/threads/cve/mc-safe-gcwait-vs-classa-stop.js`.

**CLOSED 2026-06-10 (CVE close-out round).** The test found THREE real
composition bugs, all landed:
1. **Mid-finalize GC sweeps the claimed plan's CodeBlocks (UAF).** GIL-off,
   `DFG::Plan::finalize`'s contended `GILOffCompilationLocker` parks via
   `parkSitePollAndParkForStopTheWorld` (access-released), but the plan was
   already OUT of `JITWorklist::m_plans` (AB18-R1-A claim) — a
   sibling-conducted shared GC then swept `m_codeBlock`/`alternative` under
   the finalizing mutator (`CodeBlock::replacement` RELEASE_ASSERT /
   null-`alternative()` SEGV in `compilationDidComplete`). Fix (AB18-R1-B):
   the claim table is now `JITWorklist::m_finalizingPlans`
   (key -> RefPtr<JITPlan>), walked UNCONDITIONALLY by
   `iterateCodeBlocksForGC` (`JITPlan::iterateCodeBlocksForFinalizeRoots`)
   and by `visitWeakReferences` — the claim itself is the root.
2. **AHA revert legs lost the §10.4 barrier wakeup.** The §A.3-word and
   Mode-machine revert legs in `GCClient::Heap::acquireHeapAccess` flipped
   HasAccess->NoAccess WITHOUT the GSP-conditional `m_gcBarrierCondition`
   notify that RHA performs; a GC conductor that had sampled the client
   HasAccess slept forever in the untimed barrier wait holding GCL, and a
   queued Class-A requester watchdogged at 30s in the `JSThreadsStopScope`
   tryLock-poll. Fix: both legs now mirror RHA's notify (Heap.cpp).
3. **GC stop fan lacked the S2 re-fire.** §10.4's barrier loop never
   re-fired the stop request, so under the §A.2.1 single-trap-word alias a
   sibling whose bit was consumed by the FIRST latching thread ran JS (or
   spun in a compile-lock tryLock loop) holding access forever — the
   ic-publish-reset-loops "OM transition stop" watchdog and the 100s pure
   hangs. Fix: `Heap::conductSharedCollection` step 4 re-fires
   `VMManager::requestStopAll(StopReason::GC)` per non-converged sample
   (GBL dropped for the call — m_worldLock ranks above GBL) and waits in
   1ms quanta, mirroring the §A.3 conductor's re-fire
   (VMManager.cpp:759-770). RETIRED with that one when per-lite trap words
   land.
Pinned bar after the fixes: 20/20 sequential + 20/20 amplified + 240/240
under 24-way load GIL-off Release; 3/3 GIL-on; Debug/ASAN green under the
S2a-prescribed lane options (`detect_stack_use_after_return=0`, lanes pin
`detect_leaks=0`).

---

## S5. Conductor vs exiting thread (handshake-vs-exiting-thread analog)

**Surface.** The JBS family: a handshake/stop counts a thread that is
concurrently exiting; the conductor either waits forever on a thread that
will never poll again, or dereferences per-thread state freed by the exit.

**Our construction.** ANNEX EXIT1 (SPEC-ungil §A.3.1, BINDING):
- The entered set IS the VMLiteRegistry; every predicate sample RE-WALKS the
  registry under `VMLiteRegistry::lock`
  (`runtime/VMManager.cpp:395-448`, `forEachEnteredThread`); lite/client
  pointers are never cached across samples.
- `state != Live` (TEARDOWN or absent) ⇒ counted EXITED before any client
  deref (EXIT1.4(a)); `clientHeap == nullptr` ⇒ not-entered (write-once
  release-published pointer, EXIT1.4(b)).
- Re-acquisition after teardown is FORBIDDEN — a TEARDOWN lite asserts in
  `acquireHeapAccess`, and fresh acquisition by any live thread funnels
  through the §A.3.2b(i) stop-word gate (`heap/Heap.cpp:7983-8030`) so an
  "exited-then-reborn" thread parks instead of running JS inside the window.
- `~VM` BLOCKS until VM-empty (EXIT1.9), so the conductor's `VM&` target
  cannot die under the window.

**Adversarial check.** The classic TOCTOU — thread sampled as exited, then
re-enters — is closed structurally, not by sampling: re-entry requires heap
access, and acquisition is the gate (the SB1 seq_cst Dekker pair, conductor
fenced sample at VMManager.cpp:436-444 vs client seq_cst CAS+poll at
Heap.cpp:7918-7990, both interleavings litmus-proved in ANNEX SB1.4).
A thread exiting BETWEEN samples leaves a registry entry transition
Live→TEARDOWN under the registry lock, which the next sample (re-walk,
never cached) observes. No freed-lite deref is possible inside the walk
because the functor runs under the registry-lock hold of the walk that
found the lite.

**Verdict: immune-by-construction** (EXIT1.1-1.9 + §A.3.2b(i); the sampled
walk is re-derived per sample and the gate, not the sample, carries
soundness).

---

## S6. Entry / fresh attach during a stop

**Surface.** A thread entering the VM (or spawning) after the fan misses the
stop bits and runs JS inside the window.

**Our construction.** §A.3.4: entry parks. All entry shapes funnel through
`GCClient::Heap::acquireHeapAccess`, which after the F8 CAS polls the §A.3
stop word seq_cst and mandatory-reverts + parks on a pending window
(`heap/Heap.cpp:7983-8030`), and likewise gates Mode-machine stops
(:8022-8030). The load-bearing claim ("This leg CARRIES soundness for every
unenumerable AHA/RHA bracket ... fresh acquisition never admits a mutator
into an open window") is at the gate. Conductor exemption (HBT3.2/HBT2.1)
is exact: tenure is thread-keyed seq_cst (`runtime/VMManager.cpp:733`).

**Verdict: immune-by-construction** (§A.3.2b(i)/§A.3.4 + SB1; the AB-21
conductor re-acquire inside its own window is exempted by tenure and keeps
the satisfied predicate satisfied since `allEnteredThreadsAreQuiescent`
skips the conductor's lite, :437-438).

---

## S7. State at resume: stale instruction streams (the icache flavor of state-at-poll mismatch)

**Surface.** The window patches machine code; a sibling that never parked
(it was access-released in a native region for the whole window) re-enters
JIT code with a stale icache — executing pre-patch instructions is exactly
"stopped state misdescribing reality", one fetch at a time.

**Our construction.** Three layers (ANNEX ISB1, SPEC-jit F5/R1.d):
1. Patcher-side: `crossModifyingCodeFence` BEFORE the stop-generation bump
   and stop-word clear (`runtime/VMManager.cpp:824-827`; the seq_cst word
   clear is the synchronizes-with edge that publishes the bump).
2. Every park exit runs `jsThreadsNVSExitInstructionSync` (NVS ticket tail,
   :500, :574; notifyVMStop sibling path :1292/:1322).
3. May-execute-JIT transitions that bypassed an NVS exit (the S4
   access-released-throughout sibling) compare a per-lite stop-generation
   copy at `acquireHeapAccess` and ISB on mismatch (ISB1.2,
   `heap/Heap.cpp:8080` region).

**Adversarial check.** The mechanism is only as good as the enumeration of
"may execute JIT" transitions. That enumeration is closed by the AHA funnel
claim (every re-entry needs access, every access acquisition runs the
compare). The U-T5 sleep-through-jettison arm (arm64) is the existing
targeted test; keep it in the post-ungil ladder.

**Verdict: immune-by-construction** (ISB1.1/ISB1.2 + F5; existing U-T5 arm
covers it — no new test from this audit).

---

## S8. Frame/oop-map misdescription at the stop (the GC/stack-walk flavor)

**Surface.** HotSpot's half of the mechanism class: the stopped thread's
frames are described by oop maps valid only at the poll PC; a mismatch lets
the stopped-world operation walk wrong state.

**Why our analog is structurally different.** JSC has no per-PC oop maps:
GC roots from thread stacks are CONSERVATIVE (every word of every stopped
thread's stack and registers is scanned), so there is no "map at poll PC
disagrees with frame layout" failure mode — misdescription is impossible
because nothing is described. The stopped-world operations of THIS
machinery (Class-A fire bodies, jettison) do not walk sibling JS frames at
all: jettison patches invalidation points / unlinks incoming calls and
defers machine-code reclamation ("reclamation rides the GC"), so a parked
sibling's return into a jettisoned CodeBlock lands on a patched
invalidation point whose OSR exit descriptor was compiled with the code —
state description and code version travel together by construction.
Debugger walk closures are carrier-only in v1 (UNGIL §A.2.7/SD13).

**Residual (out of MC-SAFE scope, recorded for the congc audit):** N-mutator
conservative scanning — the shared collector must capture EVERY client
thread's stack bounds + register state, including spawned threads parked on
NVS tickets; that is SPEC-heap/SPEC-congc territory and audited there.

**Verdict: immune-by-construction** for the §A.3 stop machinery
(conservative scanning + invalidation-point semantics + deferred
reclamation), with the congc handoff noted.

---

## S9. Handshake-ack ≠ quiescence (JDK-8226705 / JDK-8343607 analog)

**Surface.** The §A.3 predicate equates ack with `!hasHeapAccess()`
(VMManager.cpp:441). A thread that has released access ("acked") is NOT
necessarily quiescent for arbitrary purposes — it may still hold derived
state the closure invalidates, and threads NOT in the registry (compiler
workers, GC helpers) never ack at all.

**(a) RHA brackets holding closure-invalidatable state.** The canonical
shape is the parkSite tryLock-poll loop (S4 list above): a mutator RHAs to
park, the closure runs, the mutator AHAs and resumes a slow path that began
BEFORE the window. Construction:
`parkSitePollAndParkForStopTheWorld` brackets every park with a
`conductorHeapFactRewriteEpoch()` compare and, on mismatch, runs
`jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite`
(`JSThreadsSafepoint.cpp:810-818`) — i.e., the helper itself enforces
"re-validate after ack". Stack-resident `JSCell*` held across the bracket
are GC-safe (conservative scan, S8); the one NON-GC-visible held-state
violation found to date — DFGPlan finalize holding an unrooted plan across
the park — is the S4 close-out item 1, fixed by `m_finalizingPlans` rooting
(`jit/JITWorklist.h:135` comment). The remaining call sites
(FunctionRareData, JSObject proto-cycle, CodeCache, BytecodeGenerator,
UnlinkedFunctionExecutable, DFGOSRExit) each loop on a tryLock and re-check
their wait predicate on every `true` return per the helper's contract
("the caller must treat that as a fresh acquisition episode",
JSThreadsSafepoint.cpp:780-784); none caches a butterfly/offset/handler
pointer across the call (audited per call site, 2026-06-15).

**(b) Non-registry threads.** `allEnteredThreadsAreQuiescent` walks ONLY
the VMLiteRegistry (VMManager.cpp:432-448); concurrent DFG/FTL compiler
threads and GC marking helpers are NOT participants and are NOT stopped by
a §A.3 window. This is by design (SPEC-heap §11: "Compiler/GC-helper
threads aren't participants"): the §A.3 closure operations (watchpoint
fire, jettison, OM transition stop) interact with compiler-shared state
through the SAME concurrent-JIT protocols that already govern
mutator-vs-compiler in stock JSC (`ConcurrentJSLock`, `DFG::Safepoint`,
`JITWorklist` cancellation, deferred-watchpoint-fire). SPEC-jit P3 ("code
metadata readable at safepoints without locks") is the contract scope: the
closure may READ; WRITES go through the concurrent-safe paths. Retired-JIT
data freeing — the one operation that DOES need compiler quiescence — runs
only at the GC stop (`GCSafepointEpoch` reclaim sequence, SPEC-heap §11:
`suspendCompilerThreads()` BEFORE `bumpAndReclaim()`), never inside a §A.3
closure.

**Adversarial check.** The composition gap would be: a §A.3 closure that
mutates state a compiler thread reads WITHOUT going through ConcurrentJSLock
/ deferred-fire. The Task-11 audit table (SPEC-jit annex App. 5.6(c)) +
the STW-WATCHDOG-CLOSURE Repatch.cpp fire-before-lock fix enumerate the
fire sites; no closure-body write bypasses the concurrent-safe path. The
(a)-side gap would be a NEW parkSite caller that holds a non-conservatively-
rooted derived pointer across the call — guarded by the helper's contract
comment + the heapFactRewriteEpoch jettison, but ultimately open-world
(future call sites). The S4 test family is the runtime guard.

**Verdict: immune-by-construction** for (b) (registry-walk scope + SPEC-jit
P3 + heap §11 compiler suspension at the only freeing site);
**immune-by-construction** for (a) at the audited call sites (epoch bracket
+ per-site re-validation), with the S4 test as the standing regression
guard for the pattern. No new test added beyond S4's.

---

## Test inventory (all under JSTests/threads/cve/, EXECUTED POST-UNGIL)

| Test | Surfaces | Kind |
|---|---|---|
| `mc-safe-spin-vs-classa-stop.js` | S1, S2 | deterministic liveness (regression guard for the per-lite trap-word migration) |
| `mc-safe-regexp-tts-watchdog.js` | S3 | regression guard for the Yarr matchLimit ratchet (was a susceptibility demonstrator; now PASSES) |
| `mc-safe-gcwait-vs-classa-stop.js` | S4, S9(a) | deterministic liveness for the GCL-ordering shield + parkSite ack-then-revalidate, amplifier-ready |

Action items (non-test): when wasm is re-enabled GIL-off, wasm loop polling
is a HARD precondition (S3); a `CheckTraps`-style poll inside Yarr
`matchDisjunction` would retire the residual ~1.8s jank (S3, perf charter);
keep the conductor re-fire until the per-lite trap words land (S2).

---

## B16 (Tier-B cross-family residual): STW-watchdog 30s under OM-transition stops

**Status: TRIAGE INSTRUMENTATION LANDED 2026-06-18; root cause not yet
localized.** ~1/6 amplified flake on
`JSTests/threads/cve/mc-gc-weakgcmap-registry-vs-prune.js` and
`JSTests/threads/cve/mc-jit-delete-reuse-stale-offset.js` (the latter
usually masked by an unrelated `Structure.cpp:717` materializePropertyTable
assertion — see that test's DIAGNOSIS.md).

**Captured evidence**
(`JSTests/threads/cve/mc-gc-weakgcmap-registry-vs-prune.stw-variant.txt`):
the conductor (a spawned lite, "OM transition stop" context) fail-stops at
the §A.3.2 predicate wait with exactly ONE non-quiescent sibling
(hasHeapAccess=true). Every audited FIX-2 class-(2) wait
(PerEventStopClaim spin, every GILOffCompilationLocker tryLock-poll,
lockStaticPropertyReificationLockContended, the §A.3 arbitration leg, the
JSThreadsStopScope GCL leg) releases the caller's client access before
blocking — so the wedged lite is in a wait NOT on that audited list. The
existing dump's REQUESTER backtrace (the WTFCrash on the conductor thread)
says nothing about where the SIBLING is blocked; a re-symbolize attempt
against the current binary failed (crash log predates a rebuild).

**Instrumentation landed** (`bytecode/JSThreadsSafepoint.cpp`,
`watchdogAssertStopProgress`): the 30s fail-stop now additionally dumps
(a) per-lite owning `WTF::Thread*` (via the per-thread client's
`parkedRootSnapshotThread()`, stable under U-T6); (b) conductor-side
held-lock facts — `gilOffCompilationLock().isOwner()`,
`staticPropertyReificationLock().isOwner()`, `isCurrentlyTenuredConductor`,
server `gcStopPendingForAllClients` / `worldIsStoppedForAllClients`
(discriminates FIX-2 mech (1) "conductor holds a long-hold lock the wedged
sibling contends" from mech (2) "sibling sits in an unbracketed
access-holding wait"); (c) a SUSPEND + frame-pointer-walk native backtrace
of every non-quiescent sibling (PC + up to 64 return addresses, validated
against that thread's real `StackBounds`, dumped raw for offline
`llvm-addr2line` against the SAME build — registry lock dropped first so
the SIGUSR2 ack handshake cannot itself wedge). All on the fail-stop path;
gilOff-only callers, so flag-off byte-identical.

**Next step (after the builder reruns the two repros under amplify with
this instrumentation):** the (c) backtrace names the exact blocked frame.
Then EITHER bracket that frame's wait with the FIX-2 class-(1)
`gcClientWillParkForThreadGranularStop` /
`gcClientDidResumeFromThreadGranularStop` pair (or a per-quantum
`parkSitePollAndParkForStopTheWorld` poll) if it is an access-holding
native wait, OR — if (b) shows the conductor holds one of the two
recursive locks — route the conductor's OM-transition-stop call site so it
drops that lock first (the mech-(1) "escaped lock-holding fireAll caller"
shape; SPEC-jit annex App. 5.6(c) bucket iii). Gate (unchanged): both
repros 0/60 watchdog trips under `Tools/threads/amplify.sh`.
