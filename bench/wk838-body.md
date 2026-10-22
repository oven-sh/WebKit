Related to oven-sh/bun#44893. The first commit is #800.

### Problem
- An LRU cache collection marks 1 to 10 MB with about 220 thread wake-ups. One marker is twice as fast. Each donation (`SlotVisitor::donateKnownParallel`) wakes every parked helper under `m_markingMutex`. #800 covers collections under 1 MB.
- With #800 a long list takes 120 ms, not 54: `drain` reads the clock per cell.

### Fix
- Marking pays for wake-ups: one per 2 MB a visitor visits (`minimumBytesMarkedForParallelMarking`). A donation wakes what is paid for, with the lock released.
- No helper parks. An idle one polls for 50 µs, then leaves. `ParallelHelperClient::requestThread` brings one back.
- A cycle stays in its pause until the first payment. `drain` reads the clock once per 100 cells. Option 0 and `collectContinuously` keep the upstream protocol.
- Verified: the issue's program (60,056 to 821 context switches, 2.53 to 0.15 s system CPU), JSTests, Bun's GC tests.

### Background
- Helpers are `ParallelHelperPool` threads that steal from shared mark stacks. On main all join each cycle and park when idle.
- Considered a higher #800 threshold: wide 10 MB collections then mark on one thread.

### Downsides
- Peak RSS grows 10 to 20% in a GC-bound churn (798 MB against 706). The JS thread no longer waits for the marking lock and allocates 1.5 times faster during a cycle.
- Wide collections get fewer helpers: 1 of 7 at 3 MB marked, 6 at 54 MB.
- Measured on one busy KVM guest. With cheap futex wakes, helpers that start later can be a net loss.

<details><summary>Notes</summary>

**Two commits.** The first one is #800, unchanged. The second one is this change. It replaces most of what #800 adds to the Collector (the start of all helpers at 1 MB, the hook in the stochastic scheduler). It keeps the option, the GC log format and the tests. The branch is on `d6375c72`, the commit that Bun pins: Bun's CI does not build a branch that is far behind its main. If #800 merges first, a rebase drops the first commit.

**How a wake-up is paid for.**
- `SlotVisitor::drain` checks once per 100 cells if the visitor has visited 2 MB more (`payForMarkerWakeupIfNeeded`). If so, `Collector::didPayForMarkerWakeup` adds one to `m_numberOfMarkerWakeupsPaidFor`.
- `Collector::choosePaidForMarkersToWake` runs with `m_markingMutex` held. It gives the markers that are paid for and not woken yet. It gives no more markers than there are shared cells, and it subtracts the markers that poll the shared stacks or are on their way. `Collector::wakeMarkers` makes the system calls after the caller has released the lock.
- Its callers: a donation (`donateKnownParallelSlow`), a payment while cells are shared, the resume of the mutator, and a task of the constraint solver (`runTaskInParallel`).
- A marker that was woken pays after an eighth of the price (`didWakeUpForMarking`). So a graph that gives each new marker work has all markers after about 2 MB more. A linked list, where a woken marker finds a cell or two, gets one wake-up per 2 MB.
- One wake-up needs no payment. A thread that stops marking while the mutator runs wakes one marker if none is at work or on its way (`chooseMarkerToWakeIfNoneIsMarking`). The callers are the resume of the mutator, `donateAll` and `drainInParallelPassively`.

**The option at 0, and `collectContinuously`,** keep the upstream protocol (`Collector::helpersJoinEveryCycle`): the helpers join every cycle at its start, park in it, and every donation wakes all of them. `SlotVisitor::drainFromShared` keeps its upstream body for that, and calls `drainFromSharedWithHelpersOnRequest` otherwise.
- The option at 0 is the way back if this change hurts a workload. Bun's tests for marking races use it, to have a small heap marked by several threads at once. An earlier revision let 0 mean that no wake-up needs a payment. A 100 MB tree then took 309 ms to collect, against 176 on main: each helper that ran dry left and was asked for again.
- `collectContinuously` is there to have marking threads overlap the mutator. With the new protocol a collection of a small heap is one pause, also in that mode: 1.13 stops per collection against 1.85 on main in `stress/string-regexp-methods-cross-realm-dfg.js`. That takes the overlap away from the collect-continuously mode of JSTests and from the 24 test files of Bun that use the option. So that mode keeps the protocol it had.

**A small collection** (`Collector::isSmallSoFar`). Until the first payment or the first resume, the conductor's visitor shares nothing, and `runFixpointPhase` keeps the mutator stopped: each time the scheduler wants to resume, the cycle marks for `minimumGCPauseMS` more. #800 did this in the stochastic scheduler only. Here every scheduler gets it. It is off with one marker, with the upstream protocol, and with `minimumGCPauseMS=0` (there the cycle would not mark at all, `stress/array-unshift-moved-cell-barrier.js` found it).

