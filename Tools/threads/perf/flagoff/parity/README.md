# Exact counts: where a flag-off process executes more than `main`

The counters of `../` say how much slower flag off is. These tools say where, to the instruction, and they drive the
conversion of functions to the per-mode form (`runtime/ThreadsModePage.h`). They need valgrind (3.18 or later), `perf` is
not used. Build `main` and the branch with the same flags; the tools run a copy of each binary without debug information.

    export OUT=<results directory> [VALGRIND=<valgrind> VALGRIND_LIB=<its lib directory>] [JETSTREAM=<JetStream2>]

| script | what it does | output |
|---|---|---|
| `parity.sh <name> <jsc> <iterations> <parallel> [options]` | each test in a process of its own under callgrind, main thread only | `$OUT/<name>/<test>.fn` |
| `suite-cg.sh <name> <jsc> <iterations> [options]` | the suite in one process (a quarter of an hour per iteration) | `$OUT/<name>/suite.fn` |
| `cgdiff.py [-sum] <main> <branch> [N]` | per function, branch minus main, largest first; per-mode copies are paired with the function they came from | text |
| `famdiff.py <main> <branch>` | the same difference by family (interpreter, slow paths, collector, object model, ...) | text |
| `gatepar.sh <name> <jsc> <iterations> <parallel> [options]` | how often each function executes a load of the threads-mode byte | `$OUT/<name>/summary.txt` |
| `gate-census.py <jsc>` | which functions contain such a load at all (static) | text |
| `option-exec.py <jsc with debug information> <callgrind file with --dump-instr=yes>` | how often the run executes a read of one of the options the branch added (the tests that the per-mode compilation does not fold), per option and per function | text |
| `option-read-census.py <jsc> [<main jsc>]` | which bytes of the Config page the code reads, and in how many functions (static) | text |
| `incdiff.py <main .inc> <branch .inc> [regex]` | inclusive counts (`INCLUSIVE=1` for `cg.sh`), branch minus main: what an entry point costs, whatever is inlined into what | text |
| `auto-permode.py <jsc with debug information> <source root as built> <tree to edit> <list of functions>` | puts `JSC_PER_THREADS_MODE_BEGIN/END` into the listed functions, found through the binary's line table | edits the tree |
| `../../../lint-upstream-removed-lines.py` | lines the branch still has that the base's history removed | text |

## How the thirteenth round used them

1. `parity.sh` with `--useJIT=0 --useConcurrentGC=0`, then with `--useConcurrentJIT=0 --useConcurrentGC=0` and
   `VGOPTS=--smc-check=all-non-file`; `suite-cg.sh` with the latter. `cgdiff.py` ranks the functions.
2. A function that `main` has out of line and that costs more on the branch is a candidate for the per-mode form;
   `auto-permode.py` converts a list of them. A function that `main` inlines and the branch does not (it has no row on
   `main`) needs `ALWAYS_INLINE`, on its first declaration when it is a template.
3. Rebuild, run the pass again, and compare each converted function before and after. **Keep a conversion only if the
   function got cheaper.** The test at the entry of a converted function costs three instructions: a function without a
   gate in it, or whose gates sit in helpers that are not inlined into it, only gets slower. Of 35 functions converted
   by their excess alone in one batch, 21 got cheaper (every JIT operation did) and 13 got dearer and were reverted.

## What the fourteenth round added

- Exclusive counts mislead where the two binaries inline differently: a function that `main` inlines into its caller has
  no row on `main` and its whole count looks like excess. Decide on the inclusive count of the nearest function both
  binaries have out of line (`incdiff.py`), or on the family sums of `famdiff.py`.
- The count of generated code depends on when collections happen: 32.3 G or 33.0 G instructions for the same iteration
  of the suite, for `main` as for the branch, by the number of markers or by chance. When the two binaries of a
  comparison differ in generated code by more than a tenth of a percent, compare families, not totals.
- Marking is only deterministic with `--numberOfGCMarkers=1`: with several markers the main thread's share of the marking
  depends on scheduling (it read +25 % with several and +2 to +4 % with one).
- `gate-exec.py` counts the instructions that address the mode page. Read the form of the test in the disassembly as
  well: it was `lea` + load + test + branch with a register kept for later tests, where two instructions were assumed.

## Rules

- Counts are of the main thread. With the concurrent JIT the main thread's count includes its polling for the compiler
  threads, which valgrind serializes: use `--useConcurrentJIT=0`.
- With the concurrent collector on, how often the write barrier's slow path runs differs five-fold between two runs of
  one binary. Use `--useConcurrentGC=0`. Even so the collector's share is only indicative in `suite-cg.sh`: collections are
  also started by timers, and time under valgrind is not the program's.
- An address below 0x1000000 without a name is a function of a stripped system library (on the machine of the thirteenth
  round 0x1a1040 was `memset` and 0x1a0880 `memmove`); the other unnamed addresses are generated code.
- Sampled instruction profiles (`perf record -e instructions`) skid across calls on some machines and put the excess on
  the wrong function. They were not used.
