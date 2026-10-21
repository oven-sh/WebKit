# Scratch state for oven-sh/bun#44893 (robobun session c4a0b0aa). Delete this branch when the work is merged.

The container is reset between turns. Everything local is lost. This branch carries the scripts and the state.

## After a reset
    cd /workspace/WebKit && git fetch origin robobun/c4a0b0aa/scratch && git worktree add /workspace/scratch FETCH_HEAD && /workspace/scratch/bench/setup.sh
    # WebKit branch: robobun/c4a0b0aa/marker-wakeups (PR oven-sh/WebKit#838, draft, stacked on #800 branch robobun/6d0d11d3/lazy-parallel-markers)
    # bun branch: robobun/c4a0b0aa/marker-wakeups (no PR yet), base = robobun/6d0d11d3/lazy-parallel-markers (oven-sh/bun#45568)
    # A/B builds: /tmp/repro/build-arm.sh <arm> <webkit tag>  (bun 590035462 worktree /workspace/bun-ab, LTO) -> /workspace/bins/<arm>/{bun,bun-profile}
    #   main = 87ee10da087b63e011bd1ada64d1814a850a4395, wk800 = autobuild-preview-pr-800-2cacac75, old838 = autobuild-preview-pr-838-9c0c7a0d
    # prebuilt jsc for quick checks: https://github.com/oven-sh/WebKit/releases/download/<tag>/bun-webkit-linux-amd64-lto.tar.gz (bin/jsc), also -debug-asan

## Design review result (2026-10-18, run ae40f463e5f1): pick = on-request-helpers = WebKit#838, with repairs that gate the merge
1. Zero per-cell cost in SlotVisitor::drain (gate 1: +0 instructions per cell, +0 to donateKnownParallel's early-outs; objdump main vs PR; reference 31 per cell).
2. One wake function: payment check inside; every futex wake after m_markingMutex is released.
3. Large-cycle staffing (gate 2): at each resume request the paid-for deficit; a helper that runs dry while another marker is active polls
   the shared stacks with a bounded spin before it leaves. Gate: parity with main on bench-app.mjs 20000 8 100:
   in-pause share (main 18%, old #838 56%), collections with hf >= 0.98 at the last stop (main 34%, old 87%), peak RSS (main 660-713 MB, old 807-832).
   Pick the price default from the shape matrix, not from the repro alone.
4. "A cycle that asked for nobody stays paused" in Collector::runFixpointPhase for all schedulers; performIncrementOfDraining through the wake function;
   one marker keeps today's paths; option 0 = no payment needed; a refused thread start must not count as a requested marker (compose with WebKit#824).
5. Liveness: JSTests stress at 8 markers with collectContinuously and verifyGC, option 0 and default, on Debug+ASAN jsc; bun CI lanes macOS/Windows/musl.
6. bun tests: wake-ups per collection (mw=) under markedKB/price + resumes + 1 at capacity 4,096 and 131,072; helpersKB > 0 for a 40 MB full;
   option 0 at every race-test site, and show each race test fails with its fix reverted.
7. Ship: WebKit merges first; WEBKIT_VERSION = merged sha, never a preview tag; PR body reports pause and memory both.
Other findings: linked LRU at 16,384 to 65,536 entries was 12-32% above one marker at 2 MB (16,384: 134 ns/op vs 104; 96 with 8 MB);
workers: 173 HeapHelper sleeps per worker per 100 rounds; solver gets 0 helpers under 2 MB (weak-heavy edens);
3 race-guard test files stop exercising concurrent marking; platforms other than Linux x64 never ran it.
Measurements the PR must report: drain loop instructions; repro switches and wake-ups per collection; 115-190 MB churn in-pause share,
headroom used up, peak RSS; tree/map/list full GC ms and helper share; 16k/64k/131k LRU ns/op vs one marker; workers sleeps;
weak-heavy eden pause; thread starts and requests per collection (gdb); release binary size and sizeof SlotVisitor/Collector.

## Earlier numbers (old design 9c0c7a0d, LTO, quiet machine, Oct 18)
repro lru-gc.mjs 500: main 188.9 ns/op 3.09 s sys 69,830 vcs; #800 196.5 / 3.00 / 72,041; old #838 68.3 / 0.16 / 1,226; old #838 one marker 79.8 / 0.08 / 120;
main one marker 82.5 / 0.10 / 105; node 26.3 78.4 / 0.17 / 670. List 2M nodes full GC: main 60-73 ms, #800 122-132, old #838 58-62.
Records churn ns/record (3/25/101 MB live): main 2528/1416/1425; #800 2160/1389/1305; old 872/1002/1160; one 811/933/1246; node 763/2205/3114.
Peak RSS 101 MB churn: main 540, #800 533, old #838 676, one marker 610. Wide eden pause ms/MB (2/8/32 MB): main 6.55/2.33/1.13; #800 6.05/2.53/1.07; old 2.53/1.68/0.83; one 2.43/1.85/1.09.

## Log
- 2026-10-20: rework pushed as WIP f92d2da2 (credits per 2 MB visited, choosePaidForMarkersToWake / chooseMarkerToWakeIfNoneIsMarking / wakeMarkers,
  pollSharedMarkStacks with Options gcMarkerPollMS=0.05 and gcMarkerPollRearmBytes=64K (EXPERIMENT knob), deficit at resume, small-cycle rule in
  runFixpointPhase, lazy pool task, nothing per cell in drain). CI: all lanes build; JSTests jobs timed out (90 min).
- 2026-10-21: cause found with the preview jsc: SIGSEGV in GCSegmentedArray::isEmpty() from pollSharedMarkStacks (isEmpty follows the head segment,
  which donate/steal take off for a moment). With --gcMarkerPollMS=0 all probes pass. Fix 1dbad083 (size() instead) pushed 18:27 UTC.
