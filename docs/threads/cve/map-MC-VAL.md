# map-MC-VAL — Validator/consumer disagreement mapped to the JSC threads surface

Status: defensive audit artifact, 2026-06-07. Part of the thread-cve-research
slice (CVE-AUDIT.md class MC-VAL). READ-ONLY analysis of `jarred/threads`;
tests written but NOT executed (tree mid-bring-up; run post-ungil via
thread-cve-audit). Spec revs as pinned by the tree: objectmodel ("OM") r14,
jit r12, api r14, heap r13, vmstate r13, SPEC-ungil + UNGIL-HANDOUT rev 32.

Mechanism class (from CVE-AUDIT.md §MC-VAL, web-derived data):
code consuming "already validated" input operates under different assumptions
than the validator enforced; a racing writer manufactures these from *correct*
validators (MC-DF is the racing special case). Exemplars: CVE-2018-4121 (JSC
wasm section-order validator/consumer split), CVE-2024-2887 (V8 main bug:
type-index valid in one namespace consumed against another), CVE-2026-2796
(SpiderMonkey wasm validator/compiler disagreement).

Audit lens used per surface (adversarial checklist):
1. WHO validates, WHAT exact predicate, on WHICH bytes/namespace?
2. WHO consumes, under WHICH assumptions, on WHICH bytes/namespace?
3. Can a second mutator make (1) and (2) diverge — different bytes, different
   namespace, different point in time — without re-validation?
4. Per CVE-AUDIT cross-cutting rule 1: "window too small" is NOT a defense;
   only "no writer can exist" or "consumer revalidates from the same fetch".

Verdict key: IMMUNE = immune-by-construction (protocol cited, adversarial
argument given); NEEDS-TEST = targeted susceptibility test written under
`JSTests/threads/cve/`; SUSPECT = susceptible-suspected, hole described.

---

## Surface inventory and verdicts

### V1. LLInt metadata caches (validated (structure,offset) consumed by asm fast paths) — NEEDS-TEST

- Surface: `Source/JavaScriptCore/llint/LLIntSlowPaths.cpp:837-846` (threaded
  publish arm under `useThreadedLLIntPropertyCaches()`),
  `Source/JavaScriptCore/bytecode/GetByIdMetadata.h:50-78`
  (`LLIntCachedIdAndOffset`, alignas(8) u64 + static_asserts).
- Governing spec: SPEC-jit §4.3 (frozen survivor table), §5.4, I13, I18;
  CVE-AUDIT "bytecode/metadata validated once then consumed by N tiers".
- Validator: the C++ slow path proves "structure S has property at offset o,
  cacheable, non-dictionary" and publishes the pair. Consumer: asm fast path
  on ANY thread loads the pair and dereferences at the offset.
- Why the design is immune: the pair is ONE aligned u64 written by a single
  relaxed store and read by a single 64-bit load; the consumer's structureID
  compare comes from the SAME load as the offset it consumes, so a stale or
  republished word self-invalidates (id half fails the compare against the
  live cell). Every cache whose threaded form cannot be one self-validating
  word is DISABLED flag-on: `setupGetByIdPrototypeCache` wholesale,
  put_by_id transition cache, private names/brands (§4.3 table; mode-byte
  coherence restricted to Default/ArrayLength).