**An idle helper.** `drainFromShared(HelperDrain)` does not wait on `m_markingConditionVariable`. A helper with no shared cell and no task looks at the shared stacks without the lock for up to `gcMarkerPollMS` (50 µs) if another marker is at work (`pollSharedMarkStacks`). Then it leaves `runMarkingHelper`. It polls again only after it has marked something. The thread in `MainDrain` polls the same way before it parks. So a `notifyAll` on that condition wakes the conductor at most. The poll reads `MarkStackArray::size()`, as `donateKnownParallel` already does without the lock. It must not read `isEmpty()`, which follows the head segment.

**The pool.** `ParallelHelperClient::setTaskOnRequest` sets a task that no pool thread runs until `requestThread` asks for one. The Collector sets it at the first request of a cycle. A collection that asks for nobody does not take the pool's lock, and `runEndPhase` calls `finish()` only if the task was set.

**Cost for a collection that never shares.** Disassembly of `SlotVisitor::drain` in LTO builds of Bun, main against this PR: the loop that visits one cell has the same 55 instructions. The two lock-free checks of `donateKnownParallel` are inline in `drain` in both (12 and 5 to 6 instructions). Per round of up to 100 cells the PR adds 15 instructions after the loop and 8 for the payment check (measured with `618674ce`; the head checks one value less there). The text of the Bun binary grows by 4,416 bytes (106,754,509 to 106,758,925). `SlotVisitor` grows by 16 bytes, `Collector` by about 32, `ParallelHelperClient` by 8 (by reading, not measured).

**Measurements.** LTO builds of Bun (`590035462`) with WebKit `87ee10da` (main on Oct 16), the preview of #800 (`2cacac75`) and an earlier head of this PR (`618674ce`). Since that head the branch was rebased, the option at 0 and `collectContinuously` got the upstream protocol, and the payment check lost a load. The new protocol is the same code. Linux x64 KVM guest, 12 vCPUs, 8 GC markers. Fresh processes, arms interleaved, medians. The host was busy: in a sample, steal time was 2.5 to 4 times the CPU time that the benchmark got. So the context switch counts and the system CPU time are the signals to trust. The wall-clock numbers of the arms with 8 markers move by up to 2 times between sessions.

The program of oven-sh/bun#44893 (`lru-gc.mjs 500`, 4,096 entries), 7 runs:

| | ns/op | system CPU | voluntary context switches |
|---|---|---|---|
| main | 162.9 | 2.53 s | 60,056 |
| #800 | 163.0 | 2.28 s | 56,665 |
| this PR | 66.8 | 0.15 s | 821 |
| this PR, one marker | 74.2 | 0.09 s | 107 |
| main, one marker | 79.2 | 0.06 s | 107 |
| Node 26.3 | 75.6 | 0.18 s | 741 |

The same program with a larger cache, 5 runs, ns/op (voluntary context switches):

| entries | main | this PR | one marker | Node |
|---|---|---|---|---|
| 16,384 | 220.8 (49,499) | 128.0 (1,611) | 111.8 (190) | 127.3 |
| 65,536 | 335.4 (26,253) | 269.5 (1,478) | 247.8 (605) | 305.6 |
| 131,072 | 437.2 (24,274) | 385.5 (1,452) | 368.4 (776) | 474.7 |

Peak RSS for these three sizes: main 85, 169 and 187 MB. This PR 92, 166 and 184 MB. One marker 103, 259 and 370 MB.

Churn of JSON-like records in a sliding window (`bench-app.mjs`), 7 runs, ns per record (voluntary context switches) [peak RSS in MB]:

| live | main | #800 | this PR | one marker | Node |
|---|---|---|---|---|---|
| 3 MB | 2061 (41,767) [64] | 1982 (39,604) [64] | 869 (1,176) [63] | 774 (94) [66] | 689 (695) [124] |
| 25 MB | 1246 (12,095) [202] | 1313 (11,373) [212] | 929 (1,454) [220] | 851 (358) [213] | 1961 (1,998) [304] |
| 100 MB | 1215 (6,752) [586] | 1075 (6,336) [591] | 909 (2,163) [647] | 990 (1,064) [609] | 2244 (1,796) [668] |

**Large collections.** The same churn with 125 MB live, where a collection marks 115 to 190 MB. 9 runs with `logGC`, median (range):

