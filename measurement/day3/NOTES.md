# Notes for oven-sh/WebKit#735 and oven-sh/bun#44024 (tools and results, not for merge)

## State on Sep 29, 15:15 UTC

- oven-sh/WebKit#735: ready for review, base main (f20ce77445), head 7da0e718d4, CI green. Preview release autobuild-preview-pr-735-7da0e718.
- oven-sh/bun#44024: head bdfc13b0a8, pins that preview release. Draft until #735 merges. Its CI is red on test/js/bun/s3/s3.test.ts only (the minio image cannot be pulled), which main has too.
- Defaults: missCountForLLIntTierUp=12, maximumBytecodeCostForLLIntMissTierUp=10000, useStartupJITDeferralAfterLLIntMisses=1, useLLIntStringLengthFastPath=1. Off: useLLIntUnsetCaching, useLLIntPrototypeCacheRearming.
- All review threads on #735 were resolved on Sep 29, 07:30 UTC. The review bots look at each push.

## Open decisions of the maintainer

1. The startup deferral after the misses (default: it applies).
2. The unset cache and the rearming: off, or out of the pull request.
3. The misses with no startup deferral scale: counted now. The gain is 0.3% there, for 1.9% more compile work.

## After a merge of #735

- Pin the merge commit in scripts/build/deps/webkit.ts of Bun when the release autobuild-<sha> exists.
- Commit message and body of the Bun pull request: no "preview".
- Tests: test/js/bun/jsc/llint-get-by-id-caches.test.ts, test/js/node/vm/vm.test.ts, test/js/bun/compile/jit-policy.test.ts.
- When oven-sh/WebKit#738 is in main: the rule for a read that finds nothing of a global object can go (hasGlobalObjectOnChain in llint/LLIntSlowPaths.cpp), with its test blocks.

## How to build and measure

- configure.sh, build-new.sh, build-main.sh, build-both.sh: jsc shell builds (release, and release with assertions).
- run-modes.py: runs each //@ runDefault mode of a stress test. time-modes.py: CPU time of each mode (limit of JSTests/README.md: 200 ms).
- count.sh, count2.sh: calls of the three LLInt slow paths (gdb breakpoint hit counts), Baseline compiles, JIT bytes.
- dr-run2.sh, dr-run3.sh, dr-run4.sh with dr-client2: instructions of the main thread under DynamoRIO (cronbuild-11.91.20715), compile and collection counted apart. The shell needs the libpas scavenger off: make-noscav.py.
- rss-batch.py with rss-end.js: memory of the process when the workload ends.
- multi.py with runstat.c: CPU time and RSS in interleaved runs.
- Workload: TypeScript 5.9.2, tsc-workload.js (40 files of Bun's src/js) and tsc-small.js (the first 4 files).
