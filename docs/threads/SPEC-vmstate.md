# SPEC-vmstate — Shared VM State (FROZEN, rev 12)

W1 process-global AtomString table; W2 structure-allocation locking; W3 per-thread
"VM-lite" split. **FROZEN**: implement without redesigning; MUST-deviation⇒spec revision.
Rev 13=r4 fixes (R4 prep/overlay; gate split); r1-r3 logs: history
(`SPEC-vmstate-history.md`="history", non-normative). Manifest:
`INTEGRATE-vmstate.md` (§9); design of record: `THREAD.md` (top).
Shorthand: `ASI`=`Source/WTF/wtf/text/AtomStringImpl`, `SI`=`.../StringImpl`,
`AST`=`.../AtomStringTable`, `SAMA`=`heap/StructureAlignedMemoryAllocator.cpp`,
WS=workstream.

## 0. Ground truth (verified in tree; narrative: history)

- Atom table: unlocked hash set, **per-WTF::Thread** (`AST.h:35-49`, `AST.cpp:28-37`, `Threading.h:408-409,244-253`); dtor `setIsAtom(false)`s non-static entries (I17).
- `AtomStringTableLocker` real only under `USE(WEB_THREAD)` (`PlatformUse.h:114-116`) — **no-op on Bun**; 17 sites (`ASI.cpp:42-63,67-70,87-509`).
- Refcount atomic, both ops relaxed (`SI.h:163,1187-1213,593-594`); **statics rest at masked refcount 0, live in the table**; Bun `addStatic` inserts when it carries `StringAtom` (`SI.h:1330-1340,1191-1205`; `ASI.cpp:319-344`).
- `StructureID::decode()`=`m_bits + structureIDBase()` — **pure VA arithmetic** (`StructureID.h:37-97`); one process-global 4GB reservation (`SAMA:98-146,254-257`; `InitializeThreading.cpp:111`).
- Structure-block handout: `USE(LIBPAS)` safe (`SAMA:152`); system-heap bitvector safe (`SAMA:126,167-176,236`); `USE(MIMALLOC)` **unsafe — no lock, thread-affine heaps** (`SAMA:49,138-142,190`; `WebKitFeatures.cmake:83-117`) — §5.1. `StructureMemoryManager` ctor (`SAMA:254-257`; `InitializeThreading.cpp:112`) runs after `Options::initialize` (`:90`; `Options.cpp:762,1111`) — M9.
- Cell handout in a block NOT synchronized (`LocalAllocator.cpp:138,170-171`) — heap WS. `GCDeferralContext(VM&)` stack-local, threaded into the allocator (`GCDeferralContext.h:37-49`); `DeferGC*` plain-`++/--` shared `Heap::m_deferralDepth` (`HeapInlines.h:166-176`) — N4.
- Call-frame pair adjacency assert (`VM.h:405-406,1365`); offset fns (`VM.h:772-796`).
- Stack limits, **two layers**: VM members `VM.h:1237-1240` via `updateStackLimits()` (`VM.cpp:1162-1165,1206,684`) vs generated-code limits=`VMTraps::m_stack` via `VM::m_threadContext` (`VM.h:438`; `VMThreadContext.h:33`; `LowLevelInterpreter.asm:277-280`) — §6.8.
- Scratch buffers main-thread-only (`VM.h:893-897,1267-1268`); microtasks: per-VM `SentinelLinkedList` of refcounted unlocked queues (`VM.h:1253`, `MicrotaskQueue.h:215-275`); regexp (`VM.h:891,930-932`).
- GIL hand-off (**load-bearing**, §6.1): `didAcquireLock` swaps the atom table then rewrites `lastStackTop`/`stackPointerAtVMEntry` (`JSLock.cpp:115-145`); outermost only, via `m_entryAtomStringTable` (`JSLock.h:162`).
- offlineasm: `VM::field` in `.asm`=`OFFLINE_ASM_OFFSETOF`=`OBJECT_OFFSETOF` (`generate_offset_extractor.rb:135`, `StdLibExtras.h:79-91`)⇒field MUST be a real non-static `VM` member; chained offsets supported.

## 1. Deviations from THREAD.md

1. Atom "flip" is partial: table per-`WTF::Thread`, JSLock-swapped; real work =
   global sharded table+migration (§4.8); swap kept (§4.3).
2. Hard part=refcount lifecycle (§4.4); plain `m_hashAndFlags`⇒publication
   races (§4.5).
3. Block handout unsafe under `USE(MIMALLOC)` (§5.1); the lock serializes
   Structure *cell* allocation+ID-creating transitions; IDs=addresses.
4. VMManager has non-wasm-debugger users (`VMManager.h:272-276`); benign.
5. Microtask split keeps a GC-visible registration list (§6.5).
6. Generated code checks `VMTraps::StackManager`, not bare VM members (§0);
   per-threading=Phase B; frozen here: Group 3+§6.8.
7. Phase A `VMLite`s=carriers only (§6.1); THREAD.md=end state.
8. `~StringImpl` symbol arm (`SI.cpp:132-137`) safe: registry holds STRONG refs
   (`SymbolRegistry.cpp:58`, teardown `:40-43`); post-GIL=§2 non-goal (history).
9. RESOLVED (api r9; verify-only at INT).
10. Phase B UNOWNED (SPEC-jit.md:24 scopes `VMLite` Out, no Phase-B tasks);
    "Phase B" refs here=frozen contract for a FUTURE chartered WS; GIL
    milestone independent — but Phase B GATES the N-mutator perf milestone
    (api §2 composed-deliverable note); INTEGRATE records orchestrator
    sign-off/charter. r12: Phase B ALSO covers thread-granular STW — per-thread
    `VMThreadContext`/`VMTraps` (§6.8)+VMManager counting entered THREADS per
    VM (per-thread NVS tickets); jit R1.c re-frozen there (api §2).

## 2. Scope and non-goals

