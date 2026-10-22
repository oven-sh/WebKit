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
- 2026-10-22 00:55 UTC (same turn as the 23:25 entry; the container was NOT reset):
  bun branch head is now e1161fe5a3 (tests commit amended: 131072 case limits switchesPerCollection 200, collectionsOver2MB 3; comments say "2 times", "3 to 5 (100 to 150 before)", "50 to 80 (200 to 560 before)").
  With final3: marking tests 5/5 in 6 runs; on main 4 of 5 fail (102-123 vs 30, 300-438 vs 200).
  ALL TABLES RE-MEASURED WITH final3 (= 618674ce): bench/results/final3-{A,B,C,D,D2,E,F,W,K}.txt (driver bench/final-tables.sh). The host was busy (steal 2.5-4x the CPU
  the benchmark got), so 8-marker wall-clock numbers are up to 2x slower than in the 22:40 session; context switches and system CPU are the robust signals.
  D is now measured by bench/churn-phases.mjs (definitions in its header): "in-pause %" = share of marked bytes that are marked while the mutator is stopped.
  gdb samples of the JS thread (bench/gdb-sample2.sh + gs-classify.mjs, 400 per arm, bench-app 20000 8): main 32 samples = allocation slow path parked on m_markingMutex
  (Collector::runConcurrentPhase -> SlotVisitor::didReachTermination), PR 0. In pauses: main 55 samples in a lock or a wake-up call, PR 13 (12 in requestThread).
  => the mutator allocates 1.5x faster while a cycle marks (0.62 vs 0.40 MB/ms), uses up the headroom in 78% vs 27% of collections, peak RSS 798-814 vs 678-706 MB.
  Debug+ASAN sample (every 40th JSTests/stress test, 151 tests, 3 modes): no crash, no assertion, no ASAN report. Timeouts at 180 s: 2 tests loop forever without their
  --watchdog option; has-own-property-name-cache-symbol-keys (main 379 s, PR 192 s), regress-158411 (main 54-426 s, PR 121-284 s),
  string-regexp-methods-cross-realm-dfg (cc+verifyGC: main 157 s, PR option 0 374 s), typedarray-construct-from-array-mutated-by-tonumber (main 540 s, PR option 0 575 s).
  Drafts: bench/wk838-body-draft.md, bench/bun-pr-body-draft.md (placeholders PRICE_SWEEP, ASAN_SAMPLE_TIMEOUTS, SWITCHES_MAIN).
  NEXT: asan-trio result (is option 0 slow in collectContinuously?), price sweep (bench/price-sweep.sh final3 final3), PATCH WebKit#838 (title, body, ready),
  open bun PR (base robobun/6d0d11d3/lazy-parallel-markers), status comment, issue comment, pushback self-review on oven-sh/WebKit#838, wrap-up.
- 2026-10-22 01:40 UTC: WebKit head is now 20c0b1bc2e (force-pushed; 618674ce is kept in robobun/c4a0b0aa/marker-wakeups-wip). Change vs 618674ce:
  the option at 0 = Collector::helpersJoinEveryCycle() = the UPSTREAM protocol (helpers join at the start, park in the cycle, notifyAll on every donation);
  SlotVisitor::drainFromShared keeps its upstream body and dispatches to drainFromSharedWithHelpersOnRequest() otherwise. Why: with "0 = no payment" a
  tree-shaped 100 MB full GC took 309 ms (main 176, default 110), Debug+ASAN collectContinuously tests were 1.9x slower than main, and the race tests
  that set the option to 0 did not get the concurrency they were written for. Price sweep (bench/results/final3-P-*.txt): repro ns/op 0:140 512K:97 1M:81
  2M:74 4M:65 8M:70 one:89; wide eden 8 MB ms/MB 0:1.92 512K:1.55 1M:1.40 2M:1.44 4M:1.84 8M:2.71; 32 MB: 0.95/0.86/0.65/0.77/1.02/1.23; churn RSS 770-835 at every price.
  collectContinuously: a small heap's cycle is one pause now, so more cycles run per second: release jsc runs cc tests 4-7x faster; Debug+ASAN with verifyGC
  1.3-1.4x slower (388-415 s vs 279-320 s on string-regexp-methods-cross-realm-dfg.js); cycles in 100 s: main 1527 (1.85 stops each), PR 2756 (1.13 stops each).
  Local compile check without a build: bench/syntax-check.sh <prebuilt dir> heap/SlotVisitor.cpp heap/Collector.cpp (passes for 20c0b1bc).
  NEXT: wait for preview autobuild-preview-pr-838-20c0b1bc; build-arm.sh final4; verify option 0 == main (repro vcs ~60k, tree), default unchanged, bun tests,
  objdump; ASAN sample with the new debug-asan tarball; update pin in bun branch; PR bodies (drafts in bench/); open bun PR; issue comment; pushback.
