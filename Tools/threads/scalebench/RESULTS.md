# SCALEBENCH RESULTS — 2026-06-10T20:42:10Z

**Known structural handicap:** GC under JS threads on this branch is currently stop-the-world with parallel marking; concurrent marking is designed (SPEC-congc.md) but not implemented. Go and Java ship fully concurrent collectors. Allocation-heavy phases (A especially) are expected to show STW pauses scaling with heap size and thread count. Results must be reported with this stated up front, not buried.

- Host: 64 cores, kernel 6.12.68-92.122.amzn2023.x86_64
- js: /root/WebKit/WebKitBuild/Release/bin/jsc [sha256:b5c3a009d5142da68002104669896779c6ac3ab95ce11312c1d0119e2b5b7ab0] flags: --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1
- go: go version go1.24.13 linux/amd64
- java: openjdk version "21.0.10" 2026-01-20 LTS
- Constants: {"N_BASE": 28000, "V": 65536, "K": 128, "N_QUERIES": 28000, "seed": "0x5CA1AB1E0BADF00D"}
- Checksums (identical across all cells): {"A": "b3e65a6855b9bdeb", "postings": 4158957, "A2": "39c33392b2a4c5b2", "B": "c4bdd580f85ee058", "C": "af028188d7a56a96"}

Medians of 5 measured reps (1 warmup discarded); speedup is vs the same language at W=1. JSC compiler/GC helper threads are part of the platform and are not counted in W; cpu_util may exceed 1.0 for JS — reported as-is.

Note on cpu_util's wall basis: it is the FULL process lifetime from `/usr/bin/time` (per SPEC §4), which INCLUDES runtime startup/teardown (JVM class loading, jsc shell init) — unlike the phase/speedup tables, which are in-program barrier-to-barrier. At high W the in-bench wall shrinks while startup is a fixed, mostly serial cost, so cpu_util is systematically depressed for runtimes with slower startup (Java most, then js). Each run in results.json carries `inprogram_share` (in-bench wall / process wall) to quantify the dilution.

## Phase A — INGEST

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 18680 | 1.00x | 1255 | 1.00x | 1245 | 1.00x |
| 2 | 22607 | 0.83x | 771 | 1.63x | 828 | 1.50x |
| 4 | null | null | 498 | 2.52x | 611 | 2.04x |
| 8 | null | null | 352 | 3.57x | 530 | 2.35x |
| 16 | null | null | 259 | 4.84x | 470 | 2.65x |
| 32 | null | null | 225 | 5.58x | 493 | 2.52x |
| 48 | null | null | 226 | 5.54x | 533 | 2.34x |
| 64 | null | null | 220 | 5.70x | 568 | 2.19x |

## Phase B — QUERY 90/10

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 5591 | 1.00x | 390 | 1.00x | 507 | 1.00x |
| 2 | 6167 | 0.91x | 206 | 1.89x | 327 | 1.55x |
| 4 | null | null | 114 | 3.43x | 275 | 1.84x |
| 8 | null | null | 71 | 5.52x | 280 | 1.81x |
| 16 | null | null | 52 | 7.57x | 266 | 1.91x |
| 32 | null | null | 45 | 8.71x | 233 | 2.18x |
| 48 | null | null | 47 | 8.33x | 240 | 2.11x |
| 64 | null | null | 46 | 8.41x | 263 | 1.93x |

## Phase C — ANALYTICS

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 138 | 1.00x | 13 | 1.00x | 30 | 1.00x |
| 2 | 545 | 0.25x | 11 | 1.16x | 42 | 0.70x |
| 4 | null | null | 11 | 1.20x | 66 | 0.45x |
| 8 | null | null | 11 | 1.22x | 91 | 0.33x |
| 16 | null | null | 12 | 1.13x | 101 | 0.29x |
| 32 | null | null | 11 | 1.18x | 151 | 0.20x |
| 48 | null | null | 11 | 1.20x | 216 | 0.14x |
| 64 | null | null | 11 | 1.15x | 207 | 0.14x |

## Total

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 33373 | 1.00x | 1737 | 1.00x | 1893 | 1.00x |
| 2 | 41913 | 0.80x | 1068 | 1.63x | 1303 | 1.45x |
| 4 | null | null | 701 | 2.48x | 1041 | 1.82x |
| 8 | null | null | 512 | 3.39x | 977 | 1.94x |
| 16 | null | null | 401 | 4.34x | 931 | 2.03x |
| 32 | null | null | 360 | 4.82x | 976 | 1.94x |
| 48 | null | null | 370 | 4.70x | 1092 | 1.73x |
| 64 | null | null | 357 | 4.87x | 1153 | 1.64x |

## Peak RSS (MB, median)

| W | js | go | java |
|---|---|---|---|
| 1 | 11898 | 176 | 760 |
| 2 | 14389 | 178 | 767 |
| 4 | null | 179 | 802 |
| 8 | null | 182 | 797 |
| 16 | null | 189 | 869 |
| 32 | null | 200 | 726 |
| 48 | null | 199 | 939 |
| 64 | null | 207 | 863 |

## CPU utilization ((user+sys) / (wall * W), median)

| W | js | go | java |
|---|---|---|---|
| 1 | 1.16 | 1.47 | 2.03 |
| 2 | 0.72 | 1.31 | 1.69 |
| 4 | null | 1.10 | 1.29 |
| 8 | null | 0.85 | 1.03 |
| 16 | null | 0.62 | 0.87 |
| 32 | null | 0.41 | 0.80 |
| 48 | null | 0.30 | 0.78 |
| 64 | null | 0.24 | 0.72 |

## Failures (SPEC §6 — findings, not hidden)

- js W=2: 1 of 5 measured reps failed; median reported (>= 3 reps ok)
- js W=4: 3 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=8: 3 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=16: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=32: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=48: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
- js W=64: 5 of 5 measured reps failed; cell is null (< 3 reps ok)
  - js W=4 rep0: exit_code=133 
  - js W=32 rep0: exit_code=133 
  - js W=48 rep0: exit_code=133 
  - js W=64 rep0: exit_code=133 
  - js W=8 rep1: exit_code=133 
  - js W=16 rep1: exit_code=133 
  - js W=32 rep1: exit_code=133 
  - js W=48 rep1: exit_code=133 
  - js W=64 rep1: exit_code=134 
  - js W=4 rep2: exit_code=139 
  - js W=16 rep2: exit_code=133 
  - js W=32 rep2: exit_code=134 
  - js W=48 rep2: exit_code=133 
  - js W=64 rep2: exit_code=133 
  - js W=16 rep3: exit_code=134 
  - js W=32 rep3: exit_code=133 
  - js W=48 rep3: exit_code=133 
  - js W=64 rep3: exit_code=134 
  - js W=4 rep4: exit_code=134 
  - js W=8 rep4: exit_code=133 
  - js W=16 rep4: exit_code=133 
  - js W=32 rep4: exit_code=134 
  - js W=48 rep4: exit_code=133 
  - js W=64 rep4: exit_code=139 
  - js W=2 rep5: exit_code=134 
  - js W=4 rep5: exit_code=133 
  - js W=8 rep5: exit_code=133 
  - js W=16 rep5: exit_code=134 
  - js W=32 rep5: exit_code=134 
  - js W=48 rep5: exit_code=133 
  - js W=64 rep5: exit_code=133 

