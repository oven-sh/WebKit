# MC-GROW — resize/detach/transfer invalidates a cached base+length

Mechanism class (web-derived, data): a grow/detach/transfer replaces a backing
store or moves a bound while a holder (JIT code, native frame, view,
half-built instance) retains the old base or length. Exemplars:
CVE-2017-15399, CVE-2018-5093, V8 bug 826434, CVE-2024-2887 (GSAB
length-underflow leg).

Audit date: 2026-06-07; **re-audited 2026-06-15** against the post-closeout
tree (line numbers refreshed; verdicts re-verified — no change). Tree:
`jarred/threads`. Scope: GIL-off (`--useJSThreads=1 --useThreadGIL=0`);
GIL-on serializes every arm below and is not at risk.

Governing design: SPEC-ungil §N.6 (docs/threads/SPEC-ungil.md) with the
BINDING full text + torn-pair table in annex N6
(docs/threads/UNGIL-HANDOUT.md:2834-2935), audit rows R10/R11
(docs/threads/SPEC-ungil-audit-N7.md:201-202); butterfly storage is governed
by SPEC-objectmodel §4.4 (docs/threads/SPEC-objectmodel.md:119-128) and
invariants I6/I7/I16/I26/I27/C2/C4.

The design principle (annex N6, verbatim intent): every tier's TA/DataView
fast path loads LENGTH, bounds-checks, then loads BASE, with no ordering
between the two loads — so store ordering alone can never close a torn
two-word read. The invariant is therefore lifetime-shaped, not
ordering-shaped: **a racing reader must never pair a passing length with an
unmapped-or-short base**; retirement of a mapping requires that no
pre-retirement length remain live, which heap §10 stop quiescence provides.

---

## Surface inventory and verdicts

| # | Surface | Where | Verdict |
|---|---------|-------|---------|
| S1 | Butterfly element-storage grow (JS arrays/objects) | runtime/JSObject.cpp:525-987, JSObjectInlines.h:308; ButterflyInlines.h | immune-by-construction |
| S2 | Growable SharedArrayBuffer in-place grow | runtime/ArrayBuffer.cpp:1547-1612 | immune-by-construction |
| S3 | Resizable ArrayBuffer shrink / re-grow | runtime/ArrayBuffer.cpp:1237-1377, 539-620 | needs-test |
| S4 | ArrayBuffer detach + transfer (quarantine arms 1-2) | runtime/ArrayBuffer.cpp:970-1202, 150-523 | needs-test |
| S5a | Wasm grow, Signaling / reserved-VA (in-place) | runtime/ArrayBuffer.cpp:1647-1680; wasm/WasmMemory.cpp:359-381 | immune-by-construction |
| S5b | Wasm grow, BoundsChecking relocation | wasm/WasmMemory.cpp:337-415; runtime/ArrayBuffer.cpp:311-348, 1658-1680, 921-944 | **fix-landed (premise-gated needs-test)** |
| S6 | JSArrayBufferView mode transitions (wasteful/oversize) | runtime/JSArrayBufferView.cpp:265, :327 | immune-by-construction |
| S7 | Half-built instance publication (transferee, new views) | runtime/ArrayBuffer.cpp:1020-1031; runtime/JSArrayBufferPrototype.cpp:330-346 | immune-by-construction |
| S8 | JIT-hoisted TA base/length (per-tier fast paths) | DFG/FTL TA length-load sites; ArrayBuffer.cpp:1202 (detach watchpoint) | needs-test |

