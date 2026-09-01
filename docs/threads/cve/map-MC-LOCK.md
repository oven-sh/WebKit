# MC-LOCK — Lock-state / state-machine transition race: mapping to our threads surface

Mechanism class (CVE-AUDIT.md §MC-LOCK): a multi-step state machine (lock-word
encodings, COW state, allocator-carrier lifecycle, flag transitions) admits an
interleaving that observes a mid-transition state or skips/repeats a
transition. The standing lesson (JEP 374): an asymmetric lock optimization
whose "deoptimize the lock" path must stop or introspect another thread is a
permanent race generator. Exemplars mapped: JDK-6444286 / JDK-8240723 (biased
locking revocation/epoch), JDK-8319137 / JDK-8315884 (mark-word inflation /
header-word overload races), CVE-2016-5195 (Dirty COW), ERTS allocator-carrier
deletion deadlock.

Status note (re-audit 2026-06-15): every surface below is GIL-masked in phase 1
(single mutator at a time); all verdicts and tests are about the post-ungil
N-mutator world. Tests live in `JSTests/threads/cve/mc-lock-*.js` and are
EXECUTED POST-UNGIL ONLY. **S6 was confirmed susceptible at first audit, fixed
in the AB-17c F3 round (CVE-AUDIT-STATUS.md item 4), and is re-verdicted
needs-test (regression) below.**

---

## S1. The per-object cell lock word itself (acquire / release / park transitions)

Surface:
- `Source/JavaScriptCore/runtime/JSCellInlines.h:379-399` (`JSCellLock::lock/tryLock/unlock/isLocked` over the `m_indexingTypeAndMisc` byte),
- `Source/JavaScriptCore/runtime/JSCell.cpp:283-293` (`lockSlow/unlockSlow` via `IndexingTypeLockAlgorithm`),
- `Source/WTF/wtf/LockAlgorithmInlines.h` (ParkingLot compareAndPark / unparkOne),
- `Source/JavaScriptCore/runtime/ConcurrentButterfly.h:296-321` (`cellHeaderVolatileMask`: held 0x40, parked 0x80, m_cellState lane, per-cell type-flags bit; `headerDiffersOnlyInVolatileBits` / `mergeVolatileHeaderBits`).

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
   CASed lock bits (lost unpark = wedged waiter; the JDK-6444286 / JDK-8319137
   shape) — is closed by the §3.0 lane discipline: 0x40/0x80 are *volatile*
   lanes, every 64/128-bit header CAS folds them from the freshest read
   (`mergeVolatileHeaderBits`, all DCAS sites: ConcurrentButterfly.cpp:890-891,
   1407-1408, 1593-1594, 1833-1834, 2061-2062), and a CAS that observes a
   semantic-lane change either RELEASE_ASSERTs (under the lock) or abandons
   (lock-free). The mask is static_asserted byte-exactly
   (ConcurrentButterfly.h:306).
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
republished — `JSObjectInlines.h:157-167` (`nukeStructureAndSetButterflyConcurrent`),
`JSObjectInlines.h:79-128` (`storeTaggedButterflyWordConcurrent` b1-only fold +
RELEASE_ASSERT trap), `ConcurrentButterfly.cpp` locked protocols (taxonomy
(a)-(d) + RESTART), CoW publication :1792-1842, the per-event-stop publication
legs in `JSObject.cpp` (createInitialIndexedStorageConcurrent / createArrayStorageConcurrent).

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
convention: `storeTaggedButterflyWordConcurrent` RELEASE_ASSERTs the b1-only
fold, so a protocol escape crashes deterministically instead of losing a store.
Repeating a transition is impossible because the ID CAS consumes the pre-state
exactly once.

Adversarial self-check: exhaustiveness of the taxonomy is an audit claim, not a
local proof — but it is the claim the whole objectmodel corpus
(`JSTests/threads/races/`, `objectmodel/`) plus the amplifier already targets,
and S6 below documents the one enumerated writer that *escaped* the
arbitration (now closed). The taxonomy itself needs no new test.

## S3. TTL watchpoint ownership elision (E4/E1-E3) — our biased-locking analog

