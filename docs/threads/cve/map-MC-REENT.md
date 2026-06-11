# map-MC-REENT — Side-effect re-entrancy under a single-mutation assumption

Status: surface map, 2026-06-07. Defensive audit artifact for `jarred/threads`
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

Line numbers reference the tree at audit time (branch `jarred/threads`,
post-AB-17b). Paths are under `Source/JavaScriptCore/` unless noted.

---

## S1. Atomics-on-property: key/operand coercion ordering

Surface: the SPEC-api §4.5 property-Atomics family advertises every op as
"one atomic step", yet four of its inputs are coerced and coercion runs user
JS: the property key (`ToPropertyKey`), the RMW operand (`ToNumber`/`ToInt32`),
the wait timeout (`ToNumber`), the notify count (`ToIntegerOrInfinity`).

Evidence the ordering is coercion-FIRST, validation+mutation AFTER, with no
callout in between:

- `runtime/AtomicsObject.cpp:217-227` — property-path dispatch coerces the key
  (`args[1].toPropertyKey`) before any probe; same at `:418`, `:608-609`,
  `:660`, `:751`.
- `runtime/ThreadAtomics.cpp:179-209` — the GIL-on atomicity contract comment:
  "that holds only if NO user JS can run between the own-property read and the
  write below (operand coercions happen before the read)".
- GIL-on RMW: `ThreadAtomics.cpp:716-727` coerces the operand
  (`toInt32`/`toNumber`) BEFORE `getOwnPropertyForAtomics` (`:731`) reads the
  slot; the read→compute→`putExistingOwnDataPropertyForAtomics` tail
  (`:729-773`) contains no callout.
- GIL-off RMW: `ThreadAtomics.cpp:531-553` ("Operand coercions first (may run
  JS), exactly as the GIL bodies order them; the atomic step is the accessor
  call below") builds the `AtomicSlotRequest` from the coerced operand before
  the probe→accessor loop.
- wait/waitAsync: `ThreadAtomics.cpp:977-978` / `:1111-1112` parse the timeout
  (`parseAtomicsTimeout`, `:960-970`, runs `toNumber`) BEFORE the step-1 value
  read (`:981`, `:1114`).
- notify: `ThreadAtomics.cpp:1208-1214` coerces the count before the waiter
  table is consulted (and notify pre-validates nothing the count coercion
  could falsify).

Adversarial self-check — what runs between the read and the write?

- `sameValueZeroForAtomics` (`ThreadAtomics.cpp:167-177`): `sameValue` is
  non-coercing; string comparison can resolve ropes, which allocates and can
  throw OOM but never calls user JS. GIL-on the GIL is never dropped there, so
  the step stays atomic; GIL-off the CAS accessor instead returns
  `NeedsStringResolution` and ropes are resolved OUTSIDE any lock followed by
  a full re-probe (`ThreadAtomics.cpp:498-514`) — the resolution callout is
  hoisted out of the atomic step by construction.
- GC at allocation points (rope resolution, `constructEmptyObject` in
  waitAsync `:1117`): GC runs no synchronous user JS in JSC; out of class.

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
  TypeError before any probe: `ThreadAtomics.cpp:107-114` (GIL-on),
  `:279-283` (GIL-off twin). Rationale block `:179-200` names the
  cross-thread TOCTOU explicitly. Recorded as landed deviation D3
  (`docs/threads/INTEGRATE-api.md`, carried across the U-T10 re-home per
  SPEC-ungil ANNEX C.2).
- After Gate 1 the in-source claim is "the method-table probe below runs no
  user JS (other exotic getOwnPropertySlot implementations may reify lazy
  properties or allocate, but never call out to JS), and
  atomicsStoreOnProperty's isExtensible() is a plain structure-flag read"
  (`:107-110`). Spot-checked: lazy reification (function name/length),
  module-namespace TDZ throws, typed-array index probes — engine code, no JS.
  CustomAccessor/CustomValue getters are NOT invoked by an
  `InternalMethodType::GetOwnProperty` probe (classification only); the value
  read is a raw `getDirect(offset)` (`:163`).
