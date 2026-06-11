# MC-AINT — asynchronous interruption at an unsafe point: mapping to our threads surface

Mechanism class (from the catalog; web-derived exemplars treated as data):
*"Asynchronous interruption at an unsafe point: interrupt/abort/termination
delivered between invariant-breaking and invariant-restoring instructions.
Design rule: thread termination must be safepoint-polled, never delivered
asynchronously."* Exemplars: Thread.Abort partial-trust escape lineage
(removed in .NET Core), POSIX signal-in-VM bug families.

Audited against the tree at jarred/threads (phase-1 GIL'd API landed,
UNGIL §A.2 trap/termination machinery + AB-17/AB-17b landed, GIL-off
bring-up in progress). Specs of record: SPEC-jit.md (frozen; I2/I16/I21),
SPEC-ungil.md rev 32 + BINDING annexes via UNGIL-HANDOUT.md (§A.2.4 TERM1,
§A.2.5, §A.2.8 annex W, D9 park-quanta rule), SPEC-api.md §5.6/I24,
SPEC-objectmodel.md (I20/I29/O2 poll-free-window discipline).

Verdict legend: **immune** = immune-by-construction (protocol cited, with
the adversarial argument), **needs-test** = susceptibility test written
under `JSTests/threads/cve/` (run post-ungil), **suspected** =
susceptible-suspected with the precise hole.

Summary: the architecture *is* the design rule — termination and stops are
trap-bit + cooperative-poll only, the JS-level abort surface does not exist
(TERM1.1), and the signal path is compiled out of reach flag-on. The one
real hole found is S4: SPEC-jit **I21(b) is specified but NOT implemented**
— flag-on DFG/FTL trap polls are not invalidation points, so a mutator
parked at a poll across a Class-A fire resumes into stale TTL-elided code.
S4 also falsifies the immunity argument recorded at
`docs/threads/cve/map-MC-CODE.md:31-34` (S1 there cites I21 as in force).

---

## S1. Signal-based VMTraps delivery (the literal POSIX signal-in-VM analog)

The legacy delivery mechanism is the textbook MC-AINT surface: a
`SignalSender` work-queue thread suspends the mutator and installs halt
breakpoints into *running* JIT code from outside
(`runtime/VMTraps.cpp:293-441`; `tryInstallTrapBreakpoints` at `:417`,
AccessFault handler at `:307-330`). If that ever ran against N mutators it
would deliver interruption at arbitrary instruction boundaries — between
any invariant-breaking and invariant-restoring pair.

**Verdict: immune-by-construction (flag-on).** Three independent gates:

1. `runtime/Options.cpp:920` (inside `notifyOptionsChanged()`, which runs
   on every options batch and is final at `Options.cpp:1240-1242`):
   `useJSThreads` forces `Options::usePollingTraps() = true` (SPEC-jit M2b,
   I21(a)). A hostile `--usePollingTraps=false --useJSThreads=1` command
   line is overridden because the forcing runs *after* parsing, in the
   finalize hook; no later code path under `useJSThreads` sets it back
   (the other assignments at `:616/:711/:724` also force true).
2. `VMTraps::initializeSignals()` (`runtime/VMTraps.cpp:455-462`) installs
   the signal handlers only when `!usePollingTraps` — flag-on the handlers
   are never registered at all.
