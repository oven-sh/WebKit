# MC-LOCK — Lock-state / state-machine transition race: mapping to our threads surface

Mechanism class (CVE-AUDIT.md §MC-LOCK): a multi-step state machine (lock-word
encodings, COW state, allocator-carrier lifecycle, flag transitions) admits an
interleaving that observes a mid-transition state or skips/repeats a
transition. The standing lesson (JEP 374): an asymmetric lock optimization
whose "deoptimize the lock" path must stop or introspect another thread is a
permanent race generator. Exemplars mapped: JDK-6444286 / JDK-8240723 (biased
locking revocation/epoch), HotSpot mark-word inflation races, CVE-2016-5195
(Dirty COW), ERTS allocator-carrier deletion deadlock.

Status note: every surface below is GIL-masked in phase 1 (single mutator at a
time); all verdicts and tests are about the post-ungil N-mutator world. Tests
live in `JSTests/threads/cve/mc-lock-*.js` and are EXECUTED POST-UNGIL ONLY.

---

## S1. The per-object cell lock word itself (acquire / release / park transitions)

Surface:
- `Source/JavaScriptCore/runtime/JSCellInlines.h:322-346` (`JSCellLock::lock/tryLock/unlock/isLocked` over the `m_indexingTypeAndMisc` byte),
- `Source/JavaScriptCore/runtime/JSCell.cpp:283-293` (`lockSlow/unlockSlow` via `IndexingTypeLockAlgorithm`),
- `Source/WTF/wtf/LockAlgorithmInlines.h:160-215` (ParkingLot compareAndPark / unparkOne),
- `Source/JavaScriptCore/runtime/ConcurrentButterfly.h:286-309` (`cellHeaderVolatileMask`: held 0x40, parked 0x80, m_cellState lane, per-cell type-flags bit).

