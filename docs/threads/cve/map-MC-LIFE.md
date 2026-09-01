# MC-LIFE — Shared-backing-store lifetime accounting

Mechanism class (web-derived, treated as data): refcount/ownership of a shared
raw buffer or shared resource mismanaged across agent boundaries — serialize/
deserialize failure paths, unbalanced increments, width-limited counters, native
code holding pointers past release, **allocator owned by a mortal thread**.
Exemplars: Mozilla bug 1352681 (SpiderMonkey SAB refcount leak + uint32 wrap →
cross-thread UAF; see `jsengine-sab.md`, class L), Node.js #28777 (Worker
terminate frees the Isolate-owned `ArrayBuffer::Allocator` while a SAB
BackingStore created via it is still live in another agent), V8 d8
cr/1215233004 (d8 Worker SAB transfer: externalized backing store outlives the
creating Isolate), Erlang NIF resource-destructor races (destructor runs on the
wrong thread / past release).

Audit date: 2026-06-15 (re-audit; first pass 2026-06-07; **B4/B10/B11 update
2026-06-18**). Tree: `jarred/threads` (UNGIL-HANDOUT rev 32 era; annex N6
quarantine landed in `runtime/ArrayBuffer.cpp`, GIL removal in progress; S6
stop conduction NOW LANDED in `wasm/WasmMemory.cpp` per CVE-AUDIT Tier-B
B4). Defensive audit of our own engine; read-only on `Source/**`.

Verdict legend: **immune** = immune-by-construction (cited protocol/invariant),
**needs-test** = targeted susceptibility test written under `JSTests/threads/cve/`,
**suspected** = susceptible-suspected (precise suspected hole stated).

---

## S1. Thread-boundary value passing — the serialize/deserialize surface itself

- Where: `runtime/ThreadObject.cpp` (spawn/args/result; views allowlist at
  `ThreadObject.cpp:762-770`); SPEC-api §4.1 ("this===undefined", arg identity) and
  §5.10 (argSlots root, "no copy"); corpus pin `JSTests/threads/api/thread-basic.js`
  (API-I2: `new Thread(() => v).join()` is SameValue/reference-equal to `v`).
- Mechanism fit: Mozilla 1352681 lived in structured clone — a *serializer*
  manually incremented `SharedArrayRawBuffer::refcount_` and a failed
  *deserialize* leaked the increment.
- **Verdict: immune.** Our Thread API performs **no serialization at all**: values
  cross by reference into the shared heap (SPEC-api I2 identity invariant; argSlots
  are GC roots, not counted handles). There is no serialize step that takes a ref,
  no deserialize step that can fail and strand one, and no per-agent proxy object
  whose count must balance. The exemplar's trigger surface does not exist inside
  the Thread() boundary. Adversarial check: the only "transfer-shaped" paths left
  in-tree are (a) the jsc shell `$.agent` harness and (b) embedder API — covered as
  S2/S8 below; neither is reachable from `Thread()`.

## S2. SharedArrayBufferContents refcount (the direct Mozilla analog)

- Where: `runtime/ArrayBuffer.h:59` — `SharedArrayBufferContents final :
  ThreadSafeRefCounted<SharedArrayBufferContents>`; count is
  `std::atomic<uint32_t>` with **no overflow check** (`wtf/ThreadSafeRefCounted.h:44`,
  relaxed `fetch_add`). Ref-taking sites enumerated:
  `ArrayBufferContents::shareWith` (`ArrayBuffer.cpp:665-680`, RefPtr copy),
  `ArrayBufferContents(Ref<SharedArrayBufferContents>&&)` ctor,
  `ArrayBuffer::transferTo` shared arm (`ArrayBuffer.cpp:942-945`),
  N6 quarantine entries (co-ref, `ArrayBuffer.cpp:330-340` comment),
  `Wasm::Memory` (`wasm/WasmMemory.h`), jsc shell `$.agent` broadcast/receive
  (`jsc.cpp:2733-2746` enqueue, `2664-2685` dequeue).
- Width: uint32, same as Mozilla's `refcount_`. Difference that matters: **every
  increment here is RAII** (`Ref`/`RefPtr`), each long-lived ref is embedded in an
  owner object ≥ tens of bytes (JSArrayBuffer wrapper, ArrayBufferContents, shell
  `Message`, quarantine entry), and every failure path I could find unwinds through
  the owner's destructor. The shell broadcast path is the closest structural match
  to the Mozilla bug (enqueue refs per worker before any receiver runs) and it is
  balanced: an undequeued `Message` derefs in its destructor. Wrapping the counter
  therefore requires ~2^32 *simultaneous live owners* (≥64 GiB of holders), not
  2^32 cheap leaked increments.