- Gate 2 rejects own properties not backed by plain structure/butterfly
  storage (`:125-164`), so the later write targets exactly the probed slot —
  no exotic setter can be reached by the mutation half.
- GIL-off, the second structure read at `:336-361` re-validates accessor-ness
  against the SAME structure the `{offset, structureID}` provenance is taken
  from, and excludes `CustomValue` from the lock-free arms (`:343-358`) — the
  U-T10 amend that closed the racing data→accessor reconfiguration
  (type-confusion CAS over a GetterSetter).

Verdict: **immune-by-construction** (Gate 1 + Gate 2 + GIL-off provenance
re-validation). Residual observation (out of class, no user JS involved):
GIL-on classification of a hypothetical `CustomValue` slot that answers
`slot.isValue()` relies on the Gate-2 `structure()->get` attribute read at
`:156-163` rejecting nothing — the GIL-off twin rejects
`Accessor|CustomAccessor|CustomValue` explicitly (`:354`) while the GIL-on
body does not. GIL-on this is at worst an MC-VAL validator/consumer skew on a
slot kind no plain-object test can construct; routed to map-MC-VAL for the
attribute-preservation check rather than tracked here.

## S3. Atomics.store Missing-arm: validate-then-add

Surface: `Atomics.store(o, k, v)` on an ABSENT key is the one §4.5 op whose
privileged step is a structure transition (property ADD), with two validated
facts (`Missing`, `isExtensible`) licensing it.

S3a — GIL-on body (`ThreadAtomics.cpp:609-647`): probe → `isExtensible`
(`:632`, plain flag read post-Gate-1) → `putDirectMayBeIndex` (`:644`). No
callout anywhere in the window; the GIL is never dropped.
Verdict: **immune-by-construction** (GIL-on only; SPEC-api §4.5 step
atomicity).

S3b — GIL-off NAMED add (`ThreadAtomics.cpp:444-462`): the original
probe(Missing)→isExtensible→put sequence was a CONFIRMED Missing-arm TOCTOU
(racing `defineProperty(accessor/non-writable)` or `preventExtensions`
silently clobbered/overtaken — `docs/threads/INTEGRATE-ungil.md` U-T10
item 3). Fixed: named adds route through
`JSObject::putDirectForAtomicsMissingAdd` (`runtime/JSObject.h:1002`), which
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
`JSObject::putDirectIndexForAtomicsMissingAdd` (ThreadAtomics.cpp), the
indexed twin of the named conditional add: a shape-dispatched loop whose
sparse terminal is a conditional `map->add` + locked value publish in ONE
object-cellLock window (the same lock `defineOwnIndexedProperty` holds around
its `map->add`); `!isNewEntry` = lost race => the store body RESTARTS and the
fresh probe throws the precise D3/D7 TypeError. CVE-suite close-out added three
mutually reinforcing pieces (the amplifier found two distinct windows):
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

- Sync-hold epilogue guard: `runtime/LockObject.cpp:635-639` — after `fn`
  returns, release runs only `if (state.heldByCurrentThread())`; a
  `cond.asyncWait`-consumed hold skips it (SPEC-api 5.3 "hold epilogue skips
  release").
- Async grant: single-consumption CAS `ticket->tryConsume()` —
  `LockObject.cpp:472-474` (implicit post-fn release E) and `:493-495`
  (explicit release fn; second call → 4.2 Error). `markGrantDelivered`
  (`:460`, `:489`) gates consumption so `fn` always starts with the lock
  genuinely held (I23).
- Recursion via re-entry: `:535-540` — `m_holder` AND the D10
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

Evidence: `runtime/ThreadObject.cpp:351-357` orders the get BEFORE the
ThreadState allocation precisely because it "can run JS / throw" (a TS
allocated first would leak as a forever-Running entry against `maxJSThreads`,
SPEC-api I17); everything after the allocation is infallible straight-line up
to `Thread::create` (`:354-355`, `:366-397`). The only state validated before
the callout is callability (`:347-349`), and callability is an immutable
per-cell fact — the callout cannot falsify it. Same shape in `constructLock`
(`runtime/LockObject.cpp:515-517`) and the other constructors.

Verdict: **immune-by-construction** (callout precedes all privileged state;
surviving validation is immutable).

## S6. Thread.restrict: exclusion validation → conversion → registration

Surface: `Thread.restrict(o)` validates the receiver (Dev 8/11 exclusions,
`hijacksIndexingHeader`, method-table allowlist), then mutates it
(`ensureArrayStorage`, possibly `convertToUncacheableDictionary`), then
registers ownership.

Evidence: `runtime/ThreadObject.cpp:739-833` is straight-line engine code:
exclusion detection ptr-compares `o` against the global's slots and "never
force[s] lazy slots" (SPEC-api Dev 8 — forcing them would itself be a
callout); `ensureArrayStorage`/`convertToUncacheableDictionary` allocate and
transition but run no user JS. No coercion: a non-object is rejected, the
argument is used as-is.

