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

No surface earned **susceptible-suspected**: the two places the design
leans on audits rather than construction (S3 unowned-caller bound
derivation, S4 I34 windows / Reusable-feeder exclusivity) are exactly where
the tests aim.

## Test manifest (EXECUTED LATER, post-ungil — do not run during bring-up)

- JSTests/threads/cve/mc-df-ta-detach-resize.js — `--useJSThreads=1`
- JSTests/threads/cve/mc-df-segmented-length.js — `--useJSThreads=1 --forceButterflySWBit=1`
- JSTests/threads/cve/mc-df-wasm-compile-race.js — `--useJSThreads=1 --useWebAssembly=1`
- JSTests/threads/cve/mc-df-delete-reuse.js — `--useJSThreads=1`

All four are deterministic-oracle / nondeterministic-interleaving:
trivially green under the phase-1 GIL, signal-bearing GIL-off, and
amplifier-ready (Tools/threads/amplify.sh, TSAN no-JIT target). None spawn
unbounded work; all join every thread (annex T2 conventions).
