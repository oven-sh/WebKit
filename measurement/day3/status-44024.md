**Status**

- Draft until oven-sh/WebKit#735 merges. It pins the preview build `autobuild-preview-pr-735-7da0e718` of oven-sh/WebKit#735. Before this merges, the pin must be the merge commit of oven-sh/WebKit#735.
- Checked with a debug build against the preview: `bun bd test test/js/bun/jsc/llint-get-by-id-caches.test.ts` (13 pass), `bun bd test test/js/node/vm/vm.test.ts` (309 pass) and `bun bd test test/js/bun/compile/jit-policy.test.ts` (6 pass).
- The two new tests in `vm.test.ts` fail with Bun 1.4.3, and with a debug build of this branch that does not have the new structure flag of `NodeVMSpecialSandbox`.
- To get the behaviour before the change in one binary, set `BUN_JSC_missCountForLLIntTierUp=0 BUN_JSC_useLLIntStringLengthFastPath=0`.
- CI build 121609 (`bdfc13b0`): the one test that fails on each try is `test/js/bun/s3/s3.test.ts`. It cannot pull its `minio` image (`quay.io` answers 401), also on `main`. It is not from this change.
