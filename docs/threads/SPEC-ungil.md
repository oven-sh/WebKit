# SPEC-ungil.md - N-mutator execution model (GIL removal)

Status: rev 35 (history = SPEC-ungil-history.md; r33 = the
SPEC-congc §13.5(1)-(3) adoption-gate closure, back-cites congc
ANNEX CGS2.1-4; r34 = watchdog/queue-term fixes, CGS2A
[r34]; r35 = pause ship-gate + COUNT bound, REV 35). Closes
UNGIL-PLAN.md Part III/IV
CHARTERED/GAP A-J. Authorities: THREAD.md;
SPEC-{heap,vmstate,objectmodel,jit,api}.md (+annexes);
INTEGRATE-api D1-D13. Verified vs tree 2026-06-06; r33 cites
vs tree 2026-06-10.
Re-freezes/SUPERSESSIONS cite both sides. vmstate:/api:/om:/
jit:/heap: = SPEC files; IU = INTEGRATE-ungil.md.

Master rule: GIL-off is a MODE (useThreadGIL=false). Every GIL-on
path stays compiled, the fallback/bisection oracle (§J); GIL-on
observable behavior unchanged EXCEPT both-mode deltas SD6
(§C.6/§A.2.6) and SD7 (§I). "GIL-off" = useJSThreads() &&
!useThreadGIL(); requires useVMLite=1, useSharedAtomStringTable=1,
shared GC server (U0).

## 0. Execution model

Post-GIL, ONE VM may have N concurrently *entered threads*. An
entered thread holds a VM entry token (§F): registered VMLite +
unique TID (vmstate §6.4.4), GCClient::Heap ACT (heap Dev 8),
VMThreadContext/VMTraps (§A.2), microtask + task queues (§E),
per-entry record (§A.1.5). Cross-thread soundness of PROPERTY/shape
storage is the landed OM/jit/heap machinery (UNGIL-PLAN Part IV);
builtin cell-INTERNAL state is NOT - §N rules it.

- U0 config gate: GIL-off with {useVMLite=0 |
 useSharedAtomStringTable=0 | useSharedGCHeap=0} refused at
 option validation (forced useThreadGIL=1); runtime server
 designation = U0c's ctor CAS.
- U0b multi-VM (heap I13 one-sticky assert KEPT, Heap.cpp:
 4124): GIL-off, exactly
 ONE VM/process - the m_gilOff VM (U0c) - may hold per-thread
 clients. Other VMs: Thread spawn RangeErrors (api 5.1 shape);
 multi-embedder entry keeps the GIL-on single-migrating-client
 + real m_lock protocol (mode per-VM; §F.1/§A.3.6 branch on it;
 storage = §A.1.3's discriminator, r10). Corpus + IU row per
 r22 list.
- U0c m_gilOff assignment (r11/r12; defines F7's input; FULL
 text: history annex U0C, BINDING; r33 compressed):
 vm.m_gilOff set ONCE in the VM ctor, IMMUTABLE; designation
 = the U0C CAS; winner m_gilOff=1 + noteSharedServerSticky();
 loser 0; add:69 trigger STAYS (SUPERSESSION vs heap §5.1
 size-EVER>1, both sides) + gilOffProcess=>server-VM assert.
 ISS-flip(a) discharged; 10D/lite-byte/corpus per annex.

## A. vmstate Phase B - per-thread execution-state consumption

Charter: vmstate:42-48 (Phase B UNOWNED; r12); api §2; jit R1
freeze scope (jit:233). Phase A's frozen layout consumed
unmodified: VMLite::offsetOfPrimitives()/offsetOfTID() + L1-L5 are
ABI; only accessor implementations change (L4).

### A.1 Pinned base + VM::field rerouting

1. TLS base: t_currentVMLite (vmstate L4). C++: VMLite::current().
 Asm/JIT use new emitter loadVMLite per jit App. R5 (per-OS
 mechanisms there; Windows unsupported, inherited).
 No reserved GPR.
2. Mid-body access rule, all tiers: correctness carrier =
 rematerialization - any site needing the lite re-loads via
 loadVMLite; prologue temps + the new VMEntryRecord::m_vmLite slot
 are OPTIMIZATIONS only.
3. Group-3 storage, per mode (DECIDED). THREAD.md:19's Group-3 set
 - topCallFrame, exception state, stack limits (§A.2), scratch
 (rule 6), m_microtaskQueue (§E), lazy regexp stack/match buffers:
 - GIL-on (flag-on OR off): VM storage, emission &vm +
 OBJECT_OFFSETOF - zero codegen delta flag-off (jit I1); U19
 oracle keeps VM storage (J.5/J.6).
 - GIL-off: VMLitePrimitives storage. Mode PER-VM (U0b);
 TWO-LEVEL discriminator (r10, re-freezes history F7 both
 sides): (i) derived JSCConfig byte gilOffProcess (U0c;
 JSCConfig.h:106 M4a slot comment, r25) - false => VM
 storage, one not-taken LLInt branch/site; (ii) true => LLInt
 loads per-lite byte VMLite::gilOff (L2, = vm.m_gilOff) - 0
 => VM storage (second-VM GIL-on intact), 1 => lite storage.
 VM accessors branch on vm.m_gilOff. Baseline/DFG/FTL + §A.1.6
 select emission AT CODEGEN TIME on the COMPILED-FOR VM's
 mode. R1.e's byte + landed ifJSThreadsBranch consumers
 UNCHANGED, both modes (F7). GC root walk, §A.1.5 fan-outs,
 handoff writes, L7 assert: keyed per-VM. Flag-off
 golden-disasm gate RE-BASELINED once. Flag-off identity
 SUPERSESSION (jit I1 jit:180 + vmstate R3 + api I1 vs this +
 §A.2.6, both sides; history r10 F6; r33 compressed) -
 permitted flag-off deltas now ALSO: (a) one not-taken
 gilOffProcess branch per LLInt Group-3 site; (b)
 atomicsWaitImpl's useJSThreads branch; (b2) §N.5's twin
 intrinsics; (c) nothing else - re-audited at U-T14. GC roots
 (full text history r8 item 11; walk r6 F5): registry walk
 under its lock, per-VM filter. Unqualified "gilOff"
 predicates = vm.m_gilOff unless gilOffProcess is NAMED
 (r27). U-T1; amplifiers per annex.
4. stackPointerAtVMEntry/lastStackTop become per-entry-token lite
 fields; JSLock.cpp:166's L7 RELEASE_ASSERT is GIL-on-only; the
 GIL-off token ctor asserts the *lite's* slot empty (re-entry uses
 the VMEntryRecord chain).
5. Per-entry record (entryScope race): m_vm.entryScope
 (VMEntryScope.cpp:90/:133), VM::isEntered() + service bits
 move into the lite (L2); ctor/dtor use the CURRENT lite;
 isEntered() = "§A.3.1 set non-empty"; VM-wide consumers
 iterate the registry under its lock. GIL-on/flag-off
 unchanged. U23. Service routing (mirrors §A.2.3): services
 classify VM-wide vs thread-local (table, U-T1); VM-wide +
 CONCURRENT_SAFE set a VM-level word + fan into this VM's
 lites under the registry lock; thread-local: current lite
 (§F.2 gigacage deferred = VM-wide).
6. Scratch buffers - FULL text: history annex A16 (BINDING;
 r26 ext: §K.6; r33 compressed): process-wide
 ScratchBufferRegistry + per-lite append-only segmented
 tables (L2); baked scratch ADDRESSES become loadVMLite ->
 segment -> [index] (all tiers); JITCode-RESIDENT buffers ->
 per-lite indices (U-T4); §LK re-rank SUPERSESSION vs vmstate
 §6.5.1/§7, both sides. GIL-on/flag-off keeps baked
 addresses; GC-scan via the registry walk (jit R2).
