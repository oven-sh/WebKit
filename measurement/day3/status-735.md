**Status**

- Ready for review, on `main` (`f20ce77445`). It has the commit that was oven-sh/WebKit#736 as its first commit.
- Preview release of the head: [`autobuild-preview-pr-735-867429ea`](https://github.com/oven-sh/WebKit/releases/tag/autobuild-preview-pr-735-867429ea). oven-sh/bun#44024 pins it.
- Defaults: `missCountForLLIntTierUp=12` and `useLLIntStringLengthFastPath=1` are on. `useLLIntUnsetCaching`, `useLLIntPrototypeCacheRearming` and `useStartupJITDeferralAfterLLIntMisses` are off.
- The last two commits have what the reviews found: the exemption from the startup deferral is with the counter, in a byte of its own (one code in two realms). A read that finds no property of a global object, or through one, does not count. The tests take 100 ms of CPU time at most.
- The commit before it fixes a wrong result of the unset cache: a read of the object of a `node:vm` context (`vm.constants.DONT_CONTEXTIFY`) in Bun returned `undefined` for a property that the context got after the second read. The unset cache now refuses an object whose class has its own `getOwnPropertySlot()`.
- Measured with the TypeScript compiler in the `jsc` shell: calls of the slow paths, compiles, JIT code, and instructions of the main thread with the JIT. The numbers are in the Notes of the description.