In scope: W1 (`Source/WTF/wtf/text/**`), W2+§7 ranks, W3 (`runtime/VMLite*`).
Non-goals (interfaces here): cell-level concurrent allocation
(heap WS); butterfly tagging/segmenting/watchpoint elision (objectmodel WS;
consumes §6.7); Phase-B JIT/LLInt codegen incl. per-thread
`VMThreadContext`/`VMTraps` (UNOWNED — Dev 10); `Thread()`/GIL/TIDs (api WS);
post-GIL `Symbol.for`/`SymbolRegistry` sync (UNOWNED — Dev 8).

## 3. Options flags (manifest M_opts)

Default `false`, `JSC_OPTIONS` (`OptionsList.h` shared-hot; ships via M_opts):

```
v(Bool, useSharedAtomStringTable, false, Normal, "process-global shared atom string table"_s) \
v(Bool, useVMLite, false, Normal, "per-thread VMLite carriers (Phase A: inert)"_s) \
v(Bool, useStructureAllocationLock, false, Normal, "serialize Structure cell allocation + ID-creating transitions"_s) \
```

- R1. `useSharedAtomStringTable` read **once** at `JSC::initialize` after
  `Options::finalize`, latched via `WTF::enableSharedAtomStringTable()` (M3): sets
  `g_sharedAtomStringTableEnabled` (`AST.cpp`)+migrates pre-latch atoms
  (§4.8); immutable after.
- R2. `useJSThreads=1` MUST imply all three on. **Sole provider=M_opts2** (§9)
  in `notifyOptionsChanged()` (`Options.cpp:762`; before any flag consumer, §0);
  SPEC-api owns no such hunk.
- R4 (r13). M_opts is orchestrator-PRE-APPLIED to the shared tree before
  fan-out (jit §10 prep; api 9.2-1 canonical for useJSThreads); NO local
  OptionsList patches. M_opts2 stays INT. Task-10 self-checks build in a
  heap-§14-style private overlay worktree carrying M3/M4/M6/M9/M11-M13
  (hunks never committed).
- R3. All three off⇒**behavior** identical; gate=in-noise perf (bench),
  NOT instruction identity. Flag-off codegen deltas (behavior-neutral,
  bench-gated): (a) §4.5 atomic type+RMW flag writes; (b) one latched-flag
  branch in `deref` (relaxed path kept verbatim, F3); (c) one latched-flag
  branch per routed entry point (§4.3) / locker (I10); (d) M6 compile-time VM
  reordering (offset immediates)+`{ }` init of previously uninitialized
  members (VM.h:878-890). Composed flag-off bar (INT): bench-noise+golden
  disasm modulo each spec's listed deltas ((a)-(d); jit D7; api I1).

## 4. W1 — Process-global sharded atom-string table

Owned: `SharedAtomStringTable.h/.cpp` (new)+existing `ASI`/`AST`/`SI`; WTF CMake: M1.

### 4.2 Data structure layout

```cpp
// wtf/text/SharedAtomStringTable.h
namespace WTF {
class SharedAtomStringTable {
    WTF_MAKE_NONCOPYABLE(SharedAtomStringTable);
public:
    static constexpr unsigned shardCountLog2 = 7;     // 128 shards
    static constexpr unsigned shardCount = 1u << shardCountLog2;
    struct alignas(64) Shard {
        Lock lock;
        AtomStringTable::StringTableImpl table WTF_GUARDED_BY_LOCK(lock);
        // pad so sizeof(Shard) >= 128 (no false sharing); static_assert it.
    };
    WTF_EXPORT_PRIVATE static SharedAtomStringTable& singleton(); // NeverDestroyed
    ALWAYS_INLINE Shard& shardForHash(unsigned hash)
    {
        // 24-bit StringHasher output; HIGH bits pick the shard (buckets use
        // LOW bits, HashTable.h:676,732,772).
        return m_shards[(hash >> (24 - shardCountLog2)) & (shardCount - 1)];
    }
    Shard m_shards[shardCount];
};
WTF_EXPORT_PRIVATE void enableSharedAtomStringTable();  // latch+migrate (§4.8); idempotent; init only
WTF_EXPORT_PRIVATE bool sharedAtomStringTableEnabled(); // relaxed load (F4)
} // namespace WTF
```

