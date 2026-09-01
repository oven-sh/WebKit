# AUDIT-checktraps — CheckTraps de-jank: invalidation-point modeling + precise jettison

Task: `checktraps-dejank-invalidation-point` (UNGIL §K.5 / SPEC-jit I21, AB-10
closure). Status: IMPLEMENTED + AMENDED ×2.
Amend round 1: 2 blockers (epoch bump edge) + 3 majors (walk lock shape, P10
coverage claim, audit rows) addressed; see §2/§4/§7.
Amend round 2 (this round): 2 blockers + 2 majors addressed:
  (B1) missed third conductor entry point — the evaporable-GC-stop-evidence
       reroute in `stopTheWorldAndRun`'s worldIsStopped() branch handed the
       RAW work closure to `jsThreadsThreadGranularStopTheWorldAndRun`; now
       wrapped with the identical in-window epoch bump (§2/§3, BUMP-EDGE LAW);
  (B2) P10c allocation-class park hole — CLOSED for the chartered widening
       via option (iii): `jsThreadsParkableSlowPathClobbersHeapFacts`
       (DFGClobberize.h) makes allocation-class and other parkable-slow-path
       nodes clobber heap facts gilOff, consumed by clobberize AND the
       abstract interpreter from ONE predicate (§4 P10c; residual P10c-R);
  (M1) P10b rejoin residual — per-caller modeling proven site-by-site, with
       the two non-clobbering caller classes (MultiPutByOffset transition/
       realloc variants; CheckTierUp* tier-up service) moved INTO the
       parkable-slow-path clobber (§4 P10b);
  (M2) §7.1 spin-loop visibility — INTERIM DEFAULT landed: value-heap writes
       at the poll keep plain field/element/global/closure/collection reads
       non-hoistable across polls (partial de-jank per §7.1's YES branch);
       shape facts stay hoistable. Ruling still required to finalize, but the
       hang-regression class is closed pending it (§7.1).
Build + ladder verification PENDING (write-only rounds; one builder compiles
after). OPEN ITEMS BLOCKING DONE: §7 item 1 (ruling must be recorded — the
interim default removes the regression but the contract must be ratified)
and §7 item 7 (P10c-R residual: systemic AHA-edge closure, option (ii)).

## 1. What changed

GIL-off (`useJSThreads && !useThreadGIL`), DFG/FTL `CheckTraps` is no longer
modeled as `read(World)/write(Heap)` (the clobberWorld haymaker that killed all
heap-fact hoisting across every poll site). It is now modeled and compiled as
an **invalidation point at the poll's rejoin**, and the §K.5 park-site
semantics (the conductor may rewrite ANY heap fact during a §A.3 window) are
enforced by **precise jettison**: every conductor-side heap-fact rewrite that
can occur inside a stop window bumps a process-global epoch
(`JSThreadsSafepoint::noteConductorHeapFactRewrite`), and a mutator whose park
in `VMTraps::handleTraps` overlapped a bump jettisons its own on-stack
optimizing-JIT code on resume — jettison fires the CheckTraps invalidation
points, so the resumed mutator OSR-exits at the poll BEFORE reusing any
hoisted heap fact.

GIL-on flag-on keeps the previous conservative `read(World)/write(Heap)` model
(see §6, GIL-on row). Flag-off, every new branch is dead: codegen is
byte-identical (LAW).

### The modeling (DFGClobberize.h `CheckTraps`)

```
read(InternalState); write(InternalState);            // the poll itself, all modes
flag-on, GIL-off, node->origin.exitOK:
    write(Watchpoint_fire);                            // ORDER LOAD-BEARING, see below
    write(SideState);
    write(NamedProperties); write(IndexedProperties); // §7.1 INTERIM DEFAULT
    write(Butterfly_publicLength); write(Absolute);   // (amend round 2):
    write(JSMapFields); write(JSSetFields);           // plain user-visible
    write(JSWeakMapFields); write(JSWeakSetFields);   // DATA stays poll-
    write(JSInternalFields); write(JSDateFields);     // bounded; SHAPE facts
    write(RegExpObject_lastIndex);                    // remain hoistable
    def(HeapLocation(InvalidationPointLoc, Watchpoint_fire), LazyNode(node));
otherwise flag-on:  read(World); write(Heap);          // unchanged conservative model
```

The §7.1 interim writes mean the de-jank is PARTIAL by design pending the
memory-model ruling: structure / butterfly pointer / vector length / indexing
type / typeinfo facts (the CheckStructure + GetButterfly + CheckArray LICM
wins this change targets) cross polls; plain field/element/global/closure/
collection VALUE reads do not, so spin-on-plain-flag loops cannot be hoisted
into hangs. `IndexedProperties` is the supertype of the four indexed kinds
plus DirectArguments/Scope/TypedArray properties — plain typed-array element
reads stay poll-bounded too (plain non-SAB ArrayBuffers are shareable across
Threads here and get no ECMA SAB memory-model cover). Note value STORES also
cannot sink below a poll (the same writes block store motion), preserving
the old model's store-visibility-at-polls behavior for the listed heaps.

Additionally (amend round 2, P10b/P10c): a SEPARATE pre-switch clobber in
clobberize makes every parkable-slow-path node write(Heap) gilOff, driven by
the single-source predicate `jsThreadsParkableSlowPathClobbersHeapFacts`
(DFGClobberize.h) which the abstract interpreter consumes too
(`didFoldClobberStructures()` at the top of `executeEffects`, keeping the
DFGCFAPhase AI-clobberize agreement assert — didClobberOrFolded vs
writesOverlap(JSCell_structureID) — green). Side effect, sound and accepted:
`InvalidationPointInjectionPhase` keys on writesOverlap(Watchpoint_fire), so
parkable nodes gilOff now get a trailing standalone InvalidationPoint exactly
like calls always have — which incidentally narrows the rejoin-to-next-IP
stale-consumption window after those nodes.

**CSE poll-deletion hazard, closed by ordering**: `def()` of
`InvalidationPointLoc` lets CSE replace a later node def'ing the same location
with an earlier one (`DFGCSEPhase.cpp` `def(HeapLocation,...)`,
`m_node->replaceWith`). If CheckTraps def'd without the preceding
`write(Watchpoint_fire)`, a second CheckTraps (straight-line, or its own def
reaching around a loop backedge) could be CSE'd away — a deleted poll is a
missed safepoint (STW watchdog timeout / GIL never yielded). The
`write(Watchpoint_fire)` is processed before the `def` within the node's
clobberize walk, kills any prior `InvalidationPointLoc` availability, and so a
CheckTraps can never be replaced. A later **plain** `InvalidationPoint` CAN be
CSE'd into a preceding CheckTraps — sound, because CheckTraps codegen emits a
real watchpoint label whenever the def is emitted (gate identity, §3).

This deliberately subsumes the parser-emitted standalone
`ExitOK; InvalidationPoint` pair that `ByteCodeParser::handleCheckTraps`
(DFGByteCodeParser.cpp:7037) appends after every flag-on CheckTraps: CSE may
now fold that standalone IP into the CheckTraps node; either way exactly one
invalidation point guards the poll rejoin. NOTE: the comment at
DFGByteCodeParser.cpp:7048 ("The flag-on CheckTraps clobbers the heap") is now
stale for the GIL-off leg; the file is outside this change's ownership — fix
the comment in a follow-up.

### Abstract interpreter (DFGAbstractInterpreterInlines.h)

Same gate; mirrors the `InvalidationPoint` case
(`setStructureClobberState(StructuresAreWatched)` +
`observeInvalidationPoint()`) instead of `clobberWorld()`. Conservative
`clobberWorld()` retained for GIL-on and for any `exitOK == false` CheckTraps.

### Codegen

- DFG `SpeculativeJIT::compileCheckTraps`: after the trap-bit branch +
  `operationHandleTraps` slow path, gate-matched call to
  `compileInvalidationPoint(node)` (covers the unlinked-DFG
  `JITData::isInvalidated` form too). Fast path and slow-path return both flow
  through the watchpoint label.
- FTL `compileCheckTraps`: gate-matched `compileInvalidationPoint()` emitted in
  the continuation block (rejoin of fast path and lazy slow path). B3-level:
  the patchpoint is `exitsSideways` / reads-top exactly like a plain
  InvalidationPoint; load motion across it stays sound by the standard
  IP argument (fall-through ⇒ no invalidation fired ⇒ pre-poll facts valid).
  The I16 pinned-write machinery (`loadTaggedButterflyPinnedForWrite`) is
  untouched and still required (it guards ordinary cross-thread OM races, not
  conductor windows).

### Gate identity (MUST stay in lockstep — drift here is unsound)

`Options::useJSThreads() && !Options::useThreadGIL() && node->origin.exitOK`
at all four sites:

| Site | File |
|---|---|
| clobberize def | Source/JavaScriptCore/dfg/DFGClobberize.h (CheckTraps case) |
| abstract interpreter | Source/JavaScriptCore/dfg/DFGAbstractInterpreterInlines.h (CheckTraps case) |
| DFG codegen | Source/JavaScriptCore/dfg/DFGSpeculativeJIT.cpp `compileCheckTraps` |
| FTL codegen | Source/JavaScriptCore/ftl/FTLLowerDFGToB3.cpp `compileCheckTraps` (`m_origin.exitOK` == same node's origin) |

If clobberize def's but codegen skips, CSE can delete a standalone IP with no
label to replace it → unsound. If codegen emits but clobberize stays
conservative → merely jank. Any future edit must move all four together.

## 2. Enforcement machinery (precise jettison)

| Piece | File | What it does |
|---|---|---|
| Epoch | bytecode/JSThreadsSafepoint.cpp (decls in runtime/VMTraps.h): `conductorHeapFactRewriteEpoch` / `noteConductorHeapFactRewrite` | Process-global `std::atomic<uint64_t>`. Cross-thread ordering rides the park/resume synchronization + F5 ISB. |
| **In-window bump (LOAD-BEARING, gilOff)** | BOTH conductor entry points into `jsThreadsThreadGranularStopTheWorldAndRun` (JSThreadsSafepoint.cpp): (a) the `stopTheWorldAndRun` gilOff reroute, and (b) — AMEND ROUND 2 BLOCKER FIX — the evaporable-GC-stop-evidence reroute inside the `worldIsStopped()` branch (the cve-structureid fail-closed path that requeues a non-GC-conducting Class-A caller at §A.3 arbitration). In both, `work` is wrapped so the bump runs AFTER the window's body and BEFORE the conductor publishes resume (stop-word clear / ticket wake), unless the requester opened the suppression scope | This is the bump a mutator parked BY the window observes. AMEND-ROUND-1 BLOCKER FIX: the previous design bumped only at publication (ctor), which lands strictly before the trap bits that park the window's own victims — those victims sample the epoch in `handleTraps` post-bump and compared equal on resume, deterministically missing every CA/OM-window victim. Bump-in-(entry-sample, exit-compare) requires bumping while the world is still stopped. AMEND-ROUND-2 BLOCKER FIX: entry point (b) previously handed the RAW closure to the conductor — its requesters are exactly Class-A nuking/patching callers, and its victims' only later edge was the requester's dtor bump (post-resume for a conducted window), racing the victims' exit compare. RULE going forward: EVERY call into `jsThreadsThreadGranularStopTheWorldAndRun` on behalf of a potential heap-fact rewriter must pass the wrapped closure; grep for that callee when adding entry points. |
| In-window bump, inline leg | `stopTheWorldAndRun` R1.h already-stopped branch (JSThreadsSafepoint.cpp): post-`work` bump under the witness scope, gilOff, suppression-gated | A request that ran inline under an OUTER stop (GC stop, open §A.3 window) bumps before the OUTER stop's resume edge (we run on the thread that ends that stop). |
| Entry/exit-edge bumps | `ClassAStopWatchdogContext` ctor AND dtor (JSThreadsSafepoint.cpp), suppression-gated | Ctor (entry edge): covers mutators ALREADY inside `handleTraps` at publication, plus GIL-on legs. Dtor (exit edge): covers inline fires under an outer stop that publish a context but never reach `stopTheWorldAndRun` (e.g. a future direct `fireAllNow` under already-stopped evidence); for conducted windows it is post-resume and only belt-and-braces (worst case: one spurious jettison on an unrelated parked thread). Neither edge is sufficient alone; the in-window bump is the load-bearing one. |
| Suppression | `PureCodeLifecycleStopWindowScope` (decl runtime/VMTraps.h, def JSThreadsSafepoint.cpp); opened in `CodeBlock::jettison` | Jettison-only windows rewrite CODE, not heap facts; without suppression every reoptimization would cascade into a process-wide on-stack jettison of all parked mutators. Thread-local depth; gates the in-window, ctor, and dtor bumps identically (the wrapped closure runs on the requester's own stack). |
| Mutator check (polls) | `VMTraps::handleTraps` (runtime/VMTraps.cpp): epoch sampled at entry, compared in a scope-exit on every exit path | The park (NeedStopTheWorld `notifyVMStop`, GIL yield, D9 quanta) happens between sample and compare; with the in-window bump, any window that parked this thread compares unequal. |
| Mutator check (class-2 native waits) | `JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld` (JSThreadsSafepoint.cpp): epoch sampled before publishing quiescence, compared after resume | AMEND-ROUND P10 FIX: access-holding runtime waits (JSObject.h/JSObjectInlines.h transition spins, FunctionRareData, ScriptExecutable) park here with DFG/FTL frames live; their rejoin is NOT a poll with an IP, so this check narrows but does not fully close that class — see P10b/P10c. Compile-side callers (no `VMLite`) skip the walk: the VM-word `topEntryFrame` there can belong to a RUNNING foreign thread. |
| The walk | `VMTraps::jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite` (runtime/VMTraps.{h,cpp}) | NOT gated on the one-shot `m_needToInvalidateCodeBlocks` (under N mutators every overlapped thread must walk its OWN stack); compiled regardless of `ENABLE(SIGNAL_BASED_VM_TRAPS)`. AMEND-ROUND LOCK FIX: collects optimizing-JIT blocks under the codeBlockSet lock, DROPS the lock, then jettisons — `CodeBlock::jettison` re-enters `stopTheWorldAndRun` (section 5.3 choke point), and conducting a window while holding the process-shared codeBlockSet lock deadlocks against siblings that take the same lock on their way to their park (`handleTraps`' breakpoint-sweep walk). Collected blocks are pinned by this thread's own stack (conservative scan), so the post-unlock jettison is safe; the list is deduplicated so each block opens at most one nested window. |

Cascade-termination argument: the mutator-side jettison re-enters
`stopTheWorldAndRun` via `CodeBlock::jettison` (section 5.3 choke point) but
that nested window is suppressed from the epoch, so jettisons never re-trigger
each other; after one generation of post-window jettisons every affected stack
has no optimizing frames and the walks no-op.

## 3. Firing-site audit (conductor-side heap-fact rewrites inside stop windows)

| # | Rewrite | Site | GIL-off coverage | GIL-on flag-on coverage | Row status |
|---|---|---|---|---|---|
| HBT | haveABadTime butterfly conversion (fast indexing → (SlowPut)ArrayStorage on OTHER threads' live objects) | `JSGlobalObject::haveABadTimeImpl` (runtime/JSGlobalObject.cpp) | In-window wrapped-closure bump (the body runs inside the §A.3 window) + the explicit in-body bump (redundant gilOff) | Explicit in-body bump (body runs inline under the GIL with siblings parked at polls; no context published; the bump runs inside the GIL tenure, i.e. before any parked sibling resumes) — GIL-on modeling is still conservative, so it is belt-and-braces there | COVERED |
| CA | Class-A watchpoint fire (structure retags / section 5.6 step-5 work) | `WatchpointSet::fireAllUnderClassAStop` (bytecode/Watchpoint.cpp): branch (2) publishes `ClassAStopWatchdogContext("WatchpointSet Class-A fire")` and conducts via `stopTheWorldAndRun`; branch (1) fires inline under already-stopped evidence | Branch (2): in-window wrapped-closure bump (pre-resume — AMEND-ROUND FIX; the previous ctor-only coverage missed every mutator parked by the window itself, see §2). Branch (1): context dtor bump (pre-resume w.r.t. the outer stop; the fire never reaches `stopTheWorldAndRun`) | Conservative model stands (clobber) — covered regardless of bumps | COVERED (gilOff, re-derived); N/A-conservative (gilOn) |
| OM | OM transition stop (Class-A object-model rewrites) | runtime/ConcurrentButterfly.cpp publishes `ClassAStopWatchdogContext("OM transition stop")` and conducts via `stopTheWorldAndRun` | In-window wrapped-closure bump (pre-resume — AMEND-ROUND FIX, same hole as CA: ctor-only deterministically missed window-parked siblings whose LICM-hoisted CheckStructure+GetButterfly facts reference the transitioned object) | Conservative model stands | COVERED (gilOff, re-derived) |
| DBG | Debugger entry / debugger JS during a stop | debugger/Debugger.cpp publishes `ClassAStopWatchdogContext("Debugger STW walk")`; plus explicit bump in `VMTraps::handleTraps` `NeedDebuggerBreak` service | In-window wrapped-closure bump for the conducted window + service-side bump (runs during servicing, i.e. inside the stop, pre-resume); the legacy `m_needToInvalidateCodeBlocks` one-shot (first servicing thread only) is superseded for siblings by the epoch check | Service-side bump; conservative model stands | COVERED (re-derived) |
| JTL | CodeBlock jettison window (reoptimization, weak-ref, etc.) | bytecode/CodeBlock.cpp `jettison` | Rewrites code only, NO heap facts → deliberately suppressed (`PureCodeLifecycleStopWindowScope` gates the in-window, ctor, and dtor bumps alike) | n/a | EXCLUDED BY DESIGN |
| NEW | Any future stop-window requester | — | RULE: conduct through `stopTheWorldAndRun` WITHOUT the suppression scope (the wrapped closure then bumps in-window), or — for work run inline under an outer stop — publish a `ClassAStopWatchdogContext` (dtor bump) or bump explicitly INSIDE the stopped region. A publication-time-only bump is never sufficient (§2). Opening the suppression scope requires proving the window rewrites no heap fact. | — | POLICY |

Ordinary cross-thread JS mutation (another mutator's plain writes) is NOT a
firing site: it occurs outside stop windows GIL-off anyway, and soundness
against it is the object-model workstream's contract (snapshot-sound reads,
pinned-write predicates), unchanged by this task.

## 4. Coverage audit — every traps-check / poll site

| # | Site | Tier | Mechanism | Verdict |
|---|---|---|---|---|
| P1 | `op_enter` → `handleCheckTraps()` (DFGByteCodeParser.cpp:7635) | DFG/FTL | CheckTraps node → invalidation-point modeling + codegen IP (this change); parser's standalone IP may be CSE-folded into it | INVALIDATION-COVERED (gilOff, exitOK); CONSERVATIVE otherwise |
| P2 | `op_check_traps` (loop-hint adjacency) → `handleCheckTraps()` (DFGByteCodeParser.cpp:10156) | DFG/FTL | same | INVALIDATION-COVERED (gilOff, exitOK); CONSERVATIVE otherwise |
| P3 | `LoopHint` node (DFGByteCodeParser.cpp:10150) | DFG/FTL | Not a poll (no trap check; `write(SideState)` only). The poll is the adjacent P2 CheckTraps emitted for `op_check_traps`. | NOT A POLL — covered via P2 |
| P4 | DFG `compileCheckTraps` (DFGSpeculativeJIT.cpp; dispatched DFGSpeculativeJIT64.cpp:6088) | DFG | Emits poll + (gate-matched) IP at rejoin; unlinked form uses the explicit `JITData::isInvalidated` test | INVALIDATION-COVERED |
| P5 | FTL `compileCheckTraps` (FTLLowerDFGToB3.cpp) | FTL | Emits poll + (gate-matched) IP patchpoint in continuation block | INVALIDATION-COVERED |
| P6 | Standalone `InvalidationPoint` nodes (signal-traps parser form, post-call IPs) | DFG/FTL | Unchanged stock machinery; jettison patches them like always | INVALIDATION-COVERED (stock) |
| P7 | Baseline `op_check_traps` (jit/JITOpcodes.cpp:1859, thunk :1896) | Baseline | No cross-poll code motion exists in baseline: no LICM/CSE; property/array accesses re-check structure/shape per access via ICs; no heap fact is cached in registers across the poll by construction | SAFE BY CONSTRUCTION (conservative tier) |
| P8 | LLInt `op_check_traps` (LowLevelInterpreter.asm:3077) | LLInt | Interpreter re-loads everything per op; no facts survive any op boundary | SAFE BY CONSTRUCTION |
| P9 | CLoop form of P8 | CLoop | Same as P8 (CLoop additionally suppressed wholesale per TSAN policy; unused in production) | SAFE BY CONSTRUCTION |
| P10a | Off-JS park sites reached ONLY through DFG/FTL `Call`-class nodes (JSLock DAL2 bracket; D9 GILDroppedSection parks: Atomics.wait, property-wait, Lock/Condition/Thread joins — all host-function calls) | native | DFG/FTL model calls as clobbering (read(World)/write(Heap)), so no heap fact is hoisted across the node whose slow path parks. Parks here that overlap a rewrite window need no jettison for fact-freshness. | SAFE BY CONSTRUCTION (call boundary clobbers) |
| P10b | Class-(2) access-holding runtime waits: `parkSitePollAndParkForStopTheWorld` callers (JSObject.h/JSObjectInlines.h transition spins, FunctionRareData, ScriptExecutable, compile-side spins) | native | AMEND-ROUND-1 FIX: the helper brackets its park with the epoch check and runs the on-stack jettison on overlap (mutator lites only). AMEND-ROUND-2 (major): the deferred "each caller's own node modeling" claim is now PROVEN SITE-BY-SITE: **(1) JSObject.h M7(c) get-stabilization spin** — reachable only via GetById-family runtime calls (`GetById`/`GetByIdFlush`/`GetByIdDirect*`/`GetByVal` Generic legs all clobberTop; `TryGetById` writes every heap but RegExpObject_lastIndex — clobbering for every heap-fact location); `GetByOffset`/`MultiGetByOffset` are inline loads with no call and cannot reach the spin. **(2) JSObjectInlines putDirectInternal RESTART spin** — reachable via `operationPutById*` / generic `operationPutByVal` (PutById-family and PutByVal Generic/OOB legs clobberTop; in-bounds PutByVal legs and `PutByOffset`/`PutStructure` are inline stores that never call into putDirectInternal) and via `MultiPutByOffset` transition/realloc variants — those variants are now IN the parkable-slow-path clobber (predicate tests `writesStructures() \|\| reallocatesStorage()`). **(3) FunctionRareData allocation-profile spin** — reachable via `CreateThis`/`CreatePromise`/`CreateGenerator`/`CreateAsyncGenerator` (all clobberTop). **(4) ScriptExecutable `GILOffCompilationLocker` spin** — reachable via Call-class boundaries (clobberTop) and via the tier-up service path; `CheckTierUpInLoop`/`AtReturn`/`AndOSREnter` are now IN the parkable-slow-path clobber (cheap: DFG-only nodes; LICM runs only in FTL-mode plans, which never see them). With every reaching node clobbering heap facts gilOff, no hoisted fact can cross a node whose slow path parks here, so the rejoin needs no IP. | CLOSED (epoch check + per-caller clobber proof) |
| P10c | Heap-access-release parks NOT routed through `handleTraps` or the P10b helper: allocation slow paths blocking on shared-GC handshakes, GC-wait resume funneled through the gated `acquireHeapAccess` (F8/§A.3.2b), GILDroppedSection re-acquire | native | **CLOSED FOR THE CHARTERED WIDENING (amend round 2, option (iii))** — the round-1 row wrongly claimed no option fit the owned files; option (iii) lands in DFGClobberize.h, which this change owns. `jsThreadsParkableSlowPathClobbersHeapFacts` (DFGClobberize.h, single source of truth for clobberize AND the abstract interpreter) makes every GC-parkable-slow-path node clobber heap facts gilOff via a pre-switch `write(Heap)`: all non-Phantom `write(HeapObjectCount)` allocation nodes (NewObject/NewArray/Create*/Materialize*/AllocatePropertyStorage/…, including the leg-precise cases NewTypedArray Int32/Int52, Spread of PhantomNewArrayBuffer/PhantomCreateRest, NewSymbol/NewRegExpUntyped string legs, Compare* StringUse legs, NewResolvedPromise known-non-thenable), the pure-modeled GC-cell allocators (MakeRope/MakeAtomString/StrCat — result stays PureValue-CSE-able; heap facts do not cross), Arrayify/ArrayifyToStructure (butterfly allocation), the hash-storage family (SetAdd/MapSet/MapOrSetDelete/WeakSetAdd/WeakMapSet), MultiPutByOffset transition/realloc variants, and CheckTierUp* (P10b item 4). Phantom* nodes are excluded (no code, no park site). The pre-change straight-line CSE exposure AND the loop-hoist widening are both killed (writes block CSE and LICM alike). AI lockstep: `executeEffects` consumes the same predicate with `didFoldClobberStructures()`, satisfying the DFGCFAPhase AI-clobberize agreement assert without nuking in-block precision. | CLOSED (option (iii)); residual → P10c-R |
| P10c-R | RESIDUAL long tail of option (iii): doesGC-true nodes NOT in the predicate and NOT clobbering in clobberize — e.g. string-producing/rope-resolving ops modeled pure or location-precise (ResolveRope, StringFromCharCode, ToLowerCase, StringSlice, CompareStrictEq string legs, …), whose runtime can trigger GC via `reportExtraMemory`/DeferGC rather than a direct cell allocation; plus AI in-block structure propagation across ALL parkable nodes (pre-existing straight-line exposure — predates this change, unchanged by it) | native | The mechanical audit predicate is: every node where `DFGDoesGC.cpp` returns true must, gilOff, either clobber heap facts in clobberize or be proven park-free. Option (iii) cannot scale to the full doesGC set (~120 further nodes, including hot string ops) without re-introducing haymaker-grade jank — which is the strongest argument that the SYSTEMIC closure is option (ii): the epoch check + jettison at the Heap.cpp AHA resume edge (one site covers every park of this class, including the AI in-block residual, because the on-stack jettison fires the trailing IPs that InvalidationPointInjectionPhase now plants after parkable nodes). Heap.cpp is outside this change's ownership: option (ii) must be RULED AND ASSIGNED before final sign-off. Probability note: these residual parks require the slow path to both trigger a blocking GC handshake AND have a heap-fact window complete over it, strictly rarer than the closed allocation class. | OPEN (narrowed; needs option (ii) ruling/assignment) |
| P11 | `operationHandleTraps` (jit/JITOperations.cpp:3044) → `handleTraps` / `handleTrapsForCurrentThreadIfNeeded` | runtime | THE park covered by the epoch check (sample at entry, compare at every exit via scope-exit) | INVALIDATION-COVERED (this is the enforcement point) |
| P12 | OSR-entry into DFG/FTL at loop hints | DFG/FTL | Entry into a jettisoned block is impossible (jettison unlinks/installs baseline); entry into a live block implies no un-jettisoned rewrite crossed its IPs | COVERED BY JETTISON SEMANTICS |

## 5. exitOK == false CheckTraps

Not expected (parser emits CheckTraps at `op_enter` / `op_check_traps`
boundaries with valid exit origins), but the gate fails CLOSED: such a node
keeps the conservative `read(World)/write(Heap)` + `clobberWorld()` model and
emits no IP. No assert was weakened anywhere in this change.

## 6. Mode matrix

| Config | CheckTraps model | Enforcement |
|---|---|---|
| flag-off | `read/write(InternalState)` only (unchanged) | n/a — byte-identical codegen LAW; every new branch gated on `Options::useJSThreads()` (clobberize/AI/codegen additionally on `!useThreadGIL()`) |
| flag-on GIL-on | conservative `read(World)/write(Heap)` (unchanged from pre-change flag-on) | clobber = trivially correct; the GIL park/hand-off edges are not all routed through `handleTraps`' epoch check, so IP coverage is NOT claimed here. Epoch bumps still fire (haveABadTime, debugger) — inert belt-and-braces. |
| flag-on GIL-off | invalidation-point model (exitOK nodes) + §7.1 interim value-heap writes at the poll (shape facts hoistable, plain data not) + parkable-slow-path nodes clobber heap facts (`jsThreadsParkableSlowPathClobbersHeapFacts`) | epoch + precise jettison + codegen IP; AI mirrors the parkable clobber via `didFoldClobberStructures()` |

## 7. Known risks / follow-ups (for the builder + ladder round)

1. **Spin-loop visibility — INTERIM DEFAULT LANDED (amend round 2), ruling
   still required for final sign-off.** The poll's gilOff model now keeps a
   `write(NamedProperties)`-grade clobber (NamedProperties, IndexedProperties
   incl. typed arrays/scope/arguments, Butterfly_publicLength, Absolute,
   Map/Set/WeakMap/WeakSet/internal/date fields, RegExpObject_lastIndex) —
   exactly this item's YES branch — so plain-flag spin loops CANNOT be
   hoisted into hangs and value stores cannot sink below a poll. The
   user-visible regression class is closed pending the ruling; what remains
   for the spec owner (Jarred) is to RATIFY poll-bounded visibility of plain
   writes as the contract (record in SPEC-ungil/SPEC-objectmodel; keep the
   writes) or reject it (delete the interim write block in DFGClobberize.h's
   CheckTraps case AND `JSTests/threads/checktraps-invalidation.js` Part 4,
   adjust the corpus). Part 4 of that test now exercises the class directly:
   plain (non-Atomics) named-field and indexed-element spins with call-free,
   allocation-free bodies — the regression the round-2 review noted the
   Atomics-based tests could not catch. Do not close the task on a green
   ladder alone until the ruling is recorded.
2. **Gate drift** (§1): four sites must stay in lockstep; no shared predicate
   helper exists yet (clobberize is included in contexts where a new shared
   header was not worth the churn this round). Candidate cleanup: a
   `static inline bool checkTrapsActsAsInvalidationPoint(Node*)` in
   DFGClobberize.h consumed by all four.
3. **`vm.topCallFrame` vs per-lite top frame** in the epoch-check walk: the
   walk mirrors the existing `NeedDebuggerBreak` service
   (`invalidateCodeBlocksOnStack(vm.topCallFrame)` + lite-aware
   `group3Primitives().topEntryFrame`). If per-lite topCallFrame splits from
   the VM word on spawned threads, all three call sites (handleTraps scope
   exit, park-site helper, debugger service) need the same fix.
4. **RESOLVED (amend round): jettison under the codeBlockSet lock.** The walk
   now collects under the lock and jettisons after dropping it (see §2, "The
   walk"). The legacy debugger-path shape (`invalidateCodeBlocksOnStack`
   jettisoning under the lock) and the `handleTraps` breakpoint-sweep
   (`hasInstalledVMTrapsBreakpoints` walk, which jettisons under the same
   lock) remain as inherited stock shapes — both are GIL-on/signal-traps
   territory (the SignalSender never starts gilOff) and are recorded here as
   a follow-up for whoever de-janks the GIL-on leg, not re-derived in this
   round.
5. **Watchpoint_fire write breadth**: `write(Watchpoint_fire)` at every poll
   kills cross-poll CSE of standalone InvalidationPoints (sound, slightly
   conservative). No reader of `Watchpoint_fire` exists in clobberize besides
   `InvalidationPointLoc` defs, so no heap-fact hoisting is affected.
6. **Verification pending** (builder round): flags-off bench-gate delta 0 +
   identity; GIL-off ladder rungs; `JSTests/threads/checktraps-havebadtime-park.js`;
   `JSTests/threads/checktraps-invalidation.js` (including the
   no-watchpoint-masking OM scenario AND the new Part 4 plain-field-spin
   termination check, see test comments); targeted perf measurement of
   hoisting across polls (e.g. `hotDot`/`hotSum` style loop with the clobber
   vs without — note the round-2 interim writes mean field VALUE hoisting is
   intentionally still blocked; measure shape-fact hoisting:
   CheckStructure/GetButterfly/CheckArray). Round-2 additions to verify:
   (a) the parkable-slow-path clobber compiles and the DFGCFAPhase
   AI-clobberize agreement assert stays green in debug gilOff runs (it
   crashes loudly on drift — that assert is the lockstep enforcement for the
   shared predicate); (b) InvalidationPointInjectionPhase's extra IPs after
   parkable nodes gilOff do not regress DFG compile time pathologically;
   (c) the JSThreadsSafepoint.cpp evaporable-evidence reroute bump (entry
   point (b) in §2) — no dedicated deterministic JS test exists (it needs a
   Class-A fire racing a live GC stop on a non-conducting thread); covered
   probabilistically by the congc corpus (`congc-t8-stop-interleaving.js`,
   `congc-t3-barrier-storm.js`) — builder should run those amplified.
7. **P10c park class — CLOSED for the chartered allocation widening (amend
   round 2, option (iii)); residual narrowed to P10c-R.** See the §4 P10c /
   P10c-R rows. What still blocks final "done": option (ii) (AHA-edge epoch
   check + rejoin discipline in Heap.cpp, outside this change's files) must
   be ruled and assigned to cover the doesGC-true long tail and the
   pre-existing in-block AI propagation class. The mechanical audit predicate
   for the residual is recorded in P10c-R.
8. **Spurious-jettison cost of the dtor bump**: a conducted window's dtor
   bump is post-resume, so an unrelated thread parked in `handleTraps` across
   that instant jettisons needlessly. Sound, perf-only, bounded by window
   frequency; revisit only if Class-A fire storms show jettison churn.
9. **Park-site jettison caller constraints** (P10b): the
   `parkSitePollAndParkForStopTheWorld` epoch check runs `CodeBlock::jettison`
   (a nested stop window) on the resumed caller's stack. The helper's
   contract already requires callers to hold no rank-3 locks per quantum;
   the builder round must confirm no class-(2) caller holds a lock a
   to-be-parked sibling needs (same 5.3 rule the §2 walk fix discharges for
   the codeBlockSet lock).
