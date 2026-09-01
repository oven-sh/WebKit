# SCAN-RESULTS — thread-scanners verification, 2026-06-15

Verifier: solo post-fix verification agent. Binary of record:
`WebKitBuild/Debug/bin/jsc` (ASAN-instrumented, mtime 2026-06-15 18:55:19;
`find Source -newer` returned nothing — current against the working tree at
the time every measurement below was taken). `ninja -C Debug jsc`: no work
to do.

**Tree-stability caveat (load-bearing for everything below):** the
orchestrator's "you run ALONE" precondition was empirically false during
this verification window. Eight source files were modified at
18:53:20-18:54:55Z (Structure.cpp, JSObject.h, then a batch of
UnlinkedFunctionExecutable.cpp / BytecodeGenerator.cpp / DFGOSRExit.cpp /
DFGPlan.cpp / CodeCache.cpp / ScriptExecutable.cpp at the same second) and
the Debug binary spontaneously relinked at 18:55:19 — neither action by this
agent. A `Tools/threads/run-tests.sh --cve` runner not launched here (PID
3153473, PPID 3153450, started 19:06:24) was also live and contending for
the machine through the validation re-run. An initial corpus pass against
the 18:40 binary (the first "current" binary this agent saw) showed
`jit/ftl-osr-entry-catch-loop-amplifier.js` failing 5/5 deterministic with
`ExceptionScope::assertNoException()`; on the 18:55 binary the same test
passes 5/5 and the 00-baseline sweep is 94/0/4. The 18:40 state therefore
appears to have been a transient mid-fix tree that the second-round 18:54
edits corrected. **All results below are against the 18:55 binary**, which
is the current tree state.

## Corpus regression gate (Debug, run-tests.sh)

| Mode | Result | Evidence |
|---|---|---|
| GIL-off (pinned env) | **94 / 0 / 4** | `Tools/threads/scan/jsc-validation/00-baseline.log` (re-run, 19:01). Matches r0 pre-fix baseline; no regression. |
| GIL-on (no env) | **96 / 0 / 2** | `/tmp/corpus-gilon.log`. Clean. |

(The 93/1/4 GIL-off observed against the 18:40 binary —
`ftl-osr-entry-catch-loop-amplifier.js` 5/5 abort — does NOT reproduce on
the 18:55 binary; see tree-stability caveat.)

---

## Per-scanner re-run

### 1. TSAN (tsan-deep, CLoop no-JIT) — build break repaired; r1 re-run staged

**[SCAN-TSAN-REVERIFY, 2026-06-18]** The build break below is resolved:
`icConcurrentRelaxed{Load,Store}` are now declared UNCONDITIONALLY at the top
of `bytecode/PropertyInlineCache.h:46-66`, outside the `#if ENABLE(JIT)` guard
(the helpers are generic relaxed-load/store templates with no JIT dependency).
`CodeBlock.cpp:80` includes the header unconditionally and the
`InterpreterThunk` arm at `CodeBlock.cpp:1569-1570` compiles in the
ENABLE_JIT=OFF / ENABLE_C_LOOP=ON config. Mtime evidence: the TSan binary
relinked at 19:18 (33s after the header edit), confirming the lift compiles;
the binary is since stale against later edits and must be rebuilt for r1.

The r0 logs are archived (`gil-{off,on}-r0/`, `families-gil-{off,on}-r0.txt`)
and a one-shot r1 driver is staged at
`Tools/threads/scan/tsan-deep/r1-reverify.sh` (runs `run.sh both`, extracts
families via `Tools/threads/tsan/dedup.py`, gates via `compare-families.py`:
r1 family count ≤ r0 with zero new keys, per mode). **Builder step:**
`ninja -C WebKitBuild/TSan jsc && Tools/threads/scan/tsan-deep/r1-reverify.sh`.

The `WTF::WeakRandom::advance` family (268 GIL-off / 263 GIL-on r0) is now
addressed in code (relaxed-atomic conversion, see ACCEPTED below) rather than
suppressed; expected r1 outcome is ≤13/≤11 families.

---

*Original r0 record preserved below for the audit trail:*

Re-verification was attempted (`ninja -C WebKitBuild/TSan jsc`) and **FAILS
to compile**:

