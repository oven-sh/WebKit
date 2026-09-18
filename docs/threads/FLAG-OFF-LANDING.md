# Landing with the flag off: what is known, and the plan

Written at the end of the eleventh session (2026-09-17), after DESIGN-PROPOSALS.md and PARITY-PLAN.md. Those two are about
what it takes to make the threaded configurations as good as `main`; PARITY-PLAN's measurements say that for GIL off this
is a long road with an uncertain end. This document is about the first thing on that road and the only one that gates
shipping: **the branch built, the flag not set, is `main`.** If that holds, the threads work can be merged and shipped in
the embedder's ordinary binary as an experimental feature that is off by default, and the threaded modes can mature
behind the flag for as long as they need. It is PARITY-PLAN's gate A, made into a plan of its own.

Part 2 collects everything the project knows about the flag-off configuration, with where each fact comes from; some of
it corrects earlier documents. Part 3 is the definition of done. Part 4 is the work in order. Part 5 is what the project
owner has to decide (ten decisions). Nothing in Parts 3 to 5 is implemented.

## Part 0. One page

**Where flag off stands.**
- *Speed.* 0.966 of `main` in this session's quiet pass (0.97 to 0.98 in the rounds'; target 0.99, never met), but only
  1.013 of `main`'s main-thread cycles over the same run. The score is not time: it weighs the first iteration and the four
  worst as much as the other 115, and flag off's cost sits there (first iteration 1.039; Startup 0.950, Worst Case 0.957,
  Average 0.978). It is C++, not generated code (+0.4 % over all tiers): locked array-profile updates at every tier-up check,
  locks taken unconditionally, call linking, stack sanitizing, array creation, the collector's marking and end-of-cycle
  walks. The interpreter alone is 4 to 5 % slower on every test, which JetStream barely sees and the embedder's short-lived
  programs would. Most of this is `main`'s form still present behind a gate and cheap to restore; with the designs already on
  record for the steady state (F-D4, E-D1 to E-D6) the estimate is 0.985 to 0.99.
- *Memory.* Start-up +1.4 MB; single tests +2 %; the suite in one process peaks 8 to 25 % higher in every pair of runs, and
  that is not explained. The shell's text is +4.6 MB (+18 %), +3.7 MB (+8.5 %) under LTO five rounds ago; the embedder's
  binary has never been measured.
