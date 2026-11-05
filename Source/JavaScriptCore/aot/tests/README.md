# AoT engine tests

## What is here

`testaot` is the only test program for the engine half of the compiler. It runs IR text tests and native tests.

An IR text test is `<stage>/[<subject>/]<case>.ir`, for example `driver/check-next.ir`. Native tests are the
`AOT*Tests.cpp` files and `unit/`. `AOTShell.cpp` is not part of `testaot`: it is everything the `jsc` shell knows about AoT.

The WebKit repository has no front end, so nothing here compiles `.js` or `.ts`. Whole programs and their expected output are in
Bun's `test/aot/parallel`, and the tests of lowering are in Bun's `test/aot/ir`.

Every boundary between two stages is text. So a bug gets a test at the stage that has it.

| Boundary | In | Out | Commands | Where |
|---|---|---|---|---|
| Wire IR | source text | IR text | in Bun. Here: `print`, `round-trip`, `verify` | Bun's `test/aot/ir`, `ir/`, `verifier/` |
| Scan and Link | IR text | the thin summary of a function, Link's tables, the texts that arrive at RegExp | `summary`, `link`, `regexp-texts` | `link/` |
| Plan | IR text | slots, shapes, variables, units | `plan` | `plan/` |
| Walk | IR text | Code text, walked and swept | `walk` | `walk/`, a folder for each mechanism |
| Code | Code text | Code text, machine operations | `code-round-trip`, `verify-code`, `sweep-code`, `emit-code` | the section `code` of a test file: `walk/code-text/`, `walk/sweep/` |
| Machine | operations built through `Output` | what they answer when run in the test process, and the allocation as text | native tests, `emit`, `order`, `allocate`, `frame`, `bytes` | `AOTMachine*Tests.cpp`, `AOTOutputTests.cpp`, `machine/` |
| Whole programs | IR text | what the program prints | `run-in-shell` | `answers/` |

| Folder | Tests |
|---|---|
| `driver/` | the directives and the matcher themselves, with `print` and `verify` alone, and the word `mechanisms=`; a whole build through the queue of Compile, which holds each mention against `LinkTables::referrers` |
| `ir/text/`, `ir/text/errors/` | each family of operations and each record prints and round-trips; a mistake in IR text is reported with its line |
| `ir/index/`, `ir/edge-results/` | the body index and the liveness of the input; what a passed test hands over its edge |
| `verifier/` | each file breaks one rule of the wire IR, and `verify` names the rule |
| `types/` | the algebra of types |
| `link/` | what a summary says and leaves out; what a read, a store, and a call by name, by value and by binding find when a type arrives; what an operator gives for what has arrived at its operands, and what may be called an integer that fits int32; classes, their members and their prototype objects; lost against escaped; each change of a layout; reach, recursion, a cycle of edges, a join of a family; who may name a function |
| `plan/` | which units exist, shapes and variables; `plan/typed-slots/`: which slots hold raw values |
| `walk/<mechanism>/` | for each mechanism: what it does, what it must leave alone, the same input with the mechanism withheld (`..-withheld.ir`), and a suspension in the middle of its territory |
| `walk/code-text/` | Code text is read and printed; each file breaks one rule of Code, and `verify-code` names the kind and the rule, with green controls beside them. No walk is run |
| `walk/sweep/` | the sweep by itself, on Code that is written out: what goes and what stays, stores that wait for their object, handlers, barriers, hints, what is live at a suspension, and how often an operation is visited. No walk is run |
| `machine/` | a unit's operations, allocation, frame and bytes as text: frames, polls, entries, calls, exceptions, code shape |
| `image/` | what the flags, sections and tables of the image say to the run time |
| `answers/` | programs that run in the shell and hold what they answer, and nothing else: no count, no shape of code. The shell runs the same text itself and compares. Bun's `test/aot/tools/answers-test-from-source.ts` makes such a file from a program |
| `host/`, `unit/` | the stand-in host's table of built-ins (no tests); native tests of the table of built-ins |

## Running

```
testaot all <folder>                                              native tests, then every *.ir under the folder, in parallel
testaot run <file or folder>.. [--filter=<text>] [--jobs=<n>] [--show-output] [--single-process] [--option=<option word>].. [--only-diagnostic=<kind>]
testaot native [<filter>] [--after=<name>] [--part=<i>/<n>] [--single-process]
testaot list commands|options|mechanisms|statistics|diagnostics|image-format
testaot <command> <program> { argument } { option word } [mechanisms=<list>] [--program-of-tag]
testaot print <program> --wire
testaot compile <program> --output=<image> { option word | statistic } [mechanisms=<list>] [--program-of-tag]     prints "statistic <name> <value>" for each StatisticID named, then "entry <i> <id>" for each entry point
testaot write-out <file>                                          prints IR text with REPEAT, PROGRAM-OF and the labels of check lines inside functions written out
testaot fuzz-allocation [--seed=<n>] [--count=<n>] [--blocks=<n>] [--show=<seed>]
```

`<program>` is IR text, or the bytes that `bun build --aot-out=<file>` writes. `--program-of-tag` takes bytes that a Bun built from the
tag wrote: the hash of the schema is not compared. See "From a program to running code".

`<command>` is any name that `testaot list commands` prints: `testaot emit f.ir f3`. It is what a `RUN` line runs, without the check
lines. The words are read differently. On a `RUN` line every word is an argument of the command. On the command line:

- A word that begins with `--` is a flag of `testaot` or a JSC option. So the shell options of `run-in-shell` come only from a `RUN` line.
- Every other word is first tried as an option word. A word that the options refuse is an argument of the command if it holds no `=`, and an
  error if it does. So `says=Pow:Open` reaches `bytes-by-form` only from a `RUN` line.

The section `code` and the `OPTIONS` lines of a text file count too.
`compile` is also the name of a command. On the command line it is the line above, which writes the image.

**Mechanisms.** Every mechanism of the middle but the first, closed operations, can be withheld, and only here: it is no option of a build.
`driver/AOTMechanismWords.h` has the word, and `testaot list mechanisms` prints it with the names.

```
mechanisms=none      mechanisms=all      mechanisms=none+TypedOperations+Guesses      mechanisms=-Inlining-Guesses
```

- `none` and `all` set the whole and come first. `+name` adds a mechanism and is refused while something that it leans on is withheld.
  `-name` withholds one, and with it all that leans on it. Without `none` or `all` the list changes the set as it stands.
- Every command takes the word: on a `RUN` line, where it is no argument of the command, on an `OPTIONS` line, behind `--option=`, and on the
  command line. The words are applied in this order: `OPTIONS`, `--option`, the `RUN` line or the command line.
- `--mechanisms=<list>`, on any command line of `testaot`, is a ceiling. What the list lacks is withheld in every build that the process and
  its children make, whatever a native test or a word of the command line asks for. `testaot all <folder> --mechanisms=none` is the first run of
  a bring-up, and `--mechanisms=none+Folding` the next. A `RUN` whose words (its own, those of `OPTIONS` and of `--option`) ask for a mechanism
  above the ceiling is left out, as one is whose `REQUIRES` does not hold. So a failure under a ceiling belongs to what the ceiling lets through.
- A native test of a whole program compiles it in each of `waysToCompilePrograms` (`AOTTestHarness.h`), with all mechanisms and with none,
  and expects the same of both.

The exit code is 0 if nothing failed, 1 if a test failed, no test matched or a build reported an error, and 2 if the command
line is wrong. Each failure is a line `FAIL <file>:<line>: <why>`, and the last line is `<n> passed, <m> failed, <k> todo`. A file
that fails under a `TODO` line is not a failure: it prints `TODO <file>:<line>: <owner>: <what is lost>` and, indented below it, where
it still fails.

`--filter=<text>` runs only the files whose path contains the text. `--jobs=1` runs the files one at a time and prints each path
before the file runs. `--show-output` prints, after a file's failures, what each of its `RUN` lines printed, under a line
`OUTPUT <file>:<line>:`.

