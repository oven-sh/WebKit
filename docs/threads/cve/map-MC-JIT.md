# map-MC-JIT — "JIT proves an invariant once; a second mutator falsifies it between proof and use"

Status: defensive audit artifact, 2026-06-07. Mechanism class MC-JIT from
`docs/threads/CVE-AUDIT.md` (merged JS-J + JVM-8; exemplars CVE-2021-2388,
CVE-2023-22044/22045, CVE-2024-20918/20952, CVE-2019-5782). Read-only audit of
the `jarred/threads` tree at this revision; specs cited at their on-disk revs
(SPEC-jit r12, SPEC-objectmodel ("OM") r14, SPEC-ungil + UNGIL-HANDOUT rev 32).

The mechanism: a compiler caches/hoists/CSEs a length, base pointer, shape, or
eliminated bounds check across a point where another agent can legally mutate
it — or proves an invariant once and emits unchecked code that a path the
proof never covered can break. The check side is correct in isolation; the
*lifetime of the proof* is the bug.

Our design's two sanctioned proof lifetimes:

- **Watchpoint-funneled** (E1/E2 TTL elision, detach/neutering, structure
  adaptors): every falsifying event fires a Class-A WatchpointSet under STW
  (jit §5.6), the fire jettisons registered code (jit §5.3), and parked
  mutators resume into an invalidation exit (jit I21), never across the dead
  proof.
- **Stability-backed** (everything unregistered): the cached fact may go
  stale, but staleness is harmless because the thing it points at never
  changes meaning — slot addresses are stable (OM §4.2 zero-copy aliasing,
  I8), spines are immutable (OM I6), superseded storage is never rewritten
  (AS-COPY, OM §4.6) and never freed while a stack can reference it
  (conservative scan, jit R2 / OM I7), deleted slots are quarantined (OM I18).

Every surface below is classified by which lifetime covers it — and whether it
actually does.

Verdict summary:

| # | Surface | Verdict |
|---|---|---|
| S1 | TTL-justified elision (E1/E2/E3) vs concurrent falsification | immune-by-construction |
| S2 | Value-numbered butterfly/shape/length facts surviving polls in **unregistered** DFG/FTL code | **CLOSED Tier-B B3** (poll-clobber + Task-13 lint landed; pinned by `mc-jit-stale-base-grow-oob.js`) |
| S3 | Bounds-check elimination / CheckInBounds CSE on butterflies | immune-by-construction (conditional on S2) |
| S4 | Typed-array/ArrayBuffer cached {base,length} vs detach/shrink/grow | needs-test (design covered, unimplemented) |
| S5 | Compile-time proof vs installation (DFG Plan finalize) | immune-by-construction |
| S6 | E4 owner-transition validate→commit window (OM I29) | immune-by-construction |
| S7 | IC handler chains / call-link records | immune-by-construction |
| S8 | LLInt/Baseline single-word metadata caches | immune-by-construction |
| S9 | Racy-profile-driven speculation | immune-by-construction |
| S10 | Compiler thread reads mutable heap state (V8 ArrayShift analog) | immune-by-construction |
| S11 | GC write-barrier elision vs N-mutator collection advance | immune-by-construction |
| S12 | Lock-pairing elision (HotSpot lock-coarsening analog) | immune-by-construction (no surface) |

---

## S1. TTL-watchpoint-justified elision (E1/E2/E3) vs concurrent falsification

