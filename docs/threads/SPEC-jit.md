# SPEC-jit - JIT tiers, ICs, watchpoints under N mutators (FROZEN, rev 12)

Shared-memory `Thread()`, JIT layer. Design of record: `THREAD.md` (top). Cites: in-tree=file:line, cross-spec=anchors. Normative annex: `SPEC-jit-annex.md` ("annex": App. G/G6/5.6/R1/R5); history=`SPEC-jit-history.md`, NON-NORMATIVE (r4: history §20). Siblings (on-disk revs, authoritative): objectmodel ("OM") r14, heap r13, vmstate r13, api r14; on conflict heap+OM win. "The flag"=`Options::useJSThreads()`; VMM=`VMManager`, NVS=`notifyVMStop`, GCL=`Heap::m_gcConductorLock` (heap rank 2), STWR=`JSThreadsSafepoint::stopTheWorldAndRun`, AS=ArrayStorage/SlowPut shape.

Owned (ONLY editable; new files OK): `Source/JavaScriptCore/{jit,dfg,ftl,bytecode,llint}/**`, `JSTests/threads/jit/**`, `docs/threads/INTEGRATE-jit.md`, plus ADDITIVE-ONLY R5 emitters in `assembler/{X86Assembler.h,MacroAssemblerX86_64.h,ARM64Assembler.h,MacroAssemblerARM64.h}` (R5's list only). NOT touchable (=>§10 manifest): `runtime/{OptionsList.h,Options.cpp,VM.h/.cpp,JSGlobalObject.*,VMManager.*,JSCConfig.h}`, `Sources.txt`, `CMakeLists.txt`.

## 1. Ground truth

G1-G12: annex App. G (FROZEN NORMATIVE); restated at point of use.

G13 (**heap dispositions, normative**). GC NEVER enters VMM's reason latch/dispatch (heap §13.5c; `VMManager.cpp:461-463` assert stays); N1. JSThreads stops hold GCL the whole stopped window (CS2), NEVER `bumpAndReclaim` (heap I11, §13.10a/b).

## 2. Deviations+Notes

* **D1** `useHandlerICInFTL` force-disabled as incomplete, not just off (G4; Task 2). **D2** LLInt has no Handler ICs, only metadata caches (G9; §4.3/§5.4). **D3** Baseline IC state=mutable multi-field data (§4.2/I6). **D4** DirectCall linking still patches code (G10; §5.8). **D5** 64-bit profiles not lock-guarded (G8); tolerance=word-atomicity (§5.7).
* **D6** TTL sets can't prove tag bits zero off-main-thread (vs THREAD.md:15): elided reads keep the mask (foldable `and`); unmasked emission forbidden (I14).
* **D7** §4.2/§4.3/§5.8 repacks are UNCONDITIONAL layout changes; the flag gates only publication/predicates/disables; I1 scoped accordingly.
* **D8** R5 TLS per-platform=annex App. R5; others unsupported flag-on.
* **D9** THREAD.md:15's full TID/SW write elision is UNSOUND (history §17): writes NEVER elide the fused TID compare (§5.5); mask-only E3=READS only; residual cost=one cmp/branch. OM correction=CS5.
* **N1** GC-parked mutators park/exit via NVS (heap §13.5a/b); R1.d stands.
* **N2-N5**: history §16-§19 (N4 incl. OM-8b); folded into §§4-10.
* **N6 (r11)** heap-symbol shims (§4.4/R1.i): compile dark until heap lands; heap-before-Task-13 ordering in `INTEGRATE-jit.md`.

## 3. Scope

In (THREAD.md:17): (1) handler-IC publish/reset under N executors; (2) FTL handler ICs completed, code-patching property ICs retired; (3) code-patching call linking retired; (4) TID/SW checks all tiers (TTL elision reads-only, D9; mask kept; AS regime-3 dispatch §5.5); (5) epoch+scan reclamation of jettisoned code/stubs/handlers; (6) N-safe watchpoint fires; (7) racy profiling tolerated. Out (consumed via §9): tagging/segmentation, TTL storage, allocators, N-stack scan, stop machinery, epoch storage, atom table, `Thread()`, `VMLite`. Flag-gated (api §M; M1; CS1); off=>today (I1/D7).

## 4. Layouts

### 4.1 `InlineCacheHandler` chain

No layout change. Node fields **frozen at publish**; only refcounts+trailing call-link data (§5.8) mutable. `m_next` set once at prepend (`PropertyInlineCache.cpp:960`), immutable until reset. `resetStubAsJumpInAccess` (`:973-988`) installs a fresh slow-path-only head; old chain retired via §4.4, never inline; `removeOwner()` stays; final deref deferred (I9).

### 4.2 `PropertyInlineCache` inlined fast-path fields

Today's independent fields (`PropertyInlineCache.h:421-422`): compare-then-load unsound, ARM64 (F2).
* **Single-word pair**: `{byIdSelfOffset (i32), m_inlineAccessBaseStructureID}`=one **8B-aligned** unit: `union { struct { PropertyOffset byIdSelfOffset; WriteBarrierStructureID m_inlineAccessBaseStructureID; }; std::atomic<uint64_t> m_packedSelfWord; }` (+`static_assert`s); repack UNCONDITIONAL (D7); flag-off keeps per-field accesses (same offsets). Flag on: via `m_packedSelfWord` only - readers ONE relaxed 64-bit load (compare id half, use offset half); writers (under `m_lock`) one 64-bit store; no torn pairs/reader fences.
* **GC write barrier preserved**: `m_inlineAccessBaseStructureID` stays `WriteBarrierStructureID` (`PropertyInlineCache.cpp:894-910`). Flag-on publish=build word->`m_packedSelfWord.store`->**`vm.writeBarrier(codeBlock)`**. All-zero invalidation barrier-free (=`.clear()`, `:278,927`); `visitAggregate` reads the id half as today.
* **Holder-bearing inlined forms disabled flag-on** (`m_inlineHolder` can't pack): never installed via `setInlinedHandler` (asserts holder-free self-access); dispatch via chain (F2).
* Invalidation: one 64-bit store `{0, StructureID()}`; ABA-safe.

### 4.3 LLInt metadata caches (frozen)

Survivors=`alignas(8)` POD `LLIntCachedIdAndOffset { StructureID structureID; int32_t offset; }`=one aligned u64 (G9; per-op `static_assert`, I13). Repack UNCONDITIONAL (D7; size deltas at Task 6); §5.4 gate controls publication/disables/predicates. Flag-on writers: one-word store, no lock (last-writer-wins); asm readers: one 64-bit load (compare id half, use offset half), unfenced. Survives IFF threaded form is exactly one such word; wider=>disabled (slow path, never caches). Inventory:

| Op (today: App. G/G9) | Frozen threaded form |
|---|---|
| `get_by_id` (16B union, `GetByIdMetadata.h:41-75`) | Word 1->atomic u64; **only `Default`+`ArrayLength` published**; **`setupGetByIdPrototypeCache` (`LLIntSlowPaths.cpp:839-913`) disabled wholesale** (sole ProtoLoad/Unset installer) |
| `try_get_by_id` (`:761-768`, 8B) | survives, repacked u64 |
| `get_by_id_direct` (`:810-817`, 8B) | survives, repacked u64 |
| `instanceof` (rb:239); `iterator_open_get_next`/`iterator_next_*` (`:988-1046`) | Default/ArrayLength only |
| `put_by_id` (24B, BytecodeList.rb:271; `:1151-1192`) | **transition cache disabled** (fields null forever; asm branch dead); replace cache survives as u64 |
| `get_private_name` (`:1327-1334`) | **disabled** (cell compare must cohere) |
| `put_private_name` (`:1459-1496`) | **disabled** |
| `set_private_brand`/`check_private_brand` (`:1532-1545`) | **disabled** |

**Mode coherence.** Asm dispatches on the mode byte FIRST, then word 1 (`LowLevelInterpreter64.asm:1649-1693`); not coherently readable; `Unset` poison (history §13.3)=>only `Default` (word 1=publish/all-zero invalidation) and `ArrayLength` (never touches word 1; self-validates via indexing bits) publish. Mode-byte writes=1B relaxed stores; debug asserts keep `setUnsetMode`/`setProtoLoadMode` unreachable (I18). I13 lint: every `metadata.m_*structureID*=` write in `llint/` is in this table or flag-off-only. Charter: proto/transition caches return as immutable single-ptr records (§5.8 pattern); REQUIRED pre-ship if Task-13's budget is missed.

### 4.4 Retired-artifact reclamation

Epoch facility heap-owned (heap §11); one domain per shared heap. Owned stateless adapter (DATA ONLY; forwards to `safepointEpoch().retire()`): `class RetiredJITArtifacts { static void retireHandlerChain(JSC::Heap&, RefPtr<InlineCacheHandler>&& head); static void retire(JSC::Heap&, std::unique_ptr<RetiredCallback>&&); };` Shim (N6): bodies compile iff `__has_include("GCSafepointEpoch.h")`; else no-op leak-until-INT stub (sound: GIL stub=>no concurrent retirement).

**Hard rule: epoch expiry frees only non-executable data whose every JIT-side access is in a safepoint-free window** (handler nodes, IC metadata; G2) - never machine code: expired chains drop their `Ref<GCAwareJITStubRoutine>`s into jettison machinery (code deletion waits on R2); `retireHandlerChain` RELEASE_ASSERTs `isGCAware`.

Obligations: **(a)** JIT'd code holds no retired-data pointer across a safepoint: cooperative stops (R1.f); no poll in an IC fast-path/handler-field window (G2; I16); **(b)** native slow paths holding a handler-allocation pointer across a possible safepoint take `Ref<InlineCacheHandler>` first (I15). Task 3 audits. **Cadence=G13/CS4; heap also reclaims at legacy collection end (heap §11)=>phase-1 frees retired items.**

### 4.5 Atomic refcounts

Plain today (G11), mutated cross-thread under §4.4(b)/§5.8. `InlineCacheHandler` (+`WithJSCall`)->`ThreadSafeRefCounted`; `JITStubRoutine::m_refCount`->`std::atomic<unsigned>` (relaxed inc, release dec, acquire RMW pre-`observeZeroRefCount()`; covers subclasses). **Unconditionally atomic** (C++ state; I1 unaffected). Task 3 audits shared counters/lists reachable through these into I17's table (`addOwner`/`removeOwner` stay under `m_lock`).

## 5. Protocols

### 5.1 Handler-IC mutation

Writers (`addAccessCase`, `InlineCacheCompiler`): unchanged locking (`m_lock` via `GCSafeConcurrentJSLocker`). New-head publish: `storeStoreFence()` before `m_handler=WTF::move(handler)` (`PropertyInlineCache.cpp:961`); readers address-depend through the head (F2). Reset: replace head with slow-path handler (fenced), then `retireHandlerChain(heap, oldHead)`; never inline frees. Jettison-time IC `deref()` (`CodeBlock.cpp:2320-2327`) same.

### 5.2 FTL handler ICs

* Complete the lowering (D1). Acceptance: `--useHandlerICInFTL=1` honored, full JSTests+stress single-threaded; FTL never allocates `RepatchingPropertyInlineCache` (`FTLState.cpp:172-179`).
* **Gate**: `Options.cpp:814` non-touchable=>**M2a=prep precondition (§10)**, inert, defaults false. Slip-hatch: Task 2 may smoke-test via a TEMPORARY owned env-var hook, REMOVED at handoff - MUST run BEFORE `Config::finalize` or pair with `Config::disableFreezingForTesting()`. M2b at handoff.
* Flag on: `useHandlerICInFTL` implied true; `RepatchingPropertyInlineCache` construction=release assert (I3); `rewireStubAsJumpInAccess`'s `replaceWithJump` (`:965-971`) unreachable.
* `DFGStrengthReductionPhase.cpp:1758` DirectCall bailout stays; DFG DirectCall=§5.8.

### 5.3 Jettison+invalidation

`CodeBlock::jettison` (G5) runs only (a) on the JSThreads conductor inside an STWR closure (R1), or (b) during GC, world stopped; ditto `invalidateLinkedCode`+every `JumpReplacement::fire` (I8); enforce `RELEASE_ASSERT(!Options::useJSThreads() || JSThreadsSafepoint::worldIsStopped(vm))`. Cross-modifying-code flush by patcher+per-mutator ISB on resume (F5/R1.d). `ExecutableMemoryHandle`s released ONLY on the GC sweep after R2's scan; handler data retired at jettison->`RetiredJITArtifacts` (I9). Poll/trap rule: I21.

### 5.4 LLInt metadata+flag gate

Reads/writes per §4.3 (single u64, no fences; stale values fail the id compare). **Gate**: interpreter compiles once=>runtime gating: **M4a (PREP-PHASE)** adds `uint8_t useJSThreads` to `JSCConfig` (set at options-finalize); offlineasm `ifJSThreadsBranch(label)`=one `_g_config` byte-load+branch, ONCE per affected fast path (§4.3/§5.5 hang off it; §10). Flag-off cost=one not-taken branch per opcode; `--useJIT=0` bench gate (Task 13).

### 5.5 TID/SW checks per tier

Consumes R3 (OM §9)+R5. OM §2: segmented<=>`(tagged >> 48) == 0xFFFF` (top-16 compare total).

**AS-rule (=OM manifest 8/§4.6 AS-COPY/I31).** AS never segments; flag-on, AS relayout (shift/unshift `JSArray.cpp:1650,1818`; any vector move/indexBias/vectorLength change)=copy-on-write under the cell lock: fresh AS butterfly, `casButterfly`; superseded storage never rewritten; installed vectorLength immutable. I31 any-SW cell-locking governs SLOW paths (R3 locked ops at any SW). A GENERATED AS fast path is legal only via (a) E2 elision (fire=>§5.3 jettison; writes keep the TID compare, D9); (b) SW-bit test, SW=1->R3 locked ops; (c) excluding AS modes; watchpoint-invalid compiles MUST use (b)/(c). Residual unlocked fast accesses sound (proof history §17): SW=0 READS any thread (AS-COPY snapshots); SW=0 WRITES owner-only (never-elided TID+SW compare; I16 window; OM §4.6 per-event STW). Generic paths (LLInt array paths, generic ICs): the SW branch also loads the indexing byte; SW=1∧AS->locked slow path (I20).

**Frozen predicates** (OM §3+AS-rule):
* **Read**: load tagged; top16==0xFFFF->segmented read (dependent load through the spine; R3 op or inline); SW=1∧AS->locked slow path (absent under (a)/(c)); else mask, proceed as today. No TID check.
* **Write**: load tagged; (1) top16==0xFFFF->segmented write; (2) `(tagged & butterflyTagMask)==<currentButterflyTIDTag>`->mask+store (owner; fused cmp/branch; R5 tag, hoistable); (3) `tagged & butterflySWBit`∧NOT AS->mask+store (AS->locked slow path; test absent under (a)/(c)); (4) else: AS-shaped (indexing byte per generic-path rule; shape-specialized ICs know statically)->tail-call the locked R3 op (`operationSharedArrayStorage*`), which fires F1, flips SW and performs the write ITSELF under the cell lock/OM §4.6 regime - NEVER `ensureSharedWriteBit`-then-store inline (breaks I20/OM I31); non-AS->`ensureSharedWriteBit(vm, object)`, then store. ICs may reorder (2)/(3).
* **Transition** (= OM E4 EXACTLY incl. its PA exclusion; D9; r10=OM r12 per-object keying): legal iff (compile-time) BOTH source TTL sets valid+watched (registered; fire=>jettison) AND (runtime) `!isPreciseAllocation(cell)` (PA=8-mod-16 base, `PreciseAllocation.h:68-70`; OM I36: no E4 on PA) AND, butterfly-bearing, `(tagged & butterflyTagMask)==g_jscButterflyTIDTag` ((currentTID,0)=instance owner; no structure-TID compare: foreign-thread shape reuse stays lock-free, OM N1); butterfly-less (N1/N2): PA test+`g_jscButterflyTIDTag == uint64_t(source->transitionThreadLocalTID()) << 48` are the sole runtime checks; else R3 slow paths. Emission: PA bit-test `cell & 8`->R3 if set (alt: compile-time MarkedBlock-provenance speculation+OSR/slow fallback); load the R5 tag; butterfly-bearing: compare tag bits vs the R5 tag; butterfly-less: compare R5 tag vs `tid << 48` immediate when specialized on S, else vs `Structure::m_transitionThreadLocalTID`<<48; the JIT never implements transition semantics.

**Structure->butterfly ordering (R7=OM M7; F7).** Fast paths that structure-check then load the butterfly SEPARATELY (incl. elided E1+E2) order the loads: address dependency structureID->butterfly (`Dependency::consume`, `JSObject.cpp:385-405`; no-op x86-64). FTL `B3::Depend`; DFG/Baseline/stubs eor+add; LLInt same, ARM64-only; C++ `WTF::Dependency`. Checked by I14.

Per tier: LLInt+Baseline/stubs - full predicates, NO elision (asm fast paths; IC guards+thunks regenerated per-flag); DFG - `GetButterfly` (`DFGSpeculativeJIT.cpp:11311`), `PutByOffset`/array paths (E1/E2); FTL - `compileGetButterfly` (`FTLLowerDFGToB3.cpp:5823`)+property patchpoints, after Task 2 (E1/E2).

**Choke-point rule (I14 outside DFG/B3 IR).** Flag on, EVERY generated butterfly access uses a per-tier choke point: LLInt macros `loadButterflyForRead/ForWrite` (the ONLY `m_butterfly` offset uses in `llint/*.asm` outside flag-off paths); Baseline/stubs `CCallHelpers::loadButterflyForRead/ForWrite` in every IC/property/thunk emitter; DFG/FTL via `GetButterfly` lowering+patchpoints. Grep lint (Task 8): no raw offset load outside choke points; site inventory in `INTEGRATE-jit.md`.

Elision (=OM E1-E3, D9/CS5-corrected) via `DFGDesiredWatchpoints` on R3 sets: E1 omit segmented-dispatch check (reads+writes) iff every speculated structure's `transitionThreadLocal` valid+watched; E2 (writes) omit ONLY the SW branch (3)+AS SW test iff `writeThreadLocal` likewise - predicate (2)'s fused TID compare+case-(4) fallback ALWAYS emitted, every tier incl. E1+E2 DFG/FTL (sole F1 detection point; D9); E3 (load+mask+access) READS only (+F7 ARM64); mask ALWAYS emitted unless the IC proves the tag constant zero (I14, D6). Set fire=>§5.3 jettison. Inline (cell) properties never checked/masked. Flag off: nothing emitted (I1).

### 5.6 Watchpoint fires (owned)

Fire sites span `runtime/**` (G6)=>intercept INSIDE `WatchpointSet::fireAllSlow`/`InlineWatchpointSet::fireAll` (owned `bytecode/Watchpoint.{h,cpp}`); no `runtime/**` call-site edits but M6.
* **Classification**: new bit `m_invalidatesCode` (Class A), default A; owned constructors opt data-only sets into B; `FireDetail` override for rare sites.
* **Class A** (in `fireAllSlow`, flag on - ALWAYS; no >1-mutator gate, G7/I10): (1) `worldIsStopped(vm)`->fire inline; (2) else `STWR(vm, closure)` (lock-free callers only); (3) re-check `state()==IsWatched` (I11); (4) existing fire body; (5) jettisons (§5.3) in the same closure; (6) resume->fire COMPLETE. Synchronous completion load-bearing (history §13.5). **Coalescing (REQUIRED, r10):** R1.g's job slot queues concurrent fires; the winner drains ALL queued fires in ONE stop (losers' STWR returns once run); inline-fire count gate stays REFUSED (G7).
* `worldIsStopped(VM&)`=VMM `Mode::Stopped` OR `worldIsStoppedForAllClients()` (heap §9) OR legacy `vm.heap.worldIsStopped()` (`Heap.h:386`; true through End) OR (pre-M4 ONLY) OM's stub witness `g_jsThreadsStubWorldStopped` (OM manifest 6; deleted at M4, CS6). (Disjuncts 3/4: legacy-GC `finalizeUnconditionally`/`visitWeak`+pre-M4 stub fires take branch 1; heap §13.10b; debug-assert.) TTL fires assert branch 1.
* **Deferral**: deferred-overload fires (`Watchpoint.h:493-508`); the scope-exit fire (lock-free by construction) runs steps 2-6. **AMENDED (B-relabelrace, history §22)**: flag-on the deferred overload's entry is NOT owner-serialized for every caller (inline original-array `nonPropertyTransition` fires it lock-free; `firePropertyReplacementWatchpointSet` pre-checks `IsWatched` lock-free), so the claim is a CAS `IsWatched`->`IsInvalidated` BEFORE the membership transfer: exactly one racer transfers (splice still under the membership lock); losers return with their deferred set `ClearWatchpoint` (no scope-exit fire — benign, the winner's fire supersedes) and ASSERT the failed CAS's prior state is `IsInvalidated` (states monotonic, so the legitimate race is assert-clean; any other prior — e.g. `ClearWatchpoint` from a precheck-bypassing direct caller — traps exactly as the former entry assert did). `take()` therefore installs `IsWatched` into the deferred set explicitly (the source's pre-claim state), and `takeWatchpointsToFire`'s entry invariant is re-scoped to "source `IsInvalidated`-by-claimant" flag-on / `IsWatched` flag-off (each arm asserts the one state its protocol permits). Flag-off sequence unchanged (assert + take + flip). DIRECT `fireAll`/`fireAllSlow` callers REQUIRED lock-free w.r.t. every §7/cell lock; Task 11 buckets each: lock-free/world-stopped/lock-holding->**M6** (expected empty; table in PR). STWR debug watchdog RELEASE_ASSERTs stop progress, naming the escaped set.

