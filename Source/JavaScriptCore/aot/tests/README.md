# AoT engine tests

## What is here

`testaot` is the only test program for the engine half of the compiler. It runs IR text tests and native tests.

An IR text test is `<folder of the code under test>/[<subject>/]<case>.ir`, for example
`passes/sink-allocations/escapes-through-call.ir`. Native tests are the `AOT*Tests.cpp` files, `unit/` and the `.cpp` files of
`cliffs/`. `AOTShell.cpp` is not part of `testaot`: it is everything the `jsc` shell knows about AoT.

The WebKit repository has no front end, so nothing here compiles `.js` or `.ts`. Whole programs and their expected output are in
Bun's `test/aot/programs`, and the tests of lowering are in Bun's `test/aot/ir`.

| Folder | Tests |
|---|---|
| `driver/` | the directives and the matcher themselves |
| `ir/text/`, `verifier/`, `types/` | IR text and the reader, the verifier, the type lattice |
| `ir/reader/` | what the reader makes of positions, families and layouts that nobody allocates |
| `ir/families/` | which layouts the reader puts into the family of a type: those that have the names it requires, but for the ones a literal field or a tag tells apart |
| `ir/tagged-layouts/` | layouts by tag: the reader's plan, the pass `AllocateTaggedLayouts` |
| `ir/result-types/`, `ir/edge-results/`, `ir/type-values/`, `ir/int32/` | result rules, what an edge proves, typing of values and requests, `int32` promises |
| `ir/effects/` | effects, locations, summaries |
| `support/`, `pipeline/` | the build context, limits, diagnostics, threads; the pass table, schedules, derived functions |
| `analysis/` | the analyses of one function, the builder, the region cloner, the call graph |
| `valueflow/{bodies,heap,calls,protocols,builtins,host,modules,families}/` | the flow analysis, one folder per part: the reduced bodies, then the rules of the heap, of calls, of the protocols of the language, of built-ins, of what the run time does by itself, of modules, of families |
| `valueflow/placeholders/` | what the untracked and the unknown object stand for: calls, lookups, stores, escape |
| `valueflow/{results,validation}/` | the results of the flow analysis, and the check of results that a test has damaged |
| `passes/` | every pass of the pass table: `<pass>.ir`, `<pass>-<case>.ir`, `<pass>/<case>.ir` |
| `derived-functions/` | the builders of derived functions: `<role>-<case>.ir` |
| `builtins/` | the resolution of built-ins |
| `select/` | one family of files or one folder per selection rule; raw words |
| `place/` | slots, numbering, shape tests, site caches, program tables |
| `represent/` | the representations of values |
| `codegen/` | code generation, type test plans, barriers, frame maps, validation |
| `validation/` | a lie per producer: a program in which one thing that typed code relies on is false. The validating build ends the process with the kind that names the lie, or fails at build time |
| `cliffs/` | the cliffs that no program can cause |

## Running

```
testaot all <folder>                                              native tests, then every *.ir under the folder, in parallel
testaot run <file or folder>.. [--filter=<text>] [--jobs=<n>] [--show-output] [--option=<option word>].. [--only-diagnostic=<kind>]
testaot native [<filter>] [--after=<name>] [--single-process]
testaot list commands|passes|options|limits|statistics|diagnostics
testaot print <program> [--until=<PassID>] [--function=f<n>].. [--wire] { option word }
testaot compile <program> --output=<image> { option word }        prints "entry <i> <id>" for each entry point
testaot fuzz-reader [--seed=<n>] [--count=<n>] [<program>..]
```

`<program>` is IR text, or program bytes as the front end writes them.

The exit code is 0 if nothing failed, 1 if a test failed, no test matched or a build reported an error, and 2 if the command
line is wrong. Each failure is a line `FAIL <file>:<line>: <why>`, and the last line is `<n> passed, <m> failed, <k> todo`. A file
that fails under a `TODO` line is no failure: it prints `TODO <file>:<line>: <owner>: <what is lost>` and, indented below it, where
it still fails.

`--filter=<text>` runs only the files whose path contains the text. `--jobs=1` runs the files one at a time and prints each path
before the file runs. `--show-output` prints, after a file's failures, what each of its `RUN` lines printed, under a line
`OUTPUT <file>:<line>:`.

