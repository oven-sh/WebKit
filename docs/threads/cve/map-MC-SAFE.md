# MC-SAFE — Safepoint reachability / state-at-poll mismatch

Mechanism class (web-derived, treated as data): a thread can never be brought
to a stop (poll elision, unbounded time-to-safepoint, watchdog DoS), or stops
with frame/oop-map state misdescribing reality so the stopped-world operation
walks wrong state. Exemplars: JDK-8161147 (counted-loop safepoint elision
family), the JBS handshake-vs-exiting-thread races.

Our stop-the-world machinery: the §A.3 thread-granular conductor
(`Source/JavaScriptCore/runtime/VMManager.cpp:200-674`,
`jsThreadsThreadGranularStopTheWorldAndRun` at :514) consumed by
`JSThreadsSafepoint::stopTheWorldAndRun`
(`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:203-350`), used by
code jettison (SPEC-jit §5.3) and Class-A watchpoint fires (SPEC-jit §5.6).
The conductor predicate is access-based (§A.3.2: every entered thread of the
target VM other than the conductor is access-released or not-entered), with
re-acquisition gated at `GCClient::Heap::acquireHeapAccess`
(`Source/JavaScriptCore/heap/Heap.cpp:5681-5790`, SPEC-ungil §A.3.2b(i)).
Governing spec sections: SPEC-ungil §A.2 (trap fan-out, D9 quanta), §A.3
(thread-granular STW, ANNEXES SB1/HBT2-4/EXIT1/ISB1), SPEC-jit §5.3/§5.6 +
annex App. 5.6(d) (watchdog), I2/I21.

Verdict legend: immune-by-construction / needs-test / susceptible-suspected.

---

## S1. Poll emission and retention across tiers (poll-elision analog)

**Surface.** A mutator is stopped only at cooperative polls. Poll sites:
- Bytecode: every loop back-edge emits `OpLoopHint` + `OpCheckTraps`
  unconditionally (`bytecompiler/BytecodeGenerator.cpp:1498-1502`,
  `emitCheckTraps` at :1509); function prologues likewise.
- LLInt: `llintOp(op_check_traps, ...)`
  (`llint/LowLevelInterpreter.asm:2964`).
- Baseline: `JIT::emit_op_check_traps` + slow path + handler thunk
  (`jit/JITOpcodes.cpp:1814-1874`).
- DFG: `ByteCodeParser::handleCheckTraps`
  (`dfg/DFGByteCodeParser.cpp:7037-7039`) emits a real `CheckTraps` node —
  not an `InvalidationPoint` — whenever `Options::usePollingTraps()` holds.
- FTL lowers the same node (`ftl/FTLLowerDFGToB3.cpp:1879`).

