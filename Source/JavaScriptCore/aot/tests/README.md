# AoT engine tests

`testaot` runs every test in this folder. There are two kinds:

- **IR text tests** are the `.ir` files. Each one holds a small program in IR text, the commands to run on it and the output to expect.
- **Native tests** are the `AOT*Tests.cpp` files and `unit/`. They cover code that has no text form.

WebKit has no front end, so nothing here compiles `.js` or `.ts`. Whole programs are in Bun's `test/aot/parallel`, and the tests of lowering are in Bun's `test/aot/ir`.

## Layout

Every stage of the compiler reads and prints text, so a bug gets a test at the stage that has it.

| Folder | What it tests | Commands |
|---|---|---|
| `driver/` | the test runner itself: directives and patterns | `print`, `verify` |
| `ir/`, `verifier/` | IR text prints and parses, and each rule of the IR verifier | `print`, `round-trip`, `verify`, `body-index` |
| `types/` | the type algebra | `subset`, `union`, `intersect` |
| `link/` | whole-program facts: types, callers, escapes | `summary`, `link` |
| `plan/` | object shapes, slots, variables, units | `plan` |
| `walk/<mechanism>/` | the optimizer, one folder per optimization | `walk`, `sweep-code`, `verify-code` |
| `machine/` | the back end: operations, register allocation, frames, bytes | `emit`, `allocate`, `frame`, `bytes` |
| `compile/`, `image/` | a whole build, and what the image says | `compile`, `image` |
| `answers/` | programs that run in the `jsc` shell | `run-in-shell` |
| `host/` | the stand-in host's table of built-ins | |

## Build

In Bun, with a WebKit checkout at `$BUN_WEBKIT_PATH` or `vendor/WebKit`:

```
bun run build:local --target=engine-test-programs
```

That builds `testaot` and `jsc`. `bun scripts/build/aot-paths.ts --profile=<name>` prints where they are. WebKit's own CMake build links them too. `testaot` starts the `jsc` in its own folder.

## Run

```
testaot all <folder>                  native tests, then every .ir file under the folder
testaot run <file or folder>..        .ir files
testaot native [<part of a name>]     native tests
testaot list commands                 also: options, mechanisms, statistics, diagnostics
```

`testaot` with no arguments prints every flag. These are the useful ones for `run`:

| Flag | What it does |
|---|---|
| `--filter=<text>` | runs only the files whose path contains the text |
| `--show-output` | prints what each `RUN` line printed |
| `--jobs=1` | runs one file at a time and prints each path first |
| `--single-process` | starts no child process, for a debugger |
| `--option=<option word>` | adds an option word to every file |

A failure is a line `FAIL <file>:<line>: <why>`. The last line is `<n> passed, <m> failed, <k> todo`. The exit code is 0 if nothing failed, 1 if something did, and 2 for a bad command line.

Tests run in child processes, so a crash fails one test. The failure names the signal and the command line that reproduces it.

`Tools/Scripts/run-javascriptcore-tests --testaot` runs `testaot all` on this folder. Bun's `test/aot/engine/*.test.ts` run it in CI.

## Write an IR text test

This is `driver/check-next.ir`:

```
# NEXT matches on the line after the match before it, also after a label
# RUN: print
# CHECK: Add
# CHECK-NEXT: Mul
# CHECK-NEXT: Sub
# CHECK-LABEL: name="sum2"
# CHECK-NEXT: b0

module m0 path="main.js" displayPath="main.js"
function f0 name="sum" parameters=[{type=number}, {type=number}] results=[{type=number}] {
  b0(v0: number, v1: number):
    v2 = Add v0, v1
    v3 = Mul v2, v2
    v4 = Sub v3, v0
    Return v4
}
function f1 name="sum2" parameters=[{type=number}] results=[{type=number}] {
  b0(v0: number):
    v1 = Negate v0
    v2 = Mul v1, v1
    Return v2
}
```

To see what a command prints before you write the check lines, run it by hand: `testaot print driver/check-next.ir f1`.

Every command but `print` and `verify` needs a verified program, and that needs a line `entry module=m0`. The back-end commands only know the functions that the entry module reaches.

You rarely write IR text from nothing. Copy the nearest test, or have Bun's `test/aot/tools/ir-test-from-source.ts` draft one from a small program. `ir/AOTSchema.def` is the reference for every operation, field and flag. The printer leaves out a field that has its zero value, so a pattern cannot name one.

### Directives