7. Cross-thread Group-3 READERS (history r9 F7;
 SamplingProfiler.cpp:391-431, VMInspector/$vm). NORMATIVE:
 every off-thread reader of a rerouted field (i) resolves the
 TARGET lite via the registry (locked, target suspended),
 (ii) is refused GIL-off with a defined error, or (iii) is
 proven on-thread. v1: SamplingProfiler = (i), carrier lites
 only (spawned unsampled). SUSPEND RULE (r24): a
 (i)-reader, target suspended, allocates NOTHING,
 takes no lock beyond the held registry lock -
 buffers pre-allocated; carved out of §LK.6. U-T8d enumerates
 readers per field (IU) + storm arm.

### A.2 Per-thread VMThreadContext / VMTraps

vmstate §6.8 (per-thread per L2, chained offsets OK):

1. VMLite appends (L2, after Group 6) VMThreadContext threadContext
 + VMTraps traps; generated code reaches lite->traps.m_trapBits
 via the chained offset.
2. Stack limits live in the lite's VMThreadContext, set at thread VM
 entry (handoff migration GIL-on-only; vmstate §2 r3 preserved
 GIL-on).
3. Trap fan-out. VM keeps a VM-level "process traps" word;
 raising a VM-wide trap (termination, GC stop reason - §A.3.8;
 debugger/watchdog bits carrier-only, §A.2.7-8) = under registry
 lock, set the bit in every lite OF THIS VM (§A.1.3 filter) +
 the VM word (token acquisition ORs it in; replaces
 notifyGrabAllLocks()). Per-thread: one lite.
4. Termination is VM-WIDE ONLY v1 (FULL text: history ANNEX
 TERM1, BINDING; r27/r33 compressed): sources watchdog/
 embedder VMTraps/corpus --watchdog; NO
 Thread.prototype.terminate (api 4.1 stands); every
 "terminate" arm = this form, rule-3 fanned to ALL this VM's
 lites. The D9 park-poll predicate re-points at the polling
 thread's PARK lite (spawned = CURRENT; main/embedder = the
 §J.3-captured lite, U31).
5. Async (signal) delivery OFF GIL-off (VMTraps.cpp:305/:80;
 rationale per history): SignalSender never started; delivery
 = bit fan-out (rule 3) + poll sites + D9 quanta; vmIsInactive
 = "no registered lite entered".
6. Sync-park termination wake BYPASSED under useJSThreads, both
 GIL modes (annex A26; r33 compressed): §C.6's per-wait nodes
 (SD6) orphan the VMTraps.cpp:329/:419 vm.syncWaiter() wakes;
 replacement: TA + §C.3 sync parks wait in D9 quanta polling
 termination - U2's bound; U19 terminate-parked arm.
 Flag-off: vanilla-SAB machinery COMPILED AND LIVE;
 atomicsWaitImpl branches on useJSThreads. §T flag-off gates
 incl. terminate-during-TA-wait.
7. Debugger/inspector (r11 F3; pause state singular,
 Debugger.h:342). v1 GIL-off: trap bit carrier-only; Debugger
 entry hooks early-return on a spawned lite (SD13); pause
 keeps the landed carrier protocol; attach/detach +
 CodeBlock-wide recompile walks under a §A.3 stop. N-thread
 debugging post-ungil. GIL-on unchanged. IU row; corpus per
 annex.
8. Watchdog (FULL text: history annex W + W ext, BINDING; r15
 F2; r27/r33 compressed): v1 CARRIER-ONLY. W0 budget
 carrier-only (SD14); W1 parked carrier reacquires EARLY; W2
 last-carrier exit wall-clock; W3 no-carrier tokenless timer;
 W4 the four APILock asserts = §F.2 EXCLUSIVITY CONSUMERS.
 §K.4 routes here (K4 V.1). GIL-on unchanged. IM
 Watchdog.{h,cpp}; U-T2/U-T11 arms per annex.

### A.3 Thread-granular stop-the-world (re-freezes jit R1.c)

Re-freezes jit:233 ("N threads in ONE VM=thread-granular STW"),
both sides. Stub replaced: JSThreadsSafepoint.cpp:244-250
RELEASE_ASSERT; real R1.a-i sequence.

1. Counting unit = entered thread. VMManager tracks per-VM entered
 threads (token holders, §F): forEachEnteredThread(VM&, f) /
 numberOfEnteredThreads = REGISTRY WALKS - the entered SET is
 the VMLiteRegistry (§A.1.3 filter); m_worldLock (heap rank
 3) serializes conductors, owns NO
 membership. EXIT1 (FULL text: history ANNEX EXIT1 as AMENDED
 r32, BINDING; r33 compressed): every sample RE-WALKS the
 registry UNDER VMLiteRegistry::lock (§LK.6); lite/client
 ptrs NEVER cached across samples; lite absent OR TEARDOWN =>
 EXITED; clientHeap null => not-entered; ~VM BLOCKS until
 VM-empty (EXIT1.9). U32; U20; U4 arms.
2. Per-thread NVS park tickets. A stop request sets the stop bit in
 every target lite (§A.2.3); threads park at poll sites (cooperative
 only, jit R1.f-g) on their own ticket; the conductor proceeds when
 every entered thread of every target VM is
 parked/not-entered/access-released (per EXIT1) - sound ONLY
 with 2b:
 2b. Re-acquisition gate. A JSThreads stop sets NO client-visible
 GC stop state (Heap::JSThreadsStopScope only). GIL-off: (i)
 acquireHeapAccess()/attachCurrentThread() polls the lite's stop
 bit; set => park on its NVS ticket until resume; (ii) every
 park site polls post-wake BEFORE re-acquiring access or
 running JS/JIT. (i) carries soundness; (ii) defense. Tokens
 kept while parked (§A.3.4 gates FRESH acquisition). ORDERING
 (r24, FULL text+proof: history ANNEX SB1 as AMENDED by
 EXIT1, BINDING): fan-out stores, conductor samples + polls
 ALL seq_cst; acq/rel UNSOUND. U4 litmus; U20 lint.
 SUPERSESSION (heap §10A "never blocks" + the F8 AHA step list
 vs this, both sides; IH row): GIL-off AHA gains the stop-bit
 gate; park = F8 mandatory-revert BEFORE the NVS park (r9 F3).
 GIL-on/flag-off AHA = frozen F8.
 2c. JIT re-entry sync (r20 F3, FULL text history ANNEX ISB1,
 BINDING; r26/r33 compressed): process-wide seq_cst
 stop-generation counter bumped in-window; may-execute-JIT
 transitions that bypassed NVS exit compare a per-lite copy
 (L2), on mismatch context-sync BEFORE JIT entry.
 SUPERSESSION (jit F5 NVS-exit-only delivery, jit:156 +
 INTEGRATE-heap:608, vs this, both sides; IJ row): delivery
 set WIDENED. Flag-off/GIL-on zero cost. U-T5
 sleep-through-jettison arm (arm64); U20 lint.
3. R1.c re-frozen (conductor release): arbitration releases exactly
 one requesting THREAD as conductor; the park-aware mutex on the
 pending-job slot is keyed by thread; losers PARK during the
 winner's stop, then retry; a SAME-VM second thread participates
 fully. ORDER PIN (r19 ANNEX HBT4, BINDING; ALL §A.3
 conductors): release access -> arbitration -> GCL;
 only the WINNER touches GCL, access-released; losers park on
 the slot, NEVER raw GCL (HBT4.3). SUPERSESSION (jit R1.i
 step order "GCL bracket -> stop", jit:227-234, + jit §7 edge
 "[GCL] > [STWR ownership]", jit:164-167, vs this, both sides;
 IJ rows): the :252-304 bracket reordered AND the :208-221
 restoration comment REWRITTEN at U-T5. Slot mutex ranked
 §LK.4b (U20). [r33] Order EXTENDS to GC-conduct window
 RE-ENTRY (SUPERSESSION, HBT4 single-stop scope vs this, both
 sides; congc CGS2.4(b)/§13.5(3) back-cite; HBT4.1 in-place
 [r33] marker; FULL: CGS2A.4(b), [r34] compressed): legal
 because access-released all tenure (congc
 §3.1(a)-(b)/CG-I19); F15 carve-out unchanged; election/poll
 tryLock-only; flag-gated, flag-off frozen (congc CG-I0).
