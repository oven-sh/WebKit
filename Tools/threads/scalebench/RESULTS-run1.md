# SCALEBENCH RESULTS — 2026-06-10T05:46:15Z

**Known structural handicap:** GC under JS threads on this branch is currently stop-the-world with parallel marking; concurrent marking is designed (SPEC-congc.md) but not implemented. Go and Java ship fully concurrent collectors. Allocation-heavy phases (A especially) are expected to show STW pauses scaling with heap size and thread count. Results must be reported with this stated up front, not buried.

- Host: 64 cores, kernel 6.12.68-92.122.amzn2023.x86_64
- js: /root/WebKit/WebKitBuild/Release/bin/jsc [mtime+size: 1781068898 378642200] flags: --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1
- go: go version go1.24.13 linux/amd64
- java: openjdk version "21.0.10" 2026-01-20 LTS
- Constants: {"N_BASE": 28000, "V": 65536, "K": 128, "N_QUERIES": 28000, "seed": "0x5CA1AB1E0BADF00D"}
- Checksums (identical across all cells): {"A": "b3e65a6855b9bdeb", "postings": 4158957, "A2": "39c33392b2a4c5b2", "B": "c4bdd580f85ee058", "C": "af028188d7a56a96"}
- Default-flag exceptions (SPEC §4): ["js: OPEN ENGINE BUG (not a flag exception): shared-GC-heap corruption under the pinned GIL-off flag set at W>=4 \u2014 GC-dependent under-marking; live cells swept and re-allocated (repro: js/repro-bigint-shared-ingest.js, 0/4 corrupt with --useGC=0, 4/4 with GC on). js W>=4 cells are recorded as failed per SPEC \u00a76; js W>=2 runs with wrong checksums are quarantined as failed instead of aborting the batch. go/java cells and the js W=1 baseline are unaffected."]

Medians of 5 measured reps (1 warmup discarded); speedup is vs the same language at W=1. JSC compiler/GC helper threads are part of the platform and are not counted in W; cpu_util may exceed 1.0 for JS — reported as-is.

Note on cpu_util's wall basis: it is the FULL process lifetime from `/usr/bin/time` (per SPEC §4), which INCLUDES runtime startup/teardown (JVM class loading, jsc shell init) — unlike the phase/speedup tables, which are in-program barrier-to-barrier. At high W the in-bench wall shrinks while startup is a fixed, mostly serial cost, so cpu_util is systematically depressed for runtimes with slower startup (Java most, then js). Each run in results.json carries `inprogram_share` (in-bench wall / process wall) to quantify the dilution.

## Phase A — INGEST

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 15363 | 1.00x | 1275 | 1.00x | 1271 | 1.00x |
| 2 | null | null | 774 | 1.65x | 821 | 1.55x |
| 4 | null | null | 502 | 2.54x | 610 | 2.08x |
| 8 | null | null | 354 | 3.60x | 513 | 2.48x |
| 16 | null | null | 256 | 4.98x | 466 | 2.73x |
| 32 | null | null | 230 | 5.55x | 504 | 2.52x |
| 48 | null | null | 221 | 5.76x | 501 | 2.53x |
| 64 | null | null | 243 | 5.24x | 558 | 2.28x |

## Phase B — QUERY 90/10

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 2825 | 1.00x | 395 | 1.00x | 493 | 1.00x |
| 2 | null | null | 206 | 1.92x | 339 | 1.46x |
| 4 | null | null | 116 | 3.41x | 268 | 1.84x |
| 8 | null | null | 71 | 5.58x | 246 | 2.01x |
| 16 | null | null | 52 | 7.62x | 204 | 2.41x |
| 32 | null | null | 45 | 8.81x | 268 | 1.84x |
| 48 | null | null | 46 | 8.63x | 267 | 1.85x |
| 64 | null | null | 49 | 8.13x | 258 | 1.91x |