`--option=<option word>` adds an option word after the `OPTIONS` of every file. A failure then ends with ` (with <word>)`.
`--only-diagnostic=<kind>` runs the files for that diagnostic alone: a file fails if one of its runs reports it, nothing else of the file is
compared, and a file that expects the diagnostic itself is left out. Together they run a folder in another way than its files were written for.

A test that crashes fails alone, because tests run in child processes. `run` starts one child for 16 files. If that child crashes or
hangs, each of its files runs again in a process of its own. `native` starts one child for all its tests, and after a crash another
one for the tests that are left. The failure is `FAIL <file>:1: crashed with <signal>` or `FAIL <name>: crashed with <signal>`, below
the last lines the child printed. A child that prints nothing for 100 seconds (300 with ASan or assertions) is killed, and its test fails.

Under a debugger, run the test in `testaot` itself: `run` with a single file starts no child, and neither does `--single-process`.
`--after=<name>` runs only the native tests listed after that one. `--part=<i>/<n>` runs every n-th native test, from the i-th on.

`print --wire <program>` is what the front end's tests call. It reads the program, keeps its text with wire fields only, then
verifies it. Without a diagnostic it prints that text and exits with 0. Otherwise it prints nothing on stdout, one line
`<Kind>: <message>` per diagnostic on stderr, and exits with 1.

`--show-refused-bodies`, on any command line, prints each body that the verifier of bodies refuses to stderr, in the children too.

Any other `--<name>=<value>` is set as a JSC option, as in `jsc`. WebKit's own test script runs `testaot all` on this folder:
`Tools/Scripts/run-javascriptcore-tests --testaot`.

The parsers and the printers of IR text and Code text are sources of `tests/`. No shipped binary holds them.

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
REQUIRES: [ x86_64 | arm64 ] { word }
REQUIRES(<prefix>): [ x86_64 | arm64 ] { word }
OPTIONS: <option word> { option word }
JSC-OPTIONS: --<name>=<value> { --<name>=<value> }
TYPES: f<n>
<P>: p
<P>-NEXT: p
<P>-NOT: p
<P>-LABEL: p
<P>-COUNT-<n>: p
DIAGNOSTIC: <UserDiagnosticKind or InternalErrorKind> [ <count> ]
STATISTIC: <StatisticID> ( = | >= | <= ) <n>
LIBRARY: own
TODO: <owner>: <what is lost>
HANGS-ON-ORDER: <owner>: <what differs>
REPEAT: <name> = ( <first>..<last> | <word> { <word> } ) { ; <name> = .. }
END REPEAT
PROGRAM-OF: <file>
```

A directive is a whole comment line: `# NAME: value`, with one blank after the `#`, the name in capitals and the colon right after it. A
comment after IR tokens on the same line is never a directive.

A comment that was meant as a directive is an error, not a comment:

- Its first word is the name of a directive or a check prefix of the file, in capitals, and the line is not written as above
  (`# CHECK-NEXT v0 = Add`, `#CHECK: x`, `# CHECK : x`).
- Its first word has a colon after it or a suffix (`-NEXT`), and is such a name in another case (`# check: x`) or in capitals and within two
  edits of one (`# CHEKC: x`, `# CHECK-NXT x`).

Any other comment is prose. So prose does not begin with `RUN`, `CHECK` or a prefix of the file in capitals, and never with capitals and a
colon: `# NOTE: ..` is an unknown directive, which fails the test. A prefix is `[A-Z][A-Z0-9]*`. Words are split at spaces and tabs, and
there is no quoting. Directives can appear in any order. Only the order of the check lines with the same prefix matters.

A line that begins with `# --` is a marker. Any marker but these is an error.

```
# -- source: <file>            the lines down to `# -- end of source` are a program, each after `# `, and none is a directive. Another such line begins another file
# -- end of source             there is one such block at most, and it ends
# -- code                      Code text, down to `# -- end of code`. See "The section `code`"
# -- generated: <function> ..  once. Each name (or `f<n>`, or `*`) is a function of the program and has its -LABEL line below. Only check lines follow
```

Bun's `test/aot/tools/update-ir-tests.ts` rewrites the IR text from the source, and everything below `# -- generated:` from what the
runs print. List the functions the test is about, and a library function (`role=Library`) only if the test is about it. In a source, print
strings: `console.log` of a value of unknown type keeps about 500 library functions reachable, which is 1.6 MB of IR text.

Each `RUN` parses the file again (directives are comments to the parser) and verifies the program: first its records, then its bodies.
`print` and `verify` get the program unverified, because they report on it themselves. A verified program has an entry point. So every file
that hands a program to another command has a line `entry module=m0`. Without it the run reports `NoEntryPoint`, and the command does not run.

Then the command runs on a new `BuildContext`, and what it printed is matched against the check lines with its prefix. There is one form of
the IR: the wire IR, as a front end sends it. `OPTIONS`, `JSC-OPTIONS`, `TYPES`, `DIAGNOSTIC` and `STATISTIC` apply to every `RUN` in the file.

**`LIBRARY`.** A front end always sends the library functions that the built-ins table names. So that a test need not, the Machine commands
and the commands on whole builds get a stand-in library: one more module without a body (`flags=[IsLibrary]`) and in it one function,
`role=Library`, that returns `undefined`, registered for every runtime function that the text has no `runtimefunction` line for,
but for the helpers of the compiler (the entries under `CompilerHelpers`: steps of an iteration, sorts, splits, spreads).
`testaot compile` on IR text and the native tests that compile IR text do the same. In such a program every built-in that is written in
JavaScript (`Array.prototype.map`, `new Promise`, `Object.entries`) and that the program can come by returns `undefined`. A test that runs one writes the `runtimefunction`
lines of the functions it uses. The line `# LIBRARY: own`, written exactly like that, says that the text brings all it needs: nothing is added.
The stand-in answers for the statements of the shell too, wherever the build has compiled it: for the built-ins that the program can come by (`link/library-*.ir`),
which is all of them once unseen code can run. So a test whose
program or statements run such a built-in and bring no text for it (`map`, `Array.from`, `apply`, a `for of` that goes by the protocol, a generator) has the
line as well: the engine's own functions then run.

**`JSC-OPTIONS`** sets JSC options in the process that runs the commands, and only while the file runs. The shell of `run-in-shell` is
another process and takes its own options.

**`TYPES`** names the function whose parameters give types their short names. `t<k>`, in an argument, is the declared type
of parameter k of that function. Without the line it is `f0`.

**`REQUIRES`** is for output that depends on the target architecture, which is the CPU `testaot` was built for. The `RUN` lines with
its prefix (`REQUIRES:` goes with `RUN:`) run only on that architecture. Their check lines are parsed on every architecture, so a
malformed one fails everywhere. A test that names a register has a `REQUIRES`. Check what all targets share under a prefix without one.
With or without an architecture, it takes these words:

- `unsanitized-code`: not AddressSanitizer. Such an engine is a checking build, and a run that expects sizes or faults may find other code there.
- `not-windows`: on Windows, x86-64 code follows another calling convention. `windows` is the other half.
- `faulting-loop-polls`: a loop poll is a load that faults (on Linux, macOS and Windows; not in a hardened engine for ARM64).
  `calling-loop-polls` is the other half: a comparison with a call on its slow path.
- `no-jscvt`: a number is converted to an int32 by a truncation, a test and a slow path. ARM64 with FEAT_JSCVT (macOS) has one instruction for it.
- `code-outside-image`: the engine may run code that is not in the image. A hardened engine never does.

**`DIAGNOSTIC`.** A run must report exactly the diagnostic kinds listed. Kinds are compared by name, never by message, and warnings and notes
count too. A kind listed without a count must be reported at least once. With no `DIAGNOSTIC` line, the run must report no
diagnostic. A program that does not parse or verify is not a failure by itself: the command does not run, and the `DIAGNOSTIC`
lines alone decide. `# RUN: verify` with `# DIAGNOSTIC: <InternalErrorKind>` is how to test the verifier.