| | main | #800 | this PR | one marker |
|---|---|---|---|---|
| peak RSS, MB | 678 (612 to 767) | 683 (619 to 736) | 814 (758 to 855) | 742 (697 to 751) |
| peak RSS without `logGC`, MB | 706 (651 to 851) | 721 (683 to 783) | 798 (730 to 817) | 745 (721 to 762) |
| ns per record | 1193 | 1199 | 1084 | 1110 |
| voluntary context switches | 7,408 | 6,886 | 2,717 | 1,296 |
| total pause, ms | 625 | 548 | 552 | 1259 |
| longest pause, ms | 123 (53 to 227) | 60 (50 to 150) | 134 (93 to 153) | 223 |
| bytes marked while the mutator is stopped | 22% | 16% | 37% | 99% |
| collections that use up their headroom | 27% | 30% | 78% | 100% |
| MB marked per ms while the mutator runs | 1.23 | 1.28 | 1.10 | 0.03 |
| MB allocated per ms while a cycle marks | 0.40 | 0.37 | 0.62 | 1.75 |

The helpers mark at about the same rate in the three arms with 8 markers. The program allocates 1.5 times faster while they do, so a cycle more often ends with the headroom used up and a longer last pause. That is the RSS. The cause is in 400 gdb samples of the JS thread per arm. On main 32 of them are an allocation slow path that waits for `m_markingMutex` (`Collector::runConcurrentPhase` calls `SlotVisitor::didReachTermination`). With this PR none is. On main the helpers hold that lock while they wake each other. In pauses, the JS thread is in a lock or in a wake-up call in 55 samples on main and in 13 with this PR, 12 of them in `requestThread`. The total pause time is not longer.

Eden batches of arrays of small objects (a wide graph), 7 runs, pause per MB marked and total pause over the run:

| batch | marked per collection | main | #800 | this PR | one marker |
|---|---|---|---|---|---|
| 2 MB | 3 MB | 6.43 ms/MB, 311 ms | 6.22, 296 ms | 1.60, 85 ms | 2.05, 104 ms |
| 8 MB | 17 MB | 2.43 ms/MB, 618 ms | 2.00, 471 ms | 1.12, 267 ms | 1.57, 349 ms |
| 32 MB | 54 MB | 1.09 ms/MB, 1256 ms | 1.05, 1214 ms | 0.73, 847 ms | 1.02, 1095 ms |

Helpers that marked something, of 7, median per collection: main 4, 7 and 7. This PR 1, 4 and 6. The helpers mark 23%, 68% and 83% of the bytes, against 92 to 97% on main. A second set of 9 runs of the 2 MB batches gave 4.64 ms/MB on main, 2.83 with this PR and 2.73 with one marker.

Full collections of 100 MB heaps (`Bun.gc(true)`, median of 7 in a process, 9 processes), ms. The first set is from this session. The second set is from a session with a quieter host and the commit before the last one, which differs in inlining only:

| shape | main | #800 | this PR | one marker | main, quiet | #800, quiet | this PR, quiet | one marker, quiet |
|---|---|---|---|---|---|---|---|---|
| balanced tree | 176.4 | 125.3 | 109.7 | 107.7 | 78.4 | 52.8 | 42.4 | 105.1 |
| records | 110.1 | 101.3 | 73.3 | 97.4 | 30.5 | 25.7 | 31.3 | 94.9 |
| Map | 91.1 | 91.5 | 82.7 | 81.6 | 51.5 | 55.6 | 53.0 | 78.6 |
| wide array | 50.2 | 44.1 | 40.2 | 58.3 | 24.5 | 27.0 | 21.7 | 57.1 |
| linked list | 89.3 | 201.8 | 87.3 | 85.5 | 86.9 | 193.4 | 86.6 | 84.4 |
| linked list, 2 million nodes | 54.4 | 120.1 | 52.5 | | 52.9 | 116.9 | 52.8 | |

Four workers that each run the program of the issue: 412.6 ns/op and 6,500 context switches on main, 258.3 and 652 with this PR, 266.7 and 384 with one marker. With 12 workers on the 12 vCPUs the ranges of the three arms overlap (343 to 1781 ns/op). Eden collections with much weak work for the constraint solver, total pause: 143,000 `EventTarget`s per batch, 1058 ms on main, 818 with this PR, 890 with one marker. 200,000 old `WeakMap`s, 574, 402 and 446 ms.

**Why 2 MB.** The price on this build, 5 to 7 runs. The host was busier than for the tables above:

