# INTEGRATE-vmstate — shared-file hunks and checklists (SPEC-vmstate §9)

Manifest of every change the vmstate workstream needs in files it may not
write. Entries are ready-to-paste; insertion points are described against the
current tree. M-numbers follow SPEC-vmstate §9. Entries marked PENDING are
owed by later vmstate tasks; entries below are complete and consumable now.

> **ADJUDICATE FIRST — BLOCKING manifest conflict with INTEGRATE-api 9.2-1.**
> Do NOT apply this manifest's M4/M_opts2 hunks or INTEGRATE-api 9.2-1's
> OptionsList.h deletion blind, in either order: 9.2-1 deletes `useThreadGIL`
> and `useThreads`, which M4's item-13 `RELEASE_ASSERT(!Options::useJSThreads()
> || Options::useThreadGIL())` backstop and M_opts2's alias normalization
> READ. Either blind order breaks compilation of a shared hot file
> (JSLock.cpp or Options.cpp), and the tempting "fix" — deleting the assert —
> silently removes the only mechanical guard against the GIL-off
> shared-main-carrier tid-0 install (flat-butterfly corruption, item 13).
> Resolve cross-WS item 16 (resolutions (a)/(b)) and item 14 BEFORE applying
> whichever manifest lands second. api 9.2-1's "no call site reads
> useThreadGIL (grep-verified)" premise is stale: both reads live in manifest
> text here, invisible to an in-tree grep.

## Task log

- Task 1 (W1 scaffolding) — LANDED in-tree:
  - `Source/WTF/wtf/text/SharedAtomStringTable.h` (new): §4.2 frozen interface
    (128 shards, `shardForHash` on the HIGH 7 of the 24-bit hash, padded
    `alignas(64)` shards with `sizeof >= 128` static_assert), §4.8/§8 ordering
    contract documented in the header comment, `enableSharedAtomStringTable()`
    / `sharedAtomStringTableEnabled()` declarations, and the internal latch
    global `g_sharedAtomStringTableEnabled` (extern; defined in
    AtomStringTable.cpp per §3 R1).
  - `Source/WTF/wtf/text/SharedAtomStringTable.cpp` (new): idempotent latch +
    §4.8 migration (migrate first, then latch: per-entry insertion into
    `shardForHash(existingHash())` under one shard lock at a time, then
    `clear()` of the source set without touching `isAtom`). (Review round 2:
    for bare-tree link-soundness the NeverDestroyed singleton moved to
    AtomStringTable.cpp — always compiled, one instance even in DLL builds —
    and the F4 relaxed query became header-inline in SharedAtomStringTable.h;
    this TU now holds only `enableSharedAtomStringTable()`.)
  - `Source/WTF/wtf/text/AtomStringTable.cpp` (edited, vmstate-owned): defines
    `g_sharedAtomStringTableEnabled`; destructor now implements I17 — shared
    mode ⇒ `RELEASE_ASSERT(m_table.isEmpty())` and SKIPS the
    `setIsAtom(false)` loop (frozen, §4.7 I17 / history R7). Legacy path
    verbatim.
  - Unit tests for I5 (shard selection) and I17/§4.8 (migration) are carried
    in this manifest under M14 below (file content included) because
    `Tools/TestWebKitAPI/**` is outside this agent run's write set.

- Task 2 (W1 atomics, §4.5/§4.4.2/F3) — LANDED in-tree (vmstate-owned files
  only; no new shared-file hunks):
  - `Source/WTF/wtf/text/StringImpl.h`:
    - `StringImplShape::m_hashAndFlags` is now `mutable std::atomic<unsigned>`
      (compile-time, not flag-gated; §4.5). All readers go through a relaxed
      `hashAndFlags()` accessor; flag mutations on possibly-published strings
      (`setIsAtom`, `setNeverAtomize`, lazy `setHash`, `cost()` report bit) use
      idempotent `fetch_or`/`fetch_and(relaxed)`; constructor stores stay plain
      member-init. (Round 4 amendment: `setNeverAtomize` is now a guarded CAS
      returning bool, paired with `trySetIsAtomIfAtomizable` — see the round-4
      log entry and cross-WS item 17.) Size/lock-freedom static_asserted (JIT/LLInt read the field
      as a plain 32-bit load at `flagsOffset()`; offsets unchanged — `.asm`
      `StringImpl::m_hashAndFlags` refs resolve as before).
    - `setHash()` no longer asserts `!hasHash()`; it now asserts
      stored == computed when a hash was already present (racing lazy hashers
      store the identical value; §4.5).
    - New public `bool StringImpl::tryRefAtom()` (§4.4.2): static bit ⇒ plain
      `ref()`, return true; else relaxed CAS loop that FAILS at masked
      refcount 0 (0 is final; never revives a dying string).
    - `deref()` branches on the latched `g_sharedAtomStringTableEnabled`
      (relaxed, F4): shared mode = `fetch_sub(release)` + acquire fence on the
      zero transition, then out-of-line `derefSharedZero()`; legacy mode =
      today's relaxed path verbatim (R3(b)).
  - `Source/WTF/wtf/text/StringImpl.cpp`: `derefSharedZero()` —
    `!isAtom() || !length()` ⇒ `destroy()` as today, else
    `AtomStringImpl::removeDeadAtom(this)` (§4.4.3 step 3).
  - `Source/WTF/wtf/text/AtomStringImpl.h`: declares
    `WTF_EXPORT_PRIVATE static void removeDeadAtom(AtomStringImpl*)` (§4.4.5).
    DEFINITION lands with task 4 (W1 lifecycle) — until then WTF does not link
    in shared-mode-reachable configurations (intra-WS dependency, by task
    order; no integrator action needed).
  - `Source/WTF/wtf/text/ExternalStringImpl.cpp`: the four in-constructor
    `BufferExternal` flag writes became explicit relaxed load/store (object
    provably unpublished; §4.5).
  - Composed flag-off bar (§3 R3): this task introduces exactly the R3(a)
    delta (atomic type + RMW flag writes) and the R3(b) delta (one
    latched-flag branch in `deref`); bench-gated, not instruction-identical.

- Task 3 (W1 routing, §4.3 rule A1 / §4.4.4) — LANDED in-tree (vmstate-owned
  files only; NO new shared-file hunks):
  - `Source/WTF/wtf/text/AtomStringImpl.cpp`: every atomization, lookup, and
    removal entry point is dual-pathed on `sharedAtomStringTableEnabled()`
    with the frozen §4.3 shape (`[[unlikely]]` shared arm → shard via
    `shardForHash(HashTranslator::hash(value))` → `Locker { shard.lock }` →
    shared add/find; then the legacy `AtomStringTableLocker`+`stringTable()`
    body verbatim, with the ex-M10 drift guard
    `ASSERT(!sharedAtomStringTableEnabled())` atop each legacy arm). Routed
    entries covering all 17 legacy locker sites:
    - generic `addToStringTable<T, Translator>(value)` (covers all
      span/buffer/substring/literal adds incl. the CF path, which delegates);
    - `addSymbol(StringImpl&)` / `addStatic(const StringImpl&)` via new
      `addSymbolShared`/`addStaticShared` (all four static buffer arms;
      static-pointer atoms keep one process-wide `StringImpl*` — I19);
    - `addSlowCase(StringImpl&)` and `addSlowCase(Ref<StringImpl>&&)` via new
      `addOwnedStringToSharedStringTable` (default `StringHash` ⇒ shard from
      `string.hash()`, I5; `setIsAtom(true)` published under the shard lock,
      F1);
    - `addSlowCase(AtomStringTable&, StringImpl&)`: shared mode IGNORES the
      passed table (A1) and delegates to the table-less overload — this is
      the choke point for the `ASI.h` explicit-table overloads
      (`add(AtomStringTable&,...)`, `addWithStringTableProvider`); legacy mode
      honors its argument (`USE(WEB_THREAD)` untouched, R3);
    - `remove(AtomStringImpl*)`: shared arm finds by hash + POINTER equality,
      removal CONDITIONAL (racing add may have replaced the entry; I6 — no
      RELEASE_ASSERT in shared mode; legacy RELEASE_ASSERT kept verbatim);
    - `lookUpSlowCase` / `lookUp(span8)` / `lookUp(span16)`: shared hits MUST
      `tryRefAtom()` under the shard lock (§4.4.4) — success adopts the
      in-lock ref, failure (dead entry) is a miss;
    - `isInAtomStringTable` (ASSERT_ENABLED): consults the shard.
    Table hits in BOTH shared add helpers use the §4.4.4 protocol: live ⇒
    adopt `tryRefAtom()`'s ref; dead ⇒ `ASSERT(!isStatic())`, locked
    remove-and-reinsert-fresh (racing `removeDeadAtom` skips the replacement
    via pointer-match, I6/I19).
  - `Source/WTF/wtf/text/AtomStringImpl.h`: documentation only — A1 notes on
    `addWithStringTableProvider`, `addSlowCase(AtomStringTable&,...)`, and the
    inline `add(AtomStringTable&, StringImpl&)`. No signature changes.
  - Flag-off delta introduced: R3(c) only (one latched-flag branch per routed
    entry point). `<wtf/Lock.h>` is now included unconditionally (was
    `USE(WEB_THREAD)`-only) — header-only, no behavior change.
  - Intra-WS dependency unchanged: `removeDeadAtom` is declared (task 2) and
    now also the §4.4.5 partner of the conditional-removal arms here; its
    DEFINITION still lands with task 4. No integrator action.

- Task 4 (W1 lifecycle, §4.4.5) — LANDED in-tree (vmstate-owned files only;
  NO new shared-file hunks):
  - `Source/WTF/wtf/text/AtomStringImpl.cpp`: `removeDeadAtom(AtomStringImpl*)`
    DEFINED (closes the task-2/3 intra-WS dependency; WTF now links in
    shared-mode-reachable configurations). Protocol per §4.4.5: preconditions
    debug-asserted (shared mode, atom, non-static, non-symbol, nonzero length,
    refcount 0 — caller uniquely owns); locks
    `shardForHash(string->existingHash())` (same shard every insert path used,
    I5); finds via the existing pointer-equality removal translator; removal
    CONDITIONAL on pointer match (a racing §4.4.4 add may have replaced the
    dead entry with a fresh atom for the same characters — no unconditional
    `RELEASE_ASSERT(wasRemoved)`; identity debug-asserted; I6); then the
    destructor bypass: `setIsAtom(false)` (idempotent `fetch_and`, §4.5) UNDER
    the shard lock, pre-destroy, so `~StringImpl` skips its legacy removal arm
    and never touches the table in shared mode; `StringImpl::destroy` runs
    OUTSIDE the shard lock (leaf rank, I7 — destruction can run arbitrary
    deallocation, including substring-base deref chains that re-enter
    `removeDeadAtom` for the base; in-lock destroy would nest shard locks).
  - `Source/WTF/wtf/text/AtomStringImpl.h`: comment refresh only ("definition
    lands with task 4" note dropped; bypass mechanism documented).
  - `Source/WTF/wtf/text/StringImpl.cpp`: comments only —
    `derefSharedZero()`'s removeDeadAtom note finalized; `~StringImpl`'s
    isAtom arm documented as unreachable for dying table atoms in shared mode
    (reachable there only via direct `destroy()` callers, which
    `AtomStringImpl::remove`'s conditional shared arm covers).
  - `Source/WTF/wtf/text/AtomStringTable.cpp`: NO change — the I17 destructor
    enforcement this task's file list covers landed with task 1 and is
    exercised by the new `PerThreadTablesStayEmpty` test below.
  - M14 test content (carried in this manifest, below) extended with the
    task-4 suite: `DeadAtomRemovedOnFinalDeref` (deterministic §4.4.3/§4.4.5
    round trip), `LiveUniqueness` (I1, barrier-synchronized), `DeadEntryChurn`
    (I1/I2/I3/I6 stress + racing lookups), `SingleKeyDeathRace` (focused I6
    replace-vs-removeDeadAtom amplifier), `StaticAtomSurvives` (I19, static
    pointer identity under churn), `PerThreadTablesStayEmpty` (I17 positive,
    raw `WTF::Thread` death). All shared-mode tests latch inside `EXPECT_EXIT`
    children so the parent TestWTF process is never latched; they double as
    the W1 TSAN gate (§10) and the ASAN UAF probe for the §4.4 protocol.
  - Flag-off delta: none beyond tasks 2/3 (this task adds no new branches on
    the latch; R3 budget unchanged).

- Task 5 (W2, §5/§6.5.1) — LANDED in-tree (vmstate-owned files only):
  - `Source/JavaScriptCore/runtime/VMLiteShared.h` (new): frozen §5.2
    `SharedVMState` — NeverDestroyed singleton, `structureAllocationLock()`
    (SPEC-heap §6 rank 7a; recursive acquisition forbidden), and the RAII
    `StructureAllocationLocker` with the FROZEN member order
    (`std::optional<GCDeferralContext> m_deferralContext` FIRST, lock state
    after, so `~GCDeferralContext` — and any deferred collection — runs
    strictly AFTER unlock, S1). `deferralContext()` returns null when
    inactive; M7 sites pass it into the cell allocation (N4 = SPEC-heap
    L5/I14). Also the frozen §6.5.1 `VMLiteRegistry` (leaf lock; same lock
    that M11/M12 wrap around `VM::m_microtaskQueues`), with the §6.5.1
    lifetime/N8 contract documented at the declaration.
  - `Source/JavaScriptCore/runtime/VMLiteShared.cpp` (new): locker ctor =
    lock → I8 counter (`fetch_add`, `RELEASE_ASSERT(!previous)`; depth
    exposed via `SharedVMState::structureAllocationRegionDepth()` for the
    §10 TSAN/stress hooks) → N7 `incrementSTWForbiddenScope()` under
    `#if defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)` → `emplace`
    `GCDeferralContext(vm)`. Dtor = F5 `WTF::storeStoreFence()` →
    `decrementSTWForbiddenScope()` → I8 decrement (`RELEASE_ASSERT(== 1)`) →
    unlock → implicit `~GCDeferralContext` last (member order). Flag off ⇒
    one predictable branch, everything else skipped (I10; flag-off delta is
    within the R3 budget — the branch is on a latched Option, untaken).
    `VMLiteRegistry::registerLite` is the SOLE writer of `VMLite::vm`
    (asserts absent + was-null); `unregisterLite` asserts present.
  - N7 STATUS (cross-WS item RESOLVED → verify-only): the heap WS has
    ALREADY landed `Heap::incrementSTWForbiddenScope()` /
    `decrementSTWForbiddenScope()` (`heap/Heap.cpp:3714`, `heap/Heap.h:445`)
    AND defines `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE 1` in `heap/Heap.h:75`.
    `VMLiteShared.cpp` includes `Heap.h`, so the shim is ACTIVE in this
    tree. Integrator action: none beyond confirming the macro is still
    defined at final merge.
  - Intra-WS dependencies (no integrator action; by §11 task order):
    (a) `VMLiteShared.cpp` includes `runtime/VMLite.h` — lands with task 6
    (W3 struct); until then the TU does not compile (mirrors the task-2/3
    `removeDeadAtom` precedent). (b) `Options::useStructureAllocationLock()`
    is read in the locker ctor — provided by M_opts (above), which is also
    not yet in tree; both gate compilation of this TU, which is itself only
    reachable once M2 (below) is applied.
  - §5.3/M7 reminder (unchanged): acquisition sites are
    SPEC-objectmodel-owned (`Structure::create`/`createStructure`/allocating
    transition-table inserts); the integrator AUDITS for coverage and fills
    gaps only — never nests lockers (self-deadlock by design).

- Task 6 (W3 struct, §6.3/§6.7/L4) — LANDED in-tree (vmstate-owned files
  only; no new shared-file hunks beyond completing M2 below):
  - `Source/JavaScriptCore/runtime/VMLite.h` (new): the frozen §6.3 layout —
    `FOR_EACH_VMLITE_PRIMITIVE_FIELD` X-macro (authoritative; M6's VM.h block
    expands the SAME macro), `VMLitePrimitives` (POD prefix; trivially-
    copyable + Group-1 pair-at-0x00/0x08 static_asserts; NO standard-layout
    assert per L3 — `targetInterpreterPCForThrow` is a Variant), per-field
    `offsetOf_<name>()` accessors (VMLitePrimitives-relative, Phase B
    consumes, L5), and class `VMLite` (`primitives` at offset 0, asserted;
    `tid`/`vm`/Groups 4-6 exactly per spec; frozen TLS accessor signatures
    `currentIfExists()/current()/setCurrent()`; explicit out-of-line
    ctor/dtor because `RefPtr<MicrotaskQueue>`/`unique_ptr` members need
    complete types at destruction). Declares `using ButterflyTID = uint16_t`
    (identical alias also in ConcurrentButterfly.h — legal redeclaration),
    `JS_EXPORT_PRIVATE ButterflyTID currentButterflyTID()`, and
    `JS_EXPORT_PRIVATE void setVMLiteTIDTagHook(void (*)(uint16_t))` (§6.7).
    Includes `Interpreter.h` for `JSOrWasmInstruction` (per §6.3); forward-
    declares `struct ScratchBuffer` (defined in VM.h — VMLite.h must never
    include VM.h, since M6 makes VM.h include VMLite.h).
  - `Source/JavaScriptCore/runtime/VMLite.cpp` (new): L4 TLS backing
    (`static thread_local VMLite* t_currentVMLite`, NOT pthread_getspecific);
    `setCurrent` returns the previous lite, debug-asserts I18
    (`tid != 0x7fff`) and I20 (lite registered in `VMLiteRegistry`, leaf
    lock), and invokes the TID-tag hook AFTER the TLS write with
    `lite ? lite->tid : 0` including uninstalls (jit CS3/I19); hook storage =
    null-default `std::atomic` function pointer, registered once by jit task
    1b. SOLE defining TU for `currentButterflyTID()` (returns installed
    lite's tid, else 0; never notTTLTID). `~VMLite` asserts not TLS-current
    on this thread + not registered, and poisons `vm`/`tid`/`executingRegExp`
    in debug (I20).
  - `Source/JavaScriptCore/runtime/VMLiteInlines.h` (new):
    `VMLite::isInstalledOnCurrentThread()` (the I11 owner-assert substrate)
    and the Group-4 lazy `ensureRegExpAllocator()` (owner-asserted). The §6.5
    microtask helpers (lazy `defaultMicrotaskQueue` + enqueue/drain with I11
    asserts) land here with task 7.
  - Cross-WS ODR note (§6.7, integrator verify-only): now that `VMLite.h`
    exists, the `#if !__has_include("VMLite.h")` interim shims compile away
    automatically — `runtime/ConcurrentButterfly.h:73-80` (objectmodel) and
    the static shim in `jit/ConcurrentButterflyOperations.cpp` — and
    `jit/ConcurrentButterflyOperations.cpp:199`'s
    `setVMLiteTIDTagHook(&updateButterflyTIDTag)` (signature
    `void(uint16_t)`, verified at `:175`) now compiles against the real
    export under its `JSC_JIT_HAS_VMLITE` guard. Verify at final merge:
    exactly ONE defining TU for `currentButterflyTID` (`VMLite.cpp`).
  - Notes for task 8's M6 hunk (verified against live VM.h at this tree):
    Group-1 pair = VM.h:405-406 (pair comment at 403-404); Group 2 =
    VM.h:395,397 (`m_exception`/`m_lastException`, under the `private:` at
    394) + 879-890; Group 3 = VM.h:1237,1239-1240. Relocation list (§6.3):
    `m_terminationException` (396), `maybeReturnPC` (407), `topJSPIContext`
    (408), `m_currentSoftReservedZoneSize` (1238), `m_executingRegExp` (891)
    — keep sites, move just outside the block. The whole X-macro block goes
    under ONE `public:` label replacing the 395-406 region (frozen
    placement, §6.3/history F3); I15 asserts go in the two exception
    setters; `m_mainVMLite` + ctor/`~VM` ordering per §6.4.4/I20.
  - Flag-off delta: none (nothing in existing code includes VMLite.h until
    M6; the ConcurrentButterfly shim swaps are declaration-for-declaration
    identical — same signature, same value 0 with no lite installed — and
    `Sources.txt` pickup of `VMLite.cpp` adds only code that is unreachable
    until M4/api §5.2 land).

- Task 7 (W3 plumbing, §6.5/§6.6 per-thread facilities; inert §6.1.4) —
  LANDED in-tree (vmstate-owned files) + M4/M11/M12/M13 hunks below:
  - `runtime/VMLite.h`: declarations for the per-thread facilities —
    `ensureDefaultMicrotaskQueue()` (Group 6 lazy; out-of-line),
    `enqueueMicrotaskToDefaultQueue(QueuedTask&&)` /
    `drainDefaultMicrotaskQueue()` (inline, I11), and the frozen §6.6
    Phase-B signature `scratchBufferForSize(size_t)` +
    `clearScratchBuffers()`. One L2-compliant data append AFTER Group 6:
    `size_t sizeOfLastScratchBuffer { 0 }` (guarded by `scratchBufferLock`;
    logically Group-5 state, but L1/L2 forbid inserting it next to
    `scratchBuffers`). Frozen layout untouched otherwise; all §6.3 asserts
    unchanged.
  - `runtime/VMLite.cpp`: `ensureDefaultMicrotaskQueue()` — asserts I11
    (installed owner) + I14 (`vm->currentThreadIsHoldingAPILock()`), then
    `MicrotaskQueue::create(*vm)`; GC visibility comes for free because the
    `MicrotaskQueue(VM&)` ctor appends to `VM::m_microtaskQueues`, the single
    registration list (§6.5) — M12 below puts the registry lock around that
    append. `scratchBufferForSize`/`clearScratchBuffers` mirror
    `VM::scratchBufferForSize`/`clearScratchBuffers` (VM.cpp:1595-1624)
    including the geometric-growth policy, under the leaf
    `scratchBufferLock`. `~VMLite` now `VMMalloc::free`s the scratch buffers
    (mirrors ~VM, VM.cpp:655-656). New includes: `VM.h`, `VMLiteInlines.h`.
  - `runtime/VMLiteInlines.h`: the I11 enqueue/drain helpers
    (`enqueueMicrotaskToDefaultQueue` → `MicrotaskQueue::enqueue`;
    `drainDefaultMicrotaskQueue` → `performMicrotaskCheckpoint<false>` with a
    no-op globalObject-switch callback; no-op when the queue was never
    created). Now includes `MicrotaskQueueInlines.h` + `VM.h` — this header
    is consumed only by `VMLite.cpp` and tests, never by `VM.h` (M6 includes
    `VMLite.h` only), so no cycle.
  - Phase-A inertness (I13): no interpreter/JIT/runtime path calls any of
    these; `VM::queueMicrotask`/`drainMicrotasks` are NOT rerouted; flag-off
    builds reach none of this code (`useVMLite` gates every install path).
  - GC caveat recorded in VMLite.h: lite scratch buffers are NOT visited by
    `VM::gatherScratchBufferRoots` in Phase A; Phase B must add
    registry-wide scratch-buffer root gathering before any baked DFG/FTL
    pointer is routed VMLite-relative (see cross-WS notes under PENDING).
  - Test obligations handed to task 9 (`JSTests/threads/vmstate/**` +
    harness): I11 (owner-only enqueue/drain — assert-death or
    owner-thread-only coverage), lazy-creation idempotence, drain executes
    enqueued tasks exactly once, `scratchBufferForSize(0) == nullptr`,
    geometric growth reuses the last buffer for smaller sizes, destructor
    frees without leaks (ASAN), and I13 single-threaded `useVMLite=1`
    behavior identity. C++-level pieces that need a thread without JS glue
    belong next to the M14 WTF tests if the runner cannot reach them.

- Task 8 (Manifest, §9/§11-8) — this file is now COMPLETE for the spec's
  hunk set: M_opts, M_opts2, M1, M2, M3, M4, M6, M9, M11, M12, M13, M14 are
  ready-to-apply hunks; M7 + M8 + cross-WS are checklists (M7 verification
  per §5.3/N5; M8 verification per §9; cross-WS section below). All M6
  content was derived from the LIVE VM.h/VM.cpp at this tree (line numbers
  re-verified: Group-1 pair 405-406, Group-2 395/397+879-890, Group-3
  1237/1239-1240, relocations 396/407/408/891/1238, exception setters
  VM.h:1205/VM.cpp:1094, pair assert 1366, ~VM 591-670, ctor tail 563-569)
  and expands the SAME `FOR_EACH_VMLITE_PRIMITIVE_FIELD` X-macro that
  `runtime/VMLite.h` (task 6) froze. M9 was verified against the live SAMA
  ctor (mimalloc arm 138-142; `m_useSystemHeap` overwrite at 126). No
  in-tree files were touched by task 8 — manifest only. Recommended apply
  order recorded at the end of M6: M_opts → M_opts2 → M1/M2/M3 → M6 → M4 →
  M11/M12/M13 → M9 (M6 needs M_opts; M4 needs M6's `mainVMLite()`; M9's
  `useSharedGCHeap` disjunct needs INTEGRATE-heap's M_opts entry).

- Task 9 (Tests, §11-9/§10) — LANDED:
  - `JSTests/threads/vmstate/**` (new, in-tree; SPEC-vmstate §9 owned glob —
    verbatim from SPEC-vmstate.md §9 "Owned paths and manifest": Writable:
    `Source/WTF/wtf/text/**`; `Source/JavaScriptCore/runtime/`{...} (all
    new); **`JSTests/threads/vmstate/**`**; `Tools/TestWebKitAPI/Tests/WTF/
    SharedAtomStringTable.cpp` (new); `docs/threads/INTEGRATE-vmstate.md` —
    so the in-tree landing is sanctioned by the frozen spec; any run-config
    summary that omits the glob is the abbreviation, not the grant.
    SPEC-api's `JSTests/threads/**` runner globs pick it up — N6 verify-only;
    `resources/` follows the existing non-test-dir precedent):
    - `resources/workload.js` — deterministic per-thread-execution-state
      workload (Groups 2/3/4/6 + W1/W2 light touch); single hard-coded digest
      `VMSTATE_WORKLOAD_EXPECTED_DIGEST` shared by all identity tests.
    - `flags-off-baseline.js` (I4/R3 baseline; NO flag directives — runs in
      today's tree), `vmlite-single-thread-identity.js` (I13 with
      `--useVMLite=1`; I14 via the M13 assert), `all-flags-identity.js`
      (I13/I14 with `--useJSThreads=1` ⇒ A=V=S=1 via M_opts2; main thread +
      3 spawned threads + post-teardown re-run). All three assert the SAME
      digest — cross-config divergence is an I13/I4 violation by construction.
    - `structure-churn-threads.js` + `structure-churn-dictionary.js` (I8/I9:
      fresh-shape churn from N threads; delete/dictionary, prototype change,
      preventExtensions/seal/freeze, array indexing-type transitions) and
      `structure-lock-single-thread.js` (I8 never-nest / I10 with ONLY
      `--useStructureAllocationLock=1`; nested-allocation shapes — literals
      in literals, super chains, allocating getters).
    - `exception-state-per-thread.js` (I15/Group 2: throw/catch identity +
      finally-rethrow unwind order per thread), `stack-limits-per-thread.js`
      (Group 3/§6.1.3: per-thread RangeError overflow, twice per thread,
      main-thread limits intact after hand-offs), `regexp-churn-threads.js`
      (Group 4: exec loops with backreferences, reentrant replace,
      shared-regexp stateless match), `microtask-ordering.js` (Group 6/§6.5
      Phase A non-reroute; pins the JS-observable contract behind I11).
    - `README.md` — file-to-invariant map + the FLAG MATRIX (§10): row 1
      flags-off baseline (R3/I4, golden-disasm bar), row 2 `useVMLite=1`
      single-threaded (I13), row 3 `useStructureAllocationLock=1` alone
      (I8/I10), row 4 `useSharedAtomStringTable=1` (primary gate = M14 WTF
      unit tests), row 5 `useJSThreads=1` GIL-on full suite, row 6 TSAN
      no-JIT all-flags-on, row 7 race amplifier (incl. one `USE(MIMALLOC)`
      config for M9/I9). Availability note: rows needing the three §3 flags
      start only once M_opts is present (R4 orchestrator pre-apply).
  - M14 file content (carried above) EXTENDED with two W1 tests:
    `ConcurrentLazyHashingAndAtomFlags` (§4.5 idempotent-RMW integrity under
    racing lazy hash vs. atomization of the same StringImpl, in a latched
    EXPECT_EXIT child) and `LegacyModeUnaffected` (parent-process, unlatched:
    legacy per-thread routing intact, dormant shared table empty — the I4
    face of R3). Registration hunk (M14 step 1) unchanged.
  - Integrator notes: none new. N6 stays verify-only; the W2/W3 files assume
    M_opts/M_opts2 (flag rows) and the M4/M6/M11-M13 hunks for the assert
    layer they exercise in debug builds.
  - ACKNOWLEDGED GAP (recorded at review round 2): the task-7 C++ test
    obligations for the VMLite per-thread facilities were NOT delivered by
    this task — the JS suite cannot reach them (Phase A inert) and the two
    M14 additions cover W1 only. Tracked as a BLOCKER-for-Phase-B PENDING
    entry below; do not read this task's "LANDED" as covering VMLite.cpp's
    facility code.

- Task 10 (Self-check, §11-10 / §10 compile gate) — STATIC AUDIT EXECUTED;
  build/run gates SPECIFIED for the §3 R4 overlay (this agent run forbids
  builds/tests/git, so the executable half is owed to the build phase). Full
  record, per-gate status, the overlay recipe, and one load-bearing finding
  (the bare tree does not LINK without M1/M2 — see "Task 10 — Self-check
  record" at the end of this file). No source file changed by this task; all
  audited code passed as landed.

- Review round 2 (adversarial findings; all six verified REAL and fixed):
  1. §4.8 carve-out hole closed: the ordering contract now requires a
     pre-latch thread's happens-before edge before ANY ref/deref of ANY
     WTF::String (pre-latch-owned exemption kept for atomize/lookUp only) —
     `addSlowCase` atomizes co-owned strings in place, so a pre-latch-owned
     string can become a shard atom post-latch. Amended:
     `SharedAtomStringTable.h` contract, F4 argument (now in the header),
     `StringImpl.h` deref comment, in-place-atomization note at
     `AtomStringImpl.cpp` `addSlowCase(StringImpl&)`; cross-WS item 15 (Bun
     embedder-thread verify).
  2. M4 GIL-off gate recorded: cross-WS item 13 (blocker-if-violated) +
     `RELEASE_ASSERT(!useJSThreads || useThreadGIL)` backstop added to the
     M4 install hunk; foreign-VM tid-0 aliasing and the `m_entryVMLite`
     nested-teardown edge documented under M4.
  3. WTF link breakage removed: `singleton()` is defined in always-compiled
     `AtomStringTable.cpp` and `sharedAtomStringTableEnabled()` is
     header-inline; bare-tree WTF links with only `AtomStringTable.cpp`
     compiled. Task-10 A2 + M1 notes updated (JSC half still needs M2;
     unavoidable under the shared-hot-file rule).
  4. Task-7 C++ test obligations acknowledged as UNMET: PENDING blocker
     entry added (must land before Phase-B routing); Task-9 log amended;
     coverage-status comment added in `VMLite.h`.
  5. `useThreads` alias hole closed: M_opts2 now normalizes the alias into
     `useJSThreads` before the R2 implication; cross-WS item 14 ties it to
     INTEGRATE-api 9.2-1's alias removal.
  6. Foreign-VM tid-0 install documented + fail-stopped (see 2; reviewer
     filed it separately — same M4 hunk, same backstop).

- Review round 3 (five findings: two REAL+fixed, one REAL conflict recorded,
  one disclosed-gap hardened, one REFUTED):
  1. REAL (filed twice, findings 1+3): `AtomStringImpl::lookUpSlowCase` kept
     the unconditional `!string.isAtom()` assert that round 2 relaxed in
     both `addSlowCase` overloads — the same legal shared-mode race (another
     thread atomizing the SAME co-owned StringImpl* in place under the shard
     lock, after the caller's unlocked `lookUp(StringImpl*)` check) would
     fail-stop ASSERT_ENABLED builds. FIXED: assert now reads
     `sharedAtomStringTableEnabled() || !string.isAtom()` with the §4.4.4
     comment; the function body needed no change (the shared arm's
     character-keyed find + tryRefAtom is race-correct, including a hit on
     `&string` itself).
  2. REFUTED (ownership blocker on `JSTests/threads/vmstate/**`): the
     reviewer could not find SPEC-vmstate.md, but it IS in tree —
     `docs/threads/SPEC-vmstate.md` §9 "Owned paths and manifest" lists
     `JSTests/threads/vmstate/**` verbatim in the Writable set. The suite's
     README now carries the exact citation plus a one-line grep for future
     reviewers. No relocation needed.
  3. REAL (disclosed gap, VMLite facility coverage): the PENDING blocker
     entry is now an explicit HARD GATE — any new caller of the six VMLite
     facilities in any workstream's diff is a blocker by construction until
     the TestWebKitAPI JavaScriptCore-suite test lands.
  4. REAL (manifest-vs-manifest conflict): vmstate M4's
     `RELEASE_ASSERT(... || Options::useThreadGIL())` backstop vs
     INTEGRATE-api 9.2-1's deletion of `useThreadGIL`. Recorded as cross-WS
     item 16 (resolution (a)/(b)); M_opts anchor note made 9.2-1-tolerant;
     vmstate README flag-matrix row annotated. Mirror into INTEGRATE-api
     9.2-1 owed by the api WS (file outside this write set).

- Review round 4 (three findings: two REAL+fixed in-tree, one RE-CONFIRMED
  conflict, already recorded as item 16, escalated to the banner atop this
  file):
  1. REAL (TOCTOU: unlocked `canBecomeAtom()` vs in-place shared-table
     atomization racing `setNeverAtomize()` could park a NeverAtomize —
     early-buffer-releasable ExternalStringImpl — string in a shard, and
     `setNeverAtomize`'s unconditional `ASSERT(!isAtom())` fail-stopped
     debug builds under the same legal shared-mode race the rounds-2/3
     fixes relaxed elsewhere). FIXED in vmstate-owned files, by making the
     race total-ordered instead of merely narrower (a shard-lock re-check
     alone would NOT close it — `setNeverAtomize` takes no lock):
     - `StringImpl.h`: `setNeverAtomize()` is now a CAS loop on
       `m_hashAndFlags` that refuses (returns false, flag NOT set) once the
       isAtom bit is visible in the same word; legacy mode keeps the
       historical debug fail-stop for the already-atom caller bug. New
       `StringImpl::trySetIsAtomIfAtomizable()`: sets isAtom unless
       NeverAtomize is set, same atomic word, so exactly one side of the
       race wins. `canBecomeAtom()` documented as advisory.
     - `AtomStringImpl.cpp`: `addOwnedStringToSharedStringTable` claims
       atom-hood via `trySetIsAtomIfAtomizable()`; on loss it backs the
       table insert out under the still-held shard lock (entry never
       observable: isAtom never set, probes need the lock) and returns
       null; both `addSlowCase` shared arms then fall back to the copying
       `add(span)` path AFTER releasing the shard lock (same hash ⇒ same
       shard ⇒ relock; mirrors the BUN_JSC_ADDITIONS bail).
     - Static sibling hardened (found while fixing the above):
       `addStaticShared` re-checks `canBecomeAtom()` with a copying
       fallback before parking the buffer-ALIASING static copy for
       refcount-static ExternalStringImpls; full closure there needs the
       embedder set-before-sharing rule (item 17), since the parked alias
       is a different StringImpl and word-level adjudication can't apply.
     - EMBEDDER-VISIBLE: `setNeverAtomize()` now returns bool — see new
       cross-WS item 17 (Bun must check it before `releaseBufferEarly()`,
       and must set the flag before sharing for external static strings).
     - M14 test content extended with
       `NeverAtomizeVsInPlaceAtomizationRace` (BUN-gated, child-process
       latched): races `add(impl)` against `setNeverAtomize()` per
       iteration and asserts the winner-exclusivity invariants.
  2. REAL (the `enableSharedAtomStringTable` comment overclaimed I17 as
     "the backstop" for §4.8 breaches; I17 only catches the ATOMIZE half;
     the ref/deref half was undetectable and silently UAFs in release).
     FIXED: comment rewritten to state the asymmetry explicitly
     (`SharedAtomStringTable.cpp`), and a debug-only heuristic backstop
     added in `StringImpl::deref`'s LEGACY zero-transition arm —
     `ASSERT(!g_sharedAtomStringTableEnabled)` re-read: the legacy arm was
     chosen because the first latch load read false, so a second load
     reading true is a stale-latch §4.8 breach in flight. Zero release
     cost; cross-WS item 15 remains the release-mode mitigation.
  3. RE-CONFIRMED (no new in-tree fix possible in this write set): the live
     two-sided conflict between INTEGRATE-api 9.2-1's `useThreadGIL` /
     `useThreads` deletion and this manifest's M4 backstop + M_opts2
     normalization. Already fully recorded (cross-WS items 14 + 16, with
     resolutions (a)/(b)); round 4 adds the ADJUDICATE-FIRST banner at the
     top of this file so neither manifest is applied blind. The mirror note
     inside INTEGRATE-api.md 9.2-1 remains owed by the api WS/integrator —
     that file is outside this workstream's write set.

## M_opts — `Source/JavaScriptCore/runtime/OptionsList.h` (SPEC-vmstate §3)

Insert these three lines into `FOR_EACH_JSC_OPTION`, adjacent to the existing
`useJSThreads` block (currently `OptionsList.h:681`). Anchor: immediately
after the LAST surviving line of that block — today that is the
`v(Bool, useThreadGIL, ...)` line, but INTEGRATE-api 9.2-1 carries a diff
deleting both `useThreads` and `useThreadGIL` (see cross-WS item 16); if
9.2-1 has been applied first, insert after the
`v(Unsigned, jsThreadGILTimeSliceMs, ...)` line instead. The anchor is
positional only; the three lines do not reference either option:

```cpp
    v(Bool, useSharedAtomStringTable, false, Normal, "process-global shared atom string table"_s) \
    v(Bool, useVMLite, false, Normal, "per-thread VMLite carriers (Phase A: inert)"_s) \
    v(Bool, useStructureAllocationLock, false, Normal, "serialize Structure cell allocation + ID-creating transitions"_s) \
```

NOTE (§3 R4): the spec expects M_opts to be orchestrator-pre-applied before
fan-out; as of task 1 the tree does NOT yet contain these options (verified:
no `useSharedAtomStringTable` in OptionsList.h). Until they land, nothing
reads them — `enableSharedAtomStringTable()` is only reachable via M3.

## M_opts2 — `Source/JavaScriptCore/runtime/Options.cpp` (§3 R2; sole provider)

In `Options::notifyOptionsChanged()` (currently `Options.cpp:762`), immediately
after the `AllowUnfinalizedAccessScope scope;` line, insert:

```cpp
    // The live tree still carries the prep-stub alias `useThreads`
    // (OptionsList.h:685), honored by ThreadManager.h's useJSThreadsEnabled()
    // with NO normalization anywhere in Options.cpp. Fold it into the
    // canonical flag FIRST so the R2 implication below cannot be bypassed by
    // the alias spelling (--useThreads=1 must not enable the Thread API with
    // all three vmstate flags off). Drop this line when INTEGRATE-api 9.2-1
    // (alias removal) lands — see cross-WS item 14.
    if (Options::useThreads())
        Options::useJSThreads() = true;
    if (Options::useJSThreads()) {
        Options::useSharedAtomStringTable() = true;
        Options::useVMLite() = true;
        Options::useStructureAllocationLock() = true;
    }
```

Rationale: `useJSThreads=1` MUST imply all three (R2) — and so must every
spelling that enables the Thread API, which today includes the `useThreads`
alias (verified: OptionsList.h:685 defines it; `grep useThreads Options.cpp`
is empty, so nothing else normalizes it; ThreadManager.h:54-56
`useJSThreadsEnabled()` returns `useJSThreads() || useThreads()`).
`notifyOptionsChanged` runs at the tail of `Options::initialize`
(Options.cpp:1111), hence before any flag consumer including M3 and the SAMA
ctor (M9). Whichever lands first — this hunk or INTEGRATE-api 9.2-1's alias
removal — R2 stays true; once 9.2-1 lands the normalization line is dead code
and should be dropped with it (cross-WS item 14).

## M1 — `Source/WTF/wtf/CMakeLists.txt`

Two insertions, both in the existing alphabetized `text/` runs:

1. Header list (public WTF headers; `text/AtomStringTable.h` is at line 476):
   insert in sorted order between `text/RapidHash.h` and `text/StringBuffer.h`:

```
    text/SharedAtomStringTable.h
```

2. Source list (`text/AtomStringTable.cpp` is at line 681): insert in sorted
   order between `text/LineEnding.cpp` and `text/StringBuffer.cpp`:

```
    text/SharedAtomStringTable.cpp
```

If other WTF build files enumerate sources for this platform set
(Xcode project, PlatformBun cmake fragments), add the same pair there.

Link note (review round 2): `SharedAtomStringTable.cpp` now contains ONLY
`enableSharedAtomStringTable()` — `singleton()` is defined in the
always-compiled `AtomStringTable.cpp` and `sharedAtomStringTableEnabled()` is
header-inline — so the bare tree's WTF links without M1. M1 remains REQUIRED
before applying M3 (JSC::initialize calls `enableSharedAtomStringTable()`) or
M14 (tests call it too).

## M3 — `Source/JavaScriptCore/runtime/InitializeThreading.cpp` (§3 R1)

1. Add the include (alphabetical among the `<wtf/...>` includes, currently
   after `#include <wtf/Threading.h>` at line 51):

```cpp
#include <wtf/text/SharedAtomStringTable.h>
```

2. In `initializeWithOptionsCustomization`, immediately AFTER
   `Options::finalize();` (currently line 113), insert:

```cpp
        // SPEC-vmstate §3 R1: read once, latched, immutable after. §4.8/§8
        // ordering contract: no other thread may atomize before
        // JSC::initialize returns (binds embedder and internal service
        // threads; breaches fail-stop at thread death via I17).
        if (Options::useSharedAtomStringTable())
            WTF::enableSharedAtomStringTable();
```

Placement constraints: must be after `Options::finalize()` (option value
final, M_opts2 already applied during `Options::initialize`) and before any
code that can atomize on another thread; line 114 is the earliest such point.

## M14 — `Tools/TestWebKitAPI/CMakeLists.txt` + test file

1. Registration: in `set(TestWTF_SOURCES ...)` (starts at line 28), insert in
   sorted order between `Tests/WTF/SetForScope.cpp` (line 122) and the
   following entry:

```
    Tests/WTF/SharedAtomStringTable.cpp
```

2. Create `Tools/TestWebKitAPI/Tests/WTF/SharedAtomStringTable.cpp` with
   exactly this content (task-1 coverage: I5 shard selection, §4.8 migration +
   idempotence; task-4 coverage: §4.4 lifecycle — dead-entry stress I1-I3/I6,
   static atoms I19, I17 thread-death emptiness; task-9 coverage: §4.5
   concurrent lazy hashing vs. atomization — idempotent-RMW flag integrity —
   and a parent-process legacy-mode/I4 sanity test, appended at the end of
   the namespace). Every shared-mode test
   latches inside an `EXPECT_EXIT` child process: the latch is one-way and
   process-global, so the parent TestWTF process must NEVER latch (the
   migration test's pre-latch assertion depends on it). The multithreaded
   children double as the W1 TSAN gate (SPEC-vmstate §10) when TestWTF is
   built with `-fsanitize=thread`; run with
   `--gtest_death_test_style=threadsafe` under sanitizers to avoid
   fork-from-instrumented-parent noise:

```cpp
/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <wtf/Locker.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/SharedAtomStringTable.h>
#include <wtf/text/WTFString.h>

namespace TestWebKitAPI {

// ---- I5: shard selection is a pure function of the hash (SPEC-vmstate §4.7).

TEST(WTF_SharedAtomStringTable, ShardSelectionPureFunction)
{
    auto& table = SharedAtomStringTable::singleton();
    static_assert(SharedAtomStringTable::shardCount == 128);
    static_assert(SharedAtomStringTable::shardCountLog2 == 7);

    // Same hash => same shard, and the shard is exactly
    // m_shards[(hash >> 17) & 127] (HIGH 7 of the 24-bit hash; §4.2).
    for (unsigned hash : { 0u, 1u, 0x1FFFFu, 0x20000u, 0xFFFFFFu, 0xABCDEFu, 0x800000u }) {
        auto& shard = table.shardForHash(hash);
        ASSERT_EQ(&shard, &table.shardForHash(hash));
        unsigned expectedIndex = (hash >> (24 - SharedAtomStringTable::shardCountLog2))
            & (SharedAtomStringTable::shardCount - 1);
        ASSERT_EQ(&shard, &table.m_shards[expectedIndex]);
    }
}

TEST(WTF_SharedAtomStringTable, ShardSelectionUsesHighBits)
{
    auto& table = SharedAtomStringTable::singleton();

    // Hashes differing only in the LOW 17 bits land on the SAME shard
    // (the per-shard HashTable consumes the low bits for buckets)...
    ASSERT_EQ(&table.shardForHash(0x000000u), &table.shardForHash(0x01FFFFu));
    ASSERT_EQ(&table.shardForHash(0x540000u), &table.shardForHash(0x55ABCDu));
    // ...while flipping bit 17 (lowest shard-selecting bit) changes the shard.
    ASSERT_NE(&table.shardForHash(0x000000u), &table.shardForHash(0x020000u));
    // Bits above the 24-bit hash domain are ignored by the mask.
    ASSERT_EQ(&table.shardForHash(0x00ABCDEFu), &table.shardForHash(0xFFABCDEFu));
}

TEST(WTF_SharedAtomStringTable, EqualStringsSameShard)
{
    auto& table = SharedAtomStringTable::singleton();

    // Two independently-built equal strings hash equally, hence always
    // contend on one lock (I5).
    String a = makeString("shard"_s, "-selection-probe"_s);
    String b = makeString("shard-selection"_s, "-probe"_s);
    ASSERT_NE(a.impl(), b.impl());
    ASSERT_EQ(a.impl()->hash(), b.impl()->hash());
    ASSERT_EQ(&table.shardForHash(a.impl()->hash()), &table.shardForHash(b.impl()->hash()));
}

TEST(WTF_SharedAtomStringTable, ShardLayoutNoFalseSharing)
{
    static_assert(sizeof(SharedAtomStringTable::Shard) >= 128);
    static_assert(alignof(SharedAtomStringTable::Shard) >= 64);
    auto& table = SharedAtomStringTable::singleton();
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&table.m_shards[0]) % 64, static_cast<uintptr_t>(0));
}

// ---- §4.8 migration + I17. Latching is process-global and irreversible, so
// this runs in a death-test child process (fork on POSIX; spawned re-exec on
// Windows). Everything inside the lambda executes in the child only.

TEST(WTF_SharedAtomStringTable, MigrationOnLatch)
{
    EXPECT_EXIT({
        // Pre-latch: atomize on the initializing thread.
        AtomString probe("WTFSharedAtomStringTableMigrationProbe"_s);
        StringImpl* probeImpl = probe.impl();
        RELEASE_ASSERT(probeImpl);
        RELEASE_ASSERT(probeImpl->isAtom());
        unsigned probeHash = probeImpl->existingHash();

        auto* threadTable = Thread::currentSingleton().atomStringTable();
        RELEASE_ASSERT(!threadTable->table().isEmpty());
        size_t preLatchCount = threadTable->table().size();

        RELEASE_ASSERT(!WTF::sharedAtomStringTableEnabled());
        WTF::enableSharedAtomStringTable();
        RELEASE_ASSERT(WTF::sharedAtomStringTableEnabled());

        // (2) Source set cleared, isAtom NOT cleared.
        RELEASE_ASSERT(threadTable->table().isEmpty());
        RELEASE_ASSERT(probeImpl->isAtom());

        // (1) Every pre-latch atom is now in shardForHash(existingHash()),
        // found by pointer identity; total migrated count matches.
        auto& shared = SharedAtomStringTable::singleton();
        {
            auto& shard = shared.shardForHash(probeHash);
            Locker locker { shard.lock };
            bool found = false;
            for (const auto& entry : shard.table) {
                if (entry.get() == probeImpl)
                    found = true;
            }
            RELEASE_ASSERT(found);
        }
        size_t migratedCount = 0;
        for (unsigned i = 0; i < SharedAtomStringTable::shardCount; ++i) {
            auto& shard = shared.m_shards[i];
            Locker locker { shard.lock };
            migratedCount += shard.table.size();
            // I5/§4.8: every migrated entry sits in its own hash's shard.
            for (const auto& entry : shard.table)
                RELEASE_ASSERT(&shared.shardForHash(entry.get()->existingHash()) == &shard);
        }
        RELEASE_ASSERT(migratedCount == preLatchCount);

        // Idempotent: second call is a no-op (would otherwise re-migrate an
        // empty table; must not assert or duplicate).
        WTF::enableSharedAtomStringTable();
        RELEASE_ASSERT(threadTable->table().isEmpty());

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// ---- Task 4 (W1 lifecycle, §4.4): no-resurrection destroy protocol,
// removeDeadAtom (§4.4.5), dead-entry stress (I1-I3, I6), static atoms (I19),
// I17 thread-death emptiness. All shared-mode: each test latches inside its
// own EXPECT_EXIT child (the parent process is never latched). Failures are
// RELEASE_ASSERTs in the child — a tripped assert crashes the child and fails
// ExitedWithCode(0). Under TSAN these are the W1 data-race gate; under ASAN
// the churn is a UAF probe for revival-at-0 / double-destroy / dangling shard
// entries.

// Simple reusable spin barrier; no WTF lock is taken so the synchronization
// under test is not masked.
class SpinBarrier {
    WTF_MAKE_NONCOPYABLE(SpinBarrier);
public:
    explicit SpinBarrier(unsigned total)
        : m_total(total)
    {
    }

    void arriveAndWait()
    {
        unsigned generation = m_generation.load(std::memory_order_acquire);
        if (m_count.fetch_add(1, std::memory_order_acq_rel) + 1 == m_total) {
            m_count.store(0, std::memory_order_relaxed);
            m_generation.fetch_add(1, std::memory_order_release);
        } else {
            while (m_generation.load(std::memory_order_acquire) == generation)
                Thread::yield();
        }
    }

private:
    const unsigned m_total;
    std::atomic<unsigned> m_count { 0 };
    std::atomic<unsigned> m_generation { 0 };
};

static std::span<const char> formatKey(char* buffer, size_t capacity, const char* prefix, unsigned index)
{
    int length = snprintf(buffer, capacity, "%s-%u", prefix, index);
    return std::span<const char> { buffer, static_cast<size_t>(length) };
}

// Deterministic single-threaded lifecycle (§4.4.3/§4.4.5): the final deref of
// an atom must remove its shard entry (via removeDeadAtom) and destroy it
// without the destructor touching the table; afterwards lookUp misses and a
// fresh add yields a live atom again.
TEST(WTF_SharedAtomStringTable, DeadAtomRemovedOnFinalDeref)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();
        static constexpr auto key = "sharedAtomLifecycleSingleThread"_span;

        {
            RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
            RELEASE_ASSERT(atom);
            RELEASE_ASSERT(atom->isAtom());
            RELEASE_ASSERT(!atom->isStatic());
            RELEASE_ASSERT(atom->refCount() >= 1);

            // While live, lookUp returns the same pointer (I1/I2).
            RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>(key));
            RELEASE_ASSERT(found.get() == atom.get());
            // The RefPtrs going out of scope run the §4.4.3 zero transition;
            // removeDeadAtom unhooks the shard entry and destroys the string.
        }

        // Entry removed: a lookup is a clean miss, never a dead hit (I3).
        RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));

        // Re-atomization works and produces a live atom.
        {
            RefPtr<AtomStringImpl> again = AtomStringImpl::add(key);
            RELEASE_ASSERT(again);
            RELEASE_ASSERT(again->isAtom());
            RELEASE_ASSERT(again->refCount() >= 1);
        }
        RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// I1: while multiple threads simultaneously hold references obtained by
// atomizing the same character sequence, they all hold the SAME pointer, and
// the refcount reflects every holder.
TEST(WTF_SharedAtomStringTable, LiveUniqueness)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 8;
        constexpr unsigned rounds = 256;

        SpinBarrier barrier(threadCount);
        Vector<RefPtr<AtomStringImpl>> results(threadCount);

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: live uniqueness"_s, [&, t] {
                char buffer[64];
                for (unsigned round = 0; round < rounds; ++round) {
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomLiveUnique", round);
                    results[t] = AtomStringImpl::add(key);
                    RELEASE_ASSERT(results[t]);
                    RELEASE_ASSERT(results[t]->isAtom());

                    barrier.arriveAndWait();

                    if (!t) {
                        // All threads hold a reference right now: exactly one
                        // live AtomStringImpl per character sequence (I1).
                        for (unsigned other = 1; other < threadCount; ++other)
                            RELEASE_ASSERT(results[other].get() == results[0].get());
                        RELEASE_ASSERT(results[0]->refCount() >= threadCount);
                    }

                    barrier.arriveAndWait();

                    // Everyone drops; the last deref of the round routes
                    // through removeDeadAtom (§4.4.5).
                    results[t] = nullptr;

                    barrier.arriveAndWait();
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// Dead-entry stress (I1/I3/I6): tight add/drop churn over a small key set
// from many threads, racing concurrent lookUps. Drives every §4.4 arm:
// tryRefAtom failure on a dying entry, dead-entry remove+reinsert in add,
// lookUp treating a dead hit as a miss, and removeDeadAtom skipping an entry
// a racing add already replaced (pointer mismatch).
TEST(WTF_SharedAtomStringTable, DeadEntryChurn)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned adderCount = 6;
        constexpr unsigned lookupCount = 2;
        constexpr unsigned keyCount = 4; // Few keys => maximal add/deref collisions.
        constexpr unsigned iterations = 20000;

        std::atomic<bool> done { false };

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < adderCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: churn adder"_s, [&, t] {
                char buffer[64];
                for (unsigned i = 0; i < iterations; ++i) {
                    unsigned keyIndex = (i + t) % keyCount;
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomChurn", keyIndex);
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
                    RELEASE_ASSERT(atom);
                    RELEASE_ASSERT(atom->isAtom());
                    RELEASE_ASSERT(atom->refCount());
                    RELEASE_ASSERT(WTF::equal(atom.get(), byteCast<Latin1Character>(key)));
                    // Drop immediately: with no other holder this is the zero
                    // transition -> removeDeadAtom (§4.4.5).
                    atom = nullptr;
                }
                done.store(true, std::memory_order_release);
            }));
        }
        for (unsigned t = 0; t < lookupCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: churn lookup"_s, [&, t] {
                char buffer[64];
                unsigned i = 0;
                while (!done.load(std::memory_order_acquire)) {
                    unsigned keyIndex = (i + t) % keyCount;
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomChurn", keyIndex);
                    RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>(key));
                    // A miss is fine (the atom may be dead or absent); a hit
                    // MUST be a live, correct atom — never a refcount-0
                    // corpse (I3).
                    if (found) {
                        RELEASE_ASSERT(found->refCount());
                        RELEASE_ASSERT(found->isAtom());
                        RELEASE_ASSERT(WTF::equal(found.get(), byteCast<Latin1Character>(key)));
                    }
                    found = nullptr;
                    ++i;
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        // Drain: all references dropped => every churn atom is dead and
        // unhooked; lookups miss cleanly (I2/I6 — a leaked shard entry or a
        // double removal would have crashed above or shows up as a dead hit).
        char buffer[64];
        for (unsigned keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
            auto key = formatKey(buffer, sizeof(buffer), "sharedAtomChurn", keyIndex);
            RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));
        }

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// Focused I6 amplifier: all threads churn ONE key with zero holders between
// iterations, maximizing the window where a refcount-0 entry is still table
// resident. Exercises tryRefAtom failure -> locked remove + fresh insert
// (§4.4.4) racing removeDeadAtom's pointer-identity removal (§4.4.5).
TEST(WTF_SharedAtomStringTable, SingleKeyDeathRace)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 8;
        constexpr unsigned iterations = 30000;
        constexpr auto key = "sharedAtomSingleKeyDeathRace"_span;

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: death race"_s, [&] {
                for (unsigned i = 0; i < iterations; ++i) {
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
                    RELEASE_ASSERT(atom);
                    RELEASE_ASSERT(atom->refCount());
                    RELEASE_ASSERT(WTF::equal(atom.get(), byteCast<Latin1Character>(key)));
                    atom = nullptr;
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        RELEASE_ASSERT(!AtomStringImpl::lookUp(byteCast<Latin1Character>(key)));

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// I19: a table-resident StaticStringImpl is returned BY POINTER to every
// thread, always survives tryRefAtom (statics rest at masked refcount 0 and
// never die), and is never evicted by the dead-entry replace arm or by
// removeDeadAtom — even under heavy same-table churn.
static StringImpl::StaticStringImpl s_sharedAtomStaticSurvivor { "sharedAtomStaticSurvivor", StringImpl::StringAtom };

TEST(WTF_SharedAtomStringTable, StaticAtomSurvives)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        StringImpl& staticImpl = s_sharedAtomStaticSurvivor;
        RELEASE_ASSERT(staticImpl.isStatic());
        RELEASE_ASSERT(staticImpl.isAtom());

        // Register the static in its shard; the returned atom IS the static.
        RefPtr<AtomStringImpl> registered = AtomStringImpl::add(s_sharedAtomStaticSurvivor);
        RELEASE_ASSERT(registered);
        RELEASE_ASSERT(static_cast<StringImpl*>(registered.get()) == &staticImpl);

        constexpr unsigned threadCount = 8;
        constexpr unsigned iterations = 10000;
        constexpr auto staticKey = "sharedAtomStaticSurvivor"_span;

        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: static atom"_s, [&, t] {
                char buffer[64];
                for (unsigned i = 0; i < iterations; ++i) {
                    // Atomizing the static's characters from any thread
                    // returns the SAME pointer (I19) — via tryRefAtom's
                    // static fast path.
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(staticKey);
                    RELEASE_ASSERT(static_cast<StringImpl*>(atom.get()) == &staticImpl);

                    RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>(staticKey));
                    RELEASE_ASSERT(static_cast<StringImpl*>(found.get()) == &staticImpl);

                    // Churn dynamic atoms alongside so dead-entry removal and
                    // replacement runs hot while the static stays resident
                    // (the replace arm asserts !isStatic; removeDeadAtom
                    // never sees a static — statics never hit refcount 0).
                    unsigned keyIndex = (i + t) % 3;
                    auto churnKey = formatKey(buffer, sizeof(buffer), "sharedAtomStaticChurn", keyIndex);
                    RefPtr<AtomStringImpl> churn = AtomStringImpl::add(churnKey);
                    RELEASE_ASSERT(churn);
                    RELEASE_ASSERT(churn->refCount());
                    churn = nullptr;
                }
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        // Still resident, still the same pointer.
        RefPtr<AtomStringImpl> after = AtomStringImpl::lookUp(byteCast<Latin1Character>(staticKey));
        RELEASE_ASSERT(static_cast<StringImpl*>(after.get()) == &staticImpl);

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// I17 positive coverage: post-latch, rule A1 keeps every per-thread
// AtomStringTable empty, so a raw WTF::Thread that atomizes heavily dies
// cleanly — its ~AtomStringTable RELEASE_ASSERT(m_table.isEmpty()) must NOT
// trip (it would crash this child and fail ExitedWithCode(0)).
TEST(WTF_SharedAtomStringTable, PerThreadTablesStayEmpty)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 4;
        Vector<Ref<Thread>> threads;
        for (unsigned t = 0; t < threadCount; ++t) {
            threads.append(Thread::create("SharedAtomStringTable: I17"_s, [t] {
                char buffer[64];
                for (unsigned i = 0; i < 512; ++i) {
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomI17Probe", (i + t * 512));
                    RefPtr<AtomStringImpl> atom = AtomStringImpl::add(key);
                    RELEASE_ASSERT(atom);
                    RELEASE_ASSERT(atom->isAtom());
                }
                // A1: nothing above touched this thread's table.
                RELEASE_ASSERT(Thread::currentSingleton().atomStringTable()->table().isEmpty());
                // ~AtomStringTable runs at thread death and re-checks (I17).
            }));
        }
        for (auto& thread : threads)
            thread->waitForCompletion();

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// ---- Task 9 additions (W1 coverage closure) ----

// §4.5: m_hashAndFlags is atomic with idempotent RMW flag writes. Racing
// lazy hashers against concurrent atomization of the SAME StringImpl must
// never drop a flag bit (a plain RMW could erase isAtom or the lazily
// stored hash) and the stored hash must equal the computed hash.
TEST(WTF_SharedAtomStringTable, ConcurrentLazyHashingAndAtomFlags)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();

        constexpr unsigned threadCount = 8;
        constexpr unsigned stringCount = 1024;
        constexpr unsigned rounds = 4;

        for (unsigned round = 0; round < rounds; ++round) {
            // Fresh, unhashed, not-yet-atomized strings.
            Vector<RefPtr<StringImpl>> strings;
            {
                char buffer[64];
                for (unsigned i = 0; i < stringCount; ++i) {
                    auto key = formatKey(buffer, sizeof(buffer), "sharedAtomLazyHash", round * stringCount + i);
                    strings.append(StringImpl::create(byteCast<Latin1Character>(key)));
                    RELEASE_ASSERT(!strings.last()->isAtom());
                }
            }

            SpinBarrier barrier(threadCount);
            Vector<Ref<Thread>> threads;
            for (unsigned t = 0; t < threadCount; ++t) {
                threads.append(Thread::create("SharedAtomStringTable: lazy hash"_s, [&, t] {
                    barrier.arriveAndWait();
                    for (unsigned i = 0; i < stringCount; ++i) {
                        StringImpl* impl = strings[i].get();
                        if (t & 1) {
                            // Lazy hasher: fetch_or of the hash bits (§4.5).
                            unsigned hash = impl->hash();
                            RELEASE_ASSERT(hash == impl->existingHash());
                        } else {
                            // Atomizer: setIsAtom(true) is published under
                            // the shard lock (F1) and races the hashers'
                            // fetch_or on the same word.
                            RefPtr<AtomStringImpl> atom = AtomStringImpl::add(impl);
                            RELEASE_ASSERT(atom);
                            RELEASE_ASSERT(atom->isAtom());
                        }
                    }
                }));
            }
            for (auto& thread : threads)
                thread->waitForCompletion();

            // No dropped bits: every string is an atom AND carries exactly
            // the hash an identical fresh string computes.
            char buffer[64];
            for (unsigned i = 0; i < stringCount; ++i) {
                StringImpl* impl = strings[i].get();
                RELEASE_ASSERT(impl->isAtom());
                auto key = formatKey(buffer, sizeof(buffer), "sharedAtomLazyHash", round * stringCount + i);
                Ref<StringImpl> fresh = StringImpl::create(byteCast<Latin1Character>(key));
                RELEASE_ASSERT(impl->existingHash() == fresh->hash());
            }
            // Dropping the vector derefs each atom to zero ->
            // removeDeadAtom unhooks every shard entry (§4.4.5).
        }

        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// ---- I4 face of R3 (legacy-mode sanity). Runs in the PARENT process, which
// is NEVER latched: with the latch off, atomization routes to the per-thread
// table exactly as today and the dormant shared table stays empty of it.
TEST(WTF_SharedAtomStringTable, LegacyModeUnaffected)
{
    RELEASE_ASSERT(!WTF::sharedAtomStringTableEnabled());

    AtomString atom("sharedAtomLegacyModeProbe"_s);
    StringImpl* impl = atom.impl();
    RELEASE_ASSERT(impl);
    RELEASE_ASSERT(impl->isAtom());

    // Legacy routing: the atom lives in THIS thread's per-thread table...
    bool inThreadTable = false;
    for (const auto& entry : Thread::currentSingleton().atomStringTable()->table()) {
        if (entry.get() == impl)
            inThreadTable = true;
    }
    ASSERT_TRUE(inThreadTable);

    // ...and in NO shard of the (dormant) shared table.
    auto& shared = SharedAtomStringTable::singleton();
    for (unsigned i = 0; i < SharedAtomStringTable::shardCount; ++i) {
        auto& shard = shared.m_shards[i];
        Locker locker { shard.lock };
        for (const auto& entry : shard.table)
            ASSERT_NE(static_cast<StringImpl*>(entry.get()), impl);
    }

    // Legacy lookUp still hits by pointer.
    RefPtr<AtomStringImpl> found = AtomStringImpl::lookUp(byteCast<Latin1Character>("sharedAtomLegacyModeProbe"_span));
    ASSERT_EQ(static_cast<StringImpl*>(found.get()), impl);
}

#if USE(BUN_JSC_ADDITIONS)
// ---- Round 4: setNeverAtomize() vs in-place atomization is total-ordered
// (StringImpl::trySetIsAtomIfAtomizable on the same atomic word). Invariant
// under the race: a string whose setNeverAtomize() returned true is NEVER
// shard-resident and never isAtom; if it returned false the string IS an
// atom (the atomization won). Either way every add() call still yields a
// usable atom with equal characters (copying fallback).
TEST(WTF_SharedAtomStringTable, NeverAtomizeVsInPlaceAtomizationRace)
{
    EXPECT_EXIT({
        WTF::enableSharedAtomStringTable();
        constexpr unsigned stringCount = 512;

        for (unsigned i = 0; i < stringCount; ++i) {
            char buffer[64];
            auto key = formatKey(buffer, sizeof(buffer), "sharedAtomNeverAtomize", i);
            Ref<StringImpl> victim = StringImpl::create(byteCast<Latin1Character>(key));
            StringImpl* impl = victim.ptr();

            std::atomic<int> flagWon { -1 };
            SpinBarrier barrier(2);
            RefPtr<AtomStringImpl> atom;
            auto atomizer = Thread::create("SharedAtomStringTable: atomizer"_s, [&] {
                barrier.arriveAndWait();
                atom = AtomStringImpl::add(impl);
            });
            auto marker = Thread::create("SharedAtomStringTable: marker"_s, [&] {
                barrier.arriveAndWait();
                flagWon.store(impl->setNeverAtomize() ? 1 : 0, std::memory_order_seq_cst);
            });
            atomizer->waitForCompletion();
            marker->waitForCompletion();

            RELEASE_ASSERT(atom);
            RELEASE_ASSERT(atom->isAtom());
            RELEASE_ASSERT(equal(*atom, *impl));
            if (flagWon.load() == 1) {
                // setNeverAtomize won: the victim must NOT be parked — the
                // atomizer fell back to a copying atom.
                RELEASE_ASSERT(!impl->isAtom());
                RELEASE_ASSERT(static_cast<StringImpl*>(atom.get()) != impl);
                RELEASE_ASSERT(!impl->canBecomeAtom());
                auto& shared = SharedAtomStringTable::singleton();
                auto& shard = shared.shardForHash(impl->hash());
                {
                    Locker locker { shard.lock };
                    for (const auto& entry : shard.table)
                        RELEASE_ASSERT(static_cast<StringImpl*>(entry.get()) != impl);
                }
                // Refuses forever after, even once an equal copy-atom exists.
                RELEASE_ASSERT(!impl->trySetIsAtomIfAtomizable());
            } else {
                // Atomization won: in-place atom; flag refused (returned 0).
                RELEASE_ASSERT(flagWon.load() == 0);
                RELEASE_ASSERT(impl->isAtom());
                RELEASE_ASSERT(impl->canBecomeAtom());
                RELEASE_ASSERT(static_cast<StringImpl*>(atom.get()) == impl);
            }
        }
        exit(0);
    }, ::testing::ExitedWithCode(0), "");
}
#endif // USE(BUN_JSC_ADDITIONS)

} // namespace TestWebKitAPI
```

## M2 — JSC `Sources.txt` (complete: both `runtime/VMLite.cpp` and `runtime/VMLiteShared.cpp` exist as of task 6)

In `Source/JavaScriptCore/Sources.txt`, the alphabetized `runtime/` run
currently reads (lines 1147-1150):

```
runtime/VM.cpp
runtime/VMEntryScope.cpp
runtime/VMManager.cpp
runtime/VMTraps.cpp
```

Insert in sorted order so the run becomes:

```
runtime/VM.cpp
runtime/VMEntryScope.cpp
runtime/VMLite.cpp
runtime/VMLiteShared.cpp
runtime/VMManager.cpp
runtime/VMTraps.cpp
```

(`runtime/VMLite.cpp` is now in-tree — task 6 landed it — so the hunk above is
applicable as-is.)

### M2b — JSC `CMakeLists.txt` (`JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`)

`Source/JavaScriptCore/CMakeLists.txt` DOES enumerate runtime headers in
`JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`; the relevant sorted run currently
reads (lines 1804-1810):

```
    runtime/VM.h
    ...
    runtime/VMInlines.h
    runtime/VMManager.h
    runtime/VMThreadContext.h
    runtime/VMTraps.h
```

Insert, in sorted order, between `runtime/VMInlines.h` (line 1807) and
`runtime/VMManager.h` (line 1808):

```
    runtime/VMLite.h
    runtime/VMLiteInlines.h
    runtime/VMLiteShared.h
```

This hunk is REQUIRED, not optional:

- objectmodel's `runtime/StructureCreateInlines.h:37` includes
  `<JavaScriptCore/VMLiteShared.h>` framework-style, and
  INTEGRATE-objectmodel.md (forwarding-header note, ~line 340) explicitly
  defers the forwarding header to this manifest. Without this entry no
  forwarding header is generated and every TU reaching
  `StructureCreateInlines.h` fails to compile.
- `runtime/ConcurrentButterfly.h` keys its shim on
  `__has_include("VMLite.h")`. If `VMLite.h` were missing from the copied
  forwarding-header set while in-tree TUs see the real header, embedder-side
  TUs would silently bind the inline `return 0` shim while JSC binds the
  real `currentButterflyTID()` — divergent definitions (IFNDR) and TID = 0
  on spawned threads in embedder TUs, i.e. flat-butterfly ownership
  corruption once the GIL lifts. Listing `runtime/VMLite.h` here keeps the
  two include contexts in agreement.
- `VMLite.h`'s transitive framework deps (`Interpreter.h`, `JSCJSValue.h`,
  etc.) are already in the list; no further additions needed.

If any OTHER platform build file also enumerates runtime headers, mirror the
same three entries there.

## M4 — `runtime/JSLock.h` / `runtime/JSLock.cpp` (§6.4.4 install/restore)

The atom-table swap (`JSLock.cpp:123-125` install, `326-329` restore) and the
stack-field updates (`m_vm->setLastStackTop(thread)` at 127,
`setStackPointerAtVMEntry` at 137-138, cleared at 320) are KEPT VERBATIM
(§4.3 rev 7 / §6.1.3 — the hand-off is load-bearing). M4 adds ONLY the VMLite
install/conditional-restore, two members, and
`uninstallVMLiteForVMDestruction()`.

Precondition: M6's `VM` accessor `VMLite* mainVMLite() { return
m_mainVMLite.get(); }` (or `friend class JSLock;`) — recorded as an M6
requirement under PENDING. `Options::useVMLite()` requires M_opts.

1. `JSLock.h` — forward declaration: after `class VM;` (line 54) add:

```cpp
class VMLite;
```

2. `JSLock.h` — public method: after `void NODELETE willDestroyVM(VM*);`
   (line 108) add:

```cpp
    // SPEC-vmstate §6.4.4: called at the TOP of ~VM (M6), while this thread
    // still holds the API lock and before lastChanceToFinalize, so no
    // thread's TLS dangles across teardown (I20). If this hold installed the
    // main carrier, restore the entry value and clear the bookkeeping.
    void uninstallVMLiteForVMDestruction();
```

3. `JSLock.h` — members: after `AtomStringTable* m_entryAtomStringTable;`
   (line 162) add:

```cpp
    VMLite* m_entryVMLite { nullptr };
    bool m_didInstallVMLite { false };
```

4. `JSLock.cpp` — includes: add `#include "VMLite.h"` and
   `#include "Options.h"` to the include block (lines 24-31, alphabetical;
   `Options.h` may already arrive transitively — keep the explicit include).

5. `JSLock.cpp` — `didAcquireLock()` (runs at OUTERMOST acquisition only,
   mirroring the atom-table swap): immediately after the swap block
   (`ASSERT(m_entryAtomStringTable);`, line 125), insert:

```cpp
    if (Options::useVMLite()) [[unlikely]] {
        // §6.4.4: install the main carrier iff this thread has no lite or a
        // foreign one (covers main thread, embedder threads, multi-VM per
        // thread). A spawned thread (api §5.2) registered + setCurrent its
        // own lite BEFORE its first JSLockHolder, so cur->vm == m_vm there
        // and nothing is installed (m_didInstallVMLite stays false).
        //
        // SOUNDNESS INVARIANT (Phase A, normative): sharing the main carrier
        // (tid 0) across whichever thread holds the API lock — including
        // embedder threads and threads entering a foreign VM — is sound ONLY
        // because the API lock is mutually exclusive among this VM's mutators
        // (phase-1 GIL). Butterfly TID tags written under tid 0 persist in
        // object headers after the lock is released; if two threads could
        // mutate concurrently while both believing they are TID-0 owners,
        // they would race unlocked flat-butterfly transitions
        // (SPEC-objectmodel §2/§3 assumes TIDs identify a unique live
        // thread). The RELEASE_ASSERT fail-stops any GIL-off configuration
        // that still reaches this install path; before GIL-off, M4 must be
        // replaced per cross-WS item 13 (per-thread carriers, unique TIDs
        // from ThreadManager, never two threads installed with the same tid).
        VMLite* cur = VMLite::currentIfExists();
        if ((!cur || cur->vm != m_vm) && m_vm->mainVMLite()) {
            RELEASE_ASSERT(!Options::useJSThreads() || Options::useThreadGIL());
            m_entryVMLite = VMLite::setCurrent(m_vm->mainVMLite());
            m_didInstallVMLite = true;
        }
    }
```

   (`m_vm->mainVMLite()` is non-null exactly when the VM was constructed
   with `useVMLite` on — the flag is latched, but the null check keeps the
   hunk safe against VMs constructed before options finalize in exotic
   embedders.)

   Two documented edges of this install path:

   - **Foreign-VM tid-0 aliasing (Phase A: GIL-masked; Phase B: forbidden).**
     When `cur && cur->vm != m_vm` (a thread carrying VM A's lite enters
     VM B), the hunk installs B's main carrier and the thread runs tagged
     tid 0 — the same tag as B's real main thread. Under the phase-1 GIL B's
     API lock serializes the two, so no concurrent transitions can race; the
     RELEASE_ASSERT above turns any GIL-off run that reaches this state into
     a fail-stop instead of flat-butterfly corruption. Cross-WS item 13 owns
     the real fix (per-thread carriers + unique TIDs) before GIL-off.
   - **`m_entryVMLite` dangling across nested foreign-VM holds.** The saved
     previous lite is restored after the inner hold ends; if that lite's
     VM/lite were torn down in between (embedder destroys VM A while this
     thread is parked inside VM B), the restore would install a destroyed
     carrier. The §6.5.1 lifetime contract forbids this ordering (a VM must
     not die while its lites can still be installed); the I20
     registered-lite assert in `VMLite::setCurrent` plus `~VMLite`'s debug
     poison catch a violation in debug builds. Release builds rely on the
     lifetime contract — record any embedder that tears down VMs on foreign
     threads as a blocker at integration.

6. `JSLock.cpp` — `willReleaseLock()` (runs at FULL release only): insert
   just BEFORE the atom-table restore block (`if (m_entryAtomStringTable)`,
   line 326) — symmetric LIFO with the install (installed after swap ⇒
   restored before unswap):

```cpp
    if (m_didInstallVMLite) {
        // §6.4.4: restore ONLY IF the installed main carrier is still
        // current — a lite swapped in after our install (e.g. DropAllLocks
        // hand-off to a spawned thread that reacquired with its own lite) is
        // NEVER clobbered; always clear both members. m_vm can only be null
        // here if willDestroyVM already ran, and ~VM calls
        // uninstallVMLiteForVMDestruction() first, which clears the flag —
        // the m_vm guard is belt-and-suspenders.
        if (m_vm && VMLite::currentIfExists() == m_vm->mainVMLite())
            VMLite::setCurrent(m_entryVMLite);
        m_entryVMLite = nullptr;
        m_didInstallVMLite = false;
    }
```

7. `JSLock.cpp` — new method (place after `willReleaseLock()`):

```cpp
void JSLock::uninstallVMLiteForVMDestruction()
{
    // SPEC-vmstate §6.4.4/I20. Caller: TOP of ~VM (M6), API lock held
    // (VM.cpp:636 asserts), m_vm still valid (runs BEFORE
    // m_apiLock->willDestroyVM(this) nulls it).
    if (!m_didInstallVMLite)
        return;
    if (VMLite::currentIfExists() == m_vm->mainVMLite())
        VMLite::setCurrent(m_entryVMLite);
    m_entryVMLite = nullptr;
    m_didInstallVMLite = false;
}
```

Flag-off delta: one latched-option branch per outermost lock/unlock
(R3-style; bench-gated). DropAllLocks/grabAllLocks need no changes: they
funnel through `unlock(lockCount)`/`lock(lockCount)` which call
`willReleaseLock`/`didAcquireLock` at the boundary, and the conditional
restore + `cur->vm != m_vm` install test give exactly the §6.4.4 semantics
across drop/regrab (a spawned thread keeps its lite; reacquire installs
nothing).

## M6 — `runtime/VM.h` / `runtime/VM.cpp` (§6.4 block, asserts, accessor, main carrier, I15)

The ONLY changes to VM.h/.cpp beyond M11 (which is a separate hunk in this
manifest). Offset functions (VM.h:772-796 — `exceptionOffset`,
`offsetOfTopCallFrame`, `callFrameForCatchOffset`, `topEntryFrameOffset`,
`offsetOfEncodedHostCallReturnValue`) and the pair static_assert (VM.h:1366)
stay VERBATIM — names are unchanged, so they compile unchanged. No member is
deleted from `VM`; Group 1-3 members are RELOCATED into one contiguous
X-macro block (compile-time, not flag-gated — R3(d) covers the offset-
immediate deltas and the `{ }` init of the previously uninitialized members
`newCallFrameReturnValue`, `targetMachinePCForThrow`,
`targetMachinePCAfterCatch`, `targetInterpreterMetadataPCForThrow`,
`targetTryDepthForThrow`, `varargsLength`, `osrExitIndex`,
`osrExitJumpDestination`).

All line numbers verified against the live tree at task 8.

### M6.1 — VM.h include

Insert into the alphabetized include block (between `#include
"StrongForward.h"` at line 50 and `#include "VMThreadContext.h"` at line 51):

```cpp
#include "VMLite.h"
```

(`VMLite.h` never includes `VM.h` — it forward-declares `ScratchBuffer` and
`VM` — so no cycle. It supplies `FOR_EACH_VMLITE_PRIMITIVE_FIELD`,
`VMLitePrimitives`, and class `VMLite`.)

### M6.2 — VM.h: the §6.4(1) X-macro block (replaces lines 394-408)

Replace this exact region (current lines 394-408, immediately after
`unsigned disallowVMEntryCount { 0 };`):

```cpp
private:
    Exception* m_exception { nullptr };
    Exception* m_terminationException { nullptr };
    Exception* m_lastException { nullptr };
public:
    // NOTE: When throwing an exception while rolling back the call frame, this may be equal to
    // topEntryFrame.
    // FIXME: This should be a void*, because it might not point to a CallFrame.
    // https://bugs.webkit.org/show_bug.cgi?id=160441
    // The following two fields are sometimes treated as a pair in assembly code, making usages of the second one implicit.
    // To find them, look for loadpairq/storepairq of "VM::topCallFrame" in *.asm files.
    CallFrame* topCallFrame { nullptr };
    EntryFrame* topEntryFrame { nullptr };
    void* maybeReturnPC { nullptr };
    JSPIContext* topJSPIContext { nullptr };
```

with:

```cpp
private:
    // SPEC-vmstate §6.3 relocated member: cross-thread by design, deliberately
    // NOT in VMLitePrimitives. Kept just outside the block; name/type/sites
    // unchanged.
    Exception* m_terminationException { nullptr };
public:
    // NOTE: When throwing an exception while rolling back the call frame,
    // callFrameForCatch may be equal to topEntryFrame.
    // FIXME: callFrameForCatch should be a void*, because it might not point
    // to a CallFrame. https://bugs.webkit.org/show_bug.cgi?id=160441
    // topCallFrame/topEntryFrame are sometimes treated as a pair in assembly
    // code, making usages of the second one implicit. To find them, look for
    // loadpairq/storepairq of "VM::topCallFrame" in *.asm files.
    //
    // SPEC-vmstate §6.4(1) (M6): VM's Group 1-3 members are declared by
    // expanding the SAME X-macro as VMLitePrimitives (VMLite.h), under this
    // ONE public: label (frozen). Names are unchanged, so every existing
    // spelling (C++, offset fns, asserts, .asm) compiles unchanged; the
    // per-field equivalence asserts below the class pin the two layouts
    // together. Freeze rules L1-L5 (§6.3) apply: do NOT add, remove, or
    // reorder fields here — change FOR_EACH_VMLITE_PRIMITIVE_FIELD (spec
    // revision) or declare new members outside this block.
#define VM_DECLARE_VMLITE_PRIMITIVE_FIELD(type, name) type name { };
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VM_DECLARE_VMLITE_PRIMITIVE_FIELD)
#undef VM_DECLARE_VMLITE_PRIMITIVE_FIELD

    // SPEC-vmstate §6.4(3): VM doubles as the main thread's physical
    // VMLitePrimitives. Guarded by the equivalence asserts below the class.
    // Phase A: consumed by tests only.
    ALWAYS_INLINE VMLitePrimitives& mainVMLitePrimitives()
    {
        return *std::bit_cast<VMLitePrimitives*>(std::bit_cast<uint8_t*>(this) + OBJECT_OFFSETOF(VM, topCallFrame));
    }

    // SPEC-vmstate §6.4.4: the main thread's carrier (tid 0); non-null exactly
    // when the VM was constructed with useVMLite on. Consumed by JSLock (M4)
    // and ~VM.
    ALWAYS_INLINE VMLite* mainVMLite() { return m_mainVMLite.get(); }

    // SPEC-vmstate §6.3 relocated members (names/types/sites unchanged):
    void* maybeReturnPC { nullptr };
    JSPIContext* topJSPIContext { nullptr };
```

### M6.3 — VM.h: remove the old Group-2 declarations (lines 879-890)

The block now declares these; delete the originals. Replace (current lines
879-891):

```cpp
    EncodedJSValue encodedHostCallReturnValue { };
    CallFrame* newCallFrameReturnValue;
    CallFrame* callFrameForCatch { nullptr };
    void* targetMachinePCForThrow;
    void* targetMachinePCAfterCatch;
    JSOrWasmInstruction targetInterpreterPCForThrow;
    uintptr_t targetInterpreterMetadataPCForThrow;
    uint32_t targetTryDepthForThrow;

    unsigned varargsLength;
    uint32_t osrExitIndex;
    void* osrExitJumpDestination;
    RegExp* m_executingRegExp { nullptr };
```

with:

```cpp
    // SPEC-vmstate §6.4(1)/M6: the Group-2 exception/unwind members formerly
    // declared here (encodedHostCallReturnValue ... osrExitJumpDestination)
    // moved up into the VMLitePrimitives X-macro block near the top of VM.
    // §6.3 relocated member (kept here; deliberately NOT in VMLitePrimitives):
    RegExp* m_executingRegExp { nullptr };
```

(This region is `public:` today and stays `public:`; `m_executingRegExp`'s
access does not change.)

### M6.4 — VM.h: remove the old Group-3 declarations (lines 1237-1240)

Replace (current lines 1237-1240, in the `private:` region):

```cpp
    void* m_stackPointerAtVMEntry { nullptr };
    size_t m_currentSoftReservedZoneSize;
    void* m_stackLimit { nullptr };
    void* m_lastStackTop { nullptr };
```

with:

```cpp
    // SPEC-vmstate §6.4(1)/M6: m_stackPointerAtVMEntry / m_stackLimit /
    // m_lastStackTop moved up into the VMLitePrimitives X-macro block. §6.3
    // relocated member (interleaved in the old Group-3 range; the §6.4(2)
    // span assert forces it out of the block; name/type/sites unchanged):
    size_t m_currentSoftReservedZoneSize;
```

NOTE on access: the Group 1-3 members were a mix of `public:` (topCallFrame
pair, the 879-890 run) and `private:` (m_exception, m_lastException, the
Group-3 run). The frozen one-`public:`-label placement makes them all public.
This widens access only; nothing outside `VM` is forced to change, the friend
list stays as-is, and the existing accessor/friend discipline remains the API
of record.

### M6.5 — VM.h: `m_mainVMLite` member (§6.4.4)

Immediately after the `private:` at current line 1254 (the one following
`public: SentinelLinkedList<MicrotaskQueue, ...> m_microtaskQueues;`), add:

```cpp
    // SPEC-vmstate §6.4.4: main thread's VMLite carrier (tid 0). Created at
    // the END of the VM ctor when useVMLite; registered there via
    // VMLiteRegistry::registerLite (sole writer of VMLite::vm); the ctor
    // NEVER calls VMLite::setCurrent — JSLock::didAcquireLock installs it
    // (M4). Uninstalled+unregistered+destroyed at the TOP of ~VM (I20).
    std::unique_ptr<VMLite> m_mainVMLite;
```

### M6.6 — VM.h: equivalence asserts (§6.4(2))

Immediately after the existing pair assert at line 1366
(`static_assert(OBJECT_OFFSETOF(VM, topEntryFrame) == ...)`), add:

```cpp
// SPEC-vmstate §6.4(2) (M6): per-field layout equivalence — VM's X-macro block
// is layout-identical to VMLitePrimitives, so VM can serve as the main
// thread's physical VMLitePrimitives (mainVMLitePrimitives()) and Phase B can
// retarget VM::field accesses VMLitePrimitives-relative without ABI drift.
#define VM_ASSERT_VMLITE_PRIMITIVE_FIELD_OFFSET(type, name) \
    static_assert(OBJECT_OFFSETOF(VM, name) - OBJECT_OFFSETOF(VM, topCallFrame) \
        == OBJECT_OFFSETOF(VMLitePrimitives, name), \
        "VM Group 1-3 member " #name " must not drift from VMLitePrimitives");
FOR_EACH_VMLITE_PRIMITIVE_FIELD(VM_ASSERT_VMLITE_PRIMITIVE_FIELD_OFFSET)
#undef VM_ASSERT_VMLITE_PRIMITIVE_FIELD_OFFSET
// Span assert: the block is exactly one VMLitePrimitives — nothing interleaves
// (this is what forces m_currentSoftReservedZoneSize out, §6.3).
static_assert(OBJECT_OFFSETOF(VM, m_lastStackTop) - OBJECT_OFFSETOF(VM, topCallFrame) + sizeof(void*)
    == sizeof(VMLitePrimitives), "VM Group 1-3 block must span exactly sizeof(VMLitePrimitives)");
```

### M6.7 — VM.h: I15 assert in `clearException()` (first of the two setters)

In `clearException()` (current line 1205), insert at the TOP of the body:

```cpp
#if ASSERT_ENABLED
        // SPEC-vmstate I15: m_exception/m_lastException are written only by
        // the JSLock holder.
        if (Options::useVMLite())
            ASSERT(currentThreadIsHoldingAPILock());
#endif
```

(`currentThreadIsHoldingAPILock()` is declared at VM.h:971, above this point;
`Options` is already used in VM.h. I15 names exactly two setters —
`clearLastException()` at VM.h:829 is deliberately NOT asserted.)

### M6.8 — VM.cpp: include

Add to VM.cpp's alphabetized include block (idempotent with M11's
instruction — one include serves both hunks):

```cpp
#include "VMLiteShared.h"
```

(`VMLite.h` itself arrives transitively via VM.h after M6.1.)

### M6.9 — VM.cpp: ctor creates+registers the main carrier (§6.4.4)

At the END of the `VM::VM(...)` constructor body, immediately BEFORE the
final block (currently lines 563-569):

```cpp
    // We must set this at the end only after the VM is fully initialized.
    WTF::storeStoreFence();
    m_isInService = true;
```

insert:

```cpp
    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.4.4: main carrier (tid 0), created at the END of
        // the ctor. registerLite is the sole writer of VMLite::vm. The ctor
        // NEVER calls setCurrent — JSLock::didAcquireLock installs the
        // carrier at the outermost acquisition (M4).
        m_mainVMLite = makeUnique<VMLite>();
        VMLiteRegistry::singleton().registerLite(*m_mainVMLite, *this);
    }
```

### M6.10 — VM.cpp: ~VM teardown order (§6.4.4 / I20)

At the very TOP of `VM::~VM()` (current line 592, immediately after the
opening brace, BEFORE `VMManager::singleton().notifyVMDestruction(*this);`),
insert:

```cpp
    // SPEC-vmstate §6.4.4/I20, at the TOP of ~VM: (1) uninstall the main
    // carrier from this thread's TLS via JSLock — must run while
    // JSLock::m_vm is still valid, i.e. BEFORE m_apiLock->willDestroyVM(this)
    // below — then (2) assert no OTHER registered lite still points at this
    // VM (§6.5.1 lifetime: spawned threads unregistered theirs under their
    // final JSLock hold, api r11 §4.6.1/5.2), (3) unregister, (4) destroy.
    // Result: no thread's TLS dangles across lastChanceToFinalize.
    if (m_mainVMLite) {
        ASSERT(currentThreadIsHoldingAPILock());
        m_apiLock->uninstallVMLiteForVMDestruction();
        ASSERT(VMLite::currentIfExists() != m_mainVMLite.get());
#if ASSERT_ENABLED
        {
            Locker locker { VMLiteRegistry::singleton().lock };
            for (VMLite* lite : VMLiteRegistry::singleton().lites)
                ASSERT(lite->vm != this || lite == m_mainVMLite.get());
        }
#endif
        VMLiteRegistry::singleton().unregisterLite(*m_mainVMLite);
        m_mainVMLite = nullptr;
    }
```

(`~VMLite` itself asserts not-TLS-current and not-registered, and poisons in
debug — the order above satisfies both. Destroying the carrier here also
destroys its lazy `defaultMicrotaskQueue`, whose dtor removes itself from
`m_microtaskQueues` under the registry lock once M12 is applied — safely
before the M11 force-removal loop later in ~VM.)

### M6.11 — VM.cpp: I15 assert in `setException()` (second of the two setters)

In `VM::setException(Exception* exception)` (current line 1094), insert after
the existing `ASSERT(!isTerminationException(...))`:

```cpp
#if ASSERT_ENABLED
    // SPEC-vmstate I15: m_exception/m_lastException are written only by the
    // JSLock holder.
    if (Options::useVMLite())
        ASSERT(currentThreadIsHoldingAPILock());
#endif
```

Apply-order note: M6 depends on M_opts (`Options::useVMLite()`), and M4
depends on M6 (`mainVMLite()` accessor, `m_mainVMLite`). Recommended order:
M_opts → M_opts2 → M1/M2/M3 → M6 → M4 → M11/M12/M13 → M9.

## M7 — VERIFICATION checklist (StructureAllocationLocker sites; §5.3/N5 — NOT hunks)

SPEC-objectmodel adopted (SPEC-objectmodel.md:213) and OWNS emitting
`SharedVMState::StructureAllocationLocker locker { vm };` at every Structure-
cell-allocating site. As of this tree, objectmodel HAS emitted lockers —
verified locations the integrator re-audits at final merge:

- `runtime/StructureCreateInlines.h:93,109` (Structure::create paths).
- `runtime/Structure.cpp:668,707,815,845,893,906,934,1022,1051,1085,1201,1258,2277,2297`
  (re-verified against the current tree; allocating transitions:
  addPropertyTransition, removeProperty, changePrototype, attributeChange,
  toDictionary, sealed/frozen/preventExtensions transitions,
  flattenDictionary, setBrand, etc.).
- `runtime/Structure.cpp:445` carries a DELIBERATE no-locker comment (the
  delegated callee takes it) — that is the pattern for avoiding nesting; keep.

Integrator audit steps:

1. Grep `Structure::create\|createStructure` callers and
   `StructureTransitionTable` allocating insertions
   (`runtime/StructureTransitionTable.h`, `Structure.cpp`); confirm each
   Structure CELL allocation + ID-creating transition executes under exactly
   ONE locker. Fill gaps ONLY where absent. NEVER nest — the §5.2 lock is
   non-recursive; nesting self-deadlocks by design. The I8 in-region counter
   (`SharedVMState::structureAllocationRegionDepth()`,
   `RELEASE_ASSERT(!previous)` in the locker ctor) turns any nesting into a
   fail-stop, so a TSAN/no-JIT run of `JSTests/threads/**` with
   `useStructureAllocationLock=1` IS the audit's executable form.
2. Where the allocation accepts a `GCDeferralContext*`, confirm the site
   passes `locker.deferralContext()` (N4 = SPEC-heap L5/I14).
3. S1/S2/S3 audit (with the heap WS): no safepoint poll, `collectSync`, or
   handshake inside a locker region; allocator internals (`LocalAllocator`,
   `BlockDirectory`, libpas) never acquire `structureAllocationLock`; no
   thread polls a safepoint or parks for STW while holding any rank 7-10
   allocation lock. GIL masks S3 in Phase A; re-verify when the GIL lifts.
4. I8 gates at integration: TSAN no-JIT + `JSTests/threads/vmstate/**`
   structure churn (incl. one `USE(MIMALLOC)` config — see M9).
5. Transition-table publication audit (F5 scope; see the
   `StructureAllocationLocker` class comment in `VMLiteShared.h`): the
   locker-dtor `storeStoreFence` covers only POST-region publications of the
   StructureID. The in-region publication — the transition-table insert — is
   synchronized by the source Structure's `m_lock` instead: every insert runs
   under `GCSafeConcurrentJSLocker` (`Structure.cpp:708/725, 907/919,
   1052/1062, 1258/1270, 2302/2307`), and under `useJSThreads` every mutator
   lookup routes through the `m_lock`-holding Concurrently variant
   (`StructureInlines.h:689-690`; compiler threads already lock —
   SPEC-objectmodel L6(i)/I37). AUDIT: grep for any
   `m_transitionTable.get/getMatching/trySingleTransition/forEachTransition`
   reader reachable flag-on WITHOUT holding the source's `m_lock` (or a
   stopped world, e.g. GC's `finalizeUnconditionally`). Any such reader is a
   BLOCKER: it would need a producer-side `storeStoreFence` immediately
   before the table insert (objectmodel-owned), not the locker-dtor fence.
   GIL masks this in Phase A — re-run the grep when the GIL lifts.

## M8 — VERIFICATION (post-M6 offlineasm/offset-extractor; no hunks)

After applying M6, verify (this is also task 10's compile gate):

1. `LLIntOffsetsExtractor` builds on every CI toolchain (GCC+Clang) — it is a
   `friend` of `VM` (VM.h:1360) and consumes `OBJECT_OFFSETOF(VM, field)`
   through `generate_offset_extractor.rb`; the X-macro block keeps every
   field a real non-static `VM` member (§0), so all references resolve.
2. Every `VM::` field referenced from `.asm` resolves unchanged. Verified
   live inventory (LowLevelInterpreter*.asm): `VM::topCallFrame` (26),
   `VM::topEntryFrame` (17), `VM::callFrameForCatch` (6), `VM::m_exception`
   (5), `VM::targetInterpreterPCForThrow` (4), `VM::m_threadContext` (4),
   `VM::targetMachinePCForThrow` (3), `VM::encodedHostCallReturnValue` (3),
   `VM::m_typeProfilerLog` (2), `VM::m_shadowChicken` (1),
   `VM::m_lastStackTop` (1). All but `m_threadContext`/`m_typeProfilerLog`/
   `m_shadowChicken` (which M6 does not move) are X-macro fields.
3. The pair contract holds: VM.h:1366 assert + the loadpairq/storepairq sites
   (`LowLevelInterpreter.asm` "VM::topCallFrame" pair loads) — Group 1 is the
   first two macro fields, asserted at VMLitePrimitives 0x00/0x08 (VMLite.h)
   and equivalence-pinned into VM (M6.6).
4. All §6.3/§6.4 static_asserts compile (I16): GCC and Clang, debug+release.
5. Flag-off composed bar (§3 R3): JSTests smoke diff-free; bench in-noise;
   golden disasm modulo R3(a)-(d) — M6 contributes ONLY R3(d) (offset
   immediates + `{ }` zero-init of the eight previously uninitialized
   members listed in the M6 preamble).

## M9 — `heap/StructureAlignedMemoryAllocator.cpp` (§5.1/N3 ctor hunk)

INTEGRATE rebases this onto the heap WS's final SAMA; verified against the
live tree at task 8 (heap WS has not modified the ctor). `Options.h` is
already included (line 31); the `StructureMemoryManager` ctor runs from
`initializeStructureAddressSpace()` (`InitializeThreading.cpp`) strictly
after `Options::initialize` (§0), so both flags are readable.
`Options::useSharedGCHeap()` ships via INTEGRATE-heap's M_opts entry; if
applying M9 before the heap manifest, drop that disjunct or apply heap's
OptionsList line first.

In the ctor's `#elif USE(MIMALLOC)` branch (currently lines 138-142), replace:

```cpp
#elif USE(MIMALLOC)
        void* memory = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_jscConfig.startOfStructureHeap) + MarkedBlock::blockSize);
        size_t size = g_jscConfig.sizeOfStructureHeap - MarkedBlock::blockSize;
        RELEASE_ASSERT(mi_manage_os_memory_ex(memory, size, false, false, false, -1, true, &structureArena));
        structureHeap = mi_heap_new_in_arena(structureArena);
#else
```

with:

```cpp
#elif USE(MIMALLOC)
        // THREADS-INTEGRATE(vmstate) SPEC-vmstate §5.1/N3 (M9): mi_heap_ts are
        // thread-affine — handing out Structure blocks through a mimalloc heap
        // is unsafe once multiple threads allocate Structures. With either
        // flag on, skip arena/heap creation and use the locked-bitvector
        // handout (tryMallocStructureBlock's m_useSystemHeap path) for the
        // process lifetime. Flags off => branch untaken, today's path
        // byte-identical (R3). Blocks are rare; I9 stress covers this config.
        if (Options::useStructureAllocationLock() || Options::useSharedGCHeap()) [[unlikely]] {
            m_useSystemHeap = true;
            m_usedBlocks.set(0);
            return;
        }
        void* memory = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_jscConfig.startOfStructureHeap) + MarkedBlock::blockSize);
        size_t size = g_jscConfig.sizeOfStructureHeap - MarkedBlock::blockSize;
        RELEASE_ASSERT(mi_manage_os_memory_ex(memory, size, false, false, false, -1, true, &structureArena));
        structureHeap = mi_heap_new_in_arena(structureArena);
#else
```

(`m_useSystemHeap` defaults `true` but line 126 overwrote it with
`!bmalloc::api::isEnabled()` before this branch — the explicit `= true` is
required, not redundant. `tryMallocStructureBlock`/`freeStructureBlock`
then take the `m_lock`-guarded bitvector path, which is already
multi-thread-safe, §0.)

## M11 — `runtime/VM.cpp` (registry lock around `m_microtaskQueues`; §6.5)

Verified current line numbers (spec cites drifted): ~VM force-removal loop is
now `VM.cpp:640-641`; `beginMarking` forEach `1885-1887`;
`visitAggregateImpl` forEach `1893-1895`.

1. Include: add `#include "VMLiteShared.h"` to VM.cpp's include block
   (alphabetical; it already includes `MicrotaskQueue.h` transitively via its
   inlines includes — add `#include "MicrotaskQueueInlines.h"` only if the
   build complains, it is already there today).

2. `~VM` force-removal (640-641) — replace:

```cpp
    while (!m_microtaskQueues.isEmpty())
        m_microtaskQueues.begin()->remove();
```

   with:

```cpp
    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.5(c): the same leaf lock that guards GC-marker
        // iteration (M11 below) and queue ctor/dtor list mutation (M12).
        Locker locker { VMLiteRegistry::singleton().lock };
        while (!m_microtaskQueues.isEmpty())
            m_microtaskQueues.begin()->remove();
    } else {
        while (!m_microtaskQueues.isEmpty())
            m_microtaskQueues.begin()->remove();
    }
```

3. `VM::beginMarking()` (1883-1888) — replace the body with:

```cpp
    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.5: markers traverse the registration list while
        // mutators run; markers hold no other lock here, and holders may
        // acquire NO lock while holding it (leaf, §7).
        Locker locker { VMLiteRegistry::singleton().lock };
        m_microtaskQueues.forEach([&](MicrotaskQueue* microtaskQueue) {
            microtaskQueue->beginMarking();
        });
        return;
    }
    m_microtaskQueues.forEach([&](MicrotaskQueue* microtaskQueue) {
        microtaskQueue->beginMarking();
    });
```

4. `VM::visitAggregateImpl` (1893-1895) — wrap the `m_microtaskQueues.forEach`
   the same way (gate + `Locker locker { VMLiteRegistry::singleton().lock };`
   around ONLY the forEach; the `USE(BUN_JSC_ADDITIONS)` synchronous-module
   loop below it stays outside the lock).

Gating rationale: §6.5 says "may gate on `useVMLite()`" — gating keeps the
flag-off GC paths byte-identical (R3: no new lock acquisitions appear in the
R3(a)-(d) delta list, so flag-off must not take one). Lock-ordering: the
registry lock is a leaf; both marker call sites hold no other lock at these
loops (verified: `beginMarking` is called from Heap with no JSC lock held
across it; `visitAggregateImpl` runs under SlotVisitor, likewise) — the
queues' `beginMarking`/`visitAggregate` only touch their own deques
(fastMalloc, no locks), satisfying the leaf rank.

SCOPE OF THE LOCK (do not overclaim): the registry lock protects LIST
MEMBERSHIP only — append/remove (M12, ~VM force-removal) vs. the markers'
forEach. It does NOT protect queue CONTENTS: inside the locked forEach,
`visitAggregate` walks each queue's `Deque` lock-free while the owner thread's
`enqueue`/`performMicrotaskCheckpoint` mutate that same `Deque` without the
registry lock. Contents are safe by a DIFFERENT, pre-existing invariant,
documented in-tree at `runtime/MicrotaskQueue.cpp:158-160`: deque contents are
read by GC only at `CollectorPhase::FixPoint` and `CollectorPhase::Begin`,
"and both suspend the mutator". Concretely, `VM::visitAggregate` is reached
via the "Sh" (Strong Handles) marking constraint (`heap/Heap.cpp:3221-3228`);
constraints execute in the fixpoint with the world stopped (the constraint's
`ConstraintConcurrency::Concurrent` default means parallel marker threads
WITHIN the stopped fixpoint, not concurrent-with-mutator), and
`VM::beginMarking` is called from `Heap.cpp:969` in the stopped Begin phase.
Under `useJSThreads`, "suspend the mutator" MUST mean ALL mutators stopped
(VMManager N-mutator STW) — that is cross-WS checklist item 12 below, an
explicit integration gate, not an assumption.

## M12 — `runtime/MicrotaskQueue.cpp` (registry lock; §6.5(a)/(b))

Verified current lines: ctor append `104-107`, dtor removal `114-118`.
Include block: add `#include "VMLiteShared.h"` and `#include "Options.h"`
(lines 29-36, alphabetical).

Replace:

```cpp
MicrotaskQueue::MicrotaskQueue(VM& vm)
{
    vm.m_microtaskQueues.append(this);
}
```

with:

```cpp
MicrotaskQueue::MicrotaskQueue(VM& vm)
{
    // SPEC-vmstate §6.5(a): list mutation under the registry leaf lock so a
    // spawned thread's lazy queue creation (VMLite::ensureDefaultMicrotaskQueue)
    // cannot corrupt LIST MEMBERSHIP against GC-marker iteration (M11). The
    // lock covers membership only; queue CONTENTS visiting is safe because
    // deque contents are GC-read only with all mutators suspended (see the
    // M11 scope note and cross-WS item 12 in INTEGRATE-vmstate.md). Gated:
    // flag-off stays lock-free (R3).
    if (Options::useVMLite()) [[unlikely]] {
        Locker locker { VMLiteRegistry::singleton().lock };
        vm.m_microtaskQueues.append(this);
        return;
    }
    vm.m_microtaskQueues.append(this);
}
```

Replace:

```cpp
MicrotaskQueue::~MicrotaskQueue()
{
    if (isOnList())
        remove();
}
```

with:

```cpp
MicrotaskQueue::~MicrotaskQueue()
{
    // SPEC-vmstate §6.5(b). The isOnList() check must be under the same lock
    // as the removal: ~VM's force-removal (M11) can race a dying queue's
    // dtor on another thread post-GIL.
    if (Options::useVMLite()) [[unlikely]] {
        Locker locker { VMLiteRegistry::singleton().lock };
        if (isOnList())
            remove();
        return;
    }
    if (isOnList())
        remove();
}
```

## M13 — `runtime/VMEntryScope.cpp` (I14 debug assert)

1. Include: add `#include "VMLite.h"` (include block, lines 28-35,
   alphabetical).

2. At the TOP of `VMEntryScope::setUpSlow()` (line 39; runs exactly when an
   outermost entry scope is set up — every real VM entry passes here once),
   insert:

```cpp
#if ASSERT_ENABLED
    // SPEC-vmstate I14: an installed VMLite always belongs to the VM whose
    // JSLock this thread holds.
    if (Options::useVMLite()) {
        if (VMLite* lite = VMLite::currentIfExists())
            ASSERT(lite->vm == &m_vm);
    }
#endif
```

(`Options.h` is already included at line 29.) Note this asserts on the
outermost entry only; nested entries reuse the scope and cannot change the
installed lite without going through setCurrent, which M4/api §5.2 keep
coherent.

## Cross-WS checklist (SPEC-vmstate §9, integration phase; verify-only unless noted)

1. **M9 rebase (N3).** Re-anchor the M9 hunk onto the heap WS's FINAL
   `StructureAlignedMemoryAllocator.cpp` before applying (as of task 8 the
   heap WS has not touched the ctor; SPEC-heap has no SAMA provision, so
   conflict risk is low). Requires `Options::useSharedGCHeap()` from
   INTEGRATE-heap's M_opts entry.
2. **S2/S3 audit (with heap WS).** See M7 step 3. Today's allocator critical
   sections are poll-free; the shared-heap WS must preserve that. Re-run the
   audit when the GIL lifts (GIL masks S3 in Phase A).
3. **Single `currentButterflyTID` TU (§6.7 ODR).** Exactly ONE defining TU:
   `runtime/VMLite.cpp`. The `#if !__has_include("VMLite.h")` shims in
   `runtime/ConcurrentButterfly.h:73-80` (objectmodel) and
   `jit/ConcurrentButterflyOperations.cpp` compile away now that VMLite.h
   exists — verify no third definition appears at final merge
   (`grep -rn "ButterflyTID currentButterflyTID" Source/JavaScriptCore`).
4. **N7 — `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE`.** RESOLVED: heap WS defines it
   (`heap/Heap.h:75`) and lands `incrementSTWForbiddenScope()`/
   `decrementSTWForbiddenScope()` (`heap/Heap.h:445`, `heap/Heap.cpp:3714`).
   Confirm still defined at final merge; `VMLiteShared.cpp`'s shim is then
   active (task-5 log).
5. **N4.** Locker `GCDeferralContext` threading = SPEC-heap L5/I14 verbatim;
   covered by M7 step 2.
6. **N6.** Verify-only: SPEC-api's JSTests runner globs cover
   `JSTests/threads/vmstate/**` (no §8 glob collision; no separate runner
   registration needed).
7. **N8.** CLOSED (api r11 §4.6.1/5.2): spawned-thread teardown runs
   unregister + `setCurrent(nullptr)` + tag clear UNDER the final JSLock
   hold (registry lock is leaf), destroy after release. Verify api's spawn/
   teardown code matches at merge; the ~VM no-other-lite assert (M6.10) is
   the backstop.
8. **TID-tag hook = jit task 1b.** `jit/ConcurrentButterflyOperations.cpp:199`
   registers `setVMLiteTIDTagHook(&updateButterflyTIDTag)` under its
   `JSC_JIT_HAS_VMLITE` guard; verify the guard is enabled once VMLite.h is
   in the build, and that api §5.2's explicit P5/clear calls remain
   (idempotent with the hook).
9. **SPEC-heap §7 cite refresh.** Confirm SPEC-vmstate §7's rank table still
   matches SPEC-heap §6's master ordering at final merge (rank 7a
   `structureAllocationLock`; atom shard locks / `VMLiteRegistry::lock` /
   `scratchBufferLock` leaf).
10. **Dev-9 / R4 overlay.** Verify-only (api r9): M_opts is orchestrator-
    pre-applied per §3 R4; task-10 self-checks build in a private overlay
    worktree carrying M3/M4/M6/M9/M11-M13 (hunks never committed from there).
11. **Phase-B charter note (Dev 10; record orchestrator sign-off here).**
    Before any baked DFG/FTL scratch-buffer pointer becomes VMLite-relative
    (§6.6), conservative root gathering must extend to lite scratch buffers —
    iterate `VMLiteRegistry::singleton().lites` under its lock (or a per-lite
    hook from `VM::gatherScratchBufferRoots`) taking each lite's
    `scratchBufferLock`. Phase A is safe without it: the helpers are
    test-only and documented to keep activeLength 0 (VMLite.h GC caveat).
    Phase B also covers thread-granular STW (per-thread
    `VMThreadContext`/`VMTraps`, §6.8) and gates the N-mutator perf
    milestone (api §2).
12. **Microtask deque-contents visiting requires all-mutators-stopped (with
    heap/api WS).** The M11/M12 registry lock protects only list MEMBERSHIP
    of `VM::m_microtaskQueues` (see the M11 scope note). Queue CONTENTS are
    visited lock-free by `MicrotaskQueue::visitAggregate` /
    `MarkedMicrotaskDeque::visitAggregateImpl`
    (`runtime/MicrotaskQueue.cpp:151-167`), sound today because GC reads
    deque contents only at FixPoint/Begin with the mutator suspended (the
    in-tree comment at `MicrotaskQueue.cpp:158-160`; the "Sh" constraint,
    `heap/Heap.cpp:3221-3228`, executes in the stopped fixpoint —
    `worldShouldBeSuspended` returns true for `Begin` and `Fixpoint`,
    `heap/CollectorPhase.cpp:33-44`; `ConstraintConcurrency::Concurrent` =
    parallel markers within the stopped fixpoint, not
    concurrent-with-mutator). VERIFY at integration, and
    re-verify when the GIL lifts: under `useJSThreads`, the constraint solve
    and `VM::beginMarking` run only inside a VMManager stop-the-world where
    ALL mutators (every registered VMLite thread) are stopped. If any future
    heap change lets constraints run concurrently with mutators, per-queue
    content locking (a per-MicrotaskQueue lock taken by owner
    enqueue/dequeue AND visitAggregate; registry lock stays
    membership-only) becomes mandatory — treat that as a blocker, not a
    follow-up.
13. **M4 main-carrier sharing must end before GIL-off (with api/objectmodel
    WS; BLOCKER if violated).** The M4 install path hands `VM::m_mainVMLite`
    (tid 0) to EVERY thread that acquires the API lock without its own lite
    of this VM — main thread, embedder threads, and threads entering a
    foreign VM alike. That is sound only while the API lock is mutually
    exclusive among mutators (phase-1 GIL): butterfly TID tags persist in
    object headers after unlock, and two threads concurrently believing they
    are TID-0 owners would race unlocked flat-butterfly transitions
    (SPEC-objectmodel §2/§3 — exactly the corruption the TID scheme
    prevents). Before ANY configuration runs with the API lock no longer
    mutually exclusive among mutators, the M4 install path MUST be replaced
    by per-thread carriers with unique TIDs for embedder threads (api WS
    allocates via ThreadManager); two threads must never be installed with
    the same tid concurrently, and a thread must never execute JS in VM B
    tagged 0 while owning VM A's lite. Mechanical backstop already in the M4
    hunk: `RELEASE_ASSERT(!Options::useJSThreads() || Options::useThreadGIL())`
    on the install path fail-stops any GIL-off run that still reaches it.
14. **`useThreads` alias must keep R2 true (with api WS).** Until
    INTEGRATE-api 9.2-1 removes the prep-stub alias (OptionsList.h:685,
    honored by ThreadManager.h `useJSThreadsEnabled()`), the M_opts2 hunk's
    leading normalization line (`if (Options::useThreads())
    Options::useJSThreads() = true;`) is what keeps `--useThreads=1` from
    enabling the Thread API with useSharedAtomStringTable/useVMLite/
    useStructureAllocationLock all off (per-thread atom tables under threads
    = broken atom identity + I17 fail-stop; unlocked Structure allocation).
    Whichever lands first is fine; once 9.2-1 lands, drop the normalization
    line together with the alias. Verify at final merge that no spelling
    that enables the Thread API bypasses the R2 implication.
15. **Embedder pre-latch threads and the §4.8 ref/deref rule (Bun
    integration verify).** The §4.8 contract (SharedAtomStringTable.h) has
    NO pre-latch-owned exemption for ref/deref: because
    `AtomStringImpl::addSlowCase` atomizes caller-owned StringImpls IN
    PLACE, a string co-owned by a thread that predates the latch can become
    a shard atom after the latch; that thread's next ref/deref of ANY
    WTF::String must therefore happen-after the latch (any release/acquire
    channel through which it learned JSC is initialized suffices). Verify
    Bun's embedder threads that exist before `JSC::initialize` (e.g. early
    service/IO threads holding WTF::Strings) all synchronize-with
    initialization before they next deref any string. The
    pre-latch-owned exemption survives ONLY for atomize/lookUp.
16. **M4's `Options::useThreadGIL()` read vs INTEGRATE-api 9.2-1's deletion
    of that option (with api WS; MUST be resolved at integration, BLOCKER if
    both applied blind).** Direct manifest-vs-manifest conflict, distinct
    from item 14 (which covers only the `useThreads` alias half):
    INTEGRATE-api 9.2-1 carries a ready-to-apply OptionsList.h diff that
    deletes BOTH `useThreads` (:685) AND `useThreadGIL` (:686), justified by
    "no call site reads Options::useThreadGIL() directly (grep-verified at
    task 14)". That grep predates this manifest's M4 hunk, whose item-13
    backstop is exactly such a read:
    `RELEASE_ASSERT(!Options::useJSThreads() || Options::useThreadGIL());`
    on the JSLock install path. Consequences if applied blind: 9.2-1 then M4
    = JSLock.cpp does not compile (`useThreadGIL` undefined); "fixing" the
    compile error by dropping the assert silently removes the ONLY
    mechanical guard against running the shared-main-carrier install path
    GIL-off (item 13's flat-butterfly TID-0 aliasing corruption).
    Resolution — exactly one of:
    (a) Amend api 9.2-1 to KEEP `useThreadGIL` (THREAD.md's vmstate mandate
        keeps the prep GIL as an Option-controlled layer; M4 now reads it);
        drop only the `useThreads` alias per item 14. Preferred while
        THREAD.md's `--useThreadGIL` wording stands.
    (b) If `useThreadGIL` is removed (api reserves `jsThreadGILTimeSliceMs`
        as the GIL knob and calls `useThreadGIL` dead weight — note the two
        specs genuinely disagree about which spelling expresses "GIL on", so
        under api semantics a GIL-off run would NOT be expressed as
        `useThreadGIL=false` and the M4 assert would be vacuous anyway),
        the M4 backstop MUST be re-expressed in the same hunk against the
        GIL signal the api WS actually honors (e.g. a ThreadManager-exported
        `jsThreadGILEnabled()` predicate), never deleted; also update the
        M_opts anchor note (already 9.2-1-tolerant, see M_opts) and the
        `--useThreadGIL` row text in `JSTests/threads/vmstate/README.md`'s
        flag-matrix section.
    INTEGRATE-api.md is not in this workstream's write set, so the mirror
    note into its 9.2-1 entry is owed by the api WS/integrator — whichever
    manifest is applied second must check this item first. (Round 4: also
    surfaced in the ADJUDICATE-FIRST banner at the top of this file.)
17. **`StringImpl::setNeverAtomize()` now returns bool — Bun embedder must
    check it before any early buffer release (Bun integration verify; round
    4).** The round-4 TOCTOU closure made `setNeverAtomize()` a CAS that
    REFUSES (returns false, flag not set) when the string is already an
    atom: in shared-atom-table mode it can legally lose the race with a
    concurrent in-place atomization (`trySetIsAtomIfAtomizable()` on the
    same atomic word adjudicates; exactly one side wins). On a false
    return the string is (or is becoming) shard-resident and table probes
    read its characters under the shard lock, so the embedder MUST NOT
    call `ExternalStringImpl::releaseBufferEarly()` (or otherwise free the
    buffer) for that string. There are no in-tree callers of
    `setNeverAtomize()` (grep-verified, round 4) — the only callers are in
    Bun; audit them all for the new return-value contract alongside the
    item-15 audit. Legacy mode is unchanged in practice (a false return
    there is the historical caller bug, still debug-asserted).
    ADDITIONALLY (static sibling, same audit): for ExternalStringImpls
    constructed with the no-ctx constructors (these set
    `s_refCountFlagIsStaticString`, so atomization takes the STATIC arm,
    which parks a buffer-ALIASING copy via `createWithoutCopying`), the
    word-level adjudication above does not apply — the parked alias is a
    different StringImpl. `addStaticShared` re-checks `canBecomeAtom()`
    (round-4 hardening, copying fallback), but the airtight guarantee
    requires Bun to call `setNeverAtomize()` BEFORE the string becomes
    visible to any other thread (set-before-sharing), so no atomization of
    it can already be in flight when the flag lands. Verify every Bun
    creation site of an early-releasable external string follows
    create → setNeverAtomize → publish, in that order.

## PENDING (owed by later vmstate tasks)

- **VMLite per-thread-facility C++ tests (task-7 obligations, UNMET; BLOCKER
  for Phase-B routing).** Task 7 handed task 9 a concrete C++ test list for
  the VMLite facilities (I11 owner-only enqueue/drain, lazy-creation
  idempotence, drain-executes-exactly-once, `scratchBufferForSize(0) ==
  nullptr`, geometric-growth reuse, destructor-frees-without-leaks under
  ASAN). Task 9 delivered the JS suite + two W1 WTF tests ONLY; none of the
  VMLite-facility tests exist. Because Phase A is deliberately inert
  (`VM::queueMicrotask`/`drainMicrotasks` not rerouted; no runtime path
  consults a VMLite), the JS suite and the Task-10 B1-B8 gates CANNOT reach
  `ensureDefaultMicrotaskQueue` / `enqueueMicrotaskToDefaultQueue` /
  `drainDefaultMicrotaskQueue` / `scratchBufferForSize` /
  `ensureRegExpAllocator` — these ~150 lines of lifecycle-sensitive code are
  currently UNGATED. They need a JSC-API-level C++ test (TestWebKitAPI
  JavaScriptCore suite — a VM, a JSLockHolder, a registered+installed lite;
  TestWTF cannot link JSC), which is outside this workstream's write set.
  The integrator MUST NOT assume these facilities are covered by the
  existing suites, and the tests MUST land before any Phase-B routing
  consults a VMLite. HARD GATE (review rule, per round-3 adjudication):
  until that TestWebKitAPI test exists and passes, ANY new caller of
  `ensureDefaultMicrotaskQueue` / `enqueueMicrotaskToDefaultQueue` /
  `drainDefaultMicrotaskQueue` / `scratchBufferForSize` /
  `clearScratchBuffers` / `ensureRegExpAllocator` appearing in another
  workstream's diff (Phase-B microtask routing, the api WS spawn path, any
  GC root-gathering change touching `MicrotaskQueue` visitation) is a
  BLOCKER finding by construction — the lock discipline, the GC-visibility
  side effect of the MicrotaskQueue-ctor registry append, and the VMMalloc
  scratch-buffer free path are all unexercised. Mirrored in the VMLite.h
  Group-6 comment.
- ~~M4 (JSLock VMLite install/restore; atom-table swap KEPT verbatim, §4.3
  rev 7)~~ — DONE (task 7; hunk above).
- ~~M6 (VM.h/.cpp X-macro block, asserts, accessor, `m_mainVMLite`, I15
  asserts, ~VM order)~~ — DONE (task 8; hunks above, incl. the two task-7
  requirements: public `mainVMLite()` accessor (M6.2) and the ~VM TOP order
  with `uninstallVMLiteForVMDestruction()` before `willDestroyVM` (M6.10)).
- ~~M7 (verification checklist)~~ — DONE (task 8; checklist above).
- ~~M9 (SAMA §5.1/N3 ctor hunk)~~ — DONE (task 8; hunk above; integrator
  rebases per cross-WS item 1).
- ~~M11/M12/M13 (microtask-registry locking; VMEntryScope I14 assert)~~ —
  DONE (task 7; hunks above).
- M8 — checklist written (task 8, above); the EXECUTION (extractor build,
  static_asserts on GCC+Clang, flags-off smoke) is task 10's self-check, in
  the §3 R4 private overlay worktree.
- ~~Task 9: `JSTests/threads/vmstate/**` (I8/I9/I11/I13/I14 + flag matrix
  documentation); W1 WTF test file content under M14~~ — DONE (task 9; JS
  suite landed in-tree at `JSTests/threads/vmstate/**` with the flag matrix
  in its `README.md`; M14 content extended with the §4.5/I4 tests above).
- ~~Task 10: self-check execution (R3/I4/I10/I13/I16/M8) in the §3 R4 private
  overlay worktree~~ — static half DONE (audit record below); BUILD/RUN half
  owed to the build phase via the overlay recipe in the Task-10 section below
  (this workstream's agent run could not execute builds, tests, or git).

## Task 10 — Self-check record (R3/I4/I10/I13/I16/M8; SPEC-vmstate §10/§11-10)

Two halves. (A) Static audit — executed in THIS run against the live tree;
every item below was verified by reading code, not by inference from specs.
(B) Build/run gates — CANNOT run here (run constraints: no builds, no tests,
no git); they are fully specified below as a mechanical recipe for the §3 R4
private overlay worktree (heap-§14 convention: throwaway private worktree,
manifest hunks applied verbatim, NEVER committed from there).

### A. Static audit — results (all PASS unless marked)

1. **I16 — static_asserts well-formed on GCC+Clang.**
   - `OBJECT_OFFSETOF` (StdLibExtras.h:79-91): Clang arm wraps
     `__builtin_offsetof` in `-Winvalid-offsetof` pragmas; GCC arm is plain
     `offsetof` under a file-level `#pragma GCC diagnostic ignored
     "-Winvalid-offsetof"` — both are constant expressions, so every
     `static_assert(OBJECT_OFFSETOF(...))` in VMLite.h (lines 138-141, 239)
     and in the M6.6 hunk is valid in both toolchains, including on the
     non-standard-layout `VMLitePrimitives`/`VM` (conditionally-supported
     offsetof; both compilers support it; L3 deliberately asserts no
     standard-layout).
   - In-class `static constexpr ptrdiff_t offsetOf_<name>()` bodies using
     `OBJECT_OFFSETOF(VMLitePrimitives, name)` are complete-class-context
     (member function bodies are compiled as if after the closing brace) —
     legal on both compilers; same pattern as existing
     `VM::offsetOfTopCallFrame` style accessors.
   - `static_assert(std::is_trivially_copyable_v<VMLitePrimitives>)`:
     `JSOrWasmInstruction` = `Variant<const JSInstruction*, uintptr_t>`
     (Interpreter.h:61) = `mpark::variant` (wtf/Variant.h:2460), which
     propagates trivial copyability/destructibility for trivial alternatives
     (trait-specialized storage; 7 `trivially` specializations in Variant.h).
     Expected to hold on both toolchains; the overlay compile is the binding
     check (watch item B3).
   - `StringImpl` §4.5 asserts present: StringImpl.h:196-197
     (`sizeof(std::atomic<unsigned>) == sizeof(unsigned)`,
     `is_always_lock_free`) — LLInt/JIT keep reading `m_hashAndFlags` as a
     plain 32-bit load at an unchanged offset.
   - `SharedAtomStringTable::Shard`: `alignas(64)` + padding arithmetic +
     `static_assert(sizeof(Shard) >= 128)` — well-formed; padding array bound
     is a constant expression.
2. **M8 — offlineasm / offset-extractor (static half).**
   - Live `.asm` inventory re-verified (grep over
     `llint/LowLevelInterpreter*.asm`): `VM::topCallFrame` (26),
     `VM::topEntryFrame` (17), `VM::callFrameForCatch` (6), `VM::m_exception`
     (5), `VM::targetInterpreterPCForThrow` (4), `VM::m_threadContext` (4),
     `VM::targetMachinePCForThrow` (3), `VM::encodedHostCallReturnValue` (3),
     `VM::m_typeProfilerLog` (2), `VM::m_shadowChicken` (1),
     `VM::m_lastStackTop` (1). Identical to the M8 checklist inventory; every
     ref is either an X-macro field (name unchanged by M6) or a member M6
     does not move. Group-1 pair contract: first two macro fields, VMLite.h
     0x00/0x08 assert + VM.h:1366 pair assert kept verbatim by M6.
   - Include-cycle check for M6.1: `Interpreter.h` does NOT include `VM.h`
     (its includes: BytecodeIndex.h, JSCJSValue.h, MacroAssemblerCodeRef.h,
     wtf/*); `VM.h` already includes `Interpreter.h` (line 41), so
     VM.h → VMLite.h → Interpreter.h introduces no cycle and
     `LLIntOffsetsExtractor` (a `friend` of VM) sees the same complete types
     as today.
3. **M6 anchors re-verified against the live tree at task 10** (they were
   verified at task 8; re-checked unchanged): VM.h 394-408 (block region),
   879-891 (Group-2 run incl. `m_executingRegExp`), 1237-1240 (Group-3 run
   incl. interleaved `m_currentSoftReservedZoneSize`), 1254 (`private:` after
   `m_microtaskQueues`), 1366 (pair assert), 1205 (`clearException`), include
   slot between `StrongForward.h` (:50) and `VMThreadContext.h` (:51).
   Likewise M_opts2 anchor `Options.cpp:762` (`notifyOptionsChanged`), M3
   anchor `InitializeThreading.cpp:113` (`Options::finalize()`), M_opts
   anchor (`useJSThreads` block at OptionsList.h:681, `useThreadGIL` at
   :686 — prep GIL flag present, default `true`, untouched by this WS).
4. **R3/I4 — flag-off delta inventory (in-tree files).** Exhaustive: the
   committed WTF changes introduce exactly R3(a) (atomic `m_hashAndFlags`
   type + idempotent RMW flag writes; compile-time), R3(b) (ONE latched-flag
   branch in `deref`; legacy arm is today's relaxed `fetch_sub` verbatim —
   re-read at StringImpl.h:1292-1297), R3(c) (one latched branch per routed
   `ASI.cpp` entry; all 23 `AtomStringTableLocker` mentions sit in legacy
   arms guarded by `ASSERT(!sharedAtomStringTableEnabled())` drift asserts).
   The I17 `~AtomStringTable` change is shared-mode-only; the legacy
   `setIsAtom(false)` loop is verbatim. R3(d) exists ONLY in the M6 hunk
   (manifest), not in-tree. No other behavioral surface found.
5. **I10 — locker shape.** `StructureAllocationLocker` ctor's first statement
   is `if (!Options::useStructureAllocationLock()) [[likely]] return;` and the
   dtor's is `if (!m_vm) return;` — flag off compiles to one predictable
   latched-option branch each; `deferralContext()` stays null.
6. **I13 substrate — Phase-A inertness.** No interpreter/JIT/runtime path
   consults a VMLite: `VMLiteInlines.h` is included only by `VMLite.cpp`;
   `VM::queueMicrotask`/`drainMicrotasks` are not rerouted; the carriers are
   created/installed only via M6.9/M4 (manifest) under `useVMLite`. With the
   flag off no VMLite is ever constructed; with it on single-threaded,
   nothing reads execution state from one (§6.1.1/§6.1.4 hold by
   construction). Binding check = overlay rows 1-2 (B4/B5).
7. **§6.7 ODR.** Exactly one declaration (VMLite.h:253) and one definition
   (VMLite.cpp:188) of `currentButterflyTID`; the objectmodel shim
   (`ConcurrentButterfly.h:73-80`) and the jit shim
   (`ConcurrentButterflyOperations.cpp`, `JSC_JIT_HAS_VMLITE` at :47-49) are
   both `__has_include("VMLite.h")`-keyed and now compile away/flip to the
   real symbol. The duplicate `using ButterflyTID = uint16_t;` alias in
   ConcurrentButterfly.h is a legal identical redeclaration.
8. **New-TU compile-soundness spot checks** (no build allowed; signatures
   resolved by reading the live headers): `MicrotaskQueue::create(VM&)`
   (MicrotaskQueue.h:219) and `performMicrotaskCheckpoint<false>(VM&,
   callback)` (template<bool> at :251-252) match the VMLite call sites;
   `ScratchBuffer::create`/`VMMalloc` (VM.h:206-234) match VMLite.cpp;
   `AtomStringTable::table()`/`StringTableImpl` are public
   (AtomStringTable.h:41,45) for the §4.8 migration; `WTF::move` exists and
   is the current idiom; `JSTests/threads/resources/assert.js` exists for the
   task-9 `load(..., "caller relative")` paths.

### A2. FINDING (load-bearing, for the orchestrator/build phase)

**The bare committed tree's JSC half does not LINK; M2 is mandatory for ANY
build of this branch, not just flag-on runs.** (UPDATE, review round 2: the
WTF half is RESOLVED in-tree — `SharedAtomStringTable::singleton()` is now
defined in the always-compiled `AtomStringTable.cpp` (next to the latch; one
instance even in DLL builds) and `sharedAtomStringTableEnabled()` is
header-inline in `SharedAtomStringTable.h` (relaxed load of the
`AtomStringTable.cpp`-defined latch), so always-compiled WTF TUs
(`AtomStringImpl.cpp`, `StringImpl.h`'s `deref`) link without
`SharedAtomStringTable.cpp`. That TU now holds only
`enableSharedAtomStringTable()`, whose callers — M3 and the M14 tests — land
in the same batch as M1. M1 is still REQUIRED before applying M3/M14, but no
longer blocks a bare-tree WTF link.) Compiled-today JSC TUs reference symbols
whose defining TUs enter the build only via manifest hunks:

- JSC: `runtime/Structure.cpp` and `bytecode/JSThreadsSafepoint.cpp`
  (both compiled) call `currentButterflyTID()`; with `VMLite.h` now present,
  the ConcurrentButterfly.h inline shim compiles away and the calls bind to
  the `JS_EXPORT_PRIVATE` definition in `runtime/VMLite.cpp` — which is NOT
  in `Sources.txt` until M2 is applied. (`jit/ConcurrentButterflyOperations
  .cpp` likewise references `setVMLiteTIDTagHook` under `JSC_JIT_HAS_VMLITE`,
  but that TU is itself not yet registered — jit WS manifest.)

This is the unavoidable consequence of the shared-hot-file rule (Sources.txt/
CMakeLists.txt are manifest-only) and was anticipated by R4's overlay
convention: **every gate below is defined on the overlay tree, and at
integration M1/M2 must land in the same commit batch as the in-tree WTF/
runtime files.** Additionally, M_opts is NOT yet orchestrator-pre-applied
(re-verified at task 10: no `useSharedAtomStringTable` in OptionsList.h), so
the overlay must apply it explicitly — already first in the apply order.

### B. Build/run gates — overlay recipe (owed to the build phase)

Heap-§14-style private overlay worktree; hunks applied verbatim from this
manifest, in the M6 apply-order note's sequence, and NEVER committed:

```
git worktree add ../vmstate-selfcheck jarred/threads        # private, throwaway
cd ../vmstate-selfcheck
# Apply from docs/threads/INTEGRATE-vmstate.md, in order:
#   M_opts -> M_opts2 -> M1 -> M2 -> M3 -> M6 -> M4 -> M11 -> M12 -> M13 -> M9
#   (+ M14 if running the WTF unit gates here)
```

| # | Gate | Command(s) in the overlay | Pass bar |
|---|---|---|---|
| B1 | R3/I4 all-flags-off build | `bun build.ts debug` (and one release config) | builds clean, zero new warnings in owned files |
| B2 | M8 extractor | build `LLIntOffsetsExtractor` (both GCC and Clang CI toolchains) | builds; offlineasm regenerates; every `.asm` `VM::` ref resolves (inventory in M8 above) |
| B3 | I16 static_asserts | compile `VM.cpp` + `VMLite.cpp` + `VMLiteShared.cpp`, GCC and Clang, debug+release | no static_assert fires (watch: `is_trivially_copyable_v<VMLitePrimitives>` via mpark::variant; M6.6 per-field + span asserts) |
| B4 | I4 flags-off smoke | full pre-existing JSTests + `jsc JSTests/threads/vmstate/flags-off-baseline.js`, default options | diff-free vs. the same suite on `main`'s harness expectations |
| B5 | I13 | `jsc --useVMLite=1 JSTests/threads/vmstate/vmlite-single-thread-identity.js`; full JSTests smoke with `--useVMLite=1`, single-threaded | behavior identical (same digest, same suite results) |
| B6 | I10 | `jsc --useStructureAllocationLock=1 JSTests/threads/vmstate/structure-lock-single-thread.js`; disassemble one `Structure::create` site flags-off | test passes; flags-off codegen delta = one latched branch (R3(c)-analogue for the locker) |
| B7 | R3 bench | bench gate per `docs/threads/BENCH.md`: flags-off vs. baseline; `useVMLite=1` 1-thread | in-noise (README flag-matrix rows 1-2) |
| B8 | composed disasm bar | golden disasm modulo R3(a)-(d) (M8.5) | only the listed deltas |

Rows 3-7 of the README flag matrix (TSAN, race amplifier, MIMALLOC/I9,
M14 WTF unit tests) are integration-phase gates (§10), not Task-10 gates;
they additionally need the heap/api/jit manifests.
