# Flag-off measurement harness

Measures a `jsc` built from this branch and run with `useJSThreads` unset ("flag off") against `jsc` built from the commit the
branch is rebased on ("main"), on one machine in one session. Nothing here compares against a recorded number: build both trees
with the same flags, run them interleaved, on an idle machine.

    export JSC_MAIN=<main's jsc> JSC_BRANCH=<the branch's jsc> OUT=<results directory>
    baseline.sh <tag>      # everything below, in order; refuses to start under load (FORCE=1 overrides)

| script | measures | output |
|---|---|---|
| `startup.sh [runs]` | user-mode instructions and peak resident set of an empty script and of a small loop | `startup.txt` |
| `sweep.sh [rounds] [P]` | main-thread instructions, cycles, locked loads and task-clock of each of the 36 tests (one test per process) | `counters.txt`; `counters-compare.py [-v]` |
| `phases.sh [rounds]` | the 36 tests in one process: whole run, first iteration, first six, each tier capped; peak resident set | `phases.txt`, `rss.txt`; `phases-compare.py` |
| `quiet-pass.sh [rounds]` | full JetStream score, main and flag off interleaved | `quiet/js-<round>-<cfg>.txt`; `score-compare.py [-v]` (score with Startup, Worst Case and Average) |
| `profile.sh [rounds]` | cycle samples of the suite, JIT code attributed by address and time | `profile/*.json`; `profile-diff.py full -s 40` |
| `per-test.sh` | one test: first iteration, interpreter only, peak resident set | one line |

Rules (learned the hard way, FLAG-OFF-LANDING Part 2):

- Time is main-thread cycles (`perf stat --no-inherit`), not whole-process instruction counts (those include compiler and collector
  threads and weigh a locked instruction at one).
- The score is the geometric mean of the first iteration, the four worst and the mean of all iterations: a cost in five
  iterations has two thirds of the weight. Always read Startup / Worst Case / Average beside the score, and the first-iteration
  cycles beside the whole-run cycles.
- The quiet-pass score moves by about a point and a half between sessions with no code change: compare only against a `main`
  measured in the same session, medians of five. The cycle metrics are stable to a few tenths of a percent.
- Several tests are bimodal in every configuration including main (FlightPlanner, earley-boyer, ai-astar, Babylon, async-fs): take
  minima for counters, medians for scores, and never conclude from one run.
- `perf record -c` periods below about 4 M instructions can be throttled; `profile.sh` samples cycles at 2 M and 300 k.
- zsh does not word-split an unquoted variable: run these with bash.
