# Parity plan: GIL off at 0.90 of `main`, as safe as `main`

Written at the end of the eleventh session (2026-09-17), after DESIGN-PROPOSALS.md. That document says what is wrong
and how each thing would be fixed. This one answers a narrower question: **which set of changes, if all of them land
and pass their acceptance tests, makes a GIL-off process run at 0.90 of `main` and be as safe as `main`, and how sure is
that.** It rests on measurements made for it with a throw-away experiment build (Part 2); where it rests on an estimate
it says so and gives a range.

Addendum, same date: the project owner took this document's conclusion and set the order of goals. First: the branch
built with the flag not set equals `main` in safety and speed (gate A of section 6.5), so that the work can be merged and
shipped as an experimental feature that is off by default. That landing has its own document, `FLAG-OFF-LANDING.md`, which
also corrects two things said here about flag off (the identity script; two per-test figures). The gates G-U, G-T and
G-S below are what comes after it and are reported as measured, not as conditions of the first merge.

Nothing here is implemented. The SPEC files stay normative for what is; DESIGN-PROPOSALS stays the place where each
design is argued; this file is the order of work, the budget, the gates and the rules for a miss.

## Part 0. The plan on one page

**The honest answer first.** No set of changes known today guarantees 0.90. The part of the plan that was prototyped and
measured takes GIL off from 0.758 of `main` to 0.804. Every prototype and every ceiling together (no polls in optimized
code, untagged words) reach 0.832. The rest of the plan is estimated; its central estimate is **0.87 with threads alive
and 0.90 for a process that never spawns**, in a range of 0.835 to 0.925, and the largest term of that estimate has
families
but no designs yet. What follows is the best plan the measurements support, with its uncertainty where it belongs.

**What the measurements changed.** Time is main-thread cycles; the rounds and DESIGN-PROPOSALS budgeted in whole-process
instruction counts, which include compiler and collector threads and weigh a locked instruction at one. In time:
- polls and tags, a fifth of the distance in instructions, are three points once inlined callees stop polling (D-B1);
  the designs aimed at them (entry poll as the stack check, strip-mined polls, one predicate per window) buy under one;
- the Double family with its lane checks is 2.9 points and exists today only before the first spawn: C-D3 is no longer
  optional;