`--option=<option word>` adds an option word to the `OPTIONS` of every file. `--only-diagnostic=<kind>` runs the files for that
diagnostic alone: a file fails if one of its runs reports it, nothing else of the file is compared, and a file that expects the
diagnostic itself is left out. Together they run a folder in another way than its files were written for.

A test that crashes fails alone, because tests run in child processes. `run` starts one child per file. `native` starts one
child for all its tests, and after a crash another one for the tests that are left. The failure is
`FAIL <file>:1: crashed with <signal>` or `FAIL <name>: crashed with <signal>`, below the last lines the child printed. A child that
prints nothing for 300 seconds is killed, and its test fails.

Under a debugger, run the test in `testaot` itself: `run` with a single file starts no child, and neither does
`native --single-process`. `--after=<name>` runs only the native tests listed after that one.

`print --wire <program>` is what the front end's tests call. It reads the program, keeps its text as read with wire fields only,
then types and verifies it. Without a diagnostic it prints that text and exits with 0. Otherwise it prints nothing on stdout, one
line `<Kind>: <message>` per diagnostic on stderr, and exits with 1. It takes no `--until` and no `--function`.

Any other `--<name>=<value>` is set as a JSC option, as in `jsc`. Without `--function`, `print` prints the functions that
`aotFunctionToDump` names, and what was derived from them:

```
testaot print main.bin --until=<PassID> --aotFunctionToDump=<name>
```

Only `testaot` reads IR text, and without assertions only `testaot` prints it: the lexer and the parser are sources of `testaot`,
and the sources that print are empty in the library (`AOTIRPrinter.h` says how). So `dumpAOTIR` prints IR in a build with
assertions alone, in Bun, `jsc` and `testaot` alike.

WebKit's own test script runs `testaot all` on this folder:

```
Tools/Scripts/run-javascriptcore-tests --testaot
```

## A test file

A test is one file: a program in IR text, plus directives that say what to run and what to expect. This is
`driver/check-next.ir`:

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

These are all the directives. `<P>` is `CHECK` for a `RUN:`, and the prefix for a `RUN(<prefix>):`.

```
RUN: <command> { argument }
RUN(<prefix>): <command> { argument }
REQUIRES: x86_64 | arm64
REQUIRES(<prefix>): x86_64 | arm64
FORM: ( AsRead | Prepared | Selected | Placed ) [ optimizing ]
OPTIONS: <option word> { option word }
<P>: p
<P>-NEXT: p
<P>-NOT: p
<P>-LABEL: p
<P>-COUNT-<n>: p
DIAGNOSTIC: <UserDiagnosticKind or InternalErrorKind> [ <count> ]
STATISTIC: <StatisticID> ( = | >= | <= ) <n>
LOSS: <LimitID> { x<count> | f<n> | v<n> | L<n> }
LIBRARY: own
TODO: <owner>: <what is lost>
```

A directive is a whole comment line: `#`, a name in capitals, then `:`. A comment after IR tokens on the same line is never a
directive.

Each `RUN` parses the file again (directives are comments to the parser), sends the wire records through the reader in the form and
applies the engine-only fields. In form `AsRead` that is all for `print`, `round-trip`, `verify`, `result-types`, `pass`, `pipeline`, `compile` and the commands that run
the pipeline themselves: a test runs `pass TypeValues` or `verify` itself, and `verify` refuses a program that still has a `Convert` (`RequestAfterTyping`). A command registered with `ProgramAsRead::TypedAndVerified` (the commands of the flow analysis, of the
analyses and of effects) gets the program after the read stage of the pipeline: every value typed, every request resolved, the program verified. In a later form every
value is typed by its rule, the types the text states are applied again, and the program is verified. Then the command runs on a new
`BuildContext`, and what it printed is matched against the check lines with its prefix. A command registered with `RawWordCheck::Skip` gets a program that was verified
without the check of raw words, and verifies it itself after its rewrite.