3. `requestThreadStopIfNeeded` (`runtime/VMTraps.cpp:499-510`) gates
   SignalSender creation on `!Options::usePollingTraps() && !vm.gilOff()`
   (UNGIL §A.2.5: "no single ownerThread to suspend; trap-breakpoint
   installation assumes one mutator stack"). So even if gate 1 rotted, a
   gilOff VM still never constructs a SignalSender; the per-lite VMTraps
   instances additionally assert the SignalSender machinery is per-lite
   unreachable (`VMTraps.cpp:90-95`).

Adversarial residue: flag-off (vanilla JSC) keeps signal delivery — that is
upstream's threat model, out of audit scope. Debugger break
(`NeedDebuggerBreak`) stays on the landed carrier protocol (rule-3
exemption, `VMTraps.cpp:864-867`) — still bit+poll, not signal, flag-on.

## S2. Thread termination delivery (the Thread.Abort analog)

- **No JS-level abort surface exists.** TERM1.1 (UNGIL-HANDOUT.md:320-337):
  `Thread.prototype.terminate` DOES NOT EXIST in v1; the Thread surface is
  constructor/join/asyncJoin/id/tls (verified: no terminate host function in
  `runtime/ThreadObject.cpp` — the only "terminate" tokens are local poll
  results, e.g. `:420`). The .NET-lineage fix (remove the API) is adopted by
  construction. A thread-targeted terminate is POST-UNGIL chartered only.
- **Raising termination is bit-only.** `VMTraps::fireTrapVMWide`
  (`runtime/VMTraps.cpp:850-905`): under the leaf registry lock it performs
  only `exchangeOr` of trap bits into every sibling lite word + the VM word
  (§A.2.3 rule 3) — it never suspends a thread, never patches code, never
  touches a foreign stack. Same for the sibling fan
  `fanOutTerminationToSiblingLites` (`:906-961`) and the watchdog W3
  timer-thread raise (`runtime/Watchdog.h` W3 — rule-3 fan-out only).
- **Observing termination is poll-only.** Delivery points are exactly: the
  generated-code trap polls (LLInt `op_check_traps`, Baseline/DFG/FTL
  CheckTraps -> `operationHandleTraps`, `jit/JITOperations.cpp:3007-3022`),
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
(`runtime/VMTraps.cpp:516-536`).

Sites and their invariant-restoration discipline:

- `Thread.prototype.join` — `runtime/ThreadObject.cpp:438-487`: quantum
  loop holds `joinLock` only between sleeps; on a termination poll hit it
  exits the park, ends the GIL-dropped section, and only then (back under
  the GIL, **no native lock held**) does request-then-throw
  (`vm.setHasTerminationRequest(); vm.throwTerminationException()`,
  `:475-486`).
- Property/TA `Atomics.wait` — `runtime/WaiterListManager.cpp:99-103`
  (sync TA wait polls `hasTerminationRequest` each quantum) and the
  per-wait-node park `:134-300`: termination break exits via the tail that
  unlinks the waiter node under the owning listLock before returning
  Terminated (SD8/§E.5), so list invariants are restored before any
  control-flow diversion; SPEC-api I24 pins the observable ("never 'ok'
  or 'timed-out' under termination").
- Lock/Condition/ThreadAtomics — `runtime/LockObject.cpp:334`,
  `runtime/ConditionObject.cpp:171-250`, `runtime/ThreadAtomics.cpp:1032+`:
  same predicate pair.
- The poll predicates themselves obey U2's bound: they read only atomic
  trap words + the request flag and take no lock
  (`parkLitePollTerminationRequested`, `runtime/VMTraps.cpp:1148-1169`;
  GIL-on form `jsThreadParkTerminationRequested`,
  `runtime/LockObject.cpp:346-355`) — legal under a rank-3 listLock, so
  polling can never deadlock against the state it must restore.

Adversarial probe — the one genuinely delicate window: **W1
service-vs-notify race** (r15 F2 disposition (a)). A parked *carrier*
observing `NeedWatchdogCheck` runs the full §J.3 reacquisition and services
`Watchdog::shouldTerminate` on its own thread; on a terminate verdict
`fireTerminationVMWideAfterParkedCarrierService`
(`runtime/VMTraps.cpp:963-986`) pre-sets the consumed-by-carrier shield on
the SD8-fail premise. The recorded CAVEAT (`:973-985`): a racing notify
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
`runtime/Watchdog.h:61-72` claims the park sites do not yet drive W1 and
still fold NeedWatchdogCheck terminally. The code has moved past it: the
GIL-off predicate split is landed (`parkLitePollTerminationRequested`
GIL-off arm excludes the watchdog bit, `VMTraps.cpp:1163-1169`;
`parkLitePollWatchdogCheckRequested` `:1171+`) and join/Condition/
ThreadAtomics/WaiterListManager all drive
`reacquireParkedCarrierAndServiceWatchdogCheck` (`runtime/JSLock.cpp:846`).
The folded form survives only as the GIL-on arm (landed semantics, where
it is sound — single carrier). Recommend refreshing the banner so a future
reviewer does not "re-fix" it.

## S4. Cooperative stop delivered at a poll inside a TTL-elision window — **CLOSED 2026-06-10: I21(b) landed**

**Closure:** `ByteCodeParser::handleCheckTraps` (DFGByteCodeParser.cpp) now
emits `ExitOK` + `InvalidationPoint` immediately after every flag-on
CheckTraps (the AB-10 closure banner in-function), so a mutator parked at a
poll across a Class-A fire resumes into the patched exit, never across
jettisoned elided code. mc-aint-poll-resume-stale-elided.js: 20/20 + 40/40
GIL-off full-tier runs, zero oracle hits. GIL-on disposition (2026-06-10):
the test PREMISE-SKIPs (runner-recognized `THREADS-PREMISE-SKIP:` marker)
via a MODE-DERIVED gate — since 2026-06-10 it reads `$vm.useThreadGIL()`
(the post-U0-validation effective mode; `--useDollarVM=1` in the header)
instead of the original behavioral spawn-and-spin probe, which decided
"cooperative GIL" from a 2s no-progress deadline and could therefore
misfire on a saturated host, silently premise-skipping the exact GIL-off
lane that pins this closure. Its progress assertions
(checks/foreignRounds > 0) assert cross-thread progress against a
never-blocking main driver, which SPEC-api Deviation 9 (cooperative-only
preemption; 5.2 blocking primitives are the only yield points) explicitly
does not promise, and the I21(b) window itself is closed by construction
GIL-on (sole mutator runs fires inline). Test-side fix, not an engine
concession: inserting parks into the hot loops to "fix" GIL-on progress
would gut the GIL-off poll-resume window the oracle exists to catch. (One observed failure in 80 runs
was the SAFE-family gcwait-vs-classa stop deadlock — shared GC conducted in
a mutator thread, parallel markers blocked on a CodeBlock ConcurrentJSLock,
Class-A conductor 30s watchdog — owned by mc-safe-gcwait-vs-classa-stop,
not this oracle.) The Task-13 poll-placement LINT chartered below is still
unbuilt — regression-risk note, not a live hole. Original finding follows:

This is MC-AINT with the stop itself as the interruption: GIL-off, a
Class-A watchpoint fire runs as an STWR (SPEC-jit §5.6); every other
mutator parks at its next trap poll; the fire falsifies a watched fact and
jettisons/invalidates the elided code (SPEC-jit §5.3/I8); the parked
mutators then resume **at the instruction after the poll**. The interval
[poll, next invalidation point/exit] is an
invariant-breaking-to-invariant-restoring window: code in it may contain
E1/E2-elided butterfly accesses (`dfg/DFGDesiredWatchpoints.cpp:165-180` —
elision IS landed) whose soundness depended on the fact that just died.

The frozen spec closes this with I21(b) (SPEC-jit.md:200; history §313
blocker): *flag-on, every DFG/FTL cooperative poll is immediately followed
by an invalidation point (CheckTraps emits one), so parked mutators resume
into the patched exit, never across jettisoned elided code.*

**The tree does not implement it:**

- `dfg/DFGByteCodeParser.cpp:7039` (`handleCheckTraps`): emits
  `usePollingTraps() ? CheckTraps : InvalidationPoint` — flag-on (polling
  forced, S1 gate 1) it emits **CheckTraps only, never an
  InvalidationPoint**, at every trap-poll site.
- `dfg/DFGSpeculativeJIT.cpp:2552-2564` (`compileCheckTraps`): plain
  bit-test + slow-path call; no jump-replacement landing pad, no
  `useJSThreads` branch.
- `ftl/FTLLowerDFGToB3.cpp:20230-20252` (`compileCheckTraps`): same shape.
- `dfg/DFGClobberize.h:617-620`: CheckTraps reads/writes `InternalState`
  only — it does NOT write `Watchpoint_fire`, so
  `DFGInvalidationPointInjectionPhase` (`:82-97`, which inserts
  InvalidationPoints only after `Watchpoint_fire`-writing nodes) inserts
  nothing after it.
- `jit/JITOperations.cpp:3007-3022` (`operationHandleTraps`): services
  traps (and is the park site for the stop) and returns straight to the
  post-poll PC; no jettisoned-CodeBlock check on the way out.
- The I21 poll-placement lint chartered for Task 13 does not exist:
  `validateButterflyTagDiscipline` is an OptionsList stub
  (`runtime/OptionsList.h:688`) with no validator behind it, and no
  DFGValidate/B3 pass checks poll->elided-access windows.

What IS handled is the *lifetime* half: jettison-time IC state is routed
through `RetiredJITArtifacts` precisely because "mutators resumed after
this stop keep executing this code … until their next invalidation point,
I21" (`bytecode/CodeBlock.cpp:2409-2415`,
`bytecode/PropertyInlineCache.cpp:145-160`). So resumed stale code won't
UAF its own metadata — it will instead *semantically* execute against the
falsified fact: an E1-elided flat read on a butterfly that became
segmented/SW during the stop (mask alone does not detect regime change),
or an E2-elided write skipping the SW branch. That is type confusion /
torn-state territory, not just staleness.

Reachability: GIL-off only (GIL-on the sole mutator runs fires inline and
is never parked at a poll across one — phase-1 closes the window by
construction). Baseline/LLInt are unaffected (no elision below DFG;
checks always emitted, D9).

**Verdict: susceptible-suspected + needs-test** —
`JSTests/threads/cve/mc-aint-poll-resume-stale-elided.js` (readers hot in
elided DFG/FTL loops; owner triggers synchronous Class-A fires — first
foreign write/transition — so each fire parks the readers at polls and
jettisons their code; oracle: sentinel-set discipline on the values readers
observe post-resume). Amplifier-ready; EXECUTED POST-UNGIL ONLY.
Also: correct `map-MC-CODE.md` S1/preamble (`:31-34`), which records this
property as in force — as written it is an immunity claim citing an
unimplemented invariant.

Fix shape (for the eventual thread-fix item, not applied here — read-only
audit): either make handleCheckTraps emit `CheckTraps` **plus**
`InvalidationPoint` under `Options::useJSThreads()`, or give CheckTraps a
`write(Watchpoint_fire)` flag-on so the injection phase places the
invalidation point, or plant the invalidation landing pad inside
`compileCheckTraps` itself; plus the Task-13 lint so it cannot regress.

## S5. Watchdog (timer-thread "interrupt") — annex W

The watchdog timer fires on its own thread — historically the kind of
context that suspends or aborts. Here `timerDidFire` under `m_lock` only
raises `notifyNeedWatchdogCheck()` (a carrier-serviced trap bit, rule-3
exemption `VMTraps.cpp:864-867`) when any carrier is entered-or-parked;
the `shouldTerminate` embedder callback runs on the *carrier's own thread*
under its token (entered service, or the W1 parked-carrier episode,
`runtime/Watchdog.h` W1 / `runtime/JSLock.cpp:846`); the W3 no-carrier arm
evaluates wall-clock on the timer thread but delivers only via the rule-3
bit fan-out. No path suspends, signals, or diverts a mutator.
**Verdict: immune-by-construction** (modulo the S3 W1-revoke race, tested
there; and the stale Watchdog.h banner, noted there).

## S6. Stop-progress watchdog: fail-stop, not async abort

`bytecode/JSThreadsSafepoint.cpp:405-414` (`watchdogAssertStopProgress`):
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
polls/quanta, `:416-447`). The complement discipline — that *interruption
points never sit inside invariant-breaking windows* — is the poll-free-
window rule family: jit I16 (enforced as a pinned effectful patchpoint in
FTL, `ftl/FTLLowerDFGToB3.cpp:13453-13467`, so B3 cannot CSE the predicate
load across a poll), OM I29 (no poll/alloc between validation and
StructureID store), OM I20/O2 (no safepoint under cell/Structure locks).
**Verdict: immune-by-construction** for the scan itself; the mechanical
enforcement gap for the window rules is the same missing lint recorded in
S4.

