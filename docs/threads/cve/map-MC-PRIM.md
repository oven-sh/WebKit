# map-MC-PRIM — Trusted-primitive invariant bypass

Status: surface map, 2026-06-07. Defensive audit artifact for `--useJSThreads`
(jarred/threads). Class definition: docs/threads/CVE-AUDIT.md "MC-PRIM" (merged from
JVM-10 / CVE-2016-3587 note / RG Unsafe pattern; exemplars CVE-2012-0507
AtomicReferenceArray covariant store, sun.misc.Unsafe exposure, CVE-2016-3587
MethodHandle.invokeBasic).

Mechanism, restated for our engine: a privileged or "atomic" primitive performs raw
loads/stores trusting a construction-time (or probe-time) invariant that OTHER machinery
lets a second thread break; the atomic op is the least-checked store in the engine.
CVE-2012-0507's anatomy maps 1:1 onto our new surfaces: AtomicReferenceArray's CAS/set
trusted "backing array is Object[]" established at construction; deserialization let an
attacker substitute a covariant `T[]`; the raw store then wrote an arbitrary-typed value
through the unchecked slot. Our equivalents of "the backing array" are: an object's
{StructureID, PropertyOffset} slot provenance, a butterfly payload, a typed array's
{base, length, !detached} tuple, and a generator's claimed internal-field frame. Our
equivalents of "deserialization" are: cross-thread transitions/deletes/dictionary
conversions, butterfly republish (T1 grow / §4.2 conversion), ArrayBuffer
detach/transfer/resize, and unclaimed resume paths.

Every surface below is a primitive that bypasses (or is allowed to bypass) the generic
cell-lock path — CVE-AUDIT's framing: "every C++/JIT intrinsic that bypasses the
cell-lock fast path is our Unsafe and needs an explicit soundness statement." This file
is that statement, per surface.

Verdict key: **immune-by-construction** (protocol cited, adversarial argument given) /
**needs-test** (test written under JSTests/threads/cve/, executed post-ungil) /
**susceptible-suspected** (precise suspected hole).

---

## P1 — OM §9.5 atomic slot accessors, lock-free arms

**Where:** `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:2786`
(`atomicSlotLockFreeLoop`), `:2822` (`JSObject::atomicSlotReadModifyWrite`, named),
`:3006` (`...AtIndex`, indexed); probe side
`Source/JavaScriptCore/runtime/ThreadAtomics.cpp:273`
(`probeOwnPropertyForAtomicsConcurrent`).

