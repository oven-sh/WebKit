# SPEC-nativeaffinity — per-NativeExecutable concurrency bit + native serial lock

Status: DRAFT rev 11 (2026-06-10; review rounds 1-7 applied — history
PART B; round 7 = the SPEC-congc cross-document pass; rev 9 = the
congc §13.5 gate-closure record, no review round — history PART B
r9; rev 10 = EX1 cite restore + ungil-r34 coordination — history
r10; rev 11 = BL1.8 reacquire re-pin — history r11). Frozen-spec
conventions per the
SPEC-{heap,vmstate,objectmodel,jit,api,ungil}.md family: normative
clauses cite `file:line` (tree at branch jarred/threads as of this
rev) or a `SPEC-* §`; FULL-text overflow lives in
`docs/threads/SPEC-nativeaffinity-history.md` PART A as BINDING
annexes (NL1/PT1/EX1/AT1/BL1/SC1/SC2/SC3/ACQ1/TC1/CF1/EM1); supersessions are recorded BOTH
sides — where the other side is a frozen spec this document does not
own, the row is marked SUPERSESSION-PENDING and is an ADOPTION GATE
(§9), not yet in force. Invariants NA-I*, test charters NA-T*,
extensions NA-X*. Size cap 50000 bytes; history file uncapped.

Reading order: SPEC-ungil.md §A.3, §LK, §E.1b.4/U-T8e, §K/§N +
audits K4/N7, UNGIL-HANDOUT.md §A.1.3, then this file.

## 0. Scope, motivation, threat model

0.1 PROBLEM. GIL removal (SPEC-ungil) makes JS-visible engine
state safe under N mutators via §K, §N, §A.3. Host/native
functions are a long tail those audits cannot close: every
`JSC_DEFINE_HOST_FUNCTION` body, static-table getter thunk, and
embedder (Bun) native is arbitrary C++ running today with the GIL
as its implicit serializer; K4 covers VM-resident state, not the
BODIES of thousands of natives.

