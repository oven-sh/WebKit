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
- 2026-10-21 22:10 UTC: FINAL WebKit commit is now e54eb6aff4 (preview autobuild-preview-pr-838-e54eb6af). Changes since 6e7c8ca9:
  collectContinuously cycles are never "small" (keeps race tests and JSTests collect-continuously mode concurrent); the poll re-arms after ANY marking
  (a marker polls each time it runs dry until it has marked nothing since its last poll): fixes a tree-shaped 100 MB full GC (51 ms -> 31 ms, main 37-45).
  496e0878 (before the re-arm change) had ALL 49 CI checks green, JSTests included.
  bun branch robobun/c4a0b0aa/marker-wakeups: tests commit + pin commit (pin = autobuild-preview-pr-838-e54eb6af), pushed, NO PR yet.
  Tests in the bun branch: gc-controller-cadence 'marking helper threads' (5 tests; 2 new LRU tests at 4096 and 131072 entries), and
  dynamic-import-evaluation-error-gc sets BUN_JSC_minimumBytesMarkedForParallelMarking=0. With 496e0878: all pass; on main 4 of 5 fail.
  FINAL TABLES measured with 496e0878 ("final" arm) on a QUIET machine (load 1-3), LTO, bun 590035462:
   A repro (5 runs): main 194.8 ns/op 3.07 s sys 70,080 vcs; wk800 175.9 / 2.63 / 64,771; final 71.3 / 0.15 / 920; final one marker 76.7 / 0.07 / 109;
     main one marker 76.3 / 0.07 / 99; node 72.6 / 0.19 / 671. (a later quiet run: final 64.2 / 0.12 / 710, one 71.7)
   B LRU ns/op (vcs): 16,384: main 175.4 (42,873) final 105.3 (1,327) one 104.7 (152) node 119.6; 65,536: 295.7 (23,521) / 236.0 (1,412) / 220.9 (598) / 292.5;
     131,072: 376.0 (18,156) / 348.0 (1,346) / 332.1 (627) / 411.9. Peak RSS final = main (80/170/210 MB), one marker 105/269/371.
   C records churn ns/record (vcs) [peak RSS], 7 runs: 3 MB live: main 1766 (37,415) [64] wk800 1736 (35,282) final 800 (1,174) [64] one 724 (96) node 689 (726);
     25 MB: 917 (11,108) [205] / 1048 (10,845) [208] / 825 (1,376) [222] / 865 (288) [214] / 1798 [351];
     101 MB: 615 (5,815) [577, 504-632] / 628 (6,034) [559] / 580 (1,741) [658, 592-668] / 911 (859) [609] / 1826 [671].
   D big churn bench-app 20000 8 100 (9 runs, GC log on): in-pause share main 16.0% wk800 14.7% final 39.0% one 99.2%; headroom used up (hf>=.98 at last stop)
     22% / 33% / 78% / 100%; peak RSS 678 / 693 / 801 / 734; ns/record 1221 / 1210 / 1020 / 1152; vcs 7,195 / 6,725 / 2,543 / 1,485. Without GC log (7 runs):
     ns/record 949.5 / 1120 / 985.7 / 1060.7; peak RSS 701 (652-761) / 681 / 781 (765-857) / 748.
     Rates: MB marked per ms outside pauses: main 1.22, final 1.25. Mutator allocation per ms of concurrent phase: main 0.39 MB, final 0.78 MB.
     => marking is as fast; the mutator runs 2x faster during concurrent marking (on main it waits for m_markingMutex, which helpers hold across futex wakes).
   E wide eden pause ms/MB (total ms): 2 MB: main 4.61 (244) wk800 3.92 (175) final 3.29 (152) one 2.52 (139); 8 MB: 1.41 (369) / 1.53 (382) / 1.05 (267) / 1.20 (267);
     32 MB: 0.66 (802) / 0.68 (791) / 0.56 (653) / 0.89 (945). helpers used (of 7): final 1 / 3 / 6, main 5 / 7 / 7.
   F 100 MB heaps full GC ms (Bun.gc(true), median of 7, 5 runs): tree main 44.7 wk800 47.2 final(496e) 54.2 one 100.9 -> with the re-arm change 31.1 (new2 R=1);
     map 51.4 / 51.4 / 54.5 / 78.2; list 83.3 / 188.4 / 83.2 / 81.5; wide 12.8 / 12.8 / 13.1 / 54.2; records 23.9 / 24.7 / 25.6 / 86.7. list 2M nodes: main 52.7, wk800 115.3, final 51.3.
   workers (new2): 4 workers ns/op main 431.9 (6,436 vcs) new2 307.6 (667) one 330.6; 12 workers 782.4 / 703.8 / 749.9.
   weak-heavy edens total pause ms (new2): 143k EventTargets: main 1175 new2 896 one 775; 200k old WeakMaps: main 540 / 430 / 394.
   sizes: bun text +4.4 KB (106,754,509 -> 106,758,925).
  TODO: wait preview e54eb6af + CI; build-arm.sh final2 autobuild-preview-pr-838-e54eb6af; rerun bun tests + tables A, D, E, F (and objdump gate 1);
  then open bun PR (base robobun/6d0d11d3/lazy-parallel-markers), update WebKit#838 title/body, issue comment, status comment, wrap-up.