- *Behaviour.* The seven JSC collections equal `main`'s but for five results that are not the branch's. test262:
  identical to `main` on all 102,475 records. `testmasm`, `testair`, `testb3`, `testdfg`, `testRegExp`: identical.
  `testapi` aborts on `main` and on the branch alike (upstream's atom-table assertion in the fork); what runs of it is
  identical. A Debug build of the stress suite, sampled (12,900 of 86,000 plans): **one flag-off regression, Debug
  builds only**, a one-line fix (2.3).
- *What runs flag off.* 658 source files changed, +87,000 lines. No build flag compiles the feature out. About 7,500
  changed lines in 1,638 existing functions of 473 files are outside every flag-on gate and run in every process, plus
  1,972 inserted gate tests; most are the same algorithm in another shape, a dozen families are deliberate or accidental
  differences (section 2.4); nobody has reviewed them as one list.
- *Platforms.* Built on Linux x86-64 only, ever. The arm64 build is broken in two places that are compiled flag off too.
  macOS, Windows and musl: unknown.
- *The embedder.* Three small hunks are needed to build Bun against the branch; twelve directories of its tests show no
  flag-off behaviour difference on a Debug build; nothing else has been run.
- *The flag.* An environment variable turns it on in any Bun built on this engine.

**What is corrected here.** The identity script the documents cite for "flag-off generated code equals `main`'s" compares
the branch with itself; that rule (SPEC-jit I1) has never been checked against `main`. Per-test flag-off ratios taken
from whole-process instruction counts overstate two tests (earley-boyer 1.15, stanford-crypto-pbkdf2 1.08): on the main
thread they are 1.009 and 1.024; the real residue is the RegExp and string tests, the JSON and code-load tests, then a one
to three percent band.

**The plan.** Rebase; build on every platform the embedder ships, first, because it is unknown and cheap to learn (L1); run
the suites that never ran (L2); generate a ledger of every hunk a flag-off process executes, shrink it by putting behind the
gate what does not need to be outside, argue the rest (L3); build the golden compare that was always assumed (L4); then
the speed, start-up, memory and size work, now ordered by what the first and the worst iterations execute and judged on
four cycle metrics and the score's three sub-scores, on the shipped LTO configuration (L5); a flag-off fuzz campaign
against the same campaign on `main` (L6, if the owner agrees); the flag's
exposure, its documentation, and two small flag-on fixes so that "experimental" does not mean "known memory-unsafe" (L7);
the pull request in reviewable slices (L8); the final battery (L9).

**What decides the schedule.** Not the performance work, which is designed and mechanical. The ledger: nobody has yet
read the whole of what a flag-off process executes as one list, and the merge should not happen before somebody has.

## Part 1. Scope: what lands, what ships, what does not

**What lands.** The whole of the threads work as it stands on the branch (658 source files, +87,000 lines, with its tests,
tools and documents), rebased on the commit the embedder pins, merged into the WebKit fork's `main`. There is no build
flag that compiles the feature out and this plan does not add one: the flag-on code is in every binary.

**What ships.** The embedder's ordinary binary, in which `useJSThreads` is false unless somebody sets it. With the flag
false: no `Thread`, `Lock`, `Condition` or `ThreadLocal` global, `Atomics` as on `main`, no new thread, no signal handler,
no per-thread structure allocated. With the flag set: the GIL-on mode, experimental; and, only with the second option
`useThreadGILOffUnsafe`, the GIL-off mode, experimental and named unsafe. On platforms where the flag is unsupported,
setting it is refused at option validation.

**What "equal to `main`" is measured against.** `main` at the rebase base, built with the same flags on the same machine in
the same session. Not a recorded number, not another machine, not an earlier week: two of this project's false alarms came
from exactly that.

**What this plan does not try to do.** Make the flag-on modes correct, fast or portable (PARITY-PLAN). Support the flag on
arm64, macOS or Windows. Put WebAssembly or FFI on spawned threads. It takes along two flag-on fixes (L7.3) and nothing else
of that kind.

**Where the risk is.** Not in the code behind the gates: a flag-off process does not run it, and the gates are frozen
bytes. It is in what is *not* behind a gate: about 7,500 changed lines in 1,600 existing functions that every process
executes (section 2.4), most of them the same algorithm in a different shape (a plain field that became a relaxed atomic,
an inserted test, a renamed accessor), a dozen or so deliberate differences, fifteen locks taken unconditionally, sixty
locked instructions `main` does not execute, and about twenty new release assertions. None of it is known to be wrong. None
of it has been reviewed as a whole either, and that review is what this plan is mostly about.

## Part 2. What is known about flag off

Marks: **measured** = measured in this session on the branch head's Release binary against the rebase base's, same
machine; **read** = checked in the current tree by reading; **recorded in X** = taken from that document and not
re-measured. "Flag off" = the branch built, `useJSThreads` not set. `main` = the rebase base.


Marks:  on the branch head's Release binary against the
rebase base's; **read** = checked in the current tree by reading; **recorded in X** = taken from that document, not
re-measured. "Flag off" = the branch built, `useJSThreads` not set. `main` = the rebase base `cf1b36ec8703`.

### 2.1 What flag off is, and how the flag is read and frozen

- **read** `useJSThreads` is an ordinary `Options` entry, default `false` (`runtime/OptionsList.h`). Its help string
  carries the security statement: enabling it is a high-resolution-timer capability grant; embedders must not enable it for
  semi-trusted or multi-tenant code in-process.
- **read** Everything else about the modes is derived from it in `Options::notifyOptionsChanged` (`runtime/Options.cpp`):
  with the flag on, `useVMLite`, `useSharedAtomStringTable` and (GIL off) the shared collector are forced; the derived
  `useTaggedButterflies` = `useJSThreads && (!useThreadGIL || !useJSThreadsSingleOwnerWithGIL || forceSegmentedButterflies ||
  forceButterflySWBit)`; `useJSThreadsWaitForJITPlans` is derived GIL off. Flag off all of these are false and no default
  of any pre-existing option changed (**recorded in** LANDING-PLAN, tenth round, "Changes to code that runs flag off":
  three new options, none read flag off; two derived ones always false flag off).
- **read** Generated code and C++ test two bytes of the frozen Config page: `g_jscConfig.gilOffProcess` (latched once from
  the finalized options, `runtime/JSCConfig.h`, a `call_once` that asserts `options.isFinalized`; the page is frozen by
  `WTF::Config::finalize()` right after, so a later store faults) and the `useTaggedButterflies` option byte (declared nine
  bytes after `useJSThreads`; **recorded in** LANDING-PLAN tenth round). Flag off both are 0. `Options.cpp` additionally keeps
  a process latch (`s_gilOffProcessLatch`) and asserts that the derivation never changes after it is set.
- **read** Can anything turn the flag on at run time? After `Options::finalize()` the options block is read-only memory; the
  only writers are `Options::setOption(s)` before finalization. The `jsc` shell reads `JSC_*` environment variables and
  `--option` arguments before finalization. `$vm` and the shell's test hooks exist only with `useDollarVM`.
- **read** Bun: `JSCInitialize` (`src/jsc/bindings/ZigGlobalObject.cpp`) calls `JSC::Config::enableRestrictedOptions()` and
  `JSC::Config::disableEnvironmentOptions()` (so `JSC_*` variables are ignored), then, inside the `JSC::initialize`
  callback and therefore before finalization, applies every environment variable that starts with `BUN_JSC_` through
  `Options::setOption`, crashing on an invalid one, then `assertOptionsAreCoherent()`. So **in any Bun built on this engine
  `BUN_JSC_useJSThreads=1` in the environment turns the flag on** (that is how every flag-on Bun lane of the rounds ran).
  There is no command-line flag, no `bunfig` key and no JavaScript API for it. `.env` files are loaded later and cannot set
  it (the help text in `src/jsc/lib.rs` says so). Whether an experimental feature should be reachable
  by an environment variable that a parent process controls is decision F1.
- **read** JavaScript-visible surface flag off: none. The `Thread`, `Lock`, `Condition`, `ThreadLocal` constructors are
  installed in `JSGlobalObject::init()` only under the flag (**recorded in** SPEC-api: "flag off => no own prop"); `Atomics`
  accepts only typed arrays flag off (the ordinary-object extension is flag-on, 34 suite results).
- **recorded in** SPEC-jit I1 / SPEC-objectmodel I22, E3: the rule is "flag off => emitted instruction sequences identical
  to the base modulo field-offset immediates moved by unconditional repacks; inline-cache object types and option defaults
  identical; the LLInt differs only by its gate branch; layouts and behaviour identical, flat tags zero".

### 2.2 Performance

#### Scores (the gate's measure)

| when, method | `main` | flag off | flag off / `main` | source |
|---|---|---|---|---|
| ninth round, quiet pass, medians of five (2026-09-11) | 353.5 | 339.8 | 0.961 | **recorded in** PERF-RESULTS 6.11 |
| tenth round, start of the round, same session as the final | - | 344.0 | 0.990 (with `main`'s slow earley-boyer) | PERF-RESULTS 6.12 |
| tenth round, final, quiet pass, medians of five | 347.3 (343.0-356.6) | 345.4 (343.0-348.1) | 0.994; **0.981 without earley-boyer** | PERF-RESULTS 6.12 |
| the pass an hour earlier (tree before the last) | 355.2 | 344.3 | 0.970; 0.966 without earley-boyer | PERF-RESULTS 6.12 |
| this session, single-test processes, best of two (not the gate's method) | - | - | 0.990 | **measured** |
| this session, quiet pass, full suite, interleaved, medians of five | 350.6 | 339.7 | 0.966; 0.969 without FlightPlanner, which is bimodal on every build (Startup 0.950, Worst Case 0.957, Average 0.978) | **measured** |

This session's pass, lowest rows: FlightPlanner 0.840 (bimodal), splay 0.914, octane-code-load 0.925, regexp 0.929,
stanford-crypto-pbkdf2 0.931, Air 0.936, delta-blue 0.947, OfflineAssembler 0.948, ML 0.950, richards 0.950, Basic 0.952;
two passes an hour apart (the first with some background load) gave 0.966 both times. Across the passes on record the
ratio has been anywhere from 0.966 to 0.994 with no change to flag-off code in between, which is why D7 asks for two
sessions and leans on main-thread cycles (1.011, stable to a few tenths of a percent).

`main`'s earley-boyer is bimodal (926 or 545); flag off did not fall into the slow mode in either tenth-round pass, which
is why the headline ratio moved between 0.970 and 0.994. The honest figure recorded by the round: **0.97 to 0.98 of `main`,
target 0.99 not met; the round gained about a point** (start 0.959-0.975). Lowest per-test rows of the final pass: regexp
0.925, json-stringify-inspector 0.939, Basic 0.942, Air 0.943, raytrace 0.943, UniPoker 0.947, ML 0.956,
json-parse-inspector 0.956, first-inspector-code-load 0.956, ai-astar 0.958, OfflineAssembler 0.962; no test below 0.80.


#### Instruction and cycle counts

| measure | flag off / `main` | source |
|---|---|---|
| whole-process instructions, 36 tests, start of the tenth round / final (geometric mean; sum) | 1.027 / 1.025 (sum 1.018) | PERF-RESULTS 6.12 |
| per-symbol sweep, samples, final tree's predecessor | +4,006 of 220,329 (+1.82 %) | DESIGN-PROPOSALS E, PERF-RESULTS 6.12 |
| whole-process instructions, three runs, minima | 1.018 | **measured** |
| main-thread instructions (`perf stat --no-inherit`), two runs, minima | 1.015 | **measured** |
| main-thread cycles | 1.011 | **measured** |
| locked loads per 1,000 main-thread instructions | 0.29 against 0.25 (0.3 % of `main`'s cycles at twenty each) | **measured** |

Per test, this session (**measured**; main-thread instructions, main-thread cycles, single-test score, whole-process
instructions, each flag off / `main`):

| test | mt instr | mt cycles | score | wp instr |
|---|---|---|---|---|
| OfflineAssembler | 1.092 | 1.025 | 0.981 | 1.078 |
| regexp | 1.057 | 1.044 | 0.964 | 1.053 |
| UniPoker | 1.044 | 1.044 | 0.934 | 1.043 |
| pdfjs | 1.036 | 1.018 | 1.003 | 1.018 |
| json-stringify-inspector | 1.035 | 1.017 | 1.058 | 1.035 |
| stanford-crypto-sha256 | 1.032 | 1.009 | 1.044 | 1.029 |
| typescript | 1.032 | 1.026 | 0.993 | 0.991 |
| Air | 1.031 | 1.037 | 0.939 | 1.026 |
| async-fs | 1.031 | 1.020 | 0.966 | 1.018 |
| WSL | 1.028 | 1.013 | 0.983 | 1.016 |
| stanford-crypto-aes | 1.026 | 1.029 | 1.025 | 1.015 |
| json-parse-inspector | 1.025 | 1.064 | 0.936 | 1.027 |
| splay | 1.025 | 1.003 | 1.003 | 1.015 |
| stanford-crypto-pbkdf2 | 1.024 | 1.025 | 0.975 | 1.077 |
| octane-code-load | 1.024 | 1.027 | 0.968 | 1.024 |
| Basic | 1.018 | 1.008 | 0.950 | 1.005 |
| ML | 1.017 | 1.032 | 0.961 | 1.017 |
| Babylon | 1.013 | 1.010 | 1.026 | 0.896 |
| first- / multi-inspector-code-load | 1.012 | 1.012 / 1.004 | 0.969 / 1.019 | 1.012 |
| earley-boyer | 1.009 | 1.014 | 1.002 | 1.147 |
| raytrace | 1.008 | 1.017 | 0.987 | 1.013 |
| the other fourteen | 0.99-1.004 | 0.99-1.015 | 0.97-1.03 | 0.99-1.015 |
| FlightPlanner (bimodal on every build) | 0.931 | 0.907 | 1.019 | 1.053 |

Two corrections these columns make to the recorded picture. earley-boyer's 1.147 (whole process) is not main-thread work
(1.009): it is `main`'s own bimodality picked up by the per-test minimum, or helper threads. pbkdf2's 1.077 likewise
(1.024 on the main thread). The main-thread residue is where sections E and F put it: the RegExp and string tests
(OfflineAssembler, regexp, UniPoker), the JSON and code-load tests, then a 1-3 % band.


#### Start-up and memory (**measured**; never measured before)

The `jsc` shell, Release, fifteen runs for instructions, peak resident set over five runs:

| script | `main` | flag off | |
|---|---|---|---|
| empty | 8.14 M instructions, 18.9 MB | 8.36 M, 20.4 MB | +2.7 %, +1.4 MB |
| a thousand-iteration loop and a print | 8.86 M, 20.1 MB | 9.11 M, 21.9 MB | +2.9 %, +1.9 MB |

Not decomposed. Candidates by reading (section 2.4): new globals and their constructors, 478 added members (18 on the VM,
89 on the heap, five structures on the global object), the larger text's page faults. The embedder's own start-up and
resident set have not been measured.

#### Where the score goes: the part that was not explained, examined (**measured**)

Three things did not add up in the figures above: main-thread cycles say 1.011 and the quiet pass says 0.966; the
sub-scores are Startup 0.950, Worst Case 0.957, Average 0.978; and the per-symbol sweep left +455 samples of generated
code and +170 of the parser without an owner. They were taken apart on an idle machine in the gate's own configuration
(the 36 tests in one process, main thread only, `perf stat --no-inherit` and `perf record --no-inherit`), `main` and flag off
interleaved.

**1. The time is small; the score is not time.** Whole suite, 120 iterations per test, three runs each:

| | `main` | flag off | ratio |
|---|---|---|---|
| main-thread instructions | 464.4 G | 471.1 G | 1.0145 |
| main-thread cycles | 145.7 G | 147.6 G | 1.0133 |
| main-thread CPU time | 40.9 s | 41.3 s | 1.010 |
| locked loads | 155 M | 172 M | 1.11 |
| total score of the same runs | 349.7 | 337.0 | 0.964 |

One percent of time, three and a half percent of score. JetStream's score of a test is the geometric mean of three
numbers: the first iteration, the mean of the four worst iterations, and the mean of all 120. Five iterations carry two
thirds of the weight. A cost that falls on the first iteration or on the iterations in which a collection or a tier-up
happens is multiplied; a cost spread evenly is not. Flag off's cost is of the first kind:

| what runs | flag off / `main`, main-thread cycles |
|---|---|
| the first iteration of every test (suite, one iteration each) | 1.039 (instructions 1.028; locked loads 1.29) |
| the first iteration, per test, geometric mean of 36 | 1.028 (instructions 1.018) |
| the first six iterations | 1.023 |
| all 120 | 1.013 |

Startup and Worst Case scores are whole milliseconds (a first iteration is 5 to 20 ms), so one extra millisecond is 5 to
10 % of a sub-score: the quantization is visible in the per-test ratios (0.750, 0.818, 0.857, 0.889, 0.900, 0.909 are
3/4, 9/11, 6/7, 8/9, 9/10, 10/11).

**2. By tier.** The same suite with the tiers capped (six iterations per test, two runs, minima):

| highest tier allowed | instructions | cycles |
|---|---|---|
| LLInt only (`--useJIT=0`) | 1.063 (per test, geometric mean: 1.052) | 1.057 (1.041) |
| Baseline (`--useDFGJIT=0`) | 1.014 | 1.010 |
| DFG (`--useFTLJIT=0`) | 1.015 | 1.025 |
| all | 1.015 | 1.023 |

**The interpreter is 4 to 5 % slower flag off on every test** (2 to 8 %, octane-zlib the one exception): the gate test in
front of every property and call opcode, the packed `get_by_id` metadata, the call-link record word (2.4). In JetStream
the interpreter is 2 % of the first iteration, so this is a tenth of a point there; in the embedder's real use (scripts
that run once, command-line tools, module loading) it is the tier that matters most, and nothing before this measured it.

**3. Generated code is not where it is.** Samples attributed by address to the code that was loaded there *at that time*
(the JIT dump has no unload records and executable memory is reused; an attribution that ignores time moves a quarter of
Baseline's samples to the FTL and invents a tier shift that does not exist: recorded here because the first attempt did
exactly that). Whole run, two profiles each: FTL +0.17 % of the run, DFG +0.09, Baseline +0.02, inline-cache stubs +0.04,
unnamed +0.10: **+0.4 % in all generated code together**, no tier receiving more or less time than on `main`. The sweep's
"+455 samples of generated code" is this, and is inside the noise of tier-up timing. The parser's "+170": parser, bytecode
generator and linking together are +0.12 % of the run and +0.12 % of the first iteration; the named part is a new accessor
(`UnlinkedCodeBlock::unlinkedBaselineCodeConcurrently`) and a hash lookup in the source-provider cache. Both items are
closed: they are small and they are not the score.

**4. It is C++, and it has names.** Difference flag off minus `main`, percent of `main`'s samples:

| family | whole run (+2.0 % in all) | first iteration (+2.3 % in all) |
|---|---|---|
| generated code, all tiers | +0.40 | +0.17 |
| profiling, Baseline and inline-cache compilation, linking, on the main thread | +0.14 | **+0.66** |
| object model and arrays in C++ | **+0.57** | +0.31 |
| collector and allocation | +0.26 | **+0.42** |
| libc and unclassified | +0.25 | +0.48 |
| calls, exceptions, entry (lock guards land here) | +0.03 | +0.20 |
| RegExp | +0.16 | -0.04 |
| parser, bytecode generator, linking | +0.12 | +0.12 |
| strings | +0.07 | -0.01 |
| LLInt | +0.01 | -0.02 |

First iteration, by symbol (samples per run, `main` -> flag off, of 57,800):
`ArrayProfile::computeUpdatedPrediction` 63 -> 206 and `CodeBlock::updateAllArrayProfilePredictions` 44 -> 182 (**half a
percent of the first iteration by themselves**: every tier-up check walks every array profile of the code block, and each
profile is now two locked read-modify-writes where `main` has a load, an `or` and a store; section 2.4.3 (C)5);
`ConcurrentJSLockerBase::~ConcurrentJSLockerBase` 0 -> 78 and `GCSafeConcurrentJSLocker`'s 0 -> 22 (the release of locks
taken unconditionally, out of line; (C)4); `sanitizeStackForVMImpl` 115 -> 174 (E-D4); `operationDefaultCall` 35 -> 91
(call linking: the record and the locked flag updates; (C)2); `Ref<AtomStringImpl>` and the string joiner's entry vector
0 -> 111 (reference-count shape); `JSArray::tryCreate` 583 -> 633 and `operationNewArrayWithSize`,
`tryAllocateCell<JSCellButterfly>` (E-D3); `UnlinkedCodeBlock::unlinkedBaselineCodeConcurrently` 0 -> 47; `Structure::Structure`
13 -> 42 (two more watchpoint sets to construct); `SharedJITStubSet::getSlowPathHandler` 2 -> 30 (its lock);
`JITStubRoutineSet::deleteUnmarkedJettisonedStubRoutines` and `reconcileWeakReferencesAtGCEndImpl` +55 (atomic reference
counts at the end of a collection; (C)3); `Heap::didAllocateBlock` 2 -> 21; `BlockDirectory::findBlockForAllocation` 78 ->
101. Whole run, the same names in smaller proportion, plus `operationRegExpExecNonGlobalOrSticky` 618 -> 720 (F-D4) and the
sweep (748 -> 790).

Locked loads in the first iteration alone: 33.0 M -> 42.5 M. Per test the extreme is octane-zlib: 0.45 M -> 5.5 M and +9 %
cycles in its first iteration, mandreel 5.65 M -> 6.83 M: the array-profile updates made from C++ slow paths while the code
is still below the optimizing tiers.

**5. Memory, not explained.** Start-up +1.4 MB (above). Single tests: peak resident set +2.2 % (geometric mean of 36;
+1.6 % in sum). The suite in one process: 1,801 / 2,007 / 2,164 / 1,918 MB on `main`, 2,198 / 2,163 / 2,716 / 2,223 MB flag
off in four pairs of runs: flag off higher in every pair, by 8 to 25 %, with a spread between runs as large as the
difference. Sampling the resident set through two runs of each: the curves are equal (within 30 MB) until splay, where flag
off grows 170 MB more, and regexp, another 90 MB; the gap then stays between 100 and 250 MB to the end. The collector's own
log of the same sequence shows the same number of collections (134 and 131) and the same heap sizes (680 MB at most on
both). So the difference is outside what the collector accounts for, or is memory it has released that the process has not:
not attributed. It matters more than the half point of score: the embedder's users see resident memory.

#### The residue by mechanism (**recorded in** DESIGN-PROPOSALS sections E and F unless marked)

One shape throughout: a hot leaf function gains one to twenty tests of a Config-page byte (`lea g_config; cmp byte; jcc`,
three or four instructions), each test's cold arm is inlined behind it, the function grows 1.2 to 4 times in bytes, the
register allocator spills around the cold arms, and callers that used to inline the leaf now call it. No single site is more
than 0.2 % of the suite. Binary-wide: 26,150 static tests of `gilOffProcess`, 4,164 of `useTaggedButterflies`; `lea` of the
Config base 34,831 times against 11,492.

| class (per-symbol sweep) | `main` | flag off | delta | share of the suite |
|---|---|---|---|---|
| arrays / objects / structures runtime (C++) | 11,055 | 12,056 | +1,001 | +0.45 % |
| strings (ropes, substrings, atom strings, joins) | 6,514 | 7,312 | +798 | +0.36 % |
| RegExp / Yarr entry points | 8,419 | 9,143 | +724 | +0.33 % |
| collector: marking | 7,664 | 8,350 | +686 | +0.31 % |
| generated code | 140,987 | 141,442 | +455 | +0.21 % (possibly tier-up timing; not explained) |
| libc / pthread / unclassified | 2,017 | 2,341 | +324 | +0.15 % |
| parser / bytecode generator | 13,500 | 13,670 | +170 | +0.08 % (not examined) |
| sweep, allocation slow paths, stack sanitizing | 2,795 | 2,891 | +96 | +0.04 % |
| interpreter glue, calls | 649 | 702 | +53 | +0.02 % |
| DFG / B3 / FTL compiler threads; malloc | | | -118; -118 | |

- **Marking, +6.4 % instructions per collection** (fixed live graph of 1.6 M cells: 385.7 M -> 410.3 M per full collection;
  the same with one marker thread): +13 instructions per visited cell. 5 from the visit counters and the mark stack's
  `m_top` made relaxed atomics (the compiler no longer merges the loads or folds `m_bytesVisited += size`; the comment in
  `GCSegmentedArray.h` that the code is identical is wrong); 8 from `visitButterflyImpl`: the mode flag captured by
  reference (closure frame 0x38 -> 0x58 bytes) and a switch reshaped by a GIL-off-only case (`main`'s one range test
  became a cascade of three `bt` tests). The helper-pause checkpoint contributes nothing. splay +4.4 %, hash-map +2.5 %,
  pdfjs +2 % of their instructions. Designs **E-D1** (one load per operation, plain accesses outside the sanitizer:
  about +260 of the +686) and **E-D2** (`visitButterflyImpl` instantiated per butterfly-word mode: about +400); together
  marking within 0.5 % of `main` per collection (410 M -> about 388 M; `main` 386 M).
- **RegExp and string entry points** (section F): per call on a microbenchmark that defeats folding, `main` -> flag off:
  `exec` 1,463 -> 1,536 (+5 %), `test` 905 -> 903, `match` 1,461 -> 1,536, `replace` 2,189 -> 2,430 (+11 %), regexp
  `split` 2,316 -> 2,647 (+14 %), `search` 959 -> 988 (+3 %), string `split` 1,252 -> 1,410 (+13 %), `slice` 121 -> 129.
  17 (`exec`) to 53 (regexp `split`) Config-byte tests per call, 23 of them `RETURN_IF_EXCEPTION` expansions in one split
  (the macro tests `gilOffProcess` ahead of `main`'s three instructions; it is written about 3,600 times). Helpers the
  compiler stopped inlining: `jsSubstring`, `JSObject::putDirectIndex`, the 8-bit `StringImpl` constructor,
  `RegExpObject::create`, and **the matching context's constructor** (`Yarr::MatchingContextHolder`: an out-of-line symbol of
  213 bytes against 114 on `main`, 47 call sites against 4, 81-109 samples in OfflineAssembler flag off, none on `main`; only
  the hot 8-bit leg of one operation has it inline: LANDING-PLAN's "inlines again" was corrected).
  `operationRegExpExecNonGlobalOrSticky` 6,970 bytes against 4,066. Worth: regexp 1.051, OfflineAssembler 1.076-1.096,
  UniPoker 1.04 in instructions; quiet-pass rows regexp 0.925, OfflineAssembler 0.962, UniPoker 0.947. Design **F-D4**
  (entry points instantiated per mode, `Main` = `main`'s bodies, selected per VM at no run-time cost; about +150 KB text):
  regexp -> about 1.010, OfflineAssembler -> about 1.021, every per-call row back to `main`'s count; **F-D5** (one gate per
  throw scope instead of per check) is the cross-cutting half; **F-D7** (split builds its array directly) is for all modes.
- **Array and object creation**: `JSArray::tryCreate` 109 -> 124 instructions per call, all from three inlined callees'
  gates (`optimalContiguousVectorLength` tests `useSharedGCHeap`, +6; `allocateCell` chooses the allocator, +4; the
  constructor tests `useTaggedButterflies`, +2; spills, +3); the same three are in every creation path
  (`operationNewArrayWithSize`, `tryAllocateCell<JSCellButterfly>`, `constructArray`, `fastSlice`, concat): about +350
  samples. Design **E-D3** (about twenty runtime functions instantiated per process mode; about +500 samples, +40 KB).
- **`array-int32-to-double-relabel` 1.18** (the one micro row above 1.06): not the conversion; per-operation gates in three
  runtime calls per iteration (`operationArrayPushDouble` 1,297 -> 3,100 bytes, `operationEnsureDouble` 76 -> 320, the
  temporary `WatchpointSet` 44 -> 163); +78 instructions per iteration of 582 below the FTL, about +20 in FTL steady state.
  Partly explained (per-call traces not obtained). E-D3.
- **`sanitizeStackForVMImpl`** 16 -> 20 instructions per call (a Config-byte test in the assembly routine), +85 samples.
  **E-D4** (the slot chosen in C++).
- **Strings outside RegExp**: `resolveRope` +160, `jsSubstringOfResolved` +143, `getPropertySlot<false>` +114 (4,694 ->
  7,128 bytes), `operationGetByValGaveUp` +80; static sizes only. `StringImpl::~StringImpl` +3 instructions on the
  atom-string path (the shared-table latch is a WTF global). E-D6 with F-D4.
- **LLInt**: affected opcodes carry 1-4 Config-byte tests each (`op_get_by_id` 119 -> 198 static instructions,
  `op_put_by_id` 158 -> 242, `op_put_by_val` 295 -> 357, `op_get_from_scope` 179 -> 251, `op_put_to_scope` 559 -> 715,
  `op_instanceof` 116 -> 200; `op_new_object` / `op_new_array` unchanged): two to three instructions per executed
  property or call opcode in the interpreter; below the sweep's threshold individually. No design.
- **Builds that do not inline (Bun's Debug+ASAN)**: `Options::x()` and `gilOffWithProcessGate()` are call chains ending in
  `addressOfJSCConfig()`: 461 `useJSThreads()` sites, 386 `useTaggedButterflies()`, 131 `gilOffWithProcessGate()`, 71
  `useSharedGCHeap()` (hot flag-off-reachable ones in `MarkedVector`, `CompleteSubspace`, `Heap.h`, `VM.h`). Bun's
  `fetch-tcp-stress` +16.9 % instructions, about 3 points of it the call chains (`addressOfJSCConfig` 3.81 % of samples
  against 2.48 %). **E-D5**.
- **arm64 only, by reading** (N9): five unconditional acquire loads on per-function / per-compilation paths
  (`FunctionExecutable::rareDataConcurrently`, the poly-proto watchpoint accessors, the lazy-state bit in `CodeBlock.h`,
  `UnlinkedCodeBlock`'s liveness pointer, `UnlinkedFunctionExecutable`'s deferred-state byte) are `ldar` there and plain
  `mov` on x86-64 where every sweep was taken. Not measured.
- **Ranking if all of it is done** (E): F-D4/F-D5 with E-D6 about 1,050 samples (0.48 %), E-D1 + E-D2 about 650 (0.30 %),
  E-D3 about 500 (0.23 %), E-D4 85: about +2,300 of the +4,006, leaving +0.8 % instructions; "about 0.99 of `main`,
  reachable, with no margin".
- **Micro set, final pass** (PERF-RESULTS 6.12, medians of five): all 26 rows within 1.00-1.06 of `main` except
  `array-int32-to-double-relabel` 1.18 and `megamorphic-access` 0.84 (faster); at 1.05-1.06: class-ctor-4,
  regexp-exec, megamorphic-put-transition, string-concat.
- **Found this session (measured)**: navier-stokes GIL on (not flag off) runs identical code in 1.28 of the cycles; flag
  off navier-stokes is 1.003 in cycles, so that item does not touch flag off.

### 2.3 Behaviour

- **JSC suites against `main`** (**recorded in** LANDING-PLAN tenth round, final battery; Release; collections: stress,
  microbenchmarks, mozilla, es6, modules, complex, ChakraCore): `main` 579 failures, flag off 581; **5 results fail that
  pass on `main`**: FFI `ftl-eager-no-cjit` executable-memory runs that fail on `main` alone too (the executable-allocation
  fuzzer fails one `JITCompilationCanFail` allocation in ten; four in, three out between runs; FO-10 / D-H1c) and
  `re-enter-resolve-rope-string.js`, whose watchdog loses against a rope that grows a gigabyte a second on `main` as well
  (3 of 9 single runs on `main` pass a 40 GB cap). Ninth round: 4 that pass on `main`, the same FFI class. No flag-off
  behaviour difference has been attributed to the branch in any round since the third.
- **Corpus** (`JSTests/threads`): the flag-off lanes 0 failures, Release and Debug (tenth round parity table).
- **Identity check** (**read**): `Tools/threads/v5a-identity.sh` runs every fiftieth file of `JSTests/stress` (40 files)
  with `--useJSThreads=false` and with no option **on the same binary** and compares exit code and output. It shows that an
  explicit `false` equals the default; it is NOT a comparison of generated code with `main`'s. Recorded as 40/40 in the
  pre-landing documents (MEGA-RUN-RESULTS, SCALEBENCH); not recorded in any landing round. The "golden disassembly diff flag
  off" that SPEC-jit's task list names (I1) has no tool in the tree; section E-7 says generated code "was not examined
  with disassembly dumps". So **I1 (generated code identical to `main` modulo field offsets) rests on the design and on
  the +0.2 % generated-code sample delta, not on a byte compare.**
- **Read hunk by hunk** (tenth round): of the round's source change, 376 sites are pure re-keys (`useJSThreads()` ->
  `useTaggedButterflies()`), 85 hunks are flag-on only, **73 run flag off**; the list with reason and impact is in
  LANDING-PLAN ("Changes to code that runs flag off"): rebase fix-ups converging on `main`; one remaining deviation (the three
  resolve-scope operations load the resolve type once, where `main` loads it again after `JSScope::resolve()`; differs only if
  a Proxy `has` trap re-enters the same `op_resolve_scope`, and then only in what gets cached); about twenty added
  predicted-false byte tests, none on a per-instruction path of generated code; an FTL phase that returns at its first test;
  two accesses with the same instruction and a different memory order (`setProtoLoadMode`, `RegExp::minimumSize`). Earlier
  rounds' lists are under "Flag-off changes" in each round's Results (second to sixth rounds).
- **Branch changes that touch flag-off code, from the upstream audit** (AUDIT-upstream-since-rebase section 6):
  `PropertyInlineCache.cpp` `resetStubAsJumpInAccess` has a `storeStoreFence()` on the flag-off path (`dmb ishst` on arm64);
  `VariableEnvironmentInlines.h` takes its map lock unconditionally; `ArrayBuffer.h` keeps `m_locked` as a separate atomic
  (layout differs from upstream flag off); `ObjectConstructorInlines.h` lost a redundant butterfly test. Three rebase
  losses that changed flag-off code were found and fixed (RB-1 `setMayStoreHole`, RB-2 entry-scope service checks: a
  wrong result flag off, RB-3 the `BadType` backoff gated on the flag).
- **A pre-existing defect reachable flag off on `main` too** (CVE-AUDIT-STATUS item 16): `Atomics.waitAsync` settle ->
  `DeferredWorkTimer::doWork` -> `drainMicrotasks` -> `gc()` uses a swept promise reaction (Debug assertion; Release
  use-after-free), reproduced with no options on the unmodified engine; recorded as a named close-out item, not a threads
  regression.
- **Layout / ABI flag off** (**read**): field offsets moved by unconditional repacks (I1's "modulo"); `TopExceptionScope`
  grew from 56 to 72 bytes; `RegExp::ovectorSpan` takes a `VM&`; `vm.jsonAtomStringCache` became
  `JSONAtomStringCache::live(vm)` with `VM&` arguments. An embedder must be rebuilt against the branch's headers (2.5).
- **Never run on the branch in any round** (what this session then ran is below): test262
  (`JSTests/test262` with `config.yaml` and expectations is in the tree); any JSC suite on a **Debug** build (the corpus ran
  Debug; the suites only Release); `testapi`, `testmasm`, `testb3`, `testair`; the wasm suite flag off as a collection of
  its own. The fuzzers have not run since June on any tree of the landing rounds. The seven validation sweeps of
  SCAN-RESULTS (June) were not re-run.

#### Lanes run for the first time in this session (**measured**, flag off against `main`, Linux x86-64)

"Branch" is the branch head built as in every round and run with no option; "`main`" is the rebase base built the same way.
Nothing in the tree was edited for these runs.

##### test262

Runner: the tree's `Tools/Scripts/test262-runner` (Python; its package auto-installer has no network here, so it was run with
`DISABLE_WEBKITCOREPY_AUTOINSTALLER=1` against the system's `urllib3` and `PyYAML`; it warns that it runs outside the
container SDK). Same `JSTests/test262` tree (the branch's), its `config.yaml` and `expectations-linux.yaml` for both binaries,
28 child processes each, both runs at the same time.

    DISABLE_WEBKITCOREPY_AUTOINSTALLER=1 Tools/Scripts/test262-runner -j <jsc> -t JSTests/test262 -p 28 --no-progress

| | `main` | branch, flag off |
|---|---|---|
| (file, mode) records | 102,475 | 102,475 |
| tests run | 101,989 | 101,989 |
| files skipped by `config.yaml` | 486 | 486 |
| passed as expected | 101,707 | 101,707 |
| failed as expected | 134 | 134 |
| failed, not in the expectations file | 140 | 140 |
| passed, expected to fail | 8 | 8 |
| wall time | 214 s | 218 s |

**Records whose outcome differs between the two binaries: 0 of 102,475.** Compared field by field (result, error text,
captured output; only the time field differs). No test had to be re-run alone.

The 140 unexpected failures and 8 unexpected passes are the same tests with the same messages on both binaries, so they
are the machine's, not the branch's: the 140 (73 files) are 64 under `language/identifiers` (identifier characters of a
Unicode version newer than the system ICU 70.1 knows), 64 under `intl402` (Temporal 28, Locale 20, NumberFormat 12,
DateTimeFormat 2, Intl 2), 5 `language/import`, 3 `language/expressions`, 4 `staging/sm`; the 8 unexpected passes are
`staging/sm/RegExp` 4, `TypedArray` 4. They would have to be re-baselined on the embedder's CI image before the lane can
gate anything; for the landing
question what matters is that the two columns are identical.

##### The test executables

They are declared only under `DEVELOPER_MODE`, which the embedder's configurations never set, so a second build directory
was configured for each tree with the Release configuration's cache values plus `-DDEVELOPER_MODE=ON
-DDEVELOPER_MODE_FATAL_WARNINGS=OFF`, and the six targets built (`testapi testmasm testb3 testair testdfg testRegExp`; no
source change was needed in either tree). `main` was built from a worktree of `cf1b36ec8703` outside the repository
(removed afterwards).

First finding, not the branch's: **the fork compiles the JIT disassembler out of optimized builds**
(`BUN_ENABLE_JIT_DISASSEMBLER` defaults to `ASSERT_ENABLED` in `wtf/PlatformEnable.h`, unchanged by the branch). `testair` and
`testb3` contain tests that inspect disassembly; without it `testair` dies in `std::string(nullptr)` (`matchAll`,
`testElideSimpleMove`) and `testb3` segfaults in `checkDisassembly` (`testStoreRelAddLoadAcq64`). Both trees were therefore
rebuilt with `-DBUN_ENABLE_JIT_DISASSEMBLER=1`, and the table below is from those builds. (With the define absent the
branch behaved as described; `main` was not built that way.)

| executable | `main` | branch, flag off | output compared |
|---|---|---|---|
| `testmasm` | exit 0, "Completed 394 tests" | exit 0, "Completed 394 tests" | identical after sorting (tests run on a thread pool) |
| `testair` | exit 0, 276 `OK!` | exit 0, 276 `OK!` | identical after sorting |
| `testb3` | exit 0, 216,588 `OK!` | exit 0, 216,588 `OK!` | identical after sorting except twelve "That took N ms" lines |
| `testdfg` | exit 0 | exit 0 | identical |
| `testRegExp JSTests/regexp/RegExpTest.data` | exit 0, "698 tests passed" | exit 0, "698 tests passed" | identical |
| `testapi` (no argument) | **abort (exit 134) before printing anything** | **the same abort** | same stack |
| `testapi ""` (the C++ half only, every test) | 18 of 18 tests `OK!`, 1,220 `PASSED` lines, then the same abort at thread teardown | 18 of 18 `OK!`, 1,220 `PASSED`, same abort | identical except object addresses |

Second finding, not the branch's: **`testapi` cannot complete on the fork's `main`.** Its first act
(`testLaunchJSCFromNonMainThread`: a `std::thread` constructs and destroys an API context) dies in `VM::~VM` ->
`Heap::lastChanceToFinalize` -> `PropertyTable::~PropertyTable` -> `StringImpl::~StringImpl` -> `AtomStringImpl::remove` at
upstream's `RELEASE_ASSERT(wasRemoved, "The string being removed is an atom in the string table of an other thread!")`.
Three runs of three on `main`, identical stack on the branch (the assertion is in the legacy arm, which flag off takes). The C
half of `testapi` (`testapi.c`, the larger part, including `MultithreadedMultiVMExecutionTest` and
`VMManagerStopTheWorldTest`) therefore never runs on either tree. Until the fork's `main` is fixed (or the API tests are
declared unsupported for the fork) `testapi` cannot be a landing gate; what can be said today is that the part that runs
gives the same 18/18 on both trees. The branch changes one line under `API/` (`JSContextRef.cpp`).

None of these executables sets the threads flag; they run the engine with its defaults (`testapi` calls
`Config::configureForTesting`), which is the flag-off configuration.

##### One Debug pass, flag off

Binary: the branch head's Debug build with AddressSanitizer (`ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0`).
There is no Debug build of `main` on the machine, so the comparison is with the tenth round's Release flag-off run of the
same suite (579 failing plans there, the same 579 as `main`'s Release run).

    ruby Tools/Scripts/run-jsc-stress-tests -j <debug jsc> -o <outside the tree> -c 60 <sample of JSTests/stress>

`JSTests/stress` is 5,831 files and about 86,000 plans (file x run mode); on the Debug+ASAN binary the machine does about
100 to 300 plans a minute depending on which tests are in flight, so a full pass is about fourteen hours. Two samples instead:

Both samples were **cut short** and are partial; neither is a pass of the suite.

| sample | files | plans planned | plans started before the stop | failing plans |
|---|---|---|---|---|
| A: every fifth file of `JSTests/stress` (sorted; 1,166 files) | 600 of 1,166 reached | 17,481 | 8,624 (30 children; stopped because a full sample would have taken over two hours) | 74 |
| B: every twentieth file (292 files) | 318 (with the helper directories' files) | 4,361 | 4,294 (60 children; all but the last file, `has-own-property-name-cache-symbol-keys.js`, whose plans take minutes each on this build) | 1 |

The runner schedules plans in parallel, not alphabetically, so "how far" is a count, not a position: A reached files from
`0*` to `setter-inlining-...`, B from `0*` to `yield-out-of-generator.js`.

Every failing plan, classified (the tenth round's Release flag-off list has 579 failing plans in 43 files, the same on
`main`'s Release binary):

| failing plans | test | in the Release flag-off list (and `main`'s) | what it is |
|---|---|---|---|
| 17 (A) | `eval-func-decl-block-with-var-and-remove.js`, every mode | yes, 17 and 17 | pre-existing, not the branch |
| 1 (A) | `ffi-callbacks.js`, one mode | yes, 1 and 1 | pre-existing (executable-allocation fuzzer class) |
| 1 (A) | `buffer-accessor-jit-large.js.lockdown` | yes, 1 and 1 | pre-existing |
| 2 (A) | `ffi-conversion-errors.js`, `ffi-threadsafe-callback.js`, one mode each | no | "`bun:ffi` failed to allocate executable memory": the same executable-allocation fuzzer class (DESIGN-PROPOSALS D-H1c), in the run modes that inject allocation failures; not an assertion |
| 34 (A) | `iterator-prototype-every.js`, `get-prototype-of.js`, every mode | no | the tests compare exact error messages; a Debug build appends the offending source text ("... (near '...item of wrapper...')", "(evaluating 'v')"). A property of the build type, not of the branch; no assertion involved. Not verified on a `main` Debug build (none exists here) |
| 17 (A) | `error-instance.js`, every mode | no | "Maximum call stack size exceeded": the test's recursion depth does not fit the Debug+ASAN frame sizes. Build type, not the branch; same caveat |
| **2 (A) + 1 (B)** | `async-stack-trace-promise-all-basic.js` (`dfg-eager`, `ftl-eager`), `generator-yield-star.js` (`dfg-eager`) | no | **`ASSERTION FAILED: decontaminate()` in `StructureID::decode()` (`StructureID.h`): a flag-off Debug regression of the branch.** Below |

**The one real finding.** The assertion text is upstream's, not one of the branch's 1,065 added `ASSERT` lines, but the branch is
what reaches it. Stack (symbolized): `StructureID::decode` <- `JSCell::structure` <- `slowValidateCell` <- `validateCell` <-
`WriteBarrier::get` <- `CallLinkInfo::unlinkOrUpgradeImpl` (`bytecode/CallLinkInfo.cpp`) <-
`CodeBlock::unlinkOrUpgradeIncomingCalls` <-
`ScriptExecutable::installCode` <- `BaselineJITPlan::finalize` <- `JITWorklist::completeAllReadyPlansForVM` <- `llint_replace`.
In the monomorphic-upgrade arm the branch added

    publishRecord(vm, std::bit_cast<uintptr_t>(m_callee.get()), target, newCodeBlock);

`publishRecord` returns at once when the flag is off, but its argument is evaluated first, and in a build with assertions
`WriteBarrier::get()` validates the cell it returns. `m_callee` is a weak reference that the collector clears in
`visitWeak`; between the end of marking and that clearing it can name a dead, already swept cell (structure ID zero), which is
exactly what the collect-continuously run modes produce. `main` never reads `m_callee` on this path. Release builds only
copy the pointer bits and are unaffected. Rate: 3 plans in about 12,900; the failing plan of B re-run alone 12 times: 0
failures; re-run 240 times with 120 at once: 4 failures (1.7 %), all this assertion. Fix: `m_callee.unvalidatedGet()` (the bits
are only a comparand), or evaluate the argument under the flag test. It is the only call of `publishRecord` that reads
`m_callee` this way (the other four pass a callee the caller already holds, or a constant). The embedder's CI runs Debug
builds, so this one line is a flag-off landing item.

Nothing else in the 12,900 plans differs from what the Release lists and the build type explain, and no plan failed on one
of the branch's own added assertions.

##### The fuzz rig

**Checked, nothing installed.** The June rig is gone: no Swift toolchain anywhere on the machine (`swift` not on the
path, the documented toolchain directory absent), no Fuzzilli checkout, no `WebKitBuild/Fuzz`. Docker is installed and has
no images. `Tools/threads/fuzz/build-jsc-fuzz.sh` hard-codes compiler paths that no longer exist (the machine's compiler
is elsewhere). What is in the tree and sufficient to rebuild it: the build script (REPRL + AddressSanitizer +
`-fsanitize-coverage=trace-pc-guard`, `-DENABLE_FUZZILLI=ON`, into `WebKitBuild/Fuzz` only), the `jscthreads` profile
(`JSCThreadsProfile.swift`) and its registration patch for a fresh Fuzzilli clone, `run-fuzzilli.sh`, the triage scripts,
`FUZZ.md` (toolchain version, build line, the `ASAN_OPTIONS` pin).

A **flag-off** campaign does not use the `jscthreads` profile (it passes `--useJSThreads=true` and the threaded-tier
switches): it is upstream Fuzzilli's stock `jsc` profile against the same instrumented binary, which is exactly what
`main` would be fuzzed with, so the two trees can be fuzzed side by side with one corpus:

    FuzzilliCli --profile=jsc --storagePath=<dir> --resume --timeout=1000 --jobs=<N> WebKitBuild/Fuzz/bin/jsc

Needed first: a Swift 6 toolchain (swift.org's Linux package or the official container image), a clone of
google/fuzzilli built with `swift build -c release`, the build script's compiler paths corrected, one instrumented build of
each tree (about the cost of a Debug build each).

### 2.4 What a flag-off process executes that `main` does not

An audit, made for this document, of `git diff <base> <head> -- Source/JavaScriptCore Source/WTF Source/bmalloc`: the
branch's whole change to the engine. Marks: **measured** = counted by a script over the diff or the head tree; **read** =
established by reading the code named; **heuristic** = produced by the classifier of 2.4.0 and only sample-verified. The
lists behind the counts are regenerated by L3.1's script; they are not in the tree yet.

#### 2.4.0 Method and its precision

A script splits the diff into hunks, takes the head version of every changed C/C++ file, strips comments and strings,
tracks brace scopes, and gives every added line one class: comment/blank; inside a block whose header tests a flag-on
gate positively, or after an early return on the negated gate (**gated**); inside the negated arm or the `else` of a
gate (**flag-off arm**); whitespace-identical to a line the same file removes (**moved**); in a new file that is flag-on
only by construction; outside any function (**declaration**); everything else (**ungated**). Gate spellings are those of
section 2 plus, per file, local `bool`s initialized from a gate. A second pass finds each ungated line's enclosing
function and whether that function is itself new.

Precision, by hand on random samples (**read**):
- 30 lines classified gated: 27 are flag-on only; 3 are harmless either way (a `static_assert`; two lines after an early
  return that is always taken flag off). None executes flag off. The gated class can be trusted.
- 30 lines classified ungated inside pre-existing functions (`heap/Heap.cpp` excluded, see below): 29 do execute in a
  flag-off process, 1 sits inside a flag-on arm the scope tracker lost. So the ungated class over-counts by a few
  percent, not by a factor. Of the 29: 25 are shape changes (an inserted gate test, a renamed accessor, a relaxed-atomic
  load or store where `main` has a plain one, a reworded assertion, a call to a function that returns at once flag off),
  3 are data-structure changes that behave identically with one mutator (compare-and-swap publication of a lazily created
  cache, a walk over a table that is empty flag off, a relaxed-atomic store added beside an existing store), 1 is an
  added debug assertion.
- The scope tracker is defeated by `heap/Heap.cpp` (a macro in the constructor's initializer list) and by the one-class
  body of `ftl/FTLLowerDFGToB3.cpp`: function attribution in those two files is wrong, line classes are still right.
  New functions whose names do not say what they are for (650 of 954 new functions) are counted ungated although most
  are reached only from gated call sites; that is the largest source of over-count and is why section 3 works from
  pre-existing functions.

#### 2.4.1 Size (**measured**)

658 files changed, +87,069 / -6,320 lines; 42 files added, 2 created as copies, 614 modified. `Source/bmalloc`
(libpas included) is untouched. No build flag compiles the feature out: there is no `ENABLE(JS_THREADS)`; every
difference between flag off and `main` is a run-time test or an unconditional change.

| directory | files | added | removed |
|---|---|---|---|
| JavaScriptCore/runtime | 264 | 48,357 | 3,076 |
| JavaScriptCore/heap | 83 | 12,351 | 504 |
| JavaScriptCore/bytecode | 64 | 7,830 | 875 |
| JavaScriptCore/dfg | 57 | 5,477 | 437 |
| JavaScriptCore/jit | 36 | 3,364 | 306 |
| JavaScriptCore/ftl | 20 | 3,151 | 370 |
| JavaScriptCore/llint | 6 | 1,889 | 121 |
| WTF/wtf | 32 | 1,690 | 196 |
| JavaScriptCore/interpreter | 19 | 974 | 193 |
| the rest (tools, debugger, parser, wasm, assembler, builtins, yarr, inspector, bytecompiler, ffi, offlineasm, domjit, lol, API, b3, top level) | 77 | 1,986 | 242 |

Added lines by class: 42,998 comment or blank (49 %); 44,071 code, of which 8,911 gated, 7,201 in new flag-on-only
files, 393 flag-off arm, 1,687 moved, 6,752 declarations, 19,127 ungated (**heuristic**). The ungated 19,127 split:
8,640 in 954 new functions (4,061 of them in functions whose names say flag on), 9,898 in 2,093 pre-existing functions
of 473 files, 787 in assembly, Ruby and JavaScript files, 195 unattributed.

Hunks: 3,989. Comment-only 200; removal-only 25; declarations-only 688; **entirely inside a flag-on gate (or new
flag-on file) 25, plus 9 that add only gated code and re-indented original code**; 3,019 contain at least one ungated
code line; 23 mixed declarations. The small number of wholly gated hunks is a property of how the change is written
(a gate test is inserted next to the code it guards, so almost every hunk carries the test line itself), not evidence
that little is gated.

In pre-existing functions the 9,898 ungated lines are: **1,972 inserted gate tests** (or mentions of the per-thread
structure, the mode-split accessor, a diagnostic counter or the race amplifier) in 273 files, about 440 trivial lines
(braces, `else`), and **7,484 other lines in 1,638 functions of 473 files**. That last number is the honest size of
"code a flag-off process runs that `main` does not": it is pervasive, and the argument that it is safe has to be made
by category (section 3), not by enumeration.

#### 2.4.2 The gates (**read**)

| spelling | defined | written | what flag off sees |
|---|---|---|---|
| `Options::useJSThreads()` | `runtime/OptionsList.h`, default false | options parsing; the options block lives on the Config page | false; 463 added uses. The LLInt tests the same byte (`ifJSThreadsBranch`, 17 sites in `LowLevelInterpreter64.asm`) |
| `Options::useTaggedButterflies()` | derived in `Options::notifyOptionsChanged` (`useJSThreads` and not the single-owner GIL-on case) | recomputed on every options change until finalization | false; 388 added uses; LLInt `ifTaggedButterfliesBranch` |
| `Options::useThreadGIL/useVMLite/useSharedAtomStringTable/useSharedGCHeap/useStructureAllocationLock` | `OptionsList.h`, all default false | `useJSThreads` forces the middle three and the last on; the GIL-off checklist may force `useThreadGIL` back on | all false |
| `g_jscConfig.gilOffProcess` | `runtime/JSCConfig.h` | once, `Config::latchGILOffProcess()` (`std::call_once`) from the first VM constructor and from `Config::finalize()`, before `WTF::Config::finalize()` freezes the page | 0; every LLInt "Group-3" site pays one not-taken byte test (`gilOffGroup3Check`, 13 + 16 sites) |
| `VM::gilOff()` / `VM::m_gilOff` | `runtime/VM.h:364`, plain `bool` member | VM constructor, from `isGILOffProcess()` | false; 574 added uses |
| `VM::gilOffWithProcessGate()`, `VM::group3Primitives()`, `VM::isGILOffProcess()` | `runtime/VM.h`, `VM.cpp` | read-only tests of the Config byte then of the member | VM-block storage, as `main` |
| `Heap::isSharedServer()` | `heap/Heap.h:688`, `std::atomic<bool> m_isSharedServer` (ordinary memory, not the Config page) | only by the flip a second GC client performs, which requires `useSharedGCHeap` | false; one relaxed load per test (236 added uses; `MutatorSlowPathLocker` is this test plus a conditional lock) |
| `VMLite::current()/currentIfExists()`, `lite` pointers | `runtime/VMLite.h`; thread-local | `useVMLite` | no lite exists; the pointer is null |
| template parameter `jsThreads`, `Structure`'s `ConcurrentCtorMember<>`, `RacyArrayBufferViewField<>`, `ICRacyCell<>`, `RelaxedAtomic*` wrappers | headers named in section 3(D) | compile time | same size and layout as the plain field; accesses are relaxed atomics (a plain `mov` on x86-64 and arm64) |
| `g_wtfConfig.useAtomicDeferrableRefCount`, `WTF::g_sharedAtomStringTableEnabled` | `WTFConfig.h`, `StringImpl.h` | the first from the latch above (Config page); the second a process global set when the shared table is enabled | false |
| `Options::validateFreeListStructure`, `verifyConcurrentButterfly`, `validateButterflyTagDiscipline`, `forceSegmentedButterflies`, `forceButterflySWBit`, `randomYieldPeriod` (race amplifier), `countJSThreadsCounters/reportJSThreadsCounters` | `OptionsList.h`, all default off | options parsing | off; the amplifier and the counters cost one predicted-false byte test where they are compiled in |
| preprocessor | none for the feature; `TSAN_ENABLED` (58 added `#if`), `ASSERT_ENABLED` (71), `CPU(...)`/`OS(LINUX)` for the thread-local slot | build | - |

No existing option's default changed (the `OptionsList.h` diff is additions only). `Options::useHandlerICInFTL` is
forced false exactly as on `main` unless `useJSThreadsUnlockHandlerICInFTL` (new, default false) or the flag is set.

**After initialization nothing can turn the flag on.** Options are finalized (`Options::finalize`, which asserts
coherence and sets `isFinalized`); the Config page holding the options block and the `gilOffProcess` byte is then made
read-only (`WTF::Config::finalize`); `notifyOptionsChanged` additionally latches the GIL-off derivation and
fail-stops if a later call would flip it. `Options::setOption` exists for the shell's command line and runs before
finalization; `$vm` has no option setter. `Config::disableFreezingForTesting()` exists upstream and is refused once the
page is frozen. **What would have to go wrong for flag-on code to run in a flag-off process:** (a) a write to the frozen
Config page (faults); (b) corruption of ordinary memory holding `VM::m_gilOff` (a plain member) or
`Heap::m_isSharedServer` (an atomic member): both are read on hot paths and neither is on the Config page, so a wild
write to either switches a subset of paths to the flag-on protocols while the Config-byte-gated paths stay flag off;
that is a consequence of memory corruption `main` also does not survive, but it is a new kind of mixed state and an
argument for deriving those two from the Config byte in release builds; (c) an embedder that sets the option: Bun
reads engine options only through its own `BUN_JSC_*` environment prefix, before initialization.

#### 2.4.3 What flag off sees, by class

##### (A) Code only behind a gate
16,112 added code lines (8,911 gated + 7,201 in new flag-on-only files), plus most of the 8,640 lines of new functions
(**heuristic**; sample precision in section 0). Nothing to argue flag off beyond the gates of section 2.

##### (B) Same algorithm, different shape (**read**, families)
1. **Inserted gate tests**: 1,972 lines in 273 files: `if (Options::useJSThreads()) [[unlikely]] {...}`,
   `std::optional<Locker<...>> threadsLocker; if (gate) threadsLocker.emplace(...)` (35 sites: `CallLinkInfo.cpp`,
   `DFGOperations.cpp`, `JITThunks.cpp`, `JSArray.cpp`, `JSObject.cpp`, `JSGlobalObject.cpp`, `Interpreter.cpp`,
   `UnlinkedMetadataTableInlines.h`, ...), `MutatorSlowPathLocker` (12 sites in `heap/`), mode-split accessors
   (`vm.group3Primitives().x` for `vm.x`, `vm.trapsForCurrentThread()` for `vm.traps()`, `currentThreadEntryScope()`).
   Cost: the design session measured them as instruction-count residue and code-size growth (text 35.26 MB against
   30.50 MB); safety: each test reads a frozen byte or a member written once.
2. **Plain fields that became relaxed atomics of the same size**: `Structure` (lock, transition table, watchpoint sets,
   property hash, seen-properties behind `ConcurrentCtorMember<>`; readers `concurrentRelaxedLoad`), `StructureRareData`
   (enumerator word, caches), `CodeBlock` (`m_jitCode`, `m_shouldAlwaysBeInlined`, capability level, `m_isJettisoned`,
   `m_jitData` loads), `CallLinkInfo` (`m_flags`, `m_record`), `WatchpointSet`/`InlineWatchpointSet` (`m_state`,
   `m_setIsNotEmpty`, `m_data`), `PropertyInlineCache` (`ICRacyCell<>`, packed self word), `JSArrayBufferView`
   (`RacyArrayBufferViewField<>` for length, offset, mode), `JSDataView::m_buffer`, `ArrayBuffer` (`m_pinCount`, `m_locked`),
   `InferredValue`, `ValueProfile`/`ArrayProfile`/`ArithProfile` buckets, `ObjectAllocationProfile`, `FunctionExecutable`/
   `ScriptExecutable`/`UnlinkedFunctionExecutable` feature bytes, `MicrotaskQueue` flags, `VMTraps`, `JSLock`
   (`m_ownerThreadPtr` stored beside `m_ownerThread`), `Heap` counters (`m_barriersExecuted`, `m_worldIsStopped`,
   bytes-allocated-this-cycle, `MarkedSpace::m_capacity`), the marker (`visitCount`, the mark stack's `m_top`:
   the design session's +6.4 % per collection), `IsoCellSet` bit reads, `WTF::StringImpl::m_hashAndFlags`/`m_length`/
   `m_refCount` initialization, `WTF::RecursiveLockAdapter::m_ownerThreadUID`, `WTF::WeakRandom` state, `WTF::Thread`
   bytes, `WTF::BitSet`/`FastBitVector`/`ConcurrentVector`/`ConcurrentBuffer`/`CompactPointerTuple`/`CagedPtr` accessors.
   564 ungated relaxed-atomic lines in pre-existing functions. On x86-64 and arm64 a relaxed
   load or store is the plain instruction; what changes is what the compiler may merge or hoist.
3. **Functions split or templated per mode**: `flattenDictionaryStructure` -> `...Impl` that may return null (flag off
   it never does; asserted), `JSObject::ensureArrayStorageSlow` and the relabel drivers (`switch` arms that test
   `useTaggedButterflies()`), `visitButterflyImpl` (mode flag captured by reference), `LazyProperty::callFunc` (the
   pre-existing contract verbatim behind `!VM::isGILOffProcess()`), `RegExp` match entry points (`MatchingContextHolder`
   constructor out of line, 213 bytes against 114), `JSArray::tryCreate` (109 -> 124 instructions), `AllocatingScope`/
   `RunningScope`/`SweepingScope`/`CollectingScope` (through `mutatorStateSlot()`).
4. **Calls that return at once flag off**: `Structure::runDeferredFlattenGILOff(vm)` after every inline-cache repatch,
   `traceEntryFrameIfGILOff`, `GILOffCompilationLocker(vm, gate)` in `UnlinkedFunctionExecutable::unlinkedCodeBlockFor`
   and the builtin-executable getters, `JSThreadsCounters::enabled()` tests, `RaceAmplifier::perturb()` sites (25; one
   byte test each), `JITWorklist::iterateCodeBlocksForGC`'s walk of `m_finalizingPlans` (empty flag off).

##### (C) Flag-off behaviour that differs from `main` on purpose (**read**; every one found, not proven complete)
1. **LLInt metadata repacked unconditionally.** `get_by_id_direct` and `try_get_by_id` caches are one 8-byte
   `LLIntCachedIdAndOffset` word instead of two fields, which moves them to the 8-aligned metadata region
   (`BytecodeList.rb`, `GetByIdMetadata.h`); the LLInt assembly and `LLIntSlowPaths.cpp` read and write the packed form in
   every mode. `PropertyInlineCache` likewise repacks nine advisory flags and `m_icType` and adds a packed
   `{offset, structureID}` self word ("footprint cost, paid flag-off too").
2. **`CallLinkInfo` carries a record pointer in every mode**: +8 bytes per call opcode's metadata; `m_flags` are
   updated by `lock or`/`lock and` (`exchangeOr`/`exchangeAnd`) on the linking slow path.
3. **Reference counts that became atomic for everybody**: `JITStubRoutine` (and subclasses), `InlineCacheHandler`.
   `GCAwareJITStubRoutine::addOwner/removeOwner` take `m_ownersLock` when the routine is in the shared stub set.
4. **Locks taken unconditionally** (uncontended with one mutator): `SharedJITStubSet::m_lock` (every accessor),
   `g_watchpointMembershipLock` in `AdaptiveInferredPropertyValueWatchpointBase::install`, `BlockDirectory::m_localAllocatorsLock`
   at collection time ("taken unconditionally"), `CompactTDZEnvironmentMap::m_lock`, `IntlCache::m_lock`,
   `JSGlobalObject::m_installedWatchpointsLock`, `CodeBlock::m_lock` in `DFG::Plan::finalize` and
   `DesiredWeakReferences::reallyAdd`, the cell lock in `AbstractModuleRecord` and `FunctionExecutable` paths,
   `ArrayBuffer` transfer/detach (`handle->lock()`, the pending-detach table), `SimpleTypedArrayController`
   (`m_wrapperRepublishLock`), `MicrotaskQueue::m_foreignTasksLock`, `SourceProvider::m_sourceCodeDumpLock`,
   `DeferredWorkTimer::m_taskLock` on three more paths, `LazyProperty`'s `tables.lock` (GIL-off arm only; listed because
   the classifier cannot see the early return). 159 lock lines among ungated lines; about 60 are conditional
   (`std::optional<Locker>`/`MutatorSlowPathLocker`), about 50 are in shared-collector functions of `Heap.cpp`, the rest
   are the list above.
5. **Locked read-modify-writes flag off executes and `main` does not** (61 lines): `ArrayProfile` (`atomicExchangeOr`
   into `m_observedArrayModes` at every C++ observation and at prediction update; `atomicExchange` of the last-seen
   structure), `CallLinkInfo::m_flags`, `JITStubRoutine` reference count, `MarkedSpace::m_capacity` per block and per
   precise allocation, `MarkedBlock::s_markedBlocksCreated`, `MarkingConstraint::m_lastVisitCount`,
   `VM::m_currentWeakRefVersion` (every job end), `VM::m_drainMicrotaskDelayScopeCount` (every scope),
   `ArrayBuffer::pin/unpin`, `SymbolImpl` hash counter, `SourceProvider::getID` (atomic counter then compare-and-swap),
   `DFGDriver` compilation counter, `AbstractMacroAssembler` random seed, and the compare-and-swap publications of item 6.
   The plan's main-thread measurement agrees in size: 0.29 locked loads per thousand instructions flag off against 0.25
   on `main` (**measured**, section 2.2). `ArithProfile` deliberately avoids the read-modify-write (its comment says
   why); `ArrayProfile` does not, and is the first candidate to bring back to a relaxed load, `or`, relaxed store.
6. **Lazily created members published by compare-and-swap in every mode** (the loser frees its copy):
   `Structure::allocateRareData`, `StructureRareData`'s special-property cache and cached-value slots, `InferredValue`'s fat
   set, `WatchpointSet` inflation (`InlineWatchpointSet::inflateSlow`), `GenericArgumentsImpl::m_modifiedArgumentsDescriptor`,
   the FFI context slot of `JSGlobalObject`, `BuiltinExecutables` (check-then-create under `GILOffCompilationLocker`, store
   after an optional fence).
7. **Tier-up bookkeeping**: `CodeBlock::tryBeginTierUp` (a `fetch_or` latch per edge, "unconditional: with a single
   mutator the compare-and-swap always wins"); `DFG::JITCode::setOSREntryBlock`/`clearOSREntryBlock` and
   `triggerFTLReplacementCompile` now `find` the trigger and **`RELEASE_ASSERT` it exists** where `main` does
   `tierUpEntryTriggers.set` (insert or update): a new fail-stop if a key is ever missing; padding of 64 bytes before
   and after the execution counters of `BaselineJITCode` and `DFG::JITCode` (false-sharing avoidance, all modes).
8. **`DFG::Plan` finalization roots** and `JITPlan` finalize-claim: "unconditional finalize-claim roots"; empty flag off.
9. **Generator and iterator-helper builtins** (`GeneratorPrototype.js`, `JSIteratorHelperPrototype.js`) branch on the new
   `@gilOffProcess` constant; the bytecode generator's `IfElseNode` and condition contexts now fold a bytecode-intrinsic
   constant and emit only the live arm (`NodesCodegen.cpp`), so flag-off bytecode keeps `main`'s arm. The folding is a
   generator change every builtin that tests a registry constant now goes through.
10. **Unrelated to threads**: `JSBigInt` single-digit fast paths for add, subtract and multiply
    (`createFromDigitInline`, 252 added lines, present since the first squash commit; not on `main`). It changes
    which code computes small BigInt results flag off. Either it lands on its own or it leaves this change.
11. **`Thread::create`** is `tryCreate` plus `RELEASE_ASSERT` (same fail-stop, new null-returning variant for the API);
    `Interpreter::...varargs` gained `RELEASE_ASSERT(newCallFrame < callFrame)`; `CallFrame::globalObjectOfClosestCodeBlock`-
    style paths assert the entry scope instead of dereferencing it; `FrameTracers.h` restores assert storage identity;
    `WTF::StringImpl::setNeverAtomize` is a compare-and-swap loop and returns whether it succeeded.
12. **WebAssembly and FFI**: refusals are flag-on only; `JSWebAssemblyInstance/Module` constructors assert
    `!useSharedGCHeap()`/`!vm.gilOff()` (true flag off).

##### (D) New data that exists flag off (**measured**: 478 member or global declarations in 115 headers)
- `JSCell`, `JSObject`, `Butterfly`, `IndexingHeader`, `JSString`: **no new members**; cell sizes and the offsets the
  JIT and LLInt use for them are unchanged. (`Butterfly.h` gains the spine and fragment types, flag-on only.)
- `Structure`: `m_transitionThreadLocalTID` (uint16), two more `InlineWatchpointSet`s (transition-thread-local,
  write-thread-local); wrappers on the rest. `StructureRareData`: a `Lock`. Structure size changes; `Structure` is a cell
  in its own iso subspace, so this is memory, not layout seen by generated code beyond the offsets the offlineasm
  extractor regenerates.
- `CodeBlock`: `m_exceptionHandlersLock`, tier-up latch and state bytes, a count of unmaterialized function
  executables; `BaselineJITCode`/`DFG::JITCode`: 128 bytes of padding each, `m_gilOffDFGForLoopEntry`, `m_tierUpTriggersLock`;
  `FTL::JITCode`: handler-IC data.
- `VM`: 18 members (termination exception and its lock, `m_gilOff`, epoch, `m_heapRandomLock`, the main `VMLite` pointer,
  atomics for termination/forbidden/tainted flags, `m_checkpointSideStateLock`, a thread-local `s_initializingObjectClass`).
  `Heap.h`: 89 members, almost all shared-collector state. `JSGlobalObject`: five structures for the API objects, two
  locks, `m_stackTraceLimitBits`. `ArrayBuffer`: wrapper-republish lock and weak impl pointer, atomics. `JSLock`: six.
  `MicrotaskQueue`: foreign-task lock and flag. `VMTraps`: owner fields, two atomics. `PropertyTable`: an edit counter, a
  deleted-offset count.
- WTF: `StringImpl::m_hashAndFlags` is `std::atomic<unsigned>` (same size, `static_assert`ed); `Thread` gains
  `m_didExit`/`m_isJSThread` as dedicated bytes and an atomic GC-thread-type byte; `g_sharedAtomStringTableEnabled`.
- New globals: `g_watchpointMembershipLock`, `CallLinkInfo::s_callLinkSerializationLock`, `MegamorphicCache::s_processEpoch`,
  `g_jscAnyJSThreadEverSpawned`, the JS-threads counters singleton. New thread-local slots: the butterfly TID tag and the
  current `VMLite` (ELF TLS on Linux, a pthread key on Darwin; `initializeButterflyTIDTagForCurrentThread()` is called
  only when the flag is on). **No new thread is started and no signal handler is installed flag off** (the stop
  watchdog, the conductor and the amplifier are flag-on or option-gated; **read** in `ThreadManager.cpp`,
  `JSThreadsSafepoint.cpp`, `RaceAmplifier.cpp`).

#### 2.4.4 Fail-stops a flag-off process can reach

**Release** (**measured** count, **heuristic** gating, then hand triage): 596 added `RELEASE_ASSERT*`/`CRASH` lines.
324 gated; 70 in new functions whose names say flag on; 13 moved; **189 heuristically ungated**, of which 48 reword an
assertion `main` already has (accessor renames), 16 assert that the flag is off (`RELEASE_ASSERT(!Options::useJSThreads())`
at sites reserved for flag off: true by construction), 4 are vacuous flag off (`!flag || ...`), 8 assert the flag is
on inside flag-on code, and **113 need reading**. Read so far (**read**): about 75 of the 113 are in flag-on-only code
the classifier could not see (the self-test and 128-bit compare-and-swap of `ConcurrentButterfly.h`, the spine accessors
of `Butterfly.h`, `ThreadManager.h`, `VMLite*.cpp`, the GIL-off arms of `JSLock.cpp`, the shared-collector functions of
`Heap.cpp`, per-thread scratch-buffer lookups in `DFGOSREntry.cpp`/`FTLOSREntry.cpp`, thread-local loads in the two
macro assemblers). **Reachable flag off, new relative to `main`:**

| site | condition | note |
|---|---|---|
| `dfg/DFGJITCode.cpp` `setOSREntryBlock`/`clearOSREntryBlock`, `dfg/DFGOperations.cpp` `triggerFTLReplacementCompile` loop | the tier-up trigger key exists | `main` inserts; see (C)7 |
| `interpreter/Interpreter.cpp` varargs frame set-up | `newCallFrame < callFrame` | replaces a `Vector` overflow crash the old decode gave for free |
| `interpreter/CallFrame.cpp:400` | the current thread has an entry scope | `main` dereferences `vm.entryScope` unchecked |
| `interpreter/FrameTracers.h:80,83` | exception and traps storage is the storage seen at construction | trivially the VM's flag off |
| `runtime/ArrayBuffer.h` `pin`/`unpin` | no overflow / underflow | preserves `Checked<unsigned>`'s crash |
| `runtime/ArrayBuffer.cpp` six sites | size within `MAX_ARRAY_BUFFER_SIZE` | re-statements of existing bounds on relaxed loads |
| `runtime/JSDataView.h:104` | `unsharedBuffer()` on an unshared buffer | as `main` |
| `runtime/Structure.cpp:1759,1992` | flatten did not bail | the bail is flag-on gated |
| `runtime/JSObject.cpp:4316`, `JSObject.h` six `RELEASE_ASSERT_NOT_REACHED` | default arms of new `switch`es over indexing shapes | unreachable by the shape filters before them; worth one reading |
| `parser/SourceProvider.cpp:79` | the source-ID counter did not wrap | - |
| `heap/AllocatingScope.h` and the three sibling scopes | mutator state is the expected one | `main` asserts the same in the same places (reworded through a slot accessor) |
| `heap/LocalAllocator.cpp:138,552` | free-list structural soundness | behind `validateFreeListStructure` (off) |
| `heap/HeapInlines.h:330` | a client heap is not standalone before `vm()` | standalone clients are flag-on |
| `ftl/FTLJITFinalizer.cpp:82` | no repatching inline cache in a handler-IC compilation | inside the handler-IC arm; flag off that arm is not taken |
| `ftl/FTLLocation.cpp` three sites | unknown location kind | copies of `main`'s assertions in a new restore helper |
| `bytecode/Watchpoint.cpp:413-414` | a queued fire was serviced | inside the flag-on class-A path |
| `runtime/JSLock.cpp:541,568,621,1692`, `runtime/VM.cpp:1316,3104`, `runtime/DisallowVMEntry.h`, `ExceptionScope`/`ThrowScope`/`TopExceptionScope` | lock depth, stack bounds, scope-verification storage | reworded or verification-build only |
| `WTF/Threading.cpp:315` | `Thread::create` succeeded | as `main`'s `establishHandle` assertion |

The complete candidate list with its triage column is produced by the generator of L3.1. Of the 24 emitted traps, the 18
LLInt `break`s (**read**) all follow a `VMLite::vm == vm` compare inside the GIL-off arm of a
Group-3 site, after the Config-byte test: unreachable flag off. The 6 `jit.breakpoint()` (OSR-exit thunks and the
per-lite arms of `AssemblyHelpers`/`LLIntThunks`) were not re-read here.

**Debug** (**measured**): 1,070 added `ASSERT*` lines; 295 gated, 200 in flag-on-named new functions, 32 moved,
**543 heuristically ungated**: 130 reword existing assertions, 3 vacuous, 52 assert the flag is on inside flag-on code,
**358 to read**. Per file: `heap/Heap.cpp` 54, `WTF/text/AtomStringImpl.cpp` 30,
`heap/MarkedSpace.cpp` 19, `ftl/FTLLowerDFGToB3.cpp` 15, `runtime/VMTraps.cpp` 11, `runtime/VM.cpp` 11, `jit/CCallHelpers.cpp` 11,
`runtime/Structure.cpp` 10, `runtime/JSBigInt.cpp` 10, `runtime/JSObject.{h,cpp}` 16, `runtime/Butterfly.h` 7, `heap/Heap.h` 7,
`heap/CompleteSubspace.cpp` 7, `runtime/JSLock.cpp` 6, `heap/BlockDirectory.cpp` 6, `dfg/DFGNode.h` 5,
`WTF/text/StringImpl.{h,cpp}` 7,
`ftl/FTLLocation.cpp` 4, `bytecode/CallLinkInfo.cpp` 4, the rest in ones and twos. Those in widely reached functions:
`MarkedSpace` (capacity accounting), `BlockDirectory` (lock-held assertions), `Structure` (table and pin state),
`JSObject.h` (butterfly flatness: `ASSERT(!isSegmentedButterfly(...))` on every `butterfly()` read in Debug),
`AtomStringImpl.cpp` (table-mode assertions on every add and remove), `VMTraps.cpp`. None was seen to fire flag off in the
rounds' Debug corpus runs, but the JSC stress suite has never been run on a Debug build of the branch, and Bun's CI is
Debug: those 358 are the flag-off regression surface of a Debug build.

#### 2.4.5 Locks, atomics and fences

Covered in 3(B)2, 3(C)3-6. Fences: 22 ungated lines, all `storeStoreFence`/`loadLoadFence` (compiler barriers on
x86-64, `dmb ishst`/`dmb ishld` on arm64) beside publications that exist in every mode: `ObjectAllocationProfile::clear`,
`Structure` rare-data and property-table publication, `CodeBlock` handler publication, `PropertyInlineCache::
publishHandlerChainHead`, `BuiltinExecutables` (behind `vm.gilOff()`), `JSObject` transition stores that were already
fenced when `mutatorShouldBeFenced`. 25 `seq_cst` lines among ungated ones (**read**): 17 are in shared-collector
functions of `Heap.cpp` (flag on), 4 are default-order
loads of the collector's park/resume hook pointers in `VMManager.cpp` (a plain load on x86-64), and two are stores a flag-off
process executes on cold paths: `RegExp::deleteCode` clears `m_publishedCodeGILOff` with a default-order store (an `xchg`), and
`ArrayBuffer` detach zeroes `m_sizeInBytes` with a `seq_cst` store. No `seq_cst` store was found on a flag-off hot path.

#### 2.4.6 Generated code

**There is no tool that compares flag-off generated code with `main`'s.** `Tools/threads/v5a-identity.sh`, cited in
the landing documents as the identity check, runs forty stress tests twice on the same binary, once with
`--useJSThreads=false` and once with no option, and compares exit code and output. It shows that the explicit `false`
equals the default; it says nothing about `main`, and it looks at no machine code in any tier. SPEC-objectmodel I22
("flag off, byte-identical") is a design rule that individual emitters follow and that comments cite; it has never been
checked mechanically. The Release build of the measurement machine has no disassembler (`--dumpDisassembly` prints
address ranges only), which is part of why.

What differs by reading: **LLInt**: every Group-3 site (VM entry and exit, exception unwinding, host-call return value,
stack checks: 29 sites) executes `leap _g_config; bbeq gilOffProcess, 0` before `main`'s access; every property fast path
of `LowLevelInterpreter64.asm` starts with `ifTaggedButterfliesBranch`; `get_by_id_direct`/`try_get_by_id` use the packed
cache (C1); call opcodes read the call-link record word. **Baseline and inline caches**: data-IC self accesses use the
packed word when the flag is on only; flag off the emitters take the `!useTaggedButterflies()` early return and emit
`main`'s sequence (`CCallHelpers::loadButterfly...`, `InlineCacheCompiler`), but handler bodies are compiled from code that
consults `Options` at emission, so identity is per emitter, not structural. **DFG/FTL**: 106 emitter functions carry
ungated emission lines; the ones read emit the same instructions flag off through a helper that tests the option at
emission. The design session measured flag-off generated code at +455 samples of 215,000 against `main` over JetStream:
small, not zero, and not attributed. New DFG node kinds (`GeneratorClaimResume`, `GeneratorPublishResume`,
`CheckTransitionOwner`, poll-visibility data) are only produced from bytecode or profiles that exist flag on.

#### 2.4.7 WTF and bmalloc

`Source/bmalloc`: no change. `Source/WTF`: 32 files, +1,690/-196; 464 ungated code lines. Unconditional (not tested
against any JSC flag): relaxed-atomic accessors in `Atomics.h` (new helpers), `BitSet.h`, `FastBitVector.h`, `CagedPtr.h`,
`CompactPointerTuple.h`, `ConcurrentBuffer.h`, `ConcurrentPtrHashSet.h`, `ConcurrentVector.h`, `SentinelLinkedList.h`
(two const-correctness lines), `SimpleStats.h` (relaxed atomics for the collector's statistics), `WeakRandom.h`
(state through `__atomic_*` builtins: GCC/Clang only, fine for clang-cl, not for MSVC), `RecursiveLockAdapter.h` (atomic
owner id), `WordLock.cpp` (the spin load is relaxed under TSan only), `MetaAllocator` (a lock-held assertion and an
accessor), `Threading.{h,cpp}`, `ThreadingPOSIX.cpp`, `ThreadingWin.cpp` (`tryCreate`; dedicated bytes written with
relaxed atomics; the thread's atom table pointer accessors), `StringImpl.h` (`m_hashAndFlags` atomic; every flag read
through `hashAndFlags()`, plain outside TSan; `setNeverAtomize` a compare-and-swap loop; cost reporting a plain store
unless the shared table is on), `StringImpl.cpp`, `SymbolImpl.cpp` (atomic hash counter), `SymbolRegistry.cpp` (a leaf lock
around the registry: every `Symbol.for` flag off takes it), `ExternalStringImpl.cpp`. Gated on the shared table or the
Config byte: `AtomStringImpl.cpp` (394 added lines; the legacy arm is kept and selected when the shared table is off),
`SharedAtomStringTable.{h,cpp}` (new), `AtomStringTable.cpp`, `DeferrableRefCounted.h` (atomic arm behind
`g_wtfConfig.useAtomicDeferrableRefCount`), `WTFConfig.h` (that byte).

#### 2.4.8 Reading checklist for the landing review, by risk

1. **(C)7, the tier-up trigger assertions**: confirm every bytecode index passed to `setOSREntryBlock`,
   `clearOSREntryBlock` and the hierarchy walk is a key inserted at link time, in every tier-up configuration
   (`useFTLJIT=0`, OSR entry disabled, catch entry). A missing key is an abort where `main` silently inserts.
2. **(C)1-2, metadata repack**: `UnlinkedMetadataTable::finalize`'s alignment ordering, the offlineasm-extracted offsets,
   the cached bytecode (`CachedTypes.cpp`) round trip of the new metadata shapes, and the 32-bit LLInt:
   `LowLevelInterpreter32_64.asm` is not touched by the branch at all (**measured**: six files changed in `llint/`, not this
   one) while `BytecodeList.rb` removed the `structureID`/`offset` metadata fields it reads for `get_by_id_direct` and
   `try_get_by_id`; a 32-bit build very likely fails in offlineasm. Bun ships no 32-bit target; upstream does.
3. **(C)5, `ArrayProfile` read-modify-writes** and the other new locked instructions on warm paths: decide which go back
   to `main`'s plain form flag off (performance, and one fewer difference to argue).
4. **(C)6, compare-and-swap publication of lazily created members**: the loser's object is freed or left to the
   collector correctly (`Structure::allocateRareData`, `InlineWatchpointSet::inflateSlow`, `StructureRareData` caches).
5. **(C)4, unconditional locks**: none is taken while another is held in an order `main`'s code can invert
   (`g_watchpointMembershipLock` inside watchpoint installation is the one to read: it is process-global and is taken
   with the structure's watchpoint set in hand).
6. **(D) `Structure` and `CodeBlock` size growth**: iso-subspace cell size classes, `static_assert`s on sizes, memory
   per structure in a large Bun process.
7. **(C)10 `JSBigInt` fast paths**: land separately or drop; it is 252 lines of arithmetic with its own correctness
   argument and nothing to do with threads.
8. **(C)9 bytecode-generator constant folding**: every existing builtin that branches on a registry constant compiles
   to the same bytecode as before (`@isConcurrentJSEnabled`-style constants, if any).
9. **Section 4's table**: each new release assertion's condition holds for every flag-off caller; the six
   `RELEASE_ASSERT_NOT_REACHED` default arms in `JSObject.h` get one careful reading.
10. **Section 4, Debug**: run the JSC stress suite and test262 on a Debug build flag off before anything else; 358
    unread assertions is too many to clear by reading.
11. **Section 6**: build the missing tool (disassembly of a fixed corpus per tier on `main` and on the branch flag off,
    diffed modulo addresses) before claiming identity; until then say "same instruction sequences by construction of
    each emitter, not verified".
12. **Section 2(b)**: consider deriving `VM::gilOff()` and `Heap::isSharedServer()` from the Config byte in release
    builds so that a flag-off process cannot enter a mixed state through one corrupted byte.
13. **WTF**: `SymbolRegistry` lock and `StringImpl::setNeverAtomize` compare-and-swap are new work in every WTF
    client of this fork; `WeakRandom`'s builtins need a non-GCC fallback if MSVC proper is ever a target.

#### 2.4.9 What this audit did not settle

- (C) is not proven complete: it was assembled from the branch's own comments (153 declarations of "unconditional"),
  the lock/atomic greps and two samples. A reviewer reading the ungated residue file by file would find more shape
  changes and possibly more behaviour changes; the files with the most residual lines are `Heap.cpp` (799),
  `FTLLowerDFGToB3.cpp` (464), the two LLInt assembly files (454), `JSArray.cpp` (221), `Structure.cpp` (157).
- The 113 release and 358 Debug assertion candidates were triaged, not all read; the table of section 4 is what was read.
- A 32-bit (`JSVALUE32_64`) build was not attempted; by the diff it cannot build (checklist item 2). The CLoop shares the 64-bit
  assembly and was built in the rounds.
- Whether any existing builtin's bytecode changes through the new constant folding was not checked.
- The list of 106 emitter functions with ungated emission lines was not read emitter by emitter; "same instructions flag
  off" is by reading a dozen of the 106.

### 2.5 Bun

- **Recorded runs** (tenth round, twelve directories: `bun/jsc bun/ffi bun/util node/vm node/util web/timers
  node/worker_threads web/workers node/fs node/http web/fetch bun/http`, Debug build against the branch engine, side by side
  with a stock build of the same Bun commit): **flag off no behaviour differs**. Over their time budgets: the four
  `fetch-tcp-stress` cases (30 s budget; since the ninth round) and `abort-signal-leak-read-write-file` (stock 293 s,
  branch 321 s, budget 300 s, no leak on either); `bun/jsc`'s DOMJIT cases time out at 60 s on stock and on the branch in
  different subsets. The cause of the throughput difference is E-9 (accessor call chains in a build that does not inline).
  No Release Bun build has been measured; no Bun benchmark has been run in any mode.
- **What of the Bun-side patch flag off needs** (**read**, the tenth round's patch, 15 files): three hunks are needed to
  BUILD Bun against the branch's headers, whatever the flag: `TopExceptionScope.rs` / `TopExceptionScopeBinding.cpp` (the
  size constant 56 -> 72 and its `static_assert`), `URLPatternComponent.cpp` (`ovectorSpan(vm)`), `JSONRowsToJS.cpp`
  (`JSONAtomStringCache::live(vm)`). Everything else exists for flag-on behaviour and is inert or unreached flag off: heap
  walks wrapped in `runWithOtherClientsStopped` (`JSEnvironmentVariableMap`, `BunDebugger`, `JSInspectorProfiler`;
  **read**: the wrapper is an inline
  `if (!isSharedServer()) [[likely]] { func(); return; }` in `Heap.h`, and a heap is a shared server only for a GIL-off VM, so
  flag off it is a direct call), `JSLockHolder` around the boot-time
  time-zone reset, `notifyNeedTerminationForCurrentThread` in `node:vm`, the microtask-tick hooks returning on a null default
  global, client subspaces registered with the main client's allocator cache, destructors marshalled to the VM's thread
  (`generate-classes.ts`, `BunClientData`, `StrongRef`), the `destroyVM` wait for completed Threads, one test that spawns Bun
  with the flag on. Whether Bun's tree is to carry the flag-on hunks at the first landing is a decision.
- Standing embedder items that matter only with the flag on (Bun natives on engine-spawned threads, the marshalling
  predicate, the accept loop): not flag-off items.

### 2.6 Platforms

- **read** `Options.cpp` (D8): with the flag ON, option validation crashes with "useJSThreads is unsupported on this
  platform" unless `CPU(ADDRESS64)` and (Linux x86-64/arm64, or Darwin with `ENABLE(FAST_TLS_JIT)`, which needs Apple's
  internal SDK). So a macOS build from the public SDK and every Windows build refuse the flag, GIL on included; GIL off is
  further forced back to GIL on (with a log line) on every non-Linux, CLoop or 32-bit build. Flag off none of this is
  reached. Two more `CRASH()` refusals are flag-on only (`useHandlerICInFTL` off; `useProfiler` GIL off).
- **recorded in** DESIGN-PROPOSALS N1 (reading pass, nothing built): **the arm64 build is broken in two places, and these
  break a flag-off build too because the code is compiled unconditionally**: `branchAtomicStrongCAS32/64` exist only in
  `MacroAssemblerX86_64.h` and are called without a `CPU` guard from five emitters (`storeMegamorphicProperty`, three
  inline-cache handler sites, the two generator claim/publish emitters); `batomicweakcasi` is an x86-only offlineasm
  instruction used unguarded in `LowLevelInterpreter64.asm`'s `op_put_by_val` (the arm64 backend raises "Unhandled opcode").
  Design N-D1 (small). The fork's CI builds Linux and macOS arm64; no local arm64 sysroot exists, so this was found by
  reading only.
- Nothing has been built or run on macOS, Windows, musl or arm64 in any round (LANDING-PLAN "Current state": results come
  from Linux x86-64). Whether the branch compiles there flag off is unknown beyond N1; Windows and musl have their own
  TLS and gigacage notes in the option's help string (flag-on concerns).
- N6 (the heap fenced for its whole life on non-x86 targets) and N7 are flag-on. N9 above is the one flag-off arm64
  performance item. The dead `#if USE(JSVALUE64)` (K5) compiles out a flag-on arm only; flag off emits the plain store as
  `main` does.
- ThreadSanitizer builds keep relaxed atomics where E-D1 would make accesses plain (by design).

### 2.7 Binary size

- **recorded in** DESIGN-PROPOSALS F-I2: text segment 35.26 MB against 30.50 MB (+4.75 MB, +15.6 %).
- **measured** (`size -A` of the two `jsc` shell binaries, Release, static): `.text` section 29.90 MB against 25.32 MB
  (+4.58 MB, +18.1 %); `.rodata` 1.82 MB both; `.data` equal; `.bss` 90 KB against 46 KB. File sizes (with debug
  information) 393 MB against 333 MB.
- **recorded in** PERF-RESULTS section 4 (fifth round, not repeated since): both trees built with thin LTO, the
  configuration Bun ships: `jsc` shell 43.6 MB on `main`, 47.3 MB on the branch (+3.7 MB, +8.5 %); the micro set's flag-off /
  `main` ratios matched the non-LTO pair row for row (1.00-1.05 except class-ctor-4 1.08, regexp-exec 1.10, string-concat
  1.10, json-stringify 1.07; megamorphic-access 0.80). "LTO does not change the picture in either direction." Five rounds
  old; the branch has grown since (the non-LTO text delta was not recorded then).
- F-D4 adds about 150 KB and E-D3 about 40 KB of text while removing inlined cold arms from hot instances; neither has been
  built.

### 2.8 What is not known

1. Whether the branch compiles anywhere but Linux x86-64, beyond the two arm64 breaks found by reading.
2. Whether flag-off generated code equals `main`'s (SPEC-jit I1): never compared instruction by instruction. In time it
   does: +0.4 % of a run over all tiers together (2.2); the sweep's +455 and +170 samples are closed.
3. What the 7,500 ungated lines contain beyond the families of 2.4: the list of deliberate differences was assembled from
   the branch's own comments, greps and two samples, and is not proven complete.
4. Whether any of the 358 unread Debug assertions fires on the suites: two partial samples on the branch's Debug+ASAN
   build (12,900 plans of about 86,000): no plan failed on an assertion the branch added; **one flag-off Debug
   regression found** (an upstream cell-validation assertion reached through an argument the branch evaluates before a
   call that returns at once flag off, `CallLinkInfo::unlinkOrUpgradeImpl`; 1.7 % of runs under load; one-line fix); 51
   failing plans are properties of the build type and were not checked against a Debug `main` (none exists on the
   machine).
5. Why the suite in one process peaks 8 to 25 % higher in resident memory flag off when single tests differ by 2 % and the
   collector's own figures are equal (2.2 item 5).
6. Size and speed in the configuration the embedder ships (thin LTO): one measurement, five rounds old. The embedder's
   binary, start-up, memory and benchmarks: never measured.
7. Whether the three build-fix hunks are the complete set for the embedder's current `main`.
8. What a fuzzer finds flag off that it does not find on `main`: no fuzzer has run on a tree of the landing rounds.
9. Whether a Worker's VM start-up in the embedder can re-run option code after finalization (by reading the latch forbids
   a change; not traced through the embedder).

## Part 3. What "landed with the flag off" means: the definition of done

The claim to be able to make on the day of the merge: **a process that does not set `useJSThreads` cannot tell this engine
from `main`: not by a result, not by a crash, not by a measurable amount of time or memory; and the code that such a
process executes has been accounted for line by line.** The flag-on modes are in the same binary, reachable only by
setting the flag, documented as experimental with their known defects listed.

Each criterion below has a check that can fail, and its state today. "Equal to `main`" always means: the same binary
flags, the same machine, the same session, `main` at the commit the branch is rebased on.

| # | criterion | check | today |
|---|---|---|---|
| D1 | Results equal `main`'s on every suite the engine has | failure lists equal, Release: the seven JSC stress collections, the wasm collection, test262, `testmasm`, `testb3`, `testair`, `testdfg`, `testRegExp`, and `testapi` as far as decision F10 makes it runnable; one Debug+ASAN pass of `JSTests/stress` with no failure that Release does not have and no fire of an assertion the branch added | seven collections: equal but for five results that are not the branch's (2.3). The rest: test262 identical to `main` on 102,475 records; `testmasm` (394), `testair` (276), `testb3` (216,588), `testdfg`, `testRegExp` (698) identical; `testapi` cannot complete on the fork's `main` either (same abort, same stack), its C++ half identical 18/18; wasm collection and the validation sweeps: not run; Debug: samples only (2.3) |
| D2 | Generated code equals `main`'s | a golden compare (L4) over a fixed corpus, per tier, concurrent compilation off: differences only in field-offset immediates and predicted-false tests of the two Config bytes; the LLInt: the list of opcodes that carry a gate branch, reviewed | never checked against `main` (2.3: the existing identity script compares the branch with itself); the rule is SPEC-jit I1 |
| D3 | Every change that a flag-off process executes is accounted for | a checked-in ledger generated from `git diff <base> HEAD` (L3): every hunk not enclosed by a flag-on gate has a row: class (same algorithm in another shape / deliberate difference / new data / new fail-stop / new lock, atomic or fence), one line of reason, reviewer; the script fails when a hunk has no row | the tenth round listed its own 73 hunks; earlier rounds theirs; no list over the whole branch. This session's pass over the whole diff: section 2.4 |
| D4 | No fail-stop the branch added can fire flag off on script input | the ledger's fail-stop rows are all "internal invariant" or "unreachable flag off"; the flag-off fuzz campaign (D6) and the Debug pass (D1) hit none | 596 added release sites; 189 not enclosed by a gate by the classifier's reckoning; 113 of those needed reading; about 75 of the 113 are flag-on code; the rest is section 2.4.4's table, about twenty new conditions; 358 ungated Debug assertions unread |
| D5 | Sanitizers | corpus and a one-in-five sample of `JSTests/stress` flag off under ASAN (Debug) and TSan: no report `main` does not have | ASAN: the corpus's flag-off lanes are clean; the stress sample: two partial samples on the branch's Debug+ASAN build (12,900 plans of about 86,000): no plan failed on an assertion the branch added; **one flag-off Debug regression found** (an upstream cell-validation assertion reached through an argument the branch evaluates before a call that returns at once flag off, `CallLinkInfo::unlinkOrUpgradeImpl`; 1.7 % of runs under load; one-line fix); 51 failing plans are properties of the build type and were not checked against a Debug `main` (none exists on the machine). TSan flag off: not run as a lane |
| D6 | Fuzzing (subject to decision F9) | the rig rebuilt; the same campaign (profile without the threads generators, flag off) on `main` and on the branch, 10 CPU-days each: no signature on the branch that `main` does not have | no fuzzer has run on any tree of the landing rounds; the rig is gone from the project's machine (no toolchain, no checkout, no instrumented build; the scripts and the profile are in the tree; a flag-off campaign is the stock `jsc` profile, usable on `main` side by side) |
| D7 | Speed | the metrics table of L5, all of it: first-iteration, interpreter-only, tier-capped and whole-run main-thread cycles at their targets; quiet pass, full JetStream, interleaved, medians of five, in two sessions: score at or above 0.99 of `main` with Startup and Worst Case at or above 0.985, no test under 0.95 that `main`'s own bimodality does not explain; the micro set: no row above 1.03; the same on a thin-LTO pair (the configuration Bun ships) | score 0.966 (Startup 0.950, Worst Case 0.957, Average 0.978); whole-run main-thread cycles 1.013, first iteration 1.039, interpreter only 1.041; the cost sits in the iterations the score weighs most (2.2); one micro row at 1.18, four at 1.05-1.06; LTO: measured once, five rounds ago |
| D8 | Start-up and memory | shell start-up instructions and peak resident set for an empty script and a small one within 1 % of `main`; peak resident set of single tests within 1 % and of the suite in one process within 2 % (median of five); Bun's `--version` and hello-world the same | start-up +2.7 % instructions, +1.4 MB; single tests +2.2 %; the suite in one process +8 to +25 % in four pairs of runs, not explained (2.2) |
| D9 | Size | text size delta of the shell and of Bun's binary under thin LTO stated in the pull request; a bound agreed with the embedder | shell, non-LTO: `.text` +4.58 MB (+18 %); LTO: +3.7 MB (+8.5 %) five rounds ago; Bun's binary: never measured |
| D10 | Builds and runs flag off everywhere the embedder ships (support of the flag itself there is not asked: F9) | the embedder's CI matrix green with the flag off: Linux x86-64 and arm64 (glibc and musl), macOS x86-64 and arm64, Windows x86-64; Debug, Release, LTO, ASAN. Where the flag is unsupported, setting it is refused at option validation with a message | built on Linux x86-64 only, ever. Known: two compile breaks on arm64 (N1), reached flag off because the code is compiled unconditionally. Everything else unknown |
| D11 | The embedder | Bun builds against the new headers with the three build-fix hunks (2.5) and nothing else; Bun's whole test suite on its CI, flag off, equal to the baseline run of the same Bun commit on the old engine; a small Release benchmark set (start-up, HTTP, bundler) within noise | twelve directories, Debug, flag off: no behaviour difference, five cases over their time budgets (call chains in a build that does not inline, E-D5). No Release Bun, no benchmark, no full suite |
| D12 | The flag | the decision of Part 5 on how the flag may be set; the help text and the embedder's documentation say experimental, say what is unsupported, and list the known flag-on defects; GIL off keeps its second, explicitly named option; nothing after option finalization can change either | `BUN_JSC_useJSThreads=1` in the environment turns it on in any Bun built on this engine; GIL off additionally needs `useThreadGILOffUnsafe`; frozen after finalization (2.1) |

What is deliberately not in the definition: anything about the flag-on modes' own correctness or speed (PARITY-PLAN's gates B
and C), arm64 or non-Linux support of the flag itself, WebAssembly or FFI on spawned threads. Two flag-on items are taken
along anyway because they are small and their absence would make "experimental" mean "known unsafe": section 4, L7.

## Part 4. Work packages, in order

The order puts first what is cheapest to learn and most likely to change the rest: whether the branch builds on the
platforms it has never been built on, and what the suites that have never run say. Performance work comes after the
ledger, because the ledger removes flag-off changes and the performance work would otherwise tune code that goes away.
Every package ends with the standing battery of the rounds on the flag-off lanes (the corpus flag off on Release and
Debug, the seven collections against `main`) and leaves the flag-on lanes no worse than it found them.

### L0. Rebase and baselines

Rebase onto the commit Bun pins that day; rebuild `main` at that commit and the branch; re-take the baselines of Part 2
with the harness of PARITY-PLAN's P0.5 (main-thread counters, the quiet pass). Done when 2.2's tables are reproduced
within 1 % on the new base. From here on every measurement is against that `main`, interleaved, same session.

### L1. Build everywhere first

| item | content | done when |
|---|---|---|
| L1.1 | N-D1: `branchAtomicStrongCAS32/64` and `batomicweakcasi` behind `CPU(X86_64)` with the portable forms DESIGN-PROPOSALS section N names; the arm64 emitters compile, the flag stays refused for GIL off on arm64 (decision 5). Also known by reading: `LowLevelInterpreter32_64.asm` was not updated for the metadata fields `BytecodeList.rb` repacked, so a 32-bit or CLoop build very likely fails (none is shipped; state it or fix it); `WeakRandom.h` uses GCC/Clang builtins (fine for clang-cl, not for MSVC proper) | an arm64 build links; the 32-bit state is written down |
| L1.2 | A draft pull request (or a CI branch) whose only purpose is the embedder's build matrix with the flag off: Linux x86-64 / arm64, glibc / musl; macOS x86-64 / arm64 (public SDK); Windows x86-64; Debug, Release, LTO, ASAN. Every break fixed in the branch, each fix a row of the ledger (L3) if it touches code that runs flag off | the matrix is green; the list of fixes is in the pull request |
| L1.3 | On every platform where `Options.cpp` refuses the flag: a test that setting it prints the refusal and exits, flag off runs the corpus's flag-off lane | the refusal test passes on macOS and Windows runners |

This needs a push to a CI-visible branch, which is the project owner's call each time (Part 5, decision F3). It comes
first because every other package assumes a tree that compiles where it has to, and because nothing about Windows, musl or
macOS is known beyond what reading found.

### L2. The suites that never ran

| item | content | done when |
|---|---|---|
| L2.0 | The one defect these lanes already found: `CallLinkInfo::unlinkOrUpgradeImpl` passes `m_callee.get()` to `publishRecord`; flag off the call returns at once but the argument is evaluated first, and a build with assertions validates a weak callee that may already be swept. `unvalidatedGet()` (the bits are only a comparand), or the argument evaluated under the flag test. A test: the two stress files that hit it, `dfg-eager` under `collectContinuously`, 240 runs at 120 in parallel (today 4 failures) | 0 in 1,000 |
| L2.1 | test262 on `main` and flag off, same tree of tests, same expectations | failure lists equal; every differing test run alone five times on both and explained |
| L2.2 | `testapi`, `testmasm`, `testb3`, `testair`, `testRegExp` from a developer-mode build directory, `main` and branch | equal; `testapi`'s multi-threaded VM tests (`MultithreadedMultiVMExecutionTest`, `VMManagerStopTheWorldTest`) get a second look because the branch changed `VMManager` and `JSLock` |
| L2.3 | One Debug+ASAN pass of `JSTests/stress`, flag off, against the Release list; then the other six collections once | no failure Release does not have; no fire of an added assertion |
| L2.4 | The wasm collection flag off as a collection of its own; the seven validation sweeps of SCAN-RESULTS on the flag-off lane (`verifyGC`, `scribbleFreeCells`+`useZombieMode`, `collectContinuously`, `validateDFGClobberize`, `validateGraphAtEachPhase`, `validateExceptionChecks` on Debug, `GIGACAGE_ENABLED=0`) | equal to `main` under the same option |

What this session already ran of L2 is in section 2.3: L2.1 and the executables of L2.2 are equal today and have to be
kept equal (they become lanes); `testapi` needs a decision (it does not complete on the fork's `main`); L2.3 has been
sampled, not run.

### L3. The ledger of code that runs flag off

| item | content | done when |
|---|---|---|
| L3.1 | The generator: split `git diff <base> HEAD -- Source` into hunks, find each hunk's enclosing gates (the vocabulary of section 2.4), emit the hunks that are not enclosed; the ledger file has one row per such hunk or group; CI fails when a hunk has no row | the script and the ledger are in `Tools/threads` and `docs/threads`; section 2.4's counts reproduce |
| L3.2 | **Shrink before arguing.** Every ungated hunk is first asked "why is this not behind the gate". From section 2.4 the first candidates: `ArrayProfile`'s locked read-modify-writes back to `main`'s load, `or`, store flag off; the fifteen locks taken unconditionally made conditional on the gate (the `std::optional<Locker>` shape the branch already uses at 35 sites); `JITStubRoutine`'s and `InlineCacheHandler`'s reference counts atomic only with the flag; the tier-up trigger's `find` + `RELEASE_ASSERT` back to `main`'s `set`; the `JSBigInt` single-digit fast paths (252 lines, nothing to do with threads) out of this change, to land or not on their own; `SymbolRegistry`'s lock and `setNeverAtomize`'s compare-and-swap behind the shared-table latch; `VM::gilOff()` and `Heap::isSharedServer()` derived from the Config byte in release builds so that one corrupted byte of ordinary memory cannot put a flag-off process into a mixed state; the unconditional store-store fence in `resetStubAsJumpInAccess` and `VariableEnvironment`'s map lock; the 8 bytes per call opcode of `CallLinkInfo`'s record pointer and the 128 bytes of padding per JIT code object weighed against their flag-off memory cost. Whatever can go behind the gate or be instantiated per mode without cost does, and its row disappears | the ledger's size after the pass is reported beside its size before (today: 7,484 ungated lines in 1,638 pre-existing functions of 473 files, plus 1,972 inserted gate tests) |
| L3.3 | **Argue what remains.** Class C rows (deliberate differences: upstream fixes found on the way, the resolve-scope reload, reordered stores, changed memory orders) each get the argument why `main`'s behaviour is preserved or why the difference is a fix, and a test; candidates for upstreaming are marked. Class D rows (layout: `TopExceptionScope` 56 -> 72 bytes, repacked fields, `ArrayBuffer::m_locked`) each get the list of offset consumers checked (JIT, LLInt offsets extractor, the embedder's bindings) | every row has a reviewer's initials |
| L3.4 | Fail-stops: every added `RELEASE_ASSERT`, `CRASH()` and emitted trap that is not enclosed by a gate is classified (internal invariant with the invariant named / unreachable flag off with the reason). Today: 596 added release sites, 189 not enclosed by the classifier's reckoning, 113 of those needed reading, about 75 of the 113 turned out to be flag-on code, and the remainder is section 2.4.4's table (about twenty new conditions a flag-off process can evaluate; the tier-up trigger key and the six default arms in `JSObject.h` first). Debug: 1,070 added `ASSERT` lines, 358 ungated and unread; they are cleared by running, not by reading: L2.3 over all seven collections and test262 | D4's table complete; the Debug runs of L2.3 fire none |
| L3.5 | WTF (bmalloc and libpas are untouched): the unconditional changes listed (section 2.4.7) and each either gated on a WTF-level latch that is false flag off or argued; the embedder links WTF directly, so these rows are reviewed with the embedder | rows signed off by the embedder's side |

### L4. The golden compare

A tool that the project's documents have named since SPEC-jit's first task list and that does not exist: for a fixed
corpus (the 40-file sample of `JSTests/stress` the identity script uses, the 36 JetStream tests' hottest functions, the
corpus's JIT directory), `main` and the branch flag off each compile with `--useConcurrentJIT=0` and fixed thresholds and
dump, per code block and tier, the emitted code. The Release configuration has no disassembler; two workable forms: (a) a
developer-mode build with the disassembler on, text dumps normalized (addresses, immediates that are field offsets or
Config-page addresses) and diffed; (b) no disassembler: the FTL's Air listing and the DFG's and Baseline's
`MacroAssembler` call trace under a new dump option, same normalization. The report lists, per tier, how many code blocks are
identical, how many differ only by the permitted classes, and every other difference in full. Done when the last list
is empty or every entry is a ledger row. The LLInt is compared at the source level: the list of opcodes and macros that
carry a gate branch, reviewed once.

### L5. Speed, start-up, memory, size

Section 2.2 ("Where the score goes") changed what this package is. The gap is one percent of time and three and a half
percent of score, because it sits in the iterations the score weighs most: the first, and those in which a collection or a
tier-up happens. So the package is ordered by what those iterations execute, and it is judged on four numbers, not one:
first-iteration cycles, interpreter-only cycles, whole-run cycles (all main thread, `perf stat --no-inherit`, the suite in
one process), and the quiet-pass score with its three sub-scores.

| metric (flag off / `main`) | today | target |
|---|---|---|
| first iteration of every test, main-thread cycles | 1.039 (per test 1.028) | 1.010 |
| interpreter only (`--useJIT=0`), cycles, per-test geometric mean | 1.041 | 1.010 |
| Baseline-capped / DFG-capped, cycles | 1.010 / 1.025 | 1.005 / 1.010 |
| whole run, main-thread cycles | 1.013 | 1.005 |
| locked loads, whole run / first iteration | 1.11 / 1.29 | 1.02 / 1.03 |
| quiet pass: Startup / Worst Case / Average / score | 0.950 / 0.957 / 0.978 / 0.966 | 0.985 / 0.985 / 0.992 / 0.99 |
| start-up: instructions, resident set (empty script) | +2.7 %, +1.4 MB | +1 %, +0.5 MB |
| peak resident set, suite in one process (median of five) | +8 to +25 % | +2 % |

**First group: what the first iteration and the tier-up iterations execute.** Each item is small; most are also rows that
L3.2 removes from the ledger, and are done there once.

| item | content | measured today | expected |
|---|---|---|---|
| L5.1 | `ArrayProfile`: flag off the observation and the prediction update are `main`'s load, `or`, store (the locked forms stay behind the gate; `ArithProfile` already does this). `CodeBlock::updateAllArrayProfilePredictions` loses its two locked instructions per profile per tier-up check | 107 -> 388 samples in the first iteration (+0.49 %); first-iteration locked loads 33 M -> 42.5 M; octane-zlib's first iteration +9 % | first iteration -0.5 %; locked loads in the first iteration back to within 3 % |
| L5.2 | The unconditional locks of 2.4.3 (C)4 conditional on the gate (the `std::optional<Locker>` shape used at 35 sites already): `CodeBlock::m_lock` in call linking and plan finalization, `SharedJITStubSet::m_lock`, `g_watchpointMembershipLock`, `JSGlobalObject::m_installedWatchpointsLock`, `CompactTDZEnvironmentMap`, `IntlCache`, `VariableEnvironment`'s map, `SymbolRegistry` | lock guards' destructors 0 -> 100 samples, `getSlowPathHandler` 2 -> 30 (+0.2 %) | -0.2 % |
| L5.3 | Call linking: `CallLinkInfo::m_flags` updated with plain stores flag off, the record pointer not touched (`publishRecord`'s arguments evaluated under the gate, which is also L2.0's fix) | `operationDefaultCall` 35 -> 91 | -0.1 % |
| L5.4 | Reference counts of `JITStubRoutine` and `InlineCacheHandler` atomic only with the flag (the collector's end-of-cycle walks `deleteUnmarkedJettisonedStubRoutines`, `reconcileWeakReferencesAtGCEndImpl` pay them) | +55 samples first iteration, +62 whole run | -0.1 %; shortens collection ends (Worst Case) |
| L5.5 | E-D4 `sanitizeStackForVMImpl` (the slot chosen in C++, the assembly routine as on `main`) | 115 -> 174 first iteration; 68 -> 120 whole run | -0.1 % |
| L5.6 | E-D3 array and object creation per process mode (`JSArray::tryCreate`, `operationNewArrayWithSize`, `tryAllocateCell<JSCellButterfly>`, `allocateCell` allocator choice); `Structure`'s two added watchpoint sets constructed lazily flag off | +130 samples first iteration | -0.2 % |
| L5.7 | E-D1, E-D2 the marker (one load per operation, plain outside the sanitizer; `visitButterflyImpl` per mode); `Heap::didAllocateBlock` and `BlockDirectory::findBlockForAllocation` back to `main`'s shape flag off | marking +6.4 % instructions per collection; collector and allocation +0.42 % of the first iteration, +0.26 % of the run | Worst Case +1 point on the allocating tests; splay's in-suite Worst Case (0.775) re-measured |
| L5.8 | The reference-count shape of atom strings in the joiner and the identifier paths (`Ref<AtomStringImpl>` out of line, the joiner's entry vector): `main`'s inline forms flag off | 0 -> 111 samples first iteration | -0.2 % |

**Second group: the interpreter.**

| item | content | measured today | expected |
|---|---|---|---|
| L5.9 | The LLInt dispatches through a table filled at initialization (`g_opcodeMap` and its two wide siblings). The opcodes that carry a gate (`get_by_id` family, `put_by_id`, `get_by_val`, `put_by_val`, `in_by_*`, `instanceof`, `get_from_scope`, `put_to_scope`, `resolve_scope`, the call family, `new_array*`: about two dozen) get two bodies under two labels, `main`'s and the threaded one; `LLInt::initialize` installs the threaded labels when the flag is on. Flag off no opcode executes a gate test. The 29 "Group-3" sites (VM entry and exit, unwinding, host-call return) keep their one predicted-false byte test: they are per call into or out of the VM, not per opcode. The packed `get_by_id_direct`/`try_get_by_id` metadata stays (it is a layout, not a test) unless L3.2 un-repacks it | interpreter only: +5.2 % instructions, +4.1 % cycles per test (2 to 8 % on 35 of 36 tests) | interpreter only within 1 %; text +40 KB |

**Third group: steady state** (the designs of DESIGN-PROPOSALS sections E and F, unchanged).

| item | content | expected |
|---|---|---|
| L5.10 | F-D4 with F-D5 and E-D6: RegExp and string entry points per mode, one gate per throw scope, the matching context's constructor inline again | regexp, OfflineAssembler, UniPoker to within 2 % in main-thread instructions; Average +0.5 point |
| L5.11 | E-D5 Config-page bytes for the `Options` gates flag-off code reaches in builds that do not inline | the embedder's Debug lanes under their budgets |

**Fourth group: memory and size.**

| item | content | done when |
|---|---|---|
| L5.12 | **Explain the resident-set difference of the suite in one process** before changing anything: (a) `smaps` of both processes at the end of splay and at the end of the run, by mapping kind (collector blocks, the malloc heaps, executable memory, thread stacks); (b) the collector's capacity and block counts from its own statistics at the same two points; (c) the malloc side: live bytes by size class; (d) bisect by the ledger's class D rows (the 128 bytes of padding per JIT code object, the record pointer per call opcode, `Structure`'s growth, `CodeBlock`'s, the handler-IC data) with each compiled out in turn. Then fix what it names | the curves of 2.2 item 5 coincide within 2 % in five runs of five |
| L5.13 | Start-up: the 0.22 M instructions and 1.4 MB of an empty script attributed (static constructors, new globals, per-VM structures created eagerly, the larger text's page faults) and made lazy where flag off never uses them | D8 |
| L5.14 | The thin-LTO pair: both trees in the configuration the embedder ships; every metric of the table above, the micro set, text size; the embedder's binary size | D7 and D9 on the shipped configuration |

Order: L5.1 to L5.8 (most fall out of L3.2), then L5.9, then the quiet pass; L5.10 after it; L5.12 can start at once, in
parallel, because it changes no code until it has an answer. Expected after the first two groups, from the sub-scores'
composition: Startup 0.950 -> about 0.985, Worst Case 0.957 -> about 0.98, Average 0.978 -> about 0.985; with L5.10 the
Average to about 0.99: **score about 0.985 to 0.99**. The recorded estimate "about 0.99 with no margin" stands, but it now
has a route that does not depend on the last half point of steady-state code: two thirds of the distance is in places
that are cheap to restore to `main`'s form because `main`'s form is still there behind the gate.

### L6. Fuzzing

The rig rebuilt (toolchain, the fuzzer's checkout, the instrumented build script of `Tools/threads/fuzz`); the stock
profile (no threads generators), flag off, on `main` and on the branch for 10 CPU-days each; signatures de-duplicated by
the triage scripts; every branch-only signature is a defect to fix before the merge. The threads profile flag on is
PARITY-PLAN's L6 and is not part of this gate.

### L7. The flag as shipped

| item | content |
|---|---|
| L7.1 | The decision on exposure (Part 5, F1) implemented: environment variable as today, or an explicit embedder-level opt-in |
| L7.2 | The option's help text, the embedder's documentation and the API reference say: experimental; off by default; a high-resolution-timer capability; not for semi-trusted code; unsupported on which platforms; GIL off additionally behind `useThreadGILOffUnsafe`; the known defects of PARITY-PLAN section 6.2 by name |
| L7.3 | Two flag-on fixes taken along because "experimental" should not mean "known memory-unsafe with the GIL on": W4/W5 (a spawned Thread must not run WebAssembly; the wasm atomic wait must drop the GIL: both flag-on modes; G D3.1) and the property-table rebuild race (PARITY-PLAN 9.1; a re-check in one function; flag off untouched). Both with their tests |
| L7.4 | A lane in the embedder's CI that runs the corpus with the flag on (GIL on), so that the experimental mode does not rot between landing and the next round |

### L8. The pull request

658 source files, +87,000 / -6,300 lines; 489 files of tests; 25 commits that are rounds, not reviewable units. One
merge, but presented for review as slices that each stand alone flag off: (1) WTF and bmalloc; (2) options, the Config
bytes, the gate vocabulary; (3) layout and new data; (4) the heap; (5) the object model in C++; (6) LLInt; (7) Baseline and
inline caches; (8) DFG; (9) FTL and B3; (10) the API objects and the shell; (11) tests and tools; (12) documents. The
ledger is the reviewer's index: per slice, the rows that run flag off first, the gated remainder after. Whether the slices
are separate commits on the branch or a review guide over one squashed commit is decision F4.

### L9. Final battery and merge

On the rebased final tree, in one session: D1 to D11 re-checked, the quiet passes (non-LTO and LTO), the embedder's full
suite on its CI, then the merge and the pin bump in the embedder with its three hunks. The flag-on lanes of the rounds'
battery run too and are reported, not gated.

## Part 5. Decisions that are the project owner's

**F1. How may the flag be set in a shipped binary?** Today any process that inherits `BUN_JSC_useJSThreads=1` has the flag
on; there is no command-line switch, configuration key or JavaScript API. Options: (A) leave it: the engine's every other
option is reachable the same way, and the flag's help text already says it is a capability grant; (B) the embedder refuses
the environment form for this one option and adds an explicit switch (`--experimental-threads` or a `bunfig` key), so that
turning it on is an act of the program's author and not of whoever controls the environment; (C) compile it out of release
builds until gate B. Recommendation: (B). It costs a few lines in the embedder, it matches how other runtimes expose
experimental features, and it makes the statement "a process that did not ask for threads cannot get them" true without an
argument about environments. GIL off keeps `useThreadGILOffUnsafe` under every option.

**F2. How much flag-on correctness rides along.** The gate is about flag off. But the binary will contain modes with known
defects, two of them memory-safety relevant with the GIL on (W4: WebAssembly reachable from a spawned Thread) and one with
the GIL off that is small (the property-table rebuild race). Options: (A) none: experimental means experimental; (B) the
two of L7.3; (C) all of PARITY-PLAN's P1 before the merge. Recommendation: (B): an afternoon each against "shipped with a
known way to corrupt memory".

**F3. When to push to CI.** L1 cannot happen on the project's machine. It needs a branch the embedder's CI builds.
Recommendation: a draft pull request against the WebKit fork as the first act after the rebase, marked not for review,
whose description is the build matrix's state; pushes remain on request as in every round.

**F4. The shape of the pull request.** One squashed commit with a review guide, or a dozen slice commits that each build
and pass flag off. The second is a large rebase exercise (the rounds interleave every directory) and gives bisectability
flag off; the first is what the rounds produced. Recommendation: one commit per slice only for slices 1 to 3 (WTF, options,
layout), which are small, touch what the embedder links directly, and are where a flag-off regression would most likely
hide; the rest as one commit with the ledger as its index.

**F5. The speed bar.** 0.99 in the quiet pass has been the stated gate since the ninth round and has not been met (0.966 to
0.98 in the passes on record). This session found why the score sits below the time (one percent of time, three and a half
of score: the cost is in the first and the worst iterations) and that two thirds of it is cheap to remove (L5's first two
groups restore `main`'s forms that are still there behind the gate). Options: hold the merge until two sessions show 0.99;
or merge when the four cycle metrics of L5 are at their targets and the score is at or above 0.985, with the remainder
stated. Recommendation: the second. The cycle metrics are stable to a few tenths of a percent and say what a user pays; the
score moves by a point and a half between sessions with no change to the code.

**F6. The size bar.** +3.7 MB (+8.5 %) on the shell under thin LTO five rounds ago, more now. The embedder has worked to
take hundreds of kilobytes out of its binary. Options: accept; or compile the flag-on tiers out of the smallest targets;
or set a bound and spend L5 effort on cold-arm outlining (the same work that recovers speed also stops duplicating cold
arms into leaves). Recommendation: measure first (L5.14), then set the bound with the number in hand.

**F7. What the embedder's tree carries.** Three hunks are needed to build. The other twelve files of the embedder-side
patch serve flag-on behaviour (destructors marshalled to the VM's thread, heap walks under a stop, the teardown wait).
Land them with the engine, or later with gate B? Recommendation: with the engine, behind the same opt-in: they are inert
flag off (checked, 2.5), and a flag that can be set but whose embedder half is missing is a worse experimental feature than
one that works with the GIL on.

**F8. Upstream.** Several flag-off rows are fixes to upstream code found on the way (the rebase audits list them). Each
is a candidate for an upstream patch independent of threads; sending them shrinks the ledger permanently. Recommendation:
mark them in L3.3 and send them as they are confirmed; none blocks the merge.

**F9. Two earlier rulings that this goal touches.** The fuzzers and arm64 were ruled out of scope for the landing rounds, with
the instruction that the documents list what each would need. This plan keeps both rulings for what they were about (the
flag-on modes) and asks for a narrower thing in each case, which is the owner's to grant or refuse. *arm64:* support of the
flag on arm64 stays out of scope; but the embedder ships arm64 binaries, so the branch has to **compile and run flag off**
there, which today it does not (N1). That is L1 and is not optional for a merge. *Fuzzers:* a campaign on the threads
profile stays out of scope (it is PARITY-PLAN's gate C). D6 asks only for the stock profile with the flag off, against the
same campaign on `main`, because shipping in the embedder's binary changes what an undetected flag-off regression costs.
If the ruling stands for this too, D6 is replaced by: the Debug pass of L2.3 over all seven collections plus test262, the
validation sweeps of L2.4, and the ledger's fail-stop table; and the pull request says that no fuzzer has run on the branch.

**F10. `testapi`.** It aborts on the fork's `main` in its first test (a second native thread creates and destroys a context;
upstream's assertion that a string being removed is in the current thread's atom table), three runs of three, and so its C half,
which holds the multi-VM and stop-the-world API tests, has never run on the fork at all. Options: fix the fork's `main` first
(it is not this branch's defect, but the branch changes `VMManager`, `JSLock` and the atom table, which is exactly what those
tests exercise); or declare the API tests unsupported for the fork and drop `testapi` from D1. Recommendation: fix it on
`main` as its own small change before the rebase of L0, because a suite that exercises multi-threaded VM entry is the one this
merge most wants to have.

## Part 6. After the merge

Gate A is the first of three (PARITY-PLAN section 6.5). What follows it, in that document's terms: gate B, GIL on supported as
an option (W4/W5 are already in by L7.3; the extension list published; the GIL-on lanes; `Atomics` on ordinary objects behind
its own option, decision 2); gate C, GIL off supported (everything in PARITY-PLAN Parts 5 and 6; the constant
`gilRemovalPreconditionsMetValue` flips). Neither has a date in this plan. What the merge changes for them: the flag-on
lanes run in the embedder's CI from then on (L7.4), every later round lands as an ordinary pull request against `main`
instead of growing a branch, and the ledger's script keeps every such pull request honest about what it makes a flag-off
process execute.

The rule that protects gate A afterwards: **a change to the threads work may not add an ungated hunk without a ledger
row**, checked in CI by the generator of L3.1; and the flag-off quiet pass joins the embedder's engine-bump checklist with
the bar of D7.
