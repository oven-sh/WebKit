# JSTests/threads/vmstate — SPEC-vmstate W2/W3 JS stress (task 9)

JS-level tests for the shared-VM-state workstream: W2 structure-allocation
locking (I8/I9) and W3 per-thread VMLite carriers (I11/I13/I14). The W1
shared-atom-table unit tests are C++ (raw `WTF::Thread`, no JS glue) and live
in `Tools/TestWebKitAPI/Tests/WTF/SharedAtomStringTable.cpp` (content carried
in `docs/threads/INTEGRATE-vmstate.md` under M14 until registered).

Runner pickup: SPEC-api's `JSTests/threads/**` globs cover this directory
(SPEC-vmstate §9 N6, verify-only). `resources/` is not a test directory.

Ownership provenance: this directory is an explicit owned glob of the vmstate
workstream — `docs/threads/SPEC-vmstate.md` IS in tree; its §9 "Owned paths
and manifest" (the "Writable:" list starting at the `## 9.` heading, ~line
615) lists `JSTests/threads/vmstate/**` verbatim in the Writable set
(alongside `Source/WTF/wtf/text/**` and the `runtime/VMLite*` files).
Run-config summaries that abbreviate the owned-path list omit it; the frozen
spec is the grant of record. Reviewers: verify with
`grep -n 'JSTests/threads/vmstate' docs/threads/SPEC-vmstate.md` before
re-filing an ownership finding against this directory.

## Files

| File | Spec hook | Flags directive |
|---|---|---|
| `flags-off-baseline.js` | I4/R3/I13 baseline digest | none (default options) |
| `vmlite-single-thread-identity.js` | I13 (`useVMLite=1`, 1 thread), I14 via M13 assert | `--useVMLite=1` |
| `all-flags-identity.js` | I13/I14, §6.4.4 install/restore, N8 teardown | `--useJSThreads=1` |
| `structure-churn-threads.js` | I8/I9 (fresh-shape churn, N threads) | `--useJSThreads=1` |
| `structure-churn-dictionary.js` | I8/I9 (delete/proto/seal/freeze/array transitions) | `--useJSThreads=1` |
| `structure-lock-single-thread.js` | I8 never-nest, I10 (lock flag alone) | `--useStructureAllocationLock=1` |
| `exception-state-per-thread.js` | I15 / Group 2 | `--useJSThreads=1` |
| `stack-limits-per-thread.js` | Group 3 / §6.1.3 JSLock hand-off | `--useJSThreads=1` |
| `regexp-churn-threads.js` | Group 4 (lazy regexp state) | `--useJSThreads=1` |
| `microtask-ordering.js` | Group 6 / §6.5 Phase A non-reroute (I11 contract) | `--useJSThreads=1` |
| `resources/workload.js` | shared deterministic digest workload | (helper, not a test) |

All three identity files assert the SAME hard-coded digest
(`VMSTATE_WORKLOAD_EXPECTED_DIGEST`); a divergence in any flag configuration
is an I13/I4 violation by construction.

## Flag matrix (SPEC-vmstate §3/§10)

Options: `useSharedAtomStringTable` (A), `useVMLite` (V),
`useStructureAllocationLock` (S); `--useJSThreads=1` forces A=V=S=1 (M_opts2,
R2). `--useThreadGIL` defaults true (phase-1 GIL stays on; the build phase
verifies semantics GIL-on before going GIL-off). NOTE: INTEGRATE-api 9.2-1
proposes deleting the `useThreadGIL` option in favor of
`jsThreadGILTimeSliceMs`; if that lands, this row's spelling and the M4
backstop assert change per INTEGRATE-vmstate cross-WS item 16.

| Row | Flags | What runs | Gate |
|---|---|---|---|
| 1 | A=V=S=0 (default) | `flags-off-baseline.js` + the FULL pre-existing JSTests suite | behavior identical; bench in-noise; golden disasm modulo R3(a)-(d) (task 10 / INTEGRATE M8.5) |
| 2 | V=1 only, single-threaded | `vmlite-single-thread-identity.js` + full JSTests smoke | I13: behavior identical, bench in-noise |
| 3 | S=1 only, single-threaded | `structure-lock-single-thread.js` | I8 counter never trips (never-nest), I10 |
| 4 | A=1 only | WTF unit tests (M14) are the primary gate; any JS file may be re-run with the flag added | I1-I6, I17, I19 |
| 5 | `useJSThreads=1` (A=V=S=1, GIL on) | everything in this directory + `JSTests/threads/**` | I8/I9/I11/I13/I14/I15 |
| 6 | Row 5 under TSAN, no-JIT config | `JSTests/threads/**` incl. this dir | §10 TSAN gate (binds I19 too) |
| 7 | Row 5 under the race amplifier (randomized yields) | atomize/drop churn (I1/I3/I6), static-atom churn (I19), structure churn (I8/I9, ≥1 `USE(MIMALLOC)` config — M9 path) | §10 |

Availability note (§3 R4): rows 2-7 reference options that ship via
INTEGRATE-vmstate's M_opts hunk (orchestrator pre-applied per R4). In a tree
without M_opts, only row 1 and the `--useJSThreads=1` files whose option
already exists can start; the rest fail at option parse — that is expected
until integration, not a test bug.

Debug builds add the assert layer: I14 (`VMEntryScope`, M13), I15 (exception
setters, M6.7/M6.11), I18/I20 (`VMLite.cpp` setCurrent/destructor), I8
(`RELEASE_ASSERT` in the locker ctor — active in release too), I17
(`RELEASE_ASSERT` at thread death — release too).
