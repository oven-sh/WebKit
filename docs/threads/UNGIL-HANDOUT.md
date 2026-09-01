# UNGIL-HANDOUT.md — Consolidated normative implementation handout for GIL removal

> **GENERATED FROM FROZEN SOURCES.** This document flattens, into one
> linear read, **SPEC-ungil.md rev 32** (the doc of record), every
> BINDING annex of **SPEC-ungil-history.md** (through rev 32; rev 32
> is an AMENDMENT RECORD — its EXIT1 deltas are applied IN PLACE to
> the inline EXIT1 copy below, whose header states the
> correspondence rule; the rev-32 A36 amendment is the A36 text of
> record and is applied in place to the inline A36 copy), the two
> EXECUTED audits **SPEC-ungil-audit-K4.md** and
> **SPEC-ungil-audit-N7.md**, and the ordered task list (rev-9 annex 3
> as amended through r32, with the history's own task-sizing license
> applied: U-T4 split into U-T4a/U-T4b). Every "see rX FY"-style
> pointer in the spec body has been resolved into the actual text at
> its reference point. **On any conflict, SPEC-ungil.md (+ its cited
> BINDING annexes in SPEC-ungil-history.md) remains the document of
> record; this handout is a derived artifact and must be regenerated,
> not edited, when the spec moves past rev 32.**
>
> Authorities behind the spec: THREAD.md; SPEC-{heap,vmstate,
> objectmodel,jit,api}.md (+ annexes); INTEGRATE-api D1–D13. Verified
> vs tree 2026-06-06 (branch jarred/threads). Citation shorthand:
> vmstate:/api:/om:/jit:/heap: = the five SPEC files; IU =
> INTEGRATE-ungil.md (created at U-T1, see §IM); IA/IJ/IV/IH/IO =
> INTEGRATE-{api,jit,vmstate,heap,objectmodel}.md.

## Master rule

GIL-off is a **MODE** (`useThreadGIL=false`). Every GIL-on path stays
compiled and is the fallback/bisection oracle (§J); GIL-on observable
behavior is unchanged EXCEPT the two recorded both-mode deltas **SD6**
(§C.6/§A.2.6) and **SD7** (§I). "GIL-off" = `useJSThreads() &&
!useThreadGIL()`; it requires `useVMLite=1`,
`useSharedAtomStringTable=1`, and the shared GC server (U0).
Re-freezes/SUPERSESSIONS of frozen text always cite both sides.

Definitional sentence (ANNEX A26, BINDING): "Both modes"/"deleted" in
§A.2.6-class statements = both GIL modes **under useJSThreads=1**;
flag-off keeps the landed vanilla-SAB machinery compiled and live.

---

## 0. Execution model

Post-GIL, ONE VM may have N concurrently *entered threads*. An entered
thread holds a VM entry token (§F): registered VMLite + unique TID
(vmstate §6.4.4), GCClient::Heap ACT (heap Dev 8),
VMThreadContext/VMTraps (§A.2), microtask + task queues (§E), a
per-entry record (§A.1.5). Cross-thread soundness of PROPERTY/shape
storage is the landed OM/jit/heap machinery (UNGIL-PLAN Part IV);
builtin cell-INTERNAL state is NOT — §N rules it.

### U0 — config gate

GIL-off with {useVMLite=0 | useSharedAtomStringTable=0 |
useSharedGCHeap=0} is refused at option validation (forced
useThreadGIL=1). U0 is thus: a pure options check at validation plus
the ctor-time CAS (U0c) for the runtime server designation.

### U0b — multi-VM

heap I13's one-sticky-shared-server RELEASE_ASSERT is KEPT
(Heap.cpp:4124). GIL-off, exactly ONE VM per process — the m_gilOff VM
(U0c) — may hold per-thread clients. Any other VM refuses Thread spawn
with a RangeError (api 5.1 shape) and keeps the GIL-on
single-migrating-client + real m_lock protocol for multi-embedder
entry. GIL mode is per-VM, not per-process; §F.1/§A.3.6 branch on it;
storage selection = §A.1.3's two-level discriminator. Lifting I13 to N
shared servers is a post-v1 renegotiation (would re-derive heap
§10/§10D + VMManager for two concurrent shared servers).

Corpus (r22 list): second-VM spawn refused; second-VM two-embedder
entry green beside the shared VM, and the two-embedder arm must
EXECUTE JS (throw/catch + deep recursion against the stack limit + a
GC), not merely enter. IU row for Heap.cpp:4123-4124.

### U0c — m_gilOff assignment (ANNEX U0C, BINDING — full text)

vm.m_gilOff is computed ONCE in the VM ctor — BEFORE m_mainVMLite
registration (vmstate §6.4.4), any entry, any codegen — and is
IMMUTABLE for the VM's lifetime. Designation primitive (r12;
noteSharedServerSticky() is loser-FATAL — its inner CAS
RELEASE_ASSERTs, Heap.cpp:4123-4124 — so it cannot BE the
designation): NEW `Heap::tryDesignateStickySharedServer()` = the
s_stickySharedServer CAS, returning won/lost, NO assert. Under
gilOffProcess every VM ctor calls it.

- WINNER: m_gilOff=1, then noteSharedServerSticky() at
  clientSet()==1 (quiescence trivial at birth; the inner CAS sees
  previous==this — I13 stands TEXTUALLY UNCHANGED and never fires on
  this path).
- LOSER: m_gilOff=0, never calls noteSharedServerSticky() from the
  ctor; U0b spawn-refusal keeps its clientSet()<=1, so the
  HeapClientSet::add:69 trigger site — which STAYS, idempotent
  (SUPERSESSION vs heap §5.1's "option && clientSet().size() EVER >
  1" trigger, Heap.cpp:4106-4124, both sides) — is unreachable for
  it; a loser reaching it IS a bug, and I13 firing is the correct
  behavior. That add site additionally gains
  RELEASE_ASSERT(gilOffProcess => the server VM's m_gilOff == 1).

gilOffProcess itself is OPTION-derived at Config finalization
(useJSThreads && !useThreadGIL && the U0 trio). This discharges
§F.2's ISS-flip clause-(a) STRUCTURALLY (the flip pre-dates first
entry/codegen/lite registration). §10D never clears m_gilOff (it is
not heap state); the Heap.cpp:4755 m_isSharedServer=false arm is
conditioned on !gilOffProcess — under gilOffProcess the server stays
ISS for process lifetime (codegen and lite bytes were stamped against
gilOff=1; un-sharing would not un-stamp them; a GIL-off process that
joins all threads keeps shared-server overheads — accepted, the mode
is per-process-singular anyway). Lites copy the final byte at
registration; no jettison/migration story is needed.

Corpus: compile-heavy single-thread run (forces all tiers) THEN first
spawn, then Group-3 consistency checks (topCallFrame/exception/stack
limits) on both threads; TWO VMs CONSTRUCTED under gilOffProcess —
loser ctor completes, loser spawn RangeErrors, loser embedder entry
executes JS beside the shared VM.

---

## A. vmstate Phase B — per-thread execution-state consumption

Charter: vmstate:42-48 (Phase B UNOWNED; r12); api §2; jit R1 freeze
scope (jit:233). Phase A's frozen layout is consumed unmodified:
VMLite::offsetOfPrimitives()/offsetOfTID() + L1–L5 are ABI; only
accessor implementations change (L4).

### A.1 Pinned base + VM::field rerouting

**A.1.1 TLS base.** t_currentVMLite (vmstate L4). C++:
VMLite::current(). Asm/JIT use a new emitter `loadVMLite` per jit
App. R5 (per-OS mechanisms there: ELF IE-TLS; Darwin pthread key via
the M4a JSCConfig slot; Windows already unsupported flag-on per App.
R5 — no new Windows story owed). No reserved GPR. (Rationale: a
dedicated callee-saved register was rejected — x86_64 register
pressure, ABI churn vs the L4 escape hatch; TLS-read-per-prologue +
temp caching costs one load on entry paths; the
VMEntryRecord::m_vmLite slot exists so OSR exit/unwinding recover the
base without TLS in awkward contexts.)

**A.1.2 Mid-body access rule, all tiers.** The correctness carrier is
REMATERIALIZATION — any site needing the lite re-loads via
loadVMLite; prologue temps and the new VMEntryRecord::m_vmLite slot
are OPTIMIZATIONS only.

**A.1.3 Group-3 storage, per mode (DECIDED).** THREAD.md:19's Group-3
set — topCallFrame, exception state, stack limits (§A.2), scratch
(rule A.1.6), m_microtaskQueue (§E), lazy regexp stack/match buffers:

- GIL-on (flag-on OR off): VM storage, emission &vm +
  OBJECT_OFFSETOF — zero codegen delta flag-off (jit I1); the U19
  oracle keeps VM storage (§J.5/§J.6).
- GIL-off: VMLitePrimitives storage. Mode is PER-VM (U0b);
  **TWO-LEVEL discriminator** (r10, re-freezes the r6-F7 gilOff-byte
  ruling both sides):
  1. A derived JSCConfig byte **gilOffProcess** ("a GIL-off VM exists
     in this process"; lands beside the M4a slot comment,
     JSCConfig.h:106, r25 line-drift fix). FALSE => VM storage, one
     not-taken LLInt branch per site.
  2. TRUE => LLInt loads a per-lite byte **VMLite::gilOff** (L2
     append, copied from vm.m_gilOff AT lite registration; a CURRENT
     lite exists whenever the byte matters — useJSThreads=1 installs
     m_mainVMLite/carriers for every VM, vmstate §6.4.4). Byte 0 =>
     VM storage (second VM's GIL-on protocol intact); 1 => lite
     storage.
- VM same-name accessors branch on vm.m_gilOff (the per-VM member,
  source of the lite copy), NOT the Config byte.
  Baseline/DFG/FTL + §A.1.6 baked-vs-indirected select **AT CODEGEN
  TIME** on the COMPILED-FOR VM's mode (codeBlock->vm); code never
  migrates VMs. U0c fixes the mode pre-codegen, so codegen-time
  selection can never observe a mode change.
- **R1.e is NOT re-pointed** (r6 F7): gilOffProcess is a SECOND
  derived byte ADDED BESIDE jit R1.e's useJSThreads byte (jit:230/
  :251 M4a); R1.e's byte and ALL its landed ifJSThreadsBranch
  consumers (LowLevelInterpreter64.asm:1615-1618 gating the §5.4
  LLInt cache disables and §5.5 TID/SW butterfly choke points) are
  UNCHANGED, both modes. Only the NEW Group-3 storage-selection
  branches test gilOff. Recorded as an EXTENSION of jit R1.e, both
  sides cited. (Testing useJSThreads instead of gilOff in the new
  branches would route flag-on+GIL-on — phase-1 production / the U19
  oracle — to VMLite storage: unsound.)
- GC root walk, §A.1.5 fan-outs, §J.5/§J.6 handoff writes, the L7
  assert: ALL keyed per-VM.
- Flag-off golden-disasm gate RE-BASELINED once. **Flag-off identity
  SUPERSESSION** (jit I1 jit:180 + vmstate R3 + api I1 vs this +
  §A.2.6, both sides; r10 F6): the permitted flag-off deltas are now
  ALSO: (a) one not-taken gilOffProcess branch per LLInt Group-3
  storage-selection site (where a site already sits inside an
  ifJSThreadsBranch region the gilOff test NESTS under it — ZERO new
  flag-off branches there; emission rule, not optional); (b)
  atomicsWaitImpl's useJSThreads branch; (b2) §N.5's twin intrinsics
  (uniform builtin-bytecode change whose flag-off LOWERING is the
  landed sequence behind the delta-(a) branch); (c) NOTHING else —
  §K/§N/§E/§F machinery is gilOff-runtime-only or flag-on-only; the
  list is re-audited at U-T14 (any new flag-off branch = gate
  failure). BENCH: the flag-off --useJIT=0 bench gate (jit Task-13)
  is RE-RUN after the one-time LLInt golden-disasm re-baseline and
  must stay in-noise — Group-3 sites include the LLInt prologue
  stack-limit + exception checks.
- **GC roots (r6 F5, full text, NORMATIVE).** heap/Heap.cpp:3585
  roots vm.exception()/vm.lastException() through VM accessors inside
  the ConservativeScan/VMExceptions constraint; post-rerouting those
  resolve through the CURRENT lite — wrong on a GC visit thread.
  Rule: the shared collection's root/handle visit phase iterates the
  VMLiteRegistry under its lock and appends EVERY registered lite's
  cell fields (m_exception, m_lastException, scope-verification
  cells, lazy regexp match buffers) — but ONLY lites with lite->vm ==
  the collecting VM (per-VM filter; the same filter applies to
  §A.1.6/§K.1 scans and the §A.1.5/§A.2.3 fan-outs). The registry is
  stable because mutators are quiesced by the heap §10 stop.
  vm.m_terminationException (Heap.cpp:3592) stays VM-global (per-VM
  singleton, not per-thread Group-3 state). §IM heap/Heap.* row lists
  "A.1.3 root walk (:3585-class sites)". U-T1 amplifiers: a thrower
  parked pre-catch survives a forced full collection; a two-VM arm.
- Predicate keying (r27/TERM1.4): unqualified "gilOff" predicates in
  this spec = **vm.m_gilOff** (level (ii), per-VM) unless
  gilOffProcess is NAMED.
- Owner: U-T1 (+U-T3/U-T4 for emission).

**A.1.4 Stack pointers.** stackPointerAtVMEntry/lastStackTop become
per-entry-token lite fields; JSLock.cpp:166's L7 RELEASE_ASSERT is
GIL-on-only; the GIL-off token ctor asserts the *lite's* slot empty
(re-entry uses the VMEntryRecord chain).

**A.1.5 Per-entry record (entryScope race).** m_vm.entryScope
(VMEntryScope.cpp:90/:133), VM::isEntered() + the service bits move
into the lite (L2); ctor/dtor + executeEntryScopeServicesOnExit use
the CURRENT lite; isEntered() = "§A.3.1 set non-empty"; VM-wide
consumers iterate the registry under its lock. GIL-on/flag-off
unchanged. U23. Service routing (mirrors §A.2.3): services classify
VM-wide vs thread-local (table, U-T1); VM-wide + CONCURRENT_SAFE
requests (requester may hold NO lite) set a VM-level word + fan into
this VM's lites under the registry lock; thread-local services use
the current lite (§F.2's gigacage-disable deferred arm = VM-wide).

**A.1.6 Scratch buffers (ANNEX A16, BINDING — full text; r26
extension: §K.6/AUD1.K4).** Baked scratchBufferForSize ADDRESSES
(DFG/FTL) are shared by N threads. NORMATIVE GIL-off: a process-wide
**ScratchBufferRegistry** (§LK rank, outside VMLiteRegistry::lock;
monotonic indices + an index->size map, never freed); each lite holds
(L2) an append-only segmented pointer table (lock-free reads). Every
baked site becomes `loadVMLite -> segment -> [index]`, all tiers
incl. OSR-exit + calleeSaveRegistersBuffer; a buffer exists at (lite,
index) BEFORE the code runs (install fans to the VM's lites;
registration backfills); install nesting SBR -> VMLiteRegistry::lock
-> scratchBufferLock is LEGAL (§LK.6 re-rank; SUPERSESSION vs vmstate
§6.5.1/§7, both sides). Non-baked: the CURRENT lite's table by
size-class — this IMPLEMENTS the reserved
VMLite::scratchBufferForSize(size_t) (re-freeze recorded vs
vmstate:534-539, both sides); the inert Group 5
(scratchBufferLock + Vector<ScratchBuffer*>) is REPURPOSED, not dead:
it is the lite's buffer-ownership list (every buffer installed into
this lite's table appended under scratchBufferLock), backing the
jit-R2 registry GC scan and teardown free. Frozen L1–L5 untouched (L4
sanctions accessor-implementation changes). JITCode-RESIDENT members
(catchOSREntryBuffer, FTL m_entryBuffer) become registry indices per
entering lite (U-T4 amplifier: concurrent catch/loop OSR entry on one
CodeBlock). GIL-on/flag-off keeps baked addresses; per-lite buffers
are GC-scanned via the registry walk (jit R2).

A16 EXTENSION (AUD1.K4, BINDING): the loadVMLite -> segment -> index
rework EXTENDS beyond scratch buffers to every §K.1 per-lite member
whose address is baked into Baseline/DFG/FTL inline paths:
VM::m_megamorphicCache (VM.h:960), VM::m_hasOwnPropertyCache
(VM.h:956), JSGlobalObject::m_regExpGlobalData (AUD1.K2/SD19), and
JSGlobalObject::m_weakRandom (K4.VIII.10, Math.random fast path).
Mechanism: gilOff-mode compilation (the §A.1.3 COMPILED-FOR-VM-mode
rule) emits one loadVMLite (rematerialized per §A.1.2) +
lite-relative offsets to the lite-resident copy; the lite holds the
cache inline or via one indirection slot filled at lite registration
(lazy §K.3 publish for ensure* contents). Flag-off/GIL-on: baked
VM/global addresses unchanged (golden gates intact). Epoch/age bumps
(MegamorphicCache invalidation) fan out via the registry walk INSIDE
the stop that fires the corresponding watchpoints (K4.VI.2) — no new
fence on the probe path. Per-lite caches are private => probe/fill
races vanish; no locked fallback needed. U-T4a/U-T4b own the
emission; disasm arm per A16.

**A.1.7 Cross-thread Group-3 READERS (r9 F7 + r24 F2, NORMATIVE).**
SamplingProfiler.cpp:391-431 suspends one m_jscExecutionThread and
reads m_vm.topCallFrame from the profiler thread; VMInspector/$vm are
kin. Every off-thread reader of a rerouted field must (i) resolve the
TARGET thread's lite via the registry (registry locked, target
suspended), (ii) be refused GIL-off with a defined error, or (iii) be
proven on-thread. v1: SamplingProfiler samples carrier lites via (i)
only (spawned threads unsampled — --cpu-prof stays useful; N-thread
sampling is post-ungil; see AUD1.K1/SD18). **SUSPEND RULE (r24 F2):**
while ANY thread is suspended by a (i)-reader, the suspending thread
performs NO allocation (fastMalloc included) and acquires NO lock
beyond the already-held registry lock; all sample/trace buffers are
pre-allocated before suspension (today's SamplingProfiler discipline
made normative). This is a scoped carve-out of §LK.6's
fastMalloc-under-registry-lock allowance — the allowance does NOT
apply while a thread is suspended by the holder (partial return to
vmstate 6.5.1's stricter rule for exactly this window; the §LK.6
SUPERSESSION otherwise stands, both sides re-cited). U-T8d enumerates
off-thread readers per rerouted field (IU table) + a sample-storm arm
(target spinning in fastMalloc-heavy native code; TSAN + deadlock
watchdog). IM rows: SamplingProfiler.{h,cpp}, VMInspector.cpp.

### A.2 Per-thread VMThreadContext / VMTraps

vmstate §6.8 (per-thread per L2, chained offsets OK):

1. VMLite appends (L2, after Group 6) VMThreadContext threadContext +
   VMTraps traps; generated code reaches lite->traps.m_trapBits via
   the chained offset.
2. Stack limits live in the lite's VMThreadContext, set at thread VM
   entry (handoff migration GIL-on-only; vmstate §2 rule 3 preserved
   GIL-on).
3. **Trap fan-out.** The VM keeps a VM-level "process traps" word;
   raising a VM-wide trap (termination, GC stop reason — §A.3.8;
   debugger/watchdog bits are carrier-only, §A.2.7-8) = under the
   registry lock, set the bit in every lite OF THIS VM (§A.1.3
   per-VM filter) + the VM word (token acquisition ORs it in;
   replaces notifyGrabAllLocks()). Per-thread traps: one lite. The
   "per-thread: one lite" arm exists for genuinely per-thread traps
   (per-lite stop tickets §A.3; debugger/watchdog carrier-only bits)
   and NEVER carries the termination bit (TERM1.2).
4. **Termination is VM-WIDE ONLY in v1 (ANNEX TERM1, BINDING — full
   text).**
   - TERM1.1 No thread-targeted termination surface exists in v1.
     Thread.prototype.terminate DOES NOT EXIST. The Thread surface
     stays api 4.1 VERBATIM (constructor, join, asyncJoin, id,
     current, restrict; Lifecycle Running->Finished(result)|
     Failed(exc); no detach/cancel) — NOT a supersession: nothing in
     the frozen set granted a terminate API. Every "terminate" arm in
     the spec (SD6's terminate-parked arm, SD8, U19's
     terminate-parked arm, §T's terminate-during-TA-wait flag-off
     gate, §E.5's termination trap) means VM-LEVEL termination
     requested by: (a) Watchdog (annex W; corpus --watchdog, cf.
     SPEC-api-annex property-wait-termination.js), (b) the embedder's
     VMTraps termination request (NeedTermination class, api G23
     anchor VMTraps.h:149-156), (c) shell/embedder teardown paths
     routing through (b). A thread-targeted terminate() is POST-UNGIL
     work requiring a new SD plus an api-4.1 supersession recorded
     both sides.
   - TERM1.2 Granularity: VM-WIDE ONLY. Raising termination = the
     rule-3 VM-wide form: under the registry lock, set the
     termination bit in EVERY lite of the target VM (§A.1.3 filter) +
     the VM word; token acquisition ORs it in. There is NO mechanism
     in v1 to raise termination on exactly one lite. Consequences
     (binds the SD8/U19 corpus arms): terminating the VM terminates
     EVERY entered thread; a sibling parked in Atomics.wait takes the
     Terminated arm (api 5.6-4 throwTerminationException) and then
     ALSO closes per §E.5; main's in-flight JS unwinds with the
     termination exception to the host. The VM is NOT destroyed: the
     carrier host services the termination (watchdog shouldTerminate
     callback / embedder clears the trap per the landed VMTraps
     protocol) and may re-enter; join()s performed after re-entry
     observe Phase::Failed.
   - TERM1.3 Failed payload + join (SD8 ext2). The §E.5 close of a
     terminated thread publishes into the landed F1/F5 result Strong
     a FRESH ordinary Error("Thread terminated"), allocated
     native-side at close (thread entered, with access, no JS runs) —
     NEVER vm.m_terminationException (deliberately
     cross-thread/sticky: vmstate "Deliberately NOT in
     VMLitePrimitives" list; K4 traps row "sticky release-publish";
     rethrowing IT would re-terminate the joiner, contradicting
     §E.5's main-fallback drain assumption). join() rethrows this
     Error as a NORMAL catchable exception (api F1/I3 identity rule
     applies); asyncJoin's promise rejects with it. If the close
     itself cannot allocate (OOM), fall back to the landed
     OOM-failure shape for F1/F5. Corpus (U19 terminate arms +
     U-T11): join-after-termination catches an ordinary Error (joiner
     continues executing); asyncJoin rejection observed; GILOn/GILOff
     variants per the SD8 pattern.
   - TERM1.4 Discriminator notes. VMLite::isSpawned is written ONLY
     in the §B.1 spawn path (=1 BEFORE setCurrent); carrier lites
     never set it. Because spawned threads are single-VM (TERM1.5), a
     spawned thread's CURRENT lite is always its own spawn lite, so
     the §I JIT prologue byte check and the C++ isJSThreadCurrent()
     gates agree at every site, as does §A.2.7's carrier-only
     exemption. Unqualified "gilOff" in §C/§I/§N rulings =
     vm.m_gilOff; §C.4's lifted TA gate reads vm.m_gilOff (spawned
     threads exist only in the m_gilOff VM, so the keyings coincide
     on every reachable path).
   - TERM1.5 §F.5 caller scope: see §F.5 below.
   - TERM1.6 IU creation rule: see §IM below.
   - The D9 park-poll predicate jsThreadParkTerminationRequested
     re-points at the polling thread's **PARK lite**: spawned =
     CURRENT lite; main/embedder park sites = the §J.3-captured lite
     (U31).
5. **Async (signal) delivery OFF GIL-off** (VMTraps.cpp:305/:80).
   SignalSender never started; delivery = bit fan-out (rule 3) + poll
   sites + D9 quanta; vmIsInactive = "no registered lite entered".
6. **Sync-park termination wake BYPASSED under useJSThreads, both GIL
   modes** (ANNEX A26 + r6 F3): §C.6's per-wait nodes (SD6) orphan
   the VMTraps.cpp:329/:419 vm.syncWaiter() wakes. Replacement: TA +
   §C.3 sync parks wait in D9 10ms quanta polling termination
   (GIL-on: the VM-wide rule-4 form; GIL-off: the rule-4 PARK lite's
   bit) — U2's bound; U19 terminate-parked arm. **Flag-off
   disposition (r6 F3): the wakes are BYPASSED, not deleted** —
   vm.syncWaiter() (VM.h:1174/:1376), the :329/:419 wakes and the
   landed waitForSync park stay compiled AND LIVE; atomicsWaitImpl
   branches on useJSThreads; the D9 predicate is never consulted
   flag-off. §T flag-off gates include
   terminate-during-infinite-TA-wait. (The D8 single-flight gate,
   AtomicsObject.cpp:500-511, is itself on the flag-gated path, so
   deleting IT in both GIL modes stays sound.)
7. **Debugger/inspector (r11 F3; pause state singular,
   Debugger.h:342).** v1 GIL-off: the debugger trap bit is EXEMPT
   from rule 3 — delivered ONLY to main/embedder carrier lites
   (VMLite::isSpawned discriminator, same byte as §I); Debugger entry
   hooks early-return on a spawned lite (SD13); spawned-thread
   breakpoints are defined no-ops; pause keeps the landed
   single-threaded carrier protocol; attach/detach + CodeBlock-wide
   recompile/registration walks run under a §A.3 stop so spawned
   threads cannot execute mid-walk. N-thread debugging is post-ungil.
   GIL-on unchanged. IU row; corpus: a spawned thread crosses a set
   breakpoint without abort or pause; main still pauses.
8. **Watchdog (ANNEX W + W ext, BINDING — full text).** v1
   CARRIER-ONLY; per-thread CPU deadlines are post-ungil; §K.4 routes
   Watchdog here (K4.V.1). GIL-on unchanged.

   Landed mechanics (the GIL-off baseline): m_hasEnteredVM toggles in
   enteredVM/exitedVM (Watchdog.cpp:115-126); startTimer records
   m_cpuDeadline from the ARMING thread's CPU clock plus a wall-clock
   m_deadline, then dispatches a timer whose callback calls
   m_vm->notifyNeedWatchdogCheck() (:129-155); shouldTerminate —
   reached ONLY at JS poll sites via NeedWatchdogCheck
   (VMTraps.h:105-118) — rejects stale timers, re-arms if CPU budget
   remains, else consults the embedder callback and returns the
   terminate decision (:55-108); the API-lock asserts read as the
   §F.2 token GIL-off; carrier watchdog state is serialized by the
   real Watchdog::m_lock (§F.1).

   GIL-off NORMATIVE rules (useJSThreads, GIL-off only):

   - **W0 (accounting):** arms/measures on main/embedder carriers
     only. Spawned entry/exit toggles neither carrier-entered state
     nor the timer; spawned CPU never advances the CPU budget; the
     watchdog-check trap bit is set ONLY in carrier lites (rule-3
     exemption, like the debugger bit). SD14.
   - **W1 (parked-carrier service — §J.3 carve-out):** a
     main/embedder carrier parked under §J.3 already polls the
     CAPTURED lite's trap bits each D9 quantum. On observing the
     watchdog-check bit it performs the FULL §J.3 exit reacquisition
     (m_lock + token + access: §A.3.6 swap + §F.1 OR + §A.3.2b
     poll — the same sequence, run EARLY), services
     Watchdog::shouldTerminate under its token on its own thread
     (callback semantics and CPU re-arm identical to an entered
     carrier), then: terminate => raise VM-wide termination (rule 3)
     and proceed to final park exit (the wait fails per SD8/§E.5); no
     terminate => re-release per §J.3 and re-park. §J.3's "exactly
     once" is renormalized to once per ACQUISITION EPISODE: W1
     service ends one episode; re-parking opens a new one. Lock-rank
     clean: reacquisition happens only after the quantum wait
     returns — no rank-3 waiter-list lock is held across it; api
     5.9(e) ordering holds per episode; listLock is taken only AFTER
     reacquisition and dropped BEFORE any re-park.
     **Old-node disposition (r15 F2, AMENDS W1 — NORMATIVE):** after
     a no-terminate service, BEFORE re-parking, the carrier disposes
     of its still-enqueued wait node under the owning waiter list's
     listLock: (a) old node already notified/dequeued => the wait
     completes "ok" immediately — NO re-park, NO fresh node (the
     consumed notify is honored, never stranded); (b) old node still
     enqueued and un-notified => remove it and tail-enqueue a FRESH
     node, then re-park (FIFO-position loss declared, = the existing
     I10 eats-one-notify class). At no point are both nodes live past
     the disposition; a notify landing DURING it serializes through
     listLock and hits exactly one of (a)/(b). The SAME disposition
     applies to ANY future early-service exit from a §J.3 park (a
     property of the J.3 carve-out, not of the watchdog).
   - **W2 (exit deferral):** exitedVM() on the LAST carrier (carrier
     entry depth, under Watchdog::m_lock) while spawned lites remain
     registered clears m_cpuDeadline (CPU budget is carrier-scoped)
     but PRESERVES m_deadline and the pending dispatched timer: the
     watchdog stays armed for wall-clock purposes. m_hasEnteredVM
     splits GIL-off into m_carrierEntered (depth) + m_wallClockArmed;
     the asserts re-point accordingly. A carrier re-entering re-arms
     the CPU budget as landed. When the last spawned lite unregisters
     with no carrier entered, the watchdog disarms fully.
   - **W3 (no-carrier enforcement):** the dispatched timer callback
     gains a GIL-off branch under Watchdog::m_lock: if any carrier
     lite is entered-or-parked => notifyNeedWatchdogCheck() as landed
     (entered carriers service at poll sites; parked carriers via
     W1). Else (spawned-only execution): evaluate the WALL-CLOCK
     deadline on the timer thread itself (same stale-timer rejection
     as shouldTerminate); if expired, raise VM-wide termination
     directly via the rule-3 fan-out (registry lock only — the
     async-delivery path §A.2.5 already runs tokenless) and disarm.
     The embedder callback is NOT consulted in W3 (it needs a
     JSGlobalObject, the token, and carrier thread identity):
     terminate-by-default, matching the !m_callback default. The CPU
     budget is not evaluated in W3: spawned-only execution is
     governed by wall-clock only.
   - **W4 (the four Watchdog APILock asserts; ANNEX W ext, BINDING):**
     runtime/Watchdog.cpp:44 (setTimeLimit), :57 (shouldTerminate),
     :132 (enteredVM), :160 (exitedVM) guard state with no serializer
     other than today's GIL; under §F.2's redefined token meaning N
     spawned threads would satisfy them simultaneously. Ruling: all
     four sites are §F.2 EXCLUSIVITY CONSUMERS in the U-T8 table;
     named serializer = the REAL JSLock m_lock (§F.1 keeps
     main/embedder mutual exclusion GIL-off, so at most one carrier
     is between the asserts at a time). Spawned threads never reach
     them: Watchdog entry/exit hooks early-return on
     VMLite::isSpawned (the W0 exemption's enforcement point);
     spawned JS is watchdog-unobserved v1 (SD14). Assert rewrite
     (GIL-off branch): the four ASSERTs become
     JSLock::currentThreadIsHoldingLock() (mutex-literal predicate,
     §F.2) && !VMLite::isSpawned. GIL-on/flag-off byte-identical.
     W2/W3 interplay unchanged: the W3 timer thread fires termination
     via the rule-3 fan-out, never via these methods.

   Interactions: the VMTraps NeedWatchdogCheck -> NeedTermination
   fall-through is unchanged for entered carriers. Termination raised
   by W1/W3 reaches spawned threads via rule 3 + D9 park quanta
   (§A.2.6), and parked main via the rule-4 park-lite predicate.

   Corpus (U-T2): wall-clock limit while a spawned thread loops,
   three shapes — (a) main parked in join (W1: callback consulted;
   extension honored — grants more time once, then terminates), (b)
   main did lock/eval/unlock and left (W3: terminate fires WITHOUT
   the callback — SD14 arm), (c) main spinning in JS (landed shape,
   regression guard). All three: spawned terminated, no abort. GIL-on
   //@ runThreadsGILOn variants keep old expectations. U-T2 also
   gains the W4 assert-rewrite + spawned-unreachability lint; U-T11
   arm: watchdog fires while a spawned thread runs hot JS and the
   carrier is parked (W1 path) — no spawned-side assert trips, TSAN
   clean on m_timeLimit/m_cpuDeadline; plus the r15 F2 arm: watchdog
   fires while main is parked in property Atomics.wait AND a spawned
   thread notifies during the service window => main's wait returns
   "ok"; a second parked waiter + counted notify(1) budget asserts no
   notify stranded; GIL-on variant keeps landed behavior.
   IM: Watchdog.{h,cpp} (carrier depth + m_wallClockArmed split,
   timer-callback branch); VMTraps.cpp (no new surface — rule-3
   fan-out reused).
### A.3 Thread-granular stop-the-world (re-freezes jit R1.c)

Re-freezes jit:233 ("N threads in ONE VM = thread-granular STW"),
both sides. The stub is replaced: JSThreadsSafepoint.cpp:244-250
RELEASE_ASSERT out, real R1.a-i sequence in.

**A.3.1 Counting unit = entered thread.** VMManager tracks per-VM
entered threads (token holders, §F): forEachEnteredThread(VM&, f) /
numberOfEnteredThreads are REGISTRY WALKS — the entered SET is the
VMLiteRegistry (vmstate §6.5.1), filtered lite->vm in the target VM
set (§A.1.3 filter). VMManager::m_worldLock (heap rank 3, held for
the window) serializes world transitions and conductor tenure but
owns NO membership — there is no second entered-thread structure
(ANNEX EXIT1.1 below, r28-r30).

**A.3.2 Per-thread NVS park tickets.** A stop request sets the stop
bit in every target lite (§A.2.3); threads park at poll sites
(cooperative only, jit R1.f-g) on their own ticket; the conductor
proceeds when every entered thread of every target VM is parked /
not-entered / access-released (sampled per ANNEX EXIT1 below) — the
last sound ONLY with rule 2b:

**A.3.2b Re-acquisition gate.** A JSThreads stop sets NO
client-visible GC stop state (Heap::JSThreadsStopScope only).
GIL-off: (i) acquireHeapAccess()/attachCurrentThread() polls the
lite's stop bit; set => F8 mandatory-revert (seq_cst
exchange->NoAccess) then park on its NVS ticket until resume; (ii)
every park site polls post-wake BEFORE re-acquiring access or running
JS/JIT. (i) carries soundness (AHA/RHA brackets are unenumerable —
heap §9 requires bracketing every indefinite block); (ii) is defense
in depth. Tokens are kept while parked; this makes the
access-released exemption sound (§A.3.4 gates FRESH acquisition).

**ORDERING (ANNEX SB1, BINDING — full text; r24 F4; SUPERSEDES the
r9-F3 item-3 ordering argument, both sides; the existing r9-F3 IH
supersession row gains this ordering text):**
1. Stop-bit fan-out stores (§A.2.3) are **seq_cst**. The
   VMLiteRegistry lock is retained for ENUMERATION only and carries
   no ordering duty. **[r28: the "ENUMERATION only" clause is
   SUPERSEDED by ANNEX EXIT1.2 below, both sides — the registry lock
   also OWNS the sampled set's membership/lifetime for every open
   §A.3 window (per-sample re-walks, no pointer caching); the
   no-ordering-duty clause for the stop-bit/access pair STANDS and
   item 4's proof is unchanged.]**
2. The conductor's per-client/per-lite access-state samples in the
   §A.3.2 predicate wait are **seq_cst loads** (executed inside the
   EXIT1.2 registry-walk lock hold).
3. The AHA stop-bit poll (2b(i)) is positioned AFTER the F8 step-1
   seq_cst CAS — beside the F8 step-2 GSP load — and is a seq_cst
   load; on set, F8 mandatory-revert then NVS park. Same position +
   ordering for the §A.3.4 token-acquisition stop-word check and the
   §F.4/DAL2 dtor re-acquire gate.
4. Proof (mirrors heap F8's): the four ops — conductor S1 =
   store(stop bit), L1 = sample(access); re-acquirer S2 =
   CAS(access), L2 = load(stop bit) — are all seq_cst, hence in ONE
   total order with S1 < L1 and S2 < L2. If L1 does not observe S2
   then L1 < S2, so S1 < L1 < S2 < L2 and L2 observes S1: the
   re-acquirer reverts and parks. If L1 observes S2, the conductor
   counts the thread HasAccess-unparked and keeps waiting. Either way
   no thread executes JS/JIT inside the stop window. **acq/rel is
   INSUFFICIENT** — both interleavings are store-buffering (SB)
   litmus shapes, observable on x86 and arm64 without the seq_cst
   total order.
5. Defense leg (ii) is unchanged and remains defense-only. §A.3.8
   needs nothing: it rides heap F8's GSP, whose ordering is pinned.
6. Tests: U4 gains a TSAN + litmus arm — conductor fan-out racing a
   release-then-immediately-reacquire loop, run on arm64 hardware;
   U20 lint: any stop-bit store/load not through the seq_cst
   accessors is flagged.

SUPERSESSION (heap §10A "never blocks" + the F8 AHA step list vs
this, both sides; IH row): GIL-off AHA gains the stop-bit gate; park
= F8 mandatory-revert BEFORE the NVS park. GIL-on/flag-off AHA = the
frozen F8 byte-for-byte. U4 litmus; U20 lint.

### ANNEX EXIT1 (BINDING, as AMENDED by rev 31 + the rev-32
### amendment record - the rev-28/29/30 texts are superseded where
### they differ. THIS INLINE COPY IS THE ANNEX OF RECORD AS
### AMENDED: rev 32 was recorded as an AMENDMENT RECORD (history
### "# REV 32"), not a full re-issue - its three re-issued
### paragraphs (EXIT1.9 step (2); EXIT1.9's Carrier-TLS-death
### disposition; the EXIT1.8 CARRIER-TLS-DEATH-DURING-DETACH arm)
### replace their rev-31 texts IN PLACE below and are
### byte-identical to the rev-32 amendment record; every other
### paragraph is the rev-31 annex verbatim; "A36 as AMENDED r31"
### in unamended paragraphs reads "as AMENDED r32" - the rev-32
### A36 amendment is the A36 text of record) -
### exit-during-stop-window lifetime: per-sample registry
### re-enumeration + TEARDOWN-mark-before-destroy + physical
### removal LAST + the EXIT1.9 ~VM completion fence + the r31/r32
### carrier-state handshake (amends
### §A.3.1, §A.3.2, §B.2, annex E2A's close tail, INV U3 and
### annex A36's ~VM-teardown clause; SUPERSEDES annex SB1 item
### 1's "ENUMERATION only" clause and vmstate §6.5.1/§6.4.4's
### assert-only ~VM fence, both sides)

Closed holes. (r28) Thread EXIT is never gated on the stop bit -
only acquisition is (§A.3.2b gates AHA/attach; §A.3.4 gates
entry) - so without a lifetime rule a spawned thread could
complete its §B.2 T5 teardown (free its GCClient::Heap and
VMLite) inside an open §A.3 window while the conductor keeps
issuing SB1.2 samples against cached pointers, dereferencing
freed memory. (r29 review, BLOCKER) rev 28's fix ordered
unregisterLite FIRST (before DCT/client-destroy), which silently
stripped the REGISTRATION-BASED VM-LIFETIME FENCE: pre-r28 the
lite stayed registered through the heap-touching teardown tail,
so vmstate §6.5.1's ~VM assert ("registry empty for this VM";
the VM.cpp walk; annex A36) fenced it; post-r28, between
unregisterLite and DCT, the exiting thread was invisible to that
walk yet still dereferenced the server JSC::Heap (DCT,
~GCClient::Heap) and VM::m_microtaskQueues (the M12 removal in
the lite free). join() notifies BEFORE T5 (ThreadObject.cpp:
236-244) and api §4.6.1 has no implicit join, so
embedder-destroys-VM-after-join raced the T5 tail - UAF. rev 29
split LOGICAL from PHYSICAL removal: a TEARDOWN lite state
supplies r28's conductor semantics; the physical removal returns
to the old position (LAST). (r30 review, BLOCKER) rev 29's
restored fence was ASSERT-ONLY and DEBUG-ONLY: the cited ~VM
walk (VM.cpp:652-658) is #if ASSERT_ENABLED, so a RELEASE build
retained the UAF verbatim - T_exit marks TEARDOWN, stalls
mid-DCT; the embedder's join() has already returned (settle
precedes unregister, ThreadObject.cpp:240-246 vs :259); ~VM
proceeds, destroys the server Heap; T_exit resumes DCT against
the freed heap, then the M12 removal against a destroyed VM. A
debug build merely converted the UAF into a nondeterministic
abort on a pattern the embedder cannot avoid (join is its ONLY
completion signal and fires pre-T5). rev 30 makes ~VM BLOCK: the
EXIT1.9 completion fence waits, under the registry lock, until
no registered lite other than m_mainVMLite points at this VM;
the assert walk is demoted to a post-wait debug sanity check.
(r30 review, MAJOR) rev 29's EXIT1.3 bound its order to "EVERY
teardown path - ... the ~VM walk", contradicting BINDING annex
A36, whose ~VM foreign-carrier collection unregisters FIRST and
DEFERS client+lite destruction to the owner's TLS destructor.
rev 30 scopes EXIT1.3 to live-VM paths, AMENDS A36 (full
server-side detach moves into the walk; deferred destruction
restricted to non-VM memory; the deferred M12 removal proven a
no-op), and pins the A36-collection-BEFORE-EXIT1.9-wait order so
the fence never waits on a TLS destructor. (r31 review, BLOCKER)
rev 30's amended A36/EXIT1.9 step (2) unregistered the carriers
under the registry lock, then RELEASED the lock and ran each
collected client's full server-side detach lock-free - but
nothing gated the carrier OWNER's OS-thread DEATH (re-entry is
API-lock-gated; thread death is not). A dying owner's TLS
destructor could not learn it had been collected: the
dead-detached discriminator was written by the walk AFTER the
lock release and read by the dying owner - a data race on the
discriminator itself - so the dtor either took the live
carrier-TLS-death path (a live ~GCClient::Heap: access bracket +
lastChanceToFinalize under MSPL + clientSet().remove racing the
walk's in-flight detach of the SAME client - double remove /
racing MSPL sections), or keyed on "unregistered" and fired the
dead-detached assert spuriously (release: freed client+lite
while the walk still held the pointers - UAF inside ~VM).
Holding the registry lock across the whole detach is NOT a legal
fix: the detach acquires MSPL and can PARK in the access bracket
(Heap.cpp:5078-5110), and LK.6 registry-lock holders acquire NO
lock and never wait (vmstate I7) - ILLEGAL on both counts. rev
31 closes it with a LOCK-PUBLISHED carrier-state handshake: the
registry-owned lite-state byte gains COLLECTED and DETACHED; the
walk marks COLLECTED (under the lock, BEFORE unregistering),
detaches lock-free, then flips COLLECTED->DETACHED under a short
re-hold and notifyAll()s vmTeardownCondition; the owner's TLS
destructor takes the registry lock FIRST and keys ONLY on the
state - LIVE => live path, COLLECTED => predicate-wait for
DETACHED, DETACHED => degenerate path. Rejected shapes:
defer-the-unregister (a dtor seeing REGISTERED takes the live
path and races the detach anyway - a COLLECTED state is needed
regardless, and a still-registered carrier deadlocks the EXIT1.9
wait); pinning the carrier's ThreadState ref
(ThreadManager.h:166-186 ThreadSafeRefCounted defers only
~ThreadState - the client+lite free lives in the TLS map
destructor, which a ref pin does not defer). Also fixed (r31
nits): the assert-walk cite is VM.cpp:652-658 (r30 wrote
:651-657), the ~VM API-lock assert is VM.cpp:649 (r30 wrote
:650), and the post-release lite free is ThreadObject.cpp:263
(r30 wrote :262).

EXIT1.1 Set identity. The entered-thread set the §A.3.2
conductor predicate samples IS the VMLiteRegistry (vmstate
§6.5.1), filtered lite->vm in the target VM set (§A.1.3 filter).
forEachEnteredThread(VM&, f) / numberOfEnteredThreads are
REGISTRY WALKS. VMManager::m_worldLock (heap rank 3) serializes
world transitions and conductor tenure but owns NO membership:
there is no second entered-thread structure, hence no
two-structure consistency protocol to state.

EXIT1.2 Per-sample re-enumeration (SUPERSESSION, both sides:
annex SB1 item 1's "retained for ENUMERATION only and carries no
ordering duty" - the registry lock now OWNS THE SAMPLED SET FOR
THE LIFETIME OF EVERY OPEN §A.3 WINDOW; its no-ordering duty for
the stop-bit/access Dekker pair STANDS - the SB1.4 seq_cst proof
is unchanged and the lock carries the LIFETIME duty only).
Normative: every conductor predicate sample RE-WALKS the
registry UNDER VMLiteRegistry::lock; lite/client pointers are
NEVER cached across samples (including from the §A.2.3 fan-out
walk - the fan-out enumeration is one walk, each subsequent
sample is a fresh walk); every SB1.2 seq_cst access-state load
executes INSIDE the lock hold of the walk that found that lite;
the walk is allocation-free, acquires nothing (§LK.6 inner set
suffices for nothing here - the walk takes NO inner lock), and
the registry lock is DROPPED before the conductor blocks/yields
between samples (registry-lock holders never wait, vmstate I7
class).

EXIT1.3 TEARDOWN-mark-before-destroy, physical removal LAST
(r29; path scope + fence wording AMENDED r30; state-byte values
+ cites AMENDED r31; amends §B.2 +
annex E2A's close tail + INV U3, both sides; NO exit gating
added; REPLACES rev 28's unregisterLite-first order). On every
LIVE-VM lite/client teardown path - spawned T5 and carrier
TLS-death - LOGICAL removal precedes any destruction and
PHYSICAL removal comes LAST: under VMLiteRegistry::lock the
exiting thread marks its lite TEARDOWN (the registry-owned
lite-state byte - values LIVE/TEARDOWN/COLLECTED/DETACHED, r31;
EVERY transition AND every read under VMLiteRegistry::lock; the
lite stays PHYSICALLY registered;
conductors count it EXITED per EXIT1.4(a)); THEN DCT and
GCClient::Heap destruction; THEN VMLiteRegistry::
unregisterLite(lite) (under the registry lock); the lite is
freed by its owner AFTER unregisterLite returns (the registry
lock is not recursive; the free - and the M12 default-queue
removal inside it - runs outside the lock: the EXIT1.9
residual-tail rule covers it). T5 order: Strong clears -> access
release (seq_cst RHA, F8) -> TEARDOWN mark (registry lock) ->
DCT -> destroy GCClient::Heap -> unregisterLite -> free lite.
SCOPE (r30): the ~VM foreign-carrier collection is EXPLICITLY
EXCLUDED from this order - it follows annex A36 as AMENDED r31
(cross-ref both sides; the A36 annex carries the [r31] marker):
COLLECTED-mark then unregister FIRST (one lock hold; TEARDOWN
lites SKIPPED - their owner is mid-live-detach and the EXIT1.9
step-(3) wait covers them), full server-side detach inside the
walk, client+lite destruction DEFERRED to the owner's TLS
destructor and restricted to non-VM memory; EXIT1.9 step (2)
pins its ordering against the ~VM wait. rev 29's "on EVERY teardown path
incl. the ~VM walk" claim is WITHDRAWN (it contradicted BINDING
A36). The registration-based VM-lifetime fence: the registry
stays non-empty for this VM until the server-touching tail (DCT,
~GCClient::Heap) is done, and rev 30's EXIT1.9 wait - NOT the
debug assert walk - is what enforces it, in every build
configuration. Exit remains UN-GATED: no stop-bit poll, no park
point, no new deadlock edge; E2A's close sequence BEFORE T5
(deadline harvest, residue routing, F1/F5) is unchanged. vmstate
§6.5.1's lifetime contract (unregistered before destroyed) is
PRESERVED (physical removal still strictly precedes the lite
free); vmstate N8's "unregister under the final JSLock hold"
clause is the GIL-on/carrier shape and is untouched (GIL-off
spawned threads hold no m_lock, §F.1); ~VM's own m_mainVMLite
handling already complies (unregistered after the EXIT1.9 wait +
sanity walk, before being destroyed; no TEARDOWN mark needed
there - a VM inside ~VM has no live conductors, embedder
contract §F.6).

EXIT1.4 Predicate disposition of a TEARDOWN/absent/clientless
lite. (a) A lite marked TEARDOWN - and a lite ABSENT from the
current walk - counts as EXITED (r31: ANY non-LIVE state counts
EXITED - defensive only for COLLECTED/DETACHED, which are
unregistered in the same hold that marks them COLLECTED and are
never conductor-visible; §F.6 - no conductors inside ~VM). Soundness (r29 re-stated): the
TEARDOWN mark is set only AFTER the exit path's seq_cst access
release (EXIT1.3 order), and the mark's registry-lock unlock
happens-before any walk that observes it, so a conductor that
sees TEARDOWN (or misses the lite) has the thread's NoAccess
release ordered before the sample - the same argument as r28's
absence rule, and STRONGER in one respect: because the lite
stays registered until after client destroy, every walk sample
touches LIVE memory by construction (walks never dereference
beyond lite fields owned by the registry, and never the
clientHeap of a TEARDOWN lite - counted EXITED at (a) before any
client deref). A TEARDOWN lite's access RE-ACQUISITION IS
FORBIDDEN (the §A.3.2b/SB1-gated acquire refuses it; asserted):
re-entry to JS would need re-acquisition, so a TEARDOWN lite can
never run JS again - re-entry requires FRESH registration +
§A.3.4-gated token acquisition (the VM stop word, ORed in at
acquisition per §A.2.3, gates entrants that registered AFTER the
fan-out walk - they park before completing entry and appear in
later walks as not-entered). (b) lite->clientHeap is written
ONCE per registration epoch (§B.1 spawn / §F.1 first carrier
entry), with a release store, BEFORE the thread's first access
acquisition, and is never nulled or repointed while the lite is
registered. A sampler reading null counts the lite
not-entered/no-access - sound: access cannot be held without a
client, and acquisition is §A.3.2b-gated. A sampler reading
non-null on a NON-TEARDOWN lite under the walk's lock hold
dereferences a live client (EXIT1.3: destroy is fenced behind
the TEARDOWN mark, and the mark waits for the walk's lock).

EXIT1.5 Why both interleavings die. Conductor-vs-T5 (r28's UAF -
that fix is fully preserved): every conductor dereference of a
lite/client happens inside a registry-lock hold of a walk that
found the lite registered and NOT TEARDOWN; T_exit's TEARDOWN
mark must WAIT for any in-progress walk to drop the lock; after
it, every walk counts the lite EXITED and dereferences nothing
further; DCT/client-destroy are program-ordered after the mark,
unregisterLite/lite-free after those. No sample ever touches
freed memory - zero new park points on exit, no change to the
SB1 ordering proof. Embedder-destroy-vs-T5 (the r29 BLOCKER;
fence made REAL r30): the exiting thread stays registered
through the whole server-touching tail, and ~VM BLOCKS in the
EXIT1.9 wait until that thread's unregisterLite signals - so
join-then-destroy-VM (join settles BEFORE T5,
ThreadObject.cpp:240-246 vs :259; api §4.6.1 has no implicit
join) completes WITHOUT UAF in release AND debug builds. rev
29's assert-only form had left the release-build UAF intact and
made debug builds abort on a pattern the embedder cannot avoid.

EXIT1.6 Lock-order argument (§LK; no rank change). The conductor
holds VMManager::m_worldLock (heap rank 3, inside the LK.5
frozen heap block) for the window (§A.3.1) and acquires
VMLiteRegistry::lock (§LK.6) per sample: strictly outer -> inner
in the LK order, acyclic. Registry-lock holders acquire nothing
and never wait (LK.6 inner set untouched by the walk AND by the
TEARDOWN mark; vmstate I7 class), so no new edge appears in
either direction; the LK.6 fastMalloc-excluded-while-suspended
carve-out is unaffected (§A.3 conductors suspend nobody - that
carve-out belongs to §A.1.7 readers). Exit side: the TEARDOWN
mark and unregisterLite at T5 run access-released holding NO api
or heap lock (E2A's close dropped inboxLock before T5) - no new
edge. The §A.2.3 fan-out walk already took the registry lock;
its rank position is unchanged. ~VM-wait side (r30): at the
EXIT1.9 wait, ~VM holds the API lock (VM.cpp:649) and acquires
the registry leaf - an EXISTING legal edge (api 5.9 leaf;
ThreadObject.cpp:256-259 already takes the registry lock under
the final JSLock). While parked, Condition::wait RELEASES the
registry lock into the parking lot and re-acquires it before
returning (WTF::Condition semantics - pinned), so no thread
ever waits while OWNING the registry lock: vmstate I7's
holders-never-wait class is preserved in the ownership sense,
and unregisterLite can always acquire the lock and signal. The
waited-on threads acquire only the leaf registry lock in the
tail and never any api lock (§F.1: GIL-off spawned threads hold
no m_lock, so the API lock the waiter holds is unreachable from
them) - no edge back to the waiter: acyclic; the wait always
makes progress. Carrier-dtor side (r31): a TLS destructor parked
in its COLLECTED->DETACHED wait OWNS nothing (Condition::wait
drops the registry lock into the parking lot) and holds no api
or heap lock (TLS-death runs outside any VM entry); the walk's
per-client DETACHED-flip hold is short and acquires nothing
(LK.6); the walk never parks while holding the registry lock -
no new edge in either direction, acyclic.

EXIT1.7 INV + amendment record.
- INV U32 (r30 form): no VMLite or GCClient::Heap is destroyed
  or freed while observable to any §A.3 fan-out or
  predicate-sample registry walk as a live (non-TEARDOWN) lite -
  the TEARDOWN mark precedes DCT/client-destroy, physical
  removal comes LAST, and conductors hold no lite/client pointer
  across sample boundaries; no lite leaves the registry before
  its server-touching teardown tail completes; and ~VM BLOCKS
  (EXIT1.9) until no registered lite other than its m_mainVMLite
  has lite->vm == this - the NORMATIVE completion fence (the
  assert walk is a post-wait debug sanity check).
- INV U3 (unchanged from r29; the rev-9 annex 1 row's [r29]
  marker stands): "lite -> ACT -> alloc; Strong clears -> access
  release -> TEARDOWN mark (registry lock) -> DCT -> destroy
  client -> unregisterLite/free lite" (EXIT1.3; the free runs
  outside the registry lock per EXIT1.3/EXIT1.9).
- INV U4 keeps the EXIT1.8 exit-storm arm.
- Carrier-state machine (r31, NORMATIVE): LIVE -> TEARDOWN
  (owner's live path) | LIVE -> COLLECTED -> DETACHED (~VM
  walk); TEARDOWN and DETACHED terminal; no other transitions;
  EVERY transition AND every read under VMLiteRegistry::lock.
  The state byte - NEVER "is my lite registered" - is the sole
  owner-vs-walk discriminator.
- ANNEX A36 AMENDED (r30; re-AMENDED r31, both sides; the A36
  annex carries the in-place [r31] marker; TERM1.5's "A36 stands
  unamended" keeps its [r30] scope note - its single-VM clause
  stands): carrier collection marks COLLECTED then unregisters
  BEFORE the EXIT1.9 wait; full server-side detach inside the
  walk, lock-free; per-client COLLECTED->DETACHED flip +
  notifyAll under a short re-hold; deferred destruction
  restricted to non-VM memory (degenerate path gated on
  DETACHED; M11/M12 no-op queue removal). Full text: the rev-31
  A36 amendment record.
- vmstate §6.5.1 SUPERSESSION extended (both sides; IV row):
  VMLiteRegistry gains one WTF::Condition; unregisterLite
  notifyAll()s it under the lock after removal; the walk's
  DETACHED flips notifyAll it too (r31); BOTH waiters - the
  step-(3) ~VM wait and the COLLECTED dtor wait - are predicate
  loops, so cross-wakeups are benign; the §6.4.4 ~VM
  assert becomes wait-then-debug-assert (EXIT1.9).

EXIT1.8 Tests + lint.
- Corpus/litmus (U-T5 + U-T6, U4 arm): EXIT-STORM-UNDER-STOP-
  STORM - N threads spawn, run briefly, and exit in a tight loop
  while a conductor thread fires back-to-back §A.3 stops
  (Class-A fire or a synthetic test-only conductor); ASAN + TSAN
  clean; the race-amplifier variant injects delays between every
  T5 step (post-release, post-mark, post-DCT, post-destroy) and
  inside the conductor's between-sample gap. Carrier variant:
  embedder TLS-death teardown racing a stop window.
- T5-TAIL-VS-~VM arm (r29; REWRITTEN r30; joins the U-T6 gate
  list). RELEASE + ASAN build (load-bearing - the r29 form
  "observed" a debug-only assert): embedder join()s a spawned
  thread then immediately destroys the VM while the amplifier
  stalls the joined thread inside the T5 tail (variants:
  post-mark pre-DCT, mid-DCT, mid-client-destroy,
  pre-unregister): ~VM must BLOCK in the EXIT1.9 wait and return
  only after the stalled thread's unregisterLite (instrumented
  ordering check), with no UAF on the server Heap or
  VM::m_microtaskQueues; ASAN clean - the wait absorbs
  arbitrarily long stalls. DEBUG variant additionally exercises
  the post-wait sanity walk (passes: only m_mainVMLite remains).
  CARRIER variant (r30, A36): a foreign carrier still registered
  at ~VM is collected + unregistered BEFORE the wait (the wait
  never counts it); the amplifier delays the owner's TLS
  destructor past VM destruction; the deferred degenerate dtor +
  the no-op M12 removal (isOnList() false after the M11
  force-removal) touch no VM/server memory; ASAN clean.
- CARRIER-TLS-DEATH-DURING-DETACH arm (r31; +WALK-FREE variant
  r32; joins the U-T6 gate list; DEBUG AND RELEASE builds,
  ASAN): the amplifier stalls the ~VM walk INSIDE a collected
  client's lock-free server-side detach (variants:
  post-unregister pre-detach, mid-lastChanceToFinalize,
  post-detach pre-flip) while the owner thread exits;
  instrumented checks: the owner's TLS destructor takes the
  registry lock, reads COLLECTED, parks on vmTeardownCondition,
  and runs the degenerate path only AFTER the walk's DETACHED
  flip (ordering check) - no double clientSet().remove, no
  concurrent MSPL section on the same client, no UAF; ASAN
  clean. Reverse variant: the dtor wins the lock BEFORE
  collection - sees LIVE, marks TEARDOWN, takes the live path;
  the walk SKIPS the TEARDOWN lite and the step-(3) wait
  absorbs it. WALK-FREE variant (r32; walk-side disposition
  racing a late-firing TLS dtor): the process MAIN thread
  enters the VM (bit-SET carrier, destructor-free map) and an
  embedder thread enters it too (bit-CLEAR); ~VM runs on a
  third thread while the amplifier (i) stalls the walk between
  the bit-set lite's DETACHED flip and its degenerate free and
  (ii) fires the embedder owner's TLS destructor inside that
  window; instrumented checks: the walk frees the bit-SET lite
  exactly ONCE and no destructor ever visits it
  (registration-time instrumentation: a bit-set lite never has
  a destructor-bearing map entry, and vice versa); the
  embedder's dtor parks on COLLECTED per the disposition
  paragraph; the bit-CLEAR lite is NEVER walk-freed - the r30
  CARRIER variant (dtor delayed past VM destruction) re-asserts
  the unconditional deferral: no walk-side free occurred for
  it; ASAN clean.