**`STATISTIC`** holds a counter of the build to a number.

**`TODO`** marks a test that shows a debt of the compiler. The test keeps
its expectation and still runs. While it fails it is counted as `todo` and not as `failed`, so that `failed` means something new. As
soon as it passes it fails, with `passes: remove the TODO line`, so no mark outlives its reason. The text names who owes it and, in
plain words, the thing that is lost. A file has at most one. A file that crashes, or whose directives are wrong, still fails. A test
whose expectation is out of date is restated and gets no `TODO`, and neither does one that shows wrong code. Under `--option` and
`--only-diagnostic` a file with a `TODO` line may pass.

**`REPEAT`** is for a program, or for check lines, that are a loop written out. The lines up to `# END REPEAT` are written once for each
value of its names before anything else reads the file. A name takes the numbers of a range, both ends included, or the words of a list,
and all names of one `REPEAT` take equally many values. `%(name)` is the value. A number can be multiplied, moved and padded with zeros, in
this order: `%(i*2+1)`, `%(i-3)`, `%(i:2)`. One `REPEAT` can be nested in another. Both lines start in the first column, and `# END REPEAT`
has no colon. A failure names the line in the file; a line that the parser names in a message is a line of `testaot write-out <file>`.
`driver/repeat.ir` is a model.

**`PROGRAM-OF`** is for a test of a program that another test has: the same text under other options, through another
command. The line stands for the lines of the named file that are not comments, so none of its directives and sections come along. The path
is relative to the folder of the file that has the line. The named file has the program itself, not a `PROGRAM-OF` line.

**Check lines inside the braces of a function** are about that function. An implied `<P>-LABEL: function fN name=".."`, with the id and the
name from the function's line, comes before the first one of each prefix. That is how `print`, `body-index`, `edge-results` and `liveness`
begin a function. The commands of later stages begin a unit differently (`code f13:Body`, `unit f13:Body`), so their check lines go outside
the braces, with a label of their own.

```
# RUN: body-index
function f13 name="size" parameters=[{type=L0|L2}] results=[{type=anything}] {
    v1 = PropGet v0 key="size"
    Return v1
# CHECK: v1 PropGet block: b0 uses: 1 only user: terminator of b0
}
```

## The section `code`

The section holds Code text: what the sweep and the emitters take. It starts at `# -- code` and ends at `# -- end of code`. Each line is a
comment: the text is what follows `# `, and `#` alone is an empty line. Its lines are not directives. A file has at most one. It goes outside
the source block and before `# -- generated:`. An error in it names the line of the file where the text starts, and the line within the text.
`code-round-trip`, `verify-code`, `sweep-code` and `emit-code` read it.

Code text is the line `code f3:Body {`, its lines, and `}`. The first line names the unit, and the section cannot leave it out. Without
a role the unit is the `Body`. The grammar is at the top of `AOTCodeParser.cpp`.

- An operation is one line: `vN: TYPE @REPRESENTATION = Operation ..`. The proven type and the representation are left of the `=`. Right of it
  are the words of IR text, with the engine's fields.
- A record that leans on facts is followed by its `fact` lines.
- What the sweep adds (`# swept`, `# dropped v3`, `# uses=2`) comes after a `#`, which the parser skips.
- Records and blocks are numbered without a gap. So the text of swept Code parses only where the sweep dropped nothing.

`ir/AOTTypeText.h` has the grammar of the types in Code text. It has more parts than the
wire: named objects (`site(f0, v3)`, `instance(L2)`, `escaped(L2)`), and after a `?` the seeds that an `unknown` came from.

## IR text

IR text is a second spelling of the records of `aot/ir/AOTSchema.def`. Printer and parser are generated from the schema, so the
schema is the reference for every keyword, field, operand, payload name and flag.

- A list with a keyword prints one line per record: `layout L0 name="Point" fields=[..]`. Fields come in declaration order. A field
  that has its zero value (0, false, absent, empty, the first enumerator) is not printed, and a field that is left out
  parses as zero. The zero value of an id is absent, not id 0: `module=m0` and `body=f0` are printed.
- An id is a letter and a number: `v` value, `b` block, `f` function, `L` layout, `g` variable, `m` module, `q` module failure,
  `w` type site, `e` eval scope, `x` expected result, `d` data.
- A flag word is `[A, B]`, a nested record `{field=value ..}`, a variant `Case` or `Case{field=value ..}`, a list `[a, b]`.
- A property key is `"x"`, `@"x"` (engine private name), `#"x"@L2` (private name of layout 2), `@@iterator`, `symbol@m0:120`
  (the symbols that the call with `callNumber=120` in module 0 makes).
- A type is parts joined by `|`: `undefined|number`, `i32`, `int[0, 9]`, `"a"|"b"`, `L3|L4`, `Array<number>`, `function`, `f3`, `env L1`,
  `builtin(MathMax)`, `anything`, `never`, `object`, `absent`. `L3` reads as its family: the layouts below it.
- A position is `{type=T site=w0 flags=[..]}`. The type is what was written. It claims nothing about the values that arrive: a `Convert`
  marks a value that enters a position.
- A value is `vN = Operation operands.. name=value.. [flag, flag] @position`. Operands come first, in declaration order, separated by
  commas, with `-` for an absent optional operand that another operand follows. An operation without a result has no `vN =` and
  still takes the next id. The position is the offset in the module's text that a stack trace names.
- A terminator is `Name operands.. name=value.. -> target bK(arguments | proofs), ..`.
- A block is `bN(parameters) role=.. subject=vK handler=bK(arguments):`. The head of `b0` may be left out. The block then has the
  parameters that the function's line gives it, `v0` onward: the captures, `this`, the callee, the new target and the parameters, in this
  order, each with its declared type. The printer always prints the head.
- A body has no engine fields: no proven type, no expected type, no representation. Those are in Code text.

A test passes what it proves along the edge. The parameters of a block are filled by the explicit arguments, then by one edge
result per proof. Here `v2` is `v0`, cut by the type `string`:

```
  b0(v0: anything):
    v1 = TypeTest v0 testedType=string
    Branch v1 -> taken b1(| 0), notTaken b2(| 0)
  b1(v2):
```

**Built-ins.** A program has one built-ins table, and whoever links `testaot` compiles it: the engine's entries (`aot/builtins/*.builtins.ts`)
and a host's. WebKit's own build links the stand-in host's (`aot/tests/host`), which has few entries. Bun's build links Bun's. IR text names an
entry by its token (`builtin=MathMax`, `runtimefunction id=ArrayPrototypeMap`, `overload=MathMax/0`). A test in this folder says the same
under both tables:

- It names entries of the engine and of the stand-in host alone. A token that only Bun's table has (`builtin=QueueMicrotask`) does not
  parse in WebKit's build. To hand a function to the host, which calls it later: `Construct` of `builtin=FinalizationRegistry`.
- It pins nothing that hangs on what else the table holds: the number of a name or of an object, or how an entry that both hosts
  declare (`BunServe`) treats an operand the test is not about.

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
- A pattern matches part of a line, so `v5: L0` also matches `v5: L0|L1`. Put `CHECK-NOT: |` after it and go on with `CHECK:`.

## Commands

`testaot list commands` prints every command with its arguments.

`f` is a function, `f3`. Without one, a command takes every function with a body. A `unit` is one piece of machine code that the compiler makes
of a function: `f3`, `f3:GenericCallEntry`. That is a function and a `CodeRole` by its C++ name (`ir/schema/operations.def`).
Without a role it is `Body`. Without a unit, a Machine command and `same-bytes-alone` take
every unit that the build compiles: entries and units of the library too.

**Wire IR** (`AOTIRTestCommands.cpp`)

- `print [f..]` prints the program, or the named functions.
- `round-trip [wire]` prints the program, reads that text and prints it again. Then `round trip: same` or `round trip: differs at line <n>`, and
  the same for the bytes (`round trip of bytes: ..`). `wire` prints wire fields only, which is what a front end sends, and compares no bytes.
