# MC-LIFE — Shared-backing-store lifetime accounting

Mechanism class (web-derived, treated as data): refcount/ownership of a shared raw
buffer mismanaged across agent boundaries — serialize/deserialize failure paths,
unbalanced increments, width-limited counters, native pointers held past release.
Exemplars: Mozilla bug 1352681 (SpiderMonkey SAB refcount leak + uint32 wrap →
cross-thread UAF; see `jsengine-sab.md`, class L), Erlang NIF resource-destructor
races (destructor runs on the wrong thread / past release).

Audit date: 2026-06-07. Tree: `jarred/threads` (UNGIL-HANDOUT rev 32 era; annex N6
quarantine landed in `runtime/ArrayBuffer.cpp`, GIL removal in progress). Defensive
audit of our own engine; read-only on `Source/**`.

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

- Where: `ArrayBuffer.h:332` `Checked<unsigned> m_pinCount` (NON-atomic),
  `ArrayBuffer.h:337` `bool m_locked` (plain); mutators `pin()/unpin()`
  (`ArrayBuffer.h:381-389`), `pinAndLock()` (`:396-399`); predicate
  `isDetachable() { return !m_pinCount && !m_locked && !isShared(); }` (`:391-394`).
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
- **Verdict: suspected.** Fix shape: make `m_pinCount` atomic (or rule pin/unpin
  under the buffer's `BufferMemoryHandle::lock()` / a documented single-thread
  embedder contract) and add the pair to the N7 field table. Needs a native/TSAN
  arm, not a JSTests case; flagged for the thread-scanners battery.

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
  (`ArrayBuffer.cpp:1536-1575`) + stale-mapping keepalive
  (`quarantineStaleWasmMappingGILOff`, `:281-320`); governing spec: annex N6 arm 4
  (GROW): relocation must run under a heap §10 stop, old mapping quarantined to
  the NEXT stop.
- Status, per the code's own comment (`:1552-1558`): the **quarantine half is
  landed** (replaced `BufferMemoryHandle` kept alive until a stop ⇒ torn
  {pre-grow length, pre-grow base} never derefs an unmapped base) but the **stop
  conduction in `Wasm::Memory::grow`'s BoundsChecking arm is NOT YET ESTABLISHED —
  "OPEN DEPENDENCY, blocks U-T13 sign-off"**. Until it lands, a GIL-off relocating
  grow racing a reader can pair a *post-grow length* with the *pre-grow base*:
  the old mapping is alive but **short**, so the read runs off its end — a shared
  raw buffer's pointer held past (the end of) its allocation, squarely MC-LIFE.
- **Verdict: suspected** (known-open, already tracked as a U-T13 blocker; this
  audit adds an executable witness). Test:
  `JSTests/threads/cve/mc-life-wasm-grow-relocate.js` — spawned TA readers hammer
  views over a no-maximum `WebAssembly.Memory` while main grows in a loop
  (spawned threads do plain TA accesses only — SPEC-api §I refuses spawned wasm
  *execution*, not views). Expected to pass only once the stop conduction lands;
  amplifier-ready.

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
- **Verdict: suspected** (contract gap, not an engine bug; cannot be witnessed
  from JSTests). Recommendation: document in INTEGRATE-api.md that GIL-off
  `ArrayBufferDestructorFunction`s MUST be thread-agnostic, and have Bun route
  thread-affine finalizers through its own marshalling (as it already must for
  napi post-ungil).

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

---

## Summary table

| # | Surface | Verdict | Artifact |
|---|---------|---------|----------|
| S1 | Thread() value passing | immune (no serialization exists; SPEC-api I2/5.10) | — |
| S2 | SharedArrayBufferContents refcount | needs-test (balance churn; width infeasible) | `mc-life-sab-refchurn.js` |
| S3 | ArrayBuffer refcount cross-thread | immune (atomic DeferrableRefCounted; stale comment noted) | — |
| S4 | m_pinCount/m_locked vs detach | suspected (non-atomic, uncovered by N6/N7; embedder-only) | native/TSAN arm needed |
| S5 | N6 quarantine ownership machine | needs-test | `mc-life-detach-quarantine-storm.js` |
| S6 | Relocating wasm grow, old-mapping lifetime | suspected (documented OPEN DEPENDENCY, U-T13 blocker) | `mc-life-wasm-grow-relocate.js` |
| S7 | Embedder destructor thread-affinity | suspected (contract, Bun-side; doc fix) | — |
| S8 | jsc shell $.agent transfer | immune (RAII Message ownership) | — |
| S9 | NLS/NCS/WaiterList lifetime | immune (SPEC-api 5.3/5.4 RAII; dtor-unregister tie) | — |

Tests live under `JSTests/threads/cve/` and are written to be EXECUTED LATER
post-ungil (do not run against the mid-bring-up tree). Each is self-checking
(failure = throw) per annex T2 conventions; the racing arms are vacuous-but-green
under the GIL and become meaningful with `--useThreadGIL=0`.