### 5.7 Racy profiling (D5)

All "tolerate, don't synchronize" except tier-up:
1. Execution counters (G8): relaxed atomic adds from C++; JIT'd fast-path adds may stay plain.
2. **Tier-up CAS**: per-CodeBlock `std::atomic<uint8_t> m_tierUpInFlight` per tier-up edge; threshold slow paths (`operationOptimize`; LLInt tier-up; owned DFG->FTL triggers) enqueue only after a 0->1 CAS win, cleared on complete/cancel; losers defer, stay in tier.
3. **Worklist dedup backstop**: `JITWorklist::enqueue` replaces the `:187` assert: key present under `*m_lock`->cancel, `CompilationDeferred` (not flag-gated).
4. `ValueProfile` buckets: aligned 64-bit stores word-atomic, never torn; guards validate (I12); TSAN annotations.
5. `ArrayProfile`-style flag merges: relaxed atomic OR where compiler-read; lost bit benign (I12).
6. Multi-word Status snapshots stay under `ConcurrentJSLocker` on `m_lock` (G8); IC-state writers also hold `m_lock` (§5.1); Task 12 audits every `computeFor*` entry (verify only).
7. A datum stays plain iff <=8B word-aligned AND advisory to every consumer; multi-word reads under `m_lock`.

### 5.8 Call linking (D4)

