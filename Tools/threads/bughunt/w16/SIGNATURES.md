# W>=16 residual sweep — ASAN Debug signature ledger (2026-06-11)

Binary: WebKitBuild/Debug/bin/jsc (ASAN). Tree: post-varargs-fix 5c0e51c2b543.
Workload: bench-local.js (= scalebench bench.js + CLI N override; N_BASE via 3rd arg).
Pinned flags + per-cell amplifiers. Rounds r1..r8 under rounds/ (failure logs only kept).
NOTE: jsc cannot read /proc/self/environ — SCALEBENCH_SMOKE=1 env is a NO-OP; the
literal `smoke` argument is required (cost one wasted 10-min full-size calibration).

Run matrix: 14 base-flag runs (W=8/16/32, N=600..2000) = 0 failures.
22 amplified failures over 24 amplified runs: --useJIT=0 (LLInt-forced) fails ~100%
at W>=8/N=400; --collectContinuously=1 asserts 3/3; --validateFreeListStructure=1
alone 0/2; fasttier thresholds 0/4. Solo base N=2000 W=16 passes in 7m17s.

| id | kind | count | crash stacks (dedupe keys) |
|---|---|---|---|
| A1 | wild memcpy r/w | 8 | __asan_memcpy < JSCellButterfly::copyToArguments JSCellButterfly.cpp:63 < loadVarargs < setupVarargsFrame(AndSetThis) < LLInt::varargsSetup < llint_slow_path_call_varargs. T0 victims show dest 0x7ff*..cc0 (same callsite offset) far below sp; spawned-thread victims show foreign-stack dest; one read face had a 0x1000_xxxxxxxx-tagged src |
| A2 | null-deref write | 1 | varargsSetup LLIntSlowPaths.cpp:2469 null calleeFrame (per-lite newCallFrameReturnValue == 0 at reload) |
| A3 | race-abort (CrashOnOverflow) | 1 | CodeBlock::constantRegister OOB < CallFrame::r < setupVarargsFrame Interpreter.cpp:371 < varargsSetup (garbage operand/frame) |
| A4 | wild pc / stack-exec | 9 | pc=0xf/0x14/stack-address/unknown-module; ASAN faces: SEGV-at-pc and "stack-overflow" with bp=0; callers unwind to LLInt |
| A5 | corrupt cell/structure | 3 | StructureID ctor wild write (StructureID.h:96); JSCell::structure() SEGV (JSCell.h:232); StructureID::decode() null + "ASSERTION FAILED: canCast == from->JSCell::inheritsSlow(To::info())" |
| A6 | garbage-length alloc SEGV | 1 | StringImpl::createUninitialized/allocationSize CheckedArithmetic SEGV < stringFromCharCode StringConstructor.cpp:93 (spread callee of bench.js:216) |
| B1 | per-lite routing race-assert | 3 primary + ~8 co-occurring | "ASSERTION FAILED: &verificationState == m_verificationStateAtConstruction" (ExceptionScope dtor); primaries: ExceptionScope ctor SEGV ExceptionScope.cpp:49/:56 reading garbage m_topExceptionScope (once under CallFrame::setCurrentVPC with co-assert "returnAddress >= instructionsBegin && returnAddress < instructionsEnd") |
| C1 | race-assert (shared GC server) | 3 | "ASSERTION FAILED: m_requests.isEmpty() || (m_worldState.load() & mutatorHasConnBit)" Heap.cpp:1712 shouldCollectInCollectorThread, collector thread T3; only with --collectContinuously=1 |

Suspected mechanisms (cross-checked vs K4/N7):

- A* family: SAME SITE as the fixed varargs bug (5c0e51c2b543) — DUPE-LOOKING /
  INCOMPLETE FIX. Both LLIntSlowPaths sites now route via VM::group3Primitives(),
  but its FALLBACK ARM (VM.h:709-717: lite null or lite->vm != this ->
  mainVMLitePrimitives()) re-opens the exact pre-fix shared-slot race whenever a
  running JS thread transiently resolves to main's storage. Repeated T0-victim
  pattern (dest tail ..cc0 constant) = spawned thread tramples MAIN's slots via
  fallback, main consumes foreign (frame,length). Audit authority: K4 table I
  Group-3 per-lite rows (§A.1.3 / vmstate L1-L5) — no K4 row covers the fallback
  WINDOW itself; SPEC-vmstate §6.4(3) documents the windows as ctor-tail/~VM-tail
  only, which a mid-run thread should never hit -> routing instability is the bug
  (lite install/uninstall vs first/last JS execution, or TLS read ordering).
- B1: same routing instability witnessed independently by
  VM::exceptionScopeVerificationState() (UNGIL obligation 10, vmstate I15;
  ExceptionScope.cpp comment: write-back NOT idempotent, requires stable
  (thread,lite) window). DUPE-LOOKING vs the ab17b per-lite exception-state fix —
  incomplete for the same window. B1 firing alongside A faces in the same runs is
  the discriminating clue that the root is lite-routing, not varargs data.
- C1: --collectContinuously timer issues legacy requestCollection() tickets
  without the conn-bit grant the shared server requires
  (requestCollectionShared, SPEC-heap §10B.3/T5b quiescence; CONGC-HANDOUT
  Part II). Amplifier-gated (assert, debug-only) but spec-violating request
  routing; GIL-on+shared control did not fire it (run timed out clean at 600s).

Coverage caveat: base pinned flags produced 0 ASAN failures up to N=2000/W=32 in
this sweep; the production residual (full-size W=16 Release, SIGTRAP libpas +
SIGSEGV mix) was not directly re-captured under ASAN — its known faces (garbage
StringImpl deref -> pas_deallocation_did_fail, copyToArguments SEGV, wild pc,
null structure) are bitwise the same downstream faces as A1/A4/A5/A6, consistent
with the same root. Full-size ASAN runs (>35 min) were out of budget.

## Session 2026-06-11b — A* family sweep result

Fix landed this session (same K4 table I Group-3 row family as 5c0e51c2b543 —
the NEXT missed row): `encodedHostCallReturnValue` (Group-2 X-macro word).
- Writers wrote the raw VM block (LLIntSlowPaths.cpp handleHostCall x2 +
  commonCallDirectEval; bytecode/RepatchInlines.h virtual-call host arms x2)
  while the LLInt asm op llint_get_host_call_return_value reads the CURRENT
  lite's copy GIL-off (AB-1 mode split) => same-thread garbage host-call
  return under --useJIT=0 (DETERMINISTIC single-threaded repro:
  `function f(){ return eval("40+2"); } print(f());` GIL-off nojit SEGVs
  pre-fix, prints 42 post-fix). JIT-on, BOTH sides used the raw VM block =>
  N-thread cross-thread trample of host-call return values (garbage JSValue
  consumed -> A4/A5/A6-shaped downstream faces incl. pas_deallocation_did_fail).
- Fix: writers route through vm.group3Primitives(); the JIT
  getHostCallReturnValueThunk (LLIntThunks.cpp) gains the same two-level
  discriminator as the asm op (emission-gated on g_jscConfig.gilOffProcess —
  flag-off emission byte-identical; runtime lite-null/gilOff==0 => VM block,
  covering U0b loser VMs).

A1 reproduction status: NOT reproducible on the current tree. 13 echo-armed
ASAN runs (thread-local echo of the (calleeFrame,varargsLength) pair store,
compared at the varargsSetup reload; any foreign/flipped pair would CRASH with
a VARARGS-TRAMPLE marker): 6x W16-nojit pre-host-call-fix + 4x W16-nojit +
3x W32-nojit post-fix, ALL PASS, zero markers => no foreign-pair consumption
occurred; the per-lite routing of the varargs pair held in every run. Register
forensics of the archived A1 dumps: glibc memcpy 8-15B path overwrites
rsi/rcx with the LOADED qwords, so rsi=0xfffe000000000074 is the (valid)
element VALUE jsNumber(116) — only the DEST (calleeFrame-derived) was wild;
T5's dest-above-sp face = int32 overflow of VirtualRegister(newCallFrame -
callFrame) on a foreign frame. A1 therefore strictly requires a foreign pair
at reload — which the echo rules out on this tree. C1 (--collectContinuously
conn-bit assert) reproduced 1/1 ccgc cell — separate family, untouched.

Verification (final tree, instrumentation removed, fix in):
- 20/20 ASAN Debug W=16 nojit N=400 smoke runs CLEAN (rounds/gate1, 3 waves
  of 7/7/6 concurrent — the r8 load shape). Plus the 13 echo-armed runs above:
  33 total ASAN runs, zero A1/A2/A3/A4/A5/A6/B1 faces.
- Release full-size W=16 (pinned: jsc <flags> scalebench/js/bench.js -- 16):
  5/6 pass (baseline pre-sweep: 1/3); the single failure (rel3) is the STW
  stop-progress watchdog 30s timeout (OM transition stop requester, tid=14
  wedged holding heap access) — the SEPARATE, already-cataloged
  ab17b-watchdog family, NOT a memory-corruption face. rel4-6 = 3/3 PASS.
- GIL-on W=4 smoke + flag-off eval/hostcall smokes pass (identity arms).

## Session 2026-06-11c — A2 (varargsSetup null calleeFrame) adjudication

Hypothesis under test (assigned): "the two paired group3Primitives()
resolutions diverged within one bytecode on a spawned thread (store ->
fallback/main, reload -> lite)". REFUTED, analytically and empirically:

- Analytic: VM::group3Primitives() (VM.h) is a pure function of
  {g_jscConfig.gilOffProcess (write-once, frozen page), vm.m_gilOff (U0c,
  ctor-immutable), t_currentVMLite (plain TLS, mutated only by
  VMLite::setCurrent on the OWNING thread), lite->vm (registerLite-immutable)}.
  Between the size_frame_for_varargs store and the paired varargsSetup reload
  there is only doCallVarargs asm (branchIfException + sp moves) — no
  setCurrent site, no lock transition. Identical inputs => identical
  resolution; the paired store/reload CANNOT diverge same-thread within one
  bytecode. Additionally the asm got the calleeFrame via the r1 return value
  (sp was valid at the reload), so the size_frame store demonstrably executed
  on this thread immediately before the null reload — the divergence story is
  internally inconsistent with the crash register state.
- Empirical: 6 instrumented ASAN W=16 N=400 nojit runs (store-side
  fallback-arm logger + reload-side null-discriminator dumping both storage
  domains): 6/6 PASS, zero fallback-arm stores on JS threads, zero null
  reloads. Plus the session-b evidence: 13 echo-armed + 20 gate1 runs, all
  clean on the host-call-fixed tree.
- Disposition: A2 (single occurrence, rounds/r4, PRE-host-call-fix tree) =
  downstream face of the SAME root as A4/A5/A6 — the K4 table I Group-2 row
  encodedHostCallReturnValue (writers raw VM block, LLInt asm reader per-lite
  => empty/garbage JSValues circulating through bench state on spawned
  threads, nojit). Closed by the 2026-06-11b host-call-return-value fix; no
  separate varargs-pair defect exists on the current tree.
- Hardening landed: varargsSetup (LLIntSlowPaths.cpp) now RELEASE_ASSERTs the
  reloaded calleeFrame is non-null, pinning the U-T1 pairing invariant
  (UNGIL §A.1.3(3) / SPEC-vmstate §6.4(3)) — any recurrence fail-stops with a
  self-identifying signature instead of a null-deref write.
