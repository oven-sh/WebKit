# MC-TEAR — Torn multi-word publication: surface map for the JSC threads work

Status: defensive audit artifact, 2026-06-07. Class definition: CVE-AUDIT.md "MC-TEAR".
Mechanism (web-derived, treated as data): a logically-atomic multi-word value
(type ptr + data ptr; base + length; StructureID + butterfly) written as multiple plain
stores; a racing reader pairs halves of different writes, yielding type confusion or OOB
at chosen extent. Exemplars: StalkR 2015 Go interface-tear PC-control exploit, Go
slice-header tearing, ECMA-335 §I.12.6.6 torn longs/structs.

Tree state at audit time: branch `jarred/threads`; SPEC-objectmodel rev 14 +
SPEC-ungil/UNGIL-HANDOUT rev 32 implementation in progress. ConcurrentButterfly and the
annex-N6 ArrayBuffer quarantine are LANDED; handout §N.2 (rope release-CAS), §N.5
(generator claim CAS), §N.3 (date-cache bypass) are NOT yet in tree. Verdicts below are
split GIL-on (phase 1, JSLock serializes all JS) vs GIL-off (post-ungil). Under the
phase-1 GIL every surface is trivially immune (one mutator at a time); all verdicts are
for GIL-off unless stated.

Method: for each surface, the logically-atomic tuple, the writer's publication protocol
(file:line), the reader's pairing discipline, the governing SPEC invariant, and an
adversarial argument for both torn-pairing directions (new-A/old-B and old-A/new-B).

Verdict key: **immune** = immune-by-construction (cite why the mechanism cannot occur);
**needs-test** = test written under `JSTests/threads/cve/` (executed post-ungil);
**suspected** = susceptible-suspected, precise hole stated.

| # | Surface | Tuple | Verdict (GIL-off) |
|---|---------|-------|-------------------|
| S1 | JSCell header + butterfly word | {StructureID/indexingType, tagged butterfly ptr} | immune |
| S2 | Flat butterfly | {base ptr, vectorLength/publicLength} | immune |
| S3 | Segmented spine | {spine ptr, publicLength, vectorLength} | immune |
| S4 | TypedArray/ArrayBuffer | {base ptr, byteLength} | needs-test |
| S5 | JSRopeString resolution | {flags/fiber0 word, length, fiber1/2, StringImpl ref} | suspected (unlanded §N.2) + needs-test |
| S6 | Generator/async internal fields | {state word, frame/field words} | suspected (unlanded §N.5) + needs-test |
| S7 | DateInstance GregorianDateTime cache | {RefPtr m_data; cachedForMS key + >8B struct} | suspected (unlanded §N.3) + needs-test |
| S8 | Property/element value slots | one EncodedJSValue / raw double | immune |
| S9 | Structure transition/property tables | table internals | immune |
| S10 | JIT-emitted {structure check, butterfly load} pairs | same as S1, compiled | immune (conditional, per-tier) |
| S11 | GetterSetter | {getter, setter} cell ptrs | immune (memory-safety sense; semantic raciness accepted) |

---

## S1. {JSCell header (StructureID, indexingType), tagged butterfly word} — the exact Go-interface analog

This is the canonical "type ptr + data ptr" pair: the header tells readers how to
interpret the storage the butterfly word points at.

- Tuple: bytes [0,16) of every JSObjectWithButterfly cell — 8B header (m_structureID,
  indexingType, type/flags) + 8B tagged butterfly word (SPEC-objectmodel §3.0, I1).
- Writers:
  - Non-E4 transitions publish {new semantic header, new tagged butterfly} in ONE
    128-bit seq_cst DCAS — `dcasHeaderAndButterfly`,
    `Source/JavaScriptCore/runtime/ConcurrentButterfly.h:337` (I5, M3); inline
    `cmpxchg16b`/`casp` only, lock-based 16B CAS forbidden at startup (I32,
    ConcurrentButterfly.h:381).
  - Element-storage resizes change ONLY the butterfly word, via one 64-bit CAS —
    `casButterfly`, ConcurrentButterfly.h:475 (I16/I17): never a multi-store.
  - PreciseAllocation cells (8-mod-16 bases, 16B CAS would fault): I36/M8 fenced nuke
    order under the cell lock — structureID CAS->nuke, storeStoreFence, 64-bit
    butterfly CAS, semantic header bytes, fence, new-ID store —
    `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:725-756` (conversion) and
    `:1225-1248` (segmented transition).
  - E4 owner lock-free transitions keep today's sequence (value, nuke, butterfly,
    StructureID — M5) gated on BOTH TTL watchpoint sets valid+watched AND instance tag
    == (currentTID,0) AND !PA (§5 E4, I15): the only threads that could pair halves are
    foreign readers, addressed below.
