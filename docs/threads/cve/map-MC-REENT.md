# map-MC-REENT — Side-effect re-entrancy under a single-mutation assumption

Status: surface map, 2026-06-07; **re-verified 2026-06-15** against the
post-ungil tree (GIL removal landed 43fd5fb94387; scalability passes through
3345f3bfe501). Defensive audit artifact for `jarred/threads`
(`--useJSThreads`). Class definition: `docs/threads/CVE-AUDIT.md` §MC-REENT;
per-CVE detail: `docs/threads/cve/jsengine-sab.md` class S.

Mechanism: user JS runs (valueOf/toPrimitive coercion, getter, proxy trap,
import resolution) inside a privileged operation that pre-validated state.
Exemplars: CVE-2017-5122 (V8 `Table.grow` `Symbol.toPrimitive` re-entry),
CVE-2017-15401 (import-object getter mid-instantiation). This is the
*sequential twin* of "a second mutator appears mid-operation": GIL-on it is a
same-thread interleaving bug; GIL-off every such window is additionally a real
MC-DF/MC-GROW race, because the re-entry point is now also a point where any
OTHER thread can mutate.

Audit method (per CVE-AUDIT "use as a finder"): enumerate every place in OUR
new code (api/objectmodel/ungil workstreams) where a privileged step can call
out to user JS, or where validation and the mutation it licenses are separated
by a callout; check the validated state is either immune to the callout,
re-derived after it, or the callout is ordered strictly before validation.
Inherited (pre-fork) S-shaped JSC sites are cross-referenced to the MC-DF /
MC-GROW maps rather than re-enumerated here (CVE-AUDIT priority note 7).

Line numbers reference the tree at re-audit time (branch `jarred/threads`,
post-ungil + scalability passes). Paths are under `Source/JavaScriptCore/`
unless noted.

---

## S1. Atomics-on-property: key/operand coercion ordering

Surface: the SPEC-api §4.5 property-Atomics family advertises every op as
"one atomic step", yet four of its inputs are coerced and coercion runs user
JS: the property key (`ToPropertyKey`), the RMW operand (`ToNumber`/`ToInt32`),
the wait timeout (`ToNumber`), the notify count (`ToIntegerOrInfinity`).

Evidence the ordering is coercion-FIRST, validation+mutation AFTER, with no
callout in between:

- `runtime/AtomicsObject.cpp:241` — property-path dispatch coerces the key
  (`args[1].toPropertyKey`) before any probe; same at `:440`, `:631`, `:682`,
  `:773`.
- `runtime/ThreadAtomics.cpp:212-218` — the GIL-on atomicity contract comment:
  "that holds only if NO user JS can run between the own-property read and the
  write below (operand coercions happen before the read)".
- GIL-on RMW: `ThreadAtomics.cpp:1027-1038` coerces the operand
  (`toInt32`/`toNumber`, "may run JS") BEFORE `getOwnPropertyForAtomics`
  (`:1042`) reads the slot; the read→compute→
  `putExistingOwnDataPropertyForAtomics` tail (`:1040-1084`) contains no
  callout.
- GIL-off RMW: `ThreadAtomics.cpp:837-858` ("Operand coercions first (may run
  JS), exactly as the GIL bodies order them; the atomic step is the accessor
  call below") builds the `AtomicSlotRequest` from the coerced operand before
  the probe→accessor loop.
- wait/waitAsync: `ThreadAtomics.cpp:1427` / `:1605` parse the timeout
  (`parseAtomicsTimeout`, `:1289-1296`, runs `toNumber`) BEFORE the step-1
  value read (`:1447`, `:1627`) — and, post-§C.3(b), BEFORE the entire
  dequeue-and-restart loop, so the coercion runs exactly once and never
  inside a restart iteration.
- notify: `runtime/AtomicsObject.cpp:788` coerces the count before the waiter
  table is consulted (and notify pre-validates nothing the count coercion
  could falsify).

Adversarial self-check — what runs between the read and the write?

