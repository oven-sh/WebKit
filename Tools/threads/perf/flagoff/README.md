# Flag-off measurement harness

Measures a `jsc` built from this branch and run with `useJSThreads` unset ("flag off") against `jsc` built from the commit the
branch is rebased on ("main"), on one machine in one session. Nothing here compares against a recorded number: build both trees
with the same flags, run them interleaved, on an idle machine.

    export FLAGOFF_MAIN=<main's jsc> FLAGOFF_BRANCH=<the branch's jsc> OUT=<results directory>
    baseline.sh <tag>      # everything below, in order; refuses to start under load (FORCE=1 overrides)

| script | measures | output |
|---|---|---|
| `startup.sh [runs]` | user-mode instructions and peak resident set of an empty script and of a small loop, without any `JSC_` variable in the environment, and of the empty script with one (`envopt`) | `startup.txt` |
| `startup-smaps.py <main jsc> <branch jsc>` | the resident set of a shell that has started, by mapping (text, read-only data, anonymous): where the start-up's resident set differs | text |
| `sweep.sh [rounds] [P]` | main-thread instructions, cycles, locked loads and task-clock of each of the 36 tests (one test per process) | `counters.txt`; `counters-compare.py [-v]` |
| `phases.sh [rounds]` | the 36 tests in one process: whole run, first iteration, first six, each tier capped; peak resident set | `phases.txt`, `rss.txt`; `phases-compare.py` |
| `quiet-pass.sh [rounds]` | full JetStream score, main and flag off interleaved | `quiet/js-<round>-<cfg>.txt`; `score-compare.py [-v]` (score with Startup, Worst Case and Average) |
| `profile.sh [rounds]` | cycle samples of the suite, JIT code attributed by address and time | `profile/*.json`; `profile-diff.py full -s 40` |
| `per-test.sh` | one test: first iteration, interpreter only, peak resident set | one line |
| `multi.sh <rounds> <phases> <name=jsc>...` | the phases of `phases.sh` for any number of binaries in one session (before and after a change beside `main`); `sjit` is the first iteration with the JIT compiling on the main thread | `multi.txt`; `multi-compare.py` |
| `pertest.sh <rounds> <parallel> <interp\|first> <name=jsc>...` | the per-test geometric mean of the L5 table, for any number of binaries | `pertest-<mode>.txt` |
| `cycles-breakdown.sh <rounds> <phase> <name=jsc>...` | the main thread's cycles of a phase split by hardware events (pipeline slots, front end, caches, TLBs, branches), four events to a group beside cycles and instructions so that nothing is multiplexed; `GROUPS_LIST` picks the groups | `breakdown.txt`; `cycles-breakdown-compare.py <dir>` prints each event's ratio to `main` and its share of the cycles |
| `event-by-symbol.sh <event> <period> <phase> <name=jsc>...` | samples of one hardware event per symbol (for the events that count stalls: where the front end waits) | `<event>-<name>.txt`; `event-by-symbol-compare.py` |
| `type-sizes.py <main jsc> <branch jsc> [regex]` | the classes whose size differs | text |
| `type-fields.py <main jsc> <branch jsc> <types>` | offset and size of every member of the named types, side by side: which member made a class larger | text |
| `text-by-symbol.py <main jsc> <branch jsc>` | where the text is larger: symbols only the branch has, growth of the symbols both have, the copies per threads mode | text |
| `parity/` | exact instruction counts per function under callgrind, their difference against `main`, and the tools that convert functions to the per-mode form (its own README) | |

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
- The shell reads every environment variable that begins with `JSC_` as an option. Nothing of the harness may be passed
  to it under such a name (the binaries were, until the fourteenth round: `JSC_MAIN`, `JSC_BRANCH`), or the start-up
  measured is that of a process with options in its environment.
- The whole run's cycle ratio of one pair of binaries moves by about 0.3 points between two passes of 15 to 40 runs in
  one session. A difference below that between two builds is not a result; the exact counts of `parity/` are.
- The hardware events are read in groups of at most four beside cycles and instructions. In a virtual machine some are
  not available (`topdown.slots`, `frontend_retired.*` on the machine of the fourteenth round): `perf stat` reports them
  as not supported and the group is lost with them. Check a group once by hand before a long pass.
- The first iteration's instruction count moves by about 0.4 % between sessions for one binary (how far the compiler threads
  got decides what the main thread runs), its cycles by about 1 %: take five rounds, and use `multi.sh ... sjit` and the exact
  counts of `parity/` to decide whether a change helped.
