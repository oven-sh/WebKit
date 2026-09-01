# MC-DF — Double-fetch / TOCTOU of shared data: mapping to our threads surface

Mechanism class (from the catalog): a trusted consumer (parser, compiler,
bounds check, IC fast path) reads attacker-shared memory or metadata more
than once — or validates on one fetch and acts on another — while a second
agent writes between the fetches. Exemplars: CVE-2017-5116 (V8 wasm bytes in
SAB raced by a Worker between validate and compile), CVE-2018-4222 (JSC
detach-window OOB), CVE-2026-5893 (V8 validate-vs-use race, per third-party
advisory), the Bochspwn kernel double-fetch corpus, Watson WOOT'07.

Date: 2026-06-07. Tree: jarred/threads (phase 1 GIL'd complete, ungil in
progress). Specs consulted: SPEC-objectmodel rev 14, SPEC-ungil + annexes
N6/N7, UNGIL-HANDOUT rev 32, SPEC-jit rev 12. Read-only audit; tests under
JSTests/threads/cve/ are written but NOT executed (bring-up loop owns the
build); they run post-ungil via thread-cve-audit.

Why this class matters more for us than for SAB-era engines: in the SAB
model only ArrayBuffer BYTES are shared, so the double-fetch surface is
typed-array data and wasm bytes. Under --useJSThreads the entire object
graph is shared — every length word, structureID, butterfly pointer,
property table and IC input is "attacker-shared metadata". The specs were
written with exactly this class in view (the C4/I33/I24/I34/N6 machinery IS
anti-double-fetch machinery), which is why most verdicts below are
immune-by-construction — but each one is argued adversarially, and the
binding-but-mechanism-shaped ones get tests anyway.

---

## S1. Wasm module bytes raced by a second thread (CVE-2017-5116 analog)

**Surface.** Main thread calls `new WebAssembly.Module(buf)` /
`WebAssembly.validate/compile/instantiate(buf)` where `buf` is a TA/AB whose
bytes a spawned Thread can reach (shared heap: any TA stored on a shared
object). Spawned threads cannot run wasm themselves (SD7 refusal,
WebAssemblyModuleConstructor.cpp:294 and siblings; SPEC-ungil §I) — but SD7
does NOT block the exemplar: the racing agent only needs plain TA writes;
the compile happens on main.

**Mechanism check.** All four byte-consuming entry points snapshot first:

- Source/JavaScriptCore/wasm/js/WebAssemblyModuleConstructor.cpp:301
- Source/JavaScriptCore/wasm/js/JSWebAssembly.cpp:155, 281, 422

each via `createSourceBufferFromValue`
(Source/JavaScriptCore/wasm/js/JSWebAssemblyHelpers.h:165-185), which copies
the span into a private `Vector<uint8_t>` BEFORE any validation; validator
and every compile tier consume only the copy. There is no second fetch of
attacker-visible bytes — the V8 bug shape (validate from SAB, compile from
SAB) cannot occur.

**Adversarial self-check.**
(a) The copy itself does two loads — `vector()/data()` base and
`byteLength()` (JSWebAssemblyHelpers.h:160-162) — so a concurrent
detach/shrink during the memcpy is a torn {length, base} pair. That pair is
governed by annex N6 (S2 below): any observable base maps a region >= any
still-observable length, so the memcpy reads stale-but-mapped bytes; the
resulting copy may be torn GARBAGE but is consumed coherently (worst case
CompileError or a validly-compiled module of torn-but-valid bytes — the
attacker can already produce either by writing the bytes earlier).
(b) `JSSourceCode` arm: provider data is engine-owned, not script-shared.
(c) Streaming compile (`JSWebAssembly.cpp` streaming path) chunks through
the same copied-Vector plumbing; no zero-copy path exists in this tree.

**Verdict: immune-by-construction** — the copy-once snapshot is the
load-bearing line; SPEC anchor = annex N6 invariant for the snapshot's own
torn pair. **Plus tripwire test** (the copy is one "optimization" away from
the CVE): `JSTests/threads/cve/mc-df-wasm-compile-race.js` — flips a
const-immediate byte between two valid encodings under main-thread
compile+run loop; oracle: result in {1,2} or CompileError, never anything
else, never a crash.