- `sameValueZeroForAtomics` (`ThreadAtomics.cpp:200-210`): `sameValue` is
  non-coercing; string comparison can resolve ropes, which allocates and can
  throw OOM but never calls user JS. GIL-on the GIL is never dropped there, so
  the step stays atomic; GIL-off the CAS accessor instead returns
  `NeedsStringResolution` and ropes are resolved OUTSIDE any lock followed by
  a full re-probe (`ThreadAtomics.cpp:800-812`, `:891`) — the resolution
  callout is hoisted out of the atomic step by construction.
- GC at allocation points (rope resolution, `constructEmptyObject` in
  waitAsync): GC runs no synchronous user JS in JSC; out of class.

Verdict: **immune-by-construction**. Every user-JS-capable coercion is
sequenced strictly before the validate+act step; the GIL-off bodies re-derive
all validated state in the probe→accessor restart loop anyway (SPEC-ungil
ANNEX C1; OM I34 provenance re-validation), so even a hypothetical future
callout inserted into the window degrades to a Restart, not a stale-state act.

## S2. Atomics-on-property: reentrant receivers inside the atomic step

Surface: the one unavoidable callout-shaped call inside the step is the
own-property probe itself — `methodTable()->getOwnPropertySlot` — plus
store's `isExtensible`. On a Proxy/GlobalProxy these run arbitrary trap JS
which (GIL-on) can reach a GIL-dropping park site (join, cond.wait, contended
hold, property wait) mid-step, handing another thread the window — exactly
the CVE-2017-5122 shape transplanted onto our API.

Evidence:

- Gate 1 rejects `ProxyObjectType`/`GlobalProxyType` receivers with a
  TypeError before any probe: `ThreadAtomics.cpp:140-147` (GIL-on),
  `:313-317` (GIL-off twin). Rationale block `:212-230` names the
  cross-thread TOCTOU explicitly. Recorded as landed deviation D3
  (`docs/threads/INTEGRATE-api.md`, carried across the U-T10 re-home per
  SPEC-ungil ANNEX C.2).
- After Gate 1 the in-source claim is "the method-table probe below runs no
  user JS (other exotic getOwnPropertySlot implementations may reify lazy
  properties or allocate, but never call out to JS), and
  atomicsStoreOnProperty's isExtensible() is a plain structure-flag read"
  (`:140-144`). Spot-checked: lazy reification (function name/length),
  module-namespace TDZ throws, typed-array index probes — engine code, no JS.
  CustomAccessor/CustomValue getters are NOT invoked by an
  `InternalMethodType::GetOwnProperty` probe (classification only); the value
  read is a raw `getDirect(offset)`.
- Gate 2 rejects own properties not backed by plain structure/butterfly
  storage (`:158-198`), so the later write targets exactly the probed slot —
  no exotic setter can be reached by the mutation half.
- GIL-off, the second structure read at `:370-396` re-validates accessor-ness
  against the SAME structure the `{offset, structureID}` provenance is taken
  from, and excludes `CustomValue` from the lock-free arms (`:388`) — the
  U-T10 amend that closed the racing data→accessor reconfiguration
  (type-confusion CAS over a GetterSetter).

Verdict: **immune-by-construction** (Gate 1 + Gate 2 + GIL-off provenance
re-validation). Residual observation (out of class, no user JS involved):
GIL-on classification of a hypothetical `CustomValue` slot that answers
`slot.isValue()` relies on the Gate-2 `structure()->get` attribute read
rejecting nothing — the GIL-off twin rejects
`Accessor|CustomAccessor|CustomValue` explicitly (`:388`) while the GIL-on
body does not. GIL-on this is at worst an MC-VAL validator/consumer skew on a
slot kind no plain-object test can construct; routed to map-MC-VAL for the
attribute-preservation check rather than tracked here.

## S3. Atomics.store Missing-arm: validate-then-add

Surface: `Atomics.store(o, k, v)` on an ABSENT key is the one §4.5 op whose
privileged step is a structure transition (property ADD), with two validated
facts (`Missing`, `isExtensible`) licensing it.

S3a — GIL-on body (`ThreadAtomics.cpp:930-956`): probe → `isExtensible`
(`:943`, plain flag read post-Gate-1) → `putDirectMayBeIndex` (`:955`). No
callout anywhere in the window; the GIL is never dropped.
Verdict: **immune-by-construction** (GIL-on only; SPEC-api §4.5 step
atomicity).