Surface: DFG/FTL code that omits the segmented-dispatch check (E1), the SW
branch (E2), or both (E3 reads) because a structure's
`transitionThreadLocal`/`writeThreadLocal` sets were valid+watched at compile
time. This is the literal MC-JIT shape: the proof ("no instance of S is
segmented / shared-written") is established once, at compile time.

- `Source/JavaScriptCore/dfg/DFGDesiredWatchpoints.h:262-286` —
  `considerButterflyTransitionThreadLocal` / `considerButterflyWriteThreadLocal`,
  registration as `CodeBlockJettisoningWatchpoint`.
- Governing spec: jit §5.5 (elision contract, D9/CS5), §5.6 (Class-A fires),
  §5.3 (jettison under STW), I21 (poll → invalidation point), OM §5 E1-E4 +
  F1/F2 firing triggers, OM I13 (fires only in STW), I14 (elide only while
  valid+watched).

Verdict: **immune-by-construction.** The falsification chain is closed at
every link:

1. Every event that falsifies the proof (first foreign write F1, foreign or
   shared transition F2, flatten F3) fires the set *before* the falsified
   state is publishable (OM I10b: fire precedes cell-lock acquisition and
   first SW/segmented publication), under STW (OM I13).
2. The fire jettisons every registered CodeBlock inside the same stop closure
   (jit §5.6 steps 4-5), synchronously (history §13.5; coalesced fires drain
   in one stop).
3. A mutator parked mid-loop resumes into the invalidation exit, not the
   elided body: jit I21 (`usePollingTraps` forced by M2b; every DFG/FTL poll
   immediately followed by an invalidation point), with the F5 ISB on resume.
4. The compile-vs-fire race is closed at link time: the helpers' validity is
   re-checked under `reallyAdd()` (S5 below) — a set fired between compilation
   and linking fails the compilation.
5. D9: the write predicate's fused TID compare is NEVER elided in any tier —
   it is the sole F1 detection point — so even E1+E2 writes detect a
   shared-write transition without any watchpoint help.

Adversarial residue: link 3 is a *discipline*, not a structure — it holds only
if no compiled fast path holds elided state across a poll without an
invalidation point in between. That is exactly jit I21's "Task-13 lint extends
I16 to poll windows", which is owed but not yet visible in the tree. The
generic version of that gap (for UNregistered code, where the invalidation
point is a no-op) is S2, where it is a real finding. For registered code the
invalidation point makes any residual hoisting safe. Existing coverage:
`JSTests/threads/jit/int-gate-fire-vs-execute.js`,
`int-gate-jettison-vs-execute.js` (integration gate, re-run at M4/CS2).

---

## S2. Value-numbered butterfly/shape/length facts surviving safepoint polls in UNREGISTERED flag-on DFG/FTL code

**Verdict: CLOSED (Tier-B B3, B3-JIT-POLL-CLOBBER-LINT).** This was the
audit's central finding; the original analysis is preserved below for the
audit trail.

**Closure (2026-06):** the GIL-off `CheckTraps` clobberize entry
(`dfg/DFGClobberize.h`) now writes `JSObject_butterfly` and
`Butterfly_vectorLength` in addition to the §7.1 interim value-heap writes,
so CSE/LICM cannot carry a `GetButterfly` result (or a vectorLength fact)
across a poll. That forces SAME-SNAPSHOT `{base, publicLength,
vectorLength}` per poll boundary — the sufficient condition for S3's
immune-by-construction BCE argument — closing arm (a) (the only arm not
already covered by the precise-jettison + epoch-bump mechanism: (b)/(c) go
through STW and bump `conductorHeapFactRewriteEpoch`, jettisoning the parked
mutator on resume; (a)'s flat→segmented conversion with dead TTL sets is
lock-free, no STW, no epoch bump). `JSCell_structureID`/`JSCell_indexingType`
remain hoistable across polls (the de-jank's chief win), guarded by the
invalidation point + precise jettison. The Task-13
`validateButterflyTagDiscipline` lint is now a real check
(`validateButterflyTagDisciplineForGraph`, defined in
`dfg/DFGSpeculativeJIT.cpp`, declared in `dfg/DFGClobberize.h`, called from
both backends just before codegen): a forward "available GetButterfly"
dataflow that asserts (I14) every butterfly storage consumer takes its
storage from a tag-masking/tag-zero producer and (I21(b)) no `GetButterfly`
result is consumed across a `JSObject_butterfly`-clobbering boundary. Gate:
`JSTests/threads/cve/mc-jit-stale-base-grow-oob.js` (was XFAIL/ASAN, now
EXPECTED PASS); `JSC_validateButterflyTagDiscipline=1` corpus run = 0
violations.

### The surface

Flag-on, non-elided DFG/FTL code emits the full §5.5 predicates — but emits
them **once per `GetButterfly` / structure-check node**, and the DFG value
numbers those nodes. The clobberize model lets the resulting masked base
pointer, structure proof, and AI shape facts survive safepoint polls:

- `Source/JavaScriptCore/dfg/DFGClobberize.h:617-620` — `CheckTraps` reads and
  writes **only `InternalState`**. It does not clobber `JSObject_butterfly`,
  `JSCell_structureID`, `JSCell_indexingType`, or any length heap — flag-on or
  off.
- `Source/JavaScriptCore/dfg/DFGClobberize.h:1594-1604` — `GetButterfly`
  flag-on reads `JSCell_structureID`/`JSCell_indexingType` (predicate inputs)
  but still `def()`s its result: CSE folds repeated loads, and LICM may hoist
  a `GetButterfly` out of a loop whose body contains `CheckTraps`.
- `InvalidationPoint` (`DFGClobberize.h:622-625`) writes only `SideState` — it
  protects **jettisoned** code (the jump is patched), but for code nobody
  jettisons it is a no-op and kills no defs.

So an unregistered compiled loop can carry `{masked flat base, structure
proof, shape proof}` across a park. Class-A fires can only save it if the code
registered the fired set — which, by hypothesis (sets already invalid; that is
*why* the code compiled without elision), it did not.

### Which foreign falsifiers can strike while the proof is parked

All require the victim structure's TTL sets to be already fired — i.e. a
shared, mature object graph, exactly the attacker-reachable steady state:

(a) **Flat→segmented conversion + growth → stale-base OOB (OM I9b).**
Foreign thread converts the object (OM §4.2 — no STW once sets are dead) and
grows it (T2 new spine, bigger vectorLength). `setSegmentedPublicLength`
stores into fragment-0-slot-0's low half — which **aliases the flat
IndexingHeader** (`runtime/Butterfly.h:479-482`). The live publicLength now
exceeds the frozen flat-era vectorLength. A tardy flat reader that re-loads
publicLength **through its stale base** sees the grown value:

- `Source/JavaScriptCore/dfg/DFGSpeculativeJIT64.cpp:2784, 2861` — contiguous
  GetByVal in-bounds check is `branch32(AboveOrEqual, property,
  Address(storageReg, Butterfly::offsetOfPublicLength()))` — publicLength
  only, through the (possibly stale) storage register, flag-on unchanged.
- Indices in `[frozenFlatVectorLength, livePublicLength)` then pass the bounds
  check and dereference past the flat allocation's edge → heap OOB
  read/write. This is bit-for-bit the V8 typer-BCE shape (CVE-2019-5782) and
  the C2 range-check-elimination shape (CVE-2021-2388), transplanted.
- The runtime side honors I9b (`runtime/Butterfly.h:302-303,447`,
  `ButterflyInlines.h:408`, `ConcurrentButterfly.cpp:436` —
  `frozenFlatVectorLength()` asserts), and OM I9b names exactly this bound for
  "tardy flat-side array access". **No JIT fast path in the tree enforces
  it.** Phase-1 is sound only because the GIL serializes mutators — the tree
  says so itself (`dfg/DFGSpeculativeJIT64.cpp:8517-8521` FIXME: "mask-only is
  sound while the GIL serializes mutators").
- Note the mismatch case needs *stale base + fresh length*: a loop body that
  clobbers `Butterfly_publicLength` (e.g. a push on a different array) but not
  `JSObject_butterfly` re-loads the length while CSE keeps the base. Same-
  snapshot staleness (both stale / both fresh) is safe (S3).

(b) **Per-event-STW Double relabel vs hoisted shape proof (OM §4.7/I28).**
Int32↔Double / Double→Contiguous on an SW=1 object relabels slots **in place**
under a per-event STW. OM I28's guard is "no reader holds the old shape across
a stop" — i.e. OM I34 ("no path polls between obtaining a slot pointer and the
access, unless it re-validates structureID after") applied to generated code.
A compiled loop with `CheckArray(Double)` + `GetButterfly` hoisted above its
poll *is* a reader holding the old shape across a stop. Post-resume it keeps
storing **raw unboxed doubles** into slots every other thread now interprets
as JSValues: an attacker-controlled 64-bit bit pattern read as a cell pointer
— the classic fakeobj primitive. (Reverse direction: boxed values written into
raw-double slots → arbitrary-double leak of pointer bits.) Nothing fires: the
sets are dead, and the relabel's STW only *parks* the victim, it doesn't
invalidate it.

(c) **Delete → quarantine-epoch reuse vs hoisted structure proof (OM I18/D1).**
Foreign delete (locked, no STW with dead sets) quarantines the slot; the
owning heap's epoch bumps at a collection stop — which the victim loop can sit
through, parked, holding a hoisted `CheckStructure(S_old)` proof and butterfly
base; the slot is then promoted and reused by a new property `g`. The resumed
loop keeps writing property `f`'s old offset → writes `g`'s slot: OM I21's
forbidden "read of f returning g's value". JSValue-into-JSValue, so semantic
corruption rather than direct memory unsafety — but it is the exact JVM-8
"proof outlives a structural change" mechanism, and it composes with (b) for
shape-changing reconfigurations.

### Why this is a design seam, not just an implementation TODO

SPEC-jit's poll discipline (I16, I21's "lint extends I16 to poll windows")
states the right invariant — no dependent use of a butterfly/IC fact across a
poll — but the only *mechanism* in the tree is per-emission predicates plus
clobberize, and clobberize currently says polls preserve butterfly state. The
fix shape (for the implementation, not this audit): flag-on, `CheckTraps` /
`InvalidationPoint` clobberize must write `JSObject_butterfly`,
`JSCell_structureID`, `JSCell_indexingType`, `Butterfly_publicLength` (so CSE/
LICM/AI cannot carry unregistered object-shape facts across a park), with the
E1/E2-registered facts exempt (their lifetime is watchpoint-funneled); plus
the I9b vectorLength clamp on any flat indexed fast path that can be reached
with a stale base, if cross-poll caching is ever re-allowed.

### Tests (written, NOT run — execute post-ungil)

- `JSTests/threads/cve/mc-jit-stale-base-grow-oob.js` — arm (a):
  ASAN/validation oracle for the stale-flat-base / grown-publicLength OOB.
- `JSTests/threads/cve/mc-jit-double-relabel-stale-shape.js` — arm (b):
  fakeobj oracle for raw-double stores under a relabeled shape.
- `JSTests/threads/cve/mc-jit-delete-reuse-stale-offset.js` — arm (c):
  cross-property aliasing oracle (OM I21) for quarantine-epoch reuse under a
  hoisted structure proof.
  **OPEN FLAKE (recorded 2026-06-10, review round):** ~1/6 GIL-off Release
  runs die exit 134 on the 30s STW watchdog ("Pending Class-A fire context:
  WatchpointSet Class-A fire" — SPEC-jit App. 5.6(c) bucket iii / FIX-2
  mechanisms (1)/(2)), NOT on this test's aliasing oracle. Same
  stop-progress signature class mc-aint's record attributes its single
  flake to, and a live candidate for the unattributed historical GIL-off
  failures recorded in map-MC-VAL V1/V8 and map-MC-INT S4. Needs
  stop-progress triage (escaped lock-holding fireAll caller, or a mutator
  parked in a native wait holding heap access without an access-release
  bracket/per-quantum poll); a test repair would be the WRONG fix.

---

## S3. Bounds-check elimination / CheckInBounds CSE on butterflies

Surface: `DFGIntegerRangeOptimizationPhase` and CheckInBounds CSE
(`dfg/DFGClobberize.h:262-263` pure; `dfg/DFGCSEPhase.cpp:546`) prove
`i < length` from one length SSA value and drop later checks.

Verdict: **immune-by-construction, conditional on S2's discipline.** Within
one snapshot, {base, length} from the same `GetButterfly`/`GetArrayLength`
pair is safe under any staleness:

- Flat storage is valid to its vectorLength and is never shrunk in place or
  freed while reachable from a stack (conservative scan, jit R2 / OM I7; T1
  copying resize is owner-only and CASes, leaving the old allocation frozen).
- Spines are immutable (OM I6); segmented growth allocates fresh fragments,
  never moves old ones (OM §4.3-1).
- AS — the only shape whose innards relocate (indexBias/vector moves) — never
  reaches a generated fast path unlocked (jit I20 / OM I31, AS-COPY), enforced
  in-tree by the choke-point shape dispatch
  (`jit/CCallHelpers.h:944-1012`, `ConcurrentButterflyShape`).
- `a.length` shrink on a shared array is a publicLength store; storage
  validity is bounded by vectorLength, so a stale larger publicLength is
  semantically stale but memory-safe.

The only way BCE goes memory-unsafe is a *mixed* snapshot — stale base with
fresh (grown) length — which is exactly S2(a). No separate test; S2's test is
the discharge.

---

## S4. Typed arrays / ArrayBuffer: cached {base,length} vs detach/transfer/shrink/grow

Surface: every tier's TA/DataView fast path loads length, bounds-checks, then
loads base, and DFG/FTL hoist `GetTypedArrayVector`/lengths out of loops.
Falsifiers: `ArrayBuffer::detach` (`runtime/ArrayBuffer.cpp:525-528`),
`transferTo` (`:498,519`), downward `resize` (`:628-639`), `memory.grow`;
view-side `m_vector/m_length/m_mode` (`runtime/JSArrayBufferView.cpp:265,327`).
This is the RAB/GSAB structural hazard named in the class definition, and the
CVE-2024-20918-shaped "uncovered path falsifies the cached base".

Governing design: UNGIL-HANDOUT item 6 / annex N6 (rev 32, lines ~2820-2925) +
N7 rows R10/R11:

- detach publishes length=0 seq_cst, never clears the base word; the mapping
  moves into a per-server **quarantine** retired only at a heap §10 stop under
  quiescence — so any pre-retirement {oldLen, oldBase} torn pair still points
  at mapped memory;
- transfer = copy + detach (no live-transferee aliasing of a quarantined
  mapping);
- shrink defers `freePhysicalBytes`/protect to the next stop (tail stays
  committed for stale readers);
- grow is base-immutable (reserved VA: commit pages, then release-publish the
  larger length); relocating grow only under a stop, old mapping quarantined
  one more stop "for captured/hoisted bases in jettisoning code";
- hoisted vectors are watchpoint-funneled (detach/neutering adaptors →
  jettison), with the quarantine as the second net for code that raced the
  jettison.

Verdict: **needs-test.** The design closes every torn-pair row (annex N6
table) — by the S1 funnel *plus* a stability backstop, which is the right
belt-and-suspenders for MC-JIT — but none of the quarantine arms exist in the
tree yet (the landed code frees on the detaching/resizing thread), and the
handout itself owes the U28 amplifier arms. Test written:
`JSTests/threads/cve/mc-jit-ta-resize-hoisted-base.js` (spawned compiled
TA reader/writer storm vs main running detach/transfer/shrink/grow churn; any
pre-quarantine build crashes under ASAN, a conforming build must only ever
observe in-bounds stale values or bounds failures).

---

## S5. Compile-time proof vs installation (DFG Plan finalize)

Surface: the compiler thread proves watchpoint validity during
`compileInThread`; the proof can die before the code becomes reachable
(HotSpot's recurring nmethod-dependency race; JVM-8 family).

- `Source/JavaScriptCore/dfg/DFGPlan.cpp:595-605` — `reallyAdd`:
  `areStillValidOnMainThread` then registration; registration itself fails if
  a set fired in between (`DFGDesiredWatchpoints.h:275-277`: "revalidated
  under reallyAdd(), so a set fired between compilation and linking fails the
  compilation").
- `Source/JavaScriptCore/dfg/DFGPlan.cpp:617-647` — UNGIL IT-8
  `GILOffCompilationLocker`: finalization vs sibling-lite installs/jettisons
  serialized; `:674-679` — post-`reallyAdd` `isJettisoned()` re-check (a fire
  *during* registration resolves to CompilationInvalidated).
- The remaining window — fire lands after `areStillValidOnMainThread` but
  before registration — requires the finalizing mutator to park, and finalize
  is straight C++ with the heap deferred and no poll sites; Class-A fires are
  cooperative-stop-only (jit R1.f), so the fire's STW cannot complete until
  finalize exits, by which point registration has happened and the jettison
  path owns the rest.

Verdict: **immune-by-construction.** The documented residual
(`DFGPlan.cpp:639-646`: compiler reads baseline profiling/IC state mutated by
plain stores) is profile-*quality* only — jit I12: profiles select, guards
validate — and is MC-DF territory, not MC-JIT, provided no guard is elided on
profile data alone (it is not: elision requires watchpoints, S1).

## S6. E4 owner-transition validate→commit window

Surface: the lock-free owner transition validates (sets valid+watched, tag ==
(currentTID,0), !PA) and then commits structureID/butterfly with no
synchronization — a window MC-JIT-falsifiable if anything (GC, poll,
allocation) can interpose after validation.

Verdict: **immune-by-construction.** OM I29 is precisely the closing
invariant: allocate *before* final validation; **no poll/alloc/safepoint
between validation and the StructureID store**; else re-validate and take
§4.3. The JIT emission rule mirrors it (jit §5.5 Transition: the runtime
checks are the last thing before the commit; "the JIT never implements
transition semantics"), and in-tree the flag-on transition emission asserts
its preconditions (`dfg/DFGSpeculativeJIT.cpp:11340-11343,11384,11448,11486`).
Foreign contenders cannot interleave invisibly: they must fire F2 (STW — the
owner is not at a poll inside the window, so the stop waits out the window) or
lose the (D)CAS. Existing coverage: OM Task-12 suites
(`JSTests/threads/objectmodel/`, I21/I15/I29 targets) + jit
`spawned-thread-butterfly-stress.js`.

## S7. IC handler chains / call-link records

Surface: Baseline/DFG/FTL property ICs and call linking — the proof is the
guard (structure id / callee comparand); the cached payload is the handler
node or `CallLinkRecord`.

Verdict: **immune-by-construction.** Publish-once immutability is the whole
design: handler nodes frozen at publish (jit I4), single-word inlined state
(§4.2 packed word, all-zero invalidation, I6 "never valid id + mismatched
offset"), records immutable with all reads address-dependent through one
published pointer (§5.8/F2/F6 — stale read = complete OLD record, benign);
unlink is a monotone null store; retirement is epoch-deferred (§4.4, I7/I9) so
a stale pointer never dangles; I16 keeps the load→use window poll-free
(`jit/CCallHelpers.h:991-992`: "the helpers emit none"). The falsified-proof
case (guard passes against a stale chain) cannot produce a wrong payload —
guard and payload travel in one word or behind one pointer.

## S8. LLInt / Baseline metadata caches

Verdict: **immune-by-construction.** LLInt re-dispatches every opcode
execution — no cross-poll caching exists at this tier — and the flag-on cache
form is one aligned u64 `{structureID, offset}` (jit §4.3/F3): a stale word
fails the id-compare half; everything not expressible as one word is disabled
(proto-load, transition, private-name rows of the §4.3 table). The mode-byte
coherence hazard is closed by publishing only `Default`/`ArrayLength` (I18-jit),
and `ArrayLength` self-validates via indexing bits without touching word 1.

## S9. Racy-profile-driven speculation

Verdict: **immune-by-construction** by principle, not by luck: jit §5.7/I12 —
every profile datum is advisory (selects a speculation), every speculation is
revalidated by an emitted guard or OSR exit; torn/stale profile values can
mis-speculate but not mis-prove. Tier-up itself is CAS-serialized (§5.7.2) and
worklist-deduped. The one place profile data could have become a proof —
elision — is explicitly watchpoint-gated instead (S1).

---

## Cross-references

- MC-TEAR owns the {structureID, butterfly, length} *tearing* side (M7/F7
  address dependencies); MC-JIT owns the *lifetime* side. S2(a) is their
  composition.
- MC-CODE owns patching/jettison/reclamation of the machine code itself (jit
  §5.3/§4.4, AB-17 territory); MC-JIT assumes those funnels work.
- MC-REENT's sequential twin: every same-thread side-effect that today kills
  DFG abstract state still does (clobberize is *more* conservative for
  effectful nodes); MC-JIT is exclusively about the second mutator, whose
  writes correspond to **no node at all** in the victim graph — which is why
  the poll-clobber rule in S2 is the only general fix shape.

---

## Addendum (2026-06-15): round-2 exemplars — barrier/lock-pairing elision + concurrent-compiler-as-agent

The round-2 mechanism-class wording widens "another agent" to include the
engine's own concurrent compile/mark thread, and adds "barrier" and
"lock-pairing invariant" to the list of facts a JIT may prove-once-then-reuse.
New exemplars: V8 ArrayShift × concurrent-TurboFan race (no CVE),
CVE-2011-0990, JDK-8268347, JDK-8007898, RAB/GSAB length-tracking.

| # | Surface | Verdict |
|---|---|---|
| S10 | DFG/FTL **compiler thread** reads mutable heap state (butterfly base/slot/shape) during `compileInThread` while N mutators run | immune-by-construction |
| S11 | GC **write-barrier elision** (StoreBarrierInsertionPhase epoch proof) vs a poll where another mutator can advance the collector | immune-by-construction |
| S12 | **Lock-pairing** elision (HotSpot lock-coarsening / biased-lock analog) | immune-by-construction (no surface) |
| — | CVE-2011-0990 (Array.FastCopy element-type race) | folds into S2(b) |
| — | JDK-8268347 (unswitched skeleton predicate misses a path) | folds into S3 (single-thread BCE; second-mutator arm is S2(a)) |
| — | RAB/GSAB length-tracking | already S4 |

### S10. Concurrent compiler thread reads mutable heap state (V8 ArrayShift analog)

The V8 bug: TurboFan's compiler thread reads `elements()` while the main
thread executes `Array.prototype.shift`, which moves the backing-store base
in place — the compiler dereferences a relocated pointer. The "second agent"
falsifying the proof is the *mutator*, and the proof-holder is the *compiler
thread*; the inverse of S5.

Our surface: every compiler-thread heap read that dereferences a butterfly or
captures a shape/value as a frozen compile-time fact —
`Graph::tryGetConstantProperty` (`dfg/DFGGraph.cpp:1329-1406`, the cellLock
bracket at `:1377-1382`), `Graph::freezeWithValidatedStructure`
(`:1667-1689`, the M5/M7 TOCTOU comment), `JSObject::getDirectConcurrently`
(`runtime/JSObject.h:2033-2042`), and the AS shift/unshift in-place relayout
that is V8's literal trigger (`runtime/JSArray.cpp:2223-2303,2551-2631`).

Verdict: **immune-by-construction.** Three independent closures:

1. **cellLock serializes against every base-relocating mutation.** The
   compiler thread takes `object->cellLock()` (`DFGGraph.cpp:1377`) before
   dereferencing. Every flag-on mutation that relocates or relabels named
   storage holds the same lock: §4.2 conversion step 2, §4.3 locked
   transition step 2, §4.6 AS-COPY (`JSArray.cpp:2240` `Locker locker {
   cellLock() }`), flatten (the upstream invariant the `:2016-2019` comment
   names). The lock-free mutations that *don't* take it — §4.4 64-bit
   `casButterfly` and the §3 lock-free SW DCAS — never rewrite the old
   storage (OM §4.6 AS-COPY "superseded storage never rewritten"; OM I7
   conservative scan keeps the old allocation live), so a stale pointer the
   compiler captured before its cellLock dereferences a frozen snapshot.
2. **The dereference itself is regime-safe.** `getDirect` →
   `locationForOffset` dispatches on the tagged word flag-on
   (`runtime/JSObject.h:980-999` →
   `locationForOutOfLineOffsetConcurrent`): a segmented word is decoded as a
   spine, never type-confused as a flat IndexingHeader. The structure
   re-check under the lock (`DFGGraph.cpp:1378-1380`) bounds the offset
   against the live shape.
3. **The captured value is watchpoint-funneled.** The constant is only
   used if `propertyReplacementWatchpointSet(offset)` is valid+watched
   (`:1343-1350`), and S5's `reallyAdd` revalidation closes the
   read→install window. `freezeWithValidatedStructure` (`:1667-1689`)
   carries the caller's validated `{value, structure}` snapshot precisely
   so the freeze re-decode cannot TOCTOU against an N-mutator transition
   (the in-tree comment is the closure record).

Adversarial residue: closure (1) assumes the compiler thread can *acquire*
the cellLock without participating in safepoints. It can: cellLock is the
2-bit IndexingType spinlock (rank 10, leaf; jit §7), the compiler thread is
not an entered mutator, and STW is cooperative (jit R1.f) — a Class-A stop
waits on entered mutators only, so the compiler holding a cellLock cannot
deadlock the stop. The compiler thread *is* enumerated by `JITWorklist`
safepoints (`DFGPlan::isInSafepoint`, asserted at `:1678`), which serialize
against GC, not against mutator transitions.

### S11. GC write-barrier elision vs N-mutator collection advance (JDK-8007898 analog)

JDK-8007898: C2 eliminates a memory barrier it proved redundant; an
uncovered path makes the proof unsound. Our barrier-elision surface is
`DFGStoreBarrierInsertionPhase`: it proves "base was allocated/barriered at
epoch *e*; no GC point since *e*; therefore elide the StoreBarrier"
(`dfg/DFGStoreBarrierInsertionPhase.cpp:65,403-406`). Under N mutators, the
falsifier would be: another mutator triggers a collection (or the concurrent
marker advances `m_barrierThreshold`) at a poll *between* the proof and the
elided store, ageing the base past the threshold — the elided barrier then
drops an old→new edge.

Verdict: **immune-by-construction.** The phase resets its epoch at every
node for which `doesGC` is true (`:403` `m_currentEpoch.bump()`); `CheckTraps`
— the *only* point at which compiled code can yield to a JSThreads or
GC-conducted stop (jit I21 / R1.f cooperative-only) — has `doesGC = true`
(`dfg/DFGDoesGC.cpp:555-558`). So no barrier is ever elided across a poll;
the proof's lifetime is exactly one poll-free window, inside which no other
mutator can advance the collector against this thread (heap §10.4 barrier:
collection windows open/close only with all clients NoAccess). The
concurrent-mark side (SPEC-congc §5.3 F19): GIL-off C1+ pins the server
master `mutatorShouldBeFenced` always-true until the per-client A16 reroute
lands, so the elision-vs-threshold race is moot — every emitted barrier is
the fenced form. `InvalidationPoint` is `doesGC = false`
(`DFGDoesGC.cpp:181`), but invalidation points are not GC entry points
(§5.3: jettison runs world-stopped; resume reaches the next `CheckTraps`
before any GC-visible store).

### S12. Lock-pairing elision

HotSpot proves a `monitorenter`/`monitorexit` pair balanced and elides or
coarsens it; a second thread (or an uncovered path) breaks the pairing.

Verdict: **immune-by-construction — no surface exists.** The JIT never emits
a lock acquire/release pair as IR it could prove on: cell-lock,
Structure-lock, and the §4.6 AS-COPY bracket are runtime C++ only (jit §5.5:
"the JIT never implements transition semantics"; AS-rule (b) "SW=1 → R3
locked ops" tail-calls `operationSharedArrayStorage*`, an opaque C call).
There is no DFG/B3 node representing "I hold lock L", so no pass can
strength-reduce one. The closest analog — D9's "never elide the fused TID
compare on writes" — is the *refusal* to let the compiler prove away the
sole F1 detection point, recorded in S1 link 5.

---

## Test inventory (all under `JSTests/threads/cve/`, EXECUTE POST-UNGIL)

| Test | Surface | Oracle |
|---|---|---|
| `mc-jit-stale-base-grow-oob.js` | S2(a)/S3 | ASAN/heap-validation crash; value-domain check |
| `mc-jit-double-relabel-stale-shape.js` | S2(b) | fakeobj/typeof-garbage check on every slot |
| `mc-jit-delete-reuse-stale-offset.js` | S2(c) | OM I21 no-cross-property-aliasing check |
| `mc-jit-ta-resize-hoisted-base.js` | S4 | ASAN crash pre-quarantine; stale-or-fail post |