Shard selection MUST be `shardForHash` (the `HashTranslator`'s hash) from every
entry path (I5; §4.4.5/§4.8 too).

### 4.3 Routing in ASI.cpp

Frozen rule **A1**: when `sharedAtomStringTableEnabled()`, **no atomization,
lookup, or removal path may read or write any `AtomStringTable` instance** —
thread-current or passed by reference. Exhaustive: 17 locker sites;
`remove`, `lookUp*`, `addSymbol`, `addStatic`, `isInAtomStringTable`; the
explicit-table overloads — `add(AtomStringTable&,...)` `ASI.h:156-176`,
`addSlowCase` `ASI.cpp:411`, `addWithStringTableProvider` `ASI.h:114-120` — MUST
ignore the passed table and route to the shard (history). Frozen
dual-path shape: `if (sharedAtomStringTableEnabled())
[[unlikely]]` → `auto& shard=singleton().shardForHash(HashTranslator::hash(value));
Locker locker { shard.lock }; return addToStringTableImpl<...>(shard.table, value);`
— then the legacy body (`AtomStringTableLocker`+`stringTable()`) verbatim.

Legacy `USE(WEB_THREAD)` path untouched (R3); explicit-table overloads honor their
argument there. **Rev 7: JSLock atom-table swap KEPT in shared mode** — harmless
under A1, keeps all 14
`vm.atomStringTable() == Thread::currentSingleton().atomStringTable()` asserts
true (`Identifier.cpp:77`; `Completion.cpp:63-287`; `Heap.cpp:2348`; history).
None relaxed (ex-M5). `VM::m_atomStringTable` stays allocated; the swap
(`JSLock.cpp:124`)+14 asserts read its POINTER — A1 bans only
atomize/lookup/remove USE. Drift guard (ex-M10): debug
`ASSERT(!sharedAtomStringTableEnabled())` atop each `ASI.cpp` legacy arm.

### 4.4 No-resurrection destroy protocol (normative)

`deref()` commits to destruction pre-lock (§0); a table hit could revive a
dying entry (UAF). Frozen: **refcount 0 is final** — no table hit refs at 0;
exactly one thread destroys a string.

1. `ref()` stays `fetch_add(relaxed)` for callers already owning a reference; NEVER
   used to take a reference *from a shared-table entry*.
2. New `bool StringImpl::tryRefAtom()` (`SI.h`): load `m_refCount` relaxed;
   **static bit set⇒`ref()`, return `true` — no zero check, no CAS** (statics
   rest at masked count 0, §0); else CAS loop: count field
   (`value & ~s_refCountFlagIsStaticString`) == 0⇒**fail**, else
   `compare_exchange_weak(old, old + s_refCountIncrement, relaxed)` (OK under the
   shard lock).
3. `deref()` branches on `sharedAtomStringTableEnabled()` (latched, F4):
   shared⇒`fetch_sub(s_refCountIncrement, release)`; old == increment ⇒
   `atomic_thread_fence(acquire)`; `!isAtom() || !length()`⇒destroy as today,
   else `AtomStringImpl::removeDeadAtom(this)`. Legacy⇒today's relaxed
   path verbatim (no flag-off RMW upgrade).
4. **Table hit paths** (lookups+`add`'s `isNewEntry == false` arm) MUST, under
   the shard lock, `tryRefAtom()` the entry: success⇒return it (`Ref` adopts the
   in-lock ref); failure⇒entry **dead**
   (statics never fail — debug `ASSERT(!entry->isStatic())`): lookups treat as miss;
   `add` removes the dead entry (locked) and inserts fresh.
5. `AtomStringImpl::removeDeadAtom(AtomStringImpl* string)` (new, ASI.cpp).
   Precondition: refcount 0⇒caller uniquely owns it. Lock
   `shardForHash(string->existingHash())`; find by existingHash+POINTER
   equality, NOT characters (a racing add may have replaced it); remove
   only on pointer match; unlock; `StringImpl::destroy(string)`.
6. Legacy mode: today's exact destructor-driven path (`SI.cpp:122-137`,
   `ASI.cpp:446-455` incl. `RELEASE_ASSERT`) — R3.

`~StringImpl` in shared mode is reached only via `removeDeadAtom` for atoms; skip
its `isAtom()` branch (pass-a-flag or clear `isAtom` under the shard lock
pre-destroy; NEVER touch the table from the dtor). Soundness: history.

### 4.5 `m_hashAndFlags` atomicization

`SI.h:173`→`mutable std::atomic<unsigned>` (compile-time, not flag-gated). Reads →
`load(relaxed)`. Writes→`store(relaxed)` **only when provably unpublished**
(constructors, `translate()` pre-insert), else `fetch_or(relaxed)` on possibly
published strings (`setIsAtom`, `setNeverAtomize`, lazy `setHash`). Lazy-hash race is
benign-by-value but MUST be `fetch_or`-idempotent (plain RMW drops concurrent flag
bits); `setHash` asserts stored == computed when present; `setIsAtom(false)` =
`fetch_and(relaxed)`. R3(a) gate.

### 4.6 Memory fences (W1)

- F1. New-atom publication: all `StringImpl` init (chars, hash, `setIsAtom(true)`)
  happens before `shard.lock` release; consumers acquire the same lock.
- F2. Atom pointer-equality fast paths never touch the table; no fences.
- F3. Shared mode only: `deref` release+acquire-fence-on-zero (§4.4.3; NOT
  seq_cst); flag-off keeps today's relaxed ops (`SI.h:1196,1208`; R3(b)).
- F4. `sharedAtomStringTableEnabled()`=relaxed load; soundness=§4.8 ordering
  contract+thread-creation sync.

### 4.7 Invariants

- **I1.** Shared mode: a character sequence has ≤ 1 `AtomStringImpl` with nonzero
  refcount reachable from the table at any instant.
- **I2.** `isAtom()` ∧ refcount > 0⇒present in exactly
  `shardForHash(existingHash())`, or a static/empty atom (`SI.cpp:120`).
- **I3.** No destroy while any thread holds a reference; no table hit returns a
  string whose refcount was 0.
- **I4.** Flag off⇒zero behavioral diff modulo the R3(a)-(d) deltas.
- **I5.** Shard choice=pure function of the hash; equal strings always contend on
  one lock.
- **I6.** Per refcount-reaches-0, exactly one `removeDeadAtom`; any removed
  entry pointer-equals the dying string; removal conditional — NO unconditional
  `RELEASE_ASSERT(wasRemoved)`; debug-assert identity.
- **I7.** No JS-heap allocation, GC, or JSC lock acquisition under a shard lock
  (leaf — §7); the table only `fastMalloc`s.
- **I19.** Static atoms survive shared mode: atomizing a table-resident
  `StaticStringImpl`'s characters from any thread returns the SAME pointer; never
  evicted by §4.4.4 replace or `removeDeadAtom` (tests §10).
- **I17.** Shared mode: every per-thread `AtomStringTable` empty from latch on.
  Enforcement (frozen; owned `AST.cpp` dtor): shared mode ⇒
  `RELEASE_ASSERT(m_table.isEmpty())`, skipping the `setIsAtom(false)` loop
  (flag-strip would bypass `removeDeadAtom`); breaches fail-stop at thread death.

### 4.8 Pre-latch atom migration (normative)

Unmigrated pre-latch atoms⇒duplicates (I1)+shard-miss at death.
`enableSharedAtomStringTable()` MUST, after the latch: (1) iterate
`Thread::currentSingleton().atomStringTable()->table()`, inserting each entry
into `shardForHash(entry->existingHash())` under that shard's lock; (2) `clear()`
the source set (do NOT clear `isAtom`); (3) per-VM tables need no migration (no
`VM` pre-`JSC::initialize`). Ordering contract (§8): NO other thread may atomize
before `JSC::initialize` returns — binds embedder AND service threads
(GC/JIT/sampler don't atomize). Not assertable at breach; backstop=I17.

## 5. W2 — Structure allocation locking

### 5.1 Existing vs. added

Sufficient as-is: VA reservation+`structureIDBase`, once-only init,
libpas/system-heap block handout (§0). **NOT sufficient: `USE(MIMALLOC)` block
handout** — `mi_heap_t`s are thread-affine. **N3**/hunk **M9** (§9; SPEC-heap has
no SAMA provision): in the ctor `USE(MIMALLOC)` branch (`SAMA:138-142`), when
`useStructureAllocationLock() || useSharedGCHeap()` (readable
there, §0), SKIP arena/heap creation; set `m_useSystemHeap=true;
m_usedBlocks.set(0);` ⇒ locked-bitvector handout for the process lifetime. Flags off ⇒ branch untaken (R3); blocks rare (I9 stresses `USE(MIMALLOC)`).
Added: one process-global lock serializing *Structure cell creation* across
threads sharing a heap (rare; IDs baked into JIT code).

### 5.2 Interface (frozen; `runtime/VMLiteShared.h`)

```cpp
namespace JSC {

class SharedVMState {
    WTF_MAKE_NONCOPYABLE(SharedVMState);
public:
    JS_EXPORT_PRIVATE static SharedVMState& singleton(); // NeverDestroyed

    // Rank: SPEC-heap §6 rank 7a (§7). Recursive acquisition forbidden.
    Lock& structureAllocationLock() { return m_structureAllocationLock; }

    // RAII; no-op unless useStructureAllocationLock(). Frozen member order:
    // std::optional<GCDeferralContext> m_deferralContext FIRST, lock state after.
    // Ctor: lock; incrementSTWForbiddenScope() (N7; SPEC-heap I14); emplace
    // m_deferralContext(vm). Dtor: F5 fence; decrementSTWForbiddenScope();
    // unlock; THEN ~GCDeferralContext (deferred collection after unlock).
    class StructureAllocationLocker {
    public:
        explicit StructureAllocationLocker(VM&);
        ~StructureAllocationLocker();
        // Null when inactive; M7 sites pass it into the cell allocation (L5/I14).
        GCDeferralContext* deferralContext();
    };
private:
    Lock m_structureAllocationLock;
};
} // namespace JSC
```

**N7 (compile shim, frozen).** `Heap::incrementSTWForbiddenScope()/decrement...` =
SPEC-heap §9 hooks, not yet in tree; `VMLiteShared.cpp` calls them only under
`#if defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)` (no-op else); INTEGRATE defines
the macro once heap lands (§9); task 10 builds shim-inactive.

### 5.3 Acquisition sites (integration-phase obligation)

The locker MUST be held across `Structure::create` / `createStructure` /
allocating transition-table insertions (`Structure.cpp`,
`StructureCreateInlines.h`, `StructureTransitionTable.h`) — all
**SPEC-objectmodel-owned**. **N5**: objectmodel **adopted**
(SPEC-objectmodel.md:213) and **owns emitting**
`SharedVMState::StructureAllocationLocker locker { vm };` (passing
`locker.deferralContext()` where accepted). **M7=VERIFICATION checklist, not
hunks**: INTEGRATE audits, adds lockers ONLY where absent — NEVER nested (§5.2
non-recursive⇒self-deadlock). I8 gates at integration.

### 5.4 GC interaction, fences, invariants (W2)

- **S1.** While the lock is held: (a) holder inside the STW-forbidden scope;
  allocations pass the locker's `GCDeferralContext` — may slow-path into fresh
  blocks (heap ranks 7-10) but never *trigger* a sync collection or VMManager
  STW; (b) no safepoint poll, no `collectSync`/handshake. With S3, a holder
  always runs to release⇒no STW cycle.
- **S2.** Allocator internals (`LocalAllocator`, `BlockDirectory`, libpas) MUST
  NEVER acquire `structureAllocationLock`.
- **S3.** No thread may poll a safepoint or park for STW while holding any heap
  allocation lock (ranks 7-10) — else STW deadlock (derivation: history);
  today's allocator critical sections are poll-free; shared-heap WS preserves;
  INTEGRATE checklist w/ S2; GIL masks it in Phase A.
- **N4.** GC deferral=SPEC-heap L5/I14 verbatim: the locker's stack-local
  `GCDeferralContext`, threaded into the M7 sites.
- **F5.** A `StructureID` created on A, read on B, is consumed only via a
  release/acquire-published location or dependency-carrying loads (cell header →
  `decode()`→deref is dependency-ordered on arm64; x86-64 TSO). The locker's
  dtor runs `WTF::storeStoreFence()` after Structure init, before release —
  covers the publish-after-unlock window.
- **I8.** Flag on⇒never two threads simultaneously inside a
  Structure-cell-allocating region; no collection begins while the lock is held.
  (TSAN no-JIT; atomic in-region counter ≤ 1; assert `deferralContext()`
  in-region.)
- **I9.** `StructureID::decode(encode(s)) == s` for structures allocated on any
  thread (asserts `StructureID.h:95`). Stress incl. `USE(MIMALLOC)`.
- **I10.** Flag off⇒locker compiles to one predictable branch on a latched option.

## 6. W3 — VMLite: per-thread execution state

### 6.1 Strategy (normative)

Phase A (this WS)+frozen layout for Phase B (UNOWNED, Dev 10). Phase A rules:

1. Under the GIL, ALL JS-visible execution state lives in `VM` members as today;
   nothing reads/writes execution state in a `VMLite`.
2. Fields stay declared in `VM` under current names (§6.4); zero `.asm`,
   call-site, or offset-fn changes (§0).
3. Per-thread stack limits under the GIL=the JSLock hand-off (§0).
   **Load-bearing; M4 MUST preserve it** (swap kept, §4.3; M4's only change=§6.4.4).
4. Per-thread `VMLite`s are carriers only: TID (§6.7), VM back-pointer, inert
   Groups 4–6; registered/TLS-installed (§6.4.4); no interpreter/JIT/runtime path
   consults them.
5. Behavior-identical with all flags off AND with `useVMLite=1`
   single-threaded (I13).

Phase B (out of scope): pinned register/TLS base; `VM::field` accesses become
`VMLitePrimitives`-relative; per-thread `VMThreadContext`/`VMTraps` (§6.8); main
thread=§6.4(3) view or real `VMLite` (§6.4.4). Contract: frozen layout+§6.4
asserts.

### 6.3 Frozen layout

Frozen ABI artifact: **`VMLitePrimitives`** — Groups 1–3, one X-macro so `VM`
(§6.4) and it cannot drift:

```cpp
// Source/JavaScriptCore/runtime/VMLite.h
namespace JSC {

// Names/types EXACTLY mirror current VM declarations (incl. m_ prefixes) —
// ABI via offsetof and .asm. X-macro authoritative.
#define FOR_EACH_VMLITE_PRIMITIVE_FIELD(v) \
    /* Group 1: pair; order+adjacency ABI (LowLevelInterpreter.asm:1872,1900) */ \
    v(CallFrame*, topCallFrame) \
    v(EntryFrame*, topEntryFrame) \
    /* Group 2: exception/unwind (VM.h:395,397,878-890) */ \
    v(Exception*, m_exception) \
    v(Exception*, m_lastException) \
    v(CallFrame*, callFrameForCatch) \
    v(void*, targetMachinePCForThrow) \
    v(JSOrWasmInstruction, targetInterpreterPCForThrow) \
    v(uintptr_t, targetInterpreterMetadataPCForThrow) \
    v(void*, targetMachinePCAfterCatch) \
    v(CallFrame*, newCallFrameReturnValue) \
    v(EncodedJSValue, encodedHostCallReturnValue) \
    v(uint32_t, targetTryDepthForThrow) \
    v(uint32_t, osrExitIndex) \
    v(unsigned, varargsLength) \
    v(void*, osrExitJumpDestination) \
    /* Group 3: VM stack bookkeeping (VM.h:1237-1240); NOT VMTraps limits (§6.8) */ \
    v(void*, m_stackPointerAtVMEntry) \
    v(void*, m_stackLimit) \
    v(void*, m_lastStackTop)

struct VMLitePrimitives {
#define VMLITE_DECLARE_FIELD(type, name) type name { };
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VMLITE_DECLARE_FIELD)
#undef VMLITE_DECLARE_FIELD
};

// No standard-layout assert (Variant member, rev 7); OBJECT_OFFSETOF OK via
// __builtin_offsetof (§0); §6.4(2) asserts=the contract; includes Interpreter.h.
static_assert(std::is_trivially_copyable_v<VMLitePrimitives>); // variant of trivials
static_assert(OBJECT_OFFSETOF(VMLitePrimitives, topCallFrame) == 0);
static_assert(OBJECT_OFFSETOF(VMLitePrimitives, topEntryFrame) == sizeof(void*),
    "pair-load contract");

class VMLite {
    WTF_MAKE_NONCOPYABLE(VMLite); WTF_MAKE_TZONE_ALLOCATED(VMLite);
public:
    VMLitePrimitives primitives;  // FIRST member, offset 0 (asserted). FROZEN.
    uint16_t tid { 0 };           // ButterflyTID; 0 = main thread. §6.7.
    VM* vm { nullptr };           // set by registerLite(lite, vm) §6.5.1; immutable after
    // Group 4: regexp, lazy:
    RegExp* executingRegExp { nullptr };
    std::unique_ptr<BumpPointerAllocator> regExpAllocator;
    // Group 5: scratch buffers (§6.6):
    Lock scratchBufferLock;
    Vector<ScratchBuffer*> scratchBuffers;
    // Group 6: microtasks (§6.5), lazy:
    RefPtr<MicrotaskQueue> defaultMicrotaskQueue;

    static constexpr ptrdiff_t offsetOfPrimitives() { return OBJECT_OFFSETOF(VMLite, primitives); }
    static constexpr ptrdiff_t offsetOfTID() { return OBJECT_OFFSETOF(VMLite, tid); }
    // Per-field offsets: VMLitePrimitives-relative (Phase B consumes).
    JS_EXPORT_PRIVATE static VMLite* currentIfExists();   // TLS read
    JS_EXPORT_PRIVATE static VMLite& current();           // asserts non-null
    JS_EXPORT_PRIVATE static VMLite* setCurrent(VMLite*); // returns previous
};
static_assert(OBJECT_OFFSETOF(VMLite, primitives) == 0);
} // namespace JSC
```

Deliberately NOT in `VMLitePrimitives`: `m_terminationException` (VM.h:396;
cross-thread by design), `maybeReturnPC`/`topJSPIContext` (VM.h:407-408),
`m_currentSoftReservedZoneSize` (VM.h:1238, interleaved in Group 3; the §6.4(2)
span assert forces it out), `m_executingRegExp` (VM.h:891). M6 relocates these
just outside the block, names/types unchanged (no absolute offsets frozen;
VM.h:1365 pair contract preserved). Placement (frozen): block replaces the
VM.h:395-406 region (:392 hot-fields-top intent); Group-2/3 members move up
into it; relocated members keep their sites.

Freeze rules:

- L1. `VMLitePrimitives` field order frozen (add/remove/reorder=spec revision);
  `primitives` stays at `VMLite` offset 0.
- L2. New `VMLite` fields append after Group 6 only.
- L3. Asserts above MUST remain; NO standard-layout assert (Variant member).
- L4. TLS: `thread_local VMLite* t_currentVMLite` in `VMLite.cpp` (NOT
  `pthread_getspecific`); accessor signatures frozen, impl replaceable in Phase B.
- L5. No numeric offsets frozen beyond "Group 1 pair at 0x00/0x08"; everything
  else via `OBJECT_OFFSETOF`; padding=ABI's choice.

### 6.4 VM integration (Phase A; M6)

**No member deleted from `VM`, no call site respelled, no `.asm` touched.** `VM`
becomes the main thread's physical `VMLitePrimitives`:

1. The Group 1–3 members of `VM` are **reordered into one contiguous block** by
   expanding `FOR_EACH_VMLITE_PRIMITIVE_FIELD(VMLITE_DECLARE_FIELD)` in class
   `VM`, replacing the scattered declarations (VM.h:395,397,405-406,878-890,1237-1240;
   compile-time, not flag-gated — R3(d)). Names unchanged⇒every existing
   spelling (C++, offset fns, asserts, `.asm`) compiles unchanged. **Whole block under one `public:` label** (frozen; history).
2. Equivalence asserts (VM.h via M6): per X-macro field,
   `static_assert(OBJECT_OFFSETOF(VM, <field>) - OBJECT_OFFSETOF(VM, topCallFrame)
   == OBJECT_OFFSETOF(VMLitePrimitives, <field>));` plus span assert
   (`m_lastStackTop` delta+`sizeof(void*)` == `sizeof(VMLitePrimitives)`) ⇒
   layout-identical.
3. `VM` gains `ALWAYS_INLINE VMLitePrimitives& mainVMLitePrimitives()` =
   `*std::bit_cast<VMLitePrimitives*>(std::bit_cast<uint8_t*>(this) +
   OBJECT_OFFSETOF(VM, topCallFrame))` — guarded by point-2 asserts. Phase A:
   tests only.

4. **§6.4.4 Main carrier+install/restore (frozen).** `VM` owns
   `std::unique_ptr<VMLite> m_mainVMLite` (tid 0), created at END of the VM ctor
   when `useVMLite`, registered via `registerLite(*m_mainVMLite, *this)`
   (§6.5.1 sets `vm`); the ctor NEVER calls `setCurrent`. Install/restore mirror
   the atom-table swap (outermost only, §0) — M4:
   - `didAcquireLock`: `cur = currentIfExists()`; install iff `!cur || cur->vm !=
     m_vm` (covers multi-VM-per-thread): `m_entryVMLite =
     setCurrent(m_mainVMLite.get()); m_didInstallVMLite=true`; else no install.
   - api rev 9 §5.2 (api:148): `registerLite`+`setCurrent(lite)` BEFORE the
     first `JSLockHolder`⇒spawned threads' `didAcquireLock` sees `cur->vm ==
     m_vm`, installs nothing (`m_didInstallVMLite` false). Install arm fires
     only for no-lite/foreign-lite entries (main, embedder, multi-VM); GIL ⇒
     ≤1 installed.
   - `willReleaseLock`: if flag set: `setCurrent(m_entryVMLite)` ONLY IF
     `currentIfExists() == m_mainVMLite.get()` — a lite swapped in post-install
     is NEVER clobbered; always clear both members. (DropAllLocks on a spawned
     thread keeps its lite; reacquire⇒no install.)
   - M4 members: `VMLite* m_entryVMLite`, `bool m_didInstallVMLite`;
     `JSLock::uninstallVMLiteForVMDestruction()`=if flag set: if
     `currentIfExists() == m_vm->m_mainVMLite.get()`, `setCurrent(m_entryVMLite)`;
     clear both. `~VM` (M6; holds the API lock, `VM.cpp:630`) at TOP: call it,
     THEN §6.5.1 no-other-lite assert, unregister, destroy `m_mainVMLite`⇒TLS
     never dangles across `lastChanceToFinalize` (**I20**).
   Phase B decides main-thread carrier vs §6.4(3) view.

Offset functions (VM.h:772-796) and the 1365 assert **unchanged**. Groups 4–6 VM
state untouched in Phase A; `VMLite` carries the counterparts (§6.5-6.7).

### 6.5 Microtask queues

- Per-thread default queue: `VMLite::defaultMicrotaskQueue`, lazy.
- GC visibility: `VM::m_microtaskQueues` stays the single registration list, but
  GC markers traverse it **while mutators run** (`VM.cpp:1886-1891`;
  `Heap.cpp:3091-3098`). Frozen protocol — **both sides take
  `VMLiteRegistry::singleton().lock`** (§6.5.1; leaf, §7): (a) ctor append
  (`MicrotaskQueue.cpp:104-107`), (b) dtor removal (`:114-118`) — **M12**; (c)
  `~VM` force-removal (`VM.cpp:635-636`) — **M11**; GC iteration in
  `beginMarking` (`VM.cpp:1878-1883`)+`visitAggregateImpl` — **M11**. Markers
  hold no other lock here; holders may acquire NO lock while holding it.
- **§6.5.1 `VMLiteRegistry`** (frozen; `VMLiteShared.h/.cpp`; NOT in the §6.3 body):

  ```cpp
  struct VMLiteRegistry {
      JS_EXPORT_PRIVATE static VMLiteRegistry& singleton(); // NeverDestroyed
      Lock lock;                                            // leaf rank (§7)
      Vector<VMLite*> lites WTF_GUARDED_BY_LOCK(lock);      // fastMalloc only
      // Takes lock; asserts absent; stores lite.vm=&vm (was null) — sole writer.
      JS_EXPORT_PRIVATE void registerLite(VMLite&, VM&);
      JS_EXPORT_PRIVATE void unregisterLite(VMLite&); // takes lock; asserts present
  };
  ```

  Lifetime (frozen): unregister a lite before destroying it and before its
  thread's teardown `setCurrent(nullptr)`; a `VM` MUST NOT die while a registered
  lite's `vm` points at it (§6.4.4 `~VM` assert, under the lock).
  **N8 RESOLVED (api r11 4.6.1/5.2):** unregister+`setCurrent(nullptr)`+tag
  clear run UNDER the final JSL hold, pre-release (registry lock is leaf);
  destroy after JSL release. Mutators
  register/unregister; markers only iterate. Same lock guards
  `VM::m_microtaskQueues` (M11/M12).
- Phase A: `VM::queueMicrotask`/`drainMicrotasks` NOT rerouted (§6.1.1);
  per-thread queues registered under M11/M12, exercised only by unit tests
  (`VMLiteInlines.h`). Phase B routes to the current thread's queue;
  cross-thread enqueue out of scope.
- **I11.** A per-thread `MicrotaskQueue` is enqueued/drained only by its owner;
  debug assert in the `VMLiteInlines.h` helpers.

### 6.6 Scratch buffers

Baked `scratchBufferForSize` pointers in DFG/FTL code are per-code — concurrent
OSR exits race. Phase A: no change. Frozen Phase-B contract (Dev
10): baked pointers become `VMLite`-relative; reserved: Group 5 +
`ScratchBuffer* VMLite::scratchBufferForSize(size_t)`.

### 6.7 Thread IDs: `currentButterflyTID()`

- `VMLite.h`: `using ButterflyTID = uint16_t;` (15-bit payload, SPEC-objectmodel
  §9.1)+`JS_EXPORT_PRIVATE ButterflyTID currentButterflyTID();` defined in
  `VMLite.cpp`: `auto* l = VMLite::currentIfExists(); return l ? l->tid : 0;` —
  0=main / no installed VMLite. **Sole defining TU**=`VMLite.cpp`
  (INTEGRATE verifies ODR).
- **Authority split (frozen):** `ThreadManager` (api)=**sole TID allocator**;
  spawn (api §5.2) writes `VMLite::tid` **before** `setCurrent`; immutable while
  installed; reads need no sync. Main carrier tid stays 0 (§6.4.4).
- **Embedder threads (frozen):** GIL phase — no installed VMLite⇒0; post-GIL —
  lazy first-entry MUST install a carrier before any object-model fast path;
  never-entering threads touch no JS objects⇒0 unobservable.
- Recycling: phase 1 NONE (exhaustion⇒RangeError); GC-time rebias/reissue=
  CHARTERED-OWNED (OM 8c r12/Task 13+api Dev 10: a shared-GC stop restamps
  dead-TID tags+structure TIDs to 0, then api reissues); N2 closed by non-reuse
  pre-rebias.
- **TID-tag hook (jit CS3/I19 provider):** `VMLite.h/.cpp`:
  `JS_EXPORT_PRIVATE void setVMLiteTIDTagHook(void(*)(uint16_t));` (null
  default); `setCurrent` calls it AFTER the TLS write with `lite ? lite->tid :
  0` (incl. nullptr). jit task 1b registers the P5 tag-update body⇒§6.4.4
  install/restore+multi-VM switches keep `g_jscButterflyTIDTag` coherent (jit
  I19) w/o runtime/→jit/ include; api §5.2 P5/clear calls stay (idempotent);
  null hook⇒no-op (Phase-A standalone builds).

### 6.8 `VMThreadContext`/`VMTraps::StackManager` reconciliation

`VMThreadContext` holds `VMTraps`, whose `StackManager` carries the limits
generated code checks (§0). Frozen: Group 3=ONLY the VM-member bookkeeping,
NO `StackManager` duplicate (one authority: `VMTraps::m_stack`). Phase A:
`VM::m_threadContext` stays; `updateStackLimits()` keeps both layers coherent
via the JSLock hand-off (§6.1.3). Phase B (Dev 10): per-thread
`VMThreadContext`/`VMTraps` per L2; chained offsets sanctioned (§0).

### 6.9 Invariants (W3)

- **I13.** `useVMLite=false`⇒behavior identical+bench-gate in-noise
  (NOT codegen-identical — R3(d)); `useVMLite=true` single-threaded⇒behavior
  identical (Phase A reads nothing from VMLite). Full JSTests+bench gate, both.
- **I14.** Installed `VMLite`⇒`VMLite::current().vm` == the VM whose JSLock the
  thread holds; debug assert in `VMEntryScope` when `useVMLite` — **M13**.
- **I15.** `m_exception`/`m_lastException` written/read only by the JSLock
  holder; debug asserts in the two exception setters (M6).
- **I16.** All §6.3/§6.4 static_asserts hold on every CI toolchain.
- **I18.** `currentButterflyTID()` returns 0 on any thread without an installed
  VMLite, never `notTTLTID` (0x7fff; debug-asserted at `setCurrent`).
- **I20 (rev 8).** No thread's TLS ever points at a destroyed `VMLite` (§6.4.4
  `~VM` uninstall-before-destroy). Debug: `setCurrent`+the `willReleaseLock`
  restore assert the lite is registered; destroyed lites poisoned.

## 7. Lock ordering (subordinate to SPEC-heap §6)

**SPEC-heap §6 owns the master ordering**; ranks here=ITS row ids — on
disagreement SPEC-heap wins. Rows: 1 `JSLock`, 2 GCL, 3
`VMManager::m_worldLock`, 4 GBL, 5 `Heap::m_threadLock`, 6 `HeapClientSet`, 7a
`structureAllocationLock`, 7-10 allocation locks; existing JSC locks unranked.

| SPEC-heap rank | Lock | Notes |
|---|---|---|
| 7a | `structureAllocationLock` (§5) | below 1-6; held *across* 7-10; never waits on STW; allocator internals never acquire (S1-S3) |
| leaf | atom shard locks (§4) | nothing acquirable while held (I7); never inside 7-10; never nest (I5; §4.8 migrates one-at-a-time) |
| leaf | `VMLiteRegistry::lock` (§6.5.1) | nothing acquirable while held; mutators AND GC markers (holding no other lock) |
| leaf | `VMLite::scratchBufferLock` | fastMalloc only |

SPEC-heap cite refresh: INTEGRATE.

## 8. Public interface summary

§3 flags; §4.2 latch/query fns (unchanged `AtomStringImpl` signatures, §4.3);
§5.2 `SharedVMState`; §6.3 `VMLite`/`VMLitePrimitives`; §6.7
`currentButterflyTID()`+TID-tag hook; §6.4.4 main carrier +
`mainVMLitePrimitives()`; §6.5.1 `VMLiteRegistry`; §4.8 embedder ordering
contract (header comment).

## 9. Owned paths and manifest

Writable: `Source/WTF/wtf/text/**`;
`Source/JavaScriptCore/runtime/`{`VMLite.h/.cpp`, `VMLiteInlines.h`,
`VMLiteShared.h/.cpp`} (all new); `JSTests/threads/vmstate/**`;
`Tools/TestWebKitAPI/Tests/WTF/SharedAtomStringTable.cpp` (new);
`docs/threads/INTEGRATE-vmstate.md`.

Tests: `JSTests/threads/vmstate/**`=W2/W3 JS stress (I8/I9/I11/I13/I14); no
SPEC-api §8 glob collision; runner pickup=**N6** (verify-only).
WTF tests (I1-I6, I17, I19; raw `WTF::Thread`)=`SharedAtomStringTable.cpp`;
registration=**M14**.

**No cross-tree mechanical rewrite** (§6.4 keeps every spelling valid). Shared hot
files (M-entry files+`runtime/JSGlobalObject.*`, `llint/*`) MUST NOT be
edited; manifest carries ready-to-apply hunks (EXCEPT M7+cross-WS):

- **M_opts** — the three `OptionsList.h` lines from §3.
- **M_opts2** — `runtime/Options.cpp` (`notifyOptionsChanged`,:762):
  `if (useJSThreads()) { useSharedAtomStringTable()=true; useVMLite()=true;
  useStructureAllocationLock()=true; }` (R2).
- **M1** — `Source/WTF/CMakeLists.txt`: add `text/SharedAtomStringTable.cpp`+header.
- **M2** — JSC `Sources.txt`+`CMakeLists.txt`: add `runtime/VMLite.cpp`,
  `runtime/VMLiteShared.cpp`.
- **M3** — `runtime/InitializeThreading.cpp`: call
  `WTF::enableSharedAtomStringTable()` after `Options::finalize` when flag on.
- **M4** — `runtime/JSLock.cpp/.h`: atom-table swap (123-124,326-328) +
  stack-field updates (126,135-137; §6.1.3) KEPT verbatim (§4.3). Hunk=§6.4.4
  install/conditional-restore, two members, `uninstallVMLiteForVMDestruction()`.
- **M5** — REMOVED rev 7.
- **M6** — `runtime/VM.h`: include `VMLite.h`; the §6.4 block/asserts/accessor
 +§6.3 relocated members; `m_mainVMLite` (§6.4.4); two I15 asserts in the
  exception setters (ONLY changes beyond these). `runtime/VM.cpp`: ctor registers the main carrier; `~VM` top =
  uninstall-then-teardown per §6.4.4/I20. Nothing else (offset fns+1365 assert
  verbatim; M11 separate).
- **M7** — VERIFICATION checklist, not hunks (§5.3/N5: objectmodel emits the
  lockers; integrator audits, fills gaps only, never nests); verify I8 coverage.
- **M8** — verification-only: `LLIntOffsetsExtractor` builds; all `.asm` `VM::`
  refs resolve unchanged after M6.
- **M9** — `heap/StructureAlignedMemoryAllocator.cpp`: the §5.1/N3 ctor hunk;
  INTEGRATE rebases onto heap WS's final `SAMA`.
- **M10** — REMOVED rev 9 (history): replaced by §4.3 legacy-arm asserts
  (owned ASI.cpp, no hunk).
- **M11** — `runtime/VM.cpp`: registry lock around the `m_microtaskQueues.forEach`
  loops in `beginMarking` (1878-1883) and `visitAggregateImpl` (1886-1891) AND the
  `~VM` force-removal loop (635-636; §6.5(c)); may gate on `useVMLite()`.
- **M12** — `runtime/MicrotaskQueue.cpp`: registry lock around ctor append
  (104-107), dtor removal (114-118).
- **M13** — `runtime/VMEntryScope.cpp`: I14 debug assert.
- **M14** — `Tools/TestWebKitAPI/CMakeLists.txt`: register
  `Tests/WTF/SharedAtomStringTable.cpp`.
- Cross-WS (INTEGRATE): M9 rebase (N3); S2/S3 audit; single
  `currentButterflyTID` TU (§6.7); define `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` when
  heap lands (N7); N4 note; SPEC-heap cite refresh (§7); **N6** verify-only (api globs
  cover us); Dev-9 verify-only; **N8**
  closed (§6.5.1; api r11; verify-only); TID-tag hook=jit task 1b (§6.7).

## 10. Verification hooks

TSAN no-JIT: `JSTests/threads/` (incl. `vmstate/**`), all three §3 flags on (binds
I19). WTF unit tests (M14): I1-I6, I17, I19. Race amplifier: atomize/drop churn
(I1/I3/I6), static-atom churn (I19), structure churn (I8/I9; ≥1 `USE(MIMALLOC)`
config). Bench gate: flags-off perf-neutral (R3/I4/I10/I13); `useVMLite=1` 1-thread
in-noise; flag-ON atomization microbench recorded;
budget: jit Task-13 gate ({useJSThreads=1,useSharedGCHeap=0} <=5%; {1,1} recorded — heap dev-7 r13 split). Compile gate: I16+M8.

## 11. Ordered task list

1. **W1 scaffolding**: `SharedAtomStringTable.{h,cpp}` (§4.2); latch+migration
   (§4.8); unit-test shard selection (I5), migration (I17).
2. **W1 atomics**: §4.5 atomic `m_hashAndFlags`; `tryRefAtom()` (§4.4.2); `deref`
   upgrade (F3); existing string tests.
3. **W1 routing**: dual-path 17 locker sites+explicit-table overloads per A1
   (§4.3); table hits via `tryRefAtom()` under the lock (§4.4.4).
4. **W1 lifecycle**: `removeDeadAtom`+destructor bypass (§4.4.5); dead-entry
   stress (I1-I3, I6); static-atom test (I19), TSAN.
5. **W2**: `VMLiteShared.{h,cpp}` — `SharedVMState`, option-gated locker (N7 shim
  +`GCDeferralContext`, §5.2/S1/N4), F5, I8 counter; `VMLiteRegistry` (§6.5.1).
6. **W3 struct**: `VMLite.{h,cpp}`+`VMLiteInlines.h` (§6.3), TLS (L4),
   `currentButterflyTID()` (§6.7).
7. **W3 plumbing**: per-thread facilities (§6.5), unit-tested, inert (§6.1.4).
8. **Manifest**: `INTEGRATE-vmstate.md` — M_opts/M_opts2/M1-M4/M6/M8-M9/M11-M14
   as ready-to-apply hunks (M7+cross-WS=checklists); M6 X-macro from live VM.h.
9. **Tests**: W1 in `Tests/WTF/SharedAtomStringTable.cpp` (+M14); W2/W3 in
   `JSTests/threads/vmstate/**`; document flag matrix.
10. **Self-check**: R3/I4/I10/I13/I16/M8 — all-flags-off build, JSTests smoke
    diff-free, extractor builds, static_asserts (GCC+Clang).
