**Status**

- Draft until oven-sh/WebKit#735 merges. It pins the preview build `autobuild-preview-pr-735-867429ea` of oven-sh/WebKit#735. Before this merges, the pin must be the merge commit of oven-sh/WebKit#735.
- Checked with a debug build against the preview: `bun bd test test/js/bun/jsc/llint-get-by-id-caches.test.ts` (12 pass), `bun bd test test/js/node/vm/vm.test.ts` (308 pass) and `bun bd test test/js/bun/compile/jit-policy.test.ts` (6 pass).
- The new test in `vm.test.ts` fails with Bun 1.4.3, and with a debug build of this branch that does not have the new structure flag of `NodeVMSpecialSandbox`.
- To get the behaviour before the change in one binary, set `BUN_JSC_missCountForLLIntTierUp=0 BUN_JSC_useLLIntStringLengthFastPath=0`. The other options are off by default: `BUN_JSC_useLLIntUnsetCaching`, `BUN_JSC_useLLIntPrototypeCacheRearming`, `BUN_JSC_useStartupJITDeferralAfterLLIntMisses`.