- **Verdict: needs-test** (balance regression, not wrap). The construction is
  sound today, but the class's real risk is a *future* unbalanced site (e.g. a Bun
  embedder path calling `leakRef()` or a hand-rolled `ref()` on a failure path).
  Test: `JSTests/threads/cve/mc-life-sab-refchurn.js` — cross-thread SAB +
  growable-SAB + shared-wasm-memory wrapper/view churn under GC pressure with
  sentinel integrity checks; a premature final-deref surfaces as UAF (deterministic
  under ASAN, amplifier-ready under TSAN). Defense-in-depth recommendation (not a
  hole): an overflow `RELEASE_ASSERT` in `ThreadSafeRefCountedBase::ref()` for
  SAB-sized objects, as V8 added after the Mozilla bug class.

## S3. ArrayBuffer (non-shared) refcount across threads

- Where: `ArrayBuffer : GCIncomingRefCounted<ArrayBuffer>` over
  `wtf/DeferrableRefCounted.h:55-103` — converted to `std::atomic<uint32_t>`
  (relaxed inc, acq_rel final dec) **specifically for GIL removal** (comment at
  `ArrayBuffer.cpp:230-236`: "GIL-removal review round 3 … closing the gap").
- Non-shared ArrayBuffers cross threads in our model as ordinary heap objects
  (no per-agent contents duplication), so cross-thread ref/deref is reachable;
  the atomic conversion is the governing fix. Effective width 31 bits
  (`normalIncrement = 2`, `DeferrableRefCounted.h:52`) — wrap again requires 2^31
  live owners; refs are RAII throughout (`Ref<ArrayBuffer> protect(*this)` in
  `transferTo` `ArrayBuffer.cpp:927` is the pattern).
- One stale comment: `ArrayBuffer.cpp:345-351` (quarantine-entry rationale) still
  says the count "is a plain uint32_t" and avoids a cross-thread ref/deref pair via
  the raw-pointer+generation closure. The avoidance is *more* conservative than
  needed post-conversion, so it is documentation drift, not a hazard — but it should
  be reconciled before someone "fixes" the closure into a Ref keepalive and deletes
  the generation/ABA guard, which is still needed for the `~ArrayBuffer`-before-stop
  case independent of refcounting.
- **Verdict: immune** (atomic count; acq_rel final-deref publication; RAII holders;
  exercised incidentally by both tests below).

## S4. Pin/lock accounting: `m_pinCount` / `m_locked` — native pointers vs detach

- Where: `ArrayBuffer.h:404` `std::atomic<unsigned> m_pinCount` (was
  `Checked<unsigned>`, NON-atomic, through 2026-06-15; atomicized 2026-06-18
  per B10), `std::atomic<bool> m_locked`; mutators `pin()/unpin()`
  (`ArrayBuffer.h:453-461`), `pinAndLock()` (`:468-`); predicate
  `isDetachable() { return !m_pinCount && !m_locked && !isShared(); }` (`:463-466`).
  Callers: `ArrayBufferView::setDetachable` (`ArrayBufferView.cpp:95-108`) and the
  C API `JSTypedArray.cpp:250,337` (`JSObjectGetTypedArrayBytesPtr` et al. —
  exactly the "API user fetched a native pointer" case the field documents).
- Spec coverage: **none found.** Grepped `UNGIL-HANDOUT.md` and
  `SPEC-ungil-audit-N7.md` for pin/pinCount/pinAndLock/m_locked — annex N6 governs
  detach/transfer/resize ordering but is silent on the pin counter; N7's rows do
  not list these fields.
- Suspected hole (post-ungil, embedder-only): two embedder threads (SPEC-api §
  "TID note": post-GIL embedder threads get real TIDs and enter in parallel)
  racing `pin()`/`unpin()` on wrappers of the same ArrayBuffer lose an update
  (plain `++`/`--` on `Checked<unsigned>`): undercount → `isDetachable()` true
  while a native `bytesPtr` is outstanding → JS-side `transfer()` detaches, the
  N6 quarantine frees the mapping at the next stop → embedder pointer held **past
  release** — exactly the MC-LIFE end state. (Overcount/underflow instead traps
  via `Checked`, a DoS.) `m_locked` is sticky-true and idempotent, so the
  `pinAndLock` C-API path alone is benign; `setDetachable(true/false)` pairs are
  the racing surface (WebCore uses them; Bun's embedder surface should be audited
  for them).
