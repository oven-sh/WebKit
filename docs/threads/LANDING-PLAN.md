# Landing Plan: Safety and Performance

This document lists the work that remains before the shared-memory threads
work (`THREAD.md`) can land on `main`. Two questions decide whether it lands:

1. **Safety.** Does it work, with the flag off, with the GIL on, and with the
   GIL off?
2. **Performance.** Does ordinary single-threaded code get slower when the
   flag is off?

Each section below says what is known today, what is missing, and what
"done" means.

## Current state

The branch `sosuke/threads` is the squashed threads work (oven-sh/WebKit#249),
rebased in the tenth round onto `main` at `cf1b36ec8703` (the WebKit commit
Bun pins; the ninth round's base was `dfd696443b9b`, the eighth's
`2e2aa2290fac`, and before that `491b5cc236e9`), plus the review fixes from
oven-sh/WebKit#549 and the fixes the rebases needed. The tenth round is one
commit on top of the ninth's.

The table below is the state before the safety round of 2026-09-02. For the
state after it, see the "Results" sections at the end of Part 1 (one per
round; the tenth, 2026-09-14, is the latest).

The results in this table come from one configuration: Linux x86-64, Debug,
ASAN, `-DPORT=JSCOnly`, the `build.ts` flags. Nothing had been measured in
Release, under TSAN, on macOS, on arm64, on Windows, or inside Bun.

| Suite | Mode | Result |
|---|---|---|
| `Tools/threads/run-tests.sh` | GIL on (`--useJSThreads=1`) | 153 pass, 0 fail, 2 skip |
| `Tools/threads/run-tests.sh` | GIL off (see below) | 148 pass, 4 fail, 3 skip |
| `Tools/threads/run-tests.sh --cve` | GIL on | 46 pass, 19 fail |
| `Tools/threads/run-tests.sh --cve` | GIL off | 58 pass, 4 fail, 3 skip |
| `JSTests/stress`, every 40th file | flag off | 142 of 143 pass |

GIL off means `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1
JSC_useThreadGIL=0` in the environment.

Every failure in the table also fails, the same way, on a Debug build of
oven-sh/WebKit#549 (the tree before the rebase). So none of them comes from
the rebase, but they still block landing. The one `JSTests/stress` failure
(`re-execute-error-module.js`) is a Bun change to a module error message and
fails on `main` too.

## Part 1: Safety

### 1.1 Known failures

Fix or explain each of these. A test that is wrong is fixed in the test, with
the reason in the commit message. A test that only fails in Debug is still a
bug: an assertion that a race can trip means the code does not hold the
invariant it asserts.

#### Default corpus, GIL off

| Test | Symptom | Notes |
|---|---|---|
| `objectmodel/typedarray-view-conversion-vector-length.js` | `BlockDirectory::assertIsMutatorOrMutatorIsStopped` from `MarkedSpace::size()` | A heap walk while other mutators run. |
| `vmstate/regexp-atom-retained-across-delete-all-code.js` | `MarkedSpace::stopAllocating` assertion from `Heap::deleteAllCodeBlocks` | `HeapIterationScope` on the shared heap without a stop-the-world. |
| `jit/call-link-record-pin-dying-caller.js` | Same assertion from `Heap::globalObjectCount` (`$vm.globalObjectCount()`) | Same class as the row above. |
| `jit/osr-entry-must-handle-values-generator-threads.js` | `InternalFunctionAllocationProfile::createAllocationStructureFromBase` assertion, 9 of 16 runs under load | Two threads miss the profile and race to fill it. |

The first three are one class: a `HeapIterationScope` (or a directory walk) on
the shared heap while other clients run. Decide once whether these callers
must stop the world or must not run GIL-off, and apply that to every
`HeapIterationScope` user. `git grep HeapIterationScope` lists them.

#### CVE suite

Timeouts (exit 124) at the runner's 120 s limit, GIL on:
`mc-code-deferred-fire-stale-window`, `mc-df-arraycopy-relabel`,
`mc-df-ta-detach-resize`, `mc-df-ta-sort-inplace`, `mc-df-wasm-compile-race`,
`mc-init-butterfly-grow-slack`, `mc-init-cloned-arguments-specials`,
`mc-init-direct-arguments-override`, `mc-life-wasm-grow-relocate`,
`mc-lock-stop-vs-park`, `mc-spec-timer-capability`, `mc-tear-date-cache`,
`mc-tear-typedarray-detach-grow-shrink`, `mc-val-fire-vs-link`.

First rerun these on a Release build. A test that passes in Release and only
runs out of time in Debug+ASAN needs a smaller iteration count in Debug
builds, not a longer timeout. A test that also hangs in Release
is a deadlock or a lost wakeup: take a backtrace of every thread while it
hangs.

Other failures:

| Test | Mode | Symptom | First step |
|---|---|---|---|
| `mc-aint-poll-resume-stale-elided.js` | both | ASAN SEGV | Real crash. Symbolize and find the owner of the address. |
| `mc-df-arraycopy-relabel.js` | GIL off | `RangeError` from `TypedArray.prototype.set` | Decide whether a racing writer may change the source length here. If yes, the test is wrong. If no, the length read is. |
| `mc-life-creator-thread-dies.js` | GIL off | `TypeError: Buffer is already detached` | Find who detaches the buffer when the creating thread exits. |
| `mc-gc-weakgcmap-registry-vs-prune.js` | both | `ReferenceError: $vm is not defined` | The test lacks `//@ requireOptions("--useDollarVM=1")`. |
| `mc-jit-delete-reuse-stale-offset.js`, `mc-jit-double-relabel-stale-shape.js`, `mc-jit-ta-resize-hoisted-base.js` | GIL on | The writer never leaves its `idle` state | These races need the GIL off. Confirm, then mark them GIL-off only. |

Done when both suites pass in Debug and Release, in both modes, with no test
marked expected-fail without a written reason.

#### Status (2026-09-02)

Every failure above is fixed or explained. The changes are in the working tree,
not committed. Results for the whole corpus are in "Results" at the end of
Part 1.

The default corpus, GIL off:

| Test | Cause | Fix |
|---|---|---|
| `typedarray-view-conversion-vector-length.js`, `regexp-atom-retained-across-delete-all-code.js`, `call-link-record-pin-dying-caller.js` | Heap walks (`HeapIterationScope`, `MarkedSpace::size()` and `objectCount()`) while other threads allocate. | `Heap::runWithOtherClientsStopped` runs a walk with the other threads stopped (inline when the world is already stopped). Every JSC user of `HeapIterationScope` uses it. `VM::deleteAllCode` and `deleteAllLinkedCode` run their work in a stop window, after the JIT plans finish, and requeue while a thread is inside JS. `MarkedSpace::size()` and `objectCount()` take the exclusive slow-path lock. New test: `vmstate/heap-walk-while-threads-run.js`. |
| `osr-entry-must-handle-values-generator-threads.js` | Two threads fill one `InternalFunctionAllocationProfile`. | With the GIL off, the fill takes the cell lock, and a thread that finds a matching structure uses it. |

The CVE suite:

| Test | Cause | Fix |
|---|---|---|
| The 14 GIL-on timeouts, and the three `mc-jit-*` tests | Each test spins on shared state. With the GIL on, a thread runs until it blocks, so the spinning thread never lets the other one run. A backtrace of every thread showed one thread in JS and the others in `JSLock::lock`, in Debug and in Release. | The tests need the GIL off. They are marked with the new `//@ threadsRequireGILOff` directive, and the runner skips them with the GIL on. Five tests in the default corpus had the same cause and got the same mark. |
| `mc-aint-poll-resume-stale-elided.js` | A real crash. An OSR exit can park for a stop-the-world. When the thread resumes, it walks its stack from its top call frame, and the exit had not set it. `mc-code-deferred-fire-stale-window.js` crashed the same way in Release. | `OSRExitGenerationLocker` sets the top call frame to the exiting frame. The LLInt OSR entry slow paths do the same with the GIL off. |
| `mc-df-arraycopy-relabel.js` | The test was wrong. A writer that grows the source array between the length read and the copy makes `set` throw `RangeError`, as the spec says. | The test accepts `RangeError`. |
| `mc-life-creator-thread-dies.js` | The test was wrong. The main thread transfers the buffer while the readers run, so a view made after the transfer throws `TypeError`, as the spec says. | The test accepts the `TypeError`. |
| `mc-gc-weakgcmap-registry-vs-prune.js` | The test lacked `--useDollarVM=1`. | Added. |

Failures found in Release, which had not been run before:

- `i08-named-vs-indexed-first-install.js` hung. The compiler moved the load of
  a nuked StructureID out of a spin loop. `JSCell::structureIDConcurrently()`
  is a relaxed atomic load, used by every such loop.
- The link failed: `DeferredWorkTimer::Ticket::cancel()` was `inline` in the
  `.cpp` and exported in the header.

Failures found when the corpus was widened. The runner had run only some
directories. It now runs every directory under `JSTests/threads`:

- `semantics/frozen-seal-race.js`, GIL off, about one run in seven: a
  property that a delete removed came back with no value. `Object.seal` read
  the structure, planned the sealed one, and stored it with no check, so it
  could undo a delete that landed in between. The audit of every such store
  found eleven more (a prototype change, a dictionary conversion, a private
  brand, `Object.assign`, and others). They all go through
  `JSObject::publishStructureOnlyTransitionConcurrently` now, which re-checks
  the structure and the property table under the cell lock. New tests:
  `objectmodel/seal-freeze-vs-delete-add-race.js` (30 of 30 runs failed
  before), `objectmodel/structure-only-transition-races.js` (24 of 30).
- With the flag on, and one thread, a second delete from an object with many
  properties aborted, in Debug and in Release. Bun's test preload does this,
  so every Bun test aborted with the flag on. The threads delete path assumed
  that a cacheable dictionary's property table is always pinned. It is not: a
  delete from a cacheable dictionary makes a cacheable dictionary with an
  unpinned copy of the table, as upstream does. An unpinned table is never
  edited in place, so the path now accepts it and re-checks the pin state
  under the cell lock. New test: `objectmodel/cacheable-dictionary-delete.js`.
  The threads corpus had no object with more than 64 properties and two
  deletes, and neither had the JSC suites with the flag on, which had not been
  run.
- `lifecycle/create-basics.js`: the test was wrong. It counted with a plain
  `++` from eight threads, which can lose an update with the GIL off, as on a
  SharedArrayBuffer. It uses `Atomics.add` now.
- `w16-c1-prevent-collection.js`, GIL off, every run, in Debug and Release,
  and before this work too: a heap snapshot kept a node whose cell had died.
  The snapshot builder installs itself as the heap analyzer and runs one
  `collectNow` inside a `PreventCollectionScope`, and it assumes that runs one
  cycle. In the shared heap, that call also served the tickets that other
  threads' `gc()` calls got while the gate was up, and it kept going while they
  asked: one call ran about 50 cycles. A cell marked in one of them and dead in
  a later one was never pruned, because the new snapshot is not in the prune's
  chain until the build ends. Fixed in `Heap.cpp`: while the gate is up,
  cycles run only up to the holder's own ticket. Tickets granted before it are
  served first, with the analyzer uninstalled; tickets granted after it wait
  for `allowCollection()`. This also makes "prevent collection" hold for the
  other threads, which it did not.

### 1.2 Test coverage still missing

Run each of these and file the failures in this document.

- **The full JSC test suites, flag off.** `Tools/Scripts/run-javascriptcore-tests`
  (stress, microbenchmarks, `mozilla`, `test262`), Debug and Release. This is
  the check that the branch changes nothing for code that never sets the
  flag. Compare against `main`, not against an absolute pass count.
- **The same suites with `--useJSThreads=1`, GIL on.** Nothing spawns a
  thread, so every difference from the flag-off run is a bug in a flag-on path
  that single-threaded code takes: tagged butterflies, handler ICs, per-thread
  state.
- **The same suites GIL off.** Same reasoning, for the GIL-off paths.
- **TSAN.** `TSAN.md` describes the build and the triage. Run `JSTests/threads`
  and the CVE suite. Every report is either fixed or listed in
  `TSAN-TRIAGE.md` with the reason it is benign.
- **The amplifier and the fuzzers.** `AMPLIFIER.md` and `FUZZ.md`. Run them
  for hours, not minutes, in both GIL modes.
- **Bun.** Build Bun against this branch and run its test suite with the flag
  off. Then run it with the flag on and no threads. Bun is the first user, and
  it uses `USE(BUN_JSC_ADDITIONS)` paths that the JSC tests do not reach.
- **Other platforms.** macOS arm64, Linux arm64, and Windows x64. arm64 is
  weakly ordered and is where a missing fence shows up. The ARM64 paths
  (address dependencies, TLS loads) have not run at all since the rebase.

### 1.3 Audit upstream code added since the merge base

The rebase brought in about 5,800 upstream commits. Some of them added code
that assumes the old object model, and it compiled without complaint. The
rebase found these cases only because a test happened to reach them:

- `USE(JSVALUE64)` no longer existed, so 280 threads blocks compiled out.
- New data IC slow paths were never emitted from the FTL.
- A new LLInt diagnostic read a property slot before the threads re-read.
- New typed array sorts wrote shared lanes in place.

There are certainly more. Audit the upstream diff since the merge base
(`5851d4722e46..origin/main`) for:

- direct butterfly loads in JIT or LLInt code that do not go through the
  tagged-butterfly helpers,
- new `switch` statements over node types or cache types that the threads
  code extends,
- new `HeapIterationScope` users,
- new caches on the VM or on a cell that a second thread could read,
- new uses of `Options` or macros the threads code changes.

Write the result in a new `AUDIT-upstream-since-rebase.md`, in the same form
as the other `AUDIT-*.md` files.

#### Status (2026-09-02)

Done. `AUDIT-upstream-since-rebase.md` lists 39 rows. Its "Status after the
fixes" table gives each row's fix and test. Open rows: OM-9 (the DFG
element-write gap), VM-9 (a `node:vm` timeout terminates every thread), VM-13
and VM-14 (semantic only).

The `bun:ffi` rows (VM-8, OPT-1, OPT-2) are closed by refusal, not by a fix:
with the GIL off, only the main thread can create or call an FFI function, and
the FFI JIT paths are off. A spawned thread gets a TypeError. This is a user
visible limit, and it needs a decision before landing.

### 1.4 Review the async generator port