- U20 lint: §A.3 conductor code must reach lites ONLY via the
  forEachEnteredThread registry-walk helper; any lite*/client*
  value in conductor code that crosses a sample boundary
  (escapes the walk's lock scope) is flagged; teardown paths are
  checked for TEARDOWN-mark-precedes-DCT/client-destroy AND
  unregisterLite-LAST (after client destroy, before the lite
  free); ~VM is checked for EXIT1.9-wait-precedes-teardown (the
  wait before notifyVMDestruction/heap teardown) and the A36
  deferred-dtor path for naming NO m_server or VM member. r31:
  EVERY physical removal from the registry must be a
  VMLiteRegistry::unregisterLite call (the notifying function) -
  the A36 collection and ~VM's m_mainVMLite removal INCLUDED;
  any hand-rolled lites mutation is flagged; any lite-state read
  or write outside a registry-lock hold is flagged.

EXIT1.9 ~VM completion fence (r30; step (2) + the carrier
disposition AMENDED r31/r32; NORMATIVE; SUPERSESSION, both
sides: vmstate §6.5.1/§6.4.4's assert-only "registry empty for
this VM" - the VM.cpp:652-658 #if ASSERT_ENABLED walk - and
A36's assert wording vs this; IV row). Mechanism: VMLiteRegistry
gains one WTF::Condition (vmTeardownCondition) beside lock;
unregisterLite - already under the lock - notifyAll()s it after
removing the lite. ~VM order at the §6.4.4 top:
(1) uninstall the main carrier TLS (unchanged);
(2) the A36 foreign-carrier collection (as AMENDED r32 - the
carrier-state handshake): under ONE registry-lock hold, each of
this VM's carriers not marked TEARDOWN is
token-free-RELEASE_ASSERTed, marked COLLECTED (the lite-state
byte - the lock-published discriminator the owner's TLS
destructor keys on), and PHYSICALLY UNREGISTERED via
unregisterLite (U20: every physical removal is an unregisterLite
call); TEARDOWN lites are SKIPPED (owner mid-live-detach, still
registered - step (3) covers them). The lock is RELEASED; the
walk performs the full server-side detach of each COLLECTED
client lock-free (the detach acquires MSPL and can park in the
access bracket - holding the registry lock across it is ILLEGAL,
LK.6/I7); after EACH client's detach the walk re-acquires the
registry lock, flips COLLECTED->DETACHED, notifyAll()s
vmTeardownCondition, drops the lock (short hold; acquires
nothing), and NEVER touches that lite/client again - EXCEPT
(r32) a lite whose ownerHasNoTlsDtor bit is set (FIXED AT
REGISTRATION TIME under the registry lock, A36 as AMENDED r32:
set iff the registering thread is the process main thread,
whose carriers live in the destructor-FREE map - a static
structural fact, NEVER a liveness probe): no destructor is ever
installed over a bit-set lite's storage on any platform, so no
competing dtor exists BY CONSTRUCTION, and the walk itself runs
the degenerate free immediately after that lite's DETACHED flip
(outside the lock; the flip is still made and notified - the
bit changes only the freeing party, not the handshake). The
main thread's TLS-map entry for this VM then dangles - never
consulted: lock() compares the process-monotonic VM epoch
BEFORE the cached carrier (A36 staleness rule), and re-entry
during ~VM is excluded (the m_lock gate in the disposition
paragraph below). A bit-CLEAR lite is NEVER walk-freed -
destruction is unconditionally deferred to its owner's TLS
destructor. ALL of step (2) precedes step (3), so the wait
never counts a carrier whose deferred TLS destructor runs at an
unbounded future time (no deadlock on A36's deferral);
(3) the WAIT: under VMLiteRegistry::lock, while any registered
lite other than m_mainVMLite has lite->vm == this:
vmTeardownCondition.wait(lock). THIS WAIT IS THE NORMATIVE
COMPLETION FENCE for the T5 server-touching tail; the
pre-existing assert walk is DEMOTED to a post-wait debug sanity
check;
(4) only after the wait: unregisterLite(m_mainVMLite) and the
rest of ~VM (notifyVMDestruction, lastChanceToFinalize, the M11
force-removal, heap/member teardown).
Boundedness/progress: every lite counted at (3) belongs to a
spawned thread past its F5 join-settle (or being driven to close
by E2A/§E.5); its remaining work is straight-line teardown that
runs access-released holding NO api or heap lock (EXIT1.6) and
acquires ONLY the leaf registry lock, which the parked waiter
does NOT own (Condition::wait drops it into the parking lot) -
it always reaches unregisterLite and signals. ThreadObject's
join settle order is UNCHANGED and api §4.6.1 still has NO
implicit join: the embedder needs no new contract - the wait
alone makes join-then-destroy-VM safe.
Carrier-TLS-death disposition (r31; AMENDED r32): the owner's
TLS destructor takes the registry lock FIRST and keys ONLY on
the lock-published lite-state - NEVER on whether its lite is
still registered: LIVE => mark TEARDOWN in the same hold and
take the live EXIT1.3 path; COLLECTED => predicate-wait on
vmTeardownCondition until DETACHED (the wait drops the registry
lock; unregisterLite notifies are tolerated - predicate loop),
then the degenerate path; DETACHED => the degenerate path
immediately. A TLS destructor exists ONLY for bit-CLEAR lites
(r32: ownerHasNoTlsDtor set => destructor-free map; the walk
freed that lite post-flip and no dtor will ever visit it). A
COLLECTED owner's re-entry is excluded for the duration of ~VM:
re-entry requires fresh §F.1 registration under m_lock, which
~VM holds (VM.cpp:649). The lite (and its state byte) is freed
ONLY by the path that observed DETACHED (the owner's dtor for a
bit-CLEAR lite; the walk itself, post-flip, for a bit-SET lite)
or by the live path's own free, and the owner cannot pass its
COLLECTED wait before the walk's DETACHED flip - the walk's
LAST touch of a bit-CLEAR lite - so the byte is never read
after free and the walk's pointers never dangle. Progress: the
COLLECTED wait depends only on the ~VM walk, which is running,
straight-line, and never blocks on the dtor (its collection and
flip holds are short; its detach work is lock-free); acyclicity
per EXIT1.6 (no thread waits while OWNING the registry lock).
Shared condition (both sides with step (3)):
vmTeardownCondition is notified by unregisterLite (r30) AND by
the walk's DETACHED flips (r31); both waiters are predicate
loops, so cross-wakeups are benign.
Residual tail OUTSIDE the fence (r30 precision; amends r29's
"M12 removal inside the fence" wording, both texts here): the
lite free - and the M12 removal of the lite's default
MicrotaskQueue inside it - runs AFTER unregisterLite returns
(the registry lock is not recursive; ThreadObject.cpp:263
already frees post-release), so the wait can return while it is
still pending. It is safe by the M11/M12 protocol, not by the
fence: the queue dtor (MicrotaskQueue.cpp:128-141) takes the
PROCESS-LIFETIME registry lock (VMLiteRegistry is
NeverDestroyed) and is isOnList()-guarded; ~VM's M11
force-removal runs under the same lock before any VM memory dies
(VM.cpp:710-719: "Locker locker {
VMLiteRegistry::singleton().lock }; while
(!m_microtaskQueues.isEmpty())
m_microtaskQueues.begin()->remove();"). Lock-serialized cases:
queue dtor first => the list is a live VM member (M11 has not
yet run; ~VM destroys members only after M11) - a legal remove;
force-removal first => isOnList() is false and the dtor touches
ONLY its own SentinelLinkedList node - the landed comment
anticipates exactly this race ("~VM's force-removal (M11) can
race a dying queue's dtor on another thread post-GIL"). The same
argument covers A36's deferred carrier queues (the rev-31 A36
amendment record).

**A.3.2c JIT re-entry sync (ANNEX ISB1, BINDING — full text; r20
F3).** Stops patch code while access-released threads sleep
UN-parked; jit R1.d's ISB fires only on NVS exit — so:
1. State: one process-wide seq_cst uint64 **stop-generation counter**
   (JSCConfig-adjacent, GIL-off only); EVERY §A.3 conductor AND every
   heap §10 conductor that patched/jettisoned code (Class-A fire,
   jettison, D1R rebias fire; cheap conservative form: every
   conductor) increments it INSIDE the window, before resume.
   Per-lite uint64 copy (L2 append).
2. Rule (normative, GIL-off): every transition into "may execute JIT
   code" that did NOT pass through an NVS exit — F8 AHA
   re-acquisition (incl. 2b's bit-already-clear path, §J.3/§E.2
   wakes, the DAL2 dtor, the §F.5 LIFO restore), §F token
   acquisition, ACT — loads the global counter, compares the per-lite
   copy, and on mismatch executes a context-synchronizing instruction
   (arm64 ISB; x86-64 serializing instruction, e.g. cpuid or
   membarrier) BEFORE any JIT-code entry, then stores the new value.
   NVS exit keeps the unconditional R1.d ISB and ALSO refreshes the
   per-lite copy.
3. SUPERSESSION (jit F5's NVS-exit-only delivery, SPEC-jit.md:156 +
   INTEGRATE-heap.md:608, vs this, both sides; IJ row; frozen jit
   text stands unedited): the delivery SET is WIDENED to item 2's
   transitions; the protocol itself (data -> flush -> resume ->
   per-mutator sync) is unchanged. ANNEX D1R item 2's reliance is
   re-cited to this annex.
4. Alternative recorded, NOT chosen v1: patcher-side process-wide
   membarrier(SYNC_CORE) after patching before resume; rejected for
   portability (no Windows/macOS twin in-tree) — revisit post-ungil
   if the compare cost shows in §B.5.
5. Cost: GIL-on/flag-off zero (counter never bumps; compare branch on
   GIL-off-only paths). GIL-off steady state: one relaxed load +
   compare per access/token transition.
6. U-T5 arm (arm64 amplifier): conductor jettisons during a stop
   while a thread sleeps access-released through it; the sleeper
   re-enters via AHA and executes the patched region; TSAN/exec
   corpus asserts the new code runs. U20 lint: may-execute-JIT
   transitions missing the generation check.

**A.3.3 R1.c re-frozen (conductor release) + ORDER PIN (ANNEX HBT4,
BINDING — full text; promotes HBT3 item 3 to ALL conductors).**
Arbitration releases exactly one requesting THREAD as conductor; the
park-aware mutex on the pending-job slot is keyed by thread; losers
PARK during the winner's stop, then retry; a SAME-VM second thread
participates fully.
1. ORDER (normative, ALL §A.3 conductors, default AND class-4):
   release access (R1.i's first step KEPT) -> §A.3.3 arbitration on
   the park-aware pending-job-slot mutex -> (WINNER ONLY)
   Heap::JSThreadsStopScope (GCL) -> fan stop bits -> stop -> work ->
   resume -> drop scope -> re-acquire access. Losers park on the
   job-slot mutex access-released (counts as parked for the winner's
   §A.3.2 predicate) and NEVER block raw on GCL (heap §10.4/§A.3.8
   never wait on it — HBT4.3).
2. SUPERSESSION (both sides; frozen jit text stands unedited,
   superseded here; IJ rows): (a) jit R1.i's step order "release
   access -> JSThreadsStopScope (GCL) -> stop" (SPEC-jit.md:227-234)
   — the GCL bracket moves AFTER arbitration for every conductor; (b)
   jit §7's table edge "[Heap GCL (rank 2) — ONLY inside STWR via
   R1.i] > [R1/VMM world-stop ownership (STWR)]" (SPEC-jit.md:
   164-167) — INVERTED: job-slot/STWR arbitration is OUTER to GCL.
   R1.i's access-release-first step, client scoping, resume order,
   and (for default conductors) allocation-free closure all stand.
3. Soundness: at most ONE thread — the arbitration winner — is ever
   blocked in GCL.lock(), and it blocks access-released, so the heap
   §10.4 barrier and §A.3.8 per-thread GC parking never wait on it;
   it queues behind any in-progress shared GC (heap §10C(b)/(e)
   shapes unchanged). GC conductors never touch the job slot (§LK
   negative edges; the slot is §A.3-conductor-only), so no cycle
   through GC.
4. §LK row 4b — pending-job-slot mutex: §A.3-conductors ONLY; inner
   to rank 1/token (the requester holds its entry token entering
   arbitration; tokens ordering-inert per LK.1); OUTER to heap rank 2
   (GCL); held across the ENTIRE stop window; losers park on it
   access-released; never held together with any api **rank-1..3**
   lock (NLH1 amendment — see §LK.4b: long-hold NLS::m_lock is
   EXCLUDED from "api lock" here; a conductor MAY hold NLS on entry,
   never ACQUIRES it). U20 lints it.
5. Licensed edits at U-T5 (IJ rows): (a) the
   JSThreadsSafepoint.cpp:252-304 bracket is reordered — the
   arbitration call moves BETWEEN the access release and the
   JSThreadsStopScope ctor; (b) the :208-221 "Real sequence (R1.a-i),
   restored at integration" comment is REWRITTEN (arbitration becomes
   step 2, GCL step 3) — restoring the landed comment verbatim builds
   the deadlocking order. The §A.3.5 DEFAULT bracket is therefore no
   longer "the landed lines verbatim".
6. Class-4 (§A.3.5(i)/HBT3) is unchanged and is now an INSTANCE of
   the general order, not an exception.

**A.3.4 Entry during a stop parks.** Token acquisition (§F) checks
the stop word (SB1 ordering), parks on a fresh ticket before
completing entry. Licensed deletions: the JSThreadsSafepoint.cpp stub
assert + evidence walk + s_stubWorldStoppedDepth; the M7 tripwire
VMEntryScope.cpp:44-70.

**A.3.5 R1.i GC bracket (W5).** DEFAULT = access-release -> rule-3
arbitration -> Heap::JSThreadsStopScope (:252-304, GCL AFTER
arbitration, HBT4.5), client-scoped, allocation-free closure, own
client.

(i) **CLASS-4 variant (ANNEXES HBT2 + HBT3, BINDING — full text;
§K.5 conductors ONLY):**
- HBT3.1: the class-4 conductor KEEPS the default R1.i access-release
  BEFORE the JSThreadsStopScope (GCL) acquisition. Access retention
  begins ONLY once GCL is HELD; what remains superseded for class-4
  conductors is (a) the "access released" precondition FOR THE HELD
  SCOPE (the conductor re-acquires inside it) and (b) R1.i's
  allocation-free-closure rule.
- HBT3.2: immediately after the JSThreadsStopScope ctor returns (GCL
  held), BEFORE fanning its own §A.3 stop bits, the conductor
  re-acquires access on its OWN client via plain F8 AHA. Non-blocking
  proof: GSP is seq_cst-cleared at §10.8 before the prior GC
  conductor releases GCL at step 9, so under OUR held GCL no GC is in
  progress and no new §10.2 election can complete — GSP is false, F8
  step (3) never triggers; the §A.3.2b stop-bit gate sees no pending
  §A.3 stop (this conductor has not yet set its bits; no other §A.3
  conductor can exist past arbitration); so AHA is one CAS, no park.
  Only then does the conductor fan stop bits and wait for the §A.3.2
  predicate.
- HBT2.1 (as re-scoped by HBT3): SUPERSESSION (heap §9
  JSThreadsStopScope "pre: access released", SPEC-heap.md:201-205 +
  jit R1.i "Closures: allocation-free", vs this, CLASS-4 ONLY, both
  sides; IH + IJ rows): the class-4 conductor RETAINS heap access on
  its OWN GCClient::Heap across the (post-acquisition) stop window
  and MAY allocate from it. Soundness (post-acquisition window): the
  §A.3.2 predicate requires every OTHER entered thread
  parked/not-entered/access-released, so exactly one access-held
  client exists — the conductor; heap F8/§10.4 barriers never wait on
  it (no shared GC can be IN PROGRESS: the §10.2 election cannot
  complete against the conductor's GCL scope, and rule (ii) forbids
  the conductor starting one); §A.3 sets no client-visible GC stop
  state, so no GC barrier is active in-window. The default R1.i
  bracket REMAINS the rule for every non-class-4 §A.3 closure.

(ii) **NO-GC-IN-WINDOW (HBT2.2, normative, ALL §A.3 stop windows, any
closure class).** SUPERSESSION (heap §9 CSAC/RCAC "not in stop
window" precondition, SPEC-heap.md:184, vs this, both sides; IH row):
inside a §A.3 window, GC initiation is FORBIDDEN; the §10.2 election
is NEVER entered. CIND, DeferGC-exit
(decrementDeferralDepthAndGCIfNeeded — e.g. haveABadTime's
function-scoped DeferGC dies inside the closure), and allocation slow
paths reached by the conductor instead ENQUEUE a ticket (RCAC arm
only, under m_threadLock — legal: rank 5, taken stop-free) and
RETURN; the deferred-GC check re-runs on the conductor AFTER resume +
scope exit, where the normal §10 protocol serves the ticket. Debug
enforcement: the conductor brackets the window in heap I14's
STW-forbidden counter (incrementSTWForbiddenScope), which
CSAC/SINFAC entries already check. HBT2.3: in-window allocation
failure: the conductor's allocation may take the heap L3 conductor
allowance (MSPL freely, world is stopped) to grow/handout
directories; if memory is truly exhausted it FAILS HARD
(RELEASE_ASSERT/OOM crash) — it never collects in-window. Pre-sizing
(Vector::reserveCapacity before requesting the stop) is RECOMMENDED,
non-normative. HBT2.4: the original HBT item-2 sentence "an emergency
shared GC inside the window degenerates to the single-client case" is
SUPERSEDED (it was the unsound arm): there is NO in-window shared GC.
HBT2.5/HBT3.5 corpus (U-T13): haveABadTime fired from a thread whose
conversion walk must grow (large found-set) with a deliberately tiny
nursery + a pending GC ticket — ticket served post-resume, no
in-window election, I14 counter clean; haveABadTime fired while a
shared GC is mid-§10.4-barrier (force via a second mutator parked in
native code delaying its NoAccess release) — no deadlock, the class-4
stop runs strictly after the GC resumes, the conductor's AHA never
parks (instrumented).

**A.3.6 Main/embedder carriers (ANNEXES A36 + A36C, BINDING — full
text; vmstate §6.4.4).**

GIL-off EVERY thread uses a real carrier lite with a TM-allocated
unique TID, lazily installed at first entry; m_mainVMLite (tid 0) is
GIL-on-only. Carriers are per-(thread,VM) in a TLS VM->carrier map
(r32 registration clause — TWO slots, chosen ONCE at §F.1
first-entry registration: every NON-MAIN thread's carriers live in
the destructor-BEARING WTF::ThreadSpecific map whose TLS destructor
IS the carrier-TLS-death path — registration ALWAYS installs it for
those threads; the process MAIN thread's carriers live in a
destructor-FREE plain thread_local map — pthread TLS destructors
run only at pthread_exit and never for a thread exiting via
exit()/return-from-main (ThreadSpecific.h:31-40 documents the
pthread/Windows cleanup split), and a late Windows FLS callback
over the same storage would re-read a walk-freed lite, so NO
cleanup is ever installed over the main-thread slot on ANY platform
(entries leak at process exit unless a ~VM walk frees them —
accepted). The choice is recorded per-lite as ownerHasNoTlsDtor,
FIXED AT REGISTRATION TIME under the registry lock — set iff
WTF::isMainThread() at §F.1 registration — immutable thereafter,
read under the registry lock like the state byte (U20): a static
structural fact, NEVER a liveness probe);
lock() (still per-VM m_lock, §F.1) installs the entered VM's carrier
as CURRENT lite AND swaps the jit P5/CS3 butterfly-TID-tag TLS to its
TID, restoring the prior tuple LIFO on release (nested entry: §F.5).
Install precedes any allocation/OM fast path; tag cleared at
teardown; never tag 0 or a foreign-VM TID (TTL/§D.1). **Spawned
Threads are single-VM in v1 (foreign-VM token RELEASE_ASSERTs;
TERM1.5).** U1: TLS tag == CURRENT lite TID && lite->vm == entered
VM. JSLock.cpp:151 backstop REPLACED (§J.7). Lazy embedder TIDs count
vs 2^15 (Dev 10; §D lifts).

TID SUPERSESSIONS (r9 F4, full text; both sides; IV rows): vmstate
§6.7 "Main carrier tid stays 0" is GIL-ON-ONLY; main/embedder TS.tid
STAYS 0 — thr.id/Thread.current.id unchanged, NO new SD; the carrier
lite TID is a separate nonzero TM allocation from the same 2^15 space
(I17 exhaustion accounting includes carriers); currentTID() GIL-off
returns the CARRIER TID (it feeds tagging/TTL consumers, never JS);
api 5.2's lite->tid==ts->tid equality is SPAWNED-only — main/embedder
TS.tid and carrier TID intentionally diverge. OM §2's tid-0 note is a
perf remark only: GIL-off main-allocated butterflies carry the
nonzero carrier tag; correctness unaffected (both-modes note, not an
SD). This kills the JSLock.cpp:151 two-embedders-share-tag-0 race an
implementer following frozen vmstate §6.7 would have shipped.
(Rationale: the rejected alternative — embedder threads share a tid-0
"view" — is exactly the configuration JSLock.cpp:136-148 documents as
unsound GIL-off; unique lazy TIDs make embedder threads ordinary
mutators to the OM machinery; api SPEC-api.md:361 already promised
"post-GIL: real TID lazily at first VM entry".)

~VM teardown (SUPERSESSION: vmstate M6 + §6.5.1 assert vs this, both
sides; clause AMENDED r31, re-AMENDED r32 — the rev-32 A36 amendment
is the text of record). The registry-owned lite-state byte (EXIT1.3) gains two
values — the carrier state machine is LIVE -> TEARDOWN (owner's TLS
destructor, live path) | LIVE -> COLLECTED -> DETACHED (~VM walk);
TEARDOWN and DETACHED are terminal; no other transitions; EVERY
transition AND every read is under VMLiteRegistry::lock. The state
byte — NEVER "is my lite registered" — is the sole owner-vs-walk
discriminator. Foreign carriers may still be REGISTERED at ~VM => M6
replaced: ~VM COLLECTS this VM's carriers under ONE registry-lock
hold: each non-TEARDOWN carrier is token-free-RELEASE_ASSERTed,
marked COLLECTED, and unregistered via unregisterLite (U20: EVERY
physical registry removal — this collection and m_mainVMLite
included — goes through unregisterLite, the notifying function);
TEARDOWN carriers are SKIPPED (owner mid-live-detach, still
registered — the EXIT1.9 step-(3) wait covers them). The lock is
released; the walk performs the FULL SERVER-SIDE detach of each
COLLECTED client while the server Heap is alive — everything in
~GCClient::Heap that names m_server: the access bracket,
lastChanceToFinalize's shared-directory allocator relinquishment
under MSPL, machineThreads removal, m_server.clientSet().remove()
(Heap.cpp:5078-5110 is the live-path dtor doing exactly these
against the server) — leaving each client dead-detached. The detach
runs LOCK-FREE of the registry lock NECESSARILY: it acquires MSPL
and can PARK in the access bracket, and LK.6 registry-lock holders
acquire NO lock and never wait (vmstate I7) —
whole-detach-under-the-lock is ILLEGAL. After EACH client's detach
the walk re-acquires the registry lock, flips COLLECTED->DETACHED,
notifyAll()s vmTeardownCondition, drops the lock (short hold;
acquires nothing), and NEVER touches that lite/client again. ALL of
this precedes the EXIT1.9 ~VM wait, so the wait never counts a
carrier. Remote detach (SUPERSESSION: heap I4 "lifecycle on the
using thread" + §10A.1, both sides; r32): client + lite destruction
is DEFERRED to the owner's TLS destructor for bit-CLEAR lites —
unconditionally; a bit-clear lite is NEVER walk-freed; for a
bit-SET lite (ownerHasNoTlsDtor, the r32 registration clause above)
the walk itself runs the degenerate free immediately after its
DETACHED flip — no competing dtor exists BY CONSTRUCTION, since no
destructor is ever installed over the main-thread slot. rev 31's
no-TLS-destructor-will-run liveness-probe clause is WITHDRAWN (the
bit replaces it). The deferred dtor takes the registry lock FIRST
and keys ONLY on the lock-published state: LIVE => mark TEARDOWN in
the same hold and take the live EXIT1.3 path; COLLECTED =>
predicate-wait on vmTeardownCondition until DETACHED
(Condition::wait drops the lock into the parking lot;
unregisterLite notifies tolerated — predicate loop), then the
degenerate path; DETACHED => the degenerate path immediately:
assert DETACHED, SKIP every m_server touch (all already done by
the walk), destroy only client-local memory (TLC tables,
m_perDirectory, the lite, the lite's default MicrotaskQueue).
Progress/acyclicity: the COLLECTED wait depends only on the
running, straight-line ~VM walk; no thread waits while OWNING the
registry lock; the lite is freed only by the path that observed
DETACHED (or by the live path), strictly after the walk's last
touch — the state byte is never read after free. Cross-client
detach concurrency (live dtor of carrier X vs the walk detaching Y)
is the exit-storm case — serialized by MSPL and
HeapClientSet::m_lock (heap §5.1/§6 ranks). The
deferred M12 queue removal after the VM is gone is a NO-OP by the
M11/M12 protocol: the M11 force-removal (VM.cpp:710-719) empties
VM::m_microtaskQueues under the process-lifetime registry lock
(VMLiteRegistry is NeverDestroyed) before any VM memory dies, so the
deferred ~MicrotaskQueue (MicrotaskQueue.cpp:128-141) finds
isOnList() false and touches only its own node (EXIT1.9
residual-tail rule). heap §10A.1's TLS slot becomes {client, epoch};
stale epoch => null (no UAF). The §6.5.1 assert is re-read through
EXIT1.9: wait-then-debug-assert ("registry empty for this VM"). VMs
carry a process-monotonic epoch; the TLS map stores {VM*, epoch,
carrier}; lock() compares epochs BEFORE the cached carrier. I20
holds (dead carriers are token-free, never CURRENT). EXIT1.3's
six-step order EXPLICITLY EXCLUDES this path (live-VM paths only;
cross-ref both sides). U27 + teardown storm + the
deferred-degenerate-dtor / delayed-TLS-destructor arm, the r31
CARRIER-TLS-DEATH-DURING-DETACH arm AND its r32 WALK-FREE variant
(= the EXIT1.8 CARRIER + r31/r32 arms).

**A36C — carrier-swap §10A.1 client-slot re-stamp (BINDING; r23
F1):**
1. The §A.3.6 swapped TLS state is the TUPLE {lite, TID-tag, heap
   §10A.1 currentThreadClient slot} — NOT {lite, tag}. EVERY carrier
   install (first entry, every lock(), §B.1 spawned attach) AND every
   LIFO restore (depth-0 unlock, §F.5 nested exit) re-stamps
   currentThreadClient() to the now-current lite's clientHeap,
   through A36's {client, epoch} staleness check (stale epoch =>
   stamp null, never a dangling client); restoring to "no lite"
   clears the slot. The stamp precedes any allocation/OM fast path
   AND the §F.1 gated AHA (preserving heap §10A.1's
   slot-correct-before-AHA ordering). Spawned threads: ACT's stamp at
   §B.1 attach is already correct and unique (single-VM v1); the rule
   is vacuous after attach.
2. SUPERSESSION EXTENSION (heap §10A.1 "once ISS,
   JSLock::didAcquireLock's forwarding re-stamps it before AHA"
   clause, SPEC-heap.md:283, vs §B.3 + this annex, both sides; IH
   row): GIL-off, the re-stamp duty is THIS annex's tuple swap;
   GIL-on/flag-off forwarding + re-stamp UNCHANGED. One IH row covers
   both clauses of SPEC-heap.md:281-283.
3. Verification. U1 EXTENDED (ID frozen): whenever a thread holds an
   entry token, TLS tag == CURRENT lite TID && lite->vm == entered VM
   && currentThreadClient() == lite->clientHeap (checked at the §J.7
   backstop + token acquisition/release in debug). U-T6 + U27 gain:
   (a) two-VM alternating-entry arm — embedder thread enters the
   m_gilOff VM A, exits, enters GIL-on VM B, exits, re-enters A, then
   allocates + DeferGC + triggers CIND (asserts route to A's client);
   (b) a §F.5 nested arm — A -> nested B -> LIFO-restore A ->
   allocate (slot re-stamped at restore, not left at B's client).
   Race-amplifier hook at the restore-side re-stamp.

**A.3.7 Atom-table routing (X1).** GIL-off, token acquisition points
the thread at the shared sharded table (U0); the per-handoff swap is
GIL-on-only. SUPERSESSION (vmstate §4.3 "None relaxed (ex-M5)" vs
this, both sides): each of the **14** atom-table asserts
(Identifier.cpp:77; Completion.cpp:63-287 x12; Heap.cpp:**2796** —
requestCollection, r16 F4: the rewrite is PREDICATE-PRESERVING — each
assert's LANDED predicate P becomes "gilOff ?
sharedAtomStringTableEnabled() : P"; :2796 KEEPS its
worldIsStoppedForAllClients() disjunct GIL-on, the landed T5b
late-ISS-flip guard). GIL-on/flag-off unchanged; IU row.

**A.3.8 GC-stop parking, N threads one VM** (closes heap Dev 8,
heap:27; SUPERSESSION vs heap §13.5 one-parked-thread-per-VM +
notifyVMStop's per-VM state machine, both sides; r8 item 8 full
text). The GC stop reason is THREAD-granular: the trap bit fans per
rule 3 (§A.2.3); EACH entered thread parks on its OWN ticket (§A.3.2
NVS); notifyVMStop asserts per-entered-thread (Mode keyed on
all-parked/released/not-entered; landed per-VM machine
double-transitions/asserts with 2 same-VM observers —
VMManager.cpp:404-590, duplicate-dispatch atomic :321,
RELEASE_ASSERT(m_targetVM==&vm) :218/:580); heap §13.5a/g
willPark/didResume run on currentThreadClient(), with per-client
m_releasedByGCPark pairing; 5b/5f/5g per thread (GC-bit keep-parked
:354-363; re-check-while-parked :510-557). Unlike §A.3, the GC stop
DOES set client-visible stop state (§10A/F8 gates re-acquisition; SB1
not needed — rides F8's pinned GSP ordering). IM: VMManager.cpp +
§13.5 hooks (IH). Amplifier: spawned-conductor shared GC, two same-VM
threads mid-JS (U29).

---

## B. Per-thread GCClient lifecycle in one VM

Charter: heap Dev 8 (ONE GCClient PER Thread); Dev 7 (full list:
B.6).

1. **Create at spawn.** threadMain (ThreadObject.cpp:162-176),
   GIL-off: after lite registration/setCurrent + TID-tag handshake,
   BEFORE any allocation, construct the thread's GCClient::Heap
   (ACT), store clientHeap in the lite (L2), acquire access
   (§A.3.2b-gated). JSLockHolder degrades to the §F token.
2. **Teardown at exit (EXIT1.3/EXIT1.9 as AMENDED by r31/r32).** In the
   T5 sequence after the Strong clears + unregisterThread: release
   access (seq_cst RHA) -> TEARDOWN mark (under the registry lock;
   logical removal — conductors count it EXITED, its access
   re-acquire FORBIDDEN) -> DCT/destroy the client ->
   unregisterLite LAST (physical removal; r31: EVERY physical
   registry removal — the A36 collection and m_mainVMLite included
   — goes through unregisterLite, the notifying function; U20);
   the lite free (with its M12 default-queue removal) runs after
   unregisterLite returns (EXIT1.9 residual-tail rule). ~VM BLOCKS
   in the EXIT1.9 NORMATIVE completion fence (registry Condition,
   signaled by unregisterLite and, r31, by the walk's DETACHED
   flips; the assert walk is a post-wait debug sanity check) until
   no registered lite other than m_mainVMLite has lite->vm == this
   — join-then-destroy-VM is safe in every build configuration
   (U3; U32). Exit stays UN-GATED (no stop-bit poll, no park
   point). The same mark-before-destroy + remove-last order binds
   the LIVE-VM teardown paths — spawned T5 and carrier TLS death;
   the ~VM foreign-carrier collection is EXPLICITLY EXCLUDED (A36
   as AMENDED r32 — the carrier-state handshake: lock-published
   lite state LIVE->COLLECTED->DETACHED; COLLECTED-mark +
   unregister BEFORE the EXIT1.9 wait; full server-side detach in
   the walk, lock-free; per-client DETACHED flip + notifyAll under
   a short re-hold; the owner TLS dtor takes the registry lock
   first and keys ONLY on the state — LIVE => live path, COLLECTED
   => wait for DETACHED, DETACHED => degenerate dtor restricted to
   non-VM memory; r32: the ownerHasNoTlsDtor bit, FIXED AT
   REGISTRATION (main thread => destructor-free map) => the walk
   itself frees post-flip, a bit-clear lite is NEVER walk-freed;
   a COLLECTED owner's re-entry is excluded for the duration of
   ~VM — re-entry requires fresh §F.1 registration under m_lock,
   which ~VM holds (VM.cpp:649); M11/M12 no-op queue removal).
   Lazy carriers own the VM's original client (main) or create one
   at first entry (embedder, §F.1). Full text: ANNEX EXIT1 as
   AMENDED by rev 31 + the rev-32 amendment record (§A.3 above) +
   the rev-32 A36 amendment.
3. **SUPERSESSION (heap §10A ISS forward-to-main-client wiring,
   SPEC-heap.md:281, vs §F.1, both sides; IH row; r13 F4):** GIL-off
   the JSLock pair acquires/releases on the CURRENT carrier's OWN
   client (§F.1) — NEVER the main client (heap Dev 8; following the
   heap text verbatim would have an embedder thread acquire access on
   the main client while the main thread uses it — two threads, one
   client, unsound per heap's one-client-per-thread model).
   GIL-on/flag-off forwarding + the §10A.1 re-stamp unchanged;
   GIL-off the re-stamp duty = the §A.3.6 swap (A36C EXTENDS this
   supersession to §10A.1's re-stamp clause, both sides, IH row);
   U-T6/U27 two-VM + nested arms.
4. **TLC-aware inline allocation:** fast paths address
   lite->clientHeap's TLC table, base = loadVMLite + frozen offsets;
   the §5.3 vm-relative chain stays GIL-on (heap Dev 6).
5. **Perf budget (heap Dev 7):** {useJSThreads=1, sharedGC=1,
   GIL-off, 1 thread} composite <=10% geomean vs the {1,0} flag-on
   baseline (BENCH.md); the {1,0} <=5% gate stays; a miss REQUIRES
   jit §4.3 LLInt-cache revival pre-ship. The 4-thread alloc
   microbench >=2.5x is recorded, not gated.
6. **heap Dev-7 GC-throughput items** (heap:26 list — per-directory
   handout + out-of-lock sweep, concurrent marking/incremental sweep)
   — SUPERSESSION (heap:26 + api:26 vs this, both sides; IH row; r6
   F8): DEFERRED to a post-ungil perf milestone; GIL-off ships on the
   synchronous conductor-driven heap §10 protocol + single-MSPL slow
   path (correctness-complete — the Dev-4 disables are perf modes
   only, heap:23). Gate = §B.5; a §B.5 miss pulls the deferred items
   forward pre-ship. INTEGRATE-heap records the override.

---

## C. api Dev 12 / OM 8g re-freeze

Charter: api:22; OM 8g; INTEGRATE-api D1/D2/D4/D8/D12. IS rev-15
content (IA sign-off).

### C.1 OM §9.5 atomic slot accessors (ANNEX C1, BINDING — full text)

atomicSlotCompareExchange / atomicSlotReadModifyWrite -> JSValue,
ONLY plain structure/butterfly-backed own NAMED data slots + the
indexed pair. NORMATIVE:
- **Lock-free arms** (inline, flat OOL, segmented-fragment slots —
  receivers NOT OM-locked): seq_cst 64-bit CAS/RMW loop on the
  EncodedJSValue slot word; NO cell lock on the segmented arm (a
  lock-held RMW would not serialize vs lock-free fragment stores,
  U5).
- **Flat-path transition discipline** (flat GROW = butterfly-CAS +
  copy, NO nuke — an old-butterfly CAS is silently lost).
  currentButterflyTID() != butterfly tag => FIRST the OM §2
  foreign-write SW-set DCAS, re-validate structureID + butterfly per
  I34, THEN CAS the slot. Validation failure restarts the WHOLE probe
  (I33-bounded); a completed RMW/CAS is NEVER re-applied.
- **Third arm: OM-locked regimes.** Dictionary (I19/L3) and AS-shape
  (§4.6; Thread.restrict FORCES AS): probe + CAS/RMW UNDER the
  JSCellLock OM already requires. **AS PRE-LOCK** (r8 item 6): the
  cell lock suffices only AFTER SW=1 (jit §5.5 owner AS fast paths
  are UNLOCKED while SW=0) — SW==0 && currentButterflyTID()!=tag =>
  FIRST the OM §4.6 first-foreign-write protocol (per-event STW,
  fire-then-publish (installerTID,1); I10b), then RESTART the probe;
  only SW=1 (or owner) enters the locked CAS/RMW. The lock is
  REQUIRED (dictionary delete is I34-blind — a lock-free CAS could
  "succeed" on an absent property, U5); dictionary-ness is re-checked
  under it. U5/U28 amplifier: owner unlocked AS store storm vs
  foreign CAS, same index, SW initially 0.
- **Indexed arm (8g re-freeze), by shape:** CoW — materialize per OM
  §4.8/I35 first. Int32/Double — raw-word CAS REJECTED: first atomic
  access CONVERTS to Contiguous (owner direct; foreign SW-set DCAS
  first). Contiguous — flat arm verbatim. ArrayStorage/dict-indexed —
  third arm. §C.2 routes parseIndex hits here; one arm per shape.
- Write barrier after success, as §9.5 orders.

### C.2 ThreadAtomics re-homing (UNGIL-PLAN P1)

The GIL-step atomicity block is replaced — bodies call the §9.5
accessors. CARRIED: D3 exotic-receiver TypeErrors; D7 writability
inside the atomic body.

### C.3 PWT arming re-home + I10 re-derivation (ANNEX C3 + r9 F1,
BINDING — full text)

The landed I10 closure is the JSLock; GIL-off the lost store+notify
window REOPENS. NORMATIVE, BOTH arms:
- (a) the PRE-ENQUEUE validation (api 5.6 step 1, api:229 —
  previously a PLAIN read) routes through the §9.5 atomic load —
  forcing any CoW/Int32/Double conversion OUTSIDE listLock.
  **Monotonicity lemma:** a §9.5-touched slot never returns to a
  converting arm — §C.1 converts CoW/Int32/Double on FIRST atomic
  access; Contiguous->AS/dictionary transitions land in the
  cell-locked third arm; no transition re-creates CoW/Int32/Double on
  a §9.5-touched object (OM I34/I35 forward-only shape order);
  AS/dictionary arms never allocate under the cell lock (OM I20) — so
  the under-listLock re-load is alloc/STW-free (api 3 -> 10a is the
  already-legal §LK cross edge).
- (b) enqueue under listLock; RE-VALIDATE SVZ(o[k], expected) via the
  §9.5 load STILL UNDER listLock; mismatch => dequeue, "not-equal";
  rope re-read OR convert-needed shape => DEQUEUE TOO (eats one FIFO
  notify — the I10 class; r7 F3: leaving the node enqueued would
  strand a genuine waiter's wakeup), unlock, resolve/convert via §9.5
  / the §N.2 single-flight protocol (may allocate — legal, no lock
  held), FRESH enqueue (NO alloc/STW under listLock, ever). After
  dequeue+restart the waiter is indistinguishable from a first-time
  arrival; the notifier-orders-through-listLock argument is
  unchanged: a missed store notifies AFTER our enqueue.
- waitAsync settles via §E.4 (finite timeout: §E.7.5); sync parks per
  §J.3. U5/U-T11. Corpus: wait/waitAsync on an Int32/Double/CoW index
  (first-ever atomic access) racing a notifier. GIL-on unchanged.

### C.4 4.5-1a TA gate lifted GIL-OFF ONLY

SUPERSESSION (api I21 :315 "deleted by re-freeze" + api:79 vs this,
both sides; IU row; r12 F5): the sole spawned gate,
AtomicsObject.cpp:613-621 (isJSThreadCurrent() => throwVMTypeError;
the only grep hit), becomes **vm.m_gilOff-conditional** — KEPT GIL-on
(SD4; the deletion is NARROWED to GIL-off by the master oracle rule);
no twin. ThreadAtomics.cpp:536-541 is NOT 4.5-1a: it is the G11
property-wait gate — KEPT, re-pointed at mayBlockSynchronously()
(§G.2). Post-lift blocking = §G-only (deadlock = user error, ruling
recorded r23).

### C.5 D2 notify-yield

GIL-off notify() is NOT a yield point — jsThreadGILHandoffYield is
GIL-on-only (§J.4); no foreign JS in notify(); parallel waiters
(SD5).

### C.6 D4/D8 lifted together (IA)

atomicsWaitImpl's sync path allocates a per-wait node instead of the
single vm.syncWaiter(); the D8 single-flight gate is deleted in BOTH
GIL modes — SD6. Nodes park per §A.2.6 (D9 quanta; flag-off keeps
the central wakes). (Per-wait nodes are strictly more correct under
the GIL too; a mode-conditional D8 gate would keep dead machinery
alive solely to preserve a wart — r2 finding 16.)

### C.7 D1 / D12

D1 ruling — §F.4. D12: grants settle via §E routing on the
registering thread; uniform (closed).

---

## D. OM Tasks 13 (TID rebias) + 14

### D.1 Task 13 (om:377, 8c) — IN SCOPE (ANNEXES D1 + D1R, BINDING —
full text)

Rebias runs world-stopped INSIDE the next FULL shared collection
under the heap §10 GC stop barrier — NOT a §A.3 stop (jit R1.h);
re-entry blocked per §A.3.8. Restamps dead TIDs' butterfly tags +
Structure::m_transitionThreadLocalTID to 0; TM reissues via
m_freeTIDs. Trigger: >=75% of 2^15 arms the next full collection;
spawn during exhaustion RangeErrors (api 5.1/I17, SD9) until rebias
completes; lifts Dev 10. Enumeration = world-stopped
HeapIterationScope (precise + aux) + StructureID-table walks.
Soundness of restamp-to-0: restamped objects become equivalent to
main-allocated (payload-0/TID-0 regime; OM decode tests payload
first); restamp is ordered BEFORE m_freeTIDs release within one stop,
which is exactly what the false-owner hazard requires.

**Two-phase vs §LK (r9 F2, full text — the conductor takes NO api
lock):** PRE-STOP, a mutator-side pass under TM::m_lock snapshots the
dead-TID set into a conductor-readable buffer; the conductor restamps
world-stopped FROM THE SNAPSHOT ONLY; m_freeTIDs release runs
POST-RESUME on a mutator under TM::m_lock, ordered before the >=75%
RangeError gate lifts. Soundness: spawn in the shared VM is blocked
by the RangeError window for the whole interval; concurrent
lazy-carrier creation (other VMs; TM is process-global; their threads
are NOT stopped) only ADDS live TIDs and cannot resurrect a
snapshotted-dead TID (a dead TID has no lite, no TLS map entry, and
TM never reissues before the post-resume release). The §LK
negative-edge row is annotated with this sole sanctioned interplay.

**D1R — rebias watchpoint fire (BINDING; AMENDS D1; r18 F3):**
1. In the same heap §10 stop, for EVERY structure whose
   m_transitionThreadLocalTID is restamped (held a dead TID), the
   conductor ALSO calls fireTransitionThreadLocal (which fires
   writeThreadLocal too, om:325; OM F4 chain-fire applies) BEFORE the
   stop resumes — hence strictly before the post-resume m_freeTIDs
   release that makes reissue possible. This jettisons every
   DFG/FTL/IC body specialized on such a structure (E4 emission
   requires the TTL set valid+watched; fire => jit §5.3/§5.6
   jettison), so no baked tid<<48 immediate survives to the reissue
   point; OM I11/I15 hold by construction.
2. SUPERSESSION (jit I13 + OM §9.4/I13 "fired only in VMM STW" vs
   this, REBIAS-STOP FIRES ONLY, both sides; IJ/IO rows): the heap
   §10 stop barrier provides equivalent quiescence (every mutator
   parked/not-entered/access-released, WSAC set); jit §5.6's
   worldIsStopped() ALREADY includes the
   worldIsStoppedForAllClients() disjunct and routes such fires to
   branch 1 (run inline) — mechanics need no change; the resume-side
   sync is ANNEX ISB1 (item 2's widened delivery set); conservative
   scan R2 + I7 gate the jettisoned-code frees as for any GC-stop
   jettison.
3. Cost bound: the fired set = structures holding dead TIDs — the
   same set D1 already enumerates for restamping; chain-fire bounds
   per OM F4 (the jit Task-13 stop-budget gate covers it; rebias is a
   rare, exhaustion-driven event under SD9's spawn gate).
4. Instance tags need no fire: jit read/write predicates load the
   instance tag at runtime and compare against the R5 per-thread TLS
   tag — neither side is baked as an immediate; restamp-to-0 + tag
   uniqueness suffice (r9 F2). Only the structure-specialized
   transition immediate is baked, and item 1 kills it.
5. Amplifier (U-T12, new arm): compile E4-specialized transition code
   against a dying thread's structure (butterfly-less path), exit the
   thread, force rebias, force TID reissue to a fresh thread,
   transition storm from the reissued thread vs a foreign locked
   transitioner; assert I15 (instrumented) and that the specialized
   CodeBlock was jettisoned during the rebias stop.

Amplifiers: U-T12's two-VM TM-churn arm (rebias in VM A while an
embedder lazily enters VM B); spawn-storm past 2^15 (U18).

### D.2 Task 14 (om:378) STAYS DEFERRED pending the bench verdict

Timing SUPERSESSION (both sides; INTEGRATE-om §46 holds NO verdict):
the gate re-times to a HARD precondition of **U-T10 ENTRY** (a
docs-only round cannot run the construction bench; UNGIL-PLAN.md:250
binds this spec to record, not redesign — the supersession only moves
the gate). PROMOTE => Task 14 lands before U-T10 and §C's third arm
is re-reviewed pre-code; else 8h ships as landed (OM 8h/L6/I37).
---

## E. Per-thread event loop + settlement (THREAD.md:98)

Ground truth replaced (api 4.6.1 GPO drain; DWT settlement). Landed
inert: inboxLock/inbox/inboxOpen, the per-lite microtask slot
(vmstate §6.6), I11. SUPERSESSION (both sides; IA row): api 4.6.1
never-waits + 4.6.2 SHELL-granular keepalive are SUPERSEDED GIL-off
by §E.2/E.3 (queues-empty + keepalive==0, thread-granular); GIL-on
keeps the old text (SD1).

(Why main keeps DeferredWorkTimer: spawned threads get bespoke queues
rather than per-thread DWT instances because DWT is RunLoop-coupled
and the embedder owns the only real WTF::RunLoop — Bun's event loop,
USE_BUN_EVENT_LOOP. A spawned thread's "runloop" is the E.2 drain
loop — a condition-variable pump; tickets keep their DWT registration
solely for shell-liveness (I20/4.6.3) and the VM-shutdown
cancelPendingWork backstop, both process-global concerns.)

### E.1 Queues

Every ThreadState owns, GIL-off:
- **Microtask queue:** the per-lite MicrotaskQueue (vmstate §6.6),
  enqueued/drained ONLY by its owner (I11); VM::queueMicrotask/
  drainMicrotasks re-route to the CURRENT lite's queue.
- **inboxOpen** (landed default false): set true EXACTLY ONCE on the
  owning spawned thread, under inboxLock, post-§B.1 attach, BEFORE fn
  (happens-before any registration vs this TS; r22 list).
  Main/embedder NEVER open theirs; increment sites assert
  spawned+OPEN (U25).
- **Host hook (X1.7):** queueMicrotaskToEventLoop
  (JSGlobalObject.h:1238) is consulted ONLY for carrier enqueues;
  spawned enqueues are ALWAYS per-lite — else I11/U22 break (r22
  list). Corpus test with an installed hook.
- **Task (macrotask) queue:** TS fields under the EXISTING inboxLock
  (api rank 3): Deque<ThreadTask> taskQueue, uint64_t keepaliveCount,
  Condition runLoopCondition, waitDeadlines (§C.3/§E.7.5: a
  deadline-ordered list of {deadline, PWT waiter}, guarded by the
  SAME inboxLock, appended at §C.3 waitAsync registration when the
  registrant TS is spawned and the timeout is finite — r12 F3);
  ThreadTask = settle task + Ref<AsyncTicket>; the landed inbox
  vector IS the task queue.

### E.1b Ordinary shared-promise settlement (NEW)

E.4 routes only AsyncTickets; under the shared heap ANY thread can
resolve an ordinary JSPromise whose .then() registered elsewhere.
NORMATIVE v1:

1. **Reaction jobs run on the SETTLING thread:** the resolver
   enqueues to ITS OWN per-lite queue via the rerouted
   VM::queueMicrotask — I11; no per-reaction registrant hop v1.
   SD10. (Per-reaction registrant tracking REJECTED: a new
   heap-visible per-reaction structure with its own lifetime/teardown
   races, and the registrant may be dead at resolve time; the
   settling-thread rule needs no new state and is I11-clean.)
2. **Concurrent then()/resolve() (ANNEX E1B, BINDING — full text):**
   GIL-off, JSPromise internal-state transitions run under the
   promise's JSCellLock (10a) — internal fields are NOT §9.5 slots.
   Bodies RESTRUCTURED per OM I20 (no GC alloc under 10a): allocate
   reactions (+ the Bun InternalFieldTuple context,
   JSPromise.cpp:346-359) OUTSIDE; re-check status under the lock
   (settled => drop the allocation, queueMicrotask post-unlock;
   Pending => re-read reactionHead, fix next, publish via
   setPackedCell); resolve/reject swap status + extract the chain
   under it, enqueue post-unlock; performPromiseThen's pre-switch
   status() read is ADVISORY; one uncontended cell-lock per op.
   GIL-on unchanged. **U-T9 audit:** every other promise
   internal-field writer/tier-inlined access locks or is disabled
   GIL-off (PromiseOperations.js/PromiseConstructor.js contain no
   @putPromiseInternalField sites — the native-restructure path is
   coherent; only the non-promise types need §N.5's primitive);
   non-promise = §N.
3. U22: reactions on the settling thread; AsyncTicket settlements on
   the REGISTERING thread (ThreadTask hops, §E.4).
4. **promiseRejectionTracker (JSPromise.cpp:405-637; r16 F3, BINDING
   — full text):** GIL-off the tracker is invoked INLINE only when
   the acting thread is a main/embedder carrier. Spawned-thread
   Reject/Handle events are appended (no JS, no allocation beyond the
   record) to the annex-E7 m_pendingLock-guarded handoff queue as
   tracker records {promise Strong, operation}, flushed and EXECUTED
   at the §F.1 carrier drain points like off-carrier DWT work;
   ordering vs carrier-side tracker events is unspecified (SD15; the
   unhandled-rejection report may arrive a drain late, never lost
   while the carrier still drains; process-exit-before-drain drops
   are the same class as landed exit-before-microtask drains). Strong
   create/clear inside the record follows §F.3 (enqueuer holds a
   token; carrier clears under its token). No hooks installed => same
   routing (the queue is DWT-owned, not hook-owned); a VM with no
   carrier ever draining leaks reports — declared.
   **AUDIT U-T8e (runs with U-T8b/c, gates U-T9):** enumerate EVERY
   globalObjectMethodTable / host-callback slot reachable from JS on
   a spawned TS (reportUncaughtExceptionAtEventLoop, moduleLoader*/
   importModule, shadowRealm hooks, codeForEval/canCompileStrings,
   deriveShadowRealmGlobalObject, currentScriptExecutionOwner, etc.)
   and give each an IU-table disposition: {inline-safe,
   carrier-queued (this mechanism), refused-with-error,
   unreachable-on-spawned (proof)}. Corpus (U-T9): spawned resolver
   rejects a shared promise with a Bun-style tracker installed =>
   report arrives on the carrier; handle-after-reject arm.
5. **AsyncLocalStorage (Bun; ANNEX ALS1, BINDING — full text).**
   Tree facts: capture is PER-REACTION, at registration time — each
   site reads the CURRENT cursor
   globalObject->m_asyncContextData.get()->getInternalField(0)
   (cursor slot: JSGlobalObject.h:507) and stashes it into the
   reaction's [userContext, asyncContext] InternalFieldTuple or the
   microtask's argument (JSPromise::performPromiseThen
   JSPromise.cpp:341-357; performPromiseThenWithContext :433-449;
   await resume resolveWithInternalMicrotaskForAsyncAwait :989-1001;
   thenable jobs :692-695/:724-727; then() prototype fast path
   JSPromisePrototype.cpp:296-303). Restore is at job-run time on
   WHATEVER thread drains: the runner saves the cursor, writes the
   captured value, runs the reaction, writes the saved value back
   (JSMicrotask.cpp:1531-1556/:1578-1598/:1611-1631). No
   per-thread-VM-state capture exists on these paths.
   RULING: (1) SD10 thread-migrating continuations PRESERVE ALS —
   the captured tuple is an ordinary shared-heap cell carried BY THE
   JOB; the carry already exists structurally. (2) ALS1.2 visibility:
   the capture site publishes the tuple via the normal §E.1b enqueue
   (I11 own-queue or §E.4 ThreadTask append under inboxLock); both
   edges carry release/acquire, so the settling thread reads an
   initialized tuple. (3) ALS1.3 — the CURSOR m_asyncContextData is
   shared mutable state, swap-WRITTEN by every job run and by Bun's
   enter/exit hooks; two threads draining same-realm reactions would
   clobber each other's bracket. GIL-off the cursor reroutes
   PER-LITE (§K.1 class duplicate: accessor keys on the CURRENT
   lite; cell-holding copy GC-scanned via the registry walk).
   "Current async context" is thread-local by definition — per-lite
   is the semantically correct shape. GIL-on/flag-off unchanged. (4)
   Corpus (U-T9; rides SD10, no new SD): spawned B resolves a shared
   promise; A registered .then()/await inside ALS store S; GIL-off
   the continuation runs ON B (SD10) and MUST observe S (the
   registration-time store), not B's current store; after the
   reaction, B's own cursor value is restored exactly. GIL-on
   variant keeps phase-1 expectations. Embedder note: this
   discharges §F.6(b)'s continuation-affinity question for ALS
   specifically — Bun need NOT demand a carrier hop for ALS;
   §F.6(b)'s U-T9 entry-gate sign-off remains for non-ALS concerns.

### E.2 Thread lifecycle — normative drain loop (ANNEX E2A, BINDING —
pseudocode VERBATIM)

threadMain GIL-off, after fn returns/throws:

```
loop:
 drainMicrotasks(own); releaseClientHeapAccess()
 under inboxLock:
   termination trap pending => goto close (§E.5)
   task = taskQueue.takeFirst() if any
   else if keepaliveCount == 0: goto close
   else wait runLoopCondition, min(10ms, earliest waitDeadline)
     quanta, D9 pred (§A.2.4)
 post-wake §A.3.2b poll; reacquireClientHeapAccess()
 EXPIRE deadlines (r12; landed 5.6 timeout, inline): while
   {under inboxLock: earliest waitDeadline <= now? take : break}:
   listLock dequeue, DROP listLock, §E.4 settle "timed-out"
   (rule-1 decrement); rank-3 locks NEVER together (§LK)
 run task if any (arbitrary JS, under §F token); loop
close:
 under inboxLock (access-released):
   inboxOpen = false (keepalive DEAD, E.3 r3)
   residue = std::exchange(taskQueue, {})
   deadlines = std::exchange(waitDeadlines, {}) (r16 F5)
 drop inboxLock; §A.3.2b poll; reacquireClientHeapAccess()
 for each deadline: listLock dequeue (already-dequeued => skip),
   drop listLock, §E.4 settle "timed-out" (closed => main
   fallback; SD8 ext: finite waitAsync never hangs; r16 F5)
 retire residue DWT work + route residue to main (E.4 dead rule);
 F1/F5 as landed; access release at the landed T5 point, then the
 EXIT1.3 teardown order (§B.2, as AMENDED by r31): TEARDOWN mark
 (registry lock) -> DCT -> destroy client -> unregisterLite ->
 free lite (free outside the registry lock, EXIT1.9 residual
 rule; U3 as AMENDED, U32)
```

**Lock/access rule.** Heap-access transitions are NOT leaf: NO
transition while holding any api rank 1-3 lock — release BEFORE,
re-acquire AFTER (ditto §J.3 park sites). **RANK-4 EXEMPTION (api
5.9(e), api:271; r8 fix 1):** NLS::m_lock/ParkingLot internals MAY
span token+access (re)acquisition — block/quanta-loop on m_lock ONLY
while both are RELEASED, then (re)acquire gated (§A.3.2b/§A.3.8)
while holding m_lock (§LK long-hold; landed contended hold/cond.wait
LockObject.cpp:334-380). The U20-compliant alternative (hold access,
block on m_lock) deadlocks GC: a ParkingLot-blocked waiter holding
access never polls GSP/stop bits (heap §9 RHA/AHA contract). Acyclic
because every m_lock waiter is access-released and no GC/§A.3
conductor acquires NLS::m_lock. U20 lints the order.

Thread completes — and join/asyncJoin settle (F5) — ONLY at close
(U7), not fn-return (SD1). Park sites inside fn do NOT service the
task queue. Wakeups: task append, stop, termination, quantum.

### E.3 Keepalive accounting (ANNEX E3, BINDING — full text)

keepaliveCount counts outstanding registrations that may still
enqueue a task here; transitions under the registrant's inboxLock;
exactly-once via the per-ticket **m_keepaliveReleased** flag,
CONSTRUCTED **true** (=released; r6 F1 — the safe default mirrors the
landed m_settled CAS, ThreadManager.cpp:78-81). The INCREMENT site
ALONE stores false (=armed) BEFORE the ticket is visible; decrement
sites act ONLY on winning the false->true CAS — never-armed tickets
(asyncJoin, TA waitAsync, main/embedder, any future non-counted
registration) lose the CAS and never decrement (else uint64 wrap =>
the §E.2 exit predicate "keepalive == 0" never fires). U8
mutual-asyncJoin-with-OPEN-inboxes arm.

INCREMENT (+1), once, at registration (I20 addPendingWork), on the
REGISTERING TS: every spawned-TS AsyncTicket EXCEPT asyncJoin —
asyncHold, cond.asyncWait, property Atomics.waitAsync (§C.3).
Main/embedder registrations never touch keepalive (§E.7).
- asyncJoin: NO keepalive — settles only at the JOINEE's close
  (F5/§E.2; counting deadlocks mutual/self asyncJoin); closed
  registrant => E.4 main fallback (I12). SD12; mutual/self arms.
- TA Atomics.waitAsync: NO keepalive — not an AsyncTicket; WLM
  settles via DWT scheduleWorkSoon MAIN-side. SD11; re-home REJECTED
  v1 (§E.7.5 covers PROPERTY waitAsync only).

DECREMENT (-1), exactly once — every site first wins the
m_keepaliveReleased CAS; losers do nothing:
1. Settle-enqueue (E.4): decrement in the SAME inboxLock section as
   the append, iff inboxOpen (closed: CAS won, decrement SKIPPED,
   main fallback).
2. Cancel (VM-shutdown cancelPendingWork, api 5.5; D5 bailout): iff
   CAS won AND inbox open, under inboxLock.
3. Inbox-close: NO claim step — inboxOpen=false => the counter is
   DEAD; a later settle/cancel wins its CAS, the open check skips =>
   main fallback. Exactly-once (U8) from 1-2.

U9: decrement + append atomic under inboxLock; E.2's exit check reads
both under the same lock (no exit can interleave between a decrement
and its append); the decrementer signals runLoopCondition before
unlocking. Intentional leak: never-notified waitAsync/asyncHold keeps
keepalive>0 => join hangs (api 4.6.2 class); §E.5 escapes.
(Rationale for decrement-at-enqueue over decrement-at-run: the
single-inboxLock critical section gives a one-lock proof of U9; a
cancel path never enqueues and would need a separate discipline
otherwise.)

### E.4 Cross-thread ticket settlement routing

Implements api:200's open arm; the closed arm is **SUPERSEDED** (r17
F6; api 5.5 :200 "else append to MAIN TS inbox" arm vs this + §E.1
main-inbox-never-opens, both sides; IA row — the main inbox is
structurally dead GIL-off; scheduleWorkSoon is the landed mechanism
and composes with §E.7.3-4; the api:200 GIL-phase paragraph governs
GIL-on unchanged).

AsyncTicket::settle GIL-off: CAS m_settled (as landed); cancelled =>
bail; under m_registrant->inboxLock READ inboxOpen; **open** =>
append ThreadTask, rule-1 decrement (armed only), notifyOne; DROP
inboxLock; **closed** => FALLBACK to MAIN via the LANDED
scheduleWorkSoon path AFTER the drop, NO api lock held (r18 F2 —
decide-under-lock / act-after-drop; sound because inbox closure is
MONOTONIC: inboxOpen true exactly once pre-fn, false forever at
close, so a post-drop fallback can never race a reopen; the open-arm
append/decrement stays atomic under inboxLock, U9 unchanged; the
§E.7.3 wake — hook or vm.runLoop().dispatch — fires with NEITHER
m_pendingLock NOR any TS::inboxLock nor any other api rank-1..3 lock
held; U20's lint extends to wake-under-rank-3; §E.7.3-4 apply).

**PRECONDITION (r17 F2, BINDING, incl. the api 5.5a/F5 SUPERSESSION,
GIL-off, both sides; IA row):** settle is invoked holding NO api
rank-1..3 lock. The frozen call sites held rank-3 locks (api 5.5a A
"u/QL set m_asyncHeld/m_asyncHolder, settle" and P "dequeue head,
settle", SPEC-api.md:206-209 under NLS::m_queueLock; F5 asyncJoin
"u/joinLock — !=Running => schedule settle", SPEC-api.md:140) — A/P
now record the granted ticket under QL, DROP QL, then settle (the F5
Compl "drop joinLock; settle moved tkts" shape — no lost grant: the
ticket is already owner, R/P observe m_asyncHeld); asyncJoin drops
joinLock pre-settle (no lost wakeup: Phase is re-checked under
joinLock before the drop decides settle-vs-append, and completion
settles appended tickets). GIL-on text stands (settle = DWT
scheduleWorkSoon, no rank-3 lock). U-T8 IU settle-site lock-context
table; U20 lints rank-3 settles.

**DWT retirement on the task-queue path** (ThreadManager.cpp:88-95):
the ThreadTask body, on the owner under its token: (a) settle, (b)
cancelPendingWork (fires the §E.7.4 wake), (c) clear m_promise.
Thread keepalive supersedes DWT shell-liveness for spawned
registrants; dead=>main keeps the landed retirement. U24.

I11/I12 are satisfied post-GIL. join() parks unchanged;
GILDroppedSection out (§J.3); §G gates the block.

### E.5 Termination

A termination trap observed by the E.2 loop (or during fn) takes the
landed Failed path VIA THE §E.2 CLOSE BLOCK — incl. its deadline
harvest (SD8; r16 F5: close harvests waitDeadlines under inboxLock
together with taskQueue; after dropping inboxLock with access
re-acquired, each harvested entry: dequeue the waiter under its
listLock — already-notified/dequeued => skip, the in-flight settle
wins — DROP listLock, settle "timed-out" via §E.4, which takes the
MAIN fallback since the inbox is closed; keepalive is DEAD post-close
so the rule-1 decrement skip is the existing exactly-once story;
early "timed-out" before the wall-clock deadline at owner
close/termination is the declared SD8 EXTENSION; re-registering on
main's 5.6 timer was rejected — it keeps a dead thread's deadline
machinery alive cross-thread for no observable benefit): close inbox
(E.3 rule 3), residue to main, F1/F5 with Phase::Failed.

A terminated thread completes with keepalive>0; its tickets settle
later via main fallback (4.6.2). **Per-lite microtask residue is
DROPPED at close (I11; never drained) — SD17** (r24 F3: the
settlement is already published cross-thread; B's reaction jobs in
A's queue vanish; adoption forbidden by I11; a termination-tolerant
drain would run JS on a terminated thread — rejected); published
settlements stay visible. Termination = the §A.2.4 VM-wide trap
(TERM1): EVERY entered thread takes its OWN close; the VM SURVIVES
(the carrier services it, annex W). Failed publishes a FRESH ordinary
Error("Thread terminated") — NEVER the sticky m_terminationException:
join() rethrows it NORMALLY (the joiner is not re-terminated),
asyncJoin rejects with it — SD8 ext2 (TERM1.3); main fallback =
scheduleWorkSoon, runs at carrier re-entry. U-T11 arms: terminate a
spawned thread holding a pending finite-timeout property waitAsync —
the promise settles "timed-out"; a variant where a notify races the
close harvest — exactly one of ok/timed-out, never both, never hang;
the terminated-join rethrow arm (SD8 ext2); the SD17 arm (settler
terminated between publish and drain; GILOn: reaction runs; GILOff:
settled state visible, reaction dropped).

### E.7 DeferredWorkTimer under N threads (NEW)

m_pendingTickets is JSLock-serialized today. NORMATIVE GIL-off:

1. m_pendingTickets (+ other JSLock-serialized DWT state) gains Lock
   **m_pendingLock**, rank LEAF (§LK; never across user JS) —
   add/cancel/hasPendingWork + peers, shutdown walk; cross-thread
   cancel (E.4) safe. **Name equation (K4 VII.4 / AUD1.K5):**
   §E.7.1's m_pendingLock IS the in-tree DWT::m_taskLock
   (DeferredWorkTimer.h:116), EXTENDED to m_pendingTickets (:121),
   whose three-condition comment (:125-126) loses the GIL leg. One
   §LK.7 leaf lock; no second lock.
2. DWT's API-lock asserts keep the §F.2 token meaning — incl. the
   NEGATIVE assert at runRunLoop.
3. **Embedder-hook ruling (USE_BUN_EVENT_LOOP; r8 annex + ANNEX E7 +
   r17 F3 + r18 F2, all BINDING — full mechanics):** installed hooks
   onAddPendingWork/onScheduleWorkSoon/onCancelPendingWork
   (DeferredWorkTimer.h:110-112) BYPASS m_pendingTickets and run
   INLINE on the caller (landed dispatch unconditional,
   DeferredWorkTimer.cpp:204/:234/:266-269). **hookManaged** is set
   at addPendingWork iff hooks are installed AND the registrant is
   main/embedder; EVERY dispatch site (scheduleWorkSoon,
   cancelPendingWork) checks hookManaged BEFORE the installed-hook
   branch — internal tickets take the internal arm on ANY calling
   thread, incl. on-carrier: hooks never see a ticket that skipped
   onAddPendingWork. Off-carrier settle/cancel with hooks: the
   m_pendingLock-guarded handoff queue, flushed + EXECUTED at §F.1
   drain points on the carrier under its token (incl. E.4(b) retire +
   m_promise clear) — the embedder does NOT pump DWT's timer, so
   internal-arm scheduleWorkSoon entries are NOT timer-scheduled; the
   wake is the FOURTH hook onCrossThreadWorkEnqueued (REQUIRED with
   the other three, boot-checked; never runs JS), driving them to
   completion; fallback vm.runLoop().dispatch of the flush (else a
   parked-main settle deadlocks; U-T9 hook arm). **Wake-edge lock
   contract (r17 F3):** the handoff-queue append, removal, and
   emptiness reads happen under m_pendingLock; the wake (hook call or
   vm.runLoop().dispatch()) fires strictly AFTER dropping it (and per
   r18 F2 with no api rank-1..3 lock held); append happens-before the
   post-drop wake; the carrier drain re-checks queue-nonempty under
   m_pendingLock after each wake; a wake-side race (drain between
   drop and wake) is benign (spurious wake). Boot-check contract:
   onCrossThreadWorkEnqueued is invoked with NO JSC lock held and
   must not reenter JSC. U24 Bun arms: dead-registrant settle with
   hooks; a hook-that-takes-the-loop-lock variant; closed-registrant
   settle FROM A SPAWNED THREAD with hooks installed — no deadlock,
   task reaches main.
4. **No-hooks runloop wake:** an off-carrier E.4(b) retire would
   strand a parked shell (RunLoop::stop fires only in DWT's timer
   callback); internal-arm cancel/retire while
   m_shouldStopRunLoopWhenAllTicketsFinish dispatches an ON-loop
   re-check via vm.runLoop().dispatch() AFTER dropping m_pendingLock
   (r17 F3); emptiness reads under m_pendingLock. U24 shell arm.
5. **vm.runLoop()-bound paths route BY REGISTRANT (r10 F3, hooks or
   not — the api 5.5a schedPump pump task P, G28, + the 5.6 waitAsync
   finite-timeout timer; rev-9's hooks-only ruling deadlocked the
   hooks-OFF shell: main parked in join, nothing pumps the main
   runloop, grant waits P, P waits the runloop — a NEW GIL-off hang
   of a satisfiable program):**
   - HEAD registrant/waiter SPAWNED: P runs INLINE on the
     releasing/notifying thread (P is lock-free: clear m_pumpPending,
     tryLock, settle via E.4 — it never runs JS; G28's "GI" RL-turn
     rationale is void GIL-off because settle-enqueue is not a JS
     execution point); the 5.6 timer becomes a DEADLINE on the
     registrant TS (waitDeadlines, §E.1; E.2's wait sleeps
     min(quantum, earliest deadline) and expires it locally; the loop
     stays alive — §C.3 waitAsync holds keepalive; expiry = §E.2
     EXPIRE or the close harvest). Spawned-registrant work NEVER
     routes via carrier drain points or vm.runLoop().
   - MAIN/EMBEDDER registrant: hooks => rule 3; no hooks =>
     vm.runLoop() as landed (a parked main's own registrations are
     §G-gated user choices — the api 4.6.2 class).
   - **SD16 (r18 F4, BINDING, incl. the api 4.5/5.6 SUPERSESSION,
     GIL-OFF ONLY, both sides; IA row):** a finite-timeout PROPERTY
     Atomics.waitAsync registered on a spawned TS settles "timed-out"
     only when the registrant next reaches its §E.2 drain loop
     (EXPIRE) or closes/terminates (the r16 F5 harvest, SD8 ext); a
     registrant parked forever inside fn (or spinning in JS that
     never drains) never settles it — GIL-on keeps the landed timer +
     timing. Notify-driven settlement is UNAFFECTED (PWT notify
     settles via §E.4 from the notifier); only the TIMEOUT edge is
     registrant-bound. Liveness alternatives REJECTED v1 (recorded):
     (a) a main-side DWT fallback timer (SD11 shape) — dual-settler
     complexity for a corner the embedder can avoid; (b) a §J.3
     quantum deadline check — quanta may poll ONLY lock-free state
     (U2's bound) and waitDeadlines is inboxLock-guarded. Either may
     be revived post-ungil (earlier settlement is always legal).
     Corpus (U-T11): register finite waitAsync, park forever in
     cond.wait — GIL-off: promise unsettled (bounded observation
     window), joiner sees the api 4.6.2-class hang; GIL-on (U19
     variant): settles "timed-out"; api I22 keeps passing both modes.
   Corpus (U-T9/U-T11): the r10 hooks-OFF join/asyncHold cycle;
   spawned waitAsync finite timeout, hooks on AND off (exercises
   waitDeadlines + the E.2 EXPIRE step).

---

## F. Post-GIL API-lock contract

### F.1 JSLock GIL-off mode

JSLock::lock() branches on mode+caller:

- **Spawned Thread: NO m_lock.** Installs an entry token {depth,
  spAtEntry} in the VMLite — records sp/lastStackTop (§A.1.4), ORs
  the VM trap + service words in, acquires client heap access (§B.1,
  §A.3.2b-gated), bumps depth; unlock() symmetric; depth 0 releases
  access. JSLockHolder = token.
- **Main/embedder: REAL lock (ANNEX F1B, BINDING — full text).**
  m_lock still mutually excludes embedder threads (Bun exclusion
  kept). Acquiring it ALSO takes an entry token (§A.3.1 set uniform)
  + the §A.3.6 carrier+tag+client swap; GIL-on extras skipped per
  §§A.3.7/B.3: FIRST entry creates the carrier lite + its
  GCClient::Heap (main reuses the original client; embedder creates
  one, §B.2), runs ACT (heap I4(b)); EVERY lock() runs
  §A.3.2b/§A.3.8-gated acquireHeapAccess on THAT client (idempotent
  at depth>0, F8 step 0); unlock() at depth 0 releases.
  Spawned-conductor GC scans a lock/eval/unlock embedder's stack
  (U27/U-T6 negative arm). Drain-on-release KEPT GIL-off:
  willReleaseLock drains the CURRENT lite's queue (I11; other
  drains: embedder runloop/DWT §E.4, explicit drainMicrotasks, the
  §E.7.3 flush). Park sites release m_lock per §J.3.

U1 asserts; JSLock.cpp:151 backstop = §J.7; ~VM: M6 replaced per
annex A36. I20 holds. U27 + teardown storm.

### F.2 Two predicates, split

- VM::currentThreadIsHoldingAPILock() is REDEFINED GIL-off as
  "current thread holds an entry token for this VM" — the host-call
  assert meaning (DWT §E.7.2).
- JSLock::currentThreadIsHoldingLock() stays MUTEX-LITERAL — §F.4's
  DAL handling + m_lockDropDepth LIFO depend on it. Spawned unlock()
  takes the token branch BEFORE the mutex RELEASE_ASSERT.
- **Consumer audit (U-T8):** the ~60 consumers of either predicate
  get an IU table — classified {assert (token meaning), BRANCH,
  EXCLUSIVITY CONSUMER (needs a §K serializer)}. **Fixed rulings
  (ANNEX F2, BINDING — full text, six named sites):**
  - sanitizeStackForVM — uses the CURRENT lite's lastStackTop.
  - primitiveGigacageDisabled — MUTEX predicate + §A.1.5 deferred arm
    (the gigacage-disable service is VM-wide).
  - JSCell::validateIsNotSweeping — token + per-CLIENT mutator state.
  - ISS-flip clause-(a) — DISCHARGED by U0c's eager ctor-time flip.
  - DeferredWorkTimer asserts — §E.7.2 token meaning (incl. the
    NEGATIVE assert at runRunLoop).
  - WeakSet::allocate (WeakSetInlines.h:44) — token+access predicate,
    NOT exclusivity (REFUTED r11 F2: the free-list pop is
    MSPL-locked under ISS, WeakSetInlines.h:69, the landed SharedGC
    round-4 lock; deallocate is deliberately lock-free with a
    recorded soundness argument, WeakSet.h:121-131 — reachable from
    in-lock-sweep destructors where MSPL would self-deadlock; sound
    because conducted sweeps are world-stopped and mutator-concurrent
    MSPL sweeps skip weak-bearing blocks; the GIL is NOT the sole
    serializer there).
  - (r13 adds) Watchdog's asserts (:44/:57/:132/:160) — token meaning
    for the DATA-RACE question; the SEMANTIC ruling is §A.2.8
    carrier-only / W4 (the audit row points there, not §K).
  - (r10 adds) the ~AsyncTicket assert (ThreadManager.cpp:57
    currentThreadIsHoldingAPILock) gains a sweep-context arm GIL-off
    (the sweeper holds a token; satisfied by the redefinition —
    classed "assert (token)").

### F.3 Strong-handle discipline (api 5.10)

ONE shared HandleSet per VM, new leaf **HandleSet::m_strongLock**
inside Strong allocate/free/set-slot only (never across user code).
Mutation needs an entered thread WITH heap access (E.2's close
re-acquires first); GC scans the set under the heap §10 stop (NOT
§A.3). (Per-thread HandleSets rejected: Strong lifetime is not
thread-affine — the 5.10 finalizer hook, ThreadObject.cpp:96-131, and
~AsyncTicket, ThreadManager.cpp:48-59, exist precisely because
last-refs drop on foreign threads; a leaf lock is two uncontended
atomic ops on a non-hot path; revisit only on bench evidence.)

Carve-outs (r10 F1): (a) in-lock-sweep Strong FREES under
MSPL/BVL/9b are legal — m_strongLock joins the destructor-leaf class
(§LK.8; verified chain: JSLockObject::destroy -> ~NativeLockState ->
Deque<Ref<AsyncTicket>> m_asyncWaiters -> ~AsyncTicket destroys a
STILL-SET Strong<JSPromise> for never-settled tickets; Strong free =
list-splice + fastMalloc, acquires nothing, never waits — the §LK.8
proof shape; the epoch-retire alternative was REJECTED: heap §9
forbids retire() under ranks 7-9b too); the ~AsyncTicket assert
GIL-off = token meaning. (b) heap finalizers clearing Strongs (api
5.10/D5-companion addFinalizer lambdas — they need m_strongLock +
access) run entered-with-access OUTSIDE the stop window (heap §10B(5)
JS-finalizer ban respected; the conductor runs them after resume,
before releasing its own client's access). LANDED (CVE-AUDIT B7 /
MC-GC S5): Heap::LambdaFinalizerOwner::finalize defers the lambda
body when WSAC is set (gilOff-gated; flag-off byte-identical) into
Heap::m_deferredLambdaFinalizers, drained by
Heap::drainDeferredLambdaFinalizers at the conductSharedCollection
tail (after closeSharedGCStopWindow + acquireHeapAccess). The
WeakImpl is freed in-stop (lock-free); the deferred lambda receives
a null JSCell* (its block may already be reclaimed) — the 5.10
lambda captures by Ref and ignores the argument; any future
LambdaFinalizer needing the cell must capture by value. WSAC is
set/cleared only by open/closeSharedGCStopWindow, both reachable
only from conductSharedCollection, so every deferred lambda has a
matching same-thread drain. The WS(ii) closed list stays closed:
this body no longer mutates HandleSet/joinLock from inside a stop.
U-T7 dead-lock-object-with-pending-asyncHold sweep-storm amplifier;
gate test JSTests/threads/cve/mc-gc-thread-shell-finalizer-storm.js.

### F.4 DropAllLocks GIL-off (IA D1; ANNEX DAL2, BINDING — full
text)

Main/embedder: drops m_lock + token (§F.4 main arm; GIL-on stands).
**Spawned: a HEAP-ACCESS bracket, NOT a pure no-op (r20 F1):**
1. Ctor: releaseClientHeapAccess() on the CURRENT lite's client (F8
   mandatory-revert, seq_cst exchange->NoAccess); returns 0; token,
   entry depth, m_lock, m_lockDropDepth ALL untouched
   (JSLock::currentThreadIsHoldingLock() stays mutex-literal false,
   JSLock.cpp:423-425). Dtor: re-acquire the SAME client's access
   §A.3.2b/§A.3.8-gated (SB1 ordering), then poll the lite's trap
   bits before returning to JS. Nesting: per-lite DAL depth counter;
   only the OUTERMOST bracket transitions access (inner = pure
   count). LIFO not required (no m_lockDropDepth participation) — the
   D1 livelock shape cannot recur.
2. Effect: an embedder blocking section on a spawned thread is
   access-released for the heap §10.4 barrier AND counts for the
   §A.3.2 conductor predicate; trap delivery is deferred to the
   dtor's poll (same shape as §F.5 nested-entry deferral).
3. Lock context: DAL ctor/dtor are access transitions — per the §E.2
   rule they run holding NO api rank-1..3 lock and no heap 10a/10b
   lock. U20's lint covers DAL sites.
4. SUPERSESSION (INTEGRATE-api D1's phase-1 constraint — no
   DropAllLocks on the shared VM while spawned Threads are live,
   INTEGRATE-api.md:834-847 — vs this, both sides; IU row): LIFTED
   GIL-off for spawned threads by this bracket; GIL-on keeps the
   constraint until the flip.
5. Embedder contract: §F.6 delta (c) — a spawned-thread blocking
   native section using NEITHER DAL NOR §J.3 must RHA/AHA-bracket per
   heap §9 (SPEC-heap.md:244). The IU embedder checklist enumerates
   Bun's DAL/blocking host-call sites (U-T8 row).
6. U14 re-derived: "spawned DAL = access bracket, token/depth
   invariant, returns 0". U24 arm: spawned thread blocked in a
   DAL-bracketed native call while main conducts a shared GC AND a
   haveABadTime (§K.5) stop — both complete; release the native call
   — the thread resumes and observes deferred traps.

### F.5 Nested foreign-VM entry (r10 F2 full text; owns §A.3.6's
nested window; U30)

**CALLER SCOPE (r27, TERM1.5): main/embedder carriers ONLY** —
SPAWNED foreign-VM lock() RELEASE_ASSERTs (A36 single-VM v1; §F.6(e);
not an SD; the refusal is a process-abort RELEASE_ASSERT with a
message naming §F.6(e), NOT a catchable error — an embedder-contract
violation; pure JS cannot reach it; r10 F2's rejection of "option
(b)" concerned the carrier-side Bun JSContext-inside-host-call
pattern, which executes on a main/embedder carrier — nothing licenses
SPAWNED nesting; post-ungil: revisit as a catchable TypeError if a
real embedder pattern needs it; corpus: U27/U-T6 gain a
spawned-foreign-VM death-test arm expecting the assert message).

lock() on VM B while holding any other VM A's token FIRST releases
A's client heap access (F8 mandatory-revert: seq_cst
exchange->NoAccess) BEFORE installing B's carrier: in the nested
window T counts access-released for A's heap §10.4 barrier AND the
§A.3.2 conductor predicate (heap I4(b): A's JS frames stay alive via
the conservative machine-thread stack scan — the thread remains
registered; T mutates only B's heap while nested). A's
trap/stop/termination delivery is DEFERRED to the LIFO restore: it
re-acquires A's access gated (parks if A is mid-stop), re-stamps A's
client (A36C), and polls A's bits before any A JS. DELIVERY DEFERRAL
recorded: outer-VM termination/§A.3 latency is bounded by the nested
window, NOT by U2 (U2 re-scoped to threads whose CURRENT lite belongs
to the polled VM); not an SD (GIL-on nesting already defers via the
handoff protocol). The rule applies per nesting level (LIFO stack of
releases). Corpus (U-T6): shared-VM GC requested while an embedder
thread is nested in the second VM; IH row (heap §9
blocking-primitive note gains the cross-VM-JS bullet), IV row
(§6.4.4 nesting note).

### F.6 Embedder contract (r17 F4 + r20 F1 + ANNEX EC1, BINDING —
full text)

Five GIL-off deltas on embedder (Bun) code, NORMATIVE:
- (a) m_lock excludes only embedder threads (§F.1) — Bun's
  out-of-tree JSLockHolder critical sections today exclude ALL JS and
  silently stop excluding spawned threads GIL-off; the IU checklist
  carries the Bun JSLockHolder exclusivity audit.
- (b) embedder-REGISTERED ordinary-promise reactions settled by a
  spawned thread run on the settler (SD10/§E.1b.1, X1.7) — off
  m_lock, off the embedder loop. A carrier-hop demand = a NEW
  negotiated SD (the per-reaction registrant hop was rejected for v1,
  §E.1b.1); a late carrier-hop SD would reshape §E.1b/§E.3/§E.4 + the
  U-T9 corpus — hence the sign-off timing below. The ALS slice is
  discharged by ANNEX ALS1 (Bun need not demand a hop for ALS).
- (c) spawned-thread blocking native sections using NEITHER §F.4 DAL
  NOR §J.3 must RHA/AHA-bracket per heap §9 (DAL2.5).
- (d) **FIRST-VM-WINS (U0c/ANNEX EC1):** under gilOffProcess the
  FIRST VM CONSTRUCTED wins the ctor CAS
  (Heap::tryDesignateStickySharedServer, Heap.cpp:4123; I13
  RELEASE_ASSERT :4124; one sticky server per process EVER — a §10D
  reversion does not free the slot) and is the ONLY spawn-capable VM
  for PROCESS LIFETIME (others spawn-RangeError, U0b). A "utility" VM
  constructed first (config parsing, pre-boot snapshot, diagnostics)
  PERMANENTLY demotes the real main VM — there is no re-designation
  API in v1 (deliberate: I13 + the immutable m_gilOff byte are
  load-bearing for §A.1.3 codegen). NORMATIVE: (1) the embedder MUST
  construct the VM intended to spawn Threads strictly before any
  other VM; (2) recommended: immediately after constructing the main
  VM, boot-assert vm.m_gilOff == 1 so a violated construction order
  fails at boot, not at first Thread() with a confusing RangeError;
  (3) the IU embedder checklist gains a construction-order audit row
  (enumerate every VM construction site in Bun, incl. lazily created
  helper VMs; prove main-first or gate them behind first-entry of the
  main VM); (4) v1 explicitly declines a designation override option
  — revisit post-ungil if Bun's boot order cannot guarantee
  main-first.
- (e) native code on a spawned Thread never enters/creates another
  VM (§F.5 RELEASE_ASSERT; A36).

IU row = the embedder checklist (JSLockHolder audit;
continuation-affinity disposition; blocking-site enumeration, U-T8;
(d) construction-order + (e) audits). **Sign-off SPLIT (r21 F2, §D.2
shape):** (b)'s SD10 disposition = HARD precondition of **U-T9
ENTRY**; (a)/(c)/(e) stay U-T14 close items; (d)'s audit row joins
the U-T14 close items.

---

## G. Per-thread blocking policy

Replaces the per-VM G11 gate (jsThreadsCanBlockOnCurrentThread).
1. Per-THREAD predicate mayBlockSynchronously(): spawned TS = true;
   main/embedder = embedder policy
   (isAtomicsWaitAllowedOnCurrentThread()).
2. Governs ALL sync parks: TA/property Atomics.wait (KEPT G11 gate,
   §C.4), join, contended lock.hold, cond.wait; violations throw the
   existing TypeErrors (api I18 intact).
3. The D4 GIL-dropped main TA wait machinery is GIL-on-only; GIL-off
   a permitted main sync wait parks per §J.3. D8 per §C.6.

---

## H. SymbolRegistry / Symbol.for

Closes vmstate Dev 8: WTF::SymbolRegistry's m_table gains Lock m_lock
(destructor-leaf, §LK.8) — symbolForKey, remove, destructor walk;
~StringImpl's registered-symbol arm calls remove() under it (any
thread, incl. in-lock sweep); registries destroyed in ~VM after
spawned exit (U16). (A plain leaf lock, not a sharded/concurrent
registry: Symbol.for traffic is orders of magnitude below atomization
traffic; the non-goal carries an explicit bench-evidence reopen
condition.)

---

## I. Wasm on spawned threads — REFUSED in v1

Closes UNGIL-PLAN I. Wasm EXECUTION from a spawned Thread throws
TypeError (WARM calls incl.). NORMATIVE (both GIL modes, SD7):
1. The WebAssembly ctor/compile surface throws on a spawned TS —
   full list (r22): WebAssembly.{compile,instantiate,validate} +
   Module/Instance/Memory/Table/Tag/Global ctors.
2. Under useJSThreads, jsCallICEntrypoint() returns nullptr AND every
   generated JSToWasm entry emits a spawned-TS prologue check.

**Discriminator (r7 F4 full text):** an L2-append uint8_t
**VMLite::isSpawned** (=1 stored at spawned lite registration BEFORE
setCurrent — §B.1 ordering makes it visible before any wasm entry can
run on that thread); main/embedder carriers (incl. GIL-off lazy
carriers) keep 0; the emitted check = loadVMLite -> null => fall
through (no lite = not spawned; compiler threads never execute
JSToWasm entries, and fall-through is the safe polarity) -> load byte
-> branch-to-throw — **NOT TID-tag** (TID!=0 is wrong GIL-off: §A.3.6
gives every main/embedder thread a nonzero carrier TID, so a tag
check would throw on main/embedder wasm — which §I does NOT refuse —
while still passing U17's spawned-positive tests). isSpawned: SPAWN
lites only; spawned single-VM (§F.5) => the CURRENT-lite byte always
agrees with isJSThreadCurrent() (ditto §A.2.7; TERM1.4). U17 NEGATIVE
arm: carrier non-GC wasm never throws, both GIL modes (the only arm
that can catch a broken discriminator). C++ gates keep
isJSThreadCurrent() (ThreadManager.cpp:157 — a WTF::ThreadSpecific,
not loadable from generated code). GIL-on corpus edited (SD7).
EXECUTION only; §N.6 rules wasm-buffer grow/detach/shrink.

**Wasm-GC (r9 F8 full text):** heap §5.5/manifest 11's RELEASE_ASSERT
(JSWebAssemblyInstance.cpp:142) made MAIN-thread wasm-GC
instantiation a process abort under useJSThreads. SUPERSESSION (heap
§5.5/manifest 11, both sides; the §5.5 never-populate rule itself
stands): a hasGCObjectTypes() precheck BEFORE instance construction
throws WebAssembly.LinkError (compile-side: CompileError), both GIL
modes; the RELEASE_ASSERT remains only on non-JS-reachable paths. Not
an SD (was an abort, not a behavior). U17 positive arm: LinkError, no
abort. IU row; U-T13 owner.

(Why refuse rather than GIL-serialize wasm: a "wasm takes the GIL"
hybrid reintroduces a global lock with all of §F's deleted semantics
for an unaudited subsystem, creates a priority-inversion channel, and
is still unsound where wasm calls back into JS. The TypeError is
honest, testable (U17), cheap to lift later under a dedicated
charter. The gate being active GIL-on too (SD7) buys mode-equal
corpus behavior.)

**SUPERSESSION (r33, this section's carrier-wasm arm only):** GIL-off
(vm.gilOff()), wasm is refused on ALL threads of the VM — including
the main/embedder carrier — at the same ctor/compile surface
(JSWebAssemblyHelpers.h throwIfWebAssemblyRefusedOnSpawnedThread,
second arm). Reason: a gilOff carrier lite publishes exceptions
per-lite (VM::setException -> group3Primitives()), but every
wasm-tier emitted exception check still consumes the inert VM-block
word (WasmToJS.cpp/JSToWasm.cpp/WebAssemblyBuiltinTrampoline.cpp
`Address(gpr, VM::exceptionOffset())`; InPlaceInterpreter.asm reads
AND a raw `storep ... VM::m_exception[t1]`), so carrier wasm GIL-off
silently loses exceptions. The U17 NEGATIVE arm ("carrier non-GC wasm
never throws") is narrowed to GIL-on / flag-off until the wasm
emission sites get the per-lite mode split (incl. the IPInt asm
arms), at which point this supersession lifts. GIL-on and flag-off
behavior are unchanged.

---

## J. GIL-machinery end state (GIL-on unchanged — oracle)

- **J.1** useThreadGIL: KEPT, supported fallback; default flips false
  at the milestone gate.
- **J.2/J.4/J.5** jsThreadGILHandoffYield + D2 notify-yield (§C.5),
  GILParkSavedExecutionState + resetForFreshThread: dead GIL-off
  (state per-lite, §A.1); J.5 compiled out;
  unlockAllForThreadParking is NOT dead — re-derived by J.3.
- **J.3 GILDroppedSection, GIL-off by caller (r10 F5 + r7 F2 full
  text, BINDING; U31).**
  - Spawned (token-only) = access release + §A.3 park cooperation +
    the §A.3.2b post-wake poll.
  - Main/embedder park sites (join, cond.wait, TA/property
    Atomics.wait) ALSO release m_lock + token via the
    unlockAllForThreadParking shape (JSLock.cpp:389-408; drain
    suppressed via the m_lockDropDepth bump as landed) — holding
    m_lock across the wait would be a strict regression vs GIL-on
    and an outright deadlock for the Bun pattern where the notifying
    store runs on a second embedder thread that must first enter the
    VM through JSLock::lock().
  - **Captured-lite poll (r10 F5):** park sites CAPTURE the entered
    VM's carrier lite pointer BEFORE the release; per-quantum polls
    read the CAPTURED lite's bits + the waiter-state atomic, never
    VMLite::current() (the release path runs the §A.3.6 LIFO
    restore, so CURRENT is the prior lite — null for a Bun thread
    that entered from native — for the whole park). Lifetime proof:
    a carrier dies only at owner TLS death or the ~VM walk; the
    owner is alive mid-park, and ~VM while this VM's JS frames are
    live on a parked thread is an embedder error (vmstate M6
    precondition) — the captured pointer cannot dangle. §A.2.4's D9
    clause is re-pointed accordingly (PARK lite).
  - Per-quantum wakes poll ONLY lock-free state (waiter atomic +
    lite trap/stop bits) under the rank-3 lock (U2's bound); FULL
    reacquisition happens EXACTLY ONCE per acquisition episode
    (final exit, or annex-W W1 early service — re-parking opens a
    new episode), after all rank-3 locks are released (api 5.9(e);
    NLS::m_lock exempt, §E.2), re-running the §A.3.6 carrier/tag/
    client swap + §F.1 service-word OR before re-checking the wait
    condition (re-check mandatory: the condition may have changed
    while unlocked).
  - C-API corpus arm: main parks in property Atomics.wait, a second
    embedder thread enters and notifies; conductor stops mid-park.
    U15 + U31; U-T11 carries the implementation.
- **J.6** JSLock handoff body: §F.1 drain + §A.3.6 swap KEPT; rest
  skipped per §§A/B/E/F (GIL-on load-bearing).
- **J.7** JSLock.cpp:151 backstop (L1): REPLACED — the GIL-off branch
  RELEASE_ASSERTs U1 (incl. the A36C client clause; "tid-0 never
  installed" folded into U1).
- **J.8** Stub witnesses W2/W3/W4 + the OM stub witness: DELETED at
  U-T5, both modes.
---

## K. GIL-serialized VM/global caches + lazy init (NEW)

The GIL is today's ONLY serializer for VM-/JSGlobalObject-resident
mutable state outside Group 3. Rulings (GIL-on/flag-off unchanged):

### K.1 Per-lite duplicates (L2)

Hot per-op scratch/caches. The BINDING list is **annex K4 §II**
(reproduced in the Audit Tables section below: K4.II.1-19, plus
K4.VIII.10 m_weakRandom, the ALS1.3 cursor m_asyncContextData, the
AUD1.K2/SD19 m_regExpGlobalData, and the AUD1.K3(b)
m_synchronousModuleQueue). GIL-off accessors route to the CURRENT
lite's copy; cell-holding copies are GC-scanned via the registry walk
(§A.1.3 GC-roots rule, per-VM filter); the ~VM walk frees per-lite
copies. JIT-baked addresses among them follow the A16 extension
(AUD1.K4).

### K.2 Leaf locks

Cold/keyed VM caches whose hits must be shared. The BINDING list is
**annex K4 §III** (below). RegExpCache is ALREADY locked
(RegExpCache.h:79) — the §K.2 exemplar; unlocked peers get a leaf
Lock (§LK.7). Weak-handle creation inside any of them obeys WS1(i):
hoist Weak construction BEFORE the lock, publish under it.

### K.3 Atomic lazy publication (r16 F2 + ANNEXES LZ1 + LZ2 + r25
ext, all BINDING — full text)

LazyProperty/LazyClassStructure (runtime/LazyProperty.h; tags
:114-115, slow path :95-97) + VM ensure* + initLater:
- First-touch: load-acquire fast path; the initializing CAS RECORDS
  the OWNER (r16 F2 — implementation may use a per-VM side table
  {property address -> carrier TID} under a leaf lock, or spare bits
  adjacent to initializingTag); the winner initializes lock-free and
  release-stores the result (the release-store IS the publication).
- OWNER re-entry returns null — exactly the landed recursion contract
  (LazyProperty.h:75-76; LazyPropertyInlines.h:99-100).
- FOREIGN threads wait PARK-CAPABLE in bounded quanta (GIL-off only,
  not an SD): release heap access (E.2 ordering, no lock held), poll
  BOTH stop families (the lite §A.3 stop bit AND heap §10 per-client
  GC stop state via stopIfNecessary — polling one alone deadlocks: a
  loser spinning WITH access while the winner's allocating
  initializer triggers a collection is a three-way deadlock, r6 F2),
  re-acquire via the §A.3.2b-gated path, re-test the load-acquire.

**LZ1 — cycle escape + init abandonment (BINDING; r19 F2):**
1. Wait-for edges: the per-VM owner side table additionally records,
   per in-flight init, the set of waiting threads — a foreign waiter
   publishes (self -> ownerOf(P)) under the leaf lock BEFORE its
   first park quantum on P, erasing it when it stops waiting.
2. CYCLE escape: before EACH park quantum (under the leaf lock,
   bounded walk — at most one in-flight init per thread, so the owner
   chain is a function), the waiter follows owner-of -> waits-on
   edges from P; if the chain reaches the waiter ITSELF (possible
   only if it OWNS some in-flight init Q), get() returns null — the
   landed owner-null contract extended to cross-thread ownership
   cycles. Sound: cycle membership is stable while all participants
   wait; at least one participant detects and nulls, unblocking the
   rest. GIL-on unchanged; NOT an SD.
3. ABANDONMENT: the winner installs an unwind scope around the
   initializer call; ANY non-normal exit (JS/C++ exception,
   termination at an in-initializer poll site, §E.5 thread
   termination) CASes lazyTag initializing->empty and erases the
   side-table entry BEFORE propagating. Foreign waiters re-test and
   observe empty; a later toucher re-runs the initializer (sound:
   initializers publish only on success; partial work is garbage).
   Thread exit (§E.2 T5) and the ~VM walk ASSERT the thread owns no
   in-flight init.
4. U26 arms: a deliberately recursive initLater initializer (touches
   its own property mid-init, expects null) + a concurrent foreign
   toucher that must park then observe the initialized value; (d)
   two-thread crossed lazy-init cycle (A inits P touching Q, B inits
   Q touching P — no hang, at least one inner touch nulls, both end
   initialized); (e) terminate the owner mid-init, then foreign touch
   — re-run completes; plus the r6 F2 forced-full-GC-during-winner-
   initializer liveness arm.

**LZ2 — first-touch PRECONDITION (BINDING; r20 F2 + r21 F1):**
- Lock context: no §K.3 first-touch site may run holding any api
  rank-1..3 lock, heap 10a/10b lock, a §N cell lock, or a §LK.8
  destructor-leaf hold (a foreign waiter's access-release park would
  violate heap I6/OM O2; the winner's allocate/GC would violate OM
  O1).
- LZ2.1 PROHIBITION: no §K.3 first-touch (winner OR foreign) may
  execute from inside a §A.3 or heap §10 stop window while the
  executing thread is acting as CONDUCTOR (incl. the §A.3.5 CLASS-4
  variant, the §A.2.7 walk, and D1R in-stop fires). Foreign-touch
  consequence: park-capable wait while holding GCL + the §LK.4b slot
  mutex against an owner parked under the conductor's own stop —
  unbounded, unescapable (LZ1.2's walk terminates at the parked
  owner). Winner-touch consequence: §A.3.5(ii) violation or
  HBT2/HBT3 fail-hard spurious OOM.
- LZ2.2 DISCHARGE: every lazy member reachable from a conductor
  closure must be (a) proven pre-initialized at every call site that
  can become a conductor, or (b) pre-resolved by the conductor BEFORE
  arbitration — before acquiring the §LK.4b slot mutex, while still
  an ordinary mutator able to win/wait per §K.3 — or (c) re-ruled §K
  class 1 (per-lite) or class 2 (leaf lock). U-T8b's touch-context
  table gains a conductor-closure-reachable column recording
  (a)/(b)/(c) per member.
- LZ2.3 NON-FIX (normative rejection): a conductor MUST NOT
  abandonment-CAS (LZ1.3) a parked owner's in-flight init — LZ1.3 is
  owner-unwind-only; a foreign reset races the owner's release-store
  on resume and republishes partial state. Any future
  foreign-init-cancel scheme requires a new negotiated annex.
- LZ2.4 U26 arm (f): owner parks mid-init at a poll site under an
  incoming stop; a CLASS-4 conductor whose closure would touch the
  same member runs; no hang (the member was pre-resolved per
  LZ2.2(b)) and the owner's init completes post-resume with a single
  publication.
- LZ2.5 U20 lint: flag any §K.3 touch site dominated by
  Heap::JSThreadsStopScope construction or §LK.4b slot-mutex
  acquisition (conservative interprocedural domination over the
  conductor entry points in LZ2.1); offenders must carry an LZ2.2
  disposition. U20 also lints park-under-10a/10b and
  access-transition-under-10a/10b (covers DAL2.3 and every §E.2-rule
  site).

U-T8b columns; U26 arms per LZ1.4 + LZ2.4.

### K.4 Inventory audit — EXECUTED

U-T8b EXECUTED -> **SPEC-ungil-audit-K4.md** (BINDING annex K4; rows
K4.<table>.<row>; reproduced below). Implementation CONSUMES the
tables verbatim; §F.2 EXCLUSIVITY CONSUMERS cite K4 rows. Residue
rulings: §K.6. U26.

### K.5 Class 4 — requires-stop (r16 F1)

GIL-serialized writers that iterate/rewrite OTHER threads' objects.
**JSGlobalObject::haveABadTime (:2900; JS-reachable :2460) — ANNEX
HBT as AMENDED by HBT2-HBT4 (all BINDING — full text):**

GIL-off, the ENTIRE haveABadTime body from the isHavingABadTime()
early-return to the end of the conversion walk runs as ONE §A.3
thread-granular stop:
1. The calling mutator requests the stop (§A.3.3 arbitration; it is
   the conductor). Arbitration losers park and RETRY; on retry the
   isHavingABadTime() early-return makes double entry idempotent for
   the same global; DIFFERENT globals' calls serialize through the
   same arbitration (each runs its own complete stop).
2. After all other entered threads are parked/not-entered/
   access-released (§A.3.2, gated by 2b), the conductor RE-CHECKS
   isHavingABadTime() (another thread may have completed the same
   transition while it waited), then runs the landed body unmodified
   ON ITS OWN CLIENT per the §A.3.5 CLASS-4 variant (HBT2/HBT3
   order: default R1.i access-release -> arbitration -> GCL
   access-released -> own-client F8 AHA re-acquire BEFORE fanning
   bits; access then RETAINED, allocation legal in-window —
   ArrayStorage conversion, Vector growth; no in-window GC per
   HBT2.2; DeferGC enqueues an RCAC ticket).
3. Ordering: the m_havingABadTimeWatchpoint fire + structure
   transitions happen INSIDE the stop; mutator re-entry/
   re-acquisition is blocked until resume (§A.3.4/§A.3.8 via
   §A.3.2b) — no thread can allocate in a fast indexing mode after
   the watchpoint fired but before the walk saw the heap. The
   CONCURRENT-COMPILER half is unchanged: the landed jit I2/R1
   watchpoint/jettison protocol (the :2854 comment's race) covers
   compiler threads, which do not park under §A.3 (jit R1.f
   cooperative set = mutators). Jettison: jit I2/R1 (:2854).
4. StructureCache invalidation (:2814) and the multi-global
   dependency pass run inside the SAME stop (one stop per
   haveABadTime call).
5. GIL-on/flag-off: unchanged (the GIL is the serializer; no stop
   requested).
6. §K.4 taxonomy WIDENED: class 4 "requires-stop" — any
   GIL-serialized writer that must iterate or rewrite other threads'
   objects/structures. Each class-4 ruling must name its stop kind
   (§A.3 stop vs piggyback on a heap §10 stop, like §D.1's rebias)
   and its double-entry serialization. Peers route here (annex K4
   §VI: watchpoint-set firing, VM::deleteAllCode/deleteAllLinkedCode,
   the StructureCache bulk-clear path).
7. Corpus (U-T13): spawned thread allocation + indexing-type
   transition storm racing main installing an indexed accessor on a
   shared prototype (triggers haveABadTime); all affected arrays read
   SlowPutArrayStorage semantics after; a two-global double-fire arm;
   TSAN + amplifier; plus the HBT2.5/HBT3.5 arms (§A.3.5 above).

### K.6 r26 audit-residue rulings (ANNEX AUD1, BINDING — full text;
rows re-ruled in annex K4 §0)

- **AUD1.K1 (K4-U1) SamplingProfiler — SD18.** GIL-off the profiler
  runs in §A.1.7 form (i) and samples ONLY the main/carrier thread's
  lite; spawned frames are NEVER captured; start/stop/shutdown/
  reports remain main-thread APIs (SD13/SD14 family); internals keep
  m_lock (SamplingProfiler.h:218); m_jscExecutionThread binds to the
  main carrier; the (i)-reader SUSPEND RULE (r24) applies to its
  stack walk. GIL-on unchanged. SD18 (GIL-off only): profiles omit
  spawned-thread samples. N-thread capture (per-lite frame buffers +
  registry iteration under a §A.3 stop) is chartered post-ungil.
  Corpus: profiler-on + 2 threads -> no crash, main-only samples;
  U-T2 arm.
- **AUD1.K2 (K4-U2 = N7-U7) RegExp legacy statics — SD19.**
  JSGlobalObject::m_regExpGlobalData (RegExpGlobalData.h:64-65,
  RegExpCachedResult.h:66-82) becomes a §K.1 per-lite member: each
  entered thread owns a private RegExpGlobalData stream; cell-holding
  copies join the registry-walk root set; the ~VM walk frees them.
  SEMANTICS (SD19, GIL-off only): RegExp.$1-$9 / lastMatch /
  leftContext / rightContext / input observe ONLY matches performed
  by the CURRENT thread. (Deprecated Annex-B statics; a global lock
  would put a §LK acquisition on EVERY successful match and still
  yield nondeterministic cross-thread values.) TIERS: DFG/FTL
  RecordRegExpCachedResult + every offsetOfResult/offsetOfLastInput/
  offsetOfLastRegExp consumer re-pointed through the lite per AUD1.K4
  (A16 ext); gilOff-mode compilation emits loadVMLite ->
  liteRegExpGlobalData -> field; flag-off keeps the baked
  global-object-relative address. The reify flip (m_reified + 4
  barriers) stays single-thread-private => plain stores. Corpus:
  regexp legacy-statics arm, GILOn/GILOff variants.
- **AUD1.K3 (K4-U3) module evaluation state.** (a)
  VM::m_moduleAsyncEvaluationCount (VM.h:1332): std::atomic
  fetch_add, relaxed (ECMA [[AsyncEvaluation]] needs only
  uniqueness+monotonicity; cross-thread interleaving of independent
  graphs is otherwise unobservable; no SD). (b)
  VM::m_synchronousModuleQueue (VM.h:1358, Bun): per-lite (§E.1
  family) — each thread drains its OWN synchronous module jobs with
  its microtask queue; enqueue sites route via the CURRENT lite. (c)
  Cross-thread Evaluate() of ONE record: AbstractModuleRecord status
  advance (Linked -> Evaluating) is a CLAIM under the record's cell
  lock (the lock already serializing m_dependencies/
  m_asyncParentModules, annex N7 R16); the winner evaluates (user JS
  OUTSIDE the lock); losers re-read status under the lock and (async
  graph) adopt the existing top-level promise per spec, or (sync
  completion required) PARK-CAPABLE wait access-released on the
  record's completion, §K.3-wait shape (bounded quanta, §A.2.4
  polls; LZ2 preconditions apply to the waiting site). Settled
  completion published release; errored => rethrow per spec. GIL-on
  unchanged. Supersedes annex K4's interim "main-thread evaluation"
  note (K4.V.18 re-ruled per-lite + claim protocol, NOT main-only).
- **AUD1.K4 (K4-U4)** = the A16 EXTENSION, inlined at §A.1.6 above.
- **AUD1.K5-K7 (K4-U5/U6/U7) MECHANICAL:** K5 = the DWT lock-name
  equation (§E.7.1 above). K6 = JSGlobalObject::
  m_canFastQueueMicrotask / m_associatedContextIsFullyActive: writes
  main-only (debugger/context attach, SD13 umbrella), reads
  relaxed-atomic from any thread; a stale-true window at most skips
  debugger microtask observation = SD13-class degradation, no new
  SD. K7 = SmallStrings verification PASSED: initializeCommonStrings
  runs in the VM ctor (VM.cpp:335); the !m_isInitialized fallback
  (SmallStrings.cpp:121-127) allocates a fresh AtomStringImpl and
  writes NO member; setIsInitialized(false) is teardown-only
  (VM.cpp:707); immutable-after-init CONFIRMED; gets the K4 §VIII
  no-write-after-first-cross-thread-entry assert.

---

## N. Builtin cell-internal mutable state (NEW)

OM §9.5/I21 cover PROPERTY slots; §K covers VM/JSGlobalObject
members. Multi-word C++/internal-field state INSIDE other shareable
cells was unruled (r7 F1; THREAD.md:200 "no race should ever lead to
a VM crash"; THREAD.md:470 names SparseArrayValueMap and
Map/WeakMap/Set as lockable). **DEFAULT GIL-off protocol
(GIL-on/flag-off unchanged): mutations AND structure-traversing reads
under the cell's JSCellLock (10a), §E.1b shape — allocate OUTSIDE,
re-validate under, never allocate/park holding it (OM I20).**
Rulings:

1. **JSMap/JSSet (JSOrderedHashTable) + JSWeakMap/Set (WeakMapImpl):**
   ALL ops cell-locked, reads too (rehash/delete splices the backing
   store — lock-free probes UAF). DFG/FTL map intrinsics
   (MapHash/MapGet/MapSet/WeakMapGet, DFGNodeType.h:608-629) DISABLED
   GIL-off -> locked native bodies; revival is a post-ungil perf
   item (§B.6's deferral shape).
2. **Rope resolution/atomization (JSString.h:637-682): lock-FREE** —
   the resolver computes into a fresh buffer and publishes by ONE
   release-CAS of the fiber0/flags word; losers discard + re-read;
   readers load-acquire; resolveRopeToAtomString does the same vs the
   shared sharded atom table (U0), whose insert is already
   concurrent; JIT rope slow calls land here; §C.3's resolve = this.
   (Cell lock REJECTED: strings are read-hot, resolution idempotent;
   losers' buffers are garbage; readers already branch on isRope.)
3. **DateInstance GregorianDateTime cache (DateInstance.h:62-75):
   BYPASSED GIL-off** (the cached pair is >8 bytes, not CASable;
   per-call caller-local computation is strictly correct and only
   costs the cache win under sharing); m_data lazy alloc
   CAS-published; vm.dateCache = §K.1/2 (ruled K.1, K4.II.11).
4. **FunctionRareData (JSFunction.h:136-144):** materialize per §K.3
   (exactly the lazy-publish shape); internals (structure caches,
   watchpoint-bearing fields) mutate under the function's cell lock;
   profiling-only fields racy-tolerated (jit item 7); cached
   Structures consumed under I34 re-validation.
5. **Non-promise JSInternalFieldObjectImpl** (generators, async
   fns/generators, iterator helpers; r11 F4 + r12 F4 + r15 F1 + r17
   F5 + r25 ext, all BINDING — full text):
   - Single-word **resume-claim CAS** SuspendedX->Running on the
     STATE field (the builtin check-then-store —
     GeneratorPrototype.js:36 @putGeneratorInternalField after the
     :60/:77/:91 state checks — becomes ONE claim). At-most-one-
     resumer exclusion keeps interior frame/field stores WHILE
     claimed PLAIN + tier-inlined (one CAS per await/yield —
     §B.5's premise).
   - **Claim FAILURE dispatches on a RE-READ (r12 F4; no SD):**
     Executing => the existing already-running TypeError; Completed
     => the landed completed path (GeneratorPrototype.js:35's guard:
     {value: undefined, done: true} for next(), the landed
     return/throw semantics for abrupt variants); another SuspendedX
     (winner resumed and yielded back) => retry the claim with the
     new expected value — each retry is the legal serialization
     "loser ran entirely after that winner". The rule is uniform per
     site: failed claim => re-read => the landed serial path for the
     observed state (per-site column in the N7-consumed table).
   - **UNCLAIM transitions are store-RELEASE (r15 F1; replaces the
     r11 "stay plain" sentence):** Running->SuspendedX and
     Running->Completed MUST be release stores on the state field's
     EncodedJSValue word in ALL tiers — the publish-release pairs
     with the claim CAS's acquire half, so the winning claimant
     inherits happens-before over the previous owner's frame/field
     stores (plain stores torn frames on arm64; SD6 means two
     threads alternate gen.next() with no other sync edge).
   - **Twin intrinsics** @atomicInternalFieldClaim(cell, index,
     expected, replacement) -> bool / @atomicInternalFieldPublish
     (cell, index, value), emitted UNCONDITIONALLY in all modes/flag
     states (single-threaded they are observably identical to the
     landed sequences — avoids mode-conditional bytecode; rides
     §A.1.3 flag-off delta (b2); golden gates re-baselined once for
     both).
   - **LOWERING is mode-keyed (r17 F5; replaces the r11/r15 lowering
     sentences):** LLInt/Baseline branch on the JSCConfig
     gilOffProcess byte — false (flag-off + every GIL-on process) =>
     the landed INLINE plain get+compare+put (claim) / plain store
     (publish), i.e. today's machine code behind one not-taken
     branch (delta-(a) class; nests in ifJSThreadsBranch regions —
     zero new flag-off branches there); true => host-op slow path
     (v1; LLInt inline CAS via the jit annex R5 emitter class is the
     NAMED CONTINGENCY if the gilOff-arm cost matters). DFG/FTL: the
     AtomicInternalFieldClaim/Publish nodes lower at codegen time on
     the COMPILED-FOR VM's mode: gilOff => seq_cst 64-bit strongCAS
     / release store (storeStoreFence+store or stlr) via the
     existing internal-field offset machinery, write barrier as
     PutInternalField; else the landed plain nodes.
   - Bench contract: the §B.5 r9 async/generator microbench joins
     the BENCH.md flag-off suite as a GATED benchmark at the
     standard 1% threshold vs the pre-threads baseline; its gilOff
     configuration is RECORDED under the §B.5 composite, not
     separately gated. The r10 F6 --useJIT=0 in-noise re-run stands
     and covers these sites.
   - Annex N7 lists claim+publish sites (GeneratorPrototype.js
     resume head :36 + Completed stores :41/:47 + the
     generatorResume suspend path + AsyncFunction/AsyncGenerator/
     iterator-helper equivalents); the cell lock is used ONLY for
     named multi-word cases. Amplifier (TSAN AND arm64 hardware):
     two threads ping-pong next() on ONE generator whose body
     round-trips a per-resume counter through frame state; every
     observed value is the predecessor's published value.
   - (Concurrent resume mapping onto the EXISTING serial "already
     running" TypeError is why §N.5's TypeError is NOT an SD.)
6. **ArrayBuffer detach/transfer/resize + wasm grow (ANNEX N6,
   BINDING — full text; r12 F2, r13 F1/F3, r14 F2).**
   PRINCIPLE: every tier's TA fast path loads LENGTH, bounds-checks,
   then loads BASE; the reader's two loads carry no ordering, so
   store ordering alone cannot close a torn two-word read. INVARIANT:
   a racing reader must NEVER pair a passing length with an
   unmapped-or-short base — any observable base must point at a
   mapping that is mapped and sized >= every length still observable
   against it; retirement of a mapping requires that no
   pre-retirement length remain live, which heap §10 stop quiescence
   provides (no JS/JIT fast path straddles a stop).

   Arms (GIL-off; GIL-on/flag-off unchanged):
   1. **DETACH-AND-FREE** (ArrayBuffer::detach(VM&),
      ArrayBuffer.cpp:525-528): publish length=0 (seq_cst store) + a
      separate detached FLAG (isDetached() becomes the flag, NOT
      !m_data); the base word is NOT cleared. The move-destructed
      ArrayBufferContents moves INTO a per-server quarantine list
      entry — the entry OWNS the contents (m_data + m_destructor +
      m_memoryHandle), hence the eventual free. A heap §10 stop
      retires entries enqueued before the stop (OM §6 epoch shape):
      under quiescence the base word is cleared/poisoned, then the
      entry destroyed (mapping released). notifyDetaching/neutering
      watchpoints fire as landed — hoisted-vector code jettisons; the
      quarantine additionally covers code that raced the jettison.
   2. **TRANSFER** (ArrayBuffer::transferTo, ArrayBuffer.cpp:498;
      detach-by-move at :519): GIL-off the detachable non-shared arm
      is REWRITTEN as **COPY + DETACH** — the transferee gets a FRESH
      allocation, then the source runs arm 1 verbatim (its contents —
      the original mapping — enter the quarantine owning the free).
      Allocation of the copy (r14 F2): source WITHOUT
      m_hasMaxByteLength => plain m_contents.copyTo(result)
      (ArrayBuffer.cpp:233-244); source WITH m_hasMaxByteLength =>
      copyTo is INSUFFICIENT (it copies only m_data/m_sizeInBytes;
      ArrayBuffer.prototype.transfer's resizable path routes through
      this arm and then resize()s the transferee,
      JSArrayBufferPrototype.cpp:330-346) — allocate via the
      tryAllocateResizableMemory shape (:108-141) with the SOURCE's
      maxByteLength reservation; stamp m_maxByteLength,
      m_hasMaxByteLength, and the NEW m_memoryHandle onto the result
      BEFORE the memcpy of byteLength() bytes. OOM in either shape =>
      transfer fails as the landed non-detachable arm does. The
      post-transferTo resize of the transferee is thread-local (the
      JSArrayBuffer wrapper is created only afterwards, :341-346 —
      no concurrent reader). isShared()/shareWith and !isDetachable()
      arms unchanged. REJECTED: refcounted holder (r13 — restructures
      ArrayBufferContents/m_destructor ownership for every embedder
      ctor, Bun external buffers); handle-MOVE + quarantined-handle
      ref (r14 — reintroduces live-transferee aliasing over a
      quarantine-visible mapping). Perf delta: detachable non-shared
      transfer goes O(1)->O(n) GIL-off only; recorded, accepted v1.
   3. **SHRINK** (ArrayBuffer::resize downward, ArrayBuffer.cpp:
      628-639): under memoryHandle->lock() compute desiredSize as
      landed; publish m_sizeInBytes = newByteLength (seq_cst) but DO
      NOT call freePhysicalBytes/OSAllocator::protect on the resizing
      thread. The tail range [desiredSize, previous handle size) is
      appended to the SAME quarantine list as a page-range entry
      {memoryHandle ref, offset, size}; retirement at the next heap
      §10 stop performs the protect + freePhysicalBytes (+
      memoryHandle->updateSize) under quiescence. Re-grow before the
      stop consumes/cancels overlapping pending tail entries under
      memoryHandle->lock() (pages still committed => zeroFill as
      landed). The wasm isWasmMemory() delta<0 rejection (:574-577)
      stands. VA is already reserved to maxByteLength
      (tryAllocateResizableMemory, ArrayBuffer.cpp:108-141), so
      deferral costs only physical-page residency until the stop.
   4. **GROW** (memory.grow + resizable AB upward): the base is
      IMMUTABLE GIL-off — in-place ONLY via reserved VA (wasm
      Signaling memories; shared memories' ceiling reservation;
      resizable ABs' maxByteLength reservation): commit the new
      pages, THEN release-publish the larger length — both torn pairs
      index the one immutable mapping. Where no reservation exists
      (BoundsChecking memories without VA), a gilOff grow that must
      relocate runs under a heap §10 stop (mutators quiesced), and
      the old mapping is still quarantined to the NEXT stop for
      captured/hoisted bases in jettisoning code.

   Torn-pair table (reader = any tier TA/DataView fast path):
   - detach: {oldLen, oldBase} stale-but-safe (quarantine mapped);
     {0, *} bounds-fails. transfer: identical (source = detach);
     transferee rows vacuous (unpublished during mutation);
     transfer-of-resizable source rows = detach rows.
   - shrink: {oldLen, base} stale-but-safe (tail still committed);
     {newLen, base} in-bounds.
   - grow in-place: both pairs in-bounds. grow relocate:
     stop-separated, no concurrent reader.

   Wasm-backed detach/grow = these arms (§I refuses spawned wasm
   EXECUTION only; shared views over a main-created
   WebAssembly.Memory reach spawned threads as plain TA accesses).
   Quarantine sizing: entries are byte-accounted against heap extra
   memory so a detach/shrink storm pulls the next collection forward.
   U28 amplifier: spawned TA readers vs main running a {memory.grow,
   detach, transferTo, structuredClone-with-transfer, resize-shrink,
   re-grow-after-shrink} storm; the transferee-GC'd-before-stop arm;
   transfer() of a RESIZABLE buffer under reader storm + post-transfer
   resize/grow of the transferee up to maxByteLength;
   transfer(newByteLength > byteLength). IM rows: ArrayBuffer.{h,cpp}
   (resize/transferTo/detach), JSArrayBufferView + per-tier TA fast
   paths (length-load sites), JSArrayBufferPrototype.cpp
   arrayBufferCopyAndDetach (read-side anchor); U-T13 owner.
7. **Audit U-T8c EXECUTED** -> SPEC-ungil-audit-N7.md (BINDING annex
   N7; rows R1-R31; reproduced below). Implementation CONSUMES the
   N7 table verbatim (§IM: IU adds call sites); tier-inlined accesses
   disabled or re-pointed per row. Residue rulings: §N.9. U28 arms
   per annex N7.
8. **ScriptExecutable -> CodeBlock FIRST install (ANNEX CBI, BINDING
   — full text; r20 F4):**
   1. Compile fully OUTSIDE any cell lock (landed shape preserved):
      each racer may link its own CodeBlock from the
      UnlinkedCodeBlock (the unlinked/bytecode side is immutable
      post-generation; UnlinkedCodeBlock generation itself is a
      §K.3-class lazy publication on the executable — CAS-claimed,
      foreign waiters park per §K.3 incl. its LZ2 lock-context
      precondition).
   2. Publication: release-CAS of the executable's
      m_codeBlockFor{Call,Construct} slot (single pointer word). The
      loser DISCARDS its CodeBlock (unreachable => GC-collected; no
      installCode side effects before winning) and ADOPTS the winner
      via load-acquire re-read. installCode's executable-side writes
      happen only on the winner.
   3. Adjacent multi-word state: m_jitCodeFor{Call,Construct} +
      arity/numParameters mirrors are published by the SAME winner
      AFTER the CAS, each as single-word release stores ordered
      before a final "installed" flag the fast path acquires (or:
      all derived loads go through the codeBlock pointer —
      address-dependent, jit F2). Per-field table consumed with N7;
      any field not single-word-publishable is ruled under the
      executable's JSCellLock (10a, OM I20 shape).
   4. Dedup (optional, perf-only): a per-executable in-flight claim
      CAS in the jit §5.7.2 m_tierUpInFlight pattern; losers either
      compile anyway (item 2 arbitrates) or K.3-park; correctness
      never depends on it.
   5. CodeBlockSet registration: under its existing heap-side lock
      (heap-owned; any thread). Debugger CodeBlock-wide walks =
      §A.2.7 (under a §A.3 stop); jettison = jit §5.3 — both already
      exclude racing installers via the stop (an installer
      parks/releases first).
   6. Tier-up (existing CodeBlock) stays jit §5.7.2 verbatim; this
      annex governs FIRST install only. No frozen text superseded
      (jit is silent on first install; INTEGRATE-jit.md:295-304's
      main-thread-install note is an FTL-finalization fact,
      unchanged: optimizing-tier installs still occur on the owning
      mutator).
   7. N7 row R12 + first-call amplifier: two spawned threads
      first-call the same fn (LLInt-only and tiered variants);
      exactly one CodeBlock installed, loser adopts, no torn
      m_jitCodeFor* observation; TSAN clean.
9. **r26 audit-residue rulings (ANNEX AUD1, §N side — full text):**
   - **AUD1.N1 (N7-U1) AbstractModuleRecord::m_resolutionCache:** §N
     default cell lock — tryGetCachedResolution/cacheResolution take
     the record's JSCellLock (the SAME lock as the sibling maps,
     AbstractModuleRecord.cpp:1465/:1561), §E.1b alloc-outside shape
     (the HashMap add runs under the lock; resolution computation
     stays outside). No tier-inlined access (namespace loads IC on
     the namespace object). Fixes a GIL-off HashMap-rehash UAF (OM
     annex 15.7 class). **PRIORITY — memory-unsafe today.**
     Amplifier: 2-thread shared-namespace property storm (U28).
   - **AUD1.N2 (N7-U2) RegExp::m_ovector:** the per-match output
     scratch moves OFF the shared cell GIL-off — matchInline
     (RegExpInlines.h) writes into the CURRENT lite's regexp match
     buffer (the §A.1.3 Group-3 "lazy regexp stack/match buffers"
     member, annex K4 table I), sized per match; ovectorSpan()
     consumers receive the lite buffer span. The RegExp cell retains
     compile-state only, already cell-locked in-tree (N7 R13).
     DFG/FTL RegExpExec/Match thunks land in matchInline and inherit
     the re-point; no inline JIT reads m_ovector directly. GIL-on
     keeps the cell vector. Fixes a racing-resize realloc UAF + torn
     capture reads. **PRIORITY.** Amplifier: 2-thread exec() on one
     shared RegExp (U28).
   - **AUD1.N3 (N7-U3/U4) arguments family publication:**
     DirectArguments m_mappedArguments + GenericArgumentsImpl
     m_modifiedArgumentsDescriptor: CAS-PUBLISH — allocate + fill the
     bitmap/storage COMPLETELY, then ONE release-CAS of the pointer
     word; losers discard (GC-collected); foreign readers
     load-acquire (the tier-inlined null-check is an
     address-dependent load, jit F2 — stays inline). ScopedArguments
     m_overrodeThings: release-store AFTER the length/callee/caller
     OM puts; foreign slow-path readers acquire. ClonedArguments
     m_callee clear (the materialized flag): release-store AFTER
     materializeSpecials' OM puts; readers acquire on the slow path
     (no lost callee/length — THREAD.md "no lost properties"). The
     property-materialization halves follow OM property rules
     unchanged. Amplifier: foreign reader vs owner override (U28).
   - **AUD1.N4 (N7-U5) StructureRareData runtime caches:** all cache
     INSTALLS (cachedPropertyNameEnumerator + watchpoint vector +
     flag word; m_cachedPropertyNames slots; special-property caches)
     run under Structure::m_lock (the structure owns its rare data;
     OM GT lock order). Each JIT-read word
     (m_cachedPropertyNameEnumeratorAndFlag,
     m_cachedPropertyNames[i]) is published by a SINGLE release
     store, LAST — the watchpoint FixedVector is fully constructed
     before the flag word publishes and is immutable thereafter;
     baseline/DFG readers consume one word. m_specialPropertyCache
     pointer = §K.3 lazy-publish; its interior fill precedes
     publication. Watchpoint FIRING stays jit-spec/§K.5 territory
     (K4.VI.2). OM annex 15 gains a pointer row to this ruling
     (doc-only). Amplifier: 2-thread for-in over one shared structure
     (U28).
   - **AUD1.N5 (N7-U6) Intl cell family:** every member mutated
     post-construction (IntlNumberFormat::m_numberingSystem and peer
     lazy Strings; IntlSegmentIterator's UBreakIterator advance;
     IntlLocale lazy fields) is accessed under the owning cell's
     JSCellLock; lazy Strings computed OUTSIDE the lock, published
     under it (two-word String => lock, not CAS).
     Construction-frozen ICU handles (UCollator, UNumberFormatter,
     ...) may be used concurrently WITHOUT the lock ONLY where the
     call site is verified against ICU's const/thread-safe contract
     (checklist consumed at implementation time per cell class);
     unverified sites clone-per-use under the cell lock
     (ucol_safeClone class) or take the lock for the call. No
     foreign-thread TypeError, no SD. All host-call paths; no
     tier-inlined access.

---

## LK. Merged process lock table

ONE order; heap §6 master; api §5.9 anchored here; vmstate §7
amended; acyclicity: r8 tree-walk as amended by WS1 + NLH1.

Outermost -> innermost:
1. heap rank 1: JSLock::m_lock / entry token / heap access (tokens
   ordering-inert; "held entering NVS" per thread).
2. api 1: TM::m_lock.
3. api 2: PWT::m_lock / ThreadAffinityTable (never both).
4. api 3 group (mutually unnested, api 5.9(d)): NCS::queueLock,
   NLS::m_queueLock, listLock, TS::inboxLock (§E.1), TS::joinLock.
   DISJOINT from heap rank 3 (VMManager::m_worldLock).
4b. **§A.3.3 pending-job-slot mutex (HBT4.4 as AMENDED by ANNEX NLH1,
   BINDING):** §A.3 conductors ONLY; inner to rank 1/token, OUTER to
   heap rank 2 (GCL); held across the stop window; losers park on it
   access-released; never held with any api **RANK-1..3** lock.
   **NLH1 (full text; r24 F1):** in LK.4b and HBT4.4, "api lock"
   means api RANKS 1-3 — it explicitly EXCLUDES the long-hold
   NLS::m_lock class. The slot mutex MAY be taken while NLS::m_lock
   is already held: lock.hold(fn) runs arbitrary user JS that can
   reach mandatory-synchronous conductors (any Class-A watchpoint
   fire — jit §5.6 routes ALL such fires through STWR with
   synchronous completion; haveABadTime, JS-reachable :2460; OM
   per-event stops, om §4.6/4.7/F3). New recorded edge: NLS::m_lock
   (long-hold) > job-slot mutex > GCL. Soundness: every NLS::m_lock
   WAITER blocks token+access-released (§E.2 rank-4 exemption), so
   the conductor's §A.3.2 barrier and the heap §10.4 barrier are
   INDEPENDENT of the NLS holder; and no conductor or heap 2..9b
   holder ACQUIRES NLS::m_lock — a hold(fn) conductor (Class-A fire,
   §K.5, OM stops) may HOLD NLS on entry, never ACQUIRES it; the
   edge is one-directional, the merged order stays acyclic. jit
   §5.6's caller precondition ("NO section-7/cell lock") is
   adjudicated here, frozen text unedited: it does not — and need
   not — exclude held long-hold NLS::m_lock. Tests: U-T11/U-T13
   amplifier — Class-A fire AND haveABadTime triggered from inside
   lock.hold(fn) with a second thread contending the lock and a
   third parked; stop completes, lock lint passes. The U20
   slot-mutex lint WHITELISTS held-NLS and FLAGS held api rank-1..3.
5. heap ranks 2-10b as frozen. Cross edges: api 3 -> 10a legal
   (§C.3); api locks NEVER wrap heap ranks 2-9b.
6. **VMLiteRegistry::lock — RE-RANKED outer-of-leaves** (SUPERSESSION
   vs vmstate §6.5.1/§7 "no lock while held", both sides): inner set
   {VMLite::scratchBufferLock, atomic bit ops, fastMalloc} ONLY;
   fastMalloc is EXCLUDED while a thread is suspended by the holder
   (§A.1.7 SUSPEND RULE). ScratchBufferRegistry sits OUTSIDE it
   (§A.1.6). The ~VM walk is collect-unregister-release THEN client
   work (no GBL-rank transition under the registry lock).
7. Leaves: HandleSet::m_strongLock (F.3); DWT::m_pendingLock (=
   m_taskLock, §E.7 note); §K class-2 cache locks;
   VMLite::scratchBufferLock.
8. **Destructor-leaf class** (SUPERSESSION vs heap §6 leaf row "never
   7-9b" + vmstate §7, both sides; IH rows): AtomString shards +
   SymbolRegistry::m_lock + HandleSet::m_strongLock acquirable UNDER
   MSPL/BVL/9b — in-lock sweep dtors reach them (~JSString -> last
   StringImpl deref -> removeDeadAtom / registered-symbol remove;
   r10 F1's ~AsyncTicket chain). Sound: holders are
   fastMalloc/list-splice-only, acquire nothing, never wait (vmstate
   I7 extended).
- **Long-hold:** NLS::m_lock is NOT a leaf (lock.hold runs user JS
  holding it; held across parks + token/access reacq, §E.2
  exemption) — ordered OUTSIDE heap 2-10 + api 1-3; acyclic: no
  conductor or heap-2..9b holder ACQUIRES it; §A.3 conductors MAY
  hold it on entry (NLH1.4). SUPERSESSION (r16 F6; api §5.9's rank-4
  leaf + (f) "Ranks not swapped", api:263/:272, vs this, both sides;
  IA row): GIL-on UNCHANGED — 5.9(e)/(f) ARE the leaf-form encoding
  of this order; §LK is the both-modes canonical form for U20's lint
  (r22 list).
- **Negative edges (normative):** no heap 2-9b holder acquires ANY
  api lock; no 10a/10b holder acquires api rank<=3; GC/§A.3
  conductors acquire NO api lock (interplay: §D.1's mutator-side TM
  snapshot/release + the WS(ii) finalize carve-out); api 1-3 holders
  never transition heap access (§E.2). Acyclicity (r8): every cross
  edge points inward; api->10a only via §C.3; verified by tree walk
  (no 10a holder takes listLock; notifiers order store-then-LL).
- **WS rows (ANNEX WS1, BINDING — full text; r22 W1 + r25 ext):**
  - **WS(i) PROHIBITION:** Weak handle CREATION (WeakSet::allocate;
    any Weak<T>/JSWeakValue construction reaches it) acquires MSPL
    (heap rank 7) whenever the server is shared (ISS;
    WeakSetInlines.h:66-73), and GIL-off implies ISS (U0). Therefore
    no thread may construct a Weak while holding ANY api rank-1..3
    lock or §LK.7 leaf (class-2 cache locks included). Strong
    creation (m_strongLock, fastMalloc HandleBlocks — no MSPL) is
    NOT prohibited, but the U-T8b column records its lock context
    anyway.
  - **WS1.2 CODE SHAPE (the SUPERSESSION vs api 5.7.2's landed
    shape; IU rows IU-WS1a ThreadManager.cpp, IU-WS1b
    RegExpCache.cpp + any class-2 weakAdd peer the audit finds):**
    ThreadManager::restrictObject (ThreadManager.cpp:259-280, its
    stale-replace arm calling makeAffinityEntry :234-243 under
    m_affinityLock — api rank 2): construct the ThreadAffinityEntry
    (with its Weak + finalizer context) BEFORE taking
    m_affinityLock; under the lock, ensure() into the table by
    MOVING the pre-built entry (fresh-insert arm) or REPLACING a
    stale entry with it (swap out the old entry under the lock,
    destroy it AFTER release); on the lose arm (live entry already
    present) destroy the pre-built entry after release. Entry
    destruction is only WeakSet::deallocate's lock-free clear
    (WeakSet.h:121-131) + fastMalloc free — legal in either
    position. The makeAffinityEntry comment's "created under the
    GIL" rationale is superseded. RegExpCache::lookupOrCreate
    (RegExpCache.cpp:62-65): construct Weak<RegExp> before the
    second Locker; weakAdd under it; a racing winner's duplicate
    Weak is discarded after release (lookup re-check under the lock
    decides). Pattern generalizes: NO Weak construction inside any
    api rank-1..3 or class-2/leaf section, ever; build outside,
    publish under. GIL-on behavior unchanged (same publication
    order; construction outside the lock is trivially legal under
    the GIL).
  - **WS(ii) CONDUCTOR CARVE-OUT** (amends the negative edge
    "conductors acquire NO api lock" + the r8 acyclicity derivation,
    both sides — vs ThreadManager.cpp:186-202 +
    RegExpCache.cpp:75-80): WeakHandleOwner::finalize bodies MAY
    acquire the ThreadAffinityTable lock (rank 2) and class-2 cache
    leaves in-window (conducted weak sweep, lastChanceToFinalize —
    pruneRestrictedObject :282-296 runs from
    ThreadAffinityWeakHandleOwner::finalize :192-202, i.e.
    WeakBlock::sweep; RegExpCache::finalize :75-80 likewise).
    Soundness, recorded: (a) holders of those locks are poll-free,
    access-retaining, never park and never wait (post-WS1.2 the
    sections are HashMap + fastMalloc only), so the heap §10.4/F8
    access barrier guarantees no thread is parked or stopped HOLDING
    one — the conductor always acquires in bounded time; (b) the
    reverse edge (MSPL -> these locks) no longer exists after WS1.2,
    and mutator in-lock sweeps skip weak-bearing blocks
    (WeakSet.h:121-131), so finalize bodies never run under a
    mutator's MSPL — acyclicity restored BY CONSTRUCTION. (c) The
    carve-out is CLOSED: exactly WeakHandleOwner::finalize-driven
    table pruning (pruneRestrictedObject, RegExpCache::finalize +
    audited peers); TM::m_lock (rank 1) is NOT excepted — §D.1's
    two-phase snapshot stands; any new finalize-side lock needs a
    new row.
  - WS1.4 AUDIT + LINT: U-T8b gains a handle-creation lock-context
    column (every Weak/Strong construction site records the locks
    held; Weak-under-api/leaf = WS1.1 violation, re-shape per
    WS1.2). U20 lints: (i) WeakSet::allocate reachable while an api
    rank-1..3 or §LK.7 lock is held (static path or debug-assert
    instrumentation); (ii) any api/leaf lock acquisition inside a
    WeakHandleOwner::finalize body not on the WS1.3 row list. Debug
    builds: a RELEASE_ASSERT hook in WeakSet::allocate checking a
    per-thread "in api-rank-1..3/leaf section" counter.
  - WS1.5 Corpus: restrict/collect churn (N threads Thread.restrict
    + dead-object storms forcing finalizer pruning during conducted
    sweeps); regexp-cache churn (distinct patterns, GC pressure) —
    both TSAN'd, gated with U-T8b.

§C.1 lock-free arms + §N.2 ropes take NO lock.
---

## INV. Invariants (rev-9 annex 1 + r10/r11 additions — full text;
IDs FROZEN)

- **U0** config gate matrix (§0).
- **U0b** GIL-off, exactly one VM per process (the
  sticky-shared-server VM) holds per-thread clients; other VMs
  spawn-refuse (RangeError) and keep the GIL-on embedder protocol;
  heap I13's assert never fires in supported configs (§0).
- **U0c** m_gilOff ctor assignment + eager sticky designation +
  immutability (§0; annex U0C).
- **U1** GIL-off JS thread: registered lite for the ENTERED VM,
  unique TID, live token, TLS tag == CURRENT lite TID && lite->vm ==
  entered VM && currentThreadClient() == lite->clientHeap (A36C
  extension); tid 0 never installed; multi-VM swap (§A.3.6/J.7).
- **U2** a VM-wide trap is observed by a parked T within one quantum,
  both GIL modes — carrier = §J.3's lock-free lite-bit poll, NOT
  token reacq; terminate-while-parked (§A.2). Re-scope (r10): the
  bound applies per-VM to threads whose park/current lite belongs to
  that VM; nested windows defer (U30).
- **U3** lifecycle order (§B.1-2/E.2; AMENDED r28, re-AMENDED r29
  per EXIT1.7, both sides; unchanged r30): lite -> ACT -> alloc;
  Strong clears -> access release -> TEARDOWN mark (registry lock)
  -> DCT -> destroy client -> unregisterLite/free lite (the free
  runs outside the registry lock, EXIT1.3/EXIT1.9).
- **U4** §A.3 stop: every entered thread parked/not-entered/
  access-released; entry during a stop parks; no access-released
  thread runs JS mid-stop (2b); wake-during-stop amplifier; + the
  SB1.6 litmus arm (conductor fan-out vs release-then-reacquire loop,
  arm64 hardware); + the EXIT1.8 exit-storm-under-stop-storm arm
  (r28).
- **U5/U6** §9.5 atomicity + D3/D7 in-body; CAS-storms all arms;
  dict-delete-vs-CAS; restricted AS; convert-first (incl. §C.3
  pre-enqueue conversion); the SW=0 AS pre-lock arm; §C.3 I10 arms.
- **U7** completion <=> fn returned && queues empty && keepalive==0,
  OR termination (§E.2/E.5).
- **U8/U9** keepalive: at-most-once decrement; no underflow
  (never-armed never decrements; mutual-asyncJoin-OPEN arm); no
  missed shutdown (§E.3).
- **U10** settles per §E.4 (registrant iff inboxOpen, else main);
  never a foreign microtask queue (I11).
- **U11** join/asyncJoin see Phase!=Running only post-close; join
  sees post-fn macrotask effects; the loop closes incl. deadline
  expiry (r12).
- **U12/U13** nested spawned JSLockHolder depth-counted; the APILock
  predicate is true on host-call paths GIL-off (§F.2).
- **U14** spawned DAL = access bracket, token/depth invariant,
  returns 0 (DAL2 re-derivation); embedder DAL excludes only
  embedders; embedder C test incl. §F.1 drain.
- **U15** §G policy; G11 TypeError preserved (api I18); spawned
  sync-wait OK; main parks release m_lock (§J.3 notifier arm); the
  lint extends to flag any park-quantum body taking locks other than
  the W1 full-exit sequence (r14).
- **U16** concurrent Symbol.for(one key) => one symbol.
- **U17** §I wasm throws from spawned threads, both modes, incl.
  warm-call; NEGATIVE: main/embedder NON-GC wasm never throws;
  POSITIVE: wasm-GC under useJSThreads => LinkError, no abort.
- **U18** rebias: no live dead-TID tag post-stop; restamp (from the
  §D.1 pre-stop snapshot) before reissue; spawn-storm past 2^15.
- **U19** the GIL-on fallback corpus is green after every U-task,
  unchanged EXCEPT SD6/SD7 (edited once).
- **U20** lint: inboxLock/joinLock never nested; leaf locks never
  across user JS; no token/access transition holding any api rank
  1-3 lock; rank-4 across transitions only per §E.2 (a)->(b);
  extended by: settle-under-rank-3 (r17), wake-under-rank-3 (r18),
  the job-slot mutex (r19; whitelists held-NLS, flags held api 1..3
  — NLH1.5), park-/access-transition-under-10a/10b +
  missing-generation-check (r20), the LZ2.5 dominated-touch rule
  (r21), WS1.4's two Weak lints (r22), non-seq_cst stop-bit access
  (r24 SB1.6), conductor lite/client pointers crossing a sample
  boundary + TEARDOWN-mark-precedes-DCT/client-destroy +
  unregisterLite-LAST + ~VM EXIT1.9-wait-precedes-teardown + the
  A36 deferred-dtor no-m_server check + EVERY physical registry
  removal an unregisterLite call (hand-rolled lites mutation
  flagged) + lite-state access under-registry-lock-only (r28
  EXIT1.8 as amended by r29-r31).
- **U21** bench (§B.5, incl. the r9 async/generator line).
- **U22** reactions on the settling thread; AsyncTicket on the
  REGISTERING thread (dead=>main); queues owner-only (§E.1b/I11).
- **U23** per-entry record correct under entry/exit churn; fan-out
  reaches every entered T of THIS VM (§A.1.5).
- **U24** DWT: post-settle ticket out of m_pendingTickets, Strong
  cleared; shell exits; hooks hookManaged-only; handoff wake; Bun
  dead-registrant settle; §E.7.5 pump/timer arms (§E); the DAL2.6
  DAL/GC/haveABadTime arm.
- **U25** inboxOpen once pre-fn, spawned only; increment sites assert
  spawned+open (§E.1).
- **U26** §K: concurrent String(0.5)/split/lazy first-touch — one
  init, no race; full GC during the winner's init (no deadlock); +
  the LZ1.4 arms (recursive-null; crossed cycle; terminated owner)
  and the LZ2.4 conductor-pre-resolve arm.
- **U27** ~VM walk: token-free carriers COLLECTED-marked +
  unregistered BEFORE the EXIT1.9 wait, server-side detached
  lock-free, DETACHED-flipped per client, deferred-destroyed via
  the state-keyed degenerate dtor (A36 as AMENDED r32; bit-clear
  lites only — a bit-SET, registration-fixed ownerHasNoTlsDtor
  lite is walk-freed post-flip and never dtor-visited);
  epoch-stale TLS (both maps — carrier + heap §10A.1 client slot)
  never consulted live; teardown storm; the r31
  CARRIER-TLS-DEATH-DURING-DETACH arm + its r32 WALK-FREE
  variant (walk-side disposition racing a late-firing TLS dtor);
  spawned-conductor GC scans an entered embedder's stack
  (§A.3.6/§F.1); the A36C two-VM/nested arms; the TERM1.5
  spawned-foreign-VM death test.
- **U28** §N: no UAF/torn builtin internal state; map.set + Date
  storms; rope race; generator double-resume (CAS claim) incl. the
  r15 arm64 ping-pong arm; detach/grow-vs-read incl. wasm memory (no
  UAF) + the annex-N6 storm list; + the AUD1/N7 arms (shared
  namespace, shared RegExp exec, arguments override, shared-structure
  for-in, regexp legacy statics SD19).
- **U29** §A.3.8: GC with >=2 threads entered in one VM — per-thread
  park/release; no per-VM double-transition/assert; per-thread
  willPark/didResume pairing.
- **U30** (§F.5): a thread nested in VM B holds VM A's token only in
  access-released state; A's conductors never wait on it; restore
  re-acquires gated and observes deferred bits before running A JS.
- **U31** (§J.3): every main/embedder park-quantum poll reads the
  captured park lite of the VM it entered; no poll reads
  VMLite::current() after the park release.
- **U32** (§A.3/§B.2, ANNEX EXIT1.7 as AMENDED by r31): no VMLite or
  GCClient::Heap is destroyed or freed while observable to any §A.3
  fan-out or predicate-sample registry walk as a live (non-TEARDOWN)
  lite — the TEARDOWN mark precedes DCT/client-destroy, physical
  removal comes LAST, and conductors hold no lite/client pointer
  across sample boundaries; no lite leaves the registry before its
  server-touching teardown tail completes; and ~VM BLOCKS (EXIT1.9)
  until no registered lite other than its m_mainVMLite has lite->vm
  == this — the NORMATIVE completion fence (the assert walk is a
  post-wait debug sanity check).

---

## SD. Semantic deltas vs phase 1 (corpus impact; IDs FROZEN — full
text, rev-9 annex 2 + r11/r13/r14/r16/r18/r24/r26 additions)

- **SD1** join settles at close (queues empty + keepalive 0), not
  fn-return (§E).
- **SD2** completion drains OWN queues till empty (GPO).
- **SD3** tickets settle on the REGISTERING thread, dead=>main
  (§E.4).
- **SD4** spawned TA sync wait allowed GIL-OFF ONLY (was TypeError;
  gate kept GIL-on); tests per-variant (§C.4/§G).
- **SD5** notify() no yield point; parallel waiters (§C.5).
- **SD6** main TA single-flight lifted (was second-wait throw, D8);
  per-wait nodes, D9 quanta, **both GIL modes** (§C.6/§A.2.6;
  flag-off untouched); GIL-on corpus edited (incl. the
  terminate-parked arm = VM-level termination, TERM1).
- **SD7** wasm on spawned threads: TypeError **both modes** (§I);
  GIL-on corpus edited.
- **SD8** terminate parked: Failed completion, residue to main
  (§E.5); **r16 F5 ext:** pending finite-timeout property
  Atomics.waitAsync registrations settle "timed-out" at close;
  **r27 ext2 (TERM1.3):** Failed carries a FRESH ordinary
  Error("Thread terminated"), never m_terminationException — join
  rethrows it normally, asyncJoin rejects with it.
- **SD9** TID exhaustion RangeErrors till next rebias (§D.1).
- **SD10** ordinary-promise reactions on the SETTLING thread
  (§E.1b); ALS preserved per ANNEX ALS1 (capture per-reaction,
  cursor per-lite).
- **SD11** spawned TA waitAsync settles main-side, no keepalive
  (§E.3).
- **SD12** asyncJoin: no keepalive; the registrant may close first;
  dead=>main; mutual/self never deadlocks (§E.3).
- **SD13** spawned breakpoints no-op (GIL-off only; §A.2.7).
- **SD14** watchdog (annex W; §A.2.8): CPU budgets + entry
  accounting measure main/embedder carriers only; spawned CPU never
  advances the budget; spawned entry/exit toggles neither
  carrier-entered state nor the timer. Wall-clock deadlines REMAIN
  armed while spawned lites are registered, even after the last
  carrier exits (W2), and are enforced via a parked carrier's early
  service episode (W1 — embedder callback consulted as landed) or,
  with no carrier entered-or-parked, on the timer thread WITHOUT the
  embedder callback (W3 — terminate-by-default; embedders needing
  extension semantics keep a carrier entered or parked). Spawned
  threads are terminated by the VM-wide fan-out in all shapes.
  GIL-on/flag-off unchanged.
- **SD15** rejection-tracker carrier-queued (§E.1b.4): spawned
  Reject/Handle events run at carrier drains; ordering vs
  carrier-side events unspecified; never lost while the carrier
  drains.
- **SD16** finite-timeout property waitAsync on a spawned TS settles
  "timed-out" only at registrant drain/close (§E.7.5); a registrant
  parked forever inside fn never settles it.
- **SD17** termination drops the settler's undrained per-lite
  microtask residue at close (§E.5); published settled state stays
  visible cross-thread.
- **SD18** sampling-profiler main-thread-only capture (§K.6/
  AUD1.K1).
- **SD19** per-thread RegExp legacy statics — RegExp.$1-$9/
  lastMatch/leftContext/rightContext/input observe only the CURRENT
  thread's matches (§K.6/AUD1.K2).

All GIL-off only EXCEPT SD6/SD7. The U19 fallback corpus keeps OLD
expectations via //@ runThreadsGILOff/GILOn variants for
SD1-SD5/SD8-SD19; SD6/SD7 GIL-on expectations change (edited once).
NOT SDs: §N.5's already-running TypeError; §I's wasm-GC LinkError
(was an abort); main thr.id (stays 0 per the A.3.6 TID
supersessions); §K.3 foreign parking; TERM1.5's RELEASE_ASSERT.
Per-rev SD attribution: r9 SD1-12; r11 SD13; r13/r14 SD14; r16
SD15 + SD8 ext; r18 SD16; r24 SD17; r26 SD18+SD19; r27/r28 none (ext2
rides SD8).

---

## IM. Integration manifest (rev-7 relocated table + rev-8/10/11/12/
13/14 add-lists — full text)

IU = INTEGRATE-ungil.md. **IU does NOT exist yet: U-T1 CREATES it
(TERM1.6).** IU is the landing ledger, schema per the INTEGRATE-*
house pattern, and MUST contain at least: (i) the supersession
ledger — one row per SPEC-ungil SUPERSESSION, both sides (spec side
already written; IU side written at landing); (ii) the §F.2
predicate-consumer table (U-T8, ~60 rows: assert/BRANCH/EXCLUSIVITY
CONSUMER); (iii) the §E.4 settle-site lock-context table (U-T8);
(iv) the §A.1.7 off-thread-reader table (U-T8d, per rerouted field);
(v) the §E.1b.4 hook-disposition table (U-T8e); (vi) the §F.6
embedder checklist incl. the (d) construction-order and (e)
spawned-no-foreign-VM audits; (vii) the per-row call-site
enumerations that annex K4/N7 rows defer to IU. Until U-T1, every
"IU row" citation is an OBLIGATION on the landing task; the audits'
"implementation CONSUMES the table verbatim" refers to the EXECUTED
K4/N7 tables — IU adds call-site enumeration only and NEVER re-rules
a K4/N7/TERM1 disposition.

Hot-file -> section table (bare names = runtime/):

- JSLock.{h,cpp} = §F, §A.3.6-7, §B.3, §J.3 (IA D1/D11; IV); also
  lock()/unlock() -> §F.5 nested release/restore + per-lite gilOff
  install (IV/IH); didAcquireLock/willReleaseLock -> §B.3
  supersession (U-T6, IH row vs heap §10A:281).
- ThreadObject.cpp + ThreadManager.{h,cpp} = §B.1-2, §E.1-E.4, §D.1
  (IA D5); ThreadManager.cpp:57 -> §F.3/§LK.8 sweep arm (IA);
  restrictObject/makeAffinityEntry -> WS1.2 (IU-WS1a).
- DeferredWorkTimer.{h,cpp} = §E.4/§E.7 (IA D5).
- LockObject/ConditionObject = §J.3-5, §C.5, C.7/D12 (IA
  D2/D9/D12); NativeLockState pump (api 5.5a P) + 5.6 timer ->
  §E.7.5 registrant routing (IA).
- ThreadAtomics.cpp + AtomicsObject.cpp = §C.2-6 (:613-621
  gilOff-conditional), §G (IA D3/D4/D7/D8).
- JSPromise.* + JSGlobalObject = §E.1/E.1b, §K.
- JSMap/JSSet/WeakMapImpl + JSString/JSRopeString + DateInstance +
  FunctionRareData + JSGenerator* + ArrayBuffer* = §N.
- bytecode/JSThreadsSafepoint.cpp = §A.3 stub swap + HBT4.5 bracket
  reorder + :208-221 comment rewrite, §J.8 (IJ, IO).
- VMEntryScope.{h,cpp} = §A.1.5, §A.3.4 (IJ M7; IV).
- VM.{h,cpp} + NumericStrings/LazyProperty* = §A.1, §E.1, §F.2, §K
  (IV M4-M6); VM ctor (m_gilOff) -> §0 U0c (IH/IV).
- VMLite.* + VMTraps.* = §A.1-2, §B.4, §I isSpawned — L2 appends
  only (IV); park sites (join, cond.wait, TA/property wait) -> §J.3
  captured-lite poll + §A.2.4 re-point (IV/IA).
- WaiterListManager.{h,cpp} = §E.3, §C.6 (IA D4); + the r12
  waitDeadlines timeout path (IA).
- ConcurrentButterfly.h + Structure* = §D.1 (IO).
- VMManager.{h,cpp} = §A.3.1-4 (IJ R1; IH); + heap §13.5a-g hooks =
  §A.3.8 per-thread GC parking (IH; IJ R1 cross-ref).
- heap/Heap.* + HandleSet.* = §A.1.3 root walk (:3585-class sites),
  §A.3.2b, §D.1, §F.3 (IH); noteSharedServerSticky/§10D arm
  (:4106-4124, :4755) + HeapClientSet.cpp:69 +
  tryDesignateStickySharedServer (NEW) -> §0 U0c (IH/IV).
- llint/jit/dfg/ftl (+OSR-entry) = §A.1 incl. non-baked, §B.4, §I
  isSpawned check (IJ; gilOff); LLInt Group-3 sites +
  atomicsWaitImpl -> the flag-off delta supersession rows (jit
  I1/vmstate R3/api I1; IJ/IV/IA); JSCConfig.h:106 -> gilOffProcess
  + per-lite byte (IJ).
- builtins/GeneratorPrototype.js + AsyncFunctionPrototype.js +
  AsyncGeneratorPrototype.js + iterator-helper builtins +
  BytecodeIntrinsicRegistry.{h,cpp} + BytecodeList.rb + LLInt/
  Baseline slow-path ops + DFG/FTL nodes
  AtomicInternalFieldClaim/Publish (DFGNodeType/clobberize/
  SpeculativeJIT/FTLLowerDFGToB3) -> §N.5 (flag-off delta (b2);
  golden re-baseline; r12 claim-failure dispatch) (IJ).
- debugger/Debugger.{h,cpp} + inspector pause path -> §A.2.7 (SD13)
  (IV/IJ).
- Watchdog.{h,cpp} -> §A.2.8/annex W + W ext (U-T2; carrier depth +
  m_wallClockArmed split, timer-callback branch, W4 asserts).
- LazyProperty.h:117 machine + JSGlobalObject initLater/VM ensure*
  -> §K.3 (IV).
- ArrayBuffer.{h,cpp} (resize/transferTo/detach, detached flag,
  base retention) + JSArrayBufferView + wasm
  BufferMemoryHandle/BufferMemory.* + JSArrayBufferPrototype.cpp
  arrayBufferCopyAndDetach -> §N.6/annex N6 (IV/IH; U-T13).
- Identifier.cpp + Completion.cpp + Heap.cpp:2796 atom-assert sites
  = §A.3.7 supersession (IV).
- HandleSet/ScratchBufferRegistry/VMLiteRegistry rank rows = §LK
  merged table (IV, IH).
- SamplingProfiler.{h,cpp}, VMInspector.cpp = §A.1.7/AUD1.K1 (IV).
- WTF SymbolRegistry.* = §H.
- wasm/js/* = §I (SD7); JSWebAssemblyInstance.cpp wasm-GC precheck.
- RegExpCache.cpp = WS1.2 (IU-WS1b).
- OptionsList.h = U0, §J.1, gilOff (all).
- INTEGRATE-api I21 annotation row -> §C.4 SUPERSESSION (IA).
---

# AUDIT TABLES (EXECUTED, BINDING)

The two §K.4/§N.7 inventory audits were EXECUTED against the tree
and frozen at spec rev 26. Implementation CONSUMES these tables
verbatim; IU adds call-site enumeration only and never re-rules a
row. They are reproduced here in full from
SPEC-ungil-audit-K4.md / SPEC-ungil-audit-N7.md (those files remain
the binding copies).

## SPEC-ungil Annex K4 (BINDING, audit executed)

Status: executed 2026-06-06 against the tree at branch jarred/threads.
This is the §K.4 inventory audit (U-T8b input; gates U-T9): every
VM-/JSGlobalObject-resident or process-global mutable member that N
concurrently-entered threads can reach and whose ONLY serializer today
is the GIL, ruled into the §K scheme. Implementation tasks consume the
tables verbatim; rows are addressed as K4.<table>.<row>.

Classification key (maps to SPEC-ungil §K classes):

| Class | Meaning | Spec authority |
|---|---|---|
| per-lite | duplicate into VMLite; GIL-off accessors route to CURRENT lite; cell-holding copies GC-scanned via registry walk | §K.1 |
| lock | shared hits required; leaf Lock per §LK.7 (or existing lock verified) | §K.2 |
| lazy-publish | first-touch CAS publication, then immutable (immutable-after-init with a publication protocol) | §K.3 / annex LZ1+LZ2 |
| immutable-after-init | written only during VM/global ctor or pre-thread embedder setup; GIL-off mutation asserted absent | §K (degenerate K.3: no protocol needed; add debug assert) |
| main-only | reachable only via debugger/inspector/profiling/test options; GIL-off restricted to the designated thread or no-op'd (SD13/SD14 pattern) | §A.2.7-8, SD13/SD14 |
| requires-stop | class 4: writer iterates/rewrites other threads' state; §A.3 stop conductor | §K.5 |
| already-safe | existing lock/atomic/stop-window discipline verified sufficient | — |

---

## 0. Residue dispositions — ALL RESOLVED at spec rev 26

Former UNRESOLVED items U1-U7. U1-U4 carry spec rulings (SPEC-ungil
§K.6, FULL text history ANNEX AUD1); U5-U7 were purely MECHANICAL and
are reclassified here with rationale. No row blocks U-T9.

- **U1 — SamplingProfiler — RESOLVED (AUD1.K1, SD18).** GIL-off the
  profiler samples ONLY the main/carrier thread (§A.1.7 form (i),
  SUSPEND RULE applies); spawned frames never captured; start/stop/
  report main-only (SD13/SD14 family). Internals keep `m_lock` (:218).
  N-thread capture is post-ungil. Row V.3 re-ruled main-only.
- **U2 — m_regExpGlobalData — RESOLVED (AUD1.K2, SD19).** Per-lite
  (§K.1): each thread owns a private RegExpGlobalData stream;
  RegExp.$1-$9/lastMatch etc are per-thread GIL-off. Tier-inlined
  RecordRegExpCachedResult re-pointed via the lite (AUD1.K4 A16 ext).
  Joins the registry-walk root set + ~VM walk. Regexp corpus arm
  UNBLOCKED (SD19 GILOff variants).
- **U3 — module evaluation state — RESOLVED (AUD1.K3).**
  m_moduleAsyncEvaluationCount = atomic fetch_add, relaxed (row
  III.16 order question discharged: uniqueness+monotonicity suffice).
  m_synchronousModuleQueue = per-lite (§E.1 family) — row V.18
  RE-RULED per-lite (NOT main-only). Cross-thread Evaluate() of one
  record = status claim under the record's cell lock (N7 R16);
  losers adopt the top-level promise (async) or park-capable wait
  access-released (sync).
- **U4 — JIT-baked per-lite cache addresses — RESOLVED (AUD1.K4).**
  ANNEX A16 EXTENDED to MegamorphicCache, HasOwnPropertyCache,
  m_regExpGlobalData and m_weakRandom (VIII.10): gilOff-mode
  compilation indexes the lite-resident copy via loadVMLite;
  flag-off keeps baked addresses. Rows II.18/II.19 JIT arms
  discharged; no locked fallback needed (caches private per lite).
- **U5 — DWT lock-name drift — MECHANICAL.** Spec §E.7.1's
  `m_pendingLock` IS the in-tree `m_taskLock`
  (`DeferredWorkTimer.h:116`) — name equation now noted in spec
  §E.7/§LK.7. Its coverage EXTENDS to `m_pendingTickets` (:121);
  the :125-126 three-condition comment loses the GIL leg. One §LK.7
  leaf lock; row VII.4 final.
- **U6 — m_canFastQueueMicrotask / m_associatedContextIsFullyActive —
  MECHANICAL (SD13 umbrella).** Writes main-only (debugger/context
  attach); reads relaxed-atomic from any entered thread. A stale-true
  window at most skips debugger microtask observation for in-flight
  enqueues = SD13-class degradation; no new SD. Classified main-only
  + relaxed-atomic reads.
- **U7 — SmallStrings lazy tail — MECHANICAL (verification PASSED).**
  `initializeCommonStrings` runs in the VM ctor (VM.cpp:335); the
  `!m_isInitialized` fallback (`SmallStrings.cpp:121-127`) allocates
  a fresh AtomStringImpl and writes NO member; `setIsInitialized
  (false)` is teardown-only (VM.cpp:707). immutable-after-init
  CONFIRMED unconditionally; row VIII.6 condition discharged.

Everything below this line is RULED.

---

## I. VM execution state already ruled elsewhere (listed for closure; no new ruling)

| Member (file:line) | Class | Rationale / authority |
|---|---|---|
| Group-3 set: topCallFrame, exception/unwind words, stack limits (VMLitePrimitives X-macro block, `VM.h` top; comment :1263-1265) | per-lite | §A.1.3 Group-3 storage; vmstate L1-L5 ABI |
| `m_exception`/`m_lastException`/`m_needExceptionCheck`/`m_throwingThread` (`VM.h:1273-1276`, setException comment :1228) | per-lite | vmstate I15 + §A.1.3; throw state is thread-local |
| `entryScope` (`VM.h:939`), `isEntered()` (:298) | per-lite | §A.1.5 per-entry record; moved into lite |
| `m_entryScopeServicesRawBits` (`VM.h:444`) | per-lite | §A.1.5 service routing: VM-wide word + registry fan-out |
| `m_executingRegExp` (`VM.h:911`) + regexp match/stack buffers, `m_regExpAllocator`/`m_regExpAllocatorLock` (`VM.h:951-952`) | per-lite | §A.1.3 Group-3 explicitly lists lazy regexp stack/match buffers; allocator becomes per-lite, lock retired GIL-off |
| Scratch buffers: `m_scratchBufferLock`/`m_scratchBuffers`/`m_sizeOfLastScratchBuffer` (`VM.h:1300-1302`), threading comment :914-916 | per-lite | annex A16 (BINDING): per-lite segmented tables |
| `m_checkpointSideState` (`VM.h:1303`) | per-lite | OSR side state is per-frame, per-thread (§A.1) |
| Microtasks: `m_defaultMicrotaskQueue` (`VM.h:1375`), `queueMicrotask` (:1026) | per-lite | §E / vmstate §6.6 per-lite queue; rerouted |
| `m_microtaskQueues` registry list (`VM.h:1280`) | lock | queue REGISTRATION list (not drain path); §LK.7 leaf; cold |
| `m_aboutToBeNotifiedRejectedPromises` (`VM.h:1321`) | per-lite | SD15 rejection-tracker carrier-queued (§E.1b.4) |
| Termination/traps: `m_terminationException` (:399), `m_hasTerminationRequest` (:1363), `m_executionForbidden{,OnTermination}` (:1364-1365) | already-safe | §A.2 VMTraps fan-out + SD8/§E.5; termination exception = sticky release-publish |
| `m_apiLock` (`VM.h:465`) | already-safe | IS the §LK rank-1 lock; GIL-off protocol per §F |
| `m_syncWaiter` (`VM.h:1376`) | already-safe | WaiterListManager internally locked; §C waits ruled |
| `m_numberOfActiveJITPlans` (`VM.h:1378`) | already-safe | std::atomic, relaxed (:1169) |
| `m_mainVMLite` (`VM.h:1287`) | immutable-after-init | set once in ctor; lites managed by registry |
| `m_gilOff` byte (VM ctor, U0c) | immutable-after-init | U0c BINDING: set ONCE in ctor |

## II. VM string/number/per-op caches — class per-lite (§K.1)

All are hot, mutated on ordinary JS paths, value-cache semantics (a
miss is only a perf event), and hold GC cells => per-lite copy,
GC-scanned via the registry walk (§A.1.3 GC-roots rule).

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.II.1 | `numericStrings` (`VM.h:657`) | named in §K.1; number->string per-op cache |
| K4.II.2 | `lastCachedString` (`VM.h:659`) | single-entry rope/string cache, torn cross-thread reuse unsound |
| K4.II.3 | `lastAtomizedIdentifierStringImpl` / `lastAtomizedIdentifierAtomStringImpl` (`VM.h:660-661`) | last-atomization memo; pure per-thread locality |
| K4.II.4 | `jsonAtomStringCache` (`VM.h:662`) | JSON.parse key atomization scratch |
| K4.II.5 | `keyAtomStringCache` (`VM.h:663`) | property-key atomization scratch |
| K4.II.6 | `stringSplitCache` (`VM.h:664`) + `stringSplitIndice` (`VM.h:665`) | named in §K.1 (as stringSplitIndice); split result cache + index scratch vector |
| K4.II.7 | `stringReplaceCache` (`VM.h:666`) | replace result cache, same shape as split |
| K4.II.8 | `m_cachedSortScratch` / `m_sortScratchSentinel` (`VM.h:637-638`) | Array.prototype.sort scratch butterfly; live across one sort only |
| K4.II.9 | BigInt divisor cache: `m_cachedBigIntDivisor`, `m_nextCachedBigIntDivisor`, `m_bigIntCachedInverse`, `m_bigIntDivisorCount` (`VM.h:680-683`) | multi-word cache mutated on BigInt modulo; not CAS-able as a unit |
| K4.II.10 | `stringRecursionCheckFirstObject` / `stringRecursionCheckVisitedObjects` (`VM.h:941-942`) | recursion guard tracks the CURRENT thread's JS stack; sharing is wrong, not just racy |
| K4.II.11 | `dateCache` (`VM.h:944`, class `runtime/JSDateMath.h:87`) | §N.3 ruling: "vm.dateCache = §K.1/2"; ruled K.1 (timezone/parse caches hot); timezone-change notification fans out via registry |
| K4.II.12 | `m_stringSearcherTables` (`VM.h:1311`) | adaptive search scratch tables; creation = lazy-publish (LazyUniqueRef), contents per-lite |
| K4.II.13 | `m_random` (`VM.h:469`) | WeakRandom state advanced on JS paths; per-lite stream (seeded per-lite) |
| K4.II.14 | `m_integrityRandom` (`VM.h:471`) | Integrity audit sampling on allocation paths |
| K4.II.15 | `m_mightBeExecutingTaintedCode` (`VM.h:547`) | execution-context flag of the CURRENT thread (Group-3-adjacent) |
| K4.II.16 | `m_deletePropertyMode` (`VM.h:1291`, scope RAII :746-758) | scoped mode around the current thread's host call |
| K4.II.17 | `m_doesGC` (`VM.h:1383`) | ASSERT_ENABLED-only expectation state; per-thread by meaning |
| K4.II.18 | `m_hasOwnPropertyCache` (`VM.h:956`) | entry = {structureID, impl, result} multi-word; interleaved writes can pair a key from A with a result from B => per-lite. Creation = K.3 (LazyUniqueRef). JIT path: A16 ext (§0 U4) |
| K4.II.19 | `m_megamorphicCache` (`VM.h:960`) | multi-word epoch'd entries (`MegamorphicCache.h:90`); torn entry can satisfy the wrong key => per-lite. Creation = K.3. JIT path: A16 ext (§0 U4) |

## III. VM shared keyed caches — class lock (§K.2 leaf, §LK.7)

Hits MUST be shared (dedup/memory) or mutation is cold. Leaf Lock;
weak-handle creation inside follows §LK WS1(i): hoist Weak
construction BEFORE the lock, publish under it.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.III.1 | `m_regExpCache` (`VM.h:950`; `RegExpCache.h:79` `m_lock`, `m_weakCache` :80, `m_strongCache` :81) | already-safe — the §K.2 exemplar; verify every mutator path takes :79 (audit found none missing) |
| K4.III.2 | `m_codeCache` (`VM.h:1293`; `CodeCache.h:242 m_sourceCode`) | eval/program/module unlinked-code dedup cache; sharing is the point; parse happens OUTSIDE the lock, publish under it (§N.8 CBI pattern) |
| K4.III.3 | `m_intlCache` (`VM.h:1294`; `IntlCache.h:62-64`) | ICU pattern-generator + canonicalized-locale maps; ICU objects not thread-safe => ALL use under the leaf lock (cold) |
| K4.III.4 | `symbolImplToSymbolMap` (`VM.h:724`) | WeakGCMap, no internal lock; Symbol identity REQUIRES sharing => lock + WS1(i) hoist |
| K4.III.5 | `atomStringToJSStringMap` (`VM.h:725`) | same; with useSharedAtomStringTable the jsString dedup must be shared => lock + WS1(i) |
| K4.III.6 | `wasmGCStructureMap` (`VM.h:727`) | RTT->Structure identity map; identity requires sharing; cold => lock |
| K4.III.7 | `sourceProviderCacheMap` (`VM.h:779`) | parser info cache keyed by SourceProvider; parse-time only => lock |
| K4.III.8 | `m_impurePropertyWatchpointSets` (`VM.h:1296`) | keyed WatchpointSet registry mutated via addImpureProperty (host API) => lock; FIRING any set = requires-stop (K4.VI) |
| K4.III.9 | `m_compactVariableMap` (`VM.h:954`; `parser/VariableEnvironment.h:516 m_map`, no lock) | TDZ environment interning shared across parses => leaf lock |
| K4.III.10 | `m_symbolTableCache` (JSGlobalObject, `JSGlobalObject.h:501`) | WeakGCMap of cloned SymbolTables; codegen-time => lock + WS1(i) |
| K4.III.11 | `m_loopHintExecutionCounts` + `m_loopHintExecutionCountLock` (`VM.h:1372-1373`) | already-safe — existing lock; keep as §LK.7 leaf |
| K4.III.12 | `jitStubs` JITThunks (`VM.h:781`; `jit/JITThunks.h:260` RecursiveLock `m_lock`) | already-safe — internally locked; recursion audited by jit spec |
| K4.III.13 | `m_sharedJITStubs` (`VM.h:784`; `bytecode/SharedJITStubSet.h:134-145`) | already-safe — every accessor takes its own m_lock (landed in thread-implement, R2-2) |
| K4.III.14 | `ftlThunks` (`VM.h:787`; `ftl/FTLThunks.h:98 m_lock`) | already-safe — internally locked |
| K4.III.15 | `m_drainMicrotaskDelayScopeCount` (`VM.h:1318`) | embedder API counter; make atomic (degenerate lock); not on JS fast path |
| K4.III.16 | `m_moduleAsyncEvaluationCount` (`VM.h:1332`, ++ at :1178) | atomic fetch_add, relaxed; order discharged per §0 U3 (AUD1.K3) |
| K4.III.17 | `machineCodeBytesPerBytecodeWordForBaselineJIT` (`VM.h:658`) | stats-only SimpleStats at JIT finalize; leaf lock (or per-lite merge); no correctness payload |
| K4.III.18 | JSGlobalObject `m_installedObjectPropertyChangeAdaptiveWatchpoints` (`JSGlobalObject.h:593`) | append-only Vector of installed watchpoints; install paths race GIL-off => lock; firing = K4.VI |

## IV. VM/JSGlobalObject lazy one-shot members — class lazy-publish (§K.3, annex LZ1/LZ2)

First-touch CAS publication per `runtime/LazyProperty.h` (tags :114-115,
slow path :95-97, isInitialized :91); owner-re-entry/abandonment per
LZ1; conductor precondition per LZ2.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.IV.1 | ALL `LazyClassStructure` members (`JSGlobalObject.h:263-270`, :302-303, :450-452, :461 macro, :493) | the §K.3 named case (initLater) |
| K4.IV.2 | ALL `LazyProperty<JSGlobalObject, T>` members (`JSGlobalObject.h:281-301`, :305-312, :318-328, :340, :342, :359-373, :432-449, :469-472, :489-494) | ditto |
| K4.IV.3 | `m_linkTimeConstants` (`JSGlobalObject.h:498`) | FixedVector of LazyProperty; per-slot §K.3 |
| K4.IV.4 | VM `ensure*` LazyRef/LazyUniqueRef CONTAINERS: `m_watchdog` (:1309), `m_heapProfiler` (:1310), `m_stringSearcherTables` (:1311), `m_shadowChicken` (:1316), `m_hasOwnPropertyCache` (:956), `m_megamorphicCache` (:960) | §K.3 names "VM ensure*"; CONTENTS ruled separately (rows II.12/II.18/II.19, V.1, V.2, V.6) |
| K4.IV.5 | `m_fastCanConstructBoundExecutable` / `m_slowCanConstructBoundExecutable` (`VM.h:640-641`) | lazily created NativeExecutables on bound-function paths; single-word release-CAS publish, loser discards |
| K4.IV.6 | `m_fastRemoteFunctionExecutable` / `m_slowRemoteFunctionExecutable` (`VM.h:643-644`) | same, but Weak<> => creation obeys WS1(i) (hoist MSPL work outside any K-lock) |
| K4.IV.7 | `m_emptyPropertyNameEnumerator` (`VM.h:606`) | created on first empty enumeration; single-word CAS publish |
| K4.IV.8 | `m_exceptionFuzzBuffer` (`VM.h:1308`, alloc :924-925) | fuzz option only; still gets the CAS-publish (cheap) — effectively main-only in practice |
| K4.IV.9 | JSGlobalObject `m_rareData` (`JSGlobalObject.h:524`; struct `JSGlobalObjectInlines.h:52-57`; createRareDataIfNeeded :475-479) | pointer = §K.3 CAS-publish; contents: `profileGroup` immutable-after-init (embedder), `opaqueJSClassData` = JSC C-API class data => leaf lock (C-API entry is rare/cold) |

## V. VM debugger/profiler/tooling members — class main-only (SD13/SD14 family)

GIL-off: feature restricted to the designated (main) thread or
disabled at option validation; spawned-thread interaction = no-op
(SD13 pattern). GIL-on unchanged. Cross-thread WALKS of other lites'
frames use §A.2.7/§A.3 stops.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.V.1 | `m_watchdog` Watchdog (`runtime/Watchdog.h:40`; `m_lock` :68 guards `m_vm` :69; `m_timeLimit`/`m_cpuDeadline`/`m_deadline` :71-73; callbacks :75-77) | already-safe + annex W: internals locked; SD14 + §K.4-routed per-thread CPU deadlines (spec :186); deadline reads on JS slow paths become per-lite mirrors per annex W |
| K4.V.2 | `m_heapProfiler` (`VM.h:1310`) + `m_activeHeapAnalyzer` (`VM.h:1292`, setter :283) | heap snapshot runs inside a heap §10 stop; analyzer pointer written only by the conducting thread |
| K4.V.3 | `m_samplingProfiler` (`VM.h:1313`; `SamplingProfiler.h:218-231`) | main-only capture per §0 U1 (AUD1.K1, SD18); internals keep m_lock |
| K4.V.4 | Debugger list `m_debuggers` (`VM.h:1386`) + Debugger object (`debugger/Debugger.h:54`; `m_vm` :323, `m_globalObjects` :324, parse/blackbox maps :325-326, pause bits :330-337, `m_pauseOnCallFrame`/`m_currentCallFrame` :341-342) | SD13: spawned breakpoints no-op; attach/detach + pause machinery main-thread; cross-thread frame walks = §A.2.7 stops |
| K4.V.5 | `m_isDebuggerHookInjected` (`VM.h:1366`, setter :1176) | sticky monotonic bool; main-only writer, relaxed reads |
| K4.V.6 | `m_shadowChicken` (`VM.h:1316`) | debugger-feature log written from prologues; GIL-off active only on main thread (SD13 umbrella); if ever revived N-thread it must be re-ruled per-lite |
| K4.V.7 | `m_typeProfiler` / `m_typeProfilerLog` / `m_typeProfilerEnabledCount` (`VM.h:1297-1299`) | option-gated; log written from inline JIT paths => GIL-off refused at option validation unless single-threaded |
| K4.V.8 | `m_controlFlowProfiler` / count (`VM.h:1306-1307`) + `m_functionHasExecutedCache` (`VM.h:1305`) | same gating as V.7 |
| K4.V.9 | `m_perBytecodeProfiler` (`VM.h:946`) | Profiler::Database, option-gated tooling |
| K4.V.10 | `m_fuzzerAgent` (`VM.h:1315`) | fuzzing option only |
| K4.V.11 | `m_rtTraceList` (`VM.h:974`, ENABLE(REGEXP_TRACING)) | debug build tooling |
| K4.V.12 | `m_failNextNewCodeBlock` (`VM.h:1288`, :870-875) | test hook |
| K4.V.13 | `m_shouldBuildPCToCodeOriginMapping` (`VM.h:1290`) | sticky bool set by profiler/debugger attach; main-only writer, relaxed reads by compiler threads |
| K4.V.14 | `m_debugState` Wasm::DebugState (`VM.h:1369`) | wasm debugger; main-only (VMManager wasm-debugger stops are §A.3 territory) |
| K4.V.15 | VMInspector (`tools/VMInspector.h:40`) | already-safe — stateless static facade (post-refactor: no instance list in the header; VM enumeration lives in VMManager, row VII.2); dump entry points are debugger/REPL main-only |
| K4.V.16 | JSGlobalObject `m_inspectorController` / `m_inspectorDebuggable` (`JSGlobalObject.h:512-513`) + `m_debugger` (`JSGlobalObject.h:237`) | inspector wiring; SD13 umbrella |
| K4.V.17 | JSGlobalObject `m_globalScopeExtension` (`JSGlobalObject.h:256`) | debugger/embedder scope injection; main-only writes |
| K4.V.18 | `m_synchronousModuleQueue` (`VM.h:1358`) | RE-RULED per-lite (§E.1 family) per §0 U3 (AUD1.K3); listed here for history only |

## VI. Requires-stop — class 4 (§K.5)

Writers that iterate/rewrite OTHER threads' reachable state. One §A.3
stop, conductor = caller, §A.3.5 CLASS-4 variant.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.VI.1 | `JSGlobalObject::haveABadTime` (`JSGlobalObject.cpp:2900`; JS-reachable :2460; jettison comment :2854; `m_havingABadTimeWatchpointSet` `JSGlobalObject.h:517`) | THE ruled class-4 case: history annex HBT+HBT2-4 verbatim |
| K4.VI.2 | Watchpoint-set FIRING, all of: `JSGlobalObject.h:516-521` const Ref sets, InlineWatchpointSets :531-572 (incl. `m_structureCacheClearedWatchpointSet` :564), adaptive watchpoints :576-589, VM `m_primitiveGigacageEnabled` (`VM.h:1304`), impure-property sets (row III.8) | invalidation jettisons/deoptimizes code other threads may be RUNNING => fire only inside a §A.3 stop (jit-spec deopt machinery); INSTALL/registration is lock-class (III.18); reads (hasBeenInvalidated, e.g. `JSGlobalObject.h:1192-1194`) are already-safe loads |
| K4.VI.3 | `VM::deleteAllCode` / `deleteAllLinkedCode` (`VM.h:998-999` decls) | rewrites every thread's executable state; route through the same §A.3 stop family (jit spec I2/R1) |
| K4.VI.4 | JSGlobalObject `m_structureCache` CLEAR path (`JSGlobalObject.h:500`; `StructureCache.h:60-61, 67-68`) | per-entry add/get under its existing `m_lock` (already-safe); but bulk clear coupled to :564's watchpoint => clear rides the VI.2 stop |

## VII. Process-global singletons

| Row | Member (file:line) | Class | Rationale |
|---|---|---|---|
| K4.VII.1 | Options / JSCConfig (`gilOffProcess` byte, JSCConfig.h:106 per spec §A.1.3) | immutable-after-init | Config::permanentlyFreeze before threads; U0 option-validation gate |
| K4.VII.2 | VMManager singleton (`runtime/VMManager.h:243`; world-stop state `m_worldMode`/`m_currentStopReason`/`m_pendingStopRequestBits` :142-186 commentary) | already-safe | heap rank 3 m_worldLock + §A.3 machinery; ruled by heap spec §6 |
| K4.VII.3 | AtomString table shards + SymbolRegistry (`VM.h:651-653` pointers; WTF shards) | already-safe | §LK.8 destructor-leaf class; requires useSharedAtomStringTable=1 (U0) |
| K4.VII.4 | DeferredWorkTimer (`VM.h:646`; `DeferredWorkTimer.h:116 m_taskLock`, `m_tasks` :120, `m_currentlyRunningTask` :119, `m_pendingTickets` :121 with the :125-126 safety comment) | lock | §LK.7 leaf (spec name DWT::m_pendingLock); m_taskLock IS the spec's m_pendingLock; m_tasks/m_currentlyRunningTask AND m_pendingTickets under it (§0 U5) |
| K4.VII.5 | `m_runLoop` (`VM.h:467`) | immutable-after-init | const Ref bound at ctor; RunLoop itself WTF-thread-safe for dispatch |
| K4.VII.6 | `m_heapRandom` (`VM.h:470`) | already-safe | advanced only by heap-side allocation paths already serialized by heap spec (GC server / stop windows); NOT touched on JS mutator fast paths |
| K4.VII.7 | `m_currentWeakRefVersion` (`VM.h:1330`) | already-safe | written only inside the GC stop window; mutator reads relaxed (epoch compare) |

## VIII. Immutable-after-init (assert-only; debug assert "no write after first cross-thread entry")

| Row | Members (file:line) | Rationale |
|---|---|---|
| K4.VIII.1 | VM structure roots `structureStructure` ... `bigIntStructure` (`VM.h:552-604`) | written once in VM ctor; thereafter GC-barrier reads only |
| K4.VIII.2 | Promise/native executable roots (`VM.h:607-621`) | ctor-initialized |
| K4.VIII.3 | Sentinels: `m_orderedHashTableDeletedValue`/`m_orderedHashTableSentinel` (:623-624), `m_sentinelStructure` + fast-iterator sentinels (:626-635) | ctor-initialized constants |
| K4.VIII.4 | `heapBigIntConstantOne`/`Zero` (`VM.h:676-677`) | ctor constants |
| K4.VIII.5 | `propertyNames` CommonIdentifiers (:654), `m_emptyList` (:655), `m_bytecodeIntrinsicRegistry` (:1317), `m_builtinExecutables` POINTER (:1295; per-slot fills are §K.3 single-word publishes, `BuiltinExecutables.h:76`) | built in ctor; builtin slots: release-CAS per slot |
| K4.VIII.6 | `smallStrings` (`VM.h:656`; `SmallStrings.h:69-127`) | ctor `initializeCommonStrings`; verification PASSED (§0 U7) — unconditional |
| K4.VIII.7 | `m_identifier` (:464), `m_typedArrayController` (:947), `clientData` (:548), `m_globalConstRedeclarationShouldThrow` (:1289) | set at VM/embedder init before any thread spawn; Bun sets clientData pre-threads |
| K4.VIII.8 | Embedder hooks: `m_onEachMicrotaskTick` (:1323), `m_onComputeErrorInfo{,JSValue}` (:1325-1326), `m_onAppendStackTrace` (:1327), `m_computeLineColumnWithSourcemap` (:1328), `m_didPopListeners` (:1380), `m_crossTaskToken` (:463) | Bun installs once at startup; setters (:1091-1094) get the no-write-after-entry assert; if Bun ever needs dynamic swap, re-rule to lock |
| K4.VIII.9 | JSGlobalObject: `m_vm` (:236), `m_globalThis` (:253), `m_globalLexicalEnvironment` POINTER (:255; var contents = OM territory), `m_name` (:503), `m_isAsyncContextTrackingEnabled` (:506), `m_evalEnabled`/`m_webAssemblyEnabled`/`m_needsSiteSpecificQuirks` (:672-674), disabled-error messages (:677-678; setters :1249-1255) | global-object configuration written by embedder before sharing the global across threads |
| K4.VIII.10 | JSGlobalObject `m_weakRandom` (`JSGlobalObject.h:526`) | **per-lite**, not immutable — listed here only to pin the decision: Math.random state advances per call; per-lite streams (independently seeded) are spec-compliant (no SD: outputs remain uniform); JIT inline fast path re-pointed via lite, same A16-style pattern as II rows |

## IX. Coverage statement

Sweep inputs: `runtime/VM.h` (all 1479 lines; every declared data
member is in a row above or in table I), `runtime/JSGlobalObject.h`
(all member blocks :236-678), `runtime/Watchdog.h`,
`debugger/Debugger.h`, `runtime/SamplingProfiler.h`,
`tools/VMInspector.h`, `runtime/DeferredWorkTimer.h`,
`runtime/RegExpCache.h`, `runtime/VMManager.h`, plus reached-into
helpers (`CodeCache.h`, `IntlCache.h`, `StructureCache.h`,
`JITThunks.h`, `SharedJITStubSet.h`, `FTLThunks.h`,
`RegExpGlobalData.h`, `SmallStrings.h`, `NumericStrings.h`,
`LazyProperty.h`, `JSGlobalObjectInlines.h`, `WeakGCMap.h`,
`VariableEnvironment.h`). Cell-INTERNAL state (JSMap/rope/etc.) is §N,
not this annex. Heap/GC-owned members (`VM::heap` and everything it
roots) are heap-spec territory and intentionally absent.

Binding consequences:

1. Every §F.2 EXCLUSIVITY CONSUMER must cite its row here (spec §K.4).
2. The seven §0 items are RESOLVED at spec rev 26 (§K.6 / history
   ANNEX AUD1); the U-T9 audit gate is SATISFIED on this annex's
   side. U-T8b consumes these tables verbatim.
3. Per-lite rows holding GC cells join the registry-walk root set
   (§A.1.3); the ~VM teardown walk (U-T8) must free all per-lite
   copies.
4. Immutable-after-init rows each get the debug
   assert-no-write-after-first-cross-thread-entry hook (one shared
   macro; U-T8b deliverable).

## SPEC-ungil Annex N7 (BINDING, audit executed)

Executes SPEC-ungil §N.7 (task U-T8c, gates U-T9, beside U-T8b).
Scope: every shareable JSCell subclass under
Source/JavaScriptCore/runtime/ carrying NON-PROPERTY multi-word
mutable state (C++ members / internal fields / aux allocations
mutated on JS-reachable paths after publication). Property slots,
butterflies, Structures, PropertyTable = OM spec by definition;
VM/JSGlobalObject members = §K/U-T8b (cross-refs only). Method:
header sweep of runtime/*.h for mutable members on cell classes +
cellLock()-user census (runtime/*.cpp) + cross-check against
SPEC-ungil §N.1-8, §E.1b, §K, §H, §I, OM §4.6/§9.5/I21/annex 15,
jit §5.7.2, annexes N6/CBI/E1B (history). All paths below relative
to Source/JavaScriptCore/.

Disposition vocabulary (§N.7): CELL-LOCK | CAS-PUBLISH |
RACY-TOLERATED | GIL-OFF TypeError | COVERED(section) |
PHASE-1-IN-TREE (already locked in this branch) | UNRESOLVED.

---

## Residue dispositions — ALL RESOLVED at spec rev 26

Former UNRESOLVED items 1-7, each now BINDING in SPEC-ungil §N.9
(§K.6 for item 7) with FULL text in history ANNEX AUD1. The
original analyses are kept below for the implementation record;
the RULING line at the head of each item is the disposition.
No item blocks U-T9.

### RESOLVED-1 (AUD1.N1). AbstractModuleRecord::m_resolutionCache — UNLOCKED HashMap, cross-thread UAF
RULING: CELL-LOCK (§N default) — all access under the record's
JSCellLock, the sibling-map lock; §E.1b alloc-outside shape; no
tier-inlined access. PRIORITY (UAF today). Amplifier owed (U28).
- State: `Resolutions m_resolutionCache` (WTF HashMap),
  runtime/AbstractModuleRecord.h:297-298.
- Mutation: `cacheResolution()` runtime/AbstractModuleRecord.cpp:342-345
  (`m_resolutionCache.add` — rehash frees bucket array); read:
  `tryGetCachedResolution()` :334-340. NEITHER takes any lock,
  while the SIBLING maps on the same cell ARE cell-locked in-tree
  (m_dependencies AbstractModuleRecord.cpp:1465; m_asyncParentModules
  :1561; visitChildren :100, :237).
- Reachability: any thread touching a shared module namespace object —
  JSModuleNamespaceObject::getOwnPropertySlot -> resolveExport ->
  resolveExportImpl -> cacheResolution (AbstractModuleRecord.cpp:667-669,
  :722, :771, :795, :933). Two threads reading `ns.x` race a rehash
  against a bucket walk = exactly the OM annex 15.7 SparseArrayValueMap
  UAF class.
- Proposed ruling: §N default — all access under the record's
  JSCellLock (10a, §E.1b alloc-outside shape), same lock already used
  for the sibling maps. No tier-inlined access exists (namespace loads
  are IC'd on the namespace object, not this cache) so no JIT work.

### RESOLVED-2 (AUD1.N2). RegExp::m_ovector — shared per-match scratch, unlocked on the mutator path
RULING: per-lite match buffer (annex K4 §I regexp row) — scratch
moves OFF the cell GIL-off; ovectorSpan consumers + DFG/FTL exec
thunks take the lite buffer; cell keeps compile state only (R13).
PRIORITY (UAF today). Amplifier owed (U28).
- State: `Vector<int> m_ovector` runtime/RegExp.h:232; resized per
  match at runtime/RegExp.cpp:183; handed out raw via `ovectorSpan()`
  RegExp.h:103.
- Compile state IS phase-1-in-tree cell-locked (RegExp.cpp:227, :251,
  :306, :319 compile*/compileIfNecessary*; deleteCode :384;
  matchConcurrently :373 — compiler-thread side only). But the JS-thread
  match path (RegExpInlines.h matchInline, called from RegExp::match
  RegExp.cpp:365) writes m_ovector with NO lock. Two threads exec()ing
  the SAME shared RegExp cell concurrently: racing resize = realloc UAF +
  torn capture reads.
- Proposed ruling: move per-match scratch off the cell (per-lite scratch
  buffer, §K.1 class; matches THREAD.md "lazy regexp stack" per-thread
  split) — preferred over cell-locking the whole match (would serialize
  hot regexp workloads and §N forbids parking under the lock for
  long-running Yarr JIT execution). Tier-inlined: DFG/FTL RegExpExec
  thunks land in matchInline — they inherit whatever the ruling picks;
  re-point to the per-lite buffer.

### RESOLVED-3 (AUD1.N3). DirectArguments lazy override storage (tier-inlined)
RULING: CAS-PUBLISH as proposed — alloc+fill complete, release-CAS
the pointer, losers discard; readers load-acquire; tier-inlined
null-check stays (address-dependent load, jit F2).
- State: `MappedArguments m_mappedArguments` (CagedBarrierPtr, lazy
  alloc on overrideThings) runtime/DirectArguments.h:183 (offsets baked
  for JIT :153-154); plus GenericArgumentsImpl
  `m_modifiedArgumentsDescriptor` lazy bitmap
  (offsetOfModifiedArgumentsDescriptor DirectArguments.h:154,
  init in GenericArgumentsImplInlines.h).
- Race: foreign read of arguments[i] (DFG GetFromArguments /
  inlined offsetOfMappedArguments null-check) vs owner's first
  `delete arguments.length`-class override: bitmap alloc + flag-flip +
  property materialization is a multi-word publication with no rule.
  OM annex 15.6 audited GenericArgumentsImplInlines only for butterfly()
  callers — explicitly NOT this state.
- Proposed ruling: m_mappedArguments/-Descriptor become
  CAS-PUBLISH (allocate+fill bitmap, release-CAS the pointer; losers
  discard); the materialize-properties half follows OM property rules;
  readers load-acquire. Tier-inlined null-check stays (address-dependent
  load, jit F2 shape).

### RESOLVED-4 (AUD1.N3). ScopedArguments::overrideThings flag + ClonedArguments::materializeSpecials publish order
RULING: as proposed — flag words release-stored AFTER the OM puts;
foreign slow-path readers acquire; no lost properties.
- ScopedArguments: `bool m_overrodeThings` runtime/ScopedArguments.h:170
  (JIT offset :156) flipped after length/callee/caller materialization;
  same family as UNRESOLVED-3 (flag must be release-published AFTER the
  OM puts; foreign tier-inlined readers acquire).
- ClonedArguments: `m_callee` doubles as the not-yet-materialized flag
  (runtime/ClonedArguments.h:100-104, JIT offset :78); materializeSpecials
  does OM puts then clears m_callee — single-word flag but publication
  ORDER is unruled; a foreign reader seeing the cleared flag before the
  puts misses callee/length entirely (lost-property class, violates
  THREAD.md "no lost properties").
- Proposed ruling: both = release-store of the flag word ordered after
  the OM puts; readers acquire on the slow path; tier-inlined fast paths
  re-pointed or fenced per jit item 7 audit.

### RESOLVED-5 (AUD1.N4). StructureRareData runtime caches (tier-inlined flag word)
RULING: installs under Structure::m_lock; each JIT-read word
single-word release-published LAST (watchpoint vector filled
first, immutable after); m_specialPropertyCache = §K.3. OM-annex
cross-pointer recorded in AUD1.
- State: `uintptr_t m_cachedPropertyNameEnumeratorAndFlag`
  runtime/StructureRareData.h:165 (+ FixedVector
  m_cachedPropertyNameEnumeratorWatchpoints :166, installed together);
  `WriteBarrier<JSCellButterfly> m_cachedPropertyNames[...]` :167
  (JIT offsets :110, :115); lazy
  `std::unique_ptr<SpecialPropertyCache> m_specialPropertyCache` :175
  (ensureSpecialPropertyCacheSlow :157, cacheSpecialPropertySlow :154).
- Race: for-in / Object.keys / toString caching mutates these on ANY
  thread iterating a shared structure; baseline/DFG read the
  enumerator+flag word and cachedPropertyNames directly. Multi-word
  install {enumerator, watchpoint vector, flag} has no rule; not covered
  by OM (not property storage), not §K (cell, not VM/global member),
  not §N.1-8. Structure::m_lock is not documented to guard rare-data
  cache installs on read paths.
- Proposed ruling: installs under Structure::m_lock (the structure
  already owns its rare data lifecycle, OM GT order); the
  JIT-read words each single-word release-published; watchpoint vector
  immutable post-publication (publish pointer last). m_specialPropertyCache
  = §K.3-class lazy publication. Needs an OM-annex cross-amendment
  because watchpoint-fire sites are jit-spec territory.

### RESOLVED-6 (AUD1.N5). Intl cell family — lazy mutable members + ICU reentrancy unproven
RULING: post-construction-mutable members = CELL-LOCK (lazy
Strings computed outside, published under it); construction-frozen
ICU handles used concurrently ONLY via call sites verified
const/thread-safe (AUD1 checklist), else clone-per-use under the
lock. NO TypeError, NO SD.
- IntlNumberFormat: `mutable String m_numberingSystem`
  runtime/IntlNumberFormat.h:232 (lazy compute on read; String = two
  words refcounted — torn publish + non-atomic ref).
- IntlLocale / IntlSegmenter / IntlSegmentIterator and kin: lazily
  computed String/UObject members of the same shape; IntlSegmentIterator
  advances a UBreakIterator (inherently mutating) per next().
- Cross-cutting: even immutable-after-init ICU handles (UCollator at
  IntlCollator, UNumberFormatter at IntlNumberFormat.h:225, initialized
  in initialize* during construction) are only safe for CONCURRENT use
  via const ICU APIs — unverified per call site.
- Proposed ruling: per-class audit row; default = cell-lock around any
  member that mutates post-construction (segment iterators, lazy
  strings); ICU const-use proof or per-thread clone for format/compare
  hot paths. No tier-inlined accesses (all host calls). Until ruled:
  candidates for GIL-OFF TypeError on foreign-thread use (SD entry
  required if taken).

### RESOLVED-7 (AUD1.K2, SD19; cross-ref §K.4/U-T8b scope). RegExpGlobalData / RegExpCachedResult — tier-inlined multi-word global cache
RULING: per-lite (§K.1) with per-thread RegExp.$1-$9 semantics =
SD19; DFG/FTL RecordRegExpCachedResult re-pointed via the lite
(AUD1.K4 A16 ext). Annex K4 §0 U2 row owns it.
- State: per-JSGlobalObject `RegExpCachedResult m_cachedResult`
  runtime/RegExpGlobalData.h:64 — multi-word {m_result(2 words),
  m_lastInput, m_lastRegExp} updated on EVERY global-flag match, plus
  lazy reification flip {m_reified + 4 reified barriers}
  (runtime/RegExpCachedResult.h:75-82).
- Listed HERE because DFG/FTL write m_result/m_lastInput/m_lastRegExp
  inline (offsetOfResult/offsetOfLastInput RegExpCachedResult.h:66-70 —
  RecordRegExpCachedResult): §N.7's "tier-inlined accesses disabled or
  re-pointed" clause applies even though the carrier is a global member.
  U-T8b must rule it (per-lite copy = §K.1, matching RegExp.$1 semantics
  per thread = SD entry; or locked = kills inlining). Ruled per-lite
  + SD19 at rev 26 (annex K4 §0 U2; AUD1.K2).

---

## Resolved inventory (IU table)

Dispositions cite the governing frozen text. "PHASE-1-IN-TREE" =
the serialization already exists in this branch's source (census:
cellLock() users in runtime/), satisfying §N default shape; GIL-off
keeps it as-is.

| # | Cell class (file:line) | Non-property mutable state | Disposition |
|---|---|---|---|
| R1 | JSMap/JSSet (runtime/JSMap.h:32, JSSet.h:32, JSOrderedHashTable storage) | hash table buffer, load factors | COVERED §N.1 — ALL ops (reads too) cell-locked; DFG/FTL map intrinsics DISABLED GIL-off (tier-inlined accesses disabled), locked native bodies |
| R2 | JSWeakMap/JSWeakSet (runtime/WeakMapImpl.h:209) | m_buffer, m_keyCount, m_deleteCount | COVERED §N.1 (WeakMapImpl named) |
| R3 | JSMapIterator/JSSetIterator (runtime/JSMapIterator.h:36, JSSetIterator.h:36) | internal fields (entry cursor) + table traversal | COVERED §N.5 (internal-field claim/publish) + §N.1 (storage reads under cell lock); transparent-to-GC bucket hopping inherits N.1 |
| R4 | JSString rope/atomization (runtime/JSString.h:637-682) | fiber0/flags publication | COVERED §N.2 — lock-free release-CAS publish, losers discard; resolveRopeToAtomString vs shared table per U0; JIT rope slow calls land here |
| R5 | DateInstance (runtime/DateInstance.h:62-75) | GregorianDateTime cache m_data | COVERED §N.3 — cache BYPASSED GIL-off; m_data lazy alloc CAS-published; vm.dateCache per §K.1/2 |
| R6 | JSFunction/FunctionRareData (runtime/JSFunction.h:136-144; FunctionRareData.h:44, profiles :72-99) | rare-data materialize; allocation profiles; cached structures | COVERED §N.4 — materialize per §K.3; internals under function's cell lock; profiling fields RACY-TOLERATED (jit item 7); cached Structures per I34 |
| R7 | JSGenerator (runtime/JSGenerator.h:33), JSAsyncGenerator (JSAsyncGenerator.h:36), async function frames | resume state internal fields | COVERED §N.5 — single-word resume-claim CAS SuspendedX->Running; @atomicInternalFieldClaim/Publish twin intrinsics, mode-keyed lowering; interior stores plain while claimed |
| R8 | JSArrayIterator (.h:32), JSStringIterator (:33), JSIteratorHelper (:32), JSRegExpStringIterator (:34), JSWrapForValidIterator (:34), JSAsyncFromSyncIterator (:34), InternalFieldTuple (Bun ALS) | internal fields | COVERED §N.5 (iterator helpers named; InternalFieldTuple per §E ALS1.3 + history r25 ext) |
| R9 | JSPromise + reactions | flags/reactions internal fields | COVERED §E.1b/annex E1B + §E.7 (settle CAS; out of §N by charter, listed for closure; U-T9 settle-site IU table owns call sites) |
| R10 | ArrayBuffer (runtime/ArrayBuffer.h:199, :298) | detach/transfer/resize/grow {base,length} pairs | COVERED §N.6 + annex N6 torn-pair table — detach length=0 seq_cst + quarantine to heap §10 stop; grow base-immutable commit-then-release-length; wasm grow ditto (wasm cells otherwise §I REFUSED v1) |
| R11 | JSArrayBufferView (runtime/JSArrayBufferView.cpp:265, :327 cell-locked wasteful/oversize paths) | m_vector/m_length/m_mode | COVERED §N.6/annex N6 (hoisted vectors jettison) + PHASE-1-IN-TREE for mode transitions |
| R12 | ScriptExecutable/FunctionExecutable/EvalExecutable/ProgramExecutable/ModuleProgramExecutable | first CodeBlock install; m_jitCodeFor*; unlinked generation | COVERED §N.8/annex CBI — compile outside locks, release-CAS m_codeBlockFor{Call,Construct}, loser discards; adjacent fields per-field ruled; UnlinkedCodeBlock = §K.3-class. visitChildren already cell-locked in tree (FunctionExecutable.cpp:91, EvalExecutable.cpp:61, ScriptExecutable.cpp:444, ProgramExecutable.cpp, ModuleProgramExecutable.cpp) |
| R13 | RegExp compile state (runtime/RegExp.h:222-231: m_state, m_regExpBytecode, m_regExpJITCode, m_rareData) | lazy compile/deleteCode | PHASE-1-IN-TREE — cell-locked at RegExp.cpp:227, :251, :306, :319, :373, :384; conforms to §N default. (m_ovector -> RESOLVED-2: per-lite buffer) |
| R14 | RegExpObject (runtime/RegExpObject.h:165 m_lastIndex) | lastIndex word + writability bit | RACY-TOLERATED — single WriteBarrier word, SAB-grade staleness; property-equivalent semantics (spec'd as a property); tier-inlined offsetOfLastIndex stays |
| R15 | ErrorInstance (runtime/ErrorInstance.h:170-171 m_stackTrace, m_errorInfoMaterialized; m_sourceAppender :170) | lazy stack/errorInfo materialization | PHASE-1-IN-TREE — cell-locked at ErrorInstance.cpp:117, :128, :141, :177, :209, :229, :393, :418, :451; m_sourceAppender single-word. Conforms to §N default |
| R16 | AbstractModuleRecord maps EXCEPT resolution cache (runtime/AbstractModuleRecord.h:48; m_dependencies, m_asyncParentModules) + module loader pipeline (ModuleGraphLoadingState.cpp:64, :79; JSModuleLoader.cpp:269, :758, :945, :1072, :1087) | link/evaluate bookkeeping | PHASE-1-IN-TREE — cell-locked (AbstractModuleRecord.cpp:100, :237, :1465, :1561). Resolution cache -> RESOLVED-1 (cell lock) |
| R17 | JSModuleNamespaceObject (runtime/JSModuleNamespaceObject.h:95 m_exports) | export map | IMMUTABLE post-finishCreation — no entry needed; its getOwnPropertySlot path inherits RESOLVED-1 |
| R18 | JSFinalizationRegistry (runtime/JSFinalizationRegistry.h:116-117; lock-taking API :88-96) | live/dead registration maps | PHASE-1-IN-TREE — all access via Locker<JSCellLock> parameters (JSFinalizationRegistry.cpp); GC-side sweep under heap spec stops |
| R19 | JSWeakObjectRef (runtime/JSWeakObjectRef.h:49-55, :75) | m_lastAccessVersion + m_value | RACY-TOLERATED with one amendment: m_lastAccessVersion load/store made relaxed-atomic (single word); m_value single WriteBarrier; deref'd cell kept live by conservative scan (heap I7) regardless of version race |
| R20 | ProxyObject (runtime/ProxyObject.h:138-142) | target/handler internal fields; m_handlerStructureID/-PrototypeStructureID caches; m_isCallable bits | RULED HERE: internal fields single-word (revoke = seq_cst null store, foreign readers re-validate — TypeError path already exists); structure-ID caches each single-word, independently re-validated => RACY-TOLERATED; m_isCallable/m_isConstructible immutable post-construction |
| R21 | JSBoundFunction (runtime/JSBoundFunction.h:94-99) | m_boundArgs (immutable); m_nameMayBeNull lazy; m_length NaN-sentinel double | RULED HERE: m_boundArgs/boundThis immutable post-construction; m_nameMayBeNull = idempotent single-word release-publish (CAS-PUBLISH, losers' value identical); m_length = idempotent 8-byte store, RACY-TOLERATED |
| R22 | GetterSetter | m_getter/m_setter words | RACY-TOLERATED — two independent single words; pair-tearing = SAB-grade staleness, each word always a valid callee; OM accessor-slot rules own the slot itself |
| R23 | JSPropertyNameEnumerator (runtime/JSPropertyNameEnumerator.h:115-116) | names buffer + cached StructureID | IMMUTABLE post-creation (computeNext mutates caller-owned cursor only) — no entry |
| R24 | SparseArrayValueMap (runtime/SparseArrayValueMap.cpp cell-locked) | hash map innards | COVERED OM §4.6 + annex 15.7 — AS family fully cell-locked both sides, AS-COPY; jit never fast-paths sparse |
| R25 | SymbolTable (runtime/SymbolTable.h:799 ConcurrentJSLock m_lock), JSSegmentedVariableObject (own m_lock, JSSegmentedVariableObject.cpp) | symbol map / variable spine | PHASE-1-IN-TREE (pre-existing concurrent locks; jit spec already consumes them) |
| R26 | Structure/StructureRareData TRANSITION state, PropertyTable, butterflies, indexing storage | — | OUT OF §N SCOPE — OM spec (§2-§10, I-series). StructureRareData runtime CACHES -> RESOLVED-5 |
| R27 | JSGlobalObject lazy properties, VM caches (numericStrings, dateCache, RegExpCache — already locked RegExpCache.h:79) | — | OUT OF §N SCOPE — §K.1-5 + U-T8b inventory. RegExpGlobalData cross-ref -> RESOLVED-7 (per-lite, SD19) |
| R28 | Wasm cells (JSWebAssembly*, WebAssemblyModuleRecord) | all | COVERED §I — REFUSED in v1 (GIL-OFF TypeError on spawned threads) |
| R29 | Symbol, BigInt, StringObject/NumberObject/BooleanObject internals, Temporal* ISO fields, ShadowRealmObject, JSGlobalProxy/JSProxy target word, Exception, JSNativeStdFunction, JSCustomGetterFunction/JSRemoteFunction targets, JSSourceCode, JSTemplateObjectDescriptor descriptor ref, JSScriptFetcher/JSScriptFetchParameters | construction-time-only or single-word state | IMMUTABLE post-publication / single-word — no entry. (JSRemoteFunction lazy name word: same idempotent shape as R21.) |
| R30 | API cells (API/JSCallbackObject* private properties, JSAPIWrapperObject) | callback data maps | OUT OF runtime/ SWEEP — owned by SPEC-api §F/U-T8 (api lock ranks); named here so U-T8c closure is explicit |
| R31 | DebuggerScope + inspector-reachable cells | scope cursor | COVERED §A.2.7 — debugger walks only inside §A.3 stops |

## Gate disposition

- U-T8c result: 31 ruled/covered rows + 7 residue items, ALL
  RESOLVED at spec rev 26 (§N.9/§K.6; history ANNEX AUD1). The U-T9
  audit gate is SATISFIED on this annex's side; implementation
  CONSUMES this table verbatim.
- Severity note: RESOLVED-1 and -2 are memory-unsafe TODAY under any
  GIL-off interleaving (HashMap-rehash UAF; scratch-vector realloc
  UAF) — same defect class OM annex 15.7 fixed for ArrayStorage.
  Implement first.
- Amplifier arms owed (U28): two-thread shared-namespace property
  storm (RESOLVED-1); two-thread exec() on one shared RegExp
  (RESOLVED-2); foreign-reader vs owner override on Direct/Scoped/
  ClonedArguments (RESOLVED-3/4); two-thread for-in over one shared
  structure (RESOLVED-5); regexp legacy-statics SD19 variants
  (RESOLVED-7). TSAN + arm64 per §N.5 precedent.
- Census note: cellLock() users found in runtime/ (this branch):
  AbstractModuleRecord, ErrorInstance, Eval/Function/Program/
  ModuleProgram/ScriptExecutable, JSArray(+Inlines), JSArrayBufferView,
  JSFinalizationRegistry, JSGlobalObject, JSModuleLoader,
  ModuleGraphLoadingState, JSObject(+Inlines), JSSegmentedVariableObject,
  RegExp, SparseArrayValueMap, Structure, ConcurrentButterfly,
  JSGenericTypedArrayViewInlines — i.e. phase-1 already landed the §N
  default shape for most multi-word cases; this annex's residue list
  is exactly what the census exposed as unlocked or unruled (all now
  resolved, §0 above).

---

# T. ORDERED TASK LIST

Source: rev-9 NORMATIVE ANNEX 3 (IDs frozen) as extended by the
r10–r28 §T deltas, with the history's own task-sizing license
applied: *"if FTL's B3 stack-slot interactions with per-lite scratch
buffers exceed budget, split U-T4 into U-T4a (Baseline/DFG) and
U-T4b (FTL) at the implementation workflow's discretion — the spec's
freeze is on the addressing contract, not the task boundary."* That
split is taken here. 18 tasks.

**Dependencies:** T1 -> {T2, T3, T4a}; T4a -> T4b; {T2, T5} -> T6;
T5 gates T12; {T8, T8b, T8d, T8e} -> T9; T9 gates T11; T14 last.
**Entry gates:** U-T9 requires the §F.6(b) SD10
continuation-affinity embedder sign-off (r21; the ALS slice is
discharged by ALS1) AND annex-K4/N7 §0 closure (DONE at r26). U-T10
requires the Task-14 verdict recorded (§D.2). **T1–T7 are dark**
(land behind the flag with no behavior change until later tasks
activate them).

---

### U-T1 — §A.1.2-7 / §A.3.6 base rerouting (dark)
Mode-split Group-3 storage (§A.1.3 two-level discriminator; VM
accessors branch on vm.m_gilOff) + the per-VM GC root walk (r6 F5) +
lazy main/embedder carriers (annexes A36 + A36C: {lite, tag, client}
tuple swap) + per-lite scratch/regexp members + the per-entry record
(§A.1.5) + the VM-wide/thread-local service table.
**Creates INTEGRATE-ungil.md (IU)** with the TERM1.6 skeleton tables
(i)–(vii). Amplifiers: thrower parked pre-catch survives a forced
full GC; two-VM root-walk arm.

### U-T2 — §A.2 traps, watchdog, debugger
Per-lite VMThreadContext/VMTraps, rule-3 fan-out (TERM1 VM-wide
termination form), SignalSender off, D9/§C.6 re-points, stack
limits; §A.2.7 debugger carrier-only delivery (SD13); §A.2.8 = annex
W in full (W0–W3, the r15 F2 old-node disposition, the W ext W4
assert rewrite + spawned-unreachability lint). Corpus: the annex-W
(a)/(b)/(c) wall-clock arms; SD14 GILOn variants; AUD1.K1 SD18
profiler arm. IM: Watchdog.{h,cpp}, VMTraps.cpp, Debugger.

### U-T3 — §A.1.1-3 LLInt + U0c (dark)
loadVMLite emitter (jit App. R5 per-OS); the LLInt
gilOffProcess/per-lite-byte two-level selection; VMEntryRecord
m_vmLite slot; U0c ctor designation
(Heap::tryDesignateStickySharedServer + m_gilOff + the eager
noteSharedServerSticky + the add:69 assert). Corpus: compile-heavy
run THEN first spawn (Group-3 consistency); the two-VM construction
arm.

### U-T4a — §A.1.3/6 Baseline + DFG emission (dark)
Baseline/DFG codegen-time mode-keyed emission for Group-3 + scratch
(annex A16 two-load indirection; non-baked arm;
VMLite::scratchBufferForSize) + the AUD1.K4 A16-ext rows
(MegamorphicCache, HasOwnPropertyCache, m_regExpGlobalData,
m_weakRandom lite-relative emission). Golden-disasm re-baseline
(once, shared with U-T4b).

### U-T4b — §A.1.3/6 FTL + OSR emission (dark)
FTL (B3) emission for the same contract, incl. OSR-exit +
calleeSaveRegistersBuffer scratch indirection and JITCode-RESIDENT
buffers (catchOSREntryBuffer, FTL m_entryBuffer) as per-lite registry
indices. Amplifier: concurrent catch/loop OSR entry, one CodeBlock.
The addressing contract is frozen; only the task boundary is split.

### U-T5 — §A.3 thread-granular STW + §A.3.8 + deletions
Real R1.a-i sequence with the HBT4 order (licensed edits: the
:252-304 bracket reorder + the :208-221 comment rewrite); SB1
seq_cst stop-bit/access contract; the conductor predicate
implemented as EXIT1.2 per-sample registry walks (forEachEntered-
Thread helper; no lite/client pointer caching across samples; lite
absent OR TEARDOWN => exited, a TEARDOWN lite's re-acquire refused
(asserted), clientHeap null => not-entered — r28-r30); ISB1
stop-generation counter + context-sync on non-NVS JIT re-entry;
§A.3.8 per-thread GC parking (notifyVMStop + heap §13.5 re-rule);
the §A.3.2b AHA stop-gate supersession; DELETE the stub
asserts/witnesses/M7 tripwire (§J.8). Gates: GIL-on no-regression +
N-separate-VMs + $vm stop/resume vs access-released embedders; U4 +
§A.3.8 amplifiers; the SB1.6 arm64 litmus arm; the ISB1.6
sleep-through-jettison arm64 arm; the EXIT1.8
exit-storm-under-stop-storm litmus/amplifier arm (shared with
U-T6); the EXIT1.8 U20 lint extension (as amended by r29-r31:
TEARDOWN-mark-precedes-destroy + unregisterLite-LAST + ~VM
EXIT1.9-wait-precedes-teardown + the A36 deferred-dtor
no-m_server check + EVERY physical registry removal an
unregisterLite call + lite-state access under-lock-only).

### U-T6 — §B.1-3 clients + nested entry
Per-thread GCClient spawn/teardown + lazy-carrier ACT (§F.1), token
access, JSLock forwarding GIL-on-only (§B.3 supersession); the
EXIT1.3 T5 teardown order as AMENDED by r31 (TEARDOWN mark -> DCT
-> destroy client -> unregisterLite -> free lite) on the LIVE-VM
paths — spawned T5 and carrier TLS-death; this task OWNS the
teardown paths and implements the EXIT1.9 ~VM completion fence
(registry Condition; unregisterLite notify; ~VM order steps
(1)-(4): main-carrier TLS uninstall -> A36 carrier collection
(COLLECTED-mark + unregister under one lock hold, lock-free full
server-side detach, per-client DETACHED flip + notifyAll, ALL
PRE-wait) -> the blocking
WAIT -> m_mainVMLite unregister + the rest of ~VM; the assert walk
demoted to a post-wait debug sanity check), the r31 carrier-state
machine (lite-state byte LIVE/TEARDOWN/COLLECTED/DETACHED;
transitions and reads under-lock-only; the state-keyed TLS
destructor with the COLLECTED wait), the unregisterLite-only
physical-removal mandate, and the A36 amendment
(degenerate DETACHED-gated deferred dtor; M11/M12 no-op queue
removal) — plus the EXIT1.4(b) clientHeap write-once
release-publish (r28); §F.5 nested-entry release/restore + the
TERM1.5 spawned-foreign-VM RELEASE_ASSERT; U0b second-VM behavior +
corpus arms; the A36C two-VM alternating-entry and nested re-stamp
arms; nested-GC corpus arm; the EXIT1.8 carrier
TLS-death-vs-stop-window arm; the r30-REWRITTEN EXIT1.8
T5-TAIL-VS-~VM / join-then-destroy-VM race arm (RELEASE + ASAN
build: amplifier stalls the joined thread in the T5 tail —
post-mark pre-DCT, mid-DCT, mid-client-destroy, pre-unregister —
while the embedder destroys the VM; ~VM must BLOCK in the EXIT1.9
wait and return only after the stalled thread's unregisterLite,
instrumented ordering check, no UAF, ASAN clean; DEBUG variant
adds the post-wait sanity walk; CARRIER variant: collected carrier
unregistered pre-wait, owner TLS destructor delayed past VM
destruction, deferred degenerate dtor + no-op M12 removal touch no
VM/server memory) and the r31 EXIT1.8
CARRIER-TLS-DEATH-DURING-DETACH arm (DEBUG AND RELEASE, ASAN:
amplifier stalls the ~VM walk inside a collected client's
lock-free detach — post-unregister pre-detach,
mid-lastChanceToFinalize, post-detach pre-flip — while the owner
thread exits; the TLS destructor must read COLLECTED under the
registry lock, park on vmTeardownCondition, and run the degenerate
path only AFTER the walk's DETACHED flip; no double
clientSet().remove, no concurrent MSPL on the same client, no UAF;
+ the reverse dtor-wins-LIVE variant; + the r32 WALK-FREE variant:
main-thread bit-SET carrier + embedder bit-CLEAR carrier, the
amplifier stalls the walk between the bit-set lite's DETACHED flip
and its degenerate free while the embedder's dtor fires — the walk
frees the bit-set lite exactly once, no dtor ever visits it, the
bit-clear lite is never walk-freed) — both join this task's gate
list; U-T6 also implements the r32 ownerHasNoTlsDtor bit
(registration-time-fixed under the registry lock; main thread =>
destructor-free thread_local carrier map, all other threads =>
the destructor-bearing ThreadSpecific map; the rev-32 A36
amendment).

### U-T7 — §B.4-6 TLC addressing
TLC lite-relative inline allocation, all tiers; U21 bench (§B.5
budgets; the Dev-7 deferral gate). Sweep-storm amplifier
(dead-lock-object-with-pending-asyncHold, §F.3).

### U-T8 — §F/§J lock contract
Tokens; the §F.2 predicate split + the ~60-consumer IU table (annex
F2 fixed rulings; the ~AsyncTicket/finalizer rows); DAL2 (spawned
DropAllLocks access bracket; U14 re-derivation; the U24
DAL/GC/haveABadTime arm; §F.6 delta (c) + the Bun blocking-site IU
enumeration); HandleSet m_strongLock; J.7 replacement; the §E.4
settle-site lock-context table; the §F.6 embedder checklist rows;
§10A.1-slot consumers note A36C as their GIL-off stamping authority.

### U-T8b — CONSUME annexes K4 + N7 (+ ~VM walk)
No enumeration work remains (audits EXECUTED at r26). Deliverables:
the §K class implementations per K4 rows (per-lite duplicates +
registry-walk rooting; leaf locks; §K.3/LZ1/LZ2 machinery incl. the
owner side table, wait-for edges, winner unwind scope, exit/~VM
asserts); §N dispositions per N7 rows (AUD1.N1/N2 FIRST — both are
memory-unsafe today); the K4 §VIII no-write-after-first-cross-
thread-entry assert macro; the ~VM per-lite teardown walk; §F.2
consumer-row citations; the touch-context + conductor-closure-
reachable (LZ2.2) + handle-creation lock-context (WS1.4) columns;
m_asyncContextData as a PRE-RULED class-1 row (ALS1.3); the WS1.2
re-shapes (ThreadManager/RegExpCache) + WS1.5 churn corpus. Gates
U-T9.

### U-T8d — §A.1.7 off-thread readers
Enumerate every off-thread reader of each rerouted Group-3 field
into the IU table with disposition (i)/(ii)/(iii); SamplingProfiler
v1 carrier-only capture (AUD1.K1) under the r24 SUSPEND RULE;
sample-storm arm (target spinning in fastMalloc-heavy native code,
TSAN + deadlock watchdog). Gates U-T9.

### U-T8e — §E.1b.4 host-hook dispositions
Enumerate EVERY globalObjectMethodTable/host-callback slot
JS-reachable on a spawned TS; IU disposition per hook: {inline,
carrier-queued, refused, unreachable}; the SD15 tracker handoff
machinery. Gates U-T9.

### U-T9 — §E event loop + settlement
**ENTRY GATES: the §F.6(b) SD10 continuation-affinity sign-off (r21;
ALS slice discharged by ALS1); annex-K4/N7 §0 closure (satisfied at
r26); U-T8/U-T8b/U-T8d/U-T8e complete.**
E2A drain loop + close; E.3 keepalive; E.4 routing + retirement
(r17 F2 precondition, r18 F2 act-after-drop); E.1b promise protocol
(E1B) + ALS1 per-lite cursor reroute + SD15 tracker; E.5/TERM1
termination; E.7 hooks (r8/E7/r17 F3) + §E.7.5 registrant routing.
Corpus: SD1-SD3/SD8/SD10-SD12/SD15/SD17 + hook arms (incl.
hook-takes-loop-lock, closed-registrant spawned-settle,
dead-registrant w/ hooks, the hooks-OFF join/asyncHold cycle) + §N
arms + the ALS1.4 arm + U26 arms (d)/(e) + the U4 one-VM arm.

### U-T10 — §C.1-2 atomic slot accessors
**ENTRY GATE: the OM Task-14 verdict recorded (§D.2); a PROMOTE
verdict lands Task 14 first and re-reviews §C's third arm
pre-code.**
§9.5 accessors, all arms incl. the AS pre-lock SW protocol +
flat-path SW discipline + indexed-by-shape; ThreadAtomics re-home
with D3/D7. U5/U28 CAS-storm arms.

### U-T11 — §C.3-6/§G/§J.3 waits
PWT re-home + I10 re-validation (C3: pre-enqueue §9.5 routing +
converting-arm corpus); the 4.5-1a vm.m_gilOff lift; G11 re-point;
D2/D4/D8 (the SD6 GIL-on edit); the §G predicate; §J.3
captured-lite parks + main-park m_lock release + the W1
episode/old-node rules; §E.7.5 timeout-timer + SD16 arms; the §E.5
close-harvest arms; the terminated-join rethrow arm (SD8 ext2); the
NLH1.5 conduct-inside-hold(fn) amplifier; the SD17 arm; the W4
carrier-parked watchdog arm. Corpus SD4-SD6 + the §J.3 C-API
embedder arm.

### U-T12 — §D.1 TID rebias
Rebias inside a full shared-GC stop; the two-phase TM
snapshot/restamp/release; D1R TTL fires + jettison in-stop;
spawn-storm; the two-VM TM-churn amplifier; the D1R.5
reissue/jettison amplifier. Gated by U-T5 (stop machinery).

### U-T13 — §H/§I/§A.3.7/§K.5/§N.6
SymbolRegistry lock; atom-table swap GIL-on-only + the 14-assert
predicate-preserving supersession; wasm isSpawned prologue checks +
both U17 arms (SD7 edit) + the wasm-GC LinkError precheck
(JSWebAssemblyInstance.cpp); the §N.6/annex-N6 four arms +
quarantine list/extra-memory accounting + the extended U28
amplifier; the §K.5 class-4 implementations (HBT/HBT2-4) verified
LZ2-clean (pre-resolution sites land with the class-4 work) + the
HBT corpus arms.

### U-T14 — close
U0/U0b/U0c gates; TSAN + the full amplifier battery; U19; the
default flip (useThreadGIL -> false); IU dispositions complete; the
flag-off delta list (a)/(b)/(b2) re-audit incl. the r17 F5 lowering
rule (no host-op call reachable gilOffProcess=false); the full U20
lint set (incl. job-slot, LZ2.5, WS1.4, SB1.6 rules); §F.6 (a)/(c)/
(e) embedder sign-off + the (d) construction-order audit row.

---

# PER-TASK GATE LIST (run after EVERY task U-T1 … U-T14)

Every task, on landing, re-runs ALL of:

1. **Flag-off golden-disasm gate.** Byte-identical flag-off codegen
   vs the recorded baseline. The baseline is RE-BASELINED exactly
   twice in the whole program — once for the LLInt Group-3
   gilOffProcess branches (§A.1.3 delta (a), at U-T3) and once for
   the §N.5 twin-intrinsic uniform bytecode (delta (b2), at its
   landing) — and is frozen otherwise; any other flag-off codegen
   delta is a gate failure. The flag-off --useJIT=0 bench gate (jit
   Task-13) is re-run after the re-baseline and must stay in-noise;
   the §N.5 flag-off microbench is GATED at 1% vs the pre-threads
   baseline (BENCH.md).
2. **U19 GIL-on fallback oracle.** The full GIL-on corpus
   (useJSThreads=1, useThreadGIL=1) green and UNCHANGED except the
   two recorded both-mode edits SD6/SD7 (edited once, at their
   landing tasks); all GIL-off-only SDs (SD1-SD5, SD8-SD19) run via
   //@ runThreadsGILOff/GILOn variants with the OLD expectations
   preserved GIL-on. U19's terminate arms assert TERM1 VM-wide
   semantics (parked sibling ALSO terminated + closes Failed;
   join-after-termination rethrows the ordinary
   Error("Thread terminated"); asyncJoin rejects with it).
3. **Flag-off delta re-audit.** The permitted flag-off delta list is
   exactly {(a) one not-taken gilOffProcess branch per LLInt Group-3
   storage-selection site (nested inside existing ifJSThreadsBranch
   regions where applicable — zero NEW branches there); (b)
   atomicsWaitImpl's useJSThreads branch; (b2) the §N.5 twin
   intrinsics' uniform bytecode lowering to the landed sequence
   behind the delta-(a) branch}. Any task introducing a flag-off
   branch, call, or bytecode shape outside this list fails the gate.
   The final full re-audit (incl. the r17 F5 no-host-op-call check)
   runs at U-T14.

Additional standing gates carried with their owning tasks: the §B.5
perf composite (<=10% 1-thread GIL-off; {1,0} <=5%; a miss pulls jit
§4.3 LLInt-cache revival / the heap Dev-7 items forward); the U20
lock-order lint (full rule set per INV U20, incl. the r28 EXIT1.8
extensions as amended by r29-r31); TSAN + race-amplifier
arms as enumerated per task — incl. the EXIT1.8
exit-storm-under-stop-storm arm (ASAN + TSAN, U-T5/U-T6), the
r30-REWRITTEN EXIT1.8 T5-tail-vs-~VM / join-then-destroy-VM arm
(RELEASE + ASAN + the DEBUG sanity-walk and CARRIER deferred-dtor
variants, U-T6) and the r31 EXIT1.8 CARRIER-TLS-DEATH-DURING-DETACH
arm (DEBUG AND RELEASE, ASAN, U-T6);
arm64-hardware runs for the SB1.6,
ISB1.6 and §N.5 ping-pong arms.

## ADDENDUM (post-AB-17 obligation-10/B review round)

1. **SuspendExceptionScope trap-bit ops are the ONE deliberate
   mode-UNKEYED semantic change of the obligation-10 reroute** (not a
   byte-identical reroute): the ctor clearTrap(NeedExceptionHandling) /
   dtor fireTrap run in ALL modes, including flag-off, where the old
   fork code raw-saved vm.m_exception/m_lastException and left
   bit=set/word=null inside a suspend window. The new behavior restores
   bit<->word coherence (upstream clearException/restorePreviousException
   semantics) and is REQUIRED by RETURN_IF_EXCEPTION's
   EXCEPTION_ASSERT(!!exception == needHandling). Coverage: the V5a
   flag-off identity gate (40/40) covers this site specifically. Do NOT
   "restore" the raw saves as a perf cleanup — that reintroduces the
   bit/word desync inside every suspend window in assert builds.

2. **§A.3.2 per-park-site happens-before/poll discharge table** (the
   FIX-2 banner in JSThreadsSafepoint.cpp was corrected to match):

   | Park class | Release mechanism | HB edge to conductor sample | Re-acquire gate |
   |---|---|---|---|
   | Executing JS | requestStop trap + per-sample re-fire (VMManager loop) | seq_cst stop word / trap RMW | trap return path |
   | LockObject hold | GILDroppedSection bracket (spawned: DropAllLocks; carrier: unlockAllForThreadParking -> willReleaseLock gilOff client release, JSLock.cpp) | seq_cst access-state store vs fenced registry walk (SB1 item 2) | bracket-exit gated AHA (F8/§A.3.2b) |
   | Atomics/property wait (ThreadAtomics.cpp) | same GILDroppedSection bracket | same | same |
   | Thread join (ThreadObject.cpp) | same GILDroppedSection bracket | same | same |
   | Compile-side waits (BytecodeGenerator/DFGPlan GILOffCompilationLocker spins, ScriptExecutable, JSObject.h:2005) | parkSitePollAndParkForStopTheWorld per quantum | gcClientWillParkForThreadGranularStop seq_cst RHA + jsThreadsNotifyMutatorQuiesced | gcClientDidResumeFromThreadGranularStop -> gated AHA |
   | JITWorklist::waitUntilAllPlansForVMAreReady | gcClientWillParkForThreadGranularStop bracket (whole wait) | same | same |
   | Lookup.cpp static-property reification contention | same shape | same | same |
   | C++ host call holding access in any OTHER unbounded wait | **UNDISCHARGED unless bracketed or polled — this is the residual root-cause-B class** | — | — |

   Rule for new code: any unbounded native wait reachable while an
   entered lite's client holds heap access must take one of the two
   mechanisms above. Root cause B (counter-lock 5/5 watchdog timeout)
   remains OPEN: at least one class-(2) wait is still unfound; the
   watchdog timeout dump now names the non-quiescent lite
   (JSThreadsSafepoint.cpp watchdogAssertStopProgress(requestStart, &vm))
   so the next counter-lock run localizes it directly. The thread-ab17b
   acceptance gate stays RED until that site is found and bracketed.

3. **§I wasm refusal — emission/instantiation fail-stops added**:
   RELEASE_ASSERT(!vm.gilOff()) at JSWebAssemblyModule::create and
   JSWebAssemblyInstance::tryCreate. The 14-surface
   throwIfWebAssemblyRefusedOnSpawnedThread gate remains the primary
   line; the asserts convert a gate bypass (new API surface, cross-VM
   module transfer under useSharedGCHeap, refactor) from silent
   lost-exception behavior into a loud crash at minting.

4. **Obligation-10 straddle enforcement now exists in code** (was
   documented-only): ExceptionScope's ctor captures the resolved
   verification storage and its dtor RELEASE_ASSERTs storage identity
   plus the strict LIFO invariant (m_topExceptionScope == this);
   SuspendExceptionScope ASSERTs ctor/dtor group3Primitives() identity
   (TopCallFrameSetter precedent). Assert-class builds only; shipping
   configurations are byte-identical.

— End of handout. SPEC-ungil.md remains the doc of record. —
