# SPEC-objectmodel — review-resolution history

## PART A / PART B boundary (read this first)

This file has TWO mechanically distinct parts (spec line 3 "NBR"):

**PART A — SUPERSEDED (rev 12):** the former normative-annex sections were moved
VERBATIM to `docs/threads/SPEC-objectmodel-annex.md` (a sub-cap FROZEN NORMATIVE
file); THIS ENTIRE FILE IS NOW NON-NORMATIVE audit trail. The list below is kept
for the record; consult the annex, not this file:
1. `Appendix: full-detail ground-truth rows` (GT1—GT17, incl. GT10's T1/T2-vs-locked
   resize-site classification),
2. the two `Deviations from THREAD.md` lists (rev-7 full list + rev-8 addition) — together
   they are "history §14" as cited by spec §1,
3. `15.T12 Task-12 race-suite scenario list`,
4. `App. R8 — §10.7 guard list`,
5. `16.8 Cross-spec ledger — full dispositions` (rev-9 state; current state=spec §10.8,
   which takes precedence; §17 logs the rev-10 updates).
(Round-6's "a separate annex file is not possible" no longer holds: the spec
process owns docs/threads/SPEC-*.md, so SPEC-objectmodel-annex.md carries the
normative annex; implementers treat it read-only exactly like the spec.)

**PART B — everything else.** Non-normative audit trail (review-round logs, rationale,
refutation arguments). New review rounds append here ONLY.

Verbatim review-resolution logs moved out of `docs/threads/SPEC-objectmodel.md` (formerly its §12/§13) to keep the spec under the size cap. The normative outcome of every entry below is folded into the frozen spec; this file is the audit trail only. Section numbers and `SPEC-*.md:line` citations inside the logs refer to the revisions current when each round closed and may have drifted.

---

## 12. Round-1 review resolutions and refuted details

Resolutions (all accepted findings are folded into the normative text above):

- DCAS vs concurrent header-byte mutators (cellState CAS, lock parked bit) → §3.0
  merge-retry discipline, §9.2 helpers, I5 restated, I26. Evidence: `JSCell.h:226-233,251`;
  `LockAlgorithmInlines.h:144` (parked bit CASed while held); `IndexingType.h:97-98`.
- arm64 reader load-load reordering → M7 + I24, modeled on `JSObject.cpp:385,400,403-405`.
- Aliased-flat-butterfly GC liveness & missing marking spec → spine
  `aliasedAllocationBase/Size` (§4.1), visit algorithm §4.5, I7 corrected, I25, task 6b.
- §4.1 addressing inconsistency → exact equations with arithmetic verification (§4.1);
  out-of-line slot-within-fragment is `3−(k%4)`, indexed mapping is `(i+1)/4,(i+1)%4`
  with the header in fragment 0 slot 0; C1/C2/C3 capacity & preCapacity rules.
- Locked transition vs winning array CAS → §4.3 failure taxonomy case (c), I27b.
- Null-butterfly / first-install / inline-add races → §2 None row, §2.1 N1-N4, E4
  amended, I21 expanded.
- Nuking vs single-DCAS composition → M5 rewritten; §4.2 step 5 / §4.3 step 5.
- Double arrays → §4.7 R-DOUBLE rule, I28. ArrayStorage/preCapacity → §4.6, C3.
- JSArray.cpp + PropertyTable ownership → added to owned paths (header of this doc);
  quarantine relocated to `PropertyTable.h/.cpp` (§6), citing `PropertyTable.h:152,293,467`.
- butterfly() contract / unowned callers → §9.5 contract + §10.7 guard manifest +
  `mayBeSegmentedButterfly()`.
- §4.2 missing watchpoint-fire step → step 0, I10b, F2 ordering.
- §3 vs §4.4 foreign-resize conflict + unspecified CAS tag bits → resolved in rev 2 by
  the T1-T4 carve-out, which round 2 then proved unsound; superseded by §13's first
  entry (T1 owner+SW=0 only, no exemptions). Recorded so the history is traceable.
- I22 vs unconditional masking → I22 restated (behavior/layout identity + no-op mask);
  machine-code identity moved to E3 where it is per-tier testable.

Refuted/corrected reviewer details (so round 2 does not re-trip on them):

- "The flat IndexingHeader is the 8 bytes AT the butterfly pointer (Butterfly.h:168-171)"
  — incorrect: it occupies [B−8, B); `IndexingHeader::offsetOfIndexingHeader() == −8`
  (`IndexingHeader.h:46`), `Butterfly.h:168` merely forwards. Elements start at B+0, not
  B+8. The layout contradiction the finding pointed at was nonetheless real and is fixed
  by §4.1's equations (which place the header at B−8 in indexed fragment 0 slot 0).
- "out-of-line slot k is at pointer−8(k+1)" — off by the header: it is at B−16−8k
  (`propertyStorage()` is B−8 per `IndexingHeader.h:97-99`, and
  `offsetInOutOfLineStorage = −k−1`, `PropertyOffset.h:106-111`). Same conclusion
  (descending addresses, slot-reversal within fragments) — adopted in §4.1.
- "masked 128-bit CAS does not exist in hardware, so merge-retry is the only sound
  formulation" — agreed and adopted; noted here only to record that the alternative
  (excluding volatile bytes from the comparison) was considered and rejected for the
  reviewer's stated reason.

---

## 13. Round-2 review resolutions and refuted details

Accepted findings, folded into the normative text above:

- **Copying flat resize loses racing writes (SW=1 / foreign)** → §4.4 rewritten: T1
  copy path is owner+SW=0 only (the §3 SW-setting DCAS is the synchronization point —
  it changes the CASed word and fails the resize CAS); T2 carve-out withdrawn; foreign
  or SW=1 resizes grow segmented. §3's exemption sentence deleted; I10/I27 restated;
  Deviations entry added.
- **§4.3(b) merge-SW-and-retry unsound for copied-flat publications** → (b) split into
  (b1)/(b2); §3's "may stay flat" qualified (stay flat only if the DCAS succeeds with
  SW=0 expected; copied-flat plan abandoned on SW flip, converting to segmented
  aliasing the *current* butterfly).
- **§4.5 step 4 visited the wrong (low) slots of partial fragments** → step 4 now
  visits `slots + (4 − liveCount)`, with worked examples and a mandatory cross-check
  test against the flat range (two reviewers filed this; one fix).
- **Delete path stores empty JSValue (encoded 0), tardy readers crash** → §6 D1 /
  I30: release-store `jsUndefined()` (verified: `JSObject.cpp:2388,2397` call
  `clear()` today), plus the GC-visibility argument (maxOffset does not shrink).
- **Wrong Structure.cpp citations for F3** → ground truth #8 and F3/task 3 corrected:
  flatten at `Structure.cpp:1024-1110`, pin at `StructureInlines.h:477`, pinForCaching
  at `Structure.cpp:1112`; the old 415-480/598-670 ranges identified and excluded with
  reasons. Bonus: flatten's cellLock(:1047)-then-m_lock(:1049) order confirms §6's
  lock ranking in-tree.
- **§4.6 ArrayStorage hybrid incoherent** → option (a) adopted: ArrayStorage-family
  objects never segment; shared ones are fully regime 3 behind a per-event stop
  (I31); C3/§4.5 simplified accordingly.
- **C2 produced a phantom indexed fragment for header-less butterflies** → C2 gated on
  `hasIndexingHeader` (verified against `Butterfly::totalSize`, `Butterfly.h:141-146`);
  `indexedFragmentCount = 0`, no header fragment, publicLength accessors assert;
  later element gain = fresh-fragment shape transition. C3 base formula corrected
  (`fromBase` adds +1 unconditionally).
- **E4 TOCTOU across the allocation safepoint** → I29 (allocate-first, validate-late,
  poll-free publish; binds all tiers), wired into §3's E4 rule and §5 E4's soundness
  argument.
- **16-byte CAS lock-freedom asserted, not required** → §9.2 normative inline-hardware
  requirement + startup `__atomic_is_lock_free` RELEASE_ASSERT + arm64 build manifest
  entry + explicit mixed-size-overlap hardware assumption; I32.
- **§10.7 guard manifest stale/incomplete** → list re-derived by running the grep
  against this tree: `runtime/CommonSlowPaths.h` path corrected, `JSONObject.cpp`
  dropped, `JSGenericTypedArrayViewInlines.h` + three `tools/` files added,
  `JSCellButterfly.*` recorded as CoW-exempt, `llint/LLIntSlowPaths.cpp` assigned to
  SPEC-jit's surface.
- **`convertToSegmentedButterfly` could not express §4.2's one-publication rule** →
  signature extended with `(Structure* newStructureOrNull, PropertyOffset, JSValue)`;
  the nullptr two-step (T2) composition defined as a legal state.
- **Two master flags (`useConcurrentJS` vs `useJSThreads`)** → renamed throughout to
  `useJSThreads` (the name SPEC-api/SPEC-jit freeze); §10 entry 1 dedupes and lints;
  SPEC-jit CS1's alias request is satisfied by adoption.
- **No StopReason / no manifest entry for this spec's stops** → all stops now name
  `StopReason::JSThreads` via `JSThreadsSafepoint::StopScope`; §10 entry 6 records the
  dependency on SPEC-jit manifest M4 (which owns that VMManager entry itself —
  SPEC-jit.md:786-823, 877) and the landing-order constraint.
- **`releaseQuarantinedSlots` had no caller** → §6 release path redesigned as lazy,
  owned-code promotion keyed on `GCSafepointEpoch::current()` (SPEC-heap §11 frozen
  facility, read-only use); no SPEC-heap change needed; I18 restated.

Refuted/corrected reviewer details (file:line evidence, so round 3 does not re-trip):

- *"SPEC-jit R3 requires `runtime/ButterflyThreadTag.h` with `pointerMask` etc., which
  nobody provides"* — stale against the SPEC-jit rev in this tree: R3
  (SPEC-jit.md:824-845) adopts **this spec's** `runtime/ConcurrentButterfly.h` names
  verbatim (`butterflyTIDShift(48)`, `butterflySWBit`, `notTTLTID(0x7fff)`, …); no
  `ButterflyThreadTag` string exists in SPEC-jit. `butterflyPointerMask` is
  nonetheless added to §9.1 for convenience.
- *"R3d requires JIT operations this spec never enumerates"* — also stale: current
  SPEC-jit R3 defines the operation shims in **its own** owned
  `jit/ConcurrentButterflyOperations.{h,cpp}` and states "the object-model workstream
  … exports C++ functions only" (SPEC-jit.md:833-841). This spec's exports (§9.3/§9.5)
  are exactly those C++ entry points.
