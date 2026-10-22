The cause is in JavaScriptCore's parallel marking. #45568 does not cover this program. #46432 with oven-sh/WebKit#838 does.

**Cause**

- A collection of this program marks 1 to 10 MB. oven-sh/WebKit#800 (the change in #45568) keeps the helper threads out of a collection until it has marked 1 MB. So most of these collections still get every helper.
- Each time a marking thread shares a cell or two (`SlotVisitor::donateKnownParallel`), it wakes every parked helper, and it holds the marking lock while it does. One helper takes the cells. The others park again. A `Map` over a linked list gives a second thread next to nothing, so this repeats for the whole collection: about 220 thread wake-ups per collection.

**Numbers**

`lru-gc.mjs 500` on a 12 vCPU Linux KVM guest. 7 fresh processes per arm, arms interleaved, medians. All Bun arms are LTO builds of `590035462`, and the row of #46432 is from an earlier build of it with the same marking protocol:

| | ns/op | system CPU | voluntary context switches |
|---|---|---|---|
| main | 162.9 | 2.53 s | 60,056 |
| #45568 | 163.0 | 2.28 s | 56,665 |
| #46432 | 66.8 | 0.15 s | 821 |
| main, `BUN_JSC_numberOfGCMarkers=1` | 79.2 | 0.06 s | 107 |
| Node 26.3 | 75.6 | 0.18 s | 741 |

The build that #46432 pins makes 855 voluntary context switches in the same run on a loaded machine, where main makes 30,006.

With oven-sh/WebKit#838 the marking pays for each wake-up of a helper thread (2 MB visited for one), and an idle helper leaves the collection. The pull requests list the costs. The largest one: peak RSS grows 10 to 20% on a churn that keeps 125 MB live.

Until a release has this, `BUN_JSC_numberOfGCMarkers=1` is the workaround for a small heap. A full collection of a 100 MB heap is then up to 3 times slower.