A front end always sends the library functions that the built-ins table names. So that a test need not, every command that runs the flow analysis on a program in form `AsRead` (all but `print`, `round-trip`, `verify`, `result-types` and `pass`) gets a
stand-in library: one more module without a body (`flags=[IsLibrary]`, as a front end sends the library's files) and in it one more function, `role=Library`, that returns `undefined`, registered for every runtime function of the table that the text has no `runtimefunction` line for. A test
about a library function or a model writes the lines of the functions it is about, and the stand-in serves the rest. The line `# LIBRARY: own`, written exactly so, says that the text brings all it
needs: nothing is added. `testaot compile`, `testaot print --until` on IR text and
`compileProgramText` (`AOTLoadedProgramTestSupport.h`) do the same. An image built with the stand-in carries it: in the realm of such a program every built-in that is written in
JavaScript (`Array.prototype.map`, `new Promise`, `Object.entries`) returns `undefined`, and the flow analysis enters it as a function that ignores its operands. A test that runs such a
built-in, or asks what flow knows about one, writes `# LIBRARY: own` or the `runtimefunction` lines of the functions it uses.

`FORM` overrides the command's default form. `FORM: Prepared optimizing` reads and verifies the program as it is in the middle of the
optimization stage: operations of `AsRead` and of `Prepared` are legal together, and a stored type may be wider than its rule gives.
`OPTIONS`, `DIAGNOSTIC`, `STATISTIC` and `LOSS` apply to every `RUN` in the file.

`REQUIRES` is for output that depends on the target architecture, which is the CPU `testaot` was built for. The `RUN` lines with
its prefix (`REQUIRES:` goes with `RUN:`) run only on that architecture. Their check lines are parsed on every architecture, so a
malformed one fails everywhere. Check what all targets share under a prefix without `REQUIRES`:

```
# RUN: place
# CHECK: L1 shape={{*}} size=144
# RUN(X64): place
# REQUIRES(X64): x86_64
# X64: s22 boolean Boolean 8
```

A run must report exactly the diagnostic kinds listed. Kinds are compared by name, never by message, and warnings and notes
count too. A kind listed without a count must be reported at least once. With no `DIAGNOSTIC` line, the run must report no
diagnostic. A program that does not parse or verify is not a failure by itself: the command does not run, and the `DIAGNOSTIC`
lines alone decide. This is how to test the verifier:

```
# RUN: verify
# DIAGNOSTIC: <InternalErrorKind>
```

A run must give up on exactly the limits listed. A build keeps one loss record per limit: the number of hits and the lowest
subject. `x<count>` is the number of hits, `f<n>`, `v<n>` and `L<n>` are the function, value and layout of the subject. A word that
is left out is not compared. With no `LOSS` line, the run must hit no limit.

The commands that run the whole pipeline themselves (`pipeline`, `compile`, `run-in-shell`, `generate-code`, `plan-slots`, `place`, `tables`, `check-placement`, `allocation-remarks`,
`unsupported-conversions`) are the exception: the thresholds of heuristics are limits too, a build of almost any program hits some, and which ones depends on the target. There a listed
limit must be hit, a limit that is not listed may be, and `LOSS: <LimitID> x0` says that a limit is not hit.

`TODO` marks a test that shows a debt of the compiler: an optimization that is lost, a result that flow does not give. The test keeps
its expectation and still runs. While it fails it is counted as `todo` and not as `failed`, so that `failed` means something new. As
soon as it passes it fails, with `passes: remove the TODO line`, so no mark outlives its reason. The text names who owes it and, in
plain words, the thing that is lost: the `TODO` lines of the folder are the list of debts. A file has at most one. A file that
crashes, or whose directives are wrong, fails as ever. A test whose expectation is out of date is restated and gets no `TODO`, and
neither does one that shows wrong code. Under `--option` and `--only-diagnostic` a file with a `TODO` line may pass.

Directives can appear in any order. Only the order of the check lines with the same prefix matters.

A directive the driver does not understand fails the test, so a misspelled `CHECK-NXT` cannot pass silently. For the same reason
an ordinary comment never starts with capitals and a colon: `# NOTE: ..` is an unknown directive. A prefix is `[A-Z][A-Z0-9]*`.
Words are split at spaces and tabs, and there is no quoting.

## IR text

IR text is a second spelling of the records of `aot/ir/AOTSchema.def`. Printer and parser are generated from the schema, so the
schema is the reference for every keyword, field, operand, payload name and flag.

- A list with a keyword prints one line per record: `layout L0 name="Point" fields=[..]`. Fields come in declaration order. A field
  that has its zero value (0, false, absent, empty, the first enumerator) is not printed, and a field that is left out
  parses as zero. The zero value of an id is absent, not id 0: `module=m0` and `body=f0` are printed.
- An id is a letter and a number: `v` value, `b` block, `f` function, `L` layout, `g` variable, `m` module, `q` module failure,
  `w` type site, `e` eval scope, `x` expected result, `d` data.
- A flag word is `[A, B]`, a nested record `{field=value ..}`, a variant `Case` or `Case{field=value ..}`, a list `[a, b]`.
- A property key is `"x"`, `@"x"` (engine private name), `#"x"@L2` (private name of layout 2), `@@iterator`, `symbol@m0:120`.
- A type is parts joined by `|`: `undefined|number`, `i32`, `int[0, 9]`, `"a"|"b"`, `L3|L4`, `Array<number>`, `function`, `f3`, `env L1`,
  `builtin(MathMax)`, `anything`, `never`, `object`, `absent`, an opaque kind in lower case (`guard`, `iteratorstate`). `L3` reads as its
  family: the layouts below it. `nolayout` beside layouts says that the family also has members without a layout (`L3|nolayout`, in a layout
  set `[L3, nolayout]`). `f3` names a function, or a function type (a function with `role=Signature`), which reads as the functions
  converted into it (`functionTypes=[f3]` on a function or on a function type below it) and the built-ins converted into it (`builtinMembers=[MathAbs]` on the function type). The contents of `Array<L3>` are expected. The engine has more parts than the wire: `whole`, `fractional`, `double`, a
  symbol key, `Array<L3, proven>` (contents proven), `Array<number, proven, Int32>` (storage kind), `RegExp<"ab+c"[Global]>`.
- A position is `{type=T site=w0 flags=[..]}`. Every value that enters it is inside `T`, by a conversion that the families cover or by a
  `Check`. With the flag `IsExpected` nothing checks what enters: a read of it proves nothing and expects `T`.
- A value is `vN = Operation operands.. name=value.. [flag, flag] @position`. Operands come first, in declaration order, separated by
  commas, with `-` for an absent optional operand that another operand follows. An operation without a result has no `vN =` and
  still takes the next id.
- A terminator is `Name operands.. name=value.. -> target bK(arguments | proofs), ..`.
- A block is `bN(parameters) role=.. subject=vK frequency=.. handler=bK(arguments):`.

Three fields of a value are the engine's: `vN: TYPE`, `~ EXPECTED` and `@REPRESENTATION`, in this order and all left of
the `=`. The type is proven. The expected type lies inside it and is there only where the best that is known is not proven. They are legal in every form. Text in form `AsRead` needs none: `TypeValues` types every value. Text in a later form writes types on entry positions,
and on a value only where the test wants another type than the rule gives:

```
v4: object ~ L0|L1 @Cell = GetSlot v1, v3 key="x" receivers=[L0] level=Expected
```

In form `AsRead` the layouts that the reader derives (flag `IsDerived`) print as comment lines, `# layout L4 ..`, and with wire fields only
not at all: the reader makes them again. `L<n>` with n past the layout lines of the text names the (n - lines)th of them.

What the flow analysis knows is written in the records it belongs to, so a test of a pass states its own flow results:

```
layout L0 name="Point" fields=[{key="x" declared={type=number} flow={occupancy=AlwaysFilled}}] flow={members=[L0]}
```

Passes append the functions they derive. So in text of a form after `AsRead` the functions as read come first, and they end at the
first function that has a `derivation=` or a role that only an appended function has: every role of the engine but `CoroutineRamp`,
`ResultWrapper`, `ParameterWrapper` and `StubBody`, which a pass leaves under the id of the function whose body it moved. Diagnostics,
losses and `disable-in=` name a derived function by the function as read at the end of its chain of `derivation={source=..}`:

```
function f0 name="load" role=ResultWrapper results=[{type=Array<number>}] creator=ResultWrapper{worker=f1}
function f1 name="load" role=ResultWorker results=[{type=number}, {type=number}] derivation={source=f0} creator=ResultWorker{origin=f0} {
```

A test passes what it proves along the edge. The parameters of a block are filled by the explicit arguments, then by one edge
result per proof:

```
  b0(v0: anything):
    v1 = TypeTest v0 testedType=string
    Branch v1 -> taken b1(| 0), notTaken b2(| 0)
  b1(v2):
```

`v2` is `v0` with the type `string`. A test that holds for a stretch of code passes a guard the same way, and the access names it as
an operand.

## Patterns

Check lines work like LLVM's FileCheck, without regular expressions. A pattern matches part of an output line.

- `<P>: p` matches somewhere after the previous match, possibly on the same line.
- `<P>-NEXT: p` matches on the line after the previous match.
- `<P>-NOT: p` matches nowhere between the matches before and after it. With no match after it, it covers the rest of the block.
- `<P>-LABEL: p` splits the output into blocks. Labels are found first, and the check lines between two labels see only the
  output between them. Without labels, the whole output is one block.
- `<P>-COUNT-<n>: p` matches n times in order, with any output between the matches. For exactly n, put a `-NOT` after it.

Three things in a pattern are not literal text:

- A run of spaces or tabs matches a run of spaces or tabs.
- `{{*}}` matches any text within the line, including none.
- `[[name]]` is a variable. Its first use binds it to a whole token (the longest run of `A-Z a-z 0-9 _ . $`, never part of one),
  and every later use must equal that token.

There is no `[[x:..]]`. A label cannot use a variable. Each `RUN` starts with no variable bound. A `-NOT` binds nothing: it is
tried after the match that follows it, with that match's bindings.

Three mistakes that are easy to make:

- The printer omits a field that has its zero value, in a payload (`mode=Strict`) and in a record (`role=Source`, `kind=Record`).
  A pattern cannot name one: a positive line with it never matches, and a `-NOT` with it always passes.
- A `-NOT` stops at the positive lines around it. To check that something appears nowhere in a function, give the function a
  block (`-LABEL`) with no positive line, or put the `-NOT` on both sides of the only one.
- A pattern matches part of a line, so `v5: L0` also matches `v5: L0|L1`. Put `CHECK-NOT: |` behind it and go on with `CHECK:`.

## Commands

This prints every command with its default form:

```
testaot list commands
```

A command is registered by the owner of the code it runs, in `aot/**/AOT*TestCommands.cpp`, through one provider in
`FOR_EACH_AOT_TEST_COMMAND_PROVIDER` (`AOTTestCommand.h`). These run the pipeline or a part of it:

- `pass <PassID> { <PassID> }` computes the inputs the pass table lists for the pass, runs that one pass, and prints its results, then
  `outcome: Changed` or `outcome: Unchanged`. With several passes it does so for each in turn, and a later pass sees the results of an earlier one
  (`pass NumberAndEncode ChooseRepresentations RemoveRedundantBarriers` from `Selected` gives the last two the real placement; text in form `Placed` gets an
  empty one, in which no layout has slots). Under `OPTIONS: report=Declines` it then prints one line
  `decline <DeclineKind> f<n> v<n> <LimitID> [<count>] reason=<member>` per decline. The member is one of the enum that belongs to the kind
  (`decodeDeclineReason` in `AOTLosses.cpp`), the flags of `AllocationNotSunk` are joined by `+`, and a decline whose reason is `None`, or
  whose kind has no enum, has no ` reason=`. Then it prints the program. Its default form is `Prepared`.
- `function-passes <PassID> { <PassID> }` runs the named function passes on one function after the other, all of them with one
  `FunctionPassContext` per function, as a schedule does: what a pass remembers in the context about a function is there for the next.
  It prints `f<n> <PassID>: Changed` or `Unchanged` per run, then the program. Passes that derive functions are refused.
- `pipeline <PassID>` runs the pipeline from `AsRead` through the first run of that pass, then prints that pass's results, under
  `OPTIONS: report=Declines` the `decline` lines of all passes that ran, and the program.
- `compile` runs the whole build and prints `diagnostic <kind>`, `loss <limit> x<count>`, `image <empty|not empty>` and
  `statistic <name> <value>` lines.
- `run-in-shell { word }` runs the whole build, then the entry module in `jsc` from the folder of `testaot`. The words are statements of
  JavaScript, joined by one space, which the shell runs first; `id` is the identity function. It prints what the shell printed, then
  `exited with <code>` or `crashed with <signal>`. A failure of a validating build is the line `AOT validation: <Kind> f<n> v<n> ..` in
  front of `crashed with`.
- `list-passes` prints the pass table: id, scope, necessity, forms.
- `pass-context` prints what a function pass can ask its context: heat and "runs at most once" per block, the subject of a value.
- `derived-functions <Role>:<target> ..` asks the table of derived functions for keys and prints, under `OPTIONS: report=Declines` the
  `decline` lines of the builders, and the program after the commit. A target is
  `f<n>` or `f<n>:<parameter>`, `L<n>`, `g<n>` (the declared type of variable n) or `x<n>.<k>` (validation node k of expected result n).

`crash` crashes. Only `AOTChildProcessTests.cpp` runs it, to check that a crash fails one test.

If you write a command:

- It runs next to other commands, so it touches only its context.
- It may replace `context.program` or leave it null.
- For an argument it cannot use, it prints a line that starts with `error: ` and returns. It never crashes on an argument.
- It never calls `Diagnostics::finish`: the runner does, once per `RUN`. After a call that has already finished the diagnostics
  (`runPipelineUntil` returning null), it calls `reopenDiagnostics(build)` (`AOTTestRunner.h`).
- It never calls `takeLosses` unless losses are what it prints. After a call that took them out of the context (a whole build), it
  moves them into `context.losses`.

## Options, limits, statistics

`# OPTIONS:` takes option words, the same words as `bun compile --aot-option`:

```
testaot list options
```

Every test file runs with `check=VerifyEachStage`: the program is verified after each stage. A build of the product verifies the program it reads and
the program it hands to the back end; `check=VerifyEachPass` verifies after each stage and each pass.

The negative test for a limit lowers the limit, checks the answer one level down, and names the loss:

```
# OPTIONS: limit=<LimitID>:<n>
# LOSS: <LimitID> f0 v3
```

To turn off one pass or one flow result:

```
# OPTIONS: disable=pass:<PassID>
# OPTIONS: disable=flow:<FlowResultKind>
```

To run a pipeline with only some of the optional passes:

```
# OPTIONS: only-passes=<PassID>,..
```

Every optional pass but the named ones and `AnalyzeFlow` is off.

## Built-ins in IR text

There is one built-ins table, and `testaot` has all of it, the host's entries included. IR text names an entry, a class, a slot, a
result layout, a runtime module and an overload by its token (`builtin=MathMax`, `Array<number>`, `overload=MathMax/0`). A test that
needs a built-in with some attribute (starts an instance, loads a module, runs unseen code, reads `this`) names a real entry that
has it.

## Native tests

Native tests are for code that has no IR text.

```cpp
struct NativeTest { ASCIILiteral name; void (*run)(); };
AOT_TEST_CHECK(condition)
```

`AOT_TEST_CHECK` prints the file, the line and the condition, then crashes, in every build type.

A file of native tests has one provider: a function that returns a `static constexpr` array of tests. To add one, add its name to
one of the three lists in `AOTNativeTest.h`, define it in the namespace that list is declared in, and add the file to
`JavaScriptCore_AOT_TEST_SOURCES` (`aot/AOT.cmake`).

The `structured-clone/` tests and `webassembly-instance/imported-memory-in-shell` run an image in the `jsc` shell. They start
`jsc` from the folder of `testaot` and fail if it is not there. `AOTShellProcess.h` has the path of the shell. A test that needs a
process of its own (one image, other JSC options) starts `testaot native --single-process <its name>` again.
`AOTTemporaryFile.h` has the temporary file that a test hands to another process.

Native tests run serially. With `--single-process` each name is printed before its test runs, so the last name printed is the
test that crashed. That is also how `native` learns from a child which test to report.

| Header | For |
|---|---|
| `AOTIRTestSupport.h` | functions and programs built by hand, without text |
| `AOTReaderFixture.h` | the bytes of one program that has every kind of record, and where each record lies in them |
| `AOTReaderTestSupport.h` | reads changed copies of those bytes and compares the errors; tables of changes by kind of field (id, enumerator, flags, span, variant) |
| `AOTRealmTestSupport.h` | a VM with a plain realm and a realm whose always-locked built-ins are immutable |
| `AOTLoadedProgramTestSupport.h` | a program in a VM: compiles IR text, starts `testaot` again with the image, runs the body there |
| `AOTShellProgramTestSupport.h` | a program that runs: compiles IR text under option words, runs its entry module in `jsc` with the image and compares what it printed |
| `AOTPrivateNameProgramTestSupport.h` | the same for the programs about private methods, accessors and brands: the default build and nine sets of option words, each of which must print what plain JavaScript prints |
| `AOTCollectionTestSupport.h` | a collection that calls the collector's hooks in a VM without an image |
| `AOTImageTestSupport.h`, `AOTImageTestBodies.h`, `AOTImageBuildSteps.h` | sketches of compiled code, the steps of the image builder one at a time, an image to read back |
| `AOTStubTestSupport.h` | every stub key, body and option variant; a child process for code that must crash |
| `AOTStubRunner.h` | runs one stub of a blob on zeroed memory in place of an instance, with an immediate or a key that binds one; every operation is counted, one may be recorded or be the real leaf; a call lands in code that records its registers or its engine frame; the slot handler tables are filled; `FakeObject` and `FakeAllocator` stand for cells and allocators |

`runWithLoadedProgram` evaluates no module, and the body runs no compiled code.

## What a good test looks like

- It tests one optimization or one rule.
- Every positive case has a negative case next to it: the same shape with the one property changed that must stop the pass,
  checked with `outcome: Unchanged` or `-NOT`.
- A case that gives up shows the answer one level down (the declared type, the larger set, the expected type, the generic path), for
  the one value, site or object that caused it, and that its neighbor in the same function keeps its answer.
- It binds the ids of values a pass creates with `[[name]]`, because the number of a new value is not part of the contract.
- It states the form.
- Its name says the case: `escapes-through-call.ir`.
- It has at most 1,000 lines. A subject with many cases is a folder.

## From a program to running code

The first command is in Bun's repository.

```
bun compile main.ts --aot-out=main.bin
testaot print main.bin --until=<PassID>
testaot compile main.bin --output=main.image validate
jsc --aotImagePath=main.image -e "runCompiledEntryPoint(1)"
```

The shell has eight functions for compiled code:

- `runCompiledEntryPoint(id)` takes an id that `testaot compile` printed, runs that entry point and returns its promise. A
  rejection prints `Exception: ..` and exits with 3.
- `evictCompiledModule(id)`, `setRequireCacheEntry(id, entry)` and `overrideRequire(isOverridden)` tell the engine what a host
  tells it for `delete require.cache[path]`, `require.cache[path] = entry` and `Module.prototype.require = f`.
- `aotOperationCount(name)` returns how often a run-time operation ran. It needs `--aotCountsPath=<path>`.
- `aotRuntimeCounters()` returns the run-time counters as text.
- `aotBuiltinEntries()` returns the built-ins table as Bun's `test/aot/builtins` reads it.
- `storeInSlotWithoutTest(object, name, value)` writes the value into the slot of that field of a compiled object and tests no type: the
  writer that nobody listed, for the tests in `validation/`. It throws where the object has no such slot or the form of the slot cannot
  hold the value.

The shell has no host: a program that calls a built-in of Bun's compiles, and the call throws when it runs.

The run time refuses what the flow analysis did not see, in the shell too. With an image that admits no unknown JavaScript, `eval`,
`Function` and the other function constructors throw an EvalError in every realm, also when the script of the shell calls them
(that script itself runs). With an image that loads no module by name, `runCompiledEntryPoint` throws for a module that has no
`entry` line, and so do `RequireDynamic` and `ImportDynamic` in a function with `role=Library`. With an image in which nothing
replaces what the loader answers, `setRequireCacheEntry` and `overrideRequire` throw, and `evictCompiledModule` throws for a
CommonJS module whose body is proven to run at most once. The option word `admit=Inspector` opens all of it: the flow analysis then
states nothing, and any code may run.
`AOTClosedWorldTests.cpp` has the cases.
