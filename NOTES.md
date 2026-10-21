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
- 2026-10-21 19:00-20:20 UTC (one turn, do NOT end the turn while work is local):
  - 1dbad083 builds on all lanes; JSTests jobs cancelled at 90 min again. Cause found by running the CI harness locally
    (bench/run-jsc-tests.sh): stress/array-unshift-moved-cell-barrier.js (requireOptions --minimumGCPauseMS=0 --gcPauseScale=0)
    hard-times-out in all 12 modes: with a minimum pause of 0 the small-cycle rule in runFixpointPhase drained for 0 ms and never resumed (livelock).
    Fix in 8a84de25: staysPausedWhileSmall() = isSmallSoFar() && minimumGCPauseMS > 0; m_isSmallSoFar = false at resume.
  - 8a84de25 also adds the payback rule: a woken marker pays for a wake-up after gcMarkerWakeupPaybackBytes (EXPERIMENT knob, 256 KB) instead of the
    full price (SlotVisitor::didWakeUpForMarking, m_bytesVisitedAtNextPayment). Plan: final = price / 8, same threshold for the poll re-arm; remove both knobs.
  - Measured with 1dbad083 (LTO, bun 590035462), 5-7 interleaved runs:
    repro lru-gc 500: main 191.9 ns/op 3.09 s sys 69,831 vcs; new 74.6 / 0.18 / 850; T0 (no poll) 71.5 / 0.15 / 846; p1m 81.0 / 0.34 / 1,975;
      p512k 86.7; p4m 68.5 / 0.07 / 346; p0 123.5 / 1.34 / 27,327; one marker 78.2 / 0.07 / 109; node 77.6 / 0.16 / 699.
    lru 16,384 entries (100 rounds): main 237.6; new 126.6; T0 120.4; p4m 111.8; p8m 100.3; one 114.3; node 136.1.
    big churn bench-app 20000 8 100 (in-pause % / hf>=.98 % / peak RSS / ns/op / mw): main 12.8-20.5 / 20-27 / 662-672 / 1390-1486 / 0;
      new 34 / 70 / 797-837 / 1281-1364 / 650-680; T0 38.6 / 89 / 809; T200 27.7 / 60 / 772; R0 30.6 / 55 / 718 (user CPU 3.86 s vs 2.1);
      p512k 24 / 50 / 749; p128k 36 / 70 / 772; p0 25 / 50 / 763 / 1207.
    KEY ANALYSIS: MB marked per ms OUTSIDE pauses is the same: main 0.97, new 1.02, p0 1.30. The mutator allocates 2x faster during concurrent
      phases in new (0.59 MB/ms vs 0.31 on main): main's mutator is slowed by the 7 helpers (lock contention / steal). So new hits the
      headroom cap more often and peaks higher. Gate 2 "parity with main" is not reachable by faster marking alone on this VM.
  - jsc quick checks: /root/.bun/build-cache/webkit-preview-pr-838-<sha8>-lto/bin/jsc after a build-arm.sh; stress-run.sh <jsc> <out> [opts] runs every
    JSTests/stress test (30 s timeout); default mode and collect-continuously mode showed no new timeouts vs main.
  - CI logs and artifacts are NOT reachable from the container (blob host blocked): reproduce failures locally with run-jsc-tests.sh.
  - gh: REST quota is often exhausted by other sessions; bench/checks.sh <owner/repo> <sha> reads check runs through GraphQL.
- 2026-10-21 21:01 UTC: FINAL WebKit commit 6e7c8ca952 force-pushed to robobun/c4a0b0aa/marker-wakeups (one commit on top of #800's 2cacac75).
  Preview tag: autobuild-preview-pr-838-6e7c8ca9. The commit before the cleanup (691ff31b, same behaviour but rearm 64K and knobs) had ALL 49 CI checks green.
  Final parameters: price = minimumBytesMarkedForParallelMarking = 2 MB; a woken marker pays after price/8 (256 KB), same threshold re-arms the poll;
  gcMarkerPollMS = 0.05.
  Gate 1 (objdump, LTO bun-profile main vs 1dbad083): the per-cell loop of the drain lambda has the same 55 instructions in both (identical sequence up
  to the type dispatch), so +0 per cell. Per round: +15 instructions in the lambda tail and 8 at the top of the loop (payment check).
  donateKnownParallel was a call in 1dbad083 (+4 instructions per early-out); 691ff31b splits the locked tail out of line (verify it inlines again).
  Measured with 8a84de25 = "new2" (payback 256 KB, rearm 64 KB), LTO, 5-7 runs:
    repro: main 190.5 ns/op / 3.17 s sys / 72,382 vcs; new2 70.9 / 0.14 / 917; one marker 75.4 / 0.03 / 105. (p4m: 64.5 / 0.06 / 354)
    lru 16,384: main 224.6; new2 123.1; one 115.7; (4 MB price: 102-106). lru 65,536: main 336.8, new2 249.0, one 239.4, node 302.0.
    lru 131,072: main 387.8, new2 351.5, one 368.2, node 518.1 (peak RSS 186 / 229 / 371 / 280).
    big churn (bench-app 20000 8 100): main in-pause 16.6% hf 25% RSS 690 ns/op 1691; new2 30.0% / 67% / 792 / 1251 (mw 867);
      MB/ms marked outside pauses: main 1.05, new2 0.93-1.15; mutator alloc MB/ms in concurrent phases: main 0.31, new2 0.46-0.53.
    wide eden pause ms/MB (2/8/32 MB batches): main 5.73/2.20/0.98; new2 3.16/1.39/0.48; one 1.94/1.57/0.93. (4 MB price: 2.6-4.1/1.64/0.69-0.82)
    100 MB heaps, full GC ms: tree main 158.0 new2 95.5 one 102.9; map 88.2/86.4/87.6; list 94.0/99.3/86.7; wide 52.0/39.0/57.3; records 79.3/67.3/100.7.
    list 2M nodes best ms: main 52.8-56.2, new(1dbad083) 52.3-53.
  TODO after CI of 6e7c8ca9: build arm "final" (build-arm.sh final autobuild-preview-pr-838-6e7c8ca9), re-measure the tables for the PR bodies
  (repro, lru sizes, big churn, eden, shapes, records 3/25/101 MB), objdump gate 1 again, sizes; update bun branch pin + tests; open bun PR
  (base robobun/6d0d11d3/lazy-parallel-markers); update WebKit#838 title/body; issue comment; pushback self-review if the session allows.
