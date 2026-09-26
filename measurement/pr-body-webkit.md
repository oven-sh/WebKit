### Problem
- An LLInt `get_by_id` site has one cache entry for one structure. A site whose receivers have several structures calls `llint_slow_path_get_by_id` again and again, and its function gets no credit toward the Baseline JIT for that. With `thresholdForJITAfterWarmUp=5000` the function stays in the LLInt for about 330 calls.
- The entry never holds an absent property: `GetByIdMode::Unset` has a fast path, but `performLLIntGetByID` (`llint/LLIntSlowPaths.cpp`) requires `!slot.isUnset()`. It never holds the length of a string. It holds a value of the prototype chain at most once for each site.

### Fix
- `missCountForLLIntTierUp` (12): the 12th slow path call of one `get_by_id` or `put_by_id` site calls `jitSoon()`. The function then waits for `thresholdForJITSoon`.
- `useLLIntUnsetCaching`: an absent property is cached by structure, with a watchpoint for its absence on each object of the chain. `useLLIntStringLengthCaching`: new mode `StringLength`, ropes included.
- `useLLIntPrototypeCacheRearming`: the countdown to a prototype or unset cache starts again when the cache is cleared or replaced. It stops after 255 misses of the site.
- Verified: four new `JSTests/stress/llint-*.js` tests, each with the options on and off. All of JSTests on an assert build, with the JIT and with `JSC_useJIT=0`.

### Background
- The LLInt is the interpreter. Each `get_by_id` has 16 bytes of metadata (`GetByIdModeMetadata`): a mode, a structure, an offset.
- A watchpoint runs code when a structure changes. The LLInt uses it to clear a cache whose prototype chain changed.
- The execution counter of a function counts calls and loop turns. At the threshold the Baseline JIT compiles the function.
- Considered 24 bytes of metadata, and a miss count for each CodeBlock. The first costs 8 bytes for each site. The second makes the first visits of a large function add up. The count is in 2 bytes that a 16-bit offset frees.

### Downsides
- More functions reach the Baseline JIT: NUMBERS.
- A property at an offset above 65535 is no longer cached in the LLInt, with the options off too.
- A site with a getter, or with a receiver that is not a cell, still misses on each call. The miss count sends its function to the Baseline JIT.

<details><summary>Notes</summary>

NOTES

</details>
