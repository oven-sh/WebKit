# EVIDENCE PACK — W>=16 scalebench crash family (GIL-off, shared heap)

Date: 2026-06-11. Hunt target: the SCALEBENCH run-2 §11.3 P0 — the crash
family that fails every js big-program cell at W>=16 (and 1/5 at W=2, 3/5 at
W=4/8), exit mix 133 (SIGTRAP, libpas `pas_deallocation_did_fail`) / 134
(SIGABRT) / 139 (SIGSEGV) plus one logged type error from a corrupted posting
list. Binary: `WebKitBuild/Release/bin/jsc`
(sha256 b5c3a009d514..., 2026-06-10 17:02 — includes BOTH landed fixes:
GC under-marking 25375a997f4f and STW watchdog 6b298a4fdd99). ASAN runs used
`WebKitBuild/Debug/bin/jsc` (ASAN-instrumented). Host: 64-core Linux.
Previous pack (STW watchdog) archived in `prev-stw-watchdog-20260610/`.

Pinned GIL-off flags everywhere below ("PINNED"):
`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
--useSharedGCHeap=1 --useThreadGILOffUnsafe=1`

Artifacts referenced: `Tools/threads/bughunt/evidence-w16/` (campaign logs,
core backtraces, ASAN reports, rr traces, the no-spread bench variant),
`Tools/threads/bughunt/repro.js` (minimal repro),
`Tools/threads/scalebench/out/p0-cores/` (the four run-2 cores; gitignored).

---

## 0. Bottom line (what the data forces)

ONE mechanism with three loud faces and one silent face. Every piece of
evidence converges on the LLInt `op_call_varargs` slow-path pair racing the
**shared-VM scratch slots** `vm.newCallFrameReturnValue` and
`vm.varargsLength`:

- `llint_slow_path_size_frame_for_varargs` computes this thread's callee
  frame and STORES it: `Source/JavaScriptCore/llint/LLIntSlowPaths.cpp:2429-2430`
  (`vm.varargsLength = length; vm.newCallFrameReturnValue = calleeFrame;`).
- the LLInt asm returns to `doCallVarargs`
  (`llint/LowLevelInterpreter64.asm:3010-3025`: exception check,
  `move r1, sp`), then makes a SECOND slow call;
- `varargsSetup` RE-READS both slots from memory:
  `LLIntSlowPaths.cpp:2453-2454` (`CallFrame* calleeFrame =
  vm.newCallFrameReturnValue; ... vm.varargsLength ...`), and
  `setupVarargsFrame` then memcpy's the spread arguments through that pointer.
- `vm` here is `codeBlock->vm()` (`LLINT_BEGIN_NO_SET_PC`,
  LLIntSlowPaths.cpp:81-87) — the ONE shared VM block for all GIL-off
  threads, and per VM.h:604-606 "VM doubles as the MAIN thread's physical
  VMLitePrimitives": these raw member accesses hit the MAIN thread's lite no
  matter which thread executes the slow path. The two fields are declared
  per-lite in the SPEC-vmstate Group-2 X-macro (`runtime/VMLite.h:118` and
  `:122`), and the per-lite ROUTING MACHINERY EXISTS and is used for sibling
  fields (`VM::group3Primitives()` VM.h:708-716 and
  `exceptionScopeVerificationState()` VM.h:744-752 both route through
  `VMLite::currentIfExists()` when GIL-off) — but the two varargs sites
  access the raw members, bypassing it. Grep confirms the only
  writers/readers of `newCallFrameReturnValue` are these two slow paths
  (`SamplingProfiler.h:146` is a comment).

If thread B's store lands in the window between thread A's store and A's
re-read, A copies its arguments through **B's callee-frame pointer** (a
pointer into B's thread stack) and/or with **B's length**:
- A scribbles boxed JSValues onto B's live stack → B later crashes or
  computes garbage (victim face);
- A then executes the callee on a frame it never wrote / on B's stack →
  garbage callee/arguments in A (thief face);
- garbage JSValues flow into `StringImpl::deref` / `toPrimitive` → the libpas
  SIGTRAP and null-structure faces;
- when the foreign frame is still plausible, the run COMPLETES with a wrong
  string → the silent posting-list corruption.

Baseline JIT (`jit/JITCall.cpp:64-100` `compileSetupFrame`) and DFG
(`dfg/DFGSpeculativeJIT64.cpp:771`) keep the size and frame in REGISTERS —
those tiers are immune. That is why the family is rare under full tiering
(only code still executing `op_call_varargs` in LLInt is exposed: warmup,
cold re-entries) and why exposure scales with W x work.

NOT claimed: why `--useSharedGCHeap=0` makes it vanish is not directly
proven (see §6, F6) — the slot race itself is flag-independent code, so the
plausible reading is that sharedGCHeap=0 changes the sharing topology (each
Thread no longer runs lites of one shared VM); that reading is inference,
the 0-crash measurements are fact.

---

## 1. Reproduction + campaign numbers

### 1.1 Recorded run-2 matrix (full size, PINNED, from runs.jsonl/§11.3)
W=1: 6/6 OK; W=2: 5 OK + 1x134; W=4: 2 OK + 2x133+1x134+1x139;
W=8: 3 OK + 3x133; W=16: 1 OK + 3x133+2x134; W=32: 3x133+3x134;
W=48: 6x133; W=64: 3x133+2x134+1x139.

### 1.2 This hunt, same binary (campaign logs in evidence-w16/)
| config | result |
|---|---|
| bench full W=16, PINNED, x5 | 4 OK + 1x139 — the 139 was the §5 STARTUP face (died at 0.023 s, empty output), not the varargs face |
| bench smoke W=16, PINNED, x40 | 38 OK + 1x139 + 1x135, BOTH instant startup-face (~0.023 s) |
| bench smoke W=4, PINNED + `--useJIT=0`, x10 | **0/10 survive** (7x139, 3x134) |
| bench smoke W=4, PINNED + `--useJIT=0`, spread call replaced (§1.3), x10 | **10/10 OK, all checksumA=f9f349a1fc92a0d0 (correct)** |
| minimal repro (§2), PINNED + `--useJIT=0`, W=2/3/4/8/16, x3 each | **15/15 crash** (mix of 139/134), usually < 10 s; several runs ALSO print decoded silent mismatches before dying |
| minimal repro, PINNED (full tiering), W=16, 2M iters, x6 | 3 crash (139,133,139), 3 complete — one completed run had 1 silent MISMATCH |

Note vs run 2's 0/5 at W>=16 full size: this pass ran cells concurrently
with other campaign load; the recorded matrix ran them back-to-back on a
quiet host. Rate differs, family and faces are identical; the noJIT
amplification (0/10) is the stable handle.

### 1.3 The one-line carve
`evidence-w16/bench-nospread.js` is js/bench.js with ONLY line 216 changed —
`out.push(String.fromCharCode(...codes))` → per-char `fromCharCode` loop +
concat. Result: 0/10 survive → 10/10 survive, identical correct checksums.
The workload's ONLY spread/varargs site is the trigger. (`genDocText`'s
fromCharCode at :173 and `termOf`'s at :137 are single-arg, non-varargs.)

### 1.4 The four preserved cores (gdb against Release jsc; full bt in
`evidence-w16/cores-bt.txt`)
- core.1986681 (139): MAIN thread in
  `JSCellButterfly::copyToArguments` ← `setupVarargsFrame` ←
  `llint_slow_path_call_varargs`, with `newCallFrame=0x7f978297d610` — inside
  SPAWNED thread LWP 1986685's stack (its TLS base 0x7f978297e640).
  Cross-thread callee frame, caught in the act. The spread arguments array
  itself is INTACT (header 0x...01001080, length 4/4, boxed ints
  116,100,99,104).
- core.1986672 (134): spawned thread, same two slow-path frames, garbage
  `callFrame=0x1e5072` → `CodeBlock::constantRegister` Vector bounds
  `CrashOnOverflow::crash` → abort. newCallFrame=0x7ffc6adfe140 = MAIN-thread
  stack range.
- core.2007431 (133, recorded with `--useGC=0`): spawned thread in
  `stringFromCharCode` → `jsString` → `StringImpl::deref(0x55da3c465db4)` →
  libpas "Large heap did not find object" on begin=0x55da3ac4623b — a CODE
  address (inside `jsString` itself) treated as a StringImpl. Garbage
  argument value, allocator is just the crash REPORTER.
