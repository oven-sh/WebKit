# MC-INIT mapping — Publication-before-initialization / half-built metadata observable

Status: surface map for mechanism class MC-INIT (CVE-AUDIT.md catalog entry, merged
JVM-9 + RG-9 + jvm.md §3.12 swap-vs-use). Compiled 2026-06-07; **revised 2026-06-15**
against jarred/threads to (i) record §K.3, §N.2, and AUD1.N3 rulings as LANDED in tree
and (ii) extend the audit with sub-shape (d) *metadata swap-vs-use* per the 2026-06-15
catalog refresh. Defensive audit artifact. Tests live in `JSTests/threads/cve/mc-init-*.js`.

Mechanism (from the catalog): an object, table, or result is made reachable to a
concurrent reader before its contents are initialized — missing release ordering,
error-path init skip, a sanctioned "being-initialized" state leaking default values,
**or a metadata swap-vs-use** (Structure/Method/class replaced while another thread
holds a raw pointer into the old version). Disclosure twin: uninitialized memory
copied out.

Four concrete sub-shapes audited per surface:
- (a) missing release ordering between contents-init and the publishing store;
- (b) error/abandonment path leaves a "being-initialized" state observable forever
  or re-publishes partial work;
- (c) uninitialized backing memory (alloc slack) reachable through a published
  pointer + length/flag (disclosure twin);
- (d) **swap-vs-use**: a metadata holder's raw pointer outlives a replacement of the
  metadata it points at (JDK-8022887 / JDK-8291830 RedefineClasses shape).

Audit rule applied (CVE-AUDIT.md cross-cutting rule 1): no surface is downgraded on
"window too small" grounds — only on "no concurrent reader/writer can exist" grounds.

---

## Verdict summary

| # | Surface | Verdict |
|---|---------|---------|
| 1 | General new-cell publication (M8 fence discipline) | immune-by-construction |
| 2 | Butterfly/spine publication + allocation slack | immune-by-construction (T5 removal closed the one hole); regression test retained |
| 3 | Sharded atom-string table fill / migrate-then-latch | immune-by-construction; one documented embedder-contract residual |
| 4 | Structure / PropertyTable publication (L6/I37) | immune-by-construction (existing i03 test) |
| 5 | VM / VMLite construction + registration | immune-by-construction |
| 6 | StructureRareData runtime caches (AUD1.N4) | immune-by-construction (ruling landed); amplifier owed by U28 |
| 7 | LazyProperty/LazyClassStructure first-touch (§K.3/LZ1) | **immune-by-construction — §K.3 CORE LANDED 2026-06-10**; regression test retained; U26 arms owed |
| 8 | Rope resolution / atomization (ungil §N.2) | **immune-by-construction — §N.2 LANDED**; regression test retained |
| 9 | ClonedArguments::materializeSpecials (AUD1.N3) | **immune-by-construction — RESOLVED-4 LANDED**; regression test retained |
| 10 | DirectArguments lazy override storage (AUD1.N3) | **immune-by-construction — RESOLVED-3 LANDED**; regression test retained |
| 11 | Metadata swap-vs-use (Structure / PropertyTable / butterfly / spine) | immune-by-construction |

Surfaces 7–10 were *needs-test* at the 2026-06-07 cut (pre-threads in-tree shape, sound
under the phase-1 GIL, textbook MC-INIT the moment GIL came off). All four governing
rulings have since landed; the tests written then are RETAINED as regression gates and
the per-surface entries below cite the in-tree implementation. Any future regression of
a ruling re-opens the surface to MC-INIT.

---

## 1. General new-cell publication — immune-by-construction