```
Source/JavaScriptCore/bytecode/CodeBlock.cpp:1569:46: error: use of
undeclared identifier 'icConcurrentRelaxedLoad'
Source/JavaScriptCore/bytecode/CodeBlock.cpp:1570:46: error: use of
undeclared identifier 'icConcurrentRelaxedLoad'
```

The fix for the `propagateTransitions` TSAN family (tsan-deep r0 family
"auto void JSC::CodeBlock::propagateTransitions" / `llint_slow_path_put_by_id`,
2+1 reports) calls `icConcurrentRelaxedLoad()`, which is declared inside the
`#if ENABLE(JIT)` block of `bytecode/PropertyInlineCache.h` (line 164,
inside the 45..290 guard). The call site is the `JITType::InterpreterThunk`
arm of `propagateTransitions` — interpreter code that is compiled in the
ENABLE_JIT=OFF / ENABLE_C_LOOP=ON TSAN configuration. **The fix broke the
TSAN target.** No spot-scope re-run of any tsan-deep family is possible
until this is repaired (either move `icConcurrentRelaxedLoad` outside the
JIT guard, or guard the call site).

**r0 families (14 GIL-off / 12 GIL-on, 1813 / 1932 reports) — status
unknown, cannot re-verify.** The fix-round touched
Heap.cpp / IsoCellSet.cpp / MarkedBlockInlines.h / DeferredWorkTimer.cpp /
CodeBlock.cpp, which map to the r0 families
`prepareForConservativeScan`/`addCoreConstraints` (1148/1247 reports),
`specializedSweep` (321/354), `BitSet<1024>::get`/`concurrentFilter`
(58+4+4+2/50+5+6+1), `IsoCellSet::sweepToFreeList` (1/3),
`DeferredWorkTimer addPendingWork`/`runRunLoop` (1/1), and
`propagateTransitions` (2+1/0). Whether those fixes are correct is
unverifiable here; the propagateTransitions one is provably broken for this
config.

### 2. UBSAN — not re-run (binary stale, no threads-scope fixes to verify dynamically)

The UBSan binary (`WebKitBuild/UBSan/bin/jsc`, mtime 17:34) predates the fix
round; rebuild was not attempted in this window (would compile cleanly —
JIT=ON config is unaffected by the CodeBlock.cpp break — but was deferred in
favor of the validation re-run, which uses the already-current Debug
binary).

r0 produced **54 unique threads-scope reports**
(`Tools/threads/scan/ubsan/reports-scope-r0.txt`):

- **53 × `misaligned-pointer-use` in `BytecodeStructs.h` (DerivedSources)**:
  unaligned `uint16_t` loads in the generated `Op*::Op*(unsigned short
  const*)` decoders, reached via `CodeBlock::finishCreation`. Upstream
  bytecode-stream layout is byte-packed by design; the generated decoders
  read it as `uint16_t`/`uint32_t` regardless of alignment. **Pre-existing
  upstream behavior, present with `--useJSThreads=false`, not introduced by
  the threads work.** Accepted-with-rationale (see ACCEPTED below).
- **1 × `signed-integer-overflow` at
  `bytecode/UnlinkedFunctionExecutable.cpp:275:65`** — the only
  threads-scope UBSAN finding that is a real candidate.
  `UnlinkedFunctionExecutable.cpp` was modified post-scan (mtime 18:54:55).
  **Not dynamically re-verified** (UBSan binary stale). Recorded as RESIDUAL
  pending a UBSan rebuild + spot run.

The remaining ~50k raw UBSAN reports (`summary-counts.txt`) are in
`bmalloc/libpas/**`, `wtf/Vector.h`, and `wtf/text/FastCharacterComparison.h`
— all out of threads scope and pre-existing upstream (libpas deliberately
uses misaligned packed structures).

### 3. clang-tidy + clang --analyze — spot re-run

Spot re-run on the two threads-surface files that were modified post-scan:

| r0 finding | r1 spot-check | Status |
|---|---|---|
| `JSLock.cpp:1781:12 Called C++ object pointer is null [clang-analyzer-core.CallAndMessage]` | `clang-tidy -p ccdb --checks='-*,clang-analyzer-core.CallAndMessage' JSLock.cpp` → **no warning** | **FIXED** |
| `Structure.cpp:2422:102 repeated branch body [bugprone-branch-clone]` | warning present at **:2504:102** (line moved) | **NOT FIXED** (residual, low-severity) |

All other r0 clang findings are accepted-with-rationale (see ACCEPTED
below) — they were not in the fix scope and the spot re-run confirms no
modified-file regression in the checked checks.