---

## Verdict table

| # | Surface | Governing invariant | Verdict |
|---|---------|---------------------|---------|
| S1 | SignalSender / trap-breakpoint patching (`VMTraps.cpp:293-441`) | SPEC-jit M2b/I21(a)/I2; UNGIL §A.2.5 | immune (triple-gated off flag-on) |
| S2 | Termination raise+delivery (`VMTraps.cpp:850-986`; no terminate API) | UNGIL §A.2.4 TERM1.1/1.2, rule 3 | immune (bit fan-out + poll only) |
| S3 | Park sites under termination; W1 revoke race (`ThreadObject.cpp:438`, `WaiterListManager.cpp:134`, `VMTraps.cpp:963-986`) | UNGIL D9/§A.2.4 rule 4, annex W; api I24 | needs-test (`mc-aint-terminate-notify-park-race.js`) |
| S4 | Poll-site resume across Class-A fire/jettison — I21(b) missing (`DFGByteCodeParser.cpp:7039`, `DFGClobberize.h:617`, `JITOperations.cpp:3007`) | SPEC-jit I21(b)/I8, OM I13 | **suspected** + needs-test (`mc-aint-poll-resume-stale-elided.js`); also fixes a false immunity cite in map-MC-CODE.md S1 |
| S5 | Watchdog timer thread (annex W W0-W4) | UNGIL §A.2.8 | immune (stale Watchdog.h:61 banner noted) |
| S6 | Stop-progress RELEASE_ASSERT (`JSThreadsSafepoint.cpp:405-414`) | D9/FIX-2 | immune (fail-stop by design) |
| S7 | Suspend-for-scan / poll-free-window discipline | R1.f; jit I16, OM I20/I29/O2 | immune (lint gap shared with S4) |
