# SPEC-vmstate — revision history and review-resolution log

This file holds, verbatim, the non-normative review-resolution logs and rejected-design
records that lived in `SPEC-vmstate.md` through rev 3. Rev 4 moved them here unchanged to
meet the 40KB size cap on the spec proper. The spec is the sole normative document; this
file is a record only.

## Rev preambles

- **Rev 2** incorporated the round-1 adversarial-review resolutions (§1.2 below).
- **Rev 3** incorporated the round-2 resolutions (§1.3 below): static-atom handling in
  `tryRefAtom` (spec §4.4), the `USE(MIMALLOC)` structure-block path (spec §0.2/§5.1),
  GC-concurrent microtask-queue registration (spec §6.5), the single-access-specifier
  X-macro block (spec §6.4), the SPEC-heap locker/rank reconciliation (spec §5.2/§5.4/§7),
  the M7 ownership transfer (spec §5.3), manifest entries for previously homeless asserts
  (M11–M14), and owned test paths (spec §9).
- **Rev 4** is a pure compression of rev 3: normative content (layouts, signatures,
  numbered invariants, lock ranks, manifest, task list) unchanged; resolution logs moved
  here; rationale prose tightened. No design change.

Cross-spec citation convention (established rev 3): the four sibling specs are revised
concurrently, so their line numbers drift between reads (observed during round 2:
SPEC-api's §7 interface block moved ~160 lines between two reads of the same tree).
Sibling-spec references therefore cite **section numbers plus short quoted anchors**,
never bare line numbers. Line:number citations are used only for source files the spec's
ground truth was verified against.

## §1.2 Round-1 review resolutions (all verified against the tree; none refuted)

Every blocker/major from round 1 was reproduced from source. Resolution map:

- **§4.4 resurrection UAF / double-destroy** (blocker, filed twice) — confirmed: with
  resurrection permitted, thread A (committed to destroy, pre-lock — `StringImpl.h:1208`
  commits before any lock) and a resurrect-then-re-deref thread B could both enter the
  destroy path; B could free the string before A's recheck (UAF). Rev 2 **forbids
  resurrection**: table hits use `tryRefAtom()` (CAS-from-nonzero) under the shard lock;
  refcount 0 is final; exactly one thread destroys. §4.4 rewritten; I2/I3/I6 re-derived;
  the unconditional `RELEASE_ASSERT(wasRemoved)` is gone in shared mode (removal is
  conditional on pointer identity).
- **M8 "static constexpr offset aliases" impossible** (blocker, filed twice) — confirmed
  (§0.4: `OFFLINE_ASM_OFFSETOF` → `offsetof` needs a real data member). Rev 2 removes the
  need entirely: **Phase A keeps every Group 1–3 field physically declared in `VM` under
  its current name** (X-macro mirror, §6.4). Zero `.asm` edits, `offsetof(VM, topCallFrame)`
  stays legal, and the rev-1 mass rewrite of `vm.topCallFrame` spellings (old M9 — which
  also violated the owned-path rule) is deleted.
- **`static_assert(is_standard_layout_v<VMLite>)` unsatisfiable** (major, filed twice) —
  confirmed (`WTF::Vector` splits members across `VectorBuffer` base and derived,
  `Vector.h:217-300,620`; `unique_ptr` standard-layout is not guaranteed cross-STL). Rev 2
  asserts standard-layout + trivial-copyability only on the POD prefix `VMLitePrimitives`
  (§6.3); non-POD members live after it, unasserted. Note `OBJECT_OFFSETOF` itself never
  needed standard-layout (§0.4 point 2).
- **§7 rank table self-contradiction (40 vs 30)** (major, filed twice) — confirmed. Rev 2:
  `structureAllocationLock` sits after `JSLock`, before the heap allocation locks; the
  "rank 45 leaf usage" patch-prose is deleted. (Rev 3 further demotes §7's numbering to
  SPEC-heap §6's master table — §1.3.)
- **GC / stop-the-world while holding structureAllocationLock** (major, filed twice) —
  confirmed deadlock shape. Rev 2: the locker enters `DeferGCForAWhile`
  (`heap/DeferGC.h:50`) and safepoint polls are forbidden while held (§5.4 rule S1), so a
  holder never waits on other mutators and waiters cannot deadlock.
- **§6.1 Phase-A GIL paragraph self-contradictory / stack-limit location unspecified**
  (major) — confirmed. §6.1 is now normative: all JS-visible state on the VM's fields;
  `JSLock::didAcquireLock`'s per-thread rewrite of the stack fields
  (`JSLock.cpp:115-145`, §0.3) is the load-bearing mechanism and M4 MUST preserve it; the
  "per-thread VMLite for native-side state" sentence is deleted.
- **Pre-latch atoms break shared mode** (major, filed twice) — confirmed. §4.8: the latch
  migrates the initializing thread's table into the shards; embedder ordering contract in
  §8; per-VM tables cannot exist pre-`JSC::initialize` (VMs require it).
- **Explicit-table overloads unspecified** (major) — confirmed (§0.1 bullet 5). §4.3 rule
  A1 now covers *every* `AtomStringTable&`-taking path and `VM::m_atomStringTable` (M10).
- **VMThreadContext/VMTraps StackManager ignored** (major) — confirmed (§0.3). §6.8 added;
  Group 3 is explicitly NOT the generated-code limits.
- **§6.4 garbled / ownership-violating rewrite; accessor-name drift** (major) — confirmed.
  §6.4 rewritten as a single decision with zero call-site rewrites; old M9 deleted.
- **`currentButterflyTID` has no provider** (blocker) — confirmed at round-1 time. §6.7
  defines it in `VMLite.cpp`; `ThreadManager` (SPEC-api) is the sole TID allocator;
  recycling policy reconciled in §6.7 (note N2). *(Rev-3 update: SPEC-api's rev 2
  independently added its own definition, creating an ODR conflict; SPEC-api rev 3 has
  since deleted it and named this spec's §6.7 as provider — see §0.5 and §1.3.)*
- Minor: rev-1's `0x00`–`0x38` byte-offset comments assumed padding facts not asserted —
  removed; offsets are consumed only via `offsetOf*()` functions and static_asserts.

## §1.3 Round-2 review resolutions (rev 3; each verified against the tree / sibling specs)

- **§4.4 `tryRefAtom` treats live static atoms as dead** (blocker, filed twice) —
  **confirmed**: statics rest at masked count 0 and live in the table (§0.1 static-string
  bullet, with line evidence). Rev-2's "static strings always have a huge count" rationale
  was false. §4.4 step 2 now checks the static bit FIRST and succeeds unconditionally
  (delegating to `ref()`, whose TSAN early-out keeps statics' counts untouched there);
  the dead-entry remove/replace arm debug-asserts `!isStatic()`; new invariant I19 covers
  static atoms in shared mode.
- **`currentButterflyTID` defined by both this spec and SPEC-api** (blocker, filed twice)
  — **confirmed for SPEC-api rev 2, already resolved cross-spec**: SPEC-api rev 3's
  changelog deletes its definition and names §6.7 as sole provider, and its §5.2 spawn
  path "adopts the VMLite tid handshake" (so the sub-claim "SPEC-api never mentions VMLite"
  is true only of SPEC-api rev 2 and is **refuted against rev 3**, which references the
  handshake in its changelog, resolutions list, and §5.2). §6.7 adds the integration gate
  (exactly one exported definition, in `VMLite.cpp`) and reconciles the embedder
  lazy-TID semantics. §0.5's stale line citations are re-derived as section+anchor cites.
- **`USE(MIMALLOC)` structure-block path not thread-safe** (major) — **confirmed**
  (§0.2 path 3). §5.1 now specifies the treatment: in shared mode, mimalloc builds MUST
  NOT call `mi_heap_malloc_aligned` cross-thread; block handout routes through the
  already-thread-safe locked-bitvector mechanism (cross-spec note N3 — the file is
  heap-workstream-owned).
- **§6.5 GC visibility claim wrong (visitAggregate runs from a Concurrent constraint)**
  (major) — **confirmed**: the "Strong Handles" constraint (`heap/Heap.cpp:3091-3098`)
  is registered without `ConstraintConcurrency::Sequential` and the default is
  `Concurrent` (`heap/MarkingConstraintSet.h:53`, `heap/MarkingConstraint.h:51`), so
  `VM::visitAggregateImpl` (`VM.cpp:1886-1891`) can run on marker threads while mutators
  run. §6.5 now requires the registry lock on BOTH sides, with manifest entries M11
  (VM.cpp iteration) and M12 (`MicrotaskQueue.cpp` registration — the existing insertion
  point `MicrotaskQueue::MicrotaskQueue` → `vm.m_microtaskQueues.append`,
  `runtime/MicrotaskQueue.cpp:104-107`, previously had no manifest entry).
- **§6.4.1 X-macro cannot interleave `public:`/`private:` labels** (major, filed twice) —
  **confirmed** (access is genuinely mixed today: `private:` at VM.h:394 covers
  `m_exception`/`m_lastException`, `public:` at VM.h:398 covers the call-frame pair; Group
  3 sits under the `private:` at VM.h:1167). §6.4.1 now declares the whole block under a
  single `public:` label (option (b) from the finding); rationale recorded there.
- **I14/I15/I11 asserts target files with no manifest entry** (major) — **confirmed**.
  Rev 3: M13 (VMEntryScope assert hunk), M6 extended to permit exactly the two
  exception-setter asserts (I15), I11's assert re-homed to owned `VMLiteInlines.h`
  enqueue/drain helpers (§6.5).
- **S1 silently depends on "rank-30 holders never park at a safepoint"** (major) —
  **confirmed** deadlock shape (holder of the structure lock blocks on a heap lock whose
  holder is parked at a safepoint). New rule **S3** states the companion invariant
  explicitly (§5.4) and is flagged for INTEGRATE coordination with the shared-heap
  workstream.
- **Locker contract drift vs SPEC-heap (incrementSTWForbiddenScope/GCDeferralContext)**
  (major) — **confirmed** against SPEC-heap §9 ("SPEC-vmstate's StructureAllocationLocker
  must call `incrementSTWForbiddenScope()/decrementSTWForbiddenScope()` and allocate under
  a `GCDeferralContext` (I14/L5)") and its Heap interface hooks. §5.2 now calls the frozen
  hook pair; deferral-mechanism convergence recorded as note N4 (§5.4).
- **M7 hunks target objectmodel-owned files being rewritten concurrently** (major) —
  **confirmed**: `runtime/Structure.h/.cpp` are in SPEC-objectmodel's exclusive owned-file
  list and no sibling spec mentions `StructureAllocationLocker`. M7 is downgraded from
  ready-to-apply hunks to an integration-phase obligation/checklist (§5.3, note N5).
- **§7 "process-wide" rank table contradicts SPEC-heap §6** (major) — **confirmed**
  (SPEC-heap §6: `JSLock` rank 1 outermost, `VMManager::m_worldLock` rank 2 UNDER it,
  structure lock rank 4 held across heap ranks 5-8, shard locks "never acquired from
  inside ranks 5-8"). §7 is demoted to "ranks for locks this spec introduces, positioned
  relative to SPEC-heap §6's master table"; the inverted JSLock/VMManager rows and the
  "process-wide / no exceptions" claim are deleted.
- **No owned test paths** (major) — **confirmed** (SPEC-api §8's narrowed ownership map
  allocates nothing to this workstream). §9 adds `JSTests/threads/vmstate/**` (collides
  with no glob in SPEC-api §8's map) and the `TestWebKitAPI` WTF test file + its CMake
  registration via M14; cross-spec note N6 asks SPEC-api's map to name the subtree.

## §7 supersession history

Rev 2 declared a "process-wide total order; one rank per lock; no exceptions" that
contradicted SPEC-heap §6's frozen table on the same locks (SPEC-heap has `JSLock` at
rank 1, "held while entering `notifyVMStop`, hence outermost", with
`VMManager::m_worldLock` at rank 2 acquired UNDER it — the opposite of rev-2's 10/20
ordering — plus three distinct nested heap locks at ranks 5-7 that a single "rank 30"
cannot express). SPEC-heap's existing errata flag against this section (filed when it
carried the rev-1 30/40 numbers) is resolved by the rev-3 demotion; SPEC-heap §6 already
carries the correct rows for this spec's locks (its rank 4 and its two leaf rows), and
they match the spec's §7 table. The rev-1 "rank 40 / rank 45" and rev-2 "process-wide
10/20/25/30/50/60/90" tables are both superseded.

## §6.4 rejected alternatives (recorded so re-review doesn't re-litigate; NOT normative)

- Embedding a `VMLite` member in `VM` and rewriting ~73 `vm.topCallFrame`-style sites
  across ~29 non-owned files (rev-1 §6.4/M9) — violates owned paths and the
  no-`.asm`-edit requirement (spec §0.4).
- `static constexpr` offset aliases — ill-formed in `offsetof` (spec §0.4).
- Anonymous-struct-in-union overlay — non-trivial members and compiler-extension/MSVC
  risk.
- Three-column access-carrying X-macro (to preserve today's mixed public/private access
  on the Group 1–3 members) — rejected: it would force the same macro to suppress
  per-field access in `VMLitePrimitives` (standard-layout requires uniform member access
  control), buying complexity for zero benefit. Making the previously-private members
  public is sound: they were already fully reachable (accessors
  `exception()`/`setException`/`lastException`, `addressOfException()`, friend classes,
  and `OBJECT_OFFSETOF`/LLInt access bypass privacy anyway), nothing semantic depends on
  their privacy, and `VM` is not an API boundary. Uniform access also removes any
  reliance on implementation-defined cross-access-specifier member ordering (pre-C++23);
  within one access run, declaration order is layout order on every ABI we ship, and the
  spec §6.4.2 equivalence static_asserts make any violation a compile error rather than a
  silent layout break.

## §4.4 withdrawn rationale

Rev-2's claim that "static strings always have a huge refcount and trivially succeed the
CAS" was false and is withdrawn: `StaticStringImpl` is constructed with
`m_refCount == s_refCountFlagIsStaticString == 0x1` (`StringImpl.h:1330-1340`), so the
masked count field is **0** at rest, and under `TSAN_ENABLED` both `ref()` and `deref()`
early-return for statics (`StringImpl.h:1191-1193, 1203-1205`) so the count never moves
at all. A count-based liveness test misclassifies every live static atom as dead. Hence
spec §4.4 step 2's static-bit-first check and invariant I19.

## Full rationale texts displaced from rev 4 (normative rules unchanged; spec sections cite this file)

### §4.4 soundness derivation (full)

The only transition to refcount 0 is a `fetch_sub` whose observer immediately and
unconditionally proceeds to `removeDeadAtom` exactly once; no path increments from 0
(`tryRefAtom` fails at 0; plain `ref()` is only legal for holders of an existing
reference, of which there are none at 0). Hence at most one destroyer per string, and a
"second thread inside the destroy path for the same string" cannot exist. A successful
`tryRefAtom` can never race a destroy of the same string either: success means the
count was nonzero at the CAS, and since 0 is final, the count had not yet reached 0 at
that point; the CAS adds a reference, so the eventual `fetch_sub`-to-zero (the unique
destroy trigger) happens only after this reference is later released — i.e. strictly
after the hit path has returned its `Ref`. UAF is impossible: `removeDeadAtom`
dereferences only a string it uniquely owns; table waiters never dereference a dead
entry beyond the in-lock `tryRefAtom`/hash reads, and the dead entry's memory is freed
only after it is unreachable from the shard (destroy happens after the conditional
removal, under no lock, by the unique owner).

Static atoms under this protocol: a static entry always hits the step-2 static-bit
branch (unconditional success via `ref()`), so it is never treated as dead, never
removed by the replace arm, and never reaches `removeDeadAtom` (its `deref()` cannot
trigger destroy — odd refcount; TSAN builds never even decrement). I1 (one atom per
character sequence) and atom pointer-equality therefore hold for static identifiers,
which are the common case for property names. Covered by I19. The tryRefAtom
static-bit branch delegates to `ref()` so the count stays balanced against the caller's
eventual `deref()` in every build (under TSAN both sides no-op; otherwise both sides
count), and is sound because statics can never be destroyed (refcount always odd,
never equal to `s_refCountIncrement` in `deref()`'s test), so "0 is final" never
applies to them.

### §5.4 N4 full justification

SPEC-heap §9/I14 words the requirement as "allocate under a `GCDeferralContext`"; this
spec's locker uses `DeferGCForAWhile` (`heap/DeferGC.h:50`). Both mechanisms make
`collectIfNecessaryOrDefer` defer rather than collect; `DeferGCForAWhile` does it
scope-wide via the heap's deferral depth, which subsumes plumbing a per-site
`GCDeferralContext` through every allocation under the lock and cannot miss a site.
The `incrementSTWForbiddenScope()/decrementSTWForbiddenScope()` calls are the part
SPEC-heap's I14 interlock actually checks, and are adopted verbatim. The integration
agent records in `INTEGRATE-heap.md`'s errata (SPEC-heap already maintains one for
this spec's §7) that `DeferGCForAWhile` is the agreed deferral mechanism; if the heap
workstream insists on `GCDeferralContext`, that is a SPEC-heap revision, not an
implementation-time fork.

### §6.4 access-specifier rationale (full)

Today's access is mixed: `private:` at VM.h:394 covers `m_exception`/`m_lastException`;
`public:` at VM.h:398 covers the call-frame pair; Group 3 sits under the `private:` at
VM.h:1167. A single expansion of the two-argument X-macro cannot emit interleaved
access labels. Making the previously-private members public is sound: they were
already fully reachable (accessors `exception()`/`setException`/`lastException`,
`addressOfException()`, friend classes, and `OBJECT_OFFSETOF`/LLInt access bypass
privacy anyway), nothing semantic depends on their privacy, and `VM` is not an API
boundary. The existing accessors stay (M6 does not remove them), so no call site
changes. A three-column access-carrying X-macro was rejected: it would force the same
macro to suppress per-field access in `VMLitePrimitives` (standard-layout requires
uniform member access control), buying complexity for zero benefit. Uniform access
also removes any reliance on implementation-defined cross-access-specifier member
ordering (pre-C++23); within one access run, declaration order is layout order on
every ABI we ship, and the §6.4.2 equivalence static_asserts make any violation a
compile error rather than a silent layout break.

### §0.3 Group-2 field inventory with line numbers (full)

Exception state: `Exception* m_exception { nullptr }` (VM.h:395), `Exception*
m_lastException { nullptr }` (397), `exceptionOffset()` (772-775),
`addressOfException()` (833), `callFrameForCatch` / `targetMachinePCForThrow` /
`targetInterpreterPCForThrow` / `targetTryDepthForThrow` /
`targetInterpreterMetadataPCForThrow` / `targetMachinePCAfterCatch` /
`newCallFrameReturnValue` / `varargsLength` (878-888), `encodedHostCallReturnValue`
(880), `osrExitIndex` / `osrExitJumpDestination` (889-890).

## Rev-3 §0 ground truth, detailed form (spec rev 4 carries the condensed index)

The rev-4 spec's §0 is a condensed fact index; this is the fuller narrative form of the
same verified facts (all line numbers checked on branch jarred/threads). Notable
details not repeated in the index:

- AtomStringTable: `StringTableImpl = UncheckedKeyHashSet<StringEntry>` with
  `StringEntry = CompactPtr<StringImpl>` or `PackedPtr<StringImpl>` (AtomStringTable.h:40).
- Locker class shape: `class AtomStringTableLocker : public Locker<Lock>` over static
  `s_stringTableLock` under USE(WEB_THREAD); plain empty class otherwise
  (AtomStringImpl.cpp:42-63).
- Static strings: `StaticStringImpl` constructed with
  `m_refCount == s_refCountFlagIsStaticString == 0x1` (StringImpl.h:1330-1340);
  `addStatic` reached via `StaticStringAtomBuffer`/`StaticStringAtomTranslator`
  (AtomStringImpl.cpp:319-344) from `add(const StaticStringImpl&)` (352-356) and
  `addSlowCase` (365-366).
- StructureMemoryManager ctor sets `g_jscConfig.startOfStructureHeap`,
  `sizeOfStructureHeap`, `structureIDBase` (SAMA:113-123); constructed via
  `LazyNeverDestroyed` from `initializeStructureAddressSpace()` (SAMA:254-257).
- mimalloc path: `structureHeap = mi_heap_new_in_arena(structureArena)` (SAMA:142);
  `tryMallocStructureBlock` returns
  `mi_heap_malloc_aligned(structureHeap, MarkedBlock::blockSize, MarkedBlock::blockSize)`
  (SAMA:161).
- Stack-limit chained offset constant:
  `const VMTrapAwareSoftStackLimitOffset = VM::m_threadContext + VMThreadContext::m_traps
  + VMTraps::m_stack + StackManager::m_trapAwareSoftStackLimit`
  (LowLevelInterpreter.asm:277-280).
- JSLock::didAcquireLock order: swap atom table (m_entryAtomStringTable =
  thread.setCurrentAtomStringTable(m_vm->atomStringTable())), then
  m_vm->setLastStackTop(thread), heap-access acquisition, then
  RELEASE_ASSERT(!stackPointerAtVMEntry) + setStackPointerAtVMEntry(currentStackPointer())
  (JSLock.cpp:115-145).
- VM scratch-buffer comment text: "only set activeLength / write entries from the main
  thread" (VM.h:893-897); members `Lock m_scratchBufferLock` /
  `Vector<ScratchBuffer*> m_scratchBuffers` (VM.h:1267-1268).
- Microtask members: `queueMicrotask` (VM.h:1006), `defaultMicrotaskQueue()` (1025),
  `drainMicrotasks()` (1028); `MicrotaskQueue` refcounted+unlocked
  (MicrotaskQueue.h:215-275).
- RegExp members: `m_regExpCache` / `BumpPointerAllocator m_regExpAllocator` /
  `ConcurrentJSLock m_regExpAllocatorLock` (VM.h:930-932), `m_executingRegExp` (891).
- VMManager: requestStopAll/requestResumeAll (VMManager.h:279-284), forEachVM (306),
  memory/JS-debugger callbacks (272-276).
- IndexingType lock bits: IndexingTypeLockIsHeld = 0x40, IndexingTypeLockHasParked =
  0x80, IndexingTypeLockAlgorithm (IndexingType.h:53,97-98,230).

## Rev 5 (size-cap compression; no normative change)

Rev 4 finished at 44,163 bytes, over the 40,000-byte cap. Rev 5 compresses the spec
under the cap. No layout, signature, invariant, lock rank, manifest entry, or task
was removed; prose was tightened and the following non-normative material moved
here verbatim. Two §0 bullets whose citations were fully duplicated inline elsewhere
(call-frame LLInt pair-op cites — now only in spec §6.3 group comments; the
"Hash/flags pointer line" with no cites of its own) were dropped or shortened.

### Moved: §4.4.5 `removeDeadAtom` reference implementation (spec now states the algorithm in prose)

```cpp
auto& shard = shardForHash(string->existingHash());
{
    Locker locker { shard.lock };
    // racing add (step 4) may have removed-and-replaced this dead entry;
    // remove only on pointer match
    auto it = shard.table.find(string);          // pointer-identity find
    if (it != shard.table.end() && *it == string)
        shard.table.remove(it);
}
StringImpl::destroy(string);
```

### Moved: §4.3 A1 explicit-table rationale

If the explicit-table overloads honored their `AtomStringTable&` argument in shared
mode, JSC `Identifier` (which passes `VM::m_atomStringTable` through
`addWithStringTableProvider`) would keep writing per-VM tables while bare WTF
atomization used the shards — two divergent atom universes, breaking I1 and atom
pointer-equality.

### Moved: §5.4 S3 deadlock derivation

Without S3: thread A holds the structure lock and blocks on a heap allocation lock;
thread B holds that heap lock and parks at a safepoint for STW; the STW initiator
waits on A (which never reaches a safepoint while blocked) — three-party cycle.
Hence S3 forbids safepoint polls/parking under any heap allocation lock (ranks 5-8).

### Moved: §3 R1 note

Options live in JSC; WTF cannot read them — which is why the latch is a WTF-side
bool set from `JSC::initialize` rather than a direct Options read in WTF.

### Moved: §8 interface summary, code-block form

```
WTF::enableSharedAtomStringTable()   // once, from JSC::initialize (M3); latches AND migrates
WTF::sharedAtomStringTableEnabled()
JSC::VMLite::current() / currentIfExists() / setCurrent(VMLite*)
JSC::currentButterflyTID()
JSC::VMLitePrimitives; OBJECT_OFFSETOF(VMLitePrimitives, field)
JSC::VMLite::offsetOfPrimitives() / offsetOfTID()
JSC::VM::mainVMLitePrimitives()
JSC::SharedVMState::singleton().structureAllocationLock()
JSC::SharedVMState::StructureAllocationLocker(VM&)
Options::useSharedAtomStringTable() / useVMLite() / useStructureAllocationLock()
```

### Moved: M7 full checklist text (spec M7 now points at §5.3/N5)

Insert `SharedVMState::StructureAllocationLocker locker { vm };` at every
ID-creating Structure-cell allocation site in the final post-objectmodel code
(`Structure.cpp`, `StructureCreateInlines.h`, `StructureTransitionTable.h`; site
classes: `Structure::create`, `createStructure`, allocating transition-table
insertions); verify I8 coverage.

### Moved: §10 TSAN flag spelling

`--useSharedAtomStringTable=1 --useVMLite=1 --useStructureAllocationLock=1`
(the spec now says "all three §3 flags on").

---

## Rev 6 — Adversarial-review round 1: resolutions (full log)

All 14 filed findings verified against the tree; every one was REAL (no
false-positive refutations needed). Dedup: 14 filings = 8 distinct issues.

### R1 (blocker): N4 `DeferGCForAWhile` races `Heap::m_deferralDepth` + frozen-vs-frozen conflict with SPEC-heap L5/I14

Verified: `Heap::incrementDeferralDepth()/decrementDeferralDepth()` are plain
non-atomic `m_deferralDepth++/--` (`heap/HeapInlines.h:166-176`; the only guard is
`ASSERT(!Thread::mayBeGCThread() || m_worldIsStopped)`), plus the racy
`m_didDeferGCWork` dance at `:178-205`. In shared-heap mode that Heap is shared by
N mutators and `DeferGC(VM&)` scopes are pervasive, so a locker-held
`DeferGCForAWhile` concurrent with any other thread's `DeferGC` is a data race that
can corrupt the deferral depth (GC permanently deferred, or under-deferred =
use-during-collection). `GCDeferralContext(VM&)` (`heap/GCDeferralContext.h:42-49`)
is stack-local and threaded into the allocator precisely to avoid the shared
counter — which is why SPEC-heap L5/I14 (`SPEC-heap.md:173, 203` ["allocations pass
GCDeferralContext"], `:298` contract note) mandates it.

Resolution (rev 6): N4 rewritten — the locker now embeds
`std::optional<GCDeferralContext> m_deferralContext` (FIRST member so its dtor runs
LAST, after the dtor body releases the lock — any deferred collection fires
strictly post-unlock), exposes `GCDeferralContext* deferralContext()`, and the
§5.3/M7 checklist requires passing it into the Structure-cell allocation. This is
SPEC-heap L5/I14 verbatim, so the frozen-vs-frozen conflict dissolves: no SPEC-heap
revision needed, heap's I14 debug interlock passes as written. The rev-5 "both
defer collectIfNecessaryOrDefer; scope-wide deferral cannot miss a site"
equivalence claim is DELETED (it was false on the only axis that matters:
m_deferralDepth atomicity). The rev-5 instruction to record an erratum in
INTEGRATE-heap.md (a SPEC-heap-owned file vmstate cannot write) is replaced by a
flagged note in INTEGRATE-vmstate.md (vmstate-owned) for the INTEGRATE agent.

### R2 (blocker): N3 mimalloc structure-block handout had no implementing owner

Verified: `grep -i 'mimalloc|SAMA|structureHeap|StructureAlignedMemoryAllocator'
SPEC-heap.md` = 0 hits; SPEC-heap's owned `heap/**` task list never touches SAMA.
Rev-5 N3 was a "verification step for a change no workstream is tasked to make" —
on `USE_MIMALLOC_DEFAULT` configs (`WebKitFeatures.cmake:83-117`), cross-thread
`mi_heap_malloc_aligned(structureHeap, ...)` (`SAMA:160-161`) would be UB once
flags turn on.

Resolution (rev 6): N3 ships as concrete manifest hunk **M9** in
INTEGRATE-vmstate.md (the shared-hot-file hunk mechanism already used by M_opts,
M3-M6, etc.). Shape: in `StructureMemoryManager`'s ctor `USE(MIMALLOC)` branch
(`SAMA:138-142`), when `Options::useStructureAllocationLock() ||
Options::useSharedGCHeap()`, skip `mi_manage_os_memory_ex`/`mi_heap_new_in_arena`
and set `m_useSystemHeap = true; m_usedBlocks.set(0);` — the whole process lifetime
then uses the already-thread-safe locked-bitvector route (`SAMA:165-190`,
`m_lock`-guarded), with no mi-heap/bitvector mixing in one VA reservation (mixing
was rejected: blocks allocated by `mi_heap_malloc_aligned` must be freed by
`mi_free`, bitvector blocks by decommit — a per-block route bit is avoidable
complexity for a rare allocation). Init-order soundness verified:
`initializeStructureAddressSpace()` runs at `InitializeThreading.cpp:112`, after
`Options::initialize` (`:90`) whose tail runs `notifyOptionsChanged`
(`Options.cpp:1111`) — so both options (and the M_opts2 implication) are readable
at SAMA-init time, before `Options::finalize`. INTEGRATE rebases M9 onto the heap
WS's final SAMA (cross-WS checklist item).

### R3 (major, filed 3x): lock-rank numbering contradicted SPEC-heap §6

Verified: SPEC-heap §6 master table (`SPEC-heap.md:152-167`): 1 JSLock, 2 GCL,
3 `VMManager::m_worldLock`, 4 GBL, 5 `Heap::m_threadLock`, 6
`HeapClientSet::m_lock`, **7a `SharedVMState::structureAllocationLock`** ("held
across Structure allocation (ranks 7-10)"), 7 MSPL, 8 `m_localAllocatorsLock`,
9 BVL, 10 cell lock. Rev-5 vmstate said rank 4 / "below JSLock(1), world(2),
HeapClientSet(3)" / "held across heap alloc locks (5-8)" — three numeric
collisions (4=GBL, 3=worldLock not HCS, alloc locks are 7-10 not 5-8). An
implementer wiring S3's "no safepoint while holding ranks 5-8" from rev-5 numbers
would have checked `m_threadLock`/`HeapClientSet` and missed BVL/cell lock.

Resolution (rev 6): every numeric rank in §5.2/§5.4/§7 now uses SPEC-heap's row
ids: lock = rank 7a, below ranks 1-6, held across allocation locks 7-10; S1/S3 say
"SPEC-heap ranks 7-10"; §7 quotes SPEC-heap's row list verbatim and states "on any
disagreement SPEC-heap §6 wins". Directional constraints were never in conflict —
only the numerals. Residual: SPEC-heap itself still cites rev-2-era vmstate line
numbers (`SPEC-vmstate.md:364-372`, `:1159-1172`, "rank-25", its §13.9 erratum) —
those lines no longer exist (file is now ~700 lines); a one-line SPEC-heap cite
refresh is flagged in INTEGRATE-vmstate.md (vmstate cannot edit SPEC-heap).

### R4 (major, filed 2x): shard selection reused HashTable's bucket bits

Verified: `WTF::HashTable` computes the initial bucket as `unsigned i = h &
sizeMask` (`HashTable.h:676, 732, 772, 876`; double-hash probing after). Rev-5's
frozen `hash & (shardCount - 1)` therefore made all keys in a shard share their low
7 bits: for any per-shard table with capacity >= 128, all initial probes land in
the coset {h0 + 128k} — 1/128 of buckets — a structural, permanent first-probe
collision defeating the contention pass's purpose.

Resolution (rev 6): frozen formula changed to
`m_shards[(hash >> (24 - shardCountLog2)) & (shardCount - 1)]` — the HIGH 7 bits of
the 24-bit StringHasher value (`existingHash()` = `m_hashAndFlags >> s_flagCount`,
the same value the HashTranslator returns, so I5 [pure function of the hash; equal
strings on one shard] is preserved) while the low bits the per-shard HashTable
consumes stay uncorrelated. One helper serves §4.3 routing, §4.4.5
`removeDeadAtom`, and §4.8 migration. The reviewer's rejected alternative
`((hash >> log2) ^ hash)` folding was indeed unacceptable (changes per-shard hash
distribution vs. the translator's); plain high-bit selection adopted. A §0 bullet
now pins the HashTable evidence so the next review round need not re-derive it.

### R5 (major, filed 2x): R3/I4 "byte-identical / same instructions" was unsatisfiable

Verified: today `setIsAtom`/`setNeverAtomize` are plain `|=`
(`StringImpl.h:361` and siblings) and both refcount ops are relaxed
(`SI.h:1196-1208`); §4.5 (compile-time `fetch_or`) and F3 (release deref + acquire
fence + new zero-path branch) are deliberately NOT flag-gated; on x86-64 `fetch_or`
is `lock or`, on arm64 release `fetch_sub` is `ldaddl` not `ldadd`. So rev-5's
"byte-identical, same instructions (modulo §4.5's type change)" was impossible to
satisfy alongside §4.5/F3, and the bench-gate invariant was defined against the
impossible wording.

Resolution (rev 6): option (a) — honest carve-out. R3 now enumerates the EXHAUSTIVE
flag-off codegen deltas: (a) §4.5 atomic type + RMW flag writes (x86-64 `lock or`),
(b) F3 deref ordering + acquire fence + zero-path flag branch (arm64
`ldadd`→`ldaddl`, `dmb ishld` on the rare zero path), (c) one predictable
latched-flag branch per routed entry point / locker. The gate invariant is
explicitly "perf within bench-gate noise", not instruction identity; I4 references
"the R3(a)-(c) deltas". Option (b) (flag-gating plain stores / relaxed deref in
legacy mode) was REJECTED: it would put a data-dependent branch inside `deref()`'s
hot path on every build and double the code shapes to test, to recover ~nothing
(`lock or` on a setIsAtom slow path and `ldaddl` are within bench noise; the gate
verifies exactly that). These ARM64/x86 expectations give the bench gate its
correct baseline per the reviewer's ask.

### R6 (major): `VMLite::Registry` was undefined (no layout, no API, impossible nesting)

Verified: rev 5 referenced `Lock VMLite::Registry::lock` "(in VMLiteShared.h)" —
but a nested class of VMLite cannot be declared from VMLiteShared.h without
amending the frozen §6.3 class body, and no registration signatures/lifetime rules
existed; M6/M11/M12 all depended on the phantom.

Resolution (rev 6): §6.5.1 defines a STANDALONE `struct VMLiteRegistry` (declared
`VMLiteShared.h`, defined `VMLiteShared.cpp` — NOT nested; the frozen §6.3 VMLite
body is untouched): `singleton()` (NeverDestroyed), `Lock lock` (leaf rank, §7),
`Vector<VMLite*> lites WTF_GUARDED_BY_LOCK(lock)` (fastMalloc only — no intrusive
DoublyLinkedList node, which would have required amending the frozen VMLite layout;
a Vector is fine at these cardinalities and keeps the leaf-lock I7-style "fastMalloc
only" discipline), `registerLite(VMLite&)` / `unregisterLite(VMLite&)` (frozen
signatures, take the lock, assert absent/present). Lifetime rules: unregister
before VMLite destruction and before teardown `setCurrent(nullptr)`; a VM must not
die while a registered lite's `vm` points at it (`~VM` debug-assert under the lock,
M6). One lock guards both the lite list and `VM::m_microtaskQueues`
mutation/traversal (M11/M12) — GC marker threads only iterate, holding no other
lock. Added to §8's consumable set; §6.5/§7/M6/M11 references renamed.

### R7 (major): §4.8 migration left non-calling pre-latch threads as silent corruption

Verified: `AtomStringTable::~AtomStringTable` (`AST.cpp:28-37`) calls
`setIsAtom(false)` on every non-static entry. If a pre-latch thread (other than the
JSC::initialize caller) had atomized anything: (a) its atoms are invisible to
shards → post-latch duplicates break I1/I2 silently; (b) worse, at that thread's
death the dtor strips `isAtom` from strings that may also be shard-resident
(duplicated by content) or pointer-shared, after which deref-to-zero takes the
"!isAtom() ⇒ destroy as today" path, bypassing `removeDeadAtom` → dangling shard
entry → UAF on next lookup. Rev-5's I17 was only a debug-assert at thread death.

Resolution (rev 6): failure-stop in an owned file — in shared mode the AST dtor
`RELEASE_ASSERT(m_table.isEmpty())` and SKIPS the `setIsAtom(false)` loop (frozen,
I17). The §4.8 contract is widened explicitly to bind WTF/JSC-internal service
threads, not just "the embedder" (GC/JIT/sampler threads do not atomize; an
internal thread that does is a contract violation). A breach now fails
deterministically at that thread's death instead of as a UAF. The reviewer's
companion ask — RELEASE_ASSERT(!shared) on "legacy-table entry points" — is
subsumed: under A1 the legacy body is the else-branch of the same routed function
and is unreachable in shared mode by construction; the only breach vector is
pre-latch residue, which the dtor assert catches.

### R8 (major): R2 (`useJSThreads` ⇒ all three flags) had no provider

Verified: SPEC-api.md contains zero mentions of
useSharedAtomStringTable/useVMLite/useStructureAllocationLock; its §9.2-1 OptionsList
manifest entry covers only its own four options; no spec owned a
dependent-options hunk. With `useJSThreads=1` alone, atom tables would stay
per-thread post-GIL (unsound) and the api test corpus
(`//@ requireOptions("--useJSThreads=1")`) would never exercise vmstate features
under TSAN.

Resolution (rev 6): **M_opts2** manifest hunk — in `Options::notifyOptionsChanged()`
(`Options.cpp:762`, this tree's dependent-options hook; runs at the tail of
`Options::initialize`, `Options.cpp:1109-1111`, hence before `JSC::initialize`
consumers and before SAMA init reads the flags for M9): `if (useJSThreads()) {
useSharedAtomStringTable() = true; useVMLite() = true;
useStructureAllocationLock() = true; }`. R2 names M_opts2 as sole provider so the
next review round finds the implementation site immediately.

### R9 (major): W2 could not compile standalone (`incrementSTWForbiddenScope` does not exist)

Verified: `grep -r incrementSTWForbiddenScope Source/` = 0 hits; the functions are
SPEC-heap §9 frozen signatures (`SPEC-heap.md:246-247`) to be written by the
concurrent heap agent. Rev-5's task 10 (standalone build self-check) was impossible.

Resolution (rev 6): **N7** compile shim — `VMLiteShared.cpp` calls the two hooks
only under `#if defined(JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE)` (no-op otherwise);
INTEGRATE defines the macro once the heap WS lands (cross-WS checklist item). A
macro guard was chosen over weak symbols (not portable to MSVC, which I16 requires)
and over a free-function indirection (an extra call in a frozen interface for no
gain). Task 10's build check is explicitly valid shim-inactive.

### Size-cap note

Rev 6's new normative material (VMLiteRegistry, M9, M_opts2, N7, deferralContext,
R3 delta list, I17 enforcement) added ~5KB; the cap was held by compressing
non-normative prose throughout (this file carries the displaced rationale). No
layouts, signatures, numbered invariants, lock orders, manifest entries, or task
steps were dropped; §1's VMManager observation and §0's evidence bullets were
tightened, never removed.

## Rev 7 — adversarial-review round 2 (vs rev 6) resolutions

All eight blocker/major findings were verified against the tree and accepted as
real; none was a false positive, so rev 7 carries no refutation notes — every
finding produced a spec revision. Full dispositions:

### 7.1 (major) R3 "exhaustive" flag-off delta list vs M6's unconditional reordering

Accepted. Rev 6's R3 claimed "exhaustive; else byte-identical" while §6.4(1)
unconditionally (compile-time, not flag-gated) reorders VM's Group 1-3 members
into the X-macro block, replacing scattered declarations at VM.h:395, 397,
405-406, 878-890, 1237-1240 (verified: those fields sit in four separate regions
of /root/WebKit/Source/JavaScriptCore/runtime/VM.h). Reordering changes
OBJECT_OFFSETOF results, hence offset immediates across the interpreter/JIT —
byte-identical codegen is unattainable. Additionally `VMLITE_DECLARE_FIELD`'s
`type name { };` brace-inits fields that today are declared uninitialized
(newCallFrameReturnValue, targetMachinePCForThrow, targetInterpreterPCForThrow,
targetInterpreterMetadataPCForThrow, targetTryDepthForThrow, varargsLength,
osrExitIndex, osrExitJumpDestination; VM.h:878-890). Resolution: R3 restated —
gate invariant is behavior identity + in-noise perf, NOT instruction identity;
delta (d) added covering both the reordering and the brace-init; I13 restated as
"behavior identical + bench-gate in-noise (NOT codegen-identical — R3(d))"; I4
now cites R3(a)-(d). The (a)/(b) ISA examples (x86-64 `lock or`; arm64
`ldadd`→`ldaddl`, `dmb ishld`) were dropped from the spec as non-normative color.

### 7.2 (blocker) Unmanifested RELEASE_ASSERT crashes under the shared atom table

Accepted; resolved by reversing M4 rather than relaxing asserts (reviewer option
(a)). Verified sites asserting
`vm.atomStringTable() == Thread::currentSingleton().atomStringTable()`:
Heap.cpp:2348 (Heap::requestCollection — every mutator GC request),
Completion.cpp:63, 77, 85, 100, 119, 139, 192, 220, 251, 262, 277, 287 (13
RELEASE_ASSERTs), Identifier.cpp:77 (ASSERT). Rev 6's M4 (skip the
setCurrentAtomStringTable swap in shared mode) would leave thread-current ≠ vm
table on any spawned GIL'd thread, crashing Heap.cpp:2348 on its first GC
request — the primary Phase-A use case. Rev 7: the swap is KEPT in shared mode —
harmless under rule A1 (no atomization/lookup/removal path consults any
AtomStringTable instance), preserves all 14 asserts unchanged, and removes any
need to touch Heap.cpp/Completion.cpp (outside this WS's ownership; Heap.cpp is
SPEC-heap territory). M5 (Identifier.cpp:77 relaxation) is deleted as no longer
needed. M4's hunk is now solely the §6.4.4 VMLite install/restore. Soundness of
keeping the swap: under the GIL exactly one thread holds a VM's JSLock; the swap
is the same outermost-acquire/release save/restore discipline as today
(JSLock.cpp:124, 326-328; JSLock.h:162 m_entryAtomStringTable); per-thread
tables stay empty post-migration (I17) because A1 means nothing ever inserts
into them. SPEC-api.md:12 ("M4 skips the JSLock swap") flagged stale in the
cross-WS checklist.

### 7.3 (major) static_assert(is_standard_layout_v<VMLitePrimitives>) vs MSVC Variant

Accepted. Group 2 carries `JSOrWasmInstruction targetInterpreterPCForThrow`;
JSOrWasmInstruction = `Variant<const JSInstruction*, uintptr_t>`
(Source/JavaScriptCore/interpreter/Interpreter.h:61). std::variant is not
guaranteed standard-layout, and MSVC's STL spreads members across the
_Variant_base/_Variant_storage hierarchy, disqualifying it — the frozen assert
would fail on the Windows CI toolchain I16 requires. Resolution: the
standard-layout assert is REMOVED (spec comment records why);
is_trivially_copyable retained (variant of trivially-copyable alternatives is
trivially copyable per [variant.variant]); the real layout contract is the
§6.4(2) per-field OBJECT_OFFSETOF equivalence asserts, valid for
non-standard-layout types via __builtin_offsetof (StdLibExtras.h:79-91). L3
updated accordingly. Include note added: VMLite.h includes Interpreter.h, which
VM.h already includes (VM.h:41), so M6 introduces no new include cycle.

### 7.4 (major) Main-thread VMLite ownership/installation unspecified

Accepted. Rev 6 said per-thread lites are "registered/TLS-installed" and M6
should register "the main carrier" without saying who allocates it, when
setCurrent runs, what happens with two VMs on one thread, or with cross-thread
VM movement (Bun moves VMs across threads), making I14/I18 untestable and
leaving the M6 implementer a design decision. Resolution: new frozen §6.4.4 —
VM owns `std::unique_ptr<VMLite> m_mainVMLite` (tid 0), created at the end of
the VM ctor under useVMLite, registered via registerLite(*lite, *this); ctor
never calls setCurrent; torn down at the top of ~VM (assert-then-unregister-
then-destroy) before member teardown. Installation rides the JSLock
outermost-acquire/release hooks (the M4 hunk), mirroring m_entryAtomStringTable:
didAcquireLock installs the carrier iff no lite is installed or the installed
lite is another VM's tid-0 carrier (covers multi-VM-per-thread and cross-thread
movement, with stack-discipline restore via m_entryVMLite/m_didInstallVMLite);
a spawned-thread lite (tid != 0) is never displaced and must satisfy
cur->vm == m_vm (SPEC-api §5.2 installs it before JSLockHolder). Phase B
explicitly decides carrier-vs-aliased-view for the main thread.

### 7.5 (blocker) ~VM microtask force-removal loop outside the registry lock

Accepted. Verified third mutation site: VM.cpp:635-636
`while (!m_microtaskQueues.isEmpty()) m_microtaskQueues.begin()->remove();` —
direct SentinelLinkedList::remove(), not via ~MicrotaskQueue (whose removal,
MicrotaskQueue.cpp:114-118, only covers list-resident queues at their own
destruction; queues are refcounted and outlive the loop). Unlocked, it races
concurrent marker traversal (visitAggregateImpl runs while mutators run) —
exactly the race §6.5 exists to close. Resolution: §6.5's mutation-site
enumeration restated as three sites — (a) ctor append, (b) dtor removal (M12),
(c) the ~VM loop — and M11 (the VM.cpp hunk owner) extended to wrap 635-636 in
VMLiteRegistry::singleton().lock.

### 7.6 (major) VMLite::vm never assigned — provider/consumer gap with SPEC-api §5.2

Accepted. Rev 6 declared `VM* vm` "set at registration" but registerLite(VMLite&)
took no VM; SPEC-api §5.2 (SPEC-api.md:144) defers the vm pointer to "6.5.1" —
circular. Unassigned, M13's I14 assert fails on every spawned-thread VM entry.
Resolution: §6.5.1 signature is now `registerLite(VMLite&, VM&)` — stores
lite.vm under the lock, asserts it was null, and is the sole writer of
VMLite::vm; §6.3's field comment points at it; §8 lists the two-arg form;
cross-WS checklist flags SPEC-api's §5.2 cite refresh
(`registerLite(*lite, vm)`).

### 7.7 (major) M7 double-insertion hazard vs SPEC-objectmodel's adopted SAL

Accepted. SPEC-objectmodel (pinning "vmstate r6") already adopted the
obligation: SPEC-objectmodel.md:213 — "SAL = SharedVMState::
StructureAllocationLocker, heap rank 7a (vmstate §5.3/N5 adopted): held across
every ID-creating Structure::create / allocating transition-table insertion in
owned Structure.cpp/StructureCreateInlines.h/StructureTransitionTable.h". Rev
6's M7 verb ("the INTEGRATE agent inserts ... at every site") would therefore
emit a second, nested locker — self-deadlock, since §5.2 freezes the lock as
non-recursive WTF::Lock. Resolution: §5.3/N5 rewritten — objectmodel owns
EMITTING the lockers in its files; M7 is a VERIFICATION checklist: audit every
ID-creating site, add a locker only where absent, never nest one in a scope
already holding it.

### 7.8 (major) JSTests/threads/vmstate/** picked up by no runner

Accepted. SPEC-api's Tools/threads/run-tests.sh — the sole runner until its
9.2-7 yaml stanza lands — globs `JT/{api,atomics,races}/*.js + threads/heap-*.js
+ threads/objectmodel/*.js` (SPEC-api.md §8); `threads/vmstate/**` is absent,
and the runner file is api-owned, so this WS cannot add the glob itself. The
I8/I9/I11/I13/I14 stress evidence would be written but never executed.
Resolution: N6 extended (spec §9 Tests paragraph + cross-WS checklist) — the
INTEGRATE agent must add `threads/vmstate/*.js` to run-tests.sh and the yaml
stanza, and SPEC-api is requested to state the glob in its next revision.

### Size-cap note (rev 7)

Rev 7's additions (§6.4.4, three-site §6.5 enumeration, two-arg registerLite,
R3(d), M4/M5/M7 rewrites, N6 runner item, MSVC note) totaled ~4KB; the 40KB cap
was held by compressing non-normative prose across §0, §1, §4, §5, §7, §8, §10,
§11 and the manifest, and by moving the full dispositions above into this file.
No layouts, signatures, numbered invariants, lock orders, manifest entries, or
task steps were dropped.

---

# Round-3 adversarial review — dispositions (rev 7 → rev 8)

Eight findings (1 blocker, 7 major); several were duplicates of the same two
defects. Verified against the tree and sibling specs on branch jarred/threads.

## Finding A (blocker + dup): §6.4.4 install/restore vs SPEC-api §5.2 spawn order

**Claim.** Rev 7's §6.4.4 justified its `!cur || cur->tid == 0` install condition
with "(SPEC-api §5.2: one VM per spawned thread, lite installed
pre-JSLockHolder)". Both halves false: SPEC-api rev 8 §5.2 is a single shared VM
(GIL = its JSLock), and docs/threads/SPEC-api.md:146 orders the spawn handshake
"Spawn (u/JSL, before fn): lite=makeUnique<VMLite>() -> lite->tid=ts->tid ->
registerLite(*lite) -> setCurrent(lite.get())" — i.e. the lite is installed
UNDER the JSLock, AFTER `didAcquireLock` ran. Implemented verbatim, rev 7
produced: first acquire installs the main carrier (tid 0); handshake swaps in
the spawned lite; first DropAllLocks-driven `willReleaseLock` unconditionally
restored `m_entryVMLite` (nullptr), discarding the spawned lite; every reacquire
reinstalled the tid-0 main carrier. Two threads then claim TID 0 — unsound TTL
inference once objectmodel tagging consumes §6.7.

**Disposition: REAL — fixed in rev 8** via the reviewer's option (b), which
requires NO change to SPEC-api's spawn ordering:

- `didAcquireLock` installs iff `!cur || cur->vm != m_vm` (was `tid == 0`). Any
  lite of THIS VM already in TLS — main carrier or spawned, either install
  order — is never displaced. `cur->vm != m_vm` still covers
  multi-VM-per-thread nesting (another VM's carrier saved as entry).
- `willReleaseLock` restores ONLY IF `currentIfExists() == m_mainVMLite.get()`;
  a lite swapped in after install is never clobbered; bookkeeping
  (`m_didInstallVMLite`/`m_entryVMLite`) always cleared.
- State walk-through under api §5.2 as written:
  - Spawned thread, acquire #1: cur == nullptr ⇒ install main carrier
    (flag set, entry = nullptr). Handshake: setCurrent(lite). The window between
    install and handshake runs no JS (api CS3 "before any JS" is already
    MANDATORY per SPEC-jit CS3), so the transient tid-0 view is unobservable;
    the GIL guarantees at most one thread has the main carrier installed at any
    instant (install only under the lock; conditional restore on release).
  - Any DropAllLocks cycle: release ⇒ current == lite ≠ main carrier ⇒ no
    restore, flag cleared; reacquire ⇒ cur->vm == m_vm ⇒ no install. Lite
    survives every blocking primitive.
  - Completion (api §5.2): unregisterLite → setCurrent(nullptr) → destroy lite,
    then final release: flag already false ⇒ no-op; TLS stays nullptr.
  - Main thread: install on acquire, current == main carrier at release ⇒
    restore entry. Identical to rev 7 behavior.
- The false parenthetical was deleted; §6.4.4 now states the api §5.2 reality
  (install-after-acquire) and sanctions it explicitly. §9 cross-WS notes that
  api spawn ORDER needs no change.

## Finding B (3 duplicate majors): ~VM destroys m_mainVMLite while TLS-current

**Claim.** Rev 7 froze "torn down at the TOP of ~VM ... before any member
teardown", but ~VM runs holding the API lock (VM.cpp:630 asserts
currentThreadIsHoldingAPILock; reviewer line cites of 1875/1876 are stale but
the substance is right) and the main carrier is typically the installed TLS
lite. Destroying it at the top of ~VM leaves `t_currentVMLite` dangling through
heap.lastChanceToFinalize() (VM.cpp:633), the m_microtaskQueues force-removal
loop (VM.cpp:635-636), and JSRunLoopTimer unregistration — until the eventual
unlock's willReleaseLock, which would then also restore from a destroyed-lite
comparison. Latent UAF for any VMLite::currentIfExists()/currentButterflyTID()
call in finalizers/sweeps once sibling flags are on.

**Disposition: REAL — fixed in rev 8.** M4 adds
`JSLock::uninstallVMLiteForVMDestruction()`: if `m_didInstallVMLite` — if
`currentIfExists() == m_vm->m_mainVMLite.get()`, `setCurrent(m_entryVMLite)`;
clear both members. ~VM (M6) calls it at the TOP, BEFORE the §6.5.1
no-other-lite assert / unregister / destroy. The restore target is
`m_entryVMLite` (not bare nullptr) so multi-VM nesting restores the outer VM's
carrier. New invariant **I20**: no thread's TLS ever points at a destroyed
VMLite; debug enforcement = poison destroyed lites + assert
registered-at-restore/setCurrent. (JSLock::willDestroyVM at JSLock.cpp:76 /
VM.cpp:631 was considered as the hook, but it runs AFTER the spec's top-of-~VM
lite teardown point, so a dedicated method called first is required.)

## Finding C (major): ~StringImpl symbol arm / SymbolRegistry unaddressed

**Claim.** StringImpl::~StringImpl's symbol arm (StringImpl.cpp:132-137 in
tree; reviewer's 131-136 is close enough) calls symbolRegistry->remove() on an unlocked
per-VM WTF::SymbolRegistry; shared-mode deref-to-zero on a foreign thread would
race it the same way §4.4 fixes atoms; post-GIL Symbol.for identity owned by no
spec.

**Disposition: PARTLY FALSE-POSITIVE; spec now says so explicitly (Dev 8 + §2).**
The race premise does not hold for symbols the way it does for atoms:
`SymbolRegistry::m_table` is `UncheckedKeyHashSet<RefPtr<StringImpl>>` and
`symbolForKey` stores the symbol itself strongly (`*addResult.iterator =
symbol;`, SymbolRegistry.cpp:58). A registered symbol therefore can NEVER hit
refcount 0 while table-resident — there is no destroy-vs-table race shape at
all. The only teardown interleaving is ~SymbolRegistry (per-VM, in ~VM), which
FIRST calls clearSymbolRegistry() on every entry (SymbolRegistry.cpp:40-43) and
THEN drops the strong refs; a dying symbol's dtor on any thread sees a null
registry pointer and skips remove(). Cross-thread visibility of that clear is
given by the very F3 deref upgrade (release on deref, acquire fence on zero)
this spec already mandates — the clear happens-before the ref-drop on the
destroying thread, and F3 orders the foreign zero-observation after it.
`symbolForKey` itself runs only from JS execution paths (per-VM
`VM::m_symbolRegistry`, VM.h:624) under the GIL this round. What IS true: post-GIL
`Symbol.for`/registry locking is future work owned by nobody — now listed in §2
non-goals as UNOWNED with Dev 8 as the pointer, mirroring how Phase B is marked.

## Finding D (major): registerLite signature drift (vmstate two-arg vs api one-arg)

**Disposition: REAL — escalated.** SPEC-api.md:146 ships `registerLite(*lite)`
in its frozen 9.2-8 hunk while §6.5.1 freezes `registerLite(VMLite&, VM&)` as
the SOLE writer of `VMLite::vm`. Rev 7 buried this in the §9 cross-WS checklist
(read at INTEGRATE — too late, as the reviewer notes, since the five
implementation agents run concurrently). Rev 8 promotes it to **Dev 9, marked
BLOCKING**: the orchestrator must patch api 9.2-8 to `registerLite(*lite, vm)`
BEFORE implementation unfreezes. vmstate does not add a one-arg overload: a
second vm-setting channel would break the single-writer invariant that makes
`VMLite::vm` reads sync-free. The §9 cross-WS entry remains as the INTEGRATE
backstop.

## Finding E (major): Phase B has no owner (SPEC-jit scopes VMLite out)

**Disposition: REAL — declared.** SPEC-jit.md:24 lists `VMLite` under "Out
(consumed via §9)" and its task list has no Phase-B items, while rev 7's
§6.1/§6.5/§6.6/§6.8 said "Phase B (JIT WS)". Rev 8 adds **Dev 10**: Phase B is
UNOWNED this round; every "Phase B" reference is a frozen contract
(VMLitePrimitives ABI, Group-5 reservation, §6.8 chained-offset mechanism) for a
FUTURE chartered workstream, not a committed sibling; the GIL milestone does not
depend on it. All in-body "(JIT WS)" labels were rewritten to "(Dev 10)". The
frozen layout is kept frozen precisely so the future WS can consume it without
a vmstate re-freeze; per-thread-state remains THREAD.md's stated end state
(Dev 7).

## Size accounting

Rev 8 additions (§6.4.4 rewrite incl. uninstall API and api-order
sanctioning, Devs 8-10, I20, §2 non-goal, M4/M6/cross-WS refresh) ≈ +2.9KB;
held under the 40KB cap by compressing non-normative prose across the header,
§0-§11 and the manifest (wording only — no layout, signature, numbered
invariant, lock-order row, manifest entry, or task step dropped). Final size:
39999 bytes.

# Round-4 adversarial review — resolutions (rev 8 → rev 9)

Nine blocker/major findings; all verified against the tree and the frozen
sibling specs. Six REAL (spec revised), two were the SAME finding filed twice
(spawn order; also a third duplicate pair on Dev-9 staleness), one
REAL-by-design (Phase B gap — clarified, not changed). No false positives
requiring refutation-only notes; every finding produced a spec edit.

## F1 — M10/§4.3 self-contradiction ("never consulted")

**Disposition: REAL — M10 removed, §4.3 reworded.** Verified:
`JSLock::didAcquireLock` executes
`m_entryAtomStringTable = thread.setCurrentAtomStringTable(m_vm->atomStringTable())`
(Source/JavaScriptCore/runtime/JSLock.cpp:124), i.e. it reads
`VM::m_atomStringTable` on every outermost acquire; the 14 kept assert sites
(Identifier.cpp:77, Completion.cpp ×13, Heap.cpp:2348) also call
`vm.atomStringTable()` for pointer comparison. Any accessor-level "never
consulted in shared mode" assert (rev-8 M10, VM.cpp:263 region) fires on the
first shared-mode lock acquire under the §10 matrix (all flags on). M10 was a
leftover from the rev-6 "skip the swap" design that rev 7 reversed. Rev 9:
(a) §4.3 now distinguishes POINTER reads (swap + asserts — allowed) from
atomization/lookup/removal USE (banned by A1); (b) M10 deleted from the
manifest; the drift guard moves in-WS as reviewer suggested — debug
`ASSERT(!sharedAtomStringTableEnabled())` atop each legacy locker-site body in
AtomStringImpl.cpp (vmstate-owned, no shared-file hunk). These arms are
unreachable via the routed entries, so the assert is a pure guard against
future paths that bypass A1 routing.

## F2/F5 (duplicate pair) — Dev 9 / cross-WS stale vs SPEC-api rev 9

**Disposition: REAL — staleness on OUR side; all three items converted to
verify-only.** Verified in docs/threads/SPEC-api.md (rev 9): line 148 already
has two-arg `VMLiteRegistry::singleton().registerLite(*lite, vm)`; line 12
already reads "M4 KEEPS the swap - only change=§6.4.4 install/restore"; line
383 already globs `threads/{objectmodel,vmstate}/*.js` (plus 9.2-7 yaml at
api:423). Rev 8's Dev 9 "BLOCKING pre-unfreeze patch" and the two §9 cross-WS
bullets described api rev 8. Rev 9: Dev 9 rewritten as RESOLVED-in-api-rev-9
with the three file:line anchors; cross-WS list now carries them as
verification items only — the unfreeze precondition list contains no phantom
patches.

## F4/F7 (duplicate pair) — §6.4.4 spawn-order narrative vs api rev 9 §5.2

**Disposition: REAL — narrative rewritten to api's actual order.** api:148
freezes: spawn does registerLite → setCurrent(lite) →
initializeButterflyTIDTagForCurrentThread, ALL BEFORE the first JSLockHolder.
Under that order the spawned thread's first `didAcquireLock` sees
`cur->vm == m_vm` and installs nothing; the main carrier is never installed on
spawned threads, `m_didInstallVMLite` stays false, and rev 8's
"install-after-acquire ⇒ transient tid-0 window, unobservable" soundness
paragraph described a sequence that never occurs. The M4 logic itself is
order-tolerant (as the reviewers noted), so no code-shape change; rev 9
replaces the bullet with the real sequence and recasts the install arm as the
no-lite/foreign-lite (main, embedder, multi-VM) path, and fixes the §9
cross-WS wording ("spawn ORDER unchanged (works install-after-acquire)" —
deleted). Tests/asserts must NOT expect a main-carrier install on spawned
threads.

## F3 — X-macro Group 3 omits m_currentSoftReservedZoneSize; block placement unspecified

**Disposition: REAL — relocation list + placement added.** Verified VM.h:1237-1240:
`void* m_stackPointerAtVMEntry; size_t m_currentSoftReservedZoneSize;
void* m_stackLimit; void* m_lastStackTop;` — the size_t is interleaved inside
the cited Group-3 range, appears in neither the X-macro nor rev 8's relocation
list, and the §6.4(2) span assert (m_lastStackTop delta + sizeof(void*) ==
sizeof(VMLitePrimitives)) forces it out of the block. Rev 9 adds it to the
"Deliberately NOT in VMLitePrimitives / M6 relocates" list (so M6's "ONLY
changes beyond these" now authorizes the move; M6 manifest entry now cites the
§6.3 relocations) and freezes placement: the X-macro expansion replaces the
VM.h:395-406 region (top of VM, preserving the :392 "Keep super frequently
accessed fields top" intent); Group-2 (878-890) and Group-3 (1237-1240)
members move up into it; relocated members keep their original declaration
sites. Hot-layout consequences remain R3(d) bench-gated.

## F6 — Phase B unowned (program-level gap)

**Disposition: REAL-BY-DESIGN — clarified, no scope change.** Rev 8's Dev 10
already declared Phase B UNOWNED; the reviewer's residual ask was orchestrator
confirmation that THREAD.md's "shared VM state" GIL-removal step deliberately
has no implementing workstream among the five. Rev 9 appends to Dev 10: the
gap is deliberate and INTEGRATE records orchestrator sign-off / a follow-on
charter decision. Spec scope, layouts and the GIL milestone are unchanged.

## F8 — VM destruction races a finishing JSThread's lite teardown

**Disposition: REAL — new N8 (cross-WS BLOCKING), provider named.** Verified
api 4.6.1/5.2: completion publishes the result and wakes joiners UNDER the
final JSL, releases the JSL, and only then runs unregisterLite →
setCurrent(nullptr) → destroy lite → release TID. A joiner that proceeds from
join() to VM teardown (jsc shell exit follows the "join every spawned thr"
convention) can execute ~VM while the dying thread sits between JSL release
and unregisterLite — intermittently firing vmstate's §6.4.4 no-other-lite
assert and leaving a registered lite whose `vm` dangles. Neither spec
synchronized this; api's TM never joins native threads at teardown (its Strong
table only covers handles). Rev 9 adds N8 next to the §6.5.1 lifetime rule
(the assert's provider citation) and to the cross-WS BLOCKING list: api must
either move unregisterLite + setCurrent(nullptr) BEFORE the final JSL release
(legal — the registry lock is a leaf, JSL-independent; on a spawned thread
willReleaseLock's conditional restore is a no-op since the installed lite is
not the main carrier, so order is safe) or make TM await native-teardown
completion before the shared VM may die. Choice is api's; vmstate's assert
stays as the enforcement point.

## F9 — Butterfly TID tag not maintained across install/restore (jit CS3/I19)

**Disposition: REAL — option (a) chosen: registered hook.** jit CS3
(SPEC-jit.md:263) requires `VMLite::setCurrent` to keep the tag current; jit
I19 (SPEC-jit.md:204) RELEASE_ASSERTs at VM entry that g_jscButterflyTIDTag ==
uint64_t(currentButterflyTID()) << 48. Rev 8's setCurrent was a bare TLS
write, and §6.4.4's install/restore (multi-VM-per-thread) switches the
installed lite without a tag update — stale tag ⇒ I19 fires or butterflies get
stamped with a wrong owner TID. Forbidding multi-VM installs (option b) would
contradict the M4 design and still leave embedder lazy-entry uncovered. Rev 9
freezes the hook: `setVMLiteTIDTagHook(void(*)(uint16_t))` exported from
VMLite.cpp (null default); setCurrent invokes it after the TLS write with
`lite ? lite->tid : 0` (including uninstalls). jit task 1b registers the P5
tag-update body, giving tag coherence on every lite switch with no
runtime/→jit/ include and no behavior when jit hasn't landed (Phase-A
standalone builds and flag-off runs see a null hook). api §5.2's explicit
P5/clear calls remain and are idempotent with the hook. Cross-WS list carries
the registration obligation.

## Also fixed while editing (round-4 sweep)

- §6.7 recycling bullet said "api §5.1 recycles at join-completion" — STALE vs
  api Dev 10/5.1 ("dying thr returns TID to m_freeTIDs as LAST teardown step;
  join compl never releases TIDs"). Corrected to the dying-thread-last-step
  rule; the safety argument (release after setCurrent(nullptr)) is unchanged
  and now matches api verbatim.
- §11 task 8's manifest enumeration dropped M10 (now "M8-M9/M11-M14").

## Size accounting

Rev 9 additions (Group-3 relocation + placement, N8, TID-tag hook, §6.4.4
rewrite, Dev 9/10 updates, ex-M10) ≈ +2.4KB gross; held under the 40KB cap by
compressing non-normative prose only (header, Devs 2/8, §4.2-§4.8 narration,
§5.3/§5.4 S3/N4, §6.5-§6.8, §7 note, §8 summary, §9 intro, I4/I6/I11/I15/I18
phrasing; moved the HashTable LOW-bits cite from §0 into the §4.2 comment that
consumes it). No layout, signature, numbered invariant, lock-order row,
manifest entry, or task step was dropped; M5/M10 remain as tombstones so
reviewers don't re-flag their absence. Final size: 39987 bytes.

## Rev 10 — whole-design adversarial review round 1 dispositions

1. BLOCKER N8 (x2: cross-cutting + api): CLOSED. api rev 11 adopts the fix this spec demanded: §4.6.1/§5.2 now run unregisterLite + setCurrent(nullptr) + butterfly-tag clear UNDER the final JSLock hold, before release (the registry lock is leaf rank, so taking it under JSL is legal per api §5.9); lite destruction and TID retirement happen after release. §6.5.1's text rewritten from "N8 cross-WS BLOCKING" to "N8 RESOLVED"; the ~VM no-other-registered-lite assert (§6.4.4/I20) is now satisfiable: a joiner observing completion can only reach ~VM after the dying thread has unregistered, because publication-to-joiners and unregistration happen under the same JSL critical section. Cross-WS manifest line updated to verify-only.

2. BLOCKER/MAJOR TID recycling (three-way inconsistency): CLOSED in objectmodel's direction. §6.7's "GC safepoint after rebias" clause cited a mechanism no spec implements (rebias is unowned, Dev 10 pattern); api's GIL-phase teardown reissue would have been live precisely when OM tagging is compiled+flag-on (same master flag). New rule everywhere: TIDs are NEVER reissued this milestone — api r11 Dev 10 (m_freeTIDs dead; m_nextTID exhaustion => RangeError at spawn), OM ledger 8c, this spec §6.7. N2 is closed by non-reuse: "a TID MUST NOT be recycled while any installed VMLite carries it" holds vacuously.

3. MAJOR ("unconditional StringImpl deref/hash atomicization taxes flag-off; vmstate unilaterally weakens the flag-off gate"): PARTIALLY ACCEPTED. (a) ADOPTED — F3/§4.4.3 deref ordering upgrade is now GATED on the latched sharedAtomStringTableEnabled(): legacy mode keeps today's relaxed fetch_sub path verbatim; only shared mode pays release + acquire-fence-on-zero. The latch is immutable after JSC::initialize and pre-dates any second atomizing thread (§4.8 ordering contract), so no pre-latch string can be cross-thread-deref'd in shared mode with relaxed ordering. R3(b) shrinks to "one latched-flag branch in deref". The §4.5 m_hashAndFlags atomicization stays compile-time: relaxed loads/stores and fetch_or compile to the same instructions as plain accesses on x86-64/arm64; the only behavior-relevant delta is the fetch_or on possibly-published strings, which is required for flag-correctness under WEB_THREAD-style embedders too and is bench-gated (R3(a)). (b) ADOPTED — composed flag-off bar: R3 now defines the ONE cross-spec gate (bench-noise + golden disasm modulo each spec's LISTED deltas: ours (a)-(d), jit D7 repacks, api I1 scoped to api-owned files); api I1 reworded to match. The reviewer is right that literal "byte-identical binary" was unsatisfiable once any sibling's unconditional repack lands; the per-spec lists make the gate mechanical instead of vacuous. (c) ADOPTED — §10 bench gate gains a flag-ON single-threaded atomization microbench (shard-lock + tryRefAtom vs today's unlocked per-thread probe), recorded with a regression budget set at INT.

4. Byte-cap edits: N8 paragraph and §6.7 recycling bullet shrank (resolutions are shorter than open blockers); no normative content removed.


## §12. Whole-design adversarial review round 2 — resolutions (rev 10 -> rev 11)

1. **No TID recycling ever / 32766-thread lifetime cap (major, cross-cutting) — ACCEPTED.** §6.7 updated: recycling is phase-1 NONE, but GC-time rebias/reissue is now CHARTERED-OWNED (OM ledger 8c r12/OM Task 13+api task 15): a shared-GC stop restamps dead-TID butterfly tags and structure transition-TIDs to 0, after which the api workstream reissues TIDs via m_freeTIDs. vmstate remains the TID-read provider only; no vmstate code change, wording only.
2. **Composed flag-on single-thread tax (blocker, cross-cutting) — ADDRESSED via budgets:** §10's "budget set at INT" replaced by membership in jit Task-13's normative <=5% composite flag-on 1-thread gate (the W1 atomization deltas — shard lock, tryRefAtom CAS, deref upgrade — are inside that composite).
3. **Phase-B-unowned charter gap (major, cross-cutting) — RECORDED:** Dev 10 now states Phase B GATES the N-mutator perf milestone and points at api §2's composed-deliverable note, which enumerates all gating charters for orchestrator sign-off before GIL removal.
4. **Cap compliance:** rev 11 additions paid by wording-only squeezes (arrow/equals spacing outside code spans; Dev-9 and §7 note compression). No normative change beyond items 1-3.

## §13. Whole-design adversarial review round 3 — resolutions (rev 11 -> rev 12)

1. **Thread-granularity STW unowned (major, cross-cutting) — ACCEPTED.** Dev 10 r12 extends the Phase-B frozen contract's scope: beyond per-thread `VMThreadContext`/`VMTraps` (§6.8), Phase B ALSO covers VMManager counting entered THREADS per VM (per-thread NVS tickets), so a stop request parks every thread of a multi-thread VM — without it, post-GIL VMM bookkeeping counts one VM and a JSThreads conductor could proceed while sibling threads of the same VM still run elided code (voiding OM I13/jit I2/I8). jit R1 gains the matching freeze-scope note (R1.c re-frozen at Phase B); api §2's charter enumeration names it. Phase B remains UNOWNED; this revision only fixes what the future charter must deliver, which is exactly the renegotiation-before-fan-out the finding asked for. heap is unaffected (its GC barrier is per-client access state — heap §3.8 note).
2. **Perf-gate matrix (major, cross-cutting) — mirrored.** §10's budget line now cites jit Task-13's two-config matrix (the flag-on atomization microbench remains inside the composite).
3. Cap compliance: paid by compressing the rev banner, Dev 9 (details preserved in the §9 cross-WS list: api:148/api:12 cites), the §7 old-tables pointer, and the SPEC-heap cite-refresh note. No W1/W2/W3 protocol, layout, invariant, or manifest change.

## Round-4 COMPOSED-design review — rev 13 (r4) resolutions

### r4.1 R4 prep/overlay (CONFIRMED bootstrap finding)
The composed review found vmstate's Task-10 self-check ("all-flags-off build, JSTests smoke
diff-free, extractor builds") impossible under its own ownership rules: the §3 flags did not
exist on disk, OptionsList.h/VM.h/JSLock.cpp/MicrotaskQueue.cpp/VMEntryScope.cpp are
non-editable, and vmstate had granted itself no overlay. New R4: M_opts is orchestrator-
PRE-APPLIED to the shared tree before fan-out (single cross-spec convention, jit §10; api
9.2-1 canonical for useJSThreads; no local OptionsList patches anywhere); M_opts2 remains an
INT hunk (its implied flags are inert pre-landing, so pre-application is unnecessary);
Task-10 self-checks build in a heap-§14-style private overlay worktree carrying
M3/M4/M6/M9/M11-M13, hunks never committed. M8 (extractor) runs in the overlay too.

### r4.2 Gate split (heap deviation-7 r13)
§10's budget line now cites the split: jit Task-13 gates {useJSThreads=1,useSharedGCHeap=0}
at <=5%; {1,1} is recorded, not gated phase 1. No vmstate protocol change; R3/I4/I10/I13
flag-off bars unchanged.

### r4.3 Phase-B (Dev 10) status
The review flagged thread-granular STW as unowned. Dev 10 already declares Phase B a frozen
contract for a FUTURE chartered WS gating the N-mutator milestone; api §2 (r14) now marks the
charter a HARD precondition of GIL removal and jit Task-13's integration gate labels its
config coverage. No vmstate text change beyond the rev bump.

### r4.4 Editorial (size cap)
M5/M10 removal rationales compressed (full text: M10 was removed in rev 9 because a VM.cpp
"never consulted" assert is unsatisfiable — the JSLock swap and the 14 atomStringTable
asserts legitimately read the table POINTER (JSLock.cpp:124); replaced by debug asserts atop
each ASI.cpp legacy arm, owned, no hunk. M5 was removed in rev 7: the swap is kept and no
assert is relaxed). §6.3 comment block, I16 toolchain list, §0 extractor cites, N6 cross-WS
note compressed; semantics unchanged.