4. Entry during a stop parks: token acquisition (§F) checks the
 stop word, parks on a fresh ticket before completing entry.
 Licensed deletions: JSThreadsSafepoint.cpp stub assert +
 evidence walk + s_stubWorldStoppedDepth; M7 tripwire
 VMEntryScope.cpp:44-70.
5. R1.i GC bracket (W5): DEFAULT = access-release -> rule-3
 arbitration -> Heap::JSThreadsStopScope (:252-304, GCL AFTER
 arbitration, HBT4.5), client-scoped, own client -
 "allocation-free closure" STRUCK [r33] (congc F43/CGS2.4(a),
 both sides; HBT4.2 [r33] strike): the conductor is
 a FULL CLIENT in-window (AB-21/AB-10/HBT2.1 class-4;
 CGS2A.4(a), [r34] compressed). [r33] STOP-SCOPE PAUSE (congc
 CGS2.4(a)/§9.1(2), both sides; FULL: CGS2A.4(a)):
 BOTH ctors (Heap.cpp:5546-5566 blocking;
 :5568-5590 watchdog tryLock-poll), GCL held, phase-gated
 pause BLOCK; dtor (:5592-5596) resumes BEFORE releasing GCL;
 no api-rank/heap >= 7 lock (CG-I16). [r34] WATCHDOG COVERAGE
 (CGS2A.4(a) [r34]; PENDING-CONGC-COUNTERPART; FULL: REV 34):
 pause = TIMED wait sampling per quantum (requestStart, vm);
 blocking ctor gains requestStart
 (JSThreadsSafepoint.cpp:445); watchdog ctor threads
 vm (:5586); CG-T8 owes a wedged-marker arm. [r35] SHIP GATE
 (REV 35): C1/§9.1(2)-pause stages MUST NOT ship until congc
 records F-A (1)+(4) (CGS2.4(a)/CGT1).
 WAIT BOUND (congc CGS2.3 + CGS2A.3
 [r34/r35], both sides; supersedes U32/HBT4.5 whole-conduct):
 the CGS2.3 windowed sum (terms: CGS2A.3) bounds ONE WINNER's
 GCL leg, STRUCTURAL via congc §9.1(2a)/CG-I26; the landed
 budget is per-REQUESTER end-to-end (VMManager.cpp:556-566),
 ADDS a QUEUE term (k earlier winners x (GCL leg + full stop
 window)) - [r35] COUNT bound ('supported fan-in' RETIRED,
 undefined); SOLE time bound = the 30s fail-stop
 (JSThreadsSafepoint.cpp:512; CG-T8: per-winner arm
 + ATTRIBUTION storm arm); waiting conductor keeps EXIT1/U32
 sampling. [r33-r35] flag-gated
 useConcurrentSharedGCMarking; flag-off the frozen rule-5
 text (incl. allocation-free) operative (congc CG-I0); FULL:
 ANNEX CGS2A (+[r34]/[r35]).
 r17/r18 F1 FULL: ANNEXES HBT2-HBT4
 (BINDING; r27/r33 records). (i) CLASS-4
 variant - SUPERSESSION (heap §9 JSThreadsStopScope precond +
 jit R1.i closures vs this, §K.5 conductors ONLY, both sides;
 IH/IJ rows): GCL access-RELEASED, own-client gated
 re-acquire pre-fan; access RETAINED, alloc legal
 in-window. (ii) NO-GC-IN-WINDOW, ALL §A.3 windows -
 SUPERSESSION (heap §9 CSAC/RCAC vs this, both sides):
 slow paths enqueue RCAC post-resume;
 alloc = heap L3 pre-grow or fail hard; heap I14 brackets.
6. Main/embedder carriers (vmstate §6.4.4). FULL text: annexes
 A36 (as AMENDED r32) + A36C (BINDING; r9 F4; SUPERSESSIONs vs
 vmstate §6.7/M6/§6.5.1 + heap I4/§10A.1, both sides; r33
 compressed): GIL-off every thread uses a real carrier lite
 (lazy, per-(thread,VM) TLS); lock() installs/swaps the
 {lite, TID-tag, §10A.1 client} tuple, LIFO (§F.5);
 installs/restores re-stamp currentThreadClient(),
 {client,epoch}-checked (A36C). U1; JSLock.cpp:151 backstop =
 §J.7; ~VM: M6 per annex. I20 holds. U27 arms.
