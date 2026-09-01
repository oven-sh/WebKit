# MC-AINT — asynchronous interruption at an unsafe point: mapping to our threads surface

Mechanism class (from the catalog; web-derived exemplars treated as data):
*"Asynchronous interruption at an unsafe point: interrupt/abort/termination
delivered between invariant-breaking and invariant-restoring instructions.
Design rule: thread termination must be safepoint-polled, never delivered
asynchronously."* Exemplars: Thread.Abort partial-trust escape lineage
(removed in .NET Core), POSIX signal-in-VM bug families.

Audited against the tree at jarred/threads (GIL-off bring-up; AB-10/AB-17
families landed). Specs of record: SPEC-jit.md (frozen; I2/I16/I21),
SPEC-ungil.md rev 32 + BINDING annexes via UNGIL-HANDOUT.md (§A.2.4 TERM1,
§A.2.5, §A.2.8 annex W, D9 park-quanta rule), SPEC-api.md §5.6/I24,
SPEC-objectmodel.md (I20/I29/O2 poll-free-window discipline). Line numbers
re-pinned 2026-06-15.

Verdict legend: **immune** = immune-by-construction (protocol cited, with
the adversarial argument), **needs-test** = susceptibility test written
under `JSTests/threads/cve/` (run post-ungil), **suspected** =
susceptible-suspected with the precise hole.

Summary: the architecture *is* the design rule — termination and stops are
trap-bit + cooperative-poll only, the JS-level abort surface does not exist
(TERM1.1), and the signal path is compiled out of reach flag-on. The S4
hole originally found here (SPEC-jit I21(b) specified-but-unimplemented:
flag-on DFG/FTL trap polls were not invalidation points, so a mutator
parked at a poll across a Class-A fire could resume into stale TTL-elided
code) is now CLOSED by the AB-10 landing — the closure is enforced at
three levels (parser, clobberize, codegen) and pinned by
`mc-aint-poll-resume-stale-elided.js`.

---

## S1. Signal-based VMTraps delivery (the literal POSIX signal-in-VM analog)

The legacy delivery mechanism is the textbook MC-AINT surface: a
`SignalSender` work-queue thread suspends the mutator and installs halt
breakpoints into *running* JIT code from outside
(`runtime/VMTraps.cpp:315-450`; `tryInstallTrapBreakpoints` at `:177`/`:439`,
AccessFault handler at `:329-330`). If that ever ran against N mutators it
would deliver interruption at arbitrary instruction boundaries — between
any invariant-breaking and invariant-restoring pair.

**Verdict: immune-by-construction (flag-on).** Three independent gates:

1. `runtime/Options.cpp:1012` (inside `notifyOptionsChanged()` at `:763`,
   which runs on every options batch and is final at `Options.cpp:1369-1371`):
   `useJSThreads` forces `Options::usePollingTraps() = true` (SPEC-jit M2b,
   I21(a)). A hostile `--usePollingTraps=false --useJSThreads=1` command
   line is overridden because the forcing runs *after* parsing, in the
   finalize hook; no later code path under `useJSThreads` sets it back
   (the other assignments at `:616/:711/:724` also force true).
2. `VMTraps::initializeSignals()` (`runtime/VMTraps.cpp:549-554`) installs
   the signal handlers only when `!usePollingTraps` — flag-on the handlers
   are never registered at all.