S3b — GIL-off NAMED add (`ThreadAtomics.cpp:758`): the original
probe(Missing)→isExtensible→put sequence was a CONFIRMED Missing-arm TOCTOU
(racing `defineProperty(accessor/non-writable)` or `preventExtensions`
silently clobbered/overtaken — `docs/threads/INTEGRATE-ungil.md` U-T10
item 3). Fixed: named adds route through
`JSObject::putDirectForAtomicsMissingAdd` (`runtime/JSObject.h:1085`), which
re-derives existence and extensibility inside the SAME OM §2/E4 loop
iteration whose structureID-CAS publishes the add; a lost race returns a
non-null error and the store body RESTARTS, so the fresh probe throws the
precise D3/D7/non-extensible TypeError. Corpus:
`JSTests/threads/objectmodel/property-store-missing-define-race.js`.
Verdict: **immune-by-construction** (publication-coupled re-validation; OM
I21/I37 govern the underlying transition).

S3c — GIL-off INDEXED add: **CLOSED (fix landed; test green)**. The former
KNOWN RESIDUAL (unconditional `putDirectIndex` on the indexed Missing arm,
INTEGRATE-ungil U-T10 item 3) is replaced by
`JSObject::putDirectIndexForAtomicsMissingAdd` (`ThreadAtomics.cpp:527`,
wired `:737`), the indexed twin of the named conditional add: a
shape-dispatched loop whose sparse terminal is a conditional `map->add` +
locked value publish in ONE object-cellLock window (the same lock
`defineOwnIndexedProperty` holds around its `map->add`); `!isNewEntry` = lost
race => the store body RESTARTS and the fresh probe throws the precise D3/D7
TypeError. CVE-suite close-out added three mutually reinforcing pieces (the
amplifier found two distinct windows):
(1) the AS in-vector fill arm re-checks, under the object cellLock, that no
sparse map governs the index (sparseMode or an existing entry at i) before
writing the vector slot — an existing entry is a lost race (restart
reclassifies), sparse-mode-without-entry falls through to the map
conditional-add (returning lost-race there would livelock: the re-probe
still answers Missing on that settled state);
(2) `increaseVectorLength` refuses sparse-mode storage under its cellLock —
GIL-on every caller checks `sparseMode()` atomically before growing, GIL-off
the mode can flip between the caller's unlocked decision and the locked
body, and a vector grown over sparse entries makes every map entry below
vectorLength UNREADABLE (the AS lookup is "if (i < vectorLength) vector ELSE
IF (map)") while a later in-vector fill SHADOWS its descriptor;
(3) `defineOwnIndexedProperty`'s locked add window re-establishes the
sparse-mode invariant (vectorLength == 0, strays migrated into the map)
before the add — the blank-receiver arm of
`ensureArrayStorageExistsAndEnterDictionaryIndexingMode` publishes
`createArrayStorage(0,0)` BEFORE the map/sparseMode pair, and a racing
attribute-0 add that grew/filled the vector in that map-less window is
otherwise an unreachable-GIL-on heap state in both shadow directions; a
migrated value at the defined index surfaces as the current {value, attrs 0}
property and takes the reconfiguration path (store-then-define
linearization). Narrowed residual recorded in the
helper's header comment: a racing `preventExtensions` can still be overtaken
within one lock-internal interval (extra plain property, never a descriptor
clobber — the same state pre-fork code produced).
Test: `JSTests/threads/cve/mc-reent-store-missing-indexed-define-race.js`
(40/40 GIL-off Release, 3/3 GIL-on at close-out).

## S4. Lock.hold / asyncHold / Condition.asyncWait: hold consumption by user fn

Surface: the lock primitives run user `fn()` INSIDE the privileged
hold — the designed re-entrancy site of the whole API. `fn` can consume the
hold out from under the epilogue (`cond.asyncWait` 4.3(a)/(b)), spawn
threads, re-enter `hold` (recursion), or call the minted release fn twice.

Evidence the post-callout state is re-derived, never assumed:

- Sync-hold epilogue guard: `runtime/LockObject.cpp:1002-1006` /
  `:1037-1040` — after `fn` returns, release runs only
  `if (state.heldByCurrentThread())`; a `cond.asyncWait`-consumed hold skips
  it (SPEC-api 5.3 "hold epilogue skips release").
- Async grant: single-consumption CAS `ticket->tryConsume()` —
  `LockObject.cpp:748` (implicit post-fn release E) and `:768` (explicit
  release fn; second call → 4.2 Error). `markGrantDelivered`
  (`:735`, `:764`) gates consumption so `fn` always starts with the lock
  genuinely held (I23).
- Recursion via re-entry: `:814-817` — `m_holder` AND the D10
  `m_asyncGrantRunner` both count as "held by the current thread", closing
  the sync-inside-delivered-async-fn self-deadlock.

Verdict: **immune-by-construction** (SPEC-api 4.2/4.3(a)/5.3, invariants
I6/I23; existing corpus `JSTests/threads/sync/**` exercises the
consumed-hold and double-release shapes; GIL-off the same guards stand
because every consumption/transition is a CAS or runs under the grant
token).

## S5. Thread/Lock/Condition/ThreadLocal constructors: prototype get()

Surface: `constructThread` fetches `callFrame->jsCallee()->get(globalObject,
prototype)` — a real user-JS callout (subclass with an accessor `prototype`,
`Reflect.construct` shenanigans) — inside thread spawn.

Evidence: `runtime/ThreadObject.cpp:423-428` orders the get BEFORE the
ThreadState allocation precisely because it "can run JS / throw" (a TS
allocated first would leak as a forever-Running entry against `maxJSThreads`,
SPEC-api I17); everything after the allocation is infallible straight-line up
to `Thread::create`. The only state validated before the callout is
callability, and callability is an immutable per-cell fact — the callout
cannot falsify it. Same shape in `constructLock` and the other constructors.
The `Thread.current` minting path (`:660-680`) explicitly resolves the
prototype "without running any JS" via `getDirect` on the non-forced
constructor slot, so it is not a callout site at all.

Verdict: **immune-by-construction** (callout precedes all privileged state;
surviving validation is immutable).

## S6. Thread.restrict: exclusion validation → conversion → registration

Surface: `Thread.restrict(o)` validates the receiver (Dev 8/11 exclusions,
`hijacksIndexingHeader`, method-table allowlist), then mutates it
(`ensureArrayStorage`, possibly `convertToUncacheableDictionary`), then
registers ownership.

Evidence: `runtime/ThreadObject.cpp:712-870` is straight-line engine code:
exclusion detection ptr-compares `o` against the global's slots and "never
force[s] lazy slots" (SPEC-api Dev 8 — forcing them would itself be a
callout); `ensureArrayStorage`/`convertToUncacheableDictionary` allocate and
transition but run no user JS. No coercion: a non-object is rejected, the
argument is used as-is.