7. Atom-table routing (X1). GIL-off, token acquisition points the
 thread at the shared sharded table (U0); the per-handoff swap
 is GIL-on-only. SUPERSESSION (vmstate §4.3 "None relaxed
 (ex-M5)" vs this; r33 compressed): the 14 atom-table asserts
 (Identifier.cpp:77; Completion.cpp:63-287 x12; Heap.cpp:2796,
 r16 F4) rewrite predicate P to "gilOff ?
 sharedAtomStringTableEnabled() : P"; :2796 KEEPS its
 worldIsStoppedForAllClients() disjunct GIL-on.
 GIL-on/flag-off unchanged; IU row.
8. GC-stop parking, N threads one VM (closes heap Dev 8,
 heap:27; SUPERSESSION vs heap §13.5 one-parked-thread-per-VM
 + notifyVMStop per-VM state machine, both sides; r33
 compressed). GC stop reason THREAD-granular: trap bit fans
 per rule 3; EACH entered thread parks on its OWN ticket
 (§A.3.2 NVS); §13.5a/g willPark/didResume on
 currentThreadClient(), per-client m_releasedByGCPark;
 5b/5f/5g per thread. Unlike §A.3, the GC stop DOES set
 client-visible stop state (§10A/F8 gates re-acquisition).
 IM: VMManager.cpp + §13.5 hooks (IH). Amplifier per annex.

## B. Per-thread GCClient lifecycle in one VM

Charter: heap Dev 8 (ONE GCClient PER Thread); Dev 7 (full list:
B.6).

1. Create at spawn. threadMain (ThreadObject.cpp:162-176),
 GIL-off: after lite registration/setCurrent + TID-tag
 handshake, BEFORE any allocation, construct the thread's
 GCClient::Heap (ACT), store
 clientHeap in the lite (L2), acquire access (§A.3.2b-gated).
 JSLockHolder degrades to the §F token.
2. Teardown at exit (EXIT1.3/1.9 as AMENDED r32; r33
 compressed). T5: release access (seq_cst RHA) -> TEARDOWN
 mark (registry lock; counted EXITED) -> DCT/destroy client
 -> unregisterLite/free lite LAST; ALL physical removals via
 unregisterLite (U20). ~VM BLOCKS (EXIT1.9 NORMATIVE fence;
 U3/U32) until no other lite->vm==this. Exit stays UN-GATED;
 same order on LIVE-VM paths - carrier TLS death; ~VM carrier
 collection EXCLUDED (A36 as AMENDED r32:
 LIVE->COLLECTED->DETACHED protocol, full text annex). Lazy
 carriers per §F.1.
3. SUPERSESSION (heap §10A ISS forward-to-main-client wiring,
 SPEC-heap.md:281, vs §F.1, both sides; IH row): GIL-off the
 JSLock pair acquires/releases on the CURRENT carrier's OWN
 client (§F.1) - NEVER the main client (heap Dev 8).
 GIL-on/flag-off forwarding + §10A.1 re-stamp unchanged;
 GIL-off the duty = the §A.3.6 swap (A36C, both sides, IH
 row); U-T6/U27 two-VM + nested arms.
4. TLC-aware inline allocation: fast paths address
 lite->clientHeap's TLC table, base = loadVMLite + frozen offsets;
 the §5.3 vm-relative chain stays GIL-on (heap Dev 6).
5. Perf budget (heap Dev 7): {useJSThreads=1, sharedGC=1,
 GIL-off, 1 thread} composite <=10% geomean vs {1,0} flag-on
 baseline (BENCH.md); {1,0} <=5% gate stays; a miss REQUIRES
 jit §4.3 LLInt-cache revival pre-ship. 4-thread alloc
 microbench >=2.5x recorded, not gated (history).
6. heap Dev-7 GC-throughput items (heap:26 list) -
 SUPERSESSION (heap:26 + api:26 vs this): DEFERRED post-ungil;
 GIL-off ships on the conductor-driven heap §10 protocol +
 single-MSPL slow path. Gate = §B.5; a miss pulls them forward.
 IH row.

## C. api Dev 12 / OM 8g re-freeze

Charter: api:22; OM 8g; INTEGRATE-api D1/D2/D4/D8/D12. IS rev-15
content (IA sign-off).

1. OM §9.5 atomic slot accessors (8g): atomicSlotCompareExchange
 / atomicSlotReadModifyWrite -> JSValue, ONLY plain structure/
 butterfly-backed own NAMED data slots + the indexed pair.
 FULL text: history annex C1 (BINDING; r25 ext; r27/r33
 compressed): lock-free seq_cst (U5); flat SW-set DCAS + I34
 re-validate FIRST; OM-locked dict/AS; indexed by shape (C1);
 barrier after success. U5/U28 arms.
2. ThreadAtomics re-homing (UNGIL-PLAN P1): the GIL-step
 atomicity block replaced - bodies call the §9.5 accessors.
 CARRIED: D3 exotic-receiver TypeErrors; D7 writability inside
 the atomic body.
3. PWT arming re-home + I10 re-derivation (F4 GIL-off). FULL
 text: history r9 F1 + annex C3 (BINDING; r33 compressed):
 pre-enqueue validation via §9.5 atomic load; enqueue + SVZ
 re-validation UNDER listLock; mismatch => dequeue
 "not-equal"; rope/convert => dequeue (I10), resolve outside,
 FRESH enqueue. waitAsync settles via §E.4 (finite timeout:
 §E.7.5); sync parks per §J.3. U5/U-T11; corpus per annex.
 GIL-on unchanged.
4. 4.5-1a TA gate lifted GIL-OFF ONLY - SUPERSESSION (api I21
 :315 + api:79 vs this, both sides; IU row): the sole
 spawned gate, AtomicsObject.cpp:613-621, becomes
 vm.m_gilOff-conditional - KEPT GIL-on (SD4); no twin.
 ThreadAtomics.cpp:536-541 NOT 4.5-1a: G11 property-wait gate
 KEPT, re-pointed §G.2. Post-lift blocking = §G-only.
5. D2 notify-yield: GIL-off notify() is NOT a yield point -
 jsThreadGILHandoffYield is GIL-on-only (§J.4); no foreign JS in
 notify(); parallel waiters (SD5).
6. D4/D8 lifted together (IA): atomicsWaitImpl's sync path
 allocates a per-wait node instead of the single vm.syncWaiter();
 the D8 single-flight gate deleted in BOTH GIL modes - SD6.
 Nodes park per §A.2.6 (D9 quanta; flag-off keeps central
 wakes).
7. D1 ruling - §F.4. D12: grants settle via §E routing on the
 registering thread; uniform (closed).

## D. OM Tasks 13 (TID rebias) + 14

1. Task 13 (om:377, 8c) - IN SCOPE. FULL text: history annexes
 D1 + D1R (BINDING; r27/r33 compressed): rebias world-stopped
 INSIDE the next FULL shared collection (heap §10, NOT §A.3);
 restamp dead TIDs->0, fire restamped TTL sets in-stop,
 jettison baked tid<<48 immediates pre-reissue; SUPERSESSION
 (jit/OM I13 VMM-STW-only fire vs this, both sides). Trigger
 >=75% of 2^15; exhaustion spawns RangeError (SD9). Two-phase
 vs §LK (r9 F2): dead-TID snapshot under TM::m_lock pre-stop;
 restamp+fire FROM SNAPSHOT. U-T12 arms per annex.
2. Task 14 (om:378) STAYS DEFERRED pending the bench verdict -
 timing SUPERSESSION (both sides; INTEGRATE-om §46 holds NO
 verdict): the gate re-times to a HARD precondition of U-T10
 ENTRY. PROMOTE => lands before U-T10, §C third arm
 re-reviewed pre-code; else 8h ships as landed (OM 8h/L6/I37).

## E. Per-thread event loop + settlement (THREAD.md:98)

Ground truth replaced (api 4.6.1 GPO drain; DWT settlement).
Landed inert: inboxLock/inbox/inboxOpen, per-lite microtask
slot (vmstate §6.6), I11. SUPERSESSION (both sides; IA row):
api 4.6.1 never-waits + 4.6.2 SHELL-granular keepalive
SUPERSEDED GIL-off by §E.2/E.3 (thread-granular); GIL-on keeps
the old text (SD1).

### E.1 Queues

Every ThreadState owns, GIL-off:
- Microtask queue: the per-lite MicrotaskQueue (vmstate §6.6),
 enqueued/drained ONLY by its owner (I11); VM::queueMicrotask
 + drains re-route to the CURRENT lite's queue.
- inboxOpen (landed default false): set true EXACTLY ONCE on the
 owning spawned thread, under inboxLock, post-§B.1 attach,
 BEFORE fn (HB vs any registration; r22 list). Main/embedder
 NEVER open theirs; increment sites assert spawned+OPEN (U25).
- Host hook (X1.7): queueMicrotaskToEventLoop consulted ONLY for
 carrier enqueues; spawned ALWAYS per-lite (I11/U22; r22 list).
- Task (macrotask) queue: TS fields under the EXISTING inboxLock
 (api rank 3): taskQueue, keepaliveCount, runLoopCondition,
 waitDeadlines (§C.3/§E.7.5); ThreadTask = settle task +
 Ref<AsyncTicket>; the landed inbox vector IS the task queue.

### E.1b Ordinary shared-promise settlement (NEW)

E.4 routes only AsyncTickets; under the shared heap ANY thread can
resolve an ordinary JSPromise whose .then() registered elsewhere.
NORMATIVE v1:
1. Reaction jobs run on the SETTLING thread: the resolver enqueues
 to ITS OWN per-lite queue via the rerouted VM::queueMicrotask -
 I11; no per-reaction registrant hop v1. SD10.
2. Concurrent then()/resolve(): GIL-off, JSPromise internal-state
 transitions under the promise's JSCellLock (10a); bodies per
 OM I20. FULL text: history annex E1B (BINDING; r33
 compressed). U-T9 audit: every other promise internal-field
 writer/tier-inlined access locks or is disabled GIL-off;
 non-promise = §N. GIL-on unchanged.
3. U22: reactions on the settling thread; AsyncTicket settlements
 on the REGISTERING thread (ThreadTask hops, §E.4).
4. promiseRejectionTracker (JSPromise.cpp:405-637) - r16 F3,
 FULL text history (BINDING; r33 compressed): inline for
 carriers; spawned events append Strong+op records to the
 annex-E7 handoff queue, run at §F.1 carrier drains (SD15).
 U-T8e audit (gates U-T9): every globalObjectMethodTable/host
 hook JS-reachable on a spawned TS gets an IU disposition.
 Corpus per annex.
5. AsyncLocalStorage (Bun; FULL text+cites annex ALS1, BINDING;
 r27/r33 compressed): SD10 migrating continuations PRESERVE
 ALS - capture PER-REACTION into the reaction's tuple/job
 arg; runner swap/restores. The CURSOR m_asyncContextData
 (JSGlobalObject.h:507) = GIL-off per-lite (§K.1; ALS1.3).
 U-T9 arm: ALS inside await after foreign resolve ==
 registration store.

### E.2 Thread lifecycle (normative drain loop)

FULL text (pseudocode VERBATIM): history annex E2A (BINDING;
r27/r33 compressed): after fn, loop = drain-own-microtasks ->
release access -> under inboxLock {termination => close (§E.5)
| take task | keepalive==0 => close | wait runLoopCondition D9
quanta} -> post-wake §A.3.2b poll + reacquire -> EXPIRE
deadlines (§E.4 "timed-out") -> run task (§F token). close
(r16 F5): inboxOpen=false, residue/deadline exchange under
inboxLock access-released; F1/F5 as landed; T5 per §B.2
(EXIT1.3/1.9).

Lock/access rule. Heap-access transitions are NOT leaf: NO
transition holding any api rank 1-3 lock - release BEFORE,
re-acquire AFTER (ditto §J.3 park sites). RANK-4 EXEMPTION (api
5.9(e), api:271): NLS::m_lock/ParkingLot MAY span token+access
(re)acquisition - block ONLY while both RELEASED, then
(re)acquire gated holding m_lock (§LK long-hold). U20
lints order.

Thread completes - and join/asyncJoin settle (F5) - ONLY at close
(U7), not fn-return (SD1). Park sites inside fn do NOT service the
task queue. Wakeups: task append, stop, termination, quantum.

### E.3 Keepalive accounting

FULL text: history annex E3 (BINDING; r33 compressed):
keepaliveCount = outstanding registrations that may still
enqueue here; transitions under the registrant's inboxLock;
INCREMENT at registration (I20) except asyncJoin (SD12)/TA
waitAsync (SD11); DECREMENT exactly-once CAS-gated; close
kills the counter - later => main fallback (U8). U9: decrement
+ append atomic under inboxLock. Intentional leak (api 4.6.2);
§E.5 escapes. U8 mutual-asyncJoin-OPEN arm.

### E.4 Cross-thread ticket settlement routing

Implements api:200's open arm; the closed arm is SUPERSEDED
(r17 F6; api 5.5 :200 main-inbox append vs this + §E.1, both
sides; IA row).
AsyncTicket::settle GIL-off: CAS m_settled (as landed);
cancelled => bail; under m_registrant->inboxLock READ
inboxOpen; open => append ThreadTask, rule-1 decrement,
notifyOne; DROP inboxLock; closed => FALLBACK to MAIN via the
LANDED scheduleWorkSoon path AFTER the drop, NO api lock held
(r18 F2; §E.7.3-4 apply).
PRECONDITION (r17 F2, history BINDING, incl. the api 5.5a/F5
SUPERSESSION, both sides; IA row; r33 compressed): settle holds
NO api rank-1..3 lock - record grant under QL, DROP QL, settle;
asyncJoin drops joinLock pre-settle. U-T8 IU settle-site table;
U20 lints rank-3 settles.

DWT retirement on the task-queue path (ThreadManager.cpp:88-95):
ThreadTask body, on the owner under its token: settle ->
cancelPendingWork (§E.7.4 wake) -> clear m_promise. Thread
keepalive supersedes DWT shell-liveness for spawned registrants;
dead=>main keeps landed retirement. U24.

I11/I12 satisfied post-GIL. join() parks unchanged;
GILDroppedSection out (§J.3); §G gates the block.

### E.5 Termination

A termination trap observed by the E.2 loop (or during fn) takes
the landed Failed path VIA THE §E.2 CLOSE BLOCK - incl. its
deadline harvest (SD8): close inbox, residue to main, F1/F5 with
Phase::Failed. A terminated thread completes with keepalive>0;
tickets settle later via main fallback (4.6.2). Per-lite
microtask residue DROPPED at close (I11) - SD17; published
settlements stay visible. Termination = the §A.2.4 VM-wide trap
(TERM1): EVERY entered thread takes its OWN close; the VM
SURVIVES (carrier services it, annex W). Failed publishes a
FRESH ordinary Error("Thread terminated") - NEVER the sticky
m_terminationException (SD8 ext2); main fallback =
scheduleWorkSoon at carrier re-entry. U-T11 arms per annex.

### E.7 DeferredWorkTimer under N threads (NEW)

m_pendingTickets is JSLock-serialized today. NORMATIVE
GIL-off:
1. m_pendingTickets (+ other JSLock-serialized DWT state) gains
 Lock m_pendingLock, rank LEAF (§LK; never across user JS):
 add/cancel/hasPendingWork + peers, shutdown walk;
 cross-thread cancel (E.4) safe.
2. DWT's API-lock asserts keep the §F.2 token meaning - incl. the
 NEGATIVE assert at runRunLoop.
3. Embedder-hook ruling (USE_BUN_EVENT_LOOP; FULL mechanics:
 history r8 + E7 + r17 F3 + r18 F2, all BINDING; r33
 compressed): hooks ONLY for hookManaged tickets ONLY on the
 carrier; spawned ALWAYS internal arm; off-carrier
 settle/cancel = m_pendingLock handoff queue run at §F.1
 carrier drains; wake AFTER dropping m_pendingLock. U24 Bun
 arms per annex.
4. No-hooks runloop wake: internal-arm cancel/retire while
 m_shouldStopRunLoopWhenAllTicketsFinish dispatches an ON-loop
 re-check via vm.runLoop().dispatch() AFTER dropping
 m_pendingLock (r17 F3); emptiness reads under
 m_pendingLock. U24 shell arm.
5. vm.runLoop()-bound paths (api 5.5a schedPump P, G28, + the
 5.6 waitAsync finite-timeout timer) route BY REGISTRANT (r10
 F3; r33 compressed). SPAWNED: P INLINE on the notifying
 thread (no JS); 5.6 timer = waitDeadlines on the registrant
 TS - SD16 (r18 F4, history BINDING, incl. the api 4.5/5.6
 SUPERSESSION): timed-out settles ONLY at registrant
 drain/close; spawned work NEVER via carrier drains or
 vm.runLoop(). MAIN/EMBEDDER: hooks => rule 3; else landed.
 Corpus per annex.

§E.7.1's m_pendingLock = in-tree DWT::m_taskLock, EXTENDED to
m_pendingTickets (r26; K4 VII.4).

## F. Post-GIL API-lock contract

1. JSLock GIL-off mode. JSLock::lock() branches on mode+caller:
 - Spawned Thread: NO m_lock. Installs an entry token {depth,
 spAtEntry} in the VMLite - records sp/lastStackTop (§A.1.4),
 ORs the VM trap + service words in, acquires client heap
 access (§B.1, §A.3.2b-gated), bumps depth; unlock()
 symmetric; depth 0 releases access. JSLockHolder = token.
 - Main/embedder: REAL lock - m_lock still mutually excludes
 embedder threads (Bun exclusion kept); FULL text: history
 annex F1B (BINDING; r33 compressed): lock() = entry token +
 §A.3.6 swap; FIRST entry creates carrier lite/client/ACT;
 EVERY lock() = gated AHA on THAT client; depth-0 unlock
 releases; drain-on-release KEPT (I11). U27/U-T6 negative
 arm.
2. Two predicates, split.
 - VM::currentThreadIsHoldingAPILock() REDEFINED GIL-off as
 "current thread holds an entry token for this VM" - the host-call
 assert meaning (DWT §E.7.2).
 - JSLock::currentThreadIsHoldingLock() stays MUTEX-LITERAL -
 §F.4's DAL no-op + m_lockDropDepth LIFO depend on it. Spawned
 unlock() takes the token branch BEFORE the mutex RELEASE_ASSERT.
 - Consumer audit (U-T8): the ~60 consumers of either predicate
 get an IU table - assert / BRANCH / EXCLUSIVITY CONSUMER.
 Fixed rulings: FULL text history annex F2 (BINDING; six
 named sites; r33 compressed).
3. Strong-handle discipline (api 5.10). ONE shared HandleSet per
 VM, new leaf HandleSet::m_strongLock inside Strong allocate/
 free/set-slot only (never across user code). Mutation needs
 an entered thread WITH heap access; GC scans the set under
 the heap §10 stop (NOT §A.3). Carve-outs (r10 F1): (a)
 in-lock-sweep Strong FREES under MSPL/BVL/9b legal -
 m_strongLock destructor-leaf (§LK.8); (b) heap finalizers
 clearing Strongs run entered-with-access OUTSIDE the stop
 window (10B(5); conductor: post-resume, own client). U-T7
 sweep-storm amplifier.
4. DropAllLocks GIL-off (IA D1; r20 F1, FULL text history ANNEX
 DAL2, BINDING; r26/r33 compressed). Main/embedder: drops
 m_lock + token. Spawned: a HEAP-ACCESS bracket - ctor
 releases client access; dtor re-acquires §A.3.2b/§A.3.8-gated
 + polls traps; outermost-only. SUPERSESSION (D1 phase-1
 no-DAL constraint, INTEGRATE-api.md:834-847, vs this, both
 sides; IU row): LIFTED GIL-off, KEPT GIL-on. Ctor/dtor are
 access transitions (§E.2 rule; U20). Park sites still §J.3.
 U14 re-derived; U24 arm per annex.
5. Nested foreign-VM entry (r10 F2; owns §A.3.6's nested window;
 U30). CALLER SCOPE (r27, TERM1.5): main/embedder carriers
 ONLY - SPAWNED foreign-VM lock() RELEASE_ASSERTs (A36
 single-VM v1; §F.6(e); not an SD; r10 F2's option-(b)
 rejection = carrier-side).
 lock() on VM B holding VM A's token FIRST releases A's
 client heap access BEFORE installing
 B's carrier: in the nested window T counts access-released
 for A's heap §10.4 barrier AND the §A.3.2 conductor
 predicate (heap I4(b)). A's trap/stop/termination delivery
 DEFERRED to LIFO restore: re-acquires A's access gated,
 re-stamps A's client (A36C), polls A's bits before any A JS.
 U2 re-scoped per-VM; not an SD. U-T6 nested-GC arm. IH/IV
 rows.
6. Embedder contract (r17 F4 + r20 F1 + annex EC1, history;
 BINDING; r26 compressed). Five GIL-off deltas on embedder
 (Bun) code, NORMATIVE: (a) m_lock excludes only embedder
 threads (§F.1); (b) embedder-REGISTERED ordinary-promise
 reactions settled by a spawned thread run on the settler
 (SD10/§E.1b.1, X1.7) - off m_lock/loop; (c) spawned-thread
 blocking native sections using NEITHER §F.4 DAL NOR §J.3
 must RHA/AHA-bracket per heap §9 (DAL2.5); (d) FIRST-VM-WINS
 (U0c/EC1): the FIRST VM CONSTRUCTED wins the ctor CAS = sole
 spawn-capable VM (U0b); construct the main VM strictly first
 + boot-assert m_gilOff==1; (e) native code on a spawned
 Thread never enters/creates another VM (§F.5
 RELEASE_ASSERT; A36). IU row = embedder checklist (audits
 per r21 list). Sign-off SPLIT (r21 F2, §D.2 shape): (b)'s
 SD10 disposition = HARD precondition of U-T9 ENTRY;
 (a)/(c)/(e) stay U-T14 close items.

## G. Per-thread blocking policy

Replaces the per-VM G11 gate (jsThreadsCanBlockOnCurrentThread).
1. Per-THREAD predicate mayBlockSynchronously(): spawned TS = true;
 main/embedder = embedder policy
 (isAtomicsWaitAllowedOnCurrentThread()).
2. Governs ALL sync parks: TA/property Atomics.wait (KEPT G11
 gate, §C.4), join, contended lock.hold, cond.wait; violations
 throw the existing TypeErrors (api I18 intact).
3. D4 GIL-dropped main TA wait machinery is GIL-on-only; GIL-off a
 permitted main sync wait parks per §J.3. D8 per §C.6.

## H. SymbolRegistry / Symbol.for

Closes vmstate Dev 8: WTF::SymbolRegistry's m_table gains Lock
m_lock (destructor-leaf, §LK.8) - symbolForKey, remove, dtor
walk; ~StringImpl's registered-symbol arm calls remove() under
it (any thread, incl. in-lock sweep); registries destroyed in
~VM after spawned exit (U16).

## I. Wasm on spawned threads - REFUSED in v1

Closes UNGIL-PLAN I. Wasm EXECUTION from a spawned Thread throws
TypeError (WARM calls incl.). NORMATIVE (both GIL modes, SD7):
(1) the WebAssembly ctor/compile surface throws on a spawned TS
(list: r22); (2) under useJSThreads, jsCallICEntrypoint()
returns nullptr AND every generated JSToWasm entry emits a
spawned-TS prologue check. Discriminator: L2-append uint8_t
VMLite::isSpawned (=1 BEFORE setCurrent, §B.1); check =
loadVMLite, null => fall through, else throw - NOT TID-tag.
isSpawned: SPAWN lites only (carriers 0); the CURRENT-lite byte
always agrees with isJSThreadCurrent() (§F.5; ditto §A.2.7).
U17 negative arm: carrier non-GC wasm never throws. C++ gates
keep isJSThreadCurrent(); GIL-on corpus edited (SD7). EXECUTION
only; §N.6 rules wasm buffers. Wasm-GC: hasGCObjectTypes()
precheck => LinkError, both GIL modes - SUPERSESSION (heap
§5.5/manifest 11, both sides; FULL text: r9 F8 + r22 list; r33
compressed). U17 positive arm. IU row.

## J. GIL-machinery end state (GIL-on unchanged - oracle)

- J.1 useThreadGIL: KEPT, supported fallback; default flips false
 at the milestone gate.
- J.2/J.4/J.5 jsThreadGILHandoffYield + D2 notify-yield (§C.5),
 GILParkSavedExecutionState + resetForFreshThread: dead GIL-off
 (state per-lite, §A.1); J.5 compiled out;
 unlockAllForThreadParking NOT dead - re-derived J.3.
- J.3 GILDroppedSection, GIL-off by caller (FULL text: history
 r10 F5, U31 - BINDING; r33 compressed). Spawned = access
 release + §A.3 park cooperation + post-wake poll;
 main/embedder park sites ALSO release m_lock + token. Wakes
 poll ONLY lock-free state off the pre-captured lite; full
 reacquisition EXACTLY ONCE per episode, after all rank-3
 locks released (§E.2 exemption). C-API arm per annex.
- J.6 JSLock handoff body: §F.1 drain + §A.3.6 swap KEPT; rest
 skipped per §§A/B/E/F (GIL-on load-bearing).
- J.7 JSLock.cpp:151 backstop (L1): REPLACED - GIL-off branch
 RELEASE_ASSERTs U1.
- J.8 Stub witnesses W2/W3/W4 + OM stub witness: DELETED at U-T5,
 both modes.

## K. GIL-serialized VM/global caches + lazy init (NEW)

The GIL is today's ONLY serializer for VM-/JSGlobalObject-resident
mutable state outside Group 3. Rulings (GIL-on/flag-off
unchanged):
1. Per-lite duplicates (L2), hot per-op scratch/caches
 (BINDING list: annex K4 §II). GIL-off accessors route to the
 CURRENT lite's copy; cell-holding copies GC-scanned via the
 registry walk.
2. Leaf locks: cold/keyed VM caches whose hits must be shared
 (BINDING list: annex K4 §III). RegExpCache ALREADY locked
 (RegExpCache.h:79); unlocked peers get a leaf Lock (§LK).
3. Atomic lazy publication (FULL text: history r25 ext + r16
 F2 + ANNEXES LZ1+LZ2, all BINDING; r27/r33 compressed):
 first-touch load-acquire fast path; CAS records the OWNER;
 winner inits lock-free, release-stores; owner re-entry null;
 foreign threads wait PARK-CAPABLE bounded (not an SD);
 covers initLater + VM ensure*. LZ1 cycle escape/abandonment;
 LZ2 first-touch PRECONDITION. U-T8b columns; U20 per LZ2.4;
 U26 arms per LZ1.4+LZ2.4.
4. Inventory audit U-T8b EXECUTED -> SPEC-ungil-audit-K4.md
 (BINDING annex K4; rows K4.*). Implementation CONSUMES the
 tables verbatim; §F.2 EXCLUSIVITY CONSUMERS
 cite K4 rows. Residue rulings: §K.6. U26.
5. Class 4 - requires-stop (r16 F1): GIL-serialized writers that
 iterate/rewrite OTHER threads' objects.
 JSGlobalObject::haveABadTime (:2900; JS-reachable :2460) -
 FULL text: history annex HBT as AMENDED by HBT2-HBT4 (all
 BINDING; r33 compressed): whole body under ONE §A.3 stop;
 conductor = caller, own client, §A.3.5 CLASS-4 variant;
 re-check post-arbitration; re-entry blocked; jettison jit
 I2/R1 (:2854). GIL-on unchanged. Peers route here (annex K4
 §VI). Corpus per annexes (U-T13).
6. r26 audit-residue rulings (FULL text: history ANNEX AUD1,
 BINDING; rows re-ruled in annex K4 §0; r27/r33 compressed):
 K4-U1 SamplingProfiler = SD18; K4-U2(=N7-U7)
 m_regExpGlobalData per-lite = SD19 (tiers per A16 ext);
 K4-U4 = ANNEX A16 EXTENDED; K4-U3 modules per AUD1.K3;
 K4-U5/U6/U7 MECHANICAL (annex K4 §0).

## N. Builtin cell-internal mutable state (NEW)

OM §9.5/I21 cover PROPERTY slots; §K covers VM/JSGlobalObject
members. Multi-word C++/internal-field state INSIDE other
shareable cells was unruled. DEFAULT GIL-off protocol
(GIL-on/flag-off unchanged): mutations AND structure-traversing
reads under the cell's JSCellLock (10a), §E.1b shape - allocate
OUTSIDE, re-validate under, never allocate/park holding it (OM
I20). Rulings (full args: history):
1. JSMap/JSSet (JSOrderedHashTable) + JSWeakMap/Set
 (WeakMapImpl): ALL ops cell-locked, reads too. DFG/FTL map
 intrinsics DISABLED GIL-off -> locked native bodies; revival
 post-ungil.
2. Rope resolution/atomization (JSString.h:637-682): lock-FREE -
 resolver computes into a fresh buffer, publishes by ONE
 release-CAS of the fiber0/flags word; losers discard +
 re-read; readers load-acquire; resolveRopeToAtomString same
 vs the shared table (U0); §C.3's resolve = this.
3. DateInstance GregorianDateTime cache (DateInstance.h:62-75):
 BYPASSED GIL-off; m_data lazy alloc CAS-published;
 vm.dateCache = §K.1/2.
4. FunctionRareData (JSFunction.h:136-144): materialize per §K.3;
 internals mutate under the function's cell lock; profiling-only
 fields racy-tolerated (jit item 7); cached Structures per I34.
5. Non-promise JSInternalFieldObjectImpl (generators, async
 fns/generators, iterator helpers; FULL text: r11 + r15 F1 +
 r17 F5 + r25 ext history, all BINDING; claim sites per
 annex; r26/r33 compressed): single-word resume-claim CAS
 SuspendedX->Running on the STATE field; failure dispatches
 on a RE-READ (no SD); interior stores WHILE claimed stay
 PLAIN + tier-inlined. Twin intrinsics
 @atomicInternalFieldClaim/@atomicInternalFieldPublish,
 emitted UNCONDITIONALLY all modes; LOWERING mode-keyed (r17
 F5). Flag-off: §A.1.3 (b2) uniform bytecode; golden gates
 re-baselined. Annex N7 lists claim+publish sites; cell lock
 ONLY for named multi-word cases. BENCH.md + §B.5 gates;
 amplifiers per annex.
6. ArrayBuffer detach/transfer/resize (ArrayBuffer.h:199/:298) +
 wasm grow. FULL text + torn-pair table: annex N6 (BINDING;
 r14 AMENDS arm 2; r25 ext; r33 compressed). PRINCIPLE (r12
 F2): a racing reader never pairs a passing length with an
 unmapped-or-short base. DETACH len=0 seq_cst + flag, contents
 QUARANTINED to a heap §10 stop; TRANSFER = COPY + source
 detach; SHRINK tail free deferred; GROW base IMMUTABLE -
 commit THEN release-publish length. U28 amplifier per annex.
7. Audit U-T8c EXECUTED -> SPEC-ungil-audit-N7.md (BINDING
 annex N7; rows R1-R31). Implementation CONSUMES the N7 table
 verbatim (§IM: IU adds call sites); tier-inlined accesses
 disabled or
 re-pointed per row. Residue rulings: §N.9. U28 arms per
 annex N7.
8. ScriptExecutable -> CodeBlock FIRST install (r20 F4, FULL
 text history ANNEX CBI, BINDING; r26/r33 compressed):
 compile OUTSIDE any cell lock; publish = release-CAS of
 m_codeBlockFor{Call,Construct}, loser discards/adopts;
 adjacent state per-field ruled (annex); tier-up jit §5.7.2
 verbatim. No frozen text superseded. Annex N7 row R12 +
 first-call amplifier.
9. r26 audit-residue rulings (FULL text: history ANNEX AUD1,
 BINDING; rows re-ruled in annex N7 §0; r27/r33 compressed):
 N7-U1 m_resolutionCache = §N cell lock (UAF, PRIORITY);
 N7-U2 RegExp::m_ovector = per-lite match buffer (K4 §I; UAF,
 PRIORITY); N7-U3/U4 arguments CAS-PUBLISH (flags
 release-stored AFTER the OM puts); N7-U5 StructureRareData
 under Structure::m_lock, m_specialPropertyCache = §K.3;
 N7-U6 Intl cell-locked/ICU const-or-clone; no SD.

## LK. Merged process lock table (ONE order; heap §6 master; api
§5.9 anchored here; vmstate §7 amended; acyclicity: history r8
+ WS1 + NLH1)

