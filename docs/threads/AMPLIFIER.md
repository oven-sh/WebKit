# Race amplifier

Part of the shared-memory `Thread` work (see `THREAD.md`, execution plan step
"adversarial race-hunting pass with TSAN (no-JIT config) and a
randomized-yield race amplifier").

Races in the new object model and N-mutator GC live in windows that are a few
instructions wide — a foreign-thread transition racing a flat→segmented
butterfly conversion, a watchpoint fire racing a non-atomic owner-thread
transition, a block handout racing a sweep. On an unloaded machine those
windows essentially never get hit. The amplifier widens them: at instrumented
safepoint-adjacent **slow-path** sites, it injects a randomized `sched_yield`
or a short sleep, turning astronomically rare interleavings into ones you can
reproduce in a 100-run loop.

It complements TSAN, not replaces it: TSAN finds unsynchronized accesses; the
amplifier finds *wrongly* synchronized logic (lost wakeups, publication-order
bugs, time-travel reads) that is data-race-free but still wrong, and it works
in JIT'd configurations where TSAN cannot.

## Components

| Path | What |
| --- | --- |
| `Source/JavaScriptCore/runtime/RaceAmplifier.{h,cpp}` | The in-VM hook: `RaceAmplifier::perturb()` |
| `Tools/threads/amplify.sh` | Driver: run a JS file M times under random seeds, report crash/hang/divergence |
| `docs/threads/INTEGRATE-amplifier.md` | Paste-ready `OptionsList.h` / `Sources.txt` / `VM.cpp` manifest (merged by the Stub phase) |

## JSC options

| Option | Default | Meaning |
| --- | --- | --- |
| `--randomYieldPeriod=N` | `0` (off) | Perturb on average once every N visits per thread to an instrumented site. `0` disables. |
| `--randomYieldSeed=S` | `0` | PRNG seed (32-bit). `0` picks a random seed and dataLogs it for replay. |
| `--randomYieldMaxMicroseconds=U` | `100` | Upper bound for injected sleeps. |

When enabled, JSC logs one line at startup:

```
[RaceAmplifier] enabled: period=64 seed=12345 maxSleepUs=100
```

Re-running with `--randomYieldSeed=12345` reproduces the same per-thread
perturbation decision streams (per-thread PRNGs are seeded from the global
seed mixed with a thread-creation ordinal, not OS thread ids), so replays are
deterministic modulo the OS scheduler itself.

### Behavior when firing

About 3/4 of perturbations are a bare `Thread::yield()` (fine-grained
interleaving churn); 1/4 sleep for a uniform 1..`maxSleepUs` microseconds
(parks the thread long enough for another thread to drive an entire slow path
through the window). Re-arm intervals are drawn uniformly from
`[1, 2*period)` so thread phases keep drifting instead of resonating with
loop trip counts.

### Cost when off

`RaceAmplifier::perturb()` compiles to one load of a global word plus a
never-taken predicted branch. It is only ever placed on slow paths (paths
that already lock, allocate, or fire watchpoints) — never in JIT-emitted fast
paths or LLInt assembly — so `--randomYieldPeriod=0` (the default) is free.
This keeps the amplifier permanently compiled in, including release builds,
which is what lets CI fuzz release binaries.

## The harness: `Tools/threads/amplify.sh`

```
Tools/threads/amplify.sh [options] /path/to/jsc script.js

  --runs M           runs (default 100)
  --period N         --randomYieldPeriod (default 64)
  --seed-base S      use seeds S+1..S+M instead of random (reproducible campaign)
  --max-sleep-us U   --randomYieldMaxMicroseconds (default 100)
  --timeout SECS     per-run timeout, counts as a hang (default 60, 0 = off)
  --jsc-arg ARG      extra jsc argument (repeatable)
  --keep-logs        keep logs of clean runs too
```

The first non-crashing run's stdout+stderr is the **reference**. Every run is
then classified:

- **CRASH** — exited by signal (segfault, assertion, etc.)
- **HANG** — exceeded `--timeout` (lost wakeup / deadlock smell)
- **DIVERGENCE** — exit status or output differs from the reference

Any finding prints the seed, the log path, and a one-line deterministic
replay command, keeps the log directory, and makes the harness exit 1.
Exit 0 means all runs clean; exit 2 is a usage error.

Note: test scripts should produce deterministic output for a *correct*
engine (fixed iteration counts, sorted result dumps, no timing prints), or
divergence detection degrades to crash/hang detection only. The
`JSTests/threads/` corpus follows this rule.

## Typical workflows

Smoke a single test hard:

```sh
Tools/threads/amplify.sh --runs 500 --period 16 \
    ./WebKitBuild/Debug/bin/jsc JSTests/threads/transition-races.js
```

Reproducible CI campaign (same seeds every time):

```sh
Tools/threads/amplify.sh --runs 200 --seed-base 1000 \
    ./WebKitBuild/Release/bin/jsc JSTests/threads/stress.js
```

Per-tier sweeps (each tier independently verifiable, per THREAD.md):

```sh
for cap in --useBaselineJIT=0 --useDFGJIT=0 --useFTLJIT=0 ""; do
    Tools/threads/amplify.sh --runs 100 --jsc-arg "$cap" ./jsc test.js || break
done
```

Replay one bad seed under a debugger:

```sh
lldb -- ./jsc --randomYieldPeriod=16 --randomYieldSeed=987654 test.js
```

Tuning: small periods (8–32) hammer the instrumented windows but slow the run;
large periods (256+) keep near-native speed for long-running stress tests.
Vary `--period` across CI shards — different periods expose different races.

## Intended call sites

Call sites are **slow paths only** and land with the workstream that owns each
file (the amplifier workstream ships zero call sites). The authoritative list
lives in the header comment of `runtime/RaceAmplifier.h`; in summary:

- **Object model**: `putDirect`/transition slow paths, butterfly
  (re)allocation and flat→segmented conversion, `Structure` transition-table
  paths, per-cell lock acquire/release.
- **Heap/GC**: `LocalAllocator::allocateSlowCase` / block handout (the
  LocalAllocator.cpp FIXMEs), FreeList refill, `Heap::stopIfNecessarySlow`,
  VMTraps deferred work, VMManager stop-the-world entry/exit.
- **JIT/code lifecycle**: `WatchpointSet::fireAllSlow`, handler-IC case
  append/publish under `CodeBlock::m_lock`, `CodeBlock::jettison` and
  epoch-based reclamation.
- **Shared VM state**: process-global atom table insertion, structure
  allocation lock.

Rules for adding a site:

1. Slow paths only — never JIT-emitted code, LLInt assembly, or allocation
   fast paths.
2. Place the `RaceAmplifier::perturb()` *inside* the racy window you want to
   widen (after the read, before the publish), not merely near it.
3. Never perturb while holding a lock whose hold time is correctness- or
   GC-pause-critical, unless stressing that lock is the point — note it in
   the call-site comment.
4. `#include "RaceAmplifier.h"` is cheap; `perturb()` before
   `RaceAmplifier::initialize()` is safe and does nothing.
