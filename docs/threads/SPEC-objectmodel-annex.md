# SPEC-objectmodel-annex — FROZEN NORMATIVE ANNEX (rev 13)

This file is the NORMATIVE ANNEX of `SPEC-objectmodel.md` (its line-3 "annex"); it
replaces the former history PART A verbatim. Implementers MUST consume it alongside
the spec. Read-only for implementers (like the spec). `SPEC-objectmodel-history.md`
is NON-NORMATIVE. Precedence: on any conflict, `SPEC-objectmodel.md` rev 12 wins —
r12 changed: E4/F2 keyed per-object (N1 narrowed to butterfly-less), §4.7 shared
ContiguousDouble, O4 allocation-free stop closures, 8c TID-rebias charter,
manifest 7b I34 audit. Sections below keep their historical numbers so spec cites
(GT#n, §14, §15.T12, App. R8, §16.8) resolve here unchanged; r12 also added
§L (cross-spec ledger rev-11 text), §M (manifest entries 1-7b) and §Q (§9.5
dispatch rules), moved from the spec for the size cap.

---
## Appendix: full-detail ground-truth rows (compressed out of the frozen spec, rev 5)

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
    [SUPERSEDED r13: the global epoch was itself unsound in multi-heap processes;
    spec §6 re-keys it PER-SERVER-HEAP (history §20). This entry kept as record.]
11. **SAL obligation / StructureCreateInlines.h + StructureTransitionTable.h
    unowned (major)** — confirmed (vmstate §5.3/N5; files exist in tree, were in no
    owned-path list). → Both files added to owned paths; §6 adopts the SAL
    obligation (heap rank 7a, before JSCellLock, never across STW/park, heap L5,
    GCDeferralContext).

### Deviations from THREAD.md — full rev-7 list (NBR from spec §1)

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

### 15.T12 Task-12 race-suite scenario list (NBR from spec §11.12)

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

### App. R8 — §10.7 guard list verbatim (NBR from spec §10.7; verified vs this tree)

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

### Deviations from THREAD.md — rev-8 addition to the §14 list

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

### 16.8 Cross-spec ledger — full dispositions (NBR from spec §10.8)

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


## §L. Cross-spec ledger — rev-11 dispositions verbatim (NBR; spec §10.8 r12 deltas SUPERSEDE c/h/i below)

8. Cross-spec ledger (full=history §16.8 NBR+§17/§18; precedence=on-disk revs): a. AS/I31+AS-COPY ADOPTED (jit §5.5 AS-rule/I20); residual SW=0 compiled READS sound via AS-COPY. b. jit ADOPTED `!isPreciseAllocation(cell)` ("= OM E4 EXACTLY", SPEC-jit.md:102; jit N4); Task-13 PA gate LIFTED. c. NO TID recycling EVER: api r11 Dev 10 adopts (m_freeTIDs dead; TID exhaustion=>RangeError); vmstate §6.7 concurs (rebias=unowned). d. jit CS5 ADOPTED (E2/E3). e. RESOLVED: heap r10 §6 rank-10 split 10a<10b (L1 exemption; §13.10f). f. resolve cites vs on-disk revs. g. post-GIL prop-Atomics (api 4.5/5.6): atomic slot CAS/RMW added HERE at re-freeze - UNOWNED chartered WS (api Dev 12); r13: GIL-removal-gating, listed in api §2's charter enumeration. h. elision covers thread-confined structures; shared-hot ones run non-elided predicates steady-state (jit D9; T13 flag-on bench); per-thread structure splitting=unowned; r13 trigger=jit Task-13 N-thread construction bench. i. per-object §4.6/§4.7 STWs+boxed doubles into shared Int32: accepted phase-1 cost; shared ContiguousDouble (8B tear-free fragments)+fire batching: chartered pre-GIL.


## §M. Integration manifest entries 1-7b verbatim (NBR; from spec §10 r12)

1. runtime/OptionsList.h - four §9.6 options; useJSThreads deduped; lint: no useConcurrentJS.
2. Sources.txt - add runtime/ConcurrentButterfly.cpp.
3. CMakeLists.txt - install ConcurrentButterfly.h; `-mcx16` (x86-64); arm64 LSE or outlined atomics; §9.2 assert backstop (I32).
4. runtime/VM.h/.cpp - (a) §9.2 startup assert behind useJSThreads; (b) M8: force `heap.m_mutatorShouldBeFenced=true`; (c) §6 quarantine-epoch adapter registration (Task 9; r13: PER-SERVER-HEAP — the adapter bumps the registering Heap's ButterflyQuarantineEpochs slot, NEVER a process-global counter; spec §6 wins). Else none.
5. runtime/JSGlobalObject.* - none (PA-cell global object: I36 locked path).
6. runtime/VMManager.h/.cpp - StopReason::JSThreads, stopTheWorldAndRun, resume ISB=jit M4 (INTEGRATION-DEFERRED). Interim stub (normative, owned ConcurrentButterfly.cpp) until M4: `jsThreadsStopTheWorldAndRun(VM&, const ScopedLambda<void()>&)` RELEASE_ASSERTs APILock held && <=1 entered VM, sets owned `g_jsThreadsStubWorldStopped` around the closure, runs inline. §9.4 asserts use owned `butterflyWorldIsStopped(VM&)`=stub flag || jit worldIsStopped (2nd disjunct once jit Task 1 lands; no landing-order constraint, CS6). ALL stop sites (§4.2-0/§4.6/§4.7/F1-F3) call only this veneer; integrate doc swaps to M4 STWR(+CS2)/jit predicate.
7. Flatness guards in unowned files: `if (object->mayBeSegmentedButterfly()) { /* generic */ }` at entry of each butterfly()-caller. Guard list+grep+exemptions (JSCellButterfly=I35)+jit-surface exclusions=annex App. R8 (NBR; exact functions: integrate doc). quickly-family/length(): §9.5 (grep-invisible). getDirect-family: §9.5, NOT guards. mayBeSegmentedButterfly()=false flag-off (I22); owned files get real dispatch.
7b. I34 unowned-window audit (integrator-applied like 7): grep every unowned caller of getDirect/getDirectOffset/putDirectOffset/locationForOffset; any window where a poll/allocation can intervene between the offset-producing structure lookup and the deref gets structureID revalidation (mismatch=>re-lookup/generic) or moves to get/putDirectConcurrent; out-of-line offsets only (inline never quarantined); site list ships in INTEGRATE doc.


## §Q. §9.5 dispatch rules — locationForOffset/getDirect-family/quickly-family verbatim (NBR; from spec §9.5 r12)

locationForOffset() (JSObject.h:713-725, owned), out-of-line offsets flag-on: (1) M7(d) loadLoadFence; (2) load tagged word: Segmented->segmentedOutOfLineSlot under I33 (OOR=stale spine=>acquire-re-load, re-dispatch); Flat->as today (proof: history §15.4). Hence getDirect/getDirectOffset/putDirectOffset/putDirectWithoutBarrier (JSObject.h:750-754): regime-safe+I24-conforming for ALL callers (~45 unowned files, e.g. JSONObject.cpp:612): NO guards (I34 offset staleness across polls=manifest 7b audit); nuked IDs: M5 structure()-only masking. Flag-off=>branch+fence dead (I22; E3). get/putDirectConcurrent stay (full dispatch+M5 tryDecode, poll-free, I34). outOfLineStorage() (JSObject.h:710-711) flat-only; other callers owned or §10.7-guarded.

Quickly-family (can/get/tryGet/set/trySetIndexQuickly)+length() (owned JSObject.h:303-420, JSArray.h:102, JSArrayInlines.h) call butterfly() in owned headers, INVISIBLE to the §10.7 grep (history §15.6)=>dispatch INTERNALLY flag-on: Segmented->segmentedIndexedSlot under C4; AS->canGet/canSetIndexQuickly FALSE (callers->generic=E5=§4.6 lock); Flat->mask+today (vectorLength from the SAME loaded butterfly); length(): segmentedPublicLength / one masked load (AS staleness legal, AS-COPY). Flag-off=>identity.

## §L2. r12 ledger deltas c/h/i (NBR; NORMATIVE; supersede §L's; from spec §10.8, r14)

c. TID rebias=CHARTERED-OWNED (Task 13+api Dev 10): a shared-GC stop (visits every butterfly word, spec §4.5) restamps dead-TID tags to 0 (main adopts; SW preserved)+dead structure TIDs to 0 (stale baked tid-immediates fail compare: safe); api then reissues; until landed, exhaustion=>RangeError.
h. r12 per-object keying (spec §5) keeps butterfly-bearing transitions elided across shape reuse; residual: foreign BUTTERFLY-LESS adds fire F2 once per structure, then locked N2 process-wide=>per-thread structure splitting=CHARTERED-OWNED (Task 14, post-GIL: per-(thread,shape) transition-chain clones restore N1 ownership); jit fire-coalescing+F4 chain-fire bound warmup; trigger=jit Task-13 shared-constructor construction bench, which (r14) runs PRE-INT vs the GIL stub; Task-14 promotion decided BEFORE integration (spec §10.8; history §21).
i. shared ContiguousDouble SPECCED (spec §4.7: no boxing, no sharing-onset STW); remaining per-event STWs=§4.6 AS+§4.7 relabels (accepted phase-1).

### 15.T12 addendum (r14)
+ I37 suite: same-shape add storm — N threads, one shared constructor shape, racing out-of-line adds on DISTINCT instances; verifies transition-table lookup/insert+property-table steal/walk under L6 (no lost/duplicated transitions, no torn reads).