Verdict: **immune-by-construction** for MC-REENT (no callout in the window).
Cross-thread restrict-vs-mutate and restrict-vs-restrict races are MC-DF /
MC-LOCK scope (CAE ownership check; MC-HAND S6 closed 2026-06-10) — see
those maps.

## S7. Property wait/waitAsync: read→SVZ→enqueue step

Surface: `Atomics.wait(o,k,exp,t)` validates (own data k, SVZ equal) and then
enqueues; a callout between read and enqueue would be the classic lost-wakeup
re-entry.

Evidence: timeout coercion is hoisted before the read AND before the §C.3(b)
restart loop (S1; `:1427`/`:1605`), so it runs exactly once. Between the
step-1 read (`ThreadAtomics.cpp:1447`/`:1627`) and the enqueue only SVZ
comparison (rope resolution at most — no user JS), the §C.3(b) provenance
probe (`:1469-1475`/`:1646-1653` — engine-only, post-Gate-1), and waiter
allocation run. waitAsync's `constructEmptyObject`/`JSPromise::create`
allocate only; the `ticketRetirer` scope-exit (`:1618-1621`) is engine-only.

Verdict: **immune-by-construction** for MC-REENT. The previous cross-class
note ("the I10 banner argument is void GIL-off; routed to MC-WAIT") is now
DISCHARGED: the §C.3(b) under-listLock SVZ re-validation
(`sameValueZeroForAtomicsUnderListLock`, `ThreadAtomics.cpp:1348`, applied
`:1407`) landed and closed MC-WAIT S3a (CVE-AUDIT-STATUS 2026-06-10). For
MC-REENT this strengthens the verdict: even a hypothetical callout inserted
into the read→enqueue window would now be re-derived against under the list
lock before the waiter is parked.

