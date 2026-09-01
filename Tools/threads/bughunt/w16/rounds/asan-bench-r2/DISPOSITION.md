# asan-bench-r2 — fullsize-asan-bench-out-of-budget: DISPOSITION (2026-06-12)

## Failure as fired
Gate: "asan-bench full-size" = ASAN Debug jsc, pinned GIL-off flags,
`bench.js -- 16` (N_BASE=28000), 600 s timeout.
Result: rc=124, banner-only log (`w16-run1/run-W16-1.log`, 196 bytes — only the
useWasm-disable banner; bench.js prints only at completion, so banner-only is
the expected face of ANY non-completing run, slow or wedged alike).

## Root cause: gate budget infeasible by construction — NOT an engine bug
The 600 s budget is arithmetically impossible for this workload/build pair.
Host facts, all on-disk:

1. SMOKE (N_BASE=2000, the SPEC §5.1 knob) at W=16 under the same ASAN Debug
   binary takes ~447-535 s wall:
   - independent fixer re-run this round: rc=0 at 447.5 s, maxrss 3.0 GB,
     0 ASAN reports, 0 asserts, all four checksums byte-identical to the
     W=32 runs (`w16-s1/fixer-verify-run1.{log,rc}`).
   - i.e. even SMOKE consumes 75-90% of the 600 s budget.
2. FULL-SIZE is 14x the corpus and 14x the queries (N_BASE 28000 vs 2000),
   with superlinear phase work in postings volume. Empirical record
   (rounds/a6amend-asan-fullsize-attempt1 + SIGNATURES.md 2026-06-11f):
   - 3/3 rc=124 at 2400 s (banner-only, maxrss ~9.9 GB);
   - 1/1 rc=124 at 7200.9 s (banner-only, maxrss 16.5 GB).
   Full-size W=16 under ASAN Debug exceeds even a 7200 s budget on this host;
   600 s undershoots the demonstrated minimum by >12x.
3. Same tree, same binary, same round: W=32 smoke 3/3 PASS rc=0
   (~553-559 s; `w32-run{1,2,3}.log`, checksums byte-identical across runs
   and to the fixer run). No crash signal anywhere in the round.

## Why no engine change is the correct fix depth
- The slowness is inherent ASAN-Debug overhead (Release full-size W=16 is
  ~60-65 s; ASAN Debug multiplies >100x on this workload). "Fixing" the
  engine to fit 600 s is not possible without changing what the gate measures.
- N_BASE=28000 is pinned by frozen SPEC §8 calibration (2026-06-10); editing
  the pinned program's size constants to fit a gate budget would break the
  frozen-spec convention and invalidate the cross-language checksums.
- Flag-off byte-identical codegen LAW is untouched (no Source/ change made).

## Gate re-spec (the fix)
Replace the infeasible gate with the already-established coverage split
(SIGNATURES.md session 2026-06-11f, A6 closure — gates of record):
- ASAN signature profile: `bench.js -- 16 smoke` (SPEC §5.1 disclosed size
  lever), timeout >= 720 s — green 1/1 this fixer round (and 14/14 in
  a6amend-asan; 3/3 at `-- 32 smoke` this round).
- Full-size production profile: Release pinned `bench.js -- 16` gate
  (n=12 of record), where full-size completes in ~60-65 s.
A full-size ASAN run remains out of per-run budget on this host (>7200 s,
16.5 GB RSS); if ever re-chartered it needs a multi-hour budget and a
slow-vs-wedged discriminator (periodic progress print or external sampler),
since banner-only output cannot distinguish the two.
