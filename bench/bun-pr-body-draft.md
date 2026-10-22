Fixes #44893

### Problem
- A single-threaded LRU churn (a `Map` over a linked list, 4,096 live nodes) makes 60,056 voluntary context switches and spends 2.53 s in the kernel, at 163 ns/op. With `BUN_JSC_numberOfGCMarkers=1`: 107 switches, 0.06 s, 79 ns/op. Node: 76 ns/op.
- A collection of it marks 1 to 10 MB. Each time a marking thread shares a cell or two (`SlotVisitor::donateKnownParallel`), it wakes every parked helper thread while it holds the marking lock. #45568 covers collections under 1 MB only.

### Fix
- oven-sh/WebKit#838: marking pays for each wake-up of a helper (2 MB visited for one), a shared cell wakes only what is paid for, and an idle helper leaves the collection. This PR pins that build.
- Result: 821 switches, 0.15 s, 67 ns/op.
- Verified: `test/js/bun/gc/gc-controller-cadence.test.ts`, two new tests. On the current pin they fail with SWITCHES_MAIN context switches per collection against limits of 30 and 300. Also the GC race tests.

### Background
- JSC marks with one collector visitor and up to 7 helper threads that take cells from shared mark stacks. `BUN_JSC_logGC=1` now prints `mw=`, the wake-ups of a collection. The tests read it.
- Considered a higher threshold in #45568: a wide 10 MB collection then marks on one thread.

### Downsides
- Peak RSS of a GC-bound churn grows 10 to 20%: 798 MB against 706 with 125 MB live. The JS thread allocates 1.5 times faster while a cycle marks.
- A wide collection gets fewer helpers: 1 of 7 at 3 MB marked, 6 at 54 MB.
- Measured on one Linux KVM guest. Stacked on #45568. Do not merge before oven-sh/WebKit#838: the pin is its preview build.

<details><summary>Notes</summary>

**The base branch** is the branch of #45568, so the diff here is the pin and the tests. The WebKit pull request is stacked on oven-sh/WebKit#800 in the same way. When oven-sh/WebKit#838 merges, the pin moves to the merged sha.

**The gate cannot show the tests failing.** The change is in the WebKit pin (`scripts/build/deps/webkit.ts`), which the gate keeps, and a debug build skips these tests. With a release build of the current pin, 4 of the 5 `marking helper threads` tests fail.

**Measurements.** LTO builds of Bun (`590035462`) with WebKit main (`87ee10da`), the preview of oven-sh/WebKit#800 and the preview of oven-sh/WebKit#838. Linux x64 KVM guest with a busy host, 12 vCPUs, 8 GC markers. Fresh processes, arms interleaved, medians. The context switch counts and the system CPU time are the signals to trust. The wall-clock numbers with 8 markers move by up to 2 times between sessions.

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

The same churn with 125 MB live, 7 runs: peak RSS 706 MB (651 to 851) on the current pin, 798 MB (730 to 817) with this PR, 745 MB with one marker. The total pause time is not longer (625 ms against 552). The WebKit pull request has the analysis, the pauses of wide eden collections, full collections of 100 MB heaps of five shapes, workers, and what was not measured.

**The tests.** Two new tests run the LRU program at 4,096 and at 131,072 entries with 8 markers. They bound the voluntary context switches per collection (not on Windows, which does not count them) and the `mw=` count of the GC log: one wake-up per MB marked plus two per collection. `dynamic-import-evaluation-error-gc.test.ts` sets `BUN_JSC_minimumBytesMarkedForParallelMarking=0`: it needs a collection that ends at a later safepoint than it began, and a collection under 2 MB is one pause now.

**Not done.** The three tests that force 8 markers to reach a marking race set the option to 0 (since #45568). I did not check that each of them still fails when its fix is reverted. No measurement on macOS or Windows.

</details>