### 4. semgrep / CodeQL — not re-run (r0 was 0 confirmed findings)

r0 (`Tools/threads/scan/codeql-semgrep/TRIAGE.md`): CodeQL skipped (not
quickly obtainable; full traced compile required), semgrep registry packs 0
hits, semgrep custom concurrency rules 33 hits → 33 false-positives (all
under brace-init `Locker { }` or documented-safe patterns). Nothing to
re-verify.

### 5. JSC validation modes (`Tools/threads/scan/jsc-validation/sweep.sh`) — full re-run

Re-run sweeps 00/01/04/05/06 against the 18:55 Debug binary; 02/03 were
clean in r0 and were not re-run (carried as PASS). Contention note: a
foreign `--cve` runner shared the machine through sweeps 04-06; result
counts were stable on cross-check.

| Sweep | r0 (pre-fix) | r1 (post-fix) | Delta |
|---|---|---|---|
| 00-baseline (no validation) | 94/0/4 | **94/0/4** | unchanged |
| 01-graph-bce (`validateDFGClobberize` + graph/BCE, no concurrent JIT, gcAtEnd) | 89/5/4 | **86/8/4** | **NOT FIXED**; +3 noise (2 known flakes + 1 new flake) |
| 02-verifygc | 94/0/4 | (carried) | clean |
| 03-butterfly | 94/0/4 | (carried) | clean |
| 04-butterfly-swbit (`forceButterflySWBit` + verify, gcAtEnd) | 92/2/4 | **93/1/4** | **1 FIXED** (`heap-client-churn.js` deferralContext assert); 1 residual |
| 05-bytecode-exc (`validateExceptionChecks` + bytecode/freelist) | ~44/50/4 | **44/50/4** | **NOT FIXED** (same site set, identical counts ±1) |
| 06-zombie-scribble (`useZombieMode` + scribble + freelist, gcAtEnd) | 93/1/4 | **93/1/4** | unchanged (same residual) |

---

## FIXED (verified this run)

1. **clang-analyzer-core.CallAndMessage `JSLock.cpp:1781`** — the null-ptr
   path no longer warns under `clang-tidy -p ccdb
   --checks=clang-analyzer-core.CallAndMessage`. (JSLock.cpp modified
   post-scan.)
