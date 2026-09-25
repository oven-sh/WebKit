# Design proposals: what still separates each configuration from `main`, and how to remove it

This document is the output of the eleventh session of the landing work, a design phase with no code changes. It
answers two questions for each of the three configurations (flag off; GIL on, `--useJSThreads=1`; GIL off,
`JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0` on top of the flag): exactly what still differs
from `main` in behaviour, safety or performance after the tenth round, and what change would remove each difference.

**Status of everything in here: proposed.** Nothing in this document is implemented. The SPEC files and their history
files stay normative for what the tree does; a proposal moves into a SPEC (with its argument into the history file)
in the session that implements it, spec first, as in every round. Where this session's reading found that a SPEC, a
history section, an audit row or the landing plan no longer matches the code, the document was corrected in place and
the correction is listed in the section's "Doc mismatches" part.

How it is organized:

- Part 1 is the inventory: one row per remaining bottleneck or behaviour difference, per configuration, with its
  mechanism in one line, what it is worth, whether the mechanism is established, and the section that carries the
  evidence and the design.
- Part 2 is the ranked implementation plan for the next session. Its order and its expected gains are superseded by
  `PARITY-PLAN.md`, written after this document from measurements of an experiment build: gains here are in whole-process
  instructions and are larger than their effect on time (polls and tags in particular), C-D3 and the flag-on tax moved
  up, the entry points for RegExp and the long tail of section L became the largest item, and two defects were added
  (section C-1's exit feedback, which makes stanford-crypto-pbkdf2 five times slower under load, and a property-table
  rebuild race found while chasing section I's unexplained wrong value).
- Part 3 lists the decisions that are the project owner's, each with its options, costs and a recommendation.
- Sections A to N are the investigations themselves. Each has its own inventory with the evidence next to every entry
  (file and function in the current tree, the dump, count or disassembly that shows the mechanism, and the command in
  generic form), the designs (each marked **Status: proposed**, written in SPEC terms: who writes, who reads, in which
  tiers; the memory-ordering argument for x86-64 and for arm64; the collector, the stop protocol, watchpoints and
  deferred claims; what a second thread can observe; what flag off sees; failure modes and their detection; tests with
  before/after counts; verification; expected gain with the number it is derived from; risks; alternatives rejected),
  the arm64 / non-Linux notes for its area, its decisions, its doc mismatches, and what was run.

Method. Everything was established by reading the current tree and by cheap measurements on binaries that already
existed (`main` at the branch's base, the tenth round's first and final trees, two experiment binaries of the tenth
round): OSR exit counts (`--printEachOSRExit`), `--verboseOSR`, DFG/FTL graph and disassembly dumps,
`--reportCompileTimes`, option dumps, `perf stat -e instructions:u`, `perf record -e instructions:u` on single tests,
`objdump` of single symbols in two binaries, hardware execute breakpoints for exact call counts
(`perf stat -e mem:<address>:x`), `describe()` and `$vm`. Nothing was built; no suite, campaign, sanitizer lane or Bun
run was made. Instruction and exit counts do not depend on machine load; no wall-clock number is used as evidence.
All numbers are Release, Linux x86-64.

Two measurement pitfalls found on the way, recorded because they silently produce wrong numbers:

- *Sampling periods.* On the measurement machine the kernel had lowered `perf_event_max_sample_rate` to 8,000/s.
  `perf record -e instructions:u -c N` with N of 1-2 M (the tenth round's period) then drops 25-50 % of the samples,
  and drops them where the instruction rate is highest - poll-dense GIL-off loops above all - so per-symbol and
  per-node shares taken that way under-represent exactly the code under study. A period of 4 M loses nothing on this
  machine (`perf report --stats` shows no throttle events and the event count equals `perf stat`'s total). Ratios
  between two configurations taken the same way are less affected than absolute shares. Two per-test instruction
  ratios quoted in PERF-RESULTS §6.12 (delta-blue 1.95, hash-map 1.61 GIL off over GIL on) are 1.61 and 1.66 by
  `perf stat` on the final tree; section B has the details.
- *Shell word splitting.* Under zsh an unquoted variable is not split into words, so `G="JSC_a=1 JSC_b=1"; env $G jsc`
  sets one variable with a garbage value and the run is silently GIL on. Several first measurements of this session
  were taken that way, were recognized by their equal counts in both modes, and were retaken with the variables written
  literally (or through a bash wrapper) and verified with `--dumpOptions` (`useThreadGIL=false`,
  `useTaggedButterflies=true`, `usePollingTraps=true`). The helper scripts of the rounds are bash and were not
  affected.

## Part 1. Inventory: what still separates each configuration from `main`

One row per remaining bottleneck or behaviour difference. "Status" says whether the mechanism is established
(explained: a measurement or a code path shows it; partly; not explained). "Where" is the section that holds the
evidence and the design. Worth is in the units the rounds use: JetStream 2 score ratios from the tenth round's quiet
pass (flag off 0.97-0.98 of `main`, GIL on 0.958 of flag off, GIL off 0.804 of GIL on), instruction ratios, micro rows,
scaling factors, or numbers of test results that pass on `main`.

### 1.1 Flag off against `main`

JetStream 0.97-0.98 of `main`; instructions +1.82 % over the 36 tests (+4,006 samples of 220,329). Grouped by what the
symbol belongs to (E, first table): arrays / objects / structures runtime +1,001, strings +798, RegExp entry points +724,
marking +686, generated code +455, libc and unclassified +324, parser +170. The classes share one shape: a hot leaf
function gains one to twenty tests of a Config-page byte, each test's cold arm is inlined behind it, the function grows
1.2 to 4 times in bytes, the register allocator spills around the cold arms, and callers that used to inline the leaf
now call it. No single site is more than 0.2 % of the suite. Binary-wide: 26,150 static tests of the `gilOffProcess`
byte and 4,164 of `useTaggedButterflies`; text 35.26 MB against 30.50 MB.

| id | what | mechanism | worth | status | where |
|---|---|---|---|---|---|
| FO-1 | RegExp and string entry points | 17 (`exec`) to 53 (regexp `split`) Config-byte tests per call, 23 of them `RETURN_IF_EXCEPTION`'s in one split; `jsSubstring`, `JSObject::putDirectIndex`, the 8-bit `StringImpl` constructor, `RegExpObject::create` and (on most legs) the matching-context constructor pushed out of line | per call `exec` +5 %, `replace` +11 %, regexp `split` +14 %, string `split` +13 %; regexp 1.051, OfflineAssembler 1.076-1.096, UniPoker 1.04 instructions; +724 and about +350 of the string class | explained | F-I2, F-I3, E-6; F-D4, F-D5, F-D7 |
| FO-2 | marking: +6.4 % instructions per collection | +13 instructions per visited cell: the visit counters and the mark stack's `m_top` as relaxed atomics (5; the compiler no longer merges the loads or folds the read-modify-write), and `visitButterflyImpl`'s mode flag captured by reference plus a switch reshaped by a GIL-off-only case (8; not recorded before). The helper-pause checkpoint contributes nothing | +686 samples (0.31 % of the suite); splay +4.4 %, hash-map +2.5 %, pdfjs +2 % of their instructions | explained | E-1; E-D1, E-D2 |
| FO-3 | array and object creation and growth | `JSArray::tryCreate` 109 -> 124 instructions per call, all from three inlined callees' gates (`optimalContiguousVectorLength`, `allocateCell`, the constructor); the same three in every creation path | +174 and about +350 with the other creation paths; WSL, Basic, ML | explained | E-2; E-D3 |
| FO-4 | `array-int32-to-double-relabel` 1.18 | not the conversion: per-operation gates in three runtime calls per iteration (`operationArrayPushDouble` 1,297 -> 3,100 bytes, `operationEnsureDouble` 76 -> 320, the temporary `WatchpointSet` 44 -> 163); +78 instructions per iteration of 582 before the FTL, about +20 in the FTL steady state; the 200 k-iteration row measures mostly the lower tiers | one micro row | partly (per-call traces of two operations not obtained) | E-3, C-6, AG-4; E-D3, D-AG-5 |
| FO-5 | `sanitizeStackForVMImpl` | a Config-byte test in the assembly routine, 16 -> 20 instructions per call | +85 samples | explained | E-4; E-D4 |
| FO-6 | `resolveRope`, substring helpers, `getPropertySlot<false>`, `operationGetByValGaveUp` | sizes grew 1.15-1.55 times; two helpers out of line; reached mostly from FO-1's entry points | +160, +143, +114, +80 samples | partly (static sizes only) | E-6; with F-D4 |
| FO-7 | generated code +455 samples of 140,987 | by I1 only field-offset immediates differ; +0.3 % over single runs is inside tier-up timing variation; the LLInt's affected opcodes carry 1-4 Config-byte tests each (`op_get_by_id` 119 -> 198 static instructions) | +0.2 % of the suite, possibly noise | not explained | E-7 |
| FO-8 | parser +170 samples | no mode gates in the parser; atomization through the shared-table latch or code layout | +0.08 % | not examined | E-8 |
| FO-9 | mode gates that are function calls in a build that does not inline (Bun's Debug+ASAN build) | `Options::x()` -> `addressOfJSCConfig()` call chains; 71 `useSharedGCHeap()` and 131 `gilOffWithProcessGate()` call sites, the hot flag-off-reachable ones in `MarkedVector`, `CompleteSubspace`, `Heap.h`, `VM.h` | Bun `fetch-tcp-stress` +16.9 % instructions, about 3 points of it the call chains | explained | E-9; E-D5 |
| FO-10 | the FFI tests under the executable-allocation fuzzer | `ftl-eager-no-cjit` fails one `JITCompilationCanFail` allocation in ten; on `main` too | 9-11 suite results of noise in every configuration | explained | H1c; D-H1c |
| FO-11 | arm64: five unconditional acquire loads on per-function / per-compilation paths | an acquire load is `ldar` there, a plain `mov` on x86-64 where the sweeps were taken | unknown until measured on hardware | explained by reading | N9 |

If F-D4/F-D5, E-D1/E-D2, E-D3 and E-D4 are all done about +2,300 of the +4,006 samples are recovered, leaving roughly
+0.8 % instructions: flag off at about 0.99 of `main`, with no margin. Behaviour flag off: no difference from `main` in
the JSC suites or Bun beyond time budgets (tenth round).

### 1.2 GIL on against flag off

Instructions GIL on over flag off: 1.015 in sum over the 36 tests. Of the ten JetStream rows that were below 0.95 in
the quiet pass, four are real by instruction count (WSL 1.09, Air 1.054, earley-boyer 1.05, typescript 1.017), the
others are equal by instruction count or bimodal at the same rate in every configuration (M "Verdict per row").

| id | what | mechanism | worth | status | where |
|---|---|---|---|---|---|
| GO-1 | property inline caches in FTL code are handler chains | the flag forces `useHandlerICInFTL`; every site is a data IC walking a handler chain and clobbering every volatile register, where flag off patches one stub per site | WSL +4 to +9 %, Air +3.4 %, typescript +1.5 % instructions; class-ctor-4 23 of its 33 points. The earlier "a wash" rested on Babylon, which is bimodal flag off | explained | M-I1; D-M1 |
| GO-2 | inlined transitions keep a fence; `create_this` re-reads its pair | two gates history §52 meant to re-key to the tagged-word option and did not; the B3 fence stops dead-store elimination of intermediate structure-ID stores | transition-heavy-constructor 1.24 (86 -> 108 Air lines; twelve structure-ID stores where flag off keeps one) | explained | M-I2; D-M2 |
| GO-3 | every string pays locked read-modify-writes | shared-atom-table mode: `StringImpl::deref` always `lock xadd`, `cost()` and `setHash` `fetch_or` | map-set-get 1.19 and regexp-exec 1.13 (identical instruction counts, 94 M and 18 M more locked operations); 0.7-2.3 % of cycles on string-heavy tests; the same GIL off | explained | M-I4, F-I6; D-M4 (= F-D6) |
| GO-4 | calls go through call-link records; known callees are data ICs | SPEC-jit §5.8 in every tier | +5 instructions per non-inlined call (FTL), +6 LLInt, +8 `new`; richards 1.02, delta-blue 1.01, add-props-escaped 1.06 | explained | M-I3; D-M3 step 2 (last) |
| GO-5 | the FTL loses its strength-reduction call conversions | upstream's early break on handler ICs in FTL plans | +27.7 instructions per call where it bites; an FFI call in FTL code 90 against 19; `CallWasm`: richards-wasm 1.46x | explained | M-I3(c), M-I7, H1b; D-M3 step 1 (= D-H1b), G D3.3 |
| GO-6 | a put to a global property, an implicit global, a late `let` | scope metadata frozen after link with the flag | 14x, 27x, 11.6x per operation; nothing in JetStream | explained | M-I5; D-M5 |
| GO-7 | dictionary objects and the inlined prototype-load form | inline caches refuse dictionary structures with the flag | 5.9x per read, 7.5x per replace on an object after `delete`; nothing in JetStream | explained | M-I6; D-M6 |
| GO-8 | navier-stokes lands in its slow timing mode | same code (identical counts under a synchronous JIT); which FTL version of `lin_solve` survives depends on concurrent-compile timing | 0.846 on one row, 0.45 % of the mean; GIL on 0 of 14 runs in the fast mode, flag off 5 of 5 | partly (why GIL on never reaches it is not established) | M-I10 |
| GO-9 | earley-boyer +5 % instructions | not located (moves both ways between runs; bimodal on `main`) | one row | not explained | M-I10 |
| GO-10 | atomization through the shared table; Baseline profile write avoidance | shard locks; compare-before-store | small; below noise | explained | M-I8, M-I9 |

With D-M1, D-M2, D-M4, D-M5, D-M6 done the expected instruction ratio is about 1.004-1.006 and JetStream about
0.975-0.985 of flag off.

### 1.3 GIL off against GIL on: performance

Decomposition first (L-0; fifteen JetStream tests, whole-test instructions, geometric mean): GIL off executes 1.30
times GIL on's instructions = 1.06 (the poll as an instruction sequence: GIL on with polling traps) x 1.05 (the
butterfly tag: GIL on with per-thread tags) x 1.16 (what only a GIL-off process does). A process that ran untagged
until its first spawn would win back the 1.05, not the 1.30.

| id | what | mechanism | worth | status | where |
|---|---|---|---|---|---|
| GF-1 | polls at the entries of inlined callees | an inlined callee's `op_check_traps` is a `CheckTraps` in the caller's loop, and a write of the loop's visibility set; the unroller clones header polls | 136 of delta-blue's 182 in-loop polls; raytrace 11 %, delta-blue 10 %, richards 6 % of FTL instructions; micro rows int-loop 1.65, inline-property-read 2.00, flat-butterfly-read 2.01, proto-method-calls 1.70 are polls only | explained | B-1, L-2; D-B1 (= L-D1), D-B3 |
| GF-2 | the visibility rule proper | every control-deciding read of a loop is performed again after each poll, with the checks that hang off the re-read value; half of the re-reads are second copies behind inlined-entry polls | releasing every read: delta-blue -15.5 %, hash-map -14 % instructions (40 % and 35 % of their gaps), 2-6 % on seven tests; crypto's `am3` | explained | B-2; D-B2, then D-B5 or D-B6 (decision 1) |
| GF-3 | `PutStack` writes the whole heap | the parkable-slow-path predicate's default arm includes every `doesGC`-true node; the poll analysis gives up on any loop containing one | 126 of delta-blue's 182 in-loop polls keep the interim set; why history §50 moved delta-blue by nothing | explained | B-3; D-B4 |
| GF-4 | the Double family | Int32->Double served as Int32->Contiguous (T4-O), copies of Double sources boxed (T4-C); a failed check on a merged local reports to no profile | sha256 0.40/0.72 (bimodal), ML 0.56, navier-stokes 2.68x instructions, aes and pbkdf2 in part, BadIndexingType storms; about +4 % of the GIL-off total | explained (typescript's storm partly) | C; C-D1, C-D2, C-D3 |
| GF-5 | array growth | the store is re-dispatched three times around one copy (960 against 360 C++ instructions per event); growth garbage makes gbemu collect 45 times against 17 | aes -8 %, sha256 -2.8 %, pbkdf2 -2.5 % instructions | explained | D (AG-1), J7; D-AG-1 |
| GF-6 | RegExp and strings | per-thread scratch, legacy statics, slot and stack limit looked up per use; `RegExpTestInline`, folding and the cached-result record off; split's result array built through the owner-relabel protocol | regexp 0.79, OfflineAssembler 0.71 (6.9 % of its instructions in four lookups and a constructor), per call `test` 1.26, `replace` 1.29, regexp `split` 1.55 | explained | F-I4, F-I5, F-I7; F-D1, F-D2, F-D3, F-D7 |
| GF-7 | `Array.prototype.join` | the two-pass joiner is off GIL off | UniPoker 6.4 % | explained | F-I8, L-12; L-D10 (or F-D8) |
| GF-8 | the conductor sits out every cycle | a GIL-off process is a shared server from VM construction; the conducting mutator waits out the concurrent window with its access released | splay 0.67 (out of JavaScript 265 ms against 69 ms); one of N mutators idles per window in multi-threaded runs; Bun's main thread; Debug lanes | explained | J1, J2, J5, J10; D-J1 |
| GF-9 | optimized code that goes stale is re-entered from loops | `operationOptimize` consults the FTL replacement's exit counter but enters the superseded DFG block and its FTL-for-OSR-entry child | a stale loop: 2 exits on `main`, flag off, GIL on; 903 GIL off; `richards-like` with the withdrawn rule 2.4 -> 4.7 s and 5.6 -> 56 s | explained | A-2, A-3, K2; D2 (K) |
| GF-10 | forced exits for puts profiled by the LLInt | the transition cache is disabled with tagged words and the DFG reads the null fields as "never ran" | InadequateCoverage exits OfflineAssembler 82 -> 1,554, delta-blue 187 -> 564, pdfjs 21 -> 222, Babylon 20 -> 208; about 2 % of OfflineAssembler | explained | A-1; D-A1 (after GF-9's fix) |
| GF-11 | the interpreter's disabled caches | prototype loads, method calls and every property add take the slow path in the LLInt | +690 / +627 / +646 instructions per operation in the LLInt; first-iteration ratios 1.05-1.26; about 1.5 % of Air | explained | A-4, L-1, L-14; D-A2 (low priority) |
| GF-12 | `MakeAtomString` with a concat-key cache is a locked call | the inline probe is off for a reason the writer no longer has | 22 % of WSL's excess, 3.7 % of its instructions | explained | L-5; L-D4 |
| GF-13 | Map, Set, WeakMap | mutation and iteration as calls, WeakMap reads as host calls, `Map.get` below the FTL +250 instructions | WSL about 1,000 samples (13 % of its excess), Basic about 290 of 1,305; map-set-get 1.27 | explained | L-6; L-D8 |
| GF-14 | `CreateThis` on a poly-proto function | the prototype is looked up by name on every construction | class-ctor-4 2.67, astar-like-nodes 3.05 (mono-proto variants 1.5-1.8) | explained | L-8; L-D6 |
| GF-15 | (re)allocating transitions that leave generated code | per-case stubs and the megamorphic probe call C++ | megamorphic-put-transition 2.0; Baseline +20 per add | explained | L-9; L-D7 |
| GF-16 | math inline caches never regenerate | `JITMathIC::generateOutOfLine` returns early GIL off | 10 % of megamorphic-put-transition; not seen in JetStream | explained | L-10; L-D5 |
| GF-17 | structure tables under the structure's lock | the transition map and the property walk | json-parse-inspector 42 % of its excess; JSON.stringify 80 % | explained | L-11; L-D9 |
| GF-18 | per-access sequences | out-of-line write 9 instructions against 2; allocator through the thread's table 6 extra; `CheckTransitionOwner` on fresh objects 7 | a constructor-plus-six-adds loop 77 -> 127 instructions | explained | L-1, L-3, L-4; L-D3 |
| GF-19 | generators, async generators, promises | two compare-and-swaps per `next()`, out-of-line allocation, locked queues | Basic (cycles), async-fs 1.21 over tags and polls | partly | L-7, L-13; not designed |
| GF-20 | a thread body is entered once and recovers only through loop entry; the way back from one jettison costs 4^r | a failed loop entry is charged to the FTL replacement: 5 x (1 + N) x 2^r failures, each spaced by 1,000 x s x 2^r back-edges; `r` never decays (upstream's law, on `main` too: five serial invocations of `richardsWorkload` flag off 365 -> 2,498 ms) | richards-like T(1) 1,250 ms where a fresh copy of the function runs 485 ms; 30 M of 130 M back-edges in Baseline | explained | K1, K8; D2, D6 |
| GF-20b | an FTL loop-entry block that exits at its entry, every entry | LICM hoists a blind speculation (the `1 / expected` of an inlined, never-taken branch) into the OSR-entry block; `HoistingFailed` is learned only on the exit-count path | string-heavy's second warm-up 801 + 166 exits GIL off against 4 flag off; with GF-9 it is the slow mode | explained | K3; D3 |
| GF-20c | string-heavy's slow mode at four threads | a watched property-replacement fire's stop bumps the heap-fact epoch, every parked thread jettisons its on-stack optimized frames, and GF-9 + GF-20b keep all four in Baseline for the whole phase (1,361 entries, 1,361 exits, 148 M Baseline back-edges) | 2.4x or 0.9x at four threads (6 of 10 runs slow with counters on) | explained (which watchers the fired sets have is open) | K4; D4, D2, D3 |
| GF-20d | shared mutable state in lower-tier code | value-profile write avoidance is compiled out (`#if USE(JSVALUE64)`, a macro upstream removed: 67 unconditional profile stores in the Baseline code); the shared Baseline/DFG execution counters; polymorphic call stub slot counts | equal instructions per thread at 1/2/4 threads, 3.5 times the cycles; Baseline 50 -> 260 cycles per back-edge from one thread to two | explained in kind | K5; D1, D5 |
| GF-20e | the live-thread multiplier charges threads that share nothing | `exitCountThreadMultiplier()` = 1 + all live spawned threads, for every block and for entry failures too | private copies of one function: 1,266 / 2,392 / 3,555 ms at 1 / 2 / 4 threads | explained | K6; D6 |
| GF-21 | Eden pauses with N threads | the stop count is flat in N; the second window grows with N; a rendezvous costs about 0.04 ms per running mutator | 3-5 % of a parallel leg | explained | J4, J12; D-J3 (measure first) |
| GF-22 | segmented arrays in optimized code | DFG/FTL exit on a segmented word; the Baseline get_by_val cache never settles | 243.7 against 19.0 instructions per element read; nothing in JetStream | explained | AG-7; D-AG-3's note |
| GF-23 | a process that has not spawned runs tagged code | by construction | tags 1.05 of the 1.30 | explained | L-0; L-D2 (not recommended), C-D2 (one slice) |

### 1.4 GIL off: behaviour

| id | what | mechanism | worth | status | where |
|---|---|---|---|---|---|
| GB-1 | WebAssembly does not exist | forced off because the wasm tiers read VM-level words that are inert GIL off; no tier polls in loops | 117 suite results, `baselinejittrue`, Bun's 47 cases | explained | G (W1-W3, W11); D1, D2 |
| GB-2 | the FFI fast paths are off | nine VM-word sites and a bypassed refusal; nothing patches | 16 results, 1 Bun test; an FFI call 541 against 118 instructions | explained | H1; D-H1 |
| GB-3 | `--useProfiler` refused | `Profiler::Compilation` is not thread-safe reference counted; unlocked save | 2 results | explained | H2; D-H2 |
| GB-4 | `array-slice-cow` | T4-C seen through `$vm.indexingMode` | 16 results | explained | C-5; C-D2 / C-D3 |
| GB-5 | spawned threads are not sampled | one binding | profiles of threaded programs | explained | H3; D-H3 |
| GB-6 | `new Thread` in a second VM | the cap's RangeError for another reason | message | explained | H4; D-H4 |
| GB-7 | module evaluation on spawned threads has no claim | none of AUD1.K3 is implemented | measured: double evaluation, TDZ errors, TypeErrors; a vector race by reading | explained | H5; D-H5 |
| GB-8 | exit and compilation counts differ | GF-9, GF-10 | eight tests | explained | A |
| GB-9 | memory of structure-heavy programs | weak-bearing blocks recycled 32 per cycle end | string-heavy 478 MB against 88 MB at one thread | explained | J6; D-J2 |
| GB-10 | Bun: incomplete-body memory bound | suspected finalizer posting delay | one test | not explained (needs a counter in Bun) | J11; D-J5 |
| GB-11 | Bun: `node/vm` collection-heavy test at its limit | GF-8 in a Debug build | one test | explained | J10 |

### 1.5 Both flag-on modes: behaviour

| id | what | mechanism | worth | status | where |
|---|---|---|---|---|---|
| FB-1 | `Atomics` accepts ordinary objects | the API extension (SPEC-api 4.5): seven gates in `AtomicsObject.cpp` route a non-view object to the property path | 34 suite results (two files x 17 configurations) in both flag-on columns | explained; a decision | M-I11; D-M7 |
| FB-2 | a spawned Thread can run WebAssembly | `Interpreter::executeCall` and the microtask call reach `vmEntryToWasm` without the refusal | measured GIL on through three call paths | explained | W4; D3.1 |
| FB-3 | a wasm `memory.atomic.wait` parks holding the GIL | no `GILDroppedSection` | measured: a notifier on a spawned Thread wakes nobody | explained | W5; D3.1 |
| FB-4 | the FTL plants no `DirectCall`, `CallFFI`, `CallWasm` | upstream's early break on handler ICs in the FTL, which the flag implies | an FFI call in FTL code 90 against 19 instructions; +27.7 per call for a folded callee | explained | H1b; D-H1b |
| FB-5 | module continuations run on the settling thread | promise reactions run on the settler | the async parents' bodies run on a spawned thread | explained | H5 |
| FB-6 | the FFI tests under the allocation fuzzer | `ftl-eager-no-cjit` fails one allocation in ten | 9-11 results in every configuration, `main` included | explained | H1c; D-H1c |

### 1.6 Safety residue

| id | what | status | where |
|---|---|---|---|
| S-1 | a deleted slot reused inside a deferred-claim window (kind-unsafe) | explained by code path, not reproduced | I-1a; D-1(E) |
| S-2 | the loser of a deferred claim returns into its own stale code (value-safe at the remaining sites) | explained | I-1; D-1(F), (G) |
| S-3 | the claim observation can miss a claim on arm64 | explained by reading | I-1b |
| S-4 | the persistent wrong value under foreign churn | not explained; object-model legs cleared; allocator aliasing is the standing hypothesis | I-2; D-2 |
| S-5 | VMManager counters with Workers (R9-20) | explained | I-3; D-3 |
| S-6 | module evaluation: unlocked walks of `asyncParentModules()` against locked appends | by reading | H5; D-H5 |
| S-7 | arm64: two build breaks, inline-slot ordering, the virtual-call recompare, R7 gaps | explained by reading | N1-N4; N-D1 to N-D3 |

## Part 2. Ranked implementation plan for the next session

Ordering rule: defects before speed; among speed items, measured gain per unit of verification; items whose gain exists
only for a process that has not spawned are labelled as such. "Verification" names what each item needs beyond the
standing per-item battery of the rounds (corpus in four modes on Release and Debug, TSanJIT in both GIL modes, the
touched tests under the amplifier). Every item lands spec first (the SPEC section and a history entry with the
argument), then C++, then every tier, with a JSTests/threads test that shows the old count or the failure before and
passes after. Items inside a group are independent unless a dependency is stated.

### Group 0. Defects found in this session (behaviour and safety; small; land first)

| # | item | configurations | size | depends on | verification beyond the battery |
|---|---|---|---|---|---|
| 0.1 | WebAssembly refusal as a generated choke point in both shared JS->wasm entries; `memory.atomic.wait` inside a `GILDroppedSection` (G D3.1 = D1.4/D1.5) | GIL on, GIL off | small (`VMLite::isSpawned` byte, two entry checks, one bracket) | - | the six existing `api/wasm-*` tests; two new tests (every call path refused; the wait is woken) |
| 0.2 | Loop entry gets its own artifacts and accounting: `operationOptimize` consults the entered block's counters and its FTL child's, entry failures are no longer exits and carry neither 2^r nor the thread multiplier, a loop-entry DFG block is compiled on demand into the existing slot (K D2); an FTL loop-entry block that exits at its entry is an entry failure, and no blind LICM speculation into the OSR-entry block (K D3); the dead `#if` around value-profile write avoidance removed (K D1) | GIL off (D1: flag on) | medium: policy in `operationOptimize`, `tierUpCommon`, one callback class; the slot's publication becomes release/acquire | - | the in-tree stale-loop test (801 entries -> a handful; tighten its bound); `vmstate/loop-entry-when-replacement-is-ftl-gil-off.js` (bound 1,000 -> 50 refusals); the scaling gate; the no-concurrent-JIT configurations of the GIL-off stress suite |
| 0.3 | Precondition 10: no quarantine promotion while a claim is in flight (D-1(E)); the loser waits at scope exit (D-1(F)); acquire on the claim observation (I-1b) | GIL off | a few lines each | - | the precondition's CVE test extended with a delete and with same-thread rounds, fire entry perturbed under the amplifier |
| 0.4 | `VMManager`: a VM counted stopped is counted active (D-3) | GIL off with a second VM | small | write the shell test first (it confirms the diagnosis) | Bun `worker_threads` / `web/workers` under load, 50 runs |
| 0.5 | Module evaluation lock released at the GIL's hand-over points (D-H5) | GIL off (GIL-on parity by construction) | medium (one lock, five entries, one hook in `GILDroppedSection`) | - | three module tests; TSan on the parent-list walk |
| 0.6 | Weak-bearing blocks: sweep the new active sets' blocks at every Eden cycle end (D-J2 stopgap) | GIL off | small | - | gc-stress matrix; string-heavy peak resident set 478 -> about 145 MB; a peak-memory column in the scaling gate |
| 0.7 | Messages and test hygiene: `new Thread` in a second VM (D-H4); FFI tests tolerate the allocation fuzzer (D-H1c); mono-proto variants of two micro rows; a note under `array-int32-to-double-relabel` | all | trivial | - | - |
| 0.8 | The two no-build experiments on the persistent wrong value (D-2), before any protocol work in that area | GIL off | test-side only | - | an overnight amplifier run of the diagnostic variant |

### Group 1. GIL off performance that needs no ruling (ranked by expected effect on the GIL-off JetStream total)

| # | item | expected gain (derived from) | risk | depends on |
|---|---|---|---|---|
| 1.1 | Double family, first two steps: exit feedback for merged locals (C-D1) and in-place relabels / Double copies while no Thread has ever been spawned (C-D2) | about +4 % of the total (sha256 0.40 -> about 0.85, ML 0.56 -> about 0.80, navier-stokes 0.86 -> about 0.95; kernels 82 -> 43 and 43.7 -> 17.4 instructions per iteration); 16 `array-slice-cow` results. **Pre-spawn only for C-D2** | low (C++ only; one marker ordering rule) | - |
| 1.2 | Polls: no poll at an inlined callee's entry (D-B1 = L-D1), only the header poll refreshes (D-B2), one poll per unrolled trip (D-B3), `PutStack` is not a park site (D-B4) | -13 to -16 % instructions on delta-blue and hash-map; raytrace about -4 %, richards -2 %, poll-bound micro rows to 1.1-1.4; about +1.2 % of the total | low to medium (a semantics argument for D-B2) | - |
| 1.3 | Service conductor for long cycles and the lone-conductor rule (D-J1) | splay 0.67 -> 0.9-1.0 (the experiment's x1.34-1.52): +0.8 to +1.2 % of the total; the conducting mutator stops idling in multi-threaded runs | medium (collector battery; Bun's marshalling predicate must change first) | 0.6 helps its verification |
| 1.4 | RegExp: one per-thread context resolved once per entry point (F-D1), build split's result array directly (F-D7), then `RegExpTestInline` / folding / cached-result record GIL off (F-D2) | OfflineAssembler 1.311 -> about 1.19, regexp 1.150 -> about 1.09 in instructions; about +0.4 % of the total | low (F-D1, F-D7), medium (F-D2) | F-D2 needs F-D1 |
| 1.5 | One-pass owner append and growth (D-AG-1) | aes -8 %, sha256 -2.8 %, pbkdf2 -2.5 %, delta-blue -0.9 % instructions; fewer collections in gbemu; about +0.35 % | low (T1's protocol, order of three stores) | - |
| 1.6 | `MakeAtomString` inline probe (L-D4); `Array.prototype.join` from a private snapshot (L-D10) | WSL -3.7 %, UniPoker -6.4 % instructions; about +0.3 % | low | - |
| 1.7 | Transition profile word for the LLInt `put_by_id` (D-A1) | exit counts of eight tests at GIL on's; OfflineAssembler -2 % | low after 0.2; **it is the withdrawn rule's effect without 0.2** | 0.2 |
| 1.8 | The smaller emitter and C++ items: fused write predicate, allocator table, owner check on fresh objects (L-D3); poly-proto profile record (L-D6); reallocating transitions stay in generated code (L-D7); math inline caches (L-D5); Map/Set/WeakMap reads outside the FTL (L-D8); lock-free transition map and property walk (L-D9) | 1-2 % each on the tests they touch (raytrace, WSL, Basic, json-parse-inspector); micro rows class-ctor-4 2.3 -> about 1.2, megamorphic-put-transition 2.0 -> about 1.5 | low each; L-D9 touches weak-map finalization | - |

Expected after groups 0 and 1, from the per-item derivations (products of per-test ratios over 36 tests): GIL off /
GIL on 0.804 -> about 0.86-0.87, of which about four points exist only before the first spawn (C-D2). The thirteen rows
below 0.80 become at most four (delta-blue, hash-map, Basic, FlightPlanner's bimodality). The 0.90 target is not
reached without the visibility ruling (group 3.1: another +1.3 %) and what is left of tags and polls.

### Group 2. Flag off and GIL on

| # | item | expected gain | risk | note |
|---|---|---|---|---|
| 2.1 | GIL on: FTL repatching inline caches (D-M1), the two missed re-keys (D-M2), `StringImpl` last reference and fresh cost bit (D-M4 = F-D6), scope metadata and dictionary cases in `main`'s form (D-M5, D-M6), the strength-reduction break (D-M3 step 1 = D-H1b) | GIL on / flag off 1.015 -> about 1.005 in instructions, JetStream 0.958 -> about 0.975-0.985; two order-of-magnitude cliffs removed | low (all rest on G1's argument); D-M4 changes WTF code | one derived option "concurrent mutators" |
| 2.2 | Flag off: RegExp and string entry points instantiated per mode (F-D4) | regexp 1.051 -> about 1.01, OfflineAssembler 1.076 -> about 1.02 instructions | medium (a template parameter through a dozen headers; about +150 KB text) | start with the operations the JIT calls |
| 2.3 | Flag off: the marker's counters and `m_top` with one load per operation and atomic only under the sanitizer (E-D1); `visitButterflyImpl` instantiated per butterfly-word mode (E-D2) | marking back to within 0.5 % of `main` per collection (410 M -> about 388 M instructions per collection of the fixed graph; `main` 386 M): about 650 of the +4,006 samples, 0.30 % of the suite | low (E-D1 is `main`'s code; E-D2 needs TSan GIL off and a mirror pass) | independent of everything else |
| 2.4 | Flag off: array and object creation and growth paths instantiated per process mode, selected once per entry (E-D3); the stack-sanitizing slot chosen in C++ (E-D4); Config-page bytes for the `Options` gates flag-off code reaches (E-D5) | about 500 + 85 samples; `array-int32-to-double-relabel` 1.18 -> about 1.03; Bun's Debug lane loses the accessor call chains | medium (broad, mechanical, object-model entry points; three-configuration before/after per batch) | same recipe as 2.2; do 2.2 first |

### Group 3. Larger projects; each needs a decision first

| # | item | what it buys | decision |
|---|---|---|---|
| 3.1 | The visibility ruling: bounded staleness (D-B5) or full release (D-B6) | delta-blue and hash-map another -14 to -15 %, 2-6 % on seven tests | 1 |
| 3.2 | Encoding-changing shape transitions as copies with the raw-double bit (C-D3), staged in five steps | the Double family with threads alive; no Double->Contiguous stops; four special rules withdrawn | 6 |
| 3.3 | WebAssembly GIL off, carrier-only (G D1), then Wasm GC through the slow path (D2), then the GIL-on call path (D3.2, D3.3) | 100 suite results (117 with D2), Bun's 47 | 8 |
| 3.4 | FFI fast paths for the carrier (D-H1); `--useProfiler` (D-H2); sampling every thread (D-H3) | 17 + 2 results; correct profiles of threaded programs | 10, 11 |
| 3.5 | arm64: compile fixes (N-D1), load-acquire guard loads (N-D2), the virtual-call pair (N-D3), the fence split (N-D4) | an arm64 build; GIL on correct and close to x86-64's relative cost; GIL off only after hardware | 3, 4, 5 |
| 3.6 | Weak-bearing blocks, the two-half sweep (D-J2 proper); up to three concurrent windows with two or more mutators (D-J3, measure first) | the in-stop cell sweep gone; shorter second windows at eight threads | 13 |
| 3.7 | What is left of tier-up under N threads after 0.2: watchpoint-fire stops as code-lifecycle windows (K D4; re-opens SPEC-jit history §41 with the measurement it lacked), saturating call-slot counts and per-thread execution counters (K D5), the back-off hygiene of K D6 (one on-stack jettison episode counts once; the multiplier counts the threads that exited; `r` decays) | string-heavy always in its 2.4x mode; richards-like at four threads 1.15x -> 1.5x after 0.2, 3x or more with D5 | 19 |

Expected after group 2: flag off about 0.99 of `main` with no margin (about +2,300 of the +4,006 samples recovered, +0.8 %
instructions left, part of it possibly noise); GIL on about 0.975-0.985 of flag off.

### What can land independently

Everything in group 0 except 0.2 -> 1.7. In group 1: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6 and 1.8 touch disjoint code (object
model C++; DFG parser and phases; the collector; RegExp; `ConcurrentButterfly`; two operations; emitters). Group 2.1's
items share one new derived option and nothing else. 3.1 builds on 1.2; 3.2's first stage (the bit, always-true
validation) is behaviour-neutral by construction and can start beside anything.

## Part 3. Decisions that are the project owner's

Each decision names its options, what each costs and buys, and a recommendation. The sections cited carry the full
argument. Decisions 1-4 shape what the threads feature promises to programs; 5-12 are scope; 13-19 are engineering
choices with a visible side.

**1. The visibility rule: what a plain read inside a loop promises** (section B; SPEC-jit I21).
- (c0) Keep today's promise ("every read that can decide a loop's exit is performed again every iteration") and take
  the four refinements that need no ruling: D-B1 no poll at an inlined callee's entry, D-B2 only a loop's header poll
  refreshes, D-B3 one poll per trip of an unrolled loop, D-B4 `PutStack` is not a park site. Estimated -13 to -16 %
  instructions on delta-blue and hash-map (GIL off / GIL on 1.61 -> about 1.38 and 1.66 -> about 1.43), the poll-bound
  micro rows to 1.1-1.4. Low risk, four small changes.
- (c1) Bounded staleness on top of (c0), D-B5: the poll's fast path keeps nothing fresh, its slow path re-enters the
  loop through a refresh pre-header, a per-thread tick (1 ms proposed) bounds the wait. A spin on a plain flag still
  ends, within about a millisecond; no tier-dependent hangs. Another -14 to -15 % on those two tests (to about 1.27 and
  1.35) and 2-6 % on seven more. Costs a CFG-shaping phase in every GIL-off FTL plan, per-thread trap delivery to
  generated code (today's FTL poll tests the VM-level word), a ticker thread while spawned Threads live. The largest
  piece of work in section B.
- (c2) Full release, D-B6: plain reads may be hoisted for the loop's lifetime; signalling needs `Atomics` or a lock. Same
  performance as (c1), smallest implementation (delete code). It is what `main` already does for plain reads of
  SharedArrayBuffer memory (FTL code hoists them; measured) and what ECMA-262's shared-memory guidelines allow. Costs:
  a program that signals through a plain field works in the LLInt, Baseline and DFG tiers and hangs once the FTL
  compiles the loop; the corpus and scaling tests that do so move to `Atomics`; `Atomics` on ordinary objects becomes
  necessary (decision 2).
- Recommendation: (c0) in the next session whatever the ruling; for the ruling (c1), because it buys (c2)'s code without
  making correctness depend on which tier runs a loop and keeps the behaviour the branch has tested since the interim
  default. (c2) is defensible on precedent if (c1)'s phase is judged too invasive for this landing, and (c1) can follow
  it later without changing the contract.

**2. `Atomics` on ordinary objects behind its own option** (34 suite results in both flag-on modes; M-I11, D-M7).
`stress/SharedArrayBuffer.js` and `SharedArrayBuffer-opt.js` expect `Atomics.store({}, 0, 0)` and `Atomics.notify({}, 0, 0)`
to throw; with the extension the first creates the property and the second returns 0. (A) As now: part of the flag; the
two files differ by design in both flag-on columns. (B) Own option `useAtomicsOnObjects`, default following the flag:
programs see no change, the suites' flag-on lanes pass it off and then match `main` except for design items; one option
byte, seven re-keyed tests, a line in SPEC-api. (C) Own option, default off: flag-on suites match by default, programs
must opt in - breaks THREAD.md's API for every current user. Recommendation: (B). It interacts with decision 1: under
(c0)/(c1) the extension is optional (ordering, atomic read-modify-write, immediate visibility); under (c2) it is the
only lock-free way to signal through an object and must stay on by default.

**3. What racy plain accesses to different locations promise** (section N, decision 1). x86-64 gives programs TSO for
free: a thread that reads `a.length` and then `a[i]` never sees the second older than the first. (A) Portable model:
racy plain accesses to different locations are unordered (what arm64 already does for length-then-element, what Java
says for non-volatile fields); engine safety is kept by load-acquire guard loads only (N-D2, N-D3). Costs nothing on
x86-64, one `ldar` per guard on arm64; a program that relies on TSO without `Atomics` or a lock behaves differently on
arm64. (B) TSO everywhere: every plain heap read in generated code is an acquire on arm64; probably double-digit percent
there. Recommendation: (A), stated in SPEC-api next to the staleness model; it is the same kind of promise as
decision 1 and the two should be written together.

**4. x86-64-only rules versus portable ones** (section N, decision 2; its table N7). Today: the out-of-line Replace
inlined at the call site (TLS memory-operand `xor`), the FTL exit's published ramp, the rewritten global-scope cache,
the heap fence that follows marking. Each has a slower portable fallback. (A) Keep them and port by measured need once
hardware exists. (B) Forbid architecture-specific rules. Recommendation: (A) with one constraint - an x86-64-only rule
must have its fallback exercised by a test on x86-64 too (an option that disables the fast form), or the fallback rots;
today only one of the four is. No new x86-64-only rule is proposed by this document: the in-place vectorLength raise,
the one candidate, is proposed for rejection (decision 7).

**5. Whether arm64 GIL off is a landing requirement** (section N, decision 4). From the reading pass: two build breaks
(small), two unsafe orderings with designs touching every tier (N-D2, N-D3), one heap change with a likely large
performance effect (N-D4), then a first hardware campaign of unknown length. GIL on arm64 needs only the compile fixes
and N-D4's re-key. Options: both modes on arm64 before landing; or GIL on only, with GIL off refused on arm64 at option
validation until N-D2 and N-D3 have run on hardware. Recommendation: the second - one line, makes the unsafe rows
unreachable, mirrors how non-Linux is handled. Related (N decision 5): admit GIL on for macOS and Windows (a GIL-on
process no longer reads a thread-local slot, N-D5) only together with a first CI run there.

**6. How far to go for the Double family** (section C). (a) C-D1 only: sha256 stops being bimodal (0.40 -> about 0.72);
a day. (b) C-D1 + C-D2: about +4 % on the GIL-off JetStream total (navier-stokes about 0.95, ML about 0.80, sha256 about
0.85 of GIL on), the 16 `array-slice-cow` results; C++ only; buys nothing once a Thread exists, and must be reported as
such. (c) C-D3 with the raw-double bit in the butterfly word, staged: the same gains with threads alive, removes the
Double->Contiguous stops, withdraws four special rules; the largest sweep since tagged words (38 C++ reader functions
behind 7 choke points, four tiers, 33 word constructors), several weeks with its own campaigns. (d) C-D3 with the header
re-check of history §29 as recorded: same gains, dearer readers, an arm64 barrier. Recommendation: (b) now, (c) as its own
project starting with the behaviour-neutral plumbing stage; not (d). Sub-decision: C-D1 gated on tagged words here,
proposed upstream separately (the reduced reproducer storms on `main` too).

**7. Close the tenth round's decision (b), vectorLength growth** (section D). (A) In-place raise by the owner, x86-64-only
or portable with clear-at-birth: at most three lanes per butterfly, reopens the converter race the withdrawn T5 had.
(B) Segmented growth for owned arrays: O(1) growth, 13 times slower reads today. (C) Keep T1's copy and remove the
re-dispatch around it (D-AG-1): portable, no protocol change, -8 % on aes. Recommendation: (C); record that aes' remaining
distance is the Double family. Related: whether segmented arrays in the optimizing tiers (AG-7) are in scope - at least
the inline-cache give-up is recommended, the DFG array mode is a scaling-round item.

**8. WebAssembly with the flag on** (section G). Do stage 1 (carrier-only GIL off: 100 suite results, Bun's 47, a wasm
verification pass the rounds never had)? Recommended yes. Land the two defects found (spawned Threads reach wasm through
`call()` paths; the wasm atomic wait holds the GIL) first and alone, in both flag-on modes? Recommended yes. Refuse the
whole WebAssembly prototype surface on spawned Threads, or keep reads? Recommended uniform refusal in stage 1,
`memory.buffer` as the first relaxation if asked for. Explicit entry polls (recommended) or folded into the stack check.
Wasm GC GIL off through the slow-path allocator first (17 results; recommended after stage 1). `CallWasm` behind a
never-spawned watchpoint GIL on (recommended).

**9. Is single-threaded speed of a GIL-off process before its first spawn a goal?** (section L, L-D2.) (A) No: the
JetStream GIL-off gate keeps measuring tagged, synchronized code, effort goes to mechanisms that help threaded programs
too. (B) Tags only: about 5 % of instructions for a process-wide mode whose failure mode is memory unsafety. (C) Fully:
up to 0.95 of GIL on before the first spawn; about 980 gates to classify, two live copies of the object model GIL off,
and the JetStream gate must then run with an idle spawned thread to keep its meaning. Recommendation: (A) now; C-D2 is
the one slice worth taking because it needs no code invalidation at the spawn. Revisit (C) only if GIL off becomes the
only flag-on mode that ships.

**10. FFI** (section H). Give the carrier its stub and `CallFFI` back GIL off behind a TID-range gate (17 results, FFI
calls 4-25 times cheaper; recommended), or leave off. The refusal on spawned threads stays either way (standing
decision); what lifting it would need is listed in D-H1. Narrow the FTL's handler-IC early break so that it guards
`DirectCall` only (recommended; it is also D-M3 step 1, which shows the `DirectCall` half is already exercised as a data IC on every run). FFI tests tolerate the executable-allocation fuzzer
test-side (recommended; removes 9-11 noisy results from every suite comparison, `main` included).

**11. Sampling, the second-VM message, module evaluation** (section H). Sampling semantics once every thread is sampled:
wall-clock with a per-tick budget and a thread ID in every trace (recommended), or running threads only. `new Thread` in
a second VM: keep RangeError with a message that names the rule (recommended) or TypeError. Module evaluation: one
evaluation lock released at the GIL's hand-over points (recommended: GIL-on parity by construction), or refuse
evaluation off the carrier, or the per-record claim of AUD1.K3(c) (does not compose for overlapping graph walks).

**12. Safety residue** (section I). Precondition 10: (E) alone, (E)+(F) (recommended), or (E)+(F)+(G); then retire the
precondition and state consequence (b) as the staleness model's rule. The persistent wrong value: run the two no-build
experiments first, build the checked binary only if they leave slot confusion standing (recommended). The VMManager fix:
write the shell test first, land the fix with it (recommended).

**13. The collector** (section J). The service conductor in full (adaptive, with the lone-conductor rule; recommended), or
only the lone-conductor rule (a few lines, splay +4 %), or nothing; it needs Bun's marshalling predicate changed to "not
the VM's thread". Weak-bearing blocks: the stopgap now, the two-half sweep with its own campaign after it (recommended).
A peak-resident-set gate in the scaling suite (recommended: the 478 MB went unseen because the gate measures time only).

**14. RegExp and strings** (section F). Flag off: entry points instantiated per mode and selected per VM (F-D4:
`main`'s counts exactly, about +150 KB of text; recommended), or one gate per entry point and a clone, or hoisting only
the RegExp gates. Legacy statics stay per thread GIL off (SD19; F-D2 compiles that into generated code; recommended to
keep). The `StringImpl` last-reference shortcut (F-D6) changes WTF code that the embedder's own threads run in
shared-table mode; it buys the GIL-on `regexp-exec` row and about 18 cycles per temporary string flag on; recommended
with its Debug assertion. F-D7 (build a split's result array directly) changes code `main` runs; small; recommended.

**15. Where flag-on code polls** (D-B1 = L-D1). Inlined callee entries stop polling in every configuration that polls
with the flag on; stop and termination latency stay bounded by the machine function's straight-line length. Recommended
to accept.

**16. The LLInt caches** (section A). D-A1 (profile word, DFG-only reader) after the loop-entry fix: recommended. D-A2
(the caches back in the interpreter as immutable records): shelve until a workload shows interpreted-phase property
access GIL off as a cost.

**17. The micro set** (section L, decision 4). class-ctor-4 and astar-like-nodes measure the poly-proto slow path (the
harness re-creates the constructor in every timed call, in every configuration, `main` included). Keep them (they guard
L-D6) and add mono-proto variants so that the gate also measures constructor transitions. Note under
`array-int32-to-double-relabel` what it measures (C-6). Note that the "append by index" and "push" array rows contain no
growth event in steady state (AG-3).

**18. Flag off** (section E). (a) The marker's visit counters and the mark stack's `m_top`: plain accesses outside sanitizer
builds (restores `main`'s code exactly; the TSan lane keeps the atomic form and stands guard; recommended) or only
restructure to one load per operation (keeps relaxed atomics in every build, leaves 2 of the 5 instructions). (b) Is
flag off at 0.99 worth the breadth of E-D3 + F-D4 - some twenty runtime functions and the RegExp entry points
instantiated per mode, all shared with the flag-on configurations, each batch needing a three-configuration
before/after? Recommended: yes for F-D4 and the marker (two thirds of the gain, well bounded); E-D3 entry by entry with
a bench-gate row (an instruction count with `main`'s as the bound) for each. All of it together reaches about 0.99 with
no margin; nothing else found is worth more than 0.05 %.

**19. Tier-up of one shared function under N threads** (section K). (a) Is `scaling/richards-like.js` a fair gate? It
creates its constructors and helpers inside the workload function, so every invocation exits in the prologue of the
optimized code on `main` too (five serial invocations flag off: 365 -> 2,498 ms), and its T(1) is the third invocation's
recovery time. Recommended: hoist the inner functions out of the workload for the gate (the `raytrace-like` shape) and
add the present shape as a "thread body entered once" test bounded by Baseline back-edges per thread, not by time.
(b) K D4 (a watchpoint fire's stop does not bump the heap-fact epoch) changes a safety rule that was implemented and
withdrawn once (SPEC-jit history §41); recommended after K D2/D3, alone, with its own audit and campaign. (c) Per-thread
execution counters (K D5(b)) move tier-up from "hot in the process" to "hot on a thread"; recommended to measure after
K D1-D3 first, then take the folded form. (d) The LICM / `HoistingFailed` weakness (K D3) is upstream's and visible flag
off; carry a GIL-off gate here, offer the unconditional fix upstream separately.

## Section A. Property-adding puts profiled by the LLInt, and what the withdrawn generic-`PutById` rule ran into

Scope: SPEC-jit §4.3 (the LLInt `put_by_id` transition cache is disabled with tagged butterfly words), history §57 (the
parser rule that emitted the generic `PutById` there, tried and withdrawn in the tenth round because
`scaling/richards-like.js` went from 2.4 s to 4.7 s on one thread and from 5.6 s to 56 s on four, with a storm of BadCache
exits whose cause was not established), and §4.3's charter (the transition cache back as an immutable single-pointer
record). Everything below is Release, Linux x86-64. Exit counts and instruction counts do not depend on load.
GIL off is `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0` with `--useJSThreads=1`, the variables
written literally on the command line (a first set of runs passed them through an unquoted shell variable under zsh,
which does not word-split, ran GIL on and was discarded).

### A: Inventory

#### A-1. GIL off, a property-adding put in a function still in the LLInt forces an exit in its inliner (explained)

Configuration: GIL off (`useTaggedButterflies`). Flag off and GIL on run `main`'s cache.

Mechanism. `llint_slow_path_put_by_id` (`LLIntSlowPaths.cpp`) publishes the transition form of the cache
(`m_oldStructureID`, `m_offset`, `m_newStructureID`, `m_structureChain`) only when
`useUnthreadedLLIntPropertyCaches()`; with tagged words the fields stay null and the asm branch
(`.opPutByIdThreaded` in `LowLevelInterpreter64.asm`) knows only the one-word replace cache.
`PutByStatus::computeFromLLInt` reads a null `m_oldStructureID` as `NoInformation`, and
`ByteCodeParser::handlePutById` plants `ForceOSRExit` for a status that is not set. A small constructor inlined while
it is still in the LLInt therefore exits at its first `this.x = ...`, inside the callee, whose entry counter does not
move; the recompiled caller is told "never ran" again until the callee has been entered often enough from Baseline
code.

Evidence (final tree, one run per cell, `--printEachOSRExit=1`, InadequateCoverage exits GIL on -> GIL off):
OfflineAssembler 82 -> 1,554 (the top sites are `bc#39` of `parseExpressionAdd` 464, `parseExpressionAtom` 195,
`parseExpressionMul` 159, five more at 83-101: the first put after `super()` of inlined node constructors), delta-blue
187 -> 564, WSL 4,216 -> 4,621, pdfjs 21 -> 222, Babylon 20 -> 208, async-fs 101 -> 202, hash-map 104 -> 187, ai-astar
4 -> 40; total exits of those tests GIL on -> GIL off: OfflineAssembler 715 -> 2,309, delta-blue 1,506 -> 2,018, pdfjs
2,925 -> 3,825, Babylon 1,379 -> 1,782, async-fs 125 -> 342. Every other test of the 36 is within a few percent of its
GIL-on count in this kind.

Worth. The tenth round's experiment build (the rule on) measured OfflineAssembler at GIL on's exit count with
instructions -2 % and no score change. So: a behaviour difference in eight tests (exit and recompilation counts that
`main` does not have), about 2 % of OfflineAssembler's instructions, less elsewhere. It is also a trap for anyone
comparing exit counts between modes.

#### A-2. Why the generic-`PutById` rule made `richards-like` slow: optimized code that stays enterable after it has gone stale (explained)

The objects were never wrong. `describe()` of the five task objects at the end of each of the workload's three
invocations shows the same structures with the rule on and with it off: the workload declares its constructors inside
the workload function, so every invocation makes fresh function objects, fresh prototypes and fresh structures
(invocation 1 `{id..state}` 8/8 inline; invocation 2 the same shape under another structure ID; invocation 3, on the
spawned thread, the executable has gone poly-proto and the objects are 9/12 with a `PolyProto` slot). Optimized code
compiled in one invocation checks the previous invocation's structures and fails its first structure check in the
next one in every configuration, `main` included. What differs is how long the engine takes to notice.

1. *With `main`'s forced exit* the function-entry path of the workload function is cut in its prologue (the inlined
   constructors' puts are `ForceOSRExit`), so the only optimized code that contains the hot loops is an OSR-entry
   compilation. For such a compilation the loop header's expected values come from the entry's must-handle values
   alone, and `AbstractValue::mergeOSREntryValue` turns a clear abstract value into the constant it was given
   (`m_value = value`): the expectation for the scope register is the previous invocation's activation object. In the
   next invocation `AbstractValue::validateOSREntryValue` refuses the entry (`--verboseOSR`: "OSR failed because
   variable loc4 is Object ... JSLexicalEnvironment ... expected (OtherObj, TOP, [...]") 319 times; each failure is
   charged to the replacement's exit counter by `operationOptimize` and answered with an ordinary warm-up; at
   5 x 2 x 2^5 = 320 the loop trigger jettisons the replacement ("BaselineLoopReoptimizationTrigger") and the recompile
   sees the new structures. 1,602 BadCache exits all the same (801 each in the FTL code of the two small helper
   functions), 1.41 s.
2. *With the generic `PutById`* the prologue is live, the loop header merges the function-entry path with the entry
   values, its expectations are general, and the next invocation's loop entries succeed - into code whose first
   structure check fails. From there the defect of A-3 takes over: 1,600 exits in one DFG block and 915 in its
   FTL-for-OSR-entry child (`--verboseOSR`: 1,729 "Performing OSR ... (superseded DFG: the replacement is FTL)", each
   preceded by "Entered optimize ... exitCounter = 0"), every exit followed by a long warm-up (98,394 counter ticks) in
   Baseline. 4.3-4.7 s on one thread; with four threads the thresholds are 2.5 times higher and every jettison is a
   stop: 55-60 s.

So the rule removed an accident that was hiding A-3 from this workload. It did not change what the put path builds.

#### A-3. GIL off, code entered from a loop through the superseded DFG block is outside the loop reoptimization trigger (explained; on the final tree, independent of the rule)

Configuration: GIL off only.

Mechanism. `operationOptimize` (`JITOperations.cpp`), entered from a Baseline loop when an optimized replacement
exists, first asks `replacement->shouldReoptimizeFromLoopNow()` - the replacement's `osrExitCounter` against
5 x `exitCountThreadMultiplier()` x 2^`reoptimizationRetryCounter` - and jettisons when that holds. Flag off and GIL on
the block it then enters IS the replacement (or, if the replacement is FTL, the entry fails and the failure is charged
to the replacement with `countOSRExit()`), so a loop whose optimized code exits right after every entry trips the
trigger after a handful of rounds. GIL off, when the replacement is an FTL function-entry block, the eighth round's
rule enters `codeBlock->gilOffDFGForLoopEntry()` instead (the DFG block the FTL superseded, kept by
`ScriptExecutable::installCode`), and from that block's loop tier-up the thread enters the block's
FTL-for-OSR-entry child. Exits of those two blocks increment their own counters. The replacement never runs; its
counter stays where it was; the loop trigger never fires. The only way out is each exit stub's own test
(`DFG::handleExitCounts`): `osrExitCounter` above `exitCountThresholdForReoptimization()` = 100 x (1 + live spawned
threads) x 2^retry, per block, and every exit below it sets the Baseline counter to the long warm-up before the next
entry.

Evidence. A reproducer with no constructor and no LLInt put (three invocations of one function whose hot loop reads
`objs[i].a`, each invocation building its objects with another property order; added to the tree in its
characterising form as `JSTests/threads/jit/stale-loop-code-entered-through-superseded-dfg-gil-off.js`, which counts the
entries into the superseded block through the `loopEntryIntoSupersededDFGGILOff` counter: 0 GIL on, 801 GIL off),
final tree: `main`, flag off, GIL on: 2 BadCache exits, third invocation 172 ms. GIL off: 903 exits in every one of
three runs, third invocation 328-356 ms (GIL off runs this loop at about 1.2 times GIL on's instructions when nothing
is wrong, so the defect costs about 70 % here). 903 = 100 x 2 x 2^2 + the DFG/FTL split.

Worth. Any GIL-off program whose hot loop's speculation goes stale while an FTL replacement is installed pays
hundreds of exits times a long warm-up instead of a handful, per block, times the thread multiplier. It is the likely
mechanism of the slow mode of `scaling/string-heavy.js` at four threads and of the margin of
`vmstate/loop-entry-when-replacement-is-ftl-gil-off.js` (section K has the measurements and owns the fix). It gates
D-A1: removing the forced exits without fixing this moves `richards-like` from 1.25 s to 4.7 s.

#### A-4. What the disabled LLInt caches cost while code is still interpreted (explained; small)

`perf record -e instructions:u -c 4000000`, final tree, samples directly in the LLInt property slow paths (callees not
included): Air GIL on `performLLIntGetByID` 0.83 %, `llint_slow_path_put_by_id` 0.14 %, `setupGetByIdPrototypeCache`
0.07 %; GIL off `performLLIntGetByID` 1.24 %, `llint_slow_path_get_by_id` 0.79 %, `llint_slow_path_put_by_id` 0.45 %:
about 1.5 % more of a startup-heavy test's instructions, before counting `JSObject::get`/`putInline` under them.
OfflineAssembler: under 0.05 % in both modes. This is what an asm fast path for transitions and prototype loads
(D-A2) could win back: at most a few percent of first iterations, nothing in steady state.

### A: Designs

#### D-A1. A transition profile word for the LLInt `put_by_id`, written under the code block's lock and read only by the DFG

**Status: proposed.** Depends on K's fix of A-3 (must land after it, or together).

Rule (SPEC-jit §4.3, tagged processes). The `put_by_id` metadata keeps its size (24 bytes: the four bytes between
`m_newStructureID` and the 8-aligned `m_structureChain` are padding today) and gains `m_profiledOldStructureID` there.
In a tagged process the pair `{m_newStructureID, m_profiledOldStructureID}` is a *profile*: written by
`llint_slow_path_put_by_id` on exactly `main`'s conditions for caching a transition (cacheable put, `NewProperty`,
source not a dictionary and not `mayBePrototype`, `previousID() == oldStructure`, same out-of-line capacity, a
normalizable prototype chain without poly-proto) - minus the structure chain, which only the asm fast path needs.
Writer: any mutator, holding `codeBlock->m_lock` (`ConcurrentJSLocker`, as `main` does for its three-field write), two
relaxed 32-bit stores, then `vm.writeBarrier(codeBlock)`. Readers: `PutByStatus::computeFromLLInt`, which is only ever
called from `PutByStatus::computeFor` with that lock held; it builds `PutByVariant::transition(old, new, conditions,
offset)` from the pair exactly as it does from `main`'s fields (`m_offset` is not consulted there today either: the
offset comes from `newStructure->getConcurrently(uid)`). The asm never reads the pair: `.opPutByIdThreaded` keeps
testing the replace word `{m_oldStructureID, m_offset}` only, so a transition still takes the slow path in the LLInt
(A-4's cost stays). `CodeBlock::propagateTransitions` and `finalizeLLIntInlineCaches` treat the pair like `main`'s
(clear it when either structure is dead; both run with the world stopped or under the lock already).

What the DFG then does is unchanged code: with a set status the parser inlines the Transition variant if the four
thread-local sets of source and target are valid and watchable, and otherwise emits the generic `PutById`, whose inline
cache takes the claimed form (SPEC-jit §5.5 Transition). Neither forces an exit.

Memory ordering. None: both sides hold the same lock. The pair needs no single-word publication because no lock-free
reader exists; this is what makes it cheaper than the record form (D-A2) and is sufficient for what the DFG needs.

Collector. The structure IDs are weak, as on `main`; `finalizeLLIntInlineCaches` clears a pair that names a dead
structure at the end of the collection that found it dead, before the ID can be reused. Stops, watchpoints, deferred
claims: not involved (profiling only; the DFG's own admission rules decide what is compiled).

What a second thread can observe: another thread's slow path overwriting the pair (last writer wins, under the lock);
a compiler thread reads one writer's pair or the other's, never a mix.

Flag off: one more field in padding that already exists; no code reads or writes it (`useUnthreadedLLIntPropertyCaches()`
arm unchanged). GIL on: `main`'s cache, unchanged.

Failure modes. (1) The pair naming a structure pair that is not a transition of this site's identifier: the variant's
offset lookup fails (`getConcurrently` invalid) and the status degrades to `NoInformation`, today's behaviour. (2) A
stale pair after the site turned polymorphic: the DFG compiles a CheckStructure that fails and exits with BadCache,
the ordinary way profiles are corrected, provided A-3 is fixed. Detection: exit counts per kind in the tests below.

Tests. `jit/llint-profiled-transition-is-inlined-or-generic-gil-off.js`: a constructor kept in the LLInt (called fewer
times than the Baseline threshold per caller compile, many distinct callers), inlined into callers that reach the DFG;
counts InadequateCoverage exits at the constructor's first put and DFG compilations of the callers: before, one exit
per caller activation until the callee tiers up and 3-4 compilations per caller; after, 0 exits there and `main`'s
compilation count. Both modes, main thread and a spawned thread. JetStream evidence recorded in PERF-RESULTS:
InadequateCoverage counts of A-1 before/after (expected: OfflineAssembler 1,554 -> about 80, delta-blue 564 -> about
190, pdfjs 222 -> about 20, Babylon 208 -> about 20).

Verification. Corpus four modes; the scaling gate (it is what caught the first attempt - `richards-like` must stay at
1.25 s / 1 thread); the stale-loop test as a regression test for K's fix first; GIL-off JSC stress suite (DFG inlining
changes which code is compiled in every test that constructs objects from interpreted callees).

Expected gain. Exit counts of eight JetStream tests at GIL on's; OfflineAssembler -2 % instructions (tenth-round
measurement of the equivalent rule); delta-blue, pdfjs, Babylon under 1 % each. The main value is removing a
difference and a trap, not throughput.

Risk. Low once A-3 is fixed; without that fix this is the change that was withdrawn.

Alternatives rejected. The parser heuristic of history §57 (emit the generic `PutById` for an unset LLInt status): same
effect on exits, but it answers "no information" with a guess for every put, including ones that really never ran,
and it does not give the DFG the transition to inline. Publishing the pair with one 64-bit store and no lock: possible
(the two fields are one aligned word), needed only if a lock-free reader appears; the slow path is slow enough that
the lock is free.

#### D-A2. The transition cache (and the prototype-load cache) back in the LLInt as immutable single-pointer records

**Status: proposed, low priority** (A-4: about 1.5 % of instructions on the most startup-heavy test, nothing in
steady state). Written down because §4.3 charters it and so that it is not re-derived.

Record. `struct LLIntPutByIdTransitionRecord { StructureID oldStructureID; StructureID newStructureID; PropertyOffset
offset; uint32_t chainLength; StructureID chain[]; }` - pure data, allocated with `fastMalloc` at cache time, immutable
after publication; `chain[]` is a copy of the `StructureChain` vector `main` stores a cell pointer to, so the record
holds no collector-visible reference (structure IDs are weak and swept by `finalizeLLIntInlineCaches`, as today).
Placement: the 8-byte slot that is `m_structureChain` in an untagged process is the record pointer in a tagged one
(`useTaggedButterflies` is fixed at options finalization, so a process only ever has one interpretation; the metadata
type becomes a small wrapper with both views; offsets and size unchanged, D7).

Writer (`llint_slow_path_put_by_id`, tagged, `main`'s conditions plus the inline caches' refusals: source not
ArrayStorage-shaped, not copy-on-write; same capacity only). Under `codeBlock->m_lock`: build the record,
`storeStoreFence`, exchange it into the slot with one 64-bit store, `RetiredJITArtifacts::retire` the displaced record
(epoch reclamation, SPEC-jit §4.4: the asm readers' window has no poll, I16). The lock serializes writers and excludes
the DFG reader during the swap; it is not there for the asm readers.

Reader, LLInt asm (`.opPutByIdThreaded`, after the replace word misses): load the record pointer (null: slow path);
every further field through that pointer (F2: address-dependent, no fence on arm64); `oldStructureID` against the
cell's; the prototype chain walk of `main`'s loop against `chain[]`; then OM E4-C's claim-first sequence exactly as
the shared transition handler emits it GIL off: precise-allocation bit test (`cell & 8` -> slow path), one load of the
tagged word and the owner test `((tagged ^ tag) & mask) == 0` (butterfly-less words included), compare-and-swap of the
structure ID `S -> nuke(S)` (failure: slow path, nothing written), re-load and mask the word for an out-of-line
offset, store the value, store-store fence on targets that need one, store `S'`. Reader, DFG:
`PutByStatus::computeFromLLInt` under the lock, through the pointer. The prototype-load and unset `get_by_id` modes
follow the same pattern with a record `{structureID, holder slot address or null, conditions' structure IDs}`; their
invalidation stays the adaptive watchpoints `setupGetByIdPrototypeCache` installs (a fire runs in a stop and nulls the
slot; no asm window straddles a stop). One thing to establish before that half is built: the cached slot address of a
holder whose butterfly can be reallocated without a structure change (an object that also has indexed storage) -
`main` has the same question; if it is real there it is a bug to report upstream, not to copy.

arm64. The asm needs a 32-bit compare-and-swap (`batomicweakcasi` is x86-only in offlineasm; section N, N-D1, adds
the load-linked/store-conditional form) and the structure-ID load of the owner/claim sequence falls under N-D2.

Tests: the D-A1 test plus `llint_slow_path_put_by_id` call counts for a constructor-heavy interpreted phase (a `$vm`
counter or `perf stat` on the symbol): before, one call per put; after, one per (site, structure). Expected gain: A-4.
Risk: a second implementation of the claimed transition, in assembly; the reason to prefer D-A1 alone for now.

#### D-A3. Fix of A-3

Owned by section K (tier-up of one shared function under N threads): `operationOptimize` must apply the loop trigger to
the block it is about to enter. Minimal form, recorded here for the dependency: GIL off, before entering
`gilOffDFGForLoopEntry()`, test that block's `shouldReoptimizeFromLoopNow()` (and its OSR-entry FTL child's); if
either holds, jettison that block (which clears `gilOffDFGForLoopEntry`), charge the FTL replacement one
`countOSRExit()` as a failed entry does, and return. The refused-entry path then jettisons the replacement after the
loop threshold as it does today. The test above is the regression test: 801 entries and 903 exits before, `main`'s 2 exits plus at
most the loop threshold after; its bound is to be tightened in the same change.

### A: arm64 / non-Linux notes

- D-A1 has no lock-free reader and therefore no ordering question on any target.
- D-A2's record is a fence-published pointer read through address dependencies (F1/F2); its claim needs the arm64
  compare-and-swap emitter that offlineasm lacks today (N1) and its guard load falls under N-D2.
- A-3 and its fix are policy in C++ under existing locks; nothing architecture-specific.

### A: Decisions for the user

None of its own. D-A1 is recommended after K's fix; D-A2 is recommended to stay on the shelf until a workload shows
interpreted-phase property access GIL off as a cost.

### A: Doc mismatches

- SPEC-jit history §57 and LANDING-PLAN's Open item "A property-adding put in a function still in the LLInt forces an
  exit GIL off" say the generic-`PutById` rule made `richards-like` slow "through a BadCache storm whose cause was not
  established" and that "objects built through the generic put path fail the consumers' structure checks". The objects
  are the same either way; the cause is A-2/A-3. To be rewritten with a pointer to this section.
- SPEC-jit §4.3's table row for `put_by_id` repeats the same sentence.
- LANDING-PLAN "Tier-up of one shared function under N threads" attributes string-heavy's bimodality to "the FTL
  loop-entry `Overflow` exit and the reoptimization back-off inflated by per-thread counted jettisons"; A-3 is a more
  specific statement of the second half (section K).
- The comment above `exitCountThreadMultiplier()` (`CodeBlock.cpp`) argues from "every thread takes the same
  speculation failure once"; it does not mention that with the superseded-DFG entry rule the same thread takes it
  hundreds of times.

### A: What was run

`scaling/richards-like.js` on the two tenth-round experiment binaries (rule on, rule switchable off) and the final
tree, one and four threads, with `--printEachOSRExit`, `--verboseOSR`, `--dumpDFGDisassembly` (about twenty runs of
1.5-60 s); the stale-loop reproducer on `main` and the final tree in four configurations; `perf record` of Air and
OfflineAssembler in two modes; one exit-count sweep of the 36 JetStream tests, GIL on and GIL off, ten at a time
(under a minute). No build.

## Section B. Polls and the visibility rule GIL off (delta-blue, hash-map, the poll-bound micro rows)

Scope: SPEC-jit I21 as amended by history §39 and §50, AUDIT-checktraps §7.1 (the ruling that was never made), and the
cost they leave in GIL-off FTL code. Everything below is Release, Linux x86-64, the tenth round's final tree; GIL off
is `JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0` with `--useJSThreads=1`, checked with
`--dumpOptions=1` (`useThreadGIL=false`). Numbers are instruction counts (`perf stat -e instructions:u`) or counts of
nodes in `--dumpFTLDisassembly=1` output; none is a wall-clock time.

### B: Inventory

#### B-1. A GIL-off poll is two instructions, and FTL code executes far more polls than it has loops  (explained)

Configuration: GIL off only (GIL on and flag off have no polls since the tenth round: traps are delivered by signal).

Mechanism. `FTL::LowerDFGToB3::compileCheckTraps` lowers a poll to a 32-bit test of the trap word against
`VMTraps::AsyncEvents` and a branch (on x86-64 `test dword [reg], 31; jnz`, the address kept in a register), with the
invalidation point as a zero-byte patchpoint at the rejoin. That part cannot go: asynchronous breakpoint patching is
I2's violation with more than one running mutator. What can go is the number of polls. `ByteCodeParser::handleCheckTraps`
plants a `CheckTraps` for every `op_check_traps`, and the bytecode generator emits one at every function entry as well
as after every `op_loop_hint`. An inlined callee therefore brings its entry poll into the caller's loop body, and the
loop unroller (`DFGCloneHelper.h` lists `CheckTraps` as cloneable) copies the header poll into every unrolled copy.

Evidence.
- Micro loops, instructions per iteration, GIL on -> GIL off (one noInline'd function per row, `perf stat` over a run of
  1e8-3e8 iterations, per-iteration Air listing from `--dumpFTLDisassembly=1`):

  | loop | GIL on | GIL off | extra | what the extra instructions are |
  |---|---|---|---|---|
  | `s = (s + i) \| 0` (int-loop) | 4.7 | 6.7 | +2.0 | the poll; the loop is unrolled four times and every copy keeps its own poll (`pollWrites()` is empty: nothing is re-read) |
  | `s += o.x` inline property | 4.7 | 6.7 | +2.0 | the poll only; `o.x` is a data-only read and is hoisted (history §50 works here) |
  | `s += o.p9` out-of-line property | 4.7 | 6.9 | +2.2 | the poll only |
  | `s = a.f(s)` prototype method, inlined | 4.7 | 8.8 | +4.1 | two polls per iteration: the loop's and the inlined callee's function-entry poll |
  | `s += f(i)` closure call, inlined | 12.7 | 20.3 | +7.6 | two polls (4); the closure variable re-read and its Int32 check (3) and a `PutStack` per unrolled copy - see B-3 |
  | `for (i < a.length) s += a[i]` | 8.4 | 18.3 | +9.8 | the poll 2; `a.length` re-read 1 (visibility rule, B-2); bounds check against the re-read length 2 (integer range optimization cannot use `i < previous length`); bounds check against the hoisted storage's vectorLength 2-3 (history §39); the Int32 lane verification 3 (I41, outside this topic) |

  So `int-loop` 1.65, `inline-property-read` 2.00, `flat-butterfly-read` 2.01 and `proto-method-calls` 1.70 of
  PERF-RESULTS §6.12 are polls and nothing else (their time ratio exceeds the instruction ratio because the GIL-on loop
  retires one iteration per cycle and the poll adds a second branch and a load to it); `closure-calls` 1.41 is polls
  plus B-3; `array-element-read` 1.75 is one fifth polls, three tenths the visibility rule, two tenths §39, three tenths I41.
- delta-blue, samples of `instructions:u` inside FTL code attributed to DFG nodes through the dump's address ranges
  (period 4,000,037; see "Measurement caveat" below): 1,101 samples GIL off against 572 GIL on (1.93x: the tenth
  round's "twice the FTL instructions" reproduces). `CheckTraps` holds 130 of the 529 extra samples; 116 of those 130
  are polls whose origin is `bc#0`, i.e. function-entry polls. hash-map: FTL 2,557 against 1,496; `CheckTraps` 262, of
  which 202 at `bc#0`.
- Static count over every final FTL graph of delta-blue GIL off: 182 polls sit inside loops; 46 of them are loop-header
  polls and 136 are entry polls of inlined callees. hash-map: 50 in-loop polls, 16 header and 34 inlined-entry.
- The hot loop of delta-blue, `Plan.prototype.execute` (`for (i = 0; i < this.size(); i++) this.constraintAt(i).execute()`,
  everything inlined), has eight polls on the path of one iteration: the loop's, and the entry polls of `constraintAt`,
  `OrderedCollection.at`, `execute`, `output`, `input`, `size` and `OrderedCollection.size`. Sixteen instructions per iteration that GIL on does not execute.

Worth. Polls are 11.8 % of delta-blue's FTL instructions and 10.2 % of hash-map's; FTL code is about 63 % of delta-blue's
7.02 G instructions, so entry polls alone are 0.46 G, 6.6 % of the test. They also multiply B-2's cost, because every
poll is a point after which the control-deciding reads are performed again.

#### B-2. The visibility rule proper: what is re-read after a poll, and what that costs  (explained)

Configuration: GIL off, FTL plans (DFG plans have no LICM; their polls write the interim set and the butterfly).

Mechanism. `DFGPollVisibilityPhase` (run before each global CSE and before LICM in `DFG::Plan::compileInThreadImpl`)
computes, per natural loop, the backward slices of the loop's `Branch`/`Switch` conditions and attaches the value
heaps they read to every `CheckTraps` whose innermost loop it is; `clobberize`'s `CheckTraps` arm writes that set
(`pollWrites(...)` in graph dumps) instead of the interim set. A read of a heap in the set is not loop-invariant and is
not CSE-able across a poll; what is loaded from a re-read pointer is a new SSA value, so its `CheckStructure`,
`GetButterfly`, `GetArrayLength` and `CheckInBounds` are executed again too (`CheckStructure` on the SAME value does
survive polls: shape facts are covered by the invalidation point and precise jettison).

Evidence, `Plan.prototype.execute` (graph "just before FTL lowering", GIL off against GIL on):
- The polls of the loop write `{NamedProperties(v), NamedProperties(elms), Butterfly_publicLength,
  IndexedContiguousProperties, NamedProperties(execute), NamedProperties(direction)}`: `this.v`, `v.elms`, the length
  and the element decide the loop's exit and, with the `execute` lookup, the `Switch` on the constraint's method;
  `c.direction` decides a `Branch` inside the inlined `execute`.
- Per iteration GIL off: `this.v` is loaded twice (after the loop poll for `constraintAt(i)`, after `size`'s entry poll
  for the exit test), `CheckStructure(v)` twice, `v.elms` twice, `CheckStructure(elms)` twice, `GetButterfly(elms)`
  twice, `GetArrayLength` twice, `CheckInBounds(i, length)` once and `CheckInBounds(i, vectorLength)` once,
  `c.direction` twice (once per inlined `output()`/`input()`, separated by `input`'s entry poll) with its Int32 check,
  compare and branch. GIL on: the whole `this.v.elms.length` chain, the butterfly and both structure checks are in the
  pre-header, the bounds check is gone (integer range optimization), `direction` is loaded once and its compare reused.
- Counted from the graph and the Air listing (an Air check or poll is two x86-64 instructions): about 44 extra
  instructions per iteration on the path through one constraint, 16 of them polls (B-1) and 28 re-reads and the checks
  that hang off them (the function's samples agree: 434 GIL off against 302 GIL on, 1.44x, for a GIL-on iteration of
  about a hundred instructions including the constraint's body); the second copy of each re-read (about 12-14 instructions) exists only because an
  inlined callee's entry poll separates it from the first.
- What is NOT required by the rule's purpose (no loop can be made non-terminating, no `if` in a loop can stop seeing a
  plain write): the seven entry polls as refresh points. One refresh per iteration gives the same guarantee. What IS
  required by the rule as written: one re-read of each control-deciding location per iteration, and the checks on the
  re-read values (a compare against the hoisted value instead of the structure check would cost the same two
  instructions and save nothing, because the next link of the chain must be re-read anyway). The vectorLength bounds
  check belongs to §39, not to the visibility rule: it is what makes a hoisted or stale butterfly memory-safe against a
  fresh publicLength (OM I9b).

Evidence, whole tests: three builds of the tenth round that differ in exactly this rule, GIL off, instructions (G),
one run each (`perf stat`, load-independent): the interim set at every poll; history §50's analysis; and an
experiment build whose FTL polls write no value heap at all (every read hoistable; the ping-pong test hangs on it):

| test | interim set | §50 (shipped) | polls write nothing | nothing / §50 |
|---|---|---|---|---|
| delta-blue | 7.196 | 7.222 | 6.101 | 0.845 |
| hash-map | 13.198 | 12.988 | 11.166 | 0.860 |
| gbemu | 42.04 | 41.35 | 38.83 | 0.939 |
| Box2D | 13.36 | 13.45 | 12.77 | 0.949 |
| richards | 9.025 | 8.985 | 8.624 | 0.960 |
| navier-stokes | 10.70 | 10.68 | 10.29 | 0.964 |
| async-fs | 5.504 | 5.376 | 5.214 | 0.970 |
| Air | 7.140 | 7.313 | 7.116 | 0.973 |
| raytrace | 6.176 | 6.231 | 6.096 | 0.978 |
| ai-astar, Basic, OfflineAssembler, splay, UniPoker, pbkdf2 | | | | 0.99-1.01 |
| crypto | 8.137 | 7.744 | 7.973 | 1.03 (run-to-run) |

Worth. On the final tree delta-blue is 4.36 G instructions GIL on and 7.02 G GIL off (1.61x), hash-map 7.75 G and
12.9 G (1.66x). Releasing every read (the right-hand column still has every poll and B-3's clobbers) is worth 1.1 G of
delta-blue's 2.66 G gap and 1.8 G of hash-map's 5.2 G: 40 % and 35 % of the gap, and 2-6 % on seven other tests. The
rest of those two gaps is B-1, B-3, the array-length compare-and-swap of `ArrayPush`, allocation through the thread's
allocator table, and (hash-map) a different set of compilations: `hasNext` takes 81 InadequateCoverage exits in its DFG
code GIL off against 1 GIL on, and `_rehash`, `_makeNext`, `hasNext` run as separate FTL functions GIL off where GIL
on has them inlined into `run`. Those are outside this topic.

Note that §50 itself moved delta-blue by nothing (7.196 -> 7.222) and hash-map by 1.6 %, while it moved float-mm.c by
a third: B-3 is why.

#### B-3. GIL off, `PutStack` (and every other node `DFGDoesGC` answers "true" for) writes the whole heap  (explained)

Configuration: GIL off, DFG and FTL plans.

Mechanism. `jsThreadsParkableSlowPathClobbersHeapFactsGILOff` (`DFGClobberize.h`) makes a node that can park in a
slow path write `Heap` before its own effects (AUDIT-checktraps P10c). Its `default:` arm returns
`doesGCIgnoringClobberize(graph, node)`, so every node for which `DFGDoesGC.cpp` does not prove "cannot collect" is
in the set. `main`'s `DFGDoesGC.cpp` lists `PutStack` in its conservative `return true` group (it is harmless there:
the answer only feeds store-barrier epochs). GIL off that makes every `PutStack` - a store to the machine frame that
calls nothing - a full heap clobber: `PutStack(... W:Stack(loc14),Heap ...)` in GIL-off dumps, `W:Stack(loc14)` GIL
on. Two consequences: no heap read or check can be hoisted out of, or reused across, a loop that contains a
`PutStack` (an inlined closure call stores its callee, an inlined call whose frame may be inspected stores its
arguments), and `PollVisibilityPhase::compute` gives up on such a loop (`writesWholeHeap -> return nullptr`), so its
polls keep the interim set.

Evidence. Static count over the final FTL graphs GIL off: delta-blue has 245 `PutStack` nodes that write `Heap`; of
the 123 in-loop nodes that write the whole heap, 60 are `PutStack` (then `DirectCall` 17, `ArrayPop` 11, `GetById` 10,
`ArrayPush` 8, `NewObject` 8); 126 of its 182 in-loop polls carry the interim set and only 56 an analysed one.
hash-map: 48 of 53 in-loop whole-heap writers are `PutStack`; 35 of 50 in-loop polls carry the interim set. GIL on
the same graphs have no `PutStack` writing `Heap`. The closure-call micro loop: no `pollWrites` attached, the closure
variable and its type check re-executed every iteration (B-1's table).

Worth. Not separable without a build; bounded below by what §50 failed to deliver on the object-heavy tests (nothing
on delta-blue, 1.6 % on hash-map, against the 14-15 % that releasing every read is worth there), since those tests'
loops are exactly the ones `PutStack` disqualifies. The `closure-calls` micro row would lose 3 of its 7.6 extra
instructions per iteration.

Doc state: AUDIT-checktraps P10c-R still describes the `doesGC`-true long tail as NOT in the predicate (see "Doc
mismatches").

#### B-4. Allocation and other truly parkable nodes write the whole heap  (explained; by design, recorded for its cost)

Same predicate, the nodes it was written for: `NewObject`, `NewArray`, `MakeRope`, `AllocatePropertyStorage`, ... write
`Heap` GIL off because an allocation slow path can park for a collection and its rejoin has no invalidation point of
its own. A loop that allocates therefore hoists nothing and keeps the interim set at its polls. Static count: 14
in-loop allocation nodes in delta-blue's FTL graphs (against 60 `PutStack`). AUDIT-checktraps §7 item 7 names the
systemic closure (an epoch check at the heap-access re-acquire edge plus the invalidation points that are already
planted after such nodes) and leaves it unassigned. Not measured separately.

#### B-5. What `main` does with the one kind of memory JavaScript already shares  (explained)

On `main` (stock, no threads flag) an FTL-compiled `while (ta[0] === 0 && n < limit) n++` over an `Int32Array` on a
`SharedArrayBuffer` never sees another agent's plain store: warmed up, then started with `limit = 2^31 - 1`, it runs
to the limit (the load is hoisted by LICM; the same with `--usePollingTraps=1`). With `--useFTLJIT=0` it sees the
store after 1.0e8 iterations. So `main`'s contract for plain shared reads is already "may be hoisted for the loop's
lifetime; use `Atomics`", and it is tier-dependent. ECMA-262's shared-memory guidelines for implementers say the same
in words (hoisting a non-atomic read out of a loop is permitted even if that affects termination). The threads branch
GIL off is stricter than `main` here: `jit/poll-visibility-control-reads-gil-off.js` asserts that a spin on a plain
typed-array element over a SharedArrayBuffer ends.

#### Measurement caveat  (method)

`perf record -e instructions:u -c 1000003` was throttled by the kernel on this machine (84 THROTTLE events in one
hash-map run; `perf report --stats`): the GIL-off main thread retires 4.6 instructions per cycle in its hot loops and
the sample rate limit drops samples exactly there. The sampled total was 8.7 G against 12.9 G from `perf stat`. With
a period of 4,000,037 there is no throttling and the totals agree. Per-symbol and per-node sample sums taken with a
1 M or 2 M period under-represent high-IPC code (poll-dense loops above all); ratios between two configurations taken
the same way are less affected than absolute shares.

### B: Designs

The first four need no memory-model ruling: each keeps today's guarantee that every control-deciding read of a loop is
performed again at least once per iteration. The fifth and sixth are the two ways to finish decision (c).

#### D-B1. An inlined callee's entry poll is an invalidation point, not a poll  -  **Status: proposed**

Rule (I21, GIL off, DFG and FTL plans). `ByteCodeParser::handleCheckTraps` emits `CheckTraps` for an `op_check_traps`
of an inlined code block only when it is a loop poll (the instruction follows `op_loop_hint`); for the entry poll of an
inlined code block it emits a plain `InvalidationPoint` (not a trap-breakpoint site), which is what flag off and GIL on
emit there. The machine code block's own entry poll and every loop poll stay. LLInt and Baseline are unchanged (their
entry polls are what bounds recursion there).

Why it is sound. A poll has two jobs GIL off. (1) Safepoint latency: every loop iteration and every real call passes
a poll; between two of them lies loop-free code bounded by the inlining budget, so removing inlined entry polls does
not create an unbounded poll-free path (recursion passes a real entry). (2) Parking under a stop: a thread parks only
inside a poll's slow path, and resumes into that poll's invalidation point; with fewer polls there are fewer park
sites, each still followed by its invalidation point. Watchpoint fires and deferred claims: a fire runs with every
mutator parked at some poll or inside a call; the `InvalidationPoint` left at the inlined entry keeps the flag-off
placement for code that returns into this frame. Nothing about the heap-fact epoch changes.

Memory ordering: none involved (a poll is a relaxed load of the trap word on both architectures).

Collector / stop protocol: stop latency is "time to the next poll"; the longest stretch added is one inlined body.
The 30 s stop watchdog is unaffected.

What another thread can observe: nothing. What flag off and GIL on see: nothing (the branch is behind
`Options::usePollingTraps() && gilOff`; with `--usePollingTraps=1` on `main`'s terms the old form is kept).

Failure mode and detection: a poll-free unbounded path would show as a stop-watchdog abort; test with a deep chain of
inlined calls inside a call-free loop, stopped repeatedly from another thread (`gc()` in a loop), asserting every stop
completes.

Tests: `jit/inlined-entry-poll-elided-gil-off.js` - a spawned thread runs `for (...) s = a.f(s)` and a loop over a
chain of five inlined calls, call-free and allocation-free, while the main thread requests 200 collections and 200
watchpoint-firing transitions: every stop completes and the results are right (a missing safepoint shows as a
stop-watchdog abort). The count half cannot be observed from script in a Release build (a fast-path poll leaves no
trace), so the before/after evidence is the instruction count per iteration recorded in PERF-RESULTS
(`proto-method-calls` +4.1 -> +2.0) and the number of `CheckTraps` nodes in the loop in a Debug graph dump (2 -> 1).

Verification: corpus four modes, TSanJIT GIL off, the stop-latency tests of `vmstate/` and `gc-stress/` under the
amplifier (they are the ones that would notice a missing poll).

Expected gain: removes 136 of 182 in-loop polls statically in delta-blue, 34 of 50 in hash-map; dynamically entry polls
are 116 of 130 poll samples in delta-blue's FTL code (10.5 % of its FTL instructions, about 6 % of the test) and 202 of
262 in hash-map (8 % of FTL instructions); `proto-method-calls` 1.70 -> about 1.35. It also halves B-2's re-reads
wherever the second copy was separated from the first only by an entry poll (D-B2 makes that independent of this).

Risk: low. Alternative rejected: keeping the poll but making it non-clobbering (that is D-B2) - the two instructions
would stay for no purpose.

#### D-B2. One refresh point per loop iteration: only loop-header polls carry the visibility set  -  **Status: proposed**

Rule (I21, GIL off, FTL plans). `DFGPollVisibilityPhase` attaches a loop's control set only to the `CheckTraps` nodes
that are loop polls of that loop (origin: the `op_check_traps` that follows the loop's `op_loop_hint`, in the machine
code block or an inlined one; after unrolling, see D-B3). Every other poll inside the loop gets an attached EMPTY set
(so it writes `InternalState`, `Watchpoint_fire`, `SideState` only). If a loop contains no loop poll of its own (not
expected), all its polls keep today's attachment. Polls outside any loop (function entries) get the empty set too.

Why it keeps the guarantee. Every iteration of every loop passes that loop's header poll, after which every
control-deciding read of the loop is performed again: no spin can be made non-terminating and an `if` inside a loop
sees a plain write by the next iteration at the latest. What a program can no longer rely on is a second refresh
inside one iteration at the point where a callee happened to be inlined - which it could not rely on anyway, since
whether the callee is inlined, and whether there is a poll between two reads, is not visible to the program (two reads
with no call between them are already CSE'd today). A function-entry poll needs no set at all: no heap read of the
activation precedes it.

Tiers: FTL only (the DFG tier keeps the interim set at every poll; it has no LICM and is transitional). C++ and the
lower tiers re-read everything.

Memory ordering, collector, stop protocol, watchpoints: unchanged (the polls themselves, their invalidation points and
the epoch bracket are untouched; only the abstract-heap write set of some polls shrinks).

Flag off / GIL on: nothing (the phase returns at its first test).

Failure mode: a loop whose header poll was removed or moved by another phase would lose its refresh; the phase asserts
(`validationEnabled`) that each analysed loop has at least one set-carrying poll that dominates every back edge source
or sits in the header. Detected by `jit/poll-visibility-control-reads-gil-off.js` (ten spin shapes, entered at their
top) plus two new shapes: a spin whose condition is read through an inlined getter (`while (!flag.get())`), and a spin
inside a callee inlined into a loop.

Expected gain: in `Plan.prototype.execute` the second copy of the `this.v`/`v.elms`/butterfly/length chain and of
`c.direction` disappears: about 12-14 of 28 re-read instructions per iteration; with D-B1 the loop goes from +44 to
about +16 instructions per iteration over GIL on (2 poll, 14 one refresh of the chain and its checks). On delta-blue as
a whole, D-B1 + D-B2 together: about -11 to -13 % instructions (6 % polls, 5-7 % half of the 15.5 % that all re-reads
are worth); hash-map about -9 to -11 %.

Risk: low-medium (a semantics argument, not a protocol). Alternative rejected: moving the header poll to the latch so
that the exit test and the next iteration's body share one refresh - the bytecode generator emits the condition twice
(loop inversion) and the latch does not dominate the header, so CSE could not use it; the dominance already works the
other way round once inlined-entry polls stop clobbering (the header's reads dominate the latch's).

#### D-B3. After unrolling, one poll per trip of the unrolled loop  -  **Status: proposed**

Rule (GIL off). When `DFGLoopUnrollingPhase` clones a loop body, the clones' header `CheckTraps` become
`InvalidationPoint`s; only the first copy of a partially unrolled loop keeps its poll, and a fully unrolled loop keeps
none. Poll latency grows by the unroll factor (at most four small bodies). The refresh guarantee becomes "once per trip
of the unrolled loop", i.e. at most the unroll factor in source iterations.

Expected gain: `int-loop` +2.0 -> +0.5 instructions per iteration (1.65 -> about 1.15 in time), the same for every
small counted loop that qualifies for unrolling (crypto and float kernels). Flag off / GIL on: nothing. Test: the
int-loop shape with `perf`-independent evidence - a `$vm` counter of `operationHandleTraps` calls cannot see fast-path
polls, so the evidence is the instruction count recorded in PERF-RESULTS and a functional test that such a loop is
still stopped (collection requested from another thread completes within the test's deadline).

Risk: low. This weakens "again after every poll" to "again after every remaining poll", which is what the rule says
already.

#### D-B4. `PutStack` is not a park site  -  **Status: proposed**

Rule. `jsThreadsParkableSlowPathClobbersHeapFactsGILOff` names the nodes that `DFGDoesGC` answers "true" for out of
upstream conservatism and that emit no call and no allocation, and returns false for them: `PutStack` first (also
`PutToArguments`-class stores if the audit below confirms they emit no call; terminals such as `Return`, `Throw`,
`ForceOSRExit` are irrelevant either way). `DFGDoesGC.cpp` itself is NOT changed (it runs flag off; changing it would
move store-barrier placement on `main`'s paths). The predicate's three consumers (clobberize, the abstract
interpreter's `executeEffects`, `clobbersExitState`) share the function, so they stay in lockstep.

Soundness: the predicate exists because a node's slow path may release heap access and park across a stop that
rewrites heap facts. `PutStack` lowers to one store to the frame (`FTL::compilePutStack`, `SpeculativeJIT::compilePutStack`):
no call, no allocation, no lock. Audit rule for adding a node to the exclusion list: its lowering in both tiers
contains no `callOperation`/`vmCall`/`lazySlowPath`/allocation.

Flag off / GIL on: nothing (the gate returns before the switch).

Tests: `jit/putstack-does-not-defeat-poll-analysis-gil-off.js` - the functional half: the spin shapes of the existing
poll-visibility test wrapped in a loop that also makes an inlined closure call, and one with an inlined call whose
arguments are flushed to the frame, must still end (the control set is now analysed there instead of being the interim
set); the count half: instructions per iteration of the closure-call loop recorded in PERF-RESULTS (+7.6 -> about +4.6,
with D-B1 about +2.6), and in a Debug graph dump the loop's poll shows `pollWrites(` where it showed the interim set.

Expected gain: lets history §50's analysis apply to 60 of the 123 disqualified in-loop sites in delta-blue and 48 of 53
in hash-map, and restores CSE/LICM across those nodes. Upper bound: the part of the 14-15 % (B-2's table) that lies in
loops with a `PutStack`; together with D-B1/D-B2 the no-ruling package is estimated at -13 to -16 % instructions on
delta-blue and hash-map (GIL off / GIL on instruction ratio 1.61 -> about 1.38 and 1.66 -> about 1.43).

Risk: low. Follow-up recorded, not designed here: B-4 (allocation nodes as invalidation points instead of heap
clobbers, AUDIT-checktraps §7 item 7 option (ii)).

#### D-B5. Bounded staleness: the poll's fast path keeps nothing fresh, its slow path refreshes, a tick bounds the wait  -  **Status: proposed (needs the ruling)**

Contract offered to programs. "A plain read inside a loop may return a value that is up to one visibility tick old
(default 1 ms) plus one loop iteration; after that the loop performs it again. A spin on a plain field, element,
closure or global variable ends. Ordering and immediacy come from `Atomics` and locks only."

Rule (I21, GIL off, FTL plans).
1. *The poll is control flow before SSA.* A new phase (`performPollRefreshLoopCreation`, after loop pre-header creation
   and unrolling, before SSA conversion, where locals are still in load/store form and no Phi plumbing is needed)
   rewrites each natural loop L whose header H begins `LoopHint, CheckTraps(exitOK)` and has a pre-header P0:
   - `H` := `[LoopHint; t = TrapBitsSet; Branch(t, rarely S, usually H2)]`, `H2` := the rest of the old header.
   - `S` := `[HandleTraps; ExitOK; InvalidationPoint; Jump PH']` - the old slow path with its invalidation point.
   - `PH'` := `[Jump H]`, a new block; P0's terminal is retargeted from H to PH'.
   PH' is now L's pre-header and at the same time the header of an outer loop O = {PH', S} + L whose only back edge is
   S -> PH'. `TrapBitsSet` reads `InternalState`, writes nothing and defines nothing; `HandleTraps` reads
   `InternalState`, writes `InternalState`, `Watchpoint_fire`, `SideState` and the loop's visibility set V(L) - the set
   `DFGPollVisibilityPhase` computes today, or the interim set where it gives up - and defines the invalidation point.
2. *Existing phases do the rest.* LICM hoists L-invariant loads (now including the control-deciding ones: nothing in
   L writes V(L)) into PH'. It cannot hoist them further, out of O, because `HandleTraps` in S writes V(L); data-only
   reads and structure checks still go all the way to P0. Global CSE cannot carry a load around S -> PH'. SSA conversion
   builds the Phis at PH' (values from P0 or from H's Phis via S) and at H. So every time the slow path is taken, the
   loop re-enters through PH' and re-executes exactly the hoisted control-deciding reads and the checks on them; on the
   fast path the loop body is GIL on's body plus the two-instruction poll.
3. *The tick.* A `VisibilityTicker` (one thread per process, started with the first spawned Thread and parked when no
   spawned Thread is live) raises a no-op trap event in the trap word of every thread that is inside the VM, every
   T = `visibilityTickIntervalMs` (default 1). `VMTraps::handleTraps` services it by clearing it. Delivery must be per
   thread: each thread clears its own bit. GIL-off generated code reaches the running thread's lite through
   `AssemblyHelpers::loadVMLite`; the poll tests the lite's trap word (address loaded once per function and hoisted)
   instead of the VM-level word, and VM-wide events keep fanning out into every lite as they do today. (Fallback if the
   per-lite word cannot be polled by generated code yet: one VM-level bit plus a generation number, the slow path
   comparing the thread's last seen generation, the ticker clearing the bit after a fixed 20 us window; costs a few
   hundred redundant slow-path entries per tick.)
4. *Everything else keeps today's form*: polls not at a loop header (D-B1 removes most), loops without a pre-header,
   `exitOK == false` polls, DFG-tier plans (no LICM; their polls keep the interim set), catch-containing or irreducible
   loops.

OSR exit state. S and PH' carry the bytecode origin of the loop's `op_loop_hint` with `exitOK`; the locals there are
the header's (nothing between the header's head and the poll changes a local). A check hoisted into PH' that fails
after a refresh exits to the loop header in Baseline, as a check hoisted to the pre-header does today. OSR entry
targets H as before (`isOSRTarget` stays on H; `FTLForOSREntry` reaches H through the entry root and PH').

Invalidation points, heap-fact epoch. The park happens in `HandleTraps`; the invalidation point follows it in S, so a
thread whose code was jettisoned during its park exits before it re-enters the loop (I21's clause is kept: every park
site is followed by an invalidation point). The fast path needs none: nothing parks there. The epoch bracket in
`VMTraps::handleTraps` is unchanged. A side effect worth having: every stop now also refreshes every loop.

§39 and typed arrays: unchanged. A hoisted butterfly is still bounded by its own vectorLength; a hoisted
`{vector, length}` pair is still cut off by the epoch bump of the stop that retires quarantine entries.

Memory ordering. No new inter-thread protocol. The ticker's store to a trap word is a relaxed atomic OR, the poll a
relaxed load; both architectures make a store visible to a polling load in finite time (coherence), which is all the
contract promises. The refreshed reads are ordinary loads; no fence is needed for "eventually", and none is promised
for ordering. arm64: the poll is `ldr; tst; b.ne` (three instructions), so D-B1/D-B3 matter more there; nothing in this
design relies on x86-64's store order.

Collector: the ticker thread holds no heap access and touches only trap words under the lite registry's lock, like the
VM-wide fan-out of a stop request. Marking sees hoisted values through the conservative scan as today.

What a second thread can observe mid-way: nothing new; this changes when a reader re-reads, never what a writer does.

Flag off: the phase, the two nodes and the ticker do not exist (first-test return; ticker never started). GIL on: no
polls.

Failure modes and detection.
- A thread that never receives ticks spins forever: test with eight threads spinning on plain flags released one by
  one; and a `$vm` counter of serviced ticks per thread.
- CFG surgery errors: `--validateGraphAtEachPhase` lanes of the JSC stress suite GIL off; the DFG validator checks
  SSA dominance after conversion.
- Wrong exit state in PH': a test that lets the refresh find a changed structure (the main thread transitions the
  object the loop reads through while a worker loops), asserting results equal the interpreter's.
- Refresh storms (a trap bit left set): bounded by the slow path parking; counter of slow-path entries per second in
  the scaling bench must stay near 1000 x threads.

Tests. `jit/poll-refresh-spins-end-gil-off.js` (the ten shapes of the existing test, now expected to end through the
tick: asserts each ends and that `serviced ticks > 0`); `jit/poll-refresh-without-ticker-hangs-gil-off.js`
(`--visibilityTickIntervalMs=0`, run under the runner's expected-timeout mode: proves the refresh edge, not luck, ends
the spin); `jit/poll-refresh-exit-state-gil-off.js`; `jit/poll-refresh-osr-entry-gil-off.js` (a function called once
that spins: entered through loop OSR entry); instruction counts for the micro rows and delta-blue/hash-map before and
after in PERF-RESULTS.

Verification: corpus four modes Release and Debug; TSanJIT GIL off (the ticker's stores are atomics; the refreshed
reads race by design exactly as today's re-reads do); the GIL-off JSC stress suite and the mirror harness (CFG change
in every FTL plan GIL off); amplifier on the touched tests.

Expected gain. The "polls write nothing" column of B-2 is this design's code on the fast path (that build kept every
poll and B-3's clobbers): delta-blue -15.5 %, hash-map -14.0 %, gbemu -6.1 %, Box2D -5.1 %, richards -4.0 %,
navier-stokes -3.6 %, async-fs -3.0 %, Air -2.7 %, raytrace -2.2 %. Added to D-B1 (about -6 % and -5 %) and D-B4:
delta-blue 7.02 G -> about 5.5 G (GIL off / GIL on 1.61 -> about 1.27), hash-map 12.9 G -> about 10.4 G (1.66 -> about
1.35); `array-element-read` +9.8 -> +7 per iteration (the length re-read and its bounds check go; §39's and I41's
stay). Tick cost: one slow-path call and one pass through PH' per thread per millisecond, under 0.05 % of a thread.

Risks. A new CFG-shaping phase in every GIL-off FTL plan (precedents: loop pre-header creation, critical edge
breaking, loop unrolling); more Phis on the cold edge; per-thread trap delivery to generated code is a prerequisite
that the trap architecture (SPEC-ungil §A.2) plans but generated code does not use yet (today's FTL poll tests the
VM-level word).

Alternatives rejected. (a) OSR-exit on a tick and re-enter later: a Baseline stretch plus a DFG OSR entry per tick per
loop, and exits count towards jettison. (b) A per-thread countdown in the poll (`dec [mem]; js`): a read-modify-write
on the loop-carried path, 5 cycles per iteration on the int-loop shape. (c) Expressing the diamond inside the loop and
teaching LICM partial redundancy: DFG has no PRE; the refresh loop gets the same code from phases that exist.
(d) Doing it in B3: hoisting is done by DFG LICM on DFG nodes; B3 sees loads already placed. (e) Loop versioning (a
fresh copy and a stale copy of the body): doubles code size for the same effect.

#### D-B6. Full release: plain reads may be hoisted for the loop's lifetime  -  **Status: proposed (the other ruling)**

Rule: delete the value-heap writes from `CheckTraps`'s GIL-off arm in `clobberize` (FTL and DFG), keep
`JSObject_butterfly`/`Butterfly_vectorLength` for DFG plans and everything about invalidation; drop
`DFGPollVisibilityPhase`. Contract: `main`'s contract for SharedArrayBuffer memory (B-5), extended to shared ordinary
objects: a plain read in optimized code may never be performed again; cross-thread signalling needs
`Atomics.load`/`Atomics.store`/`Atomics.wait` (on typed arrays, or on ordinary objects through the API extension) or
a lock. Same generated code as D-B5's fast path; same gains; no new phase, no ticker. Tests to rewrite:
`checktraps-invalidation.js` Part 4, part (1) of `jit/poll-visibility-control-reads-gil-off.js` and the scaling and
corpus files that signal through plain fields (the experiment build hung the ping-pong test); each becomes an
`Atomics` test, and one new test pins the hoisting (bounded spin runs to its limit, as on `main`).

The cost is behavioural and tier-dependent: a program that signals through a plain flag works in the LLInt, Baseline
and DFG tiers and hangs once the FTL compiles the loop - on `main` that is true for typed arrays over shared buffers
only; here it would be true for every object a Thread shares. It also makes the `Atomics`-on-ordinary-objects
extension load-bearing instead of optional (see Decisions).

#### Considered and not proposed

- *Compare-and-skip on re-read pointers* (skip the derived checks when the re-read value equals the hoisted one):
  `CheckStructure` is a two-instruction compare against memory; a register compare plus branch costs the same, and the
  next link of the chain (`v.elms`, the length) must be re-read whatever the compare says. No gain.
- *Treating `a.length` specially* (let the bound of an array loop go stale): it is D-B6 for one field; a consumer loop
  `while (true) { if (i < q.length) ... }` is exactly the spin the rule protects.
- *Dropping the publicLength bounds check where §39's vectorLength check and a hole check exist* (slots between the
  public and the vector length are empty): saves two instructions per shared-array access GIL off, but turns an
  invariant of the C++ array code into a JIT safety premise under races (pop, `length =`, push's store-after-raise);
  not worth the audit.
- *Exempting loops that contain a real call*: the call already writes the world at its position; the interim set at
  the poll costs nothing extra there.

### B: arm64 / non-Linux notes

- The poll: a relaxed 32-bit load of the trap word, a test and a branch. Correctness needs only that a store to the
  word becomes visible to a later load in finite time, which both architectures give; the handshake that matters
  (parking, the stop's barriers) is in the slow path under locks. arm64 spends three instructions per poll
  (`ldr`, `tst`, `b.ne`) against two, so D-B1 and D-B3 are worth relatively more there.
- The visibility rule itself is about compiler motion, not hardware ordering: a hoisted read is stale by the
  compiler's doing on every architecture, a re-performed read sees whatever coherence has delivered. Nothing in
  I21/§50 or in D-B1..D-B6 relies on x86-64's store order.
- History §39 (a hoisted butterfly with bounds from its own vectorLength): the vectorLength and every element are
  loaded THROUGH the hoisted pointer (address dependency), so no fence is needed on arm64; the freshness of
  publicLength is not a safety premise (that is what the vectorLength clamp is for).
- The invalidation point after a poll's slow path: the patched exit is seen by the resuming thread because the
  resume path executes an instruction-synchronization barrier after cross-modifying-code flushes (SPEC-jit F5); that is
  existing protocol, unchanged, and is the one arm64-sensitive piece in this area.
- D-B5's per-thread trap word: reached through `AssemblyHelpers::loadVMLite` (thread-pointer register read plus a
  load on arm64; a segment-relative load on x86-64 Linux, macOS and Windows use their own thread-pointer forms, which
  that helper already abstracts). The ticker's write is `fetch_or` relaxed; the poll's read relaxed.
- Platforms without signal-delivered traps (Windows, and ThreadSanitizer builds) poll with the flag off and GIL on too.
  D-B1 and D-B3 would help them, but are gated GIL off here so that flag-off code does not change; widening them to
  every polling configuration is an upstream-able follow-up.

### B: Decisions for the user

1. **The visibility rule (decision (c))**. Options, with the measured or derived numbers:
   - (c0) *Status quo plus the four no-ruling refinements D-B1..D-B4.* Keeps "every control-deciding read of a loop
     is performed again every iteration". Estimated -13 to -16 % instructions on delta-blue and hash-map (GIL off /
     GIL on 1.61 -> about 1.38, 1.66 -> about 1.43), most poll-bound micro rows to 1.1-1.4. Low risk; four small
     changes. Does not reach the 0.90 target on these two tests by itself.
   - (c1) *Bounded staleness, D-B5* on top of (c0). A spin on a plain flag still ends, within about a millisecond
     instead of nanoseconds; no tier-dependent hangs. Another -14 to -15 % on delta-blue and hash-map (to about 1.27
     and 1.35), 2-6 % on seven more tests. Costs a CFG phase in GIL-off FTL plans, per-thread trap delivery to
     generated code, a ticker thread while spawned Threads live. Medium risk, the largest piece of work here.
   - (c2) *Full release, D-B6* on top of (c0). Same performance as (c1), smallest implementation (delete code), and it
     is exactly `main`'s and ECMA-262's rule for SharedArrayBuffer memory (B-5). Costs: programs that signal through
     plain fields hang once the FTL compiles the loop; the corpus and scaling tests that do so must move to `Atomics`;
     `Atomics` on ordinary objects becomes necessary.
   Recommendation: take (c0) regardless, in the next implementation session. For the ruling, (c1): it buys (c2)'s
   performance without making correctness depend on which tier a loop runs in, and it keeps the behaviour the branch
   has tested since AUDIT-checktraps §7.1's interim default (plain spins end). If the refresh-loop phase is judged too
   invasive for this landing, (c2) is defensible on precedent, and D-B5 can be added later without changing the
   contract (it only makes more programs terminate).
2. **`Atomics` on ordinary objects behind its own option.** Under (c0)/(c1) the extension is optional (ordering, atomic
   read-modify-write, immediate visibility), so gating it off by default costs nothing and makes the 34
   `SharedArrayBuffer.js` results match `main` flag on. Under (c2) it is the only way to signal through an object
   without a lock and should default on. The two decisions should be taken together.
3. **Tick period** (only with (c1)): 1 ms is the proposal (cost under 0.05 % of a thread); it is the latency a program
   sees when it busy-waits on a plain flag, so it is user-visible.

### B: Doc mismatches

- `AUDIT-checktraps.md` §4 rows P10c / P10c-R and §7 item 7 describe `jsThreadsParkableSlowPathClobbersHeapFacts` as
  a list of allocation-class nodes and say the `doesGC`-true long tail ("~120 further nodes") is NOT in the predicate
  and is left to an unassigned option (ii). The code's `default:` arm returns `doesGCIgnoringClobberize()`: the whole
  `doesGC`-true set IS in the predicate, including non-allocating `PutStack` (B-3). Fix: rewrite P10c-R as "closed by
  widening, at the cost recorded in DESIGN-PROPOSALS B-3", and record the exclusion rule of D-B4 when it lands.
- `SPEC-jit-history.md` §50 lists among the fallbacks "a loop that contains a node writing the whole heap (nothing is
  hoistable there anyway)". The parenthesis is wrong for nodes whose only heap write is the injected parkable clobber
  (`PutStack`, allocation nodes): on `main`'s terms everything is hoistable across them. Same sentence in
  `DFGPollVisibilityPhase.cpp`'s comment. Fix: say "a real call; GIL off also every node of AUDIT-checktraps P10c,
  which is most object-heavy loops".
- `LANDING-PLAN.md` Open items, GIL-off mechanism (2), and `PERF-RESULTS.md` §6.12 say poll visibility "already
  released the reads that only feed data" and that delta-blue's extra FTL work "is the visibility rule, not a missing
  optimization". Measured: 126 of delta-blue's 182 in-loop polls still write the interim set (B-3); 136 of the 182 are
  inlined callees' entry polls (B-1); in `Plan.prototype.execute` 16 of about 44 extra instructions per iteration are
  polls and about half of the re-reads are second copies caused by entry polls. The rule as such accounts for 40 % of
  delta-blue's instruction gap and 35 % of hash-map's (B-2's table), not for all of it.
- `PERF-RESULTS.md` §6.12 "Method added this round": add the throttling caveat (periods of 1-2 M instructions are
  throttled by the kernel on poll-dense GIL-off code; use `perf report --stats` and a period of 4 M or check totals
  against `perf stat`). The per-test instruction ratios it quotes for delta-blue and hash-map (1.95, 1.61) do not match
  `perf stat` on the final tree (1.61, 1.66).
- `SPEC-jit.md` I21 says a GIL-off FTL pre-pass "attaches to each poll inside a loop" the slice set; correct, but it
  omits that function-entry polls of inlined callees are polls inside the loop and get the same set (the source of the
  duplicated re-reads). To be rewritten when D-B1/D-B2 land.

### B: What was run

All on existing binaries; nothing was built.
- `perf stat -e instructions:u` on six one-function micro loops, three modes, final tree (seconds each); the first
  attempt passed the GIL-off variables through an unquoted shell variable under zsh and ran GIL on (all three modes
  gave equal counts); discarded and retaken through bash wrappers, modes verified with `--dumpOptions=1`.
- `--dumpFTLDisassembly=1` of those loops, GIL on and GIL off, condensed to per-block Air listings.
- delta-blue and hash-map, GIL on and GIL off: one run each under `perf record -e instructions:u` with
  `--dumpFTLDisassembly=1`, samples binned into the dump's address ranges per (function, DFG node); done twice (period
  500,009: throttled, used only for ranking; period 4,000,037: no throttle events, used for the numbers above). One
  `perf record` each without the dump to cross-check totals; `perf stat` on both tests, both modes, repeated three
  times for hash-map (12.88-12.95 G GIL off, 7.74-7.82 G GIL on).
- `--printEachOSRExit=1` on hash-map, both modes (exit kinds per function).
- A 16-test GIL-off instruction sweep over three tenth-round builds (interim set / §50 / polls write nothing), one run
  per cell, run 16 at a time.
- A two-agent SharedArrayBuffer test on the `main` build (FTL, FTL with polling traps, DFG only).
- Static statistics over the dumps (polls per loop, attached sets, in-loop whole-heap writers) with small
  throw-away scripts.

## Section C. The Double family, GIL off

Scope: arrays that `main` (and GIL on) keeps as `ArrayWithDouble` and that a GIL-off process holds as `ArrayWithContiguous`
(boxed doubles) or leaves unconverted, and everything that follows from it in the optimizing tiers. Rows: stanford-crypto-sha256
0.40 of GIL on (bimodal), ML 0.56, navier-stokes 0.86 (2.45x-2.68x instructions), the 16 `array-slice-cow` suite results, the
BadIndexingType storms (typescript, aes, sha256), and two micro rows that carry "double" in their name or neighbourhood
(`array-int32-to-double-relabel`, `astar-like-nodes`) and turn out to belong elsewhere. All numbers are instruction or exit
counts on the round-10 final tree (Release, Linux x86-64) unless marked.

### C: Inventory

#### C-1. sha256 is bimodal GIL off because a failed check on a merged local reports to no profile - `explained`

Configuration: GIL off only (latent on `main`, see the reproducer).

Mechanism. sjcl's block function `u(a, b)` is called from `finalize` with `b = padded.splice(0, 16)`. `padded` comes from
`bitArray.concat(words, [partial(1, 1)])`: an Int32 array concatenated with a one-element Double literal. On `main` and GIL on the
result is `ArrayWithDouble`; GIL off it is `ArrayWithContiguous` by rule T4-C (a fresh copy of a Double source is boxed), and so is
the `splice` result. Lane 8 of every block holds the partial word `0x10080000000` (an int52-sized double); the other fifteen lanes
are int32.

- In `u`, `d = b[c]` (bytecode 74) is a `GetByVal` in mode `Contiguous+InBoundsSaneChain`, typed by its value profile alone. The
  profile has one bucket that the next fifteen lanes overwrite, so a harvest sees the double only if it lands between the round that
  loaded lane 8 and the next one. With a Double array (`main`) the array mode types the load as a double regardless of the profile.
- `d` reaches its first checked use, `d + q` (bytecode 312), through a control-flow merge, so the DFG sees `GetLocal(loc11)` created
  at bytecode 312 and the check is `Int52Rep(Check:Int32:@GetLocal)` with exit kind BadType.
- `Graph::methodOfGettingAValueProfileFor(exitingNode, operandNode)` returns nothing for that exit: the block that would hand out
  the GetLocal's lazy-operand profile is guarded by `node->origin.semantic != currentNode->origin.semantic || !currentNode->hasResult()`,
  and a GetLocal is always created with the origin of the bytecode that consumes it. The function is byte-for-byte `main`'s.
- So every recompilation is given the same prediction. The batches double with the reoptimization counter: 101, 201, 401, 801 exits
  at the same node in four successive DFG code blocks of `u` (execute thresholds 2,587 -> 5,174 -> 10,348 -> 20,697), 1,504 in all,
  until a harvest happens to catch lane 8 (the fifth compile shows `GetByVal ... BoolInt32|NonBoolInt32|AnyIntAsDouble` and
  `Int52Rep(Check:AnyInt)`). In the fast population the first compile already has the double at bytecode 74 and the only `u` exits
  are 101 at bytecode 130, a use whose operand is the `GetByVal` of the same block, which does have a profile and heals after one batch.

Evidence. Six runs, `perf stat -e instructions:u`: 10.03 G (scores 624, 626) and 10.47-10.48 G (454-480); GIL on 8.36 G (874); flag
off 8.29 G; `main` 8.09 G. `--printEachOSRExit=1`, per function and bytecode: GIL on 1,696 exits, none in `u`; GIL off 1,997 (fast
population: `u` bc#130 x101) to 3,400 (`u` bc#312 x1,502); always extra GIL off: `bitLength` bc#1 BadIndexingType x201 (C-4).
`--dumpDFGDisassembly=1` gives the node shapes quoted above. Reduced reproducer (`getlocal-exit-storm2.js`, same loop shape, an
array forced Contiguous on every build): `main` 703 exits in one run and 302 in the next, flag off 302, GIL off 302 and 703 - the
same storms on `main` once the array is Contiguous.

Worth: the median of five lands on either population; slow 310-410, fast 460-620 in the suite (0.40 or 0.72 of GIL on). Removing the
slow population is worth +4.4 % instructions on this test and, through the first-iteration component, up to x1.8 of its score.

#### C-2. navier-stokes runs on boxed doubles because its arrays are allocated once - `explained`

Configuration: GIL off only.

Mechanism. `reset()` allocates six `new Array(16900)` and fills them with the integer 0 once, at load time; the solver's first double
store requests Int32->Double. `JSObject::relabelIndexingShapeConcurrent` GIL off executes that request as Int32->Contiguous (T4-O:
`effectiveTransition = AllocateContiguous` when `gilOff && hasInt32(source) && transition == AllocateDouble`). The allocation-profile
promotion (`ArrayAllocationProfile::noteSubstitutedDoubleRequestGILOff`, T4-P) cannot help: the site never allocates again.

Evidence. `describe()` of the density array after setup / after three runs: GIL on `ArrayWithInt32` / `ArrayWithDouble`; GIL off
`ArrayWithInt32` / `ArrayWithContiguous`. Whole test 3.96 G instructions GIL on, 10.62 G GIL off (2.68x; cycles 1.14x). The
`lin_solve` inner loop isolated (`linsolve.js`, 128x128 grid, instructions per inner iteration, difference of two run lengths):

| arrays | GIL on | GIL off |
|---|---|---|
| born Double (first store a double) | 31.1 | 43.1 |
| zero-filled with ints, doubles later (the benchmark) | 31.0 (Double after the in-place relabel) | 82.0 (Contiguous) |

So of the 2.64x on the kernel, 1.39x is the general GIL-off cost of an array loop (polls, the vector-length bound) and 1.90x on top of
that is boxing: a Double array GIL off would run the kernel at 43 instructions per iteration instead of 82.

Worth: navier-stokes 0.86 of GIL on; instructions 2.68x -> about 1.4x.

#### C-3. ML's hot loops are polymorphic because Int32 rows can no longer be converted on arrival - `explained`

Configuration: GIL off only.

Mechanism. `mmul` reads `this[i][k]` over rows that arrive from many sites. A probe in `mmul` (every seventh call, `describe()` of
every row) over twelve iterations: GIL on `this` rows ArrayWithDouble 167,496, CopyOnWriteArrayWithInt32 360; GIL off ArrayWithDouble
125,688, CopyOnWriteArrayWithInt32 38,556, ArrayWithContiguous 3,612 (`other` rows: Double 142,416, Contiguous 144). The Int32 rows are
the training-set literals (`[[0, 0], [0, 1], ...]`). On `main` a site that has seen {Int32, Double} is compiled `Double+Convert`
(`ArrayMode::fromObserved`'s mixed-shape arm: `shouldUseDouble(observed)` selects `Array::Double` with `Array::Convert`), its `Arrayify` turns each Int32 row into a Double row in place the first time it arrives, and from then on the
site is monomorphic Double. GIL off the conversion request is executed as Int32->Contiguous, `ArrayifyToStructure`'s post-condition
fails (BadIndexingType exit, C-4), the rows stay CopyOnWrite Int32 or become Contiguous, and the site ends up a three-shape
`MultiGetByVal` with unboxing. The T4-P promotion has done its part on the round-10 final tree (most rows are born Double); what is left is rows that are
int32 literals by nature, which only a real Int32->Double transition can serve.

Evidence. `mmul.js` (the inner loop with 64 rows of 16, instructions per inner iteration):

| rows | GIL on | GIL off | final shapes GIL off |
|---|---|---|---|
| all born Double | 10.4 | 17.4 | 64 Double |
| one row in four an int32 literal | 14.7 (all 64 Double after the first pass) | 43.7 | 48 Double, 12 CopyOnWrite Int32, 4 Contiguous |

Whole test 27.2 G instructions GIL on, 53.2 G GIL off (1.95x). Exits do not differ (1,313 GIL on, 1,178 GIL off): this is steady-state
code, not a storm.

Worth: ML 0.56 of GIL on; the kernel would go from 2.98x to about 1.2x of GIL on.

#### C-4. The BadIndexingType storms are `Arrayify(Double)` and hoisted array checks failing on arrays `main` would have converted - `explained` for sha256/aes, `partly explained` for typescript

- sha256 and aes: `bitLength` bytecode 1 (`a.length`), node `ArrayifyToStructure(Double+OriginalNonCopyOnWriteArray+InBounds+Convert)`:
  an Int32 array arrives, `operationEnsureDouble` -> `tryMakeWritableDoubleSlow` -> `convertInt32ToDouble` -> the substituted relabel
  leaves it Contiguous, the operation returns null and the node exits. 201 exits in two batches, then the site is recompiled generic.
  aes: 207 BadIndexingType GIL off (201 of them this site), 0 GIL on.
- typescript: 1,730 BadIndexingType GIL off against 138 GIL on. 1,299 are one check at bytecode 0 of one function (498 in DFG code,
  801 in FTL code: thirteen batches, a check hoisted to the function's entry whose exit cannot feed the array profile of the access it
  was hoisted from) and 401 one FTL site at bytecode 380 of another. Which arrays arrive there was not traced; by their kind (array
  checks that pass GIL on) they are the same family. Next step: `--dumpDFGDisassembly` of function `#CIoSvw` and `describe()` of the
  argument.

Worth: each storm is a few hundred exits and one to thirteen recompilations; small in instructions, visible in first-iteration times.

#### C-5. `array-slice-cow` (16 suite results) is T4-C observed through `$vm.indexingMode` - `explained`

`JSArray::fastSlice`, the `concat` fast paths and the DFG `ArraySlice` admission make every fresh copy of a Double source Contiguous
GIL off (`describe(src.slice())` = ArrayWithContiguous, `src` stays ArrayWithDouble). Visible through `$vm` only. Both designs below
that restore Double transitions withdraw T4-C.

#### C-6. `array-int32-to-double-relabel-200k` measures neither an Int32->Double relabel (GIL off) nor the conversion body (flag off) - `explained`

The micro is `const a = [1, 2, 3, 4]; a[1] = 2.5; a.push(i); s += a.length`.

- GIL off / GIL on (3.13 in time). In steady state no relabel happens GIL off: T4-P has promoted the literal's site, the array is born
  CopyOnWrite Double (the push operation called is `operationArrayPushDouble`). Instructions per iteration in FTL steady state: `main`
  584, flag off 658, GIL on 654, GIL off 1,535. The GIL-off extra is array growth that copies: `tryMaterializeCopyOnWriteButterflyForSharedWrite`
  988 samples of 3,837 (4 M-instruction period), `ensureLengthSlowConcurrent` 658, `putByIndexBeyondVectorLengthWithoutAttributes` 496,
  `casButterfly` 418, `trySetIndexQuicklyConcurrent` 197, `butterflyConcurrentCopyWordsSlow` 176 (the materialized copy has no slack,
  so the push reallocates and copies again). It belongs to the array-growth item, not to this one. A fresh 200k-iteration run (what the
  micro times, warm-up tiers included): 145 M instructions flag off, 324 M GIL off.
- flag off / `main` (1.18 in time, 1.10-1.13 in instructions). Per iteration +74 instructions, by symbol (samples at a 4 M period over
  4e7 iterations, flag off minus `main`): `operationArrayPushDouble` +294 (+29 instructions per call; the function is 710 instructions
  against 326 on `main`: `JSArray::pushInline`'s tagged-word arms are compiled in and cost register pressure on the flag-off path),
  `operationEnsureDouble` +159 (+16; 82 instructions against 29: two tests of the GIL-off process byte, one in the prologue's
  top-call-frame store and one in the epilogue's exception read, one test of `useTaggedButterflies` in `tryMakeWritableDouble`),
  generated code +91 (+9), `WatchpointSet` constructor and destructor +123 (+12: the temporary set of every
  `DeferredStructureTransitionWatchpointFire` now initializes four bytes with relaxed stores and its destructor tests `m_everLinked`),
  `ensureLengthSlow` +58, `convertFromCopyOnWrite` +52, `nonPropertyTransition` -95. `convertInt32ToDouble` itself is unchanged (it is
  inlined into `tryMakeWritableDoubleSlow`, -6). This is the per-operation gate cost of the flag-off inventory, concentrated in a loop
  that makes three runtime calls per iteration; nothing specific to doubles.

#### C-7. `astar-like-nodes` (3.05) is not in this family - `explained` as to membership

No double is involved (objects pushed into `[]`, six property puts per object). 65.9 M instructions per run GIL on, 137.7 M GIL off,
all of the difference in one stretch of generated code (nine addresses within 0x170 bytes hold 52 % of the samples): the put sequence
of `clean(nd)`. It belongs with the tag-predicate / claimed-transition item.

#### Summary of worth

| row | GIL off / GIL on now | what removes it |
|---|---|---|
| stanford-crypto-sha256 | 0.40 (slow population) / 0.72 (fast) | C-D1 removes the slow population; C-D2 or C-D3 make the block array Double again |
| ML | 0.56 | C-D2 (before the first spawn) or C-D3 |
| navier-stokes | 0.86 | C-D2 or C-D3 |
| pbkdf2 0.82, aes 0.67 (the part that is not growth) | | C-D2 or C-D3 (their word arrays are Double on `main`) |
| 16 `array-slice-cow`, the BadIndexingType storms | | C-D2 (single-threaded tests) or C-D3 |

A rough total: if sha256 goes to 0.85, ML to 0.80, navier-stokes to 0.95, aes and pbkdf2 up by a tenth each, the geometric mean over
the 36 tests moves by exp((0.74 + 0.36 + 0.10 + 0.11 + 0.10) / 36) = 1.04: GIL off / GIL on 0.804 -> about 0.84.

### C: Designs

#### C-D1. An exit on a merged local reports to the local's lazy-operand profile

**Status: proposed**

Rule. In `Graph::methodOfGettingAValueProfileFor(currentNode, operandNode)`, a `GetLocal` operand hands out
`MethodOfGettingAValueProfile::lazyOperandValueProfile(node->origin.semantic, node->operand())` also when its origin equals the
exiting node's. The same-origin guard exists because the *bytecode's* value profile and arithmetic profile describe the exiting
node's result, not its operand; the lazy-operand profile is keyed by {bytecode index, operand} and is read back at exactly that key
by `ByteCodeParser::injectLazyOperandSpeculation` when the next parse creates the `GetLocal`, so it cannot be confused with the
result. Nothing else in the function changes.

Who writes, who reads. Writer: the OSR exit ramp (DFG and FTL exits compiled by `OSRExit::compileExit`, which already knows how to
store into a lazy-operand profile). Reader: the DFG bytecode parser on the compiler thread, through the profiled block's
`CompressedLazyValueProfileHolder`. No tier below the DFG is touched.

Memory ordering. The bucket store is the racy, word-atomic profile store of SPEC-jit 5.7.4 (aligned 64-bit); nothing is published.
Identical on arm64.

Collector, stops, watchpoints: none involved. A second thread observes nothing but a profile bucket.

Flag off. This is a change to `main`'s profiling rule. Two ways to land it: (a) gated on `Options::useTaggedButterflies()` so that flag
off and GIL on compile exactly what they compile today; (b) unconditional, as an upstream-quality fix (the reproducer storms on
`main`). Recommendation: (a) in the threads branch, (b) proposed upstream separately; (b) needs a flag-off JetStream pass because it
changes what recompilations see.

Failure modes. A wrong key would pollute another local's speculation: detected by the test below (the healed compile must widen
`loc11` and nothing else) and by `--validateGraph`. A profile that now always widens could cost int32 speculation on a local that
sees a double once: the same trade every other exit-feedback path makes; bounded by one exit batch.

Tests. `JSTests/threads/jit/exit-on-merged-local-feeds-lazy-operand-profile.js`: the reproducer's loop on a Contiguous array with one
int52-sized lane; counts exits of the function with `$vm`-free means (`numberOfDFGCompiles`, a counter in a catch-free exit
observer) - before: 302-703 exits and 3-4 DFG compiles, after: at most 101 + the unrelated sites. Run in all four modes.

Verification. The corpus, JSC stress `exit-*`/`osr-*` directories flag off if landed unconditionally, sha256 ten runs GIL off (all in
the 10.03 G population).

Expected gain. sha256 GIL off: the slow population (about two runs in three) disappears: 10.47 G -> 10.03 G instructions and the
first-iteration time from 7 ms to 2-3 ms; suite score from 0.40 to about 0.72 of GIL on. typescript's thirteen-batch storm is the
same disease at a hoisted check and is not cured by this rule (the hoisted check's origin is the function entry).

Risks. Low. Alternatives rejected: widening the `GetByVal` value profile to several buckets (memory in every profiled load, and the
harvest is still a lottery); teaching Contiguous `GetByVal` to distrust an Int32-only profile GIL off (costs every int32 array loop a
double path).

#### C-D2. Before the first Thread is spawned, a GIL-off process relabels in place as GIL on does

**Status: proposed**

Rule. While `g_jscAnyJSThreadEverSpawned` is 0 (set by `ThreadManager::allocateSpawnedThreadState` before the first spawned Thread
exists, never cleared), a GIL-off process has one running mutator at a time - carriers enter the VM under the API lock - which is the
premise of T4-O's GIL-on leg. In that state:
1. `JSObject::relabelIndexingShapeConcurrent`'s owner leg admits every (source, target) pair (`ownerLegAllowed` gains
   `|| !anyJSThreadEverSpawned()`), Int32->Double is not substituted, no allocation-site note is taken, and `publishAndFireInOneStop`
   is false (there is no second thread to hold a stale elided check).
2. T4-C is off: `JSArray::fastSlice`, the `concat` fast paths (`tryConcatAppendArrayFastWithWatchpoints`,
   `tryConcatMultipleArraysFast`, `tryConcatAppendOneNonArray`), `appendMemcpy`'s Double leg and `Butterfly`'s boxed-copy helper produce Double
   copies of Double sources.
3. The collector's defensive value-visit of Int32 lanes (`JSObjectWithButterfly::visitButterflyImpl`, `visitElements`, the
   `ALL_WRITABLE_INT32_INDEXING_TYPES` arm, I41(d)) is off, because it is the one reader that is not a mutator and that would decode
   lanes an in-place Int32->Double rewrite is turning into raw doubles. The byte is read (acquire) BEFORE the early structure read of
   the visit: every in-place rewrite happens-before the byte's release store (same thread), so a visit that reads 1 reads a structure
   that already says Double for every array rewritten in place, and a visit that reads 0 does not value-visit Int32 lanes.
4. Generated code is unchanged. The GIL-off array-mode rules (`ArrayMode::fromObserved`'s Generic arms, the `ArraySlice` admission, the
   I41 lane checks) are conservative, not unsafe, and code compiled before the spawn keeps running after it. `Arrayify(Double)` and
   `operationEnsureDouble` succeed before the spawn because the C++ under them converts.

After the first spawn nothing is relabelled in place any more; arrays that are Double stay Double (shared Double is supported,
R-DOUBLE) and leave Double under the stop, as today.

Who writes, who reads. Writer of the byte: the spawning thread, holding the VM's entry token, before the native thread is created
(release). Readers: the relabel driver and the copy paths on a mutator (relaxed: only a mutator can spawn, so the reader is the writer
or is ordered after it by the API lock or by thread creation), the marker (acquire, ordered as in 3).

Memory ordering. x86-64: the release store is a plain store, the acquire load a plain load. arm64: `stlr` / `ldar` (or `ldapr`) on one
byte; the marker's visit gains one acquire load per array visit GIL off only.

Collector. Concurrent marking sees an in-place rewrite exactly as `main`'s marker does: Int32 and Double lanes are not visited, the
structure seqlock brackets the butterfly read. The shared-heap stop is not involved.

What a second thread can observe. There is none while the rule is active. The first spawned thread starts after the byte is set, which
is after every in-place rewrite completed; it finds arrays in settled shapes.

Flag off. Nothing: `relabelIndexingShapeConcurrent` and the T4-C arms are reached only with tagged words, and the marker's arm is
already behind `jsThreads && gilOffProcess`.

Failure modes. (a) A path that spawns without passing `allocateSpawnedThreadState`: none exists (it is the only constructor of a
spawned `ThreadState`); assert in `ThreadManager::spawn`. (b) A reader of lanes that is neither a mutator nor the marker (a compiler
thread folding elements, the sampling profiler): audited - the DFG folds only copy-on-write literals, whose storage is never rewritten.
(c) The marker ordering of rule 3 forgotten: a wild pointer on the mark stack; detected by the gc-stress matrix with
`--collectContinuously` on the test below, Debug and TSan.

Tests. `objectmodel/double-relabel-in-place-before-first-spawn-gil-off.js`: the four birth patterns of `shapes.js` reach
`ArrayWithDouble` before any spawn and `slice`/`concat` of a Double source stay Double; then a Thread is spawned and the same sites
produce today's shapes, existing Double arrays are read correctly from the new thread, and a Double->Contiguous conversion of a
pre-spawn array takes the stop. Counts: relabel stops 0 before the spawn; `$vm.indexingMode` strings before/after. Plus the JSC stress
files `array-slice-cow.js` (16 results) GIL off.

Verification. Corpus in four modes (most tests spawn at once, so the rule is dormant; the new test and the JSC stress suite GIL off are
what exercise it), gc-stress matrix on the new test, JetStream GIL off instruction sweep.

Expected gain (JetStream GIL off is measured without a spawn). navier-stokes 10.6 G -> about 5.5 G instructions (kernel 82 -> 43 per
iteration); ML 53 G -> about 33 G (kernel 43.7 -> 17.4); sha256's block array is Double again (no storm even without C-D1); aes and
pbkdf2 lose their boxed arithmetic; the 16 `array-slice-cow` results and the BadIndexingType storms of single-threaded runs go. About
+4 % on the GIL-off JetStream total.

Risks. It moves the benchmark configuration without helping a program that has spawned a thread; it must be presented as what it is -
the first slice of "a process that has not spawned runs `main`'s forms" (LANDING-PLAN's item on tag predicates), chosen because this
slice needs no code invalidation at the spawn. Bun's test lanes use a keep-alive Thread from the preload, so they never see it.

Alternatives rejected. Keying on "exactly one live Thread" instead of "never spawned": a joined thread leaves no stale reader, but the
marker argument and the code compiled by the departed thread make the transition back harder to argue, for a case (a program that
spawned and is single-threaded again) nobody measures. Relaxing the JIT's array-mode rules before the spawn: needs a watchpoint fired
at the spawn to retire that code, which is the larger design.

#### C-D3. Encoding-changing shape transitions as copies, readers validate the storage they loaded (history 29, finished)

**Status: proposed**

This is what lets T4-O's substitution, T4-C, the Generic fallback of SPEC-jit history 42 and the per-array Double->Contiguous stop go
away for the owner of an array while other threads run. Two variants of the reader's validation are specified; the writer is the same.
The recommendation is variant W.

##### Invariants

- E1. Published indexed storage never changes lane encoding. Int32->Double and Double->Contiguous on a flat butterfly allocate a new
  butterfly; the old one is abandoned unmodified. (Int32->Contiguous and Undecided->X keep their in-place, stop-free owner leg: the
  first preserves the encoding, the second has no readable lanes.) In-place rewrites remain only inside a stop (foreign, shared-write
  and segmented objects, `haveABadTime`), where the heap-fact epoch retires every frame that holds the old storage.
- E2. A reader decodes a lane as a raw double, or as a JSValue under an Int32 key, only after validating the storage it loaded against
  the shape it keyed on. A reader keyed on Contiguous or ArrayStorage needs no validation: shapes move only upward
  (Undecided < Int32 < Double < Contiguous < ArrayStorage), the writer publishes the word before the header, and the reader loads the
  word after the header (F7's dependency on arm64), so a reader that saw Contiguous loads JSValue-encoded storage.

##### Writer (the owner leg of `relabelIndexingShapeConcurrent`; predicate unchanged: flat word (currentTID, SW=0), storage present, not copy-on-write, precise allocations allowed)

1. Derive `S'` (`Structure::nonPropertyTransition`, may allocate and park); allocate the new butterfly through
   `Butterfly::tryCreateUninitialized` with the old vector length, property capacity and pre-capacity (may collect and park).
   Re-validate the plan after both: structure ID still `S`, table snapshot still matches, word unchanged.
2. Poll-free window (`AssertNoGC`; the claim is lock-free when both thread-local sets of `S` are valid by fresh loads, else under the
   cell lock, exactly as today): CAS the structure ID `S -> nuke(S)`; re-load the word and require it unchanged (an SW flip or an
   install landed: un-claim, take the stop leg).
3. Fill the copy from the old lanes, which are stable now (the owner is here; a foreign writer needs SW=1, which the claim excludes):
   out-of-line properties and the indexing header word for word; Int32->Double: `isInt32 ? double(int) : PNaN`; Double->Contiguous:
   `d == d ? box(d) : empty`. Slack up to the vector length is hole-filled in the new encoding.
4. `storeStoreFence`; CAS the word to {copy, same TID, SW=0} (variant W: with the raw-double bit of the target encoding). A failed CAS
   cannot happen (the ID lane is ours and the word was re-checked under it); it is asserted.
5. `storeStoreFence`; publish the header with the indexing byte BEFORE the structure ID: CAS-merge the new shape into
   `m_indexingTypeAndMisc` (the lock bits share the byte), `storeStoreFence`, store `S'` (un-nukes). `JSCell::setStructure` stores the
   ID first today; the relabel needs its own publication helper with the reversed order (or one 64-bit CAS loop over the header, which
   variant H requires).
6. `vm.writeBarrier(this)`. The deferred transition-watchpoint fire runs at scope exit as for any transition.

Int32->Double of a copy-on-write source folds into the materialization: the materialized copy is written in Double encoding directly
(one allocation instead of two). `CopyOnWriteArrayWithDouble` words carry the bit in variant W.

Cost: one allocation and one pass over the vector per transition, where `main` makes one pass in place and today's GIL off makes none
(Int32->Double substituted) or a stop (Double->Contiguous).

##### Reader validation, variant H (header re-check; history 29 as recorded)

Shape-then-storage-then-shape: `id1 = structureID` (not nuked), the shape check as today, load the word, load-load order, `id2 =
structureID`, require `id1 == id2`. Readers that key on the indexing byte alone must take the byte and the ID from one 64-bit header
load, and the writer must publish ID and byte with one 64-bit CAS: a byte re-check alone does not see the nuke, and with separate
stores a reader can pair a fresh ID with a stale byte. In the DFG this is `CheckArrayAfterStorage(array, storage)` after every
`GetButterfly` whose user has an Int32 or Double mode: reads `JSCell_structureID`/`JSCell_indexingType`, takes the storage as an
operand so CSE cannot fold it into the earlier check, exits BadIndexingType; clobberize gives it the reads of `CheckArray` plus a
dependency on the storage node; LICM hoists the pair (SPEC-jit history 39's argument carries over: a hoisted {storage, check} pair
stays consistent because the old storage is never re-encoded). C++ readers that load the word first need word-shape-word instead.
Cost per butterfly load outside loops: a 4- or 8-byte load, compare, branch, and on arm64 a `dmb ishld` (or a fake address dependency
from the word into the header address) between the word load and the re-check.

##### Reader validation, variant W (the encoding travels in the word; the follow-up history 27 recorded)

The tagged butterfly word gets a raw-double bit `R` = bit 0 of the payload (butterfly pointers are 8-byte aligned). `R` is set in
every word, flat or segmented, that points to raw-double lanes (`ArrayWithDouble`, `CopyOnWriteArrayWithDouble`, non-array Double
shapes) and clear otherwise. `butterflyPointerMask` becomes `0x0000fffffffffff8`, so every existing mask-and-use site strips it for
free. It exists only with tagged words (`Options::useTaggedButterflies()`): an untagged process uses the pointer unmasked and never
sets it.

- Double-keyed reader or writer of lanes: requires `R == 1` in the word it took the base from. Int32-keyed: requires `R == 0`.
  Mismatch: the slow path (LLInt, Baseline stubs, C++: re-dispatch on fresh state) or an exit with BadIndexingType (DFG/FTL).
  Contiguous-, ArrayStorage- and Undecided-keyed accesses: nothing (E2).
- No second header load, no fence: the validation is a test of a register the reader already holds, and it is hoisted with the
  `GetButterfly` it belongs to.

Per tier:
- LLInt (`LowLevelInterpreter64.asm`, the `.op*Threaded` blocks of get_by_val, put_by_val, in_by_val, and the array arms of the
  iterator and enumerator ops): after `loadp m_butterfly` and the read/write predicate, compare `word & 1` with `shape == DoubleShape`
  (shape from the byte already in a register); unequal -> the op's slow path. Int32Shape additionally requires 0. This is the
  "re-validate the shape from data derived after the butterfly load" that the frozen premise at those sites asks for.
- Baseline and the handler ICs (`InlineCacheCompiler`: IndexedInt32Load/Store, IndexedDoubleLoad/Store, IndexedInt32InHit,
  IndexedDoubleInHit, the put_by_val Replace forms; the choke points `CCallHelpers::loadButterflyForRead/ForWrite`): the choke point
  returns the tagged word in a scratch before masking; the Int32 and Double cases test bit 0 and fail to the next handler.
- DFG (`SpeculativeJIT::compileGetButterfly`, the segmented-aware helpers, `compileArrayPush/Pop/Shift/Unshift`,
  `compileDoublePutByVal*`, `compileGetByValSegmentedAwareDouble`, `jumpSlowForUnwantedArrayMode`/`checkArray`, `compileArrayIndexOf`,
  `compileHasIndexedProperty`, `compileArraySlice`, `compileSpread`'s fast path) and FTL (`compileGetButterfly`,
  `threadedButterflyLoadForRead/ForWrite`, `compileGetByValImpl`, `compilePutByVal`, `compileArrayPush/Pop/Shift/Unshift`,
  `compileArrayIndexOfOrArrayIncludes`, `compileHasIndexedPropertyImpl`, `compileEnumeratorNextUpdateIndexAndMode`,
  `compileMultiGetByVal`/`compileMultiPutByVal` per arm, `compileArraySlice`): `GetButterfly` gains an OpInfo
  `ExpectedLaneEncoding { Any, JSValueLanes, RawDoubleLanes }` set by the fixup phase from the array mode of the node it is planted
  for (`Array::Double` -> RawDoubleLanes, `Array::Int32` -> JSValueLanes, everything else Any); its lowering tests the bit before
  masking and exits BadIndexingType. `clobberize`: unchanged reads (`JSObject_butterfly`), the def is keyed by {cell, encoding} so an
  `Any` load does not stand in for a checked one; LICM and the poll set treat it as today's `GetButterfly` (history 39, 50). Nodes that
  load the word themselves test it themselves.
- C++: every decode goes through the word it loaded. `untaggedButterfly(word)` stays; two checked views are added,
  `rawDoubleLanes(word)` (null unless `R`) and `jsValueLanes(word)` (null if `R`), and `Butterfly::contiguousDouble()` is reachable in
  tagged code only through the first (enforced by making the accessor's tagged-mode callers go through the view, so that a missed site
  is a compile error rather than an audit finding). The audit table below lists the functions.

Writers of words. Every constructor of a tagged word passes the encoding: `encodeButterfly(butterfly, tid, sharedWrite, rawDoubles)`
(33 call sites in `ConcurrentButterfly.cpp` 12, `JSObject.cpp` 7, `JSObjectInlines.h` 5, `ConcurrentButterfly.h` 3,
`JSArrayBufferView.cpp` 2, `JSObject.h`, `JSArray.cpp`, `AssemblyHelpers.{h,cpp}`, `FTLLowerDFGToB3.cpp`), and
`encodeSegmentedButterfly(spine, rawDoubles)`. Growth and copy paths take it from the word they replace
(`ensureLengthSlowConcurrent`, `casButterfly` callers, the flat->segmented conversion, the SW-flip DCAS keeps the payload bits as they
are). Generated allocations of Double arrays or butterflies (`SpeculativeJIT::compileNewArray`, `compileAllocateNewArrayWithSize`,
`compileNewButterflyWithSize`, `emitAllocateRawObject`, FTL `compileNewArray`, `allocateJSArray`, `compileMaterializeNewArrayWithButterfly`,
`compileMaterializeNewObject`, `compileNewArrayBuffer` for a CopyOnWrite Double literal, and the OSR materializers
`operationMaterializeObjectInOSR`/`operationPopulateObjectInOSR`) or the bit into the word they stamp the TID tag into. Undecided->Double
in place sets the bit with a word CAS inside the claimed window. The in-stop relabels rewrite the bit with the lanes.

Why a missed site fails safe. A word constructor that forgets the bit yields a Double array whose Double-keyed readers always take the
slow path (a performance bug the validator below reports); a lane decoder that forgets the check is the unsafe kind, and that is
what the typed views make a compile error. A bit set on JSValue storage needs a site that copies a word's bit while changing the
encoding; the validator catches it at the next stopped visit.

##### Memory-ordering argument

Writer order: nuke (CAS) -> lanes of the copy -> [release] word -> [release] indexing byte -> [release] structure ID.

Variant W. *Double- or Int32-keyed access*: the bit and the base arrive in one load, so whatever shape the reader validated and
whenever, it decodes storage in the encoding the word declares, and E1 says that storage is never re-encoded: a stale word is old
values, never mis-typed values. No ordering between the shape load and the word load is needed for type safety on either
architecture. *Contiguous-keyed access*: needs header-before-word. x86-64: load order. arm64: the existing F7 dependency
(structure ID or indexing byte -> butterfly address) which tagged code already emits for `CheckStructure`-then-`GetButterfly`; the
implementation must confirm it is also emitted where the key is the indexing byte (`CheckArray`, the IC stubs, LLInt), or let those
readers test `R == 0` on arm64 (`tbnz`, no barrier). *Lanes of the copy* are read through the pointer obtained from the word: an
address dependency, ordered on arm64 against the writer's release of the word.
Variant H needs load-load order twice (shape -> word -> shape): free on x86-64, a `dmb ishld` per validated load on arm64 unless
address dependencies are threaded through both.

##### The collector, stops, watchpoints, deferred claims

- Concurrent marking: `visitButterflyImpl`'s structure seqlock sees the nuke; BEFORE (old structure, old butterfly), AFTER, or didRace.
  Double lanes are never value-visited; a fresh Contiguous copy of a Double array holds only numbers until its first barriered store;
  the object is re-greyed by step 6. The old butterfly dies unless a frame still holds it (conservative scan). Variant W adds an
  assertion, not a dependency: world-stopped visits check `R == hasDouble(shape)` under `verifyConcurrentButterfly`.
- Shared-heap stop: the allocation in step 1 can park; the plan is re-validated. The window of steps 2-5 is poll-free and O(vector
  length) long (about 5 us for navier-stokes' 16,900 lanes), during which other threads' slow paths see a nuked ID and spin, as they do
  for today's in-place owner leg.
- Thread-local sets: unchanged (the claim is lock-free while both are valid, cell-locked otherwise).
- Deferred claims (SPEC-jit 5.6, precondition 10): a relabel out of Double is on the publish-and-fire-in-one-stop list because a stale
  consumer stored raw doubles into boxed storage. Under E2 that consumer fails its validation on the new word and exits, so the owner's
  copy no longer needs the stop. Keep the rule for the first landing (the foreign and in-stop legs need it anyway) and drop the owner
  leg from it once the amplifier has run on `jit/unsafe-transitions-publish-and-fire-in-one-stop-gil-off.js` with the rule off.
- `haveABadTime`, foreign and shared-write relabels: unchanged (stop, in place, epoch bump); they also rewrite the bit.

##### What a second thread can observe mid-way

Before the claim: the old array. Between nuke and publication: a nuked structure ID (generic C++ readers retry; generated code that
decodes through `JSCell::structure()` sees the pre-transition structure) with either word; an Int32- or Double-keyed fast path that
loads the new word fails its validation and goes to the slow path, which waits out the nuke. After publication: the new array. A
reader that holds the old butterfly (hoisted across polls) keeps reading the values of the moment of the copy for as long as its
loop runs - the staleness the poll-visibility rule already allows for data reads - and never a mis-typed lane.

##### What is withdrawn when it lands

T4-O's Int32->Double substitution and `noteSubstitutedDoubleRequestGILOff`; T4-C (copies of Double sources are Double again);
the per-array Double->Contiguous stop for owners; `ArrayMode::fromObserved`'s `convertsDoubleArrays` Generic arm (the ArrayStorage arm
stays); the `ArraySlice` Double exclusion. T4-P's promotion becomes a plain profile (`main`'s). I41's lane check on Int32-mode reads
stays (Int32->Contiguous is still in place).

##### Flag off

Nothing in generated code or C++ fast paths: the bit, the mask constant and every validation sit behind the tagged-word predicate
that already selects the tagged access forms. One unconditional change: `encodeButterfly`'s signature (a constant `false` flag off).

##### Failure modes and detection

| failure | symptom | detection |
|---|---|---|
| a lane decoder without validation (W: bypasses the typed view; H: no re-check) | raw double decoded as JSValue: forged pointer | W: compile error by construction; both: the canary below; amplifier on `objectmodel/double-transitions-vs-foreign-readers-gil-off.js` |
| a word constructor drops `R` | Double array permanently on slow paths / exit storm | `validateTaggedButterflyWord(word, shape)` at every publication under `verifyConcurrentButterfly`; the stopped GC visit asserts `R == hasDouble(shape)`; exit counts in the test |
| `R` set on boxed storage | Double-keyed reader decodes boxed lanes | the same two assertions |
| publication order (ID before byte) | a byte-keyed reader pairs the new ID with the old shape | H only; TSan plus a Debug delay injected between the two stores |
| fill before claim | a foreign store lost | assertion that the word is re-checked under the claim; `objectmodel/` race test with an SW flip storm |

Lane-encoding canary (Debug and TSan builds): with `--verifyConcurrentButterfly`, a Double butterfly's pre-capacity word (or a side
table keyed by butterfly base) records its encoding at allocation, and every checked view asserts it against the way it is about to
decode; a copy path asserts source and destination encodings. Cheap enough for the amplifier's Debug lane.

##### Audit table: C++ lane decoders in the current tree

132 uses of `contiguousDouble()`, 8 of `indexingPayload<double>`, 61 `ALL_DOUBLE_INDEXING_TYPES` arms, 28 `case ArrayWithDouble`, 5
`case DoubleShape`, in 140 functions outside B3. Classes: **W** runs or is part of the transition protocol; **F** initializes a fresh,
unpublished object (must stamp `R`, needs no validation); **O** owner-only by construction (an `InPlaceWrite` snapshot requires the
word to be (currentTID, SW=0), and only the owner relabels stop-free); **R** reads or writes lanes of an array another thread may own:
needs the validation, through the snapshot helper where there is one; **G** flag-off / untagged body (tagged processes route to a
`*Concurrent` sibling first); **S** runs inside a stop or under the cell lock with re-verification; **N** no lane access (type algebra,
sizes, dumps).

| file | functions | class |
|---|---|---|
| runtime/JSObject.cpp | `relabelIndexingShapeConcurrent`, `convertInt32ToDouble`, `convertDoubleToContiguous`, `convertUndecidedToDouble`, `convertToIndexingTypeIfNeeded`, `convertFromCopyOnWrite`, `tryMakeWritable{Int32,Double,Contiguous}Slow`, `convertDoubleToArrayStorage`, `convertToArrayStorageConcurrent`, `ensureArrayStorageSlow`, `ensureArrayStorageExistsAndEnterDictionaryIndexingMode`, `enterDictionaryIndexingMode`, `switchToSlowPutArrayStorage` | W (the ArrayStorage conversions are S: they stay under their stop) |
| runtime/JSObject.cpp | `createInitialDouble`, `createInitialIndexedStorageConcurrent`, `tryCreateInitialForValueAndSetConcurrent` | F |
| runtime/JSObject.cpp | `canGetIndexQuicklyConcurrent`, `getIndexQuicklyConcurrent`, `tryGetIndexQuicklyConcurrent`, `trySetIndexQuicklyConcurrent`, `setIndexQuicklyConcurrent` | **R** - the five funnels of the generic paths; they load the word first and then the shape, so W is one comparison each; H is word-shape-word |
| runtime/JSObject.cpp | `getOwnPropertySlotByIndex`, `putByIndex`, `deletePropertyByIndex`, `getOwnIndexedPropertyNames`, `getEnumerableLength`, `countElements`, `countElementsIn`, `putByIndexBeyondVectorLength`, `putByIndexBeyondVectorLengthWithoutAttributes`, `putDirectIndexSlowOrBeyondVectorLength` | R in their tagged arms where they touch lanes themselves (`deletePropertyByIndex`, `getOwnIndexedPropertyNames`, `getEnumerableLength`, `countElements`, `putByIndexBeyondVectorLengthWithoutAttributes`); the others dispatch to the five funnels; their untagged arms are G |
| runtime/JSObject.cpp | `ensureLengthSlow`, `reallocateAndShrinkButterfly` | G (tagged: `ensureLengthSlowConcurrent`) |
| runtime/JSObject.h | `canGetIndexQuickly`, `getIndexQuickly`, `tryGetIndexQuickly`, `setIndexQuickly`, `trySetIndexQuickly`, `ensureLength` | G (each routes to its `*Concurrent` sibling with tagged words) |
| runtime/JSObject.h | `tryMakeWritableDouble` (fast arm returns the lanes of the loaded word) | R: return null unless `R` |
| runtime/JSObject.h, JSObjectInlines.h | `hasSparseMap`, `inSparseIndexingMode`, `canHaveExistingOwnIndexed*`, `forEachOwnIndexedProperty` | N / dispatch to funnels |
| runtime/JSObjectInlines.h | `initializeIndex`, `initializeIndexWithoutBarrier` | F (inside an ObjectInitializationScope) |
| runtime/ConcurrentButterfly.cpp | `ensureLengthSlowConcurrent`, `shrinkButterflyForSetLengthConcurrent`, `tryGrowSegmentedVectorLength`, `convertToSegmentedButterfly`, `tryMaterializeCopyOnWriteButterflyForSharedWrite`, `materializeCopyOnWriteButterflyConcurrent` | W: copy the encoding (and `R`) of the word they replace; the materializer may change it when asked for Double |
| runtime/ConcurrentButterfly.cpp | `putIndexConcurrent`, `butterflyConcurrentStore`, `atomicSlotReadModifyWriteAtIndex` (+ `ThreadAtomics.cpp` `putDirectIndexForAtomicsMissingAdd`) | R |
| runtime/ConcurrentButterfly.cpp | `visitSegmentedButterfly`; runtime/JSObject.cpp `visitButterflyImpl`; runtime/JSCellButterfly.cpp `visitChildrenImpl` | collector: seqlock; add the `R` assertion world-stopped |
| runtime/Butterfly.h, ButterflyInlines.h | `bumpPublicLengthToAtLeast`, `clearRange` | N (no decoding) / called by W |
| runtime/JSArray.cpp | `jsThreadsFlatSnapshot` | **R choke point**: returns the word's encoding with the snapshot; the 11 callers key their decoding on it |
| runtime/JSArray.cpp | `fastSlice`, `fastToReversed`, `fastWith`, `fastToSpliced`, `fastFlat`, `fastFlatIntoBuffer`, `calculateFlattenedLength`, `fillArgList`, `copyToArguments`, `tryCloneArrayFromFast`, `appendMemcpy` (source side), `canDoFastIndexedAccess` | R through the snapshot (Read intent) |
| runtime/JSArray.cpp | `fastFill`, `fastShift`, `shiftCountWithAnyIndexingType`, `unshiftCountWithAnyIndexingType`, `appendMemcpy` (destination side), `setLength`, `pop` | O (InPlaceWrite snapshot or owner leg); `pop` and `setLength` take their own snapshot: R for the lane they read |
| runtime/JSArray.cpp | `tryCreateUninitializedRestricted`, `eagerlyInitializeButterfly`; JSArrayInlines.h `tryCreate` | F |
| runtime/JSArrayInlines.h | `pushInline` | R (dispatches on the word; Double arm requires `R`) |
| runtime/ArrayPrototype.cpp, ArrayPrototypeInlines.h | `arrayProtoFuncReverse`, `sortCompact`, `canDoFastIndexedAccess`, `fastArrayJoin`/`canUseFastArrayJoin`, `tryConcatAppendArrayFastWithWatchpoints`, `tryConcatMultipleArraysFast`, `tryConcatAppendOneNonArray` (through `flatButterflySnapshot`, about 14 callers across ArrayPrototype.cpp, ArrayPrototypeInlines.h, JSONObject.cpp, StringConstructor.cpp and DFGOperations.cpp, or a hand-written one-load snapshot) | R: second choke point `flatButterflySnapshot`; `arrayProtoFuncReverse` and `sortCompact` write in place: O after the owner test they already make |
| runtime/ArrayConstructor.cpp | `fastArrayOf`, `tryCreateArrayFromSet`, `tryCreateArrayFromSetIterator`, `tryCreateArrayFromMapIterator`, `forEachArgumentsElement` | F (results); `forEachArgumentsElement` reads a source: R |
| runtime/StringConstructor.cpp `stringRaw`; runtime/JSGenericTypedArrayViewInlines.h `copyFromDoubleShapeArray`; runtime/JSCellButterfly.h `createFromArray`; runtime/ScopedArguments.cpp `fastSlice`; runtime/JSONObject.cpp (2 snapshot callers) | hand-written one-load snapshots | R |
| runtime/JSCellButterfly.h | `get`, `setIndex` | immutable storage: F at creation, reads need nothing (a JSImmutableButterfly never changes encoding and carries its own indexing mode) |
| runtime/CachedTypes.cpp | `encode`, `decode` | F / immutable |
| runtime/IndexingType.{h,cpp}, IndexingHeaderInlines.h, StructureTransitionTable.h, bytecode/ArrayProfile.h, ArrayAllocationProfile.cpp, dfg/DFGArrayMode.h | type algebra, sizes, profiles | N |
| dfg/DFGOperations.cpp | `operationArrayIndexOfDouble`, `operationArrayIndexOfValueDouble`, `operationArrayIncludesDouble`, `operationArrayIncludesValueDouble` (storage handed in by generated code, bound from `searchableLengthOfStorageFromJIT`) | covered by the generated `GetButterfly` validation: the storage argument was validated as RawDoubleLanes |
| dfg/DFGOperations.cpp | `operationArrayShiftElementsDouble`, `arraySpliceImpl`, `operationEnsureDouble` | R / W (re-derive from the object) |
| ftl/FTLOperations.cpp | `operationMaterializeObjectInOSR`, `operationPopulateObjectInOSR` | F |
| jit/JITOperations.cpp | `directPutByVal` | dispatches to the funnels |
| bytecode/Repatch.cpp | `tryCacheArrayGetByVal`, `tryCacheArrayInByVal`, `tryCacheArrayPutByVal` | N (select the stub; the stub validates) |
| tools/JSDollarVM.cpp | `functionCpuClflush` | test helper: R |

Counts: R 38 functions (of which 22 go through the two snapshot helpers and 10 through the five funnels), W 21, F 17, O 8, G 9, S 4, N
the rest. The Int32-keyed twins of every R entry (`contiguousInt32()` 16 uses, the `ALL_INT32_INDEXING_TYPES` arms) take `R == 0`
through the same views; most already test `isInt32()` per lane for I41 and then only need the word-level test to tell a hole from
`0.0`.

##### Tests

- `objectmodel/double-transitions-are-copies-gil-off.js`: an owner relabels Int32->Double and Double->Contiguous on arrays that three
  reader threads scan in every tier (LLInt through FTL, `indexOf`, `join`, `slice`, spread, typed-array `set`, `JSON.stringify`);
  every value read must be a number the owner wrote; counts relabel stops (before: one per Double->Contiguous; after: 0) and asserts
  `$vm.indexingMode` reaches ArrayWithDouble for the four birth patterns of C-2/C-3 while a second thread is alive.
- `jit/lane-encoding-validation-exits-gil-off.js`: a reader thread in FTL code with the butterfly hoisted across polls while the owner
  transitions the array; the reader must exit (BadIndexingType, counted) or keep reading old values, and must never produce a
  non-number.
- `objectmodel/double-word-bit-survives-growth-and-sharing-gil-off.js` (variant W): push past the vector length, foreign first write
  (SW flip), flat->segmented conversion, copy-on-write materialization; after each, Double-mode code does not exit
  (`numberOfDFGCompiles` stable, exit counter 0).
- The existing `objectmodel/double-copies-are-contiguous-gil-off.js`, `double-array-profile-promotion*-gil-off.js`,
  `double-allocation-profile-feedback-gil-off.js` are rewritten to expect `main`'s shapes; `jit/unsafe-transitions-publish-and-fire-in-one-stop-gil-off.js`
  keeps passing.

##### Verification it needs

Corpus in four modes Release and Debug with `--verifyConcurrentButterfly`; TSanJIT both modes; the three new tests under the amplifier
(1,000 runs, pinned-to-one-core lane included: the windows are a few instructions wide); the GIL-off JSC stress suite (it is what found
the T4-P register bug); the mirror harness Release eval pass; gc-stress matrix; an instruction sweep of JetStream GIL off; arm64
cannot be run and is argued above.

##### Expected gain

With threads alive, what C-D2 gives without: navier-stokes' kernel 82 -> 43 instructions per iteration plus the validation (hoisted,
so 0 per iteration) and one 135 KB copy per array per run; ML's kernel 43.7 -> 17.4; sha256's block array Double; no Double->Contiguous
stops (3,900-6,200 per run on ML and the sjcl tests in the eighth round's count). For scaling workloads that do float arithmetic on
arrays born integer (raytrace-like vectors initialised with literals) the same factor of about 1.9 on the array loops.
Cost on code that does not transition: variant W one `test`/`jne` per validated `GetButterfly` outside loops (Int32 and Double modes
only); variant H a load, compare, branch.

##### Risks

The reader sweep across four tiers is the largest single change since the tagged word itself; a missed decoder is memory-unsafe.
Variant W turns the C++ half of that risk into compile errors and makes the JIT half a property of `GetButterfly`'s lowering and of
the dozen nodes that load the word themselves; variant H leaves both to audit and adds an ordering requirement on arm64.

##### Alternatives rejected

- *Double arrays allowed only while the structure's thread-local sets are valid, converted under the firing stop.* The sets track
  foreign transitions (`transitionThreadLocal`) and foreign writes (`writeThreadLocal`); nothing fires when another thread READS an
  array, and the hazard is a foreign reader holding a stale shape check across the owner's in-place rewrite. A third, read-side set
  would have to fire on the first foreign read of any object of the structure, which every array of a realm shares; it would fire at
  once in any threaded program. Rating: unsound as stated, useless when repaired.
- *NaN-checked racy reads over an in-place or copied relabel* (seventh round): a cell pointer's bits read as a subnormal double is an
  address disclosure; rejected then, still rejected.
- *128-bit DCAS of {header, word}*: does not help the reader, who loads them separately (history 25).
- *Re-encode in place under the per-array stop only* (today): 1.9x on the array loops of numeric code, and a stop per Double->Contiguous.
- *Keep the bit in the pointer and bias Double-mode displacements by -1* (no check instruction): a Double-keyed reader handed boxed
  storage would read across two lanes instead of failing; rejected.

##### Staging

1. C-D1 (independent; a day).
2. C-D2 (C++ only; JetStream's configuration).
3. C-D3-W plumbing with no behaviour change: the bit set at every word constructor and allocation site, the mask, the validator and
   the assertions; every Double-keyed reader validates (always true); ship and amplify. Exit and compile counts must not move.
4. C-D3 writer for Double->Contiguous (removes the stop), T4-C withdrawn.
5. C-D3 writer for Int32->Double, the substitution and the Generic arm withdrawn, tests rewritten to `main`'s shapes.
Each of 3-5 lands alone with the battery above.

### C: arm64 / non-Linux notes

- C-D1: none.
- C-D2: the ever-spawned byte is a release store by the spawner and an acquire load in the marker's visit; `stlr`/`ldar`. Mutator
  readers are ordered by the API lock or by thread creation and may stay relaxed.
- C-D3 writer: the nuke is a CAS (needs acquire-release on arm64: `casal`); lanes -> word and word -> header are release stores
  (`stlr`, or `dmb ishst` + `str` as `storeStoreFence` emits); the byte-before-ID order of step 5 is a second `dmb ishst`.
- C-D3-W readers: Double- and Int32-keyed validation needs no barrier (same word). Contiguous-keyed readers rely on header-before-word:
  the F7 dependency (structure ID -> butterfly address; FTL `B3::Depend`, DFG/Baseline `eor`+`add`, LLInt arm64-only, C++
  `WTF::Dependency`). To check in the tree: that the dependency is also emitted when the key is the indexing byte (`CheckArray`, the IC
  array stubs, LLInt's byte dispatch), not only after `CheckStructure`; if it is not, either add it or have those readers test `R == 0`
  on arm64. The lanes of a fresh copy are reached through the word (address dependency): ordered.
- C-D3-H readers: two load-load orderings per validated access; on arm64 a `dmb ishld` (tens of cycles) or a dependency threaded from
  the word into the header's address for the re-check. This is the main reason to prefer W.
- The C++ funnels load the word and then the indexing byte with no fence between them today (`getIndexQuicklyConcurrent` and
  siblings: "E5 None first"); on arm64 the two loads may be reordered. Today's argument does not need the order (no stop-free relabel
  changes encoding); variant W does not need it either; variant H would.
- `relabelIndexingShapeConcurrent`'s settled-shape early return carries an explicit `loadLoadFence` already.
- Non-Linux: nothing in this area depends on the platform beyond what tagged words already require (the per-thread TID tag in
  thread-local storage).

### C: Decisions for the user

1. **How far to go for the Double family.**
   - (a) *Nothing beyond C-D1*: sha256 stops being bimodal (0.40 -> about 0.72); ML, navier-stokes and the rest stay. Cost: a day.
   - (b) *C-D1 + C-D2*: adds about +4 % on the GIL-off JetStream total (navier-stokes about 0.95, ML about 0.80, sha256 about 0.85 of GIL
     on), the 16 `array-slice-cow` results and the single-threaded BadIndexingType storms; C++ only, no tier touched, small audit.
     Buys nothing once a Thread exists, and the report must say so.
   - (c) *C-D3 variant W*, staged: the same gains with threads alive, removes the Double->Contiguous stops, withdraws four special rules
     (T4-O substitution, T4-C, T4-P promotion, the Generic arm); the largest sweep since tagged words (38 C++ reader functions behind 7
     choke points, four tiers, 33 word constructors, a dozen allocation sites), several weeks with its own amplifier campaigns.
   - (d) *C-D3 variant H* (history 29 as recorded): same gains, no new bit, more expensive readers and an arm64 barrier.
   Recommendation: (b) now, then (c) as its own project starting with the behaviour-neutral plumbing stage; not (d).
2. **C-D1 gated or unconditional.** Gated on tagged words it changes nothing flag off; unconditional it is an upstream fix that needs a
   flag-off performance pass. Recommendation: gated here, proposed upstream separately.
3. **Whether to drop the owner's relabel out of Double from the publish-and-fire-in-one-stop list once C-D3 lands.** Recommendation:
   keep it for the first landing, drop it after an amplifier campaign with the rule switched off shows the validation catches every
   stale consumer.

### C: Doc mismatches

- LANDING-PLAN Open items, item (6) and PERF-RESULTS 6.12 attribute sha256's Contiguous message array to T4-O ("item (1)'s family").
  The array is Contiguous by **T4-C**: `bitArray.concat(words, [partial])` is an Int32 array concatenated with a Double literal, and
  `describe()` shows `ArrayWithDouble` GIL on, `ArrayWithContiguous` GIL off for the concat and for the `splice` of it. Also "each
  batch of a hundred": the batches are 101, 201, 401, 801 (the threshold doubles with the reoptimization counter).
- PERF-RESULTS 1.x / the micro table: `array-int32-to-double-relabel-200k` GIL off does not relabel in steady state (the site is
  promoted; the cost is copy-on-write materialization plus growth by copying), and flag off over `main` is per-operation gates, not
  the conversion body. The row's name suggests otherwise; a one-line note under the table would do.
- SPEC-objectmodel history 40 says ML "promoted six sites and did not move either ... its hot loops read rows that arrive from many
  sites in three shapes (Int32, Contiguous, Double)". On the round-10 final tree the third shape is almost gone (Contiguous 3,612 of 167,856 sampled rows);
  what keeps the loops polymorphic is CopyOnWrite Int32 literal rows (38,556) that `main` converts on arrival. Same conclusion,
  different population.
- INTEGRATE-jit.md "FROZEN PREMISE: flag-on, no indexing-shape conversion of a reachable object happens outside a stop-the-world
  window" and the LLInt comment that cites it: T4-O (rev 17) has made Undecided->X and Int32->Contiguous stop-free for the owner since
  the sixth round; the premise that still holds is the narrower "no conversion that changes lane ENCODING happens outside a stop",
  which is what the LLInt comment's argument uses. Reword both.
- SPEC-objectmodel 4.4 T4 says the DFG `ArraySlice` "GIL-off neither plants on Double-typed sites nor admits Double arrays" and
  SPEC-jit 5.8's bullet repeats it; correct, but neither mentions that `concat` with a Double ARGUMENT onto an Int32 receiver is also
  demoted (the case that matters for sha256). Add "or a Double operand".

### C: What was run

All on the existing Release binaries; no build, no suite.
- `icount.sh` (perf stat, instructions and cycles): sha256 six times GIL off, once GIL on / flag off / `main`; navier-stokes and ML GIL on
  and GIL off.
- JetStream single tests with `--printEachOSRExit=1`: sha256 GIL off x6 and GIL on x2; ML, typescript (twice), aes in both modes.
  One sha256 GIL-off run with `--dumpDFGDisassembly=1`.
- Small single-purpose scripts (not kept in the tree; each is described where its numbers are used): `relabel.js` (steady state, four configurations, two lengths), `relabel-micro.js` (fresh
  function per 200k iterations), `linsolve.js`, `mmul.js`, `shapes.js`, `ns-shapes.js`, `sha-shapes.js`, `ml-shapes.js` (ML with a probe
  line added to a copy of its source), `astar.js`, `getlocal-exit-storm{,2}.js`; `perf record -e instructions:u -c 4000037` on
  `relabel.js` in four configurations and on `astar.js` GIL off (the machine's sample-rate cap is 8,000/s: periods below 2 M drop
  samples silently, so the first two attempts at 100 k and 1 M were discarded).
- `objdump --disassemble=operationEnsureDouble|operationArrayPushDouble` on `main`'s and the final binary.
- One batch of GIL-off numbers was first taken through an unsplit shell variable (silently GIL on) and retaken through a bash wrapper
; every GIL-off number above is from the wrapper or from `icount.sh`, verified with `--dumpOptions=2`.

## Section D. Array growth, first writes to copy-on-write arrays, slice and concat, GIL off

Scope: the "array growth that copies" item of the tenth round's Open items (stanford-crypto-aes 0.67 of GIL on;
decision (b), "in-place vectorLength raise as an x86-64-only rule versus segmented growth for owned large arrays"), the
array rows of the micro set (append by index, push, first write to a literal, slice, concat, array-element-read), and
whatever of crypto / gbemu / json is array growth. Release, Linux x86-64, the tenth round's final tree against `main`
at the branch's base. All numbers are `instructions:u` counts, function-entry counts from hardware execute
breakpoints, or the tree's own event counters; none is a wall-clock time.

Method notes that matter for reading the numbers:

- Per-call costs are differences of two runs with different iteration counts (`(I(N2) - I(N1)) / (N2 - N1)`), so
  start-up and warm-up cancel; repeated three times they agree to under 0.5 %.
- Per-symbol shares come from `perf record -e instructions:u -c <period>`. On this machine the kernel had lowered
  `perf_event_max_sample_rate` to 8,000/s, and periods of 100 k - 2 M instructions silently lost 25-50 % of the
  samples (the reported event count was 4.87 G for a run `perf stat` measures at 6.5 G). A period of 4 M loses none
  (event count equals the `perf stat` total). Shares below are from 4 M-period recordings, or, for the whole-test
  recordings taken at 1 M, are scaled by the measured total (the loss was 46 % GIL on and 49 % GIL off, so ratios
  survive). Sampling on `instructions` has skid: a `lock cmpxchg` absorbs the samples of the instructions after it, so
  small functions that contain one (`casButterfly`) are over-counted and their callers under-counted; sums over a
  call chain are reliable, single small symbols are not.
- Function-entry counts: `perf stat -e mem:<address>:x` (a hardware execute breakpoint, user mode, no privileges
  needed) with address-space randomization off gives exact call counts of up to four functions per run. This is the
  cheap replacement for uprobes.

### D: Inventory

#### AG-1. The C++ path of one growth event costs 2.4-2.7 times `main`'s, and none of that is the copy (GIL off) - explained

Mechanism. GIL off an owner's flat butterfly always grows by allocating a fresh butterfly, copying, and publishing with
one 64-bit compare-and-swap of the tagged word (`ensureLengthSlowConcurrent`, the T1 leg; SPEC-objectmodel 4.4 T1).
That is also what `main` does for every butterfly that lives in a MarkedBlock: `JSObject::ensureLengthSlow` calls
`Butterfly::reallocArrayRightIfPossible`, which reallocates in place only a property-less butterfly backed by a
PreciseAllocation while the mutator is not fenced, and otherwise allocates and `memcpy`s. The growth policy is the same
(`nextLength`, 1.5x, then `Butterfly::optimalContiguousVectorLength`); GIL off makes slightly fewer copies than `main`
because its vector lengths are rounded to 4k-1 (an empty literal starts at 7 instead of 5; a four-element slice at 7
instead of 4). What differs is the dispatch around the copy:

- An out-of-bounds indexed store from optimized code enters `operationPutByValBeyondArrayBounds*` ->
  `JSObject::putByIndexInline`, which calls `trySetIndexQuicklyConcurrent` (fails: index >= vectorLength), then the
  virtual `putByIndex` -> `JSObject::putByIndex`'s tagged loop (copy-on-write, ArrayStorage, Undecided, shape tests)
  -> `JSObjectWithButterfly::putIndexConcurrent` (the same tests again, the foreign-writer test, then
  `trySetIndexQuicklyConcurrent` a second time, fails) -> `ensureLengthSlowConcurrent` (copy-on-write and
  ArrayStorage tests a third time, a seq_cst word load, the foreign-writer test, `structure()` with the nuke mask,
  counters, the copy, `casButterfly` out of line, the write barrier) -> back in `putIndexConcurrent`'s loop ->
  `trySetIndexQuicklyConcurrent` a third time, which finally stores the element and raises the length with the
  CAS-max. `push` goes through `putByIndexBeyondVectorLengthWithoutAttributes`' tagged loop and makes two such calls.
- `trySetIndexQuicklyConcurrent` is about 55 instructions before it reaches its shape switch (mode, fence, word, the
  foreign-writer test, which reads the thread's lite from thread-local storage, tests two configuration bytes and
  loads the TID from the lite; the shape and ArrayStorage classification), 65-80 on the paths used here.

Evidence.
- Hardware-breakpoint counts over stanford-crypto-aes, GIL off: `ensureLengthSlowConcurrent` 4,049,892 calls,
  `casButterfly` 4,049,956, `trySetIndexQuicklyConcurrent` 12,153,976 - 3.00 per growth event. The tree's counter
  `ensureLengthFreshCopy` says 4,050,376 events copying 1.816 GB (448 bytes on average: these are arrays of a few
  dozen words). GIL on the same test makes 4,533,948 calls of `ensureLengthSlow`, of which 3,781,303 reach
  `reallocArrayRightIfPossible` (a copy) and 752,645 (16.6 %) are the in-place raise into size-class slack.
- A microbenchmark that appends 28 elements by index to `[]` (one growth event per call in steady state, 27 -> 51
  lanes, counted by `ensureLengthFreshCopy`), instructions per call, `main` / GIL on / GIL off: 1,085 / 1,135 / 1,903;
  with 27 elements (no event) 587 / 595 / 925. So one event costs 498 on `main` and 978 GIL off. By symbol (4 M
  period, per call): `main` - `ensureLengthSlow` 119, `reallocArrayRightIfPossible` 66, the operation 45,
  `putByIndex` 40, `putByIndexBeyondVectorLength` 31 and its shape instance 20, `trySetIndexQuickly` 15, `memcpy` 7,
  allocator about 20: about 360 in C++. GIL off - `ensureLengthSlowConcurrent` 298, `trySetIndexQuicklyConcurrent` 235
  (three calls), `casButterfly` 147 (mostly skid from its own `lock cmpxchg`), `putByIndex` 83, `putIndexConcurrent`
  75, the operation 67, the word copy 18, `trySetIndexQuickly` 16: about 960 in C++. The same event by `push`: 394 on
  `main`, 943 GIL off; with double-valued elements 541 and 995.
- The copy itself is `gcSafeMemcpy` in 64-byte SSE chunks (`butterflyConcurrentCopyWordsSlow`), no dearer than
  `memcpy`; the fence is free on x86-64; the publication is one `lock cmpxchg`.

What it is worth. Growth events per test, GIL off (the 36 JetStream tests run once with the event counters):
stanford-crypto-aes 4,050,376 (at 950 instructions each 13.9 % of the test's 27.7 G instructions),
stanford-crypto-sha256 540,146 (4.9 %), stanford-crypto-pbkdf2 491,704 (4.4 %), delta-blue 115,221 (1.5 %), pdfjs
229,217 (0.8 %), every other test under 0.2 % (crypto 9,214 events, gbemu 13,827, json-parse and json-stringify 15
each: none of crypto's 1.42x, gbemu's 1.33x or the json rows is array growth). In aes the growth chain is 4.0 G
instructions GIL off against 1.65 G in `main`'s chain GIL on: 2.35 G of the 10.96 G that separate the two modes
(16.46 G -> 27.42 G), 21 %.

#### AG-2. Two thirds of stanford-crypto-aes' gap is generated code, and the arrays are Double arrays GIL on - partly explained (belongs with the Double-array entry)

Evidence. Per-symbol difference GIL off minus GIL on over aes (scaled to the true totals): generated code +7.3 G
(67 %), the growth chain +2.35 G (21 %), `arrayProtoFuncSlice` + `JSArray::fastSlice` +0.53 G (5 %), the
copy-on-write materializer and `Structure::nonPropertyTransition` with its deferred fire +0.4 G (3.5 %). Inside the
FTL code (samples binned by DFG node through `--dumpFTLDisassembly` ranges, GIL on -> GIL off): CheckInBounds 299 ->
1,184, GetByVal 1,628 -> 2,499, PutByVal 3 -> 682, ArithBitRShift 161 -> 707, MultiGetByVal 0 -> 378, CheckStructure
242 -> 493, GetVectorLength 0 -> 179, CheckTraps 0 -> 93, Jump/Branch +840. GIL on the test's arrays are Double arrays
(its words exceed int32): the profile shows `putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>` and
`operationPutDoubleByValBeyondArrayBounds*`. GIL off the same arrays are Contiguous holding boxed doubles (17,866 owner
relabels, T4-O), some allocation sites are promoted to Double (135 promotions, T4-P), and the sites that meet both
compile MultiGetByVal. `fastSlice` runs 756,012 times GIL off against 422 GIL on: the DFG does not plant its ArraySlice
intrinsic on a Double-typed site GIL off (T4-C, `DFGByteCodeParser` `ArraySliceIntrinsic`, `noDoubleSliceGILOff`), so
every `slice` of those arrays is a call into C++, about 700 instructions each.

So aes is a member of the Double family (sha256, ML, navier-stokes) first and a growth test second. The
LANDING-PLAN and PERF-RESULTS sentence "half of it in the concurrent growth, store and materialization helpers
(2,250 of 4,422 extra samples)" counts the GIL-off helpers gross; GIL on spends 1,150 samples (same scale) in `main`'s
counterparts (`ensureLengthSlow`, `reallocArrayRightIfPossible`, `convertFromCopyOnWrite`,
`putByIndexBeyondVectorLength*`), so the net is about a quarter.

#### AG-3. Per-element cost of an append inside the vector: 27 instructions against 16 (GIL off, optimized code) - explained, not growth

`a[i] = v` with `i == publicLength < vectorLength` is handled inline in every configuration. Instructions per element
between growth events, `main` / GIL off: by index 16 / 27, by push 15 / 24. The micro rows "append by index 0.85 -> 1.28
G" and "push 0.79 -> 1.12 G" (sixteen elements into `[]`) contain no growth event at all in steady state: the
allocation profile's vector-length hint has grown to 25 (27 GIL off), so the literal is born large enough
(`ensureLengthFreshCopy` stays at its warm-up value). The FTL loop GIL off (Debug build's disassembly, breadcrumb stores
removed): the poll (3 instructions), the array re-loaded from its stack slot, the tagged word re-loaded, the thread's
tag re-loaded from a spill slot, xor, mask, the owner compare and branch (11), both lengths loaded and `min(publicLength,
vectorLength)` formed (4), compare and branch into the hole leg, the hole leg's second compare against vectorLength, the
length store (plain here: the write-thread-local set is watched, so E2 elides the CAS-max), the element store, the
increment and the loop branch. GIL on: load, compare, store, store, increment, branch. This is the tag-predicate and
poll family of the Open items ((7) "everything else"), listed here only so that nobody looks for it in the growth path.
Arrays of 100,000 and 1,000,000 elements built by index: 13.8 / 29.4 and 28.3 / 33.9 instructions per element (`main`
/ GIL off): the large-array case is not worse than the small one; `main`'s PreciseAllocation realloc buys it nothing
measurable in instructions.

#### AG-4. First write to a copy-on-write literal: 798 instructions against 486 (GIL off against GIL on), and 482 against 426 flag off against `main` - explained

Four stores into `[0, 0, 0, 0]` (one materialization). `main` 426, flag off 482, GIL on 486, GIL off 798, stable over
three repetitions. GIL off the owner's materialization is the same function as the foreign one
(`tryMaterializeCopyOnWriteButterflyForSharedWrite`, reached through `materializeCopyOnWriteButterflyConcurrent`): the
structure transition with its deferred watchpoint-fire scope, allocate and copy, then the cell lock, two seq_cst
re-reads, a compare-and-swap that nukes the structure ID, a 128-bit compare-and-swap of {header, word}, the unlock and
two write barriers: 403 instructions per call in the function against 272 in `convertFromCopyOnWrite`, plus 38 in the
word copy, 23 in the driver, 28 in the wrapper, and four atomic read-modify-writes where GIL on has none. Generated
code 131 against 43 (tag predicates on the four stores). In aes: 1,518,510 materializations, +0.4 G instructions with
the transition bookkeeping, 3.5 % of the gap.
Flag off against `main`, +56 (13 %): the FTL code is the same instruction for instruction (normalized Air dumps diff
only in constants and scheduling); the difference is spread over the C++ under it, a few predicted-false tests and
lost leaf-function shapes each - `operationEnsureInt32` 29 -> 82 static instructions (the prologue and epilogue test the
"VM words live in the lite" configuration byte twice and the tagged-word byte once), `WatchpointSet::~WatchpointSet` 15
-> 51 (the deferred-fire scope's temporary set now tests the flag and its ever-linked byte and needs a frame),
`Structure::fireStructureTransitionWatchpoint` 69 -> 99, `Structure::didTransitionFromThisStructure` 7 -> 25,
`JSObject::tryMakeWritableInt32Slow` 89 -> 141, `WatchpointSet`'s constructor with its classification argument. This
is the "spread thin" class of the flag-off list; the row "first write to a literal 0.88 -> 1.00" of PERF-RESULTS 6.12
is this.

#### AG-5. `slice` and `concat` of small arrays - explained

`[...8 ints].slice(0, 4)`: all generated code in every configuration: 143 / 146 / 146 / 237 (`main` / flag off / GIL
on / GIL off). `a.concat(b)` of two four-element literals: 364 / 402 / 404 / 608. Flag off `tryConcatAppendArrayFast
WithWatchpoints` grew from 219 to 254 instructions per call (two `flatButterflySnapshot` calls, three tests of the
tagged-word byte, the GIL-off boxing flag, the re-read of both shapes after the allocation); GIL off it is 348 with its
lambda, and the generated code around it 155 against 55 (the call into the operation is the same; the difference is
allocation through the thread's allocator table, the tag stamp and the rounded vector length of 7 lanes cleared instead
of 4). Nothing here is growth; the flag-off part is 35 instructions in a function that runs once per `concat`.

#### AG-6. `array-element-read` 1.75 (GIL off against GIL on) - explained, not growth

The loop `sum += array[i & 1023]` over a constant array. GIL off the visibility analysis attaches an empty write set to
the poll (the loop's exit reads no heap), so the butterfly, `publicLength` and `vectorLength` are all hoisted; what
stays per iteration is the poll, a second `CheckInBounds` against `vectorLength` next to the one against
`publicLength` (SPEC-jit history section 39), and the Int32 lane check of I41. Nineteen instructions per iteration for
the variant that reads the array from a global, against about eight GIL on.

#### AG-7. A segmented array is thirteen times slower per element in optimized code, and the Baseline inline cache never settles on it - explained (mechanism), recorded for the scaling work

Found while pricing option B below. An array grown under `--forceSegmentedButterflies=1` (what any array becomes once a
second thread grows it) and then read in a hot loop, GIL off: 243.7 instructions per element against 19.0 flat; read
plus write 555.9 against 34.0. Counters for 40 M reads: `icGetByValOptimize` 37,551,438, `icGetByValGaveUp` 0, four FTL
compilations, three jettisons, 1,711 exits of kind BadIndexingType at the GetByVal. The optimizing tiers exit on a
segmented word and the function ends in Baseline, whose get_by_val stub sends a segmented word to its slow case
(`CCallHelpers::loadButterflyForRead`: "Segmented => slow: the generic path performs the dependent spine load"), and
the slow case of a stub is `failAndIgnore`, which leaves the site calling the Optimize operation for good - the
pathology the ArrayLength case of `InlineCacheCompiler.cpp` describes in its own comment and fixes for ArrayLength
only ("a state the stub never handles keeps the site at the Optimize operation forever"). Worth: nothing in JetStream
(no array is ever foreign there); every shared array that a non-owner appends to in the scaling workloads.

### D: Designs

#### D-AG-1. One pass for an owner's append and growth (T1-S)

**Status: proposed.** Recommended; portable; no protocol change.

Rule. SPEC-objectmodel 4.4 T1 is unchanged: an owner's flat resize is a copy published by one 64-bit compare-and-swap
whose expected value is exactly the (currentTID, SW=0) word that was copied from. What changes is where the element and
the length are written and how often the state is re-dispatched.

T1-S, a single straight-line helper (an always-inlined head and an out-of-line growth tail), tried first by
`JSObjectWithButterfly::putIndexConcurrent` and by the tagged arm of
`JSObject::putByIndexBeyondVectorLengthWithoutAttributes` (the `push` route); `JSObject::putByIndexInline`'s tagged arm
calls the helper in place of its leading `trySetIndexQuicklyConcurrent` when the cell's method-table entry is
`JSObject::putByIndex` itself (plain objects and arrays; classes that override `putByIndex` keep the virtual call), so
an append that grows is one call instead of the quick attempt, the virtual `putByIndex` and two nested dispatch loops:

1. `mode = indexingMode()`; require a writable Int32, Double or Contiguous mode and a value the shape admits (int32;
   a non-NaN number; anything). Load-load fence (mode before word, as today).
2. `word = taggedButterflyWord()`; require `((word ^ currentTag) & butterflyTagMask) == 0` and a non-null payload,
   where `currentTag` is the thread's tag word, the one generated code uses (R5): one compare that says flat, owned by
   this thread, SW = 0 (a segmented word carries the reserved TID and SW = 1 and fails it). Anything else: the
   existing dispatch loop, unchanged.
3. `i < vectorLength`: store the lane (relaxed atomic double store, or `setWithoutWriteBarrier`), CAS-max the length
   (release), write barrier. This is today's `trySetIndexQuicklyConcurrent` body for the owner.
4. `i >= vectorLength`, dense policy holds (`i < MIN_SPARSE_ARRAY_INDEX`, not `indexIsSufficientlyBeyondLengthFor
   SparseMap`, `i + 1 <= MAX_STORAGE_VECTOR_LENGTH`): compute the new vector length as today; allocate under the
   `GCDeferralContext` (no collection between the word load and the publication, as today); copy the property prefix,
   the header and the old lanes in 64-bit units; fill `[oldVectorLength, newVectorLength)` with holes (PNaN for
   Double) **except lane `i`, which gets the value**; set the copy's `publicLength` to `max(copied publicLength, i + 1)`
   and its `vectorLength`; store-store fence; compare-and-swap the word from the value loaded in step 2 to the copy
   with the same tag; on success write-barrier the object and return; on failure drop the copy and fall into the
   existing loop (which re-dispatches on the fresh word, never re-copies: I21, I27).

Who writes, who reads. Writers: the owner only, in C++ (the JIT tiers reach it through their existing out-of-bounds
operations and the LLInt/Baseline put_by_val and push slow paths; no generated code changes). Readers: every tier, as
today; they load the word and then the header and lanes through it.

Why storing the element and the length into the private copy is sound. Today the owner publishes a copy that has a
hole at `i` and the old length, then stores the element, then raises the length; every intermediate state is visible.
With T1-S a reader sees either the old word (old storage, old length, no element: the state before the store) or the
new word, through which the element and the raised length are both visible. The only writers of an owned SW = 0 payload
are the owner and a foreign first-writer, and the latter flips SW with a compare-and-swap of {header, word} before its
store lands, which changes the word and fails step 4's compare-and-swap: the torn or stale copy is discarded, not
published, and the generic path stores the element again into whatever the object has become (a second store of the
same value by the same thread is not observable). A foreign raise of `publicLength` needs the same flip first, so the
copied length cannot be behind a published one.

Memory ordering. x86-64: the copy's plain stores, a compiler barrier, `lock cmpxchg` (a full fence). arm64: `dmb ishst`
(the store-store fence) before a compare-and-swap that is itself at least a release; readers reach the header and the
lanes by address dependency from the word (M1, F2), so a reader that sees the new word sees initialized lanes, the
element and the length - which also removes, for this path, the reader-side residual recorded at
`Butterfly::bumpPublicLengthToAtLeast` (a fresh length paired with a stale empty lane on arm64): here length and lane
are both published by the word.

Collector. Unchanged from T1: the copy is auxiliary memory reachable only from the object after publication; the object
is barriered after the compare-and-swap, which re-greys it so the new storage is scanned; a marker that visited the old
storage just before sees a consistent old butterfly (never rewritten). The allocation in step 4 can park this thread
in somebody's stop (the allocator's slow path polls `stopIfNecessaryForAllClients`): everything a stop can do to this
object - a foreign first write, a foreign transition (F2 and segmentation), `haveABadTime`, a per-event relabel of a
shared word - changes the word, so the compare-and-swap is a complete witness, as it is today. The owner's own
stop-free relabels cannot run (the owner is here).

Stop protocol, watchpoints, deferred claims: not involved (no structure changes, no set fires). Thread restriction
(SPEC-api 5.7) and indexed accessors on the prototype chain are not involved either: a restricted object and an object
of a realm that is having a bad time are ArrayStorage-shaped, which step 1 refuses, and `JSObject::putByIndex`'s
restriction check on uncacheable dictionaries stays in front of the helper.

Flag off and GIL on: nothing; all of it sits behind the `Options::useTaggedButterflies()` arms that exist today.

Failure modes and detection. (a) A lost element or a lost foreign store if the copy were published over a flipped word:
excluded by the exact-expected compare-and-swap; `objectmodel/i03-t1-vs-sw-flip.js`,
`objectmodel/i03-b2-stay-flat-growth-vs-sw-flip.js`, `objectmodel/e4c-growth-vs-foreign-segmentation.js`,
`objectmodel/i03-array-resize-cas.js`, `objectmodel/i03-t5-racing-growers.js`, `jit/length-update-races-gil-off.js`
are the standing tests for exactly this and must pass unchanged, in all four modes, under ThreadSanitizer and under
the amplifier. (b) A length published ahead of storage: impossible by construction (one publication). (c) The hole and
out-of-bounds bits of the array profile: the JIT operations pass no profile today and the interpreter and Baseline slow
paths record their bits before calling; unchanged.

Tests. New `objectmodel/owner-append-single-pass-gil-off.js`, run with `--countJSThreadsCounters=1`: 100,000 appends by
index and by `push`, int32, double and object values, on the main thread and on a spawned thread, contents checked;
through `$vm` it reads a new counter `ownedFlatGrowSinglePass` and `ensureLengthFreshCopy`: before the change the
second equals the number of growth events (4 for 256 elements, 6 for 1,000) and the first does not exist; after, the
first equals that number and the second stays at zero for these arrays. The instruction line for the write-up: one
growth event 978 -> at most 450 (the measurement of AG-1 repeated).

Expected gain. The floor is `main`'s chain plus the compare-and-swap and the owner compare: about 400 instructions per
event against 950: 550 saved per event. stanford-crypto-aes 4.05 M events: -2.2 G of 27.4 G, -8 % instructions, its
ratio to GIL on 1.67 -> 1.53 (score about 0.67 -> 0.72 if cycles follow); stanford-crypto-sha256 -2.8 %,
stanford-crypto-pbkdf2 -2.5 %, delta-blue -0.9 %; nothing elsewhere. It also drops three of the four atomic
read-modify-writes of an event that raises the length (the CAS-max disappears into the publication).

Risks. Small: the protocol is T1's; the change is the order of three stores inside a private copy and the removal of
re-dispatch. The shape-specific store logic exists three times already (`trySetIndexQuicklyConcurrent`, `pushInline`,
the beyond-vector-length template); the helper should be the one templated body the three call.

Verification. Corpus in four modes on Release and Debug; TSanJIT both modes; the six tests above under the amplifier
(500 runs each, both modes); the array and indexing files of the GIL-off stress suite; a mirror pass.

Alternatives rejected. D-AG-2 and D-AG-3 below.

#### D-AG-2. In-place vectorLength raise by the owner (decision (b), first option)

**Status: proposed for rejection.** Written out because the decision was recorded as open.

What there is to raise into. A butterfly's `vectorLength` is its capacity: C++ sizes every fresh contiguous butterfly to
fill its size class (`availableContiguousVectorLength`), GIL off rounded down to 4k-1. The only room above a published
`vectorLength` is (a) up to three lanes lost to that rounding, (b) the slack of butterflies that generated code sized
exactly (`ArraySlice`'s result, `new Array(n)` with a known small `n`), (c) nothing for a PreciseAllocation (it is
exact). `main`'s in-place branch exists for (b) ("this is the case where someone else selected a vector length that
caused internal fragmentation") and in aes fires for 16.6 % of the growth calls - raising a four-lane slice result to
five lanes, one store before the real copy. GIL off the same arrays are born with seven lanes (SPEC-jit 5.5 "Vector
length of an inline-allocated array"), so that case is already gone, and what remains is postponing a copy by one to
three elements. There is no large-array case: `main`'s realloc of a PreciseAllocation moves the block (allocate, copy,
free at once), which GIL off cannot do in place either, since a stale reader may still be loading from the old block.
Measured: building 100,000- and 1,000,000-element arrays costs GIL off 29 and 34 instructions per element against
`main`'s 14 and 28, and the GIL-off excess is the per-element inline code of AG-3, not growth.

What the rule would have to say, for the record. Writers: the owner, in the C++ slow path only, under the cell lock
(not merely under a StructureID claim: the flat-to-segmented converter reads the flat `vectorLength` under the cell lock
in its step 3 and claims the ID lane only in step 5, so an owner that claims, raises and un-claims between the two goes
unnoticed and the spine is published without coverage of the raised range - the defect that made the fifth revision's
T5 cell-locked; SPEC-objectmodel history 15.3 and 16.1). Order: clear `[old, new)`, store-store fence, store the new
`vectorLength`. Readers, all tiers, bound by the `vectorLength` they load from the same butterfly and then load the
lane: on x86-64 loads are not reordered with loads, so a reader that sees the raised bound sees cleared lanes; on arm64
the bound check is a control dependency, which does not order the lane load after the bound load, so a reader can pair
the raised bound with the lane's previous contents - allocator residue, read as a JSValue or a double: type confusion.
That is why the recorded form was "an x86-64-only rule". The portable form does not order anything: every butterfly is
cleared through its whole size-class capacity at birth, and the raise is then a monotone store of `vectorLength` with no
fence on either side (a stale smaller bound is conservative). It costs up to three extra lane clears per allocation and
makes the JIT's inline allocations clear to the class size. SPEC-jit 5.5's premise for keeping a butterfly across polls
("a flat butterfly's vectorLength never changes in place GIL off", history section 39) would weaken to "never shrinks":
still sound (a hoisted smaller bound sends the access to its slow path), but a foreign reader's loop then takes an
OutOfBounds exit the first time the owner raises under it.

Why not. It buys at most three lanes per butterfly, it reintroduces the converter race that T5 had, and its measured
counterpart on `main` is a one-lane raise that GIL off no longer needs. Decision (b) should be closed in favour of
D-AG-1.

#### D-AG-3. Segmented growth for owned large arrays (decision (b), second option)

**Status: proposed for rejection.** Appending fragments instead of copying would make an owner's growth O(1), and
would make the array segmented: AG-7 measured what that costs a reader today (243.7 against 19.0 instructions per
element in a hot loop; the optimizing tiers exit, the Baseline cache never settles). Even with inline segmented paths
in every tier (a dependent load through the spine plus index arithmetic: five to seven instructions more per access,
and no hoisting of a base pointer), a read-mostly array never recovers the growth it saved: geometric growth copies
each element about twice over an array's life, so the break-even is two to three reads per element. No measured row
motivates it.

What AG-7 does call for is separate from growth and is recorded for the owner of the scaling work: the indexed
inline-cache cases (`IndexedInt32Load`, `IndexedDoubleLoad`, `IndexedContiguousLoad` and the store forms) should
either carry a segmented arm, as `ArrayLength` does, or let a site that keeps meeting segmented words give up to the
generic operation instead of re-entering Optimize (the countdown `failAndIgnore` refills); and the DFG needs an array
mode for segmented words so that a hot loop over a shared array stays in optimized code.

#### D-AG-4. The owner materializes a copy-on-write butterfly claim-first (E4-C extended to CopyOnWrite sources)

**Status: proposed, optional, low priority.**

Rule. GIL off, the thread whose tag the copy-on-write word carries ((currentTID, 0); not a PreciseAllocation cell)
materializes without the cell lock and without the 128-bit compare-and-swap: transition target as today; allocate and
copy as today; then, in one poll-free and allocation-free window, (1) compare-and-swap the StructureID `S -> nuked(S)`
(lost: drop the copy, RESTART); (2) re-load the word and require it unchanged (it cannot have moved: every other
materializer needs the lane first; kept as a tripwire); (3) store the tagged word (a compare-and-swap, to keep I17's
form); (4) store-store fence; (5) store `S'`; barriers as today. This is E4-C's sequence; SPEC-objectmodel 5 excludes
CopyOnWrite sources from E4-C today only because the materializer predates it. Exclusion with a foreign materializer:
it takes the cell lock, re-verifies, and then claims the same lane with the same compare-and-swap, and its code already
treats a lost claim as RESTART ("a claim-first leg holds the lane ... kept uniform"); its word-stability release
assertions sit after its own successful claim, when the owner's step (1) can no longer succeed. The marker pairs a
CopyOnWrite structure only with an immutable butterfly and a writable one only with auxiliary storage because the ID is
nuked across the word store (its `didRace` revisit), as today. Readers that loaded the old word read the immutable
butterfly, which never changes. arm64: the fence in (4) and readers' ID-to-word ordering (M7) are the existing
arguments.

Gain. About 150 instructions and three atomic read-modify-writes per materialization: the literal microbenchmark 798
-> about 650; aes 1.5 M materializations, 0.23 G, under 1 %. Tests: `objectmodel/i03-cow-materialize-race.js` and
`cve/mc-lock-cow-materialize-race`-style tests unchanged; a counter pair (locked / claim-first materializations) in a
new `objectmodel/owner-cow-materialization-claim-first-gil-off.js`. Risk: the round-three review's assertions were
written for "no lock-free CopyOnWrite publication exists"; each must be re-read against the lane argument above.

#### D-AG-5. Small incidentals found on the way

**Status: proposed, each independent.**

- *The thread's tag word in C++.* `butterflyWriterIsForeign` / `currentButterflyTID()` cost six instructions and two
  configuration-byte tests per use (thread-local lite pointer, null test, "GIL-off process" byte, TID load), and the
  dense store paths evaluate it up to three times per call. Generated code reads one thread-local word that already
  holds `TID << 48` (R5, `g_jscButterflyTIDTag`). The C++ owner test should be the same single xor-and-mask against
  that word. About 10 instructions per `trySetIndexQuicklyConcurrent` call.
- *One bound per in-bounds access.* Where the FTL adds `CheckInBounds(index, GetVectorLength(storage))` next to the
  `publicLength` check (history section 39), express the pair as one `CheckInBounds(index, ArithMin(publicLength,
  vectorLength))`, as the out-of-bounds legs already do with `publicLengthForBounds`. When both lengths are hoisted the
  minimum is loop-invariant and the loop keeps one compare (array-element-read: 2 of about 19 instructions per
  iteration); when the length is re-read after a poll it is compare-and-conditional-move against compare-and-branch,
  a tie. Memory safety is unchanged: the bound used is never above the hoisted storage's own `vectorLength`.
- *Flag off.* The deferred-fire scope constructs and destroys a temporary `WatchpointSet` per array conversion; its
  destructor's flag test forces a frame on a function that was a leaf on `main`. Testing `m_everLinked` inline in the
  header and calling the locked drain out of line restores the leaf (about 7 instructions per conversion); the JIT
  operation prologue/epilogue tests are the flag-off list's general item.

### D: arm64 / non-Linux notes

- T1 / T1-S publication: `storeStoreFence()` then a seq_cst compare-and-swap of the tagged word; readers load the word
  and reach header and lanes by address dependency (SPEC-objectmodel M1, SPEC-jit F2). Sound on arm64 as written. T1-S
  improves the reader side for appends that grow: element and length are covered by the word's publication.
- `Butterfly::bumpPublicLengthToAtLeast` (the CAS-max): release on the writer; readers load `publicLength` relaxed and
  then the lane with no dependency between the two. x86-64 orders the two loads; on arm64 a reader may pair a fresh
  length with a stale empty lane and take the generic path (a spurious hole, never a wrong value). Recorded in the
  function's comment as a known residual; still open; needs a load-acquire of `publicLength` (or of the word) on the
  C++ reader side and the equivalent in LLInt/Baseline for the arm64 port if holes must not be spurious.
- In-place vectorLength raise (D-AG-2): the clear-then-raise order relies on x86-64 load ordering at the reader (a
  control dependency does not order arm64 loads). Portable only in the clear-at-birth form. Not proposed.
- `trySetIndexQuicklyConcurrent`'s "mode before word" rule uses `loadLoadFence()` (a `dmb ishld` on arm64, a compiler
  barrier on x86-64) and pairs with the materializer's word-then-header order; arm64-sound as written.
- The copy-on-write materializer's PreciseAllocation leg and D-AG-4 publish word, fence, then StructureID; readers
  need ID-before-word ordering (M7: dependency, acquire or re-check). Existing argument, no new reliance.
- `butterflyConcurrentCopyWordsSlow` is `gcSafeMemcpy`: 16-byte `movups` on x86-64, `ldp`/`stp` of Q registers on
  arm64. The requirement is 64-bit single-copy atomicity per lane (the source may be receiving a racing 8-byte store).
  arm64 gives it architecturally (a 128-bit SIMD access that is 64-bit aligned is treated as a pair of 64-bit
  single-copy-atomic accesses); x86-64 gives it in practice for 8-byte-aligned halves, not by the manual. Both are
  what upstream already relies on for the concurrent marker, so nothing new is assumed.
- Non-Linux: nothing in this area depends on the platform beyond thread-local storage for the tag word (R5).

### D: Decisions for the user

1. **Close decision (b).** Options: (A) in-place `vectorLength` raise by the owner, x86-64-only or, portably, with
   clear-at-birth: buys at most three lanes per butterfly, reopens the converter race T5 had; (B) segmented growth for
   owned arrays: O(1) growth, 13x slower reads today and slower reads always; (C) keep T1's copy and remove the
   re-dispatch around it (D-AG-1): portable, no protocol change, -8 % instructions on aes, -2.5 to -3 % on the two
   other sjcl tests. Recommendation: C, and record in the Open items that aes' remaining distance is the Double family
   (AG-2), not growth.
2. **Whether D-AG-4 (claim-first copy-on-write materialization) is worth its review.** Under 1 % on aes, 18 % on the
   literal microbenchmark; touches assertions the third review round wrote. Recommendation: defer until the Double work
   has landed and aes is re-profiled.
3. **Whether segmented arrays in the optimizing tiers (AG-7) are in scope for the landing.** Nothing in JetStream or
   the micro set sees it; every program that appends to one array from two threads does (13x per element).
   Recommendation: at least the inline-cache give-up (small, local), with the DFG array mode as a scaling-round item.

### D: Doc mismatches

- SPEC-objectmodel 4.4 **T5** ("In-place vectorLength-only growth ... CELL-LOCKED, owner-only ...") and the
  parenthesis in 4.2 step 3 ("T5 grows VL in place: pointer/tag unchanged") describe a leg the tree does not have.
  `JSObject::ensureLengthSlow`'s comment and `ensureLengthSlowConcurrent`'s header say T5 was removed in the first
  review round ("flat vectorLengths are immutable flag-on"); the only in-place forms left are the two in
  `ensureLengthSlowConcurrent`'s `!vm.gilOff()` branch (an unlocked raise into size-class slack and the
  PreciseAllocation realloc, M8 r17), reachable only in a tagged GIL-on process (`--useJSThreadsSingleOwnerWithGIL=0`).
  Fix: mark T5 withdrawn, point to M8 for the GIL-on forms, and state "a published flat vectorLength is immutable GIL
  off" as the invariant SPEC-jit history section 39 relies on.
- LANDING-PLAN Open items, "GIL off below GIL on" item (3), and PERF-RESULTS 6.12 "Array growth that copies": "half of
  it in the concurrent growth, store and materialization helpers (2,250 of 4,422 extra samples)" is gross; net of what
  GIL on spends in `main`'s counterparts it is about a quarter (AG-1, AG-2), and two thirds of the gap is generated
  code. "In-place growth of an owned flat butterfly GIL off is decision (b)" presumes `main` grows in place; `main`
  copies every block-allocated butterfly too.
- SPEC-jit 5.5 Read predicate: "top16 == 0xFFFF -> segmented read (dependent load through the spine; R3 op or
  inline)". In the tree the inline-cache read choke point sends a segmented word to the stub's slow case and only the
  ArrayLength case has a segmented arm; for indexed loads the "R3 op" is the Optimize operation on every access
  (AG-7). The text should say what is implemented and point to the open item.
- PERF-RESULTS 6.12, array microbenchmarks: "append by index" and "push" read as growth rows; in steady state they
  contain no growth event (the allocation profile's vector-length hint covers sixteen elements). They measure the
  inline per-element path (AG-3).

### D: What was run

Nothing was built. No suite, campaign, Bun or sanitizer lane.

- `perf record -e instructions:u` on stanford-crypto-aes, GIL on and GIL off, twice each (once plain, once with
  `--dumpFTLDisassembly` for the per-node binning); `perf stat` totals for the same test in three configurations.
- One pass over the 36 JetStream tests GIL off with `--reportJSThreadsCounters=1` under `perf stat` (eight at a time).
- Hardware-breakpoint call counts (`perf stat -e mem:<addr>:x`) on aes, GIL on and GIL off, four functions each.
- About 250 short microbenchmark runs under `perf stat` (the staircases and per-call differences above) and twelve
  4 M-period `perf record` runs of microbenchmarks.
- `--printEachOSRExit`, `--reportCompileTimes`, `--dumpDFGDisassembly`, `--dumpFTLDisassembly` on microbenchmarks; one
  Debug-build run of a microbenchmark for the machine code of the append loop; `objdump` of six functions in the `main`
  and branch binaries.

## Section E. Flag off against `main`: what is left, instruction by instruction

Scope: everything that separates the branch with no option set from `main` (`cf1b36ec8703`), except the RegExp and
string-split entry points (section F). Release builds, Linux x86-64. Numbers are instruction counts (`perf stat -e
instructions:u`), instruction samples (`perf record -e instructions:u -c 2000000`, summed per symbol) or exact
dynamic instruction traces of one call (a debugger single-stepping one invocation of a function in each binary, other
threads held, the two traces diffed). Wall-clock time is not used.

Method note that matters for every per-symbol number below: instruction-count sampling attributes a sample to the
instruction that is retiring when the counter overflows, which favours instructions behind a stall. The TOTAL of a run
is exact; the split between symbols is not (in one experiment a function whose code is byte-identical in the two
binaries and which ran the same number of times showed +12 % samples). Per-symbol sample deltas rank candidates;
the per-call instruction trace is what establishes a mechanism. Sampling periods below about 400,000 instructions are
throttled by the kernel's sample-rate limit on this machine and undercount; every recording here uses 1,000,000 or
2,000,000.

### E: Inventory

Suite-wide sums (36 JetStream tests, `main` 220,329 samples, flag off 224,335, +4,006 = +1.82 %), recomputed from the
tenth round's stored sweep and grouped by what the symbol belongs to:

| class | main | flag off | delta | share of the suite |
|---|---|---|---|---|
| arrays / objects / structures runtime (C++) | 11,055 | 12,056 | +1,001 | +0.45 % |
| strings (ropes, substrings, atom strings, joins) | 6,514 | 7,312 | +798 | +0.36 % |
| RegExp / Yarr entry points (section F) | 8,419 | 9,143 | +724 | +0.33 % |
| collector: marking | 7,664 | 8,350 | +686 | +0.31 % |
| generated code | 140,987 | 141,442 | +455 | +0.21 % |
| libc / pthread / unclassified | 2,017 | 2,341 | +324 | +0.15 % |
| parser / bytecode generator | 13,500 | 13,670 | +170 | +0.08 % |
| collector: sweep, allocation slow paths, stack sanitizing | 2,795 | 2,891 | +96 | +0.04 % |
| interpreter glue, calls | 649 | 702 | +53 | +0.02 % |
| DFG / B3 / FTL compiler (compiler threads) | 21,052 | 20,934 | -118 | -0.05 % (includes +336 of a helper fixed since) |
| malloc | 2,897 | 2,779 | -118 | -0.05 % |

The sweep predates two fixes of the round (the DFG clobberize helper, +336, gone in the final tree; the RegExp matching
context, see Doc mismatches). Six tests were recorded again on the final tree: OfflineAssembler 1.096 (sweep 1.127),
UniPoker 1.036, Air 1.026, pdfjs 1.019, splay 1.009 (sweep 1.063), Babylon 0.996 (sweep 1.055: the clobberize fix).
Single runs; splay's and pdfjs's totals move by several percent with the number of collections a run happens to make,
which is why the collector is measured per collection below. Per thread on the final tree: OfflineAssembler main thread
8,005 -> 8,725 samples (+9.0 %), compiler threads +3 %, marker threads +1 %; splay main thread 2,139 -> 2,193 (+2.5 %),
marker threads 4,904 -> 4,936; pdfjs main thread +3.1 %; UniPoker +3.8 %; Air +2.9 %; Babylon +4.0 % on the main thread
with compiler threads -4.7 %.

What the classes have in common is one shape, the same one section F found for RegExp: a hot leaf function gains
between one and twenty Config-page byte tests (each `lea g_config; cmp byte; jcc`, three instructions, or four when the
byte is an `Options` bool compared with 1), each test's cold arm is inlined behind it, the function grows 1.2x-4x in
bytes, the register allocator spills around the cold arms, and callers that used to inline the leaf now call it. No
single site is more than 0.2 % of the suite. Static sizes of the functions named in the sweep (bytes, `main` -> flag
off): `operationEnsureDouble` 76 -> 320, `WatchpointSet::~WatchpointSet` 44 -> 163, `operationArrayPushDouble` 1,297 ->
3,100, `JSArray::fastSlice` 2,103 -> 5,329, `LocalAllocator::allocateSlowCase` 597 -> 1,475,
`JSObject::convertFromCopyOnWrite` 676 -> 1,186, `JSValue::getPropertySlot<false>` 4,694 -> 7,128,
`operationGetByValGaveUp` 5,391 -> 8,293, `JSRopeString::resolveRope` 1,336 -> 1,966, `sanitizeStackForVM` 470 -> 754,
`sanitizeStackForVMImpl` 49 -> 101, `JSObject::convertInt32ToDouble` 648 -> 833, `JSObject::trySetIndexQuickly` 589 ->
767, `jsSubstringOfResolved` 736 -> 845, `StringImpl::~StringImpl` 253 -> 286.

#### E-1. Collector marking: +6.4 % instructions per collection - explained

Configurations: all three (the code is shared; flag off pays it for nothing).

Measurement. A fixed live graph (400,000 records, each a four-property object, a string, a three-element array and
a two-property object: 1.6 M cells and 0.4 M butterflies) and K synchronous full collections; instructions(K=20) -
instructions(K=0), divided by 20. `main` 385.7 M per collection, flag off 410.3 M, +24.6 M (+6.4 %); with one marker
thread (`--numberOfGCMarkers=1`) 385.6 M and 410.4 M, so the number is not a parallel-marking artefact. Both
binaries visit the same bytes (65.75 MB, from `--logGC`) in the same number of collections. A 12,000-instruction trace
of the drain loop in each binary: `main` visits 55 cells (218 instructions per cell), flag off 52 (231): +13 per cell.
PERF-RESULTS 6.12's "+5 % per collection" is this; its attribution (visit counters and the helper-pause checkpoint)
is half right.

Where the 13 instructions are (per-call traces, one invocation of each function diffed):

| function | `main` | flag off | extra | cause (source) |
|---|---|---|---|---|
| `SlotVisitor::setMarkedAndAppendToMarkStack<MarkedBlock>` (once per marked cell) | 37 | 41 | +4 | `m_bytesVisited += size` was one `add [mem], reg`; as `atomicStore(atomicLoad() + size)` it is load, add, store (+2; the compiler folds the `m_visitCount` increment back into `inc [mem]` but not this one, because two other loads sit between the atomic load and the store). `GCSegmentedArray::append` reads `m_top` for the capacity test and again in `postIncTop()`; two relaxed atomic loads are not merged (+1) and the block layout gains a `jmp` (+1). `heap/SlotVisitor.cpp` `appendToMarkStack`, `heap/GCSegmentedArrayInlines.h`. |
| drain lambda in `SlotVisitor::drain` (once per popped cell) | 30.9 | 31.9 | +1 | `canRemoveLast()` and `removeLast()` each load `m_top` (relaxed atomic, not merged). |
| `JSObjectWithButterfly::visitChildren` (the inlined body of `visitButterflyImpl`; once per object with a butterfly, and the same body inside `JSFinalObject::visitChildren`) | 85.4 | 95.0 | +9.6 | `const bool jsThreads = Options::useTaggedButterflies()` is captured BY REFERENCE by the `visitElements` closure, so it is stored to the stack and the closure grows from two captured pointers to four (frame 0x38 -> 0x58 bytes); plus `test al, al; jne` before the butterfly load. `runtime/JSObject.cpp` `visitButterflyImpl`. |
| the `visitElements` closure (once per visited object) | 38.9 | 47.6 | +8.7 | the GIL-off-only "value-visit Int32 shapes" case split `ALL_WRITABLE_INT32_INDEXING_TYPES` out of the contiguous group; `main`'s switch is one range test (`lea -10; cmp 4`), the branch's is a cascade of three `bt` tests against bit masks plus a load of the captured flag through the closure (`cmpb $1, (%rdx)`). |
| `SlotVisitor::markAuxiliary`, `appendHiddenSlow`, `JSString::visitChildren` | 40 / 14 / 91 | same | 0 | byte-identical |
| helper-pause checkpoint in `drain()` | - | - | ~0 | one byte test of the visitor's own field per batch of `minimumNumberOfScansBetweenRebalance` cells; not a contributor |

Same kind as section F's findings (a gate that reshapes a leaf) for the last two rows; the first two rows are a
different kind: accesses made atomic for the sanitizer's sake that the comment in `GCSegmentedArray.h` claims
("Codegen is identical to the plain accesses") compile to the same code, and do not.

Worth: marking symbols are 3.5 % of the suite's instructions, mostly on marker threads; +686 samples (+0.31 % of the
suite); splay +4.4 % of its instructions in the sweep, pdfjs +2 %, hash-map +2.5 %. JetStream scores see the part that
runs while the mutator waits (splay's Worst-case component, pdfjs).

#### E-2. `JSArray::tryCreate` and array creation: +15 instructions per call (109 -> 124) - explained

Trace of one steady-state call (`new Array(n)` from Baseline code). The function's source is unchanged; all fifteen
come from its inlined callees: `Butterfly::optimalContiguousVectorLength` tests `Options::useSharedGCHeap()`
(`lea; movzbl; cmp $1; je` and two register moves, +6); `allocateCell<JSArray>` tests a Config byte before choosing
the VM's allocator or the thread's (+4: test, branch, a `lea` of the allocator's address that used to be a folded
displacement, a move); the `JSArray` constructor tests `useTaggedButterflies` before storing the butterfly word (+2);
two spills to a frame that grew from 0x28 to 0x38 bytes and one layout `jmp` (+3). Static size 255 -> 424
instructions. Same recipe as F-D4. Worth: +174 samples (WSL 62, Basic 62, ML 48); the same three callees are inlined
into every other array and object creation path (`operationNewArrayWithSize` +26, `tryAllocateCell<JSCellButterfly>`
+17, `constructArray`, `JSArray::fastSlice` +29, `tryConcatAppendArrayFastWithWatchpoints` +28), which together are
about +350.

#### E-3. `array-int32-to-double-relabel` 1.18 - partly explained

`[1,2,3,4]; a[1] = 2.5; a.push(i)` per iteration. Instructions per iteration over the first 800,000 iterations:
`main` 582, flag off 660 (+78, +13 %); after the loop has reached the FTL (6 M iterations) the difference is about
+20 per iteration, so the 200,000-iteration row measures mostly Baseline and DFG code and the C++ under it. By
symbol (sampled, 6 M iterations): `operationArrayPushDouble` +61 samples of 165 (its body is `JSArray::pushInline`,
which gained the tagged-word dispatch block and a `gilOffProcess` test in front of every `setPublicLength`; 1,297 ->
3,100 bytes), `operationEnsureDouble` +49 of 27 (76 -> 320 bytes), generated code +46 of 177,
`JSObject::convertFromCopyOnWrite` +45 of 952 (676 -> 1,186 bytes), `WatchpointSet`'s constructor and destructor +53
(the two-argument constructor with the Class A/B classification is out of line; the destructor 44 -> 163 bytes), and
`Structure::nonPropertyTransition` -51 (work moved, not removed). Per-call traces of these operations were not
obtained (the single-step tracer deadlocks when the traced thread is suspended by the signal-based trap sender); the
static sizes and the sampled deltas are what is established. The Double and array-growth sections have the same
functions from the GIL-off side and found the same static growth.

#### E-4. `sanitizeStackForVMImpl`: +4 instructions per call (16 -> 20), and its C++ wrapper - explained

The assembly routine gained a Config-byte test (`lea g_config; cmpb $0, gilOffProcess; je`) in front of the address
computation of the last-stack-top slot, and the flag-off arm reaches the common body through an extra `jmp`.
`sanitizeStackForVM` (the C++ wrapper that decides whether to call it) 470 -> 754 bytes. Called at every VM entry and
from the allocation slow path. Worth: +85 samples (+37 % of the symbol).

#### E-5. `StringImpl::~StringImpl` and `deref`: +3 instructions on the atom-string path - explained (static)

One test of `WTF::g_sharedAtomStringTableEnabled` (`lea; cmp byte; je`) before the atom-table removal, and a second
exit that ends in `lock sub; jne; jmp derefSharedZero`. The inline `deref()` fast path is unchanged (`sub 2; je
destroy`). The shared-table latch is a WTF global, not a Config-page byte, so it costs a relocation-addressed load
the same as the others. Worth: not visible as its own row in the sweep (`StringImpl::~StringImpl` -7 .. +7 per test).

#### E-6. `JSRopeString::resolveRope` (+160), `jsSubstringOfResolved` (+143), `jsSubstring` (+65, out of line now), `StringImpl(unsigned, Force8Bit)` (+29, out of line now), `JSValue::getPropertySlot<false>` (+114), `operationGetByValGaveUp` (+80) - partly explained (static only)

Sizes grew 1.15x-1.55x (list above); `jsSubstring` and the 8-bit `StringImpl` constructor exist as out-of-line
symbols in the branch binary and have zero samples on `main`, i.e. their callers stopped inlining them. `resolveRope`
and the substring helpers are reached mostly from the RegExp and split paths of section F (OfflineAssembler 57 + 67 +
58, regexp 48, UniPoker 38 + 7) and the recipe there (F-D4's mode-instantiated entry points) covers them.
`getPropertySlot<false>` is the generic get behind `operationGetByIdGeneric`, `operationGetByValGaveUp` and the
interpreter's slow paths; it inlines `JSObject::getOwnNonIndexPropertySlot` -> `Structure::get` -> the property-table
walk, which has `useTaggedButterflies` legs for the out-of-line offset decode. Per-call traces not taken.

#### E-7. Generated code: +455 samples of 140,987 (+0.3 %) - not explained

By SPEC-jit I1 flag-off machine code differs from `main`'s only in field-offset immediates. +0.3 % of generated-code
samples over 36 single runs is inside the run-to-run variation of tier-up timing (per test: stanford-crypto-aes +115,
OfflineAssembler +108, pdfjs +97, hash-map -16). Not examined with disassembly dumps in this pass. What is
established statically for the one tier that is compiled into the binary: the LLInt's affected opcodes carry 1-4
Config-byte tests each (`ifTaggedButterfliesBranch` and the call-record gate) - `op_get_by_id` 119 -> 198 static
instructions with 1 test, `op_put_by_id` 158 -> 242 (3 tests, 2 on `main`), `op_get_by_val` 226 -> 245 (3, 2),
`op_put_by_val` 295 -> 357 (4, 0), `op_call` 48 -> 51 (1), `op_get_from_scope` 179 -> 251 (2), `op_put_to_scope` 559
-> 715 (2), `op_instanceof` 116 -> 200 (2), `op_iterator_next` 62 -> 65 (1), `op_new_object` / `op_new_array`
unchanged. Dynamically that is two to three instructions per executed property or call opcode in the interpreter;
LLInt symbols are below the sweep's reporting threshold individually.

#### E-8. Parser and bytecode generator: +170 samples (+1.3 % of the class) - not examined

`parseAssignmentExpression` +73, `parseMemberExpression` +37, `Scope::~Scope` +29, `parseSourceElements` +28,
`parseFunctionInfo` +24, all in the code-load tests. The parser has no mode gates; candidates are identifier
atomization through `AtomStringImpl::add` (the shared-table latch; `AtomStringImpl::add` itself is -84, so work
moved between symbols) and code layout. Not examined.

#### E-9. Mode gates that are function calls in a build that does not inline (Bun's Debug+ASAN build) - explained

Source counts in `Source/JavaScriptCore`: `Options::useJSThreads()` 461 call sites, `Options::useTaggedButterflies()`
386, `VM::gilOffWithProcessGate()` 131, `g_jscConfig.gilOffProcess` 115 direct reads, `Options::useSharedGCHeap()` 71.
In a Release build each is a byte load from the frozen Config page. In a build with inlining off each `Options::x()`
is a call to the accessor, which calls `addressOfJSCConfig()`, and `gilOffWithProcessGate()` is a call that makes a
second one - the chain LANDING-PLAN's `fetch-tcp-stress` item measured (`addressOfJSCConfig` 3.81 % of samples against
2.48 %, `useSharedGCHeap` 1.12 %, `gilOffWithProcessGate` 0.78 %). The flag-off-reachable hot readers of
`Options::useSharedGCHeap()`: `MarkedVector.h` four (`MarkedVector` construction, expansion and destruction: every
host call's argument buffer that spills), `MarkedVector.cpp` two, `CompleteSubspace` nine (`allocate`, the inline
allocator lookup, `allocateSlow`, `reallocatePreciseAllocationNonVirtual`), `Heap.h` eleven,
`Butterfly::optimalContiguousVectorLength` one. `VM.h` has some forty `gilOffWithProcessGate()` accessors (the
per-thread redirections of scratch buffers, string-searcher tables, RegExp state, the top call frame).

### E: Designs

#### E-D1. Marker counters and the mark stack's `m_top`: one load per operation, atomic only for the sanitizer

**Status: proposed.**

Rule. (a) `GCSegmentedArray<T>::append`, `removeLast` and their helpers read `m_top` once per operation into a local
and store once: `size_t top = loadTop(); if (top == s_segmentCapacity) { expand(); top = 0; } m_segments.head()->data()[top] = value; storeTop(top + 1);`
and `removeLast`: `size_t top = loadTop() - 1; storeTop(top); return data()[top];` with `canRemoveLast()` folded into a
`tryRemoveLast(T&)` that the drain loop uses, so the emptiness test and the pop share the load. (b) `loadTop` /
`storeTop`, `m_visitCount`, `m_bytesVisited`, `m_nonCellVisitCount` and `m_mutatorIsStopped` are plain accesses unless
`TSAN_ENABLED`, where they stay relaxed atomics - the rule `JSCJSValue.h` already follows for value slots. Every one
of these has a single writer (the visitor's own thread; the shared stacks' writers are serialized by the heap's locks,
as the comment in `MarkStack.h` says); the cross-thread readers (`isEmpty()` / `size()` in donation heuristics,
`visitCount()` / `bytesVisited()` sums after the markers have joined) are the ones `main` has with the same plain
accesses.

Tiers: C++ only; no generated code reads these fields.

Memory ordering. x86-64 and arm64 alike: a relaxed atomic word load or store and a plain aligned word load or store
are the same instruction (`mov` / `ldr` / `str`); what changes is only what the compiler may merge, which is the
point. No ordering is lost because none was provided: relaxed atomics order nothing, and the hand-off of mark-stack
contents between threads is ordered by `m_markingMutex` and the `rightToRun` lock on both architectures. The
single-load form in (a) is valid under either typing, so (a) alone is a smaller step that keeps the atomics in every
build (it removes the two reloads, not the `m_bytesVisited` split).

Collector, stops, watchpoints: no interaction; the fields are private to marking.

What a second thread can observe mid-way: a stale `m_top` or counter, as today and as on `main`; consumers are
heuristics and post-join sums.

Flag off: `main`'s code again in these functions (checked by diffing the disassembly of
`setMarkedAndAppendToMarkStack` and the drain closure against `main`'s: 76 and 127 instructions).

Failure modes. A reader that needed the atomic for correctness: there is none by construction (relaxed), but the
sanitizer lane is the detector and keeps the atomic form. A compiler that tears a plain 8-byte store: not on any
supported target for an aligned `size_t`.

Tests: no behaviour to test; the gate is the per-collection instruction count of the fixed-graph benchmark in this
section (a `JSTests/threads/gc-stress` file that builds the graph, runs 20 `fullGC()` calls and prints nothing; counted
with `perf stat` by the bench gate: 410 M -> expected 397 M per collection from D1, `main` 386 M) and the TSan lanes
unchanged at 0 reports.

Verification: corpus 4 modes Release + Debug, TSanJIT both modes (the fields are in the sanitizer's view there),
gc-stress matrix. No JIT or object-model change, so no mirror pass needed.

Expected gain: 5 of the 13 extra instructions per cell: about 38 % of E-1 = +260 of the +686 samples (0.12 % of the
suite); splay about -1.7 % instructions.

Risks: low. Alternatives rejected: leaving the atomics and reordering the expression so the compiler folds the
read-modify-write (works today for `m_visitCount`, depends on instruction selection details, not a rule).

#### E-D2. `visitButterflyImpl` instantiated per butterfly-word mode

**Status: proposed.**

Rule. `JSObjectWithButterfly::visitButterflyImpl<Visitor>` becomes `visitButterflyImpl<Visitor, ButterflyWords mode>`
with `mode` in {Untagged, Tagged}; the public entry tests `Options::useTaggedButterflies()` once and tail-calls the
instance (`[[unlikely]]` on Tagged). In the Untagged instance every `jsThreads` conjunct is `constexpr false`: no
captured flag, the closure captures what `main`'s captures, the switch is `main`'s (`ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES`
first, Int32 shapes not visited), no segmented dispatch. The GIL-off "value-visit Int32 lanes" arm and the SW=1
vector-length bound live in the Tagged instance only, where the Int32 test is made before the switch
(`if (gilOff && hasInt32(indexingMode)) goto contiguous;`) so that its switch is `main`'s too.

Soundness: the mode is process-wide and fixed before the first object exists (the derived option is computed at
options finalization; SPEC-objectmodel G1), so an object is never visited by the instance of the other mode.

Tiers: C++ (the collector). Memory ordering: unchanged in each instance - the Tagged instance keeps the
structure-ID / butterfly / structure-ID bracket with its dependency chain (arm64: the `Dependency` consume stays; it
is what orders the tagged-word load after the early structure load); the Untagged instance is `main`'s bracket.

Collector interplay: this IS the collector's object visit; nothing else changes. Concurrent marking GIL on runs the
Untagged instance against a mutator that publishes untagged words with `main`'s fences (E4-G), which is `main`'s
protocol.

Flag off: `main`'s code plus one byte test and a direct jump per visited object (2 instructions instead of 18).

Failure modes: an instance used in the wrong mode would mis-decode a tagged word; detected by a debug assertion at
the top of each instance (`ASSERT(Options::useTaggedButterflies() == (mode == Tagged))`) and by the corpus in all
modes. Code size: one more copy of a 100-instruction function per visitor type (two: `SlotVisitor`,
`AbstractSlotVisitor`), times the two callers that inline it.

Tests: `gc-stress` lanes as they are; the same instruction gate as E-D1 (expected 397 M -> 388 M per collection).

Verification: corpus 4 modes, TSanJIT GIL off (concurrent marking against tagged words), the gc-stress matrix, the
mirror Release eval pass GIL off (it exercises relabels racing the marker).

Expected gain: 16 of the 18 per-object instructions: about +400 of the +686 samples together with D1 leaving about
+30. Combined D1 + D2: marking within 0.5 % of `main` per collection.

Alternatives rejected: capturing the flag by value (removes the stack traffic, keeps the reshaped switch: about half
the gain); a virtual visitor per mode (an indirect call per object).

#### E-D3. Mode-instantiated creation and growth paths for arrays and objects (the recipe of F-D4 applied to `runtime/`)

**Status: proposed.**

The entries: `JSArray::tryCreate` / `create` / `tryCreateUninitializedRestricted`, `allocateCell<T>` for the classes on
creation fast paths (`JSArray`, `JSFinalObject`, `JSCellButterfly`, `JSRopeString`, `JSString`),
`Butterfly::optimalContiguousVectorLength`, `JSArray::pushInline` (and therefore the five `operationArrayPush*`),
`operationEnsureInt32` / `EnsureDouble` / `EnsureContiguous`, `JSObject::convertFromCopyOnWrite`,
`convertInt32ToDouble`, `JSArray::fastSlice`, `WatchpointSet`'s constructor and destructor.

Rule. One process-mode constant, `ThreadsMode { Off, Untagged, Tagged }` (flag off and single-owner GIL on are both
"words untagged, one allocator set, no CAS-max lengths"; GIL off is Tagged), selected ONCE at the entry of each
listed function from one Config byte that encodes it (a new `g_jscConfig.threadsMode`, written where
`useTaggedButterflies` is derived) and passed as a template argument through the inlined callees, so that inside the
`Off`/`Untagged` instance `Options::useSharedGCHeap()`, `useTaggedButterflies()`, `gilOffProcess` and
`gilOffWithProcessGate()` are compile-time false. Entry points exported to generated code (`operation*`) keep one
symbol and dispatch inside; C++ callers call the dispatcher. The Tagged instance is today's code.

Why this is sound: every gate inside these functions is a function of the three process-wide, finalize-time bytes;
none is per-VM or per-object. `VM::gilOffWithProcessGate()` is the exception (it reads `m_gilOff` behind the process
byte): with `threadsMode != Tagged` the process byte is 0 and the accessor's answer is false for every VM, which is
the invariant its own comment states.

Memory ordering, collector, stops, watchpoints: nothing moves; each instance is existing code with dead arms removed.

Flag off: `main`'s instruction sequences plus one byte test per entry (3 instructions) instead of 6-20 per call.

Failure modes: a gate that is NOT a function of the process mode folded to a constant (a per-VM or per-object
predicate): found by review of each folded predicate (the list is mechanical: grep the five accessors inside the
instantiated bodies) and by the corpus. Text size: the branch's text is already 35.26 MB against 30.50 MB (section F);
instantiating about twenty leaf functions twice adds on the order of 40 KB and removes the inlined cold arms from the
hot instances, which is what gives callers their inlining back.

Tests: a micro set row per entry in the bench gate with `main`'s instruction count as the bound (`new Array(n)`:
109 per call on `main`, 124 now; `[1,2,3,4]; a[1] = 2.5; a.push(i)`: 582 per iteration on `main`, 660 now).

Verification: every listed function is shared with the flag-on modes, so each batch needs the three-configuration
before/after (instruction counts, not times) plus corpus 4 modes and TSanJIT; the array functions also the GIL-off
stress suite and a mirror pass (they are object-model code).

Expected gain: E-2's +350 and E-3's row (1.18 -> about 1.03) and a share of the +1,001 of the arrays/objects class;
estimate +500 samples (0.23 % of the suite).

Risks: medium - it is broad, mechanical, and touches object-model entry points. Alternatives rejected: (1) start-up
code patching of gates into no-ops (static keys): zero cost per gate, but needs writable text at start-up, which
signed macOS binaries do not allow, and is a second code-patching mechanism to reason about; (2) merging several
bytes into one and hoping the compiler merges the loads: it does not across the opaque calls between them.

#### E-D4. `sanitizeStackForVMImpl`: choose the slot in C++

**Status: proposed.** `sanitizeStackForVM` already decides whether to call the routine; it passes the address of the
last-stack-top slot (the VM's flag off and GIL on, the running thread's lite's GIL off) as the argument, and the
assembly routine loses its gate and its thread-local load and is `main`'s sixteen instructions with a pointer
argument instead of a VM-relative displacement. Flag off: -4 instructions per call. No ordering, collector or
watchpoint content. Test: existing `vmstate` stack tests; bench gate row `throw-catch` / `closure-calls`. Verification:
corpus 4 modes (the routine runs at every VM entry in every mode). Gain: +85 samples.

#### E-D5. Config-page bytes for the flag-off-reachable `Options` gates, and out-of-line flag-on arms in `VM.h` accessors

**Status: proposed** (LANDING-PLAN already names the first half as the next step).

(a) `Options::useSharedGCHeap()` in `MarkedVector`, `CompleteSubspace`, `Butterfly::optimalContiguousVectorLength`,
`Heap.h`'s inline gates: read `g_jscConfig.useSharedGCHeap` (a byte next to `gilOffProcess`, latched at the same
place) through a `static constexpr`-offset inline that a non-inlining build still compiles to a load (a macro or an
`ALWAYS_INLINE` function whose body is one member access of a global; whether Bun's debug build honours
`always_inline` was not checked - a macro does not depend on it).
(b) The forty `VM.h` redirection accessors keep `if (g_jscConfig.gilOffProcess) [[unlikely]] return gilOffX();` with
the byte read spelled directly rather than through `gilOffWithProcessGate()` -> `addressOfJSCConfig()`.
Release builds: no change in instruction count (already a byte load), but (a) turns the four-instruction
`movzbl; cmp $1` form into the three-instruction `cmpb $0` form. Debug+ASAN Bun: removes the call chains measured at
about 3 % of `fetch-tcp-stress` instructions of the 16.9 % gap; the rest of that gap is the same gates as inlined
loads and is covered by D3. Verification: Bun's flag-off lane for the four `fetch-tcp-stress` cases with their
instruction counts.

#### E-D6. Strings: `resolveRope`, substring helpers, atom-string paths

**Status: proposed as part of F-D4** (they are reached from the RegExp and split entries; instantiating those
entries per mode removes the gates these helpers inherit and gives `jsSubstring` and the 8-bit `StringImpl`
constructor their inlining back). Not designed separately; per-call traces still to be taken when F-D4 is
implemented, with `main`'s counts as the bound.

### Ranking, and what is left if all of it is done

| # | item | samples recovered (of +4,006) | suite instructions | configurations to re-verify |
|---|---|---|---|---|
| 1 | F-D4 / F-D5 (RegExp entry points; section F), with E-D6 | ~700 + ~350 | 0.48 % | all three (shared code) |
| 2 | E-D1 + E-D2 (marker) | ~650 | 0.30 % | D1: flag-off-neutral for the other modes (same plain accesses as `main`); D2: GIL off needs TSan + mirror |
| 3 | E-D3 (array / object creation and growth) | ~500 | 0.23 % | all three; object-model verification |
| 4 | E-D4 (stack sanitizing) | ~85 | 0.04 % | all three, trivially |
| 5 | E-D5 (Config bytes) | ~0 in Release; Bun Debug lane | - | flag off only |

Done together: about +2,300 of the +4,006 samples, leaving roughly +0.8 % instructions (generated code +0.2 % that may
be noise, parser +0.08 %, LLInt gates, and a tail of symbols below 40 samples each). On the measured correspondence
of this suite (instructions +1.8 % with scores at 0.97-0.98) that puts flag off at about 0.99 of `main`: reachable, with
no margin. Items 1-3 are the whole of it; nothing else found is worth more than 0.05 %.

### E: arm64 / non-Linux notes

- E-D1: plain versus relaxed-atomic word accesses compile to the same `ldr`/`str` on arm64; the mark stack's
  cross-thread hand-off is ordered by locks, not by these accesses. No change needed.
- E-D2: the Tagged instance must keep the `Dependency`-ordered butterfly load after the early structure-ID load
  (SPEC-jit F7 / OM M7); the Untagged instance has `main`'s ordering, which on arm64 relies on the same dependency
  (`main` has it too). Nothing new.
- E-D3..D5: gates only; no ordering content. On macOS the Config page is frozen the same way; the new byte must be
  written before `Config::finalize`.
- The rejected static-key alternative is where platforms differ (signed text pages on macOS / iOS).

### E: Decisions for the user

1. Whether to make the marker's counters plain outside sanitizer builds (E-D1 b) or only restructure to single loads
   (E-D1 a). (b) restores `main` exactly and is what `main` ships; (a) keeps well-defined C++ in every build and leaves
   2 of the 5 instructions. Recommendation: (b), with the TSan lane as the guard.
2. Whether flag off at 0.99 is worth the breadth of E-D3 + F-D4 (some twenty runtime functions and the RegExp entries
   instantiated per mode, all shared with the flag-on modes). Recommendation: yes for F-D4 and the marker (they are
   two thirds of the gain and well bounded), E-D3 entry by entry with the bench-gate row for each.

### E: Doc mismatches

- `docs/threads/LANDING-PLAN.md`, tenth round, Flag off / "Removed cost", and PERF-RESULTS 6.12: "the RegExp
  matching context's constructor inlines into the match operations again". In the final binary
  `Yarr::MatchingContextHolder::MatchingContextHolder` is an out-of-line symbol of 213 bytes (114 on `main`) and takes
  109 samples in OfflineAssembler (0 on `main`); the fix shortened it (234 -> 213 bytes between the mid-round and final
  builds) but did not restore the inlining. Fix: say "shortened, still out of line", and move it to the open list.
- LANDING-PLAN Open items and PERF-RESULTS 6.12: "the marking loop (+5 % instructions per collection: relaxed-atomic
  visit counters and the helper-pause checkpoint)". Measured +6.4 %; the helper-pause checkpoint contributes nothing
  measurable (one byte test per batch); the counters and `m_top` are 5 of the 13 instructions per cell, and the other
  8 are `visitButterflyImpl`'s captured mode flag and reshaped switch, which neither document mentions.
- `Source/JavaScriptCore/heap/GCSegmentedArray.h`, comment on `loadTopRelaxed`: "Codegen is identical to the plain
  accesses." It is not (+1 load per pop, +1 load and +1 jump per append). A source comment, recorded here because
  the documents cite it.
- PERF-RESULTS 6.12 micro table: `array-int32-to-double-relabel` flag off 1.18 is described as one row; at 200,000
  iterations it measures Baseline/DFG-tier code and their C++ operations (+78 instructions per iteration of 582); in
  the FTL steady state the difference is about +20.

### E: What was run

- `perf record -e instructions:u -c 2000000` of six JetStream tests (splay, OfflineAssembler, pdfjs, UniPoker,
  Babylon, Air) on `main` and on the final flag-off binary, one run each, in parallel.
- `perf stat -e instructions:u` of a fixed-graph collection benchmark (K = 0 and 20 full collections, default markers
  and one marker) and of the relabel microbenchmark at six iteration counts, both binaries.
- `perf record -c 1000000` of those two benchmarks, both binaries.
- Debugger single-step traces (one invocation, other threads held): `JSArray::tryCreate`; 12,000 instructions of the
  marker's drain loop with one marker thread; both binaries. Traces of the array-push and ensure-double operations
  were attempted and abandoned (the traced thread deadlocks when the trap sender's suspend signal arrives while other
  threads are held).
- `objdump` / `nm` of some thirty symbols in both binaries; `git diff cf1b36ec8703 HEAD` of the heap and array
  sources.
- No build, no suite, nothing multi-threaded beyond the engine's own helper threads.

## Section F. RegExp and string entry points (flag off, GIL on, GIL off)

Scope: `RegExp.prototype.exec/test`, `String.prototype.match/replace/search/split` (regexp and string separator),
the substring helpers under them, and what the same measurements showed about `Array.prototype.join` (UniPoker) and
`StringImpl` reference counting (the GIL-on `regexp-exec` row). Tree: the tenth-round final tree; `main` is
`cf1b36ec8703`. Release, Linux x86-64. Every number below is an instruction count or an event count.

Method used throughout (new in this phase): **an exact instruction trace of one warmed-up call**. The benchmark runs
400,000 iterations (the function is in the FTL), calls a marker host function, then calls the function again; a
debugger script breaks at the marker, sets a temporary breakpoint at the entry point's first instruction and single-steps
until the entry point returns, recording every program counter; each is then mapped to its instruction text, source
line and innermost inlined function. This gives the exact per-call count inside the entry point, per callee and per
source line, in each configuration, and lists every mode gate the call executes. Per-call totals come from
`perf stat -e instructions:u` at two iteration counts (difference divided by the iteration difference); per-test
attribution from `perf record -e instructions:u -c 2000000` on single JetStream tests.

### F: Inventory

#### F-I1. Per-call instruction counts today (all configurations) - explained

Microbenchmark: 64 distinct subject strings (`"hello" + i + " world " + (12345 + i) + " foo"`, so nothing folds),
`re = /(\w+)\s(\d+)/`, `reG = /o/g`, one `noInline` function per entry point, FTL-compiled.

| entry point | main | flag off | GIL on | GIL off | flag off - main | GIL off - GIL on |
|---|---|---|---|---|---|---|
| `re.exec(s)` | 1,463 | 1,536 | 1,535 | 1,767 | +73 (+5.0 %) | +232 (+15 %) |
| `re.test(s)` | 905 | 903 | 908 | 1,148 | -2 | +240 (+26 %) |
| `s.match(re)` | 1,461 | 1,536 | 1,543 | 1,769 | +75 | +226 |
| `s.replace(reG, "0")` | 2,189 | 2,430 | 2,435 | 3,131 | +241 (+11 %) | +696 (+29 %) |
| `s.split(/\s/)` | 2,316 | 2,647 | 2,649 | 4,105 | +331 (+14 %) | +1,456 (+55 %) |
| `s.search(re)` | 959 | 988 | 998 | 1,111 | +29 (+3 %) | +113 (+11 %) |
| `s.split(" ")` | 1,252 | 1,410 | 1,414 | 1,628 | +158 (+13 %) | +214 (+15 %) |
| `s.slice(2, 11)` | 121 | 129 | 130 | 143 | +8 | +13 |

GIL on equals flag off to within 10 instructions per call on every row: by instruction count there is no GIL-on
RegExp gap (the GIL-on gap is cycles, F-I6).

Inside the entry point (exact trace, one call; the remainder of the per-call total is the caller's FTL code):

| `exec`: `operationRegExpExecNonGlobalOrSticky` | main | flag off | GIL off |
|---|---|---|---|
| Yarr JIT code | 693 | 693 | 693 |
| the operation's own body (everything inlined) | 367 | 422 | 551 |
| `jsSubstringOfResolved` x3 | 162 | 177 | 204 |
| `threadRegExpGlobalDataSlow` | - | - | 28 |
| `regExpGilOffPerThreadMatchOvector` | - | - | 15 |
| `VM::softStackLimitForCurrentThreadGilOffSlow` | - | - | 11 |
| `MatchingContextHolder::executingRegExpSlotGILOff` | - | - | 10 |
| total | 1,222 | 1,292 | 1,512 |

| other entry points, total inside | main | flag off | GIL off |
|---|---|---|---|
| `operationRegExpSearchString` | 753 | 781 | 889 |
| `operationStringProtoFuncReplaceRegExpString` | 2,042 | 2,282 | 2,967 |
| `regExpSplitFast` | 2,140 | 2,461 | 3,883 |
| `stringSplitFast` | 1,125 | 1,270 | 1,465 |
| `operationRegExpTestString` (GIL off only; inline elsewhere) | - | - | 922 |

#### F-I2. Flag off above main: every entry point pays one Config-page test per redirection - explained

Configuration: flag off (and GIL on, identically).

Mechanism. Each per-thread redirection the GIL-off design needs is reached through an inline helper whose flag-off arm
is "test a byte of the frozen Config page, branch": `VM::gilOffWithProcessGate()` (`runtime/VM.h`),
`VM::group3Primitives()` (the `topCallFrame` store of every JIT operation prologue, `interpreter/FrameTracers.h`),
`VM::trapsMaybeNeedHandlingForCurrentThread()` (**every `RETURN_IF_EXCEPTION` expansion**, `runtime/ExceptionScope.h`,
whose own comment says the macro is no longer byte-identical to `main`'s), and the `useTaggedButterflies` byte in
`JSObject::butterfly()` / `JSObjectWithButterfly::butterfly()` (`runtime/JSObject.h`). A test is 2 instructions when the
Config base is in a register, 3 with the byte load, plus a `lea` of the Config base whenever a call clobbered the
register. The RegExp paths stack these: `RegExp::ovectorSpan(VM&)` and `RegExp::compileIfNecessary`
(`runtime/RegExpInlines.h`), `threadRegExpGlobalData()` (`runtime/RegExpGlobalDataInlines.h`), two in
`Yarr::MatchingContextHolder`'s constructor (stack limit, executing-RegExp slot; `yarr/YarrMatchingContextHolder.h`),
the allocator selection in every `allocateCell` (`VM::ropeStringSpace` / `arraySpace` behind the gate), the cache
exclusions in `regExpSplitFast` / `stringSplitFast` (`runtime/RegExpPrototype.cpp`, `runtime/StringPrototype.cpp`),
`vm.gilOff()` (a VM-member byte, not the Config byte) in `RegExp::match`'s release assertion and in
`addToRegExpSearchCache` (`runtime/StringPrototypeInlines.h`).

Evidence: Config-byte tests executed in one call (exact trace), and the instructions attributed to gate source lines:

| entry point | flag off - main | tests of `gilOffProcess` | tests of `useTaggedButterflies` | extra `lea` of the Config base | instructions on gate lines |
|---|---|---|---|---|---|
| `exec` | +70 | 11 (2 call-frame, 3 exception-check, 6 RegExp/allocator) | 6 | 8 | 59 |
| `search` | +28 | 6 | 0 | 3 | 29 |
| `replace` (5 matches) | +240 | 38 (13 exception-check, 23 RegExp/allocator) | 0 | 28 | 137 |
| regexp `split` (4 matches) | +321 | 40 (23 exception-check, 17 RegExp/allocator) | 11 | 27 | 195 |
| string `split` | +145 | 17 (11 exception-check) | 1 | 13 | 59 |
| `test` | 0 | none: the whole match is inline FTL code flag off | | | |

`main` executes none of these (one read of the Config page per call, the structure-ID base).

The remainder of each delta is lost inlining, F-I3.

Breadth (not RegExp-specific, recorded here because it dominates `replace` and `split`): the final tree has 26,150
static sites that test the `gilOffProcess` byte and 4,164 that test `useTaggedButterflies`; `lea` of the Config base
appears 34,831 times against 11,492 on `main`; the text segment is 35.26 MB against 30.50 MB (+15.6 %).
`operationRegExpExecNonGlobalOrSticky` is 6,970 bytes against 4,066.

Worth (flag off over `main`, instruction samples, single runs of the final tree): JetStream `regexp` 6,322 against
6,015 (1.051), of which +250 of the +307 samples are in the entry points above (`operationRegExpExecNonGlobalOrSticky`
+86, `operationRegExpExec` +62, `jsSubstringOfResolved` +37, `replaceUsingRegExpSearch` +36,
`operationStringProtoFuncReplaceRegExpEmptyStr` +14, the out-of-line 8-bit `StringImpl` constructor +11);
`OfflineAssembler` 10,981 against 10,204 (1.076), +561 of +777 in them (`operationRegExpExecNonGlobalOrSticky` +254,
the out-of-line `MatchingContextHolder` constructor +81, `operationRegExpMatchFastString` +80, out-of-line `jsSubstring`
+50, `jsSubstringOfResolved` +47, `RegExp::match` +30, out-of-line `RegExpObject::create` +19); `UniPoker` 3,446 against
3,311 (1.041), about +40 of +135 (the rest is rope resolution and `join`, F-I8). The quiet pass's rows: regexp 0.925,
OfflineAssembler 0.962, UniPoker 0.947 of `main`.

#### F-I3. Flag off: helpers the compiler stopped inlining - explained

Configuration: flag off, GIL on.

Mechanism. The cold GIL-off arm of each gate is inline (`VMLite::currentIfExists()`, two compares, a field load), so
every `RETURN_IF_EXCEPTION`, every `butterfly()` and every allocation grows the body of the function it is expanded in;
functions that were under the inliner's threshold on `main` are over it now. Evidence (exact trace, string `split`):
`jsSubstring(JSGlobalObject*, VM&, JSString*, unsigned, unsigned)` (`runtime/JSString.h`, two `RETURN_IF_EXCEPTION`) is
a called function flag off - 132 instructions for 4 calls - where `main` has it inside the split lambda (445 -> 413
for the lambda: net +100). Regexp `split`: `JSObject::putDirectIndex` is a called function (135 for 4 calls) where
`main` inlines it into `regExpSplitFast`. `replace`: the 8-bit `StringImpl(unsigned, Force8Bit)` constructor and
`Ref<StringImpl>` adoption are called (22 per call). `Yarr::MatchingContextHolder`'s constructor: the tenth round moved
its GIL-off lookup out of line and recorded the constructor as inlined again; in the final binary it is still emitted
out of line (213 bytes) with 47 call sites (4 on `main`, all in `RegExp::matchConcurrently`) - the hot 8-bit JIT leg of
`operationRegExpExecNonGlobalOrSticky` has it inline, the other legs and `operationRegExpMatchFastString`,
`operationRegExpExec*`, `RegExp::match` call it: 81 instruction samples (0.74 %) in OfflineAssembler flag off, none on
`main`. `RegExpObject::create` likewise (19 samples).

#### F-I4. GIL off: per-thread RegExp state is looked up per use, not per call - explained

Configuration: GIL off.

Mechanism. Four pieces of state are per thread GIL off, each behind its own out-of-line lookup that starts from the
thread-local lite pointer again:
1. the match scratch (`regExpGilOffPerThreadMatchOvector`, `runtime/RegExp.cpp`: a `thread_local Vector<int>` with a
   destructor, grown to the pattern's size) - `RegExpGlobalData::performMatch` (`RegExpGlobalDataInlines.h`) asks for
   it up to three times per match;
2. the legacy-statics stream (`threadRegExpGlobalDataSlow`, `runtime/JSGlobalObject.cpp`: the lite, four routing
   compares, an acquire load of the purge generation, a three-field thread-local memo) - once per `performMatch` /
   `recordMatch`, i.e. once per match inside the `replace` and `split` loops;
3. the executing-RegExp slot (`MatchingContextHolder::executingRegExpSlotGILOff`) and
4. the soft stack limit (`VM::softStackLimitForCurrentThreadGilOffSlow`, `runtime/VM.cpp`) - once per match each.
Plus, not RegExp-specific: the exception check through the lite (about 10 instructions against 3; 23 checks in one
regexp `split`: 236 against 75), the call-frame store through the lite, the allocator through
`Heap::allocationClientForCurrentThread` (about 16 per allocation, 4-5 allocations in an `exec`), and the tag of a fresh
array's butterfly.

Evidence (exact trace, GIL off, one call): thread-pointer (`fs:`) loads 17 in `exec`, 72 in `replace`, 85 in regexp
`split`; scratch lookups 1 / 14 / 11; legacy-statics lookups 1 / 5 / 4. `replace`: 210 instructions in the scratch
lookup and 140 in the legacy-statics lookup of 2,967. `exec` +220 over GIL on: RegExp per-thread state about 120,
allocation about 65, exception check and call frame 22, butterfly tags about 30.

Worth (GIL off over GIL on, instruction samples): JetStream `regexp` 7,285 against 6,335 (1.150): the four lookup
symbols 232 (`threadRegExpGlobalDataSlow` 94, scratch 59, stack limit 43, slot 36), and +517 inside
`operationRegExpExec`, `operationRegExpExecNonGlobalOrSticky`, `operationStringProtoFuncReplaceRegExpEmptyStr`,
`RegExp::match`, of which the trace says about half is the same lookups' inline halves and gates. `OfflineAssembler`
14,451 against 11,027 (1.311): the lookup symbols and the constructor 991 (`MatchingContextHolder` +314, slot 267, stack
limit 177, legacy statics 128, scratch 105), i.e. 29 % of the difference and 6.9 % of what the test executes GIL off.
Quiet-pass rows: regexp 0.79, OfflineAssembler 0.71 of GIL on.

#### F-I5. GIL off: `RegExpTestInline`, constant folding and the caches are off - explained

Configuration: GIL off.

Mechanism. (a) `DFGStrengthReductionPhase` (`dfg/DFGStrengthReductionPhase.cpp`, the `RegExpExec`/`RegExpTest`/... case)
returns after `convertToStatic()` when `vm().gilOff()`: no `foldToConstant()` (a match against a constant string is a
constant plus a `RecordRegExpCachedResult`), no `convertTestToTestInline()`, and no `convertToSticky()`. The reason is
in `runtime/RegExpCachedResult.h`: both forms store the six fields of the realm's in-object `RegExpCachedResult`
inline, and GIL off that stream is per thread and reachable only through C++. `compileRegExpTestInline` and
`compileRecordRegExpCachedResult` (DFG and FTL) fail-stop GIL off as a tripwire. Note what `RegExpTestInline` is: not a
call into Yarr code but the pattern's match-only code compiled into the FTL/DFG body (`Yarr::jitCompileInlinedTest`),
with no offset vector, no matching-context holder and no stack-limit read; its only state outside the frame is the
cached result. No configuration inlines `RegExpExec`: it is an operation call on `main` too. (b) `regExpSplitFast` and
`stringSplitFast` skip `vm.stringSplitCache()` GIL off (VM-wide, unlocked) and use a `thread_local` index vector;
`addToRegExpSearchCache` uses the lite's own `StringReplaceCache` (exists). (c) `Yarr::interpret`
(`yarr/YarrInterpreter.cpp`) uses a `thread_local BumpPointerAllocator` GIL off.

Evidence: `test` is 1,148 instructions per call GIL off against 908: the match itself is 601 in both; GIL off adds
`RegExpObject::testInline` 132, `RegExp::match` 112, `operationRegExpTestString` 38, legacy statics 28, stack limit 11
= 321 where the inline form spends about 60. OfflineAssembler: `RegExp::match(…, unsigned)` +313, `RegExpObject::testInline`
+86, `operationRegExpTest` +23 samples over GIL on. JetStream `regexp`: `operationRegExpExec` 752 against 554 samples
where per-call cost rose 15 %: about 115 samples are calls that GIL on folded.

Worth: about 420 samples (2.9 %) of OfflineAssembler GIL off; 1-2 % of `regexp`; `test` micro 1.26 -> about 1.03.

#### F-I6. GIL on: `regexp-exec-1M` at 1.13 is three locked instructions per iteration, not instructions - explained

Configuration: GIL on (and GIL off).

The micro row (`re.exec("id " + i + "-abc")`, 1 M iterations) executes 1,929 instructions per iteration flag off and
1,929 GIL on, but 464 cycles against 517 (three runs each, 1.39-1.41 G against 1.55-1.57 G cycles for 3 M
iterations). `mem_inst_retired.lock_loads`: 4.3 per iteration flag off (`main` the same: allocator locks), 7.2 GIL on.
Mechanism: with the flag on the process-wide shared atom table is latched (`Options.cpp` forces
`useSharedAtomStringTable`; `InitializeThreading.cpp` calls `WTF::enableSharedAtomStringTable()`), and in that mode
`StringImpl::deref()` (`wtf/text/StringImpl.h`) is always `fetch_sub(release)` - `main`'s and flag off's "refcount is
1, nobody can be racing, skip the read-modify-write" shortcut is the legacy arm only - and `StringImpl::cost()` sets
the reported-cost bit with `fetch_or` (`JSString::create` reports the cost of every string it wraps). Each iteration
creates two `StringImpl`s (the number's digits, the resolved rope): two locked decrements when the sweep destroys the
`JSString`s, one locked `or` in `JSString::create`. Sampled by symbol: `MarkedBlock::Handle::specializedSweep` +1.8
locked loads per iteration, `JSString::create` +0.9; about 18 cycles each accounts for the 53-cycle difference.

Worth: the `regexp-exec` micro row (1.13); a share of every string-heavy GIL-on and GIL-off row ("atomization through
the shared atom table" in the GIL-on Open item is this plus the table's shard locks).

#### F-I7. GIL off regexp `split`: the result array is built through the owner-relabel protocol - explained

`createArrayFromSplitSpans` (`runtime/RegExpPrototype.cpp`) and the non-cached leg of `stringSplitFast` build the result
with `constructEmptyArray(globalObject, nullptr, n)` (Undecided) and `putDirectIndex` per element. The first store
converts Undecided to Contiguous: `convertUndecidedToContiguous` flag off (95 instructions), GIL off
`JSObject::relabelIndexingShapeConcurrent` (235 + 175 in its publication lambda, two compare-and-swaps, +53
`Structure::nonPropertyTransition`), and every store is `trySetIndexQuicklyConcurrent` (340 for four elements against
`setIndexQuickly` 144). About 660 of regexp `split`'s +1,422 GIL off. The array is fresh and unpublished: none of the
protocol is needed. (Array relabels in general belong to the array proposals; this one is local to `split`.)

#### F-I8. GIL off `Array.prototype.join`: the two-pass joiner is off - explained (adjacent finding)

`fastArrayJoin` (`runtime/ArrayPrototypeInlines.h`) and `arrayProtoFuncJoin` skip `JSOnlyStringsAndInt32sJoiner` when
`g_jscConfig.gilOffProcess`: it measures the lanes, allocates, then re-reads them, and a racing writer could overrun
the buffer. UniPoker GIL off over GIL on, 4,723 against 3,460 samples: `operationArrayJoin` +391, `appendStringToData`
+99, `JSStringJoiner::joinImpl` +54, its vector's destructor +46, against -289 for the fast joiner's symbols: net +300
(6.4 % of the test); generated code +670 is the larger part and is not strings. UniPoker's RegExp share is about 100
samples. Quiet-pass row: UniPoker 0.78 of GIL on.

#### F-I9. Not explained / not pursued

- `regexp`'s and OfflineAssembler's `[JIT]` sample differences GIL off (+151, +784) are generated code outside this
  area (polls, tag predicates, the LLInt put_by_id exit storm of LANDING-PLAN).
- Flag off `JSRopeString::resolveRope` (+64 samples OfflineAssembler, +29 UniPoker) and `resolveToBuffer` were not
  traced here; they are on the flag-off per-symbol list of another section.

### F: Designs

#### F-D1. One per-thread RegExp context, resolved once per entry point (GIL off) - **Status: proposed**

Rule. Every RegExp / string entry point that can match (the `operationRegExp*` family, `RegExpObject::exec/test/match`,
`regExpSplitFast`, `replaceUsingRegExpSearch` and its siblings in `StringPrototypeInlines.h`, `regExpSearchFast`,
`RegExpSubstringGlobalAtomCache::collectMatches`, the RegExp constructor's legacy-statics getters) resolves ONE
reference at its top, `RegExpThreadState& ts = regExpThreadState(vm, globalObject)`, and passes it down;
`RegExpGlobalData::performMatch`, `recordMatch`, `resetResultFromCache`, `RegExp::matchInline*`,
`Yarr::MatchingContextHolder`, `createRegExpMatchesArray`, `genericSplit` take it as a parameter instead of calling
`threadRegExpGlobalData()`, `ovectorSpan(vm)`, `softStackLimitForCurrentThreadSlow()` and
`executingRegExpSlotGILOff()` themselves. GIL off `regExpThreadState` is one thread-pointer load (`VMLite::current()`)
plus the legacy-statics memo check below; the state is a struct embedded in `VMLite`:

```
struct RegExpThreadState {            // VMLite member, JIT-addressable by offset
    RegExp*           executingRegExp;        // today's Group-4 slot, moved in
    int*              matchScratch;           // grow-only, fastMalloc'd, freed in ~VMLite
    unsigned          matchScratchCapacity;
    JSGlobalObject*   legacyStaticsRealm;     // one-entry memo of (realm -> stream)
    RegExpGlobalData* legacyStatics;          // the realm's in-object stream on the main carrier,
    uint64_t          legacyStaticsGeneration;//   else the per-(realm, lite) table's entry
    Vector<unsigned>  splitIndices;           // replaces stringSplitIndicesForCurrentThreadGILOff()
    std::unique_ptr<StringSplitCache> splitCache;   // F-D3
    // existing: VMLite::stringReplaceCache; the soft stack limit stays in threadContext.traps()
};
```

Who writes, who reads. Only the owning thread reads or writes any field while the lite is installed (I11's
owner-thread rule); the collector's conductor clears `splitCache` at cycle end inside the stop, as it already does for
`stringReplaceCache` (`Heap::finalize`'s registry walk); `~VMLite` frees the buffers after the lite left the registry.
`matchScratch` grows under the owner only and is never shrunk; a span handed out is invalidated by the thread's next
match of a larger pattern, which is today's documented lifetime rule of `regExpGilOffPerThreadMatchOvector`. The memo
is filled by `threadRegExpGlobalDataSlow` (kept as the miss path, under the table's leaf lock) and validated by
`legacyStaticsRealm == realm && legacyStaticsGeneration == perLiteRealmTable().purgeGeneration.load(acquire)`.

Tiers. C++ slow paths and operations only in this step; LLInt and Baseline reach RegExp through the same operations and
host functions. DFG/FTL are F-D2.

Memory ordering. Everything in the struct is thread-private, so plain accesses on both x86-64 and arm64. The one
cross-thread edge is the purge: a realm's or lite's death bumps `purgeGeneration` under the table lock BEFORE the
entries are detached and freed (`purgePerLiteRealmStateForLite` / the global-death half, `JSGlobalObject.cpp`), and the
fast path's acquire load of the generation pairs with it. The hazard it closes is address reuse: a new realm allocated
at a dead realm's address would hit a stale memo whose stream is freed. A thread can only come to execute in the new
realm through a publication that happens after the realm's creation, which happens after the purge (the sweep that
freed the cell ran the purge first), so its acquire load sees the bump. On x86-64 the acquire is a plain load; on arm64
`ldar` (one instruction, once per entry point). The `thread_local` destructor-bearing vector goes away, and with it the
TLS-guard sequence.

Collector. No change to what is visited: per-lite streams stay in the per-(realm, lite) table that the realm's
`visitChildren` walks under the leaf lock, their barrier owner stays the realm, `record()` keeps its
`vm.writeBarrier(owner)`. The memo holds no cell the collector needs (the realm is on the thread's stack while the memo
can be consulted; the stream is malloc memory). `matchScratch` holds integers.

Stop protocol, watchpoints, deferred claims. None touched: no new lock, no new park site; the miss path's leaf lock is
today's.

What a second thread can observe. Nothing new. Two threads that `exec` the same RegExp object still race on
`lastIndex` (a program-level race on an ordinary slot), each with its own scratch; `RegExp.$1` and friends stay
per-thread (SD19).

Flag off. With F-D4, flag off and GIL on do not run this code at all (their instantiation uses `main`'s direct
accesses). If F-D4 is not taken, the interim form is ONE `gilOffWithProcessGate()` test per entry point selecting
between the legacy direct accesses (`regExp->m_ovector`, `globalObject->regExpGlobalData()`, `vm.m_executingRegExp`,
`vm.softStackLimit()`) and the context, in place of one test per helper per match; that alone removes 5 of `exec`'s 11
`gilOffProcess` tests, 22 of `replace`'s 38 and 16 of regexp `split`'s 40, and costs flag off nothing it does not pay
today.

Failure modes and detection. (1) A consumer that holds a scratch span across a second match: exists today, unchanged;
the debug assertion in `matchInlineOnce` that the span is not the cell-resident vector stays. (2) A stale memo after
address reuse: the generation check; a Debug assertion in the fast path that the stream found equals what the slow
lookup returns (every Nth call). (3) A missed call site that still uses the out-of-line lookups: keep them, counted
(below), and assert the count is zero in the test.

Tests. `JSTests/threads/vmstate/regexp-thread-state-one-lookup-per-call-gil-off.js`: with
`--countJSThreadsCounters=1`, new counters `regExpThreadStateResolve` (incremented in `regExpThreadState`) and
`regExpLegacyStaticsSlowLookup`; runs `exec`, `test`, `match`, `replace` (5 matches), both `split`s and `search` 10,000
times on the main thread and on a spawned thread: resolve count equals call count (before: not a counter, but the
trace's 14 scratch and 5 stream lookups per `replace`), slow lookups at most one per (thread, realm).
`shared-objects/regexp-legacy-statics-per-thread-after-realm-reuse-gil-off.js`: a spawned thread matches in realm A
(created with the shell's realm function), A is dropped and collected, realms are created until one reuses the address
(bounded loop, skip if none), the thread matches there: `RegExp.$1` must be that match's. The existing
`shared-objects/regexp-first-match-publication-race.js` and the SD19 corpus stay as they are.

Verification. Corpus in four modes, TSan GIL off (the struct replaces three `thread_local`s TSan already models), the
amplifier on the two new tests, the mirror harness's RegExp stress files.

Expected gain. Per call, from the trace: `exec` -95 of 1,767 (the four out-of-line lookups 64 + their inline halves and
gates about 40, less 9 for the resolve); `replace` -330 of 3,131 (210 + 140 + gates, less the resolve); regexp `split`
-300 of 4,105. JetStream, instruction samples GIL off: `regexp` -330 of 7,285 (GIL off / GIL on 1.150 -> about 1.10),
OfflineAssembler -900 of 14,451 (1.311 -> about 1.23).

Risks. Signature churn across about 40 call sites; a JIT thunk that enters `matchInline` with a caller-passed span
(`DFG`'s `RegExpExec` family lands in the operations, so none today).

Rejected. (a) Keeping the four lookups and making each cheaper (inline thread-local memo per lookup): still four
thread-pointer loads and four validity checks per match. (b) A per-RegExp, per-thread scratch keyed in a hash map:
a lookup per match again. (c) Making the stream shared and locked: a lock acquisition on every successful match, and
it changes SD19.

#### F-D2. `RegExpTestInline`, `RecordRegExpCachedResult` and constant folding GIL off - **Status: proposed** (needs F-D1)

Rule. GIL-off DFG/FTL compilations emit the cached-result stores against the CURRENT THREAD's stream: `loadVMLite`
(one `mov reg, fs:[offset]` on x86-64, `mrs` + `ldr` on arm64 Linux; `AssemblyHelpers::loadVMLite`), load
`lite.regExpThreadState.legacyStaticsRealm`, compare with the node's constant realm, load and compare
`legacyStaticsGeneration` against the process generation word (absolute address), load `legacyStatics`; on any mismatch
call `operationResolveRegExpLegacyStatics(globalObject)` (returns the stream, fills the memo), then the six stores of
today relative to that pointer (`RegExpGlobalData::offsetOfCachedResult()` + the `RegExpCachedResult::offsetOf*`
accessors, whose layout stays frozen). `DFGStrengthReductionPhase` drops its GIL-off early return (so `foldToConstant`,
`convertTestToTestInline` and `convertToSticky` run GIL off), and the two fail-stop tripwires become this emission.

Tiers. FTL `compileRegExpTestInline` (inside the patchpoint generator: the realm constant is already materialized in a
scratch; two more scratches are available - `numGPScratchRegisters` is 5-6), FTL `compileRecordRegExpCachedResult`, DFG
`compileRegExpTestInline` and `compileRecordRegExpCachedResult` (`DFGSpeculativeJIT64.cpp`). LLInt and Baseline have no
inline form. C++: the new operation.

Why the rest of the inline test is already thread-safe: `jitCompileInlinedTest` compiles from the immutable pattern and
flags, uses only registers and the caller frame's argument area (sized at compile time through `m_parameterSlots` /
`requestCallArgAreaSizeInBytes`), reads the subject's characters (immutable), and never touches the RegExp cell's
scratch, code pointers or state byte; the FTL frame's own stack check covers the area.

Memory ordering. The stream is thread-private: plain stores, both architectures. The generation compare is a load of
a process word written under a lock with a sequentially-consistent read-modify-write; the reader needs acquire
semantics for the address-reuse argument of F-D1: x86-64 plain load; arm64 `ldar` (or the plain load followed by
`dmb ishld` on the hit path; one per executed node).

Collector. `DFGStoreBarrierInsertionPhase` already treats both nodes as stores into the realm cell and plants the
barrier on it; the realm's `visitChildren` visits every per-lite stream, so greying the realm re-scans the stream the
stores went to. A helper marker can read `m_lastInput` / `m_lastRegExp` while the owner rewrites them: single aligned
words, either value is a live cell (the barrier re-greys the owner), and `m_reified` is already read with a relaxed
atomic there.

Stops and polls. The inlined match has no poll (as the out-of-line Yarr code has none: `yarr/Yarr.h` records that a
long match delays a stop in every form); no new park site. Nothing here is a heap fact another thread can rewrite, so
no invalidation point is needed after the node beyond what the node has today.

What a second thread can observe. Nothing: the stores land in the executing thread's stream. With
`foldToConstant` on, a match against a constant string no longer reads the RegExp's `lastIndex` for non-global
patterns (as flag off); for global/sticky ones the fold keeps `SetRegExpObjectLastIndex`, an ordinary store to the
shared RegExp object (program-level race, as the operation's store is).

Flag off / GIL on. Unchanged emission (the baked realm-relative address); the new arm is compiled only when
`vm().gilOff()`.

Failure modes. (1) The inline stores racing the thread's own C++ `record()`: same thread, program order. (2) A lite
switch between the memo check and the stores: impossible inside a node (no call, no poll). (3) Wrong stream after a
nested foreign-VM entry on the same thread: the memo is keyed by realm and lives in the lite of THIS VM; a nested VM
has its own lite. Detection: a Debug-only check after the node that the C++ lookup agrees with the pointer used.

Tests. `jit/regexp-test-inline-gil-off.js`: `--countJSThreadsCounters=1`; a hot `re.test(s)` on the main thread and on
two spawned threads, each thread then reads `RegExp.lastMatch` / `RegExp["$1"]` and must see its own last subject;
counts calls of `operationRegExpTestString` through a new counter (before: one per iteration, 300,000; after: only the
warm-up's). `jit/regexp-constant-fold-records-per-thread-statics-gil-off.js`: `/(a+)b/.exec("xaab")` in a hot function
on two threads with different constants per thread (two functions), legacy statics checked per thread; asserts through
`numberOfDFGCompiles`-style introspection that the exec folded (the result array's identity is fresh but no operation
ran: the new counter again).

Verification. Corpus four modes; TSan cannot see JIT stores, so the amplifier run of the two tests with the C++
cross-check enabled in Debug is the detector; the JSC stress files `regexp-*` and `regress-*cached-result*` GIL off.

Expected gain. `test` 1,148 -> about 945 per call (the 321 C++ instructions become about 60 inline plus 12 for the
memo). OfflineAssembler GIL off about -420 samples (2.9 %), `regexp` -1 to -2 %. With F-D1: OfflineAssembler 1.311 ->
about 1.19, `regexp` 1.150 -> about 1.09.

Risks. The memo check adds 5-6 instructions to a node that is 6 stores today; acceptable against a 320-instruction
operation. Register pressure inside the patchpoint (two more scratches).

Rejected. Pinning a register to the lite GIL off (would make the load free everywhere, but it takes a callee-save
register from every tier and is a separate decision). Storing through the realm's in-object stream when "only one
thread has ever matched" and switching later: a process-wide mode switch with code invalidation for a small gain.

#### F-D3. Per-thread split cache - **Status: proposed** (small)

`RegExpThreadState::splitCache`: a `StringSplitCache` per lite, created on first fill, consulted and filled by
`regExpSplitFast` / `stringSplitFast` GIL off exactly as the VM's is flag off (atom subject, no limit, not having a bad
time), cleared by the conductor at every cycle end in the same registry walk that clears `stringReplaceCache` (the
VM's split cache is cleared at every collection too, `Heap::finalize`), freed in `~VMLite`. The cached value is an
immutable copy-on-write `JSCellButterfly`; the array handed out is a fresh `JSArray` over it, as today. Keys are atom
`StringImpl`s: GIL off atoms live in the shared table and may be dereferenced from any thread, which is why
`VMLite.cpp` already drops the replace cache before the lite's teardown; same here. Memory ordering: owner-only plus
the in-stop clear. Gain: only programs that split the same atom repeatedly (JetStream `regexp`: `regExpSplitFast` is 75
of 7,285 samples; OfflineAssembler none). Test: `shared-objects/split-cache-per-thread-gil-off.js`, a counter of cache
hits per thread (before 0, after n-1 of n identical splits), results compared with a fresh split.

#### F-D4. Flag off and GIL on run `main`'s code in these entry points: mode-instantiated entry points - **Status: implemented for flag off in another form, thirteenth round**

*Thirteenth round.* Implemented for the flag-off half, not by a template parameter handed through the helpers but by a
process-wide mode byte that the compiler treats as constant and that a copy compiled per mode states once
(`runtime/ThreadsModePage.h`; SPEC-ungil history, thirteenth round). The entry points named below are compiled per mode, with
the interpreter's 192 slow paths, 109 JIT operations, 15 host functions, 96 other out-of-line functions and 25 inline ones; `Yarr::MatchingContextHolder`,
`RegExpObject::create`, `jsSubstring` and the cell allocation are inlined again where `main` inlines them. GIL on runs the
threaded copy, which is the code it ran before: the half of this proposal that gives GIL on `main`'s code is not done (the
copy with threads would need the GIL-on/GIL-off distinction as a second mode).


Problem. F-I2 and F-I3: the cost is not any one gate but their number (17 to 53 per call) and the code growth that
un-inlines helpers. Hoisting gates helps partly (F-D1's interim form); only removing the GIL-off arms from the code
flag off executes brings the counts back to `main`'s and the function sizes with them.

Rule. A compile-time mode, `enum class ThreadsMode : bool { Main, Threaded }`, parameterizes the inline helpers that
carry a mode gate: `VM::trapsMaybeNeedHandling<Mode>()` and with it a `ThrowScopeFor<Mode>` / `RETURN_IF_EXCEPTION_M`,
the call-frame tracer of `JSC_DEFINE_JIT_OPERATION`'s prologue, `allocateCell<T, Mode>` (allocator selection),
`JSObject::butterfly<Mode>()`, and the RegExp helpers of F-D1 (`regExpThreadState<Mode>` is the legacy direct accesses
in `Main`). In `Main` each is exactly `main`'s body; in `Threaded` each is today's body (dynamic gates kept, so a GIL-on
process with per-thread tags, the escape hatch, runs `Threaded` unchanged). The hot entry points are templates on the
mode and instantiated twice: the fourteen `operationRegExp*` / `operationStringProtoFuncReplaceRegExp*` /
`operationStringSplit*` operations, `regExpSplitFast`, `stringSplitFast`, `replaceUsingRegExpSearch` (both overloads),
`regExpSearchFast`, `RegExp::match` (both), `RegExpObject::exec/test/match`, `jsSubstring`, `jsSubstringOfResolved`,
`createRegExpMatchesArray*`.

Selection, at no run-time cost: `Threaded` iff `vm.gilOff() || Options::useTaggedButterflies()`, a per-VM constant
fixed in the VM constructor.
- DFG/FTL/Baseline call sites: the emitter picks the function pointer when it emits the call
  (`vmCall(..., selectForMode(vm, operationX<Main>, operationX<Threaded>), ...)`); generated code is per VM. Both
  instantiations are registered in `JITOperationList`.
- Host functions (`regExpProtoFuncExec`, `regExpProtoFuncTest`, `stringProtoFuncSplit`, `stringProtoFuncReplace*`,
  the private `@regExpSplitFast` / `@regExpSearchFast` / `@regExpMatchFast`): `RegExpPrototype::finishCreation`,
  `StringPrototype::finishCreation` and `JSGlobalObject::init` create the `JSFunction`s at run time and pass the
  instantiation for the VM's mode.
- LLInt slow paths do not reach these functions directly.

Memory ordering, collector, stops: none involved; this is code selection. Both instantiations are semantically today's
code in their mode.

What flag off sees: `main`'s instruction sequences in these functions (the structure-ID-base read of the Config page
remains, as on `main`), `main`'s function sizes (`operationRegExpExecNonGlobalOrSticky` 4,066 bytes instead of 6,970),
and the helpers inlined again. GIL on sees the same code as flag off.

Cost: text. The `Threaded` instantiations are today's sizes, the `Main` ones `main`'s: about +150 KB for the list above
(`replaceUsingRegExpSearch` alone is 61 KB on `main`), against the +4.75 MB the branch already carries. Measured both
ways before it lands (size and the per-call table).

Failure modes. (1) A call path that reaches a `Main` instantiation in a `Threaded` VM: every `Main` instantiation
starts with `ASSERT(!vm.gilOffWithProcessGate() && !Options::useTaggedButterflies())`, and the Debug corpus GIL off
finds it; in Release the selection is by construction (one selector function, grep lint that no emitter names an
instantiation directly). (2) A helper left un-parameterized inside a `Main` body: costs instructions, stays correct;
found by the instruction-parity gate below. (3) Two VMs of different modes in one process (a Worker's GIL-on VM in a
GIL-off process): selection is per VM, and `Threaded` code is correct GIL on.

Tests. Instruction parity cannot be a `JSTests/threads` test; it is a gate: `Tools/threads/bench-gate.sh` gains an
instruction-count mode (`perf stat -e instructions:u`, two iteration counts) over the seven-entry micro of F-I1 with
`main`'s counts as baseline and a 1 % threshold, run flag off and GIL on. Behaviour: the corpus and the JSC suites in
all modes (the `Main` instantiations run flag off and GIL on, the `Threaded` ones GIL off and with
`--useJSThreadsSingleOwnerWithGIL=0`). A Debug-only test, `api/mode-instantiation-selection.js`, runs every listed entry
point in both flag-on modes with the assertion armed.

Expected gain (flag off over `main`, instruction samples): `regexp` 1.051 -> about 1.010 (the +250 samples listed in
F-I2), OfflineAssembler 1.076 -> about 1.021, UniPoker 1.041 -> about 1.03; per call every row of F-I1 back to `main`'s
count. GIL on the same. Quiet-pass rows today: regexp 0.925, OfflineAssembler 0.962, UniPoker 0.947 of `main`.

Risks. A template parameter threaded through a dozen headers; merge cost against upstream changes in
`StringPrototypeInlines.h` / `RegExpPrototype.cpp`. The mode-parameterized scope and allocation helpers are the reusable
part: the same two-instantiation recipe applies to any other concentrated flag-off loss (array creation, rope
resolution).

Rejected. (a) One test at the top of each entry point that tail-calls a `Threaded` clone: also two instantiations, and
2-3 instructions per call remain flag off, but no selection plumbing; keep as the fallback if the emitter-side
selection proves awkward. (b) Moving only the cold GIL-off arms out of line (`NEVER_INLINE` slow functions behind each
gate): restores inlining and most of the text, keeps every test (17-53 per call) - about half the gain; it slows GIL
off by a call per exception check unless F-D5 lands. (c) A function-pointer table in the VM consulted at each call:
an indirect call per helper, worse than the gates.

#### F-D5. The exception check and call-frame gates (cross-cutting; owner: the flag-off proposals) - **Status: implemented for flag off by F-D4's mechanism, thirteenth round**

*Thirteenth round.* In a copy compiled without threads the exception check, the call-frame tracer and the stack-limit accessors
are `main`'s (their gates fold). In code that is not compiled per mode they are what they were: one test of the mode byte each,
which the compiler now merges within a function because the byte is constant memory to it.


`RETURN_IF_EXCEPTION` flag off is `movzx Config.gilOffProcess; test; jne` ahead of `main`'s three instructions, 23 times
in one regexp `split`; the macro is written about 3,600 times in JavaScriptCore (each use expands once per template or inline instantiation, and the binary has 26,150 sites that test the byte in all). GIL off it is about 10 instructions because the thread's word
AND the VM's word are tested. Two changes, independent: (1) GIL off, VM-wide trap requests are fanned out by the
requester to every registered lite's word (the stop conductor walks the registry already), so a mutator tests its own
word only; (2) the throw scope captures the address of the word to test when it is constructed
(`&vm.trapsForCurrentThread()`, one gate per scope instead of one per check), and `RETURN_IF_EXCEPTION` loads through
it. Flag off that is one gate per function with a scope instead of one per check; with F-D4's `Main` instantiation it is
none. Details (which traps are VM-wide, the fan-out's ordering against a lite registering concurrently) belong with the
VM-state owner; listed here because it is 69 of regexp `split`'s +321 flag off and 161 of its +1,422 GIL off.

#### F-D6. `StringImpl`: no locked instruction for a non-atom's last reference or a fresh string's cost bit (GIL on and GIL off) - **Status: proposed**

Rule (shared-atom-table mode only; legacy mode untouched).
- `StringImpl::deref()`: before the `fetch_sub(release)`, `if (m_refCount.load(acquire) == s_refCountIncrement &&
  !(hashAndFlags() & s_hashMaskStringKind)) { destroy; return; }` (plain strings only: neither atom nor symbol; the
  static bit lives in the count, so a static string never reads as exactly one increment). Argument: a string whose
  count is 1 is reachable only through the reference being dropped. The ways another thread gains a reference without
  already holding one are table hits on raw pointers - the shared atom table (`tryRefAtom` under the shard lock) and
  the symbol registry - which exist only for atoms and symbols; in-place atomization of a co-owned string
  (`AtomStringImpl::addSlowCase`) requires the atomizer to hold a reference, so the count would be at least 2, or the
  atomizer is this thread. So for a plain string observed at count 1 no concurrent increment is possible, and its kind
  bits cannot flip under us. (To be re-verified at implementation: that no other table in WTF or JavaScriptCore keeps
  raw `StringImpl*` keys to plain strings and re-references them on a hit; the ones found by reading -
  `atomStringToJSStringMap`, the split and replace caches, `NumericStrings` - key on atoms or hold references.) The acquire load pairs with the previous owners' release
  decrements (the value 1 was written by one of them, or is the constructor's), so their accesses happen before the
  destruction - the role the acquire fence after the zero transition plays today. x86-64: a plain load. arm64: `ldar`
  instead of `ldaddl` + `dmb`.
- `StringImpl::cost()`: `JSString::create(VM&, Ref<StringImpl>&&)` and the rope-resolution paths call
  `costOfUnpublished()` (exists, plain store) when the `StringImpl` was created by the caller in the same expression
  (number-to-string, `resolveRope`'s fresh buffer, `jsSubstring`'s copies); `cost()` keeps `fetch_or` for strings of
  unknown provenance.

Collector: `JSString` destruction during sweep runs the shortcut; a dead cell's string is unreachable from any
mutator. Stops: none. Second thread: cannot hold a pointer by the argument above; a `StringView` over the characters
without a reference is a bug in every mode.

Flag off: both arms are behind the shared-table latch that is never set flag off; no instruction changes (the latch
test exists today).

Failure modes: a future path that hands out references to non-atoms through a table (a string cache shared between
threads that stores raw pointers and re-refs on hit) would break the argument; the rule is written next to
`tryRefAtom` and enforced by a Debug assertion in `ref()` that a 0->1 transition never happens.

Tests. `vmstate/stringimpl-last-ref-no-rmw.js` cannot count locked instructions; the gate is the micro: cycles per
iteration of `regexp-exec-1M` GIL on within 2 % of flag off on a quiet machine, and `mem_inst_retired.lock_loads` per
iteration equal to flag off's (4.3 against 7.2 today). TSan corpus both modes (the acquire load is modelled). A stress
test, `shared-objects/string-last-ref-vs-atomize-gil-off.js`: threads create strings, pass them through
`SharedArrayBuffer`-free channels (object properties), atomize them as property keys on one thread while another drops
its last reference; ASAN/Debug run, 10,000 rounds.

Expected gain. `regexp-exec-1M` GIL on 1.13 -> about 1.00; about 18 cycles per temporary string in every flag-on
configuration (json-parse, string-concat, the RegExp tests' substrings that get resolved).

Rejected. Running GIL on without the shared table (the VM's table swapped at GIL hand-off, as `main`'s `JSLock` does
for embedders): removes every locked string operation GIL on, but a process-global latch decided at initialization
cannot follow a per-VM GIL mode, and atoms cross VMs in Bun; recorded under decisions.

#### F-D7. Build a split's result array directly (all configurations) - **Status: proposed**

`createArrayFromSplitSpans` and `stringSplitFast`'s uncached leg allocate the result with
`JSArray::tryCreateUninitializedRestricted` for `ArrayWithContiguous` at the known length and fill it with
`initializeIndex` inside an `ObjectInitializationScope` (the pattern `createRegExpMatchesArray` uses), instead of an
Undecided array plus `putDirectIndex` per element. The array is unpublished until returned, so GIL off none of the
owner-relabel protocol runs and the butterfly is tagged once at creation. Having-a-bad-time realms keep the generic
path (as `createRegExpMatchesArrayForPlainRegExpHavingABadTime` does). Substring allocation inside the initialization
scope needs the deferral context those helpers already take (`jsSubstringOfResolved(vm, &deferralContext, ...)`).
Gain per 4-element regexp `split`: flag off and `main` about -150 of 2,461 (`convertUndecidedToContiguous` 95-101,
`putDirectIndexSlowOrBeyondVectorLength` 77-98 replaced by four stores); GIL off about -650 of 3,883. This is a change
to code `main` runs: reason (faster everywhere, removes a GIL-off protocol from a fresh object), impact (same array
shape and structure as today after the first store). Test: `objectmodel/split-result-array-built-fresh-gil-off.js`
counts `relabelOwnerLeg` (before: 1 per split; after: 0) and compares results with the generic path, including
undefined capture elements and a having-a-bad-time realm.

#### F-D8. `Array.prototype.join`: a bounded second pass GIL off - **Status: proposed**

`JSOnlyStringsAndInt32sJoiner::tryJoin<shape, Checked>`: GIL off (`Checked = true`) the second pass writes through a
cursor that checks the remaining capacity and the buffer's width before every append and gives up (returns null, the
caller falls to the one-pass `JSStringJoiner`) if a lane no longer fits, is no longer a string or Int32, or the final
length differs from the measure. Memory safety no longer depends on the lanes being stable; the result may mix values
from before and after a racing store, as the one-pass joiner's can. Lane reads are single aligned 64-bit loads on both
architectures; a string lane's `length()` and `is8Bit()` are immutable per string. The array's storage is pinned by the
flat-butterfly snapshot the caller already takes. Flag off and GIL on instantiate `Checked = false` (today's code,
and the `gilOffProcess` tests in `fastArrayJoin` move into the template selection). Gain: UniPoker GIL off about -300
of 4,723 samples (6.4 %). Test: `shared-objects/array-join-fast-path-vs-writer-gil-off.js` (a writer thread alternates
short and long strings in one lane while two threads join; every result must be a concatenation of values the lanes
held; ASAN build; counts fast-path successes through a counter: 0 before, most calls after).

### F: arm64 / non-Linux notes

- `RegExp::m_state` / code publication (`RegExp.h`, `RegExpInlines.h`): writer stores the code pointers, then a
  store-store fence, then the state byte, then `publishCodeGILOff` (`exchangeOr`, release). GIL-off reader:
  `hasPublishedCodeGILOff` acquire load, then plain reads of state and code: ordered on arm64 by the acquire (`ldarb`).
  The interpreter arm adds a load-load fence before reading `m_regExpBytecode`. Sound on arm64 as written. Flag off /
  GIL on read `m_state` and the code pointers with no fence: single mutator at a time, and a GIL hand-off is a lock
  release/acquire.
- `RegExp::m_atom` (`publishAtom` release store of the impl pointer; readers `racyLoad` it and dereference): relies on
  an address dependency from the pointer load to the character reads. Holds on arm64 hardware; in C++ terms it is a
  relaxed load feeding a dereference (F2-style). GIL off the read happens after `hasPublishedCodeGILOff`'s acquire in
  every match path, which already orders it, so the dependency is belt and braces there; `RegExp::hasValidAtom()`
  callers outside `matchInlineOnce` (`RegExpObject::matchGlobal`, `removeAllUsingRegExpSearch`) rely on it alone. If a
  portable rule is wanted: make `atomImplConcurrently()` an acquire load (free on x86-64, `ldar` on arm64).
- `RegExp::minimumSize()`, `constructionErrorCode()`, `specificPattern()`: relaxed single-word advisory loads; any
  value ever stored is correct to act on. Fine on arm64.
- `threadRegExpGlobalDataSlow`'s memo generation: acquire load against a read-modify-write under a lock; fine. F-D1 and
  F-D2 keep that pairing; JIT code on arm64 must emit `ldar` (or `ldr` + `dmb ishld`) for the generation word.
- `RegExpCachedResult::lastResult`'s `storeStoreFence` before `m_reified = true` GIL off, and the collector's relaxed
  read of `m_reified`: the marker tolerates either order (it visits null barriers), so no reader-side fence is needed
  on arm64.
- `loadVMLite` exists for Linux x86-64 and Linux arm64 only (constant-offset initial-exec TLS); elsewhere GIL off is
  refused at option validation, so F-D2's emission has no other target to cover. macOS would need the fast-TLS key
  form that `loadButterflyTIDTag` already has.
- F-D6's `deref` shortcut: needs `load(acquire)` on arm64 (`ldar`), not a relaxed load, for the happens-before edge
  from previous owners' release decrements to the destruction. On x86-64 the legacy arm's relaxed load is already
  ordered.
- Nothing in this area relies on x86-64 store order beyond the items above.

### F: Decisions for the user

1. **How far to go for flag off in these entry points.** (a) F-D4, two instantiations selected per VM: `main`'s counts
   exactly, about +150 KB of text, a template parameter through the RegExp/string headers. (b) One gate per entry
   point and a `Threaded` clone: 2-3 instructions per call remain, same text, no selection plumbing. (c) Hoist only the
   RegExp gates (F-D1's interim form): `exec` +70 -> about +40, `replace` +240 -> about +130, no new text.
   Recommendation: (a), starting with the operations the JIT calls (selection is one line per call site there), host
   functions second; it is also the recipe for the other concentrated flag-off losses.
2. **Legacy statics stay per thread GIL off (SD19).** F-D2 compiles the ruling into generated code. Alternative: a
   shared, locked stream (one lock per successful match, `main`'s cross-thread visibility of `RegExp.$1`).
   Recommendation: keep SD19; say so in the API notes.
3. **The `StringImpl` last-reference shortcut (F-D6)** changes WTF code that Bun's own threads run in shared-table
   mode. It buys the GIL-on `regexp-exec` row and about 18 cycles per temporary string flag on. The argument rests on
   "only atoms can be re-referenced through a table". Recommendation: take it, with the Debug assertion; alternative is
   to leave GIL on at 1.13 on that row.
4. **Whether GIL on should keep the shared atom table at all** (rejected alternative of F-D6): not recommended now; it
   would make GIL on's strings exactly flag off's but needs the latch to become per VM.
5. **F-D7 changes code `main` runs** (split's array construction). Small and local; recommendation: take it, it also
   removes a GIL-off protocol from a fresh object.

### F: Doc mismatches

- LANDING-PLAN, "Results, tenth round", **Flag off** paragraph and the "Removed cost" bullet: "the RegExp matching
  context's constructor inlines into the match operations again ... one call less per match". In the final binary
  `Yarr::MatchingContextHolder::MatchingContextHolder` is still emitted out of line and called from 47 sites (4 on
  `main`); only the hot 8-bit JIT leg of some operations has it inline. OfflineAssembler flag off spends 81 of 10,981
  instruction samples in it, `main` none. Fix: say "on the 8-bit JIT leg of `operationRegExpExecNonGlobalOrSticky`; the
  other legs and operations still call it (Open items)".
- LANDING-PLAN Open items, GIL off (4), and PERF-RESULTS §6.12 "RegExp and strings": "`RegExpTest`/`RegExpExec` are not
  inlined as calls into Yarr code GIL off". No configuration inlines `RegExpExec` or calls Yarr code inline: what is
  off GIL off is `RegExpTestInline` (the pattern's match-only code compiled into the DFG/FTL body) together with
  `foldToConstant`, `RecordRegExpCachedResult` and `convertToSticky` (`DFGStrengthReductionPhase`'s GIL-off early return).
  Fix the wording in both places.
- SPEC-jit history §54, last paragraph ("looked up the per-thread match scratch three times per match", fixed): true
  for the split index vector; `RegExpGlobalData::performMatch` still calls `RegExp::ovectorSpan(vm)` up to three times
  per match GIL off (14 lookups in one five-match `replace`, 11 in a four-match regexp `split`). Fix: "the split paths'
  index vector; the match scratch itself is still looked up per use (DESIGN-PROPOSALS F-D1)".
- LANDING-PLAN Open items, "Flag off above `main`": names `jsSubstring` and `StringImpl`'s 8-bit constructor as the two
  helpers no longer inlined; the trace adds `JSObject::putDirectIndex` (regexp `split`), `RegExpObject::create` and the
  matching-context constructor, and the cause (the inline cold arm of every gate growing callers past the inliner's
  threshold) is not stated.
- LANDING-PLAN / PERF-RESULTS micro table: `regexp-exec-1M` GIL on 1.13 is listed among rows "above 1.10" without a
  cause; it is not an instruction-count difference (1,929 = 1,929) but three locked instructions per iteration from
  shared-atom-table mode (F-I6).
- `runtime/RegExp.cpp` banner comment, routing (1): quotes `vm.gilOff() ? ... : ...`; the code is
  `vm.gilOffWithProcessGate()` (`RegExpInlines.h`). Cosmetic.

### F: What was run

All with the existing binaries (`main` and the final tree, Release); nothing was built.
- `perf stat -e instructions:u` on a seven-mode RegExp micro and a two-mode substring micro, two iteration counts,
  four configurations (about 120 runs of 1-3 s).
- `perf record -e instructions:u -c 4000003` on `exec` and `test` (8 runs), `-c 2000000` on JetStream `regexp`,
  `OfflineAssembler`, `UniPoker` in four configurations (12 runs, in parallel).
- Debugger single-step traces of one call (1,000-4,000 steps each): `exec` x4 configurations, `search`, `replace`,
  regexp `split`, string `split` x3, `test` GIL off (20 runs).
- `perf stat -e cycles:u,instructions:u,mem_inst_retired.lock_loads:u` and a lock-load `perf record` on the
  `regexp-exec-1M` loop, flag off / GIL on / `main` (12 runs; cycles are the only load-sensitive numbers here, taken at
  load 10 and consistent across three runs each).
- `objdump` of both binaries once (for instruction text and static gate counts), `nm -S` for sizes.
- GIL-off runs were launched from bash scripts with the three environment variables word-split by `env`; every GIL-off
  profile shows the GIL-off-only symbols (`threadRegExpGlobalDataSlow` etc.), which confirms the mode.

## Section G. WebAssembly with the flag on

Scope: what separates WebAssembly from `main` in the two flag-on configurations, and the design that removes it.
GIL off the target is SPEC-ungil §I's own v1 - wasm executes on carriers (main/embedder threads) only, spawned Threads
are refused - which the tree currently supersedes by forcing wasm off altogether. GIL on the target is `main`'s call
performance after the first spawn and two behaviour defects found while reading. Multi-threaded wasm execution stays
out of scope; stage 3 lists what it would need.

All file and function names below were checked against the current tree. "Carrier" is SPEC-ungil's term: a native
thread that enters the VM through the API lock (`JSLock::m_lock`), as opposed to a Thread spawned from JS. GIL off,
carriers still exclude one another through `m_lock` (SPEC-ungil §F.1), so "carrier-only" means at most one thread of a
VM executes wasm at any moment, although which native thread that is may change over time.

### G: Inventory

#### W1. GIL off, WebAssembly does not exist (explained)

- Configuration: GIL off. Worth: 117 of the 197 GIL-off JSC-suite results that pass on `main` (list below), Bun's 47
  cases, `typeof WebAssembly === "undefined"` for every program, and `stress/baselinejittrue.js` (wasm off sets
  `useLLInt`, exactly as `main --useWasm=0`).
- Mechanism: `applyGILOffActivationChecklist` (`runtime/Options.cpp`) stores `Options::useWasm() = false` whenever
  `useJSThreads && !useThreadGIL`; `disableAllWasmOptions` then clears the tiers, fast memory and the fault handler
  (option dump GIL off: `useWasm=false useWasmIPInt=false useBBQJIT=false useOMGJIT=false useWasmFastMemory=false
  useWasmFaultSignalHandler=false`, `useJSPI=true`). An explicit `--useWasm=1` is overridden. Behind it stand three
  more gates that would have to go at the same time: the second arm of `throwIfWebAssemblyRefusedOnSpawnedThread`
  (`wasm/js/JSWebAssemblyHelpers.h`: any thread of a GIL-off VM gets a TypeError), and the
  `RELEASE_ASSERT(!vm.gilOff())` in `JSWebAssemblyModule::create` and `JSWebAssemblyInstance::tryCreate`.
- The stated reason (the comment in `Options.cpp`, the helper's comment): the wasm tiers and their glue read and write
  the VM-level Group-3 words, which GIL off are inert - the C++ side already publishes through the current thread's
  `VMLitePrimitives` (`VM::group3Primitives()`), so an exception thrown under a carrier's wasm call would be missed by
  the emitted check (W2). That is the whole of blocker (a); (b) and (c) of the Open item are W3 and W10.
- The 117, from the final tree's GIL-off suite against `main` (classified by the first error line): 55 `new
  WebAssembly.Module` (`stress/hoist-get-wasm-exports.js`, `table-with-noexternref-wasm-type.js`,
  `taintedness-tracking-wasm-proxying.js`, 17 configurations each, and four microbenchmarks), 38 `new
  WebAssembly.Memory` (`stress/map-forEach.js` x17, `shared-wasm-memory-with-zero-byte.js` x16, four `memcpy-wasm*`
  microbenchmarks, `resizable-array-constant-folding.js`), 17 `stress/wasm-gc-structureid-cast-optimization.js` (needs
  Wasm GC: W7), 3 `new WebAssembly.Instance`, and one each of `new WebAssembly.Table`, `WebAssembly.Memory`,
  `WebAssembly.instantiate`, `WebAssembly.compile`. None of them spawns a Thread.
- Bun's 47: `web/fetch` (the `WebAssembly.compileStreaming` / `instantiateStreaming` cases), `bun/jsc` (the
  "Wasm (BBQ/OMG)" JIT stress group, which includes five JSPI files, and two upgrade checks), `web/workers` and
  `bun/util` (a `structuredClone` transfer-list check). All run wasm on the thread that owns the VM; the Worker cases
  run it in a Worker's VM, which in a GIL-off process is a GIL-on VM (U0b) of the same process, so it shares the
  process-wide option and the process-wide wasm code (W11).

#### W2. Emitted code and asm that touch per-thread VM words (explained; recounted)

The Open item says "fifteen sites in seven files". Recounted on the current tree (the JSPI assembly came with the last
rebase and is not in that count): 15 word accesses in 5 JIT files, 25 in the two IPInt assembly files (counting each macro once), one raw C++
member read. None is inside a BBQ or OMG function body: every one is in a thunk, a trampoline, an entry/exit wrapper
or an interpreter slow-path stub. The OMG generator's `m_vmValue` and BBQ's three `JSWebAssemblyInstance::offsetOfVM()`
loads (`emitWriteBarrier*`) feed only `VM::offsetOfHeapBarrierThreshold()` / `offsetOfHeapMutatorShouldBeFenced()`,
which are the shared heap's words, not per-thread ones: the JS tiers bake the same two addresses GIL off
(`AssemblyHelpers::barrierBranch`). They need no change.

JIT (all reach the VM through `JSWebAssemblyInstance::offsetOfVM()` because wasm code is not compiled for one VM):

| File, function | Word | Access |
|---|---|---|
| `wasm/WasmThunks.cpp` `throwExceptionFromWasmThunkGenerator` | `topEntryFrame` | load (callee-save buffer) |
| `wasm/WasmThunks.cpp` `throwExceptionFromOMGThunkGenerator` | `topEntryFrame` | load |
| `wasm/WasmThunks.cpp` `catchInWasmThunkGenerator` | `topEntryFrame` (through `restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(vmGPR, ...)`), `callFrameForCatch` | 2 loads |
| `wasm/WasmIRGeneratorHelpers.h` `emitThrowRefImpl`, `emitThrowImpl` | `topEntryFrame` (through `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(vmGPR)`) | 2 loads |
| `wasm/js/JSToWasm.cpp` `createJSToWasmJITShared` (exception path), `RTT::jsToWasmICEntrypoint` (exception path) | `topEntryFrame` | 2 loads |
| `wasm/js/WasmToJS.cpp` `wasmToJS` (after `operationConvertToF32` / `ToF64`) | `m_exception` | 2 tests |
| `wasm/js/WasmToJS.cpp` `wasmToJS` (exception path), `emitThrowWasmToJSException` | `topEntryFrame` | 2 loads |
| `wasm/js/WebAssemblyBuiltinTrampoline.cpp` `generateWasmBuiltinTrampoline` | `topCallFrame` store of null, `m_exception` test, `topEntryFrame` load | 3 |

Assembly (`llint/InPlaceInterpreter.asm` unless noted):

| Label / macro | Word | Access |
|---|---|---|
| `js_to_wasm_wrapper_entry` `.unwind`; its `.handleException` | `topEntryFrame` (LLInt macro `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer`) | 2 loads |
| `wasm_throw_from_slow_path_trampoline`, `wasm_unwind_from_slow_path_trampoline`, `wasm_throw_from_fault_handler_trampoline_reg_instance` | `topEntryFrame` | 3 loads |
| `ipintCatchCommon` (every IPInt catch entry) | `topEntryFrame` (restore), `callFrameForCatch` load and clear, `targetInterpreterPCForThrow`, `targetInterpreterMetadataPCForThrow` | 5 |
| `wasmBuiltinCallTrampoline` | `topCallFrame` store of null, `m_exception` test | 2 |
| `populateSentinelVMEntryRecord` (JSPI) | `topCallFrame`, `topEntryFrame` | 2 loads, 2 stores |
| `restoreSentinelVMEntryRecordAndReturnWithStackOverflow` (JSPI) | `topCallFrame`, `topEntryFrame` | 2 stores |
| `_enterWebAssemblySuspendingFunction` (JSPI) | `topEntryFrame` | load |
| `_exit_implanted_slice` (JSPI) | `topCallFrame`, `topEntryFrame` | 2 stores |
| `.jspi_unwind_current_slice` (JSPI) | `m_exception` | store |
| `InPlaceInterpreter64.asm` `_throw`, `_rethrow`, `_throw_ref` | `topEntryFrame` | 3 loads |

C++: `runWebAssemblySuspendingFunction` (`wasm/js/WebAssemblySuspending.cpp`) reads the member `vm.topEntryFrame`
directly; every other C++ wasm path already goes through `VM::group3Primitives()` (`WasmOperationPrologueCallFrameTracer`,
the unwind-word readers in `WasmOperations.cpp`, `WasmOperationsInlines.h`, `WasmIPIntSlowPaths.cpp`, all commented as
rerouted). So today the writer is per-thread and the reader is not: the split the forcing exists to hide.

Not per-thread and therefore fine as they are: the stack-overflow checks (`JSWebAssemblyInstance::offsetOfSoftStackLimit()`
in the BBQ prologues, `AssemblyHelpers::checkWasmStackOverflow` for OMG, `createJSToWasmJITShared`, the warm entry,
IPInt `checkStackOverflow`) compare against the instance's `StackManager::Mirror`, which mirrors the VM-level
`StackManager`; `VM::updateStackLimits` keeps publishing that word from carrier entries for exactly this reader (its
comment names "wasm instance StackManager mirrors (wasm execution is carrier-only GIL-off, §I)"). `vm.maybeReturnPC`,
`vm.topJSPIContext`, `vm.wasmContext` are VM-level by design and have one user at a time under carrier-only execution.

#### W3. No wasm tier polls, except IPInt at function entry (explained)

- `main` has no trap delivery into wasm loops at all: `VMTraps.cpp` knows wasm only as a fault it cannot attribute
  (`"Either we trapped for some other reason, e.g. Wasm OOB..."`); the signal sender patches JS code blocks only. What
  exists is upstream's stack-mirror mechanism: `VMTraps::requestThreadStopIfNeeded` calls
  `StackManager::requestStop`, which stores the marker `UINTPTR_MAX` into the manager's trap-aware limit and into every
  registered `Mirror`; each `JSWebAssemblyInstance` registers `m_stackMirror` with `m_vm->traps()` in `finishCreation`.
  IPInt's `checkStackOverflow` macro compares the new frame against
  `m_stackMirror.m_trapAwareSoftStackLimit` and calls `ipint_extern_check_stack_and_vm_traps` (which already does the
  GIL-off lite-then-VM dispatch, `handleTrapsForCurrentThreadIfNeeded`). That is a poll per IPInt function entry.
- IPInt loops (`ipintOp(_loop)` -> `ipintLoopOSR`) only bump the tier-up counter. BBQ prologues
  (`BBQJIT::addTopLevel`, `BBQJIT::addLoopOSREntrypoint`) and OMG (`checkWasmStackOverflow`) compare against the plain mirror word,
  which a stop request does not change; OMG leaf functions with small frames have no check at all
  (`OMGIRGenerator::computeStackCheckSize`). BBQ loop headers (`emitLoopTierUpCheckAndOSREntryData`) test the
  force-OSR byte and bump the counter; OMG loop headers (`OMGIRGenerator::addLoop`) emit nothing.
- Consequence GIL off: every stop-the-world (`VMManager::requestStopAll(StopReason::GC)` from the shared heap's
  conductor, Class-A watchpoint fires, jettisons) and every VM-wide trap waits for a carrier inside a BBQ/OMG loop or a
  loop-free recursion until it returns to JS or calls an import; the 30 s stop watchdog then aborts the process. Flag
  off and GIL on this is `main`'s behaviour (a watchdog cannot end a wasm loop on `main` either), and with the GIL the
  other threads are parked anyway, so it matters only GIL off.
- The delivery side already reaches the mirrors GIL off: `VM::requestStop` -> `VMTraps::fireTrapVMWide` sets the bit
  in every lite and in the VM word and then runs the VM-level `updateThreadStopRequestIfNeeded`, which arms the
  VM-level `StackManager` and so the instance mirrors; carrier-only events (`NeedWatchdogCheck`, `NeedDebuggerBreak`,
  `NeedShellTimeoutCheck`) are raised with the VM-level `fireTrap`, same path. One raise does not:
  `VMTraps::fireTargetedTermination` sets the VM word's bit without running the VM-level update (its comment:
  "call-free loops poll only the VM word" - the JS tiers' `CheckTraps` tests `vm().traps().trapBitsAddress()`), so a
  per-thread time limit on a carrier would not arm the mirrors until some other raise does.

#### W4. The spawned-thread refusal is bypassed by C++ call paths (explained; measured)

- Configuration: both flag-on modes. SPEC-ungil §I item (2) requires that "every generated JSToWasm entry emits a
  spawned-TS prologue check". The tree refuses in one place only, the host function `callWebAssemblyFunction`
  (`wasm/js/WebAssemblyFunction.cpp`), and its comment calls that "the single cold JS->wasm entry". It is not:
  `Interpreter::executeCall` (`interpreter/Interpreter.cpp`) and the microtask call (`callMicrotask` in
  `runtime/JSMicrotask.cpp`) test `callData.native.isWasm` and call `vmEntryToWasm` on the function's
  `jsToWasm()` entry directly. The shared entry (`createJSToWasmJITShared`, LLInt `js_to_wasm_wrapper_entry`) has no
  check.
- Measured, final Release build, GIL on, a spawned Thread holding an export `add` created on the main thread:
  `add(1, 2)`, `Reflect.apply(add, ...)`, `Array.from([1, 2], add)` throw the SD7 TypeError; `JSON.parse("[5]", add)`
  returns 5, `"x".replace("x", add)` returns 0, and `Promise.resolve(40).then(add)` fulfils with 40 - wasm ran on the
  spawned thread three times. Every embedder `call()` (the C API, N-API, bound functions) takes the same route.
- Worth: with the GIL it happens to work (the running thread's limit is in the VM word and the mirrors follow it), so
  it is a specification and test gap GIL on (`api/wasm-call-refused-on-spawned-thread.js` covers the direct call
  only). GIL off it would be a second wasm mutator next to the carrier, with the carrier's stack limit: it has to be
  closed before the forcing is lifted.

#### W5. `memory.atomic.wait` parks holding the GIL (explained; measured)

- `Wasm::waitImpl` (`wasm/WasmOperationsInlines.h`) calls `WaiterListManager::waitSync` directly. The JS
  `Atomics.wait` (`runtime/AtomicsObject.cpp`) wraps the same call in a `GILDroppedSection` whenever the flag is on, for
  the reason its comment gives: a notifier on a spawned Thread can never run while the waiter holds the GIL.
  `waitSyncWithPerWaitNode` states the contract ("every caller parks inside a GILDroppedSection") and, GIL off,
  `RELEASE_ASSERT`s that the caller has released heap access.
- Measured, GIL on: the main thread in a wasm `memory.atomic.wait32` with a 3 s timeout on a shared memory while a
  spawned Thread loops on `Atomics.notify`: the wait returns 2 (timed out) after 3,000 ms and the notifier woke nobody;
  with `Atomics.wait` in the same program the wait returns "ok" after 1 ms and the notifier reports 1. With an
  infinite timeout it is a deadlock. GIL off (once wasm exists) the same call would hit the release assertion.
- Worth: no suite result today (no test combines wasm waits with Threads); it is a flag-on behaviour defect of its
  own and a precondition of stage 1.

#### W6. The WebAssembly JS API surface is gated at construction only (explained)

`throwIfWebAssemblyRefusedOnSpawnedThread` is called by the constructors, the `WebAssembly.*` static functions, the
JSPI resumption handlers and `callWebAssemblyFunction`. The prototype functions are not gated:
`WebAssembly.Memory.prototype.grow` / `buffer`, `Table.prototype.get` / `set` / `grow`, `Global.prototype.value`,
`Exception.prototype.getArg`, `Module.exports` / `imports` / `customSections`. GIL on that is harmless (one mutator at
a time). GIL off a spawned Thread holding a carrier-created object could grow a memory, rewrite a table entry (a
multi-word `{callee, entrypoint, instance, rtt}` record that `call_indirect` reads unlocked) or set a global while the
carrier executes wasm on it. The relocating-grow arm in `Wasm::Memory::grow` (stop-the-world publication, annex N6)
is written and dormant; tables and globals have no such arm.

#### W7. Wasm GC is refused under the shared heap (explained)

`JSWebAssemblyInstance`'s constructor copies `subspace->allocatorsForSizeSteps()` - the server's `LocalAllocator`s -
into the instance so that BBQ/OMG can allocate structs and arrays inline through
`JSWebAssemblyInstance::offsetOfAllocatorForGCObject`; a shared heap never materializes server-side allocators (each
thread allocates from its own client's), hence `RELEASE_ASSERT(!Options::useSharedGCHeap())` there, a LinkError in
`tryCreate` and a CompileError from the section parser. 17 of the 117 results
(`stress/wasm-gc-structureid-cast-optimization.js`) stay after stage 1 unless D2 is done.

#### W8. GIL on: the warm entry only until the first spawn; no `CallWasm` (explained; numbers from the tenth round)

`WebAssemblyFunction::jsCallICEntrypoint` returns null once `anyJSThreadEverSpawned()`; the warm entry
(`RTT::jsToWasmICEntrypoint`) tests the same process byte. `DFG::ByteCodeParser`'s `WasmFunctionIntrinsic`
case and `DFG::StrengthReductionPhase` refuse the `CallWasm` conversion whenever `useJSThreads`. Cost, from
SPEC-ungil §I: 5 M calls of an i32 add 11 ms through the warm entry, 338 ms through the cold one. So a GIL-on program
calls wasm at `main`'s speed from the interpreter and Baseline until it spawns its first Thread, 30 times slower per
call afterwards, and never gets the FTL's direct call.

#### W9. `stress/baselinejittrue.js` (explained)

A side effect of W1 (`disableAllWasmOptions` leaves `useLLInt` as `main --useWasm=0` does); goes away with it.

#### W10. Instance and VM state that assumes one wasm mutator (explained; for the record)

Cached memory base/size pairs in the instance and the pinned base/bounds registers refreshed after calls and
`memory.grow`; tables and their entries; globals (including portable bindings); import call-link infos
(`DataOnlyCallLinkInfo`, record protocol flag on, `CallLinkInfo::emitDataICFastPath`); `m_exception` / fault PC
(`setFaultPC` from the signal handler); `vm.wasmContext`'s scratch buffers (OSR entry, catch entry buffers);
`vm.topJSPIContext`; the instance-embedded allocator table (W7). All of it is sound with one wasm mutator per VM and
the JS API refused on spawned Threads (D1.6). What is NOT single-mutator on `main` already and needs nothing: the
module-level machinery (`Wasm::CalleeGroup`, tier-up plans on the wasm worklist, `updateCallsitesToCallUs`'s
`repatchNearCall<jitMemcpyRepatchAtomic>`, the indirect-call entrypoint table, `TierUpCount`, the callee registry, the
fault handler's callee lookup): a `Wasm::Module` posted to Workers runs on several threads of several VMs at once on
`main`, so this layer was written for N executors.

#### W11. Wasm code is not compiled for one VM (explained)

The JS tiers choose Group-3 emission at compile time from the VM they compile for (`vm.gilOff()` in
`AssemblyHelpers::prepareCallOperation`, `loadException`, `loadTopEntryFrame`, ...). Wasm cannot: thunks are process
singletons (`Thunks::singleton()`), a `CalleeGroup`'s code runs under whichever instance is in
`GPRInfo::wasmContextInstancePointer`, and in a GIL-off process the same code serves the GIL-off VM's carrier and the
GIL-on VMs of Workers (U0b). The selection therefore has to happen at run time inside a GIL-off process, and at
emission time only on the process-wide byte (`g_jscConfig.gilOffProcess`). The LLInt is in the same position and has
the pattern: `gilOffGroup3Check` in `LowLevelInterpreter.asm` (Config byte, then the current lite's `gilOff` byte).

### G: Designs

#### D1. Stage 1: carrier-only WebAssembly with the GIL off

**Status: proposed.**

##### D1.1 Rule

GIL off, WebAssembly is enabled. Wasm code executes only on a thread that holds `JSLock::m_lock` of its VM (a carrier);
a spawned Thread can neither enter wasm nor use the WebAssembly JS API (TypeError, SD7's message). Everything the wasm
tiers and their glue read or write that is per-thread GIL off goes through the running thread's lite, selected by the
same predicate the C++ writers use. Every wasm tier polls for stop requests at loop headers and function entries in a
GIL-off process. Wasm GC stays refused (D2 lifts it). This is SPEC-ungil §I's v1 text ("U17 negative arm: carrier
non-GC wasm never throws"); what is removed is the supersession.

##### D1.2 Group-3 words from wasm code: one selector, the writer's

Invariant: for every Group-3 word, the emitted reader and the C++ writer use the same storage. The writer's selector
is `VM::group3Primitives()`: `gilOffProcess && vm.m_gilOff && lite && lite->vm == &vm ? lite->primitives : the VM
block`. The reader gets exactly that:

- *JIT sites* (the first table of W2). New helper `AssemblyHelpers::loadWasmGroup3Base(GPRReg instanceGPR, GPRReg
  destGPR)`, emission keyed on `g_jscConfig.gilOffProcess` (known and frozen before any wasm thunk is generated):
  - byte clear (flag off, GIL on): `loadPtr [instance + offsetOfVM] -> dest`, and callers address
    `[dest + VM::xOffset()]` - the instruction sequences of today, byte for byte. The helper returns which base it
    produced so that callers add `OBJECT_OFFSETOF(VM, topCallFrame)` or 0.
  - byte set: `loadVMLite(dest)`; `branchTestPtr(Zero, dest) -> vmBlock`; `branchTest8(Zero, [dest +
    VMLite::offsetOfGilOff()]) -> vmBlock`; fall through with `dest` = lite (= its primitives, offset 0 by the
    static assertion in `VMLite.h`); `vmBlock:` `loadPtr [instance + offsetOfVM]`, `addPtr
    OBJECT_OFFSETOF(VM, topCallFrame)`. Both arms leave a `VMLitePrimitives*` in `dest`, so the access that follows
    is `[dest + VMLitePrimitives::offsetOf_x()]` either way. No same-VM compare is needed on the lite arm in
    Release: a carrier running wasm of VM A has A's lite installed (the lite-vs-frame coherence invariant the LLInt
    discriminator already relies on); Debug asserts `lite->vm == instance->m_vm`.
  - `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(vmGPR)` and
    `restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(vmGPR, scratch)` get instance-taking twins built on the
    helper; the 15 accesses of the table become calls of the helper or the twins. Scratch discipline: every site
    already owns the register it loads the VM into (argument registers on exception paths,
    `nonPreservedNonReturnGPR` after the conversion calls, `regT5` in the builtin trampoline); `loadVMLite` writes
    only its destination.
- *IPInt assembly* (the second table). The macros exist: `copyCalleeSavesToVMEntryFrameCalleeSavesBufferGroup3`,
  `restoreCalleeSavesFromVMEntryFrameCalleeSavesBufferGroup3`, `branchIfGilOffGroup3ToT{2,3,5,6}` in
  `LowLevelInterpreter.asm`, which includes `InPlaceInterpreter.asm`. The `topEntryFrame` loads become the Group3
  macros; `ipintCatchCommon`, `wasmBuiltinCallTrampoline`, the four JSPI sequences and `.jspi_unwind_current_slice`
  get the split written out with `gilOffGroup3Check` (VM-storage arm = today's code, lite arm = the same accesses at
  `VMLitePrimitives` offsets). Scratch registers per site, from reading the code: `t5` is dead at
  `wasm_throw_from_slow_path_trampoline` / `wasm_unwind_from_slow_path_trampoline` / the builtin trampoline (they
  load the VM into it); `t0` at the three `InPlaceInterpreter64.asm` throw ops; `t3`/`t0` in `ipintCatchCommon`
  (no wasm registers are live at a catch entry); `ws0`/`ws1`/`t0`/`t1` in the JSPI sequences as they use them today;
  `a0`/`a1` at `.unwind`. On x86-64 `t6` is `a0`/`wa0`, which is live in the builtin trampoline and the throw ops, so
  those sites must not use the `ToT6` form.
- *C++*: `runWebAssemblySuspendingFunction` reads `vm.group3Primitives().topEntryFrame`.

Memory ordering: every word is written and read by the same thread (the C++ operation that throws or unwinds and the
glue that checks afterwards run on the carrier), so there is no cross-thread ordering on x86-64 or arm64. The two
discriminator bytes are write-once before publication (`gilOffProcess` latched before the first VM's designation;
`VMLite::gilOff` copied at registration before the lite can be installed).

What a second thread can observe: nothing new. A spawned Thread never executes these sites (D1.4); another VM's
thread executing the same thunk takes the VM-block arm for its own VM. The VM block of the GIL-off VM stays inert.

##### D1.3 Polls

Word: the instance's `m_stackMirror.m_trapAwareSoftStackLimit`, compared with the marker (`StackManager::StopRequestMarkerValue`,
all ones). It is the word upstream chose for wasm, it needs no VM or lite load (`[wasmContextInstancePointer +
const]`), it is per instance and therefore per VM without a discriminator (a Worker VM's instances mirror that VM's
manager), and the raise paths already reach it (W3). Form:

- BBQ, at the loop label in `emitLoopTierUpCheckAndOSREntryData` (before the `canTierUpToOMG()` return; every wasm
  value is in its canonical stack location there because `addLoop` flushed) and once after the prologue's frame setup
  of a function that contains a loop or makes a call: x86-64 `cmpq $-1, off(%instance); je slow` (2 instructions,
  fused), arm64 `ldr tmp, [instance, #off]; cmn tmp, #1; b.eq slow` (3, macro scratch). Slow path in a late path:
  near-call a new thunk `wasmCheckTrapsThunk` that spills all registers (the shape of
  `triggerOMGEntryTierUpThunkGeneratorImpl`), calls `operationWasmCheckTraps(instance)` and either returns or, when the
  operation reports a pending termination, tail-jumps to `throwExceptionFromWasmThunk` with
  `ExceptionType::Termination`. The operation is `ipint_extern_check_stack_and_vm_traps` without the stack part:
  `handleTrapsForCurrentThreadIfNeeded(vm)`, then `vm.hasPendingTerminationException()`.
- OMG, in `OMGIRGenerator::addLoop` at the top of `body` and after the stack-check patchpoint of the root function
  (not of inlined callees): a B3 `Branch` on `Equal(Load(instance, off), -1)` with a rare successor holding a
  patchpoint that calls the same thunk; `effects = Effects::none()` plus `exitsSideways` and `reads = top` (it may
  throw; it writes nothing wasm can see, because nothing else may grow this VM's memories or rewrite its tables while
  the carrier is parked: D1.6). The pinned base/bounds registers need no reload after it for the same reason.
- IPInt: `checkStackOverflow` already polls entries. `ipintLoopOSR` gains `bpeq
  JSWebAssemblyInstance::m_stackMirror + StackManager::Mirror::m_trapAwareSoftStackLimit[wasmInstance], -1, .poll` whose
  slow arm is `operationCall(cCall2(_ipint_extern_check_vm_traps))` and the `.continue` fall-through; gated by the
  Config byte like every IPInt addition (`leap _g_config; bbeq gilOffProcess, 0, .skip`).
- Emission gate for BBQ/OMG: `g_jscConfig.gilOffProcess`. Flag off and GIL on emit nothing.
- One delivery fix: `fireTargetedTermination` on a carrier's lite also runs the VM-level
  `updateThreadStopRequestIfNeeded` (the VM word already has the bit), so a per-thread time limit reaches a carrier's
  wasm loop. Spawned targets are unaffected (they never run wasm).
- Folding the entry poll into the stack check (compare against the trap-aware word instead of the plain one, as IPInt
  does) would make entry polls free, but BBQ compares signed (`LessThan`) and `checkWasmStackOverflow` adds the frame
  size to the limit first, so the all-ones marker reads as -1 or wraps: both would silently never trap. Kept as a
  follow-up with its own emission (subtract from the frame pointer, compare `Below`); stage 1 uses the explicit poll.

Memory ordering: the poll is a relaxed load of a word another thread stores under `m_mirrorLock`. It needs eventual
visibility only, which cache coherence gives on both architectures; nothing is inferred from the value except "take
the slow path", and the slow path re-reads the trap bits with its own atomics and locks. A stale "no stop" costs one
more iteration. The cancel (`cancelStop` restores the limit) racing a poll costs at most one spurious slow-path call.

Cost, derived: on `main` the five JetStream 2 wasm tests execute 9.1 % (gcc-loops), 11.6 % (HashSet), 12.8 %
(richards), 19.0 % (quicksort) and 19.3 % (tsf) branches per instruction (`perf stat -e instructions:u,branches:u`).
If a third to a half of the branches are loop back edges or calls into polled functions, two instructions each add
6-9 % (gcc-loops) to 13-19 % (tsf, quicksort) instructions; the load is independent of the loop's dependency chain and
the branch is never taken, so cycles move less. The JS analogue measured in the tenth round: polling traps on `main`
cost delta-blue 9 % and richards 5 % of their scores. An empty counting loop is the worst case (the JS
`int-loop-3e8` row: 96.6 -> 158.9 ms with poll and invalidation point).

##### D1.4 Who may enter wasm: one choke point in generated code

The refusal moves to where SPEC-ungil §I item (2) put it, the entries themselves, so that no caller can bypass it
(W4): `createJSToWasmJITShared` and the LLInt `js_to_wasm_wrapper_entry` test "is the current thread a spawned
Thread" right after their prologue and, if so, call `operationThrowWasmRefusedOnSpawnedThread` and unwind through
their existing exception path. Discriminator: the `VMLite::isSpawned` byte the SPEC names and the tree does not have
(L2 append; set to 1 by `ThreadManager` before `setCurrent` on a spawned thread's lite, 0 on carriers and on every
GIL-on main lite), read as `loadVMLite; branchTest8 [lite + offsetOfIsSpawned]`, behind `branchTest8
[addressOfAnyJSThreadEverSpawned()]` so that a process that never spawns pays one byte test. Flag off emits nothing
(`Options::useJSThreads()` at thunk generation; the LLInt entry uses `ifJSThreadsBranch`). The C++ checks in
`callWebAssemblyFunction`, `Interpreter::executeCall` and `callMicrotask` stay or are added for the error's stack
and as defence in depth (they call `throwIfWebAssemblyRefusedOnSpawnedThread`). GIL off the warm entry
(`jsCallICEntrypoint`) stays null in stage 1 (every JS->wasm call takes the shared entry; D3 brings the warm entry
back in both modes with the same discriminator); `CallWasm` stays off.

Non-main carriers: allowed. A second embedder thread entering the GIL-off VM has its own carrier lite and excludes
the first through `m_lock`; every selector above uses the current lite, the stack mirror carries whichever carrier
entered last (`VM::updateStackLimits`'s carrier-only VM-word publish), and `DropAllLocks` inside an import restores
the carrier's state on re-acquisition as it does for JS.

##### D1.5 Blocking inside wasm

`Wasm::waitImpl` wraps `waitSync` in `GILDroppedSection droppedSection(vm)` when `useJSThreadsEnabled()`, after the
`isAtomicsWaitAllowedOnCurrentThread()` policy check `Atomics.wait` makes, and polls traps on exit as that path does.
GIL on this releases the GIL for the wait (W5's fix); GIL off it releases `m_lock`, the token and heap access (§J.3),
which is what `waitSyncWithPerWaitNode` asserts. While the carrier is parked there another carrier may enter the VM
and run wasm on the same instance: that is two wasm activations interleaved on one instance, never concurrent, the
same as two embedder threads on `main`.

##### D1.6 The JS API on spawned Threads

Every `WebAssembly.{Memory,Table,Global,Instance,Module,Tag,Exception}` prototype function and accessor calls
`throwIfWebAssemblyRefusedOnSpawnedThread` first (one line each: about twenty host functions in seven files - the Memory, Table, Global, Tag, Exception and
Instance prototypes and `WebAssembly.Module`'s three static functions). That makes "nothing but a carrier mutates
or reads wasm objects through the wasm API" true, which D1.3's patchpoint effects and W10 rest on. What a spawned
Thread can still hold and use: an `ArrayBuffer` or typed array over a memory, obtained on a carrier and passed over -
ordinary JS objects under annex N6 (detach quarantined to a stop, in-place grow publishes the length after the
commit, a relocating grow publishes under a stop: the dormant arm in `Wasm::Memory::grow` becomes live); and opaque
references (an exported function object, a Wasm GC reference once D2 lands), which it can store and pass but not call
or inspect. `memory.buffer` from a spawned Thread is a candidate relaxation (its lazily cached wrapper needs the cell
lock); not in stage 1.

##### D1.7 Collector, stop protocol, watchpoints

- A carrier in wasm is an ordinary mutator with heap access. Conservative scan: the poll's slow path pushes every
  register before it calls out, BBQ has flushed at loop headers, so a carrier parked in a stop has its wasm values on
  the stack, like a carrier parked in an allocation slow path under wasm today. Concurrent marking of instances,
  tables, memories and function wrappers while the carrier runs wasm is `main`'s behaviour.
- Stops: requested as for JS (`fireTrapVMWide`), observed at D1.3's polls, served in
  `VMTraps::handleTraps` -> `notifyVMStop`. JS frames under the wasm frames may be jettisoned during the stop; they
  are re-entered through their invalidation points on return as from any callee. Wasm code is never invalidated by a
  watchpoint (it holds no JS heap facts), so a poll needs no invalidation point and the heap-fact epoch does not apply.
- Write barriers in BBQ/OMG use the shared heap's words (W2). Wasm-to-wasm call-site repatching and tier-up stay as
  on `main` (W10); SPEC-jit I2's "no tier modifies reachable code while more than one mutator may execute JS" is about
  JS code blocks; wasm near-call repatching is upstream's atomic protocol, already exercised by Workers sharing a
  module, and only carriers (one per VM) execute the patched code.
- Destruction: `~JSWebAssemblyInstance` and the memory/table destructors can run from a spawned thread's sweep, as
  they already can GIL on; they take their own locks (`m_mirrorLock`, the callee registry, the memory manager).
- Deferred watchpoint claims, the transition protocols and the object model are untouched: wasm touches JS objects
  only through C++ operations and import calls.

##### D1.8 Flag off and GIL on

Flag off: no emitted byte changes (JIT emission keyed on the process byte; the two entry checks keyed on the flag);
the IPInt additions cost one not-taken Config-byte test at each converted label - all on exception, unwind and JSPI
paths except two per builtin-trampoline call and one per IPInt loop iteration - reported through the `--useJIT=0`
bench gate like the LLInt's Group-3 sites. `VMLite` grows by one byte. GIL on: the entry choke point (one byte test
before the first spawn, a TLS load and a second byte test after), the API gating, the wait bracket; no polls, VM-block
storage.

##### D1.9 Failure modes and detection

| Failure | Symptom | Detection |
|---|---|---|
| A Group-3 site left unconverted | exception thrown under wasm not seen by its check: wrong value returned instead of a throw, or a stale catch frame | Debug: `WasmOperationPrologueCallFrameTracer`'s assertion against `topEntryFrame`; the new test walks every table row; a grep lint over `wasm/` and `InPlaceInterpreter*.asm` for `VM::topCallFrame|topEntryFrame|m_exception|callFrameForCatch|target*ForThrow` outside the helper |
| A site converted with the wrong scratch | clobbered wasm argument or result | the IPInt and JIT stress directories, flag off as well (the VM-storage arm runs there) |
| A loop or recursion without a poll | stop watchdog abort after 30 s naming the carrier as non-quiescent | `gc-stress` test below; `--forceTrapAwareStackChecks=1` run of the wasm stress directory (every poll taken) |
| Poll slow path loses a live value | wrong result after a stop | same run with `--forceTrapAwareStackChecks=1` and a collection in the operation (Debug) |
| A JS->wasm entry not covered by the choke point | wasm on a spawned Thread | Debug assertion `!ThreadManager::isJSThreadCurrent()` in `WasmOperationPrologueCallFrameTracer` and in `ipint_extern_prepare_function_body`; the bypass test below |
| API prototype function left ungated | racy table/global write | the gating test enumerates the prototypes' own properties and calls each on a spawned Thread |

##### D1.10 Tests (JSTests/threads)

- `api/wasm-every-glue-path-gil-off.js`: on the main thread, one module exercising each row of W2's tables - throw
  and catch in IPInt, BBQ and OMG (`--useOMGJIT=0`, `--useBBQJIT=0` variants), `throw_ref`, a trap (unreachable, OOB
  through the fault handler), an import that throws, an import returning an object to an `f32`/`f64` result whose
  `valueOf` throws, a builtin call that throws, stack overflow from each tier, JSPI suspend / resume / reject /
  rethrow - while a second Thread runs JS. Before: `typeof WebAssembly === "undefined"` GIL off. After: passes in all
  four modes.
- `api/wasm-entry-refused-through-every-call-path.js`: a spawned Thread calls a carrier-created export directly,
  through `Reflect.apply`, `JSON.parse`'s reviver, `String.prototype.replace`, a bound function, a promise reaction
  and `Array.prototype.sort`'s comparator. Before (GIL on): three of the paths run wasm and return 5, 0 and 40. After:
  TypeError on all, both modes; the main thread keeps calling the same export.
- `api/wasm-atomic-wait-vs-spawned-notifier.js`: W5's program. Before (GIL on): `timed-out` after 3,000 ms, 0 woken.
  After: `ok`, 1 woken, both modes.
- `api/wasm-api-refused-on-spawned-thread.js`: every own function and accessor of the seven prototypes, called on a
  spawned Thread with a carrier-created receiver: TypeError; on the main thread: works.
- `gc-stress/wasm-loop-polls-gil-off.js`: the main thread in an OMG, a BBQ and an IPInt loop (three runs by option) of
  about two seconds and in a loop-free recursive function, while a spawned Thread allocates and forces collections
  and another triggers Class-A fires; asserts the collections completed during the loop (count from `$vm`). Before:
  not runnable; on a build with wasm on and no polls: stop watchdog abort. After: passes; counts of collections
  served during the wasm loop > 0.
- `api/wasm-termination-in-loop-gil-off.js`: the shell watchdog and a per-thread time limit end a BBQ/OMG loop on the
  main thread (Termination propagates as on the JS side).
- The six existing `api/wasm-*.js` and `api/thread-restrict-wasm-gc-receiver.js` are guarded by `typeof WebAssembly
  !== "undefined"` and start running GIL off; `api/wasm-gc-shared-heap-refused.js` keeps passing until D2.

##### D1.11 Verification

The corpus in four modes, Release and Debug; TSanJIT GIL off on the new tests (the poll word and the discriminator
bytes are the only new cross-thread reads); the amplifier on the new tests. The three JSC suites GIL off (the 100
non-GC results should flip; `baselinejittrue` with them). `JSTests/wasm` was never part of the rounds' suites: run it
GIL off once against `main` in the default configuration - `stress` (592 files), `function-tests` (66), `js-api` (61),
`regress` (35), `ipint-tests` (111), `v8` (259), `references`, `threads-spec-tests` - and the `spec-tests` directory;
then `stress` and `ipint-tests` again with `--forceTrapAwareStackChecks=1` and once each with `--useOMGJIT=0` and
`--useBBQJIT=0 --useOMGJIT=0`. `gc` (82) waits for D2. The mirror harness over `wasm/stress` (two threads running the
same file) checks the refusal, not execution: the second thread must fail cleanly. Bun: the twelve directories GIL
off; expect the 47 back. Flag off: the per-symbol instruction sweep must show nothing in generated code; the
`--useJIT=0` micro gate for the IPInt byte tests. Performance: the five JetStream 2 wasm tests, GIL off over GIL on,
instruction counts.

##### D1.12 Expected result

JSC suites GIL off: 197 -> 96 results that pass on `main` (117 - 17 Wasm GC, and `baselinejittrue`); with D2, 79. Bun
GIL off: the 47 cases (the JSPI five need the JSPI rows of D1.2). JetStream 2's wasm tests run GIL off at 0.85-0.95 of
GIL on by the poll estimate; the 36-test total is unaffected.

##### D1.13 Risks

The scratch-register discipline at 25 assembly accesses (a wrong choice corrupts a wasm argument on the rare path
only); the JSPI stack surgery, which writes `topCallFrame`/`topEntryFrame` while frames are being moved - its lite arm
must publish to the same lite across suspend and resume, and a resume on another carrier thread than the suspend is
legal on `main` and must stay so (the words are re-derived at each entry, not carried in the slices: verify in
review); an upstream change adding a new VM-word site in wasm glue (the lint of D1.9 is the guard); poll cost on
loop-dense modules.

##### D1.14 Alternatives rejected

- *Embed the carrier's lite in the VM* (the Open item's alternative). GIL off every carrier, the process main
  thread included, has a lazily created, separately allocated lite per (thread, VM) (`JSLock.cpp`: "m_mainVMLite
  (tid 0) is GIL-on-only"). Making the VM words BE a carrier's would mean the VM contains a whole `VMLite` whose
  `primitives` is the existing block, that lite being handed to one distinguished carrier. Flag off only constants
  change, but the VM grows by about 2.5 KB (`scratchSegments`, a second `VMThreadContext`), the hot VM fields move
  (the tree has measured cache-line placement there), every "VM block is inert GIL off" fallback and assertion
  inverts for that one carrier, a second embedder thread still needs the run-time selection, and wasm code shared
  with Worker VMs still needs to work on their VM blocks. It would leave the 40 accesses untouched, which is its only
  gain. It does not give the carrier its FFI inline cache back either: the FFI stub and the DFG/FTL FFI call sit in
  JS code that spawned Threads execute too, so they need the lite route (`loadVMLite` + `VMLitePrimitives` offsets,
  two sites) or a spawned-thread guard regardless of where the carrier's words live.
- *A VM-level "current wasm Group-3 base" pointer re-pointed at every carrier lock acquisition*: branch-free and
  TLS-free, but a second selector whose equivalence with `group3Primitives()` has to be proved at every lock path
  (`DropAllLocks`, nested foreign-VM entry, parked-carrier watchdog service), and one more load flag off unless gated
  anyway.
- *Force JSPI off GIL off* to skip its ten accesses: a behaviour difference (`WebAssembly.promising` undefined) and
  five of Bun's cases.
- *Polling the VM's trap-bits word as the JS tiers do*: two dependent loads (the VM, then the word) where the mirror
  needs one, and the word is armed by other threads' targeted terminations that a carrier ignores.
- *Signal-delivered traps for wasm*: asynchronous patching with other mutators running is what I21 forbids GIL off.

#### D2. Wasm GC under the shared heap

**Status: proposed (after D1).** The instance-embedded allocator table cannot hold a thread's allocators when the
thread may change. Step one: in a shared-heap process the table stays empty and the emitted allocation
(`emitAllocateGCStructUninitialized`, `emitAllocateGCArrayUninitialized`, the OMG twins) takes its slow path, the
`operationWasmStructNew` / `ArrayNew` family, which allocates through `tryAllocateCell` on the current thread's client -
correct, an operation call per allocation; the three refusals (section parser, `tryCreate`, the constructor's
assertion) go. Step two, if wasm-GC throughput GIL off matters: resolve the allocator through the running carrier's
table as the JS tiers do (`loadVMLite` -> `tlcTable[slot]`, SPEC-jit §5.5 "Inline allocation GIL off"), behind the
process-byte emission gate and the lite's `gilOff` byte. Struct and array payloads are written by wasm code with the
existing barriers and visited by the concurrent marker as on `main`; JS cannot read or write their fields, and the
accessor exports are refused on spawned Threads, so no second mutator reaches a payload. Tests: `JSTests/wasm/gc`
GIL off against `main`; `api/wasm-gc-shared-heap-refused.js` inverted. Expected: the 17 results.

#### D3. Stage 2, GIL on

**Status: proposed; independent of D1 except for the shared discriminator byte.**

1. *The refusal choke point and the wait bracket* (W4, W5): D1.4 and D1.5 apply to both flag-on modes and are
   behaviour fixes GIL on today. They can land first and alone.
2. *The warm entry after the first spawn.* `jsCallICEntrypoint` hands the entry out whenever the flag is on (GIL off
   too, after D1), and the entry's prologue becomes: process byte clear -> fast; else `loadVMLite`, `isSpawned` clear
   -> fast; else the cold path (which refuses). One byte test before the first spawn, a TLS load and a byte test
   after, against a 30-fold slower call today (338 ms against 11 ms per 5 M calls). On targets without a constant-offset
   TLS read the entry keeps today's process-byte form.
3. *`CallWasm` in DFG/FTL.* The node calls the callee's wasm entry with the instance pinned, bypassing every check, so
   it needs a fact, not a test: a process-level watchpoint set "no Thread has ever been spawned", fired (Class A, so
   inside a stop) by the first spawn. The parser and strength reduction plant `CallWasm` only while it is valid and
   register it as a desired watchpoint; the first spawn jettisons those code blocks; afterwards the conversion is
   refused as today. Programs that use wasm and never spawn get `main`'s FTL code; programs that spawn lose only the
   direct call. A per-thread guard inside the node (exit or generic call when `isSpawned`) would keep `CallWasm` after
   a spawn; it is the follow-up if a workload shows it.
4. Flag off: nothing (all three are behind `useJSThreads`). Tests: `api/gil-on-wasm-warm-entry-before-first-spawn.js`
   extended to assert warm calls on the main thread AFTER the spawn (count `callWebAssemblyFunction` entries with a
   `$vm` counter: before 100 % of post-spawn calls, after 0 % on the main thread, 100 % refused on the spawned one); a
   `CallWasm` test counting FTL compilations and the jettison at the first spawn.

#### D4. Stage 3: wasm on spawned Threads (list only)

Per-thread stack limits in every wasm check (the mirror is per instance, not per thread: the checks move to the lite's
word); instance state under N mutators - memory base/size publication for non-shared memories (or "only shared
memories on spawned Threads"), table entries as single published records, globals, `m_exception`/fault PC per thread;
`vm.wasmContext` scratch and `topJSPIContext` per thread; import call-link records are already N-safe; Wasm GC
allocation through each thread's table (D2 step two); polls already there; `memory.atomic.wait`/`notify` already
per-wait-node; the fault handler is thread-agnostic; JSPI continuations bound to the suspending thread; the
WebAssembly API objects' lazily cached members (`memory.buffer`) under the cell lock; the wasm debugger refused; and
the memory-model statement for plain wasm loads and stores against JS accesses to the same buffer from another thread
(already SAB semantics for shared memories).

### G: arm64 / non-Linux notes

- The Group-3 selection and the entry choke point read the current lite through `loadVMLite` (JIT) and the
  `loadCurrentVMLiteToT*` macros (LLInt/IPInt), which exist for ELF initial-exec TLS on Linux x86-64 and Linux arm64
  only (`OFFLINE_ASM_GILOFF_TLS`; arm64: `mrs tpidr_el0` + `ldr`, clobbering `x16`/`x9` in the assembly macros - `x9`
  must be checked against IPInt's register map at each site, it is a volatile temp there). macOS (no constant-offset
  TLV) and Windows refuse the GIL-off shape at option validation, so D1 inherits "Linux only"; D3.2 falls back to the
  process byte there.
- The poll is a relaxed load of `Atomic<void*>` stored by the requester under a lock; it relies on coherence, not on
  ordering, on both architectures (D1.3). arm64 needs a scratch register for the compare (`ldr; cmn; b.eq`); BBQ has
  the macro scratch free at loop labels, OMG gets one from the patchpoint-free `Branch(Equal(Load, const))` form.
- All Group-3 words are same-thread (D1.2): no fences, no dependency ordering.
- The fault handler runs on the faulting thread (POSIX signals) or on the exception-port thread with the faulting
  thread's state (Mach); it reads the instance from that thread's pinned register and never looks up "the VM's
  thread". Nothing to change; on arm64e the presigned trampoline is per process.
- Wasm call-site repatching is a single aligned branch-with-link rewritten with `jitMemcpyRepatchAtomic` and a
  deferred instruction-cache flush, upstream's protocol for modules shared across Workers; carrier-only execution
  does not add an executor.

### G: Decisions for the user

1. **Do stage 1 (carrier-only wasm GIL off)?** Buys 100 JSC-suite results (117 with D2), Bun's 47, `WebAssembly`
   defined. Costs one helper and 15 JIT call sites, 25 assembly accesses, polls in three tiers, about twenty API functions gated,
   and a wasm verification pass that the rounds have not had so far. Recommendation: yes; it is SPEC-ungil §I's own v1
   and the largest remaining behaviour difference.
2. **Land W4/W5 (D3.1) ahead of everything?** Two GIL-on defects independent of the rest: wasm runs on spawned Threads
   through `call()` and promise reactions despite SD7, and a wasm atomic wait cannot be woken from a spawned Thread.
   Recommendation: yes, first, both modes.
3. **Refuse the whole WebAssembly prototype surface on spawned Threads (D1.6), or keep reads?** Uniform refusal is
   one rule and makes D1.3's "nothing else mutates wasm state during a stop" true by construction; keeping reads
   (`memory.buffer`, `table.get`, `global.value`) serves programs that pass the Memory object instead of its buffer.
   Recommendation: uniform refusal in stage 1; `memory.buffer` as the first relaxation if asked for.
4. **Explicit entry polls or folded into the stack check?** Explicit: 2-3 instructions per call of a function that
   loops or calls, identical in all tiers. Folded: free, but needs a different compare in BBQ and OMG (signedness and
   the limit-plus-size wrap) and a slow path that tells a trap from an overflow. Recommendation: explicit now.
5. **Wasm GC GIL off (D2) through the slow path first?** 17 results for little code, at an operation call per
   allocation. Recommendation: yes, after D1, step one only.
6. **`CallWasm` behind a never-spawned watchpoint (D3.3)?** Recommendation: yes; it costs nothing once a Thread
   exists and gives never-spawning flag-on programs `main`'s code.

### G: Doc mismatches

- SPEC-ungil §I: "every generated JSToWasm entry emits a spawned-TS prologue check. Discriminator: L2-append uint8_t
  VMLite::isSpawned". The tree has neither the byte (`VMLite.h`) nor a check in the shared entry
  (`createJSToWasmJITShared`, `js_to_wasm_wrapper_entry`); the refusal is in `callWebAssemblyFunction` and two C++
  callers of `vmEntryToWasm` skip it (W4). The section should say what is implemented (host-function refusal, warm
  entry gated by the process byte GIL on) and list the choke point as not landed.
- `wasm/js/WebAssemblyFunction.cpp` (comment in `callWebAssemblyFunction`) and
  `JSTests/threads/api/wasm-call-refused-on-spawned-thread.js` call the host function "the single cold JS->wasm entry"
  / "the only JS->wasm entry under useJSThreads"; `Interpreter::executeCall` and the microtask call are two more.
- LANDING-PLAN Open item "WebAssembly GIL off": "fifteen sites in seven files (`InPlaceInterpreter.asm` 6,
  `WasmThunks.cpp` 3, `WasmBBQJIT.cpp`, `JSToWasm.cpp`, `WasmToJS.cpp`, `WebAssemblyBuiltinTrampoline.cpp` 2 each,
  `JSWebAssemblyInstance.h` 1) plus the OMG generator's instance-relative loads". Current tree: 15 accesses in five JIT
  files (none in `WasmBBQJIT.cpp`, whose three VM loads are write-barrier words, and none in OMG for the same reason;
  `WasmIRGeneratorHelpers.h` has two), 25 in `InPlaceInterpreter.asm` / `InPlaceInterpreter64.asm` (JSPI added ten),
  one raw C++ read. "(b) no tier polls" should read: IPInt polls at function entry through the stack mirror; nothing
  polls in loops; BBQ/OMG never.
- LANDING-PLAN / PERF text "111 of the GIL-off suite's 200 results" (Open item) against "117 WebAssembly off" (Results
  and parity table): the final suite has 117.
- `JSWebAssemblyHelpers.h`'s comment gives the exception slot as the reason for refusing carriers; after D1 it should
  name only the spawned-thread rule.

### G: What was run

- `python3 classify.py <main suite> <final GIL-off suite>` over the stored suite results (no test executed) for the
  117-result breakdown.
- Final Release build, GIL on, two single-file programs of under four seconds each (W4's call-path probe; W5's wait
  against a spawned notifier, wasm and JS variants); one option dump GIL off (three `JSC_*` variables written
  literally on the command line).
- `main` Release build: the five JetStream 2 wasm tests once each under `perf stat -e instructions:u,branches:u`
  (about a minute in total).
- Everything else is reading. No build, no suite, no Bun run.

## Section H. Five smaller GIL-off differences: the FFI inline cache, the bytecode profiler, sampling of spawned threads, "one VM spawns Threads", module evaluation

Every statement about the code below was checked against the round-ten tree (the branch head); `main` is the
base commit the branch is rebased on. Numbers are instruction counts (`perf stat -e instructions:u`, difference of two
loop lengths divided by the iteration difference) or event counts; none depends on load.

### H: Inventory

#### H1. The FFI fast paths are off GIL off (explained)

- Configurations: GIL off. 16 results of the JSC stress suite (`ffi-callffi-was-compiled.js`, one per configuration),
  1 Bun test (the same file run as a fixture).
- What is off: `Options::notifyOptionsChanged` (`Options.cpp`, the block after the WebAssembly force-off) clears
  `useFFIICStub`, `useFFICallInDFG` and `useFFIDirectCall` whenever `useJSThreads && !useThreadGIL`. Every call of a
  `JSFFIFunction` then runs `FFI::ffiHostCall` (C++ argument conversion, `StringArena::Scope`, the invoke thunk, C++
  boxing). The test asserts `$vm.ffiCompileCounts().icStub` grows at function creation and that a hot caller compiled
  by the DFG contains a `CallFFI`; GIL off it stops at the first: "no IC stub was compiled by JSFFIFunction creation:
  before 0, after 0".
- Mechanism. Nothing in the FFI patches code and nothing is published at run time. The "inline cache" is an
  immutable per-function host-call thunk generated once in `JSFFIFunction::create`
  (`FFI::generateICStubCode`, `ffi/FFIICStub.cpp`) and installed as the `NativeExecutable`'s call entry, so call
  sites reach it through the ordinary call-link record like any native function. `CallFFI` is a DFG node planted at
  compile time (`FFI::tryConvertCallToCallFFI` from the strength-reduction phase, fed by the parser's
  `CheckIsConstant` + constant-callee `Call`). The three are off only because they bake VM-level words that are inert
  GIL off (SPEC-ungil A.1.3), and because they bypass the spawned-thread refusal that lives in `ffiHostCall`:
  - IC stub (`generateICStubCode`): `storePtr(callFrameRegister, &vm.topCallFrame)` twice (prologue and exception
    handler) and `copyCalleeSavesToEntryFrameCalleeSavesBuffer(vm.topEntryFrame, ...)` once. Its three exception
    checks go through `AssemblyHelpers::emitExceptionCheck(vm)`, which is already mode-keyed
    (`materializeGILOffExceptionSlot`), so the stub's exception reads are NOT a problem any more.
  - DFG (`SpeculativeJIT::compileCallFFI`, `ffi/FFIDFGCodegen.cpp`): one `storePtr(callFrameRegister,
    &vm().topCallFrame)` before the thunk call and one raw `loadPtr(vm().addressOfException(), returnValueGPR)` in
    `emitArenaExitIfExceptionPending`; the node's `exceptionCheck()` calls are mode-keyed already.
  - FTL (`compileCallFFIImpl`, `FTLLowerDFGToB3.cpp`): three `m_out.storePtr(m_callFrame,
    m_out.absolute(&vm().topCallFrame))` (direct call, thunk call with arena, thunk call without) and two
    `m_out.load64(m_vmValue, m_heaps.VM_exception)` in `exceptionCheckWithArenaExit`.
  - The helpers that do the right thing exist and are used by every other tier site:
    `AssemblyHelpers::emitPublishTopCallFrameForHostCall`, `AssemblyHelpers::loadException`, the `gilOff` arm of
    `nativeForGenerator`'s exception handler (`ThunkGenerators.cpp`), FTL `emitPublishTopCallFrame()` and
    `loadCurrentThreadException()`.
  - The refusal: `FFI::throwIfFFIRefusedOnCurrentThread` is called from `ffiCall`, `JSFFIFunction::create` and
    `JSFFICallback::create` only. A stub or a `CallFFI` executed by a spawned thread would reach the global object's
    unlocked `StringArena` and UTF-8 cache (audit row VM-8).
- Evidence (commands in generic form: `<jsc> --useDollarVM=1 bench.js -- N` with a loop of `add(s & 0xffff, i & 0xff)`
  over a two-int32 fixture, two values of N):

  | instructions per FFI call | Baseline only (`--useDFGJIT=0`) | all tiers |
  |---|---|---|
  | `main` / flag off | 115 (402 with `--useFFIICStub=0`) | 19 |
  | GIL on | 118 | 90 (see H1b) |
  | GIL off | 541 | 496 |

  GIL off `--dumpOptions=2` shows the three options false; `$vm.ffiCompileCounts()` stays `{0,0,0}`. A spawned thread
  calling or creating an FFI function gets `TypeError: bun:ffi is not available on spawned threads when the GIL is
  off` (checked GIL off; GIL on both succeed).
- Worth: 16 + 1 test results; FFI call cost GIL off 4.6x (Baseline) to 26x (FTL) what flag off pays.

#### H1b. Flag on, the FTL's strength reduction never plants `CallFFI` (nor `CallWasm`, nor its own `DirectCall` conversions) (explained)

- Configurations: GIL on and GIL off (hidden GIL off behind H1).
- Mechanism: the `Call`/`Construct`/`TailCall` case of `DFGStrengthReductionPhase` starts with upstream's
  `if (m_graph.m_plan.isFTL()) { if (Options::useHandlerICInFTL()) break; }`. The break exists to keep the FTL from
  making `DirectCall`s (code-patching call linking) when handler ICs are in use. With the flag on
  `useHandlerICInFTL` is implied true (SPEC-jit 5.2), so the FTL leaves the whole case - and the `CallFFI` and
  `CallWasm` conversions sit below the break in the same case although neither patches code.
- Evidence: `--reportCompileTimes=1` GIL on shows `run` compiled by FTLForOSREntry and FTL (1,088 bytes against 352
  flag off); `--dumpGraphAfterParsing=1` shows the same `CheckIsConstant` + `Call` in both modes;
  `$vm.ffiCompileCounts()` ends `{icStub 1, dfgCallFFI 1, ftlCallFFI 0}` GIL on against `{1, 1, 2}` flag off and on
  `main`; `--dumpOptions=2` shows `useHandlerICInFTL=true` with the flag, false without. 90 against 19 instructions
  per call.
- Worth: a 4.7x FFI call in FTL code with the flag on; and the FTL's strength-reduction `DirectCall` conversions
  (a callee that became constant after parsing). The parser's own `DirectCall` conversion is not affected - FTL graphs
  do contain `DirectCall` nodes flag on, lowered as data ICs - so this is narrower than "no `DirectCall` in the FTL";
  section M (M-I3) has the counts (richards 4 -> 2, delta-blue 17 -> 7 nodes) and the cost where it bites (+27.7
  instructions per call).

#### H1c. The "FFI executable-memory runs" that fail on `main` too (explained)

- 9-11 suite results per run in every configuration including `main`, different files each time.
- Mechanism: the runner's `ftl-eager-no-cjit` configuration includes `EXECUTABLE_FUZZER_OPTIONS`
  (`--useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzRandomly=true`): every executable allocation
  made with `JITCompilationCanFail` fails with probability 0.1 (`doExecutableAllocationFuzzing`, unseeded
  `WeakRandom`). The FFI's three generators (invoke thunk, IC stub, callback thunk) all link with
  `JITCompilationCanFail`; a failed invoke thunk makes the call throw `OutOfMemoryError: bun:ffi failed to allocate
  executable memory for the invoke thunk`, a failed IC stub silently leaves the function without a stub. The FFI tests
  assert success.
- Evidence: `main` with those two options, ten runs each: `ffi-callffi-was-compiled.js` 8 of 10 fail ("expected one
  IC stub per JSFFIFunction: before 0, after 3/4" - five functions, 1 - 0.9^5 = 41 % for the stubs alone plus the
  invoke thunks), `ffi-types-echo.js` 2 of 10, `ffi-arity.js` 2 of 10. Not a threads matter.
- Worth: removes 9-11 results of noise from every suite comparison.

#### H1d. `bun:ffi` on GIL-off spawned threads (refused by ruling; what it would need is listed under Designs)

#### H2. `--useProfiler` is refused GIL off (explained)

- Configurations: GIL off; 2 suite results (`tail-call-profiler.js`, `op-push-name-scope-crashes-profiler.js`, whose
  `runProfiler` lane runs `jsc --useConcurrentJIT=false -p <file>` and post-processes the JSON).
- Mechanism: `useProfiler` clears `useConcurrentJIT` on every tree (`Options.cpp`; `ensureOptionsAreCoherent` crashes
  on "Bytecode profiler is not concurrent JIT safe"). GIL off the concurrent JIT cannot be turned off, so the block
  that turns a synchronous-JIT request into the derived wait option (SPEC-jit history 56) prints `FATAL: useProfiler
  is not supported with useJSThreads and the GIL off.` and crashes. Reproduced: `-p out.json` GIL off prints exactly
  that.
- What is actually unserialized (reading `profiler/*` on the branch head):
  - `Profiler::Database`: its maps, `m_compilations`, `m_events` and `m_bytecodes` appends are already under
    `Database::m_lock` (`ensureBytecodesFor`, `addCompilation`, `logEvent`, `notifyDestruction`). `toJSON()`/`save()`
    read all of them without the lock.
  - `Profiler::Compilation` is `RefCounted` (not thread-safe) and is referenced by the plan (created on the mutator in
    `DFG::Plan`'s constructor or in `JIT::compileAndLinkWithoutFinalizing`), by the compiler thread, by
    `CommonData::compilation`, and by the database; it dies wherever the last of them dies (a sweeping thread
    included). Its members are written by one plan until `addCompilation`, then by `addOSRExit` from the lazy exit
    compilers (`DFGOSRExit.cpp`, `FTLOSRExitCompiler.cpp`) and by `setJettisonReason`.
  - GIL off both exit compilers already run under the process-wide `OSRExitGenerationLocker`, and jettison runs
    world-stopped (I8), so those two writers are serialized already.
  - Emitted counters (`CountExecution` in DFG/FTL, the Baseline per-bytecode counters, the exit ramps' `add64` on
    `OSRExit::counterAddress()`) are plain 64-bit read-modify-writes on 8-byte aligned words.
- Worth: 2 results; the feature (a JSON per-bytecode profile) for GIL-off processes.

#### H3. Spawned threads are not sampled GIL off (explained; by ruling SD18)

- `SamplingProfiler` keeps ONE sampled thread (`m_jscExecutionThread`, and GIL off its lite
  `m_jscExecutionThreadLite`). `noticeCurrentThreadAsJSCExecutionThreadWithLock` returns early for a spawned thread
  GIL off (`shouldBindCurrentThreadAsJSCExecutionThread`), so a program whose work runs on Threads gets a profile of
  the main thread waiting in `join`. With the GIL on the binding follows the API lock's owner, so every thread is
  sampled while it runs.
- The pieces a wider sampler needs exist: every thread that enters the VM runs
  `VM::executeEntryScopeServicesOnEntry` -> `noticeVMEntry` on itself; an exiting Thread calls
  `noticeCurrentThreadIsExiting`; `takeSample` already resolves a lite through the registry under `tryLock`, reads
  `entryScope`, `topCallFrame`, `topEntryFrame`, `executingRegExp` from it, and takes every lock it needs before it
  suspends the target; the report entry points run as the conductor of a stop GIL off.
- Worth: no test result today (no test expects spawned frames); a profile that is wrong for exactly the programs the
  feature is for.

#### H4. `new Thread` in a second VM of a GIL-off process throws the cap's RangeError (explained)

- `ThreadManager::allocateSpawnedThreadState` returns null first thing when `VM::isGILOffProcess() && !vm.gilOff()`
  (U0b: only the VM that won the constructor's designation CAS holds per-thread heap clients), and `constructThread`
  (`ThreadObject.cpp`) maps every null to `RangeError: too many live Threads (or thread-ID space exhausted)`.
- Reproduced in the shell with a second VM (`$262.agent.start`): GIL off the agent's `new Thread` throws that text,
  the main VM spawns; GIL on both spawn.
- Worth: a misleading message in every Worker of a GIL-off Bun process; the behaviour difference itself (a Worker
  cannot spawn) stays until the shared heap serves several VMs.

#### H5. Module evaluation is reachable from spawned threads and has no claim (explained; three observable failures measured, one memory-safety hazard by reading)

- Configurations: GIL off. No suite result (no test does it); behaviour and safety.
- What runs where. `JSModuleLoader::importModule` refuses a spawned thread GIL off, so LOADING and LINKING stay on
  carriers. EVALUATION does not:
  1. a deferred namespace (`import defer * as ns`) evaluates on whichever thread first reads a property
     (`JSModuleNamespaceObject::ensureDeferredNamespaceEvaluation` -> `AbstractModuleRecord::evaluateSync` ->
     `CyclicModuleRecord::evaluate` -> `innerModuleEvaluation`);
  2. a top-level-await continuation runs on the thread that settles the awaited promise (reactions run on the
     settler in both flag-on modes), and its completion runs `CyclicModuleRecord::asyncExecutionFulfilled` /
     `asyncExecutionRejected` there, which execute the bodies of the async PARENT modules on that thread. Measured in
     both modes: a child module awaiting a promise that a Thread resolves prints "child after await on thread id 1"
     and its importer's body prints "parent body on thread id 1".
- None of AUD1.K3's three rulings is implemented in the C++ loader that came with the tenth round's base:
  `CyclicModuleRecord::setStatus` is a plain store, `VM::incrementModuleAsyncEvaluationCount` is `m_count++` on a
  plain `int64_t`, `VM::m_synchronousModuleQueue` is a VM member.
- Failure modes GIL off, four threads released together onto 200 deferred modules (each module: a counter increment
  and one export), three runs:
  - `TypeError: Unable to synchronously evaluate deferred module` 121-139 times per thread (a loser's
    `readyForSyncExecution` sees the winner's `Evaluating`);
  - `ReferenceError: Cannot access 'x' before initialization.` 4-11 times per thread (a loser passes the readiness
    walk while the status is still `Linked`, then meets `Evaluating` in `innerModuleEvaluation` step 3, takes it for
    a cycle edge of its own walk, returns without evaluating, and reads the binding);
  - 12-17 of the 200 modules evaluated TWICE (both saw `Linked`);
  - Debug build: `ASSERTION FAILED: module->status() == Status::EvaluatingAsync || module->status() ==
    Status::Evaluated` in `CyclicModuleRecord::evaluate` (step 10.a).
  GIL on the same program (with a blocking barrier) gives four correct reads and one evaluation per module: the GIL
  is not handed over inside an evaluation unless the body blocks.
- By reading, not reproduced: `gatherAvailableAncestors` and `asyncExecutionRejected` iterate
  `record->asyncParentModules()` (a `Vector`) without the cell lock, while `appendAsyncParentModule` appends under
  it from `innerModuleEvaluation` step 12.b.v. A spawned thread fulfilling a child (path 2) while a carrier walks an
  overlapping graph (a dynamic `import()` of another importer of the child) is an append racing an iteration: a
  reallocated buffer under the reader, or a parent appended after the reader passed, which is then never notified
  (its evaluation promise never settles). The DFS bookkeeping (`dfsAncestorIndex`, `pendingAsyncDependencies`,
  `cycleRoot`, `asyncEvaluationOrder`) is likewise written by both walks.
- Also: `CyclicModuleRecord::evaluate`'s failure path stores the status before the evaluation error (steps 9.a.ii,
  9.a.iii), so a lock-free reader of the deferred-namespace fast path can see `Evaluated` with no error yet.

### H: Designs

#### D-H1. FFI: the carrier gets its stub and its `CallFFI` back GIL off

**Status: proposed.**

Rule. GIL off, `useFFIICStub`, `useFFICallInDFG` and `useFFIDirectCall` keep their defaults. Every FFI fast path
(a) uses the per-thread words and (b) starts with the carrier gate; a thread that fails the gate takes the path that
ends in `ffiHostCall`'s refusal. Spawned threads stay refused; nothing about the `FFIContext` changes.

- The carrier gate (generated code): `loadVMLite(t); load16(t + VMLite::offsetOfTID()) -> t; branch32(Below, t,
  ThreadManager::carrierTIDBase) -> refuse`. GIL off every carrier lite, the main thread's included, carries a TID
  from `[carrierTIDBase, notTTLTID)` (`ensureCarrierLiteForCurrentThread` asserts `isCarrierTID`), spawned lites carry
  `[1, carrierTIDBase)`; `~VM`'s walk already tells the two apart by that range. The TID is written before the lite
  is installed and is immutable while installed, and the reader is the owning thread: no ordering question.
- Who writes / who reads, per tier:
  - IC stub (`generateICStubCode`, mode fixed at generation by `vm.gilOff()`, immutable per VM): prologue publishes
    the frame with `emitPublishTopCallFrameForHostCall(vm)`; then the gate, appended to the stub's existing
    `slowPath` list (the slow path calls `operationFFICallSlowPath` -> `ffiCall`, which throws the TypeError); the
    exception handler takes `nativeForGenerator`'s `gilOff` arm (lite `topEntryFrame` for the callee-save copy, lite
    `topCallFrame`). Scratch: the helpers use only the macro-assembler temp (r11 / ip1), which the stub's register
    set (rax/rdx/rcx/r8/r10; x0/x2/x3/x4/x8) does not include.
  - DFG `compileCallFFI`: gate first (before `operationFFIArenaEnter`); on failure a slow path generator calls a new
    `operationThrowFFIRefusedOnSpawnedThread(globalObject)` and takes `exceptionCheck()`.
    `emitPublishTopCallFrameForHostCall(vm())` replaces the store; `loadException(vm(), returnValueGPR)` replaces the
    raw load.
  - FTL `compileCallFFIImpl`: the same gate as an unlikely branch to a block that `vmCall`s the throwing operation
    (the pattern of the node's existing allocation-failure arm); `emitPublishTopCallFrame()` at the three sites,
    `loadCurrentThreadException()` at the two.
  - LLInt and Baseline call the function through its executable's entry, which is the stub; the generic native
    thunk behind stub-less functions is already mode-keyed. C++: `ffiHostCall` unchanged.
  - `Options.cpp`: delete the three force-offs. `throwIfFFIRefusedOnCurrentThread` stays the single C++ refusal.
- Memory ordering: every word touched is the executing thread's own lite (x86-64 and arm64 alike: program order).
  The thunks are immutable after `LinkBuffer` finalization; the invoke thunk is published per signature with a
  release store (`m_publishedInvokeThunk`) and read with acquire; carriers exclude each other with the API lock, a
  full barrier. On arm64 a second carrier thread executing a thunk the first generated relies on `LinkBuffer`'s
  broadcast instruction-cache invalidation plus its own context synchronization at the next exception return; this
  is the recorded "first fetcher of a thunk" class (AUDIT, arm64-only, theoretical), not new.
- Collector and stops: an FFI call is a host call made with heap access held, as on `main`; a stop waits for it to
  return as it waits for any host function. `FFIContext::didGarbageCollect` (arena shrink) runs from
  `Heap::didFinishCollection` with the world stopped and only touches an idle arena (`m_depth == 0`), so a carrier
  parked between `enter` and `exit` is safe. `JSFFIFunction` cells are swept by whichever thread sweeps; the stub's
  `JITCode` is thread-safe reference counted and freed through the executable allocator's lock. Watchpoints and
  deferred claims: none involved (`CallFFI` is guarded by `CheckIsConstant` on the callee cell).
- What a second thread observes: a spawned thread that calls an FFI function, from any tier, gets the same TypeError
  as today. It can observe nothing of the carrier's call in progress.
- Flag off: no emitted byte changes (every new emission is under `vm.gilOff()`); `Options.cpp` loses three stores in
  a block that never ran flag off. GIL on: unchanged.
- Failure modes and detection: a missed VM-word site shows as a lost exception (Debug: the frame tracer assertion
  named in `prepareCallOperation`'s comment) - the FFI stress files (`ffi-osr-and-exceptions`,
  `ffi-callback-throw-unwind`, `ffi-conversion-errors`) cover each throw path and currently run through the host path
  only GIL off; a missing gate shows under TSan as a race on `StringArena` from a test that calls from two threads.
- Tests: `api/ffi-fast-paths-carrier-only-gil-off.js` (`$vm.ffiFunction`): counts `icStub` after creation (0 before,
  1 after), tiers a caller to DFG and FTL on the main thread (`dfgCallFFI`/`ftlCallFFI` 0 before), then calls the
  same optimized function from a Thread and expects the TypeError from each tier, then again from the main thread
  (still correct). The 16 suite results and the Bun fixture are the acceptance count. The FFI stress directory run
  once GIL off on Release and Debug (it has never exercised the fast paths there).
- Expected gain: 541 -> about 125 instructions per call in Baseline (115 + gate 3 + two TLS loads), 496 -> about 25
  in FTL code once D-H1b lands (19 + gate + TLS); 17 test results.
- Risks: small - nine emission sites, helpers proven elsewhere. The one new idea is the TID-range gate.
- Alternatives rejected: (a) embedding the carrier's lite in the VM so the VM-level words are the carrier's (LANDING-
  PLAN): helps one carrier thread only, leaves shared DFG/FTL code needing the gate anyway, and moves every
  VM-relative constant flag off; nine mode splits are cheaper. (b) An OSR exit instead of a throw at the gate: a
  spawned caller would exit on every call until the site's exit profile turns the conversion off for the carrier
  too. (c) Publishing through the call-record protocol (SPEC-jit 5.8): not applicable, nothing is patched or
  republished.

**What `bun:ffi` on spawned threads would need (not proposed; the refusal is a standing decision).** Per-thread (in
the lite) or locked `StringArena` and UTF-8 cache - the arena's pointers are handed to native code for the duration
of the call, so a lock would have to be held across the native call, which means per-thread; a per-thread return
buffer for `JSFFICallback::setReturnCString` (its pointer is the native return value); `m_liveCallbacks` under a
lock; a rule for which thread a non-threadsafe callback may be invoked on (today: whoever holds the API lock) and the
threadsafe dispatch routed to the owning thread's queue; heap-access release around long native calls made from
spawned threads (SPEC-ungil embedder contract (c)); Bun's side: the TinyCC `cc()` path serialized (one compiler
state) and its trampolines' publication, and `dlopen` handle lifetime against a closing thread.

#### D-H1b. The FTL's handler-IC early break guards `DirectCall` only

**Status: proposed.** In `DFGStrengthReductionPhase`'s call case, move the `isFTL() && useHandlerICInFTL()` break
from the top of the case to just before the `DirectCall` conversion, below the `CallWasm` and `CallFFI` conversions
(neither patches code; `CallWasm` stays refused flag on by its own `useJSThreads` test until the WebAssembly item
lifts it). Flag off `useHandlerICInFTL` is false and the break is never taken: no change. Expected: FFI call in FTL
code 90 -> about 20 instructions GIL on. Test: `jit/ftl-plants-callffi-with-handler-ics.js` (`ftlCallFFI` 0 before,
at least 1 after, with `--useJSThreads=1`). Risk: none beyond the node's own FTL lowering, which flag off runs today.

#### D-H1c. The FFI tests under the executable-allocation fuzzer

**Status: proposed (test-side).** The FFI tests tolerate a refused allocation the way the fuzzer's other victims do:
wrap creation in a helper that, when `$vm` reports the fuzzer on (or the error is the exact out-of-memory message),
retries a bounded number of times, and `ffi-callffi-was-compiled.js` compares `icStub` against the number of
functions that report `hasICStub`. Alternative (engine-side): the three FFI generators retry once under
`useExecutableAllocationFuzz`. Either removes 9-11 results of noise from every comparison in all configurations.

#### D-H2. The bytecode profiler GIL off

**Status: proposed.**

Rule. `useProfiler` GIL off is a synchronous-JIT request like `forceEagerCompilation`: it sets
`useJSThreadsWaitForJITPlans` and leaves the concurrent JIT on (SPEC-jit 5.7.3, history 56); the FATAL arm and the
coherence check's crash admit that combination. The database tolerates N mutators and compiler threads:

1. `Profiler::Compilation` becomes `ThreadSafeRefCounted` (allocated only when the profiler is on, so no
   configuration without the profiler sees an atomic).
2. `Database::toJSON()`/`save()` take `m_lock` for the walk over `m_bytecodes`, `m_compilations`, `m_events`
   (`logEvent` and `addCompilation` already do). `Compilation::toJSON` reads counters with plain loads.
3. `Compilation::addOSRExit` needs nothing new GIL off: both exit compilers call it under
   `OSRExitGenerationLocker`. `setJettisonReason` runs world-stopped. State both as the rule in a comment at the two
   call sites and assert them (`JSThreadsSafepoint::worldIsStopped` / the generation lock held).
4. Emitted counters stay plain adds: aligned 64-bit words, never torn; two threads in the same code can lose an
   increment. This is SPEC-jit 5.7 item 1 ("JIT'd fast-path adds may stay plain") applied to the profiler, and the
   output says so (a `"countsAreApproximate": true` key when more than one Thread ever ran).
5. Everything the compiler thread does for the profiler (`Bytecodes` construction, which dumps the code block's
   bytecode; `OriginStack`; `addDescription`) runs under the database lock or on plan-private data; it reads the
   code block the way `--dumpBytecodeAtDFGTime` already does from compiler threads.

- What "synchronous" means with it: the tier-up call returns with the code installed (the parked wait of history
  56), so a profile taken by a single-threaded program lists the same compilations in the same order as `main`'s.
- Ordering: all shared structure is under `WTF::Lock`; nothing relaxed. arm64: nothing to add.
- Collector/stops: `Database::m_lock` is a leaf taken from mutators, compiler threads and `CodeBlock`'s destructor
  (`notifyDestruction`, possibly on a sweeping thread); nothing parks under it. `save()` at exit runs with other
  Threads possibly alive; the lock makes the walk consistent.
- Flag off / GIL on: `Compilation`'s counter type is the only unconditional change; visible only with
  `--useProfiler`.
- Failure modes: a reference-count race on `Compilation` (TSan, the two profiler tests GIL off with a spawned thread
  compiling); a torn JSON (the display script fails to parse).
- Tests: the two `runProfiler` results; `vmstate/bytecode-profiler-with-threads-gil-off.js` (two Threads tier up
  distinct functions under `--useProfiler=1`, the saved profile parses and lists both).
- Gain: 2 results. Risk: low.
- Alternative rejected: per-thread databases merged at save - the consumer (`display-profiler-output`) keys
  compilations by code block, which are shared.

#### D-H3. Sampling every running thread GIL off

**Status: proposed.** Supersedes SD18's "spawned unsampled" for v2; A.1.7's SUSPEND RULE stands.

Rule. GIL off the profiler keeps a set of bindings `{Ref<Thread>, VMLite*, tid}` instead of one. Every thread binds
itself in `noticeVMEntry` / `noticeJSLockAcquisition` (the early return for spawned threads goes away GIL off; GIL on
is unchanged: one binding that follows the API lock) and unbinds in `noticeCurrentThreadIsExiting`. One tick:

1. under `m_lock`, `tryLock` the lite registry (as today: a contended registry costs the tick);
2. drop every binding whose lite is no longer registered to this VM (carrier lites vanish without a notice);
3. take the locks a walk needs once (machine threads, code block set, executable allocator, native callees), then
   for each binding whose lite has an entry record: suspend, read registers and the lite's four words, walk into the
   preallocated frame buffer, resume; after the resume copy the frames into `m_unprocessedStackTraces` with the
   binding's `tid`;
4. release the registry.

One thread is suspended at a time; nothing is allocated and no lock is taken while one is (the buffer is sized
before the loop and grown between threads). A registered lite pins its thread: unregistration takes the registry
lock, which the sampler holds across the suspend/resume, so the target cannot finish exiting under it (the tenth
round's bound-thread-exits hang cannot recur for a bound lite).

- Trace format: `UnprocessedStackTrace` and `StackTrace` gain the thread's TID (0 for the main carrier);
  `stackTracesAsJSON` adds `"threadId"` per trace; `reportTopFunctions`/`reportTopBytecodes` aggregate over all
  threads and print a per-thread sample count line; the inspector's trace payload gains an optional field.
- Cost: one suspend/resume pair per sampled thread per tick (tens of microseconds each on Linux). A budget keeps
  it bounded: at most `samplingProfilerMaxThreadsPerTick` (default 8) per tick, rotating the starting binding; each
  trace records how many ticks its thread was skipped so a consumer can weight it. A thread parked without heap
  access (in `join`, a lock wait, `Atomics.wait`) can be woken at any moment, so it is suspended like the others
  when it is sampled; whether parked threads are sampled at all is the decision below (wall-clock or running-only).
- Ordering: the target is suspended while its lite and stack are read (the existing argument). The `tid` is
  immutable while the lite is installed.
- Collector and stops: the sampler thread is not a mutator. Suspending a thread that is the conductor of a stop or
  is parked in one delays that stop by one walk. The report entry points already run as the conductor of a stop and
  defer traps GIL on; unchanged. Processing unverified traces still needs the heap iteration inside a stop.
- Failure modes: a suspended thread holding a lock the sampler needs - excluded by taking all of them first, as
  today, and by holding them across the whole loop (no acquisition between two suspensions); the registry lock held
  longer than today (N walks): spawn and exit wait that long (sub-millisecond); a Thread that exits between steps 2
  and 3 - excluded by the registry lock.
- Flag off / GIL on: one predicted-false mode test in `takeSample` and the notice hooks, as today.
- Tests: `vmstate/sampling-profiler-samples-spawned-threads-gil-off.js` (two Threads spin in two distinctly named
  functions while the main thread joins; the traces contain both names under two thread IDs: 0 spawned frames
  before); the sixteen `sampling-profiler-*` stress files and the two-readers test unchanged; the mirror harness over
  those files (it found the lock rule last round).
- Gain: correct profiles for threaded programs; no suite count. Risk: medium (signals against many threads; the
  amplifier on the new test, 500 runs, both modes).
- Alternatives rejected: per-thread timers delivering a signal to each thread (async-signal-safe walking in the
  target: the walker takes locks); sampling at polls only (no samples in native code or poll-free loops).

#### D-H4. The message, and what lifting U0b would take

**Status: proposed (message only).** `constructThread` tests `VM::isGILOffProcess() && !vm.gilOff()` before
`allocateSpawnedThreadState` and throws `RangeError: Thread cannot be created in this VM: with the GIL off only the
first VM of the process can run Threads` (RangeError kept: SPEC-api 5.1's shape for a refused spawn). The cap and
TID-exhaustion message stays for the two cases it names. Test: `api/thread-spawn-refused-in-second-vm-gil-off.js`
(`$262.agent`; the message names the rule; the main VM still spawns). Flag off and GIL on never take the branch.

What lifting the rule would take (a list, not a design). Single-VM assumptions in the tree today:
- the shared heap server is process-unique: `Heap::tryDesignateStickySharedServer`'s process-wide CAS and the I13
  "one sticky shared server" assertion; `VM::m_gilOff` is decided once by that CAS;
- `ThreadManager` is a process singleton whose TID space, live-thread map, rebias pipeline (run inside the winner
  heap's next Full collection: `Heap::shouldDoFullCollection`, `conductSharedCollection`'s `vm().gilOff()` assertion)
  and `s_liveSpawnedThreadCount` assume one spawning VM;
- process-wide counters that gate protocols of "the" GIL-off VM: the deferred-claims-in-flight count
  (`Watchpoint.cpp`), the conductor heap-fact rewrite epoch and the stop request count (`JSThreadsSafepoint.cpp`),
  the megamorphic-cache epoch, `ArrayAllocationProfile`'s substituted-request table, `g_jscAnyJSThreadEverSpawned`;
- `JSThreadsSafepoint::stopTheWorldAndRun(vm, ...)` stops the clients of that VM's heap: two GIL-off VMs would need
  either one heap (then one structure ID space, one atom table - already shared - and cross-VM object references) or
  two independent stop domains with every process-wide counter above made per-domain;
- the lite's level-2 `gilOff` byte and the per-VM mode of generated code are already per VM and would carry over;
- the embedder contract's "native code on a spawned Thread never enters another VM" would have to stay.
A Worker that needs Threads in a GIL-off process is therefore "the multi-VM shared heap", not an increment.

#### D-H5. Module evaluation is serialized by one lock with the GIL's hand-over points

**Status: proposed.** Replaces AUD1.K3(c) (per-record claim), keeps (a) and (b) moot.

Why not the per-record claim. Evaluate / InnerModuleEvaluation / AsyncModuleExecutionFulfilled / Rejected are one
graph algorithm (a Tarjan walk with a stack, per-record DFS indices, SCC roots, pending counts, parent lists) that
the specification runs under "no other Evaluate in this agent at the same time". Two walkers over overlapping graphs
do not compose record by record: walker B meeting a record that is `Evaluating` on walker A's stack can neither treat
it as its own cycle edge (the ReferenceError above) nor wait for it (it may belong to an SCC that contains a record
on B's stack). What the GIL gives GIL on is exactly the needed exclusion: one evaluator at a time, handed over only
where the evaluating body blocks.

Rule. A GIL-off VM has one module-evaluation lock M: owner thread, recursion depth, park-capable waiters.
- Taken (RAII, recursive for the owner) at the entry of: `AbstractModuleRecord::evaluate` (the dispatcher in front
  of `CyclicModuleRecord::evaluate` and of the synthetic and WebAssembly records' evaluation),
  `AbstractModuleRecord::evaluateSync` (so the readiness walk and the evaluation are one critical section),
  `CyclicModuleRecord::asyncExecutionFulfilled` and `asyncExecutionRejected` (reached from the
  `AsyncModuleExecutionDone` microtask), and `CyclicModuleRecord::link` (it reads and writes the same status byte).
  Module bodies that the walk executes synchronously run under M; an async body's continuations after its first
  `await` are ordinary reactions and do not.
- Acquisition: one compare-and-swap when free or already mine; otherwise release heap access, park in bounded quanta
  polling this thread's termination request, re-acquire access through the gated path (the code-deletion wait's
  shape). Never requested while a cell lock, the registry lock or a structure lock is held (all entries are host-call
  or microtask entries: lock-free points). A carrier may hold the API lock while it waits: spawned threads never take
  the API lock GIL off, so the holder of M cannot need it.
- Hand-over: `GILDroppedSection` - the one bracket all 26 blocking sites use (`join`, `Lock.hold`, `Condition.wait`,
  `Atomics.wait`, ...) - releases M entirely (saving the depth) if the current thread owns it, in both its spawned arm
  and its carrier arm, and takes it back, by the ordinary acquisition, in its destructor. So a module whose body
  blocks shows `Evaluating` to another thread exactly where it does GIL on, and nowhere else.
- Readers outside M: `m_status` becomes a relaxed-atomic byte stored with release and the deferred-namespace fast
  path (`ensureDeferredNamespaceEvaluation`: root `Evaluated`, no error) loads it with acquire, so a thread that
  takes the fast path sees every binding the body initialized. `evaluate`'s failure path stores the evaluation error
  BEFORE the status. `gatherAvailableAncestors`' and `asyncExecutionRejected`'s unlocked walks of
  `asyncParentModules()` become safe because every appender runs under M too; the collector keeps reading the vector
  under the cell lock, against appends under the cell lock.
- `VM::incrementModuleAsyncEvaluationCount` is only called under M (innerModuleEvaluation step 12.b): the plain
  counter is sound; K3(a)'s atomic is unnecessary. `m_synchronousModuleQueue` is touched by loading only, which is
  carrier-only and serialized by the API lock: K3(b)'s per-lite queue is unnecessary while `import()` stays refused
  on spawned threads.
- x86-64: the release store and acquire load are plain moves. arm64: `stlrb` / `ldarb` on the status byte; M's own
  acquire/release come from `WTF::Lock`-style atomics.
- Collector and stops: a holder of M keeps heap access and parks at polls like any mutator, holding M; the conductor
  of a stop or a collection never needs M, so no cycle. Waiters hold no heap access. Watchpoints/deferred claims:
  none. Module environments and namespaces keep their own publication rules (R9-15's compare-and-swap).
- What a second thread observes: a record is `Evaluating` only while its evaluator is parked in a blocking
  primitive or is the observer itself (re-entrancy: `require(esm)`-style nested evaluation and the fork's
  `depInOuterSCC` arm are same-thread and keep working); otherwise a contender waits and then finds `Evaluated` /
  `EvaluatingAsync`. No double evaluation, no TDZ read, no TypeError that GIL on does not also give.
- Flag off and GIL on: one predicted-false `vm.gilOff()` test at five entries that run once per module graph
  operation; the two reordered stores in `evaluate`'s failure path are adjacent plain stores flag off.
- Failure modes: a body that spins on a plain flag another thread sets only after evaluating a module deadlocks -
  GIL on it deadlocks the same way (the spinner never hands over); a missed entry point shows as the Debug assertion
  above under the race test; a blocking site that does not use `GILDroppedSection` would keep M while parked (audit:
  grep the 26 sites; assert in the park primitives that the current thread does not own M).
- Tests: `api/module-evaluation-race/` (a module test: 200 deferred modules, four Threads released by an
  `Atomics.wait` barrier so that GIL on passes too; counts TypeErrors, ReferenceErrors and modules whose counter
  exceeds 1: 121-139 / 4-11 / 12-17 before, 0 / 0 / 0 after; Debug: the step-10.a assertion before);
  `api/module-tla-parent-notified-gil-off/` (a Thread fulfils a child's awaited promise while the main thread
  `import()`s a second importer of the child, in a loop over fresh graphs; every import settles; TSan lane for the
  vector); `api/module-body-blocks-in-join/` (a deferred module whose body joins a Thread that reads the same
  namespace: the TypeError, in both modes).
- Gain: closes three measured behaviour differences and one memory-safety hazard; no suite count.
- Risk: medium-low - one lock, five entries, one hook in an existing bracket. The subtle part is the bracket hook's
  order against heap access, which the ordinary acquisition path settles (M is always acquired access-released when
  contended).
- Alternatives rejected: (1) per-record claim under the cell lock with losers waiting (K3(c)): does not compose for
  overlapping walks, above. (2) Refusing evaluation off the carrier (deferred namespace read on a spawned thread
  throws; module continuations carrier-queued): a larger difference from GIL on than today's (where a spawned thread
  does evaluate), needs a wake-up of an idle carrier event loop the engine does not own, and leaves
  `asyncExecutionFulfilled` racing the carrier's own walks unless continuations are rerouted too. (3) Running
  evaluation inside a stop: module bodies are arbitrary JS.

### H: arm64 / non-Linux notes

- FFI (D-H1): all new reads are owner-thread reads of the thread's own lite (program order on both
  architectures). `loadVMLite` exists for Linux x86-64 and Linux arm64 only (ELF initial-exec TLS); elsewhere GIL
  off is refused before any of this is reached, so the FFI arms need no other platform. On arm64 `loadVMLite` writes
  the macro-assembler temp through the cache-invalidating accessor; the stub's fixed registers avoid x16/x17. A thunk
  generated by one carrier and first executed by another relies on the generator's broadcast icache invalidation and
  the executor's next context-synchronizing event: the recorded arm64-only "first fetcher" residual, shared with
  every thunk.
- Profiler counters (D-H2): `add64` to an absolute address is a load/add/store on arm64 as on x86-64 without `lock`:
  lost increments only; words are 8-byte aligned, never torn.
- Sampling (D-H3): suspension is signal-based on Linux (both architectures) and Mach-based on Darwin; the frame walk
  is the existing one. No relaxed cross-thread read is added: the lite is read with its thread suspended.
- Module status (D-H5): the only fence-published fact. Writer: bodies' stores, then status with release; reader:
  status with acquire, then bindings. x86-64 needs no instruction; arm64 needs `stlrb`/`ldarb` (or `dmb ishst` /
  `dmb ishld` around plain accesses). The same pairing covers the evaluation error stored before the status.
- U0b (D-H4): nothing.

### H: Decisions for the user

1. **FFI for the carrier GIL off (D-H1).** Option A (recommended): re-enable with the TID gate - 17 results, FFI
   calls 4-25x cheaper GIL off, nine emission sites. Option B: leave off; costs nothing, keeps the differences.
2. **Narrow the FTL's handler-IC break (D-H1b).** Recommended: yes, for `CallFFI` now (and it is the place
   `CallWasm` will need once WebAssembly calls are admitted); it changes flag-on FTL code only.
3. **FFI tests against the allocation fuzzer (D-H1c).** Test-side tolerance (recommended; no engine change) or an
   engine-side retry; either way the 9-11 noisy results go in every configuration, `main` included - upstreamable to
   the fork independently of threads.
4. **Sampling semantics (D-H3).** (a) wall-clock: every entered thread every tick, parked ones too (what `main` does
   for its one thread; cost grows with parked threads); (b) running-only: threads parked without heap access are
   skipped (cheaper, a CPU profile; the main thread blocked in `join` disappears from its own profile);
   (c) recommended: (a) with the per-tick budget and rotation, plus the thread ID in every trace so consumers can
   filter. The trace format gains a field in Bun's `profile()` output either way.
5. **U0b message (D-H4).** Keep RangeError with the new text (recommended: SPEC-api's refused-spawn shape) or switch
   to TypeError.
6. **Module evaluation (D-H5).** The evaluation lock with the GIL's hand-over points (recommended: GIL-on parity by
   construction, smallest change) against refusing evaluation off the carrier (simpler to state, larger difference
   from GIL on, needs continuation rerouting) against the per-record claim (does not compose).

### H: Doc mismatches

- LANDING-PLAN, Open items, "bun:ffi on GIL-off spawned threads": "It would need the FFI function's IC to publish
  through the call-record protocol (SPEC-jit 5.8) instead of patching, TinyCC compilation and trampoline
  publication". The engine's FFI never patches: the stub is an immutable per-function thunk installed as the native
  executable's entry, and `CallFFI` is a compile-time conversion. TinyCC is the embedder's `cc()` path, outside this
  tree. Replace with the list under D-H1.
- LANDING-PLAN / Options.cpp comment: "the IC stub ... read[s] the VM-level exception word". The stub's exception
  checks are mode-keyed through `emitExceptionCheck` on the branch head; what remains VM-level in the stub is
  `topCallFrame` (twice) and `topEntryFrame`. The raw exception reads left are one in the DFG node and two in the FTL
  node.
- AUDIT-upstream-since-rebase, the closing row for VM-8/OPT-1/OPT-2: "a CAS publish of the context ... is not done":
  `JSGlobalObject::ffiContext()` publishes with a compare-and-swap on the branch head. OPT-1/OPT-2's line numbers are
  stale; the site list above is current.
- LANDING-PLAN (every round's suite paragraph): the "FFI executable-memory runs" are described as flakes that fail
  on `main` too, without a cause. Cause: `EXECUTABLE_FUZZER_OPTIONS` in `ftl-eager-no-cjit` (H1c).
- SPEC-ungil-audit-K4 rows K4.III.16 ("`m_moduleAsyncEvaluationCount` ... atomic fetch_add, relaxed") and K4.V.18,
  and SPEC-ungil-history AUD1.K3(a)/(b)/(c): written against the loader before the tenth round's base. On the branch
  head the counter is a plain `int64_t` incremented with `++`, the synchronous queue is a VM member, and no status
  claim exists; line references (`VM.h:1332`, `:1358`) no longer match. Mark all three "not implemented in the C++
  loader" and point to D-H5.
- AUDIT R10-12: "`JSModuleRecord::evaluate` fills the slots before the body runs" and the closing rule "only the main
  thread loads modules GIL off" read as if evaluation were main-thread too. Evaluation reaches spawned threads
  through deferred namespaces and top-level-await continuations (measured, both flag-on modes); the rule covers
  loading and linking only.
- LANDING-PLAN, Open items, "Module evaluation claim": add the measured failure modes (TypeError, TDZ
  ReferenceError, double evaluation, the Debug assertion) and the unlocked `asyncParentModules()` walks.
- LANDING-PLAN, "GIL on above flag off": add that with `useHandlerICInFTL` implied, the FTL's strength reduction
  leaves the whole call case (none of its `DirectCall`, `CallFFI`, `CallWasm` conversions), not only "handler inline
  caches in FTL code".

### H: What was run

All single-process, seconds each, on the existing Release binaries (`main`, final round-ten tree) and one Debug run:
- `--dumpOptions=2` GIL off / GIL on / flag off (option state of the FFI, handler-IC and wait-for-plans options).
- `ffi-callffi-was-compiled.js` GIL off and GIL on; `-p out.json tail-call-profiler.js` GIL off (the FATAL line).
- `main` with `--useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzRandomly=true` on three FFI stress
  files, ten runs each.
- An FFI call microbenchmark (`$vm.ffiFunction` on the `ffi_add_i32` fixture) under `perf stat -e instructions:u`,
  two loop lengths, in: flag off (default; `--useFFICallInDFG=0`; `--useFFIICStub=0`; `--useDFGJIT=0` with and without
  the stub), GIL on (default; `--useDFGJIT=0`), GIL off (default; `--useDFGJIT=0`); `--reportCompileTimes=1`,
  `--dumpGraphAfterParsing=1`, `--printEachOSRExit=1` on the same file GIL on and flag off.
- A spawned-thread FFI call and creation, GIL off and GIL on.
- `$262.agent` + `new Thread` in the second VM, GIL off and GIL on.
- Deferred-module races (`--useImportDefer=1 -m`): 4 threads on one slow module (5 runs), 4 threads on 200 modules
  (3 Release runs GIL off, one Debug run), and a top-level-await child fulfilled by a Thread (both modes).
- A promise reaction settled by a Thread (which thread runs it), both modes.

Every GIL-off run above went through a two-line bash wrapper that sets the three GIL-off environment variables
literally, and the wrapper was checked with `--dumpOptions=2` (`useThreadGIL=false`, `useTaggedButterflies=true`,
`usePollingTraps=true`) before any number was taken. The reproducers (FFI call from a Thread, the second-VM spawn,
the three module scenarios, the FFI microbenchmark) are described where their numbers are used; the proposed tests
are their in-tree form.

## Section I. GIL-off safety residue: deferred claims (precondition 10), the persistent wrong value, the stop counters

Scope: three GIL-off safety items that the tenth round left open. All three were worked by reading the current tree
(the round-10 commit); nothing was built and no GIL-off measurement was taken. File and function names are given for
every claim; line numbers are omitted on purpose.


### I: Inventory

#### I-1. What is left of GIL-removal precondition 10 (deferred Class-A claims) - GIL off - *explained*

**Mechanism, as the code has it.** `WatchpointSet::fireAllSlow(VM&, DeferredWatchpointFire*)` (bytecode/Watchpoint.cpp)
claims a set with `m_state.compareExchangeStrong(IsWatched, IsInvalidated)`, moves the members into the deferred
object's private set (`WatchpointSet::take`, which re-arms that private set to `IsWatched`), and returns; the members
fire when the `DeferredStructureTransitionWatchpointFire` goes out of scope (`~DeferredStructureTransitionWatchpointFire`
-> `DeferredStructureTransitionWatchpointFire::fireAllSlow`, runtime/Structure.h / Structure.cpp), through
`fireAllUnderClassAStop`. GIL off the claim is counted in `s_deferredClaimsInFlight` (incremented before the CAS,
decremented by `noteDeferredClaimFired` after the fire; a thread's own share is kept in `t_ownDeferredClaimsInFlight`).
Only a *fat* `InlineWatchpointSet` reaches this code (a thin set has no member, so nothing elides a check on it).

The only set that is fired through a deferred object is a Structure's `m_transitionWatchpointSet`
(`Structure::fireStructureTransitionWatchpoint`, reached from `Structure::finishCreation(vm, previous, deferred)` for every
transition that creates a Structure, and from the inline original-array leg of `Structure::nonPropertyTransition`). Its
consumers, i.e. what holds "this object still has structure S" without a run-time check:

| consumer | where registered | what it elides |
|---|---|---|
| DFG/FTL constants whose structure is watchable | `Structure::dfgMayWatch`; `DFG::Graph` (the three `dfgMayWatch` sites in DFGGraph.cpp), `StructureAbstractValue` | `CheckStructure` on a compile-time-constant cell; everything folded from it: `GetByOffset`/`PutByOffset` offsets, the indexing shape of a constant array, `CheckArray`, presence/absence of a property, `in`/`hasOwnProperty` folds |
| DFG adaptive structure watchpoints | `DFG::AdaptiveStructureWatchpoint::install` (`addTransitionWatchpoint`) | an `ObjectPropertyCondition` on a prototype or singleton (Presence at an offset, Absence, AbsenceOfSetEffect, Equivalence together with the replacement set) |
| inline-cache conditions | `PropertyInlineCacheClearingWatchpoint`, `InlineCacheCompiler` (the `structureTransitionWatchpoint` of a condition set) | prototype-chain conditions of proto loads, misses, setter-miss for cached transitions |
| runtime caches | `StructureRareData` cached property-name enumerators (`propertyNameEnumeratorMayWatch`), `CachedSpecialPropertyAdaptiveStructureWatchpoint` (cached `toString`/`valueOf`/`@@toPrimitive`), `ObjectAdaptiveStructureWatchpoint` / `AdaptiveInferredPropertyValueWatchpointBase` (the realm's iterator, species and similar protocol sets) | a re-validation of the cached answer |

**Every deferring site in the tree**, with the GIL-off leg that actually runs (`Options::useTaggedButterflies()` is true
GIL off), the fact that goes false, and what a stale consumer can read or do between the publication and the fire.
"Loser" is a second thread that finds the set already claimed, publishes its own transition out of S on its own object
and returns into its own optimized code (consequence (a) of SPEC-jit section 5.6); "bystander" is any other thread's
stale code running against the transitioned object (consequence (b)).

| # | site (function) | transition | stale consumer sees | verdict |
|---|---|---|---|---|
| 1 | `JSObject::putDirectInternal`, last leg -> `Structure::addNewPropertyTransition` (also via `salDeferredFire` when the caller passes none) | property addition, reallocating or not; conversion to a cacheable dictionary when the slot count passes the limit | every offset of S keeps its meaning in S'; a superseded flat butterfly is never rewritten, so a read through a held butterfly pointer returns the old value and a write through it is lost; an absence fact (no own `foo`, no setter `foo` on the chain) stays "true" for the stale code | value-safe. Same-thread (loser) effect: its own `o.foo = v` is not seen by its own folded `'foo' in o`; a setter defined on a prototype is skipped by a stale inlined add |
| 2 | `deletePropertyNamedConcurrent` (JSObject.cpp), non-dictionary leg -> `Structure::removePropertyTransition` / `removeNewPropertyTransition` | property deletion | the slot holds the old value, then `undefined` (the D1 store precedes the publication), **until the slot is reused**; see I-1a | value-safe while the slot is quarantined; **not safe after reuse** |
| 3 | `putDirectInternal` replace legs and dictionary leg -> `Structure::attributeChangeTransition` (kind kept) | writable / enumerable / configurable change | the slot and its kind are unchanged; stale code can still store to a property that is now read-only, and does not fire the new structure's replacement set | value-safe; semantic (a write that the other order forbids succeeds) |
| 4 | `JSObject::definePropertyChangingKindGILOff` | data <-> accessor/custom | - | already "publish and fire in one stop" (tenth round) |
| 5 | `JSObject::publishStructureOnlyTransitionConcurrently` callers: `seal`, `freeze`, `preventExtensions`, `notifyPresenceOfIndexedAccessors`, `switchToSlowPutArrayStorage`, `setPrototypeDirect`, `setPrivateBrand`, `convertToDictionary`, `convertToUncacheableDictionary` | structure-only | offsets unchanged. Stale code keeps the old prototype chain, the old extensibility, the old brand. An *inlined transition* (`PutStructure`) cannot overwrite the new structure: GIL off DFG/FTL inline a transition only under the four thread-local sets and `CheckTransitionOwner`, and a foreign transition fires those sets under a stop before it publishes (SPEC-jit section 5.5 Transition) | value-safe; semantic |
| 6 | `JSObject::relabelIndexingShapeConcurrent`: Undecided->Int32/Contiguous, Int32->Contiguous (Int32->Double is served as Int32->Contiguous, T4-O) | encoding-preserving relabel | lanes stay boxed values or holes; Int32-typed reads verify `isInt32` GIL off (OM I41) | value-safe |
| 7 | `relabelIndexingShapeConcurrent` out of Double; `JSObject::convertToArrayStorageConcurrent`; `createArrayStorageConcurrent` | encoding-changing / relayout | - | already in one stop (tenth round) |
| 8 | `JSObject::createInitialIndexedStorageConcurrent` (first indexed storage: blank -> Undecided/Int32/Double/Contiguous) | new butterfly word, out-of-line properties copied | stale code that believes "no indexed storage" answers a hole; a held old butterfly still has the properties | value-safe |
| 9 | `tryMaterializeCopyOnWriteButterflyForSharedWrite` / `JSObject::convertFromCopyOnWrite` | copy-on-write -> writable | stale readers read the immutable butterfly; code compiled for copy-on-write arrays never writes without its own conversion | value-safe |
| 10 | `Structure::flattenDictionaryStructureByTransitionConcurrent` | dictionary -> non-dictionary, same offsets, same butterfly (N2) | nothing changes for a reader | safe |
| 11 | `JSGlobalProxy::setTarget` | proxy target change | the old target | semantic |
| 12 | the inline original-array leg of `Structure::nonPropertyTransition` (arrays of the realm's original structures) | as 6/7 by kind | as 6/7 | as 6/7 |

So the statement in LANDING-PLAN ("elided checks read a slot that holds a value of the kind they expect - the old
value, `undefined` after a delete") holds for every site **except row 2 once the deleted slot is handed out again**.

**I-1a. The unsafe remainder: a deleted slot reused inside a claim window.** *explained by code path; not reproduced.*
A deleted offset is quarantined with the owning heap's quarantine epoch (`PropertyTable::quarantineDeletedOffset`) and
becomes reusable once the epoch has moved (`PropertyTable::releaseQuarantinedSlots`, called lazily from
`hasDeletedOffset`/`nextOffset`). The epoch moves once per collection, world stopped
(`butterflyQuarantineEpochSafepointHook`, runtime/ConcurrentButterfly.cpp). The argument for reuse (ConcurrentButterfly.cpp,
"Task 9") is that one stop flushes every holder of a stale offset, because offsets are held only across poll-free
windows. Optimized code that elided its check on a claimed, unfired set is a holder that **survives stops**: it is
retired only by the claimant's own fire. And a collection's stop can fall inside the window: between its publication
and its fire the claimant executes no poll, but the fire itself begins with `JSThreadsSafepoint::stopTheWorldAndRun`, in
which a requester that finds another stop pending (a shared-heap collection holds the conductor lock for its whole
stopped window) parks for it before it conducts its own. Sequence:

1. Thread A deletes `k` from constant object `C` of watched structure S: claims S's set, stores `undefined` into slot X,
   publishes S - k (X quarantined at epoch e), reaches the scope exit and asks for its stop.
2. A collection is conducted first; its hook moves the epoch to e+1; the world resumes. A is next in line.
3. Before A's stop parks everybody, thread D adds a property to `C` (or to any object that reaches a table carrying X):
   `nextOffset` promotes X and hands it out. If the new property is an accessor, slot X now holds a `GetterSetter`.
4. Thread B's optimized code, which folded "`C` has S, `k` lives at X", runs `C.k` and receives the `GetterSetter` cell
   as a value (or, for a stale `C.k = v`, stores a plain value over the accessor's cell, which a later accessor lookup
   casts).

This is the same class as the two crashes that started history section 55 (a non-value cell reaching a data read).
Steps 2-4 need a collection to slip between A's publication and A's stop and a foreign add within the few
microseconds after the collection resumes; nothing has shown it, and `cve/mc-code-deferred-fire-stale-window.js` does
not delete. It is the one consumer of the remaining windows that is not a wrong value but a wrong kind.

**What it is worth.** No test result depends on it today. It is the content of the "precondition 10" open item; closing
it is what lets that item be retired rather than restated.

#### I-1b. The protected transitions' wait can be missed on arm64 - GIL off, arm64 only - *explained by reading*

The three protected transitions decide "is somebody's claim in flight" by reading the claimed set's state and then
`s_deferredClaimsInFlight`. The count is incremented (seq_cst) before the claim CAS precisely so that "whoever observes
`IsInvalidated` from this claim also observes the count". The observation of `IsInvalidated` is, on the common path, the
inline pre-check of `InlineWatchpointSet::fireAll(vm, deferred)` / `WatchpointSet::fireAll`, a **relaxed** load
(`state()`); the count is read afterwards with a seq_cst load. On x86-64 two loads are not reordered. On arm64 a plain
load followed by a load-acquire may be satisfied in the other order, so a thread can read count == 0 (old) and then
`IsInvalidated` (new), conclude that the set was fired long ago, and publish inside another thread's window. See "arm64
notes" for the one-line change.

#### I-1c. Two threads that each hold a claim and wait for the other - GIL off - *latent, by reading*

`WatchpointSet::awaitDeferredClaimsInFlight` waits until the process-wide count equals the caller's own share. A
thread that waits while it holds an outer, unfired claim of its own, against a second thread in the same position,
waits for ever (each outer claim fires only when the nested call returns). Today no protected transition is reachable
from inside a claim scope (the scopes of `putDirectInternal`, `deletePropertyNamedConcurrent`,
`publishStructureOnlyTransitionConcurrently` call only the locked publication protocols), so this cannot happen in the
current tree; any design that adds waiting at more sites (design D below) has to keep it that way.

#### I-2. A persistent wrong value under heavy foreign churn - GIL off - *not explained; the object model's add and delete legs are cleared by reading; three hypotheses ranked with a discriminator each*

**Symptom (from the open item).** Two threads add `w<id>_<i & 255>` and delete the key sixteen iterations behind on an
object a third thread created; about once in 100,000 amplified runs on a loaded machine `o["w1_67"]` reads `"w0_55!"`
through every path, `w0_55` is absent, a write through `w1_67` lands in that slot, no table shows a duplicate offset,
and the value stays until thread 1 rewrites `w1_67`.

**What the end state requires.** The last store into the slot that the table gives `w1_67` was thread 0's value, and
thread 0's later `delete o.w0_55` either missed or stored its `undefined` elsewhere. There are exactly three ways to get
there: (S) *slot confusion* - thread 0's add stored at X and was recorded at Y (or not at all) while thread 1's add got
X; (K) *key confusion* - thread 0's put resolved its key string to the atom of `w1_67` and replaced an existing
property; (V) *value-cell aliasing* - the slot is right and holds thread 1's string cell, but that cell's memory now
holds thread 0's string.

**(S) was read end to end; no site was found.** Every leg a foreign or owning thread can take for `o[k] = v` and
`delete o[k]`, with where the value store sits relative to the point after which the publication can no longer fail:

| leg (function) | regime | order | can the store be orphaned? |
|---|---|---|---|
| `tryStructureOnlyTransition` (ConcurrentButterfly.cpp) | inline offset, any thread | cell lock, header ID verified, **claim CAS**, store, header CAS | no: nothing is written before the claim |
| `trySegmentedTransition`, flavor FirstInstall | first butterfly | store into the private new butterfly, claim, DCAS | no: private storage |
| same, flavor StayFlat (owner, SW=0) | flat, owner, locked | private copy: store then claim; existing storage: **claim, then store** (`deferredSharedSlot`) | no |
| same, flavor StayFlatShared (foreign, no growth) | flat, foreign | claim, then store, then DCAS | no |
| same, flavor Segmented | segmented | **store into the (shared) fragment slot, then claim**, then DCAS | only if the claim can fail after the ID check made under the cell lock. The lock-free claimants are the owner legs (`butterflyWordOwnedByCurrentThread`, false for a segmented word), N2-LF (butterfly-less only), the N3 indexed first install (butterfly-less only) and the inline-cache and megamorphic transition stubs (owner test, false for a segmented word). So it cannot fail; the taxonomy-(c) re-entry re-stores. It is the one place where the order is not claim-first, and the diagnostic build should trip on "stored, then lost the claim" there |
| `convertToSegmentedButterfly` | flat -> segmented with the add fused | build spine, claim, store, DCAS | no |
| `JSObject::tryPutDirectTransitionConcurrent`, E4 leg | owner, sets valid | poll-free, no foreign actor possible | no |
| same, N2-LF and E4-C legs | owner, sets dead | claim CAS, word re-check, store, publish | no |
| `putDirectInternal` dictionary leg -> `Structure::addOrReplacePropertyWithoutTransition` | any dictionary | cell lock, ID verified, offset drawn and value stored inside the structure-lock section, entry published last (history section 41) | no: every other writer of the object takes the same cell lock, dictionaries have no lock-free owner leg |
| `deletePropertyNamedConcurrent`, both legs | | cell lock, ID (and for a cacheable dictionary table pointer and edit stamp) verified, D1 store, header CAS; a lost lane restarts | the D1 store lands in the slot of the key being deleted, which still owns it in whatever structure won |
| `Structure::flattenDictionaryStructureByTransitionConcurrent` | reader or writer inline cache | clone, publish under the cell lock if the edit stamp and the clone still match | offsets never move (N2) |

Also read and cleared for this signature: the seqlock fast path of `Structure::getConcurrently` (a probe that overlaps
a thief's edit fails the stamp or the slot re-check; on x86-64 the load order carries the argument); the steal in
`Structure::takePropertyTableOrCloneIfPinned` (nulls the source's slot under the source's lock before the thief
mutates); `Structure::materializePropertyTable` with `replayFromRecord` (recorded offsets, residual re-quarantined at
the current epoch); `putMegamorphic` and `tryCachePutBy` (the transition entry is refused unless
`newStructure->previousID() == oldStructure`; replace entries are never made by this test and are refused for
dictionaries GIL off in Repatch); the per-thread megamorphic caches (only their owner fills them). Table contents are a
function of the structure in every path, so "X is free in S" is a property of S alone.

**(K) was read for the caches on this path; all are per-thread or content-verified.** `JSString::toIdentifier`
bypasses `vm.lastAtomizedIdentifier*` GIL off (the comment there names exactly this failure: "property reads/writes
land on another thread's last-atomized key"); `KeyAtomStringCache::make` returns the snapshot it verified by hash and
content (the fix for the earlier "sibling's value" incident, SPEC-objectmodel history section 22);
`JSRopeString::resolveRopeToAtomString` and the `jsAtomString` family flatten into stack buffers;
`ConcatKeyAtomStringCache::getOrInsert` is locked GIL off and its inline probes are not emitted (and the key of this
test, a two-variable concatenation, gets no such cache: `compileMakeAtomString` creates one only when the other
operands are constants); `VM::liveNumericStrings` is per thread; the shared atom table is sharded and locked.

**(V) is where the campaign's tooling points.** Every `RaceAmplifier::perturb()` site in the tree is in the heap or in
thread/VM teardown (`LocalAllocator::allocateSlowCase`, the two empty-block steals, `CompleteSubspace`,
`GCSafepointEpoch`, `Heap.cpp`, `ThreadManager`, `JSLock`, `VM`); none is in the object model. An amplified run
therefore widens block handout and steal windows, not transition windows. The two writers allocate the value ropes
`k + "!"` from two `LocalAllocator`s over one `BlockDirectory` at the same instant (the indices 55 and 67 of the one
recorded example are twelve iterations apart, i.e. the threads were in step). If one free cell reaches both
allocators' free lists (own-directory refill under the per-directory stripe against the exclusive cross-directory steal
in `LocalAllocator::allocateSlowCase`, or a block swept twice), thread 1's `o["w1_67"]` and thread 0's `o["w0_55"]`
hold the same address and the second initialization wins: every detail of the symptom follows, including "absent"
(thread 0 deleted its key sixteen iterations later), "no duplicate offset", "needs two cores", and "present on the
round's first build". A premature free followed by reuse gives the same picture but is less likely on that build
(window liveness retention was still on there). This was not read further: it is outside the object model.

**Worth.** One standing divergence class in the amplifier campaign (about 1 in 100,000 amplified runs of one test); a
silent wrong value in a user program if real.

#### I-3. `VMManager` counters under the shared collector's stop with Workers (AUDIT R9-20) - GIL off with a second VM - *explained by code path*

`VMManager::enterStopTheWorldParticipation` fails `m_numberOfStoppedVMs + m_numberOfBlockedVMs <= m_numberOfActiveVMs`.
The three counters, all under `m_worldLock`:

- active: set to 0 by `requestStopAllInternal` when the mode is RunAll, then recounted over **entered** VMs only
  (`if (vm.isEntered()) incrementActiveVMs(vm)`); also incremented, unconditionally, by `notifyVMStop` for the arriving
  VM; invalidated by `resumeTheWorld`, which also clears every VM's `m_hasBeenCountedAsActive`.
- stopped: incremented by `notifyVMStop` (and `notifyVMUnblocking`), decremented by the participant itself at the very
  end of `enterStopTheWorldParticipation`, after it has left the wait loop and re-taken the lock.

**The escape.** A participant that is parked in the wait loop across a resume immediately followed by the next stop
never decrements (it re-evaluates `shouldStop()`, finds the new request pending and keeps waiting), so it stays in
`stopped`. It is put back into `active` by the recount only if its VM is *entered*. A VM that reached `notifyVMStop`
without being entered is therefore counted stopped and not active from the second stop on, and as soon as every
entered VM has arrived, stopped = entered + 1 > active. Three ways to arrive not entered, confirmed against the code:

1. `VMManager::notifyVMConstruction`: a VM constructed while a stop is in progress calls
   `notifyVMStop(vm, VMCreated)` before it has ever been entered (then `decrementActiveVMs`). This is a Worker being
   spawned during a collection, which is what Bun's `worker_threads` test does under load. **Most likely cause.**
2. A trap poll outside any `VMEntryScope` (an embedder calling `VMTraps::handleTraps` or
   `VM::hasExceptionsAfterHandlingTraps` from its event loop with the API lock held; the jsc shell never does).
3. GIL off, `decrementActiveVMs` returns early for a sibling only while `vm.isEntered()`; if the representative arrived
   not entered and the last entered sibling leaves, the VM is uncounted from `active` while its representative is
   still in `stopped`.

Not an escape: `notifyVMDestruction` does not touch the counters, but an entered VM passes through
`notifyVMDeactivation` first, and a VM cannot be destroyed by a thread that is parked in participation.

The open item's candidates are therefore confirmed as (1) and (2) being the same defect (the recount keys on
`isEntered()`, participation does not), (3) a second instance of it, and "a VM destroyed during a stop" refuted.

**Worth.** One abort in 3 and one in 5 Bun `worker_threads` runs under heavy load, 0 of 28 since; GIL off only (it
needs a shared-collector stop and a second VM).


### I: Designs

#### D-1. Close the deferred-claim windows that are not value-safe, and the same-thread one everywhere

**Status: proposed.** Four parts that can land separately; (E) and (F) are a few lines each.

**(E) No quarantine promotion while a claim is in flight.** Rule: `butterflyQuarantineEpochSafepointHook` does not move
the epoch when the process-wide claim count is non-zero. Writers: the collection's conductor, world stopped. Readers:
`PropertyTable::hasDeletedOffset` (mutators, under the structure lock or on a private table). Tiers: none; generated
code never reads the epoch. Argument: the hazardous window of I-1a is [publication of the delete, completion of the
claimant's fire], and the claim is counted from before its CAS to after its fire, so every epoch move inside a window
happens with count > 0; suppressing those moves means the deleted slot is still quarantined when the fire retires the
stale code, and a parked thread resumes into the poll's invalidation point (I21) before it can use the offset again.
Memory ordering: the hook runs inside the stop; every claimant's seq_cst increment precedes its arrival at the
safepoint, and the stop's barrier orders it before the hook's load on both x86-64 and arm64. Collector: promotion is
delayed by at most the collections that overlap a claim; the index-vector quarantine of `PropertyTable::rehash` shares
the epoch and is delayed likewise (memory only). Second thread mid-way: sees a quarantined slot a cycle longer. Flag
off: the hook is registered only flag on; nothing. Failure mode: a claim that never fires pins the epoch; detected by
the existing `RELEASE_ASSERT(before)` pairing in `noteDeferredClaimFired` and, in Debug, by asserting count == 0 at VM
teardown. Test: `jit/deleted-slot-is-not-reused-inside-a-claim-window-gil-off.js` - thread A deletes from a watched
constant object in a loop with fresh structures, thread D adds an accessor after each collection it forces, thread B
reads the deleted name in FTL code; counts non-`undefined`, non-old reads (expected 0 before too - the window is
microseconds - so the test's value is under the amplifier with a `perturb()` added to the fire's entry). Expected gain:
none in performance; removes the only kind-unsafe consumer. Risk: negligible. Alternative rejected: making deletes of
watched structures "publish and fire in one stop" - correct, but it adds the wait protocol to the commonest structural
operation after adds.

**(F) The loser waits before it returns to its own code.** Rule: GIL off, the destructor of every
`DeferredStructureTransitionWatchpointFire`, after firing its own claim if it holds one, waits - park-capable, through
`WatchpointSet::awaitDeferredClaimsInFlight` - until no *other* thread's claim is in flight, unless this thread still
holds an outer claim (`t_ownDeferredClaimsInFlight != 0` after its own fire) or runs inside a stop
(`shouldAwaitDeferredClaimsInFlight` already says so). Who writes: nobody new. Who reads: the count, one seq_cst load
per transition scope exit, plus the thread-local. Tiers: C++ only; generated transitions (inline-cache handlers,
DFG/FTL `PutStructure`) do not create structures and so never defer. Argument: a thread that made a transition out of S
while S's set was claimed by W and unfired returns to JS only after W's fire has completed; that fire jettisons every
code block that watched S, the loser's own frames included, and they exit at the invalidation point after the call.
So consequence (a) - a thread's own program order broken by its own stale code (its own `delete`, `freeze`,
`defineProperty` not visible to its own next statement) - is closed at every deferring site, without a restart path and
without moving any fire before its publication. It is conservative in two ways that cost nothing measurable: it waits
for any claim, not only the one on its set (claims are the first transition out of a watched structure: rare, and one
transition long), and it waits even if this thread lost no race. The outer-claim exemption is what prevents I-1c: a
thread never waits while it holds something another waiter needs; the outermost scope exit of the same dynamic extent
waits instead. Memory ordering: as the existing rule; on arm64 with the change of "arm64 notes". Collector and stops:
the wait parks for every stop (`parkSitePollAndParkForStopTheWorld`), which is how W's fire runs. What a second thread
sees: nothing new. Flag off and GIL on: the destructor's new leg is behind `g_jscConfig.gilOffProcess`. Failure mode: a
wait that never ends = a claim that never fires (as (E)); a deadlock through a lock held at scope exit - the scope
exits are the places that already conduct stops, so none holds a lock (SPEC-jit section 5.6, "lock-free by
construction"); the stop watchdog names it if one does. Tests: extend
`jit/unsafe-transitions-publish-and-fire-in-one-stop-gil-off.js` with two "same-thread" rounds (thread B deletes /
freezes its own object of the shared watched structure inside thread A's window and checks its own next read and
write); count wrong own-reads (seen only with the fire's entry perturbed) before, 0 after. Expected gain: none in
performance (one load per transition; transitions GIL off cost hundreds of nanoseconds); behaviour: a thread's own
structural changes are always visible to itself. Refinement worth doing in the same change: count only claims whose
set had members (`m_setIsNotEmpty`), since a claim on an empty fat set has no consumer; this removes almost all waits.
Alternative rejected: a fourth watchpoint state ("invalidated, fire pending") so that a loser waits only for its own
set - exact, but `state() == IsInvalidated` is compared in C++ and in generated code in many places, and the
process-wide count gives the same guarantee.

**(G) Stops that cannot wait fire the parked claimants' sets themselves.** The corner of the open item: a transition
made *inside* somebody's stop (`JSGlobalObject::haveABadTime` converts every array inside its stop) cannot wait for a
claimant that the same stop has parked. Rule: GIL-off deferred claims are linked, from claim to fire, on a process-wide
intrusive list (the claim CAS winner links its `DeferredStructureTransitionWatchpointFire` under a leaf lock; the
destructor unlinks). A conductor that is about to publish one of the protected transitions inside a stop first walks
the list and fires every listed object's private set (`fireAllNow`, legal: the world is stopped for every client),
marking it "fired by proxy"; the owner, when it resumes, finds `holdsClaim()` false, skips its fire and only drops its
count. What is given up: a set fired by proxy is fired *before* its owner's publication, so an adaptive watchpoint
among its members re-installs against the old structure and may give its fact up conservatively
(`objectmodel/indexing-transition-keeps-adaptive-watchpoint.js` is the test of that order); here that happens only
when a realm has a bad time in the instant another thread is between a claim and its fire, and giving up is sound.
Collector: the list holds stack objects of parked threads, read only world-stopped. Flag off: nothing. Failure mode: a
list entry outliving its frame - the destructor unlinks unconditionally; Debug asserts the list empty at VM teardown.
Test: `objectmodel/having-a-bad-time-vs-claimed-transition-gil-off.js` gains a round that checks a constant-folded
read of the converted array between the end of the stop and the claimant's resume. Expected gain: closes the last
corner of the three protected transitions. This part is optional; without it the corner stays as documented.

**(H) State the remainder.** After (E), (F) and (G) what is left is consequence (b) for value-safe transitions: another
thread's optimized code reads, for at most one stop's length, the state before a transition that raced it without
synchronization. That is the staleness model's own statement and needs no protocol; SPEC-jit section 5.6 should say so
in those words and precondition 10 can be retired.

Options the task asked to weigh, with the verdict: (A) status quo - not acceptable as it stands because of I-1a; (B)
extend "one stop" to a named subset - the only member worth naming would be deletes, which (E) covers at no cost; (C)
fire before publishing with adaptive watchpoints fixed another way - needs a restart path out of every lock-holding
transition, rejected again, except in the proxy form of (G) where the race is already in progress; (D) the loser waits
- adopted as (F).

#### D-2. One diagnostic build for the persistent wrong value, preceded by two experiments that need no build

**Status: proposed.** The aim is to decide between (V), (K) and (S) before any protocol is touched.

*No build, the diagnostic variant of `cve/mc-val-multislot-clone.js`:*
1. **(K) read-back.** Each writer, right after `o[k] = v`, builds the key string again and checks `o[k2] === v`. A put
   that went to another thread's key fails its own read-back at once (the key it meant to add is absent). Under (S)
   and (V) the read-back passes.
2. **(V) identity.** Each writer keeps its last 32 values in a per-thread array (which also keeps them alive) and logs
   `describe(v)` (the cell address) into a per-thread ring. When the reader sees the bad state it compares
   `o["w1_67"]` with both rings: the same address in both threads' rings is a cell handed out twice; if the bug stops
   reproducing with the values kept alive and reproduces without, it is a premature free.

*One checked Release build, all behind one option so that the same binary runs the normal lanes:*
- (S1) in every add leg of the table above, after the claim or lock and before the value store: the slot must be empty
  or hold `undefined` (the existing Debug assertions of `putDirectInternal` and `tryPutDirectTransitionConcurrent`,
  made release checks under the option);
- (S2) at every publication under the cell lock: no other entry of the published table maps to the offset just filled,
  and a reused offset's previous key is gone (the open item's check);
- (S3) `trySegmentedTransition`, flavor Segmented: fail-stop if `storedValue` is set when the claim CAS fails;
- (S4) at `putDirectInternal`'s flag-on exits: look the key up again and compare the offset;
- (V1) in the allocation path, under the option: the cell taken from a free list must still be zapped
  (`HeapCell::isZapped`); a cell that another thread has already initialized is reported with both threads' ids;
- (V2) `validateFreeListStructure` on for the run;
- (K1) at the put's entry: the resolved uid's characters equal the subscript string's characters (compare content, not
  pointer) when the subscript is a string.
Run it under the same load, pinned off, with the amplifier, on the diagnostic variant only. Each check names its
hypothesis; one campaign decides. Verification cost: one Release build and one overnight campaign of one test.

If (V1) fires the fix belongs to SPEC-heap (block handout under the per-directory stripe versus the exclusive steal);
if (K1) fires, to the cache it names; if (S1)-(S4) fire, the leg is named by the check.

#### D-3. `VMManager`: a VM counted stopped is counted active

**Status: proposed.** Invariant (all under `m_worldLock`, mode not RunAll): **every VM whose participant is counted in
`m_numberOfStoppedVMs` is counted in `m_numberOfActiveVMs`.** `stopped + blocked <= active` follows.

- New per-VM flag `VMTraps::m_hasBeenCountedAsStopped`, set where `++m_numberOfStoppedVMs` is (both sites), cleared
  where `--m_numberOfStoppedVMs` is.
- Recount (`requestStopAllInternal`, from RunAll): `if (vm.isEntered() || vm.traps().m_hasBeenCountedAsStopped)
  incrementActiveVMs(vm)`. This is the fix for escapes 1 and 2.
- `decrementActiveVMs`: return early when the VM is counted stopped (it generalizes the existing GIL-off sibling guard
  and fixes escape 3); the caller of `notifyVMConstruction` already decrements after participation, when the flag is
  clear.
- Participation exit: a VM that is not entered uncounts itself from `active` as it leaves (mode not RunAll), so that a
  poll outside a `VMEntryScope` does not leave a phantom active VM that `allActiveVMsHaveReachedStoppingPoint` would
  wait for.
- `notifyVMDestruction`: release-assert the VM is not counted stopped; if it is counted active and the mode is not
  RunAll, uncount it.
- Assert `m_numberOfStoppedVMs <= m_numberOfActiveVMs` at the end of the recount as well as at the top of the
  participation loop; keep the 128-event ring.

Memory ordering: every access is under `m_worldLock`; nothing to say for arm64. Collector: the shared collector's stop
is the requester; its conductor never parks here (`currentThreadIsDoingGCWork`). Flag off: the flag is one byte
written under a lock that flag-off code takes only in debugger stops; the recount's extra disjunct is evaluated per
VM per stop. Failure mode: a VM left counted stopped after its participant returned - impossible by construction (the
clear is the same statement as the decrement). Test: `vmstate/vm-constructed-during-back-to-back-stops-gil-off.js`
(jsc shell: a Worker-like second VM is constructed in a loop while the main VM forces collections back to back; before:
the abort within a few hundred constructions if candidate 1 is right - this also *confirms* the diagnosis; after: 0),
plus Bun's `worker_threads` directory under load. Verification: the corpus, Bun's `web/workers` and `node/worker_threads`
GIL off, 50 runs at high load. Risk: low; the change is confined to counter bookkeeping.


### I: arm64 / non-Linux notes

1. **Claim observation (I-1b).** `WatchpointSet::fireAll(vm, deferred)` / `InlineWatchpointSet::fireAll(vm, deferred)`
   pre-check with a relaxed `state()` load; `deferredClaimsInFlight()` then loads the count seq_cst. x86-64: loads stay
   in order. arm64: needs the pre-check (or, equivalently, the first statement of `shouldAwaitDeferredClaimsInFlight`)
   to be an acquire load of the state, or a `loadLoadFence()` between the two. One instruction, GIL-off path only. The
   writer side is already right on both (seq_cst increment, fence, seq_cst CAS).
2. **`PropertyTable` seqlock** (`Structure::getConcurrently` fast path): writer = relaxed odd store, `storeStoreFence`,
   data, release even store; reader = acquire stamp, relaxed probe, `loadLoadFence`, stamp, slot. Correct on arm64 as
   written. The final slot re-check after the stamp re-check has no fence between the two loads; it is needed only for
   a steal that completed before the stamp snapshot, and that case is ordered by the acquire of the snapshot itself.
3. **Table publication** (`Structure::setPropertyTable` release store; the fast path's relaxed load with address
   dependency into the probe): relies on a dependency on arm64; under TSan it is an acquire. Listed for the reading
   pass: if the probe were ever changed to read a member of the Structure rather than of the table after the load, the
   dependency is gone.
4. **D1 store before publication** (`storeUndefinedIntoDoomedSlotConcurrent`, release store, then a seq_cst header
   CAS): fine on both.
5. **Value before structure in the add legs**: release stores of the value followed by seq_cst CAS/DCAS or
   `storeStoreFence` + `setStructure`: fine on both; the E4 leg's comment already notes that the fence does not cover
   `setStructure`'s trailing flag bytes, harmless while E4 excludes ArrayStorage and type changes.
6. **Quarantine epoch** read (`hasDeletedOffset`, acquire) against a world-stopped increment: ordered by the stop.
7. **`VMManager` counters**: lock only.
8. Non-Linux: nothing in these three items depends on the platform's thread primitives beyond `WTF::Lock` and the stop
   machinery already listed elsewhere.


### I: Decisions for the user

1. **How far to close precondition 10.** Options: (i) leave as documented; (ii) (E) alone - removes the only
   kind-unsafe consumer, a dozen lines, no cost; (iii) (E)+(F) - also makes a thread's own structural changes always
   visible to itself, one load per transition scope exit; (iv) (E)+(F)+(G) - also closes the `haveABadTime` corner, adds
   a small registry. Recommendation: (iii) now, (G) only if the corner is ever observed; then retire the precondition
   and state consequence (b) as the staleness model's rule.
2. **Whether the diagnostic build for the wrong value is worth one Release build.** The two no-build experiments come
   first and may already decide the class. Recommendation: do both experiments at the start of the implementation
   session; build the checked binary only if they leave (S) standing.
3. **Whether the `VMManager` fix lands without a reproducer.** The shell test proposed in D-3 should reproduce candidate
   1 deterministically enough to serve as one. Recommendation: write the test first; land the fix with it.


### I: Doc mismatches

- `LANDING-PLAN.md`, Open items, "What is left of GIL-removal precondition 10", and `SPEC-jit.md` section 5.6 last
  paragraph, and `SPEC-jit-history.md` section 55 "What remains": "`undefined` after a delete" is true only while the
  slot is quarantined; one collection inside the window releases it (I-1a). Fix: add the sentence and point to the
  proposal.
- `LANDING-PLAN.md`, Open items, "A persistent wrong value ...": "so the stray store belongs to an add" assumes slot
  confusion; a mis-resolved key (a replace) or an aliased value cell produce the same end state without any stray
  store. "The transition paths store before claiming only under the cell lock with the source re-verified" is exact
  for one leg only (`trySegmentedTransition`, Segmented flavor); every other leg is claim-first or writes private
  storage. Fix: reword as in I-2 and name the two cheaper discriminators.
- Source comments (cannot be changed in this phase; for the implementation session): `JSObjectInlines.h`,
  `putDirectInternal` dictionary leg, still says "dictionary readers are cell-locked, L3" twice, while the comment
  added in the tenth round a few lines below (and history section 41) says no reader takes the cell lock;
  `JSObject.cpp`, `deletePropertyNamedConcurrent`, describes flatten as renumbering offsets in place under the same
  StructureID, which is the GIL-on form only (GIL off it is `flattenDictionaryStructureByTransitionConcurrent`: new
  ID, same offsets).
- `LANDING-PLAN.md`, Open items, "VMManager counters ...": the three candidate escapes can be replaced by the
  diagnosis of I-3 (the recount keys on `isEntered()`); "a VM destroyed during a stop" is refuted.


### I: What was run

Reading only (`grep`, file reads of the current tree). No binary was run, no GIL-off measurement was taken, nothing was
built.

## Section J. The collector with the GIL off

Scope: collection latency on the conducting thread, the service-conductor experiment, Eden pauses with N allocating
threads, weak-bearing blocks, memory of the scaling suite, continuous collection and Debug-build collection cost, and the
finalizer hand-over Bun sees. Release numbers are from the tenth round's final tree unless a line says otherwise; the
service-conductor numbers are from the experiment build of the tenth round (the final tree's predecessor by a few days,
with the conductor behind a run-time option). Collection counts, cycle shapes, instruction counts and resident-set peaks do
not depend on machine load; the few wall-clock figures (pause lengths from `--logGC=1`, the stop-latency micro) were taken
on a loaded machine and are marked "indicative".

### J: Inventory

#### J1. GIL off, the thread that conducts a collection does not run JavaScript for the whole cycle (splay 0.67) - explained

Configuration: GIL off. Rows: splay 0.67 of GIL on (Worst Case 187 against 313), a share of pdfjs (0.81) and gbemu (0.85);
every program whose live heap makes a cycle longer than a few milliseconds; Bun's main thread (its GIL-off runs always
have a second, parked client); Debug builds (J10).

Mechanism. A GIL-off process is a shared server from VM construction (`Heap::tryDesignateStickySharedServer` /
`noteSharedServerSticky`, called by every VM constructor when the process is GIL off), also with one thread. Under the
shared protocol the collector thread is quiesced (`Heap::shouldCollectInCollectorThread` stays false) and an
allocation-triggered ticket is conducted by whichever mutator's poll finds it
(`Heap::stopIfNecessaryForAllClients` -> `tryConductSharedCollectionForPoll` -> `conductSharedCollection`). The conductor
runs the phase loop as `GCConductor::Mutator` with its heap access released for the whole tenure (SPEC-congc section 3.7:
conducting is a closed loop). `runFixpointPhase` schedules one Concurrent phase per cycle GIL off (the single-handoff cap,
`t_sharedGCConcurrentHandoffsThisCycle`), and in `runConcurrentPhase` the shared arm calls
`waitBetweenSharedGCWindows()`: `donateAll()` and `waitForTermination(timeToStop())`, a condition-variable wait. The
legacy Mutator arm (return to JS, poll again at the next allocation slow path) and the legacy Collector arm
(`drainInParallelPassively`) are both excluded when shared (SPEC-congc history, annex CGD1.3: the first returns to JS in
the middle of a tenure that other threads wait on, the second keys on the main client's access state and has no pause
checkpoint). So with one mutator the "concurrent" window has nobody to be concurrent with: seven helpers mark, the
conductor sleeps, and the program loses the whole cycle. With the flag off and with the GIL on the mutator that requested
the collection takes the conn in the legacy protocol and returns to JS between increments; it is stopped only inside the
fixpoint windows.

Evidence (`<jsc> [mode options] --logGC=1 -e 'testList=["splay"]' cli.js`, summed per cycle from the log; "allocated
during cycles" is the largest `a=` of each cycle):

| splay | collections | sum of cycle lengths | sum of in-window pauses | longest pause | allocated during cycles |
|---|---|---|---|---|---|
| GIL on | 9 Eden + 9 Full | 279 ms | 69 ms | 2.1 ms | 256 MB |
| GIL off | 11 Eden + 11 Full | 265 ms | 32 ms | 1.4 ms | 0 |

GIL off the main thread is out of JavaScript for the 265 ms (every cycle: `a=0kb`), GIL on for the 69 ms; the test's wall
time differs by 168 ms (1,254 against 1,086). The marking work is the same: instruction samples per thread
(`perf record -e instructions:u -c 1000003`, `perf report --sort comm`): helpers 10.1 G against 11.1 G, the main thread
4.4 G against 5.5 G. Eden cycles are 17 ms and Full cycles 7.7 ms GIL off, of which 1.4 ms and 1.4 ms are inside windows;
the worst iterations of splay contain one such cycle.

Worth: splay 298 -> 390-445 (the experiment of J2: x1.34 to x1.52), which moves the JetStream total by
1.34^(1/36) to 1.52^(1/36) = +0.8 % to +1.2 %; one of the thirteen rows below 0.80. Also the mechanism behind J5 and J10.

#### J2. What the service-conductor experiment lost, and why - explained (the per-cycle overhead is measured as a sum, not split into its parts)

The experiment (tenth round): a thread with a standalone client (`GCClient::Heap::markStandalone`, attached, access
released) waits under `*m_threadLock` for a granted, unserved ticket, takes access and calls
`tryConductSharedCollectionForPoll`; a mutator's poll leaves a pending ticket to it unless the ticket is older than 5 ms.
The mutators then stop only for the two windows. Recorded result: splay Worst Case x2.3-3.6, FlightPlanner, earley-boyer,
hash-map and Babylon down, total +0.3 % with window-liveness retention off, and the explanation "the second client
engages the client-count heuristics of the allocation limits".

Re-measured on the experiment build with retention off, conductor off against on, one test per process, from `--logGC=1`:

| test | collections off -> on | main thread stopped per cycle, off (whole cycle) | on (in-window sum per cycle) | allocated during cycles, on |
|---|---|---|---|---|
| splay | 11E+11F -> 10E+9F | 17.3 / 7.7 ms | 5.0 / 6.8 ms | 221 MB |
| hash-map | 20E+8F -> 18E+6F | 2.9 / 4.0 ms | 3.0 / 3.9 ms | 41 MB |
| FlightPlanner | 4E+1F -> 4E+1F | 4.3 / 5.0 ms | 2.5 / 2.5 ms | 2.8 MB |
| earley-boyer | 32E -> 31E | 1.56 ms | 1.90 ms | 8.7 MB |
| Babylon | 11E -> 11E | 2.1 ms | 2.5 ms | 0.4 MB |
| cdjs | 29E -> 29E | 1.5 ms | 1.6 ms | 0.3 MB |

The number of collections does not rise with the second client (it falls slightly: the mutator overshoots its budget while
the conductor wakes). What rises is the time the main thread is stopped per cycle when the cycle is short: a cycle the
mutator conducts itself costs it the cycle (1.5-3 ms here); a cycle the service thread conducts costs it both windows,
and with the single-handoff cap nearly all of a short cycle's marking is inside the windows anyway, plus two real
rendezvous with a second thread (request, trap, park, wake; about 0.04 ms per running mutator per stop, J12), a root scan
of another thread's stack, and the work the mutator's own allocation and barriers add to the second window. The break-even
is a cycle of about 3-4 ms (hash-map); below it the service conductor costs the main thread 0.1-0.5 ms more per cycle
(cdjs, earley-boyer, Babylon), above it it saves the concurrent part of the cycle (splay's Eden cycles: 17 -> 5 ms). Three
of the four tests that fell have 1.5-4 ms cycles; the fourth, FlightPlanner, runs five collections and about 20 ms of them
either way and is bimodal on `main` as well (PERF-RESULTS section 6.12), so its fall in a three-run median is not the
collector's. So the loss is not the allocation limits; it is that the experiment used the service thread for every
cycle, short ones included.

With retention on (the experiment's other arm) the client count did matter, through the retention constraint's own gate
(`clientSet().size() <= 1` in the constraint's body in `Heap::addCoreConstraints`): cdjs fell to 0.65 in the full-suite
run. Retention is off by default since the tenth round.

#### J3. What in the tree is keyed on the number of registered clients - explained (by reading)

Consumers of `clientSet().size()` that a never-allocating service client would move:
1. `Heap::updateAllocationLimits`, Full leg: `perTLCFragmentationBytes = capacityOverhead / numClients * numParked`,
   subtracted from the growth headroom when `numClients >= 2`. `numActive` counts `m_sharedGCConductorClient` and every
   client with `m_releasedByGCPark`; a mutator that released through the allocation-path poll instead of the trap park is
   not counted, so with a service conductor the rebase is nonzero whenever the one real mutator happened to stop in an
   allocation slow path (the limits shrink by half the capacity overhead; more collections, never fewer). Not observed in
   the counts above, reachable by reading.
2. Same function, reset block: `m_distinctAllocatorByteThreshold = m_maxEdenSize / registered / 4` (a service client
   halves the threshold a real mutator must cross to count as allocating; harmless with one mutator).
3. `Heap::runBeginPhase`: the sibling-visitor pool is sized `min(cap, clients - 1)` (cap 0 by default).
4. The window-liveness constraint's `clientSet().size() <= 1` early return (option off by default).
5. `LockObject.cpp`: the waiter-shard heuristic at 8 and 16 registered clients.
Consumers keyed on allocating clients (`m_distinctAllocatingClientsThisCycle`: the Eden allowance in
`Heap::collectIfNecessaryOrDefer`, the floating-garbage Full trigger in `updateAllocationLimits`) are not moved by a client
that never allocates.

#### J4. Eden pauses with N allocating threads: what a stop costs now - explained and measured

`JSTests/threads/scaling/splay-like.js` and `raytrace-like.js`, GIL off, `--logGC=1`, the cycles of the parallel leg
(indicative wall-clock, per Eden cycle: whole cycle, then the two windows):

| threads | splay-like Eden cycle | window 1 + window 2 | marked | splay-like Full | raytrace-like Eden (one window) |
|---|---|---|---|---|---|
| 1 | 15-17 ms | 1.0 + 0.7 ms | 50 MB | 1.3-2.3 ms | 0.6-3.0 ms at 33 MB |
| 4 | 27-35 ms | 1.1-3.6 + 2.4-5.0 ms | 97 MB | 2.0-4.0 ms | 1.1-1.5 ms at 131 MB |
| 8 | 38-56 ms | 1.7-2.5 + 5.7-8.6 ms | 160-174 MB | 3.4-5.1 ms | 1.5-1.9 ms at 262 MB |

- The number of stops no longer grows with the thread count (SPEC-heap section 10F): raytrace-like's parallel leg has 10
  Eden collections at four threads and at eight, 11 at one; splay-like 5-6 Eden and as many Full at four and eight.
- What grows is the second window: whatever the one concurrent window left unmarked, plus what N mutators allocated and
  logged in barriers meanwhile, is drained inside it with eight markers. Sixteen markers shorten the second window (4.0-4.5
  against 4.5-7.2 ms at eight threads) and lengthen the first by as much (2.3-6.5 against 1.4-2.8 ms: 8.4 against 8.3 ms
  per cycle in all); four markers give second windows of 13-24 ms. (The ninth round
  measured the same wash: SPEC-heap history section 33.)
- The collector's own user instructions are small: raytrace-like at four threads, the process's GC symbols together are
  under 0.5 % of instruction samples; the 1.1-1.9 ms of a zero-survivor Eden is waiting and kernel work, as flag off.
- Share of the parallel leg spent stopped: splay-like at eight threads about 75 ms of 1,750 ms (6 x 9 ms + 6 x 3.6 ms),
  4 %; at four threads about 3 %; raytrace-like 14 ms of about 300 ms at four threads, 5 %. The remaining distance from
  linear scaling (splay-like 5.7x at eight) is not in the stops.

#### J5. With N threads, one mutator sits out every concurrent window - partly explained (mechanism by reading; its share of the scaling gap was not separated)

The conductor of J1 is one of the N mutators. During the concurrent window of each cycle (splay-like at four threads:
27-35 ms cycles, 4-8 ms of them windows) the other three run and the conductor waits. Which thread conducts is whichever
poll finds the ticket; if the same thread conducted every cycle of the run above it would lose about 100 ms of a
1,230 ms leg, and the harness waits for the slowest thread. The quiet pass's splay-like at four threads is 126 ms away from
linear.

#### J6. Weak-bearing blocks are recycled 32 per collection; a program that makes more than that grows without bound - explained (new)

Configuration: GIL off, any number of threads. Row: `scaling/string-heavy.js` at ONE thread peaks at 478-498 MB resident
against 88 MB flag off and 100 MB GIL on; nothing in JetStream shows it.

Mechanism. A block whose WeakSet has WeakBlocks may not be swept while mutators run: `LocalAllocator::tryAllocateIn`,
`BlockDirectory::findEmptyBlockToSteal`, `BlockDirectory::sweep` and the incremental sweeper skip it (the weak-mutation
protocol: `MarkedBlock::Handle::sweep` begins with `m_weakSet.sweep()`, which rewrites WeakImpl states and free lists that
`Weak<>::clear` reads and writes without a lock, and runs weak finalizers on the sweeping thread). Such blocks are swept
only by the conductor at the end of a cycle (`Heap::reclaimSharedGCMemoryAtCycleEnd` ->
`MarkedSpace::sweepWeakBearingBlocks`), at most `sharedGCWeakBearingSweepBudget` = 32 blocks per cycle end unless the
collection was requested. Every Structure block is weak-bearing (a structure's single transition is a `Weak<>`), and so is
any block holding a cell some `Weak<>` names. string-heavy builds objects with computed keys and makes about 200
structure blocks per Eden cycle; 32 are swept, the other ~170 (2.7 MB) can be reused by nothing, and the allocator mints
fresh blocks: capacity at the start of successive cycles 26, 30, 33, 35, 38 ... 372 MB over 132 cycles with the live heap
under 10 MB and no Full collection.

Evidence: `--sharedGCWeakBearingSweepBudget=256` or `=0` (all): peak 141-145 MB, capacity 26 -> 46 MB; the log's "swept N
weak-bearing blocks" lines sum to 4,224 (always 32) with the default and 26,568 (mean 203, most 317 per cycle end) with
no budget. The specification's bound ("each block is swept within ceil(n / budget) cycle ends", SPEC-heap section 10E
third amendment) holds for a fixed population only.

Cost of the in-stop sweep: 45.78 G against 46.02 G user instructions for the whole run with budget 32 against none, that
is about 10.7 k instructions per block (22.3 k more blocks), about 3 microseconds; 200 blocks add about 0.6 ms to every
stop of this workload, serially on the conductor, and N threads make N times as many blocks.

#### J7. gbemu runs 45 collections GIL off against 17 GIL on - explained (belongs to the array-growth item)

`--reportJSThreadsCounters=1`: `ensureLengthFreshCopy` 13,827 calls, `ensureLengthFreshCopyBytes` 1,129,446,112. Growth by
copy leaves 1.13 GB of dead storage over the run; heap sizes at the start and end of a cycle are the same in both modes
(60 -> 30 MB), so the garbage is 28 extra collections and 54 ms more time out of JavaScript (91 ms against 37 ms). The fix
is the array-growth design, not the collector.

#### J8. ML's Worst Case is not collection latency - explained

ML runs 70 Eden collections GIL on and 74 GIL off, 80 ms and 82 ms in all, every one a single window with nothing allocated
meanwhile in either mode; its Worst Case and its Average fall together (151.5 / 166.8 GIL on, 80.0 / 87.4 GIL off, the
same 0.91 ratio). Of the three tests the tenth round listed under "collection latency on the main thread" only splay is
(Worst over Average 0.55 GIL on, 0.38 GIL off).

#### J9. Memory of the scaling suite, final tree - measured

Peak resident set, MB, GIL off at 1 / 2 / 4 / 8 threads (flag off serial; GIL on at four threads): splay-like 322 / 290 /
403 / 639 (242; 503); map-heavy 167 / 256 / 351 / 548 (165; 168); raytrace-like 71 / 105 / 170 / 303 (70; 70);
string-heavy 478 / 526 / 637 / 721 (88; 100). With window-liveness retention off, a thread adds about one nursery (35-70
MB), which is section 10F's stated cost; the ninth round's 1 GB per thread on map-heavy (1,140 / 2,188 / 4,281 / 8,463 MB)
is gone. string-heavy's first column is J6.

#### J10. Continuous collection and Debug-build collection cost GIL off - explained as J1

A requested synchronous collection costs the same in every configuration, Release and Debug (100 k live objects, `edenGC()`
/ `fullGC()`, indicative: Release 0.42 / 5.1 ms flag off, 0.47 / 5.3 ms GIL off; Debug 8.7 / 483 ms flag off, 8.7 / 505 ms
GIL off). What differs is an allocation-triggered cycle: flag off the mutator keeps running through most of it, GIL off it
loses the whole cycle, and a Debug build's marking is a hundred times slower, so a collection-heavy loop in a Debug Bun
loses hundreds of milliseconds per Full cycle where stock loses tens. `--collectContinuously` with generational collection
off (`stress/delete-property-inline-cache.js`: 1.8 s flag off, 8.9 s GIL off, indicative) is bounded by stop count, not
by the conductor's identity (the service conductor does not change it: its cycles are all windows).

#### J11. Bun: request bodies freed a loop turn late - not explained (cannot be measured without running Bun)

The suspected mechanism is as recorded: a native finalizer that another thread's sweep finds is posted to the VM's event
loop and runs a turn later, so a burst of dead request bodies stays resident across turns. Next cheapest experiment: in
the fixture, count posted finalizers and their queueing delay (a counter in the posting helper), and compare the peak with
the second thread absent.

#### J12. What a stop costs besides marking - measured (indicative)

`edenGC()` 300 times on an almost empty heap while N threads spin in optimized JavaScript (polling back edges): median
0.27 ms with no other thread (the same flag off and GIL on), 0.38 / 0.41 / 0.50 / 0.57 / 0.93 ms with 1 / 2 / 4 / 8 / 16:
about 0.04 ms per running mutator per stop (trap, park, cooperative root snapshot, wake). Parked threads publish their
own stack snapshot (`GCClient::Heap::publishParkedRootSnapshot`), so the conductor does not signal them.

### J: Designs

#### D-J1. A service conductor, used when the cycle is long - **Status: proposed**

Rule (SPEC-congc section 7.2a, new; SPEC-heap section 10.2 amended).

1. *The thread.* A GIL-off shared server owns at most one service conductor: a thread holding a `GCClient::Heap` marked as
   a service client (standalone, and additionally flagged so that J3's consumers skip it). It is created on the first
   allocation-triggered ticket that selects it (rule 3) and joined before `Heap::lastChanceToFinalize`. It never runs
   JavaScript, never allocates cells, holds heap access only from just before its election attempt to just after its
   tenure (as the experiment: `acquireHeapAccess`, `tryConductSharedCollectionForPoll`, `releaseHeapAccess`). Its
   tenure is an ordinary section 10.2 tenure (GCL, GCA, `m_gcConductorThread`), its windows ordinary windows; it is the
   closed loop of section 3.7 with nothing else to do, which is what that contract wants.
2. *Who conducts a ticket nobody waits for.* A mutator's poll that finds a granted, unserved, allocation-triggered ticket
   (`stopIfNecessaryForAllClients`, the tail that calls `tryConductSharedCollectionForPoll`) decides under the same
   opportunistic `m_threadLock` try-lock it already takes:
   - if the predicted length of the cycle is below `sharedGCServiceConductorMinimumCycleMS` (proposed default 5 ms), the
     mutator conducts, as today; the prediction is the length of the last cycle of the same scope (`m_lastEdenGCLength`,
     `m_lastFullGCLength`, written by `didFinishCollection` inside the stop), 0 before the first;
   - otherwise it notifies the service thread and returns to JavaScript; if the ticket is still unserved
     `sharedGCServiceConductorOverdueMS` (5 ms) later, the next poll conducts it after all (the service thread was not
     scheduled, or is being created).
   A requester that waits (`collectSync`, `gc()`, memory pressure) runs the election itself as today: it has nothing
   better to do with its thread.
3. *A lone conductor does not schedule the concurrent window.* When a mutator conducts and this stop interrupted nobody
   else - no other client was running when the stop was requested: none has `m_releasedByGCPark` set and none released
   through the allocation-path poll for this stop (the count `updateAllocationLimits` already takes as `numActive`, made
   exact by also latching the poll-side release) - `sharedFixpointMayResume()` answers false: the fixpoint drains to
   termination inside the one window with the conductor as the eighth marker, and the second rendezvous and the second
   constraint pass are not paid. (Today a single-threaded GIL-off program, and Bun's main thread beside its parked
   keep-alive thread, pay a window in which nobody runs.) A client that was blocked in a native wait and wakes during the
   cycle blocks in its access acquisition until the final close, as it would have for the second window. The predicate is
   evaluated inside the window, where the client set is frozen (I13).
4. *Client-count consumers.* `HeapClientSet` gains `mutatorCount()` (registered clients that are not service clients); the
   five consumers of J3 use it. In `updateAllocationLimits` the conductor counts as active only if it is a mutator.
5. *Destructors and finalizers* that the conductor runs inside its stop (the End phase's eager sweep of lower-tier precise
   allocations, the cycle-end weak-bearing sweep, weak finalizers) and the deferred lambda finalizers it runs after the
   final close (`drainDeferredLambdaFinalizers`) now run on a thread that is neither the VM's thread nor a spawned JS
   thread. SPEC-heap section 10G already says a destructor must not assume its thread; the embedder's marshalling
   predicate must therefore be "not the VM's own thread", not "a spawned JS thread". (Bun's patch tests
   `isOnSpawnedJSThread`; that has to become the complement of "is the VM's thread" before this lands, or the service
   thread runs thread-affine teardown directly.)

Tiers. No generated code changes: mutators already run during the concurrent window when there are siblings; barriers are
always fenced GIL off (`setMutatorShouldBeFenced`'s GIL-off arm) and append to the server mark stack under
`m_serverMutatorMarkStackLock`; allocation during marking is black as for any sibling. C++: the poll tail, the service
thread's loop, `sharedFixpointMayResume`, `HeapClientSet`, the five consumers, VM teardown.

Memory ordering (x86-64 and arm64 alike; nothing here relies on store order). The prediction fields are written by the
conductor inside the stop and read by a mutator after it resumes: the final close's seq_cst `GSP = false` store and the
mutator's gated re-acquisition (F8) order them. The hand-off to the service thread is a condition variable under
`*m_threadLock`, as are the ticket counters and GCA. The service client's access state is the per-client seq_cst byte of
section 10A; the section 10.4 barrier counts it like any client (it is NoAccess except around its own tenure, and during
its tenure it releases first, step 3).

How it meets the rest.
- *Concurrent marking and the stop.* A mutator running during the window is today's N >= 2 case; no new pair of
  concurrent actors exists. The rule that a cell lock holder does not park (CG-I18) and the pause checkpoints for a
  foreign thread-granular stop (section 9.1) are unchanged: the service thread waits in `waitForTermination`, which has
  no counter and no checkpoint obligations (CGD1.3).
- *JSThreads stops and watchpoint fires* interleave through GCL exactly as with a mutator conductor (GCL is released
  between windows; the service thread's re-entry acquires it blocking, which is legal because it holds no access). It is a
  VM-less requester in the GCL-busy rule: timed waits, no VMTraps poll (the standalone arm of
  `tryConductSharedCollectionForPoll` exists).
- *Deferred claims*, the heap-fact epoch and code jettison at End are conductor-context work already audited for a
  conductor that is not the API lock's holder (SPEC-heap T9).
- *What a second thread can observe mid-way:* that the main thread keeps running while a collection is in progress, as any
  sibling does today. Nothing else.

Flag off and GIL on: nothing. The thread is created only by a GIL-off shared server; `mutatorCount()` equals `size()`
there, and the five consumers are already inside `isSharedServer()` arms.

Failure modes and detection. (a) The service thread is not scheduled: the 5 ms overdue rule hands the ticket back; a
counter of overdue take-backs shows a machine where that is the rule. (b) The prediction is wrong (a heap that just grew):
one cycle is conducted by the wrong party, the next prediction corrects it. (c) Teardown with a tenure in flight:
`stopSharedGCServiceConductor` sets the exit flag under `*m_threadLock`, notifies, joins; a VM destroyed while the service
thread is inside a window waits for the final close (the join). Test below. (d) Thread-affine teardown on the service
thread: Debug Bun asserts (the ninth round's listener test is the template). (e) A consumer of `size()` missed: grep gate
in the test plan; the visible symptom is a collection count that differs with and without the thread on a fixed
allocation script.

Tests (`JSTests/threads/gc-stress/`): `service-conductor-runs-the-mutator-through-long-cycles-gil-off.js` (one thread, a
splay-shaped live set; a new counter pair `gcConductedByMutator` / `gcConductedByService` and the bytes the main thread
allocated while a cycle was open: 0 before in every cycle, as `--logGC` shows today; positive after; short-cycle phase of
the same script must show zero service-conducted cycles); `service-conductor-is-not-an-allocating-client.js` (a fixed
allocation script: Eden and Full counts equal with `--useSharedGCServiceConductor=0` and `=1`); `service-conductor-vs-
destroy-vm.js` (`--destroy-vm` while a long cycle is open, 40 runs); `lone-conductor-skips-the-concurrent-window.js`
(one thread: windows per cycle 2 before, 1 after, from a counter); the existing `destructors-run-once-on-any-thread.js`,
`destructor-takes-api-lock-inside-stop-gil-off.js`, `zero-pause-budget-fixpoint.js` unchanged.

Verification: the gc-stress matrix in four modes, the GIL-off corpus under `--verifyGC=1`, TSan GIL off, the amplifier on
the new tests and on `gc-stress/` (500 runs), the scaling bench with scribbled and zombie cells (checksums), a quiet
JetStream pass with the option off and on interleaved, Bun's GIL-off lanes with the marshalling predicate changed.

Expected gain. splay 298 -> 390-445 (the experiment: x1.34 with retention off, x1.52 with it on; by log, stopped time per
Eden cycle 17 -> 5 ms), that is 0.67 -> 0.9-1.0 of GIL on and +0.8-1.2 % of the JetStream total; the short-cycle tests
keep today's behaviour by rule 2 (the experiment lost 6-10 % on four of them by using the service thread for 1.5-4 ms
cycles) and gain rule 3 (one rendezvous and one passive window less per cycle: on splay itself, with the mutator
conducting, 17 -> about 15 ms per Eden cycle, 8/7 of the marking rate). Scaling: the conducting mutator no longer idles
through concurrent windows (J5: up to about 100 ms of splay-like's 1,230 ms leg at four threads). Bun's main thread and
Debug builds stop losing whole cycles (J10).

Risks: a second kind of thread that runs destructors (embedder contract, above); one more actor in the election (covered by
the existing follower path); the threshold is a tuning constant (it is a time, measured on the machine it runs on, not a
heap size).

Alternatives rejected. (i) *The collector thread as a non-client conductor* (SPEC-congc section 7.2 as written): the
same behaviour without a client, so no `mutatorCount()`; needs a nullable conductor client through
`conductSharedCollection` and its helpers (annex CGA2 lists the uses) and an access-less sampler of the section 10.4
barrier; more code for the same gain, and the experiment already runs the client form. Kept as the later shape if the
service client's special cases multiply. (ii) *Run the legacy protocol while there is one client*: it would give a
single-threaded GIL-off program `main`'s collector, and reintroduces the protocol flip at the first spawn that the
designate-at-construction rule removed, for a gain only single-threaded programs see. (iii) *Let the conducting mutator
return to JavaScript during the window*: the tenure is what followers and thread-granular stops wait on; a conductor
blocked in a lock a follower holds is a deadlock (CGD1.3 has the rest). (iv) *More markers / parked siblings as markers*:
measured twice, a wash (J4; SPEC-heap history section 33; the sibling assist is in the tree with a cap of 0).

#### D-J2. Weak-bearing blocks: finalize inside the stop, sweep cells outside it - **Status: proposed**

Stopgap (can land alone): at the end of an Eden cycle sweep every weak-bearing block the cycle allocated into (the
blocks of `MarkedSpace::m_newActiveWeakSets`), and keep the budget of 32 for the backlog a Full cycle creates. That is
the production rate, so the backlog no longer grows; cost about 3 microseconds per block in the stop (string-heavy: 0.6 ms
per stop, peak 498 -> about 145 MB).

Rule (SPEC-heap section 10E third amendment, replaced). The sweep of a block has two halves with different hazards:
- the *weak half*, `WeakSet::sweep()`: finalizes WeakImpls that `reap()` found dead, rebuilds the WeakBlocks' free
  lists, detaches logically empty WeakBlocks. It races `Weak<>::clear` / `WeakSet::deallocate` (lock-free) and an owner
  destroying the handle a finalizer is looking at. It must stay inside the stop (or under the thread-granular stop
  conductor's licence).
- the *cell half*: destructors, the free list, the directory bits. It touches no WeakImpl, and a destructor that drops a
  `Weak<>` only performs the lock-free deallocate. It is what every non-weak-bearing block already does alongside
  mutators under the directory's refill stripe.
So: (1) `WeakSet::reap()` records whether it left any impl Dead (`m_mayHaveDeadImpls`); (2) inside the stop, right after
`MarkedSpace::reapWeakSets()` and over the same lists (the new active sets in an Eden cycle, all active sets in a Full
one), the conductor runs `WeakSet::sweep()` on every set with the flag and clears it: every finalizer of the cycle runs
there, as `reap` already visits every impl of those sets this is a second pass over the same WeakBlocks (1 KB each); (3)
`MarkedBlock::Handle::sweep`, when the server is shared and the world is running, asserts the flag clear and skips
`m_weakSet.sweep()`; (4) the carve-outs in `LocalAllocator::tryAllocateIn`, `BlockDirectory::findEmptyBlockToSteal`,
`BlockDirectory::sweep` and the incremental sweeper's shared step test the flag instead of `weakSet().head()`, so a
weak-bearing block is reused by the allocation path like any other; (5) `sweepWeakBearingBlocks` and its budget go away;
the requested-Full synchronous sweep stays.

Why it is sound. A WeakImpl becomes Dead only in `reap()`, inside a stop, and (2) finalizes it before that stop ends, so
while mutators run no set has a Dead impl and the weak half has nothing to do. A WeakImpl allocated afterwards
(`WeakSet::allocate`, exclusive slow-path lock, which excludes every stripe holder) names a live cell; the cell half never
looks at it. The finalizer-before-destructor order is kept: finalizers at the end of the cycle that found the cell dead,
the destructor at some later sweep. Physical freeing of an empty block (which unlinks its WeakSet) stays world-stopped.
Flag off and GIL on: `isSharedServer()` is false, `MarkedBlock::Handle::sweep` is unchanged; the flag write in `reap()` is
one byte store per reaped set.

Memory ordering: the flag is written inside the stop and read by allocation paths after the resume; the stop's close
(seq_cst) orders it on both architectures. `WeakImpl` states keep their present discipline.

Collector, stop, watchpoints: weak finalizers that fire watchpoints or take the API lock (Bun's unprotect) run where they
run today, inside the conductor's window with conductor re-entry (SPEC-heap history section 36). More of them run per stop
than the budget allowed (all of the cycle's, as flag off runs them lazily); `StructureTransitionTable`'s owner and the
inline caches' weak references are the bulk and do no work beyond clearing a slot.

Failure modes: a mutator-side sweep that meets a Dead impl (the assertion in (3), Debug and the amplifier); a finalizer
whose cost was hidden by the budget (continuous-collection lanes: section 35's test
`weak-bearing-sweep-bounded-per-stop-gil-off.js` is rewritten to bound the in-stop weak pass by time, and the
progress-instrumented `delete-property-inline-cache.js` must not regress from 21 s).

Tests: `gc-stress/weak-bearing-blocks-are-reused-gil-off.js` (a loop that makes 200 structure blocks of garbage per
cycle: `$vm.heapCapacity()` after 100 Eden cycles, 370 MB before, under 60 MB after; passes flag off and GIL on
unchanged); `destructors-run-once-on-any-thread.js` (every cell destroyed once: the ninth round's 12,350 lost cells are
the regression to watch); a two-thread test in which one thread clears `Weak<>`-backed handles (FinalizationRegistry
unregister, WeakRef churn) while the other allocates from the same directories, under the amplifier and TSan.

Expected gain: string-heavy one-thread peak 478 -> about 100 MB (flag off 88); the stop loses the cell sweep of weak-bearing
blocks (0.1-0.6 ms per stop on the workloads measured, N times that with N threads); structure-heavy programs stop paying
first-touch faults for blocks they already own (the run with no budget executes 1,534-1,611 ms against 1,721 ms,
indicative).

Alternatives rejected: a larger fixed budget (any constant is below some program's production rate, and the cost is
in the stop); sweeping weak-bearing blocks on the owner thread only (there is no owner: any thread may hold the `Weak<>`).

#### D-J3. More than one concurrent window when several mutators run - **Status: proposed, measure first**

The cap of one hand-off per cycle was set when a re-entry cost about 9 ms at sixteen threads (SPEC-congc section 7.1a).
A stop now costs about 0.04 ms per running mutator (J12), and at four and eight threads the second window is the long
one (2.4-5.0 and 5.7-8.6 ms). Proposal: `runFixpointPhase` may schedule a further Concurrent phase while (a) at least two
mutators would run in it, (b) the fixpoint's drain after the constraint pass has already run for the scheduler's target
pause and the shared stacks are not empty, and (c) fewer than three hand-offs were made this cycle. Everything else is
section 7.1a verbatim (the C1 machinery handles any number of windows; the cap is the only thing that changes).
Expected: stopped time per Eden cycle at eight threads about 9 -> 4 ms (three windows of 1-1.5 ms), longest pause 8.6 ->
about 3 ms, that is about 2 % of splay-like's leg. It needs the measurement the ninth round's tools already make (the
scaling gate plus `--logGC`), and nothing else; if the gain is not there the cap stays.

#### D-J4. Thread-local nursery collections - **not proposed**

Collecting one thread's young objects without stopping the others needs the invariant that no other thread can reach
them. In a shared-memory object model any plain store publishes an object, so the invariant needs a publication barrier on
every pointer store in every tier and in `WriteBarrier<>::set` (is the value young and owned by this thread, and is the
target visible to others?), which on publication must make the value and everything young it reaches global (pinned in
place, since the collector does not move and stacks are scanned conservatively) before the store completes: an unbounded
transitive walk on the publishing thread, per-thread mark bits and remembered sets for the young generation, and a second
liveness regime in `MarkedBlock`. Every generated store grows by a test the owner-tag test does not subsume (the tag names
the butterfly's owner, not the cell's age). The pauses this would remove cost 3-5 % of a parallel leg today (J4). Out of
proportion; recorded so that it is not re-derived.

#### D-J5. Finalizers found by another thread's sweep (Bun) - **Status: proposed as a measurement, then a rule**

First the counter of J11. If the delay is confirmed, the rule belongs to the embedder contract (SPEC-heap section 10G):
a native finalizer that frees memory only (no event-loop or I/O state, no thread-local lookups) is declared
thread-agnostic by its class and runs on the sweeping thread; only thread-affine teardown is posted. In Bun's generator
that is a per-class attribute defaulting to "post". JSC's side is unchanged. With D-J1 the conducting thread is more
often not the VM's thread, so the share of posted finalizers rises unless this is done.

### J: arm64 / non-Linux notes

- The heap-access protocol (per-client access byte, GSP, the section 10.4 barrier, the cooperative parked-root snapshot)
  is written with seq_cst operations on both sides (F7/F8); nothing in it leans on x86-64 store order. The snapshot's
  validity window is argued from program order on the publishing thread plus seq_cst publication; arm64 needs no change.
- `mutatorShouldBeFenced`: on x86-64 the flag follows marking for a shared server whose marking is inside the stop; a
  GIL-off process and every weakly ordered target keep it raised from the sticky flip (`setMutatorShouldBeFenced`). On
  arm64 this is what orders a new cell's initialization before its publication to a concurrent marker and to another
  mutator; the JIT's allocation paths emit the store-store fence under that flag. No change, but the arm64 bring-up must
  run the GIL-off corpus with `--verifyGC=1`, which is where a missing fence shows.
- Relaxed counters (`m_nonOversizedBytesAllocatedThisCycle`, `m_distinctAllocatingClientsThisCycle`, the per-client
  latches) are advisory between safepoints and exact at them through the stop's barrier, on any architecture.
- Directory bit vectors read without the bit-vector lock under a refill stripe rely on the stripe's lock for the resize,
  not on ordering.
- D-J1's prediction fields and D-J2's per-set flag are written inside a stop and read after it; the stop's close orders
  them (seq_cst), as for every other in-stop result.
- Conservative scanning of a running thread uses signals on Linux, `thread_suspend` on Darwin and `SuspendThread` on
  Windows (`MachineThreads::tryCopyOtherThreadStacks`); parked clients bypass all three. The service conductor adds a
  thread that is never scanned as a mutator (it is the conductor, scanned through `m_currentThreadState`). macOS and
  Windows need the park and wake primitives under the stop checked, not anything in these designs.

### J: Decisions for the user

1. **Build the service conductor (D-J1)?** Options: (a) as proposed, adaptive, with the lone-conductor rule; buys splay
   (the largest row below 0.80 that is not a Double or visibility item), about +1 % JetStream GIL off, the conducting
   thread's share of every multi-threaded run, Bun's main thread and Debug lanes; costs a medium C++ change with the full
   collector battery, and a Bun patch change (marshalling predicate). (b) only the lone-conductor rule (rule 3): a few
   lines, splay +4 %, nothing else. (c) leave. Recommendation: (a); (b) first if the session is short.
2. **Weak-bearing blocks (D-J2):** the stopgap alone (small, fixes the 5x memory on structure-heavy programs, adds up to
   0.6 ms to a stop) or the two-half sweep (removes the in-stop cell sweep altogether; touches the weak-mutation
   protocol's carve-outs and needs the amplifier and TSan). Recommendation: stopgap in the next session, two-half sweep
   with its own campaign after it.
3. **A memory gate for the scaling suite.** The 478 MB of J6 went unseen because the gate measures time only.
   Recommendation: record peak resident set per cell in the scaling gate and fail a cell above 2x the flag-off serial
   peak plus one nursery per thread.
4. **D-J3** is a measurement, not a decision; **D-J4** is a decision not to.

### J: Doc mismatches

- LANDING-PLAN Open items, "GIL off below GIL on", item (5), and PERF-RESULTS section 6.12 "Collection latency on the main
  thread (splay, gbemu, ML Worst Case)": only splay is collection latency. ML has the same 80 ms of collections in both
  modes (J8); gbemu's difference is 28 extra collections made by array growth that copies (J7). Fix: name splay (and the
  long-cycle share of pdfjs), move gbemu to item (3), drop ML.
- LANDING-PLAN "Measured and not adopted" and the same Open item: "the second client it creates engages the client-count
  heuristics of the allocation limits, and FlightPlanner, earley-boyer, hash-map and Babylon Worst fall". With retention
  off the collection counts do not rise (J2); the loss is 0.3-0.5 ms more stopped time per short cycle. Fix: say so, and
  that with retention on the client-count gate that mattered was the retention constraint's own.
- SPEC-heap section 10E, third amendment: "with n such blocks each is swept within ceil(n / budget) cycle ends" is true for
  a fixed n only; a program that makes more than 32 weak-bearing blocks per cycle grows without bound (J6). Fix: state the
  limitation until D-J2 lands.
- LANDING-PLAN Open items, "scaling-suite memory at eight threads: see PERF-RESULTS section 2": that section has no memory
  figures. Fix: J9's table, and the string-heavy row.
- SPEC-heap's header says rev 14 while section 10F is marked r15 and section 3 deviation 4 still says concurrent marking
  is disabled when shared; GIL off one concurrent window per cycle is live (SPEC-congc section 7.1a). Fix: a sentence in
  deviation 4 pointing at 7.1a.
- SPEC-congc cites `Heap.cpp` by line throughout (for example `conductSharedCollection` at 4830; it is near 7100 now), and
  the C2-C4 stage options exist in `OptionsList.h` with nothing but the `sharedGCWindowedConductActive()` predicate reading
  them. Fix: cite functions; say the three options are reserved.
- Code comments, not documents: `Heap::updateAllocationLimits` and `Heap::didAllocate` say "isSharedServer() is false at
  W=1" and "single-client GIL-off takes the landed branch"; a GIL-off server is shared from VM construction, so those arms
  run at one thread too (they are guarded by `numClients >= 2`, so behaviour is as described, the stated reason is not).

### J: What was run

All on existing binaries, no build. `--logGC=1` single JetStream tests (splay, gbemu, ML, hash-map, earley-boyer,
FlightPlanner, Babylon, json-parse-inspector, Basic, pdfjs) GIL on and GIL off on the final binary; the same six tests
plus cdjs on the experiment binary with `--useSharedGCServiceConductor=0/1` and retention 0/1; `perf record -e
instructions:u` on splay (both modes) and on `scaling/raytrace-like.js` at four threads; `--reportJSThreadsCounters=1` on
gbemu and on three scaling workloads at 1/2/4/8 threads; `--logGC=1` on `scaling/splay-like.js`, `raytrace-like.js`,
`string-heavy.js` at 1/4/8 threads, and splay-like at eight threads with `--numberOfGCMarkers=4/16` and
`--sharedGCMaxSiblingMarkingAssists=8`; string-heavy with `--sharedGCWeakBearingSweepBudget=32/256/0` (log, peak resident
set, `perf stat -e instructions:u`); peak resident set of four scaling workloads in three modes; two micro scripts
(stop latency with N spinning threads; cost of a requested collection, Release and Debug); one continuous-collection
stress file in four configurations. A first batch of experiment-binary runs was taken through a shell variable that the
shell did not split, ran GIL on, and was discarded and retaken with the variables exported; every GIL-off log used here
shows the closed-loop shape (`a=0kb`, two windows per cycle) or was checked with `--dumpOptions=1`.

## Section K. Tier-up of one shared function under N threads (GIL off)

Scope: `scaling/richards-like.js` (0.54x at two threads, 1.15x at four, 1.71x at eight), `scaling/string-heavy.js`
(bimodal at four threads: 2.4x or 0.9x), the bound of `vmstate/loop-entry-when-replacement-is-ftl-gil-off.js` (1,072
and 1,601 refused entries against 1,000), and the LANDING-PLAN open item "Tier-up of one shared function under N
threads". All numbers are from the round-ten final tree, Release, Linux x86-64; counts (exits, entries, checkpoints,
instructions) do not depend on load. The withdrawn generic-`PutById` rule's BadCache storm is treated elsewhere; it
runs through mechanism K2 below.

Notation. `r` is a function's reoptimization retry counter (`CodeBlock::m_reoptimizationRetryCounter` of the Baseline
block; every counted jettison adds one; nothing ever lowers it). `N` is `ThreadManager::liveSpawnedThreadCountApproximate()`.
`s` is `CodeBlock::optimizationThresholdScalingFactor()` (2.92 for `richardsWorkload`, 2.54 for `stringWorkload`).
A "back-edge" is one executed `loop_hint`. Baseline code reaches `operationOptimize` every 1,000 counted back-edges
once its threshold is crossed, DFG code reaches `triggerOSREntryNow` every 30,000: with `--verboseOSR=1` the number of
"Entered optimize" lines times 1,000 and of "Entered triggerOSREntryNow" lines times 30,000 is the number of
back-edges a function executed in each tier, which is how tier residency is measured below.

### K: Inventory

#### K1. A thread body is entered once and changes tier only through loop entry; after one jettison the way back costs 4^r (all configurations; explained)

Both workloads are one function called once per thread with all the work in loops inside it - the normal shape of a
`new Thread(fn)` body. Such a frame can reach optimized code only by loop OSR entry (Baseline -> DFG at a `loop_hint`,
DFG -> FTL through an `FTLForOSREntry` block). Every mechanism below is a way in which that path becomes expensive
once the function has a history.

`richards-like` makes it visible on `main`: the workload creates its constructors and helpers as closures on every
invocation, so every invocation of the installed FTL function-entry code exits in the prologue (GIL off:
`BadConstantValue` at the first `new Task(...)`, bc#152 - the callee is a fresh closure; on `main` flag off a `BadCache` at
the first access to a fresh packet's `a2` array, one per invocation, at bc#590, 616, 646 or 790) and lands in Baseline with a 25-million-step loop ahead of it. Five
consecutive invocations in one process, flag off, on `main`: 365, 535, 684, 1,294, 2,498 ms (the last is the whole
invocation in Baseline: no compilation is logged during it); the branch flag off: 369, 621, 715, 1,380, 2,513 ms. GIL on
the third invocation is already at 1,409 ms; GIL off, main thread, the third to fifth are 3,640-3,675 ms.

The mechanism, from `operationOptimize` (`jit/JITOperations.cpp`) and `CodeBlock::adjustedCounterValue` /
`adjustedExitCountThreshold` (`bytecode/CodeBlock.cpp`):

- The Baseline frame's loop trigger finds an FTL replacement. `DFG::prepareOSREntry` cannot enter an FTL block; GIL off
  the superseded DFG block kept by `ScriptExecutable::installCode` (`CodeBlock::gilOffDFGForLoopEntry`) is tried first
  and its entry fails validation (the block's entry expectations were recorded for the previous invocation's scope
  and closures). The failure is charged to the FTL replacement (`optimizedCodeBlock->countOSRExit()`), and the
  Baseline counter is set to `optimizeAfterWarmUpIgnoreQuickTierUp()` = 1,000 x s x 2^r back-edges before the next
  attempt.
- The replacement is jettisoned when its exit counter reaches `exitCountThresholdForReoptimizationFromLoop()` =
  5 x (1 + N) x 2^r.
- So the frame executes 5 x (1 + N) x 2^r x 1,000 x s x 2^r = 5,000 x s x (1 + N) x 4^r back-edges in Baseline before it
  may even request a DFG compile. After the harness's two warm-up invocations `r` = 5 for
  `richardsWorkload` (2 at the end of the first, 5 at the end of the second, read from the counter the verbose log
  prints at every checkpoint; over the workload's five functions the process counts nine `UnprofiledWatchpoint`
  jettisons - the single-instance inferences "Allocated a scope" / "Allocating a function", fired when the second
  invocation allocates a second scope and second closures - and four `OSRExit` ones), which gives
  5 x 2 x 32 = 320 failures x 93,500 back-edges = 29.9 M for one thread. Measured on
  one thread GIL off: 319 "OSR failed" lines, then one "Triggering reoptimization ... (in loop)", 30,271 Baseline
  checkpoints (30.3 M back-edges) of the invocation's ~130 M. With two threads: 478 failures (5 x 3 x 32 = 480), 45,993
  checkpoints (23 M per thread).
- After the jettison `r` = 6: the DFG compile is requested after another 187,000 back-edges, and the DFG code's
  FTL tier-up threshold is `thresholdForFTLOptimizeAfterWarmUp` x s x 2^r = 64,000 x 2.92 x 64 = 11.97 M back-edges
  ("finalThreshold=11966829" in the log), counted on a counter all threads share. One thread: 497 DFG checkpoints
  (14.9 M back-edges); two threads: 2,159 (64.8 M in sum).

Worth: `richards-like` T(1) GIL off is 1,250-1,300 ms where a fresh copy of the same function (no history) runs the
same work in 485 ms (K7); the same 4^r law is what `main` shows flag off.

#### K2. GIL off, exits of the blocks a Baseline loop actually enters are not the ones the loop trigger consults (GIL off; explained)

`operationOptimize` decides whether to reoptimize from `replacement->shouldReoptimizeFromLoopNow()` - the exit
counter of the installed replacement - and then, GIL off with an FTL replacement, enters
`codeBlock->gilOffDFGForLoopEntry()` instead; from that DFG block the loop tier-up enters its `FTLForOSREntry` child.
Exits taken in those two blocks are charged to their own `m_osrExitCounter` (`handleExitCounts`,
`dfg/DFGOSRExitCompilerCommon.cpp`), which for the outermost frame compares against the non-loop threshold
`exitCountThresholdForReoptimization()` = 100 x (1 + N) x 2^r, baked into the exit stub when it is first compiled. The
replacement never runs, its counter stays at 0-1, and the 5x loop trigger never fires. Flag off and GIL on the block
entered from the loop is the replacement (or the entry into an FTL replacement fails and the failure is charged to
it), so the loop trigger sees the exits.

Evidence on `string-heavy`, one thread, the harness's second warm-up invocation on the main thread (exits by code
block, `--printEachOSRExit=1`):

| configuration | `Overflow` exits at bc#247 in `stringWorkload`'s FTL loop-entry code | second invocation |
|---|---|---|
| flag off | 4 blocks, 1 exit each (r = 2, 3, 4, 5) | 1,308 ms (first 1,046) |
| GIL on | 4 blocks, 2 + 1 + 1 + 1 exits | 1,404 ms (first 1,137) |
| GIL off | 1 block, 801 exits (= 100 x 1 x 2^3 + 1), then a second block, 166 exits | 2,155 ms (first 1,451) |

Each of the 801 exits is followed by a Baseline stretch of 1,000 x s x 2^3 = 20,300 back-edges: 16 M of the
invocation's 31 M back-edges in Baseline. The same signature with four threads is K4.

#### K3. An `FTLForOSREntry` block can fail on every entry and nothing counts that (upstream behaviour, amplified GIL off; explained)

The exit K2 counts is `D@865 ArithDiv(Int32, Int32, CheckOverflow, bc#37, exit: bc#247, WasHoisted)` in the FTL graph
dump: the `1 / expected` of the corpus helper `shouldBe` (`JSTests/threads/resources/assert.js`), inlined into
`stringWorkload`, inside `else if (expected === 0)` - a branch that never executes, so the division has no profile
and is speculated Int32. In the function-entry compilation `expected` is the constant 48 and the branch folds away.
In the loop-entry compilation the local comes from the OSR entry buffer, the branch stays, the division is
loop-invariant, and `DFGLICMPhase` hoists it blindly (`canSpeculateBlindly`: no `HoistingFailed` exit site recorded)
into the pre-header of the outer loop - which in a loop-entry compilation is the OSR entry block. 1/48 is not an
integer: the block exits at its entry, every time it is entered.

The recorded remedy does not engage: a hoisted exit is remembered as `HoistingFailed` only through
`OSRExitBase::considerAddingAsFrequentExitSite`, which runs from `operationTriggerReoptimizationNow`, i.e. only when
the exit-count threshold is crossed. When the block is replaced through the Baseline loop trigger instead (flag off,
GIL on: four consecutive blocks, each exiting once, each followed by a counted jettison) nothing is learned and the
next block hoists the same node. `tierUpCommon` (`dfg/DFGOperations.cpp`) has an entry-failure policy
(`ftlOSREntryFailureCountForReoptimization` = 15) but an entry that succeeds and exits on its first instruction is
not an entry failure.

Worth: on `main` one counted jettison (one doubling of every threshold) per invocation; GIL off, through K2, 800 to
32,000 entries that each cost a Baseline warm-up.

#### K4. string-heavy's slow mode: one non-pure stop knocks every thread into Baseline, and K2 + K3 keep them there (GIL off; explained, one sub-question open)

Ten four-thread runs with `--reportJSThreadsCounters=1`: the four runs at 2.7 s have no `jettison[VMTraps]`; the six
slower ones (3.4-6.7 s) have 3, 3, 3, 3, 6 and 18, equal to `heapFactRewriteOnStackJettison`, and each of those runs -
and none of the fast ones - has one to seven `classA fire: Did cache property replacement` /
`Property did get replaced` stops. Ten runs with `--printEachOSRExit=1` and instruction counts: 971 exits / 74.3 G
instructions (fast, six runs; all 971 exits are the main thread's K2 exits of the second warm-up), 1,141-1,314 exits /
82 G, 2,073-2,182 exits / 96-111 G (slow).

In a fast run the threaded phase contains no tiering event at all: the four threads enter the installed FTL
function-entry code and stay in it. In a slow run (verbose log of one): a property-replacement set with watchers
fires from an inline-cache slow path early in the threaded phase; `JSThreadsSafepoint::stopTheWorldAndRun` bumps the
conductor heap-fact rewrite epoch for every window that is not declared code-lifecycle-only; the three parked threads
each run `VMTraps::jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite` and jettison the shared FTL block
(counted: `r` 5 -> 6). All four frames are now in Baseline mid-loop, and from there each repeats: 162,793 Baseline
back-edges (1,000 x 2.54 x 2^6), entry into the superseded DFG block, entry into its `FTLForOSREntry` child, the K3
exit at the child's first instruction. Counted in that run's threaded phase: 1,361 entries into the superseded DFG,
1,380 DFG -> FTL entries, 1,189 + 172 `Overflow` exits at the two loop headers, 147,868 Baseline checkpoints - the
whole phase (4 x 31.5 M back-edges) in Baseline. The child's threshold is 100 x 5 x 2^6 = 32,000 exits; a run has room
for about 1,400.

The same happens to a fresh copy of the function with no history (K7: one fire, three on-stack jettisons, 664 to
3,866 entries that each exit; 3.2 to 7.9 s): a high `r` is not required, only K2 + K3.

Open sub-question: which watchers the fired replacement sets have (a set without members fires without a stop,
SPEC-jit history §36), and why an inline cache caches a Replace case for the first time seconds into a run whose
caches were warmed twice. It decides only how often the trigger occurs, not what follows.

SPEC-jit history §41 recorded that watched fires did not jettison parked threads' frames; on this tree they do (see
Doc mismatches).

#### K5. Shared mutable state in Baseline code and in shared stubs (GIL off; explained in kind, split between its parts not measured)

`perf stat` on whole runs of `richards-like`, GIL off: instructions per spawned thread are the same at one, two and
four threads (14.8 G, 14.8 G, 13.9 G: 43.7 G, 58.5 G, 84.5 G per process); cycles are not (9.6 G, 38.1 G, 63.7 G per
process; thread CPU time 1.29 s -> 4.6 s each). The threads execute the same code 3.5 times slower, with no stop of
any length (`stwMs` 0.95 for the run) and no collection.

With symbolized samples (the JIT dump option of the shell and `perf inject --jit`), cycles of one spawned thread, one
thread against two: Baseline `richardsWorkload` 1.49 G -> 6.0 G for 30 M and 23 M back-edges (50 -> 260 cycles per
back-edge); "GetById Load handler" 0.39 G -> 1.0 G; "PolymorphicCall" 0.06 G -> 0.93 G; FTL `richardsWorkload` 1.69 G
-> 2.05 G. With the DFG disabled (Baseline emits no profiling and no counters) the Baseline function does not slow
down at all (3,596 -> 2,578 samples per invocation) and the polymorphic call stub goes from 144 to 6,400 of 12,000
samples per thread: that run is 3.9 s on one thread and 6.6 s on two with 53 % of each thread in one 0x20-byte
slot.

What is written by every thread, per execution, into memory all threads read:

1. *Value-profile buckets.* `JIT::emitValueProfilingSite` (`jit/JITInlines.h`) has the seventh round's
   write-avoidance (compare, store only a new value) inside `#if USE(JSVALUE64)`. Upstream removed that macro
   ("Remove 32-bit JSValues"); it is defined nowhere in the tree and this is its only remaining use, so the block is
   compiled out: the annotated Baseline code has 67 unconditional `mov %rax, -N(%r12)` and no compare, with
   `--useSharedProfileWriteAvoidance` 1 or 0 alike; the Debug binary's `emitValueProfilingSite<OpGetById>` calls
   neither option accessor while `emitArrayProfilingSiteWithCell` next to it does. Four 16-byte profiles share a
   line; `richardsWorkload` stores about ten per step.
2. *The Baseline execution counter*: `addl $1, 0x60(%r13)` per `loop_hint` on the shared `BaselineJITData`
   (`emit_op_loop_hint`), about five per step. The DFG tier-up counter (`DFG::JITCode::tierUpCounter`, one per DFG
   block, incremented by `CheckTierUpInLoop`) is the same thing one tier up.
3. *Polymorphic call stub slot counts*: `add32 1, CallSlot::count` in the polymorphic call thunk
   (`jit/ThunkGenerators.cpp`), for every non-top-tier call through a shared stub; the slot's callee word, which the
   dispatch loop of every thread reads first, is in the same line (89.7 % of the stub's samples are on the compare
   after that load).
4. Array profiles and IC state cells have write-avoidance and do not show.

Worth: every phase in which N threads run one function's Baseline (or DFG) code costs about five times the
single-thread cycles per operation at two threads. With K1-K4 that is most of each thread's time; with them fixed it
is the tier-up transient only.

#### K6. The live-thread multiplier charges threads that share nothing (GIL off; explained)

`exitCountThreadMultiplier()` = 1 + N multiplies both reoptimization thresholds for every optimized block in the
process, whether or not more than one thread runs it, and also the entry-failure count of K1, which is not a
speculation failing on N threads but one frame failing to enter. Private copies of `richardsWorkload` (one function
object per thread, each warmed twice on the main thread so that r = 5; nothing shared): 1,266 ms on one thread, 2,392 on
two, 3,555 on four - each thread alone must now produce 5 x (1 + N) x 2^r failures, 30 M / 45 M / 75 M Baseline
back-edges. The multiplier was introduced for one case (N threads taking the same exit once each; string-heavy 23
jettisons -> 6).

#### K7. What the engine does when nothing is shared and nothing has a history (reference)

| `richards-like`, GIL off, ms | 1 thread | 2 | 4 |
|---|---|---|---|
| private copy per thread, no warm-up | 487 | 553 | 502 |
| one shared copy, no warm-up | 484 | 667 | 1,362 |
| private copies, each warmed twice (r = 5) | 1,266 | 2,392 | 3,555 |
| one shared copy warmed twice (= the harness) | 1,248 | 4,459 | 4,185 |

| `string-heavy`, GIL off, ms | 1 thread | 4 |
|---|---|---|
| private copy per thread, no warm-up | 1,443-1,477 | 2,608-2,658 |
| one shared copy, no warm-up | 1,434-1,451 | 2,693 / 4,262 / 7,712 |

Private code scales (3.9x at four threads for `richards-like`; `string-heavy`'s 2.2x is its strings, atoms and
collections, not tiering) and is stable. Everything between the first row and the last is K1-K6.

#### K8. The bound of `vmstate/loop-entry-when-replacement-is-ftl-gil-off.js` (GIL off; explained)

A refusal (`loopEntryRefusedReplacementIsFTLGILOff`) is a failed entry while no superseded DFG block is recorded; the
lock-out ends when the FTL replacement's counter reaches 5 x (1 + N) x 2^r (K1). With four spawned threads that is
799 refusals for r = 5 and 1,599 for r = 6; the recorded excesses are 1,601 (ninth round, under ThreadSanitizer) and
1,072 (tenth, 800 plus part of a second episode). The bound of 1,000 sits between r = 5 and r = 6; whether a run
reaches r = 6 depends on how many on-stack jettisons its stops produce (K4). Not a margin that load moves
continuously, as the test's comment and the open item say, but a step function of `r`.

### K: Designs

Order of value: D1 (one line), D2 + D3 (the policy; they remove the 4^r law and the K2/K3 cycle), D4 (the trigger),
D5 (shared counters), D6 (back-off hygiene). D2-D4 and D6 are GIL-off only; D1 and D5 are flag-on only. None changes
what flag off executes or compiles.

#### D1. Compile the value-profile write-avoidance back in

**Status: proposed.**

Rule. Remove the `#if USE(JSVALUE64)` / `#endif` pair around the write-avoidance arm of
`JIT::emitValueProfilingSite(const Bytecode&, BytecodeIndex, GPRReg)`; the arm stays behind
`Options::useJSThreads() && Options::useSharedProfileWriteAvoidance()`. Writers: Baseline code of every thread, one
compare and a conditional store per profiled bytecode. Readers: the compiler threads' prediction updates, as today
(SPEC-jit §5.7 item 4: a bucket is one aligned word, torn or stale values select, guards validate).

Memory ordering. None needed on x86-64 or arm64: the bucket is advisory; the compare and the store are relaxed
accesses to one aligned 64-bit word; a lost update or a stale compare only delays a prediction by one execution.

Collector, stops, watchpoints: untouched (the bucket holds a JSValue the conservative scan or the profile's own
visit keeps alive exactly as before; a skipped store never removes a reference that a performed store would have
kept).

Flag off: the emitted code is the plain store, byte for byte (the arm is not taken; it is already compiled for the
array-profile sites). GIL on: G1 runs `main`'s object model but the profile stores were never part of it; the arm is
keyed on the flag, so GIL on gets the compare too (one instruction pair per profiled bytecode in Baseline; if that is
unwanted the key can be `useTaggedButterflies`, at the cost of GIL-on handoffs bouncing the lines - irrelevant, one
thread runs at a time).

Failure mode: none that is new; the option already exists to switch it off.

Tests. `JSTests/threads/jit/` new file counting nothing at run time (a Baseline-held two-thread loop has no
load-independent observable); the verification is structural: a lint in `Tools/threads` (or a `static_assert`-style
check) that `USE(JSVALUE64)` and `USE(JSVALUE32_64)` appear nowhere in `Source/JavaScriptCore`, because the macro
is gone and any use silently evaluates to 0. Measurement: `perf stat` cycles per thread of `scaling/richards-like.js`
held in Baseline (`--useFTLJIT=0 --thresholdForOptimizeAfterWarmUp` raised) at one and two threads, before and after.

Expected gain. Part of K5's factor of five on Baseline-held phases at two threads (the part that is not the
execution counter; the annotated samples are spread evenly over the profile stores and the counter). The seventh
round measured the array-profile and IC half of the same idea at 2,594 -> 1,894 ms on Baseline-held string-heavy.

Risk: negligible. Alternative rejected: leaving it and relying on D2/D3 to keep threads out of Baseline - the
tier-up transient of every shared function still runs in Baseline on all threads at once.

#### D2. Loop entry has its own artifacts, its own accounting, and no exponent

**Status: proposed.** Replaces the eighth round's "keep the superseded DFG" (SPEC-ungil history, "loop entry into
the superseded DFG code") by the rule it was a special case of.

Rule (GIL off). A Baseline CodeBlock has a *loop-entry slot* (`BaselineJITData::m_gilOffDFGForLoopEntry`, existing)
that names a DFG block frames may enter at loops while the function's replacement is something they cannot enter.

1. *Who fills it.* (a) `installCode`, when an FTL replacement supersedes a live DFG block (existing). (b) New:
   `operationOptimize`, when it is at a loop, the replacement is optimizing code it cannot enter (an FTL block, or a
   DFG block whose `prepareOSREntry` failed validation for this frame), and the slot is empty or its block's entry
   failed validation for this frame `k` = 2 times in a row: it starts a DFG compile for loop entry
   (`DFG::compile(vm, codeBlock->newReplacement(), nullptr, DFG mode, bytecodeIndex, mustHandleValues(frame), callback)`)
   under the existing `TierUpEdgeLocker(BaselineToDFG)` and the worklist's key dedup. The callback is a new
   `DeferredCompilationCallback` whose `compilationDidComplete` does not call `installCode`: if the function's
   replacement is still valid optimizing code it stores the new block into the slot (release store), else it installs
   the block as the replacement as today. Its Baseline threshold handling is `optimizeNextInvocation()` /
   `optimizeSoon()` as for a normal DFG completion, so the requesting frame retries at its next checkpoint.
2. *Who reads it.* `operationOptimize` at a loop, before trying the replacement (existing order), any thread.
3. *Accounting.* A failed entry (validation failure or "not DFG") is an *entry failure*, not an exit: it is counted
   in the slot's block if that is what failed (new field `m_loopEntryFailures` in `DFG::JITCode`, relaxed) or, with
   an empty slot, in a counter of the Baseline block, never in any block's `m_osrExitCounter`. The retry spacing
   after an entry failure is `thresholdForOptimizeAfterWarmUp x s` without the 2^r factor (the factor exists to space
   recompilations; a retry compiles nothing). Entry-failure thresholds are constants (`k` above; an upper bound of
   `ftlOSREntryFailureCountForReoptimization` = 15 after which the slot's block is dropped) and carry neither 2^r nor
   the thread multiplier: N frames failing to enter are N frames that need an entry, not one speculation failing N
   times.
4. *Exits of the entered blocks.* When `operationOptimize` is about to enter the slot's block it consults *that
   block's* `shouldReoptimizeFromLoopNow()` and, if the block has an `osrEntryBlock()`, the child's counter against
   the same loop threshold (5 x 2^r', see 6), instead of the replacement's. Tripped: the slot is cleared and the block
   (with its child, through `clearOSREntryBlockAndResetThresholds`) is jettisoned, its exits having been offered to
   `considerAddingAsFrequentExitSite` first (so K3's `HoistingFailed` is learned); the next checkpoint takes 1(b).
5. *The replacement is left alone.* Frames that are in Baseline mid-loop no longer jettison a function-entry block
   that the next caller could use: `replacement->countOSRExit()` for a failed loop entry goes away GIL off, and with it
   the lock-out of K8 (`loopEntryRefusedReplacementIsFTLGILOff` counts only the checkpoints between the first failure
   and the completion of 1(b)'s compile).
6. *Back-off for the slot.* A slot block dropped under 3 or 4 increments a slot-local retry count r' (in the Baseline
   block, saturating at 4) that scales 1(b)'s `k` and 4's threshold by 2^r'. It does not touch `r`: dropping a
   loop-entry artifact changes nothing about the function-entry code's quality. r' is reset when a slot block
   survives one full `thresholdForFTLOptimizeAfterWarmUp` of execution.
7. *The DFG block in the slot tiers up through its loops only.* `DFG::JITData::neverExecutedEntry()` stays true for a
   block that was never entered at its prologue, and `tierUpCommon` already skips `triggerFTLReplacementCompile` for
   such a block ("avoiding replacement compile"); 1(b)'s blocks rely on that and must not set the flag false when
   entered by OSR. Its FTL tier-up threshold uses r' in place of `r` (K1's 12 M back-edges become
   64,000 x s x 2^r').

Tiers. LLInt: unaffected (LLInt loop tier-up goes to Baseline). Baseline: the `loop_hint` slow path calls the same
operation. DFG / FTL: no emitted-code change; `triggerOSREntryNow` unchanged except 7's threshold. C++: the operation,
the new callback, `CodeBlock::jettison`'s existing slot clear, `CodeBlock::visitChildren` of the slot (existing).

Memory ordering. The slot is one pointer. Publication of a freshly compiled block (1(b)) is a *release* store after
`Plan::finalize` has completed under the worklist lock; readers load it with a consume/acquire load and dereference
only through it (`isJettisoned()`, `jitType()`, the OSR entry data of its `JITCode`). x86-64: plain `mov` both sides.
arm64: `stlr` on the writer; the reader's address dependency suffices for fields reached through the pointer, but
`prepareOSREntry` also reads the block's `DFG::CommonData` and the entry-point vectors through further pointers
written before the release, so dependency chains cover them; use `loadAcquire` rather than argue each chain. Today's
`storeRelaxed` / `loadRelaxed` in `setGILOffDFGForLoopEntry` / `gilOffDFGForLoopEntry` is sound only because case (a)
publishes a block that was installed (fenced) long before; with (b) it must become release/acquire. Clearing is a
relaxed null store made world-stopped (jettison) or by the thread that drops the block under 3/4 before it requests
the jettison's stop.

Collector. The slot is visited by its Baseline owner (existing), so an uninstalled DFG block named by it is marked
while the Baseline block is; its `FTLForOSREntry` child is reached through `DFG::JITCode::m_osrEntryBlock` (a
`WriteBarrier`, existing). A block that lost the slot and is on no stack dies at the next collection like any
uninstalled code. `CodeBlock::finalizeUnconditionally` / `jettisonCodeBlockEdgeIfDead`: the slot's block is an "edge"
of the Baseline block and must be treated like the replacement edge there (if the End phase finds it dead it clears
the slot, world stopped; the existing clear in `jettison` covers the `JettisonDueToOldAge` arm with its `isMarked`
guard - keep that guard).

Stop protocol. Jettisons stay world-stopped (SPEC-jit §5.3, I8). 1(b) allocates a CodeBlock (`newReplacement`): a
park-capable allocation before any lock is taken, as in the existing compile path. A frame enters a slot block only
from `operationOptimize`, which holds `DeferGCForAWhile` from before it reads the slot until it has planted the
block in the frame (existing comment at the top of the operation): a jettison of the slot block needs a stop, the
operation polls nowhere between the slot load and the return into `osrEntryThunk`, so the block cannot be invalidated
in between; if it was invalidated before the load, `isJettisoned()` refuses it.

Watchpoints, deferred claims. A slot block registers the watchpoints any DFG block registers; their fire jettisons
it, which clears the slot. Nothing about claims changes.

What a second thread can observe mid-way. (i) The slot null while a compile is in flight: it fails its entry, counts
an entry failure, retries after an unscaled warm-up - bounded by the compile time. (ii) Two threads requesting 1(b)
at once: the edge locker admits one, the other sees `CompilationDeferred`. (iii) A slot block compiled from thread
A's frame values that thread B's frame fails to validate against: B counts its own failure; after `k` consecutive
ones B requests a replacement of the slot block compiled from its values. Two threads with incompatible frames could
alternate; r' bounds that (each replacement is a drop under 3), and the must-handle values merge into the block's
entry expectations as they do today (`AbstractValue::mergeOSREntryValue`), so the second block accepts both.
(iv) The replacement jettisoned while a slot compile is in flight: the callback installs the block as the
replacement (the ordinary path).

Flag off / GIL on: nothing. Every new branch is behind `vm.gilOffWithProcessGate()`, where the existing slot logic
already is; the new callback class is never constructed.

Failure modes and detection. A compile storm (slot blocks compiled and dropped in a loop): bounded by r' and visible
as `compileDFG` in the counters - the test below bounds it. A slot block that is never dropped although it exits on
every entry: exactly K2, which 4 removes; detected by the exits-per-entry ratio in the test. A leak of uninstalled
blocks: `--verifyGC` lanes and `$vm` code-block counts in the test after a full collection.

Tests. `vmstate/loop-entry-when-replacement-is-ftl-gil-off.js`: the bound becomes 50 refusals (the checkpoints of one
compile latency) and a new bound of 20 on Baseline checkpoints per thread after the first failed entry... stated as
counters, not time: `loopEntryRefusedReplacementIsFTLGILOff` <= 50, new counter `loopEntryCompileRequestedGILOff`
>= 1. New `jit/loop-entry-artifacts-have-their-own-accounting-gil-off.js`: the K2 reproducer (a function whose
loop-entry block exits on entry: an untaken branch holding a blind Int32 division by an OSR-entry local, the shape of
K3), one thread and four: before, 801 / 903 exits per invocation and a second invocation at 1.5x the first; after, at
most 15 + 5 x 2^r' exits and `numberOfDFGCompiles` bounded. `scaling/richards-like.js` and `scaling/string-heavy.js`
with `--verboseOSR`-derived checkpoint counts in a wrapper (Baseline back-edges per thread in the threaded phase:
23-30 M before for `richards-like`, < 1 M after).

Verification. Corpus in four modes, TSanJIT both modes (the slot and the new counters are racy on purpose: relaxed
atomics, annotate), the amplifier on the three tests, the GIL-off stress suite (the no-concurrent-JIT configurations
exercise the synchronous form of 1(b)), the scaling gate.

Expected gain. `richards-like`: the threaded phase loses 23-30 M Baseline back-edges per thread (of 130 M) and 17 M of
the DFG residency: T(1) 1,250 ms -> about 500 ms (K7's fresh-function row: the way back is then two compile
latencies); T(4) 4,200 ms -> between 500 ms and 1,360 ms (K7 rows 1 and 2; the rest is D5). Gate speedup at four threads
1.15x -> 1.5x without D5, 3x or more with it. `string-heavy`: with 4, a loop-entry block that exits on entry is dropped
after 5 x 2^r' exits and its exit site learned; the slow mode's 1,400 cycles per run become at most a few dozen: every
run lands at the fast mode's 2.4-2.5x. The main thread's second warm-up invocation 2,155 -> about 1,450 ms.

Risks. The change is policy in the most timing-sensitive code of the engine; the no-concurrent-JIT and
`--forceEagerCompilation` configurations (parked wait on the concurrent JIT, SPEC-jit history §56) must see the same
promise for 1(b)'s compile. A loop-entry DFG block per function roughly doubles DFG code for functions that are
entered at loops after an FTL install; they die at the next collection once unreferenced.

Alternatives rejected. (a) Only fix the counters consulted (4) and keep jettisoning the replacement on entry
failures: keeps the 4^r law of K1 and K8's lock-out. (b) Make the FTL replacement enterable from Baseline (an
FTL-for-loop-entry compile requested by Baseline): upstream has no Baseline -> FTL entry path; the must-handle values
would be Baseline-format; much larger. (c) Let Baseline frames enter `FTLForOSREntry` blocks of the superseded DFG
directly: same. (d) Reset `r` when a thread starts: hides K1 for the first invocation on a thread and nothing else.

#### D3. An `FTLForOSREntry` block that exits at its entry is an entry failure

**Status: proposed.** Closes K3 at both ends; independent of D2 (D2.4 bounds the damage, D3 removes the cause).

Rule, part 1 (recording; GIL off, could be offered upstream). `FTL::ForOSREntryJITCode` gains `m_entryCount` (relaxed
increment in `FTL::prepareOSREntry` on success). In `tierUpCommon`, before entering an existing `osrEntryBlock()`: if
`entryBlock->osrExitCounter() >= ftlOSREntryFailureCountForReoptimization` (15, unscaled) and at least half of the
block's entries ended in an exit (`2 x exits >= entries`), take the `failedOSREntry` "too many times" arm after
offering every exit of the block to `considerAddingAsFrequentExitSite(profiledBlock)`:
`clearOSREntryBlockAndResetThresholds`, then jettison the entry block (`JettisonDueToOSRExit`, world-stopped as
always). The DFG parent is kept. It counts toward D2's r', not toward `r` (`countReoptimization()` in that arm is
replaced GIL off).

Rule, part 2 (not hoisting; GIL off only because it changes what is compiled). In `LICMPhase::attemptHoist` (`dfg/DFGLICMPhase.cpp`),
`canSpeculateBlindly` is false in a plan whose mode is `FTLForOSREntry` when the pre-header being hoisted into is the
plan's OSR entry block (the block `m_graph.m_roots` names for the entry bytecode). A failing blind speculation there
fails at every entry, which is the one place where "it will be learned after the first failure" is false.
Control-equivalent nodes are still hoisted.

Memory ordering: counters only (relaxed; x86-64 and arm64 alike).

Collector / stops / watchpoints: part 1 is a jettison like the existing too-many-failures arm; part 2 is compile-time.

Second thread mid-way: two threads can both decide part 1 for the same block; `clearOSREntryBlockAndResetThresholds`
already absorbs the loser (the P0-osr-entry-toctou early return), and the jettison's "already invalidated" return
absorbs the second jettison.

Flag off: part 1 behind `gilOffWithProcessGate()` (the exits-per-entry test is one load and a compare in a slow
path); part 2 behind the same gate at compile time: flag-off and GIL-on plans hoist as `main` does.

Failure modes. Part 2 forgoes a legitimate hoist into the entry block: the node then executes per iteration until
the function-entry FTL code (which may hoist it into a real pre-header) takes over; visible as instructions per
iteration in loop-entry code only. Part 1 drops a block that exits for another reason at a high rate: that block was
not worth entering either.

Tests. `jit/ftl-loop-entry-block-that-exits-at-entry-gil-off.js`: the K3 shape (inlined helper with an untaken
branch dividing by an OSR-entry local). Counts `Overflow` exits at the loop header per invocation (`$vm`-free:
`numberOfDFGCompiles` and an exit counter via `--countJSThreadsCounters` `osrExitFTLOperation`): 801+ before, 0 with
part 2, <= 15 with part 1 alone. `scaling/string-heavy.js` main-thread second invocation: 967 exits before, 1 after.

Expected gain: with D2, string-heavy's slow mode is gone (K4); alone, part 2 removes this instance and part 1 bounds
every other instance at 15 entries.

Risks: part 2 is a change to an optimization phase; keep it to the entry block. Alternative rejected: recording
`HoistingFailed` at the first exit of a hoisted node for every plan (`operationCompileOSRExit` time): correct and
simpler, but it changes flag-off profiles and therefore flag-off code; proposed upstream separately rather than
carried behind a flag.

#### D4. A watchpoint fire's stop window is a code-lifecycle window

**Status: proposed (SPEC-jit history §41 re-opened with the measurement it lacked).**

Rule. The stop that `WatchpointSet::fireAllSlow` / `InlineWatchpointSet::fireAll` requests for a Class-A set with
members (SPEC-jit §5.6 steps 2-6), and the scope-exit fire of a deferred claim when it requests its own stop, run
under `JSThreadsSafepoint::PureCodeLifecycleStopWindowScope`: the window invalidates code and code-side caches and
rewrites no structure, indexing type, butterfly word or array storage, so it does not bump the conductor heap-fact
rewrite epoch, and mutators parked by it keep their optimized frames (those that watched the fired set are
jettisoned by the fire itself, inside the window). Not covered, and unchanged: a fire made *inside* a stop whose
conductor also rewrites heap facts (the publish-and-fire-in-one-stop transitions of SPEC-jit §5.6 "Deferred claims in
flight", `haveABadTime`, segmentation, a foreign first write's SW flip): the outer requester's depth is 0 there and
the bump stays.

Argument. The epoch exists for bystanders that hold a *heap fact* across a poll without watching anything: a hoisted
butterfly or vector/length pair (SPEC-jit history §39, §50). A watchpoint fire changes no object. What the firing
thread does after the window (the store that replaces a property, the transition that follows a claimed fire) is an
ordinary mutator action performed with every thread running, under the object model's own protocols, exactly as if
no set had been watched. AUDIT-checktraps row CA's handler-by-handler audit (every `Watchpoint::Type` handler
rewrites code or code-side caches) is the proof obligation and was done in the eighth round.

Memory ordering: none added; the suppression is a thread-local depth on the requester.

Second thread mid-way: a bystander parked by the fire resumes into its optimized frame at the poll's invalidation
point; if its code block watched the set, the invalidation point is patched and it exits there (existing); if not,
it continues with facts no one rewrote.

Flag off / GIL on: untouched (the epoch is read only GIL off).

Failure mode. A handler that does rewrite an object (a future `Watchpoint` subclass): caught by extending the debug
assertion the pure-window scope already has for `CodeBlock::jettison` - in Debug, a pure window asserts on exit that
the process-wide count of conductor object rewrites (the counters the conductor paths already bump:
`relabelOwnerLeg`, `lockedTransition`, segmentation, SW flips) did not move.

Tests. `jit/watchpoint-fire-does-not-jettison-bystanders-gil-off.js`: four threads in FTL loops that watch nothing
the main thread fires; the main thread fires twelve watched sets (replacement sets of a structure the threads never
see); `heapFactRewriteOnStackJettison` 3 per fire before, 0 after; the threads' results unchanged. §41's own test
must first be explained: it saw no jettison with or without the change, this tree shows 3 per fire in six of six
slow string-heavy runs (see Doc mismatches for the likely reason). `scaling/string-heavy.js` at four threads, ten
runs: `jettison[VMTraps]` 0 in all.

Expected gain: removes string-heavy's slow-mode trigger (six runs in ten with counters on, two to four in ten
without); with D2 + D3 in place the trigger costs a few compile latencies instead of the run, so D4 is then worth
the jettisons and recompiles themselves (each knocked-out thread: about 40 ms of DFG + FTL compile latency).

Risks: a soundness rule change, small in code, large in audit surface - which is why it was withdrawn once. It
should land after D2/D3, alone, with the amplifier on the corpus's `cve/mc-code-*` tests.

#### D5. Counters that N threads increment are not shared

**Status: proposed.** Three pieces, independent.

(a) *Polymorphic call stub slot counts saturate.* In the polymorphic call thunk (`jit/ThunkGenerators.cpp`), flag on
with tagged butterflies (GIL off): `load32 count; branch32(AboveOrEqual, count, 2^16) -> skip; add32 1, count`.
`CallLinkStatus::computeFromCallLinkInfo` uses the counts as relative weights with `frequentCallThreshold` = 2 and a
cap of five variants; after saturation every hot variant weighs the same, which keeps them all in the inlining set -
the information lost is the order among variants that each ran 65,536 times. After saturation the line is read-only
and stays shared in every core. The republication of SPEC-jit history §58 copies counts as it does now. Readers
(compiler threads) already take the link lock for the edge list; the count words are relaxed.
Gain: K5's DFG-off measurement is the ceiling - 6,400 of 12,000 samples per thread at two threads; in the tiered run
0.93 G of each thread's 12.5 G cycles.

(b) *Baseline and DFG execution counters are per thread, GIL off.* Each VMLite gets a fixed table of 1,024 (4 KB)
`BaselineExecutionCounter`-shaped slots; a Baseline CodeBlock and a DFG `JITCode` get a slot index at creation
(a hash of the CodeBlock pointer; collisions are tolerated - two functions sharing a slot tier up earlier or later
than they would, never incorrectly). GIL off `emit_op_loop_hint`, the prologue's optimization check (the `branchAdd32` on
`BaselineJITData::offsetOfJITExecuteCounter()` in `op_enter`'s shared thunk and its inline form), DFG
`CheckTierUpInLoop` / `CheckTierUpAtReturn` / `CheckTierUpAndOSREnter` address `lite + tableOffset + slot x
sizeof(counter)` through the lite register / thread-local base the tiers already have, instead of `jitData + offsetOfJITExecuteCounter`
or the absolute `tierUpCounter` address. The slow paths (`operationOptimize`, `triggerTierUpNow`,
`triggerOSREntryNow`) read and re-arm the *calling thread's* slot (`setNewThreshold`, `optimizeAfterWarmUp`, ...
take the lite); `forceOptimizationSlowPathConcurrently` (compiler thread: "compilation became ready") has no single
counter to poke any more: it sets a per-CodeBlock `m_optimizedCodeReady` byte that the slow path and - new - the
counter's threshold arming consult, and additionally each thread's next checkpoint (at most 1,000 Baseline / 30,000
DFG back-edges away) finds the ready plan through `completeAllReadyPlansForVM` as today. The tier-up decision itself
is per thread (each thread crosses its own threshold); the first to cross compiles for all (the edge locker and key
dedup exist); the others find the replacement at their next checkpoint.
Tiers: LLInt keeps its per-CodeBlock counter (LLInt residency is short: `thresholdForJITAfterWarmUp` = 500);
Baseline, DFG as above; FTL has no counters.
Memory ordering: a slot is touched by its own thread only, plus the rare cross-thread "ready" byte (relaxed store
by the compiler thread, relaxed load by mutators: advisory).
Collector / stops: slots hold integers.
Flag off / GIL on: the emitters keep today's addressing (the gate is the GIL-off predicate at compile time; GIL-on
processes have one running thread).
Failure modes: slot collisions distort thresholds (bounded by the table size; detectable by a debug counter of
collisions); a thread that never crosses a threshold because it runs a function rarely never triggers a compile for
it - correct by definition of "hot on this thread", but total executions across threads no longer add up: a function
called 900 times on each of eight threads stays in Baseline. If that matters, the slow path can fold a thread's
count into the shared total at each checkpoint (one shared write per 1,000 executions instead of per execution).
Gain: with D1, the rest of K5's factor on Baseline-held and DFG-held phases; also removes the lost-update inflation
of the DFG residency (two threads: 32 M back-edges each against a 12 M threshold).
Risk: touches every tier-up emitter; land after D2/D3 have removed most of the time spent in lower tiers, and
measure whether it is still needed.

(c) *Exit counters.* `m_osrExitCounter` and `OSRExitBase::m_count` stay shared (exits are rare once D2/D3 hold).

Tests for D5: `scaling/richards-like.js` with the DFG disabled at one and two threads, instructions and cycles per
thread (before: cycles x2 at equal instructions; after (a): within 1.1x); a Baseline-held two-thread loop with a
polymorphic call and the JIT dump, samples in the stub; the scaling gate.

#### D6. What counts toward `r`, and what the live-thread multiplier multiplies

**Status: proposed.** Small rules that remove the amplification in K4, K6 and K8 without touching the stock
exponential back-off for real recompilation.

1. *One on-stack heap-fact jettison episode counts once per Baseline block.*
   `jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite` passes `CountReoptimization` for the first optimized
   block of a given Baseline block jettisoned in a given epoch value and `DontCountReoptimization` for the others
   (the FTL function-entry block, its loop-entry sibling and the superseded DFG block of one function are three
   blocks of one Baseline block). The compile-storm case the counting was introduced for (two threads mutating one
   global: 48,000 DFG compiles in 40 s) still backs off: one count per episode is the stock rate.
2. *The multiplier applies to exits only, and only by the threads that exited.* `exitCountThreadMultiplier()` becomes
   a property of the optimized block: the number of distinct threads that have taken an exit from it (a 64-bit mask
   of butterfly TIDs modulo 64 in `DFG::CommonData`, or-ed in `operationTriggerReoptimizationNow`'s caller path; one
   relaxed `or` per exit), at least 1. A block only one thread runs keeps `main`'s thresholds however many threads
   are alive (K6: 2,392 / 3,555 ms -> 1,266 ms for private code). Because the exit stub bakes its threshold as an
   immediate when first compiled, the comparison against the scaled threshold moves into
   `operationTriggerReoptimizationNow` (the stub keeps the unscaled immediate; the operation returns without
   reoptimizing while `count < threshold x popcount(mask)`).
3. *Entry failures carry no multiplier and no exponent* (D2.3).
4. *`r` decays.* A Baseline block whose current replacement has executed `thresholdForFTLOptimizeAfterWarmUp x 2^r`
   back-edges without a counted jettison lowers `r` by one (checked where the DFG/FTL tier-up thresholds are re-armed).
   Upstream never lowers `r`; GIL off the jettison sources are more numerous (stops, claims, on-stack jettisons), and a
   long-lived thread body pays for its function's whole history at every recovery.

Memory ordering: relaxed counters and one relaxed `or`. Flag off / GIL on: gated like the existing multiplier
(`g_jscConfig.gilOffProcess`). Failure mode: compile storms - bounded by items 1 and 4 keeping the stock exponent in
force while jettisons keep coming; the seventh round's string-heavy measurement (23 jettisons -> 6) and the
two-threads-one-global reproducer are the regression tests, both by `compileDFG` / `jettison` counts.

Expected gain: with D2 in place these matter for the function-entry blocks only; alone, item 2 is K6's row (private
code back to 1,266 ms at any thread count) and item 1 keeps `r` from climbing by three per fire in K4.

### K: arm64 / non-Linux notes

- `BaselineJITData::m_gilOffDFGForLoopEntry`: today `storeRelaxed` / `loadRelaxed` and then a dereference. Sound on
  x86-64 by TSO; on arm64 sound today only because the published block was installed (fenced) long before it is
  stored into the slot and readers reach it through an address dependency. D2 publishes freshly compiled blocks:
  release store / acquire load required (listed in D2).
- Execution counters, exit counters, `m_reoptimizationRetryCounter`, `m_entryFailureCount`, call-slot counts, value
  and array profiles: relaxed by design (SPEC-jit §5.7); no ordering is relied on, on either architecture. The
  write-avoidance compare-then-store of D1 and D5(a) is not atomic and need not be.
- The exit stub's baked threshold immediate (K2) is read by no other thread.
- The heap-fact epoch compare on a parked thread's resume edge (D4 leaves its ordering untouched): `fetch_add` with
  acq_rel on the conductor inside the window, acquire load on the mutator after the resume publication; the argument
  in `JSThreadsSafepoint.cpp` is stated in terms of program order plus the stop word's release/acquire pair and holds
  on arm64 as written.
- JIT dump / `perf inject --jit` used for the measurements is Linux-only; the counters
  (`--reportJSThreadsCounters`, `--countJSThreadsCounters`) are portable and are what the proposed tests read.

### K: Decisions for the user

1. **Is a per-invocation-closure workload a fair scaling gate?** `richards-like` creates its constructors and
   helpers inside the workload function, so every invocation exits in the prologue of the optimized code, on `main`
   too (five runs flag off: 365 -> 2,498 ms). Options: (a) keep it as it is: it then measures K1-K6, which D2-D6 address,
   and its T(1) is the third invocation's recovery time rather than the engine's speed; (b) hoist the five inner
   functions out of `richardsWorkload` (the `raytrace-like` shape): it then measures property access and dispatch
   under N threads, as its header says, and needs another test for "thread body entered once". Recommendation: (b) for
   the gate, and add (a)'s shape as `scaling/thread-body-entered-once.js` with a bound on Baseline back-edges per
   thread rather than on time.
2. **D4 (fires are code-lifecycle windows)** changes a safety rule that was implemented and withdrawn once. Options:
   land it after D2/D3 with its own audit and campaign (recommended: the measurement now exists, and with D2/D3 a
   mistake in it costs performance only where a handler does rewrite objects - which the debug assertion finds); or
   leave the epoch as it is and accept three recompiles per watched fire.
3. **D5(b) (per-thread execution counters)** moves tier-up from "hot in the process" to "hot on a thread". Options:
   (a) per-thread slots as written; (b) per-thread slots folded into the shared counter at each checkpoint (keeps
   process-wide hotness, one shared write per 1,000 executions); (c) nothing, relying on D1/D2/D3 to keep shared
   lower-tier residency short. Recommendation: (c) first, measure, then (b).
4. **D3 part 2 and the `HoistingFailed`-at-first-exit alternative** are upstream JSC weaknesses visible flag off (one
   counted jettison per invocation of any loop-entered function with an untaken blind speculation). Carrying a
   GIL-off-only gate is the landing project's rule; offering the unconditional fix upstream is a separate decision.

### K: Doc mismatches

- **LANDING-PLAN, seventh round, P8** ("Profile stores now skip unchanged values (value/arith/array profiles, IC
  state cells; the Baseline JIT's profile stores flag-on)") and the option text of `useSharedProfileWriteAvoidance`
  ("Baseline value/array profile stores skip a value the profile already holds"): the value-profile half is not in
  the binary. `jit/JITInlines.h` guards it with `#if USE(JSVALUE64)`, a macro upstream removed; it is the only use
  left in `Source/JavaScriptCore` and evaluates to 0. Fix: D1; until then the documents should say "array profiles and
  IC state cells".
- **SPEC-jit history §41** ("four optimized worker threads parked by twelve watched transition fires were not
  jettisoned on resume with or without the change"): on the round-ten tree a watched fire's stop does jettison every
  parked thread's on-stack optimized code (`heapFactRewriteOnStackJettison` = 3 per fire with four threads, in every
  slow string-heavy run). `stopTheWorldAndRun` bumps the epoch for every window not under
  `PureCodeLifecycleStopWindowScope`, and only `CodeBlock::jettison`, one site in `VM.cpp` and one in `Heap.cpp`
  open that scope.
  Either §41's test parked threads that had no optimized frame on the stack at the poll, or the bump-edge rework
  ("BUMP-EDGE LAW", in-window bump) postdates it. The entry should say which, or be marked superseded by D4.
- **LANDING-PLAN open item "`vmstate/loop-entry-when-replacement-is-ftl-gil-off.js`: the margin of its bound"** ("a
  counter that grows with load, not a lock-out"): the count is 5 x (1 + N) x 2^r - 1 per lock-out episode - 799 at
  r = 5, 1,599 at r = 6 with four threads - which matches the recorded 1,601 exactly; it is a lock-out whose length is a
  step function of `r`, and load matters only through the number of counted jettisons that raise `r` (K8).
- **LANDING-PLAN open item "Tier-up of one shared function under N threads"** and PERF-RESULTS §6.9 attribute the
  `Overflow` exit to "the FNV multiply" / "a hash multiply". The exiting node is the hoisted `1 / expected` of the
  corpus's `shouldBe` helper (K3); the workload's own arithmetic does not exit.
- **PERF-RESULTS §2**, richards-like note ("its 'speedups' are not meaningful"): still true of the absolute numbers;
  the table of §6.12 nevertheless lists 1.15x at four threads as a scaling result and LANDING-PLAN's parity table
  carries it. The note and K7's table belong next to that row.
- **`CodeBlock.cpp` comment on `exitCountThreadMultiplier`** ("every JS thread running this CodeBlock's optimized
  code"): the multiplier is the process's live spawned threads plus one, whether or not they run the block (K6).

### K: What was run

All on the existing Release binaries (`main` and the round-ten final tree); nothing was built; no suite, campaign or
sanitizer lane. GIL-off runs went through bash wrapper scripts that put the three options in the environment (checked
with `--dumpOptions=1`: `useThreadGIL=false`, `useSharedGCHeap=true`).

- `scaling/richards-like.js` and `scaling/string-heavy.js` (copies with phase markers around the harness's two
  warm-up invocations and the threaded phase) at one, two and four threads, a few seconds each, about 120 runs in all,
  with combinations of `--verboseOSR=1 --printEachOSRExit=1 --reportCompileTimes=1`,
  `--reportJSThreadsCounters=1`, `--useDFGJIT=0`, `--dumpFTLDisassembly=1` (graph dump of one single-thread flag-off
  run), `--dumpGeneratedBytecodes=1`.
- `perf stat -e instructions:u,cycles:u` around those runs; `perf record -e cycles:u -k mono` with the shell's JIT
  dump option and `perf inject --jit` / `perf annotate` on five runs (one and two threads, tiered and Baseline-only);
  one attempt at HITM load sampling (no samples: the virtualized PMU does not deliver the event).
- Serial five- and seven-invocation variants of both workloads on `main`, flag off, GIL on and GIL off.
- Private-code variants (one function object per thread built from the workload's source text, with and without
  main-thread warm-up) at one, two and four threads.
- `nm` / `objdump` of two template instantiations in the Debug binary (to confirm the compiled-out arm of D1's
  function); `git log -S` for the removed macro.

## Section L. GIL off below GIL on: the broad residue (tags, polls, allocation, transitions, per-test rows)

Scope: what is left of the GIL-off deficit once the Double family, the visibility rule, array growth, RegExp and
the collector are set aside (those have their own sections): "tag predicates and polls everywhere else" (LANDING-PLAN
Open items, GIL off (7)), the JetStream rows Basic, WSL, json-parse-inspector, crypto, raytrace, UniPoker, async-fs,
OfflineAssembler (non-RegExp part), and the micro rows class-ctor-4, astar-like-nodes, megamorphic-put-transition,
proto-method-calls, closure-calls, json-parse / json-stringify, throw-catch, out-of-line-replace-poly.

All numbers are from the tenth round's final Release build, Linux x86-64. Every number is an instruction count
(`perf stat -e instructions:u`, or `perf record -e instructions:u -c <period>` samples), never a time. "Steady-state
instructions per iteration" is the difference of two runs of the same script with loop counts N and 3N, divided by
the extra iterations, so that start-up and compilation cancel. Generated code was read from the Release binary's own
output: `--dumpFTLDisassembly` / `--dumpDFGDisassembly` give the address range of every Air instruction or DFG node,
and the bytes of those ranges were taken from the live process (a debugger attached at `exit`) and disassembled with
`objdump`; the Release build carries no disassembler and the Debug build's generated code carries assertion code, so
neither can be read directly.

### L: Inventory

#### L-0. Where the GIL-off distance is: polls, tags, and "the rest" (explained)

One run per cell, whole JetStream test, instructions. A = GIL on. B = GIL on with `--usePollingTraps=1` (the poll as
GIL on models it: three instructions, no heap write). C = GIL on with `--useJSThreadsSingleOwnerWithGIL=0` (tagged
butterfly words, every tag predicate in every tier, the flag-on object-model bodies in C++). D = both. E = GIL off.

| test | A (G instr) | polls B/A | tags C/A | tags+polls D/A | GIL off E/A | rest E/D |
|---|---|---|---|---|---|---|
| delta-blue | 4.40 | 1.193 | 1.081 | 1.261 | 1.616 | 1.281 |
| crypto | 5.26 | 1.084 | 1.027 | 1.098 | 1.439 | 1.311 |
| ai-astar | 8.39 | 1.055 | 1.190 | 1.248 | 1.384 | 1.110 |
| UniPoker | 6.93 | 1.038 | 1.093 | 1.154 | 1.366 | 1.184 |
| async-fs | 2.81 | 1.085 | 0.996 | 1.072 | 1.340 | 1.250 |
| WSL | 46.92 | 1.032 | 1.032 | 1.095 | 1.339 | 1.222 |
| Basic | 6.28 | 1.026 | 1.024 | 1.045 | 1.335 | 1.278 |
| Babylon | 6.20 | 1.182 | 1.130 | 1.193 | 1.311 | 1.099 |
| OfflineAssembler | 22.12 | 1.016 | 1.050 | 1.068 | 1.302 | 1.219 |
| raytrace | 5.02 | 1.070 | 1.028 | 1.114 | 1.237 | 1.110 |
| Air | 5.83 | 1.031 | 1.092 | 1.114 | 1.227 | 1.102 |
| pdfjs | 23.03 | 1.032 | 1.050 | 1.093 | 1.210 | 1.108 |
| richards | 7.45 | 1.092 | 0.996 | 1.108 | 1.206 | 1.088 |
| json-parse-inspector | 4.39 | 1.000 | 1.063 | 1.063 | 1.164 | 1.094 |
| FlightPlanner (bimodal on every build) | 7.70 | 0.959 | 0.826 | 1.018 | 1.049 | 1.030 |
| geometric mean (without FlightPlanner) | | 1.058 (1.065) | 1.042 (1.060) | 1.114 (1.121) | 1.296 (1.315) | 1.163 (1.173) |

Single runs: differences of a few percent between columns are noise (C/A below 1 for async-fs and richards;
FlightPlanner alternates between two instruction counts on every build). Over these fifteen tests GIL off executes 1.30 times GIL on's instructions, and that factors into: the poll as an
instruction sequence 1.06; the butterfly tag (predicates in generated code and the flag-on object-model bodies in
C++) 1.05; and 1.16 that is neither - what only a GIL-off process does: the poll's heap writes (the visibility
rule), the concurrency protocols in generated code (claim compare-and-swap in transitions, compare-and-swap length
raises, seqlock-validated Map reads, per-thread allocator resolution, the second bounds check against the vector
length), operations that are inline GIL on and calls GIL off (MakeAtomString, Map/Set mutation and iteration,
WeakMap reads, array join, generators, promises), and the `gilOff`-keyed C++ bodies (RegExp, per-thread scratch,
locked structure tables). This matters for Open item (7): a process that ran untagged until its first spawn would
win back the 1.05, not the 1.30 (design L-D2).

Command, generic form: `perf stat -e instructions:u jsc --useJSThreads=1 [--usePollingTraps=1]
[--useJSThreadsSingleOwnerWithGIL=0] -e 'testList=["<test>"]' cli.js`, and the same with the three GIL-off
environment variables.

#### L-1. What one operation costs in each tier (explained)

Steady-state instructions per loop iteration, GIL on -> GIL off, for a loop whose body is the one operation (the
array and object fetches that feed it included; `empty` is the loop alone). Tier selection by `--useJIT=0`,
`--useDFGJIT=0`, `--useFTLJIT=0`, none.

| loop body | LLInt | Baseline | DFG | FTL |
|---|---|---|---|---|
| empty | 132 -> 132 (+0) | 39 -> 40 (+1) | 13.1 -> 16.0 (+2.9) | 6.7 -> 8.2 (+1.5) |
| arrayread | 238 -> 247 (+9) | 80 -> 84 (+4) | 19.9 -> 26.9 (+7.0) | 11.8 -> 18.5 (+6.7) |
| arraywrite | 196 -> 210 (+14) | 73 -> 87 (+14) | 15.4 -> 24.7 (+9.3) | 10.5 -> 13.8 (+3.3) |
| arraypush | 478 -> 1332 (+854) | 221 -> 297 (+76) | 34.8 -> 55.6 (+20.8) | 35.7 -> 50.9 (+15.2) |
| readinline (o.a) | 293 -> 303 (+10) | 98 -> 104 (+6) | 28.8 -> 34.1 (+5.3) | 18.5 -> 22.1 (+3.6) |
| readool (out of line) | 296 -> 315 (+19) | 102 -> 114 (+12) | 29.8 -> 37.0 (+7.2) | 20.1 -> 24.0 (+3.9) |
| protoread | 296 -> 986 (+690) | 103 -> 115 (+12) | 25.5 -> 30.7 (+5.2) | 17.3 -> 19.8 (+2.5) |
| writeinline | 248 -> 255 (+7) | 92 -> 95 (+3) | 28.9 -> 34.4 (+5.5) | 15.9 -> 19.9 (+4.0) |
| writeool | 251 -> 272 (+21) | 98 -> 103 (+5) | 30.0 -> 45.4 (+15.4) | 17.4 -> 29.3 (+11.9) |
| getbyval (o[k]) | 643 -> 768 (+125) | 157 -> 167 (+10) | 48.0 -> 57.0 (+9.0) | 35.4 -> 49.4 (+14.0) |
| call (noInline callee) | 384 -> 390 (+6) | 118 -> 119 (+1) | 67.0 -> 73.3 (+6.3) | 47.8 -> 54.7 (+6.9) |
| closurecall (inlined) | 449 -> 455 (+6) | 137 -> 138 (+1) | 13.0 -> 19.1 (+6.1) | 7.9 -> 11.8 (+3.9) |
| methodcall (o.m()) | 602 -> 1229 (+627) | 206 -> 223 (+17) | 30.7 -> 39.2 (+8.5) | 21.5 -> 25.4 (+3.9) |
| newobj (new Pt(i, 2)) | 854 -> 2331 (+1477) | 264 -> 306 (+42) | 38.4 -> 57.5 (+19.1) | 6.5 -> 9.7 (+3.2) |
| addprop (seven adds) | 1818 -> 6342 (+4523) | 486 -> 626 (+140) | 128.1 -> 156.9 (+28.8) | 104.9 -> 136.3 (+31.4) |
| newarray ([i, 2, 3]) | 697 -> 794 (+97) | 472 -> 564 (+93) | 47.9 -> 70.8 (+22.9) | 34.4 -> 57.6 (+23.2) |
| strconcat | 970 -> 1038 (+68) | 268 -> 295 (+28) | 65.0 -> 77.1 (+12.1) | 53.8 -> 71.2 (+17.4) |
| mapget | 440 -> 1245 (+805) | 230 -> 449 (+218) | 59.9 -> 308.9 (+249) | 43.0 -> 80.3 (+37.3) |
| trycatch | 245 -> 254 (+9) | 82 -> 87 (+5) | 20.1 -> 27.7 (+7.6) | 11.1 -> 18.9 (+7.8) |

Reading it: in the LLInt the large rows are the disabled metadata caches (SPEC-jit section 4.3: a prototype read,
a method call and every property-adding put take the slow path every time: +690, +627, +646 per add) - the
transition-cache and proto-cache records of the LLInt design cover them; in Baseline the tax is small except adds
(+20 per add: the claim-first handler) and array growth; in DFG and FTL the per-access tax is a few instructions and
it is the number of accesses that makes the total. `mapget` below the FTL is a call into the validated lock-free
reader (+220 to +250 per `get`).

The FTL sequences, from the disassembly of those loops:
- *Read of an out-of-line property*: `mov 8(obj),r` becomes `mov mask,r; and 8(obj),r` (one instruction more; the
  segmented-dispatch test is elided under the watched thread-local set, E1).
- *Write (replace) of an out-of-line property*: GIL on `mov 8(obj),b; mov v,-0x50(b)`; GIL off nine instructions:
  `mov 8(obj),w; mov tag(spill),t; xor w,t; movabs $0xffffffffffff,m; and m,w; lea 1(m),m; cmp m,t; jae slow;
  mov v,-0x50(w)`. The tag is re-loaded from its stack slot and the payload mask is rebuilt at every write.
- *Array read*: a second CheckInBounds against the vector length (two instructions; SPEC-jit history section 39) and
  the mask.
- *Poll*: `movabs $trapWord,r; testb $0x1f,(r); jne` (three; two where the address is already in a register).
- *Inline allocation*: the allocator comes from the thread's table: `cmpl $slot,bound(lite); jbe slow; mov
  table(lite),r; mov slot*8(r),r; test r,r; je slow` (six) where GIL on bakes one `movabs`.
- *CheckTransitionOwner*: `test $8,obj; jne; mov tag(spill),t; xor 8(obj),t; movabs $0xffff000000000000,m; test
  t,m; jne` (seven), once per object per run of transitions.
- *Butterfly install in a transition*: two more (`mov tag,r; or storage,r`).

A loop that constructs an object with three inline properties and adds six more (one allocation of four out-of-line
slots, one reallocation to eight), everything inlined, is 77 straight-line instructions GIL on and 127 GIL off
(measured steady state 93 and 141, the difference being allocation slow paths): three polls 9, three allocator
resolutions 18, the owner check 7, two tagged installs 4, one more store barrier and the barriers' address
materialization 6, a store-to-load forwarding the poll prevented 5, one more.

#### L-2. Polls at the entries of inlined callees (explained)

`DFG::ByteCodeParser` calls `handleCheckTraps()` at every `op_enter` it parses, the machine frame's and every
inlined callee's alike (`main`'s code; with signal-delivered traps the node is a free InvalidationPoint, with
polling traps - every GIL-off process - it is a CheckTraps). In FTL code of object-style programs nearly all executed
polls are these: samples inside FTL code, GIL off, classified by the poll's position (first `bc#0` poll of a
compilation = machine entry, other `bc#0` polls = inlined entries, the rest = loops):

| test | FTL samples | machine entry | inlined entries | loops |
|---|---|---|---|---|
| raytrace | 3,572 | 19 | 401 (11.2 % of FTL instructions) | 7 |
| delta-blue | 3,107 | 7 | 324 (10.4 %) | 49 |
| richards | 2,590 | 14 | 167 (6.4 %) | 1 |
| WSL | 9,365 | 67 | 314 (3.4 %) | 31 |
| Basic | 3,246 | 11 | 81 (2.5 %) | 26 |
| crypto | 1,424 | 8 | 0 | 59 |

Each such poll is also a write of the poll's heap set, so it ends common-subexpression elimination and
store-to-load forwarding across every inlined call: part of delta-blue's "CheckStructure +790, GetButterfly +233" (PERF-RESULTS
6.12) and the un-forwarded `o.h` read in the loop of L-1. GIL on with polling traps pays the three instructions and
not the heap write (column B of L-0: delta-blue 1.19, Babylon 1.18, richards 1.09). Design L-D1.

#### L-3. Inline allocation resolves its allocator through the thread's table on every allocation (explained)

`FTL::LowerDFGToB3::tlcAllocatorForSlot`, `AssemblyHelpers::emitLoadTLCAllocatorForSlot`: load the lite, compare
the slot with the table bound, load the table pointer, load the entry, null-test. Six instructions per allocation,
18 of the 50 in L-1's loop; in raytrace's FTL code the bound compare alone holds 46 samples of 3,572. The loads are
typed on the root abstract heap, so B3 re-does them after every store. Design L-D3(b).

#### L-4. CheckTransitionOwner on an object the same code just allocated (explained)

The parser plants `CheckTransitionOwner(base)` before the first inlined transition of every base, fresh allocations
included unless allocation sinking removes the object. raytrace 50 samples of 3,572, WSL 59 of 9,365. An object
whose allocation dominates the check in the same compilation, with no node in between that lets it escape, is owned
by the running thread and is not a precise allocation. Design L-D3(c).

#### L-5. MakeAtomString with a concat-key cache is a locked call (explained)

DFG `compileMakeAtomString` and the FTL lowering emit `operationMakeAtomString{2,3}WithCache` instead of the two
inline quick-entry probes when the GIL is off; `ConcatKeyAtomStringCache::getOrInsert` then takes the cache's lock
around the map probe. WSL: `operationMakeAtomString2WithCache` 179 -> 1,475 samples (period 2 M), 22 % of WSL's
whole GIL-off excess (5,796 samples). The stated reason (SPEC-jit history section 52: another mutator may be
rewriting an entry between the key compare and the value load) no longer describes the writer: GIL off each quick
slot is written exactly once, under the lock, value first, store-store fence, then key. Design L-D4.

#### L-6. Map, Set and WeakMap operations that are inline or lock-free GIL on (explained)

WSL: `addNormalizedGILOff` (Map 224, Set 194), `materializeGILOff` 115, `operationNewSet`/`NewMap` 98, the iterator
`next` family (`JSSetIterator::next` 106, `createIteratorResultObject` 92, `JSMapIterator::next` 73,
`setIteratorProtoFuncNext` 58, `mapIteratorProtoFuncNext` 34, `createSetIteratorObject` 43): about 1,000 samples
against `operationMapSet` 135 + `operationSetAdd` 101 GIL on, 13 % of WSL's excess. Basic: `MapGet` in FTL code 292
-> 707 samples (the seqlock-validated inline probe: +37 instructions per `get`), `MapHash`/`LoadMapValue` +105, and
WeakMap `get`/`has` as host calls into `tryReadLockFreeGILOff` (protoFuncWeakMapHas 64, protoFuncWeakMapGet 34,
the reader 156, the cell-lock destructor 34): about 290 samples of Basic's 1,305 excess. Below the FTL, `Map.get` is
`operationMapGet` into the same reader: 60 -> 309 instructions per `get` in DFG code. Mutation under the table's
cell lock with a seqlock bracket is the design (SPEC-jit section 5.8, history section 35); what is not inherent is
the DFG tier's call, WeakMap's missing inline reader, and the iterator result objects allocated out of line. Design
L-D8.

#### L-7. Generator resume claims and publishes with two compare-and-swaps (explained; cycles, not instructions)

Basic: `GeneratorClaimResume` / `GeneratorPublishResume` 357 FTL samples at the two `lock cmpxchg`. The instruction
cost is two instructions; the samples are skid around a serializing instruction, so this is a cycle cost of roughly
two uncontended atomic read-modify-writes per `next()`. Also `JSGenerator` allocation out of line
(`allocateCell<JSGenerator>` 27, `JSGenerator::create` 21, `operationNewGenerator` 13: an inline allocation site
without a thread-table arm, the SPEC-jit section 5.5 "Inline allocation GIL off" rule's next member). Not designed
here beyond that pointer.

#### L-8. CreateThis on a poly-proto function looks the prototype up by name, GIL off (explained)

`DFG::createThisFromAllocationProfileGILOff`: when the allocation profile's structure has poly proto, the {structure,
prototype} pair could be torn by a racing clear, so the function reads the live `.prototype` by name
(`JSFunction::prototypeForConstruction` -> `JSObject::get` -> `JSFunction::getOwnPropertySlot` ->
`PropertyTable::findConcurrently`) on every construction. class-ctor-4: of 6,400 extra samples (period 1 M) 3,500
are this chain. The micro rows class-ctor-4 (2.67) and astar-like-nodes (3.05) are this path and little else: the
harness re-creates the class or constructor function inside every timed call, after a few re-creations "Detected poly proto
optimization opportunity" fires, and from then on every construction takes `operationCreateThis`
(`createThis` counter 1.00 M of 1.2 M constructions, in both flag-on modes; on `main` too `operationCreateThis` and
the allocation under it are 66 % of the row's samples). With the class defined once the same loops are 6.3 -> 10.3
instructions per construction (class-ctor-4), 337 -> 620 (astar-like-nodes), 93 -> 141 (constructor plus six adds):
ratios 1.5 to 1.8, which is L-1's arithmetic. Design L-D6; and the micro set should gain mono-proto variants
(Doc mismatches).

#### L-9. (Re)allocating transitions that leave generated code (explained)

Two places. (a) A per-case-compiled Transition stub (one with conditions to check, so not the shared handler) calls
`operationPutByTransitionReallocatingConcurrent` for any transition that allocates or grows out-of-line storage
(`InlineCacheCompiler.cpp`, the `AccessCase::Transition` arm under `useTaggedButterflies`: "rare enough not to
duplicate the shared handler's inline install here"); the operation runs `tryCompleteCachedTransitionConcurrent`.
Seen in the astar-like-nodes row (`tryPutDirectTransitionConcurrent` 285, `storeTaggedButterflyWordConcurrent` 129,
`createOrGrowPropertyStorage` 118 samples of 1,500 extra) and as Baseline's +20 instructions per add; it does not
show once DFG/FTL have inlined the transitions. (b) The megamorphic store probe handles only non-reallocating
transitions, in both modes; the reallocating add goes to `operationPutByMegamorphicReallocating`, which GIL on runs
`main`'s body (34 instructions per add by samples) and GIL off runs the generic put through
`tryPutDirectTransitionConcurrent` (84 instructions in that function alone, plus storage creation and
`casButterfly`): megamorphic-put-transition 3.10 G -> 6.20 G instructions, 1,003 + 303 + 100 + 87 of the 3,100 extra
samples. Design L-D7.

#### L-10. Math inline caches never regenerate GIL off (explained)

`JITMathIC::generateOutOfLine` returns before touching code when `g_jscConfig.gilOffProcess` is set (patching live
code with several mutators is forbidden, SPEC-jit I2). A site compiled before its arithmetic profile had seen a type
is a single patchable jump to `operationValue{Add,Sub,Mul,...}ProfiledOptimize`, and a site whose inline form was
int32-only takes that operation whenever the operands are not int32; GIL on the first slow call patches in the
full snippet, GIL off every execution stays a C++ call. megamorphic-put-transition: `operationValueAddProfiledOptimize`
535 samples plus `generateOutOfLine` 61 of the 3,100 extra (the accumulator of the loop becomes a double). Not
visible in the five JetStream tests profiled (at most one sample each). The comment in `JITMathIC.h` calls the
optimized variant "the chartered follow-up"; no Open item lists it. Design L-D5.

#### L-11. Structure tables read under the structure's lock (explained)

(a) `Structure::addPropertyTransitionToExistingStructureConcurrently` finds a single-slot transition lock-free and
takes `m_lock` for a structure whose transition table is a map (several outgoing transitions). json-parse-inspector:
199 samples against 49 for `main`'s lock-free `WeakGCMap` lookup, 42 % of that test's excess (356 samples);
json-parse micro 164 -> 250. (b) The mutator's property-table walk GIL off (`Structure::forEachProperty` from
`FastStringifier`) materializes the PropertyTable and walks it under the lock: json-stringify micro
`forEachProperty` 96 -> 573 samples with `ConcurrentJSLockerBase::~ConcurrentJSLockerBase` 68, 80 % of that row's
excess. GIL on these were restored to `main`'s forms in the tenth round (L6-G). Design L-D9.

#### L-12. `Array.prototype.join`'s fast joiner is off (explained)

`arrayProtoFuncJoin` skips `JSOnlyStringsAndInt32sJoiner` when `gilOffProcess` (it measures the lanes and then
re-reads them into a buffer of that size, which a racing writer can overrun) and takes the general
`JSStringJoiner`. UniPoker: `operationArrayJoin` 38 -> 450 samples, with `appendStringToData`, `joinImpl` and the
entry vector's destructor +219 and the fast joiner's -257: about 370 samples, 45 % of UniPoker's excess over the
tagged, polling GIL-on configuration. Design L-D10.

#### L-13. Async generators, promises and microtasks (partly explained)

async-fs over the tagged, polling GIL-on configuration: generated code +95 samples, `JSPromise::performPromiseThenWithInternalMicrotask`
+38 and `JSPromise::resolve` +32 (out of line GIL off), `JSAsyncGenerator::{enqueue,dequeue,retireIfQueueEmpty}GILOff`
+69, `queueMicrotaskGILOff` +18, cell-lock destructors +16: 322 samples in all (1.21x). The per-generator queue and
the microtask queue are locked GIL off by design (SPEC-ungil section E); which of the promise paths are out of line
for a reason was not traced.

#### L-14. The LLInt's disabled caches (explained; designed elsewhere)

L-1's LLInt column. First-iteration instruction ratios GIL off over GIL on (`testIterationCount=1`): WSL 1.256, Basic
1.254, OfflineAssembler 1.206, UniPoker 1.174, typescript 1.116, Babylon and Air 1.115, crypto 1.114, pdfjs 1.093,
raytrace 1.086, json-parse-inspector 1.074, FlightPlanner 1.068, async-fs 1.053, ai-astar 0.984. The Startup
component of the score is the first iteration.

#### L-15. Compilation costs more GIL off (explained)

Compiler-thread samples: Basic 815 -> 1,086 (+33 %), raytrace 595 -> 690 (+16 %), WSL 8,858 -> 9,107 (+3 %).
`jsThreadsParkableSlowPathClobbersHeapFactsGILOff` alone is 132 samples in WSL; the rest is more nodes (a poll, an
ExitOK and an InvalidationPoint at every inlined entry; CheckTransitionOwner; larger exit ramps: a GIL-off OSR exit
stub is 13 instructions where GIL on's is 2). It runs on the compiler threads, so it lengthens tier-up rather than
the mutator's path. L-D1 removes three nodes per inlined call.

#### L-16. Attribution of the rows

| row | GIL off / GIL on (instr) | what it is made of |
|---|---|---|
| Basic | 1.34 | polls+tags 1.045; rest: WeakMap reads as host calls 290 samples (22 % of 1,305), Map `get` validated probe ~210, generator CAS pair (cycles), JSGenerator / NewFunction / NewArrayWithSize out of line ~170, compiler +271 |
| WSL | 1.34 | polls+tags 1.095; MakeAtomString 1,296 samples (22 % of 5,796), Map/Set mutation and iteration ~1,000 (17 %), `PropertyTable::findConcurrently` + `Structure::getConcurrently` + `locationForOutOfLineOffsetConcurrent` 336, `allocateCell<JSFinalObject>` out of line 114, inlined-entry polls 3.4 % of FTL |
| json-parse-inspector | 1.16 | tags 1.06 (C++ concurrent put legs); locked transition-map lookup 150 of 356 samples, `tryPutDirectTransitionConcurrent` 38, `allocateCell` 29, per-thread JSON atom cache 19 |
| crypto | 1.44 | polls+tags 1.10; the rest is generated code in `am3` (CheckInBounds +107, loop polls +67, CheckStructure +26, GetVectorLength +17 of 1,424 FTL samples): the visibility rule and the second bounds check, not this section's |
| raytrace | 1.24 | polls+tags 1.11 (inlined-entry polls 11 % of FTL instructions); allocator resolution and CheckTransitionOwner on fresh vectors |
| UniPoker | 1.37 | polls+tags 1.15; array join 370 of 818 samples over D; RegExp the rest |
| async-fs | 1.34 | polls+tags 1.07; L-13 |
| OfflineAssembler | 1.30 | polls+tags 1.07; RegExp (`operationRegExpExecNonGlobalOrSticky` +378, `executingRegExpSlotGILOff` 314, `RegExp::match` +297, `MatchingContextHolder` +224, `softStackLimitForCurrentThreadGilOffSlow` 200 - a stack-limit read out of line per match -, `convertToNonRopeGILOff` 109, the per-thread ovector 99, `threadRegExpGlobalDataSlow` 91): the RegExp section's |
| class-ctor-4, astar-like-nodes | 2.3, 2.7 | L-8 (poly proto); mono-proto variants 1.5-1.8 |
| megamorphic-put-transition | 2.0 | L-9(b) 1,500 of 3,100 samples, L-10 600 |
| proto-method-calls, closure-calls, int-loop | 1.16, 1.44, 1.44 | polls (loop and inlined entry) |
| json-parse, json-stringify | 1.19, 1.22 | L-11 |
| map-set-get | 1.27 | L-6 |

### L: Designs

#### L-D1. No poll at the entry of an inlined callee

**Status: proposed**

*Rule.* In a process with polling traps and the flag on, `op_enter` of an inlined callee plants an InvalidationPoint
and no CheckTraps, unless the callee has a recursive-tail-call entry block. The machine frame's `op_enter` and every
`op_check_traps` (loop back edges) are unchanged.

*Who writes, who reads.* Only `DFG::ByteCodeParser` (`case op_enter`: call `handleCheckTraps()` when
`!inlineCallFrame() || codeBlock->hasTailCalls()`, otherwise `addToGraph(InvalidationPoint)` after the existing
`emitExitOK()`). LLInt and Baseline do not inline and keep polling at every entry. No C++ slow path changes.

*Why it is sound.* A poll exists so that (1) a stop or a collection request is answered within a bounded number of
instructions and (2), GIL off, a read that decides a loop's exit is performed again (SPEC-jit I21). Unbounded
execution passes through a loop back edge or through a real call; both still poll (the callee's machine-frame
entry). An inlined callee's entry is neither: between two polls there is at most one function body times the
inlining depth, which the inliner bounds. The recursive-tail-call case is the exception: `op_enter` places the poll
after the switch to `m_entryBlockForRecursiveTailCall`, so for such a callee the entry poll IS the back-edge poll of
the loop the tail call became; the rule keeps it. (2) concerns loops; their polls stay. The "parked mutators resume
into the patched exit" rule needs an invalidation point after every poll, not a poll at every invalidation point.

*Memory ordering.* None involved; a poll is a relaxed byte load on both architectures.

*Collector, stops, watchpoints.* The longest poll-free stretch grows from one inlined body to the enclosing
function's body; stop latency is bounded by the machine function's straight-line length, as it is flag off with
signal traps between invalidation points. The kept InvalidationPoint is where a jettison caused by a fire lands, as
today.

*What another thread observes.* Nothing new: a plain read hoisted across the (former) poll inside straight-line
code is at most one function body stale.

*Flag off.* The condition includes `Options::useJSThreads()`; flag-off code with polling traps (platforms without
signal delivery) is unchanged.

*Failure modes.* A stop that waits too long: the stop watchdog (30 s) names it; cannot happen without a loop or a
call. A termination request not honoured in deep inlined recursion: recursion inlines to a fixed depth and then
calls.

*Tests.* `JSTests/threads/jit/no-poll-at-inlined-entry-gil-off.js`, behaviour only (node counts are not
observable from a Release shell): (a) a spin loop whose exit flag another thread sets and whose body calls inlined
functions terminates (the loop's own poll remains); (b) a self-tail-recursive inlined callee that spins on a flag
terminates (the kept poll of the recursive-tail-call entry); (c) the watchdog terminates a function that calls
inlined callees in a loop. The instruction effect is recorded in PERF-RESULTS instead: steady-state instructions per
iteration of the closure-call and constructor loops of L-1 before and after (11.8 -> about 8.5 expected for
closurecall; 141 -> about 131 for the constructor loop), and the number of `CheckTraps` nodes in raytrace's FTL
graph dumps (232 -> 26).

*Verification.* Corpus in the two flag-on modes with `--usePollingTraps=1` GIL on; the stop-latency tests
(`jit/poll-visibility-control-reads-gil-off.js`, the watchdog tests); TSan lanes (they keep polling GIL on).

*Expected gain.* Direct: the inlined-entry share of FTL instructions in L-2 - raytrace 11 %, delta-blue 10 %,
richards 6 %, WSL 3 %, Basic 2.5 % of FTL code; about 4 %, 3.5 %, 2 %, 0.3 %, 0.6 % of whole-test instructions.
Indirect: every removed poll is a removed heap write, so structure checks, butterfly loads and store-to-load
forwarding survive inlined calls (delta-blue's re-executed CheckStructure/GetButterfly; the 5 instructions of L-1's
loop). Compile time: three nodes fewer per inlined call.

*Risks.* None to memory safety. A program that relied on a callee's entry poll to see a plain write inside
straight-line code sees it one body later.

*Alternatives rejected.* Removing polls dominated by another poll in general (needs a dominance and loop analysis
for a smaller additional gain: what is left after this rule is loop polls); making the poll cheaper (it is three
instructions already).

#### L-D2. A single-mutator phase: untagged (and unsynchronized) until the first spawn

**Status: proposed; recommendation below is to not build scope A alone**

*What is asked.* Open item (7): a GIL-off process that has not yet spawned a Thread runs `main`'s forms, as a
GIL-on process does since the tenth round (G1), and switches at the first spawn.

*What it can buy, measured.* L-0: tags are 1.05 of the 1.30. Scope A (the `useTaggedButterflies` family only)
brings GIL off over GIL on, single-threaded, from 1.30 to about 1.24 in instructions (JetStream about 0.80 -> 0.84).
Scope A plus signal-delivered traps before the switch removes column D: about 1.17 (0.88). Only scope B - every
protocol that exists because another mutator may be running - reaches about 1.03-1.05 (0.95): what would remain is
the shared heap's allocation slow paths and VM state reached through the lite.

*Definitions.* Phase U (uniprocessor): exactly one thread, the first carrier, has ever been able to touch the
GIL-off VM's objects. Phase T: anything else. The switch U -> T happens once per process, before (a) the first
`new Thread` creates its native thread, or (b) a second carrier's first entry into the VM (`JSLock` registration of
a second native thread). It never goes back. State: one byte outside the frozen configuration page
(`g_jsThreadsConcurrentMutators`; the options block is read-only after finalization, so
`Options::useTaggedButterflies()` cannot itself change).

*Scope A: what is keyed on the byte.*
- C++: every `Options::useTaggedButterflies()` test (303 in `runtime/`; 28 `dfg`, 22 `jit`, 17 `bytecode`, 16
  `llint`, 14 `ftl` are emitters) reads the byte instead; `currentButterflyTID()` returns 0 in phase U.
- LLInt: `ifTaggedButterfliesBranch` loads the byte (it already loads a byte per fast path).
- Baseline, the shared IC handlers and thunks: ALWAYS the tagged forms in a GIL-off process. Baseline frames cannot
  be invalidated while on the stack, so code that survives the switch must already be correct after it; tagged
  forms are correct on untagged words (every owner test passes with tag 0: the GIL-on configurations before G1 ran
  exactly that).
- DFG and FTL (and the per-case IC stubs they own): compiled in phase U with GIL-on forms, registered on a
  process-wide "concurrent mutators" watchpoint-like list, jettisoned at the switch.
- Scope B adds, under the same byte: the claim compare-and-swaps (`gilOffProcess` tests in the transition emitters
  and in `tryPutDirectTransitionConcurrent`), compare-and-swap length raises, Int32-lane verification, the
  operations of L-5/L-6/L-12, the per-thread caches that replace VM caches for exclusion rather than for state
  placement, poll heap writes, constant allocators in DFG/FTL code. About 980 `gilOff` tests exist
  (`runtime` 585, `heap` 97, `bytecode` 86, `dfg` 76, `ftl` 65, `jit` 51, `llint` 30); each must be classified
  "state placement" (stays: `vm.m_gilOff` is immutable, SPEC-ungil U0c, and decides where Group-3 state lives) or
  "exclusion" (moves to the byte).

*State left by phase U that phase T must be able to read.*
- Butterfly words: tag 0, SW 0. Phase T's owner of those words must be the first carrier. Today a GIL-off carrier's
  tag is allocated from the carrier range (the main thread's is 0x4000; `ThreadManager::allocateCarrierTIDInternal`),
  and 0 is what a thread without a lite reports. Either the first carrier takes TID 0 for good (requires that no
  lite-less thread - compiler, collector helper, sampling profiler - executes an owner-tested write; they do not
  write JS objects, to be audited), or the switch re-tags every butterfly word and every structure's
  `m_transitionThreadLocalTID` in a heap walk (O(heap), tens of milliseconds per hundred megabytes, once).
- Structure thread-local sets: in an untagged process they are never armed or fired. With carrier TID 0 a
  structure created in phase U already reads "transitioned and written only by TID 0", which is true; nothing to
  walk. Otherwise the walk above sets them.
- PropertyTable index vectors: a tagged process allocates them with a 16-byte header carrying the allocation's
  index size (`PropertyTable::allocateIndexVector`), and `destroyIndexVector` frees from the header. The header
  must be unconditional in a GIL-off process (key it on the process mode, not on the phase); the ordering and atomic
  stores may stay keyed on the phase. Deleted offsets released in phase U sit on the reusable list, which is a
  legal phase-T state (no reader can hold them).
- `StructureTransitionTable`'s `WeakGCMap` is constructed with its locking mode; construct it locking in a GIL-off
  process regardless of phase (the lock is taken only on the map arm).
- Arrays: the 4k-1 vector-length rule is keyed on the shared heap and holds in both phases. Double arrays made in
  place in phase U are legal phase-T objects (allocation-profile promotion creates Double arrays GIL off already);
  what phase T restricts is how shapes change, not which exist. CopyOnWrite and ArrayStorage likewise.
- LLInt metadata: phase U publishes `main`'s multi-word caches; the threaded asm paths read one-word forms. The
  switch clears every CodeBlock's LLInt caches (one walk of the code-block set), so no tagged path meets a
  ProtoLoad or Unset mode byte.
- Megamorphic caches, the concat-key caches' quick entries, VM-level caches replaced per thread in phase T: cleared
  or abandoned at the switch.

*The switch (performed by the first carrier, on its own stack, before the second mutator exists).*
1. Set the byte to "switching" (C++ slow paths that test it take the tagged leg from here on).
2. Quiesce the compiler: cancel queued plans, wait for running ones (the existing per-VM wait), discard ready
   ones; they were compiled for phase U.
3. `Heap::preventCollection()` (no concurrent marker reads a word or a table while it is re-laid).
4. Jettison every DFG and FTL code block (new reason, not counted as a reoptimization) and drop their IC stubs;
   on-stack optimized frames are invalidated and exit at the invalidation point after the call they are in.
5. Clear LLInt caches; clear VM-level caches that phase T replaces; if not carrier-TID-0, the re-tagging walk.
6. Store the byte "phase T" (release), allow collection, resume. The new native thread is created after this, so
   thread creation orders every store of the switch before the new mutator's first instruction on both
   architectures; compiler threads observe the byte under the worklist lock.

*The hard part: C++ frames of the first carrier that span the switch.* `new Thread` can run inside a callback
(`sort`'s comparator, a getter under `JSON.stringify`, `valueOf` under an indexed store). A C++ routine that tested
the phase before calling out and continues down the untagged leg afterwards now runs unsynchronized against a real
second mutator. The tenth round's G1 audit established, for GIL handoffs, that nothing holds a storage pointer, a
shape check or an open fast-path window across a call; the same property makes such a routine re-load and
re-check after the call - but it re-loads through the leg it chose before. Inline accessors (`JSObject::butterfly()`
and friends) re-test the byte and adapt; routines with an explicit early branch that contains a call out do not.
Every one must be found (audit table: `JSObject.cpp` 95 gates, `JSArray.cpp` 64, `Structure.cpp` 42,
`ArrayPrototype.cpp` 15 ...) and either shown call-free or made to re-test. Detection: a Debug-only scope object at
every phase test that asserts on destruction that the phase did not change, and tests that spawn from every kind of
callback; a missed site is otherwise an unmasked load of a word another thread has tagged or replaced, i.e. a wild
pointer.

*Collector.* The switch runs with collection prevented; nothing else changes. *Stop protocol.* No other mutator
exists at the switch, so no stop is needed; step 4's jettisons assert "world stopped or single mutator".
*Watchpoints and deferred claims.* Phase U makes no deferred claim that phase T could find in flight (the switching
thread is at a call, outside any transition). Phase-U DFG/FTL code watched sets on `main`'s terms and is gone.
*Observability.* A program cannot observe the phase except through `$vm` (the owner TID of an old object) and
timing.

*Flag off and GIL on.* The byte is never written; C++ gates read it instead of the options byte (same instruction,
another address).

*Failure modes.* (1) A missed phase test in code that survives the switch (above): wild pointer; detected by the
Debug scope object, the corpus with a spawn injected at every Nth host call (a new testing option of the shell),
TSan. (2) A compiler plan that straddles the switch installs phase-U code: step 2, asserted at install by stamping
each plan with the phase. (3) A second carrier that enters without passing `JSLock`'s registration: release
assertion in lite installation.

*Tests.* `objectmodel/phase-switch-*.js`: spawn from each callback kind listed above and continue the interrupted
operation with the spawned thread hammering the same object; spawn with optimized frames on the stack; spawn during
a pending tier-up; objects, dictionaries, CopyOnWrite, Double and ArrayStorage arrays made before the switch used
by both threads after it; a process that never spawns (phase U for life) running the whole single-threaded corpus.
Bun's test runs spawn at start-up through the keep-alive preload and would exercise phase T only; phase U needs its
own lane (an option pins the phase for testing).

*Verification.* The full battery in both phases; the audit table; an amplifier campaign on the switch tests.

*Expected gain.* See "what it can buy". For Bun: a program that spawns its workers at start-up gains nothing; a
program that never spawns gains everything; the test preload spawns first thing.

*Risks.* A process-wide mode whose failure mode is memory unsafety, a second copy of most object-model paths kept
alive GIL off (phase U runs `main`'s, phase T the concurrent ones: every future fix must consider both), and a
JetStream number that stops measuring what threaded code costs.

*Alternatives.* (i) Start as a GIL-on process and convert the VM at the first spawn: `vm.m_gilOff` is immutable by
design (SPEC-ungil U0c), generated code and VM accessors select state placement on it at compile time, and the heap
would have to change from one client to server/clients under a live program; rejected. (ii) Scope A alone: 5 %.
(iii) Spend the same effort on the mechanisms of L-2..L-12, which help threaded programs too: recommended first.

#### L-D3. Three emitter changes: the write predicate, the allocator table, the owner check on fresh objects

**Status: proposed**

(a) *Fused write predicate (DFG, FTL, Baseline choke point `loadButterflyForWrite`).* `payload = word ^ tag; if
(payload & 0xffff000000000000) slow; store through payload`: an owner-tagged SW=0 word xor the tag IS the untagged
pointer (SPEC-jit section 4.2 already says so for the x86-64 Replace form). Four instructions and the store instead
of eight, with the high-bits mask in a register B3 can hoist (on arm64 `eor; tst #imm; b.ne`, the mask being a
logical immediate). The thread's tag becomes a B3 value loaded once per function from thread-local storage (it is
already; it is spilled and re-loaded, which a rematerializable load from `%fs`/`tpidr` avoids). No protocol change:
the same predicate (segmented, foreign, SW=1 all have a non-zero high half after the xor; the 0xffff sentinel
included). Expected: out-of-line write 29.3 -> about 25 instructions per iteration in L-1's loop; every PutByOffset,
PutByVal and ArrayPush on a non-elided path.

(b) *Allocator table without a bound and without a second load.* `VMLite::tlcTable` always points at a table of
lifetime capacity; a lite with no client points at a static all-null table. Generated code: `mov table(lite),r; mov
slot*8(r),r; test; je slow` (four, from six); B3 types the two loads on a dedicated abstract heap that only calls
and the poll's slow path write, so that two allocations of one class in a block share them. Same-thread data
(written by the owner in C++ at client attach, read by the owner): no ordering on either architecture. Expected: 2
of 6 instructions per inline allocation (raytrace, WSL, Basic allocate tens of millions of small objects).

(c) *CheckTransitionOwner folded for fresh allocations.* In `DFG::ConstantFoldingPhase` (or strength reduction):
remove `CheckTransitionOwner(x)` when `x` is a NewObject / CreateThis-with-known-structure / MaterializeNewObject
node of the same compilation that dominates the check, with no node between them that can store `x` to the heap or
pass it to a call (the object is unpublished, so no other thread can have flipped its word; it is not a precise
allocation because the inline allocation path produced it, and the slow path of that allocation never returns a
precise cell for these sizes - to be asserted). Seven instructions per constructed object that gains properties.

*Collector, stops, watchpoints:* untouched. *Flag off:* none of the three is emitted. *Failure modes:* (a) a wrong
mask constant lets a segmented word through: `validateButterflyTagDiscipline` and the choke-point lint already check
the predicate's shape; add the fused form to them. (c) an escape the analysis missed: the check is what stands
between a foreign object and an unsynchronized transition, so the rule must be the conservative one above (same
block or dominating with no clobbering node). *Tests:* existing `jit/` transition and write-predicate tests in four
modes; a new count test for (c) (no CheckTransitionOwner in the graph dump of a constructor loop).
*Expected gain together:* about 10 of the 50 extra instructions of L-1's constructor loop; 1-2 % of raytrace- and
WSL-like tests.

#### L-D4. The concat-key cache's quick entries are probed inline again, GIL off

**Status: proposed**

*Rule.* Writer (`ConcatKeyAtomStringCache::getOrInsert`, unchanged): under `m_lock`, quick slot `size` (0 or 1) is
written exactly once: value, `storeStoreFence`, key. Reader (DFG `compileMakeAtomString`, FTL `compileMakeAtomString`
lowering): load key of slot 0 with acquire semantics, compare with the variable string's cell pointer, on equality
load value; same for slot 1; else the operation. A slot's key goes null -> K once and never changes, so a reader
that saw K reads the value stored before K.
*Ordering.* x86-64: two plain loads, program order suffices. arm64: `ldar` for the key (or `ldr` + `dmb ishld`
before the value load); the writer's fence is `dmb ishst`.
*Collector.* The entries are `WriteBarrier<JSString>` visited under `m_lock`; the reader holds both strings in
registers across no safepoint.
*What a second thread sees mid-way.* Null key (miss, operation), or key with its value.
*Flag off / GIL on.* Already inline.
*Failure modes.* A cleared cache (the Megamorphic transition clears the map, not the quick entries: check that the
quick entries are never reset while code can probe them; if `clear()` is ever extended to them the probe must go
through a version word). *Tests.* `jit/concat-key-cache-probe-gil-off.js`: N threads through one shared code block
filling and probing a fresh cache, results compared with a reference; count of `operationMakeAtomString2WithCache`
calls per million probes before (1,000,000) and after (about 2). TSan lane.
*Expected gain.* WSL: about 1,100 of 30,162 samples (3.7 % of its instructions, a sixth of its excess).
*Alternative.* A per-thread cache: more memory, same probe.

#### L-D5. Math inline caches: the full snippet at compile time, GIL off

**Status: proposed**

*Rule.* In a GIL-off process `JITMathIC::generateInline` never emits the patchable-jump-only form nor relies on a
later regeneration: when the profile is empty it emits the generator's full snippet (`generateFastPath`: int32,
double and the call), and when it emits a type-specialized inline path it links the slow path to the non-repatching
operation. Nothing is patched afterwards, so SPEC-jit I2 is untouched.
*Tiers.* Baseline, DFG and FTL math IC sites (ValueAdd, ValueSub, ValueMul, ValueNegate ...). *Flag off, GIL on:*
unchanged (they regenerate). *Cost.* Larger code at sites that never run. *Failure mode:* none beyond code size.
*Test.* `jit/math-ic-double-operands-gil-off.js`: a loop whose accumulator turns double; counts
`operationValueAddProfiledOptimize` calls (one per iteration before, none after). *Expected gain.* 600 of 6,200
samples on megamorphic-put-transition; negligible on JetStream as measured. *Alternative.* Regenerate inside a stop
and retire the old snippet through the epoch: a stop per site for a rare pattern.

#### L-D6. The poly-proto allocation profile is one immutable record

**Status: proposed**

*Rule.* `ObjectAllocationProfileWithPrototype` publishes {structure, prototype} as one heap-allocated immutable
record through a single pointer (the call-link record pattern, SPEC-jit section 5.8): writers build, fence, store
the pointer; `clear()` stores null; records retire through `RetiredJITArtifacts`. Readers
(`createThisFromAllocationProfileGILOff`, DFG/FTL `CreateThis`) load the pointer once and read both fields through
it. A `.prototype` store that happens-before a construct has already nulled the pointer
(`clearAfterPrototypeStore`), which is the argument the mono-proto leg already uses.
*Ordering.* Publication fence + address-dependent reads (arm64 honours the dependency). *Collector.* The record's
two cells are reported by the profile's visitor as today. *Mid-way observation.* The old pair or null. *Flag off /
GIL on.* Field layout only. *Test.* `jit/poly-proto-create-this-gil-off.js`: constructions racing `.prototype`
stores on other function objects of the same executable; `createThis` slow-path counter and instructions per
construction before (940) and after (about 420). *Expected gain.* class-ctor-4 2.3x -> about 1.2x; programs that
define classes inside functions. *Alternative.* Keep the lookup but by offset: still a structure check per
construction.

#### L-D7. (Re)allocating transitions stay in generated code

**Status: proposed**

(a) The per-case Transition arm under tagged butterflies calls the shared `transitionHandlerImpl<allocating,
reallocating>` body (owner predicate, allocation through the thread's table, fill, claim, word re-check, tagged
publication, fence, new StructureID) after its condition checks, instead of
`operationPutByTransitionReallocatingConcurrent`; the operation remains the failure path. Registers: the arm already
allocates scratches for the non-reallocating form; the reallocating one needs four, available in the handler
calling convention. (b) The megamorphic store probe's `reallocating` exit GIL off calls an operation that performs
`tryCompleteCachedTransitionConcurrent` with the entry's {old, new, offset} (the cached-transition leg, which skips
the generic put's lookup) instead of the generic put.
*Protocol, ordering, collector:* exactly the shared handler's (SPEC-jit section 5.5, "(RE)ALLOCATING form"); nothing
new. *Tests.* The existing reallocating-transition tests gain a per-case variant (an object whose prototype chain
makes the conditions non-watchable); instructions per add in Baseline before (+20) and after. *Expected gain.*
Baseline and polymorphic sites: 20 instructions per reallocating add; megamorphic-put-transition about 1,000 of
6,200 samples.

#### L-D8. Map, Set and WeakMap reads outside the FTL

**Status: proposed**

DFG `MapGet`/`MapHas`/`SetHas` call a shared thunk that is the FTL's validated probe (version acquire, bounded walk,
load-load fence, version re-check; SPEC-jit section 5.8 "Map/Set get/has GIL off") instead of `operationMapGet`;
WeakMap `get`/`has` get the same inline reader in the FTL and the thunk in the DFG (the reader exists in C++:
`tryReadLockFreeGILOff`). Iterator `next` results are allocated inline through the thread's allocator table (the
inline-allocation rule). Mutation stays a call. *Ordering:* the reader's acquire and load-load fence are already
specified for arm64. *Expected gain.* Basic about 290 + part of 210 samples of 4,381; DFG-tier `Map.get` 309 -> about
100 instructions. *Test.* The existing seqlock tests run with `--useFTLJIT=0`.

#### L-D9. Structure tables readable without the structure's lock

**Status: proposed**

(a) The transition map becomes copy-on-write: inserts (already under `m_lock`) build a new table and publish its
pointer with a store-store fence; readers load the pointer and probe without the lock; old tables retire through
the safepoint epoch (readers are mutators and hold the pointer across no poll). Inserts are rare (a new shape),
reads are every C++ property add. (b) The mutator's property enumeration uses the seqlock-validated lock-free walk
that `Structure::getConcurrently` already uses for lookups, falling back to the locked walk on a changed stamp.
*Ordering.* Pointer publication + dependent loads (a); the existing seqlock discipline (b). *Collector.* Transition
tables hold weak structure references; the copy is registered with the same weak-map finalization. *Expected gain.*
json-parse-inspector about 150 of 2,546 samples; JSON.stringify 0.4 of its 1.22. *Risk.* (a) touches weak-reference
finalization of the transition map; needs its own TSan and gc-stress pass.

#### L-D10. `Array.prototype.join` measures and copies from a private snapshot

**Status: proposed**

GIL off, `arrayProtoFuncJoin`'s fast path copies the `length` lanes once (relaxed loads) into a
`MarkedArgumentBuffer`-backed vector and runs `JSOnlyStringsAndInt32sJoiner::tryJoin` over the copy: both passes see
the same values, so the buffer cannot be overrun, and a racing writer is linearized before or after the snapshot.
Strings are immutable; ropes in the snapshot resolve under the existing GIL-off rope rule. *Expected gain.* UniPoker
about 370 of 4,733 samples. *Test.* `shared-objects/array-join-vs-writer-gil-off.js` (a joiner racing a writer that
lengthens elements; result is always a join of some per-lane value; no crash), and instructions per `join` of a
hundred short strings before and after.

### L: arm64 / non-Linux notes

- L-D4: the quick-entry probe needs an acquire load of the key (or a load-load barrier before the value load);
  x86-64 needs nothing. The writer's fence is already `storeStoreFence`.
- L-D6, L-D9(a): pointer publication with `storeStoreFence` and address-dependent reads through the pointer; sound
  on arm64 as long as the reads are data-dependent on the loaded pointer (they are), with `WTF::Dependency` in C++.
- L-D2: the phase byte is read with plain loads; its single write is ordered for the new mutator by native thread
  creation and for compiler threads by the worklist lock; jettisoned code needs the usual instruction-cache
  maintenance and the per-mutator instruction barrier on resume (SPEC-jit F5), which only the switching thread
  needs here.
- L-D3(a): the fused predicate is `eor`/`tst #0xffff000000000000`/`b.ne` on arm64 (the mask is a valid logical
  immediate); the tag load is from `tpidr_el0`-relative storage (annex R5) and is loop-invariant.
- L-D3(b): same-thread data, no ordering.
- L-D1: the poll on arm64 is an address materialization (two to four instructions) plus `ldrb`/`tst`/`b.ne`, so an
  inlined-entry poll costs more there than on x86-64 and the rule is worth more.
- L-D5 avoids cross-modifying code altogether; the rejected alternative (regenerate in a stop) would need the
  arm64 cross-modifying-code sequence.
- The existing sequences read in L-1 rely on nothing new: the butterfly mask and owner test are single-word reads;
  the structure -> butterfly dependency (SPEC-jit F7) is a no-op on x86-64 and must be present in the arm64
  lowering of every sequence shown (not visible in an x86-64 disassembly; to be checked when arm64 builds exist).
- macOS and Windows: thread-local access for the tag and the lite is per platform (annex R5; Windows unsupported
  flag on); nothing in this section adds a platform dependency.

### L: Decisions for the user

1. **Is single-threaded speed of a GIL-off process before its first spawn a goal?** Option A: no - drop Open item
   (7); the JetStream GIL-off gate keeps measuring tagged, synchronized code; effort goes to L-D1, L-D3..L-D10 and
   the other sections (they help threaded programs too). Option B: yes, tags only (L-D2 scope A): about 5 % of
   instructions for a process-wide mode with a memory-safety failure mode; not recommended. Option C: yes, fully
   (scope B): up to 0.95 of GIL on before the first spawn, a classification of about 980 gates, two live copies of
   the object model GIL off, and the JetStream gate must then be run with an idle spawned thread to keep meaning
   what it means now. Recommendation: A now; revisit C only if GIL off becomes the only flag-on mode that ships.
2. **L-D1 changes where flag-on code polls** (both GIL modes when traps are polled): inlined callee entries stop
   polling. Recommendation: accept; stop and termination latency are bounded as argued.
3. **If L-D2 is ever built: carrier TID 0 for the first carrier, or a re-tagging heap walk at the switch.**
   Recommendation: TID 0 with the audit of lite-less threads; the walk is the fallback.
4. **The micro set's class-ctor-4 and astar-like-nodes rows measure the poly-proto slow path** (the harness
   re-creates the constructor in every timed call). Recommendation: keep them (they guard L-D6) and add mono-proto
   variants so that the gate also measures constructor transitions.

### L: Doc mismatches

- PERF-RESULTS sections 1 and 6.12, LANDING-PLAN parity table: the rows `class-ctor-4` and `astar-like-nodes` are
  described as constructor/transition rows; they measure `operationCreateThis` on a poly-proto function (1.0 M of
  1.2 M constructions take it in every configuration, `main` included). Fix: say so next to the rows; add mono
  variants.
- SPEC-jit history section 52 (and the comment in `FTLLowerDFGToB3.cpp`, MakeAtomString): the reason given for not
  probing the concat-key cache's quick entries GIL off - a racing entry write between the key compare and the value
  load - does not match `ConcatKeyAtomStringCacheInlines.h`, where each quick slot is written once under the lock,
  value before key with a fence, and whose own comment calls the ordering "defensive" because the JIT no longer
  reads the entries. Fix: either adopt L-D4 or restate the reason as "not yet re-enabled".
- LANDING-PLAN Open items: the math inline caches' GIL-off behaviour (`JITMathIC::generateOutOfLine` returns without
  regenerating; "the chartered follow-up" per its comment) is not listed anywhere. Fix: add it (L-10).
- LANDING-PLAN Open items, GIL off (7) ("the broad remainder: tag predicates and polls"): by L-0 the tags are 1.05
  and the poll sequence 1.06 of 1.30; the larger remainder (1.16) is GIL-off-only protocols and C++ bodies. Fix:
  reword (7) with the decomposition.
- SPEC-jit section 5.5 "Inline allocation GIL off" lists the converted sites; `JSGenerator` (`operationNewGenerator`
  in Basic) and the iterator-result object under `JS{Map,Set}Iterator::next` are further members found by the rule's
  own method (the operation's name in a GIL-off profile).

### L: What was run

All on the existing Release binary of the final tenth-round tree (and `main`'s for two profiles); nothing was built.
- `perf record -e instructions:u -c 2000000` of Basic, WSL, json-parse-inspector, crypto, raytrace (GIL on, GIL
  off), and of async-fs, UniPoker, OfflineAssembler (GIL off; GIL on tagged+polling for all eight); first-iteration
  recordings of three tests.
- `perf stat -e instructions:u`: fifteen JetStream tests in five configurations (L-0), fourteen with
  `testIterationCount=1` in two; the micro set's rows one file each in three configurations; twenty access loops x
  four tier settings x two modes x two loop counts.
- FTL/DFG dumps with address ranges plus a debugger-attached memory dump and `objdump` for seven small scripts;
  `--dumpFTLDisassembly` with simultaneous `perf record` for eight JetStream tests (node attribution, poll
  classification).
- `--reportJSThreadsCounters=1` on four scripts.

## Section M. GIL on: what still separates it from flag off

Scope: a process started with `--useJSThreads=1` and the GIL (the default with the flag: `useThreadGIL=1`,
`useTaggedButterflies=0`). After the tenth round JetStream is 0.958 of flag off (instructions 1.015 in sum) and the
JSC suites differ from flag off by the 34 `Atomics`-on-objects results. This section finds where the rest is, by
instruction counts, locked-operation counts and per-symbol profiles of the tenth round's final Release build against
`main`, and says how each piece can take `main`'s form.

All numbers are `perf stat -e instructions:u` (whole process) unless said otherwise; "locked ops" is
`mem_inst_retired.lock_loads:u`. JIT samples were given names with the engine's jitdump option
(`--useJITDump=1`, `perf record -k mono`, `perf inject --jit`), which names every code block by tier
(`JSC-FTL:`, `JSC-DFG:`, `JSC-Baseline:`) and every shared IC handler and stub (`JSC-InlineCache:`) - a method the
earlier rounds did not have; it needs no disassembler in the build.

The options a GIL-on process has that flag off does not (from `--dumpOptions=2`): `useHandlerICInFTL`, `useJSThreads`,
`useThreadGIL`, `useSharedAtomStringTable`, `useVMLite`, `useStructureAllocationLock`. Nothing else differs, so every
entry below hangs off one of those six or off a code gate that reads `Options::useJSThreads()` directly. There are
still 248 such direct gates under `bytecode/`, `dfg/`, `ftl/`, `jit/`, `llint/` (95 / 86 / 34 / 28 / 5) against 84
reads of the derived `useTaggedButterflies()`; the tenth round re-keyed the ones about the butterfly tag. What is left
is listed by what it emits.

### M: Inventory

#### Verdict per row

JetStream rows that were below 0.95 GIL on over flag off in the quiet pass, re-measured by instruction count (three
runs per cell, more where stated; G = 10^9):

| test | quiet-pass score ratio | flag off (G instr) | GIL on (G instr) | ratio | verdict |
|---|---|---|---|---|---|
| WSL | 0.928 | 43.4 / 44.4 / 45.1 | 46.5 / 48.5 / 48.6 | 1.09 | real: FTL handler ICs (M-I1) |
| Air | 0.911 | 5.49 / 5.53 / 5.54 | 5.83 / 5.83 / 5.83 | 1.054 | real: FTL handler ICs account for 3.4 of the 6 points (M-I1) |
| earley-boyer | 0.891 | 5.80 / 5.81 / 5.86 | 6.00 / 6.11 / 6.24 | 1.05 | real in count, not located: the FTL code of one function (`rewrite_nboyer`) takes 1.5x the samples while the FTL total falls; bimodal on `main` too (M-I10) |
| typescript | 0.932 | 32.3 / 32.5 / 32.8 | 32.9 / 33.1 / 33.2 | 1.017 | real, small: FTL handler ICs |
| json-stringify-inspector | 0.877 | 2.956 x3 | 2.997 x3 | 1.014 | not an instruction or cycle difference (process elapsed 0.287 s against 0.294 s); short string-heavy test, M-I4 and M-I8 are what is measurable |
| FlightPlanner | 0.838 | 6.01-6.07 or 7.43-7.70 | 6.14-6.15 or 7.29-7.83 | 1.015-1.02 within a mode | bimodal in all three configurations at the same rate (slow mode 3 of 6 on `main`, 4 of 6 flag off, 4 of 6 GIL on); the quiet-pass ratio is mode luck |
| navier-stokes | 0.846 | 3.95 (one run 4.57) | 3.96-3.97 | 1.003 | same code (identical counts under a synchronous JIT); a timing-selected mode, GIL on landed in the slower one 14 of 14 times (M-I10) |
| Babylon | 0.908 | 5.78-7.01 (bimodal) | 5.89-6.30 | inside flag off's range | noise |
| async-fs | 0.922 | 2.782-2.799 | 2.760-2.788 | 0.992 | noise |
| octane-code-load | 0.924 | 3.428 x3 | 3.434-3.435 | 1.002 | noise by instructions; +0.8 M locked ops (M-I8) |

Micro rows above 1.05 GIL on over flag off:

| row | time ratio (quiet pass) | instructions off -> on | locked ops off -> on | verdict |
|---|---|---|---|---|
| class-ctor-4 | 1.31 | 3.67 G -> 4.87 G (1.33) | same | M-I1 (0.84 G of the 1.20 G), M-I3 (`DirectConstruct` lost, record calls) |
| transition-heavy-constructor | 1.24 | 1.240 G -> 1.345 G (1.08), cycles 1.20 | same | M-I2 (store elimination lost) |
| map-set-get-2M | 1.19 | 30.24 G -> 30.27 G (1.001), cycles 1.21-1.28 | 99.6 M -> 193.0 M | M-I4 |
| regexp-exec-1M | 1.13 | 11.25 G -> 11.25 G (1.000), cycles 1.10-1.15 | 25.8 M -> 43.4 M | M-I4 (the RegExp entry points themselves cost the same GIL on as flag off) |
| astar-like-nodes | 1.15 | 1.35-1.56 G -> 1.60-1.98 G | same | FTL code +143 samples of 1,483, `PutByIdSloppy` stubs +35: M-I1/M-I2 family; run-to-run spread is as large as the difference |
| out-of-line-replace-poly-3M | 1.09 | 1.57 / 1.59 / 2.42 G -> 1.66 / 2.81 / 2.82 G | same | bimodal in every configuration (`main`: 1.59 / 1.59 / 2.01); not a GIL-on cost |
| add-props-escaped | 1.06 | 0.822 G -> 0.890 G | same | +5.6 instructions per iteration = one non-inlined call through a record (M-I3) |

#### M-I1. Property inline caches in FTL code are handler chains (explained)

*Mechanism.* `Options::notifyOptionsChanged` forces `useHandlerICInFTL` whenever `useJSThreads` is set (the "SPEC-jit
M2b" block); flag off the same function forces it off. So every `GetById`/`PutById`/`InById`/`InstanceOf`/by-val
site in FTL code is a data IC: load the handler from the site's cache, call its entry, each handler checks one case
and tail-jumps to the next (`InlineCacheCompiler::compileOneAccessCaseHandler`), where flag off the FTL allocates a
`RepatchingPropertyInlineCache` (`FTL::State`), patches the site's jump and regenerates one stub that holds all the
site's cases behind a binary switch, with the identifier and its hash baked in. The handler form also makes the site
clobber every volatile register (the comment in `InlineCacheCompiler.cpp`: "FTL handler-IC sites late-clobber every
volatile register so B3 must spill all live state"), which shows up inside the FTL code, not in the handlers.

*Evidence.* (a) The option can be forced flag off (`--useJSThreadsUnlockHandlerICInFTL=1 --useHandlerICInFTL=1`):
class-ctor-4 3.67 G -> 4.51 G (GIL on 4.87 G); a put site that sees six structures, two puts per iteration:
+21.8 instructions per put; Air 5.46-5.59 G -> 5.66-5.68 G (GIL on 5.86-5.92 G); by JIT category on Air (samples of
2 M instructions): inline-cache handlers 426 -> 471 -> 563 (flag off -> forced -> GIL on), FTL code 589 -> 619 -> 584.
(b) WSL by category, flag off -> GIL on: inline-cache code 1,703 -> 2,629, FTL code 3,545 -> 4,019; by symbol
`GetById Getter handler` 172 -> 600, `GetById Load handler` 104 -> 482, `InById` stubs and handler 78 -> 404,
`PutById Transition handler` 43 -> 122, and the per-site compiled `GetById` stubs 819 -> 391. (c) Air: `GetById
Megamorphic Getter handler` 22 -> 154-198 samples, called from the FTL code of `visitArg` and `forEachArg` (call-graph
profile); the same sites flag off are per-site stubs. Compile and exit counts are the same in the two configurations
(WSL: 1,622 / 1,665 DFG and 452 / 462 FTL compilations, 124,358 / 125,652 exits), so this is steady-state cost, not
recompilation.

*Worth.* WSL +4 to +9 % instructions, Air +3.4 %, typescript about +1.5 %, class-ctor-4 +23 of its 33 points. It is
the largest single piece of the 1.0 % of generated-code samples that PERF-RESULTS 6.12 lists as "what is left".
The earlier note that it is "a wash as a rule" rested on Babylon -15 %; Babylon is bimodal flag off (5.78, 5.83,
6.18, 6.80, 7.01 G in five runs; forced handler ICs 5.66-6.92 G; GIL on 5.89-6.30 G) and says nothing either way.

#### M-I2. Inlined transitions in FTL code keep a fence that stops store elimination; CreateThis re-reads its pair (explained)

*Mechanism.* `FTL::LowerDFGToB3::compilePutStructure` and the transition arm of `compileMultiPutByOffset` emit
`m_out.fence(&m_heaps.root, nullptr)` before the structure-ID store under `Options::useJSThreads()`, not under
`useTaggedButterflies()`. On x86-64 the fence is no instruction, but it is a B3 `Fence` that reads the whole heap: B3
can no longer remove a structure-ID store that the next transition overwrites. A constructor that adds twelve
properties keeps twelve `Move32 $id, (%obj)` where flag off keeps one. `compileCreateThis` (FTL), `JIT::emit_op_create_this`
(Baseline) and the DFG's form load the allocation profile's structure, the allocator, and the structure again, and
take the slow path unless the two structure loads agree (a torn pair under a racing `clear()`); also keyed on the flag
alone. Both guard against a second mutator inside the window; a GIL-on process has none (OM G1's rule).

*Evidence.* Final Air (after `optimizeBlockOrder`) of the FTL code of the twelve-property `make()` of
`bench/transition-heavy-constructor.js`, synchronous JIT: 86 lines flag off, 108 GIL on; the diff is twelve
`StoreFence` and eleven `Move32 $id, (%rax)`. jitdump: FTL `make` 929 -> 1,035 samples of 0.5 M instructions, nothing
else moves; instructions 1.240 G -> 1.345 G, cycles 372-406 M -> 464-471 M. The DFG graphs are node-for-node equal
(8 `PutStructure`, 8 `PutByOffset`, 1 `NewObject` in the dumped function in both).

*Worth.* transition-heavy-constructor 1.24 -> about 1.00; every constructor inlined into FTL code that adds more
than one property (the object-heavy JetStream tests; not separable from M-I1 there without a build). The DFG's
`storeFence()` is empty on x86-64, so the DFG tier pays nothing; arm64 pays a `dmb ishst` per transition in both tiers.

#### M-I3. Calls go through call-link records; known callees are data ICs; the FTL loses one DirectCall conversion (explained)

*Mechanism.* (a) `CallLinkInfo::emitFastPathImpl` under `Options::useJSThreads()`: load `m_record`, test, load
`comparand`, compare, load `codeBlockToTransfer`, store, load `target`, call - eleven instructions where flag off's
legacy-field form is eight (SPEC-jit 5.8). The LLInt's `.opCallThreadedRecord` likewise. (b) `DirectCall` /
`DirectConstruct` / `DirectTailCall` pass `UseDataIC::Yes` flag on (`DFGSpeculativeJIT64.cpp`,
`FTLLowerDFGToB3.cpp::compileDirectCallOrConstruct`), so a call whose callee the compiler knows is `load record; test;
load; store; load; call` instead of flag off's patched near call. (c) Upstream's `DFGStrengthReductionPhase`
(`case Call/Construct/TailCall...`) breaks out early when `m_graph.m_plan.isFTL() && Options::useHandlerICInFTL()`;
because the flag forces that option, FTL plans never run the strength-reduction conversion of a `Call` whose callee
became a constant after parsing (a closure allocated in the same function, a constant found by folding) into
`DirectCall`, nor the `CallWasm` / `CallFFI` conversions behind it. The parser's own conversion
(`ByteCodeParser::handleCall` -> `convertToDirectCall`, for a monomorphic site that was not inlined) is not affected,
so GIL-on FTL graphs do contain `DirectCall` nodes (Air: 106 in both configurations) and the data-IC lowering of
`DirectCall` in the FTL is exercised on every run. A report that "the FTL converts no call to DirectCall flag on" is
too strong: only the strength-reduction conversions are lost.

*Evidence.* Per call, GIL on minus flag off: FTL call of a non-inlined global function (parser `DirectCall`) +5.05
instructions (40 M calls: 1,454.0 M -> 1,656.0 M); three-way polymorphic call +3.4; forty-way (virtual) +4.9; `new` of
a non-inlined class +8.1; LLInt call +6.1 (`--useJIT=0`, 5 M calls: 1,653 M -> 1,684 M); Baseline call +1.9. The lost
conversion: a loop calling an arrow function created in the enclosing function, inlining switched off by option: 1.458 G
-> 2.564 G for 40 M calls, +27.7 instructions per call - the generic call is monomorphic on the callee cell, a new
closure per outer call misses it, and the site ends on a polymorphic call stub. FTL graph counts (one run each):
richards `DirectCall` 4 flag off / 2 GIL on; delta-blue 16 + 1 `DirectConstruct` / 7; class-ctor-4's `new P(i)`
`DirectConstruct` / `Construct`.

*Worth.* (a)+(b): 3-5 instructions per JS-to-JS call that is not inlined, in every tier; richards 1.02, delta-blue
1.01, add-props-escaped 1.06-1.08 are this and nothing else. (c): site-specific, +28 per call where it bites (a closure
too large to inline called in a loop); JetStream's polymorphic-call-stub samples do not move (Air 35 -> 37, WSL 72 ->
73), so its suite value is small.

#### M-I4. Every string pays locked read-modify-writes (explained)

*Mechanism.* `useJSThreads` sets `useSharedAtomStringTable`, and `JSC::initialize` latches
`WTF::g_sharedAtomStringTableEnabled`. With the latch on, `StringImpl::deref` always performs the release
`fetch_sub` (`lock xadd`), where the legacy arm skips the read-modify-write when the count it loads is one;
`StringImpl::cost` sets its "reported" bit with `fetch_or` (called by `JSString::create(VM&, Ref<StringImpl>&&)` for
every string cell); `StringImpl::setHash` publishes the lazily computed hash with `fetch_or` (from
`hashSlowCase`). All three are there for the GIL-off and multi-VM case (two threads may hold the same `StringImpl`,
and a table hit may try to revive an atom at count one). Instruction counts do not change; each locked operation costs
17-29 cycles here.

*Evidence.* map-set-get-2M: instructions 30.24 G -> 30.27 G, locked ops 99.6 M -> 193.0 M, cycles 12.9 G -> 15.3-16.7
G; regexp-exec-1M: 11.25 G both, 25.8 M -> 43.4 M, 2.6-2.8 G -> 3.0 G. Where the extra 93.4 M of map-set-get sit
(samples of 10,007 locked loads, GIL on minus flag off): the two `JSString` / `JSRopeString` sweep specializations
(the `deref` in the destructor) +4,480, `JSString::create` (`cost`) +2,259, `StringImpl::hashSlowCase` (`setHash`)
+2,353: 48 % / 24 % / 25 %. JetStream: Babylon +2.2 M locked ops, UniPoker +2.4 M, regexp +1.5 M, json-parse-inspector
+1.4 M, octane-code-load +0.8 M, json-stringify-inspector +0.2 M, navier-stokes +0.06 M - at 20 cycles each 0.7 to
2.3 % of those tests' cycles, nothing elsewhere.

*Worth.* map-set-get 1.19 and regexp-exec 1.13 entirely; 1-2 % on the string- and parse-heavy JetStream tests. The
same cost is in GIL off.

#### M-I5. Scope metadata stays frozen: a put to a global property, and a global lexical binding declared late, take the slow path every time (explained; a cliff, not a JetStream cost)

*Mechanism.* `CommonSlowPaths::tryCachePutToScopeGlobal` returns at once under `Options::useJSThreads()`, and
`tryCacheGetFromScopeGlobal` refuses the GlobalProperty -> GlobalLexicalVar rewrite (`if (threaded) return`), both
because the LLInt and Baseline fast paths read `{getPutInfo, structureID, operand}` without a lock (SPEC-jit history
section 28). The put side never caches, in any tier: the LLInt and Baseline call the slow path per put, and the DFG,
which takes the structure from the same metadata, compiles a generic put.

*Evidence.* (5 M iterations; `main` / flag off / GIL on.) `counter = i` where `counter` is a property of the global
object: 0.284 G / 0.285 G / 3.994 G (+742 instructions per put, 14x) with all tiers, 0.815 / 0.819 / 5.676 G without
the DFG, LLInt only (2 M) 0.486 / 0.498 / 2.482 G. A sloppy implicit global written and read: 0.278 / 0.282 / 7.547 G
(27x). A function that first ran before `let lateLex` was declared by a later script: 0.254 / 0.250 / 2.908 G (+531
per read, 11.6x). A global-property read: 0.255 / 0.252 / 0.276 G (the get side was fixed in the fifth round).

*Worth.* No JetStream test reaches these slow paths (no `put_to_scope` symbol above 0.05 % in any profile taken), so
the suite does not see it. A program that keeps a counter in an implicit global does, by an order of magnitude.

#### M-I6. Inline caches refuse dictionary structures and the inlined prototype-load form (explained; a cliff)

*Mechanism.* `PropertyInlineCache::addAccessCase` gives up on any case whose structure is a dictionary under
`Options::useJSThreads()` (counter `icDictionaryStructureRefused`), in every tier, and the LLInt's publish gates
mirror it; `main` caches cacheable dictionaries. `HandlerPropertyInlineCache` does not install the call-site inlined
form for a `GetByIdPrototype` handler flag on (the holder cannot pack into the single `{offset, structureID}` word of
SPEC-jit 4.2), so the first prototype hit of a Baseline or DFG site goes through the chain.

*Evidence.* A read of one property of a 200-property object that had a property deleted, 5 M times: 0.241 G /
0.242 G / 1.421 G (+236 per read, 5.9x); a replace on it: 0.264 / 0.254 / 1.895 G (+328, 7.5x). A method call through
the prototype in Baseline code: +10.4 instructions per iteration (record call plus chain dispatch), nothing in the
DFG. JetStream: `icDictionaryStructureRefused` is 0 in 23 of 28 tests counted and at most 6 (pdfjs), and the
slow-path counters agree between the configurations to within 2 % everywhere (`icGetByIdOptimize` Air 34,891 / 34,502,
pdfjs 315,493 / 315,439), so the suite does not see this one either.

*Worth.* Objects used as string-keyed maps, and anything after `delete`: a 6-7x slowdown per access with the flag on.

#### M-I7. `CallWasm` is off with the flag (quantified; the design belongs to the WebAssembly section)

`DFGStrengthReductionPhase` refuses the `CallWasm` conversion under `Options::useJSThreads()` (and M-I3(c) would stop
it in the FTL anyway). JetStream `richards-wasm`: 4.52 G -> 6.61 G instructions (1.46), FTL graph 5 `CallWasm` / 0;
`HashSet-wasm` 3.62 -> 3.67 G. The warm JS-to-wasm entry (handed out until the first spawn) does not make up for it.

#### M-I8. Atomization through the shared table, the concat-key cache (small; not re-measured in detail)

`addToStringTable` takes the shard lock per atomization (two locked operations on an uncontended lock) and
`operationMakeAtomString2WithCache` still has 120 -> 179 samples in WSL. Together they are the +0.8 M to +2.4 M locked
operations of the parse-heavy tests in M-I4's list. Left as is: the shared table is what makes an identifier one
pointer on every thread, and a GIL-on process does spawn threads.

#### M-I9. Baseline profile write avoidance (measured: below noise)

Flag on, Baseline value- and array-profile sites compare before they store (`JIT::emitValueProfilingSite`,
`useSharedProfileWriteAvoidance`). Switched off GIL on: Air 5.852 / 5.891 G -> 5.836 / 5.856 G, octane-code-load,
first-inspector-code-load and typescript unchanged to four digits. Not worth a rule.

#### M-I10. Rows decided by compile timing (partly explained)

- *navier-stokes.* Two modes exist in every configuration: the Average sub-score is about 1,730 or about 1,340.
  `main` 4 fast of 5, flag off 5 of 5, GIL on 0 of 14; flag off drops into the slow mode under any perturbation that
  slows start-up (`perf stat`, `--reportCompileTimes`, 1 of 3 with `--useHandlerICInFTL` forced, 2 of 3 with
  `--useVMLite=1`). With `--useConcurrentJIT=0` all three configurations execute 3.857 G instructions and 1.78 G
  cycles, in the slow mode: the code is the same, and the fast mode is a product of concurrent-compile timing. What
  distinguishes the modes: in the fast one the first FTL version of `lin_solve` takes three InadequateCoverage exits
  at the first arm of `if (a === 0 && c === 1)` (bc#19) and is replaced; it then runs MORE instructions in FEWER cycles
  (4.57 G / 1.51 G against 3.95 G / 1.85 G). Why GIL on never reaches it is not established (start-up is 8-10 % slower
  GIL on by the Startup sub-score, which is the kind of shift that moves flag off as well). Worth 15 % of one test,
  0.45 % of the geometric mean. Next cheapest experiment: a build with an in-memory ring of tier-up events (plan
  enqueued / installed / cancelled, with the profile's coverage at enqueue time) dumped at exit - every logging option
  tried perturbs the run into the slow mode.
- *FlightPlanner*, *Babylon*, *out-of-line-replace-poly*: bimodal by instruction count in all three configurations at
  similar rates; GIL on is inside flag off's range.
- *earley-boyer*: +5 % instructions GIL on in three runs of three, but the per-function profile moves both ways between
  runs (FTL `rewrite_nboyer` 976 -> 1,489 and 1,057 -> 1,544 samples in two pairs while all FTL code together fell);
  bimodal on `main` (LANDING-PLAN). Not located.
- *Basic*: 26 jettisons flag off, 50 GIL on in one counted run; the score ratio is 0.944. Not followed.

#### M-I11. Behaviour: `Atomics` accepts ordinary objects whenever the flag is on (34 suite results)

`atomicReadModifyWrite`, `atomicStore`, and the wait / waitAsync / notify entry points in `AtomicsObject.cpp` route a
first argument that is an object but not a typed-array view to the property path under `useJSThreadsEnabled()`
(SPEC-api 4.5). `stress/SharedArrayBuffer.js` and `SharedArrayBuffer-opt.js` expect `Atomics.store({}, 0, 0)` and
`Atomics.notify({}, 0, 0)` to throw TypeError; with the extension the first creates the property and the second
returns 0 (the read-modify-write forms still throw, because `{}` has no own property "0"). Two files, seventeen
configurations each. Decision below.

#### M-I12. Standing items that are not costs

- ThreadSanitizer builds keep polling traps GIL on (SPEC-jit history section 53), so the signal-delivered path is
  covered by the Release and Debug lanes only. Stays.
- Upstream's benign races GIL on (AUDIT R10-23): the corpus found two. A wider run is a plan, below.
- `--useJSThreadsSingleOwnerWithGIL=0`: three corpus tests use it to observe per-thread ownership with the GIL.
  Decision below.

### M: Designs

The rule every design below rests on is the one OM G1 and SPEC-jit history sections 52-53 already use: **in a GIL-on
process one thread runs JS at a time, the GIL changes hands only inside blocking calls (join, contended hold, wait,
`Atomics.wait`, and `JSLock::DropAllLocks` in host code), and no generated fast path, inline-cache window or slow-path
rewrite spans a call.** A writer and a reader of any mutator-only datum are therefore the same thread at different
times, or different threads ordered by the lock hand-off (a release and an acquire). What runs concurrently with the
mutator is what runs concurrently on `main`: compiler threads (which read under `CodeBlock::m_lock` or tolerate racy
profiles), collector threads, the sampling profiler and the trap signal sender. `main` itself is entered from several
threads under the API lock by embedders, so every `main` form below is already exercised with exactly this hand-off
discipline.

Each design is keyed on a process-wide derived option computed once in `Options::notifyOptionsChanged`, as
`useTaggedButterflies` is. Proposed name for the new one: `useJSThreadsConcurrentMutators` = `useJSThreads &&
!useThreadGIL` (true exactly in a GIL-off-capable process); the designs say "concurrent mutators" for it. Flag off
sees nothing in any of them: every gate they touch already reads a flag-on option, and the new byte is declared next
to the existing ones.

#### D-M1. FTL property inline caches repatch, as on `main`, unless mutators are concurrent

**Status: proposed.**

*Rule.* `useHandlerICInFTL` is forced on only with concurrent mutators (and stays forced off otherwise, as on `main`).
A GIL-on FTL plan allocates `RepatchingPropertyInlineCache`s, its sites are `main`'s patchable jumps,
`InlineCacheCompiler` regenerates one stub per site (the non-handler path, `regenerate`), and
`PropertyInlineCache::rewireStubAsJumpInAccess` / `resetStubAsJumpInAccess` patch. SPEC-jit I3 ("no
`RepatchingPropertyInlineCache` constructed") and the two release assertions that enforce it
(`PropertyInlineCache.cpp`, the repatching constructor) become GIL-off-only, as I2's patching assertions did for traps
in the tenth round. Baseline and the DFG keep handler ICs (they have them on `main`).

*Who writes, who reads.* Writer: the mutator that misses, under `CodeBlock::m_lock`
(`GCSafeConcurrentJSLocker`), exactly as flag off; it rewrites the site's jump target (one aligned store through the
JIT write path) and the `m_handler` / stub pointers. Readers: mutators executing the site. With the GIL they are never
concurrent with the writer. A thread parked in a blocking call has JS frames whose return addresses may lie (i) in the
site's slow-path call sequence, which is not patched, or (ii) inside the OLD stub (the stub called a getter or setter
that blocked). (ii) is `main`'s re-entrancy case: a stub that makes calls is a `GCAwareJITStubRoutine`, kept alive
while any scanned stack holds an address inside it, and every thread that has taken the API lock is in
`MachineThreads`, so parked threads' stacks are scanned. Compiler threads read IC state under `m_lock` as on `main`.

*Memory ordering.* x86-64 and arm64 alike: the patcher writes through the JIT write path and, on arm64, performs the
instruction-cache maintenance `main`'s patcher performs (`MacroAssembler::repatchJump` and friends). The next executor
of the site is the same thread, or a thread that acquired the GIL after the patcher released it (release / acquire on
the lock word orders the patch before the execution). What arm64 additionally needs between a patch and another
core's execution of it is what `main` already needs when an embedder enters one VM from several threads under the API
lock, and `main` meets it by patching only the instruction forms the architecture allows to be modified while another
core may execute them (direct branches and the jump-replacement forms) after cache maintenance; a GIL-on process
patches exactly the same forms from exactly the same code. No new fence.

*Collector, stops, watchpoints.* Stub lifetime as above. Watchpoint fires that reset ICs
(`PropertyInlineCacheClearingWatchpoint`) run under the flag-on Class-A stop; resetting a repatching IC inside a stop
is a patch with every mutator stopped - strictly safer than flag off. Deferred claims are a GIL-off protocol and are not
involved. Jettison is already world-stopped flag on (I8).

*What a second thread can observe.* Nothing new: it cannot run between the first and last store of a patch.

*Failure modes.* (1) A site patched while another mutator runs: only possible if the process is not what the option
says. Detected by a release assertion in the repatching constructor and in `rewireStubAsJumpInAccess`:
`!useJSThreadsConcurrentMutators`. (2) A handler-IC-only assumption elsewhere flag on (`FTL::JITCode`'s handler-IC
data, `FTLJITFinalizer`, the dummy array profile in `CodeBlock.cpp`): all are keyed on `useHandlerICInFTL` itself, not
on the flag, so they follow the option. (3) The strength-reduction break of M-I3(c) reads the same option and lifts by
itself GIL on; see D-M3.

*Tests.* `JSTests/threads/jit/gil-on-ftl-repatching-ic-across-handoffs.js`: three threads take turns (join, hold, wait)
through one FTL function whose get, put, in, instanceof and by-val sites each see a new structure per turn, including a
getter that blocks inside the stub while another thread regenerates it; counts the site's slow-path calls
(`icGetByIdOptimize` and friends) and checks results. Before: handler counts; after: the same results, and
`--dumpOptions` shows `useHandlerICInFTL=false` GIL on. The existing handler-IC-in-FTL tests get `requireOptions` for
GIL off, where the path stays. Microbenchmark gate: class-ctor-4 and a six-structure put site GIL on within 1.10 of
flag off (now 1.33 and 1.20).

*Verification.* Corpus four modes; JSC suites GIL on against `main` (the FTL property-IC paths run `main`'s code, so
no new class is expected); TSan GIL on; the amplifier on the new test.

*Expected gain.* WSL -4 to -8 % instructions, Air -3.4 %, typescript -1.5 %, class-ctor-4 1.33 -> about 1.10,
astar-like-nodes toward 1.05. Suite: about half of the 1.5 % instruction gap.

*Risks.* The FTL handler-IC lowering then runs only GIL off, so it loses the GIL-on test lanes' coverage; the corpus
and the GIL-off JSC suite still run it.

*Rejected.* Making the handler chain cheaper (a per-site polymorphic handler): a second IC design, for a
configuration where patching is safe. Leaving handler ICs and removing only the register clobber: the chain walk and
the shared thunks' loads remain (Air's megamorphic getter handler alone is 5 %).

#### D-M2. Inlined transitions and CreateThis take `main`'s form in an untagged process

**Status: proposed.** (A G1 follower: two gates that history section 52 meant to move and did not.)

*Rule.* The store-store fence before the structure-ID store in `FTL::LowerDFGToB3::compilePutStructure`,
`compileMultiPutByOffset`, and their DFG counterparts is emitted under `useTaggedButterflies()`, not `useJSThreads()`.
The structure / allocator / structure re-read in `compileCreateThis` (FTL, DFG) and `JIT::emit_op_create_this`
(Baseline), and whatever the LLInt's `create_this` does for it, likewise.

*Argument.* The fence orders the value store before the structure publication for a READER ON ANOTHER THREAD that
structure-checks and then loads (E4 / M5). Untagged there is none; the collector's concurrent marker is served by the
nuke-and-fence protocol of reallocating transitions (`nukeStructureAndSetButterfly`, unchanged) and by `main`'s rule
that a non-reallocating transition's value store may be seen in either order by the marker (it visits the slot either
way: the slot is inside the already-published capacity). That is `main`'s code, which has no fence here on any
architecture. `FunctionRareData`'s profile is cleared and refilled only by mutators (a prototype store, the first
fill) and by the collector with the world stopped, so untagged no reader overlaps a writer.

*Memory ordering.* x86-64: nothing is emitted either way; the change is what B3 may do (dead-store elimination,
store motion), which is what `main`'s B3 does. arm64: removes a `dmb ishst` per transition and a `dmb ishld` pair per
`create_this` GIL on; GIL off keeps them.

*Failure modes.* A hidden reader that did rely on the order GIL on: would be a reader on a thread other than the GIL
holder, which the corpus' GIL-on TSan lane reports as a race on the structure ID. None is known.

*Tests.* A test cannot count emitted stores, so the test is for correctness and the count goes into the history
entry. `jit/gil-on-inlined-transitions-across-handoffs.js`: a twelve-property constructor and a `create_this` site
whose callee's `prototype` is replaced between turns, run by three threads taking turns (join, hold, wait) in every
tier; every object has all twelve properties, the final structure and the right prototype. Count recorded before and
after: the final Air of the constructor's FTL code (108 lines with 12 `StoreFence` and 12 structure-ID stores now;
86, 0 and 1 flag off). Bench gate: transition-heavy-constructor GIL on within 1.03 of flag off (now 1.08 in
instructions, 1.24 in time).

*Expected gain.* transition-heavy-constructor to parity; a share of every FTL-inlined constructor (not separable
from D-M1 without a build; the same sites).

*Risks.* None beyond a wrong gate: a site moved that is about concurrency other than mutator-mutator. Both sites are
about exactly that by their own comments.

#### D-M3. Known-callee calls: lift upstream's FTL break flag on; then call linking in `main`'s form without concurrent mutators

**Status: proposed**, two steps that land separately.

*Step 1 (one line).* In `DFGStrengthReductionPhase`'s call case the early break becomes
`isFTL() && useHandlerICInFTL() && !Options::useJSThreads()`. What the break protects upstream is the unfinished
handler-IC FTL mode, where a `DirectCall` would be the only patched code; flag on a `DirectCall` in the FTL is a data
IC (`compileDirectCallOrConstruct` passes `UseDataIC::Yes`, SPEC-jit 5.8 "DirectCallLinkInfo: data IC only"), it is
already produced by the parser and lowered on every run, and the DFG tier runs this very conversion flag on. After
D-M1 the break no longer fires GIL on at all; step 1 is what fixes GIL off, where it still would. The `CallWasm` arm
keeps its own refusal (the WebAssembly section's), `CallFFI` follows the FFI section.
Test: `jit/ftl-direct-call-for-folded-callee.js` - a loop calling a closure allocated in the enclosing function, too
large to inline; counts `callLinkSlow` / polymorphic-stub links and asserts the FTL graph has a `DirectCall`
(`$vm` graph query or the dump in the history entry): +27.7 instructions per call before, the data-IC direct call
after (about +5 over flag off).

*Step 2.* Without concurrent mutators, call linking is `main`'s: `CallLinkInfo::emitFastPathImpl` and the LLInt call
opcodes read the legacy fields (`m_monomorphicCallDestination`, `m_callee`, `m_codeBlock`), `setMonomorphicCallee` /
`setStub` / `setVirtualCall` / `unlinkOrUpgrade` write them in place, no `CallLinkRecord` is allocated or retired,
`DirectCallLinkInfo` uses `UseDataIC::No` in DFG and FTL with `repatchSpeculatively`. Gate: the twenty-odd
`Options::useJSThreads()` tests in `CallLinkInfo.cpp`, the three `UseDataIC` choices, `ThunkGenerators.cpp`'s virtual
and polymorphic thunks, `JITThunks.cpp`, `LowLevelInterpreter64.asm`'s `.opCallThreadedRecord` branch - all move to
the concurrent-mutators predicate. Argument: the three-word read (`destination`, `callee`, `codeBlock`) and the
writer's three stores are mutator-only; `visitWeak` / `unlinkOrUpgrade` from the collector run stopped, as on `main`;
a parked thread's return address after a patched direct call is after the call instruction, which a retarget does not
move. The tenth round's stub republication (history section 58) is GIL-off-only already.
Expected gain: 3-5 instructions per non-inlined call in every tier, 6 in the LLInt; richards 1.02 -> 1.00,
delta-blue 1.01 -> 1.00, add-props-escaped 1.08 -> 1.00; suite about 0.2-0.3 %.
Cost: the widest of these changes (it touches every tier's call path) for the smallest measured gain.
Recommendation: step 1 now; step 2 only after D-M1, D-M2 and D-M4 are in and re-measured.

*Rejected.* Shortening the record fast path (fold the null test into a permanent empty record): saves one or two
instructions and changes the GIL-off protocol's invariants (a null `m_record` is the unlink state).

#### D-M4. `StringImpl`: no locked operation for a string nobody else can reach

**Status: proposed.** Applies to every process with the shared atom table (GIL on and GIL off).

*Rule.* (a) `StringImpl::deref`, shared-table arm: load the count with acquire; if it equals exactly one increment
(no static bit) and the flags word read after it says not-an-atom, destroy without the read-modify-write; otherwise the
existing release `fetch_sub` and zero-transition path. (b) `JSString::create(VM&, Ref<StringImpl>&&)` (and the rope
resolution paths that already use it): if the string has exactly one reference and is not an atom, use
`costOfUnpublished()` (the plain store that already exists for fresh strings in `JSString.cpp`). (c) `setHash` keeps
its `fetch_or`: it is called through borrowed pointers, where a count of one proves nothing.

*Argument for (a).* A reference can be added only by someone who already holds one, or by a table hit
(`tryRefAtom`) on an entry that does not own a reference. Only atoms have such entries (`AtomStringImpl`'s shards; the
symbol registry holds strong references, as `derefSharedZero`'s comment records). If the count is one, the caller owns
that one; no other thread holds a reference, so none can add one or atomize the string in place
(`addOwnedStringToSharedStringTable` is reached with a reference held, so the count would be at least two). A
non-atom at count one therefore cannot be revived between the load and the destruction; the atom case keeps the
read-modify-write that orders it against `tryRefAtom`'s compare-and-swap ("refcount 0 is final"). The acquire load
pairs with the release decrements of the threads that dropped their references earlier, so their accesses
happen-before the destruction: x86-64 a plain load; arm64 `ldar`, replacing an `ldaddl`-class atomic. The flags read
is ordered after the count load by that acquire.

*Argument for (b).* The `Ref<StringImpl>&&` is the caller's own reference; at count one nobody else can reach the
string, and a non-atom has no table entry, so no other writer of the flags word exists until the cell is published.

*Collector.* `costDuringGC` reads, never writes. The sweep's `deref` is the main beneficiary.

*Failure modes.* A revivable non-atom (a new non-owning registry) would turn (a) into a use-after-free. Guard: a debug
assertion in the fast path that the string is neither atom nor symbol-registered, and the rule recorded in
SPEC-vmstate 4.4 next to "refcount 0 is final". An embedder that shares a `String` OBJECT between threads and moves
from it while another thread reads it is racing on the object in any mode.

*Flag off.* The legacy arm is untouched; (b) adds one relaxed load and a compare on a path that already reads the
flags (the branch is taken the same way every time for fresh strings).

*Tests.* `vmstate/string-refcount-fast-paths.js`: two threads (GIL off) and handoffs (GIL on) create, hash, atomize in
place and drop strings that are shared through one cell, through a `Map` key, and through `Symbol.for`; the collector
runs throughout; checks values and, with `--verifyGC`-style options, no crash. Count BEFORE/AFTER: locked loads per
loop iteration of map-set-get (8.0 GIL on now, 4.2 flag off; expected about 5.3 after) - recorded in the history
entry from `perf stat`.
TSan both modes; the amplifier on `cve/` tests that exercise atomization.

*Expected gain.* 72 % of the extra locked operations (deref 48 %, cost 24 %): map-set-get 1.19 -> about 1.05,
regexp-exec 1.13 -> about 1.04, 0.5-1.5 % on Babylon, UniPoker, json-parse-inspector, regexp; the same in GIL off.

*Rejected.* Writing the hash without a locked operation by storing bytes 1-3 of the word: a torn hash for a 32-bit
reader. Computing the hash eagerly in `resolveRope`: a flag-off change to a hot function for a flag-on cost.

#### D-M5. Scope metadata is rewritten as on `main` without concurrent mutators

**Status: proposed.**

*Rule.* `tryCachePutToScopeGlobal` runs `main`'s body, and `tryCacheGetFromScopeGlobal` performs the GlobalProperty ->
GlobalLexicalVar rewrite, unless mutators are concurrent. The x86-64-only ordered publication of the get side
(history section 28) stays as the GIL-off form; without concurrent mutators every target uses `main`'s two plain stores
(so arm64 GIL on gets the cache it never had).

*Argument.* The three metadata words are written by a mutator's slow path under `CodeBlock::m_lock` and read by
mutators' LLInt and Baseline fast paths and, under the lock, by compiler threads. With the GIL the fast-path readers
are the writer or threads ordered after it by a hand-off; the compiler-thread reads are `main`'s. The put fast path's
butterfly write needs no owner predicate in an untagged process (OM G1).

*Tests.* `jit/gil-on-global-property-put-is-cached.js`: counts `llint_slow_path_put_to_scope` /
`operationPutToScope` calls (a counter exists for neither; add `putToScopeSlow` to `JSThreadsCounters`) over 1 M puts
to a global property, an implicit global and a late `let`, on the main thread and, after a hand-off, on a spawned
thread that writes the same global while the first is parked in `join`: 1 M before, a handful after; values checked
across the hand-off. GIL off: unchanged counts.

*Expected gain.* 14x / 27x / 11.6x cliffs to parity; JetStream nothing.

*Risks.* `metadata.m_getPutInfo` is also read by a Baseline compiler thread (the relaxed atomic store in the
function says so); that is `main`'s race and stays as `main` has it.

#### D-M6. Dictionary cases and the inlined prototype form are admitted in an untagged process

**Status: proposed.**

*Rule.* The dictionary refusal in `PropertyInlineCache::addAccessCase`, its mirror in `LLIntSlowPaths.cpp`'s publish
gates, and the `holderBearing` / `outOfLineReplace` refusals in `HandlerPropertyInlineCache`'s inlined-handler
installation are keyed on `useTaggedButterflies()`. Untagged, `m_inlineHolder` and the per-field inline words are used
as on `main` (the packed self word of SPEC-jit 4.2 is a layout, D7, and both forms read the same offsets).

*Argument.* The refusal exists because another thread's in-place dictionary edit can move a slot between a
generated reader's structure check and its load (THREAD.md: dictionary accesses take the structure lock). With one
mutator at a time the check and the load are in one poll-free, call-free window. The two-word inline form
(`structureID`, then `holder`) is unsound only against a writer between the two loads.

*Failure modes.* A dictionary IC surviving into a process that later has concurrent mutators: cannot happen, the
option is process-wide and fixed at start. GIL on with `--useJSThreadsSingleOwnerWithGIL=0` has tagged words and keeps
the refusals.

*Tests.* `jit/gil-on-dictionary-ic-across-handoffs.js`: a 200-property object after `delete`, read, replaced, grown
and flattened by three threads taking turns, in every tier; `icDictionaryStructureRefused` is 0 and
`icGetByIdOptimize` stays bounded (5 M before, tens after).

*Expected gain.* 5.9x / 7.5x per dictionary access to parity; Baseline prototype loads -5 instructions; JetStream
nothing measurable.

#### D-M7. `Atomics` on ordinary objects behind its own option

**Status: proposed; the default is the user's decision.** A new option `useAtomicsOnObjects`, derived in
`notifyOptionsChanged` as "follows `useJSThreads` unless given explicitly"; the seven gates in `AtomicsObject.cpp`
(`useJSThreadsEnabled() && ...isObject()`) read it. SPEC-api 4.5 step 0 becomes "`!useAtomicsOnObjects()` => today's
body". With it off in a flag-on process `Atomics.*` on a non-view throws `main`'s TypeError, and `Lock` / `Condition`
are unaffected (they do not go through `Atomics`). The JSC suite's flag-on lanes pass `--useAtomicsOnObjects=0`, which
removes the 34 results from both flag-on columns; one corpus lane keeps the default. Cost: one option byte, seven
re-keyed tests, a line in SPEC-api.

#### D-M8. A wider ThreadSanitizer pass GIL on (plan only)

Run `JSTests/stress` (every 10th file first, then all) on the TSanJIT build with `--useJSThreads=1`, no thread spawned:
every report is a race between the mutator and a compiler, collector or profiler thread in `main`'s own code, which
AUDIT R10-23 says are now this branch's to triage. Expected classes from the two found so far: LLInt metadata words
read by the Baseline compiler, tier-up counters read by compiler threads. Each gets a relaxed atomic (same
instruction) or a named suppression with its reason. About three hours of machine time; no source change until the
reports are read.

### M: arm64 / non-Linux notes

- D-M1: patching on arm64 needs the instruction-cache maintenance `main` performs in the patcher, and the patched
  instruction must be of a form the architecture lets one core modify while another may execute it (`main` patches
  only such forms: direct branches, the jump-replacement at invalidation points). `main`'s multi-thread embedders
  rely on exactly this plus the lock hand-off; the GIL hand-off is the same lock. The trap signal sender's patching
  GIL on (history section 53) already depends on it, so D-M1 adds no new requirement - but it should be stated once in
  SPEC-jit F5 ("cross-modifying code, GIL on: `main`'s patcher and `main`'s instruction forms; published by the lock
  release, executed after the lock acquire"). To verify when arm64 is built: that no flag-on-only patch site writes an
  instruction outside that class.
- D-M2: on arm64 the fences removed are real instructions (`dmb ishst` per transition, `dmb ishld` x2 per
  `create_this`); the argument is the same as on x86-64 (no reader on another thread), not an ordering argument.
- D-M3 step 2: `CallLinkInfo::emitFastPathImpl`'s arm64-only dependency fold (`xor` of the record into
  `callLinkInfoGPR`) goes away with the record GIL on. The legacy three-word read relies on nothing: no concurrent
  writer.
- D-M4: the fast path's count load must be an acquire (`ldar`), or a relaxed load followed by `dmb ishld` before the
  destruction; x86-64 needs neither. The `fetch_sub` it replaces is `ldaddl` (release) plus an acquire fence on zero.
- D-M5: the get side's ordered publication is x86-64-only today (`#if !CPU(X86_64)` returns early); GIL on it becomes
  `main`'s form on every target. GIL off on arm64 it stays frozen until the LLInt and Baseline readers carry a
  load-load barrier between the structure-ID compare and the operand load (or the pair becomes one 16-byte
  single-copy-atomic load, LSE2 `ldp`).
- M-I4's `setHash` / `cost` `fetch_or` are `ldset`-class atomics on arm64 (LSE) and `ldxr/stxr` loops without it;
  relatively more expensive than on x86-64, so D-M4's share grows there.
- Non-Linux: nothing here depends on the platform's thread or signal primitives.

### M: Decisions for the user

1. **`Atomics` on ordinary objects (D-M7).** (A) As now: part of the flag; the two suite files differ by design in
   both flag-on columns. (B) Own option, default following the flag: programs see no change, the suites can be run
   with it off and then match `main` except for design items. (C) Own option, default off: flag-on suites match by
   default, programs must opt in - breaks THREAD.md's API for every current user. Recommendation: (B). It costs an
   option byte and buys a parity table whose GIL-on behaviour column reads "FFI memory flakes only".
2. **How far to take GIL on toward `main`'s forms.** D-M1, D-M2, D-M4 are the measured JetStream items and D-M5,
   D-M6 remove order-of-magnitude cliffs that the suite does not see; all five rest on G1's argument and on nothing
   new. D-M3 step 2 (call linking) is the same argument with the widest blast radius and the smallest gain.
   Recommendation: do the five, measure, and take D-M3 step 2 only if GIL on is still above 1.005 in instructions.
   With the five done the expected instruction ratio GIL on over flag off is about 1.004-1.006 (what remains: record
   calls 0.2-0.3 %, atomization through the shared table), JetStream about 0.975-0.985 of flag off; the remainder of
   the score gap is mode selection on navier-stokes (0.45 %) and what is left of the locked operations.
3. **`--useJSThreadsSingleOwnerWithGIL=0`.** It is the only way to test the tagged object model under the GIL, which
   three corpus tests do, and after D-M2/D-M6 it is also what keeps the tagged-and-GIL combination of those gates
   honest. It costs nothing at run time. Recommendation: keep it, documented as a test configuration; do not list it
   in user-facing option docs.
4. **navier-stokes.** Spending a build on why GIL on never reaches the fast mode buys at most 0.45 % of the suite and
   an explanation of a bimodality `main` has too. Recommendation: not before the items above; record it as a
   timing-selected mode in PERF-RESULTS.

### M: Doc mismatches

1. SPEC-jit 5.5 "Untagged words": "every tier emits the flag-off form of every butterfly access ... the inline caches'
   transition legs are the flag-off stores with E4-G's fences". The FTL's `compilePutStructure` and
   `compileMultiPutByOffset` (and the DFG's) still emit the store-store fence under `useJSThreads`, and the three
   `create_this` emitters still re-read the pair; neither is listed among "everything the flag turns on that is not
   about the tag". Fix: list both until D-M2 lands; then the sentence is true.
2. SPEC-jit history section 52, "the structure-load dependency on weakly ordered targets is dropped with the tag": the
   store-side fence of the same protocol was not dropped. Add a sentence.
3. LANDING-PLAN Open items "GIL on above flag off" and PERF-RESULTS 6.12: "handler inline caches in FTL code ... Babylon
   -15 %, so a wash as a rule". Babylon is bimodal flag off (5.78-7.01 G) and cannot carry the conclusion; on the tests
   that are stable the FTL handler ICs cost 1.5-9 %. Replace with M-I1's numbers.
4. PERF-RESULTS 6.12 micro table: map-set-get 1.19 and regexp-exec 1.13 GIL on over flag off carry no mechanism; they
   have identical instruction counts and 94 M / 18 M more locked operations (M-I4). `regexp-exec` is not a RegExp cost
   GIL on.
5. LANDING-PLAN Open items lists "put-to-scope metadata frozen after link" among small GIL-on costs; it is a 14x-27x
   per-operation cliff (M-I5), invisible to JetStream. Same for "the refused prototype and dictionary cases" (M-I6:
   5.9x-7.5x on dictionary objects). Both should be stated with their factor.

### M: What was run

All on the existing Release binaries (`main` and the tenth round's final tree); nothing was built.
- `perf stat -e instructions:u,cycles:u` on single JetStream tests: ten tests x two configurations x three runs;
  FlightPlanner x three configurations x six; Babylon x three x five; five tests with the FTL handler-IC option forced
  flag off, two runs; four tests GIL on with and without `--useSharedProfileWriteAvoidance=0`; richards-wasm and
  HashSet-wasm once per configuration.
- `perf stat -e mem_inst_retired.lock_loads:u` on six micro rows and eight JetStream tests, flag off and GIL on;
  `perf record` on the same event for map-set-get.
- `perf record -e instructions:u` with the jitdump option and `perf inject --jit` on Air (four times, once with call
  graphs), WSL, earley-boyer (twice), Babylon, typescript, and on the class-ctor-4, transition-heavy-constructor and
  astar-like-nodes micro rows.
- About twenty single-file microbenchmarks written for this section (calls, constructors, polymorphic get/put sites,
  global-scope accesses, dictionary objects, a closure call), each run once per configuration (`main`, flag off, GIL
  on), some per tier with `--useJIT=0`, `--useDFGJIT=0`, `--useFTLJIT=0`.
- `--dumpFTLDisassembly` graph dumps (node counts only; the build has no disassembler) for four JetStream tests and
  three micro rows; `--dumpAirGraphAtEachPhase` for one function; `--reportCompileTimes` / `--printEachOSRExit` on
  navier-stokes and WSL; `--reportJSThreadsCounters` on 28 JetStream tests in both configurations; `--dumpOptions=2`.
- navier-stokes about forty single runs in total (sub-score distribution, option toggles, synchronous JIT).

## Section N. arm64 and non-Linux reading pass (what is implemented today)

Scope: every protocol of the threads work, as it stands at the head of the branch, that leans on x86-64's
memory model, on an instruction form that exists on one architecture only, or on a Linux facility. Nothing was built or
run on arm64; this is a reading pass over the branch's change against its base (658 files under `Source/`), driven by
a keyword sweep of the added lines (fences, memory orders, `Dependency`, CAS emitters, `CPU(...)`/`OS(...)` gates,
thread-local storage, signals) and followed into the code at every hit that belongs to a cross-thread protocol.

Platform gates that frame everything below (`Options::notifyOptionsChanged`, `Options.cpp`):

- The flag itself (`useJSThreads`) is accepted only where `CPU(ADDRESS64)` and either Linux on x86-64/arm64 or Darwin with
  `ENABLE(FAST_TLS_JIT)`; elsewhere option validation crashes with "useJSThreads is unsupported on this platform
  (SPEC-jit D8)". `ENABLE(FAST_TLS_JIT)` needs `HAVE(FAST_TLS)`, which is defined only when `<pthread/tsd_private.h>`
  exists, a header of Apple's internal SDK. A macOS build from the public SDK, and every Windows build, therefore
  refuses the flag outright, GIL on included.
- GIL off is additionally refused (forced back to GIL on, with a log line) on every non-Linux build and on every build
  that is not a 64-bit JIT build (CLoop, 32-bit), because generated code and the LLInt reach the current thread's lite
  through an ELF initial-exec TLS offset and the Group-3 mode split exists only in `LowLevelInterpreter64.asm`.
- So: GIL off means Linux x86-64 or Linux arm64; GIL on means those plus Apple-internal Darwin builds. Everything in the
  arm64 sections below is about Linux arm64 unless it says otherwise.

### N: Inventory

Status legend: **build** = the tree does not compile for the target; **unsafe** = a crash or a forged read is reachable;
**semantic** = memory-safe, but a program can observe something x86-64 never shows; **perf** = correct, slower than the
x86-64 form; **ok** = correct as written, argument recorded in the notes section.

#### N1. The arm64 build is broken in two places (build; explained)

1. `branchAtomicStrongCAS32` / `branchAtomicStrongCAS64` exist only in `MacroAssemblerX86_64.h`. The branch calls
   them, with no `CPU` guard, from five emitters: `AssemblyHelpers::storeMegamorphicProperty` (the GIL-off claim of a
   megamorphic transition), `InlineCacheCompiler.cpp` (the claimed transition handlers: two sites in the shared
   non-allocating handler, one in the reallocating handler), and `SpeculativeJIT::compileGeneratorClaimResume` /
   `compileGeneratorPublishResume`. `MacroAssemblerARM64.h` has `atomicStrongCAS32(cond, expectedAndResult, newValue,
   address, result)` (load-acquire-exclusive / store-release-exclusive loop, five operands, no jump result) and the
   LSE `atomicStrongCAS32(expectedAndResult, newValue, address)` (`casal`), but no `branchAtomicStrongCAS*`. Found by
   diffing the method names of the two macro-assembler headers against the branch's added lines; WebAssembly's BBQ
   uses the same x86-only names under `#if CPU(X86_64) ... #elif CPU(ARM64)`, which is the pattern the five sites lack.
2. `batomicweakcasi` is listed under `X86_INSTRUCTIONS` only in `offlineasm/instructions.rb` (arm64 has
   `loadlinkacq*` / `storecondrel*`). `LowLevelInterpreter64.asm` uses it, unguarded, in `op_put_by_val`'s GIL-off
   length raise (`.casMaxLengthRetry`). The arm64 backend raises "Unhandled opcode" for it when the LLInt is
   generated. The in-place interpreter shows the intended shape (`if X86_64 ... batomicweakcas ... else loadlinkacq /
   storecondrel loop end`).

Worth: nothing runs on arm64 until both are fixed. Cost of the fix: small (N-D1).

#### N2. A structure check does not order the read of an inline slot that follows it (GIL off; unsafe; explained)

SPEC-objectmodel I9: "a reader seeing a transition's new StructureID sees the new property's value (release-stored
first), modulo M7". M7 and its JIT counterpart (SPEC-jit F7/R7) order the structure-ID load before the *butterfly-word*
load. An inline (in-cell) slot has no butterfly load in between, and "inline (cell) properties never checked/masked"
(SPEC-jit section 5.5) is read by the emitters as "nothing to do". On x86-64 the two loads are ordered by the
hardware. On arm64:

- Writer (all forms: the claimed inline-cache transition, DFG/FTL `PutByOffset` + `PutStructure`, the C++ add): value
  store, store-store fence, structure-ID store. Correct.
- Reader, handler inline caches (`InlineCacheCompiler.cpp` `loadHandlerImpl<ownProperty>` ->
  `CCallHelpers::loadProperty`): `load32 structureID; compare with the handler's; load32 offset from the handler;`
  then for an inline offset `addPtr(imm, object, storageScratch); loadValue [storageScratch + offset*8]`. The offset
  comes from the handler, the base is the object register: nothing in the slot load's address depends on the
  structure-ID load. The R7 dependency (`loadButterflyWithStructureDependency`) is applied on the out-of-line leg only.
- Reader, DFG and FTL: `CheckStructure` is a `branch32` against a constant; `GetByOffset` / `MultiGetByOffset` on inline
  storage is a load at a constant offset from the same base. No dependency, no fence, in either tier.
- Covered by construction, not by rule: the LLInt threaded `get_by_id` forms (`butterflyLoadDependsOnStructureID` folds
  the zero into the *object* register, and `loadPropertyAtVariableOffsetThreaded` uses that register for the inline leg
  too); the Baseline packed self word (the offset register is `(cellID << 32) ^ word`, data-dependent on the
  structure-ID load, and it is part of the slot address); the megamorphic load probe (the cache entry address is hashed
  from the structure ID, the offset is loaded through it).

What a reader can then see: the new structure ID and the slot's previous content. For a slot first used by this
transition that content is the allocation's zero fill, i.e. the empty value, which generated code treats as impossible
(a null cell pointer at the first use). Reachable by exactly the program the multi-slot reader tests exercise (`o.x =
v` on one thread, `o.x` on another through a warm cache for the target structure). Not observable on x86-64; on arm64
it needs the two loads to be reordered, which is permitted and observed for message-passing shapes on current cores.

Worth: a GIL-off crash class on arm64. Design N-D2.

#### N3. The virtual-call pair (arity-check entry, CodeBlock) is validated by three unordered loads (GIL off; unsafe; explained, recorded as IT-8)

`ScriptExecutable::installCode` GIL off: retract the mirror (null), store-store fence, publish the CodeBlock slot,
store-store fence, publish the mirror for that CodeBlock last. Readers - `virtualThunkFor` (JIT, `ThunkGenerators.cpp`),
the bound-function and remote-function thunks (same file), `virtualThunkFor` in `LowLevelInterpreter.asm` - load the
mirror, load the CodeBlock, load the mirror again and compare. SPEC-jit section 5.8 says it: "x86-64 TSO orders the
loads, weakly-ordered targets are the recorded IT-8 residual". On arm64 the three loads are independent (same base
register, different offsets, only control dependencies between them), so the second mirror load can be satisfied
before the CodeBlock load and the comparison proves nothing: a stale entry can be paired with the new CodeBlock, the
failure the recompare was added for (a Baseline prologue profiling arguments against a DFG CodeBlock's null profile
storage). The C++ gate has the same shape (`ScriptExecutable::prepareForExecution`: `hasJITCodeFor` then
`codeBlockFor`, two relaxed loads of different slots; the source comment calls it "ARM64-suspect").

Worth: a GIL-off crash class on arm64, on every virtual call that overlaps a tier-up install. Design N-D3.

#### N4. Remaining structure-to-butterfly (R7) gaps (GIL off; unsafe; explained, recorded)

`loadButterflyWithStructureDependency` (`CCallHelpers.cpp`) closes R7 for every choke-point caller by re-loading the
structure ID into the destination register and folding `eor x, x` into the base - except when the destination
register is the base register (no temporary exists; the plain load is emitted). `INTEGRATE-jit.md` "Known gaps" item
3 records the DFG `Spread` / `ArraySort` / enumerator choke calls, whose guards are indexing-shape checks and which
pass no structure-ID register; the FTL has no gap (`loadTaggedButterflyWithStructureDependency` re-loads inside the
helper). The `ArrayLength` handler re-loads and depends explicitly. There are 47 raw `JSObject::butterflyOffset()`
loads across the Baseline, IC and DFG emitters; the x86-64 inventory classifies them by tag discipline, not by R7.

Worth: a stale, smaller butterfly indexed with a new structure's offset (I24) - out-of-bounds read. Folded into N-D2
(one rule replaces the per-site dependency).

#### N5. A length read does not order the element read that follows it (GIL off; semantic; explained, recorded)

`Butterfly::bumpPublicLengthToAtLeast` raises with a release CAS; generated raises are acquire-release exclusives.
Readers (`tryGetIndexQuicklyConcurrent`, every tier's `GetByVal`) load the public length relaxed and then the element,
with only a control dependency. arm64 can return the raised length and the element's previous content (a hole). The
outcome is memory-safe in every tier: holes are checked wherever they can exist (in-bounds modes exit on a hole,
sane-chain modes return `undefined`, Int32 lanes are verified GIL off, Double holes are PNaN). A program can see
`a.length === n + 1` and then `a[n] === undefined` for an element another thread stored before raising the length -
which TSO never shows. The same holds for the in-cell equivalent in N2 once N2 is fixed for safety only. This is a
memory-model statement, not a defect: decision 1.

#### N6. The heap is fenced for its whole life on every non-x86 target, in both flag-on modes (perf; explained)

`VM::VM` forces `Options::forceFencedBarrier()` and calls `heap.setMutatorShouldBeFenced(true)` when `useJSThreads &&
!isX86()`; `Heap::setMutatorShouldBeFenced` keeps the pair raised for a shared server when `!isX86()` ("weakly-ordered
targets keep it raised for the heap's shared lifetime"). Raised means two things at once: generated code's
`mutatorFence` / `nukeStructureAndStoreButterfly` execute their `dmb ishst`, which N mutators need; and
`m_barrierThreshold` is the tautological threshold, so every write barrier, in C++ and in generated code, takes the
slow path and executes a store-load fence, which only concurrent marking needs. On x86-64 the same forcing measured
1.4-2.7x on barrier-heavy microbenchmarks before the fifth round removed it there. With the GIL on, the hand-off is a
full barrier and neither half is needed outside marking; the comment above the forcing says "GIL-on no longer forces
the fenced barrier", the condition under it still does on arm64.

Worth: unknown until measured on hardware; by the x86-64 number, the largest single flag-on cost arm64 would show.
Design N-D4.

#### N7. Rules that exist for x86-64 only, with a slower portable fallback (perf; explained)

| Rule | x86-64 form | What other targets get | Where |
|---|---|---|---|
| Out-of-line `PutByIdReplace` inlined at the call site | owner test as `xor` from a `%fs:` memory operand, no scratch register | the put goes through the handler chain | `AssemblyHelpers::supportsXorButterflyTIDTagInPlace`, `JITInlineCacheGenerator.cpp` |
| FTL OSR exit reaches its compiled ramp without the generation thunk | exit thunk reads the published ramp pointer and `ret`s through it | every exit of a compiled ramp runs the generation thunk and `operationCompileFTLOSRExit`'s acquire-load fast path | `FTLOSRExitHandle.cpp` (`#if CPU(X86_64)`) |
| Global-property scope caching flag on (`get_from_scope`) | metadata rewritten, readers' two loads ordered by TSO | metadata frozen: the site takes the slow path for good - GIL on as well, where a hand-off already orders everything | `tryCacheGetFromScopeGlobal`, `CommonSlowPathsInlines.h` (`#if !CPU(X86_64)`) |
| Heap fence follows marking | yes | N6 | `Heap.cpp`, `VM.cpp` |
| Load-load order in generated code | free | `loadFence()` = `dmb ish` (known-atom bit, allocation-profile triple, Map/Set version re-check, catch-entry jitData) where a load-acquire would do | `AssemblyHelpers.h`, `JITOpcodes.cpp`, `DFGSpeculativeJIT.cpp`, FTL `LoadFence` |
| LLInt generator-state store-release | nothing | `memfence` = `dmb sy` behind a `gilOffProcess` byte test | `LowLevelInterpreter64.asm` `op_put_internal_field` |

#### N8. Code freshly published to N mutators is executed without a context synchronization on the consuming core (GIL off; architectural; partly explained)

`main`'s contract is that the one mutator executes `WTF::crossModifyingCodeFence()` when it finalizes a plan
(`DFG::JITFinalizer::finalize`, the FTL and Baseline twins, `JITThunks::ctiStubImpl`'s first fetch) before it runs code a
compiler thread wrote. GIL off the finalizing thread does; the other mutators reach the same code through a call-link
record, an installed CodeBlock or a thunk with no `isb` of their own. The architecture asks the consuming core for one.
What the branch does guarantee: every stop bumps a generation and every mutator executes an `isb` before it re-enters
generated code after a stop (`jsThreadsNVSExitInstructionSync`, `jsThreadsSyncToStopGenerationBeforeJITEntry`, the
shared-collection close in `Heap.cpp`), and executable memory is released only by a sweep inside a stop. So no core can
hold instructions of a *previous* occupant of an address it is now sent to; what is left is a core's speculative fetch
of an address before the writer's cache maintenance completed, which the writer's broadcast invalidate covers for the
caches and which a never-taken branch cannot leave in the pipeline. Same practice as `main` with the concurrent JIT;
recorded as a note with an optional strict form (N-D6), not as a required change. "Partly explained": the argument is
the standard one and is not a proof against an implementation that prefetches across an indirect branch target it has
never resolved.

#### N9. Flag off on arm64 is not `main`'s code in a handful of C++ loads (flag off; perf; explained by reading, not measured)

The flag-off rule ("same instructions as `main`") has been checked by instruction sweeps on x86-64, where an acquire
load is a plain `mov`. On arm64 it is `ldar`. Unconditional acquire loads the branch added on paths that run flag off:
`FunctionExecutable::rareDataConcurrently` (behind `ensureRareData`), `ensurePolyProtoWatchpoint` /
`sharedPolyProtoWatchpoint`, the lazy-state bit test in `CodeBlock.h` (`IsLazyStatePreparedForConcurrentCompilation`),
`UnlinkedCodeBlock`'s liveness pointer, `UnlinkedFunctionExecutable`'s deferred-state byte. None is on a per-instruction
path; all are per function or per compilation. The many TSan-only acquire loads (`JSString::m_fiber`,
`JSFunction::m_executableOrRareData`, `Structure::m_previousOrRareData`, the butterfly word, `BlockDirectory` links)
compile to the plain load outside TSan and are fine; `IsoCellSet`'s pointer was deliberately kept relaxed plus a
dependency for this reason. Worth: a flag-off arm64 per-symbol sweep against `main` has to be part of the first arm64
session; the list above is where to look first.

#### N10. macOS and Windows refuse the flag entirely (flag on; behaviour; explained)

See the gates at the top. The reason is the per-thread butterfly tag (a JIT-visible TLS slot). Since the tenth round a
GIL-on process has no tags (`useTaggedButterflies` is false) and its generated code reads no thread-local slot:
`loadButterflyTIDTag` emits `move 0`, `loadVMLite` is GIL-off only, the LLInt's `loadButterflyTIDTagToT6` is reached
only behind `ifTaggedButterfliesBranch`. The D8 crash is still keyed on `useJSThreads`. Design N-D5.

#### N11. ThreadSanitizer on arm64 (tooling; explained)

`dcasHeaderAndButterfly` under TSan issues `lock cmpxchg16b` by hand because the sanitizer's 16-byte atomics are a lock
around plain accesses and are not atomic against the 1-, 4- and 8-byte atomics on the same 16 bytes; the arm is
`CPU(X86_64)` only ("arm64 TSAN builds would need the same; none run today"). An arm64 TSan lane needs the `casp` /
`ldaxp`-`stlxp` twin first or it reports (and suffers) lost lock bits.

### N: Designs

#### N-D1. arm64 compare-and-swap emitters (closes N1)

**Status: implemented, twelfth round (compile-checked for arm64, not run)**: `MacroAssemblerARM64` has
`branchAtomicStrongCAS32/64` with the contract above, in the load-linked/store-conditional form on every arm64 (no LSE variant yet:
`casal` is a later, optional speed-up; a failed compare stores nothing, and the store-conditional's status goes to the register that held
the observed value, so no third scratch register is needed); the LLInt's length-raise loop has the `loadlinkacqi`/`storecondreli` form for
targets other than x86-64. Checked by a syntax-only clang compile for `aarch64-linux-gnu` of the translation units that use the emitters
(the host's headers, a shim for the word size and the x86 `regparm` attribute; no library is linked and nothing runs); `testmasm` cases for
the two emitters need an arm64 machine or emulator. Flag off nothing of it executes.


Rule. `MacroAssemblerARM64` gains `Jump branchAtomicStrongCAS32(StatusCondition, RegisterID expectedAndResult,
RegisterID newValue, Address)` and the 64-bit twin with x86-64's contract: on return `expectedAndResult` holds the
value that was in memory, the jump is taken on `cond`. LSE (`isARM64_LSE()`, already tested at run time by the B3
lowering): `mov tmp, expected; casal tmp, newValue, [addr]; cmp tmp, expected; mov expected, tmp; b.cond`. Without LSE:
the existing `atomicStrongCAS<datasize>` loop (`ldaxr`, compare, `stlxr`, retry on a lost reservation only) followed by
`branchTest32` on its status register. Both are acquire-release, which is what every call site needs (a claim must be
ordered before the value store that follows it; the generator claim is the acquire the LLInt's store-release pairs
with). The LLInt site becomes `if X86_64 batomicweakcasi ... else loadlinkacqi / bineq / storecondreli / bineq
status, 0, retry end`, as `InPlaceInterpreter64.asm` does.
Register discipline. The arm64 forms need the data temp (`x16`) for the observed value and, when the address has a
non-zero offset (the generator's state field), the memory temp (`x17`) for the address; the structure-ID sites have
offset 0 and use the base register directly. The three IC sites run inside shared handlers: check each against
`DisallowMacroScratchRegisterUsage` scopes and against `loadButterflyTIDTag`'s use of the data temp on the
unencodable-offset fallback (`CCallHelpers.h` notes it).
Memory order, x86-64: unchanged (`lock cmpxchg`). arm64: acquire-release on success; on failure nothing is written
(LL/SC) or the same value is written back (`casal` writes nothing on mismatch), so a failed claim "writes nothing", as
SPEC-jit section 5.5 requires.
Collector, stops, watchpoints: no interaction; this is an instruction-selection change.
Flag off: the emitters are reachable only under `useTaggedButterflies` / `gilOffProcess`; nothing changes.
Failure modes. A wrong status polarity makes every claimed transition take the slow path (visible as operation counts
in `jit/` transition tests) or, worse, proceed after a lost claim (the claim tests in `JSTests/threads/objectmodel`
and the amplifier). `testmasm` gains cases for both emitters, LSE and LL/SC, success and failure, observed value.
Verification without hardware: a compile-only arm64 build (the fork's CI builds Linux arm64; no local sysroot exists),
`testmasm` under user-mode emulation for instruction-level behaviour. With hardware: corpus, four modes.
Alternatives rejected: guarding the five sites with `#if CPU(X86_64)` and sending arm64 to the operation - leaves
arm64 without inline transitions GIL off, and the generator claim has no operation fallback in the DFG.

#### N-D2. On weakly ordered targets a guard load is a load-acquire (closes N2 and N4, subsumes R7)

**Status: proposed**

Rule (replaces the wording of SPEC-objectmodel M7 and SPEC-jit F7/R7 for generated code; C++ keeps M7's four forms).
GIL off (`useTaggedButterflies`), on a target that is not x86-64, every load of a cell's structure ID - and of its
indexing-type byte where that byte is the guard - whose result licenses a later read of the same cell (an inline
slot, the butterfly word, the indexing header through it) is a load-acquire. Nothing else about the read changes.
`JSCell::structureIDOffset()` is 0, so the load is `ldar w, [base]` with no address arithmetic (`ldapr` where the
target has RCpc; acquire-PC is enough, only later loads need ordering).
Who emits it.
- LLInt: `loadi JSCell::m_structureID[obj]` in the threaded property and array fast paths becomes `loadacq`-form on
  arm64 (offlineasm has `loadlinkacq`; a plain `ldar` opcode is one backend line). The existing
  `butterflyLoadDependsOnStructureID` macro becomes unnecessary; it can stay until the change is verified, since it is
  correct.
- Baseline, inline caches, thunks: `emitDataICCheckStructure`, `emitPackedInlineAccessCheckThreaded`'s cell-ID load,
  the structure loads in the by-val and enumerator handlers, `emitArrayStorageShapeCheck`'s byte. One helper,
  `loadStructureIDForGuard(base, dest)`, keyed like the choke points; `loadButterflyWithStructureDependency` loses its
  arm64 arm.
- DFG: `CheckStructure` / `CheckArray` / `MultiGetByOffset`'s dispatch loads in `SpeculativeJIT` use the helper when the
  plan is threaded. The checks stay hoistable and CSE-able: an acquire at the check orders every later read in
  program order, however far away, which the per-site dependency could not do for a `GetByOffset` separated from its
  check by CSE.
- FTL: the `JSCell_structureID` / `JSCell_indexingTypeAndMisc` loads that feed a check are emitted with a fence range
  (`MemoryValue::setFenceRange`), which Air lowers to `ldar`; B3 already treats a fenced load as a barrier for later
  loads, so LICM and CSE remain sound without the `B3::Depend` re-load.
- Elided checks (E1/E2 with watched sets): no load, nothing to order; the object's shape is a compile-time fact kept
  true by a stop.
Memory order. Writer: value (or butterfly word) store, `dmb ishst`, structure-ID store - already emitted everywhere
("[storeStoreFence non-x86]"). Reader: `ldar` of the structure ID, then the dependent reads. Release-to-acquire on the
structure ID gives I9 and I24 without M7's "modulo". x86-64: the helper is the plain load; no instruction changes.
Collector: the marker's own seqlock around a butterfly visit is unchanged. Stops, watchpoints, deferred claims: none
involved - this orders two loads of one thread. A second thread mid-way sees what it sees today on x86-64.
Flag off and GIL on: the helper emits `main`'s load (`!useTaggedButterflies`).
Failure modes. A guard load left plain reproduces N2/N4 at that site only, silently. Detection: (1) a lint in the style
of the I14 pass - in a threaded plan on a non-x86 target, every `CheckStructure`-class node's load is fenced, every
handler's structure compare goes through the helper (grep lint for raw `structureIDOffset()` loads next to
`loadProperty`); (2) a litmus pair under `JSTests/threads/objectmodel`: one thread adds the first inline property to
fresh objects handed over through a plain field, another reads it through a warm cache in each tier, counting empty
reads - must be 0, and on arm64 hardware the unfixed build should show non-zero counts within minutes at four to
eight threads; the same pair for an out-of-line add that grows the butterfly (N4).
Expected cost. `ldar` against `ldr` for one load per guarded access: no extra instruction, some lost overlap of
younger loads; removes two ALU instructions and one re-load per choke-point read where R7 is emitted today. Not
measurable without hardware.
Risks. Acquire semantics at a hoisted check hold for the loop's whole life, which is more than needed but never less.
`ldapr` needs a run-time feature test (RCpc); `ldar` is universal.
Alternatives rejected. (a) Extend the `eor`/`add` dependency to the inline leg at every site: cannot express "a
`GetByOffset` whose `CheckStructure` was hoisted", needs a temporary the `dest == base` sites do not have, and leaves
the rule as a per-site inventory. (b) `dmb ishld` after each check: strictly more expensive than `ldar`. (c) Leave it
and rely on the hole/empty checks: there are none for an in-cell slot a structure check vouches for.

#### N-D3. The virtual-call pair on weakly ordered targets (closes N3)

**Status: proposed**

Rule. The reader's three loads are ordered by making the first two acquires: `ldar mirror; ldar codeBlock; ldr mirror;
cmp`. An acquire orders every later load after it, so CodeBlock is read after the first mirror value and the second
mirror value after CodeBlock; with the writer's order (null, fence, CodeBlock, fence, entry) equal non-null mirror
reads then bracket the CodeBlock read exactly as SPEC-jit section 5.8 argues for x86-64. Both fields sit at non-zero
offsets of the executable, so the address is materialized first (`add tmp, executable, #offset; ldar`). Sites: the
three JIT thunks in `ThunkGenerators.cpp` that re-compare today, `virtualThunkFor` in `LowLevelInterpreter.asm`
(arm64 arm), and in C++ `ScriptExecutable::prepareForExecution`'s gate (`hasJITCodeFor` becomes an acquire load when
`gilOffWithProcessGate()`; acquire is free on x86-64, and the gate keeps flag off and GIL on at the plain load on
arm64). Only emitted GIL off on non-x86 targets; x86-64 thunk bytes unchanged.
Failure mode: a stale entry with a new CodeBlock; the test is the one that found it on x86-64 before the recompare
(`--useFTLJIT=0`, virtual calls racing tier-ups across threads), run on hardware; the lint is "every reader of
`offsetOfJITCodeWithArityCheckFor` in a threaded thunk uses the ordered form".
Alternative rejected: an address dependency chain (`eor` from the first mirror value into the CodeBlock load's base,
and from CodeBlock into the second mirror load's base) - works, costs four ALU instructions and is harder to read than
two `ldar`s on a path that is already a slow-ish call.

#### N-D4. Separate "publication fences are needed" from "every write barrier is slow" (closes N6)

**Status: proposed**

Rule. Two independent conditions replace the one flag on non-x86 targets.
(1) *Publication*: generated `mutatorFence` and the nuke/publish sequences execute their store-store fences whenever
`m_mutatorShouldBeFenced` is set. It stays set for the heap's life only in a GIL-off process (N mutators can load a
freshly published cell at any time). With the GIL on it follows marking, as on `main`: a hand-off is a full barrier on
both sides, which is the argument SPEC-objectmodel M8 already makes and the VM constructor's comment already states;
the condition `useJSThreads && !isX86()` becomes `gilOffProcess && !isX86()` in both places in `VM::VM`, and
`Heap::setMutatorShouldBeFenced`'s `keepRaised = !isX86()` becomes `!isX86() && VM::isGILOffProcess()`.
(2) *Barrier threshold*: `m_barrierThreshold` is the tautological threshold only while a collection that marks
concurrently with mutators is in progress, on every target. The two words are already separate
(`addressOfBarrierThreshold`, `addressOfMutatorShouldBeFenced`; generated code reads each where it needs it); only
`setMutatorShouldBeFenced` writes them together. It gains a second argument or a sibling that sets the threshold from
the marking state alone. `Heap::writeBarrierSlowPath` keeps its `mutatorShouldBeFenced()` test, which now costs a
store-load fence only on the remembered-set path (a black source).
Interaction with concurrent shared marking (`useConcurrentSharedGCMarking`, off by default): that stage keeps both
raised GIL off until the per-client flag reroute exists; unchanged.
Memory order. Nothing weaker than today for GIL off publication. The threshold's raise at `beginMarking` happens in
the stop that starts the cycle; its lower at `endMarking` likewise; mutators re-read it per barrier.
Flag off: `forceFencedBarrier` is not forced; identical to `main`.
Failure modes: a lowered publication fence GIL off on arm64 is a foreign reader seeing a cell's header before its
fields - caught by the allocation-publication litmus (one thread allocates and hands over through a plain field,
another reads every field; count of zero/garbage fields) and by TSan on arm64. A threshold lowered during marking is
a lost remembered-set entry - `--verifyGC` lanes.
Expected gain: by the x86-64 measurement, 1.4-2.7x on barrier-heavy microbenchmarks GIL off, all of it GIL on.
Verification: hardware only.

#### N-D5. GIL on without a JIT-visible thread-local slot (addresses N10)

**Status: proposed**

Rule. The D8 refusal is keyed on what needs the slot: `useTaggedButterflies` (GIL off, or GIL on with
`--useJSThreadsSingleOwnerWithGIL=0`). A GIL-on single-owner process is admitted on any `CPU(ADDRESS64)` JIT platform.
`initializeButterflyTIDTagForCurrentThread` keeps maintaining the C++ `thread_local`; the pthread-key mirror is
created only where `ENABLE(FAST_TLS_JIT)`. On platforms without the slot `--useJSThreadsSingleOwnerWithGIL=0` is
refused with a message instead of reaching the `RELEASE_ASSERT_NOT_REACHED` in `loadButterflyTIDTag`.
What else macOS and Windows need, from reading: thread start/park go through `WTF::Thread` and `ParkingLot` (portable;
the branch's only platform edits are `Thread::tryCreate` and a Windows handle fix); traps GIL on are delivered as flag
off, which on Darwin is the Mach-exception `SignalSender` and on Windows is polling (no signal-based traps there, as on
`main`); the sampling profiler suspends through `Thread::suspend` (Mach, `SuspendThread`); `jsThreadsYieldToScheduler`
and the amplifier fall back to `Thread::yield()`; the stop watchdog's stack dump is `HAVE(MACHINE_CONTEXT) ||
OS(WINDOWS)`; Windows pre-commits stack on the per-thread soft-limit publish (`VM::updateStackLimits`). No raw `futex`,
`membarrier`, affinity or `/proc` use exists in the branch (swept). GIL off stays Linux-only until generated code has
a lite slot elsewhere: Darwin could use the same dynamic pthread key as the tag (`loadFromTLS64(fastTLSOffsetForKey)`,
internal SDK only); Windows would read the TEB's TLS array (`gs:[0x58]`, index, offset) - two dependent loads more per
lite access; neither is designed here.
Verification: the GIL-on corpus on macOS arm64 and Windows x64 CI; nothing can be said before it has run once.

#### N-D6. (optional, strict form of N8) Process-wide core synchronization when code is published to N mutators

**Status: proposed, not recommended now**

`membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)` after `LinkBuffer` finalization in a GIL-off process on Linux
arm64 makes every thread of the process pass a context synchronization before it can run the new code. Cost: an
inter-processor interrupt to each running thread per link, tens of microseconds; acceptable per DFG/FTL plan, not per
inline-cache stub. It is the only form that satisfies the architecture's letter. Rejected for now for the reason in
N8; listed so that an unexplained arm64-only illegal-instruction or stale-code report has a first thing to try.

### N: arm64 / non-Linux notes

The complete table. "ok" rows carry the argument; "change" rows name the design.

| # | Protocol | Where (tier) | Leans on | arm64 |
|---|---|---|---|---|
| 1 | Handler chain publish and read (F1/F2) | `PropertyInlineCache.cpp` prepend; handlers read through `handlerGPR` (Baseline, DFG, FTL) | fence-published pointer, reader address-dependent | ok: `storeStoreFence` is `dmb ishst`; every field is loaded through the head pointer |
| 2 | Packed self word, LLInt one-word caches (F3) | `PropertyInlineCache.h`, `GetByIdMetadata.h`, LLInt | one aligned 64-bit load/store | ok: single-copy atomic; `CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)` guards present |
| 3 | Call-link record (F6) | `CallLinkInfo.cpp` data-IC fast path (all tiers), LLInt `.opCallThreadedRecord` | fence-published pointer, all reads through it | ok: comparand, CodeBlock and target are loaded through the record register |
| 4 | Always-call targets re-load `m_stub` through the CallLinkInfo | same; virtual and polymorphic thunks | store order (`m_stub` before record) | ok: arm64 folds `eor r, r` into the CallLinkInfo pointer (`#if CPU(ARM64)` in `CallLinkInfo.cpp`, `callLinkInfoDependsOnRecord` in the LLInt) |
| 5 | Structure ID to butterfly word (F7/R7, OM M7) | choke points in every tier; C++ `Dependency` / `loadLoadFence` | TSO load-load | ok where routed (LLInt macro, `loadButterflyWithStructureDependency`, DFG `emitButterflyLoadWithStructureDependency`, FTL `B3::Depend`, `ArrayLength` handler); gaps N4 -> N-D2 |
| 6 | Structure ID to inline slot (OM I9) | handler ICs, DFG/FTL `GetByOffset` | TSO load-load | change: N2 -> N-D2 |
| 7 | Transition publish: value, fence, structure ID; nuke, fence, word, fence, structure | IC handlers (`storeFence`), DFG `PutStructure`/`NukeStructureAndSetButterfly`, FTL fence, C++ `nukeStructureAndSetButterflyConcurrent` | TSO store-store | ok: fences emitted ("[storeStoreFence non-x86]"); the `isX86() ||` guards in `JSObjectInlines.h` are `main`'s |
| 8 | Transition claim (`lock cmpxchg` on the structure ID) | megamorphic store probe, transition handlers | x86 instruction | change: N1 -> N-D1 |
| 9 | Length raise (CAS-max) | LLInt `op_put_by_val`, Baseline IC, DFG `branchAtomicWeakCAS32`, FTL `atomicStrongCAS`, C++ release CAS | locked RMW as fence | LLInt: change (N1); others ok (`ldaxr`/`stlxr`, or `casal`) |
| 10 | Length then element (I21 reader) | every tier's `GetByVal`, `tryGetIndexQuicklyConcurrent` | TSO load-load | semantic: N5, decision 1 |
| 11 | vectorLength of a published flat butterfly never changes in place | `ensureLengthSlowConcurrent` | - | ok by construction: the former in-place raise was removed because a reader has only a control dependency from the bound to the slot; see decision 3 |
| 12 | Segmented spine chain (word, spine, fragment, slot) (OM M1) | C++ readers, slow paths | address dependency | ok: `Dependency` plus explicit `loadLoadFence` in the visit (one `dmb ishld` per segmented visit) |
| 13 | Header plus butterfly word double-width CAS (OM M3) | `dcasHeaderAndButterfly` | `cmpxchg16b` | ok: clang inlines `ldaxp`/`stlxp` or calls the outlined-atomics helper (`casp` when LSE), both lock-free and coherent with the 1/4/8-byte atomics on the same 16 bytes; `-mcx16` is x86-only in CMake and arm64 needs no flag; `__atomic_always_lock_free(16)` is true for clang on AArch64, so the constructor's assertion passes. TSan arm: N11 |
| 14 | Virtual-call arity mirror recompare (IT-8) | three JIT thunks, LLInt `virtualThunkFor`, `prepareForExecution` | TSO load-load | change: N3 -> N-D3 |
| 15 | Copy-on-write mode then word | `trySetIndexQuicklyConcurrent` | load-load | ok: `loadLoadFence` between them |
| 16 | Out-of-line location for a caller-supplied offset (M7(d)) | `locationForOutOfLineOffsetConcurrent` | load-load | ok: `loadLoadFence` before the word load |
| 17 | Map/Set seqlock | `JSOrderedHashTableHelper.h`; FTL probe | - | ok: writer odd / fence / writes / fence / even-release; reader acquire, reads, `loadLoadFence`, re-load; FTL emits the acquire and Air `LoadFence` |
| 18 | Known-atom bit then impl; allocation-profile {structure, allocator, structure}; catch entry's jitData | `AssemblyHelpers::branchIfNotAtomStringImpl`, `JITOpcodes.cpp` | TSO load-load | ok: explicit `loadFence()` (`dmb ish`); perf note N7 |
| 19 | Generator resume claim / unclaim | DFG/FTL CAS, LLInt `op_put_internal_field`, `BytecodeGeneratorification` | TSO store-store for frame saves | DFG: change (N1); LLInt ok (`memfence` behind the process byte); FTL ok |
| 20 | FTL lazy slow path stub pointer | `FTLThunks.cpp` | TSO acquire | ok: arm64 arm uses `ldar` and an `isb` before the jump |
| 21 | FTL OSR exit ramp pointer | `FTLOSRExitHandle.cpp` | x86 form | perf: arm64 uses the C++ acquire path (N7); an arm64 thunk would need `ldar` + `isb` like row 20 |
| 22 | Watchpoint state read by generated code (OM M6) | all tiers | changes only in a stop | ok: every mutator passes the stop's barrier and an `isb` on resume |
| 23 | Patching under a stop (F5, R1.d) | `JumpReplacement::fire`, jettison, the GIL-on `SignalSender` | cross-modifying code | ok: patcher-side `crossModifyingCodeFence` on every conductor exit (`VMManager.cpp`, `JSThreadsSafepoint.cpp`, the shared-collection close), unconditional `isb` on every park exit, generation compare in `acquireHeapAccess` for threads that waited without parking. GIL on, a thread woken from a blocking call has passed an exception return |
| 24 | Freshly published code on other mutators | records, `installCode`, thunks | - | N8 |
| 25 | Stop word, heap access word, GC stop pending, parked root snapshot | `VMManager.cpp`, `Heap.h` | store-load order | ok: `seq_cst` accessors only (linted for the stop word); `m_worldState` is `main`'s seq_cst CAS |
| 26 | Lite registry, thread manager completion, lock/condition objects | `VMLite.cpp`, `ThreadManager.cpp`, `LockObject.cpp`, `ConditionObject.cpp` | - | ok: explicit acquire/release pairs and `WTF::Lock`; no relaxed publication found |
| 27 | `Atomics.*` on ordinary properties | `ThreadAtomics.cpp`, `atomicSlot*` in `ConcurrentButterfly.cpp` | - | ok: the accessor's load, exchange and compare-exchange are `seq_cst` |
| 28 | Structure construction, transition-table single slot, rare data, special-property cache, RegExp bytecode, atom table | `Structure.cpp`, `StructureInlines.h`, `StructureRareData.cpp`, `RegExpInlines.h` | - | ok: release store or `storeStoreFence` on the writer, acquire / dependency / `loadLoadFence` on the reader, each gated so flag off keeps the plain store (no `dmb`) |
| 29 | Reference counts shared across threads | `StringImpl.h`, `DeferrableRefCounted.h`, `InlineCacheHandler`, `JITStubRoutine` | - | ok: relaxed increment, release decrement, acquire fence on zero |
| 30 | Heap fence forced on non-x86 | `VM.cpp`, `Heap.cpp` | x86 exemption | perf: N6 -> N-D4 |
| 31 | Global-property scope cache frozen on non-x86 | `CommonSlowPathsInlines.h` | x86 exemption | perf: N7; GIL on it can be keyed on `useTaggedButterflies` with no ordering argument needed |
| 32 | Per-thread tag and lite in TLS | `loadFromELFTLS64` (arm64: `mrs tpidr_el0`, `add`/`sub` #hi12, `ldr` #lo12, offset asserted within 16 MB and 8-aligned), LLInt `:gottprel:` with `x16` | ELF initial-exec | ok on glibc and musl; Darwin: pthread key, internal SDK only; Windows: none (N10) |
| 33 | TLS memory-operand `xor` | `xorFromELFTLS64` | x86 instruction | perf: guarded by `supportsXorButterflyTIDTagInPlace()`; arm64 uses the chain (N7) |
| 34 | Scratch registers for per-lite stores | `AssemblyHelpers.h` (`prepareCallOperation`, `emitPublishTopCallFrameForHostCall`, soft stack limit, exception slot), DFG `DoesGC` | - | ok: arm64 uses the memory temp through the cache-invalidating accessor; other CPUs reach `RELEASE_ASSERT_NOT_REACHED` behind the option refusal |
| 35 | Spin hint | `WTF::spinLoopPause` | - | ok: `isb` on arm64 |
| 36 | Scheduler yield | `jsThreadsYieldToScheduler`, `RaceAmplifier` | `sched_yield` | ok: `Thread::yield()` elsewhere |
| 37 | Conservative scan of parked threads, stop watchdog dump | `MachineStackMarker.cpp`, `JSThreadsSafepoint.cpp` | signals (`SIGUSR2` suspend) | ok on POSIX; Mach / `SuspendThread` paths are `main`'s; GIL off is Linux-only anyway |

Emitters with no arm64 implementation at all: `branchAtomicStrongCAS32/64` (N1, unguarded, a compile error),
`batomicweakcasi` (N1, an offlineasm error), `xorFromELFTLS64` (guarded, fallback exists), the FTL exit thunk's
published-ramp read (guarded, fallback exists). Everything else the branch emits goes through generic macro-assembler
operations that `MacroAssemblerARM64` implements (checked by diffing the two headers' method sets against the branch's
added lines: no other x86-only name appears).

How to test. Without hardware: (1) a compile-only arm64 build - the fork's CI already builds Linux arm64 for every
push, so a draft push is the cheapest compiler; no aarch64 sysroot exists on the development machine; (2) `testmasm`
and the offlineasm output under user-mode emulation for N-D1 (emulation says nothing about ordering: it executes in
program order); (3) the five protocols of N2, N3, N5, row 4 and row 17 as litmus tests against the architecture's
published axiomatic model (herd-style), which is how the "ok" rows above were argued and how a change should be
checked before hardware exists. With hardware (any multi-core Linux arm64 machine; more cores and a non-Apple core
reorder more): the corpus in four modes; the amplifier on `cve/mc-val-*`, `shared-objects/multislot-readers-*`,
`objectmodel/*transition*`, `jit/polymorphic-call-stub-republished-*`, `jit/unsafe-transitions-*`; the two new litmus
tests of N-D2 and the virtual-call test of N-D3 with eight or more threads pinned to distinct cores; TSan on arm64
once N11's arm exists; a flag-off per-symbol instruction sweep against `main` (N9).

### N: Decisions for the user

1. **What a plain read promises across locations.** x86-64 gives JavaScript programs TSO for free: a thread that reads
   `a.length` and then `a[i]`, or a structure-dependent property after another, never sees the second older than the
   first. Option A, *portable model*: the specification says racy plain accesses to different locations are unordered
   (what N5 already is on arm64, what Java says for non-volatile fields); engine safety is kept by N-D2 and N-D3 only.
   Cost: nothing on x86-64, one `ldar` per guard on arm64; a program that relies on TSO without `Atomics` or a lock
   behaves differently on arm64. Option B, *TSO everywhere*: every plain heap read in generated code is an acquire on
   arm64 (or a `dmb ishld` per access); cost: every property and element load, probably double-digit percent GIL off
   on arm64. Recommendation: A, stated in SPEC-api next to the staleness model, with the visibility rule decision of the
   JIT section (they are the same kind of promise).
2. **x86-64-only fast forms versus portable ones** (N7's table). Each buys a measured amount on x86-64 and is
   invisible elsewhere; arm64 falls back to a correct slower path. Option A: keep them, and port by measured need once
   hardware exists (the out-of-line Replace needs a spare register in the Baseline put register file; the OSR exit thunk
   needs `ldar` + `isb`; the scope cache needs only a re-key GIL on). Option B: forbid architecture-specific rules.
   Recommendation: A, with one constraint: a rule that is x86-64-only must have its fallback exercised by a test on
   x86-64 too (an option that disables the fast form), or the fallback rots; today only the handler-chain fallback of
   the Replace form is covered that way.
3. **In-place vectorLength raise GIL off** (LANDING-PLAN decision (b), stanford-crypto-aes). As an x86-64-only rule it
   is sound there (the reader's bound load and slot load are ordered). A portable form exists and costs the readers
   nothing: allocate the owner's flat butterfly with slack, clear the whole capacity before the butterfly word is
   published, and raise vectorLength up to the capacity with a plain store; a foreign reader that sees the new bound
   reads slots whose hole fill was ordered by the word's publication (address dependency through the word), so no
   reader-side acquire is needed on any target. It spends memory (the slack is cleared and scanned) instead of
   ordering. Recommendation: design (b) in the portable form, so that it does not add a row to N7's table.
4. **Whether arm64 GIL off is a landing requirement.** The exit criteria name "one arm64 platform". From this pass:
   two build breaks (small), two unsafe orderings with designs (N-D2, N-D3: moderate, every tier), one heap change with
   a likely large performance effect (N-D4), then a first hardware campaign of unknown length. GIL on arm64 needs only
   N-D1's compile fixes plus N-D4's re-key to be both correct and close to x86-64's relative cost. Options: require both
   modes on arm64 before landing; require GIL on only, with GIL off refused on arm64 at option validation until N-D2
   and N-D3 have run on hardware. Recommendation: the second - the refusal is one line, makes the unsafe rows
   unreachable, and mirrors how non-Linux is handled today.
5. **GIL on for macOS and Windows** (N-D5): admit it now that a GIL-on process needs no thread-local slot, or keep the
   flag Linux-only until someone can run the corpus there. Recommendation: make the change together with the first CI
   run on those platforms, not before; an admitted but never-run configuration is worse than a refused one.

### N: Doc mismatches

- `runtime/VM.cpp`, comment above the `forceFencedBarrier` forcing: "GIL-on no longer forces the fenced barrier ...
  GIL-off keeps the forcing". The condition is `useJSThreads && !isX86()`: on x86-64 neither mode forces it, on other
  targets both do. SPEC-objectmodel M8's last sentence ("GIL-off keeps the heap-lifetime forcing") is stale the same
  way: `Heap::setMutatorShouldBeFenced` keeps it raised for `!isX86()`, or GIL off with concurrent shared marking, and
  otherwise lets it follow marking. Fix: state the rule as the code has it, or adopt N-D4 and state that.
- SPEC-objectmodel I9 ("... modulo M7") and M7 / SPEC-jit F7, R7, section 5.5 "Inline (cell) properties never
  checked/masked": together they leave the structure-ID-to-inline-slot order unstated; on x86-64 it holds by hardware,
  on arm64 nothing provides it outside the LLInt, the Baseline packed word and the megamorphic probe (N2). Fix: M7
  should say "any read the structure ID licenses", whichever design is taken.
- SPEC-jit section 5.5, "LLInt+Baseline/stubs ... `CCallHelpers::loadButterflyForRead/ForWrite`" and
  `INTEGRATE-jit.md` "R7/F7 ARM64 dependency gaps - CLOSED at the choke point": closed except `dest == base` (plain
  load emitted) and the DFG sites of "Known gaps" item 3; the word "closed" should carry those two exceptions where it
  is used.
- SPEC-jit-annex App. R5 and `ConcurrentButterflyOperations.h` describe Darwin as supported through a pthread key. It
  is, only under `ENABLE(FAST_TLS_JIT)`, which public-SDK builds do not have; there option validation crashes.
  LANDING-PLAN "arm64 and non-Linux" says macOS and Windows "need the thread start/park and stop-the-world primitives
  checked"; what they need first is for the flag to be accepted (N10).
- LANDING-PLAN "arm64 and non-Linux": "arm64 needs a build, the corpus and TSAN there" - the build does not compile
  (N1), and the TSan lane needs N11's arm. The list of places "that rely on address dependencies instead of fences (F2,
  the call-record fast path, butterfly word reads, and this round's unclaimed allocating transition)" are the rows that
  are *fine*; the rows that are not are N2, N3 and N4.
- `CommonSlowPathsInlines.h` `tryCacheGetFromScopeGlobal`: the non-x86 refusal is keyed on `useJSThreads`; the tenth
  round's rule (a GIL-on process runs `main`'s forms) is not applied to it on arm64. SPEC-jit history section 28
  describes the x86-64 form only.

### N: What was run

Nothing of cost. `git diff` of the branch against its base saved to a scratch file and swept with `grep`/Python;
source files read at the head of the branch. No `jsc` was executed, so the shell word-splitting pitfall for the
GIL-off environment variables does not touch this report. No arm64 compiler, sysroot or emulator exists on the machine;
the two build breaks were established by reading the macro-assembler headers and `offlineasm/instructions.rb`, not by
compiling.
