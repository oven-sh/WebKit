(draft of the bun PR body, Oct 18, numbers are for the OLD design 9c0c7a0d: redo the numbers for the reworked build)

Fixes #44893

### Problem
- An LRU churn (a Map over a linked list) runs at 189 ns/op in Bun, 78 in Node and 83 with one marker, with 3.1 s system CPU and 69,830 context switches.
- Each collection marks 1 to 10 MB, and each donation of a cell or two wakes every sleeping GC helper (`SlotVisitor::donateKnownParallel`): about 220 wake-ups per collection. #45568 applies under 1 MB only.

### Fix
- oven-sh/WebKit#838, pinned here: a marking thread wakes one thread per 2 MB it has visited, and an idle helper leaves.
- Correct because the cost is per wake-up, not per byte.
- Verified: `test/js/bun/gc/gc-controller-cadence.test.ts` (new test, fails on main and #45568). The issue's program: 68 ns/op, 0.16 s system CPU, 1,226 switches.

### Background
- JSC marks with the collector's thread and up to 7 helper threads. `BUN_JSC_logGC=1` now prints their wake-ups (`mw=`).
- Stacked on #45568. Merge oven-sh/WebKit#838 first: the pin is its preview build.
- Considered a higher #45568 threshold: a wide 10 MB collection then marks on one thread.

### Downsides
- (redo from the new measurements: helpers in wide collections, pause, peak RSS, one machine only)

Issue comment draft: "I checked this program against #45568 (oven-sh/WebKit#800, the fix for #44797). It does not close this gap. <bun PR> does." + table
(main / #45568 / PR / PR one marker / main one marker / Node) + "Why #45568 does not cover it: its WebKit change keeps the helper threads out of a
collection that marks under 1 MB. The collections of this program mark 1 to 10 MB each (median about 3 MB)." + "What is left: a marking thread that
shares a cell or two wakes every sleeping helper (SlotVisitor::donateKnownParallel)... about 220 thread wake-ups per collection... That is the futex
and sched_yield time in the report."