- `verify` prints `verified`, or one line `<Kind>: <message>` per diagnostic.
- `body-index [f..]` prints, per block, the dominator, the loop, the depth, the incoming edges and the flags. Per value, the use count (`many`
  where it saturates) and the only user. Then the loop tree.
- `edge-results [f..]` prints each edge result: proof, test, subject, operand index, side or case, parameter.
- `liveness [f..]` prints what is live at each head, the last use of each value, and what is live across each suspension.

**Types** (`AOTTypeTestCommands.cpp`)

- `subset`, `union`, `intersect`, `pass-filter`, `cast`, `canonical`, `rep`, `members`, `truthy` and `edges` print the answer for each of `t0`, `t1` .. or for each
  pair of them, check the laws of that operation, and end with `laws hold` or with `law broken: ..` lines. Each argument is one more type, numbered
  behind those of the parameters, in the text of `ir/AOTTypeText.h`: `fractional[0,5]`. It is one word, so it holds no blank.
- `laws-of-types` holds every pair of those types to every law of the algebra. `find-field` and `layout-set` take layouts as arguments.
- `aliases` prints, for each pair of those types, the objects of the first that the second names under another number, and how many pairs of
  objects were held side by side for that. Then it holds `ObjectSetAliasIndex` against `mayBeSameObject()` over sets of the program's objects and
  of their copies for instances, and with it `mayBeSameObject()` of two types and `membersWithAliasIn()` in an arena of a thread, across `reset()`. It
  gives `f0` sites to name: `site(f0, v<2n>)` has `L<n>`, `v<2n + 1>` has `L<n>` or a layout below it, and five without a layout follow.

**Scan and Link** (`AOTFactsTestCommands.cpp`). Both print what `computeFacts()` has left behind `Facts`. The top of each printer shows its lines.

- `summary [f..]` prints the thin summary of each function: `summary f1 operations=<n> traits=[..] effects=..`, then its nodes by number (`n1 Parameter 0`,
  `n2 Field L0.1 "x"`, `n3 Capture 0 of f3`, `n4 Member L0 Static 1`, `n5 Join`, `n6 ClassInstance Array #8`, `n7 Contents Element of ClassInstance Array #8`), then its records, then its edges.
  `ClassInstance Array #8` is an allocation site: the list that `v8` makes. Without a number: the object of the coroutine itself. `Contents <slot> of <site>` is what the objects made there hold in the slot. The records come first so that a check
  line can bind one (`access [[read]] v1 Read Parameter 0 "x"`) and then follow what it gives (`edge [[read]] -> Result 0`).
  A record or an edge names a node by what it is, never by the number that the scan gave it: `Parameter 0`, `Closure f2`, `Constant string`. It names a result
  by its record (`c0.1`, `a2`), and only a join by its number (`n5`).
  - `call c0 v5 Name "size" this=Parameter 0 (Constant string) in-loop`. The callee is `Name <key>`, `Value <node>`, `Binding f2` or `Builtin ObjectFreeze`.
    Without `this=` no receiver is passed. `new-target=<node>`, `constructs`, `in-tail-position` and `passes-list` stand where they hold.
  - `access a0 v1 Read Parameter 0 "x" -> a0`, `access a1 v2 Store a0 [Parameter 1] <- Constant string`, `access a2 v3 Define Parameter 0 "y" <- Closure f2 at-construction`,
    `access a3 v4 Has Parameter 0 "x"`. In brackets: the node that a computed key comes from.
    An access to what an object of one of the engine's classes holds names a slot in place of a key: `access a4 v6 Read Parameter 0 contents Element -> a4`, `.. Store Parameter 0 contents field 2 <- Parameter 1` (an internal field).
    `access a5 v7 Read c0.0 contents Value else n4 then n7 -> a5` is a wait (`await`, a promise that is resolved): `else` is the node that gets whatever of the subject is no promise, `then` the node with what `then` finds on that.
  - `change h0 v8 Freezes Parameter 0`, `change h1 v9 DefinesByDescriptor Parameter 0 "x" other=Parameter 1`.
  - `inspection v3 WalksChain Parameter 0`, `inspection v5 ListsKeys n2`: the operation walks up the chain of what lies at the node, or lists its keys, which runs a trap on a proxy.
  - `on-no-way-to-return` at a call, an access or a change: no way leads from it to a Return. On the head line `writes-less-on-return`, `operators=<effects>` with `may-join-texts` (the operators that are left out of `effects=`),
    `has-handler`, `own-writes-in=any|<n>`. `operands-of-operators Parameter 0, a1`: where the operands of those operators come from.
  - `change h2 v3 CopiesInto Allocation L1 other=Parameter 0 defines`: the copy defines each property, as `{ ...other }` does, and no setter takes a value.
  - `handler b3 caught=n7`: the join with what the handler block catches, which its `CatchValue` gives. A call or an access under the handler ends in `throws-to=n7`. Without that word it throws to `Thrown`.
  - `member L0 Prototype 0 Method "size" = Closure f2`, `member L0 Static 1 Getter [computed] = Closure f4`, `classes L0` for the classes it evaluates, and
    `hands-out m1` for the modules whose namespace object, or a promise of it, an operation gives.
  - `allocation-lists L0 "flag" "tail"`: what an allocation of the body lists, as written, where that does not begin the order in which the layout declares its fields. The plan takes an order from it.
  - Of the body of a module without a table of exports: `stores-first g0 at v2, g1 at v9`, the variables that it stores into on every way in front of its own first read, and `uses-first f1 at v4`,
    the first operation that does more with a function than make a closure of it. `names-records-of m1`: an operation names a variable or a function of another module.
  - `edge Parameter 1 -> Field L0.1 "x"`, with `passes=<type>` where a test of the program stands between. `edge Parameter 0 -> Unknown` is an escape: there is
    no list of what escapes, and none of the names that are read, called or stored.
  - `under-constant-key v2 "kind"`: an access under a computed key that the constants of the body make one under a name; its `access` line has the name. ` begins-with "on"` at the end of an `access` line: `o["on" + kind]`.
    `reads-paths-of m0`, `may-read-path-of-any-module`: `Facts::isPathReadByOperation()`.
  - Behind the layouts `link` prints `parsers-may-make L0` (`Facts::parsersMayMake()`) and `path-unread m1` (`!Facts::isPathReadByOperation()`).
- `test-run [oldest-first|scattered]` prints what the build knows of a test run (`facts/AOTTestRuns.h`; `# OPTIONS: admit=ReplacedBuiltins` makes one): `through-mock m1 "space"` for each export that may be a mock's
  (`mayComeThroughMock()`), by module and place, and behind them `any-other-name m1` where that holds of a name that the module lacks . `replaces MathImul` for each member that the table locks and the image opens because a write was followed to it . `adds FunctionPrototype "marked"` for each opened name, those
  of the wire in front of those that Link has found . `left-to-program <count>`: the members whose lock the table of the build leaves to the program, 0 in an executable.
- `closure` prints nothing where Link's flow ends closed. Behind the flow it goes along every edge and tells every observer once more, as one that has never been told,
  in each of three orders of the worklist (`newest first`, as a build has it, `oldest first`, `scattered`). A node that climbs then, or a set that grows, was missed by an observer that
  decided on the state at the time of its telling, and is a line: `oldest first: f5.node2 climbs from f2 to lostFunctions | brought by subject of call 0 of f5, told at f5.node1`,
  `reached grows`. `run` asks it of every file that has a program, behind the file's own `RUN` lines, and a line is a failure of the file.