- Not reachable from pure JS (no JS-visible pin), so no JS test can be written.
- **Verdict: fix-landed (CVE-AUDIT Tier-B B10, 2026-06-18).** `m_pinCount` is
  now `std::atomic<unsigned>` and `m_locked` is `std::atomic<bool>` (relaxed;
  `ArrayBuffer.h:404,409` + accessors `:453-487`); the `Checked<>` overflow/
  underflow crash is preserved as `RELEASE_ASSERT` in `pin()`/`unpin()` so the
  fail-stop is not weakened. The lost-update on concurrent embedder
  `pin()/unpin()` is closed and the fields are TSAN-clean. The residual
  embedder obligation (an unpinned `data()` read has no lifetime guarantee
  against a concurrent detach) is documented in
  `docs/threads/INTEGRATE-api.md` §9.2-10. Gate stays a native/TSAN arm
  (thread-scanners battery); no JSTests case.

## S5. Annex N6 quarantine — the ownership-accounting machine itself

- Where: `runtime/ArrayBuffer.cpp:151-560` ("SPEC-ungil §N.6 / annex N6:
  per-server ArrayBuffer mapping quarantine"); governing spec: UNGIL-HANDOUT §6
  item 6 / annex N6 (BINDING), invariant: *any observable base must point at a
  mapping that is mapped and sized ≥ every length still observable against it;
  retirement requires heap §10 stop quiescence*.
- This is where MC-LIFE accounting now *lives*: detach/transfer move mapping
  ownership into a quarantine entry (`ArrayBufferQuarantineEntry`,
  `ArrayBuffer.cpp:330-360` — entry owns `m_data + m_destructor`, co-refs
  `m_shared`/`m_memoryHandle`); the writer-writer arbiter is the detached-buffer
  side table (`:199-247`): test-and-set under a leaf lock guarantees **exactly one**
  racing `detach()`/`transferTo()` enqueues and fires `notifyDetaching` (no double
  enqueue, no double `std::exchange` of `m_destructor`); generation numbers make
  the stop-time `clearBaseWordAtStop` ABA-safe against a recycled `ArrayBuffer*`
  (`~ArrayBuffer` unregisters, `:268-279`); shrink keeps the one-tail-per-handle /
  tail-abuts-end invariant (`deferShrinkTailGILOff` `:505-530`,
  `consumeQuarantinedTailOnRegrow` `:532-560`); entries are byte-accounted as heap
  extra memory so a detach storm pulls the next stop forward (`:489-497`).
- Adversarial review of the accounting (what I tried to break):
  - double-detach / detach-vs-transfer: arbitrated by the table (`:203-205`);
    `transferTo` re-checks the flag and its trailing `detach(vm)` no-ops on loss
    (`:929-940, 1003-1007`). OK.
  - source GC'd between detach and stop: entry owns the mapping independently;
    closure neutered by generation (`:268-279`). OK.
  - shrink-then-regrow-then-stop: regrow consumes/trims the pending tail under the
    handle lock *before* any GC-capable call (`:532-560` precondition comment);
    retirement asserts `handle->size() == tailOffset + tailSize` (`:418`). OK.
  - enqueue concurrent with first hook registration: entry waits for the stop
    after the hook append — the binding direction ("enqueued before the stop") is
    preserved (`:470-477`). OK.
- **Verdict: needs-test** (the design is sound on paper; the class demands an
  executable witness for the ownership arms). Test:
  `JSTests/threads/cve/mc-life-detach-quarantine-storm.js` — concurrent
  `transfer()` races on one buffer (exactly-one-ownership arm), detach/transfer/
  resize-shrink/re-grow storm vs spawned readers, and a transferee/source-GC'd-
  before-stop arm; double-free or premature free surfaces under ASAN, stale-base
  deref under TSAN/ASAN. Mirrors the N6 U28 amplifier from the MC-LIFE
  (ownership/double-free) angle rather than the torn-pair angle.

## S6. Relocating wasm grow — old-mapping lifetime (KNOWN OPEN)

- Where: `ArrayBufferContents::refreshAfterWasmMemoryGrow`
  (`ArrayBuffer.cpp:1647-1675`) + stale-mapping keepalive
  (`quarantineStaleWasmMappingGILOff`, `:343-`); governing spec: annex N6 arm 4
  (GROW): relocation must run under a heap §10 stop, old mapping quarantined to
  the NEXT stop.
- Status, per the code's own comments (`ArrayBuffer.cpp:325`, `:1401`,
  `:1661-1664`): the **quarantine half is landed** (replaced `BufferMemoryHandle`
  kept alive until a stop ⇒ torn {pre-grow length, pre-grow base} never derefs an
  unmapped base) and the **stop conduction in `Wasm::Memory::grow`'s
  BoundsChecking arm is NOW ESTABLISHED** (CVE-AUDIT Tier-B B4, 2026-06-18):
  the handle swap + per-instance cache + `refreshAfterWasmMemoryGrow` (and its
  per-view `refreshVector` walk) run inside a
  `JSThreadsSafepoint::stopTheWorldAndRun` closure, gated
  `g_jscConfig.gilOffProcess` so flag-off is byte-identical. With every other
  mutator parked at a safepoint during publication, a reader cannot pair a
  *post-grow length* with the *pre-grow base*; the quarantine still bounds the
  old mapping's lifetime to the NEXT stop for captured/hoisted pre-grow
  snapshots. U-T13 dependency discharged.