0.2 DESIGN (Jarred's proposal). A defense-in-depth RATCHET:

1. Every `NativeExecutable` carries a **concurrent-ok bit**. Bit
   set => audited safe on N threads. Bit clear => calls serialize
   on one per-VM **native serial lock** ("NL", §3).
2. Default policy: a small audited hot core ships concurrent-ok;
   the long tail and ALL Intl/ICU ship locked (§2).
3. Bits move Locked->ConcurrentOk ONLY with TSAN + fuzzer evidence
   in an audit table (§5); the reverse flip needs no evidence.

0.3 The bit is NOT a security boundary; it is an engineering
ratchet converting "all natives proven re-entrant before GIL-off"
into "un-audited natives are correct-but-serial, audited natives
parallel". "Serial" covers NativeExecutable-backed natives plus
the rev-6 bracketed families (NA-I26/I27/I28) — raw
GlobalObjectMethodTable hooks get NO NL coverage (NA-I24, §6) —
and means serial across ALL JS-executing threads of the gilOff VM,
carriers included (NA-I9). Serial cost paid ONLY on gilOff-VM threads calling
locked natives; other configurations per NA-I1 (§7).

0.4 The bit COMPLEMENTS the U-T8e hook dispositions
(SPEC-ungil:461-463, §6) and the §K/§N rulings: a native whose
shared touches are ruled by N7 rows still defaults to locked until
ITS BODY is audited (§2.3).

## 1. The concurrent-ok bit

### 1.1 Carrier: NativeExecutable (function identity)

The bit lives on `NativeExecutable`
(runtime/NativeExecutable.h:33-105), NOT on a property descriptor
or `PropertyAttribute`: concurrency safety is a property of the
FUNCTION IDENTITY (the same callable is reachable via
`.call`/`.apply`, bind products, cross-thread-published
`JSFunction`s — all bypass a property-level attribute). `NativeExecutable` is the identity object for the
`m_function`/`m_constructor` pair (NativeExecutable.h:98-99),
shared by all `JSFunction`s minted over it (JSFunction.cpp:100 ->
VM::getHostFunction). LIMIT (rev 5): for dispatch TRAMPOLINES the
pointer does not determine the body — §1.5/NA-I25.

### 1.2 Storage and JIT addressability

NA-I2 (storage). A dedicated byte `uint8_t m_concurrentOk` is
appended to `NativeExecutable` adjacent to the 2-bit
`m_implementationVisibility` field (NativeExecutable.h:101). It
MUST be a whole, independently addressable byte — emitted code does
`load8 + branch` (§4). A `static constexpr ptrdiff_t
offsetOfConcurrentOk()` is added beside `offsetOfNativeFunctionFor`
(NativeExecutable.h:63-69); the LLIntOffsetsExtractor friend (:35)
already exposes the class to the asm layer.

NA-I3 (immutability). `m_concurrentOk` is written EXACTLY ONCE, in
the `NativeExecutable` constructor (NativeExecutable.cpp:76),
before `finishCreation` (:61) publishes the cell; never mutated.
"Ungating" (§5) is a SOURCE change to the creation-site policy
input. Consequences: readers need no fences beyond cell
publication (SPEC-objectmodel safe-publication); DFG/FTL MAY
constant-fold the bit for a known CallVariant, no watchpoint
(§4.4).

NA-I4 (one bit covers both kinds). The single bit governs BOTH
`m_function` and `m_constructor` (NativeExecutable.h:55-61): most
constructors are `callHostFunctionAsConstructor` (JSFunction.cpp:
52), whose body only throws, so the bit is effectively the
call-path bit; an executable whose REAL constructor is not audited
MUST stay locked even if its call path is audited. Two bits =
NA-X2, post-v1.

### 1.3 Policy key and consistency

`JITThunks::hostFunctionStub` interns `NativeExecutable`s in a weak
map keyed by `HostFunctionKey` (function, constructor, visibility,
NAME — jit/JITThunks.h:224). NA-I5 (REKEYED rev 5, R4.2/R4.6 —
FULL text + defect walk = ANNEX CF1.2, BINDING): the POLICY key is
the `(m_function, m_constructor)` TaggedNativeFunction PAIR, NOT
the full HostFunctionKey — policy is a claim about the BODY, so it
is name-independent (rev-4 defects walked in CF1.2). The bit MUST be a deterministic
function of the pair; conflicting policy for one pair
RELEASE_ASSERTs (debug + release) — registration-order-, alias-,
GC-timing- AND cache-state-independent (rev 7, R6.1).

Enforcement structure (rev 2, R1.4; REKEYED rev 5; RE-PINNED rev 7,
R6.1 — FULL defect walk = ANNEX SC3.1, BINDING: the consult never
ran on a warm JITThunks cache HIT, JITThunks.cpp:262-267): a per-VM STRONG,
append-only `HashMap<PolicyKey, NativeConcurrency>` (bounded by
distinct code-address pairs) written on first registration,
consulted by EVERY REGISTRATION CALL — at funnel ENTRY of
`VM::getHostFunction` (VM.cpp:1429) / `JITThunks::hostFunctionStub`
(JITThunks.cpp:253, BEFORE the :260 lookup Locker, hit or miss) and
at each exempt-cited direct site; the validated NativeConcurrency
flows into create as a plain argument, miss path only. The
JITThunks weak map is NOT the enforcement structure
(JITThunks.cpp:262-268; no-JIT arm has no map, VM.cpp:1440-1442).
MODE DISPOSITION (rev 5, R4.8): active in EVERY configuration; cost
= one leaf-lock consult per REGISTRATION CALL (cold), declared
NA-I1. LOCK ROW (rev 6, R5.7; FULL discipline = ANNEX SC2.6,
BINDING; SC2.6(a)'s before-create pinning superseded by SC3.1, leaf
rank + holding discipline unchanged): NEW process lock — TRUE LEAF,
§LK.7 row required (gate §9.7, SUPERSESSION-PENDING); consult runs
OUTSIDE JITThunks::m_lock (Lockers JITThunks.cpp:260/:284) and
OUTSIDE the ctor; nothing acquired/allocated/parked under it; never
held across create or GC. NA-T8: JITThunks + no-JIT +
collected-then-re-registered + alias conflicts + rev-7 WARM-HIT arm
(assert fires on a live-cache hit, no GC; SC3.1(e)).

### 1.4 What "concurrent-ok" asserts (audit obligation)

A bit set to 1 asserts, with §5 evidence: for every reachable path of
the native's body (call AND construct kinds, NA-I4),
1. every touched VM-/global-/process-resident mutable datum is ruled
   by a K4/N7/NA row (per-lite, lock, lazy-publish,
   immutable-after-init, already-safe — SPEC-ungil-audit-K4.md
   classification key);
2. every touched cell-internal datum follows SPEC-ungil §N /
   SPEC-objectmodel rules (cell lock, lock-free arm, rope rules);
3. it performs no thread-affine side effect (no TLS assumption beyond
   the lite, no main-thread-only OS API);
4. any JS re-entry it performs (valueOf/toString/getters/callbacks)
   is itself safe under N mutators — JS re-entry is governed by the
   core SPEC-ungil machinery, so this leg is usually free.

### 1.5 Dispatch trampolines (NEW rev 5, R4.3)

NA-I25. A NativeExecutable whose function is a DISPATCH TRAMPOLINE
(body determined by callee-cell or out-of-band state, not the
pointer) is PERMANENTLY Locked; NA1 rows for its pair may only
carry Locked-keep. In-tree instance: JSNativeStdFunction's
`runStdFunction` (runtime/JSNativeStdFunction.cpp:60-65) — the
semantic body is an OPEN SET, the §1.4 audit undischargeable.
ANNEX AT1 gains the mandatory "body closed over key?" field.
Per-cell affinity = NA-X6, post-v1. FULL text = ANNEX CF1.3,
BINDING.

## 2. Default policy

### 2.1 Policy input: every creation (funnels + exempt-cited direct sites)

(REVISED rev 5, R4.1; FULL text = ANNEX CF1.1, BINDING.)
`NativeExecutable::create` ITSELF carries the `NativeConcurrency
{ Locked, ConcurrentOk }` parameter, HARD DEFAULT Locked — NA-I6
covers "every creation": un-plumbed direct creators are
conservatively safe by construction. Sites:

- Policy funnels (plumb the registration site's input):
  `VM::getHostFunction` (VM.cpp:1376, :1429) /
  `JITThunks::hostFunctionStub` (JITThunks.cpp:248, :253, :311);
  concurrent-ok is always an explicit opt-in at the registration
  site (greppable, diffable, audit-citable).
- Static property tables (.lut.h via create_hash_table /
  runtime/Lookup.h lazy reification): the LUT attribute grammar gains
  a `ConcurrentOk` marker; absent marker => Locked. The reification
  path plumbs it into the funnel above (the marker is the unit the
  §5 audit flips).
- Macro/registration layer (NEW rev 6, R5.5 — NA-I30; FULL text =
  ANNEX SC2.5, BINDING; r9 compressed): a defaulted
  `NativeConcurrency` parameter (default Locked) threaded through
  putDirectNativeFunction/WithoutTransition + JSFunction::create
  into getHostFunction, plus trailing defaulted JSC_NATIVE_*
  macro arguments — the opt-in stays greppable at the prototype
  finishCreation call site.
- Direct site, EXEMPT-CITED: `WebAssemblyFunction::create`
  (wasm/js/WebAssemblyFunction.cpp:101) mints a deliberately
  NON-interned CallIC-identity clone of the :99 funneled base.
  Locked UNCONDITIONALLY; consults the NA-I5 table per-PAIR
  (CF1.1; DROP side = NA-I26). NA-T7 NINTH token family:
  `NativeExecutable::create(` callers.

### 2.2 Seed allowlist (index; FULL table = history ANNEX PT1, BINDING)

Ships concurrent-ok at v1, each group with an audit row (§5) before
the flag flips on:

- PT1.A property/object hot core (hasOwnProperty,
  propertyIsEnumerable, Object.keys fast paths, getPrototypeOf —
  OM/K4-audited bodies).
- PT1.B array hot core (butterfly/Structure paths ruled by
  SPEC-objectmodel).
- PT1.C string hot core (rope-safe; §N.2 ropes take no lock).
- PT1.D Math.* (pure value math; m_weakRandom per-lite, K4
  AUD1.K4/VIII.10).
- PT1.E JSON.parse/stringify (fresh-object puts; heap/OM-ruled).
- PT1.F Atomics.* (designed for concurrency; waiter list
  process-locked).

Everything else — INCLUDING every Intl/ICU native — ships Locked.

### 2.3 Intl/ICU: ALL locked, normatively

NA-I7: every native reachable from `Intl.*`, plus every non-Intl
native calling into ICU (Date locale paths, `localeCompare`,
`toLocaleString` family, locale case conversion), is Locked in v1,
NOT PT1-eligible. ICU carries library-global state out of audit
scope; deliberately REDUNDANT with N7-U6 (§0.4). Ungating any Intl
native requires the §5 row's ICU column per touched API.

### 2.4 Embedder API (Bun)

- Bun links JSC internally and registers natives through
  `VM::getHostFunction`/`JSFunction::create`: the §2.1 parameter IS
  the embedder API; Bun-side evidence lands in the same §5 table
  (rows NA1.E.*). REV 5 (R4.3): the opt-in exists ONLY for natives
  with DEDICATED function pointers — std-function-backed natives
  are permanently Locked per NA-I25 (CF1.3).
- The public C API (CORRECTED rev 6, R5.3 — rev-5's two false
  sentences superseded; FULL walk = ANNEX SC2.3):
  `JSObjectMakeFunctionWithCallback` creates `JSCallbackFunction`,
  `final : public InternalFunction` (API/JSCallbackFunction.h:37)
  — NO NativeExecutable; serialization = NA-I8's InternalFunction
  arm. The JSClassRef OBJECT face (JSCallbackObject) = §2.7/NA-I27;
  its call/construct faces = NA-I28 (§4.5). No C-API opt-in
  surface exists in v1. NA-X3: a `...WithCallbackAndConcurrency` C
  entry point, post-v1.

### 2.5 InternalFunction (no NativeExecutable)

`InternalFunction` constructors carry raw native pointers
(runtime/InternalFunction.h:68), bypassing `NativeExecutable`
(separate LLInt trampoline, LowLevelInterpreter64.asm:3221). v1
rule (NA-I8): on NL-eligible threads (§3.1) EVERY InternalFunction
native call takes NL — conservatively locked, no per-instance bit.
CAVEAT (rev 2, R1.5): scoped to calls that reach the
trampoline/C++ entry — DFG lowers constant-callee InternalFunction
constructions to call-free graph nodes
(DFGByteCodeParser.cpp:6080-6140); that set is
NA-I16(c)/NA-T6-governed (§4.4). NA-X1: mirror the byte onto
`InternalFunction` with the same policy funnel.

### 2.6 Custom accessors (CustomGetterSetter / static-table CustomValue)

NEW rev 2 (R1.6). `JSC_DEFINE_CUSTOM_GETTER`/`SETTER` bodies (105
sites in runtime/*.cpp alone) are RAW pointers — no
`NativeExecutable`; invoked from IC code and slow-path operations,
bypassing every §4 emitter (sites = SC1.4).

NA-I20 (v1 disposition — conservative, NA-I8-style; REGROUNDED
rev 3 R2.7, rev 4 R3.6; FULL enumeration + cites = ANNEX SC1.4,
BINDING; r9 compressed): on NL-eligible lites, every
custom-accessor invocation is NL-bracketed AT THE DISPATCH
FUNNEL — wrapping the typed `FunctionPtr` INVOCATION; bracket
form = NA-I31/ACQ1 (rev 7). Bracketed funnels (lint-pinned):
`PropertySlot::customGetter`'s `m_data.custom.getValue(...)`
(runtime/PropertySlot.cpp:36-48) and the put-side
`customSetter(...)` invocations (SC1.4). Tag/symbol set = FIVE
tags + TWO vmEntry symbols, ALL NA-T7 token families (SC1.4).
gilOff IC suppression: the FOUR custom AccessCase kinds are NOT
CREATED in gilOff mode (bytecode/Repatch.cpp:711-715/:1251); the
slow path reaches the bracketed funnels. DFG/FTL direct-call
nodes are SEPARATE (§4.4.3, NA-I23). NA-X4: static-table marker,
post-v1. Gate §9.2.

### 2.7 JSClassRef / JSCallbackObject embedder callbacks (NEW rev 6)

NA-I27 (R5.2 — major; FULL text + site enumeration = ANNEX SC2.2,
BINDING; r9 compressed). JSCallbackObject's ClassInfo-methodTable
overrides invoke raw JSClassRef embedder callbacks — arbitrary
client C reachable from a SPAWNED thread's plain property access
on a shared-heap callback object; no NativeExecutable, no §4/§2.6
surface. v1: on NL-eligible lites every JSClassRef-callback
INVOCATION EXPRESSION is NL-bracketed (bracket form = NA-I31/
ACQ1, rev 7); finalize EXEMPT-CITED (GC context, NA-I10). NA-T7
ELEVENTH token family. NA-I24 amended: exclusion covers
GlobalObjectMethodTable hooks ONLY. NA-X7: refusal alternative,
post-v1.

## 3. The native serial lock (NL)

### 3.1 Definition and eligibility

One `NativeSerialLock` per VM (the single `m_gilOff` VM,
UNGIL-HANDOUT U0b/U0c). It serializes the BODIES of locked natives
across ALL JS-executing threads of that VM — spawned and carrier
(rev 2, NA-I9).

NA-I9 (eligibility; REVISED rev 2, R1.1 — supersedes the rev-1
spawned-only rule). NL is acquired ONLY on threads whose lite has
the new L2 byte `VMLite::nativeLockEligible` set, computed ONCE at
lite registration, same append region and recipe as
`VMLite::gilOff` (runtime/VMLite.h:234-242):

    nativeLockEligible = vm.m_gilOff   // EVERY lite of the gilOff
                                       // VM: spawned AND carrier

Rationale (normative; FULL text = history round 1, R1.1; r9
compressed): GIL-off carriers run JS in PARALLEL with spawned
threads (SPEC-ungil §A.3.6) — a carrier-exempt NL falsifies §0.3;
carrier TIDs are NONZERO GIL-off, same NL1 loop. Flag-off and
GIL-on processes never reach the byte (level-0 discriminator,
§4.1); a GIL-on VM's lites in a gilOff process emit 0. Today's
instruction sequences execute unchanged in those modes (NA-I1).

### 3.2 Acquisition protocol (index; FULL pseudocode = history ANNEX NL1, BINDING)

NL is park-capable and safepoint-polling. Cited protocol:
SPEC-ungil §A.3.2/2b — NVS-ticket parks, post-wake polls before
re-acquiring access or running JS/JIT; conductor predicate per
§A.3.1-2/EXIT1.

1. Fast path: CAS acquire (owner = lite/tid + depth word). Reentrant
   per-thread via a per-lite `m_nativeLockDepth` (depth survives the
   §3.3 drops as a saved value, never counts across them).
2. Contended path: a §A.3-compliant PARK SITE, ordering NORMATIVELY
   IDENTICAL to ANNEX NL1 (rev 2, R1.2): poll-first; every wake
   re-polls before any CAS; acquire() never returns without a stop
   poll after its last park (§A.3.2b(ii)); a winning CAS re-polls
   and, if stopped, parks on the NVS ticket HOLDING NL (NA-I10);
   an NL-blocked thread never blocks a conductor nor resumes a
   host body inside a stop window. TRAP-CLASS SPLIT (rev 6, R5.6;
   NL1 rewritten): only STOP-CLASS traps park; a TERMINATION-ONLY
   trap (SPEC-ungil §A.2.4/TERM1) NEVER parks the loop — the
   acquisition COMPLETES (NA-I12), delivery at the §4 bracket
   (NA-T4 termination arm).
3. NA-I10 (conductor exclusion). §A.3 stop conductors, GC conductors
   (heap §10), and any heap rank 2-10b or api rank 1-3 holder NEVER
   ACQUIRE NL (§LK negative-edge style; holding-on-entry = the §3.5
   rev-5 conductor-HOLD clause). Consequence: a holder may be
   safepoint-stopped WHILE holding NL without deadlock.
4. Tokens KEPT while parked on NL; heap access kept at §A.3
   JSThreads stops, F8-MANDATORY-reverted at rule-8 GC stops
   (SPEC-ungil.md:289-298) — legal WITH NL held (rev 3, R2.6;
   NA-I13 §3.4).

### 3.3 Mandatory release around JS re-entry

THE load-bearing rule. If a locked native re-enters JS holding NL,
the entire JS callback graph — including locked natives on OTHER
threads waiting behind it — serializes on NL: the GIL regrows.

NA-I11 (drop rule). NL is NEVER held across JS execution. Every JS
re-entry from native code releases NL fully (depth saved) before
entering JS and re-acquires to the saved depth (via the §3.2
park-capable protocol) before control returns to the native frame.
Funnel (REVISED rev 3, R2.2): defined by CALLEE over the
WHOLE JS-entry symbol FAMILY — every caller of any
`vmEntryToJavaScript*` symbol of the llint/LLIntThunks.h
declaration block (:39 bare + :46-52 `With0..With6Arguments`; that
block NORMATIVELY defines the family — a future With7 cannot
silently escape) instantiates the drop scope. NA-T7's symbol list
is GENERATED from LLIntThunks.h, prefix-matched. Known caller set =
ANNEX EX1 items 1-8 (BINDING; load-bearing detail in EX1.3-4). FAMILY SCOPE (NEW rev 6, R5.1 — blocker): the
`vmEntryToJavaScript*` family closes ONLY the C++->JS boundary;
any OTHER native-code-to-JS channel needs its own NAMED drop
family. NA-I26: `vmEntryToWasm` callers (LLIntThunks.h:72; three
sites — WebAssemblyFunction.cpp:94, Interpreter.cpp:1316,
JSMicrotask.cpp:203) are the SECOND callee-defined family, EX1
site 9 / NA-T7 TWELFTH token family: each instantiates the drop
scope around the WHOLE wasm activation (wasm->JS imports ride
the wasmToJS stub, invisible to vmEntry-symbol greps; FULL walk
= ANNEX SC2.1, BINDING). The drop hook is a
RAII `NativeLockDropScope` keyed on the per-lite depth word
(`m_nativeLockDepth != 0` => save+release; destructor reacquires):
zero work when depth is 0. Mode gating (rev 4 R3.11; rev 5 R4.5;
FULL ctor forms + rationale = ANNEX EX1): the DEFAULT ctor takes
NO lite — level-0 `g_jscConfig.gilOffProcess` test FIRST, in-ctor
lite resolve gilOff-only; flag-off/GIL-on pay one predictable
branch per JS-entry funnel, zero TLS/lite/depth loads. EX1 site 8
ALONE uses the EXPLICIT-LITE form (THREAD's gilOff-VM lite,
BL1.2).

NA-I12 (exception safety of the drop). JSC propagates host
exceptions by RETURN + pending-exception state (per-lite GIL-off
`m_exception`), NOT by C++ unwinding through host frames. The
RAII destructor therefore runs on every exit path INCLUDING
exception-pending and MUST re-acquire even then, via the
park-capable §3.2 loop; a termination trap observed mid-reacquire
never bypasses re-acquisition — the frame above releases to depth
0 through the §4 brackets (cites: ANNEX EX1).

### 3.4 Blocking and heap-access transitions while holding NL

(REVISED rev 3, R2.1/R2.5/R2.6; index — FULL text = history ANNEX
BL1, BINDING.)

NA-I13. NL never held across a VOLUNTARY heap-access transition
(debug-assert `m_nativeLockDepth == 0`) or an indefinite block
another mutator must release. EXEMPT from the assert: (a) §J.3
park-site MANDATORY reverts — the rule-8 GC-stop F8 revert
(SPEC-ungil §A.3.8; r35 anchor :314-323) is legal WITH NL held at
an NL1 stop poll (BL1.1); (b) rev 5, R4.7 — NARROWED rev 8, R7.1 (BL1.6's
sync-collection leg, derived vs the landed SINGLE-window
conduct, is FALSIFIED by the SPEC-congc §3 window model;
superseded BOTH sides, congc CGD6.1): the HBT4
conductor-bracket transitions — release-access -> arbitration ->
GCL, incl. the §LK.4b loser park access-released — ONLY when the
thread is a §K.5/heap-rule §A.3 (single-window,
haveABadTime-class) conductor or arbitration loser (FULL walk =
BL1.6). A SYNC-COLLECTION requester (GC conduct, follower ticket
park, or F28 successor — an N-window tenure under congc) instead
instantiates the BL1.8 NL DROP SCOPE: NA-I11-style depth-saved
FULL release BEFORE the request funnel
(election/follower-park/conduct), park-capable §3.2 reacquire
[r11] after the funnel's CALLER-SIDE GCL release, holding NO
heap rank >= 2 lock (= NA-I10; BL1.8 items 2/7) / follower
resume; congc CG-I19 carries
the matching `m_nativeLockDepth == 0` conducting-entry assert
(F40 there). VOLUNTARY native-side transitions remain forbidden
and asserted. [r9] congc §13.5(4) GATE CLOSED: the BL1.8/CG-I19
supersession reads RECORDED-BOTH-SIDES (this side BL1 [r9] note;
congc side CG-I19/CGD6.1/§13.5(4), back-cite congc ANNEX
CGS2.2-2.3 NL terms; ungil side SPEC-ungil rev 33 §LK row 9d
consumes the edge removal). The CGS2.3 wait budget this clause
leans on is now STRUCTURAL (congc F45, §9.1(2a)/CG-I26 fairness),
landed ungil-side in SPEC-ungil §A.3 rule 5 [r33].

NA-I21 (cross-VM nesting, R2.1; BL1.2). The §F.5 nested foreign-VM
entry funnel (carrier-only, §F.6(e)) instantiates
`NativeLockDropScope` keyed on the THREAD's gilOff-VM lite at its
F8-revert point (inside VM B no §3.3 caller can release VM A's
NL), reacquiring at LIFO restore; RELEASE_ASSERT depth==0 in the
nested window. GATE §9.6.

NA-I22 (engine blocking natives, R2.5; FULL = BL1.3). G11
(`join()`, `lock.hold()`, `cond.wait()`) + the Lock/Condition/
Thread/ThreadLocal family are seeded ConcurrentOk as ANNEX PT1.G
(NA1 rows still required): they never hold NL; demotion to Locked
requires internal drop scopes (BL1.3).

Embedder natives that block (Bun I/O) wrap the blocking region in
the public `NativeLockDropScope` (§3.3). LIVENESS SCOPE (BL1.5): NA-I10 = CONDUCTOR
liveness only; MUTATOR liveness = NA-I11 + NA-I21 + NA-I22 + §3.5
negative edge, each with a constructible deadlock if violated.

### 3.5 Rank — proposed §LK row (ADOPTION GATE §9; index — FULL
### row = ANNEX BL1.7, BINDING; MOVED rev 7 under the size cap)

Proposed SPEC-ungil §LK row **LK.1c "NativeSerialLock"**
(SPEC-ungil §LK; r35 anchor :837-917): inner to heap rank 1;
OUTER to api ranks 1-3, heap ranks 2-10b (range VERBATIM, rev 4
R3.7) and all leaves.
LONG-HOLD; acyclicity by the NA-I10 negative edge; §E.2 exemption
LIMITED to §J.3 MANDATORY F8 reverts (NA-I13; U20 gains the
matching exemption); conductor-HOLD clause (rev 5, R4.7; BL1.6 —
scope NARROWED rev 8 to §A.3 conducts ONLY, GC sync-collection
conduct = the BL1.8 drop, congc CG-I19 both sides) + §LK.4b
held-with amendment in the §9.1 scope. Pinned:
**NLS::m_lock OUTER to NL**, negative edge **no NL holder ever
blocks on NLS::m_lock or any G11 primitive** (NA-I22/PT1.G;
companion SPEC-api §5.9 row in the §9.1 scope). U20 extends to NL
edges. BOTH-SIDES RULE: any edge move is a supersession recorded
in BOTH specs; until the SPEC-ungil owner lands the cross-cite the
row is SUPERSESSION-PENDING and §3 implementation MUST NOT begin
(§9).

## 4. Host-call check shape per tier

The check follows the established two-level gilOff discriminator
(UNGIL-HANDOUT §A.1.3: process-level `JSCConfig::gilOffProcess` byte,
then per-lite byte) and the in-tree mode-split pattern
(`vm.gilOff()` C++ arms, ThunkGenerators.cpp:482-491; LLInt
`branchIfGilOffGroup3To*` macros,
llint/LowLevelInterpreter.asm:497-636).

### 4.1 Common shape (normative)

On the gilOff-mode-split path only:

    if lite->nativeLockEligible        // L2 byte, §3.1; set on EVERY
                                       // lite of the gilOff VM (rev 2)
        if !executable->m_concurrentOk // §1.2 byte
            operationAcquireNativeSerialLock(lite)   // §3.2
    call host function
    if <acquired>                      // same two tests / saved flag
        operationReleaseNativeSerialLock(lite)
    <existing exception check>         // release BEFORE the
                                       // exception branch (NA-I12)

NA-I14 (placement). Release is emitted BEFORE the exception-check
branch on every emitter (the exception arm never returns to the
trampoline and would leak a held NL). The acquire/release
operations are C++ slow calls; no inline CAS in v1 (the locked
path is not the hot path).

NA-I31 (C++ funnel form; NEW rev 7, R6.3 — FULL helper + rationale
= ANNEX ACQ1, BINDING). Every C++ funnel instantiating "the §4.1
bracket as an inline C++ helper" (§2.6, §2.7, §4.5 incl. NA-I28)
uses the RAII `NativeLockBracket` (ACQ1): level-0
`g_jscConfig.gilOffProcess` gate FIRST, lite resolve on the gated
path only, then the lite byte and (byte-keyed form, §4.5
host-JSFunction arm only) the §1.2 byte; the destructor releases
at the invocation expression's scope close, BEFORE the funnel's
exception branch (NA-I14 by construction; NA-I12 discipline). The
§4.1 lite-byte-first pseudocode is the EMITTER shape ONLY;
transcribing it at a C++ funnel recurs the R4.5 eager-TLS-load
defect (EX1) on hotter paths.

NA-I15 (cost; rev 2; rev 4 R3.10; rev 5 NA-I1). GIL-on/flag-off
arms unchanged on the emitted paths. ALL threads of a gilOff VM —
carriers included — pay `load8+branch` of the lite byte plus the
executable-byte load on the already-mode-split gilOff path, and
take NL when Locked. GIL-on-VM lites in a gilOff process pay only
the lite-byte test. Same cost discipline as the existing mode
splits (jit R1.e EXTENSION pattern, UNGIL-HANDOUT:172-179).

### 4.2 LLInt (index; FULL text = history ANNEX EM1.1, BINDING)

`nativeCallTrampoline` (llint/LowLevelInterpreter64.asm:3161-3219):
extend the EXISTING gilOff `.liteStoreTopCallFrame` arm
(`branchIfGilOffGroup3ToT3`, :3175-3182) with the two §4.1 byte
loads; acquire cCall preserves a0-a2; release before the
`.checkLiteException` test (:3211) per NA-I14.
`internalFunctionCallTrampoline` (:3220) takes the NA-I8
unconditional arm (lite byte only).

### 4.3 Baseline/JIT thunk (index; FULL text = ANNEX EM1.2, BINDING)

`nativeForGenerator` (jit/ThunkGenerators.cpp:455-576): per-VM
cached thunk, level-0 split = the existing C++ `vm.gilOff()` branch
(:481-491, :551) — GIL-on VMs get an unchanged thunk. gilOff thunk:
two `load8`s, acquire `callOperation` on the locked path; release
before the `loadException` check (:535-536) per NA-I14, on BOTH the
JSFunction and InternalFunction arms; same shape construct-kind
(:460).

### 4.4 DFG/FTL

DFG/FTL calls to host functions land on the NativeExecutable's call
thunk (§4.3) — covered EXCEPT (rev 7, R6.6) the NA-I16 union below
(rev 4, R3.3) AND the isDirect CONSTANT-PATH emitter: for a known host callee with
intrinsic() == NoIntrinsic, DFG/FTL emit a DIRECT call of the
TaggedNativeFunction with NO thunk
(dfg/DFGSpeculativeJIT64.cpp:999-1062 — :1031 JITCage
vmEntryHostFunction arm, :1033 HostFunctionPtrTag arm;
ftl/FTLLowerDFGToB3.cpp:14749-14796 — :14787/:14789); governed by
§4.4.2 (FULL walk = ANNEX SC3.2, BINDING). Obligations:

1. NA-I16 (inline-bypass closure; REVISED rev 2 R1.7, EXTENDED
   rev 4 R3.3/R3.4). SIX surfaces execute a native's semantics
   without reaching any §4 bracket; each member of their UNION must
   be ConcurrentOk or have its bypass disabled in gilOff mode:
   a. `handleIntrinsicCall` (dfg/DFGByteCodeParser.cpp:2097, :2558)
      inlines selected natives without calling them.
   b. Specialized intrinsic call thunks (rev 2, R1.7):
      `thunkGeneratorForIntrinsic` (VM.cpp:1283, consumed :1435)
      installs the executable's CALL CODE; §4.3 bracket reached
      only via the fallback (SpecializedThunkJIT.h:173). RULE:
      gilOff getHostFunction passes nullptr generator unless
      ConcurrentOk. `BoundFunctionCallIntrinsic` (VM.cpp:1444-1458)
      not PT1-seeded: NA1 row or suppressed.
   c. DFG constant-InternalFunction lowering
      (dfg/DFGByteCodeParser.cpp:6080-6140; §2.5 caveat).
   d. DOMJIT signature dispatch (rev 4, R3.3; FULL text = ANNEX
      SC1.1, BINDING; r9 compressed): `handleDOMJITCall` emits
      CallDOM — direct, NOT an Intrinsic. RULE (mirrors b):
      gilOff getHostFunction passes nullptr signature unless
      ConcurrentOk. $vm signatures PT1-LOCKED. NA-I23's
      CallDOMGetter row does NOT cover CallDOM.
   e./f. Intrinsic GETTER inlining, DFG + IC arms (rev 4, R3.4;
      FULL text = ANNEX SC1.2): `handleIntrinsicGetter` and
      `IntrinsicGetterAccessCase`/`emitIntrinsicGetter` execute
      getter-native semantics with no call; closed by NA-I29.
   NA-I29 (NEW rev 6, R5.4 — the b/d nullptr RULEs do NOT close
   (a)/(e)/(f); FULL text = ANNEX SC2.4, BINDING; predicate
   RE-TYPED rev 7, R6.2 per the SC2.4 rev-7 note; r9
   compressed): in gilOff configs, handleIntrinsicCall,
   handleIntrinsicGetter and canEmitIntrinsicGetter BAIL IFF the
   callee executable isHostFunction() AND its m_concurrentOk
   byte is clear (constant-foldable, NA-I3).
   ScriptExecutable-carried intrinsics are OUT OF SCOPE and
   remain admitted; the b/d suppressions remain for the
   thunk/CallDOM arms.
   Lint NA-T6 over the union sets (enumerated TC1); rev 6: the
   three NA-I29 guards exist and fire (SC2.4 witness) — a
   locked-but-bypassed member is a build error in gilOff test
   configs.
2. Constant-path bracket (§4.4.2; NORMATIVE rev 7, R6.6 — FULL
   walk = ANNEX SC3.2, BINDING): the isDirect emitter MUST emit the §4.1 bracket around the direct
   native call. Callee known, bit immutable (NA-I3): bit=1 => omit
   the bracket; bit=0 => unconditional acquire/release, release
   BEFORE the loadException check
   (DFGSpeculativeJIT64.cpp:1041-1043 /
   FTLLowerDFGToB3.cpp:14794-14796) per NA-I14. TC1's
   vmEntryHostFunction exempt is SPLIT per caller (SC3.2):
   ThunkGenerators.cpp:510/:518 sit inside the §4.3 bracket; the
   :1031/:14787 callers sit inside THIS bracket; the symbol joins
   the NA-T7 generated check.
3. NA-I23 (DFG/FTL direct custom-accessor calls; rev 3, R2.8).
   `CallCustomAccessorGetter`/`Setter`/`CallDOMGetter` lower to
   DIRECT calls of the retagged accessor pointer (built at
   DFGByteCodeParser.cpp:6647/:7076/:5559; lowering cites =
   history R2.8). v1: the gilOff parser DOES NOT BUILD these nodes
   (the access compiles as generic get/put reaching the §2.6
   funnels) — locally verifiable, lint-pinned (NA-T6 gains the
   three node kinds). Lowering files NA-T7-exempt WITH the NA-T6
   cross-ref.

### 4.5 C++ direct entry

The caller set of `vmEntryToNative` is DEFINED by the NA-T7
vmEntryToNative token family (rev 4, R3.2). Known members, THREE
sites, each gaining the NA-I31/ACQ1 bracket (rev 7; byte-keyed
form where the callee is a host JSFunction):
`Interpreter::executeCall` /
`executeConstruct` native arms (interpreter/Interpreter.cpp:1320,
:1409) AND the runJSMicrotask native arm (runtime/JSMicrotask.cpp
:206, CallData::Type::Native — reachable from plain JS on a
spawned thread via queueMicrotask(nativeFn)). HELPER POLICY ARM
(rev 6, R5.3; SC2.3): callee is a JSFunction host function =>
read the §1.2 byte; otherwise => Locked UNCONDITIONALLY
(InternalFunction = NA-I8; everything else = NA-I28). The
adjacent wasm arms are NO LONGER exempt-cited (R5.1): every
`vmEntryToWasm` caller instantiates the NA-I26 drop scope (§3.3,
EX1.9, SC2.1).

NA-I28 (NEW rev 6, R5.3 — blocker; FULL text = ANNEX SC2.3,
BINDING): the TWO handleHostCall C++ dispatch funnels —
llint/LLIntSlowPaths.cpp `callData.native.function(...)`
:2222/:2243 and bytecode/RepatchInlines.h :96/:117 — are reached
ONLY by callables with NO NativeExecutable (ProxyObject
:644/:703, JSCallbackObject call/construct faces,
JSCallbackConstructor.cpp:75; dispatch walk = SC2.3). On
NL-eligible lites both funnels NL-bracket the invocation
UNCONDITIONALLY (no executable byte exists; NA-I31/ACQ1 default
form, rev 7; release before their exception checks, NA-I14). NA-T7 TENTH token family:
`native.function(` call sites. Byte-keyed relaxation = NA-X8.

### 4.6 Coverage closure

NA-I17 (REVISED rev 3 R2.7/R2.8; rev 4 R3.1-R3.4; rev 6
R5.1-R5.3). The COMPLETE set of surfaces where a native body's
semantics execute is: the four emitters/sites above (LLInt
trampolines, JIT thunk, DFG/FTL constant-path, C++ entry — §4.5
set DEFINED by the NA-T7 vmEntryToNative family) PLUS the §4.4.1
inline-bypass union a-f (NA-T6/NA-I29-governed) PLUS the §2.6
custom-accessor funnels (NA-I20) PLUS the §4.4.3 node family
(NA-I23) PLUS — rev 6 — the handleHostCall funnels (NA-I28), the
JSClassRef hook invocations (NA-I27, §2.7) and the vmEntryToWasm
callers (NA-I26, §3.3). Raw GlobalObjectMethodTable hooks remain
EXCLUDED (NA-I24, §6 — scope NARROWED rev 6: the exclusion covers
that family ONLY, per SC2.2). Any new host-call emitter or
inline-implementation surface MUST add the bracket, join NA-T6,
or refuse gilOff mode. Audit hook: `grep` charter in NA-T7 over
TWELVE token families (enumerated in ANNEX TC1 NA-T7, BINDING) —
each site bracketed or exempt-cited; rev 7: the
vmEntryHostFunction exempt is split PER CALLER and the symbol
joins the generated check (SC3.2). NA-T7 additionally greps callers
of the WHOLE `vmEntryToJavaScript*` family — symbol list GENERATED
from the llint/LLIntThunks.h:39-52 block, prefix-matched (§3.3,
R2.2) — for the NA-I11 drop-scope funnel.

## 5. Ungating process (the ratchet)

Extends the K4/N7 audit style (executed audit files, rows addressed
`<file>.<table>.<row>`, rows consumed verbatim).

5.1 Audit artifact: `docs/threads/SPEC-nativeaffinity-audit-NA1.md`
(created at first flip). Row `NA1.<group>.<n>`; the field set =
history ANNEX AT1, BINDING (natives/kinds/shared state/cell state/
JS re-entry/ICU/TSAN/fuzzer/disposition/revocations + the rev-5
"body closed over key?" field, NA-I25).

5.2 NA-I18 (flip discipline). A Locked->ConcurrentOk source change
MUST (a) cite its NA1 row, (b) land the row in the same change or
earlier, (c) carry BOTH evidence columns — TSAN alone is insufficient.
ConcurrentOk->Locked requires NO evidence and MAY land
immediately; the row gains a revocation note. PT1 seed groups NOT
exempt: each needs its NA1 rows executed before `useJSThreads`
ships default-on (bit-set earlier is legal behind the flag).

5.3 TSAN/fuzzer substrate: the existing TSAN no-JIT target
(docs/threads/TSAN.md) + race amplifier (docs/threads/AMPLIFIER.md)
+ the thread-fuzz rig; NA-T5 defines the per-flip campaign shape.

## 6. Interaction with U-T8e hook dispositions

SPEC-ungil §E.1b.4 / U-T8e (SPEC-ungil:461-463): every hook
JS-reachable on a spawned thread gets a disposition in {inline,
carrier-queued, refused, unreachable}. The bit COMPLEMENTS this:

- NA-I24 (rev 4, R3.1; FULL text + cites = ANNEX SC1.3, BINDING;
  scope NARROWED rev 6 per SC2.2): raw
  `globalObjectMethodTable`/host-hook pointers
  (GlobalObjectMethodTable.h:58-71) mint NO NativeExecutable and
  reach no §4 emitter, §2.6 funnel, or NA-T7 family: NO NL
  coverage; safety rests SOLELY on the U-T8e disposition audit
  (§0.4); an INLINE disposition MUST NOT be granted on the
  strength of NL — none exists for THIS family. The exclusion
  covers GlobalObjectMethodTable hooks ONLY (rev 6):
  ClassInfo-methodTable callbacks = NA-I27/§2.7. A hook
  implemented by REGISTERING a NativeExecutable-backed host
  function is covered normally.
- **carrier-queued** hooks (promiseRejectionTracker spawned
  events, SD15) run on a CARRIER at §F.1 drains as raw-pointer
  calls (VM.cpp:2265): NOT NL-serialized; safety = the SD15
  carrier-drain ordering. **refused / unreachable** hooks never
  execute on spawned threads; the bit is moot.
- NA-I19 (no laundering). A disposition MUST NOT be downgraded to
  inline ON THE STRENGTH OF NL: U-T8e dispositions encode ORDERING
  and IDENTITY requirements, not just data races. Changing one
  remains a SPEC-ungil-side supersession, both sides.

## 7. INV — numbered invariants (normative index)

- NA-I1  Per-surface cost contract (RESTATED rev 5/rev 7; TC1
         NA-T9(d)). LLInt: non-GILOFF_TLS ASM
         byte-identical; new bytes confined to
         branchIfGilOffGroup3* arms. GIL-on per-VM thunks
         byte-identical. C++: flag-off/GIL-on pay one predictable
         gilOffProcess branch per EX1/§2.6/§2.7/§4.5(incl. NA-I28)
         funnel in ALL builds (ACQ1/EX1 forms — zero
         TLS/lite/depth loads), plus one §1.3 leaf-lock consult
         per REGISTRATION CALL (cold; declared; rev 7).
         gilOff-VM threads pay §4.1 (NA-I15).
- NA-I2  Bit storage: whole `uint8_t` on NativeExecutable, JIT-
         addressable via `offsetOfConcurrentOk()` (§1.2).
- NA-I3  Bit immutable after ctor; flips are source changes (§1.2).
- NA-I4  One bit covers call + construct kinds (§1.2).
- NA-I5  Policy deterministic per (function, constructor) PAIR —
         not per HostFunctionKey (rev 5); conflicting registration
         RELEASE_ASSERTs — order-, alias-, GC-timing- and
         cache-state-independent; consult per REGISTRATION CALL at
         funnel entry, hit or miss (rev 7, SC3.1); table active in
         every configuration (§1.3).
- NA-I6  Default Locked at EVERY NativeExecutable creation — the
         parameter lives on `create` itself (rev 5); direct sites
         exempt-cited (§2.1).
- NA-I7  ALL Intl/ICU natives Locked in v1; ungating needs the ICU
         column (§2.3).
- NA-I8  InternalFunction natives unconditionally locked on eligible
         lites in v1 (§2.5).
- NA-I9  NL touched only by lites with `nativeLockEligible` =
         vm.m_gilOff — EVERY lite of the gilOff VM, carriers
         included (rev 2; §3.1).
- NA-I10 Conductors and heap-2..10b/api-1..3 holders never acquire
         NL (§3.2.3).
- NA-I11 NL fully released around every JS re-entry; reacquired to
         saved depth before native frame resumes (§3.3).
- NA-I12 Drop scope is exception-safe; reacquire happens on the
         exception-pending path too; a TERMINATION-ONLY trap never
         parks the acquire loop — acquisition COMPLETES, delivery
         at the §4 bracket (rev 6, R5.6; §3.2/NL1) (§3.3).
- NA-I13 NL never held across VOLUNTARY heap-access transitions /
         indefinite blocks; exempt: rule-8 F8 reverts (BL1.1) +
         §A.3 conductor/loser transitions ONLY (rev 5; NARROWED
         rev 8 — sync-collection conduct = the BL1.8 drop, congc
         CG-I19 both sides RECORDED [r9], §13.5(4) closed;
         §3.4/BL1.6/BL1.8).
- NA-I14 Emitters release NL before the post-call exception branch
         (§4.1).
- NA-I15 The check shape is load8+branch on the gilOff-mode-split
         path only, per the two-level discriminator (rev 2; §4.1).
- NA-I16 Every member of the inline-bypass UNION a-f is
         concurrent-ok or its bypass is gilOff-disabled (rev 4;
         §4.4; mechanism for a/e/f = NA-I29).
- NA-I17 The surface set (§4 emitters + §4.4.1 union a-f + §2.6
         custom accessors + §4.4.3 nodes + rev-6 NA-I26/I27/I28
         surfaces) is closed; GlobalObjectMethodTable hooks
         excluded by NA-I24; new surfaces must bracket, join
         NA-T6, or refuse (rev 4, rev 6; §4.6).
- NA-I18 Locked->ConcurrentOk only with NA1 row + TSAN + fuzzer
         evidence; reverse flips free (§5.2).
- NA-I19 The bit never launders a U-T8e disposition (§6).
- NA-I20 Custom accessors unconditionally Locked v1; bracket wraps
         the FunctionPtr invocation at the dispatch funnels; four
         AccessCase kinds not created gilOff; lint-covered (rev 4;
         §2.6/SC1.4).
- NA-I21 §F.5 nested-VM entry drops the thread's gilOff-VM NL at
         the F8-revert point + RELEASE_ASSERT depth==0; gate §9.6
         (rev 3; §3.4/BL1.2).
- NA-I22 Threads-API/G11 natives seeded ConcurrentOk (PT1.G); never
         Locked without internal drop scopes (rev 3; §3.4/BL1.3).
- NA-I23 DFG/FTL custom-accessor nodes not built in gilOff configs;
         NA-T6-pinned; does NOT cover CallDOM (rev 4; §4.4.3).
- NA-I24 Raw GlobalObjectMethodTable hook pointers (ONLY — scope
         narrowed rev 6) have NO NL coverage; safety = U-T8e audit
         alone; inline dispositions never granted on the strength
         of NL (rev 4; §6/SC1.3/SC2.2).
- NA-I25 Dispatch-trampoline executables (runStdFunction family)
         are PERMANENTLY Locked; AT1 "body closed over key?" field
         mandatory before any flip; ConcurrentOk opt-in only for
         dedicated function pointers (rev 5; §1.5/§2.4/CF1.3).
- NA-I26 vmEntryToWasm callers are the SECOND callee-defined
         drop-scope family: NL never held across a wasm activation
         or its wasm->JS imports (rev 6; §3.3/§4.5/EX1.9/SC2.1).
- NA-I27 JSClassRef/JSCallbackObject embedder-callback invocations
         NL-bracketed unconditionally; finalize exempt-cited
         (rev 6; §2.7/SC2.2).
- NA-I28 The handleHostCall funnels NL-bracket unconditionally;
         the §4.5 helper treats executable-less callees as Locked
         (rev 6; §4.5/SC2.3).
- NA-I29 gilOff intrinsic admission bails IFF the callee
         executable isHostFunction() AND its m_concurrentOk is
         clear; ScriptExecutable-carried (builtin-JS) intrinsics
         out of scope, still admitted (rev 7, R6.2); the §4.4.1b/d
         nullptr RULEs do not strip intrinsics (rev 6;
         §4.4.1/SC2.4).
- NA-I30 Third policy surface: defaulted NativeConcurrency through
         the putDirect/macro layer into getHostFunction (rev 6;
         §2.1/SC2.5).
- NA-I31 C++ funnels take the bracket via the ACQ1
         NativeLockBracket — process-byte gate first, lite resolve
         gated, release before the funnel's exception branch;
         §4.1's lite-first ordering is emitter-only (rev 7;
         §4.1/ACQ1).

## 8. T — test charter (index; FULL charters = history ANNEX TC1, BINDING)

(MOVED to ANNEX TC1 rev 4 under the size cap; the annex text is
normative and includes every rev 2-4 arm.)

- NA-T1 Serialization witness: Locked body, N spawned threads +
  carrier arm — zero overlap; bit=1 control overlaps under the
  amplifier; TSAN arm clean BECAUSE of NL.
- NA-T2 Tier coverage matrix: LLInt / thunk / DFG-FTL / C++ entry /
  microtask-native-callee (rev 4, R3.2) / InternalFunction `new` /
  construct kind — each cell serializes.
- NA-T3 Re-entry drop: blocking + throwing `toString` proves
  NA-I11/NA-I12; CachedCall, module, small-arity (With2),
  microtask-runner and rev-6 wasm arms (TC1).
- NA-T4 Conductor liveness vs NL holder/waiters within watchdog
  (rev 8: vs the congc CGS2.3 budget; [r9] budget STRUCTURAL per
  congc F45/§9.1(2a) — arm asserts, not samples);
  wake-mid-stop, lost-wakeup, multi-waiter, GC-stop-with-NL arms;
  rev 5 conductor-holds-NL arm (haveABadTime leg; sync-collection
  leg RE-ARMED rev 8 per BL1.8 — drop expected, not hold); rev 6
  termination-vs-NL-waiter arm; rev 8 multi-window arm (composes
  congc CG-T8 F40) (TC1).
- NA-T11 Nesting + G11 liveness: §F.5 nested-window depth 0;
  join-vs-Locked-exit completes; contended hold/wait — no NA-I13
  assert; rev 6: (d) wasm-import lock.hold deadlock witness
  (SC2.1).
- NA-T5 Flip campaign template per NA1 row (amplifier + TSAN no-JIT
  + ASAN JIT + thread-fuzz; evidence ids in the row, NA-I18).
- NA-T6 Inline-bypass lint over the §4.4.1 union a-f sets (FULL
  = TC1); rev 4: signature-bearing-executable + intrinsic-getter
  sets, NA-I23 node-kind check; rev 6: the three NA-I29 guards
  exist + fire (SC2.4); rev 7: builtin-JS-intrinsic control arm
  (no over-bail, R6.2).
- NA-T7 Surface-closure lint: TWELVE token families (§4.6; TC1),
  each site bracketed/exempt-cited; every `vmEntryToJavaScript*`
  caller instantiates the drop scope; symbol list GENERATED from
  LLIntThunks.h:39-52 with snapshot self-test (R2.2); rev 7:
  vmEntryHostFunction exempt split per caller, symbol joins the
  generated check (SC3.2).
- NA-T8 Policy-conflict RELEASE_ASSERT via the strong side table:
  JITThunks + no-JIT + collected-then-re-registered + rev 5
  same-body-different-name/visibility alias arm; rev 7 warm-hit
  arm (live-cache hit, no GC; SC3.1).
- NA-T9 Mode-cost oracle (RECHARTERED rev 4 R3.10, rev 5 R4.8,
  rev 7 R6.4; FULL = TC1): GIL-on thunk byte-compare; arm-level
  LLInt diff; ASM-artifact scope (c); C++ branch-count/TLS-load
  oracle over the FULL acquire+drop funnel set (EX1 + ACQ1).
- NA-T10 U-T8e non-interference: queued-hook ordering/identity
  unchanged; carrier NL only for Locked NativeExecutable bodies,
  NEVER for raw-pointer hook invocations (rev 4, NA-I24).

## 9. Adoption gates (this spec is NOT in force until all close)

1. SPEC-ungil §LK row LK.1c landed by the SPEC-ungil owner,
   both-sides (§3.5) — rev 3: NLS::m_lock > NL edge + negative
   edge + SPEC-api §5.9 companion row. rev 4: range 2-10b
   VERBATIM; the §E.2 rank-4 carve-out + U20 exemption; the
   §E.2-close depth==0 / ~VM m_word==0 asserts vs EXIT1.9. rev 5:
   conductor-HOLD clause + §LK.4b held-with amendment (BL1.6).
   BLOCKS §3 implementation. rev 8 (R7.1): the conductor-HOLD
   clause lands NARROWED (§A.3 conducts only; sync-collection =
   the BL1.8 drop, both sides with congc CG-I19/CGD6.1); the §LK
   delta is COORDINATED with congc §13.5(1)-(3) — watchdog
   budget stated ONCE (congc CGS2.3). [r9] congc §13.5(1)-(3)
   are CLOSED (SPEC-ungil rev 33: §LK rows 9c/9d + U20 ext, the
   rule-5/HBT4.5 amendment incl. the CGS2.3 budget + F43 strike,
   the HBT4 re-entry extension — anchors per the ungil r33
   ledger); LK.1c/§LK.4b/api-§5.9 REMAIN OPEN — this gate does
   NOT close; the row 9d chain walk (CGS2A.2) cites LK.1c as
   pending.
2. SPEC-jit gilOff-mode codegen note (jit R1.e family extension,
   both sides) covering BY NAME (rev 4, R3.9; rev 6 R5.4; rev 7
   R6.2/R6.6): (a) the §4.3/§4.4 bracket arms INCLUDING the
   isDirect constant-path emitters
   (DFGSpeculativeJIT64.cpp:999-1062 /
   FTLLowerDFGToB3.cpp:14749-14796, §4.4.2); (b) the §2.6 IC
   suppression (four AccessCase kinds, bytecode/Repatch.cpp);
   (c) the NA-I23 parser suppression; (d) the §4.4.1b/d
   generator/signature suppressions + (e) the NA-I29 admission
   guards (handleIntrinsicCall/handleIntrinsicGetter/
   canEmitIntrinsicGetter — predicate keyed on isHostFunction(),
   rev 7). BLOCKS §4 implementation.
3. SPEC-api notes (item (a) REWRITTEN rev 7, R6.7 — the rev-6
   gate still asked the api owner to land the falsified rev-5
   claim): (a) the public C API mints NO NativeExecutable in v1 —
   JSCallbackFunction serializes via NA-I8, JSCallbackObject
   call/construct faces via NA-I28, JSClassRef object hooks via
   NA-I27, finalize exempt-cited; no C-API concurrency opt-in
   surface exists (NA-X3 post-v1) (§2.4/SC2.3);
   (b) rev 3: threads-API natives are PT1.G ConcurrentOk (NA-I22) —
   api-owner ack that their lock discipline is the §5.9-audited set.
4. VMLite L2 append for BOTH NL1 fields — `nativeLockEligible` AND
   `uint32_t m_nativeLockDepth` (rev 4, R3.9: rev 3's gate silently
   dropped the depth word the rev-1 open-item list named) —
   ratified against the vmstate L1/L2 append-only layout rule
   (VMLite.h:234 "Append-only per L1/L2; nothing above moves").
5. Adversarial review loop to a clean pass per the family convention;
   review log -> history PART B.
6. (rev 3, R2.1) SPEC-ungil/UNGIL-HANDOUT owner lands the §F.5
   nested-entry NL drop obligation (NA-I21) + §F.6 IU NL-depth row,
   both-sides. BLOCKS §3 in any VM-nesting embedder topology.
7. (rev 6, R5.7) SPEC-ungil owner lands the §LK.7 leaf row for the
   §1.3 policy-table lock (U20-linted), both-sides; discipline per
   ANNEX SC2.6. BLOCKS the §1.3 enforcement structure. (SC2.1's
   §I-refusal observation at the non-host-function vmEntryToWasm
   arms is recorded for the SPEC-ungil owner; not a gate here.)

## 10. History / annex index

`docs/threads/SPEC-nativeaffinity-history.md`:
- PART A (BINDING annexes): NL1 (NL protocol full text; rev-5
  conductor/loser paragraph; rev-6 trap-class split), PT1 (seed
  table incl. PT1.G; rev-6 registration header), EX1 (drop scope +
  caller list; rev-5 ctor forms; rev-6 site 9 wasm family +
  termination rewrite), BL1 (blocking/nesting/liveness), AT1 (NA1
  row template), SC1 (rev-4 surface closures), SC2 (rev-6: wasm
  channel, JSCallbackObject hooks, handleHostCall family,
  intrinsic admission, macro plumbing, policy-lock row), SC3
  (rev-7: registration-call consult, isDirect emitter), ACQ1
  (rev-7 acquire-side NativeLockBracket), TC1 (charters), CF1
  (creation closure / policy key / trampolines), EM1 (emitter
  full text).
- PART B (non-normative audit trail): revision log, review rounds.