- 2026-10-21 22:46 UTC: FINAL WebKit commit amended again: 618674cea25071fa43e24417aa733e6a41e1ffd4 (preview autobuild-preview-pr-838-618674ce): donateKnownParallel checks force-inlined into drain (no behaviour change vs ad68fb32, which had all 49 checks green and passed all bun GC tests: 'final2' arm). bun branch pin updated to it.
- 2026-10-21 23:25 UTC: WebKit final = 618674cea2 (all 49 CI checks green). bun branch e47302a672 pins autobuild-preview-pr-838-618674ce; bun tests pass with that build
  ("final3" arm): gc-controller-cadence marking tests 5/5, abort-controller-gc-reason 6/6, broadcast-channel-worker-gc 4/4, dynamic-import-evaluation-error-gc,
  module-children-concurrent-gc, 29519. Gate 1 by objdump (final3 vs main, LTO bun-profile): per-cell loop 55 instructions in both (+0);
  the two lock-free checks of donateKnownParallel are inline in drain in both (12 and 5-6 instructions); per round +15 in the lambda tail and 8 for the payment check.
  bun text +4,416 bytes.
  FINAL TABLES with the re-arm change (final2 = ad68fb32, same behaviour as 618674ce), 7-9 interleaved runs:
   A repro: main 151.1 ns/op 2.31 s sys 57,485 vcs; wk800 145.7 / 1.88 / 50,241; final 67.8 / 0.16 / 794; final one marker 76.7 / 0.06 / 102; main one marker 76.7 / 0.06 / 110; node 78.6 / 0.22 / 681.
   B LRU ns/op: 16,384: main 270.3 final 131.5 one 122.3 node 135.1; 65,536: 380.4 / 263.4 / 228.6 / 279.0; 131,072: 507.7 / 448.0 / 381.1 / 529.3 (final is 8-18% above one marker, vcs 1,400-1,700 vs main 28,000-52,000).
   C records ns/record (vcs) [RSS]: 3 MB: main 2104 (43,751) [63] wk800 2124 final 867 (1,160) [64] one 820 node 743; 25 MB: 1428 (12,800) [209] / 1356 / 1029 (1,440) [221] / 888 / 2127;
     101 MB: 1465 (7,975) [577] / 1334 [546] / 1157 (2,163) [649] / 1057 [624] / 2589 [670].
   D big churn (9 runs, log on): in-pause main 19.7% wk800 18.3% final 31.4% one 99.2%; headroom used up 30% / 20% / 80% / 100%; peak RSS 698 / 662 / 803 / 748;
     ns/record 1017 / 1102 / 878 / 1044; vcs 6,745 / 6,782 / 2,518 / 1,397. Rates: marked outside pauses MB/ms main 1.40 final 1.54; mutator alloc MB/ms in concurrent phases main 0.49 final 0.80.
   E wide eden pause ms/MB (total ms): 2 MB: main 6.96 (327) wk800 5.67 (321) final 4.01 (167) one 2.20 (115); 8 MB: 2.33 (575) / 2.16 (519) / 1.46 (337) / 1.38 (363);
     32 MB: 1.04 (1315) / 0.97 (1139) / 0.70 (839) / 0.96 (978). helpers used final 1 / 3 / 6 (main 5 / 7 / 7).
   F 100 MB full GC ms (9 runs): tree main 78.4 wk800 52.8 final 42.4 one 105.1; records 30.5 / 25.7 / 31.3 / 94.9; map 51.5 / 55.6 / 53.0 / 78.6; wide 24.5 / 27.0 / 21.7 / 57.1;
     list 86.9 / 193.4 / 86.6 / 84.4; list 2M: 52.9 / 116.9 / 52.8.
  NEXT: debug-asan jsc probes; PR bodies; mark WebKit#838 ready + new title; open bun PR; status comment; issue comment; self-review.
