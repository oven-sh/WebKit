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
state after it, see "Results (2026-09-02)" at the end of Part 1.

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


### Open items

Safety work that is not done. Each item says why.

- **A typed array store can write through a null base, GIL off.** This blocks
  landing. `JSArrayBufferView::detachFromArrayBuffer` sets the length to 0 and
  then the base (`m_vector`) to null. JIT code on another thread can load the
  old length and the new base, and store to `null + index`. With the
  Gigacage off, that is a SIGSEGV: `cve/mc-grow-buffer-storm.js` crashes in 1
  of 48 Release runs and 1 of 16 Debug runs with `GIGACAGE_ENABLED=0`, and in
  every TSAN run (the TSAN build uses the system malloc, so it has no
  Gigacage). The faulting store had base 0, length 4096, index 4095. With the
  Gigacage on, which is the default, the base is caged to the start of the
  primitive cage, so the same store probably writes into another buffer's
  memory and nothing crashes. The design already covers this
  (`ArrayBuffer.cpp`, "per-server ArrayBuffer mapping quarantine"): a base
  that a reader can see must stay mapped until a stop. `ArrayBuffer` follows
  it, and the views do not. The fix is to keep the view's base until the
  stop, GIL off. Then a null base no longer means detached, which
  `JSArrayBufferView::isDetached()` and the JIT paths that test the base for
  null rely on. That is why it is not in this round.
- **The inspector's `HeapHolderFinder`** (`JSInjectedScriptHost.cpp`) installs
  a heap analyzer and collects with no `PreventCollectionScope`. With the GIL
  off, other threads can start cycles while it is installed, so it can see
  more than one cycle, which is the `w16-c1` bug (section 1.1) without the
  gate. Not fixed.
- **`bun:ffi` with the GIL off** is refused on spawned threads (section 1.3).
  Decide whether that limit is acceptable, or do the full fix.
- **Bun does not build against this branch as it is.** Two Bun changes are
  needed: `RegExp::ovectorSpan` takes a `VM&`, and `TopExceptionScope` is 72
  bytes in Debug (two pointers for the exception scope verification).
- **A Bun worker test stops making progress with the flag on**
  (`worker-terminate-lifetime.test.ts`, see the Bun results). Not
  root-caused.
- **Bun's own heap walks.** `JSEnvironmentVariableMap.cpp`, `BunDebugger.cpp`
  and `JSInspectorProfiler.cpp` use `HeapIterationScope` directly. With the GIL
  off, they must go through `Heap::runWithOtherClientsStopped`, as the JSC
  callers do now.
- **Bun's `JSC__IdentifierArray__setFromSlot`** (audit row VM-12) reads the
  cached-bytecode string tables. Not traced.
- **A `node:vm` timeout with the GIL off** terminates every thread, not only
  the one that timed out (audit row VM-9).
- **The DFG element-write gap** (audit row OM-9).
- **Two more `topCallFrame` readers, GIL off.** `Debugger.cpp` reads the VM's
  `topCallFrame` in `breakProgram` and `topEntryFrame` at four sites, and the
  collector passes the VM's `topCallFrame` to `ShadowChicken::update`
  (`Heap.cpp`). With the GIL off those fields are not written. The debugger
  has its own carrier protocol, and the collector thread has no lite, so each
  needs a decision about which thread's frame to use. No test reaches them.
- **One TSAN group left as it is.** `ArrayStorage::m_numValuesInVector`, which
  `putByIndexBeyondVectorLength` reads with no lock (two sites) as a density
  heuristic; the locked path rebuilds the count. Reading it under the locks
  its writers hold needs two more locks on that path, which I did not trace.
- **Plain double accesses that TSAN has not reported yet.** Six sites in
  `JSObject.cpp` read or write a raw double slot with a plain access. Each
  one is fine if the storage is not shared; I did not check which are.
- **JIT-emitted structure transitions** (DFG and FTL `PutStructure` and
  `MultiPutByOffset`, and the LLInt and baseline `put_by_id` transitions) were
  not in the structure-store audit. The C++ transitions were.
- **WebAssembly** is off with the GIL off. Two notes for the day it is on:
  `memory.atomic.wait` does not enter a `GILDroppedSection`, and the IPInt
  writes the VM-level top call frame directly.
- **Fuzzers.** Fuzzilli and its Swift toolchain are not installed on the
  machine used for this work, so no campaign ran.
- **Other platforms.** Everything here ran on Linux x86-64 only.

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