- **Verdict: fix-landed (premise-gated needs-test).** Test:
  `JSTests/threads/cve/mc-life-wasm-grow-relocate.js` — spawned TA readers hammer
  views over a no-maximum `WebAssembly.Memory` while main grows in a loop
  (spawned threads do plain TA accesses only — SPEC-api §I refuses spawned wasm
  *execution*, not views). Premise-SKIPs while the GIL-off wasm refusal is in
  force; must PASS once the refusal lifts. Amplifier-ready.

## S7. Embedder destructors run on a foreign agent (Erlang-NIF analog)

- Where: `ArrayBufferDestructorFunction` (`ArrayBuffer.h:57`);
  `SharedArrayBufferContents::~SharedArrayBufferContents`
  (`ArrayBuffer.cpp:644-651`) runs `m_destructor` on whichever thread performs the
  **final deref** — post-ungil that can be any JS thread, a stop conductor, or the
  shell worker reaper; quarantine retirement likewise runs contents destructors on
  the stop-conducting thread (`arrayBufferQuarantineSafepointHook` drain,
  `ArrayBuffer.cpp:440-456`).
- Engine-side this is memory-safe (destructor runs exactly once, after ownership
  is provably sole — that is what S2/S5 establish). The exposure is the *embedder
  contract*: Bun external buffers (the N6 r13 rejection note names them) and
  napi-style finalizers are frequently thread-affine ("must run on the JS thread
  that created them"). A thread-affine destructor invoked on the stop conductor is
  the Erlang NIF destructor-race shape: not a JSC heap corruption, but a
  use-after-release in the embedder's own bookkeeping.
- **Verdict: doc-landed (CVE-AUDIT Tier-B B11, 2026-06-18).** The contract is
  now recorded in `docs/threads/INTEGRATE-api.md` §9.2-10: under GIL-off an
  `ArrayBufferDestructorFunction` MAY run on any thread (final-deref thread,
  the §10 stop conductor via the N6 quarantine drain, or after the creator
  thread has exited), with no `JSLock`/`VMLite` installed; it MUST be
  thread-agnostic and MUST NOT touch per-thread/per-creator state or call
  back into JS. Bun routes thread-affine finalizers (napi external buffers,
  FFI) through its own marshalling. Engine-side remains memory-safe
  (exactly-once after sole ownership, per S2/S5).

## S8. jsc shell `$.agent` broadcast/receive (test-infra serialize path)

- Where: `jsc.cpp:2733-2746` (broadcast: `transferTo` = share for SAB / wasm
  shared `RefPtr` into a per-worker `Message`), `jsc.cpp:2664-2685` (receive:
  adopt into fresh wrapper).
- The only true serialize/deserialize of SAB contents in-tree. Failure paths
  (worker exits before dequeue; wrapper allocation throws) all unwind through
  `Message`/`Ref` destructors — RAII, no manual counter, no leak-then-wrap path.
  Test-infra only (not shipped surface).
- **Verdict: immune** (RAII ownership end-to-end; exercised by the refchurn test's
  shell-agnostic arms).

## S9. Native lock/condition/waiter backing state (NLS/NCS/Waiter)

- Where: `runtime/LockObject.h:39` (`NativeLockState :
  ThreadSafeRefCounted`), `runtime/ConditionObject.h:36,60` (`CondWaiter`,
  `NativeConditionState`), `runtime/WaiterListManager.h:40,121` (`Waiter`,
  `WaiterList`, both ThreadSafeRefCounted); governing spec: SPEC-api §5.3 (NLS),
  §5.4 (NCS).
- Cross-agent lifetime: the JS cell (`LockObject`/`ConditionObject`) holds a
  `Ref` to the native state; every parked/async waiter holds its own `Ref`
  (`m_asyncWaiters` deque of `Ref<AsyncTicket>`, `waiters` deque of
  `Ref<CondWaiter>`), so a cell GC'd while a foreign thread is parked cannot free
  the native state under the parker — RAII, atomic counts. SAB waiter lists are
  address-keyed with the mapping↔list tie maintained by
  `WaiterListManager::unregister` in `~SharedArrayBufferContents`
  (`ArrayBuffer.cpp:646`), i.e. the list dies with (never after) the mapping, and
  the manager's own locks order unregister vs notify.
- **Verdict: immune** (ThreadSafeRefCounted RAII per SPEC-api 5.3/5.4; the
  dtor-unregister tie covers the waiter-list/mapping accounting; same posture as
  the empty public-CVE record for class W noted in `jsengine-sab.md`).

## S10. Per-thread GC allocator (TLC) — allocator owned by a mortal thread

- Where: `heap/GCThreadLocalCache.h:54,97-101,117` (`~GCThreadLocalCache`
  → `stopAllocatingForGood()`; `Vector<unique_ptr<LocalAllocator>>
  m_ownedAllocators`), `heap/GCThreadLocalCache.cpp:80-96,248-269`; governing
  spec: SPEC-heap §5.3 (TLC, teardown under MSPL), I9 (post-teardown no allocator
  in any `m_localAllocators` list, every held block `inUse == false`), I5b
  (directory bitvector writers hold MSPL ∨ WSAC), I12 (window-witness rooting:
  `stopAllocating` NA-stamps every non-free cell of every block).
- Mechanism fit: this is the literal "allocator owned by a mortal thread" surface
  for *JS heap cells*. A spawned `Thread` owns a `GCClient::Heap` whose TLC owns
  `LocalAllocator`s; the thread allocates objects (cells live in `MarkedBlock`s
  obtained from the server's `BlockDirectory`), shares them by reference (S1),
  then exits.
- Why the Node #28777 shape cannot occur: the TLC owns the **bump-pointer/
  free-list state** (`LocalAllocator`), NOT the storage. `MarkedBlock`s belong to
  the server-side `BlockDirectory` (SPEC-heap §3.1: "directories/blocks/bitvectors
  stay shared server-side"); `~GCThreadLocalCache` runs `stopAllocatingForGood()`
  under MSPL (`GCThreadLocalCache.cpp:248-269`, the I5b writer rule), which flips
  the directory's `inUse` bit and NA-stamps every handed-out cell (I12 leg (a)) so
  the conductor roots them at the next stop, then unlinks from
  `m_localAllocators` (rank 8). The destructor frees only the `LocalAllocator`
  struct (`m_ownedAllocators.clear()`, `:96`) — no cell, no block. The server heap
  outlives every client by MC-TDWN S1's `Ref<VM>` fence. Adversarial check: a
  partially-allocated cell (TLC died mid-allocation)? L2 ("no client releases heap
  access mid-allocation/initialization", SPEC-heap I12 soundness clause) forbids
  it — DCT runs only after the JSLock release (SPEC-api §5.2 teardown order,
  `ThreadObject.cpp:391-397`).
- **Verdict: immune** (storage server-owned; teardown is hand-back under MSPL per
  SPEC-heap §5.3/I9/I12; native witness exists as `clientChurnVsGC` in
  `SharedHeapTestHarness`, SPEC-heap §12.1).

## S11. ArrayBuffer backing-store allocator scope — creator-thread mortality

- Where: default contents destructor is the static SharedTask
  `[] (void* p) { Gigacage::free(Gigacage::Primitive, p); }`
  (`ArrayBuffer.cpp:114` — capture-free); growable/handle-backed paths free via
  `BufferMemoryManager::singleton()` (`runtime/BufferMemoryHandle.cpp:199-202`,
  `LazyNeverDestroyed<BufferMemoryManager>`); the N6 quarantine list and its
  safepoint hook are per-**server-heap** (SPEC-heap §13.10d "PER-SERVER-HEAP
  quarantine epoch"), not per-client; `~ArrayBuffer` unregisters via the
  process-global side table (`ArrayBuffer.cpp:1536-1545`).
- Mechanism fit: this is the **direct Node.js #28777 / V8 d8 analog** — a buffer
  allocated on agent A, referenced by agent B, A dies, B's last deref must free
  via an allocator that no longer exists (Node) or whose externalization
  bookkeeping was per-Isolate (d8). In our model, A = a spawned `Thread`; B =
  main or a sibling.
- Why the trigger does not exist in-engine: every allocator a JSC-created
  ArrayBuffer's destructor reaches is **process-immortal** — Gigacage is
  process-global, `BufferMemoryManager` is `LazyNeverDestroyed`, and the
  destructor closure captures *nothing* (no `vm`, no `VMLite*`, no
  `GCClient::Heap*`). Heap extra-memory accounting on the quarantine drain
  (`:489-497`) targets the shared server `Heap`, which the Ref<VM> fence keeps
  alive past every client (MC-TDWN S1). Adversarial check: a spawned thread can
  *enqueue* a quarantine entry (it called `transfer()`) and then die before the
  retiring stop — the entry lives in the server-heap list, the closure carries
  only `{ArrayBuffer*, generation}` (process-scoped table lookup), so creator
  death is irrelevant; covered by S5's generation/ABA arm.
- Residual exposure: an *embedder*-supplied `ArrayBufferDestructorFunction` that
  closes over per-creator-thread state (e.g. a Bun napi finalizer capturing a
  `napi_env` bound to the spawning thread). That is S7's contract gap restated
  for the mortality direction; same recommendation applies.
- **Verdict: needs-test** (the construction is immune for in-engine buffers;
  the test exists to (a) lock the "creator dies first" direction into the corpus
  — every existing MC-LIFE arm creates on main and shares *to* spawned, never the
  reverse — and (b) exercise the quarantine/extra-memory accounting when the
  enqueueing client is gone). Test:
  `JSTests/threads/cve/mc-life-creator-thread-dies.js` — spawned threads create
  SAB / growable-SAB / non-shared ArrayBuffer / resizable ArrayBuffer, stamp
  sentinels, return them to main, and **exit**; main then churns views, detaches/
  resizes the survivors, applies GC pressure across multiple stops, and verifies
  sentinel integrity. A per-creator-thread allocator/destructor dependency would
  surface as UAF/crash under ASAN once the creator's `GCClient::Heap`/`VMLite`
  are gone.

---

## Summary table

| # | Surface | Verdict | Artifact |
|---|---------|---------|----------|
| S1 | Thread() value passing | immune (no serialization exists; SPEC-api I2/5.10) | — |
| S2 | SharedArrayBufferContents refcount | needs-test (balance churn; width infeasible) | `mc-life-sab-refchurn.js` |
| S3 | ArrayBuffer refcount cross-thread | immune (atomic DeferrableRefCounted; stale comment noted) | — |
| S4 | m_pinCount/m_locked vs detach | fix-landed (B10: atomic count+flag, Checked<> crash preserved; embedder contract §9.2-10) | native/TSAN arm |
| S5 | N6 quarantine ownership machine | needs-test | `mc-life-detach-quarantine-storm.js` |
| S6 | Relocating wasm grow, old-mapping lifetime | fix-landed (B4 stop conduction; premise-gated PASS until wasm refusal lifts) | `mc-life-wasm-grow-relocate.js` |
| S7 | Embedder destructor thread-affinity | doc-landed (B11: INTEGRATE-api.md §9.2-10 thread-agnostic contract) | — |
| S8 | jsc shell $.agent transfer | immune (RAII Message ownership) | — |
| S9 | NLS/NCS/WaiterList lifetime | immune (SPEC-api 5.3/5.4 RAII; dtor-unregister tie) | — |
| S10 | Per-thread TLC/LocalAllocator teardown | immune (SPEC-heap §5.3/I9/I12; blocks server-owned) | `clientChurnVsGC` (native) |
| S11 | Backing-store allocator scope (creator dies) | needs-test (process-immortal allocators; reverse-direction corpus gap) | `mc-life-creator-thread-dies.js` |

Tests live under `JSTests/threads/cve/` and are written to be EXECUTED LATER
post-ungil (do not run against the mid-bring-up tree). Each is self-checking
(failure = throw) per annex T2 conventions; the racing arms are vacuous-but-green
under the GIL and become meaningful with `--useThreadGIL=0`.