2. **jsc-validation 04 `heap-client-churn.js`** — the
   `ASSERTION FAILED: deferralContext || isDeferred() ||
   !AssertNoGC::isInEffectOnCurrentThread()` at `Heap.cpp:4272
   collectIfNecessaryOrDefer` under `forceButterflySWBit + gcAtEnd` no
   longer fires. (Heap.cpp / JSObject.h modified post-scan.) Sweep 04 is
   now 93/1/4, the surviving fail being the `gcAtEnd` MachineStackMarker
   SEGV (residual #2 below), not this finding.

(Unverified-probable: TSAN-deep families targeted by Heap.cpp /
IsoCellSet.cpp / MarkedBlockInlines.h / DeferredWorkTimer.cpp edits, and the
UBSAN `UnlinkedFunctionExecutable.cpp:275` overflow — source touched, but
the respective sanitizer binaries could not be re-exercised; see RESIDUALS.)

---

## RESIDUALS (still failing / open after the fix round)

1. **TSAN target build break** — *RESOLVED (SCAN-TSAN-REVERIFY).*
   `icConcurrentRelaxed{Load,Store}` lifted outside the `#if ENABLE(JIT)`
   guard in `PropertyInlineCache.h:46-66`; `ninja -C WebKitBuild/TSan jsc`
   compiles (binary relinked 19:18 post-lift). r1 re-run staged via
   `Tools/threads/scan/tsan-deep/r1-reverify.sh`; gate = r1 families ≤ r0
   with zero new keys. **tsan-deep re-verification unblocked; pending
   builder execution.**

2. **`gcAtEnd` × `property-wait-termination.js` MachineStackMarker SEGV** —
   present unchanged in sweeps 01b/04/06 (every sweep that sets
   `JSC_gcAtEnd=true`). ASAN SEGV at `MachineStackMarker.cpp:128
   copyMemory` via `tryCopyCooperativelyParkedThreadStack` ->
   `gatherStackRoots` during the end-of-process forced GC, while a JS
   Thread is parked in a watchdog-terminated `Atomics.wait`. The
   cooperatively-parked thread's recorded stack range is stale/past-end at
   shutdown. Reproduces on `jsc-v33` (pre-scan binary) — **pre-existing,
   not introduced by this round**, but in threads scope (the cooperative
   parked-stack copy is threads code). r0 evidence:
   `repro-gcAtEnd-property-wait-termination.log`.

3. **`validateDFGClobberize` SIGTRAP family (sweep 01)** — 5-6 tests
   (`races/counter-lock`, `objectmodel/i03-i37-same-shape-add-storm`,
   `vmstate/regexp-churn-threads`, `jit/spawned-thread-butterfly-stress`,
   `jit/tid-tag-3-threads`, plus a flaky `api/lock-basic`) die with exit
   133 (SIGTRAP, no diagnostic) under
   `validateDFGClobberize=true useConcurrentJIT=false`. r0's
   `01b-graph-bce-noclobberize.log` (same sweep minus
   `validateDFGClobberize`) had only the residual-#2 fail, so the SIGTRAPs
   are the clobberize validator. **NOT FIXED by the DFGPlan.cpp /
   DFGOSRExit.cpp edits.** A threads-introduced DFG node's
   clobberize/def-use is mis-declared (likely a `JSThreadsSafepoint` /
   `CheckTraps` adjacent node — sweep 01's victims are exactly the
   long-loop / heavy-tier-up tests).

4. **`validateExceptionChecks` unchecked-exception sites (sweep 05)** —
   50 fails, 86 raw reports, **same site set as r0**:
   - `VMTraps.cpp:686 handleTraps` — throws (84) AND is the unchecked
     scope (77). The trap-handler's own scope discipline; reachable from
     every threads test that takes a safepoint. Dominant family.
   - `ThreadAtomics.cpp:1425 atomicsWaitOnProperty` — unchecked (4).
   - `ConditionObject.cpp:103 conditionProtoFuncWait` — unchecked (2).
   - `ThreadManager.cpp:1124 threadRestrictCheck` — throws (1).
   - `JSObject.cpp:4283 setPrototypeWithCycleCheck` — unchecked (1; was
     :4245 in r0, line moved by an unrelated edit).
   - `CallData.cpp:76 call` — unchecked (1).
   None of these source files were in the post-scan edit set; **not
   addressed by this fix round.** All five named sites are threads-scope
   (ThreadAtomics/ConditionObject/ThreadManager) or threads-reachable on a
   path the threads corpus exercises (VMTraps via safepoint, JSObject via
   the proto-cycle-race test).

5. **`atomics/property-cas-delete-undefined-sentinel-u5.js` flaky SEGV** —
   GIL-off Debug, ~5-7% rate (current binary 2/40, `jsc-v33` pre-fix 2/40).
   ASAN SEGV @ `0x0000975afdde` (same low address every time) in
   `cellHeaderConcurrentLoad<JSType>` -> `JSCell::type()` ->
   `JSObject::getOwnNonIndexPropertySlot` (JSObject.h:2093) ->
   `getOwnPropertySlotImpl` -> `probeOwnPropertyForAtomicsConcurrent`
   (ThreadAtomics.cpp:319). A stale/torn `Structure*` is being
   dereferenced inside the lock-free Atomics property probe.
   **Pre-existing** (reproduces on the pre-fix `jsc-v33` binary at the
   same rate), so not a fix-round regression — but it is a real
   threads-scope race in `ThreadAtomics.cpp` that the r0 single-pass
   baseline missed by chance (94/0/4). New finding of this verification
   run; not in any prior scan record.

6. **`Structure.cpp` bugprone-branch-clone** — the r0 :2422 finding is
   still present at :2504 (lines shifted). Low-severity tidy warning; not
   addressed.

7. **UBSAN `UnlinkedFunctionExecutable.cpp:275` signed-integer-overflow** —
   *FIX LANDED (SCAN-UBSAN-UFE-OVERFLOW), pending builder verify.* The
   18:54:55 post-scan mtime bump was a no-op (file byte-identical to
   43fd5fb), so the r0 report was still live. Root cause: `oneBasedInt()`
   returns `int` and `m_firstLineOffset` is `unsigned : 31`, which
   integral-promotes to `int` (all values representable), giving `int +
   int`; the r0 trigger was `1 + 2147483647`. Fix: cast the LHS to
   `unsigned` so the addition is unsigned (result was already stored to
   `unsigned`). The adjacent `:274` `startOffset()` line has the identical
   `int + (unsigned:31 → int)` shape (`UnlinkedSourceCode.h:266` returns
   `int`); same cast applied pre-emptively so the gate cannot resurface one
   line up. No flag-off behavior change — defined wraparound replaces UB
   that compiled to the same wraparound. The `2147483647` operand traces to
   `:176` (negative `node->firstLine() - parentSource.firstLine()`
   truncating into the 31-bit field); pre-existing upstream behavior, not
   threads-introduced, left unchanged. **Gate (builder):** `ninja -C
   WebKitBuild/UBSan jsc` then spot-run `JSTests/threads/arrays/` (and
   `semantics/private-fields-shared.js`, the r0 trigger) under UBSan; grep
   the output for `UnlinkedFunctionExecutable.cpp:` — no site (any line)
   may appear.

---

## ACCEPTED with rationale (no fix intended)

- **UBSAN `misaligned-pointer-use` in DerivedSources/BytecodeStructs.h
  (53 sites)** — upstream-generated bytecode decoders reading the
  byte-packed instruction stream as wider integers. Present with
  `--useJSThreads=false`; not threads-introduced; would require a
  bytecode-generator change upstream to address. Out of threads scope.
- **UBSAN libpas/bmalloc `misaligned-pointer-use` (the ~50k-report tail)**
  — libpas's compact-pointer representation deliberately packs pointers at
  4-byte alignment. Upstream design; out of threads scope.
- **UBSAN `wtf/Vector.h:122` / `StdLibExtras.h:1135`
  `invalid-null-argument`** — upstream `memcpy(nullptr, nullptr, 0)`
  pattern in `Vector`'s zero-length copy path. Pre-existing, out of scope.
- **clang-tidy `bugprone-bitwise-pointer-cast` (every `std::bit_cast`
  pointer-to-pointer)** — WebKit-wide deliberate idiom (replaces
  `reinterpret_cast` per upstream guidance); cosmetic.
- **clang-tidy `bugprone-multi-level-implicit-pointer-conversion`
  (AssemblyHelpers.cpp)** — JIT assembler `storePtr(void**, ...)` calling
  convention; cosmetic.
- **clang-tidy `clang-analyzer-deadcode.DeadStores` for `auto scope =
  DECLARE_THROW_SCOPE(vm)` in `ConcurrentButterflyOperations.cpp`
  (281/295/310/324/339/353/368)** — the scope object's lifetime IS the
  effect (RAII); the analyzer mis-reads RAII as a dead store. False
  positive.
- **clang --analyze `alpha.core.CastToStruct` in `ConcurrentButterfly.cpp`
  (929-930, 1451-1453, 1667-1669, 3918-3919, 3943, 3986)** — the
  butterfly/spine layout IS a non-struct type cast to a header struct; that
  is the design (same shape as `Butterfly*` upstream). Alpha checker.
- **clang --analyze `alpha.core.PointerArithm` in
  DerivedSources/BytecodeStructs.h (10027+)** — generated bytecode-decode
  pointer arithmetic; same rationale as the UBSAN misaligned class.
- **clang --analyze `alpha.cplusplus.IteratorRange` in `VMLite.cpp`
  (1024/1036), `AtomStringImpl.cpp` (759/815/861), `CodeBlock.cpp:249`** —
  alpha checker false positives on `HashMap::find`/`end()` comparison
  idioms (the analyzer cannot model the bucket-iterator invariant). Spot
  source-read confirms each site guards `it != end()` before deref.
- **clang-tidy `clang-analyzer-cplusplus.NewDeleteLeaks` for
  `NeverDestroyed` / `ThreadSpecific` / `HandleSet`** — intentional
  process-lifetime leaks; upstream pattern.
- **clang-tidy `bugprone-unused-return-value`
  `RetiredJITArtifacts.cpp:269`, `JSLock.cpp:285`, `LockObject.cpp:114`** —
  each site is a documented fire-and-forget (epoch-retire, lock-adopt
  return ignored, futex-wake count ignored). Low value.
- **semgrep custom concurrency rules (33 hits)** — every hit triaged FP in
  `Tools/threads/scan/codeql-semgrep/TRIAGE.md` (brace-init `Locker {}`
  not matched by the rule's `pattern-not-inside`; relaxed stores of
  process-lifetime / non-deref'd pointers; comment-text matches).
- **CodeQL** — not run; documented in `CODEQL-SKIPPED.md` (not quickly
  obtainable; requires a full traced compile).
- **TSAN `WTF::WeakRandom::advance` family (268+263 reports r0)** — VM's
  shared `WeakRandom` is read-modify-written from multiple Threads at
  safepoint-jitter sites. The value is non-semantic (jitter only); a torn
  state degrades to a different random seed. Accepted per the standing
  TSAN ruling for "racy words whose value is non-semantic" (TSAN-TRIAGE
  §15 relaxed-atomic class). **Landed (SCAN-TSAN-REVERIFY):** the
  relaxed-atomic conversion (NOT a suppression — the interleaving is
  threads-introduced, so suppressions.txt rule 1 disqualifies it).
  `WeakRandom::advance()` and `setSeed()` now access `m_low`/`m_high` via
  `__atomic_{load,store}_n(..., __ATOMIC_RELAXED)` over the existing plain
  `uint64_t` storage (`Source/WTF/wtf/WeakRandom.h:50-61,129-152`).
  Single-word relaxed atomics compile to the identical mov/ldr/str the
  plain accesses did (flag-off byte-identical preserved; same shape as the
  `icConcurrentRelaxed{Load,Store}` precedent). `m_low`/`m_high` remain
  plain `uint64_t` so `lowOffset()`/`highOffset()` and the manually-inlined
  JIT codegen are unchanged; the JIT path is not compiled in the
  ENABLE_JIT=OFF TSAN config that observes this family.

---

## Summary

| Scanner | r0 findings | Fixed (verified) | Residual | Accepted | Re-run status |
|---|---|---|---|---|---|
| TSAN (tsan-deep) | 14 fam GIL-off / 12 GIL-on | 0 verified | build break **resolved**; r0 families pending r1 | WeakRandom (1 fam, **landed** as relaxed-atomic) | **r1 staged** (`r1-reverify.sh`; pending builder) |
| UBSAN | 54 threads-scope | 0 verified | 1 (UnlinkedFunctionExecutable overflow — **fix landed**, pending rebuild+spot-run) | 53 (BytecodeStructs misaligned) | not re-run (binary stale) |
| clang-tidy/analyze | ~80 lines | **1** (JSLock.cpp:1781 null-ptr) | 1 (Structure.cpp branch-clone) | rest (idioms/alpha/RAII) | spot re-run |
| semgrep | 33 | — | 0 | 33 (all FP) | not re-run (0 confirmed) |
| CodeQL | — | — | — | skipped | skipped |
| jsc-validation 01 (clobberize) | 5 SIGTRAP | 0 | **5-6 SIGTRAP** | — | re-run |
| jsc-validation 04 (SW-bit) | 2 | **1** (heap-client-churn deferral) | 1 (gcAtEnd MachineStackMarker) | — | re-run |
| jsc-validation 05 (exc-checks) | ~50 / 6 sites | 0 | **~50 / 6 sites** | — | re-run |
| jsc-validation 06 (zombie) | 1 | 0 | 1 (= residual #2) | — | re-run |

**Honest verdict:** the fix round verifiably closed **two** scanner findings
(JSLock.cpp null-ptr, heap-client-churn deferral assert), introduced **one
build break** (TSAN/CLoop config via the CodeBlock.cpp propagateTransitions
hunk), and left the three substantive jsc-validation families
(validateDFGClobberize SIGTRAP, validateExceptionChecks site set, gcAtEnd
parked-stack SEGV) unchanged. TSAN-deep cannot be re-verified until the
build break is repaired. One **new** pre-existing flake
(`property-cas-delete-undefined-sentinel-u5.js`, ThreadAtomics
lock-free-probe stale-Structure SEGV, ~6%) was surfaced by this run's
repeat-sampling that r0's single-pass baseline missed.

Raw evidence: `Tools/threads/scan/jsc-validation/{00-baseline,01-graph-bce,
04-butterfly-swbit,05-bytecode-exc,06-zombie-scribble}.log` (r1 timestamps
19:01-19:13Z), `Tools/threads/scan/tsan-deep/` (r0 only),
`Tools/threads/scan/ubsan/reports-scope-r0.txt`,
`Tools/threads/scan/clang-analyzer/SUMMARY.txt`,
`Tools/threads/scan/codeql-semgrep/TRIAGE.md`.
