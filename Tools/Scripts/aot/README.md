# Tools for working on ahead-of-time compilation

The compiler and its runtime are in `Source/JavaScriptCore/aot/`, the static heap in `Source/JavaScriptCore/heap/StaticHeap.*` and
`Source/bmalloc/bmalloc/StaticRegion.*`. All of it is behind `ENABLE(AOT)` and `BENABLE(STATIC_REGION)`.

Each script says how to use it at the top, and with `--help`.

| Question | Tool |
| --- | --- |
| Did I break it? (10 seconds) | `run-tests.py <jsc>` runs `JSTests/stress/aot-*.js` and `sound-types-*.js`, each as it says and validated. |
| Does it still do what the interpreter does? (minutes) | `compare-with-interpreter.py <jsc>` runs all of `JSTests/stress` both ways. It needs nothing but a `jsc`, so it is also the quickest way to try a new platform. |
| Will it compile where there is no precompiled header? (a minute a pass) | `check-includes.py <build directory> <base commit>`, with and without `--headers` and `--gate-off`. |
| What else is wrong? (as long as you like) | `fuzz.py` mutates `JSTests/stress` and compares the interpreter with compiled code. `minimize.py` makes a finding smaller. |
| How do I keep a runaway compile from taking the machine down? | `capped.py <GB> <seconds> <command...>` |

## Before pushing

1. `run-tests.py <jsc>`
2. `check-includes.py`, all four combinations.
3. The embedder's tests. In Bun: `test/bundler/bundler_compile_aot*.test.ts`, `test/cli/run/run-aot.test.ts`,
   `test/cli/test/test-aot.test.ts`, and the two that share the module graph, `test/bundler/bundler_compile.test.ts` and
   `test/js/bun/module-graph/module-graph-compile.test.ts`.
4. After changing what `ENABLE(AOT)` guards: a syntax check cannot see a link error. Look at the undefined symbols of the
   embedder's objects (`nm -u`) for anything of `AOT`, `StaticHeap` or `StaticRegion` that is only defined with the gate on.

`run-javascriptcore-tests` has the modes `aot` and `aot-validate`. The second checks every type that the compiler inferred against
the value at run time, and runs the B3 and Air validators after every phase. Built-in objects are immutable in those modes, so the
tests that change one are skipped there: `JSTests/bun-tests-that-change-builtins.txt` says how that list is made.

## What CI keeps

A job that is cancelled or fails still uploads its artifacts. `jsc-test-results-<platform>` has the log of every test
(`jsc-tests.log`, with the command to repeat each failure), and `jsc-shell-<platform>` the very binary, with symbols. For a failure
that is rare, run its command in a loop in a container with core dumps on (`--ulimit core=-1`), and read the core with `gdb`.

## Checks that cannot fail

Four of the checks behind these tools passed for a while because they could not do otherwise: one looked at output and not at exit
codes, one filtered coloured diagnostics with a pattern for plain ones, one filed a crash under a harmless verdict, and one ran a test
without the option that made it a test. Show that a new check fails on something known to be bad before believing that it passes.
