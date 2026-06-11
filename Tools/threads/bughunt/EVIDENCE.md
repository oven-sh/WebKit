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