**Governing spec:** SPEC-objectmodel §9.5 + annex §Q; invariants I34 (no
poll/alloc/park between offset resolution and access unless structureID is re-validated
after), I21 (no read of f returning g's value), I30/D1 (delete quarantine sentinel),
I27/T1 (flat copying grow is owner-only against `(currentTID, 0)`), I33/C4 (segmented
bounds = min(publicLength, loaded spine VL)); SPEC-ungil ANNEX C1 (U-T10 re-home, U5
hardening).

**Why this is THE MC-PRIM primitive:** the accessor takes a caller-supplied
`{offset, expectedStructureID}` provenance and then runs a raw seq_cst 64-bit CAS/RMW
loop directly on the slot word — no cell lock on the lock-free arms. The provenance is
exactly a construction-time invariant another thread's transition machinery can break.

**Verdict: immune-by-construction.** The adversarial cases and their refutations:

1. *Data→accessor reconfiguration between probe and CAS* (the literal 2012-0507 shape:
   CAS a primitive over a GetterSetter). Non-dictionary attribute changes publish a NEW
   StructureID; the loop re-validates `owner->structureID() != expectedStructureID`
   EVERY iteration, ordered after the seq_cst slot load (acquire half pins the order,
   `ConcurrentButterfly.cpp:2799-2806`). The probe additionally re-reads attributes
   against the SAME structure the provenance is taken from
   (`ThreadAtomics.cpp:331-349`, the U-T10 amend), closing the
   methodTable-walk-vs-getConcurrently double-read window, and keeps CustomValue out of
   the lock-free arms.
2. *Dictionary mutation under an unchanged StructureID* (dictionaries reconfigure
   in place — the ID check alone is blind). Dictionary structures NEVER reach the
   lock-free arms: `structure->isDictionary() || arrayStorageShape` routes to the
   cell-locked third arm (`:2837`), and a flat→dictionary CONVERSION changes the ID
   (argued at `:2901-2903`), so a loop spinning across the conversion restarts.
3. *Delete under an unchanged butterfly word* (named non-dictionary delete is a
   structure-only transition; the D1 jsUndefined sentinel is stored into the doomed slot
   BEFORE the structure publication, so an ID check can pass while the sentinel is
   visible — CAS with expected===undefined would "succeed" on an absent property). U5
   hardening: named lock-free reads of jsUndefined bounce out as `LockedRevalidate` and
   are disambiguated under the cell lock, where both delete flavors hold their whole
   sentinel→publication window (§6 L4; `lockedUndefinedArm`, `:2910-2985`). Indexed
   arms pass `revalidateUndefined=false` soundly: indexed holes are EMPTY JSValues,
   never the sentinel, and `!current` restarts.
4. *Butterfly republished under the loop* (T1 copying grow has NO nuke — a CAS into the
   old payload would be silently lost: a lost-write MC-PRIM). Post-resolution word
   re-validation (`:2990-3000`): after `taggedButterflyWord() == word` re-check the flat
   payload is pinned because T1 is owner-only against `(currentTID, 0)` — we are that
   owner (cannot race ourselves) or SW=1 makes T1 impossible (I27); §4.2 conversion
   aliases the flat slices so the slot address stays live (I7); segmented out-of-line
   fragment identity is stable across replacement spines (T2 aliases verbatim).
5. *StructureID recycled to a fresh Structure that passes the equality check with a
   different layout.* Excluded by I34's no-poll rule: there is no GC safepoint between
   the probe's `structure->id()` capture and the accessor's checks (straight-line,
   AssertNoGC-style; Structure reclamation requires a stop this thread must join), and
   the OBJECT itself keeps its current Structure marked. Delete→re-add offset reuse
   across GC is additionally gated by the per-server-heap quarantine epoch
   (SPEC-objectmodel §6 / release of quarantined slots only at stop boundaries).
6. *Foreign first write on a thread-local flat butterfly* (raw store would bypass the
   TTL machinery other code trusts). The flat arm runs the §3 SW-set DCAS
   (`ensureSharedWriteBit`) BEFORE any store, then re-dispatches (`:2977-2984`).

Residual fragility (not a hole, an obligation): `probeArrayStorageElementForAtomics`
(`ThreadAtomics.cpp:79`) is a cell-lock site OUTSIDE ConcurrentButterfly.cpp, unobserved
by the TU-private O3 depth witness — its no-alloc/no-park/no-safepoint obligation is
comment-enforced only (`:74-77`). Flag for the thread-scanners pass (clang-tidy
lock-scope checks), not for a JS test.

**Existing evidence:** JSTests/threads/atomics/property-cas-delete-undefined-sentinel-u5.js,
property-cas-dictionary-delete-u5.js, property-cas-storm-u28-flat.js,
property-cas-storm-u5-as.js — these are the class tests for cases 2-4 and already live
in the corpus; no duplicate written here.

## P2 — §9.5 third arm (dictionary / ArrayStorage, cell-locked)

**Where:** `ConcurrentButterfly.cpp:2837-2895` (named third arm), `:3028+` (indexed
AS/dictionary arm). **Spec:** OM I19/L3 (dictionary cell-lock), I31/L5 (every flag-on
runtime AS access cell-locked, reads included; AS never segments), AS PRE-LOCK r8
item 6 (SW=0 foreign writer runs the §4.6 first-foreign-write STW protocol BEFORE
taking the lock, then restarts).

**Verdict: immune-by-construction.** The primitive holds the same lock every other
dictionary/AS accessor must hold; dictionary-ness, offset, attributes (incl. ReadOnly,
Accessor/CustomAccessor/CustomValue) are ALL re-derived under the lock
(`:2851-2864`); the store is `slot->set()` under that lock, so there is no
"least-checked" store — it is exactly as checked as the generic path. The pre-lock
SW protocol retires the only unlocked writers (owner AS fast paths gate on SW=0).
Adversarial corner — probe classifies Plain, accessor then sees a hole (racing
delete/shrink): bounded restart, fresh probe re-classifies (I5 fix,
`ThreadAtomics.cpp:251-263`); livelock is the same bounded-adversarial class as
§C.3(b), accepted by the spec.

## P3 — GIL-on "one atomic step" property-Atomics bodies

**Where:** `ThreadAtomics.cpp:102` (`getOwnPropertyForAtomics`), `:216`
(`putExistingOwnDataPropertyForAtomics`), dispatched from `AtomicsObject.cpp:215/:416`
(non-view receivers route here flag-on). **Spec:** SPEC-api §4.5 (every property op is
one atomic step) + the D3 landed deviation (Proxy/GlobalProxy and exotic-own-data
receivers rejected with TypeError up front).

**Verdict: immune-by-construction (GIL phase only).** The trusted invariant is "no GIL
drop between the own-property read and the write". The two mechanical breakers are
gated: (1) reentrant receivers (Proxy traps could reach a park site mid-step) →
TypeError at `:111-114`; (2) exotic own data properties not backed by plain
structure/butterfly storage (JSArray length, RegExp lastIndex, StringObject chars,
sparse indices) where putDirect would install a duplicate shadow slot → rejected by the
canGetIndexQuickly / isValidOffset gates (`:130-160`). After the gates, the window
contains only non-reentrant structure/butterfly probes; allocation can trigger GC but
the phase-1 GIL hands off only at named park sites (join, cond.wait, contended
lock.hold, property Atomics.wait — none reachable). These bodies become dead GIL-off
(U-T10 re-homed the four value ops onto P1); their soundness does not carry forward and
must NOT be cited post-ungil.

## P4 — Atomics.store Missing-arm, INDEXED leg

**Where:** `ThreadAtomics.cpp:430-443` (`atomicsStoreOnPropertyGilOff`, Missing case,
`putDirectIndex` at `:440`). **Spec:** SPEC-ungil §C.2; the named-key fix is the U-T10
conditional add (`putDirectForAtomicsMissingAdd`, `:455-462`, re-derives
existence/extensibility inside the E4-published §2 loop).

**Verdict: CLOSED (fix landed 2026-06-10; test green).** Original finding (kept for
the record): a fresh INDEXED element was added through `putDirectIndex`'s define-own
leg with no conditional re-derivation — a racing indexed `defineProperty` (accessor or
non-writable) on another thread forced a sparse-map/SlowPutAS conversion the put was
not conditional on; the least-checked store could clobber a just-defined indexed
accessor or non-writable element (CVE-2012-0507's "atomic primitive writes through an
invariant someone else just changed"). **Landed fix:** the indexed twin of the
named-key conditional add — `JSObject::putDirectIndexForAtomicsMissingAdd`
(`ThreadAtomics.cpp:496`, wired at `:706`; named-key twin at `:727`) re-derives
existence/extensibility/shape inside the E4-published §2 loop and restarts on loss.
Same closure as MC-REENT S3c — full executed record (40/40 + 3/3, plus the narrowed
preventExtensions residual) lives in map-MC-REENT.md S3c.

**Test:** `JSTests/threads/cve/mc-prim-indexed-missing-define-race.js` (deterministic
invariant per owner iteration; indexed twin of
JSTests/threads/atomics/property-store-missing-define-race.js, which covers only named
keys; second phase covers the non-writable-data variant). PASSING GIL-on and GIL-off
since 2026-06-10 — a future failure is a REGRESSION of the landed conditional add.

## P5 — Internal-field resume claim (generators / async fns / iterator helpers)

**Where:** SPEC-ungil §N.5 — `@atomicInternalFieldClaim` / `@atomicInternalFieldPublish`
twin intrinsics; claim+publish site list = BINDING annex N7. NOT YET IN TREE (grep for
`atomicInternalFieldClaim` is empty as of this audit) — the surface is chartered, the
primitive lands with the ungil implementation.

**Spec invariant:** single-word resume-claim CAS SuspendedX→Running on the STATE field;
at-most-one-resumer keeps every interior store WHILE CLAIMED **plain and tier-inlined**.

**Verdict: needs-test.** This is MC-PRIM by design: the plain interior stores are
deliberately the least-checked stores in the engine, trusting one CAS. The bypass risk
is any writer that touches generator/async internal fields WITHOUT the claim — resume
heads are converted ("check-then-store = ONE claim"), but `.return()`/`.throw()` paths,
debugger/inspector state pokes, and host-code completions must all appear in the annex
N7 claim-site list; one missed site = two simultaneous "owners" interleaving plain
multi-word stores (torn {state, frame, resumeMode}). Cannot be verified by construction
until the intrinsics exist; the susceptibility probe is written now.

**Test:** `JSTests/threads/cve/mc-prim-generator-resume-claim.js` (two threads race
`.next()` on one shared generator; exactly-once delivery of 0..N-1, per-thread
monotonicity, only TypeError or well-formed IteratorResult escapes; amplifier-ready —
the claim window is a few instructions).

## P6 — Raw buffer primitives vs cross-thread detach/transfer/resize

**Where:** consumers: `AtomicsObject.cpp` typed-array RMW lanes + every
memmove-class fast path (fill/set/copyWithin/slice); breakers:
`Source/JavaScriptCore/runtime/ArrayBuffer.h:199` (`ArrayBufferContents::detach`),
`:298` (`detach(VM&)`/`transferTo`), resizable grow/shrink; view-side
`JSArrayBufferView` {m_vector, m_length, m_mode}. **Spec:** SPEC-ungil §N.6 + annex N6
torn-pair table (BINDING); audit rows SPEC-ungil-audit-N7.md R10/R11. PRINCIPLE (r12
F2): a racing reader never pairs a passing length with an unmapped-or-short base.

**Why MC-PRIM here and not just MC-GROW:** pre-threads, a NON-shared ArrayBuffer was
single-agent by construction — every fast path's "checked detached once at entry, then
raw pointer arithmetic" pattern trusted that construction-time invariant. The shared
heap hands every non-shared buffer to N threads; detach/transfer/resize is the "other
machinery" and the entry-checked raw store is the least-checked store. (The shared/SAB
lanes were always multi-agent and are not the new risk.)

**Verdict: needs-test** (spec-immune, implementation unverifiable mid-bring-up). The
N6 rulings, if implemented verbatim, close the class: DETACH publishes length=0 seq_cst
FIRST and quarantines contents to a heap §10 stop (so a loser's raw access lands in
stale-but-mapped memory — SAB-staleness semantics, not UAF); TRANSFER = copy + source
detach; SHRINK publishes length seq_cst with the tail free deferred; GROW keeps the
base immutable and commits before release-publishing the length. The torn pair is
unrepresentable in every interleaving IF the publication orders are exactly as ruled —
which is precisely what only an executed test + TSAN can confirm.

**Test:** `JSTests/threads/cve/mc-prim-arraybuffer-transfer-vs-atomics.js` (hammer
thread does Atomics.add/load + fill on an Int32Array over a resizable non-shared
buffer; main thread resize-shrinks/grows and transfers in rounds; invariant: only
TypeError or values inside the written band; amplifier/ASAN/TSAN-ready).

## P7 — Tier-inlined atomic/IC bypasses of the cell lock

**Where/spec:** DFG/FTL Map/Set intrinsics — DISABLED GIL-off, routed to the locked
native bodies (SPEC-ungil §N.1); internal-field tier lowering mode-keyed (§N.5, r17
F5); N7 §IM: tier-inlined accesses to every audited row are "disabled or re-pointed per
row"; per-tier butterfly/structure check placement = SPEC-jit E1-E4/M7.

**Verdict: immune-by-construction, CONTINGENT.** The design answer to "JIT'd code is
our Unsafe" is to not let any tier keep an inlined fast path whose invariant the row's
protocol doesn't re-establish — immunity rests on the N7 table being consumed verbatim
at ungil implementation time. This map does not re-audit per-tier placement (that is
MC-JIT's map); MC-PRIM's residue here is exactly the rows whose tier action is
"re-pointed" — each re-pointed intrinsic must land on a body in P1/P2/P5/P6 above.
Cross-reference: map-MC-JIT (when written) owns check-placement; this file owns "the
body the JIT lands on is one of the audited primitives".

## P8 — Property waiter table (Atomics.wait/notify on plain objects)

**Where:** `ThreadAtomics.cpp:776+` (PropertyWaiterTable), `:972`
(`atomicsWaitOnProperty`). **Spec:** SPEC-api §5.6, §5.10 (cellProtect roots the
waited-on cell while the list is non-empty; per-cell finalizer clears lists so a
recycled cell address never aliases a stale waiter list, `:831-835`); the wait's
value-check + park is performed under the list lock with the GIL dropped via
GILDroppedSection (harness.js documents the park semantics).

**Verdict: immune-by-construction** for the MC-PRIM angle: the primitive's trusted
invariant is list-key identity (cell, uid), and the machinery that could break it
(GC recycling the cell address) is intercepted by the finalizer + Strong-rooting
protocol. The check-value-then-park atomicity itself is MC-WAIT territory (lost-wakeup
class), audited there, not here.

---

## Test index (JSTests/threads/cve/, written NOT run — execute post-ungil)

| Test | Surface | Mode |
|---|---|---|
| mc-prim-indexed-missing-define-race.js | P4 | deterministic invariant; passes GIL-on, probes residual GIL-off |
| mc-prim-generator-resume-claim.js | P5 | deterministic invariant; amplifier-ready |
| mc-prim-arraybuffer-transfer-vs-atomics.js | P6 | hammer; ASAN/TSAN/amplifier-ready |

All three carry `//@ requireOptions("--useJSThreads=1")` and use the §8 harness
(`load("../harness.js", "caller relative")`). None were built or executed in this
audit (tree is mid-bring-up); they join the post-ungil thread-cve-audit run.

## Fix queue (when execution confirms)

1. P4: **DONE 2026-06-10** — `putDirectIndexForAtomicsMissingAdd` landed
   (`ThreadAtomics.cpp:496`/`:706`), mirroring `putDirectForAtomicsMissingAdd`;
   closure record in map-MC-REENT.md S3c. Nothing left on this item.
2. P5: at ungil-implementation review, diff the annex N7 claim-site list against every
   writer of generator/async internal fields (incl. `.return()`/`.throw()`, debugger
   pokes) before enabling tier-inlined claimed stores.
   **DIFF RESULT (post-landing review round): NOT fully discharged — recorded
   deferral.** Claimed: GeneratorPrototype.js resume/return/throw heads,
   JSIteratorHelperPrototype.js next/return. Release-ordered but unclaimed: the
   yield-side unclaim store (all generator-ish bodies — now store-RELEASE in all
   tiers gilOff via the put_internal_field fence, relocated after the frame saves,
   fail-closed; the generatorRegister()-based validation extends the reorder to
   wrapper-less async functions previously silently skipped). UNCLAIMED and OPEN:
   `AsyncGeneratorPrototype.js` resume head (plain check-then-store :37/:78/:83) +
   asyncGeneratorEnqueue queue-field writers — two threads racing `agen.next()`
   still get two simultaneous owners. Deferral + owed async susceptibility test
   recorded in CVE-AUDIT-STATUS.md item 3 and SPEC-ungil-history.md
   "§N.5 LANDED SHAPE".
3. P1 fragility note: add the `probeArrayStorageElementForAtomics` lock scope to the
   thread-scanners clang-tidy target so the no-alloc/no-park obligation is
   machine-checked.