Verdict: **immune-by-construction** for MC-REENT (no callout in the window).
Cross-thread restrict-vs-mutate and restrict-vs-restrict races are MC-DF /
MC-LOCK scope (CAE ownership check, `ThreadObject.cpp:788-795`) — see those
maps.

## S7. Property wait/waitAsync: read→SVZ→enqueue step

Surface: `Atomics.wait(o,k,exp,t)` validates (own data k, SVZ equal) and then
enqueues; a callout between read and enqueue would be the classic lost-wakeup
re-entry.

Evidence: timeout coercion is hoisted before the read (S1); between the
step-1 read (`ThreadAtomics.cpp:981`) and the enqueue (`:1010`) only SVZ
comparison (rope resolution at most — no user JS) and waiter allocation run.
Same for waitAsync (`:1114-1143`; `constructEmptyObject`/`JSPromise::create`
allocate only).

Verdict: **immune-by-construction** for MC-REENT. Cross-class note (routed to
map-MC-WAIT, not a REENT finding since the window contains no user JS): the
step-1 comment "JSLock held from the step-1 read through the enqueue closes
the lost store+notify window (I10)" (`:1001-1004`) rests on the JSLock
serializing mutators — GIL-off it does not; whether a store+notify landing
between `:981` and `:1010` can strand a waiter until timeout needs the
MC-WAIT audit to discharge against SPEC-ungil annex W's wait-episode rules.

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
| S1 | Atomics prop key/operand/timeout/count coercion | immune-by-construction | SPEC-api §4.5 step atomicity; coercion-first ordering (ThreadAtomics.cpp:179-209) |
| S2 | Reentrant receivers in the atomic step | immune-by-construction | D3 Proxy/GlobalProxy gate; SPEC-ungil ANNEX C1; OM I34 provenance |
| S3a | store Missing-arm, GIL-on | immune-by-construction (GIL-on only) | SPEC-api §4.5; no callout in window |
| S3b | store Missing-arm, GIL-off NAMED | immune-by-construction | putDirectForAtomicsMissingAdd publication-coupled re-validation (U-T10 item 3); OM I21/I37 |
| S3c | store Missing-arm, GIL-off INDEXED | closed (fix landed: putDirectIndexForAtomicsMissingAdd + in-vector map-governance gate) | OM §4.6/I31; one-cellLock add+publish window |
| S4 | hold/asyncHold/asyncWait hold consumption | immune-by-construction | SPEC-api 4.2/4.3(a)/5.3, I23; epilogue guard + consume CAS |
| S5 | Constructor prototype get() | immune-by-construction | callout-before-privileged-state ordering (ThreadObject.cpp:351-357); I17 |
| S6 | Thread.restrict window | immune-by-construction | no callout; Dev 8/11 lazy-slot rule |
| S7 | wait read→enqueue step | immune-by-construction (REENT); MC-WAIT note routed | SPEC-api 5.6/F4; I10 premise flagged for MC-WAIT GIL-off |
| S8 | Inherited S-shaped sites | cross-referenced to MC-DF/MC-GROW | OM E5/RESTART/I21/I34 |

## Tests written (NOT run — tree is mid-bring-up; execute post-ungil)

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
  all threads joined).