3. `requestThreadStopIfNeeded` (`runtime/VMTraps.cpp:593-603`) gates
   SignalSender creation on `!Options::usePollingTraps() && !vm.gilOff()`
   (UNGIL §A.2.5: "no single ownerThread to suspend; trap-breakpoint
   installation assumes one mutator stack"). So even if gate 1 rotted, a
   gilOff VM still never constructs a SignalSender; the per-lite VMTraps
   instances additionally assert the SignalSender machinery is per-lite
   unreachable (`VMTraps.cpp:94-97`).

Adversarial residue: flag-off (vanilla JSC) keeps signal delivery — that is
upstream's threat model, out of audit scope. Debugger break
(`NeedDebuggerBreak`) stays on the landed carrier protocol (rule-3
exemption, `VMTraps.cpp:924`) — still bit+poll, not signal, flag-on.

## S2. Thread termination delivery (the Thread.Abort analog)

- **No JS-level abort surface exists.** TERM1.1 (UNGIL-HANDOUT.md:320-337):
  `Thread.prototype.terminate` DOES NOT EXIST in v1; the Thread surface is
  constructor/join/asyncJoin/id/tls (verified: no terminate host function in
  `runtime/ThreadObject.cpp` — the only "terminate" tokens are local poll
  results in the join loop, `:492-546`). The .NET-lineage fix (remove the
  API) is adopted by construction. A thread-targeted terminate is POST-UNGIL
  chartered only.
- **Raising termination is bit-only.** `VMTraps::fireTrapVMWide`
  (`runtime/VMTraps.cpp:1016-1070`): under the leaf registry lock it performs
  only `exchangeOr` of trap bits into every sibling lite word + the VM word
  (§A.2.3 rule 3) — it never suspends a thread, never patches code, never
  touches a foreign stack. Same for the sibling fan
  `fanOutTerminationToSiblingLites` (`:1071`) and the watchdog W3
  timer-thread raise (`runtime/Watchdog.h` W3 — rule-3 fan-out only).
- **Observing termination is poll-only.** Delivery points are exactly: the
  generated-code trap polls (LLInt `op_check_traps`, Baseline/DFG/FTL
  CheckTraps -> `operationHandleTraps`, `jit/JITOperations.cpp:3044`),
  `handleTrapsForCurrentThreadIfNeeded` at VM-entry seams, and the D9 park
  quanta (S3). All of these sit at bytecode/host-call boundaries where VM
  invariants hold; `throwTerminationException` runs the ordinary exception
  machinery from those sites, never mid-sequence.

**Verdict: immune-by-construction.** The adversarial question — "can any
code path divert a mutator's control flow without its cooperation?" — has
exactly two historical answers (signal traps: S1, off; suspend-based
conservative scan: S7, inspect-only/resume-same-PC) and no flag-on third.

## S3. Parks (Atomics.wait / Lock / Condition / join) under termination — the abort-during-wait analog

The classic exploit shape is termination delivered to a thread blocked in a
runtime wait while it holds half-mutated wait-queue state. Our protocol
(UNGIL §A.2.4 rule 4 / annex A26, D9): sync parks never rely on an async
wake — they wait in 10ms quanta and poll termination between quanta. The
flag-off `vm.syncWaiter()` wake is bypassed-not-deleted
(`runtime/VMTraps.cpp:596-603`).

Sites and their invariant-restoration discipline:

- `Thread.prototype.join` — `runtime/ThreadObject.cpp:492-546`: quantum
  loop holds `joinLock` only between sleeps; on a termination poll hit it
  exits the park, ends the GIL-dropped section, and only then (back under
  the GIL, **no native lock held**) does request-then-throw
  (`vm.setHasTerminationRequest(); vm.throwTerminationException()`).
- Property/TA `Atomics.wait` — `runtime/WaiterListManager.cpp:100`
  (sync TA wait polls `hasTerminationRequest` each quantum) and the
  per-wait-node park `:139-327`: termination break exits via the tail that
  unlinks the waiter node under the owning listLock before returning
  Terminated (SD8/§E.5), so list invariants are restored before any
  control-flow diversion; SPEC-api I24 pins the observable ("never 'ok'
  or 'timed-out' under termination").
- Lock/Condition/ThreadAtomics — `runtime/LockObject.cpp:481-595`,
  `runtime/ConditionObject.cpp:158-176`, `runtime/ThreadAtomics.cpp:1524-1571`:
  same predicate pair.
- The poll predicates themselves obey U2's bound: they read only atomic
  trap words + the request flag and take no lock
  (`parkLitePollTerminationRequested`, `runtime/VMTraps.cpp:1314-1335`;
  GIL-on form `jsThreadParkTerminationRequested`,
  `runtime/LockObject.cpp:586-595`) — legal under a rank-3 listLock, so
  polling can never deadlock against the state it must restore.

Adversarial probe — the one genuinely delicate window: **W1
service-vs-notify race** (r15 F2 disposition (a)). A parked *carrier*
observing `NeedWatchdogCheck` runs the full §J.3 reacquisition and services
`Watchdog::shouldTerminate` on its own thread; on a terminate verdict
`fireTerminationVMWideAfterParkedCarrierService`
(`runtime/VMTraps.cpp:1127-1152`) pre-sets the consumed-by-carrier shield on
the SD8-fail premise. The recorded CAVEAT (`:1141-1150`): a racing notify
that dequeued the parked waiter DURING the service window falsifies the
premise — the park completes "ok", the carrier has NOT serviced the
termination, and the shield would let the host's clear-and-re-enter
swallow it. The park sites are responsible for revoking (re-raising)
in that disposition; current revokes live in `waitSyncWithPerWaitNode`
(WaiterListManager.cpp) and ConditionObject's wait loop. This is a
multi-party protocol with a caller-side obligation — exactly the kind of
seam that rots.

**Verdict: needs-test** —
`JSTests/threads/cve/mc-aint-terminate-notify-park-race.js` (watchdog
termination racing a notify storm against a re-parking carrier; oracle:
termination is never lost — the run must end terminated, never complete
normally, never hang). Amplifier-ready; the lost-termination failure mode
presents as a hang the runner timeout catches.

Side note (stale doc, not a hole): the `*** WIRING STATUS ***` banner at
`runtime/Watchdog.h:61-86` claims the park sites do not yet drive W1 and
still fold NeedWatchdogCheck terminally. The code has moved past it: the
GIL-off predicate split is landed (`parkLitePollTerminationRequested`
GIL-off arm excludes the watchdog bit, `VMTraps.cpp:1329`;
`parkLitePollWatchdogCheckRequested` `:1337`) and join/Condition/
ThreadAtomics/WaiterListManager all drive
`reacquireParkedCarrierAndServiceWatchdogCheck` (`runtime/JSLock.cpp:846`).
The folded form survives only as the GIL-on arm (landed semantics, where
it is sound — single carrier). Recommend refreshing the banner so a future
reviewer does not "re-fix" it.

## S4. Cooperative stop delivered at a poll inside a TTL-elision window — **CLOSED (AB-10): I21(b) landed**

**Closure (verified 2026-06-15):** the I21(b) gap is closed at three levels:

- Parser: `ByteCodeParser::handleCheckTraps`
  (`dfg/DFGByteCodeParser.cpp:7037-7057`) emits `ExitOK` +
  `InvalidationPoint` immediately after every flag-on CheckTraps (the
  in-function AB-10 closure banner; unlinked plans excluded — not used
  flag-on).
- Clobberize: `dfg/DFGClobberize.h:809-850` — flag-on CheckTraps now writes
  `Watchpoint_fire` and defs `InvalidationPointLoc` exactly like
  InvalidationPoint (the *checktraps-dejank-invalidation-point* model), so
  `DFGInvalidationPointInjectionPhase` and CSE treat the poll as a hard
  fence; the write-before-def order is documented as load-bearing so CSE
  cannot replace a CheckTraps with an earlier one across a backedge.
- Codegen: `SpeculativeJIT::compileCheckTraps`
  (`dfg/DFGSpeculativeJIT.cpp:2574`) and FTL `compileCheckTraps`
  (`ftl/FTLLowerDFGToB3.cpp:20335`) plant an InvalidationPoint-shaped
  jump-replacement landing pad at the poll's rejoin. A mutator whose park
  in `VMTraps::handleTraps` overlapped a conductor heap-fact rewrite
  (`JSThreadsSafepoint::noteConductorHeapFactRewrite` BUMP-EDGE LAW,
  `bytecode/JSThreadsSafepoint.cpp`; site table
  `docs/threads/AUDIT-checktraps.md`) jettisons its on-stack optimizing-JIT
  code on resume, firing this landing pad — execution OSR-exits at the poll
  BEFORE any hoisted heap fact is reused against the rewritten heap.

`mc-aint-poll-resume-stale-elided.js`: 20/20 + 40/40 GIL-off full-tier
runs, zero oracle hits. GIL-on disposition: the test PREMISE-SKIPs
(runner-recognized `THREADS-PREMISE-SKIP:` marker) via a MODE-DERIVED
gate — it reads `$vm.useThreadGIL()` (the post-U0-validation effective
mode; `--useDollarVM=1` in the header) instead of the original behavioural
spawn-and-spin probe, which decided "cooperative GIL" from a 2s no-progress
deadline and could therefore misfire on a saturated host, silently
premise-skipping the exact GIL-off lane that pins this closure. Its
progress assertions (checks/foreignRounds > 0) assert cross-thread progress
against a never-blocking main driver, which SPEC-api Deviation 9
(cooperative-only preemption; 5.2 blocking primitives are the only yield
points) explicitly does not promise, and the I21(b) window itself is closed
by construction GIL-on (sole mutator runs fires inline). Test-side fix, not
an engine concession: inserting parks into the hot loops to "fix" GIL-on
progress would gut the GIL-off poll-resume window the oracle exists to
catch. (One observed failure in 80 runs was the SAFE-family
gcwait-vs-classa stop deadlock — shared GC conducted in a mutator thread,
parallel markers blocked on a CodeBlock ConcurrentJSLock, Class-A conductor
30s watchdog — owned by mc-safe-gcwait-vs-classa-stop, not this oracle.)

What IS handled on the *lifetime* half: jettison-time IC state is routed
through `RetiredJITArtifacts` precisely because "mutators resumed after
this stop keep executing this code … until their next invalidation point,
I21" (`bytecode/CodeBlock.cpp:2671-2702`). So resumed stale code won't UAF
its own metadata; the AB-10 closure removes the *semantic* hazard (executing
against the falsified fact: an E1-elided flat read on a butterfly that
became segmented/SW during the stop, or an E2-elided write skipping the SW
branch).

Residual notes (regression-risk, not live holes): the Task-13
poll-placement LINT chartered in SPEC-jit is still unbuilt —
`validateButterflyTagDiscipline` is an OptionsList stub
(`runtime/OptionsList.h:696`) with no validator behind it. The cross-doc
correction recorded in the original finding (map-MC-CODE.md S1 cited I21(b)
as in force before it landed) is now factually true.

**Verdict: immune-by-construction (closed; pinned by needs-test
`mc-aint-poll-resume-stale-elided.js`).** Original finding (the gap as
observed pre-AB-10) preserved in this file's git history for the audit
trail.

## S5. Watchdog (timer-thread "interrupt") — annex W

The watchdog timer fires on its own thread — historically the kind of
context that suspends or aborts. Here `timerDidFire` under `m_lock` only
raises `notifyNeedWatchdogCheck()` (a carrier-serviced trap bit, rule-3
exemption `VMTraps.cpp:924`) when any carrier is entered-or-parked;
the `shouldTerminate` embedder callback runs on the *carrier's own thread*
under its token (entered service, or the W1 parked-carrier episode,
`runtime/Watchdog.h` W1 / `runtime/JSLock.cpp:846`); the W3 no-carrier arm
evaluates wall-clock on the timer thread but delivers only via the rule-3
bit fan-out. No path suspends, signals, or diverts a mutator.
**Verdict: immune-by-construction** (modulo the S3 W1-revoke race, tested
there; and the stale Watchdog.h banner, noted there).

## S6. Stop-progress watchdog: fail-stop, not async abort

`bytecode/JSThreadsSafepoint.cpp:673-738` (`watchdogAssertStopProgress`):
a mutator that fails to reach the stop within 30s trips a RELEASE_ASSERT
on the *conductor* — the design deliberately converts "interrupt the
non-cooperating thread" into "crash the process with a diagnostic". That
is the correct anti-MC-AINT posture (never async-abort the offender);
worst case is DoS-by-crash, which the assert message routes to the D9
audit (park sites that hold heap access without quantum-polling).
**Verdict: immune-by-construction.**

## S7. Suspend-for-scan (conservative stack scan)

Legacy `MachineThreads` suspension interrupts a thread asynchronously but
only *inspects* it and resumes at the identical PC — no control-flow
diversion, no invariant can be broken by the interrupted thread's own
schedule. Flag-on shared-heap stops are cooperative anyway (R1.f; no
thread-suspend call exists in `JSThreadsSafepoint.cpp` — mutators park at
polls/quanta). The complement discipline — that *interruption points never
sit inside invariant-breaking windows* — is the poll-free-window rule
family: jit I16 (enforced as a pinned effectful patchpoint in FTL,
`ftl/FTLLowerDFGToB3.cpp:6219-6372`, `:13520-13521`, so B3 cannot CSE the
predicate load across a poll), OM I29 (no poll/alloc between validation and
StructureID store), OM I20/O2 (no safepoint under cell/Structure locks).
**Verdict: immune-by-construction** for the scan itself; the mechanical
enforcement gap for the window rules is the same missing lint recorded in
S4.

---

## Verdict table

| # | Surface | Governing invariant | Verdict |
|---|---------|---------------------|---------|
| S1 | SignalSender / trap-breakpoint patching (`VMTraps.cpp:315-450`) | SPEC-jit M2b/I21(a)/I2; UNGIL §A.2.5 | immune (triple-gated off flag-on) |
| S2 | Termination raise+delivery (`VMTraps.cpp:1016-1152`; no terminate API) | UNGIL §A.2.4 TERM1.1/1.2, rule 3 | immune (bit fan-out + poll only) |
| S3 | Park sites under termination; W1 revoke race (`ThreadObject.cpp:492`, `WaiterListManager.cpp:139`, `VMTraps.cpp:1127-1152`) | UNGIL D9/§A.2.4 rule 4, annex W; api I24 | needs-test (`mc-aint-terminate-notify-park-race.js`) |
| S4 | Poll-site resume across Class-A fire/jettison (`DFGByteCodeParser.cpp:7037`, `DFGClobberize.h:809`, `DFGSpeculativeJIT.cpp:2574`, `FTLLowerDFGToB3.cpp:20335`) | SPEC-jit I21(b)/I8, OM I13 | immune — CLOSED AB-10; pinned by `mc-aint-poll-resume-stale-elided.js`; Task-13 lint still unbuilt |
| S5 | Watchdog timer thread (annex W W0-W4) | UNGIL §A.2.8 | immune (stale Watchdog.h:61 banner noted) |
| S6 | Stop-progress RELEASE_ASSERT (`JSThreadsSafepoint.cpp:673-738`) | D9/FIX-2 | immune (fail-stop by design) |
| S7 | Suspend-for-scan / poll-free-window discipline | R1.f; jit I16, OM I20/I29/O2 | immune (lint gap shared with S4) |
