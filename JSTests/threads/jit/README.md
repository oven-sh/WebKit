# JSTests/threads/jit — SPEC-jit Task 13 validation suite

Owned by the jit workstream (SPEC-jit §11 Task 13). Two phases:

PRE-INTEGRATION (runs against the Task-1 STWR stub, phase-1 GIL):
  - `lint.sh`              — I13/I14/I18/I10/I21 + PA-transition + private-name grep lints (static, no jsc needed)
  - `golden-disasm.sh`     — I1 golden disassembly diff, flag-off (record/compare)
  - `bench-gates.sh`       — I1 `--useJIT=0` gate; flag-on {useJSThreads=1,useSharedGCHeap=0}
                             composite <=5% geomean GATED; {1,1} MEASURED+RECORDED (not gated);
                             fires/sec + shared-constructor microbench RECORDED
  - `tag-discipline.js`    — I14 `--validateButterflyTagDiscipline=1` corpus + I21 poll run
  - `ic-publish-reset-loops.js`        — I6 packed-word flip under readers + §5.1 publish/reset loops
  - `spawned-thread-butterfly-stress.js` — I14 spawned-thread butterfly stress (GIL-interleaved)
  - `shared-arraystorage-stress.js`    — I20 shared-AS stress (GIL-interleaved)
  - `tid-tag-3-threads.js`             — I19 (landed at Task 1b)
  - `fires-per-sec.js` / `construction-shared-constructor.js` — recorded benches

INTEGRATION GATE (skipped-by-default while STWR is stubbed; re-run at M4/CS2;
validates the N-separate-VMs config ONLY — N threads in ONE VM is the Phase-B
charter, SPEC-jit R1 freeze scope):
  - `int-gate-jettison-vs-execute.js`
  - `int-gate-fire-vs-execute.js`
  - `int-gate-direct-call-relink.js`
  - `int-gate-epoch-reclaim.js`   (also runs PRE-integration in its legacy-GC
                                   variant iff heap's GCSafepointEpoch landed, N6)
  - `int-gate-stop-budget.js`

Driver: `run-jit-tests.sh [--int-gate] [--bench] [--disasm|--disasm-record] /path/to/jsc`.
The driver PROBES jsc for options that are §10 prep preconditions
(`--validateButterflyTagDiscipline`, `--useJSThreadsUnlockHandlerICInFTL`,
`--useSharedGCHeap`) and skips the dependent runs with a loud SKIP line when a
precondition has not been applied to the shared tree yet (see
docs/threads/INTEGRATE-jit.md, Task 13 section, for the full gate matrix and
budget-miss consequences — notably the §4.3 LLInt-cache revival trigger).