Upstream moved the async generator driver to C++ and made it follow the
current spec. The GIL-off claim from oven-sh/WebKit#249 no longer applied, so
it was ported (commit "Serialize async generator requests across threads when
the GIL is off"). The protocol is described in `JSAsyncGenerator.cpp`. It
needs a review by someone who has not seen it, and a stress test with several
threads calling `next()`, `return()` and `throw()` on one generator.

#### Status (2026-09-02)

Done. The review is in `REVIEW-async-generator.md`. It found no error in the
protocol. Its findings were about code around it:

1. A `for await` body could be resumed on a second thread before it suspended.
   Not reproduced. Fixed: with the GIL off, `for await` never uses the fused
   driver, in the LLInt slow path and in the DFG.
2. `VM::syncResumeCallCache()` was shared by all threads. Reproduced. Fixed
   (audit row VM-1).
3. A thread read the variables of an earlier activation. Reproduced. The cause
   was audit row HW-1, found by bisecting; fixed.
4. Two threads filled one allocation profile. Fixed (section 1.1).
5. The protocol comment left out one invariant. Fixed.
6. The runner did not run `semantics/`. Fixed: it runs every directory now.

The stress tests are `semantics/async-generator-multithread-requests.js` and
`semantics/async-generator-multithread-for-await.js`.

### Results (2026-09-02)

All runs are on Linux x86-64. The tree is the branch plus the uncommitted
fixes listed in section 1.1 and below. The scripts and logs are not in the
tree. The corpus and the last TSAN round ran on the final tree. The other
runs are long, so they ran once, on the tree of that moment, and each rerun
after a fix covered only the failing tests.

**Threads corpus** (`Tools/threads/run-tests.sh`), on the final tree:

| Build | Default, GIL on | Default, GIL off | CVE, GIL on | CVE, GIL off |
|---|---|---|---|---|
| Release | 257 pass, 0 fail, 22 skip | 272 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |
| Debug (ASAN) | 257 pass, 0 fail, 22 skip | 272 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |

Every failure in the "Current state" table above is fixed. The counts differ
from that table because of the new tests.

**JSC suites, flag off** (`run-javascriptcore-tests`, Release, against `main`
at the merge base): the same pass and fail lists (97,054 pass, 579 fail on
both). The only differences are FFI tests in `ftl-eager-no-cjit` that fail
with "failed to allocate executable memory". They fail on `main` too, and a
rerun fails different files.

**JSC stress tests, flag on, GIL on** (`JSC_useJSThreads=1`, nothing spawns a
thread): 302 failures that `main` does not have. After the fixes, 275 remain,
and each is by design or is not a threads bug:

- 234: the test sets `forceEagerCompilation` or runs the profiler, and the
  flag refuses that (`useJSThreads requires useConcurrentJIT`).
- 34: `Atomics.store` on a plain object succeeds, because this branch extends
  `Atomics` to object properties (SPEC-api section 4.5).
- 2: the sampling profiler shows no `wasm-stub` frame, because JS-to-wasm calls
  take the cold path with the flag on (AB-15).
- 2: `has-indexed-property-*-ftl.js` checks that the FTL ran within
  `testLoopCount`. The flag forces concurrent compilation, and the FTL code
  runs later. Flag off with `--useConcurrentJIT=true` fails the same way.
- 2: `class-subclassing-function.js` fails on `main` in every configuration
  (a hash collision in the code cache, from `SourceCodeKey` under
  `USE(BUN_JSC_ADDITIONS)`).
- 1: the FFI executable-memory failure above.

The 27 that were fixed are four bugs, all in flag-on paths that one thread
takes:

- The FTL handler IC for `delete` and `instanceof` had no unwind entry, so an
  exception skipped the `catch`. The flag forces the FTL handler IC on; `main`
  never runs it.
- Four indexing transitions fired their deferred watchpoints before the new
  structure was published, which invalidated the array iterator watchpoint.
- `CodeBlock::setupWithUnlinkedBaselineCode` skipped `capabilityLevel()`, so a
  baseline call could reach a `CRASH()`. This came from the branch, and also
  fails flag off with `--useConcurrentJIT=true`.
- A DFG type filter gave an empty type for a final object checked against
  `JSObject`. This is a `main` bug, which the flag exposes.

**JSC stress tests, GIL off** (nothing spawns a thread): 1,656 configurations
failed that pass on `main` and with the GIL on. After the fixes, 342 remain,
and each is by design or a flaky FFI test:

- WebAssembly is off with the GIL off (117), and the sampling profiler is
  refused (170).
- The FFI IC stub and its DFG and FTL paths are off with the GIL off (16,
  audit rows VM-8, OPT-1, OPT-2).
- `typedarray-sort-out-of-memory.js` (16): with the GIL off, `sort()` copies
  the array first (CVE-AUDIT-RESULTS.md, A2), and the 4 GB copy fails before
  the "already sorted" check.
- `taintedness-tracking.js` (16): the taint flag stays set with the GIL off,
  which over-reports. The code says so; no doc does.
- `baselinejittrue` (1): turning wasm off sets `useLLInt=true`. `main` with
  `--useWasm=0` fails the same way.
- Output-comparison tests print one more line, the "disabling useWasm"
  warning. They pass when that line is removed. Six configurations of FFI
  tests under `--useExecutableAllocationFuzz` fail at random, more often with
  the GIL off; not investigated.

The fixed failures were three bugs:

- Some runtime code read the VM's `topCallFrame`, which is never written with
  the GIL off (the current thread's copy is in its VM lite). Error messages
  lost their `(evaluating '...')` part, `f.caller` and `f.arguments` were
  wrong, and one test crashed. About 700 configurations.
- `VM::cancelTermination` cleared only the VM's trap word, and a termination
  request sets a word in every thread's lite too. So a cancelled request still
  terminated the next code that ran on the thread. Bun's `node:vm` timeout
  uses this path.
- With the shared heap (GIL on or off), object literals got one less inline
  slot, because the server's allocator table is empty by design and the
  allocation profile skipped the size-class rounding. Not wrong, but slower.


**Bugs found by the GIL-off runs**, all fixed, all older than this work:

- The JIT decoded a structure ID without clearing the nuke bit. A transition
  on another thread nukes the ID while it publishes a new butterfly, and a
  nuked ID still names the old structure (`StructureID::decode` clears the
  bit). The LLInt, baseline, DFG and FTL added the bit to the address, so they
  read the structure one byte off. An `instanceof` then returned false, or
  crashed (`semantics/ic-instanceof-vs-transition.js` crashed in 7 of 120
  Release runs; the new `jit/instanceof-nuked-structure-id.js` returned false
  in 40 of 40). The JIT tiers clear the bit only with the flag on; the LLInt
  clears it always, one `and`, which changes nothing flag off.

- With `--destroy-vm --collectContinuously=true`, the process hung at exit. A
  collection request was still pending when the VM was destroyed, and the
  destroying thread, the last mutator, waited for it after giving up its heap
  access. `Heap::prepareForVMDestruction` now serves it first.
- With a zero pause budget (`--minimumGCPauseMS=0 --gcPauseScale=0` and a
  low `--maximumMutatorUtilization`), a collection never finished. The
  marking deadline had always passed, and the shared-heap fixpoint, which does
  not resume the mutator, retried with the same deadline. It now drains to the
  end when it will not resume.

**TSAN** (the full-JIT configuration of the earlier campaign, `TSAN.md`).
Report files by round:

| Round | Corpus, GIL on | Corpus, GIL off | CVE, GIL on | CVE, GIL off |
|---|---|---|---|---|
| 1 (before the fixes) | 75 | 100 | 21 | 39 |
| 2 | 1 | 5 | 0 | 3 |
| 3 | 1 | 5 | 0 | 1 |
| 4 | 0 | 6 | 1 | 2 |
| 5 | 0 | 3 | 0 | 1 |
| 6 | 0 | 7 | 0 | 1 |
| 7 | 0 | 2 | 0 | 1 |
| 8 | 0 | 2 | 0 | 2 |
| 9 (the final tree) | 0 | 1 | 0 | 2 |

A run reports only the races whose timing it hits, so each round shows a
different subset, and the counts do not fall in a straight line. In every
round from 4 on, one "CVE, GIL off" file is the `mc-grow-buffer-storm.js`
crash in "Open items", not a race report. In round 9, the "Corpus, GIL off"
file is the open `m_numValuesInVector` group, and the other "CVE, GIL off"
file is one more report of the CodeBlock publish that the LLInt hides, which
is suppressed now. TSAN-RESULTS.md, "Post-rebase campaign", has the details.

Each round's reports were triaged into three classes: upstream races that
also exist flag off (suppressed, with a one-line reason each), accesses that
TSAN cannot pair because one side is in JIT code or is an address dependency
(suppressed, or made acquire under `TSAN_ENABLED` only, so other builds are
unchanged), and real ordering bugs (fixed). The real ones: a pointer loaded
relaxed and then used, with the publisher on another thread (the rare data of
`CodeBlock` and `FunctionRareData`, the client heap's lazy `IsoSubspace`s, the
poly-proto watchpoint box, and the property table of a structure that another
thread had just published); the direct eval cache and the promise `isHandled`
read in `didExhaustMicrotaskQueue`, which were not locked; and the Map and Set
tables (audit row OM-10). On x86 the first group are compiler-ordering bugs
only; on arm64 they can read uninitialized memory.

**Amplifier** (`AMPLIFY_RUNS=10 run-tests.sh --amplify`, Release, on the tree
before the structure-ID and GIL-off fixes above, ten seeds per test, both GIL
modes): no crash and no hang. The harness flags 9 tests in each mode as
divergent, and each of them prints a timing, a count, or an address
(`scaling/`, `heap-bench-allocation.js`, `int-gate-stop-budget.js`,
`lock-fairness.js`, `dump-registers-*.js`). Ten seeds per test is a smoke run,
not the hours that section 1.2 asks for.

**Bun.** Bun built against this branch (after the two Bun changes listed below)
and ran `test/js/bun/{jsc,ffi,util}`, `test/js/node/{vm,util,worker_threads}`
and `test/js/web/{timers,workers}` (Debug, ASAN). That Bun was built partway
through this round, after the dictionary fix and before the JIT, GIL-off and
TSAN fixes below it; it was not rebuilt.

- Flag off: the failures are the same as on the Bun binary built before this
  work (timeouts under load, and a stack overflow in Bun's `ConsoleObject`,
  `bun-inspect.test.ts`, which is a Bun bug).
- Flag on, GIL on: the same failures, plus one:
  `worker-terminate-lifetime.test.ts` ("terminate() while a worker's
  Bun.connect() open is firing"). The child process stops making progress. Its
  main thread accepts connections in a loop and never returns to the event
  loop, so timers and worker messages never run. uSockets accepts until
  `EAGAIN` with no limit, and with the flag on the JS that handles each
  connection is slow enough (its property stores give up their ICs) that the
  worker reconnects faster than the main thread drains. A slower reconnect
  makes it pass. Flag off, a 1 ms handler still passes, so the slow handler is
  not the whole story; not root-caused.
- Before the dictionary fix (section 1.1), every flag-on run aborted at
  startup.


### Results, second round (2026-09-03)

The second round worked through the safety entries of "Open items", in their
order, and fixed what its runs found on the way. Each fix that the jsc shell
can reach has a test in `JSTests/threads/` that fails on a build without the
fix and passes with it. The entries without a test say so. The counts below
are from Release, GIL off, unless the entry says otherwise. All runs are on
Linux x86-64. The corpus and TSAN ran on the final tree. The JSC suites, Bun,
and the amplifier ran on the tree before the last fix (the watchpoint install,
two functions), which the corpus and TSAN then covered again.

**Fixes.**

- **A typed array store wrote through a null base, GIL off.** This was the
  entry that blocked landing. `JSArrayBufferView::detachFromArrayBuffer` now
  keeps `m_vector` and sets `m_detachedKeepingVector` before it publishes the
  zero length, as `ArrayBuffer` keeps its base word. The quarantine keeps the
  mapping until the next stop, so a racing access pairs the old length with
  the old base, or a length of 0 with any base. A null base still means
  detached, so `isDetached()` is "the byte is set, or the base is null". The
  JIT tests that used the null base test both (the resizable-view bounds
  check, `CheckDetached` in the DFG and the FTL, the `DataView` `byteLength`
  IC, and the FFI argument paths). C++ keeps the contract that `vector()` is
  null for a detached view, because Bun, N-API and the FFI use that. Bun has
  about ten such sites (its `Buffer` constructor, SQLite binding, and
  `napi_get_typedarray_info`). The hoisted case was already covered: a GIL-off
  `CheckTraps` clobbers `MiscFields`, so no length and base pair survives a
  poll. `arrays/typed-array-detach-keeps-base-gil-off.js` runs with
  `GIGACAGE_ENABLED=0` (a new `threadsEnv` directive) and failed 20 of 20
  before. `cve/mc-grow-buffer-storm.js` with `GIGACAGE_ENABLED=0` failed 2 of
  48 before and 0 of 48 after, and it no longer crashes under TSAN. With the
  Gigacage on, the store did not crash: it wrote at 4095 bytes past the start
  of the primitive cage, into whatever buffer was there.
- **Trap handling jettisoned code that `linkFor` was about to link.** Found by
  the corpus (`cve/mc-val-tid-reissue-false-owner.js`, 7 to 10 aborts in 300
  to 400 runs, on the tree before this round too). `linkFor` defers traps
  because it links a call to a CodeBlock that it has already written into the
  callee frame. `VMTraps::handleTraps` returned early for the deferral, but its
  scope exit still ran the on-stack jettison when the heap-fact epoch had
  moved, and a conductor moves the epoch at the edges of its request, with no
  park on this thread. The jettison found the callee's CodeBlock at the top of
  the stack, and `noticeIncomingCall` hit `RELEASE_ASSERT(!m_isJettisoned)`.
  The deferral check now comes before the epoch sample. A deferred call parks
  nowhere, so it has nothing to jettison, and the park that ends the deferral
  sees the in-window bump. `giloff-link-call-defers-on-stack-jettison.js`
  failed 14 of 40 before. `mc-val-tid-reissue-false-owner.js` passes 400 of
  400.
- **The remaining `topCallFrame` and `topEntryFrame` readers.** `Debugger.cpp`
  reads the frames of the calling thread (`group3Primitives()`). The debugger's
  pause state is the carrier's, and the hooks that read these frames run only
  on the carrier. `breakProgram` is now a no-op on a spawned thread, like the
  other hooks, because `console.assert` reaches it from any thread. No test:
  the jsc shell has no debugger that pauses.
- **The shadow chicken with the GIL off.** The collector passed the VM-level
  `topCallFrame`, which is not written GIL-off, to `ShadowChicken::update`. That
  update only prunes the shadow stack (the log and the stack are marked either
  way), so it is skipped GIL-off. The larger problem was that the log and the
  shadow stack are one per VM, and every thread wrote them once a debugger was
  attached. Now they belong to the threads that are not spawned. A spawned
  thread's packet goes to a per-thread slot that nothing reads
  (`ShadowChicken::acquirePacketGILOff`, used by the LLInt slow path and the
  three JIT tiers with the GIL off), and `iterate()` shows a spawned thread its
  machine frames. `giloff-shadow-chicken-spawned-threads.js` aborted 20 of 20
  before (a `Vector` index out of range in the shared stack).
- **`HeapHolderFinder`** needed no change. Its only caller, `queryHolders`,
  creates it inside a `PreventCollectionScope`, as upstream does. The entry in
  "Open items" read the constructor alone. The three heap analyzers
  (`HeapSnapshotBuilder`, Bun's V8 snapshot, `HeapHolderFinder`) all run inside
  the scope.
- **TSAN: `ArrayStorage::m_numValuesInVector`.** With threads, every C++
  writer holds the cell lock. The four density checks in
  `putByIndexBeyondVectorLengthWithArrayStorage` and
  `putDirectIndexBeyondVectorLengthWithArrayStorage` now read the count under
  it too. JIT code writes the count without the lock only on the owner thread
  while the storage is not shared-written, and a foreign writer sets that bit
  first. `arrays/array-storage-density-count-gil-off.js` reported in 3 of 3
  TSAN runs before, and in none after.
- **TSAN: the six raw double accesses in `JSObject.cpp`.** Five are safe: a new
  fragment before it is published, two flag-off paths, and two conversions
  inside a stop. The sixth, the hole store of a delete on a segmented
  butterfly, runs at the same time as readers on other threads, and is a
  relaxed atomic now. `arrays/segmented-double-delete-gil-off.js` reported in 2
  of 2 TSAN runs before, and in none after.
- **JIT-emitted structure transitions.** No tier emits one flag-on, GIL on or
  off. Each transition is refused where it is created (`Repatch.cpp` for the
  ICs, the LLInt cache fill, `handlePutById` and constant folding in the DFG),
  and the object model performs it. Four sites had no local check. The DFG
  and the FTL `PutStructure` now assert that the flag is off, the DFG parser
  routes a delete hit and a private brand to the generic node, and the
  private-brand handler thunk jumps to its slow path. No test: none of these
  can run.
- **The DFG and FTL element-write gap (audit row OM-9).** A foreign store to an
  array whose word is not shared-written must set the bit first, and the DFG
  and the FTL stored with no write predicate. The owner then kept its lock-free
  copying resize, whose CAS succeeds when the bit is clear, so the store was
  lost. A `GetButterfly` that feeds an element store now runs the write
  predicate (`Graph::markButterflyLoadsThatFeedElementWrites`). A foreign store
  to such a word exits, and the generic path sets the bit.
  `arrays/dfg-foreign-element-store-sets-shared-write.js` failed 20 of 20
  before, and passes with the DFG off.
- **A `node:vm` timeout terminated every thread (audit row VM-9).** A deadline
  now terminates the thread that added it. Firing only that thread's trap word
  is not enough, because the loop checks of every tier poll the VM word. The
  bit goes into both words, and the other threads ignore the VM word's bit while
  no VM-wide termination is raised (`VMTraps::m_vmWideTerminationRaised`). The
  target retires the bit when it services the request, and
  `VM::cancelTermination` keeps another thread's pending request.
  `giloff-time-limit-terminates-one-thread.js` failed 10 of 10 before. The
  GIL-off JSC suite found a mistake in the first version of this fix
  (`stress/vm-termination-deadline.js`). The VM's request flag means that a
  thread has handled the termination, and the thread's flag was set when the
  request was made. So a limit that passed after its call had left script
  still counted as a timeout. A lite has a request flag and a handled flag
  now, and the test checks that case too. The watchdog stays VM-wide. Bun's `node:vm` code makes the request again for an
  enclosing run, and the Bun change listed below makes it for the current
  thread.
- **Bun's `JSC__IdentifierArray__setFromSlot` (audit row VM-12).** The decoder
  string table's `atomFor` rewrites a slot with no lock. JSC's callers hold the
  compilation lock, and Bun's module-info path did not. `atomForSlot` takes the
  lock now (it is recursive). No test: the jsc shell has no module-info path.
- **Tier-up raced with a late install, GIL off.** Two bugs, found by a corpus
  flake that predates this round: `semantics/stack-overflow-per-thread.js`
  aborted in about 1 of 400 GIL-off runs, in
  `setOptimizationThresholdBasedOnCompilationResult`, and crashed in
  optimized code less often. Both let a lower tier replace a higher one.
  - The function prologues (`entry_osr_function_for_call` and the three like
    it) tiered up the executable's current CodeBlock, as upstream does. A
    caller can enter the LLInt prologue through a call link that another
    thread relinks a moment later, when it installs the optimized
    replacement. The prologue then gave the optimized CodeBlock baseline code,
    in place of its own. Now the prologue tiers up its frame's CodeBlock, and
    only when that one is current. A `RELEASE_ASSERT` in
    `jitCompileAndSetHeuristics` keeps optimized code from getting baseline
    code.
  - `BaselineJITPlan::finalize`, and the LLInt's shortcut to shared baseline
    code, publish the baseline code (the JIT type) before `installCode` takes
    the compilation lock. In between, another thread can run the baseline
    code, tier up, and install the optimized replacement, and the late install
    then put the baseline CodeBlock back. `installCode` now skips an install
    whose CodeBlock is the alternative of the optimized one in the slot. A
    jettison installs the alternative on purpose, with a reason, and is not
    skipped.

  `giloff-prologue-tiers-up-frame-code-block.js` (32 threads, a new function
  per round) failed 13 of 20 runs one at a time and 20 of 20 four at a time
  before the fixes, 3 of 30 and 1 of 30 with the first fix only, and 0 of 40
  and 0 of 40 with both. `stack-overflow-per-thread.js` failed 15 of 6,000
  runs before, and passes 2,000 of 2,000.
- **A collection under a cell lock, GIL off.** Found by the GIL-off JSC
  suite: `stress/intl-having-a-bad-time.js` (in `ftl-eager`, which sets
  `--collectContinuously`) stopped making progress in 1 of about 200 runs.
  `defineOwnIndexedProperty` adds to the sparse map under the object's cell
  lock, and the add reports the map's new capacity to the heap. With the GIL
  off, that report can conduct a collection on this thread, which waits for
  its markers, and a marker that visits the object waits for the lock. The
  locked region now holds a `DeferGC`, as
  `enterDictionaryIndexingModeWhenArrayStorageAlreadyExists` does, so the
  collection runs after the unlock.
  `objectmodel/sparse-define-collects-outside-cell-lock.js`, which spawns no
  thread, stopped in 8 of 40 runs before, and in 0 of 40 after. The intl test
  passes 300 of 300.

  The suite then stopped once more, in `stress/redefine-property-writable.js`,
  from a second shape of the same bug. A dictionary delete
  (`deletePropertyNamedConcurrent`) allocates nothing under the cell lock, but
  the structure's `remove` takes a `GCSafeConcurrentJSLocker`, and that
  locker's `DeferGC` ended under the cell lock. The release of a `DeferGC` can
  conduct a pending collection. The delete holds a `DeferGC` across the lock
  now. `objectmodel/dictionary-delete-collects-outside-cell-lock.js` stopped in
  13 of 16 runs before, and in 0 of 32 after. A search of the other cell-locked
  regions, for both shapes, found one more that may be reachable with the GIL
  on and a shared heap (`flattenDictionaryStructureImpl`), which has the same
  fix. `FunctionRareData` allocates under its own lock on purpose: no marker
  takes that lock, and its waiters poll for stops.
- **`butterfly()` on a segmented word.** Found by the corpus:
  `objectmodel/json-stringify-array-race.js` aborted once, GIL off, with
  `--verifyConcurrentButterfly`. A read past the end of a contiguous array
  takes the slow path (`getByVal` in `JITOperations.cpp`, twice, and its LLInt
  twin), which reads the length through `butterfly()`. That accessor must not
  decode a segmented word, and another thread can segment the array. With the
  verifier off, the length came from the memory just before the spine. The
  three sites read the length through the word now (`getArrayLength()`), as
  `directPutByVal` already does. `arrays/segmented-out-of-bounds-read.js` and
  its LLInt twin failed 10 of 10 runs before, and pass 20 of 20.

  A search for the same mistake found more, and each is fixed the same way.
  On a segmented array or object, with the verifier off, these returned wrong
  results: `Object.assign` copied no indexed property, and `Array.from`
  returned an empty array.
  - Any use of a segmented object: the fast paths of `Object.assign`,
    `Object.entries` and `Object.values`, the spread of an arguments object,
    the fast clone of `Array.from`, `concat` and `toSorted`, the indexed length
    of a `for-in`, and the heap snapshot (`analyzeHeap` and `estimatedSize`).
    `objectmodel/segmented-indexed-copy-paths.js` and
    `objectmodel/segmented-array-read-paths.js` failed 10 of 10 runs before,
    and pass 10 of 10.
  - Races, fixed by reading, with no test: `JSArray::setLength` counted the
    elements through a second load of the word, `pushInline` decoded a second
    load, and DFG constant folding called `butterfly()` before its structure
    check.

  The audit has the list (PRE-7).
- **A watchpoint install asserted after a foreign transition.** Found by the
  amplifier: `gc-stress/watchpoint-storm.js` aborted in 5 of 400 amplified
  GIL-off runs, in `AdaptiveStructureWatchpoint::install`, from
  `DFG::Plan::finalize`. The plan checks that its conditions are still
  watchable and then installs the watchpoints, and another thread can
  transition the watched prototype in between. The install asserted
  watchability (`RELEASE_ASSERT`). With the flag on, it and
  `AdaptiveInferredPropertyValueWatchpointBase::install` return false instead,
  which every caller already handles: `reallyAdd` discards the compile as
  invalidated, and `fireInternal` jettisons. Flag off, the assert stays. After
  the fix, 0 of 400. The window is a few instructions, so no plain test hits
  it; a test written for it hit only under the amplifier, at about 1 in 80,
  which the existing storm test already does, so it was not kept.
- **TSAN: the property table's index vector.** The lock-free lookup
  (`Structure::getConcurrently`) reaches a rebuilt table's index vector
  through the vector word, which is an address dependency. TSAN saw no order
  between the lookup and the zero fill of the new vector. Under TSAN the word
  is now an acquire load and a release store, as the table pointer already
  is. Other builds keep the relaxed accesses. It reported in the corpus run,
  from `cve/mc-df-delete-reuse.js` and `cve/mc-val-multislot-clone.js`, and
  in 1 of about 50 reruns of the second.
  `objectmodel/property-table-rehash-vs-lookup.js` (eight readers, a small
  table rebuilt many thousands of times) reported in 5 of 5 TSAN runs before,
  and in none of 5 after.
- **TSAN: the park flag.** A thread that parks at a trap check released its
  heap access and then set `m_releasedByGCPark`, which a collector reads once
  every client has released access (`Heap::updateAllocationLimits`). The flag
  is set before the release now, and the release publishes it. This one is a
  real race in every build, of a value that only tunes the collector's
  schedule. `cve/mc-jit-delete-reuse-stale-offset.js`, eight runs at a time
  under TSAN, reported in 14 of 40 runs before, and 0 of 40 after. A test
  written for it did not report, so it was not kept.
- **TSAN: `WordLock`.** Under TSAN, a process with 20 or more threads that
  contend for one lock stopped making progress, in `WordLock::lockSlow`.
  TSAN's runtime takes a read lock for each acquire load of the word and the
  write lock for a CAS or a store, and readers get in while a writer waits, so
  the spinning threads keep out the thread that must release the queue lock.
  The spin loads are relaxed under TSAN now (TSAN-RESULTS.md has the
  details). Other builds are unchanged. `cve/mc-gc-thread-shell-finalizer-storm.js`
  timed out in the TSAN corpus, GIL on, and ran for more than 280 s alone. Now
  it runs in 3 s. The 32-thread test above, run 4 at a time under TSAN,
  stopped in every batch, and now passes 24 of 24.

**Corpus** (`Tools/threads/run-tests.sh`), on the final tree, with the fifteen
new tests:

| Build | Default, GIL on | Default, GIL off | CVE, GIL on | CVE, GIL off |
|---|---|---|---|---|
| Release | 272 pass, 0 fail, 22 skip | 287 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |
| Debug (ASAN) | 272 pass, 0 fail, 22 skip | 287 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |

**JSC suites** (`run-javascriptcore-tests`, Release, the same collections
as in the first round), on the final tree:

- Flag off: 580 failures, against 579 on `main`. The lists differ only in FFI
  tests in `ftl-eager-no-cjit`: six fail here and pass on `main`, and five do
  the opposite. Five of the six fail with an out-of-memory error from the
  FFI's executable memory, the failure that the first round describes, which
  fails different files in each run. The sixth
  (`ffi-callffi-was-compiled.js`) failed on `main` in an earlier run of the
  FFI tests.
- Flag on, GIL on (`JSC_useJSThreads=1`): 278 failures that flag off does not
  have, against 275 in the first round. Three are the same FFI failure (one
  then). One is `int8-repeat-in-then-out-of-bounds.js` in
  `ftl-no-cjit-no-inline-validate`, which expects a reoptimization within
  `testLoopCount`, like the two `has-indexed-property` tests. It fails in 23
  of 40 runs on the tree before this round. The other 274 are the by-design
  failures that the first round lists.
- GIL off (nothing spawns a thread): 928 failures that neither flag off nor
  the GIL on has. 583 are output comparisons that differ only by the
  "disabling useWasm" line, which the first round did not count. The other
  345 are the first round's 342 categories: WebAssembly (117), the sampling
  profiler (173), the FFI IC stub (14), `typedarray-sort-out-of-memory.js`
  (16), `taintedness-tracking.js` (16), `baselinejittrue` (1), and the FFI
  executable-memory failure (8). The suite found the two collections under a
  cell lock and the termination mistake above. On the final tree it runs to
  the end.

**TSAN** (the full-JIT configuration, `TSAN.md`), on the final tree: no
report and no failure, in the corpus and the CVE suite, in both GIL modes.
TSAN-RESULTS.md has each step of the round.

**Flag-off changes.** No fix changes what flag-off code does. These change
the code that runs flag off:

- `JSArrayBufferView::vector()` and `isDetached()` test a byte of the frozen
  config page (`g_jscConfig.gilOffProcess`), a predicted branch. The JIT
  emits the new detach test only in a GIL-off process. The view's size does
  not change: the new byte fills padding.
- `VM::hasTerminationRequest()`, `clearHasTerminationRequest()` and
  `hasPendingTermination()` add the same config test.
- `VMTraps::handleTraps` tests for deferred traps first. The tests it moved
  past have no effect flag off.
- The paths listed under "`butterfly()` on a segmented word" test
  `useJSThreads()` before they read the storage. Two of them changed shape:
  `JSArray::setLength` passes the butterfly it loaded to `countElementsIn()`,
  and `getEnumerableLength()` gets its butterfly from a lambda. Both give the
  same result flag off.
- Slow paths add one GIL-off test each: the LLInt's `entry_osr_function_for_*`
  and shadow chicken logging, `ScriptExecutable::installCode`,
  `DecoderStringTable::atomForSlot`, the debugger's `breakProgram`, and the
  collector's shadow chicken update. The LLInt's shadow chicken fast path
  tests the config byte, and it runs only with a debugger attached.
- `JSObject`'s density checks for array storage test `useJSThreads()`.
- The DFG and the FTL assert that `PutStructure` never compiles with the flag
  on, and the DFG parser tests the flag before it inlines a delete or a
  private brand. Both are compile-time work. The two adaptive watchpoint
  `install` functions test the flag; flag off they assert as before.

The `WordLock` and property table changes apply to TSAN builds only.

**Amplifier** (`AMPLIFY_RUNS=10 run-tests.sh --amplify`, Release, ten random
seeds per test per pass, the four corpus modes in parallel for three hours on
the tree before the watchpoint fix: 42 passes of the CVE suite GIL on, 37 GIL
off, 16 and 15 passes of the default corpus). No hang. The harness flags the
same output-divergent tests as the first round (timings, counts, addresses),
plus the `PASS (...)` summary lines of a few CVE tests, which print counts.
Real findings:

- 6 crashes (SIGABRT): 2 in `gc-stress/watchpoint-storm.js`, the watchpoint
  install above, fixed; 4 in `shared-objects/map-set-shared-writers.js`, GIL
  off, the stop-the-world watchdog ("failed to reach a stopped world within
  30 s", pending context "OM transition stop", one entered lite with heap
  access and no owner thread recorded). The pre-round build does the same (1
  of 80 amplified runs). Open.
- 17 exit-code divergences in two tests. `cve/mc-dos-waiter-table-storm.js`
  exits 3 with no exception in about 4% of amplified runs under load: the
  shell reports 2 of 3 async passes, so one `Atomics.waitAsync` promise chain
  never settles before the event loop drains. `jit/int-gate-fire-vs-execute.js`
  throws "every worker made progress across fires". Both reproduce at the same
  rate on the pre-round build, and neither reproduces at 60 runs on an idle
  machine. Open.

**Bun.** Bun built against the final tree (Debug, ASAN), with the two Bun
changes listed in the first round's results, and three more: the heap walks in
`JSEnvironmentVariableMap.cpp`, `BunDebugger.cpp` and `JSInspectorProfiler.cpp`
run inside `Heap::runWithOtherClientsStopped`, and `NodeVMRunTermination.cpp`
makes the request again for an enclosing run with
`notifyNeedTerminationForCurrentThread()`, because a `node:vm` deadline now
terminates one thread. None of these changes is in this tree. The same test
directories as in the first round ran with the flag off and on.

- Flag off: the same 14 failures as in the first round. One test that failed
  then passes now (`DOMJIT > TextDecoder.decode`, a timeout).
- Flag on, GIL on: the flag-off failures, and the one below.

- **The stall in `worker-terminate-lifetime.test.ts` is not a threads bug.**
  The child's main thread accepts connections in
  `us_internal_dispatch_ready_poll` (`bun-usockets/src/loop.c`), which accepts
  until `EAGAIN` and runs the JS `connection` handler for each socket before
  it accepts the next. The test's 4 workers keep 128 connections in flight,
  and a worker connects again once the main thread has accepted and ended its
  previous connection. So while the main thread takes longer for 128
  connections than a worker takes to reconnect, the accept queue never
  empties, and the loop never returns. Flag off, with the handler made slower
  by a busy wait, the child passes at 1 ms per connection and stops at 3 ms.
  The unchanged handler accepts about 350 connections a second flag on, and
  about 1,900 flag off. With the loop bounded to 64 accepts per readiness event (the
  listen socket is level-triggered, so the rest are reported on the next
  iteration), the test passes flag on, and the child's run takes 36 s instead
  of 56 s flag off. That change belongs in Bun and is not in this tree.
- **The handler is slower flag on because property adds are not cached.** JIT
  code does not perform a structure transition with the flag on (section 1.3
  and the audit's PRE-4), so every property add goes to C++. Two million
  objects with six properties each take 20 ms flag off and 850 ms flag on, in
  a Release `jsc`, GIL on or off. Stores to existing properties do not change.
  This is a performance item (Part 2).

### Results, third round (2026-09-04)

The third round took the three amplifier findings that the second round left
open, ran the corpus and the GIL-off JSC suite with
`--verifyConcurrentButterfly=1`, ran Bun's test directories with the GIL off,
ran longer amplifier campaigns, and did the smaller entries of "Open items".
The new tests it wrote found five more engine bugs and the campaigns found two;
seven tests were wrong. Each engine fix has a test in `JSTests/threads/` that
fails on a build without the fix and passes with it, or, where only the
amplifier reaches it, an amplified count; the counts are from Release, GIL
off, unless the entry says otherwise. All runs are on Linux x86-64. The corpus, TSAN, the three JSC
suites, the amplifier campaign and Bun ran on the final tree; the
`--verifyConcurrentButterfly` runs and a first amplifier campaign ran on
earlier trees of the round, as noted.

**Fixes.**

- **A `Set` spread never ended after a rehash, GIL off.** This was the
  stop-the-world watchdog abort of `shared-objects/map-set-shared-writers.js`
  ("OM transition stop", one lite with heap access and no owner thread). A
  backtrace of a stalled run showed a spawned thread in
  `JSCellButterfly::createFromSet` and the main thread waiting for it to
  stop. `[...set]` copies the keys in C++, and the GIL-off branch, which
  collects the keys first because the size can change under it, called
  `transitAndNext` with the set's first table on every step and gave it the
  entry it had reached in the newest one. Once another thread had rehashed the
  set, each step moved that entry back by the deleted entries of the obsolete
  table (a clear moved it to 0), so the walk returned one entry for ever,
  appended its key to a `MarkedArgumentBuffer`, and never reached a safepoint;
  the next stop request then ran into the watchdog after 30 s. The walk now
  goes on from the table it reached, as `JSSetIterator` and the other C++
  loops over these tables do. `shared-objects/set-spread-vs-rehash.js` (two
  threads spread a set while two others delete and re-add keys) failed 20 of
  20 before and passes 100 of 100; `map-set-shared-writers.js` under the
  amplifier, 48 runs at a time, aborted in 22 of 960 runs before and in 0 of
  960 after.
- **`cve/mc-dos-waiter-table-storm.js` was wrong.** No wake-up is lost: every
  `waitAsync` promise settles. The reclamation arm's assertion failed inside an
  `async` callback whose rejection nothing handled, so the shell printed
  nothing and exited with status 3. That arm expects a `gc()` to collect most
  of 128 cells once their waiters are drained, and retries across `await`
  turns, and two things defeat it. JSC releases the objects that
  `WeakRef.prototype.deref` kept alive when the microtask queue drains, not at
  the end of each job, and the retries' own continuations keep the queue full,
  so once the first turn had called `deref()` no later `gc()` could collect the
  cells: only the first one counted. And with the shared heap the first `gc()`
  keeps them whenever another thread is still attached to the heap, which arm
  2's threads sometimes are under load: the window-liveness constraint (`Wlr`
  in `Heap::addCoreConstraints`) retains the newly allocated cells of every
  attached client's active blocks for that cycle, the conductor's own blocks
  included, and the cells sit in the main thread's active block. A heap
  snapshot taken at that point lists the cells with no incoming edge and no
  root, which is how that constraint marks, and a build that logged the
  constraint's appends showed them in exactly the failing runs. The test now
  clears the kept objects (`releaseWeakRefs()`) before each `gc()`, and prints
  unhandled rejections. It failed in 20 of 1,700 amplified runs, 64 at a time,
  before, and in 0 of 1,500 after.
- **`jit/int-gate-fire-vs-execute.js` was wrong**, and so was
  `cve/mc-jit-double-relabel-stale-shape.js`, which this round's campaign
  flagged the same way (exit 3, "expected relabeled but got idle", 2 of about
  250 amplified GIL-off runs). No wake-up is lost. Both start a worker and then run a
  fixed number of rounds on the main thread, which takes a millisecond or two,
  and under load the worker takes longer than that to start; a worker whose
  first look at the stop flag came after the rounds did nothing, and both
  tests demand that it did something. `int-gate-fire-vs-execute.js` failed in
  both GIL modes (38 and 73 of 800 amplified runs, 128 at a time). The main
  thread now waits for the worker's first step before its rounds: 0 of 800 in
  each mode after.
- **The allocation-profile lock of `FunctionRareData` was a cell lock, GIL
  off.** The open item asked whether the Debug cell-lock check could fire in
  the `FunctionRareData` tryLock loops. It does, with
  `--useConcurrentSharedGCMarking` (off by default): the holder fills the
  profile under the rare data's cell lock, and the fill allocates structures
  and property tables, ends deferral scopes, and parks for stops
  (`didBecomePrototype`), each of which reaches the check that no cell lock is
  held at a collection point or a park (`Heap::stopIfNecessaryForAllClients`,
  `GCClient::Heap::acquireHeapAccess`). A `DeferGC` does not help, because the
  park is not a collection. Nothing deadlocks, since no marker takes that lock,
  so the rule is right and the lock was wrong: the fills and clears now
  serialize on a plain `Lock` in the rare data, taken with the same tryLock
  polls, which fits in the object's tail padding (its size is unchanged).
  `objectmodel/allocation-profile-init-lock-not-a-cell-lock.js` (four threads
  construct 150 fresh functions, derived classes and bound functions together
  while the collector runs continuously) aborted 10 of 10 in Debug before and
  passes 10 of 10; Release has no check and passed before. Under TSAN this test
  then found the next entry.
- **TSAN's 16-byte compare-and-swap tore the cell header.** Found by the test
  above under TSAN, GIL off, in 1 of 40 runs: "Invalid value for lock: 0" from
  `JSCellLock::unlock` in `trySegmentedTransition`, on a `JSFunction` whose
  `prototype` was being created on two threads. TSAN builds lower the
  `__sync` builtin in `dcasHeaderAndButterfly` to
  `__tsan_atomic128_compare_exchange`, and TSAN's runtime implements 16-byte
  atomics with a lock of its own around a plain load and a plain store, which
  is not atomic against the 1-, 4- and 8-byte atomics that other threads apply
  to the same 16 bytes; a cell-lock bit set between that load and that store
  was lost, and the holder's unlock found the lock free. Only TSAN builds are
  affected (the instruction is atomic), but every TSAN result depends on it.
  Under `TSAN_ENABLED` on x86-64 the DCAS is now the `lock cmpxchg16b`
  instruction itself, preceded by two no-op read-modify-writes that give TSAN
  the release edge of the publish on both words. Other builds are unchanged.
  The test above passed 40 of 40 under TSAN after; the whole TSAN corpus was
  rerun (below).
- **Two threads that first used one function as a constructor got two
  prototype objects, GIL off.** Seen in the backtrace of the TSAN failure
  above (two threads filling one allocation profile with different
  prototypes) and then reproduced directly: a function's `prototype` is created
  on first use, and `JSFunction::getOwnPropertySlot`, `put`,
  `defineOwnProperty` and `reifyLazyPrototypeIfNeeded` each created and stored
  one when they found it missing, so with two threads the second store replaced
  the first, and objects the first thread had already made were not
  `instanceof` the function (about 1 in 100 objects in the test). A store to
  `F.prototype` that raced with the first read could be replaced by a fresh
  default object the same way. GIL off, the check and the store are now one
  step under a process-wide lock (`storeLazyPrototypeIfMissingGILOff`), whose
  waiters poll for stops because the holder allocates and may park; a later
  arrival finds the property and uses it, and a user store that lost the race
  replaces the value as a store after a read does.
  `semantics/lazy-prototype-first-use-race.js` (four threads construct with
  1,500 fresh functions; then three threads read `F.prototype` while one
  stores it) failed 3 of 3 before and passes 40 of 40, and 6 of 6 in Debug.
  (The first version of the fix declared the lock inside the function template
  and so had one lock per call site; the test's second half caught that.)
- **Two threads that first read one error's `stack` freed the trace under each
  other, GIL off.** Checked because it is the same shape as the previous entry:
  `ErrorInstance` keeps the captured stack trace until `stack`, `line`, `column`
  or `sourceURL` is first read, and that read (`materializeErrorInfoIfNeeded`)
  builds the strings and frees the trace. Two threads doing it on one shared
  error crashed every time, in Release and Debug (a null `Vector` in
  `computeErrorInfo`). GIL off, one thread now claims the materialization
  through a state byte on the error and the others wait for it, polling for
  stops, and read the properties it made; the state byte, not the bit-field
  flag the worker writes, is what they read. The collector's own use of the
  trace (`finalizeUnconditionally`) is unchanged: the worker holds
  `DeferGCForAWhile` as before. `semantics/error-stack-first-access-race.js`
  (four threads read `stack` of 1,500 shared errors) crashed 3 of 3 before and passes 40 of 40, 16 of 16
  in Debug and 20 of 20 under TSAN. Its Debug and TSAN runs found the next two
  entries first.
- **Two caches on the stack-trace path were filled with no lock.**
  `ExpressionInfo::lineColumnForInstPC` keeps a `HashMap` from instruction to
  line and column per code block, and `SourceProvider::sourceURLStripped()`
  computes and assigns a `String` on first call; both run whenever a stack
  trace is built, so with the GIL off two threads reading `stack` of errors
  thrown from the same code raced on them (a Debug hash-table iterator check
  crashed in 1 of 8 runs of the test above; TSAN reported the `String`
  assignment in 5 of 10). The line/column cache now takes one process-wide
  lock when the flag is on, and the stripped URL is published once, under the
  provider's existing lock and behind an acquire/release flag. After: the
  counts in the previous entry.
- **`Atomics.store` of a missing indexed property handled traps under a cell
  lock, GIL off.** Found by this round's amplifier campaign:
  `cve/mc-reent-store-missing-indexed-define-race.js` hit the 30 s
  stop-the-world watchdog ("CodeBlock jettison" pending, the main thread holding
  heap access) in 1 of about 1,300 amplified runs. A backtrace of a stalled
  run: the storing thread, in `JSObject::putDirectIndexForAtomicsMissingAdd`
  with the object's cell lock held, had reached the `RETURN_IF_EXCEPTION` that
  follows the sparse-map store, which handles traps; it parked there for the
  main thread's stop, and when that window had moved the heap-fact epoch it
  asked for a stop of its own to jettison its optimized code, still holding
  the lock. The main thread was by then blocked on that cell lock in
  `Object.defineProperty` of the same index, and a cell-lock wait has no
  safepoint (holders must not stop under one), so nothing moved until the
  watchdog fired. That check is now `RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED`;
  the traps are serviced at the caller's next check. A brace-aware search
  found no other trap check inside a cell-locked block, and `VMTraps::handleTraps`
  now asserts in Debug, GIL off, that no cell lock is held, so the Debug corpus
  catches the next one. Reachable only under the amplifier: the CVE test, 64
  amplified runs at a time, hit the watchdog in 35 of 3,000 runs before and in
  0 of 3,000 after. A test written to force it (a third thread requesting stops
  while the storer and the definer contend) did not fail without the fix and
  was not kept.
- **`Atomics` operations on a typed array read the base after the detach
  check, GIL off.** Found by the final amplifier campaign:
  `cve/mc-prim-arraybuffer-transfer-vs-atomics.js` (one thread hammers
  `Atomics.add`/`load` and `fill` on an `Int32Array` while the main thread
  transfers its buffer) crashed with signal 11 in 2 of about 270 amplified
  GIL-off runs, and in Debug, 64 amplified runs at a time, in 17 of 600: a
  write to address 0 to 12 in `atomicReadModifyWriteCase`. GIL off, a detach
  keeps the view's base word for JIT code and makes the C++ accessor
  `vector()` return null instead (`JSArrayBufferView::detachKeepsVector`), and
  every C++ path that touches the elements loads the base once, before or with
  its bounds proof, and treats null as detached: the element accessors, `set`,
  `fill`, `copyWithin`, the searches, `reverse`, `sort`, `DataView`. The two
  `Atomics` templates (`atomicReadModifyWriteCase`, which serves `add`, `and`,
  `compareExchange`, `exchange`, `load`, `or`, `sub` and `xor`, and
  `atomicStore`) still checked `isDetached()` and the bounds first and called
  `typedVector()` afterwards, so a transfer that landed in between gave them a
  null base. They now load the base first and throw the detached `TypeError`
  on null, as `DataView` does; loaded before the checks, a non-null base is a
  mapping that stays until the next stop. The `Atomics.wait`/`notify` paths
  need a `SharedArrayBuffer`, which cannot be detached. Reachable only under the
  amplifier: 0 of 600 such Debug runs after.
- **`api/thread-lifecycle.js` was wrong, GIL off.** Found by the final
  amplifier campaign, once (exit 3, "continuation cannot run while the lock is
  held: expected 0 but got 1"), and not again in 2,400 targeted amplified
  runs. Its second part created the thread that calls `lock.asyncHold` before
  the main thread took the lock, and checked inside the hold that the
  continuation had not run. With the GIL on the thread cannot run until the
  main thread yields inside the hold; with the GIL off it can take the lock
  first and run the continuation at once, legitimately. The thread is now
  created inside the hold, so its `asyncHold` always finds the lock held, which
  is what the test means to exercise (a ticket whose registrant has finished).
- **`sync/atomics-futex-lock.js` was wrong, GIL off.** Also from the final
  campaign (one hang in about 130 amplified GIL-off runs; targeted, 96 at a
  time, 17 hangs and 3 exits with status 3 in 1,000). Its ping-pong part hands
  a turn marker to a fresh worker each round and parks in an untimed
  `Atomics.wait`, and it assumed the GIL: that the worker cannot run before
  the main thread parks. Without it the worker sometimes ran before the marker
  was handed over, threw "worker ran out of turn", and left the main thread
  waiting for ever; or it finished before the main thread parked, whose wait
  then returned "not-equal". The worker now spins for its turn and the main
  thread re-checks the marker around its wait; the strict alternation is still
  what is checked. 0 of 1,000 after, and 0 of 300 GIL on.
- **`cve/mc-tear-rope-resolve-race.js` was wrong, GIL off.** From the final
  campaign: "torn length: 612 vs 596" in 1 of about 460 amplified GIL-off runs
  (targeted, 1 of 1,000). Not a torn rope: the main thread published each
  round's rope and expected string as four separate fields and the threads
  read them one by one, so a thread delayed between two of those reads while
  the main thread was already storing the next round's fields compared one
  round's rope with the next round's expected value. Each round is now one
  record, published by a single store and read once. 0 of 1,000 after.
- **`jit/dfg-array-shift-elements-race.js` was wrong.** From the final
  campaign, GIL on: "expected ArrayWithInt32 but got ArrayWithArrayStorage" for
  an array that a spawned thread had just made, in 1 of about 160 amplified
  runs (targeted, 4 of 400). This is the array allocation profile, not a race:
  an array literal's site starts making ArrayStorage arrays once an array it
  made has become one, from the moment its profile next looks at that array,
  which for a baseline or LLInt maker is the next allocation and for an
  optimizing-tier maker is never; and part 1 of the test converts an array to
  ArrayStorage (that is what it checks) and then expects the next array from
  the same maker to start flat. It did whenever the maker was still optimized
  code, and the amplifier's delays sometimes had it running in a lower tier at
  that point. The same happens flag off with `$vm.ensureArrayStorage`. The
  arrays the test converts now each come from a maker with its own source text
  (functions with the same source share the site through the code cache):
  0 of 600 amplified runs GIL on and 0 of 300 GIL off after.
- **The LLInt's `op_put_private_name` and `op_set_private_brand` test the
  flag** (the audit's PRE-4). Their caches are never filled with the flag on,
  so the fast paths never matched, but those paths store a StructureID and
  write through a raw butterfly, and nothing local said they must not run. Both
  now branch to the slow path on the flag, as `op_put_by_id` does. No test:
  nothing changes.
- **Terminations that stay VM-wide: decided.** The watchdog (a time budget for
  the VM's scripts), a `SIGINT` in Bun, and a worker's `terminate()` are
  requests about a VM, and they stop every thread of it: with the GIL off a
  spawned thread does not service the watchdog check itself, the carrier does
  and fans the termination out (`VMTraps.cpp`, `NeedWatchdogCheck`), and
  `notifyNeedTermination()` fans out by definition. Only a deadline
  (`VM::addTerminationDeadline`, `node:vm`'s `timeout`) belongs to one
  evaluation on one thread, and it terminates that thread (second round). The
  comments on `VM::notifyNeedTermination` and
  `notifyNeedTerminationForCurrentThread` say this; nothing else changes.

**`--verifyConcurrentButterfly`.** The corpus in the four modes, Release and
Debug, with `JSC_verifyConcurrentButterfly=1` in the environment, on the tree
with the `createFromSet` fix: no failure (273/288/48/62 pass, as without it).
The GIL-off JSC suite with the verifier added, on the same tree plus the LLInt
change: no verifier abort. Against the second round's GIL-off run its failure
list differs by two FFI tests in `ftl-eager-no-cjit` (the executable-memory
flake; four others went the other way) and
`stress/re-enter-resolve-rope-string.js.no-ftl`, a `memoryHog` test killed
with signal 9 under the load of that run, which passes alone in every mode.

**Corpus** (`Tools/threads/run-tests.sh`), on the final tree, with the four
new tests:

| Build | Default, GIL on | Default, GIL off | CVE, GIL on | CVE, GIL off |
|---|---|---|---|---|
| Release | 276 pass, 0 fail, 22 skip | 291 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |
| Debug (ASAN) | 276 pass, 0 fail, 22 skip | 291 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |

The Debug runs include the new `handleTraps` assertion; it did not fire.

**JSC suites** (`run-javascriptcore-tests`, Release, the same collections
as before), on the final tree, with the amplifier campaign and Bun's tests
running on the same machine:

- Flag off: 578 failures, against 579 on `main`. The lists differ only in FFI
  tests in `ftl-eager-no-cjit` (three fail here, four on `main`), the
  executable-memory failure that changes files with every run.
- Flag on, GIL on (`JSC_useJSThreads=1`): 851 failures, against the second
  round's 853 on the same collections. The differences are FFI tests in
  `ftl-eager-no-cjit` again (two against three) and
  `int8-repeat-in-then-out-of-bounds.js.ftl-no-cjit-no-inline-validate`, the
  reoptimization-count test the second round described, which passed this
  time.
- GIL off: 1,781 failures, against the second round's 1,780. Six FFI
  `ftl-eager-no-cjit` tests went one way or the other. Three more failed here:
  `big-int-strict-spec-to-this.js.default`, which asserts
  `numberOfDFGCompiles(foo) > 1` and passes alone (a compile-count test under
  load); `re-enter-resolve-rope-string.js.no-ftl`, a `memoryHog` test killed
  with signal 9 under the load, which passes alone in every mode; and
  `regress-174463162.js.dfg-eager-no-cjit-validate`, which crashed with signal
  11. That one is not this branch's: the test installs an inline-cache
  watchpoint with a dead owner through `$vm` and runs here with
  `--collectContinuously=1 --verifyGC=1`, and under the same load it crashes
  in 7 of 200 runs flag off, and in 7 of 200 with a jsc built from `main`
  (`ASSERTION FAILED: decontaminate()` in Debug). It is recorded here so that
  it is not rediscovered as a threads failure.

**TSAN** (JIT on, `Tools/tsan/suppressions.txt`), on the final tree: the
corpus, GIL on (276 pass, 22 skip) and GIL off (291 pass, 7 skip), and the
CVE suite, GIL on (48 pass) and GIL off (62 pass): 0 reports, 0 failures.
`races/` under the amplifier, ten seeds per test, both GIL modes: 7 of 7 pass
in each, 0 reports. Two earlier TSAN runs of the round and the two findings
they produced (the 16-byte DCAS emulation and `sourceURLStripped`) are in
TSAN-RESULTS.md, "Third round".

**Amplifier.** Three campaigns, each running the four modes in parallel with
ten random seeds per test per pass (`run-tests.sh --amplify`), on a machine
that was also running the suites: about two hours on the tree with the first
three fixes (11, 11, 30 and 25 passes of default GIL on, default GIL off, CVE
GIL on, CVE GIL off), 75 minutes on the tree before the `Atomics` fix (12, 11,
31, 28), and four hours on the final tree (42, 40, 108 and 96 passes; a
default pass is about 280 tests, a CVE pass 48 or 62). Findings other than
output that differs from the reference run (thread timings and counts that the
tests print; every such test was looked at once): the first campaign produced
the `mc-jit-double-relabel-stale-shape.js` exit and the
`mc-reent-store-missing-indexed-define-race.js` watchdog abort; the second,
`thread-lifecycle.js`, `atomics-futex-lock.js` and the `Atomics` crash in
`mc-prim-arraybuffer-transfer-vs-atomics.js`; the last, one
`dfg-array-shift-elements-race.js` exit and one `mc-tear-rope-resolve-race.js`
exit in its first 100 minutes, both test mistakes fixed while it ran (above),
and nothing in the 26 default GIL-on passes and 56 CVE GIL-off passes after
those two fixes, nor in the other two modes. No crash, hang or assertion is
open.

**Bun.** The second round's Bun changes (the two build fixes and the three
GIL-off protocol changes, kept as patches outside this tree) applied to Bun's
tree, a `debug-local` build against the final tree, and the same eight test
directories three times: flag off, `BUN_JSC_useJSThreads=1`, and
`BUN_JSC_useJSThreads=1` with `JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0`, one after the other on a
loaded machine (the JSC suites and the amplifier campaign were running).

| Directory | Flag off | Flag on | GIL off |
|---|---|---|---|
| `test/js/bun/jsc` | 262 pass, 2 fail | 262 pass, 2 fail | 262 pass, 2 fail |
| `test/js/bun/ffi` | 232 pass | 232 pass | 232 pass |
| `test/js/bun/util` | 2038 pass, 6 fail | 2038 pass, 6 fail | 2038 pass, 6 fail |
| `test/js/node/vm` | 292 pass | 292 pass | 292 pass |
| `test/js/node/util` | runner crashed | runner crashed | runner crashed |
| `test/js/web/timers` | 69 pass, 4 fail | 69 pass, 4 fail | 68 pass, 5 fail |
| `test/js/node/worker_threads` | 156 pass | 156 pass | 156 pass |
| `test/js/web/workers` | 455 pass, 2 fail | 454 pass, 3 fail | 454 pass, 3 fail |

The failing tests are the same tests in the three columns, with two
exceptions. `web/workers`, flag on and GIL off: the accept-loop test of the
second round (`terminate() while a worker's Bun.connect() open is firing`),
Bun's unbounded accept loop, unchanged. `web/timers`, GIL off: `setInterval
runs with at least the delay time` saw a tick 31 ms late and the next one 7 ms
after it, which the fixture rejects; it did not fail again in 20 GIL-off and
20 flag-off reruns of the fixture under the same load, and it passed GIL off in
the second round, so it is recorded as load, not as a difference. The rest is
flag-independent: the two `bun/jsc` failures are the `DOMJIT` `node:vm` tests
timing out in this Debug build, `node/util`'s runner dies of a stack overflow
in Bun's console writer (`bun-inspect.test.ts`) in every mode, and the `util`
and `timers` failures are leak and subprocess tests that fail flag off too. No
JSC-side difference was found; nothing GIL-off-specific was left to fix on
this side. The Bun-side items stay as they were (the five changes, the accept
loop), plus one to check: Bun's error-stack hooks (`computeErrorInfo` and the
source-map lookup behind it) now run on whichever thread first reads an
error's `stack`, serialized per error but concurrently for different errors.

**Flag-off changes.** No fix changes what flag-off code does. These change
the code that runs flag off: `op_put_private_name` and `op_set_private_brand`
in the LLInt load the flag byte and branch, once per execution, as
`op_put_by_id` already does; `ExpressionInfo::lineColumnForInstPC` tests the
flag before it takes its lock; `SourceProvider::sourceURLStripped()` tests an
atomic flag instead of the `String` and takes the provider's lock on its first
call, in all modes; the two `Atomics` templates test the loaded base for
null, which flag off is implied by the detach check next to it;
`FunctionRareData`, `ErrorInstance` and `SourceProvider` each gain a byte in
padding they already had, which flag-off code does not read
(`FunctionRareData` and `ErrorInstance` keep their size). The
`createFromSet`, `JSFunction`, `ErrorInstance` and `ThreadAtomics.cpp` changes
are in GIL-off branches, the `VMTraps` assertion is Debug-only, and the DCAS
change is TSAN-only. The intro's "final tree" includes the `Atomics` fix: the
three JSC suites, the corpus and TSAN were rerun after it.

**Found on the way, not fixed (performance).** `Array.prototype.shift` and
`unshift` on an array with ArrayStorage copy the whole storage for every call
when the flag is on (`unshiftCountWithArrayStorageConcurrent` and its `shift`
twin, the AS-COPY rule), where flag off they adjust the index bias in place.
A loop that drains a 200,000-element queue with `shift()` takes 37 s instead
of 10 ms (Release, GIL on or off), and
`stress/array-unshift-should-not-race-against-compiler-thread.js` takes 5
minutes instead of 1 s, which is what made the flag-on suites of this round
and the last take hours. This is the largest flag-on cost found so far, ahead
of the uncached property adds; both are Part 2.

### Results, fourth round (2026-09-05)

The fourth round searched for the rest of the first-use races that the third
round had found four of by chance, made the "no park under a cell lock" rule
something the Debug build checks at every site rather than only where a stop
happens to be pending, decided the conductor's-own-blocks question in the
shared collector, and ran the wider amplifier, TSAN and Bun passes. The search
went through the branch's own audit tables (the K4 VM-state rows and the N7
per-cell rows), checking each ruling against the code as it is now, and then
through the lazily filled members of the runtime classes those tables do not
name. It found nine; the new assertions found a deadlock; TSAN found one
more race that matters and one that does not; running JS threads inside Bun
found one in the console client. Each fix has a test in `JSTests/threads/`
that fails on a build without it and passes with it, unless the entry says
otherwise; counts are Release, GIL off, unless noted. All runs are on Linux
x86-64.

**Fixes.**

- **`WeakMap` and `WeakSet` were not locked, GIL off.** The N7 table lists
  them as covered with `Map` and `Set`, but nothing had been done: `set`,
  `add` and `delete` rehash the table and free the old buffer while another
  thread's `get` or `has` is probing it, and the DFG inlined the probe. Two
  threads writing one `WeakMap` crashed every time (a write to freed memory,
  or bmalloc's free-list check). In a GIL-off process every operation on the
  table now holds the map's cell lock (nothing under it allocates in the GC
  heap or parks; a rehash uses `fastMalloc`), `getOrInsert`'s find-then-add
  is one hold, and the five `WeakMap`/`WeakSet` intrinsics stay calls, as the
  `Map` and `Set` ones do. `shared-objects/weakmap-weakset-shared-writers.js`
  (four writers, a prober, delete-and-re-add runs so the tables shrink too)
  crashed 3 of 3 before and passes 20 of 20, 6 of 6 in Debug and under TSAN.
- **A scoped `arguments` object's first `length` store crashed, GIL off.**
  `ScopedArguments::overrideThings` (the first store or delete of `length`,
  `callee` or `Symbol.iterator` materializes all three) asserted, in Release,
  that it had not run yet; two threads doing that first store on one shared
  arguments object both ran it. `DirectArguments` got a lock for this in an
  earlier round (N7's RESOLVED-3); `ScopedArguments` (RESOLVED-4) had not. It
  now takes the same kind of process-wide lock, re-checks, and publishes the
  flag after its puts (readers acquire); `unmapArgument`, which copies the
  arguments table, holds it too, so two deletes cannot drop each other.
  `semantics/scoped-arguments-override-race.js` aborted 14 of 20 before and
  passes 20 of 20.
- **`JSON.parse` returned another thread's keys, GIL off.** The parser keeps
  recently seen property names in a 512-slot table on the VM (characters,
  length and atom per slot, plus a `JSString` per slot), with no lock; K4
  ruled it per-thread and it was never done. Two threads parsing at once
  paired one thread's characters with another's atom, so an object came back
  with a key that is not in its text (10 of 10 runs of the test), besides the
  `RefPtr` race. GIL off, each thread now parses with a table of its own,
  which holds atoms only (a `JSString` cached there could not be cleared or
  visited by the collector, the same rule as the per-thread numeric-string
  table). `vmstate/json-parse-key-cache-per-thread.js`: 0 of 20 after.
- **`String.raw` used the VM's number-to-string table directly, GIL off.**
  Every other user goes through `liveNumericStrings()`, which is per-thread
  GIL off; this one (numbers in the `raw` array) did not, and two threads got
  each other's digits (10 of 10). `vmstate/string-raw-number-cache-per-thread.js`:
  0 of 20 after.
- **A function's lazy `length` and `name` were seen missing, GIL off.** The
  third round read this site and passed it ("equal values, a double store is
  harmless"). It is worse than that: the "reified" flag was set before the
  property was put, so a second thread that found the flag skipped the
  reification, missed the property, and read `Function.prototype.length` (0)
  instead; and because that miss went through a cacheable structure, the
  inline cache then served 0 for every later function with the same
  structure: a thread saw hundreds of wrong lengths in 2,000 (10 of 10 runs).
  The four flags also shared one byte as bit-fields, so two threads setting
  two of them lost one. Now the flags are one atomic byte (or'ed in, read
  with acquire), set after the put, and GIL off the first reification runs
  under a process-wide lock with a re-check, as the lazy `prototype` does, so
  a value defined over `length` on one thread is not put back by another.
  `semantics/lazy-length-name-first-use-race.js`: 0 of 20 after, 6 of 6 Debug.
- **Intl objects filled members on first use, GIL off.** N7's RESOLVED-6
  ruled these cell-locked; nothing had been done. A `Locale`'s subtags,
  keywords, `maximize()`/`minimize()`/`toString()` strings, the numbering
  system and calendar that `resolvedOptions()` of `NumberFormat`,
  `DateTimeFormat`, `RelativeTimeFormat` and `DurationFormat` report, the ICU
  formatter behind `DateTimeFormat.formatRange` and `NumberFormat.formatRange`,
  the per-unit formatters of `DurationFormat` and the Temporal formatters of
  `DateTimeFormat` are all computed and assigned on first use;
  `RelativeTimeFormat.formatToParts` reused two ICU scratch objects kept on
  the cell; `Segments.containing` and the segment iterator's `next` move a
  break iterator kept on the cell. Four threads using shared Intl objects
  crashed 8 of 10 (a `String` freed under a reader in `Locale.minimize`) and
  got wrong segments in the other 2. Now, in a GIL-off process, each lazy
  member is computed outside any lock and published once under the cell lock
  (a loser drops its copy; `intlLazyField` in `IntlObjectInlines.h`),
  `formatToParts` opens scratch objects per call, `containing` scans with a
  clone, and the iterator's step runs under the cell lock with the objects
  made after it. `semantics/intl-lazy-fields-race.js`: 0 of 20 after, 6 of 6
  Debug, 6 of 6 TSAN (with the ICU suppression below).
- **Assigning to a `const` under contention deadlocked, GIL off.** Found by
  the new Debug assertion (below) in 22 files of the GIL-off stress run:
  `symbolTablePut` threw its "read-only" `TypeError` while holding the scope's
  symbol table lock, and making the error object adds properties, which GIL
  off can wait for a pending stop-the-world; a thread blocked on that symbol
  table lock (any other access to a variable of the same scope) has no
  safepoint, so the stop never completed. Written as a test, it hit the 30 s
  watchdog 10 of 10: `semantics/const-assign-throw-vs-scope-access-under-stops.js`
  (two threads assign to a shared closure's `const`, two bump its `let`, the
  main thread collects). The throw now happens after the lock is released;
  0 of 10 after, and the test runs GIL on too.
- **A `gc()` kept the calling thread's own newest garbage while another
  thread was attached** (the open item from the third round). The
  window-liveness constraint retained the newly allocated cells of every
  attached client's active blocks, the conductor's included. The constraint
  runs on the conducting thread (it is `Sequential`, and a shared collection
  is always conducted by a mutator), whose stack and registers are in
  `m_currentThreadState` and are scanned exactly as in the single-mutator
  protocol, which is the argument the single-client gate in the same
  constraint already makes; so the conductor's blocks carry no witness
  obligation and are now left to the ordinary scan. The precise-allocation
  leg is unchanged (it cannot tell whose allocation it is).
  `gc-stress/gc-reclaims-conductor-garbage-with-thread-attached.js` (200 fresh
  objects, `releaseWeakRefs()`, one `fullGC()`, count the survivors, ten
  rounds, with a second thread parked in a wait): 0 of 10 rounds reclaimed
  their batch before, 10 of 10 after, in 10 of 10 runs; GIL on it passed
  before and after.
- **The owner's in-place element moves paired with a foreign first store
  under TSAN.** `jit/dfg-array-shift-elements-race.js` reported
  `JSArray::fastShift`'s `memmove` against another thread's atomic element
  store (2 of the TSAN corpus runs, 7 of 10 targeted). The owner of an array
  whose butterfly word says no other thread has written it moves elements in
  place (`fastShift`, `shift`/`unshift` on flat storage, `copyWithin`), and
  another thread's first store can land during the move, in the window
  before that store publishes the shared-write bit; the design tolerates the
  value race, but a plain `memmove` promises no unit of copying, so a slot
  could in principle be written in halves, and for a double array a torn
  slot is an impure NaN. Flag on, those moves now go through
  `butterflyConcurrentMoveWords`: `gcSafeMemmove` (64-bit units) in
  production, relaxed word atomics under TSAN. 0 of 10 after. Flag off keeps
  `memmove`.
- **Smaller ones, by reading.** `SourceProvider::getID()` assigns with a
  compare-and-swap, so a provider never answers two IDs (the third round left
  this as harmless; it costs nothing). The VM's lazily made empty property
  name enumerator and promise-resolving executables publish with a
  compare-and-swap flag on, so two first uses keep one cell (the Debug
  assertion in their slow paths would have fired). An optimizing-JIT
  `CodeBlock`'s exception handler table, which grows when an inline cache
  that calls is linked inside a `try` block and shrinks when that stub dies,
  is now read by the unwinder under a lock flag on, since another thread can
  be unwinding through the same `CodeBlock`; `jit/ic-exception-handler-table-vs-unwind.js`
  drives that shape from four threads but did not fail before the fix either
  (the window is one reallocation), so it is coverage, not a regression test.
- **`jit/int-gate-jettison-vs-execute.js` was wrong**, the same way its
  sibling was in the third round: eight rounds on the main thread can finish
  before a worker has started under load, and the test demands progress from
  every worker. It failed once in this round's first Release corpus run. The
  main thread now waits for each worker's first iteration.

**The park-under-lock rule, checked at every site (item 2).** The third
round's assertion ran inside `VMTraps::handleTraps`, so it caught a trap
check under a cell lock only when a trap was pending there. Debug builds now
count held ConcurrentJSLocks per thread as they already count cell locks
(`ConcurrentJSLockDepth`, for `Structure::m_lock`, `CodeBlock::m_lock`,
`SymbolTable`'s and the other users of the locker classes), and assert that
both counts are zero: on every call of
`JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld` (pending or not), in
every expansion of `RETURN_IF_EXCEPTION` GIL off (unless a `DeferTraps`
scope is open, since no trap is handled under one), in `handleTraps`, at the
shared collector's collection-point poll and its two acquire-access park legs
(previously gated on `--useConcurrentSharedGCMarking` only), and in
`releaseHeapAccess` (a thread that gives up heap access under such a lock
lets a stop complete that a thread blocked on the lock cannot join). The Debug
corpus in four modes passed with these, so no corpus path holds either kind of
lock at a park, a poll or a trap check; the whole `JSTests/stress` directory
run once GIL off on the Debug build (5,753 files, default options) fired the
assertion in 22 files, all at the `symbolTablePut` site above, and nowhere
after the fix. `WaiterListManager` was read for the same shape: it allocates
its promise before taking a list lock, releases heap access before the list
lock in the synchronous wait, and only reads flags inside; nothing to change.
`SparseArrayValueMap`'s lock is the cell lock and is covered by the counter.

**Bun's error stacks and console from a spawned thread (item 5's question).**
Reading `error.stack` on a spawned thread runs Bun's `computeErrorInfo` hooks
and its source-map remap there; the remap is serialized on Bun's side
(a mutex around the frame remap and the source-map table's lock), and the
rest uses the error's own global object. Running it for real needed three
changes first: Bun's two microtask-tick hooks dereferenced the thread-local
default global object, which a spawned JS thread does not have (they now
return; a Bun change, kept with the others), and `console.log` from a spawned
thread tripped the single-thread assertion of the `WeakPtr` in
`JSGlobalObject::m_consoleClient` in Debug — every thread of the VM uses that
global's console, so with the flag on `setConsoleClient` now keeps the pointer
without the thread assertion (a JSC change). After that a Bun script in which
four threads read the stacks of 300 fresh errors and of errors the main thread
made, thrown three frames deep in a TypeScript file (so every frame goes
through the remap), printed identical, correctly mapped frames on every
thread, flag on and GIL off.

**Corpus** (`Tools/threads/run-tests.sh`), on the final tree, with the nine
new tests:

| Build | Default, GIL on | Default, GIL off | CVE, GIL on | CVE, GIL off |
|---|---|---|---|---|
| Release | 285 pass, 0 fail, 22 skip | 300 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |
| Debug (ASAN) | 285 pass, 0 fail, 22 skip | 300 pass, 0 fail, 7 skip | 48 pass, 0 fail, 17 skip | 62 pass, 0 fail, 3 skip |

The Debug runs include all of the new assertions.

**Debug GIL-off stress run.** Every file of `JSTests/stress` once, default
options, GIL off, on the Debug build (5,753 files): before the `symbolTablePut`
fix 22 files hit the new assertion, all at that site; after it, none. The
rest of that run's non-zero exits (147 timeouts of slow tests in Debug, and
ten aborts that the flag-off Debug build produces too: module tests that need
the harness's working directory, `$vm` tests that need an option, the
sampling profiler's GIL-off refusal, an intentional crash test) are not
threads findings.

**JSC suites** (`run-javascriptcore-tests`, Release, the same collections
as before, 97,641 runs each), on the final tree, with the amplifier campaign
and Bun's tests running on the same machine:

- Flag off: 577 failures, against 579 on `main`. The lists differ only in FFI
  tests in `ftl-eager-no-cjit`, in both directions, as in every round.
- Flag on, GIL on (`JSC_useJSThreads=1`): 854 failures, against the third
  round's 851. New on the list are three FFI `ftl-eager-no-cjit` entries and
  `int8-repeat-in-then-out-of-bounds.js.ftl-no-cjit-no-put-stack-validate`
  (the reoptimization-count test the second round described); one entry of
  the old list passed. Nothing else differs.
- GIL off: 1,779 failures, against the third round's 1,781. Five FFI
  `ftl-eager-no-cjit` entries are new and seven entries of the old list
  passed; nothing else differs.

**TSAN** (JIT on, `Tools/tsan/suppressions.txt` with this round's three
new entries), on the final tree: the corpus, GIL on (285 pass, 22 skip) and GIL
off (300 pass, 7 skip), and the CVE suite, GIL on (48 pass) and GIL off (62
pass): 0 reports, 0 failures. Then the whole default corpus under the
amplifier on the TSAN build, ten seeds per test, once in each GIL mode (about
75 minutes each): 0 reports; the only failures were the nine tests whose
output is timings or counts and so differs from the reference run under any
amplification (the `scaling/` benchmarks, `heap-bench-allocation.js`,
`jit/int-gate-stop-budget.js`, `vmstate/dump-registers-gil-on-vm-in-gil-off-process.js`),
the same nine in both modes. TSAN-RESULTS.md, "Fourth round", has the
intermediate runs and the four findings (ICU, `fastShift`, `TypeInfoBlob`,
the fence-state pair).

**Amplifier.** One campaign on the final tree, the four modes in parallel with
ten random seeds per test per pass, four hours, on the machine that was also
running the suites and Bun: 38, 33, 106 and 93 passes of default GIL on,
default GIL off, CVE GIL on and CVE GIL off. Findings other than output that
differs from the reference run: `api/blocking-gate.js` (9 amplified GIL-off
runs of the first pass exited 0 where the reference run had thrown) and
`cve/mc-lock-stop-vs-park.js` (one run threw "locker made progress: expected
true") were both tests assuming timing they cannot have GIL off — the first
asserted that `join()` on a just-spawned thread throws "cannot block", but GIL
off the thread can already have finished, so it now starts its work only after
that check; the second demanded progress from a thread that under load had not
started before the rounds ended, so the main thread now waits for its first
iteration, as the third round did for two similar tests. Both were fixed while
the campaign ran and did not recur in the 32 default and about 90 CVE GIL-off
passes after. One crash is open: `cve/mc-grow-s4-detach-nullvec-repro.js`
(four threads store into a `Uint8Array` while the main thread transfers its
buffer 4,000 times; the whole test runs for about 30 ms) died with signal 5 —
a Release assertion, which prints nothing — in one amplified GIL-off run out
of roughly 900 in the campaign. It did not reproduce in 8,000 targeted
amplified runs on the same binary (up to 200 at a time, also pinned to two
cores to mimic the load) nor in 800 Debug runs, and the campaign's remaining
two hours, with core dumps enabled, did not hit it again. Recorded under "Open
items".

**Bun.** The Bun changes (now seven, kept as patches outside this tree)
applied, a `debug-local` build against the final tree, and twelve test
directories three times — flag off, `BUN_JSC_useJSThreads=1`, and that plus
`JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0` — one
mode after the other while the suites and the amplifier ran:

| Directory | Flag off | Flag on, GIL on | Flag on, GIL off |
|---|---|---|---|
| `test/js/bun/jsc` | 258 pass, 6 fail | 260 pass, 4 fail | 262 pass, 2 fail |
| `test/js/bun/ffi` | 232 pass | 232 pass | 232 pass |
| `test/js/bun/util` | 2,038 pass, 6 fail | 2,037 pass, 7 fail | 2,038 pass, 6 fail |
| `test/js/node/vm` | 292 pass | 292 pass | 292 pass |
| `test/js/node/util` | runner crashed (stack overflow in a parallel test, flag off too) | same | same |
| `test/js/web/timers` | 69 pass, 4 fail | 69 pass, 4 fail | 69 pass, 4 fail |
| `test/js/node/worker_threads` | 156 pass | 156 pass | 156 pass |
| `test/js/web/workers` | 455 pass, 2 fail | 454 pass, 3 fail | 454 pass, 3 fail |
| `test/js/node/fs` | 817 pass, 2 fail | 816 pass, 3 fail | 817 pass, 2 fail |
| `test/js/node/http` | 697 pass, 4 fail | 697 pass, 4 fail | 697 pass, 4 fail |
| `test/js/web/fetch` | 11,413 pass, 8 fail | 11,417 pass, 4 fail | 11,417 pass, 4 fail |
| `test/js/bun/http` | 2,814 pass, 2 fail | 2,814 pass, 2 fail | 2,814 pass, 2 fail |

Every failure in the flag-on columns either fails flag off too (the DOMJIT
and `node:vm` timeouts in Debug, the timer and leak tests, the `node/util`
runner crash) or is a Debug-build timeout that depends on speed, not on
threads: `error gc test #4` took 60.7 s against a 60 s limit flag on (it takes
35 s alone in either mode and fails its own 5 s default alone in both);
`readdirSync … x 100` took 13.7 s against 10 s under load and passes alone
flag on; and `worker-terminate-lifetime`'s "terminate() while a worker's
`Bun.connect()` open is firing" timed out flag on and GIL off — it also times
out flag off with `BUN_JSC_useJIT=0`, so what it needs is a main thread fast
enough to keep up with the worker's reconnect loop, which the flag-on Debug
build, with property adds uncached, is not. No failure is a threads bug; the
last one is one more reason to do the transition-caching work.

**Flag-off changes.** No fix changes what flag-off code does, with one
ordering exception: `symbolTablePut` now creates its read-only `TypeError`
after releasing the symbol table lock instead of under it. These change the
code that runs flag off: `WeakMapImpl`'s operations, `String.raw`, the JSON
key table lookups (once per key), the Intl lazy members, `ScopedArguments`'
override and unmap, and `JSFunction`'s first `length`/`name` reification each
test one byte (the frozen `gilOffProcess` byte or the flag); `ScopedArguments::overrodeThings()`
and `FunctionRareData`'s flag byte are acquire loads and their stores release
stores or, flag on only, an atomic or (plain moves on x86-64; `ldapr`/`stlr`
on arm64, flag off too); `SourceProvider::asID()` is a relaxed load and
`getID()` a compare-and-swap once per provider; the VM's lazy enumerator and
executables, `CodeBlock`'s handler-table append/remove and the unwinder's
lookup, and the array element moves branch on the flag; `setMutatorShouldBeFenced`
stores with relaxed atomics (plain moves); `ConcurrentJSLockerBase::unlockEarly`
clears its `std::optional`. The assertions, the lock-depth counter and
`VM::assertNoLockHeldAtTrapCheck` are Debug-only; `RETURN_IF_EXCEPTION`
calls an empty inline function in Release. The Wlr change affects shared
heaps only; the `setConsoleClient` change is flag-on only.

**Performance (item 6): looked at, not changed.** Both items turned out to
need an object-model protocol change rather than a local fast path, so this
round leaves them as designs.

- **`shift`/`unshift` on ArrayStorage (AS-COPY).** Flag off, `shift()` on an
  ArrayStorage array is O(1): `Butterfly::shift` moves the header forward one
  slot and bumps `m_indexBias`, so the butterfly pointer changes but no
  element moves; `unshift` does the reverse into the pre-capacity. Flag on,
  every call copies the whole storage into a fresh butterfly, so a loop that
  drains an array converted to ArrayStorage (which `shift` itself does above
  `MIN_SPARSE_ARRAY_INDEX` elements) is quadratic: 20,000 elements take 240 ms
  against 1 ms, 200,000 take 37 s against 10 ms. The idea for this round was an
  in-place path for the owner thread when the butterfly word says no other
  thread has written the array (SW=0). That is not enough. The JIT reads
  ArrayStorage lock-free from any thread when SW=0 (SPEC-jit 5.5's AS-rule,
  `CCallHelpers::loadButterflyForRead`, `KnownArrayStorage` tests only the SW
  bit), and that is sound only because superseded storage is never rewritten:
  a reader that loaded the old butterfly pointer reads a frozen snapshot. A
  header move rewrites the old header's bytes in place — after `shift` by one,
  the old `m_vector[0]` slot holds the new `{m_indexBias, m_numValuesInVector}`
  pair, which a stale reader would take for a JSValue, and with out-of-line
  properties or on `unshift` the old `vectorLength` itself is overwritten, so
  its bounds check passes on garbage. So an in-place relayout needs *no*
  lock-free foreign reader, and reads leave no trace in the word. The sound
  version changes the read rule for ArrayStorage to owner-only: a foreign
  thread's read of an ArrayStorage array takes the locked path whatever the
  SW bit says (`loadButterflyForRead`'s `KnownArrayStorage` arm compares the
  thread tag as the write arm does; the `MaybeArrayStorage` arm sends foreign
  readers to the shape check, which needs the indexing-byte scratch register
  at every such site rather than the conservative form; the three LLInt sites
  and the DFG/FTL butterfly plans likewise; `ArrayLength` on an ArrayStorage
  base included). With that, the owner's `shift`/`unshift` can run the
  flag-off algorithm under the cell lock (the C++ readers, the marker and every
  foreign writer already take it). In-place compaction without moving the
  header (moving elements down, which the existing rules do allow, since each
  slot stays a valid value) was considered and rejected: it removes the
  allocation but is still O(n) per call. Not done in this round; it is the
  first item for the performance work, with tests for a foreign reader and a
  foreign `length` inline cache hammering an array its owner drains.
- **Cached property-add transitions.** `tryCachePutBy` refuses every
  `Transition` case flag on, and the DFG never sees a transition in a put
  status, so each add of a new property runs `putDirectInternal` in C++ (the
  40x on object-creation loops). The spec already defines the predicate a JIT
  transition needs (SPEC-objectmodel E4: both of the source structure's
  thread-local sets valid and watched, not a precise allocation, and the
  butterfly word's tag equal to this thread's with SW=0), and the C++ path
  uses exactly it — but only for objects that *have* a butterfly. An object
  with no butterfly (a `{}` with inline properties, the common case) is
  excluded from E4 in C++ too (`tryPutDirectTransitionConcurrent`, the
  "word == 0" rule, because a plain structure store would race the lock-free
  first indexed install's nuke-CAS on the same word), and goes through
  `tryStructureOnlyTransition`: the F2 check, the cell lock, and a 64-bit
  header compare-and-swap. An inline cache cannot take a cell lock, so caching
  the common case needs a lock-free variant of that protocol: check the thread
  tag against the structure's transition-thread-local TID, claim the
  StructureID lane with a `cmpxchg` from the old ID to its nuked form (fail
  to the slow path, nothing written yet), store the value, fence, store the
  new ID. The other participants already tolerate a nuked ID (the C++
  structure-only transition re-reads under its lock and restarts, the indexed
  first install claims with the same kind of CAS, readers re-dispatch), and a
  foreign transitioner fires the thread-local sets in a stop-the-world before
  it takes the lock, which jettisons the stub. That is a protocol addition, to
  be written into SPEC-objectmodel and reviewed before it is emitted, together
  with: the four watchpoints on the stub, a `PutByStatus` gate so that the DFG
  does not start planting unguarded `PutStructure`s once the caches hold
  transition cases, and, separately, the observation that a DFG `PutStructure`
  on an object the same compilation unit allocated and has not yet stored
  anywhere is thread-local by construction and needs no runtime check at all —
  which is the object-literal pattern the 40x figure comes from. Not done in
  this round.

### Results, fifth round (2026-09-06)

Two goals. A: take the GIL-off engine to the point where a new way of looking
stops finding new bugs, and say what that rests on. B: make the flag cost
nothing when it is off and much less when it is on, and make GIL-off threads
scale on ordinary object-heavy code. The safety half added a harness that
runs every `JSTests/stress` file's global code on two JS threads at once
("mirror"), a read of the directories the earlier rounds had not walked, a
read of Bun's bindings plus Bun's test directories with a second JS thread
kept alive, and a longer amplifier campaign with core dumps on. The
performance half is written up in `PERF-RESULTS.md`; the protocol changes it
needed are SPEC-objectmodel rev 15 (N2-LF, L4-K) and rev 16 (N1-I instance
keying, F4 chain-fire withdrawn, M8 no longer forced, §4.6 AS-INPLACE) and
SPEC-jit §5.5's Transition row (now emitted) and I21 (GIL-on `CheckTraps`),
each with a history entry (objectmodel §23-§24, jit §24-§28, heap §26). The
round's findings run F3-F24; F16-F24 were found by the final verification
passes themselves (amplifier campaign, final mirror passes, Bun GIL off, the
scaling suite) and are reported where they were found. Every engine
fix has a test in `JSTests/threads/` that fails on a build without it and
passes with it unless the entry says otherwise; counts are Release, GIL off,
Linux x86-64, unless noted.

**A1. The mirror harness** (`Tools/threads/mirror/`). `mirror.js` reads a test
file and runs its source on N threads sharing one global at once — `eval`
mode: the global code itself through indirect eval; `func` mode: the source as
a function body called on every thread — behind a start gate so the threads
really overlap. `run-mirror.sh` drives a directory with a per-file watchdog
that separates a spinning timeout (exit 124: the test's own logic livelocked
by two copies sharing its globals, expected and ignored) from a blocked one
(125: every thread asleep, a deadlock candidate; it takes a `gdb` backtrace of
all threads before killing), and collects crashes, assertion failures and
TSAN reports; `$vm.crash()` (how stress tests fail) is turned into an
exception so that only engine aborts count. The point is coverage without
authorship: 5,752 files written to test everything else in the engine become
5,752 two-thread races through every subsystem, with no idea in advance of
what should break. Passes: eval and func modes on Release (300 s per file),
eval mode on Debug+ASAN and on TSanJIT (420 s), on the tree as of the start of
the round; then again on the final tree (the paragraph before A2).

First passes. Release: 42 findings in eval mode and 32 in func mode; most were
the shell (`jsc.cpp`'s `Worker` and agent machinery assumed one JS thread;
fixed there so the harness can run: `Worker::current()` is created on demand,
`setTimeout` on a spawned thread posts to that thread's run loop,
`Workers::broadcast` no longer deadlocks against itself), the rest the engine
findings below. Debug: 34 crashes and 26 assertion failures, which reduce to
the same engine set plus F7, F9, F11, F12 and F15 below, the shell's
`PropertyFilter` (an unlocked HashSet, 21 files), harness artefacts (`$vm`
test functions that assert they were called on the real `$vm`, 6; the type
profiler's single-thread option tests, 3; agent-report waits that cannot be
satisfied with two copies running, 7), and four stop-watchdog aborts under a
load average above 90 that do not reproduce alone (latency, A5). TSAN:
reports in 448 files, 75 distinct stack signatures after de-duplication
(`Tools/threads/mirror/tsan-dedupe.py`), every one classified in
TSAN-RESULTS "Fifth round": 14 code changes, the rest publication idioms and
racy-by-design words added to `Tools/tsan/suppressions.txt` with the reason.

Engine findings from the mirror, all GIL off:

- **F3. Two first `Object.defineProperty` calls raced the property-descriptor
  fast-path watchpoint install** (`RELEASE_ASSERT(!isBeingWatched())` in
  `tryInstallPropertyDescriptorFastPathWatchpoint`, 3 of 10). The install now
  runs inside a stop with a re-check; the three `m_installed*Watchpoints`
  vectors that the other first-use installs append to got a lock; the custom
  getter/setter function caches (`WeakGCSet`) got `WeakGCMap`'s locking.
  `vmstate/property-descriptor-watchpoint-first-use-race.js` 3 of 10 before,
  0 of 20 after; `vmstate/custom-accessor-function-cache-race.js` 7 of 20
  before, 0 of 20.
- **F4. Global `let`/`function` declarations from two scripts raced**
  (`initializeGlobalProperties` checks for a clash, then adds; both threads
  passed the check: duplicate symbol-table entries, 5 of 20 aborts). Program
  and eval global declaration hold one process lock across check and add
  (`GILOffFirstUseLocker`, polling). `semantics/global-declaration-race.js`:
  exactly one winner per name, 0 of 20.
- **F5. `defineProperty` turning a data property into an accessor (or back)
  published the `GetterSetter` before the structure**, so a reader between the
  two stores took the accessor cell for the value (a `GetterSetter` reached
  `jsAdd`; 10 of 10 SIGSEGV). Every tier's reader is "check structure, load
  slot" with nothing after the load, so the fix is on the writer: a kind
  change of an existing slot is one per-event stop (SPEC-objectmodel L4-K,
  rev 15), and the three C++ readers whose window can park re-validate (I39).
  `objectmodel/define-property-kind-change-vs-readers.js`: 0 of 40 after.
- **F6. Heap snapshots from two threads deadlocked, then crashed**
  (`preventCollection` parked in a plain lock with heap access held while the
  holder requested a stop; then two `HeapSnapshotBuilder`s appended to one
  profiler). Polling `tryLock` GIL off; builders serialize on a profiler lock;
  the profiler is created eagerly GIL off.
  `gc-stress/heap-snapshot-from-two-threads.js`: 10 of 10 before, 0 of 10.
- **F7. Builtin creation parsed outside the compilation lock** (two threads
  touching different builtins first raced the VM's per-provider parser cache;
  a Debug HashTable assertion in 7 files). `BuiltinExecutables::createExecutable`
  takes the GIL-off compilation lock and `VM::addSourceProviderCache` asserts
  it. Read, not reproduced outside the mirror.
- **F9. A static-table property read paired an offset from one structure
  sample with a slot from another** (`setUpStaticFunctionSlot`; a Debug
  `PropertySlot::setValue` assertion when two threads froze the global object:
  one thread's dictionary flatten, inside a stop, moved the slots between the
  other's offset lookup and its load, and the lookup can park in the
  reification lock). Flag-on the pair is re-derived from one validated
  structure sample (`getDirectRevalidatingConcurrently`, which now also
  returns the offset). 1 of 6 mirror runs before; 0 of 10 after.
- **F11. A special-property cache install release-asserted watchability it
  had checked before another thread's transition** (`CachedSpecialPropertyAdaptiveStructureWatchpoint::install`,
  1 file). Flag-on the install declines and the caller drops the half-built
  entry, as it already did for the equivalence watchpoint of the same entry.
- **F12. `putByIndexBeyondVectorLength` on ArrayStorage asserted a bound
  another thread had just grown past** (1 file). Flag-on it re-reads under the
  cell lock and takes the in-vector store when the vector now covers the index.
- **F15. Entering dictionary indexing mode from a blank indexing type
  allocated the sparse map unlocked** while a second thread, now seeing
  ArrayStorage, did the same under the cell lock (TSAN; one map and its
  entries lost). The blank case goes through the locked path.
- **A typed-array view created while another thread detached its buffer kept
  a pointer into freed memory.** Found by the Debug corpus, not the mirror
  (`cve/mc-life-detach-quarantine-storm.js` failed once: "reader observed
  corrupt word"). GIL off, `transfer()` copies and detaches, the old mapping
  goes to the quarantine and is freed at the next stop, and the detacher
  neuters a snapshot of the buffer's views taken under the incoming-reference
  lock; a view whose constructor had checked `isDetached()` before the detach
  and registered after the snapshot was never neutered and read the mapping
  after the stop freed it. The view constructor re-checks the flag after
  registering (ordered by the same lock) and neuters itself.
  `vmstate/typed-array-view-vs-concurrent-detach.js`: Debug 5 of 5 before, 0
  of 5 after (Release 0 of 10 either way; the window needs ASAN's timing).
  This is very probably the fourth round's one unexplained SIGTRAP
  (`cve/mc-grow-s4-detach-nullvec-repro.js`, same view/detach race, whose
  only release assertions are the two in `detachFromArrayBuffer`); the
  campaign below ran with core dumps on to settle it: no recurrence in about
  2,000 amplified runs of that test.
- **F16. A compile/install/jettison loop between two threads, ending in an
  out-of-memory abort** (Release mirror, final-tree pass:
  `yarr-terminal-parentheses-min-count.js` and, less often, two `create-this`
  files; 25x slower when it did not abort). Two threads sharing one global
  kept requesting stops (each first indexed store into an array built by the
  generic path — `Array.from` over an iterator, a species-created `map`
  result — relabels Undecided to a typed shape, and flag-on that relabel was
  a stop-the-world unconditionally); every stop whose window rewrote a heap
  fact makes each parked thread jettison its own on-stack optimized code on
  resume, and those jettisons were not counted as reoptimizations, so the
  60-bytecode function was DFG-compiled again on its next warm-up — 48,000
  times in 40 s — each install re-linking every incoming call site and
  retiring the old link records until the retired list exhausted memory.
  Two changes. The resume-time jettison counts toward the stock exponential
  reoptimization back-off (`VMTraps.cpp`). And an Undecided-source relabel of
  an array the calling thread owns no longer stops the world: nothing in any
  tier reads an element lane of an Undecided-shaped object, so the owner
  claims the structure lane (the N2-LF CAS), re-checks the word, writes the
  hole/PNaN lanes and publishes the typed shape (SPEC-objectmodel T4, rev
  16, history §24.5; the general in-place relabel keeps its stop, for the
  reason recorded at the withdrawn "thread-local gate"). The file runs in
  2-5 s, 8 of 8 (was 55-80 s and an abort in 5 of 6); 3,000
  `Array.from(match).map(...)` with a second thread running: 38 -> 3.8 ms.
  `objectmodel/undecided-owner-relabel-no-stop.js` counts stop requests
  (`$vm.jsThreadsStopRequestCount`, new) across 6,000 owned relabels after
  the shape's sets have fired (≤ 200; one each before) and has a foreign
  thread read arrays while their owner relabels them (holes or the values,
  nothing else).
- **F17. A thread waiting for a collection did not park for a JS-threads
  stop** (amplifier, `w16-c1-prevent-collection.js`, 2 of 120 amplified
  runs under load: the 30 s stop watchdog). `Heap::waitForCollector`'s
  shared-heap loop cooperates with the collector's own stops but held heap
  access across a thread-granular stop requested meanwhile (a jettison on a
  third thread), and the collection it waited for had its marking paused
  behind that stop: a three-way wait. The loop polls the JS-threads park site
  too now. 0 of 40 amplified runs after, and no recurrence in the final
  campaign (33 GIL-off passes of the default set).
- **F18. `setUpStaticFunctionSlot` release-asserted when another thread
  deleted the property between its reification and its re-probe** (Release
  mirror, `temporal-timezone.js` in func mode: "Static hashtable initialiation
  for PlainDateTime did not produce a property"). Flag-on that is "not
  found". F9's re-derivation there was also revised: it re-derives only on a
  hole (a stale offset after a flatten, or a concurrent delete — freed offsets
  are quarantined, so never another property's value), a bounded number of
  times, instead of revalidating against the structure and the dictionary
  edit stamp, which looped for as long as another thread kept adding
  properties to the shared global.
- **F19. The shared collector's conductor parked in its own stop** (Bun,
  GIL off, `test/js/bun/util/filesink.test.ts` with the keep-alive thread:
  a hang, every time). The collection's stop request sets the trap bit on
  every thread of the VM, the conductor's included, on the assumption that
  the conductor runs no JS until it resumes the world; but it runs C++ that
  polls traps — here Bun's error-info hook, called from
  `ErrorInstance::finalizeUnconditionally` while the collector materializes
  a dying stack trace, calls `VM::hasExceptionsAfterHandlingTraps()` — and
  `notifyVMStop` parked it as a participant of its own stop, with the second
  thread waiting for heap access behind the same stop. `notifyVMStop` now
  returns at once on a thread that is doing GC work. The jsc shell cannot
  reach this (its error-info path polls no traps); the Bun file is the
  regression check (64 of 64 pass GIL off with the preload after; hung
  before).
- **F20. A foreign thread's first out-of-line add on a butterfly-less
  dictionary object aborted** (Release mirror, final-tree pass:
  `megamorphic-instance-dictionary-miss.js`, 16 of 30). A regression of this
  round's N1-I: the tagged-word store on the locked add path release-asserts
  that the word it replaces is "empty or owner-tagged", and a butterfly-less
  word is no longer empty — it carries its allocator's TID. That store is the
  N3 first-install shape (fresh storage, the StructureID lane already
  claimed, nothing to copy), so the assertion now admits a payload-free word
  of any owner and the installer becomes the owner, as when the word was 0.
  `objectmodel/foreign-first-outofline-add-on-dictionary.js`: 6 of 6 aborted
  before, 0 of 6 after.
- **F21. An inline cache release-asserted that a property condition it had
  just validated still held** (Release mirror, final-tree pass:
  `primitive-poly-proto.js`, once; "This condition is no longer met"). Between
  `couldStillSucceed()` and code generation another thread transitioned the
  condition's object — a shared prototype. Flag-on the case is given up
  instead (the next repatch re-derives it): `collectConditions` reports the
  stale condition, the polymorphic-access path emits an always-miss arm and
  the handler path returns `GaveUp`. `jit/ic-condition-stale-at-generation.js`
  (one thread reshaping two prototypes while another generates ICs through
  them at fresh sites): 5 of 10 aborted before, 0 of 12 after.
- **F22. `JSON.stringify`'s fast path asserted its object's structure
  cannot change mid-walk** (Debug mirror, final-tree pass:
  `symbol-with-json.js`). True on one thread (the fast path first rules out
  getters, `toJSON` and proxies); another thread can transition the shared
  object meanwhile. Flag-on the fast path gives up when it sees the structure
  move and the generic stringifier finishes ("structure changed
  concurrently"); flag-off the assertion stands.
  `objectmodel/json-stringify-vs-concurrent-transition.js` (one thread builds
  objects in place while the other stringifies them and parses the result
  back): Debug 4 of 4 asserted before, 0 of 4 after; the results always parse
  and keep the stable properties.
- **F23. The heap's observer list was appended to from two threads** (TSanJIT
  mirror, final-tree pass: the thirteen `ffi-*.js` files). A global object's
  FFI context registers a `HeapObserver` when it is created, lazily, by
  whichever thread uses FFI on that global first — two threads at once with
  JS threads (the context itself is CAS-published; the loser's registration
  and removal still ran). `Heap::addObserver`/`removeObserver` take a lock;
  the list is iterated only by the collector with the mutators stopped.
- **F24 (performance). Global property reads never cached flag-on.** The
  first review round froze scope metadata flag-on, so every read of a
  non-`var` global (`Math`, `JSON`, constructors, user globals assigned as
  properties) took the slow path in every tier: 50x on a bare read, 14x on
  `Math.sqrt` in a loop, and 1.2-2.8x on four of the five scaling workloads
  single-threaded — the largest flag-on cost left in the tree, missed by the
  micro set because its loops use locals. Gets are cached again on x86-64
  with an ordered publish (SPEC-jit history §28); puts and the lexical-var
  rewrite stay frozen. `jit/global-property-cache-vs-global-transitions.js`.
- **Bun's `JSValue.isLiveCell` asserted GIL off** (`MarkedBlock::Handle::isLive`
  reads directory bits lock-free, which the shared heap allows only under a
  stop, the slow-path lock or the directory's refill stripe; a plain mutator
  holds none while another client may be re-allocating the bit vectors). The
  mutator-side `isLive(cell)` overload takes the directory's bit-vector lock
  for that one read GIL off; the marker's explicit-version form is unchanged.
  The GIL-off "disabling useWasm" start-up line is no longer printed (Bun
  tests compare stderr; the refusal is documented and observable as
  `typeof WebAssembly`).
- Also fixed from the TSAN pass, in code: the `StructureRareData` bit-field
  word (a real lost update between the cached-`toPrimitive` bits and the
  replacement-watchpoint count, now one atomic word), `Heap::immutableButterflyToStringCache`
  (an unlocked VM-wide HashMap; off GIL off), `RegExp::m_atom` publication
  and the bytecode-fallback ordering, the DFG's plain read of the
  async-iterator profiling word, `JSGlobalObject::ffiContext` (CAS publish)
  and `stackTraceLimit` (one word), `fastSlice`'s byte-granular copy from a
  possibly shared source (64-bit lanes flag-on), and a dozen racy-by-design
  words given relaxed-under-TSAN accessors (`racyLoad`/`racyStore`); the
  table is in TSAN-RESULTS.

Final-tree mirror passes (Release on the final tree; the two engine findings
of the first "final" pass, F20 and F21 below, were fixed and the pass rerun).
Eval mode: 5,752 files; 3,603 ran to completion, 2,096 threw (the test's own
assertions under two copies), 35 spun to the deadline, and 18 stopped
otherwise — every one an artefact of running test code twice rather than an
engine fault: seven `$vm` test functions that assert they were reached
through the real `$vm` object (the harness wraps it), three type-profiler
option tests, seven `waitAsync`/agent files whose report counts cannot be met
by two copies (blocked, 125), and one test built to exhaust the JIT memory
pool, which two copies exhaust into its release assertion (133). Func mode:
5,083 completed, 618 threw, 36 spun, 15 others — the same ten
`$vm`/type-profiler files, two agent/SharedArrayBuffer worker files
(blocked), two out-of-memory kills (`try-get-value-without-gc.js`,
`re-enter-resolve-rope-string.js`, each allocating without collecting,
twice), one agent test exiting 1. Debug eval mode: 4,185 completed, 1,441
threw, 105 spun, 21 others — the shared artefacts, one BigInt-division
watchdog test run without its watchdog (blocked to the deadline), one
codegen-OOM test, and one engine assertion, F22 below. TSanJIT eval mode:
3,587 completed, 2,023 threw, 63 spun, 57 with a TSAN report (30 signatures,
triaged in TSAN-RESULTS: one engine fix, F23 below; the rest publication
idioms, idempotent lazy caches, test hooks or JavaScript-level races of the
doubled test), 22 others (the shared artefacts plus a JIT-pool-exhaustion
test and a 4,000-realm test killed by memory). F22 and F23 were fixed after
these passes and verified by their own tests and the corpus rather than by a
fourth full mirror pass.

**JSC suites** (`run-javascriptcore-tests`, Release, the same collections
as before), on the final campaign tree, with the TSanJIT mirror and the
TSanJIT amplifier running on the same machine:

- Flag off: 580 failures, against 579 on `main` and the fourth round's 577;
  the lists differ only in the FFI `ftl-eager-no-cjit` entries that move in
  every round.
- Flag on, GIL on: 856, against the fourth round's 854. New: two FFI
  `ftl-eager-no-cjit` entries, four configurations of
  `int8-repeat-in-then-out-of-bounds.js` (the reoptimization-count test)
  and `big-int-spec-to-this.js.default`, whose `numberOfDFGCompiles === 1`
  assertion is load-sensitive (0 of 30 reruns fail in either mode); five old
  entries passed.
- GIL off: 1,196, against the fourth round's 1,779. Four FFI
  `ftl-eager-no-cjit` entries are new; 587 old entries pass now — 583 of
  them the ChakraCore collection, which compares output against baselines
  and had failed on the "disabling useWasm under GIL-off" start-up line this
  round removed. What remains over flag-off is the fourth round's list: tests
  whose required
  options JS threads refuse at start-up (`--forceEagerCompilation`,
  `--useConcurrentJIT=false`, the profilers: 341 of the 617 extra
  configurations exit that way), the sampling-profiler collection (refused on
  a GIL-off VM by design), WebAssembly, and the FFI `ftl-eager` set.

**A2. Static sweep** of `interpreter/`, `jit/`, `llint/`, `heap/`, `yarr/`,
`inspector/`, `debugger/`, `API/`, `wtf/`: PRE-17 in
AUDIT-upstream-since-rebase.md, one row per hazard with a disposition. Fixed
from it: `DFG::CodeOriginPool` growth at IC link against lock-free stack
walkers (the sibling of PRE-14's handler table: published array plus retired
list), the Yarr interpreter's VM-wide backtracking-allocator lock (every
interpreted match of every thread serialized on it with heap access held; a
per-thread allocator GIL off), the JSCOnly console agent fed from spawned
threads, two debugger hooks missing the spawned-thread early return, and
`clearConcurrentRetainedDataIfPossible`'s guard made explicit. Recorded, not
changed: option-gated diagnostics (`JITSizeStatistics`, `ICStats`), the C
API's callback-object/class/weak-map structures (single-mutator; unsupported
for cross-thread sharing GIL off; Bun does not use them), the arm64-only
thunk-fence weakening, benign counters.

**A3. Bun.** Three things, one of them a correction. (1) Since 19 August Bun
disables `JSC_*` environment options (`Config::disableEnvironmentOptions()`;
only `BUN_JSC_*` is read), and the second, third and fourth rounds' "GIL off"
Bun runs passed the three GIL-off options as `JSC_*`: those runs were GIL-on
runs. This round passes all four as `BUN_JSC_*`, and GIL-off Bun (Debug)
asserted at start-up, twice. (2) The two start-up findings, both Bun-side and
now in the Bun patch set (nine changes): Bun's client `IsoSubspace`s
(`BunClientData.cpp subspaceForImplSlow`) were not registered with the
thread-local cache of the client that allocates from them, which the shared
heap's allocator-ownership rule requires (JSC registers its own dynamic client
subspaces the same way) — Debug asserted on the first `Zig::GlobalObject`
allocation, Release allocated unowned; and `$TZ` at boot cleared the date
caches and walked the heap before the runtime took the API lock (the
round-two date-cache change requests a stop there GIL off, which needs an
entered thread) — `resetDateCachesAfterTimeZoneChange` takes a
`JSLockHolder`. With those, a Bun process GIL off runs a second JS thread in
parallel with the event loop (50 timed `Atomics.wait` wake-ups per second on
the keep-alive thread while the main thread sits in a timer). (3) The
bindings read (`src/jsc/bindings`, the generated classes, the Rust host
functions): the single largest fact is that Bun's Rust host functions find
their `VirtualMachine` through a thread-local that only Bun's own threads set,
so essentially every Bun native — `console.log`, timers, `require`, `fetch`,
`process.env`, `path.*`, `Bun.*` — is unusable from a JSC-spawned thread today
(a null dereference in Release), independently of any race; behind that, the
per-VM singletons those natives use (the timer heap, the module registry, the
`ScriptExecutionContext` observer set, `DOMURL`'s base cache, N-API handle
scopes, the uWS loop) are unsynchronized. This is a policy decision for Bun
(refuse Bun natives off the main thread with an exception, as the engine does
for `import()` and `bun:ffi`, or route them), recorded with the fifteen most
common entry points in the patch notes; nothing in the engine changes for it.
The twelve test directories ran in three modes on the Debug build against the
final tree, the two flag-on modes with a preload that keeps a second JS
thread alive for the whole process (`Thread` running a timed `Atomics.wait`
loop with a little allocation). Flag off / GIL on / GIL off, pass-fail per
directory: `bun/jsc` 261-3 / 259-5 / 241-23, `bun/ffi` 232-0 / 232-0 /
232-0, `bun/util` 2038-6 / 2037-7 / 1879-13, `node/vm` 292-0 / 292-0 /
291-1, `node/util` (crashes flag-off already, rc 139; 1 both flag-on modes),
`web/timers` 69-4 / 69-4 / 67-6, `node/worker_threads` 156-0 in all three,
`web/workers` 455-2 / 455-2 / 451-6, `node/fs` 817-2 / 817-2 / 816-3,
`node/http` 697-4 / 697-4 / 696-5, `web/fetch` 11417-4 / 11417-4 / 11381-40,
`bun/http` 2814-2 / 2814-2 / 2805-11. GIL on with a second thread alive is
flag-off to within one or two tests per directory (the differences are tests
that count heap objects or time a collection, which the keep-alive thread's
own allocations perturb). GIL off, the extra failures are, by class: tests
that need WebAssembly (disabled GIL off by design: 13 in `bun/jsc`, some 30
`compileStreaming` cases in `web/fetch`, the worker `terminate()` fixtures
that instantiate a module, a `structuredClone` of a `WebAssembly.Memory`);
collection-heavy tests that exceed their time limit in this Debug build
because a shared-heap collection marks the conservative window-witness root
set (SPEC-heap I12 "Wlr") — a continuous-collection allocation loop measures
6 s flag-off, 6 s GIL on, 60 s GIL off in Debug and no difference in Release
— which accounts for the leak/lifetime tests in `web/fetch`, `bun/http`,
`web/timers` and `node/vm` that poll for an object to be collected; and six
`Bun.stripANSI` "returns the same object" checks that compare
`heapStats()` string counts across a call while the keep-alive thread makes
strings. Two start-up assertions and one hang found on the way are F19 and
the two Bun patch entries above; no GIL-off run crashed or hung on the final
tree.

**A4. Amplifier campaign.** Four modes (default and CVE sets, GIL on and
off), ten random seeds per test per pass, passes repeated for four hours on the
final Release build, core dumps enabled (`ulimit -c unlimited`; a Release
assertion prints nothing, a core names the site), then the default set once
under the amplifier on the TSanJIT build in both modes. The first pass, on the
tree before the last fix, found a crash this round had introduced: with
profiled allocation enabled GIL off (B3), the JIT `create_this` fast path's
two loads of a function's allocation profile ({allocator, structure}) could
be torn by another thread's `clear()` or refill (a `.prototype` store), and a
null structure or a size-mismatched allocator reached the inline allocator
(`objectmodel/allocation-profile-init-lock-not-a-cell-lock.js`, SIGSEGV in 6
of 10 seeded runs and 4 of 6 plain runs; before this round the GIL-off
profile allocator was always null, so the fast path never ran and the pair was
never read). The fast path now reads structure, allocator, structure and
takes the slow path unless the two structure reads agree and are non-null;
`clear()` stores structure before allocator. `jit/create-this-profile-torn-pair.js`
(three constructing threads against a main thread flipping `.prototype`): 6
of 6 crashed before, 0 of 6 after; the original test 0 of 16 after. The
campaign was restarted on each later tree; the record below is the final
tree's. Four hours seven minutes: default set GIL on 40 passes and GIL off 33
passes, CVE set GIL on 109 and GIL off 99 passes, ten seeds per test per pass
— about 341,000 amplified runs. No crash, no hang, no unexpected exit code,
no core file. The harness flagged the same output-divergent tests as in every
earlier campaign, and only those: the six `scaling/` workloads,
`heap-bench-allocation.js`, `jit/int-gate-stop-budget.js`,
`vmstate/dump-registers-gil-on-vm-in-gil-off-process.js` (timings, counts,
addresses), and in the CVE set `mc-aint-poll-resume-stale-elided.js`,
`mc-tear-generator-resume.js`, `mc-tear-date-cache.js`,
`mc-tear-typedarray-detach-grow-shrink.js`, `mc-spec-timer-capability.js`,
`mc-code-deferred-fire-stale-window.js`, `mc-tear-rope-resolve-race.js`
(interleaving-dependent output by construction); plus one run in 330 of
`giloff-time-limit-terminates-one-thread.js`, whose "a 10 ms limit passes
while the call sleeps 50 ms in native code and returns before any trap check"
premise the amplifier's injected yields break (the call met a poll after the
deadline and was terminated, as it then should be). The fourth round's
unexplained SIGTRAP (`cve/mc-grow-s4-detach-nullvec-repro.js`) did not recur
in 990 amplified GIL-off and 1,090 GIL-on runs with core dumps enabled; the
view/detach registration race fixed this round (above) is on that test's
path and remains the most plausible cause. The intermediate campaigns on
earlier trees of the round found the two defects reported above (the
`create_this` pair, 6 of 10 seeded runs; the collection-waiter park, 2 of
120) and nothing else. TSanJIT under the amplifier, default set, both modes,
ten seeds per test: 0 report files; the flags were the same timing-printing
divergences, one stop-latency diagnostic line under TSAN's slowdown, and
three 60-second amplifier timeouts of the two-thread heap-snapshot test,
which takes 50-70 s under TSAN and completes (TSAN-RESULTS).

**B. Performance** (`PERF-RESULTS.md` has the tables and commands).

- **B1, flag off = main.** The 15-20 % flag-off regressions on object
  creation, `Map`, `RegExp` and `throw` at the start of the round were
  unconditional relaxed atomics on hot words (`std::atomic` relaxed is a plain
  load on x86-64 but the compiler may not combine or hoist it), GIL-off arms
  inlined into always-inline bodies (hot functions doubled in size and fell
  out of their callers' inlining budget), non-`constinit` thread-locals (a
  wrapper call per access), and an out-of-line `currentButterflyTID()`. After:
  1.00-1.04 of `main` on 20 of 22 micro-benchmarks; `regexp-exec` 1.10 (two
  helper calls per match that `main` inlines, reason recorded); `class-ctor`
  1.06-1.08 (inside this host's run-to-run band; instruction counts within
  2 %).
- **B2, cached transitions flag-on.** Baseline ICs, DFG and FTL (including
  `MultiPutByOffset` and allocation sinking) emit the non-reallocating
  property-add transition under one owner test (SPEC-jit §5.5): `{a,b,c,d,e}`
  in a loop went from 370x flag-off to 1.1x, escaped-object adds to 1.05-1.09x,
  the transition-heavy constructor to 1.25x, a polymorphic-construct benchmark
  to 2.5x (from 17x). Tests: `jit/transition-ic-owner-and-foreign.js`,
  `jit/transition-ic-vs-foreign-indexed-install.js`,
  `jit/transition-ic-vs-foreign-structure-transition.js`,
  `jit/dfg-transition-check-owner.js`,
  `objectmodel/n1i-instance-keyed-ownership.js` (each asserts both the
  fast-path result and that a foreign thread's racing transition or install
  is never lost).
- **B3, GIL-off scaling.** Two threads creating objects independently ran 230x
  (adds through a function) and 400x (object literals) slower than one thread.
  Cause: ownership of a butterfly-less object was keyed on the SHAPE's
  creating thread (N1), so every other thread was foreign to its own fresh
  `{}`, fired the root shape's thread-local sets on its first add, and from
  then on every thread took the locked path for plain objects; r13's
  chain-fire spread one such fire over the whole shape family. Rev 16 keys
  ownership on the instance — the allocating thread's TID is stamped into the
  butterfly word at birth, butterfly or not, by every C++ constructor and JIT
  allocation — and withdraws the chain-fire. 2M iterations of four adds on
  1/2/4/8 threads: 6.8/7.9/7.4/9.9 ms (was 8/1920/2100/4206); object literals
  21 ms at 2 threads (was 6343). Also GIL off: the shared heap's heap-lifetime
  fenced write barrier is dropped on x86 (kept on weakly ordered targets and
  under concurrent shared marking), allocation profiles carry the size class's
  TLC slot so profiled allocations stay inline, OSR exits no longer issue a
  serializing instruction per exit (`throw` in a loop 3.3x -> 1.15x of GIL
  on), `Structure::get` on a mutator materializes a property table instead of
  walking the transition chain per call, the virtual-call path lost a
  refcount bounce and `operationCreateThis` a `.prototype` lookup, the Yarr
  interpreter allocates backtracking state per thread. Scaling gate, GIL off,
  speedup at 2/4/8 threads (PERF-RESULTS §2): ray tracer 1.6/3.0/4.2 with
  serial parity, splay 1.9/3.6/5.7, `Map`-heavy 1.9/3.0/4.4 from a 2.3x
  serial base (cell-locked reads), string-heavy 0.86/0.75/0.79 (does not
  scale: computed-string-key puts through the process atom table; recorded),
  Richards pathological in stock JSC too and dominated flag-on by the
  per-structure F2 demotion once two threads share its shapes.
- **B4, ArrayStorage shift/unshift.** Owner-only in-place element moves under
  the cell lock (§4.6 AS-INPLACE) replace the fresh-butterfly-per-call copy on
  arrays the calling thread owns: draining a 20,000-element ArrayStorage array
  by `shift()` 227 -> 30 ms flag-on (flag-off 1.4 ms: its O(1) head move
  relocates the header a stale lock-free reader decodes and stays excluded
  until ArrayStorage reads carry an owner test; recorded as the follow-up),
  `unshift`/`pop` churn 20 -> 1.1 ms (flag-off 0.4).
  `objectmodel/arraystorage-shift-unshift-inplace-owner.js` checks a foreign
  lock-free reader sees only elements or holes while the owner shifts, and
  that a foreign shifter still copies.
- **B5, smaller taxes.** GIL-on `CheckTraps` is modelled as flag-off (no heap
  clobber; cloneable, so loop unrolling works again); the loop, read and call
  rows that remain at 1.6-2.0x against default `main` are 1.00x against `main
  --usePollingTraps=1` — the flag requires polling traps (I21) and that is the
  whole difference on those rows. GIL-on M8 no longer forces the fenced write
  barrier (`class`-constructor loop 157 -> 86 ms, `throw` 115 -> 89, `RegExp`
  98 -> 85). Remaining GIL-on costs with an engine cause, against `main` with
  polling traps: polymorphic construct 2.5x, `JSON.stringify` 1.45x and
  `JSON.parse` 1.2x and megamorphic access 1.25x (VM-global caches disabled
  flag-on), `RegExp` 1.35x, `throw` 1.25x, `Map` 1.25x. Found late in the
  round by the scaling suite rather than the micro set: reads of global
  PROPERTIES (`Math`, `JSON`, constructors, `globalThis.x`) were uncached in
  every tier flag-on (scope metadata frozen since the first review round) —
  50x on a bare read, 14x on `Math.sqrt` in a loop, 1.2-2.8x single-threaded
  on four of the five scaling workloads. Gets are cached again with an ordered
  metadata publish (F24 above; x86-64): the ray tracer's single-thread time
  went from 2.8x flag-off to 1.05x, Richards from 2.7x to 1.01x.

**A5. Convergence.** The claim is not that the GIL-off engine has no bugs
left; it is that each independent way of looking that this work has used has
stopped producing new ones on the final tree, and that the ways of looking are
different enough in kind that their agreement means something. The evidence,
by method:

1. *Written tests* (`JSTests/threads/`, every fix of five rounds has one):
   final tree, Release and Debug+ASAN, GIL on and off, default and CVE sets —
   318/303/62/48 passed, 0 failed in both builds; TSanJIT both modes — 303
   and 318 passed, 0 failed, 0 report files; `verifyConcurrentButterfly=1`
   over the object-model tests, both modes, 0 failed. (The campaign, mirror
   and suite passes below ran on the tree of F19; F20-F24 and the FTL fence
   change came after them, each with its own test, and the corpus in all three
   builds plus one amplifier pass of the default set were rerun on the final
   tree: same counts, 0 failures, 0 reports, divergences only in the known
   timing-printing set.)
2. *The amplifier* (random yields at every poll, park and lock site; ten
   seeds per test per pass, four modes, four hours, about 341,000 runs, core
   dumps on): no crash, hang or unexpected exit; output divergences only in
   the known timing-printing set. Two intermediate campaigns on earlier trees
   of this round each found one real defect within their first hour (A4),
   which is the evidence that the method still bites when there is something
   to bite. The fourth round's unexplained SIGTRAP did not recur in about
   2,000 amplified runs of its test with cores enabled.
3. *Borrowed coverage* (the mirror: 5,752 stress tests on two threads, three
   builds): the first passes found eleven engine bugs (F3-F15 and the TSAN
   fixes), of kinds the written tests had not imagined — first-use races in
   watchpoint installs, a publication order in `defineProperty`, a parser
   cache outside its lock. The first "final" Release pass found two more
   (F20, F21 — one a regression of this round's own object-model change, one
   an IC-generation invariant that was single-threaded), the pass after their
   fixes found none; the Debug and TSanJIT passes on that tree found one
   more each (F22, a Debug-only assertion with a benign Release outcome, and
   F23, a registration race in the FFI context), fixed and verified by test. What the mirror cannot see is also clear: it shares everything, so
   it says nothing about objects that stay thread-local (the performance work
   covered those), and its load is JSC's own test corpus, not an application.
4. *Reading* (the static sweep of the nine directories no earlier round had
   walked, PRE-17; the Bun bindings): five engine changes, all in code no
   test reaches from the shell (IC-time table growth, the Yarr allocator
   lock, inspector and debugger hooks); the rest dispositioned in writing.
5. *An application* (Bun's twelve test directories with a second JS thread
   alive, GIL on and off, Debug): GIL on matches flag off to within a test or
   two per directory; GIL off ran to completion in every directory with the
   extra failures accounted for by class (WebAssembly disabled, Debug-only
   collection cost, heap-count assertions perturbed by the extra thread) and
   turned up two start-up assertions, one liveness bug (F19) and one
   embedder-API gap (`isLiveCell`) on the way. This is the weakest leg: Bun's
   own natives do not run on spawned threads yet (A3), so the second thread
   exercises the engine's two-mutator machinery (stops, shared collection,
   per-thread caches, barriers) under Bun's workload rather than Bun's code
   on two threads.

What would change the statement: a sixth method. The two obvious ones not
used are a fuzzer that generates two-thread programs (differential against
the same program run sequentially), and a second application that actually
computes on spawned threads. Both are listed under "Open items". The latency
class (a thread in a long native loop delays other threads' stops, as it
delays GC in the stock engine) is a property, not a bug count, and is
recorded there too.

### Results, sixth round (2026-09-07)

One goal: performance, without giving back any of the safety the earlier
rounds established. The tasks were PERF-RESULTS §5's list in its order —
stop-free indexing-shape relabels and in-place growth for owned arrays,
owner transitions that survive the F2 fire, the megamorphic and JSON caches
GIL on, `Map`/`Set` reads without the table lock GIL off, the reallocating
transition in the JIT tiers — and each was done as protocol work first
(SPEC-objectmodel rev 17, history §25-§26 and addendum; SPEC-jit §5.5 rows and
history §29-§31; SPEC-ungil §N.1 and its history; SPEC-heap §10B.5 and history
§27), then C++ and every JIT tier, then a test that fails on the fifth-round
binary and passes now, then the corpus in four modes. Looking for the cost
found defects that had been in the tree since the second to fifth rounds —
a GIL-off crash class (F25), two performance defects each large enough to
dominate a benchmark (F26, F27), a GIL-off deadlock (F28) — which are
reported with the task that led to them, and the final passes found three
more that are recorded and not fixed (F29-F31). Numbers are Release, Linux
x86-64; "before" is the fifth-round final binary. Net, JetStream (PERF-RESULTS
§3): GIL on 0.77 -> 0.89 of flag off; flag off 0.97 of `main` (unchanged);
GIL off 229 -> 236 absolute, with one test 4x down for a stated reason.

**B1. Indexing-shape relabels of owned arrays without a stop (SPEC-objectmodel
T4-O, history §25).** Flag on, every Int32-to-Double, Int32-to-Contiguous and
Double-to-Contiguous relabel of an array — one thread or many — was a
stop-the-world (4,000 stops for 4,000 relabels in the test below), and no
butterfly ever grew in place. The revision first records why the proposed
"owner-only copy-convert whose publication order lets a racy reader misread a
lane only as a double" cannot be had: a GIL-off reader's shape check may be
arbitrarily stale (the DFG hoists it across polls), so no publication order —
not even a double-word CAS — prevents it pairing the OLD shape with the NEW
storage, and an Int32-keyed reader over Double storage reads raw double bits
as a JSValue. What can be had: GIL on, every typed-source relabel in place
(no thread runs between another's polls); GIL off, Int32-to-Contiguous in
place (an Int32 lane IS a valid JSValue lane), Int32-to-Double executed as
Int32-to-Contiguous (the array simply never becomes a Double array; the
value is boxed), Double-to-Contiguous keeps its stop. The consequence GIL off
is I41: a reader keyed on a stale Int32 shape may now see a non-Int32
JSValue in a lane, so the DFG/FTL Int32-mode loads verify the lane (one
compare, GIL off only), the C++ Int32 fast paths tolerate it, the
copy-by-`memcpy` paths that assumed Int32 lanes check them when the source is
foreign-owned, and the collector visits Int32 butterflies GIL off. The relabel
itself is the claim-first form (CAS StructureID S -> nuked S, rewrite lanes,
fence, store S'), lock-free while the structure's sets are valid and under
the cell lock once they are dead (the rev-16 hole this closes: two owners of
one segmented word could otherwise both relabel). In-place growth of owned
butterflies (`canReallocInPlace`, M8) is back GIL on.
`objectmodel/typed-owner-relabel-no-stop.js` (stops per relabel kind, shapes,
a foreign reader/copier racing the owner's relabels and stores): "4000 stops"
on the fifth-round binary in both modes, passes now. JetStream GIL on:
`stanford-crypto-pbkdf2` 0.40 -> 0.84 of flag off, `-sha256` 0.37 -> 0.84,
`Air` 0.67 -> 0.91, `ai-astar` 0.66 -> 0.92 (PERF-RESULTS §3; the target was
0.9: `pbkdf2`'s remainder is its `Array.prototype.concat`/`slice` traffic
through the flag-on C++ copies; `Air` and `ai-astar` owe most of their
movement to B3 and B5). GIL off the same tests went 3.4x UP (`pbkdf2`,
`sha256`) and one went 4x DOWN: `stanford-crypto-aes`, whose double-valued
literals give it Double arrays next to the Contiguous ones the GIL-off
Int32->"Double" substitution produces, so the DFG arrayifies Double to
Contiguous at the shared sites — the one relabel that keeps its stop GIL off
(PERF-RESULTS §3 and §5 item 1; SPEC-objectmodel history §26 addendum 2 has
the analysis and the fix candidates).

- **F25 (GIL off, since the second round). `Array.prototype.join`'s two-pass
  joiner overran its buffer when a foreign writer replaced strings between
  the measuring pass and the copying pass** (`std::span::first` assertion /
  `RELEASE_ASSERT`, 3 of 3 on a 2,048-element array with a writer flipping
  every lane between a short and a long string). Found while auditing the
  Int32 fast paths for I41. GIL off the empty-separator and
  `JSOnlyStringsAndInt32sJoiner` fast paths are skipped (the general joiner
  measures and copies each element once). `objectmodel/join-two-pass-vs-
  foreign-writer.js`: 3 of 3 abort before, 0 of 20 after.

**B2. Owner transitions after the F2 fire: claim-first everywhere
(SPEC-objectmodel E4-C, history §26).** The fifth round left "F2 is per
structure" open: the first cross-thread transition of any object of a shape
retired that shape's claim-free owner path for every object and every
thread, for good, and nothing cached the transition again (a `{}.a.b.c.d`
loop 20x slower on the main thread after another thread extended ONE such
object). The fire has to stay (it is what makes the claim-free owner window
sound), but the post-fire regime does not: once every writer of an object's
StructureID lane CLAIMS it before writing anything another claimant can
reach, the lane itself is the exclusion. Two things were missing and are now
in: the cell-locked writers (§4.3's stay-flat legs, locked N2) stored the
value before their nuke CAS and asserted the CAS — they now claim first and
RESTART on a lost claim; and the owner had no claim-first leg once the sets
were dead — E4-C (owner tag test, CAS S -> nuked S, word re-check, store,
fence, S'). In the JITs the inline caches' butterfly-bearing transition leg
now claims like the butterfly-less one, so a cached transition watches no
thread-local set and stays valid after the fire, and `tryCachePutBy` accepts
fired sources; the DFG keeps its claim-free inline form under watched sets
and otherwise plants the IC. `objectmodel/owner-transitions-after-fire-
claim-first.js` counts cell-locked transitions after the fire (a new `$vm`
counter): 10 M owner transitions, 0 locked (every one before); and races
owner adds (JIT, claim-first) against foreign adds (locked, claim-first) on
the same 2,000 objects with no add lost. The 2 M-iteration loop after the
fire: 619 -> 88 ms GIL on (31 ms before the fire: what remains is inline-DFG
versus inline-cache, not locked versus lock-free). The scaling suite's
Richards "second thread after the first" figure that motivated the item
turned out to be dominated by B3's megamorphic cache and by run order, not by
F2 (PERF-RESULTS §2).

**B3. The megamorphic cache and the JSON fast paths GIL on (SPEC-jit history
§30).** The VM-global `MegamorphicCache` and `Structure::forEachProperty`'s
lock-free walk (the `JSON.stringify` fast path, `Object.assign`) were disabled
per flag; the reason — unsynchronized multi-word fills — exists only GIL off.
They are now disabled per process mode. The probes learned the tagged
butterfly (`loadPropertyTagged`, `storePropertyTagged`), and the store probe's
transition arm is B2's claimed sequence with runtime refusals
(PreciseAllocation, copy-on-write, ArrayStorage), so it needs no per-structure
watchpoints; its reallocating arm's operation completes through the
object-model protocols. Micro rows GIL on against the polling baseline:
`megamorphic-access` 1.27 -> 0.94, `json-stringify` 1.44 -> 1.05; JetStream
`FlightPlanner` 0.41 -> 0.83 of flag off, `Babylon` 0.53 -> 0.83,
`typescript` 0.46 -> 0.81 (with B5), `json-stringify-inspector` 0.94 -> 1.07.
GIL off keeps the caches off; the per-thread cache design is recorded in the
history entry, not built.

**B4. `Map`/`Set` reads without the table lock GIL off (SPEC-ungil §N.1 and
history; AUDIT PRE-1's recorded follow-up).** `has`/`get`/`size` are
seqlock-validated lock-free reads: an owner version word that every writer
brackets under the table lock, a reader that trusts nothing until the
version re-check (bounds from the cell's immutable length, obsolete and
scribbled-header detection, int32-only links, a chain bound) and falls back
to the lock after four tries. Four threads reading one shared Map: 2,022 ->
318 ms (one thread: 187). `shared-objects/map-lock-free-readers.js` (torn-read
freedom against a writer that inserts, overwrites, deletes, clears and
rehashes; read scaling 16x -> 1.5x of one reader). The other half of the item,
"string-keyed puts do not scale", was pinned down and is not about strings:
see Open items (shared tier-up counters).

**B5. Reallocating transitions in every tier (SPEC-jit §5.5 (RE)ALLOCATING
form, history §31); PERF-RESULTS §5 item 7.** The add that grows or first
installs out-of-line storage was C++-only flag-on in every tier; profiles of
`typescript`, `Babylon` and `ai-astar` GIL on had it on top. Now cached in
the inline caches (claim-first, with a word re-check under the claim because
the grown storage is copied before it) and inlined by DFG/FTL under the
watched sets (E4's plain order, an `InvalidationPoint` between allocation and
install; the value store into the fresh storage uses it directly and never
exits; GIL off the caches allocate through the thread's TLC slot and the FTL
`MultiPutByOffset` keeps refusing reallocating variants). The ai-astar-shaped
micro (construct, six adds of which two allocate, five rounds of replaces):
7.0x -> 1.2x of flag off GIL on with top-level functions; 3.6x when the
constructor and helper are closures made per run (poly-proto structures,
so the handlers are compiled per case, and the per-case allocating form
still calls out — PERF-RESULTS §1.2).

Found by B5's profiles, all older than this round:

- **F26 (every mode, since the second round). An out-of-line Replace never
  cached in baseline or DFG-generic code.** The flag-on `put_by_id` call-site
  fast path stored inline offsets only and sent out-of-line ones to the
  handler chain, but a monomorphic Replace handler is installed as the site's
  inlined handler, not into the chain; so every out-of-line replace called
  the optimize operation, whose new Replace case was refused as a duplicate,
  forever (10x on a loop of out-of-line replaces to the thread's own
  objects). The call-site fast path now handles out-of-line offsets with a
  scratch-free owner test (x86-64: the TID tag xor'ed from thread-local
  storage as a memory operand), and where that form does not exist such a
  Replace goes into the chain. `jit/put-by-id-replace-out-of-line-cached.js`
  (two shapes, FTL off): 7.9x -> 1.0x of the inline-offset loop.
- **F27 (GIL off, since the fifth round's fence change). The shared heap raised
  the "mutator should be fenced" flag when it became shared and only the end
  of the first collection's marking lowered it**, so every JIT write barrier
  took its store-load-fenced slow path from start-up to the first GC (2.2-2.5x
  on a put loop that never collects; SPEC-heap history §27). The flip keeps the
  idle value on x86. `scaling/write-barrier-idle-fence.js`: 2.5x before, 1.0x
  after.
- **F28 (GIL off, since the fifth round). A jettison reached from the shared
  collector's conductor took the GIL-off compilation lock and deadlocked**:
  `installCode` skips that lock when the world is stopped, but tested the
  per-VM stop flag, which is false inside a shared-server stop the current
  thread conducts as mutator; a Class-A watchpoint fire from the conductor's
  end-phase work jettisoned a CodeBlock, `installCode` spun on the lock, and
  its holder was another thread parked for that very stop inside
  `prepareForExecution` (Debug corpus, `cve/mc-tdwn-tid-recycle-storm.js`,
  1 hang in 8 runs on the fifth-round binary too). The exemption now also
  covers "all clients stopped and this thread is doing the GC work", the
  test `stopTheWorldAndRun` itself uses for its inline-execution licence.
  0 of 30 after.

Found by the final passes and NOT fixed (both predate the round; see Open
items):

- **F29 (GIL off, multi-VM). `VMManager::enterStopTheWorldParticipation`
  release-asserted `m_numberOfStoppedVMs + m_numberOfBlockedVMs <=
  m_numberOfActiveVMs`** on the keep-alive thread's `Atomics.load` poll while
  Bun `worker_threads` came and went (the Bun GIL-off pass, once in four runs
  of `node/worker_threads`; reproduced with a 60-worker `process.exit(0)`
  script 1-4 of 8, and equally on the fifth-round Bun binary). A stop census
  (`m_numberOfActiveVMs`) taken while a worker VM that is inside a blocking
  scope is torn down is the suspect; VMManager's multi-VM accounting was not
  otherwise exercised by this branch's shell tests, which run one VM.
- **F30 (GIL off). The fourth round's `cve/mc-grow-s4-detach-nullvec-repro.js`
  SIGTRAP recurred** twice in about 3,000 amplified runs of that test (it did
  not in the fifth round's 990). The core files identify it: `FTLCrash` from
  `safelyInvalidateAfterTermination` — execution reached a block the FTL
  emitted as unreachable because its abstract interpreter had proven the
  preceding node exits — in three threads at once at the same address of
  the test's shared hot function, while a typed array's buffer was being
  transferred on another thread. The seeds do not reproduce it (0 of 6). An
  abstract-interpreter proof taken from concurrently mutable state (the
  view's length/vector during the detach race) without a watchpoint is the
  hypothesis; not confirmed.

Caught inside the round by its own verification, on code the round had just
written (recorded because the mechanisms are instructive, not as findings):
the megamorphic load probe's tagged read wrote its result register — which
in the data-IC handlers is `handlerGPR`, dereferenced by the fall-through —
before its slow-case branch (Release corpus, four tests GIL on); the
`convert*` family's Debug shape assertions and `tryCachePutBy`'s "structure
is the transition target" assertion became reachable through legal races
(Debug corpus); and E4-C's growth path re-loaded the butterfly word inside
`allocateMoreOutOfLineStorage` (asserting it flat) while, with the sets dead,
a foreign thread may segment the object at any moment, and §4.2's conversion
still stored the new value into a possibly ALIASED fragment before its claim
— together an aliased add (`o.g` reading the foreign thread's `o.f` value)
that the Release mirror pass found on `stress/regress-187060.js` and two
other files; fixed by growing from the loaded word and by moving the
conversion's value store after its claim, with
`objectmodel/e4c-growth-vs-foreign-segmentation.js` (6 of 6 before, 0 of 10
after, also with the JITs off). And the first cut of B5's DFG/FTL install put
`NukeStructureAndSetButterfly` before the value store: when the value is a
sunk allocation, its materialization — an allocation, hence a possible
collection — then ran with the object's header nuked (the GIL-on suite's
`ftl-eager` run of `stress/materialize-past-butterfly-allocation.js`: "GC
scan found object in bad state: structureID is nuked"); the install now
follows the value store with the `InvalidationPoint` immediately before it,
and `jit/realloc-transition-inline-vs-materialization-gc.js` (that test's body
under the flag and the heap verifier) aborts before, passes after.

**Verification on the final tree** (Linux x86-64; the tree includes every fix
above and the findings sections' tests).

- The corpus (`Tools/threads/run-tests.sh`, default and `--cve`, GIL on and
  off): Release 309 + 326 + 48 + 62 pass, 0 fail; Debug+ASAN the same four
  counts, 0 fail; TSanJIT the same four counts, 0 fail, 0 reports (one new
  suppression, TSAN-RESULTS "Sixth round"). The corpus was also run after each
  task on that task's tree; the failures those intermediate runs produced are
  the in-round catches listed above.
- Touched-area tests under the amplifier, 500 seeded runs each, both modes:
  `typed-owner-relabel-no-stop`, `join-two-pass-vs-foreign-writer`,
  `owner-transitions-after-fire-claim-first`, `e4c-growth-vs-foreign-
  segmentation`, `put-by-id-replace-out-of-line-cached`, `write-barrier-idle-
  fence`, `map-lock-free-readers`, and the older `map-set-shared-writers`,
  `i03-i37-same-shape-add-storm`, `structure-only-transition-races`,
  `transition-ic-owner-and-foreign`, `no-torn-shapes`: no crash, timeout or
  unexpected exit in any run. The four tests that assert a time ratio
  (`owner-transitions-…`, `put-by-id-replace-…`, `write-barrier-idle-fence`,
  `map-lock-free-readers`) diverge in output under injected yields by
  construction, like the `scaling/` set; their non-timing assertions held.
- Amplifier campaign, four modes in parallel for 1 h 53 min on the Release
  build with core dumps on: default set GIL on 19 passes and GIL off 15, CVE
  set GIL on 56 and GIL off 49, ten seeds per test per pass — about 165,000
  amplified runs. No timeout, no unexpected exit code. Two crashes, both
  `cve/mc-grow-s4-detach-nullvec-repro.js` GIL off (F30 above, pre-existing).
  Divergent-output flags: the fifth round's list unchanged (the six `scaling/`
  workloads, `heap-bench-allocation.js`, `jit/int-gate-stop-budget.js`,
  `vmstate/dump-registers-gil-on-vm-in-gil-off-process.js`, the seven
  interleaving-dependent CVE tests), plus this round's five timing-assertion
  tests, whose diagnostic prints were made deterministic afterwards.
- Mirror harness, Release, eval mode, every `JSTests/stress` file on two
  threads: 5,752 files; 3,475 completed, 2,223 threw (the test's own
  assertions under two copies; which files throw varies run to run by
  hundreds), 34 spun to the deadline, 20 others — the fifth round's artefact
  list exactly (seven `$vm`-hook and three type-profiler tests, nine
  `waitAsync`/agent files blocked, one exiting 1). No engine finding on the
  final tree; the pass on the tree before the last E4-C fix is what found the
  aliased add above.
- JSC suites (`run-jsc-stress-tests`, Release, the same seven collections,
  24 children, with the Bun tests and the amplifier running beside them):
flag off 583 failures against the fifth round's 580 (three FFI `ftl-eager-no-cjit`
  entries that move every round, `re-enter-resolve-rope-string.js.no-ftl` and
  `regress-174463162.js.dfg-eager` killed by the OOM killer under the
  concurrent load); GIL on 855 against 856 (FFI `ftl-eager-no-cjit` moves and
  two load-sensitive configurations of `int8-repeat-in-then-out-of-bounds.js`,
  five old ones passing); GIL off 1,200 against 1,196 (five FFI moves, three
  `int8-repeat` configurations, four old FFI entries passing). The GIL-on run
  on the tree before the last install-order fix had two more,
  `materialize-past-butterfly-allocation.js.ftl-eager` and
  `.ftl-eager-no-cjit` — the catch described above; they pass on the final tree.
- Bun (Debug, `bun run build:local` against this tree with the nine-change
  patch, twelve test directories, the second-thread preload GIL on and off):
flag off / GIL on / GIL off per directory, this round then (fifth round):
  `bun/jsc` 262-2 / 262-2 / 244-20 (261-3 / 259-5 / 241-23), `bun/ffi` 232-0
  in all three (same), `bun/util` 2038-6 / 2038-6 / 1879-13 (same), `node/vm`
  292-0 / 292-0 / 291-1 (same), `node/util` crashes flag off already (rc 139;
  rc 1 in both flag-on modes; same), `web/timers` 69-4 / 69-4 / 67-6 (same),
  `node/worker_threads` 156-0 in all three (same; one of the four GIL-off runs
  made this round aborted on F29), `web/workers` 455-2 / 455-2 / 451-6 (same),
  `node/fs` 817-2 / 818-1 / 816-3 (817-2 / 817-2 / 816-3), `node/http` 698-4 /
  698-4 / 696-5 (697-4 / 697-4 / 696-5), `web/fetch` 11417-4 / 11412-9 /
  11379-42 (11417-4 / 11417-4 / 11381-40; the GIL-on run's five extra are
  network-timeout tests — `should work with ipv6 localhost`, `simultaneous
  HTTPS fetch`, TLS-extension and shutdown timing — that ran while the JSC
  suites loaded the machine), `bun/http` 2814-2 / 2814-2 / — (2814-2 / 2814-2 /
  2805-11): the GIL-off `bun/http` run ended in a Bun panic, F31 below. GIL on
  with a second thread alive stays flag-off to within a test or two per
  directory; GIL off the extra failures are the fifth round's classes
  (WebAssembly-dependent tests, collection-polling leak tests in the Debug
  build, `heapStats` string counts with the keep-alive thread allocating).

- **F31 (GIL off, Bun-side; recorded, not an engine change). A Bun cell
  type's destructor ran on the thread that conducted a shared collection**:
  the keep-alive thread swept a test-runner `Expect` object whose Rust
  `RefPtr` is thread-locked to the main thread, and Bun panicked
  ("`ThreadLock` is locked by thread A, not thread B", `bun/http` GIL off).
  With the shared heap any client thread can conduct a collection and run
  destructors and unconditional finalizers (SPEC-heap §10; SPEC-nativeaffinity
  exempts finalizers from the native lock by design), so an embedder's cell
  destructors must not assume the allocating thread — the destructor half of
  the "Bun natives on JSC-spawned threads" open item, which so far listed
  only host-function calls.
- Performance: PERF-RESULTS §1-§3 and §5 re-measured on the final tree
  (quiet machine, medians of 5-10 runs): JetStream `main` 352.2, flag off
  342.1 (0.971), GIL on 305.4 (0.893 of flag off; fifth round 0.768), GIL off
  235.5 (0.771 of GIL on); the micro table's flag-off column 0.98-1.12 of
  `main`; the scaling suite's serial ratios and GIL-off speedups within noise
  of the fifth round's except raytrace-like at 8 threads (4.24 -> 4.53).

**Flag-off changes.** None of the above changes flag-off code generation:
every new emitter arm is behind the flag test the site already had; the C++
changes are in flag-on branches or (F25, the `Map` version word, the heap
flip) in GIL-off ones; `JSMap`/`JSSet` grow by one word; `InlineCacheHandler`
gains an accessor; two x86-64 assembler forms were added (`xorq` with an
absolute address, used fs-relative). The flag-off column of PERF-RESULTS §1
and §3 was re-measured (§ there).

### Results, seventh round (2026-09-08)

One theme: GIL off was 0.77 of GIL on on JetStream while the design intends
GIL off to be the fast mode. The round began with a measurement pass over the
whole difference (PERF-RESULTS §6: per-test ratios, tier dwell, cycles and
instructions, a diagnostic event-counter facility built for it -
`--reportJSThreadsCounters`, off by default - and profiles of the worst
tests), which replaced the earlier guesses with a ranked ledger of mechanisms,
and then worked the ledger in order. Each change was written into the
specifications first (SPEC-heap §10E and history §28; SPEC-jit §5.8 and
history §32-§36; SPEC-objectmodel rev 18 and history §27; SPEC-ungil history,
three entries), then C++ and the JIT tiers, then a JSTests/threads test that
fails or shows the old count on the sixth-round binary and passes now, then
the corpus in four modes. The measurement corrected two beliefs the round
started from: the FTL structure-check poison that PERF-RESULTS §5 ranked first
costs about 1 % (removing every poll clobber moves no test by more than
noise), and the GIL-off deficit is not one mechanism but a dozen 3-8 % items
plus three test-specific collapses. The final passes (stress suite GIL off,
mirror harness, amplifier campaign) then found four defects in the round's
own changes and two older ones; one change (P3) was withdrawn on that
evidence. Numbers are Release, Linux x86-64; "before" is the sixth-round final
binary. Net, JetStream (PERF-RESULTS §3): GIL off 0.83 of GIL on (sixth
round 0.77; absolute 248.0); GIL on 0.86 of flag off by the five-run medians (runs 0.85-0.89; sixth round 0.89, runs 0.87-0.91 - the GIL-on mode is bimodal on several tests and an A/B of the round's flag-on JIT change on the tests that moved shows no difference); flag off 0.98 of `main`.

**P1. The shared heap kept no blocks between eden cycles (SPEC-heap §10E,
history §28).** GIL off (the heap is shared from the first thread), the end
of every collection - eden included - freed every empty marked block down to
the allocation budget and the next cycle minted them again through the
kernel: `splay` minted 94,000 blocks per JetStream run against 3,900 GIL on,
`Basic` 39,000 against 2,300, and page-fault/`madvise` kernel time was 10-17 %
of those tests. Eden cycle ends now retain the block set the program cycles
through (the heap size at the last eden start, or the budget) and shed only
what lies 25 % above it, half the excess per cycle; Full cycle ends keep the
sixth round's rule (capacity 50 % over the live size just measured -> the
synchronous sweep and a full shrink). Two intermediate forms were measured and
rejected: holding the pre-Full peak as well cost 3.7x the resident set of the
sixth round's policy on the scaling suite's splay-like at eight threads
(932 MB against 254 MB), and dropping the Full-cycle sweep too left an
embedder's idle server at 3.0 GB resident where the sixth round kept under
0.8 GB (Bun's `serve-body-leak` tests GIL off: eden cycles never sweep
destructible blocks, so without the Full arm nothing freed them). Final form:
`Basic` mints 2.3 k blocks per run (39 k), `splay` 70 k (94 k; 37 k without the
Full arm, which its eighteen Full collections per run now pay for), splay-like
at eight threads peaks at 257 MB. Test: `heap-shared-retains-blocks.js`
(steady phase mints 25,000+ blocks before, under 200 after).

**P2. Virtual calls took the slow path on every call GIL off (SPEC-jit history
§32).** The sixth round's arity-check mirror published only one of the two
entry words; the virtual-call thunk reads the other, found it null GIL off,
and called `operationVirtualCall` per call (2.5 M per `Basic` run, 1 M in
`Babylon`). `installCode` now publishes both, retract-first / publish-last.
Test: `jit/virtual-call-fast-path-gil-off.js` (2,000,000 slow-path virtual
calls before, 3 after; timing ratio checked in Release only).

**P3 (withdrawn). Dictionary prototypes are still never flattened GIL off
(SPEC-jit history §33).** The refusal costs `Air` 401 k generic loads per run
and shows in `Babylon` and `typescript`. A deferred flatten (request under the
IC lock, flatten after it is released, the site re-caches) was built, measured
(4 M generic loads -> none on its test) and carried through most of the round,
then withdrawn when the final amplifier campaign crashed about 1 run in 50 of
`jit/ic-condition-stale-at-generation.js` GIL off (a stub reading an
out-of-line slot through a null butterfly) and bisection over the round's
binaries put it on this change (0 of 150 amplified runs on the binary before
it, 3 of 150 with it, 0 of 150 with it reverted): the flatten runs
world-stopped but keeps the StructureID while it renumbers offsets and can
drop the butterfly, so what a second thread derived from the prototype's
pre-flatten layout survives the stop. Open items has what a sound version
needs.

**P4. Every RegExp match took the RegExp's cell lock GIL off (SPEC-ungil
history).** `compileIfNecessary` locked to read "is there code for this
width": 27.7 M acquisitions per `regexp` run, 6 M in `FlightPlanner`. The
compile now publishes per-width "code published" bits with release semantics
after the code pointers, the matcher reads them with one acquire load and
locks only to compile, and the provisional `ByteCode` state a compile used to
pass through (visible to a lock-free reader as "interpret", with no bytecode
yet) is no longer stored GIL off. Lock acquisitions per `regexp` run 27.7 M -> 0; the test's score did not move outside its noise (GIL off 0.73 of GIL on in the five-run suite medians before and after; single-test runs range 330-415 in both trees), so the lock was uncontended cost below what the suite resolves, not the 10 % the ledger's cycle share suggested.
Test: `shared-objects/regexp-first-match-publication-race.js` (eight threads
race first matches of fresh RegExps at both subject widths and check every
result; part 2: 200,000 lock acquisitions for 200,000 compiled matches before,
0 after). TSAN flagged the first, fence-based form of the read; the
published-bits form is clean (TSAN-RESULTS).

**P5. Double arrays GIL off: copies are Contiguous, allocation sites learn
(SPEC-objectmodel rev 18 T4-C / T4-P, history §27; SPEC-jit history §34).** The
sixth round left `stanford-crypto-aes` GIL off at 0.19 of GIL on: Double arrays
made by literals and their `slice`/`concat` copies met sites that also see
Contiguous arrays, and every Double-to-Contiguous conversion GIL off is a
stop-the-world (T4-O keeps that stop because a stale Double-keyed reader over
JSValue lanes would box an impure NaN; history §27 records why a validated
copy-publish does not remove it - a stale reader can read pointer bits as a
subnormal double, an information leak). Two rules instead: copies of Double
sources (`slice`, `concat`, spread, the DFG `ArraySlice` intrinsic) produce
Contiguous arrays GIL off, boxing lane by lane during the copy the operation
already makes; and an array allocation profile that observes its arrays
leaving Double fires a per-profile watchpoint, so DFG/FTL code that inlined the
Double allocation is jettisoned and recompiled Contiguous (the JIT tiers
record each allocation in the profile's last-array word and call out only when
the previous array already left Double). `aes` GIL off 0.19 -> 0.62; stops
per run of the profile test 196,969 -> 2. Tests:
`objectmodel/double-copies-are-contiguous-gil-off.js` (176,389 stops and 0.81 s
for 150,000 copies before, 0 after; values checked on three threads),
`objectmodel/double-allocation-profile-feedback-gil-off.js` (196,969 late
stops before, 2 after). The GIL-off stress suite then reports
`stress/array-slice-cow.js` in all sixteen configurations: it asserts through
`$vm.indexingMode` that a slice of a Double array is `ArrayWithDouble`, which
T4-C makes `ArrayWithContiguous` GIL off by design.

**P6. `Map.prototype.get`/`has` and `Set.prototype.has` are inlined again GIL
off (SPEC-jit history §35).** The sixth round made the C++ readers lock-free
but the DFG still refused the intrinsics GIL off, so every `get` was a call
(`Basic`: 5 M runtime reads per run). The FTL now emits the bucket probe
inline as a seqlock reader of the table's version word (read version, probe
with bounds checks against the loaded butterfly's length and a step bound,
re-read version; odd, changed, or an empty key slot -> the runtime reader),
the DFG calls an operation on the lock-free reader, and the load of the
entry's value tolerates the empty and deleted sentinels a racing writer can
expose. `Basic` GIL off 0.46 -> 0.65. Test:
`jit/map-get-has-inlined-gil-off.js` (a writer thread rebuilds the map 60
times under FTL readers that check every value; part 2: 300,000 runtime reads
for 300,000 FTL gets before, 0 after).

**P7. A Class-A watchpoint set nobody watches fires without a stop (SPEC-jit
history §36).** Property-replacement sets that ICs and scope caches arm
speculatively on every structure they cache have no members; firing one
changed a byte and stopped the world. GIL on this was most of the stop
traffic of the IC-heavy tests (`typescript` 3,460 stop requests per run ->
171, `Babylon` 226 -> 24); GIL off at four threads the scaling suite's
string-heavy took 376 such stops per run (-> 2), each parking three threads
and jettisoning their optimized code through the heap-fact epoch. The
thread-local sets whose fire is a claim barrier (SPEC-objectmodel §5) are
fired by their callers inside an explicit stop and are unaffected. Test:
`jit/watcherless-watchpoint-fire-no-stop.js` (five threads; 6,445 stop
requests in the threaded phase before, 0 after, GIL on).

**P8. Shared profiling under N threads (SPEC-ungil history).** Measured on
string-heavy held in Baseline: four threads ran at 0.72x of one. Profile
stores now skip unchanged values (value/arith/array profiles, IC state cells;
the Baseline JIT's profile stores flag-on), the execution counters no longer
share a cache line with the JIT data's global-object and constant-pool words,
and the OSR-exit reoptimization thresholds scale with the live thread count
GIL off. Baseline-held string-heavy at four threads 2,594 -> 1,894 ms; the
tiered run did not improve (below).

**Found by the final passes in the round's own changes, fixed (each with a
test that fails on the binary before the fix):**
- *T4-P, DFG tier.* The report of a fresh array to its allocation profile kept
  the array in a temporary register across the demotion call, which the DFG's
  silent spill does not preserve: a garbage pointer as the node's result
  whenever the call ran (GIL-off stress suite `double-to-int32-NaN.js`, mirror
  harness 23 of 30; single-threaded too). The operation now hands the array
  back into the result register.
  `jit/double-allocation-profile-slow-path-keeps-result.js`: 3 of 3 crash
  before, 0 after. Lesson recorded: the GIL-off stress suite runs right after
  a JIT-tier change, not at the end.
- *P6's probe.* Its fences were emitted as B3 fences that read and do not
  write - store-store fences - so B3 folded the version re-load into the first
  load and the compiled probe validated nothing (found by reading the B3 graph;
  no test could see it directly); and an EMPTY key slot exposed by a writer
  passed `isCell()` into a null type load (about 1 run in 15 of
  `shared-objects/map-lock-free-readers.js`, Release corpus). Load-load fences
  (B3: writes, no instruction on x86-64); empty key -> runtime reader.
- *Adaptive watchpoint re-adaptation* re-reads the object's structure, which
  another thread may have replaced: `install()` found no replacement set at
  its offset (amplifier, `jit/global-property-cache-vs-global-transitions.js`,
  about 1 in 100), and `fire()` running inside a collection's end phase
  (InferredValue clean-up) CREATED the missing set, allocating a
  StructureRareData while the allocator refuses (mirror harness, once in 5,750
  files). A refused install in the first case - that function's stated flag-on
  contract - and no set creation inside a collection phase in the second
  (outside one the set is still ensured; `objectmodel/indexing-transition-
  keeps-adaptive-watchpoint.js` depends on it). Both predate the round; P3
  made the first frequent enough to see.

**Found by the final passes, older (sixth round), fixed: F33, the delete leg
after E4-C (SPEC-objectmodel history §27 follow-up).** r17 let the owner of an
object whose shape's thread-local sets are dead keep transitioning it without
the cell lock, claim-first, and rewrote the cell-locked writers to claim the
StructureID lane and RESTART on a lost claim - except the structure-only
delete leg, which still asserted that only volatile header bits move under the
lock. An owner adding properties to its object while another thread deletes
one from it aborted the process (release assertion; 6 of 6 on the sixth-round
binary). Found by the mirror harness on `stress/delete-by-val-ftl.js` (3 in
100). A moved lane is now a RESTART there and, defensively, in the
attribute-change leg. `objectmodel/delete-vs-owner-claim-first-transition.js`
(abort 6/6 before; pass with 3,000-5,000 lost-lane restarts per run after).

**Found, not fixed (F32).** TSanJIT, GIL off, about 1 run in 20-60 of
`objectmodel/json-stringify-vs-concurrent-transition.js`, and the same on the
sixth-round binary: the main thread's stringifier reads a cell while the other
thread's rope allocation sweeps the block holding it as empty. Read as stated
that is a reachable cell in a dead block; the follow-ups point the other way
(`--scribbleFreeCells` 0 of 100 functional failures, `--useJIT=0` 0 of 30,
`--forceGCSlowPaths=1` 0 of 40 - the signature of a cell initialized in JIT
code, which TSAN cannot see, read in C++ on another thread and paired with the
last instrumented write to its address). Neither reading was proved; it stays
visible (TSAN-RESULTS) and is the first item for the next round.

**Costs of the round's own rules, measured on the final tree (PERF-RESULTS
§3).** Two GIL-off tests went down: `ML` 55.7 -> 44.3 (0.51 -> 0.39 of GIL on)
and `hash-map` 563 -> 467. `ML` is T4-C's price on a program whose Double
arrays never met a Contiguous site: its `slice`/`concat` copies are now boxed
Contiguous arrays and its arithmetic on them boxes and unboxes; T4-C trades
that against `aes`-class stop storms (0.19 -> 0.62) and the trade is recorded
as such in SPEC-objectmodel history §27. `hash-map` was not analysed (it is
allocation-heavy and runs Full collections, so the retained Full-cycle sweep
and the eden retention are the suspects); `Box2D` -5 % and `regexp` -3 % are
inside their run-to-run range. Everything else GIL off moved up or stayed:
`Basic` +33 %, `stanford-crypto-aes` 3.3x, `pbkdf2` +27 %, `earley-boyer`
+21 %, `json-parse-inspector` +20 %, `gbemu` +12 %.

**What was measured and left as is, with the mechanism that blocks it:**
- *GIL off against GIL on, the remaining 0.83.* By test (PERF-RESULTS §3 and
  §6.8): `Basic` 0.65 (generator resume claims are host calls, 2.5 M per
  run; its remaining Map traffic is `set`/iteration, still refused in the
  DFG); `gbemu` 0.66 (generic IC traffic - 1 M GetById, 480 k PutByVal,
  230 k InstanceOf per run - with the megamorphic cache off GIL off; NOT an
  exit storm: both modes take the same 21,300 exits, GIL off merely routes each
  through the operation because the exit jump is never repatched under
  concurrent execution); `FlightPlanner` and the string tests (ropes resolve
  through the cell-locked slow path, 340 k-1.5 M per run; `Air` also pays
  P3's withdrawal); `ML`, `float-mm`, the crypto kernels (T4-O's
  Int32-to-Contiguous substitution keeps doubles boxed where GIL on has Double
  arrays, and the DFG's per-access seg-mode branch and butterfly reload,
  1.29x instructions on `am3`); `splay` (structure transitions and the
  transition-table lock under object churn). Each is a JIT- or
  object-model-level project of the size of P5/P6.
- *GIL on against flag off, 0.86.* Unchanged in kind from the sixth round:
  polling traps on tight loops, `operationCreateThis` for polymorphic
  construct, global writes. New observation: `earley-boyer` GIL on is bimodal
  run to run (500-870 against a stable ~890 flag off); slow runs spend 380 ms
  in the FTL's generic PutById transition handler for one allocation site and
  show twice the recompilations - a tier-up ordering sensitivity, not
  root-caused.
- *Scaling suite (PERF-RESULTS §2).* Clean workloads at four threads:
  raytrace-like 2.94x (eden collections double with four allocating threads
  and each is a full stop, 47 ms of a 275 ms run; a four-times nursery removes
  the pauses and gains nothing because the larger footprint costs the single
  thread as much), map-heavy 3.03x, splay-like 2.93x; at eight threads
  4.93x/4.50x/4.45x. string-heavy stays at 0.82x: its threads call the workload
  once and change tier only through OSR entry; whether they run FTL or
  Baseline for the measured phase depends on whether a function-entry FTL
  replacement happened to be installed when they were spawned (the harness's
  warm-up jettisons and recompiles it five times), and in Baseline the
  per-operation cost at four threads is 4x single-threaded from the shared
  structure and atom tables the workload builds 3 M structures per thread
  through. Pure compute scales perfectly on this machine (402 ms at 1, 4 and 8
  threads), so none of this is hardware.

**Verification on the final tree** (Linux x86-64).

- The corpus (`Tools/threads/run-tests.sh`, default and `--cve`, GIL on and
  off): Release 317 + 334 + 48 + 62 pass, 0 fail (one run flagged
  `map-lock-free-readers`' four-reader timing ratio at 3.0x while three other
  suites ran; 80 of 80 quiet); Debug+ASAN the same four counts, 0 fail;
  TSanJIT the same four counts, 0 fail, 0 reports on the final pass (one new
  suppression from an intermediate pass, TSAN-RESULTS "Seventh round"). The
  corpus was run after every change on that change's tree (twelve
  intermediate trees this round); what those runs and the passes below found
  is listed above.
- Touched-area tests under the amplifier, 500 seeded runs each, both modes:
  the nine tests this round added or rewrote plus `typed-owner-relabel-no-
  stop` and `map-lock-free-readers`; again 500 GIL off for the four T4-P/Map
  tests on the tree with the DFG fix; and on the final tree, both modes,
  `delete-vs-owner-claim-first-transition`, `indexing-transition-keeps-
  adaptive-watchpoint`, `global-property-cache-vs-global-transitions`,
  `ic-condition-stale-at-generation`, `heap-shared-retains-blocks`: no crash,
  timeout or unexpected exit in any run.
- Amplifier campaign, four modes in parallel for 2 h on the Release build
  with core dumps on: default set GIL on 18 passes and GIL off 14, CVE set GIL
  on 56 and GIL off 49, ten seeds per test per pass - about 170,000 amplified
  runs. No crash, no timeout, no unexpected exit code. Divergent-output flags:
  the sixth round's list (the six `scaling/` workloads, `heap-bench-
  allocation.js`, `jit/int-gate-stop-budget.js`, `vmstate/dump-registers-gil-
  on-vm-in-gil-off-process.js`, `scaling/write-barrier-idle-fence.js`, the
  seven interleaving-dependent CVE tests) plus `map-lock-free-readers` (its
  timing assertion under injected sleeps). The campaign before it, on the
  tree with P3, is the one that found the crashes recorded under P3.
- Mirror harness, Release, eval mode, every `JSTests/stress` file on two
  threads: 5,753 files; 3,772 completed, 1,929 threw (the test's own
  assertions under two copies), 34 spun to the deadline, 17 others - the known
  artefact list (seven `$vm`-hook crash tests and `regress-174463162`, three
  type-profiler tests, five `waitAsync`/agent files blocked, one exiting 1)
  plus the one InferredValue-clean-up abort fixed above. The passes on the
  intermediate trees are what found the T4-P register, F33 and that abort.
- JSC suites (`run-jsc-stress-tests`, Release, the same seven collections):
  flag off 582 failures against the sixth round's 583 (the FFI `ftl-eager-no-
  cjit` entries that move every round and `int8-repeat-in-then-out-of-bounds`
  timing configurations); GIL on 852 against 855 (the same movers, plus
  `big-int-strict-spec-to-this.js.default`, whose `numberOfDFGCompiles > 1`
  assertion is load-sensitive GIL on on the sixth-round binary too: 13 of 40
  there, 0-2 of 40 here); GIL off 1,217 against 1,200: `array-slice-cow.js` in
  all sixteen configurations (P5, by design) and one memory-exhaustion abort of
  `symbol-is-destructed-before-refing-underlying-symbol-impl.js.default` while
  three other suites ran (5 of 5 quiet). The GIL-off suite ran on the tree
  before the last two watchpoint changes; those touch a flag-on branch that a
  single-threaded stress test cannot reach differently.
- Bun, the debug-local build against this tree with the nine-change patch,
  the twelve directories. Flag off and GIL on: within one DOMJIT-test timeout
  of the sixth round's counts directory by directory (`bun/jsc` 2-3,
  `bun/util` 6, `web/timers` 4, `web/workers` 2, `node/fs` 1-2, `node/http`
  4, `web/fetch` 5, `bun/http` 2, `node/util` the runner's own SIGSEGV as
  before). GIL off, final tree: `web/fetch` 11,381 pass / 40 fail (sixth round
  11,380 / 41), `bun/http` 2,810 / 6 (2,803 / 13), `node/worker_threads`
  156 / 0 (the sixth round's runner abort there did not recur), `bun/jsc` 20
  (19; WebAssembly and FFI GIL off, unsupported, plus DOMJIT timeouts),
  `bun/util` 13, `node/vm` 1, `web/timers` 6, `web/workers` 6 (8),
  `node/fs` 3, `node/http` 5 - the sixth round's list. The GIL-off pass on
  the intermediate tree without the Full-cycle sweep is the one that measured
  the 3 GB server (`bun/http` 26 failures, every one an RSS ceiling) and whose
  `web/fetch` directory stopped making progress after
  `fetch-retry-chunked.test.ts` until the two-hour limit while three other
  suites loaded the machine; on the final tree, run alone, both directories
  completed with the counts above.

### Results, eighth round (2026-09-09)

Three parts. The tree was first rebased onto the WebKit commit Bun now pins
(368 upstream commits, one upstream merge among them) and re-verified before
anything else changed; then the seventh round's one open TSAN report (F32)
was settled; then the performance list of PERF-RESULTS §5 was worked in its
order, each item written into the specifications first (SPEC-jit history
§37-§42, SPEC-objectmodel rev 19 / history §28-§29, SPEC-heap §10E-§10F /
history §29, SPEC-ungil history "Eighth landing round"), then the C++ and
the JIT tiers, then a JSTests/threads test that fails or shows the old count
on the previous binary and passes now, then the corpus in four modes. Two
items were designed, measured and NOT adopted (SPEC-jit history §41,
SPEC-objectmodel history §29); they are recorded with the measurement that
stopped them. Numbers are Release, Linux x86-64. Net, JetStream
(PERF-RESULTS §3, five runs, four configurations, quiet machine, plus the
rebased tree before the round in the same session): GIL off 0.86 of GIL on
(seventh round 0.83; absolute 262.2, +5.0 % over the rebased tree's 249.8);
GIL on 0.90 of flag off (seventh 0.865, but already 0.895 on the rebased
tree before the round - the base moved, the round did not move GIL on); flag
off 0.96-0.97 of `main` by session (seventh 0.975; the round's changes cost
nothing flag off - the rebased tree before them measures the same). Against
the round's targets: GIL off >= 0.90 of GIL on with no test under 0.75 - not
met (0.86; `Basic`, `ML`, `splay`, `gbemu`, `sha256` under 0.75); GIL on >=
0.89 - met, by the base; flag off >= 0.97 - at the line. Scaling GIL off:
raytrace-like 3.3x at four / 5.7x at eight and map-heavy 3.3x / 5.1x (target
3.2x at four: met), splay-like 3.1x / 5.2x (not met), string-heavy 2.2x at
four in one session and 1.5x in the other (target 2x: bimodal, mechanism
known); the per-test table and the remaining ledger are in PERF-RESULTS §2,
§3 and §6.9.

**Phase 0. Rebase and re-verification.** The 22 branch commits were replayed
onto the new base as one fixup; conflicts were resolved keeping the threads
protocols, and where upstream had rewritten code the branch changes, the
upstream change was ported into the branch's form (the fixup message lists
them: the JSValueRegs removal through the branch's JIT code, the
non-destructible `DateInstance` with SPEC-ungil §N.3 re-applied, the FTL
exit-stub list, `StringRecursionChecker` made per-thread again, the promise
reaction's async context through the GIL-off publish loops, the inline-cache
and array changes of #562/#564). Verification on the rebased tree before new
work: the threads corpus in four modes Release and Debug, 0 failures (the
rebased corpus first caught two resolution mistakes - a VM-wide
string-recursion set that was a data race GIL off and a false cycle GIL on -
fixed in the fixup); the corpus under TSanJIT, 0 reports GIL on, 1 GIL off (an
`AccessCase` publication TSAN cannot key, annotated; TSAN-RESULTS); the
GIL-off JSC stress suite, 1,215 failures against the seventh round's 1,217,
the difference being the known FFI `ftl-eager` movers, timing configurations
of one test, and one new upstream test (E1 below); one Release pass of the
mirror harness, the seventh round's artefact list plus load-dependent
timeouts; Bun built with the seventh-round patch re-applied (one conflict, in
the date-cache reset the embedder now sweeps) and its thread-heavy test
directories in three modes at the seventh round's counts. Two upstream fixes
to the megamorphic store cache (a frozen prototype, an in-place delete on a
dictionary prototype) had landed inside functions the branch splits by the
flag and were missed by the resolution; the GIL-off stress suite could not
see them until P1 gave GIL off a megamorphic cache, and its run then did -
both are ported into the flag-on legs. A third resolution miss surfaced only
in the round's final amplifier campaign: the new base's per-cell "known
atom" bit on `JSString`, whose JIT readers test the bit after loading the
impl and then trust the earlier load; GIL off a concurrent rope resolution
lands between the two (`cve/mc-tear-rope-resolve-race.js`, SIGSEGV in the
megamorphic get-by-value stub, 13 of 24,000 amplified runs; diagnosed from
the core dump: a 16-bit substring rope, fiber word `0x3`, resolved to an
atom by another thread between the two loads). The rebase had ordered the
writers but not the readers; the reader rule (GIL off, re-load the impl
after the bit; the FTL checks the loaded value) is in SPEC-ungil history
"the known-atom bit and rope resolution GIL off", with
`jit/known-atom-bit-vs-rope-resolve-gil-off.js` (8 of 2,400 plain runs
before, 0 of 9,600 after; the cve test 0 of 24,000 amplified after).
JetStream on the rebased tree before the round's changes, measured in the
final session: flag off 341.7, GIL on 305.7, GIL off 249.8 (PERF-RESULTS §3,
last two columns) - against the seventh round's 345.6 / 298.8 / 248.0 on the
old base.

**E1 (found in Phase 0; recorded, not changed). Flag on, a linked callee's
optimized code never ages out.** The new upstream test
`stress/codeblock-aging-ftl-idle.js` (FTL code with no execution counter of
its own is discarded by an idle collection after a quiet period) fails GIL on
and GIL off and passes flag off. A GC-debugging heap snapshot shows the FTL
CodeBlock marked as a root through the branch's call-link record pins
(SPEC-jit §5.8: a published `CallLinkRecord` pins the CodeBlock it names for
the record's lifetime - the fix for a dispatcher transferring a swept
CodeBlock into a callee frame). So flag on, any function still linked from a
live caller keeps its optimized code, and what that code's records name, for
as long as the caller lives; upstream holds callees weakly and unlinks
incoming calls when the callee dies. A retention difference, not a crash; the
idle-aging policy is inert flag on. Open items has what replacing the
publish-time pin with GC-end unlinking needs.

**F32 (the seventh round's open TSAN report): benign, fixed in TSAN's
model.** TSAN-RESULTS "Eighth round" has the proof: the racing address is
always the first payload cell of a block swept as empty, whose one free-list
word the sweeping thread's own allocation hands straight to the `JSRopeString`
constructor that overwrites it before the rope is published; the reader read
the constructor's value, and TSAN paired the plain `String&` read with the
sweep's store only because the rope constructors stored the fiber word
relaxed. The GC verifier (`--verifyGC=1`, re-marking from roots at each of the
test's ~60 collections) is silent across 140 runs including the reporting
ones. The rope constructors now repeat their last fiber store as a release
under TSAN; 0 of 100 after, no suppression; the test also runs under the
verifier in the corpus.

**P1. The megamorphic property cache GIL off: one cache per thread, one
epoch per process (SPEC-jit history §37).** GIL off every megamorphic get,
put and `in` refused the cache (the VM's cache is one mutable table) and ran
the generic path: `megamorphic-access` 1.77x GIL on, `megamorphic-put`
2.7x, and the ledger's largest single line on `typescript`, `Babylon`, `Air`.
Each VMLite now owns a cache; invalidation is a process-wide epoch the
structure-chain events bump and every thread's probe compares; a Full
collection clears every thread's entries world-stopped. The JIT probes load
the cache through the installed lite (flag-off code is byte-identical), and
the `has` probe is now emitted flag-on too. Micro: `megamorphic-access` GIL
off 2,410 -> 1,500 ms (GIL on 1,330), `megamorphic-put-transition` 128 ->
53 ms (GIL on 48). Test: `jit/megamorphic-cache-gil-off.js` (200,000 of
200,000 accesses gave up before, 0 after).

**P2. Dictionary flattening GIL off is a transition (SPEC-jit history
§38, SPEC-objectmodel history §28).** The seventh round withdrew its deferred
flatten because an in-place flatten keeps the StructureID while it renumbers
offsets. This round's form gives the flattened layout a NEW Structure and
publishes it through the ordinary structure-only transition protocol
(claim-first, planned property table, restart on a lost claim), so anything a
second thread derived from the old StructureID fails its check instead of
reading renumbered slots; the inline-cache paths that used to refuse an
unflattened dictionary prototype request the flatten after their lock is
released and retry. Test: `objectmodel/dictionary-flatten-by-transition-gil-off.js`
(100,000 of 100,000 loads through an uncacheable-dictionary prototype gave up
before; 1 after, 1 flatten). The final passes found the cacheable-dictionary
case (an unpinned table; handled by requiring the source still unpinned at
publication), and then the amplifier campaign found a hang: a flatten that
lands between the two halves of an in-place kind-changing `defineProperty`
freezes the old attributes over the new value in the flattened structure,
after which every reader and the defining thread spin. Three windows, three
rules (§38 follow-up): the define re-checks its structure after the in-place
edit; the flatten compares its clone with the source under the cell lock
before publishing; and - the one a plain corpus run under load still hit
after the first two - the in-place attribute edit is made under the cell
lock rather than only the structure's, so it cannot fall between the
flatten's compare and publish. `define-property-kind-change-vs-readers.js`
GIL off with the flatten on, plain runs on a loaded machine: 6 hangs in 720
before the third rule, 0 in 10,800 after (0 in 800 amplified runs). The
flatten is on by default; `--useGILOffDictionaryFlatten=0` is kept as a
switch. Worth, on the final tree (JetStream GIL off, switch off vs on):
+1.9 % overall, `ML` +40 %, `Basic`, `regexp`, `raytrace` +6-7 %.

**P3. Doubles GIL off (SPEC-objectmodel rev 19 / history §28, SPEC-jit
history §42; the larger protocol designed and deferred, history §29).**
Three changes and one decision. (a) A numeric allocation site whose arrays
turn out to hold non-int32 numbers is promoted to recommend Double GIL off
(its arrays are then born Double instead of Int32-then-Contiguous-with-boxed-
doubles; one-shot per site, demoted by T4-P if its arrays get converted;
samples only arrays the sampling thread owns). (b) GIL off a get/put-by-val
site that meets Double arrays among Contiguous ones is compiled generic
(inline-cached per shape) instead of with a converting `Arrayify`: flag-off
that conversion is an in-place rewrite, GIL off it is a stop-the-world per
Double array, and `Basic` had one such site converting up to 840,000
one-element arrays a run (3.7 s of stops in a 5 s run; the source of its
145 / 420 / 520 trimodal scores across runs), the sjcl tests 2,000-4,000.
Test: `objectmodel/double-arrays-at-polymorphic-site-no-stop-gil-off.js`
(50,000 stops for 50,000 Double arrays before, 0 after). (c) `concat`/`slice`
fast copies admit Double + Contiguous pairs GIL off (result Contiguous, Double
lanes boxed) instead of the generic element loop: 185 -> 45 ms on the test's
micro (`objectmodel/concat-double-with-contiguous-gil-off.js` pins the
results of every pairing). (d) The protocol that would let Int32->Double be a
real transition GIL off and make Double->Contiguous a copy instead of a stop -
storage-then-shape re-checks in every lane reader - is written down with its
argument and NOT adopted this round (its reader-side audit is the whole lane-
decoding surface of the runtime and four tiers); history §29 says why and
what it needs.

**P4. The FTL hoists the butterfly across polls GIL off; bounds come from
the same butterfly; the Int32 lane check is one compare (SPEC-jit history
§39).** GIL off the DFG's `CheckTraps` clobbered the butterfly and the named-
property heaps, so every loop with a poll reloaded storage per iteration
(`crypto`'s `am3`: three reloads per multiply-accumulate). In the FTL the
butterfly is now loop-invariant across polls GIL off; in-bounds accesses on
Int32/Double/Contiguous arrays bound by `min(publicLength, vectorLength)` of
the SAME butterfly (a hoisted pointer can only be stale, never short: old
storage is abandoned, not shrunk), out-of-bounds and push/pop paths re-derive;
the I41 Int32 lane validation is one unsigned compare. `crypto` GIL off
1,121 -> 1,356 (0.73 -> 0.89 of GIL on). Test:
`jit/hoisted-butterfly-vector-bound-gil-off.js` (readers race an owner that
pushes, pops and truncates; every read is a tagged integer or a hole, sums
agree across threads).

**P5. Exits reach their compiled ramps without the generation thunk GIL off
(SPEC-jit history §40).** GIL off an exit's jump is never repatched, so every
exit of an already-compiled ramp went through `operationCompileOSRExit` /
`operationCompileFTLOSRExit` - about 25,000 times a `gbemu` run. DFG code now
dispatches exits through the JITData exit vector (as unlinked DFG does), FTL
exit thunks read the published ramp pointer and `ret` to it. Test:
`jit/osr-exit-reaches-ramp-directly-gil-off.js` (303 exit operations for 3
compiled ramps before, 4 for 4 after).

**P6. The shared heap: an allocation-paced Full keeps its blocks, and the
eden allowance grows with the allocating threads (SPEC-heap §10E amendment,
new §10F, history §29).** (a) Only a REQUESTED Full (embedder `gc()`, idle,
memory pressure) sweeps synchronously and frees everything; a Full the heap
upgraded to on its own takes the eden retention rule, whose working size is
now keyed on the start of the cycle that ran. `splay` GIL off minted 69,650
blocks a run before, steady state 0 after; 252 -> 283 (three-run medians at
the time). (b) With k >= 2 clients really allocating, the cycle's allowance
is one nursery plus (k-1) capped nurseries, so four threads doing the same
work each cost the eden count of one (test
`heap-eden-allowance-scales-with-threads.js`: 51 collections for four
threads against 13 for one before, 13 after; GIL on legitimately stays 51).

**P7. Per-thread state that was still shared or refused GIL off, and the
generator claim protocol in the optimizing tiers (SPEC-ungil history).**
`String.prototype.replace`'s regexp cache per lite (it was bypassed GIL off:
`regexp` 0.72 of GIL on before); a per-thread memo for the RegExp
legacy-statics stream lookup; `@claimGeneratorResume` /
`@publishGeneratorResume` as DFG/FTL intrinsics (`Basic`: 300,000 host claim
calls per 300,000 resumes before, 0 after tier-up; test
`vmstate/generator-resume-claim-inline-gil-off.js`, four threads racing forty
shared generators, every value delivered once); typed-array bulk copies from
Int32/Double arrays through one destination snapshot; `forEachProperty`'s
snapshot under the plain structure lock; the deferred-fire temporaries'
destructor skips the process-wide membership lock when nothing was linked; a
Class-A fire that lost the race to another thread's fire of the same set
returns instead of queueing an empty stop.

**P8. GIL off, a Baseline frame at a loop can enter the DFG code an FTL
replacement superseded (SPEC-ungil history, "loop entry into the superseded
DFG code").** The scaling suite's string-heavy stayed at 0.8-0.9x at four
threads through three rounds. Trace: the threads land in Baseline mid-loop
whenever a watchpoint fire jettisons the function's optimized code; the DFG's
loop trigger, remembering the function had an FTL replacement, installs a new
one early; from then on every loop OSR-entry attempt of the frames still in
Baseline fails ("target code block is not DFG", 1,900 a run), is counted as
an exit against the FTL code, and sets a long warm-up - the threads run the
whole measured phase in Baseline on shared profiles (running the suite with
`--useFTLJIT=0` alone took four threads from 7.5 s to 2.5 s). installCode now
keeps the superseded DFG block enterable for such frames and
`operationOptimize` enters it. string-heavy: four threads 7.3-7.6 s -> 2.4-3.2 s
(0.9x -> 2.0-2.4x), eight 13.6 s -> 3.4 s (3.5-3.8x); the scaling gate's
numbers are in PERF-RESULTS §2. Test:
`vmstate/loop-entry-when-replacement-is-ftl-gil-off.js` (the workload as a
corpus test: 4.3 s before, 2.4-2.7 s after, checksums equal, refusals
counted). What is left on that workload is an `Overflow` exit the FTL entry
code keeps taking under the reoptimization back-off (PERF-RESULTS §6.9).

**Defects the round's own passes found in the round's changes** (all fixed
before the final passes; each named where it was found). Debug corpus GIL
off: `fireAllSlow`'s entry assertion is racy flag-on once the megamorphic
store path arms property-replacement sets from two threads; the megamorphic
store's reallocating operation asserted the object still had the entry's
structure before the flag-on code that already handles a racing transition
(once, in the last Debug pass; the assertion now follows that code); later, with P2's
flatten reachable, `tryCacheInBy`'s "condition offset equals slot offset"
assertion met the racing-holder-transition case `tryCacheGetBy` already
treats as retry-later GIL off (a holder flattened by another thread between
the lookup and the condition walk; harmless for an `in` hit, and the
poly-proto forms of both sites, which would have cached the stale offset in
Release, take the same retry now). Debug and TSan: P2 assumed every
dictionary's table is pinned. TSan: P3(a)'s sampling read foreign arrays (now
owned only) and made pre-publication Double lane fills visible to TSAN as
races (modelled: the butterfly word load is an acquire under TSAN;
TSAN-RESULTS). Release corpus GIL off: P6(b)'s doubled allowance exposed the
cycle-end retention shedding blocks from the wrong directories
(`heap-shared-retains-blocks.js` 12,600 blocks; §10E's second amendment).
GIL-off stress suite: the two rebase misses above, and P3(b)'s first form
(below). The four-mode amplifier campaign, GIL off: a hang of
`objectmodel/define-property-kind-change-vs-readers.js` in about 1 run in 20
under random yields, bisected to P2: a flatten cloned a dictionary's table
between the two halves of an in-place kind-changing `defineProperty` (value
under the cell lock, attributes after it) and froze the old attributes over
the new value, after which every reader and the defining thread spun on the
disagreement; three rules close it (P2 above, SPEC-jit history §38
follow-up), the third found after a plain corpus run under load hung once
more with the first two in place. JetStream itself: `Basic`'s trimodal
score, traced to P3(b).

**Measured and left as they are (PERF-RESULTS §6.9 has the evidence).**
`gbemu` (0.68 of GIL on): steady-state FTL throughput is equal in both modes;
the deficit is the first 0.6 s of each run spent in Baseline and DFG - one
hot function's third DFG compile keeps a `to_this` structure check that then
fails on every call, and GIL off it takes 400 exits (the reoptimization
threshold) to replace it where GIL on replaced its equivalent after 31
through the loop trigger; the difference in trigger is understood
(the function's Baseline counter is rewritten by every exit and never
reaches the loop trigger between exits) but not why the two modes' third
compiles differ, and it is left for the next round with the trace. Large
arrays grown by `push` GIL off copy at every growth (a published flat
vectorLength is immutable GIL off; GIL on grows into size-class slack and
reallocates precise allocations in place): `gbemu` copies 1.1 GB a run
against 30 MB, `pdfjs` 1.2 GB against 0.26 - allocation volume, not GC time
(23 ms more a run); the in-place forms need either an x86-only ordering
argument or segmented growth for owned arrays, both recorded as decisions to
take. `splay` (0.67): GC time is equal; the difference is three more Full
collections a run landing in scored iterations (worst-case 206 vs 364).
`OfflineAssembler` (0.75): 1.9 M rope resolutions a run take the cell lock
GIL off. E1 above. The string-heavy scaling workload's tiering (seventh round
note) was re-examined: an attempt to exempt watchpoint-fire windows from the
heap-fact epoch showed no effect and was withdrawn (SPEC-jit history §41).

**Final passes on the final tree** (Debug and TSanJIT built from the same
sources as the Release binary; the tree includes the two campaign findings
below and their rules). The threads corpus in four modes: Release 329 + 346 +
48 + 62, Debug 329 + 346 + 48 + 62, 0 failures; TSanJIT both modes 329 +
346, 0 failures, 0 reports. The JSC stress suite (`JSTests/stress`,
microbenchmarks, mozilla, es6, modules, complex, ChakraCore; every
configuration the runner generates): GIL off 1,214 failures against the
rebased baseline's 1,215 - the only names not in the baseline's list are
five FFI tests of the known `ftl-eager` mover class (seven others of that
class left the list), and the two megamorphic-store tests the baseline
failed (the rebase misses) pass; GIL on 859 against the seventh round's 852:
six FFI movers and E1's `codeblock-aging-ftl-idle`, with the seventh
round's two load artefacts gone; flag off 578 against 579. An intermediate
GIL-off suite run is what found P3(b)'s first form claiming JSArray class
for its generic mode (the DFG then compiled `Array.prototype.push` on it
without storage: five typed-array tests crashed in every configuration);
fixed before the final runs, and the corpus test covers push/pop/indexOf at
such a site. The mirror harness, Release eval GIL off (5,774 stress files,
two threads each): 18 findings against the baseline's 19, the same artefact
classes (type-profiler and IC-generator dumps that abort on a second thread
by design, `$262.agent` worker tests including
`stack-overflow-in-syntax-checker` whose agent script's SyntaxError the
harness no longer ignores because it drops the test's own
`--ignoreUncaughtExceptions`, the `waitasync-*` timing set, and
`try-get-value-without-gc`, which reaches 50 GB in 90 s on the baseline
binary too once the harness drops its `--watchdog`). Amplifier: 500 runs x 2
modes of each of the round's eleven new tests and the ten older ones its
changes touch, on the final binary, 0 failures - except that one GIL-off run
in 500 of `vmstate/loop-entry-when-replacement-is-ftl-gil-off.js`, made
while the campaign, the Bun directories and six other amplifier streams
loaded the machine, counted 658 refused loop entries against the test's
first bound of 600 (unfixed: 1,900; the bound is now 1,000 with the reason
in the test, and 300 more amplified runs per mode after that passed). The
four-mode campaign ran twice. On the tree before the last three fixes (2 h 11 min, 116 passes: 13 + 12 default, 48 + 43 CVE) it
produced, beyond the seventh round's known output-divergence and
scaling-timing set, two findings, both real and both fixed: the P2 hang
(above; P2 now ships disabled) and one SIGSEGV of
`cve/mc-tear-rope-resolve-race.js` GIL off in 430 runs - the third rebase
miss (Phase 0 above; 13 of 24,000 amplified runs before its reader rule, 0
of 24,000 after). On the final tree: 2 h 4 min, 122 passes (14 + 13 default,
50 + 45 CVE), no test outside the seventh round's known divergence and
scaling-timing set, 0 hangs, 0 crashes. Bun, built against the
final tree with the round-8 patch, its twelve thread-heavy test directories
in three modes: at or above the seventh round's pass counts in every
directory with the same failure counts or fewer, with one exception common
to all three modes - `node/fs`'s 300 s abort-signal leak fixture times out on
this Debug build under the campaign's load (it did GIL off in the seventh
round too). Flag off: `bun/jsc` 262/2 against 261/3, `worker_threads` 161/0
against 156/0, `web/workers` 460/1 against 455/2, `fetch` 11,441/4 against
11,416/5; GIL on the same counts; GIL off `worker_threads` 161/0 against
155/1, `web/workers` 456/5 against 451/6, `bun/http` 2,821/11 against
2,790/26, `node/http` 696/5 against 686/5, and `web/fetch`, which timed out
as a directory in the seventh round, completes: 11,405 pass, 40 fail. (The
first GIL-off pass of `node/http` on the final build was cut short by a Bun
debug assertion - a `Strong` slot released off its VM's thread from a
sweep-time finalizer; Open items - and the counts are from its rerun.)
JetStream and the scaling gate on the final tree are in PERF-RESULTS §1-§3.

### Results, ninth round (2026-09-10)

The round's goal: a program behaves, stays safe and performs as on `main` in
every configuration - flag off, GIL on (`--useJSThreads=1`), GIL off
(`--useJSThreads=1` with the shared heap and the GIL off) - each gap closed or
left with a measured, written reason. The tree was rebased onto the WebKit
commit Bun pins (`dfd696443b9b`) and re-verified first; the upstream code
since the rebase was audited (AUDIT-upstream-since-rebase §8, rows R9-1 to
R9-20); the eighth round's open behaviour item (E1) and the destructor-thread
item were closed; then the performance gaps were worked by per-symbol
instruction counts in each configuration. Every change was written into the
specifications first, with a JSTests/threads test that fails or shows the old
count before and passes after, and the corpus in four modes. Numbers are
Release, Linux x86-64. On a quiet machine, the final candidate (r9za, medians
of five): JetStream main 353.5, flag off 339.8 (0.961 of main), GIL on 308.4
(0.908 of flag off), GIL off 257.0 (0.833 of GIL on); GIL off at four threads
splay-like 3.57x, string-heavy 2.52x (PERF-RESULTS §6.11).

**Phase 0.1. Rebase onto `dfd696443b9b`.** Four upstream commits past the
eighth round's base, one of them large (#588, pre-resolved module loading,
thin child executables, lazy FunctionExecutables, lazy catch liveness, lazy
SymbolTable constants, lazy RegExp construction, a startup JIT deferral
scale). The 23 branch commits were replayed; two static size assertions were
restored by repacking bit-fields (`UnlinkedFunctionExecutable` 104 -> 96,
`CodeBlock` 232 -> 224, checked with clang's record layout). The rebased
tree's own passes found three regressions the new base brought in, fixed
before anything else: R9-2 (heap snapshots walked the heap outside a stop;
`w16-c1-prevent-collection.js` GIL off 15 of 20 crashes -> 0 of 20), R9-7
(lazy FunctionExecutables raced GIL off: Debug 10 of 10 asserts -> 0,
TSAN 11 reports -> 0), R9-17 (the global inlining planner validated a callee
another thread relinked: 6 of 10 Debug failures -> 0).

**Phase 0.2. Behaviour ledger.** JSC suites on the rebased tree against
`main` (579 failures): flag off 576, none that pass on main; GIL on 280 that
pass on main, all classified - 234 fail-stops (`useJSThreads requires
useConcurrentJIT`), 34 `Atomics` on objects (SPEC-api §4.5), the rest
concurrent-compilation timing that fails on main with `--useConcurrentJIT=1`
too, plus the aging test E1 fixed; GIL off 637, in the classes of earlier
rounds (the sampling profiler refused, Wasm off, FFI off, and the tests listed
in PERF-RESULTS §3). Bun, twelve directories, three modes, against the stock
build: flag off, no behaviour differs (the DOMJIT timeouts pass with a 120 s
budget on both). `fetch-tcp-stress` times out on the branch where stock
passes, which this ledger first read as load; measured side by side at the
end of the round it is throughput (Open items: every case passes with a
longer budget, and the branch builds take 15-22 % longer);
GIL on likewise; GIL off: Wasm and FFI tests (out of scope), the sampling
profiler's refusal, heap-wide counters and RSS deltas that a second JS thread
moves (the keep-alive preload), collection-timed timer tests, and one abort -
AUDIT R9-20, below. On the final tree (Final battery, below): flag off 4 that pass on main (the FFI executable-memory runs), GIL on 277 and GIL off 634, in the classes above; Bun flag off and GIL on differ from stock only in the four `fetch-tcp-stress` cases, and GIL off from the round's start only in two heap-count checks that the second JS thread's two-Full retention moves (I12).

**Phase 0.3. Audit of upstream code since the rebase** (AUDIT §8). Twenty
rows; the new base's lazily materialized bytecode-cache and link-time state
accounts for most (R9-1 to R9-10): decoded eagerly, published by
compare-and-swap, or bypassed GIL off, each with its evidence (R9-1: a
two-process driver, 40 of 40 crashes GIL off -> 0 of 40; cost of eager
decoding +6.1 % instructions on a populated cache). R9-11 (the pending-reaction
walk: copied under the cell lock; TSAN 5 of 5 runs -> 0), R9-12 (catch buffers:
compare-and-swap; the flag race TSAN found: relaxed atomics), R9-15 (module
namespace publication: compare-and-swap; 10 of 10 failures -> 0), R9-17 (above),
and three found in branch code by this round's own tests: R9-18, R9-19, R9-20.

**Phase 1.** E1: a call-link record no longer pins the CodeBlock it names;
the End phase that finds the block dead clears the record, as upstream clears
its call links (SPEC-jit history §43). The new base's
`stress/codeblock-aging-ftl-idle.js` passes in every configuration; its flag-on
copy failed 6 of 6 before, 6 of 6 pass after. The GIL-off activation
checklist runs once, after option parsing (`api/options-environment-order-gil-off.js`:
3 of 3 failed, 4 of 4 pass).

**Phase 2.1. Destructors run on whichever thread sweeps.** SPEC-heap §10G
states the contract; Bun marshals its generated classes' finalizers and
`Strong` releases to the VM's thread when a JS-spawned thread sweeps them
(Bun patch; its debug build's "Strong dropped off the JS thread" abort: 5 of
5 before, 0 of 15 after; `test/js/bun/net/socket.test.ts` 3 of 3 fail before,
pass after). The JSC test for the contract,
`gc-stress/destructors-run-once-on-any-thread.js`, found **R9-18**: GIL off,
weak-bearing blocks were never swept (12,350 of 50,000 destructible cells
never destroyed); the conductor now sweeps them at every cycle's end inside
the stop (SPEC-heap §10E third amendment): 0 never destroyed, at most 10
blocks swept in a cycle on the scaling workloads. Amended at the end of the
round (history §35): a Full collection makes every block unswept again, so
back-to-back Full cycles re-swept every weak-bearing block each time - the
JSC suite's continuous-collection lanes GIL off, about 800 blocks at each of
about 20,000 cycle ends a minute, where a progress-instrumented run did 50 of
1,000 iterations in 20 s against 550 on the round's first build. One
unrequested cycle end now sweeps at most 32 of them, resuming where the last
stopped, and a requested collection all of them
(`gc-stress/weak-bearing-sweep-bounded-per-stop-gil-off.js`: at most 495
blocks in one cycle end with the old rule, 32 with the budget); the
progress-instrumented run finishes all 1,000 iterations in 21.2 s.

**Phase 2.2. Audit fixes found in branch code.** **R9-19**: the fast
data-property copies (`{...o}`, rest destructuring, `Object.assign`,
`cloneObject`) stored a value read from a source another thread was deleting
from - an empty value in a live object (Debug assert, Release crash). They
re-check the structure and the values after the reads and copy property by
property otherwise (SPEC-objectmodel history §30, I42):
`shared-objects/data-property-copies-vs-delete-gil-off.js` Release 4 of 20
crashes, Debug 1 of 10 asserts -> 0 of 20, 0 of 10. **R9-21**: the
window-liveness constraint's precise-allocation leg took "newly allocated"
as its witness, which a Full collection's `flip()` sets for everything marked
the cycle before, so with two clients attached every precise allocation
(large `Map` storages, large butterflies) that was ever marked stayed alive
(SPEC-heap history §34): map-heavy GIL off peaked at 1.1 GB per allocating
thread. It has its own witness bit now:
`gc-stress/precise-allocations-reclaimed-with-two-clients-gil-off.js` 3 of 3
fail before (150 MB kept over four rounds of garbage), 5 of 5 pass after;
map-heavy at one thread 1,140 -> 352 MB and 2,884 -> 1,448 ms (flag off 1,441
ms). **R9-22**, found by the end-of-round amplifier campaign and older than
the round: generated code (the interpreter, the Baseline IC, DFG, FTL) raised
a shared array's length with a plain store on hole stores, push and unshift,
and so did the runtime's `JSArray::pushInline` on the owner's leg, so GIL off
a thread holding a stale length lowered a racing thread's CAS-max raise and
hid its element (the owner is no exception: once the set has fired a foreign
writer flips SW lock-free). GIL off without the E2 elision every raise is now
the CAS-max, inline in every tier and in the runtime; lowers (pop, shift, the
length setter) stay plain, as the runtime's always were (SPEC-jit §5.5 length
updates, history §46). A first version routed every such leg to the runtime
instead: 3.2-3.9x the instructions of a hole/push/pop loop once the sets had
fired, 3.6x in the lower tiers; the inline CAS-max costs 1.06-1.09x there and,
in the lower tiers, 1.11x (hole) and 1.02x (push); nothing measurable unfired,
GIL on or flag off (PERF-RESULTS §6.10). `jit/length-update-races-gil-off.js`
(disjoint hole stores; the owner pushing under foreign hole stores): 5 of 5
runs fail before, 5 of 5 pass after; a store warmed up in bounds and then hit
by concurrent hole stores loses the length in 947 of 1,500 rounds before, 0
after; `objectmodel/i03-n3-first-install-races.js` amplified 11 of 8,000 -> 0.
An audit of the runtime's plain raises found only `pushInline`. **R9-23**,
found by the final battery's Bun run GIL off (`test/js/web/fetch` with the
keep-alive thread): the shared GC's conductor runs destructors and weak
finalizers inside its own stop window with its heap access released, and a
destructor that takes the VM's API lock - Bun's `Bun__JSValue__unprotect` does
- re-ran the gated access acquire, saw the conductor's own stop pending, and
waited for a clear only the conductor performs; every other mutator parked
behind it. The hazard is as old as the in-stop sweeps, but this round's fixes
made it live: in the Bun hang the destructor ran from the End phase's eager
sweep of lower-tier precise allocations, which before R9-21 never found a dead
precise cell while two clients were attached; R9-18's cycle-end sweep is the
other in-stop route. The conductor now re-enters its own access inside its
window, past both the stop-pending leg and the Mode-machine leg (a first
version exempted only the former, and the JSC reproduction parked the
conductor in the latter), and §10G says a destructor may take the API lock
(SPEC-heap F8 conductor re-entry, history §36):
`gc-stress/destructor-takes-api-lock-inside-stop-gil-off.js` hangs 3 of 3 runs
before (120 s limit), passes 5 of 5 after in under a second; GIL on passes on
both. Bun, `test/js/web/fetch` GIL off: 4 of 4 runs hang on r9y and r9u, 2 of
2 complete on r9za with the round start's counts. **R9-20 (open)**: the
VMManager counters under the shared collector's stop (Open items: not
reproduced in 28 later runs; diagnostics shipped).

**Phase 3.1. Flag off against `main`, by instruction count.** Deterministic
collector, medians of 3, then per-symbol samples below the kernel's throttle
(5M instructions per sample: the first recordings at 1M were throttled, their
totals disagreed with perf stat). The branch's flag-on arms had been inlined
into flag-off hot functions and had pushed small upstream helpers over the
inliner's threshold; ALWAYS_INLINE on those helpers, cold GIL-off arms out of
line, gate order, plain loads outside TSAN, and `DeferTermination` resolving
its traps once. Rows above 1.03 at the start -> now: class-ctor-4 1.118 ->
1.083, array-int32-to-double 1.170 -> 1.121, regexp-exec 1.128 -> 1.075,
map-set-get 1.097 -> 1.073, megamorphic-put-transition 1.130 -> 1.047,
throw-catch 1.061 -> 1.045, json-stringify 1.049 -> 1.040, array-push-pop
1.048, astar-like-nodes 1.036 -> 1.027, startup 1.038 -> 1.026, json-parse
1.036 -> 1.015. What remains is measured, not guessed: no extra calls and no
new allocations in any row, only one-byte mode tests in C++ fast paths (Open
items). Tried and reverted with their numbers: five out-of-line moves that
gained nothing or traded one configuration against another (PERF-RESULTS).
Time, quiet machine (r9za): JetStream flag off 0.961 of main (the round's
start 0.969; target 0.99, not met); micro rows flag off against main by time
within 1.03 except array-int32-to-double 1.08, out-of-line-replace-poly 1.07,
astar-like-nodes 1.06, megamorphic-put-transition 1.05, json-parse,
json-stringify and throw-catch 1.04 (PERF-RESULTS §6.11).

**Phase 3.2. GIL on against flag off.** FTL handler ICs at sites that can
throw into a catch of their own frame were refused whenever a live value was
on the stack (the check counted a stack slot as the frame and stack pointers);
the check ignores those two now (SPEC-jit history §44): astar-like-nodes GIL
on 0.613x its instructions, 700,000-1,000,000 give-ups a run -> 0. The inline
caches' and the megamorphic probe's owner transitions do not claim the
StructureID lane GIL on (OM E4-G, history §33; the audit before building found
every leg sound, with a store fence the allocating leg needs on non-x86):
class-ctor-4 GIL on 0.61x cycles, megamorphic-put-transition 0.86x,
astar-like-nodes 0.86x; the new
`objectmodel/gil-on-unclaimed-transitions-across-handoffs.js` passes 5 of 5
and 500 of 500 amplified runs in both GIL modes. A fresh RegExp matches array
is written through its flat storage (OM history §31): regexp-exec GIL on
0.96x, GIL off 0.965x. Time, quiet machine (r9za): JetStream GIL on 0.908 of
flag off (the round's start 0.905; target 0.95, not met); micro GIL on against
flag off, astar-like-nodes 3.51 -> 2.16, class-ctor-4 2.35 -> 1.47,
megamorphic-put-transition 1.74 -> 1.40, regexp-exec 1.23 -> 1.12; above 1.3x
still: array-int32-to-double 2.72, astar-like-nodes 2.16, class-ctor-4 1.47,
megamorphic-put-transition 1.40, and the polling-trap rows
(flat-butterfly-read 1.98, inline-property-read 1.97, proto-method-calls 1.76,
int-loop 1.67), which are 1.00 against `main` with polling traps.

**Phase 3.4. Scaling.** splay-like at four threads: the design note (decided
with the user: implement the candidates and keep the best by measurement,
other workloads included) found the four-thread loss was not the collector
(2.9 % of wall in stops) but a word every thread rewrote - the GIL-off
array-allocation report, 8.2 % of all cycles on two instructions. The report
is written now only when the word is empty, after the slow path, or for one
allocation in 32 per thread (OM history §32). GIL off, speedup at four
threads: splay-like 3.11 -> 3.62, raytrace-like 3.23 -> 3.37, map-heavy 3.34
-> 3.38, string-heavy 2.41 -> 2.43 (bimodal). The eden-ratio change (A) and
more markers (B) were built or measured and not adopted (SPEC-heap history
§33). map-heavy memory GIL off, after R9-21: 352 MB at one thread, then about
1 GB for each thread beyond (1,330 / 3,450 / 7,530 MB at 2 / 4 / 8 threads;
flag off 176 MB), all of it freed by one Full collection after join: the
window-liveness constraint's conservative retention of precise allocations and
an allowance feedback loop (Open items). Quiet machine (r9za, five runs a
cell), GIL off at four threads: splay-like 3.57x, string-heavy 2.52x,
raytrace-like 3.40x, map-heavy 2.94x (both of its times faster than at the
round's start: one thread gained more), richards-like 1.20x (the round's start
1.08x).

**Harness and tests.** The amplifier took a failing reference run as the
baseline, so an always-failing test passed under it; a failing reference is
now a finding. The runner no longer probes with `--collectContinuously`,
gives tests that use it three times the timeout, and runs tests marked
`//@ threadsNoAmplify` (timing-ratio checks) plain under `--amplify`. Test
races fixed, each rerun to 30-500 runs clean: the OSR-exit, arraymode and
foreign-reify tests (DFG code arriving after the warm-up under forced
concurrent compilation), `mc-lock-cow-materialize-race` (a final wake-up
counted as a round), `condition-notify-all` (a worker taking the lock before
the main thread parked), `mc-jit-ta-resize-hoisted-base` (a reader never
scheduled before the storm ended), the flag-on aging copy (upstream's own
racy check dropped), `int-gate-direct-call-relink` (workers that had not
started), and the timing tests' measurement (best of five, three attempts).

**Final battery.** The candidate is r9za: r9s, plus R9-21 (r9t), the per-stop
budget on R9-18's sweep (r9u), R9-22's length-update rule in its final form
(r9y; r9w and r9x were superseded forms, their batteries stopped) and R9-23's
conductor re-entry (r9za; r9z exempted one leg of two). R9-22 reaches the
interpreter, the runtime's push and every JIT tier, so the battery was taken
again on r9y; R9-23 reaches only the GIL-off shared collector's conductor (its
flag is set only by the shared window's open and both exempted legs are
gilOff-gated), so r9y's flag-off and GIL-on results stand for r9za and the
GIL-off lanes were taken again on r9za. On r9za: the corpus, Release and
Debug, four modes, 0 failures; TSanJIT 0 reports, GIL on 341 pass, GIL off 362
pass; 500 amplified runs in both GIL modes of the touched tests (the
length-update test, i03-n3, the new destructor test and the three late heap
tests): clean; the GIL-off JSC suite 634 that pass on `main` (the round's
classes; against r9y 1 new and 5 fixed, all in the FFI executable-memory and
`int8-repeat` compile-timing classes), every configuration finished; mirror,
5,801 files, 18 findings and 35 spun to the deadline - the rc=134 abort list,
seven waitAsync/SAB-worker tests blocked at the deadline, and one rc=133 of
`call-apply-exponential-bytecode-size.js`, which exhausts JIT memory by design
and under the mirror's two threads ends differently on every build (rerun
three times each: r9s 1 rc=133 and 2 passes, r9y and r9za passes and
stack-overflow exceptions; standalone GIL off it passes in a second). Bun GIL
off, twelve directories on r9za's Bun: `test/js/web/fetch` runs through
(11,605 pass, 37 fail, where r9y hung); against r9y 0 new failures, against
the round's start 2 - the two `serve.test.ts` HEAD-stream leak checks, which
count live `ReadableStream`s after one `Bun.gc(true)`. With the keep-alive
thread attached the eight streams survive one Full collection and go at the
second (10, then 2, then 2; flag off, GIL on and GIL off without the second
thread 2/2/2): I12's bound, a dead cohort rides at most two Full collections
while a second client is attached - the known class of heap counters a second
JS thread moves; on the round's first build the sync variant's streams are
never freed while the second thread is attached (10/10/10: the
precise-allocation retention R9-21 fixed) and the async variant's pass there
was incidental; r9za frees both at the second Full (Open items, GIL-off
memory). Amplifier campaign on r9za, four modes in parallel for 2 h
(16:29-18:30): default set GIL on 13 passes and GIL off 11, CVE set GIL on 52
and GIL off 47, ten seeds per test per pass; no crash, no hang, no exit
divergence, no failing reference; the divergent-output flags are the known
list (the six `scaling/` workloads, `jit/int-gate-stop-budget.js`,
`dump-registers-gil-on-vm-in-gil-off-process`, six interleaving-dependent CVE
tests). The quiet performance pass on r9za (PERF-RESULTS §6.11): JetStream as
above; against the round's start flag off and GIL on hold within noise, GIL
off fell on five rows whose five-run ranges do not overlap - earley-boyer
0.65x, json-stringify-inspector 0.86x, delta-blue 0.90x, Air 0.91x,
json-parse-inspector 0.95x - and rose on four (ML 1.50x, Basic 1.14x, regexp
1.13x, hash-map 1.06x) Measured again interleaved in one session on the quiet
machine - every build of the round from r9d to r9za, five full runs each - the
five rows are flat (earley-boyer is bimodal and came out higher on r9za, 608
-> 692; json-stringify-inspector 399 -> 393, delta-blue 816 -> 794, Air 416 ->
418, json-parse-inspector 321 -> 327) and GIL off's total moves from 260.5 to
257.9 within overlapping ranges: the falls were a comparison across days (the
round-start reference is from 09-09). The one step the interleaved runs show
is at r9h, R9-18's in-stop sweep (260.5 -> 255.6), which r9u's per-stop budget
brought back to 258.0.

On r9y, whose flag-off and GIL-on results stand for r9za: the corpus, Release,
four modes, 0 failures; Debug, GIL on 0 failures, GIL off two timeouts at 120
s during the battery's load peak (load 181) -
`arrays/segmented-out-of-bounds-read-llint.js` (three threads spin-waiting on
each other) and `cve/mc-val-tid-reissue-false-owner.js` (about 13,000 thread
spawns) - which alone pass on r9y in the time r9u takes (8.0-8.4 s against
7.9-8.6 s, 19.3-22.6 s against 20.4-22.8 s); TSanJIT 0 reports, GIL on 340
pass, GIL off 361 pass, 0 failures (r9u's loop-entry threshold failure did not
recur). 500 amplified runs in both GIL modes of the touched tests (the
length-update test, i03-n3, the i03-t5 racing growers and the three late heap
tests): clean but for three hangs of the length-update test in each lane, all
in one three-minute window both lanes share (10:18-10:21, the load peak; two
of each did not print even the amplifier's banner) - the workspace's memory
limit, not the tests: its cgroup recorded 12 OOM kills during the battery, and
the mirror pass runs the stress suite's memory-hog tests without the watchdog
their headers ask for (`try-get-value-without-gc.js` reaches 16 GB in 21 s on
r9s, 42 s on r9u, 29 s on r9y); rerun afterwards, r9y 500 of 500 in both modes
and r9u 500 of 500 GIL on, no hang. JSC suites against `main` (579 failures on
main): flag off 4 that pass on main, all FFI `ftl-eager-no-cjit`
executable-memory runs (against r9s 3 new and 1 fixed, the same class); GIL on
277 (r9s 281), the round's classes, against r9s 0 new and 4 fixed; GIL off 638
(r9u 635), the round's classes, against r9u 6 new and 3 fixed, all in the two
flaky classes (FFI executable memory, `int8-repeat` compile timing); every
configuration finished. Mirror, Release eval mode: 5,801 files, 19 findings
and 37 spun to the deadline (r9s 14 and 36). The difference is the artefact
classes moving between runs: rerun side by side, r9y and r9s give the changed
files the same statuses (the waitAsync and SAB-worker tests blocked or
spinning at the deadline on both,
`growable-sharedarraybuffer-parallel-grow-during-prototype-methods.js` passing
on both in 5-6 s, `try-get-value-without-gc.js` killed by the memory limit on
both). Bun, twelve directories, the debug build from this tree: flag off,
against stock, the only new failures are the four `fetch-tcp-stress` cases
(throughput, Open items) - every other directory has stock's counts
(`node/util` crashes before its first test on stock too); against the round's
start 0 new, 1 fixed; GIL on, against stock the same four and nothing else,
against the round's start and r9u 0 new. Amplifier campaign, four modes in
parallel for 2 h on the Release build (11:25-13:33): default set GIL on 14
passes and GIL off 12, CVE set GIL on 52 and GIL off 47, ten seeds per test
per pass. No crash, no hang, no exit divergence, no failing reference; the
divergent-output flags are the known list (the six `scaling/` workloads,
`jit/int-gate-stop-budget.js`, `dump-registers-gil-on-vm-in-gil-off-process`,
six interleaving-dependent CVE tests GIL off and two of them GIL on). Bun GIL
off on r9y: `test/js/web/fetch` hung - R9-23, above - and Bun GIL off and the
quiet performance pass were taken on r9za.

The r9u battery, kept for the record: JSC suites against `main` (576 / 856
failures on main in the two comparisons): flag off 2 that pass on main, both
FFI `ftl-eager-no-cjit` executable-memory runs, which fail once in three on
`main` alone too; GIL on 281, the round's classes (234 fail-stops, 34
`Atomics` on objects, the compile-timing tests, `class-subclassing-function`)
plus four such FFI runs. Mirror, Release eval mode (GIL on): 5,801 files, 14
stopped otherwise and 36 spun to the deadline, round 8's artefact classes,
fewer than round 8's. GIL off, the r9t suite: the round's classes plus 48
continuous-collection configurations it could not finish (the R9-18 re-sweep;
r9u bounds it). On r9u: the corpus, Release, four modes, 0 failures; Debug,
four modes, 0 failures, the two continuous-collection objectmodel tests
included (the earlier timeouts, first read as load, were the same re-sweep);
TSanJIT, 0 reports, GIL on 339 pass, GIL off 359 pass and one failure,
`vmstate/loop-entry-when-replacement-is-ftl-gil-off.js` (a JIT counter over
its threshold under TSAN at load 60-130: 1,601 refusals against a limit of
1,000; alone, 10 of 10 pass on TSanJIT r9u and on r9t; under the amplifier,
100 runs each side by side, r9u 0 and r9t 1 over the limit). Every
configuration the r9t suite could not finish, rerun on r9a and r9u side by
side: 204 each, r9u in 145 s and r9a in 223 s, the same 17 failures (the
sampling profiler's refused deep-stack test). 500 amplified runs in both GIL
modes of the three late heap tests (weak-bearing budget, destructor contract,
precise-allocation reclamation): clean. The GIL-off JSC suite on r9u: 635 that
pass on `main` (637 at the round's start), in the round's classes (234
fail-stops, the sampling profiler refused, Wasm off, FFI); against the round's
first build 5 new, all in two flaky classes (four FFI `ftl-eager-no-cjit`
executable-memory runs, one `int8-repeat` compile-timing run), 7 fixed (E1's
aging test among them), and no configuration left unfinished. Bun, twelve
directories: flag off, against stock, no behaviour differs - the only new
failures are the four `fetch-tcp-stress` cases, which cross their 30 s budget
(throughput, Open items); GIL on, against the round's start 0 new failures,
against stock the same four. Amplifier campaign, four modes in parallel for 2
h on the Release build (06:48-08:57): default set GIL on 14 passes and GIL off
11, CVE set GIL on 53 and GIL off 47, ten seeds per test per pass. No crash,
no timeout, no failing reference run. Divergent-output flags: round 8's list
(the six `scaling/` workloads, `heap-bench-allocation.js` - now run plain,
`jit/int-gate-stop-budget.js`, `dump-registers-gil-on-vm-in-gil-off-process`,
the seven interleaving-dependent CVE tests). Two exit divergences: the
loop-entry threshold (above) and one GIL-off run of
`objectmodel/i03-n3-first-install-races.js`, a lost racing indexed store older
than the round (R9-22, above). (r9u's Bun GIL-off run and the quiet
performance pass were superseded by r9y's and r9za's.)

**Parity, configuration by configuration** (final candidate r9za; flag off and GIL on from r9y, which R9-23 does not
reach). Targets from the round's brief; "classes" means the classified lists above.

| | flag off | GIL on | GIL off |
|---|---|---|---|
| Behaviour: JSC suites vs `main` | 4 fail that pass on main, all FFI executable-memory runs that fail on main alone too | 277, the round's classes (234 fail-stops, 34 `Atomics` on objects, compile timing) | 634, the round's classes (fail-stops, sampling profiler refused, Wasm off, FFI) |
| Behaviour: Bun, 12 dirs vs stock | only the four `fetch-tcp-stress` cases (throughput, Open items) | the same four | the round start's GIL-off classes plus the two HEAD-stream counts (I12 two-Full retention with a second thread; Open items) |
| Safety: corpus (Release, Debug) | 0 failures (flag-off lanes) | 0 failures | 0 failures (two Debug load timeouts on r9y, classified; r9za clean) |
| Safety: TSanJIT | - | 0 reports | 0 reports |
| Safety: amplifier, 2 h + 500-run | clean | clean (known divergent-output list) | clean (known list; r9za: 2 h campaign and 500-run, no crash, hang or exit divergence) |
| Performance: JetStream | 0.961 of main (target 0.99; start 0.969) | 0.908 of flag off (target 0.95; start 0.905) | 0.833 of GIL on (target 0.90; the round's start 0.853 in the same session), 10 rows below 0.80 |
| Performance: micro rows | time <= 1.03 except seven rows 1.04-1.08 (Phase 3.1) | above 1.3x: array-int32-to-double 2.72, astar 2.16, class-ctor-4 1.47, megamorphic-put-transition 1.40, polling-trap rows | below 0.80 of GIL on: astar, class-ctor-4, megamorphic-put-transition, transitions-after-fire, array-element-read, json-stringify (GIL off's own times unchanged; GIL on got faster) |
| Scaling (4 threads) | - | - | splay-like 3.57x (target 3.2x), string-heavy 2.52x (target 2x; bimodal), raytrace-like 3.40x, map-heavy 2.94x, richards-like 1.20x |

### Results, tenth round (2026-09-14/15)

The round's goal: raise JetStream in the two flag-on configurations by a lot, close the flag-off gap, lose none of
the ninth round's behaviour or safety, and leave every remaining failure either fixed or named as an intentional,
documented compromise. Targets: flag off >= 0.99 of `main`, GIL on >= 0.95 of flag off, GIL off >= 0.90 of GIL on, no
JetStream test below 0.80. The tree was rebased onto the WebKit commit Bun pins (`cf1b36ec8703`) and re-verified
first; the upstream range was audited (AUDIT-upstream-since-rebase §9, rows R10-1 to R10-23); then the gaps were
profiled per configuration (per-symbol instruction samples over the 36 tests, and - new this round - samples inside FTL
code attributed to DFG nodes through the code's address ranges), ranked, and worked in that order. Every change went
into the specifications first, then C++, then every tier, with a JSTests/threads test that fails or shows the old
count before and passes after. Numbers are Release, Linux x86-64. On a quiet machine, final candidate against the
round's first build, interleaved in one session (medians of five): `main` 347.3, flag off 345.4 (the round's first build 344.0), GIL on 330.9 (312.1), GIL off 265.9 (259.4); flag off / main 0.97-0.98 once `main`'s bimodal earley-boyer is set aside (0.994 with it, in a pass where `main` ran it slow; 0.970 in the pass an hour earlier), GIL on / flag off 0.958 (0.907 at the start), GIL off / GIL on 0.804 (0.831: GIL on gained 6 %, GIL off 2.5 %).

**Phase 0. Rebase and ledger.** `dfd696443b9b..cf1b36ec8703` is one upstream merge (69 commits under
JavaScriptCore, WTF, bmalloc) and the fork's additional module loaders. Twenty-five branch commits were replayed; the
hand resolutions are listed in AUDIT §9. The rebased tree (the round's first build): corpus Release and Debug, four
modes, 0 failures; TSanJIT 0 reports; JSC suites against `main` (578 failures): flag off 2 that pass on main (FFI
executable-memory runs), GIL on 279, GIL off 637, in the ninth round's classes. One class was not ours:
`class-subclassing-function.js` fails on `main` too once it runs long enough (a fork-side code-cache key collision,
reported separately); the eager configurations clamp its loop on `main`, and GIL on they did not because GIL on
forced the concurrent JIT (fixed below).

**What was chosen, and why.** The first instruction sweep (36 tests, geometric means, instructions / cycles): flag
off over main 1.027 / 1.025; main with polling traps over main 1.055 / 1.005; GIL on over flag off 1.114 / 1.084; GIL off over
GIL on 1.169 / 1.143. So: (1) GIL on's distance was in generated code and the C++ under it - per-thread butterfly
tags, their predicates in every tier, the flag-on object-model paths, polls - none of which a process with a GIL
needs; that became G1 and its followers, the round's largest item. (2) GIL off's was generated code too (polls that
made every heap read unhoistable, tag predicates), plus collection latency on the main thread and a retention rule
that made Eden collections reclaim nothing young; the first was narrowed (poll visibility), the third removed (Wlr
off), the second measured and left (C2a, below). (3) Flag off's was spread thin over C++ (RegExp and string paths,
the marking loop's counters, compiler-phase helpers); the concentrated pieces were fixed and the rest is recorded.

**GIL on.**
- *One owner with the GIL* (SPEC-objectmodel G1, history §39; SPEC-jit §5.5 "Untagged words", history §52). In a
  GIL-on process every thread's butterfly TID is 0: no word carries a tag, nothing is foreign, the shared-write bit
  and segmented butterflies are unreachable. A derived option (`useTaggedButterflies`: GIL off, or GIL on with
  `--useJSThreadsSingleOwnerWithGIL=0`) keys every emitter and every object-model entry point: untagged, the four
  JIT tiers emit `main`'s butterfly accesses, DFG/FTL admit transitions, deletes and private brands on `main`'s
  terms (no thread-local-set watch, no CheckTransitionOwner), the inline caches and their shared handlers are
  `main`'s, clobberize models the butterfly nodes as `main` does, the LLInt runs `main`'s fast paths on `main`'s
  metadata caches (prototype-load and unset get_by_id modes, the put_by_id transition cache), and the C++ object
  model runs `main`'s bodies. What keeps it sound is the rule every earlier GIL-on shortcut rests on: the GIL changes
  hands only inside blocking calls, and nothing holds a storage pointer, a shape check or an open fast-path window
  across a call. Tests: `objectmodel/gil-on-single-owner-across-handoffs.js` (three threads take turns, separated by
  every handoff primitive, growing, reshaping, shifting, slicing, deleting from and freezing the same objects and
  arrays in every tier), the three tests that observe per-thread ownership now ask for it explicitly. Measured on the
  way: WSL's FTL code GIL on had 652 PutById nodes and no PutStructure where flag off has 36 and 311 (after: 35 and
  580); instructions GIL on over flag off WSL 1.16 -> 1.07, Air 1.12 -> 1.06, typescript 1.11 -> 0.98, delta-blue
  1.04 -> 1.01, stanford-crypto-aes 1.02 -> 1.00.
- *Thread.restrict against untagged words* (SPEC-api §5.7, history r10.1). G1 exposed two of `main`'s fast paths
  that store an existing element of a SlowPutArrayStorage array without passing the hooked entry points; per-thread
  tags had covered them by accident. Closed whenever the flag is on. `api/thread-restrict.js` failed on the first
  single-owner build, `api/thread-restrict-hot-indexed-store.js` is the JIT half.
- *Traps are delivered as flag off* (SPEC-jit I21, history §53). Polling traps were forced with the flag on because
  asynchronous breakpoint patching is unsafe with more than one running mutator; with the GIL there is one, and the
  signal sender already re-verifies the API lock's owner under suspension. GIL-on code has no polls now. `main` with
  polling against `main`: delta-blue -9 % score, richards -5 %, ai-astar -6 %. ThreadSanitizer builds keep polling
  (the sanitizer defers asynchronous signals past any call-free loop). Tests: `api/gil-on-trap-delivery-without-polls.js`,
  `api/trap-delivery-in-call-free-loop.js`.
- *Earlier in the round*: transitions that constant folding proves are inlined flag on (SPEC-jit history §48: a
  constructor microbenchmark GIL on 3.26 G -> 1.74 G instructions, flag off 1.45 G); the mutator's property-table walk
  GIL on is the flag-off walk (L6-G, SPEC-objectmodel history §36); the C++ publication helpers store instead of
  compare-and-swapping GIL on (E4-G extended, §37); the owner materializes a CopyOnWrite butterfly without the cell
  lock (§38: 1,495 M -> 914 M cycles for two million four-element literals, flag off 600 M); a synchronous compile is
  admitted GIL on (SPEC-jit history §49), which removes the 234 fail-stops of the no-concurrent-JIT configurations
  from the GIL-on suite.
- *The concat-key atom string cache* (SPEC-jit history §52). After the inline probes of DFG/FTL MakeAtomString
  were back GIL on, the C++ operation behind them still ran its flag-on body (the cache's lock around the map probe
  and again around the insert: 255 instruction samples in WSL). GIL on it runs `main`'s body. A 40-key site: main 414,
  flag off 420, GIL on 431.5 -> 426.6 instructions per read. Test: `jit/gil-on-concat-key-cache-across-handoffs.js`.
- *WebAssembly calls* (SPEC-ungil §I). The warm JS->wasm entry was refused with the flag on, so every call took the
  cold path: 5 M calls of an i32 add 338 ms against 11 ms flag off, and the sampling profiler saw no wasm frames (two
  JSC stress tests). GIL on it is handed out while no Thread has ever been spawned in the process, and its prologue
  tests the same process byte, so a site linked before the first spawn goes cold after it (the refusal for spawned
  threads lives on the cold path). Test: `api/gil-on-wasm-warm-entry-before-first-spawn.js`.
- *The sampling profiler and a Thread that exits* (SPEC-ungil history, tenth round; AUDIT R10-30). With the GIL on
  the profiler's sampled thread is whoever took the API lock last, so every spawned Thread binds - and stayed bound
  after it exited: the 1 ms timer's next sample signalled a thread that no longer exists, waited for its
  acknowledgement forever with the machine-threads lock held, and the main thread hung taking the API lock back. On
  every tree since the flag existed; found by the amplifier on the GIL-off profiler's new test (2 hangs in 500 GIL-on
  runs, 1 in 1,000 on the tree before that change; a hang detector took the stacks). An exiting Thread unbinds
  itself under the profiler's lock. Test: `vmstate/sampling-profiler-bound-thread-exits.js` (the main thread sleeps
  while spawned threads run and exit: hangs in 6 of 6 runs before, on the ninth round's tree too; 0 of 10 after).
**GIL off.**
- *What a poll keeps fresh* (SPEC-jit I21, history §50; AUDIT-checktraps §7.1 "narrowed, not ruled"). A GIL-off poll
  used to write every user-visible value heap, so no heap read could be hoisted out of, or reused across, a loop
  iteration. In FTL plans a pre-pass now gives each in-loop poll the set of value heaps read by the backward slices
  of the loop's Branch/Switch conditions; a read that can decide control flow is still performed again after every
  poll (a spin on a plain field, element, closure or global variable, a cancel flag, a loop bound), a read that only
  feeds data may be hoisted. Memory safety of a hoisted typed-array {vector, length} pair: a stop that retires
  ArrayBuffer quarantine entries bumps the heap-fact epoch. float-mm.c GIL off 174.3 G -> 116.6 G instructions.
  Test: `jit/poll-visibility-control-reads-gil-off.js` (ten shapes of spin released by plain stores, called
  repeatedly so the functions are entered at their top; hangs on a build that hoists everything).
- *Window liveness retention off* (SPEC-heap I12, history §37). With two or more clients every block allocated in a
  window was retained whole until the next Full collection, so Eden collections reclaimed nothing young. The
  historical reproducers, the corpus (also with `--verifyGC`), the gc-stress matrix and the scaling bench pass with
  it off; it stays behind an option. JetStream GIL off with an idle second thread: resident set 2.4-2.8 GB -> 1.2-1.35
  GB, Full collections 57-62 -> 19-25, same score. Test: `gc-stress/eden-reclaims-young-garbage-with-parked-thread-gil-off.js`.
- *Inline allocation* (SPEC-jit §5.5, history §51 and §54). Two ways an inline allocation silently became an
  operation call per allocation GIL off: the C++ slow path sizes a fresh contiguous vector to 4k-1 under the shared
  heap and the inline path did not, so it drew from a size class nothing ever refilled (arrays of a length unknown at
  compile time: 355 -> 109 instructions per `new Array(n)`); and sites that bake `allocatorForConcurrently`, which
  is empty GIL off (`str.slice(a, b)` 292 -> 156 instructions per call; the BigInt-from-Int64 and
  RegExpStringIterator result sites likewise). The rule is in §5.5 and the audit says how to find the next one.
  Tests: `jit/variable-size-allocation-stays-inline-gil-off.js`.
- *Typed-array sort of sorted input* (SPEC-ungil §N.6): GIL off `sort` works on a private copy, and took the copy
  before anything else, so a 4 GB zero-filled view - which `main` sorts without allocating - threw OutOfMemory
  (`typedarray-sort-out-of-memory.js`, 16 configurations of the GIL-off suite). A read-only scan with relaxed lane
  loads now precedes the copy for views long enough for `main`'s radix path. Test:
  `shared-objects/typed-array-sort-sorted-input-gil-off.js` (every element type, around the thresholds, plain and
  shared, NaN and -0, and a sort racing a writer).
- *A dictionary's in-place add published the name before the value* (SPEC-objectmodel I9, I42, history §41; AUDIT
  R10-24). Found by this round's own end-of-round amplifier campaign: `cve/mc-val-multislot-clone.js`, GIL off,
  signal 11, once in the campaign's 410 amplified runs of that test. Amplified alone on a loaded machine it crashed in
  13-25 of every 2,000 runs on every build of the round including the rebased tree it started from, so it is older
  than the round. All 59 recorded faults are one instruction: the general `JSON.stringify` walker dereferencing an
  empty value it had read from a listed property slot. The ordinary `o[k] = v` on a dictionary
  (`putDirectInternal`'s dictionary leg) stored its value after `addOrReplacePropertyWithoutTransition` had already
  published the table entry; every other flag-on add stores inside the add callback, before publication. In the
  window the name resolved to a slot holding EMPTY (a fresh slot) or the previous occupant's residue (a reused one),
  for every reader - the specification's "dictionary readers take the cell lock" describes no reader in the tree. The
  window is a few instructions wide and opens when the scheduler takes the writer off its core inside it (16 crashes
  in 2,400 runs with each run pinned to one core, 0 in 12,000 on a lightly loaded machine, same build). Two steps:
  first the ninth round's after-read check (same structure ID, no empty value, else the generic path) was extended to
  the readers its sweep had missed - the general stringifier's two arms, `FastStringifier`'s object arm, the
  `Object.entries` fast path (an empty value became a hole in the pair) and the `Object.defineProperties` fast path
  (`toPropertyDescriptor` dereferenced it) - which removed the crash; then the residue form showed up as wrong
  values (`w1_180 -> "w0_206!"` from `JSON.stringify`, `undefined` from `Object.entries`), also through the generic
  path, and the store was moved before the publication. Tests:
  `shared-objects/multislot-readers-vs-dictionary-add-gil-off.js` (small objects made cacheable dictionaries with
  released slots, so a reader reaches the last listed slot within nanoseconds of listing it; no amplifier, no
  pinning: signal 11 in 7 of 40 runs at first, a wrong or `undefined` value in 2 of 60 with the readers' check alone,
  0 of 120 with the store moved); the CVE test pinned and amplified, 16 crashes in 2,400 before, 0 in 2,400 after.
  The same campaign's rarer PERSISTENT divergence is an Open item.
- *Destroying a VM after join* (SPEC-api §4.6 item 4, history r10.2). Found by the mirror harness once it ran
  memory-hog tests with their own options: `--destroy-vm`, two threads, GIL off, a silent SIGABRT in 2-7 of 16 runs,
  on the round's first build too. `join()` returns when completion is published; the native thread drops the
  `Ref<VM>` of its entry closure only after its exit tail, so an embedder that joins and then releases its reference
  under the API lock was not always the last owner, and the last release, off-lock on the exiting thread, hit
  `~VM`'s fail-stop. (Taking the stack needed a SIGABRT handler with the unwinder loaded in advance and an exit
  handler that makes `main` linger: anything slower lost the race with `exit`, and ptrace hid it.) ThreadManager now
  counts, per VM, native threads that have published completion and not yet released, and
  `waitForCompletedThreadsToReleaseVM` lets the embedder wait for them before its own release; the shell's
  `--destroy-vm` and worker teardown call it. Destroying under a Thread that is still running stays a violation with
  the fail-stop. Test: `api/destroy-vm-after-join.js` (SIGABRT in 12 of 40 runs GIL off before, 0 of 40 after; the
  mirror reproducer 9 of 40 -> 0 of 40). Bun's Worker teardown should make the same call (Open items).
- *Transitions whose stale consumers are unsafe publish and fire in one stop* (SPEC-jit §5.6 "Deferred claims in
  flight", history §55; GIL-removal precondition 10, narrowed). Found by the mirror harness, whose two threads start
  together: `create-this-structure-change.js` (SIGABRT in 6 of 40 runs: optimized code added a GetterSetter cell it
  had loaded as data) and `arith-nodes-abstract-interpreter-untypeduse.js` (SIGSEGV in 2-5 of 60: optimized code
  stored a raw double into an array that had become Contiguous, and reading it back dereferenced 0x1234), on every
  build back to the round's first. The first transition out of a structure that optimized code watches claims the
  structure's transition set at once and fires it only at the end of the transition; a second thread that makes its
  own transition out of the same structure in between finds the set claimed, has nothing to fire, publishes, and
  returns into its own optimized code, which still elides the check. The CVE audit had recorded the window from the
  other side (another thread's elided check on the claimant's object) and called the late-comer benign. For three
  kinds of transition the stale code is unsafe, not merely stale - relabels out of Double, conversions to
  ArrayStorage, and data<->accessor kind changes. Those now wait (parking for stops) until no other thread's deferred
  claim is in flight - a process-wide count kept around every GIL-off deferred claim - then derive their target,
  claiming the set themselves if it is still watched, re-plan if somebody else claimed meanwhile, and publish and
  fire in the same stop. All three already published under a stop; nothing is added but the wait - which is skipped
  inside a stop (`haveABadTime` converts every array inside its own, and a claimant parked by that stop keeps its
  claim until the world resumes: the first build with the rule hung three `having-a-bad-time` stress files under the
  mirror harness until the stop watchdog fired; test
  `objectmodel/having-a-bad-time-vs-claimed-transition-gil-off.js`, SIGABRT after 30 s in 9 of 16 runs on that build,
  0 of 16 after). A first form that
  fired BEFORE publishing closed the window too and failed
  `objectmodel/indexing-transition-keeps-adaptive-watchpoint.js` (an adaptive watchpoint must find the new structure
  when it fires, or `Array.prototype`'s iterator protocol is given up and `BigInt64Array.from` throws another
  TypeError than `main`'s); the fire stays after the publication. The other deferring sites keep their order (their
  stale consumers read an old value or `undefined`; Open items). Test:
  `jit/unsafe-transitions-publish-and-fire-in-one-stop-gil-off.js` (a fresh watched structure every round, three
  threads released together: SIGSEGV at 0x1239 in 20 of 20 runs before, 0 of 20 after); under the mirror harness the
  two stress files 0 of 120 and 0 of 180 after.
- *The taint hint is per thread* (SPEC-ungil history, K4.II.15). The "tainted code may have run in this execution"
  byte that filters the taint stack walk was one VM byte made sticky GIL off, so after any tainted code had run every
  thread answered IndirectlyTaintedByHistory for good: `taintedness-tracking.js` failed in all sixteen GIL-off
  configurations of the stress suite and was carried as a known class until the ninth round's review. GIL off the byte
  lives in the lite, as the K4 inventory had ruled: the three tiers' prologues store through the running thread's lite
  (only for code blocks that could be tainted), the C++ accessors route to it, and the end of a synchronous execution
  clears the ending thread's byte only. Test: `vmstate/taint-hint-is-per-thread-gil-off.js` ("main while another
  thread is inside tainted code: IndirectlyTaintedByHistory" before; passes after, and the stress test passes GIL off
  in its default and eager configurations).
- *A requested synchronous JIT is a parked wait on the concurrent one* (SPEC-jit §5.7.3 / M2b, history §56). GIL off
  the process exited with a FATAL line when an option turned the concurrent JIT off: thirty-nine stress tests ask for
  `--forceEagerCompilation` in their headers, 234 of the 606 GIL-off suite results that differed from `main`. What
  those options promise is a time, not a thread - compiled and installed when the tier-up call returns - and the
  reason GIL off cannot give them the thread (a compilation on a mutator holds heap access with no poll) does not
  stand in the way of the promise: the request sets a derived option, the concurrent JIT stays on, and
  `JITWorklist::enqueue` waits for the VM's plans with the existing code-deletion wait (which gives up the thread's
  heap access, so stops and collections go on around it) and completes them before it returns. The sixteen
  no-concurrent-JIT configurations of every other test run as `main` runs them, too, instead of with the concurrent
  JIT and the shell's larger loop counts: the four compile-timing tests of the class list pass
  (`has-indexed-property-*-ftl` x2, `startup-jit-deferral`, `int8-repeat-in-then-out-of-bounds`). `--useProfiler`
  stays refused (two results), with its own message. Test: `jit/requested-synchronous-jit-waits-gil-off.js` (FATAL in
  5 of 5 runs before; 0 of 5 after, 0.17 s a run); the 234 suite commands re-run on the new build: all pass but the
  two `--useProfiler` ones.
- *The sampling profiler samples the carrier GIL off* (SPEC-ungil §A.1.7 form (i); AUD1.K1, SD18). The spec had
  ruled the profiler a reader that resolves the sampled thread's lite through the registry, for carrier threads; the
  implementation refused instead and a GIL-off VM never sampled: every profiler test failed (eleven stress files in
  seventeen configurations, some 170 results; Bun's two `bun:jsc` `profile()` tests), and a program profiling itself
  got empty traces. The thread that binds as the sampled thread now records its lite; `takeSample` takes the
  registry lock with `tryLock` before it suspends the target (a contended registry costs a sample, never a wait),
  checks the lite is still registered to the VM, and reads the entry record, the top frames and the executing RegExp
  from it; nothing is allocated and no lock taken while the target is suspended. Turning raw frames into traces
  iterates the heap, which on a shared heap is a stop of the other clients, made with the profiler's own lock
  dropped. Spawned threads stay unsampled by ruling. Test:
  `vmstate/sampling-profiler-samples-the-carrier-gil-off.js` ("never sampled in 1,839 reads" before; passes after, in
  both modes); the sixteen `sampling-profiler-*` stress files pass GIL off. The mirror harness then found the rule
  that was missing: the report entry points hold the profiler's lock while they name the frames, a name lookup polls
  traps and can reify a lazy property, and a reader parked there with the lock held deadlocked the next reader's
  stop (`sampling-profiler-stack-trace-with-double-quote-in-function-name.js` on two threads, hung to the deadline;
  the collector's profiler constraint takes the same lock with the world stopped). GIL off the three report entry
  points run as the conductor of a stop; with the GIL on, where the same poll hands the API lock to a thread whose
  collection or acquisition notice then waits for the lock, the holder defers its traps. Test:
  `vmstate/sampling-profiler-two-readers-gil-off.js` (GIL off: stop-watchdog abort in 4 of 4 runs before, 0 of 10
  after; GIL on: hung in 2 of 5 runs on the tree before any of the round's profiler work, 0 of 10 after); the
  sixteen stress files under the mirror harness, three passes, no finding.
- *The exception-check validator GIL off* (AUDIT R10-29). The GIL-off Map/Set rehash hashed each copied key through a
  throw-scoped helper and never checked, so with `validateExceptionChecks` (a Debug facility) the second string key
  of any rehash aborted: two of Bun's tests run fixtures under the validator and failed GIL off while loading a
  module (`new Set([...])`). The loop asserts after each hash, under a scope that hands no check to its callers. Test:
  `api/map-set-rehash-under-exception-check-validator.js` (Debug: abort before, passes after). The Debug corpus was
  then run with the validator on in both modes: 385 / 358 pass and no validator report (the one failure in each lane was the old test that asserted the profiler's refusal, rewritten).
- *A call site's history across its callees' tier-ups* (SPEC-jit §5.8, history §58). Found late, by counting OSR
  exits per test GIL off against GIL on (the counts do not depend on load): Basic 13,205 against 5,890,
  OfflineAssembler 2,434 against 757, pdfjs, Babylon, delta-blue the same way - the DFG being told the same wrong
  thing at every recompilation. A polymorphic call stub's slot cannot be rewritten in place GIL off when that
  variant's callee tiers up (other threads execute through the slots), and the fallback unlinked the whole site, so
  every tier-up of every variant reset the site to "monomorphic on whoever comes next", variant list and call counts
  gone; the DFG speculated on that one callee and exited at the next (Basic: 4,002 BadConstantValue exits at the
  callback call inside `map` against 801). Every install - a tier-up, and a jettison's reinstall of the alternative -
  now publishes a copy of the stub with that slot upgraded and the call counts kept, through the same `setStub`
  publication a new variant uses; never from a thread doing the collector's work (the End phase jettisons dead code
  block edges through the same drain, where a routine may neither be allocated nor run write barriers and where
  callers just found dead are still listed). The first build republished there, and the GIL-off JSC suite caught
  it: `stress/array-shift-intrinsic.js` crashed in the collector in the three configurations that run it without the
  FTL, every time (a dead CodeBlock on the mark stack; Bun's `web/fetch` directory aborted on the same assertion).
  After: Basic 6,060 exits (GIL on 5,890), delta-blue 1,770 (1,603); instructions Basic -4.5 %. Tests:
  `jit/polymorphic-call-site-keeps-its-history-across-callee-tier-up-gil-off.js` (`map` compiled by the DFG 6 times
  and 3,105 exits before; 4 and 703 after, which are `main`'s numbers with the synchronous JIT);
  `jit/polymorphic-call-stub-republished-while-threads-call-through-it-gil-off.js` (four threads calling through
  forty shared four-variant sites while their callees tier up: 334 full unlinks before, 3 after - the collector's -
  every result checked; TSan: 0 reports); `jit/polymorphic-call-stub-not-republished-by-the-collector-gil-off.js`
  (SIGSEGV 3 of 3 on the first build, 0 of 3 after).
- *Tried and withdrawn: the generic `PutById` for puts the LLInt cannot profile* (SPEC-jit history §57). The other
  half of the exit difference: with tagged words the LLInt publishes no `put_by_id` transition cache (§4.3), the DFG
  reads the null fields as "never ran" and forces an exit, and a constructor inlined while still in the LLInt exits
  at its first `this.x = ...`, in the middle of the LLInt callee, whose entry counter therefore does not move
  (OfflineAssembler: 1,480 InadequateCoverage exits, none GIL on). Emitting the generic `PutById` there brought
  OfflineAssembler's exits to GIL on's and left its score where it was; the quiet pass's scaling gate then showed
  `scaling/richards-like.js` GIL off at 4.7 s on one thread against 2.4 s and 56 s on four against 5.6 s, with a new
  storm of 1,683 BadCache exits in its main function. A binary that can switch the rule off at run time confirmed the
  cause; why the consumers' structure checks fail there was not established. Withdrawn; Open items.
- *`api/lock-async-hold.js`, the hang carried since the ninth round, is a race in the test.* It timed out once in
  this round's final Release lane; a loop with a hang detector (eight lanes under load, gdb on any run alive after
  45 s) caught three in 3,200 runs, all the same: two native threads left, the main thread parked in `lock.hold()`.
  Section 5 of the test spawns a thread that calls `lock.asyncHold()` and then takes a sync hold meant to be in
  place first; a spawned thread that gets there first is granted the lock at registration and exits, and the only
  caller of the release function is the thread now parked. With a 50 ms delay before the hold it hangs in every
  run, GIL on too. The spawned thread now registers from inside the main thread's hold (0 of 80 after; with the same
  delay 0 of 8).
- *Smaller*: the split paths reuse a per-thread offset vector instead of a `fastMalloc` per call; the Double
  promotion of an allocation site learns from the substituted Int32->Double request as well as from sampled lanes
  (SPEC-objectmodel T4-P, history §40; test `objectmodel/double-array-profile-promotion-by-request-gil-off.js`).

**Flag off.** The per-symbol sweep (flag off over `main`, 36 tests): instructions +1.8 % in sum, +2.5 % geometric
mean; OfflineAssembler +12.7 %, regexp +7.5 %, pdfjs +6.8 %, splay and UniPoker +6.3 %. Fixed: a DFG clobberize
helper whose early return the compiler had left out of line (336 samples of 220,329, 1.5 % of Babylon); the RegExp
matching context's constructor, out of line since it learned the GIL-off slot (66 samples of OfflineAssembler's
6,912). Left, with their measure: the RegExp and substring operations (per-match gates on the GIL-off scratch,
cached-result and compile paths; `exec` +5 %, `replace` +11 %, regexp `split` +16 %, string `split` +14 % instructions
per call on a microbenchmark that defeats constant folding), the marking loop (+5 % instructions per collection:
relaxed-atomic visit counters and the helper-pause checkpoint), `StringImpl::deref`'s shared-table latch. Reason and
impact of every change to code that runs flag off are in the list at the end of this section.

**Measured and not adopted.** A service conductor thread for GIL-off collections (the main thread stops being the
passive conductor that waits out the concurrent window): splay Worst x2.3-3.6, but a second client engages the
client-count heuristics and FlightPlanner, earley-boyer, hash-map and Babylon Worst fall; total +2.8-4.4 % with
retention on, +0.3 % with it off. Needs the heuristics to ignore the service client first (Open items). Hoisting every
read across polls (no visibility rule): the ping-pong test hangs, as it should. The storage-then-shape re-check
protocol (SPEC-objectmodel history §29) was surveyed again and left: see Open items for what it would buy now.

**Final battery.** The round's last items came in one after another and each was found by the battery of the tree
before it, so the lanes below ran on consecutive trees whose differences are stated. The corpus, TSan and the GIL-off
JSC suite ran on the last tree (the suite on the tree before it as well: the same classes, 198 against 197). The
mirror pass, the two-hour campaign and Bun's GIL-off lane ran on the tree that differs from it by three things: the acquire load of `trySingleTransition` (the same `mov` on x86-64), two tests' loop
counts, and the generic-`PutById` rule that the last tree withdrew (the last tree has `main`'s code there, the code
every earlier battery of the round ran with). The flag-off and GIL-on JSC suites and Bun lanes ran on the tree before
that one, which differs from it only inside GIL-off-only branches (the collector exclusion of the stub
republication). Corpus, four modes: Release 391 / 364 / 62 / 48 pass, Debug the same counts, 0 failures in all eight
lanes. TSanJIT: GIL off 391 pass, GIL on 364 pass, 0 reports, 0 failures (on the tree before, one timeout that is the
tool's: the profiler's suspend signal landing inside the sanitizer's allocator; TSAN-RESULTS). The Debug corpus with the
exception-check validator on, both modes: no validator report. The round's new and touched tests under the
amplifier, 500 runs each in both modes: clean on the final trees (on the way: one run in 500 of the synchronous-JIT
test, two GIL-on hangs in 500 of the profiler test, each leading to a fix recorded above). JSC suites against `main`
(579 failures): **flag off** 581 - FFI executable-memory runs that fail on `main` alone too (four in, three out) and
`re-enter-resolve-rope-string.js`, whose watchdog loses against a rope that grows a gigabyte a second on `main` as
well (3 of 9 single runs on `main` pass a 40 GB cap; twice this round it passed 100 GB in a suite run before it was
stopped, and a guard stopped it at 40 GB from then on); **GIL on** 617 - the flag-off set plus the 34 configurations
of `SharedArrayBuffer.js` / `SharedArrayBuffer-opt.js` (`Atomics` accepts ordinary objects with the flag on: the API
extension) and FFI runs; the round started at 279 that pass on `main`; **GIL off** 773, 197 of them passing on `main`
(round start 637): 117 WebAssembly off, 34 `Atomics` on objects, 16 `ffi-callffi-was-compiled` (no FFI inline cache
GIL off), 16 `array-slice-cow` (T4-C: a copy of a Double source is Contiguous GIL off; visible through
`$vm.indexingMode` only), 2 `--useProfiler` (the bytecode profiler, refused GIL off), `baselinejittrue` (WebAssembly
off sets `useLLInt`, as `main` with `--useWasm=0`), and, not design: 9 FFI executable-memory runs and
`class-subclassing-function` x2 (the fork's code-cache key, Open items); the rope test passed in this run. Gone since the round's
start: the 234 fail-stops in both flag-on modes, some 170 sampling-profiler results GIL off, `taintedness-tracking`
x17, `typedarray-sort-out-of-memory` x17, the four compile-timing tests, the sampling-profiler-wasm pair GIL on.
The suite also caught a crash the round had introduced an hour earlier (the stub republication running from the
collector; above). Mirror harness, Release eval GIL off, 5,831 files on two threads: 19 findings, all of the standing
artefact classes (`$vm` inline-cache and type-profiler dumps that abort on a second thread by design; the
`$262.agent`, `waitasync` and shared-array-buffer choreographies that two threads running the same global code cannot
complete); the files it found on the way - `unlinked-code-block-destructor` (VM teardown),
`create-this-structure-change` and `arith-nodes-abstract-interpreter-untypeduse` (deferred claims), three
`having-a-bad-time` files (the first form of that rule), `sampling-profiler-stack-trace-with-double-quote-in-function-
name` (the profiler's lock) - are fixed above. Amplifier campaign, four modes in parallel for 2 h on the Release build (08:54-11:04): default set GIL on 12 passes
and GIL off 9, CVE set GIL on 48 and GIL off 42, ten seeds per test per pass. No crash, no timeout, no failing
reference run. Divergent-output flags: the standing list (the six `scaling/` workloads, `jit/int-gate-stop-budget.js`,
`dump-registers-gil-on-vm-in-gil-off-process`, the seven interleaving-dependent CVE tests). One exit divergence, in
one of those seven: `cve/mc-spec-timer-capability.js` asserts that a free-running counter thread advances between two
back-to-back loads at least once in 100,000 samples, and in one run of 420 it did not (the counter thread was not
scheduled for the few milliseconds of the sampling; the machine ran four campaign lanes, a suite and Bun's tests at
the time). Earlier campaigns of the round, on the candidates they led to fixes on, found the dictionary-add defect
and the loop-entry bound's one excess (1,072 refused against 1,000; Open items). Bun, twelve directories against a stock build of the same commit, the three modes side by side in separate
processes: **flag off** no behaviour differs - the four `fetch-tcp-stress` cases and
`abort-signal-leak-read-write-file` cross their time budgets (the last measured side by side: stock 293 s, branch
321 s, budget 300 s, no leak on either), and `bun/jsc`'s DOMJIT cases time out at 60 s on stock and on the branch in
different subsets; **GIL on** the same cases, `fetch-leak`'s URLSearchParams case over its budget, and one
`web/workers` case ("worker exit with streaming-request-body fetches") that passed 3 of 3 alone (three Bun runs, a
campaign and three suites shared the machine); **GIL off** (rerun on the following tree after the first run's
`web/fetch` directory aborted on the collector defect above): against stock the 47 WebAssembly cases, the FFI
inline-cache test, the four `fetch-tcp-stress` cases and `abort-signal-leak`, and the three standing differences of
Open items (the incomplete-body memory bound, `node/vm`'s collection-heavy test at its limit, the worker `loader`
fixture that instantiates a module); against the ninth round's final tree 12 fewer: the two HEAD-stream retention
checks, the two `profile()` tests, the two exception-check-validator tests, the six `Bun.stripANSI` checks.

**Parity, configuration by configuration** (the final tree; which lane ran on which of the last trees is in the
paragraph above). Targets from the round's brief; "classes" means the classified lists above.

| | flag off | GIL on | GIL off |
|---|---|---|---|
| Behaviour: JSC suites vs `main` | 5 fail that pass on main: FFI executable-memory runs that fail on main alone too, and the rope test that outgrows its watchdog on main as well | 38: those plus the 34 `Atomics`-on-objects results (the API extension); 279 at the round's start | 197: 117 WebAssembly off, 34 `Atomics` on objects, 16 FFI inline cache, 16 `array-slice-cow` (`$vm` only), 2 `--useProfiler`, `baselinejittrue`, and 11 not design (FFI runs, the fork's code-cache key); 637 at the round's start |
| Behaviour: Bun, 12 dirs vs stock | time budgets only: the four `fetch-tcp-stress` cases and `abort-signal-leak` (throughput, Open items) | the same, `fetch-leak`'s URLSearchParams case over budget, one load flake (3 of 3 alone) | the 47 WebAssembly cases, the FFI inline-cache test, the same budgets, three standing differences (Open items); 12 fewer than the ninth round's tree, none new |
| Safety: corpus (Release, Debug) | 0 failures (flag-off lanes) | 0 failures | 0 failures |
| Safety: TSanJIT | - | 0 reports | 0 reports (one signature fixed on the way) |
| Safety: amplifier, 2 h + 500-run | clean | clean (known divergent-output list) | clean (known list; one scheduling-dependent assertion of a listed test in 420 runs) |
| Performance: JetStream | 0.97-0.98 of main (0.994 in the pass where main's earley-boyer ran slow; target 0.99; start 0.96-0.975) | 0.95-0.96 of flag off (target 0.95; start 0.91) | 0.80-0.81 of GIL on (target 0.90; start 0.83; GIL off itself +2.5 %, GIL on +6 %), 13 rows below 0.80 |
| Performance: micro rows | within 1.06 of main except array-int32-to-double-relabel 1.18 | above 1.10: class-ctor-4 1.31, transition-heavy-constructor 1.24, map-set-get 1.19, astar 1.15, regexp-exec 1.13; the 1.3x-2.7x rows of the start are at 1.00 | unchanged from the start (every ratio to GIL on rose by what GIL on gained) |
| Scaling (4 threads) | - | - | splay-like 3.59x, raytrace-like 3.33x, map-heavy 3.26x, string-heavy 2.4x or 0.9x (bimodal), richards-like 1.15x |

**Changes to code that runs flag off, with reason and impact.** The round's source change was read hunk by hunk against
the gates that make code flag-on only (the flag itself, the derived tagged-word option, the GIL-off predicates, the
`jsThreads` template legs, the threads-only files): 376 sites are pure re-keys, 85 hunks are flag-on only, 73 run flag
off. The re-keys replace `Options::useJSThreads()` by `Options::useTaggedButterflies()` (and the LLInt's
`ifJSThreadsBranch` by `ifTaggedButterfliesBranch`) in C++, the JIT emitters and the LLInt; flag off both bytes are 0,
the byte is declared nine bytes after the first, and the predicted-false test is the same instruction.
- *Rebase fix-ups, converging on `main`*: the replayed commits did not build on the new base, and the hand-merged tree
  takes `main`'s form where upstream moved: the Full-GC timer no longer bails under memory pressure
  (`FullGCActivityCallback.cpp`, now identical to `main`), the Yarr JIT prologue lost a stale stack check
  (`YarrJIT.cpp`, identical to `main`), the parallel-marker waiting count is balanced, the Date narrowing bound, the
  RegExp minimum-size fast-fail and eager `m_ovector` sizing, `setLength` / `increaseVectorLength` clearing per
  element, the wasm module-loader argument. One deviation from `main` remains in the three resolve-scope operations:
  the resolve type is loaded once (relaxed) before `JSScope::resolve()` and used for both the module-variable test and
  the switch, where `main` loads it again after the resolve; it differs only if the resolve re-enters the same
  `op_resolve_scope` and rewrites its metadata across case groups (a Proxy `has` trap on the global's prototype
  chain), and then only in what gets cached.
- *Removed cost*: `jsThreadsParkableSlowPathClobbersHeapFacts` split into an inline gate and an out-of-line GIL-off
  body (called from every `clobberize`, `executeEffects` and `clobbersExitState`: one call less per node per phase;
  336 instruction samples of 220,329 over the suite, 1.5 % of Babylon); the RegExp matching context's constructor
  inlines into the match operations again (its GIL-off lookup moved out of line: one call less per match); the split
  paths no longer construct and destroy an empty local vector; one test less in the single-variant
  `MultiPutByOffset` fold.
- *Added predicted-false tests* (each one byte load and a branch): per call of `ArrayMode::fromObserved` (DFG parse
  time); per allocating-transition fold and per `tryFoldAsPutByOffset` in DFG constant folding; per
  `operationNewArrayWithSize` (the round's counter macro); per `operationArrayIndexOf*` / `Includes*` and the
  copy-on-write string search (`searchableLengthOfStorageFromJIT`); once per collection (`didFinishCollection`);
  per synchronous `JITWorklist::enqueue` (only without the concurrent JIT); per property read by offset in the general
  JSON stringifier and in `FastStringifier`; per call of the `Object.entries`, `Object.values` and
  `Object.defineProperties` fast paths; per `{...o}` clone with out-of-line storage (`butterflyConcurrentCopyWords`,
  then the same `memcpy`); in the SlowPutArrayStorage arm of `trySetIndexQuickly` (a rare shape); in the taint hint's
  accessors (the eval and Function-constructor paths); once per JS->wasm entry thunk generation and once at shell VM
  teardown; per concurrent `JITWorklist::enqueue` (the derived wait-for-plans option); per sampling-profiler sample
  and per trace report (`takeSample` reads the sampled thread's four words through a mode test and the frame walkers
  take the entry frame as an argument; the four report entry points test `gilOff()` before main's heap iteration).
  None is on a per-instruction path of generated code; none showed in the per-symbol sweep.
- *Compile time*: the poll-visibility analysis is a phase of every FTL plan and returns at its first test flag off
  (three to four empty phase objects per plan; it shifts phase numbers in dumps); `DFG::Graph` grew by one pointer;
  DFG/FTL `NewArrayWithSize`, `emitAllocateButterfly` and `emitInitializeButterfly` test `useSharedGCHeap` at compile
  time (false unless that option is set on its own, which no configuration does); four allocation sites take an
  optional GIL-off arm and otherwise the same constant allocator.
- *Same instruction, different memory order*: `GetByIdModeMetadata::setProtoLoadMode` (relaxed atomic store of the
  cache word), `RegExp::minimumSize` / `setMinimumSize` (relaxed).
- *Options*: three new ones, none read flag off (`useSharedGCWindowLivenessRetention`,
  `useJSThreadsPollVisibilityAnalysis`, `useJSThreadsSingleOwnerWithGIL`), and the derived `useTaggedButterflies`
  and `useJSThreadsWaitForJITPlans`, recomputed at every options change and always false flag off. No default changed. With the flag on,
  `usePollingTraps` and `useConcurrentJIT` are forced only when the GIL is off.

### Open items

Work that is not done, after the tenth round. Each item says why. Closed by the tenth round and gone from this list:
the mirror pass running memory-hog tests without their options (the harness passes each file's own options now, which
is what found the two teardown and transition defects in its "Results" section); GIL-off memory with two or more
allocating threads and the two-Full retention Bun's HEAD-stream checks counted (window liveness retention is off);
the taint flag over-reporting GIL off; `typedarray-sort-out-of-memory`; the fail-stops of the no-concurrent-JIT
configurations GIL on and, by waiting, GIL off; the sampling profiler's GIL-off refusal; the `api/lock-async-hold.js`
hang (a hang detector took its stacks this round: only the main thread is left, parked in its sync hold - the
spawned thread had reached `asyncHold()` first, was granted the lock at registration and exited; a race in the
test, which now starts the spawned registration from inside the main thread's hold); the GIL-on items
"property-adding transitions in C++" and "the Double relabel" (a GIL-on process runs `main`'s object model).

- **GIL off below GIL on on JetStream, by mechanism** (PERF-RESULTS §6.12; 0.80 of GIL on in the quiet pass, target 0.90; 13 rows below 0.80). Instructions GIL
  off over GIL on, 36 tests: 1.25 (geometric mean), 68 % of the difference in generated code.
  (1) *The Double family*: navier-stokes 2.45x, ML 1.89x. Arrays that start as integers and meet doubles become
  Contiguous, not Double (T4-O), and run on boxed values; ML's hot loops also read rows of three shapes. The
  allocation-profile promotion (history §28, §40) cannot reach either: navier-stokes allocates its arrays once, ML's
  rows come from many sites. Needs the transition itself: the storage-then-shape re-check protocol (SPEC-objectmodel
  history §29), surveyed again this round (132 uses of the double-lane accessor in C++, 93 Double-shape switch arms,
  four tiers, one new DFG node) and again left as a change to land alone with its own audit table.
  (2) *The visibility rule in loops whose condition reads the heap*: delta-blue 1.95x, hash-map 1.61x, and the
  10-35 % on the object-heavy tests. After every poll, GIL-off code re-reads whatever can decide the loop's exit
  (SPEC-jit I21), and everything derived from a re-read value is recomputed, checks included. Poll visibility
  (history §50) already released the reads that only feed data. Going further means deciding that a plain read in a
  loop may stay stale for the loop's lifetime unless the program uses `Atomics` or a lock - a memory-model decision,
  listed under decisions below.
  (3) *Array growth that copies*: stanford-crypto-aes 1.64x, half of it in the concurrent growth, store and
  materialization helpers. In-place growth of an owned flat butterfly GIL off is decision (b) below.
  (4) *RegExp and strings*: `exec` 1.15x, `test` 1.27x, `replace` 1.29x, regexp `split` 1.52x per call; the inline
  calls into Yarr code, constant folding against constant strings and the split caches are off GIL off, and the
  legacy-statics stream and match scratch are looked up per match.
  (5) *Collection latency on the main thread* (splay, gbemu, ML Worst Case): the main thread is the passive
  conductor and waits out the concurrent window. A service conductor thread was built and measured (splay Worst x2.3-
  3.6) and not adopted: the second client it creates engages the client-count heuristics of the allocation limits,
  and FlightPlanner, earley-boyer, hash-map and Babylon Worst fall; total +0.3 % with retention off. It needs those
  heuristics to ignore a client that never allocates.
  (6) *stanford-crypto-sha256 is bimodal GIL off* (about 325 or 620 in the suite; the table's median lands on
  either by luck, on the round's first tree too). Single runs split into two populations by instruction count, 10.0 G
  (scores 460-600) and 10.46 G (310-410); GIL on 8.36 G and stable. The slow population is an exit storm in the block
  function: 1,504 BadType exits at one `add` (none GIL on), each batch of a hundred ending in a reoptimization that
  doubles the function's threshold, so it spends the early iterations in Baseline and reaches the FTL late
  (first-iteration time 7 ms against 2-3 ms, which the Startup component squares into the score). The operand is a
  local loaded from the message-word array, which GIL off is Contiguous holding boxed doubles where `main` has a
  Double array (T4-O: item (1)'s family); the DFG speculates Int32 from a value profile that saw no double during
  warm-up, and the exit reports the offending value to no profile (the check sits on a `GetLocal` with the consuming
  node's own origin, which `methodOfGettingAValueProfileFor` skips - `main`'s rule, harmless there because a Double
  array types the load). Whether warm-up meets a double decides the population. The same family shows as
  BadIndexingType storms GIL off only: typescript 1,700, ML 263, stanford-crypto-aes 201.
  (7) *Tag predicates and polls everywhere else*: the broad remainder. A process that has not yet spawned a Thread
  could run untagged code and switch at the first spawn (every butterfly is already TID 0 there); that switch needs
  every structure's thread-local sets armed and every property table re-laid out at the switch, and was not designed.
- **GIL on above flag off** (0.95-0.96 of flag off, target 0.95 met; instructions 1.015). What is left: handler inline caches in FTL code
  (flag off patches stubs; forcing handlers flag off: WSL +3.5 %, Air +1.4 %, Babylon -15 %, so a wash as a rule); the
  packed self-access word and the refused prototype and dictionary cases of the Baseline inline caches; put-to-scope
  metadata frozen after link; atomization through the shared atom table. DFG/FTL `CallWasm` is still off flag on, and the warm JS->wasm entry is
  handed out only until the first spawn.
- **`--useJSThreadsSingleOwnerWithGIL=0`** restores per-thread butterfly tags with the GIL on (the ninth round's
  behaviour); three corpus tests that observe ownership use it. It is an escape hatch, not a supported configuration
  for performance.
- **Flag off above `main`** (0.97-0.98 of `main` on JetStream, target 0.99; instructions +1.8 % in sum, +2.5 % geometric mean). Spread over:
  the RegExp and substring operations (OfflineAssembler +12.7 %, regexp +7.5 %: a Config-page test per per-thread
  redirection - match scratch, cached result, compile check, stack limit - each 2-3 instructions, and two helpers the
  compiler no longer inlines, `jsSubstring` and `StringImpl`'s 8-bit constructor); the marking loop (+5 % per
  collection: visit counters as relaxed atomics, the helper-pause checkpoint); `StringImpl::deref`'s shared-table
  latch; `JSArray::tryCreate`, `resolveRope`. Each is a flag-off change to a hot function shared with the flag-on
  configurations; none was taken without its own before/after on all three.
- **Window liveness retention is off by default** (`useSharedGCWindowLivenessRetention=0`; SPEC-heap I12, history
  §37). The reproducers that stood guard over it pass without it; the option stays for bisecting a lost-edge report.
  With it off the ninth round's "GIL-off memory with two or more allocating threads" item and the two-Full retention
  that Bun's HEAD-stream counts saw are gone (both checks pass GIL off in this round's Bun runs).
- **ThreadSanitizer builds differ from what ships, GIL on**: they keep polling traps (the sanitizer defers
  asynchronous signals), so signal-delivered traps are covered by the Release and Debug lanes only.
- **Upstream's benign races are now ours to watch GIL on**: a GIL-on process runs `main`'s object-model and
  interpreter paths, which were never TSan-clean; the corpus found two (AUDIT R10-23). A wider TSan run (the JSC stress
  suite GIL on under TSan) was not done.
- **The fork's code cache compares keys without the source text** (`SourceCodeKey::operator==` under
  `USE(BUN_JSC_ADDITIONS)`): two `new Function` bodies of equal length whose 32-bit hashes collide share one
  cached function. `class-subclassing-function.js` hits it after a few hundred distinct bodies on `main` too. Not a
  threads issue; reported to the fork's owners.
- **Bun's additional module loaders** (AUDIT R10-12): only the main thread loads modules GIL off, and every loader
  must enter through `importModule`'s refusal on a spawned thread.
- **A persistent wrong value under heavy foreign churn, GIL off** (found with a diagnostic variant of
  `cve/mc-val-multislot-clone.js` after the dictionary-add fix; present on the round's first build). Two threads add
  and delete rolling windows of their own keys on one object a third thread created; about once in 100,000 amplified
  runs on a heavily loaded machine (never on a quiet one, never with each run pinned to one core) the object itself
  ends up with one thread's value in the other's slot: `o["w1_67"]` reads `"w0_55!"` through every path including
  the generic one, `w0_55` is absent, a write through `w1_67` lands in that slot, and the wrong value stays until the
  key's owner rewrites it (21 to 499 of the reader's 4,000 sweeps). The keys' owners only ever ADD a key that is
  absent and DELETE it sixteen iterations later, so the stray store belongs to an add. The transition paths store
  before claiming only under the cell lock with the source re-verified; the in-place dictionary add allocates its
  offset under the cell lock; no duplicate offset was seen in a table. Not explained. Next step: a build that
  checks, under the cell lock at every flag-on add, that no other table entry maps to the offset being handed out
  and that a reused slot's previous owner is gone from the table, run under the same load.
- **Bun's VM teardown and `waitForCompletedThreadsToReleaseVM`** (SPEC-api §4.6 item 4): an embedder that destroys
  a VM on which Threads ran must wait for the native threads that have published completion to release their
  reference, or its own release may not be the last and `~VM` fail-stops on the exiting thread. The shell does; Bun's
  patch does now (in its `destroyVM`, which otherwise takes every outstanding reference, the exiting thread's
  included). No failure was observed without the call in 5 x 40 Worker teardowns GIL on, and GIL off a Worker cannot
  spawn Threads (below), so the call is covered by the contract and the shell's test, not by a Bun test.
- **What is left of GIL-removal precondition 10** (SPEC-jit §5.6 "Deferred claims in flight", history §55). The
  three transitions whose stale consumers were unsafe now publish and fire in one stop. Every other deferring site
  (property additions, deletions, attribute changes that keep the kind, prevent-extensions, prototype changes) still
  publishes first and fires at scope exit, and a thread that finds the set already claimed still returns into its
  own optimized code before the claimant's fire. In that window elided checks read a slot that holds a value of the
  kind they expect - the old value, `undefined` after a delete - or, for an owned Double array relabelled while
  another thread reads it, a boxed lane as a double (a wrong number, never a pointer): wrong values for at most a
  stop's length, which the staleness model allows for unsynchronized access. Closing it everywhere means firing before
  publishing at every site, which needs a restart path out of every lock-holding transition and gives up the adaptive
  watchpoints' move to the new structure. `cve/mc-code-deferred-fire-stale-window.js` stays the record of it. One
  corner of the three protected transitions is open as well: inside somebody's stop they cannot wait, so a
  conversion `haveABadTime` makes while another thread is parked between a claim on the same structure and its
  fire is published against that unfired set, and code that constant-folds the converted object's structure could
  run stale between the end of that stop and the claimant's fire (one realm's one-time event against one thread's
  first transition out of the same watched structure; not observed).

- **Bun GIL off: the standing differences that are not design.** Against stock, after the classes that are decisions
  (WebAssembly off: 47 cases across `bun/jsc`, `web/fetch`, `web/workers`; the FFI inline-cache test: 1) and the
  throughput class shared with flag off: (1) `bun/http` "request body leak ... streaming the body incompletely": the
  fixture's resident size peaks 394 MB above its start against the test's 256 MB bound in Bun's Debug+ASAN build
  (stock 57 MB; the ninth round's tree failed the same scenario on its other bound, 98 MB retained against 64) and
  is back to the stock level at the scenario's end; suspected, not verified: request bodies are freed by finalizers,
  and the ones the keep-alive thread's sweeps find are posted to the VM's thread and run a loop turn later; (2)
  `node/vm` "SourceTextModule link() m_resolveCache survives concurrent GC" reaches its 120 s limit (a
  collection-heavy loop in a Debug build GIL off; "Debug-build collection cost GIL off" below). Closed this round:
  the two `bun:jsc` `profile()` tests (the sampling profiler samples GIL off), the two exception-check-validator
  tests (AUDIT R10-29), and the six `Bun.stripANSI` "returns the same object" checks, which compared `heapStats()`
  string counts across a call while the keep-alive preload's thread made strings - the preload allocates objects and
  arrays only now.
- **WebAssembly GIL off: what it needs, measured.** It is the largest difference left (111 of the GIL-off suite's 200
  results that pass on `main`; 47 Bun cases; `typeof WebAssembly === "undefined"` for every program). The spec's v1
  is carrier-only execution (SPEC-ungil §I); what stands between that and the forced-off state: (a) the tiers'
  exception checks, top-call-frame stores and catch hand-over read the VM's words, which GIL off are inert - fifteen
  sites in seven files (`InPlaceInterpreter.asm` 6, `WasmThunks.cpp` 3, `WasmBBQJIT.cpp`, `JSToWasm.cpp`,
  `WasmToJS.cpp`, `WebAssemblyBuiltinTrampoline.cpp` 2 each, `JSWebAssemblyInstance.h` 1) plus the OMG generator's
  instance-relative loads, to reroute through the lite like the JS tiers; (b) no tier polls: IPInt, BBQ and OMG
  loops reach no safepoint, so every stop and every shared-heap collection would wait out a wasm loop and the 30 s
  stop watchdog would abort a long kernel - a poll at loop headers and function entries in three tiers, GIL off
  only; (c) instance state (memory base and size across `memory.grow`, tables, globals) is single-mutator - fine
  carrier-only, and the refusal on spawned threads exists (GIL on uses it). An alternative for (a) that would also
  give the carrier its FFI inline cache back: embed the carrier's lite in the VM so that the VM-level words ARE the
  carrier's (flag off only constants change); not evaluated.
- **GIL off, only one VM of the process spawns Threads.** A Worker's VM in a GIL-off process is a GIL-on VM (U0b:
  the first VM to go GIL-off wins) and `new Thread` there throws the cap's RangeError ("too many live Threads (or
  thread-ID space exhausted)"), which says nothing about the reason; with the GIL on a Worker spawns Threads. Seen
  this round when a Worker-teardown script was tried GIL off. The message should name the rule; lifting the rule is
  the multi-VM shared heap, not designed.
- **A property-adding put in a function still in the LLInt forces an exit GIL off, every time** (SPEC-jit §4.3,
  history §57). The LLInt publishes no transition cache with tagged words, the DFG reads that as "never ran", and a
  small constructor inlined before it reached Baseline exits at its first put until it has been entered often enough
  from the jettisoned caller: OfflineAssembler 2,335 exits GIL off against 757 GIL on, 1,480 of them this. The parser
  rule that emitted the generic `PutById` instead was withdrawn (it made `scaling/richards-like.js` two to ten times
  slower through a BadCache storm whose cause was not established). The proper form is §4.3's charter: the transition
  cache back as an immutable single-pointer record that `PutByStatus::computeFromLLInt` can read.
- **Bun needs its patch** to build and run against this branch, kept outside this tree: the ninth round's (the
  eighth round's ten changes plus the destructor marshalling) plus one call added in the tenth - Bun's VM teardown
  waits, API lock dropped, for completed Threads' native threads to release their VM reference before it drops every
  reference itself (SPEC-api §4.6 item 4).
- **`vmstate/loop-entry-when-replacement-is-ftl-gil-off.js`: the margin of its bound.** A counter that grows with
  load, not a lock-out (ninth round: 1,601 under TSan at load 60-130, 1,429 once in 100 amplified runs). Tenth round:
  one flag in 8 default GIL-off passes of an earlier campaign of the round (1,072 refused against the bound of 1,000), none in the final campaign's 9 passes. If it keeps recurring, the bound can scale with the observed failed-entry threshold.
- **Decisions recorded for the user, not taken**: (a) SPEC-objectmodel history §29 (encoding-changing shape
  transitions as copies with reader re-checks) - what it would buy now is in the first item below; (b) in-place
  vectorLength raise GIL off as an x86-64-only rule versus segmented growth for owned large arrays
  (stanford-crypto-aes); (c) the visibility rule: the tenth round released the reads that only feed data across a
  poll (SPEC-jit history §50); releasing the reads that decide a loop's exit too means a plain read in a loop may
  stay stale for the loop's lifetime unless the program uses `Atomics` or a lock (delta-blue, hash-map).
- **GIL on `earley-boyer` bimodality**, **`big-int-strict-spec-to-this`'s compile-count assertion under load**,
  **Bun natives on JSC-spawned threads**, **Bun's accept loop has no bound**, **F31**, **F2 after the fire, DFG
  tier**, **ArrayStorage `shift()` O(n) flag-on**, **F29**, **F30**, **the concurrent indirect-eval declaration
  observation**, **GIL-off latency class**, **Debug-build collection cost GIL off**: as after the eighth round; none
  was worked on in the ninth or the tenth.
- **Thread affinity of Bun's other destructors.** Only the generated classes
  and `Strong` are marshalled. Hand-written C++ destructors under
  `src/jsc/bindings` and the JSSink classes were not audited for
  thread-affine teardown; a JS thread's sweep can run them GIL on and GIL off
  (SPEC-heap §10G states what they may assume).
- **VMManager counters under the shared collector's stop, GIL off with
  Workers (AUDIT R9-20).** Bun's `worker_threads` test aborted in
  `enterStopTheWorldParticipation` (stopped + blocked > active) in 1 of 3 and
  1 of 5 runs of the r9e tree under heavy load, and once in the r9b ledger
  run; 0 of 28 later runs, including 8 of the same r9e binary at a lower
  load. The count that goes wrong is `stopped`: `resumeTheWorld` leaves it to
  each woken thread to decrement, and the next stop recounts only entered VMs,
  so a thread still counted stopped whose VM escapes the recount leaves
  stopped > active. Candidate escapes: a representative that arrived at a trap
  poll outside any VMEntryScope (Bun runs C++ from its event loop; the jsc
  shell never does, and seven jsc repros never failed), a sibling's exit
  decrementing the VM while its representative is counted, a VM destroyed
  during a stop. Fix design, not built for want of a repro to test it: a
  per-VM counted-stopped flag included in the recount, the representative
  uncounting itself at participation exit (and a non-entered VM on the way
  out), destruction uncounting a VM not counted stopped, and `stopped <=
  active` asserted at the recount. The diagnostics shipped this round (the
  last 128 counter transitions and every VM's state, printed when the
  invariant breaks) name the escape the next time (SPEC-ungil history).
- **Module evaluation claim (AUD1.K3(c)).** The move of a cyclic module
  record to Evaluating is not claimed under its cell lock; evaluation driven
  from a spawned thread (a deferred namespace's `evaluateSync`, a top-level
  await continuation) writes the status unlocked. Found by the R9-15 trace,
  not traced further.
- **GIL off, continuous collection with generational collection off.** The
  JSC stress suite's continuous-collection lanes (`--collectContinuously`,
  `--useGenerationalGC=false`, `--verifyGC` in the eager lanes) did not
  finish in the r9t GIL-off suite. Two causes, measured with a
  progress-instrumented `delete-property-inline-cache.js` (elapsed time every
  50 of its 1,000 iterations): this round's cycle-end sweep of weak-bearing
  blocks re-swept about 800 blocks at each of about 20,000 cycle ends a
  minute (50 iterations in 20 s, against 550 on the round's first build) -
  now bounded per stop (SPEC-heap history §35), and the same run completes in
  21.2 s; and, from before this round, every GIL-off cycle is a stop the
  mutator conducts at its next poll, so under continuous collection the
  mutator runs only between stops (flag off 3.7 s, where the cycles mark
  concurrently with it). Without the sweep (the round's first build) the heap
  at each cycle's start grew from 48 MB to 176 MB in three minutes, which that
  build's cycles then had to walk; without continuous collection the test
  ends in 1-2 s in every configuration with 0.5-0.6 MB after `fullGC()`. A
  testing option; if the remaining factor of about six matters, the conductor
  could let the mutator run a minimum stretch between continuous cycles.
  The r9u GIL-off suite finished every such configuration: the whole suite
  took about an hour, where r9t's took about four and a half.
- **Bun, flag off: `fetch-tcp-stress.test.ts` crosses its 30 s budget.** Its
  four cases (32,768 fetches in batches of 48 against a raw TCP server)
  time out on every branch build (r8, r9b, r9e, r9u) and pass on stock at
  low load. Not a hang and not a behaviour difference: with 300 s budgets
  every case passes on every build - the four together 125 s on stock, 144 s
  on r9b (the round's start) and 152 s on r9u, side by side at load 24 - and
  stock itself fails 2 and 3 of the four at the 30 s budget under that load.
  The branch builds are 15-22 % slower on this workload in Bun's debug
  build; stock sits at the budget (about 31 s a case at load 24). One case
  (`gently close`) under perf: 270.0 G instructions on r9u against 231.1 G
  on stock (+16.9 %), cycles +15.7 %. Bun's debug build is ASan and does not
  inline, and the symbols that grow or appear only on the branch are the
  mode gates: `JSC::addressOfJSCConfig()` 3.81 % of samples against 2.48 %,
  `WTF::addressOfWTFConfig()` 2.19 % against 1.57 %, `Options::useSharedGCHeap()`
  1.12 % and `VM::gilOffWithProcessGate()` 0.78 % (neither in stock's top
  list) - each a chain of calls there, a byte load in a Release build (the
  residual phase 3.1 measured). `Options::useSharedGCHeap()` is read on
  hot flag-off paths in `MarkedVector` (nine reads, the argument buffers of
  host calls) and `CompleteSubspace` (nine, the allocation slow path); a
  `g_jscConfig` byte there, as phase 3.1 did for the heap's other gates, is
  the next step. Left with this measurement: tightening those gates is a
  flag-off change that would need the whole battery again.
- **Weak-bearing blocks are swept inside the stop** (R9-18): at most 10
  blocks in a cycle on splay-like, 9 on map-heavy, and at most 32 in any
  cycle since the per-stop budget (r9u; a requested collection sweeps them
  all). The destructors and finalizers that run there may take the API lock
  (R9-23). A program that keeps many weak-bearing blocks unswept carries them
  over several cycles instead of growing one pause.
- **Continuous collection, GIL off, Debug: bring-up and teardown are
  load-sensitive.** `jsc -e ''` with `--collectContinuously=1` and the
  GIL-off options took 2-3 s at load 24, 90-108 s at load ~50 and over 300 s
  at load ~100 (Debug builds of the whole round alike). The runner no longer
  probes with the option and gives such tests three times the timeout; at
  load 35-40 the two tests that use it still exceed 400 s alone on the final
  Debug build, and equally on the r9h build whose Debug corpus had passed
  them - which this round first read as load. It was not: r9h is the first
  build with R9-18's cycle-end sweep, and every build compared had it; with
  the per-stop budget (SPEC-heap history §35) the test passes alone in 126 s
  on Debug r9u and 32 s on TSanJIT r9u, while r9t reaches 400 s beside it, and
  both tests pass in r9u's Debug and TSAN corpus lanes. Harness follow-up: the
  three-times timeout multiplies the TSAN lane's 900 s into 2,700 s per test;
  cap it (edit `run-tests.sh` only when no run is using it - bash reads a
  running script as it goes, and an edit during this round's r9r runs broke
  their final summaries).
  Suspected: the continuous collector's hand-offs with the mutator waiting out
  scheduler quanta when the CPUs are oversubscribed. A testing option; not
  traced.
- **Tier-up of one shared function under N threads**: string-heavy stays
  bimodal (the FTL loop-entry `Overflow` exit and the reoptimization back-off
  inflated by per-thread counted jettisons; the fix is policy, see the eighth
  round's text).
- **Eden pauses with N allocating threads** (each pause still stops every
  thread) and **scaling-suite memory at eight threads**: see PERF-RESULTS §2.

Out of scope this round, and what each would need:
- **bun:ffi on GIL-off spawned threads**: the FFI IC stub and its DFG/FTL
  paths are off GIL off (audit rows VM-8, OPT-1, OPT-2) and a spawned
  thread's call is refused. It would need the FFI function's IC to publish
  through the call-record protocol (SPEC-jit §5.8) instead of patching, TinyCC
  compilation and trampoline publication that are safe with other threads
  running, and a rule for which thread owns a callback's JS function.
- **WebAssembly GIL off**: Wasm is off when the GIL is off (GIL on, spawned threads are refused at the cold JS->wasm
  entry and the warm entry is handed out only until the first spawn; DFG/FTL `CallWasm` stays off flag on). It would need Wasm
  instances, memories and tables whose state other threads may observe, Wasm
  call sites and tier-up (BBQ -> OMG, OSR entry) published without patching
  live code, and the Wasm GC object model under the shared-heap rules.
- **Fuzzers**: none ran in the ninth or tenth round. The GIL-off fuzz harness
  (Tools/threads/fuzz) needs a campaign on the final tree with the
  amplifier's fixed reference check, and triage of what it finds.
- **arm64 and non-Linux**: nothing was built or run. arm64 needs a build, the
  corpus and TSAN there, and a review of the places that rely on address
  dependencies instead of fences (F2, the call-record fast path, butterfly
  word reads, and this round's unclaimed allocating transition, whose
  nuke-to-word fence matters there). macOS and Windows need the thread
  start/park and stop-the-world primitives checked on their platforms.

## Part 2: Performance

The goal, from `THREAD.md` and `BENCH.md`, is unchanged: about zero cost for
single-threaded code when the flag is off. The flag-on cost with no threads
running matters too, because that is what Bun users get once the flag ships.
Results as of the fifth round are in `PERF-RESULTS.md` (tables, commands,
causes, and what is left); the sections below are the plan they answer.

### 2.1 Baseline

Every comparison below is against `main` at the branch's base commit, built
with the same compiler and flags, on the same machine. Build both trees in
Release with LTO, the way Bun ships. Record the exact commands with the
results.

### 2.2 Benchmarks

| What | Tool | Compare |
|---|---|---|
| Microbenchmarks that stress the changed paths | `Tools/threads/bench-gate.sh` (see `BENCH.md`) | Record the baseline from `main`, then gate the branch, flag off. |
| Whole-engine benchmarks | JetStream 3 and the JSC microbenchmarks, via `Tools/Scripts/run-jsc-benchmarks` | `main`, branch with the flag off, branch with `--useJSThreads=1` |
| Bun | Bun's benchmark suite, and startup time (`bun -e 0`) | Bun built against `main`, and against the branch |
| Memory | Peak RSS on the above, and the size of `JSObject`, `Structure`, `Butterfly` and `VM` | `main` against the branch |
| Binary size | Size of `libJavaScriptCore.a` and of the Bun binary | `main` against the branch |
| Threads | `SCALEBENCH.md`, GIL on and GIL off | Only against the branch itself |

Run each benchmark enough times to report a median and a confidence interval.
Report a slowdown only when the interval does not include zero.

### 2.3 Find where the cost is

A benchmark shows that a slowdown exists. These show where it comes from.

- **Generated code.** Dump the LLInt (`offlineasm` output) and the Baseline,
  DFG and FTL code for a few hot functions, on `main` and on the branch, flag
  off, and diff them. Every flag-off difference needs a reason.
- **Flag checks on hot paths.** Count the `Options::useJSThreads()`,
  `vm.gilOff()` and `g_jscConfig.gilOffProcess` tests on paths that run per
  property access, per allocation, or per call. Each one is a load and a
  branch. Move it off the hot path or into a watchpoint where possible.
- **Atomics.** `JSCJSValue.h` keeps JSValue accesses plain outside TSAN, so
  loops still vectorize. Check that nothing else turned a hot plain access
  into an atomic one, and that no `seq_cst` access appeared on a flag-off path.
- **Layout.** Compare `sizeof` and field offsets of the hot classes. A field
  that moves out of the first cache line costs on every access.
- **Profiles.** Where a benchmark regresses, take a `perf` profile on both
  trees and compare the top functions.
- **Known so far.** With the flag on, JIT code caches no structure transition,
  so every property add runs in C++ ("Open items"). Start there.

Write the results in a new `PERF-RESULTS.md`, with the commands, the numbers,
and one line of explanation for each difference.

## Order of work

1. Fix the failures that are clearly bugs (1.1). They may hide others.
2. Record the performance baseline and run the bench gate (2.1, 2.2). A design
   change forced by a regression is cheaper to make early.
3. Run the full suites in all three modes (1.2).
4. Do the upstream audit (1.3) and the async generator review (1.4).
5. Run TSAN, the amplifier, the fuzzers, and the other platforms (1.2).
6. Run Bun's tests and benchmarks against the branch (1.2, 2.2).

## Exit criteria

The branch is ready to land when all of these hold:

- `JSTests/threads` and the CVE suite pass in Debug and Release, GIL on and
  GIL off, on Linux x86-64 and on one arm64 platform.
- The JSC test suites give the same results as `main` with the flag off, and
  no new failures with the flag on.
- TSAN reports nothing on `JSTests/threads` that `TSAN-TRIAGE.md` does not
  explain.
- Bun's test suite gives the same results as with `main`.
- No benchmark in 2.2 is slower than `main` with the flag off, beyond noise.
- Every flag-off difference in generated code has a written reason.