- `facts enforced [counts]` (`AOTEnforcedFieldsText.cpp`) prints `mechanisms <list>`, then a line for each field of which `facts/AOTEnforcedFields.h` says something: what its slot is held to, where that comes from, and the slot that the plan has made of it:
  `field L0.1 "flags" source=written type=i32 markers spares-test slot=JSValue held=undefined|null|i32`, `field L0.7 "id" appended source=family ..`, `field L2.0 "socket" taken-back`. `source=written | family | proven | compiler`.
  `markers`: `FieldFact::SlotHoldsUndefinedAndNull`. `spares-test`: the type takes all that a raw form holds, so in that form a store by name tests nothing. `int32-by-guess`. `held=`: `PlacedSlot::type`; without the word the plan holds the slot to nothing.
  `differs-from-table`: `enforcedTypeOfField()` and `Facts::field()` say two things of one field, which is a bug. A field whose slot takes every value has no line. At the end two lines of `counts`; with the word `counts` those alone.
- `regexp-texts [hashes] [realms]` prints a line for each text that Facts list for RegExp (`facts/AOTFactsRegExpTexts.cpp`), those that the program spells in front: `text /a+b/i`, with the flags that a record
  goes by (neither `g` nor `d`), and ` with flags that nobody knows` where the rehearsal knew the text alone. `hashes`: the length of the text and a hash in its place.
  `realms`: in front, `realms <n>`: how many VMs the build has made by then. A program whose own code hands RegExp nothing to work out gets none.
- `link [held] [functions|calls|accesses|fields|variables|captures|members|layouts|families|counts].. [oldest-first|scattered]` prints `mechanisms <list>`, then the named tables in the order of the words, or all of them.
  `held`: `Facts::held()` in place of `Facts::linked()`: what the run time and checked stores hold, in which `proven` and `changes` are no proofs.
  `oldest-first` and `scattered` take the worklist of the flow in another order than a build does. The tables may not hang on it: `run` holds the three against each other for every file that has a program.
  A file with a `HANGS-ON-ORDER` line owes that, and nothing else of the file is touched by the line. As soon as the three agree, with all mechanisms, the line is a failure.
  `counts`, only where it is named: how much Link has proven of the whole program, in numbers alone.
  `lookups`: `LookupsPastProgramObjects`, field by field, a line an entry: `lookup name-read "size"`, `lookup name-called "push"`, `lookup read | called | let-go | called-out-of-sight ArrayPrototypePush`, `lookup taken-for-constant | written | written-under-any-name | lost | at-unseen DateNow`, `lookup write Date at f0 v3`, `lookup instance-at-unseen Map`, the names and flags of reads and writes through lost built-ins and through what is unseen.
  The uses of built-ins, the locks and the reports are made of it. It is among the parts that `link` prints without a word, so `run` holds it against the other orders of the worklist too.
  A line says only what is claimed: `function f1 callers-known one-callee fresh-result cycle=2 rank=1 result=number returns=[..] throws=none`, `function f2 unreached ..`,
  `parameter 0 expected=number declared proven=i32 kept`, `referrers f0, shared call 0`, behind the functions `group shared call 0 cycle=1 rank=0` (or `anybody`, `constructors of F1`) with its own `referrers`, `call f0 v5 -> f1`,
  `access f1 v2 under symbol("depth")@m0:0` (an access under a computed key that can only be that one symbol: the walk builds it by the name),
  `field L0.1 "x" proven=number writers-known written-at-construction-alone` (`filled-by-accessors-alone`: only accessors that literals lay are seen to fill it), `variable g0 proven=f1 writers-known`, `capture f2.0 proven=L1 writers-known`,
  `member L0 Prototype 0 Method "size" function=f2 writers-known`, `layout L0 family=F1 changes=[GainsProperty]`, `layout L3 never-allocated changes=[]`, `layout L2 changes=[GainsProperty] filled-under-computed-keys` (`counts[word] = 1`), `layout L4 changes=[] heritage=L2` (`class Socket extends NetSocket`, written without a base: what arrives), `on-prototype L0 "width" getter=f4 setter=f5` (what a descriptor written in the body lays: expected), `family F1 [L0, L2]`.
  `keys-never-listed`, at the end of the line of a layout: nothing ever sees in which order its objects got their keys. `closures-stay-as-made`, at the end of the line of a function: `LinkedFunction::closuresStayAsMade`.
  `only-given-undefined`, on the line of a field: its makers give it the constant undefined alone. `accessors L0.0 "total" getter=f2 setter=f3`, among the accesses: the one function that literals lay there as either half.
  `summary f1` has `lists-keys-of n4, n7`: the objects on which an operation of the body runs OwnKeys or OwnValues.
  The parameters are the entry parameters: captures, `this`, the callee and the new target come in front of what is written. `kept`: the parameter does not escape.
  `coroutines`, one more word for a table: `coroutine f1 fulfilled=L0 returned=L0 as-made`, `coroutine f2 yielded=i32 sent=undefined as-made only-iterated`: `LinkedCoroutine`, a line for each coroutine that is reached. `never`: nothing ever arrives.
  In `summary`: `only-iterated` at the end of a `call` (`SummaryCall::resultIsOnlyIterated`), and `Contents yielded of ClassInstance Generator`, `Contents returned of ClassInstance Promise` for what the body yields and returns, whoever has the object.
  `catch f1 b3 caught=L0`, behind the functions: what was seen to arrive at the handler block beside whatever nobody knows. A guess in both readings. No line: nothing was seen.
  `runs-once`, at the end of the line of a function: in one realm it runs at most once. `in-queue-from-start`: `LinkedFunction::isInQueueFromStart`. `takes-arrivals`: every caller known, every call with one callee, no referrer in its own cycle.
  `ring-by-value`, at the end of the line of a function: it stands in a ring of calls in tail position of which one goes through an entry by value, so it returns boxed values.
  `function=`: the one function whose closure the member holds in every run. Effects are `anything`, `none`, or the traits of Code text.
  Behind `throws=`, where no unknown code runs: `fields-written-on-return=1 fields-written=2` (how many of the 64 places of the fields of objects are written, `all` for the entire heap, nothing where none is) and
  `layouts-written=none|any|<n>` (`LinkKnownEffects::layoutsWrittenBy()`: how many bits). Below a function `given-arguments as-expected=[0] called=[1] layouts-written=.. returns=.. throws=..` (`LinkedEffectsGivenArguments`).
  A type that is `anything` is not printed. So where anything is the right answer, a test checks with `-NOT` that each narrower answer is absent.

**Plan** (`AOTPlanTestCommands.cpp`)

- `plan [layouts|variables|deferred|shapes|units|module-starts|literal-strings|orders|classes]..` prints `mechanisms <list>`, then the named parts or all of them:
  `layout L0 CompiledObject size=32 shapes=1..1`, `slot 0 "x" OptionalDouble Inline offset=24 record=0 type=undefined|null|number` (`accessor`: it holds the pair of functions, and no value), `variable g0 Statics JSValue Plain`,
  `deferred g8 made-by f8 v11 NewRegExp mark=undefined at-declaration` (a variable whose object its first read makes: `DeferredVariable`),
  `shape 1 L0`, `unit f1:Body (JSValue, JSValue) -> (JSValue)`, `unit f1:GenericCallEntry -> f1:Body`,
  `unit f2:CoroutineBody (JSValue, JSValue) -> (JSValue) never-suspends` (an async function in which nothing suspends),
  behind the forms of a unit: ` covered-by-callers` (a unit that walks a body and makes no stack test), ` saves-registers-of-callers`, ` reserves=<stack words>`, ` ends-its-tail-calls` (of a Body),
  `class L2 lazy-members prototype-first class-object-on-mutation guards-hold-while-clear members=[ByClassRecord, AtDefinition]` (the members in the order of `Facts::membersOf()`; no line for a class of which the plan says nothing).

**Walk and Code** (`AOTWalkTestCommands.cpp`)

- `walk [unit..]` prints `mechanisms <list>`, then the Code of each unit behind the walk and the sweep, in the order of the words. With
  `Arrivals` the whole stage runs once in front of the first unit, so a callee takes what all its callers have left (`walk/AOTArrivals.h`), as
  in a build, whichever units are named. The Machine commands do the same. Where a unit fails the line is `the walk of f1:Body failed`, and no
  unit follows.

