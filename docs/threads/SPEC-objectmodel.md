# SPEC-objectmodel - TID/SW butterfly tagging, segmented butterflies, per-object lock

Status: FROZEN rev 14 ("annex"=docs/threads/SPEC-objectmodel-annex.md, FROZEN NORMATIVE, MUST be consumed; "history"=SPEC-objectmodel-history.md, NON-NORMATIVE; r2-r4: history §19-§21). Master flag=`Options::useJSThreads()` (§9.6). Pinned: jit r12, api r14, heap r13, vmstate r13 (on-disk revs); cites: in-tree=file:line, cross-spec=section anchors. STW=stop-the-world, WS=watchpoint set, flag-on/off=useJSThreads, GT#n=§1 row n, B=flat butterfly ptr, AS=ArrayStorage/SlowPut shape, PA cell=isPreciseAllocation(cell) (PreciseAllocation.h:68-70; 8-mod-16; MarkedBlock cells 16B-aligned). NBR=annex sections (GT1-GT17; §14; §15.T12; App. R8; §16.8; §L; §L2; §M; §Q); spec wins on conflicts.

Owned paths (ONLY editable): `JSTests/threads/objectmodel/**` (Task 12), `docs/threads/INTEGRATE-objectmodel.md` (Task 11), plus under `Source/JavaScriptCore/runtime/`: ConcurrentButterfly.h/.cpp (new), Butterfly.h, ButterflyInlines.h, JSObject.h/.cpp, JSObjectInlines.h, JSArray.h/.cpp, JSArrayInlines.h, PropertyTable.h/.cpp, Structure.h/.cpp [SAL], StructureInlines.h, StructureCreateInlines.h [SAL], StructureTransitionTable.h [SAL] ([SAL]=Task 3b emission sites), IndexingType.h (comments only), JSCell.h (doc/asserts only), JSCellInlines.h. OptionsList.h, VM.h/.cpp, JSGlobalObject.*, Sources.txt, CMakeLists.txt, §10.7 files=§10 manifest entry.

## 1. Ground truth

GT1-GT17 (fact+`file:line`)=annex (NBR); restated at use.

Deviations: FULL list=annex §14 (NBR)+r12: E4/F2 per-object keying (§5), shared ContiguousDouble (§4.7), O4 allocation-free stops (§6), manifest 7b, 8c rebias charter; r13: per-server-heap quarantine epoch (§6), F4 chain-fire (§5); r14: L6/I37 (§6), 8h survey, prep (§9.6).

## 2. Tag encoding (regime selector)

Butterfly word: bit 63=SW; 62..48=TID (installer/owner); 47..0=payload. ConcurrentButterfly.h:

```cpp
static constexpr unsigned butterflyTIDShift=48;
static constexpr uint64_t butterflyTIDMask=0x7fffULL << butterflyTIDShift,
    butterflySWBit=1ULL << 63, butterflyTagMask=butterflySWBit | butterflyTIDMask;
static constexpr uint16_t mainThreadButterflyTID=0, // == today's raw ptr
    notTTLTID=0x7fff; // reserved; never a real TID
```

Decode (total): all-zero=>None (§2.1); payload!=0, t!=notTTLTID: SW=0=>Flat (owner t transitions lock-free, §5), SW=1=>Flat shared-written (any transition=>regime 2); TID=notTTLTID, SW=1, payload!=0=>Segmented (spine payload); notTTLTID+SW=0 illegal (I3); payload 0+nonzero tag illegal.

Dispatch tests payload==0 BEFORE any TID compare; TID 0 (main)=>bit-identical to today. api §5.1 allocates TIDs (over cap=>RangeError); we consume currentButterflyTID() (vmstate §6.7). No recycling this milestone (8c charter); tags sticky; 2^15 cap.

### 2.1 Regime None (first install / inline adds)