A directive is a whole comment line, written `# NAME: value`. Any other comment is prose, but a comment that starts with capitals and a colon (`# NOTE: ..`) is an unknown directive and fails the test.

| Directive | Meaning |
|---|---|
| `RUN: <command> <arguments>` | runs a command and matches its output against the `CHECK` lines |
| `RUN(<PREFIX>): ..` | the same, matched against the `<PREFIX>` lines, so one file can hold several runs |
| `OPTIONS: <option word>..` | compile options for every run in the file |
| `JSC-OPTIONS: --<name>=<value>..` | JSC options while the file runs |
| `REQUIRES: x86_64` or `arm64` | the run is skipped on the other architecture. `REQUIRES(<PREFIX>):` goes with `RUN(<PREFIX>):` |
| `DIAGNOSTIC: <kind> [f<n>] [<count>]` | the run must report exactly the kinds listed. With no such line it must report none |
| `STATISTIC: <name> = <n>` | holds a counter of the build to a number. Also `>=` and `<=` |
| `TODO: <owner>: <what is lost>` | a known failure. It is counted as `todo`, and the file fails once it passes |
| `PROGRAM-OF: <file>` | uses the program of another test |
| `REPEAT: <name> = <first>..<last>` | writes the lines up to `# END REPEAT` once per value. `%(name)` is the value. `testaot write-out <file>` shows the result |
| `LIBRARY: own` | adds no stand-in for the built-ins that are written in JavaScript. A test that runs one (`map`, `for of`, a generator) needs it |
| `TYPES: f<n>` | the function whose parameter types the type commands call `t0`, `t1`, .. Without the line it is `f0` |
| `AS-USERS-BUILD: <why>` | runs without `check=VerifyEachStage`, which every other file runs under |
| `HANGS-ON-ORDER: <owner>: <what differs>` | a known failure: what `link` prints depends on the order of its worklist |

`REQUIRES` also takes these words, with or without an architecture:

- `unsanitized-code`: not AddressSanitizer.
- `not-windows`: on Windows, x86-64 code follows another calling convention. `windows` is the other half.
- `faulting-loop-polls`: a loop poll is a load that faults (on Linux, macOS and Windows; not in a hardened engine for ARM64).
  `calling-loop-polls` is the other half: a comparison with a call on its slow path.
- `no-jscvt`: the CPU has no instruction that converts a number to an int32. ARM64 on macOS has one.
- `code-outside-image`: the engine may run code that is not in the image. A hardened engine never does.
- `as-written`: no `--option` adds an option word.

A `RUN` line also takes `mechanisms=<list>`, which switches optimizations off: `mechanisms=none`, `mechanisms=-Inlining`, `mechanisms=none+Folding`. `testaot list mechanisms` prints the names. Use it for the control of a test: the same input with the optimization withheld.

### Patterns

Check lines work like LLVM's FileCheck, without regular expressions. A pattern matches part of an output line.

| Line | Matches |
|---|---|
| `CHECK: p` | somewhere after the previous match |
| `CHECK-NEXT: p` | on the line after the previous match |
| `CHECK-NOT: p` | nowhere between the matches before and after it |
| `CHECK-LABEL: p` | splits the output into blocks. The lines between two labels see only the output between them |
| `CHECK-COUNT-<n>: p` | n times in order |

`{{*}}` matches any text within the line. `[[name]]` binds a whole token at its first use and must equal it afterwards. Bind the ids of values that the compiler creates, because their numbers are not part of the contract.

A pattern matches part of a line, so `v5: L0` also matches `v5: L0|L1`.

### Built-ins

WebKit's `testaot` links the stand-in host's table of built-ins, and Bun's links Bun's. A test here passes with both, so it names only the engine's entries (`builtins/*.builtins.ts`) and the stand-in host's. A test that needs an entry of Bun's goes in Bun's `test/aot/engine-tests/<the same folder>`.

## Write a native test

```cpp
static void newListIsEmpty()
{
    Vector<int> list;
    AOT_TEST_CHECK(list.isEmpty());
}

std::span<const NativeTest> listTests()
{
    static constexpr NativeTest tests[] = {
        { "lists/new-list-is-empty"_s, newListIsEmpty },
    };
    return tests;
}
```

`AOT_TEST_CHECK` prints the file, the line and the condition, then crashes, in every build type.

For a new file, add its provider (`listTests`) to one of the three lists in `AOTNativeTest.h`. Then run `bun scripts/build/aot-source-lists.ts --webkit=<checkout>` in Bun, which adds the file to `AOT.cmake`.