The other three take the section `code` and no argument.

- `code-round-trip` takes the section through the parser and the printer. It prints the text, then `round trip: same` or
  `round trip: differs at line <n>`.
- `verify-code` prints `verified`, or the kind of the stop and the first rule of Code that the section breaks.
- `sweep-code` sweeps the Code and prints it, then `# visits of operations: <n>`.

**Machine** (`AOTMachineTestCommands.cpp`). `machine/AOTMachinePrinter.h` shows the texts. They hold no address. These commands get the
stand-in library, so they see one more module and one more function than the text has. Each unit starts with the line
`unit f<n>:<Role>`. Where a stage fails, the line is `the build failed`. Where the back end refused the procedure, the procedure follows, with
the order, the liveness and the allocation as far as they were made, and the line `failure: <kind> at @<n>`.

- `emit [unit..]` prints the operations as the emitters appended them. `emit-code` does so for the section `code`, swept first.
  The plan has to have the unit that the text names.
- `order`, `liveness-of-operations`, `allocate` and `frame` print the prepared procedure after that step, with the allocation checked.
- `bytes [unit..]` prints the size of the code, entries, relocations by kind and by the name of the symbol, return sites, handlers, faulting
  accesses, poll sites, the frame description, and `hash <hex>` of the code with the relocated fields zeroed. The heading also has the module
  path, the position and the name: `unit f3:Body "src/a.ts":120 "handle"`. Below it, everything is spelled by name and never by number, so two
  programs compare line by line. The top of `AOTMachineTestCommands.cpp` has the lines.
- `bytes-by-form` prints `<operation> <state> <place> <Open|Closed|Runtime> <sites> <bytes> <bytes a site>`, summed over every unit that the
  build compiles. The form, the state of knowledge and the place are what the emitter said (`Output::noteForm()`), or what the schema names
  where it said nothing: `Always Anywhere` and the row's `form`. A site is one time that the operation was handed to its emitter and got
  code, so a constant has one for each use. The build counts these under `report=Flow` alone. The top of `AOTMachineTestCommands.cpp` has the
  other lines: what the schema does not name, what looks otherwise than was said, what is longer than a call, what a place other than a loop has
  picked, and the sums. `bytes-by-form says=Pow:Open` makes the emitter of that operation say that form whatever it writes: the control.
  `bytes-by-form Body` counts the units of that role alone, and adds `units <count>` and `inline frames <count>`.
- `named-forms <operation>..` takes no program. It prints what the schema names for each operation: `<operation> <state> <form>, in a loop
  <form>` for each state of knowledge that the row names, with `by the loop` behind it where the row lists `Loop`, which is what
  `CodeGenerator::takesOpenForm()` goes by, and `<operation> in <place> <form>` for each place that the row lists.
- `bytes-outside-operations` prints the rest of `all code`, under the same option and behind the same names of roles: what the allocator and
  the frame add, by what it is and what it stands in front of (`Reload Edge main <count> <bytes>`), and what the emitters append for the end of
  a block (`End Throw rare <count> <bytes>`).
- `ranges <unit>..` prints all the code of each unit range by range, under the same option: `<begin> <end> <main | rare>` and the operation with
  its value in `walk` (`GetSlot v7`), or what `bytes-outside-operations` names (`Move Patch`). Laid under the places of a profile's samples it
  says what the instructions that run were written for.

**Whole builds** (`AOTProgramTestCommands.cpp`). They get the stand-in library.

- `compile { statistic }` runs the whole build. It prints `diagnostic <kind>` for each diagnostic, `image <empty|not empty>`, and
  `statistic <name> <value>` for each `StatisticID` named as an argument.
- `run-in-shell { --<option> } { word }` runs the whole build, then the entry module in `jsc` from the folder of `testaot`. The leading words
  that begin with `--` are options of the shell. The other words are statements of JavaScript, joined by one space, which the shell runs first;
  `id` is the identity function. It prints what the shell printed, then `exited with <code>` or `crashed with <signal>`. Where the build makes no
  image, it prints `image empty` and runs nothing. Where the program does not have exactly one entry module, it prints
  `image without one entry module`.
- `run-in-shell` and the source. Where the file has a `-- source` block of one `.js` or `.mjs` file, the shell also runs that text itself, with
  the same options and statements. The last line is then `the source prints the same`. If the outputs differ, it prints `the source prints:`
  and the other output, and the run fails whatever its check lines say. For any other source block the last line is
  `the shell cannot run the source itself`.
- `image [section..]` prints the flags of the image (`flags ShowsWaysBetweenModuleObjects ..`: what the run time lets in and what it refuses), the sizes of its sections and the text form of the named ones. Of `ProgramTables` it prints
  `<table> <rows>` for each table that has rows, and `compiledRuntimeFunction <token> function=f<entry>` for each function of the library that
  the host finds compiled. Then what a heap snapshot names: `moduleInHeapSnapshots place=<place> module=m<module> variables=<how many>`, `namedVariable module=<place> name=s<string> offset=<in the statics>` (or
  `closureOf=f<entry>`), `closuresOfModule module=<place> first=<closure number> length=<how many>` and `namedCapture closure=<number> slot=<slot> name=s<string>`.
- `units-with-code [unit..]` compiles the whole program as a build does and prints which units got code: `unit f3:Body 184 bytes`,
  and `unit f5:Body has no code` for a named unit that the queue never came to. `unit f7:GenericCallEntry fused into f7:Body, 3 bytes` stands for a generic call entry that only
  moves words (`CallingConventions::hasFusedCallEntry()`): it is no unit, and its moves lie in front of the primary entry of that unit.
  The last lines count all units:
  `units <n>, bytes <n>`, `fused call entries <n>, bytes <n>` and
  `call entries <n>`.
- `same-bytes-alone [unit..]` compiles the whole program with all its threads. Then a fresh worker compiles each unit again, in a program that
  keeps the words of `Function::body` only for the functions that the unit's code mentions or inlines. `hasBody()` answers as before, since that a
  function has a body is the program's record of it, and a read of words that were taken faults. The fresh worker verifies Code where the whole
  build does not, and the other way round, so the run also shows that the verifier changes no byte. It compares the
  bytes, the relocations and the lists they index. It prints `f3:Body same: <n> bytes, <m> relocations` or `f3:Body differs: <what>`, and a
  difference fails the run. Behind a difference it looks for the cause by halving the bodies that it took away: `f3:Body depends on the body of
  f9 "lib.ts":120 "join", which its code neither mentions nor inlines` for each body that brings the same bytes back, or `f3:Body differs with
  every body in its place too`, which leaves the thread and the verifier. A compile that fails on the way ends the command with its error.
  Bun's `test/aot/compiler/` has the other half of the independence test, with determinism, linear cost and the budgets.

**Support** (`AOTSupportTestCommands.cpp`): `diagnostics`, `parallel-loops`, `build-context`, `hashed-index`. `crash` crashes. Only
`AOTChildProcessTests.cpp` runs it, to check that a crash fails one test and names the function being compiled. `crash without-memory` does what the engine does where
a heap gets no block, in the middle of a function: an error of the build and exit code 1 (`support/AOTSystemResources.h`); `without-memory-at-no-work`: on a thread that compiles nothing, a crash.

If you write a command:

- Add it to one provider of `FOR_EACH_AOT_TEST_COMMAND_PROVIDER` (`AOTTestHarness.h`). A provider is one source of `tests/`. A `TestCommand` is
  `{ name, run, ProgramOfCommand, synopsis }`. A name that is used twice stops `testaot` at its start.
- It prints text and decides nothing. It runs next to other commands, so it touches only its context.
- `context.stage()` takes the program and gives a `StagedBuild`: the stages of a build, each at its first request, out
  of the driver's own pieces, with the mechanisms of the run (`context.mechanisms`). Each accessor answers null once a stage has reported
  an error. The command then returns. A command on a whole build calls `compileWithMechanisms()`.
