# EVIDENCE PACK — Atomics.waitAsync settle chain UAF under gc()-in-drain (Debug-deterministic, FLAG-OFF reachable)

Date: 2026-06-10. Binary under test: `WebKitBuild/Debug/bin/jsc` (current
tree, post family-1/family-2 CVE-close landings; `ninja: no work to do` at
test time). Found by: `JSTests/threads/cve/mc-dos-waiter-table-storm.js`
crashing 6/6 on Debug GIL-off (it is 20/20 green on Release GIL-off and
GIL-on — the Release pass is MASKING, not absence; see §4). Facts only;
hypotheses are labeled as such.

## 0. Headline

A pending promise-job's `promiseOrCapability` argument is a SWEPT cell
(scribbled `0xbadbeef0` — `scribbleFreeCells()` is unconditionally on under
ASSERT_ENABLED, Scribble.h) by the time the job runs, when `gc()` runs
inside a microtask drain that was entered from `DeferredWorkTimer::doWork`
after `Atomics.waitAsync` settles. Deterministic on Debug (5/5, 6/6 across
repro variants). **Reproduces FLAG-OFF VANILLA (no options at all) on the
TypedArray/SAB lane** — this is NOT a threads-flag regression and none of
the jarred/threads family fixes are on the path (see §3).

## 1. Repros (all deterministic on Debug)

- `repro-uaf-reaction.js` — `./WebKitBuild/Debug/bin/jsc repro-uaf-reaction.js`
  (NO flags). 128× `Atomics.waitAsync(ta,0,0,60000)` + immediate notify;
  `Promise.all(...).then(async ...)` loops `gc(); await Promise.resolve()`.
  Crash: `ASSERTION FAILED: isSymbol()` JSCJSValue.cpp:198
  (`synthesizePrototype`), from JSMicrotask.cpp:1819
  `promiseOrCapability.get(globalObject, vm.propertyNames->resolve)` in
  `PromiseReactionJob` — promiseOrCapability is a scribbled (freed) cell.
  Stack: DeferredWorkTimer::doWork → VM::drainMicrotasks →
  MicrotaskQueue::drainImpl<1> → runInternalMicrotask.
- `repro-late-double-settle.js` — same shape, single waiter. Prints PASS,
  then at the 60s waitAsync deadline crashes
  `ASSERTION FAILED: status() == Status::Pending` (JSPromise.cpp:1117
  `fulfillPromise`) from `PromiseResolveWithoutHandlerJob` — a settle job
  fulfills an already-settled promise. Plausibly the same root cause
  presenting as cell-address reuse rather than a caught scribble
  (hypothesis, not established).
- `repro-property-lane.js` — same shape on the property lane
  (`{v:0}` cells), needs `--useJSThreads=1` (any GIL mode). Same
  isSymbol() signature. Shows the bug is lane-independent: the shared
  machinery (DWT doWork → settle → drain → gc) is what matters.

## 2. Established facts

- Flag-off vanilla repro → the threads option set is irrelevant
  (also crashes GIL-on and GIL-off; also with `--useVMLite=0` forced).
- Debug-only DETECTION: `scribbleFreeCells()` == ASSERT_ENABLED. On Release
  the freed cell's stale bits usually still "work", so Release runs pass —
  i.e. Release executes a use-after-free silently. This is why
  mc-dos-waiter-table-storm is 20/20 on Release and 0/6 on Debug.
- The crashing queue IS the VM's default MicrotaskQueue and IS registered
  in `vm.m_microtaskQueues` (gdb: sentinel list contains exactly the
  draining queue; `m_markedBefore == 0` at crash).
- `verifyGC=true` does not change the signature.
- Rooting every USER-VISIBLE promise (`r.value`, the `Promise.all`, the
  `.then` result, the TAs) makes all variants pass — but so do several
  smaller perturbations (dropping the `new WeakRef(ta)` per cell flips
  repro-uaf-reaction.js to PASS), so the bisection is TIMING/LAYOUT
  sensitive: do not read "WeakRef is the cause" from it. What the
  perturbation sensitivity does establish: the dead cell is in the promise
  reaction chain created by the test's own `then`/`await` graph, and
  whether it dies depends on heap layout at the gc() inside the drain.
- Two presentations, one window: (a) reaction job runs with a swept
  `promiseOrCapability`; (b) a late settle job fulfills a non-pending
  promise (address reuse is the unproven link).

## 3. NOT implicated (checked)

- Family-2 / DOS-S4 fix (`AsyncTicket::scheduleViaDeferredWorkTimer`
  `dwtTicket->cancel()` tail, ThreadManager.cpp): TA-lane `Atomics.waitAsync`
  is NOT an AsyncTicket (WaiterListManager.cpp:436 banner) and the vanilla
  repro never constructs one. Property-lane repro crashes GIL-ON too, where
  the settle takes the landed scheduleViaDeferredWorkTimer path — but the
  vanilla TA repro proves the bug exists with zero threads code on the path.
- Family-2 §C.3 re-validation (ThreadAtomics.cpp): property-lane flag-on
  only; vanilla repro unaffected.
- Family-1 generator claim/publish: not on any repro path.
- MicrotaskQueue foreign-inbox arms: gated on `VM::isGILOffProcess()`,
  false in the vanilla repro.

Suspect lineage (hypothesis): the recent upstream merge
(51cc3feb7298 "[JSC] Promise jobs must not run with the realm of a
cross-realm settle site" — touches exactly the JSMicrotask.cpp
promiseOrCapability handling) or the surrounding Bun microtask additions;
unverified — needs a pre-merge binary or a source diff to discriminate.

## 4. Disposition

- `mc-dos-waiter-table-storm.js` remains GREEN on the pinned CVE-suite
  verify (Release). Its Debug runs currently die on THIS bug before the
  test's own oracles run; the map/status docs have been corrected to say
  Release-only until this is fixed.
- This is a memory-safety bug (UAF observable in Release as silent stale
  reads / double-settle), reachable from vanilla `Atomics.waitAsync` — it
  should get its own thread-bughunter run; it is NOT a CVE-suite test
  failure and not part of the family-2 close.

## 5. Open questions for the hunt

1. Who is supposed to root the dead cell at the gc() moment — the queued
   `QueuedTask::m_arguments` (visitAggregate covers `m_queue`/`m_toKeep`),
   the DWT TicketData (nulled in doWork before `vm.drainMicrotasks()`),
   or the in-flight dequeued task local (conservative stack scan)?
   All three look individually sound in source; one of them is lying.
2. Why does `new WeakRef(ta)` (keepDuringJob version bump machinery)
   flip the layout into the failing shape?
3. Is presentation (b) really address reuse of (a)'s freed cell?