Guard/payload word-pair protocols unsound (history §13.2); fast-path reads flow through ONE published pointer:

```cpp
struct CallLinkRecord {  // bytecode/CallLinkInfo.h (owned); immutable after publish
    uintptr_t comparand;  // callee cell, or sentinel: bit 0 (polymorphicCalleeMask)=always-call
    CodePtr<JSEntryPtrTag> target;  // entrypoint (monomorphic/virtual/stub/direct)
    CodeBlock* codeBlockToTransfer; // stored to the callee frame by the fast path
};
```

* **Fast path (frozen, all tiers/flavors flag-on)**: `load r=m_record; if (!r) goto slow; load c=r->comparand; if (c == calleeGPR || (c & polymorphicCalleeMask)) { store r->codeBlockToTransfer->callee frame; load t=r->target ONCE; call t; } else goto slow`. c cell=monomorphic; c sentinel=virtual/always-call+stub dispatch (today's bit-test, G10); direct calls skip the comparand check; all reads THROUGH `r` (F2).
* **Placement (frozen)**: `DataOnlyCallLinkInfo` (`CallLinkInfo.h:320`; LLInt/Baseline bytecode metadata=>+8B per call-op, unconditional, D7), `OptimizingCallLinkInfo` (`:437`), `DirectCallLinkInfo` (`:343`) each gain `m_record` as LAST member; records heap-alloc'd at link, freed via §4.4. Legacy mirrors (`m_callee`/`m_codeBlock`/`m_monomorphicCallDestination`; Direct: `m_target`/`m_codeBlock`) stay in sync under existing locks, read by GC+slow paths as today.
* **Writers** (=F6): every transition (first link; monomorphic upgrade - the `CallLinkInfo.cpp:106-115` in-place rewrite becomes a publish; `setVirtualCall`/`setStub`/`setCallTarget`) publishes a NEW record, never mutates; stale read=complete OLD record, benign.
* **GC**: `comparand`=RAW word, never dereferenced/visited, no `WriteBarrier`. Legacy mirror=sole GC root/weak ref; `visitWeak`/`unlinkOrUpgrade` read it as today plus null `m_record` on clear/relink. Always-call records unlinked under STW/GC only.
* **Unlink** (`reset`, `visitWeak`-driven): single `m_record=nullptr` store; legal from a running slow path (monotone) or under STW. Flag on: no JIT'd fast path reads legacy fields; off: no records, fast paths unchanged (I1).
* **Retirement**: replaced/unlinked records->`RetiredJITArtifacts::retire` (pure data; entrypoints kept alive via stub refs/CodeBlock ownership, freed via §4.4/R2); stale-record safety=I16.
* **DirectCallLinkInfo: data IC only.** The three `UseDataIC::No` sites (G10) pass `UseDataIC::Yes` under the flag; fast paths re-emitted in record form. `repatchSpeculatively` forbidden (`RELEASE_ASSERT(!Options::useJSThreads())` on `!isDataIC()` branches). **Polymorphic stubs**: `setStub`/`revertCallToStub` publish a record targeting the stub; routine GC-aware, ref'd by the owner (§4.5).

## 6. Fences

F1. Every publish making a handler node/chain JIT-reachable: `WTF::storeStoreFence()` after payload init, before the publishing store (`Watchpoint.cpp:133` pattern).
F2. JIT'd readers of handler chains: address-dependent loads only (load head, load *through* it). Compare/branch does NOT order ARM64 loads; multi-field state without an address dependency=one aligned <=8B load (§4.2/§4.3) or pointer publish.
F3. Single-word publication (§4.2/§4.3): one aligned 64-bit store; invalid=all-zero; no multi-store seqlocks in JIT-readable state.
F4. `WatchpointSet::m_state` keeps its fence pair (`Watchpoint.cpp:133,136`); Class-A fires also ride the stop entry/exit barrier.
F5. Code patching (§5.3): data writes->patcher's cross-modifying-code flush->world resume->per-mutator ISB (R1.d) before re-entering JIT code. NVS-exit hook covers JSThreads AND GC stops (N1); M4's fence FIRST, then heap's `gcDidResumeFromStopTheWorld` (heap §10.9, 5a).
F6. Call-link publish: init immutable record->`storeStoreFence`->single `m_record` store (§5.8); readers address-dependent (F2); `target` loaded once, called via register; unlink=single null store.
F7. =§5.5 R7 (ARM64 address dependency structureID->butterfly; no-op x86-64); checked by I14.

## 7. Lock ordering (deadlock=bug)

Outermost->innermost; holding L, acquire only strictly rightward. STWR holding ANY lock below the GCL row forbidden (§5.6). `requestStopAll*` takes only its `m_worldLock` briefly.

```
[Heap GCL (rank 2) - ONLY inside STWR via R1.i; requester releases heap access first]
 > [R1/VMM world-stop ownership (STWR)]
  > JIT worklist/Plan locks (jit/JITWorklist.{h,cpp}; no DFGWorklist)
   > CodeBlock::m_lock (bytecode/CodeBlock.h:813)
    > PropertyInlineCache state (guarded by CodeBlock::m_lock)
    > Structure/cell 2-bit lock (runtime/IndexingType.h:97-98)
     > heap GCSafepointEpoch retire lock (leaf; rank-10 cell/Structure holders OK,
       heap ranks 7-9 NEVER - heap §6/§9; not signal-safe)
     > JITStubRoutineSet/ExecutableAllocator locks (leaf)
```

`GCSafeConcurrentJSLocker` keeps its GC-yield; no other jit-owned code touches GCL.

## 8. Invariants

I1. Flag off=>emitted instruction sequences identical to today MODULO field-offset immediates moved by the unconditional repacks (D7; shapes/counts identical); IC object types+option defaults identical; LLInt differs only by §5.4's gate branch (Task 13).
I2. No tier modifies reachable machine code while >1 mutator may execute JS, except inside R1 STW; RELEASE_ASSERTs at each patching site (`invalidateLinkedCode`, `JumpReplacement::fire`, `rewireStubAsJumpInAccess`, §5.8 DirectCall).
I3. Flag on: no `RepatchingPropertyInlineCache` constructed; no `DirectCallLinkInfo` with `UseDataIC::No`.
I4. A published `InlineCacheHandler` node's payload never changes (debug checksum at publish/retire); sole mutable trailing word=`m_record` (§5.8), pointees immutable.
I5. Every handler-chain publish preceded by `storeStoreFence` (F1); debug counter pairs fences/publishes.
I6. Inlined fast-path state never observable as valid structure id+mismatched offset (structural, §4.2); stress: flip an IC between two structures under readers.
I7. Machine code freed only after R2's scan, never by epoch expiry alone; handler DATA nodes freed only at heap epoch >= retire+1 AND refcount zero (§4.4).
I8. Flag on, `CodeBlock::jettison` with `reason!=JettisonDueToOldAge` runs only world-stopped (asserted).
I9. Flag on, `resetStubAsJumpInAccess` never frees chain nodes inline; routed via `RetiredJITArtifacts`.
I10. Every `WatchpointSet` classified A or B at construction (default A); Class-A fires world-stopped; table in PR; constructor lint.
I11. Fires idempotent: already-invalidated set=no-op (`state()` check; re-check after stop).
I12. No profiling datum read by a compiler thread causes unsoundness for any torn/stale value: profiles select, guards validate.
I13. Flag-on LLInt caches=single aligned word (`alignas(8)`+`static_assert`s, §4.3) or disabled; §4.3 grep lint.
I14. Every butterfly dereference in generated code (a) masks the tag (always, even elided; E3), (b) is proven tag-zero by the IC, or (c) is inline-cell. Verify: `validateButterflyTagDiscipline` (DFG/B3); choke-point lint (LLInt/Baseline); Task 13.
I15. Native slow paths holding handler-allocation pointers across potential safepoints take `Ref<InlineCacheHandler>` (§4.4b; Task 3 list).
I16. No safepoint poll between an IC fast-path's head/state load and its last dependent use, incl. `m_record` load->call (§4.4a; I14 pass; choke points poll-free+lint).
I17. Cross-thread refcounts atomic per §4.5; Task 3 tabulates shared counters/lists+guards.
I18. Flag on, only observable LLInt `GetByIdModeMetadata` modes=`Default`/`ArrayLength` (§4.3); setter asserts+I13 lint.
I19. `g_jscButterflyTIDTag` initialized before any JS on a thread (CS3): VM-entry debug RELEASE_ASSERT it equals `uint64_t(currentButterflyTID()) << 48`; 3-thread test (Task 1b).
I20 (mirrors OM I31). Flag on, no generated code makes an unlocked butterfly access reachable by an SW=1 AS butterfly: every AS fast path satisfies AS-rule (a)/(b)/(c). Checked by the I14 pass (AS modes carry E2 registration or an SW test)+choke-point lint; Task 13.
I21. Flag on: polling traps ONLY (`usePollingTraps` forced, M2b; async breakpoint patching=I2 violation); every DFG/FTL poll is immediately followed by an invalidation point (CheckTraps emits one)=>parked mutators resume into the patched exit, never across jettisoned elided code; Task-13 lint extends I16 to poll windows.

## 9. Interfaces

### 9.1 Provides

P1. `bytecode/RetiredJITArtifacts.h` (§4.4): data retirement adapter over the heap epoch; stateless; any thread; rank-10 cell/Structure locks may be held, heap ranks 7-9 NOT (heap §9 contract; §7).
P2. Central Class-A fire safety in `fireAllSlow` (§5.6): any `fireAll`/`invalidate` gets STW semantics when the set invalidates code; non-owned sets default A.
P3. Flag on: no machine code patched outside STW (I2)=>code metadata readable at safepoints without locks.
P4. Per-tier threaded support switchable via M1 flags.
P5. `JSC::initializeButterflyTIDTagForCurrentThread()`/`clear...()` exported from `jit/ConcurrentButterflyOperations.{h,cpp}` for CS3 callers (attach/detach); P5 init also registers the CS3 tag-update body via `setVMLiteTIDTagHook`.

### 9.2 Requires (frozen)

R1. **Safepoint=VMM STW with a JSThreads reason, requester-as-conductor** (G7); ONE primitive over VMM+M4, consumed by §5.3/§5.6:

```cpp
// bytecode/JSThreadsSafepoint.h (owned), veneer over M4. STWR: release heap access, GCL
// bracket (R1.i), stop, run `work` ON THE CALLER'S OWN STACK, resume, release, re-acquire.
// Caller: entered mutator, NO §7/cell lock (§5.6).
namespace JSThreadsSafepoint {
    void stopTheWorldAndRun(VM&, const ScopedLambda<void()>& work);
    bool worldIsStopped(VM&); // four disjuncts: §5.6
}
```

Mechanics (M4+owned veneer; full prose annex App. R1, superseded by c/h/i):
a-b. stop reason `v(JSThreads)`+NVS dispatch->new `JSC_CONFIG_METHOD(jsThreadsStopTheWorld)` slot+`VMManager::setJSThreadsCallback` (`VMManager.h:200-212,272-277`); registered from owned `JSThreadsSafepoint.cpp` at first flagged VM;
c. **requester pinning**: exported `static void VMManager::requestStopAllWithConductor(StopReason, VM*)` stores `m_jsThreadsConductor` under `m_worldLock`, then=`requestStopAll`; arbitration (`VMManager.cpp:413-460`): reason==JSThreads∧all active VMs parked=>`m_targetVM=m_jsThreadsConductor` (NOT the last parker, G7); only the requester runs `work`; `{&vm,&work}` pending-job slot OWNED (`JSThreadsSafepoint.cpp`), cleared with the reason bit;
d. resume hook: ISB on each mutator leaving NVS after a `JSThreads`/`GC` stop (F5/N1);
e. (=M4a) `JSCConfig` `useJSThreads` gate byte (§5.4)+Darwin `butterflyTIDTagTLSKey` slot (R5);
f-g. **cooperative stops only** (park at poll sites, never async; load-bearing for §4.4a); **requester-vs-requester**: owned park-aware mutex on the pending-job slot (App. R1) - a loser PARKS (=stopped) during the winner's stop, then retries;
h. **GC does NOT share this** (G13): GC bit never latched/dispatched; non-GC reasons nest via per-reason bits+one-at-a-time service loop (`:391-411`); a fire reached world-stopped runs inline (§5.6 branch 1);
Freeze scope (r11): VM-counting arbitration=final only for the N-separate-VMs config; N threads in ONE VM (api §5.2, post-GIL)=thread-granular STW (vmstate Dev-10 Phase-B charter, api §2); R1.c re-frozen there.
i. **GC serialization**: STWR brackets its ENTIRE stopped window: release this VM's heap access->`Heap::JSThreadsStopScope` (CS2; over GCL)->stop->resume->release scope->re-acquire (heap §10C). Pre-heap builds gate the bracket on `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` (N6; stub has no bracket). NEVER calls `bumpAndReclaim`. Non-shared heap: no-op. Closures: allocation-free (OM O4; pre-allocate first); heap-metadata WRITES without access OK (heap §10A exemption).
R2. **Conservative scan covers all N mutator stacks** (incl. parked threads' registers, heap §10.2/§10.4) before `deleteUnmarkedJettisonedStubRoutines` (G1)+CodeBlock sweep; gates ALL machine-code frees (I7).
R3. **Butterfly thread tag**=OM §9 (`runtime/ConcurrentButterfly.h`), adopted by name, unmodified. **JIT-operation wrappers** (`operationSegmentedButterflyLoad/Store`, `operationButterflyEnsureSharedWrite`, `operationSharedArrayStorage*` locked AS ops, array variants)=thin `JSC_DEFINE_JIT_OPERATION` shims in OWNED `jit/ConcurrentButterflyOperations.{h,cpp}`. Elision contract: E1-E3 D9-corrected+I12/I14/I15 (mask kept, D6).
R4. **Epoch facility**: `heap/GCSafepointEpoch.h` via `heap.safepointEpoch()`; we only adapt (§4.4; bump GC-conductor-only, G13).
R5. **Per-thread tag constant off the thread register (one load in JIT tiers)**. TID provider=vmstate §6.7 `currentButterflyTID()` (D8; Task 1b):
Per-platform mechanics=annex App. R5 (FROZEN NORMATIVE, implemented verbatim; ELF IE-TLS+emitters+LLInt `loadButterflyTIDTag`; Darwin pthread key via the M4a slot; Windows unsupported flag-on).
Value=`uint64_t(currentButterflyTID()) << 48`, written by P5's init+CS3 hook (zero-init correct ONLY for main thread=>CS3 MANDATORY; I19). Emission (WRITE/TRANSITION only; reads never compare TID): LLInt `loadButterflyTIDTag` (annex R5); Baseline/stubs via `InlineCacheCompiler`; DFG/FTL loop-invariant pure load (hoistable).
R6. **Options**: api defines `useJSThreads`; M1 adds JIT-local flags (CS1).
R7. **Structure->butterfly reader ordering**=OM §7 M7 option (a): address dependency. Emission §5.5; F7; I14.

## 10. Manifest (`INTEGRATE-jit.md`; integrator-applied, implementer must NOT edit)+cross-spec

**Prep preconditions (r12)**: orchestrator applies to the shared tree BEFORE fan-out: ALL FIVE specs' OptionsList.h entries (api 9.2-1 canonical for useJSThreads; M1's other flags, vmstate M_opts, heap manifest 2, OM entry 1)+M2a+M4a. Task 1 fails fast (compile-time ref to `Options::useJSThreads()`); absent=>STOP+escalate — local OptionsList patches ABOLISHED across all five specs; non-Options hunks for self-checks: heap-§14-style private overlays, never committed; never a different M4a load shape (golden diffs).

M1. `runtime/OptionsList.h`: `v(Bool, useJSThreads, …)` (dedupe with api's); kill switches `useThreadedLLIntICs`/`...BaselineICs`/`...DFG`/`...FTL` (Bool, true); `validateButterflyTagDiscipline` (Bool, false); `useJSThreadsUnlockHandlerICInFTL` (Bool, false; M2a hatch).
M2. `runtime/Options.cpp`: **M2a (prep, §5.2/§10):** gate the `:814` force-disable on `!useJSThreadsUnlockHandlerICInFTL()`. **M2b (handoff):** `if (Options::useJSThreads()) { Options::useHandlerICInFTL()=true; Options::usePollingTraps()=true; /*I21*/ }`+startup error if `useJSThreads && !useHandlerICInFTL`.
M3. `Sources.txt`+`CMakeLists.txt`: `bytecode/RetiredJITArtifacts.cpp`, `bytecode/JSThreadsSafepoint.cpp`, `jit/ConcurrentButterflyOperations.cpp`.
M4. `runtime/VMManager.h/.cpp`+`runtime/JSCConfig.h` (heap manifest 3-5; NVS resume tail: M4's fence first, then heap's hook, 5a). Items=**exactly R1.a-d** (+optional `VMManager::worldIsStopped()`); R1.g's mutex+pending-job slot OWNED (`JSThreadsSafepoint.{h,cpp}`). **M4a (=R1.e)=prep precondition (header)**: gate byte+finalize store+Darwin TLS-key slot, inert; BEFORE Tasks 6/8 (no interim substitute: load shape=I1/Task-13 golden diffs). **Rest of M4 INTEGRATION-DEFERRED** (OM manifest 6 concurs): Tasks 5/11/13 land against Task 1's interim stub; multi-mutator acceptance moves to the integration gate (Task 13).
M5. `runtime/VM.h`: none (epoch state=heap's, §4.4).
M6. **Audit-driven `runtime/**` deferred-fire conversions** (§5.6): per fire site Task 11 proves lock-holding; expected empty; entries ship file:line+replacement.

Cross-spec (orchestrator routes; owned-path shape unchanged):
CS1. api/OM: alias `useConcurrentJS`<->`useJSThreads` (one option).
CS2. heap: **RESOLVED-AS-PROVIDED** (heap manifest 10b) - GCL bracket=`class Heap::JSThreadsStopScope` (RAII; heap §9/§10C; pre: caller released heap access). R1.i consumes by that exact name; no-op when `!isSharedServer()` (§5.6).
CS3. **MANDATORY** (tag 0=main-thread owner=>skipped `ensureSharedWriteBit`). Mechanism=vmstate §6.7 `JSC::setVMLiteTIDTagHook(void(*)(uint16_t))` (`VMLite.h`; null default): P5 init REGISTERS a body storing `uint64_t(tid) << 48` to the R5 slot (guarded for Phase-A builds w/o VMLite.h); `setCurrent` calls it post-TLS-write with `lite ? lite->tid : 0`=>lazy installs/§6.4.4 multi-VM switches/detach keep the tag coherent (I19). api §5.2's direct P5 spawn/detach calls stay (idempotent).
CS4. **REFUSED for JSThreads stops**: bumps only at heap's two GC-side contexts (heap I11); JSThreads stops enqueue a GC request (13.10a), which reclaims (heap §11, legacy incl.). §4.4 updated.
CS5. OM ADOPTED the D9 write-elision correction (ledger 8d); jit emits soundly regardless (§5.5).
CS6. OM: pre-M4 jit reads OM's `g_jsThreadsStubWorldStopped` witness (§5.6 disjunct 4; default), OR (preferred post-Task-1) OM's §10.6 veneer delegates to STWR; orchestrator picks; INTEGRATE doc records the M4 deletion.

## 11. Ordered task list

1. **Scaffolding.** `bytecode/RetiredJITArtifacts.{h,cpp}`; `bytecode/JSThreadsSafepoint.{h,cpp}` - R1 veneer, **interim stub until M4** (=OM manifest 6): STWR RELEASE_ASSERTs <=1 entered VM (phase-1 GIL), runs `work()` inline; `worldIsStopped(vm)` true inside AND under OM's stub witness (§5.6.4/CS6; swapped to M4+CS2 at INT, disjunct deleted); `jit/ConcurrentButterflyOperations.{h,cpp}` (P5 init/clear+AS locked shims, R3). Wire flag-gated asserts (I2/I3/I8).
1b. **Per-thread tag TLS** (R5/P5/I19): `g_jscButterflyTIDTag`+init/clear exports+CS3 `setVMLiteTIDTagHook` registration+per-platform offset/key setup+constancy assert; per-arch emitters incl. R5's new assembler additions; 3-thread test. Prereq of 8-10.
2. **Complete FTL handler ICs** (D1, §5.2): `ftl/FTLLowerDFGToB3.cpp`/`FTLState.cpp`/`InlineCacheCompiler` until the full suite passes flag-on single-threaded; drop FTL's `rewireStubAsJumpInAccess`.
3. **Handler publish fences+epoch retirement** (§5.1/§4.4): fence publishes; reroute `resetStubAsJumpInAccess`+jettison-time IC `deref()` via `RetiredJITArtifacts`; Ref-ify slow paths (I15).
4. **Inlined fast-path repack** (§4.2, I6): single-load Baseline/DFG readers; holder-bearing forms disabled flag-on.
5. **Jettison under STW** (§5.3): world-stopped gates+F5 barriers; reopt/watchpoint jettisons via STWR closures (incl. R1.i).
6. **LLInt metadata repack/disable** (§4.3/§5.4, I13): frozen table+gate+single-load readers+size deltas+grep lint.
7. **Call-link records** (§5.8): frozen placement+sequence; flip the three `UseDataIC::No` sites; forbid `repatchSpeculatively`; GC-mirror sync; §4.5 refcounts if not yet landed.
8. **TID/SW emission, LLInt+Baseline** (§5.5): choke points+frozen predicates (incl. AS-rule+PA test) in asm fast paths+IC guards+thunks; R3 slow paths; site inventory+lint (I14).
9. **TID/SW+TTL elision, DFG** (§5.5 sites)+`DFGDesiredWatchpoints`; OSR-exit on segmented dispatch where profitable else slow path.
10. **TID/SW+TTL elision, FTL**: mirror Task 9 at §5.5's FTL sites+patchpoints (after 2).
11. **Watchpoint classification+central Class-A stop** (§5.6): bit+Class-B opt-ins+`fireAllSlow` protocol+scope-exit stop+lints+watchdog+direct-fire audit (lock-holders=>M6).
12. **Racy profiling+tier-up serialization** (§5.7): relaxed-atomic counters; tier-up CAS; worklist dedup; Status locker audit; TSAN annotations.
13. **Validation+tests** (I1/I6/I14/I16/I19/I20), OWNED `JSTests/threads/jit/**`. PRE-integration (vs Task-1 stub, phase-1 GIL): golden disasm diff flag-off+`--useJIT=0` bench gate (I1); flag-on 1-thread bench (r12=heap dev-7 split): composite budget **<=5% geomean vs flag-off, GATED in {useJSThreads=1, useSharedGCHeap=0}**; {1,1} MEASURED+RECORDED, not gated (heap §5.5 alloc cost; budget set at GIL-removal chartering); {1,0} miss=>§4.3 LLInt-cache revival REQUIRED pre-ship; fires/sec recorded; shared-constructor construction microbench vs the GIL stub (relative per-op cost, E4 vs post-F2 locked N2/§4.3+L6 with sets force-fired; OM 8h Task-14 trigger — promotion DECIDED PRE-INT); `validateButterflyTagDiscipline`+poll-placement check (I21); PA-transition lint (§5.5; OM 8b); spawned-thread butterfly+shared-AS stress (I14/I20, GIL-interleaved); IC publish/reset loops. INTEGRATION-GATE (skipped while STWR stubbed; re-run at M4/CS2; validates the N-separate-VMs config ONLY — N threads in ONE VM=Phase-B charter (R1 freeze scope), a HARD GIL-removal precondition, api §2; green != one-VM coverage): true-concurrent jettison-vs-execute, fire-vs-execute, direct-call-relink stress as owned `$vm` loops (amplifier best-effort; api G15); epoch tests (retire->safepoint->refcount/free ordering incl. parked-in-slow-path, §4.4b; retire->legacy-GC->free variant runs PRE-integration on heap's §11 legacy site iff N6 shim live, else here); N-thread warmup stop-budget bench (stop count+stopped-time ceiling, INTEGRATE sign-off; bounded by OM F4+§5.6 coalescing).
14. **Manifest handoff**: final M1-M6 diffs into `INTEGRATE-jit.md`; nothing outside owned paths touched; CS1-CS6 dispositions recorded.

Deps: 1->2->3->4->5; 1b after 1, before 8; 6-8 after 1+M4a (prep-phase §10; Task-13 baselines need M4a); 9 needs 1b+3+4; 10 needs 2+9; 11-14 last.
