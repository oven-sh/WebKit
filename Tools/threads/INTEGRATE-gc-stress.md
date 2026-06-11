# Integrating the gc-stress suite (JSTests/threads/gc-stress + Tools/threads/gc-stress-matrix.sh)

Staged under `staging-threads/`; integration is a single `mv` per subtree:

    mv staging-threads/JSTests/threads/gc-stress       JSTests/threads/gc-stress
    mv staging-threads/Tools/threads/gc-stress-matrix.sh  Tools/threads/gc-stress-matrix.sh
    mv staging-threads/Tools/threads/INTEGRATE-gc-stress.md Tools/threads/INTEGRATE-gc-stress.md

## Default-runner wiring (required — the suite is dark without it)

`Tools/threads/run-tests.sh` collects its corpus from explicit globs
(`api/`, `atomics/`, `races/`, `heap-*`, `objectmodel/`, `vmstate/`, `jit/`)
and does NOT pick up `gc-stress/`. Nothing else runs these four tests by
default: `gc-stress-matrix.sh` refuses a bare invocation by design, and its
`--full` mode is hours of saturating load.

Decision: ADD the suite to the default corpus. The four tests are written to
pass under the default GC regime (no scribble/zombie flags required for
correctness — those modes only sharpen the failure into deterministic
poison), are self-contained, and are bounded well under 30s each under
default GC. Edit the corpus-collection loop in `run-tests.sh`:

```sh
for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js \
         "$JT"/heap-*.js "$JT"/objectmodel/*.js "$JT"/vmstate/*.js \
         "$JT"/gc-stress/*.js; do
```

(One added glob; `[[ -e "$f" ]]` already makes it a no-op before the `mv`
lands, so the runner edit and the `mv` can land in either order.)

`gc-stress-matrix.sh` already includes `gc-stress/` in its own corpus globs
("plus the gc-stress suite when present") — no edit needed there.

## Pinned Validate-phase smoke (controlled, bounded)

    Tools/threads/gc-stress-matrix.sh --mode=scribble --filter=gc-stress/

- Scope: 1 mode x 4 tests = 4 runs. `--mode=...` is an accepted explicit
  scope, so this does not trip the bare-invocation refusal; the script
  prints the resolved plan and worst-case bound before the first jsc run.
- Worst-case wall clock: 4 x 120s (scribble's timeout multiplier is x1 on
  THREADS_TEST_TIMEOUT_SECS=120) = 8 minutes; expected real time is a few
  minutes. Exit 0 = all four PASS under sweep-time poisoning, which is the
  regime that gives zombie-uaf-canary.js and conservative-scan-register.js
  their deterministic teeth.
- Heavier follow-ups (not part of the smoke): `--quick` (races/ + jit/ +
  gc-stress/ under scribble + contgc) when the machine is otherwise idle;
  `--full` only as a dedicated rung.

## What each test enforces (one line each)

- `watchpoint-storm.js` — cross-thread watchpoint fire/invalidation: in-loop
  domain asserts catch torn/UAF reads; per-reader post-stop exact-value
  epilogue catches a per-thread fast path stuck on the pre-fire constant.
- `zombie-uaf-canary.js` — allocator reuse + stub-outlives-structure: canary
  reoccupation, and a LAYOUT-SHIFTED post-death probe so a recycled-
  StructureID stale stub reads poison, not the accidentally-correct slot.
- `conservative-scan-register.js` — conservative scan of a parked thread's
  frame/registers; the thread itself asserts the park overlapped the storm
  (waitResult === "ok"), so a missed overlap fails instead of passing.
- `havebadtime-vs-indexed-fastpath.js` — haveABadTime transition racing
  indexed fast paths on other threads.