A third field marks a known failure, like a `TODO` line: `{ "<name>"_s, <function>, "<owner>: <what is lost>"_s }`.

A native test that needs Bun's table of built-ins goes in Bun's `test/aot/native`.

## Add a command

Add it to one provider of `FOR_EACH_AOT_TEST_COMMAND_PROVIDER` in `AOTTestHarness.h`. A command prints text and decides nothing. For an argument it cannot use, it prints `bad argument <word>` and returns.

## Options

`# OPTIONS:` takes option words. `ir/schema/compile-call.def` declares them, and `testaot list options` prints them. Every test file runs with `check=VerifyEachStage`.

The `testing=<name>` options are for tests only, except the three that [`--aot-harden`](#run-time-checks) stands for. `bun build` takes them as `--aot-testing=<name>`. There are three kinds:

- **A cost choice made differently** (`IgnoreBranchHints`, `ShuffleBlockFrequencies:<seed>`, `FailSpeculations:<n>`, `ShuffleChoices:<seed>`, `WidenFlowNodes:<seed>`). The program must print what it prints without the option.
- **A check at run time** (`Check..AtRunTime`). The process ends with a message that names the function and the position where something the compiler assumed does not hold.
- **A fault injection**, which is most of the list (`Assume..`, `Skip..`, `Drop..`, `Mis..`, `Overstate..`, `UnderstateEffects`). It makes the compiler wrong on purpose, so that a test can show that a check catches it.

To see what an option does, search this folder and Bun's `test/aot` for it.

### Run-time checks

A default build checks a proven type where a value enters the location that has the type: a parameter, a call's result, a store to a field, a variable or a captured variable, and a read of an element. These options add checks:

| Option | Public name | What it checks |
|---|---|---|
| `testing=CheckProofsAtRunTime` with `testing=DisableFrameCells` | `--aot-harden=proof-checks` | a proven type again at each read, what a branch has narrowed, and what the walk remembers across other code. Every cell stays in the heap |
| `testing=CheckBoundsAtRunTime` | `--aot-harden=ignore-told-ranges` | every index against the length, at the access |
| `testing=CheckWalkAtRunTime` | none | what the walk assumes about its own work: a guard that an access relies on, the range of an unboxed counter, the parameters of a loop header |

The walk builds a `CheckProof` operation on the value. `testaot walk main.bin testing=CheckProofsAtRunTime` shows each one with its types and its source. A check that fails prints `internal AOT error`, the name of the check and the position in the source, and aborts the process.

Some proofs have no check, because no test of a value reveals them: the effects of a callee, that a location is written only at construction, the key of a proven access and the order of keys.

The statistics that end in `ProofsChecked`, such as `FieldProofsChecked`, count the checks by source. `UntestableProofsSkipped` counts the types that no test can tell, and `ProofChecksFolded` the checks whose test folded to a constant.

A new check needs a test that fails without it. Inject a fault, such as `testing=AssumeUnknownCallsReturnUndefined` or `testing=AssumeIndexesInBounds`, and show that the process stops, as in `answers/false-proofs-are-stopped-where-the-value-comes-in.ir`.

## Look at a real program

`bun build --aot-out=<file>` writes the IR of a program, and every command takes that file in place of IR text.

```
bun build --compile --aot --aot-out=main.bin main.ts
testaot print main.bin | grep 'name="add"'     # find the function's id, say f1
testaot walk main.bin f1                       # optimized code
testaot emit main.bin f1                       # back-end operations
testaot compile main.bin --output=main.image   # prints "entry 0 1"
jsc --aotImagePath=main.image -e "runCompiledEntryPoint(1)"
```

The shell has no host. A program that calls one of Bun's functions compiles. When the call runs, the shell prints `AOT: the program needs the host's Bun.nanoseconds, and this realm has no host` and exits with 4.

## The allocation fuzzer

`testaot fuzz-allocation [--seed=<n>] [--count=<n>]` builds random procedures, sends them through the back end with every check on, runs them, and compares the results with an interpreter's. A seed gives the same procedure on every machine. `--show=<seed>` prints one.

## What a good test looks like

- It tests one rule or one rewrite, at the stage that has it. A compiler bug gets a text test at its stage, not a whole program.
- Every positive case has a negative case next to it: the same shape with the one property changed that must stop the rewrite, checked with `-NOT`.
- A checker has a test that makes it fail. A checker that never fails proves nothing.
- Its first line says the rule in plain words, and its name says the case.
- It is short. A subject with many cases is a folder.