- **eleven points are in none of the switches**: locked instructions (a GIL-off main thread retires twice `main`'s; 70 %
  of
  delta-blue's are in generated code, claim compare-and-swaps on objects nobody else can see), property-table walks under
  the structure's lock, atomic string reference counts, cell-locked WeakMaps and async generators, the RegExp context's
  constructor, allocation through C++;
- **five points are paid by both flag-on modes** (GIL on: 1.050 of `main`'s cycles, 0.947 in score) and count in full.

**What the plan is.** In order: instruments, including a census of generated code by node kind (P0); defects, now
 including
the exit-feedback defect that makes stanford-crypto-pbkdf2 five times slower on a loaded machine and the property-table
rebuild race behind the lost add and, by inference, the persistent wrong value (P1); the measured designs: polls D-B1 to
D-B4, one-pass append, RegExp entry, MakeAtomString, the lone-conductor rule (P2 to P4); C-D3 (P3); the long tail, family
by family, with four new designs for its largest members: no claim on an unescaped object, owner-first cell locks,
unshared strings' reference counts, lock-free structure tables (P4.6, P5); the flag-on tax (P6); a single-mutator phase
for optimized code only, for processes that never spawn (P7); scaling (P8); the safety campaign and the constant (P9).

**Two gates, not one.** JetStream never spawns a thread, so "GIL off on JetStream" measures a process that has the
configuration on and has not used it (G-U). The plan adds the same suite with one spawned thread alive (G-T). Today G-T
is already 1.5 points below G-U with identical instructions: the doubled exit thresholds and the stop's second client
cost Worst Case scores.

| gate | today | measured part of the plan (rows A to D) | whole plan: pessimistic / central / optimistic |
|---|---|---|---|
| G-T, threads alive | 0.743 | 0.776 (the Double family switches itself off at the spawn; 0.80 with C-D3) | 0.835 / 0.87 / 0.915 |
| G-U, never spawned | 0.758 | 0.804 | 0.865 / 0.90 / 0.925 |

**Safety** (Part 6). "As safe as `main`" is six checkable properties and one constant in the code that flips in the
commit citing the evidence. Today one memory-safety item is unexplained (the FTL's unreachable trap), one has a
confirmed mechanism and no fix yet (the property-table rebuild race), four are designed and unimplemented, and the
existing evidence has three blind spots that the program closes before any hour counts: ThreadSanitizer cannot see
generated code, no amplifier site exists in the object model or the code life cycle, no fuzzer has run since June or ever
at scale GIL off. Every performance package names the safety work that precedes it.

**If a gate is missed** (Part 7): the rules are fixed now. The last of them is to state the measured per-thread cost
instead of the gate; this document says today that that is a possible outcome.

## Part 1. What is claimed, and how it is checked

### 1.1 The three performance gates

The sentence "GIL off reaches 0.90 of `main`" has been measured, in every round, as: the 36 JetStream tests of
PERF-RESULTS run as one suite in the `jsc` shell with the three GIL-off settings, per-test scores compared with the
rebase base's
binary by geometric mean in a quiet interleaved pass (medians of five). JetStream never spawns a Thread. That
measurement therefore says what a program pays for turning the configuration on before it uses it; it says nothing about
a program whose threads are alive. Both matter, and an optimization that exists only before the first spawn moves one
and not the other. The plan keeps them apart:

| gate | what runs | bar | what it stands for |
|---|---|---|---|
| **G-U** (unspawned) | the 36 tests, GIL off, as in every round | score >= 0.90 of `main` | every existing program, the day the embedder turns the configuration on |
| **G-T** (threads alive) | the same 36 tests, GIL off, after a preamble that spawns one Thread which parks in `Atomics.wait` for the whole run | score >= 0.90 of `main` | per-thread speed of a program that uses threads; nothing keyed on "never spawned" counts |
| **G-S** (scaling) | the scaling suite (`JSTests/threads/scaling`) at 1, 2, 4, 8 threads with private copies of the workload per thread | T(4) >= 3.2 x T(1) on the non-allocating rows, >= 2.4 x on the allocating rows; peak resident set within the per-test bound | that the per-thread number of G-T multiplies |

Between quiet passes the working measure is the main thread's cycles (`perf stat --no-inherit`, section 2.1): the score
is 0.98 divided by the main-thread cycle ratio in every configuration measured, so 0.90 is 1.09. Whole-process instruction
counts, the rounds' working measure, are kept for explaining per-test movements only. A gate is only ever passed by a
quiet
score pass.

G-T's preamble, in the shell: `globalThis.w = { k: 0 }; new Thread(() => { Atomics.wait(w, "k", 0); });`. On the tenth
round's tree G-T and G-U execute the same main-thread instructions (Part 2): nothing is keyed on the first spawn yet.
Their
scores differ already (0.743 against 0.758): with a spawned thread alive the exit thresholds double and a stop has a
second client.

### 1.2 "As safe as `main`"

Six properties and one gate in the code, defined with their checks in Part 6 (S1 memory safety; S2 no new
script-reachable abort class; S3 no new hang or unbounded-resource class; S4 single-threaded behaviour equal to `main`;
S5 a stated model of what a racy program may observe; S6 the embedder contract; S7 the constant
`gilRemovalPreconditionsMetValue` flips in the commit that cites the evidence for S1-S6). The claim is relative: `main`
has bugs, accepted races and assertions that fire on engine bugs; the threads work adds no new class of any of them.

### 1.3 What the plan claims

If every work package of Part 5 lands with its acceptance test, and the campaign of Part 6 reaches its exit criteria
on the resulting tree, then:

1. G-U lands at 0.90 in the central estimate (0.865 to 0.925), of which the U track is two points.
2. G-T lands at 0.87 in the central estimate (0.835 to 0.915). It passes only if the long tail (row R) comes in at two
   thirds of its ceiling; at one half it misses by two to three points. Part 7 says what is done then.
3. G-S passes on the rows that do not allocate; the allocating rows depend on D-J1/D-J3 and are the least certain item
   of the plan.
4. S1 to S7 hold in the sense of Part 6, on x86-64 Linux. arm64 stays refused GIL off until it has run on hardware.

This is not a guarantee of 0.90 and the document does not pretend to one. It is the set of changes whose measured part
is worth five points, whose estimated part is worth seven to ten more, and whose order puts the measurements that would
falsify the estimate (P0.7's census, the checkpoint after P4) before the work that depends on it.

What is measured and what is estimated is marked on every line of the ledger. Nothing in this document is implemented:
the prototypes behind the measurements were run-time switches in a throw-away build and are not in the tree.

## Part 2. Where GIL off stands, measured for this plan

### 2.1 What is measured, and a correction of method

Three kinds of numbers appear below, and they are not interchangeable.

- **Scores**: full JetStream in one process, the quiet machine, configurations interleaved, medians of three rounds
  (2.2). This is the gate's own measure.
- **Main-thread counters**: one test per process under `perf stat --no-inherit` (the process's first thread only):
  instructions, cycles, locked loads (`mem_inst_retired.lock_loads`), two runs on an otherwise idle machine at a
  parallelism of six, per-test minima, geometric means of ratios to `main` over the 36 tests (2.3, 2.4).
- **Whole-process instruction counts** (the rounds' and DESIGN-PROPOSALS' working measure): three runs on a loaded
  machine, minima. They include the compiler and collector threads, which execute about the same number of
  instructions in every configuration; every ratio to `main` is therefore diluted (delta-blue GIL off: 1.64 whole
  process, 1.72 main thread, 1.69 in time). They remain useful for ranking per-test mechanisms and useless as a
  budget. Where a whole-process figure is quoted it says so.

Two relations hold across all measured configurations and the ledger of Part 3 rests on them. (1) The main thread is busy
for the whole run: its CPU time equals the wall time within 2 % on every test examined except splay GIL off (188 ms of
1,193 waiting for the collector). Time is main-thread cycles. (2) The quiet pass's score times the main-thread cycle
ratio is 0.98 to 0.99 in all four configurations that have both (GIL on 0.947 x 1.050, GIL off 0.758 x 1.291, the two
experiment configurations 0.804 x 1.216 and 0.832 x 1.178). **A score of 0.90 is a main-thread cycle ratio of 1.09.**
(Addendum: where the factor 0.98 comes from is measured in FLAG-OFF-LANDING.md section 2.2. The cycle ratios here are of
single-test processes over all iterations; the score weighs the first iteration and the four worst as much as the other 115, and
every configuration of the branch, flag off included, pays more in those: flag off is 1.013 of `main`'s cycles over a whole run
and 1.039 over first iterations. The flag-on modes inherit that cost; their Worst Case sub-scores, 0.735 GIL off, are where the
same examination should be repeated.)

### 2.2 Scores

Quiet pass, full JetStream, medians of three, ratios of per-test scores to `main`, geometric mean of 36:

| configuration | score / `main` | Startup | Worst Case | Average |
|---|---|---|---|---|
| GIL on | 0.947 | 0.952 | 0.935 | 0.950 |
| GIL off, branch head (G-U today) | 0.758 | 0.794 | 0.735 | 0.757 |
| GIL off, branch head, one parked spawned Thread (G-T today) | 0.743 | 0.790 | 0.697 | 0.749 |
| experiment build: design set + Double family + lanes ("rows A to D"), never spawned | 0.804 | 0.837 | 0.768 | 0.811 |
| the same with a parked Thread (the Double family switches itself off at the spawn) | 0.776 | 0.801 | 0.731 | 0.796 |
| experiment build: every switch and every ceiling (no polls in optimized code, untagged words) | 0.832 | 0.868 | 0.795 | 0.839 |

The tenth round's pass had 0.958 / 0.766 for the first two rows; this pass is three rounds, not five.
**G-T is already below G-U today by 1.5 points** with identical main-thread instructions (2.3): it is all in the
Worst Case sub-score (0.735 -> 0.697). With one live spawned thread the exit-count thresholds are doubled (the thread
multiplier of section K) and a stop has a second client to account for: Air loses 17 points of score, FlightPlanner and
the two json tests 6 to 8, delta-blue 4 (splay gains 13: its collections take a different path with a second client).
K D6 (the multiplier counts the threads that exited) is therefore a G-T item, not only a scaling item.

### 2.3 The ladder, main thread

| configuration | instructions | cycles | cycles per instruction | score | locked loads per 1,000 instructions | locked-load cycles, % of `main`'s cycles |
|---|---|---|---|---|---|---|
| main | 1.000 | 1.000 | 1.000 | 1.000 | 0.25 | 0.0 |
| off | 1.015 | 1.011 | 0.996 | 0.990 | 0.29 | 0.3 |
| gilon | 1.037 | 1.050 | 1.012 | 0.963 | 0.34 | 1.0 |
| D-both | 1.163 | 1.132 | 0.973 | 0.906 | 0.35 | 1.5 |
| giloff | 1.349 | 1.291 | 0.957 | 0.783 | 0.48 | 4.7 |
| giloffT | 1.349 | 1.294 | 0.959 | 0.781 | 0.47 | 4.6 |
| xoff | 1.354 | 1.295 | 0.957 | 0.789 | 0.48 | 4.7 |
| design | 1.289 | 1.251 | 0.971 | 0.816 | 0.47 | 4.0 |
| t1 | 1.285 | 1.252 | 0.974 | 0.814 | 0.47 | 4.0 |
| pre | 1.249 | 1.241 | 0.993 | 0.827 | 0.48 | 4.0 |
| real | 1.212 | 1.216 | 1.003 | 0.840 | 0.50 | 4.1 |
| vis | 1.198 | 1.206 | 1.007 | 0.841 | 0.50 | 4.0 |
| nopolls | 1.172 | 1.195 | 1.019 | 0.847 | 0.52 | 4.1 |
| ceil | 1.123 | 1.178 | 1.049 | 0.865 | 0.71 | 6.6 |

`off` flag off; `gilon` GIL on; `D-both` GIL on with polls in optimized code and tagged words (the floor); `giloff` the
branch head; `giloffT` the same with a parked Thread; `xoff` the experiment build with every switch off. Then
cumulatively: `design` D-B1 to D-B4, the RegExp entry approximations (F-D1, F-D7), one-pass append (D-AG-1), the
lone-conductor rule, the MakeAtomString probe (L-D4), `join`'s fast path (ceiling of L-D10), the fused write predicate
and the fresh-owner rule (L-D3(a), (c)); `t1` no poll at the machine frame's entry; `pre` the Double family as GIL on
before the first spawn (C-D2, standing for C-D3) and F-D2 on the carrier; `real` no Int32-lane verification and no
second bound (what C-D3 withdraws); `vis` every FTL poll writes nothing (ceiling of decision 1's (c1)/(c2)); `nopolls`
no poll in optimized code at all; `ceil` untagged butterfly words in every tier and in C++. The `score` column is the
single-test score of these runs, not the gate's. `ceil`'s locked loads rise because the untagged ceiling sends array
appends of four tests (aes 18 M -> 74 M locked loads, gbemu, ai-astar, pdfjs) through a compare-and-swap path the tagged
build does not take: an artefact of the ceiling, worth about 2 % of its cycles.

What the table says.

1. **Cycles, not instructions.** GIL off executes 1.35 of `main`'s main-thread instructions in 1.29 of its cycles. The
   extra instructions are cheap: polls, masks and compares retire beside the useful work. Removing them buys less time
   than their count suggests. The floor configuration has 1.163 of the instructions and 1.132 of the cycles.
2. **Polls and tags are worth little in time once D-B1 is in.** Every remaining poll gone: 0.9 % of cycles. Every poll
   writing nothing: 0.8 %. Untagged words everywhere: 1.4 % (3.5 % without the artefact at most). No machine-frame entry
   poll: nothing measurable. In whole-process instructions the same four steps looked like 1.9, 1.1, 4.1 and 0.8 %.
3. **The design set is 3.4 % of cycles, the Double family with its lane checks 2.9 %.** Those are the measured part of
   the plan: 1.291 -> 1.216, score 0.758 -> 0.804 in the quiet pass.
4. **What remains is large and is not polls or tags.** With everything prototyped and every ceiling, GIL off is at 1.178
   of `main`'s cycles; GIL on is at 1.050. Twelve points, a third of the original distance, are in none of the switches.
5. **Locked instructions.** A GIL-off main thread retires 0.48 locked loads per thousand instructions against 0.25 on
   `main` and 0.34 GIL on. At twenty cycles each the difference to `main` is 4.7 % of `main`'s cycles (GIL on: 1.0 %). No
   switch moves it (4.0 % in every experiment row). 2.5 says where they are.
6. **Both flag-on modes pay 5 % of time over `main`** (GIL on: instructions 1.037, cycles 1.050, score 0.947). Every
   GIL-off
   number carries it; DESIGN-PROPOSALS group 2 is the work that removes it.

### 2.4 The floor

`--usePollingTraps=1` gives a GIL-on process the polls a GIL-off process executes in optimized code, without their
heap effects; `--useJSThreadsSingleOwnerWithGIL=0` gives it tagged words, every tag predicate in every tier and the
flag-on object-model bodies in C++. Both: 1.163 of `main`'s main-thread instructions, 1.132 of its cycles, 0.906 in
single-test score (whole process, three runs: polls alone 1.050 of GIL on, tags alone 1.047, both 1.105). A GIL-off
process in which everything else cost nothing would sit at about 0.87 of `main`. The designs of DESIGN-PROPOSALS remove
"everything else"; they cannot reach 0.90 by themselves.

### 2.5 What remains after rows A to D, by family

`perf record --no-inherit -e cycles:u`, GIL on against the `real` configuration, eighteen tests (those with the largest
remainder), differences by symbol grouped into families; each cell is the difference as a percentage of the test's
GIL-on samples.

| test | cycles / GIL on | generated code | Map, Set, WeakMap | promises, generators | RegExp | property tables, structures | strings | object model in C++ | allocation, collector | compiler on the main thread | calls, entry | other |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Air | 1.229 | 11.0 | 3.6 | -0.1 | 0.0 | 2.7 | -0.5 | 2.3 | 0.8 | -0.8 | 1.2 | 2.7 |
| Babylon | 1.296 | 21.9 | 0.0 | 0.1 | 0.0 | 0.4 | 2.5 | 1.8 | 0.7 | 0.6 | 0.2 | 1.5 |
| Basic | 1.361 | 25.0 | 10.8 | 3.1 | 1.0 | 0.5 | 0.3 | -3.7 | -0.7 | 0.1 | -0.2 | 0.0 |
| FlightPlanner | 1.158 | -8.4 | 0.0 | 0.0 | 2.8 | 10.6 | 8.9 | -0.4 | 1.9 | 0.7 | -0.8 | 0.6 |
| ML | 1.360 | 19.5 | 0.0 | -0.0 | 0.0 | 2.9 | 5.4 | 5.2 | 1.8 | 0.1 | 0.2 | 1.0 |
| OfflineAssembler | 1.294 | 10.2 | 0.6 | -0.0 | 8.5 | 0.5 | 2.9 | 0.5 | 1.2 | 0.1 | 3.2 | 1.8 |
| UniPoker | 1.183 | 8.4 | 0.0 | -0.0 | 3.7 | -0.1 | 3.3 | 0.0 | 0.5 | 0.2 | 0.2 | 2.1 |
| WSL | 1.288 | 10.5 | 6.4 | 0.7 | -0.0 | 1.3 | 2.6 | 1.9 | 1.9 | 0.0 | 1.3 | 2.2 |
| async-fs | 1.288 | 5.2 | 1.7 | 15.8 | 0.0 | 0.4 | -0.6 | 4.5 | 0.9 | 0.9 | -0.8 | 0.7 |
| delta-blue | 1.292 | 23.5 | 0.0 | 0.0 | 0.0 | 0.1 | 3.0 | 1.0 | 1.6 | 0.1 | 0.0 | -0.1 |
| earley-boyer | 1.169 | 12.7 | 0.0 | -0.1 | 0.1 | -0.1 | 0.3 | 0.9 | 2.1 | -0.7 | 0.4 | 1.2 |
| gbemu | 1.125 | 6.9 | 0.0 | 0.0 | -0.0 | 0.3 | 0.2 | 3.4 | 0.4 | 0.2 | 0.3 | 0.9 |
| hash-map | 1.173 | 14.4 | 0.0 | 0.0 | 0.0 | 0.0 | 0.3 | 0.9 | 1.7 | 0.2 | 0.2 | -0.3 |
| json-parse-inspector | 1.350 | 0.3 | 0.0 | -0.2 | 0.0 | 10.1 | 5.0 | 3.4 | 2.4 | 10.0 | 2.6 | 1.5 |
| json-stringify-inspector | 1.244 | -1.1 | 0.0 | 0.4 | 0.0 | 26.2 | -1.1 | 2.0 | 1.0 | -1.4 | -0.4 | -1.2 |
| pdfjs | 1.144 | 1.8 | -0.0 | 0.0 | 0.1 | 0.8 | 1.9 | 5.0 | 1.5 | 0.1 | 0.1 | 3.2 |
| raytrace | 1.303 | 25.0 | 0.0 | 0.0 | 0.0 | 0.2 | 0.2 | 0.6 | 3.7 | 0.1 | -0.2 | 0.7 |
| typescript | 1.149 | 9.4 | 0.0 | 0.0 | -0.0 | 3.2 | 0.3 | -0.8 | 1.5 | 0.1 | 0.1 | 1.1 |
| **mean of 18 tests** | | **10.9** | **1.3** | **1.1** | **0.9** | **3.3** | **1.9** | **1.6** | **1.4** | **0.6** | **0.4** | **1.1** |

- **Generated code is still the largest family, 10.9 % of these tests' time**, after inlined-entry polls, the
  visibility analysis' blind spot, the Double rules and the lane checks are gone. The poll and tag ceilings account for
  about 3 points of it (2.3). The rest is what GIL-off code does that no switch touched: claim compare-and-swaps in
  transitions, compare-and-swap length raises, allocation through per-thread allocator resolution, validated Map reads,
  inline caches through handler chains, calls where GIL on is inline. Locked loads sampled by address: 70 % of
  delta-blue's
  are in generated code, 47 % of Basic's, 29 % of aes's; delta-blue retires 6.9 M against `main`'s 0.8 M, about a tenth of
  its GIL-off time. Nobody has a census of them by node kind; P0.7 takes one.
- **Property tables and structures 3.3 %**: `PropertyTable::forEachProperty` out of line under the structure's lock
  (json-stringify-inspector 26 %), `addPropertyTransitionToExistingStructureConcurrently` under the transition table's
  lock
  (json-parse-inspector: 20 % of its locked loads), `findStructuresAndMapForMaterialization`, `getConcurrently`.
- **Strings 1.9 %**: atomic reference counts on `StringImpl` (UniPoker: 62 % of its locked loads are `operationArrayJoin`
  and the joiner's entries; json-parse: 27 % in the literal parser), `convertToNonRopeGILOff`, the shared atom table's
  slow
  adds, `operationMakeAtomString2WithCache` at twice GIL on's cost even with the probe.
- **Object model in C++ 1.6 %, allocation and collector 1.4 %, Map/Set/WeakMap 1.3 %** (Basic 10.8 %, WSL 6.4 %: WeakMap
  `get`/`has` under the cell lock are 30 % of Basic's locked loads), **promises and generators 1.1 %** (async-fs 15.8 %:
  the cell lock again, 19 % of its locked loads), **RegExp 0.9 %** (OfflineAssembler 8.5 %: the matching context's
  constructor), the compiler on the main thread 0.6 %, calls and entry 0.4 %.

None of these mechanisms is new to section L (L-5, L-6, L-12; designs L-D5 to L-D9). What is new is their sum in time:
13.6 % of these eighteen tests' time in the C++ families and 8 % in generated code beyond polls and tags, against 3.4 %
of the suite for the whole design set; and the fact that a good part of it is locked instructions, which instruction
counts weigh at one and the processor at twenty.

### 2.6 Per test (whole-process instructions, for ranking mechanisms)

| test | GIL on | floor | GIL off | design+T-1 | +Double, lanes | +no polls | +untagged | left over GIL on |
|---|---|---|---|---|---|---|---|---|
| Air | 1.084 | 1.204 | 1.313 | 1.303 | 1.286 | 1.279 | 1.179 | 1.088 |
| Babylon | 0.946 | 1.113 | 1.213 | 1.231 | 1.102 | 1.214 | 1.132 | 1.196 |
| Basic | 1.033 | 1.132 | 1.420 | 1.370 | 1.354 | 1.310 | 1.287 | 1.246 |
| Box2D | 1.016 | 1.089 | 1.230 | 1.152 | 1.142 | 1.111 | 1.063 | 1.046 |
| FlightPlanner | 1.071 | 1.111 | 1.388 | 1.307 | 1.351 | 1.334 | 1.296 | 1.211 |
| ML | 1.020 | 1.336 | 2.003 | 1.843 | 1.399 | 1.351 | 1.169 | 1.146 |
| OfflineAssembler | 1.044 | 1.119 | 1.370 | 1.336 | 1.299 | 1.281 | 1.258 | 1.205 |
| UniPoker | 1.044 | 1.180 | 1.427 | 1.311 | 1.276 | 1.260 | 1.153 | 1.104 |
| WSL | 1.099 | 1.131 | 1.374 | 1.313 | 1.332 | 1.286 | 1.268 | 1.153 |
| ai-astar | 1.049 | 1.300 | 1.438 | 1.341 | 1.305 | 1.258 | 1.082 | 1.032 |
| async-fs | 1.024 | 1.112 | 1.363 | 1.284 | 1.267 | 1.206 | 1.200 | 1.172 |
| cdjs | 1.026 | 1.107 | 1.166 | 1.106 | 1.107 | 1.078 | 1.071 | 1.044 |
| crypto | 1.014 | 1.109 | 1.458 | 1.429 | 1.151 | 1.036 | 1.024 | 1.010 |
| delta-blue | 1.013 | 1.294 | 1.637 | 1.277 | 1.284 | 1.116 | 1.073 | 1.059 |
| earley-boyer | 1.175 | 1.327 | 1.385 | 1.278 | 1.268 | 1.284 | 1.238 | 1.054 |
| first-inspector-code-load | 1.008 | 1.009 | 1.020 | 1.020 | 1.020 | 1.020 | 1.020 | 1.012 |
| float-mm.c | 1.000 | 1.097 | 1.098 | 1.098 | 1.098 | 1.000 | 1.000 | 1.000 |
| gaussian-blur | 0.999 | 1.029 | 1.025 | 1.009 | 1.008 | 1.005 | 1.005 | 1.006 |
| gbemu | 1.028 | 1.207 | 1.357 | 1.248 | 1.229 | 1.167 | 1.053 | 1.024 |
| hash-map | 1.043 | 1.272 | 1.746 | 1.438 | 1.420 | 1.146 | 1.100 | 1.055 |
| json-parse-inspector | 1.029 | 1.095 | 1.195 | 1.195 | 1.194 | 1.193 | 1.133 | 1.101 |
| json-stringify-inspector | 1.049 | 1.054 | 1.186 | 1.186 | 1.185 | 1.185 | 1.177 | 1.122 |
| mandreel | 1.006 | 1.115 | 1.157 | 1.093 | 1.090 | 1.025 | 1.017 | 1.011 |
| multi-inspector-code-load | 1.008 | 1.009 | 1.020 | 1.021 | 1.021 | 1.021 | 1.020 | 1.012 |
| navier-stokes | 1.006 | 1.290 | 2.683 | 2.685 | 1.126 | 1.039 | 1.014 | 1.009 |
| octane-code-load | 1.026 | 1.038 | 1.069 | 1.069 | 1.069 | 1.069 | 1.054 | 1.027 |
| octane-zlib | 1.003 | 1.056 | 1.083 | 1.089 | 1.082 | 1.008 | 1.003 | 1.000 |
| pdfjs | 1.026 | 1.116 | 1.264 | 1.240 | 1.204 | 1.185 | 1.121 | 1.093 |
| raytrace | 1.021 | 1.144 | 1.266 | 1.136 | 1.123 | 1.122 | 1.068 | 1.046 |
| regexp | 1.055 | 1.099 | 1.212 | 1.167 | 1.151 | 1.142 | 1.110 | 1.051 |
| richards | 1.019 | 1.131 | 1.213 | 1.063 | 1.053 | 1.027 | 1.033 | 1.013 |
| splay | 1.045 | 1.037 | 1.212 | 1.175 | 1.174 | 1.176 | 1.028 | 0.984 |
| stanford-crypto-aes | 1.020 | 1.198 | 1.697 | 1.579 | 1.087 | 1.062 | 1.048 | 1.028 |
| stanford-crypto-pbkdf2 | 1.077 | 1.269 | 1.313 | 1.310 | 1.336 | 1.301 | 1.157 | 1.074 |
| stanford-crypto-sha256 | 1.037 | 1.181 | 1.237 | 1.208 | 1.193 | 1.196 | 1.102 | 1.063 |
| typescript | 1.022 | 1.084 | 1.200 | 1.125 | 1.140 | 1.157 | 1.117 | 1.094 |
| **geometric mean** | **1.032** | **1.141** | **1.318** | **1.255** | **1.187** | **1.152** | **1.104** | **1.070** |

### 2.7 Found on the way

- **stanford-crypto-pbkdf2 has a mode five times slower GIL off**: 55.6 G whole-process instructions against 10.8 G. On a
  loaded machine the branch head takes it four runs in four (the experiment build one in four; a quiet machine rarely).
  Its SHA-256 block function exits 101 + 201 + 401 + 801 times at one `BadType` check in DFG code: section C-1's mechanism
  (a failed check on a merged local reports to no profile), on the array T4-C made Contiguous. C-D1 is its fix and is
  promoted to the defect group. stanford-crypto-sha256's bimodality (0.38 in the quiet pass) is the same defect.
- navier-stokes GIL on: 0.87 of `main`'s main-thread instructions in 1.28 of its cycles (score 0.894). Identical
  generated code (DESIGN-PROPOSALS section M); the cycles are not instructions. Not explained; a cache or alignment
  effect of where the flag-on heap puts the arrays is the first thing to test (P6).
- ai-astar, Babylon, async-fs and FlightPlanner vary by 5 to 20 % in whole-process instructions between runs under load
  in every configuration including `main`. Single runs of those are not evidence.
- Compiler threads: GIL off the parkable predicate falls through to `doesGCIgnoringClobberize` for every node on every
  `clobberize` call; 1 to 2 % of WSL's, typescript's and Air's whole-process instructions. It costs the main thread
  nothing
  and is one table lookup to fix (P4.6(g)).
- The experiment build with every switch off is not neutral: its RegExp prototype enlarges the matching context's
  constructor (OfflineAssembler +5 %, regexp +3 % whole-process instructions with or without the switch). Charged to the
  experiment, not to F-D1.

## Part 3. The ledger: from 1.29 to 1.09

Unit: main-thread cycles relative to `main` (time; section 2.1). Target: 1.09, which is a score of 0.90. Each row is a
multiplicative step. "Measured" is a step of section 2.3's ladder or a ceiling it established. "Share" is the part of a
ceiling that the named designs can realize; it is an estimate wherever nothing was prototyped, and the pessimistic,
central and optimistic columns make the total a range.

| row | what | packages | measured | kind | pessimistic / central / optimistic |
|---|---|---|---|---|---|
| A | the design set: polls D-B1 to D-B4, RegExp entry, one-pass append, lone conductor, MakeAtomString, join, fused predicate, fresh owner | P2, P3.1, P4.1-P4.4, P5.1 | 0.966 | prototypes (join: a ceiling worth 0.1 %) | 0.966 |
| C+D | Double family with threads alive; lane verification withdrawn; second bound hoisted | P3.2-P3.4 (C-D3) | 0.971 | C-D2's prototype plus two ceilings; C-D3 adds copies at encoding changes | 0.975 / 0.972 / 0.971 |
| | **after A to D** | | **1.216, score 0.804 in the quiet pass** | measured | |
| K | thread multiplier and the stop's second client, G-T only (today G-T is 0.743 against G-U's 0.758) | P1.6, P8 (K D6) | 1.020 | the measured G-U/G-T difference | 1.015 / 1.010 / 1.000 |
| E | polls that write nothing | not planned; Part 7 | 0.992 | ceiling | 1 / 1 / 0.996 |
| P | polls that remain in optimized code | Part 7 (T-3) | 0.991 | ceiling; T-3 (a contingency) covers counted loops only | 1 / 1 / 0.994 |
| T | tag sequences | P5.1 (T-4), P5.3 (T-2, optional) | 0.986 to 0.965 | ceiling with an artefact | 0.997 / 0.993 / 0.985 |
| R | the remainder: locked instructions, generated code beyond polls and tags, the C++ families of 2.5 | P0.7, P4.5, P4.6, P5.2 (R-1 to R-4) | 0.895 to reach GIL on (1.195 / 1.050 = 1.138, less the tag row) | not prototyped; families ranked in 2.5 | 0.966 / 0.945 / 0.927 (30 / 50 / 65 % of the ceiling) |
| F | what both flag-on modes pay over `main` (1.050 -> 1.035 / 1.025 / 1.020) | P6 | 0.952 to reach `main` | section M's and E's derivations; navier-stokes unexplained | 0.986 / 0.976 / 0.971 |
| | **G-T: cycles / `main`** | | | | **1.18 / 1.12 / 1.07** |
| | **G-T: score** (0.98 / cycles) | | | | **0.835 / 0.87 / 0.915** |
| | **G-U with the U track** (rows E, P and the generated-code part of T in full before the first spawn: 0.972; no row K) | P7 | | | **0.865 / 0.90 / 0.925** |
| | G-U without the U track | | | | 0.845 / 0.88 / 0.915 |

What the table says, plainly.

1. **The measured part of the plan takes GIL off from 0.758 to 0.804.** That is everything DESIGN-PROPOSALS designed for
   JetStream's single-threaded numbers, with C-D3 done.
2. **Nothing prototyped gets near 0.90.** Every switch and every ceiling together: 0.832.
3. **The central estimate for the whole plan is 0.87 with threads alive and 0.90 without**, and both rest on row R,
   which is eleven points of time, has no prototype, and is assumed to come in at half. At 30 % both gates miss by four
   to five points; at 65 % G-T passes by a point.
4. Polls and tags, which looked like a fifth of the distance in instruction counts, are three points of time between
   them after D-B1, and the designs that would attack them (T-1, T-2, T-3) buy at most one. They leave the base plan
   (Part 4 says what becomes of each).
5. The flag-on tax (row F) is worth as much as the Double family. It has been treated as GIL on's problem; it is half of
   what stands between GIL off and 0.90 once A to D are in.
6. The plan is therefore **not a plan that guarantees 0.90**. It is the set of changes whose measured part is 0.80, whose
   estimated part centres on 0.87 to 0.90, and whose largest term is known by family and not yet by design. Part 7 says
   what is decided at each judgement point, including the last one: stating the measured per-thread cost instead of a
   gate.

## Part 4. Designs that are new in this plan

DESIGN-PROPOSALS.md has the designs for everything the ledger cites by a section letter. This part adds what that
document does not have. It is in two halves. The first (R-1 to R-4) is aimed at row R, where the time is: locked
instructions and the C++ families of section 2.5. They are sketches at the depth this session could reach without a census
of generated code (P0.7); each becomes a full design, in DESIGN-PROPOSALS' template, as the first act of its package.
The second half (T-1 to T-4, U-1) was designed in full before the main-thread measurements existed, when polls and tags
looked like a fifth of the distance. The measurements demoted most of it; the designs are kept, with their measured value
in time stated, so that they are not proposed again for the wrong reason.

### R-1. No claim on an object nobody else can see yet

**Status: proposed (sketch). Evidence: 70 % of delta-blue's locked loads and 47 % of Basic's are in generated code
 (2.5).**

*Problem.* A property-adding transition GIL off claims the object (a compare-and-swap on the header or the butterfly word,
SPEC-objectmodel section 4) before it stores, so that a racing transition by another thread loses cleanly. A
constructor's `this.a = ...; this.b = ...` pays one locked instruction per added property on an object that no other
thread can have a pointer to. At about twenty cycles each the ones in generated code are about 7 % of delta-blue's
GIL-off time.

*Change.* In the DFG, a transition (`PutByOffset` + `PutStructure`, `MultiPutByOffset`, `PutById` with a transition
status, `AllocatePropertyStorage`/`ReallocatePropertyStorage`) whose base is proven **unescaped** uses the unclaimed form:
plain stores in `main`'s order, no compare-and-swap. Unescaped: the base is defined by `NewObject`, `CreateThis`,
`MaterializeNewObject`, `NewArray*` or `NewFunction*` in the same compilation, dominates the transition, and on every
path between them is used only as the base of a load or store or as an argument of a node on the phase's private list
(the list `noOwnerCheckOnFresh` uses, L-D3(c), extended from one block to dominance). Any other use (a store of it into
the heap, a call argument, a return, a `Phi` that merges it with something else) ends the region. It is the escape
analysis
of object-allocation sinking with a weaker goal, and it shares its pass.

*Soundness.* A pointer that has been stored nowhere and passed to nobody is in one thread's registers and stack only;
no other thread can run a transition on it, which is all the claim defends against. The collector is not a second
mutator of structure or butterfly. The stop protocol: a stop inside the region (at a poll or a parkable node) cannot
publish the object. OSR exit inside the region materializes the object as it does today and leaves the region.

*Expected.* To be derived from P0.7's census; from the two tests sampled, several percent of time on constructor-heavy
tests (delta-blue, raytrace, Basic, Babylon, earley-boyer, Air), about one percent of the suite.

### R-2. The cell lock is taken by the second thread, not the first

**Status: proposed (sketch). Evidence: `WTF::Locker<JSCellLock>` is 30 % of Basic's locked loads (WeakMap `get`/`has`) and
19 % of async-fs's (async generator queues); Map/Set/WeakMap are 1.3 % and promises/generators 1.1 % of the eighteen
tests'
time.**

Objects whose GIL-off protocol is "every operation under the cell lock" (WeakMap, the async generator's request queue,
the promise reaction list) get the butterfly word's discipline instead: the object records the TID that created it; that
thread operates on it with `main`'s code; the first operation by another thread converts it under a stop to the locked
form (one bit in the cell header's indexing-type byte or in the table header) and fires a watchpoint for optimized code
that had inlined the unlocked form. The conversion is the same event class as a butterfly's first foreign write (SPEC-
objectmodel F1) and reuses its machinery. Map and Set keep their validated lock-free reader for foreign readers; their
owner's `add`/`delete` become `main`'s in-place algorithm while the table is unshared, and `addNormalizedGILOff` remains
for shared tables. Risk: medium; each object kind is its own small protocol and needs the conformance set's item 1.

### R-3. Reference counts of strings a thread has not shared

**Status: proposed (sketch). Evidence: 62 % of UniPoker's locked loads are `operationArrayJoin` and the joiner's entries;
27 % of json-parse-inspector's are in the literal parser; GIL on already pays 94 M locked operations in one micro row
for the same reason (section M, D-M4).**

With the shared atom table every `StringImpl::ref`/`deref` is a locked add. D-M4 removes the last-reference case and the
fresh-cost bit. This plan adds: (a) the joiner and the literal parser hold `StringImpl*` without taking references for the
duration of one operation (the strings are kept alive by the array or the identifier table they were read from, under the
operation's own `EnsureStillAliveScope` or a `DeferGC`); (b) a `StringImpl` created by a thread and not yet stored into
the JS heap or the atom table is counted with plain adds (one flag bit, cleared by the first publication, which is a
release store; after that, locked). (b) is WTF-wide and needs its own argument for every publication point; (a) is local
and is most of the measured cost.

### R-4. Structure tables without the structure's lock on the reader side

L-D9 (lock-free transition map and property walk) promoted from "one of the emitter items" to a package of its own:
`PropertyTable::forEachProperty` and `Structure::forEachProperty` read a published, immutable-while-unpinned table through
the validated-snapshot discipline the deleted-property path already uses; `addPropertyTransitionToExistingStructure
Concurrently` probes the transition map without the table's lock and takes it only to insert. 3.3 % of the eighteen tests'
time (json-stringify-inspector 26 %, FlightPlanner 10.6 %, json-parse-inspector 10 %). It must land after P1.8, which
changes
the same function's publication rule.

### T-1. The machine frame's entry poll is the stack check

**Status: designed; NOT in the base plan. Measured: nothing in time (main-thread cycles 1.251 -> 1.252 with the entry poll
removed; 0.8 % of whole-process instructions). Kept because it is cheap and removes three instructions per call; land
it only with a measurement that shows a gain.**

*Problem.* GIL off every DFG and FTL function polls at its entry (`op_enter` -> `CheckTraps; ExitOK; InvalidationPoint`):
three instructions on x86-64 (the address of the VM's trap word, a test, a branch) in front of a stack check that is
itself four (thread-local lite, its soft limit, compare, branch). `main` executes two for the pair: its optimized tiers
have no poll and compare against an absolute address.

*Mechanism that already exists.* `StackManager` keeps a second limit word, `m_trapAwareSoftStackLimit`, equal to the
soft limit except while a stop is requested, when it holds `stopRequestMarker()` (a value no stack pointer is below).
The LLInt prologue compares against that word and calls `llint_check_stack_and_vm_traps` when the comparison fails;
the slow path serves the trap if there is one, re-checks the real limit, and either resumes the prologue or throws.
GIL off the word is per lite and the stop fan already writes every lite's copy (`VMTraps.h`, checklist item 3c): the
LLInt depends on it today.

*Change.*
1. `AssemblyHelpers::branchPtrAgainstSoftStackLimit` gets a sibling that loads
   `VMLite::threadContext.m_traps.m_stack.m_trapAwareSoftStackLimit` instead of the soft limit (same lite pointer, same
   instruction count). The DFG's `compileFunction` and the FTL's prologue patchpoint use it GIL off. Baseline keeps its
   explicit `op_check_traps`-free prologue and its loop polls, as today.
2. Both tiers check before the frame is allocated (SP equals FP, no callee save stored yet, arguments on the stack) and
   link failure to one shared thunk, `ThrowStackOverflowAtPrologue`. GIL off that thunk becomes
   `CheckStackAndTrapsAtPrologue`: it calls `operationCheckStackAndVMTraps(CodeBlock*)`; if the lite has a stop request
   the
   operation runs `VMTraps::handleTraps` (parking for the stop), then compares the frame's extent with the real soft
   limit;
   it returns null to jump back to the instruction after the check, or the frame to unwind to, as today. The frame header
   (`CodeBlock`, callee, argument count) is complete at that point, which is all a stack walk needs; nothing of the callee
   is live in registers. One thunk, no per-function code.
3. `DFGByteCodeParser`'s `op_enter` plants `InvalidationPoint` instead of `handleCheckTraps()` for the machine frame of a
   GIL-off, linked plan whose code block has no tail calls (the tail-call exception is D-B1's: there the entry is the
   back edge of a loop). With D-B1 this makes `op_enter` poll-free in optimized code; loops keep their polls.
4. Code that resumes after the slow path must not run on facts a stop invalidated: the `InvalidationPoint` of step 3
   follows the prologue, so a jettison during the park patches it, exactly as after a `CheckTraps`.

*Who writes, who reads.* Writers of the trap-aware word: the stop fan (under `m_mirrorLock`), `setStackSoftLimit`. The
reader is the owning thread's prologue: one relaxed load. A stale read of the ordinary limit while a stop is being
requested delays that thread's park to its next poll (loop or callee entry), as a stale read of the trap word does
today. Ordering on arm64: none needed; the word is a hint and the slow path re-reads under the protocol's own rules.

*Bounded progress to a poll.* Every cycle in the dynamic call graph passes a non-inlined entry (stack check) or a loop
back edge (poll). A leaf function without loops reaches its return in bounded time. Recursion through inlined frames
only is bounded by the inlining depth. So the time to the next poll is bounded as it is today.

*Failure modes.* Every DFG and FTL function has the check (neither tier elides it), so no optimized entry is left without
a poll. `forceTrapAwareStackChecks` (upstream's test option) keeps working: it pins the marker, every entry takes the slow
path.

*Tests.* `jit/entry-poll-is-stack-check-gil-off.js`: a thread spinning through a non-inlined recursive function with no
loop is stopped by a collection on another thread within the watchdog budget (before: by its entry poll; after: by
the stack check); `$vm` counter of entry-poll slow-path stops; disassembly check that `op_enter` of a DFG function has
no trap-word load GIL off. Stack-overflow tests of `JSTests/stress` in four modes (the throw path moved).

*Expected.* Ceiling measured in Part 2 (`noMachineEntryPoll` on top of the four poll switches). Call-heavy tests:
richards, raytrace, Babylon, earley-boyer, Air, typescript.

*Risks.* Resuming after the thunk needs the callee saves and the return address intact across the operation call (the
LLInt does the equivalent with `prepareStateForCCall`); the arity-fixup path of the DFG has its own failure label and
needs the same treatment. Low to medium: one thunk, two emitters, one parser line.

*Rejected.* Keeping the poll and making it one instruction (a load from a page the conductor protects): needs a fault
handler that re-enters the engine at arbitrary prologue states and competes with the embedder's signal handlers; kept as
a contingency (Part 7).

### T-2. One write predicate per object per poll-free window

**Status: designed; optional (P5.2). The tag row's whole ceiling is 1.4 to 3.5 % of time; this design's share of it is
under one percent.**

*Problem.* Every GIL-off store through a butterfly re-loads the tagged word inside a pinned patchpoint and re-evaluates
the owner compare (SPEC-jit I16: predicate and store must see one word loaded after the last poll): nine instructions
for an out-of-line store against two on `main`, six with the fused predicate (L-D3(a)). I16 asks for freshness with
respect to polls, not per store; nothing between two polls can change what the predicate decides (every transition of
the word that matters to the predicate happens inside a stop, SPEC-objectmodel section 4.6).

*Change.* A DFG node `CheckButterflyOwnerForWrite(base)` that defines the masked storage pointer and performs the write
predicate. `clobberize`: `read(JSObject_butterfly)`, `read(ButterflyOwnership)`,
`def(HeapLocation(ButterflyForWriteLoc, ButterflyOwnership, base))`. `ButterflyOwnership` is a new abstract heap written
by every node for which `jsThreadsParkableSlowPathClobbersHeapFactsGILOff` is true, by every poll regardless of its
attached set, and by every node that writes `JSObject_butterfly`. `PutByVal`, `PutByOffset` (out of line),
`ArrayPush`'s in-vector arm and `MultiPutByOffset` take the node's result as their storage edge GIL off instead of calling
`threadedButterflyLoadForWrite` at lowering. CSE then merges the predicates of several stores to one object in one
window; LICM cannot move it out of a loop that polls (the poll writes the heap), and can out of a strip-mined inner loop
(T-3). The FTL lowers the node with the pinned load it uses today, once.

*Soundness.* The I16 argument verbatim, with "the store's own load" replaced by "a load that no poll or parkable node
separates from the store". The three consumers of the parkable predicate stay in lockstep because the new heap is
written where that predicate is consulted.

*Tests.* Graph-dump test: two stores to one array in a loop body show one predicate; a store after a call shows a second.
The `transition-owner` and `shared-write` corpus directories under `validateButterflyTagDiscipline`, which learns the new
node. TSanJIT on `ConcurrentButterfly.cpp`'s foreign-write paths.

*Expected.* Kernels with k stores per iteration to one array save 5(k-1) instructions per iteration (aes 4 per round
column, sha256 message schedule 1, gbemu memory writes 2-3): 1-3 % on the crypto tests and gbemu alone; with T-3 the
predicate leaves the inner loop altogether.

### T-3. Counted loops poll once per strip

**Status: designed; NOT in the base plan (contingency, Part 7). Measured ceiling: removing every remaining poll from
optimized code is 0.9 % of time (1.9 % of whole-process instructions); strips cover perhaps half of that, for a new CFG
transformation. float-mm (1.098 -> 1.000 in instructions) is the one test it would move visibly.**

*Problem.* A loop's back-edge poll costs two to three instructions per iteration, and, more than that, it is a barrier:
no butterfly, length, structure or ownership fact crosses it, so everything `main` hoists out of a numeric kernel is
re-established every iteration GIL off (navier-stokes 31 -> 43 instructions per inner iteration with Double arrays,
float-mm
+10 %, crypto's `am3` +45 %). D-B3 helps loops the unroller takes; it does not take loops with unknown trip counts.

*Change.* A DFG phase after loop unrolling and before SSA conversion's consumers (it shares the unroller's
`LoopData`: pre-header, induction variable, step, exit condition). For an innermost natural loop whose body contains no
node that can park (calls, allocation slow paths: the parkable predicate) and that has the shape
`for (i = a; i < n; i += c)` with loop-invariant `n`:

    outer:  limit = min(n, i + K * c)
    inner:  while (i < limit) body          (no poll)
            poll; if (i < n) goto outer

K is chosen at compile time so that K times the body's node count stays under 32,768 (1,024 iterations of a 32-node
body, 64 of a 512-node one): a strip of a few tens of microseconds. The outer loop's header carries the loop's original
poll. Loops that do not match
keep their per-iteration poll (object-graph walks, `while` loops over data: delta-blue, richards, hash-map).

*Stop latency.* A stop now waits for the longest strip instead of one iteration: tens of microseconds by construction
(the stop watchdog's budget is 30 s). The package's first act is to measure today's time from stop request to last
park at two, four and eight threads on the scaling suite, so that the change is a measured delta; the acceptance test
bounds the 99th percentile at eight threads at 300 microseconds (decision 21).

*Visibility.* Inside a strip the code reads as `main` does between calls: values may be hoisted for the strip. This
is the visibility rule's existing statement (facts are refreshed at polls) with polls K iterations apart; decision 1
(D-B5 / D-B6) is not pre-empted. Under D-B5's "a write is seen within a bound" the bound becomes max(1 ms tick, one
strip).

*Tests.* Graph test of the transformed shape; a spawned thread spinning in a strip-mined loop is stopped (watchdog
margin); a reader in a strip-mined loop sees a flag written by another thread within the bound (conformance set item 5);
OSR entry into the inner loop (the entry block targets the inner header; the outer limit is recomputed at entry).

*Expected.* The ceiling of Part 2: `noPollsAtAll` minus T-1's and D-B1's share. Tests: navier-stokes, crypto, float-mm,
gaussian-blur, mandreel, gbemu, the three stanford-crypto tests, ML, octane-zlib.

*Risks.* A new CFG transformation in the DFG: medium-high. OSR entry and exit see one more loop level; exit state is
unaffected (no new bytecode origins; the outer header shares the inner header's origin with `exitOK`). First landing may
restrict itself to loops the unroller already recognizes.

*Rejected.* Removing polls from counted loops outright (HotSpot's old default): a 10^9-iteration loop would hold a
collection for seconds.

### T-4. Tag sequences: fused forms in every emitter

**Status: proposed (P5.1); FTL write arm prototyped (switch `fusedWritePredicate`). Mechanical; a few tenths of a
percent of time.**

The FTL write arm's `xor tag; test high half` form (L-D3(a)) moved to: the DFG's `SpeculativeJIT` write helper, the
Baseline/IC `emitWritePredicate`, the LLInt macro. Read side, x86-64 with BMI2: the masked load is one `bzhi r, [base +
8], r48` (mask and load fused; the register holding 48 is rematerializable); arm64: `ldr` + `and` with a bitmask
immediate as today. `noOwnerCheckOnFresh` (L-D3(c)) extended from same-block to dominance, and to arrays allocated by
`NewArray*` in the same function. Expected 0.5-1 % of the suite; low risk; mechanical.

### U-1. A single-mutator phase for optimized code only

**Status: proposed as the plan's second track (P7). Measured ceiling for optimized code before the first spawn: rows E,
 P and
the generated-code part of T, 2.8 % of time, which is what separates 0.88 from 0.90 in G-U's central column.**

*What it is.* L-D2's phase U restricted to what can be invalidated: DFG and FTL code compiled before the first spawn is
compiled with the forms a GIL-on single-owner process uses (SPEC-objectmodel G1's untagged emission, no polls in
optimized code, no visibility writes, no lane verification, in-place relabels as C-D2), under one process-wide
watchpoint set `neverSpawnedJSThread`. Everything that cannot be invalidated while it runs (C++, LLInt, Baseline, shared
inline-cache handlers) stays in its GIL-off form from process start. `new Thread` fires the set before the native thread
exists: every code block registered on it is jettisoned; the firing thread is the only mutator, inside a host call, so
the fire needs no stop.

*Why the restricted form is sound where L-D2 scope A needed an audit of 303 sites.* The two copies of the object model
never coexist in C++: C++ always runs the tagged forms. For untagged generated code to read and write the same heap,
every butterfly word that exists before the first spawn must equal its payload: tag 0 and SW 0. That holds if the first
carrier's TID is 0 for good (L-D2's first option). SW is set only by a foreign write, segmented words only by foreign
growth; neither exists before the spawn. After the fire, TID 0 is an ordinary owner tag and every tagged form is correct
on those words (the GIL-on configurations before G1 ran exactly that).

*What has to be shown.* (1) No lite-less thread (compiler, collector helper, sampling profiler) executes an owner-tested
write: they would read as TID 0 = the carrier. By reading they do not write JS objects; to be asserted
(`currentButterflyTID()` on a thread without a lite is a Debug assertion failure instead of 0). (2) A second carrier
entering the VM (`JSLock` registration of a second native thread) fires the set too. (3) The latch: the set is fired, and
the fire's jettison is complete, before `pthread_create`; the spawned thread's first instruction therefore runs with no
U-form code on any stack except the spawning thread's, whose U-form frames are below a host call and return into
patched invalidation points. (4) Code compiled concurrently at the fire: the plan is cancelled at finalization because
its watchpoint is invalid (the ordinary rule). (5) Traps in phase U: U-form code has no polls, so termination, the
watchdog and the debugger's break reach it the way they reach a GIL-on process today, by the signal sender planting
breakpoints at the invalidation points that stand where the polls would be (`InvalidationPoint` with the breakpoint-site
bit, `main`'s form). `Options::usePollingTraps()` therefore stops being a process-wide emission switch GIL off and becomes
a property of the plan (U-form: breakpoint sites; T-form: polls); `VMTraps` sets the trap word and the trap-aware stack
limits in both phases (C++, the LLInt and Baseline poll in both) and starts the signal sender only while the
`neverSpawnedJSThread` set is valid. After the fire no U-form code exists and the sender is never started again.
(6) The test modes that create SW or segmented words without a second thread (`forceButterflySWBit`,
`forceSegmentedButterflies`) and `forceTrapAwareStackChecks` disable the phase at option validation.

*What it buys.* A process that never spawns runs optimized code at GIL-on cost. What remains GIL-off-specific is C++
(tagged object-model bodies, per-thread RegExp context, the shared heap's allocation slow paths) and the lower tiers.
Measured ceiling in Part 2: 2.8 % of time. It moves G-T by nothing: it is not a substitute for the work on row R.

*Tests.* The latch test of the safety program (C-D2's prerequisite) generalized: a function hot before the spawn,
entered by the spawned thread as its first act, runs T-form code (counter of U-form entries after the fire is zero). The
corpus with the phase forced off (one option) stays a lane: T-form code keeps its coverage before the first spawn.

*Risks.* One process-wide watchpoint on every optimized code block: the fire is a whole-heap jettison, tens of
milliseconds in a large Bun process, once. The spawn itself becomes slower by that. Medium; most of the machinery
(untagged emitters, jettison) exists and is exercised GIL on.

## Part 5. Work packages, in order

Rules for every package. Spec first (the SPEC section and a history entry with the argument), then C++, then every tier.
A JSTests/threads test that shows the old count or the failure before and passes after. The standing battery per change:
the corpus in four modes on Release and Debug, TSanJIT in both GIL modes, the touched tests under the amplifier. After
each package: the main-thread sweep of section 2.1 (36 tests, `perf stat --no-inherit`: instructions, cycles, locked
loads; `main`, flag off, GIL on, G-U, G-T; two runs on an idle machine, minima) and the package's acceptance line checked
on it **in cycles**; whole-process instruction counts only to explain a per-test movement. After P3, P4, P5 and P7: a
quiet
score pass (full JetStream, interleaved, medians of five). A package whose acceptance line fails is not "mostly done": it
is examined until the difference is explained, and the ledger of Part 3 is corrected in this file.

"Safety first" names the item of Part 6 (section 6.5's table) that must exist before the package lands.

### P0. Instruments and re-adjudication (no behaviour change)

| item | content | done when |
|---|---|---|
| P0.1 | Part 6 step 0: the eleven preconditions, AB-1 to AB-26, CVE SUSPECT rows, heap containers, K4 against today's `VM.h` (47 unlisted members), OM-9; the S2 ledger and its regeneration script | every row has a verdict dated on the final tree; open rows joined to section 6.2 |
| P0.2 | Validators V-1 (amplifier sites in the object model, claim windows, relabels, IC publication, jettison, unwinding, `ArrayBuffer` transfer), V-4 (free-cell check at hand-out), V-5 (claim-window assertions), V-7 (concurrent-entry census), V-8 (signature ledger) | each runs in a lane; V-1 reproduces I-1a or the derivation is corrected |
| P0.3 | The `$vm` termination hook and an effective heap cap; `termination-storm.js` and `oom-one-thread.js` un-skipped | both tests run in both GIL modes |
| P0.4 | The fuzz rig rebuilt (toolchain, Fuzzilli checkout, instrumented build, triage scripts), generators added per section 6.4.1 | a one-hour GIL-off smoke campaign triaged |
| P0.5 | The measurement harness of this plan in `Tools/threads/perf`: the main-thread sweep (above), the G-T preamble, the quiet score pass driver, per-test tables; `perf` sampling period 4 M; a small set of Bun-level benchmarks chosen with the embedder, in the same configurations | the numbers of Part 2 reproduce within 1 % |
| P0.6 | The staleness-model conformance set (section 6.4.2), items 1-4, 6, 7, on the current tree, per tier | passes on the tenth round's tree, or each failure is a new row of section 6.2 |
| **P0.7** | **Census of generated code.** Cycles and locked loads of the main thread attributed to DFG node kinds: the FTL's disassembly dump lists, after each node, the address ranges of its instructions even without a disassembler; sampled addresses are binned into them (done for two tests in this session; it needs a machine with precise sampling, or the skid corrected by binning on the preceding range). For the 18 tests of section 2.5, GIL on against GIL off | a table like 2.5's with node kinds as columns; R-1's expected gain derived from it; every node kind that owns more than 0.3 % of a test's locked loads named with the protocol that emits it |

### P1. Defects (DESIGN-PROPOSALS group 0, plus two this plan promotes)

| item | content | acceptance | safety first |
|---|---|---|---|
| P1.1 | W4/W5: WebAssembly refusal as a generated choke point; `memory.atomic.wait` in a `GILDroppedSection` (G D3.1) | the six `api/wasm-*` tests; every call path refused; the wait is woken | - |
| P1.2 | Precondition 10: D-1(E), (F), I-1b | the CVE test extended with delete and accessor re-add, under V-1 | V-1, V-5 |
| P1.3 | `VMManager` D-3 | shell test written first; Bun `worker_threads` 50 runs under load | - |
| P1.4 | Module evaluation lock D-H5 | three module tests; TSan on the parent-list walk | - |
| P1.5 | Weak-bearing blocks D-J2 stopgap | string-heavy at one thread under 150 MB; gc-stress matrix | - |
| P1.6 | Loop entry gets its own artifacts and accounting: K D1, D2, D3 | the in-tree stale-loop test 801 -> under 10; `loop-entry-when-replacement-is-ftl-gil-off.js` bound 50; scaling gate | V-1 sites in jettison and loop-entry publication; unwinding-against-jettison test |
| **P1.7** | **Exit feedback for merged locals: C-D1.** Promoted from a performance item: on a loaded machine stanford-crypto-pbkdf2 GIL off runs 55.6 G instructions instead of 10.8 G in four runs of four (its hash kernel exits 101+201+401+801 times at one `BadType` check and spends the run below the FTL); sha256 is bimodal by the same mechanism | pbkdf2 and sha256: 10 runs of 10 within 3 % of each other in instructions, with 24 busy-loop processes beside them | - |
| P1.8 | I-2: `Structure::materializePropertyTable` re-checks under the structure's lock that the slot is still empty and the structure unpinned before it publishes, and replays from a snapshot of each chain member's transition fields taken under that member's lock (section 9.1; SPEC-objectmodel L6 first). F30: the abstract-interpreter reading session | the reduced `diagl.js` test (today 1 lost add in 150 runs with a small heap and two listing threads) 0 in 20,000; the checked campaign clean at both heap sizes; 1,000,000 amplified runs of `cve/mc-val-multislot-clone.js` without a divergence. F30 explained and fixed, or shown outside the engine | V-1 site between the walk and the lock in `materializePropertyTable`; V-4, V-6 |
| P1.9 | Messages and test hygiene (0.7) | - | - |

### P2. Polls

| item | content | acceptance (whole-process instructions per test; the suite in main-thread cycles) | safety first |
|---|---|---|---|
| P2.1 | D-B1 no poll at inlined entries; D-B4 `PutStack` is not a park site; D-B3 one poll per unrolled trip | the prototype's figures within 2 %: delta-blue 1.64 -> 1.33 of `main`, hash-map 1.75 -> 1.44, richards 1.21 -> 1.07, raytrace 1.27 -> 1.15; suite: about 2 % of main-thread cycles | conformance items 3, 5 baseline; watchdog budget re-measured |
| P2.2 | D-B2 only the loop's own poll refreshes | graph dump: only a loop's own poll shows `pollWrites(...)`, every other poll `pollWrites()`; delta-blue and hash-map not above P2.1's figures | the semantics argument of section B written into SPEC-jit I21 |
| P2.3 | (optional, not in the ledger) T-1 the machine frame's entry poll is the stack check: measured at nothing in time | land only with a main-thread measurement that shows a gain | stack-overflow stress tests in four modes; the termination hook |

### P3. Arrays and the Double family

| item | content | acceptance | safety first |
|---|---|---|---|
| P3.1 | D-AG-1 one-pass owner append and growth | aes, sha256, pbkdf2: `ensureLengthFreshCopy` near zero; aes -8 %, sha256 -2.8 %, pbkdf2 -2.5 % (section D's derivation; the prototype's aes figure is -8.5 %) | conformance items 1, 2, 6 for elements and length; TSan on `ConcurrentButterfly.cpp` |
| P3.2 | C-D3 stages 1-2: the raw-double bit in the butterfly word, validated everywhere, always true (behaviour-neutral) | flag-off identity compare; `validateButterflyTagDiscipline` learns the bit | V-3 |
| P3.3 | C-D3 stages 3-5: encoding-changing transitions are copies; T4-O, T4-C, T4-P and I41 withdrawn; Double arrays exist with threads alive | navier-stokes, ML, aes, crypto at the measured figures of Part 2's `pre`+`cd3` columns **under G-T** | V-2 over the 140-function table; the shape-change fuzzer generator; decision 6 |
| P3.4 | SPEC-jit history section 39's second bound computed once per storage edge instead of per access (new, small: the clamp moves from `publicLengthForBounds` to the node that defines the storage), hoisted with it by T-3 | gbemu, crypto, the stanford-crypto tests | - |

### P4. RegExp, strings, collector

| item | content | acceptance | safety first |
|---|---|---|---|
| P4.1 | F-D1 per-thread RegExp context resolved once per entry; F-D7 split builds its array directly | against the same build without them: OfflineAssembler -10 %, regexp -6 %, UniPoker -3 % (prototype: -12, -7, -3.5), and the matching context's constructor inline again in the 8-bit legs | - |
| P4.2 | F-D2 `RegExpTestInline`, folding and the cached-result record GIL off, per-thread | OfflineAssembler, regexp, UniPoker | mirror harness over the RegExp stress files at four threads |
| P4.3 | L-D4 `MakeAtomString` inline probe; L-D10 `join` from a private snapshot | UniPoker -6 %, WSL -3.7 % (section L) | conformance item 1 for the joined array |
| P4.4 | D-J1 lone-conductor rule, then the service conductor for long cycles | splay main-thread time out of JavaScript in collections 265 ms -> under 100 ms (GIL on: 69 ms); Bun marshalling predicate changed first | F31 contract in Bun; map-MC-GC re-run |
| P4.5 | The emitter items of L-D3(b), L-D5 to L-D9 | per-test lines of section L | L-D9: D-J2 proper |
| P4.6 | **The long tail** (section 2.5), family by family, largest first: (a) Map/Set/WeakMap: R-2 (the owner runs `main`'s code, the second thread converts), reads inline and validated in the DFG as in the FTL for shared tables, iterator `next` and collection allocation back in generated code; (b) promises, async functions, generators: R-2 for the request queue and the reaction list, generator and promise objects allocated inline; (c) RegExp entry: F-D1 in its real form (the context resolved once per entry point and passed down; the constructor inline again); (d) property-table walks and the transition map: R-4 (after P1.8); (e) strings: R-3 (reference counts), per-thread rope resolution buffers, L-D4's operation halved; (f) allocation through C++: L-D3(b) constant allocators per thread; (g) the compiler's predicate: the parkable test memoized per node kind (one table lookup; removes 1-2 % of three tests' process instructions); (h) generated code, the rest: decomposed per test as the package's first act (ML, Basic, UniPoker) | each family's samples within 1.3 x of GIL on's on its named tests; suite: with P5.2, row R's central share (half of its eleven points of time) | per family; (a) and (b) need the conformance set's item 1 for their objects and the mirror harness at four threads over the Map/Set/Promise stress files |

Checkpoint after P4: main-thread cycles at or under 1.22 of `main` with C-D3 in (the ladder's `real` row), quiet score
at or
above 0.80; the first pass over the long tail's families shows in row R.

### P5. Locked instructions and tags in generated code

| item | content | acceptance | safety first |
|---|---|---|---|
| P5.1 | T-4 fused tag forms in every emitter; owner check elided on fresh objects and arrays by dominance | out-of-line store 9 -> 6 instructions in every tier's disassembly; suite: a few tenths of a percent of cycles | V-2 |
| P5.2 | R-1 no claim on an object nobody else can see yet | delta-blue's main-thread locked loads 6.9 M -> under 2 M; the census's constructor-heavy tests (delta-blue, raytrace, Basic, Babylon, earley-boyer, Air) at the figures P0.7 derives | the escape region checked by `validateButterflyTagDiscipline`'s lint; a fuzzer generator that publishes `this` from inside constructors; conformance item 6 (publication) |
| P5.3 | (optional) T-2 one write predicate per object per poll-free window | graph test; aes, gbemu | the lint learns the node |
| P5.4 | Decision 1 stays (c0): nothing to implement. SPEC-jit I21 records the measurement (0.8 % of time for (c1)/(c2) on top of P2) | - | the conformance set complete |

Quiet pass after P5: **G-T is judged here.** Part 7 applies if it is under 0.90.

### P6. The flag-on tax common to both modes, and flag off

DESIGN-PROPOSALS group 2, unchanged: 2.1 (GIL on: FTL repatching inline caches D-M1, re-keys D-M2, `StringImpl` last
reference D-M4, D-M5, D-M6, D-M3 step 1), 2.2 (RegExp and string entry points per mode F-D4), 2.3 (marker counters E-D1,
`visitButterflyImpl` per mode E-D2), 2.4 (array and object creation paths per mode E-D3 to E-D5). Plus one item this
session found: navier-stokes GIL on runs 0.87 of `main`'s main-thread instructions in 1.28 of its cycles (explain it:
array
placement and 4 KB aliasing first). They count toward both GIL-off gates in full: GIL on is 1.050 of `main` in time,
0.947 in
score, and every point of it is a point of GIL off. Row F is as large as the Double family. Acceptance: GIL on at or
under 1.025 of `main`'s main-thread cycles, score at or above 0.97; flag off at or above 0.99.

### P7. The U track

U-1 (which contains C-D2). Acceptance: before the first spawn, main-thread cycles at or under 0.975 of the same tree's
T-form figure (rows E, P and the generated-code part of T); the latch test; the corpus lane with the phase forced off
stays green. Safety first: V-3; the lite-less-thread assertion; decision 9 re-taken as (C'). **G-U is judged here.**

### P8. Scaling

K D4 to D6 (what is left of tier-up under N threads after P1.6), D-J3 (up to three concurrent windows, measure first),
D-J2 proper. Acceptance: G-S.

### P9. Gate C

Part 6's campaign on the frozen tree: L1 to L10 at their exit criteria; the constant flips in the commit that cites them.

## Part 6. The safety program: what "as safe as `main`" means, and the work that gets there

This is the safety half of the plan. Parts 2 to 5 say what has to change for a GIL-off process to reach 0.90 of `main`
on JetStream; this half says what has to be true, and shown, before anybody may say that the same
process is as safe as `main`, and in which order the two kinds of work have to happen. It is written against the tree
at the tenth round's commit (rebased on `cf1b36ec8703`) and the documents beside it. Nothing here is implemented;
statements about the tree were checked against the sources and say so where they were only read.

Conventions. "GIL off" is `useJSThreads` with `useThreadGIL=0`, the shared collector and `useThreadGILOffUnsafe=1`;
"GIL on" is `useJSThreads` alone; "flag off" is the branch without the flag. Section letters (A to N) and design names
(D-1, D-H5, N-D2, ...) are those of `DESIGN-PROPOSALS.md`. "Lane" means one suite in one configuration on one build.
Costs are wall-clock estimates on a 64-thread x86-64 machine, from the rounds' own runs, and are estimates.

---

### 6.1 Definition: six properties, each with a check

"As safe as `main`" is not one property. It is the conjunction below; each item names the evidence that discharges it
and the state today. The definition is deliberately relative: `main` has bugs, races that upstream accepts, and
`RELEASE_ASSERT`s that fire on engine bugs. The claim is that the threads work adds no new class of any of these.

#### S1. Memory safety

**Claim.** No JavaScript program, racy or not, using any API reachable from script in the embedder, makes a GIL-off
process read or write memory outside the object it names, use freed memory, or treat a value of one kind as another
(a cell where a value is expected, a raw double where a boxed value is expected, a butterfly of one shape read as
another), beyond what the same program does on `main`.

**Check.** All of:
1. every entry of section 6.2 marked *memory-safety relevant* is closed (fixed with a test that failed before), or
   explained and shown unreachable;
2. every area of section 6.3 has a written disposition (reading checklist done, targeted test in the corpus);
3. the verification program of section 6.4 reaches its exit criteria with zero unexplained signatures: sanitizer
   reports, segmentation faults, `RELEASE_ASSERT` fires, the FTL's unreachable-block trap, debug assertions;
4. the validators of section 6.4.3 exist and run in a lane (they are what makes "zero signatures" mean something in the
   windows the existing tools do not widen).

**Today.** Not met. Known open: I-1a (a deleted slot reused inside a deferred-claim window: wrong kind), W4 (a spawned
Thread runs WebAssembly through three C++ call paths), H5 (module bookkeeping walked and appended without a common
lock), N2 to N4 on arm64, F30 (the FTL's unreachable trap reached during a typed-array transfer; not explained), I-2
(a persistent wrong value; this session found a reproducible sibling and a candidate mechanism in the property-table
rebuild, section 9.1; the free-cell hypothesis is out for ordinary runs). Not examined:
section 6.3.

#### S2. No new script-reachable crash or abort class

**Claim.** Every fail-stop the branch adds is either an internal invariant (it fires only if the engine has a bug, as
`main`'s own `RELEASE_ASSERT`s do) or a refusal of a configuration or of embedder misuse, documented in the embedder
contract. None is reachable from script in a supported configuration.

**Census (checked: `git diff cf1b36ec8703 HEAD -- Source/JavaScriptCore Source/WTF Source/bmalloc`, added lines,
comments excluded).** 596 C++ fail-stop sites: 538 `RELEASE_ASSERT`, 46 `RELEASE_ASSERT_NOT_REACHED`, 6
`RELEASE_ASSERT_WITH_MESSAGE`, 6 `CRASH()`; 56 `RELEASE_ASSERT` lines removed. 24 emitted traps: 18 `break` in the two
LLInt assembly files, 6 `jit.breakpoint()` (three in `DFGOSRExitCompilerCommon.cpp`, one each in `DFGThunks.cpp`,
`AssemblyHelpers.cpp`, `LLIntThunks.cpp`). 1,065 Debug-only `ASSERT` lines added, which matter because the embedder's
CI runs Debug builds. By area: `runtime/` 408 (`ConcurrentButterfly.{cpp,h}` 177, `JSObject.{cpp,h}` and
`JSObjectInlines.h` 45, `ThreadManager.{cpp,h}` 33, `JSLock.cpp` 21, `VM.cpp` 16, `ButterflyInlines.h` 14), `heap/` 90
(`Heap.cpp` 52; 19 are in `SharedHeapTestHarness.cpp`, test-only), `bytecode/` 32, `jit/` 21, `dfg/` 12, `ftl/` 12, the
rest 21.

**Classification by reading.** Three classes.

- *I, internal invariant* (about 580 of the 620): protocol state machines whose inputs are the engine's own words.
  Examples: the butterfly word's tag and owner checks in `ConcurrentButterfly.cpp`
  (`storeTaggedButterflyWordConcurrent`'s owner-TID assertion, the flavour preconditions of `trySegmentedTransition`),
  the lite life cycle (`VMLite::State` transitions in `JSLock.cpp` and `VM.cpp`), entry-scope pairing in
  `VMEntryScope.cpp`, `ArrayBuffer.cpp`'s size bounds, the conductor's election words in `Heap.cpp`, the OSR-exit
  traps ("foreign GIL-off lite: `didAcquireLock` foreclosure violated"). Two of these families have fired in fuzzing
  on script input before they were fixed (campaigns r3b and r47 of `FUZZ.md`: an ArrayStorage object reaching
  `trySegmentedTransition`; three foreign-TID callers of `setButterfly`), which is what the class looks like when it
  is wrong: an abort instead of corruption. The exit criterion for the class is statistical, in section 6.4.
- *C, configuration or embedder refusal* (about 35): `Options.cpp`'s three `CRASH()`es (unsupported platform;
  `useHandlerICInFTL` off; `useProfiler` GIL off), the second-mutator tripwire in `attachSpawnedThreadGCClient`
  (`ThreadManager.cpp`), a spawned thread entering a VM that is not its own (`JSLock.cpp`, "spawned threads are
  single-VM"), draining or clearing the VM's default microtask queue from a thread that is neither the carrier nor a
  spawned thread of that VM (`VM.cpp`, recorded there as interim fail-stops), running the deferred-work timer's tasks
  on a spawned thread (`DeferredWorkTimer.cpp`), creating a WebAssembly instance or module on a GIL-off VM behind the
  JavaScript-level refusal (`JSWebAssemblyInstance.cpp`, `JSWebAssemblyModule.cpp`), acquiring heap access another
  thread holds (`Heap.cpp`, upstream's assertion reworded). Each must appear in the embedder contract (S6) with the
  call that reaches it.
- *S, reachable from script or from load* (must be empty at the gate; today four):
  1. **The stop watchdog** (`watchdogAssertStopProgress`, `JSThreadsSafepoint.cpp`; `jsThreadsStopWatchdogMs`, default
     30 s). It turns "a stop never completes" into an abort. Causes seen so far: a lock held across a stop request
     (`STW-WATCHDOG-CLOSURE.md`), a native wait holding heap access without a release bracket, slow sanitizer builds
     under load. `main` has no such abort; it would hang. Accepting it is a decision (a hang is not safer than an
     abort), but the class of programs that reach it must be empty: every lane greps for its message.
  2. **`VMManager::enterStopTheWorldParticipation`'s counter assertion** with a second VM (I-3; explained; design D-3).
  3. **The FTL's unreachable-block trap** during a typed-array transfer on another thread (F30; about one in 1,500
     amplified runs of `cve/mc-grow-s4-detach-nullvec-repro.js`; not explained). The trap is `main`'s; reaching it is
     new.
  4. **Debug only, by reading:** lazily created VM members whose creator asserts "not yet created"
     (`VM::promiseAllFulfillFunctionExecutableSlow` and its fourteen siblings; eleven of the fifteen begin with
     `ASSERT(!m_...Executable)`, all end in a plain store). Two threads making their first `Promise.all` call can both
     take the slow path. Eight Debug GIL-off runs of a four-thread race did not hit it; the window is a few
     instructions. Release is benign (two executables, both live). It stands for a family: section 6.3, "lazily created
     VM members".

**Check.** A checked-in ledger assigns every added site its class with one line of reason; a script regenerates the
site list from the diff and fails when a site has no row. Class S is empty. Every C row is in the embedder contract.

#### S3. No new hang or unbounded-resource class

**Claim.** A program that terminates on `main` terminates GIL off; a process that runs in bounded memory on `main` does
so GIL off, up to a stated factor per thread.

**Check.** Zero hangs in every lane under a hang detector that distinguishes "blocked" from "burning CPU" (the mirror
harness's status 125 against 124); a peak-resident-set column in the scaling gate with a bound per test; the
termination tests of section 6.3 (watchdog and `VMTraps` termination with N threads) in the corpus.

**Today.** Open: W5 (a WebAssembly `memory.atomic.wait` parks holding the GIL: both flag-on modes), I-1c (two threads
that each hold an unfired claim and wait for the other: latent, not reachable in the tree, becomes reachable if D-1(F)
is implemented carelessly), J6 (weak-bearing blocks recycled 32 per cycle: `scaling/string-heavy.js` at one thread
peaks at 478 to 498 MB against 88 MB flag off), Bun's incomplete-body memory bound (not explained), two `sync/` tests
skipped in every mode whose headers blame the GIL's hand-over (`condition-worker-waiter.js`,
`condition-notify-all-multi-waiter.js`: a spinning thread starves the others GIL on; a GIL-on limitation, to be
documented as one, and the tests to be un-skipped GIL off), `semantics/termination-storm.js` and
`semantics/oom-one-thread.js` skipped for want of a test hook.

#### S4. Single-threaded behaviour equal to `main`

**Claim.** (a) Flag off, the branch is `main`: same results on every suite, generated code that differs only by
predicted-false tests of two Config-page bytes. (b) With the flag on and no second thread ever spawned, every
difference from `main` is on a published list of API extensions.

**Check.** The JSC suites and test262 against `main` in three configurations (section 6.4.1, lane L3); the flag-off
identity byte-compare; Bun's directories in three modes (L9). (Correction, same date: `Tools/threads/v5a-identity.sh`, which
the documents name for this, runs forty tests twice on one binary, with `--useJSThreads=false` and with no option; it does
not compare with `main` and looks at no machine code. The compare this property needs does not exist yet: FLAG-OFF-LANDING.md
section 2.3 and package L4.)

**Today.** (a) holds on the seven stress collections in Release (five failures that pass on `main`, all the
executable-allocation fuzzer class and the rope test's watchdog); never run: test262, the Debug build of the suites,
`testapi`. (b) 38 GIL on (34 are `Atomics` on ordinary objects: decision 2 / D-M7), 197 GIL off (117 WebAssembly, 34
`Atomics`, 16 FFI, 16 `array-slice-cow`, 2 `--useProfiler`, the rest small).

#### S5. What a racy program may observe

**Claim.** A program with data races observes, for every read, a value that some thread wrote to that location (or the
initial value), of the kind the location holds; per-location order is respected by one thread's successive reads; a
write becomes visible to a polling reader within a stated bound. Nothing else is promised, and nothing less.

**Check.** The staleness-model conformance set of section 6.4.2, per tier, on x86-64 and arm64. This is also the
contract against which the visibility ruling (decision 1: D-B5 or D-B6) is judged: the performance plan may relax
*when* a write is seen, never *what* is seen.

**Today.** The object-model specification states the per-location rules (SPEC-objectmodel ground truths; SPEC-jit I21
for polls); there is no test set that enumerates them per tier. The Double family (T4-O, T4-C, I41) exists precisely
because one of these rules (a stale Double-keyed reader over boxed lanes) had no cheap check.

#### S6. The embedder contract

**Claim.** Everything the engine now requires of an embedder that `main` did not is written down and checked in the
embedder's tree: destructors and unconditional finalizers may run on any thread that conducts a collection (F31);
host functions may be entered by N threads at once (the native-affinity design of `SPEC-nativeaffinity.md`, a
per-executable "concurrent-ok" bit and one serial lock for the rest, is a draft: nothing of it is in the tree, so
every one of the engine's 1,180 host functions and 167 custom accessors, and every embedder native, runs unserialized
GIL off); which API calls are refused on a spawned thread (class C above); native waits must release heap access or
poll (`parkSitePollAndParkForStopTheWorld`), or they trip the watchdog; the marshalling predicate ("is this the VM's
thread") must become "is this not a thread of the VM" before a service conductor exists (D-J1); `bun:ffi` and
WebAssembly are carrier-only.

**Check.** A contract document in the embedder's tree; a lane that runs the embedder's tests with a second thread
alive in three modes (L9); for natives, the concurrent-entry census of section 6.4.3 (V-7) names the natives that were
never entered by two threads at once in any lane, and that list is either read or put behind a lock.

#### S7. The gate in the code

The tree already contains the mechanical form of "not yet": `gilRemovalPreconditionsMetValue` is a `constexpr false`
(`bytecode/JSThreadsSafepoint.h`), asserted at the second mutator's attach unless `useThreadGILOffUnsafe` is set;
`Options.cpp` refuses the GIL-off shape without that option ("activation checklist incomplete"); the header records a
second attach point (the non-main carrier arm of `JSLock.cpp`'s `perThreadClientForCarrierEntry`) that still lacks
the assertion. **Definition of done for this whole document: one commit flips the constant, deletes
`useThreadGILOffUnsafe` and the refusal clause, wires the second attach point, and cites, for S1 to S6, the ledger rows
and campaign logs that discharge them.** The eleven GIL-removal preconditions (`INTEGRATE-jit.md`, consolidated list)
and the activation-blocker list of `INTEGRATE-ungil.md` (AB-1 to AB-26) were last adjudicated in June, before the
rebases and ten landing rounds; by a quick reading 5 and 11 have landed, 3 is closed by not recording delete caches
flag on, 9's stub operations are gone, 6 and 10 are open (N4, I-1); 1, 2, 4, 7 and 8 need one line of evidence each.
Re-adjudicating both lists against the final tree is the first work item (section 6.5, step 0).

---

### 6.2 Inventory: what is known unsafe or unexplained today

Status: **fixed** (in the tree, with a test), **designed** (mechanism known, change written in DESIGN-PROPOSALS, not
implemented), **open** (mechanism known, no design), **unexplained**. "MS" marks memory-safety relevance.

#### 6.2.1 Defects and residue from the design session

| id | what | modes | status | closing design and its evidence |
|---|---|---|---|---|
| W4 (G) | A spawned Thread runs a carrier-created WebAssembly export through `JSON.parse`'s reviver, `String.prototype.replace`'s callback and promise reactions: `Interpreter::executeCall` and the microtask call reach `vmEntryToWasm` without passing the refusal. Wasm code reads the VM block's words, not the thread's. **MS** | GIL on, GIL off | designed | G D3.1: the refusal as a generated choke point in both shared JS-to-wasm entries (`VMLite::isSpawned` byte). Tests: every call path refused (new), the six `api/wasm-*` tests |
| W5 (G) | `memory.atomic.wait` parks holding the GIL (hang) | GIL on, GIL off | designed | G D3.1: the wait inside a `GILDroppedSection`. Test: the wait is woken by a spawned thread |
| H5 (H) | Module evaluation reaches spawned threads (deferred namespaces, top-level-await continuations) and has no claim: 12 to 17 of 200 modules evaluated twice, TypeErrors, TDZ errors, a Debug assertion; by reading, `asyncParentModules()` iterated while appended. **MS** (the vector) | GIL off (GIL on by construction safe) | designed | D-H5: one module-evaluation lock released at the GIL's hand-over points. Three module tests; TSan on the parent-list walk |
| I-1a (I) | A deleted slot is quarantined by collection epoch; a collection can run between a claimant's publication and its fire; the slot can be handed to an accessor while optimized code that folded the old offset survives. Wrong kind. **MS**. Derived from the code path, not reproduced | GIL off | designed | D-1(E): no quarantine promotion while `s_deferredClaimsInFlight` is non-zero. The precondition's CVE test extended with a delete and an accessor re-add, fire entry perturbed (needs V-1) |
| I-1 rest | Eleven other deferring sites: value-safe by the table of section I; "the loser returns into its own stale code" | GIL off | designed | D-1(F): wait at transition-scope exit for other threads' claims; keep I-1c unreachable (no protected transition inside a claim scope) |
| I-1b | Claim observation is a relaxed load followed by a seq_cst load: reorderable on arm64 | GIL off, arm64 | designed | one acquire (section I, arm64 notes) |
| I-2 | Persistent wrong value under foreign churn (about 1 in 100,000 amplified runs of one test). This session's campaigns (section 9.1): the wrong value not reproduced in 314,000 runs; a sibling reproduced five times (a thread does not see its own add) and one Release abort in `materializePropertyTable`; ordinary runs perform no collection, which removes the allocator hypotheses and I-1a for them. Candidate by reading: `Structure::materializePropertyTable` publishes a table it assembled without the structure's lock over one that another thread has meanwhile pinned and edited (check-then-act across two lock holds). **MS** | GIL off | **candidate mechanism, overwrite window confirmed by a checked build (256 of 619 runs), link to the wrong value by inference** | P1.8: the re-check under the lock and the snapshot walk; T1/S4/S5 assertions kept as gated checks; exit: checked campaign clean at both heap sizes, then 1,000,000 amplified runs of the original test without a divergence |
| I-3 / F29 | `VMManager` counts a VM stopped and not active when it arrived unentered (a Worker constructed during a collection) | GIL off, 2+ VMs | designed | D-3, with a shell test that also confirms the diagnosis; Bun `worker_threads` 50 runs under load |
| J6 (J) | Weak-bearing blocks (every Structure block) recycled 32 per cycle end; unbounded growth for structure-heavy programs | GIL off | designed | D-J2 stopgap (sweep the new active sets' blocks at every Eden cycle end), then the two-half sweep; a peak-memory column in the scaling gate |
| N1 (N) | arm64 does not compile (`branchAtomicStrongCAS32/64`, `batomicweakcasi`) | arm64 | designed | N-D1 |
| N2, N4 | A structure check does not order the inline-slot read (or butterfly read) after it on arm64. **MS** | GIL off, arm64 | designed | N-D2: guard loads are load-acquire on weakly ordered targets; litmus tests on hardware |
| N3 | The virtual-call pair {arity entry, CodeBlock} is three unordered loads. **MS** | GIL off, arm64 | designed | N-D3 |
| A-3, K | Loop entry into the superseded DFG block bypasses the reoptimization trigger (2 exits on `main`, 903 GIL off); an FTL loop-entry block can exit at its entry on every entry; a watched fire's stop knocks every parked thread to Baseline | GIL off | designed (K D1 to D6) | Not memory safety: the superseded block is replaced, not invalidated, and keeps its watchpoints. It is an S3 item (a phase that runs in Baseline for its whole life looks like a hang to a user) and its fixes touch jettison and loop-entry publication, so they are in scope of the verification program |

#### 6.2.2 Carried over from the landing rounds

| id | what | status | closing |
|---|---|---|---|
| F30 | FTL unreachable-block trap in three threads at once while a typed array's buffer is transferred; hypothesis: an abstract-interpreter proof taken from concurrently mutable view fields without a watchpoint. **MS** | **unexplained** | a DFG-AI reading session over every `AbstractInterpreter` rule that reads a typed-array view's `length`/`vector`/`mode` or an `ArrayBuffer`'s detach state from the heap at compile time; V-6 (exit-state and unreachable census); the test under the amplifier with V-1's sites in `ArrayBuffer::transferTo`/detach |
| F31 | An embedder cell's destructor ran on the conducting thread; the embedder's `RefPtr` is thread-locked | open (embedder) | S6: contract; in Bun, destructors that touch thread-affine state post to the owning thread; D-J1's service conductor makes "never the allocating thread" the common case, so the contract must precede it |
| Bun natives on spawned threads | Host functions of the embedder entered from a JSC-spawned thread | open (embedder) | S6; V-7 census restricted to embedder natives; default refusal (TypeError) on a spawned thread for natives not on an audited list is the cheap form of the native-affinity design |
| Bun's accept loop has no bound | resource | open (embedder) | S3 |
| F2 after the fire, DFG tier | performance only (IC instead of inline claimed sequence) | open | none needed for safety |
| Concurrent indirect evals declaring one global function | a call in one copy can find the binding `undefined` for a moment ("foo is not a function", 3 of 4 mirror runs of `stress/activation-sink.js`) | open: not decided whether declaration instantiation racing a reader is permitted | S5: state the rule (global declaration instantiation is not atomic with respect to other threads' reads; each binding's initialization is), add a conformance test, or make `GlobalDeclarationInstantiation` publish function bindings initialized |
| VM-9 | a `node:vm` timeout terminates every thread | open | section 6.3, termination |
| OM-9 | the DFG element-write gap (`AUDIT-upstream-since-rebase.md`) | recorded open at the first round; not re-adjudicated | step 0 |
| GIL-off latency class; continuous collection GIL off in Debug is load-sensitive | S3 | open | lane budgets |

#### 6.2.3 Tool-accepted classes (what "0 reports" and "clean" currently exclude)

**ThreadSanitizer.** "0 reports" is relative to `Tools/tsan/suppressions.txt`, which has about 130 active matchers in
nine classes. Each is argued in the file; together they define what the TSan lanes cannot see:

| class | matchers (examples) | why accepted | what it blinds |
|---|---|---|---|
| Upstream parallel marking | `MarkedBlock::aboutToMarkSlow`, `noteMarked`, `SlotVisitor::donateKnownParallel`, `ParallelHelperClient::claimTask`/`runTask`, `Heap::reportExtraMemoryVisited`, `overCriticalMemoryThreshold` | exist flag off on `main`; racy by upstream's design | nothing of ours |
| Blessed concurrent probes (the object model's relaxed accessors) | `cellHeaderConcurrentLoad`, `butterflyConcurrentLoad`, `JSValue::decodeConcurrent`, `icConcurrentRelaxedLoad`, `WatchpointSet::state`, `IndexingHeader::publicLength`/`vectorLength`, `StructureChain::head`, `WriteBarrierBase<*>::cell`, `CallLinkInfo::owner()` | atomic relaxed loads whose results are revalidated by specification; the racing "write" is the allocator's hand-out | a future **plain** writer whose report contains one of these reader frames (recorded as the accepted masking trade-off) |
| Cell and block hand-out, JIT one-sider | `MarkedBlock::tryCreate`, `WarmUpBlockProvider::refill`, `MarkedBlock::Header::Header`, `PreciseAllocation::tryCreate`, `Butterfly::growArrayRight`, `ArrayBuffer::tryCreate`, `copyArrayElements`, `Butterfly::clearRange` | the reader is JIT code, which TSan cannot instrument; the real edge is publish-by-store and an address dependency | **any race whose one side is generated code.** This is the largest blind spot: TSan checks C++ against C++ only |
| Construct-then-publish | some thirty constructors (`JSString::JSString`, `JSBigInt::*`, `Symbol::Symbol`, `CodeBlock::CodeBlock`, `BytecodeGenerator::generate`, ...) | fields written before the cell is published through a JS heap store | a constructor that publishes `this` early |
| Idempotent lazy caches | `IntlCollator::updateCanDoASCIIUCADUCETComparison`, `JSBigInt::hashSlow`, `RegExpCache::ensureEmptyRegExpSlow`, `JSBoundFunction::canConstructSlow` | every racer computes the same word | a cache that stops being idempotent |
| Shared memory by design | `genericTypedArrayViewProtoFuncIncludes`/`IndexOf`, `WTF::findFloat`/`findDouble`/`unalignedLoad` | typed-array element races are JavaScript-level races | nothing |
| Parked-stack stale snapshot | `MachineThreads::tryCopyCooperativelyParkedThreadStack` | both racing values fail a bounds check by construction | a plain access added inside that function |
| Uninstrumented library | `libicui18n.so`, `libicuuc.so` | ICU publishes with its own atomics; calls on shared objects are `const` | **any race inside ICU**, including one caused by our sharing a non-`const` ICU object |
| Test-shell hooks | `functionSetRandomSeed`, `$vm` setters, `ScriptExecutable::setNeverInline` | process-global test state | nothing in a product |

Consequences for the program: TSan is evidence for C++-to-C++ protocol code only. Generated code against C++ and
generated code against generated code are covered by the amplifier, the mirror harness, the fuzzers and the
conformance set, which is why those carry the exit criteria. The suppression file is frozen at the gate: a new entry
needs a triage paragraph and review.

**Race amplifier.** Every `RaceAmplifier::perturb()` call is in the heap or in thread and VM teardown (checked: 25
sites in `Heap.cpp` 6, `ThreadManager.{cpp,h}` 6, `LocalAllocator.cpp` 3, `JSLock.cpp` 3, `VM.cpp` 3,
`GCSafepointEpoch.cpp` 2, `CompleteSubspace.cpp` 2). `AMPLIFIER.md` lists object-model, watchpoint and code-life-cycle
sites as intended; none exists. "Clean under the amplifier" therefore says nothing about transition windows, claim
windows, relabels, inline-cache publication or jettison. V-1 closes this before any amplifier hour is counted toward
an exit criterion.

**Fuzzers.** The last campaigns are of June (r3b: 4 h, r47 and r48: 2 h each, 310,000 to 420,000 executions, 10 to 12 %
edge coverage), on a tree ten rounds and three rebases older, mostly in the GIL-on shape; the rig (Swift toolchain,
Fuzzilli checkout, the instrumented build) no longer exists on the project's machine. No fuzzer has run on any tree
of the landing rounds. GIL-off fuzzing at scale has never happened.

**Scanners (`SCAN-RESULTS.md`, June).** Three residual families were left open and never re-run: `validateDFGClobberize`
traps on five to six corpus tests, `validateExceptionChecks` failing about fifty corpus runs at six sites, and a
segmentation fault in `tryCopyCooperativelyParkedThreadStack` with `gcAtEnd` while a Thread is parked in a terminated
wait. The sweep scripts the document names are not in the tree. Re-running the seven validation sweeps on the final
tree is lane L7.

**CVE audit (`CVE-AUDIT-STATUS.md`, June).** The 67 tests of `JSTests/threads/cve` pass in four modes on Release and
Debug. The audit's SUSPECT rows that are design gaps rather than tests were never re-adjudicated: MC-DOS S3 (no
per-thread heap quota), MC-GC S5 (native finalizer in a stop window), MC-GC S6 (FinalizationRegistry across threads),
MC-LIFE S4 (`m_pinCount`/`m_locked` non-atomic against detach, embedder-only), MC-LOCK S6, MC-TDWN S1 (last VM
dereference on a spawned thread), MC-TEAR's two, MC-SPEC (the flag is a high-resolution timer: an embedder
obligation). And the audit's own caveat 1 is now due: "every MC-GC immune verdict leans on no concurrent marking in
shared mode; concurrent-marking work must re-run map-MC-GC" - a GIL-off process runs one concurrent marking window per
cycle today (SPEC-congc 7.1a, which postdates the maps), and the map was not re-run.

**Heap containers (`AUDIT-heapcontainers.md`).** Checked against the tree: HC-01 is sharded (`m_markListSetShards`),
HC-03, HC-04 and HC-10 have their locks (`m_protectedValuesLock`, `m_weakGCHashTablesLock`, `m_observersLock`); HC-02
is closed by not using the cache GIL off (`JSArray::fastToString`'s copy-on-write string cache, `!vm.gilOff()`); HC-05
(`m_deprecatedExtraMemorySize`, a plain read-modify-write reached from the C API's extra-memory report) and HC-07
(`m_isInOpportunisticTask`, a plain `bool` under `SetForScope`) are still plain: accounting only, no memory-safety
consequence, to be made relaxed atomics so that TSan stays quiet when an embedder reaches them. HC-09, HC-11, HC-12
(needs-trace) have no recorded trace.

**The VM-member audit (SPEC-ungil annex K4, executed in June).** Checked mechanically: of 158 `m_` members named in
`VM.h` today, 47 appear in none of the K4 annex, the upstream-since-rebase audit, SPEC-ungil or SPEC-vmstate. Most are
the threads work's own or immutable after construction (`m_fast*Sentinel`, `m_privateSymbolRegistry`); some are
upstream additions that are lazily written (the fifteen `m_promise*Executable` members, `m_stringSplitCache` - the
latter is handled at its one use, `stringSplitFast` skips it GIL off, but is in no audit). At least one K4 row no
longer describes the code: VII.7 says `m_currentWeakRefVersion` is "written only inside the GC stop window"; it is
bumped by `VM::finalizeSynchronousJSExecution` on whichever thread ends a job (atomic, so memory-safe; the consequence
is that one thread's job end can end another thread's `WeakRef` keep-alive: an S5 item). Re-executing K4 against the
current `VM.h`, `JSGlobalObject.h` and `Heap.h` is step 0.

---

### 6.3 What has never been examined, or only read

"Tree" says what a quick check of the sources shows (checked unless marked). Risk is a guess: **H** plausible memory
unsafety, **M** crash, hang or wrong result, **L** semantic corner. "How" names the reading checklist, the targeted
test, and the tool.

| area | tree | risk | how to examine |
|---|---|---|---|
| Host function bodies at large (1,180 host functions, 167 custom accessors, 702 JIT operations, 189 slow paths) | The native-affinity design is unimplemented. Coverage is whatever the mirror harness (JSC's stress files on two threads), the corpus and the fifth round's directory sweep reached | **H** | V-7 census over all lanes gives the list never entered concurrently; read those bodies for: VM or global members, static locals, caches on cells, `Vector`/`HashMap` members mutated without the cell lock, `Strong`/`Weak` creation, re-entrancy into script between a check and a use; mirror harness at four threads; fuzzer generators per builtin family |
| Lazily created VM and global-object members | `LazyProperty` has a claim protocol (K.3); plain "if null, create, store" members do not (the fifteen promise executables; others not enumerated) | M (Debug assert; Release leak or a lost root if the first creator's object is referenced only by the overwritten slot) | grep every `WriteBarrier<>`/`std::unique_ptr` member of `VM`, `JSGlobalObject`, `Heap` written outside a constructor; each gets a once-flag, a CAS, or a per-thread copy; a four-thread first-use race test per family under V-1 |
| Debugger and Web Inspector with Threads | `Debugger.cpp` was read (pause state is the carrier's; hooks return early on a spawned thread; the shadow chicken is per thread); `inspector/` has no threads code at all; no test (the shell cannot pause) | M | reading: every `Debugger` callback reachable from `op_debug`, exception unwinding and `console.*` on a spawned thread; every `InspectorAgent` that iterates VM or heap state (`HeapAgent`, `RuntimeAgent` previews) while threads run; test: Bun `--inspect` with a spawned thread hitting a breakpoint, stepping and evaluating on the carrier meanwhile; decision: breakpoints on spawned threads are ignored (today) or stop the world |
| `$vm` and shell-only paths | 30 threads mentions in `JSDollarVM.cpp`; many hooks are process-global; suppressed in TSan as test-shell state | L for a product (`$vm` is off), M for the lanes themselves (a racy hook produces false findings) | list the `$vm` functions the corpus calls from spawned threads; make each either per-thread or locked |
| C API and Objective-C API under N threads | zero threads code in `API/*.cpp`; the fifth round recorded callback objects, classes and the API weak map as single-mutator and unsupported for sharing; `testapi` never ran on the branch | M (H if an embedder shares `JSObjectRef`s) | run `testapi` flag off, GIL on, GIL off; reading: `JSObjectMake` with a class (the per-context class data map), `JSValueProtect` (now locked), `JSContextGroup` entry from a second native thread (the non-main carrier arm, which is also S7's unwired attach point); state "unsupported" in the contract or lock them |
| `JSLock::DropAllLocks`, `GILDroppedSection`, nested entry | `JSLock.cpp` has 203 threads mentions and 21 added assertions; exercised by the corpus's blocking tests | M | a matrix test: drop-all at depth 1 to 3, on the carrier and on a spawned thread, around `Atomics.wait`, `Lock.hold`, a collection and a watchpoint fire; amplifier sites exist here |
| Heap snapshots, `HeapHolderFinder`, heap profiler | F6 (two builders) fixed; all three analyzers run inside a `PreventCollectionScope` | L | one test with four mutators running during a snapshot, Debug and TSan |
| FinalizationRegistry, WeakRef, WeakMap/WeakSet | registry maps are under the cell lock (upstream's discipline); `WeakRef`'s keep-alive uses one VM-wide version (above); weak-map finalization interacts with L-D9 if that lands | M | reading: `JSFinalizationRegistry::finalizeUnconditionally` and the callback's scheduling thread (deferred-work timer: carrier only - so cleanup callbacks never run on the registering thread if that was a spawned one: state it); test: register/unregister/collect storms from four threads; `deref()` twice in one job while another thread ends jobs |
| Intl and ICU | done well: lazy members through `intlLazyObject`/`intlLazyString` under the cell lock, `IntlSegments`/`IntlSegmentIterator` lock their break iterator GIL off, shared impls only GIL on | L to M (ICU internals are invisible to TSan) | keep; one fuzzer generator sharing each Intl object kind across threads; an ICU built with TSan for one campaign |
| Date cache, time zone | `DateInstance` bypasses its per-instance cache GIL off; `JSDateMath.cpp` has 3 mentions; the VM's `DateCache` and a time-zone change notification under N threads not checked | M | reading: `DateCache` members and `resetIfNecessary` against concurrent `Date` construction; test: `setTimeZone` on one thread during `toLocaleString` storms |
| Temporal | off by default (`useTemporal`); zero threads code | L | leave refused or run the mirror harness with it on once |
| Proxy and Reflect re-entrancy | zero threads code; the handler-trap offset cache race is argued memory-safe in the TSan file | M | the classic check-then-use audits of `main`'s CVE history replayed with the "side effect" coming from another thread instead of a trap: `ProxyObject::performGet`'s invariant checks against a target mutated concurrently; fuzzer generator exists (`SharedProxyGetterGenerator`) |
| Typed arrays, resizable and growable buffers, transfer, detach | heavily worked (`ArrayBuffer.cpp` 67 mentions, relaxed view fields, `mc-grow-*`, `mc-life-*` tests) and the home of F30 | **H** | F30's reading session; every DFG/FTL typed-array node's bounds proof against a concurrent `resize`/`transfer` (not just detach); generator: resize/transfer/detach storms against hot indexed loops at forced tiers |
| structuredClone, transfer lists, `postMessage` | not in JSC; in the embedder (serialization walks objects that other threads mutate GIL off) | M (H if the serializer caches structure-derived offsets across a walk) | embedder reading: the serializer's fast paths for arrays and plain objects (butterfly reads must use the concurrent accessors); test: clone an object under foreign churn |
| Generators, async functions, async generators resumed on another thread | sync generators and iterator helpers claim their resume (`@claimGeneratorResume`); async generators serialize requests (`JSAsyncGenerator.cpp`, reviewed); an async function's continuation runs on the settling thread in both flag-on modes | M | reading: what frame state is per-thread and captured implicitly across an `await` (none should be; check `ThreadLocal`, RegExp legacy statics, `lastAtomizedIdentifier`-like hints); test: a continuation that resumes on three different threads in turn, at forced tiers (OSR entry into a resumed generator body on a thread that never ran it) |
| Symbol registry | one leaf lock in `SymbolRegistry.cpp` (checked) | L | none |
| Property-name enumerators, `for-in` caches, `StructureRareData` caches | `StructureRareData.cpp` 10 mentions; enumerator cache watchpoints are consumers of the deferred-claim windows (section I's table); `JSPropertyNameEnumerator.cpp` no threads code | M (H: a cached enumerator's offsets used after a foreign delete is the I-1a shape without the quarantine argument being stated for it) | reading: `JSPropertyNameEnumerator` validity checks (`cachedStructureID`, the prototype-chain watchpoints) against a transition published but not fired; test: `for-in` over an object another thread deletes from and re-adds accessors to, at every tier |
| Code and bytecode caches, source providers | `CodeCache.cpp` 11 mentions; the fork's code-cache key is a standing item; `SourceProvider` id assignment asserted | M | reading: `CodeCacheMap` under its lock for every entry point (eval, `Function`, module, program); `CachedBytecode` decode on two threads of one provider (the decoder string table was touched in round 2) |
| Module loader (load and link) | refused on spawned threads GIL off (`importModule`); evaluation is H5 | M | covered by D-H5's tests plus: a carrier loading while a spawned thread evaluates an overlapping graph |
| Error stack capture, `Error.captureStackTrace`, the embedder's `onComputeErrorInfo` hook | `ErrorInstance.cpp` 9 mentions; `m_onComputeErrorInfoJSValue` and `m_nativeStackTraceOfLastThrow` are VM members in no audit | M | reading: who writes those two members and from which thread; test: four threads throwing and reading `.stack` lazily on a fifth |
| Exception unwinding through a frame whose code another thread jettisoned | handler tables are published arrays with a lock for growth (`m_exceptionHandlersLock`); code-origin pools fixed in round 5; no targeted test | **H** | test: thread A throws through deep frames of function f while thread B's watchpoint fire jettisons f's DFG/FTL code (V-1 site in `CodeBlock::jettison` and in `Interpreter::unwind`); check `handlerForIndex` on a jettisoned block, the catch-OSR-entry path, and `genericUnwind`'s `callerFrame` walk against a frame being OSR-exited by a stop |
| `eval`, `Function`, direct-eval caches | `DirectEvalCodeCache` has its lock (checked: `setSlow` re-checks under it); the indirect-eval declaration race above | M | the declaration-instantiation rule; test: direct evals of one string from four threads in one function |
| Termination, watchdog, `VMTraps` with N threads | `VMTraps.cpp` 130 and `Watchdog.cpp` 23 mentions; VM-9 open (a `node:vm` timeout terminates every thread); `termination-storm.js` skipped: no hook terminates a chosen thread from a test | M (H: a termination exception delivered to a thread inside a claim window or holding a cell lock) | add a `$vm` hook to request termination of a thread or of all; test the storm; reading: every `RETURN_IF_EXCEPTION`-free region of the concurrent protocols (`ConcurrentButterfly.cpp` holds locks across none, to be confirmed) |
| Out of memory with N threads | `oom-one-thread.js` skipped: the heap cap options are inert (`CompleteSubspace.cpp` compares against physical RAM) | M | make `forceRAMSize` effective for the cap or add a per-VM cap option; test: one thread hits the cap, the others survive or all fail cleanly |
| Memory-pressure handlers, opportunistic tasks | `VM::performOpportunisticallyScheduledTasks` sets a plain flag (HC-07); handlers not examined | L | reading only |
| Options mutated at run time | the Config page is frozen after initialization; the embedder disables environment options; `$vm`/shell `setOption` paths exist for tests | L | confirm no run-time writer of an option that generated code tests (the two Config-page bytes are latched: `s_gilOffProcessLatch` is asserted) |
| A process that forks | not examined | M | state unsupported after the first spawn, as for any multi-threaded process |

---

### 6.4 The verification program

Three parts: lanes with exit criteria (4.1), the conformance set (4.2), validators to build first (4.3). A *signature*
is a de-duplicated failure identity: sanitizer report key, faulting function and assertion text, watchdog message, FTL
unreachable trap, hang with its blocked frames, or a divergent output of a deterministic test.

#### 6.4.1 Lanes

| lane | what runs | builds and modes | exit criterion | cost (est.) |
|---|---|---|---|---|
| L1 corpus | `Tools/threads/run-tests.sh` default and `--cve` | Release, Debug+ASAN; GIL on, GIL off (4 modes each) | 0 failures, 0 unexpected passes; per change | 15 min Release, 1.5 h Debug |
| L2 TSan | the corpus under TSanJIT, both GIL modes; **widened**: `JSTests/stress` through the mirror harness under TSanJIT, two threads | TSanJIT | 0 reports with the frozen suppression file; per change (corpus), per release candidate (mirror) | 1.5 h; 12 to 16 h |
| L3 suites | `run-jsc-stress-tests` over stress, microbenchmarks, mozilla, es6, modules, complex, ChakraCore, **wasm**, and **test262** (`JSTests/test262` with its `config.yaml` and expectations is in the tree and has never been run on the branch), plus `testapi`, `testmasm`, `testb3`, `testair` | Release per release candidate, **Debug once per gate**; flag off, GIL on, GIL off | failure list equals `main`'s plus the published extension list (S4); the extension list is empty GIL on after D-M7 | 2 to 3 h per mode Release; test262 about 1 h per mode; Debug about 10 times that |
| L4 mirror | `Tools/threads/mirror/run-mirror.sh`, eval and func modes | Release at 2 and 4 threads; Debug+ASAN at 2; one pass with eager tier thresholds | findings are a subset of the artefact list (type-profiler dumps, `$262.agent` tests, timing sets) | 2 to 3 h Release per pass; 20 h Debug |
| L5 amplifier | corpus in four modes pass after pass (the rounds' campaign script), only after V-1 | Release and Debug | **48 consecutive hours** on the candidate tree with 0 crashes, 0 hangs, divergences only on the listed non-deterministic tests; any finding resets the clock after its fix | 48 h x 2 builds, in parallel |
| L6 fuzz | Fuzzilli with `Tools/threads/fuzz/JSCThreadsProfile.swift`; rig rebuilt (toolchain, checkout, `build-jsc-fuzz.sh`), crash triage scripts refreshed for the final tree | instrumented Release+assertions, GIL off; a smaller GIL-on campaign | **30 consecutive CPU-days GIL off and 10 GIL on with zero new signatures** after the last fix (32 jobs for a day is 32 CPU-days); edge coverage reported and not below the June campaigns' 12 %; every crash triaged to a root cause or reproduced on `main` | rig: 1 to 2 days; then calendar days |
| L7 validation modes | the corpus and a 1-in-10 sample of `JSTests/stress` under: `verifyGC`, `scribbleFreeCells`+`useZombieMode`, `collectContinuously`, `forceButterflySWBit`, `validateButterflyTagDiscipline`, `validateDFGClobberize`, `validateGraphAtEachPhase`, `validateExceptionChecks` (Debug), `GIGACAGE_ENABLED=0`; the gc-stress matrix | Debug+ASAN, Release where the option exists | 0 failures that flag off does not have; the three June residual families explained or fixed | 6 to 10 h |
| L8 arm64 | build (N-D1), then L1, L2 (TSan supports arm64), L5 for 24 h, the conformance set, litmus loops for N2/N3 (10^9 iterations each) | Linux arm64 and macOS arm64 hardware | as the x86-64 lanes; until then GIL off is refused on arm64 at option validation (decision 3) | first session: 2 to 3 days |
| L9 embedder | Bun's twelve directories with a second JS thread alive | Debug and Release; flag off, GIL on, GIL off | equal to stock apart from the published budget list; 0 panics; `worker_threads`/`web/workers` 50 runs under load (D-3) | 3 to 5 h per mode |
| L10 scaling and memory | the scaling gate with the peak-resident-set column | Release GIL off at 1, 2, 4, 8 threads | bounds per test (after D-J2: string-heavy at one thread under 150 MB) | 20 min |

Fuzzer generators. Existing (16): spawn, join, shared-object property storm, shared array resize race, dictionary
flip, `Thread.restrict`, property `Atomics`, property wait/notify, lock contention, condition wait/notify,
`ThreadLocal`, shared Proxy getter, cross-thread JIT warm-up, forced DFG and FTL compilation, collection. To add, each
tied to an entry above: WebAssembly on the carrier while threads run and wasm exports handed to threads through
callbacks (W4); deferred and top-level-await module graphs touched from threads (H5); delete, accessor re-add and
collection in one storm (I-1a); `WeakRef`/`FinalizationRegistry`/`WeakMap` churn; shared Intl objects; typed-array
resize, transfer and detach against hot loops (F30); shared generators and async generators; `for-in` against
foreign mutation; throw and catch across frames being jettisoned; `eval`/`Function` of shared strings; termination
(needs the hook); Double and Int32 arrays changing shape under readers (the C-D3 audit's 140 functions).

#### 6.4.2 The staleness-model conformance set

A directory of litmus tests, each a few lines, each run per tier (LLInt only, Baseline, DFG, FTL, by capping tiers
and by forcing thresholds), GIL off, 10^8 iterations, on x86-64 and arm64. For locations of each kind (inline
property, out-of-line property flat and segmented, array element of each indexing shape, typed-array element, global
variable, closure variable, `Map` entry):

1. *No thin-air, no wrong kind:* a reader sees only values some writer stored; a location that only ever held int32s
   never yields a double, a cell, or a hole; a property that was only ever data never yields a `GetterSetter`.
2. *No tearing:* a JSValue is read whole; a Double lane is one of the written doubles bit for bit.
3. *Per-location coherence:* two reads by one thread in program order do not go backwards against a single writer's
   increasing sequence (this is the rule a hoisted read in a poll-free region may violate across a poll; SPEC-jit I21).
4. *Message passing through `Atomics` and `Lock`:* a plain write before a release is seen after the matching acquire.
5. *Bounded visibility* (only once decision 1 is taken): a spinning reader with no synchronization sees a plain write
   within the stated bound under D-B5; under D-B6 the test asserts only items 1 to 4 and documents that a poll-free
   loop may never see the write, as on `main` for SharedArrayBuffer.
6. *Publication:* an object built by one thread and published by a plain store is seen with its constructor's fields
   or not at all (the construct-then-publish class TSan cannot check).
7. *Deletion and reuse:* a property deleted and another added never makes an old reader see the new property's value
   under the old name (I-1a's observable).

Exit: all pass in every tier on both architectures; the set is part of L1 afterwards at 10^6 iterations.

#### 6.4.3 Validators and tools to build before counting any hour

| id | what | catches | size |
|---|---|---|---|
| V-1 | Amplifier sites in the object model and code life cycle: between a transition's publication and its fire (`DeferredStructureTransitionWatchpointFire` scope exit), in `PropertyTable::releaseQuarantinedSlots`, in `relabelIndexingShapeConcurrent` and `convertToSegmentedButterfly` after the read and before the publish, in handler-IC publish under `CodeBlock::m_lock`, in `CodeBlock::jettison`, in `CallLinkInfo::publishRecord`, in `operationOptimize`'s loop-entry decision, in `ArrayBuffer` transfer and detach, in `Interpreter::unwind` | everything sections 6.2 and 6.3 call "window of a few instructions"; turns I-1a from derived to reproduced | small; slow paths only, per the amplifier's rules |
| V-2 | Tag-discipline lane: `validateButterflyTagDiscipline` exists as a DFG-graph lint (DFG and FTL call it) and is exercised by two corpus tests and the fuzzer profile; run it over L3 GIL off and JetStream at forced tiers (a 60-file sample of `JSTests/stress`, GIL off, Release: no violation); extend it to the Baseline and inline-cache emitters as a source lint (raw butterfly loads outside the helper functions) | a generated access that skips the tag mask or the owner proof | lane: trivial; lint: small |
| V-3 | Lane-encoding canary: a validation option under which every generated Double-shape read checks that the butterfly it read from is of Double encoding (after C-D3: the raw-double bit of the word), and every Int32-shape read checks `isInt32` (I41 does this in product code already) | the stale-shape reader the Double rules exist for; required before C-D2 and C-D3 land | small |
| V-4 | Free-cell check at hand-out: in a checked build `LocalAllocator`'s slow path and the free-list pop assert that the cell is zapped and mark it taken with a compare-and-swap | one cell on two free lists (I-2 hypothesis V); a block swept twice | small; Debug and one Release+assert lane |
| V-5 | Claim-window assertions: no quarantine promotion while a claim is in flight (the rule of D-1(E), asserted); own claims are zero at transition-scope exit; `awaitDeferredClaimsInFlight` never entered while holding an unfired claim (I-1c) | regressions of D-1; I-1c becoming reachable | trivial |
| V-6 | Exit-state and trap census: a validation option under which an OSR exit GIL off checks that every recovered value is a well-formed JSValue and every recovered cell is live and of a plausible type; every lane greps for the FTL unreachable trap and the watchdog message | F30's class (a proof taken from mutable state) as an exit with a bad value instead of a later crash | small |
| V-7 | Concurrent-entry census: a diagnostic counter per `NativeExecutable` and per JIT operation that records whether two threads were ever inside it at once; dumped at exit; aggregated over all lanes | the natives never exercised concurrently: the reading list of S6 and section 6.3's first row | small |
| V-8 | The signature ledger and the assertion-class ledger (S2) with their regeneration scripts | silent growth of class S; a lane's "clean" that ignores a known signature | small |

---

### 6.5 Order of work, and what gates what

**Step 0 (documents, no code; 2 to 3 days).** Re-adjudicate against the final tree, one table each: the eleven
GIL-removal preconditions, AB-1 to AB-26, the CVE audit's SUSPECT rows (re-running map-MC-GC for the concurrent
window), the heap-container rows, the K4 annex against today's `VM.h`/`JSGlobalObject.h`/`Heap.h` (47 unlisted members
to classify), OM-9; build the S2 ledger. Output: the list of rows that are open, which joins section 6.2.

**Step 1 (tools; about a week).** V-1, V-4, V-5, V-7, V-8; the `$vm` termination hook and an effective heap cap; the
fuzz rig. Nothing else is counted until these run. The two no-build experiments of D-2 run here.

**Step 2 (defects; package P1).** W4/W5, D-1(E)(F) with I-1b, D-3, D-H5, D-J2's stopgap, K D1. Each
with its test failing before. F30's and I-2's reading sessions run in parallel; they are the two items that can
still change a design.

**Step 3 (the unexamined; section 6.3).** In risk order: host-function census and the lazily created members;
unwinding against jettison; enumerator caches against the claim windows; typed arrays against resize and transfer;
termination and out of memory; debugger; the C API. Each ends in a disposition and a corpus test.

**What must precede which performance item** (Part 5's package numbers in brackets):

| performance item | safety work that comes first | why |
|---|---|---|
| Polls: no poll at inlined entries, one refresh per iteration, one per unrolled trip [P2.1, P2.2; D-B1 to D-B4] | the conformance set (4.2) items 3 and 5 on the current tree; V-5; the watchdog budget re-measured | fewer polls lengthen poll-free windows: stops take longer to gather, and a consumer of a claimed set runs longer on the old fact. The set fixes what may change (when) and what may not (what) |
| The visibility ruling [not planned; Part 7's contingency; D-B5 or D-B6] | the conformance set complete; decision 1 | it is a change of contract; the set is the contract |
| Pre-spawn in-place relabels and Double copies [inside P7's U-1; C-D2] | V-3; a test that the "no Thread has ever been spawned" latch is set before the first spawned thread can run a single instruction, and that code compiled before the latch is invalidated or never assumed it | the item is only sound if the latch is airtight |
| Encoding-changing transitions as copies [P3.2, P3.3; C-D3] | V-3, V-2 over the 140-function audit table, the shape-change fuzzer generator | it withdraws four special rules that exist for memory safety |
| Loop-entry artifacts and accounting, fire stops as code-life-cycle windows [P1.6, P8; K D2 to D4] | V-1's sites in jettison, loop-entry publication and unwinding; the unwinding-against-jettison test of section 6.3 | it changes when code is retired and re-opens a withdrawn design (SPEC-jit history section 41) |
| Service conductor and lone-conductor rule [P4.4; D-J1] | the embedder contract's destructor and marshalling rules in Bun (F31); D-J2's stopgap; map-MC-GC re-run | it changes which thread runs destructors and finalizers |
| One-pass append and growth [P3.1; D-AG-1] | conformance items 1, 2, 6 for array elements and length; TSan on `ConcurrentButterfly.cpp` | order of three stores in a published protocol |
| RegExp fast paths GIL off [P4.1, P4.2; F-D2] | F-D1 (per-thread context resolved once) first; mirror harness over the RegExp stress files at four threads | generated code reading per-thread state |
| `MakeAtomString` inline probe, `join` from a snapshot [P4.3] | conformance item 1 for the joined array; the I-2 key-cache reading stays valid (K) | both touch caches section I cleared |
| Lock-free transition map and property walk [P4.5, P4.6(d); L-D9] | D-J2 proper (weak-map finalization), the structure-transition fuzzer generator | touches weak finalization and the seqlock reader |
| WebAssembly on the carrier GIL off [after gate C; G D1] | W4/W5 fixed and their tests; the wasm fuzzer generator | it turns a refusal into execution |
| arm64 [decision 5] | N-D1 to N-D3, then L8 on hardware | nothing on arm64 is evidence until it has run there |
| The entry poll is the stack check [P2.3; T-1] | the stack-overflow stress tests in four modes; the termination hook of P0.3 (termination and the watchdog must be delivered through the stack check's slow path too); a V-1 site in that slow path | a trap that was served at an explicit poll is now served before the frame's locals exist |
| One write predicate per window [P5.2; T-2] | V-2's lint learns the node and fails a store whose storage edge crosses a poll or a parkable node; TSanJIT over the foreign-write tests | it relaxes "predicate and store see one load" (SPEC-jit I16) to "no poll between them" |
| Counted loops poll once per strip [P5.3; T-3] | conformance item 5 with the strip bound; the stop-latency percentile test; the watchdog budget re-measured at eight threads | poll-free windows become K iterations long |
| The single-mutator phase for optimized code [P7; U-1] | V-3; the latch test; the assertion that a thread without a lite never evaluates an owner predicate; one corpus lane with the phase forced off | code compiled before the first spawn assumes no other mutator |
| The long tail [P4.6] | per family: conformance item 1 for the family's objects, the mirror harness at four threads over its stress files, a fuzzer generator sharing the object kind across threads | each family replaces a GIL-off protocol by a cheaper one behind an ownership or never-shared proof |

Items with no safety prerequisite beyond the per-change battery: package P6 (the GIL-on items rest on G1's
single-owner argument; the flag-off items are instantiation per mode, covered by L3 and the identity compare).

**Three gates.**

- *Gate A, merge with the flag off by default.* S4(a): L3 flag off including test262 and one Debug pass, the identity
  compare, L9 flag off; builds on every platform the embedder ships (the arm64 compile breaks N1 fixed; the flag
  refused where unsupported). Nothing else in this document gates it.
- *Gate B, GIL on supported as an option.* W4 and W5 fixed (they are GIL-on defects too); S4(b) with the extension
  list published; L1, L2, L3, L4 (the mirror is a GIL-off tool: not needed), L9 GIL on; the C rows of S2 for GIL on
  in the contract. No fuzz campaign is required for this gate, but the GIL-on 10 CPU-days are cheap and recommended.
- *Gate C, GIL off supported: the constant of S7 flips.* Everything: steps 0 to 3 done; section 6.2 has no open or
  unexplained MS row (F30 and I-2 each either explained and fixed, or shown by a discriminating experiment to be
  outside the engine); every row of section 6.3 dispositioned; L1 to L10 at their exit criteria on one candidate tree,
  with the 48 hours, the 30 CPU-days and the mirror passes all counted on that tree after the last change to
  `Source/`; S2's class S empty; S6's contract merged in the embedder. arm64 stays refused GIL off until L8 has run.

What can follow gate C rather than precede it: the debugger on spawned threads (ignored breakpoints are a safe
default), Temporal, the C API's cross-thread sharing (documented unsupported), per-thread heap quotas (MC-DOS S3),
sampling of spawned threads, `--useProfiler`, macOS and Windows GIL off.

**Cost in total (estimate).** Steps 0 and 1: two weeks. Steps 2 and 3: three to five weeks, dominated by reading
(the host-function list and the two unexplained items). The campaigns of gate C: about ten calendar days on two
machines once the tree stops moving, and they restart on every fix. The performance work of the other half can
proceed beside steps 2 and 3 in the order of the table above; it cannot be counted toward gate C until the
campaigns have run on the tree that contains it.

---

### 6.6 Summary

1. "As safe as `main`" is six checkable properties (memory safety; no new script-reachable abort; no new hang or
   unbounded-memory class; single-threaded behaviour equal to `main`; a stated model for racy programs; an embedder
   contract) plus the gate the code already has (`gilRemovalPreconditionsMetValue`, `useThreadGILOffUnsafe`).
2. The branch adds 596 C++ fail-stop sites and 24 emitted traps; by reading about 580 are internal invariants, about
   35 refusals of configuration or embedder misuse, and four are reachable today (the stop watchdog's causes, the
   `VMManager` counter, the FTL unreachable trap of F30, a Debug assertion family on lazily created VM members).
3. Known memory-safety items: W4, H5's parent list, I-1a, N2 to N4 (arm64): all designed; I-2: mechanism confirmed
   (section 9.1), fix in P1.8. Unexplained: F30.
4. What the existing evidence excludes: TSan sees C++ against C++ only (about 130 suppressions in nine argued
   classes; generated code is invisible); every amplifier site is in the heap, none in the object model, watchpoints
   or code life cycle; no fuzzer has run since June or ever at scale GIL off; test262, `testapi` and Debug suites
   never ran; the June audits (preconditions, AB list, CVE SUSPECT rows, K4: 47 of 158 VM members unlisted) were not
   re-adjudicated after three rebases.
5. Never examined in execution: host-function bodies at large (the native-affinity design is unimplemented),
   unwinding against jettison, enumerator caches against claim windows, termination and out of memory with N
   threads, the debugger, the C API.
6. Build first: amplifier sites in the object model (V-1), the free-cell check (V-4), claim-window assertions (V-5),
   the concurrent-entry census (V-7), ledgers (V-8); then the lane-encoding canary (V-3) and exit-state check (V-6)
   before the Double and tier-up performance items.
7. Exit criteria on one frozen candidate tree: corpus and CVE suite 0 failures in eight lanes; TSan 0 reports
   including the mirrored stress suite; suites and test262 equal to `main` plus a published list; mirror at 2 and 4
   threads; 48 consecutive amplifier hours; 30 CPU-days of GIL-off fuzzing with zero new signatures; the conformance
   set per tier on x86-64 and arm64; Bun's directories in three modes.
8. Order: re-adjudicate, build tools, fix group 0, examine section 6.3; each performance item has a named safety
   prerequisite; gate A (merge, flag off) needs only flag-off parity, gate B (GIL on) needs W4/W5 and the GIL-on
   lanes, gate C (GIL off) needs everything and flips the constant in one commit.

## Part 7. If a gate is missed

The ledger's central column is a product of shares; its largest, row R, has no prototype. The plan therefore fixes in
advance what happens at each judgement point, so that a miss is handled by a rule and not by optimism.

### 7.1 G-T under 0.90 at the quiet pass after P5

In this order; each is taken only if the one before it did not close the distance.

1. **Re-measure the ledger's rows, not the total.** The instruction sweep after every package already gives each row's
   realized share. The row that fell short of its realizable column by the most is examined first (per-test table,
   `perf` diff against the experiment figures of Part 2). A row that cannot be explained is a finding, recorded in this
   file, and its realizable figure is corrected.
2. **The long tail, second pass (row R).** P4.6 and P5.2 take each family to its designed form (R-1 to R-4). The second
   pass takes the three largest remaining families further: Map and Set tables that carry their owner's tag and run
   `main`'s algorithm for the owner in every operation, not only the cell-locked ones (R-2 generalized; a project of
   the size of C-D3); thread-unshared strings' reference counts WTF-wide (R-3(b)); the transition claim elided for
   objects proven thread-local by their structure's watchpoint, not only unescaped ones.
3. **Polls.** T-3 (strips), then the poll as one instruction (a load from a page the conductor protects). Together at
   most 0.9 % of time by measurement; taken only if the distance is under a point.
4. **Decision 1 as (c2).** Every remaining poll writes nothing: 0.8 % of time measured, a deletion in code, a change of
   contract
   (a spin on a plain field may never end in FTL code; `Atomics` on ordinary objects becomes the way to signal).
5. **Accept and state.** If after 1-4 the quiet pass is under 0.90, the honest statement is: a process that
   has not spawned pays less than 10 % (G-U), a thread of a process that has pays N % (the measured figure), four
   threads deliver at least 3.2 times one. That is a product decision, not an engineering failure; this plan's claim 2
   of section 1.3 says so now, not after the fact.

### 7.2 G-U under 0.90 after P7

G-U differs from G-T by row K and by the U track (two points). A miss is a miss of row R or row F; the remedy is 7.1's
items 1 and 2. U-1's
scope is not widened to C++ (L-D2 scope A/B): two live copies of the object model in C++ is the failure mode decision 9
rejected, and nothing in Part 2 argues for re-opening it.

### 7.3 G-S missed on the allocating rows

D-J3 (up to three concurrent windows) is "measure first" by design. If the allocating rows stay under 2.4 x at four
threads after D-J1 and D-J3: per-thread Eden collection (a thread's young blocks are collected without a stop while no
old-to-young pointer into them was published to another thread) is the known next design and is out of this plan's
scope; the scaling gate's allocating bar is then restated as measured, with the collector's share named.

### 7.4 A safety criterion missed

No performance package is counted toward a gate until Part 6's campaign has run on the tree that contains it. A new
signature in the 30 CPU-day fuzz window or the 48-hour amplifier window resets the clock after its fix; three resets
caused by one package are the rule for backing that package out and landing the rest. I-2 and F30 are the two items that
can still change a design; if either is explained by a protocol error (not an implementation error), every package that
relies on the protocol is re-examined before it lands, and this file's order changes.

### 7.5 Things that would make this plan wrong

Recorded so that they are checked, not hoped against.
- The relation score = 0.98 / main-thread cycles (four configurations, section 2.1) stops holding: the collector's share
  of wall time grows (splay is the one test where the main thread waits today), or Worst Case scores move independently
  of cycles as they do between G-U and G-T. Every quiet pass reports the three sub-scores.
- The parked-thread preamble under-represents a program with threads: two threads that both allocate arrays defeat
  any elision keyed on "all instances of this structure are owned by one thread". The plan uses no such elision; T-4's
  fresh-object rule is per allocation site and thread-independent.
- JetStream is not Bun's workload. P0.5 adds a small set of Bun-level benchmarks, chosen with the embedder, measured in
  the same configurations; a regression there over 5 % blocks the package that caused it even if JetStream improved.

## Part 8. Decisions this plan assumes

DESIGN-PROPOSALS Part 3 lists nineteen decisions that are the project owner's. A plan cannot wait for nineteen answers;
it assumes the recommendation of that document except where this session's measurements changed it, and says what moves
if the owner decides otherwise. The three changed ones are marked.

| # | decision | assumed | what changes otherwise |
|---|---|---|---|
| 1 | visibility rule | **(c0) only, changed.** With D-B1 to D-B4 in, making every remaining poll write nothing is 0.8 % of time (1.1 % of whole-process instructions; section 2.3). D-B5's phase, per-thread trap delivery and ticker are not worth that. The promise stays as tested since the interim default: a read that can decide a loop's exit is performed again every iteration  | (c1)/(c2) add at most the measured ceiling and cost D-B5's phase or a contract change |
| 2 | `Atomics` on ordinary objects | (B) own option following the flag | - |
| 3 | racy plain accesses to different locations | (A) portable model | arm64 cost only |
| 4 | x86-64-only rules | (A) keep, each fallback exercised on x86-64 | - |
| 5 | arm64 GIL off a landing requirement | no: refused at option validation until L8 has run on hardware; GIL on after N-D1 | the plan's gates are x86-64 Linux |
| 6 | Double family | **(c) C-D3 is in the base plan, changed.** (b) alone moves G-U only; the Double family with its lane checks is 2.9 % of time (section 2.3), as much as the whole flag-on tax, and nothing else in the plan recovers it with threads alive. C-D2 is subsumed by U-1 | without (c) G-T loses row C+D of the ledger |
| 7 | vectorLength growth | (C) D-AG-1 | - |
| 8 | WebAssembly | stage 1 after gate C; W4/W5 now | not on the performance path |
| 9 | speed before the first spawn | **(C'), new option.** Not L-D2's scope A or B: U-1, the single-mutator phase for optimized code only. C++ never has two copies of the object model. G-T stays the gate that keeps the feature honest | without it G-U's central estimate is 0.88 |
| 10, 11 | FFI, sampling, messages, module lock | as recommended | not on the performance path |
| 12 | precondition 10 | (E)+(F) | - |
| 13 | collector | service conductor in full with the lone-conductor rule | splay and G-S's allocating rows |
| 14 | RegExp and strings flag off | F-D4 | flag off / `main` |
| 15 | where flag-on code polls | D-B1 in every polling configuration | - |
| 16 | LLInt caches | D-A1 after P1.6 | - |
| 17 | micro set | mono-proto variants added | - |
| 18 | flag off | all four items | P6's acceptance line |
| 19 | tier-up under N threads | K D2-D3 now (P1.6), D4-D6 in P8 | G-S |

New decisions this plan raises.

**20. The definition of the gate.** G-U, G-T or both (Part 1). Assumed: both must pass for the statement "GIL off is at
0.90 of `main`" to be made without qualification; G-U alone permits "turning the configuration on costs under 10 %".

**21. Which number is reported if 0.90 is not reached.** Assumed: G-U, G-T and the scaling factor, all three, with the
statement of section 7.1 item 5. The alternative, reporting G-U alone, would hide the cost a threaded program pays.

**22. The first carrier's TID is 0 for good** (U-1). Assumed yes, after the lite-less-thread audit.

## Part 9. What was built and run for this plan

Nothing under `Source/` is changed by this document's commit. For the measurements of Part 2:

- Three Release builds of the branch head with prototype patches applied (three patches; all five plus three ceiling
  switches; the same plus a checked-assertion patch for section 9.1), built once each, and one rebuild of the head after
  the patches were reverted. About 1,900 changed lines in 37 files, written by six parallel sessions against the head,
  each checked by `git apply --check` and a syntax-only compile through a VFS overlay before it touched the tree. None of
  it is reviewed code; two of the prototypes are knowingly unsound with a second thread (`joinFastPath`,
  `regExpInlineGILOff`), two ceilings hang a program that spins on a plain flag (`pollWritesNothing`, `noPollsAtAll`), one
  cannot run with a second thread at all (`untaggedGILOff`). They measured single-threaded JetStream and nothing else.
- Whole-process instruction sweeps: the 36 tests, one per process, in 17 configurations of the experiment builds, four of
  the head, `main`, and three floor configurations, three runs each on a loaded machine (about 3,500 process runs).
- Main-thread sweeps (`perf stat --no-inherit`: instructions, cycles, locked loads): 14 configurations, two runs each,
  idle
  machine, parallelism six; one whole-process sweep with cycles and locked loads for five configurations.
- One quiet score pass: full JetStream in one process, seven configurations, three rounds, nothing else on the machine.
- Profiles: instruction samples of thirteen tests (GIL on against the full-ceiling configuration); main-thread cycle
  samples
  of eighteen tests (GIL on against rows A to D); locked-load samples of six tests; a census by DFG node kind of two tests
  from the FTL's disassembly dump (no precise sampling on the measurement machine: locked-load samples skid).
- The safety half ran only existing binaries for seconds (a sample of `validateButterflyTagDiscipline`, a four-thread
  first-use race on Debug); its census of fail-stop sites is a `git diff` against the rebase base.
- The persistent wrong value (I-2): 314,000 amplified runs of a reconstructed diagnostic variant on the head's Release
  binary, then four minutes of the checked build on 32 cores; below.

### 9.1 The persistent wrong value (I-2) after this session's campaigns

The tenth round recorded, about once in 100,000 amplified runs of `cve/mc-val-multislot-clone.js` GIL off, a property
that reads another property's value and keeps doing so until its owner rewrites it. DESIGN-PROPOSALS section I cleared
the add and delete legs by reading and left an allocator hypothesis with two discriminating experiments (D-2). Those
experiments were run for this plan on the branch head's Release binary (amplifier options, 32 cores oversubscribed):

| campaign | runs | findings |
|---|---|---|
| value-address rings with values kept alive (one cell handed out twice), and not kept alive (premature free), rotating with the key read-back | 199,300 | 0 wrong values; 2 lost adds |
| the same two ring modes with a 256 KB heap (about 20 collections per run) | 60,000 | 0 |
| key read-back only, default heap / 256 KB heap | 35,450 / 19,200 | 0 / 2 lost adds and 1 Release abort |
| key read-back with two extra threads that only call `Object.keys(o)`, default / 256 KB heap | 281 / 150 | 1 / 1 lost adds |

1. **The wrong value was not reproduced** (0 persistent, 0 transient in 314,000 runs; the detector detects an injected
   one).
2. **A sibling reproduced, five times: a thread does not see its own add.** After `o[k] = v` the writer reads `undefined`
   through a rebuilt key and through the original string; `in` and `hasOwnProperty` are false; the structure's dump does
   not list the name; nobody else writes or deletes it. In one dump the lost key's offset has been issued again to the
   next add. Rates: 1 in 50,000 (default heap), 1 in 9,600 (small heap), 1 in 150 (small heap and two listing threads).
3. **A Release abort, once**, small heap: `Detected offset inconsistency ... Detected in materializePropertyTable`, a
   rebuilt table one entry short of its structure's `maxOffset`.
4. An ordinary run of the test performs **no collection** (19 MB allocated against a 32 MB first-cycle threshold). That
   removes every allocator hypothesis and I-1a for ordinary runs. The allocator side was read anyway: a block reaches a
   second allocator only through `canAllocate & ~inUse` or `empty & ~inUse`, `inUse` is set at `addBlock` and cleared only
   by the holder; no interleaving hands one block to two allocators.
5. **Candidate mechanism, by reading, fitting all six findings and every detail of the tenth round's record:**
   `Structure::materializePropertyTable(setPropertyTable = true)` (reached from every `ensurePropertyTable*` on a
   structure whose table slot is empty: `Object.keys`, `for-in`, a non-concurrent `get`) walks the chain and copies the
   ancestor's table without the structure's lock, then takes the lock, replays and publishes, and never re-checks that
   the slot is still empty and the structure still unpinned. A materializer that loses its core publishes over a table
   another thread has meanwhile materialized, pinned and edited in place: the in-place add disappears and the next
   offset is issued twice. Its replay also reads `m_transitionPropertyName` of chain members that a concurrent `pin()`
   clears: a link is skipped (the abort for non-dictionaries; silent for dictionaries, whose consistency check returns
   early flag on; a skipped deletion link resurrects a name, which with one offset reuse is the recorded wrong value).
   ThreadSanitizer is silent because every access is under some lock: it is check-then-act across two lock holds.
6. **Confirmed by a checked build for the overwrite itself.** A Release build with assertions that each name one
   hypothesis ran the
   same test (small heap, two listing threads) for four minutes on 32 cores. The assertion placed where
   `materializePropertyTable` is about to publish, testing that the structure has become pinned since the walk, fired in
   256
   of 619 runs; single-threaded that state is unreachable. (In the other 363 runs an unrelated check of the patch aborted
   first; that check also fires flag off on `main`'s own sweep and is a defect of the patch, as is its publication check,
   which reports valid states. Neither is evidence.) The window is entered constantly; how often it loses an edit
   depends on
   what the other thread did between its pin and the overwrite, which is the measured 1 in 150. That the wrong value
   itself is this defect seen through an offset reuse remains an inference.

What closes it: after taking the structure's lock and before the replay, return the published table if the slot is no
longer empty or the structure is pinned; re-walk if `previousID` or the transition name changed since the walk; the
replay reads each chain member's transition fields from a snapshot taken under that member's lock during the walk.
Flag off is untouched (one mutator). Package P1.8 carries it with its exit criterion: the checked campaign clean at
both heap sizes, then 1,000,000 amplified runs of the original test at load without a divergence.