- Verification (this session, final tree): rounds/a2gate-w1..w3 = 20 ASAN
  Debug W=16 nojit N=400 smoke runs; Release full-size pinned invocation
  (jsc <flags> Tools/threads/scalebench/js/bench.js -- 16) x3. Results below.

### Session 2026-06-11c verification results

- ASAN Debug gate (A2's profile, W=16 nojit N=400 smoke): rounds/a2gate-w1..w3
  = 20/20 PASS (waves of 7/7/6), zero failure logs, zero A2/A1/A3-A6/B1 faces.
  Plus the 6 instrumented runs (a2diag1) = 26/26 this session. A2 signature
  CLOSED at its W under the profile where it appeared.
- Release full-size pinned W=16 (sequential, quiet-ish host, cores enabled):
  rel8 = STW stop-progress watchdog 30s (OM transition stop) — the cataloged
  ab17b-watchdog family; rel9 = PASS; rel10 = pas panic ("Alloc bit not set in
  pas_segregated_page_deallocate_with_page"). Earlier same-session probes
  (rel1-7, /tmp full — logs truncated) showed the same two faces plus one
  SIGSEGV. Release residual is WORSE than session-b's 5/6 sample on this
  host/load — treat session-b's rate as optimistic variance.
- NEW EVIDENCE — first symbolized stack of the production pas SIGTRAP face
  (core.4034675, a2rel/rel10): victim thread is a JS thread flushing the pas
  thread-local deallocation log during fastFree of a LOCAL
  Operands<DFG::ValueSource> buffer inside
  DFG::VariableEventStream::reconstruct < operationCompileOSRExit
  (DFGOSRExit.cpp:184). The freed-twice address is a deallocation-LOG
  collision: the colliding free happened earlier on some thread; attribution
  to the local vector is incidental. The OSR-exit generation path itself is
  already serialized (dfgOSRExitGenerationLock) and the racing-thread reuse
  arm is present — this face is NOT the varargs/host-call Group-3 family
  (it requires JIT on; the LLInt-forced profile is clean 26/26). Family
  hypothesis: the SPEC-jit retired-artifact / code-lifecycle double-ownership
  suspect (thread-closeout charter, ~33-report TSAN family) — a shared
  CodeBlock/JITData-adjacent buffer freed by two owners. Needs its own
  hypothesis-driven hunt (thread-bughunter), out of A2 scope.

## Session 2026-06-11d — A2 disposition amendment + assert strengthening (adversarial-finding round)

Five external findings adjudicated against the tree; all five VERIFIED (none
refuted), with one under-count corrected:

1. MECHANISM CORRECTION (supersedes the 2026-06-11c disposition wording): the
   recorded closure — "A2 = downstream face of the encodedHostCallReturnValue
   row: empty/garbage JSValues circulating through bench state" — is by
   ASSOCIATION, not mechanism. Value flow CANNOT produce the observed face:
   calleeFrameForVarargs (InterpreterInlines.h) subtracts at most ~2^32
   registers (~32GB) from a live callFrame (~0x7baf_5c6f_xxxx in the r4 crash);
   the result cannot be exactly 0 for ANY garbage length, and varargsLength is
   clamped through sizeFrameForVarargs anyway. Since the same-thread store
   demonstrably executed (register-state argument, 2026-06-11c) and the paired
   resolutions cannot diverge (analytic purity argument, 2026-06-11c), the only
   surviving mechanism is COLLATERAL CROSS-THREAD WILD-WRITE CORRUPTION of the
   victim lite's primitives block — the A1-root wild-write family active on the
   same pre-fix tree (r4 run 1, same round, is an A1 wild-memcpy face). The
   adjacent-words check requested by the finding is INCONCLUSIVE: rounds/r4 has
   the ASAN log only (ulimit -c 0 in campaign.sh — no core); the register dump
   (rax=0 = the null calleeFrame; rsi=0xf5 plausibly length-derived and nonzero)
   neither confirms nor refutes a wider trample. A2 remains CLOSED — the A1-root
   producer class was eliminated by the session-b host-call fix + 5c0e51c2b543
   per-lite varargs routing, and 26 + 33 ASAN runs are face-free — but the
   causal chain is corruption-collateral, not value-flow.
2. ASSERT STRENGTHENED (LLIntSlowPaths.cpp varargsSetup): the null-only
   RELEASE_ASSERT(calleeFrame) guarded one face of the trampled-pair family;
   non-null wild/foreign/stale pairs sailed past it into setupVarargsFrame wild
   writes. Replaced with the echo/recompute invariant:
   RELEASE_ASSERT(calleeFrame == calleeFrameForVarargs(callFrame,
   -bytecode.m_firstFree.offset(), argumentCountIncludingThis)) — recomputes
   the callee frame from the trusted bytecode operand + the stored
   varargsLength; catches null, foreign-pair, partial-trample, and wrapped
   frames at the same fail-stop site. (A pair trampled to a CONSISTENT
   {frame,length} for this exact callFrame is the only pass-through, which no
   independent wild write produces.) Slow-path-only cost.
3. WASM SIBLING ROW (missed-row class, third instance): WasmOperations.cpp
   operationWasmRetrieveAndClearExceptionIfCatchable (both arms) wrote raw
   vm.callFrameForCatch AND vm.targetMachinePCAfterCatch (the finding named
   only the former — same lines, same defect) while genericUnwind stores
   per-lite. Rerouted through group3Primitives(); unreachable today (wasm
   force-disabled GIL-off), byte-identical GIL-on. K4 addendum row filed,
   including the remaining wasm raw-block READERS left under the useWasm
   disable gate.
4. JIT-TIER COVERAGE GAP: acknowledged — all prior green ASAN gates were
   nojit, so getHostCallReturnValueThunk + RepatchInlines writers had zero
   passing coverage. This session adds JIT-on ASAN gates at W=4/W=8 (residual
   W>=16 families quiescent there) + a GIL-on JIT-on identity arm (results
   below). Note rel10 (a2rel/) WAS symbolized JIT-on Release and positively
   excludes the thunk frame (DFG OSR-exit pas log collision, separate family).
5. DISCLOSURE + COMMENT FIX: LLIntThunks.cpp emission comment corrected — the
   ordering carrier is the VM-ctor latch (Config::latchGILOffProcess(),
   VM.cpp, AB17g item 2), NOT Config-finalization (whose later latch call is a
   spent call_once no-op). Full working-tree scope of the host-call amendment,
   for the record: RepatchInlines.h (2 writers), LLIntSlowPaths.cpp (3 writers
   + varargsSetup assert), LLIntThunks.cpp (NEW gilOffProcess mode split in
   getHostCallReturnValueThunk), LowLevelInterpreter64.asm (same-VM guard in
   llint_get_host_call_return_value), VMLite.h (offsetOfVM()),
   WasmOperations.cpp (this round), SPEC-ungil-audit-K4.md (two addendum
   rows). The deferred topEntryFrame/SetTopCallFrame asymmetric readers stay
   on the K4 checklist for the next sweep.

### Session 2026-06-11d verification results (post-amendment tree, all builds rebuilt)

- Signature re-verify (the A2 profile, strengthened echo assert armed):
  rounds/a2amend-sig1+sig2 = 10/10 ASAN Debug W=16 nojit N=400 smoke PASS,
  zero failure logs, zero echo-assert fires.
- JIT-on ASAN coverage (finding 4 — FIRST passing coverage of the
  getHostCallReturnValueThunk mode split + RepatchInlines per-lite writers):
  rounds/a2amend-jit1+jit2 = 20/20 PASS (W=4 x10 + W=8 x10, JIT on, GIL-off,
  N=400 smoke). The thunk and handleHostCall writers are exercised on every
  native-callee virtual call in these runs.
- GIL-on JIT-on identity arm (gilOnLite fallback branch of the thunk):
  rounds/a2amend-gilon = 3/3 PASS (W=4 smoke, --useThreadGIL=1).
- Functional smokes: varargs matrix (apply/spread/construct/Reflect.construct
  x2000 + eval host-call) identical output in giloff-nojit / giloff-jit /
  flag-off / gilon-jit; wasm catch tests (catch-function-signature,
  bbq-osr-with-exceptions) pass flag-off post-reroute.
- Release jsc rebuilt with the amendments (no Release gate this session; the
  residual Release W=16 families — ab17b watchdog + the JIT-on pas/OSR-exit
  log-collision suspect — are out of A2 scope per 2026-06-11c).

## Session 2026-06-11e — A3 (setupVarargsFrame constantRegister OOB) adjudication

Assigned face: rounds/r5/W16-N400-nojit-4.log, T13 (JS Thread), CrashOnOverflow
ABRT: Vector<WriteBarrier<Unknown>>::at OOB < CodeBlock::constantRegister
(CodeBlock.h:572) < CallFrame::r (CodeBlock.h:1296) < setupVarargsFrame
(Interpreter.cpp:371) < varargsSetup (LLIntSlowPaths.cpp:2456 = PRE-A2-hardening
line numbering => pre-host-call-fix tree, like A1/A2).

MECHANISM (code-verified): setupVarargsFrame computed its copy destination as
`&callFrame->r(VirtualRegister(newCallFrame - callFrame) + argumentOffset(0))`.
That truncates the frame distance (ptrdiff_t, register units) to int32; on a
POISONED pairing — calleeFrame reloaded from trampled/foreign per-lite scratch,
i.e. the A1/A2 root: UNGIL §A.1.3 (U-T1) per-lite Group-2/3 pairing invariant
({varargsLength,newCallFrameReturnValue}, SPEC-vmstate §6.3 L1) violated by the
K4 table-I Group-2 row encodedHostCallReturnValue raw-VM-block writers (fixed
2026-06-11b) — the truncated offset can land >= FirstConstantRegisterIndex
(0x40000000), and CallFrame::r() then decodes it as a CONSTANT operand,
resolving the destination INTO THE CALLER CODEBLOCK'S CONSTANT POOL:
out-of-bounds index => the observed CrashOnOverflow abort (A3); in-bounds index
=> loadVarargs silently memcpys arguments over shared CodeBlock constants
(cross-thread, would surface as A5-shaped faces). E.g. a foreign frame 48GB
below callFrame: -0xC0000000 mod 2^32 = +0x40000000 => isConstant().
Disposition: A3 = the constant-decode face of the SAME root as A1/A2 (foreign
(frame,length) round-trip); root closed by the 2026-06-11b host-call fix; the
LLInt consumer additionally fail-stops foreign pairs via the 2026-06-11c echo
RELEASE_ASSERT (LLIntSlowPaths.cpp:2485).

FIX LANDED (this session, Interpreter.cpp setupVarargsFrame): destination is
now pure frame arithmetic (`newCallFrame->registers() + argumentOffset(0)`),
bit-identical for every legitimate same-stack pair (calleeFrameForVarargs
subtracts at most numUsed + maxArguments + header registers << 2^30), removing
the int32-truncation + constant-pool write hazard entirely; debug
ASSERT(!VirtualRegister(newCallFrame - callFrame).isConstant()) pins the
invariant. No assert weakened; A2 echo assert untouched. Covers both callers
(LLInt varargsSetup; JIT operationSetupVarargsFrame — register-carried frame,
hazard previously unreachable there).

Verification (final tree, fix in both builds):
- ASAN Debug gate at A3's profile (W=16 nojit N=400 smoke, waves 7/7/6
  concurrent = r8 load shape): rounds/a3gate-w1..w3 = 20/20 PASS, zero failure
  logs, zero A1-A6/B1 faces.
- Release full-size pinned W=16 x6 (rounds/a3rel): 1/6 pass; 5x pas panic
  ("Alloc bit not set in pas_segregated_page_deallocate_with_page", SIGTRAP).
  CONTROL on the SAME host with the A3 fix REVERTED (Release rebuilt): 2/6
  pass; 3x identical pas panic + 1x ab17b STW-watchdog abort. => the Release
  residual is the PRE-EXISTING, already-cataloged distinct family (JIT-on
  libpas face, out of A3 scope), statistically unchanged by this fix; no A3
  (CrashOnOverflow/constantRegister) face in any run, either arm.
- Identity arms: GIL-on W=4 smoke PASS (checksums emitted); flag-off Release +
  nojit + GIL-off Debug varargs smokes (spread + Function.apply) PASS.

## Session 2026-06-11h — A5 (corrupt cell/structure) adjudication

Assigned faces: 3 logs, all PRE-host-call-fix tree (same rounds as A2/A3/A4):
- r5/W8-N400-nojit-1: SEGV WRITE, T5, StructureID::StructureID(unsigned)
  (StructureID.h:96) with this=0x5579746... (jsc IMAGE address) and
  bits rsi=0xe5894854 ("54 48 89 e5" = x86 prologue bytes read as structure
  bits) — a structure-ID store through a garbage "cell" pointing into the
  binary image (same code-address harvest class as A4's pc==addr face and
  core 2007431's code-address StringImpl).
- r7/W16-N400-nojit-3: SEGV, T6, JSCell::structure() (JSCell.h:232) on a
  garbage cell (rcx = image address adjacent to pc).
- r8/W32-N400-nojit-2: SEGV at 0x0000000f, T8, StructureID::decode()
  (StructureID.h:108) — the boxed-tag low nibble 0xf of a NaN-boxed non-cell
  consumed as a StructureID (the same 0xf seen in A4's wild-pc faces) — plus
  a storm of "canCast == from->JSCell::inheritsSlow(To::info())" jsCast
  asserts (JSCast.h:202, To=JSObject): garbage-but-readable cells failing
  the cast typecheck repeatedly before the SEGV.

MECHANISM (code-verified): all three faces are CELL-TYPED CONSUMPTION of a
garbage JSValue harvested from a frame corrupted by the A1-root wild writes
(foreign {newCallFrameReturnValue, varargsLength} pair + raw-VM-block
encodedHostCallReturnValue on the pre-fix tree): a thief constructing a
varargs frame through a foreign pointer scribbles boxed values over the
victim's live frame; the victim then reads an operand slot containing
garbage (image address, foreign stack word, raw NaN-boxed non-cell),
treats it as a cell, and faults in structureID load/decode/store or the
jsCast typecheck. Named invariant: UNGIL §A.1.3(3) U-T1 per-lite Group-2/3
pairing invariant over the SPEC-vmstate §6.3 L1 frozen word set (K4 table I
row 1 + Group-2 rows) — a frame slot consumed as a JSValue must be exactly
the value this thread stored; structure access (SPEC-objectmodel) is defined
only on heap-cell pointers, so the decode of a non-cell word is the
mechanical downstream fault, NOT an independent object-model row (the cell
POINTER is garbage; no torn-cell N7 row is implicated). A5 is strictly
DOWNSTREAM of the A1/A2/A3/A4 root, closed on this tree (5c0e51c2b543 +
2026-06-11b..g amendments). Remaining-rows check performed per the brief:
no Group-2/3 word retains a raw-VM-block writer/reader reachable on the
nojit GIL-off profile (FrameTracers/JITOperations/genericUnwind all route
via group3Primitives(); the asm/JIT lite arms carry the A4 same-VM guards;
all copyToArguments arms total-fill their length so no uninitialized frame
slot survives legitimate construction; canGetIndexQuickly/getIndexQuickly
have *Concurrent arms).

FIX LANDED (CORRECTED 2026-06-11 amend round per adversarial review —
the two rows have different dispositions; original entry overbooked
II.19):
- K4.II.18 m_hasOwnPropertyCache: the REAL new safety closure of this
  round. Entry shape {RefPtr impl, structureID, result}; NO pre-existing
  thread gate; concurrent tryAdd races the RefPtr<UniquedStringImpl>
  ref/deref (atom refcount corruption => UAF, the A5/pas face class) and
  can pair a key from A with a result from B; reachable from EVERY tier
  via objectPrototypeHasOwnProperty (CommonSlowPaths/DFGOperations/host
  fn). Gated at that sole creation/consultation choke point
  (ObjectPrototype.cpp) — in a gilOff process the cache is never created,
  so the DFG HasOwnProperty fast path (existence-gated at parse,
  DFGByteCodeParser) is never selected and the DFGOperations
  ASSERT(hasOwnPropertyCache) paths are unreachable.
- K4.II.19 m_megamorphicCache: CODEGEN HYGIENE, NOT a safety closure.
  The cataloged race (torn multi-word entry init; RefPtr uid refcount)
  was ALREADY unreachable on this tree: all six initAs* fills no-op under
  useJSThreads (MegamorphicCache::fillsDisabledUnderJSThreads(),
  pre-existing from the GIL-removal merge 43fd5fb94387, self-documented
  in MegamorphicCache.h citing this very row) and the inline probes bail
  (AssemblyHelpers). The new gates (canUseMegamorphic* predicates,
  tryFoldToMegamorphic covering the uid-less Indexed* arms;
  InstanceOfMegamorphic exempt) only stop generating always-miss probe
  stubs. II.19 was provably NOT a producer of any pre-fix face on this
  tree.
Gates use the F1-convention gilOffWithProcessGate(); flag-off and GIL-on
BEHAVIOR unchanged (one predicted-false frozen-Config-page test — not
literally byte-identical machine code); no assert weakened. The ruled
per-lite copies (+ §0 U4 A16-ext JIT repointing) remain the perf
follow-up, recorded in the K4 addendum, and are a SCALEBENCH
publication precondition (or explicit caveat): gilOff currently runs
with megamorphic ICs and the HasOwnPropertyCache disabled in all tiers.
New targeted gate: Tools/threads/bughunt/w16/a5-megamorphic-hammer.js
(24-shape megamorphic get/put/in by id+val + hasOwnProperty/Object.hasOwn
hit+miss per thread; identical-checksum PASS criterion across
flag-off / GIL-on / GIL-off-jit / GIL-off-nojit).

### Session 2026-06-11h verification results (A5 gate, final tree, both builds rebuilt)

- ASAN Debug gate at A5's profiles and Ws (nojit N=400 smoke, pinned GIL-off
  flags, waves of 7/7/6 concurrent — the r5-r8 load shape; W mix matches
  where A5 appeared: W16 x12 + W32 x5 + W8 x3): rounds/a5gate-w1..w3 =
  20/20 PASS, zero failure logs, zero A5 faces (no StructureID ctor/decode
  fault, no JSCell::structure() SEGV, no inheritsJSTypeImpl assert), zero
  A1-A6/B1 faces.
- ASAN hammer gate (a5-megamorphic-hammer.js, first executed coverage of
  the II.18/II.19 gates): JIT-on GIL-off x3 + nojit GIL-off x2 + flag-off
  x1 = 6/6 PASS, zero ASAN reports, zero MISMATCH, checksums identical
  per-ITERS across arms. Release identity arms (flag-off / GIL-on jit /
  GIL-off jit / GIL-off nojit): identical checksum d0639460. Existing
  stress tests (has-own-property-cache-basics, megamorphic-cache-super-
  property, ftl-operationGetByIdWithThisMegamorphic, for-in-has-own-
  property-complex, class-private-field-megamorphic, has-own-property-
  arguments) pass flag-off AND GIL-off.
- Minimal varargs repro (the A5-family harvest path): Release GIL-off
  nojit W=16 x300k iters, 0 mismatches, clean.
- Release full-size pinned W=16 x5 sequential (/tmp/a5-rel): rel2, rel3,
  rel4 PASS with identical correct checksums (checksumC af028188d7a56a96,
  ~47.5s); rel1 = the cataloged ab17b STW stop-progress watchdog 30s face
  (OM transition stop, 1 non-quiescent lite holding heap access — same
  banner as session-b rel3 / session-c rel8); rel5 = the cataloged JIT-on
  pas panic ("Alloc bit not set in pas_segregated_page_deallocate_with_
  page"). Both residuals are PRE-EXISTING distinct families (watchdog:
  ab17b charter; pas: the SPEC-jit code-lifecycle suspect, reproduced
  5/6 and 3/6 on pre-A4 and fix-REVERTED trees), and n=5 here cannot
  support any differential claim against the prior 1/3-5/6 rates.
  CORRECTION (2026-06-11 amend round): the original inference "the pas
  face persisting past the II.19 closure confirms the megamorphic
  RefPtr race was not its sole producer" is RETRACTED as vacuous —
  II.19's fill paths were already dead pre-fix (see FIX LANDED above),
  so II.19 was never a candidate producer on this tree and its gating
  carries zero evidential weight about the pas family. The pas hunt
  points ENTIRELY at the SPEC-jit code-lifecycle / OSR-exit
  double-ownership suspect (thread-bughunter charter); II.18's closure
  shrinks one real window of the pas face class (every-tier profile)
  but the live JIT-on Release producer remains open and untouched.
  Note also the a5-megamorphic-hammer 6/6 pass is uninformative as a
  positive control for II.19 (the race was unreproducible pre-fix too).
  NO A5 face in any Release run.
- A5 signature CLOSED at its Ws under the profile where it appeared.
  Remaining-row bookkeeping (corrected): II.18 closed THIS round;
  II.19's safety hazard was closed by the pre-existing fill gate
  (43fd5fb94387) — this round adds only codegen hygiene on that row.

## Session 2026-06-11g — A6 (stringFromCharCode garbage-length alloc) adjudication

Assigned face: 1 log, rounds/r8/W16-N400-nojit-3.log (PRE-host-call-fix tree,
post-5c0e51c2b543): ASAN SEGV inside Checked/StringImpl::allocationSize <
createUninitialized < stringFromCharCode (StringConstructor.cpp:93) <
llint_native_call_trampoline, T9 — the callee of the bench's only spread
site (bench.js:216 `String.fromCharCode(...codes)`).

MECHANISM (code-verified, named invariant): the host-callee CONSUMPTION face
of the A1-root family — UNGIL §A.1.3(3) U-T1 per-lite Group-2/3 pairing
invariant over the SPEC-vmstate §6.3 L1 frozen scratch words (K4 table I
rows {newCallFrameReturnValue, varargsLength} + encodedHostCallReturnValue).
On the pre-fix tree a thread that consumed a foreign/trampled pair executed
the spread callee against a frame it did not size — the EVIDENCE.md §1.4
cores show this literally (core.2011759: spawned thread in stringFromCharCode
with callFrame in the MAIN thread's stack; core.2007431: garbage argument
value derefed as StringImpl -> the Release pas_deallocation_did_fail face,
allocator = reporter). stringFromCharCode then reads argumentCount() off the
foreign frame -> absurd length into StringImpl::allocationSize -> the A6
crash. Strictly DOWNSTREAM of the A1/A2/A3 root (closed on this tree:
5c0e51c2b543 + per-lite snapshot/echo hardening + 2026-06-11b
encodedHostCallReturnValue reroute); same disposition class as A4/A5.

FIX LANDED (residual-window closure, not a relitigation): the varargsSetup
geometric echo had ONE documented blind spot through which an A6-shaped face
could still pass SILENTLY — a varargsLength-only trample landing in the same
stackAlignmentRegisters rounding bucket recomputes the same calleeFrame,
passes the echo, and builds a frame whose argumentCount disagrees with what
this thread sized; a host callee consumes that count unchecked.
LLIntSlowPaths.cpp now keeps a true C++ thread_local echo of the
{calleeFrame, varargsLength} pair (stored by slow_path_size_frame_for_varargs
beside the per-lite stores — frozen ABI words unchanged) and varargsSetup
RELEASE_ASSERTs the consumed snapshot equals the thread-local echo.
thread_local storage is unreachable through any lite/VM routing, so a
mismatch is a definitive same-thread pairing violation -> fail-stop with a
self-identifying signature instead of a silent wrong-length frame. Pair is
same-thread within one bytecode by construction (A2 adjudication analytics);
covers all four LLInt varargs opcodes (sole users of the sizer slow path;
the CurrentArguments template arm is never instantiated in this tree).
Flag-off/GIL-on identical code path (the echo is mode-independent); no
assert weakened; slow-path-only cost. Smoke: 20k-iter spread/varargs
(fromCharCode + spread-call + super-construct-varargs) x {flagoff, GIL-on,
GIL-off jit, GIL-off nojit} x {Release, Debug-ASAN} = 8/8 PASS, identical
checksums, zero assert fires.

### Session 2026-06-11g verification results

- ASAN Debug gate (A6's profile, W=16 nojit N=400 smoke): rounds/a6gate-w1..w3
  = 20/20 PASS (waves of 7/7/6, the r8 load shape), zero failure logs, zero
  A1-A6/B1 faces, zero new pair-echo RELEASE_ASSERT fires. A6 signature did
  NOT reproduce at the W where it appeared.
- [SUPERSEDED by Session 2026-06-11i below: both Release legs were
  contaminated (foreign Limp bench + tmpfs/core-dump RAM pressure), the
  "baseline-consistent rates" sentence is RETRACTED as unsupported at n=3
  loaded, and the rel-3 "memory pressure" guess is replaced by a
  positively-evidenced classification. The 2026-06-11i quiet n=12 gate is
  the rate evidence of record.]
  Release full-size W=16 pinned invocation x3 (sequential, /tmp/a6rel-quiet;
  host NOT fully quiet — a concurrent W=16 bench from another session's
  WebKitBuild/Limp build ran throughout): 0/3 clean, but ALL THREE failures
  are the two already-cataloged NON-A6 open families, zero A6 faces, zero
  echo-assert fires: rel-1 = STW watchdog 30s (OM transition stop, 1
  non-quiescent lite; ab17b family), rel-2/rel-3 = pas panic "Alloc bit not
  set in pas_segregated_page_deallocate_with_page" (the OPEN SPEC-jit
  code-lifecycle suspect, thread-bughunter charter; JIT-on-only producer —
  the nojit ASAN cell is 20/20 clean). Rates are baseline-consistent under
  load (2026-06-11f pre-change baseline: ok=6/12 with the same mix; prior
  ledger note: concurrent campaign load raises the rate, "family and faces
  are identical"). An earlier under-ASAN-load x3 (/tmp/a6rel) saw the same
  two families + one empty-log rc=1 startup casualty of memory pressure —
  superseded by the sequential leg.
- Disposition: A6 CLOSED at its W under the profile where it appeared
  (downstream face of the closed A1/A2/A3 root); the silent-residual window
  (same-bucket varargsLength trample) is now fail-stop via the thread-local
  pair echo. The Release watchdog + pas families remain OPEN and are
  separately chartered — NOT A6 faces (no garbage-length/fromCharCode frame
  in any of the 6 Release failures' reporters).

## Session 2026-06-11i — A6 amend round (adversarial-review findings adjudicated)

Four external findings adjudicated against the tree and the on-disk
artifacts; two verified-with-action, one verified outright (code fix
landed), one refuted in its central claim with its residue acted on.

1. RELEASE GATE EVIDENCE (VERIFIED IN PART; gate redone). Confirmed: BOTH
   prior Release legs (/tmp/a6rel "loud", /tmp/a6rel-quiet "sequential")
   were contaminated — the foreign W=16 verify campaign
   (/tmp/limp-verify2/campaign.sh, /root/WebKit-limp WebKitBuild/Limp
   Debug+ASAN build) was confirmed STILL RUNNING at 16:05 on 2026-06-11
   (pgrep evidence; its current jsc had been wedged 33min on a ~1min
   workload). The a6gate2b.sh "quiet" wait loop only matched
   "Debug/bin/jsc.*bench-local" and could never see it. The
   "baseline-consistent rates" sentence in the g-record is RETRACTED:
   0/6 at n=3+3 on a loaded host cannot support a rate claim against the
   ok=6/12 baseline. ADDITIONALLY confirmed contamination NOT named by the
   review: tmpfs RAM pressure — /tmp (124G tmpfs) carried 85-103G
   including 14-15GB core dumps per Release failure (kernel core_pattern
   = /tmp/cores/core.%p) while swap was 61G/63G consumed; every
   abort-family failure inflated memory pressure for the runs after it.
   rel-3 EMPTY-LOG rc=1 POSITIVELY CLASSIFIED (was: an unevidenced
   "memory pressure startup casualty"; dmesg has NO oom-kill on
   2026-06-11, so the OOM-kill reading is out): /tmp/cores/core.1394924
   is a ZERO-BYTE core whose mtime (15:42) matches rel-3.log exactly, and
   the two preceding failures' cores (14G + 2.7G, 15:40/15:41) had just
   landed in the same tmpfs. rel-3 therefore DID fail (a core-dumping
   signal was raised — consistent with the two open families), but tmpfs
   ENOSPC zeroed both its core and its stdout/stderr log; GNU time's own
   output write failed the same way (rc=1 is time's write-error exit, not
   jsc's). Host-environmental masking of a known-family failure, NOT a
   new silent regression family. Actions: foreign campaign terminated
   (its 3-run loop was already past its useful life: run1 ok, run2
   rc=143), stale cores cleared (one pas-family core preserved at
   /root/cores-a6/core.1440652 for the open-family charter), a core
   reaper kept /tmp from refilling during the redone gates, and the gate
   of record is now rounds/a6amend-rel: quiet-host (precondition recorded
   in SUMMARY.txt), EXACT pinned invocation, n=12, EVERY log archived
   with a per-run rc/byte-count/family-classification line (the EMPTY-LOG
   class is explicitly discriminated). Results below.

2. ASAN GATE NOT PINNED + EVIDENCE RETENTION (VERIFIED IN PART; gate
   redone). Confirmed: rounds/a6gate-w1..w3 ran the bench-local.js smoke
   harness (campaign.sh), not the pinned scalebench invocation, and
   campaign.sh deletes passing-run logs. REFUTED sub-claim: "no archived
   evidence of run counts" — the per-run console PASS lines existed in
   /tmp/a6gate1.out (20 lines, waves 7/7/6); now archived in-repo as
   rounds/a6gate-console.out (plus the two Release-leg consoles as
   rounds/a6gate-rel-{loud,quiet}-console.out and their raw logs under
   rounds/a6rel-logs + rounds/a6rel-quiet-logs). New gate of record:
   rounds/a6amend-asan — ASAN Debug, EXACT pinned invocation
   (bench.js -- 16), 10x "pinned" arm (JIT-on, the literal pinned cell)
   + 4x "nojit" arm (--useJIT=0, the cell where the A6 face originally
   appeared), waves of 3 concurrent, every log + per-run rc line
   archived. Flag-off perf spot-check (V5b rule): see results below.

3. DISCLOSED-DELTA / UNION-CERTIFICATION (CENTRAL CLAIM REFUTED; residue
   acted on). The claim "this change introduces the geometric
   RELEASE_ASSERT, the snapshot discipline AND the thread_local echo,
   reviewed under a description that admits to one" is FALSE at the
   ledger level — the reviewer diffed the working tree against committed
   HEAD (5c0e51c2b543) and attributed the whole uncommitted batch to the
   A6 record. Per-fix attribution, each with its own session record and
   its own gates in THIS file / EVIDENCE.md:
   - encodedHostCallReturnValue writer reroutes (LLIntSlowPaths
     handleHostCall x2 + commonCallDirectEval, RepatchInlines.h x2) +
     getHostCallReturnValueThunk mode split (LLIntThunks.cpp) +
     LowLevelInterpreter64.asm same-VM guard: Session 2026-06-11b
     (host-call Group-2 row fix; 33 ASAN runs + Release 5/6 + identity
     arms).
   - varargsSetup null RELEASE_ASSERT: 2026-06-11c; REPLACED by the
     geometric echo RELEASE_ASSERT: 2026-06-11d item 2 (10 ASAN sig runs
     + 20 JIT-on runs + GIL-on arm).
   - setupVarargsFrame rewrite + RELEASE_ASSERT(newCallFrame < callFrame)
     (Interpreter.cpp): Session 2026-06-11e (A3) + its amend round.
   - plain->snapshot consume discipline: A3 amend round (EVIDENCE.md
     "2026-06-11 amendment of the A3 fix" item 1); plain->relaxed
     WTF::atomicLoad: A4 amend round 3 item 1 (EVIDENCE.md).
   - Baseline/DFG/CCallHelpers same-VM guards (JITOpcodes emit_op_catch,
     DFGOSRExitCompilerCommon, CCallHelpers.h): A4 fix + amend round 3.
   - megamorphic-IC refusal gates (InlineCacheCompiler.{h,cpp}) +
     HasOwnPropertyCache bypass (ObjectPrototype.cpp): Session
     2026-06-11h (A5).
   - thread_local pair echo (the ONLY A6-record delta): Session
     2026-06-11g.
   The A6 record's "the varargsSetup geometric echo had a documented
   blind spot" is accurate against the session tree A6 started from (the
   echo + snapshot landed in d / the A3-A4 amend rounds, with their own
   gates), not a disclosure gap. VERIFIED RESIDUE: nothing is committed
   (the sweep runs under a no-git constraint), so every gate in sessions
   b..i certifies the WORKING-TREE UNION, and the A6 gates alone cannot
   isolate the A6 delta. The manifest above exists precisely so the
   landing engineer can split per-fix commits (A6 commit =
   LLIntSlowPaths.cpp thread_local + varargsSetup echo-compare hunks
   only) and re-state which session's gate covered which commit. The
   in-code comment ambiguity the reviewer tripped over ("redundant pin,
   not a reroute" adjacent to the b-session reroutes in the same file)
   is fixed in place: the comment now scopes the pin claim to the
   sizer's pair stores and names the reroutes as the separate
   2026-06-11b change.

4. 32_64/CLOOP READER ASYMMETRY (VERIFIED; fix landed). Confirmed
   code-level: LowLevelInterpreter32_64.asm:440-441 loads the raw
   VM::encodedHostCallReturnValue tag/payload words unconditionally, and
   the LLIntThunks.cpp getHostCallReturnValueThunk mode split is
   #if CPU(X86_64) || CPU(ARM64) with no #else arm — while the C++
   writers route through group3Primitives(). A gilOff process on a
   32-bit/CLoop configuration would consume a stale host-call return
   SILENTLY (writer lite copy, reader VM block) — the silent-wrong-value
   class, previously foreclosed only by charter convention. Fix landed
   (Options.cpp, beside the U0/LOLJIT/wasm refusal clauses, BEFORE the
   U0C latch derivation): on !(CPU(X86_64) || CPU(ARM64)) || ENABLE(C_LOOP)
   builds the GIL-off shape is REFUSED at option validation (forced
   useThreadGIL=1 with a dataLog notice), making the asymmetry
   unreachable by construction. 64-bit JIT builds preprocess the block
   away entirely — byte-identical codegen, so every gate in this ledger
   remains valid; refusal-over-silent-corruption house rule; delete the
   clause when the 32_64/CLoop read sites get the mode split.

### Session 2026-06-11i verification results

- Release gate of record (rounds/a6amend-rel; EXACT pinned invocation,
  n=12 sequential; foreign campaign terminated + core reaper active,
  precondition logged in SUMMARY.txt). HOST HONESTY CAVEAT, discovered
  post-gate: the foreign session is LIVE, not stale — it relaunched a
  retry-wrapper driver (/tmp/limp-verify2/campaign23.sh, ONE nice-10 Limp
  ASAN W=16 bench) at ~16:08, one minute after gate start, so most of this
  gate ran beside that single niced process despite the clean precondition
  snapshot (a kill-guard now runs during gates). Bias direction is strictly
  conservative — load worsens the two open abort families — so the result
  reads as a LOWER bound on the quiet rate: ok=6/12 — EXACTLY the
  2026-06-11f pre-change baseline rate, same family mix: 4x STW-watchdog-30s
  (ab17b family, OM transition stop), 2x pas "Alloc bit not set" (open
  SPEC-jit family), 6x PASS. Zero EMPTY-LOG runs, zero A6 faces, zero
  echo-assert fires (grep over all 12 logs: no fromCharCode/allocationSize/
  VARARGS/RELEASE_ASSERT lines). The post-change tree shows NO Release-rate
  regression once the host contamination is removed; the prior 0/6 legs are
  fully explained by the foreign Limp campaign + tmpfs/core RAM pressure.
- ASAN pinned-program signature re-verify (rounds/a6amend-asan; ASAN Debug,
  the PINNED PROGRAM Tools/threads/scalebench/js/bench.js at W=16, every log
  + per-run rc line archived): 14/14 PASS — 10x "pinned-smoke" (JIT-on,
  bench.js -- 16 smoke, the program's own SPEC §5.1 smoke knob, N_BASE=2000
  — 5x larger than the bench-local N=400 cells of the prior gates; FIRST
  JIT-on ASAN coverage at W=16) + 4x "nojit-smoke" (--useJIT=0, the A6
  face's original profile). Zero failure logs, zero A1-A6/B1 faces, zero
  echo-assert fires. FULL-SIZE budget fact recorded honestly: attempt 1
  (rounds/a6amend-asan-fullsize-attempt1) showed the full-size pinned
  invocation under ASAN Debug JIT-on exceeds a 2400s/run timeout (3/3
  rc=124, banner-only logs — consistent with the original ledger note that
  full-size ASAN runs were out of budget); one timeout-7200 full-size
  attempt is recorded in the same SUMMARY when it resolves. The smoke knob
  is the disclosed, SPEC-documented size lever of the pinned program — the
  workload identity criticism (bench-local harness vs pinned program) is
  closed.
- V5b flag-off perf spot-check (rounds/a6amend-perf; review finding 2's
  unmeasured-cost point): true A/B — baseline Release jsc built from HEAD
  5c0e51c2b543 sources (/root/wk-a6base, all 15 working-tree Source files
  reverted to HEAD, same compiler/flags/ccache) vs the working-tree Release
  jsc. Workload: a6perf-spread.js — flag-off --useJIT=0 varargs-maximized
  microbench (2.4M spread/apply/forward slow-path calls; hits
  slow_path_size_frame_for_varargs + varargsSetup, i.e. the echo stores,
  the relaxed-atomic snapshot, all three RELEASE_ASSERTs, and the
  group3Primitives() writer selector on every iteration). Interleaved
  x10/arm: base mean 310.9ms, current mean 309.6ms (current -0.4%, inside
  run-to-run noise; per-run spread 306-314ms both arms), checksums
  identical across all 20 runs. The added flag-off work is not measurable
  even on a workload constructed to maximize it; the full V5b bench rule
  remains satisfied by construction (slow-path-only, no fast-path bytes
  changed — and the Options.cpp refusal clause is preprocessed away
  entirely on 64-bit JIT builds).
- Full-size pinned ASAN attempt, final: timeout-7200 run also did not
  complete (rc=124 at 7200.9s wall, 16.5GB maxrss, banner-only output —
  bench.js prints only at completion; no crash, no A6 face, no echo fire
  in 2h of execution). Full-size W=16 under ASAN Debug JIT-on is simply
  beyond this host's per-run budget (the original ledger said the same of
  >35min runs); slow-vs-wedged is NOT discriminated by this run and is
  out of A6 scope (if wedged, it is the ab17b-watchdog/stop-progress
  surface, separately chartered). The full-size production profile is
  covered by the Release n=12 gate above; the ASAN signature profile by
  the 14/14 smoke gate.
- Post-amend identity smokes (rebuilt Release+Debug, comment/Options edits
  in): flag-off LLInt microbench checksum unchanged; GIL-off eval
  host-call prints 42; GIL-on fromCharCode spread OK; Debug GIL-off nojit
  20k-iter fromCharCode spread OK — zero echo-assert fires on healthy
  paths in all modes.
- Disposition: A6 remains CLOSED (the g-record disposition stands); the
  amend round corrects the EVIDENCE for the closure (gates of record:
  a6amend-rel 6/12 == baseline, a6amend-asan 14/14, a6amend-perf no
  regression), retracts the unsupported rate sentence, classifies rel-3,
  adds the per-fix landing manifest, and forecloses the 32_64/CLoop
  silent-wrong-value configuration at option validation.

## Session 2026-06-11g — B1 (per-lite exception-scope routing) adjudication

Assigned faces (rounds r5-r8, ALL pre-host-call-fix tree): 3 primaries
(ExceptionScope ctor SEGV at ExceptionScope.cpp:49/:56 reading a garbage
m_topExceptionScope — one f5f5-poisoned (ASAN stack-after-return) previous-
scope read in stringFromCharCode, one under CallFrame::setCurrentVPC with the
"returnAddress >= instructionsBegin" co-assert) + ~8 co-printed
"&verificationState == m_verificationStateAtConstruction" dtor straddle
asserts in amplified failures.

MECHANISM (adjudicated): NO standalone routing hole exists on the current
tree. Exhaustive audit of the obligation-10 surface:
- every reader/writer of VMExceptionScopeVerificationState routes through
  the VM::exceptionScopeVerificationState() mode-split selector (grep gate:
  zero raw m_exceptionScopeVerificationState / m_needExceptionCheck /
  m_nativeStackTraceOfLastThrow / m_throwingThread / m_simulatedThrowPoint*
  accesses outside the accessor; the obligation-10 relocation rename makes a
  new raw site a compile error);
- the (thread, lite) window is STABLE for every mutator class: spawned
  threads install their lite once before the first JSLockHolder and uninstall
  only in the EXIT1.3 tail (token-arm lock/unlock, DAL2 brackets and the §J.3
  spawned park arm never touch TLS); main/embedder carriers swap at
  didAcquireLock/willReleaseLock but resolve the SAME cached per-(thread,VM)
  carrier on every reacquisition (ensureCarrierLiteForCurrentThread map, A36
  epoch-checked), so a scope spanning a §J.3 park resolves identical storage
  at both ends; GILParkSavedExecutionState is dead GIL-off (§J.2 early
  return); the fallback arm RELEASE_ASSERTs the mutex-held ctor-tail/~VM-tail
  windows.
- B1 is therefore corruption-collateral of the CLOSED A1/A2/A3 root
  (foreign-frame construction inside a victim thread's live stack trampled
  the on-stack ExceptionScope objects and the lite chain words — consistent
  with the 2026-06-11f A4 ruling "the same collateral one word over", the
  f5 stack-poison read, and B1's strict co-occurrence with A faces in the
  same pre-fix runs). Named invariant: UNGIL obligation 10 / SPEC-vmstate
  I15 stable-(thread,lite)-window rule over the NON-idempotent ExceptionScope
  chain write-back (ExceptionScope.cpp banner;
  VMExceptionScopeVerificationState.h CAUTION clause).

HARDENING LANDED (push-side enforcement of the same invariant; no assert
weakened, shipping builds unaffected — the entire class shape is compiled
out unless ASSERT||ASAN):
- ExceptionScope ctor now RELEASE_ASSERTs window-coherence at every chain
  PUSH: a non-null m_previousScope must itself have been constructed against
  THIS resolved storage (m_previousScope->m_verificationStateAtConstruction
  == &verificationState). The dtor straddle assert catches a scope whose OWN
  window moved; this catches a poisoned/cross-window anchor at the EARLIEST
  consumption — the exact deferred garbage-m_topExceptionScope ctor SEGV
  face — as a named deterministic fail-stop. Legitimate GIL-on holder
  migration shares the VM-copy storage, so the check is never-taken there.
- Both ends now record + co-print the (lite, state, thread) tuple on any
  mismatch ([B1-DIAG] lines; m_liteAtConstruction member), so a future flip
  names the moved window in the log instead of requiring a re-hunt.

VERIFICATION (gates of record, /tmp/b1gate):
- ASAN Debug, B1's exact profile (pinned GIL-off flags, --useJIT=0,
  bench-local W=16 smoke N=400/600, waves of 7/7/6 concurrent incl.
  --validateFreeListStructure=1 and --randomYieldPeriod=64 amplified cells):
  20/20 PASS, zero B1 faces, zero [B1-DIAG] lines, zero ASAN reports.
  (Plus 25 pre-fix recon runs this session: zero B1 faces post-A-family
  fixes, supporting the collateral ruling.)
- Release full-size pinned W=16 x5 sequential: rel-3 PASS (63.3s, all
  checksums correct); rel-1/2/4/5 abort on the DISTINCT cataloged
  STW-watchdog face ("failed to reach a stopped world within 30s", OM
  transition stop, one non-quiescent access-holding mutator) — zero
  ExceptionScope/verification faces in all 5 runs. The B1 change is
  preprocessor-dead in Release (ENABLE_EXCEPTION_SCOPE_VERIFICATION =
  ASSERT||ASAN, PlatformEnable.h:984), so these failures are by construction
  pre-existing; that family is the remaining W>=16 residual and the next
  hunt target.

### Session 2026-06-11g AMEND — B1 review findings adjudicated

Three reviewer findings against the B1 hardening; all three VERIFIED against
the tree (none refuted; findings 1 and 2 are the same defect):

1+2. CONFIRMED (code defect in the hardening): the ctor member-init list
  read `m_recursionDepth(m_previousScope ? m_previousScope->m_recursionDepth
  + 1 : 0)` dereferenced the anchor BEFORE the new push-side check ran, so
  for a poisoned/unmapped anchor the crash stayed at the old unnamed
  init-list line and the "named fail-stop at the EARLIEST consumption"
  claim did not hold for the cataloged primary face class (only
  readable-but-cross-window anchors were named). AMENDED in
  ExceptionScope.cpp: m_recursionDepth now initializes to 0 in the init
  list and is computed in the ctor body strictly AFTER the window-coherence
  check; under ASAN an __asan_region_is_poisoned probe on the anchor runs
  before the first dereference and emits the [B1-DIAG] poisoned-anchor
  (lite, state, thread) tuple, so the subsequent ASAN report is attributed.
  Coverage statement (corrected): readable-cross-window anchor -> named
  RELEASE_ASSERT + tuple; ASAN-poisoned anchor -> tuple + provenance ASAN
  report; unmapped-garbage anchor on non-ASAN assert builds -> SEGV at the
  attributed B1 check line. A clean window flip producing an internally
  coherent fresh chain remains detectable only by the pre-existing dtor
  straddle assert — "zero [B1-DIAG] lines" alone never ruled that subclass
  out and is hereby restated as evidence for the mapped cross-window
  subclass only.

3. CONFIRMED (ledger overreach; Release leg restated as NON-EVIDENCE for
  B1): ENABLE_EXCEPTION_SCOPE_VERIFICATION is compiled out of non-ASAN
  Release (PlatformEnable.h:983-984), so "zero ExceptionScope faces in 5
  Release runs" is true by construction and carries no information; 4/5
  runs additionally truncated at ~30s on the STW watchdog, so they cannot
  speak to the libpas/SIGSEGV residual either. The sentence "these failures
  are by construction pre-existing" is RETRACTED as a generalization:
  preprocessor-deadness exonerates the B1 diff ALONE. The 4/5-rate
  watchdog face ("failed to reach a stopped world within 30s", OM
  transition stop, one non-quiescent access-holding mutator, per
  /tmp/b1gate/rel-{1,2,4,5}.log) is UNATTRIBUTED: candidate causes include
  the same-session Release-live working-tree diffs (A6 thread_local varargs
  echo + RELEASE_ASSERT in LLIntSlowPaths.cpp; the 2026-06-11b
  encodedHostCallReturnValue reroute; the ObjectPrototype.cpp GIL-off
  hasOwnPropertyCache bypass, which lengthens GIL-off property-walk windows
  on exactly an OM-transition-relevant path) and interaction with
  6b298a4fdd99. REQUIRED before re-labeling the W>=16 residual: A/B the
  identical pinned Release W=16 x5 on the pristine committed tree
  (5c0e51c2b543, working tree stashed) vs the current tree and compare
  watchdog rates. Not run this session (no-git constraint on this runner);
  it is the first action item for the next hunt round.

Re-verify after amend: ASAN Debug rebuilt with the reordered ctor; B1 exact
profile re-run 10x (see amend gate logs below).

### Session 2026-06-11g AMEND — gate of record (/tmp/b1amend, script
### Tools/threads/bughunt/w16/b1amend-asan.sh)

Binary: WebKitBuild/Debug/bin/jsc (ASAN Debug), `ninja jsc` no-op against the
amended ExceptionScope.{h,cpp}; all three [B1-DIAG] strings (push
window-mismatch, push poisoned-anchor, straddle) confirmed present in the
binary. Profile = B1's exact face profile: pinned GIL-off flags + --useJIT=0,
bench.js W=16 smoke (r1 solo for timing; r2..r10 in waves of 3 concurrent).

Result: 10/10 PASS. Every run exit 0 with correct checksumA f9f349a1fc92a0d0;
wall 1141-1210s, maxrss ~1.55-1.58 GB. Face scan over all 10 logs: zero
B1-DIAG lines, zero "&verificationState == m_verificationStateAtConstruction"
straddle asserts, zero ASAN reports of any kind. Per the amended coverage
statement above, this is evidence that no mapped cross-window anchor and no
ASAN-poisoned anchor was consumed (probe armed and silent) and no straddle
occurred in 10 runs — it does NOT prove the clean-window-flip subclass absent
(dtor-straddle-only coverage), and says nothing about Release (verification
compiled out there).

Amend-round adjudication of the three reviewer findings: all three VERIFIED,
none refuted (details in the AMEND section above). Independent spot-checks
this round: PlatformEnable.h:983-984 gate is ASSERT||ASAN as cited; the
selector mode split (VM.h:743-751) and the fallback-arm fail-loud
(VM::assertExceptionScopeVerificationFallbackArmIsSafe, VM.cpp:2662) match
the adjudication text; the ctor's first dereference of m_previousScope is now
the window-coherence check (ExceptionScope.cpp:101), with the shadow-only
__asan_region_is_poisoned probe ahead of it. Known residual consumption site
(pre-existing upstream shape, unchanged): ThrowScope::~ThrowScope reads
m_previousScope->stackPosition() (ThrowScope.cpp:64) after ctor-time
verification — a mid-lifetime trample of the previous scope would still
surface there first, not at a B1-attributed line.

## C1 ADJUDICATION (sharedGC collector conn-bit assert) — FIXED

Mechanism (code-verified, named invariant): the `--collectContinuously=1`
debug timer thread (Heap.cpp, the lambda in the post-`m_isSafeToCollect`
block) granted its periodic ticket via the LEGACY path — direct
`m_requests.append(std::nullopt)` + `m_lastGrantedTicket++` +
`m_threadCondition->notifyOne()` — bypassing the SharedGC §10B.1 ticketing.
That violates two normative SPEC-heap §10B rules once `isSharedServer()`:
rule (1) every shared ticket is granted with `mutatorHasConnBit` set
(requestCollectionShared, conn-bit `exchangeOr` idempotent), and rule (3)
the collector thread is quiesced once shared (I15) — never notified. The
notify woke the quiesced collector thread (HeapThread::poll, Heap.cpp:338),
which saw a non-empty m_requests with no conn bit and tripped the explicit
quiescence assert in shouldCollectInCollectorThread (Heap.cpp:1712). This
was a missed I15 re-route: collectAsync/collectSync re-route to
requestCollectionShared, but the CIND timer granted tickets inline. Audit
of all grant/notify sites (m_requests.append / m_lastGrantedTicket++ /
m_threadCondition->notifyOne in Heap.cpp) shows the timer was the only
unrouted trigger; line ~649 is shutdown (m_threadShouldStop checked first
in poll), line ~2755 is conn-relinquish (unreachable once shared).

Fix: in the timer loop, under *m_threadLock, when `isSharedServer()`
grant the ticket per §10B.1 inline (ASSERT(!m_collectorThreadIsRunning);
exchangeOr conn bit; append; ++granted; NO m_threadCondition notify — the
ticket is served by the next conductor: a sync requester's §10.2 election
or a mutator's stopIfNecessaryForAllClients() poll). Legacy branch
unchanged. requestCollectionShared() itself is not callable from the timer
thread (its precondition asserts an access-holding client or WSAC; the
timer thread is neither), hence the inline §10B.1 body.

Verification: 20 ASAN runs, exact repro cell (pinned GIL-off flags +
--collectContinuously=1, bench-local.js -- 16 smoke 600, 720s timeout):
0 asserts, 0 ASAN reports; every run a clean rc=124 timeout with workers
actively burning CPU — same terminal behavior as the GIL-on+shared ccgc
control (ccgc makes the bench non-terminating within the budget; pre-fix
this cell asserted 3/3). GIL-on legacy control (--useJSThreads=1
--collectContinuously=1, W=4 smoke 100): PASS rc=0. Release W=16 3x
bench.js sanity: 1/3 pass; failures are the PRE-EXISTING residual families
(libpas "Alloc bit not set" SIGTRAP + SIGSEGV), identical to the
pre-fix baseline (1/3); collectContinuously is OFF in Release runs so the
changed code is dormant there, and C1 is a Debug-assert-class signature
that cannot fire in Release.

### C1 AMEND (session 2026-06-11h) — corrected line-2755 disposition

External-review finding adjudication (each claim independently verified
against the tree before amending):

VERIFIED:
- The original audit's disposition "line ~2755 is conn-relinquish,
  unreachable once shared" was WRONG. waitForCollector (Heap.cpp:2573)
  calls relinquishConn() unconditionally each iteration (line ~2598), and
  waitForCollector IS reachable once shared via (a) preventCollection()
  (PreventCollectionScope users: BunV8HeapSnapshotBuilder.cpp:114/133,
  HeapSnapshotBuilder.cpp:73, JSInjectedScriptHost.cpp:1008, and Heap.cpp
  deleteAllCodeBlocks/deleteAllUnlinkedCodeBlocks/addMarkingConstraint)
  and (b) the shutdown drain (Heap.cpp:629, before m_threadShouldStop is
  set, and sticky ISS can outlive the last secondary client there).
- The gates pass once shared: hasAccessBit is the pinned §10B.4 poison
  (Heap.cpp:4763 exchangeOr) and mutatorHasConnBit is set by every shared
  grant (requestCollectionShared:4812 and the amended CIND timer grant).
  With a pending shared ticket, the relinquish CAS clears the
  permanently-Mutator conn bit (§10B rule 1 violation) and
  finishRelinquishingConn notifies the quiesced collector thread
  (§10B.3/I15 violation) with m_requests non-empty -> the exact C1 assert
  (Heap.cpp:1712) through a different door.
- "The fix widens the window under --collectContinuously": yes — timer
  tickets now sit granted-unserved until the next mutator poll/election
  instead of being notified to the (asserting) collector immediately.
- NEW (found while verifying, worse than the reported leg): the SAME
  waitForCollector body also calls stopIfNecessarySlow(oldState); once
  shared, oldState has the pinned conn bit, so it would call
  collectInMutatorThread() -> runCurrentPhase(Mutator) ->
  runNotRunningPhase, which picks up m_requests unconditionally and
  CONDUCTS a collection outside the §10.2 election, without GCL and
  without the world stopped for all clients (I5 assert in Debug at
  runBeginPhase Heap.cpp:2298; silent corruption in Release).

REFUTED (in detail): "waitForCollector parks on m_worldState expecting a
collector-thread unparkAll" as the liveness mechanism — the serve path
(end of every collection, BOTH protocols, Heap.cpp:2201-2210) does
clearMutatorWaiting() + ParkingLot::unparkAll(&m_worldState), so a served
ticket does unpark a legacy waiter. The REAL liveness hazard is
no-conductor: shared tickets are only served by an election or a SINFAC
poll, and a shared-mode waiter that merely parks never serves them.

AMEND (Heap.cpp, two hunks):
1. relinquishConn(unsigned): early-return false when isSharedServer()
   (before the legacy RELEASE_ASSERTs, which are poison-shaped once
   shared) — under ISS the conn never leaves the mutator and the collector
   thread is never notified (§10B rules 1+3).
2. waitForCollector: ISS branch (re-checked per iteration) replaces the
   legacy body once shared — func under *m_threadLock, then
   stopIfNecessaryForAllClients() (stop-cooperative AND conducts pending
   tickets itself when eligible, addressing the no-conductor hazard), then
   a <=1ms timed wait on m_gcElectionCondition (notified by the serve
   path).

KNOWN RESIDUAL (recorded, not fixed here): a shared-mode waitForCollector
caller that is deferred (isDeferred()) or has no eligible client cannot
conduct; with no other polling mutator the wait spins on 1ms timed waits
until a conductor appears (hang-shaped, not assert-shaped). Also
preventCollection's "no collection can start" guarantee only excludes the
CIND timer once shared — other mutators can still ticket+elect after it
returns; pre-existing N-mutator semantics gap, separate row if it bites.

## Session 2026-06-11j — A1 final gate (assigned: A1-varargs-copyToArguments-wild-memcpy)

Assigned mechanism hypothesis (group3Primitives() VM.h:709-717 fallback arm
re-opening the shared-slot race) re-checked against code: REMAINS REFUTED per
the 2026-06-11c analytic purity argument (selector inputs immutable across the
paired slow calls; no setCurrent/lock-transition site between them) and the
6-run instrumented empirical record (zero fallback-arm stores on JS threads).
The ACTUAL A1 root is the already-cataloged K4 table I Group-2/3 missed row —
encodedHostCallReturnValue raw-VM-block writers vs per-lite LLInt asm reader
(UNGIL §A.1.3 U-T1 / SPEC-vmstate §6.3 L1 writer-reader same-storage
invariant) — fixed 2026-06-11b (RepatchInlines.h x2, LLIntSlowPaths.cpp x3,
LLIntThunks.cpp thunk mode split, LowLevelInterpreter64.asm same-VM guard),
with the varargs-pair consumer fail-stops (geometric echo + thread-local pair
echo RELEASE_ASSERTs) as defense-in-depth. No new code change this session;
Release jsc REBUILT (was stale vs the session-h Heap.cpp hunk).

Verification (rounds/a1final/) — CORRECTED 2026-06-11k (review audit):
- The originally recorded "ASAN Debug gate ... 21 runs, 21/21 PASS" DID NOT
  RUN as described: rounds/a1final-w1..w4 are empty directories (created
  22:50-23:05, never written), and the available window before rel-1.log
  (23:06) cannot hold 20 runs at the measured >=265s/run. Actual a1final
  evidence was: 1 solo ASAN no-JIT probe PASS (probe-W16-nojit.log) +
  6 Release full-size runs (1 pass, 5 pas SIGTRAPs).
- Superseded by the LOGGED re-gate in rounds/a1regate/ (session 2026-06-11k
  below), which re-establishes the gate with positive per-run records.
- A1 signature: CLOSED at its W under its profile (re-confirmed 2026-06-11k,
  10/10 ASAN). The Release W=16 residual is NOT mono-family: see 2026-06-11k
  (pas SIGTRAP family + STW-watchdog SIGABRT family observed; the earlier
  "pas family ONLY" wording was an over-claim from n=6 right-censored runs).

## Session 2026-06-11k — A1 gate re-run with logged evidence (rounds/a1regate/)

Audit response: all six review findings on session 2026-06-11j verified
against artifacts and CONFIRMED (none refuted):
 (F3, blocker) 21/21 ASAN wave gate never ran — wave dirs empty,
   timeline-impossible; corrected above.
 (F1/F5) JIT-side arms (RepatchInlines.h host-call writers, LLIntThunks
   gilOffProcess thunk split, JITOpcodes op_catch + DFG OSR-exit same-VM
   guards) had ZERO sanitized executions against the final tree: the amend3
   hammer/smoke legs (14:09) predate the final edits to LLIntSlowPaths.cpp/
   Options.cpp (16:08), ExceptionScope.h/.cpp (19:22/19:54), Heap.cpp (22:36)
   and the 22:37/22:40 rebuild. rounds/c1amend logs are banner-only (killed
   at start) — not evidence.
 (F4) flag-off leg was vacuous: amend3 flagoffF ran bench.js with no thread
   flags -> rc=3 ReferenceError (Lock undefined), logged FAIL and ignored.
 (F2/F6) "single pas family" rested on n=6 right-censored Release runs.

Re-gate (ALL legs against the final tree: Debug jsc 22:37 / Release jsc
22:40, ninja no-work vs sources; positive per-run records with rc+duration+
log tail in rounds/a1regate/RESULTS.txt; harness startup failures classed
HARNESS-FAIL and counted):
- sigASAN 10/10 PASS: ASAN Debug, GIL-off pinned flags, --useJIT=0,
  bench-local.js -- 16 smoke 400 (the A1 signature profile; 274-287s/run,
  waves of 3). Zero ASAN reports, zero echo-assert fires, checksums valid.
- jitASAN 3/3 PASS: same profile but JIT ON — first sanitized coverage of
  the thunk/guard JIT arms on the final tree.
- hammerD 10/10 PASS (Release, forced tier-up + FreeList validator) and
  hammerE 5/5 PASS (ASAN Debug JIT-on) hostcall-hammer.js — amend3 hammer
  legs re-run against the final binaries.
- flagoffRel 3/3 + flagoffDbg 2/2 PASS: NEW flag-off-compatible workload
  (w16/flagoff-varargs.js: apply/Reflect.apply/spread/construct-varargs/
  arguments-forwarding/big-frame/throwing-array-like + heavy throw/catch),
  NO thread flags, identical checksum (56767189) Release vs Debug; Debug
  exercises the reordered ExceptionScope ctor assert + varargsSetup
  RELEASE_ASSERTs under GIL-on. Flag-off identity leg now actually green.
- relfull x12 (Release full-size pinned W=16, sequential): 3 PASS,
  5x rc=133 pas panic SIGTRAP ("Alloc bit not set in
  pas_segregated_page_deallocate_with_page"), 4x rc=134 SIGABRT
  STW-WATCHDOG ("JSThreads stop-the-world failed to reach a stopped world
  within 30.000000s", OM-transition-stop requester, one entered lite
  hasHeapAccess=true non-quiescent, the rest quiesced). ZERO SEGV, ZERO
  copyToArguments/A1 faces in any log.

Conclusions:
- A1-varargs-copyToArguments-wild-memcpy: remains CLOSED at its W under its
  profile, now with a logged 10x ASAN signature gate, JIT-on sanitized
  coverage, and a real flag-off identity gate, all on the final tree.
- Residual W>=16 Release = at least TWO families, both with zero A1 overlap:
  (a) the open pas SIGTRAP family (thread-bughunter charter), 5/12;
  (b) an STW-watchdog 30s SIGABRT family, 4/12 — previously censored by the
  pas face in the n=6 sample. (b) involves the watchdog/STW machinery last
  touched by 6b298a4fdd99 and runs adjacent to the session-h/j Heap.cpp ISS
  hunks; per the standing rule those landed diffs are legitimate suspects
  for a NEW bug. NOT yet attributed; needs its own hunt row.
- No SIGSEGV face observed in 12 runs; absence of a low-rate SEGV family is
  NOT claimed (runs remain partially censored by faces (a)/(b)).

## Session 2026-06-12a — A2 final gate (assigned: A2-varargsSetup-null-calleeFrame)

Assigned mechanism hypothesis ("the size_frame slow call's per-lite store
landed in main via the group3Primitives() fallback arm while the paired
varargsSetup reload on the same thread resolved the still-zero lite") was
re-checked against the current tree and REMAINS REFUTED per the 2026-06-11c
analytic purity argument: group3Primitives() (VM.h:709-717) is a pure
function of {g_jscConfig.gilOffProcess (write-once frozen page), vm.m_gilOff
(U0c ctor-immutable), t_currentVMLite (TLS, sole writer VMLite::setCurrent on
the owning thread — no setCurrent site exists between the paired slow calls;
only doCallVarargs asm runs between them), lite->vm (registerLite-immutable)}.
Identical inputs => identical resolution; paired same-thread divergence within
one bytecode is impossible by construction. Governing invariant: UNGIL
§A.1.3(3) (U-T1) / SPEC-vmstate §6.4(3)+§6.3 L1 — Group-2/3 writer and reader
on one thread MUST resolve the SAME per-lite storage. A2's actual causal
chain (2026-06-11d amendment) is corruption-collateral of the A1-root: the
K4 table-I Group-2 missed row (encodedHostCallReturnValue raw-VM-block
writers vs per-lite LLInt reader), fixed 2026-06-11b; the r4 A2 log is a
PRE-host-call-fix tree (line 2469 = pre-hardening numbering).

No new code change this session. Remaining-rows audit re-run (grep of all
frozen VMLitePrimitives field names for raw vm.<field> access outside
group3Primitives()/mainVMLitePrimitives()): only the already-ruled surfaces
remain — wasm raw-block READERS (useWasm force-disabled GIL-off, K4 addendum),
JIT emission-time arms (K4 addendum EXPLICIT RULING), debugger/profiler
main-only rows (K4.V). No unruled writer of the A1/A2 class found.

Verification (final tree; Debug+Release ninja no-work before runs):
- Signature gate, A2's exact profile (ASAN Debug, pinned GIL-off flags,
  --useJIT=0, bench-local.js -- 16 smoke 400): rounds/a2final-w1..w3 =
  20/20 PASS (waves 7/7/6), zero failure logs, zero A2/echo-assert faces.
- Release full-size pinned W=16 x3 (sequential): 1 PASS (checksums match the
  known-good b3e65a6855b9bdeb set); 1x rc=134 STW-watchdog 30s SIGABRT
  (cataloged ab17b family, all listed lites quiesced); 1x rc=139 silent
  SIGSEGV (one-line log, no varargs/A2 face — the open JIT-on Release
  residual; A2's nojit profile is clean, so not this signature).
- Per the both-legs criterion: A2 signature CLOSED at its W under its
  profile (cumulative this sweep: 26+10+20 profile runs face-free), but
  Release W=16 sanity NOT clean (orthogonal cataloged families) =>
  verified=false for the workflow gate, same disposition as A1.

## Session 2026-06-12a — B1 residual closure (consume-side window-coherence)

ON-DISK VERIFICATION of the 2026-06-11g + AMEND state (mandated first step;
all confirmed before any new edit):
- all three [B1-DIAG] strings present in ExceptionScope.cpp (push
  poisoned-anchor :89, push window-mismatch :103, straddle :134) and in the
  ASAN Debug binary (strings scan: exactly the 3 push/straddle faces);
- ctor's FIRST m_previousScope dereference is the window-coherence check at
  ExceptionScope.cpp:101 (matches the amend record), with the shadow-only
  __asan_region_is_poisoned probe ahead of it (:87) and m_recursionDepth
  init-list-zeroed (:60) / computed after the RELEASE_ASSERT (:113 -> :114);
- m_liteAtConstruction member present (ExceptionScope.h);
- Release binary carries ZERO B1-DIAG strings (verification compiled out of
  non-ASAN Release, as documented — PlatformEnable.h ASSERT||ASAN gate).

(b) RESIDUAL CONSUMPTION SITE CLOSED (the AMEND-recorded leftover:
ThrowScope::~ThrowScope reading m_previousScope->stackPosition() after only
ctor-time verification). Coverage grep of EVERY m_previousScope reader in
Source/JavaScriptCore: ctor block (verified at push), dtor write-back
(pointer value only, no dereference), and exactly ONE mid-lifetime
dereference — the ThrowScope dtor site. Landed:
- ExceptionScope::verifyPreviousScopeWindowCoherenceBeforeConsume(site)
  (ExceptionScope.h:97 decl; ExceptionScope.cpp:172, RELEASE_ASSERT at
  :209): same treatment as the push side — ASAN poison probe emitting a
  "[B1-DIAG] ExceptionScope consume poisoned-anchor" tuple before the first
  dereference (:180), "consume window-mismatch" tuple (:197), then the
  window-coherence RELEASE_ASSERT. Deliberately a SEPARATE function from the
  push-side check so an unmapped-garbage anchor in a non-ASAN assert build
  SEGVs at a consume-attributed line, distinguishable from push faces by
  faulting PC alone. Lives on ExceptionScope (protected-member access
  through ExceptionScope* is only legal from that class).
- ThrowScope::~ThrowScope calls it (ThrowScope.cpp:72) immediately before
  the stackPosition() read (now :73). A mid-lifetime trample of the previous
  scope now fail-stops at an attributed line with the (lite, state, thread)
  tuple instead of an unattributed fault.
- No assert weakened; all machinery inside ENABLE(EXCEPTION_SCOPE_
  VERIFICATION) — compiled out of non-ASAN Release/shipping. Same
  limitation as the push side, restated: a trample that reuses the anchor
  memory for a coherent same-window scope is detectable only by the
  pre-existing dtor straddle assert.
- WRITE-ONLY round: NOT yet compiled (one builder compiles after). The
  binaries used below predate this edit; the consume-side strings are
  absent from them by construction. Builder must rebuild Debug(ASAN) before
  the consume check is live.

(a) SIGNATURES-mandated first action item, executed within the standing
no-git constraint: the pristine-vs-current A/B Release W=16 x5 comparison
REMAINS BLOCKED (cannot stash/checkout 5c0e51c2b543). Ran the current-tree
leg only; recorded as CURRENT-TREE-ONLY evidence, not an A/B result.
Profile: exact pinned invocation (Release jsc, GIL-off six-flag set,
scalebench bench.js -- 16, sequential x5, quiet host: 1-min load 0.91 at
start, no foreign jsc; logs /tmp/b1rel-currenttree):
- rel-1 PASS 47.7s, rel-2 PASS 48.1s (all checksums identical across the
  two passes, checksumA b3e65a6855b9bdeb);
- rel-3 + rel-4: STW-watchdog-30s face (sig 6, "failed to reach a stopped
  world within 30s", OM transition stop, one non-quiescent
  hasHeapAccess=true lite — the cataloged ab17b-family face), 75.9s/73.0s;
- rel-5: pas panic "Alloc bit not set in pas_segregated_page_deallocate_
  with_page" (sig 5, 34.1s — the open SPEC-jit pas-alloc-bit family).
Watchdog rate THIS leg 2/5 vs 4/5 in /tmp/b1gate (prior session), plus one
pas-alloc-bit face the prior x5 did not show. n=5 each, different host-load
histories: NO rate conclusion is drawn and the residual is NOT relabeled.
Zero ExceptionScope/verification faces in all 5 logs — stated once more as
NON-EVIDENCE for B1 (compiled out in Release, per the AMEND finding 3).
The A/B item stays open until a runner without the no-git constraint (or a
second checkout) exists.

(c) b1amend-asan.sh gate profile re-run, synchronous, reduced batch per
turn-discipline (ONE wave of 3 concurrent runs — the script's exact wave
shape and profile: ASAN Debug jsc, pinned GIL-off flags + --useJIT=0,
bench.js W=16 smoke, same ASAN_OPTIONS, ulimit -c 0; logs /tmp/b1amend2):
- 3/3 PASS, exit 0, correct pinned checksumA f9f349a1fc92a0d0 in each;
  wall 1227.9/1229.5/1229.0s, maxrss ~1.57 GB (consistent with the
  10/10 gate of record);
- face scan: zero B1-DIAG lines, zero straddle asserts, zero ASAN reports.
Counts stated exactly: 3 runs executed this session (not 9); the 10/10
/tmp/b1amend gate of record from the AMEND session remains on disk and was
not overwritten. This re-run exercises the AMEND-state binary; the new
consume-side check is NOT in it (see (b)) and gets its first gate after the
builder's rebuild — recommended: re-run b1amend-asan.sh in full against the
rebuilt ASAN Debug, expecting the same zero-face profile plus the two new
consume strings present in the binary.

DISPOSITION: B1 hardening surface now covers push (ctor), consume
(ThrowScope dtor), and pop/straddle (dtor) — every m_previousScope reader.
B1 remains adjudicated as collateral of the closed A-family root; open
items: (1) the pristine-tree A/B Release watchdog comparison (blocked,
no-git), (2) post-rebuild full gate for the consume-side check, (3) the
unattributed Release W>=16 STW-watchdog + pas-alloc-bit families (tracked
elsewhere; not B1).

## Session 2026-06-12a — C1 residual closure (assigned: w16-c1-collector-conn-bit-residual)

ON-DISK VERIFY FIRST (per assignment): the recorded fix + session-h AMEND
are both present in the uncommitted Heap.cpp diff, confirmed against the
working tree:
- CIND timer §10B.1 inline grant (Heap.cpp ~4437-4441, inside the
  notifyIsSafeToCollect timer lambda): isSharedServer() arm with
  ASSERT(!m_collectorThreadIsRunning), conn-bit exchangeOr, append +
  ++m_lastGrantedTicket, NO m_threadCondition notify. Legacy branch
  unchanged.
- AMEND hunk 1: relinquishConn(unsigned) early-returns false when
  isSharedServer() (Heap.cpp:2962-2964), BEFORE the legacy RELEASE_ASSERTs.
- AMEND hunk 2: waitForCollector ISS branch (Heap.cpp:2739-2776),
  re-checked per outer iteration: func under *m_threadLock, then
  stopIfNecessaryForAllClients(), then <=1ms timed wait on
  m_gcElectionCondition.

RESIDUAL LEG (a) — no-conductor waiter semantics: LANDED as KEEP-WAITING +
RATE-LIMITED DUMP, the LEDGER §3.5 STW-watchdog policy shape ("overload-
latency, not a wedge; keep-waiting + rate-limited dump"). The ISS branch
stamps issWaitStart; after 5s it calls the out-of-line
dumpSharedGCWaitForCollectorStall(elapsed) (Heap.cpp:2810, decl
Heap.h:1085, NEVER_INLINE so the template body stays small) and repeats at
most every 30s. The dump (caller holds *m_threadLock, reads coherent)
reports granted/served tickets, m_gcConductorActive, m_currentPhase,
m_sharedGCPreventCount, this client pointer + deferral depth, and names
the I17 reason a deferred/ineligible waiter cannot conduct. No abort, no
predicate weakening, no assert added or removed. Liveness unchanged: the
SINFAC poll each iteration conducts granted-unserved tickets whenever the
waiter IS eligible.

RESIDUAL LEG (b) — preventCollection N-mutator gap: CLOSED (not rowed).
Landed shape: a conduct-tenure gate m_sharedGCPreventCount (Heap.h:1558,
guarded by *m_threadLock) + raise-tracking bool m_sharedGCPreventGateRaised
(Heap.h:1565, guarded by m_collectContinuouslyLock, held from prevent to
allow — a §10D reversion between prevent and allow cannot leak a raised
gate).
- preventCollection(): once shared, the func raises the gate on its first
  iteration (under *m_threadLock; RELEASE_ASSERT(!m_sharedGCPreventCount)
  since holders serialize on m_collectContinuouslyLock) and waits for the
  in-flight cycle to drain: !m_gcConductorActive && phase == NotRunning —
  the legacy served == granted predicate is neither achievable under
  N-mutator churn nor sufficient after return, and is kept ONLY for the
  legacy arm. Post-wait RELEASE_ASSERT(!m_collectionScope) unchanged.
- Consumers — coverage audit of EVERY shared collection-start site: grep
  of `m_gcConductorActive = true` over Heap.cpp yields exactly two sites,
  both gated: (1) the §10.2 election winner arm (Heap.cpp:5182) falls to
  the follower wait via betweenWindowsBackOff when the gate is up (timed
  <=1ms GEC wait when no conductor is active — the landed GCL-busy shape,
  so a prevented sync requester cannot oversleep allowCollection; its
  ticket stays granted-unserved and wins a later election); (2)
  tryConductSharedCollectionForPoll (Heap.cpp:5282) refuses to conduct
  with the gate up. Plus the SINFAC fast-path filter (Heap.cpp:6320) adds
  && !m_sharedGCPreventCount so hot polls skip a conduct attempt the gate
  would refuse — the in-lock check at 5282 remains authoritative.
- allowCollection(): keyed on m_sharedGCPreventGateRaised (NOT
  isSharedServer(), so the legacy flag-off path grows no *m_threadLock
  acquisition and a mid-scope §10D flip is handled); drops the count under
  *m_threadLock with RELEASE_ASSERT(== 1) and notifyAll on the GC election
  condition (prevented followers/ISS waiters wake promptly; their waits
  are timed, so this is latency not correctness).
- SPEC-congc §9 composition: NOT contradicted — the gate blocks shared
  collection STARTS only; §10A stop requests are untouched and the holder
  stays stop-cooperative throughout the prevent window via the ISS
  waitForCollector SINFAC poll, so a JSThreads stop that needs the holder
  parked composes exactly as §9.1 specifies.

NEW TEST: JSTests/threads/w16-c1-prevent-collection.js — W=8 churn threads
(allocation tickets + forced gc() elections) while the main thread loops
generateHeapSnapshot()/generateHeapSnapshotForGCDebugging() (the
JS-reachable PreventCollectionScope users, HeapSnapshotBuilder.cpp:73);
each snapshot JSON.parsed (torn-walk self-check) + dw2-style per-seed
checksum reference rerun (cross-thread corruption check). Bounded SNAPS +
joinAll make a leg-(a) wedge fail by runner timeout instead of passing.
Flag-on GIL'd and flag-off arms included.

FLAG-OFF IDENTITY (by inspection — see verification status): every new
runtime branch is behind isSharedServer() (false flag-off) or
m_sharedGCPreventGateRaised (only set inside an isSharedServer() arm);
relinquishConn/waitForCollector/preventCollection/allowCollection legacy
bodies byte-unchanged; no assert weakened anywhere (one RELEASE_ASSERT
pair ADDED on the gate raise/drop).

VERIFICATION STATUS — exact counts: this was a WRITE-ONLY session (no
builds; single builder compiles after). Runs executed this session: 0.
The pinned runtime gate is PENDING BUILDER and must be run as specified:
(1) exact repro cell x>=10 ASAN synchronous (pinned GIL-off flags +
--collectContinuously=1, bench-local.js -- 16 smoke 600, 720s timeout;
expect clean rc=124 timeouts, 0 asserts/reports, per the 20/20 gate of
record for the base fix); (2) GIL-on legacy control (--useJSThreads=1
--collectContinuously=1, W=4 smoke 100) PASS rc=0; (3) flag-off
byte-identity check on the rebuilt binary; (4) the new
w16-c1-prevent-collection.js under the pinned GIL-off env, GIL-on, and
flag-off.

## Session 2026-06-12 — asan-bench-r2 fixer: fullsize-asan-bench-out-of-budget

Failure: full-size ASAN gate (ASAN Debug, pinned GIL-off flags, bench.js -- 16,
N_BASE=28000, 600s timeout) rc=124 banner-only. Adjudication: GATE-BUDGET
INFEASIBLE BY CONSTRUCTION, not an engine regression — disposition and
arithmetic in rounds/asan-bench-r2/DISPOSITION.md. Key facts: (a) independent
fixer re-run of the smoke equivalent this session: bench.js -- 16 smoke PASS
rc=0 at 447.5s wall, maxrss 3.0GB, 0 ASAN reports, 0 asserts, all four
checksums byte-identical to the round's 3/3 W=32 smoke PASSes
(rounds/asan-bench-r2/w16-s1/fixer-verify-run1.{log,rc}) — even SMOKE uses
75-90% of the 600s budget; (b) full-size is 14x N with superlinear phase work,
and the on-disk record already shows 3/3 rc=124@2400s + 1/1 rc=124@7200.9s
(16.5GB maxrss) for the identical invocation — 600s undershoots the
demonstrated floor by >12x. Evidence-hygiene note: the round's w16-s1..s5 dirs
were EMPTY (claimed 5/5 smoke PASS had no logs on disk); the W=32 3/3 claim
verified byte-for-byte; the W=16 smoke claim is now corroborated by the
independent run archived in w16-s1. Fix: gate re-spec per the 2026-06-11f A6
coverage split (ASAN signature profile -> §5.1 smoke knob, timeout >=720s;
full-size production profile -> Release pinned n=12 gate). No Source/ change;
flag-off identity LAW untouched; no asserts touched; pinned N_BASE untouched
(frozen SPEC §8).
