**Status**

- Ready for review, on `main` (`f20ce77445`), head `7da0e718d4`. CI is green on every lane, JSTests on x86_64 and arm64.
- Preview release of the head: [`autobuild-preview-pr-735-7da0e718`](https://github.com/oven-sh/WebKit/releases/tag/autobuild-preview-pr-735-7da0e718). oven-sh/bun#44024 pins it.
- Defaults: `missCountForLLIntTierUp=12`, `maximumBytecodeCostForLLIntMissTierUp=10000`, `useStartupJITDeferralAfterLLIntMisses=1` and `useLLIntStringLengthFastPath=1`. `useLLIntUnsetCaching` and `useLLIntPrototypeCacheRearming` are off.
- The last two commits are from a second review of the change. A body that runs once is no longer compiled for a loop in it (limit of the bytecode cost). The prototype load cache of the LLInt refuses a receiver with `GetOwnPropertySlotIsImpure`, which `main` cached with a stale result. The startup deferral scale applies after the misses by default.
- Three decisions are open for the maintainer. They are in the Notes of the description, with the numbers: the startup deferral after the misses, the two options that are off, and the misses with no scale.
- Measured with the TypeScript compiler and with React server rendering in the `jsc` shell, and with a bundle in Bun: calls of the slow paths, compiles, JIT code, memory, and instructions of the main thread with the JIT.