Outermost -> innermost:
1. heap rank 1: JSLock::m_lock / entry token / heap access (token
 ordering-inert; "held entering NVS" per thread).
2. api 1: TM::m_lock. 3. api 2: PWT::m_lock / ThreadAffinityTable
 (never both).
4. api 3 group (mutually unnested, api 5.9(d)): NCS::queueLock,
 NLS::m_queueLock, listLock, TS::inboxLock (§E.1), TS::joinLock.
 DISJOINT from heap rank 3 (VMManager::m_worldLock).
4b. §A.3.3 pending-job-slot mutex (HBT4.4 as AMENDED by ANNEX
 NLH1, BINDING): §A.3 conductors ONLY; inner to rank
 1/token, OUTER to heap rank 2 (GCL); held across the stop
 window; losers park on it access-released; never held with
 any api RANK-1..3 lock - long-hold NLS::m_lock EXCLUDED: a
 hold(fn) conductor (Class-A fire, §K.5, OM stops) may HOLD
 NLS on entry, never ACQUIRES it; edge NLS > slot > GCL,
 sound per NLH1.3; U20 per NLH1.5.
5. heap ranks 2-10b as frozen. Cross edges: api 3 -> 10a legal
 (§C.3); api locks NEVER wrap heap ranks 2-9b.