- For an argument it cannot use, it prints `bad argument <word>` and returns. It never crashes on an argument.
- It adds `TestSection::firstLine` to the line of an error in a section.
- It does not call `Diagnostics::finish`: the runner does, once per `RUN`. A command that needs the messages itself calls it, and then
  `reopenDiagnostics(build)` (`AOTTestRunner.h`).

## Options

`# OPTIONS:` takes option words. `ir/schema/compile-call.def` declares them, and `testaot list options` prints them. Every test file runs with
`check=VerifyEachStage,ValidateReferences`: checks that no byte of the output depends on.

`several-realms` is what `bun test --isolate` asks for: several globals of one VM may each run an instance of the image. In the shell
`createGlobalObject()` makes such a global, and its `runCompiledEntryPoint` runs the program there: `answers/tagged-maker-of-another-instance.ir`.
Without the word the image lets one global of a VM have an instance, and an import in a second one throws.

`erase-types` is a word of the front end. Bun's tests of whole programs run it.

**`testing=CheckProofsAtRunTime`: Link's proofs are held against what runs.** No option of a build: tests, `testaot` and health set it
(`--option=testing=CheckProofsAtRunTime` for a folder, `--aot-testing=CheckProofsAtRunTime` for `bun build`). The walk builds a `CheckProof` on a value, which
takes the value for one of which only `typeWithoutProof` is known and ends the process where it is not of `checkedType`: `AOT: f12 at 345: what a field holds is
not of the type that the compiler has proven of it. It is: string. This is a bug in Bun.` (the function that the operation is written in and the position there:
`testaot walk <program> f12 testing=CheckProofsAtRunTime` shows the `CheckProof` with its types).
- **A place that compiled code alone fills is checked WHERE A VALUE GOES INTO IT** (`Walker::checkProofsOfWhatIsStoredBy()`, asked by `build()`): there are fewer such operations than reads, and of most the
  walk proves on the spot that the value fits, so nothing is built (a large application: 415,000 checks at the reads, 64,000 so). An entry parameter, `this`: at a direct call against Link's word, and in the entries and
  the guard of the function, for whoever calls by value, against what the body goes by; a unit that holds its own entry by value (`holdsFusedCallEntry()`: a few moves of the emitter, no operation) asks for
  itself, of every caller . a variable: `PutModuleVariable`, but one that the run time may fill (`Plan::runTimeMayFill()`): there at the reads . a capture, a field of an environment: `NewClosure`, `NewObject`,
  `PutEnvironmentField` . a result: at the return (`endWithReturn()`), but of a function that ends in a call of something that nobody lists, or of such a function
  (`Plan::resultsPassNoReturn()`): there at the call . at a copy in line: the operands and the result against what Link says of the callee.
- **Every other place is checked WHERE THE VALUE COMES OUT:** `build()` asks the rule of the row a second time, by positions that believe of Link only what the checks above enforce (`PositionsOfWalk`,
  `believesTables`) and by operand types in the words of tests, and checks where the record claims more: what a read of a field or a read by name finds, what a container holds.
  THAT NO READ COMES IN FRONT OF THE FIRST STORE is believed of no place: a read of a variable or of a field of an environment that Link calls filled asks whether it is.
- The one callee among several closures: `checkProofOfOnlyCallee()`.
- **THAT A SLOT IS FILLED** (`checkProofThatSlotIsFilled()`), whoever said so: Link, a birth's mark, a tag taken for the layout, an order word that the walk remembers. In front of every `GetSlot` and `PutSlot` at
  level Proven whose object no passed `IsSlotFilled` of that key has narrowed stands that test, with a `CheckProof` on its answer: `what a field holds .. It is: nothing, undefined or null`. A read that counts on the declared type
  of a raw slot asks for a value that is no mark (`holdsDeclaredType`), and so does every read that gives a raw number or boolean: its type leaves no room for undefined and null, which pass every door into such a slot, and no test tells anything of a raw word.
- **THE HALF OF AN ACCESSOR THAT Link HAS PROVEN** (`callProvenAccessor()`): the access is the call, with no record that has the type. Under the switch `GetSlotGetter` | `GetSlotSetter` is built for its check alone.
- **THE HELD TYPE OF A SLOT** (`PlacedSlot::type`) is the word of the doors, and is not believed: the positions that `checkProof()` asks do not give it, so every typed read of a boxed slot is checked against it.
- **WHAT THE WALK ITSELF REMEMBERS** (`checkWhatIsRememberedOf()`, asked by `walkOperation()`): a read, or a test for a name, that was given a constant or an older value is walked once more without the answers
  of the state, and `StrictEq` holds what memory gives against what was remembered: `what memory holds where the compiler remembers another value ..`. It stops a false `fresh`, a forgotten effect of a callee,
  a store that was lost. The walk goes on with the state that it had, so one check does not end the beliefs that the next would try.
- **WHAT THE WALK REMEMBERS OF A CELL** (`checkFactsRememberedOfCell()`): not frozen, no field outside its slot, the layout's prototype, the order word. No read shows these, so `IsNotFrozen` | `HasNoFieldOutsideSlot`,
  `HasLayoutPrototype` and `HasKeyOrder` are built again, each with a `CheckProof`, in front of every `PutSlot`, `PutSlotOrAdd` and `GetSlotOrUndefined` at level Proven, and with `AreMembersUnchanged` in front of a method
  that is read behind looks.
- **A decision of the PLAN by Link's word has no record to stand behind.** Deferral takes "no read comes in front of the declaration" for leave to mark a variable from birth, and then no state is left that could
  show such a read. So under the switch every deferred variable is marked where it is declared (`deferredDefinitionsOfPlan()`), and the read finds the empty word.
- The code of a check may mention only what the function of its unit leads to (`LinkTables::referrers`): of a closure or a namespace out of that reach it asks whether it is a function, or an object.
- **WHAT A TEST HAS LET PASS** (`checkProofOfRecord()`): a `Narrowed` is a new proof about an old value, whoever has built it: the walk behind a test of the program, a guess behind its own test, a case of a switch,
  a field that tells its holders apart. That it stands on the edge on which its test says so is its builder's word: where the arms of a guess are swapped, a `Narrowed [isOnTakenEdge]` stands on the edge that is not
  taken, and no check at a source sees that. So behind each stands a `CheckProof` that takes the value for what it was in front of the test: `what a test has let pass ..`. Not where the value lies raw: a bound of an integer.
- **WHAT A READ HANDS OVER IS A VALUE OF THE LANGUAGE, WHATEVER IT CLAIMS, `anything` INCLUDED** (`checkThatReadGivesValue()`, `CastEmitter::generateCheckProof()`): not the empty word, unless the type says so, and no cell
  that is no object, text, symbol or big integer: the pair of an accessor, an environment, a structure, a table of the run time. Each passes every test of the tag bits, and `anything` asks no other. The emitter asks it of
  every value that a `CheckProof` is given, and a read of a slot, a field, an element or a variable that claims nothing else gets a check that asks nothing else: `.. It is: a cell that is no value of the language`.
- Needs no check: what `held()` says (tested at each use), what a guard has tested, a raw word (whoever made it proved the form or
  trapped), changes of a layout (`crashOnImpossibleMutation()`), an entry that nobody may reach (it is closed).
- NOT CHECKED, because no test of a value shows it: the effects of a callee and what is fresh where no read is answered by them, that a place is written at construction alone, the key of an access that Link has proven
  (`keyAt()`), the orders of keys, and whatever of a type no test tells (`ProofsThatNoTestTells` counts the values).

The statistics `ProofsCheckedOf*` count the checks by source. The red control: `testing=UnseenCodeAnswersUndefined`, a false proof on purpose
(`answers/false-proofs-are-stopped-where-the-value-comes-in.ir`).

## From a program to running code

The first command is in Bun's repository.