- *"`vmLite->m_butterflyTIDTag` is required and missing from SPEC-vmstate"* — tracked:
  SPEC-jit R5/**CS3** (SPEC-jit.md:846-855, 899) is an explicit append request against
  SPEC-vmstate with a JIT-owned thread_local fallback if denied; not this spec's field
  to add. §9.1's note pins the definition (`currentButterflyTID() << butterflyTIDShift`).
- *"SPEC-jit R3c freezes `WatchpointSet&` and P2's `fireWatchpointSetThreadSafe` cannot
  bind to `InlineWatchpointSet`"* — no such symbols in the current SPEC-jit rev:
  `grep fireWatchpointSetThreadSafe SPEC-jit.md` → no matches; R3 names the accessors
  without a return type; SPEC-jit §5.6 intercepts **both**
  `WatchpointSet::fireAllSlow` and `InlineWatchpointSet::fireAll`
  (SPEC-jit.md:561-563) and explicitly composes with this spec's world-stopped fires
  (SPEC-jit.md:594-596). §9.4 keeps `InlineWatchpointSet&` (the `Structure.h:1107`
  precedent) and now states the single firing protocol.
- *"SPEC-jit M4 adds the JSThreads reason only conditionally and points at the heap
  manifest"* — stale: current R1 says the facility is VMManager "plus manifest **M4**
  (this spec's own manifest — not deferred to another workstream)"
  (SPEC-jit.md:786-790), and M4 appears unconditionally in SPEC-jit §10
  (SPEC-jit.md:877). The real gap on our side — naming the reason and recording the
  dependency — is fixed (§10 entry 6).
- *"`mainThreadButterflyTID`/`notTTLTID` mismatch"* (implicit in the interface
  finding) — none: §2's values are bit-identical to R3's cited values.

---

## Appendix: full-detail ground-truth rows (compressed out of the frozen spec, rev 5) [PART A — FROZEN NORMATIVE ANNEX]

| # | Fact | Evidence |
|---|---|---|
| 1 | Cell header = 8B: m_structureID (4B); m_indexingTypeAndMisc (1B, "Always CAS"); m_type; m_flags; m_cellState | JSCell.h:292-299 |
| 2 | Header bytes NOT owned by transitions, CASed anytime even under the cell lock: m_cellState (GC); parked bit 0x80 (waiter, while the lock is held) ⇒ §3.0 merge discipline (else lost unpark = deadlock, or clobbered GC state) | JSCell.h:226-233; IndexingType.h:98,230; WTF/wtf/LockAlgorithmInlines.h:144 |
| 3 | Butterfly on JSObjectWithButterfly; header+butterfly = first 16B of the cell, 16B-aligned ⇒ one 128-bit CAS covers exactly {header, butterfly} | JSObject.h:1129,913-916,1599-1600; MarkedBlock.h:77 |
| 4 | Flat layout: IndexingHeader [B-8,B); element i at B+8i; propertyStorage()=B-8; out-of-line k at B-16-8k (descending); alloc base = B-8*(preCapacity+propertyCapacity+1); out-of-line capacity multiple of 4 (init 4, x2) | IndexingHeader.h:46,97-99; Butterfly.h:168,148-151; PropertyOffset.h:106-111; Structure.h:103-107 |
| 5 | Butterfly pointer loaded raw in all tiers; not PAC-signed on arm64e; high 16 bits today zero | DFGSpeculativeJIT.cpp:9410,9485,9495; offlineasm/arm64e.rb |
| 6 | 2-bit per-object lock (0x40/0x80 in indexing byte); JSCellLock; GC takes it; reused unchanged as regime-3 lock | IndexingType.h:97-98; JSCell.h:304-313,152; JSCellInlines.h:315-338; JSCell.cpp:283-292 |
| 7 | Nuke/fence publication is CONDITIONAL on `isX86() \|\| mutatorShouldBeFenced()` (default false) ⇒ M8 forces it on; same flag gates in-place butterfly reallocs. visitButterflyImpl re-loads/re-compares structureID+maxOffset after the butterfly load ⇒ didRace (= M7); cell-locks ArrayStorage butterflies only | JSObject.h:816,1142; JSObject.cpp:125-413 (:380-405,:387-396),159-181,1269-1690 (9 nuke callers); JSObjectInlines.h:70-93; Heap.h:913; StructureID.h:39-51; ButterflyInlines.h:209; JSArray.cpp:58 |
| 8 | Structure: m_transitionWatchpointSet (template for new sets); ConcurrentJSLock m_lock. flattenDictionaryStructure takes cell lock (:1047) then m_lock (:1049) — confirms §6 lock order. (Structure.cpp:415-480,598-670 = poly-proto create/materialize/removeTransition helpers, NOT F3 sites.) StructureID = base+offset arithmetic; only allocation needs a lock | Structure.h:1107,1076; Structure.cpp:1024-1110,1112,456(:480); StructureInlines.h:477; StructureID.h:73-112 |
| 9 | Deleted offsets live in PropertyTable (owned) ⇒ §6 quarantine lives there | PropertyTable.h:293,152,467-469 |
| 10 | Resize sites: JSArray.cpp:96 in tryGrowAndShiftButterflyRight (:44-99) — the one T1/T2 site in JSArray.cpp ("createArrayButterflyInDictionaryIndexingMode" does not exist in runtime/); :470 in unshiftCountSlowCase (:357-475, ArrayStorage, caller holds cell lock); :1650/:1818 in shift/unshiftCountWithArrayStorage (:1583/:1785; lock :1612/:1807). :96 → T1/T2 CAS re-dispatch; the other three stay cell-locked (I31), casButterfly only as publication form (T3/I17). Contiguous resizes in owned JSObject.cpp: ensureLengthSlow :3813-3875 (in-place vectorLength-only branch → T5; realloc branch ButterflyInlines.h:192-223); reallocateAndShrinkButterfly :3875-3893 | as cited |
| 11 | STW is closure-shaped; NO StopScope. requestStopAll(StopReason); reasons today {GC, WasmDebugger, MemoryDebugger, JSDebugger}; StopReason::JSThreads = SPEC-jit R1/M4. Frozen interface (bytecode/JSThreadsSafepoint.h): `void JSThreadsSafepoint::stopTheWorldAndRun(VM&, const ScopedLambda<void()>&)` (runs closure with world stopped; inline if already stopped under conductor) + `bool worldIsStopped()`. Caller: entered mutator, no §6-ranked lock, no cell lock; re-load/dispatch after return | VMManager.h:279,201-211; SPEC-jit R1/§5.6/M4 |
| 12 | No 128-bit CAS in WTF ⇒ §9.2 adds it | WTF/wtf/Atomics.h |
| 13 | useHandlerICInFTL exists, default false; JIT workstream flips it | OptionsList.h:638 |
| 14 | AtomStringTableLocker real only under USE(WEB_THREAD); shared-VM-state workstream owns it | WTF/wtf/text/AtomStringImpl.cpp:42-63 |
| 15 | Double arrays store raw doubles ⇒ §4.7; fragments hold only WriteBarrierBase\<Unknown\> | Butterfly.h:196-203 |
| 16 | CoW butterflies immutable (separate JSCellButterfly cell): never SW=1/segmented; CoW→writable replaces pointer under §3/§4 | JSObject.cpp:133 |
| 17 | LocalAllocator FIXMEs mark the heap workstream's sync points | heap/LocalAllocator.cpp:138,170-171 |


## Rev 6 — size-cap compression: non-normative rationale relocated verbatim

Rev 6 changed no normative content. §1's expanded fact cells remain above (rev-4 table).
Rationale sentences removed from the spec body, kept here verbatim:

- §3.0/GT#2: "...else lost unpark or clobbered GC state" — why volatile-byte merging is
  mandatory: dropping a parked-bit flip loses a waiter's unpark (deadlock); dropping a
  cellState CAS corrupts GC marking state.
- §4.3(b2): "the foreign plain store into the old flat butterfly doesn't change the
  CASed word; retrying would drop it (lost write, I21)" — why merge-and-retry is
  forbidden for copied flat payloads.
- §4.5 intro: "After an aliasing conversion the ONLY references to the old flat
  butterfly are the spine's interior fragment pointers — invisible to conservative
  scan, and markAuxiliary needs the allocation base (JSObject.cpp:103-108); hence
  aliasedAllocationBase/Size. Stale readers' stack pointers: conservative scan (I7)."
- §4.6: "a hybrid was withdrawn (flat Butterfly* not recoverable; visitor gaps)";
  "the stop flushed every pre-sharing lock-free access; none can reappear — SW
  monotonic — so copy-based growth under the lock is safe"; "element moves are races
  at SAB granularity (old or new JSValue per slot, never torn)".
- §4.7: "ContiguousDouble payloads are raw doubles (GT#15) — cannot live in
  WriteBarrierBase<Unknown> fragments; in-place reboxing racing a stale-shape reader =
  type confusion."
- E3: "the code-identity claim lives here, not in I22".
- M7: "else arm64 load-load reordering yields {new structureID, stale smaller
  butterfly/spine} → OOB (I24)"; "The reverse hazard {old structureID, new spine} is
  caught in-word by the non-elided TID check; elided code cannot coexist with spines
  (I15/I13). Flat→flat owner growth is benign."
- §9.2: "a libatomic lock-based fallback (GCC ≥ 7 can silently select one for 16B
  __atomic_*) breaks §4.3(c), the 1-byte CASes in the same 16B, and plain tagged-word
  loads"; "Hardware assumption (recorded so nobody 'portably' rewrites it): mixed-size
  atomics on overlapping addresses (16/8/1B inside the cell) are outside the C++
  memory model; we rely on x86-64/arm64 same-address coherence, valid only with inline
  hardware atomics."
- §10.7: "typedArray.set(sharedSegmentedArray) would deref a spine as flat: OOB";
  JSCellButterfly exemption rationale: "CoW never SW=1/segmented (GT#16); recorded so
  the next audit does not flag it."
- D1: "a tardy lock-free reader surfacing encoded-0 = null-cell deref; with D1 it sees
  old value or undefined."
- §6 release path: "a crossed epoch proves every mutator passed a safepoint after the
  deletion."

---

## 14. Round-3 review resolutions (rev 7)

All eleven round-3 blocker/major findings were verified REAL against the tree and
current sibling revisions and are folded into rev 7. No false positives this round.

1. **Stale-spine publicLength > vectorLength OOB (blocker)** — confirmed: publicLength
   (indexed fragment 0 slot 0) is shared across all spines (fragments aliased verbatim
   across T2 grows) while vectorLength is per-spine; a reader pinned on an old spine
   after a T2 grow + push could index past the old spine's fragment array or C2 tail.
   → New C4 + I33: every segmented indexed access and the §4.5 visit bound by
   min(publicLength, SAME-loaded-spine->vectorLength); [vectorLength, publicLength)
   reads as holes (element only ever stored via a newer spine — sound at SAB
   granularity); segmentedIndexedSlot precondition index < spine->vectorLength;
   §4.5 step 5 updated; stress test added to Task 12.
2. **Stale §9.1 contract: VMLite::butterflyTIDTag (major)** — confirmed: SPEC-jit rev 5
   CS3 dropped the field (SPEC-jit.md:289) in favor of `g_jscButterflyTIDTag` TLS in
   jit-owned jit/ConcurrentButterflyOperations.cpp (R5/P5, SPEC-jit.md:271,241).
   → §9.1 rewritten; we consume only currentButterflyTID() (vmstate §6.7).
3. **TID rebias circularly delegated, unowned (major)** — confirmed: vmstate §6.7 and
   api §126 both point back here; no spec implements rebias. → Frozen resolution
   (option a): NO TID recycling while tagging is active this milestone; rebias =
   explicitly unowned future deliverable; I4's rebias clause dropped (SW sticky for
   process lifetime); 2^15 lifetime cap accepted (api maxJSThreads RangeError, api
   I17 already says "recycling point unasserted"); Deviations row added (THREAD.md:21).
4. **getDirect-family callers unsound; §10.7 grep wrong; JSONObject wrongly dropped
   (blocker)** — confirmed: getDirect/getDirectOffset/putDirectOffset/
   putDirectWithoutBarrier reach raw m_butterfly via locationForOffset →
   outOfLineStorage (JSObject.h:710-754); ~45 unowned runtime files call them
   (JSONObject.cpp:612,626,1393 among them); also the arrow-only grep missed DOT
   calls: JSONObject.cpp:1507 `array.butterfly()` and JSArrayBufferView.cpp.
   → Mechanism (a) adopted, stated normatively in §9.5: owned locationForOffset()
   dispatches out-of-line offsets on regime (Segmented → segmentedOutOfLineSlot),
   making the whole getDirect family regime-safe for all callers with no guard list;
   get/putDirectConcurrent retained (they additionally provide M5 nuke tolerance and
   M7 ordering — NOT redundant); I22 story: option-gated branch dead with the flag
   off, machine-code identity remains a JIT-only claim (E3). §10.7 grep fixed to
   `(->|\.)butterfly\(\)`; JSONObject.cpp restored; JSArrayBufferView.cpp added.
5. **Regime-3 ArrayStorage unreachable from dispatch (blocker)** — confirmed: §2's
   decode was tag-only and §3's read rule bypassed the lock; SPEC-jit r5 has zero
   ArrayStorage/I31 handling. → §2 SW=1 row + §3 read/write rules now load the
   indexing byte and divert ArrayStorage/SlowPut to §4.6's locked path for ALL
   accesses; §9.5 adds isSharedArrayStorage(); §10 entry 8 records the cross-spec
   request to SPEC-jit (compiled ArrayStorage paths must call locked operations once
   writeThreadLocal is invalid) and scopes I31 enforcement to interpreter/runtime
   paths until adopted.
6. **aliasedAllocationBase/Size propagation unstated (major)** — confirmed; a
   fragment-array-only "copy" would let GC sweep the aliased flat allocation (UAF).
   → §4.1 struct comment + §4.3 step 1: copied VERBATIM to every replacement spine;
   immutable per object once set; convert→grow×2→GC test added to Task 12.
7. **No STW facility in tree, no interim stub (major)** — confirmed: VMManager.h's
   FOR_EACH_STOP_THE_WORLD_REASON has no JSThreads entry; stopTheWorldAndRun absent.
   → §10 entry 6 now specifies a normative owned stub `jsThreadsStopTheWorldAndRun`
   (RELEASE_ASSERT single-mutator GIL, closure inline on requester's stack,
   worldIsStopped() ≡ true inside); all spec stop sites call only the veneer;
   integrate doc swaps the body for M4's STWR + CS2 GCL bracket. GT11 marked
   "NOT in tree yet". Task 12 race suites run against the stub pre-M4 (also fixes
   the round-3 observation that jit Task 1's degraded stub would crash them).
8. **CodeBlock::m_lock order inverted vs jit §7/heap §6 (blocker)** — confirmed:
   jit §7 (SPEC-jit.md:199-210) and heap §6 leaf row both make CodeBlock::m_lock
   OUTER to the Structure/cell row; objectmodel had it innermost. → §6/I20
   re-frozen: SAL < JSCellLock < Structure::m_lock (heap rank 10); CodeBlock::m_lock
   and all jit-§7 locks above the Structure/cell row NEVER acquired while holding
   ours.
9. **Stale pins (major)** — re-pinned jit r5 / api r6 / heap r5 / vmstate r6; CS2
   added to §10 entry 6; Task 12 gating noted. Dead sibling line-cites into this
   file are theirs to fix (this file's line count changed again in rev 7).
10. **Quarantine release dead in phase 1 + heap contract mismatch (major)** —
    confirmed: GCSafepointEpoch bumps only in shared-mode GC (heap I11/§10.7), and
    heap CR §13.10d records the hook contract. → §6 release re-keyed to owned
    g_butterflyQuarantineEpoch bumped by an addStopTheWorldSafepointHook adapter
    (runs in legacy AND shared collections); resolves heap CR §13.10d with matching
    text; explicit note that no spec implies useJSThreads ⇒ useSharedGCHeap.
11. **SAL obligation / StructureCreateInlines.h + StructureTransitionTable.h
    unowned (major)** — confirmed (vmstate §5.3/N5; files exist in tree, were in no
    owned-path list). → Both files added to owned paths; §6 adopts the SAL
    obligation (heap rank 7a, before JSCellLock, never across STW/park, heap L5,
    GCDeferralContext).

### Deviations from THREAD.md — full rev-7 list (NBR from spec §1) [PART A — FROZEN NORMATIVE ANNEX]

- Butterfly lives on JSObjectWithButterfly (JSObject.h:1126-1183), not JSObject;
  butterfly-less objects carry no tag (§2.1).
- TBI claim outdated — CagedPtr untagged (wtf/CagedPtr.h:38-56); arm64e uses PAC;
  fused cmp/branch conclusion holds.
- 128-bit DCAS assumed but absent (GT#12) — §9.2 adds it.
- Dictionary reads use the cell lock, not "the structure's lock"; table edits keep
  Structure::m_lock (§6).
- Blog recipe treats the header as privately owned — false (GT#2); §3.0 merge-retry
  required.
- Foreign/SW=1 copy-resize of flat butterflies unsound (lost write): owner-only +
  SW=0-only (§4.4).
- Flag renamed useConcurrentJS → useJSThreads.
- THREAD.md:21 "TID recycled at GC after rebias": deferred out of this milestone
  (§2/I4) — no spec owns rebias; recycling without it = false-owner E4/T1 (unsound).

## §15. Round-4 adversarial review — resolutions (rev 8)

Spec went rev 7 -> rev 8. Findings, dispositions, and the full text of everything
the rev-8 spec carries as NBR pointers into this section.

### 15.1 Blocker: PreciseAllocation cells are 8-mod-16 (CONFIRMED)

Verified: PreciseAllocation.h:68-70 (`isPreciseAllocation(cell)` = `cell & halfAlignment`,
halfAlignment = MarkedBlock::atomSize/2 = 8, PreciseAllocation.h:158-159, MarkedBlock.h:77);
PreciseAllocation.cpp `isAlignedForPreciseAllocation` ASSERTs maskedPointer == halfAlignment,
and the cacheLineAdjustment static_asserts reason about sizeof(JSObjectWithButterfly) —
PA-allocated JSObjectWithButterfly instances exist (JSGlobalObject, oversized objects,
IsoSubspace lower-tier). A 16B CAS (`lock cmpxchg16b` / `casp`) on an 8-mod-16 base
#GPs / alignment-faults. Resolution: I36 + §3.0 PA paragraph — PA cells excluded from
dcasHeaderAndButterfly AND from E4 (E4 predicate gains `!isPreciseAllocation(cell)`, one
bit test); every PA transition/conversion/SW-flip is cell-locked and published via the
M8 fenced nuke order (32-bit structureID CAS->nuke, release-store 64-bit butterfly word —
8B-aligned, legal — store new structureID). With E4 off for PA cells every semantic
header/butterfly writer holds the lock, so DCAS pairing is unnecessary; lock-free readers
rely on M5/M7 exactly as for MarkedBlock cells. 64-bit casButterfly (N3/§4.4) remains
legal (cell+8 is 8B-aligned). I1 rescoped to MarkedBlock cells; dcasHeaderAndButterfly
RELEASE_ASSERTs !(cell & 15). Cost note: PA objects (incl. the global object) lose E4 —
their transitions always lock; acceptable (PA objects are rare and large).

### 15.2 Blocker: no structureID re-validation under the lock (CONFIRMED)

Both §4.2 and §4.3 allocated and fired F2 before locking; a LOCKED transition by another
thread in that window changes the structure without failing the later DCAS (the re-read
becomes the expected value), silently dropping the racer's property (I21) or publishing
segmented under a structure whose transitionThreadLocal set is valid (I11/E1 break).
Resolution: shared RESTART rule (§4.2) — step 3 of BOTH protocols compares the re-read
structureID against the planning-time source; mismatch => unlock, discard, re-enter from
§2 dispatch (fresh target, fresh firing checks vs fresh source AND target, fresh
allocation). Plus a publication debug-assert: never publish (notTTLTID,1) while the
published structure's transitionThreadLocal set is valid. I10b extended (RESTART re-check).

### 15.3 Blocker: T5 unsynchronized in-place vectorLength growth (CONFIRMED)

ensureLengthSlow's in-place branch (JSObject.cpp:3813-3874) clears [oldVL,newVL) then
fences and stores VL; two racers lose writes / regress VL. Resolution: T5 is now a
carve-out of T1, not T2: legal only with tagged word (currentButterflyTID(),0), re-checked
unchanged AFTER the clearing loop and immediately BEFORE the fenced setVectorLength;
any change => abandon -> T2. Same-tag racers are impossible (same TID = same thread);
a foreign SW flip or T2 conversion changes the tag and is caught by the re-check.
Residual interleaving analysis: a §4.2 conversion that wins between re-check and the VL
store leaves the bump on the (now-aliased) frozen flat header — tardy flat readers see
empty (cleared) slots below the bumped VL and fall to the generic path; spine readers use
the spine's authoritative vectorLength. Sound.

### 15.4 Blockers (x3, same root): §9.5 getDirect family lacked M7 ordering (CONFIRMED)

Regime dispatch alone does not order an unowned caller's structureID load before the
butterfly-word load; on arm64 a stale smaller flat butterfly (or stale spine) could be
indexed with a newer structure's offset (I24 violation; OOB). Resolution: M7 gains (d) —
a loadLoadFence immediately before the tagged-word load in locationForOffset()'s
out-of-line path (flag-on only; x86-64 no-op), the only conforming option when the
structure load lives in caller code. Segmented branch additionally bounds
offsetInOutOfLineStorage(offset) < 4*spine->outOfLineFragmentCount (I33 out-of-line
clause); out-of-range = stale spine => acquire-re-load the tagged word, re-dispatch.
FLAT-branch soundness proof (NBR from §9.5): every transition that changes the offset
map publishes the butterfly either atomically WITH the structure (locked DCAS, §4.2-5/
§4.3-5) or BEFORE the structure (E4 nuke order: value, nuke, butterfly, structureID;
M5). Therefore any load ordered after a structureID load (which M7(d) guarantees) can
only observe a butterfly >= that structure's storage requirements — never smaller.
Nuked-ID windows reaching unowned callers: new M5 clause — flag-on,
JSCell::structure()/structureID() (owned JSCellInlines.h) mask the nuke bit before
decode; the resulting pre-transition structure's offsets are satisfied by the
not-yet-replaced or superset storage because live storage never shrinks (deletes
quarantine slots — §6/I18/I30; flatten runs per-event-STW when shared — F3; flat->
segmented re-pairs old offsets via the TAG dispatch, not the structure). Exact-decode
paths (transition protocols, GC) keep tryDecode/didRace.

### 15.5 Major: §4.3(b2) re-entered §4.2 while holding the cell lock (CONFIRMED)

Literal reading deadlocked (recursive JSCellLock and/or STW under lock, O2/heap L5).
Resolution: (b2) now = RESTART — release the cell lock, restart the entire operation
from regime dispatch; explicit prohibition on calling §4.2 with the cell lock held
(step 0 may STW, step 2 re-locks). §4.2-0's post-stop path likewise RESTARTs lock-free.

### 15.6 Blocker: quickly-family invisible to the §10.7 grep (CONFIRMED)

canGetIndexQuickly/getIndexQuickly/tryGetIndexQuickly/setIndexQuickly/trySetIndexQuickly
(JSObject.h:303-420) and JSArray::length() (JSArray.h:102) call butterfly() inside owned
headers; their call sites contain no textual butterfly() and were covered by neither
§9.5 nor §10.7. Verified zero-'butterfly()' callers in: interpreter/Interpreter.cpp:351-352,
runtime/ArrayConstructor.cpp, runtime/OperationsInlines.h,
runtime/JSGenericTypedArrayViewPrototypeFunctions.h, runtime/GenericArgumentsImplInlines.h
(the spec's §9.5 cites the first two; this is the fuller list, NBR). Resolution: §9.5
makes the quickly-family + length() dispatch INTERNALLY flag-on: Segmented ->
segmentedIndexedSlot under C4; AS shape -> canGet/canSetIndexQuickly return false
(callers fall to their generic paths = E5 dispatch = §4.6 lock); Flat -> mask + today's
code with the vectorLength bound read from the SAME loaded butterfly. length():
segmentedPublicLength for spines, else one masked load (AS staleness legal under
AS-COPY). Task 2 updated; I33's suite targets cover it.

### 15.7 Blocker: SW=0 ArrayStorage foreign readers (CONFIRMED)

Foreign reads never set SW, so read-only-shared AS stayed regime-1 while the owner
mutated innards in place; a SparseArrayValueMap rehash frees the table under a racing
getOwnPropertySlotByIndex — UAF. This is precisely why the GC visitor cell-locks
ALL_ARRAY_STORAGE butterflies (JSObject.cpp:387-396). Resolution (two parts):
(1) §2/§4.6/I31/L5 — flag-on, EVERY runtime/interpreter access to an AS-shape object,
reads included, ANY SW, takes the cell lock (AS is the slow family; flag-off cost zero).
(2) AS-COPY — because compiled fast paths cannot detect foreign READS either (jit r6
§5.5 keys on SW), AS innards are never relaid-out in place flag-on: shift/unshift and
any vector-moving / indexBias/vectorLength-changing mutation allocates a fresh AS
butterfly under the cell lock and publishes via casButterfly; superseded storage is
never written again, so a stale compiled reader sees a frozen snapshot (SAB-granularity
staleness; conservative scan keeps the old allocation alive, I7). Sparse maps stay
in-place but are runtime-only (jit never fast-paths sparse) => locked on both sides.
Stress: owner sparse-insert loop vs foreign hole-reads (suite list, 15.T12).

### 15.8 Major: CopyOnWrite had no normative rule (CONFIRMED)

CoW payloads are JSImmutableButterfly GC CELLS shared across arrays from one literal;
SW-tagging them corrupts sibling arrays, §4.2 aliasing + §4.5 markAuxiliary of a cell is
heap-accounting corruption. Resolution: §4.8 + I35 — any foreign write, foreign
transition, or owner action with SW=1 on a CoW-shape object first materializes a private
flat butterfly via today's convertFromCopyOnWrite, tag (currentButterflyTID(),0),
published with casButterfly expected = the CoW-tagged word; failure = racing
materializer won => discard, re-dispatch. Racing materializations therefore serialize on
the CAS; CoW words never carry SW=1 and never reach §4.2/§4.4 (the CoW check precedes
F1's DCAS and §4.2 steps 0-1). I35: no SW=1/segmented word ever points at a
JSImmutableButterfly. §10.7's JSCellButterfly.h exemption now rests on I35, not just GT16.

### 15.9 Major: quarantine soundness needed an explicit no-poll invariant (CONFIRMED)

The §6 epoch proof (one world-stopped window after deletion) is sufficient only if no
racing reader holds a derived offset/slot pointer ACROSS a safepoint poll. Resolution:
I34 — no path polls/allocates/may-park between obtaining a PropertyOffset/slot pointer
into object storage and the access through it, unless it re-validates structureID after.
Discharged for ICs by the per-access structure check (dictionary deletes on cacheable
dictionaries change/invalidate via the existing watchpoint protocol; uncacheable
dictionaries are never IC'd); §9.5 *Concurrent accessors are written poll-free; Task 9
audits every owned offset-deref window.

### 15.10 FALSE POSITIVE: "SPEC-jit lacks ArrayStorage/I31"

The finding cited "frozen SPEC-jit rev 5". The tree's SPEC-jit is rev 6 and ADOPTED the
objectmodel manifest-8 request in round 2 (SPEC-jit-history.md N2): §5.5 AS-rule at
SPEC-jit.md:102 (compiled AS fast paths must be (a) E2-elided with registered
writeThreadLocal watchpoints — fire => §5.3 jettison, (b) SW-bit-tested with locked
operationSharedArrayStorage* fallbacks, or (c) excluded), enforced by jit I20
(SPEC-jit.md:207) and tested by jit Task 13. §10.8 updated from "recorded JIT blocker"
to ADOPTED. The genuinely-residual gap the reviewer's scenario implies — foreign SW=0
compiled fast READS — is closed on OUR side by AS-COPY (15.7), not by jit.

### 15.11 Partially stale: §10.6 stub findings

Claim "jit Task 1 degraded STWR = RELEASE_ASSERT(!Options::useJSThreads())" is stale:
jit r6 Task 1 (SPEC-jit.md:272) ships bytecode/JSThreadsSafepoint.{h,cpp} whose interim
STWR RELEASE_ASSERTs <=1 entered VM and runs the closure inline with worldIsStopped(vm)
true inside. The legitimate residue — our §9.4 fire asserts need a checkable witness
before/independent of jit integration — is resolved: our stub sets owned
g_jsThreadsStubWorldStopped (GIL => unraced) around the closure; fire asserts use owned
butterflyWorldIsStopped(VM&) = stub flag || JSThreadsSafepoint::worldIsStopped(vm)
(second disjunct once jit Task 1's header exists). Nesting analysis: fireAll invoked
inside OUR stub reaches jit-§5.6's intercept; its branch 1 (worldIsStopped) may be false
pre-integration, branch 2 calls jit's STWR stub, which under the GIL (<=1 entered VM)
runs inline — no deadlock, no recursive stop. "Single-mutator" witness defined as
vm.currentThreadIsHoldingAPILock() && <=1 entered VM, matching jit Task 1. At
integration the veneer body becomes M4 STWR (+CS2 GCL bracket) and the predicate becomes
jit worldIsStopped alone.

### 15.12 ODR/dllimport: currentButterflyTID re-declaration (CONFIRMED)

§9.1's header block re-declared currentButterflyTID() inside an "all ALWAYS_INLINE"
namespace — ill-formed against vmstate §6.7's sole-provider contract (JS_EXPORT_PRIVATE,
defined only in VMLite.cpp) and broken on Windows (dllimport mismatch). Resolution:
§9.1 now #includes "VMLite.h" with an explicit provider comment and re-declares nothing;
the ALWAYS_INLINE note is scoped to the encode/decode helpers.

### 15.T12 Task-12 race-suite scenario list (NBR from spec §11.12) [PART A — FROZEN NORMATIVE ANNEX]

One named suite per scenario, GIL-stub runnable, re-run unmodified at M4/CS2:
N2 inline-add races (I9/I21) · N3 racing first installs (I21) · T1-vs-SW-flip (I27) ·
T5 racing growers + T5-vs-conversion (I21) · §4.3(b2) stay-flat-growth vs SW flip ·
locked-transition-vs-planned-conversion RESTART (§4.2-3/§4.3-3; I10b/I11) ·
stale-spine reader vs T2 grow+push (I33 indexed AND out-of-line clauses) ·
convert -> grow x2 -> GC -> read pre-conversion property (I7/I25) ·
owner sparse-insert loop vs foreign hole-reads, locked (I31) ·
shift/unshift AS-COPY vs stale reader snapshot (I31) ·
CoW: two foreign writers race materialization (I35) ·
oversized/PA-cell transition suite: foreign write + owner transition on a PA object,
global-object property races (I36) · delete -> re-add quarantine reuse across GC (I18/I30/I34) ·
§4.5 visit-range cross-check outOfLineSize 1..9 (I25/I8).

### App. R8 — §10.7 guard list verbatim (NBR from spec §10.7; verified vs this tree) [PART A — FROZEN NORMATIVE ANNEX]

Guards (runtime/): ArrayPrototype.cpp, ArrayPrototypeInlines.h, LiteralParser.cpp,
RegExpMatchesArray.h, RegExpMatchesArray.cpp, ObjectInitializationScope.cpp,
CommonSlowPaths.h, ObjectConstructorInlines.h, ClonedArguments.cpp, DirectArguments.cpp,
JSCJSValue.cpp, JSGenericTypedArrayViewInlines.h (six contiguous paths :378-413 —
spine-as-flat deref = OOB), JSONObject.cpp (:1507 dot-call; getDirect sites covered by
§9.5), JSArrayBufferView.cpp (dot-call).
Tools: JSDollarVM.cpp, HeapVerifier.cpp, VMInspector.cpp.
Exempt: runtime/JSCellButterfly.h, runtime/JSCellButterfly.cpp (CoW words never
SW=1/segmented — I35/§4.8/GT16).
Not here: llint/LLIntSlowPaths.cpp + DFG/FTL/Baseline (SPEC-jit surface, E1-E5/M7).
List re-derived at implementation time via
`grep -rln -E '(->|\.)butterfly\(\)' runtime/ tools/ heap/ interpreter/ llint/`.

### 15.13 Rationale relocated out of the rev-8 spec body (for byte cap)

- §4.3-5 "plain stores+fences insufficient": SW DCASes and §4.4 array CASes must not
  interleave with a two-store publication; only a paired DCAS makes the {header,
  butterfly} update atomic against them.
- §4.6 first-para UAF detail: SparseArrayValueMap is a WTF::HashMap; rehash frees the
  old bucket array while an unlocked reader walks it.
- §4.8 corruption detail: SW=1 on a CoW word would license foreign element stores into
  storage aliased by sibling arrays; §4.5 markAuxiliary on a GC cell corrupts heap
  accounting; §4.2 fragment aliasing would record a cell as aliasedAllocationBase.
- §10.6 nesting analysis: see 15.11.
- §9.5 flat-branch soundness + M5 nuke-mask argument: see 15.4.
- T5 conversion-interleaving analysis: see 15.3.

### Deviations from THREAD.md — rev-8 addition to the §14 list [PART A — FROZEN NORMATIVE ANNEX]

- PA (PreciseAllocation) cells sit at 8-mod-16 addresses (halfAlignment trick), so the
  128-bit header+butterfly DCAS and E4 lock-free transitions are excluded for them;
  they use the cell lock + M8 fenced nuke order instead (spec §3.0/I36). THREAD.md's
  uniform DCAS assumption holds only for MarkedBlock cells.

---

## §16. Round-5 adversarial review — resolutions (rev 9)

Spec went rev 8 -> rev 9. Pinned sibling revisions refreshed to the on-disk set
(jit r7, api r9, heap r8, vmstate r9). Sections referenced from the spec as NBR
annexes: §16.1 (T5 proof), §16.2 (M5 scope), §16.8 (cross-spec ledger). NBR is
now defined in the spec's line-3 key: a named history section is a FROZEN
NORMATIVE ANNEX that implementers MUST consume (resolves the round-5
"undefined NBR" finding; the annex set is: Appendix GT rows incl. GT10, §14,
§15.T12, App. R8, and this §16).

### 16.1 Blocker: T5 in-place growth vs cell-locked converter (CONFIRMED)

Round-4's rev-8 T5 fix (lock-free re-check after the clearing loop) was
incomplete, exactly as the round-5 reviewer argued. Two holes:
(1) §4.2 step 3 only re-validated the flat POINTER. T5 grows vectorLength in
place — pointer, tag and structureID all unchanged — so a converter that sized
its spine (C2: indexedFragmentCount from flat vectorLength) before an owner's
T5 completed passed every step-3 check and published a spine not covering
[oldVL,newVL). The element the owner stored there (the OOB store that
triggered ensureLengthSlow) became unreachable even to the owner itself after
re-dispatch on the fresh tag — an I21/self-visibility violation, not SAB
staleness. Rev-8 §15.3's residual analysis only covered tardy READERS of the
bumped frozen header, never the triggering element store.
(2) T5 was lock-free while the converter held only the cell lock (no STW once
the TTL sets are already fired), so the converter's reads could land between
T5's re-check and setVectorLength, or between setVectorLength and the caller's
element store — plain check-then-act.

Resolution (reviewer's option (b)): T5 is now CELL-LOCKED (lock; re-check tag
==(currentButterflyTID(),0) under the lock — changed => unlock, T2; clear;
fenced setVectorLength; unlock; bounded straight-line so O2 holds), and §4.2
step 3 re-reads the flat vectorLength/publicLength UNDER the lock, recomputes
slices/C2 counts/aliased base+size when pointer OR vectorLength changed, and
gains the refit escape "counts no longer fit step-1 allocation => unlock, goto
1" (mirrors §4.3 step 3; also fixes the separate round-5 blocker that §4.2
step 3 had no re-allocation escape at all — an undersized spine + O1's no-
alloc-under-lock rule previously left no legal continuation). §4.2 step 5's
failure path notes §4.3(c)'s goto-3 inherits the refit escape. Soundness of
the caller's post-T5 lock-free element store: any converter locking after T5
reads the new VL under the same lock, so its spine aliases memory covering the
new index; any converter locking before T5 publishes a spine/tag that fails
T5's under-lock re-check (T5 => T2). Interleaving with the converter's
step-3..step-5 window is impossible because T5 needs the same lock. Stress:
§15.T12's "T5 racing growers + T5-vs-conversion" suite now also asserts
owner-read-back of the triggering element store across a racing conversion.

### 16.2 Major: M5 masking scope vs GC didRace (CONFIRMED)

Rev-8 M5 said structure()/structureID() both clear the nuke bit — but
visitButterflyImpl implements §4.5 step 1 via this->structureID() +
StructureID::isNuked() (JSObject.cpp:379-380, late re-compare :403). Masking
structureID() would make isNuked() constant-false and silently disable didRace
for every visit, voiding the ReadStructureEarly/ReadButterfly/ReadStructureLate
protocol. Rev 9 makes the scope explicit and consistent across M5, §4.5 step 1
("load RAW structureID"), §9.5 ("M5 structure()-only masking") and Task 2:
JSCell::structure() (decode-level) masks; JSCell::structureID() returns raw
bits, never masked; GC visitation and every isNuked()/didRace test read raw;
debug test keeps a nuked ID visitor-observable flag-on.

### 16.3 Major: quarantine lock-context + flag gating (CONFIRMED)

takeDeletedOffset's only consumer is PropertyTable::nextOffset
(PropertyTable.h:480-483), reached from Structure::add
(StructureInlines.h:270,428) — ordinary property-add transitions including the
E4-elided owner path, which holds NO locks. Rev-8 §6's "under JSCellLock +
Structure::m_lock" was therefore false on the dominant call path; an
implementer RELEASE_ASSERTing it would break delete-then-readd. Rev 9: the
promotion in takeDeletedOffset asserts no locks; its mutual exclusion is the
exclusion that already serializes the surrounding table mutation (E4's
single-transitioner TID gate while the TTL sets are valid, else the locked
transition's cell lock, plus Structure::m_lock for dictionary table edits per
L3). The epoch logic itself is a single owned Atomic, safe under any of these.
Also confirmed: the Quarantined/Reusable split, epoch stamping and
Reusable-only draw are now explicitly flag-on only (I22: flag-off keeps
today's single m_deletedOffsets path and allocation behavior unchanged).

### 16.4 Major: safepoint-hook registration had no manifest entry (CONFIRMED)

heap §9 (:190) requires addStopTheWorldSafepointHook registration "before
client #2 attaches", i.e. at VM/Heap construction — unowned code. Rev 9 adds
manifest §10.4(c) (register the §6 quarantine-epoch adapter at init) and
references it from §6 and Task 9. Lazy registration from owned files is
forbidden by the heap constraint.

### 16.5 Major: stale pins / broken cite matrix (CONFIRMED, with corrections)

Rev-8 pinned jit r6/api r6/heap r5/vmstate r6; on-disk are jit r7, api r9,
heap r8, vmstate r9 (the reviewer's own "vmstate r8" was itself stale). Rev 9
re-pins to the on-disk set and re-verifies every cross-spec cite against it:
jit AS-rule :104 (not :102), jit I20 :208 (not :207), jit Task 1 :274 (not
:272), jit §5.6 worldIsStopped disjuncts :124, jit CS5/CS6 :269-270, heap rank
table :109-127, heap hook :190-191, heap CR 13.10d/f :346, api Dev 10 :20/:129,
vmstate §5.3/N5 :284-295, vmstate §6.7 :542-560. Conflict precedence is stated
in §10.8: the on-disk revisions pinned at spec line 3 govern; jit r7's own
stale self-pins ("OM r8, heap r6") are recorded in §10.8f for the integrator.

### 16.6 Refuted round-5 findings (full arguments)

- "jit r6 says AS innards mutate in place / contradicts AS-COPY" — FALSE
  against the tree: SPEC-jit.md:104 (rev 7) reads "AS relayout ... is
  copy-on-write under the cell lock (OM §4.6 AS-COPY): fresh AS butterfly,
  `casButterfly`; superseded storage never written again; installed
  vectorLength immutable". The quoted in-place parenthetical does not exist in
  jit r7 (jit history N3 records the round-3 adoption). No erratum needed.
- "Dual mutually-blind STW stubs; jit branch-1 assert fires during OM's stub
  phase" — FALSE against jit r7: §5.6's worldIsStopped includes a FOURTH
  disjunct "(interim, pre-M4 ONLY) OM's exported stub witness
  g_jsThreadsStubWorldStopped" (SPEC-jit.md:124), jit Task 1's stub returns
  true "when OM's stub witness is set" (:274), and CS6 (:270) records the
  orchestrator default = witness-reading (OM's veneer delegating to STWR is
  the alternative once Task 1 lands). So TTL fires from inside OM's stub take
  branch 1; no nested stop, no assert failure. The only residue — OM's
  butterflyWorldIsStopped naming a jit header that may not exist yet — is
  handled in manifest 6: the second disjunct is compiled in only once jit
  Task 1's header exists; the stub flag alone is sufficient before that, so
  there is no landing-order constraint between the workstreams.
- "Lock-order contradicts heap master table (CodeBlock leaf; retire lock
  leaf-only)" — MOSTLY STALE vs heap r8: heap :121 already places JIT
  worklist locks + CodeBlock::m_lock at rank 6b ("outer to 7a-10") and :127
  makes GCSafepointEpoch::m_retireLock "takeable under rank-10 cell/Structure
  locks" (CR 13.10f :346 records the reconciliation). The REAL residue:
  heap L1 "never two same-rank locks" vs OM holding JSCellLock +
  Structure::m_lock together (both rank 10; heap's own ":127" wording
  presupposes both held). Rev 9 §6 defines the intra-rank order 10a=JSCellLock
  < 10b=Structure::m_lock (in-tree witness: flattenDictionaryStructure takes
  cellLock :1047 then m_lock :1049) and §10.8e routes the one-line L1
  exemption erratum to heap.

### 16.7 jit CS5 adoption (E2/E3 correction)

jit r7 CS5 routed a correction to OM: E2 elides only the SW branch (writes
always keep the fused TID compare — jit D9 showed validity alone is not a
substitute for the runtime TID check), and E3's mask-only claim applies to
reads. Rev 9 restates E2/E3 accordingly (§10.8d records the adoption). jit
already emits soundly regardless (SPEC-jit.md §5.5).

### 16.8 Cross-spec ledger — full dispositions (NBR from spec §10.8) [PART A — FROZEN NORMATIVE ANNEX]

a. AS/I31 + AS-COPY: ADOPTED by jit r7 (§5.5 AS-rule :104; I20 :208; Task 13
   tests). Residual SW=0 compiled fast READS are sound via OM §4.6 AS-COPY
   (frozen snapshots), which jit :104 cites as "the basis, not locking".
b. OPEN CR -> jit (BLOCKER until adopted): jit r7's frozen §5.5 transition
   predicate and emission recipe pre-date OM rev-8's I36 and contain no
   isPreciseAllocation test, so all tiers would emit lock-free owner
   transitions reachable by PA cells (8-mod-16 bases; e.g. JSGlobalObject),
   racing OM's cell-locked PA writers — the structureID/butterfly-mismatch and
   lost-SW races I36 exists to prevent. Request: add `!isPreciseAllocation(
   cell)` (one bit-test of the cell pointer) to the §5.5 predicate + emission,
   mirroring E4/I36 exactly. Until adopted, multi-mutator acceptance for
   JIT'd lock-free transitions reachable by PA cells is INTEGRATION-BLOCKED at
   jit Task 13; interpreter/runtime paths are sound regardless (I36 lives in
   OM-owned code). Rev-8 §10.8's "No open JIT blocker" claim is withdrawn.
c. TID lifetime, three-spec reconciliation: the binding rule is NO TID
   recycling once OM tagging is compiled in and useJSThreads is on. api Dev 10
   ships free-list reissue "until tagging lands AND is active" and switches to
   "GC-after-rebias" — a mechanism OM r8 explicitly deferred out of this
   milestone — so at integration the switch-over target is "no recycling",
   which is exactly OM §2/I4. api's free-list reissue is legal ONLY in the
   window before OM Task 2 lands (no tags are ever installed, so reissue is
   unobservable to the object model; api I17's reissue test runs there).
   vmstate §6.7's "api §5.1 recycles at join-completion" is a wording skew for
   api's "dying-thread teardown end" (api :20/:97: join completion never
   releases TIDs; release is the LAST teardown step after setCurrent(nullptr));
   vmstate :556 itself already says "tagging on => at GC safepoint after
   rebias", consistent with the binding rule. Wording-alignment CR routed to
   api (state the no-recycling end-state explicitly) and vmstate (replace
   "join-completion" with "teardown end").
d. jit CS5: adopted (16.7).
e. CR -> heap: add the 10a<10b same-rank exemption to L1 (16.6, third bullet).
f. Sibling self-pins: jit r7 pins "OM r8, heap r6" and resolves conflicts
   "heap r6+OM r8 win" — both now superseded/nonexistent. OM r9 supersedes
   OM r8; heap r8 is current. The integrator resolves any conflict against the
   on-disk revisions pinned at OM spec line 3; no jit content change needed
   beyond (b).

### 16.9 Spec-size note

Rev 9 lands at <=40000 bytes by terser operator spacing, deduplicating
restated rules (e.g. §4.6 "Consequences" line — each clause already normative
at its home section: I17, C3, §4.5 step 5, GT#7), and relocating full
dispositions to this §16. No normative content was dropped; NBR annexes are
binding per the line-3 key.

## §17. Round-6 adversarial review — resolutions (rev 10) [PART B]

Spec rev 9 -> rev 10. All cross-spec line cites re-anchored to section anchors
(heap §13.10e convention; jit r8 header mandates the same). Pins refreshed to the
actual on-disk revs: jit r8, api r10, heap r9, vmstate r9 (rev 9 wrongly pinned
jit r7/api r9/heap r8 while claiming "on-disk revs" — the round-5 §16.5 fix had
gone stale again by freeze time).

### 17.1 BLOCKER — O1/I20 "no allocation under JSCellLock" contradicted §4.6 AS-COPY (CONFIRMED)

Evidence: JSArray.cpp:1805-1806 takes `DeferGC deferGC(vm); Locker locker { cellLock() };`
and then `unshiftCountSlowCase(locker, ...)` (JSArray.cpp:357-475) allocates a fresh
butterfly under that lock. §4.6 AS-COPY mandates exactly this shape, and GT#10 keeps
the three AS resize sites cell-locked — so an implementer instrumenting rev-9 I20
verbatim would assert-crash the spec's own mandated path. Resolution: O1 rewritten —
allocation under JSCellLock forbidden EXCEPT under a DeferGC/GCDeferralContext
established BEFORE the lock (the in-tree pattern); I20 rewritten to match; §4.6 AS-COPY
now cites the exemption. The alternative (allocate-before-lock with revalidate/retry,
§4.3 pattern) was rejected for AS: it would diverge from the in-tree unshift code for
no soundness gain (DeferGC already excludes GC under the lock; O2 still bans
safepoint-blocking there).

### 17.2 MAJOR — stale pins; §10.8b declared a phantom OPEN blocker (CONFIRMED)

On-disk: SPEC-jit.md rev 8, SPEC-api.md rev 10, SPEC-heap.md rev 9. jit r8 ADOPTED the
PA-exclusion CR: SPEC-jit.md:102 "(= OM E4 EXACTLY incl. its PA exclusion ...) AND
(runtime) `!isPreciseAllocation(cell)`", and SPEC-jit.md:23 (N4) "OM ledger 8b
RESOLVED". §10.8b flipped to RESOLVED-ADOPTED; Task-13 PA gate noted LIFTED.
REFUTED sub-claim (one round-6 reviewer): "jit r8 §5.5 still contains no
isPreciseAllocation test — I verified by grep". False — the predicate is on
SPEC-jit.md:102 verbatim (the reviewer likely grepped an older working copy or the
§5.5 heading region only). One-line refutation kept in spec §1 so the next round
does not re-trip. All six broken line cites (jit :104/:124/:208/:274,
heap :121/:127) replaced by anchors (jit §5.5 AS-rule, jit §5.6 disjunct 4, jit I20,
jit Task 1/CS6, heap §6 table/leaf row).

### 17.3 MAJOR — §4.5 step-4 unsigned underflow past outOfLineSize (CONFIRMED)

`outOfLineSize - 4j` underflows for fragments j >= ceil(outOfLineSize/4) (capacity can
exceed size; C1 derives fragment count from capacity). Fix: §4.5(4) split —
markAuxiliary ALL fragments; value-visit ONLY j < (outOfLineSize+3)/4 with
`liveCount=min<size_t>(4, outOfLineSize-4*j)` (subtraction now provably non-negative);
slots past outOfLineSize are uninitialized capacity and never value-visited; the bound
is stable because D1 keeps maxOffset/outOfLineSize from shrinking while quarantined
(JSObject.cpp:110-111 is today's visitor bound).

### 17.4 BLOCKER — quarantine eligibility undefined; I18 covered only dictionary-mode deletes (CONFIRMED)

JSC's common delete path is NOT dictionary mode: JSObject.cpp:2391-2397
(removePropertyTransition branch) frees an offset that flows into
PropertyTable::m_deletedOffsets and back out through takeDeletedOffset
(PropertyTable.h:480-483) exactly like dictionary deletes; such slots are accessed
lock-free, so unquarantined reuse reproduces the stale-reader aliasing race (I21
violation) quarantine exists to prevent. Resolution: eligibility made POSITIVE and
TOTAL — every deleted out-of-line offset goes Quarantined; Reusable is fed SOLELY by
epoch promotion; I18 rewritten ("NO deleted out-of-line slot ... until a bump
postdating the deletion"). The reviewer's suggested dictionary-born bypass class was
NOT adopted: pre-dictionary slots stay lock-free even in dictionary mode (L3), so a
sound bypass would need per-slot provenance tracking for negligible win — total
quarantine is strictly simpler and at worst delays reuse by one GC.

### 17.5 MAJOR — Tasks 11/12 wrote outside owned paths (CONFIRMED)

`JSTests/threads/objectmodel/**` and `docs/threads/INTEGRATE-objectmodel.md` added to
the line-5 owned list (mirroring SPEC-jit's convention). §10 header reworded: the
files LISTED in entries 1-7 are not implementer-editable; the INTEGRATE doc is
implementer-authored. Task-12 C++ checks given a location: self-tests in owned
ConcurrentButterfly.cpp behind `verifyConcurrentButterfly`, driven by
i03-selftest.js — no unowned test directories.

### 17.6 MAJOR — Task 1 could not compile dark: hard #include of nonexistent VMLite.h (CONFIRMED)

runtime/VMLite.h does not exist (vmstate W3 creates it concurrently). §9.1 now guards
the include with `__has_include("VMLite.h")` and, when absent, defines an owned
`ALWAYS_INLINE uint16_t currentButterflyTID() { return 0; }` shim inside namespace JSC
(=the §10.6 stub pattern; INTEGRATE doc records the swap). The ODR/no-re-declaration
clause applies only while VMLite.h is present, so no ODR violation is possible: the
two definitions never coexist in a build.

### 17.7 MAJOR — heap L1 still forbids the 10a<10b nesting (CONFIRMED, escalated)

heap r9 line 128 still reads "L1. Never two same-rank locks" and §13.10f's
reconciliation omits the intra-rank exemption; vmstate §7 gives heap the master
ordering. OM cannot self-grant it. §10.8e escalated from CR footnote to
OPEN-BLOCKING (symmetric with how 8b was handled while open): orchestrator must land
the 10a<10b exemption in SPEC-heap §6 before integration. Note the requirement
pre-exists this design: in-tree flattenDictionaryStructure already takes cellLock
(Structure.cpp:1047) then m_lock (:1049), so heap's L1 as written also faults today's
code; this is a heap-spec wording bug, not an OM design risk.

### 17.8 MAJOR x2 — "normative annex hidden in a mutable history file" (PARTIALLY CONFIRMED)

Confirmed: rev 9 gave no mechanical boundary between the frozen annex and the rolling
log, and the 'history' name plus heap's opposite convention ("history NON-NORMATIVE")
invited skipping it. Resolution: this file now opens with the PART A / PART B banner;
each annex section heading carries the `[PART A — FROZEN NORMATIVE ANNEX]` tag; PART A
is frozen (never appended — this §17 and later rounds are PART B); spec line 3
describes the boundary. The suggested separate `SPEC-objectmodel-annex.md` was
REFUSED: this workstream's owned paths are exactly SPEC-objectmodel.md and
SPEC-objectmodel-history.md — creating a third doc file would itself violate the
partition the reviewers are policing. Folding the annex back into the spec was also
refused: it cannot fit under the 40000-byte cap without cutting normative content,
which the cap rules forbid. The effective normative surface (spec + PART A) is
unchanged; what changed is that the boundary is now mechanical.

### 17.9 Size note

Rev 10 lands at 39988 bytes (cap 40000) by: re-anchored cites (shorter than line
numbers), §10.8b shrink on adoption, one-line refutations (full args here),
deduplicating §9.4 PropertyTable / §10.4c restatements of §6, and wording-level
compression throughout. No numbered invariant, layout, signature, lock order,
manifest entry, or task was removed; I18/I20/O1/I32 were REWORDED (I18/I20/O1
strengthened per 17.1/17.4; I32 now points at §9.2 as the single full statement).

================ PART B (non-normative) addendum ================

## §18. Whole-design adversarial review round 1 dispositions (rev 10 -> rev 11)

1. BLOCKER (cross-cutting, x2): "10a<10b exemption missing from heap's master order." CLOSED BY HEAP r10: heap §6 now splits rank 10 into 10a=JSCellLock < 10b=Structure::m_lock with the L1 exemption, recorded in heap §13.10f. This spec's §6 lock-ordering line and ledger 8e updated from OPEN-BLOCKING to RESOLVED; §1 refutation line updated. No semantic change here — the spec always required exactly this order (flatten :1047/:1049, GT#8).

2. BLOCKER+MAJOR (cross-cutting, TID recycling three-way inconsistency): ACCEPTED; resolved in OUR direction. The composed predicate "recycle until tagging lands AND is active" was undefined: OM tagging is gated on the same useJSThreads flag api needs to spawn threads, so api's teardown-time reissue would have been live exactly when forbidden, and rebias (the precondition for safe reissue) is unowned. Adopted rule, all three specs: NO TID reissue, ever, this milestone — api r11 Dev 10/§5.1 kill m_freeTIDs (teardown only erases the m_threads entry; m_nextTID exhaustion at 0x7fff => RangeError at spawn; api I17 loses its "reissue point unasserted" clause), vmstate r10 §6.7 drops the "GC safepoint after rebias" clause (rebias = unowned future WS; N2 closed by non-reuse). Ledger 8c updated. Cost: a process can spawn at most 32766 Threads over its lifetime flag-on; acceptable for the milestone and exactly what OM's sticky-tag soundness (I11/I15, E4/T1) requires.

3. MAJOR (Atomics-on-properties has no concurrent owner; OM §9.5 lacks atomic slot CAS/RMW): ACCEPTED AS CHARTERING GAP. We did NOT add atomicSlotCompareExchange/RMW to the frozen §9.5 this round: phase 1 is GPO (atomicity = JSLock, api §5.6) and a primitive whose memory-model obligations (SVZ-atomicity vs racing plain JIT stores, segmented-fragment and locked-dictionary variants) can only be validated post-GIL would be frozen untested. Instead both specs now carry the explicit unowned-future-WS charter the reviewer offered as the alternative: api Dev 12 + §5.6 note, OM ledger 8g ("atomic slot CAS/RMW added HERE at re-freeze — UNOWNED chartered WS"); INTEGRATE records orchestrator sign-off, mirroring vmstate Dev 10's pattern.

4. MAJOR (TTL elision "realistically dead" in threaded programs): ACCEPTED AS COST-MODEL ACKNOWLEDGEMENT, ledger 8h. The observation is fair: with one shared global, hot structures shared across threads fire E1/E2 during warm-up and the sets are monotonic, so steady state for genuinely concurrent code is the non-elided regime (mask + fused TID cmp/branch + SW branch; jit D9 already prices this) — elision's zero-overhead claim holds for thread-confined structures and for single-threaded/flag-off code, which is the actual hard gate (I22/E3). Per-thread structure cloning/site-splitting is real design work with GC/IC ramifications and is recorded as an unowned future WS; jit Task 13 gains a flag-on single-threaded bench so the non-elided/disabled costs are measured before GIL removal, not assumed.

5. MAJOR (per-object STW Double rebox; shared arrays never hold unboxed doubles): ACCEPTED AS ACCEPTED-COST + CHARTER, ledger 8i. R-DOUBLE stays: in-place rebox of raw doubles racing lock-free readers is type confusion, and a snapshot-and-publish variant (AS-COPY style) is unsound for Double because the OWNER keeps writing raw doubles into the superseded flat butterfly lock-free (lost writes) — the per-event STW is what makes the rebox sound without adding predicates to every owner write. The K-objects-K-stops cost and boxed-double stores into shared Int32 arrays are real and now recorded as accepted phase-1 cost; chartered follow-up (pre-GIL-removal): shared ContiguousDouble via 8-byte tear-free raw-double fragments (doubles need no tag => plain 64-bit atomicity suffices), which also removes the boxing cost, plus batching structure-level conversion into the first F1/F2 stop. Benchmarks gate the follow-up, not this freeze.

6. Byte-cap edits: §1 refutation line compressed to a history pointer (full text preserved in §15-§17); ledger 8f shortened; pins refreshed to on-disk revs (jit r9, api r11, heap r10, vmstate r10).


================ PART B addendum ================

## §19. Whole-design adversarial review round 2 — resolutions (rev 11 -> rev 12)

1. **Cap evasion via >40KB normative history (blocker, 2 filings) — ACCEPTED.** PART A moved verbatim to `SPEC-objectmodel-annex.md` (33-38KB, under the 40000-byte cap); this file demoted to pure audit trail. Spec line 3 now names the annex; all NBR cites repointed. Later r12 size pressure also moved §10.8's rev-11 ledger text (annex §L), manifest entries 1-7b (annex §M) and the §9.5 dispatch paragraphs (annex §Q) — all with in-spec pointers+index summaries.
2. **N1 structure-creator TID kills E4 on shape reuse (major) — ACCEPTED, partially.** E4/F2 re-keyed per-object for butterfly-bearing cells (spec §2.1 N1, §5 E4/F2, I11/I15; jit §5.5 transition predicate updated in lockstep): a tag owner transitioning its own instance through a foreign-created structure neither fires nor locks; the structure TID now governs ONLY butterfly-less (None-regime) transitions, where no per-object owner bits exist (cell header has no spare bits, GT#1) and foreign locked N2 racing the N1-owner's plain E4 stores would be unsound — so foreign butterfly-less adds still fire F2 once per structure. That residual (constructor inline-add chains de-E4'd after a foreign thread reuses the shape) is now explicitly owned: per-thread structure splitting=chartered-owned Task 14 (ledger 8h); jit's now-REQUIRED fire coalescing bounds the warmup storm.
3. **Shared doubles boxed+rebox STW (major) — ACCEPTED.** §4.7 R-DOUBLE replaced: shared Double stays Double; §4.2 builds raw-double fragments (aligned 8B slots tear-free under SAB semantics; holes=PNaN); no boxing, no sharing-onset STW. Soundness: slot interpretation is shape-keyed and shape changes touching Double on shared objects relabel slots in place under per-event STW, so no reader can interpret raw-double bits as JSValues (the GT#15 type-confusion hazard needs a reader holding the OLD shape across the relabel, impossible across a stop given M7/I34 re-validation); GC value-visit skips Double fragments (§4.5-5). I28 restated; F1's rebox clause deleted; Task-12 I28 race suite added.
4. **Allocation inside per-event STW closures vs heap access bracket (major, cross-cutting) — ACCEPTED.** New O4: every §10.6-veneer closure is allocation-free; storage (AS butterflies, flatten targets) pre-allocated before the stop, re-validated inside, RESTART on refit — mirroring §4.2's existing step-0/1 split. Heap records the matching §10A exemption (conductor may WRITE heap memory inside the stopped window without access); jit R1.i carries both notes.
5. **I34 has no owner for ~45 unowned files (blocker) — ACCEPTED.** New integrator-applied manifest entry 7b (annex §M): grep-driven audit of every unowned getDirect/getDirectOffset/putDirectOffset/locationForOffset caller; any window where a poll/allocation can intervene between the offset-producing structure lookup and the deref gets structureID revalidation or moves to get/putDirectConcurrent; out-of-line offsets only (inline slots are never quarantined). I34's discharge now names it; §9.5's "NO guards" claim qualified.
6. **TID lifetime cap (major, cross-cutting) — ACCEPTED.** Ledger 8c rewritten: GC-time rebias=chartered-owned (OM Task 13+api task 15): the shared-GC stop already visits every butterfly word (§4.5); restamp dead-TID tags to 0 (main adopts orphans — sound: ownership semantics identical, just a different live owner; SW preserved) and dead `m_transitionThreadLocalTID`s to 0; stale baked tid-immediates merely fail their compare (conservative slow path). api reissues via m_freeTIDs after the sweep. No new reserved TID value needed. vmstate §6.7+api Dev 10 updated in lockstep.
7. **Flag-on single-thread tax (blocker, cross-cutting) — ADDRESSED via budgets** (reviewer's alternative): jit Task-13 composite flag-on 1-thread gate <=5% adopted cross-spec; OM contributes: r12 changes 2/3 above remove the locked-transition steady state and double boxing from the composite.
8. Phase-1 epoch-reclamation and ISS findings: heap-owned; OM unaffected (its quarantine epoch always ran in both protocols via the hook — explicitly noted by reviewers as the pattern heap now adopts).

## §20. Whole-design adversarial review round 3 — resolutions (rev 12 -> rev 13)

1. **Quarantine epoch process-global vs per-heap GC stops (BLOCKER) — ACCEPTED, fixed.** Confirmed against heap §9/§10: `runStopTheWorldSafepointHooks()` fires once per collection OF ONE SERVER HEAP, and a collection stops only that heap's clients. JSC/Bun processes routinely host multiple VMs/heaps (workers beside the JSThreads VM), so a bump of a process-global `g_butterflyQuarantineEpoch` by an UNRELATED heap's collection does NOT prove the quarantining heap's mutators crossed a stop. Concrete falsification (as filed): T1 holds an I34 offset window for f on O (heap H1); T2 deletes f (stamp E); worker heap H2 collects, bumps E->E+1; T2/T3 adds g, takeDeletedOffset promotes (E < E+1) and reuses the slot; T1 derefs and reads g's value as f — violates I18/I21. Fix (spec §6 r13): epoch is PER-SERVER-HEAP. Owned `ButterflyQuarantineEpochs` registry (ConcurrentButterfly.cpp): `Lock` + pointer-stable map `JSC::Heap* -> Atomic<uint64_t>` (slots never move once created; lookup is delete/promotion slow path only; PropertyTable additionally caches its owning server heap's slot pointer at first quarantine via vm.heap, so the hot promotion check is one indirection, no map walk). The §10.4c adapter (the hook receives `JSC::Heap&`) bumps the slot FOR THE COLLECTING HEAP only. Stamps and promotion compare against the OWNING heap's slot. Deletion-side direction also holds: every flag-on VM/Heap registers its adapter at init, so no heap quarantines forever. I18, §9.4 PropertyTable note, annex §M entry 4c, and heap CR §13.10d updated in lockstep. This mirrors heap's own per-server GCSafepointEpoch (§11), as the finding suggested.
2. **Safepoint storm under N-thread warmup (major, cross-cutting) — ACCEPTED in part.** New F4 chain-fire: any F1/F2 stop also fires still-valid TTL sets along the fired structure's previousID chain and transition-table successors in the same stop. Sound trivially (firing is monotone and always-safe; over-firing only costs elision, never correctness); it collapses the O(#structures-in-chain) per-constructor warmup stop sequence into one stop per chain. Composes with jit §5.6's REQUIRED coalescing. The budget side (N-thread warmup stop-count/stopped-time ceiling) lands in jit Task-13's integration gate; OM 8h/8i note the trigger. Per-event §4.6/§4.7 STWs stay accepted phase-1 cost (unchanged from r12 ledger 8i).
3. **TTL collapse under shared-constructor workloads (major) — DISPOSITION RECORDED.** Already admitted in ledger 8h; r13 adds the missing trigger: jit Task-13's N-thread shared-constructor construction microbench is the named gate that fires the Task-14 charter (per-thread transition-chain splitting). Pulling Task-14's design forward was REJECTED for this round: it changes Structure-identity assumptions (StructureID VA budget, SAL traffic, IC polymorphism) that all four sibling specs bake in, and the GIL-phase deliverable does not need it; api §2 now discloses the steady-state cost (cell-locked adds on shared shapes until Task 14) at the composed-deliverable level, which is the renegotiation surface the reviewers asked for.
4. **api Dev 12/OM 8g missing from the binding charter list (major) — ACCEPTED (api-owned fix).** api §2 r13 enumerates it; annex §L 8g now states it is GIL-removal-gating and cross-references api §2.
5. Cap compliance: r13 paid for by deleting the non-normative Refutations pointer line, the rev-11-dispositions tag in §10.8's intro, and assorted justification parentheticals (§4.5 underflow derivation, §4.6 UAF rationale — both preserved here and in §15.7); no layout, signature, invariant, lock-order, manifest, or task content dropped. Sibling pins refreshed (jit r11, api r13, heap r12, vmstate r12).

## §21. Round-4 COMPOSED-design adversarial review — resolutions (rev 14)

### 21.1 BLOCKER (CONFIRMED): shared-Structure transition machinery had no N-mutator protocol
Verified in tree: `Structure::addPropertyTransition` (Structure.cpp:563) calls the UNLOCKED
`addPropertyTransitionToExistingStructure`; the m_lock-taking `...Concurrently` variant
(Structure.h:303) exists but was compiler-thread-only. `takePropertyTableOrCloneIfPinned`
(Structure.cpp:912-924) STEALS the source's PropertyTable (unpinned arm takes m_lock only for
`setPropertyTable(nullptr)`, then the NEW structure's `Structure::add` mutates/rehashes the
stolen table object) while a concurrent mutator's unlocked `StructureInlines.h` walk could
still hold a pointer to it. Under r13's E4/F2 per-object keying, two threads owning distinct
instances of one shared Structure S both run these paths lock-free — racing unlocked lookup
vs m_lock-held insert/rehash (single-entry→map switch, WeakGCMap rehash) = UB, and steal+
rehash vs unlocked `Structure::get` walk = UB. SAL serializes only ID-creating creates and
allocating transition-table INSERTS (writer-writer); JSCellLock is per-INSTANCE and cannot
serialize per-Structure state; L3/m_lock covered only dictionary table edits. The round-3
disclosure ("concurrent prop adds on shared shapes=cell-locked until Task 14") did NOT match
the frozen E4/F2 text. RESOLUTION: new normative L6 + invariant I37 + Task 3c + Task-12
same-shape add-storm suite. L6: (i) mutator transition-table lookups via the Concurrently
variant; (ii) steal/clone/materialize under the SOURCE's m_lock, stolen/fresh tables private
until publication; published-table mutations under that structure's m_lock; (iii) mutator
uncached table walks hold m_lock across the walk. Lock order intact: SAL(7a)<m_lock(10b);
cell-locked §4.3 paths take 10a then 10b (existing exception). O1 extended to m_lock
(materialize allocates ⇒ pre-lock DeferGC; a 10b holder must never park, heap I6/L3).
Cost: uncontended WTF::Lock on table SLOW paths only (IC hits and E4 fast paths with baked
target structures do not walk tables); measured by jit Task-13 composite gate. api §2
wording reconciled to "cell-locked+structure-table-locked (OM 8h/L6/I37)".

### 21.2 MAJOR (PARTIALLY CONFIRMED): TTL elision dead for data-parallel constructors
CONFIRMED cost trajectory: F2's third clause fires both TTL sets on the first foreign
BUTTERFLY-LESS add (inline-capacity constructor stores), F4 chain-fires the previousID chain+
successors, sets are monotonic ⇒ after thread 2's first constructor run, every thread's
transitions on that chain drop to locked N2/§4.3 forever. This is the disclosed 8h residual;
what the finding correctly attacked is (a) the remedy (Task 14) being post-GIL with (b) the
detection bench gated behind M4/CS2. RESOLUTION: jit Task-13's shared-constructor
construction microbench MOVED to PRE-integration, run against the GIL stub with sets
force-fired (relative per-op E4 vs locked cost is measurable single-threaded); the Task-14
promotion decision is made on that data BEFORE integration (spec §10.8 r14; jit Task 13).
REFUTED sub-fixes (suggested by the reviewer): (1) lock-free "CAS-only butterfly-less adds
per N3's pattern" is UNSOUND — two racing adds of DIFFERENT properties from the same source
S both allocate the SAME next offset; I9 requires the value release-stored BEFORE the
structureID CAS, so the CAS loser has already clobbered the winner's slot value ⇒ winner's
structure paired with loser's value violates I21 ("read of f returning g's value"). N3 is
immune only because the first install's value lives inside the not-yet-published butterfly;
N2 inline adds write a SHARED slot. Legalizing post-store rollback or store-after-CAS breaks
I9. (2) "per-structure transition lock instead of per-object cell lock" has the same
uncontended cost (one CAS pair) and is strictly WORSE contended: N threads constructing
DISTINCT objects through one shared shape all serialize on S, whereas cell locks don't
contend at all. So cell-locked N2 stands as the steady-state fallback; the real fix remains
Task 14 (per-(thread,shape) transition-chain clones), now decided pre-INT.

### 21.3 heap-L4 contradiction (CONFIRMED, heap-side fix)
OM O1/§4.6 AS-COPY mandate allocation under JSCellLock with pre-lock DeferGC (in-tree
pattern JSArray.cpp:1805-1806); heap L4 said "cellLock never wraps allocation". heap r13
rewrites L4: MarkedBlock internals split to rank 9b (never wrap allocation); 10a/10b holders
may allocate ONLY under pre-lock DeferGC, the allocation may take ranks 7-9b (sole sanctioned
back-edge), acyclic because rank 7-9b critical sections never acquire 10a/10b (asserted).
O1/I20 now cross-cite heap L4. No protocol change on the OM side.

### 21.4 Options bootstrap (CONFIRMED, cross-spec)
OM had NO escape for OptionsList.h symbols (ConcurrentButterfly.cpp references §9.6 options;
Tasks 1/10/12 unbuildable). Unified r14 convention (recorded in jit §10, heap §14, vmstate
R4, api §3, OM §9.6): orchestrator PRE-APPLIES all five specs' OptionsList.h entries (api
9.2-1 canonical for useJSThreads)+jit M2a/M4a before fan-out; all per-spec local-patch
escapes DELETED; remaining manifest hunks (OM §10 entries 2-7b etc.) buildable via heap-§14
private overlay worktrees, hunks never committed.

### 21.5 Editorial (size cap)
r12 ledger deltas c/h/i relocated verbatim to annex §L2 (still NORMATIVE); §10 manifest
index compressed (annex §M is authoritative); assorted rationale parentheticals moved here.

## §22. UNGIL Race C closure record (2026-06-09, non-normative) — butterfly-stress silent value corruption was NOT an object-model bug

Incident: `JSTests/threads/jit/spawned-thread-butterfly-stress.js`, GIL-off full
JIT, ~1/120 under 6-way load: a foreign `o["p"+p]` get_by_val read returned the
clean Int32 value of a SIBLING property of the same object ({p8,p17}, observed
in both directions; "named property corrupt: got ...008 want ...017" and the
mirror). No crash, no ASAN/TSAN report, storage verified intact at the instant
of corruption ($vm.dumpCell: correct value in the slot; re-read HEALED).

Root cause (confirmed by on-demand differential, Tools/threads/bughunt/
EVIDENCE.md §§10-11): `KeyAtomStringCache::make()` (per-VM 512-slot key-atom
cache, shared by all lites under GIL-off) verified the slot's atom by
hash+equal and then RE-LOADED the slot on return (`return slot;`). "p8" and
"p17" are the test's unique 512-bucket collision pair (hash 15955301 /
14316901, both % 512 == 357); a colliding-key miss-store from another lite
between verification and return swapped in the other valid atom, so the reader
resolved the WRONG uid with a perfectly correct downstream lookup — wrong key,
clean value, tier-independent (C++ slow path).

Fix of record (landed; runtime/KeyAtomStringCache.h + KeyAtomStringCacheInlines.h):
slots are `WTF::Atomic<JSString*>`; make() takes ONE acquire-load snapshot,
verifies the snapshot, and returns the SAME snapshot (the post-verification
re-load no longer exists in the program — wrong-uid return is impossible by
construction under any interleaving); publication is a release store after
func() fully constructs the atom; `tryGetValueImpl()` is null-checked (rope
case takes the miss path); clear() relaxed-stores run only world-stopped (GC
finalize). Flag-off/GIL-on semantics and codegen unchanged (single mutator =>
snapshot == slot; cache has no JIT/B3 probes).

Object-model exoneration (why this is recorded here): the butterfly/structure
publication protocol of this spec was the prime suspect and is CLEARED for this
signature. The test's `late*` chain is PropertyAddition-only, so p8/p17 offsets
are invariant across every structure in the chain; an in-vivo
`Structure::getConcurrently` double-probe RELEASE_ASSERT (same held table lock)
stayed silent across all instrumented runs INCLUDING inside two
on-demand-reproduced corruption runs; a raced locked uid-pointer-compared table
probe can only MISS (undefined), never yield a sibling's clean Int32. The
butterfly was never reallocated during the race window (capacity proof,
EVIDENCE.md §2) and `--verifyConcurrentButterfly=1` never tripped. No protocol
text of SPEC-objectmodel.md or its annex changes as a result of this incident.

Differential evidence (same binary, seeds, flags; only the returned pointer
differs): snapshot-return 0 corruptions in 66 widened-window runs with the
race window provably entered 9+ times (detector trips, both directions);
reload-return restored via env arm: 2/5 corruptions, each causally paired
in-run with a `KEYATOM-RACE` trip. Standing tripwire: keep
Tools/threads/bughunt/repro.js in the load6 soak rotation.
