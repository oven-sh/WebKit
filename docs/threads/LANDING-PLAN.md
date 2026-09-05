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
rebased onto `main` at `491b5cc236e9`, plus the review fixes from
oven-sh/WebKit#549 and the fixes the rebase needed.

The table below is the state before the safety round of 2026-09-02. For the
state after it, see "Results (2026-09-02)", "Results, second round" and
"Results, third round" at the end of Part 1.

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

### Open items

Work that is not done, after the third round. Each item says why. The items
that the rounds closed are in their "Results" sections.

- **Bun needs five changes to build and run against this branch.** Two to
  build: `RegExp::ovectorSpan` takes a `VM&`, and `TopExceptionScope` is 72
  bytes in Debug (two pointers for the exception scope verification). Three
  for the GIL-off protocol: the heap walks in `JSEnvironmentVariableMap.cpp`,
  `BunDebugger.cpp` and `JSInspectorProfiler.cpp` go through
  `Heap::runWithOtherClientsStopped`, and `NodeVMRunTermination.cpp` makes an
  enclosing run's request again with `notifyNeedTerminationForCurrentThread()`.
  They are in Bun's tree, not here.
- **Bun's error-stack hooks run on any thread, GIL off.** JSC calls Bun's
  `computeErrorInfo` hooks from whichever thread first reads an error's
  `stack`; after the third round that is serialized per error but concurrent
  across errors. Whether Bun's side of those hooks (its source-map cache in
  particular) tolerates that has not been checked. A Bun item.
- **Bun's accept loop has no bound** (`us_internal_dispatch_ready_poll`). A
  server whose clients reconnect as fast as it accepts never returns to its
  event loop. `worker-terminate-lifetime.test.ts` stops there with the flag
  on, and it stops flag off too when the handler takes 3 ms. A bound of 64
  accepts per readiness event fixes the test. This is a Bun change.
- **`shift` and `unshift` copy the whole ArrayStorage on every call with the
  flag on**, so queue-draining loops are quadratic (37 s against 10 ms for
  200,000 elements). See "Found on the way" in the third round. Part 2, and
  the first thing to fix there.
- **Property adds are not cached with the flag on.** JIT code performs no
  structure transition, so each add runs in C++. A loop that creates objects
  with six properties runs about 40 times slower in Release. See Part 2.
- **A `gc()` keeps the caller's own newest garbage while another thread is
  attached.** The window-liveness constraint (`Wlr`, `Heap::addCoreConstraints`)
  retains the newly allocated cells of every attached client's active blocks,
  and the conducting thread's blocks are among them, although its stack and
  registers are scanned as in the single-mutator protocol (the argument the
  single-client gate in that constraint already makes). Bounded (one block per
  allocator per thread, freed by a later cycle), but it surprised a test (third
  round, `mc-dos-waiter-table-storm.js`). Decide in Part 2 whether the conductor's
  own blocks can be left to the ordinary scan.
- **`bun:ffi` with the GIL off** stays refused on spawned threads. That is
  the decision for landing. The full fix is described in section 1.3.
- **Lazily created state, GIL off.** The third round fixed four first-use
  races (a function's `prototype`, an error's stack strings, a provider's
  stripped URL, a code block's line/column cache) and read the neighbours it
  could name (`SourceProvider::getID()`, a function's lazy `length` and `name`,
  `LazyProperty`, property tables; see the audit's PRE-10). Any other cache that
  a cell or a runtime object fills on first use with a plain check-then-store
  has the same shape; no complete search was made.
- **The trap check under a cell lock is asserted, not proven absent.** A search
  found one (`ThreadAtomics.cpp`, fixed) and `VMTraps::handleTraps` asserts in
  Debug that no cell lock is held, so the Debug corpus catches a new one only
  where a test makes a trap pending inside the locked region.
- **Paths with no test in the jsc shell.** The debugger's carrier checks in
  `Debugger.cpp`, `queryHolders`, and Bun's module-info string lookup. Each
  fix was made by reading.
- **WebAssembly** is off with the GIL off. Two notes for the day it is on:
  `memory.atomic.wait` does not enter a `GILDroppedSection`, and the IPInt
  writes the VM-level top call frame directly.
- **Fuzzers.** Fuzzilli and its Swift toolchain are not installed on the
  machine used for this work, so no campaign ran.
- **Other platforms.** Everything here ran on Linux x86-64 only. The
  address-dependency arguments in this document (the property table's vector
  word, the structure's table pointer) hold on arm64 too, but nothing has run
  there. Two things from the third round are x86-64-only as written: the TSAN
  build's DCAS (the audit's PRE-12; an arm64 TSAN build still goes through
  TSAN's emulated 16-byte atomic), and the base-before-length order that the
  typed-array C++ paths rely on against a concurrent detach (PRE-13), which on
  arm64 wants a load-load fence in `JSArrayBufferView::vector()`.

## Part 2: Performance

The goal, from `THREAD.md` and `BENCH.md`, is unchanged: about zero cost for
single-threaded code when the flag is off. The flag-on cost with no threads
running matters too, because that is what Bun users get once the flag ships.

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
