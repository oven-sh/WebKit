# AOT compilation tools

The compiler and its runtime live in `Source/JavaScriptCore/aot/`, behind `ENABLE(AOT)`.

Every script documents its usage at the top of the file and via `--help`.

## The shell

`jsc` uses the same entry points as an embedder (`buildAOTFile()`, `useAOTFile()`) for a single script or module:

| Command | Effect |
| --- | --- |
| `jsc --writeAOTImageTo=<file> main.js` | Compiles the script, writes the code and program data to the file, and exits |
| `jsc --aotImagePath=<file> main.js` | Maps the file read-only and runs from it |
| `jsc --compileMainScriptAheadOfTime=1 main.js` | Both: writes a temporary file and reruns itself with it. The tests use this. |

Pass `-m` to treat the file as a module. Anything else that gets loaded is interpreted, except the same file loaded again in another
realm (`createGlobalObject().load()`), on another thread (`$.agent.start()`), or by another module loader in the same realm
(`importInNewLoader()`). `isAOTCompiled(f)` reports whether a function runs compiled code.

| Goal | Tool |
| --- | --- |
| Quick regression check (10 seconds) | `run-tests.py <jsc>` runs `JSTests/stress/aot-*.js` and `sound-types-*.js`, each with its own options and again with validation. |
| Compare against the interpreter (minutes) | `compare-with-interpreter.py <jsc>` runs all of `JSTests/stress` both ways. It only needs a `jsc` binary, so it is also the fastest way to try a new platform. |
| Catch missing includes hidden by the precompiled header (about a minute per pass) | `check-includes.py <build directory> <base commit>`, with and without `--headers` and `--gate-off`. |
| Fuzzing (open-ended) | `fuzz.py` mutates `JSTests/stress` and compares the interpreter with compiled code. `minimize.py` reduces a finding. |
| Fuzzing with generated programs (open-ended) | `fuzz-programs.py` generates programs about scopes and closures, and about objects, their shapes and aliases, and compares likewise. `--minimize` reduces a finding. It found wrong code within a minute where `fuzz.py` found none in twenty. |
| Check that a faster build still does the same work (minutes) | `compare-execution-counts.py`, see below |
| See what the compiler makes of one function of a large program | `--dumpAOTGraph=1 --aotFunctionToDump=<name>`, or `<start offset>` or `<module>:<start offset>` as in the map file for a function without a name. Also for `dumpAOTB3Graph` and `dumpAOTDisassembly`. |
| Check that a program compiles to the same image every time (a minute) | `check-reproducible.py <jsc> [file.js ...]` compiles the tests, or the given programs, with one compiler thread, with sixteen, and with one in a few shuffled orders of type inference, and compares the images. Give it a program of several megabytes as well: most of what it found did not show in the tests. |
| Check that the stubs return the way the processor predicts (a second, ARM64) | `check-paired-returns.py <jsc>` reads the machine code of the stubs and fails on one that computes its return address instead of being called, or that returns with `br`. With `--image` and `--map` it reads an executable without running it. |
| Check that no stub changes a register it is declared to preserve (seconds, ARM64) | `check-stub-clobbers.py <jsc>` follows every path through the machine code of the stubs that compiled code keeps values in registers across (`registersChangedBy` in `AOTStubs.cpp`; the `K` lines of the map), their variants and thunks, and fails on one that changes another register, or that leaves by a tail jump. Run it after any change to such a stub. With `--image` and `--map` it reads an executable as it is. `--print [--stub <name>]` shows what any stub changes, and how it leaves. |
| Find the checks of compiled code that no test covers (an hour) | `drop-guards.py <jsc>` leaves out one guard of the compiler at a time, with `--aotGuardToDropForTesting`, and runs the tests and the fuzzers. What survives lacks a test. |
| Limit a runaway compile | `capped.py <GB> <seconds> <command...>` |

## Seeing what the compiler did