- Readers:
  - GC visit: dependency-ordered chain + late re-check, didRace on nuked or drifted
    StructureID — `Source/JavaScriptCore/runtime/JSObject.cpp:437-477` (the exhaustive
    interleaving table at :241-380 enumerates every pairing and disposition).
  - Runtime concurrent paths: M7 — structureID load ordered BEFORE the butterfly-word
    load via Dependency (`JSObject.h:1582 fencedButterfly`), loadLoadFence
    (`JSObject.cpp:515-527`, `JSObject.h:892` locationForOutOfLineOffsetConcurrent), or
    mandatory re-check on nuke-tolerant paths (M7(c)); slow paths spin on nuked IDs
    (M5, StructureID::tryDecode).

Adversarial pairing analysis (both directions):

1. *Reader pairs NEW StructureID with OLD (smaller) butterfly* — the OOB direction
   (I24's "stale-smaller-storage OOB"). Excluded: (a) for DCAS writers both halves are
   one atomic 16B store — no window exists; (b) for the M8 PA order the new-ID store is
   fence-separated AFTER the butterfly publication (ConcurrentButterfly.cpp:732-756), so
   a reader whose M7-ordered first load returns the new un-nuked ID observes the new
   butterfly on the second load; (c) for E4, the StructureID store is last in program
   order and the path is restricted to non-PA MarkedBlock cells where the prior
   butterfly store is ordered by the existing fenced protocol (M8 forces
   m_mutatorShouldBeFenced for heap lifetime — the unfenced branch is compiled out
   flag-on).
2. *Reader pairs OLD StructureID with NEW butterfly* — the type-confusion direction
   (interpret a spine as a flat pointer, or new layout with old offsets). Two
   sub-cases:
   - Storage-KIND confusion is impossible because the kind is encoded in the data word
     itself: SW bit + TID tag (§2; TID==notTTLTID <=> payload is a spine, I3). The
     dispatch regime is decoded from the loaded butterfly word, not from the header —
     i.e. the spec moved the "type" half of the pair INTO the same 64-bit word as the
     pointer, which is precisely the fix shape for the Go interface tear. A reader
     holding a stale Structure still decodes the new word's regime correctly.
   - Offset confusion (old structure's PropertyOffsets against new storage) is benign
     by I8: every flat->segmented and grow publication reproduces every pre-existing
     slot at its flat address (address equations §4.1, debug-checked); shrink/relayout
     operations (deletes, attribute reconfiguration, dictionary flatten, AS innards)
     are cell-locked (L3/L4/L5) or per-event STW (F3, §4.6), and deleted out-of-line
     slots are quarantined until an owning-heap epoch bump (I18/D1: a tardy reader sees
     old value or jsUndefined, never a freed slot). Offset-CHANGING transitions nuke
     first; nuke-tolerant readers re-check (M7(c)) and everything else spins.
3. *Torn 8-byte halves of one word*: both words are individually 8B-aligned (I1);
   single-word loads/stores cannot tear on x86-64/arm64.

Residual risk consumed elsewhere: windows where a reader holds a decoded
PropertyOffset across a poll/park are I34 (audit manifest 7b) — that is MC-DF
territory, not a publication tear. **Verdict: immune-by-construction.** Existing
coverage: `JSTests/threads/objectmodel/i03-*` (transition storms, stale-spine readers,
PA races, b2 stay-flat vs SW flip), `JSTests/threads/races/transition-vs-read.js`.

## S2. Flat butterfly {base, length} — the Go-slice analog

- Tuple: butterfly pointer + vectorLength/publicLength.
- Why immune: the lengths live INSIDE the pointed-to allocation (IndexingHeader at
  B-8, `Source/JavaScriptCore/runtime/Butterfly.h:141-146`), so base and bounds travel
  in one 64-bit pointer publication (`casButterfly`, I16/I17) — the published snapshot
  is internally consistent by construction. This is exactly the known fix for Go
  slice-header tearing (make the header reachable through one word).
- Superseded storage is never freed or reused while any stale reader can hold it: I7
  (conservative scan; aliased flat base spine-recorded and marked every visit,
  JSObject.cpp:103-108) — so a stale {base,length} pair is stale-but-mapped, never
  dangling.
- The one in-place mutation, T5 vectorLength-only growth, is cell-locked, owner-only,
  monotonic, and fenced (§4.4 T5); a tardy reader pairs the old base with the old
  SMALLER length — under-read, never over-read.
- **Verdict: immune-by-construction.** Coverage: `i03-array-resize-cas.js`,
  `i03-t5-racing-growers.js`, `i03-t1-vs-sw-flip.js`.

## S3. Segmented spine {spine, publicLength, vectorLength}

- The spec explicitly models the torn pair: publicLength (fragment 0 slot 0) is SHARED
  by every spine the object ever publishes, so "newer length half + older spine half"
  is a LEGAL state, and C4/I33 require every access to bound by
  min(publicLength, loadedSpine->vectorLength), treating [VL, publicLength) as holes;
  out-of-range out-of-line offsets re-load and re-dispatch. Spines are immutable after
  publication (I6) — no torn spine fields possible (§4.5-6 "spine immutability => no
  torn spine").
- **Verdict: immune-by-construction.** Coverage:
  `i03-stale-spine-reader-vs-grow.js`, `i03-visit-range-outofline.js`.

## S4. TypedArray/ArrayBuffer {base, length} under detach/transfer/resize/grow

- Tuple: `JSArrayBufferView::{m_vector, m_length}`
  (`Source/JavaScriptCore/runtime/JSArrayBufferView.h:248-249,281-309`) and
  `ArrayBufferContents::{m_data, m_sizeInBytes}`. The handout states the principle
  verbatim (§N.6 / annex N6, BINDING): "every tier's TA fast path loads LENGTH,
  bounds-checks, then loads BASE; the reader's two loads carry no ordering, so store
  ordering alone cannot close a torn two-word read."
- Design (and landed write side, `Source/JavaScriptCore/runtime/ArrayBuffer.cpp`):
  the INVARIANT is that any observable base maps a region >= every length still
  observable against it; enforced by making the base half effectively immutable per
  mapping lifetime and quarantining retirements to heap-§10 stop quiescence:
  - detach: length=0 seq_cst + separate detached flag; base word NOT cleared; contents
    move into the per-server quarantine, freed only at a stop (ArrayBuffer.cpp:151-300,
    995-1016).
  - transfer: rewritten COPY + DETACH (no live-transferee aliasing of a
    quarantine-visible mapping).
  - shrink: publish smaller length seq_cst; tail pages stay committed until stop
    retirement (ArrayBuffer.cpp:1155+).
  - grow: base immutable (reserved VA); commit pages THEN release-publish the larger
    length (ArrayBuffer.cpp:1242); relocating grows only under a stop.
  The annex's torn-pair table shows every cross pairing is stale-but-safe or
  bounds-fails.
- Why needs-test rather than immune: the write side is landed, but the *read side* is
  every TA/DataView fast path in five tiers (LLInt/Baseline/DFG/FTL/runtime) plus
  hoisted-vector JIT code reached only through jettison+quarantine; the invariant is
  global over all of them and over wasm grow. The spec itself charters the U28
  amplifier. This is the highest-value MC-TEAR test target.
- **Verdict: needs-test** ->
  `JSTests/threads/cve/mc-tear-typedarray-detach-grow-shrink.js` (amplifier-ready;
  deterministic value-membership oracle).

## S5. JSRopeString resolution {flags/fiber0, length, fibers, StringImpl}

- Ruled protocol (handout §N.2, BINDING): lock-FREE — resolver computes into a fresh
  buffer and publishes by ONE release-CAS of the fiber0/flags word; losers discard and
  re-read; readers load-acquire. The design is tear-free because (a) the isRope flag
  bits and fiber0/value pointer share ONE 8-byte word
  (`JSString.h:626-628`: offsetOfFlags == offsetOfFiber0 == offsetOfValue), so
  "type bit + data ptr" publish atomically; (b) length and the trailing fibers are
  never cleared post-publication
  (`Source/JavaScriptCore/runtime/JSStringInlines.h:390`), so a stale isRope=true
  reader still walks valid fibers; (c) length is immutable from construction.
- Suspected hole (precise): §N.2 is NOT landed. Current
  `JSRopeString::convertToNonRope` (`JSStringInlines.h:382-393`) is a storeStoreFence +
  plain placement-new of the String into the fiber0 word — no CAS, no loser arm. Two
  GIL-off threads resolving the same rope concurrently both run
  `new (&uninitializedValueInternal()) String(...)` on the SAME word: a double
  ref-adopting store. One StringImpl ref is silently overwritten (leak) and — worse —
  both threads then run `vm.heap.reportExtraMemoryAllocated` / destruction accounting
  against it; if either path ever releases the clobbered ref (e.g. via
  `notifyNeedsDestruction` teardown of the loser's impl), the published string is a
  use-after-free reachable from JS. Readers also load the word plain (no acquire)
  while only a storeStoreFence orders the impl's contents — dependency-ordered loads
  make this safe in practice on arm64, but the spec's load-acquire is also unlanded.
- **Verdict: susceptible-suspected (unlanded §N.2) + needs-test** ->
  `JSTests/threads/cve/mc-tear-rope-resolve-race.js`. Re-run after §N.2 lands; the
  test is the acceptance check.

## S6. Generator/async-function internal fields {state, frame}

- Ruled protocol (handout §N.5, BINDING): single-word resume-claim CAS
  SuspendedX->Running on the state field; unclaim transitions are store-RELEASE in ALL
  tiers — "plain stores torn frames on arm64" is the spec's own statement of the
  MC-TEAR hazard: without the release/acquire pairing a second resumer pairs the new
  state word with stale frame words (a torn {state, frame} multi-word publication) and
  resumes into a half-written frame.
- Suspected hole (precise): the twin intrinsics @atomicInternalFieldClaim/Publish are
  NOT in tree (`Source/JavaScriptCore/builtins/GeneratorPrototype.js` still does the
  landed plain check-then-store around :36/:60/:77/:91). GIL-off today, two threads
  alternating `gen.next()` can both pass the state check and both write frame state;
  resumption can observe a frame whose words come from two different suspensions —
  type confusion on the recovered IR values.
- **Verdict: susceptible-suspected (unlanded §N.5) + needs-test** ->
  `JSTests/threads/cve/mc-tear-generator-resume.js` (the spec's own amplifier shape:
  ping-pong next() round-tripping a counter through frame state).
- **STATUS UPDATE (post-landing review round): §N.5 PARTIAL.** Landed: sync-generator
  + iterator-helper resume-claim (owner-token CAS host hooks), the yield-side unclaim
  relocation (now fail-closed, and extended to async-function/async-generator/module
  bodies via the generatorRegister()-based validation), AND the r15 F1 release half —
  gilOffProcess, `op_put_internal_field` is store-RELEASE in ALL tiers (store-store
  fence before the field store: LLInt64/Baseline/DFG/FTL), so the relocated unclaim
  actually publishes the frame saves on arm64. OPEN (recorded deferral, history
  "§N.5 LANDED SHAPE" entry): the ASYNC GENERATOR resume-head claim —
  `AsyncGeneratorPrototype.js` still does the plain check-then-store + plain queue
  mutations; owed an async clone of mc-prim-generator-resume-claim before that arm
  can close. arm64/TSAN verification of the fence pairing also still owed
  (SPEC-ungil-audit-N7.md:239).

## S7. DateInstance GregorianDateTime cache

- Ruled protocol (handout §N.3): cache BYPASSED GIL-off — "the cached pair is >8 bytes,
  not CASable"; m_data lazy alloc CAS-published.
- Suspected hole (precise): not landed.
  `Source/JavaScriptCore/runtime/DateInstance.cpp:44-73` still does
  `m_data = cache.cachedDateInstanceData(milli)` (plain RefPtr store — racing stores
  tear the refcount discipline: the overwritten RefPtr's deref and the winner's ref
  race => over-release/UAF of DateInstanceData) and then fills the >8B
  {m_gregorianDateTimeCachedForMS, m_cachedGregorianDateTime} pair with plain stores —
  a reader on `DateInstance.h:64-65` can pair a matching cachedForMS key from write A
  with date components from write B (wrong-value tear; the m_data RefPtr race is the
  memory-unsafe part).
- **Verdict: susceptible-suspected (unlanded §N.3) + needs-test** ->
  `JSTests/threads/cve/mc-tear-date-cache.js`.

## S8. Property/element value slots

- One EncodedJSValue per slot, 8B-aligned; ContiguousDouble slots are raw aligned
  doubles (§4.7 R-DOUBLE: tear-free, SAB semantics). I21 names "torn JSValues" a
  stress target; no multi-word value representation exists on 64-bit. Shape changes
  TOUCHING Double on shared objects are per-event STW (I28) — the relabel-vs-raw-bits
  type confusion (the nearest analog of an ECMA-335 torn struct) cannot interleave
  with a reader holding the old shape.
- **Verdict: immune-by-construction.** Coverage: `i03-shared-double.js`.

## S9. Structure transition/property tables

- All mutator access lock-serialized flag-on (L6/I37: lookups via the m_lock-holding
  Concurrently variant; steal/clone/materialize under the source's m_lock; uncached
  walks hold m_lock) — "no ... torn table reads under N-thread same-shape adds" is
  I37's literal text.
- **Verdict: immune-by-construction.** Coverage: `i03-i37-same-shape-add-storm.js`.

## S10. JIT-emitted {structure check, butterfly load} pairs

- Compiled fast paths are the same S1 tuple read from machine code: SPEC-jit consumes
  E1-E4/M7 (E3: load tagged butterfly, mask, M7 ordering on arm64; E2/D9/CS5: writes
  keep the fused TID compare); elision is legal only while the TTL watchpoint set is
  valid AND watched (I14), and the sets fire only inside STW (I13/M6) with
  invalidation/jettison/ISB handled at jit §5.6 — so compiled code can never run a
  pre-tear assumption across the publication that falsifies it. The conditionality:
  immunity holds per-tier only where each tier's emitted sequence is M7-conforming;
  that is SPEC-jit's verification ladder + amplifier scope, not a new MC-TEAR test
  (a JS-level test cannot distinguish the tier).
- **Verdict: immune-by-construction (conditional on per-tier E3/M7 conformance,
  verified by the jit ladder/TSAN targets).**

## S11. GetterSetter {getter, setter}

- A logically-paired two-pointer cell. The property slot itself holds ONE pointer to
  the GetterSetter (single-word publication); attribute reconfiguration is cell-locked
  (L4). A lock-free reader racing a redefinition can pair g(new) with s(old) across
  two separate calls — but each word is independently a valid live cell pointer, so
  the outcome is a semantic interleaving, not type confusion or OOB. MC-TEAR's
  memory-safety outcome cannot occur.
- **Verdict: immune-by-construction (memory-safety sense); semantic mixed-pair
  raciness is an accepted SD-class behavior.**

---

## Test inventory (this audit)

All under `JSTests/threads/cve/`, written now, EXECUTED post-ungil (tree is
mid-bring-up; do not run):

| Test | Surface | Oracle |
|------|---------|--------|
| `mc-tear-typedarray-detach-grow-shrink.js` | S4 | value-membership + no crash under detach/transfer/resize storm |
| `mc-tear-rope-resolve-race.js` | S5 | N racing resolvers all observe the exact concatenation; no crash |
| `mc-tear-generator-resume.js` | S6 | ping-pong resume observes only predecessor-published frame values or the serial TypeError |
| `mc-tear-date-cache.js` | S7 | every component read belongs to one of the two written timestamps; no crash |

Pre-existing suites already covering immune surfaces: S1-S3/S8/S9 ->
`JSTests/threads/objectmodel/i03-*.js`, `JSTests/threads/races/transition-vs-*.js`.
