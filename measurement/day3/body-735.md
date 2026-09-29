### Problem
- An LLInt `get_by_id` site caches one structure. A site that sees several calls `llint_slow_path_get_by_id` each time, with no credit toward the Baseline JIT.
- The cache never holds an absent property (`performLLIntGetByID`, `llint/LLIntSlowPaths.cpp`) or a string length. It holds a prototype value once for each site.

### Fix
- `missCountForLLIntTierUp` (12): the 12th slow path call of one `get_by_id` or `put_by_id` site lowers the CodeBlock's threshold to that of `jitSoon()`, keeps the count, and ends its startup deferral.
- `useLLIntStringLengthFastPath` (on) reads a string's length where `get_length` misses.
- Off by default: `useLLIntUnsetCaching` caches an absent property, and `useLLIntPrototypeCacheRearming` lets a site make its cache 4 times.
- Verified: five new `JSTests/stress/llint-*.js` tests, JSTests with and without the JIT.

### Background
- The LLInt is the interpreter. A `get_by_id` site has 16 bytes of metadata. The first commit keys its watchpoints by site.
- A watchpoint runs code when a structure changes. The execution counter decides when the Baseline JIT compiles a function.
- Considered a 16-bit offset and 24 bytes of metadata. The counts are in bytes that only ProtoLoad mode uses.

### Downsides
- A small `tsc` run at a deferral scale of 10 has 423 more compiles. They run 4.3% more instructions and make 2.1 MB more JIT code. The rest of the main thread runs 5.3% fewer. `useStartupJITDeferralAfterLLIntMisses` makes it 282, 2.9% and 4.6%.
- The JIT's stale miss for `globalThis.x` after a `var x` (`main` has it) shows sooner in a function that tiers up early.
- A bucket of the watchpoint map has 24 bytes, not 16.

<details><summary>Notes</summary>

**Why.** Measured by @Jarred-Sumner on a large Bun application: `llint_slow_path_get_by_id` runs about 1.28 M times in a session, at about 54 k sites. 83% of the calls come from the 4% of sites that miss more than 100 times, and most repeat misses happen between execution counts 500 and 5000. A threshold of 500 for everything removes 71% of the calls, but it compiles everything earlier. This change sends only the functions that pay for the interpreter to the Baseline JIT early.

**The options.** `JSC_<option>=0` for `jsc`, `BUN_JSC_<option>=0` for Bun.

| Option | Default | What it does |
| --- | --- | --- |
| `missCountForLLIntTierUp` | 12 | The number of the miss that lowers the threshold. 0: misses are not counted. |
| `useStartupJITDeferralAfterLLIntMisses` | false | true: `VM::startupJITDeferralScale()` applies to the threshold that the misses lowered. |
| `useLLIntStringLengthFastPath` | true | The length of a string on the miss edge of `get_length`. |
| `useLLIntUnsetCaching` | false | The cache of an absent property. |
| `useLLIntPrototypeCacheRearming` | false | A site makes a prototype load or unset cache again. |

The last two are off by default: with the first and the third on, they change the slow path calls of the `tsc` workload and its Baseline compiles by 2% (table below), and the unset cache had a wrong result (next paragraph). A measurement on an application can turn them on.

**What the self-review found.** The unset cache trusted the structure flags of the class of an object. The object of a `node:vm` context in Bun (`vm.constants.DONT_CONTEXTIFY`) has its own `getOwnPropertySlot()`, which answers with what the global object of the context has, and its class set no flag that says so. A read returned `undefined` for a property that the context got after the second read of the site. The last commit changes the rule (see "What is not cached as absent"), and oven-sh/bun#44024 sets the flag. The same review asked for the new defaults and for the option of the startup deferral.