Governing spec: SPEC-objectmodel §3.0 (volatile vs semantic header bytes, GT#2),
I26 (every header CAS/DCAS copies volatile lanes verbatim from the freshest
read), §6 lock ordering (SAL < JSCellLock < Structure::m_lock, O1-O4),
CVE-AUDIT MC-LOCK "our surface" note ("deliberately symmetric, no inflation —
keep it that way").

Verdict: **immune-by-construction.**

Why the mechanism cannot occur:
1. The lock has exactly three states (free / held / held+parked) driven by
   symmetric byte-CAS through WTF::LockAlgorithm. There is no biased mode, no
   inflation to a fat monitor, no epoch, and therefore no cross-thread
   revocation path at all — the JEP-374-shaped generator is absent by design,
   and CVE-AUDIT charters keeping it absent.
2. The classic mark-word hazard — a wide header write clobbering concurrently
   CASed lock bits (lost unpark = wedged waiter; the JDK-6444286 shape) — is
   closed by the §3.0 lane discipline: 0x40/0x80 are *volatile* lanes, every
   64/128-bit header CAS folds them from the freshest read
   (`mergeVolatileHeaderBits`, all DCAS sites: ConcurrentButterfly.cpp:719-720,
   1219-1220, 1402-1404, 1643-1644, 1854), and a CAS that observes a semantic
   lane change either RELEASE_ASSERTs (under the lock) or abandons (lock-free).
   The mask is static_asserted byte-exactly (ConcurrentButterfly.h:306-308).
3. Conversely a waiter's parked-bit byte-CAS racing the holder's 8-byte
   semantic publication simply retries: byte-CAS failure loops in ParkingLot,
   DCAS failure folds. No interleaving can drop a state or observe a state
   outside the enumerated three.

Adversarial self-check: the one place this argument can rot is *emitted* code —
SPEC-objectmodel's note requires JIT-emitted §5.5 DCAS sequences to use the
same volatile mask (recorded INTEGRATE-objectmodel.md round 4). That is a
per-tier audit obligation (MC-JIT territory), not a runtime state-machine hole;
flagged for the thread-scanners pass rather than a JS-level test.

## S2. §3.0 header/butterfly transition state machine (nuke window = the visible mid-transition state)

Surface: the fenced nuke order everywhere a {structureID, butterfly} pair is
republished — `JSObjectInlines.h:155-166` (`nukeStructureAndSetButterflyConcurrent`),
`ConcurrentButterfly.cpp` locked protocols (taxonomy (a)-(d) + RESTART at
:1036, :1083, :1189-1260, :1311), CoW publication :1628-1660, the per-event-stop
publication legs in `JSObject.cpp:2324-2331`.

Governing spec: SPEC-objectmodel §3.0 (4-step CAS loop; step 4 = abandon
lock-free / RELEASE_ASSERT under lock), M5 (never decode a nuked ID; raw-bits
spin), M8 (PA fenced order, I36), O2 (nuke windows bounded, poll-free), I21
(no lost adds / torn pairs), §4.3(b2) (a racing lock-free SW flip against a
payload-*replacing* locked publication must RESTART, never merge — the
lost-write case).

Verdict: **immune-by-construction** (with the corpus as the standing witness).

Why: the mid-transition state *is designed to be observable* (the nuked ID) and
every reader/writer is required to classify it: readers spin on raw bits (M5)
across a window that is poll-free and allocation-free (O2), so the window is
bounded by straight-line code and cannot straddle a safepoint; writers
arbitrate by CAS on the ID lane, and the failure taxonomy is exhaustive with
the dangerous repeat/skip case — re-publishing a stale copied payload over a
racing SW-flipped word — explicitly forbidden as (b2) and enforced by trap, not
convention: `storeTaggedButterflyWordConcurrent` (JSObjectInlines.h:111-128)
RELEASE_ASSERTs the b1-only fold, so a protocol escape crashes deterministically
instead of losing a store. Repeating a transition is impossible because the ID
CAS consumes the pre-state exactly once.

Adversarial self-check: exhaustiveness of the taxonomy is an audit claim, not a
local proof — but it is the claim the whole objectmodel corpus
(`JSTests/threads/races/`, `objectmodel/`) plus the amplifier already targets,
and S6 below documents the one enumerated writer I found that *escapes* the
arbitration. The taxonomy itself needs no new test.

## S3. TTL watchpoint ownership elision (E4/E1-E3) — our biased-locking analog

Surface: SPEC-objectmodel §5 (E4, F1-F4, N1), `Structure.h:872-895` /
`Structure.cpp:275,331,394,1432,1702-1707` (TTL TID + fire functions),
E4 windows in `JSObject.cpp:2197-2238` (poll-free copy window under
`AssertNoGC`), F2 keying at `JSObject.cpp:3958-3977` (deletes), :2516-2526
(blank→ArrayStorage), `ConcurrentButterfly.cpp:1586-1591` (CoW F2).

Governing spec: I13 (TTL sets fire ONLY in VMManager STW), I10b (fire precedes
cell-lock acquisition and first publication; RESTART after the stop), I34
(owned offset-deref/publication windows are poll-free), I12 (writeThreadLocal
valid ⇒ no instance ever had SW=1), F4 (chain-fire in the same stop).

This is precisely the biased-locking shape: per-structure thread ownership lets
the owner skip the cell lock and CAS ("today's code incl. nuking"), and a
foreign actor must revoke that bias before acting. The JDK-6444286 /
JDK-8240723 failure mode was revocation that inspected or stopped *one* thread
asynchronously, racing the bias owner mid-critical-sequence.

Verdict: **immune-by-construction** — for every surface where the F2/F1 keying
is actually wired (see S6 for the one suspected gap).

Why the JVM mechanism cannot occur here: revocation is never per-thread.
A fire requires a full STW (I13); the owner's elided check→publish sequence is
straight-line, poll-free and allocation-free (I34, the `AssertNoGC` window at
JSObject.cpp:2212), so it cannot be suspended mid-window — the stop lands
either wholly before (owner re-checks `isStillValid` on its next operation and
falls to the locked path) or wholly after (the foreign trigger RESTARTs and
re-keys on the post-fire state, I10b). There is no "naked oop in the revoker"
equivalent because the revoker runs world-stopped and mutates nothing of the
owner's; monotone sets (fired-once, never re-armed) eliminate the JDK-8240723
epoch/rebias re-arming family outright — SPEC-objectmodel r12 records the
rebias charter as deliberately NOT implemented.

Adversarial self-check: the construction has two load-bearing legs — (a) every
foreign transition keys and fires *before* publishing, (b) every elided window
is genuinely poll-free. (b) is an audit (I34) backed by RELEASE_ASSERT
witnesses; (a) is falsified at one site: S6.

## S4. CopyOnWrite state machine — the Dirty COW analog

Surface: `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:1539-1676`
(`tryMaterializeCopyOnWriteButterflyForSharedWrite` — the single cell-locked
serialization point: F2-fire-first for foreign words, nuke-CAS, DCAS/M8
publication, word-stability RELEASE_ASSERTs), :1875-1905 (owner driver),
`JSObject.cpp:3319-3341` (owner `convertFromCopyOnWrite` rerouted flag-on —
the review-round-3 fix that removed the owner's plain-nuke racing the locked
materializer), `JSObjectInlines.h:205` (classify reroute), `JSObject.cpp:4206-4248`
(delete-path CoW carve-out).

Governing spec: SPEC-objectmodel §4.8 / I35: CoW words never reach SW=1 or
segmented; any foreign write/transition or owner SW=1 action materializes a
private flat butterfly FIRST, `casButterfly` expected = the exact CoW word,
loser re-dispatches; the CoW check precedes F1's SW DCAS.

Dirty COW was a state-machine race in which a writer's retry path landed a
write in the *shared* copy because the COW break and the write were separately
restartable steps. Our analogous hazards: (1) two materializers (owner+foreign)
racing the break — historically real here: pre-round-3 the owner's plain nuke
raced the locked foreign materializer exactly as JDK-6444286's revoker raced
its bias owner; (2) a writer landing a store through the shared
JSImmutableButterfly after losing the break race.

Why the current protocol holds: all materializers — owner included — serialize
on the cell lock with the CAS expected pinned to the exact CoW word, and the
RELEASE_ASSERTs at :1640-1642 make "a CoW word moved under the lock" or "SW
appeared on a CoW word" (I35 violations) deterministic traps. Losers re-dispatch
and re-classify; the triggering store only runs after the winner's tag is
re-read (§2/§3 probes), and write fast paths never match CoW indexing modes
(`JSObject.cpp:757`), so there is no path that stores through the shared copy.

Verdict: **needs-test** — the historical round-3 bug shows this exact surface
was the live one; it deserves a permanent regression storm with the
Dirty-COW-shaped oracle (a CoW *sibling* observing the write).
Test: `JSTests/threads/cve/mc-lock-cow-materialize-race.js`.

## S5. Safepoint/stop state machine + native park sites — the carrier-deadlock / wedged-revocation analog

Surface: `Source/JavaScriptCore/runtime/VMManager.h:103-195,252,348` (world
Mode machine RunAll/Stopped/RunOne and its transition contract),
`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:375-413` (30s stop
watchdog turning a non-converging stop into a deterministic fail-stop),
:415-457 (`parkSitePollAndParkForStopTheWorld` — the FIX-2 per-D9-quantum poll:
publish access-released, wake the conductor's sampler, ticket-park until the
stop word clears, re-acquire through the §A.3.2b gates).

Governing spec: SPEC-jit §5.6 / annex App. 5.6(d) (watchdog), UNGIL-HANDOUT
§A.3 (stop word / access-based conductor predicate, W1/D9 park-site split),
SPEC-objectmodel O2 ("never block on a safepoint holding these locks") — which
is what guarantees a *stopped* thread never holds a cell lock, so cell-lock
waiters always drain.

This is where MC-LOCK has already bitten us once: the AB-17B finding
(CVE-AUDIT MC-LOCK note) was exactly this class — a waiter parked in a native
wait holding heap access while the conductor's "parked implies access-released"
predicate could not converge, with the waiter's notifier itself fanned into the
same stop: a true deadlock, surfaced as the JSThreadsSafepoint watchdog
timeout on jettison-requested stops. FIX-2 closed it by making every D9-quantum
park site poll the stop word and release access before sleeping — the same
shape as the ERTS allocator-carrier deletion deadlock (a state machine whose
"wait" leg was invisible to the party trying to drive the transition).

Verdict: **needs-test** (regression). The fix is in-tree but the failure mode
is a convergence property no unit assert can witness; it needs threads parked
in the property-wait path *while* F2 stop storms run, post-ungil.
Test: `JSTests/threads/cve/mc-lock-stop-vs-park.js`.

Residual risks flagged for thread-scanners: (i) the watchdog is a fail-stop —
availability, not memory safety; any new park site added without the D9 poll
re-opens the wedge and only this test/watchdog will notice; (ii) the
JSThreadsSafepoint.h R3-4 note records that pre-M4 the entered-VM tripwire is
sampled, not structural — out of scope post-ungil but worth confirming M7
landed before this map's tests run.

## S6. Foreign blank-indexing first install (N3 leg) vs owner E4 plain-store transition — suspected revocation skip

Surface: `Source/JavaScriptCore/runtime/JSObject.cpp:2168-2196` — the N3 leg of
`createInitialIndexedStorageConcurrent` (word == 0: nuke-CAS the structureID,
`casButterfly(0 -> (currentTID,0))`, then **plain** `setStructure`), reached
from `tryCreateInitialForValueAndSetConcurrent` (JSObject.cpp:2350-2377) on a
foreign thread's first indexed write to a blank-indexing object. Racing
counterparty: the owner's E4 plain publications — structure-only N2-(i)
transitions ("today's code": plain `setStructure`, no nuke, e.g. inline
property adds via putDirectInternal) and `nukeStructureAndSetButterflyConcurrent`'s
plain nuke (JSObjectInlines.h:155-166), both legal lock-free-and-CAS-free
exactly while the source structure's TTL sets are valid.

Governing spec: SPEC-objectmodel §2 N3 (line 33), §5 F2 (line 168: fire BOTH
sets on a "butterfly-less transition by a thread != S->transitionThreadLocalTID()"),
I10 ("foreign butterfly transitions fire both TTL sets under STW"), I21 (no
lost adds incl. N2/N3 races, no structure/butterfly mismatch).

Verdict: **susceptible-suspected.**

The suspected hole, precisely: the N3 leg performs a *foreign butterfly-less
transition* (indexing None → Int32/Double/Contiguous is a nonPropertyTransition)
but checks neither `currentButterflyTID() != oldStructure->transitionThreadLocalTID()`
nor TTL validity, and fires nothing — even though the routine's own header
comment, hazard (c) at JSObject.cpp:2098-2100, states a foreign first install
"must fire F2 under a §10.6 stop", and the sibling blank-indexing leg INTO
ArrayStorage (JSObject.cpp:2516-2526) does implement exactly that keying and
fire. SPEC line 33's N3 text omits the fire and prescribes "64-bit CAS if
header unchanged, else §4.3 DCAS" — but the implementation's final
`setStructure(vm, newStructure)` at JSObject.cpp:2191 is a plain store, not a
header CAS, so interference between the nuke-CAS and the final store is
undetectable.

Concrete interleaving (post-ungil), object O butterfly-less with structure S,
TTL sets valid, transition TID = thread A:

- B (foreign): `O[0] = v` → N3 leg: loads S, nuke-CASes S→nuked (succeeds — no
  fire revoked A's bias first),
- A (owner, E4-legal because the sets are still valid): `O.b = w` inline add →
  today's code: value store, plain `setStructure(S')` — blindly overwrites the
  nuked lane,
- B: `casButterfly(0 → B')` succeeds (the word is still 0), plain
  `setStructure(S_idx)`.

Orderings of the two plain stores give either (w1) A's transition silently
lost — `O.b` invisible, an I21 "lost property add" — or (w2) final state
{structure = S' (blank indexing, no indexing header), butterfly = B'
(contiguous, allocated WITH an indexing header)}: a structure/butterfly
mismatch (I21), which is not merely a logic bug — GC visitChildren and every
butterfly-size computation derive base/extent from the *structure*
(outOfLineCapacity / hasIndexingHeader), so a blank structure paired with an
indexing-header butterfly mis-sizes the scan: the marker reads outside the
live allocation. This is the JEP-374 lesson verbatim: an asymmetric
optimization (E4 bias) whose revocation step (F2 fire) is skipped on one
trigger path leaves the bias owner racing the revoker's multi-step publication.

Why this is "suspected", not "confirmed": phase-1 GIL masks it entirely, so no
current gate can hit it; and there may be an intended argument that N3's
ID-lane nuke-CAS suffices — but that argument only holds against *CAS-shaped*
competitors, and E4 N2-(i) owners are chartered plain-store writers, so I could
not construct the missing exclusion from either the spec or the code. The fix
shape is small either way: key the N3 leg on the N1 TID like the AS leg and
route foreign installs through the existing shared-trigger stop (which already
fires F2 and publishes world-stopped), or make spec line 33 explicitly exempt
N3 and then forbid E4 plain-store N2 on structures whose instances are
foreign-reachable — the former matches every neighboring leg.

Test (doubles as the repro): `JSTests/threads/cve/mc-lock-n3-install-vs-owner-add.js` —
owner inline add racing a foreign first indexed install on a fresh-shaped
object per round; oracle is I21 (both writes must survive; mismatch/crash =
hit). Amplifier-ready.

## S7. Heap-server block/allocator lifecycle — the ERTS allocator-carrier analog

Surface: `Source/JavaScriptCore/heap/LocalAllocator.cpp:133-156` (slow-path
allocate across tryAllocateWithoutCollecting / steal / addBlock under the
per-server MSPL), BlockDirectory bit-vector transitions (`BlockDirectory.h:177-178`
BVL / m_localAllocatorsLock), TLC teardown (SPEC-heap §5.3: MSPL across
`stopAllocatingForGood`).

Governing spec: SPEC-heap §5.2 (MSPL serializes steals/accounting/addBlock
resizes), I1 (an in-use block handle is referenced by at most one thread;
transfer only under its directory's BVL), I5b (bitvector reallocation only in
addBlock holding BVL+MSPL), L1 ("never two same-rank locks; never two BVLs —
steal releases each first"), lock-rank table §6 (7a/7b/8/9/10a/10b).

The ERTS bug was a carrier (allocation block container) deletion/migration
state machine that deadlocked because two carriers' locks could be held in
conflicting orders during migration, and a deleted carrier could still be
reachable from a stale allocator. Our analogs: block steal (two directories'
BVLs) and block retirement vs a stale `m_currentBlock`.

Verdict: **immune-by-construction.**

Why: the deadlock shape is closed by L1's explicit rule that a steal releases
each BVL before taking the next — there is never a moment two same-rank
carrier locks are held, so no order inversion exists to race; the
reclaim-while-referenced shape is closed by I1's single-referencer invariant
with transfer serialized under the owning directory's BVL plus the MSPL over
the whole slow path, and block death (stopAllocatingForGood at teardown) runs
under the same MSPL that any competing handout would need. Adversarial check:
the weak point is not the protocol but its *audit surface* — every
BlockDirectoryBits reader/writer must hold BVL/MSPL or be world-stopped (I5b);
that audit (SPEC-heap T8) plus the C-level SharedHeapTestHarness scenarios
(`stealRace`, `clientChurnVsGC`, `epochReclaim`, §12.1) already target exactly
this; a JS-level test adds nothing the harness doesn't do better, so no new
test here.

---

## Summary table

| # | Surface | Verdict | Artifact |
|---|---------|---------|----------|
| S1 | Cell-lock word transitions (0x40/0x80 lanes) | immune-by-construction | — (JIT-emission mask audit → thread-scanners) |
| S2 | §3.0 nuke-window state machine | immune-by-construction | existing races/objectmodel corpus |
| S3 | E4/TTL bias + STW revocation | immune-by-construction (except S6) | I34 audit + RELEASE_ASSERT witnesses |
| S4 | CoW materialization (Dirty COW analog) | needs-test | `mc-lock-cow-materialize-race.js` |
| S5 | Stop state machine vs native parks (AB-17B) | needs-test | `mc-lock-stop-vs-park.js` |
| S6 | Foreign N3 first install skips F2 fire vs owner E4 plain store | **susceptible-suspected** | `mc-lock-n3-install-vs-owner-add.js` (repro) |
| S7 | Heap-server block/LA lifecycle (carrier analog) | immune-by-construction | SharedHeapTestHarness §12.1 scenarios |