## Phase C — ANALYTICS

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 127 | 1.00x | 14 | 1.00x | 30 | 1.00x |
| 2 | null | null | 11 | 1.26x | 45 | 0.67x |
| 4 | null | null | 12 | 1.23x | 72 | 0.42x |
| 8 | null | null | 11 | 1.31x | 83 | 0.36x |
| 16 | null | null | 11 | 1.28x | 115 | 0.26x |
| 32 | null | null | 12 | 1.18x | 185 | 0.16x |
| 48 | null | null | 12 | 1.20x | 197 | 0.15x |
| 64 | null | null | 12 | 1.15x | 220 | 0.14x |

## Total

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 24307 | 1.00x | 1771 | 1.00x | 1898 | 1.00x |
| 2 | null | null | 1070 | 1.66x | 1307 | 1.45x |
| 4 | null | null | 712 | 2.49x | 1067 | 1.78x |
| 8 | null | null | 514 | 3.45x | 956 | 1.99x |
| 16 | null | null | 402 | 4.40x | 901 | 2.11x |
| 32 | null | null | 370 | 4.79x | 1079 | 1.76x |
| 48 | null | null | 362 | 4.89x | 1047 | 1.81x |
| 64 | null | null | 386 | 4.59x | 1134 | 1.67x |

## Peak RSS (MB, median)

| W | js | go | java |
|---|---|---|---|
| 1 | 421 | 178 | 837 |
| 2 | null | 178 | 766 |
| 4 | null | 180 | 800 |
| 8 | null | 182 | 796 |
| 16 | null | 186 | 868 |
| 32 | null | 196 | 755 |
| 48 | null | 211 | 939 |
| 64 | null | 206 | 914 |

## CPU utilization ((user+sys) / (wall * W), median)

| W | js | go | java |
|---|---|---|---|
| 1 | 1.02 | 1.48 | 2.05 |
| 2 | null | 1.31 | 1.65 |
| 4 | null | 1.11 | 1.31 |
| 8 | null | 0.84 | 1.02 |
| 16 | null | 0.62 | 0.86 |
| 32 | null | 0.40 | 0.82 |
| 48 | null | 0.31 | 0.76 |
| 64 | null | 0.23 | 0.69 |

## Failures (SPEC §6 — findings, not hidden)

- js W=2: 4 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=4: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=8: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=16: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=32: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=48: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=64: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
  - js W=2 rep0: exit_code=3 
  - js W=4 rep0: exit_code=3 
  - js W=8 rep0: exit_code=139 
  - js W=16 rep0: exit_code=139 
  - js W=32 rep0: exit_code=134 
  - js W=48 rep0: exit_code=134 
  - js W=64 rep0: exit_code=134 
  - js W=2 rep1: exit_code=3 
  - js W=4 rep1: exit_code=139 
  - js W=8 rep1: exit_code=139 
  - js W=16 rep1: exit_code=3 
  - js W=32 rep1: exit_code=3 
  - js W=48 rep1: exit_code=134 
  - js W=64 rep1: exit_code=134 
  - js W=4 rep2: exit_code=3 
  - js W=8 rep2: exit_code=3 
  - js W=16 rep2: exit_code=139 
  - js W=32 rep2: exit_code=139 
  - js W=48 rep2: exit_code=134 
  - js W=64 rep2: exit_code=134 
  - js W=2 rep3: exit_code=3 
  - js W=4 rep3: exit_code=139 
  - js W=8 rep3: exit_code=3 
  - js W=16 rep3: exit_code=139 
  - js W=32 rep3: exit_code=134 
  - js W=48 rep3: exit_code=134 
  - js W=64 rep3: exit_code=134 
  - js W=2 rep4: exit_code=3 
  - js W=4 rep4: exit_code=3 
  - js W=8 rep4: exit_code=139 
  - js W=16 rep4: exit_code=134 
  - js W=32 rep4: exit_code=134 
  - js W=48 rep4: exit_code=134 
  - js W=64 rep4: exit_code=134 
  - js W=2 rep5: exit_code=3 
  - js W=4 rep5: exit_code=3 
  - js W=8 rep5: exit_code=139 
  - js W=16 rep5: exit_code=3 
  - js W=32 rep5: exit_code=134 
  - js W=48 rep5: exit_code=134 
  - js W=64 rep5: exit_code=134 

