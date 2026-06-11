# STW-watchdog abort under watchpoint storm — closure evidence

Status: CLOSED. The Class-A stop-the-world watchdog abort
(`watchdogAssertStopProgress`, "JSThreads stop-the-world failed to reach a
stopped world within 30.000000s", richards-like N>=4 / string-heavy N=8 in
the SCALEBENCH parallel-self suite) is fully explained and fixed by the
landed fire-before-lock change in `Source/JavaScriptCore/bytecode/Repatch.cpp`.
This file records the auditable post-fix campaign evidence (commands, counts,
build identity) per fix-review amendment; the full investigation pack lives at
`Tools/threads/bughunt/EVIDENCE.md` (sections 0-11).

## Root cause (one sentence)

`tryCachePutBy`'s ExistingProperty branch fired the per-(structure, offset)
property-replacement WatchpointSet (`Structure::didCachePropertyReplacement`,
a Class-A stop-requesting fire under GIL-off) WHILE HOLDING
`codeBlock->m_lock`; sibling mutators blocked on that same lock in
`tryCacheGetBy`/`tryCachePutBy` locker entry hold heap access and are counted
non-quiescent by the §A.3.2 conductor predicate forever — a 2-to-4-party
deadlock cycle that the 30s watchdog converts into SIGABRT. 69/69 abort cores
(plus 11/11 WD-3 and 18/18 insurance cores) carry exactly this signature.
The "multiple non-quiescent lites" in the report are the blocked lock
acquirers (victims), not concurrent conductors; §A.3.3/rule-5 arbitration
behaved as specified (the SPEC-ungil r33-35 fan-in amendments are gated on
`useConcurrentSharedGCMarking`, which has zero hits in Source/ — frozen
rule-5 is operative).

## The landed fix (already in tree before this round)

All in `Source/JavaScriptCore/bytecode/Repatch.cpp`:

1. Replace-put fire-before-lock: `didCachePropertyReplacement` runs in a
   pre-lock block (~:1278) strictly before the `GCSafeConcurrentJSLocker`
   on `codeBlock->m_lock` (~:1283). Inside the locker, the JSThreads branch
   (~:1405-1422) never fires; if the pre-lock fire and the locked branch
   disagree (set still watched), it REFUSES to publish the Replace case and
   returns RetryCacheLater — invariant preserved, not weakened.
2. gilOff inline dictionary-flatten skip (`actionForCell`, :462-474): the
   flag-on flatten routes through a §10.6 per-event stop; requesting a stop
   while holding a lock other access-holders block on is the same wedge —
   skipped under gilOff (RetryCacheLater; perf forgone, never correctness).
3. `fireWatchpointsAndClearStubIfNeeded` fires before taking the lock.

This restores SPEC-jit App. 5.6(c) (no direct fireAll under a §7 lock) at the
one call site the Task-11 audit table missed (the audit's only
`firePropertyReplacementWatchpointSet` bucket-(iii) row was the
EnsureWatchability/addAccessCase path; M6.1's deferred-fire parameter landed
but was not passed at the Repatch site).

## The change landed THIS round (ratchet only, no behavior)

`Source/JavaScriptCore/yarr/Yarr.h`: comment +
`static_assert(matchLimit <= 100000000, ...)` pinning the boundedness
invariant the §A.3.2 quiescence argument relies on for the sole remaining
poll-free heap-access region (the Yarr interpreter: zero CheckTraps/safepoint
sites in yarr/; the only bound is `matchLimit`, decremented per
`matchDisjunction` entry at YarrInterpreter.cpp:1776 with the ErrorHitLimit
return at :1778, counter init at :2285; the Yarr JIT enforces the same bound,
YarrJIT.cpp). Measured worst-case wall at the current value: ~1.8s
interpreter / ~0.9s JIT — far under the 30s watchdog. Codegen-identical;
raising matchLimit now requires consciously revisiting the stop-latency bound.

## Build identity

- Branch `jarred/threads`, HEAD `a0291c7f8f0d` at campaign time (includes the
  GC under-marking fix 25375a997f4f and the Repatch.cpp fire-before-lock
  fix; both predate this round's working-tree delta, which is the Yarr.h
  ratchet + this file).
- Binary: `WebKitBuild/Release/bin/jsc` (Release, Linux x86_64, 64-core
  host). Pre-fix campaign binary mtime 2026-06-10 12:20 UTC (post-25375a997f4f
  rebuild, verified no-work `ninja jsc`); landed-fix campaign binary mtime
  2026-06-10 16:04 UTC. Post-ratchet rebuild for the verification campaigns
  below (ratchet is compile-time only).
- Pinned GIL-off flags (every run unless noted): `--useJSThreads=1
  --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
  --useSharedGCHeap=1 --useThreadGILOffUnsafe=1`.

## Campaign record — pre-fix (reproduction), post-25375a997f4f rebuild

Driver: `Tools/threads/bughunt/stw-watchdog/campaign.sh <runs> <N> <workload>
<tag> [extra flags]` with `SCALE=...`; workloads
`JSTests/threads/scaling/richards-like.js`, `.../string-heavy.js`,
`Tools/threads/bughunt/repro.js` (focused 70-line distillation). Abort =
rc 134 + watchdog report. Full logs: `Tools/threads/bughunt/stw-watchdog/`.