## S8. Inherited S-shaped JSC sites (the finder duty)

Every pre-fork re-entrancy-hardened site in JSC — `putByIndexBeyondVectorLength`
proto-chain interception, `defineOwnProperty` descriptor-getter re-validation,
species-constructed array ops, sort's comparator handling, JSON reviver
paths — is, per this class's definition, a pre-located MC-DF/MC-GROW window
under N mutators. Their post-ungil soundness is NOT carried by the old
sequential re-checks but by the object-model protocols those paths now sit
on: E5 full §2 dispatch in slow paths, RESTART-on-divergence in §4.2/§4.3,
caller re-dispatch on butterfly CAS failure (§4.4/GT10), I21 (no lost adds /
torn values / structure-butterfly mismatch) and I34 (no
callout/poll/allocation between offset acquisition and access without
re-validation) — SPEC-objectmodel.md §3-§6, §8. The per-site enumeration and
race tests live in map-MC-DF and map-MC-GROW (CVE-AUDIT priority 1); this map
records only the rule that converts the grep into verdicts: a sequential
re-check AFTER a callout is sufficient post-ungil ONLY if the re-check and
the mutation it licenses publish atomically (E4/DCAS/cell-lock), otherwise
the site must sit on a §2-dispatching path.

---

## Verdict summary

| # | Surface | Verdict | Governing clause |
|---|---|---|---|
| S1 | Atomics prop key/operand/timeout/count coercion | immune-by-construction | SPEC-api §4.5 step atomicity; coercion-first ordering (ThreadAtomics.cpp:212-218, :837-858, :1027-1038) |
| S2 | Reentrant receivers in the atomic step | immune-by-construction | D3 Proxy/GlobalProxy gate; SPEC-ungil ANNEX C1; OM I34 provenance |
| S3a | store Missing-arm, GIL-on | immune-by-construction (GIL-on only) | SPEC-api §4.5; no callout in window |
| S3b | store Missing-arm, GIL-off NAMED | immune-by-construction | putDirectForAtomicsMissingAdd publication-coupled re-validation (U-T10 item 3); OM I21/I37 |
| S3c | store Missing-arm, GIL-off INDEXED | closed (fix landed: putDirectIndexForAtomicsMissingAdd + in-vector map-governance gate) | OM §4.6/I31; one-cellLock add+publish window |
| S4 | hold/asyncHold/asyncWait hold consumption | immune-by-construction | SPEC-api 4.2/4.3(a)/5.3, I23; epilogue guard + consume CAS |
| S5 | Constructor prototype get() | immune-by-construction | callout-before-privileged-state ordering (ThreadObject.cpp:423-428); I17 |
| S6 | Thread.restrict window | immune-by-construction | no callout; Dev 8/11 lazy-slot rule |
| S7 | wait read→enqueue step | immune-by-construction (REENT); MC-WAIT note DISCHARGED (§C.3(b) landed) | SPEC-api 5.6/F4; SPEC-ungil §C.3(b) under-listLock re-validation |
| S8 | Inherited S-shaped sites | cross-referenced to MC-DF/MC-GROW | OM E5/RESTART/I21/I34 |

## Tests written (both PASSING in the `--cve` gate as of 2026-06-10)

- `JSTests/threads/cve/mc-reent-coercion-order.js` — deterministic pin of the
  S1 ordering contract: operand/key coercion side effects must be fully
  ordered before the atomic step (valid GIL-on today and GIL-off later; a
  regression that moves a coercion inside the step flips an exact expected
  value/exception).
- `JSTests/threads/cve/mc-reent-store-missing-indexed-define-race.js` — S3c
  susceptibility: racing indexed `Atomics.store` Missing-add vs
  `defineProperty(accessor / non-writable)`; every legal linearization leaves
  the define's result in place, so a surviving plain store value is a
  violation. Amplifier-ready (Tools/threads/amplify.sh; bounded blocking,
  all threads joined). PASSING since the S3c conditional-add landed; a
  future failure is a regression of that fix, not an expected failure.