Tests written (DO NOT RUN until post-ungil; see //@ headers):
- `JSTests/threads/cve/mc-grow-buffer-storm.js` — S2/S3/S4/S5a/S8 storm
  (amplifier-ready; deterministic invariants asserted per read).
- `JSTests/threads/cve/mc-grow-wasm-relocating-grow.js` — S5b targeted
  susceptibility test. **Stop conduction landed** in Memory::grow's
  BoundsChecking arm (B4); test is premise-gated on the GIL-off wasm refusal
  and must PASS once that refusal lifts.

---

## S1. Butterfly element-storage grow — immune-by-construction

Mechanism leg: `arr.push(...)` on one thread reallocates element storage
while another thread's read path holds the old butterfly base or a stale
length (classic JSArray analog of V8 826434).

Why it cannot occur (SPEC-objectmodel §4.4, implemented in
runtime/JSObject.cpp):

- **The base+length pair is one word.** Element-storage resizes never touch
  the object header; they CAS the single 64-bit tagged butterfly word (I16,
  SPEC-objectmodel.md:226; `casButterfly` sites, JSObjectInlines.h:308 /
  JSObject.cpp:2399-2485, :6933). A reader loads the tagged word once and
  derives base AND vectorLength from the same snapshot (`taggedButterflyWord()`
  then `untaggedButterfly(word)` / `butterflySpine(word)`,
  JSObject.cpp:525-987) — there is no torn two-location read to begin with.
  CAS failure re-dispatches on the fresh tag, never blind-retries (GT10).
- **Stale snapshots never dangle.** Published spines are never mutated
  (I6, :214); growth allocates a NEW spine; fragments are never moved or
  reused, and aliased base/size are copied verbatim precisely because
  anything else is a GC UAF (I7, §4.3-1). The old flat butterfly /
  spine remains a GC-reachable heap object until no snapshot can reference
  it — a stale base reads stale VALUES, never freed memory.
- **Stale length vs. newer storage is min-bounded.** publicLength is shared
  across all spines an object publishes and may exceed a superseded spine's
  vectorLength; every segmented indexed access bounds by
  min(publicLength, loadedSpine->vectorLength) and reads
  [vectorLength, publicLength) as holes (C4/I33, SPEC-objectmodel.md:85;
  `segmentedIndexedSlotIfReadable`, JSObject.cpp:693-839).
- **In-place growth is the one base-preserving case** and is cell-locked,
  owner-only, with the tagged word re-checked under the lock and the new
  range cleared before the fenced vectorLength store (T5,
  SPEC-objectmodel.md:128); the SW=1/foreign case is forced to T2
  (new-spine publication), never in-place (I21/I27).
- **JIT-cached butterflies**: tier fast paths revalidate via the TID/SW tag
  (jit P5/CS3); foreign transitions fire both TTL watchpoint sets under STW
  (I10), so compiled code that hoisted a flat base is jettisoned or
  re-dispatches.

Adversarial self-check: the one place a base survives while bounds change is
T5 (in-place VL growth). The reader's snapshot word is unchanged by design
(pointer/tag identical), so a racing reader uses the OLD vectorLength with
the old base — strictly smaller bounds over the same mapping; the
[oldVL,newVL) range is cleared before the fenced setVectorLength, so a
reader that observes the new VL via a fresh snapshot cannot see garbage.
No passing-length/short-base pairing exists.

Existing corpus already exercises this exact mechanism:
`JSTests/threads/objectmodel/i03-stale-spine-reader-vs-grow.js`,
`i03-t5-racing-growers.js`, `i03-array-resize-cas.js`,
`JSTests/threads/arrays/push-resize-multithread.js`. No new test needed.

## S2. Growable SAB in-place grow — immune-by-construction

CVE-2024-2887's GSAB leg was a length-underflow consumed by JIT'd bounds
math. Our analog: `SharedArrayBufferContents::grow`
(runtime/ArrayBuffer.cpp:1547-1612).

- **Base immutable**: GSAB memory is reserved to maxByteLength at creation
  (`tryAllocateGrowableBoundsCheckingMemory` / fast-memory reservation,
  wasm/WasmMemory.cpp:223-240; ArrayBuffer.cpp:165 for the non-wasm
  resizable shape). Grow only `OSAllocator::protect`s the next pages of the
  SAME mapping (ArrayBuffer.cpp:1606-1607). No relocation ⇒ both torn pairs
  index one mapping (annex N6 torn-pair table: "grow in-place: both pairs
  in-bounds", UNGIL-HANDOUT.md:2918).
- **Monotonic, lock-serialized length**: the grow runs under
  `m_memoryHandle->lock()` (:1552); current length is loaded seq_cst (:1558)
  and `newByteLength < current` is rejected — the underflow leg is closed by
  construction (no shrink path exists for shared contents; `ArrayBuffer::resize`
  rejects wasm delta<0 for the wasm-backed route).
- **Commit-then-publish**: pages are protected RW and zero-filled BEFORE
  `updateSize(newByteLength)` publishes the larger length with seq_cst
  (:1606-1612; ArrayBuffer.h `updateSize`). A reader that observes the new
  length observes it after `mprotect` returned, and page-table updates are
  CPU-coherent at syscall return — no acquire is needed on the reader side
  for the pages to be accessible.

Adversarial self-check: the reader's unordered {length, base} loads cannot
hurt because base never changes and length only grows over committed pages.
The remaining risk is JIT code specializing on a STALE length — stale-small
is safe (subset of committed pages); stale-large is impossible (length is
monotonic and was never larger). Included in the storm test as a regression
guard, not as the verdict carrier.

## S3. Resizable ArrayBuffer shrink / re-grow — needs-test

Annex N6 arm 3, implemented in `resizeGILOff`
(runtime/ArrayBuffer.cpp:1237-1377) + `deferShrinkTailGILOff` /
`consumeQuarantinedTailOnRegrow` (:539-620).

Design: under `memoryHandle->lock()` publish the smaller `m_sizeInBytes`
seq_cst but DO NOT decommit; the tail page range is enqueued on the
per-server quarantine and the protect/decommit happens only inside a heap
§10 stop (`arrayBufferQuarantineSafepointHook`, :466-513), under quiescence;
re-grow before the stop consumes/cancels overlapping tail entries under the
same handle lock (:1305). Torn pairs: {oldLen, base} stale-but-safe (tail
still committed), {newLen, base} in-bounds.

Why needs-test rather than immune: the soundness argument leans on three
separately fragile conditions — (a) every decommit really is routed through
the quarantine (no residual `freePhysicalBytes` on the resizing thread; the
landed code at :1347 must keep losing arms out), (b) the hook fires under
genuine all-clients quiescence in BOTH GC protocols, and (c) the re-grow
path consumes pending tails BEFORE any allocation/GC-capable call while
holding the handle lock (the in-file comment at :567-575 documents the
self-deadlock/ordering hazard). All three are exercised only dynamically.
The annex's own U28 amplifier (UNGIL-HANDOUT.md:2926-2935) is NOT yet in the
corpus — no JSTests/threads test storms shrink/re-grow vs foreign readers.

Test: `mc-grow-buffer-storm.js` arm RAB-SHRINK (spawned length-tracking-view
readers + writer vs main running resize-shrink / re-grow-after-shrink storm,
plus a GC-pressure phase so quarantine retirement actually runs mid-storm).

## S4. Detach + transfer — needs-test

Annex N6 arms 1-2, implemented: `ArrayBuffer::transferTo`
(runtime/ArrayBuffer.cpp:970-1055), `ArrayBuffer::detach` (:1057-1192),
`notifyDetaching` (:1194-1202), quarantine machinery (:150-523).

Design as implemented: detach publishes length=0 seq_cst plus a SEPARATE
detached flag (side table, :219-303 — the spec'd ArrayBuffer member awaits a
header change); the base word is left intact and the contents' ownership
moves into the per-server quarantine entry; a heap §10 stop clears/poisons
the stale base under quiescence and frees the mapping. The side table also
arbitrates racing detach/transfer writers (exactly one wins the test-and-set,
:288), with generation numbers against ABA on recycled ArrayBuffer*.
Transfer is rewritten COPY + DETACH (:1000-1050), including the
resizable-source arm that allocates with the source's maxByteLength
reservation before the memcpy (:1025; r14 F2). `notifyDetaching` watchpoints
fire as landed (:1202) — hoisted-vector code jettisons, and the quarantine
covers code that raced the jettison.

Why needs-test rather than immune:

1. **Recorded sign-off gap** (in-tree, :219-231): read-side `isDetached()`
   callers OUTSIDE ArrayBuffer.cpp still evaluate `!m_data` during the
   detach→stop window. The in-file argument that this is stale-but-safe
   (length already 0, every guarded fast path bounds-fails, mapping alive
   until the stop) is plausible but exactly the kind of claim a
   susceptibility test should hold to account — any out-of-file caller that
   uses `isDetached()` as its ONLY guard (no length check) before computing
   a base-relative pointer would be live during the window.
2. The writer-writer story (one winner moves m_destructor, no double
   enqueue, :1104) and the ~ArrayBuffer-between-detach-and-stop arm
   (`unregisterDetachedArrayBufferGILOff`, :303 / :1544) are lifecycle races
   only a storm finds.
3. The annex's transferee arms (transfer of a RESIZABLE source under reader
   storm; transferee resized up to maxByteLength after transfer;
   transfer(newByteLength > byteLength)) have no corpus coverage.

Test: `mc-grow-buffer-storm.js` arms DETACH / TRANSFER / TRANSFER-RESIZABLE.

## S5a. Wasm grow, Signaling / reserved-VA — immune-by-construction

`Wasm::Memory::grow` MemoryMode::Signaling arm (wasm/WasmMemory.cpp:359-381):
VA pre-reserved, grow = `OSAllocator::protect` of the next pages of the same
mapping + `updateSize` (seq_cst). The buffer-side refresh
(`ArrayBufferContents::refreshAfterWasmMemoryGrow` gilOff branch,
runtime/ArrayBuffer.cpp:1669-1675) keeps `m_data` unchanged and
release-publishes the larger `m_sizeInBytes` AFTER the handle swap — the
only published mutation is the larger length over committed pages of an
immutable base. Same shape and argument as S2 (annex N6 arm 4, reserved-VA
leg). Shared wasm memories route through S2's `SharedArrayBufferContents`.
Covered as a storm arm in `mc-grow-buffer-storm.js` (regression guard).

## S5b. Wasm grow, BoundsChecking relocation — FIX LANDED (premise-gated)

**Updated 2026-06-18: stop conduction LANDED** in `Memory::grow`'s
BoundsChecking arm (wasm/WasmMemory.cpp, gilOffProcess-gated
`JSThreadsSafepoint::stopTheWorldAndRun` around the handle swap +
`success()` publication; CVE-AUDIT Tier-B B4). The U-T13 OPEN DEPENDENCY
comments in runtime/ArrayBuffer.cpp are discharged. This is the class
exemplar (CVE-2017-15399 shape); the section below retains the hole
description for review-against-spec and so the gate test's oracle is
documented. Verdict: structural fix verified by code review against
SPEC-heap §10 / annex N6 arm 4; **executable verification is premise-gated**
on the GIL-off wasm refusal — the gate tests must PASS once that refusal
lifts (no longer expected-FAIL).

The hole, precisely:

- A non-shared `WebAssembly.Memory` that did not get fast memory (always,
  under `--useWasmFastMemory=0`; opportunistically when the fast-memory pool
  is exhausted) is MemoryMode::BoundsChecking with NO VA reservation: the
  handle is sized exactly `initialBytes` (wasm/WasmMemory.cpp:212-217).
- `Memory::grow` on such a memory RELOCATES: fresh Gigacage allocation,
  memcpy, handle swap (wasm/WasmMemory.cpp:337-358). Annex N6 arm 4 requires
  this relocation to run **under a heap §10 stop** ("grow relocate:
  stop-separated, no concurrent reader" — UNGIL-HANDOUT.md:2918-2919),
  with the old mapping quarantined to the NEXT stop for captured/hoisted
  bases.
- The stop conduction IS NOW implemented (B4, 2026-06-18): `Memory::grow`'s
  BoundsChecking arm wraps `transferAnchors` + handle swap + `success()`
  (which fans out `updateCachedMemories` + `growSuccessCallback` ->
  `refreshAfterWasmMemoryGrow` + per-view `refreshVector`) inside
  `JSThreadsSafepoint::stopTheWorldAndRun`, gated `g_jscConfig.gilOffProcess`
  so flag-off is byte-identical. Allocation + memcpy stay outside the stop.
  The in-tree OPEN DEPENDENCY comments at runtime/ArrayBuffer.cpp have been
  rewritten to record the discharge.
- The keepalive half remains as designed:
  `quarantineStaleWasmMappingGILOff` (:343-348, called from
  `refreshAfterWasmMemoryGrow` :1673-1674) keeps the OLD handle mapped until
  a stop, so the torn pair {pre-grow length, pre-grow base} is safe. The
  complementary pair **{post-grow length, pre-grow base} is unprotected**.

Concrete interleaving (GIL-off):

1. Main thread holds a resizable buffer over a BoundsChecking memory
   (`mem.toResizableBuffer()`), with a length-tracking view shared to a
   spawned thread.
2. Main calls `mem.grow(k)`: new mapping allocated, handle swapped,
   `refreshAfterWasmMemoryGrow` publishes the larger length; the per-view
   `m_vector` repointing loop (`ArrayBuffer::refreshAfterWasmMemoryGrow`,
   :921-944) then walks incoming references and `refreshVector`s each view —
   plain stores, not synchronized with foreign readers.
3. The spawned reader's fast path loads view length (sees post-grow value),
   bounds-checks an index in [oldLen, newLen), then loads `m_vector` (still
   the pre-grow base — the two loads are unordered and the refresh loop may
   not have reached this view). Access lands past the END of the old
   mapping: the quarantine keeps [base, oldLen) alive, but [oldLen, newLen)
   of the OLD mapping was never mapped (handle sized exactly oldLen).
   Result: OOB read/write into whatever neighbors the old allocation in the
   primitive Gigacage — exploitable-shaped, not just a benign crash.

Spawned-thread reachability is real despite §I's wasm-execution refusal:
SPEC-ungil §I refuses wasm EXECUTION on spawned threads only; views over a
main-created memory "reach spawned threads as plain TA accesses"
(UNGIL-HANDOUT.md:2923). Plain JS `memory.grow()`/`buffer.resize()` on
the main thread plus a spawned TA reader is the entire recipe.

Governing invariant violated: annex N6 PRINCIPLE/INVARIANT ("a racing reader
must NEVER pair a passing length with an unmapped-or-short base") — the
design closes this with the §10 stop; the implementation gap leaves the
length publication ordered but the base swap unfenced against it.

Disposition: B4 fix landed in `Memory::grow`'s BoundsChecking arm
(stopTheWorldAndRun around publication, gilOffProcess-gated). The targeted
susceptibility test `mc-grow-wasm-relocating-grow.js` is now a REGRESSION
GATE: premise-SKIPs while the GIL-off wasm refusal is in force, and must
PASS (not crash/ASAN-fault) once the refusal lifts. The alternative fix
shape — forbid the no-reservation mode under gilOffProcess by always
reserving maxByteLength VA — remains annex-compatible (converts S5b into
S5a) and is a valid future simplification, but is not required for
correctness now that the stop conduction is in place.

## S6. JSArrayBufferView mode transitions — immune-by-construction

`slowDownAndWasteMemory` / `tryBecomeWasteful` paths
(runtime/JSArrayBufferView.cpp:265, :327): Fast/Oversize → Wasteful
transitions swap `m_vector` from cell-interior (or oversize-malloc'd)
storage to a fresh `ArrayBufferContents` copy, under the view's cell lock
(`Locker locker { cellLock() }` at both sites). Audit row R11
(SPEC-ungil-audit-N7.md:202) rules these COVERED §N.6 + PHASE-1-IN-TREE.

Why the mechanism cannot occur: `m_length` is unchanged by the transition,
and BOTH possible bases a lock-free reader can observe are mapped for at
least that length — the old base is either inside the still-live cell
(FastTypedArray inline storage; GC-reachable while any frame holds the view)
or the oversize allocation freed only by the finalizer, and the new base is
the fresh contents. No {passing length, short base} pair exists; a stale
base reads stale bytes, which is the SAB-semantics staleness the model
already permits. Additionally, a FastTypedArray's storage is only ever
swapped by this cell-locked path, and `refreshVector` (the other m_vector
writer) applies to wasm-backed wasteful views handled under S5.

Adversarial residue: the half of R11 marked PHASE-1-IN-TREE means mode
transitions still assume cell-lock discipline at every m_vector/m_mode
writer post-ungil; the S5b test doubles as the watchdog for the one writer
outside the cell lock (`refreshVector`).

## S7. Half-built instance publication — immune-by-construction

The exemplar leg "half-built instance retains the old base" needs a
partially initialized view/buffer to be reachable by a second thread. By
construction it is not:

- The transferee of `transferTo` is filled in (`m_data`, `m_sizeInBytes`,
  max-byte-length stamping, memcpy) on a thread-local `ArrayBufferContents`
  BEFORE any JSArrayBuffer wrapper exists; the wrapper is created only
  afterwards (runtime/ArrayBuffer.cpp:1020-1031;
  runtime/JSArrayBufferPrototype.cpp:330-346 creates the result object after
  the transfer + thread-local resize). No concurrent reader can hold it.
- New views initialize `m_vector`/`m_length`/`m_mode` in their constructors
  before the cell is published to any shared location; cross-thread cell
  publication ordering is OM's M7 release-publish rule
  (SPEC-objectmodel §M7 — readers M7-order or re-check structureID), the
  same rule every other cell relies on.

## S8. JIT-hoisted TA base/length — needs-test

Per-tier TA fast paths are the READ side of every arm above; annex N6 names
them IM rows ("JSArrayBufferView + per-tier TA fast paths (length-load
sites)", UNGIL-HANDOUT.md:2932-2935, owner U-T13). The design covers them
via (a) the unordered-two-loads principle (so no per-site fencing is
required), (b) detach jettison through `m_detachingWatchpointSet.fireAll`
(runtime/ArrayBuffer.cpp:1202) with the quarantine absorbing the
jettison race, and (c) the §10-stop lifetime rule. But the per-tier
re-pointing work is open implementation surface owned by U-T13 — e.g. DFG
code that speculated a fixed length for a resizable view, or hoisted a
vector pointer across a call that can detach. Until U-T13 lands and is
audited, the honest verdict is needs-test: the storm test must run JIT-on
(it loops hot enough to tier up) so these sites are exercised, not just the
LLInt paths.

---

## Test index

| Test | Arms | Determinism |
|------|------|-------------|
| JSTests/threads/cve/mc-grow-buffer-storm.js | S2 GSAB grow, S3 RAB shrink/re-grow (+GC pressure), S4 detach/transfer/transfer-resizable, S5a signaling wasm grow, S8 (runs hot, JIT-on) | invariant-asserting storm; amplifier-ready (race-amplifier hooks at the §4.4/N6 choke points per AMPLIFIER.md apply); failures = crash/ASAN/assert |
| JSTests/threads/cve/mc-grow-wasm-relocating-grow.js | S5b relocating wasm grow vs spawned TA readers | targeted; §10 stop conduction LANDED (B4); premise-SKIP under GIL-off wasm refusal, must PASS once refusal lifts |

Both tests carry `//@ requireOptions` headers naming their flags and MUST NOT
be run against the current mid-bring-up tree; they are written for the
post-ungil execution pass (thread-cve-audit / thread-fix).