The base question for every surface below: thread A allocates and initializes a JSCell,
then makes it reachable to thread B via a *plain* store into a shared slot (SPEC-objectmodel
§3: "Write existing slot, owner or SW=1: mask, store, as today" — plain; M4: "subsequent
property store may be plain"). Why can B not observe a half-built cell?

Producer side — every init store is ordered before every possible escaping store:
- OM M8 (SPEC-objectmodel §7): flag-on, `m_mutatorShouldBeFenced` is forced true for the
  heap lifetime. In tree: `Source/JavaScriptCore/runtime/VM.cpp:696-698`
  (`Options::forceFencedBarrier() = true` before `Config::finalize()`) and
  `VM.cpp:733-734` (`heap.setMutatorShouldBeFenced(true)`).
- With the fence forced on, the *existing* concurrent-GC publication discipline becomes
  the cross-thread publication discipline:
  - C++ cells: `JSCell::finishCreation` executes `vm.mutatorFence()` (= storeStoreFence
    when fenced), `Source/JavaScriptCore/runtime/JSCellInlines.h:99-102`, with the
    comment "this object is ready to be escaped … none of our stores sink below here".
  - Bulk-initialized objects: `ObjectInitializationScope` fences at scope exit
    (`runtime/ObjectInitializationScope.h:66`, `.cpp:48`).
  - JIT allocation fast paths: `AssemblyHelpers::mutatorFence`
    (`Source/JavaScriptCore/jit/AssemblyHelpers.h:2068`) emitted after
    structure/butterfly/inline-storage init at every DFG/FTL allocation site
    (e.g. `dfg/DFGSpeculativeJIT.cpp:470, 8811, 9117, 9278, 9562, 9662, 10133`).
  Any store that publishes the cell pointer is, by program order, *after* the fence, so
  the storeStore barrier orders all init stores before the publishing store on arm64;
  x86-64 is TSO. **Direct JDK-8233839 analog** (aarch64 alloc stub publishes oop before
  `dmb`): the M8 force closes exactly that hole at every JIT alloc stub.

Consumer side — B's loads of the cell's innards (header, butterfly word, inline slots)
are address-dependent on the pointer load (OM M1/M7; `Dependency` use at
`runtime/JSObject.cpp` visit paths). Address dependency + producer-side storeStore
barrier = the classic safe-publication pairing; no acquire needed.

Stores made *after* finishCreation (ordinary property puts) are not covered by that
fence — but they do not need to be: the slot they overwrite was initialized (cleared /
jsUndefined / hole) before the fence, and OM I9/N2 guarantee no observable holes on
transition paths ("release-storing the inline value first (no holes, I9)"). A tardy
reader therefore sees a *stale valid JSValue* (SAB-class program-level staleness), never
uninitialized bits. The value being stale is MC-DF/program territory, not MC-INIT.

Adversarial check: the chain only holds if *every* slot a published cell exposes was
brought to a valid JSValue state before the cell escaped. The places where raw
allocation slack exists are butterflies and aux storage — surface 2.

Verdict: **immune-by-construction** (OM M8 + I9/N2 + M1/M7; in-tree fences cited above).

## 2. Butterfly / spine publication + allocation slack — immune-by-construction

This is the disclosure-twin surface: `Butterfly::tryCreateUninitialized` hands back raw
slack; if a pointer+length pair that covers the slack is ever published before the slack
is cleared, a reader materializes uninitialized memory as a JSValue.

Governing protocol: SPEC-objectmodel §4.2 step 4 / §4.3 step 4 (M2 release-store value
before publication), M3 (seq_cst DCAS/CAS publication), I6 (spines immutable after
publication), I33/C4 (reader bounds by the *loaded* spine), §7 M2/M8.

In tree (`Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp`):
- Every fresh flat/spine payload is fully initialized — live prefix copied, slack
  explicitly cleared or PNaN-filled — then `WTF::storeStoreFence()` ("Contents before
  publication"), then the CAS/DCAS: lines 1950, 2133, 2263, 2290, 2402-2412 (T1 resize:
  clear `[oldVL,newVL)` + `setVectorLength` + fence + `casButterfly`), slack-clear loops
  at 566, 1016, 1944, 2152.
- M2 release stores at 646, 1054, 1098, 1137, 1385.
- The one true MC-INIT hole this design ever had was found and removed in adversarial
  review: **T5 in-place vectorLength growth** (spec §4.4 T5) raised a lock-free foreign
  reader's bound on the *current* payload; the reader's vectorLength load → slot load
  edge is a control dependency only (same base pointer — no address dependency), which
  does not order load→load on arm64, so a reader could pair the post-growth length with
  a pre-hole-fill slot load and lift `tryCreateUninitialized` slack into a JSValue.
  T5 was deleted; every flat owner growth now publishes *fresh* fully-initialized
  storage behind a new butterfly-word load. In-tree record:
  `ConcurrentButterfly.cpp:2358-2375` ("T5 REMOVED (adversarial-review round 1)"),
  spec cross-ref at `ConcurrentButterfly.cpp:2306` and `runtime/JSObject.cpp:6153-6158`.

Adversarial check: the immunity argument is exactly the T5-removal argument — every
length/bound a reader can load travels *with* the storage pointer in one 64-bit word
(or one immutable spine), so a bound and the storage it covers are always published
together. Segmented `publicLength` (C4) is the deliberate exception: it can exceed a
loaded spine's `vectorLength`, and every access clamps by
`min(publicLength, loadedSpine->vectorLength)` (I33) — reads in the gap are *holes*,
not slack.

Verdict: **immune-by-construction**. Because this immunity rests on a reviewer-removed
feature staying removed and on the clamp discipline, a regression probe is included
anyway: `JSTests/threads/cve/mc-init-butterfly-grow-slack.js` (amplifier-ready; asserts
a foreign reader racing owner growth only ever observes the written value, `undefined`,
or a hole — never an alien value).

## 3. Sharded atom-string table — immune-by-construction (+1 documented residual)

Governing: SPEC-vmstate §4.6 F1 ("all StringImpl init (chars, hash, setIsAtom(true))
happens before shard.lock release; consumers acquire the same lock"), §4.5 (plain
hash/flags stores only when *provably unpublished* — constructors, `translate()`
pre-insert; published strings use `fetch_or`), §4.8 (migrate-THEN-latch), §4.7 I17.

In tree:
- All atomization/lookup/removal routes to a shard under its lock
  (`Source/WTF/wtf/text/AtomStringImpl.cpp:128-129, 369-373, 448, 574, 622, 710, 747`;
  rule A1 dual-path shape with `ASSERT(!sharedAtomStringTableEnabled())` on legacy arms).
  Insertion and the reader's lookup synchronize on the same shard lock, so a half-built
  StringImpl is never observable through the table — the spec's required "creation under
  lock, publish fully-built" shape, where the lock release IS the release fence.
  **Direct JDK-6507007 analog** (StringTable publish before fence): closed by the shard
  lock standing in for the missing fence.
- Latch ordering: `Source/WTF/wtf/text/SharedAtomStringTable.cpp:64-105` — singleton
  forced, pre-latch atoms migrated into shards (each under its shard lock), *then* the
  latch is release-stored (`:101`), then the source table cleared. The in-code comment
  spells out why latch-before-migrate would be a UAF; a latch observer can never
  shard-miss a live atom. No sanctioned "being-initialized" table state exists.
- Off-table escape of an atom pointer (F2 pointer-equality fast paths) is covered by
  surface 1: a `JSString`/identifier carrying the atom is a cell published under the M8
  discipline, and the StringImpl's chars/hash were written before the shard lock release
  that first published it (F1) — both orders dominate any later escape.

Residual (documented, not a code hole): the §4.8 ordering contract ("no other thread
atomizes before `JSC::initialize` returns") is an *unassertable embedder contract*. The
ATOMIZE half of a breach fail-stops deterministically (I17 `RELEASE_ASSERT` in
`~AtomStringTable`, `Source/WTF/wtf/text/AtomStringTable.cpp:49-62`); the REF/DEREF half
is a silent release-mode UAF, acknowledged in-code at `SharedAtomStringTable.cpp:43-55`
with the mitigation assigned to the Bun embedder audit (INTEGRATE-vmstate.md cross-WS
item 15). MC-INIT classification: sanctioned init-window risk, owned by an existing
audit item — no new test can exercise it from JS (the breach is by definition pre-JS).

Verdict: **immune-by-construction** for everything reachable from JS post-latch.

## 4. Structure / PropertyTable publication — immune-by-construction

The JVM exemplar (parallel classloading publishing a half-initialized class) maps to
N threads racing transitions through a shared Structure graph.

Governing: SPEC-objectmodel §6 L6/I37 (r14): flag-on, mutator transition-table LOOKUPS
use the m_lock-holding Concurrently variant; inserts already m_lock-held; PropertyTable
steal/clone/materialize runs under the SOURCE's m_lock and "a stolen/fresh table is
private until its new Structure publishes"; uncached table walks hold m_lock across the
walk. SAL (vmstate §5.3/N5) serializes Structure *cell* creation across threads sharing
a heap. A fresh Structure is fully built before the transition-table insert that
publishes it, and both sides of the publish are lock-mediated — the required
"creation under lock, publish fully-built" shape. Compiler-thread `Concurrently` readers
are the pre-existing (pre-threads) concurrent-Structure machinery, unchanged by L6 and
already designed for racing a mutator.

Existing coverage: `JSTests/threads/objectmodel/i03-i37-same-shape-add-storm.js`
(N threads, one shared shape chain; asserts no lost/duplicated transitions, no torn
table reads). No additional MC-INIT test needed — that test IS the MC-INIT probe for
this surface.

Verdict: **immune-by-construction** (L6/I37; existing test).

## 5. VM / VMLite construction + registration — immune-by-construction

- `VM::VM` publishes itself to stop-the-world machinery only after full construction:
  `Source/JavaScriptCore/runtime/VM.cpp:752-758` — `WTF::storeStoreFence()` before
  `m_isInService = true`, and `VMManager::singleton().notifyVMConstruction(*this)` last,
  with the comment "so that a stop-the-world triggered immediately on registration sees
  a fully constructed VM".
- VMLite: the `gilOff` byte is stamped BEFORE `registerLite` publishes the lite to
  registry walkers (`VM.cpp:711-726`; SPEC-vmstate §6.4.4 — registerLite is the sole
  writer of `VMLite::vm`). Registry walkers iterate under the registry lock
  (UNGIL-HANDOUT:229).
- The quarantine-epoch adapter is registered before client #2 can attach
  (`VM.cpp:746-749`; OM §6 release path, heap §9).

Verdict: **immune-by-construction** (init-before-register, lock/fence-mediated both ends).

## 6. StructureRareData runtime caches — immune-by-construction (ruling landed)

AUD1.N4 (UNGIL-HANDOUT RESOLVED-5) ruled the multi-word
{enumerator, watchpoint vector, flag} install: installs under Structure::m_lock; each
JIT-read word single-word release-published LAST after every word it summarizes;
`m_specialPropertyCache` = §K.3-class lazy publication (build, release-CAS the single
pointer, losers discard).

In tree: `Source/JavaScriptCore/runtime/StructureRareData.cpp:119-154` implements the
ruling — comment block at :119-125 restating it, `atomicCompareExchangeStrong` publish
at :148 with loser-discard, m_lock-held entry installs noted at :261. Readers consume
one released word.

Verdict: **immune-by-construction** per the landed ruling. The U28 amplifier arm for
for-in/toString caching on a shared Structure is owed by the ungil workstream (not
duplicated here).

## 7. LazyProperty / LazyClassStructure — immune-by-construction (§K.3 CORE LANDED)

The exact JVM `<clinit>` analog: first touch of a lazily-materialized global
(error-subclass structures, Intl structures, `VM::ensure*`) runs an initializer and
publishes the result; a sanctioned "being-initialized" tag state exists
(`initializingTag`).

Governing ruling (BINDING): SPEC-ungil §K.3 + annex LZ1 (UNGIL-HANDOUT:2497-2540) —
load-acquire fast path; initializing CAS records the owner; winner initializes lock-free
and *release-stores the result (the release-store IS the publication)*; foreign threads
park-capably wait; LZ1 item 3: ANY non-normal initializer exit (exception, termination
poll, thread death) CASes `initializing → empty` and erases the side-table entry BEFORE
propagating — the error-path-init-skip sub-shape (b) is explicitly closed by abandonment,
and "initializers publish only on success; partial work is garbage".

**§K.3 CORE LANDED 2026-06-10 (CVE close-out round).** The MC-SAFE S4 liveness fixes
(shared GCs now actually complete instead of wedging) stretched the init window across
GC pauses and made the foreign-null hole fire at ~27% in
`mc-init-cloned-arguments-specials.js` (SEGV: `constructObjectFromPropertyDescriptor`
consumed a null `accessorPropertyDescriptorObjectStructure()` — a foreign first-toucher
landing on the initializingTag got the recursion-null). Landed in
`runtime/LazyPropertyInlines.h::callFunc` per §K.3 + LZ1:
- claim = acquire CAS on the tag word; OWNER recorded in a leaf-locked side
  table (r16 F2 side-table option);
- OWNER re-entry returns null (landed recursion contract), extended to LZ1.2
  cross-thread ownership cycles via the bounded owner-of -> waits-on chain
  walk (waiter edges published before the first park quantum, LZ1.1);
- FOREIGN threads wait park-capably in 1ms quanta with heap access RELEASED
  (re-acquire = the §A.3.2b/F8-gated AHA, which polls both stop families —
  the r6 F2 three-way-deadlock rule);
- LZ1.3 abandonment: scope-exit restores the pre-claim word if the
  initializer did not publish, erases the owner record, notifyAll.
GIL-on / flag-off reduce to the landed recursion-null contract (foreign arms
unreachable). The U26 arms (deliberate recursion, crossed cycles, owner termination,
forced-GC-during-winner) remain owed to the ungil workstream.

Pre-landing in-tree record (kept for the audit trail):
`Source/JavaScriptCore/runtime/LazyPropertyInlines.h:88-106` / `LazyProperty.h:97,115`
were the pre-threads plain-word implementation: non-atomic `m_pointer` read /
`|= initializingTag` (:103), plain result store, and
`RELEASE_ASSERT(!(m_pointer & initializingTag))` (:106) — sound under the phase-1 GIL,
textbook MC-INIT (sub-shapes (a) AND (b)) once two mutators could first-touch
concurrently.

Verdict: **immune-by-construction** (core landed). Regression gate
`JSTests/threads/cve/mc-init-lazy-global-first-touch.js` retained — N threads
rendezvous and simultaneously first-touch a battery of lazily-initialized globals on
the shared JSGlobalObject; asserts every thread gets a working, *identical*
materialization (one winner, no default/null leak, no crash on the initializing state).

## 8. Rope resolution / atomization — immune-by-construction (§N.2 LANDED)

A JSRopeString resolves by writing the flat `String` and flipping the fiber0/flags
word; the resolved buffer + flag is a multi-word publication readable by any thread
holding the (shared) JSString.

Governing ruling (BINDING): SPEC-ungil §N.2 (UNGIL-HANDOUT annex N7 row R4) — resolver
computes into a *fresh* buffer, publishes by ONE release of the fiber0/flags word;
losers discard and re-read; readers load-acquire; `resolveRopeToAtomString` same shape
vs the shared table; JIT rope slow calls land here.

**§N.2 LANDED** (post-2026-06-07). In tree:
- `Source/JavaScriptCore/runtime/JSStringInlines.h:386-425` `convertToNonRope` —
  GIL-off arm takes the cell lock, re-checks `isRopeInPointer` under the lock (loser
  returns: idempotent), and release-stores the resolved impl into `m_fiber`; trailing
  fibers/length deliberately NOT cleared (concurrent-length readers); GIL-on arm is the
  same one-pointer release store. The release ordering replaces the pre-threads
  `storeStoreFence` and is the publication: the resolved StringImpl's contents
  happen-before any reader observing the flipped bit.
- `Source/JavaScriptCore/runtime/JSString.h:974-1008` `swapToAtomString` — GIL-off arm
  is a release `atomicExchange` of `m_fiber` (transfers ownership of exactly one prior
  impl to exactly one exchanger; closes the double-adopt/over-deref UAF that two plain
  swaps would create); GIL-on arm = relaxed-load + release-store. Old impl appended to
  `Heap::m_possiblyAccessedStringsFromConcurrentThreads` (sub-shape (d) liveness, see
  surface 11).
- Reader arm: `JSString.h:346-354` `fiberConcurrently()` is the §N.2 acquire load
  (`memory_order_acquire` flag-on); every rope-decision site snapshots `m_fiber` ONCE
  through it (`JSString.cpp:160-230, 286-310`; `JSString.h:898-901, 1231-1294`) and
  re-dispatches on a lost publish race ("recover through the non-rope toAtomString
  path").

Adversarial check (sub-shape (a)): the design landed as cell-lock-mediated idempotency
rather than the spec's literal release-CAS — equivalent for MC-INIT: the
isRope→resolved transition is monotone, the under-lock re-check makes it once-only, and
the release-store on the publishing path orders the buffer fill (done outside the lock
into a fresh impl) before any acquirer observing the flipped bit. Sub-shape (b) error
path: on OOM/exception the resolver returns BEFORE `convertToNonRope` (e.g.
`JSString.cpp` `RETURN_IF_EXCEPTION` before publish sites); `m_fiber` keeps
`isRopeInPointer` and the cell stays a valid rope — no half-state leaks.

Verdict: **immune-by-construction** (§N.2 landed). Regression gate
`JSTests/threads/cve/mc-init-rope-resolve-race.js` retained — many fresh shared ropes,
N threads concurrently force resolution (===, charCodeAt, length) and atomization
(property-key use); asserts every observation equals the independently-built flat
expectation, never empty/torn/partial.

## 9. ClonedArguments::materializeSpecials — immune-by-construction (RESOLVED-4 LANDED)

Governing ruling (BINDING): AUD1.N3 second half (UNGIL-HANDOUT RESOLVED-4): the
`m_callee` flag word (doubles as not-yet-materialized flag,
`runtime/ClonedArguments.h:100-104`, JIT offset :78) must be release-stored AFTER the
OM puts; foreign slow-path readers acquire; tier-inlined fast paths re-pointed/fenced.

**RESOLVED-4 LANDED.** In tree, `Source/JavaScriptCore/runtime/ClonedArguments.cpp`:
- `materializeSpecials` (:306-351) — snapshots `m_callee` once; GIL-off null snapshot
  = lost-to-a-completed-racer (`loadLoadFence()` then return, :319-323); both racers may
  run the puts concurrently and that is *deliberate*: every racer computes IDENTICAL
  values (per-realm singletons eagerly initialized under `useJSThreads()`, comment
  :328-338) and same-key concurrent adds are serialized by the OM transition/put
  protocol (no lost properties, I21), so the racing stores are idempotent. Then
  `WTF::storeStoreFence()` (:348-349) BEFORE `m_callee.clear()` — the release half.
- `materializeSpecialsIfNecessary` (:353-364) — the reader-acquire half:
  `WTF::loadLoadFence()` after observing the cleared flag, before the caller falls
  through to `Base::` reads of the materialized properties.

Sub-shape (b) error path: `putDirect`/`putDirectAccessor` here are the never-throwing
`JSObject::putDirect` (no allocation under the per-realm-singleton case beyond the OM
put, which RESTARTs internally on race); on the strict-mode arm `putDirectAccessor`
allocates the GetterSetter eagerly at realm init — neither path leaves the flag cleared
with puts unwritten, because the fence+clear is unconditional last.

Verdict: **immune-by-construction** (RESOLVED-4 landed). Regression gate
`JSTests/threads/cve/mc-init-cloned-arguments-specials.js` retained.

## 10. DirectArguments lazy override storage — immune-by-construction (RESOLVED-3 LANDED)

Governing ruling (BINDING): AUD1.N3 first half (UNGIL-HANDOUT RESOLVED-3):
`m_mappedArguments` (+ modified-arguments descriptor bitmap) becomes CAS-PUBLISH —
allocate+fill complete, release-CAS the pointer, losers discard, readers load-acquire;
the tier-inlined null-check stays (address-dependent load).

**RESOLVED-3 LANDED.** In tree, `Source/JavaScriptCore/runtime/DirectArguments.cpp`:
- `overrideThings` (:133-192) — allocates the bitmap, fills every byte with relaxed
  atomic stores (:172-175, "recycled-address races" annotation), then GIL-off
  `WTF::atomicCompareExchangeStrong(&m_mappedArguments, nullptr, overrides)` (:185)
  with loser-discard ("RESOLVED-3 CAS-PUBLISH: allocate + fill complete (above), CAS
  the pointer"); GIL-on plain `set` (:191). The CAS is the release-publish.
- Constructor stores `m_length`/`m_minCapacity` via relaxed atomics (:58-61) so the
  GC visit's relaxed loads (:119) pair (recycled-address discipline).
- Readers go through `overrodeThings()` (not a raw `m_mappedArguments` test) — comment
  at :44, :196, :216 — and the tier-inlined null-check is address-dependent on the
  loaded pointer (offsets at `DirectArguments.h:153-154`).
- `overrideArgument` flips bytes in the published bitmap with relaxed atomic stores
  (:211, "SAB-grade per-bit store") — post-publication mutation of bitmap *contents*
  is sub-shape MC-DF (program-level staleness), not MC-INIT.

Sub-shape (c) check: the bitmap is fully `false`-filled before the CAS; a foreign
reader pairing the published pointer with its contents is address-dependent on the CAS
result — no slack reachable.

Verdict: **immune-by-construction** (RESOLVED-3 landed). Regression gate
`JSTests/threads/cve/mc-init-direct-arguments-override.js` retained.

## 11. Metadata swap-vs-use (sub-shape (d)) — immune-by-construction

The JDK-8022887 / JDK-8291830 shape: thread B holds a raw pointer into metadata (a
Method*, vtable slot, class) while thread A *replaces* the metadata in place or swaps
in a fresh version, and the old version is freed/mutated under B. JSC analogs and the
governing invariant for each:

- **Structure replacement.** Transitions never mutate a published Structure in place;
  they create a *fresh* Structure cell and CAS/DCAS its ID into the holder
  (SPEC-objectmodel §3/§4.3, I5). A foreign reader holding an old `Structure*` (or
  StructureID) holds a still-live, immutable, GC-rooted cell: Structures are JSCells in
  the structure subspace, kept live by the holder's header AND by the conservative scan
  of the reader's stack (I7). The one in-place exception — `flattenDictionaryStructure`
  rearranging storage — is per-event STW (F3, SPEC-objectmodel §5: "flatten rearranges
  storage in place => per-event STW on shared objects"); no concurrent holder exists
  across the swap.

- **PropertyTable steal/clone.** `takePropertyTableOrCloneIfPinned`
  (`Structure.cpp:912-924`) removes the table from the source while a walker might hold
  a raw `PropertyTable*`. L6(ii)+(iii) (SPEC-objectmodel §6): steal/clone/materialize
  runs under the SOURCE's `m_lock`; mutator uncached table WALKS hold `m_lock` across
  the walk. Walker and stealer serialize on the same lock — no holder can be mid-walk
  during the steal. Compiler-thread `Concurrently` readers are the pre-existing
  pre-threads concurrent-Structure machinery (locked their side).

- **Butterfly / spine replacement.** T1/T2/§4.3 republish a fresh butterfly word; a
  foreign reader can hold the OLD payload pointer in a register across the republish.
  I7: "Nothing reachable freed/reused: stale stacks = conservative scan; aliased flat
  allocation = spine-recorded base, marked each visit (§4.5)". I6: "Published spines
  never mutated; growth = new spine". §4.6 AS-COPY: "superseded storage never written
  again (stale readers frozen; conservative scan, I7)". The old payload stays live AND
  immutable for the holder's lifetime; the reader sees a stale-but-consistent snapshot
  (SAB staleness), never a half-reused allocation. I27b ("a locked transition never
  republishes an older butterfly payload") closes the reverse direction.

- **Replaced StringImpl (rope re-publish / atom swap).** `swapToAtomString`
  (`JSString.h:974-1008`) republishes `m_fiber` while compiler/GC threads (and now
  foreign mutators) can hold the OLD `StringImpl*`. The old impl is appended to
  `Heap::m_possiblyAccessedStringsFromConcurrentThreads` and freed only at end-of-GC
  with all concurrent holders stopped (in-code comment :976-980). A holder of the old
  pointer dereferences a still-live, content-immutable StringImpl.

Adversarial check: the common shape across all four is *new cell + retire old, never
mutate-in-place under a possible holder*; the one designed in-place mutation (flatten,
Double-shape relabel §4.7) is forced through per-event STW (I28/I34: "no reader holds
the old shape across a stop"). I34 ("no path polls/allocates/parks between obtaining a
PropertyOffset/slot pointer and the access, unless it re-validates structureID after")
is the discharge that no holder can outlive a swap by yielding to one mid-use.
Sub-shape (d) therefore reduces to liveness (I7/conservative scan +
m_possiblyAccessedStrings) + post-swap immutability (I6/AS-COPY) + STW gating of the
in-place exceptions (F3/I28) — every leg cited in a frozen invariant.

Verdict: **immune-by-construction** (I6 + I7 + I27b + I34 + L6 + AS-COPY + F3/I28 STW
gating; `m_possiblyAccessedStringsFromConcurrentThreads` for StringImpl). No new test:
the holder-side liveness is already exercised by `mc-init-butterfly-grow-slack.js`
(surface 2) and `i03-i37-same-shape-add-storm.js` (surface 4); the StringImpl arm by
`mc-init-rope-resolve-race.js` (surface 8); and per-event-STW gating by the existing
`JSTests/threads/objectmodel/i28-*` corpus.

---

## Tests delivered (JSTests/threads/cve/)

| Test | Surface | Mode |
|------|---------|------|
| `mc-init-lazy-global-first-touch.js` | 7 | deterministic rendezvous + race; crash/lost-default detector — **regression gate** (§K.3 landed) |
| `mc-init-rope-resolve-race.js` | 8 | amplifier-ready race; torn/partial-resolution detector — **regression gate** (§N.2 landed) |
| `mc-init-cloned-arguments-specials.js` | 9 | amplifier-ready race; lost-property detector — **regression gate** (RESOLVED-4 landed) |
| `mc-init-direct-arguments-override.js` | 10 | amplifier-ready race; default-leak/garbage detector — **regression gate** (RESOLVED-3 landed) |
| `mc-init-butterfly-grow-slack.js` | 2 (regression) | amplifier-ready race; uninit-slack disclosure detector |

All carry `//@ requireOptions("--useJSThreads=1")` (plus stress flags where noted) and
bound every blocking operation (annex T2). Run post-ungil under TSAN no-JIT and
`Tools/threads/amplify.sh`, then with default JIT tiers.