**The commits.** The first is groundwork, with no change in behaviour (it was oven-sh/WebKit#736, which is closed). Then one for each of the four options (tier-up, unset cache, string length, rearming). Then one that makes the slow path cheaper, one for the countdown and the tier-up test, one for the absence rule, the defaults and `useStartupJITDeferralAfterLLIntMisses`, and two for what the reviews of the pull request found. Each builds, and passes the tests that it has.

**The first commit.** `CodeBlock::m_llintGetByIdWatchpointMap` had the key `(StructureID, BytecodeIndex)`. A site could have more than one entry, and nothing found the entry of a site without the structure. The key is now the `BytecodeIndex`, which has the checkpoint (`iterator_next` and `instanceof` have two sites each), and the entry has the `StructureID` and the watchpoints. `CodeBlock::llintGetByIdModeMetadata()` finds the metadata of a site: the watchpoint and the collector each had that switch. `clearToDefaultModeWithoutCache()`, `setUnsetMode()` and `setArrayLengthMode()` set bytes 8 to 13 of `GetByIdModeMetadata` to zero, where a site that left ProtoLoad mode kept 6 bytes of a pointer. A watchpoint or a collection now clears the cache that it guards. Before, it cleared what the site had at that time, and both caches of `iterator_next` and `instanceof`. Only `$vm` and the time of a read show that. The tests that cover this code before the other commits: `llint-proto-get-by-id-cache-change-prototype.js`, `llint-proto-get-by-id-cache-intercept-value.js`, `llint-get-by-id-cache-prototype-load-from-dictionary.js` and `llint-cache-replace-then-cache-get-and-fold-then-invalidate.js`.

**The metadata.** `GetByIdModeMetadata` keeps its 16 bytes, and the first 8 are what they were:

```
before  structureID(4) cachedOffset(4) | cachedSlot(8)  or  unused(6) mode(1) hitCount(1)
after   structureID(4) cachedOffset(4) | cachedSlot(8)  or  unused(4) cacheSetupCount(1) missCount(1) mode(1) hitCount(1)
```

In ProtoLoad mode the second half is the pointer to the slot base, so the three counts of a site in that mode are in its entry of `CodeBlock::llintGetByIdWatchpointMap()`. The counts go with the site when the mode changes. `OpPutById::Metadata` keeps its 24 bytes: its count is in the padding before `m_structureChain` (a `static_assert` in `LLIntSlowPaths.cpp` says so).

**What the 12th miss does.** `ExecutionCounter::lowerThreshold()` makes the threshold of the LLInt counter that of `jitSoon()` (`thresholdForJITSoon`, 100) and keeps the count. A function that had 7 calls is so compiled at its next call. It does nothing when the CodeBlock is not in the LLInt, when `dontJITAnytimeSoon()` stopped the counter, when the Baseline JIT is off, or with `--useLLIntICs=0`. It marks the UnlinkedCodeBlock, which has the counter, and `checkIfJITThresholdReached()` does not apply `VM::startupJITDeferralScale()` to a CodeBlock of a marked UnlinkedCodeBlock. So a CodeBlock of the same code in another realm has the lower threshold too, and its check of the counter does not put the deferral back (tested). With `useStartupJITDeferralAfterLLIntMisses` there is no mark, so the lower threshold is `thresholdForJITSoon` times the scale. The memory pressure scale applies in both cases. A read that finds no property is not counted if the receiver is a global object, its proxy, or an object with one of them on its chain: see the end of these notes. The mark is a byte of its own in the UnlinkedCodeBlock (192 bytes, as before), because the collector writes the bits beside it from another thread. The count is for one site: a function with 40 sites that miss 4 times each is not sent anywhere (tested).

**What is not cached as absent.** A receiver or a chain object whose class has its own `getOwnPropertySlot()` (`OverridesGetOwnPropertySlot`), with or without the flags that say that the lookup is impure. `DFG::Graph::tryEnsureAbsence()` has the same rule. The inline cache of the Baseline JIT trusts the flags. The rule takes in a global object (a `var` of a later script becomes a property of the global object with no new structure) and a proxy. It also takes in arrays, functions, strings and typed arrays: a site with such a receiver has no unset cache. For the `tsc` workload, the unset cache alone has 259738 calls with the rule, and it had 259753 before the rule. Also not cached: a dictionary that was made flat before, and a chain with poly proto. Such a receiver does not use up the countdown.

**What bounds the work of rearming.** A site tries to make a prototype load or unset cache 4 times at most (`GetByIdSiteCounts::maxCacheSetupCount`), with or without success. Before, it tried once. A new cache replaces the entry of the site in the watchpoint map. A cache of an own property or of the length of an array removes it. A watchpoint that fires cannot remove its own entry, so that entry stays until a collection or until the site makes a cache again. Its other watchpoints do nothing to a site that has no cache with guards.

**Measurements.** x86_64 Linux, release builds of `main` (`f20ce77445`) and of this change. The counts are hit counts of gdb breakpoints. The instructions are those of the main thread, from a DynamoRIO client that counts for each thread.

The TypeScript compiler 5.9.2 checks and emits 40 files of Bun's `src/js` (1.08 MB) in the `jsc` shell, with `--useConcurrentJIT=0`, so that a run does not depend on the time that a compiler thread takes. "5000" is `--thresholdForJITAfterWarmUp=5000`. "Scale 10" is `--thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10`. The table is for the last commit (`4fcab1e7c2`). Two runs of one configuration differ by 0.2% at most.

| Configuration | `get_by_id` slow path | `get_length` slow path | `put_by_id` slow path | Baseline compiles | JIT code at the end |
| --- | --- | --- | --- | --- | --- |
| 5000, four options off | 280334 | 37578 | 56747 | 2505 | 14.1 MB |
| 5000, defaults | 37336 | 567 | 18520 | 2860 | 16.3 MB |
| 5000, four options on | 36110 | 569 | 18647 | 2800 | 15.9 MB |
| 500, four options off | 48055 | 4851 | 21817 | 3956 | 19.3 MB |
| 500, defaults | 30205 | 545 | 18269 | 4011 | 19.5 MB |
| Scale 10, four options off | 276329 | 37903 | 56705 | 2537 | 12.6 MB |
| Scale 10, defaults | 37123 | 566 | 18515 | 2917 | 14.2 MB |
| Scale 10, defaults, `useStartupJITDeferralAfterLLIntMisses` | 82024 | 576 | 27115 | 2777 | 13.4 MB |
| Scale 10, four options on | 35767 | 569 | 18632 | 2850 | 14.0 MB |
| 5000, `missCountForLLIntTierUp` alone | 37311 | 1825 | 18520 | 2873 | 16.2 MB |
| 5000, `useLLIntStringLengthFastPath` alone | 280398 | 611 | 56802 | 2504 | 14.1 MB |
| 5000, `useLLIntUnsetCaching` alone | 259738 | 37578 | 56747 | 2503 | 14.1 MB |
| 5000, `useLLIntPrototypeCacheRearming` alone | 276885 | 37578 | 56747 | 2504 | 14.1 MB |
| 5000, defaults, miss count 8 | 29574 | 556 | 17564 | 2915 | 16.5 MB |
| 5000, defaults, miss count 16 | 44691 | 570 | 19530 | 2818 | 16.1 MB |
| 5000, defaults, miss count 32 | 72293 | 581 | 22816 | 2751 | 15.8 MB |

`main` had 279968, 37603 and 56719 calls, 2496 Baseline compiles and 14.1 MB for "5000", measured with an earlier commit of this change, which had 280030, 37602, 56774, 2496 and 14.0 MB with the four options off.

With the defaults, a threshold of 5000 has fewer calls of the three slow paths (56 k) than a threshold of 500 has with the options off (75 k), with 355 more Baseline compiles and 2.2 MB more JIT code where the threshold of 500 has 1451 and 5.1 MB.

With a scale of 10, the misses take 380 functions out of the deferral (2917 compiles against 2537), for 85% fewer calls of the three slow paths. With `useStartupJITDeferralAfterLLIntMisses` it is 240 functions for 70% fewer calls.

With the concurrent compiler threads, the four options on, and counters in a build of an earlier commit in place of the debugger, three runs each: the calls of the slow paths of `get_by_id` and `get_length` go from 334 k to 350 k down to 43 k to 47 k with a threshold of 5000 (69 k to 71 k for a threshold of 500 with the options off), and from 326 k to 346 k down to 41 k to 112 k with a startup deferral scale of 10. The Baseline compiles go from 2501 to 2506 up to 2799 to 2806 (3964 to 3969 for a threshold of 500).

A call of the `get_by_id` slow path runs 17 to 23 more instructions than on `main` (table below, measured with no JIT). The numbers with the JIT include them.

**Instructions of the main thread, with the JIT.** A DynamoRIO client counts the instructions of each thread. With the JIT it works only with the compilers and the collector on the main thread (`--useConcurrentJIT=0 --useConcurrentGC=0 --numberOfGCMarkers=1`), and with the scavenger of libpas off. For that, the shell is a copy with the byte of `pas_scavenger_is_enabled` set to 0: under the client, the main thread never answers the signal with which the scavenger suspends it. The client also counts the instructions between the entry and the return of `JITWorklist::enqueue()` ("Compile": each compile of each tier) and of `Heap::runCurrentPhase()` ("Collection"). "Execution" is the rest. With the concurrent compilers, "Compile" is work of the compiler threads.

The small workload: the TypeScript compiler checks and emits the first 4 of the 40 files (91 KB). Two runs of one configuration differ by 0.04%. The percentages are against the row with the options off that has the same thresholds ("500" is against "5000").

| Configuration | Execution | Compile | Compiles | Collection | All |
| --- | --- | --- | --- | --- | --- |
| 5000, `main` | 3.461 G | 7.297 G | 3421 | 0.107 G | 10.866 G |
| 5000, four options off | 3.465 G | 7.299 G | 3421 | 0.107 G | 10.871 G |
| 5000, defaults | 3.256 G (-6.0%) | 8.018 G (+9.8%) | 3950 | 0.225 G | 11.499 G (+5.8%) |
| 5000, four options on | 3.266 G (-5.7%) | 7.986 G (+9.4%) | 3891 | 0.226 G | 11.478 G (+5.6%) |
| 500, four options off | 3.025 G (-12.7%) | 8.397 G (+15.0%) | 5289 | 0.228 G | 11.650 G (+7.2%) |
| Scale 10, `main` | 3.759 G | 3.976 G | 2628 | 0.106 G | 7.841 G |
| Scale 10, four options off | 3.764 G | 3.973 G | 2628 | 0.105 G | 7.842 G |
| Scale 10, defaults | 3.563 G (-5.3%) | 4.145 G (+4.3%) | 3051 | 0.107 G | 7.815 G (-0.3%) |
| Scale 10, defaults, `useStartupJITDeferralAfterLLIntMisses` | 3.592 G (-4.6%) | 4.089 G (+2.9%) | 2910 | 0.107 G | 7.788 G (-0.7%) |
| Scale 10, four options on | 3.572 G (-5.1%) | 4.137 G (+4.1%) | 3006 | 0.108 G | 7.816 G (-0.3%) |
| Scale 10, `missCountForLLIntTierUp` alone | 3.549 G (-5.7%) | 4.085 G (+2.8%) | 3069 | 0.107 G | 7.742 G (-1.3%) |
| Scale 10, `useLLIntStringLengthFastPath` alone | 3.757 G (-0.2%) | 3.973 G | 2628 | 0.105 G | 7.835 G (-0.1%) |
| Scale 10, `useLLIntUnsetCaching` and `useLLIntPrototypeCacheRearming` alone | 3.762 G (-0.0%) | 3.971 G | 2628 | 0.105 G | 7.838 G (-0.1%) |
| Scale 10, defaults, miss count 8 | 3.551 G (-5.7%) | 4.348 G (+9.5%) | 3126 | 0.108 G | 8.007 G (+2.1%) |
| Scale 10, defaults, miss count 24 | 3.584 G (-4.8%) | 4.062 G (+2.3%) | 2944 | 0.107 G | 7.754 G (-1.1%) |
| Scale 10, four options off, `--thresholdForJITAfterWarmUp=50` | 3.366 G (-10.6%) | 4.123 G (+3.8%) | 4057 | 0.219 G | 7.708 G (-1.7%) |

With the four options off, the build of this change runs 0.1% more instructions in "Execution" than `main`, and compiles the same functions.

With the JIT, the gain is that of `missCountForLLIntTierUp`. The string length alone gives 0.2%, and the unset cache and the rearming give nothing that the count shows: the calls that they take away are few instructions each, and a function that misses often leaves the LLInt. With no JIT the three give 2.8% (table below).

The last row is the other way to the same end: the scale, with a Baseline JIT threshold of 500 for each function. It gains twice what the misses gain in "Execution" (0.398 G against 0.201 G), with 1346 more Baseline compiles against 404 and 4.0 MB more JIT code against 2.1 MB (next table). For each MB of JIT code the two gain the same. "Compile" depends on which large functions get to the DFG and FTL JIT before the run ends: a few of them are the difference between the rows with the miss counts.

With a scale of 10, the misses give 27% of what "Execution" gains from no scale and a threshold of 500 (3.764 G to 3.025 G), for 4% of the "Compile" that those cost (3.973 G to 8.397 G). With a threshold of 5000 and no scale, a function that the misses send to the Baseline JIT also gets to the DFG JIT earlier, and most of the 0.72 G is from those compiles. The collection has one more cycle in the configurations with more than 0.2 G.

The slow paths and the JIT code of the small workload, counted as for the table above:

| Configuration | `get_by_id` slow path | `get_length` slow path | `put_by_id` slow path | Baseline compiles | JIT code at the end |
| --- | --- | --- | --- | --- | --- |
| 5000, four options off | 206983 | 32127 | 40289 | 1579 | 8.1 MB |
| 5000, defaults | 31128 | 498 | 17614 | 1962 | 9.6 MB |
| 500, four options off | 39407 | 4205 | 19783 | 2924 | 12.2 MB |
| Scale 10, four options off | 206977 | 32185 | 40138 | 1593 | 6.4 MB |
| Scale 10, defaults | 31050 | 498 | 17614 | 1997 | 8.5 MB |
| Scale 10, defaults, `useStartupJITDeferralAfterLLIntMisses` | 66113 | 507 | 23155 | 1862 | 7.9 MB |
| Scale 10, defaults, miss count 8 | 24999 | 489 | 16916 | 2069 | 8.8 MB |
| Scale 10, defaults, miss count 24 | 48026 | 508 | 19440 | 1896 | 8.1 MB |
| Scale 10, four options off, `--thresholdForJITAfterWarmUp=50` | 39212 | 4237 | 19813 | 2939 | 10.4 MB |

The workload with the 40 files, counted the same way. Here the DFG and FTL compilers have most of the instructions:

| Configuration | Execution | Compile | Compiles | Collection | All |
| --- | --- | --- | --- | --- | --- |
| 5000, four options off | 9.183 G | 26.592 G | 6045 | 0.458 G | 36.233 G |
| 5000, defaults | 8.619 G (-6.1%) | 27.888 G (+4.9%) | 6522 | 0.462 G | 36.969 G (+2.0%) |
| 5000, four options on | 8.639 G (-5.9%) | 27.672 G (+4.1%) | 6444 | 0.462 G | 36.773 G (+1.5%) |
| 500, four options off | 8.186 G (-10.9%) | 28.949 G (+8.9%) | 8165 | 0.458 G | 37.593 G (+3.8%) |
| Scale 10, four options off | 9.865 G | 27.207 G | 5101 | 0.457 G | 37.528 G |
| Scale 10, defaults | 9.513 G (-3.6%) | 26.792 G (-1.5%) | 5487 | 0.458 G | 36.763 G (-2.0%) |
| Scale 10, defaults, `useStartupJITDeferralAfterLLIntMisses` | 9.587 G (-2.8%) | 27.537 G (+1.2%) | 5346 | 0.457 G | 37.582 G (+0.1%) |

CPU time and memory of the workload with the 40 files, with the concurrent compiler threads. Seven runs of each configuration in turn, medians. The machine is shared and loaded: the first configuration ran twice in each turn, and its two medians differ by 8% for the main thread. No difference between two configurations is above that, so CPU time on this machine cannot show the effect.

| Configuration | Main thread, user CPU | JIT threads, CPU | Largest RSS |
| --- | --- | --- | --- |
| 5000, four options off | 4.59 s | 6.79 s | 375 MB |
| 5000, four options off, again | 4.94 s | 6.90 s | 378 MB |
| 5000, defaults | 5.23 s | 7.76 s | 386 MB |
| 5000, four options on | 4.83 s | 7.46 s | 386 MB |
| 500, four options off | 4.42 s | 7.22 s | 395 MB |
| Scale 10, four options off | 5.50 s | 6.75 s | 402 MB |
| Scale 10, defaults | 5.86 s | 7.13 s | 405 MB |
| Scale 10, defaults, `useStartupJITDeferralAfterLLIntMisses` | 5.57 s | 6.69 s | 405 MB |

One site, 2000 executions, `--useJIT=0`. Calls of the slow path of the site. The rows for an absent property are from `useLLIntUnsetCaching`, those for the length of a string from `useLLIntStringLengthFastPath`, and the 2 more calls of the prototype chain rows from `useLLIntPrototypeCacheRearming`:

| Site | `main` | Four options off | Four options on |
| --- | --- | --- | --- |
| Own property, 1 structure | 1 | 1 | 1 |
| Own property, 2, 3, 4, 8 or 32 structures | 2000 | 2000 | 2000 |
| Own property at offset 70000 | 1 | 1 | 1 |
| Absent, 1 structure | 2000 | 2000 | 2 |
| Absent, 2 structures | 2000 | 2000 | 1003 |
| Getter | 2000 | 2000 | 2000 |
| Prototype chain, 1 structure | 2 | 2 | 2 |
| Prototype chain, 2 structures | 1001 | 1001 | 1003 |
| `indexOf` of 3 arrays in turn | 1334 | 1334 | 1336 |
| Length of a string | 2000 | 2000 | 0 |
| Length of an array, a string and an object in turn | 2000 | 2000 | 1333 |
| `put_by_id`, 8 structures | 2000 | 2000 | 2000 |

Watchpoints that a site installs with the four options on, 2000 executions, `--useJIT=0`: 4 for two receivers in turn with the value on their prototypes (1 on `main`), and 4 for `indexOf` of three arrays in turn (1 on `main`). 200 sites that each read an absent property 10 times install 400 watchpoints and make 401 calls of the slow path, where `main` makes 2001 calls.

The generated LLInt (`LLIntAssembly.h`) has 60691 instructions where it had 60567. The 124 are the three copies of the miss edge of `get_length`. No other instruction changed. A `get_length` that misses for a receiver that is not a string runs 11 of them before the call of the slow path.

Instructions of the main thread, `--useJIT=0`:

| | `main` | Four options off | Four options on |
| --- | --- | --- | --- |
| First visit of a `get_by_id` site (own property), for each site | 4643 | +17 | +23 |
| First visit of a `put_by_id` site, for each site | 5054 | +6 | +11 |
| Miss at a site in ProtoLoad mode, for each miss | 474 | +28 | +79 |
| The TypeScript compiler | 59.07 G | 59.64 G (+0.9%) | 57.96 G (-1.9%) |

The instructions are from the commit before the last one. The last commit changes the slow path only where a lookup finds no property and `useLLIntUnsetCaching` is on.

The small workload with no JIT, the last commit, the collector on the main thread, and the slow paths counted apart. A site with more than one structure never leaves its slow path here:

| | `main` | Four options off | Defaults |
| --- | --- | --- | --- |
| All instructions | 11.245 G | 11.317 G (+0.6%) | 11.069 G (-1.6%) |
| In `llint_slow_path_get_by_id`, 3563868 calls | 1.306 G | 1.365 G (+17 a call) | 1.365 G |
| In `llint_slow_path_put_by_id`, 376065 calls | 0.274 G | 0.276 G (+5 a call) | 0.276 G |
| In `llint_slow_path_get_length`, 950884 calls | 0.238 G | 0.244 G (+7 a call) | 0.000 G (564 calls) |

**The designs that were not built.** A 16-bit offset with the counts beside it: it was built first, and a property at an offset above 65535 had no cache. 24 bytes of metadata for each site: more memory for each `get_by_id` of each CodeBlock.

**A second cache entry is not in this change.** With the JIT, the 12th miss ends the misses of a site. With the JIT off, a site whose receivers alternate keeps its misses: 2000 reads of an own property of 2 structures in turn make 2000 calls of the slow path, before and after. The metadata has 4 bytes free outside ProtoLoad mode, which is room for a second structure.

**Tests.** `llint-ic-miss-tier-up.js`: a function with a site that misses leaves the LLInt at call 12 to 14 with a threshold of 100000, also with a startup deferral scale of 50, and also when the read throws. With `useStartupJITDeferralAfterLLIntMisses` and a scale of 4 it leaves at the call that `thresholdForJITSoon` times 4 gives. A function of one source in two realms leaves the LLInt in both. A read that finds no property of the global object, or through it, stays in the LLInt. A function with one structure, and one whose 40 sites miss 4 times each, stay. The count of a site goes with it through a prototype load cache, a watchpoint, a cache of an own property and a collection. `llint-get-by-id-unset-cache.js`: 30 ways to make an absent property appear (on the chain, on `Object.prototype`, as a getter, by a new prototype, on a dictionary, through a proxy, on primitives of two realms, as a variable of a later script, through an object that answers for another object). It also checks that a receiver with its own `getOwnPropertySlot()` gets no unset cache. `llint-get-length-string.js`: ropes, substrings, two encodings, a site with strings and arrays. The count of the site shows that the reads did not call the slow path. `llint-proto-get-by-id-cache-rearm.js`: the cache after a transition of the receiver, after an own property, after a watchpoint, for arrays of each indexing type, and the limit of 4 tries. `llint-get-by-id-cache-random.js`: 6000 random steps, each read compared with a walk of the chain. Each configuration of the five tests takes 100 ms of CPU time at most on a release build (`JSTests/README.md` allows 200 ms). The tests read the state of a site through `$vm.llintGetByIdCaches`, `$vm.llintGetByIdCacheHits`, `$vm.llintGetByIdMissCounts` and `$vm.llintGetByIdCacheSetupCounts`.

**JSTests.** CI runs all the tests on x86_64 and arm64 Linux, and both are green for the last commit. The table below is from the commit before the last one: `run-javascriptcore-tests` on a release build with assertions, x86_64 Linux, for the tests with one of these in their name: `llint`, `get-by-id`, `proto`, `unset`, `length`, `dictionar`, `inline-cache`, `poly`, `instanceof`, `iterator`, `for-of`, `put-by-id`, `tier`, `osr`, `string`, `array`, `global`, `missing`, `undefined`, `property`, `structure`, `watchpoint`, `jit`, `deferral`, `baseline`. The stress tests run with `--validateExceptionChecks=true` in every mode.

| Mode | Runs | Failures |
| --- | --- | --- |
| Default | 77503 | 5 |
| `JSC_useJIT=0` | 10625 | 10 |

`useLLIntUnsetCaching` and `useLLIntPrototypeCacheRearming` are off by default, so CI does not run the tests with them. The same tests with the two options on (`--env-vars`) and the last commit:

| Mode | Runs | Failures |
| --- | --- | --- |
| Default | 82493 | 15 |
| `JSC_useJIT=0`, `--no-jit-stress-tests` | 10592 | 10 |

The 10 are those of the table above. The 15 are `js/script-tests/JSON-parse-reviver.js` in 5 modes (a crash, the assertion below, or no end in 300 s) and `stress/bigint-inc-dec-in-place.js` in 10 modes (the shell uses 630 MB, and the runner allows 600 MB). Both fail with the two options off too. A build of `main` with assertions fails both on the same machine: the first in 12 of 12 runs with `--useLLInt=false`, as the build of this change does, and the second in 3 of 3.

No failure is from this change:

- Default: `js/script-tests/JSON-parse-reviver.js` in 5 modes, `ASSERTION FAILED: impl->state() != WeakImpl::State::Deallocated` (`jit/JITThunks.cpp:100`). The build of `main` hits it in 8 of 20 runs of that test with the same command, and this change in 6 of 20.
- `JSC_useJIT=0`: `stress/buffer-accessor-jit-*.js` test the JIT (9 runs), and `stress/proxy-set-failure-inline-cache.js` in bytecode cache mode hits `ASSERTION FAILED: addResult.isNewEntry` in `CachedBytecode::copyLeafExecutables`. Both are so on the build without this change.

The five new tests pass in each of their 29 modes on a release build with assertions. Each commit passes the tests that it has.

**Do the tests see a defect.** Eight builds of the commit before the last one, each with one defect made on purpose, and each fails a test:

| Defect | Test that fails |
| --- | --- |
| The watchpoints of an unset cache are not installed | `llint-get-by-id-unset-cache.js` |
| A watchpoint that fires leaves an unset cache in place | `llint-get-by-id-unset-cache.js` |
| A site that leaves a cache with guards loses its counts | `llint-proto-get-by-id-cache-rearm.js` |
| A site that leaves a cache with guards loses its miss count | `llint-ic-miss-tier-up.js` |
| A site has no limit of tries | `llint-proto-get-by-id-cache-rearm.js` |
| The startup deferral applies after the misses | `llint-ic-miss-tier-up.js` |
| The threshold counts from the last miss | `llint-ic-miss-tier-up.js` |
| The length of a rope is read as 0 | `llint-get-length-string.js`, `llint-get-by-id-cache-random.js` |

**Not from this change, found on the way.**

- The inline cache of the Baseline JIT keeps "no such property" for `globalThis.x` after a later script declares `var x`. A variable of a later script is a property of the global object, and the global object keeps its structure. So the unset cache of the LLInt does not cache a global object, as receiver or on the chain, and a read that finds no property does not count as a miss if the receiver is a global object or has one on its chain (both tested). With no concurrent compiler, `main` returns the stale `undefined` after 40 reads of the site, and so does this change. A function that the misses of another site send to the Baseline JIT gets there sooner.
- The inline cache of the Baseline JIT keeps "no such property" for the object of a `vm.constants.DONT_CONTEXTIFY` context in Bun, after the context got the property with no new structure of the object. Bun 1.4.3 returns the stale `undefined` when the read runs in JIT code. oven-sh/bun#44024 sets `GetOwnPropertySlotIsImpureForPropertyAbsence` for the class.
- The failures of the JSTests above.
- Two functions made by `new Function` from the same source share their code and their LLInt metadata. Two classes with the same shape share the structure of their prototypes until one of the prototypes changes, and the shared structure cannot be watched after that. The tests give each function a source of its own and each prototype a property of its own.

</details>