9c. [r33] GCH::m_mutatorMarkStackLock (CMS, congc §5.2) -
 TERMINAL leaf (nothing acquired holding it), INSIDE
 m_markingMutex (drain/donation sites only); MAY be held with
 heap 7-9b (addToRememberedSet append) - §LK.8
 destructor-leaf-class shape. SUPERSESSION (heap §6 leaf row
 "never 7-9b" vs this lock, both sides; congc
 CGS2.1/§13.5(1) back-cite; FULL row + soundness: history r33
 ANNEX CGS2A, BINDING).
9d. [r33] Marking-internal group (m_markingMutex,
 m_parallelSlotVisitorLock, m_raceMarkStackLock, visitor
 m_rightToRun): INSIDE GCL/GBL, mutually ordered as landed
 (markingMutex > CMS at drains); mutator-reachable
 OUT-OF-WINDOW at exactly the three CGS2.1 sites (SINFAC-tail
 donation, SINFAC I6 Heap.cpp:5216; DCT final flush; ACT/DCT
 pending enqueue - m_parallelSlotVisitorLock only). U20
 PROPER extends to rows 9c/9d - congc CG-T2 IS the extension
 (no second lock-order authority). Composed chain NL > GCL >
 m_markingMutex > CMS acyclic per CGS2.2 (CMS terminal,
 CG-I10; NL never ACQUIRED by GCL/markingMutex holders,
 NA-I10; GC-conduct NL>GCL edge REMOVED - nativeaffinity
 BL1.8; LK.1c stays a nativeaffinity §9.1 pending row). Rows
 flag-gated useConcurrentSharedGCMarking; flag-off frozen
 table operative (congc CG-I0). FULL rows: history r33 ANNEX
 CGS2A.