```
bun build --compile --aot --aot-out=main.bin main.ts
testaot emit main.bin f12
testaot compile main.bin --output=main.image
jsc --aotImagePath=main.image -e "runCompiledEntryPoint(1)"
```

**Bring-up.** A Bun built from an earlier commit writes the IR of any program to a file too, and `testaot` feeds that file to this
engine. So an engine bug and a front-end bug can be told apart: what is wrong with the new front end's bytes and right with the earlier one's is the front
end's. Compare `testaot print` of both.

```
bun-from-the-tag build --compile --aot --aot-out=p.bin main.ts
testaot compile p.bin --output=p.image --program-of-tag
testaot emit p.bin f12 --program-of-tag
```

**The shell** has fourteen functions for compiled code:

- `runCompiledEntryPoint(id)` takes an id that `testaot compile` printed, runs that entry point and returns its promise. A
  rejection prints `Exception: ..` and exits with 3.
- `setRequireCacheEntry(id, entry)`, `deleteRequireCacheEntry(path)` and `overrideRequire(isOverridden)` tell the engine what a host tells it for
  `require.cache[path] = entry`, `delete require.cache[path]` and `Module.prototype.require = f`. `filesInRequireCache()` returns the paths of the
  files that have run, joined by commas: the keys of `require.cache` in a host that lists the files of the program there.
- `aotOperationCount(name)` returns how often the run-time operation of that name was called, and null in a run that does not count.
  `aotRuntimeCounters()` returns all the counts as text.
- `aotInstructionsOfThread()` returns how many instructions the thread has retired since the first call, and null where the system counts none: a growth test of a run asks before and behind.
- `lockCell(cell)` and `unlockCell(cell)` take and release the lock of a cell, as a marking thread does.
  `callWhileAnotherThreadLocksCell(cell, function)` calls the function while another thread takes and releases that lock again and again.
- `requestTerminationOnResolve(key)` requests termination when the loader resolves that module, as a watchdog thread would at that moment.
  `requestTerminationAndCall(function, ..)` and `requestTerminationAndConstruct(function, ..)` request it and then enter the function.
  `callAndCancelTermination(function)` does what a host does for a call with a time limit, and returns whether a termination was pending.

The shell has no host. A program that calls a function of Bun's compiles. When the call runs, the shell prints
`AOT: the program needs the host's <name>, and this realm has no host` and exits with 4.

The run time refuses what the build did not see, in the shell too: `eval` and `Function` with an image that admits no unknown JavaScript, a module
without an `entry` line with an image that loads no module by name, `setRequireCacheEntry` and `overrideRequire` with an image in which
nothing replaces what the loader answers. The option word `admit=Inspector` opens all of it. `AOTClosedWorldTests.cpp` has the cases.

## The allocation fuzzer

`testaot fuzz-allocation` runs `--count` seeds (1000), starting at `--seed` (1). For each it builds a random procedure of `--blocks` blocks (12):
reducible control flow, loops, rare arms, landing pads, patches with random constraints and clobbers, in both register banks. Every fourth
seed has handlers, and every eighth saves its frame on suspension. The procedure goes through the whole back end with every check of
`machine/AOTMachineChecker.h` on. Then it runs on three pairs of arguments, and its answers are compared with an interpreter's.

A seed that fails prints `FAIL seed <n>: <why>`, and the exit code is then 1. The last line is `<n> passed, <m> failed, 0 todo`. The same seed
gives the same procedure on every machine. `--show=<seed>` prints the procedure, the order, the liveness, the allocation and the frame of that
seed, and runs nothing else. A seed runs in `testaot` itself, so a crash ends the run. `seed <n>` is printed before each seed, so the last
line then names the seed that crashed.

`AOTMachineRandomTests.cpp` runs fixed seeds as native tests. Bun's `test/aot/engine/allocation-fuzzer.test.ts` runs the loop.
`AOTMachineCheckerTests.cpp` has the red controls: an allocation damaged on purpose is refused.

## Native tests

Native tests are for code that has no text form.

```cpp
struct NativeTest { ASCIILiteral name; void (*run)(); ASCIILiteral todo; };
AOT_TEST_CHECK(condition)
```

`AOT_TEST_CHECK` prints the file, the line and the condition, then crashes, in every build type.

A test that fails for a known reason says so where it is listed: `{ "slim-closure/put"_s, testPut, "<owner>: <what is lost>"_s }`. As under a `TODO`
line of a text test it is run all the same, prints `TODO <name>: <owner>: <what is lost>` with the check that still fails below it, and is counted
as todo. Once it passes it fails with `passes: remove its todo`. With `--single-process` the words count for nothing.

A file of native tests has one provider: a function that returns a `static constexpr` array of tests. To add one, add its name to
one of the three lists in `AOTNativeTest.h`, define it in the namespace that list is declared in, and add the file to
`JavaScriptCore_AOT_TEST_SOURCES` (`aot/AOT.cmake`). Whoever deletes such a file takes its provider out of the list, or `testaot` does not link.
A native test that needs an entry of Bun's table of built-ins is listed in `AOTNativeTestsOfHostEntries.inc`. `testaot` with the stand-in
host's table leaves it out and counts it.

Native tests run serially. With `--single-process` each name is printed before its test runs, so the last name printed is the
test that crashed. That is also how `native` learns from a child which test to report. A test that needs a process of its own (one image,
other JSC options) starts `testaot native --single-process <its name>` again. A test that runs an image in the shell starts `jsc` from the
folder of `testaot` (`AOTShellProcess.h`) and fails if it is not there.

| Header | For |
|---|---|
| `AOTTestHarness.h` | `createEmptyProgram()`, `hasElements()`, `StagedBuild` |
| `AOTRealmTestSupport.h` | a VM with a plain realm and a realm whose always-locked built-ins are immutable |
| `AOTDetachedInstanceTestSupport.h` | an `Instance` that no image stands behind, for an operation that asks it for the VM and the realm alone |
| `AOTLoadedProgramTestSupport.h` | a program in a VM: compiles IR text, starts `testaot` again with the image, runs the body there. `runWithLoadedProgram` evaluates no module, and the body runs no compiled code |
| `AOTShellProgramTestSupport.h`, `AOTPrivateNameProgramTestSupport.h` | a program that runs: compiles IR text, runs its entry module in `jsc` with the image and compares what it printed |
| `AOTNativeTestReasons.h` | a failure that says why in the one line of its check: the option words and the diagnostics where a text is no program or gets no image, the first line that differs and both sides where a process printed something else, how a process ended |
| `AOTCollectionTestSupport.h` | a collection that calls the collector's hooks in a VM without an image |
| `AOTImageTestSupport.h`, `AOTImageTestBodies.h`, `AOTImageSiteTestBodies.h`, `AOTImageBuildSteps.h` | sketches of compiled code, the steps of the image builder one at a time, an image to read back |
| `AOTStubTestSupport.h`, `AOTStubRunner.h` | every stub key, body and option variant; one stub run on zeroed memory in place of an instance, with calls that land in code that records its registers |
| `machine/AOTMachineTesting.h` | `TestProcedure`: operations built through `Output`, compiled and run in the test process, in the manner of `testb3`; `buildRandomProcedure()` and `interpret()` |
| `AOTMachineTestSupport.h` | on top of that: patches whose meaning is known and that poison what they may clobber, values that make pressure, compile-or-fail, the loop of the fuzzer |
| `AOTTemporaryFile.h` | the temporary file that a test hands to another process |

## What a good test looks like

- It tests one rule or one rewrite, at the stage that has it. A compiler bug gets a text test at its stage, not a whole program.
- Every positive case has a negative case next to it: the same shape with the one property changed that must stop the rewrite, checked with `-NOT`.
- It binds the ids of values that the compiler creates with `[[name]]`, because the number of a new value is not part of the contract.
- A checker has a red control. A checker that never fails proves nothing.
- Its first line says the rule in plain words. Its name says the case: `check-next.ir`.
- It is short. A subject with many cases is a folder.