`useJSThreads` force-sets `usePollingTraps=1` at option finalization
(`runtime/Options.cpp:917-920`, SPEC-jit M2b / I21: "cooperative polls only;
async breakpoint patching = I2 violation"), so the `InvalidationPoint`
substitution (the no-poll form) is unreachable flag-on, in every tier, for
the process lifetime (options are frozen before any JIT compile).

**Adversarial check.** The JDK-8161147 mechanism is the optimizer *removing*
polls from counted loops. JSC has no counted-loop poll-elision pass:
`CheckTraps` is `NodeMustGenerate` and clobbers world state, so DFG
LICM/DCE/CSE cannot hoist or delete it; B3 sees it as an effectful
patchpoint. There is no "loop strip mining"-style compensation needed
because no elision exists. The residual risk is a *future* pass that
special-cases CheckTraps for loop performance — the I21 wording in
Options.cpp:920 is the recorded guard.

**Verdict: immune-by-construction** (cite: Options.cpp:917-920 forcing, the
unconditional bytecode emission, DFGByteCodeParser.cpp:7039). Liveness is
exercised by the S2 test below (a spinning sibling has *only* loop-hint
polls available).

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
`runtime/VMManager.cpp:583-594` ("Re-fire on every non-quiescent sample
(idempotent seq_cst RMW) ... RETIRED when the per-lite trap words land").
The fan/SB1 ordering (seq_cst stop-word store at :569, `requestStop` at
:570, storeLoadFence, fenced registry-walk samples at :327-352) puts the
fan and the clients' access transitions in one SC total order (ANNEX SB1).

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

## S3. Unbounded time-to-safepoint in poll-free native regions → watchdog fail-stop (watchdog-DoS analog)

**Surface.** The conductor predicate needs every sibling access-released.
A sibling executing a long *native* operation while holding heap access has
no poll until it returns to bytecode/JIT code. Poll-free long-running
regions reachable from attacker-controlled JS:
- **Yarr** (regexp interpreter and JIT): zero VMTraps references anywhere in
  `Source/JavaScriptCore/yarr/` (verified by grep). A catastrophic
  backtracking regexp runs for minutes holding heap access (subject string
  is heap-allocated).
- Long built-ins without JS callbacks (huge `JSON.parse`/`stringify`,
  BigInt arithmetic on enormous operands, `String.prototype.replace` over
  giant strings). Sub-30s individually on realistic inputs, but the same
  family.
- **Carrier wasm loops**: §I refuses wasm EXECUTION on spawned threads
  (UNGIL-HANDOUT §I), but the carrier may run wasm while a spawned thread
  requests a Class-A stop. Wasm tier loops do not execute `op_check_traps`
  and `Source/JavaScriptCore/wasm/` has no `NeedStopTheWorld` poll
  (verified by grep) — same TTS gap, carrier-flavored.

Consequence: a sibling Class-A fire or jettison waits in the predicate loop
and `watchdogAssertStopProgress` (`bytecode/JSThreadsSafepoint.cpp:379,
401-413`, wired at `runtime/VMManager.cpp:594`) RELEASE_ASSERTs at 30s —
a deterministic whole-process kill that web/JS content can trigger with one
regexp plus any stop requester. Note the SAME gap delays VM-wide
termination delivery (SPEC-ungil §A.2 rule 4): the watchdog/terminate arm
cannot interrupt Yarr either, so the "kill the runaway script" remedy is
itself blocked until the regexp returns.

**Why this is the chartered trade-off, and where it stops being one.** The
watchdog is by design a fail-stop conversion of a wedged stop (SPEC-jit
annex App. 5.6(d)); for an *escaped lock-holding fireAll caller* (bucket
iii) crashing is right. But for a merely-slow poll-free native region the
stop is not wedged — it would converge at 31s, 60s... The spec's own bound
(D9: 10ms quanta) is honored at park sites but has no equivalent inside
Yarr. Availability impact only — no memory-unsafety: the conductor patches
nothing until the predicate converges, so there is no "patch under a
running sibling" escape here (the watchdog fires INSTEAD of proceeding —
fail-closed, the correct polarity).

**Verdict: susceptible-suspected** (deterministic remote DoS of a
threads-enabled embedder: ~30s catastrophic regexp on thread A + any
Class-A stop on thread B ⇒ RELEASE_ASSERT). Fix shape: a Yarr backtrack-
budget poll (Yarr already counts backtracks for the stack guard; check the
lite's stop/termination bits every N backtracks and release-access-park per
the D9 protocol), and a wasm loop poll for the carrier before N-thread wasm
is ever admitted. Test:
`JSTests/threads/cve/mc-safe-regexp-tts-watchdog.js` (susceptibility
demonstrator; expected to fail-stop on today's tree, passes once Yarr gains
the D9-quantum poll).

---

## S4. Park sites: parked-while-holding-access, and the unwired FIX-2 helper

**Surface.** A thread parked in a native wait that still holds heap access
stalls the predicate exactly like S3, but forever (its waker may itself be
parked by the same stop) — the deadlock the FIX-2 comment in
`bytecode/JSThreadsSafepoint.cpp:415-429` describes.

**What the tree actually does.** The audited D9 park sites park
access-released:
- TA `Atomics.wait` per-wait nodes: `waitSyncWithPerWaitNode`
  (`runtime/WaiterListManager.cpp:202-316`) — caller contract at :186-196:
  GIL-off callers arrive inside a `GILDroppedSection` (§J.3; spawned arm =
  token-kept, access-released).
- Property-path waits: `runtime/ThreadAtomics.cpp:1020-1027`
  (`GILDroppedSection`, spawned arm access-released).
- `Lock`/`Condition`/yield parks: `runtime/LockObject.cpp:181-211
  (GILDroppedSectionSpawnedArm releases the client's heap access), :282-289,
  :546-575`.
- `Thread.join`/parks: `runtime/ThreadObject.cpp:422-428`.
- W1 reacquisition episodes drop the rank-3 list lock first
  (`WaiterListManager.cpp:230-235`), honoring the "no rank-3 lock across
  the park poll" rule.
- A trapping mutator parks in `VMManager::notifyVMStop`
  (`runtime/VMManager.cpp:909-1000`): §A.3 ticket first (:938-944,
  `gcClientWillParkForThreadGranularStop` releases access), Mode-machine
  representative election after.
- GC-completion waits (`Heap::waitForCollector`, `heap/Heap.cpp:2497-2532`,
  `ParkingLot::compareAndPark` at :2530) DO hold heap access and poll
  neither the §A.3 word nor the lite bits — but they are shielded by
  conductor ORDER: HBT4.5 takes `Heap::JSThreadsStopScope` (the rank-2 GC
  conductor lock) BEFORE publishing the stop word
  (`runtime/VMManager.cpp:560-570`), and the winner queues behind any
  in-progress shared GC (§10C(b)/(e)). So no §A.3 stop word can be pending
  while a shared-mode collection (the thing `waitForCollection` waits on,
  post-ISS) is mid-cycle. The legacy (`!isSharedServer()`) wait shape is
  only reachable before any Thread spawns (the ISS flip,
  `heap/Heap.cpp:4424`) — no siblings exist, vacuous.

**The discrepancy.** `JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld`
(`bytecode/JSThreadsSafepoint.{h:200,cpp:430-455}`) — FIX-2's per-D9-quantum
stop poll, whose own comment names "Atomics.wait per-wait nodes,
property-wait, Lock/Condition/Thread parks, GC-completion waits" as callers
— has ZERO call sites in the tree (verified by grep). Either it is
vestigial (superseded by the GILDroppedSection access-release + the
Heap.cpp:5752 AHA gate, which together satisfy the predicate without any
in-wait poll) or a planned wiring never landed. The watchdog crash message
(`JSThreadsSafepoint.cpp:411`) still directs operators to it. If ANY
GIL-off park site exists that (a) holds heap access across its wait and
(b) is not covered by the GCL-ordering shield, the FIX-2 deadlock is live;
the audited sites above are clean, but the enumeration is open-world
(embedder native code, future park sites).

**Verdict: needs-test** for the GCL-ordering shield (deterministic liveness:
GC storm on sibling threads concurrent with Class-A stop storm must never
trip the 30s watchdog): `JSTests/threads/cve/mc-safe-gcwait-vs-classa-stop.js`.
Plus a docs/code action item (not a test): either wire
`parkSitePollAndParkForStopTheWorld` per its comment or delete it and
re-point the watchdog message, so the next park-site author has one true
rule to follow.

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
   (VMManager.cpp:583-594). RETIRED with that one when per-lite trap words
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

**Our construction.** ANNEX EXIT1 (SPEC-ungil §A.3.1, BINDING, rev 32):
- The entered set IS the VMLiteRegistry; every predicate sample RE-WALKS the
  registry under `VMLiteRegistry::lock`
  (`runtime/VMManager.cpp:317-352`, `forEachEnteredThread`); lite/client
  pointers are never cached across samples.
- `state != Live` (TEARDOWN or absent) ⇒ counted EXITED before any client
  deref (EXIT1.4(a), :341-342); `clientHeap == nullptr` ⇒ not-entered
  (write-once release-published pointer, EXIT1.4(b), :343-344).
- Re-acquisition after teardown is FORBIDDEN — a TEARDOWN lite asserts in
  `acquireHeapAccess` (`heap/Heap.cpp:5696-5706`), and fresh acquisition by
  any live thread funnels through the §A.3.2b(i) stop-word gate
  (`heap/Heap.cpp:5752-5759`) so an "exited-then-reborn" thread parks
  instead of running JS inside the window.
- `~VM` BLOCKS until VM-empty (EXIT1.9), so the conductor's `VM&` target
  cannot die under the window.

**Adversarial check.** The classic TOCTOU — thread sampled as exited, then
re-enters — is closed structurally, not by sampling: re-entry requires heap
access, and acquisition is the gate (the SB1 seq_cst Dekker pair, conductor
fenced sample at VMManager.cpp:333-337 vs client seq_cst CAS+poll at
Heap.cpp:5712-5759, both interleavings litmus-proved in ANNEX SB1.4).
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
(`heap/Heap.cpp:5752-5759`), and likewise gates Mode-machine stops
(:5773-5778). The comment at :5747-5751 is the load-bearing claim: "This
leg CARRIES soundness for every unenumerable AHA/RHA bracket ... fresh
acquisition never admits a mutator into an open window." Conductor
exemption (HBT3.2/HBT2.1) is exact: tenure is thread-keyed seq_cst
(`runtime/VMManager.cpp:265,311-314`).

**Verdict: immune-by-construction** (§A.3.2b(i)/§A.3.4 + SB1; the AB-21
conductor re-acquire inside its own window at VMManager.cpp:598-625 is
exempted by tenure and keeps the satisfied predicate satisfied since
`allEnteredThreadsAreQuiescent` skips the conductor's lite, :364-380).

---

## S7. State at resume: stale instruction streams (the icache flavor of state-at-poll mismatch)

**Surface.** The window patches machine code; a sibling that never parked
(it was access-released in a native region for the whole window) re-enters
JIT code with a stale icache — executing pre-patch instructions is exactly
"stopped state misdescribing reality", one fetch at a time.

**Our construction.** Three layers (ANNEX ISB1, SPEC-jit F5/R1.d):
1. Patcher-side: `crossModifyingCodeFence` BEFORE the stop-generation bump
   and stop-word clear (`runtime/VMManager.cpp:646-649`; the seq_cst word
   clear is the synchronizes-with edge that publishes the bump).
2. Every park exit runs `jsThreadsNVSExitInstructionSync` (NVS ticket tail,
   :429, :494; notifyVMStop sibling path :995).
3. May-execute-JIT transitions that bypassed an NVS exit (the S4
   access-released-throughout sibling) compare a per-lite stop-generation
   copy at `acquireHeapAccess` and ISB on mismatch (ISB1.2,
   `heap/Heap.cpp:5786-5790` region).

**Adversarial check.** The mechanism is only as good as the enumeration of
"may execute JIT" transitions (the ISB1 delivery-set widening, IJ row).
That enumeration is owned by the U-T5 arm + U20 lint, and the AHA funnel
claim (every re-entry needs access, every access acquisition runs the
compare) makes the set closed under the same argument as S6. The U-T5
sleep-through-jettison arm (arm64) is the existing targeted test; keep it
in the post-ungil ladder.

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
defers machine-code reclamation ("reclamation rides the GC",
`bytecode/JSThreadsSafepoint.cpp:296-299`), so a parked sibling's return
into a jettisoned CodeBlock lands on a patched invalidation point whose OSR
exit descriptor was compiled with the code — state description and code
version travel together by construction. Debugger walk closures are
carrier-only in v1 (UNGIL §A.2.7/SD13).

**Residual (out of MC-SAFE scope, recorded for the congc audit):** N-mutator
conservative scanning — the shared collector must capture EVERY client
thread's stack bounds + register state, including spawned threads parked on
NVS tickets; that is SPEC-heap/SPEC-congc territory (per-thread clients,
§10A) and should be audited under the GC mechanism classes, not this one.

**Verdict: immune-by-construction** for the §A.3 stop machinery
(conservative scanning + invalidation-point semantics + deferred
reclamation), with the congc handoff noted.

---

## Test inventory (all under JSTests/threads/cve/, EXECUTED POST-UNGIL)

| Test | Surfaces | Kind |
|---|---|---|
| `mc-safe-spin-vs-classa-stop.js` | S1, S2 | deterministic liveness (regression guard for the per-lite trap-word migration) |
| `mc-safe-regexp-tts-watchdog.js` | S3 | susceptibility demonstrator — EXPECTED FAIL-STOP on today's tree |
| `mc-safe-gcwait-vs-classa-stop.js` | S4 | deterministic liveness for the GCL-ordering shield, amplifier-ready |

Action items (non-test): resolve the FIX-2 wiring discrepancy (S4); Yarr +
wasm-loop D9-quantum polls (S3 fix shape); keep the conductor re-fire until
the per-lite trap words land (S2).