6. VMLiteRegistry::lock - RE-RANKED outer-of-leaves (SUPERSESSION
 vs vmstate §6.5.1/§7 "no lock while held", both sides): inner
 set {VMLite::scratchBufferLock, atomic bit ops, fastMalloc}
 ONLY; fastMalloc EXCLUDED while a thread is suspended by the
 holder (§A.1.7). ScratchBufferRegistry OUTSIDE it (§A.1.6).
7. Leaves: HandleSet::m_strongLock (F.3); DWT::m_pendingLock
 (E.7 note); §K class-2 cache locks; VMLite::scratchBufferLock.
8. Destructor-leaf class (SUPERSESSION vs heap §6 leaf row "never
 7-9b" + vmstate §7, both sides; IH rows): AtomString shards +
 SymbolRegistry::m_lock + HandleSet::m_strongLock acquirable
 UNDER MSPL/BVL/9b - in-lock sweep dtors reach them (r10 F1);
 sound: holders fastMalloc/list-splice-only, acquire nothing,
 never wait (vmstate I7 extended).
- Long-hold: NLS::m_lock is NOT a leaf (lock.hold runs user JS
 holding it; held across parks + token/access reacq, §E.2
 exemption) - ordered OUTSIDE heap 2-10 + api 1-3; acyclic: no
 conductor or heap-2..9b holder ACQUIRES it; §A.3 conductors
 MAY hold it on entry (NLH1.4). SUPERSESSION (r16 F6; api §5.9
 rank-4 leaf + (f), api:263/:272, vs this, both sides; IA
 row): GIL-on UNCHANGED; §LK canonical for U20 (r22 list).
- Negative edges (normative): no heap 2-9b holder acquires ANY api
 lock; no 10a/10b holder acquires api rank<=3; GC/§A.3 conductors
 acquire NO api lock (interplay: §D.1's mutator-side TM
 snapshot/release + the WS(ii) finalize carve-out); api 1-3
 holders never transition heap access (§E.2).
- WS rows (r22 W1; FULL text ANNEX WS1 + r25 ext, BINDING;
 r27/r33 compressed). (i) Weak CREATION FORBIDDEN under api
 1-3 + §LK.7 leaves; SUPERSESSION (api 5.7.2 landed shape vs
 this, both sides; IU rows): hoist construction BEFORE the
 lock, publish under it. (ii) sole carve-out:
 WeakHandleOwner::finalize MAY take the rank-2 affinity lock /
 class-2 leaves in-window (WS1.3); TM rank 1 NOT excepted.
 U-T8b lock-context column; U20 lints both.

§C.1 lock-free arms + §N.2 ropes take NO lock.

## INV. Invariants

Normative text: history r9 annex + r10/r11 additions (IDs
FROZEN); rest: U6 §C.1/C.3; U10 §E.4/I11; U11 §E.2; U12/U13
§F.2; U14 §F.4; U15 §G/J.3; U18 §D.1; U29 §A.3.8; U32 §A.3/
§B.2 (EXIT1.7; U3 AMENDED there, both sides). U19 = §J
oracle (sole edits SD6/SD7).

## SD. Semantic deltas vs phase 1 (corpus impact)

Normative text: history r9/r11/r13/r14/r16/r24 annexes (IDs
FROZEN).
SD1 join-at-close (§E); SD2 own-queue drain (GPO); SD3 registrant
settle, dead=>main (§E.4); SD4 spawned TA sync wait GIL-off only
(§C.4/§G); SD5 notify no-yield (§C.5); SD6 single-flight lifted,
both GIL modes (§C.6/A.2.6); SD7 spawned wasm TypeError, both
modes (§I); SD8 terminate-parked Failed (§E.5) + r16 F5 ext
(close settles finite waitAsync timed-out) + r27 ext2 (fresh
Error, TERM1.3); SD9 TID-exhaustion RangeError (§D.1); SD10
settling-thread reactions, ALS kept (§E.1b.1/.5); SD11 spawned
TA waitAsync main-side (§E.3); SD12 asyncJoin no keepalive,
mutual/self safe (§E.3); SD13 spawned breakpoints no-op
(§A.2.7); SD14 watchdog (annex W; §A.2.8); SD15
rejection-tracker carrier-queued (§E.1b.4); SD16 finite prop
waitAsync timed-out only at registrant drain/close (§E.7.5);
SD17 termination drops undrained per-lite microtasks (§E.5);
SD18 sampling-profiler main-thread capture (§K.6); SD19
per-thread RegExp legacy statics (§K.6) - all GIL-off only
except SD6/SD7. U19 fallback corpus keeps OLD expectations via
//@ runThreadsGILOff/GILOn variants for SD1-SD5/SD8-SD19;
SD6/SD7 GIL-on expectations change. Per-rev SD attribution:
history; r27-r33 add none (ext2 rides SD8).
§N.5's TypeError + §I's wasm-GC LinkError are NOT SDs.

## IM. Integration manifest

NORMATIVE ANNEX (history): hot-file -> section table + owners +
rev-7..24 add-lists; diffs land via IU. IU does NOT exist yet:
U-T1 CREATES it (ANNEX TERM1.6); until then "IU row" = an
obligation written at landing; audits consume the EXECUTED
K4/N7 tables - IU adds call sites, never re-rules.

## T. Ordered task list

Full scope + gates: history r9 annex + r10-r33 deltas; IDs
frozen.
Index: U-T1 §A.1.2-7/A.3.6; U-T2 §A.2; U-T3
§A.1.1-3 + U0c; U-T4 §A.1.3/6 JIT emission; U-T5 §A.3 STW +
§A.3.8 + stub/witness/M7 deletion; U-T6 §B.1-3; U-T7 §B.4-6 +
U21; U-T8 §F/§J; U-T8b CONSUME annexes K4+N7 (audits EXECUTED;
§K.6/§N.9 residue; +U-T8e hooks, §E.1b.4) + ~VM walk + U-T8d
readers (§A.1.7), gates U-T9; U-T9 §E + corpus
SD1-SD3/SD8/SD10-SD12/SD15/SD17 + hook/§N arms (ENTRY GATE:
§F.6(b) SD10 disposition, r21); U-T10 §C.1-2 (ENTRY GATE:
Task-14 verdict, §D.2); U-T11 §C.3-6/§G/§J.3 + corpus SD4-SD6
+ §J.3/§E.7.5/close/SD17 arms; U-T12 §D.1 rebias +
amplifiers; U-T13 §H/§I/A.3.7 + wasm-GC precheck +
§N.6/§K.5 arms; U-T14 close (U0/U0b/U0c, TSAN + amplifiers,
U19, default flip, IU dispositions).
Deps: T1->{T2,T3,T4}; {T2,T5}->T6; T5 gates T12; {T8,T8b}->T9;
T9 gates T11; T14 last (entry gates above). T1-T7 dark. Each
task re-runs the flag-off golden gates + U19.