- 2026-10-22 01:45 UTC: WebKit#838 title and body updated on GitHub (still draft; body = bench/wk838-body.md). Pushback self-review launched on oven-sh/WebKit#838:
  run_id 49c2ef2e6e00 (resume: Workflow({module: "robobun:rdr/pushback", resume_from_run_id: "49c2ef2e6e00"})). /tmp/design.json rebuilt (bench/design.json).
  Waiting for preview autobuild-preview-pr-838-20c0b1bc (bench/wait-build.sh final4 <tag> builds the arm and unpacks the debug-asan tarball to /workspace/dl/wk-final4-asan).
  Then: bench/verify-arm.sh final4 final4; bench/asan-probes.sh + stress-sample-asan.sh with the new asan jsc; pin the new tag in the bun branch; open the bun PR.
- 2026-10-22 02:40 UTC: BIG CHANGES.
  * bun PR opened: https://github.com/oven-sh/bun/pull/46432 (status comment id 6330975638). Its first CI (build 130582) failed at the pipeline step:
    "This branch is too far behind main to build. Merge main into it." => the stack had to be rebased.
  * WebKit#838: base is now `main`; branch rebased onto d6375c72 (the commit bun main pins). TWO commits: 3767944f2f = oven-sh/WebKit#800's commit (cherry-picked,
    author kept), 1c8b23b501 = this change. Old heads: 20c0b1bc (old base, in branch ...-wip), 618674ce (measured build).
    NOTE: #800's real base is b1e30d08 (87ee10da = b1e30d08 + 1 JIT commit), so the A/B "main" arm had one more commit than the previews.
    The clone is shallow: .git/shallow was edited so that 2cacac75 has its parent b1e30d08 (needed for cherry-pick).
  * New in 1c8b23b501: Collector::helpersJoinEveryCycle() = option at 0 OR collectContinuously (upstream protocol for both);
    payForMarkerWakeupIfNeeded() no longer reads the option (didStartMarking sets the threshold to SIZE_MAX when helpers join every cycle).
    Why cc: under the new protocol a cc cycle of a small heap is one pause (1.13 stops/cycle vs 1.85) => JSTests cc mode and Bun's 24 cc test files lose the
    marker/mutator overlap.
  * bun branch (local, /workspace/bun-pr, NOT pushed yet): rebased onto origin/main 2a5cbbcb08: 3 test commits of #45568 + my test commit 688413370b
    (patches in bench/bun-rebased-patches). Review fixes from claude[bot] on #46432: LRU cases are now 4096x60 and 32768x4, skipIf(isASAN), no timeout,
    context-switch bound Linux only (20 and 50), mw bound 2/MB; option 0 added to sourcetextmodule-link-gc (2 spawns) and zlib-estimated-size-gc.
    With final3: 5/5 pass under load average 55; main fails (78-94 vs 20, 118-148 vs 50).
    TODO when preview autobuild-preview-pr-838-1c8b23b5 is published: pin commit on top (scripts/build/deps/webkit.ts), push to
    origin robobun/c4a0b0aa/marker-wakeups (force), change PR #46432 base to main (gh api -X PATCH repos/oven-sh/bun/pulls/46432 -f base=main),
    update both PR bodies (the stack is no longer stacked; bun PR contains #45568's tests), reply to the 4 review threads and resolve them.
  * Local builds: /workspace/bun-ab2 = bun main worktree; bench/build-arm2.sh <arm> <webkit> builds bun main arms (main2 = d6375c72 building now).
  * Pushback run 49c2ef2e6e00 still running (broadcast sent about the rebase).
- 2026-10-22 04:20 UTC: SHIPPED STATE.
  * WebKit#838: head 0b2110fe90 (= 1c8b23b5 re-committed to trigger CI), base main, 2 commits, ALL 52 CHECKS GREEN (JSTests included). Still DRAFT
    (waiting for the pushback result). Preview: autobuild-preview-pr-838-0b2110fe (published 03:03 UTC).
    NOTE: `curl -I` (HEAD) on a release asset gives 401; probe with `curl -r 0-99` (206). wait-build*.sh fixed.
    NOTE: a push while the PR had conflicts with its base started no CI run; a base change does not start one either: push again (amend) after the base change.
  * bun#46432: base main, head e9c994d6a6 = main 2a5cbbcb08 + 3 test commits of #45568 + my test commit 688413370b + pin commit (preview 0b2110fe).
    Body updated (bench/bun-pr-body.md). The 4 claude[bot] review threads are answered and resolved. Issue comment posted:
    https://github.com/oven-sh/bun/issues/44893#issuecomment-6332554058
  * Final build verified locally: arms /workspace/bins/main2 (bun main + d6375c72) and /workspace/bins/new2 (bun main + preview 0b2110fe), results bench/results/v2-*.txt:
    marking tests 5/5 x3 (main2: 4 fail, 76 vs 20 and 127 vs 50); abort-controller 6/6, broadcast-channel 4/4, dynamic-import 1/1, sourcetextmodule 3/3, zlib 6/6,
    module-children 1/1, esm-registry 1/1, 29519 2/2, 30205 4/4, error-stack-finalizer 3/3, message-event-init-gc 1/1. The "idle release lets FTL code age out"
    tests of gc-controller-cadence time out at 5 s on BOTH arms when the machine is loaded (not related).
    Under load (ASAN sample + review agents running): repro main 30,006 vcs / 1.84 s sys; new 855 / 0.38; new option 0: 32,162 / 2.02 (= main, upstream protocol);
    new cc 119,649 vs main cc 88,878; tree 100 MB: main 62.9 ms (13,670 vcs), new 44.8 (1,013), option 0 50.5 (24,056); churn RSS main 765, new 807, option 0 743.
  * ASAN sample of the final head runs in the background: /tmp/asan2-{default,price1,sync,cc,p0}.txt and /tmp/asan2-probes.txt (jsc /workspace/dl/wk-new2-asan).
  * TODO: ASAN results -> WebKit body; objdump main2 vs new2 (drain); pushback result -> address; mark WebKit#838 ready; wrap-up; delete scratch + wip branches at the very end.
- 2026-10-22 04:35 UTC:
  * bun#46432 head is now d20afca6a2 (test commit 50ee5ba6ac + pin). Review round 2 (coderabbit, claude[bot]): zlib-estimated-size-gc change taken out again
    (`bun -e` runs with ONE GC marker: ZigGlobalObject.cpp oneShotStartup), mw=0 assertion added to "stay out of a collection that marks little". All 7 review
    threads answered and resolved. PR body = bench/bun-pr-body.md.
  * bun CI build 130658 (for e9c994d6): the only red job was ":windows: x64 - verify-baseline": a JPEG codec test of simd-baseline.test.ts timed out at 5 s under
    Nehalem emulation; the sibling JPEG test timed out the same way in build 130636 of an unrelated PR; main passes it. => flake, not ours. No retrigger used
    (the push of d20afca6a2 runs CI again).
  * The review agents of the pushback run load the machine (load average 50-65): local test runs time out at 5 s now; do not trust local timing until it ends.
  * ASAN sample of the head (5 modes x 152 tests, bench/results/asan2-*.txt): 0 assertions, 0 ASAN reports; 11 timeouts at 180 s in the 2 modes that ran under load
    (pairs: bigint-oom... 388 s on d6375c72 vs 407 s head; has-own-property-name-cache-string-keys 316 vs 311 s).
  * objdump main2 vs new2 (drain): per-cell loop 55 instructions in both; +3 per round in drain, +16 in the lambda tail, +7 per 100 cells; text +7,424 bytes.
  * WebKit#838 body updated with all of this (bench/wk838-body.md). Still draft until the pushback result is in.
  * TODO: pushback result (run 49c2ef2e6e00) -> address; mark WebKit#838 ready; head's ASAN probes (/tmp/asan2-probes.txt) into the body; wrap-up; delete scratch + wip
    branches (robobun/c4a0b0aa/scratch, robobun/c4a0b0aa/marker-wakeups-wip in oven-sh/WebKit; robobun/c4a0b0aa/marker-wakeups-prerebase is local only in bun).
- 2026-10-22 05:55 UTC: WINDOWS x64 verify-baseline (Intel SDE) lane.
  * It failed twice on this PR (builds 130658, 130668): "JPEG codec > flat grey" of test/js/bun/jsc-stress/fixtures/simd-baseline.test.ts over its 5 s. NOT a flake:
    survey of 105 builds of other branches (/tmp/survey-vb.py): flat grey 2299..4590 ms, never over 5 s (one "gradient" 5101 fail on another branch);
    on this PR's build 3 of 17 default-protocol runs were >= 5000 (slow mode = +1.5 s), the rest 2914..3698.
  * The crash at exit seen in the diagnostics is NOT ours: mimalloc scavenger, WakeByAddressSingle after ExitProcess killed the scavenger thread
    (mi_scav_wake_one <- mi_arena_schedule_purge <- Rust TLS destructors <- ExitProcess). Open PR oven-sh/bun#46016 (another session). It is in ~45% of the
    verify-baseline runs of every branch, exit code stays 0, +27 s per crashing run.
  * HYPOTHESIS for the slow mode: Bun's GC controller asks for an idle FULL collection after 10 quiet 1 s ticks (BUN_IDLE_GC_SECONDS=10,...). The run takes ~21 s
    under SDE, so that lands near the JPEG tests in some runs. With the new protocol that collection (heap ~0.5 MB visited) is ONE pause on the JS thread, and under
    SDE (Pin translates every new code path) the first eden collection already pauses 916 ms. Upstream does it with helpers while the JS thread runs.
    Native (Linux, this host, /tmp/idlegc): idle full GC of the same test heap: PR p=3.43 ms in one pause; main first pause 0.60 ms, then collector thread 30.7 ms.
  * FIX CANDIDATE (WebKit, not pushed yet, in /workspace/WebKit working tree): Options::maximumSmallGCPauseMS = 10: Collector::staysPausedWhileSmall() is false
    once the pause has lasted that long; then the scheduler decides and the resume wakes one helper. Syntax check passes.
    Bun tests then: "stay out of a collection that marks little" must pin the limit high (a preempted pause of 13 ms was seen natively: would be mw=1);
    new test pair with BUN_JSC_minimumGCPauseMS=0.01 BUN_JSC_gcPauseScale=0 ring(4096): limit 0 -> wake-ups, limit 60000 -> none.
  * Diagnostic v3 = bun commit e7f50a50c8 ("[skip tests]" in the subject: builds + verify-baseline only), build 130728: forced idle full GCs (BUN_IDLE_GC_SECONDS=3,3,3)
    x default / upstream (option 0) / price 32 KB, then natural default runs with GC log. Result -> /tmp/vb-diag.log (wait-vb.py).
  * The temporary commit on the bun branch MUST go before the PR is ready (return to d20afca6a2 + new pin).
- 2026-10-22 06:05 UTC: diag v3 result (build 130728, bench/results/vb-diag-v3.log). THE IDLE-FULL-GC HYPOTHESIS IS WRONG.
  * Forced idle full GC (BUN_IDLE_GC_SECONDS=3,3,3) under SDE: pause 224 ms (default), 244 ms (upstream protocol), 110 ms + concurrent (price 32 KB). Not 1.5 s.
    It runs when JS next runs after the await (Windows: mutator keeps the conn while parked), so it sits at the end of flat grey: +0.4 s in every protocol.
  * First collection of a process under SDE: one pause of 951..984 ms (default) vs 601 + 360 ms in two pauses (upstream). Same total.
  * Natural slow run caught (diag 12, flat grey 5492 ms): its GC log is the SAME as a normal run: 7 eden collections, all mw=0, pauses 972/12/220/9/121/68/7 ms,
    no full collection. So the +1.9 s is NOT a GC pause of the JS thread. Forced-default run 3 was slow too (5521) with a normal GC log.
  * Slow-mode rate so far on this build: default protocol natural 4 of 29, forced 1 of 2; upstream protocol on this build 0 of 8; other builds 0 of 108.
  * Totals of the file are bimodal by ~1.4 s also in runs with a normal flat grey (21.9 vs 23.3 s): looks like ONE floating event of about 1.5 s that lands
    in the startup in most runs, in flat grey in the slow runs, nowhere in some. Cause unknown. Candidates: thread creation/exit under Pin, mimalloc scavenger
    sweep of a parked thread, a JIT tier first use, Pin code cache.
  * The pause-limit patches (WebKit maximumSmallGCPauseMS, bun tests) are parked in bench/wk-pause-limit.patch and bench/bun-test-pause-limit.patch: NOT the fix
    for this lane. WebKit working tree still has the change applied (uncommitted).
  * diag v4 = bun commit 326db49d43 ([skip tests]): instrumented copies of the test file (step times png/encode1/recode, optional 20 ms heartbeat for JS-thread
    gaps), 6 concurrent runs per batch: default x2, heartbeat|original, upstream|one marker, minimumGCPauseMS=0|GC timer off, MIMALLOC_SCAVENGER=0|useDFGJIT=0.
- 2026-10-22 06:10 UTC: NEW HYPOTHESIS for the slow mode on the SDE lane (to be confirmed by diag v5 = bun commit 80cca09eb0, build 130739; v4 was cancelled by it):
  mimalloc's idle sweep of the JS thread, WINDOWS ONLY. src/jsc/bindings/BunJSCEventLoop.cpp Bun__JSC_onBeforeWait: with the libuv loop the JS thread itself
  calls mi_on_thread_idle() before it waits (rate limit 100 ms), and up to 3 follow-up sweeps 100 ms apart while mi_on_thread_idle_pending() (a large page that
  was allocated from in the current hole-purge epoch or the one before). vendor/mimalloc/src/page.c: every 64th sweep of a thread (tld->holes_sweep_seq,
  purge_holes_full_every=64) walks the free list of EVERY page (mi_page_purge_holes_walk); the epoch is process-wide and any thread's idle sweep moves it
  (_mi_page_purge_holes_epoch_advance). WTF::AutomaticThread (WebKit fork, USE(MIMALLOC)) calls mi_on_thread_idle() 100 ms after a thread went idle:
  upstream's 7 GC helpers do that after every collection, so they move the epoch; with the new protocol no helper thread exists in a small heap, the JS thread
  needs its own follow-up sweeps, its sweep count grows faster, and the 64th (full) sweep comes within the 25 s of the emulated run: about 1.5 s under SDE,
  anywhere in the run (mostly in the startup: file totals 21.9 s without it, 23.3 s with it), in flat grey in 1 run of 7.
  v5 arms: mimalloc statistics (Bun.unsafe.mimallocDump) before/after flat grey, MIMALLOC_PURGE_HOLES_FULL_EVERY=0 and =4, MIMALLOC_PURGE_HOLES=0, upstream,
  one marker, GC timer off, heartbeat, original file. 6 runs at a time.
  If confirmed: not a GC regression; the lane's 5 s per test is the native default on an 80x slower emulated run (main: flat grey up to 4.6 s, gradient failed
  once at 5.1 s in 108 builds). Fix for the lane = --timeout for the emulated run in scripts/verify-baseline.ts, with the evidence in the PR body.
  The pause-limit patch (maximumSmallGCPauseMS) is DROPPED from the WebKit tree (kept in bench/wk-pause-limit.patch): the data does not ask for it.