## S2. TypedArray/DataView length-then-base vs detach/transfer/resize

**Surface.** Every tier's TA/DataView fast path loads LENGTH, bounds-checks,
then loads BASE — two fetches with no reader-side ordering. Racing writer
arms: `ArrayBuffer::detach` (runtime/ArrayBuffer.cpp:525 region),
`transferTo` (:498), `resize` down (:628-639), wasm/resizable grow. This is
the purest MC-DF instance we own, and the direct analog of CVE-2018-4222
and the Bochspwn pattern.

**Governing protocol.** SPEC-ungil annex N6 (BINDING; UNGIL-HANDOUT §N.6,
lines 2815-2925): invariant — a racing reader must NEVER pair a passing
length with an unmapped-or-short base; any observable base points at a
mapping mapped and sized >= every length still observable against it.
Mechanisms: detach publishes length=0 + a separate detached FLAG and moves
the contents into a per-server quarantine retired only at a heap §10 stop
(no JS/JIT fast path straddles a stop, so no pre-retirement length survives
retirement); shrink defers `freePhysicalBytes`/protect of the tail to the
same quarantine; grow keeps the base IMMUTABLE (commit pages into reserved
VA, then release-publish the larger length); relocating grow runs under a
stop. Implemented: runtime/ArrayBuffer.cpp:151 ("annex N6: per-server
ArrayBuffer mapping quarantine") and :184 (detached-flag side table).
The torn-pair table in N6 enumerates every {length, base} combination and
shows each is stale-but-safe or bounds-fails.

**Adversarial self-check.** The one combination N6 leans hardest on is
shrink-then-regrow: a reader's stale large length vs pages whose tail
entry was consumed/cancelled by the re-grow — N6 handles it under
`memoryHandle->lock()` with zeroFill, so the bytes are committed (reads 0,
not garbage from another allocation). The transferee-aliasing hole was
explicitly considered and the handle-move design REJECTED for it (r14 note
in N6 arm 2). Residual risk is implementation fidelity, not design.

**Verdict: needs-test** (design immune, mechanism is exactly MC-DF, and the
chartered U28 amplifier is broader/less CVE-shaped):
`JSTests/threads/cve/mc-df-ta-detach-resize.js` — sentinel-byte oracle
(SENTINEL | 0 | undefined; anything else = torn pair) under a
shrink/regrow/transfer/detach storm against striding TA + DataView readers.

## S3. Segmented-butterfly indexed bounds: publicLength vs spine

**Surface.** Indexed access on a shared (segmented) object: bounds-check
against `publicLength` (fragment 0 slot 0 — SHARED by every spine the
object ever publishes, SPEC-objectmodel C4) then dereference fragments of a
spine loaded by a separate fetch of the tagged butterfly word. The
double-fetch hole would be: length from a NEW spine's era, fragments from
an OLD smaller spine => deref past that spine's fragments / the C2 tail.

**Governing protocol.** SPEC-objectmodel C4 + I33: every segmented indexed
access and GC visit bounds by min(publicLength, the SAME loaded spine's
vectorLength); [vectorLength, publicLength) reads as holes; out-of-line
accesses bound by 4*outOfLineFragmentCount with OOR => acquire-re-load and
re-dispatch. Spines are immutable after publication (I6) and superseded
storage is never freed while reachable from stale stacks (I7, conservative
scan), so even a maximally stale spine dereferences only its own live
fragments. Implemented as bounded accessors that callers cannot misuse:
runtime/ConcurrentButterfly.h:405-421 ("variants of the accessors above, so
callers cannot violate the I33/C4 bound") and
runtime/ConcurrentButterfly.cpp:113-157 (stale spine => nullptr =>
re-dispatch). GC side: JSObject.cpp:437-477 (Dependency-ordered loads +
structureID/maxOffset re-compare, didRace on mismatch).

**Adversarial self-check.** The encapsulation argument holds only for OWNED
runtime code; JIT fast paths re-derive the bound in machine code
(SPEC-jit consumes C4/I33 via the §9.4 predicates), and unowned callers are
discharged by manifest audit 7b — an audit, not a construction. That
process-shaped residue is precisely what a test should hammer.

**Verdict: needs-test:**
`JSTests/threads/cve/mc-df-segmented-length.js` — index-valued elements
(a[i] is only ever i or hole) under a grow/length-truncate/sparse-regrow
storm with forced SW (`--forceButterflySWBit=1` + a foreign first write) so
growth takes the T2 spine-replacement route.

## S4. IC / structure-check-then-offset-load (property fast path)

**Surface.** Fetch 1: structureID check (IC or dispatch). Fetch 2: butterfly
word + slot at the structure-derived offset. Foreign transition, deletion,
or dictionary flattening between the fetches is validate-on-one-fetch /
act-on-another verbatim. Sharpest sub-case: deleted-offset reuse — reader
holds p's offset, a foreign delete recycles the slot for q, reader's second
fetch returns q's value as p ("read of f returning g's value", I21).

**Governing protocol** (SPEC-objectmodel; this is the spec's center of
mass):
- M5 nuke protocol: shape-affecting transitions CAS structureID to a nuked
  value first; a reader whose fetch-1 raced sees the nuke and spins/retries
  (StructureID.h:39-51); fetch-1-passed implies the slot layout it derived
  is the one fetch 2 dereferences (with M7 ordering on arm64 —
  Dependency / loadLoadFence, JSObject.cpp:515-527 and :437-477).
- I9: the new property's value is release-stored BEFORE the new structureID
  publishes — a reader seeing the new ID sees the value (no torn
  name-without-value window).
- I24/M7: no reader derefs storage at an offset from a structure not
  ordered before, or revalidated after, the butterfly-word load.
- I18 + D1/I30 close the reuse sub-case: every deleted out-of-line offset
  goes Quarantined; reuse only after an owning-heap quarantine-epoch bump
  POSTDATING the deletion (epoch bumps happen world-stopped, and no fast
  path straddles a stop — so no pre-delete offset fetch survives into the
  reuse era); the delete itself release-stores jsUndefined(), never
  clear(), so the tardy-reader menu is {old value, undefined}, nothing else.
- I34: no poll/alloc/park between obtaining a PropertyOffset and the access
  without structureID re-validation. Atomics property paths conform
  explicitly (runtime/ThreadAtomics.cpp:233-240, :346: probe classifies,
  locked arm re-validates provenance, Restart on mismatch; the AS probe
  re-checks shape under the lock, :85).
- L6/I37: shared-Structure transition-table lookups/walks/mutations under
  m_lock — the table itself cannot be doubly-fetched into inconsistency.

**Adversarial self-check.** Two audit-discharged (not constructed) edges:
(i) I34 windows in UNOWNED callers (manifest 7b audit); (ii) the epoch
argument requires that quarantine promotion really is the SOLE feeder of
Reusable (any bypass re-opens CVE-grade slot confusion — the spec says "NO
bypass" but that is one `takeDeletedOffset` refactor away). Both are
implementation-fidelity risks on a sound design.

**Verdict: needs-test:** `JSTests/threads/cve/mc-df-delete-reuse.js` —
out-of-line f/g delete/reinstall churn with disjoint sentinels + GC
pressure (epoch advancement), readers assert no cross-slot value bleed and
stable-slot integrity. (Complements the existing
JSTests/threads/races/transition-vs-read.js, which covers the
transition-not-delete half of this surface.)

## S5. Parser / eval / JSON.parse over shared strings; rope resolution

**Surface.** CVE-2017-5116's deeper lesson is "never parse attacker-shared
mutable bytes twice". Our parser inputs are JSStrings. Could a second
thread mutate string bytes between the parser's fetches?

**Mechanism check.** JS strings are immutable at the language level, and
the one engine-internal mutation — rope resolution / atomization — is ruled
in UNGIL-HANDOUT §N.2 (JSString.h:637-682): the resolver computes into a
FRESH buffer and publishes by ONE release-CAS of the fiber0/flags word;
losers discard; readers load-acquire and branch on isRope. A consumer
therefore fetches either the rope (and resolves to a private/published
immutable buffer) or the resolved immutable buffer; in both cases the span
handed to the parser/JSON scanner never changes under it. There is no
SAB-backed-string or external-mutable-string type in this tree. Atom table
inserts are concurrent (sharded, U0) but insert-only-idempotent.

**Verdict: immune-by-construction** (immutability + single-publication;
cite §N.2's explicit rejection of the cell lock as confirmation the design
was reviewed for exactly this access pattern). No test — there is no
mutable fetch to race.

## S6. JIT compiler threads: compile-time fetch vs run-time truth

**Surface.** DFG/FTL read mutator-mutable metadata (Structures, butterfly
shapes, profiling) at compile time and emit code acting on it later — a
double fetch separated by milliseconds. The JVM exemplars (HotSpot
validate-then-compile races) live here.

**Governing protocol.** The "second fetch" is replaced by watchpoint
validity: compile-time assumptions are registered on watchpoint sets that
fire ONLY under STW (SPEC-objectmodel I13, M6: no fences needed in JIT code
because state changes only world-stopped; jit §5.6
invalidation/jettison/epoch/ISB). E1-E4 elision is legal only while the set
is valid AND watched (I14), and every fast path that cannot be
watchpoint-protected keeps the fused TID/SW check (E2/E3, jit D9/CS5).
Profiling reads stay racy-tolerated-by-design (jit item 7 — profiling is a
hint, never a soundness input). Concurrent compiler access to mutator
structures uses the existing ConcurrentJSLock/Concurrently-variant
machinery, extended by L6 for shared Structure tables.

**Verdict: immune-by-construction** at the design level; per-tier
verification belongs to the thread-ungil ladder and thread-scanners, not a
JS-level CVE test (no deterministic JS-visible oracle exists for "compiler
read stale metadata" that the S2-S4 tests don't already cover at the
observable end).

## S7. Atomics property/array-storage probe (validate-then-RMW)

**Surface.** `Atomics.*` on plain shared objects: classify the slot
(shape/index/offset — fetch 1), then perform the RMW (fetch 2).
ThreadAtomics.cpp:67-90 (AS probe), :129-152, :296-331 (classification),
:346-369 (RMW dispatch).

**Mechanism check.** The implementation is written in the
lock-and-revalidate idiom: the AS probe re-checks `hasAnyArrayStorage`
under the cell lock (:85 "Shape moved before the lock landed: caller
re-classifies"); the slot RMW arm re-validates structure/shape/offset
provenance under its lock and Restarts on mismatch (:233-240); the comment
at :346 cites I34 by name. Classification is treated as a HINT, never acted
on without revalidation at the acting fetch.

**Verdict: immune-by-construction** (validate-at-act, the canonical MC-DF
fix). Covered incidentally by the existing
JSTests/threads/atomics corpus + S4's test (same accessor family).

---

## Summary table

| # | Surface | Anchor | Governing invariant | Verdict |
|---|---------|--------|---------------------|---------|
| S1 | wasm bytes validate-vs-compile | JSWebAssemblyHelpers.h:165; WebAssemblyModuleConstructor.cpp:301; JSWebAssembly.cpp:155/281/422 | copy-once snapshot; annex N6 for the snapshot's own torn pair | immune-by-construction + tripwire test |
| S2 | TA/DataView length-then-base vs detach/resize | ArrayBuffer.cpp:151/:184 (+ :498/:525/:628 writer arms); per-tier TA fast paths | SPEC-ungil annex N6 (BINDING) | needs-test → mc-df-ta-detach-resize.js |
| S3 | segmented bounds: shared publicLength vs per-spine VL | ConcurrentButterfly.h:405-421; ConcurrentButterfly.cpp:113-157; JSObject.cpp:437-477 | SPEC-objectmodel C4/I33/I6/I7 | needs-test → mc-df-segmented-length.js |
| S4 | IC structure-check-then-offset-load; deleted-offset reuse | JSObject.cpp:515-527; ThreadAtomics.cpp:233-240; PropertyTable quarantine (SPEC-om §6) | M5/M7/I9/I18/D1/I24/I34/L6 | needs-test → mc-df-delete-reuse.js |
| S5 | parser/JSON over shared strings; rope resolve | JSString.h:637-682 | UNGIL-HANDOUT §N.2 (single release-CAS publication; immutability) | immune-by-construction |
| S6 | compiler compile-time fetch vs run-time truth | jit §5.6 sites; watchpoint sets | I13/I14/M6 (fire-only-in-STW) + jit E1-E4 | immune-by-construction |
| S7 | Atomics probe-then-RMW | ThreadAtomics.cpp:67-90, :233-240, :346 | I34 validate-at-act under cell lock | immune-by-construction |

No surface in the first pass earned **susceptible-suspected**: the two
places the design leans on audits rather than construction (S3
unowned-caller bound derivation, S4 I34 windows / Reusable-feeder
exclusivity) are exactly where the tests aim.

---

## Second-pass addendum (2026-06-15)

The 2026-06-15 catalog revision folds CVE-2014-0456 (HotSpot
System.arraycopy element-type race), CVE-2020-14803 (NIO Buffer
check/use), JSC r269531 / bug 218944 (TypedArray.sort on SAB), and
CVE-2025-8880 into MC-DF, and adds *sort* and *structured clone* to the
trusted-consumer list. CVE-2020-14803 maps to S2 (already covered).
CVE-2025-8880 is provisional with no public detail; treated as generic
validate-then-act and discharged by S2-S4. The remaining exemplars open
three new surfaces and one non-surface:

### S8. Array.prototype.sort over a shared JSArray

**Surface.** `sortCompact` (runtime/ArrayPrototype.cpp:821-910) reads the
input once into a private `SortJSValueVector`, then sorts/commits the copy
— the r269531 fix shape applied to JSArrays. The flag-on gate at :830 is
the §10.7 manifest entry: `!thisObject->mayBeSegmentedButterfly()` keeps
segmented words on the generic `getIfPropertyExists` loop (:894).

**Suspected hole.** The §10.7 gate is *check-then-reload*, not
single-snapshot: :830 loads the tagged word (check), :834
`*thisObject->butterfly()` loads it AGAIN (act). The round-4 sweep that
introduced `jsThreadsFlatSnapshot` (runtime/JSArray.cpp:618-656) and the
explicit TOCTOU note at JSArray.cpp:1752-1762 establish that **once a
shape family's TTL sets are fired, a foreign §4.2 flat→segmented
conversion needs only the cell lock + DCAS — NO stop** — so it can land
between :830 and :834 with no intervening safepoint. :834's flat-only
decode then reads the ButterflySpine* payload as a Butterfly*; :835/:836
derive `publicLength`/`data` from spine innards (per :1756, "publicLength
at spine-8"). The garbage `butterflyLength` then sizes the
`compactedRoot.fill` allocation AND bounds the read loop at :839 → heap
read at attacker-influenced extent into a JS-visible sorted array. The
debug ASSERT / `verifyConcurrentButterfly` RELEASE_ASSERT at
JSObject.h:918-920 would catch this; release without verify would not.

The round-4 sweep converted every JSArray.cpp fast path to single-snapshot
for exactly this reason but did NOT touch ArrayPrototype.cpp (different
file, owned by the §10.7 manifest's pre-round-4 audit). Contrast
`appendMemcpy` (:1444-1478) and `fastSlice` (:2027-2147), which were
re-swept.

**Verdict: susceptible-suspected** — the §10.7 contract at
JSObject.h:910-915 ("a §10.7 mayBeSegmentedButterfly() guard" suffices) is
contradicted by the round-4 finding at JSArray.cpp:1752; this site was not
in the round-4 sweep. Fix shape: replace the :830 gate + :834 re-load with
a single `jsThreadsFlatSnapshot(thisObject, Read, butterfly)` and bound the
loop by the snapshot's `vectorLength` (mirroring fastSlice's flat leg).
Confirmation test: `JSTests/threads/cve/mc-df-arraycopy-relabel.js` (shared
with S10; both exercise the same §10.7-gate-vs-§4.2 window).

### S9. TypedArray.prototype.sort, no-comparator, non-SAB backing (r269531 re-opened)

**Surface.** `JSGenericTypedArrayView<Adaptor>::sort()`
(runtime/JSGenericTypedArrayViewInlines.h:950-988) is the literal r269531
fix site: when `isShared()` (:963) it copies into a private `Vector`,
sorts the copy, copies back; otherwise it `std::sort`s the backing bytes
IN PLACE (:981). The comparator path
(JSGenericTypedArrayViewPrototypeFunctions.h:1758-1828) always copies
first and is immune.

**Suspected hole.** `isShared()`
(runtime/JSArrayBufferViewInlinesLight.h:34-52) is the SAB-era predicate:
it returns true only for SAB-mode views and **false for FastTypedArray /
non-SAB Wasteful** (default arm). Under `--useJSThreads` the whole heap is
shared — a non-SAB TA stored on any reachable object is concurrently
writable by foreign threads via the ordinary TA element store path (no
per-element gate; annex N6 governs only the {base,length} pair, not byte
stability). So a foreign thread writes `ta[i]` while main runs `std::sort`
on `[array, array+length)`: introsort's partition loops assume the pivot
is a sentinel; concurrent mutation breaks the strict-weak-ordering
contract and the inner `while (a[i] < pivot) ++i` can run past `length`
(libc++/libstdc++ both — exactly the r269531 rationale). Result: OOB read
AND OOB write (the swap) into adjacent heap, from pure user JS.

Adversarial self-check: is there any threads-mode rule that routes non-SAB
TA element stores through serialization? None found — annex N6 / R10/R11
rows in UNGIL-HANDOUT cover detach/resize/mode-transition only;
SPEC-objectmodel does not own TA bytes (they are not butterfly storage);
no `useJSThreads` reference anywhere in JSGenericTypedArrayView*.{h,cpp}
except the :481 §10.7 gate.

**Verdict: susceptible-suspected** — the r269531 gate is necessary but no
longer sufficient under shared-everything. Fix shape: `if (isShared() ||
Options::useJSThreads())` at :963 (always copy-out under the flag), or a
TA-level "ever escaped to a foreign thread" bit if the copy is
bench-visible. Confirmation test:
`JSTests/threads/cve/mc-df-ta-sort-inplace.js` — index-valued sentinel
oracle (any post-sort element ∉ [0,N) = OOB evidence) under a foreign
write storm; ASAN build is the sharp detector.

### S10. Bulk element-copy fast paths (CVE-2014-0456 analog)

**Surface.** The System.arraycopy mechanism — type-check the source's
element kind once, then raw-memcpy elements trusting that kind — maps to
two families:

(a) **JSArray.cpp round-4-swept family**: `appendMemcpy(otherArray)`
(:1377-1503), `fastSlice` (:2009-2180), `fastCopyWithin` (:1030),
`fastFill` (:659), `fastToReversed` / `fastWith` / `fastIncludes` /
`shiftCount*` / `unshiftCount*`. All gated by `jsThreadsFlatSnapshot`
(:639) or hand-rolled equivalent: ONE tagged-word load, segmented/AS bail,
exclusive-owner check for in-place writes, snapshot-`vectorLength` bound,
NO `butterfly()` re-load. The CVE-2014-0456 element-type race
(Int32→Contiguous relabel under a raw reader → cell-pointer bits read as
int32) is closed by SPEC-objectmodel R-DOUBLE: shape relabels touching
Double on an SW=1/segmented object are per-event STW (§10.6), and the
fastSlice comment at :2115-2122 spells out the residual ("garbage-decoded
lanes are at worst reinterpreted numbers, never followed as cell
pointers"). **Immune-by-construction** — single-snapshot + STW relabel +
I7 frozen-snapshot reachability is the canonical MC-DF fix triple.

(b) **`setFromArrayLike` → `copyFrom{Int32,Double}ShapeArray`**
(runtime/JSGenericTypedArrayViewInlines.h:458-509, :405-455): the §10.7
manifest's own named example (annex §15 line 71: "typedArray.set(shared
SegmentedArray) would deref a spine as flat: OOB"). Gate at :481 is
`!array->mayBeSegmentedButterfly()` — check-then-reload, same defect as
S8 — and `copyFromInt32ShapeArray` then calls `array->butterfly()` FRESH
at :417/:421/:425/:429 (and the per-element loop at :429 re-loads it every
iteration). A foreign cell-lock-only §4.2 between :481 and :417 yields a
spine-as-flat deref whose `contiguous().data()` is the source span for
`WTF::copyElements` straight into the destination TypedArray —
attacker-observable heap bytes. This site is OUTSIDE JSArray.cpp and was
not in the round-4 sweep (only `useJSThreads`/`mayBeSegmented` reference
in the entire JSGenericTypedArrayView* file family is :481).

**Verdict: (a) immune-by-construction; (b) susceptible-suspected** — same
root cause as S8 (the §10.7 check-then-reload contract is unsound
post-round-4). Fix shape: hoist a single `taggedButterflyWord()` load at
:481, bail on segmented OR null, derive `data` from `untaggedButterfly` of
THAT word, bound by THAT snapshot's `vectorLength`, and pass the snapshot
span into `copyFrom*ShapeArray` instead of re-loading. Confirmation test:
`JSTests/threads/cve/mc-df-arraycopy-relabel.js` — fresh Int32 JSArray per
round, foreign thread forces SW=1 then triggers §4.2 (push past capacity);
main races `ta.set(arr)`; oracle = sentinel-set membership +
`--verifyConcurrentButterfly=1` RELEASE_ASSERT as the crisp detector.

### S11. Structured clone (catalog consumer addition)

**Non-surface.** `SerializedScriptValue` / `CloneSerializer` live in
WebCore, not this tree (only JSC reference: `validateSerializedValue`
test option, OptionsList.h:264). SPEC-api passes Thread arguments and
results BY REFERENCE through the shared heap — there is no
serialize/clone step at the thread boundary, so the "clone walks a graph
a second agent mutates" mechanism has no JSC-side instance. Any embedder
that adds `structuredClone` over a `--useJSThreads` heap inherits S3/S4's
invariants for its property/element walk (it bottoms out in
`getDirect`/`getIfPropertyExists`, which are §Q regime-safe). Recorded
here as out-of-scope for the engine audit; flagged for the embedder
integration note.

---

## Summary table (revised 2026-06-15)

| # | Surface | Anchor | Governing invariant | Verdict |
|---|---------|--------|---------------------|---------|
| S1 | wasm bytes validate-vs-compile | JSWebAssemblyHelpers.h:165; WebAssemblyModuleConstructor.cpp:301; JSWebAssembly.cpp:155/281/422 | copy-once snapshot; annex N6 for the snapshot's own torn pair | immune-by-construction + tripwire test |
| S2 | TA/DataView length-then-base vs detach/resize | ArrayBuffer.cpp:151/:184 (+ :498/:525/:628 writer arms); per-tier TA fast paths | SPEC-ungil annex N6 (BINDING) | needs-test → mc-df-ta-detach-resize.js |
| S3 | segmented bounds: shared publicLength vs per-spine VL | ConcurrentButterfly.h:405-421; ConcurrentButterfly.cpp:113-157; JSObject.cpp:437-477 | SPEC-objectmodel C4/I33/I6/I7 | needs-test → mc-df-segmented-length.js |
| S4 | IC structure-check-then-offset-load; deleted-offset reuse | JSObject.cpp:515-527; ThreadAtomics.cpp:233-240; PropertyTable quarantine (SPEC-om §6) | M5/M7/I9/I18/D1/I24/I34/L6 | needs-test → mc-df-delete-reuse.js |
| S5 | parser/JSON over shared strings; rope resolve | JSString.h:637-682 | UNGIL-HANDOUT §N.2 (single release-CAS publication; immutability) | immune-by-construction |
| S6 | compiler compile-time fetch vs run-time truth | jit §5.6 sites; watchpoint sets | I13/I14/M6 (fire-only-in-STW) + jit E1-E4 | immune-by-construction |
| S7 | Atomics probe-then-RMW | ThreadAtomics.cpp:67-90, :233-240, :346 | I34 validate-at-act under cell lock | immune-by-construction |
| S8 | Array.prototype.sort compact-phase fast path | ArrayPrototype.cpp:830-887 vs JSArray.cpp:1752 round-4 finding | §10.7 gate (check-then-reload) vs cell-lock-only §4.2 | **susceptible-suspected** |
| S9 | TA.prototype.sort() no-comparator in-place std::sort | JSGenericTypedArrayViewInlines.h:950-988; JSArrayBufferViewInlinesLight.h:34 | r269531 `isShared()` gate is SAB-only; no annex covers byte stability | **susceptible-suspected** → mc-df-ta-sort-inplace.js |
| S10a | JSArray bulk-copy fast paths (appendMemcpy/fastSlice/…) | JSArray.cpp:639-656, :1377-1503, :2009-2180 | round-4 single-snapshot + R-DOUBLE STW relabel + I7 | immune-by-construction |
| S10b | setFromArrayLike → copyFrom{Int32,Double}ShapeArray | JSGenericTypedArrayViewInlines.h:481, :417-454 | §10.7 gate (check-then-reload) — NOT round-4-swept | **susceptible-suspected** → mc-df-arraycopy-relabel.js |
| S11 | structured clone over shared graph | (WebCore, not this tree) | SPEC-api by-reference passing; §Q for embedder walks | out-of-scope / not-applicable |

S8/S9/S10b share a single root cause class: **a SAB-era or pre-round-4
sufficiency predicate (`isShared()`, `mayBeSegmentedButterfly()`
check-then-reload) that the shared-everything model has widened past.**
The round-4 sweep fixed the JSArray.cpp instances; ArrayPrototype.cpp and
JSGenericTypedArrayViewInlines.h were outside that sweep's file set.

### S8 + S10b — FIXED 2026-06-18 (Tier-B B1+B2)

Landed the round-4 single-snapshot discipline at both sites:

- **S8** `sortCompact` (ArrayPrototype.cpp): the §10.7 gate stays as the
  flag-off-dead segmented hint; flag-on, ONE `taggedButterflyWord()` load
  immediately after it drives every flat deref in all three case bodies
  (segmented/null snapshot → `default` arm → generic
  `getIfPropertyExists` loop), with each case's read loop bounded by the
  SNAPSHOT's `vectorLength`. Flag-off the `[[unlikely]]` arm is dead and
  the case bodies compile to the unchanged originals (I22).
- **S10b** `copyFrom{Int32,Double}ShapeArray`
  (JSGenericTypedArrayViewInlines.h): the helper BODIES now carry the
  authoritative snapshot — ONE `taggedButterflyWord()` load at the top,
  segmented/null/short → regime-safe `tryGetIndexQuickly` per-element
  fallback (never OOBs, never follows garbage as a pointer); otherwise
  every `array->butterfly()` re-load replaced with the single
  `sourceButterfly`. This also closes the previously-ungated
  `genericTypedArrayViewPrivateFuncFromFast` caller (allocation safepoint
  between its shape check and the helper call) without touching that
  unowned file. The `setFromArrayLike` §10.7 gate is demoted to a
  dispatch hint between the flat-helper and segmented-walk arms; both
  arms are individually authoritative.

R-DOUBLE residual matches the round-4 ruling at fastSlice
(JSArray.cpp:2115-2122): shape relabels touching Double on a shared word
are per-event STW (§4.7/§10.6); the lock-free Int32→Contiguous (§4.3)
race yields at worst reinterpreted numbers in private storage / TA
elements, never a followed cell pointer. Gate:
`mc-df-arraycopy-relabel.js` 20/20 Debug GIL-off + ASAN with
`--verifyConcurrentButterfly=1`.

## Test manifest (EXECUTED LATER, post-ungil — do not run during bring-up)

- JSTests/threads/cve/mc-df-ta-detach-resize.js — `--useJSThreads=1`
- JSTests/threads/cve/mc-df-segmented-length.js — `--useJSThreads=1 --forceButterflySWBit=1`
- JSTests/threads/cve/mc-df-wasm-compile-race.js — `--useJSThreads=1 --useWebAssembly=1`
- JSTests/threads/cve/mc-df-delete-reuse.js — `--useJSThreads=1`
- JSTests/threads/cve/mc-df-ta-sort-inplace.js — `--useJSThreads=1` (ASAN strongly recommended)
- JSTests/threads/cve/mc-df-arraycopy-relabel.js — `--useJSThreads=1 --verifyConcurrentButterfly=1`

All are deterministic-oracle / nondeterministic-interleaving: trivially
green under the phase-1 GIL, signal-bearing GIL-off, and amplifier-ready
(Tools/threads/amplify.sh, TSAN no-JIT target). None spawn unbounded
work; all join every thread (annex T2 conventions).
