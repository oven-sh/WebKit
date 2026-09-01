# SPEC-ungil-history.md — change log + overflow rationale

## Rev log

- **rev 1 (2026-06-05).** Initial freeze. Closes UNGIL-PLAN.md items A-J.
  All file:line ground-truth citations re-verified by reading the cited
  code on branch jarred/threads this date (JSLock.cpp, ThreadObject.cpp,
  LockObject.h, ThreadManager.cpp, ThreadAtomics.cpp, AtomicsObject.cpp,
  JSThreadsSafepoint.cpp, VMEntryScope.cpp, OptionsList.h:696, WTF
  SymbolRegistry.cpp) and the cited spec lines (SPEC-vmstate.md:37-48,
  335-349, 418, 443, 490, 529, 551, 572; SPEC-heap.md:26-27, §6 table at
  :109-130; SPEC-api.md:22, 26, 79, 100, 126, 200, 225, 306, 315, 361;
  SPEC-jit.md:233, 278; SPEC-objectmodel.md:359, 377-378; THREAD.md:19,
  :98).

## Overflow rationale (normative intent lives in SPEC-ungil.md; this
## section records WHY, and the alternatives rejected)

### A.1 base-pointer choice: TLS materialization over a pinned GPR
A dedicated callee-saved register for the VMLite base was rejected:
x86_64 register pressure (LLInt already pins PB/metadataTable; Baseline/DFG
allocators would lose a GPR globally for a value needed only at VM-field
touch points), and the ABI churn would invalidate the vmstate Phase A
"accessor signatures frozen, impl replaceable" escape hatch (L4) by leaking
into every tier's frame contract. TLS-read-per-prologue with caching in a
temp costs one load on entry paths and nothing on slot-access fast paths
that already have the lite in a register from the prologue. The
VMEntryRecord::m_vmLite slot exists so OSR exit and unwinding can recover
the base without TLS access in awkward contexts.

### A.3.6 main-carrier decision: why lazy real carriers, not a §6.4(3) view
The alternative (embedder threads share a tid-0 "view" of the VM) is
exactly the configuration JSLock.cpp:136-148 documents as unsound GIL-off:
two threads believing they are TID-0 owners race unlocked flat-butterfly
transitions. Unique lazy TIDs make embedder threads ordinary mutators to
the OM machinery; the cost (TID-space consumption by embedder threads) is
absorbed by Task 13 rebias. api SPEC-api.md:361 already promised "post-GIL:
real TID lazily at first VM entry", so this is a confirmation, not a new
choice.

### E.3 keepalive: why decrement-at-enqueue, not decrement-at-run
Decrementing when the settle task RUNS would make the exit condition
"queues empty && keepalive==0" racy in the other direction: a settler that
appended but not yet decremented blocks exit (fine), but a cancel path
that never enqueues would need a separate decrement anyway, splitting the
accounting across two disciplines. Decrement-at-enqueue under the same
inboxLock critical section as the append gives a single-lock proof of U9:
the predicate (queue emptiness, keepalive) changes only under inboxLock,
and every decrement either appends in the same section or is a cancel
(no append ever follows). The per-ticket m_keepaliveReleased CAS mirrors
the landed m_settled CAS (ThreadManager.cpp:78-81) so the
settle-vs-cancel-vs-close races collapse to one winner.

### E vs DWT: why main keeps DeferredWorkTimer
Spawned threads get bespoke queues rather than per-thread DWT instances
because DWT is RunLoop-coupled and the embedder owns the only real
WTF::RunLoop (Bun's event loop, USE_BUN_EVENT_LOOP). A spawned thread's
"runloop" is the E.2 drain loop — a condition-variable pump — not a
WTF::RunLoop; tickets keep their DWT registration solely for the
shell-liveness (I20/4.6.3) and VM-shutdown cancelPendingWork backstop,
both of which remain process-global concerns.

### F.3 Strong handles: why one locked HandleSet, not per-thread sets
Per-thread HandleSets were rejected because Strong lifetime is not
thread-affine in the landed design: the 5.10 finalizer hook
(ThreadObject.cpp:96-131) and ~AsyncTicket (ThreadManager.cpp:48-59) exist
precisely because last-refs drop on foreign threads (GC finalization,
embedder TLS teardown). Per-thread sets would require cross-thread free
queues with their own epoch problem; a leaf lock on allocate/free is two
uncontended atomic ops on a path that is already not hot (Strong churn is
host-API, not JS-loop). Revisit only if the bench gate shows contention.

### H: why a plain leaf lock, not a sharded/concurrent registry
Symbol.for traffic is orders of magnitude below atomization traffic (which
did get shards). A leaf Lock keeps the WTF diff minimal and the lock-order
proof trivial. The non-goal is recorded in §H.3 with an explicit
bench-evidence reopen condition.

### I: why refuse wasm rather than GIL-serialize wasm on spawned threads
A "wasm runs but takes the GIL" hybrid was rejected: it reintroduces a
global lock with all of §F's deleted semantics for an unaudited subsystem,
creates a priority-inversion channel between JS threads and wasm callers,
and would still be unsound where wasm code calls back into JS (the
re-entry would hold the wasm-GIL across arbitrary JS). The clean TypeError
is honest, testable (U17), and cheap to lift later under a dedicated
charter. Making the gate active under GIL-on too (SD7) buys mode-equal
corpus behavior — a spawned-thread wasm test must not pass GIL-on and
fail GIL-off.

### Task sizing notes
U-T3/U-T4 sizes assume the Group-3 field set's JIT touch points are
mechanical (offset-expression rewrites guarded by the golden-disasm gate);
if FTL's B3 stack-slot interactions with per-lite scratch buffers exceed
budget, split U-T4 into U-T4a (Baseline/DFG) and U-T4b (FTL) at the
implementation workflow's discretion — the spec's freeze is on the
addressing contract, not the task boundary.

### Explicitly NOT redesigned here (deferred with citations)
- OM Task 14 structure splitting: deferral + bench-verdict recording only
  (SPEC-ungil §D.2; SPEC-objectmodel.md:359 decision rule).
- heap concurrent marking / incremental sweep (heap Dev 7 lists them with
  the TLC charter): TLC emission is in scope (U-T7); marking/sweep
  concurrency upgrades are perf work behind the same budget gate and ride
  the heap WS's own charter — no semantic dependency from A-J.
- Inspector/debugger attach to spawned threads: out of scope for the ungil
  milestone (no charter; debugger pause uses the §A.3 stop and observes
  whatever thread is conductor).

- **rev 2 (2026-06-05).** Round-1 adversarial review: 17 findings (6
  blocker, 11 major), ALL verified real against the tree — none refuted.
  Dispositions (finding -> spec change):
  1. A.3.2 heap-access-released stop exemption had no re-acquisition
     gate (JSThreadsStopScope sets no client-visible stop state,
     Heap.cpp; acquireAccess consults only GC stop state) -> new
     normative §A.3.2b: acquire-slow-path poll of the per-lite JSThreads
     stop bit + mandatory post-wake poll at every park site; U4 gains a
     wake-during-stop amplifier arm; E.6/J.3 cross-referenced.
  2. C.1 accessors were unsound both paths (segmented cell-lock CAS does
     not serialize vs lock-free fragment stores; flat grow = CAS+copy
     with no nuke -> lost CAS / double-applied RMW) -> respecified:
     direct seq_cst 64-bit CAS on the fragment word (fragments never
     move); flat path runs the OM §2 foreign-write SW-set DCAS when TID
     mismatches, re-validates per I34, whole-probe restart, RMW never
     re-applied. Cell-locked segmented CAS REJECTED because mandating
     the cell lock for all segmented writes would contradict the frozen
     OM lock-free regime-2 design.
  3. Ordinary shared-promise cross-thread settlement undesigned -> new
     §E.1b: reactions run on the SETTLING thread (option (b) of the
     review's menu); JSPromise internal-state transitions cell-locked
     GIL-off; U22 rewritten; SD10 added. Per-reaction registrant
     tracking REJECTED: a new heap-visible per-reaction structure with
     its own lifetime/teardown races, and the registrant may be dead at
     resolve time — the settling-thread rule needs no new state and is
     I11-clean (enqueuer is the queue owner).
  4. E.3 termination bulk-zero + cancel decrement double-decremented ->
     close now CLAIMS each outstanding registration's
     m_keepaliveReleased flag; the CAS guards ALL decrement sites; rules
     1-3 restated.
  5+11. DWT: m_pendingTickets had no lock (DeferredWorkTimer.h:121) and
     the thread-routed settle path never retired tickets -> new §E.7
     (internal leaf m_pendingLock, asserts re-pointed at the token
     predicate) + E.4 retirement protocol (ThreadTask runs settle, then
     cancelPendingWork under DWT's lock, then clears m_promise);
     dead=>main keeps scheduleWorkSoon; U24 added; DWT files added to IM.
  6. E.2 released heap access INSIDE the inboxLock section
     (releaseAccessSlow can stop the thread, Heap.cpp:2580-2595 — GC vs
     settler cycle) -> normative ordering rule: no heap-access
     transition while holding any api-rank lock; loop rewritten; U20
     extended.
  7. F.2 conflated VM::currentThreadIsHoldingAPILock (VM.cpp:201) with
     JSLock::currentThreadIsHoldingLock -> split: VM predicate = token,
     JSLock predicate = mutex-literal (F.4's DAL no-op and
     m_lockDropDepth LIFO depend on it); spawned unlock() routes to the
     token branch before the mutex assert; U-T8 now requires a full
     ~60-consumer audit table with fixed dispositions for the branch
     sites (sanitizeStackForVM, primitiveGigacageDisabled,
     validateIsNotSweeping, the Heap.cpp ISS-flip exclusion, DWT).
  8. Lazy main/embedder carriers omitted the P5/CS3 butterfly-TID-tag
     TLS install (zero-init correct only for tid 0) -> A.3.6 mandates
     the tag install/clear at lazy carrier install/teardown; U1 and J.7
     assert TLS tag == lite TID.
  9. Group-3 storage location per mode undecided + scratch-buffer
     baked-address mischaracterization -> A.1.3 decides: VM members
     flag-off/GIL-on (golden gate trivially holds), VMLitePrimitives
     GIL-off, JIT switches at codegen time, LLInt branches on the
     JSCConfig gate byte with ONE recorded golden-gate re-baseline; new
     A.1.6: per-lite scratch pool with runtime two-load indirection in
     DFG/FTL (baked addresses GIL-on/flag-off only).
  10. VMEntryScope/VM::entryScope + entry-scope service bits stayed
     VM-global (VMEntryScope.cpp:90/:133; VM.h:298/:373-454) -> new
     A.1.5 per-entry record in the lite; isEntered() over the
     entered-thread set; consumer ruling; VMEntryScope.{h,cpp} IM row
     upgraded from tripwire-deletion-only; U23 added.
  12. A.1.1 TLS scheme platform-unsound as stated; mid-body access
     unspecified -> A.1.1 adopts the frozen jit annex App. R5
     per-platform model verbatim (ELF IE-TLS; Darwin pthread key via
     the M4a JSCConfig slot; Windows already unsupported flag-on per
     App. R5 — no new Windows story owed); A.1.2 fixes the mid-body
     model: rematerialize via loadVMLite at each site, prologue/
     VMEntryRecord caching is optimization-only.
  13. D.1 rebias had no enumeration mechanism or restamp-to-0 soundness
     argument -> D.1 specifies HeapIterationScope full-walk (precise +
     aux) + StructureID-table walk, owning files, cost note, and the
     soundness paragraph: restamped objects become equivalent to
     main-allocated (payload-0/TID-0 regime, OM decode tests payload
     first); restamp ordered BEFORE m_freeTIDs release within one stop,
     which is exactly what the annex false-owner hazard requires.
  14. queueMicrotaskToEventLoop host hook unruled -> E.1 hook clause:
     consulted only on the main/embedder carrier; spawned threads always
     per-lite; corpus test with installed hook.
  15. C.4 ordered deletion of the WRONG gate: ThreadAtomics.cpp:536-541
     is the frozen-api G11 property-wait gate (kept, re-pointed at
     mayBlockSynchronously per §G.2); the sole 4.5-1a gate is
     AtomicsObject.cpp:613-621 (grep isJSThreadCurrent: only hit).
     C.4 rewritten; IM rows corrected.
  16. U19 "unchanged" contradicted C.6/§I both-mode changes -> master
     rule and U19 amended to "unchanged EXCEPT recorded both-mode deltas
     SD6/SD7", which are direct GIL-on corpus edits, removed from the
     per-mode-variant footnote. Option (b) chosen over scoping the D8
     deletion GIL-off-only because per-wait nodes are strictly more
     correct under the GIL too and a mode-conditional D8 gate would
     keep dead machinery alive solely to preserve a wart.
  17. §E silently superseded non-GPO frozen api 4.6.1 "Never waits for
     tkts" / 4.6.2 shell-granularity keepalive -> explicit SUPERSESSION
     block added to the §E preamble citing both sides; recorded for
     INTEGRATE-api.

  Byte-budget note: rev 2 also compressed prose throughout to stay
  under 50000 bytes; all removed rationale lives here. Additional
  rejected alternatives recorded this rev: cell-locked segmented CAS
  (finding 2), per-reaction registrant tracking (finding 3),
  GIL-off-only D8 scoping (finding 16).

## Rev 3 (2026-06-05) — round-3 review dispositions

All eleven distinct round-3 findings verified against the tree and
ACCEPTED (no false positives; duplicates merged: E.3-rule-3 x3, wasm
x2). Dispositions:

1. BLOCKER, E.2 close ran F1/F5 + Strong mutation access-released
   (contradicting B.2/U3; racing the GC handle scan): close now
   re-acquires access (after the §A.3.2b poll) BEFORE DWT retirement,
   residue routing, and F1/F5; access released at the landed T5 point
   after the Strong clears. New normative rule in §F.3: ALL
   Strong/HandleSet mutation requires an entered thread WITH heap
   access — that quiescence (not m_strongLock) is what the
   collection-time handle scan relies on; m_strongLock only guards
   allocate/free/set-slot against concurrent mutators.
2. MAJOR x3, E.3 rule 3 claim step unimplementable (no per-TS
   outstanding-registration set exists; taskQueue residue holds only
   already-settled tickets — verified ThreadManager.h:200-235, no
   forward set, only asyncJoiners + inbox): chose option (a) — claim
   step DROPPED. Proof of exactly-once without it: every decrement
   site is conditioned on (flag-CAS win AND inboxOpen) under
   inboxLock; after close, inboxOpen=false makes all later
   settles/cancels decrement-free (they win the CAS, skip the
   decrement, take the main fallback), and the counter is dead — the
   E.2 exit predicate is only read while the inbox is open, so a
   stale keepalive>0 value can never cause a hang or an early exit.
   U8's race-matrix argument re-derived from rules 1-2 alone.
   Rejected (b) (a per-TS Vector<Ref<AsyncTicket>> under inboxLock):
   adds a field, lock traffic at every registration/settle, and
   ~AsyncTicket interaction, purely to zero a counter nobody reads
   after close.
3. MAJOR, E.1b.2 promise one-liner deadlock-prone (verified
   JSPromise.cpp:341-440: pre-switch status() read, mid-body GC
   allocation of JSSlim/JSFullPromiseReaction + Bun
   InternalFieldTuple, setInlineHandlerReaction fast path): expanded
   to the normative allocate-outside/validate-and-publish-under-lock
   protocol; OM I20 (no GC alloc under 10a) restated as the driver;
   inline-reaction path runs fully under the lock (no alloc);
   markAsHandled/payloadCell/tier-inlined accesses are U-T9 audit
   items.
4. BLOCKER+MAJOR, §I wasm gate bypassable via cached
   jsToWasmICEntrypoint + boxed-callee trampoline (verified
   WebAssemblyFunction.h:75-90/:101-106): enforcement re-specified —
   under useJSThreads jsCallICEntrypoint() returns nullptr AND both
   generated JSToWasm entries (entrypoint thunk + interpreter
   trampoline) emit a spawned-TS prologue check; U17 gains the
   warm-call arm; wasm files added to IM. Rejected check-only-in-slow
   -path (does not gate) and value-visibility refusal (would need an
   object-graph walk).
5. MAJOR, DWT embedder hooks unruled (verified DeferredWorkTimer.h:
   110-112; addPendingWork/scheduleWorkSoon bypass m_pendingTickets
   when installed, .cpp:190-212): new §E.7.4 — hooks fire only on the
   main/embedder carrier; spawned-thread settles to hook-managed
   tickets go through an m_pendingLock-guarded handoff queue flushed
   by the carrier; retirement embedder-owned via onCancelPendingWork.
   Rejected "hooks must be thread-safe": a mid-flight contract change
   for Bun's landed event loop.
6. MAJOR, main/embedder microtask drain unspecified GIL-off (verified
   JSLock.cpp:342-343 is the API-embedder drain point): §F.1 now
   KEEPS drain-on-release GIL-off (drains the CURRENT lite's queue
   via the rerouted drainMicrotasks; well-defined under tokens, I11);
   main-carrier drain points enumerated. No SD entry — behavior
   preserved; U14 covers it.
7. MAJOR, GC-stop vs JSThreads-stop conflation: §D.1 now runs rebias
   world-stopped INSIDE the next full shared-server collection under
   the heap §10 barrier (NOT §A.3; jit R1.h keeps the machineries
   disjoint; mid-walk re-entry blocked by the GC's client-visible
   stop state); U-T12 re-worded. §F.3's Strong scan moved to the heap
   GC stop's handle-scan phase, soundness from the finding-1 rule.
8. MAJOR, A.1.6 scratch indirection underdesigned: concrete design
   added — process-wide ScratchBufferRegistry (leaf lock; monotonic
   index allocator + index->size map), per-lite never-shrinking
   segmented pointer table (lock-free reads), population invariant
   (code install fans out to all lites under the VMLiteRegistry lock;
   lite registration backfills) so the two-load JIT sequence never
   needs a lazy-alloc branch.
9. MAJOR, U-T5's U4 gate unexecutable as sequenced (GIL-off entry
   lands at T8, task appends at T9): staged explicitly — U4 arm LANDS
   with U-T5, FIRST RUNNABLE at U-T9 close-out (a U-T9 exit
   criterion); interim U-T5 gate = GIL-on no-regression +
   N-separate-VMs INTEGRATION-GATE + $vm stop/resume vs
   access-released embedder threads.
10. BLOCKER, A.3.2b gate attached to GIL-on acquire functions
   (verified Heap.h:1557-1565: GCClient::Heap::acquireHeapAccess/
   attachCurrentThread are the per-thread API, and the header invites
   direct AHA/RHA brackets): (i) re-pointed at the per-client layer;
   per-VM acquireAccess/acquireAccessForwardedToMainClient keep the
   poll for GIL-on/forwarded acquires; (ii) park-site polls demoted
   to defense — (i) is the soundness carrier because it covers
   arbitrary embedder brackets no site list can enumerate.
11. BLOCKER, C.1 contradicted OM locked regimes (verified OM I19/L3
   dictionary cell lock, §4.6 AS+SW locked, SPEC-objectmodel.md:46/
   :178/:229; Thread.restrict forces ArrayStorage per api Dev 11; the
   8g chartering record named the locked-dictionary variant,
   history:861): third normative arm added — locked-regime receivers
   do probe+CAS/RMW under the cell lock OM already mandates (plain
   stores there are also locked, so lock-held RMW serializes; and the
   lock is REQUIRED because dictionary mutation is StructureID-
   invisible, making I34 validation blind to quarantined-slot
   deletes). LK note updated; U5 gains dictionary-delete-vs-CAS and
   restricted-object storm arms.

Overflowed rationale (compressed out of the spec this rev):
- F.2 audit per-site reasoning: sanitizeStackForVM's RELEASE_ASSERT
  checks the current stack contains lastStackTop — per-lite storage
  makes token-true per-thread-correct; primitiveGigacageDisabled's
  fast arm means "can fire watchpoints synchronously", which GIL-off
  requires a §A.3 stop, so token-only threads must take the deferred
  arm; the ISS-flip clause-(a) exclusion reasons from mutex exclusion
  tokens do not provide, hence the boot-ordering assert making it
  unreachable GIL-off.
- D.1 restamp-to-0 full argument: payload==0 tested before any TID
  compare (OM decode); TID 0 bit-identical to today; restamped flat
  butterflies behave as main-allocated (foreign writers take the
  SW-set discipline; the only flat-owner claimant is main, a live
  owner under the landed protocol); the annex E4/T1 false-owner
  hazard is about reissue-before-restamp, excluded by ordering
  restamp before the m_freeTIDs release within one stop; parked
  threads' TLS tags belong to live TIDs and are untouched.
- E.2 GC-vs-settler cycle detail: releaseAccessSlow can hand the conn
  over and stop the thread (Heap.cpp:2580-2595); a settler holding
  access blocking on inboxLock while the GC waits on that settler is
  the cycle the ordering rule breaks.

## Rev 4 (2026-06-05) - review round: 4 blockers + 6 majors, all upheld

Every finding was verified against the tree before fixing; none was a
false positive. Dispositions (spec section in parens):

1. BLOCKER - TA Atomics.waitAsync keepalive increment had no decrement
   path (E.3). VERIFIED: WaiterListManager settles outside AsyncTicket
   (WaiterListManager.cpp:291 scheduleWorkSoon; timers on the VM
   runloop :176); TicketData has no registrant/m_keepaliveReleased, so
   neither E.4 rule could ever decrement - a NOTIFIED spawned-thread
   waitAsync would park its thread forever and hang join(). FIX
   (option b): the S4 increment is DELETED; TA waitAsync from spawned
   threads settles main-side and takes NO keepalive (new SD11 +
   corpus variant). Property Atomics.waitAsync IS an AsyncTicket
   (ThreadAtomics.cpp:639) and keeps its increment. Option (a) -
   re-homing TA waitAsync onto AsyncTicket so settlement runs on the
   registering thread - was REJECTED for v1: it requires redesigning
   WaiterListManager registration (per-waiter ticket plumbing through
   the WaiterList/Waiter machinery) for no soundness gain; main-side
   settle is sound under the shared heap (any thread may resolve a
   shared promise per E.1b; U22's "settling thread" rule is satisfied
   with main as the settler). Revisit post-v1 if thread-affine TA
   waitAsync reactions are wanted.
2. BLOCKER/MAJOR (two findings, same root) - inboxOpen never opened.
   VERIFIED: ThreadManager.h:225 initializes false; the declaration is
   the only occurrence in runtime/. FIX: new E.1 "inboxOpen lifecycle"
   clause - opened exactly once on the owning spawned thread in
   threadMain, under inboxLock, after lite/GCClient setup and BEFORE
   fn (hence happens-before any registration against the TS, since
   E.3 increments run on the registering == owning thread; a foreign
   thread cannot register against a TS before that TS's fn has run
   and handed out capabilities). Main/embedder TSs never open theirs;
   settles to them always take the E.4 main path - consistent with
   E.3's main-no-keepalive rule and rule-2's open check (cancel of a
   main registration skips the decrement, correctly, since none was
   taken). New invariant U25; in U-T9 scope.
3. MAJOR - async (signal) VMTraps delivery undesigned. VERIFIED:
   VMTraps.cpp:305 targets vm.ownerThread() (JSLock mutex owner -
   never set GIL-off) and vmIsInactive (:80) reads !entryScope &&
   !ownerThread(), TRUE while N threads run JS; :330 wakes the single
   vm.syncWaiter() that C.6 deletes. FIX: A.2.5 - GIL-off never starts
   the SignalSender (effective polling-trap behavior; the jit-mandated
   poll sites + D9 park quanta carry delivery and U2's bound);
   vmIsInactive re-derived from the lite registry. A.2.6 - the :330
   wake is GIL-on-only; C.6 per-wait TA parks and C.3 property parks
   use D9 10ms quanta polling the current lite's termination bit.
   Alternative (give each lite's VMTraps an owning Thread ref and keep
   signal delivery) rejected for v1: ThreadSuspendLocker-based signal
   install against N concurrently-running mutators multiplies the
   suspension-deadlock surface for marginal latency win over 10ms
   quanta + poll sites.
4. MAJOR - cross-thread entry-scope service requests had no routing
   rule. VERIFIED: requesters with no lite for the target VM exist by
   design (VM.cpp:764 Gigacage callback, :811 SamplingProfiler, :318
   watchdog; CONCURRENT_SAFE overload VM.h:381). FIX: A.1.5 "Service
   routing" fan-out mirroring A.2.3 - VM-level service word + fan into
   every registered lite under the registry lock; token acquisition
   ORs the VM word in; thread-local requests set the current lite;
   classification table due at U-T1. F.2's primitiveGigacageDisabled
   disposition now rides the VM-wide path. U23 extended.
5. MAJOR - multi-VM per thread vs one-carrier-per-thread + J.7 assert.
   VERIFIED: the GIL-on install (JSLock.cpp:131-156) explicitly
   handles multi-VM today; testapi exercises multiple contexts per
   thread. FIX: A.3.6 - carriers are per-(thread,VM); main/embedder
   lock() installs the entered VM's carrier AND swaps the
   butterfly-TID-tag TLS, restoring the prior {lite, tag} pair on
   release (LIFO under nesting). Spawned threads stay single-VM in v1
   (RELEASE_ASSERT; boot-gated) - a spawned Thread has no API surface
   to enter a foreign VM in v1, so this is a tripwire, not a
   behavioral change. J.7/U1 restated against the ENTERED VM.
6. BLOCKER - E.7.4 handoff queue had no main-carrier wakeup under
   USE_BUN_EVENT_LOOP (main parks in the embedder epoll/kqueue loop;
   all listed flush points require main already inside JSC). FIX:
   normative wake - fourth hook onCrossThreadWorkEnqueued, the only
   hook callable off-carrier (no JS; REQUIRED whenever the other three
   are installed, boot-checked); fallback vm.runLoop().dispatch of the
   flush task. RunLoop::dispatch alone was not made primary because
   with Bun hooks installed the VM runloop may not be the loop main
   actually sleeps in; the hook lets the embedder use its native wake
   primitive. U24 + the U-T9 hook arm exercise the parked-main settle.
7. MAJOR - E.7.4 covered only the settle side; registration/cancel
   fired hooks on the calling thread. VERIFIED: addPendingWork invokes
   onAddPendingWork inline (DeferredWorkTimer.cpp:204-205);
   scheduleWorkSoon → onScheduleWorkSoon (:234); cancelPendingWork →
   onCancelPendingWork (:266) - all on the caller. FIX: TicketData
   hookManaged bit set at registration iff hooks installed AND
   registrant is a main/embedder carrier; spawned-TS registrations
   always take the internal m_pendingTickets arm with NO hook call -
   I20 liveness holds from the internal append (registration is live
   from the append, not any flush), and thread keepalive (E.3) is the
   spawned-registrant liveness carrier anyway. Off-carrier
   settle/cancel of EITHER kind goes through the handoff queue and
   re-dispatches on the carrier; onCancelPendingWork therefore only
   ever sees hookManaged tickets, on-carrier, resolving the
   E.4(b)-vs-E.7.4 composition.
8. MAJOR - IM omissions. FIX: rows added for VMTraps.{h,cpp} +
   VMTrapsInlines.h (A.2 / IV), WaiterListManager.{h,cpp} (E.3 note,
   C.6 / IA D4), ConcurrentButterfly.h + Structure* (D.1 restamp /
   IO). Watchdog.cpp consumption is covered by the A.1.5 entryScope
   rehoming under VM.{h,cpp}'s row (watchdog reads the CURRENT lite).
9. BLOCKER - §C could not express indexed property keys. VERIFIED:
   ThreadAtomics routes parseIndex hits through
   canGetIndexQuickly/getIndexQuickly (:77-82) and putDirectIndex
   (:147-148); JSTests/threads/atomics/property-rmw.js exercises
   integer keys. FIX: new indexed arm in the 8g re-freeze -
   atomicElementCompareExchange/ReadModifyWrite(JSObject*, uint32_t)
   dispatching on indexing shape: CoW materializes per OM 4.8/I35
   before any probe; Int32/Double CONVERT to Contiguous at first
   atomic access via the ordinary OM transition discipline (owner
   direct, foreign SW-set-DCAS-first); Contiguous takes the flat
   lock-free arm verbatim; AS/dictionary-indexed take the locked arm.
   Raw int32/double word CAS semantics were REJECTED: an RMW result
   (or Atomics.exchange of a non-number) can force a value-shape
   transition mid-operation - the conversion would have to happen
   inside the atomic step, which the flat protocol cannot express;
   one-time convert-to-Contiguous at first atomic access is the
   landed engines' precedent (shape stays legal for all subsequent
   plain ops). §9.5 named accessors are now explicitly scoped to
   non-index PropertyNames.
10. MAJOR - Task-14 PRE-INT verdict does not exist. VERIFIED:
   INTEGRATE-objectmodel §46 still reads "run the jit Task-13 bench
   ... and record the verdict here" - never done; UNGIL-PLAN.md:250
   binds SPEC-ungil to RECORD, but a docs-only round cannot run a
   bench. FIX: explicit SUPERSESSION in D.2 citing both sides
   (SPEC-objectmodel.md:359 + SPEC-api.md:26 + SPEC-jit.md:278 vs
   D.2): the verdict gate moves to a HARD precondition of U-T10
   ENTRY - the first task a PROMOTE verdict would invalidate (U-T1..
   T9 touch no 8h-dependent design: A/B/E/F/G/H machinery and the
   promise cell-lock protocol are structure-splitting-agnostic).
   Contingency: on PROMOTE, Task 14 lands before U-T10 and §C arm 3
   is re-reviewed pre-implementation. T-list/dependency line updated;
   U-T14 no longer carries the verdict.

Editorial: rev 4 also compressed prose throughout to stay under the
50,000-byte cap; all removed rationale lives in this file. Notably:
- E.1 host-hook rationale (X1.7): consulting queueMicrotaskToEventLoop
  for spawned-thread enqueues would route every spawned reaction to
  the embedder's main loop, violating I11 (owner-only queues) and U22
  (reactions run on the settling thread).
- A.3.2b (i)-vs-(ii): (i) is the soundness carrier because
  GCClient::Heap's acquire/attach header invites arbitrary embedder
  acquire/release brackets that a park-site enumeration (ii) cannot
  cover; (ii) remains as defense in depth.
- D.1 restamp-to-0: full annex false-owner argument as in rev 3.

## Rev 5 (2026-06-05) - review round: 4 blockers + 4 majors; 7 upheld
## (1 partially refuted), all fixed in-spec

1. BLOCKER - property Atomics.wait keeps the landed read-then-enqueue
   shape, reopening I10 GIL-off. VERIFIED: ThreadAtomics.cpp:529-531
   does the SVZ read under the JSLock with "no re-read below";
   :550-552 explicitly credits the JSLock (not the listLock) with
   closing the lost store+notify window; the D9 park loop (:571-576)
   polls only termination/deadline; the waitAsync arming enqueue
   (:646 region) has the same unlocked equality check. GIL-off, §C.1
   atomic stores take NO lock, so [read==expected] -> [store] ->
   [notify sees empty list] -> [enqueue, park] loses the wake forever
   (sync: infinite park; async: never-settled ticket; with old §E.3,
   an unjoinable registrant). FIX (§C.3 rewritten): GIL-off both arms
   enqueue under listLock then RE-VALIDATE SVZ(o[k], expected) via the
   §9.5 atomic load STILL UNDER listLock; mismatch => dequeue +
   "not-equal"; rope re-read => unlock/resolve/restart (no allocation
   under listLock; rank 3 -> 10a nesting is in §LK order). Soundness:
   the notifier acquires listLock after its seq_cst store, so any
   store invisible to the re-validation has its notify ordered after
   our enqueue - exactly WaiterListManager's under-lock check shape.
   U5/U-T11 corpus arms added.

2. BLOCKER - no design for GIL-serialized VM/global caches + lazy
   init. VERIFIED for: VM::numericStrings (VM.h:657; NumericStrings.h
   lockless), stringSplitIndice (VM.h:665, reserveInitialCapacity
   VM.cpp:324), LazyProperty/LazyClassStructure plain uintptr_t state
   machine (LazyProperty.h:117; LazyPropertyInlines.h - no atomics).
   PARTIALLY REFUTED for RegExpCache: RegExpCache.h:79 declares
   Lock m_lock and RegExpCache.cpp takes it in lookup/lookupOrCreate/
   addToStrongCache/deleteAllCode (:43,:52,:63,:78,:100) - the map is
   already internally locked; cited in-spec as the class-2 model. FIX:
   new §K with three rulings (per-lite duplicate / leaf lock / atomic
   double-checked lazy publication with stop-bit-polling spinners),
   a mandatory inventory audit task U-T8b gating U-T9, and the §F.2
   audit taxonomy extended with the third class (EXCLUSIVITY
   CONSUMER must name its §K serializer; reinterpreting the
   token-true-on-N predicate cannot fix such consumers). U26.

3. BLOCKER - asyncJoin keepalive deadlocks mutual/self asyncJoin.
   VERIFIED: join() has a self gate (ThreadObject.cpp:340-342);
   threadProtoFuncAsyncJoin (:398-435) has none; §E.3 rev 4 counted
   asyncJoin in keepalive, and its ticket settles only at the joinee's
   close, so A<->B mutual asyncJoin (or self-asyncJoin) left both
   keepalives pinned at >0 forever - a GIL-on-legal program that hangs
   GIL-off, in no SD entry. Also confirmed UNGIL-PLAN item E's
   mandated keepalive list omits asyncJoin. FIX: asyncJoin removed
   from the increment set (no keepalive); registrant closed by settle
   time => E.4 main fallback (SD11 precedent, I12 dead=>main,
   api:306). New SD12 + mutual/self corpus variants. Rationale for
   keeping the other three: their settles originate outside the
   joinee-close path and represent the registrant's own pending work.

4. BLOCKER - §A.1.6 covered only baked-address scratch. VERIFIED:
   FTLLowerDFGToB3.cpp:300 stores vm.scratchBufferForSize into
   jitCode->common.catchOSREntryBuffer (one buffer per compiled code,
   shared by all entering threads); FTLForOSREntryJITCode.cpp:47
   caches m_entryBuffer identically; DFGOSREntry.cpp:248 calls
   vm.scratchBufferForSize per entry at runtime. Verbatim rev-4
   implementation would leave concurrent catch-/loop-OSR entries
   sharing one staging buffer (silent wrong values). FIX (§A.1.6
   NON-BAKED rule): VM::scratchBufferForSize GIL-off resolves through
   the CURRENT lite's table with a per-size-class registry index
   (covers all runtime C++ callers, bounded index growth); the
   JITCode-resident ScratchBuffer* members become registry INDICES
   resolved against the entering lite at use; amplifier variant
   (concurrent catch-/loop-OSR-entry); U-T4 scope + §IM row extended
   with the FTL/DFG OSR-entry files.

5. MAJOR - DWT no-hooks runloop wake missing. VERIFIED: RunLoop stop
   only inside the DWT timer callback (DeferredWorkTimer.cpp:103-106);
   runRunLoop parks in RunLoop::run() (:185-187); the §E.4(b)
   spawned-thread retirement does cancelPendingWork with no m_tasks
   append/timer arm => jsc shell strands when the LAST pending ticket
   is retired off-carrier. FIX (§E.7.4): any internal-arm cancel/
   retire while m_shouldStopRunLoopWhenAllTicketsFinish dispatches a
   re-check via vm.runLoop().dispatch() (cross-thread-safe); the
   re-check owns the stop decision on-loop; the :103/:186 emptiness
   reads join m_pendingLock coverage; U24 gains the
   last-ticket-retired-off-carrier-while-main-parked shell arm.
   §E.4(b)'s retirement now explicitly fires this wake.

6+8. MAJOR x2 (same defect, two filings) - §A.2.6 kept the VMTraps
   syncWaiter wake "GIL-on-only" while §C.6/SD6 deletes its target in
   BOTH modes. VERIFIED: VMTraps.cpp:329 AND :419 notify
   vm.syncWaiter()->condition(); the landed GIL-on park (waitForSync,
   WaiterListManager.cpp:83-108) blocks in waitUntil and re-checks
   hasTerminationRequest only on wakeups - with per-wait nodes in both
   modes the :329/:419 notifies target a node nobody parks on, making
   GIL-on sync TA waits termination-immune (breaks the U19 oracle +
   U2 in the bisection-baseline mode). FIX (§A.2.6 rewritten + §C.6
   cross-ref): both wakes DELETED with the syncWaiter; per-wait TA
   nodes and §C.3 property sync parks poll termination in D9 10ms
   quanta in BOTH modes (GIL-on: landed VM-wide predicate form;
   GIL-off: CURRENT lite's bit); U2 declared both-mode; U19 gains a
   GIL-on terminate-parked-TA-waiter arm; SD6 entry notes the
   both-mode D9-quanta park + corpus edit.

7. MAJOR - LLInt gate byte wrong + flag-on+GIL-on unassigned in the
   §A.1.3 case split. VERIFIED: the only landed asm gate tests
   OptionsStorage::useJSThreads (LowLevelInterpreter64.asm:1617)
   inside Config::options (JSCConfig.h:104,:107); rev-4's bullets read
   "Flag-off AND GIL-on" vs "GIL-off", leaving flag-on+GIL-on (phase-1
   production AND the U19 oracle) assigned to neither - and a
   useJSThreads-only LLInt branch would route it to per-lite storage
   while J.5/J.6 GIL machinery keeps VM members authoritative
   (split-brain). FIX: bullets relabeled "GIL-on (flag-on OR
   flag-off): VM storage" vs "GIL-off"; LLInt branches on a NEW
   derived gilOff byte in Config::options written at option
   finalization as useJSThreads() && !useThreadGIL(); jit R1.e
   re-pointed; OptionsList/§IM rows note the byte; U-T3/U-T4 updated.

9. BLOCKER - lazy carrier lifetime vs frozen vmstate M6 ~VM sequence.
   VERIFIED: vmstate:486-490 (M6) runs uninstall -> §6.5.1
   no-other-lite assert -> unregister -> destroy m_mainVMLite, with
   I20 "TLS never dangles"; §A.3.6's per-(thread,VM) registered
   carriers make the assert false at ~VM GIL-off, and no supersession
   was recorded (master-rule violation); force-destroying foreign
   carriers without invalidation leaves TLS maps keyed by VM*
   vulnerable to address reuse (UAF on a later VM at the same
   address). FIX (§A.3.6 supersession, both sides cited): ~VM first
   walks the VMLiteRegistry for this VM under its lock; foreign
   carriers must be token-free (RELEASE_ASSERT - entered-elsewhere
   ~VM stays an embedder contract violation, as today) and are
   DCT'd/destroyed/unregistered in the walk; §6.5.1 becomes "registry
   empty for this VM"; VMs carry a process-monotonic 64-bit epoch and
   the TLS map stores {VM*, epoch, carrier} - lock() compares epochs
   BEFORE touching the cached carrier, so a recycled VM* builds a
   fresh carrier and the stale pointer is never dereferenced; I20
   preserved per-thread (a destroyed carrier was token-free, hence
   not CURRENT anywhere). New U27 + teardown-storm test; B.2 teardown
   text updated to reference the walk.

### Rev-5 editorial (byte cap)

To stay under 50000 bytes with ~4.5KB of new normative content, rev 5:
- introduced the citation shorthand vmstate:/api:/om:/jit:/heap: for
  the five frozen SPEC files and IU for INTEGRATE-ungil (legend in the
  header); source-file citations remain full;
- removed markdown bold/backtick markup and reduced list-continuation
  indentation (content unchanged);
- compressed prose throughout; relocated rationale here. Notably:
  - C.3: the full I10 interleaving and the WaiterListManager
    waitSyncImpl analogy (above, item 1);
  - E.3: why asyncHold/cond.asyncWait/property-waitAsync KEEP
    keepalive while asyncJoin does not (item 3);
  - A.2.6: why a GIL-on-only central wake cannot survive SD6 (item
    6+8);
  - A.1.6: the concurrent catch-OSR staging corruption scenario
    (item 4);
  - A.3.6: the VM*-reuse UAF scenario motivating the epoch (item 9);
  - K: NumericStrings/stringSplitIndice/LazyProperty evidence lines
    and the RegExpCache partial refutation (RegExpCache.h:79 +
    RegExpCache.cpp:43-100 lockers) (item 2).

================================================================
## Rev 6 (2026-06-05) - resolves 9 reviewer findings (8 unique; one
duplicate pair), all verified REAL against the tree + frozen specs.
No refutations this round. Full arguments + material compressed out
of the spec to stay under the 50000-byte cap.

### F1 (blocker) E.3/E.4 keepalive underflow on non-counted tickets
Verified: spec rev-5 enumerated the INCREMENT set ("every AsyncTicket
whose registrant is a spawned TS EXCEPT asyncJoin") but rules 1-2's
decrement was gated only on winning the m_keepaliveReleased CAS + the
inbox-open check; nothing initialized the flag released for asyncJoin
tickets (the cited analogue m_settled, ThreadManager.cpp:78-81 /
ThreadManager.h:151, constructs false). Concrete failure: mutual
asyncJoin A<->B with both inboxes OPEN - A closes, F5 settles B's
ticket via E.4 (registrant B, inbox open) -> CAS wins -> rule-1
decrement on a counter B never incremented -> uint64 wraps ->
keepaliveCount==0 never true -> B never closes; SD12's "never
deadlocks" claim was violated by the spec's own mechanics, as was U8.
FIX (normative, §E.3): m_keepaliveReleased is CONSTRUCTED true
(released = safe default); the INCREMENT site ALONE stores false
(armed) before the ticket is visible; rules 1-2 decrement only on
winning the false->true CAS, so never-armed tickets (asyncJoin, TA
waitAsync, main/embedder, any future non-counted registration) can
never decrement. Exactly-once preserved: arm happens-before
visibility, and the CAS is still single-winner. U8 gains the
mutual-asyncJoin-with-OPEN-inboxes corpus arm (the prior mutual/self
variants only covered the closed-registrant path).

### F2+F6 (blocker+major, duplicate) §K.3 lazy-init losers spin
holding heap access polling only §A.3 stop bits
Verified: §A.3 stops and GC stops are disjoint mechanisms - jit R1.h
(SPEC-jit.md:231 "GC does NOT share this") and heap Dev 8/§10
(per-CLIENT access-state barrier); §A.3.2b itself states a JSThreads
stop sets NO client-visible GC stop state. A loser spinning WITH
access while polling only the §A.3 bit never observes a shared-GC
stop request; if the winner's initializer allocates and triggers a
collection, the GC waits on the access-holding loser, the loser waits
on the winner's release-store, the winner is blocked in allocation -
three-way deadlock on a mainline path (two threads first-touch one
JSGlobalObject initLater under allocation pressure). FIX (§K.3):
losers wait PARK-CAPABLE in bounded quanta - release heap access (E.2
ordering, no lock held), poll BOTH stop families (lite §A.3 stop bit
AND heap §10 per-client GC stop state via stopIfNecessary),
re-acquire via the §A.3.2b-gated path, re-test the load-acquire. U26
gains a forced-full-GC-during-winner-initializer arm (liveness, which
the old TSAN one-init check could not catch).

### F3 (major) syncWaiter deletion was flag-off-reachable
Verified: vm.syncWaiter() (VM.h:1174/:1376), the VMTraps.cpp:329/:419
termination wakes, and the waitForSync park (WaiterListManager.cpp:
83-108, full-deadline wait re-checking termination only on wakeups)
are upstream machinery used by EVERY sync TA Atomics.wait incl.
useJSThreads=0 vanilla SAB agents. Rev-5's "deleted, both modes"
language (where "both modes" = both GIL modes under useJSThreads=1,
line 13) left the flag-off arm with its wake mechanism deleted -
verbatim implementation either hangs flag-off
terminate-during-infinite-wait or silently converts flag-off to D9
quanta (an unlicensed flag-off behavioral delta; D9's
jsThreadParkTerminationRequested predicate is JSThreads-only
machinery, LockObject.h:144-156). FIX (§A.2.6): wakes are BYPASSED
under useJSThreads (not deleted); explicit flag-off disposition -
syncWaiter + :329/:419 wakes + landed park stay compiled AND live;
atomicsWaitImpl branches on useJSThreads; the D9 predicate is never
consulted flag-off; "both modes" scoping rule stated in-doc; §T
flag-off golden gates gain terminate-during-infinite-TA-wait. §C.6
re-pointed ("central wakes bypassed flag-on; flag-off keeps them").
Note the D8 single-flight gate (AtomicsObject.cpp:500-511) is itself
on the flag-gated path (per its own comment), so deleting IT both GIL
modes remains sound.

### F4 (major) §E.7.3 hook routing incomplete
Verified: landed scheduleWorkSoon dispatches to onScheduleWorkSoon
UNCONDITIONALLY (DeferredWorkTimer.cpp:232-238) and cancelPendingWork
to onCancelPendingWork unconditionally (:266-269). Rev-5 stated the
hookManaged re-dispatch only for the OFF-carrier handoff queue and
the hookManaged-only rule only for onCancelPendingWork - an
ON-carrier settle/cancel of a spawned-registered (internal) ticket
(E.4 dead=>main fallback on the main carrier; main-side E.4(b)
retirement) would hand Bun a Ticket it never saw via onAddPendingWork
(suppressed for spawned registrants) - lost settle or crash. Also
internal entries re-dispatched to m_tasks depend on DWT's run-loop
timer, which a hook-installing embedder (who owns scheduling) does
not pump - dead-thread-fallback settles would strand;
onCrossThreadWorkEnqueued only drove the flush, which only re-queued.
FIX (§E.7.3): (a) EVERY hook dispatch site consults hookManaged
BEFORE the installed-hook branch - internal tickets take the internal
arm regardless of calling thread; (b) with hooks installed,
internal-arm scheduleWorkSoon entries are NOT timer-scheduled - the
handoff flush + every §F.1 drain point EXECUTE them inline on the
carrier under its token (incl. E.4(b) retire + m_promise clear), so
the wake hook drives them to COMPLETION; (c) U24 Bun arm:
spawned-registered ticket, registrant dead, hooks installed, settle
completes.

### F5 (blocker) GC rooting of GIL-off per-lite Group-3 cells
Verified: heap/Heap.cpp:3585 roots vm.exception()/vm.lastException()
through the VM accessors inside the ConservativeScan/VMExceptions
constraint. Post-§A.1.3 rerouting those accessors resolve through the
CURRENT lite - on a GC visit thread that is no lite or the
conductor's own - so N-1 entered threads' pending
Exception/lastException cells (and cell-holding lazy regexp buffers)
were never rooted: premature collection + UAF on the throw path.
Rev-5 defined registry-walk scanning only for §A.1.6 scratch buffers
and §K.1 cache copies; U-T1's "GC root walk" was a dangling
cross-reference. FIX (§A.1.3 "GC roots", normative): the shared
collection's root/handle visit phase iterates the VMLiteRegistry
under its lock and appends EVERY registered lite's cell fields
(m_exception, m_lastException, scope-verification cells, lazy regexp
match buffers); registry stable because mutators are quiesced by the
heap §10 stop. §IM heap/Heap.* row now lists "A.1.3 root walk
(:3585-class sites)"; U-T1 amplifier: thrower parked pre-catch
survives a forced full collection. (vm.m_terminationException at
Heap.cpp:3592 stays VM-global - the termination exception is
per-VM/singleton, not per-thread Group-3 state.)

### F7 (blocker) "jit R1.e re-points at the new byte" - unsound
reading + missing two-sided supersession
Verified: R1.e (=M4a) is the SPEC-jit gate-byte clause (jit:230/:251)
and the landed ifJSThreadsBranch macro
(LowLevelInterpreter64.asm:1615-1618) gates the §5.4 LLInt cache
disables and §5.5 TID/SW butterfly choke points (:1620+). Reading
rev-5's clause as "R1.e's byte becomes gilOff" would, under
flag-on+GIL-on (phase-1 production / U19 oracle, where gilOff=false),
disable TID/SW dispatch and re-enable the disabled LLInt caches while
butterfly TID tags + segmented spines are still installed - LLInt
would treat a segmented spine payload as a flat butterfly pointer.
FIX (§A.1.3): rewritten - gilOff is a SECOND derived byte ADDED
BESIDE R1.e's useJSThreads byte; R1.e's byte and ALL its landed
ifJSThreadsBranch consumers are UNCHANGED (run whenever
useJSThreads=1, both GIL modes); ONLY the NEW Group-3
storage-selection branches test gilOff. Recorded as an EXTENSION of
jit R1.e with both sides cited (jit:230/:251 M4a vs the clause).

### F8 (major) heap Dev-7 GC-throughput items lacked a disposition
Verified: SPEC-heap.md:26 charters, as WS gating GIL removal, not
only TLC-aware inline emission but also "per-directory handout +
out-of-lock sweep, concurrent marking/incremental sweep" (the Dev-4
carve-outs, heap:23); api:26 composes "heap Dev 7 (incl. per-THREAD
TLC addressing)" into the milestone gate. Rev-5 closed only the TLC
item. FIX (§B.6, SUPERSESSION, both sides heap:26 + api:26 vs §B.6):
the GC-throughput items are DEFERRED to a post-ungil perf milestone;
GIL-off ships on the synchronous conductor-driven heap §10 protocol +
single-MSPL slow path (correctness-complete - the Dev-4 disables are
perf modes only, heap:23); replacement gate = §B.5 (<=10% 1-thread
composite, N-thread scaling recorded); a §B.5 miss pulls the deferred
items forward pre-ship; INTEGRATE-heap records the override.

### F9 (major) §A.1.6 silently displaced vmstate §6.6's frozen
Phase-B scratch contract
Verified: SPEC-vmstate.md:534-539 freezes "Group 5 + ScratchBuffer*
VMLite::scratchBufferForSize(size_t)"; VMLite.h:168-174 carries the
inert Group 5 (scratchBufferLock + Vector<ScratchBuffer*>). Rev-5's
registry+segmented-table design neither cited §6.6 nor disposed of
Group 5/the reserved signature - undirected reconciliation at
U-T1/U-T4. FIX (§A.1.6, re-freeze recorded with both sides
vmstate:534-539 vs the clause): VMLite::scratchBufferForSize(size_t)
IS implemented - over the segmented table by size-class index (the
non-baked arm); Group 5 is REPURPOSED, not dead: the lite's
buffer-ownership list (every buffer installed into this lite's table
appended under scratchBufferLock), backing the jit-R2 registry GC
scan and teardown free. Frozen L1-L5 layout untouched (L4 sanctions
accessor-implementation changes; the list reuses the declared fields
as-is).

### Editorial compressions (content relocated here, normative rules
unchanged in the spec)
- §E.6 Park/unpark merged into §E.2's tail (same rules: access
  released before park; wakeups = task append, stop, termination,
  10ms quantum).
- §J and §LK converted from tables to lists; J.2/J.4/J.5 merged into
  one disposition row. J.7's "tid-0 never installed" folded into U1.
- §E.1b.2 promise-protocol prose tightened; full step list as in rev
  5: performPromiseThen pre-switch status() read is ADVISORY;
  allocation of JSPromiseReaction + Bun InternalFieldTuple context
  (JSPromise.cpp:346-359) strictly outside the cell lock; under the
  lock re-check status, re-read reactionHead, fix next, publish via
  setPackedCell; settled => allocation dropped and the reaction
  enqueued via queueMicrotask after unlock; one uncontended cell-lock
  per op.
- §K.3 deadlock chain abbreviated (full chain: GC waits on the
  access-holding loser; loser waits on the winner's release-store;
  winner blocked in allocation behind the GC).
- U9 mechanics long form (rev 5): increment happens-before any settle
  of the same registration; decrement + append atomic under
  inboxLock; E.2's exit check reads keepalive + emptiness under the
  same lock so no exit can interleave between a decrement and its
  append; decrementer signals runLoopCondition before unlocking.
- §D.1 enumeration/restamp bullets merged; SD "was" clauses
  shortened; INV/T re-tersed; IM rows merged (ThreadObject+
  ThreadManager; ThreadAtomics+AtomicsObject; VMLite+VMTraps); §I
  warm-path rationale shortened (full form rev 5).
- "this spec supplies §§A-K" replaces the chartered-gaps phrasing
  (§K was always in scope as the NEW cache/lazy-init class).

## Rev 7 (2026-06-06) - resolves 4 findings (1 blocker, 3 majors), all
## verified REAL against the tree; no refutations

### F1 (BLOCKER) - builtin cell-internal mutable state beyond JSPromise
Verified: OM SPEC/annex, api/heap/vmstate/jit SPECs and rev-6
SPEC-ungil contain NO ruling for JSMap/JSSet/JSWeakMap storage, rope
resolution/atomization, DateInstance cache, FunctionRareData,
non-promise JSInternalFieldObjectImpl types, or ArrayBuffer detach
(grep across all six docs). OM §9.5/I21 scope = structure/butterfly
property slots; §K.4's audit scope = VM/JSGlobalObject members;
§E.1b's U-T9 audit sentence was promise-scoped. THREAD.md:200 ("no
race should ever lead to a VM crash") and THREAD.md:470 (layout-lock
discussion explicitly naming SparseArrayValueMap and Map/WeakMap/Set
as "too complicated for segmentation" but lockable) show the class
was known and is in-charter. Tree evidence: JSMap is
JSOrderedHashTable-backed (runtime/JSMap.h:32) - set() rehash splices
storage, so a concurrent reader UAFs; JSRopeString::resolveRope*
(JSString.h:637-682) writes fibers/flags in place;
DateInstance::gregorianDateTime caches a multi-word GregorianDateTime
(DateInstance.h:62-75); JSFunction::ensureRareData
(JSFunction.h:136-144) lazily materializes; ArrayBuffer::detach
(ArrayBuffer.h:199/:298) frees contents while racing accesses may
hold the base pointer; DFG inlines MapHash/MapGet/MapSet/WeakMapGet
(DFGNodeType.h:608-629). Fix: NEW §N - (a) defines the class
(multi-word cell-internal state outside §9.5/§K), (b) default
protocol = JSCellLock 10a in the §E.1b allocate-outside/
re-validate-under shape, (c) named rulings for all seven known
members, (d) exhaustive audit U-T8c gating U-T9, U28 invariant +
corpus arms (map.set storm, rope read/atomize race, generator
double-resume, detach-vs-read, Date storm). Design choices:
- Map/Set/WeakMap: cell lock on READS too - JSOrderedHashTable
  rehash/delete splices the backing store, so lock-free probes can
  UAF; DFG/FTL intrinsics disabled GIL-off (call locked native
  bodies); revival is a post-ungil perf item, mirroring §B.6's
  deferral shape.
- Ropes: cell lock REJECTED - strings are read-hot and resolution is
  idempotent; single-flight publish via one release-CAS of the
  fiber0/flags word is sound because losers' buffers are garbage
  (discarded) and readers already branch on isRope before touching
  fibers; resolveRopeToAtomString reuses the shared sharded atom
  table (U0), whose insert is already concurrent. This is also what
  makes §C.3's "resolve then restart" arm safe to run after dequeue.
- DateInstance: the cached GregorianDateTime pair is >8 bytes and
  not CASable; per-call caller-local computation GIL-off is strictly
  correct and only costs the cache win under sharing; m_data
  allocation itself CAS-publishes. vm.dateCache is VM-resident =>
  §K's audit covers it (class 1 or 2).
- FunctionRareData: materialization is exactly the §K.3 lazy-publish
  shape; allocation-profile internals (structure caches,
  watchpoint-bearing fields) mutate under the function's cell lock;
  pure profiling counters fall under jit in-scope item 7 (racy
  profiling tolerated); any cached Structure consumed under I34
  re-validation.
- Generators/iterator helpers: concurrent resume maps onto the
  EXISTING serial "already running" TypeError - the state-field
  re-check just moves under the cell lock, so the race becomes the
  same error a re-entrant serial program sees. Not an SD.
- ArrayBuffer detach: ordering length=0 (seq_cst) before base-clear
  makes racing bounds checks fail safe (length is checked first on
  all access paths); the contents-free is quarantined to the next
  heap §10 stop, the same epoch shape OM §6 uses for deleted
  property offsets - no read-after-detach UAF without adding any
  fast-path cost. Wasm memories are out of scope per §I (wasm
  refused on spawned threads in v1).

### F2 (major) - §J.3 degraded GILDroppedSection kept m_lock held
Verified: GIL-on park sites route JSLock::unlockAllForThreadParking
(JSLock.cpp:389-408), which fully releases m_lock (drain suppressed
via the m_lockDropDepth bump, per the in-tree comment at :393-399).
Rev-6 §J.3 degraded the section GIL-off to "access release + §A.3
park cooperation" only, and §G.3's "parks holding only its token"
never said m_lock is dropped - while §F.1 takes the main/embedder
token BY acquiring m_lock. So a main/embedder thread parked in
join()/cond.wait/property-or-TA Atomics.wait would hold m_lock for
the whole wait: a regression vs GIL-on and an outright deadlock for
the Bun pattern where the notifying store runs on a second embedder
thread that must first enter the VM through JSLock::lock(). Fix:
§J.3 now rules main/embedder park sites MUST release m_lock + token
via the unlockAllForThreadParking shape (drain suppression kept;
GIL-on-only extras skipped per §F.1/§A.3.7/§B.3) and re-acquire on
EVERY wake, re-running the §A.3.6 carrier/tag swap + §F.1
service-word OR before re-checking the wait condition (re-check is
mandatory: the condition may have changed while unlocked). §G.3
re-pointed at §J.3; §F.1 cross-references it; U15 + a C-API corpus
arm (main parks in property Atomics.wait, second embedder thread
enters and notifies) added; U-T11 carries the implementation.
J.2's "dead GIL-off" list amended: unlockAllForThreadParking is NOT
dead - re-derived by J.3.

### F3 (major) - §C.3 rope arm leaked the enqueued waiter
Verified from rev-6 text: the mismatch arm dequeued; the rope arm
said "unlock, resolve, restart" with the Waiting node still on the
PropertyWaiterList. That stale node consumes one FIFO notify(o,k)
count - a genuine waiter loses its wakeup, exactly the I10
lost-notify class §C.3 exists to close (or, with node reuse, a
double-enqueue). Fix (one clause): the rope arm DEQUEUES before
dropping listLock, resolution runs via the §N.2 single-flight
protocol (may allocate - legal, no lock held), and the restart
re-runs from the probe with a FRESH enqueue. Soundness: after
dequeue+restart the waiter is indistinguishable from a first-time
arrival; the notifier-orders-through-listLock argument is unchanged.
The alternative (pre-resolve both comparands before enqueue) was
rejected: the expected value arrives as a JSValue from user code and
can be re-roped between resolution and enqueue only by GC-invisible
user action on OTHER strings; dequeue-first is strictly simpler and
covers the o[k] side too.

### F4 (major) - §I prologue check had no discriminator; TID!=0 wrong
Verified: ThreadManager::isJSThreadCurrent (ThreadManager.cpp:157)
is a WTF::ThreadSpecific - not loadable at a fixed offset from
generated code. The only TLS data generated code has are the
butterfly TID tag (jit App. R5) and the lite via loadVMLite; rev-6
enumerated no isSpawned L2 append, so the natural implementation is
"TID tag != 0" - correct GIL-on (m_mainVMLite tid 0, VMLite.h:160)
but WRONG GIL-off: §A.3.6 gives every main/embedder thread a lazy
carrier with a TM-allocated NONZERO TID, so the check would throw
TypeError on main/embedder wasm (which §I explicitly does NOT
refuse) while still PASSING U17's spawned-positive tests - a
silently shipped break of Bun main-thread wasm. Fix: §I now
normatively defines the discriminator - VMLite gains an L2-append
uint8_t isSpawned, =1 stored at spawned lite registration BEFORE
setCurrent (§B.1 ordering makes it visible before any wasm entry can
run on that thread); main/embedder carriers (incl. GIL-off lazy
carriers) keep 0; emitted check = loadVMLite -> null => fall through
(no lite = not spawned; e.g. compiler threads never execute JSToWasm
entries, but fall-through is also the safe polarity) -> load byte ->
branch-to-throw. The C++ ctor/API gates keep isJSThreadCurrent().
U17 gains the NEGATIVE arm (main/embedder wasm never throws, both
GIL modes) so the broken discriminator can no longer pass the gate;
U-T13 carries thunk+trampoline emission + both U17 arms. L2
legality: vmstate L2 permits appends after Group 6; the byte joins
the other §A appends (threadContext/traps/clientHeap/scratch table).

### Relocated normative annex - §IM full table (spec §IM points here)
Bare names = runtime/. IA/IJ/IV/IH/IO = INTEGRATE-{api,jit,vmstate,
heap,objectmodel}. file = sections (counterpart):
- JSLock.{h,cpp} = F, A.3.6-7, B.3, J.3 (IA D1/D11; IV)
- ThreadObject.cpp + ThreadManager.{h,cpp} = B.1-2, E.1-E.4, D.1 (IA D5)
- DeferredWorkTimer.{h,cpp} = E.4/E.7 (IA D5)
- LockObject/ConditionObject = J.3-5, C.5, C.7/D12 (IA D2/D9/D12)
- ThreadAtomics.cpp + AtomicsObject.cpp = C.2-6 (:613-621 del), G
  (IA D3/D4/D7/D8)
- JSPromise.* + JSGlobalObject = E.1/E.1b, K
- JSMap/JSSet/WeakMapImpl + JSString/JSRopeString + DateInstance +
  FunctionRareData + JSGenerator* + ArrayBuffer* = N (NEW rev 7)
- bytecode/JSThreadsSafepoint.cpp = A.3 stub swap, J.8 (IJ, IO)
- VMEntryScope.{h,cpp} = A.1.5, A.3.4 (IJ M7; IV)
- VM.{h,cpp} + NumericStrings/LazyProperty* = A.1, E.1, F.2, K (IV M4-M6)
- VMLite.* + VMTraps.* = A.1-2, B.4, I isSpawned - L2 appends only (IV)
- WaiterListManager.{h,cpp} = E.3, C.6 (IA D4)
- ConcurrentButterfly.h + Structure* = D.1 (IO)
- VMManager.{h,cpp} = A.3.1-4 (IJ R1; IH)
- heap/Heap.* + HandleSet.* = A.1.3 root walk (:3585-class sites),
  A.3.2b, D.1, F.3 (IH)
- llint/jit/dfg/ftl (+OSR-entry) = A.1 incl. non-baked, B.4, I
  isSpawned check (IJ; gilOff)
- WTF SymbolRegistry.* = H
- wasm/js/* = I (SD7)
- OptionsList.h = U0, J.1, gilOff (all)

### Rationale relocated from the spec body in rev-7 compression
- §A.1.3 gilOff-byte: testing useJSThreads instead of gilOff in the
  NEW Group-3 storage-selection branches would route flag-on+GIL-on
  (phase-1 production) to VMLite storage, breaking the U19 oracle.
- §A.2.6 flag-off cites: vm.syncWaiter() decls VM.h:1174/:1376.
- §E.3 never-armed rationale: without construct-true, an asyncJoin
  settle on an OPEN registrant would decrement a counter that was
  never incremented, wrapping the uint64 and hanging the §E.2 exit
  predicate (reads "keepalive == 0").
- §D.2: the verdict re-times because a docs-only round cannot run
  the construction bench; UNGIL-PLAN.md:250 binds this spec to
  record (not redesign) - the supersession only moves the gate.
- §B.6/§E supersession targets unchanged from rev 6 ("records it" =
  records the override).
- §J.3: "GIL-on releases it here" = GIL-on routes
  unlockAllForThreadParking at the same park sites, so holding
  m_lock GIL-off would be a strict regression.
- §I: "pass U17's positive arm and ship broken" - U17's spawned
  tests cannot distinguish TID!=0 from isSpawned; only the new
  negative arm can.

## Rev 8 (2026-06-06) - whole-design cross-check vs the COMPOSED
six-spec system: 13 findings (3 blockers, 10 majors), all upheld and
resolved in-spec. The five SPECs stay frozen; collisions recorded as
SUPERSESSIONS citing both sides.

1. (blocker) U20/E.2 "no access transition holding any api-rank
   lock" contradicted frozen api 5.9(e)'s sanctioned rank-4 shape
   (NLS::m_lock across GIL reacq, SPEC-api.md:271; landed
   LockObject.cpp:334-380). The U20-compliant alternative (hold
   access, block on m_lock) deadlocks GC: a ParkingLot-blocked
   waiter holding access never polls GSP/stop bits (heap §9
   RHA/AHA contract), cycle GC<-W.access, W<-m_lock, m_lock<-H,
   H<-GC-resume. FIX: §E.2 rank-4 exemption - block on NLS::m_lock
   only token+access-RELEASED, reacquire (gated) while holding it;
   acyclic because every m_lock waiter is access-released and no
   GC/§A.3 conductor acquires NLS::m_lock. U20 lints the order.
2. (major) §J.3 "m_lock+token reacquired on EVERY wake" was
   unimplementable vs kept api 5.4/5.6 loops (condition re-check
   under rank-3 locks; 5.9(e) forbids reacq holding rank 1-3). FIX:
   per-quantum wakes poll ONLY lock-free state (waiter atomic +
   lite trap/stop bits) under the rank-3 lock; full reacquisition
   exactly once at final exit after rank-3 release. U2's
   one-quantum bound re-derived from the lite-bit poll.
3. (major) No merged lock table; rev-7 §LK "Rank 3" collided with
   heap rank 3 (VMManager::m_worldLock); api 5.9 never anchored.
   FIX: §LK rewritten as the merged table - heap 1 < api 1 (TM) <
   api 2 (PWT/affinity) < api 3 group (queueLock/listLock/inboxLock/
   joinLock, mutually unnested) < heap 2-10b < leaves; NLS::m_lock
   = long-hold class outside heap 2-10/api 1-3; negative edges
   normative (no heap 2-9b holder takes api locks; no 10a/10b
   holder takes api<=3; conductors take no api lock; api 1-3
   holders never transition access). ACYCLICITY: every cross edge
   points inward (api->10a only via §C.3; api locks never wrap heap
   2-9b - verified by tree walk: no 10a holder takes listLock;
   notifiers order store-then-LL; api locks never wrap heap 2-9).
4. (major) §A.1.6 install nested ScratchBufferRegistry ->
   VMLiteRegistry::lock -> scratchBufferLock, three "leaves" two
   deep vs vmstate §6.5.1/§7 "no lock while held". FIX: §LK.6
   re-ranks VMLiteRegistry::lock outer-of-leaves with allowed inner
   set {scratchBufferLock, atomic ops, fastMalloc}; SBR outside it;
   SUPERSESSION vs vmstate §6.5.1/§7 recorded. ~VM walk now
   collect-unregister-release THEN client work (no GBL-rank
   transition under the registry lock).
5. (major) §A.3.6 ~VM walk DCT'd FOREIGN threads' GCClients,
   violating heap I4 "lifecycle on the using thread" and dangling
   the owner's §10A.1 TLS slot + machine-thread registration. FIX:
   remote detach SUPERSESSION (heap I4 + §10A.1, both sides): walk
   marks-dead + unregisters (client set + machineThreads); destroy
   deferred to owner TLS death (or owner already dead); heap §10A.1
   TLS slot becomes {client, epoch}, consults epoch-compare first.
6. (major) §C.1 third arm let a foreign atomic CAS/RMW hit an SW=0
   AS object under the cell lock, racing the owner's E2-elided
   UNLOCKED AS fast-path stores (jit §5.5 makes the lock sufficient
   only AFTER SW=1) and skipping OM §4.6's first-foreign-write
   fire+publish. FIX: AS pre-lock stage - SW=0 + foreign TID =>
   OM §4.6 per-event STW (fire PRECEDES lock, I10b), RESTART probe;
   only SW=1/owner enters the locked CAS/RMW. New U5/U28 amplifier:
   owner unlocked AS store storm vs foreign CAS, SW initially 0.
7. (major) Atom-shard + SymbolRegistry leaf locks are reachable
   under MSPL/BVL/9b via in-lock sweep destructors (~JSString ->
   last StringImpl deref -> removeDeadAtom / registered-symbol
   remove), which heap §6 leaf row + vmstate §7 forbade. FIX (the
   cheap arm, matches reality): §LK.8 destructor-leaf class,
   SUPERSESSION vs heap §6 leaf row + vmstate §7 (both sides);
   soundness = fastMalloc-only, acquire-nothing, never-wait
   (vmstate I7 extended to SymbolRegistry). No cycle existed; this
   legalizes the edge so rank-asserts encode it.
8. (blocker) GC-stop park leg for N threads in ONE VM had no owner
   (heap Dev 8 chartered "VMM trap delivery" to Phase B; §A.3 was
   JSThreads-only; landed notifyVMStop is a per-VM state machine
   that double-transitions/asserts with 2 same-VM observers; heap
   §13.5a-g hooks assumed one parked thread per VM). FIX: §A.3.8 -
   GC reason fans out per §A.2.3, per-thread tickets, notifyVMStop
   per-entered-thread (Mode keyed on all-parked/released), heap
   §13.5a/g re-ruled per currentThreadClient() with per-thread
   m_releasedByGCPark pairing; SUPERSESSION vs heap annex hook
   shape + VMManager.cpp recorded; IM row added (IH). New U29 +
   spawned-conductor amplifier.
9. (major) Main/embedder GIL-off entry lost heap-access + ACT after
   §B.3 deleted JSLock forwarding (silently missed stack roots).
   FIX: §F.1 main/embedder bullet now explicit - first entry
   creates carrier lite + GCClient (main reuses original), runs ACT
   (I4(b) addCurrentThread), every lock() runs gated AHA
   (idempotent, heap F8 step 0), depth-0 unlock releases; U27/U-T6
   negative arm: spawned-conductor GC scans an entered embedder's
   stack.
10. (blocker) Wasm-memory escape: §I refuses EXECUTION only; shared
    views over a main-created WebAssembly.Memory reach spawned
    threads as plain TA accesses; rev-7 §N.6 excluded wasm memories
    => grow/detach UAF. FIX: §N.6 now INCLUDES wasm-backed buffers
    - grow's BoundsChecking reallocation publishes length-first and
    quarantines the old BufferMemoryHandle base to the next heap
    §10 stop (epoch shape); detach likewise; racing old-base reads
    stale-but-safe, writes lost (SAB-admissible raciness). U28
    amplifier: spawned TA reader vs main grow storm. §I cross-ref
    fixed.
11. (major) §A.1.3 root/handle walk (and §A.1.6/§K.1 scans,
    §A.1.5/§A.2.3 fan-outs) iterated the process-global registry
    with no per-VM filter - cross-VM rooting/trap injection. FIX:
    all walks filter lite->vm == the target VM; two-VM amplifier
    arm added to U-T1.
12. (major) SD4 self-contradiction: §C.4 deleted the 4.5-1a gate
    unconditionally while the SD footnote kept SD4 per-variant.
    RESOLUTION (a): the AtomicsObject.cpp:613-621 deletion is
    gilOff-conditional; gate KEPT GIL-on; SD4 stays per-variant; NO
    third both-modes delta; master rule intact. (Alternative (b)
    both-modes rejected: would need a GIL-on spawned-park clause
    and a third corpus edit for no behavioral payoff.)
13. (major) §A.3.7 GIL-off atom routing broke vmstate §4.3's 14
    kept atomStringTable asserts ("None relaxed (ex-M5)" frozen).
    FIX: explicit SUPERSESSION - the 14 sites become "gilOff ?
    sharedAtomStringTableEnabled() : tables equal"; GIL-on/flag-off
    unchanged; IU row for Identifier.cpp:77, Completion.cpp:63-287,
    Heap.cpp:2348 et al.

### IM rev-8 delta rows (extends the rev-7 relocated table)
- VMManager.{h,cpp} + heap/Heap.* §13.5a-g hooks = A.3.8 per-thread
  GC parking (IH; IJ R1 cross-ref)
- wasm BufferMemoryHandle/BufferMemory.* = N.6 grow/detach
  quarantine (IH)
- Identifier.cpp + Completion.cpp + Heap.cpp atom-assert sites =
  A.3.7 supersession (IV)
- HandleSet/ScratchBufferRegistry/VMLiteRegistry rank rows = LK
  merged table (IV, IH)

### Relocated normative annex - §E.7.3 full hook mechanics (spec
points here)
Installed hooks onAddPendingWork/onScheduleWorkSoon/
onCancelPendingWork (DeferredWorkTimer.h:110-112) BYPASS
m_pendingTickets and run INLINE on the caller (landed dispatch
unconditional, DeferredWorkTimer.cpp:204/:234/:266-269). hookManaged
is set at addPendingWork iff hooks installed AND registrant
main/embedder; EVERY dispatch site (scheduleWorkSoon,
cancelPendingWork) checks it BEFORE the hook branch; internal
tickets stay internal on ANY calling thread, incl. on-carrier -
hooks never see a ticket that skipped onAddPendingWork. Off-carrier
settle/cancel appends {ticket, task-or-cancel} to the
m_pendingLock-guarded handoff queue; the embedder does NOT pump
DWT's timer, so internal-arm scheduleWorkSoon entries are NOT
timer-scheduled - the handoff flush + every §F.1 drain point EXECUTE
them inline on the carrier under its token (incl. E.4(b) retire +
m_promise clear); onCrossThreadWorkEnqueued (REQUIRED together with
the other three, boot-checked; never runs JS) drives them to
completion; fallback vm.runLoop().dispatch of the flush (else
parked-main settle deadlock; U-T9 hook arm).

### Rationale/cites compressed out of the spec body in rev 8
- §A.3.2b: "(i) carries soundness" because AHA/RHA brackets are
  unenumerable (heap §9 requires bracketing every indefinite
  block); (ii) post-wake polls are defense in depth.
- §A.3.8 cite anchors: notifyVMStop VMManager.cpp:404-590 (per-VM
  active counting, duplicate-dispatch atomic :321,
  RELEASE_ASSERT(m_targetVM==&vm) :218/:580); heap hooks
  gcWillParkInStopTheWorld/didResume :172-179/:435; GC-bit
  keep-parked 5b :354-363; re-check-while-parked 5g :510-557.
- §C.4 gate cite: AtomicsObject.cpp:613-621 isJSThreadCurrent() =>
  throwVMTypeError; ThreadAtomics.cpp:536-541 is the G11 embedder
  gate, kept and re-pointed.
- §E.2 rank-4 cites: releaseAccessSlow Heap.cpp:2580-2595; api
  5.9(e) one-permitted-shape SPEC-api.md:271; landed contended
  hold/cond.wait LockObject.cpp:334-380 (UNGIL-PLAN K5).
- §F.1 ACT cites: JSLock::didAcquireLock forwarding JSLock.cpp:
  159-164 (deleted GIL-off, §B.3); GCClient AHA idempotence heap F8
  step 0; machineThreads addCurrentThread heap I4(b)/I12.
- §I cites: WebAssemblyFunction.h:75-90/:101-106 warm IC dispatch;
  ThreadManager.cpp:157 C++ gate.
- Line-number cites dropped during rev-8 byte compression remain
  valid as of jarred/threads 2026-06-06; authoritative copies in
  the rev 5-7 entries above.

Byte budget: spec at 49996 bytes after compression; §E.7.3
mechanics, LK acyclicity proof, and per-finding rationale relocated
here. GIL-on observable behavior remains unchanged except SD6/SD7
(SD4 explicitly resolved per-variant, finding 12).

# REV 9 (2026-06-06) - whole-design cross-check vs the composed
six-spec system: nine findings resolved (two blockers, seven
major). Spec stays <=50000 bytes; the full INV/SD/§T text moved
here as NORMATIVE ANNEXES (IM precedent).

## Rev-9 findings and dispositions

1. BLOCKER, §C.3: the under-listLock §9.5 re-validation could hit
CONVERTING indexed arms (CoW materialize, Int32/Double->Contiguous
per §C.1) - allocation and per-event STW while holding listLock
(api rank 3), violating §LK negative edges and composing into a
real cycle (requester holds listLock waiting for all-parked; a
second waiter blocked on listLock still holds heap access, not at
a poll site). Fix in §C.3: (a) the PRE-ENQUEUE validation (api 5.6
step 1, api:229 - previously a PLAIN read) now routes through the
§9.5 atomic load, forcing conversion lock-free BEFORE enqueue; (b)
shape-monotonicity lemma: after a §9.5 touch a slot only moves
among {flat/Contiguous lock-free, AS/dictionary cell-locked} arms,
never back to a converting arm, so the under-listLock re-load is
alloc/STW-free; (c) defense: a convert-needed shape at
re-validation is handled exactly like the rope case (dequeue,
unlock, convert, fresh enqueue). New corpus arm: wait/waitAsync on
Int32/Double/CoW index (first-ever atomic access) racing a
notifier. Lemma sketch: §C.1 converts CoW/Int32/Double on FIRST
atomic access; Contiguous->AS/dictionary transitions land in the
cell-locked third arm; no transition re-creates CoW/Int32/Double
on a §9.5-touched object (OM I34/I35 forward-only shape order);
AS/dictionary arms never allocate under the cell lock (OM I20);
api3->10a is the already-legal §LK cross edge.

2. MAJOR, §D.1 vs §LK: rebias needed ThreadManager state
(m_threads liveness read, m_freeTIDs reissue), both under
TM::m_lock (api rank 1), but §LK forbids GC/§A.3 conductors
acquiring ANY api lock. Resolution: two-phase handshake. PRE-STOP,
a mutator-side pass under TM::m_lock snapshots the dead-TID set
into a conductor-readable buffer; the conductor restamps
world-stopped FROM THE SNAPSHOT ONLY; m_freeTIDs release runs
POST-RESUME on a mutator under TM::m_lock, ordered before the
>=75% RangeError gate lifts. Soundness: spawn in the shared VM is
blocked by the RangeError window for the whole interval;
concurrent lazy-carrier creation (other VMs, TM process-global,
their threads NOT stopped) only ADDS live TIDs and cannot
resurrect a snapshotted-dead TID (a dead TID has no lite, no TLS
map entry, and TM never reissues before the post-resume release).
§LK negative-edge row annotated with the sole sanctioned
interplay. U-T12 re-scoped; two-VM TM-churn amplifier added.

3. MAJOR, §A.3.2b: the stop-bit gate + NVS park inside
acquireHeapAccess silently extended frozen heap §10A ("Single-
client mode unchanged: never blocks") and the F8 enumerated AHA
step list. Now an explicit SUPERSESSION citing both sides (IH
row), with the ordering argument for the new pair (OUTSIDE the F8
GSP seq_cst proof): stop-bit fan-out under VMLiteRegistry::lock
precedes conductor counting; an AHA that CASed HasAccess before
observing the bit is still an entered, unparked thread the
conductor waits on (defense leg (ii) closes the race). The park
follows F8's mandatory-revert shape: seq_cst exchange->NoAccess
BEFORE parking, so both the GC barrier and the §A.3 conductor
observe NoAccess. GIL-on/flag-off AHA stays byte-identical to
frozen F8.

4. MAJOR, §A.3.6 TID text vs three frozen specs: GIL-off nonzero
carrier TIDs contradicted vmstate §6.7 ("Main carrier tid stays
0"), api §7 (mainThreadTID=0; TID note) + api 4.1 ("thr.id (main
0)"), and OM §2's tid-0 zero-overhead remark. Recorded as TID
SUPERSESSIONS (IV rows; both sides): vmstate §6.7 is GIL-ON-ONLY;
ThreadState.tid for main/embedder STAYS 0, so thr.id/
Thread.current.id are unchanged and there is NO new SD; the
carrier lite TID is a separate nonzero TM allocation from the same
2^15 space (I17 exhaustion accounting includes carriers);
currentTID() GIL-off returns the CARRIER TID (it feeds tagging/TTL
consumers, never JS); api 5.2's lite->tid==ts->tid equality is
SPAWNED-only - main/embedder TS.tid and carrier TID intentionally
diverge. OM §2's tid-0 note is a perf remark only: GIL-off
main-allocated butterflies carry the nonzero carrier tag,
correctness unaffected (both-modes note, not an SD). This kills
the JSLock.cpp:151 two-embedders-share-tag-0 race an implementer
following frozen vmstate §6.7 would have shipped.

5. MAJOR, §E.7.5 (NEW): the api 5.5a schedPump pump task P (G28)
and the api 5.6 waitAsync finite-timeout timer were left on
vm.runLoop(), which E.7.3's own premise says a hook-installing
embedder does not pump - stranding lock grants and timeouts under
Bun. Ruling: with hooks installed BOTH route through the rule-3
shape (m_pendingLock handoff queue + onCrossThreadWorkEnqueued +
inline execution at §F.1 drain points on the carrier); api
5.5a-P's "GI" owner mark is void under hooks. No hooks: landed
vm.runLoop() dispatch/dispatchAfter. Corpus arms (U-T9/U-T11,
hooks on): two spawned threads contend asyncHold while main is
parked in a permitted sync wait; spawned waitAsync finite timeout.

6. BLOCKER, multi-VM vs heap I13: GIL-off multi-VM support
(§A.3.6 carriers per-(thread,VM), §F.1 embedder clients, rev-4
finding 5) composed with heap I13's one-sticky-shared-server
RELEASE_ASSERT (Heap.cpp:4097-4124) into a process abort on Bun's
Worker pattern (second VM + Thread/second embedder thread).
Resolution: option (b) - new U0b. GIL-off, exactly ONE VM per
process (the sticky-shared-server VM) may hold per-thread clients;
any other VM refuses Thread spawn with a RangeError and keeps the
GIL-on single-migrating-client + real m_lock protocol for
multi-embedder entry (GIL mode is per-VM, not per-process). heap
I13 KEPT, no supersession. Corpus: second-VM spawn refused;
second-VM two-embedder entry green beside the shared VM. IU row
for Heap.cpp:4097-4124. Lifting I13 to N shared servers stays a
post-v1 renegotiation (would re-derive heap §10/§10D + VMManager
for two concurrent shared servers).

7. MAJOR, §A.1.7 (NEW): cross-thread READERS of rerouted Group-3
state were unruled - SamplingProfiler.cpp:391-431 suspends one
m_jscExecutionThread and reads m_vm.topCallFrame from the profiler
thread (null/asserting once rerouted); VMInspector/$vm kin.
Ruling: every off-thread reader (i) resolves the TARGET thread's
lite via the registry (suspended-thread identity, registry lock,
target suspended), (ii) is refused GIL-off with a defined error,
or (iii) is proven on-thread. v1: SamplingProfiler samples only
the main/embedder carrier via (i); spawned threads unsampled
(--cpu-prof stays useful); N-thread sampling post-ungil. New
audit U-T8d (under U-T8b) enumerates off-thread readers of every
rerouted field. IM rows: SamplingProfiler.{h,cpp},
VMInspector.cpp.

8. MAJOR, §I wasm-GC: heap §5.5/manifest 11's RELEASE_ASSERT
(JSWebAssemblyInstance.cpp:142) made MAIN-thread wasm-GC
instantiation a process abort under useJSThreads, while §I/U17
claimed "main/embedder wasm never throws". SUPERSESSION (both
sides; the §5.5 never-populate rule itself stands): a
hasGCObjectTypes() precheck BEFORE instance construction throws
WebAssembly.LinkError (compile-side: CompileError); the
RELEASE_ASSERT remains only on non-JS-reachable paths. U17
NEGATIVE arm re-scoped to NON-GC wasm; new POSITIVE arm asserts
the graceful refusal. Not an SD (was an abort, not a behavior).
IU row; U-T13 re-scoped.

9. MAJOR, §N.5 cost: the blanket cell lock on generator/async-
function internal-field transitions serialized every await/yield
(~22-cycle JSCellLock + loss of DFG/FTL PutInternalField inlining)
- plausibly the largest GIL-off serial regression, uncalled-out.
Narrowed to a single-word resume-claim CAS on the state field
(SuspendedX->Running); the loser takes the existing "already
running" TypeError; a generator legitimately has at most one
resumer, so frame/field stores while claimed stay PLAIN and
tier-inlined. Cell lock retained only for multi-word cases the
U-T8c audit names. §B.5/BENCH.md gains an async+generator
microbench line; the CAS design is itself the named contingency.

## Rev-9 NORMATIVE ANNEX 1: INV full text (IDs frozen)

- U0 config gate matrix (§0).
- U0b GIL-off, exactly one VM per process (the sticky-shared-
 server VM) holds per-thread clients; other VMs spawn-refuse
 (RangeError) and keep the GIL-on embedder protocol; heap I13's
 assert never fires in supported configs (§0).
- U1 GIL-off JS thread: registered lite for the ENTERED VM, unique
 TID, live token, TLS tag == CURRENT lite TID (§A.3.6/J.7); tid 0
 never installed; multi-VM swap.
- U2 VM-wide trap observed by a parked T within one quantum, both
 GIL modes - carrier = §J.3's lock-free lite-bit poll, NOT token
 reacq; terminate-while-parked (§A.2).
- U3 lifecycle order (§B.1-2/E.2): lite -> ACT -> alloc; Strong
 clears -> access release -> DCT -> unregisterLite. [r29: U3
 amended - see ANNEX EXIT1 as amended by rev 29; this row's
 order superseded.]
- U4 §A.3 stop: every entered thread parked/not-entered/
 access-released; entry during stop parks; no access-released
 thread runs JS mid-stop (2b); wake-during-stop amplifier.
- U5/U6 §9.5 atomicity + D3/D7 in-body; CAS-storms all arms;
 dict-delete-vs-CAS; restricted AS; convert-first (incl. §C.3
 pre-enqueue conversion); SW=0 AS pre-lock arm; §C.3 I10 arms.
- U7 completion <=> fn returned && queues empty && keepalive==0,
 OR termination (§E.2/E.5).
- U8/U9 keepalive: at-most-once decrement; no underflow
 (never-armed never decrements; mutual-asyncJoin-OPEN arm); no
 missed shutdown (§E.3).
- U10 settles per §E.4 (registrant iff inboxOpen, else main);
 never a foreign microtask queue (I11).
- U11 join/asyncJoin see Phase!=Running only post-close; join sees
 post-fn macrotask effects.
- U12/U13 nested spawned JSLockHolder depth-counted; APILock
 predicate true on host-call paths GIL-off (§F.2).
- U14 spawned DAL no-op; embedder DAL excludes only embedders;
 embedder C test incl. §F.1 drain.
- U15 §G policy; G11 TypeError preserved (api I18); spawned
 sync-wait OK; main parks release m_lock (§J.3 notifier arm).
- U16 concurrent Symbol.for(one key) => one symbol.
- U17 §I wasm throws from spawned threads, both modes, incl.
 warm-call; NEGATIVE: main/embedder NON-GC wasm never throws;
 POSITIVE: wasm-GC under useJSThreads => LinkError, no abort.
- U18 rebias: no live dead-TID tag post-stop; restamp (from the
 §D.1 pre-stop snapshot) before reissue; spawn-storm past 2^15.
- U19 GIL-on fallback corpus green after every U-task, unchanged
 EXCEPT SD6/SD7 (edited once).
- U20 lint: inboxLock/joinLock never nested; leaf locks never
 across user JS; no token/access transition holding any api rank
 1-3 lock; rank-4 across transitions only per §E.2 (a)->(b).
- U21 bench (§B.5, incl. the r9 async/generator line).
- U22 reactions on the settling thread; AsyncTicket on the
 REGISTERING thread (dead=>main); queues owner-only (§E.1b/I11).
- U23 per-entry record correct under entry/exit churn; fan-out
 reaches every entered T of THIS VM (§A.1.5).
- U24 DWT: post-settle ticket out of m_pendingTickets, Strong
 cleared; shell exits; hooks hookManaged-only; handoff wake; Bun
 dead-registrant settle; §E.7.5 pump/timer arms (§E).
- U25 inboxOpen once pre-fn, spawned only; increment sites assert
 spawned+open (§E.1).
- U26 §K: concurrent String(0.5)/split/lazy first-touch - one
 init, no race; full GC during winner's init (no deadlock).
- U27 ~VM walk: token-free carriers mark-dead + deferred-
 destroyed; epoch-stale TLS (both maps - carrier + heap §10A.1
 client slot) never consulted live; teardown storm;
 spawned-conductor GC scans an entered embedder's stack
 (§A.3.6/§F.1).
- U28 §N: no UAF/torn builtin internal state; map.set + Date
 storms; rope race; generator double-resume (CAS claim);
 detach/grow-vs-read incl. wasm memory (no UAF).
- U29 §A.3.8: GC with >=2 threads entered in one VM - per-thread
 park/release; no per-VM double-transition/assert; per-thread
 willPark/didResume pairing.

## Rev-9 NORMATIVE ANNEX 2: SD full text (IDs frozen)

- SD1 join settles at close (queues empty + keepalive 0), not
 fn-return (§E).
- SD2 completion drains OWN queues till empty (GPO).
- SD3 tickets settle on the REGISTERING T, dead=>main (§E.4).
- SD4 spawned TA sync wait allowed GIL-OFF ONLY (was TypeError;
 gate kept GIL-on); tests per-variant (§C.4/§G).
- SD5 notify() no yield point; parallel waiters (§C.5).
- SD6 main TA single-flight lifted (was second-wait throw, D8);
 per-wait nodes, D9 quanta, both GIL modes (§C.6/§A.2.6; flag-off
 untouched); GIL-on corpus edited (incl. terminate-parked arm).
- SD7 wasm on spawned threads: TypeError both modes (§I); GIL-on
 corpus edited.
- SD8 terminate parked: Failed completion, residue to main (§E.5).
- SD9 TID exhaustion RangeErrors till next rebias (§D.1).
- SD10 ordinary-promise reactions on the SETTLING thread (§E.1b).
- SD11 spawned TA waitAsync settles main-side, no keepalive
 (§E.3).
- SD12 asyncJoin: no keepalive; registrant may close first;
 dead=>main; mutual/self never deadlocks (§E.3).
Not SDs: §N.5's TypeError; the §I wasm-GC LinkError (was a process
abort); main thr.id (stays 0 per the A.3.6 TID supersessions).

## Rev-9 NORMATIVE ANNEX 3: §T full per-task scope (IDs frozen)

- U-T1 §A.1.2-7/A.3.6: mode-split Group-3 + per-VM GC root walk +
 lazy carriers + per-lite scratch/regexp + per-entry record +
 service table. Dark.
- U-T2 §A.2: per-lite VMThreadContext/VMTraps, fan-out,
 SignalSender off, D9/C.6 re-points, stack limits.
- U-T3 §A.1.1-3: loadVMLite; LLInt gilOff byte; VMEntryRecord
 m_vmLite. Dark.
- U-T4 §A.1.3/6: Baseline/DFG/FTL emission switch incl. non-baked/
 JITCode-resident scratch. Dark.
- U-T5 §A.3: thread-granular STW incl. §A.3.8 per-thread GC
 parking (notifyVMStop + heap §13.5 re-rule) + the A.3.2b AHA
 stop-gate supersession; DELETE stubs/witnesses/M7 tripwire (J.8).
 Gate: GIL-on no-regression + N-separate-VMs + $vm stop/resume vs
 access-released embedders; U4 + §A.3.8 amplifiers.
- U-T6 §B.1-3: per-thread GCClient spawn/teardown + lazy-carrier
 ACT (§F.1), token access, JSLock forwarding GIL-on-only; U0b
 second-VM behavior + corpus arms.
- U-T7 §B.4-6: TLC lite-relative addressing, all tiers; U21.
- U-T8 §F/J: tokens, predicate split + audit, DAL ruling,
 HandleSet lock, J.7 replacement.
- U-T8b §K + §N: inventories, rulings, protocols (U26/U28); F.2
 third-class audit rows; ~VM walk + epochs (§A.3.6); U-T8d
 off-thread reader enumeration (§A.1.7, incl. the SamplingProfiler
 v1 carrier-only ruling). Gates U-T9.
- U-T9 §E: runloop + settlement incl. E.7 hooks + §E.7.5 pump
 re-route, promise protocol, termination; corpus SD1-SD3/SD8/
 SD10-SD12 + hook + §N arms; U4 one-VM arm.
- U-T10 §C.1-2 (ENTRY GATE: Task-14 verdict recorded, §D.2): §9.5
 accessors all arms incl. AS pre-lock, flat-path SW discipline,
 ThreadAtomics re-home with D3/D7.
- U-T11 §C.3-6/G/J.3: PWT re-home + I10 re-validation (incl. §C.3
 pre-enqueue §9.5 routing + converting-arm corpus), 4.5-1a gilOff
 lift, G11 re-point, D2/D4/D8 (SD6 GIL-on edit), §G predicate,
 GILDroppedSection degradation + main-park m_lock release; §E.7.5
 timeout-timer arm. Corpus SD4-SD6 + §J.3 embedder arm.
- U-T12 §D.1: TID rebias inside a full shared-GC stop, two-phase
 TM snapshot/restamp/release; spawn-storm; two-VM TM-churn
 amplifier (rebias in VM A while an embedder lazily enters VM B).
- U-T13 §H/§I/A.3.7: SymbolRegistry lock; atom-swap GIL-on-only +
 14-assert supersession; wasm isSpawned checks + U17 arms (SD7
 edit) + wasm-GC LinkError precheck (JSWebAssemblyInstance.cpp);
 §N.6 wasm-memory quarantine.
- U-T14 close: U0/U0b gates; TSAN + amplifier; U19; default flip;
 IU dispositions.

## Rev-9 IM note

Rev-8 add-list (moved out of the spec's IM section): VMManager +
heap §13.5 hooks = A.3.8 (IH); wasm BufferMemory* = §N.6 (IH); the
14 atom-assert sites = A.3.7 supersession (IV). The rev-9 add-list
stays in the spec's IM section. Spec at 49999 bytes post-rev-9.

# REV 10 (2026-06-06) - whole-design cross-check round 2 vs the
composed six-spec system: six findings (2 blockers, 4 majors), all
verified REAL, all resolved in-spec. Spec stays <=50000 bytes via
rev-10 compressions (full text of compressed passages: rev-9
findings 2/3/4/8 + rev-8 E.7.3 annex remain the normative overflow).

## Rev-10 findings and dispositions

1. MAJOR, §LK.8 vs §F.3: in-lock-sweep destructors reach
HandleSet::m_strongLock under MSPL/BVL/9b - a leaf edge the composed
tables forbade (heap §6 leaf row: leaves "OK under 10a/10b, never
7-9b", heap §13.10f; §LK.8 enumerated only AtomString shards +
SymbolRegistry::m_lock). Verified chain: JSLockObject::destroy
(LockObject.cpp:63) -> ~NativeLockState -> Deque<Ref<AsyncTicket>>
m_asyncWaiters (LockObject.h:50) -> ~AsyncTicket
(ThreadManager.cpp:57) destroys a STILL-SET Strong<JSPromise> for
never-settled tickets (lock dropped with pending asyncHold waiters
is ordinary user code; the AT holds the only refs - DWT holds
TicketData, not the AT). GIL-off §F.3 routes every Strong free
through m_strongLock, so the sweeping mutator takes the edge.
No real deadlock (Strong free = list-splice + fastMalloc, acquires
nothing, never waits - the §LK.8 proof shape), but the mandated
rank asserts fire on a reachable path. FIX: §LK.8 destructor-leaf
class EXTENDED to HandleSet::m_strongLock (SUPERSESSION vs heap §6
leaf row + vmstate §7, both sides, IH row), same proof obligations.
The ~AsyncTicket assert (ThreadManager.cpp:57
currentThreadIsHoldingAPILock) gains a sweep-context arm GIL-off
(sweeper holds a token; assert satisfied by §F.2's redefinition -
recorded so U-T8's consumer table classes it "assert (token)").
Epoch-retire alternative REJECTED: heap §9 forbids retire() under
ranks 7-9b too - class extension is the only consistent arm.
ADJACENT RULING folded into §F.3: api 5.10/D5-companion heap
finalizers (addFinalizer lambdas; they CLEAR Strongs, needing
m_strongLock + access) execute GIL-off on an ENTERED thread WITH
access, OUTSIDE the heap §10 stop window (heap §10B(5) JS-finalizer
ban respected; conductor runs them after resume, before releasing
its own client's access). Amplifier (U-T7): dead-lock-object-with-
pending-asyncHold sweep storm under the §C corpus.

2. BLOCKER, nested foreign-VM entry: §A.3.6 supports nesting
(per-(thread,VM) carriers, LIFO {lite,tag} restore) and U0b keeps
a second GIL-on VM per process - but no section owned the OUTER
VM's token/access during the nested window. Walk: T entered in the
shared VM (token + HasAccess), host code enters VM2 (Bun JSContext
pattern); T's CURRENT lite/tag are VM2's, so shared-VM trap/stop
bits set in T's shared carrier are unobserved until LIFO restore,
while heap §10.4's access barrier and §A.3.2's conductor wait on
T's HasAccess unboundedly. Constructible deadlock: VM2 JS blocks
on a native lock held by a shared-VM spawned thread parked on the
GC stop bit; GC waits on T; T waits on the parked thread. FIX:
option (a), new §F.5 + U30. lock() on VM B while holding any other
VM A's token FIRST releases A's client heap access (F8
mandatory-revert shape: seq_cst exchange->NoAccess) BEFORE
installing B's carrier - T is "access-released" for BOTH the heap
§10.4 barrier and the §A.3.2 conductor predicate for the whole
nested window. Sound: A's JS frames stay alive via conservative
machine-thread stack scan (heap I4(b) - the thread remains
registered); T mutates only B's heap while nested. LIFO restore
re-acquires A's access §A.3.2b/§A.3.8-gated (parks if A is mid-stop)
and THEN observes A's deferred trap bits at the next poll site.
DELIVERY DEFERRAL recorded: outer-VM termination/§A.3 latency is
bounded by the nested window, NOT by U2 (U2 re-scoped to threads
whose CURRENT lite belongs to the polled VM); not an SD (GIL-on
nesting already defers via the handoff protocol). Rule applies per
nesting level (LIFO stack of releases). Option (b)
(RELEASE_ASSERT) rejected: kills Bun's documented JSContext-inside-
host-call pattern that U0b exists to keep. Corpus arm (U-T6):
shared-VM GC requested while an embedder thread is nested in the
second VM; IH row (heap §9 blocking-primitive note gains the
cross-VM-JS bullet), IV row (§6.4.4 nesting note).

3. MAJOR, §E.7.5 no-hooks routing: rev-9 finding 5 re-homed pump
task P + the 5.6 timer for HOOKS-INSTALLED only; "no hooks =>
vm.runLoop() as landed" recreates a join deadlock in the plain
shell. Walk: spawned A registers lock.asyncHold (keepalive +1)
while spawned C holds the lock; main join()s A (permitted, G34)
and parks per §J.3 - nothing pumps the main runloop. C releases:
schedPump dispatches P to vm.runLoop(); cycle: main's join waits
A's close (SD1) <- A's keepalive waits the grant settle <- grant
waits P <- P waits main's runloop <- main parked in join. GIL-on
the same program terminates (join settles at fn-return, api I20) -
a NEW GIL-off hang of a satisfiable program, not §E.3's
"intentional leak" (never-notified) class. The hooks-on path had
the same shape in miniature (carrier drain points suppressed at
park, §J.3). FIX: §E.7.5 re-routes BY REGISTRANT, hooks or not:
head-registrant SPAWNED => P runs INLINE on the releasing/
notifying thread (P is lock-free: clear m_pumpPending, tryLock,
settle via E.4 - it never runs JS; the GIL-on RL-turn rationale
(api G28 "GI") is void GIL-off because settle-enqueue is not a JS
execution point); the 5.6 finite-timeout timer for a SPAWNED
waiter becomes a DEADLINE on the registrant TS - E.2's quantum
wait sleeps min(10ms, earliest deadline) and expires it locally
(registrant loop alive: §C.3 waitAsync holds keepalive, §E.3).
Main/embedder registrant: hooks => rule-3 handoff (unchanged);
no hooks => vm.runLoop() as landed (main runloop is pumped
whenever main is not parked; if main parks, its OWN registrations
are §G-gated user choices - same class as api 4.6.2). Spawned-
registrant work thus NEVER routes through carrier drain points or
the VM runloop - kills the miniature too. Corpus (U-T11):
hooks-OFF join/asyncHold cycle above; spawned waitAsync finite
timeout hooks-OFF. E.2 wait gains the deadline-min clause (U11
touch); SD11 unchanged (TA waitAsync stays main-side).

4. BLOCKER, U0b vs history F7: per-VM GIL mode (U0b) composed
with the PROCESS-global gilOff Config byte (F7: second derived
byte in JSCConfig, = useJSThreads() && !useThreadGIL()) splits
Group-3 state for the second VM: in a gilOff process the byte is
globally true, so second-VM LLInt would route topCallFrame/
exception/stack-limit through the CURRENT lite while the VM's
GIL-on handoff machinery (vmstate §6.1.3 M4, J.5/J.6) keeps VM
members authoritative - stale stack limits, lost exceptions.
Codegen-time selection + §A.1.6 + the accessor "mode" branch had
the same unspecified granularity. FIX: two-level discriminator,
§A.1.3 rewritten (re-freeze of F7, both sides):
 - JSCConfig gilOffProcess byte KEPT but RENAMED semantics:
 "a GIL-off VM exists in this process" (same derivation site).
 FALSE => VM storage, one not-taken branch per LLInt site - the
 flag-off/GIL-on-process delta is unchanged from F7.
 - TRUE => LLInt loads a NEW per-lite byte VMLite::gilOff (L2
 append, copied from lite->vm at lite registration; a CURRENT
 lite exists whenever the byte matters - useJSThreads=1 installs
 m_mainVMLite/carriers for every VM, vmstate §6.4.4). lite byte
 0 => VM storage (second VM, GIL-on protocol intact); 1 => lite
 storage. Under U0b the per-VM mode = (vm is the sticky
 shared-server VM).
 - Baseline/DFG/FTL + §A.1.6 baked-vs-indirected select AT
 CODEGEN TIME on the COMPILED-FOR VM's mode byte (codeBlock->vm);
 code never migrates VMs. VM same-name accessors branch on
 vm.m_gilOff (per-VM member, source of the lite copy), NOT the
 Config byte. GC root walk (§A.1.3), §A.1.5 fan-outs, J.5/J.6
 handoff writes, L7 assert: all keyed per-VM.
 - U0b corpus row STRENGTHENED: the second-VM two-embedder arm
 must EXECUTE JS (throw/catch + deep recursion against the stack
 limit + a GC), not merely enter.

5. MAJOR, §J.3 vs §A.3.6: the per-quantum poll read "CURRENT lite
trap/stop bits", but §J.3's own park protocol releases m_lock +
token via unlockAllForThreadParking, whose release path runs the
§A.3.6 LIFO restore - CURRENT lite is the PRIOR lite (null for a
Bun thread that entered from native) for the whole park.
Main/embedder parks (join, cond.wait, TA/property Atomics.wait)
therefore missed termination (§A.2.4 D9) and §A.3/GC stop bits -
U2's bound broken; spawned threads unaffected (lite never
uninstalled). FIX: §J.3 amended - park sites CAPTURE the entered
VM's carrier lite pointer BEFORE the release; per-quantum polls
read the CAPTURED lite's bits + the waiter-state atomic, never
VMLite::current(). Lifetime proof recorded: a carrier dies only
at owner TLS death or the ~VM walk (§A.3.6); the owner is alive
mid-park, and ~VM while this VM's JS frames are live on a parked
thread is an embedder error (vmstate M6 precondition) - the
captured pointer cannot dangle. §A.2.4's D9 clause re-pointed:
"CURRENT lite" => "the polling thread's PARK lite (captured per
§J.3 at main/embedder sites; current lite for spawned)". U31.
C-API arm re-used: main parks in property Atomics.wait, second
embedder enters + notifies, conductor stops mid-park.

6. MAJOR, flag-off identity: §A.1.3's LLInt gilOffProcess
branches and §A.2.6's atomicsWaitImpl useJSThreads branch are
PRESENT (not-taken) in flag-off builds, falsifying three frozen
enumerations never cited as superseded: jit I1 ("LLInt differs
only by §5.4's gate branch", jit:180), vmstate R3's composed
flag-off bar, api I1 (byte-identical for its files). FIX: explicit
SUPERSESSION row added to §A.1.3 (both sides) enumerating the now-
permitted flag-off deltas: (a) one not-taken gilOffProcess branch
per LLInt Group-3 storage-selection site; (b) atomicsWaitImpl's
useJSThreads branch; (c) NOTHING else - §K/§N/§E/§F machinery is
gilOff-runtime-only or flag-on-only, audited + the list re-checked
at U-T14 (any new flag-off branch = gate failure). Where a Group-3
site already sits inside an ifJSThreadsBranch region, the gilOff
test NESTS under it - zero NEW flag-off branches at those sites
(emission rule, not optional). BENCH: §B.5 gains the line that the
flag-off --useJIT=0 bench gate (jit Task-13) is RE-RUN after the
one-time LLInt golden-disasm re-baseline and must stay in-noise -
Group-3 sites include the LLInt prologue stack-limit + exception
checks, squarely bench-gate-relevant under the zero-serial-cost
contract.

## Rev-10 INV additions (IDs frozen; extends rev-9 annex 1)
- U30 (§F.5): a thread nested in VM B holds VM A's token only in
 access-released state; A's conductors never wait on it; restore
 re-acquires gated and observes deferred bits before running A JS.
- U31 (§J.3): every main/embedder park-quantum poll reads the
 captured park lite of the VM it entered; no poll reads
 VMLite::current() after the park release.
- U2 re-scope note: latency bound applies per-VM to threads whose
 park/current lite belongs to that VM; nested windows defer (U30).

## Rev-10 §T deltas (extends rev-9 annex 3)
- U-T6: + §F.5 nested-entry implementation + nested-GC corpus arm.
- U-T7: + sweep-storm amplifier (finding 1).
- U-T8: consumer table rows for ~AsyncTicket assert + finalizer
 context (finding 1).
- U-T9/U-T11: §E.7.5 registrant-routing rework + hooks-OFF cycle
 arms (finding 3); E.2 deadline-min clause.
- U-T3/U-T4: per-lite gilOff byte + two-level LLInt selection +
 codegen-time per-VM keying (finding 4); U-T14 audits the flag-off
 delta list (finding 6).
- U-T12 unchanged; §D.1 spec text compressed to pointer (r9 F2).

## Rev-10 spec compressions (normative text preserved here/above)
- §A.3.6 TID-supersession block -> pointer to rev-9 finding 4.
- §D.1 two-phase mechanics -> pointer to rev-9 finding 2.
- §A.3.2b supersession ordering argument -> pointer to rev-9
 finding 3 (rule text retained).
- §I wasm-GC rationale -> pointer to rev-9 finding 8.
- §E.7.3 mechanics already pointed at the rev-8 annex; spec row
 tightened.

## Rev-10 IM add-list (extends the rev-7/8/9 rows)
- LockObject.{h,cpp} + ThreadManager.cpp:57 -> §F.3/§LK.8 (sweep
 Strong-free arm; assert re-class), IA.
- JSLock.cpp lock()/unlock() -> §F.5 nested-entry release/restore
 + §A.1.3 per-lite gilOff install, IV/IH.
- JSCConfig.h:104 -> gilOffProcess rename + per-lite byte (F7
 re-freeze), IJ.
- LLInt Group-3 sites + atomicsWaitImpl -> flag-off delta
 supersession rows (jit I1 / vmstate R3 / api I1), IJ/IV/IA.
- NativeLockState pump (api 5.5a P) + 5.6 timer -> §E.7.5
 registrant routing, IA.
- VMTraps/park sites (join, cond.wait, TA/property wait) -> §J.3
 captured-lite poll + §A.2.4 re-point, IV/IA.

================================================================
## rev 11 (2026-06-06) - review round: 6 findings (2 blocker, 4
major; 3 of the 6 were duplicates of the same m_gilOff gap)

### F1 (BLOCKER x2 + MAJOR, merged): vm.m_gilOff had no defined
assignment point; the only definition ("per-VM mode = vm is the
sticky shared-server VM", rev-10 F7 note at :1605-1608) was
dynamic and revertible. VERIFIED REAL against the tree:
Heap::noteSharedServerSticky fires only at second-client attach
(Heap.cpp:4106-4124, called from HeapClientSet::add;
m_isSharedServer.store(true) at :4312), i.e. at the FIRST Thread
spawn - long after the VM compiled code and registered
m_mainVMLite - and §10D REVERTS it (m_isSharedServer false at
:4755; the I13 comment at :4124 says the same server "may go
shared again"). A verbatim implementation either stamps gilOff=0
onto every lite at registration (split Group-3 brain: N GIL-off
mutators sharing VM-member topCallFrame/exception/stack limits)
or flips the mode dynamically, leaving pre-flip codegen with VM-
storage emission and no invalidation - both unsound; the F7
two-level discriminator rested on an undefined input, and U0's
"non-shared GC server refused at option validation" was
unimplementable as stated (option validation cannot observe
runtime shared-ness).
FIX (normative, §0 U0c + §A.1.3 re-point + §F.2 clause-(a)
discharge; re-freezes rev-10 F7 BOTH SIDES):
1. vm.m_gilOff is computed ONCE in the VM constructor - before
   m_mainVMLite registration (vmstate §6.4.4), before any entry
   and any codegen - and is IMMUTABLE for the VM's lifetime.
2. Assignment rule: m_gilOff = gilOffProcess && (this VM's heap
   WON the s_stickySharedServer CAS at Heap.cpp:4123 [r25
   line-drift fix; I13 RELEASE_ASSERT :4124]). The CAS
   doubles as the designation mechanism: the first VM constructed
   under gilOffProcess wins; later VMs lose => m_gilOff=0 and run
   the U0b GIL-on second-VM protocol (spawn RangeErrors).
   gilOffProcess itself is OPTION-derived at Config finalization
   (useJSThreads && !useThreadGIL && the U0 trio), so U0 becomes:
   pure options check at validation + the ctor-time CAS for the
   runtime designation.
3. EAGER sticky designation: under gilOffProcess the designate
   VM's ctor calls noteSharedServerSticky() while clientSet()==1.
   §10B.4 quiescence is trivially satisfied at birth (no entered
   mutator, no live API-lock holder), so the clause-(a) machinery
   is not needed on this path. SUPERSESSION (both sides) vs heap
   §5.1's "option && clientSet().size() EVER > 1" trigger: the
   size>1 call site STAYS (idempotent - m_isSharedServer already
   true) and gains RELEASE_ASSERT(gilOffProcess => the server
   VM's m_gilOff == 1), which fires if any later heap tries to
   become the server (I13 already RELEASE_ASSERTs one-per-
   process).
4. §10D reversion: never clears m_gilOff (it is not heap state);
   the Heap.cpp:4755 m_isSharedServer=false arm is conditioned on
   !gilOffProcess - under gilOffProcess the server stays ISS for
   process lifetime. Rationale: codegen and lite bytes were
   stamped against gilOff=1; un-sharing the server would not
   un-stamp them. Cost: a GIL-off process that joins all threads
   keeps shared-server overheads - acceptable, U0b already makes
   the mode per-process-singular.
5. Consequences now structural, not asserted-by-hope: the §F.2
   ISS-flip clause-(a) premise "flip pre-dates any GIL-off entry"
   holds by construction (flip at ctor, entry later); lite
   registration copies a final byte; Baseline/DFG/FTL codegen-
   time selection can never observe a mode change; no jettison/
   migration story is needed. Corpus arm (U-T3): compile-heavy
   single-thread run (forces all tiers), THEN first spawn, then
   Group-3 consistency checks (topCallFrame/exception/stack
   limits) on both threads.

### F2 (MAJOR): "WeakSet/WeakBlock free lists are GIL-serialized
today and fall through every SPEC-ungil bucket" - REFUTED.
Evidence: WeakSet::allocate's free-list pop / findAllocator walk
/ m_blocks append / WeakImpl construction already run under
MutatorSlowPathLocker(heap) when the server is shared - the
landed SharedGC round-4 lock at WeakSetInlines.h:69 (see the
in-tree comment at :49-66, which names exactly the lost/aliased-
WeakImpl race the finding describes and explains why the
construction is inside the section). U0 makes GIL-off imply ISS,
so the lock is live for every GIL-off mutator; MSPL is per-server
and was designed for N clients (SPEC-heap.md:56). WeakSet::
deallocate is DELIBERATELY lock-free with a recorded soundness
argument (WeakSet.h:121-131: reachable from in-lock-sweep
destructors, where MSPL would self-deadlock; sound because
conducted sweeps are world-stopped and mutator-concurrent MSPL
sweeps skip weak-bearing blocks). Sweep-time visits, finalizer
runs and m_newActiveWeakSets splicing are owned by heap T8 +
SPEC-heap §5.2(2). The only residue is the WeakSetInlines.h:44
assert, which the in-tree comment already re-points post-GIL
("becomes an access-held predicate (currentThreadClient())") -
rev 11 records it in the §F.2 fixed list as token+access
predicate, NOT an exclusivity consumer. No §K bucket needed; the
finding's premise ("the GIL is the sole serializer") is false in
the landed tree.

### F3 (MAJOR): debugger machinery neither designed nor refused.
VERIFIED REAL: the only mention was §A.2.3's fan-out of the
debugger trap to every lite, which would deliver N threads into
Debugger's singular pause state (m_currentCallFrame Debugger.h:
342, stepping mode, EventLoop pause nest); Bun ships an
inspector, so the path is reachable. FIX: new §A.2.7 - debugger
trap bit exempt from the §A.2.3 fan-out, delivered ONLY to
main/embedder carrier lites; Debugger entry hooks early-return on
a spawned lite (VMLite::isSpawned discriminator, same byte as
§I); spawned-thread breakpoints are defined no-ops; pause keeps
the landed single-threaded protocol on the carrier; attach/detach
+ CodeBlock-wide recompile/registration walks run under a §A.3
stop so spawned threads cannot execute mid-walk. SD13 (GIL-off
only - GIL-on keeps landed behavior, per-variant corpus). IU row
+ corpus arm (spawned thread crosses a set breakpoint without
abort or pause; main still pauses). N-thread debugging is
post-ungil work. §A.2.3's trap list reworded accordingly.

### F4 (MAJOR): §N.5 resume-claim CAS had no execution-tier
primitive. VERIFIED REAL: the check-then-store lives in builtins
(GeneratorPrototype.js:36 @putGeneratorInternalField(...,
@GeneratorStateExecuting) after the :60/:77/:91 state checks),
inlined by DFG/FTL as plain Get/PutInternalField; async-function/
async-generator/iterator-helper resumption rides the same
machinery; OM §9.5 excludes internal fields (§E.1b); no atomic
internal-field op exists in any tier. PromiseOperations.js/
PromiseConstructor.js contain no @putPromiseInternalField sites,
confirming §E.1b's native-restructure path is coherent and only
the non-promise types need the primitive. FIX (normative, §N.5):
NEW intrinsic @atomicInternalFieldClaim(cell, fieldIndex,
expected, replacement) -> boolean (true = won). Emission is
UNCONDITIONAL - all GIL modes and flag states - because single-
threaded it is observably identical to the landed get+compare+
put sequence; this avoids any mode-conditional bytecode. Tiers:
LLInt/Baseline call a slow-path host operation; DFG/FTL get a new
node AtomicInternalFieldClaim lowered to a seq_cst 64-bit
strongCAS on the field's EncodedJSValue word via the existing
internal-field offset machinery - keeping §B.5's async/generator
bench premise (one cheap inlined CAS per await/yield). Re-pointed
sites (U-T8c row): GeneratorPrototype.js resume head; AsyncFunction
/AsyncGenerator/iterator-helper equivalents enumerated by the
audit. RELEASE transitions (Running->SuspendedX/Completed) stay
plain PutInternalField - single owner while claimed. Flag-off
identity: the builtin bytecode changes uniformly => added to the
§A.1.3 permitted flag-off delta list as (b2); golden gates
re-baselined with it.

### rev-11 byte-budget relocations (NORMATIVE ANNEXES)
The four findings added ~3.3KB of normative text; to hold the
50000-byte freeze bound, the following sections' FULL text moved
here VERBATIM (rev-10 wording, BINDING - the in-spec summaries
keep every decision and defer to these annexes for the complete
protocol walks). Remaining in-spec prose was compressed without
semantic change (rationale clauses that already lived in this
file were shortened to their history pointers).

### Annex C1 (§C.1 full arm text - NORMATIVE, BINDING)

1. OM §9.5 atomic slot accessors (8g): atomicSlotCompareExchange /
 atomicSlotReadModifyWrite -> JSValue, ONLY plain structure/
 butterfly-backed own NAMED data slots + the indexed pair.
 NORMATIVE:
 - Lock-free arms (inline, flat OOL, segmented-fragment slots -
 receivers NOT OM-locked): seq_cst 64-bit CAS/RMW loop on the
 EncodedJSValue slot word; NO cell lock on the segmented arm
 (lock-held RMW would not serialize, U5).
 - Flat-path transition discipline (flat GROW = butterfly-CAS +
 copy, NO nuke - an old-butterfly CAS is silently lost).
 currentButterflyTID() != butterfly tag => FIRST the OM §2
 foreign-write SW-set DCAS, re-validate structureID + butterfly per
 I34, THEN CAS the slot. Validation failure restarts the WHOLE
 probe (I33-bounded); a completed RMW/CAS is NEVER re-applied.
 - Third arm: OM-locked regimes. Dictionary (I19/L3) and AS-shape
 (§4.6; Thread.restrict FORCES AS): probe + CAS/RMW UNDER the
 JSCellLock OM already requires. AS PRE-LOCK: the cell lock
 suffices only AFTER SW=1 (jit §5.5 owner AS fast paths UNLOCKED
 while SW=0) - SW==0 && currentButterflyTID()!=tag => FIRST the
 OM §4.6 first-foreign-write protocol (per-event STW,
 fire-then-publish (installerTID,1); I10b), then RESTART the
 probe; only SW=1 (or owner) enters the locked CAS/RMW. Lock
 REQUIRED (dictionary delete is I34-blind - a lock-free CAS could
 "succeed" on an absent property, U5); dictionary-ness re-checked
 under it.
 U5/U28 amplifier: owner unlocked AS store storm vs foreign CAS,
 same index, SW initially 0.
 - Indexed arm (8g re-freeze), by shape: CoW - materialize per OM
 §4.8/I35 first. Int32/Double - raw-word CAS REJECTED (history):
 first atomic access CONVERTS to Contiguous (owner direct; foreign
 SW-set DCAS first). Contiguous - flat arm verbatim. ArrayStorage/
 dict-indexed - third arm. §C.2 routes parseIndex hits here; one
 arm per shape.
 - Write barrier after success, as §9.5 orders.

### Annex C3 (§C.3 full text - NORMATIVE, BINDING)

3. PWT arming re-home + I10 re-derivation (F4 GIL-off; full walk
 + lemma: history r9 F1). The landed I10 closure is
 the JSLock; GIL-off the lost store+notify window REOPENS.
 NORMATIVE, BOTH arms: (a) the PRE-ENQUEUE validation (api 5.6
 step 1, api:229) routes through the §9.5 atomic load - forcing
 any CoW/Int32/Double conversion OUTSIDE listLock; monotonicity
 lemma: a §9.5-touched slot never returns to a converting arm, so
 the under-listLock re-load is alloc/STW-free (api 3 -> 10a
 legal). (b) enqueue under listLock; RE-VALIDATE SVZ(o[k],
 expected) via the §9.5 load STILL UNDER listLock; mismatch =>
 dequeue, "not-equal"; rope re-read OR convert-needed shape =>
 DEQUEUE TOO (eats one FIFO notify - the I10 class), unlock,
 resolve/convert via §9.5, FRESH enqueue (NO alloc/STW under
 listLock, ever). Notifier orders through listLock: a missed store
 notifies AFTER our enqueue. waitAsync settles via §E.4; sync
 parks per §J.3. U5/U-T11. Corpus: wait/waitAsync on an Int32/
 Double/CoW index racing a notifier. GIL-on unchanged.

### Annex A36 (§A.3.6 full text - NORMATIVE, BINDING)

6. Main/embedder carriers (vmstate §6.4.4). GIL-off EVERY thread
 uses a real carrier lite with a TM-allocated unique TID, lazily
 installed at first entry; m_mainVMLite (tid 0) is GIL-on-only.
 Carriers are per-(thread,VM) in a TLS VM->carrier map; lock()
 (still per-VM m_lock, §F.1) installs the entered VM's carrier as
 CURRENT lite AND swaps the jit P5/CS3 butterfly-TID-tag TLS to
 its TID, restoring the prior {lite, tag} LIFO on release (nested
 entry: §F.5). Install precedes any allocation/OM fast path; tag
 cleared at teardown; never tag 0 or a foreign-VM TID (TTL/§D.1).
 Spawned Threads single-VM in v1 (foreign-VM token
 RELEASE_ASSERTs). U1: TLS tag == CURRENT lite TID && lite->vm ==
 entered VM. JSLock.cpp:151 backstop REPLACED (§J.7). Lazy
 embedder TIDs count vs 2^15 (Dev 10; §D lifts).
 TID SUPERSESSIONS (both sides; IV rows; full text r9 F4):
 vmstate §6.7 "Main carrier tid stays 0" GIL-ON-ONLY; main/embedder
 TS.tid STAYS 0 (no SD); carrier lite TID = separate nonzero TM
 allocation (I17 counts carriers); currentTID() GIL-off = CARRIER
 TID; api 5.2's lite->tid==ts->tid SPAWNED-only; OM §2's tid-0
 note perf-only (main butterflies carry the carrier tag, not an
 SD).
 ~VM teardown (SUPERSESSION: vmstate M6 + §6.5.1 assert vs this).
 [r30: this clause is AMENDED - text of record: the rev-30 A36
 amendment record (full server-side detach moves into the walk;
 deferred dtor restricted to non-VM memory, degenerate
 dead-detached path; M11/M12 no-op argument; collection ordered
 BEFORE the EXIT1.9 ~VM wait). The clause below stands where the
 amendment does not differ.]
 Foreign carriers may still be REGISTERED at ~VM => M6 replaced:
 ~VM COLLECTS this VM's carriers under the registry lock (each
 token-free, else RELEASE_ASSERT), unregisters them, releases the
 lock before client work. Remote detach (SUPERSESSION: heap I4
 "lifecycle on the using thread" + §10A.1, both sides): the walk
 marks each foreign GCClient dead-detached (out of the server's
 client set + machineThreads); client + lite destruction DEFERRED
 to the owner's TLS destructor (immediate if owner dead). heap
 §10A.1's TLS slot becomes {client, epoch}; stale epoch => null
 (no UAF). §6.5.1 assert => "registry empty for this VM". VMs
 carry a process-monotonic epoch; the TLS map stores {VM*, epoch,
 carrier}; lock() compares epochs BEFORE the cached carrier. I20
 holds (dead carriers token-free, never CURRENT). U27 + teardown
 storm.

### Annex E7 (§E.7.3-5 full text - NORMATIVE, BINDING)

3. Embedder-hook ruling (USE_BUN_EVENT_LOOP; NORMATIVE mechanics:
 r8 annex). hookManaged = hooks installed AND registrant
 main/embedder; hooks fire ONLY for hookManaged tickets, ONLY on
 the carrier; spawned registrations ALWAYS internal arm.
 Off-carrier settle/cancel with hooks: m_pendingLock-guarded
 handoff queue, flushed + EXECUTED at §F.1 drain points on the
 carrier under its token (incl. E.4(b) retire); wake = FOURTH hook
 onCrossThreadWorkEnqueued (no JS; boot-checked REQUIRED);
 fallback vm.runLoop().dispatch. U24 Bun arm: dead-registrant
 settle with hooks.
4. No-hooks runloop wake: off-carrier E.4(b) retire would strand
 a parked shell (RunLoop::stop fires only in DWT's timer
 callback); internal-arm cancel/retire while
 m_shouldStopRunLoopWhenAllTicketsFinish dispatches an ON-loop
 re-check via vm.runLoop().dispatch(); emptiness reads under
 m_pendingLock. U24 shell arm.
5. Remaining vm.runLoop()-bound paths (api 5.5a schedPump's pump
 task P, G28, + the 5.6 waitAsync finite-timeout timer) route BY
 REGISTRANT, hooks or not (r10; rev-9's hooks-only ruling
 deadlocked the hooks-OFF shell - r10 F3):
 - HEAD registrant/waiter SPAWNED: P runs INLINE on the
 releasing/notifying thread (lock-free: clear m_pumpPending,
 tryLock, settle via E.4; runs NO JS - G28's "GI" rationale void
 GIL-off); the 5.6 timer becomes a DEADLINE on the registrant TS
 (E.2's wait sleeps min(quantum, earliest deadline), expires
 locally; loop alive - §C.3 waitAsync holds keepalive). Spawned-
 registrant work NEVER routes via carrier drain points or
 vm.runLoop().
 - MAIN/EMBEDDER registrant: hooks => rule 3; no hooks =>
 vm.runLoop() as landed (a parked main's own registrations are
 §G-gated user choices - api 4.6.2 class).
 Corpus (U-T9/U-T11): the r10 hooks-OFF join/asyncHold cycle;
 spawned waitAsync finite timeout, hooks on AND off.

### Annex A16 (§A.1.6 full text - NORMATIVE, BINDING)

6. Scratch buffers. Baked scratchBufferForSize ADDRESSES (DFG/FTL)
 are shared by N threads. NORMATIVE GIL-off: process-wide
 ScratchBufferRegistry (§LK rank, outside VMLiteRegistry::lock;
 monotonic indices + index->size map, never freed); each lite
 holds (L2) an append-only segmented pointer table (lock-free
 reads). Every baked site becomes loadVMLite -> segment ->
 [index], all tiers incl. OSR-exit + calleeSaveRegistersBuffer;
 a buffer exists at (lite, index) BEFORE the code runs (install
 fans to VM lites; registration backfills); install nesting SBR
 -> VMLiteRegistry::lock -> scratchBufferLock LEGAL (§LK re-rank;
 SUPERSESSION vs vmstate §6.5.1/§7, both sides). Non-baked:
 CURRENT lite's table by size-class - IMPLEMENTS reserved
 VMLite::scratchBufferForSize (re-freeze: vmstate:534-539); Group
 5 REPURPOSED as the lite's buffer-ownership list (under
 scratchBufferLock; backs jit-R2 scan + teardown free); L1-L5
 untouched. JITCode-RESIDENT members (catchOSREntryBuffer, FTL
 m_entryBuffer) become registry indices per entering lite (U-T4
 amplifier: concurrent catch/loop OSR entry, one CodeBlock).
 GIL-on/flag-off keeps baked addresses; per-lite buffers
 GC-scanned via the registry walk (jit R2).

### rev-11 ID deltas
NEW: U0c (m_gilOff ctor assignment + eager sticky designation +
immutability; §0); SD13 (spawned breakpoints no-op, GIL-off only;
§A.2.7); §A.1.3 flag-off delta item (b2) (the §N.5 intrinsic's
uniform builtin bytecode). U-T2 gains §A.2.7; U-T3 gains U0c;
U-T8c gains the @atomicInternalFieldClaim site table. SUPERSEDED
(both sides recorded above): heap §5.1 sticky trigger (eager ctor
variant added); heap §10D reversion scope (no-op under
gilOffProcess); the rev-10 F7 note's retroactive mode definition
(replaced by U0c).

================================================================
## rev 12 (2026-06-06) - review round: 8 findings (3 blocker, 5
major; the two U0c findings are duplicates, as are the two E.7.5
findings => 5 distinct items). All verified REAL against the
tree; none refuted.

### F1 (BLOCKER, U0c designation - merges the two duplicate
findings): rev-11 routed the second-VM designation through
noteSharedServerSticky(), whose inner CAS is immediately followed
by the I13 RELEASE_ASSERT (Heap.cpp:4123-4124; the only other
caller is HeapClientSet.cpp:69). A loser heap calling it gets
previous = the first heap != this and ABORTS - so "Later VMs lose
the CAS => m_gilOff=0" was unreachable, and the rev-11 F1 item-3
text ("the designate VM's ctor calls noteSharedServerSticky()")
was circular: designation was DEFINED as winning a CAS that only
exists inside the loser-fatal function. A verbatim build crashes
the second VM ctor under gilOffProcess, contradicting the U0b
corpus rows.
FIX (spec §0 U0c, r12): NEW primitive
Heap::tryDesignateStickySharedServer() = the s_stickySharedServer
compareExchangeStrong(nullptr, this), returning won/lost, NO
assert. Every VM ctor under gilOffProcess calls it. Winner:
m_gilOff=1, then runs noteSharedServerSticky() at clientSet()==1
- its inner CAS now sees previous==this, so the I13
RELEASE_ASSERT at Heap.cpp:4124 is retained TEXTUALLY UNCHANGED
and never fires on this path. Loser: m_gilOff=0, never calls
noteSharedServerSticky() from the ctor; U0b spawn-refusal keeps
its clientSet() <= 1, so the HeapClientSet::add:69 trigger never
fires for it either - if a loser heap somehow reaches
noteSharedServerSticky(), that IS a bug and I13 firing is the
correct behavior (the assert's meaning is preserved, not
weakened). The add-site additionally RELEASE_ASSERTs
(gilOffProcess => server VM's m_gilOff == 1). Corpus addition:
two VMs CONSTRUCTED under gilOffProcess (construction order
exercised; loser ctor completes; loser spawn RangeErrors; loser
embedder entry executes JS beside the shared VM). This is the
"eager ctor variant" the rev-11 ID-delta line alluded to, now
actually defined.

### F2 (BLOCKER, §N.6 detach/grow torn pair): rev-11's
"publish length=0 BEFORE base clear" and "grow publishes
{newBase,newLength} length-first" protected only readers whose
length load happens after publication. Every tier's TA fast path
loads LENGTH, bounds-checks, then loads BASE - and the reader's
two loads carry no ordering (no acquire, no dependency from
length to the base load). Fatal interleavings under rev-11:
DETACH: reader loads old length L>0; detacher stores length=0
then clears base; reader loads base=nullptr, dereferences
nullptr+index. GROW (relocating): reader loads newLength but a
stale oldBase (loads may reorder or straddle the two stores);
accesses oldBase[i], i in (oldLen, newLen) - beyond the
quarantined old mapping. Store-side ordering CANNOT close a
two-word torn read; the rev-11 quarantine only covered readers
that captured the OLD base.
FIX (spec §N.6, r12 - REPLACES the length-first rule):
- Detach: length=0 (seq_cst) + a separate detached FLAG carries
  isDetached(); the base word is NEVER cleared while a racing
  reader could hold a stale length - it keeps pointing at the
  quarantined mapping, which stays mapped and is sized >= every
  length ever published against it. Retirement at a heap §10
  stop: cooperative poll sites mean no in-flight fast path
  (length already loaded, base not yet) straddles a stop, so
  after the stop every new fast path re-loads length (0) and
  bounds-fails before touching base; the base word is then
  cleared/poisoned under quiescence and the mapping released
  (OM §6 epoch shape). Both torn pairs are now benign:
  {oldLen, oldBase} = in-bounds read of the quarantined mapping
  (stale-but-safe; writes lost); {0, anything} = bounds failure.
  DFG/FTL hoisted-vector code still jettisons via the landed
  neutering watchpoints - the quarantine additionally covers any
  code that raced the jettison.
- Grow: the base is IMMUTABLE GIL-off. In-place-only growth via
  reserved VA (wasm Signaling memories; shared memories' ceiling
  reservation; resizable ArrayBuffers' maxByteLength
  reservation): commit the new pages, copy nothing (in place),
  release-publish the new length. Both torn pairs index the one
  immutable mapping: {oldLen, base} trivially; {newLen, base}
  because the mapping is committed to newLen before the length
  store. Where no reservation exists (BoundsChecking memories
  without VA reservation), a gilOff grow that must relocate runs
  under a heap §10 stop (mutators quiesced; same no-straddle
  argument), and the old mapping is still quarantined to the
  NEXT stop to cover captured/hoisted bases in jettisoning code.
- Wasm-backed detach takes the detach arm; §I refuses wasm
  EXECUTION only, so spawned JS TA reads over wasm memory remain
  reachable and are covered by the same rules.
U28's amplifier (spawned TA reader vs main memory.grow + detach
storm) is the direct witness for both arms.

### F3 (MAJOR x2 merged, §E.7.5 deadline machinery): the rev-10/
rev-11 "deadline on the registrant TS" had no storage, no expiry
action in the §E.2 normative loop (a thread waking at the
deadline with an empty queue and keepalive>0 would re-wait
forever - api I22 hang), and no lock-order ruling (listLock and
TS::inboxLock are in the mutually-unnested api rank-3 group).
FIX: (1) STORAGE - §E.1 adds TS field waitDeadlines, a
deadline-ordered list of {deadline, PWT waiter}, guarded by the
SAME inboxLock as the task queue, appended at §C.3 waitAsync
registration when the registrant TS is spawned and the timeout
finite. (2) EXPIRY - §E.2 gains an explicit EXPIRE step after
the post-wake poll/reacquire and before running a task: loop
{under inboxLock: earliest deadline <= now ? take it : break};
for each taken entry run the landed 5.6 timeout inline: dequeue
the waiter under listLock, DROP listLock, then settle
"timed-out" via §E.4 (which takes the registrant's inboxLock and
performs the rule-1 keepalive decrement). (3) LOCK ORDER - the
two rank-3 locks are never held together: deadline harvest under
inboxLock only; dequeue under listLock only; settle re-enters
inboxLock after listLock is dropped. The wait clause already
sleeps min(quantum, earliest deadline), so the loop wakes by the
deadline; keepalive>0 (held by the §C.3 registration) keeps the
loop alive until the settle decrements it. join() therefore
completes: settle either delivered the result task (run next
iteration) or the decrement lets the close path fire.

### F4 (MAJOR, §N.5 claim-failure dispatch): the rev-11 blanket
"loser => already-running TypeError" mandated an outcome no
serial interleaving can produce when the racing resume has
already COMPLETED the generator: serially, the loser's next()
would run after the winner's and take the completed-generator
path (GeneratorPrototype.js:35's state !== @GeneratorStateCompleted
guard - {value: undefined, done: true} for next(), the landed
return/throw semantics for the abrupt variants), not throw.
FIX: claim failure re-reads the state field and dispatches:
Executing => the existing TypeError (serial-equivalent: mid-
flight resume); Completed => the landed completed path;
another SuspendedX (winner resumed and yielded back) => retry
the claim with the new expected value - each retry corresponds
to the legal serialization "loser ran entirely after that
winner", and the loop exits via the other two arms whenever the
generator stops being resumable. U-T8c's site table records the
per-site failure dispatch (async-function/async-generator/
iterator-helper claim sites have their own landed
already-running/completed behaviors; the rule is uniform: failed
claim => re-read => the landed serial path for the observed
state).

### F5 (MAJOR, §C.4 supersession record): api I21 (api:315)
freezes "deleted by re-freeze" for the 4.5-1a TA sync-wait gate;
§C.4 keeps the gate GIL-on. Both documents are binding, so the
divergence needed the formal SUPERSESSION mechanism, not a bare
citation. FIX: §C.4 now carries "SUPERSESSION (api I21 :315 +
api:79 vs this, both sides)" with the rationale (deletion
NARROWED to GIL-off by the master oracle rule: GIL-on observable
behavior changes only via SD6/SD7) and an IU row directing
INTEGRATE-api to annotate I21. No mechanical change - rev-11's
behavior was already correct; this closes the authority
conflict. ThreadAtomics.cpp:536-541 remains NOT 4.5-1a (G11
embedder gate, re-pointed at mayBlockSynchronously()).

### Rev-11 IM add-list (RETROACTIVE - omitted at r11; the spec
§IM's "rev-7..12" pointer resolves here)
- debugger/Debugger.{h,cpp} + inspector pause path -> §A.2.7
  (SD13; entry-hook early-returns on spawned lites; trap-bit
  fan-out exemption; attach/detach + CodeBlock-wide recompile
  walks under a §A.3 stop), IV/IJ.
- builtins/GeneratorPrototype.js + AsyncFunctionPrototype.js +
  AsyncGeneratorPrototype.js + iterator-helper builtins +
  BytecodeIntrinsicRegistry.{h,cpp} + BytecodeList.rb + LLInt/
  Baseline slow-path op + DFG/FTL node AtomicInternalFieldClaim
  (DFGNodeType/clobberize/SpeculativeJIT/FTLLowerDFGToB3) ->
  §N.5 (flag-off delta (b2); golden-gate re-baseline), IJ.
- VM.{h,cpp} ctor (m_gilOff) + heap/Heap.{h,cpp}
  noteSharedServerSticky/§10D arm (4106-4124, 4755) +
  HeapClientSet.cpp:69 -> §0 U0c, IH/IV.
- LazyProperty.h:117 machine + JSGlobalObject initLater/VM
  ensure* -> §K.3, IV.
- JSMap/JSSet/JSWeakMap (JSOrderedHashTable, WeakMapImpl) + DFG
  map-intrinsic disable; JSString rope publication; DateInstance;
  FunctionRareData -> §N.1-4, IJ/IV.
- ArrayBuffer.{h,cpp} + JSArrayBufferView + wasm BufferMemoryHandle
  -> §N.6, IV/IH.

### Rev-12 IM add-list
- heap/Heap.{h,cpp} tryDesignateStickySharedServer (NEW) + VM
  ctor call sites -> §0 U0c r12 designation, IH.
- ArrayBuffer.{h,cpp} detached flag + base-retention;
  BufferMemoryHandle/wasm Memory grow paths (in-place / §10-stop
  relocate) + neutering-watchpoint interaction -> §N.6 r12, IV/IH.
- ThreadState (api WS) waitDeadlines field + threadMain E.2
  EXPIRE step + WaiterListManager timeout path -> §E.1/E.2/E.7.5,
  IA.
- Generator/async/iterator-helper builtin claim-failure dispatch
  -> §N.5 r12, IJ.
- INTEGRATE-api I21 annotation row -> §C.4 SUPERSESSION, IA.

### rev-12 ID deltas
No new INV/SD IDs. U0c text replaced (designation primitive);
U0b corpus row extended (two-VM construction); §N.6 protocol
replaced (detach flag + immutable-base grow); §E.2 loop gains the
EXPIRE step (U11's "loop closes" claim now includes deadline
expiry); §C.3/§E.7.5 re-pointed at waitDeadlines; §N.5 failure
dispatch added (U-T8c table gains the per-site column); §C.4
formalized as a SUPERSESSION with IU row. U-T11 corpus arm
"spawned waitAsync finite timeout, hooks on AND off" now
exercises §E.1 waitDeadlines + the E.2 EXPIRE step; U-T13's §N.6
arm re-derived; U-T14 re-audits the (a)/(b)/(b2) flag-off list
unchanged.

### rev-12 spec compressions (byte budget; normative text
preserved at the cited locations, no content dropped)
§A.1.6 -> annex A16; §A.3.6 -> annex A36; §C.1 arms -> annex C1;
§C.3 -> r9 F1 + annex C3; §J.3 -> r10 F5; §D.1 mechanics -> r9
F2; §A.3.8/§F.5/§LK.8/§F.2-WeakSet/§A.1.3 wording tightened
(pointer targets unchanged). Heap.cpp:4115 line refs corrected to
4123-4124 (CAS) / 4124 (assert).

# REV 13 (2026-06-06) - review round: 4 findings (2 blocker, 2
major); all four verified REAL against the tree and fixed. Spec
re-frozen rev 13. Spec stayed <=50000 bytes by annexing full
texts here (annexes N6, U0C, F2 below are BINDING, same standing
as A16/A36/C1/C3/E7).

## Rev-13 findings and dispositions

### F1 (BLOCKER, accepted): §N.6 ruled detach+grow but not SHRINK
Verified: ArrayBuffer::resize, ArrayBuffer.cpp - when desiredSize
< memoryHandle->size() the shrink branch (lines 628-639) calls
BufferMemoryManager::freePhysicalBytes(bytesToSubtract) then
OSAllocator::protect(startAddress, bytesToSubtract,
readable=false, writable=false) and updates m_sizeInBytes, all on
the resizing thread with no quiescence. The delta<0 rejection at
:574-577 applies ONLY when isWasmMemory() (under
useWasmMemoryToBufferAPIs); plain resizable non-shared ABs shrink
freely from JS (ArrayBuffer.prototype.resize). GIL-off a foreign
TA fast path that loaded the OLD larger length passes bounds and
dereferences a freshly PROT_NONE'd page => SIGSEGV - exactly the
torn-pair class r12 F2 closed for detach/grow. Fix: SHRINK arm
added to §N.6/annex N6 - publish the smaller length seq_cst,
DEFER the tail protect/decommit to heap §10 stop retirement. VA
is already reserved to maxByteLength
(tryAllocateResizableMemory, ArrayBuffer.cpp:108-141 reserves
maximumBytes via tryAllocateGrowableBoundsCheckingMemory), so
deferral costs only physical-page residency until the next stop.
U28 amplifier extended with a resize-shrink storm; IM row
ArrayBuffer.cpp resize.

### F2 (MAJOR, accepted): Watchdog unruled beyond trap delivery
Verified: Watchdog.cpp - m_hasEnteredVM is a plain bool toggled
by enteredVM()/exitedVM() (:115-127, ASSERT(m_hasEnteredVM) at
:124/:131); startTimer computes m_cpuDeadline =
CPUTime::forCurrentThread() + limit (:136) - the CPU budget is
measured on whichever thread arms the timer; setTimeLimit/
shouldTerminate/startCountdown are guarded only by
ASSERT(m_vm->currentThreadIsHoldingAPILock()) (:44/:57/:132),
which §F.2 redefines to the NON-exclusive token meaning. With N
entered threads: enter/exit toggling races, cross-thread CPU
accounting is incoherent, concurrent shouldTerminate. None of
§K.4's three classes fits (a leaf lock fixes data races but not
WHOSE cpu/entry counts). Ruling added as §A.2.8 (SD13 shape,
parallel to the debugger ruling): GIL-off the watchdog
arms/measures/checks on main/embedder carriers ONLY -
enteredVM/exitedVM and the watchdog-check service bit skip
spawned lites (VMLite::isSpawned); all watchdog state then
mutates only under the carriers' real m_lock (§F.1 keeps mutual
exclusion between embedder threads GIL-off), restoring the
single-threaded protocol; a fired limit's TERMINATE decision
raises the VM-wide termination trap, which fans per §A.2.3 rule
3, so spawned threads ARE terminated when the watchdog fires.
SD14 (full text below): spawned-thread CPU time and
spawned-thread entry/exit advance neither the CPU budget nor
m_hasEnteredVM; wall-clock limits fire regardless of which
threads run (timer-driven) - spawned-only workloads ARE still
wall-clock-limited and terminated. Per-thread CPU accounting
deferred post-ungil. GIL-on unchanged. §K.4's audit routes
Watchdog state to §A.2.8 instead of classing it. IM row
Watchdog.cpp (U-T2). Corpus: wall-clock limit fires while only a
spawned thread loops -> spawned terminated, no abort; CPU-limit
variant documents SD14.

### F3 (BLOCKER, accepted): transfer is detach-by-MOVE; the r12
quarantine protocol is unsound for it
Verified: ArrayBufferContents::detach() (ArrayBuffer.h:199) is a
move; ArrayBuffer::transferTo (ArrayBuffer.cpp:498) does `result
= m_contents.detach()` (:519) - the SAME mapping migrates to a
LIVE transferee - while ArrayBuffer::detach(VM&) (:525-528)
move-destructs the contents (`auto unused = ...`, frees at end
of scope today). r12's single retirement rule ("quarantined
mapping ... until a heap §10 stop retires it ... mapping
released") is wrong for transfer both ways: (1) literal reading
=> the stop releases a mapping the transferee owns - every
post-GC access to a transferred AB is UAF, no race needed; (2)
defer-the-dtor reading => a moved-out ArrayBufferContents has
m_data nulled by the move ctor and carries no destructor
(ArrayBuffer.h:152-156 frees only via m_destructor, which also
moved), so the quarantine pins NOTHING; transferee GC'd/detached
before the stop frees the data while a racing reader's
{oldLen, oldBase} "stale-but-safe" pair dereferences freed
memory. Fix (annex N6 TRANSFER arm): GIL-off the detachable
non-shared transferTo arm becomes COPY + detach - the transferee
receives a FRESH allocation (copyTo), and the source contents
then follow the unambiguous detach-and-free quarantine
(free-at-stop). shareWith (shared) and non-detachable (already
copyTo) arms unchanged. Quarantine entries are created by
exactly the two ::detach() consumers: ArrayBuffer::detach(VM&)
:527 and the source side of the rewritten transferTo. Perf
delta: transfer of a detachable non-shared AB goes O(1)->O(n)
GIL-off only (structuredClone-with-transfer class); recorded,
accepted v1. ALTERNATIVE REJECTED v1: refcounting the underlying
data holder (Ref<DataHolder> shared by contents + quarantine) -
sound but restructures ArrayBufferContents/DataType +
m_destructor ownership for every embedder constructor
(fromSpan/external-destructor ctors, ArrayBuffer.h:131-156,
Bun's external buffers), a much larger ABI surface; revisit
post-ungil if the copy shows up in benches. U28 amplifier gains
transfer storms + a transferee-GC'd-before-stop arm.

### F4 (MAJOR, accepted): heap §10A JSLock-forwarding clause
contradicted GIL-off without a labeled SUPERSESSION
Verified: SPEC-heap.md:281 freezes "Wiring: JSLock::
didAcquireLock/willReleaseLock call the server pair today; once
ISS they forward to the *main client's* AHA/RHA". Under GIL-off
ISS is true from the ctor (U0c) and §F.1 runs AHA on the CURRENT
carrier's OWN client; following the heap text verbatim would
have an embedder thread acquire access on the main client while
the main thread uses it - two threads, one client, unsound per
heap's one-client-per-thread model (heap Dev 8). §B.3 had the
substance but not the label; annex A36's supersession list
covers heap I4 + §10A.1 only. Fix: §B.3 rewritten as an explicit
SUPERSESSION citing SPEC-heap.md:281, both sides, IH row.
GIL-on/flag-off forwarding (and the §10A.1 TLS re-stamp inside
didAcquireLock) unchanged - the supersession is the GIL-off arm
only.

## Annex N6 (§N.6 FULL NORMATIVE text - BINDING; REPLACES the
r12 §N.6 text; r13 adds TRANSFER + SHRINK arms)

PRINCIPLE (r12 F2, unchanged): every tier's TA fast path loads
LENGTH, bounds-checks, then loads BASE; the reader's two loads
carry no ordering, so store ordering alone cannot close a torn
two-word read. Invariant: a racing reader must NEVER pair a
passing length with an unmapped-or-short base. Equivalently:
any base value a reader can observe must point at a mapping
that is mapped and sized >= every length value still observable
against it; retirement of a mapping requires that no
pre-retirement length remain live, which heap §10 stop
quiescence provides (no JS/JIT fast path straddles a stop).

Arms (GIL-off; GIL-on/flag-off unchanged):

1. DETACH-AND-FREE (ArrayBuffer::detach(VM&), ArrayBuffer.cpp:
   525-528; today the moved-out temporary frees immediately).
   GIL-off: publish length=0 (seq_cst store) + the detached FLAG
   (isDetached() becomes the flag, NOT !m_data); the base word
   is NOT cleared. The move-destructed ArrayBufferContents is
   moved INTO a per-server quarantine list entry - the entry
   OWNS the contents (m_data + m_destructor + m_memoryHandle),
   hence the eventual free. A heap §10 stop retires quarantine
   entries enqueued before the stop (OM §6 epoch shape): under
   quiescence the base word is cleared/poisoned, then the entry
   is destroyed (destructor runs => mapping/allocation
   released). notifyDetaching/neutering watchpoints fire as
   landed - compiled code with hoisted vectors jettisons.
2. TRANSFER (ArrayBuffer::transferTo, ArrayBuffer.cpp:498;
   detach-by-move at :519). GIL-off, the detachable non-shared
   arm is REWRITTEN: m_contents.copyTo(result) (transferee gets
   a FRESH allocation; OOM => transfer fails as the landed
   non-detachable arm does), THEN the source runs arm 1 verbatim
   (its contents - the original mapping - enter the quarantine
   owning the free). isShared()/shareWith and !isDetachable()
   arms unchanged (already share/copy). Rationale + rejected
   refcount alternative: rev-13 F3 above.
3. SHRINK (ArrayBuffer::resize downward, ArrayBuffer.cpp:
   628-639). GIL-off: under memoryHandle->lock() compute
   desiredSize as landed; publish m_sizeInBytes = newByteLength
   (seq_cst) but DO NOT call freePhysicalBytes/OSAllocator::
   protect on the resizing thread. The tail range [desiredSize,
   previous handle size) is appended to the SAME quarantine list
   as a page-range entry {memoryHandle ref, offset, size};
   retirement at the next heap §10 stop performs the protect +
   freePhysicalBytes (and memoryHandle->updateSize) under
   quiescence. Re-grow before the stop consumes/cancels
   overlapping pending tail entries under memoryHandle->lock()
   (pages still committed => zeroFill as landed). The wasm
   isWasmMemory() delta<0 rejection (:574-577) stands. Torn
   pairs: {oldLen, base} = read of still-committed still-mapped
   pages (stale-but-safe); {newLen, base} = in-bounds.
4. GROW (memory.grow + resizable AB upward; r12 text
   unchanged): base IMMUTABLE GIL-off - in-place ONLY (VA
   reserved: Signaling memories, shared ceiling, resizable-AB
   maxByteLength per tryAllocateResizableMemory :108-141);
   commit pages, THEN release-publish the larger length - both
   torn pairs in-bounds of the one mapping. No reservation
   (BoundsChecking without VA) => gilOff grow RELOCATES under a
   heap §10 stop; the old mapping is quarantined to the NEXT
   stop for captured/hoisted bases.

Torn-pair table (reader = any tier TA/DataView fast path):
- detach: {oldLen, oldBase} stale-but-safe (quarantine mapped);
  {0, *} bounds-fails. transfer: identical (source = detach).
- shrink: {oldLen, base} stale-but-safe (tail still committed);
  {newLen, base} in-bounds.
- grow in-place: {oldLen, base} and {newLen, base} both
  in-bounds. grow relocate: stop-separated, no concurrent
  reader.
Wasm-backed detach = arm 1 (§I refuses spawned wasm EXECUTION
only; buffers stay cross-thread-reachable). Quarantine sizing:
entries are byte-accounted against heap extra memory so a
detach/shrink storm pulls the next collection forward.

U28 amplifier (extended): spawned TA readers vs main running a
{memory.grow, detach, transferTo, structuredClone-with-transfer,
resize-shrink, re-grow-after-shrink} storm; plus the
transferee-GC'd-before-stop arm (transfer, drop transferee,
force eager GC of it, reader still racing the source). IM rows:
ArrayBuffer.{h,cpp} (resize/transferTo/detach), JSArrayBufferView
+ per-tier TA fast paths (length-load sites), U-T13 owner.

## Annex U0C (§0 U0c FULL text - BINDING; verbatim the r12 §0
U0c block, preserved on spec compression)

- U0c m_gilOff assignment (r11/r12; defines F7's input).
  vm.m_gilOff is computed ONCE in the VM ctor - BEFORE
  m_mainVMLite registration (vmstate §6.4.4), any entry, any
  codegen - and is IMMUTABLE. Designation primitive (r12;
  noteSharedServerSticky() is loser-FATAL - inner CAS
  RELEASE_ASSERTs, Heap.cpp:4123-4124 - so it cannot BE the
  designation): NEW Heap::tryDesignateStickySharedServer() = the
  s_stickySharedServer CAS, returning won/lost, NO assert. Under
  gilOffProcess every VM ctor calls it. WINNER: m_gilOff=1, then
  noteSharedServerSticky() at clientSet()==1 (quiescence trivial
  at birth; inner CAS sees previous==this - I13 stands
  UNCHANGED). LOSER: m_gilOff=0, never calls
  noteSharedServerSticky() from the ctor; U0b spawn-refusal
  keeps its clientSet()<=1, so the HeapClientSet::add:69 trigger
  site - STAYS, idempotent (SUPERSESSION vs heap §5.1's
  size-EVER>1 trigger, Heap.cpp:4106-4124, both sides) - is
  unreachable for it; a loser reaching it IS a bug, I13 fires
  correctly. That site gains RELEASE_ASSERT(gilOffProcess =>
  server VM's m_gilOff==1). Discharges §F.2's ISS-flip
  clause-(a) STRUCTURALLY (flip pre-dates first entry/codegen/
  lite registration). §10D never clears m_gilOff; Heap.cpp:
  4755's arm no-ops under gilOffProcess. Lites copy the final
  byte; no jettison. Corpus: compile-heavy single-thread run
  THEN first spawn (Group-3 consistency); TWO VMs CONSTRUCTED
  under gilOffProcess - loser ctor completes, spawn RangeErrors,
  embedder entry runs JS. U0c.

## Annex F2 (§F.2 fixed-consumer rulings FULL text - BINDING;
verbatim the r12 §F.2 list, preserved on spec compression)

- sanitizeStackForVM - uses the CURRENT lite's lastStackTop.
- primitiveGigacageDisabled - MUTEX predicate + §A.1.5 deferred
  arm (the gigacage-disable service is VM-wide).
- JSCell::validateIsNotSweeping - token + per-CLIENT mutator
  state.
- ISS-flip clause-(a) - DISCHARGED by U0c's eager ctor-time
  flip.
- DeferredWorkTimer asserts - §E.7.2 token meaning (incl. the
  NEGATIVE assert at runRunLoop).
- WeakSet::allocate (WeakSetInlines.h:44) - token+access
  predicate, NOT exclusivity (REFUTED r11: the free-list pop is
  MSPL-locked under ISS, WeakSetInlines.h:69; deallocate sound,
  WeakSet.h:121-131; full argument in the r11 section).
- r13 adds: Watchdog's three asserts (:44/:57/:132) = token
  meaning for the DATA-RACE question, but the SEMANTIC ruling is
  §A.2.8 carrier-only (the audit row points there, not §K).

## Rev-13 SD addition (extends the r9/r11 SD annex; IDs frozen)

SD14 (GIL-off only; §A.2.8). Watchdog execution-time limits
measure main/embedder carriers only: spawned-thread CPU time
does not advance the CPU budget; spawned entry/exit does not
toggle m_hasEnteredVM. Wall-clock deadlines are timer-driven and
fire regardless of which threads are running; the resulting
termination fans VM-wide, so spawned threads are terminated.
GIL-on (and flag-off) behavior unchanged. Corpus carries //@
runThreadsGILOn variants with the old expectation.

## Rev-13 §T deltas (extends the rev-9 annex 3 + r10-r12 deltas)

- U-T2 gains §A.2.8 (Watchdog carrier-only wiring + SD14 corpus
  arms; IM Watchdog.cpp).
- U-T13 §N.6 scope is now the four annex-N6 arms (detach
  quarantine, transfer copy+detach, shrink deferred-decommit,
  grow) + the quarantine list/extra-memory accounting + the
  extended U28 amplifier.
- IM add-list (rev-13): Watchdog.cpp -> §A.2.8 (U-T2);
  ArrayBuffer.cpp resize/transferTo/detach + ArrayBuffer.h
  contents/detach -> annex N6 (U-T13); JSLock.cpp
  didAcquireLock/willReleaseLock -> §B.3 supersession (U-T6,
  IH row vs heap §10A:281).

## Rev-13 spec compressions (normative text preserved here)

To stay <=50000 bytes the spec's §0 U0c block, §N.6, and §F.2
fixed-consumer list were compressed to summaries pointing at
annexes U0C/N6/F2 above (BINDING), matching the A16/A36/C1/C3/E7
pattern; §A.3.6/§J.3/§C.1 summaries were tightened (their full
texts were ALREADY the BINDING annexes A36 / r10-F5 / C1 - no
normative content was dropped, only duplicated summary prose).
No other section changed meaning in rev 13.

# REV 14 (2026-06-06) - review round: 2 majors, both verified
# REAL against the tree and UPHELD; no refutations

## Rev-14 findings and dispositions

### F1 (major, UPHELD): §A.2.8/SD14 watchdog enforcement chain
broken whenever no carrier is executing JS.

Verification: the dispatched timer callback only calls
m_vm->notifyNeedWatchdogCheck() (Watchdog.cpp:149-155); the
deadline comparison and the terminate decision live in
Watchdog::shouldTerminate (Watchdog.cpp:55-108), reached
exclusively via NeedWatchdogCheck handling at JS-execution poll
sites (VMTraps.h:105-118); Watchdog::exitedVM() calls
stopTimer() (Watchdog.cpp:122-126). The r13 §A.2.8 text made
DELIVERY carrier-only and asserted "a fired limit's terminate
raises VM-wide termination => spawned threads ARE terminated",
but both common GIL-off shapes break the chain BEFORE
shouldTerminate can run:
(1) carrier PARKED (main in join()/property Atomics.wait): the
§J.3 discipline (r10 F5, BINDING) restricts per-quantum wakes
to lock-free polls of the captured lite's bits and forbids
reacquisition before final exit, so a parked carrier OBSERVES
the watchdog-check bit but could not SERVICE it; §A.2 rule 4
re-points only the TERMINATION predicate at park sites.
(2) carrier EXITED (embedder lock/eval/unlock, then leaves
while spawned threads run): exitedVM/stopTimer disarm the
watchdog even though per SD14 spawned entry advances neither
the budget nor m_hasEnteredVM - the limit never fires at all.
The r13 corpus arm "wall-clock limit mid-spawned-loop" would
only pass with main artificially spinning in JS. The rev-13
SD14 sentence "Wall-clock deadlines are timer-driven and fire
regardless of which threads are running" was asserted, not
designed - nothing in the r13 chain implemented it.

Fix: §A.2.8 REWRITTEN (annex W below, BINDING). The reviewer's
options (a) and (b) are COMBINED: (b) gives parked carriers a
service path that keeps the embedder callback on a
token-holding thread with a globalObject - preserving callback
extension semantics in the typical Bun shape (main parked in
join while a spawned thread loops); (a)'s timer-thread
evaluation is kept ONLY for the no-carrier residue, where no
callback host exists, and is narrowed to wall-clock +
terminate-by-default (declared in SD14 amended). Pure (a) as
the general path was REJECTED: shouldTerminate's CPU-budget
re-arm reads CPUTime::forCurrentThread() (:76-80) and the
callback receives a JSGlobalObject under the §F.2 token
(asserts :44/:57/:131) - neither is meaningful on the timer
thread.

### F2 (major, UPHELD): annex N6 TRANSFER arm silently loses
resizability under GIL-off.

Verification: ArrayBufferContents::copyTo (ArrayBuffer.cpp:
233-244) allocates via plain tryAllocate and copies ONLY
m_data/m_sizeInBytes - it carries neither m_maxByteLength nor
m_hasMaxByteLength nor m_memoryHandle (contrast shareWith,
:245-259, which copies all three). Resizable non-shared buffers
ARE detachable, so ArrayBuffer.prototype.transfer() with
PreserveResizability routes through transferTo's detachable arm
(:498-521) and then calls newBuffer->resize(vm, newByteLength)
on the transferee (JSArrayBufferPrototype.cpp:330-346). Under
the landed MOVE (result = m_contents.detach(), :519) the
memoryHandle/maxByteLength travel with the contents and the
resize succeeds; under the r13 copyTo rewrite the transferee
has m_hasMaxByteLength=false and no memoryHandle, so the
follow-up resize fails (RangeError) and resizability is lost -
an undeclared break of the TC39 arraybuffer-transfer
requirement that transfer() preserve max byte length. The r13
rationale modeled the copy on the landed NON-detachable arm
(:512-516), which never sees resizable sources through
transfer().

Fix: annex N6 arm 2 AMENDED (below). The reviewer's alternative
(handle MOVE with the quarantine entry holding a memoryHandle
ref + a pre-transfer length-floor obligation) REJECTED for v1:
it reintroduces exactly the live-transferee-mutating-over-a-
quarantine-visible-mapping aliasing that the r13 copy design
exists to avoid (a post-transfer shrink/grow of the transferee
would race quarantined readers of the SAME mapping), for no
semantic gain - the copy shape preserves resizability outright.

## Annex W (§A.2.8 FULL NORMATIVE text - BINDING; REPLACES the
## r13 §A.2.8 design)

Landed mechanics (GIL-on unchanged; the GIL-off baseline):
m_hasEnteredVM toggles in enteredVM/exitedVM (Watchdog.cpp:
115-126); startTimer records m_cpuDeadline from the ARMING
thread's CPU clock plus a wall-clock m_deadline, then
dispatches a timer whose callback calls
m_vm->notifyNeedWatchdogCheck() (:129-155); shouldTerminate -
reached ONLY at JS poll sites via NeedWatchdogCheck
(VMTraps.h:105-118) - rejects stale timers, re-arms if CPU
budget remains, else consults the embedder callback and returns
the terminate decision (:55-108); API-lock asserts :44/:57/:131
read as the §F.2 token GIL-off; carrier watchdog state is
serialized by the real Watchdog::m_lock (§F.1).

GIL-off NORMATIVE rules (useJSThreads, GIL-off only):

W0 (accounting; = r13 substance): arms/measures on
main/embedder carriers only. Spawned entry/exit toggles neither
carrier-entered state nor the timer; spawned CPU never advances
the CPU budget; the watchdog-check trap bit is set ONLY in
carrier lites (§A.2 rule 3 exemption, like the debugger bit).
Per-thread budgets are post-ungil.

W1 (parked-carrier service - §J.3 carve-out): a main/embedder
carrier parked under §J.3 already polls the CAPTURED lite's
trap bits each D9 quantum. On observing the watchdog-check bit
it performs the FULL §J.3 exit reacquisition (m_lock + token +
access: §A.3.6 swap + §F.1 OR + §A.3.2b poll - the same
sequence, run EARLY), services Watchdog::shouldTerminate under
its token on its own thread (callback semantics and CPU re-arm
identical to an entered carrier), then: terminate => raise
VM-wide termination (rule 3) and proceed to final park exit
(the wait fails per SD8/§E.5); no terminate => re-release per
§J.3 and re-park with a FRESH wait node (SD6 permits
multi-flight; waiter-list re-insertion as in the §A.3.2b
post-wake poll shape). §J.3's "exactly once" is hereby
renormalized to once per ACQUISITION EPISODE: W1 service ends
one episode; re-parking opens a new one. Lock-rank clean:
reacquisition happens only after the quantum wait returns - no
rank-3 waiter-list lock is held across it; api 5.9(e) ordering
holds per episode.

W2 (exit deferral): exitedVM() on the LAST carrier (carrier
entry depth, under Watchdog::m_lock) while spawned lites remain
registered (§A.1.3 registry count) clears m_cpuDeadline (the
CPU budget is carrier-scoped, W0) but PRESERVES m_deadline and
the pending dispatched timer: the watchdog stays armed for
wall-clock purposes. m_hasEnteredVM splits GIL-off into
m_carrierEntered (depth) + m_wallClockArmed; the :44/:103/:130
asserts re-point accordingly. A carrier re-entering re-arms the
CPU budget as landed. When the last spawned lite unregisters
with no carrier entered, the watchdog disarms fully (= the
landed exitedVM effect, deferred).

W3 (no-carrier enforcement): the dispatched timer callback
gains a GIL-off branch under Watchdog::m_lock: if any carrier
lite is entered-or-parked => notifyNeedWatchdogCheck() as
landed (entered carriers service at poll sites; parked carriers
via W1). Else (spawned-only execution): evaluate the WALL-CLOCK
deadline on the timer thread itself (same stale-timer rejection
as shouldTerminate :68-75); if expired, raise VM-wide
termination directly via the §A.2 rule-3 fan-out (registry lock
only - the async-delivery path §A.2.5 already runs tokenless)
and disarm. The embedder callback is NOT consulted in W3 (it
requires a JSGlobalObject, the token, and carrier thread
identity): SD14 amended - terminate-by-default, matching the
!m_callback default (:87-89). The CPU budget is not evaluated
in W3 (carrier-scoped per W0): spawned-only execution is
governed by wall-clock only.

Interactions: §K.4 still routes Watchdog here. The VMTraps
NeedWatchdogCheck -> NeedTermination fall-through is unchanged
for entered carriers. Termination raised by W1/W3 reaches
spawned threads via rule 3 + D9 park quanta (§A.2.6), and
parked main via the rule-4 park-lite predicate.

Corpus (U-T2): wall-clock limit while a spawned thread loops,
three shapes - (a) main parked in join (W1: callback consulted;
extension honored - callback grants more time once, then
terminates), (b) main did lock/eval/unlock and left (W3:
terminate fires WITHOUT the callback - SD14 arm), (c) main
spinning in JS (landed shape, regression guard). All three:
spawned terminated, no abort. GIL-on //@ runThreadsGILOn
variants keep old expectations. IM: Watchdog.{h,cpp} (carrier
depth + m_wallClockArmed split, timer-callback branch);
VMTraps.cpp (no new surface - rule-3 fan-out reused).

## Annex N6 arm 2 (TRANSFER) - r14 AMENDED text (REPLACES the
## r13 arm-2 paragraph; principle, arms 1/3/4, and the table
## stand except the additions named here)

2. TRANSFER (ArrayBuffer::transferTo, ArrayBuffer.cpp:498;
   detach-by-move at :519). GIL-off, the detachable non-shared
   arm is REWRITTEN as COPY + DETACH: the transferee gets a
   FRESH allocation, then the source runs arm 1 verbatim (its
   contents - the original mapping - enter the quarantine
   owning the free). Allocation of the copy (r14 F2):
   - source WITHOUT m_hasMaxByteLength: plain
     m_contents.copyTo(result) (ArrayBuffer.cpp:233-244) - data
     + size suffice.
   - source WITH m_hasMaxByteLength: copyTo is INSUFFICIENT (it
     copies only m_data/m_sizeInBytes; the resizable path of
     ArrayBuffer.prototype.transfer routes through THIS arm and
     then resize()s the transferee,
     JSArrayBufferPrototype.cpp:330-346). The copy is allocated
     via the tryAllocateResizableMemory shape (:108-141) with
     the SOURCE's maxByteLength reservation; m_maxByteLength,
     m_hasMaxByteLength, and the NEW m_memoryHandle are stamped
     onto the result BEFORE the memcpy of byteLength() bytes.
   OOM in either shape => transfer fails as the landed
   non-detachable arm does. The post-transferTo resize of the
   transferee is thread-local: the JSArrayBuffer wrapper is
   created only afterwards (:341-346), no concurrent reader
   exists, so no new torn pairs arise on the transferee.
   isShared()/shareWith and !isDetachable() arms unchanged
   (already share/copy). REJECTED: refcounted holder (r13);
   handle-MOVE + quarantined-handle-ref (r14 - reintroduces
   live-transferee aliasing over a quarantine-visible mapping).
   Torn-pair rows ADDED: transfer-of-resizable source rows =
   detach rows; transferee rows vacuous (unpublished during
   mutation). U28 amplifier arms ADDED: transfer() of a
   RESIZABLE buffer under reader storm, then post-transfer
   resize/grow of the transferee up to maxByteLength;
   transfer(newByteLength > byteLength) growing arm.

## Rev-14 SD amendment (IDs frozen; REPLACES the rev-13 SD14
## text)

SD14 (GIL-off only; §A.2.8/annex W). Watchdog CPU budgets and
entry accounting measure main/embedder carriers only: spawned
CPU never advances the budget; spawned entry/exit toggles
neither carrier-entered state nor the timer. Wall-clock
deadlines REMAIN armed while spawned lites are registered, even
after the last carrier exits (W2), and are enforced either via
a parked carrier's early service episode (W1 - embedder
callback consulted as landed) or, when no carrier is
entered-or-parked, on the timer thread WITHOUT consulting the
embedder callback (W3 - terminate-by-default; embedders needing
extension semantics keep a carrier entered or parked). Spawned
threads are terminated by the VM-wide fan-out in all shapes.
GIL-on (and flag-off) unchanged. Corpus //@ runThreadsGILOn
variants keep old expectations.

## Rev-14 §T deltas (extends rev-9 annex 3 + r10-r13 deltas)

- U-T2: §A.2.8 scope = annex W (W0-W3 + the carrier-depth/
  m_wallClockArmed split); corpus arms (a)/(b)/(c) above.
- U-T11 (§J.3 owner): "exactly once" renormalized to per
  acquisition episode (W1); the U15 lint extends to flag any
  park-quantum body taking locks OTHER than the W1 full-exit
  sequence.
- U-T13: annex N6 arm-2 amendment (resizable-transfer
  allocation shape) + the two added U28 amplifier arms.
- IM add-list (rev-14): Watchdog.h -> annex W (U-T2);
  JSArrayBufferPrototype.cpp arrayBufferCopyAndDetach -> annex
  N6 arm 2 (U-T13; read-side anchor only).

## Rev-14 spec compressions

§A.2.8 in the spec is now a summary pointing at annex W
(BINDING), matching the A16/A36/C1/C3/E7/N6 pattern; the §J.3
bullet gained the acquisition-episode clause; §N.6 gained the
resizable-transfer clause; the SD list's SD14 line re-points at
the r14 text. No other section changed meaning in rev 14.

# Rev 15 (2026-06-06) - review round 15: both findings VERIFIED
# REAL; F1 (BLOCKER) + F2 (MAJOR) fixed

### F1 (BLOCKER): §N.5 unclaim transitions were PLAIN stores -
### no release edge at resume-claim handoff
VERIFIED REAL. Evidence: GeneratorPrototype.js:36/41/47 are
plain @putGeneratorInternalField state transitions; the r11 F4
text above explicitly froze "RELEASE transitions
(Running->SuspendedX/Completed) stay plain PutInternalField -
single owner while claimed". The single-owner argument covers
mutual exclusion of the frame WHILE claimed, not publication at
handoff: SD6 (§C.6) deletes single-flight in BOTH GIL modes, so
two threads alternating gen.next() race directly on the claim
CAS with NO other synchronization edge between them - §N.5
itself blesses "another SuspendedX => retry (each retry = a
legal serialization)". (Async-FUNCTION resumption does inherit
an edge from the §E.1b promise JSCellLock on the reaction path,
but plain generators and iterator helpers do not; the fix
covers all uniformly.) Race: thread A wins the claim, writes
@generatorFieldFrame/internal fields with plain stores, plain-
stores state=SuspendedYield; thread B's seq_cst claim CAS reads
that plain store - a CAS reading a non-release store creates NO
happens-before with A's earlier frame stores - so on arm64
(store-store reordering; a shipping Bun target) B can enter the
frame observing pre-resume values: torn resumption state, type
confusion, heap corruption. x86-TSO masks it; TSAN-no-JIT
catches it only probabilistically (DFG/FTL lower to raw
stores).

FIX (NORMATIVE, §N.5; this REPLACES the r11 F4 "stay plain
PutInternalField" sentence): the unclaim transitions
Running->SuspendedX and Running->Completed MUST be store-
RELEASE on the state field's EncodedJSValue word in ALL tiers.
Twin intrinsic @atomicInternalFieldPublish(cell, fieldIndex,
value), emitted UNCONDITIONALLY exactly like the claim
intrinsic (single-threaded it is observably identical to the
landed PutInternalField, avoiding mode-conditional bytecode; it
rides the SAME §A.1.3 flag-off delta item (b2) - one uniform
builtin-bytecode change, golden gates re-baselined once for
both). Tiers: LLInt/Baseline = slow-path host operation;
DFG/FTL = new node AtomicInternalFieldPublish lowered to a
release store (storeStoreFence+store or stlr) via the existing
internal-field offset machinery, write barrier as
PutInternalField. Pairing: the publish-release pairs with the
claim CAS's acquire half => the winning claimant inherits
happens-before over the previous owner's frame/field stores.
INTERIOR frame/field stores while claimed stay PLAIN (at-most-
one-resumer exclusion - unchanged). The r12 failure-dispatch
re-read is unaffected (the re-read load gains acquire via the
same CAS/publish pairing on retry). Cost: release stores are
free on x86, one stlr on arm64, once per yield/await - §B.5's
one-cheap-CAS-per-suspension premise stands; the §B.5 r9
async/generator microbench gate covers the publish too.
U-T8c row EXTENDED: the audit enumerates UNCLAIM sites
(GeneratorPrototype.js:41/:47 Completed stores + the
generatorResume suspend path + async fn/async generator/
iterator-helper equivalents) and re-points them at
@atomicInternalFieldPublish, alongside the claim sites.
Amplifier arm (U28 family, runs under TSAN AND on arm64
hardware): two threads ping-pong next() on ONE generator whose
body round-trips a per-resume counter through frame state;
assert every observed value is the predecessor's published
value (stale-frame observation = failure).

### F2 (MAJOR): W1 early-service re-park left the old wait
### node's notified state undefined
VERIFIED REAL. The r14 annex W W1 sentence "re-park with a
FRESH wait node (... waiter-list re-insertion as in the
§A.3.2b post-wake poll shape)" is unsupported: §A.3.2b
(SPEC-ungil.md §A.3.2 rule 2b) defines stop-bit polling and
access re-acquisition ONLY - it contains no waiter-list
mechanics and no disposition for the OLD node. During the W1
service window the carrier's original node is STILL ENQUEUED
in the WaiterListManager/PWT list (D9 quantum parks leave the
node on-list; notify dequeues it - isOnList()/dequeue shape,
WaiterListManager.h:144-163). A concurrent Atomics.notify can
therefore dequeue-and-mark exactly that node, consuming one
FIFO notify; a verbatim implementation that discards the old
node and enqueues a fresh one (a) strands that notify - another
waiter is never woken - and (b) eventually returns "timed-out"
on a wait that was successfully notified. The U-T2 watchdog
arms (a)/(b)/(c) never race a notify against the service
window, so the hole was untested.

FIX (NORMATIVE - this paragraph AMENDS annex W W1, replacing
its re-park sentence): after a no-terminate service, BEFORE
re-parking, the carrier performs OLD-NODE DISPOSITION under the
owning waiter list's listLock:
 (a) old node already notified/dequeued => the wait completes
     with "ok" immediately - NO re-park, NO fresh node (the
     consumed notify is honored, never stranded);
 (b) old node still enqueued and un-notified => remove it and
     tail-enqueue a FRESH node, then re-park (FIFO-position
     loss is declared and = the existing I10 eats-one-notify
     class).
At no point are both nodes live past the disposition; a notify
landing DURING the disposition serializes through listLock and
hits exactly one of (a)/(b). Lock ranks unchanged: listLock
(api rank 3) is taken only AFTER the full §J.3 reacquisition
and released before the re-park quantum wait, per api 5.9(e);
the §J.3 episode accounting (W1 service ends an episode;
re-park opens a new one) is unchanged - case (a) simply ends
the wait at episode end. The SAME disposition applies to ANY
future early-service exit from a §J.3 park (it is a property
of the J.3 carve-out, not of the watchdog specifically).
Corpus arm (U-T2 + U-T11): watchdog fires while main is parked
in property Atomics.wait AND a spawned thread notifies during
the service window => main's wait returns "ok"; a second
parked waiter + counted notify(1) budget asserts no notify was
stranded; GIL-on variant keeps landed behavior.

### rev-15 spec deltas (byte budget)
Spec header bumped to rev 15. To fund the two fixes inside the
50000-byte bound, two RATIONALE-ONLY compressions (no semantic
change; full text lives here): the §A.2.8 item-8 landed-
mechanics sentence ("Timer cb only sets the check bit ...
parked or exited") compressed to its annex-W pointer; the
§A.2.7 opening parenthetical (Debugger.h:342 pause-state
inventory) compressed - full inventory in the r11 F3 entry
above. No other section changed meaning in rev 15.

# REV 16 (2026-06-06) - review round: 6 findings (2 blocker, 4
major), ALL VERIFIED REAL against the tree; all fixed.

## F1 (blocker) haveABadTime unruled; §K taxonomy cannot express it

VERIFIED: JSGlobalObject::haveABadTime (JSGlobalObject.cpp:2900)
is JS-reachable on any shared global (JSGlobalObject.cpp:2460
prototype-chain indexed-interception path, plus the
Structure::mayInterceptIndexedAccesses install sites). Its body:
fireWatchpointAndMakeAllArrayStructuresSlowPut (the :2854 comment
documents the COMPILER-thread race only - the mutator side is
GIL-serialized today), the StructureCache clear + watchpoint fire
(:2814 helper), then a HeapIterationScope +
objectSpace().forEachLiveCell walk (:2970) converting every
affected object to SlowPutArrayStorage, optionally a multi-global
dependency-graph pass. Zero coverage in the five frozen SPECs,
the INTEGRATEs (only a jit corpus-test NAME), and SPEC-ungil
rev 15. §K.4's audit verdict set (class 1/2/3) had no disposition
that fits: not per-lite duplicable, not leaf-lockable (the walk
reads/writes every thread's objects), not CAS-publishable.

### ANNEX HBT (BINDING) - haveABadTime under N mutators

GIL-off, the ENTIRE haveABadTime body from the
isHavingABadTime() early-return to the end of the conversion
walk runs as ONE §A.3 thread-granular stop:

1. The calling mutator requests the stop (§A.3.3 arbitration; it
   is the conductor). Losers of arbitration park and RETRY; on
   retry the isHavingABadTime() early-return makes double entry
   idempotent for the same global; DIFFERENT globals' calls
   serialize through the same arbitration (each runs its own
   complete stop).
2. After all other entered threads are parked/not-entered/
   access-released (§A.3.2, gated by 2b), the conductor RE-CHECKS
   isHavingABadTime() (another thread may have completed the same
   transition while it waited), then runs the landed body
   unmodified ON ITS OWN CLIENT with heap access held, per the
   R1.i client-scoped bracket shape (§A.3.5). The body MAY
   allocate (ArrayStorage conversion, Vector growth): all other
   mutators are parked access-released, so an emergency shared GC
   inside the window degenerates to the single-client case;
   DeferGC stays as landed.
3. Ordering: the m_havingABadTimeWatchpoint fire + structure
   transitions happen INSIDE the stop; mutator re-entry/
   re-acquisition is blocked until resume (§A.3.4/§A.3.8 class
   gate via §A.3.2b). Hence no thread can allocate in a fast
   indexing mode after the watchpoint fired but before the walk
   saw the heap - the missed-conversion window of the finding
   cannot exist. The CONCURRENT-COMPILER half is unchanged: the
   landed jit I2/R1 watchpoint/jettison protocol (the :2854
   comment's race) already covers compiler threads, which do not
   park under §A.3 (jit R1.f cooperative set = mutators).
4. StructureCache invalidation (:2814) and the multi-global pass
   run inside the SAME stop (one stop per haveABadTime call).
5. GIL-on/flag-off: unchanged (GIL is the serializer; stop not
   requested).
6. §K.4 taxonomy WIDENED: class 4 "requires-stop" - any
   GIL-serialized writer that must iterate or rewrite other
   threads' objects/structures. The audit routes haveABadTime
   here and any peer it surfaces (candidate class: realm-wide
   invalidation / global-object reset walks). A class-4 ruling
   must name its stop kind (§A.3 vs piggyback on a heap §10
   stop, like §D.1's rebias) and its double-entry serialization.
7. Corpus (U-T13): spawned thread allocation + indexing-type
   transition storm racing main installing an indexed accessor
   on a shared prototype (triggers haveABadTime); assert all
   affected arrays read SlowPutArrayStorage semantics after; a
   two-global double-fire arm; TSAN + amplifier.

## F2 (major) §K.3 self-deadlock on recursive lazy init

VERIFIED: LazyProperty.h:75-76 documents the recursion contract
("gracefully supports recursive calls ... simply return null");
LazyPropertyInlines.h:99-100 implements it (callFunc returns
nullptr when initializingTag is set). rev-15 §K.3 had no owner
identity in the state word and parked ALL non-winners: a
re-entrant get() from the initializing thread would park the
winner on itself - deterministic self-deadlock on a supported
pattern. FIX (AMENDS §K.3, BINDING): the winning CAS records the
OWNER - implementation may use a per-VM side table
{property address -> carrier TID} under a leaf lock, or spare
bits adjacent to initializingTag where the representation has
them; a get() during init from the OWNER thread returns null,
exactly the landed contract; ONLY FOREIGN threads take the
park-capable wait loop. Foreign-get-during-init parking is a
GIL-off-only behavior (under the GIL a foreign thread cannot
observe mid-init state), NOT an SD - no phase-1-observable
change. The U26 arm gains: a deliberately recursive initLater
initializer (touches its own property mid-init, expects null)
plus a concurrent foreign toucher that must park, then observe
the initialized value.

## F3 (major) promiseRejectionTracker fires on spawned threads

VERIFIED: JSPromise.cpp:405/:464/:502 (Handle) and :637 (Reject)
invoke globalObjectMethodTable()->promiseRejectionTracker
synchronously on the calling thread; under §E.1b/SD10 settlement
and then() run on arbitrary spawned threads; rev 15 ruled
queueMicrotaskToEventLoop (X1.7) and the DWT hooks (annex E7)
but not this third embedder-callback surface. FIX (BINDING,
§E.1b.4): GIL-off the tracker is invoked INLINE only when the
acting thread is a main/embedder carrier. Spawned-thread
Reject/Handle events are appended (no JS, no allocation beyond
the record) to the annex-E7 m_pendingLock-guarded handoff queue
as tracker records {promise Strong, operation}, flushed and
EXECUTED at the §F.1 carrier drain points like off-carrier DWT
work; ordering vs carrier-side tracker events is unspecified
(SD15; the unhandled-rejection report may arrive a drain late,
never lost while the carrier still drains; process-exit-before-
drain drops are the same class as landed exit-before-microtask
drains). Strong create/clear inside the record follows §F.3
(enqueuer holds a token; carrier clears under its token). No
hooks installed => same routing (the queue is DWT-owned, not
hook-owned); a VM with no carrier ever draining leaks reports,
declared. AUDIT (U-T8e, runs with U-T8b/c, gates U-T9):
enumerate EVERY globalObjectMethodTable / host-callback slot
reachable from JS on a spawned TS
(reportUncaughtExceptionAtEventLoop, moduleLoader* /
importModule, shadowRealm hooks, codeForEval/canCompileStrings,
deriveShadowRealmGlobalObject, currentScriptExecutionOwner etc.)
and give each an IU-table disposition: inline-safe /
carrier-queued (this mechanism) / refused-with-error /
unreachable-on-spawned (proof). Corpus (U-T9): spawned resolver
rejects a shared promise with a Bun-style tracker installed =>
report arrives on the carrier; handle-after-reject arm.

## F4 (major) §A.3.7 wrong Heap.cpp site + dropped disjunct

VERIFIED: Heap.cpp:2348 sits in the m_worldState
hasAccessBit/mutatorHasConnBit RELEASE_ASSERT cluster - no atom
assert. The 14th atom assert is Heap.cpp:2796
(Heap::requestCollection): RELEASE_ASSERT(vm().atomStringTable()
== Thread::currentSingleton().atomStringTable() ||
worldIsStoppedForAllClients()) - the disjunct is the landed T5b
late-ISS-flip guard (companion ASSERT :2795). rev 15's uniform
"tables equal" GIL-on arm would have deleted the disjunct =>
flag-on/GIL-on RELEASE_ASSERT crash, violating the master rule.
FIX: §A.3.7 now cites :2796 and states the rewrite as
predicate-preserving - each assert's LANDED predicate P becomes
"gilOff ? sharedAtomStringTableEnabled() : P". Count re-verified:
Completion.cpp x12 + Identifier.cpp:77 + Heap.cpp:2796 = 14.

## F5 (major) pending waitDeadlines orphaned at termination close

VERIFIED: rev-15 §E.2 close harvested only taskQueue; §E.5
termination closes with keepalive>0 and says nothing about
waitDeadlines; §E.7.5 routes the finite-timeout timer for
spawned registrants EXCLUSIVELY to the owner's EXPIRE step. A
terminated owner therefore strands an enqueued PWT waiter whose
only timeout driver is dead: a finite-timeout property
Atomics.waitAsync promise that must settle "timed-out" instead
hangs unless notified. FIX (BINDING, §E.2 close block - §E.5
takes the SAME close path so it inherits): close harvests
waitDeadlines under inboxLock together with taskQueue; after
dropping inboxLock (heap access re-acquired), for each harvested
entry: dequeue the waiter under its listLock (already-notified/
dequeued => skip, the in-flight settle wins), DROP listLock,
settle "timed-out" via §E.4 - the inbox is already closed, so
§E.4 takes the MAIN fallback (landed scheduleWorkSoon path);
keepalive is DEAD post-close (E.3 r3) so the rule-1 decrement
skip is the existing exactly-once story. Semantics: early
"timed-out" (before the wall-clock deadline) at owner close/
termination - declared as the SD8 EXTENSION (SD8 now: terminate-
parked => Failed completion AND pending finite-timeout waitAsync
registrations settle "timed-out" at close). Rationale for
settle-over-re-register: re-registering on main's 5.6 timer
would keep a dead thread's deadline machinery alive cross-
thread for no observable benefit (the promise's only landed
outcomes are ok/not-equal/timed-out; "timed-out" is the honest
one). Corpus (U-T11): terminate a spawned thread holding a
pending finite-timeout property waitAsync; assert the promise
settles "timed-out"; variant where a notify races the close
harvest (exactly one of ok/timed-out, never both, never hang).

## F6 (blocker) §LK vs api §5.9 NLS::m_lock re-rank unrecorded

VERIFIED: SPEC-api.md §5.9 ranks NLS::m_lock rank 4 LEAF and (f)
says "Ranks not swapped ((e) needs the rank-4 leaf)"; §LK orders
it OUTSIDE heap 2-10 + api 1-3 with no SUPERSESSION record (the
§LK header recorded only the VMLiteRegistry and destructor-leaf
supersessions). The two readings are semantically equivalent -
api 5.9(e) (held across GIL reacq) + (f) (may take QL/queueLock
while held) ARE the leaf-form statement of "this lock is outside
those ranks" - but the project convention requires contradictions
of frozen normative text to carry a both-sides record, else U20
(order lint) and the GIL-on 5.9 assert infra derive conflicting
rank tables. FIX: explicit SUPERSESSION added to §LK: §LK's
outer ordering is the BOTH-MODES canonical form; api §5.9's
rank-4-leaf + (e)/(f) exemptions remain the GIL-on assert
encoding, behavior unchanged; IA row.

### rev-16 spec deltas (byte budget)
Header bumped to rev 16. To fund the six fixes within 50000
bytes, RATIONALE-ONLY compressions of bodies whose FULL text is
already BINDING here (no semantic change): §A.2.8 watchdog body
(annex W + r15 F2), §N.6 body (annex N6), §A.3.6 body (annex
A36), §C.1 arm list (annex C1), §J.3 body (r10 F5/U31), U0c
body (annex U0C). Their main-doc text remains a faithful index;
the annexes govern.

### ANNEX E1B (BINDING) - §E.1b.2 full text (moved verbatim from
spec rev 15 for byte budget; semantics unchanged)
Concurrent then()/resolve(): GIL-off, JSPromise internal-state
transitions run under the promise's JSCellLock (10a) - internal
fields are NOT §9.5 slots. Bodies RESTRUCTURED per OM I20 (no GC
alloc under 10a): allocate reactions (+ Bun InternalFieldTuple)
OUTSIDE; re-check status under the lock (settled => drop,
queueMicrotask post-unlock; Pending => re-read reactionHead,
publish via setPackedCell); resolve/reject swap status + extract
the chain under it, enqueue post-unlock. GIL-on unchanged.

### ANNEX F1B (BINDING) - §F.1 main/embedder arm full text (moved
verbatim from spec rev 15; semantics unchanged)
Main/embedder: REAL lock semantics - m_lock still mutually
excludes embedder threads (Bun exclusion kept). Acquiring it ALSO
takes an entry token (§A.3.1 set uniform) + the §A.3.6
carrier+tag swap; GIL-on extras skipped per §§A.3.7/B.3: FIRST
entry creates the carrier lite + its GCClient::Heap (main reuses
the original client; embedder creates one, §B.2), runs ACT (heap
I4(b)); EVERY lock() runs §A.3.2b/§A.3.8-gated acquireHeapAccess
on THAT client (idempotent at depth>0, F8 step 0); unlock() at
depth 0 releases. Spawned-conductor GC scans a lock/eval/unlock
embedder's stack (U27/U-T6 negative). Drain-on-release KEPT
GIL-off: willReleaseLock drains the CURRENT lite's queue (I11;
other drains: embedder runloop/DWT §E.4, explicit
drainMicrotasks, §E.7.3 flush). Park sites release m_lock per
§J.3.

### ANNEX A26 note (BINDING) - §A.2.6 definitional sentence
(moved verbatim): "Both modes"/"deleted" in §A.2.6 = both GIL
modes UNDER useJSThreads=1; flag-off keeps the landed vanilla-SAB
machinery compiled and live.

(rev-16 budget note extended: §E.1b.2, §F.1 main/embedder arm,
and the §A.2.6 definitional sentence are now indexed in the spec
with these annexes carrying the full text.)

### ANNEX D1 (BINDING) - §D.1 Task 13 rebias full text (moved
verbatim from spec rev 15; semantics unchanged)
Task 13 (om:377, 8c) - IN SCOPE. Rebias runs world-stopped INSIDE
the next FULL shared collection under the heap §10 GC stop
barrier - NOT a §A.3 stop (jit R1.h); re-entry blocked per
§A.3.8. Restamps dead TIDs' butterfly tags + structure TIDs to 0;
TM reissues via m_freeTIDs. Trigger: >=75% of 2^15 arms the next
full collection; spawn during exhaustion RangeErrors (api
5.1/I17) until rebias completes; lifts Dev 10. Enumeration =
world-stopped HeapIterationScope + StructureID-table walks.
Two-phase vs §LK (conductor acquires NO api lock): PRE-STOP
mutator-side dead-TID snapshot under TM::m_lock; conductor
restamps FROM THE SNAPSHOT; m_freeTIDs released POST-RESUME under
TM::m_lock BEFORE the gate lifts (soundness: history r9 F2).
Amplifier: U-T12's two-VM TM churn.

### ANNEX E3 (BINDING) - §E.3 keepalive accounting full text
(moved verbatim from spec rev 16; semantics unchanged)
keepaliveCount counts outstanding registrations that may still
enqueue a task here; transitions under the registrant's inboxLock;
exactly-once via per-ticket m_keepaliveReleased, CONSTRUCTED true
(=released). The INCREMENT site ALONE stores false (=armed) before
the ticket is visible; rules 1-2 decrement ONLY on winning the
false->true CAS - never-armed tickets (asyncJoin, TA waitAsync,
main/embedder) lose, never decrement (else wrap). U8
mutual-asyncJoin-OPEN arm.

INCREMENT (+1), once, at registration (I20 addPendingWork), on the
REGISTERING TS: every spawned-TS AsyncTicket EXCEPT asyncJoin -
asyncHold, cond.asyncWait, property Atomics.waitAsync (§C.3).
Main/embedder registrations never touch keepalive (§E.7).
asyncJoin: NO keepalive - settles only at the JOINEE's close
(F5/§E.2; counting deadlocks mutual/self asyncJoin - history);
closed registrant => E.4 main fallback (I12). SD12; mutual/self
arms. TA Atomics.waitAsync: NO keepalive - not an AsyncTicket;
WLM settles via DWT scheduleWorkSoon MAIN-side. SD11; re-home
REJECTED v1 (history; §E.7.5 covers PROPERTY waitAsync only).

DECREMENT (-1), exactly once - every site first wins the
m_keepaliveReleased CAS; losers do nothing:
1. Settle-enqueue (E.4): decrement in the SAME inboxLock section as
 the append, iff inboxOpen (closed: CAS won, decrement SKIPPED,
 main fallback).
2. Cancel (VM-shutdown cancelPendingWork, api 5.5; D5 bailout):
 iff CAS won AND inbox open, under inboxLock.
3. Inbox-close: NO claim step - inboxOpen=false => counter DEAD;
 a later settle/cancel wins its CAS, the open check skips =>
 main fallback. Exactly-once (U8) from 1-2.

- U9: decrement + append atomic under inboxLock; E.2's exit
 check reads both under the same lock; decrementer signals
 before unlocking. Intentional leak: never-notified waitAsync/
 asyncHold keeps keepalive>0 => join hangs (api 4.6.2); §E.5
 escapes.

## Rev-17 findings and dispositions (whole-design cross-check vs
## the composed six-spec system)

### F1 (BLOCKER, accepted): annex HBT's access-held allocating
### conductor reaches a GCL self-deadlock
VERIFIED against three frozen authorities the r16 spec left
unsuperseded: (1) heap §9 Heap::JSThreadsStopScope "pre: access
released" (SPEC-heap.md:201-205) and jit R1.i "release this VM's
heap access -> JSThreadsStopScope -> stop -> resume -> re-acquire"
+ "Closures: allocation-free" (SPEC-jit.md:234) - yet annex HBT
item 2 had the §K.5 conductor run the landed haveABadTime body
"with heap access held" and allowed it to ALLOCATE; §A.3.5 still
said "R1.i GC bracket unchanged". (2) heap §9 CSAC/RCAC/SINFAC
precondition "not in stop window" (SPEC-heap.md:184) - the landed
body (JSGlobalObject.cpp:2899 ff) allocates ArrayStorage
butterflies for an unbounded found-object set under a
function-scoped DeferGC whose destructor runs
decrementDeferralDepthAndGCIfNeeded INSIDE the stop closure. (3)
If that (or allocation-failure CIND) enters the heap §10.2
election synchronously, tryLock(GCL) fails against the
conductor's OWN JSThreadsStopScope (GCL, rank 2; heap §10C(e)
GCL-busy arm): release access, timed GEC wait, park in NVS if a
VMM stop pends - one DOES (the conductor's own §A.3 stop). The
conductor parks on its own stop holding GCL: a self-cycle no
assert catches (heap I6 only forbids ranks >=4 in NVS). Unsound
for every JS-reachable haveABadTime on a GIL-off VM
(JSGlobalObject.cpp:2460) and every future allocating class-4
ruling. FIX = ANNEX HBT2 below; §A.3.5/§K.5 re-indexed.

### ANNEX HBT2 (BINDING) - class-4 conductor bracket + the
### no-GC-in-window rule (AMENDS annex HBT item 2; HBT items
### 1,3-7 stand)

1. R1.i CLASS-4 VARIANT. SUPERSESSION (heap §9 JSThreadsStopScope
   "pre: access released" + jit R1.i access-release step + R1.i
   "Closures: allocation-free (OM O4)" vs this, both sides;
   APPLIES ONLY to §K.5 class-4 conductors; IH + IJ rows): a
   class-4 conductor RETAINS heap access on its OWN
   GCClient::Heap across its §A.3 stop window and MAY allocate
   from it. Soundness: the §A.3.2 conductor predicate requires
   every OTHER entered thread parked/not-entered/access-released,
   so exactly one access-held client exists - the conductor
   itself; heap F8/§10.4 barriers never wait on the conductor
   (no shared GC can be IN PROGRESS: the §10.2 election cannot
   complete against the conductor's GCL scope, and rule 2 forbids
   the conductor starting one); §A.3 sets no client-visible GC
   stop state (§A.3.2b), so no GC barrier is active in-window.
   The default R1.i bracket (access released, allocation-free
   closure) REMAINS the rule for every non-class-4 §A.3 closure.
2. NO-GC-IN-WINDOW (normative, ALL §A.3 stop windows, any
   closure class). SUPERSESSION (heap §9 CSAC/RCAC "not in stop
   window" precondition vs this, both sides; IH row): inside a
   §A.3 window, GC initiation is FORBIDDEN; the §10.2 election
   is NEVER entered. CIND, DeferGC-exit
   (decrementDeferralDepthAndGCIfNeeded - haveABadTime's
   function-scoped DeferGC dies inside the closure), and
   allocation slow paths reached by the conductor instead
   ENQUEUE a ticket (RCAC arm only, under m_threadLock - legal:
   rank 5, taken stop-free) and RETURN; the deferred-GC check
   re-runs on the conductor AFTER resume + scope exit, where the
   normal §10 protocol serves the ticket. Debug enforcement:
   the conductor brackets the window in heap I14's STW-forbidden
   counter (incrementSTWForbiddenScope, heap §9), which
   CSAC/SINFAC entries already check - extends I14's machinery,
   no new facility.
3. In-window allocation failure: the conductor's allocation uses
   AllocationFailureMode that may take the heap L3 conductor
   allowance (MSPL freely, world is stopped) to grow/handout
   directories; if memory is truly exhausted it FAILS HARD
   (RELEASE_ASSERT/OOM crash) - it never collects in-window.
   Pre-sizing (Vector::reserveCapacity before requesting the
   stop) RECOMMENDED non-normative.
4. Annex HBT item 2's sentence "an emergency shared GC inside
   the window degenerates to the single-client case" is
   SUPERSEDED (it was the unsound arm): there is NO in-window
   shared GC, emergency or otherwise.
5. Corpus (U-T13, extends HBT 7): haveABadTime fired from a
   thread whose conversion walk must grow (large found-set) with
   a deliberately tiny nursery + a pending GC ticket; assert the
   ticket is served post-resume, no in-window election, I14
   counter clean.

### F2 (MAJOR, accepted): GIL-off settle under api rank-3 locks
VERIFIED: §E.4 GIL-off settle takes m_registrant->inboxLock
(rank 3); frozen call sites hold another rank-3 lock at the
settle: api 5.5a A "u/QL set m_asyncHeld/m_asyncHolder, settle"
and P "dequeue head, set ..., settle" (SPEC-api.md:206-209,
under NLS::m_queueLock; §E.7.5 runs P INLINE on the notifier for
spawned registrants), and F5 asyncJoin "u/joinLock - !=Running =>
schedule settle" (SPEC-api.md:140). GIL-on legal (settle = DWT
scheduleWorkSoon, no rank-3 lock); GIL-off = two rank-3 locks
held together, violating api 5.9(d)/§LK.4 "mutually unnested".
No reverse edge today (table violation, not a cycle), but it
breaks the U20 lint premise. FIX (BINDING, §E.4): caller
precondition - AsyncTicket::settle is invoked holding NO api
rank-1..3 lock. SUPERSESSION (api 5.5a A/P settle-under-QL +
api F5 asyncJoin settle-under-joinLock vs this, both sides;
GIL-OFF ONLY, GIL-on text stands; IA row): A/P record the
granted ticket under QL, DROP QL, then settle (the F5 Compl
"drop joinLock; settle moved tkts" shape - no lost grant: the
ticket is already owner, R/P observe m_asyncHeld);
asyncJoin drops joinLock before scheduling the settle (no lost
wakeup: Phase re-checked under joinLock before the drop decides
settle-vs-append, and completion settles appended tickets).
U-T8/U-T9 IU table enumerates ALL settle call sites with their
lock context; U20's lint flags settle-under-rank-3.

### F3 (MAJOR, accepted): DWT m_pendingLock leaf-ness vs
### unstated wake-edge lock context
VERIFIED: §E.7.3/4 + annex E7 never state whether
onCrossThreadWorkEnqueued / vm.runLoop().dispatch fire inside or
outside the m_pendingLock section. Inside => leaf violation
(RunLoop::dispatch takes RunLoop's lock; the hook runs embedder
code - Bun's wake primitive plausibly takes its event-loop lock)
and a real cycle: spawned thread holds m_pendingLock -> Bun loop
lock, while the carrier holds its loop lock -> DWT
cancelPendingWork/settle -> m_pendingLock. FIX (BINDING, amends
annex E7 rules 3-4): the handoff-queue append, removal, and
emptiness reads happen under m_pendingLock; the wake (hook call
or vm.runLoop().dispatch()) fires strictly AFTER dropping it.
Missed-wake closure: append happens-before the post-drop wake;
the carrier drain re-checks queue-nonempty under m_pendingLock
after each wake, and a wake-side race (drain between drop and
wake) is benign (spurious wake). Boot-check contract gains:
onCrossThreadWorkEnqueued is invoked with NO JSC lock held and
must not reenter JSC. U24 Bun arm gains a
hook-that-takes-the-loop-lock variant.

### F4 (MAJOR, accepted): no owner for the two embedder-side
### GIL-off deltas
VERIFIED: (a) §F.1 keeps m_lock excluding only embedder threads;
Bun's out-of-tree JSLockHolder critical sections today exclude
ALL JS and silently stop excluding spawned threads GIL-off -
§F.2/U-T8 audits in-tree consumers only. (b) Per §E.1b.1
(SD10) + X1.7, a main/embedder-registered ordinary-promise
reaction resolved by a spawned thread runs on the spawned
settler, never consulting queueMicrotaskToEventLoop - Bun
main-thread await continuations execute off-carrier, outside
m_lock and Bun's loop accounting. Internally consistent
(I11/U22) but no embedder-disposition row owns it (U-T8e covers
hooks invoked ON spawned threads, not main-registered callables
invoked BY spawned settlers; contrast SD15's carrier-queueing
choice). FIX = new §F.6 (BINDING): both deltas stated
normatively; an IU row carries the embedder-side checklist (Bun
JSLockHolder exclusivity audit; main-registered continuation
thread-affinity disposition, citing SD10/X1.7 - the embedder may
demand a carrier-hop variant pre-flip, which would be a NEW
negotiated SD, not a silent one); embedder sign-off is an
explicit U-T14 close item beside the IU dispositions. No design
change to §E.1b v1 (SD10 stands inside JSC).

### F5 (MAJOR, accepted): §N.5 unconditional intrinsic LOWERING
### vs the flag-off serial-cost gates
VERIFIED co-unsatisfiability risk: r11 F4/r15 F1 mandated
LLInt/Baseline lower the claim to a slow-path HOST OPERATION
call in ALL flag states; flag-off that is an out-of-line C call
replacing three inline bytecodes per generator/await resume,
squarely against BENCH.md's 1%-vs-pre-threads-baseline flag-off
gate and r10 F6's --useJIT=0 in-noise requirement, with no named
contingency (the r11 "observably identical" rationale is
semantic, not zero-cost). FIX (BINDING, REPLACES the r11 F4 /
r15 F1 LOWERING sentences; the intrinsics, their semantics, the
uniform-bytecode rule, and the publish/claim pairing all STAND):
bytecode stays uniform; the LOWERING is keyed on the §A.1.3
two-level discriminator. LLInt/Baseline: branch on the
JSCConfig gilOffProcess byte - false (flag-off + every GIL-on
process) => the landed INLINE plain get+compare+put (claim) /
plain store (publish), i.e. exactly today's machine code behind
one not-taken branch (the §A.1.3 delta-(a) class; where the site
sits in an ifJSThreadsBranch region the test NESTS - zero new
flag-off branches); true => host-op slow path (v1; LLInt inline
CAS via the jit annex R5 emitter class is the NAMED CONTINGENCY
if the gilOff-arm cost matters, not a flag-off concern).
DFG/FTL: the AtomicInternalFieldClaim/Publish nodes lower at
codegen time on the COMPILED-FOR VM's mode (§A.1.3/U0c fixes it
pre-codegen): gilOff => seq_cst strongCAS / release store; else
the landed plain get+compare+put / plain PutInternalField store.
Bench contract made explicit: the §B.5 r9 async/generator
microbench joins the BENCH.md flag-off suite as a GATED
benchmark at the standard 1% threshold vs the pre-threads
baseline (now satisfiable: flag-off code is the landed sequence
+ one not-taken branch); its gilOff configuration is RECORDED
under the §B.5 composite budget, not separately gated. The r10
F6 --useJIT=0 in-noise re-run stands and now covers these sites
with the same one-branch delta. §A.1.3 delta (b2) is RE-SCOPED:
the uniform bytecode change whose flag-off LOWERING is the
landed sequence behind the delta-(a) branch (golden disasm
re-baselined once, as before).

### F6 (MAJOR, accepted): §E.4 dead/closed-registrant fallback
### is an undeclared supersession of api:200
VERIFIED: SPEC-api.md:200 post-GIL surface rules the closed arm
"append to MAIN TS INBOX (compl seq closes inbox u/inboxLock,
drains residue to main)"; §E.4 rules it "FALLBACK to MAIN via
the LANDED scheduleWorkSoon path" while §E.1 makes the main
inbox structurally dead (never opened). The design choice
stands (cannot append-and-wake a never-opened inbox;
scheduleWorkSoon is the landed mechanism and composes with
§E.7.3-4), but it renegotiated frozen api text without the
both-sides citation, and the two routings differ observably
(main-task-queue ordering vs DWT/runloop + hook queue) - the
U19 oracle and corpus hang on which is binding. FIX (BINDING):
§E.4 carries an explicit SUPERSESSION (api 5.5 :200 "else
append to main TS inbox" arm vs §E.4 scheduleWorkSoon fallback
+ §E.1 main-inbox-never-opens, both sides; IA row) and no
longer describes itself as the api:200 surface verbatim; the
main-inbox arm of api:200 is DEAD GIL-off. GIL-on unchanged
(api:200's GIL-phase paragraph). No design change.

### Rev-17 SD note (IDs frozen)
No new SDs. F4(b) is SD10 restated at the embedder boundary
(disposition row chartered, not changed); F5 keeps flag-off
behavior bit-identical-but-one-branch; F2/F3/F6 are
lock-ordering/routing fixes with no JS-observable delta beyond
what SD3 already states.

### Rev-17 §T deltas (extends rev-9 annex 3 + r10-r16 deltas)
- U-T8: + settle-call-site lock-context table (F2); + embedder
  contract checklist row (§F.6; Bun JSLockHolder audit +
  main-registered continuation disposition).
- U-T9: + U24 hook-takes-loop-lock variant (F3); + A/P/asyncJoin
  drop-then-settle corpus arm (F2: grant races release; asyncJoin
  races completion).
- U-T13: + HBT2 arm 5 (in-window GC ticket, I14 counter).
- U-T14: + embedder sign-off close item (§F.6); flag-off
  delta re-audit now checks the F5 lowering rule (no host-op
  call reachable gilOffProcess=false).

### rev-17 spec deltas (byte budget)
§A.3.5 rewritten (class-4 variant + no-GC-in-window index, full
text here); §K.5 re-pointed at HBT2; §E.4 gains the F2
precondition + F6 supersession; §E.7.3-4 gain the F3
wake-after-drop sentence; new §F.6 (index; full text = F4
disposition above); §N.5 lowering sentences replaced per F5.
To stay under the cap, §E.3's index was compressed (full text =
ANNEX E3, unchanged) and §A.2.8's index trimmed (full text =
annex W + r15 F2, unchanged).

## Rev-17 spec compressions (normative text preserved here;
## extends the rev-17 byte-budget note - the full trim list)

Indexes compressed in the spec; where the trimmed fragment is not
already in a BINDING annex, it is preserved verbatim below and
remains NORMATIVE:
- §A.1.3 GC-roots amplifiers: parked thrower survives full GC;
  two-VM.
- §A.3.8 amplifier: spawned-conductor shared GC, two same-VM
  threads mid-JS.
- §A.3.6: spawned threads are single-VM in v1; currentTID() =
  CARRIER TID (also annex A36).
- §A.2.8 U-T2 arms: wall-clock vs main parked/exited/in-JS -
  spawned terminated, no abort; notify mid-W1-service => "ok",
  none stranded (U-T11) (also annex W).
- §C.1 U5/U28 arm: owner unlocked AS store storm vs foreign CAS,
  SW=0 (also annex C1).
- §C.3 corpus: wait/waitAsync on Int32/Double/CoW racing a
  notifier.
- §E.1b.4 corpus: spawned reject w/ tracker => carrier report;
  handle-after-reject.
- §E.5 U-T11 arms: terminate w/ pending finite waitAsync =>
  settles "timed-out"; notify-races-close => exactly one of
  ok/timed-out.
- §F.1 main/embedder: acquireHeapAccess idempotent at depth>0;
  spawned-conductor GC scans a lock/eval/unlock embedder's stack
  (also annex F1B).
- §J.3 C-API arm: main parks in property Atomics.wait; second
  embedder notifies (also r10 F5).
- §E.2 EXPIRE: the §E.4 settle takes inboxLock (stated in §E.4).
Other rev-17 trims (E3, W, D1, N6, HBT/HBT2, F2, A16, A36, E1B,
F1B, U0C, E7 corpus lines) only re-index text whose BINDING full
form already lives in the named annexes.

## Rev-18 findings and dispositions (second whole-design
## cross-check vs the composed six-spec system)

### F1 (BLOCKER, accepted): class-4 conductor acquires GCL
### access-held - deadlock vs an in-progress shared GC's §10.4
### access barrier
VERIFIED. ANNEX HBT2 item 1 supersedes BOTH heap §9's
JSThreadsStopScope precondition ("pre: access released",
SPEC-heap.md:200) AND jit R1.i's access-release step
(SPEC-jit.md R1.i) for class-4 conductors, stating retention
"across its §A.3 stop window" - which includes the
JSThreadsStopScope ctor, i.e. GCL.lock() (heap rank 2).
Reachable interleaving: thread B conducts a shared §10
collection - it holds GCL from the §10.2 election through step
9, and at step 4 waits under GBL until EVERY client is NoAccess
(heap F8/§10.4). Thread A triggers a JS-reachable haveABadTime
(JSGlobalObject.cpp:2460), wins §A.3.3 arbitration
(uncontended), and - per HBT2 item 1 as written - enters the
JSThreadsStopScope ctor WITH access held, blocking on GCL
inside a native WTF::Lock wait: not a poll site, not AHA, so it
never services its GC trap bit, never F8 mandatory-reverts,
never signals GBC. Cycle: A waits on GCL (held by B); B's
step-4 barrier waits on A's client going NoAccess. HBT2's own
soundness paragraph ("no shared GC can be IN PROGRESS: the
§10.2 election cannot complete against the conductor's GCL
scope") is valid only AFTER the conductor holds GCL; it says
nothing about acquisition time, when a shared GC absolutely can
be in progress. heap §10C(e)'s GCL-busy rule covers only the
reverse direction (GC requester vs a HELD JSThreadsStopScope).
r17 F1 fixed the in-window self-deadlock and introduced this
acquisition-time one. Secondary hole: the spec never pinned
whether §A.3.3 arbitration strictly precedes GCL acquisition -
a second class-4 requester blocking RAW on GCL while retaining
access would hang the winner's §A.3.2
parked/not-entered/access-released predicate the same way. FIX
= ANNEX HBT3; §A.3.5(i)/§K.5 re-indexed.

### ANNEX HBT3 (BINDING) - class-4 GCL acquisition order
### (AMENDS ANNEX HBT2 item 1; HBT2 items 2-5 stand)

1. The class-4 conductor KEEPS the default R1.i access-release
   BEFORE the JSThreadsStopScope (GCL) acquisition. HBT2 item
   1's access retention begins ONLY once GCL is HELD; the
   HBT2-1 SUPERSESSION of the R1.i release step is RESCINDED -
   what remains superseded for class-4 conductors is (a) the
   "access released" precondition FOR THE HELD SCOPE (the
   conductor re-acquires inside it) and (b) R1.i's
   allocation-free-closure rule, per HBT2 items 1/3.
2. Immediately after the JSThreadsStopScope ctor returns (GCL
   held), BEFORE fanning its own §A.3 stop bits, the conductor
   re-acquires access on its OWN client via plain F8 AHA.
   Non-blocking proof: GSP is seq_cst-cleared at §10.8 before
   the prior GC conductor releases GCL at step 9, so under OUR
   held GCL no GC is in progress and no new §10.2 election can
   complete - GSP is false, F8 step (3) never triggers; the
   §A.3.2b stop-bit gate (i) sees no pending §A.3 stop because
   this conductor has not yet set its bits and (rule 3) no
   other §A.3 conductor can exist past arbitration; so AHA is
   one CAS, no park. Only then does the conductor fan stop bits
   and wait for the §A.3.2 predicate.
3. ORDER PINNED (normative, all §A.3 conductors, not just
   class-4): §A.3.3 arbitration (the park-aware job-slot mutex)
   STRICTLY precedes GCL acquisition; only the arbitration
   WINNER ever touches GCL. Losers park on the job-slot mutex -
   which counts as parked for the winner's §A.3.2 predicate -
   access-released per the default bracket; they NEVER block
   raw on GCL. Hence at most one thread (the winner) is ever
   blocked in GCL.lock(), and per item 1 it blocks
   access-released - the §10.4 barrier never waits on it, and
   it simply queues behind any in-progress shared GC (heap
   §10C(b)/(e) shapes unchanged).
4. HBT2 item 1's soundness paragraph is RE-SCOPED to the
   post-acquisition window (where it is correct); its
   access-retention sentence now reads through items 1-2 above.
   HBT2 items 2 (no-GC-in-window), 3 (fail-hard allocation), 4,
   5 stand unchanged.
5. Corpus (U-T13, extends HBT2 item 5): haveABadTime fired
   while a shared GC is mid-§10.4-barrier - force via a second
   mutator parked in native code that delays its NoAccess
   release; assert no deadlock, the class-4 stop runs strictly
   after the GC resumes, and the conductor's AHA at item 2
   never parks (instrumented).

### F2 (MAJOR, accepted): §E.4 closed/dead fallback runs the
### scheduleWorkSoon path (and the §E.7.3 wake) inside the
### registrant's inboxLock section
VERIFIED: §E.4 as indexed put the else-arm ("FALLBACK to MAIN
via the LANDED scheduleWorkSoon path") textually inside the
"under m_registrant->inboxLock" section. For a
spawned-registered (internal-arm) ticket settled off-carrier,
scheduleWorkSoon appends to the annex-E7 m_pendingLock handoff
queue and fires the wake - onCrossThreadWorkEnqueued or
vm.runLoop().dispatch - which r17 F3 (BINDING) requires to fire
after dropping m_pendingLock AND with NO JSC lock held
(boot-checked). With TS::inboxLock (api rank 3, §LK.4) held,
that contract is violated, and the exact cycle r17 F3 closed
for m_pendingLock reopens one lock higher: spawned thread holds
TS-X.inboxLock -> wake hook -> Bun event-loop lock, while the
carrier holds the loop lock -> §F.1 drain -> executes a handoff
settle -> AsyncTicket::settle -> TS-X.inboxLock. Same shape on
the §E.2 close path (closing thread settles harvested deadlines
to its now-closed inbox). The two normative statements (E.4
locked else-arm vs the F3 hook contract) cannot both be
implemented. FIX (BINDING, amends §E.4 + the r17 F3 contract):
decide-under-lock / act-after-drop. Under
m_registrant->inboxLock determine open/closed and, if open,
append + rule-1 decrement + notifyOne; DROP inboxLock; if
closed, invoke the scheduleWorkSoon fallback holding NO api
lock. Sound: inbox closure is MONOTONIC (inboxOpen set true
exactly once pre-fn, false forever at close - §E.1/§E.3 rule
3), so a post-drop fallback can never race a reopen; the
open-arm append/decrement stays atomic under inboxLock (U9
unchanged). Stated explicitly: the §E.7.3 wake (hook or
vm.runLoop().dispatch) fires with NEITHER m_pendingLock NOR any
TS::inboxLock (nor any other api rank-1..3 lock) held - the
§E.4 precondition (r17 F2) plus this drop rule make that
invariant; U20's lint extends to wake-under-rank-3. Routing
fix only; no observable delta beyond SD3. U24 gains: closed-
registrant settle FROM A SPAWNED THREAD with hooks installed
(hook takes the loop lock) - no deadlock, task reaches main.

### F3 (MAJOR, accepted): rebias leaves DFG/FTL code with baked
### dead structure-TID immediates live across TID reissue
VERIFIED: jit §5.5 Transition emission bakes the butterfly-less
E4 ownership check as "compare R5 tag vs tid<<48 IMMEDIATE when
specialized on S". ANNEX D1 restamps dead instance tags AND
Structure::m_transitionThreadLocalTID to 0 inside the heap §10
stop, then releases the dead TIDs to m_freeTIDs - but fires NO
TTL watchpoints and jettisons nothing, so compiled code holding
a dead tid as an immediate survives. When TM reissues that tid,
the new thread's R5 tag equals the stale immediate: it passes
the baked check on a structure whose actual transition TID is
now 0 and runs the lock-free transition path - violating OM I11
("every lock-free S-instance transition was by its E4 key
owner") and I15 (transitioner == the structure transition TID
when butterfly-less) and any I15 assert. The recorded r9 F2
soundness paragraph covers only INSTANCE tags (heap words);
U18/U-T12's "no live dead-TID tag post-stop" likewise. The
at-most-one-claimant exclusion argument is nowhere recorded and
would renegotiate frozen OM text silently. FIX = ANNEX D1R
(cheapest closure: fire-and-jettison in the same stop).

### ANNEX D1R (BINDING) - rebias watchpoint fire (AMENDS ANNEX
### D1; all other D1 text stands)

1. In the same heap §10 stop, for EVERY structure whose
   m_transitionThreadLocalTID is restamped (i.e. held a dead
   TID), the conductor ALSO calls fireTransitionThreadLocal
   (which fires writeThreadLocal too, om:325; OM F4 chain-fire
   applies as on any fire) BEFORE the stop resumes - hence
   strictly before the post-resume m_freeTIDs release that
   makes reissue possible. This jettisons every DFG/FTL/IC body
   specialized on such a structure (E4 emission requires the
   TTL set valid+watched; fire => jit §5.3/§5.6 jettison), so
   no baked tid<<48 immediate survives to the reissue point;
   OM I11/I15 hold by construction.
2. SUPERSESSION (jit I13 + OM §9.4/I13 "fired only in VMM STW"
   vs this, REBIAS-STOP FIRES ONLY, both sides; IJ/IO rows):
   the heap §10 stop barrier provides equivalent quiescence
   (every mutator parked/not-entered/access-released, WSAC
   set); jit §5.6's worldIsStopped() ALREADY includes the
   worldIsStoppedForAllClients() disjunct and routes such fires
   to branch 1 (run inline) - mechanics need no change; jit
   R1.d's ISB on NVS exit after a GC stop covers the resume
   side; conservative scan R2 + I7 gate the jettisoned-code
   frees as for any GC-stop jettison.
3. Cost bound: the fired set = structures holding dead TIDs -
   the same set D1 already enumerates for restamping; chain-
   fire bounds per OM F4 (jit Task-13 stop-budget gate covers
   it; rebias is a rare, exhaustion-driven event under SD9's
   spawn gate).
4. Instance tags need no fire: jit read/write predicates load
   the instance tag at runtime and compare against the R5
   per-thread TLS tag - neither side is baked into code as an
   immediate; restamp-to-0 + tag uniqueness suffice (r9 F2).
   Only the structure-specialized transition immediate is
   baked, and item 1 kills it.
5. Amplifier (U-T12, new arm): compile E4-specialized
   transition code against a dying thread's structure
   (butterfly-less path), exit the thread, force rebias, force
   TID reissue to a fresh thread, transition storm from the
   reissued thread vs a foreign locked transitioner; assert
   I15 (instrumented) and that the specialized CodeBlock was
   jettisoned during the rebias stop.

### F4 (MAJOR, accepted): finite-timeout property
### Atomics.waitAsync liveness is bound to the registrant
### reaching its §E.2 drain loop - undeclared delta => SD16
VERIFIED: §E.7.5 routes the landed api 5.6 finite-timeout timer
(vm.runLoop().dispatchAfter, G28) to a waitDeadlines entry on
the spawned registrant TS, expired only at §E.2 EXPIRE or the
r16 F5 close/termination harvest. The r12 F3 liveness argument
("the wait clause sleeps min(quantum, earliest deadline)")
holds only once the thread is IN the drain loop. A spawned
registrant that registers a finite-timeout property waitAsync
inside fn and then parks forever at a §J.3 site (cond.wait
never notified; sync join of a long-lived thread - §E.2: park
sites inside fn do NOT service the task queue; §J.3 quanta poll
ONLY lock-free state) or simply runs long JS never settles
"timed-out" - while under GIL-on/landed semantics the runloop
timer fires during the park (parks release the GIL) and the
promise (shareable/awaitable cross-thread) settles. api I22's
corpus shape (register, then return from fn) reaches the drain
loop and masks this. No SD covered it; the design is asymmetric
with SD11 (TA waitAsync deliberately kept MAIN-side for
registrant-independent liveness, re-home REJECTED v1, annex
E3). FIX (BINDING): DECLARE it - SD16. A finite-timeout
PROPERTY Atomics.waitAsync registered on a spawned TS settles
"timed-out" only when the registrant next reaches its §E.2
drain loop (EXPIRE) or closes/terminates (r16 F5 harvest, SD8
ext); a registrant parked forever inside fn (or spinning in JS
that never drains) never settles it. SUPERSESSION (api 4.5
waitAsync row '"timed-out" on finite timeout (5.6)' + api 5.6
G28 timer arming vs §E.7.5/this, GIL-OFF ONLY, both sides; IA
row): GIL-on keeps the landed timer + timing. Notify-driven
settlement is UNAFFECTED (PWT notify settles via §E.4 from the
notifier - no registrant dependence); only the TIMEOUT edge is
registrant-bound. Doc'd hazard mirroring api 4.6.2's leak
language: pair with the intentional-leak note in annex E3 (the
§C.3 keepalive holds the registrant open until expiry - so the
common case settles at the next loop iteration; the hang
requires a registrant that never drains again). Liveness
alternatives REJECTED v1 (recorded): (a) main-side DWT
fallback timer (SD11 shape) - dual-settler complexity vs the
m_settled CAS for a corner the embedder can avoid; (b) §J.3
quantum deadline check - quanta may poll ONLY lock-free state
(U2's bound) and waitDeadlines is inboxLock-guarded; a
lock-free earliest-deadline mirror is design creep for v1.
Either may be revived post-ungil as a non-breaking liveness
improvement (earlier settlement is always legal). Corpus
(U-T11): register finite waitAsync, park forever in cond.wait -
GIL-off: promise unsettled (bounded observation window), joiner
sees the api 4.6.2-class hang; GIL-on (U19 variant): settles
"timed-out". I22 keeps passing both modes (registrant returns
from fn).

### Rev-18 SD note
SD16 (F4 above) is the sole new SD; IDs stay frozen. F1/F2 are
deadlock/lock-ordering fixes with no JS-observable delta;
F3/D1R is internal (jettison + recompile; no semantic change -
it RESTORES OM I11/I15).

### Rev-18 §T deltas (extends rev-9 annex 3 + r10-r17 deltas)
- U-T13: + HBT3 item 5 arm (haveABadTime vs mid-§10.4-barrier
  shared GC; AHA-never-parks instrumentation).
- U-T12: + D1R item 5 reissue/jettison amplifier.
- U-T9: + U24 closed-registrant spawned-settle hook arm (F2).
- U-T11: + SD16 corpus arms (GIL-off unsettled / U19 GIL-on
  timed-out).
- U-T14: U20 lint extended to wake-under-rank-3 (F2).

### rev-18 spec deltas (byte budget)
§A.3.5(i) re-indexed (HBT3: release-then-GCL-then-reacquire +
arbitration-before-GCL pin); §E.4 else-arm moved after the
inboxLock drop (F2); §E.7.3 wake sentence gains the
no-inboxLock clause; §D.1 index gains D1R (fire-in-stop +
I13 supersession); §E.7.5 + SD table gain SD16.

## Rev-18 spec compressions (normative text preserved; same
## convention as the rev-17 list)

Indexes compressed to fit the cap, all against BINDING annexes
or full-text history entries that carry the trimmed detail
unchanged: §A.3.5(ii) (HBT2 items 2-3), §K.5 (HBT items 1-4 +
HBT2), §D.1 (annex D1), U0c (annex U0C), §A.3.6 ~VM (annex
A36), §F.1 main/embedder (annex F1B), §A.2.8 W1 (annex W + r15
F2), §C.1 lock-free arm (annex C1), §E.1b.2 (annex E1B), §N.6
TRANSFER (annex N6), §N.5 lowering/publish/microbench (r17 F5 +
r15 F1), §F.6 (r17 F4), §A.1.3 GC-roots (r6 F5), §A.1.3
delta-(a)/gilOff-byte wording, §A.1.6, §J.3 Bun-notifier note
(r10 F5), §E.1 host-hook wording, §E.2/E.4/E.7.3 wording.
Fragments not verbatim in a named annex, preserved here and
still NORMATIVE:
- §A.1.3 GC roots: the visit is the Heap.cpp:3585 class; the
  registry filter also covers §A.1.5/§A.2.3 fan-outs; U-T1 =
  this + §A.1.6 + §K.1.
- §A.2.8 W1: listLock is taken only AFTER reacquisition and
  dropped BEFORE any re-park.
- §K.5 taxonomy parenthetical: class-4 "stop kind" = §A.3 stop
  vs heap §10 piggyback.
- §A.1.3: VMLite::gilOff is copied from vm.m_gilOff AT
  REGISTRATION (also annex U0C "lites copy the byte").
- §A.1.3 delta-(a): the nested gilOffProcess test adds ZERO new
  flag-off branches inside ifJSThreadsBranch regions.

## Rev-19 findings and dispositions (third whole-design
## cross-check vs the composed six-spec system)

### F1 (BLOCKER, accepted) + F3 (MAJOR, accepted; same root):
### conductor lock-order contradiction - HBT3 item 3's
### all-conductor arbitration-before-GCL pin vs jit R1.i /
### jit section 7 / the spec-body default bracket
VERIFIED. Three binding texts gave the DEFAULT (non-class-4)
section-A.3 conductor opposite orders for the two stop-machinery
locks. (1) jit section 7's table (SPEC-jit.md:164-167) ranks
"[Heap GCL (rank 2) - ONLY inside STWR via R1.i] > [R1/VMM
world-stop ownership (STWR)]" - GCL OUTER to arbitration - and
jit R1.i's frozen step order (SPEC-jit.md:227-234) is "release
access -> JSThreadsStopScope (GCL) -> stop", which the landed
bracket (JSThreadsSafepoint.cpp:252-304) implements and the
in-tree restoration comment (:208-221, GCL at step 2 /
arbitration at step 3) instructs an M4 implementer to restore
verbatim. (2) Pre-r19 SPEC-ungil scoped the opposite order
(arbitration first) to "section-K.5 class-4 conductors ONLY"
(section A.3.5(i)), and section A.3.3 never mentioned GCL. (3)
ANNEX HBT3 item 3 pins arbitration-STRICTLY-precedes-GCL as
"normative, all section-A.3 conductors, not just class-4".
Building default conductors per (1)/(2) and class-4 per (3)
yields a reachable AB-BA cycle on the GIL-off shared server:
default conductor D (any Class-A watchpoint fire) acquires GCL
via JSThreadsStopScope, then LOSES R1.c arbitration and parks on
the job slot HOLDING GCL (jit R1.g: losers park during the
winner's stop); class-4 winner W (JS-reachable haveABadTime,
JSGlobalObject.cpp:2460) holds the slot and blocks in GCL.lock()
- D waits on W's slot, W waits on D's GCL. Compounding:
section LK had NO row for the job-slot mutex, so U20's lint
(keyed off section LK as the both-modes canonical form) could
not see the outermost lock of the stop protocol, and the
supersession of jit R1.i's step order / jit section 7's edge for
NON-class-4 conductors was recorded nowhere both-sides,
violating the master rule. DISPOSITION: promote HBT3 item 3 into
the spec body for ALL conductors - ANNEX HBT4 below; the five
SPECs stay frozen (jit section 7 + R1.i superseded HERE, both
sides cited).

### ANNEX HBT4 (BINDING) - all-conductor
### arbitration-before-GCL promotion (promotes ANNEX HBT3 item
### 3; HBT3 items 1-2/4-5 and HBT2 items 2-5 stand)
1. ORDER (normative, ALL section-A.3 conductors, default AND
   class-4): release access (R1.i's first step KEPT) ->
   section-A.3.3 arbitration on the park-aware pending-job-slot
   mutex -> (WINNER ONLY) Heap::JSThreadsStopScope (GCL) -> fan
   stop bits -> stop -> work -> resume -> drop scope ->
   re-acquire access. Losers park on the job-slot mutex
   access-released (counts as parked for the winner's
   section-A.3.2 predicate, per HBT3 item 3) and NEVER block raw
   on GCL. [r33: under useConcurrentSharedGCMarking the
   release-before-GCL order EXTENDS to GC-conduct window
   RE-ENTRY (the per-window blocking GCL acquire, legal because
   the conductor is access-released all tenure, SPEC-congc
   §3.1(a)-(b)/CG-I19; F15 first-window tryLock carve-out
   unchanged; election/poll tryLock-only) - SPEC-congc ANNEX
   CGS2.4(b)/§13.5(3), recorded both sides via the REV 33
   record + ANNEX CGS2A; flag-off this item stands as written.]
2. SUPERSESSION (both sides; the frozen jit text stands
   unedited, superseded here): (a) jit R1.i's step order
   "release access -> JSThreadsStopScope (GCL) -> stop"
   (SPEC-jit.md:227-234) - the GCL bracket moves AFTER
   arbitration for every conductor; (b) jit section 7's table
   edge "[Heap GCL (rank 2) - ONLY inside STWR via R1.i] >
   [R1/VMM world-stop ownership (STWR)]" (SPEC-jit.md:164-167) -
   INVERTED: job-slot/STWR arbitration is OUTER to GCL. R1.i's
   access-release-first step, client scoping, resume order, and
   (for default conductors) allocation-free closure all stand.
   [r33: the "allocation-free closure" clause of the previous
   sentence is STRUCK under useConcurrentSharedGCMarking
   (SPEC-congc F43/CGS2.4(a), recorded both sides via the REV 33
   record + ANNEX CGS2A - the conductor is a FULL CLIENT
   in-window, congc §9.1(8)/CGD7.1); flag-off the clause stands.]
3. Soundness (re-derived for the promoted order): at most ONE
   thread - the arbitration winner - is ever blocked in
   GCL.lock(), and it blocks access-released, so the heap
   section-10.4 barrier and section-A.3.8 per-thread GC parking
   never wait on it; it simply queues behind any in-progress
   shared GC (heap section 10C(b)/(e) shapes unchanged). GC
   conductors never touch the job slot (section-LK negative
   edges: GC/section-A.3 conductors acquire no api lock; the
   slot is section-A.3-conductor-only), so no cycle through GC.
   The pre-r19 AB-BA cycle is structurally impossible once no
   conductor can hold GCL while parked on the slot.
4. section LK gains row 4b - pending-job-slot mutex
   (section A.3.3): section-A.3 conductors ONLY; inner to rank
   1/token (the requester holds its entry token entering
   arbitration; token is ordering-inert per LK.1); OUTER to heap
   rank 2 (GCL); held across the ENTIRE stop window; losers park
   on it access-released; never held together with any api lock.
   U20's lint extends to it.
5. Licensed edits at U-T5 (IJ rows): (a)
   JSThreadsSafepoint.cpp:252-304 - bracket reordered, the
   arbitration call moves BETWEEN the access release and the
   JSThreadsStopScope ctor; (b) the :208-221 "Real sequence
   (R1.a-i), restored at integration" comment REWRITTEN
   (arbitration becomes step 2, GCL step 3) - restoring the
   landed comment verbatim builds the deadlocking order. The
   section-A.3.5 DEFAULT bracket is therefore no longer "the
   landed lines verbatim".
6. Class-4 (section A.3.5(i) / HBT3 items 1-2) is unchanged and
   is now an INSTANCE of the general order, not an exception.

### F2 (MAJOR, accepted): section-K.3 cross-thread initializer
### dependency cycles + abandoned (terminated-owner) inits hang
### foreign waiters forever
VERIFIED. r16 F2's owner-null escape is owner-only; two composed
cases were unruled. (1) Crossed cycle: A wins lazyP, its
initializer touches lazyQ; B concurrently wins lazyQ, touches
lazyP. Each is FOREIGN to the other's property, so each enters
the unbounded park-capable re-test loop - permanent hang that
GC/STW liveness masks from every watchdog (annex W's CPU budget
is carrier-only, SD14). Single-threaded the same shape resolves
benignly via the landed owner-null contract (LazyProperty.h:75
documents recursion as supported - evidence such chains exist).
(2) Abandonment: winner unwinds out of the initializer
(termination exception at a poll site, or section-E.5
termination) leaving lazyTag=initializing with a dead owner;
every later toucher on every thread waits forever. DISPOSITION:
ANNEX LZ1.

### ANNEX LZ1 (BINDING) - section-K.3 cycle escape + init
### abandonment (extends r16 F2; the r16 owner-null contract and
### foreign park-capable wait loop stand)
1. Wait-for edges. The per-VM owner side table (r16 F2, leaf
   lock) additionally records, per in-flight init, the set of
   waiting threads: a foreign waiter publishes (self ->
   ownerOf(P)) under the leaf lock BEFORE its first park quantum
   on P and erases it when it stops waiting (success, cycle
   null, or unwind).
2. CYCLE escape: before EACH park quantum (under the leaf lock,
   bounded walk - at most one in-flight init per thread, so the
   owner chain is a function), the waiter follows
   owner-of -> waits-on edges from P; if the chain reaches the
   waiter ITSELF (possible only if it OWNS some in-flight init
   Q), get() returns null - the landed owner-null recursion
   contract extended to cross-thread ownership cycles. Sound:
   cycle membership is stable while all participants wait
   (edges only retract when a waiter exits the loop, which
   breaks the cycle); at least one participant detects and
   nulls, unblocking the rest. Deterministic enough for U26
   (every participant that re-tests after edges complete
   detects). GIL-on unchanged; NOT an SD (GIL-off-only
   liveness; null on cycle is the landed recursion observable).
3. ABANDONMENT: the winner installs an unwind scope around the
   initializer call; ANY non-normal exit (JS/C++ exception,
   termination at an in-initializer poll site, section-E.5
   thread termination) CASes lazyTag initializing->empty and
   erases the side-table entry BEFORE propagating. Foreign
   waiters re-test and observe empty; a later toucher re-runs
   the initializer (sound: initializers publish only on success
   - the release-store IS the publication; partial work is
   garbage, collected normally). Thread exit (section-E.2 T5)
   and the ~VM walk (section A.3.6) ASSERT the thread owns no
   in-flight init.
4. U26 gains arms (r16's three stand): (d) two-thread crossed
   lazy-init cycle - A inits P touching Q, B inits Q touching
   P; assert no hang, at least one inner touch nulls, both
   props end initialized; (e) terminate owner mid-init (park
   the initializer at a poll site, deliver termination), then
   foreign touch - assert re-run completes.

### Rev-19 SD note
No new SDs; IDs stay frozen. F1/F3 is lock-order
normalization/bookkeeping (no JS-observable delta); F2 is
GIL-off-only liveness whose cycle-null extends the landed
recursion contract's existing observable.

### Rev-19 section-T deltas (extends rev-9 annex 3 + r10-r18)
- U-T5: + the HBT4 item 5 licensed edits (bracket reorder +
  :208-221 comment rewrite); job-slot mutex lands ranked per
  section LK.4b.
- U-T8b: section-K.3 side table gains wait-for edges + the
  winner unwind scope (LZ1 items 1/3); exit/~VM asserts.
- U-T9: + U26 arms (d)/(e).
- U-T14: U20 lint extended to the job-slot mutex (LK.4b).

### rev-19 spec deltas (byte budget)
Section A.3.3 gains the ORDER PIN + the HBT4 supersession;
section A.3.5 DEFAULT re-worded (arbitration between release
and GCL; "not verbatim" note); A.3.5(i)'s order parenthetical
collapses to "order per rule 3"; section LK gains row 4b;
section K.3 gains the LZ1 index + U26 arms (d)/(e).

## Rev-19 spec compressions (normative text preserved here;
## same convention as the rev-17/18 lists)
- section A.3.6 index re-tightened against annex A36 (BINDING,
  unchanged): dropped from the index only - "(m_mainVMLite
  tid-0 GIL-on-only)"; the TLS tuple spelled "{VM*, epoch,
  carrier}"; "never tag 0 or foreign-VM TID"; U1's predicate
  spelled "TLS tag == CURRENT lite TID && lite->vm == entered
  VM"; "JSLock.cpp:151 backstop replaced (section J.7)"; M6
  replacement detail "(registry-lock collection, TLS-dtor
  deferred destruction; epoch kills stale-cache UAF)". All
  remain normative via annex A36 + section J.7.
- Further rev-19 index trims, all against BINDING full-text
  history entries that carry the trimmed detail unchanged:
  - §E.3 (r16 annex E3): dropped from the index only - the
    "INCREMENT site alone arms pre-visibility / never-armed
    never decrement" parenthetical; "registering TS" phrasing;
    asyncJoin detail "(F5/§E.2; mutual/self safe; closed => E.4
    main fallback)" (SD12 pointer kept); TA waitAsync detail
    "(not an AsyncTicket; WLM settles DWT main-side)" (SD11
    pointer kept); the (1)/(2)/(3) decrement-site numbering and
    "SAME inboxLock section as append" (atomicity still stated
    via U9).
  - §E.7.3 (r8 annex + r11 annex E7 + r17 F3 + r18 F2): the r17
    F3 sentence-level restatement (append/removal/emptiness
    under m_pendingLock; append happens-before post-drop wake;
    carrier re-checks under the lock; hook = FOURTH hook) now
    lives only in the cited entries; index keeps the
    after-drop/no-rank-1..3/no-JSC-re-entry wake contract.
  - §E.7.5 (r18 F4): "parked-forever-in-fn never settles it" +
    the spelled-out api 4.5/5.6 supersession text (still cited
    as "incl. the api 4.5/5.6 SUPERSESSION"); notify-path
    note.
  - §E.4 precondition (r17 F2): the spelled-out api :206-209/
    :140 citations + "(F5-Compl shape)".
  - §C.3 (r9 F1 + r11 annex C3): "(eats one FIFO notify -
    I10)" -> "(I10)".
  - §F.1 (r16 annex F1B): "GIL-on extras skipped";
    "GCClient::Heap" spelled -> "client".
  - §K.5 (annex HBT): the whole-body enumeration "(watchpoint
    fire + StructureCache clear + HeapIterationScope walks +
    multi-global pass)" -> "(per annex)".
  - §J.3 (r10 F5): "unlockAllForThreadParking shape / else
    second-embedder notifier deadlock"; "CAPTURE the carrier
    lite BEFORE release ... never VMLite::current()" -> "off
    the pre-captured lite"; the episode-end enumeration
    "(final exit or §A.2.8 W1 service; re-park = new
    episode)".

## Rev-20 findings and dispositions (fourth whole-design
## cross-check vs the composed six-spec system)

### F1 (BLOCKER, accepted): spawned-thread DropAllLocks no-op
### (section F.4) holds client heap access across embedder
### blocking sections - heap section 10.4 barrier / section
### A.3 stop reachable while holding rank-1 access
VERIFIED. Pre-r20 section F.4 ruled spawned DAL a pure no-op
returning 0, resolving INTEGRATE-api D1's open coexistence
question (INTEGRATE-api.md:834-847) and implicitly lifting
D1's phase-1 constraint ("the embedder must not run
DropAllLocks on the shared VM's JSLock while spawned Threads
are live"). But section-J.3 park sites are not the only
blockers: Bun host functions called from spawned-thread JS use
the landed DAL pattern around indefinitely-blocking native
work (the D1 livelock was OBSERVED from exactly this
interplay). A no-op bracket releases NOTHING - in particular
not the thread's GCClient heap access (only depth-0 token
unlock per section F.1, or section J.3, releases it). Composed
consequence: a spawned thread blocked indefinitely in native
code holds section-LK rank-1 heap access, never polls GSP
(heap F8) nor its lite stop bit (section A.3.2b); a shared
section-10 collection's step-4 access barrier (SPEC-heap.md:
269) and every section-A.3 conductor's predicate wait
unboundedly. Once other mutators are parked, any native call
whose completion depends on JS progress elsewhere
(pipe/channel fed by main or another Thread - ordinary Bun) is
a permanent process deadlock - a park reachable holding state
the tables forbid: heap section 9's contract note
(SPEC-heap.md:244) requires ALL indefinitely-blocking
primitives RHA/AHA-bracketed; section-LK row 1 makes access
the outermost rank. Neither section F.6's two embedder deltas
nor the IU checklist covered it; U14 codified the no-op
without the access consequence. DISPOSITION: ANNEX DAL2 -
spawned DAL becomes a heap-access bracket.

### ANNEX DAL2 (BINDING) - spawned-thread DropAllLocks =
### heap-access bracket (AMENDS section F.4's spawned arm;
### main/embedder arm and GIL-on stand)
1. Spawned-thread DAL GIL-off is NOT a pure no-op. Ctor:
   releaseClientHeapAccess() on the CURRENT lite's client (F8
   mandatory-revert, seq_cst exchange->NoAccess); returns 0;
   token, entry depth, m_lock, m_lockDropDepth ALL untouched
   (JSLock::currentThreadIsHoldingLock() stays mutex-literal
   false, JSLock.cpp:423-425). Dtor: re-acquire the SAME
   client's access section-A.3.2b/section-A.3.8-gated, then
   poll the lite's trap bits before returning to JS. Nesting:
   per-lite DAL depth counter; only the OUTERMOST bracket
   transitions access (inner = pure count). LIFO not required
   (no m_lockDropDepth participation) - the D1 livelock shape
   cannot recur.
2. Effect: an embedder blocking section on a spawned thread is
   access-released for the heap section-10.4 barrier AND
   counts for the section-A.3.2 conductor predicate; trap
   delivery is deferred to the dtor's poll (same shape as
   section F.5 nested-entry deferral).
3. Lock context precondition: DAL ctor/dtor are access
   transitions - per the section-E.2 lock/access rule they run
   holding NO api rank-1..3 lock and no heap 10a/10b lock.
   U20's lint covers DAL sites.
4. SUPERSESSION (both sides; IU row): INTEGRATE-api D1's
   phase-1 constraint (no DropAllLocks on the shared VM while
   spawned Threads are live, INTEGRATE-api.md:834-847) is
   LIFTED GIL-off for spawned threads by this bracket;
   main/embedder DAL keeps section F.4's main arm (m_lock +
   token drop). GIL-on keeps the constraint until the flip
   (GIL-on DAL on the shared VM remains forbidden with live
   spawned Threads - unchanged phase-1 text).
5. Embedder contract: section F.6 gains delta (c) - a
   spawned-thread blocking native section that uses NEITHER
   DAL NOR section J.3 must RHA/AHA-bracket per heap section 9
   (SPEC-heap.md:244). The IU embedder checklist enumerates
   Bun's DAL/blocking host-call sites (U-T8 row).
6. U14 re-derived: "spawned DAL = access bracket, token/depth
   invariant, returns 0". U24 corpus arm: spawned thread
   blocked in a DAL-bracketed native call while main conducts
   a shared GC AND a haveABadTime (section K.5) stop - assert
   both complete; release the native call, assert the thread
   resumes and observes deferred traps.

### F2 (MAJOR, accepted): section-K.3/LZ1 lazy-init park loop
### and may-allocate initializer have no lock-context
### precondition, audit column, or lint
VERIFIED. Every other GIL-off park/blocking edge carries an
explicit lock-context rule (section E.2's lock/access rule;
r17 F2's settle precondition; section J.3's
release-rank-3-first; annex W's W1; section LK.4b). Section
K.3 + ANNEX LZ1 define a park-capable foreign wait (access
release + dual stop-family poll) and a winner initializer that
may allocate/GC, but state nothing about the lock context of
the first-touch site. LazyProperty/LazyClassStructure/VM
ensure* first-touches are reachable from arbitrary runtime
paths; one reachable under a JSCellLock (10a), Structure::
m_lock (10b), an api rank-3 lock, or inside a section-N
cell-locked body makes the foreign waiter's access-release
park violate heap I6 and OM O2, and the winner's allocate/GC
violate OM O1. U-T8b enumerated MEMBERS, not touch contexts;
U20 had no rule for it; U26's arms all touch lock-free.
DISPOSITION: normative precondition in section K.3 (r17-F2
shape) + U-T8b touch-context column + U20 lint rules
(park-under-10a/10b; access-transition-under-10a/10b - the
latter also covers DAL2 item 3 and every section-E.2-rule
site).

### F3 (BLOCKER, accepted): access-released re-acquirers
### re-enter JIT code after a code-patching stop with no
### context-synchronizing barrier (jit F5/R1.d delivery is
### NVS-exit-only)
VERIFIED. jit F5 (SPEC-jit.md:156) + R1.d freeze the
cross-modifying-code protocol: data writes -> patcher flush ->
resume -> per-mutator ISB before re-entering JIT code, with
the ISB delivered exclusively on NVS exit (INTEGRATE-heap.md:
608: the didResume hook fires only in notifyVMStop). The
composed ungil design makes access-released-never-parked a
first-class mutator state for BOTH stop families (section
A.3.2 predicate; heap section 10 step 4), and a re-acquirer
whose stop bit cleared before its AHA proceeds straight into
JIT code with NO synchronizing event. Both stop kinds patch
code: section-A.3 windows run Class-A fires/jettisons (jit
section 5.3); the heap section-10 rebias stop fires TTL sets
and jettisons (ANNEX D1R item 2 relies on R1.d's NVS-exit ISB,
which never covers these threads). A thread that slept
access-released through the whole stop executes possibly-stale
instructions from patched/jettisoned regions on arm64 (and is
out of contract per the Intel SDM on x86 - a locked CAS is not
serializing for code modification). SPEC-ungil had no
ISB/membarrier treatment and no supersession of jit F5's
clause - master-rule violation or arm64 unsoundness. Inert in
phase 1 (GIL => <=1 JIT-executing mutator); live exactly at
GIL-off. DISPOSITION: ANNEX ISB1 - stop-generation
compare-and-ISB on every non-NVS may-execute-JIT transition.

### ANNEX ISB1 (BINDING) - non-NVS JIT re-entry context
### synchronization (extends jit F5/R1.d delivery; the
### patcher-side flush and NVS-exit ISB stand unchanged)
1. State: one process-wide seq_cst uint64 stop-generation
   counter (JSCConfig-adjacent, GIL-off only); EVERY
   section-A.3 conductor AND every heap section-10 conductor
   that patched/jettisoned code (Class-A fire, jettison, D1R
   rebias fire; cheap conservative form: every conductor)
   increments it INSIDE the window, before resume. Per-lite
   uint64 copy (L2 append).
2. Rule (normative, GIL-off): every transition into
   "may execute JIT code" that did NOT pass through an NVS
   exit - F8 AHA re-acquisition (incl. section-A.3.2b's
   bit-already-clear path, section J.3/E.2 wakes, DAL2 dtor,
   section-F.5 LIFO restore), section-F token acquisition, ACT
   - loads the global counter, compares the per-lite copy, and
   on mismatch executes a context-synchronizing instruction
   (arm64 ISB; x86-64 serializing instruction, e.g. cpuid or
   membarrier) BEFORE any JIT-code entry, then stores the new
   value. NVS exit keeps the unconditional R1.d ISB and ALSO
   refreshes the per-lite copy.
3. SUPERSESSION (both sides; IJ row; the frozen jit text
   stands unedited): jit F5's "world resume -> per-mutator ISB
   (R1.d) before re-entering JIT code" with NVS-exit-only
   delivery (SPEC-jit.md:156; INTEGRATE-heap.md:608) - the
   delivery SET is WIDENED to item 2's transitions; the
   protocol itself (data -> flush -> resume -> per-mutator
   sync) is unchanged. ANNEX D1R item 2's reliance re-cited to
   this annex.
4. Alternative recorded, NOT chosen v1: patcher-side
   process-wide membarrier(SYNC_CORE) after patching before
   resume (makes per-mutator ISB redundant for non-parked
   threads); rejected v1 for portability (no Windows/macOS
   twin in-tree) - revisit post-ungil if the compare cost
   shows in section B.5.
5. Cost: GIL-on/flag-off zero (counter never bumps; compare
   branch GIL-off-only paths). GIL-off steady state: one
   relaxed load + compare per access/token transition.
6. U-T5 arm (arm64 amplifier): conductor jettisons during a
   stop while a thread sleeps access-released through it; the
   sleeper re-enters via AHA and executes the patched region;
   TSAN/exec corpus asserts the new code runs. U20 lint:
   may-execute-JIT transitions missing the generation check.

### F4 (MAJOR, accepted): ScriptExecutable -> CodeBlock first
### creation/install racing has no named ruling
VERIFIED. Two threads first-calling the same shared function
race ScriptExecutable::prepareForExecution -> UnlinkedCodeBlock
link -> CodeBlock allocation -> installCode on the executable's
m_codeBlockForCall/m_jitCodeForCall (multi-word,
GIL-serialized today) + CodeBlockSet registration. jit section
5.7.2's tier-up CAS keys on an EXISTING CodeBlock; jit section
5.3 governs jettison/patching; section N named other cell
families but not executables, despite INTEGRATE-jit.md:295-304
assuming main-thread install. Leaving it to the U-T8c generic
audit risks discovering frozen-jit renegotiation at audit time
- the whole-design phase exists to pre-empt that. DISPOSITION:
ANNEX CBI - named section-N.8 ruling.

### ANNEX CBI (BINDING) - racing first CodeBlock
### creation/installation (section N.8 full text)
1. Compile fully OUTSIDE any cell lock (landed shape
   preserved): each racer may link its own CodeBlock from the
   UnlinkedCodeBlock (unlinked/bytecode side is immutable
   post-generation; UnlinkedCodeBlock generation itself is a
   section-K.3-class lazy publication on the executable -
   CAS-claimed, foreign waiters park per K.3 incl. its r20
   lock-context precondition).
2. Publication: release-CAS of the executable's
   m_codeBlockFor{Call,Construct} slot (single pointer word).
   Loser DISCARDS its CodeBlock (unreachable => GC-collected;
   no installCode side effects before winning) and ADOPTS the
   winner via load-acquire re-read. installCode's
   executable-side writes happen only on the winner.
3. Adjacent multi-word state: m_jitCodeFor{Call,Construct} +
   arity/numParameters mirrors are published by the SAME
   winner AFTER the CAS, each as single-word release stores
   ordered before a final "installed" flag the fast path
   acquires (or: all derived loads go through the
   codeBlock pointer - address-dependent, jit F2). Per-field
   table at U-T8c; any field not single-word-publishable is
   ruled under the executable's JSCellLock (10a, OM I20
   shape).
4. Dedup (optional, perf-only): per-executable in-flight claim
   CAS in the jit section-5.7.2 m_tierUpInFlight pattern;
   losers either compile anyway (item 2 arbitrates) or
   K.3-park; correctness never depends on it.
5. CodeBlockSet registration: under its existing heap-side
   lock (heap-owned; any thread). Debugger CodeBlock-wide
   walks = section A.2.7 (under a section-A.3 stop); jettison
   = jit section 5.3 - both already exclude racing installers
   via the stop (an installer parks/releases first).
6. Tier-up (existing CodeBlock) stays jit section 5.7.2
   verbatim; this annex governs FIRST install only. No frozen
   text superseded (jit is silent on first install;
   INTEGRATE-jit.md:295-304's main-thread-install note is an
   FTL-finalization fact, unchanged: optimizing-tier installs
   still occur on the owning mutator).
7. U-T8c named row + amplifier: two spawned threads
   first-call the same fn (LLInt-only and tiered variants);
   assert exactly one CodeBlock installed, loser adopts, no
   torn m_jitCodeFor* observation; TSAN clean.

### Rev-20 SD note
No new SDs; IDs stay frozen. F1 is embedder-facing GIL-off
liveness (DAL still returns 0; GIL-on unchanged); F2/F3/F4 are
GIL-off-only soundness/liveness with no JS-observable delta.

### Rev-20 section-T deltas (extends rev-9 annex 3 + r10-r19)
- U-T5: + ISB1 stop-generation counter + item-6 arm/lint.
- U-T8: + DAL2 (spawned DAL bracket; U14 re-derivation; U24
  DAL/GC/haveABadTime arm; F.6 delta (c) + Bun blocking-site
  IU enumeration).
- U-T8b: touch-context column per lazy member (r20 F2);
  offenders re-ruled into section-K class 1/2.
- U-T8c: + ANNEX CBI named row + first-call amplifier.
- U-T14: U20 lint gains park-under-10a/10b,
  access-transition-under-10a/10b, missing-generation-check
  rules.

### rev-20 spec deltas (byte budget)
Section F.4 rewritten (DAL2 index); section F.6 gains delta
(c); section K.3 gains the F2 precondition; section A.3 gains
rule 2c (ISB1 index); section N gains ruling 8 (CBI index); SD
note + section-T header lines updated; header rev 20.

## Rev-20 spec compressions (normative text preserved here;
## same convention as the rev-17/18/19 lists)
- section 0 U0c index re-tightened against annex U0C (BINDING,
  unchanged): dropped from the index only - "(pre-entry/
  codegen)"; "(noteSharedServerSticky stays loser-FATAL)";
  "WINNER: ... at clientSet()==1 (I13 UNCHANGED)" spelled
  detail; "Discharges section F.2 ISS-flip(a); section 10D
  never clears it; Heap.cpp:4755 no-ops" -> "F.2 ISS-flip(a)
  discharged; 10D/Heap.cpp:4755 per annex".
- section A.3.5 index re-tightened against HBT2+HBT3+HBT4
  (BINDING, unchanged): "the JSThreadsSafepoint.cpp:252-304
  bracket with GCL moved AFTER arbitration" -> ":252-304, GCL
  AFTER arbitration"; "conductor's own client" -> "own
  client"; class-4 step phrasing condensed ("takes the DEFAULT
  R1.i access-release, acquires GCL access-RELEASED (order per
  rule 3), re-acquires its OWN client via F8 AHA (non-blocking:
  GSP false under held GCL) BEFORE fanning stop bits; access
  then RETAINED + allocation legal in-window" -> annex-backed
  short form); "in-window GC initiation FORBIDDEN; slow paths
  ENQUEUE an RCAC ticket" -> "in-window GC FORBIDDEN; slow
  paths enqueue RCAC"; "NEVER collect" + "counter brackets it"
  shortened.
- section D.1 index re-tightened against annexes D1+D1R
  (BINDING, unchanged): "restamps dead TIDs to 0 AND (D1R)
  fires every restamped structure's TTL set inline in the SAME
  stop" -> "restamp dead TIDs->0 + (D1R) fire restamped TTL
  sets in-stop"; "jettisons jit section 5.5's baked tid<<48
  transition immediates BEFORE reissue" -> "jettison baked
  tid<<48 immediates pre-reissue"; "(lifts Dev 10)" kept;
  two-phase wording condensed.
- section N.5 lowering index re-tightened against r17 F5
  (BINDING, unchanged): "the landed INLINE sequence behind one
  not-taken branch (delta-(a) class)" -> "landed inline seq
  (delta-(a))"; "(contingency: R5-class inline LLInt CAS)"
  kept; "seq_cst 64-bit strongCAS on the field word (section
  B.5 premise) / release store" -> "seq_cst strongCAS (B.5
  premise) / release store".
- section C.1 arm index (r11 annex C1, BINDING, unchanged):
  "whole-probe restart, I33-bounded; completed RMW/CAS never
  re-applied" -> "I33-bounded restart, completed ops never
  re-applied".
- Further rev-20 index trims (byte budget for the r20 rulings),
  all either backed unchanged by the cited BINDING entries or
  rationale-only (rule unchanged):
  - section 0: U0b parenthetical "(U0c losers never reach it)";
    U0c index detail per the list above.
  - section A.1.3: "(U0c fixes it pre-codegen)"; the GC-roots
    sentence shortened against r6 F5 ("registry stable (heap
    section 10 quiesce)" lives there); "(nests in
    ifJSThreadsBranch regions)" on delta (a); "uniform
    builtin-bytecode intrinsic" -> "uniform intrinsic".
  - section A.1.6: "(buffer pre-exists)" (annex A16).
  - section A.2.5: the SignalSender/vmIsInactive rationale
    parenthetical (kept in the r-series history).
  - section A.2.7: spawned/main breakpoint corpus spelled
    expectations -> "corpus per annex".
  - section A.2.8: W1 old-node/early-exit detail, W2
    m_cpuDeadline clear, W3 "callback NOT consulted" (annex W
    + r15 F2 carry all three).
  - section A.3.6: r9-F4 title detail in the annex citation.
  - section A.3.8: "VMM-trap-delivery" + "shape" wording.
  - section B.1: "attached to the shared server".
  - section B.6: the Dev-7 item enumeration -> "(heap:26
    list)".
  - section C.4: "deletion narrowed to GIL-off per the oracle
    rule" (the oracle rule is section J/master-rule text).
  - section C.6: "(nodes GIL-correct)".
  - section D.2: "(both INTEGRATEs)".
  - section E.1: waitDeadlines element spelled type ->
    "deadline-ordered PWT waiters" (r12 entry carries it).
  - section E.1b: "no foreign MicrotaskQueue" (implied by
    I11/own-queue); E1B-backed I20 shape spelling; tracker
    site list :405/:464/:502/:637 -> :405-637.
  - section E.2: "A parked thread released access first -
    never delays a conductor" (restates the loop's
    release-before-wait line).
  - section E.3: per the r19-style list (annex E3 carries the
    m_keepaliveReleased name, asyncJoin/TA waitAsync
    parentheticals, settle/cancel spellings).
  - section E.4: api 5.5 :200 quoted text; "sound:" and
    "keepalive untouched" (annex r18 F2); settle-PRECONDITION
    citations unchanged.
  - section E.5: "(pending finite waitAsync settles
    timed-out)" - SD8 pointer carries it.
  - section E.7.1: member enumeration -> "+ peers".
  - section E.7.3: "(incl. E.4(b) retire)"; wake-contract
    phrasing (r17 F3/r18 F2 carry it). E.7.4: "(else a parked
    shell strands)". E.7.5: "hooks or not"; "r16 F5" close
    harvest pointer; "vm.runLoop() as landed" -> "landed".
  - section F.2: fixed-ruling parentheticals (annex F2).
  - section F.4: "token holders invisible" (DAL2 item 1);
    DAL2-backed spellings; U24 arm spelled -> "per annex".
  - section F.5: "(A's frames stay scannable, ...)" -> heap
    I4(b) citation kept.
  - section F.6: checklist phrasing.
  - section I: "(carriers keep 0)" + the TID-tag-rejection
    rationale "(carriers hold nonzero TIDs)"; wasm-GC
    "(landed T5b guard, :2795)" (r16 F4 entry carries it).
  - section K.3: LZ1 (a)/(b) index shortened against ANNEX
    LZ1 items 1-3 (side-table leaf lock, owner-null contract
    name, exception/termination enumeration, waiter re-test).
  - section K.5: HBT-backed enumeration + "(access retained
    post-GCL, may allocate, no in-window GC)" (HBT2/HBT3) +
    "(idempotent)".
  - section N.1: "(rehash/delete splice storage => UAF)"
    rationale. N.3: "(compute per call)". N.5: claim-failure
    "Serial semantics"; "(semantically the landed
    get+compare+put)"; "no JSCellLock". N.6: TRANSFER/SHRINK
    rev tags + resizable/maxByteLength + "(section I refuses
    EXECUTION only)" (annex N6 carries all).
  - section IM header: "rev-7..17" -> "rev-7..20".

# REV 21 (2026-06-06) - review round: fifth whole-design
# cross-check vs the composed six-spec system; 1 blocker + 1
# major, both VERIFIED REAL and UPHELD; no refutations

## Rev-21 findings and dispositions

### F1 (BLOCKER, accepted): a stop-window CONDUCTOR performing
### a section-K.3 lazy-init first-touch on a foreign in-flight
### init deadlocks holding GCL + the section-LK.4b job-slot
### mutex
VERIFIED. Composed wait-for cycle across section K.3 / K.5 /
A.3 + ANNEX LZ1 + heap section 10 that no spec breaks:
1. Thread O wins a LazyProperty/LazyClassStructure/VM-ensure*
   init CAS (section K.3). The winner contract lets the
   initializer allocate, so O can reach a cooperative poll
   site MID-INIT and PARK under an incoming section-A.3 stop
   (jit R1.f-g) - lazyTag stays "initializing", owner = O.
2. The stop's conductor C (a section-K.5 CLASS-4 peer such as
   haveABadTime, a section-A.2.7 debugger attach/recompile
   walk, or any section-A.3 closure) first-touches the SAME
   lazy member inside its window. C is FOREIGN to the init, so
   per section K.3/LZ1 it enters the park-capable wait loop
   (release access, dual stop-family poll, re-test).
3. O cannot resume until C resumes the world; C never exits
   the loop. LZ1.2's cycle escape does NOT fire: the
   owner-of -> waits-on chain ends at parked O and never
   reaches C (C owns no in-flight init). C waits indefinitely
   holding the JSThreadsStopScope GCL (heap rank 2) and the
   section-LK.4b pending-job-slot mutex - state the tables
   forbid waiting under (HBT4.3 "never wait on raw GCL"; heap
   section 10.2 assumes GCL holders complete) - so every
   future GC and every future stop in the process deadlocks.
4. Audit gap: the r20-F2 precondition and the U-T8b
   touch-context column enumerate only LOCK contexts (api
   rank 1-3, heap 10a/10b, section-N cell-lock,
   destructor-leaf); "inside a stop window as conductor" was
   absent, so both the audit and the U20 lint pass the path.
5. Symmetric hazard: if C instead WINS the CAS in-window, the
   section-K.3 winner contract (may allocate/GC) collides
   with section A.3.5(ii) NO-GC-IN-WINDOW - survivable only
   via HBT2/HBT3 fail-hard + LZ1.3 abandonment, i.e. a
   spec-legal first-touch becomes a spurious OOM.
Inert under the GIL (no foreign thread observes mid-init
state; no N-thread stops); live exactly at GIL-off.
DISPOSITION: third forbidden context added to the section-K.3
PRECONDITION; full normative text = ANNEX LZ2 below. The
rejected alternative - abandonment-CAS on a parked owner - is
recorded as UNSOUND in LZ2.3: O is mid-initializer and will
release-store on resume, republishing a possibly-partial
object over the reset tag.

### ANNEX LZ2 (BINDING) - section-K.3 conductor first-touch
### prohibition (extends r20 F2's precondition; LZ1 stands)
1. PROHIBITION. No section-K.3 first-touch (winner OR foreign)
   may execute from inside a section-A.3 or heap section-10
   stop window while the executing thread is acting as
   CONDUCTOR (incl. the section-A.3.5 CLASS-4 variant, the
   section-A.2.7 walk, and D1R in-stop fires). Foreign-touch
   consequence: park-capable wait while holding GCL + the
   section-LK.4b slot mutex against an owner parked under the
   conductor's own stop - unbounded, unescapable (LZ1.2's
   walk terminates at the parked owner). Winner-touch
   consequence: section-A.3.5(ii) violation or HBT2/HBT3
   fail-hard spurious OOM.
2. DISCHARGE. Every lazy member reachable from a conductor
   closure (CLASS-4 body, section-A.2.7 walk, D1R fire) must
   be (a) proven pre-initialized at every call site that can
   become a conductor, or (b) pre-resolved by the conductor
   BEFORE arbitration - i.e. before acquiring the
   section-LK.4b slot mutex, while still an ordinary mutator
   able to win/wait per section K.3 - or (c) the member is
   re-ruled section-K class 1 (per-lite) or class 2 (leaf
   lock). U-T8b's touch-context table gains a
   conductor-closure-reachable column recording which of
   (a)/(b)/(c) each such member uses.
3. NON-FIX (normative rejection): a conductor MUST NOT
   abandonment-CAS (LZ1.3) a parked owner's in-flight init.
   LZ1.3 is owner-unwind-only; a foreign reset races the
   owner's release-store on resume and republishes partial
   state. Any future scheme that cancels a foreign in-flight
   init requires a new negotiated annex.
4. U26 arm (f): owner parks mid-init at a poll site under an
   incoming stop; a CLASS-4 conductor whose closure would
   touch the same member runs; assert no hang (the member was
   pre-resolved per LZ2.2(b) before arbitration) and the
   owner's init completes post-resume with a single
   publication.
5. U20 lint: flag any section-K.3 touch site dominated by
   Heap::JSThreadsStopScope construction or section-LK.4b
   slot-mutex acquisition (conservative interprocedural
   domination over the conductor entry points named in
   LZ2.1); offenders must carry an LZ2.2 disposition.

### F2 (MAJOR, accepted): the section-F.6(b) embedder
### continuation-affinity disposition is the likeliest
### renegotiation trigger but was gated at U-T14 close, after
### section E is built
VERIFIED. Section F.6 lands three normative deltas on Bun;
item (b) - main/embedder-registered ordinary-promise reactions
run on the SETTLING spawned thread (SD10, section E.1b.1), off
m_lock and off the embedder loop - is the one most likely to
be rejected: Bun's async stack assumes loop affinity for
main-global continuations (uv handles, JSC API calls from
continuations). The spec itself says a carrier-hop demand = a
NEW negotiated SD and rejected the per-reaction registrant hop
for v1 (section E.1b.1). A late carrier-hop SD reshapes
section E.1b, section-E.3 keepalive (reaction targets would
need keepalive), section-E.4 routing, and the U-T9 corpus -
yet embedder sign-off was scheduled as a U-T14 close item,
after U-T9 has implemented and tested section E. Section D.2
already re-times the Task-14 verdict to a HARD precondition of
U-T10 ENTRY for exactly this reason; the same logic applies.
DISPOSITION: section-F.6 sign-off SPLIT - item (b)'s SD10
continuation-affinity disposition re-timed to a HARD
precondition of U-T9 ENTRY (section-D.2 shape; recorded in the
section-T index + deps); items (a) and (c) stay U-T14 close
items. If the disposition demands a carrier hop, the new SD is
negotiated and section E.1b/E.3/E.4 re-reviewed BEFORE U-T9
code or corpus lands. No SD today; SD10 text unchanged.

### Rev-21 SD note
No new SDs; IDs stay frozen. F1 is GIL-off-only liveness
(no JS-observable delta); F2 is schedule-only.

### Rev-21 section-T deltas (extends rev-9 annex 3 + r10-r20)
- U-T8b: touch-context table + conductor-closure-reachable
  column (LZ2.2 dispositions (a)/(b)/(c)); offenders re-ruled
  section-K class 1/2.
- U-T9: ENTRY GATE added - the section-F.6(b) SD10
  continuation-affinity disposition must be signed off before
  U-T9 entry (r21 F2). Deps line gains "F.6(b) disposition
  gates T9".
- U-T13: section-K.5 conductor closures verified LZ2-clean
  (pre-resolution sites land with the class-4 work).
- U-T14: U20 lint gains the LZ2.5 dominated-touch rule; U26
  gains arm (f) (LZ2.4); F.6 (a)/(c) sign-off stays here.

### rev-21 spec deltas (byte budget)
Section K.3 precondition extended (LZ2 index); section F.6
sign-off split; section-T index U-T9 entry gate + deps;
header rev 21.

## Rev-21 spec compressions (normative text preserved here;
## same convention as the rev-17..20 lists)
- section K.3: LZ1 (a)/(b) index shortened - dropped "wait-for
  edges in the owner side table", "instead of parking",
  "winner unwind scope ... on any non-normal exit BEFORE
  propagating", "no owned in-flight inits" spellings (ANNEX
  LZ1 items 1-3 carry the full text).
- section A.3.2c: the bypassed-NVS-exit transition enumeration
  "(F8 AHA incl. 2b's bit-clear path and wakes, section-F.4
  dtor, section-F token acquire, ACT)" -> "(set per ISB1)"
  (ANNEX ISB1 carries the set).
- section B.5: "r9 async/generator microbench per section N.5
  (r17 F5); r10 --useJIT=0 re-run in-noise" -> "r9/r10
  microbench notes: history" (the r9/r17-F5 BENCH gate text
  stays normative in section N.5; the r10 in-noise result is
  this line).
- section C.1: flat-arm "completed ops never re-applied"
  spelling (annex C1 carries it; I33-bounded restart implies
  it).
- section F.2: fixed-ruling site list "sanitizeStackForVM,
  primitiveGigacageDisabled, validateIsNotSweeping,
  ISS-flip(a)=U0c, DWT=section-E.7.2, WeakSet::allocate" ->
  "(six named sites)" (annex F2 carries the names + rulings).
- section F.6: r21-F2 rationale parenthetical "(a late
  carrier-hop SD reshapes E.1b/E.3/E.4 + the U-T9 corpus)"
  lives in the F2 entry above.
- section N.5: claim-failure dispatch spelling "Executing =>
  EXISTING already-running TypeError; Completed => the LANDED
  completed path (GeneratorPrototype.js:35); another
  SuspendedX => retry (legal serialization)" -> "(r12:
  TypeError / landed completed path / retry; no SD)" (the r12
  entry carries it); "(r15 F1: plain publish => arm64-torn
  frames)" -> "(r15 F1)".
- section N.8: "(per annex; INTEGRATE-jit:295-304 noted
  both-sides)" -> "(per annex)"; amplifier spelling "(LLInt +
  tiered; exactly-one installed, no torn state)" -> "per
  annex" (ANNEX CBI item 7 carries both).

## Rev-22 findings and dispositions (fifth whole-design
## cross-check vs the composed six-spec system)

### W1 (MAJOR, accepted): section-LK negative edges falsified by
### the landed Thread.restrict affinity machinery - api rank-2
### lock wraps MSPL via Weak<> creation; GC-side weak-finalizer
### pruning acquires an api lock from a conductor
VERIFIED against the tree, both arms:
1. Creation edge (api rank 2 -> heap rank 7).
   ThreadManager::restrictObject (ThreadManager.cpp:259-280)
   and its stale-replace arm call makeAffinityEntry (:234-243)
   while holding m_affinityLock - the ThreadAffinityTable lock,
   section-LK api rank 2 (SPEC-api:246/:261). makeAffinityEntry
   constructs Weak<JSObject>; Weak construction calls
   WeakSet::allocate, which under ISS takes
   MutatorSlowPathLocker (heap rank 7) - the landed SharedGC
   round-4 lock at WeakSetInlines.h:66-73. U0 makes GIL-off
   imply ISS, so EVERY GIL-off Thread.restrict insert takes
   MSPL inside an api rank-2 lock, violating section-LK.5 "api
   locks NEVER wrap heap ranks 2-9b". Same pattern one tier
   down: RegExpCache::lookupOrCreate (RegExpCache.cpp:62-65)
   constructs Weak<RegExp> (-> MSPL) under m_lock, which
   section K.2 / LK.7 classify as a LEAF. The in-tree comment
   at WeakSetInlines.h ("callers hold no rank >= 7 lock") only
   asserts the heap-side order and never saw the api edge.
2. Finalizer edge (conductor -> api rank 2 / class-2 leaf).
   ThreadManager::pruneRestrictedObject (:282-296) runs from
   ThreadAffinityWeakHandleOwner::finalize (:192-202), i.e.
   from WeakBlock::sweep (WeakBlock.cpp:88-90) - conducted weak
   sweep in the heap section-10 stop window or
   lastChanceToFinalize - and acquires the rank-2 affinity
   lock; RegExpCache::finalize (RegExpCache.cpp:75-80)
   re-acquires its class-2 leaf from the same context. Both
   violate the negative edge "GC/section-A.3 conductors acquire
   NO api lock". The ThreadManager.cpp:186-191 comment claims
   5.9-legality - true GIL-on, falsified by section LK's
   conductor edge GIL-off.
3. Why the audits missed it: the r11 F2 round examined
   WeakSet::allocate/deallocate THEMSELVES (the section-F.2
   fixed ruling + the r11 refutation) but never the lock
   CONTEXT of Weak creation, nor WeakHandleOwner::finalize
   bodies; SPEC-api 5.7.2 predates section LK and was never
   reconciled; the r8 acyclicity tree-walk enumerated listLock/
   notifier edges only.
4. NO actual deadlock cycle exists in the landed code today:
   the reverse MSPL -> affinity/cache-lock edge cannot arise
   from mutators because mutator in-lock sweeps SKIP
   weak-bearing blocks (the WeakSet.h:121-131 carve-out), and
   the affinity/cache critical sections are poll-free and never
   release access, so the F8/section-10.4 access barrier drains
   them before any conducted weak sweep runs finalizers. But
   the spec as WRITTEN is inconsistent: U20's section-LK lint
   would flag frozen api-owned code, or - loosened silently -
   the acyclicity argument loses the exact edges it relies on.
DISPOSITION: BOTH repair arms adopted (the finding's option (a)
plus the conductor exception row): the creation edge is
ELIMINATED by a normative code-shape rule (hoist Weak/entry
construction outside the lock) so the rank-table negative edge
stays literally true; the finalizer edge is SANCTIONED by an
explicit carve-out row with the recorded soundness argument.
Spec text: section-LK "WS rows"; full normative text = ANNEX
WS1 below. SUPERSESSION recorded vs SPEC-api 5.7.2's landed
shape (both sides cited). GIL-on behavior unchanged (same
publication order; construction outside the lock is legal under
the GIL trivially).

### ANNEX WS1 (BINDING) - Weak-creation lock discipline + the
### finalize-side conductor carve-out (section LK "WS rows")
1. PROHIBITION (WS(i)). Weak handle CREATION
   (WeakSet::allocate; any Weak<T>/JSWeakValue construction
   reaches it) acquires MSPL (heap rank 7) whenever the server
   is shared (ISS; WeakSetInlines.h:66-73), and GIL-off implies
   ISS (U0). Therefore no thread may construct a Weak while
   holding ANY api rank-1..3 lock or section-LK.7 leaf
   (class-2 cache locks included). Strong creation
   (HandleSet::m_strongLock, fastMalloc HandleBlocks - no
   MSPL) is NOT prohibited, but the U-T8b column (item 4)
   records its lock context anyway.
2. CODE SHAPE (the SUPERSESSION vs api 5.7.2's landed shape;
   IU rows IU-WS1a ThreadManager.cpp, IU-WS1b
   RegExpCache.cpp + any class-2 weakAdd peer the U-T8b audit
   finds). ThreadManager::restrictObject: construct the
   ThreadAffinityEntry (with its Weak + finalizer context)
   BEFORE taking m_affinityLock; under the lock, ensure() into
   the table by MOVING the pre-built entry (fresh-insert arm)
   or REPLACING a stale entry with it (stale-replace arm:
   swap out the old entry under the lock, destroy it AFTER
   release); on the lose arm (live entry for this object
   already present) destroy the pre-built entry after release.
   Entry destruction only WeakSet::deallocate's lock-free
   clear (WeakSet.h:121-131) + fastMalloc free - legal in
   either position; destroying OUTSIDE keeps the section pure
   HashMap + fastMalloc. The makeAffinityEntry comment's
   "created under the GIL" rationale is superseded.
   RegExpCache::lookupOrCreate: construct Weak<RegExp> before
   the second Locker; weakAdd under it; a racing winner's
   duplicate Weak is discarded after release (lookup re-check
   under the lock decides). Pattern generalizes: NO Weak
   construction inside any api rank-1..3 or class-2/leaf
   section, ever; build outside, publish under.
3. CONDUCTOR CARVE-OUT (WS(ii)) - amends the section-LK
   negative edge "GC/section-A.3 conductors acquire NO api
   lock" and the r8 acyclicity derivation (both sides: r8
   item 3 / the section-LK edge list vs ThreadManager.cpp:
   186-202 + RegExpCache.cpp:75-80). WeakHandleOwner::finalize
   bodies MAY acquire the ThreadAffinityTable lock (rank 2)
   and class-2 cache leaves in-window (conducted weak sweep,
   lastChanceToFinalize). Soundness, recorded: (a) holders of
   those locks are poll-free, access-retaining, never park and
   never wait (post-WS1.2 the sections are HashMap +
   fastMalloc only), so the heap section-10.4 / F8 access
   barrier guarantees no thread is parked or stopped HOLDING
   one - the conductor can always acquire in bounded time;
   (b) the reverse edge (MSPL -> these locks) no longer exists
   after WS1.2, and mutator in-lock sweeps skip weak-bearing
   blocks (WeakSet.h:121-131), so finalize bodies never run
   under a mutator's MSPL - acyclicity restored BY
   CONSTRUCTION, not by silent exception. (c) The carve-out is
   CLOSED: exactly WeakHandleOwner::finalize-driven table
   pruning (pruneRestrictedObject, RegExpCache::finalize +
   audited peers); TM::m_lock (rank 1) is NOT excepted -
   section D.1's two-phase snapshot stands; any new
   finalize-side lock needs a new row here.
4. AUDIT + LINT. U-T8b gains a handle-creation lock-context
   column: every Weak/Strong construction site found by the
   section-K audit records the locks held; Weak-under-api/leaf
   = WS1.1 violation (re-shape per WS1.2). U20 lints: (i)
   WeakSet::allocate reachable while an api rank-1..3 or
   section-LK.7 lock is held (static path or debug-assert
   instrumentation); (ii) any api/leaf lock acquisition inside
   a WeakHandleOwner::finalize body that is not on the WS1.3
   row list. Debug builds: RELEASE_ASSERT hook in
   WeakSet::allocate checking a per-thread "in api-rank-1..3 /
   leaf section" counter (cheap thread-local increment in
   Locker sites named by the IU rows).
5. Corpus: restrict/collect churn arm - N threads
   Thread.restrict + dead-object storms forcing finalizer
   pruning during conducted sweeps; regexp-cache churn arm
   (distinct patterns, GC pressure) - both TSAN'd, gates with
   U-T8b (the audit task carries the re-shape diffs).

### rev-22 spec deltas (byte budget)
Header rev 22; section-LK negative-edges line gains the WS(ii)
pointer; section-LK gains the WS rows; section-LK table header
acyclicity citation now "r8 as amended by r22 WS1".
Compressions below pay for it.

## Rev-22 spec compressions (normative text preserved here;
## same convention as the rev-17..21 lists)
- section 0 U0b: "Corpus arms + IU row per history (r22 list)"
  was "Corpus: second-VM spawn refused; two-embedder entry
  EXECUTES JS beside the shared VM. IU row."
- section C.1: "GROW per annex" was "GROW = butterfly-CAS +
  copy, NO nuke" (ANNEX C1 carries it).
- section C.4: ":536-541 NOT 4.5-1a: G11 property-wait gate
  KEPT, re-pointed section G.2" was "ThreadAtomics.cpp:536-541
  is NOT 4.5-1a, NOT deleted: the G11 embedder gate on property
  Atomics.wait KEPT, re-pointed at mayBlockSynchronously()
  (section G.2)."
- section E intro: "Ground truth replaced (api 4.6.1 GPO drain;
  DWT settlement)" was "Ground truth replaced: one completion
  drain (api 4.6.1 GPO); all settlement via
  vm.deferredWorkTimer."
- section E.1 inboxOpen: "post-section-B.1 attach, BEFORE fn
  (HB vs any registration)" was "after lite registration +
  GCClient attach (section B.1), BEFORE fn - happens-before any
  registration vs this TS"; "Main/embedder NEVER open theirs"
  dropped "(E.4 main path)".
- section E.1 host hook: "carrier enqueues; spawned enqueues
  ALWAYS per-lite" was "main/embedder-carrier enqueues; spawned
  enqueues ALWAYS per-lite (hook or not)".
- section E.1 task queue: "waitDeadlines (r12, sections
  C.3/E.7.5)" was "waitDeadlines (r12: deadline-ordered PWT
  waiters, sections C.3/E.7.5)".
- section E.1b.4: "inline for carriers only; spawned events
  append Strong+op records (no JS) to the annex-E7 handoff
  queue, run at" was "fires INLINE only for main/embedder
  carriers; spawned Reject/Handle events append {promise
  Strong, op} records (no JS) to the annex-E7 m_pendingLock
  handoff queue, EXECUTED at".
- section E.2 rank-4 exemption: "NLS::m_lock/ParkingLot MAY
  span token+access (re)acquisition - block ONLY while both
  RELEASED, then (re)acquire gated holding m_lock" was
  "NLS::m_lock/ParkingLot internals MAY be held across token +
  access (re)acquisition - block/quanta-loop on m_lock ONLY
  while both RELEASED, then (re)acquire gated (sections
  A.3.2b/A.3.8) holding m_lock".
- section F.4: "(JSLock.cpp:423-425)" was "(mutex-literal
  predicate, JSLock.cpp:423-425)".
- section F.6 (b): "embedder-REGISTERED ... off m_lock/loop"
  was "main/embedder-REGISTERED ordinary-promise reactions
  settled by a spawned thread run on the settler ... - off
  m_lock, off the embedder loop".
- section F.6 IU row: "(JSLockHolder audit;
  continuation-affinity disposition - carrier-hop demand = NEW
  negotiated SD; blocking-site enumeration, U-T8)" was
  "(JSLockHolder exclusivity audit; continuation
  thread-affinity disposition - a carrier-hop demand = a NEW
  negotiated SD; Bun blocking-site enumeration, U-T8)".
- section K.3 foreign wait: "(release access, DUAL stop-family
  poll, re-acquire gated, re-test ...)" was "release access,
  poll BOTH stop families (section-A.3 bit AND heap section-10
  state - one alone deadlocks), re-acquire gated, re-test"
  (ANNEX LZ1 carries the full loop).
- section K.3 conductor precondition: "(r21 F1, FULL text +
  rationale history ANNEX LZ2, BINDING)" was "(r21 F1, FULL
  text history ANNEX LZ2, BINDING - foreign wait parks forever
  holding GCL + the section-LK.4b slot mutex, owner parked
  under the conductor's OWN stop, LZ1.2 never fires; winner
  collides with section A.3.5(ii))" (LZ2.1 carries it).
- section LK long-hold: "api section-5.9 rank-4 leaf + (f)"
  was "... + (f) \"Ranks not swapped\""; "5.9(e)/(f) = the
  leaf-form encoding of this order; section LK canonical for
  U20" was "5.9 (e)/(f) ARE the leaf-form encoding of this
  order; section LK is the both-modes canonical form for U20's
  lint".
- section LK.4b: "held across the stop window" was "held
  across the whole stop window".
- section N.5: "Primitive (r11; claim sites per annex + r22
  list)" was "Primitive (r11; claim sites = builtin JS,
  GeneratorPrototype.js:36/:45; async/iterator-helper resumes
  ride the same machinery)".
- section I: "the WebAssembly ctor/compile surface throws on a
  spawned TS (full list: r22 list)" was
  "WebAssembly.{compile,instantiate,validate} +
  Module/Instance/Memory/Table/Tag/Global ctors throw on a
  spawned TS". Wasm-GC sentence: "hasGCObjectTypes() precheck
  => LinkError (compile-side CompileError), both GIL modes -
  SUPERSESSION (heap section-5.5/manifest 11, both sides ...)"
  was "Wasm-GC (SUPERSESSION vs heap section-5.5/manifest 11's
  RELEASE_ASSERT, JSWebAssemblyInstance.cpp:142, both sides;
  section-5.5 never-populate STANDS; history r9 F8): under
  useJSThreads, both GIL modes, hasGCObjectTypes() prechecked
  BEFORE instance construction => WebAssembly.LinkError
  (compile-side: CompileError); the assert stays on
  non-JS-reachable paths."

## Rev-23 findings and dispositions (sixth whole-design
## cross-check vs the composed six-spec system)

### F1 (MAJOR, accepted): GIL-off drops the heap section-10A.1
### currentThreadClient TLS re-stamp with no replacement - stale
### client slot on multi-VM / nested embedder entry

Verified: SPEC-heap.md:283 (section 10A.1) keeps the per-thread
GCClient::Heap* TLS slot correct across VM switches via "once
ISS, JSLock::didAcquireLock's forwarding re-stamps it before AHA
(migration-safe)"; the slot is otherwise set ONLY by ACT and
cleared by DCT. Section B.3 supersedes the forwarding GIL-off
(JSLock pair acquires/releases on the CURRENT carrier's OWN
client, never the main client), and the r21 F4 disposition
explicitly scopes the surviving re-stamp to GIL-on/flag-off. But
no GIL-off text reassigned the re-stamp duty: ANNEX F1B makes
every lock() run the gated AHA on the carrier's client - AHA
re-stamps only the debug m_accessOwner field, not the 10A.1
slot; ANNEX A36 defines the LIFO-restored TLS tuple as {lite,
tag} only; section F.5's nested restore "re-acquires A's access"
without touching the slot. ACT runs only at FIRST entry
(carrier/client creation), so a thread that enters VM B after
(or nested inside, section F.5) VM A returns to A with
currentThreadClient() still pointing at B's client - the exact
supported U0b mixed-mode configuration (one m_gilOff VM +
GIL-on second VM, multi-embedder entry). Misrouted consumers:
heap CIND/SINFAC/CSAC "find the caller's client", per-client
DeferGC depth (heap section 5.4/I17),
currentThreadIsAllocatorOwner's ISS predicate (la in the
TLS-client's m_perDirectory), section A.3.8's
willPark/didResume per-client m_releasedByGCPark - against a
different client and potentially a different server heap. U1
asserted only tag==lite TID && lite->vm==entered VM; no
U-T6/U27 arm covered the slot. Fix: ANNEX A36C below; spec
section A.3.6/B.3/F.5 index edits; U1 extended; U-T6/U27 arms.

### ANNEX A36C (BINDING) - section A.3.6 carrier-swap section
### 10A.1 client-slot re-stamp (extends annexes A36 + F1B;
### closes the GIL-off re-stamp gap left by the section B.3
### supersession + the r21 F4 GIL-on-only scoping)

1. The section A.3.6 swapped TLS state is the TUPLE {lite,
 TID-tag, heap section 10A.1 currentThreadClient slot} - NOT
 {lite, tag}. EVERY carrier install (first entry, every
 lock(), section B.1 spawned attach) AND every LIFO restore
 (depth-0 unlock, section F.5 nested exit) re-stamps
 currentThreadClient() to the now-current lite's clientHeap,
 through A36's {client, epoch} staleness check (stale epoch =>
 stamp null, never a dangling client); restoring to "no lite"
 clears the slot. The stamp precedes any allocation/OM fast
 path AND the section F.1 gated AHA - preserving heap section
 10A.1's ordering (slot correct before AHA). Spawned threads:
 ACT's stamp at section B.1 attach is already correct and
 unique (single-VM, v1); the rule is vacuous after attach.
2. SUPERSESSION EXTENSION (heap section 10A.1 "once ISS,
 JSLock::didAcquireLock's forwarding re-stamps it before AHA"
 clause, SPEC-heap.md:283, vs section B.3 + this annex, both
 sides; IH row): GIL-off, the re-stamp duty is THIS annex's
 tuple swap (carrier install + LIFO restore sites);
 GIL-on/flag-off forwarding + re-stamp UNCHANGED (the r21 F4
 scoping stands). Extends the section B.3 supersession - one
 IH row covers both clauses of SPEC-heap.md:281-283.
3. Verification. U1 EXTENDED (ID frozen): whenever a thread
 holds an entry token, TLS tag == CURRENT lite TID && lite->vm
 == entered VM && currentThreadClient() == lite->clientHeap
 (checked at the section J.7 backstop + token
 acquisition/release in debug). U-T6 + U27 gain: (a) a two-VM
 alternating-entry arm - embedder thread enters the m_gilOff
 VM A, exits, enters GIL-on VM B, exits, re-enters A, then
 allocates + DeferGC + triggers CIND (asserts route to A's
 client); (b) a section F.5 nested arm - A -> nested B ->
 LIFO-restore A -> allocate (slot re-stamped at restore, not
 left at B's client). Race-amplifier hook at the restore-side
 re-stamp.

### Rev-23 SD note
No new SDs. A36C is internal correctness (client routing);
GIL-on/flag-off observable behavior unchanged.

### Rev-23 section-T deltas (extends rev-9 annex 3 + r10-r22)
U-T1 (section A.3.6 swap): tuple widened per A36C. U-T6/U27:
arms (a)/(b) above. U-T8 IU table: section 10A.1-slot consumers
note A36C as their GIL-off stamping authority.

### rev-23 spec deltas (byte budget)
Section A.3.6 index rewritten ({lite, tag, client} tuple + A36C
pointer + U1 extension); section B.3 gains the supersession-
extension sentence; section F.5 restore sentence gains the
A36C re-stamp; header rev 23; SD/IM/T rev ranges bumped.

## Rev-23 spec compressions (normative text preserved here;
## same convention as the rev-17..22 lists)
- section A.1.3 delta (a): "(a) one not-taken gilOffProcess
  branch per LLInt Group-3 site" was "... per LLInt Group-3
  site (nests in ifJSThreadsBranch regions)".
- section A.1.7: "(i) resolves the TARGET lite via the registry
  (locked, target suspended)" was "... via the registry (under
  its lock, target suspended)".
- section A.2.6: "Replacement: TA + section C.3 sync parks wait
  in D9 10ms quanta" was "Replacement: TA + section C.3
  property sync parks wait in D9 10ms quanta".
- section A.2.8 W1: "W1 parked carrier reacquires EARLY,
  services shouldTerminate, then r15 F2 old-node disposition"
  was "W1 parked carrier reacquires EARLY (full section J.3
  exit), services shouldTerminate, then r15 F2 old-node
  disposition under listLock" (annex W carries the full text).
- section A.3.2c: "on mismatch runs a context-sync instruction
  (arm64 ISB; x86 serializing) BEFORE JIT entry" was "on
  mismatch runs a context-synchronizing instruction (arm64
  ISB; x86 serializing op) BEFORE JIT entry".
- section A.3.8: "(Mode keys on all parked/released/
  not-entered)" was "(Mode transitions key on all parked/
  released/not-entered)".
- section B.5: "4-thread alloc microbench >=2.5x recorded, not
  gated (r9/r10 notes: history)" was "4-thread alloc
  microbench >=2.5x recorded, not gated. r9/r10 microbench
  notes: history.".
- section C.1: "FULL text: history annex C1 (BINDING). Index:"
  was "FULL arm text: history r11 annex C1 (BINDING). Arm
  index:".
- section C.4: "Post-lift blocking = section G-only." was
  "Post-lift blocking is section G-only (deadlock = user
  error)." (the deadlock-is-user-error ruling stands, recorded
  here).
- section D.2: "Task 14 (om:378)" was "Task 14 (structure
  splitting, om:378)".
- section E.7 intro: "m_pendingTickets is JSLock-serialized
  today, NO lock." was "m_pendingTickets is an
  UncheckedKeyHashSet with NO lock, JSLock-serialized today.".
- annex-citation rev-number drops (annex names are unique in
  this file; the introducing rev is recoverable from each
  annex's own heading): "history annex U0C" was "history r13
  annex U0C"; "history annex A36" was "history r11 annex A36";
  "history annex F1B" was "history r16 annex F1B"; "history
  annex E1B" was "history r16 annex E1B"; "history annex E3"
  was "history r16 annex E3"; "history annexes D1 + D1R" was
  "history r16 annex D1 + r18 annex D1R"; "history annex N6"
  was "history r13 annex N6"; "history annex A16" was "history
  r11 annex A16"; "history r9 F1 + annex C3" was "FULL
  NORMATIVE text: history r9 F1 + r11 annex C3"; "history r8
  annex + annex E7" was "FULL NORMATIVE mechanics: history r8
  annex + r11 annex E7".
- section A.1.6: "every baked scratch ADDRESS" was "every
  baked DFG/FTL scratch ADDRESS".
- section A.2.2: "vmstate section-2 r3 preserved" was "vmstate
  section-2 rule 3 preserved".
- section A.2.8 heading cite: "annex W (BINDING) + r15 F2."
  was "annex W (BINDING) + r15 F2; r13 F2/r14 F1." (the r13
  F2/r14 F1 lineage lives in annex W).
- section F.4: "Embedder blocking sections satisfy" was
  "Embedder blocking sections thus satisfy".
- section K.3: "(per-VM side table/spare bits; r16 F2)" was
  "(per-VM side table under a leaf lock, or spare bits; r16
  F2)" - the side table IS under a leaf lock (ANNEX LZ1
  carries it); "BEFORE arbitration (pre-slot-mutex)" was
  "BEFORE arbitration (before the slot mutex)".
- section N.5: "@atomicInternalFieldClaim(cell,index,expected,
  replacement) -> bool" was "...(cell, index, expected,
  replacement) -> boolean".
- section LK header: "acyclicity: history r8 + r22 WS1" was
  "acyclicity: history r8 as amended by r22 WS1".
- section LK.4b: "(HBT4.4)" was "(r19 HBT4 item 4)".

## rev 24 (2026-06-06) - whole-design cross-check vs the
## COMPOSED six-spec system: 4 findings (2 blocker, 2 major),
## all accepted

### F1 (BLOCKER, accepted): slot-mutex "never held with any api
### lock" (LK.4b/HBT4.4) forbids every JS-reachable conductor
### inside lock.hold(fn)
VERIFIED. lock.hold(fn) runs arbitrary user JS while the thread
holds NLS::m_lock (section-LK long-hold row; api section 5.3/5.9
rank 4 - an api-OWNED lock). That JS can reach
mandatory-synchronous stop conductors: (a) any Class-A
watchpoint fire - jit section 5.6 routes ALL such fires through
STWR with synchronous completion load-bearing, and its caller
precondition (SPEC-jit.md:219 "entered mutator, NO section-7/
cell lock") does NOT exclude NLS::m_lock; (b)
JSGlobalObject::haveABadTime (section K.5; JS-reachable per
:2460; conductor = caller); (c) OM per-event stops (om sections
4.6/4.7/F3). Every section-A.3 conductor MUST take the
pending-job-slot mutex first (HBT4.1 order pin), but
section-LK.4b and binding ANNEX HBT4 item 4 both said the slot
mutex is "never held together with any api lock". Read strictly,
a thread inside hold(fn) can neither take the slot mutex nor
release NLS::m_lock (the user owns the hold; a Class-A fire
cannot be deferred) - the composed system had no legal execution
for everyday JS inside hold(fn). Read leniently ("api lock" =
api ranks 1-3 only) the merged graph IS acyclic - but that edge
was recorded nowhere; the section-LK long-hold acyclicity note
covered only conductors ACQUIRING NLS, not conducting while
already HOLDING it; neither HBT/HBT2-4 nor jit section 5.6
adjudicated the hold(fn) caller context; greps confirm no rev
1-23 ruling on conduct-while-holding-NLS. DISPOSITION: ANNEX
NLH1 below; section LK.4b + the long-hold row amended (lenient
reading made normative, edge recorded).

### ANNEX NLH1 (BINDING) - conduct-while-HOLDING-NLS ruling
### (AMENDS ANNEX HBT4 item 4; HBT4 items 1-3/5-6 and the
### section-LK negative edges stand)
1. Definition: in section LK.4b and HBT4 item 4, "api lock"
   means api RANKS 1-3 (TM::m_lock; PWT/affinity; the rank-3
   group) - it explicitly EXCLUDES the long-hold NLS::m_lock
   class. The slot mutex is never held together with any api
   rank-1..3 lock; it MAY be taken while NLS::m_lock is already
   held.
2. New recorded edge: NLS::m_lock (long-hold) > section-A.3.3
   pending-job-slot mutex > GCL (heap rank 2).
3. Soundness/acyclicity: every NLS::m_lock WAITER blocks
   token+access-released (section-E.2 rank-4 exemption, api
   5.9(e); r8 fix 1), so the conductor's section-A.3.2 barrier
   and the heap section-10.4 barrier are INDEPENDENT of the NLS
   holder - they never wait on it; and no conductor or heap
   2..9b holder ACQUIRES NLS::m_lock (long-hold row, unchanged).
   Conductors may therefore HOLD NLS on entry - lock.hold(fn) JS
   reaching a Class-A fire (jit section 5.6 STWR),
   haveABadTime (section K.5 CLASS-4), or an OM per-event stop
   (om 4.6/4.7/F3) - but never ACQUIRE it, so the new edge is
   one-directional and the merged order stays acyclic (r8/r22
   WS1 arguments unaffected: neither involves NLS acquisition).
4. The section-LK long-hold acyclicity clause is extended from
   "no conductor acquires it" to "no conductor or heap-2..9b
   holder ACQUIRES it; section-A.3 conductors MAY hold it on
   entry". jit section 5.6's caller precondition is adjudicated
   here (frozen text unedited): "NO section-7/cell lock" does
   not - and need not - exclude held long-hold NLS::m_lock.
5. Tests/lints: U-T11/U-T13 amplifier arm - Class-A fire AND
   haveABadTime triggered from inside lock.hold(fn) with a
   second thread contending the lock and a third parked;
   asserts stop completion + lock-order lint pass. The U20
   slot-mutex lint WHITELISTS held-NLS and FLAGS held api
   rank-1..3.

### F2 (MAJOR, accepted): section-A.1.7 suspend window composes
### unsoundly with section-LK.6's affirmative
### fastMalloc-under-registry-lock allowance
VERIFIED. Section A.1.7(i) has the SamplingProfiler resolve the
target lite via the registry (locked, target suspended); the
target is suspended ASYNCHRONOUSLY (signal/Mach) at an arbitrary
PC and may hold a bmalloc/fastMalloc internal lock. Section LK.6
re-ranks VMLiteRegistry::lock with an inner set that
affirmatively PERMITS fastMalloc while it is held (superseding
vmstate 6.5.1's stricter "no lock while held"). Composed: any
sampler allocation (Vector growth, hash insert, trace buffer)
inside the registry-locked window deadlocks the process - the
sampler blocks on the allocator lock; the target resumes only
after the sampler finishes. Neither section A.1.7, the r9-F7
ruling, nor section LK stated a no-allocation rule for the
suspended-target window. (GC registry walk unaffected: peers are
cooperatively parked at poll sites holding no rank>=4 lock, heap
I6.) DISPOSITION (normative, spec section A.1.7 SUSPEND RULE):
while ANY thread is suspended by a section-A.1.7(i) reader, the
suspending thread performs NO allocation (fastMalloc included)
and acquires NO lock beyond the already-held registry lock; all
sample/trace buffers are pre-allocated before suspension
(today's SamplingProfiler discipline made normative). Recorded
as a scoped carve-out of section-LK.6's inner set: the
fastMalloc allowance does NOT apply while a thread is suspended
by the holder (a partial return, for exactly this window, to
vmstate 6.5.1's stricter rule; the section-LK.6 SUPERSESSION
otherwise stands, both sides re-cited). U-T8d arm: sample storm
against a thread spinning in fastMalloc-heavy native code, TSAN
+ deadlock watchdog.

### F3 (MAJOR, accepted): terminated settler's per-lite
### microtask residue unowned - undeclared GIL-off-only delta
VERIFIED composition walk: spawned A resolves a shared promise
whose then() registered on B; per section E.1b.1 (SD10) the
reaction jobs land in A's OWN per-lite microtask queue (vmstate
I11 owner-only enqueue/drain). A is terminated before its next
drain: section E.5 routes termination VIA THE section-E.2 CLOSE
BLOCK, which skips the loop-top drainMicrotasks(own); the close
block's "residue" covers exactly taskQueue + waitDeadlines - the
per-lite MICROTASK queue's contents had NO disposition anywhere
(not E.5, not SD8, not annex E3), and I11 forbids adoption by
another thread. The settlement is already published cross-thread
but B's reaction jobs silently vanish. GIL-on the same reactions
sit in the SHARED VM queue (api 4.6.1) and survive A's death -
a GIL-off-only observable delta, which the SD convention
requires declared with //@ runThreadsGILOff/GILOn corpus
variants (the U19 fallback oracle hangs on the SD list being
complete). No memory-safety issue (queue GC-visible via the
vmstate 6.5 registration list until unregister). DISPOSITION
(normative, spec section E.5 + new SD17): a terminated thread's
per-lite microtask queue is DROPPED at close - never transferred
(I11), never drained (execution on a terminated thread
forbidden); declared-loss pattern per SD8/SD15 (cf. r16 F3's
process-exit-before-drain-drops precedent). REJECTED
alternative: a best-effort termination-tolerant drain at close -
it runs JS on a terminated thread. SD17 (GIL-off only):
settled state visible cross-thread; settler-local reaction jobs
enqueued-but-undrained at termination are dropped. Corpus arm
(U-T9/U-T11): spawned settler resolves a shared promise then
takes a termination trap before draining; GILOn variant expects
the reaction to run; GILOff expects settled state visible + the
reaction dropped.

### F4 (BLOCKER, accepted): section-A.3.2b JSThreads-stop Dekker
### pair (lite stop bit vs client access state) had no normative
### memory-ordering contract; the recorded r9-F3 soundness
### argument is wrong under store-buffering
VERIFIED. The thread-granular section-A.3 stop sets NO
client-visible GC stop state (no GSP), so the access-released
exemption in the section-A.3.2 conductor predicate rests
entirely on rule 2b(i): a fresh AHA polls the lite's stop bit
and parks. That is a brand-new Dekker pair - conductor:
store(stop bit) then sample(access state); re-acquirer:
CAS(NoAccess->HasAccess) then load(stop bit) - structurally
identical to heap F8's GSP pair, for which SPEC-heap.md:148
mandates seq_cst ("acq/rel misses both store-load pairs"). No
rev pinned any ordering for the new pair: section A.3.2b said
only "polls the lite's stop bit; set => park", and the
section-A.2.3 fan-out is "under registry lock" (release ordering
at best). The r9 F3 disposition recorded the soundness argument
as "an AHA that CASed HasAccess before observing the bit is
still an entered, unparked thread the conductor waits on
(defense leg (ii) closes the race)" - WRONG under
store-buffering: the conductor's sample(access) may be satisfied
before its store(stop bit) is globally visible (store-load
reordering; legal on x86 store buffers and arm64 absent seq_cst/
full fences); the conductor sees NoAccess and proceeds; the
re-acquirer's CAS lands after the sample and its stop-bit load
misses the not-yet-visible store; the thread enters JIT code
inside the patching window. Defense leg (ii) does not cover it -
the thread was never parked; the JS-poll-site trap check is too
late (jit I2/section-5.3 patching already underway). The same
unpinned pair governs section-A.3.4 entry-during-stop and the
section-F.4/DAL2 dtor re-acquire gate. Section A.3.8's GC
variant is unaffected (rides heap F8's pinned GSP). Inert under
the GIL; live exactly at GIL-off. DISPOSITION: ANNEX SB1.

### ANNEX SB1 (BINDING) - section-A.3 stop-bit / access-state
### ordering contract (heap-F8 shape; amends sections A.3.2b,
### A.3.4 and the DAL2 dtor gate; SUPERSEDES the r9-F3 item-3
### ordering argument, both sides above; the existing r9-F3 IH
### supersession row gains this ordering text only)
1. Stop-bit fan-out stores (section A.2.3) are seq_cst. The
   VMLiteRegistry lock is retained for ENUMERATION only and
   carries no ordering duty. [r28: "ENUMERATION only" SUPERSEDED
   by ANNEX EXIT1.2, both sides - the registry lock also OWNS
   the sampled set's membership/lifetime for every open
   section-A.3 window (per-sample re-walks, no pointer caching);
   the no-ordering-duty clause for the stop-bit/access pair
   STANDS, item 4's proof unchanged.]
2. The conductor's per-client/per-lite access-state samples in
   the section-A.3.2 predicate wait are seq_cst loads.
3. The AHA stop-bit poll (section A.3.2b(i)) is positioned
   AFTER the F8 step-1 seq_cst CAS - beside the F8 step-2 GSP
   load - and is a seq_cst load; on set, F8 mandatory-revert
   (seq_cst exchange->NoAccess) then NVS park (r9 F3 shape,
   unchanged). Same position + ordering for the section-A.3.4
   token-acquisition stop-word check and the section-F.4/DAL2
   dtor re-acquire gate.
4. Proof (mirrors heap F8's): the four ops - conductor S1 =
   store(stop bit), L1 = sample(access); re-acquirer S2 =
   CAS(access), L2 = load(stop bit) - are all seq_cst, hence in
   ONE total order with S1 < L1 and S2 < L2 (program order
   preserved within seq_cst). If L1 does not observe S2 then
   L1 < S2, so S1 < L1 < S2 < L2 and L2 observes S1: the
   re-acquirer reverts and parks. If L1 observes S2, the
   conductor counts the thread HasAccess-unparked and keeps
   waiting. Either way no thread executes JS/JIT inside the
   stop window. acq/rel is INSUFFICIENT - both interleavings
   above are store-buffering (SB) litmus shapes, observable on
   x86 and arm64 without the seq_cst total order.
5. Defense leg (ii) (post-wake park-site poll) is unchanged and
   remains defense-only. Section A.3.8 needs nothing: it rides
   heap F8's GSP, whose ordering is already pinned.
6. Tests: U4 gains a TSAN + litmus arm - conductor fan-out
   racing a release-then-immediately-reacquire loop, run on
   arm64 hardware; U20 lint: any stop-bit store/load not
   through the seq_cst accessors flagged.

### rev-24 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays
### in the cited BINDING annex/rev)
- U0c index: "gains RELEASE_ASSERT(gilOffProcess => server VM
  m_gilOff==1)" -> "+ gilOffProcess=>server-VM assert" (annex
  U0C carries the assert).
- A.3.2c ISB1 index: the rationale sentence "Stops patch code
  while access-released threads sleep UN-parked; R1.d's ISB
  fires only on NVS exit" moved here (it is ANNEX ISB1's
  preamble); mechanism text unchanged.
- LK WS rows: WS1.3 soundness summary "holders poll-free,
  access-retaining, never park => 10.4/F8 drains them
  pre-sweep" -> "sound per WS1.3" (annex WS1 carries it);
  "loser/stale discards drop after release (dealloc
  lock-free, ...)" -> "discards drop after release (...)".
- K.3: "U20 lints park-/access-transition-under-10a/10b +
  section-K.3 touches dominated by stop-scope/slot-mutex
  acquisition" -> "U20 per LZ2.4" (LZ2.4 carries the lint
  set); "landed recursion contract" -> "landed contract";
  "(offenders re-ruled class 1/2)" dropped (LZ2.4).
- N.5: lowering paragraph compressed to an r17-F5 pointer
  (that disposition is the FULL BINDING text); intrinsic pair
  merged into one sentence.
- A.2.8 watchdog, A.3.5, A.3.6/A36C, D.1, F.1/F1B, E.3
  ("exactly-once via per-ticket CAS" now stated once, in the
  DECREMENT clause - annex E3 unchanged), E.7.3, E.7.4
  ("dispatched AFTER" -> "AFTER"), E.7.5 ("(U-T9/U-T11)"
  dropped - the U-T rows name it), K.5, N.6 ("IM:
  ArrayBuffer.{h,cpp}" lives in the IM annex), N.8 ("optional
  section-5.7.2-pattern claim CAS dedups" -> "optional
  claim-CAS dedup"): index joins/tightenings only.
- F.4: duplicate "GIL-on unchanged." dropped ("LIFTED GIL-off,
  KEPT GIL-on" stands). F.6: duplicate "r22 list" dropped.
  B.3: "one IH row" -> "IH row". C.4: "IU row annotates I21"
  -> "IU row". I: "full list: r22 list" -> "list: r22"; "FULL
  text history r9 F8" -> "FULL text: r9 F8". A.1.3: "SECOND
  derived" -> "derived"; "(b2) section-N.5's uniform
  builtin-bytecode intrinsic" -> "twin intrinsics". A.1.1: "a
  new emitter" -> "new emitter". F.1 spawned: "a per-thread
  entry token" -> "an entry token" (tokens are per-thread by
  construction). 0/U0b: "Corpus arms + IU row per history
  (r22 list)" -> "Corpus + IU row per r22 list". A.3.3 tail:
  "Slot mutex ranked section-LK.4b; U20 lints it" -> "(U20)".
  E.2 EXPIRE line + close block: cosmetic. E.4: "U-T8 IU
  table lists settle sites' lock context" -> "U-T8 IU
  settle-site lock-context table".
- T section: Deps line drops the two entry-gate repeats
  (still stated in the Index ENTRY GATE notes); "r10-r23" ->
  "r10-r24"; U-T9 corpus + U-T11 arms gain SD17. SD section:
  normative-text cite gains r24; U19 variant range
  SD8-SD16 -> SD8-SD17. IM: rev-7..23 -> rev-7..24. LK header
  acyclicity cite: "history r8 + r22 WS1" -> "r8 + WS1 +
  NLH1".

### Rev-24 SD note
SD17 added (terminated settler's undrained per-lite microtask
queue dropped at close; GIL-off only). IDs frozen; no other SD
changes.

### Rev-24 section-T deltas (extends rev-9 annex 3 + r10-r23)
- U-T8d: + sample-storm arm (F2: target spinning in
  fastMalloc-heavy native code; TSAN + deadlock watchdog).
- U-T9/U-T11: + SD17 corpus arm (F3: settler terminated
  between publish and drain; GILOn reaction runs / GILOff
  reaction dropped, settled state visible in both).
- U-T11/U-T13: + NLH1.5 amplifier arm (F1: Class-A fire AND
  haveABadTime from inside lock.hold(fn); contender +
  parked third thread; stop completes, lock lint passes).
- U-T5/U4: + SB1.6 litmus arm (F4: conductor fan-out vs
  release-then-reacquire loop, arm64 hardware run).
- U20: slot-mutex lint whitelists held-NLS / flags held api
  1..3 (NLH1.5); + non-seq_cst stop-bit access lint (SB1.6).

# REV 25 (2026-06-06) - directed revision round (4 items: ALS x
SD10 ruling, citation-drift re-verify, U0c first-VM-wins embedder
contract, Watchdog assert ruling). All four verified against the
tree before writing. Spec re-frozen rev 25; stayed <=50000 bytes
via annexes E2A/ALS1/EC1/W4 + the r25-ext relocations below (same
BINDING standing as the rev-12/rev-13 moves).

## Rev-25 item 1 - ANNEX ALS1 (BINDING): AsyncLocalStorage under
## SD10 (thread-migrating continuations)

Question: SD10 makes async-function continuations and ordinary
.then() reactions run on the SETTLING thread (spec §E.1b.1). Bun's
AsyncLocalStorage rides JSC's Bun-additions async context
(InternalFieldTuple). Does migration lose the store?

Tree verification (all USE(BUN_JSC_ADDITIONS) paths):
- CAPTURE is PER-REACTION, at registration time. Each site reads
  the CURRENT cursor value
  globalObject->m_asyncContextData.get()->getInternalField(0)
  (cursor slot: JSGlobalObject.h:507, WriteBarrier<
  InternalFieldTuple> m_asyncContextData) and stashes it into the
  reaction's own [userContext, asyncContext] InternalFieldTuple or
  directly into the microtask's argument:
  - JSPromise::performPromiseThen, JSPromise.cpp:341-357 (tuple
    field 1 = asyncContext);
  - JSPromise::performPromiseThenWithContext, JSPromise.cpp:
    433-449 (tuple = [userContext, asyncContext]);
  - await resume: JSPromise::
    resolveWithInternalMicrotaskForAsyncAwait, JSPromise.cpp:
    989-1001 (tuple wraps the await context + asyncContext);
  - thenable jobs: JSPromise.cpp:692-695 (fast path, asyncContext
    passed as microtask arg) and :724-727
    (PromiseResolveThenableJob, 4th arg);
  - then() prototype fast path: JSPromisePrototype.cpp:296-303.
- RESTORE is at job-run time, on WHATEVER thread drains the job:
  the runner saves the cursor, writes the captured value, runs the
  reaction, writes the saved value back - JSMicrotask.cpp:
  1531-1556 (asyncContext = arguments[2]), :1578-1598 (arguments
  [3]), :1611-1631 (tuple->getInternalField(1)).
- NO per-thread-VM-state capture exists on these paths: nothing
  reads the cursor at RUN time to decide the reaction's context;
  the only run-time cursor use is the swap/restore bracket around
  the captured value. (Checked: m_asyncContextData consumers are
  exactly JSPromise.cpp, JSPromisePrototype.cpp, JSMicrotask.cpp,
  JSGlobalObject.{h,cpp}.)

RULING (normative, spec §E.1b.5):
1. SD10 thread-migrating continuations PRESERVE AsyncLocalStorage.
   The captured context tuple is an ordinary shared-heap object
   carried BY THE REACTION JOB itself; under the shared GC heap it
   is readable on the settling thread like any other cell. No
   inbox-job carry mandate is needed - the carry already exists
   structurally. (Had capture been per-thread-VM-state anywhere,
   the ruling would instead mandate carrying the context in the
   §E.4 ThreadTask / inbox job; it is not, so it does not.)
2. ALS1.2 visibility: the capture site publishes the tuple via the
   normal §E.1b enqueue (I11 own-queue or §E.4 ThreadTask append
   under inboxLock); both edges carry the needed release/acquire
   ordering, so the settling thread reads an initialized tuple.
3. ALS1.3 NEW REQUIREMENT - the CURSOR is shared mutable state:
   m_asyncContextData is per-JSGlobalObject (per-realm) and is
   swap-WRITTEN by every job run (restore bracket above) and by
   Bun's enter/exit hooks. GIL-off, two threads draining reactions
   of the SAME realm would clobber each other's bracket. RULING:
   GIL-off the cursor reroutes PER-LITE (§K.1 class duplicate:
   accessor keys on the CURRENT lite; cell-holding copy GC-scanned
   via the registry walk, §A.1.3 GC-roots rule). "Current async
   context" is thread-local by definition, so per-lite is the
   semantically correct shape, not just a race fix. GIL-on/
   flag-off unchanged (single mutator => single cursor).
4. Semantic-delta test note (corpus, U-T9; ALS note rides SD10, NO
   new SD): spawned thread B resolves a shared promise; thread A
   registered .then()/await inside ALS store S. GIL-off the
   continuation runs ON B (SD10) and MUST observe S (the
   registration-time store), not B's current store; after the
   reaction, B's own cursor value is restored exactly. GIL-on
   variant keeps phase-1 expectations (//@ runThreadsGILOn).
Embedder note: this discharges the §F.6(b) continuation-affinity
question for ALS specifically - Bun need NOT demand a carrier hop
to keep AsyncLocalStorage correct. §F.6(b) sign-off (U-T9 entry
gate, r21) remains for non-ALS affinity concerns.

## Rev-25 item 2 - citation-drift re-verify (directive list)

- "Heap.cpp:4115" (I13): tree has the s_stickySharedServer CAS at
  Heap.cpp:4123 and the I13 RELEASE_ASSERT(!previous || previous
  == this) at :4124. Spec already cites :4124 (§0 U0b) / :4123-4124
  (§F.6(d)) - the rev-12 correction held. The one remaining stale
  ref was inside BINDING annex U0C item 2 (this file): fixed
  in-place with an [r25 line-drift fix] marker.
- Atom-table assert "Heap.cpp:2348": :2348 is the m_worldState
  hasAccessBit/mutatorHasConnBit RELEASE_ASSERT cluster; the atom
  assert is Heap.cpp:2796 (requestCollection,
  RELEASE_ASSERT(vm().atomStringTable() ==
  Thread::currentSingleton().atomStringTable() ||
  worldIsStoppedForAllClients())). Spec §A.3.7 already cited :2796
  (r16 F4); r25 re-verified and dropped the historical "NOT :2348"
  gloss (recorded here instead).
- "JSCConfig.h:104": :104 is `OptionsStorage options;`. The M4a
  slot comment ("SPEC-jit M4a (Option 1 ...)") begins at
  JSCConfig.h:106 (slot itself butterflyTIDTagTLSKey :109). Spec
  §A.1.3(i) re-pointed :104 -> :106 (the gilOffProcess byte lands
  beside the M4a slot).
- Watchdog asserts: currentThreadIsHoldingAPILock at
  runtime/Watchdog.cpp:44, :57, :132, :160 - confirmed, see item 4.

## Rev-25 item 3 - ANNEX EC1 (BINDING): FIRST-VM-WINS U0c embedder
## contract (spec §F.6(d))

Previously an EMERGENT property of U0c's ctor CAS (annex U0C item
2): under gilOffProcess the first VM constructed wins
Heap::tryDesignateStickySharedServer() (CAS Heap.cpp:4123, I13
RELEASE_ASSERT :4124, one sticky server per process EVER - a §10D
reversion does not free the slot for a different server) and gets
m_gilOff=1; every later VM is m_gilOff=0 and Thread spawn throws
RangeError (U0b). Consequence an embedder can trip silently: a
"utility" VM constructed first (e.g. for config parsing, a
pre-boot snapshot, a diagnostics VM) PERMANENTLY demotes the real
main VM to spawn-RangeError for process lifetime - there is no
re-designation API in v1 (deliberate: I13 + the immutable
m_gilOff byte are load-bearing for §A.1.3 codegen and heap I13).
NORMATIVE (named contract, spec §F.6(d)):
1. The embedder MUST construct the VM intended to spawn Threads
   strictly before any other VM in the process when running
   gilOffProcess.
2. Recommended pattern: immediately after constructing the main
   VM, boot-assert vm.m_gilOff == 1 (debug: RELEASE_ASSERT in the
   embedder's init path) so a violated construction order fails at
   boot, not at first Thread() with a confusing RangeError.
3. The IU embedder checklist gains a construction-order audit row:
   enumerate every VM construction site in Bun (incl. lazily
   created helper VMs) and prove main-first or gate them behind
   first-entry of the main VM.
4. v1 explicitly declines a designation override option
   (Options::jscMainVM or similar) - revisit post-ungil if Bun's
   boot order cannot guarantee main-first.

## Rev-25 item 4 - ANNEX W ext, W4 (BINDING): the four Watchdog
## APILock asserts

The W annex (r9, ext r15 F2) ruled scheduling (W0-W3) but left the
four asserts to the §K.4 catch-all. Tree: runtime/Watchdog.cpp:44
(setTimeLimit - writes m_timeLimit, derives m_cpuDeadline), :57
(shouldTerminate - reads/rearms), :132 (enteredVM - per-entry
timer start), :160 (exitedVM - timer stop). All four guard state
with NO serializer other than today's GIL: under §F.2's REDEFINED
token meaning, N spawned threads would satisfy the assert
simultaneously while racing m_timeLimit/m_cpuDeadline and the
start/stop pair.
RULING W4 (spec §A.2.8): watchdog v1 GIL-off is CARRIER-ONLY -
chosen over per-thread CPU-deadline semantics (deferred
post-ungil; W0 already makes budget/accounting carrier-only,
SD14, so per-thread deadlines would be unobservable v1 anyway).
1. All four sites are §F.2 EXCLUSIVITY CONSUMERS in the U-T8
   table; named serializer = the REAL JSLock m_lock (§F.1 keeps
   main/embedder mutual exclusion GIL-off, so at most one carrier
   is between the asserts at a time).
2. Spawned threads never reach them: Watchdog entry/exit hooks
   early-return on VMLite::isSpawned (the W0 exemption's
   enforcement point); spawned JS is watchdog-unobserved v1
   (SD14, unchanged).
3. Assert rewrite (GIL-off branch): the four ASSERTs become
   JSLock::currentThreadIsHoldingLock() (mutex-literal predicate,
   §F.2) && !VMLite::isSpawned. GIL-on/flag-off: byte-identical
   behavior (token meaning == lock meaning there).
4. W2/W3 interplay unchanged: last-carrier exit wall-clock and the
   no-carrier tokenless timer never touch the four sites off the
   carrier; the W3 timer thread fires termination via §A.2.3
   rule-3 fan-out, not via these methods.
U-T2 gains: assert-rewrite + spawned-unreachability lint; U-T11
arm: watchdog fires while a spawned thread runs hot JS and the
carrier is parked (W1 path) - no spawned-side assert trips, TSAN
clean on m_timeLimit/m_cpuDeadline.

## Rev-25 spec compressions - r25 ext (BINDING; relocated index
## text, normative content preserved verbatim; pointer targets
## unchanged)

- §E.2 -> ANNEX E2A below: the whole drain/close pseudocode block
  moved VERBATIM; spec keeps a prose index.
- §C.1 dropped glosses: lock-free arm CAS/RMW operates "on the
  EncodedJSValue word - U5"; flat arm "GROW per annex"; OM-locked
  arm is "dict I19/L3 + AS §4.6 under JSCellLock"; indexed-by-
  shape: "Int32/Double CONVERT to Contiguous, raw-word CAS
  REJECTED; AS/dict locked". (All present in annex C1.)
- §K.3 dropped glosses: owner recorded in "per-VM side table/spare
  bits" (r16 F2); foreign-wait quanta = "release access, dual stop
  poll, gated re-acquire, re-test"; LZ1 (a) CYCLE escape -
  owner-walk reaching SELF returns null, (b) ABANDONMENT - winner
  unwind CASes initializing->empty, later toucher re-runs;
  precondition cites "heap I6/OM O2; winner alloc, OM O1" and the
  r17-F2 shape; conductor-closure-reachable examples "(CLASS-4
  body, §A.2.7 walk, D1R fire)"; pre-resolve happens
  "pre-slot-mutex".
- §N.5 dropped glosses: claim-failure dispatch list "(TypeError /
  landed completed path / retry)" (r12); intrinsic signature
  "@atomicInternalFieldClaim(cell,index,expected,replacement) ->
  bool"; publish = "UNCLAIM, Running->SuspendedX/Completed"; host
  op contingency "R5-class inline CAS"; DFG/FTL else-arm "landed
  plain nodes"; bench figures "flag-off arm GATED in BENCH.md (1%
  vs pre-threads); gilOff arm under the §B.5 composite".
- §N.6 dropped glosses: DETACH "base uncleared", jettison
  "(neutering watchpoints)"; SHRINK tail free deferred "to stop
  retirement"; GROW else-arm "relocate under a stop"; "Wasm-backed
  detach = detach arm". (All in annex N6.)
- §LK WS rows dropped cites: superseded landed shape sites
  "ThreadManager.cpp:234-280 + RegExpCache.cpp:62-65"; discard
  drop site "(WeakSet.h:121-131)"; finalize body examples
  "(pruneRestrictedObject, RegExpCache::finalize)"; "((i) removes
  the reverse edge)"; "(§D.1 stands)". (All in ANNEX WS1.)
- §D.1 dropped glosses: TTL fire chain "(=> WTL + OM F4 chain)";
  "(OM I11/I15 hold)". (Both in annexes D1/D1R.)
- §A.3.6 dropped glosses: TLS tuple TID-tag qualifier "jit P5/
  CS3"; "~VM: M6 replaced per annex" retained, "I20 holds" kept.
- §A.3.7: historical "NOT :2348" gloss dropped (item 2 above).
- Pure rewraps (no text change): §A.1.3, §A.1.6, §E.1b.2, §E.7.5,
  §D.2, §INV, §K.5 (":2460" comma), header.

### ANNEX E2A (BINDING) - §E.2 normative drain loop, VERBATIM
### (moved from spec rev 24; only the two §-internal references
### unchanged)

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
 F1/F5 as landed; access release at the landed T5 point (§B.2, U3)
```

[r28: the tail's T5 line is AMENDED by ANNEX EXIT1.3, both sides
- post-release T5 order is unregisterLite -> DCT -> destroy
GCClient::Heap -> free lite (U3 as AMENDED, U32); the loop/close
body above is unchanged.]

[r29: the r28 order above is itself superseded by ANNEX EXIT1.3
as AMENDED by rev 29 - post-release T5 order is TEARDOWN mark
(registry lock) -> DCT -> destroy GCClient::Heap ->
unregisterLite/free lite (U3 as re-AMENDED, U32); the loop/close
body above is still unchanged.]

### Rev-25 section-T deltas (extends rev-9 annex 3 + r10-r24)
- U-T9: + ALS1.4 corpus arm (foreign-thread resolve observes the
  registration-time ALS store; GILOn/GILOff variants) + the
  §K.1 per-lite m_asyncContextData reroute lands with §E.1b.
- U-T8b: m_asyncContextData added to the §K inventory as a
  PRE-RULED class-1 row (ALS1.3).
- U-T2: W4 assert rewrite + spawned-unreachability lint; U-T11:
  W4 carrier-parked watchdog arm (annex W ext).
- U-T14: §F.6(d) construction-order audit row joins the (a)/(c)
  close items; (b)'s ALS slice discharged by ALS1 (residual
  non-ALS affinity sign-off unchanged, r21).

### Rev-25 SD note
No new SDs; IDs frozen. ALS preservation is recorded as an SD10
clarification (§E.1b.5), corpus arms above.

# REV 26 (2026-06-06) - audit execution round (U-T8b/U-T8c
# EXECUTED; residue rulings; byte-budget compressions)

Spec bumped rev 25 -> rev 26. The two §K.4/§N.7 inventory audits
were EXECUTED against the tree and frozen as BINDING annexes:

- annex K4 = SPEC-ungil-audit-K4.md (VM/JSGlobalObject/process-
  global member inventory; rows K4.<table>.<row>; tables I-IX).
- annex N7 = SPEC-ungil-audit-N7.md (shareable-cell non-property
  multi-word state inventory; rows R1-R31 + §0 residue).

Spec §K.4 and §N.7 now declare the audits EXECUTED + BINDING and
re-scope U-T8b/U-T8c from "perform the audit" to "CONSUME the
audit tables" (spec §T index updated). U-T9's audit gate is
satisfied by annex closure: every formerly-UNRESOLVED row now
carries a ruling (this annex) or a MECHANICAL reclassification
recorded in the audit file itself.

## ANNEX AUD1 (BINDING) - audit-residue rulings (spec §K.6/§N.9
## index; this is the FULL text)

### AUD1.K1 (K4-U1) SamplingProfiler under N mutators - SD18
GIL-off the SamplingProfiler runs in §A.1.7 form (i) and samples
ONLY the main/carrier thread's lite; spawned threads' frames are
NEVER captured (consistent with §A.1.7 v1 "SamplingProfiler
samples ONLY carrier lites", spec A.1.7). start()/stop()/
shutdown/reports remain main-thread APIs (SD13/SD14 family);
internals keep m_lock (SamplingProfiler.h:218). m_jscExecutionThread
binds to the main carrier; the (i)-reader SUSPEND RULE (r24)
applies to its stack walk. GIL-on unchanged. SD18 (GIL-off only):
profiles omit spawned-thread samples. N-thread capture (per-lite
frame buffers + registry iteration under a §A.3 stop) is
chartered post-ungil, NOT v1. Corpus: profiler-on + 2 threads ->
no crash, main-only samples; U-T2 arm.

### AUD1.K2 (K4-U2 = N7-U7) RegExp legacy statics - SD19
JSGlobalObject::m_regExpGlobalData (RegExpGlobalData.h:64-65,
RegExpCachedResult.h:66-82) becomes a §K.1 per-lite member:
each entered thread owns a private RegExpGlobalData stream;
cell-holding copies join the §A.1.3 registry-walk root set; ~VM
walk frees them (U-T8 walk). SEMANTICS (SD19, GIL-off only):
RegExp.$1-$9 / lastMatch / leftContext / rightContext /
input observe ONLY matches performed by the CURRENT thread.
Rationale: these are deprecated Annex-B statics; a global-object
lock would put a §LK acquisition on EVERY successful match (hot
path) and still yield nondeterministic cross-thread values.
TIERS: DFG/FTL RecordRegExpCachedResult + every
offsetOfResult/offsetOfLastInput/offsetOfLastRegExp consumer is
re-pointed through the lite per AUD1.K4 (A16 ext); gilOff-mode
compilation emits loadVMLite -> liteRegExpGlobalData -> field;
flag-off keeps the baked global-object-relative address. Reify
flip (m_reified + 4 barriers) stays single-thread-private =>
plain stores. Corpus: regexp legacy-statics arm, GILOn/GILOff
variants (SD19); unblocks the regexp corpus arm flagged in
annex K4 §0.

### AUD1.K3 (K4-U3) module evaluation state
(a) VM::m_moduleAsyncEvaluationCount (VM.h:1332): std::atomic
fetch_add, relaxed. ECMA [[AsyncEvaluation]] ordering needs only
uniqueness + monotonicity of issued values, which fetch_add
gives globally; cross-thread interleaving of independent graphs
is otherwise unobservable. No SD.
(b) VM::m_synchronousModuleQueue (VM.h:1358, Bun addition):
per-lite (§E.1 family) - each thread drains its OWN synchronous
module jobs with its microtask queue; enqueue sites route via
the CURRENT lite (same reroute shape as VM::queueMicrotask).
(c) Cross-thread evaluation of ONE record: AbstractModuleRecord
status advance (Linked -> Evaluating) is a CLAIM taken under the
record's cell lock (the same lock already serializing
m_dependencies/m_asyncParentModules, annex N7 R16). The winner
evaluates (user JS OUTSIDE the lock); losers re-read status
under the lock and (async graph) adopt the existing top-level
promise per spec, or (sync completion required) PARK-CAPABLE
wait access-released on the record's completion, §K.3-wait
shape (bounded quanta, §A.2.4 polls; LZ2 preconditions apply to
the waiting site: no api 1..3/heap 10a/10b lock held). Settled
completion published release; errored => rethrow per spec.
GIL-on unchanged. Confirms + supersedes annex K4's interim
"main-thread evaluation" note (K4.V.18 reclassified: per-lite
queue + claim protocol, NOT main-only).

### AUD1.K4 (K4-U4) ANNEX A16 EXTENSION - JIT-baked per-lite
### cache addresses
Annex A16's loadVMLite -> segment -> index rework is EXTENDED
beyond scratch buffers to every §K.1 per-lite member whose
address is baked into Baseline/DFG/FTL inline paths, namely:
VM::m_megamorphicCache (VM.h:960), VM::m_hasOwnPropertyCache
(VM.h:956), JSGlobalObject::m_regExpGlobalData (AUD1.K2), and
JSGlobalObject::m_weakRandom (annex K4 VIII.10, Math.random
fast path). Mechanism: gilOff-mode compilation (the §A.1.3
COMPILED-FOR-VM-mode rule) emits one loadVMLite (rematerialized
per §A.1.2) + lite-relative offsets to the lite-resident copy;
the lite holds the cache inline or via one indirection slot
filled at lite registration (lazy §K.3 publish for ensure*
contents). Flag-off/GIL-on: baked VM/global addresses unchanged
(golden gates intact). Epoch/age bumps (MegamorphicCache
invalidation) fan out via the registry walk INSIDE the stop
that fires the corresponding watchpoints (annex K4 VI.2) - no
new fence on the probe path. Per-lite caches are private =>
probe/fill races vanish; no locked fallback needed. U-T4 owns
the emission; disasm arm per A16.

### AUD1.K5-K7 (K4-U5/U6/U7) - MECHANICAL reclassifications
Recorded in annex K4 §0 with rationale; normative content:
- K4-U5: spec §E.7.1's "m_pendingLock" IS the in-tree
  DWT::m_taskLock (DeferredWorkTimer.h:116) - name equation
  noted in §E.7/§LK.7; its coverage EXTENDS to m_pendingTickets
  (:121), whose three-condition comment (:125-126) loses the
  GIL leg. One lock, §LK.7 leaf; no second lock.
- K4-U6: JSGlobalObject::m_canFastQueueMicrotask /
  m_associatedContextIsFullyActive: writes main-only (debugger/
  context attach, SD13 umbrella), reads relaxed-atomic from any
  thread; stale-true window at most skips debugger microtask
  observation for in-flight enqueues = SD13-class degradation,
  no new SD.
- K4-U7: SmallStrings verification PASSED: initializeCommonStrings
  runs in the VM ctor (VM.cpp:335); the !m_isInitialized fallback
  (SmallStrings.cpp:121-127) allocates a fresh AtomStringImpl and
  writes NO member; setIsInitialized(false) is teardown-only
  (VM.cpp:707). immutable-after-init CONFIRMED; gets the K4 §VIII
  no-write-after-first-cross-thread-entry assert.

### AUD1.N1 (N7-U1) AbstractModuleRecord::m_resolutionCache
§N default: tryGetCachedResolution/cacheResolution take the
record's JSCellLock (10a) - the SAME lock already used by the
sibling maps (AbstractModuleRecord.cpp:1465/:1561) - §E.1b
alloc-outside shape (HashMap add may rehash => the add runs
under the lock but any resolution computation stays outside).
No tier-inlined access exists (namespace loads IC on the
namespace object). Fixes a GIL-off HashMap-rehash UAF (annex
15.7 class). PRIORITY ruling; amplifier: 2-thread shared-
namespace property storm (U28).

### AUD1.N2 (N7-U2) RegExp::m_ovector
Per-match output scratch moves OFF the shared cell GIL-off:
matchInline (RegExpInlines.h) writes into the CURRENT lite's
regexp match buffer - the §A.1.3 Group-3 "lazy regexp
stack/match buffers" member (annex K4 table I row) - sized per
match; ovectorSpan() consumers receive the lite buffer span.
The RegExp cell retains compile-state only, already cell-locked
in-tree (annex N7 R13). DFG/FTL RegExpExec/Match thunks land in
matchInline and inherit the re-point; no inline JIT reads
m_ovector directly. GIL-on keeps the cell vector. Fixes a
racing-resize realloc UAF + torn capture reads. PRIORITY;
amplifier: 2-thread exec() on one shared RegExp (U28).

### AUD1.N3 (N7-U3/U4) arguments family publication
- DirectArguments m_mappedArguments + GenericArgumentsImpl
  m_modifiedArgumentsDescriptor: CAS-PUBLISH - allocate + fill
  the bitmap/storage COMPLETELY, then ONE release-CAS of the
  pointer word; losers discard (GC-collected); foreign readers
  load-acquire (tier-inlined null-check is an address-dependent
  load, jit F2 shape - stays inline).
- ScopedArguments m_overrodeThings: release-store AFTER the
  length/callee/caller OM puts complete; foreign slow-path
  readers acquire.
- ClonedArguments m_callee clear (the materialized flag):
  release-store AFTER materializeSpecials' OM puts; readers
  acquire on the slow path. Guarantees no lost callee/length
  (THREAD.md "no lost properties").
The property-materialization halves follow OM property rules
unchanged. Amplifier: foreign reader vs owner override (U28).

### AUD1.N4 (N7-U5) StructureRareData runtime caches
All cache INSTALLS (cachedPropertyNameEnumerator + watchpoint
vector + flag word; m_cachedPropertyNames slots; special-
property caches) run under Structure::m_lock (the structure
owns its rare data; OM GT lock order). Each JIT-read word
(m_cachedPropertyNameEnumeratorAndFlag, m_cachedPropertyNames[i])
is published by a SINGLE release store, LAST - the watchpoint
FixedVector is fully constructed before the flag word publishes
and is immutable thereafter; baseline/DFG readers consume one
word (existing loads suffice on x86/arm64 with the release
publish). m_specialPropertyCache pointer = §K.3 lazy-publish;
its interior fill precedes publication. Watchpoint FIRING stays
jit-spec/§K.5 territory (annex K4 VI.2). OM-annex cross-
amendment noted: OM annex 15 gains a pointer row to this ruling
(doc-only; no frozen OM text superseded). Amplifier: 2-thread
for-in over one shared structure (U28).

### AUD1.N5 (N7-U6) Intl cell family
Default per §N: every member mutated post-construction
(IntlNumberFormat::m_numberingSystem and peer lazy Strings;
IntlSegmentIterator's UBreakIterator advance; IntlLocale lazy
fields) is accessed under the owning cell's JSCellLock; lazy
Strings are computed OUTSIDE the lock and published under it
(two-word String => lock, not CAS). Construction-frozen ICU
handles (UCollator, UNumberFormatter, ...) may be used
concurrently WITHOUT the lock ONLY where the call site is
verified against ICU's const/thread-safe contract; the
verification checklist is consumed at implementation time per
cell class (U-T8c consumption); unverified sites clone-per-use
under the cell lock (ucol_safeClone class) or take the lock for
the call. No foreign-thread TypeError, no SD. All host-call
paths; no tier-inlined access.

## Rev-26 reclassification record (audit files edited in place)
Annex K4 §0: U1-U4 -> RESOLVED pointing at AUD1.K1-K4; U5-U7 ->
MECHANICAL with rationale (AUD1.K5-K7); K4.V.18 re-ruled per
AUD1.K3(b)/(c); K4.V.3 re-ruled per AUD1.K1; K4.II.18/II.19
UNRESOLVED-4 arms discharged per AUD1.K4; K4.VIII.10 JIT note
discharged per AUD1.K4. Annex N7 §0: U1-U6 -> RESOLVED pointing
at AUD1.N1-N5; U7 -> RESOLVED cross-ref AUD1.K2; gate
disposition updated (no blocking UNRESOLVED; U-T9 audit gate
SATISFIED on the annex side).

## Rev-26 SD note
SD18 (sampling profiler main-thread-only capture) + SD19
(per-thread RegExp legacy statics), both GIL-off only; corpus
//@ runThreadsGILOff/GILOn variants per the SD1-SD17 pattern.
IDs frozen.

## Rev-26 section-T deltas (extends rev-9 annex 3 + r10-r25)
- U-T8b/U-T8c re-scoped: CONSUME annexes K4/N7 (no enumeration
  work remains); deliverables = the §K class implementations
  per K4 rows, §N dispositions per N7 rows, the VIII
  no-write-after-entry assert macro, ~VM per-lite walk, §F.2
  consumer-row citations.
- U-T4 gains the AUD1.K4 A16-ext emission rows.
- U-T9 entry gate: annex-K4/N7 §0 closure (DONE at r26) replaces
  the former "audits must close" wording.
- U26/U28 amplifier arms gain: regexp legacy statics (SD19),
  2-thread shared-namespace storm, 2-thread shared-RegExp exec,
  arguments foreign-reader, shared-structure for-in (annex N7
  list), profiler-on 2-thread arm (SD18).

## Rev-26 spec-body wording compressions (byte budget; no
## semantic change - every trimmed clause's FULL text stays in
## the cited BINDING annex/rev)
§K.3 (r25 ext/LZ1/LZ2), §N.5 (r11/r15 F1/r17 F5/r25 ext), §N.8
(ANNEX CBI), §A.3.2c (ISB1), §A.3.5 (HBT2-4), §A.3.6 (A36),
§A.3.8, §D.1 (D1/D1R), §E.1b.5 (ALS1), §E.2 (E2A), §C.1 (C1),
§C.3 (C3), §F.4 (DAL2), §F.5, §F.6 (EC1), §A.2.8 (W/W ext), §I,
§K.1/K.2 (lists -> annex K4 §II/§III), §LK.7 note, §E.4, §A.1.1.
Pointer targets unchanged; supersession rows untouched.

## rev 27 (2026-06-06) - fresh-implementer walkthrough repair
## round: 6 findings (3 blocker, 3 major)

A fresh-implementer walkthrough reconstructing the rules from the
frozen documents alone hit six ambiguities/dead pointer chains.
Each is resolved below; normative full text = ANNEX TERM1 (this
rev). Spec bumped rev 26 -> rev 27. No new SD IDs (SD8 gains
ext2, rides SD8's frozen ID).

1. BLOCKER - "Thread.prototype.terminate" unreconstructible: the
spec's terminate arms (SD6 terminate-parked, SD8, U19, sect T
terminate-during-TA-wait, sect E.5) presupposed a termination
request whose surface no frozen document defines, and api 4.1
affirmatively excludes ("no detach/cancel"). RULING: no such API
exists in v1 - TERM1.1; spec sect A.2.4 rewritten. No
supersession needed: api 4.1 stands verbatim; the arms always
meant VM-level termination (ambiguity, not contradiction).
2. BLOCKER - termination granularity: sect A.2.3/A.2.4 (VM-wide
fan-out) vs the sect E.5/SD8 narrative (which a reader could
take as one-thread-dies-others-survive). RULING: VM-WIDE ONLY -
TERM1.2; sect E.5 amended to say every entered thread closes and
the VM survives via the carrier's host servicing.
3. BLOCKER - sect F.5 (nested foreign-VM entry, generic wording)
vs BINDING Annex A36 ("Spawned Threads single-VM in v1
(foreign-VM token RELEASE_ASSERTs)"). RULING: TERM1.5 - F.5 is
carrier-only; A36 stands; new embedder-contract item F.6(e).
4. MAJOR - discriminator divergence (VMLite::isSpawned vs
isJSThreadCurrent(); which gilOff level sect C.4 reads). RULING:
TERM1.4; spec sect I + sect A.1.3 + sect C.4 annotated. Mostly
moot given finding-3's resolution (no spawned nesting), recorded
for clarity.
5. MAJOR - join()/asyncJoin() outcome for a terminated thread
unruled (which exception Phase::Failed carries; rethrow
re-terminating the joiner). RULING: TERM1.3 = SD8 ext2 (fresh
ordinary Error).
6. MAJOR - IU (INTEGRATE-ungil.md) cited ~30 times but the file
does not exist. RULING: TERM1.6; spec sect IM amended (IU is a
U-T1 deliverable; this workflow's write set cannot create it).

### ANNEX TERM1 (BINDING) - termination model, F.5 caller scope,
### discriminators, IU creation (r27)

TERM1.1 No thread-targeted termination surface in v1.
Thread.prototype.terminate DOES NOT EXIST. The Thread surface
stays api 4.1 VERBATIM (constructor, join, asyncJoin, id,
current, restrict; Lifecycle Running->Finished(result)|
Failed(exc); no detach/cancel) - NOT a supersession: nothing in
the frozen set granted a terminate API. Every "terminate" arm in
SPEC-ungil (SD6's terminate-parked arm, SD8, U19's
terminate-parked arm, sect T's terminate-during-TA-wait flag-off
gate, sect E.5's termination trap) means VM-LEVEL termination
requested by one of: (a) Watchdog (annex W; corpus --watchdog,
cf. SPEC-api-annex property-wait-termination.js), (b) the
embedder's VMTraps termination request (NeedTermination class,
api G23 anchor VMTraps.h:149-156), (c) shell/embedder teardown
paths that route through (b). A thread-targeted terminate() (and
any future Thread.prototype.terminate) is POST-UNGIL work and
would require a new SD plus an api-4.1 supersession recorded
both sides.

TERM1.2 Granularity: VM-WIDE ONLY. Raising termination = the
sect A.2.3 rule-3 VM-wide form: under the registry lock, set the
termination bit in EVERY lite of the target VM (sect A.1.3
filter) + the VM word; token acquisition ORs it in. The rule-3
"Per-thread: one lite" arm exists for genuinely per-thread traps
(per-lite stop tickets, sect A.3; debugger/watchdog carrier-only
bits, sect A.2.7-8) and NEVER carries the termination bit - there
is NO mechanism in v1 for raising termination on exactly one
lite. Consequences (binds the SD8/U19 corpus arms): terminating
the VM terminates EVERY entered thread; a sibling parked in
Atomics.wait takes the Terminated arm (api 5.6-4
throwTerminationException) and then ALSO closes per sect E.5;
main's in-flight JS unwinds with the termination exception to
the host. The VM is NOT destroyed: the carrier host services the
termination (watchdog shouldTerminate callback / embedder clears
the trap per the landed VMTraps protocol) and may re-enter;
join()s performed after re-entry observe Phase::Failed. sect
A.2.4's park-poll predicate re-pointing (PARK lite) is unchanged
- the bit it polls was fanned VM-wide.

TERM1.3 Failed payload + join (SD8 ext2). The sect E.5 close of
a terminated thread publishes into the landed F1/F5 result
Strong a FRESH ordinary Error with message "Thread terminated",
allocated native-side at close (thread entered, with access, no
JS runs) - NEVER vm.m_terminationException (deliberately
cross-thread/sticky: vmstate "Deliberately NOT in
VMLitePrimitives" list; K4 traps row "sticky release-publish";
rethrowing IT would re-terminate the joiner, contradicting sect
E.5's own main-fallback drain assumption). join() rethrows this
Error as a NORMAL catchable exception (api F1/I3 identity rule
applies to it); asyncJoin's promise rejects with it. If the
close itself cannot allocate (OOM), fall back to the landed
OOM-failure shape for F1/F5. Corpus (U19 terminate arms +
U-T11): join-after-termination catches an ordinary Error
(joiner continues executing); asyncJoin rejection observed;
GILOn/GILOff variants per the SD8 pattern.

TERM1.4 Discriminator notes (walkthrough finding 4).
VMLite::isSpawned is written ONLY in the sect B.1 spawn path
(=1 BEFORE setCurrent); carrier lites never set it. Because
spawned threads are single-VM (TERM1.5), a spawned thread's
CURRENT lite is always its own spawn lite, so the sect I JIT
prologue byte check and the C++ isJSThreadCurrent() gates agree
at every site, as does sect A.2.7's carrier-only exemption.
Predicate keying: unqualified "gilOff" in SPEC-ungil's sect
C/I/N rulings = vm.m_gilOff (the sect A.1.3 level (ii), per-VM);
gilOffProcess (level (i)) is always NAMED where meant. sect
C.4's lifted TA gate reads vm.m_gilOff; spawned threads exist
only in the m_gilOff VM (U0b), so the two keyings coincide on
every reachable path.

TERM1.5 sect F.5 caller scope (walkthrough finding 3). The sect
F.5 access-release/LIFO-restore protocol applies to
MAIN/EMBEDDER CARRIERS ONLY. A spawned Thread that reaches
JSLock::lock()/entry on ANY other VM RELEASE_ASSERTs, per annex
A36's "Spawned Threads single-VM in v1 (foreign-VM token
RELEASE_ASSERTs)" - A36 stands unamended [r30: A36's
~VM-teardown clause has since been AMENDED (rev-30 A36 amendment
record); THIS single-VM clause stands]. Reconciliation with
r10 F2: F2's deadlock walk and its rejection of "option (b)
(RELEASE_ASSERT)" concern the Bun JSContext-inside-host-call
pattern, which executes on a main/embedder carrier thread;
nothing in r10 F2 licenses SPAWNED nesting, and A36 (BINDING)
forbids it. The refusal is a process-abort RELEASE_ASSERT (with
message naming sect F.6(e)), NOT a catchable error: it is an
embedder-contract violation - native modules must not
create/enter other VMs/JSContexts on spawned Threads in v1 - and
pure JS cannot reach it (a Thread fn cannot enter another VM
without native code). Not an SD (no JS-observable behavior
defined or changed). Post-ungil: revisit as a catchable
TypeError if a real embedder pattern needs spawned nesting.
Corpus: U27/U-T6 gain a spawned-foreign-VM death-test arm
(EMBEDDER/API-level, expects the assert message).

TERM1.6 IU creation rule (walkthrough finding 6).
INTEGRATE-ungil.md does NOT exist in the frozen set (this
design workflow's write set is {UNGIL-PLAN, SPEC-ungil,
SPEC-ungil-history} only). IU is CREATED AT U-T1 as the landing
ledger, schema per the INTEGRATE-* house pattern, and MUST
contain at least: (i) the supersession ledger - one row per
SPEC-ungil SUPERSESSION, citing both sides per the master rule
(the spec side is already written; the IU side is written at
landing); (ii) the sect F.2 predicate-consumer table (U-T8,
~60 rows: assert/BRANCH/EXCLUSIVITY CONSUMER); (iii) the sect
E.4 settle-site lock-context table (U-T8); (iv) the sect A.1.7
off-thread-reader table (U-T8d, per rerouted field); (v) the
sect E.1b.4 hook-disposition table (U-T8e: {inline,
carrier-queued, refused, unreachable}); (vi) the sect F.6
embedder checklist incl. the (d) construction-order and (e)
spawned-no-foreign-VM audits; (vii) the per-row call-site
enumerations that annex K4/N7 rows defer to IU. Until U-T1,
every "IU row" citation in the spec and audits is an OBLIGATION
on the landing task to write that row; the audits' "Implementation
CONSUMES the ... table verbatim" refers to the EXECUTED K4/N7
tables shipped at r26 - IU adds call-site enumeration only and
NEVER re-rules a K4/N7/TERM1 disposition.

### rev-27 spec deltas
- sect A.2.4 REWRITTEN (VM-wide only; no terminate API; TERM1
  pointer). sect E.5 gains the TERM1.2/1.3 paragraph (SD8 ext2).
- sect F.5 gains CALLER SCOPE; sect F.6 "Four" -> "Five" deltas,
  new (e), sign-off list (a)/(c)/(e).
- sect I gains the isSpawned note; sect A.1.3 gains the
  unqualified-gilOff rule; sect C.4 "gilOff-conditional" ->
  "vm.m_gilOff-conditional".
- sect IM gains the IU creation rule; sect N.7 "IU table" ->
  "N7 table (sect IM: IU adds call sites)".
- SD8 entry cites r27 ext2; per-rev SD attribution moved here:
  r17/r19-r23 none; r18=SD16; r24=SD17; r25 none; r26=SD18+SD19;
  r27 none (ext2 rides SD8).
- Status header rev 27; sect T r10-r27.

### rev-27 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays in
### the cited BINDING annex/rev; pointer targets + supersession
### rows untouched)
sect A.2.8 (annex W/W ext: dropped "services shouldTerminate"
detail + W4 assert predicate - annex W ext keeps both), sect
K.6 + sect N.9 (ANNEX AUD1), sect LK WS rows (ANNEX WS1), sect
A.3.5 (HBT2-HBT4), sect A.3.6 (A36/A36C), sect C.1 (annex C1),
sect D.1 (D1/D1R: dropped arm-name parentheticals), sect E.2
(E2A pseudocode VERBATIM: dropped listLock-dequeue/rank-3 +
drop-poll-reacquire clauses), sect E.1b.5 (ALS1), sect E.7.3
(r8/E7), sect E.7.5, sect K.3 (LZ1/LZ2), sect F.1 (F1B), sect
F.4 (DAL2: dropped the RHA/AHA sentence - DAL2.5 keeps it,
sect F.6(c) still cites it), sect A.1.7, sect A.1.1 (jit App.
R5), sect A.3.7 (re-verify note), sect B.5 note, sect N.5/N.6
cite tails, sect T index parentheticals, SD one-liner
tightening, header line. Reminder: amplifier/arm details
trimmed from sect D.1/sect N.5 remain normative in their
annexes (D1R, N7) and the rev-26 section-T delta list.

### rev-27 corpus/arm deltas (extends rev-9 annex 3 + r10-r26)
- U19 terminate arms: assert VM-wide semantics (parked sibling
  ALSO terminated + closes Failed), join-after-termination
  rethrows ordinary Error "Thread terminated", asyncJoin
  rejects with it; GILOn/GILOff variants per SD8.
- U-T11: + terminated-join rethrow arm (SD8 ext2).
- U27/U-T6: + spawned-foreign-VM RELEASE_ASSERT death test
  (sect F.6(e)/TERM1.5).
- U-T1: + IU skeleton creation w/ TERM1.6 tables (i)-(vii).

# REV 28 (2026-06-06) - adversarial review round: 1 major
# (exit-during-stop-window lifetime hole)

Finding (fresh adversarial reviewer): sections A.3.1-.2 / B.2 /
annex E2A's close path carried NO lifetime rule for a lite/client
destroyed inside a live section-A.3 stop window. Thread EXIT was
never gated on the stop bit - only ACQUISITION is (A.3.2b gates
AHA/attach; A.3.4 gates entry) - and that is deliberate (no park
point on exit). But after E2A's deadline harvest, the section-B.2
T5 sequence (access release -> DCT -> destroy GCClient::Heap ->
unregisterLite, which frees the VMLite) ran entirely
access-released with no further coordination, so a spawned thread
could complete teardown inside an open window while SB1.2 obliges
the conductor to keep issuing seq_cst per-client/per-lite
access-state samples - and SB1.1 said the VMLiteRegistry lock is
"retained for ENUMERATION only". Nothing said (a) whether the
conductor may cache lite/client pointers across predicate
samples, (b) that destruction is forbidden/deferred while
sampled, or (c) WHICH lock (VMLiteRegistry::lock vs
VMManager::m_worldLock) owns the entered set the predicate
samples. Interleaving killed: conductor C fans stop bits and
snapshots the entered set with raw lite*/client* pointers; T_exit
(already past its last gated re-acquire) releases access at T5
(un-gated), DCTs, destroys its client, unregisterLite frees the
lite; C's next seq_cst sample dereferences freed memory. Spec
bumped rev 27 -> rev 28; sole change is ANNEX EXIT1 + its body
pointers/compressions. No new SD.

## ANNEX EXIT1 (BINDING) - exit-during-stop-window lifetime:
## per-sample registry re-enumeration + deregister-before-destroy
## (amends sections A.3.1, A.3.2, B.2, annex E2A's close tail and
## INV U3; SUPERSEDES annex SB1 item 1's "ENUMERATION only"
## clause, both sides - here and an [r28] marker at SB1.1)

[r29: THIS ANNEX IS AMENDED - the annex of record is the "# REV
29" ANNEX EXIT1 below. EXIT1.3-1.5 and 1.7-1.8 are superseded
where they differ: the T5 PHYSICAL unregisterLite returns to
LAST (after client destroy - restores the vmstate 6.5.1/A36 ~VM
registration fence rev 28 silently stripped); the LOGICAL
removal becomes the TEARDOWN mark (registry lock); TEARDOWN
counts EXITED, its access re-acquire FORBIDDEN. EXIT1.1-1.2 and
1.6 carry over verbatim modulo the TEARDOWN clauses.]

EXIT1.1 Set identity (resolves ambiguity (c)). The entered-thread
set the section-A.3.2 conductor predicate samples IS the
VMLiteRegistry (vmstate 6.5.1), filtered lite->vm in the target
VM set (section A.1.3 filter). forEachEnteredThread(VM&, f) /
numberOfEnteredThreads are REGISTRY WALKS. VMManager::m_worldLock
(heap rank 3) serializes world transitions and conductor tenure
but owns NO membership: there is no second entered-thread
structure, hence no two-structure consistency protocol to state.

EXIT1.2 Per-sample re-enumeration (SUPERSESSION, both sides:
annex SB1 item 1's "retained for ENUMERATION only and carries no
ordering duty" - the registry lock now OWNS THE SAMPLED SET FOR
THE LIFETIME OF EVERY OPEN section-A.3 WINDOW; its no-ordering
duty for the stop-bit/access Dekker pair STANDS - the SB1.4
seq_cst proof is unchanged and the lock carries the LIFETIME duty
only). Normative: every conductor predicate sample RE-WALKS the
registry UNDER VMLiteRegistry::lock; lite/client pointers are
NEVER cached across samples (including from the section-A.2.3
fan-out walk - the fan-out enumeration is one walk, each
subsequent sample is a fresh walk); every SB1.2 seq_cst
access-state load executes INSIDE the lock hold of the walk that
found that lite; the walk is allocation-free, acquires nothing
(section LK.6 inner set suffices for nothing here - the walk
takes NO inner lock), and the registry lock is DROPPED before the
conductor blocks/yields between samples (registry-lock holders
never wait, vmstate I7 class).

EXIT1.3 Deregister-before-destroy (amends section B.2 + annex
E2A close tail + INV U3, both sides; NO exit gating added). On
EVERY lite/client teardown path - spawned T5, carrier TLS-death,
the ~VM walk (section A.3.6/M6 annex) - VMLiteRegistry::
unregisterLite(lite) (under the registry lock) STRICTLY PRECEDES
DCT, GCClient::Heap destruction, and the VMLite free. T5 order
becomes: Strong clears -> access release (seq_cst RHA, F8) ->
unregisterLite -> DCT -> destroy GCClient::Heap -> free lite.
Exit remains UN-GATED: no stop-bit poll, no park point, no new
deadlock edge; E2A's close sequence BEFORE T5 (deadline harvest,
residue routing, F1/F5) is unchanged. vmstate 6.5.1's lifetime
contract (unregistered before destroyed) is PRESERVED and
strengthened (now also before CLIENT destroy); vmstate N8's
"unregister under the final JSLock hold" clause is the GIL-on/
carrier shape and is untouched (GIL-off spawned threads hold no
m_lock, section F.1); ~VM (VM.cpp:659) already complies
(unregisters before destroying m_mainVMLite).

EXIT1.4 Predicate disposition of a removed/clientless lite.
(a) A lite ABSENT from the current walk counts as EXITED.
Soundness: unregisterLite is reachable only AFTER the exit path's
seq_cst access release (EXIT1.3 order), and the unregisterLite
unlock happens-before the missing-it walk's lock acquire, so a
conductor that no longer sees the lite has its NoAccess
release ordered before the sample; re-entry requires FRESH
registration + section-A.3.4-gated token acquisition (the VM stop
word, ORed in at acquisition per section A.2.3, gates entrants
that registered AFTER the fan-out walk - they park before
completing entry and appear in later walks as not-entered).
(b) lite->clientHeap is written ONCE per registration epoch
(section B.1 spawn / F.1 first carrier entry), with a release
store, BEFORE the thread's first access acquisition, and is never
nulled or repointed while the lite is registered. A sampler
reading null counts the lite not-entered/no-access - sound:
access cannot be held without a client, and acquisition is
A.3.2b-gated. A sampler reading non-null under the walk's lock
hold dereferences a live client (EXIT1.3: destroy is fenced
behind removal, and removal waits for the walk's lock).

EXIT1.5 Why the interleaving dies. Every conductor dereference of
a lite/client happens inside a registry-lock hold of a walk that
found the lite registered. T_exit's unregisterLite must WAIT for
any in-progress walk to drop the lock; after it runs, no later
walk sees the lite; DCT/client-destroy/lite-free are program-
ordered after unregisterLite returns. So no sample ever touches
freed memory - achieved with zero new park points on exit and no
change to the SB1 ordering proof.

EXIT1.6 Lock-order argument (section LK; no rank change). The
conductor holds VMManager::m_worldLock (heap rank 3, inside the
LK.5 frozen heap block) for the window (section A.3.1) and
acquires VMLiteRegistry::lock (LK.6) per sample: strictly
outer -> inner in the LK order, acyclic. Registry-lock holders
acquire nothing and never wait (LK.6 inner set untouched by the
walk; vmstate I7 class), so no new edge appears in either
direction; the LK.6 fastMalloc-excluded-while-suspended carve-out
is unaffected (section-A.3 conductors suspend nobody - that
carve-out belongs to section-A.1.7 readers). Exit side:
unregisterLite at T5 runs access-released holding NO api or heap
lock (E2A close dropped inboxLock before T5) - no new edge. The
section-A.2.3 fan-out walk already took the registry lock; its
rank position is unchanged.

EXIT1.7 INV + amendment record.
- NEW INV U32: no VMLite or GCClient::Heap is freed while
  reachable from any section-A.3 fan-out or predicate-sample
  registry walk - registry removal precedes DCT/client-destroy/
  lite-free on every teardown path, and conductors hold no
  lite/client pointer across sample boundaries.
- INV U3 AMENDED (both sides; rev-9 annex 1 text "Strong clears
  -> access release -> DCT -> unregisterLite" superseded):
  lifecycle order is now "lite -> ACT -> alloc; Strong clears ->
  access release -> unregisterLite -> DCT -> destroy client ->
  free lite" (EXIT1.3).
- INV U4 gains the EXIT1.8 exit-storm arm.

EXIT1.8 Tests + lint.
- Corpus/litmus (U-T5 + U-T6, U4 arm): EXIT-STORM-UNDER-STOP-
  STORM - N threads spawn, run briefly, and exit in a tight loop
  while a conductor thread fires back-to-back section-A.3 stops
  (Class-A fire or a synthetic test-only conductor); ASAN + TSAN
  clean; race-amplifier variant injects delays between every T5
  step (post-release, post-unregister, post-DCT) and inside the
  conductor's between-sample gap. Carrier variant: embedder
  TLS-death teardown racing a stop window.
- U20 lint: section-A.3 conductor code must reach lites ONLY via
  the forEachEnteredThread registry-walk helper; any lite*/
  client* value in conductor code that crosses a sample boundary
  (escapes the walk's lock scope) is flagged; unregisterLite
  call sites are checked to precede DCT/client-destroy on their
  path.

### rev-28 spec deltas
- sect A.3.1 gains the EXIT1 index (set = registry; per-sample
  re-walk under VMLiteRegistry::lock, inner to m_worldLock; no
  caching; absent => exited; clientHeap null => not-entered).
- sect A.3.2b's SB1 cite -> "ANNEX SB1 as AMENDED by EXIT1".
- sect B.2 T5 order rewritten per EXIT1.3 (unregisterLite before
  DCT/destroy; free last; all teardown paths; un-gated).
- sect INV gains U32 (+ U3 amendment pointer); sect SD per-rev
  attribution gains "r28 none"; sect T cites r10-r28; header
  rev 28.

### rev-28 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays in
### the cited BINDING annex/rev; pointer targets + supersession
### rows untouched)
- sect A.3.2b (ANNEX SB1): "acq/rel UNSOUND (store-buffering)"
  -> "acq/rel UNSOUND" (SB1.4 keeps the SB shapes).
- sect A.3.3 (ANNEX HBT4): "NEVER raw GCL (heap sect 10.4/sect
  A.3.8 never wait on it - HBT4.3)" -> "NEVER raw GCL (HBT4.3)".
- sect A.3.5 (ANNEXES HBT2-HBT4): (ii) "in-window GC FORBIDDEN;"
  dropped (the clause name + annex carry it); (i) "own-client F8
  AHA re-acquire BEFORE fanning bits" -> "own-client gated
  re-acquire pre-fan".
- sect A.3.6 (ANNEXES A36/A36C): "(nonzero TID, lazy)" ->
  "(lazy)" (A36 keeps nonzero-TID); "BEFORE alloc/OM fast path +
  gated AHA" -> "pre-fast-path + gated AHA".
- sect 0/U0c (ANNEX U0C): "designation =
  Heap::tryDesignateStickySharedServer() CAS" -> "the U0C CAS".
- sect A.1.3 (r6 F5): GC-roots filter "ONLY lites with lite->vm
  == collecting VM; same filter" -> "per-VM filter; ditto".
  [r29: full-text cite corrected - the per-VM filter's home is
  rev-8 item 11; r6 F5 carries the PRE-FIX unfiltered walk (cite
  it for walk mechanics only). Spec body re-pointed.]
- sect A.1.6 (ANNEX A16): "(all tiers incl. OSR-exit)" -> "(all
  tiers)" (A16 keeps OSR-exit).
- sect A.1.7 (r9 F7 + r24): "samples carrier lites via (i) only"
  -> "= (i), carrier lites only"; "(fastMalloc incl.)" dropped.
- sect A.2.6 (ANNEX A26): "(GIL-on: VM-wide rule-4 form;
  GIL-off: the rule-4 PARK lite's bit)" -> "(rule-4; PARK lite
  GIL-off)".
- sect A.2.8 (ANNEX W/W ext): "tokenless timer + rule-3
  termination" -> "tokenless timer" (W3 keeps the rule-3 form);
  "the four Watchdog.cpp APILock asserts" -> "the four APILock
  asserts" (W4 keeps the file + line cites).
- sect C.1 (ANNEX C1): "indexed by shape (CoW I35; convert; sect
  C.2 parseIndex)" -> "indexed by shape (C1)".
- sect C.3 (ANNEX C3): "(NO alloc/STW under listLock)" dropped.
- sect D.1 (ANNEXES D1/D1R): "(heap sect 10 barrier - NOT sect
  A.3, jit R1.h)" -> "(heap sect 10, NOT sect A.3)";
  "(conductor takes NO api lock; r9 F2)" -> "(r9 F2)".
- sect E.1b.4 (r16 F3): "(no JS)" dropped.
- sect E.2 (ANNEX E2A): EXPIRE "(r12; sect E.4 "timed-out",
  rule-1 decrement)" -> "(sect E.4 "timed-out")"; close
  "(closed => main fallback, SD8 ext)" -> "(SD8 ext)"; tail "F1/
  F5 + T5 access release as landed (sect B.2, U3)" -> "F1/F5 as
  landed; T5 per sect B.2 (EXIT1.3)" (also the EXIT1.3
  amendment pointer).
- sect E.3 (ANNEX E3): "(SD11; sect E.7.5 = PROPERTY only)" ->
  "(SD11)".
- sect E.4 (r17 F6/r18 F2): "(r18 F2; closure monotonic sect E.3
  r3; sect E.7.3-4 apply)" -> "(r18 F2; sect E.7.3-4 apply)".
- sect E.7.3 (r8/E7/r17 F3/r18 F2): wake constraints "no
  rank-1..3 lock, no JS, never reenters JSC (boot-checked)" ->
  "(constraints per annex)".
- sect E.7.5 (SD16/r18 F4): "(sect E.1; expiry = sect E.2 EXPIRE
  or close harvest; sect C.3 holds keepalive)" -> "(sect E.1;
  sect C.3 holds keepalive)".
- sect F.1 (ANNEX F1B): "gated AHA on THAT client (sect A.3.2b/
  sect A.3.8)" -> "gated AHA on THAT client".
- sect F.4 (ANNEX DAL2): ctor "(F8 mandatory-revert)" dropped.
- sect F.5 (r10 F2): "(F8 mandatory-revert)" dropped.
- sect F.6(d) (ANNEX EC1): "= the ONLY spawn-capable VM for
  PROCESS LIFETIME (others spawn-RangeError, U0b)" -> "= sole
  spawn-capable VM (U0b)".
- sect J.3 (r10 F5): "(api 5.9(e); NLS::m_lock exempt, sect
  E.2)" -> "(sect E.2 exemption)".
- sect K.3 (LZ1/LZ2): "(GIL-off only, not an SD)" -> "(not an
  SD)" (LZ1 keeps the mode scoping).
- sect K.5 (ANNEX HBT): "isHavingABadTime() re-checked
  post-arbitration" -> "re-check post-arbitration".
- sect K.6 (ANNEX AUD1): "losers adopt/PARK-CAPABLE wait
  access-released" -> "losers adopt/wait per AUD1.K3".
- sect N.5 (r11/r15 F1/r25 ext): "(one CAS per await/yield)"
  dropped.
- sect N.6 (ANNEX N6): "DETACH length=0" -> "DETACH len=0".

### rev-28 section-T deltas (extends rev-9 annex 3 + r10-r27)
- U-T5: conductor predicate implemented as EXIT1.2 per-sample
  registry walks (forEachEnteredThread helper; no pointer caching
  across samples); + the EXIT1.8 exit-storm-under-stop-storm
  litmus/amplifier arm; U20 lint extended per EXIT1.8.
- U-T6: T5 teardown reordered per EXIT1.3 (unregisterLite ->
  DCT -> destroy client -> free lite) on ALL paths - spawned T5,
  carrier TLS-death, ~VM walk (audit: ~VM already complies);
  clientHeap write-once release-publish (EXIT1.4(b)); + the
  carrier TLS-death-vs-stop-window arm.
- No other task scope changes; U4's arm list grows per EXIT1.7.

### Rev-28 SD note
No new SDs; IDs frozen. EXIT1 is lifetime/ordering only - no
JS-observable behavior changes.

# REV 29 (2026-06-06) - reviewed-findings round vs rev 28's
# ANNEX EXIT1: 1 blocker + 3 majors, all fixed; nothing else
# changed

Round record (4 findings):

F1 (BLOCKER, design change). rev 28's T5 order (Strong clears ->
access release -> unregisterLite -> DCT -> destroy client -> free
lite) silently stripped the registration-based VM-lifetime fence:
pre-r28 the lite stayed registered through DCT/client-destroy, so
vmstate §6.5.1's "~VM asserts registry empty for this VM"
(SPEC-vmstate.md:519-521; the VM.cpp:654-658 walk; annex A36)
fenced the whole heap-touching teardown tail. Post-r28, between
unregisterLite and DCT, the exiting thread was invisible to that
walk yet still dereferenced the server JSC::Heap (DCT,
~GCClient::Heap) and VM::m_microtaskQueues (the M12 removal in
the lite free). join() notifies BEFORE T5 (ThreadObject.cpp:
236-244; unregister at :259) and api §4.6.1 has no implicit join
- embedder-destroys-VM-after-join raced the T5 tail (UAF). FIX:
LOGICAL removal (new TEARDOWN lite state, marked under
VMLiteRegistry::lock at T5) supplies r28's conductor semantics;
PHYSICAL unregisterLite returns to the old position, LAST (after
client destroy), restoring the ~VM fence verbatim. Composition
verified: the r28 conductor-UAF fix is preserved (r28's UAF was
freed-lite memory; with the lite registered until after
client-destroy, walk samples touch live memory by construction).
Full amended text: ANNEX EXIT1 below - the annex of record;
[r29] markers placed at the rev-28 annex and at E2A's [r28] tail
marker, both sides. New test arm: T5 tail vs ~VM /
join-then-destroy-VM (EXIT1.8; U-T6 gate list).

F2 (major). The §A.1.3 GC-roots compression cited "history r6
F5" as the full-text home of the per-VM filter, but r6 F5
carries the PRE-FIX unfiltered walk; the filter's home is rev-8
item 11. Spec-body cite fixed (r8 item 11; r6 F5 = walk
mechanics only); the rev-28 compression-record row carries an
in-place [r29] correction marker.

F3 (major). rev-28 EXIT1.7 claimed the INV U3 amendment was
recorded both sides, but rev-9 ANNEX 1's U3 row was unmarked. An
in-place [r29] marker added there (SB1.1/E2A marker style).

F4 (major). The handout's EXIT1 inline claimed "full text" but
dropped 4 clauses vs the history annex (the §A.1.3-filter cite
in EXIT1.1; EXIT1.2's "§LK.6 inner set ... walk takes NO inner
lock" clause; EXIT1.3's "§A.3.6/M6 annex" cite +
~VM-already-complies rationale; EXIT1.6's §A.2.3 fan-out
sentence). Handout regenerated at rev 29: EXIT1 re-inlined
VERBATIM from the amended annex below (diff-verified; heading
level is the only permitted delta).

Spec bumped rev 28 -> rev 29; sole normative change is ANNEX
EXIT1 as amended + its body pointers/compressions. No new SD.

## ANNEX EXIT1 (BINDING, as AMENDED by rev 29 - the annex of
## record; the rev-28 text is superseded where they differ) -
## exit-during-stop-window lifetime: per-sample registry
## re-enumeration + TEARDOWN-mark-before-destroy + physical
## removal LAST (amends §A.3.1, §A.3.2, §B.2, annex E2A's close
## tail and INV U3; SUPERSEDES annex SB1 item 1's "ENUMERATION
## only" clause, both sides)

[r30: THIS ANNEX IS AMENDED - the annex of record is the "# REV
30" ANNEX EXIT1 below. The fence claim here ("trips the
registration fence, caught contract violation") was
assert-only/debug-only; r30 adds the EXIT1.9 ~VM completion
fence (a NORMATIVE blocking wait) and scopes EXIT1.3 to live-VM
paths (the ~VM carrier collection follows A36 as AMENDED r30).
EXIT1.1-1.2 and 1.4 carry over verbatim.]

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
the VM.cpp:654-658 walk; annex A36) fenced it; post-r28, between
unregisterLite and DCT, the exiting thread was invisible to that
walk yet still dereferenced the server JSC::Heap (DCT,
~GCClient::Heap) and VM::m_microtaskQueues (the M12 removal in
the lite free). join() notifies BEFORE T5 (ThreadObject.cpp:
236-244) and api §4.6.1 has no implicit join, so
embedder-destroys-VM-after-join raced the T5 tail - UAF. rev 29
splits LOGICAL from PHYSICAL removal: a TEARDOWN lite state
supplies r28's conductor semantics; the physical removal returns
to the old position (LAST), restoring the ~VM fence verbatim.

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
(r29; amends §B.2 + annex E2A's close tail + INV U3, both sides;
NO exit gating added; REPLACES rev 28's unregisterLite-first
order). On EVERY lite/client teardown path - spawned T5, carrier
TLS-death, the ~VM walk (§A.3.6/M6 annex) - LOGICAL removal
precedes any destruction and PHYSICAL removal comes LAST: under
VMLiteRegistry::lock the exiting thread marks its lite TEARDOWN
(one registry-owned lite-state byte; the lite stays PHYSICALLY
registered; conductors count it EXITED per EXIT1.4(a)); THEN DCT
and GCClient::Heap destruction; THEN VMLiteRegistry::
unregisterLite(lite) (under the registry lock), which frees the
VMLite. T5 order becomes: Strong clears -> access release
(seq_cst RHA, F8) -> TEARDOWN mark (registry lock) -> DCT ->
destroy GCClient::Heap -> unregisterLite/free lite. The
registration-based VM-lifetime fence is thereby restored
VERBATIM: the registry stays non-empty for this VM until the
heap-touching tail (DCT, ~GCClient::Heap, the M12
m_microtaskQueues removal in the lite free) is done, so the ~VM
registry walk / A36 "registry empty for this VM" assert
re-covers the tail with NO new mechanism. Exit remains UN-GATED:
no stop-bit poll, no park point, no new deadlock edge; E2A's
close sequence BEFORE T5 (deadline harvest, residue routing,
F1/F5) is unchanged. vmstate §6.5.1's lifetime contract
(unregistered before destroyed) is PRESERVED (physical removal
still strictly precedes the lite free); vmstate N8's "unregister
under the final JSLock hold" clause is the GIL-on/carrier shape
and is untouched (GIL-off spawned threads hold no m_lock, §F.1);
~VM (VM.cpp:659) already complies (unregisters m_mainVMLite
after the per-VM teardown walk, before destroying it; no
TEARDOWN mark needed there - a VM inside ~VM has no live
conductors, embedder contract §F.6).

EXIT1.4 Predicate disposition of a TEARDOWN/absent/clientless
lite. (a) A lite marked TEARDOWN - and a lite ABSENT from the
current walk - counts as EXITED. Soundness (r29 re-stated): the
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
SB1 ordering proof. Embedder-destroy-vs-T5 (the r29 BLOCKER):
the exiting thread stays registered through the whole
heap-touching tail, so ~VM's per-VM registry walk (vmstate
§6.5.1; VM.cpp:654-658) still sees the TEARDOWN lite and the
fence holds - join-then-destroy-VM trips the registration fence
(caught contract violation) instead of silently racing the tail.

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
its rank position is unchanged.

EXIT1.7 INV + amendment record.
- INV U32 (r29 form): no VMLite or GCClient::Heap is destroyed
  or freed while observable to any §A.3 fan-out or
  predicate-sample registry walk as a live (non-TEARDOWN) lite -
  the TEARDOWN mark precedes DCT/client-destroy, physical
  removal + lite free come LAST, and conductors hold no
  lite/client pointer across sample boundaries; no lite leaves
  the registry before its heap-touching teardown tail completes
  (the ~VM fence).
- INV U3 AMENDED (both sides; the rev-9 annex 1 row carries the
  [r29] marker; rev 28's amended form re-amended): lifecycle
  order is now "lite -> ACT -> alloc; Strong clears -> access
  release -> TEARDOWN mark (registry lock) -> DCT -> destroy
  client -> unregisterLite/free lite" (EXIT1.3).
- INV U4 gains the EXIT1.8 exit-storm arm.

EXIT1.8 Tests + lint.
- Corpus/litmus (U-T5 + U-T6, U4 arm): EXIT-STORM-UNDER-STOP-
  STORM - N threads spawn, run briefly, and exit in a tight loop
  while a conductor thread fires back-to-back §A.3 stops
  (Class-A fire or a synthetic test-only conductor); ASAN + TSAN
  clean; the race-amplifier variant injects delays between every
  T5 step (post-release, post-mark, post-DCT, post-destroy) and
  inside the conductor's between-sample gap. Carrier variant:
  embedder TLS-death teardown racing a stop window.
- T5-TAIL-VS-~VM arm (r29; joins the U-T6 gate list): embedder
  join()s a spawned thread then immediately destroys the VM
  while the amplifier stalls the joined thread inside the T5
  tail (post-mark pre-unregister; variants mid-DCT and
  mid-client-destroy): the ~VM registry walk must observe the
  still-registered TEARDOWN lite (fence holds; no UAF on the
  server Heap or VM::m_microtaskQueues); ASAN clean.
- U20 lint: §A.3 conductor code must reach lites ONLY via the
  forEachEnteredThread registry-walk helper; any lite*/client*
  value in conductor code that crosses a sample boundary
  (escapes the walk's lock scope) is flagged; teardown paths are
  checked for TEARDOWN-mark-precedes-DCT/client-destroy AND
  unregisterLite-LAST (after client destroy, immediately before
  the lite free).

### rev-29 spec deltas
- header rev 29; sect T cites r10-r29; sect SD per-rev
  attribution "r27-r29 add none".
- sect A.1.3 GC-roots cite -> "r8 item 11; walk r6 F5" (F2).
- sect A.3.1 EXIT1 index: annex cite -> "as AMENDED r29"; "lite
  absent OR TEARDOWN => EXITED (re-acquire FORBIDDEN)"; U4 arm
  list gains the ~VM-race arm.
- sect B.2 item 2 T5 order rewritten per EXIT1.3 as amended
  (TEARDOWN mark -> DCT -> destroy client -> unregisterLite/free
  LAST; ~VM fence restored).
- INV U32/U3 text = EXIT1.7 as amended (IDs frozen).

### rev-29 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays in
### the cited BINDING annex/rev; pointer targets + supersession
### rows untouched)
- sect A.3.1 (ANNEX EXIT1): "(write-once release-publish per
  registration, §B.1/§F.1)" -> "(write-once release-publish,
  §B.1/§F.1)" (EXIT1.4(b) keeps the per-epoch form).
- sect A.3.2b (ANNEX SB1): "stop-bit fan-out stores, conductor
  access samples + the AHA/..." -> "fan-out stores, conductor
  samples + AHA/..." (SB1.1-3 keep the full op list); "(r9 F3;
  ordering per SB1)" -> "(r9 F3)" (SB1.3 keeps the position).
- sect A.3.3 (ANNEX HBT4): "BINDING - promotes HBT3 item 3;
  ALL..." -> "BINDING; ALL..." (HBT4 records its provenance).
- sect B.2 (ANNEX EXIT1): "Exit stays UN-GATED (no stop-bit
  poll/park point)" -> "Exit stays UN-GATED" (EXIT1.3 keeps the
  clause).
- sect 0/U0c (ANNEX U0C): "noteSharedServerSticky() (I13
  UNCHANGED)" -> "noteSharedServerSticky()" (U0C keeps it).
- sect E.2 (ANNEX E2A): "residue DWT retired + routed to main
  (E.4 dead rule)" -> "residue per the E.4 dead rule" (E2A keeps
  the retirement steps).
- sect E.5 (ANNEX TERM1): "rethrows it NORMALLY (joiner not
  re-terminated)" -> "rethrows it NORMALLY" (TERM1.3 keeps it).

### rev-29 section-T deltas (extends rev-9 annex 3 + r10-r28)
- U-T5: predicate disposition per EXIT1.4 as amended (TEARDOWN
  counts EXITED; the forbidden-TEARDOWN-re-acquire assert); the
  EXIT1.8 lint extension re-worded (TEARDOWN-mark-precedes-
  destroy + unregisterLite-LAST).
- U-T6: T5 teardown re-reordered per EXIT1.3 as amended
  (TEARDOWN mark -> DCT -> destroy client -> unregisterLite/free
  lite) on ALL paths - spawned T5, carrier TLS-death, ~VM walk
  (audit: ~VM still complies); + the EXIT1.8 T5-TAIL-VS-~VM /
  join-then-destroy-VM race arm joins the U-T6 gate list.
- No other task scope changes; U4's arm list per EXIT1.7.

### Rev-29 SD note
No new SDs; IDs frozen. r29 is lifetime/ordering only - no
JS-observable behavior changes.

# REV 30 (2026-06-06) - reviewed-findings round vs rev 29's
# ANNEX EXIT1: 1 blocker + 1 major, both fixed; nothing else
# changed

Round record (2 findings):

F1 (BLOCKER, design change). rev 29's restored ~VM registration
fence was ASSERT-ONLY and DEBUG-ONLY: EXIT1.5/1.8 claimed
join-then-destroy-VM "trips the registration fence (caught
contract violation)", but the cited ~VM walk (VM.cpp:651-657) is
#if ASSERT_ENABLED - a RELEASE build retained the UAF verbatim
(T_exit marks TEARDOWN, stalls mid-DCT; the embedder's join()
already returned - settle precedes unregister,
ThreadObject.cpp:240-246 vs :259, and api §4.6.1 has no implicit
join; ~VM destroys the server Heap; T_exit resumes DCT against
the freed heap, then the M12 removal against a destroyed VM),
and a debug build aborted nondeterministically on a pattern the
embedder cannot avoid (join is its only completion signal and
fires pre-T5). The r29 EXIT1.8 T5-TAIL-VS-~VM arm was incoherent
as written (release: walk compiled out, UAF, ASAN not clean;
debug: "observing" = crashing). FIX (NEW EXIT1.9, NORMATIVE): ~VM
BLOCKS until no registered lite other than m_mainVMLite has
lite->vm == this - a WTF::Condition (vmTeardownCondition) on
VMLiteRegistry, waited under VMLiteRegistry::lock, signaled by
unregisterLite (already under that lock). The wait is the
NORMATIVE completion fence; the assert walk is DEMOTED to a
post-wait debug sanity check; ThreadObject's join settle order
is UNCHANGED (no implicit join; embedders need no new contract).
Acyclicity restated normatively in EXIT1.6: ~VM holds only the
API lock there; the T5 tail runs access-released holding no api
or heap lock and acquires only the leaf registry lock, which
Condition::wait drops into the parking lot while parked - the
waited-on thread always makes progress. The EXIT1.8
T5-TAIL-VS-~VM arm is REWRITTEN coherently (release+ASAN
load-bearing arm; debug adds the sanity walk). U-T6 scope + gate
list updated.

F2 (MAJOR, A36 reconciliation). rev 29's EXIT1.3 mandated its
six-step order "on EVERY lite/client teardown path - spawned T5,
carrier TLS-death, the ~VM walk", contradicting BINDING annex
A36 ("stands unamended" at TERM1.5), whose ~VM foreign-carrier
collection does unregister-FIRST with client+lite destruction
DEFERRED to the owner's TLS destructor; the deferred lite's M12
queue removal would run after the VM is gone, with no argument
that M11 covers it. RESOLUTION (shape: scope EXIT1.3 + amend
A36; both sides, [r30] marker at A36): EXIT1.3 is scoped to
LIVE-VM paths (spawned T5, carrier TLS-death) and EXPLICITLY
EXCLUDES the ~VM carrier collection; A36 is AMENDED because its
deferral was NOT yet sound as written - ~GCClient::Heap's
live-path dtor touches the server (Heap.cpp:5078-5110:
acquireHeapAccess bracket, lastChanceToFinalize's
shared-directory work under MSPL, m_server.clientSet().remove),
so the amendment moves the FULL server-side detach into the ~VM
walk (server alive) and restricts the deferred dtor to a
degenerate dead-detached path touching only client-local memory.
The deferred M12 removal IS covered: ~VM's M11 force-removal
(VM.cpp:710-719) empties VM::m_microtaskQueues under the
process-lifetime registry lock before any VM memory dies, so the
deferred ~MicrotaskQueue (MicrotaskQueue.cpp:128-141,
isOnList()-guarded under the same lock) is a no-op touching only
its own node - quoted in EXIT1.9/the A36 amendment. CRUX
interaction with F1: A36's carriers are PHYSICALLY UNREGISTERED
by ~VM itself BEFORE the EXIT1.9 wait begins (EXIT1.9 step (2)),
so the wait never counts a carrier whose TLS destructor runs at
an unbounded future time - no deadlock; combined-protocol
progress argument in EXIT1.6/1.9. Same M11/M12 argument also
fixes an r29 imprecision: the spawned-path lite free (its M12
removal) runs AFTER unregisterLite (registry lock not
recursive), i.e. OUTSIDE the fence - covered by the EXIT1.9
residual-tail rule, not the wait.

Spec bumped rev 29 -> rev 30; normative changes are ANNEX EXIT1
as amended (incl. new EXIT1.9) + the A36 amendment + their body
pointers/compressions. No new SD.

## ANNEX EXIT1 (BINDING, as AMENDED by rev 30 - the annex of
## record; the rev-28/29 texts are superseded where they differ)
## - exit-during-stop-window lifetime: per-sample registry
## re-enumeration + TEARDOWN-mark-before-destroy + physical
## removal LAST + the EXIT1.9 ~VM completion fence (amends
## §A.3.1, §A.3.2, §B.2, annex E2A's close tail, INV U3 and
## annex A36's ~VM-teardown clause; SUPERSEDES annex SB1 item
## 1's "ENUMERATION only" clause and vmstate §6.5.1/§6.4.4's
## assert-only ~VM fence, both sides)

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
walk (VM.cpp:651-657) is #if ASSERT_ENABLED, so a RELEASE build
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
the fence never waits on a TLS destructor.

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
(r29; path scope + fence wording AMENDED r30; amends §B.2 +
annex E2A's close tail + INV U3, both sides; NO exit gating
added; REPLACES rev 28's unregisterLite-first order). On every
LIVE-VM lite/client teardown path - spawned T5 and carrier
TLS-death - LOGICAL removal precedes any destruction and
PHYSICAL removal comes LAST: under VMLiteRegistry::lock the
exiting thread marks its lite TEARDOWN (one registry-owned
lite-state byte; the lite stays PHYSICALLY registered;
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
EXCLUDED from this order - it follows annex A36 as AMENDED r30
(cross-ref both sides; the A36 annex carries the [r30] marker):
unregister FIRST, full server-side detach inside the walk,
client+lite destruction DEFERRED to the owner's TLS destructor
and restricted to non-VM memory; EXIT1.9 step (2) pins its
ordering against the ~VM wait. rev 29's "on EVERY teardown path
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
current walk - counts as EXITED. Soundness (r29 re-stated): the
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
EXIT1.9 wait, ~VM holds the API lock (VM.cpp:650) and acquires
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
makes progress.

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
- ANNEX A36 AMENDED (r30, both sides; the A36 annex carries the
  in-place [r30] marker; TERM1.5's "A36 stands unamended" gains
  a [r30] scope note - its single-VM clause stands): carrier
  collection unregisters BEFORE the EXIT1.9 wait; full
  server-side detach inside the walk; deferred destruction
  restricted to non-VM memory (degenerate dead-detached dtor;
  M11/M12 no-op queue removal). Full text: the rev-30 A36
  amendment record.
- vmstate §6.5.1 SUPERSESSION extended (both sides; IV row):
  VMLiteRegistry gains one WTF::Condition; unregisterLite
  notifyAll()s it under the lock after removal; the §6.4.4 ~VM
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
- U20 lint: §A.3 conductor code must reach lites ONLY via the
  forEachEnteredThread registry-walk helper; any lite*/client*
  value in conductor code that crosses a sample boundary
  (escapes the walk's lock scope) is flagged; teardown paths are
  checked for TEARDOWN-mark-precedes-DCT/client-destroy AND
  unregisterLite-LAST (after client destroy, before the lite
  free); ~VM is checked for EXIT1.9-wait-precedes-teardown (the
  wait before notifyVMDestruction/heap teardown) and the A36
  deferred-dtor path for naming NO m_server or VM member.

EXIT1.9 ~VM completion fence (r30; NORMATIVE; SUPERSESSION, both
sides: vmstate §6.5.1/§6.4.4's assert-only "registry empty for
this VM" - the VM.cpp:651-657 #if ASSERT_ENABLED walk - and
A36's assert wording vs this; IV row). Mechanism: VMLiteRegistry
gains one WTF::Condition (vmTeardownCondition) beside lock;
unregisterLite - already under the lock - notifyAll()s it after
removing the lite. ~VM order at the §6.4.4 top:
(1) uninstall the main carrier TLS (unchanged);
(2) the A36 foreign-carrier collection (as AMENDED r30): each of
this VM's carriers is token-free-asserted and PHYSICALLY
UNREGISTERED under the registry lock, the lock is released, then
the walk performs the full server-side detach of each collected
client - ALL BEFORE step (3), so the wait never counts a carrier
whose deferred TLS destructor runs at an unbounded future time
(no deadlock on A36's deferral);
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
Residual tail OUTSIDE the fence (r30 precision; amends r29's
"M12 removal inside the fence" wording, both texts here): the
lite free - and the M12 removal of the lite's default
MicrotaskQueue inside it - runs AFTER unregisterLite returns
(the registry lock is not recursive; ThreadObject.cpp:262
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
argument covers A36's deferred carrier queues (the rev-30 A36
amendment record).

## ANNEX A36 AMENDMENT (r30; BINDING; both sides - the rev-9
## ANNEX A36 carries the in-place [r30] marker at its
## ~VM-teardown clause; TERM1.5's "A36 stands unamended" gains a
## [r30] scope note - its single-VM clause stands)

A36's "~VM teardown" clause is AMENDED (text of record; the
unamended clause is superseded where they differ): ~VM COLLECTS
this VM's carriers under the registry lock (each token-free,
else RELEASE_ASSERT), unregisters them, releases the lock, then
performs the FULL SERVER-SIDE detach of each collected client
while the server Heap is alive - everything in ~GCClient::Heap
that names m_server: the access bracket, lastChanceToFinalize's
shared-directory allocator relinquishment under MSPL,
machineThreads removal, m_server.clientSet().remove()
(Heap.cpp:5078-5110 is the live-path dtor doing exactly these
against the server) - leaving each client dead-detached. ALL of
this precedes the EXIT1.9 wait, so the wait never counts a
carrier. Client + lite destruction stays DEFERRED to the owner's
TLS destructor (immediate if the owner is dead), but the
deferred dtor MUST take a degenerate dead-detached path: assert
dead-detached, SKIP every m_server touch (all already done by
the walk), destroy only client-local memory (TLC tables,
m_perDirectory, the lite, the lite's default MicrotaskQueue).
M12 story for the deferred queue (the VM is gone by then): sound
by the M11/M12 protocol quoted in EXIT1.9's residual-tail rule -
the M11 force-removal (VM.cpp:710-719) empties
VM::m_microtaskQueues under the process-lifetime registry lock
(VMLiteRegistry is NeverDestroyed) before any VM memory dies, so
the deferred ~MicrotaskQueue (MicrotaskQueue.cpp:128-141) finds
isOnList() false and touches only its own node. The {client,
epoch} TLS staleness rule, the process-monotonic VM epoch, I20,
the TID supersessions and the single-VM clause (TERM1.5) are
UNCHANGED; "§6.5.1 assert => registry empty for this VM" is
re-read through EXIT1.9 (wait-then-debug-assert). EXIT1.3's
order EXPLICITLY EXCLUDES this path (live-VM paths only;
cross-ref both sides). U27 gains the deferred-degenerate-dtor /
delayed-TLS-destructor arm (= the EXIT1.8 CARRIER variant).

### rev-30 spec deltas
- header rev 30; sect T cites r10-r30; sect SD per-rev
  attribution "r27-r30 add none".
- sect A.3.1 EXIT1 index: annex cite -> "as AMENDED r30"; gains
  "~VM BLOCKS until registry VM-empty (EXIT1.9)" (the NORMATIVE
  marker lives in the sect B.2 fence parenthetical).
- sect B.2 item 2 rewritten per EXIT1.3/1.9 as amended: order
  scoped to LIVE-VM paths; ~VM carrier collection EXCLUDED (A36
  as AMENDED r30); the ~VM blocking fence replaces the
  assert-fence parenthetical.
- sect A.3.6 item 6: A36 cite -> "as AMENDED r30".
- INV U32 text = EXIT1.7 as amended (IDs frozen; U3 unchanged
  from r29).

### rev-30 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays in
### the cited BINDING annex/rev; pointer targets + supersession
### rows untouched)
- sect A.3.1 (ANNEX EXIT1): "UNDER VMLiteRegistry::lock (§LK.6,
  inner to m_worldLock; dropped between samples; walk
  alloc-free, acquires nothing)" -> "UNDER VMLiteRegistry::lock
  (§LK.6; dropped between samples)" (EXIT1.2/1.6 keep the
  inner-to-m_worldLock + alloc-free/acquires-nothing clauses).
- sect A.3.2 2b (ANNEX SB1): "ALL seq_cst; AHA poll AFTER the F8
  step-1 CAS; acq/rel UNSOUND" -> "ALL seq_cst; acq/rel UNSOUND"
  (SB1.3 keeps the AHA poll position).
- sect A.3.1 (ANNEX EXIT1): "m_worldLock (heap rank 3, held for
  the window)" -> "(heap rank 3)" (EXIT1.6 keeps the
  held-for-the-window clause).
- sect A.3.6 item 6: "(BINDING; r9 F4; IV/IH rows;
  SUPERSESSIONs..." -> "(BINDING; r9 F4; SUPERSESSIONs..." (the
  A36/A36C annexes keep their IV/IH row obligations).
- sect B.2 item 2 (ANNEX EXIT1): the "(= the ~VM fence: registry
  non-empty until the heap-touching tail done, vmstate
  §6.5.1/A36...)" parenthetical replaced by the shorter EXIT1.9
  pointer (EXIT1.3/1.9 keep the full fence statement; EXIT1.9
  keeps the assert-demotion + M11/M12 no-op clauses the pointer
  elides).
- sect E.2 (ANNEX E2A): "deadlines => §E.4 'timed-out' (SD8
  ext)" -> "deadlines => §E.4 'timed-out'" (E2A/SD8 keep the
  ext); the close-tail T5 cite -> "(EXIT1.3/1.9)".
- sect E.2 lock/access rule (§LK): "(§LK long-hold; r22 list)"
  -> "(§LK long-hold)" (the §LK section keeps the r22 list
  cite).

### rev-30 section-T deltas (extends rev-9 annex 3 + r10-r29)
- U-T6 (owns the teardown paths): implements the EXIT1.9 ~VM
  completion fence (registry Condition; unregisterLite notify;
  ~VM order steps (1)-(4); assert walk demoted to post-wait
  sanity) and the A36 amendment (carrier collection unregisters
  pre-wait; full server-side detach in the walk; degenerate
  deferred dtor); EXIT1.3 order on LIVE-VM paths only. Gate
  list: the r30-REWRITTEN EXIT1.8 T5-TAIL-VS-~VM arm (RELEASE +
  ASAN join-then-destroy-VM under amplifier stalls, instrumented
  wait-ordering check; DEBUG sanity-walk variant; the CARRIER
  deferred-dtor variant) REPLACES the r29 arm.
- U-T5: unchanged except the U20 lint extension per EXIT1.8 as
  amended (~VM wait-precedes-teardown; A36 deferred-dtor
  no-m_server check).
- No other task scope changes; U27's arm list per the A36
  amendment.

### Rev-30 SD note
No new SDs; IDs frozen. r30 is lifetime/ordering only - the ~VM
wait blocks an embedder thread already destroying its VM; no
JS-observable behavior changes.

# REV 31 (2026-06-06) - reviewed-findings round vs rev 30's A36
# AMENDMENT: 1 blocker fixed (the carrier-state handshake) + 3
# citation nits

Round record (1 finding + 3 nits):

F1 (BLOCKER, design change). rev 30's amended A36 / EXIT1.9 step
(2) collected carriers under VMLiteRegistry::lock (token-free
RELEASE_ASSERT, unregister), then RELEASED the lock and
performed the full server-side detach of each collected client
lock-free. Nothing gates the carrier's OS-thread DEATH (re-entry
is API-lock-gated; thread death is not). Interleaving: the walk
unregisters carrier C's lite, releases the lock, is preempted;
C's owner thread exits; its TLS destructor fires and cannot know
it was collected - the dead-detached discriminator was written
by the walk POST-lock-release and read by the dying owner (a
data race on the discriminator itself). Either (a) the dtor
takes the live carrier-TLS-death path - a live ~GCClient::Heap
(access bracket, lastChanceToFinalize under MSPL,
clientSet().remove, Heap.cpp:5078-5110) racing the walk's
in-flight detach of the SAME client: double remove / racing MSPL
- or (b) it keys on "unregistered" and the dead-detached assert
fires spuriously; in release it frees client+lite while the walk
still holds the pointers: UAF inside ~VM. (The window is
inherited from pre-amendment A36, but rev 30 is the rev that
claimed the deferral sound, so it is closed now.) Whole-detach-
under-the-registry-lock was examined and is ILLEGAL: the detach
acquires MSPL and can PARK in the access bracket
(Heap.cpp:5078-5110), and LK.6 registry-lock holders acquire NO
lock and never wait (vmstate I7) - both violated. FIX (shape A,
claim-token handshake; NORMATIVE in EXIT1.9 + the r31 A36
amendment): the registry-owned lite-state byte (EXIT1.3) gains
COLLECTED and DETACHED - state machine LIVE -> TEARDOWN (owner's
live path) | LIVE -> COLLECTED -> DETACHED (~VM walk), every
transition AND read under the registry lock. The walk marks
COLLECTED BEFORE unregistering (same hold; TEARDOWN lites
skipped - still registered, the step-(3) wait covers them),
detaches lock-free, then per client re-acquires the lock, flips
COLLECTED->DETACHED and notifyAll()s vmTeardownCondition (short
hold, acquires nothing). The owner's TLS destructor takes the
registry lock FIRST and keys ONLY on the state: LIVE => mark
TEARDOWN, live path; COLLECTED => predicate-wait on
vmTeardownCondition for DETACHED, then the degenerate path;
DETACHED => degenerate path. Progress: the COLLECTED wait
depends only on the running, straight-line ~VM walk, which never
holds the registry lock during MSPL/heap work; acyclic: no
thread waits while OWNING the registry lock (Condition::wait
drops it). The condition is now shared by two predicate-loop
waiters (step (3) and the dtor) - cross-wakeups benign; recorded
both sides at the vmstate §6.5.1 supersession row. Rejected: (B1)
defer-the-unregister - a dtor seeing REGISTERED takes the live
path and races the detach anyway (a COLLECTED state is needed
regardless), and a still-registered carrier deadlocks the
EXIT1.9 wait; (B2) pinning the carrier's ThreadState ref
(ThreadManager.h:166-186 ThreadSafeRefCounted) - it defers only
~ThreadState; the client+lite free lives in the TLS map
destructor, which a ref pin does not defer. Corollary mandate:
EVERY physical registry removal goes through unregisterLite (the
notifying function) - the A36 collection and ~VM's m_mainVMLite
removal INCLUDED - and the U20 lint flags hand-rolled removals
and any lite-state access outside a registry-lock hold. New
EXIT1.8 arm: CARRIER-TLS-DEATH-DURING-DETACH (amplifier stalls
the walk inside the detach window; ASAN; DEBUG and RELEASE).

N1-N3 (citation nits, annex-of-record texts corrected; the rev-30
section retains its stale numbers as historical record): the ~VM
ASSERT walk is VM.cpp:652-658 (r30 wrote :651-657); the ~VM
API-lock assert is VM.cpp:649 (r30 wrote :650); the post-release
lite free is ThreadObject.cpp:263 (r30 wrote :262).

Spec bumped rev 30 -> rev 31; normative changes are ANNEX EXIT1
as amended (the r31 carrier-state handshake in EXIT1.9 + the
EXIT1.3 state-byte values + the EXIT1.8 arm/lint) + the r31 A36
amendment + their body pointers/compressions. No new SD.

## ANNEX EXIT1 (BINDING, as AMENDED by rev 31 - the annex of
## record; the rev-28/29/30 texts are superseded where they
## differ) - exit-during-stop-window lifetime: per-sample registry
## re-enumeration + TEARDOWN-mark-before-destroy + physical
## removal LAST + the EXIT1.9 ~VM completion fence + the r31
## carrier-state handshake (amends
## §A.3.1, §A.3.2, §B.2, annex E2A's close tail, INV U3 and
## annex A36's ~VM-teardown clause; SUPERSEDES annex SB1 item
## 1's "ENUMERATION only" clause and vmstate §6.5.1/§6.4.4's
## assert-only ~VM fence, both sides)

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
- CARRIER-TLS-DEATH-DURING-DETACH arm (r31; joins the U-T6 gate
  list; DEBUG AND RELEASE builds, ASAN): the amplifier stalls
  the ~VM walk INSIDE a collected client's lock-free server-side
  detach (variants: post-unregister pre-detach,
  mid-lastChanceToFinalize, post-detach pre-flip) while the
  owner thread exits; instrumented checks: the owner's TLS
  destructor takes the registry lock, reads COLLECTED, parks on
  vmTeardownCondition, and runs the degenerate path only AFTER
  the walk's DETACHED flip (ordering check) - no double
  clientSet().remove, no concurrent MSPL section on the same
  client, no UAF; ASAN clean. Reverse variant: the dtor wins the
  lock BEFORE collection - sees LIVE, marks TEARDOWN, takes the
  live path; the walk SKIPS the TEARDOWN lite and the step-(3)
  wait absorbs it.
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
disposition AMENDED r31; NORMATIVE; SUPERSESSION, both
sides: vmstate §6.5.1/§6.4.4's assert-only "registry empty for
this VM" - the VM.cpp:652-658 #if ASSERT_ENABLED walk - and
A36's assert wording vs this; IV row). Mechanism: VMLiteRegistry
gains one WTF::Condition (vmTeardownCondition) beside lock;
unregisterLite - already under the lock - notifyAll()s it after
removing the lite. ~VM order at the §6.4.4 top:
(1) uninstall the main carrier TLS (unchanged);
(2) the A36 foreign-carrier collection (as AMENDED r31 - the
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
nothing), and NEVER touches that lite/client again - if no TLS
destructor will run for the owner, the walk itself then runs the
degenerate free. ALL of step (2) precedes step (3), so the wait
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
Carrier-TLS-death disposition (r31): the owner's TLS destructor
takes the registry lock FIRST and keys ONLY on the
lock-published lite-state - NEVER on whether its lite is still
registered: LIVE => mark TEARDOWN in the same hold and take the
live EXIT1.3 path; COLLECTED => predicate-wait on
vmTeardownCondition until DETACHED (the wait drops the registry
lock; unregisterLite notifies are tolerated - predicate loop),
then the degenerate path; DETACHED => the degenerate path
immediately. The lite (and its state byte) is freed ONLY by the
path that observed DETACHED (or by the live path's own free),
and the owner cannot pass its COLLECTED wait before the walk's
DETACHED flip - the walk's LAST touch - so the byte is never
read after free and the walk's pointers never dangle. Progress:
the COLLECTED wait depends only on the ~VM walk, which is
running, straight-line, and never blocks on the dtor (its
collection and flip holds are short; its detach work is
lock-free); acyclicity per EXIT1.6 (no thread waits while OWNING
the registry lock). Shared condition (both sides with step (3)):
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


## ANNEX A36 AMENDMENT (r31; BINDING; both sides - the rev-9
## ANNEX A36 carries the in-place [r31] marker at its
## ~VM-teardown clause; TERM1.5's "A36 stands unamended" keeps
## its [r30] scope note - its single-VM clause stands; SUPERSEDES
## the rev-30 amendment text where they differ)

A36's "~VM teardown" clause is AMENDED (text of record): the
registry-owned lite-state byte (EXIT1.3) gains two values - the
carrier state machine is LIVE -> TEARDOWN (owner's TLS
destructor, live path) | LIVE -> COLLECTED -> DETACHED (~VM
walk); TEARDOWN and DETACHED are terminal; no other transitions;
EVERY transition AND every read is under VMLiteRegistry::lock.
The state byte - NEVER "is my lite registered" - is the sole
owner-vs-walk discriminator. ~VM COLLECTS this VM's carriers
under ONE registry-lock hold: each non-TEARDOWN carrier is
token-free-RELEASE_ASSERTed, marked COLLECTED, and unregistered
via unregisterLite (U20: EVERY physical registry removal - this
collection and m_mainVMLite included - goes through
unregisterLite, the notifying function); TEARDOWN carriers are
SKIPPED (owner mid-live-detach, still registered - the EXIT1.9
step-(3) wait covers them). The lock is released; the walk
performs the FULL SERVER-SIDE detach of each COLLECTED client
while the server Heap is alive - everything in ~GCClient::Heap
that names m_server: the access bracket, lastChanceToFinalize's
shared-directory allocator relinquishment under MSPL,
machineThreads removal, m_server.clientSet().remove()
(Heap.cpp:5078-5110 is the live-path dtor doing exactly these
against the server) - leaving each client dead-detached. The
detach runs LOCK-FREE of the registry lock NECESSARILY: it
acquires MSPL and can PARK in the access bracket, and LK.6
registry-lock holders acquire NO lock and never wait (vmstate
I7) - whole-detach-under-the-lock is ILLEGAL. After EACH
client's detach the walk re-acquires the registry lock, flips
COLLECTED->DETACHED, notifyAll()s vmTeardownCondition, drops the
lock (short hold; acquires nothing), and NEVER touches that
lite/client again. ALL of this precedes the EXIT1.9 wait, so the
wait never counts a carrier. Client + lite destruction stays
DEFERRED to the owner's TLS destructor; if no TLS destructor
will run for an owner, the walk runs the degenerate free itself
AFTER its DETACHED flip. The deferred dtor takes the registry
lock FIRST and keys ONLY on the lock-published state: LIVE =>
mark TEARDOWN in the same hold and take the live EXIT1.3 path;
COLLECTED => predicate-wait on vmTeardownCondition until
DETACHED (Condition::wait drops the lock into the parking lot;
unregisterLite notifies tolerated - predicate loop), then the
degenerate path; DETACHED => the degenerate path immediately:
assert DETACHED, SKIP every m_server touch (all already done by
the walk), destroy only client-local memory (TLC tables,
m_perDirectory, the lite, the lite's default MicrotaskQueue).
Progress/acyclicity: the COLLECTED wait depends only on the
running, straight-line ~VM walk; no thread waits while OWNING
the registry lock; the lite is freed only by the path that
observed DETACHED (or by the live path), strictly after the
walk's last touch - the state byte is never read after free. M12
story for the deferred queue (the VM is gone by then): sound by
the M11/M12 protocol quoted in EXIT1.9's residual-tail rule -
the M11 force-removal (VM.cpp:710-719) empties
VM::m_microtaskQueues under the process-lifetime registry lock
(VMLiteRegistry is NeverDestroyed) before any VM memory dies, so
the deferred ~MicrotaskQueue (MicrotaskQueue.cpp:128-141) finds
isOnList() false and touches only its own node. The {client,
epoch} TLS staleness rule, the process-monotonic VM epoch, I20,
the TID supersessions and the single-VM clause (TERM1.5) are
UNCHANGED; "§6.5.1 assert => registry empty for this VM" is
re-read through EXIT1.9 (wait-then-debug-assert). EXIT1.3's
order EXPLICITLY EXCLUDES this path (live-VM paths only;
cross-ref both sides). U27 gains the deferred-degenerate-dtor /
delayed-TLS-destructor arm AND the r31
CARRIER-TLS-DEATH-DURING-DETACH arm (= the EXIT1.8 CARRIER +
r31 arms).

### rev-31 spec deltas
- header rev 31; sect T cites r10-r31; sect SD per-rev
  attribution "r27-r31 add none".
- sect A.3.1 EXIT1 index: annex cite -> "as AMENDED r31".
- sect B.2 item 2 rewritten per EXIT1.3/1.9 + A36 as AMENDED
  r31: the A36 parenthetical now carries the lock-published
  state machine (LIVE->COLLECTED->DETACHED; COLLECTED-mark +
  unregister pre-wait; detach lock-free; per-client DETACHED
  flip notifies; the owner TLS dtor keys ONLY on the state) and
  the ALL-physical-removals-via-unregisterLite mandate (U20).
- sect A.3.6 item 6: A36 cite -> "as AMENDED r31".
- INV U32/state-machine text = EXIT1.7 as amended (IDs frozen;
  U3 unchanged from r29).

### rev-31 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays in
### the cited BINDING annex/rev; pointer targets + supersession
### rows untouched)
- sect A.3.1 (ANNEX EXIT1): "clientHeap null => not-entered
  (write-once release-publish, §B.1/§F.1)" -> "(write-once,
  §B.1/§F.1)" (EXIT1.4(b) keeps the release-publish clause).
- sect A.3.1 (ANNEX EXIT1): "~VM BLOCKS until registry VM-empty
  (EXIT1.9). U32; U20 lint; U4 exit-storm + ~VM-race arms" ->
  "~VM BLOCKS until VM-empty (EXIT1.9). U32; U20; U4 arms"
  (EXIT1.9 keeps the registry-walk predicate; EXIT1.8 keeps the
  lint rule set and the arm names).
- sect A.3.2 2b (ANNEX SB1): "fan-out stores, conductor samples
  + AHA/§A.3.4/DAL2-dtor polls ALL seq_cst" -> "fan-out stores,
  conductor samples + polls ALL seq_cst" (SB1.2/SB1.3 keep the
  poll-site list).
- sect B.2 item 2 (ANNEX EXIT1): "T5, after the Strong clears +
  unregisterThread: release access" -> "T5 (full order EXIT1.3):
  release access" (EXIT1.3 keeps the Strong-clears prefix);
  "(registry lock; conductors count it EXITED)" -> "(registry
  lock; counted EXITED)" (EXIT1.4 keeps the conductor
  disposition); "(EXIT1.9 NORMATIVE fence: registry Condition,
  signaled by unregisterLite; U3/U32)" -> "(EXIT1.9 NORMATIVE
  fence; U3/U32)" (EXIT1.9 keeps the mechanism - now signaled by
  unregisterLite AND the DETACHED flips, which the trimmed
  wording understated); "Lazy carriers own the VM's original
  client (main) or create one at first entry (embedder, §F.1)"
  -> "Lazy carriers: the VM's original client (main) or created
  at first entry (§F.1)" (A36/§F.1 keep the embedder
  attribution).
- sect A.3.6 item 6 (ANNEX A36): "I20 holds. U27 + teardown
  storm" -> "I20 holds. U27 arms" (the r31 A36 amendment keeps
  the teardown-storm + carrier arm list).

### rev-31 section-T deltas (extends rev-9 annex 3 + r10-r30)
- U-T6 (owns the teardown paths): implements the r31
  carrier-state handshake (lite-state byte values
  COLLECTED/DETACHED; COLLECTED-mark-before-unregister in the
  ~VM walk; per-client DETACHED flip + notifyAll; the
  state-keyed TLS destructor with the COLLECTED wait) and the
  unregisterLite-only physical-removal mandate. Gate list: +the
  r31 EXIT1.8 CARRIER-TLS-DEATH-DURING-DETACH arm (amplifier
  stalls the walk inside a collected client's detach while the
  owner exits; dtor must park until the DETACHED flip;
  instrumented ordering check; no double clientSet remove, no
  racing MSPL, no UAF; ASAN clean; DEBUG AND RELEASE; + the
  reverse dtor-wins-LIVE variant).
- U-T5: the U20 lint extension per EXIT1.8 as amended r31
  (unregisterLite-only physical removals; lite-state access
  under-lock-only).
- No other task scope changes; U27's arm list per the r31 A36
  amendment.

### Rev-31 SD note
No new SDs; IDs frozen. r31 is lifetime/ordering only - the
carrier TLS destructor can now block briefly on ~VM's in-flight
detach; no JS-observable behavior changes.

# REV 32 (2026-06-06) - surgical round vs rev 31 (3 fixes, no
# other changes)
# RECORDING SCHEME (chosen and binding for this rev): ANNEX EXIT1
# is amended via an AMENDMENT RECORD, not a full re-issue - the
# rev-31 annex remains the annex of record EXCEPT for the THREE
# paragraphs re-issued IN FULL below (EXIT1.9 step (2); EXIT1.9's
# Carrier-TLS-death disposition; the EXIT1.8
# CARRIER-TLS-DEATH-DURING-DETACH arm), which supersede their
# rev-31 texts, plus one heading delta (EXIT1.9's heading now
# reads "AMENDED r31/r32"). The handout's inline EXIT1 copy is
# updated IN PLACE and equals the rev-31 annex AS SO AMENDED.
# Correspondence check: the three re-issued paragraphs are
# BYTE-IDENTICAL between this record and UNGIL-HANDOUT.md; every
# other EXIT1 paragraph in the handout is the rev-31 annex
# verbatim; "A36 as AMENDED r31" in unamended paragraphs reads
# "as AMENDED r32". The ANNEX A36 AMENDMENT (r32) below is the
# A36 text of record (supersedes the rev-31 amendment where they
# differ); the handout's inline A36 copy is updated in place
# under its standing em-dash/wrap normalization (wording
# identical; same scheme as r31).

Round record (1 major + 1 sub-major + 1 minor):

F1 (MAJOR, design decision). rev 31's clause "if no TLS
destructor will run for the owner, the walk itself then runs the
degenerate free" (EXIT1.9 step (2); mirrored in the r31 A36
amendment) keyed a free on a LIVENESS PROBE that is not
lock-published - unimplementable soundly: the walk frees
client+lite; a late-firing TLS destructor for that owner then
reads the freed state byte, can observe garbage-DETACHED, and
double-frees. Shape decision made on verified facts. Fact (i):
the carrier TLS map's destructor is the carrier-TLS-death path
(r31 record: "the client+lite free lives in the TLS map
destructor"); the mechanism class is WTF::ThreadSpecific - a
pthread-key TLS whose destructor is installed unconditionally at
key creation (ThreadSpecific.h:122 pthread_key_create(&m_key,
destroy); same mechanism as ThreadManager.cpp:157's existing
slot). So for every NON-MAIN thread, §F.1/A36 registration
ALWAYS installs the destructor. Fact (ii): pthread TLS
destructors run only at pthread_exit - the PROCESS MAIN THREAD
exits via exit()/return-from-main and never runs them
(ThreadSpecific.h:31-40 documents the pthread/Windows cleanup
split). A main thread that entered a foreign VM and still has
its carrier registered at that VM's ~VM is therefore a REAL
dtor-less owner: shape (a) (clause is dead code, delete) is
FALSE; shape (b) ADOPTED, strengthened to kill platform variance
(Windows FLS callbacks CAN run for the main thread at process
exit and would re-read the walk-freed lite): §F.1 first-entry
registration BRANCHES - non-main threads' carriers go in the
destructor-BEARING ThreadSpecific map; the MAIN thread's
carriers go in a destructor-FREE plain thread_local map over
which NO cleanup is ever installed on ANY platform (entries leak
at process exit unless a ~VM walk frees them - accepted). The
choice is recorded per-lite as ownerHasNoTlsDtor, FIXED AT
REGISTRATION TIME under the registry lock (set iff
WTF::isMainThread() at §F.1 registration) - a STATIC STRUCTURAL
FACT, NEVER a liveness probe; immutable; read under the registry
lock like the state byte (the r31 U20 lite-state lint clause
covers it - no new lint rule). The walk's degenerate free is
gated ONLY on this bit; a bit-SET lite has no competing
destructor BY CONSTRUCTION (no destructor exists over its
storage); a bit-CLEAR lite is NEVER walk-freed (deferral
unconditional). Dangling main-thread map entry post-walk-free:
never consulted - lock() compares the process-monotonic VM epoch
BEFORE the cached carrier (A36 staleness rule, unchanged), and
re-entry during ~VM is excluded (F2). New EXIT1.8 WALK-FREE
variant + U27 arm: walk-side disposition racing a late-firing
TLS dtor; the r30 CARRIER delayed-dtor variant EXTENDED to
assert no walk-side free occurred for the bit-clear lite.

F2 (SUB-MAJOR). One sentence, previously round-record prose
only, promoted into EXIT1.9's carrier-disposition paragraph
(annex of record + handout inline + spec body §B.2 compressed
form): "A COLLECTED owner's re-entry is excluded for the
duration of ~VM: re-entry requires fresh §F.1 registration under
m_lock, which ~VM holds (VM.cpp:649)."

F3 (MINOR). The A36 amendment gains: "Cross-client detach
concurrency (live dtor of carrier X vs the walk detaching Y) is
the exit-storm case - serialized by MSPL and
HeapClientSet::m_lock (heap §5.1/§6 ranks)." (HeapClientSet.h:45
confirms rank 6; heap §6 table row 6.)

Spec bumped rev 31 -> rev 32; normative changes are the three
re-issued EXIT1 paragraphs + the r32 A36 amendment (registration
clause + amended ~VM clause) + their body
pointers/compressions. No new SD.

## ANNEX EXIT1 AMENDMENT (r32; BINDING; amendment record - the
## rev-31 annex remains the annex of record except as follows;
## the handout inline EXIT1 equals the annex AS AMENDED, the
## three paragraphs below byte-identical there)

Heading delta: EXIT1.9's heading reads "EXIT1.9 ~VM completion
fence (r30; step (2) + the carrier disposition AMENDED r31/r32;
NORMATIVE; ...)" - the rest of the heading is unchanged.

Re-issued paragraph 1 of 3 - EXIT1.9 step (2) (text of record;
supersedes the rev-31 step (2)):

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

Re-issued paragraph 2 of 3 - EXIT1.9 Carrier-TLS-death
disposition (text of record; supersedes the rev-31 paragraph):

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

Re-issued paragraph 3 of 3 - the EXIT1.8
CARRIER-TLS-DEATH-DURING-DETACH arm (text of record; supersedes
the rev-31 bullet):

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

## ANNEX A36 AMENDMENT (r32; BINDING; both sides - the rev-9
## ANNEX A36 carries the in-place [r32] marker at its carrier
## TLS-map and ~VM-teardown clauses; TERM1.5's "A36 stands
## unamended" keeps its [r30] scope note - its single-VM clause
## stands; SUPERSEDES the rev-31 amendment text where they
## differ)

Registration clause (NEW r32). A36's per-(thread,VM) TLS
VM->carrier map is TWO slots, chosen ONCE at §F.1 first-entry
registration: every NON-MAIN thread's carriers live in the
destructor-BEARING WTF::ThreadSpecific map whose TLS destructor
IS the carrier-TLS-death path - registration ALWAYS installs it
for those threads; the process MAIN thread's carriers live in a
destructor-FREE plain thread_local map - pthread TLS destructors
run only at pthread_exit and never for a thread exiting via
exit()/return-from-main (ThreadSpecific.h:31-40 documents the
pthread/Windows cleanup split), and a late Windows FLS callback
over the same storage would re-read a walk-freed lite, so NO
cleanup is ever installed over the main-thread slot on ANY
platform (entries leak at process exit unless a ~VM walk frees
them - accepted). The choice is recorded per-lite as
ownerHasNoTlsDtor, FIXED AT REGISTRATION TIME under the registry
lock - set iff WTF::isMainThread() at §F.1 registration -
immutable thereafter, read under the registry lock like the
state byte (U20): a static structural fact, NEVER a liveness
probe. rev 31's no-TLS-destructor-will-run liveness-probe
clause is WITHDRAWN: it keyed a free on a non-lock-published
liveness probe (the walk frees; a late-firing dtor then reads
the freed state byte, can observe garbage-DETACHED, and
double-frees) - the bit replaces it.

A36's "~VM teardown" clause is AMENDED (text of record): the
registry-owned lite-state byte (EXIT1.3) gains two values - the
carrier state machine is LIVE -> TEARDOWN (owner's TLS
destructor, live path) | LIVE -> COLLECTED -> DETACHED (~VM
walk); TEARDOWN and DETACHED are terminal; no other transitions;
EVERY transition AND every read is under VMLiteRegistry::lock.
The state byte - NEVER "is my lite registered" - is the sole
owner-vs-walk discriminator. ~VM COLLECTS this VM's carriers
under ONE registry-lock hold: each non-TEARDOWN carrier is
token-free-RELEASE_ASSERTed, marked COLLECTED, and unregistered
via unregisterLite (U20: EVERY physical registry removal - this
collection and m_mainVMLite included - goes through
unregisterLite, the notifying function); TEARDOWN carriers are
SKIPPED (owner mid-live-detach, still registered - the EXIT1.9
step-(3) wait covers them). The lock is released; the walk
performs the FULL SERVER-SIDE detach of each COLLECTED client
while the server Heap is alive - everything in ~GCClient::Heap
that names m_server: the access bracket, lastChanceToFinalize's
shared-directory allocator relinquishment under MSPL,
machineThreads removal, m_server.clientSet().remove()
(Heap.cpp:5078-5110 is the live-path dtor doing exactly these
against the server) - leaving each client dead-detached. The
detach runs LOCK-FREE of the registry lock NECESSARILY: it
acquires MSPL and can PARK in the access bracket, and LK.6
registry-lock holders acquire NO lock and never wait (vmstate
I7) - whole-detach-under-the-lock is ILLEGAL. After EACH
client's detach the walk re-acquires the registry lock, flips
COLLECTED->DETACHED, notifyAll()s vmTeardownCondition, drops the
lock (short hold; acquires nothing), and NEVER touches that
lite/client again. ALL of this precedes the EXIT1.9 wait, so the
wait never counts a carrier. Remote detach (SUPERSESSION: heap
I4 "lifecycle on the using thread" + §10A.1, both sides; r32):
client + lite destruction is DEFERRED to the owner's TLS
destructor for bit-CLEAR lites - unconditionally; a bit-clear
lite is NEVER walk-freed; for a bit-SET lite (ownerHasNoTlsDtor,
the r32 registration clause above) the walk itself runs the
degenerate free immediately after its DETACHED flip - no
competing dtor exists BY CONSTRUCTION, since no destructor is
ever installed over the main-thread slot. The deferred dtor
takes the registry lock FIRST and keys ONLY on the
lock-published state: LIVE => mark TEARDOWN in the same hold and
take the live EXIT1.3 path; COLLECTED => predicate-wait on
vmTeardownCondition until DETACHED (Condition::wait drops the
lock into the parking lot; unregisterLite notifies tolerated -
predicate loop), then the degenerate path; DETACHED => the
degenerate path immediately: assert DETACHED, SKIP every
m_server touch (all already done by the walk), destroy only
client-local memory (TLC tables, m_perDirectory, the lite, the
lite's default MicrotaskQueue). Progress/acyclicity: the
COLLECTED wait depends only on the running, straight-line ~VM
walk; no thread waits while OWNING the registry lock; the lite
is freed only by the path that observed DETACHED (or by the live
path), strictly after the walk's last touch - the state byte is
never read after free. Cross-client detach concurrency (live
dtor of carrier X vs the walk detaching Y) is the exit-storm
case - serialized by MSPL and HeapClientSet::m_lock (heap
§5.1/§6 ranks). M12 story for the deferred queue (the VM is gone
by then): sound by the M11/M12 protocol quoted in EXIT1.9's
residual-tail rule - the M11 force-removal (VM.cpp:710-719)
empties VM::m_microtaskQueues under the process-lifetime
registry lock (VMLiteRegistry is NeverDestroyed) before any VM
memory dies, so the deferred ~MicrotaskQueue
(MicrotaskQueue.cpp:128-141) finds isOnList() false and touches
only its own node. The {client, epoch} TLS staleness rule, the
process-monotonic VM epoch, I20, the TID supersessions and the
single-VM clause (TERM1.5) are UNCHANGED; "§6.5.1 assert =>
registry empty for this VM" is re-read through EXIT1.9
(wait-then-debug-assert). EXIT1.3's order EXPLICITLY EXCLUDES
this path (live-VM paths only; cross-ref both sides). U27 gains
the deferred-degenerate-dtor / delayed-TLS-destructor arm, the
r31 CARRIER-TLS-DEATH-DURING-DETACH arm AND its r32 WALK-FREE
variant (= the EXIT1.8 CARRIER + r31/r32 arms).

### rev-32 spec deltas
- header rev 32; sect T cites r10-r32; sect SD per-rev
  attribution "r27-r32 add none".
- sect A.3.1 item 1 + sect A.3.6 item 6 + sect B.2 item 2: annex
  cites -> "as AMENDED r32".
- sect B.2 item 2 gains the compressed r32 clause: "no-dtor bit
  FIXED AT REGISTRATION (main: dtor-free slot) => walk frees
  post-flip; ~VM holds m_lock => no re-entry (VM.cpp:649)" (full
  text: the re-issued EXIT1.9 paragraphs + the r32 A36
  amendment).

### rev-32 spec-body wording compressions (byte budget; no
### semantic change - every trimmed clause's FULL text stays in
### the cited BINDING annex/rev; pointer targets + supersession
### rows untouched)
- sect A.3.1 item 1: "every predicate sample RE-WALKS" -> "every
  sample RE-WALKS" (EXIT1.2 keeps "predicate sample");
  "lite/client pointers" -> "lite/client ptrs".
- sect A.3.2 item 2: "is parked / not-entered / access-released
  (sampled per EXIT1) - the last sound ONLY with 2b" -> "is
  parked/not-entered/access-released (per EXIT1) - sound ONLY
  with 2b" (EXIT1.2/SB1 keep the sampling wording).
- sect A.3.6 item 6: "every install AND restore re-stamps" ->
  "installs/restores re-stamp" (A36C keeps the
  every-install-AND-restore enumeration).
- sect B.2 item 2: "DCT/destroy the client -> unregisterLite/
  free the lite LAST" -> "DCT/destroy client -> unregisterLite/
  free lite LAST" (EXIT1.3 keeps the full order); "Lazy
  carriers: the VM's original client (main) or created at first
  entry (§F.1)" -> "Lazy carriers per §F.1" (§F.1/A36 keep the
  main-vs-embedder client attribution).
- sect B.2 item 3: "(A36C EXTENDS this supersession to §10A.1's
  re-stamp clause, both sides, IH row)" -> "(A36C extends it to
  the re-stamp clause, both sides, IH row)" (A36C keeps the
  full supersession wording).

### rev-32 section-T deltas (extends rev-9 annex 3 + r10-r31)
- U-T6: implements the r32 ownerHasNoTlsDtor bit
  (registration-time-fixed under the registry lock; main thread
  => destructor-free thread_local carrier map, all other threads
  => the destructor-bearing ThreadSpecific map; the walk-frees-
  bit-set / dtor-frees-bit-clear split). Gate list: + the r32
  EXIT1.8 WALK-FREE variant (walk-side disposition racing a
  late-firing TLS dtor; bit-set lite freed exactly once by the
  walk, never dtor-visited; bit-clear lite never walk-freed) and
  the r30 CARRIER variant's extension (asserts no walk-side free
  for the bit-clear lite).
- No other task scope changes; U27's arm list per the r32 A36
  amendment.

### Rev-32 SD note
No new SDs; IDs frozen. r32 is lifetime/ordering only - the
main thread's carrier client+lite for a destroyed VM are now
freed by the ~VM walk instead of leaking until (a never-firing)
main-thread TLS death; no JS-observable behavior changes.

### GIL-removal review round 4 — orchestrator rulings (recorded here per the
### supersession-completeness rule; cited by INTEGRATE-ungil.md R4-5/R4-7)

**Ruling 1 (UNGIL-PLAN §J default-flip ordering — SUPERSEDED).**
UNGIL-PLAN §J binds the `useThreadGIL` default flip to "the ungil milestone
gate"; the handout §T U-T14 task card mandates the flip as a U-T14 close
deliverable, and the milestone gate is NOT MET (INTEGRATE-ungil.md,
MILESTONE GATE STATUS). Per the authority clause (handout > plan), the flip
as landed (OptionsList.h:696, default false) STANDS, and this ruling
supersedes §J's flip-at-gate ordering EXPLICITLY, on these binding terms:
- The Options.cpp U0 refusal clause (notifyOptionsChanged: GIL-off without
  the trio forces useThreadGIL=1; GIL-off WITH the trio still forces
  useThreadGIL=1 unless useThreadGILOffUnsafe=1) is LOAD-BEARING SAFETY
  CODE and is the binding interim milestone gate. It may be weakened or
  deleted ONLY by the change that discharges the INTEGRATE-ungil.md AB
  list and runs the §B verification ladder GIL-off (the close-ruling
  deletion rule).
- §J's GIL-on fallback oracle survives via explicit `--useThreadGIL=1`
  (U19 unaffected; recorded at the flip, supersession ledger row 9).
- The post-finalization write-once latch (Options.cpp, R2-3 fix) is part
  of the gate: setOptions cannot re-derive gilOffProcess mid-process.

**Ruling 2 (§D.2 / OM Task-14 bench gate — DEFERRAL made explicit).**
SPEC-ungil §D.2 freezes the Task-14 promotion decision "PRE-INT on jit
Task-13's GIL-stub construction bench" (SPEC-objectmodel.md:359), re-scoped
by the U-T10 amend round to HARD before U-T11 ENTRY. U-T11 landed with no
verdict recorded — a charter-ordering breach (INTEGRATE-ungil.md AB-7).
This ruling records the deferral explicitly rather than leaving it implied:
- The no-PROMOTE arm (cell-locked 8h, the landed shape) remains the
  operative interim verdict.
- The §L2.h bench MUST run at the first Build round; the verdict is
  recorded in INTEGRATE-objectmodel §46 (deferral record now present
  there).
- On a PROMOTE outcome, TWO landed surfaces are named for mandatory
  re-review: U-T10's ConcurrentButterfly locked third arm (incl. the
  amend-round LockedRevalidate undefined-disambiguation) and U-T11's §C.3
  PWT pre-enqueue load routing.
- The U-T14 close audit's supersession-completeness claim is repaired by
  this record; AB-7 narrows to the bench run itself.

# REV 33 (2026-06-10) - SPEC-congc §13.5 adoption-gate closure
# (gates (1)-(3), ungil side; solo amender, no review round -
# the adopted texts carry SPEC-congc's six-round review record)

AUTHORITY: CONGC-HANDOUT.md §0 gates (1)-(4) + SPEC-congc-history.md
ANNEX CGS2 rows CGS2.1-CGS2.4 (BINDING there; marked
SUPERSESSION-PENDING at congc rev 12). This rev lands the
SPEC-ungil side of congc §13.5 gates (1), (2) and (3) as ONE
change with SPEC-nativeaffinity rev 9 (gate (4) + the shared
CGS2.3/BL1.6/BL1.8 story - one lock-rank narrative across the
three specs). The congc history is NOT edited from here (not this
amender's file): per the congc §13.5(5) convention ("rows NOT in
force until the named owner lands the cross-cite"), the CGS2 rows
now read RECORDED-BOTH-SIDES through this record's explicit
back-cites - CGS2.1, CGS2.2, CGS2.3, CGS2.4(a), CGS2.4(b) - plus
the nativeaffinity r9 record for §13.5(4). The CGS1+CGS2
fold-at-freeze obligation (congc §13(4)) is UNCHANGED; ANNEX CGS2A
below is the ungil-side ADOPTION record, not the fold.

## Gate-by-gate landing record

- GATE (1) (congc §13.5(1); blocks the congc §5.2 CMS lock + C1):
  SPEC-ungil §LK gains rows 9c/9d (body :856-863 / :864-879, [r33]
  marked) = ANNEX CGS2.1 as proposed, with the CGD7.4 cite refresh
  applied (SINFAC I6 Heap.cpp:5125-5127 -> :5216). The U20 lint
  extension lands as proposed: U20 PROPER covers both rows; congc
  CG-T2 IS that extension; the rev-7 "U20-class" private lint
  stays retired (no second lock-order authority). CGS2.2's
  composed-chain walk (NL > GCL > m_markingMutex > CMS) is
  recorded in row 9d; its acyclicity argument is ADOPTED VERBATIM
  (ANNEX CGS2A). The heap §6 leaf-row supersession for the CMS
  lock ("never 7-9b" -> §LK.8 destructor-leaf-class shape) is
  recorded both sides per the CGS2.1 text (heap §6 + §LK.8 class
  list); nativeaffinity LK.1c remains a PENDING nativeaffinity
  §9.1 row - row 9d cites it as pending, not as landed.
- GATE (2) (congc §13.5(2); blocks C1): §A.3 rule 5 (body
  :254-292) amended per CGS2.4(a) + CGS2.3: the
  Heap::JSThreadsStopScope ctor pause obligation (BOTH ctors,
  congc F18/F47; markers only, F29; pause after a successful
  tryLock only in the watchdog ctor) + the dtor
  resume-BEFORE-GCL-release order; the F43 "allocation-free
  closure" STRIKE (see the HBT4 amendment below); and the WAIT
  BOUND reconciliation: the frozen U32/HBT4.5 stop-progress
  reading (under which a conductor's GCL wait could span one
  whole synchronous conduct, watched only by the 30s
  watchdogAssertStopProgress fail-stop) is SUPERSEDED by the
  congc CGS2.3 windowed budget - stated ONCE in congc ANNEX
  CGS2.3 (cited, never restated, here and in nativeaffinity);
  STRUCTURAL only via congc §9.1(2a)/CG-I26 (F45); congc CG-T8
  verifies the sum against the watchdog. U32 itself (EXIT1.7
  lifetime form) is NOT amended - the rule-5 text records that
  the waiting conductor keeps the EXIT1/U32 sampling discipline
  (no lite/client pointer cached across the added in-bracket
  wait; registry re-walked per sample); the superseded reading is
  the U32/HBT4.5-derived WAIT-SPAN characterization, not the
  lifetime invariant. INV IDs frozen; no new INV.
- GATE (3) (congc §13.5(3); blocks the congc §3.1 re-entry
  blocking GCL acquire): §A.3 rule 3's HBT4 order pin (body
  :232-248) gains the [r33] window-RE-ENTRY extension per
  CGS2.4(b); ANNEX HBT4 item 1 carries the in-place [r33] marker;
  the F15 first-window carve-out (tryLock access-held) and
  tryLock-only election/poll are restated unchanged.
- GATE (4) (congc §13.5(4)): nativeaffinity side - see
  SPEC-nativeaffinity rev 9 (BL1.8/CG-I19 recorded both sides
  there). Row 9d's "GC-conduct NL>GCL edge REMOVED" clause is
  this spec's consuming cite.

MODE GATING (all four): every [r33] clause is flag-gated
useConcurrentSharedGCMarking (prefix-ruled on useSharedGCHeap,
congc §7/§13.2); flag-off and GIL-on keep the frozen rev-32 text
operative BYTE-FOR-BYTE in behavior (congc CG-I0; ungil master
rule unchanged). No new SD (SD attribution: r27-r33 add none).

## ANNEX CGS2A (BINDING) - congc CGS2 rows AS ADOPTED ungil-side

The four rows below are the SPEC-congc-history.md ANNEX CGS2
texts (rev 12 state, incl. the rev-9 F43/F45 amendments) ADOPTED
as ungil-side normative text of record behind the body indexes
(§LK 9c/9d; §A.3 rules 3/5). Deltas from the CGS2 source are
LIMITED to the CGD7.4 cite refresh and are flagged inline; on any
other textual divergence the congc CGS2 row governs and the
divergence is a defect here.

CGS2A.1 (= CGS2.1, gate §13.5(1)) - §LK rows 9c/9d:
- 9c GCH::m_mutatorMarkStackLock (CMS lock) - TERMINAL leaf:
  nothing of any rank acquired while holding it; ordered INSIDE
  m_markingMutex (drain/donation sites only); MAY be taken with
  heap ranks 7-9b held (the congc §5.2 addToRememberedSet append
  path). §LK.8 destructor-leaf-class shape; like §LK.8 it
  supersedes heap §6's leaf-row "never 7-9b" for this lock - BOTH
  SIDES (heap §6 + SPEC-ungil §LK.8 class list). Soundness: the
  holder appends to a segmented array (may fastMalloc a segment),
  acquires nothing, never waits.
- 9d marking-internal group (m_markingMutex,
  m_parallelSlotVisitorLock, m_raceMarkStackLock, visitor
  m_rightToRun) - ordered INSIDE GCL/GBL; mutually ordered as
  landed (markingMutex > CMS at drains; the others mutually
  unnested vs CMS); DISJOINT from MSPL-9b except landed in-window
  uses. NEW under SPEC-congc: mutator threads reach the group
  OUT-OF-WINDOW at exactly three sites - the §5.2(ii) SINFAC-tail
  CMS donation (m_markingMutex; access held, no 7-9b - SINFAC I6
  Heap.cpp:5216 [refreshed from :5125-5127 per congc CGD7.4]),
  the §9.2(1) DCT final flush (m_markingMutex; post permanent
  access drop), and the §9.2(1)/§9.3(3) ACT/DCT pending-list
  enqueue (m_parallelSlotVisitorLock only). U20 PROPER extends to
  BOTH rows: SPEC-congc CG-T2 IS the U20 extension - the rev-7
  "U20-class" private lint is retired (no second lock-order
  authority exists).

CGS2A.2 (= CGS2.2; U20-linted via CGS2A.1) - composed-chain walk:
NL > GCL > m_markingMutex > CMS. Edges: NL > GCL - a
nativeaffinity BL1.6 §A.3 conductor (haveABadTime-class) MAY hold
NL on entry through its HBT4 bracket's GCL acquire; GCL >
m_markingMutex - the congc §9.1(2) stop-scope ctor calls
pauseConcurrentMarkingForForeignStop while holding GCL;
m_markingMutex > CMS - WND-open drain + SINFAC-tail donation
(congc §5.2). Acyclicity: CMS is TERMINAL (CG-I10(2)); GCL /
m_markingMutex holders never ACQUIRE NL (nativeaffinity NA-I10
negative edge); the barrier-append path takes CMS under 7-9b
WITHOUT m_markingMutex (CG-I10(1)); no reverse edge exists. Under
the congc rev-8 F40 ruling the GC-CONDUCT NL>GCL edge is REMOVED
(BL1.8 drop) - the chain survives only through the BOUNDED §A.3
conduct. nativeaffinity LK.1c's "OUTER to ... all leaves" was
written before these leaves existed; it HOLDS (an NL holder doing
a barrier append takes only the CMS terminal leaf; no reverse
edge), and this row makes that consistency CHARTERED and
lint-enforced rather than accidental. [LK.1c itself stays
SUPERSESSION-PENDING - nativeaffinity §9.1, NOT closed by this
rev.]

CGS2A.3 (= CGS2.3 as AMENDED congc rev 9 F45, gate §13.5(2)) -
conductor in-bracket wait BUDGET vs the 30s watchdog
(JSThreadsSafepoint.cpp:512 [refreshed from :401/:412 per congc
CGD7.4]); amends the frozen U32/HBT4.5 stop-progress reading.
Stated ONCE in congc ANNEX CGS2.3; this spec and
SPEC-nativeaffinity BL1.6/BL1.8 cite that ledger rather than
restating it. Budget terms (cited for the record): (1) one GC
window (CG-I12); (2) one marker-pause batch (congc §9.1(2)/ANNEX
CGP1); (3) with an F28 successor: the inter-cycle re-stop + the
successor's FIRST window; (4) C3: <= one sweeper quantum (congc
§9.1(7)); (5) nativeaffinity NL terms: ZERO - conductors never
ACQUIRE NL (NA-I10), the GC-conduct NL hold is removed
(F40/BL1.8), the BL1.6 conductor-HOLD case adds nothing to a
FOREIGN conductor's wait. The frozen whole-conduct reading is
superseded by this windowed bound; STRUCTURAL only via the congc
§9.1(2a) fairness rule (CGD7.2/CG-I26) - the landed §A.3
acquisition is an unqueued 1ms tryLock poll (watchdog ctor
Heap.cpp:5568-5590; real-conductor use VMManager.cpp:577) with no
queue position; failure mode = the watchdogAssertStopProgress
fail-stop. congc CG-T8 VERIFIES the sum against the watchdog.
[r34 AMENDED - REV 34 record, finding F-B: the sum bounds ONE
WINNER's GCL leg only; the landed budget is per-REQUESTER
end-to-end and adds a queue term; CG-T8 split into the
per-winner arm + an OWED storm arm; PENDING-CONGC-COUNTERPART
for the CGS2.3 ledger + CG-T8 charter.]
[r35 AMENDED - REV 35 record, finding G-B: the queue term is a
COUNT bound; 'supported fan-in' RETIRED (referenced at four
sites, defined nowhere in the family); the per-requester 30s
fail-stop is the SOLE TIME bound; the storm arm is re-chartered
ATTRIBUTION-ONLY. A congc-defined cap (proposed home: ANNEX
CGT1) is an OPTIONAL owed item, not assumed here.]

CGS2A.4 (= CGS2.4 with the rev-9 F43 strike, gates
§13.5(2)-(3)). (a) Heap::JSThreadsStopScope ctor obligation
(congc §9.1(2), F18 as AMENDED by F47): BOTH ctors (blocking
Heap.cpp:5546-5566 AND watchdog :5568-5590; shared
!isSharedServer() early-return), after acquiring GCL - watchdog
ctor: after a SUCCESSFUL tryLock only - when m_currentPhase !=
NotRunning, BLOCK in pauseConcurrentMarkingForForeignStop
(markers only - the C3 sweeper gate is phase-independent, F29);
the shared dtor (:5592-5596) resumes BEFORE releasing GCL. This
amends the frozen §A.3 rule 5/HBT4.5 characterization of the
conductor bracket (rev-32 body :256-268; now :254-292): the
"closure stays ALLOCATION-FREE" clause is STRUCK (congc rev 9
F43 - the IMPLEMENTED conductor re-acquires its own client
access and allocates in-window: AB-21 VMManager.cpp:631-646,
AB-10 weak-sweep license WeakSet.cpp:81/:106 + Heap.cpp:3339,
ungil ANNEX HBT2.1 class-4 allocating body; conductor-as-client
rules = congc §9.1(8)/ANNEX CGD7.1 - supersession recorded BOTH
sides: the congc CGS2.4 row carried the strike for this fold;
ANNEX HBT4 item 2 carries the in-place [r33] strike); the ctor is
also no longer non-blocking past the GCL acquire; the added wait
is bounded per CGS2A.3 and acquires no api-rank or heap >= 7
lock (CG-I16). [r34 AMENDED - REV 34 record, finding F-A:
WATCHDOG COVERAGE obligations (1)-(4) - timed sampled pause,
blocking-ctor requestStart, watchdog ctor threads the target VM,
CG-T8 wedged-marker arm; (1)/(4) PENDING-CONGC-COUNTERPART.]
[r35 AMENDED - REV 35 record, finding G-A: items (1)/(4) are
PROMOTED from a bookkeeping marker to a BLOCKING SHIP GATE - C1
and any useConcurrentSharedGCMarking stage implementing the
§9.1(2) pause MUST NOT ship until the congc owner records them
(back-cites congc ANNEX CGS2.4(a) + CGT1).]
(b) The HBT4 release-before-GCL order (§A.3.3)
EXTENDS to window RE-ENTRY: the conductor's per-window blocking
GCL acquire is legal exactly because it is access-released all
tenure (congc §3.1(a)-(b)); first-window carve-out F15 (tryLock
access-held) unchanged. Election/poll stay tryLock-only.

## ANNEX HBT4 AMENDMENT (r33; BINDING; amendment record - the
## rev-19 annex remains the annex of record except as follows)

- Item 1 gains the in-place [r33] window-re-entry extension
  (text at the marker; = CGS2A.4(b)).
- Item 2's "(for default conductors) allocation-free closure all
  stand" clause is STRUCK under useConcurrentSharedGCMarking
  (in-place [r33] marker; = CGS2A.4(a)'s F43 strike); flag-off
  the clause stands.
- Items 3-6 and HBT2/HBT3 stand; HBT2.1's class-4 allocating-body
  analysis is now ALSO load-bearing for the F43 strike (cited by
  CGS2A.4(a) and congc §9.1(8)).

## Cite-anchor refresh ledger (r33; for cross-spec readers -
## congc/nativeaffinity cites into SPEC-ungil.md re-read here;
## pattern per congc ANNEX CGD7.4)

| old (rev 32) | new (rev 33) | anchor |
|---|---|---|
| :867-925 | :834-915 | §LK merged lock table |
| :256-268 | :254-292 | §A.3 rule 5 (R1.i bracket) |
| :240-247 | :232-248 | §A.3 rule 3 HBT4 order pin |
| :873-886 | :846-855 | §LK.4b slot-mutex row |
| :902-907 | :893-899 | §LK long-hold NLS row |
| :768-780 | :751-759 | §K.5 haveABadTime rule |
| :289-298 | :311-321 | §A.3 rule 8 (F8 GC-stop revert) |
| :668-670 | :659-661 | §F.6(e) spawned no-nested-VM |

## Spec-body wording compressions (r33; byte budget for the gate
## landings; no semantic change - every trimmed clause's FULL
## text stays in the cited BINDING annex/rev; pointer targets +
## supersession records intact)

Compressed to annex pointers/index form: §0 U0c (annex U0C);
§A.1.3 identity-supersession parenthetical + GC-roots tail (r10
F6 / r8 item 11); §A.1.5 service routing; §A.1.6 (A16); §A.2.4
(TERM1); §A.2.6 (A26); §A.2.7 debugger; §A.2.8 (W); §A.3.1 EXIT1
index; §A.3.2b/2c (SB1/ISB1); §A.3.6 (A36/A36C); §A.3.8; §B.2
(EXIT1.3/A36); §C.1 (C1); §C.3 (C3); §D.1 (D1/D1R); §E intro;
§E.1 task-queue/host-hook bullets; §E.1b.2/.4/.5 (E1B/r16
F3/ALS1); §E.2 (E2A); §E.3 (E3); §E.4 precondition + DWT
retirement (r17 F2); §E.5 (TERM1); §E.7.3/.5 (E7/r18 F4); §F.1
(F1B); §F.2 (F2); §F.3 carve-outs (r10 F1); §F.4 (DAL2); §F.6
checklist; §H; §I (r9 F8/r22); §J.3 (r10 F5); §K.3 (LZ1/LZ2);
§K.5 (HBT); §K.6/§N.9 (AUD1); §N.1/.2/.5 (history); §N.6 (N6);
§N.8 (CBI); §LK WS rows (WS1); §IM. IDs/cross-refs preserved;
"r33 compressed" markers at the larger trims.

## Rev-33 section deltas

- Header: rev 33 + r33 cite stamp (tree 2026-06-10; congc cites
  re-read through congc CGD7.4).
- §A.3 rules 3/5: as recorded above. §LK: rows 9c/9d inserted.
- §SD: attribution "r27-r33 add none". §T: "r10-r33 deltas"; no
  task scope changes - the congc-side work (CG-1..CG-7) is congc
  §14's; U-T5/U-T14's flag-off golden gates now ALSO witness the
  [r33] clauses' flag-off deadness (CG-I0 oracle).
- Body measured 49976/50000 bytes post-edit; this history file
  uncapped.

# REV 34 (2026-06-10) - watchdog-coverage + queue-term review
# fixes (external review round vs the r33 adoption; 4 findings,
# ALL VERIFIED REAL, 0 refuted; F-A/F-B/F-D land here, F-C lands
# in SPEC-nativeaffinity-history.md r10)

Every finding was re-verified against the tree before ruling:
Heap.cpp:5546-5596 (both stop-scope ctors + dtor),
VMManager.cpp:547-600 (requestStart sampling + comment),
JSThreadsSafepoint.cpp:395-455 (jettison bracket) and :512
(watchdogAssertStopProgress(MonotonicTime, VM*));
pauseConcurrentMarkingForForeignStop confirmed to exist in NO
source file (design-only, C1 flag-gated congc text).

## F-A (major, REAL) - watchdog coverage hole in the adopted
## CGS2A.4(a)/rule-5 text

The [r33] claim "failure mode = the watchdogAssertStopProgress
fail-stop" held ONLY for the watchdog ctor's GCL tryLock loop
(Heap.cpp:5584-5587). Two waits inside the same bracket had NO
sampling site:
(a) the CGS2A.4(a) pauseConcurrentMarkingForForeignStop BLOCK -
    specified to run AFTER a successful tryLock, i.e. holding GCL
    AND the §LK.4b slot mutex, with no watchdogAssertStopProgress
    call between tryLock success and pause return (the
    conductor's next sample sits in the VMManager predicate loop,
    never reached if the pause wedges);
(b) the BLOCKING ctor (Heap.cpp:5546-5566; landed user: the
    jettison stop bracket, JSThreadsSafepoint.cpp:445-451) - a
    raw m_gcConductorLock.lock() with no requestStart parameter
    at all.
A wedged marker batch (the thread-ab17b watchdog family) would
hang the conductor silently forever if no second requester is
queued; if one IS queued, the bystander's watchdog fires at 30s
with nullptr VM context (the watchdog ctor passes nullptr,
Heap.cpp:5586) - reproducing the known nil-Class-A-context
misattribution signature. The CGS2.3 marker-pause-batch term (2)
is a structural bound ASSUMING marker progress; the watchdog
exists precisely for the non-progress case and observed neither
leg. The finder's U32 sub-claim is CONFIRMED-FINE (no fix
needed): the waiting conductor holds only the server Heap&
(conductor-outlived, not U32-covered); no lite/client ptr is
cached across the wait.

RULING (CGS2A.4(a) [r34] amendment; body rule-5 [r34] WATCHDOG
COVERAGE clause):
(1) the CGS2A.4(a) pause becomes a TIMED wait that samples
    watchdogAssertStopProgress(requestStart, vm) per quantum
    (same 1ms quantum family as the ctor tryLock loop);
(2) the blocking ctor gains a requestStart parameter (or its
    callers are re-pointed at the watchdog ctor) so
    JSThreadsSafepoint.cpp:445 is covered - the jettison caller
    samples MonotonicTime::now() before its
    ClientHeapAccessReleaseScope;
(3) the watchdog ctor threads the TARGET VM instead of nullptr,
    so a timeout attributes to the requesting VM (kills the
    nil-Class-A-context misattribution for this site);
(4) congc CG-T8 gains a wedged-marker arm proving the fail-stop
    fires ON THE CONDUCTOR ITSELF, not only on queued bystanders.
Items (1) and (4) amend congc-owned text (the CGS2.4(a) pause
body; the CG-T8 charter, ANNEX CGT1): PENDING-CONGC-COUNTERPART
- recorded here with explicit back-cites to congc ANNEX
CGS2.4(a)/CGT1, not in force congc-side until that owner
cross-cites (the congc §13.5(5) convention, direction reversed).
Items (2)-(3) are this-side obligations, in force now; flag-off
observable behavior unchanged (the ctor parameter is
mode-independent plumbing on a shared-server-only path; the
tryLock loop is isSharedServer()-gated).

## F-B (major, REAL) - CGS2.3 per-winner budget vs the single
## end-to-end 30s watchdog: the multi-requester queue term

CGS2.3/CGS2A.3 bound ONE winner's GCL wait (window +
marker-pause batch + F28 terms + sweeper quantum + zero NL). The
LANDED budget is sampled ONCE before slot arbitration and covers
all three legs end-to-end (VMManager.cpp:556-566: "One 30s
budget covers all three legs end-to-end"; slot losers park in
the 1ms tryLock loop under the SAME requestStart). A loser's
total wait = sum over earlier winners of (their CGS2.3 GCL
budget + their FULL stop window: pause + predicate quiescence +
closure) - queue-depth-scaled, with no CGS2.3 term. So the r33
sentence "congc CG-T8 VERIFIES the sum against the watchdog"
verified a PER-WINNER bound against a PER-REQUESTER budget. The
two specs also disagreed on what a timeout MEANS: congc F45
framed budget excess as lost progress, while VMManager.cpp:553-
555 rules that long LEGITIMATE queues exceeding 30s "must
distinguish loudly".

RULING (option B of the finding; CGS2A.3 [r34] amendment - keep
the landed end-to-end budget, make the queue term explicit):
- The VMManager comment is ADOPTED as the operative reading: the
  30s watchdog is a per-REQUESTER end-to-end fail-stop. A
  Class-A fire storm whose queue legitimately serializes past
  30s IS a deliberate loud failure - the supported-fan-in cap
  below bounds when that is reachable, and the F-A item-(3) VM
  threading makes the diagnostic attributable instead of the
  nullptr signature.
- CGS2A.3 gains the explicit queue term: requester total <=
  k x (per-winner CGS2.3 sum + one full §A.3 stop window), k =
  earlier winners ahead of this requester, k <= the supported
  fan-in cap. The §9.1(2a)/CG-I26 fairness rule makes the
  PER-WINNER bound structural; slot losers stay unqueued 1ms
  pollers (ordering among losers probabilistic) - the cap is
  over COUNT, not order.
- congc CG-T8 splits: the existing arm verifies the per-winner
  sum; a NEW STORM ARM is OWED (no fail-stop at/below supported
  fan-in; a loud, attributable one above it).
- The CGS2.3 ledger amendment + the CG-T8 storm arm are
  congc-owned: PENDING-CONGC-COUNTERPART (back-cites: congc
  ANNEX CGS2.3 + its rev-9 F45 note + ANNEX CGT1;
  VMManager.cpp:553-566).
- Option A (re-arm requestStart per leg) REJECTED: it erases the
  end-to-end property the VMManager change deliberately
  introduced (the pre-predicate legs were previously unbounded
  unwatched blocks) and a perpetual slot loser could starve
  unwatched across re-arms.

## F-C (major, REAL; cross-file) - NA-I12's r9-trimmed LLInt/
## thunk anchors absent from the claimed home ANNEX EX1

Fixed in SPEC-nativeaffinity-history.md r10 (ANNEX EX1 AMENDMENT
restoring the two anchors + present-tree drift note); see that
record. No ungil-side text involved.

## F-D (major, REAL; this file) - the r33 §A.1.5 compression
## dropped a named routine binding with no history home

The r33 "Spec-body wording compressions" record's blanket
license ("every trimmed clause's FULL text stays in the cited
BINDING annex/rev") was FALSE for the §A.1.5 entry: the rev-32
body's named-routine binding survived nowhere in this spec set
(the UNGIL-HANDOUT.md:227 copy is a downstream implementation
handout, not a binding home). Sole loss found - the finder's
other spot-checks across both spec pairs verified clean.
COMPLETION (this paragraph is now the BINDING home; the r33
record's §A.1.5 list entry reads through here):

  §A.1.5 trimmed clause, FULL text (rev-32 body): "ctor/dtor +
  executeEntryScopeServicesOnExit use the CURRENT lite" - i.e.
  executeEntryScopeServicesOnExit (the VMEntryScope dtor
  service drain) resolves the CURRENT lite, the same binding as
  the ctor/dtor. The r33 body's "ctor/dtor use the CURRENT
  lite" is an INDEX of this line; the routine binding is
  normative.

## ANNEX CGS2A AMENDMENT (r34; BINDING; in-place [r34] markers
## at CGS2A.3 and CGS2A.4(a))

- CGS2A.3: "congc CG-T8 VERIFIES the sum against the watchdog"
  AMENDED per F-B (per-winner sum = the existing arm; the
  per-requester end-to-end budget carries the queue term and the
  OWED storm arm; timeout meaning = the adopted VMManager
  reading).
- CGS2A.4(a): gains the F-A WATCHDOG COVERAGE obligations
  (1)-(4).
- Both amendments PENDING-CONGC-COUNTERPART exactly where they
  touch congc-owned text (CGS2.3 ledger; CGS2.4(a) pause body;
  CG-T8/CGT1 charter); the this-side obligations (F-A (2)-(3))
  are in force.

## Rule-5/rule-3 [r34] compressions (license: full text stays in
## the cited annex; no semantic change)

- Rule-5 FULL-CLIENT cite triple (AB-21 VMManager.cpp:631-646;
  AB-10 WeakSet.cpp:81/:106 + Heap.cpp:3339; HBT2.1 class-4
  body) -> "(AB-21/AB-10/HBT2.1 class-4; CGS2A.4(a))" - full
  text CGS2A.4(a), unchanged.
- Rule-5 STOP-SCOPE PAUSE: "pause after a SUCCESSFUL tryLock
  only" + "(markers only - C3 sweeper gate phase-independent,
  F29)" trimmed to the CGS2A.4(a) pointer - full text there,
  unchanged.
- Rule-5 WAIT BOUND term enumeration (window + marker-pause
  batch + F28 terms; NL ZERO per BL1.8/NA-I10) -> "(terms:
  CGS2A.3)" - full text CGS2A.3, unchanged.
- Rule-3 [r33] window-re-entry block compressed to the
  CGS2A.4(b) pointer; trimmed wording ("the congc §3.1
  per-window blocking GCL acquire is legal exactly because the
  conductor is access-released all tenure"; "first-window
  tryLock carve-out"; "Flag-gated useConcurrentSharedGCMarking;
  flag-off frozen text operative") - full text CGS2A.4(b),
  unchanged.
- §A.3 rule-5 HBT2-HBT4 pointer: "(BINDING; r27 compressed; r33
  amendment record)" -> "(BINDING; r27/r33 records)" - both
  records unchanged.

## Rev-34 section deltas

- Header: rev 34 stamp.
- §A.3 rule 3 (window re-entry) + rule 5: as recorded above
  ([r34] markers in place).
- No SD additions; no §T task scope changes; CG-I0 oracle holds:
  every [r34] clause is either flag-gated
  useConcurrentSharedGCMarking text or watchdog plumbing with no
  flag-off observable delta (U-T5/U-T14 golden gates unchanged).
- Body measured 49997/50000 bytes post-edit; this history file
  uncapped.

# REV 35 (2026-06-10) - composition-review fixes vs the r34
# landing (external round; 4 findings, ALL VERIFIED REAL, 0
# refuted; G-A/G-B/G-D land here, G-C lands in
# SPEC-nativeaffinity-history.md r11)

Re-verified vs the tree before ruling: Heap.cpp:4596-4673
(election/poll GCL brackets - caller-side releases :4606,
:4669, :4673), :5031 (the conduct's access-reacquire tail,
conductorClient.acquireHeapAccess(), INSIDE
conductSharedCollection), :5546-5596 (stop-scope ctors/dtor),
:5584-5587 (quantum tryLock loop); JSThreadsSafepoint.cpp:445
and :512; VMManager.cpp:553-566; SPEC-congc.md §9.1(2) (the
governing untimed pause text) and §13.5(5) (gates (1)-(4), no
r34-counterpart gate).

## G-A (major, REAL) - PENDING-CONGC-COUNTERPART was a
## bookkeeping marker, not a gate

A congc-side implementer following only their governing annex
ships the §9.1(2) pause exactly as written there: an untimed,
unsampled BLOCK taken holding GCL AND the §LK.4b slot mutex,
the conductor's next watchdog sample unreachable (it sits in
the VMManager predicate loop) - with all four congc §13.5(5)
gates green, because that list predates r34 and binds CG-3 to
none of the F-A obligations. Composition status, stated
honestly: the GCL leg's bound is STRUCTURAL
(CGD7.2/CG-I26 + the :5584-5587 quantum loop); the PAUSE leg is
NOT bounded until F-A items (1)/(4) land congc-side. (For the
wait-under-rank composition question: no wait is added under
heap rank 3 - m_worldLock is not held across §A.3 windows,
VMManager.cpp:227-231 - or under api rank 3, CG-I16 + the §LK
negative edges; the added waits sit under slot(4b)+GCL(2)
only.)
RULING: the marker is PROMOTED to a blocking SHIP GATE in the
congc §13.5(5) style, direction reversed (recorded in the files
this side owns): C1 and ANY useConcurrentSharedGCMarking stage
implementing the §9.1(2) pause MUST NOT ship until the congc
owner records F-A items (1) (timed, per-quantum-sampled pause)
and (4) (CG-T8 wedged-marker arm) - back-cites congc ANNEX
CGS2.4(a) + CGT1. Body rule-5 [r35] SHIP GATE sentence = the
index; CGS2A.4(a) carries the in-place [r35] note.

## G-B (major, REAL) - 'supported fan-in' referenced at four
## sites, defined nowhere: the F-B queue-term bound was
## non-falsifiable and the owed storm arm uncharterable

Grep across SPEC-ungil{,-history}, SPEC-congc{,-history},
SPEC-nativeaffinity{,-history} and CONGC-HANDOUT.md: the term
existed only at the r34 citation sites (r34 body :278; this
file's F-B ruling; the nativeaffinity r10 coordination note) -
no numeric value, no formula, no owning annex; the definition
obligation dangled between owners (term introduced ungil-side,
cap congc-adjacent). Confirmed consequences: the boundedness
claim degenerated to 'bounded by the 30s fail-stop' for ANY k;
the storm arm had no pass/fail threshold; and the per-winner
terms the cap multiplies carry no time bound of their own
(window duration includes cooperative mutator convergence).
RULING (option (b) of the finding - the honest downgrade; a
number invented here would be unowned authority, congc owns the
slot-arbitration design):
- The queue term is a COUNT bound: requester total <= k x
  (per-winner CGS2.3 sum + one full §A.3 stop window), k =
  earlier winners, NO normative cap. 'Supported fan-in' is
  RETIRED from normative text.
- The SOLE TIME bound is the per-requester 30s fail-stop
  (JSThreadsSafepoint.cpp:512); the adopted VMManager.cpp:553-
  566 reading stands - a legitimate >30s queue fails LOUDLY.
- The CG-T8 storm arm is RE-CHARTERED ATTRIBUTION-ONLY: any
  fail-stop fired by queue serialization must name the
  requesting VM (F-A item (3) threading); no
  no-fail-stop-below-cap claim survives, none is testable.
- The owed-congc list gains an OPTIONAL item: IF congc wants a
  tighter-than-30s storm guarantee it must DEFINE a fan-in cap
  in its own annex (proposed home: ANNEX CGT1 row) and
  re-charter the arm; until then attribution-only IS the
  chartered arm.

## G-C (major, REAL; cross-file) - BL1.8 item-2 NL-reacquire
## anchor sits inside the caller-held GCL

Fixed in SPEC-nativeaffinity-history.md r11 (ANNEX BL1.8
AMENDMENT: reacquire re-pinned AFTER the funnel's caller-side
GCL release - election Heap.cpp:4606 / poll tail :4669,:4673 /
the F28 successor's final release - with the NORMATIVE
no-heap-rank>=2-held sentence (textual NA-I10 equivalence), the
NL-acquire debug assert / U20 NL-edge lint obligation, and the
stale :4955 anchor refreshed to :5031). Ungil-side consumer
note: §LK row 9d's CGS2.2 chain walk ("GC-conduct NL>GCL edge
REMOVED - nativeaffinity BL1.8") is RE-GROUNDED by that re-pin.
Before it, BL1.8's literal item-2 anchor ("after the conduct's
access-reacquire tail") licensed an NL reacquire in the window
[post-Heap.cpp:5031, pre-:4606] - a heap-rank-2 holder ACQUIRING
NL, contradicting NA-I10 and closing the ONE constructible cycle
in the rebuilt merged table (T1 = BL1.6 §A.3 conductor holding
NL, blocked in its HBT4-bracket tryLock loop :5568-5590 on T2's
GCL; T2 = sync requester holding GCL post-final-close, parked in
the NL reacquire behind T1; outcome = deterministic 30s
watchdogAssertStopProgress fail-stop, JSThreadsSafepoint.cpp:512
- loud, but a real deadlock from BINDING text, and CG-I19's
depth==0 assert fires at conducting ENTRY only, not at the
reacquire site). No ungil body change: row 9d already states the
edge removal, which the r11 re-pin makes textually true.

## G-D (major, REAL) - r34 moved body anchors and shipped no
## cite-anchor refresh ledger

The r34 edits shifted everything from §A.3 rule 3 onward by 1-3
lines; the REV 34 record carries no ledger (unlike REV 33),
while SPEC-nativeaffinity-history.md r9 re-anchored its two
drifted cites explicitly "per the ungil r33 ledger" and directs
ALL other SPEC-ungil.md:NNN cites through it - so
nativeaffinity-side cross-refs mis-resolved by 1-3 lines. Fixed
by the ledger below, which covers the r34 AND r35 moves
cumulatively (old column = the values cross-spec readers
currently hold from the r33 ledger/record).

## Cite-anchor refresh ledger (r35; covers the r34+r35 moves;
## for cross-spec readers - congc/nativeaffinity cites into
## SPEC-ungil.md re-read here; the r33 ledger re-reads THROUGH
## this one; pattern per congc ANNEX CGD7.4)

| old (r33 ledger/record) | new (rev 35) | anchor |
|---|---|---|
| :232-248 | :229-247 | §A.3 rule 3 HBT4 order pin |
| :254-292 | :253-295 | §A.3 rule 5 (R1.i bracket) |
| :311-321 | :314-323 | §A.3 rule 8 (F8 GC-stop revert) |
| :834-915 | :837-917 | §LK merged lock table |
| :856-863 | :859-866 | §LK row 9c (CMS lock) |
| :864-879 | :867-882 | §LK row 9d (marking-internal group) |

Also re-verified this rev (r33-ledger rows below the moved
region; all shifted, added for completeness):
| :846-855 | :849-856 | §LK.4b slot-mutex row |
| :893-899 | :896-902 | §LK long-hold NLS row |
| :751-759 | :754-762 | §K.5 haveABadTime rule |
| :659-661 | :662-664 | §F.6(e) spawned no-nested-VM |
Unmoved: §A.1.5 index line :104 (the F-D home's body line;
above the first r34 edit). nativeaffinity r9's two re-anchored
cites (:311-321, :834-915) re-resolve via this ledger; its r11
record carries the coordination note.

## ANNEX CGS2A AMENDMENT (r35; BINDING; in-place [r35] notes at
## CGS2A.3 and CGS2A.4(a))

- CGS2A.3: queue term DOWNGRADED to a COUNT bound; 'supported
  fan-in' RETIRED; sole TIME bound = the per-requester 30s
  fail-stop; storm arm ATTRIBUTION-ONLY; congc cap optional
  (G-B).
- CGS2A.4(a): F-A items (1)/(4) PROMOTED to a BLOCKING ship
  gate on C1/§9.1(2)-pause stages (G-A).

## Rev-35 section deltas

- Header: rev 35 stamp.
- §A.3 rule 5: [r35] SHIP GATE sentence (index of G-A); queue
  term re-worded to the COUNT-bound form (G-B); byte-funding
  compressions (license: full text stays in the cited
  annex/record; no semantic change): "conductor-as-client =
  congc §9.1(8)/CGD7.1" -> CGS2A.4(a); "phase != NotRunning
  BLOCK in pauseConcurrentMarkingForForeignStop" -> "phase-gated
  pause BLOCK" (full ctor obligation: CGS2A.4(a) + congc
  §9.1(2)); WATCHDOG COVERAGE routine name + "not nullptr" ->
  REV 34 items (1)/(3); WAIT BOUND "the frozen ... reading" +
  "(no lite/client ptr cached)" -> CGS2A.3 / REV 34 F-A
  (U32 sub-claim); "r17 F1 + r18 F1, FULL text: history
  ANNEXES" -> "r17/r18 F1 FULL: ANNEXES"; "OWED storm arm" ->
  "ATTRIBUTION storm arm" (G-B re-charter); "HBT4.2 in-place
  [r33] strike" -> "HBT4.2 [r33] strike".
- No SD additions; no §T changes; CG-I0 oracle holds: every
  [r35] clause is flag-gated useConcurrentSharedGCMarking text
  with no flag-off observable delta.
- Body measured 49999/50000 bytes post-edit; this history file
  uncapped.

## §N.5 LANDED SHAPE — supersession + deferral record (CVE-close review round, 2026-06-10)

Both-sides record for the generator resume-claim landing (MC-TEAR S6 /
MC-PRIM P5 closure rows), required because the landed code renegotiated
frozen BINDING text without a history entry (the r17 F6 process failure;
this entry is the remedy).

### (1) Lowering-shape supersession (r11 F4 / r17 F5 vs landed)

- RULED (r11 F4, SPEC-ungil-history.md:1816-1845; r17 F5, :3332-3370;
  SPEC-ungil.md §N.5 "emitted UNCONDITIONALLY all modes; LOWERING
  mode-keyed"): twin intrinsics @atomicInternalFieldClaim/Publish in ONE
  uniform bytecode shape for all modes; LLInt/Baseline branch on the
  JSCConfig gilOffProcess byte at LOWERING time (gilOff arm MAY be a host
  op in v1); DFG/FTL lower AtomicInternalFieldClaim/Publish NODES to an
  inline seq_cst strongCAS / release store.
- LANDED: mode-keyed EMISSION — a @gilOffProcess jsBoolean CONSTANT in the
  UnlinkedCodeBlock constant pool (BytecodeIntrinsicRegistry.cpp), builtins
  branch on it; @claimGeneratorResume/@publishGeneratorResume are link-time-
  constant HOST FUNCTIONS reached as ordinary calls in EVERY tier (no DFG/FTL
  nodes, no inline CAS); BytecodeGeneratorification emits a different
  generator-body stream (relocated unclaim) when gilOff.
- DISPOSITION: ACCEPTED for v1 with conditions. Flag-off identity holds (the
  constant-false branch keeps GIL-on/flag-off on the landed inline sequence
  and the landed bytecode order verbatim). The gilOff host-call cost is
  recorded-not-gated (§B.5); the r17 F5 intrinsic+node form is the NAMED PERF
  CONTINGENCY if the gilOff-arm cost matters. Mode-keyed bytecode is sound
  ONLY with cache partitioning — condition (2) below. Golden gates: no
  re-baseline needed (flag-off bytecode unchanged, unlike the r11 F4 uniform
  shape which required one).
- CONDITION (2), LANDED THIS ROUND: the disk bytecode cache version now
  mixes the gilOffProcess derivation (JSCBytecodeCacheVersion.cpp
  mixInGILOffProcessDerivation), so a cache built in one mode can never
  replay into a process in the other mode (a flag-off cache replayed GIL-off
  would have silently disabled the entire §N.5 protection).

### (2) r15 F1 release-store obligation — DISCHARGED this round

The landed relocation alone made the unclaim last in PROGRAM order only;
r15 F1 (UNGIL-HANDOUT §N.5: "Running->SuspendedX and Running->Completed
MUST be release stores ... in ALL tiers") was not satisfied (plain
op_put_internal_field in every tier; arm64 store-store reorder + DFG/FTL
disjoint-abstract-heap motion = the torn-frame hazard verbatim). NOW:
gilOffProcess, op_put_internal_field performs a store-store fence before
the field store in ALL tiers — LLInt64 (config-byte branch + memfence,
GILOFF_TLS targets; non-TLS 64-bit fail-stops pre-bytecode), Baseline +
DFG (compile-time-keyed storeFence()), FTL (B3 write-empty fence = compiler
barrier + dmb ishst/nothing). Running->Completed was already the
publishGeneratorResume acq_rel CAS. Flag-off delta: zero in the JITs, one
byte-test branch in LLInt (accepted delta-(a) class). The relocation also
FAILS CLOSED now (RELEASE_ASSERT instead of silently keeping the pre-save
order) and its base-register validation uses
BytecodeGenerator::generatorRegister(), extending the reorder to
wrapper-less async functions (generator in a LOCAL VAR — previously
silently skipped), async generator bodies, and module bodies.
STILL OWED: arm64 + TSAN verification rung for
mc-prim-generator-resume-claim/mc-tear-generator-resume
(SPEC-ungil-audit-N7.md:239 acceptance note).

### (3) Annex N7 R7 coverage — PARTIAL; async-generator deferral

N7 R7 marks JSGenerator, JSAsyncGenerator AND async-function frames COVERED
§N.5. Landed claim sites: GeneratorPrototype.js + JSIteratorHelperPrototype.js
only. AsyncGeneratorPrototype.js's resume head is still the plain
check-then-store (:37/:78/:83) with plain queue-field writers, reachable
synchronously from any thread holding the cell — two racing agen.next()
calls remain two simultaneous owners. R7 is therefore PARTIAL until either
(a) the claim/publish shape lands on the async resume heads, or (b) an
owner-affinity ConcurrentAccessError ruling narrows R7. Owed gating
artifact: an async-generator clone of mc-prim-generator-resume-claim.js.
Async-FUNCTION frames: resume is promise-job-driven (§E.1b thread
confinement) + the yield-side ordering above; the claim question is
narrower and rides the same deferral. Tracking rows updated:
CVE-AUDIT-STATUS.md item 3 + MC-TEAR table row; map-MC-TEAR.md S6;
map-MC-PRIM.md fix-queue item 2.

### §N.5 review-round amendments (2026-06-10, post-close verification round)

1. **Owed gating artifact DELIVERED:** the async-generator clone of
   mc-prim-generator-resume-claim.js now exists —
   JSTests/threads/cve/mc-prim-async-generator-resume-claim.js, registered
   [EXPECTED-FAIL GIL-off] in CVE-AUDIT-STATUS.md TO-EXECUTE. Observed
   GIL-off: SEGV 3/3 (two threads racing agen.next() through drained
   microtask queues — the unclaimed AsyncGeneratorPrototype.js resume head
   is memory-unsafe, not merely a logic race). Green GIL-on. The R7 PARTIAL
   deferral in entry (3) above is now mechanically pinned; it flips to
   passing only when closure (a) claim/publish on the async heads (incl.
   the JSMicrotask.cpp setState cluster) or (b) the owner-affinity CAE
   ruling lands.
2. **Claim-leak guard LANDED (new finding, fixed):** the §N.5 claim and the
   protected region were not coextensive — an engine-raised exception
   between a successful @claimGeneratorResume and entry into
   @generatorResume's try (deterministic: stack-overflow RangeError from
   @generatorResume's prologue check, reachable by resuming at depth) left
   the owner token in the State field forever; every later claim on every
   thread read canonical Executing — a permanent, attacker-triggerable
   cross-thread bricking of a shared generator, and a divergence from
   GIL-on/flag-off (which stays resumable). Fix: publish-on-throw wrappers
   around the @generatorResume call in ALL claiming callers
   (GeneratorPrototype.js next/return/throw,
   JSIteratorHelperPrototype.js next/return), behind @gilOffProcess
   (flag-off bytecode unchanged). Idempotent by construction: publish CAS
   ourToken->Completed cannot clear a rival's token and no-ops after
   @generatorResume's own catch/epilogue published. Gating test:
   JSTests/threads/cve/mc-prim-generator-claim-leak-stack-overflow.js.
   Recorded residuals: the fail-safe CLOSES the generator where vanilla
   would leave it resumable (benign divergence); a stack overflow inside
   the catch's publish call itself can still leak in pathological depths.
3. **Test-banner correction:** mc-prim-generator-resume-claim.js's banner
   claimed §N.5 covers "generator / async-function resume" via
   @atomicInternalFieldClaim — corrected to the landed shape (sync
   generators + iterator helpers via @claimGeneratorResume; async arm open
   and pinned by the clone test).
4. **Generator-family verification bar corrected:** the family-1 "20/20
   full tiers" numbers were default-threshold-only; tier-forced runs hit
   the (now fixed) FTL data-IC direct-call register clobber in both modes
   — see SPEC-jit-history.md §21 and CVE-AUDIT-STATUS.md (2026-06-10
   closure round, item 2 correction). Post-fix the family is green at
   default AND forced thresholds.