- 2026-10-22 06:30 UTC: diag v5 result (build 130739, bench/results/vb-diag-v5.log; 14 batches x 6 concurrent runs). THE MIMALLOC FULL-SWEEP HYPOTHESIS IS WRONG TOO.
  * The slow mode is inside the synchronous `png()` helper of the flat grey test (2880 iterations of `raw.set(rgb(x, y), ...)`, then deflateSync, 3 crc32):
    png = 2.6..3.4 s normally, 4.8..5.2 s in slow runs, 1.4..1.6 s in two runs whose startup was 4 s longer. No collection runs inside png()
    (GC #5 comes during the second await). encode1 0.55..1.0 s, recode 0.22..0.4 s do not change.
  * The times are quantized: png = k x ~1.5..1.7 s with k = 1, 2 (typical) or 3 (slow); "first test after" = 16.8 s + j x ~1.9 s with j = 0, 1, 2.
    Upstream protocol and one marker (slot 3): j = 0 and k = 2 in 14 of 14 runs. Default protocol arms: one EXTRA event in about 1 run of 3
    (in the startup: 12 runs, in png: 9 runs; slow png: default 8 of 28, MIMALLOC_PURGE_HOLES=0 1 of 7, other default-protocol arms 0 of 35).
    MIMALLOC_PURGE_HOLES=0 did not remove it, so it is not the hole sweep.
  * What upstream and one marker share and the default lacks: cycles scheduled with resumes (numberOfGCMarkers=1 sets m_isSmallSoFar=false).
    So the extra ~1.7 s event goes with "a small cycle is one pause" (compiler threads stay suspended for the whole cycle), and it lands OUTSIDE the pauses.
  * Heartbeat arm: after png() returns the JS thread is busy for another 0.7..0.9 s before the first timer callback (Bun.Image first use), then 0.25..0.4 s.
  * Working guess: the events are first uses of big code under Pin (a JIT tier's compiler on a worklist thread, zlib, ...) that stall the JS thread,
    and the one-pause cycle moves or adds one. NOT CONFIRMED.
  * diag v6 = bun commit 2d71faeec7: times the rows of png(), deflateSync and the rest; arms default x2(+1), upstream, useJIT=0 | useDFGJIT=0,
    useFTLJIT=0 | useConcurrentJIT=0, one marker | default; the slot of an arm rotates per batch (v5 had the default arms always in slots 0 and 1).
- 2026-10-22 06:50 UTC: diag v6 result (build 130758, bench/results/vb-diag-v6.log; 12 batches x 6, slots rotated; parser /tmp/parse-v6.py).
  * BUN_JSC_useJIT=0: png 716 ms (rows 58, deflate 508, rest 11), flat grey 1.6 s, whole file 17.1 s (default: png 3.1..3.6 s, flat 4.4..5.0 s at 6 concurrent,
    file 26.1 s). So under SDE the JIT tier-ups are what makes png() take seconds: row 2 or 3 of the loop takes 130 ms, row 6 takes 350..820 ms
    (png's loop OSR at 500 back edges), the IHDR chunk (first crc32) about 1.2 s, the rest 0.45..1.1 s. With useConcurrentJIT=0 the same rows spike
    (0:64 1:105 6:540) and the startup takes 23.4 s.
  * This time the default arm had NO slow run (0 of 30), upstream 0 of 12, one marker 0 of 6, no FTL 0 of 6. The no-DFG arm had 2 of 6:
    png 3.8..4.0 s against 1.4..1.8 s: +1.7 s between the end of the loop and deflateSync, and deflateSync itself 950 ms against 530 ms.
    That is a window of about 2.5 s in which the JS thread runs at about half speed: something else competes. Which thread is unknown.
  * The rate of the slow mode is erratic: v1/v2 3 of 17, v3 2 of 13, v5 8 of 28 in the default arms (slots 0 and 1) and 1 of 56 elsewhere, v6 2 of 72.
    Other builds (main WebKit): 1 of 108 (a "gradient" failure at 5.1 s).
  * DynamoRIO on Linux as a local stand-in for Pin: bun does not get through `-e 'console.log()'` in 2.5 minutes under it. Dropped.
  * BUN_JSC_reportCompileTimes=1 works in release builds (prints every Baseline/DFG/FTL compile with its time).
  * diag v7 = bun commit 86f944ce50: reportCompileTimes in every run, and a PowerShell sampler (thread-sampler.ps1, written by the harness) that prints every
    150 ms the threads of each bun-profile process that used 30 ms of CPU or more, with their names (GetThreadDescription) and the threads that came and went.
    Arms: default x4, no DFG, upstream, slots rotated. Timelines for slow runs (png > 4300) and the first 5 runs.
