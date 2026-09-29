# Notes for oven-sh/WebKit#735 and oven-sh/bun#44024 (tools and results, not for merge)

## State on Sep 29, 11:00 UTC

- oven-sh/WebKit#735: ready for review, base main (f20ce77445), head 867429ea5d, CI green. Preview release autobuild-preview-pr-735-867429ea.
- oven-sh/bun#44024: head 0eeee6d747, pins that preview release. Draft until #735 merges.
- Defaults: missCountForLLIntTierUp=12, useLLIntStringLengthFastPath=1. Off: useLLIntUnsetCaching, useLLIntPrototypeCacheRearming, useStartupJITDeferralAfterLLIntMisses.

## Open decisions of the maintainer

1. Does a function that the misses send to the Baseline JIT wait for the startup deferral scale (JIT policy)?
2. Do the unset cache and the rearming stay in the pull request, and with which default?
3. Does the credit apply with no startup deferral scale (it gains 0.3% of execution there, for 1.7% of compile work)?

## Next change (in progress)

- A limit of the bytecode cost for the credit (option maximumBytecodeCostForLLIntMissTierUp, 10000, 0 is no limit): a body that runs once, with a loop and a site that misses, was compiled whole.
- tryToSetUpGetByIdPrototypeCache() refuses a receiver with GetOwnPropertySlotIsImpure, as actionForCell() of the JIT does. main returns a stale value of the prototype for such a receiver.
- The count of the site is checked before the walk of the prototype chain for a global object.
- Tests: receiver from $vm.createImpureGetter, method read of 16 classes, for-of with iterators of 16 classes, a function above the limit.
- Bun: ProhibitsPropertyCaching for NodeVMSpecialSandbox, a fixture row with an inherited value, the wording of docs/bundler/executables.mdx, the JSDoc of setJITPolicy and jitPolicy in bun.d.ts.

## How to build and measure

- configure.sh, build-new.sh, build-main.sh: jsc shell builds (release, and release with assertions).
- run-modes.py: runs each //@ runDefault mode of a stress test. time-modes.py: CPU time of each mode (limit of JSTests/README.md: 200 ms).
- count.sh, count2.sh: calls of the three LLInt slow paths (gdb breakpoint hit counts), Baseline compiles, JIT bytes.
- dr-run2.sh, dr-run3.sh, dr-run4.sh with dr-client2: instructions of the main thread under DynamoRIO (cronbuild-11.91.20715), compile and collection counted apart. The shell needs the libpas scavenger off: make-noscav.py.
- multi.py with runstat.c: CPU time and RSS in interleaved runs.
- Workload: TypeScript 5.9.2, tsc-workload.js (40 files of Bun's src/js) and a copy with the first 4 files.