| Question | Tool |
| --- | --- |
| Where in my program did the compiler fail to specialize, how much, and why? | Type coverage, below |
| What did this line compile to? | `type-coverage.py <file> --explain <source file>:<line>` |
| Which optimizations applied to this function? | `--aotRemarksPath=<file>`: function name, tab, remark, tab, `module:start:kind`. In the shell, `aotRemarks(name)`. The tests assert on these, both that an optimization applies and that it does not. |
| What did whole-program inference conclude, and from what? | `--logAOTTypeInference=1` (lines start with `AOT inference:`) |
| What exactly happened in this function? | `--dumpAOTGraph=1`, `--dumpAOTB3Graph=1`, `--dumpAOTDisassembly=1`; add `--numberOfAOTCompilerThreads=1` to keep the output in order |
| Where does the build time go? | `--verboseAOTCompilation=1` |
| Which function is this address in, and which property is this cache slot for? | `--aotMapFilePath=<file>` |
| Is an inferred type wrong? | `--validateAOTInferredTypes=1` checks every one at run time and crashes on the first that is wrong; `--validateGraphAtEachPhase=1` runs the B3 and Air validators |
| Which values break the declared types, without failing? | `--auditAOTTypedFields=1` logs each violation (`AUDIT` lines) instead of throwing, and compiled code does not rely on the types |

An embedder passes these to the process that compiles. Bun takes them from `BUN_AOT_OPTIONS`, comma separated and without the dashes.

### Type coverage

Like code coverage, except that an operation is covered if the compiler specialized it. `--aotTypeCoveragePath=<file>` writes one
line for every instruction of every compiled function: where it is, what it is, what it compiled to and, for a property access
that the type table gives no type, why. It records decisions that are made anyway, so the code is the same with and without it.

```
jsc --writeAOTImageTo=/tmp/image --aotTypeCoveragePath=/tmp/coverage main.js
type-coverage.py /tmp/coverage --by property
type-coverage.py /tmp/coverage --explain main.js:12
```

For a bundled program, pass the bundler's source maps (`--source-map`) to get positions in the original files, and the type table's
`reasons.txt` (`--reasons`) to get the reasons in words. `--lcov` writes a tracefile for tools that show code coverage, which can
hold only hit or miss, and `--fail-under <percent>` makes the exit status usable in CI. The last table of the report compares the
number of instructions reported with the number in the bytecode: anything but 100% is a bug in the compiler's reporting.

## Before believing a speedup

A miscompiled program need not crash. A build that walked one scope too far in functions with exception handlers ran a large
program to the end with the same output as before and 12% fewer instructions, 8 of which were work it no longer did: what went
wrong was caught by the program's own handlers. Every test passed.

So before comparing the speed of two builds of a real program, compile both with `--useAOTTypeCoverageCounters=1`, run them on the
same input and give the counters to `compare-execution-counts.py`. It compares how often the operations ran that optimizations
leave in place (catches, throws, closures, object literals, property accesses) and at how many places in the program.

## Before pushing

1. `run-tests.py <jsc>`, `check-reproducible.py <jsc>`, and a few minutes of `fuzz-programs.py`.
2. `check-includes.py`, all four combinations.
3. The embedder's tests. In Bun: `test/bundler/bundler_compile_aot*.test.ts`, `test/cli/run/run-aot.test.ts`,
   `test/cli/test/test-aot.test.ts`, and the two that share the module graph, `test/bundler/bundler_compile.test.ts` and
   `test/js/bun/module-graph/module-graph-compile.test.ts`.
4. After changing what `ENABLE(AOT)` guards, remember that a syntax-only check cannot catch link errors. Inspect the embedder's
   undefined symbols (`nm -u`) for `AOT` symbols that are only defined when the gate is on.

`run-javascriptcore-tests` has two AOT modes, `aot` and `aot-validate`. The second checks every inferred type against the runtime
value and runs the B3 and Air validators after every phase. In both modes built-in objects are immutable and the program has no
source text, so tests that modify a built-in or read function source are skipped. `JSTests/bun-tests-that-change-builtins.txt` and
`JSTests/bun-tests-that-read-function-text.txt` describe how those lists are generated.

## CI artifacts

A cancelled or failed job still uploads its artifacts. `jsc-test-results-<platform>` contains the full test log (`jsc-tests.log`,
including a repro command for each failure), and `jsc-shell-<platform>` contains the exact binary with symbols. For a rare failure, run
its repro command in a loop inside a container with core dumps enabled (`--ulimit core=-1`) and open the core in `gdb`.

## Verify that new checks can fail

Four checks behind these tools passed for a while only because they could not fail: one looked at output instead of exit codes, one
matched coloured diagnostics with a pattern written for plain text, one classified a crash as harmless, and one ran a test without
the option that made it meaningful. Before trusting a new check, confirm that it fails on a known-bad input.