Surface: SPEC-objectmodel §5 (E4, F1-F4, N1), `Structure.h` / `Structure.cpp:323-338,
401-526,1693-1694,1839-1853` (TTL TID + fire functions + F4 chain-fire),
E4 windows in `JSObject.cpp:2385,2460-2470` (poll-free copy windows under
`AssertNoGC`), F2 keying at `JSObject.cpp:2368-2372,2784` (first installs),
:4565 (deletes), `ConcurrentButterfly.cpp:1588` (CoW F2).

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
is wired (S6's gap is now closed; see below).

Why the JVM mechanism cannot occur here: revocation is never per-thread.
A fire requires a full STW (I13); the owner's elided check→publish sequence is
straight-line, poll-free and allocation-free (I34, the `AssertNoGC` windows at
JSObject.cpp:2385/2460), so it cannot be suspended mid-window — the stop lands
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
witnesses; (a) was falsified at one site (S6) and is now closed by the
AB-17c/AB18-S3 keying — the regression test is the standing witness for both.

## S4. CopyOnWrite state machine — the Dirty COW analog

Surface: `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:1697-1860`
(`tryMaterializeCopyOnWriteButterflyForSharedWrite` — the single cell-locked
serialization point: F2-fire-first for foreign words, nuke-CAS, DCAS/M8
publication, word-stability RELEASE_ASSERTs at :1733-1734, :1831, :1842, :1956),
:2075-2100 (owner driver), `JSObject.cpp:3695-` (owner `convertFromCopyOnWrite`
rerouted flag-on — the review-round-3 fix that removed the owner's plain-nuke
racing the locked materializer), `JSObjectInlines.h` (classify reroute),
`JSObject.cpp:4877` (delete-path CoW carve-out via ensureSharedWriteBit).

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
RELEASE_ASSERTs at :1831/:1842 make "a CoW word moved under the lock" or "SW
appeared on a CoW word" (I35 violations) deterministic traps. Losers re-dispatch
and re-classify; the triggering store only runs after the winner's tag is
re-read (§2/§3 probes), and write fast paths never match CoW indexing modes,
so there is no path that stores through the shared copy.

Verdict: **needs-test** — the historical round-3 bug shows this exact surface
was the live one; it deserves a permanent regression storm with the
Dirty-COW-shaped oracle (a CoW *sibling* observing the write).
Test: `JSTests/threads/cve/mc-lock-cow-materialize-race.js`.

## S5. Safepoint/stop state machine + native park sites — the carrier-deadlock / wedged-revocation analog

Surface: `Source/JavaScriptCore/runtime/VMManager.h:90-197,252-` (world Mode
machine RunAll/Stopped/RunOne and its transition contract),
`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:606-700` (30s stop
watchdog turning a non-converging stop into a deterministic fail-stop;
`watchdogAssertStopProgress` at :673), :73-74 + the FIX-2 helper
(`parkSitePollAndParkForStopTheWorld` — per-D9-quantum poll: publish
access-released, wake the conductor's sampler, ticket-park until the stop word
clears, re-acquire through the §A.3.2b gates).

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
re-opens the wedge and only this test/watchdog will notice; (ii) CVE-AUDIT-STATUS
item 10 records the FIX-2 helper wiring at the class-(2) compile-side waits as a
late addition — re-audit when MC-WAIT S6 / MC-SAFE gcwait-vs-classa closes.

## S6. Foreign blank-indexing first install (N3 leg) vs owner E4 plain-store transition — CONFIRMED then CLOSED

Surface: `Source/JavaScriptCore/runtime/JSObject.cpp:2298-2442`
(`createInitialIndexedStorageConcurrent` — the N3 leg of a butterfly-less
object's first indexed install). Racing counterparty: the owner's E4 plain
publications — structure-only N2-(i) transitions ("today's code": plain
`setStructure`, no nuke, e.g. inline property adds via putDirectInternal) and
`nukeStructureAndSetButterflyConcurrent`'s plain nuke (JSObjectInlines.h:157-167),
both legal lock-free-and-CAS-free exactly while the source structure's TTL sets
are valid.

Governing spec: SPEC-objectmodel §2 N3, §5 F2 (fire BOTH sets on a
"butterfly-less transition by a thread != S->transitionThreadLocalTID()"),
I10 ("foreign butterfly transitions fire both TTL sets under STW"), I21 (no
lost adds incl. N2/N3 races, no structure/butterfly mismatch), I29 (no
poll/alloc/safepoint between final validation and the StructureID store).

History (first-audit finding, retained for posterity): the original N3 leg
performed a *foreign butterfly-less transition* (indexing None →
Int32/Double/Contiguous nonPropertyTransition) but checked neither
`currentButterflyTID() != oldStructure->transitionThreadLocalTID()` nor TTL
validity, and fired nothing — even though its own header comment stated a
foreign first install "must fire F2 under a §10.6 stop", and the sibling
blank-indexing leg INTO ArrayStorage (`createArrayStorageConcurrent`,
JSObject.cpp:2729-2784) implemented exactly that keying. Its final
`setStructure` was a plain store, so interference between the nuke-CAS and the
final store was undetectable. Concrete interleaving gave either a lost owner
add or a {blank-indexing structure, contiguous-indexing-header butterfly} torn
pair (GC mis-sized scan) — the JEP-374 lesson verbatim: an asymmetric
optimization (E4 bias) whose revocation step (F2 fire) was skipped on one
trigger path.

Fix (LANDED — AB-17c F3 round, refined AB18-S3; CVE-AUDIT-STATUS.md item 4):
`createInitialIndexedStorageConcurrent` now (JSObject.cpp:2339-2442):
1. Keys on the N1 transition TID (`currentButterflyTID() !=
   oldStructure->transitionThreadLocalTID()` incl. `forceButterflySWBit`, :2368-2372)
   — a foreign-keyed install with any still-valid TTL set on source/target
   routes through the shared per-event-stop leg (F2 fire on source+target
   inside the stop, flat publication SW=1; mirrors the `createArrayStorageConcurrent`
   sibling).
2. Owner + both source TTL sets observed valid by FRESH loads inside an
   `AssertNoGC` poll-free window (:2385-2408, I29): lock-free nuke-CAS
   publication is exclusive — a foreign locked transitioner must fire the sets
   under a §10.6 stop first, and that stop cannot land inside the poll-free
   window.
3. Fired-sets regime (any TID, sets already invalid): publishes UNDER THE CELL
   LOCK (:2410-2436) so the cell-locked named protocols' under-lock re-check
   serializes against it — closing the AB18-S3 follow-on (a lock-free nuke-CAS
   landing in a locked protocol's check→CAS window and tripping its
   RELEASE_ASSERTs).

Verdict: **needs-test** (regression — was **susceptible-suspected** at first
audit; confirmed and fixed). Verified `mc-lock-n3-install-vs-owner-add.js`
5/5 GIL-off full JIT (CVE-AUDIT-STATUS item 4).
Test: `JSTests/threads/cve/mc-lock-n3-install-vs-owner-add.js`.

Adversarial self-check on the fix: the lock-free leg's exclusion argument
(:2388-2408) is sound iff (i) every foreign locked transitioner on a
butterfly-less object fires the source's TTL sets under a §10.6 stop *before*
acquiring the cell lock (I10b — the F2 keying audit is the witness), and
(ii) the `AssertNoGC` window is genuinely poll-free (I29 — RELEASE_ASSERT
witness). Both are the same legs S3's verdict rests on; no new gap found.

## S7. Heap-server block/allocator lifecycle — the ERTS allocator-carrier analog

Surface: `Source/JavaScriptCore/heap/LocalAllocator.cpp` (slow-path allocate
across tryAllocateWithoutCollecting / steal / addBlock under the per-server
MSPL), BlockDirectory bit-vector transitions (`BlockDirectory.h` BVL /
m_localAllocatorsLock), TLC teardown (SPEC-heap §5.3: MSPL across
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

## S8. (Forward) NativeSerialLock acquire/drop state machine — spec'd, NOT YET IN TREE

Surface: SPEC-nativeaffinity §3.2/§3.3 (NL: park-capable safepoint-polling
per-VM native serial lock; CAS-acquire owner+depth word, NL1 contended loop,
NA-I11 mandatory drop around every JS re-entry, NA-I10 conductor exclusion,
trap-class split). The only in-tree reference is the CG-I19/F40 placeholder
assert comment at `Source/JavaScriptCore/heap/Heap.cpp:6213`; no
`NativeSerialLock` / `m_nativeLockDepth` implementation exists yet.

This is a charter for a NEW multi-step lock-state machine (free / held-depth-n
/ parked-polling / dropped-with-saved-depth) with an explicit asymmetric
optimization (per-lite `nativeLockEligible`) and a "deoptimize" path (the
NA-I11 drop scope around vmEntry). It is squarely MC-LOCK territory once it
lands: the JEP-374-shaped hazard would be a drop-scope reacquire racing a
conductor-side state transition, or a depth-restore skipped on an
exception/termination path (NA-I12).

Verdict: **no current surface** — re-open this entry when SPEC-nativeaffinity
implementation lands. The §3.2 ANNEX NL1 protocol already encodes the right
defenses (poll-first; every wake re-polls before any CAS; termination-only
traps never park; NA-I10 forbids conductor acquisition), so the audit when it
lands is a wiring check, not a design review.

---

## Summary table

| # | Surface | Verdict | Artifact |
|---|---------|---------|----------|
| S1 | Cell-lock word transitions (0x40/0x80 lanes) | immune-by-construction | — (JIT-emission mask audit → thread-scanners) |
| S2 | §3.0 nuke-window state machine | immune-by-construction | existing races/objectmodel corpus |
| S3 | E4/TTL bias + STW revocation | immune-by-construction | I34 audit + RELEASE_ASSERT witnesses; S6 regression test |
| S4 | CoW materialization (Dirty COW analog) | needs-test | `mc-lock-cow-materialize-race.js` |
| S5 | Stop state machine vs native parks (AB-17B) | needs-test | `mc-lock-stop-vs-park.js` |
| S6 | Foreign N3 first install skips F2 fire vs owner E4 plain store | **needs-test** (was susceptible; FIX LANDED AB-17c/AB18-S3) | `mc-lock-n3-install-vs-owner-add.js` |
| S7 | Heap-server block/LA lifecycle (carrier analog) | immune-by-construction | SharedHeapTestHarness §12.1 scenarios |
| S8 | NativeSerialLock (SPEC-nativeaffinity §3) | no surface yet — re-audit on landing | — |