- core.2011759 (139, `--useGC=0`): spawned thread in `stringFromCharCode` →
  `toUInt32` → `JSObject::toPrimitive` on 0x7ffefa213600 — a MAIN-thread
  STACK address treated as a JSObject; `callFrame=0x7ffefa2135c0` is itself
  in the main thread's stack. The thief executed the callee on a foreign
  frame.

All four: same opcode, same site, cross-thread frame/argument state. 133 vs
134 vs 139 is only WHICH garbage value is hit first.

---

## 2. Minimal repro + decoded corruption semantics

`Tools/threads/bughunt/repro.js` (~60 lines, no scalebench corpus): W threads
each loop `String.fromCharCode(...codes)` on a fresh thread-local array;
thread t uses only char code 97+t and only length 2+(t%8), so any corrupt
RESULT decodes to the thread whose state leaked in. Run:
```
WebKitBuild/Release/bin/jsc <PINNED> --useJIT=0 \
  -e "globalThis.REPRO_THREADS=16;" Tools/threads/bughunt/repro.js
```
Crashes in seconds at every W>=2; with full tiering it still crashes/corrupts
at W=16 (rate lower — §1.2).

Decoded silent mismatch (evidence-w16/repro-mismatches.txt):
```
MISMATCH thread=0 iter=1719 wantLen=2 gotLen=3 wantCode=97 gotCodes=[97,97,0] srcArrayNow=[97,97]
```
Thread 0 (length 2, all-'a') received a length-THREE string: its own two
arguments plus one never-written slot. gotLen=3 names thread 1 (the only
length-3 thread) as the writer whose `vm.varargsLength` was picked up; the
source array is untouched. So the corruption is **cross-THREAD leakage of
frame-setup state (length and frame pointer) at the instant of the call** —
not cross-property, not stale-by-N-writes, not corruption of the source
array or of any heap object. This is the same semantics as the run-2
"corrupted posting list" type error: a wrong string built from a
wrong-length/wrong-content frame, surviving into shared data.

---

## 3. Machine-state narrowing (each answer carves the space)

Minimal repro, W=4-8, 300k-400k iters, x3-4 per config:

| config | outcome | carve |
|---|---|---|
| PINNED `--useJIT=0` | 100% crash | LLInt path alone suffices |
| PINNED full tiering | crashes/corrupts at W=16/2M iters | family persists in production config |
| PINNED `--useDFGJIT=0` | 3/3 crash | baseline present doesn't save it (LLInt still runs pre-tier-up; high churn keeps slow path hot) |
| PINNED minus sharedGCHeap (`--useSharedGCHeap=0`) | 3/3 clean | matches bench-level 0/20 (§11.3); see F6 |
| PINNED minus atom table (`--useSharedAtomStringTable=0`) | 3/3 crash | shared atom table NOT required |
| GIL ON (`--useThreadGIL=1 --useJIT=0`) | 3/3 clean | GIL-off only |
| PINNED `--useJIT=0 --validateFreeListStructure=1` | crashes continue (139/134/133/132), **0 validator reports** in 4 saved logs | FreeList/allocator structurally clean — kills the TLC/double-handout hypothesis class |
| `--useGC=0` (run-2 standalone, cores 2007431/2011759) | still fires | GC-independent — kills collector/retention hypothesis class |

Tier answer: the read that goes wrong is the **LLInt slow-path re-read** of
`vm.newCallFrameReturnValue`/`vm.varargsLength`; baseline/DFG varargs setup
is register-based (JITCall.cpp:84-99: `operationSizeFrameForVarargs` →
`emitSetVarargsFrame` → `operationSetupVarargsFrame`, all in regs;
DFGSpeculativeJIT64.cpp:771 same). `forceSegmentedButterflies` /
`verifyConcurrentButterfly` were not exercised: the corrupted object is a
CALL FRAME under construction, not a butterfly — the intact spread-source
array in core.1986681 (§1.4) already rules the butterfly out.

