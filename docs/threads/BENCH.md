# Serial-Performance Bench Gate

The shared-memory threads work (see `THREAD.md`) has a hard constraint:
~zero perf cost for single-threaded code. This gate enforces that
constraint mechanically. It runs a small microbenchmark suite against a
`jsc` binary, medians each benchmark across K runs, and fails if any
benchmark's median is more than 1% slower than the recorded baseline.

Run it on every change that touches the object model, butterfly access
paths, IC machinery, watchpoints, or allocation — before merging.

## Components

| Path | Purpose |
|---|---|
| `JSTests/threads/bench/*.js` | The microbenchmark suite (one file per benchmark). |
| `JSTests/threads/bench/harness.js` | Shared harness: warmup, timing, `BENCH <name> <ms>` output. |
| `Tools/threads/bench-gate.sh` | The gate: runs the suite, medians, compares vs baseline. |
| `Tools/threads/baseline.json` | Recorded baseline medians. Created/refreshed with `--record`. |

## Usage

```bash
# Record the baseline from a known-good build (the Verify phase does this
# against the pre-threads tree):
Tools/threads/bench-gate.sh --record /path/to/jsc

# Gate a candidate build against the baseline:
Tools/threads/bench-gate.sh /path/to/jsc
```

Options:

| Flag | Default | Meaning |
|---|---|---|
| `--record` | off | (Re)write `baseline.json` from this run instead of comparing. |
| `--runs K` | 9 | Runs per benchmark; the median of K samples is used. |
| `--baseline FILE` | `Tools/threads/baseline.json` | Alternate baseline file. |
| `--threshold PCT` | 1 | Allowed regression in percent. |

Exit codes:

- `0` — all benchmarks within threshold (or baseline recorded).
- `1` — at least one benchmark regressed more than the threshold.
- `2` — usage or environment error (missing `jsc`, missing baseline, a
  benchmark crashed or produced no `BENCH` line).

Each benchmark runs in its own fresh `jsc` process (no cross-benchmark
JIT/GC pollution); only the measured loop inside the process is timed, so
`jsc` startup cost does not enter the comparison.

## The suite

Each benchmark targets a fast path the threads object model could plausibly
slow down, per the design in `THREAD.md`:

| Benchmark | Guards |
|---|---|
| `inline-property-read` / `inline-property-write` | Inline-cell slots. The cell never resizes, so these are atomic for free — they must be bit-for-bit today's code. Any delta here means the change leaked into the cell access path itself. |
| `flat-butterfly-read` / `flat-butterfly-write` | Out-of-line slots on a flat butterfly. With valid `transitionThreadLocal`/`writeThreadLocal` watchpoints the TID/SW residual checks must be elided entirely; this regresses if butterfly loads/stores pick up masking, branching, or DCAS. |
| `transition-heavy-constructor` | The structure-transition chain plus butterfly (re)allocation. Owner-thread transitions under valid watchpoints must proceed with no locking or CAS. |
| `array-element-read` / `array-element-write` | Contiguous array element access (right side of the butterfly). TTL arrays must keep today's bare load/store; only growth takes the CAS path. |
| `megamorphic-access` | 1000 structures at one site — the generic/megamorphic GetById path. Handler IC dispatch and structure lookup must not pick up locking or extra indirection for TTL objects. |

## Writing a new benchmark

Follow `JSTests/microbenchmarks` conventions, adapted to the harness:

1. Create `JSTests/threads/bench/<name>.js`. The gate auto-discovers every
   `*.js` in that directory except `harness.js`; the benchmark's name is
   the filename without `.js`.
2. Wrap everything in an IIFE. Build the workload, define a `run()`
   returning a deterministic checksum, mark it `noInline(run)`, and end
   with `reportBench("<name>", run, expected)`. The name passed to
   `reportBench` must equal the filename stem — the gate keys the baseline
   on it.
3. The harness validates the checksum on every iteration (warmup and
   measured) and throws on mismatch, so a benchmark can never silently
   measure wrong code.
4. Size the loop so one `run()` takes a few milliseconds on a release
   build; the harness does 20 warmup iterations (enough for
   LLInt → Baseline → DFG → FTL tier-up) and 50 measured iterations by
   default (`reportBench(name, fn, expected, warmup, measured)` to
   override).
5. Re-record the baseline (`--record`) after adding a benchmark — the gate
   errors out on benchmarks with no baseline entry.

## Methodology notes

- **Median, not mean** — robust to the occasional GC pause or scheduler
  hiccup in a sample.
- **1% threshold on a median of 9** is tight; run on a quiet machine
  (no parallel builds, no thermal throttling). For noisy environments,
  raise `--runs` before reaching for `--threshold`.
- **Compare like with like.** The baseline embeds the recording `jsc` path
  and host in its `meta` block. Baseline and candidate must be the same
  build type (release vs release), same machine, same flags. Never compare
  a debug build against a release baseline.
- `baseline.json` is machine-specific and is regenerated by the Verify
  phase; treat checked-in values as documentation of the gate format, not
  portable truth.