| config | runs | watchdog aborts |
|---|---|---|
| richards N=4 scale=1 | 12 | 1 (8%) |
| richards N=8 scale=1 | 10 | 1 (10%) |
| string-heavy N=8 scale=1 | 10 | 0 (SCALEBENCH saw 1; low-rate) |
| richards N=4 scale=1/16 | 40 | 20 (50%) |
| richards N=8 scale=1/16 | 40 | 21 (53%) |
| richards N=3 scale=1/16 | 40 | 11 (28%) |
| richards N=2 scale=1/16 | 40 | 5 (12% — two threads suffice) |
| richards N=4 1/16 `--useJIT=0` | 25 | 0 (LLInt fires before locking — no repro) |
| richards N=4 1/16 `--useDFGJIT=0` | 25 | 10 (40%) |
| richards N=4 1/16 `--useFTLJIT=0` | 25 | 21 (84% — highest-rate amplifier) |
| repro.js N=4 | 15 | 11 (73%) |
| control re-confirmation post-GC-fix (repro.js N=4) | 5 | 4 (80%) |

## Campaign record — landed fix (same binary lineage, fix in)

| config | runs | result |
|---|---|---|
| Experiment-A (fix shape) repro.js N=4 | 30 | 0 aborts |
| Experiment-A richards N=4 1/16 noFTL | 25 | 0 aborts |
| Experiment-A richards N=2 1/16 | 40 | 0 aborts |
| Experiment-A richards N=8 1/16 | 15 | 0 aborts |
| landed fix string-heavy N=8 scale=1 | 40 | 0 aborts |
| landed fix richards N=4 1/16 noFTL | 100 | 0 aborts (150s external gate; 0 timeouts) |
| landed fix repro.js N=4 | 30 | 0 aborts |

Cumulative landed-fix evidence at fix review: 0 aborts in 280 runs vs 50-84%
pre-fix at the same configs. Defect-restored insurance runs (env-gated
`JSC_BUGHUNT_WD3_STOCK=1` scratch binary) reproduce 11/20 and 18/20 with
frozen window counts (windowsAtArm == windowsAtAbort) — the watchdog is a
true positive of the deadlock, not an accounting defect.

## Verification campaigns for THIS round (post-ratchet rebuild, 2026-06-10)

Binary: `WebKitBuild/Release/bin/jsc` rebuilt 17:02 UTC with the Yarr.h
ratchet (ninja jsc, 206 targets, clean). Driver:
`Tools/threads/bughunt/stw-watchdog/campaign.sh` (abort = rc 134 + watchdog
report; external timeout classifies silent hangs as FAILURE per
EVIDENCE.md §10.3). Logs: `stw-watchdog/logs/v9-*`.

(1) Failing seeds from the evidence pack, 20x each — **100/100 PASS, 0
aborts, 0 timeouts**:

| seed | runs | result |
|---|---|---|
| repro.js N=4 | 20 | 20 PASS |
| richards N=4 scale=1/16 `--useFTLJIT=0` (84% pre-fix) | 20 | 20 PASS |
| richards N=2 scale=1/16 (2-party form) | 20 | 20 PASS |
| richards N=8 scale=1/16 (53% pre-fix) | 20 | 20 PASS |
| string-heavy N=8 scale=1 | 20 | 20 PASS |

(2) 240 runs under 6-way load (6 concurrent campaign workers x 40 runs of
richards N=4 scale=1/16 `--useFTLJIT=0`, workers as mutual load):
**240/240 PASS, 0 aborts, 0 timeouts**.

(3) Full GIL-off corpus (`run-tests.sh` under the pinned ambient env above):
**94 passed / 0 failed / 3 skipped** (the 93/0 expectation predates one
corpus addition; 0 failures is the gate). races/ x5: **7/0 each, 35/35**.

(4) GIL-on corpus (plain `run-tests.sh`): **95 passed / 0 failed / 2 skipped**.

(5) Flag-off smoke (default flags, no threads options): regexp-match,
put-by-id-direct-transition, put-by-id-megamorphic-have-a-bad-time,
put-by-id-build-list-order-recurse, put-by-id-direct-strict-transition,
string-rope-with-custom-valueof — **all rc=0**.

Cumulative landed-fix total: **0 watchdog aborts in 695 targeted runs**
(280 at fix review + 415 this round) vs 8-84% pre-fix.

## Perf facet (handed to TTL-rebias / de-jank charter, NOT fixed here)

Direct measurement REFUTED the SCALEBENCH §6.2 "watchpoint storm" frequency
hypothesis: at the config where T(2) ~ 14x T(1) (richards scale 1, N=2:
48.8s wall vs ~3.7s single-thread), total time in all 54 Class-A/§A.3 stop
windows is 1.6 ms (~0.003% of wall); stop-fire count is constant (~36/run,
warmup-phase one-shots: 17x didReplaceProperty, 10x
didCachePropertyReplacement, plus singletons) regardless of N and scale.
Whatever serializes richards GIL-off, it is NOT stop frequency or stop
latency — mid-run samples show JS threads executing JIT code. Fire-identity
log: `Tools/threads/bughunt/stw-watchdog/firelog-n4.txt`. Additional handoff:
a pathological regexp can stall a Class-A stop up to ~1.8s (bounded jank;
a CheckTraps-style poll in matchDisjunction would close it), and the
in-window-conductor watchdog blind spot (EVIDENCE.md §10.2-10.3) belongs to
the watchdog-hardening charter.