ASAN (Debug build, minimal repro, `--useJIT=0`, W=4; reports in
evidence-w16/asan-run{1,3}.out):
- run3: **T0** `llint_slow_path_call_varargs` → `loadVarargs` →
  `copyToArguments` → `__asan_memcpy` — **wild WRITE of
  0xfffe000000000061** (boxed int 97 — T0's own argument) **to
  0x7ff9831fde80**, far outside T0's stack (T0 sp=0x7ffc41756388): the thief
  caught writing through a foreign frame pointer.
- run1: **T1** (JS Thread) crashes inside `stringFromCharCode`
  (StringConstructor.cpp:111) writing ThrowScope state at address 0x40 —
  victim executing on a corrupted frame.
- run2: SIGABRT, no ASAN report (RELEASE_ASSERT face).

ASAN on the full bench at W=16 smoke (Debug+ASAN, full tiering):
run1 completed CLEAN (rc=0) — consistent with the few-percent per-run rate
under full tiering at smoke size (F3/F7); run2 logs in
evidence-w16/asan-bench-w16-run*.log. The precise ASAN evidence is the
minimal-repro wild-write above; the bench-level ASAN campaign is a rate
game, not a different signal.

---

## 4. rr record-replay: CAPTURED, deterministic

rr 5.9.0 works (needs `LD_LIBRARY_PATH=/usr/local/lib` for libcapnp).
`rr record --chaos` on the minimal repro (`--useJIT=0`, W=8) crashes on
essentially every attempt — even under rr's serialization the two-slow-call
window is wide enough.

Recordings kept (replay-verified):
- `Tools/threads/bughunt/evidence-w16/rr-try1` (SIGABRT face)
- `Tools/threads/bughunt/evidence-w16/rr-try2` (SIGSEGV face) — replayed to
  the fault deterministically:
  `JSCellButterfly::copyToArguments(length=9)` on a spawned thread with
  `newCallFrame=0x73a242b2a790` while the actual faulting dest is
  0x55c242b2a7c0 — identical low 32 bits, foreign high bits. (All worker
  threads sit at identical frame offsets within same-sized thread stacks, so
  a foreign thread's calleeFrame naturally shares the low bits — this also
  explains the "mixed-half pointer" look in the run-2 cores.)
Replay: `LD_LIBRARY_PATH=/usr/local/lib rr replay evidence-w16/rr-try2`
(then `continue` lands at the crash; reverse-watch `vm.newCallFrameReturnValue`
from there for the fix round).

---

## 5. Residual unexplained observation (flagged, small)

A distinct INSTANT-startup face: 3 of ~50 Release bench launches under
concurrent load died at ~0.023 s with empty stdout/stderr (rc 139, 139, 135);
this includes the single full-W=16 failure of this pass. Too fast to have
entered Phase A; no core captured (campaigns run with `ulimit -c 0`). Not
classified by this pack; do not conflate it with the varargs family when
verifying a fix (a fixed binary should be re-campaigned with cores enabled
once to classify it).

---

## 6. The facts any hypothesis must explain

- **F1 (site)**: All 4 run-2 cores, every ASAN report, and the rr replay die
  in/under LLInt `op_call_varargs` on the workload's only spread call
  (js/bench.js:216); replacing that one line makes the full bench 10/10
  clean with bit-identical correct checksums (§1.3).
- **F2 (cross-thread frames)**: Crash states contain ANOTHER thread's stack
  pointer as the callee frame — main thread holding a spawned thread's frame
  (core.1986681), spawned thread executing on a main-stack frame
  (core.2011759), ASAN T0 wild-writing its own boxed argument into a foreign
  stack region (§3).
- **F3 (tier gating)**: LLInt-forced = 100% failure at every W>=2; full
  tiering = a few percent at smoke size, certain at full size W>=16.
  The LLInt slow paths round-trip frame pointer + length through shared VM
  memory between two separate C calls (LLIntSlowPaths.cpp:2429-30 vs
  2453-54, via the shared `codeBlock->vm()`); baseline/DFG do it in
  registers and are immune.
- **F4 (decoded semantics)**: The silent face is a foreign `varargsLength`
  (wantLen=2 → gotLen=3 = the unique length-3 thread, payload = own 2 args +
  1 unwritten slot); source arrays and heap objects stay intact.
- **F5 (allocator/GC exonerated)**: Fires with `--useGC=0`; zero
  `validateFreeListStructure` reports across crashing runs; the libpas
  SIGTRAP is `StringImpl::deref` on a CODE address — garbage input to the
  allocator, not allocator state corruption.
- **F6 (config gates)**: Requires GIL-off AND `--useSharedGCHeap=1`
  (repro 3/3 and bench 0/20 clean with it off; GIL-on clean). Shared atom
  table irrelevant. Why sharedGCHeap=0 shields a VM-slot race is the one
  open question a fix author must answer (most economical reading: that
  config stops threads from sharing one VM block at all — verify, don't
  assume).
- **F7 (exposure model)**: Failure probability scales with W x work and with
  time spent in LLInt — reproducing run 2's monotone W=2: 1/6 → W>=16: ~6/6
  shape without any GC or heap-size dependence.

## 7. Repro quick reference

```
# Minimal, near-deterministic (seconds):
WebKitBuild/Release/bin/jsc <PINNED> --useJIT=0 \
  -e "globalThis.REPRO_THREADS=16;" Tools/threads/bughunt/repro.js

# Bench-level, 100%: (cd Tools/threads/scalebench)
SCALEBENCH_SMOKE=1 ../../../WebKitBuild/Release/bin/jsc <PINNED> --useJIT=0 bench.js -- 4 smoke

# Negative controls: same + bench-nospread.js (clean), or --useThreadGIL=1 (clean),
# or --useSharedGCHeap=0 (clean).

# Deterministic replay:
LD_LIBRARY_PATH=/usr/local/lib rr replay Tools/threads/bughunt/evidence-w16/rr-try2
```

---

## 8. Experimenter round 1 results (2026-06-11)

### H1-varargs-shared-vm-scratch-slots (both ids): CONFIRMED, twice over

**(a) rr reverse-watch — causation proven.** Replayed
`evidence-w16/rr-try2` to the deterministic SIGSEGV (Thread 2 = 2962334,
`varargsSetup` → `copyToArguments(length=9)`, reloaded
`newCallFrame=0x73a242b2a790`). At the crash, `&vm == 0x46ae0c400000`,
`&vm.newCallFrameReturnValue == 0x46ae0c400068`, slot value still equal to
the read value. Hardware watchpoint + reverse-continue:
- 1st (most recent) writer of the value Thread 2 read: **Thread 9
  (2962335)**, `llint_slow_path_size_frame_for_varargs`,
  LLIntSlowPaths.cpp:2430 — a DIFFERENT thread.
- Writer before that: **Thread 5 (2962330)**, same line — a third thread.
The crashing reader never appears as the writer of the value it consumed.
Transcript: `evidence-w16/h1-rr-reverse-watch.txt`.

**(b) tid-tag RELEASE_ASSERT — fires in <1 s at every W>=2.** Instrumented
build (`bin/jsc-bughunt-h1tag`, diff kept at
`Tools/threads/bughunt/h1-tidtag-instrumentation.diff`, source reverted and
pristine jsc rebuilt): thread_local copy of (calleeFrame, length) at the
:2429-30 store, equality RELEASE_ASSERT at the :2453-54 reload. Repro
`<PINNED> --useJIT=0` fired within ~1 second at W=2, W=4, W=16; every hit
shows a foreign frame pointer and a foreign per-thread-encoded length
(e.g. W=2: spawned thread stored 0x7f01e7923780, reloaded MAIN-stack
0x7ffcbbb4bc90, len 3→2). Log: `evidence-w16/h1-tidtag-assert-runs.txt`.

Refute conditions: none met (foreign writer found; assert fired immediately).
Fix-shaped confirm (per-lite routing → 15/15 clean + scalebench smoke) is
deferred to the fix round by design.

### H2-libpas-trap-is-downstream-reporter-not-double-free: CONFIRMED

The single exit-133 core (core.2007431) examined against its own mappings:
`pas_try_deallocate_known_large(ptr=0x55da3ac4623b)` — gdb symbolizes the
pointer as `<JSC::jsString(JSC::VM&, WTF::String&&)+187>`, and the only
mapping containing it is the FILE-BACKED jsc binary image
(`0x55da3abfb000-0x55da3c3c1000 /root/.../bin/jsc`), not any anonymous
libpas heap mapping. The "StringImpl" being deref'd (`this=0x55da3c465db4`)
is itself the address of libpas's own rodata string
`"deallocation did fail at %p: %s\n"` — pure garbage input harvested from
the H1 foreign/unpopulated frame. No second free of any live allocation
exists in the core. The other three cores are the same family's other
faces (SIGSEGV in copyToArguments / TypeInfo::isObject, SIGABRT in
setupVarargsFrame), all with cross-half/foreign frame pointers.
Refute condition (begin inside a libpas heap mapping): not met.
Residual H2 obligation for the FIX round: post-fix W>=16 matrix with cores
enabled must show zero exit-133 (and §5 instant-startup face classified
separately).

### H3-sharedGCHeap-gate-is-GIL-reenable-not-heap-effect: CONFIRMED

Static: runtime/Options.cpp:792-794 —
`if (useJSThreads && !useThreadGIL && !(useVMLite && useSharedAtomStringTable
&& useSharedGCHeap)) useThreadGIL = true;`
Dynamic probe (Release jsc, `--dumpOptions=1`):
- `<PINNED minus sharedGCHeap>` → `useThreadGIL=true` (forced), i.e. a
  GIL-ON process. Dump: `evidence-w16/h3-dumpOptions-minus-sharedGCHeap.txt`.
- full PINNED trio → `useThreadGIL=false`. Dump:
  `evidence-w16/h3-dumpOptions-full-pinned.txt`.
F6 is therefore a CONFIG CONFOUND: "sharedGCHeap=0 shields the race"
reduces to "GIL-on is clean" (already a control). sharedGCHeap is NOT a
discriminating variable; the F6 open question is closed. Refute condition
(useThreadGIL still 0 with sharedGCHeap=0): not met.

### State for the fix round

Single root cause stands: LLInt op_*_varargs round-trips
(calleeFrame, length) through the shared raw VM members
`vm.newCallFrameReturnValue` / `vm.varargsLength`
(LLIntSlowPaths.cpp:2429-30 store, :2453-59 reload) between two slow calls;
both fields already exist in the VMLite Group-2 X-macro (VMLite.h:118/:122)
and sibling fields already route per-lite (VM::group3Primitives() pattern).
All three crash faces (133/134/139) plus the silent corrupted-posting-list
face are downstream of the same foreign (frame,length) pair.

### 2026-06-11 amendment of the A3-setupVarargsFrame-constantRegister-ov fix (post-review)

Review findings adjudicated against the tree; both code findings CONFIRMED,
amended in place (working tree on jarred/threads):

1. TOCTOU at the LLInt consumer (CONFIRMED): varargsSetup re-read
   `primitives.varargsLength` AFTER the echo RELEASE_ASSERT (the two setup
   calls), so a trample landing between the assert and the consume bypassed
   the fail-stop, relying on compiler CSE luck. Amended: single snapshot
   `unsigned varargsLength = primitives.varargsLength;` taken before the
   assert; assert recompute and both setup calls consume only the snapshot
   (LLIntSlowPaths.cpp varargsSetup). Comment also corrected: the echo has a
   known memory-safe residual blind spot (varargsLength-only trample inside
   the same stackAlignmentRegisters rounding bucket → wrong-value class).

2. Release fail-stop downgrade (CONFIRMED): the old constant-pool decode was
   an accidental Release tripwire (Vector CrashOnOverflow = A3 ABRT face);
   the fix had replaced it with debug-only ASSERT_UNUSED. Amended:
   `RELEASE_ASSERT(newCallFrame < callFrame)` in
   Interpreter.cpp setupVarargsFrame — strictly stronger than the old
   accidental check (no int32-truncation blind spot, also covers the
   JIT-tier consumer operationSetupVarargsFrame which has no echo check),
   never fires for legitimate pairs (both callers build the callee frame
   strictly below the caller; calleeFrameForVarargs subtracts), slow path.
   Smoke: 20k-iter apply/spread (LLInt->JIT) + W=2 GIL-off bench clean on
   Release and Debug-ASAN.

3. Verification framing (CONFIRMED as process gap): prior n=6 vs n=6 cannot
   adjudicate the residual. New data on the AMENDED tree:
   - Release W=16 pinned gate, N=12: ok=6 sigtrap133=5 sigabrt134=1
     sigsegv139=0 (/tmp/w16-verify/release-amended/). All 5 SIGTRAPs are the
     known residual face `pas panic: deallocation did fail ... Alloc bit not
     set in pas_segregated_page_deallocate_with_page`; the 1 SIGABRT is the
     known STW-watchdog 30s face (OM transition stop, 2 non-quiescent lites
     holding heap access) — neither is an A3 face and the new RELEASE_ASSERT
     never fired. Point estimate 6/12 clean vs 1/6 (old fixed) / 2/6
     (control): no evidence the amended fix worsens the residual; the
     pas family remains OPEN and is the next hunt target. With the
     Release fail-stop restored, any silent A3-face conversion now
     self-identifies instead of presenting as the pas family.
   - Debug-ASAN W=16 pinned invocation x10: results recorded below when the
     campaign completes (campaign copy with 2400s/run timeout — ASAN W=16
     full bench exceeds the script's stock 600s).
   - Debug-ASAN W=16 pinned invocation, 10 runs COMPLETE (2026-06-11):
     run1 sequential (2400s cap), runs 2-6 parallel x5 (2400s cap), runs
     7-10 parallel x4 (5400s cap; parallelism deviation from the sequential
     methodology was forced by ASAN runtime — a single clean W=16 bench
     does not finish inside 90min under ASAN — and INCREASES cross-process
     load, which historically helps the family fire). Result: 10/10
     survived to timeout with ZERO crashes, zero AddressSanitizer reports,
     zero RELEASE_ASSERT/echo-check fires, stderr clean except the useWasm
     banner (~9.3h aggregate ASAN W=16 exposure; logs:
     /tmp/w16-verify/asan-par/, /tmp/w16-verify/asan10x-amended/).
     Caveat: rc=124 means signature-free exposure, not bench completion;
     pre-amendment ASAN baseline crashed with DEADLYSIGNAL well inside
     such windows (evidence-w16/asan-run1.out).

### 2026-06-11f — A4 (wild pc / stack-exec) adjudication

Assigned faces: 9 logs, rounds r5-r8 (W8/16/32 N=400 nojit, ALL pre-host-call-fix
tree): pc=0xf / pc=0x14 (SEGV at zero page, bp = a thread-stack address),
"stack-overflow" with pc INSIDE the thread's stack region and bp=0, pc==addr in
image rodata/data, unknown-module callers unwinding to LLInt; frequent B1
(ExceptionScope verificationState) co-occurrence in the same runs.

MECHANISM (code-verified, victim-side return through a wild-written frame
header): on the pre-fix tree the LLInt varargs sequence (doCallVarargs,
LowLevelInterpreter64.asm: size_frame slow call -> per-VM scratch -> move r1,sp
-> varargsSetup reload -> frame-header writes) could consume a FOREIGN
{newCallFrameReturnValue, varargsLength} pair — the A1 root: the K4 table-I
Group-2 row encodedHostCallReturnValue raw-VM-block writers (fixed 2026-06-11b)
plus the pre-5c0e51c2b543 shared scratch — and then CONSTRUCT a call frame at a
foreign address, typically inside ANOTHER thread's live stack. The header
stores are the exact A4 face generators on the victim thread:
  setArgumentCountIncludingThis(N) writes a RAW small int -> a victim ret/pop
    that lands on it jumps to pc=0xf (argc 15) / 0x14 (argc 20);
  setCallerFrame(callFrame) writes a stack pointer -> pc inside the victim's
    own stack region (the ASAN "stack-overflow at pc==sp-region" face; bp=0
    from the adjacent null/garbage slot);
  callee/JSValue cell writes -> pc==addr in image data (cell pointers).
B1 co-occurrence is the same collateral one word over: the victim lite's
primitives block / ExceptionScope chain trampled by the same foreign-frame
header/argument writes (per the 2026-06-11d A2 amendment, corruption-collateral,
not value-flow). Named invariant: UNGIL §A.1.3(3) U-T1 per-lite Group-2/3
pairing invariant over the varargs scratch pair + the SPEC-vmstate §6.3 L1
frozen Group-3 word set (K4 table I row 1) — call-frame construction must
consume exactly the pair this thread stored; the CallerFrameAndPC slot
identity of the LLInt call/return protocol is a corollary. A4 is therefore
strictly DOWNSTREAM of the A1/A2/A3 root, which is closed on this tree
(5c0e51c2b543 per-lite scratch + 2026-06-11b host-call row + 2026-06-11c/d
echo RELEASE_ASSERT fail-stop + 2026-06-11e constant-pool-decode removal).
No NEW producer found: all unwind-word writers (genericUnwind, wasm catch
arms) already store via group3Primitives(); the only raw VM-block
callFrameForCatch readers are debug-trace-only (JIT.cpp/LOLJIT.cpp
traceBaselineJITExecution probes, dead under the failing profile).

FIX LANDED (the cataloged K4 deferred rows — the remaining-row checklist the
brief points at): same-VM guards (mirroring the landed
llint_get_host_call_return_value treatment: lite arm only when
VMLite::vm == the site's VM operand, else the VM-block arm the C++
group3Primitives() writers used) at EVERY remaining LLInt asm lite arm
touching the frozen Group-3 frame/unwind words:
  LowLevelInterpreter.asm: copy/restoreCalleeSavesFromVMEntryFrameCallee-
    SavesBufferGroup3 (topEntryFrame), vmEntryToJavaScript Setup +
    SetTopCallFrame fast-path arms;
  LowLevelInterpreter64.asm: doVMEntry save-prev/store/restore-prev/
    overflow-restore topCallFrame+topEntryFrame arms;
    llint_handle_uncaught_exception, llint_op_catch,
    llint_throw_from_slow_path_trampoline (the literal wild-pc jump
    `jmp VMLitePrimitives::targetMachinePCForThrow[t6]`); native +
    internal-function call trampolines (pre-call topCallFrame store,
    post-call m_exception check).
All guards are never-taken under the JSLock::didAcquireLock foreclosure
(defense-in-depth symmetry-by-construction, not a behavior change); flag-off
and GIL-on the lite arms are not taken, so those paths are untouched. No
assert weakened; the echo RELEASE_ASSERT and the setupVarargsFrame
RELEASE_ASSERT stay armed. K4 addendum updated (deferred rows closed; the
still-unguarded stack-limit-word sites listed with the standing foreclosure
note). Stack-limit/arity/sanitize sites deliberately NOT touched (different
word family, dual-publish writers, non-pc-harvest).

### 2026-06-11f verification results (A4 gate, final tree, both builds rebuilt)

- ASAN Debug gate at A4's profiles (nojit N=400 smoke, pinned GIL-off flags,
  waves of 7/7/6 concurrent — the r5-r8 load shape; W mix matches where A4
  appeared: W16 x12 + W32 x5 + W8 x3): rounds/a4gate-w1..w3 = 20/20 PASS,
  zero failure logs, zero A4 faces (no wild-pc SEGV, no stack-exec, no
  pc==addr), zero A1-A6/B1 faces, zero same-VM-guard-related faults.
  NOTE: host concurrently carried the LIMP-worktree full-size ASAN W=16
  recon runs — extra cross-process load, which historically HELPS the
  family fire; still clean.
- Release full-size pinned W=16 x3 sequential (/tmp/a4rel): rel1 PASS
  (58.4s, all checksums emitted); rel2+rel3 SIGTRAP pas panic ("Alloc bit
  not set in pas_segregated_page_deallocate_with_page") — the PRE-EXISTING
  cataloged JIT-on libpas/OSR-exit log-collision family (SIGNATURES.md
  2026-06-11c NEW EVIDENCE; reproduced 5/6 on the pre-A4 tree AND 3/6 in
  the fix-REVERTED a3rel control), statistically unchanged here (1/3 on a
  host still carrying the LIMP ASAN runs). NO A4 face in any Release run.
  That family is out of A4 scope (LLInt-forced profile is clean; rel10
  symbolized core positively excludes the unwind/return path) and remains
  the next hunt target (thread-bughunter charter).
- Identity smokes: flag-off Release, GIL-off Release jit + nojit, GIL-on
  Release, GIL-off Debug-ASAN nojit — varargs/apply/eval-hostcall/catch
  matrix identical output across all arms (the catch arm exercises the
  guarded op_catch/throw-trampoline harvest sites).
- A4 signature CLOSED at its W under the profile where it appeared; the
  guards additionally close the K4 deferred-row checklist for the Group-3
  frame/unwind word family.

### 2026-06-11g amend round 3 (adversarial-review findings on the A4 fix)

Findings verified and dispositioned (none refuted — all four were real,
two were duplicates of each other):

1. (MAJOR, valid; two duplicate filings) varargsSetup snapshot
   (LLIntSlowPaths.cpp) used PLAIN loads of the racing per-lite pair and
   claimed CSE-immunity "by construction" — C++ gives no such guarantee:
   under the data-race-free assumption the compiler may rematerialize a
   non-atomic load at the use sites AFTER the echo RELEASE_ASSERT,
   reopening the TOCTOU the comment declares closed (latent: current
   codegen keeps registers, but compiler-version-dependent, not
   by-construction). FIXED: both words now read via
   WTF::atomicLoad(..., std::memory_order_relaxed) — exactly one load
   each feeds both the validation and the consumption; comment rewritten
   to state the actual mechanism. No assert weakened.
2. (MAJOR, valid) Same-VM guard asymmetry: the per-CodeBlock JIT lite arms
   reading the SAME frozen Group-3 unwind words had no lite->vm check.
   FIXED: guards added (lite->vm == compiled-for-VM immediate, else the
   VM-block word — writer-selector symmetry, never-taken under the
   didAcquireLock foreclosure) at Baseline emit_op_catch
   (JITOpcodes.cpp: topEntryFrame + callFrameForCatch load/clear), the
   DFG OSR-exit exception ramp (DFGOSRExitCompilerCommon.cpp:
   targetInterpreterPCForThrow store + topEntryFrame/callFrameForCatch),
   and CCallHelpers::jumpToExceptionHandler (targetMachinePCForThrow —
   the A4 wild-pc consumption, JIT face). Guards sit inside the
   vm.gilOff() emission legs: flag-off/GIL-on emission unchanged.
   EXPLICIT RULING recorded in the K4 addendum for the remaining
   emission-side helpers left unguarded (prepareCallOperation,
   emitPublish*, load{TopEntryFrame,CallFrameForCatch,Exception},
   branchPtrAgainstSoftStackLimit): non-pc-harvest words +
   reserved-temp scratch discipline leaves no free register for a
   fallback arm + same foreclosure.
3. (MAJOR, valid — process gap) The gilOff getHostCallReturnValue thunk
   arm had no executed JIT-on gate. FIXED: new targeted gate
   Tools/threads/bughunt/varargs-fix/hostcall-hammer.js (per-thread hot
   loop: Math.max.apply varargs host-call returns, parseInt plain
   host-call, throw-across-host-frame forEach catch, direct eval;
   forced tier-up + --validateFreeListStructure=1; identical-checksum
   PASS criterion). Thunk emission in a gilOff process confirmed via
   --dumpDisassembly label dump (128-byte body = discriminator arm
   present; instruction-level dump unavailable, no disassembler in
   build).

Verification (amend3-campaign.sh, amend3-logs/RESULTS.txt):
- hammerD: 10/10 PASS Release JIT-on GIL-off forced tier-up + FreeList
  validator. hammerE: 5/5 PASS Debug ASAN JIT-on GIL-off.
- reproA (the pinned A1 signature, 10x Debug ASAN nojit W=4): 10/10 PASS.
- smokeB (10x Debug ASAN bench.js -- 16 smoke, JIT on): 10/10 PASS
  (~435-482s each), zero ASAN reports.
- releaseC (10x Release full bench.js -- 16): 6/10 PASS, all 4 failures
  the PRE-EXISTING pas_deallocation_did_fail SIGTRAP family
  (SIGNATURES.md 2026-06-11c) — vs 4/10 in the round-2 campaign on the
  pre-amend tree: statistically unchanged residual, no new signature,
  no echo-assert fire, no A4 face.
- Identity smokes: flag-off Release + Debug (apply/catch matrix
  checksum), GIL-on threads smoke.js — all PASS. (The campaign's
  flagoffF leg rc=3 was a campaign-script bug — bench.js requires the
  threads API; superseded by the direct flag-off smokes above.)

### 2026-06-11h amend round (adversarial-review findings on the A5 fix)

Four findings adjudicated against the tree; ALL FOUR CONFIRMED on their
factual claims (none refuted), each independently re-verified this round:

1. (MAJOR, valid) II.19 closure was VACUOUS as a safety claim: the
   cataloged race was already dead on this tree via the pre-existing
   fill-side gate MegamorphicCache::fillsDisabledUnderJSThreads()
   (verified present at the GIL-removal merge 43fd5fb94387; introduced
   at its ancestor checkpoint ee901cc2dfd5 — immaterial nit vs the
   finding's attribution; all six initAs* fills no-op under
   useJSThreads) plus the AssemblyHelpers.cpp:83/100 inline-probe
   bails. The new canUseMegamorphic*/tryFoldToMegamorphic gates are
   codegen hygiene only (kept — verified harmless: with the fold
   refused, cases.size() >= maxAccessVariantListSize still yields
   GeneratedFinalCode at InlineCacheCompiler.cpp:5148, so no IC
   regeneration livelock; InstanceOfMegamorphic exemption verified — a
   generic prototype-walk loop, no MegamorphicCache access). AMENDED:
   SIGNATURES.md session-h entry and the K4 II.18/II.19 row addendum
   rewritten to book II.19 as "codegen hygiene atop an already-dark
   row"; the inference "pas face persisting past the II.19 closure
   confirms the megamorphic race was not its sole producer" RETRACTED
   as vacuous (II.19 was provably not a producer on this tree); the pas
   hunt re-pointed ENTIRELY at the SPEC-jit code-lifecycle / OSR-exit
   double-ownership suspect.
2. (MAJOR, valid) II.18 is the only genuinely NEW safety closure of the
   A5 round — and it is sound (re-verified: ObjectPrototype.cpp:127 is
   the tree's sole ensureHasOwnPropertyCache() site;
   DFGByteCodeParser.cpp:4661 existence gate keeps the DFG
   HasOwnProperty fast path unselected in a gilOff process — gilOff is
   process-constant so the cache stays null; DFGOperations.cpp:4845/
   4858 ASSERT paths reachable only via that intrinsic; Heap.cpp:2849
   null-checks). Booked as a real window-shrink of the pas face class,
   NOT a closure of the live JIT-on Release producer.
3. (MAJOR, valid — verification power) n=5 Release W=16 cannot support
   a differential claim about the residual pas family vs the 1/3-5/6
   baselines, and the nojit ASAN gate structurally cannot exercise
   JIT-on surfaces. AMENDED bookkeeping: the residual family stays OPEN
   and uncounted against this round (SIGNATURES.md correction); the
   N>=20 pre/post Release W=16 differential is DEFERRED to the
   thread-bughunter charter for the SPEC-jit suspect, where it is the
   opening evidence-pack item — running it on a host carrying the LIMP
   ASAN recon campaign would be load-confounded anyway.
4. (MAJOR, valid — perf) the interim disable (megamorphic ICs +
   HasOwnPropertyCache, all tiers, every gilOff process) is exactly the
   configuration SCALEBENCH measures. AMENDED: explicit
   measured-configuration caveat added to docs/threads/SCALEBENCH.md
   (runs 1-2 pre-date the disable; any re-run must land the ruled
   per-lite copies first or restate the caveat); cost note recorded in
   the K4 row addendum. Accessor note confirmed and already addressed:
   all three gates use the F1-convention gilOffWithProcessGate()
   frozen-Config-page test (VM.h:697), and the ledgers no longer claim
   literal byte-identity — "behavior unchanged, one predicted-false
   branch". The binding V5b quiet-host flag-off bench rung is OWED
   before the next flag-off-perf-sensitive landing (host currently
   inadmissible: LIMP ASAN W=16 recon running; recorded here, not
   silently dropped).

Re-verification on the amended tree (both builds rebuilt at 14:51 after
the 14:49 amendments; campaign a5amend-campaign.sh, output
w16/rounds/a5amend-campaign.out, host concurrently carrying the LIMP
ASAN W=16 recon — extra load, historically helps the families fire):
- Pinned A5 signature gate, 10x ASAN Debug nojit N=400 smoke at the
  signature W mix (W16 x6 + W32 x2 + W8 x2, two waves of 5):
  10/10 PASS, zero failure logs, zero A5/A1-A6/B1 faces, zero ASAN
  reports (w16/rounds/a5amend-w1, a5amend-w2).
- a5-megamorphic-hammer re-cover of the amended gates: ASAN JIT-on
  GIL-off x3 + ASAN nojit GIL-off x2 + ASAN flag-off x1 = 6/6
  A5-HAMMER-PASS, zero MISMATCH, zero ASAN reports, identical checksum
  d0639460 across all arms (w16/rounds/a5amend-hammer/).
- Release identity arms (flag-off / GIL-on / GIL-off jit / GIL-off
  nojit): 4/4 rc=0, identical checksum d0639460, 0 mismatches.
- A5 signature remains CLOSED on the amended tree; no assert weakened;
  flag-off and GIL-on behavior unchanged.

### 2026-06-11 A6 amend round (adversarial-review findings on the A6 closure)

Full adjudication + new gates of record: SIGNATURES.md Session 2026-06-11i
(Tools/threads/bughunt/w16/). Summary:
1. Both prior A6 Release legs were host-contaminated (foreign
   /root/WebKit-limp W=16 verify campaign confirmed still running; plus
   tmpfs RAM pressure from 14-15GB /tmp/cores dumps per abort-family
   failure with swap at 61/63G). The g-record "baseline-consistent rates"
   sentence is retracted; quiet-host n=12 pinned Release gate
   (rounds/a6amend-rel) = ok=6/12, exactly the pre-change baseline rate
   and family mix => no Release regression from the A6 echo (or the
   co-resident b..h amendments).
2. rel-3 empty-log rc=1 positively classified: /tmp/cores/core.1394924 is
   a ZERO-BYTE core mtime-matched to rel-3 — the run crashed (known-family
   signal) but tmpfs ENOSPC zeroed its core, its log, and GNU time's
   output (rc=1 = time's write-error exit). No dmesg oom-kill on
   2026-06-11; the earlier "memory pressure startup casualty" wording was
   a guess and is superseded by this evidence.
3. The reviewer's "this change secretly introduces the geometric assert +
   snapshot discipline" is refuted by the session ledger (d-record item 2,
   A3/A4 amend rounds); the union-certification point is conceded and a
   per-fix hunk manifest now sits in the i-record for landing-time commit
   splitting. The LLIntSlowPaths "redundant pin" comment was scoped to
   name the b-session reroutes explicitly.
4. 32_64/CLoop silent-wrong-value asymmetry: confirmed and closed by an
   Options.cpp refusal clause (GIL-off forced back to GIL-on on
   !(CPU(X86_64)||CPU(ARM64)) || ENABLE(C_LOOP) builds); 64-bit builds
   preprocess it away (byte-identical codegen, existing gates remain
   valid).
ASAN pinned-invocation re-verify (rounds/a6amend-asan: 10x pinned JIT-on +
4x --useJIT=0, full-size bench.js -- 16) and the V5b flag-off perf
spot-check: results recorded in the i-record as they complete.

### 2026-06-12 A2 amend round 4 (adversarial-review findings on the A2/varargs fix family)

Finding adjudications (verify-each; refutations recorded where earned):
1. "JIT-on legs near-zero verification" — PARTIALLY STALE: the round-3
   campaign (varargs-fix/amend3-logs/RESULTS.txt) already ran JIT-on legs
   (hammerD 10/10 Release forced-tier-up, hammerE 5/5 Debug-ASAN JIT-on,
   smokeB 10/10 Debug-ASAN bench -- 16 smoke JIT-on). REMAINING GAP
   accepted: the Release one-line SIGSEGV from the original 3-run sweep was
   never core-triaged, and no pre-diff baseline rate exists (pre-diff
   checkout forbidden this round: no-git constraint). This round's Release
   leg runs with cores enabled; any SEGV core is triaged against the new
   emission-time code ranges (results below).
2. DFGThunks.cpp osrExitGenerationThunkGenerator perLiteMode arms
   (osrExitIndex store32 + osrExitJumpDestination farJump) — CONFIRMED: the
   one sibling Group-3 per-lite reader/writer pair the round-3 audit
   missed, and the worst face (indirect farJump = wild PC). CLOSED: both
   arms now carry the same lite->vm == &vm guard as the rest of the family
   (&vm is an emission-time immediate; thunk is keyed per-VM in
   vm.jitStubs).
3. Co-resident uncommitted deltas (Heap.cpp waitForCollector shared-server
   wait rewrite + relinquishConn early-return + notifyIsSafeToCollect
   shared arm; ObjectPrototype/InlineCacheCompiler/ExceptionScope) —
   CONFIRMED as tree state. Partitioning via commit/stash is forbidden this
   round (no-git). RECORDED: (a) all gate results in this ledger certify
   the UNION tree, not the A2 delta alone (the existing i-record hunk
   manifest is the landing-time split); (b) the Heap.cpp
   waitForCollector/relinquishConn rewrite is hereby NAMED SUSPECT for the
   rc=133/134 Release residual families (it modifies collector-wait
   liveness on top of 6b298a4fdd99) and must be handed to the
   watchdog-family bughunter as such, not treated as prior landed state.
4. Fourteen+ never-taken foreign-lite fallback arms read the shared
   VM-block word silently — CONFIRMED inconsistent with the family's own
   fail-stop dispositions (Options.cpp refusal, varargs RELEASE_ASSERTs).
   CLOSED: every foreign-lite arm (LowLevelInterpreter.asm x4 sites/6
   lines, LowLevelInterpreter64.asm x12, JITOpcodes op_catch x2,
   CCallHelpers jumpToExceptionHandler, DFGOSRExitCompilerCommon x3,
   LLIntThunks getHostCallReturnValueThunk, DFGThunks x2 new) now
   fail-stops (break / jit.breakpoint()) under ASSERT_ENABLED and keeps the
   writer-symmetry VM-block fallback in Release only. The thunk's
   noLite/gilOnLite faces remain legitimate VM-block reads in all flavors
   (U0b loser threads). No assert weakened; Debug/ASAN gates can now
   observe a didAcquireLock-foreclosure violation instead of masking it.
5. Flag-off identity claim — CONFIRMED FALSE at the C++ slow-path level
   (unconditional RELEASE_ASSERTs + TLS echo on every LLInt varargs call).
   DISPOSITION: asserts stay unconditional (house no-assert-weakening rule;
   gating on useJSThreads() would weaken the tripwire exactly where a
   future pairing-contract break would otherwise ship silently). The delta
   is now EXPLICITLY adjudicated in-code (LLIntSlowPaths.cpp thread_local
   block: "FLAG-OFF BEHAVIORAL DELTA") and a pairing-contract guard note
   sits on the uninstantiated SetArgumentsWith::CurrentArguments arm.

Round-4 verification of record (amend4-logs/RESULTS.txt; union tree per
item 3 above; host load decayed 23->8 across the campaign):
- REGRESSION CAUGHT AND FIXED DURING THIS ROUND: the first cut of the
  DFGThunks same-VM guard used bufferGPR (x86_64 assembler scratch r11) as
  the branchPtr base against a TrustedImmPtr — the immediate materializes
  INTO r11, self-clobbering the lite base, making the compare
  always-foreign (Debug: instant SIGTRAP in the new breakpoint; Release:
  fallback read of the stale per-lite-mode VM-block words => wild farJump
  SEGV; hammerD 0/10, hammerE 0/3). Final shape: validation moved to
  after the register dump (regT0 free, restored by the load spooler),
  validated osrExitJumpDestination word ADDRESS stashed in one extra
  per-lite scratch slot, post-restore jump is a slot load + farJump with
  no compare. All other guard sites re-audited for the same hazard: regT3
  / regT1 / selectScratchGPR bases and reg-reg compares — none use the
  assembler scratch as compare base. (Incidentally this round-4 bug is
  direct evidence for finding 1's thesis: the round-3 gate could not have
  caught a thunk-emission bug like this only because the broken code did
  not exist yet; the JIT-on legs DID catch it within seconds this round.)
- hammerD (Release JIT-on forced-tier-up GIL-off) 10/10 rc=0, 0
  mismatches. hammerE (Debug-ASAN JIT-on) 3/3 rc=0 — these exercise the
  OSR-exit thunk + op_catch + thunk guards with the new ASSERT-build
  fail-stop arms armed: ZERO traps fired => the didAcquireLock
  foreclosure holds on every exercised path; the arms are detection
  power, not behavior.
- SIGNATURE GATE (pinned): reproA 10/10 rc=0 — Debug ASAN --useJIT=0
  REPRO_THREADS=4 minimal repro. Zero ASAN reports.
- smokeB (Debug-ASAN bench.js -- 16 smoke, JIT ON) 2/2 rc=0, zero ASAN
  reports, zero foreign-lite traps.
- releaseC (Release bench.js -- 16 full, cores enabled) 3/10 rc=0;
  ALL 7 failures core-triaged: 6x rc=133 libpas
  pas_segregated_page_deallocate_with_page "Alloc bit not set" panic
  (chartered W>=16 residual, faulting PCs in libpas, NOT in any new
  emission code), 1x rc=134 JSThreadsSafepoint.cpp:577
  watchdogAssertStopProgress 30s STW abort (OM transition stop context;
  ab17b family — see the named-suspect Heap.cpp record in item 3). ZERO
  silent SIGSEGVs this round (the finding-1 untriaged face did not
  reproduce in 10 runs); pass-rate delta vs round 3 (6/10) is within the
  documented host-load sensitivity and the family mix is unchanged.
- flag-off smoke (varargs workload, no thread flags): Release rc=0,
  Debug rc=0.

### 2026-06-12 A3-setupVarargsFrame-constantRegister-ov final signature gate

Mechanism re-adjudicated against the tree (no code change needed this round;
the 2026-06-11e fix + 2026-06-11 amendments are in place and both jsc builds
are newer than every source file):
- CodeBlock.h:1296 `CallFrame::r` decodes `reg.isConstant()` into
  `codeBlock()->constantRegister(reg)` = `m_constantRegisters[toConstantIndex]`
  (CodeBlock.h:572) — the Vector OOB CrashOnOverflow ABRT face. The old
  setupVarargsFrame routed (newCallFrame - callFrame) through int32
  VirtualRegister; a foreign (frame,length) pair (UNGIL §A.1.3(3) U-T1
  per-lite Group-2/3 pairing invariant; SPEC-vmstate §6.3 L1 frozen Group-3
  word set, K4 table I) makes that delta decode as a constant operand.
  Fix in tree: direct frame arithmetic + RELEASE_ASSERT(newCallFrame <
  callFrame) fail-stop (Interpreter.cpp:365-400).

Verification (this round, /tmp/a3-verify/):
- ASAN (Debug) W=16 pinned invocation, 20 runs, 4-way parallel batches:
  - 12x --useJIT=0 smoke (the exact r5 A3 signature environment): all
    rc=124 (560s signature-free exposure each; pre-fix faces fired well
    inside 600s windows per the r5 campaign methodology). stderr clean
    except the useWasm banner.
  - 8x JIT-on smoke: all rc=0, completed clean.
  - Grep gate across all 40 logs: zero AddressSanitizer reports, zero
    MISMATCH, zero CrashOnOverflow/constantRegister/setupVarargsFrame/
    ASSERT hits. Signature gate GREEN 20/20.
- Release W=16 full bench x3 (sequential): rc=133, 133, 134 — 0/3 clean.
  Both 133s are the known OPEN residual `pas panic: deallocation did fail
  ... Alloc bit not set in pas_segregated_page_deallocate_with_page`;
  the 134 is the known STW-watchdog non-quiescent face. Zero A3 faces;
  the new RELEASE_ASSERT never fired. Per the both-legs criterion this
  round reports verified=false on the Release leg, same adjudication as
  A1/A2 (orthogonal cataloged families; pas family remains the next hunt
  target).

### 2026-06-12 A3 amend round (adversarial-review findings on the A3 final gate)

Five findings, all evidence/process-class; NO code defect identified in the
A3 fix (Interpreter.cpp direct-arithmetic + RELEASE_ASSERT, LLIntSlowPaths
snapshot/echo asserts) — the fix is unchanged this round. Adjudications
(verify-each; all campaign runs in /tmp/a3-amend/, cores ENABLED throughout,
core_pattern /tmp/cores, cores stay local):

1. "Release rc=133 could be the new RELEASE_ASSERT, not pas" (blocker) —
   REFUTED EMPIRICALLY; process critique CONFIRMED. Premise correction
   first: this libpas build prints "pas panic: deallocation did fail" to
   STDOUT (rel-run1.out/rel-run2.out, 124 bytes each contain it), not
   stderr — the prior attribution was text-backed, not rc-only. New
   discriminating evidence: Release W=16 bench x6 with cores enabled →
   rc {0,0,133,134,134,139}; every SIGTRAP/ABRT core walked:
   - core.3217881 (rc=133): pas_crash_with_info_impl <-
     pas_deallocation_did_fail ("Alloc bit not set in
     pas_segregated_page_deallocate_with_page") <- fastFree of a
     WTF::HashTable backing store in DFG::VariableEventStream::reconstruct
     (operationCompileOSRExit). NOT the varargs asserts.
   - core.3214606 / core.3217882 (rc=134): JSThreadsSafepoint.cpp:577
     watchdogAssertStopProgress 30s STW abort (known ab17b family).
   - core.3214608 (rc=139): wild-PC SEGV in JIT code (no symbolized C++
     frames; separate cataloged residual face).
   Zero RELEASE_ASSERT frames in any core; grep across all logs: zero
   assert signatures. The rc=133 face IS the pas trap.
2. "pas residual orthogonality asserted, not discriminated" (major) —
   GAP CONFIRMED, NOW CLOSED. (a) bench-nospread.js (varargs-free) Release
   W=16 x6 post-fix: rc {0,0,133,133,133,134} — pas faces persist at the
   same rate with ZERO varargs ops in the workload; all three 133 cores =
   identical pas face. (b) The walked bench pas core (above) has no
   setupVarargsFrame/calleeFrameForVarargs/loadVarargs/sizeFrame frames in
   ANY of its threads (thread-apply-all grep). (c) Negative result
   recorded: the new RELEASE_ASSERTs never fired in any failing run.
   Pas family is causally independent of A3 → remains the next hunt
   target, now with two post-fix cores as its starting evidence.
3. "K4 Table I rows not re-swept this round" (major) — PARTIALLY REFUTED:
   the named rows were already ruled and guarded in recorded K4 addenda
   (catch/throw asm + JIT-emission same-VM guards; wasm catch-unwind
   reroute behind the useWasm GIL-off disable). CURRENT-TREE liveness
   verified this round, row-by-row record appended to
   docs/threads/SPEC-ungil-audit-K4.md (§I addendum 2026-06-12).
4. "No flag-off leg despite unconditional flag-off asserts" (major) —
   GAP CONFIRMED, NOW CLOSED: plain Release jsc (no thread flags) over 8
   varargs/spread stress tests x {JIT, --useJIT=0} = 16/16 rc=0;
   flag-off GIL-on bench (--useJSThreads=1 only) W=16 smoke rc=0 with
   sane output JSON. Zero assert fires. The unconditional tripwires are
   empirically inert in the production shape.
5. "ulimit -c 0 in the harness is a process regression" (major) —
   CONFIRMED, FIXED: evidence-w16/campaign.sh now sets ulimit -c
   unlimited with the F5 rationale; gate convention going forward is
   rc + stderr grep + core disposition per run.

Re-verified signature gate (pinned, cores enabled): 10x Debug-ASAN
--useJIT=0 GIL-off bench.js -- 16 smoke, 480s windows, 10-way parallel:
10/10 rc=124 (timeout = signature-free exposure), zero ASAN reports, zero
pas panics, zero assert signatures, ZERO cores produced. A3 signature gate
GREEN 10/10 and — unlike the prior round — the negative is core-adjudicable.

### 2026-06-12 A4-wild-pc-jump final signature gate (re-verification round)

Tree state: the 2026-06-11f A4 fix family confirmed present and built
(ninja no-op both build dirs): same-VM guards at every LLInt asm lite arm
touching the frozen Group-3 frame/unwind words (doVMEntry x4,
llint_handle_uncaught_exception, llint_op_catch,
llint_throw_from_slow_path_trampoline — the literal wild-pc jump site,
LowLevelInterpreter64.asm:3335-3358 — native/internal-function call
trampolines, vmEntryToJavaScript Setup/SetTopCallFrame, both
*CalleeSavesBufferGroup3 macros), JIT-emission guards (emit_op_catch,
DFG OSR-exit exception ramp, CCallHelpers::jumpToExceptionHandler), the
relaxed-atomic varargs snapshot + thread-local echo RELEASE_ASSERTs in
LLIntSlowPaths.cpp varargsSetup, and the group3Primitives() reroutes
(handleHostCall both arms, wasm catch-unwind writers). Governing
invariant re-affirmed against the spec: UNGIL §A.1.3(3) U-T1 per-lite
Group-2/3 pairing over the SPEC-vmstate §6.3 L1 frozen Group-3 word set
(K4 table I) — frame construction/unwind must consume exactly the words
THIS thread's writer stored; CallerFrameAndPC slot identity of the LLInt
call/return protocol is the corollary the A4 faces violated.

- ASAN Debug signature gate, 20/20 GREEN: campaign.sh profile (bench-local
  smoke N=400, --useJIT=0, pinned GIL-off flags), W mix matching r5-r8
  where A4 fired: W16 x12 (1 sequential calibration PASS 279s + 11 in
  waves) + W32 x5 + W8 x3, waves of 7/7/5 concurrent
  (rounds/a4verify-w1..w3). Zero failure logs, zero A4 faces (no wild-pc
  SEGV, no stack-exec face, no pc==addr), zero B1
  ExceptionScope-verificationState fires, zero echo-assert fires.
- Release full-size pinned sanity (bin/jsc <flags> scalebench bench.js --
  16), 4 runs total this round: 1 PASS (47.2s, all checksums), 2x SIGTRAP
  pas panic "Alloc bit not set in pas_segregated_page_deallocate_with_page"
  (the PRE-EXISTING cataloged JIT-on libpas family, SIGNATURES.md
  2026-06-11c — reproduced pre-A4 and in the fix-reverted control;
  statistically unchanged), 1x STW watchdog 30s diagnostic abort ("OM
  transition stop" pending, one lite NON-QUIESCENT with heap access — the
  cataloged stop-progress family, out of A4 scope). NO A4 face in any
  Release run.
- Housekeeping: /tmp tmpfs hit 100% mid-round (74G of cores, incl. 4 from
  this round's first Release batch dumped by the pas family); adjudicated
  pre-round cores deleted, newest pas core retained at
  /tmp/cores/core.3314524.

Disposition: A4 signature gate GREEN at the W/profile where it appeared;
Release sanity NOT clean (orthogonal cataloged families only), so per the
both-legs criterion verified=false; the pas/STW residuals remain the next
hunt targets (thread-bughunter charter).

### 2026-06-12 A4 amend round (adversarial-review findings on the A4 final gate)

Seven findings adjudicated (verify-each; refutations recorded where earned).
Code amendments landed this round; both build dirs rebuilt (Debug ASAN +
Release) and re-gated below.

1. "Release-leg closure evidence statistically empty (N=4); Release guard
   arms are silent fallbacks" — CONFIRMED on both points; PARTIALLY CLOSED.
   (a) Wording corrected: the A4 closure is DEBUG-LEG ONLY; the prior
   "closed at its W/profile" phrasing is superseded by "Debug-leg closed,
   Release-leg pending the both-legs gate" (N=4 cannot distinguish A4's
   historical rate from zero). (b) This round adds 6 pinned Release W=16
   runs (a4amend-rel/RECEIPTS.txt): 1 PASS, 5x rc=133 — every failure
   text-confirmed "pas panic: deallocation did fail ... Alloc bit not set"
   and the newest core (core.3384180) frame-walked: pas_crash_with_info_impl
   <- pas_deallocation_did_fail <- fastFree of a DFG MinifiedGenerationInfo
   HashTable backing store — the identical cataloged face as pre-A4
   core.3217881; ZERO A4-shaped frames, zero new-guard traps. (c) The
   retained core.3314524 can no longer be positively symbolized: this
   round's rebuild replaced the Release binary; the 5 fresh same-binary
   cores supersede it as evidence. Release-leg differential at N>=20 vs a
   fix-reverted control remains DEFERRED (no-git constraint); verified
   stays false on the both-legs criterion.
2. "Sibling wasm raw VM-block readers of targetMachinePCForThrow remain" —
   CONFIRMED (the "wasm catch-unwind writers" coverage wording in the prior
   record overstated: only the RetrieveAndClearException arms were
   rerouted). CLOSED by reroute, not comment: all 8 remaining raw
   reader sites now go through group3Primitives() — WasmOperations.cpp
   operationWasmUnwind/throw/rethrow tails, WasmOperationsInlines.h
   throwWasmToJSException, WasmIPIntSlowPaths.cpp throw_exception/
   rethrow_exception/throw_ref + slow_path_wasm_unwind_exception. GIL-on/
   flag-off group3Primitives() is the VM block (mainVMLitePrimitives), so
   behavior is identical there; the useWasm GIL-off force-disable
   (Options.cpp) remains the reachability gate, but a future gate lift no
   longer reopens A4 on the wasm catch path.
3. "Gate artifacts deleted; 20/20 not independently auditable" — CONFIRMED
   (rounds/a4verify-w1..w3 are empty; only mtimes corroborate). CLOSED
   going forward: w16/campaign.sh now writes a one-line per-run receipt
   (UTC timestamp, W, N, label, rc, wall time, flags sha1, suspect-line
   count) to RECEIPTS.txt even on PASS, and sets ulimit -c unlimited (the
   A3-round F5 convention; it previously still had ulimit -c 0). This
   round's waves regenerated auditable receipts (below).
4. "A4 not demonstrated for JIT-on Release; Release fallback arms have zero
   coverage" — CONFIRMED; PARTIALLY MOOTED by amendment 7 below (the three
   CFI arms no longer have a Release-only fallback to cover — they trap in
   all flavors). The remaining Release-only fallback arms are pure
   data-word stores/loads. The Options-gated force-the-fallback test mode
   and the N>=20 pre/post Release differential remain DEFERRED; verified
   stays false.
5. "Cross-subsystem co-resident deltas contaminate the STW-watchdog
   attribution" — CONFIRMED as tree state (already named: A2 round-4 item
   3 records the Heap.cpp waitForCollector/relinquishConn/
   notifyIsSafeToCollect rework as NAMED SUSPECT for the rc=134 family and
   the union-certification caveat). Full per-file working-tree list as of
   this round: InlineCacheCompiler.{cpp,h}, RepatchInlines.h,
   DFGOSRExitCompilerCommon.cpp, DFGThunks.cpp, Heap.cpp, Interpreter.cpp,
   CCallHelpers.h, JITOpcodes.cpp, LLIntSlowPaths.cpp, LLIntThunks.cpp,
   LowLevelInterpreter{,64}.asm, ExceptionScope.{cpp,h},
   ObjectPrototype.cpp, Options.cpp, VMLite.h, WasmOperations.cpp (+ this
   round: WasmOperationsInlines.h, WasmIPIntSlowPaths.cpp). The
   watchdog-family control with Heap.cpp hunks reverted is DEFERRED to the
   bughunter charter (no-git partitioning constraint); note this round's
   Release batch hit ZERO rc=134 faces (0/6).
6. "Flag-off RELEASE_ASSERT delta + CurrentArguments landmine" — first
   half STALE (the FLAG-OFF BEHAVIORAL DELTA adjudication block has been
   in LLIntSlowPaths.cpp since A2 round 4, and A3-amend item 4 ran the
   flag-off legs 16/16 green); second half CONFIRMED and CLOSED: the
   comment-guard is now a compile-time guard — static_assert(set ==
   SetArgumentsWith::Object) in varargsSetup, so instantiating the
   CurrentArguments arm without wiring its sizer to the thread-local echo
   pair fails at build time, not in production. No assert weakened.
7. "Debug/Release semantic fork: Release silently falls back to the shared
   VM-block word at wild-pc jump sites" — CONFIRMED inconsistent with the
   family's own fail-stop disposition. CLOSED at the three
   control-flow-integrity harvests: CCallHelpers::jumpToExceptionHandler
   (farJump target), llint_throw_from_slow_path_trampoline .liteThrowTarget
   (the literal A4 jmp site), and the DFGThunks OSR-exit destination word
   now fail-stop (breakpoint/break) in ALL build flavors — a deterministic
   trap at a known PC on a never-taken path beats a narrowed wild jump.
   Writer-symmetry Release fallbacks are retained ONLY at pure data-word
   arms (topCallFrame/topEntryFrame/callFrameForCatch stores,
   encodedHostCallReturnValue load), per the finding's own carve-out.

Re-verified signature gate (pinned, this round, after rebuild):
- ASAN Debug 10/10 GREEN: campaign.sh waves a4amend-w1/w2 (totals W16 x6,
  W32 x2, W8 x2; each wave W16x3+W32x1+W8x1), N=400 smoke, pinned
  GIL-off flags, --useJIT default: all rc=0, suspect_lines=0 in every
  receipt (RECEIPTS.txt retained per amendment 3). Zero A4 faces, zero
  echo-assert fires, zero new-guard traps.
- Release pinned sanity 6 runs: 1 PASS (48s, checksums OK), 5x pas family
  (adjudicated under finding 1 above), 0x STW watchdog, ZERO A4 faces,
  ZERO traps from the new always-on CFI arms (their codegen is exercised
  on every GIL-off Release run; the arms themselves remain never-taken,
  which is the didAcquireLock foreclosure holding).
- Flag-off smoke: Release + Debug plain jsc rc=0.

Disposition: A4 Debug-leg gate GREEN 10/10 with auditable receipts;
Release leg still blocked by the orthogonal pas/STW families (5/6 failures
here), so verified=false stands on the both-legs criterion. The pas family
(now with a fresh adjudicated core at /tmp/cores/core.3384180) is the next
hunt target.