- N1 (r12: butterfly-less ONLY). No spare cell-header bits (GT#1); butterfly-less ownership=`Structure::m_transitionThreadLocalTID` (uint16, §9.4): sole lock-free BUTTERFLY-LESS transitioner while TTL WSs valid;=creator's TID, copied to targets; others fire first (F2). Butterfly-bearing ownership=the tag (E4); structure TID plays no part.
- N2. Structure-only transitions (butterfly untouched): (i) E4->today's code, or (ii) cell lock+header CAS under §3.0, release-storing the inline value first (no holes, I9). Existing-slot inline access stays lock-free.
- N3. First install: CAS word 0->`encodeButterfly(b, currentButterflyTID(), 0)`; 64-bit CAS if header unchanged, else §4.3 DCAS. Failure=race lost: re-dispatch on the winner's tag. E4-eligible installs may plain-store as today.
- N4. butterflyRegime()=None for word 0; None payloads never dereferenced.

## 3. Regime 1 - flat butterfly (today's layout, GT#4)

### 3.0 Header CAS discipline (every header CAS/DCAS)

Volatile bytes (never owned): m_cellState; lock bits 0x40|0x80 (GT#2). Semantic bytes (lock holder/E4-owned): m_structureID (incl. nuke bit), shape bits 0x3F, m_type, m_flags. Loop: (1) 64-bit header load->expected; desired replaces only semantic bytes, volatile copied verbatim. (2) CAS/DCAS; success->done. (3) Failure: re-read; only volatile bytes (plus SW bit/butterfly payload where §4.3 permits) differ->fold fresh into expected AND desired, goto 2. (4) Semantic byte differs: under cell lock=logic error (RELEASE_ASSERT); lock-free (SW path, N3, §4.4): abandon, re-dispatch (I5/I26).

PA cells (I36): dcasHeaderAndButterfly FORBIDDEN (16B CAS faults on 8-mod-16); E4 excluded (§5). Every transition/conversion/SW flip on a PA cell is cell-locked, published via the fenced nuke order (M8): 32-bit structureID CAS->nuke, release-store 64-bit butterfly word (8B-aligned), store new structureID. E4 off=>every header/butterfly writer holds the lock (readers: M5/M7); 64-bit casButterfly stays legal.

### Access rules

Runtime dispatch on Flat also loads the indexing byte: AS shape, ANY SW=>§4.6 locked (overrides all below); CoW=>§4.8 before any write/transition.

- Read, any thread: word 0->no storage; else TID!=notTTLTID->mask, read as today (M7 if structure check separate).
- Write existing slot, owner or SW=1: mask, store, as today.
- Write existing slot, foreign, SW=0: fire writeThreadLocal(S) (F1/I12); SW via 128-bit DCAS under §3.0, (t,0)->(t,1) (divergence=>re-dispatch); store (Double shape: raw 8B store, §4.7). PA: 64-bit CAS flip under cell lock (I36).
- Transition, owner, SW=0, E4 holds: today's lock-free code: allocate, re-validate E4 (I29; fail->§4.3), store value, nuke, store butterfly (t,0), store StructureID.
- Transition, owner, SW=0, E4 fails: §4.3 under cell lock; stays flat ONLY if DCAS succeeds with SW=0 expected; SW flip after copy=>(b2); header-only may merge SW, retry.
- Transition by foreign thread, or owner with SW=1: convert (§4.2), go segmented (Double §4.7).
- Inline slots: existing-slot access untouched (one atomic EncodedJSValue); ADDS->N2.

## 4. Regime 2 - segmented butterfly

### 4.1 Layout and addressing

```cpp
static constexpr size_t butterflyFragmentSlots=4;
static constexpr size_t butterflyFragmentBytes=32;
struct ButterflyFragment { WriteBarrierBase<Unknown> slots[butterflyFragmentSlots]; }; // mutable; never moves

struct ButterflySpine { // IMMUTABLE after publication (I6)
    uint32_t outOfLineFragmentCount; // left side
    uint32_t indexedFragmentCount; // right side
    uint32_t vectorLength; // authoritative; immutable per spine
    uint32_t spineEpoch; // monotonic, debug
    void* aliasedAllocationBase; // null if none
    uint64_t aliasedAllocationSize; // 0 if none; GC §4.5. BOTH copied VERBATIM
    // to every replacement spine; immutable once set. Then:
    // fragments[outOfLine+indexed], out-of-line first.
};
```

Address equations (I8), aliasing flat B:

- Out-of-line: fragment j covers indices 4j..4j+3, base B-40-32j; index k->fragment k/4, slot 3-(k%4) (slots ascend, flat descends).
- Indexed: fragment f base=B-8+32f; fragment 0 slot 0=flat IndexingHeader [B-8,B), frozen. Element i->fragment (i+1)/4, slot (i+1)%4 (+1 hidden by accessors).
- Live publicLength=fragment 0 slot 0 low half; flat-era vectorLength (high half) frozen forever; live VL=spine's.
- C1: outOfLineCapacity%4==0 at conversion (GT#4); RELEASE_ASSERT.
- C2: indexed fragments only if the flat butterfly HAS an IndexingHeader (Butterfly::totalSize, Butterfly.h:141-146): header-less=>indexedFragmentCount=0, no header fragment, publicLength accessors RELEASE_ASSERT; else=(1+flatVectorLength+3)/4 (+1=header slot); last fragment may cover past B+8*vectorLength, never dereferenced (C4 first). Growth=fresh fragments; indexedFragmentCount==0 gaining elements=§4.3 shape transition, NEW spine+header fragment (I9b: aliased conversions only).
- C3: preCapacity!=0 only for AS, which never segments=>conversions have preCapacity==0 (RELEASE_ASSERT). Aliased base=B-8*(propertyCapacity+1), both header cases; header-less aliasedAllocationSize=8*propertyCapacity.
- C4 (I33): publicLength (fragment 0 slot 0) is SHARED by every spine the object publishes (T2 aliases fragments verbatim); a loaded spine may be superseded=>publicLength can exceed ITS vectorLength. Every segmented indexed access+§4.5 visit bounds by min(publicLength, loadedSpine->vectorLength); [vectorLength, publicLength) reads as holes (SAB staleness). segmentedIndexedSlot pre: index<spine->vectorLength; beyond=>re-load tag, re-dispatch.

### 4.2 Flat->segmented conversion (zero-copy: spine aliases 32B slices)

RESTART (here+§4.3): unlock if locked, discard allocations, re-enter the WHOLE operation from §2 dispatch on fresh tag+structureID (fresh target/F1-F2 checks/allocation); lock-free at restart.

0. Source/target TTL sets valid: stopTheWorldAndRun (§10.6 stub) fires F2 in-closure BEFORE any lock (O2/I13/I10b); after return->RESTART.
1. Allocate spine (+ fragments the trigger needs).
2. Acquire cell lock.
3. Re-read under the lock: header, tagged word, flat vectorLength/publicLength (T5 grows VL in place: pointer/tag unchanged). structureID != planning-time source->RESTART; segmented->unlock, retry segmented; flat pointer OR vectorLength changed->recompute slices, C2 counts, aliased base/size vs current B; counts no longer fit the step-1 spine/fragments->unlock, goto 1; C1/C3 violation=logic error.
4. Trigger adds a property->release-store its value into the fragment slot (M2/I9).
5. Nuke+publish under §3.0 (PA cell: I36 fenced order): CAS structureID->nuke(old); 128-bit DCAS {nuked header, expected flat word}->{new un-nuked header, spine tagged (notTTLTID,1)}. Failure->§4.3 taxonomy (its goto-3=THIS step 3 incl. refit escape).
6. Release cell lock.

Publication debug-assert: structure's transitionThreadLocal invalid (I11/E1; steps 0/3).

### 4.3 Segmented transition protocol (also locked flat transitions)

1. Allocate everything (new spine if counts grow: copy+append; fragments never moved/reused; aliased base/size copied verbatim else GC UAF - I7).
2. Acquire cell lock.
3. Re-read header+tagged word; structureID != planning-time source->RESTART (§4.2); allocation no longer fits->unlock, goto 1; recompute target slot.
4. Release-store the new property's value (M2/I9: value before type).
5. Nuke (header CAS, §3.0; skippable when m_offset+shape unaffected - GT#7); publish {new header, new tagged butterfly} via 128-bit DCAS; PA cell: I36 fenced order.
6. Release cell lock.

DCAS-failure taxonomy (exhaustive); re-read {header, butterfly}:

- (a) Only volatile bytes changed (GT#2): merge into expected+desired, retry.
- (b) SW bit changed (foreign SW-setter, lock-free): (b1) desired payload a spine, or the unchanged current payload: merge SW, retry (SW monotonic; fragments shared). (b2) desired payload a freshly copied flat butterfly (stay-flat growth): merge-and-retry FORBIDDEN (lost write, I21)->RESTART (never call §4.2 holding the cell lock: step 0 may STW, step 2 re-locks - O2/heap L5).
- (c) Butterfly payload changed (§4.4 CAS won, I17): goto 3; never republish older (I27b).
- (d) Other semantic divergence: logic error; RELEASE_ASSERT.

### 4.4 Array transitions (butterfly-pointer CAS, no lock)

Element-storage resizes change only the butterfly pointer (I16): allocate, ONE 64-bit CAS on the tagged word; failure=>re-dispatch on the fresh tag, never blind-retry (sites GT10).

- T1. Lock-free COPYING flat resize legal ONLY with expected tag (currentButterflyTID(),0); new word identical (t,0); an SW DCAS fails this CAS. Failure=>T2.
- T2. All other resizes grow segmented: convert if needed (§4.2), publish a new spine (§4.3-1) via 64-bit CAS, tag (notTTLTID,1). SW=1 flat resizes NEVER copy element storage outside STW (I27).
- T3.=I17; composes with §4.3(c).
- T4. Indexing-SHAPE transitions (e.g. Int32->Contiguous): full §4.3. Double: §4.7; AS: §4.6.
- T5. In-place vectorLength-only growth (ensureLengthSlow in-place branch, GT10): CELL-LOCKED, owner-only: lock; re-check tagged word == (currentButterflyTID(),0), changed->unlock, T2; clear [oldVL,newVL); fenced setVectorLength; unlock (bounded, O2; spine-covered: §4.2-3 re-reads VL under the SAME lock — history §16.1). SW=1/foreign OOB growth=T2, never in-place (I21).

### 4.5 GC integration

Aliased flat butterfly's sole refs=spine-interior fragment ptrs=>markAuxiliary its base (JSObject.cpp:103-108); stale reader stacks=conservative scan (I7). Segmented visit (§9.3 hook in visitButterflyImpl, owned JSObject.cpp): (1) load RAW structureID (M5); nuked->didRace. (2) load tagged word via Dependency; not segmented->flat path. (3) markAuxiliary spine+aliasedAllocationBase if set. (4) markAuxiliary ALL fragments j<outOfLineFragmentCount outside [aliasedBase, +aliasedSize); value-visit ONLY j<(outOfLineSize+3)/4 (outOfLineSize<=capacity; slots beyond=uninitialized, never visited; bound stable per D1): HIGH end, `liveCount=min<size_t>(4, outOfLineSize-4*j); appendValuesHidden(fragment->slots+(4-liveCount), liveCount)`. (5) indexed fragments (none header-less, C2): mark as (4); visit per C4/I33, skip fragment 0 slot 0; C2 tail unvisited; Double shape (step-1 raw structureID, re-checked step 6): fragments marked, slots NOT value-visited (raw doubles); AS never appears (I31). (6) re-load/re-compare structureID+maxOffset (JSObject.cpp:403-405); mismatch->didRace; spine immutability=>no torn spine. Barriers: fragment-slot stores=WriteBarrierBase::set on owner; spine/butterfly publication=vm.writeBarrier(object) like setButterfly; spines/fragments=GC auxiliary, butterfly subspace (I25).

### 4.6 ArrayStorage shapes

AS: preCapacity/indexBias+in-place innards (sparse map=rehashing HashMap; GC already locks, GT#7). Rule=I31: flag-on, EVERY runtime/interpreter access (reads INCLUDED, any SW) cell-locked (§2); AS never segments (history §15.7). First foreign WRITE/transition (F1/F2/owner SW=1)=per-event STW (StopReason::JSThreads): fire still-valid sets, publish (installerTID,1) flat; shared transitions INTO AS materialize flat (transitionerTID,1), also per-event STW; AS storage pre-allocated per O4.

AS-COPY: AS innards NEVER relaid-out in place flag-on. shift/unshift (JSArray.cpp:1650,1818) and any vector move / indexBias/vectorLength change allocate a fresh AS butterfly under the cell lock (per O1's pre-lock DeferGC exemption) and casButterfly it; superseded storage never written again (stale readers frozen; conservative scan, I7). In-place element/publicLength/m_numValuesInVector stores in an installed AS stay legal under the lock (bounds=its now-immutable vectorLength). Sparse-map structural edits stay in place: runtime-only=>locked both sides; jit never fast-paths sparse.

### 4.7 Double arrays

ContiguousDouble payloads=raw doubles (GT#15; racing in-place relabel=type confusion). R-DOUBLE (r12): shared Double STAYS Double — §4.2 of a Double-shaped object aliases/builds RAW-double fragments (aligned 8B slots: tear-free, SAB semantics; holes=PNaN as today); no rebox, no boxing, no sharing-onset STW. Slot interpretation is shape-keyed: readers M7-order or re-check structureID; GC skips value-visiting Double fragments (§4.5-5). Shape changes TOUCHING Double on an SW=1/segmented object (Int32<->Double, Double->Contiguous)=per-event STW relabeling slots in place (no reader holds the old shape across a stop, I34/M7); Int32->Contiguous on shared objects: §4.3.

### 4.8 CopyOnWrite shapes

CoW payloads point at a shared JSImmutableButterfly GC cell (history §15.8). Rule (I35): any foreign write/transition or owner SW=1 action on a CoW object FIRST materializes a private flat butterfly via today's convertFromCopyOnWrite, tag (currentButterflyTID(),0), casButterfly expected=the CoW-tagged word (CoW words never reach SW=1); failure=racing materializer=>re-dispatch. Only then runs the triggering §3/§4.2/§4.4 protocol (CoW check precedes F1's SW DCAS and §4.2 steps 0-1).

## 5. TTL WSs and elision rules

Add to Structure (Structure.h:1107):

```cpp
mutable InlineWatchpointSet m_transitionThreadLocalWatchpointSet; // valid <=> no instance ever notTTLTID
mutable InlineWatchpointSet m_writeThreadLocalWatchpointSet; // valid <=> no instance ever SW=1
uint16_t m_transitionThreadLocalTID; // N1
```

Both start IsWatched for new structures flag-on; flag-off=>none. Monotonic; fired only in STW (I13); transitionThreadLocal fire implies writeThreadLocal. §9.4 fire functions run world-stopped, call fireAll; jit §5.6 intercepts (invalidation/jettison/epoch/ISB).

Elision (we provide the predicates; JIT elides):

- E1. Fast paths may omit the notTTLTID check iff transitionThreadLocal valid+watched (I14).
- E2. Write fast paths may omit the SW branch ONLY iff writeThreadLocal valid+watched; writes always keep the fused TID compare (jit D9/CS5, §10.8d).
- E3. With E1+E2, READS: load tagged butterfly, mask top 16 bits, access - plus M7 on arm64; writes additionally per E2. Flag-off=>every tier emits today's machine code (jit I1; code identity here, not I22).
- E4 (r12 per-object). Owner transition may skip cell lock and (D)CAS - today's code incl. nuking - iff source's both sets valid+watched AND !isPreciseAllocation(cell) AND: butterfly-bearing=>tag==(currentTID,0) (instance ownership; NO structure-TID compare — foreign-thread shape reuse stays lock-free); butterfly-less (incl. N2)=>currentButterflyTID()==source->transitionThreadLocalTID() (N1). (I15/I36; sound only with I29.) E4 elides CELL-level sync only; Structure-table ops obey L6.
- E5. Interpreter/runtime slow paths never rely on elision: full §2 dispatch (None first).

Firing triggers (exhaustive):

- F1. First foreign write to an S-instance with SW=0->fire writeThreadLocal(S) before the SW DCAS completes (I12).
- F2 (r12). Fire BOTH sets on S and target, before cell-lock acquisition (I10b), on: butterfly-bearing S-instance transition by a thread != its tag TID; owner transition with SW=1; butterfly-less transition by a thread != S->transitionThreadLocalTID(). A tag owner transitioning its own instance through a foreign-created S does NOT fire.
- F3. flattenDictionaryStructure (Structure.cpp:1024-1110), pin (StructureInlines.h:477), pinForCaching (:1112) fire both sets on the result when any input set is invalid; flatten rearranges storage in place=>per-event STW on shared objects (butterfly pre-allocated per O4; GT#8 lists non-F3 sites).
- F4 chain-fire (r13). F1/F2 stops ALSO fire still-valid TTL sets along the fired structure's previousID chain+transition-table successors, in the SAME stop (monotone=sound; bounds N-thread warmup; gate=jit Task-13 stop budget).

## 6. Regime 3 - cell lock, dictionaries, deletion

Lock=existing JSCellLock (GT#6); flat/segmented slot access NEVER takes it (except AS, §4.6). Serializes:

- L1. All transitions not E4-elided (§4.3-2; N2).
- L2. Flat->segmented conversion (§4.2).
- L3. Dictionary-mode storage access (table edits also hold Structure::m_lock, Structure.cpp:480). Pre-dictionary slots stay lock-free (never move; I18).
- L4. Deletion and attribute reconfiguration.
- L5. ALL AS-object access flag-on, reads included (= I31).
- L6 (r14)=I37, shared-Structure tables. Flag-on: (i) mutator transition-table LOOKUPS use the m_lock-holding Concurrently variant (Structure.cpp:563->Structure.h:303; inserts already m_lock-held); (ii) PropertyTable steal/clone/materialize (takePropertyTableOrCloneIfPinned Structure.cpp:912-924, materialize) run under the SOURCE's m_lock — a stolen/fresh table is private until its new Structure publishes (mutate lock-free); every table mutation of a PUBLISHED Structure (addPropertyWithoutTransition, removes, dictionary=L3) holds ITS m_lock; (iii) mutator uncached table WALKS (StructureInlines.h get/find families) hold m_lock across the walk. SAL still wraps allocating inserts (7a<10b); compiler-thread Concurrently readers unchanged; flag-off=today (I22).

Quarantine (I18; flag-on ONLY — flag-off=today, I22): PropertyTable (owned, GT#9) splits m_deletedOffsets Quarantined/Reusable. Eligibility TOTAL: EVERY deleted out-of-line offset goes Quarantined - dictionary-mode (JSObject.cpp:2388) AND non-dictionary removePropertyTransition (:2391-2397); NO bypass. Reusable fed SOLELY by epoch promotion; takeDeletedOffset draws ONLY from Reusable.

D1 (I30). Today's delete stores EMPTY (`->clear()`, JSObject.cpp:2388,2397); flag-on, every out-of-line delete (all quarantine-eligible) instead release-stores jsUndefined() before the table edit (tardy reader: old value or undefined). Quarantined slots stay GC-visited; maxOffset/outOfLineSize never shrink while quarantined (JSObject.cpp:110-111).

Release (owned; =heap CR §13.10d; r13 PER-SERVER-HEAP — rationale history §20). §10.4c registers ONE adapter per Heap (Heap::addStopTheWorldSafepointHook; arg=that Heap) at VM/Heap init, BEFORE client #2 attaches (heap §9); runs world-stopped in EVERY collection of THAT heap, legacy AND shared. Adapter bumps its heap's slot in owned ButterflyQuarantineEpochs (ConcurrentButterfly.cpp: Lock+stable map Heap*->Atomic<uint64_t>; PropertyTable caches its heap's slot at first quarantine via vm.heap); entries stamp the OWNING heap's epoch at deletion; takeDeletedOffset promotes stamps<that-heap-current via releaseQuarantinedSlots(). Lock context (r14): takeDeletedOffset reached from Structure::add via nextOffset (PropertyTable.h:480-483; StructureInlines.h:270,428); the surrounding table mutation holds m_lock or is table-private (L6); promotion+epoch-map lock run under it (leaf, heap §6). Sound only with I34.

Lock ordering: `SAL<JSCellLock<Structure::m_lock`. SAL=SharedVMState::StructureAllocationLocker=heap rank 7a; JSCellLock=heap 10a<m_lock=10b (heap §6 L1 exemption; GT#8 flatten). SAL (vmstate §5.3/N5; emission=Task 3b): never across STW/park (heap L5; hence not across §4.2-0); allocates only under GCDeferralContext. CodeBlock::m_lock+jit-§7 locks=heap rank 6b, OUTER: NEVER acquired holding ours.

- O1. Never allocate (GC trigger) under JSCellLock or Structure::m_lock - allocation is step 1 - EXCEPT under a pre-lock DeferGC/GCDeferralContext (JSArray.cpp:1805-1806; users: §4.6 AS-COPY/GT#10 locked sites/L6 materialize; I20 asserts no-alloc OR pre-lock DeferGC; =heap L4's sanctioned back-edge).
- O2. Never block on a safepoint holding these locks (hence §4.2-0); lock spans bounded, straight-line.
- O3. Max one JSCellLock held per thread.
- O4 (r12). Stop-window closures (every §10.6-veneer site) NEVER allocate: storage pre-allocated before requesting the stop, re-validated inside (refit=>RESTART); closures may WRITE heap metadata without heap access (heap §10A exemption; jit R1.i).

## 7. Memory ordering

- M1. Spine-chain loads (tagged word->spine->fragment->slot): plain loads+address dependency (Dependency); no barriers x86-64/arm64.
- M2. Step-4 value stores (§4.2/§4.3/N2): release.
- M3. 128-bit DCAS, 64-bit array CAS, N3 install CAS: seq_cst; §3.0 when the header is involved.
- M4. SW-bit DCAS (§3): seq_cst; subsequent property store may be plain.
- M5. Nuking+DCAS: transitions changing m_offset/shape first CAS structureID to nuke(old); the DCAS carries nuked->new-un-nuked. E4 keeps today's nukeStructureAndSetButterfly (JSObject.cpp:159-181). Readers: GC didRace; §3-dispatch slow paths spin on StructureID::tryDecode while nuked (StructureID.h:39-51; bounded, O2). Foreign readers can see nuked IDs: flag-on, JSCell::structure() ONLY (owned JSCellInlines.h) clears the nuke bit pre-decode (history §15.4). structureID()=RAW bits, NEVER masked - GC visitation (§4.5-1/6, JSObject.cpp:379-380,403)+every isNuked()/didRace test read raw (history §16.2).
- M6. Watchpoint state reads in JIT code: no fences (changes only in STW).
- M7. Reader structure->butterfly ordering (arm64): a structure check+SEPARATE butterfly-word load must order the structureID load first (else stale-smaller-storage OOB, I24). Conforming: (a) address dependency (Dependency::consume, JSObject.cpp:385,400); (b) load-acquire structureID; (c) re-load/re-compare after (JSObject.cpp:403-405; mandatory on nuke-tolerant paths); (d) loadLoadFence just before the tagged-word load. x86-64: no-ops. JIT via E3; owned runtime: (a)/(c); locationForOffset/§9.5: (d).
- M8. Flag-on, m_mutatorShouldBeFenced forced true for heap lifetime (GT#7; §10.4b): fenced nuke/publication the only branch; in-place butterfly reallocs disabled.

## 8. Numbered invariants (stress targets)

- I1. MarkedBlock JSObjectWithButterfly cell bases 16B-aligned; bytes [0,16)={8B header, 8B tagged butterfly}; PA cells (8-mod-16) excluded (I36); dcasHeaderAndButterfly RELEASE_ASSERTs alignment.
- I2. `tagged & ~butterflyTagMask`=null or a valid live butterfly/spine; null payload=>all-zero word.
- I3. notTTLTID=>SW==1; notTTLTID <=> payload is a spine.
- I4. SW monotonic per object; never cleared this milestone.
- I5. Non-E4 transitions publish semantic header+butterfly word in one DCAS (E4: I15; PA: I36); every CAS preserves volatile bytes.
- I6. Published spines never mutated; growth=new spine.
- I7. Nothing reachable freed/reused: stale stacks=conservative scan; aliased flat allocation=spine-recorded base, marked each visit (§4.5).
- I8. §4.1 equations reproduce every pre-existing slot's flat address (k: B-16-8k; i: B+8i; header B-8).
- I9. A reader seeing a transition's new StructureID sees the new property's value (release-stored first), modulo M7.
- I9b. Tardy flat-side array access after conversion bounded by frozen flat-era vectorLength (fragment 0).
- I10. Foreign butterfly transitions (incl. element resizes) fire both TTL sets under STW and produce a segmented object; sole exception AS (I31).
- I10b. WS firing (F1/F2) precedes cell-lock acquisition and first SW/segmented publication (RESTART re-check under the lock).
- I11. transitionThreadLocal(S) valid=>no S-instance ever had TID==notTTLTID; every lock-free S-instance transition was by its E4 key owner (instance tag; butterfly-less: S's transition TID).
- I12. writeThreadLocal(S) valid=>no S-instance ever had SW=1; fired before the first SW DCAS completes.
- I13. TTL WSs fire only in VMManager STW.
- I14. Checks elided only while the set is valid AND watched.
- I15. E4 only with both source sets valid+watched, transitioner==the E4 key owner (§5; instance tag, or structure transition TID when butterfly-less).
- I16. Element-storage resizes never touch the header; they CAS the butterfly word.
- I17. Every butterfly-pointer mutation on an object with indexed properties is (D)CAS, even under the cell lock.
- I18. NO deleted out-of-line slot (any delete kind, §6) is reused until an owning-heap quarantine-epoch bump (§6) postdating the deletion; Reusable fed solely by promotion.
- I19. Dictionary-mode storage access holds the cell lock; table edits also m_lock.
- I20. Lock order SAL<JSCellLock (10a)<Structure::m_lock (10b), all inner to rank-6b CodeBlock/jit-§7 locks; no safepoint under 10a/10b; no alloc under 10a/10b except pre-lock DeferGC/GCDeferralContext (O1); SAL obeys heap L5.
- I21. No lost property adds (incl. N2/N3 races), torn JSValues, structure/butterfly mismatch, or read of f returning g's value, under any race.
- I22. Flag-off: layouts/behavior identical to today; flat tags zero; mask no-op (code identity=E3).
- I23. Spine read chain fence-free x86-64/arm64 (address deps); structure->butterfly edge=M7.
- I24. No reader derefs storage at an offset from a structure not M7-ordered before, or revalidated after, the butterfly-word load.
- I25. Every GC visit of a segmented object marks spine, aliased base, non-aliased fragments; visits every live slot.
- I26. No header CAS/DCAS publishes cellState/lock bits differing from freshest read.
- I27. T1 requires/preserves exactly (currentButterflyTID(),0); T2 publishes (notTTLTID,1); no other resize tags; no resize copies elements from a non-(currentTID,0) butterfly outside STW.
- I27b. A locked transition never republishes an older butterfly payload (§4.3c).
- I28. Shape changes touching Double on SW=1/segmented objects happen only under per-event STW; no reader interprets a slot under a shape not M7-ordered/re-checked vs the loaded spine (§4.7).
- I29. E4 (all tiers): allocate before final validation; no poll/alloc/safepoint between validation and StructureID store; else re-validate, §4.3.
- I30. Quarantine-eligible deletes release-store jsUndefined() (never clear()) before the table edit; quarantined slots GC-visited until released (D1).
- I31. AS never segments; flag-on EVERY runtime/interpreter AS access (reads incl., any SW) cell-locked; no in-place relayout (AS-COPY); SW=1 publication+shared transitions into AS=per-event STW (JIT: §10.8a).
- I32. §9.2 atomics inline-hardware-only; startup flag-on lock-free RELEASE_ASSERT; lock-based 16B CAS forbidden.
- I33. Segmented indexed accesses+GC visits bound by min(publicLength, SAME loaded spine's vectorLength); no deref past the loaded spine's fragments/C2 tail (C4). Out-of-line: §9.5 bound offsetInOutOfLineStorage<4*outOfLineFragmentCount; OOR=>acquire-re-load, re-dispatch.
- I34. No path polls/allocates/parks between obtaining a PropertyOffset/slot pointer and the access, unless it re-validates structureID after. Discharge: M7/I24+Task-9 audit (owned)+manifest 7b audit (unowned callers); D1/I18 rely on it.
- I35. No SW=1/segmented word points at a JSImmutableButterfly; CoW materializes private (§4.8) before any F1/§4.2/§4.4 protocol.
- I36. PA cells: no dcasHeaderAndButterfly, no E4; transitions/conversions/SW flips cell-locked via the M8 fenced nuke order (§3.0); 64-bit casButterfly legal.
- I37 (r14). Flag-on, mutator transition/property-table access only per L6; no lost/duplicated transitions or torn table reads under N-thread same-shape adds.

## 9. Public interface (frozen; runtime/ConcurrentButterfly.h unless noted)

### 9.1 Tag encode/decode, TID

```cpp
#if __has_include("VMLite.h") // currentButterflyTID(): SOLE provider vmstate §6.7
#include "VMLite.h" // (NOT re-declared while present - ODR/dllimport); 0 on main; never notTTLTID
#endif
namespace JSC { // encode/decode helpers below=ALWAYS_INLINE
#if !__has_include("VMLite.h") // vmstate W3 unlanded: owned interim shim (=§10.6 stub pattern);
ALWAYS_INLINE uint16_t currentButterflyTID() { return 0; } // INTEGRATE doc records the swap
#endif
using ButterflyTID=uint16_t; // 15-bit;+§2 constants
static constexpr uint64_t butterflyPointerMask=~butterflyTagMask; // low 48

uint64_t encodeButterfly(Butterfly*, ButterflyTID, bool sharedWrite);
Butterfly* untaggedButterfly(uint64_t tagged); // masks top 16
ButterflyTID butterflyTID(uint64_t tagged);
bool butterflySharedWrite(uint64_t tagged);
bool isSegmentedButterfly(uint64_t tagged); // !=0 && TID==notTTLTID
ButterflySpine* butterflySpine(uint64_t tagged); // pre: isSegmented
}
```

jit R3 adopts this header verbatim; shims=jit/ConcurrentButterflyOperations.{h,cpp}; pre-shifted tag=jit R5/P5 `g_jscButterflyTIDTag` (jit TLS, CS3 init).

### 9.2 DCAS and header-merge helpers

```cpp
struct CellHeaderAndButterfly { uint64_t header; uint64_t taggedButterfly; }; // bytes [0,16)
// Strong 128-bit CAS at cell base; seq_cst. RELEASE_ASSERT(!(cell & 15)): PA forbidden (I36).
bool dcasHeaderAndButterfly(JSCell*, CellHeaderAndButterfly expected, CellHeaderAndButterfly desired);

// §3.0 volatile bytes:
static constexpr uint64_t cellHeaderVolatileMask=(0xffULL << 56 /*m_cellState; GC CAS*/)
    | (uint64_t(IndexingTypeLockIsHeld | IndexingTypeLockHasParked) << 32);
bool headerDiffersOnlyInVolatileBits(uint64_t expected, uint64_t fresh);
uint64_t mergeVolatileHeaderBits(uint64_t desired, uint64_t fresh); // both ALWAYS_INLINE
```

(offsetof+static_asserts; literals illustrative.) I32: inline `lock cmpxchg16b` (`-mcx16`)/`casp` (LSE)/ldxp-stxp ONLY; libatomic forbidden; startup flag-on RELEASE_ASSERTs `__atomic_is_lock_free(16, sampleCell)` (+8/1B); mixed-size atomics on the first 16B rely on same-address coherence.

### 9.3 Spine/fragment operations

```cpp
ButterflyFragment* spineOutOfLineFragment(ButterflySpine*, unsigned);
ButterflyFragment* spineIndexedFragment(ButterflySpine*, unsigned);
WriteBarrierBase<Unknown>* segmentedOutOfLineSlot(ButterflySpine*, PropertyOffset); // pre: I33 bound
WriteBarrierBase<Unknown>* segmentedIndexedSlot(ButterflySpine*, unsigned index); // pre: C4
uint32_t segmentedPublicLength(ButterflySpine*); // frag 0 slot 0, low half
void setSegmentedPublicLength(ButterflySpine*, uint32_t);

// §4.2: ONE publication; nullptr=in-place (T2); no intermediate
// {old structure, undersized spine}.
ButterflySpine* convertToSegmentedButterfly(VM&, JSObjectWithButterfly*, Structure* newStructureOrNull, PropertyOffset, JSValue);
void segmentedTransition(VM&, JSObjectWithButterfly*, Structure*, PropertyOffset, JSValue); //=§4.3
void structureOnlyTransition(VM&, JSObject*, Structure*, PropertyOffset inlineOffset, JSValue); // N2 locked path
void ensureSharedWriteBit(VM&, JSObjectWithButterfly*); // §3 foreign write (F1+R-DOUBLE+§4.8)
bool casButterfly(JSObjectWithButterfly*, uint64_t expectedTagged, uint64_t newTagged);
// §4.4; asserts I27; expected 0=N3 install; false=>re-dispatch, never blind-retry.

template<typename Visitor> // §4.5 GC hook (called from owned JSObject.cpp)
Structure* visitSegmentedButterfly(Visitor&, JSObjectWithButterfly*, ButterflySpine*);
```

### 9.4 Structure additions

```cpp
InlineWatchpointSet& transitionThreadLocalWatchpointSet() const;
InlineWatchpointSet& writeThreadLocalWatchpointSet() const;
bool transitionThreadLocalIsStillValid() const;
bool writeThreadLocalIsStillValid() const;
ButterflyTID transitionThreadLocalTID() const; // §2.1 N1
// Fire world-stopped: RELEASE_ASSERT(butterflyWorldIsStopped(vm)) (§10.6);
// bodies call fireAll (jit §5.6 does the rest). F1-F3.
void fireTransitionThreadLocal(VM&, const char* reason); // also fires writeThreadLocal
void fireWriteThreadLocal(VM&, const char* reason);
```

PropertyTable (owned): §6 quarantine members (split+stamps+cached per-heap epoch slot)+`releaseQuarantinedSlots(uint64_t)`; takeDeletedOffset() gated to Reusable post-promotion.

### 9.5 JSObjectWithButterfly additions

```cpp
uint64_t taggedButterflyWord() const; // raw 64-bit load
ButterflyRegime butterflyRegime() const; // enum class { None, Flat, FlatShared, Segmented }
bool isSharedArrayStorage() const; // SW=1 && AS (dispatch keys on shape ANY SW, §4.6)
bool mayBeSegmentedButterfly() const; // one load+cmp; false flag-off

Butterfly* butterfly() const; // EXISTING signature; masks the tag. CONTRACT -
// flatness via: flag off | dominating dispatch (§3/E5) | class never segments
// | §10.7 guard. Debug ASSERT !isSegmented; RELEASE_ASSERT if verifyConcurrentButterfly.

// Interpreter/runtime slow paths (full §2 dispatch, M7-conforming, M5 nuke-tolerant):
JSValue getDirectConcurrent(PropertyOffset) const;
void putDirectConcurrent(VM&, PropertyOffset, JSValue);
JSValue getIndexConcurrent(unsigned) const;
bool putIndexConcurrent(VM&, unsigned, JSValue);
```

Dispatch rules for locationForOffset/getDirect-family and quickly-family+length()=annex §Q, FROZEN NORMATIVE, verbatim (NO guards; I24/M7; I34=manifest 7b; flag-off identity).

### 9.6 Options (manifest entry 1; names frozen; Bool, false)

`useJSThreads` master switch (shared OptionsList.h entry, api §M/jit M1; off=>I22/E3 identity); `forceSegmentedButterflies` stress: all allocs segment; `forceButterflySWBit` stress: writes treated as foreign (§3 DCAS+F1); `verifyConcurrentButterfly` debug asserts every tag decode (I2/I3)+butterfly() contract. r14 prep: ALL specs' OptionsList entries+jit M2a/M4a=orchestrator-PRE-APPLIED before fan-out (jit §10); other §10 entries: heap-§14-style private overlay, never committed. JIT tiers consume E1-E4+M7 via §9.4 predicates+§9.1 constants; race amplifier/TSAN target §8 invariants.

## 10. Integration manifest (entry files NOT implementer-editable; INTEGRATE doc=implementer-authored, Task 11)

Entries 1-7b=annex §M, FROZEN NORMATIVE, applied verbatim; `§10.x` cites herein=annex §M entry x (index: 1 Options; 2 Sources; 3 CMake; 4 VM; 5 JSGlobalObject none; 6 VMManager veneer+NORMATIVE interim stub `jsThreadsStopTheWorldAndRun`/`butterflyWorldIsStopped` (swap at jit M4/CS2/CS6); 7 flatness guards per annex App. R8; 7b I34 audit).
8. Cross-spec ledger=annex §L (NBR); r12 deltas c/h/i=annex §L2 (NORMATIVE, supersedes). r14, 8h+: post-fire fallback survey — per-structure transition lock AND lock-free N2 header-CAS REJECTED (contention; same-offset value clobber breaks I9/I21; history §21); cell-locked N2 stands pending Task 14, promotion DECIDED PRE-INT on jit Task-13's GIL-stub construction bench.

## 11. Ordered task list

1. ConcurrentButterfly.h skeleton: §2 constants, §9.1 encode/decode, §9.2 DCAS+merge helpers (self-test behind verifyConcurrentButterfly), ButterflyRegime. Compiles dark (I22; §9.1 shim if VMLite.h absent).
2. JSObject tagged accessors: butterfly() mask-on-load+§9.5 contract asserts (JSObject.h:701,913,1141); §9.5 accessors; locationForOffset dispatch (M7(d) fence+I33 bound); quickly-family+length() dispatch; M5 structure()-only nuke masking, structureID() raw (JSCellInlines.h); audit direct m_butterfly touches; stamp currentButterflyTID() at installs; N3 CAS-from-0; PA carve-outs (I36).
3. Structure WSs+transition TID: §9.4 members; fire functions assert butterflyWorldIsStopped; F3 wiring (GT#8 non-sites excluded); flatten-under-stop; E1-E4 predicates incl. N1+!isPreciseAllocation; I29 helper.
3b. SAL emission (we OWN it; vmstate §5.3/N5, §6): `SharedVMState::StructureAllocationLocker locker { vm };` (+locker.deferralContext() where accepted) at every ID-creating Structure::create / allocating transition-table insertion in [SAL] files (§5); heap L5; vmstate M7 audits only. 3c. L6 routing (I37): Concurrently lookups+m_lock steal/walk/mutation conversions ([SAL] files; O1 DeferGC on materialize).
4. §4.1 spine/fragment types in Butterfly.h/ButterflyInlines.h: address equations as inlines, debug-checked vs flat (I8); C1/C3/C4 asserts.
5. §4.2 conversion in ConcurrentButterfly.cpp: step-0 firing, RESTART, slice-aliasing, base/size recording, nuke+DCAS publish; §10.6 stub+witness flag.
6. §4.3 protocol, taxonomy (a)-(d), RESTART; structureOnlyTransition (N2); put/getDirectConcurrent, M7 conformant. 6b. GC visit (§4.5): visitSegmentedButterfly, segmented branch in visitButterflyImpl, barrier audit (I25).
7. Foreign writes (ensureSharedWriteBit): fire-then-DCAS (I12), §3.0 merge loop, R-DOUBLE rebox (§4.7), CoW materialize-first (I35), PA locked flip (I36).
8. §4.4 array CAS: GT10 sites+butterfly-replacing JSObject.cpp paths->casButterfly, caller re-dispatch (SW flip mid-resize->T2, never re-copy); T5 cell-locked growth; I27/I17 asserts; §4.6 AS all-access locking+AS-COPY+stops (I31).
9. §6 dictionary mode+quarantine: cell-lock serialization, flag-gated split+stamps, hook adapter (§10.4c), lazy promotion per §6 lock-context (I18/I19), D1 jsUndefined() store (I30); I34 poll-free audit of owned offset-deref windows.
10. Stress modes: §9.6 options; every §8 invariant gets >=1 targeted assertion.
11. INTEGRATE-objectmodel.md per §10 incl. §10.7 guard list.
12. Tests (JSTests/threads/objectmodel/, i03-*.js): single-threaded no-change (I22); GIL-stub race suites targeting I7/I8/I9/I10/I16/I18/I21/I25/I28/I31/I33/I35/I36/I37 (I28=shared-Double, I37=same-shape add-storm races), named suites=annex §15.T12 (NBR); C++ checks (encode/decode, DCAS lock-freedom+PA alignment I32/I36, §3.0 merge loop)=ConcurrentButterfly.cpp self-tests behind verifyConcurrentButterfly, driven by i03-selftest.js. STW suites run vs the §10.6 stub pre-M4; re-run unmodified at M4/CS2.

13. (post-GIL, chartered-owned w/ api) GC-time TID rebias/reissue per 8c.
14. (post-GIL, chartered-owned) per-thread structure splitting per 8h.

Order: 1-4 (incl. 3b/3c) dark; 5-9 behavior only flag-on/stress; 10-12 close the loop; 13-14 post-GIL charters.
