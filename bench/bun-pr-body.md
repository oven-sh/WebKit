Fixes #44893

### Problem
- A single-threaded LRU churn (a `Map` over a linked list) makes 60,056 voluntary context switches and spends 2.53 s in the kernel, at 163 ns/op. With one GC marker: 107 switches, 0.06 s, 79 ns/op. Node: 76 ns/op.
- Its collections mark 1 to 10 MB. Each time a marking thread shares a cell (`SlotVisitor::donateKnownParallel`), it wakes every parked helper thread under the marking lock. #45568 covers collections under 1 MB.

### Fix
- oven-sh/WebKit#838: marking pays for each helper wake-up (2 MB visited for one), and an idle helper leaves the collection. This PR pins it.
- Result: 821 switches, 0.15 s, 67 ns/op.
- Verified: `test/js/bun/gc/gc-controller-cadence.test.ts`, two new tests. On the current pin they fail: 78 to 94 and 118 to 148 context switches per collection, limits 20 and 50. Also the GC race tests.

### Background
- JSC marks with one collector visitor and up to 7 helper threads that take cells from shared mark stacks. `BUN_JSC_logGC=1` now prints `mw=`, the wake-ups of a collection. The tests read it.
- Considered a higher threshold in #45568: wide 10 MB collections then mark on one thread.

### Downsides
- Peak RSS grows 10 to 20% in a GC-bound churn (798 MB against 706, 125 MB live). The JS thread allocates 1.5 times faster while a cycle marks.
- Wide collections get fewer helpers: 1 of 7 at 3 MB marked, 6 at 54 MB.
- Measured on one busy KVM guest. `BUN_JSC_minimumBytesMarkedForParallelMarking=0` restores the old protocol. Do not merge before oven-sh/WebKit#838: the pin is its preview build.

<details><summary>Notes</summary>

**This branch contains #45568.** Its three test commits are the first three commits here, unchanged. The CI does not build a branch that is far behind main, and #45568 is. In the same way the first commit of oven-sh/WebKit#838 is oven-sh/WebKit#800. So this PR also has the change for #44797. When oven-sh/WebKit#838 merges, the pin moves to the merged sha.

**The gate cannot show the tests failing.** The change is in the WebKit pin (`scripts/build/deps/webkit.ts`), which the gate keeps, and a debug build skips these tests. With a release build of the current pin, 4 of the 5 `marking helper threads` tests fail (3 of them are from #45568).

**Measurements.** LTO builds of Bun (`590035462`) with WebKit `87ee10da` (its pin), the preview of oven-sh/WebKit#800 and the preview of an earlier head of oven-sh/WebKit#838 (`618674ce`, the same protocol as the pinned head). Linux x64 KVM guest with a busy host, 12 vCPUs, 8 GC markers. Fresh processes, arms interleaved, medians. The context switch counts and the system CPU time are the signals to trust. The wall-clock numbers with 8 markers move by up to 2 times between sessions.

The program of the issue (`lru-gc.mjs 500`), 7 runs:

| | ns/op | system CPU | voluntary context switches |
|---|---|---|---|
| current pin | 162.9 | 2.53 s | 60,056 |
| #45568 | 163.0 | 2.28 s | 56,665 |
| this PR | 66.8 | 0.15 s | 821 |
| this PR, one marker | 74.2 | 0.09 s | 107 |
| current pin, one marker | 79.2 | 0.06 s | 107 |
| Node 26.3 | 75.6 | 0.18 s | 741 |

The same program with a larger cache, 5 runs, ns/op (voluntary context switches):

| entries | current pin | this PR | one marker | Node |
|---|---|---|---|---|
| 16,384 | 220.8 (49,499) | 128.0 (1,611) | 111.8 (190) | 127.3 |
| 65,536 | 335.4 (26,253) | 269.5 (1,478) | 247.8 (605) | 305.6 |
| 131,072 | 437.2 (24,274) | 385.5 (1,452) | 368.4 (776) | 474.7 |

Churn of JSON-like records in a sliding window, 7 runs, ns per record (voluntary context switches) [peak RSS in MB]:

| live | current pin | #45568 | this PR | one marker | Node |
|---|---|---|---|---|---|
| 3 MB | 2061 (41,767) [64] | 1982 (39,604) [64] | 869 (1,176) [63] | 774 (94) [66] | 689 (695) [124] |
| 25 MB | 1246 (12,095) [202] | 1313 (11,373) [212] | 929 (1,454) [220] | 851 (358) [213] | 1961 (1,998) [304] |
| 100 MB | 1215 (6,752) [586] | 1075 (6,336) [591] | 909 (2,163) [647] | 990 (1,064) [609] | 2244 (1,796) [668] |

The same churn with 125 MB live, 7 runs: peak RSS 706 MB (651 to 851) on the current pin, 798 MB (730 to 817) with this PR, 745 MB with one marker. The total pause time is not longer (552 ms against 625 on the current pin). The WebKit pull request has the analysis, the pauses of wide eden collections, full collections of 100 MB heaps of five shapes, workers, and what was not measured.

**The tests.** Two new tests run the LRU program at 4,096 and at 32,768 entries with 8 markers. They bound the `mw=` count of the GC log (two wake-ups per MB marked) and, on Linux, the voluntary context switches per collection. They do not run on an ASAN build, which takes several times longer for the same program. Context switches per collection in 6 runs: 2 to 4 and 11 to 17 with the PR, 83 to 108 and 159 to 254 on the current pin.

**Tests that set the option to 0.** A collection that marks under 2 MB is one pause on the JS thread now. A test that needs a marking thread that is not the JS thread, or a collection that ends at a later safepoint than it began, does not get that from a small heap any more. With the option at 0 the collector runs the upstream protocol, where every helper joins every collection at its start. These tests set it: the three that force 8 markers (since #45568), `dynamic-import-evaluation-error-gc.test.ts` and two tests in `sourcetextmodule-link-gc.test.ts`. `BUN_JSC_collectContinuously` selects the upstream protocol by itself, so the 24 test files that use it run as before. `bun -e` runs with one marker, so a test that spawns it has no helper thread before or after. A test that runs its collections in the test process cannot set the option.

**Not done.** No measurement on macOS, on Windows, or on a machine without a hypervisor.

</details>