- Adversarial residue (why NEEDS-TEST, not IMMUNE): the immunity argument is
  only as strong as the I13 write inventory ("every `metadata.m_*structureID*=`
  write in llint/ is in this table or flag-off-only") and the mode-byte
  poison discipline (I18). Those are lint-enforced exhaustiveness claims, not
  structural impossibilities — exactly the kind of claim CVE-2018-4121 broke
  (a validator that was correct for the orderings someone enumerated).
- Test: `JSTests/threads/cve/mc-val-llint-cache-storm.js` (deterministic
  detector — every value encodes its key; `--useJIT=0` pins the LLInt
  consumer; amplifier-ready).
- Executed at the CVE close-out round: the V1 design verdict HOLDS — no
  wrong-offset consumption observed at 20/20 GIL-off Release + 20/20
  amplified + 120/120 under 24-way load + TSAN no-JIT clean + Debug
  (threaded publish arm confirmed live: `useThreadedLLIntPropertyCaches()`
  returns `Options::useJSThreads()` on ADDRESS64/LE, LLIntSlowPaths.cpp:124).
  One TEST-BROKEN repair, GIL-on only: the reader loops spun without any
  blocking primitive, which phase-1 cooperative-GIL scheduling starves
  forever (SPEC-api item 9, G23/G24: yields = §5.2 blocking primitives
  only) — the hang was spec-conformant scheduling, not an engine bug; same
  repair shape as V8 below (bounded property-path `Atomics.wait` every 256
  passes).
- Attribution of the ORIGINAL chartered GIL-off failure (recorded so a
  future regression of the real cause is not masked by the test repair):
  the GIL-on starvation repair above cannot explain a GIL-off failure, and
  no doc captured the GIL-off signature before the repair landed. The two
  plausible mechanisms are (a) spin-loop oversubscription timing out the
  harness under load GIL-off (the same unyielding reader loops, pre-repair,
  oversubscribed cores), and (b) the since-fixed MC-SAFE S4 stop-deadlock
  family as a cross-family flake (the watchdog signature mc-aint's record
  attributes its one observed flake to, still reproducing ~1/6 under
  mc-jit-delete-reuse-stale-offset as of 2026-06-10). NEITHER was captured
  at the time — this closure's GIL-off leg is therefore attributed
  OBSERVATIONALLY to the executed bars (20/20 + 20/20 amplified + 120/120
  under 24-way load + TSAN no-JIT + Debug), not to a point fix.

### V2. Baseline/handler IC inline state (packed self word + handler chains) — IMMUNE

- Surface: `Source/JavaScriptCore/bytecode/PropertyInlineCache.h:461-482`
  (`m_packedSelfWord` union; offset half consumed only with the id half from
  the same 64-bit load), `PropertyInlineCache.h:369,387`; handler chain
  publish at `PropertyInlineCache.cpp` (storeStoreFence before head store).
- Governing spec: SPEC-jit §4.2 (I6: "inlined fast-path state never
  observable as valid structure id + mismatched offset — structural"), §4.1
  + §5.1 (handler nodes frozen at publish, F1/F2 fence+address-dependency),
  §4.4/I7/I9 (retired chains freed only via epoch + refcount, so a stale
  consumer reads a complete OLD record, never a freed/partial one).
- Why immune: there is no separately-fetched validation result for a racing
  writer to split. The validator's output (id) and the consumer's input
  (offset) are physically the same word; chains are immutable after publish;
  holder-bearing inlined forms — the one shape that CANNOT pack — are
  disabled flag-on (§4.2). Invalidation is the all-zero word (ABA-safe: zero
  id matches no live structure).
- Adversarial check: 32-bit StructureID reuse could make a stale id half
  validate against a NEW structure with a different layout (true namespace
  aliasing). Closed because ID reuse requires the old Structure to die, and
  ICs hold the id as a visited weak reference cleared at GC (a world-stopped
  context, SPEC-jit G13/heap §9); a stale word that survives is cleared
  before its referent's ID can be reissued.

### V3. Call-link records (validated callee→entry consumed by call fast paths) — IMMUNE

- Surface: `Source/JavaScriptCore/bytecode/CallLinkInfo.h:92-101`
  (`CallLinkRecord { comparand, target, codeBlockToTransfer }`).
- Governing spec: SPEC-jit §5.8 (frozen fast path: ALL reads flow through ONE
  published record pointer; comparand checked and target loaded from the
  same immutable record), F6 (init → storeStoreFence → single pointer store;
  unlink = single null store), I16 (no safepoint between record load and
  call), §4.4 retirement (stale record = complete old record).
- Why immune: the historical unsound shape (guard word and payload word
  validated/consumed as separately-mutable locations) is retired by
  construction — writers NEVER mutate, they publish a new record;
  `repatchSpeculatively` is RELEASE_ASSERT-forbidden on non-data-IC paths.
  A racing relink gives the consumer a fully consistent old or new
  (comparand, target, codeBlock) triple; cross-pairing is impossible without
  mutating a published record, which I4 forbids and debug-checksums.

### V4. Compile-time validation vs link/install (DFG/FTL desired watchpoints, profiles) — NEEDS-TEST

- Surface: `Source/JavaScriptCore/dfg/DFGPlan.cpp:595-614`
  (`Plan::reallyAdd`: `areStillValidOnMainThread` then registration),
  `DFGPlan.cpp:616-700` (`finalize` under `GILOffCompilationLocker`, with the
  in-tree KNOWN RESIDUAL comment at :640-646),
  `Source/JavaScriptCore/dfg/DFGDesiredWatchpoints.cpp:166,201-206`
  (per-set `hasBeenInvalidated` revalidation inside registration).
- Governing spec: SPEC-jit §5.5 (E1-E3 elision contract), §5.6 (Class-A
  fires world-stopped, synchronous completion), §5.3/I8 (jettison only under
  STW), I12 ("profiles select, guards validate"), I21 (poll→invalidation
  point); SPEC-ungil §N.8 (CodeBlock first-install release-CAS) and the
  audit rows in SPEC-ungil-audit-N7.md.
- This is the purest MC-VAL instance we have: the validator (compiler
  thread) proves invariants at time T over shared metadata; the consumer
  (installed code on N mutators) runs at T+Δ under assumptions a second
  mutator can falsify. The designed defense chain: (a) fires are STW and
  jettison in the same stop; (b) link-time revalidation happens inside the
  registration step itself (same lock, per-set re-check), with heap deferred
  (`ASSERT(m_vm->heap.isDeferred())`, DFGPlan.cpp:600,609) and stops
  cooperative-only (jit R1.f) — so no stop, hence no fire, can interleave
  between revalidation and registration.
- Why NEEDS-TEST rather than IMMUNE: the no-park-inside-finalize property is
  a global code-shape invariant of everything reachable from finalize(), not
  a local structural fact — one allocation or poll added inside that window
  re-opens the CVE-2021-2388-shaped hole. And the tree itself documents a
  KNOWN RESIDUAL (DFGPlan.cpp:640-646): compileInThread reads baseline
  profiling/IC state mutated by sibling lites with plain stores. That is
  sound only while EVERY consumer of profile data emits a validating guard
  (I12). Any profile-derived fact consumed unguarded (e.g. a future
  "proven" constant or shape folded without a watchpoint/check) is an
  instant MC-VAL bug.
- Test: `JSTests/threads/cve/mc-val-fire-vs-link.js` (recompile generations
  vs a foreign-transition TTL-fire storm; value relation y==2x detects
  stale-elision consumption; amplifier-ready).

### V5. Butterfly TID namespace (owner-validated lock-free transitions) — NEEDS-TEST (re-audit 2026-06-15: D1/D1R reissue LANDED)

- Surface: `Source/JavaScriptCore/runtime/ConcurrentButterfly.h` tag encode
  (OM §2: 15-bit TID, `notTTLTID=0x7fff` reserved); consumers = E4 owner
  transitions and per-tier write predicates (SPEC-jit §5.5 fused TID
  compare, `g_jscButterflyTIDTag`). NEW since the original audit:
  `Source/JavaScriptCore/runtime/ThreadManager.h:498-585` (rebias state
  machine, `m_freeTIDs`/`m_freeCarrierTIDs` recycle lists fed ONLY by §D.1
  phase 3), `ThreadManager.cpp:339-493` (`maybeArmAndSealRebiasLocked`,
  `completeRebiasIfPendingLocked`, allocation-time reissue at :346-349 /
  :448-449), and `Source/JavaScriptCore/heap/Heap.cpp:6052-6197`
  (`conductTIDRebiasUnderSharedStop`: world-stopped restamp + D1R fire).
- Governing spec: SPEC-ungil §D.1 / ANNEXES D1+D1R (BINDING; UNGIL-HANDOUT
  :1636-1705), OM E4/I11/I15, SPEC-jit I19/R5. OM §2's "No recycling this
  milestone" is SUPERSEDED gilOffProcess (Dev 10 lifted by U-T12); GIL-on
  Dev 10 still holds (recycle lists inert, ThreadManager.h:666 comment).
- This is the direct CVE-2024-2887 analogue: an index valid in one
  namespace (TID allocated to thread A's lifetime) consumed against another
  (a fresh thread holding the SAME reissued numeric TID passes A's
  instance-ownership checks on objects it never owned, re-enabling E4
  lock-free transition paths E4 justifies only for the true owner).
- The original verdict ("immune — namespaces cannot alias because TIDs are
  never recycled") EXPIRED: reissue is live gilOffProcess. Re-audit of the
  landed D1/D1R protocol against the recorded tripwire's two soundness
  conditions:
  - "every instance tagged with the dead TID is retagged under STW before
    the TID re-enters the allocator" — DISCHARGED by phase ordering: phase
    2 runs `conductTIDRebiasUnderSharedStop` inside the heap §10 full-GC
    stop with `RELEASE_ASSERT(worldIsStoppedForAllClients())`
    (Heap.cpp:6134); the `forEachLiveCell` walk restamps every live
    JSObject's butterfly word holding a snapshotted TID to TID-0
    (Heap.cpp:6176-6184) and every live Structure's
    `m_transitionThreadLocalTID` (Heap.cpp:6160-6167); phase 3
    (`completeRebiasIfPendingLocked`, the SOLE feeder of the free lists,
    ThreadManager.h:666) is gated on `RebiasState::Restamped`
    (ThreadManager.cpp:582-584 D1R ordering comment) which only the
    conductor's post-fire `noteRebiasRestampComplete` release-store can
    set. So no TID is reissuable before its restamp completed.
  - "the proof must cover JIT-resident `g_jscButterflyTIDTag` copies on
    code that survives the stop" — DISCHARGED by D1R items 1+4: item 1
    fires `fireTransitionThreadLocal` on every restamped structure inside
    the same stop (Heap.cpp:6193-6194), jettisoning every DFG/FTL/IC body
    that baked the dead `tid<<48` immediate (E4 emission requires the TTL
    set valid+watched; fire ⇒ jit §5.3/§5.6 jettison) BEFORE reissue is
    possible; item 4 records that the per-tier read/write predicates load
    BOTH sides at runtime (TLS `g_jscButterflyTIDTag` vs the instance tag
    word) — neither is baked, so restamp-to-0 + tag uniqueness suffice.
- Adversarial residue (why NEEDS-TEST, not IMMUNE): the design is sound but
  immunity now rests on THREE enumeration/audit claims, none structural:
  (a) the `forEachLiveCell` walk is the COMPLETE restamp surface — the
  banner at Heap.cpp:6069 states this ("the D1 'precise + aux'"), and the
  walk skips aux storage and `WebAssemblyGCObjectType` (Heap.cpp:6156,
  6174) on the argument that neither carries a TID-bearing butterfly word;
  any future JSObject family whose offset-8 word is not the tagged
  butterfly, or any tagged-state holder outside `objectSpace()`, is a
  silent miss; (b) D1R item 1's coverage is exactly the set of structures
  whose `m_transitionThreadLocalTID` was restamped — a CodeBlock that
  baked a dead-TID immediate via any OTHER linkage (none is known, but
  this is the CVE-2018-4121-shaped "correct for the cases someone
  enumerated" claim); (c) the U-T12 verification arms are a RECORDED
  DEFERRAL (ThreadManager.h:567-585) — arm (3), the D1R item-5 amplifier
  (E4-specialized code vs a dying thread's structure; force rebias+reissue;
  assert I15 + assert the specialized CodeBlock was jettisoned in-stop),
  is exactly this surface's acceptance check and is NOT YET WRITTEN.
- Test: `JSTests/threads/cve/mc-val-tid-reissue-false-owner.js` — drives
  the spawned-range past the 75% per-partition trigger, forces rebias to
  completion, then has FRESH (reissued-TID) threads + main concurrently
  write/transition the SAME slots on dead-thread keepsakes; key-encoded
  values detect cross-slot bleed / lost transitions from a false-owner E4
  racing the foreign locked path. GIL-OFF ONLY (premise unconstructible
  GIL-on); heavy (≈13k spawns — no Options knob lowers the trigger);
  amplifier-ready (the `RaceAmplifier::perturb` stall points sit pre-walk
  / post-restamp / post-fire in `conductTIDRebiasUnderSharedStop`).
  Companion: `mc-tdwn-tid-recycle-storm.js` (U-T12 arm 1: SD9 recovery +
  read integrity). The fully-instrumented D1R item-5 arm (jettison
  introspection) remains a non-corpus deferred deliverable per the
  ThreadManager.h banner — this test is the JS-observable half.

### V6. Sharded atom table (atomization uniqueness consumed as pointer identity) — NEEDS-TEST

- Surface: `Source/WTF/wtf/text/SharedAtomStringTable.h:71,109`
  (`shardForHash`), `Source/WTF/wtf/text/AtomStringImpl.cpp` dual-path
  routing (e.g. :76,128,351-417 — legacy arms now assert
  `!sharedAtomStringTableEnabled()`), `Source/WTF/wtf/text/StringImpl.h:1308`
  (`tryRefAtom`, refcount-0-is-final).
- Governing spec: vmstate §4.2-§4.4 (W1), frozen rule A1 (when shared, NO
  path may touch any per-thread `AtomStringTable` — 17 locker sites +
  explicit-table overloads rerouted), I5 (shard selection MUST be
  `shardForHash` from every entry path), §4.4 no-resurrection protocol.
- Validator: atomization promises one live atom per character sequence.
  Consumers that inherit the assumption WITHOUT revalidating: PropertyTable
  lookups, Structure transition-table keys (keyed by UniquedStringImpl*),
  every IC identity compare, `Identifier` ==. A single A1 bypass (one
  overload still honoring its passed per-thread table) silently yields two
  atoms for the same chars — not memory-unsafe by itself, but it breaks
  every identity-based security/semantic check built on it (lost defines,
  split property views between threads), and via transition-table keying it
  can split structure lineages.
- Why the design is sound: deterministic shard choice from the translator
  hash means concurrent atomizers of equal strings serialize on the SAME
  shard lock; the resurrection hole (table hit on a dying entry → UAF or
  duplicate) is closed by refcount-0-is-final + `tryRefAtom` under the shard
  lock; pre-latch atoms are migrated at the once-only latch.
- Why NEEDS-TEST: like V1, immunity rests on an exhaustiveness claim (A1's
  17+N rerouted sites). The drift guards are debug asserts on the legacy
  arms; a release-build bypass is silent. A deterministic cross-thread
  identity test is cheap and permanent.
- Test: `JSTests/threads/cve/mc-val-atom-identity.js` (fully deterministic:
  join() as the HB edge; equal names built via different string paths per
  thread; includes the SPEC-ungil §H SymbolRegistry leg).

### V7. Wasm: validate-then-consume of attacker-shared bytes — IMMUNE (v1 scope)

- Surface: `Source/JavaScriptCore/wasm/js/JSWebAssemblyHelpers.h:125-184` —
  `getWasmBufferFromValue` (one base/size fetch at :160-161 after
  validateTypedArray/detach checks) + `createSourceBufferFromValue` (single
  `result.append(data)` copy at :182); callers
  `WebAssemblyModuleConstructor.cpp:301`, `JSWebAssembly.cpp:155,281,422`;
  spawned-thread refusal at `JSWebAssemblyHelpers.h:51-64`,
  `WebAssemblyModuleConstructor.cpp:294`.
- Governing spec: SPEC-ungil §I (wasm EXECUTION and the ctor/compile surface
  on spawned threads = TypeError, both GIL modes, SD7; wasm-GC = LinkError;
  heap §5.5/manifest 11), SPEC-ungil §N.6 (detach contents quarantined to a
  heap stop; grow base-immutable, commit-then-release-length).
- This is CVE-2018-4121 + CVE-2017-5116's home turf. Why immune:
  (a) the validator and every consumer (BBQ/OMG on compiler threads, entry
  thunks) operate on the engine's PRIVATE copy, taken in one pass BEFORE
  validation — a SAB-backed source mutated mid-copy yields a torn byte
  vector, but validator and consumer see the SAME torn bytes, so no
  disagreement is constructible (the CVE-2017-5116 fix shape);
  (b) v1 refuses the entire wasm surface on spawned threads, so the
  cross-thread consumer population is empty by construction;
  (c) a racing detach during the copy cannot unmap the source (N.6
  quarantine to a stop; the copy loop has no poll).
- Residue: section-order/type-index bugs of the CVE-2018-4121/-2887 kind
  remain possible as SINGLE-threaded validator bugs in upstream wasm code —
  out of scope for the threads audit (no second mutator involved), covered
  by upstream fuzzing.

### V8. Multi-slot consumers of a once-validated Structure (fast enumeration/clone) — NEEDS-TEST

- Surface: runtime fast paths that validate one Structure then consume many
  (offset,key) pairs — `Structure::canPerformFastPropertyEnumeration`
  consumers (`Source/JavaScriptCore/runtime/Structure.h:884-904` per api
  G25), `Object.assign`/spread fast cloning (`runtime/JSObject.cpp`,
  `ObjectConstructor.cpp`), for-in/`Object.keys` snapshots,
  `JSON.stringify`'s property sweep.
- Governing spec: OM M7/I24 (structure→butterfly load ordering; no deref at
  an offset from a structure not ordered before / revalidated after the
  butterfly load), I34 + manifest 7b (no poll between offset acquisition and
  access without revalidation — audited over UNOWNED callers, which is
  exactly this surface), I7 (superseded storage never freed/rewritten),
  §6 D1/I18 (deleted out-of-line slots release-store jsUndefined and stay
  quarantined until an owning-heap epoch bump), AS-COPY/I31.
- Validator: "structure S is flat, enumerable-fast, offsets 0..n live".
  Consumer: a LOOP of raw offset reads under that single validation while a
  foreign thread transitions/deletes. Why the design holds: offsets from
  S_old always fit any butterfly co-ordered after it (storage never shrinks
  while quarantined, never relaid in place flag-on); deleted slots read as
  old-value-or-undefined, never another property's value; slot reads are
  single 64-bit loads (no torn JSValues). The permitted divergence is
  SEMANTIC staleness only (SAB-staleness model, OM C4).
- Why NEEDS-TEST: this is the surface whose soundness is discharged by an
  AUDIT (7b) over code the threads workstreams do not own, and the historic
  JSC precedent (CVE-2018-4438-class enumeration bugs; MC-REENT twins) shows
  these loops are where validated-state assumptions rot. The test encodes
  keys into values so any cross-slot confusion is a hard failure while
  staleness stays green.
- Test: `JSTests/threads/cve/mc-val-multislot-clone.js` (foreign
  transition+delete storm vs assign/spread/JSON/for-in; amplifier-ready).
  Executed at the CVE close-out round: 20/20 GIL-off Release, 3/3 GIL-on —
  the design verdict holds (no cross-slot confusion observed). One
  TEST-BROKEN repair: the writer threads originally spun without any
  blocking primitive, which GIL-on starves every other thread forever —
  phase-1 GIL preemption is COOPERATIVE-ONLY (SPEC-api item 9, G23/G24:
  yields = §5.2 blocking primitives only), so the hang was spec-conformant
  scheduling, not an engine bug. The writers now issue a bounded
  property-path `Atomics.wait` (GIL-dropping park, harness.js sleepMs
  rationale) every 256 iterations.
  Attribution of the ORIGINAL chartered GIL-off failure: as with V1 above,
  the GIL-on-only starvation repair cannot explain the GIL-off failure that
  put this test on the charter's 11-failing list, and no pre-repair GIL-off
  signature was captured. Candidate mechanisms are the same pair recorded
  in V1 (pre-repair spin-loop oversubscription timeouts under GIL-off load;
  the since-fixed MC-SAFE S4 stop-deadlock cross-family flake). The GIL-off
  leg of this closure is OBSERVATIONAL against the 20/20 GIL-off Release
  bar, not a point fix — a recurrence must be triaged as a possibly-new
  cause, not assumed to be the repaired hang.

### V9. Bytecode generated/validated once, consumed by N tiers on N threads — IMMUNE (delegated residue tracked elsewhere)

- Surface: `ScriptExecutable`→`CodeBlock` first install and
  UnlinkedCodeBlock generation; `Source/JavaScriptCore/dfg/DFGPlan.cpp:622-646`
  comment documents the landed serialization.
- Governing spec: SPEC-ungil §N.8 (compile fully outside cell locks; publish
  = release-CAS of `m_codeBlockFor{Call,Construct}`; loser DISCARDS and
  adopts the winner; CodeBlockSet registration under its heap lock), annex
  CBI + N7 row R12.
- Why immune for MC-VAL: the validated artifact (bytecode stream) is
  immutable after a single release-publication; both racing generators run
  the validator themselves and one whole result wins atomically — there is
  no partially-validated hybrid for a consumer to adopt. The MUTABLE parts
  of consumed bytecode state are precisely V1 (LLInt metadata) and V2
  (IC state), verdicted above; profiling-derived consumption is V4.

---

## Summary table

| # | Surface | Verdict | Governing invariant | Test |
|---|---|---|---|---|
| V1 | LLInt metadata caches | needs-test | SPEC-jit §4.3/I13/I18 single-u64 self-validation | mc-val-llint-cache-storm.js |
| V2 | Baseline/handler IC state | immune | SPEC-jit §4.2 I6 (structural), §5.1 F1/F2, I7/I9 | — |
| V3 | Call-link records | immune | SPEC-jit §5.8/F6/I4/I16 immutable-record publish | — |
| V4 | Compile validation vs link | needs-test | SPEC-jit §5.5/§5.6/I12/I21 + DFGPlan reallyAdd revalidation | mc-val-fire-vs-link.js |
| V5 | TID namespace aliasing post-reissue | needs-test (D1/D1R landed gilOffProcess; immune GIL-on via Dev 10) | SPEC-ungil §D.1/D1R phase ordering + in-stop restamp+fire (Heap.cpp:6132-6197), OM E4/I11/I15 | mc-val-tid-reissue-false-owner.js |
| V6 | Sharded atom table identity | needs-test | vmstate §4.3 A1/I5 + §4.4 no-resurrection | mc-val-atom-identity.js |
| V7 | Wasm validate-then-consume | immune | copy-once-pre-validate (JSWebAssemblyHelpers.h:165-184) + SPEC-ungil §I refusal + §N.6 | — |
| V8 | Multi-slot Structure consumers | needs-test | OM M7/I24/I34/D1/I7 + manifest-7b audit | mc-val-multislot-clone.js |
| V9 | Bytecode publish/consume | immune | SPEC-ungil §N.8 release-CAS, loser-discards | — |

No surface is currently verdicted susceptible-suspected. Standing risks to
that conclusion, tracked above: (1) the V4 KNOWN RESIDUAL in
DFGPlan.cpp:645-651 is only safe while I12's "guards validate" covers every
profile consumer — any future unguarded profile-derived fold flips V4 to
susceptible; (2) V5's needs-test verdict rests on the D1/D1R restamp-surface
completeness + D1R-fire-coverage enumeration claims and the deferred U-T12
arm (3) — any new tagged-state holder outside `forEachLiveCell`'s reach, or
any baked dead-TID immediate not tied to a restamped structure's TTL set,
flips V5 to susceptible.

2026-06-15 re-audit delta vs the 2026-06-07 original: V5 re-verdicted
(immune→needs-test, tripwire tripped by U-T12 landing); V1-V4, V6-V9
unchanged (V4 KNOWN RESIDUAL comment re-verified present at
DFGPlan.cpp:645-651; V1/V8 closure records carried).

Cross-references: the racing special case of every surface here is MC-DF
(CVE-AUDIT.md); V4 overlaps MC-JIT (check-elision side) and MC-CODE
(install/jettison funnel); V8's sequential twin is MC-REENT. Tests follow
the corpus conventions (harness.js, bounded loops, every thread joined,
amplifier-ready per api G15); they are NOT run in this slice.