| price | issue's program, ns/op (context switches) | 16,384 entries, ns/op | wide eden, 8 MB batches, ms/MB | wide eden, 32 MB batches, ms/MB |
|---|---|---|---|---|
| 512 KB | 96.5 (5,251) | 141.3 | 1.55 | 0.86 |
| 1 MB | 81.2 (2,563) | 173.2 | 1.40 | 0.65 |
| 2 MB | 73.6 (945) | 142.2 | 1.44 | 0.77 |
| 4 MB | 65.2 (312) | 119.3 | 1.84 | 1.02 |
| 8 MB | 70.1 (94) | 111.4 | 2.71 | 1.23 |
| one marker | 88.5 (120) | 123.0 | 1.30 | 1.16 |

The linked programs get faster up to 4 MB. The wide collections get slower above 2 MB. The peak RSS of the large churn is 798 to 835 MB at each of these prices, so the price does not cause it.

**`collectContinuously` with the new protocol** (the earlier head `618674ce`, where that mode did not select the upstream protocol). On a release `jsc` three sampled stress tests ran 4 to 7 times faster in that mode (0.6 s against 3.7 to 5.2 s for `has-own-property-name-cache-symbol-keys.js`). On a Debug+ASAN `jsc` with `verifyGC`, where each collection has a fixed pause, `string-regexp-methods-cross-realm-dfg.js` took 388 to 415 s against 279 to 320 s on main: 2,756 collections in 100 s against 1,527.

**Where the wake-ups were.** Counters in a build of #800, per collection of the issue's program: 0.9 starts of all helpers, 3.3 constraint solver tasks (13.8 sleepers woken), 32.8 donations (146.5 sleepers woken), 1.4 `donateAll` (7.3), 6.9 termination notices from helpers (41.9), 1 end of cycle (6.1). A wake-up of a thread that sleeps in a futex costs the waker 44 µs on this guest (median of 2000).

**The two open findings on #800.** A chain-shaped graph never reaches the threshold check of `donateKnownParallel`, because the check is behind `from.size() < 2`. Here the payment check is in `drain`, so a visitor with nothing to share still ends the small phase of the collection. An eden collection that visits old cells again (the remembered set) counted none of them. `SlotVisitor::bytesVisited()` adds the smallest cell size for each cell that `drain` takes from the mutator's mark stack.

**Thread safety.** The lock order is `m_markingMutex`, then the pool's lock (`setTaskOnRequest` in `chooseMarkersToWake`). `wakeMarkers` takes the pool's lock alone. `m_numberOfActiveParallelMarkers` is atomic: the poll reads it without the lock, and every write is under the lock. `finish()` clears the task and its open requests and waits for the threads that run it, so no helper runs across the end of a cycle. A thread that was asked for and comes after the cycle ended finds no task to claim. If `requestThread` returns false, `wakeMarkers` takes the request back from `m_numberOfRequestedParallelMarkers`. #824 makes `AutomaticThreadCondition::notifyOne` report a thread that did not start. `requestThread` can return that when both changes are in.

**Tests.**
- CI: every lane builds, and the JSTests jobs pass on Linux x64 and arm64 (the earlier head `618674ce`; the run for the current head has not finished).
- Locally, every `JSTests/stress` test with the release `jsc`, in the default mode and with `collectContinuously`: no timeout that main does not have.
- The Debug+ASAN `jsc` of `618674ce` at 8 markers: every 40th `JSTests/stress` test (151 tests) in three modes. The modes are `collectContinuously` with `verifyGC` and no generational GC, `collectContinuously` with `verifyGC` and the option at 0, and the defaults. No assertion and no ASAN report. 11 of the 453 runs reach the limit of 180 s: 2 tests loop without their `--watchdog` option, and 4 tests take 2 to 10 minutes in these modes on main too.
- The same `jsc` on an LRU program and a wide program with 14 sets of options (1, 2 and 8 markers, the option at 0 and at 64 KB, `collectContinuously` with `verifyGC`, no concurrent GC, the space-time scheduler, `gcIncrementScale=1`, `minimumGCPauseMS=0`, `gcMarkerPollMS=0`, `slowPathAllocsBetweenGCs`): no assertion and no ASAN report.
- Bun with this build: `test/js/bun/gc/gc-controller-cadence.test.ts` (the three tests of oven-sh/bun#45568 and two new ones, which fail on main), `abort-controller-gc-reason.test.ts`, `broadcast-channel-worker-gc.test.ts`, `dynamic-import-evaluation-error-gc.test.ts`, `module-children-concurrent-gc.test.ts`.

**Not done.**
- No measurement on macOS, on Windows, or on a machine without a hypervisor. The CI of this repository builds those platforms and runs JSTests on Linux only.
- The design asked for the RSS of the large churn to stay where it is on main. It does not (table above).

</details>

