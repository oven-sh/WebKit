# AOT JSC — Design

## Executive summary

- Model: Tier L = the shipped jitless LLInt (full ES semantics incl. `eval`;
  the deopt floor). Tier A = DFG/FTL code compiled OFFLINE from whole-bundle
  STATIC analysis, no training runs; the §0.5 cost-modeled hybrid adds pruned
  variants LLInt's free profiling SELECTS by data writes — ICs without a JIT.
- Reused unchanged: parser→bytecode→DFG→B3→Air; LinkBuffer → object emitter;
  pointer constants via data tables; ICs/calls data-driven (§2.3 ledger).
- Size: KEEP ≈15.8 MB -O2; the <10 MB gate is AT RISK — only the optimistic
  corner passes; the measured -Os/LTO bridge + §4.2 levers gate freeze.
- Costs: §2.6 static losses; self-access ICs only; RegExp + cold builtins
  interpreter-class; bad speculation exits to LLInt — slow, never wrong.

**Status:** FROZEN draft 15 (round 12, fresh-eyes final; Annex O).
Cap 50,000 bytes (`wc -c`); overflow + change log: `AOT-DESIGN-history.md`
(BINDING annexes). Invariants `AOT-I*`, decisions `AOT-D*`;
REJECTED alternatives Annex A. Ground truth `docs/aot/AOT-SURVEY.md`
("survey"); file:line vs this tree (`jarred/threads`, 2026-06-10).

## 0. Provenance + constraints

**Clean-room note.** Filip Pizlo has an unpublished AOT-JSC design. We have NOT
seen it; nothing here derives from it — any resemblance is convergence on the
same public codebase. Inputs: this tree, the survey, public prior art only.

**Hard constraints (Jarred):**

- **C1 — No JS syntax fork.** Full ECMAScript semantics: `eval`,
  `new Function`, `with`, getters, proxies. No typed dialect, no static subset
  (vs Static Hermes/Porffor: §5.4).
- **C2 — Reuse DFG/FTL.** The existing pipeline IS the offline compiler. No
  new optimizer.
- **C2b — No training runs.** PRIMARY mode = STATIC analysis of the shipped
  BUNDLE alone (§2.1); a profile is NEVER required (AOT-I12; optional CI
  feed B.8). The architecture is cost-modeled, not fiat (§0.5/AOT-D0).
- **C3 — Product runtime < 10 MB** — the shipping binary, not the tool;
  measured budget table §4.
- **C4 — This doc ≤ 50,000 bytes**; overflow to the history file.

**Non-goals (recorded; full text O.4):** N1 not startup-only — ships
*optimized machine code*, no JIT. N2 not closed-world (GraalVM):
`eval`/dynamic loading legal, on Tier L. N3 not 100% coverage:
dynamic/declined code runs LLInt forever. N4 not a wasm design (product
wasm = IPInt + runtime, §4). N5 threads joint design chartered, not
solved (§6).

## 0.5 Architecture decision (AOT-D0): ARCH-S vs ARCH-L

C2b forbids fiat. Numbers: survey §3.5 (measured per-function sizes),
§1.10-1.11; derivation Annex F (BINDING) + G amendments.

**ARCH-S (static spine).** Whole-bundle analysis seeds DFG/FTL; ONE body per
function; generic-but-direct where silent. Modeled weakness: JS history says
static analysis underperforms profiling — §2.6 losses land permanently.

**ARCH-L (selection spine).** The runtime REMAINS a profiler: LLInt profiling
ships free in the jitless build (F.1: value/array profiles, metadata ICs, the
`checkSwitchToJIT` counter tick under `useJIT=0`, LLInt-resident only — C20).
The artifact ships a per-function SPECULATION LATTICE (generic →
shape-specialized variants); tier-up = SELECTION + LINKING by data writes,
never codegen; variants compile against SYMBOLIC constants in patchable pools
(precedent: the data-IC subsystem, F.1). Static analysis prunes the lattice;
deopt descends it before LLInt.

**Cost model:**

| Quantity | Value → consequence |
|---|---|
| Bytes/variant | FTL 2.7 KB mean/1.5 median, DFG 2.9/1.2, ~6.5-8 B/bc-op (survey §3.5); ramps measured 61% of FTL text — EXITMETA multiplies PER VARIANT |
| Guard cycles; link/patch | EQUAL in both (AOT-D12 table-load guards; binding = store-release per slot + entry publish, ns-scale, W^X-clean): bytes and speedup decide |
| Variant counts | symbolic slots collapse only the SHAPE axis per LAYOUT class (C15 — bodies BAKE offsets/inline-ness); REPRESENTATION axes compiled in ⇒ up to 2^k unpruned — pruning mandatory; pruned V=1/2/3 per §2.5(d) (Hopc, survey §4) |
| Artifact projections | ARCH-S: ≈1,800 × 2.9 KB ≈ 5.2 MB + comparable EXITMETA = 1× (§4.4 baseline); ARCH-L unpruned (V=3 floor) ⇒ 25-30 MB-class LINKED app `.text` — fails on bytes alone; hybrid (V=2 on silent-hot 20-40%): ≈1.15-1.4× (F.2) |
| Speedup, selection over static | ~0 where facts exist; 2-5×-class on silent sites — profiling's edge IS §2.6(1)-(2), CONFINED to silent code; overlay cost = C20 warm-up (G.6) |

**Decision (AOT-D0): hybrid — ARCH-S spine + pruned ARCH-L overlay.** Static
facts primary: bundle-closure wins (direct calls + inlining) are unrecoverable
by any selector. Overlay: a statically-silent-axis function MAY ship one extra
variant on UNBOUND symbolic slots; LLInt-RESIDENT after bind (C20) until the
counter trips; the SELECTOR validates the precondition row (incl. C15 layout
signature), binds slots, publishes variant-or-generic — inline caching without
a JIT, function-granular. Deopt descends variant → generic → §1.4. Mechanics
§1.2/§1.4/§2.5(d); soundness AOT-I13. (Rejected: A.)

**Epitaphs (recorded):** *Pure ARCH-S* — refuses free profiling; the 2-5×
silent-hot gap becomes permanent. *Pure ARCH-L* — V≥3 floor (25-30 MB app
`.text`); universal C20 warm-up; inlining/direct-call formation — the
largest static wins — unmakeable by a selector.

## 1. Execution model

### 1.1 Tiers in the product

Two execution tiers:

- **Tier L (LLInt):** the shipped jitless interpreter — full semantics, zero
  executable memory; `useJIT=0` never creates the executable allocator
  (`jit/ExecutableAllocator.cpp:160`); every entrypoint is the LLInt prologue
  (`llint/LLIntEntrypoint.cpp:60`). Runs: unbound, declined, dynamic,
  post-deopt code.
- **Tier A (AOT):** DFG/FTL machine code, produced *offline* by full-fat `jsc`
  from whole-bundle STATIC analysis (§2; C2b), shipped relocatable (§2.4), bound
  at load (§3). Speculation checks remain as the JIT emits them. Lattice
  functions (AOT-D0) carry one extra symbolic-slot variant (§1.2).

No runtime codegen (AOT-I2, §7). The image carries the CachedBytecode
payload (`runtime/CodeCache.cpp:423`) as level 0: no reparse of shipped sources.

**AOT-D15 — the bundle SOURCE TEXT ships** (SOURCE section §2.4; C8): CachedBytecode embeds no source (`runtime/CachedTypes.cpp:1610-1627`); the
text backs the SourceProvider, `toString` (charter 2), `Error.stack`, lazy
parsing of demoted/skipped functions. (Rejected: A.)

### 1.2 Dispatch: how a call reaches Tier A

**AOT-D10 — binding object model (two CodeBlocks; D-F4/C4).**
The LLInt CodeBlock is permanent: runs when unbound, and is the **exit target**
(`dfg/DFGOSRExitCompilerCommon.cpp:459-463`). Binding installs a thin AOT
CodeBlock (new `JITType::AOTJIT`; LLInt as `alternative()`): old-age jettison
off (`bytecode/CodeBlock.cpp:1485`); demotion = clear bit + uninstall, never
frees image memory; weak jettison (`:1478`) → §3.4 sweep. DFG data
structures decouple into `ENABLE(AOT_RUNTIME)` (§4.1; D);
compilers/assembler/allocator out. Bound only if the **validity bit** (§3.4)
is set; else plain LLInt (AOT-I4).

Entry is **function-granular** (call boundary). **AOT-D1: no OSR *entry* in
v1** (needs per-loop thunks + a frame-reconstruction contract); catch/loop
entrypoints (`dfg/DFGCommonData.h:130`) deferred; data ships.

**Overlay selection (AOT-D0; C20/C15; G.6).** Lattice functions bind but
DEFER entry install, staying LLInt-resident (selector inputs/trigger
LLInt-only, `llint/LowLevelInterpreter.asm:1830`). The counter is on the SHARED
UnlinkedCodeBlock — a trigger, not state: the MODIFIED `replace`/`loop_osr`
slow paths dispatch on the EXECUTING (function × realm) entry, never
`dontJITAnytimeSoon`, re-arming every trip (C42/J.3). Trip ⇒ SELECTOR checks
LLInt profiles vs the variant's STATICMETA precondition row incl. its
**layout signature** (C15: bodies BAKE layout,
`dfg/DFGSpeculativeJIT.cpp:15696`). Match ⇒ bind slots (store-release), publish; mismatch/poly ⇒
generic (sticky-with-decay); never-trip ⇒ LLInt. Spine-only functions install
at bind.

### 1.3 OSR exit without a JIT

Tier A keeps every speculation check DFG/FTL emits; a failed check exits to
Tier L — a tested mode: `adjustAndJumpToTarget` targets LLInt under
`forceOSRExitToLLInt()` (`dfg/DFGOSRExitCompilerCommon.cpp:426`);
inlined-frame return PCs select LLInt labels (`:148`); in the product ALL
exits target LLInt (AOT-I3). Gap (survey §1.5): ramps are lazily JIT-compiled
on first hit (`dfg/DFGOSRExit.cpp:270,391`); the product has no JIT.

**AOT-D2 (r2/r7): a ramp is a pure function of (recovery stream,
`SpeculationRecovery` undo record-or-none — C41/J.2; undo BEFORE value
recovery, `dfg/DFGOSRExit.cpp:391`; FTL/B3 carry none); everything else via
a per-site parameter block; `compileExit` MODIFIED; the C++ recovery
interpreter (undo first) = universal backstop.**
`compileExit`/`adjustAndJumpToTarget` bake per-VM/per-exit heap addresses
(per-immediate table Annex D/F3). The offline emitter reparameterizes each:
ramps load a per-site **parameter block** — true `.rodata`, REALM-INVARIANT
only (C25/H.1); realm-dependent inputs reached via the exiting frame's
per-realm AOT CodeBlock (C19/K.8). `handleExitCounts` → §1.4; recovery
cell constants → pool (M); LLInt labels stay link symbols (R). **Dedup keyed on the (pointer-parameterized
stream, undo record) PAIR**; the recovery interpreter (NEW,
small) covers size-pruned ramps and is the testing oracle (§7). Ramps write
NO profiles: the refinement block (`dfg/DFGOSRExit.cpp:444`,
`ftl/FTLOSRExitCompiler.cpp:408`) is DROPPED;
`m_jsValueSource`/`m_valueProfile` out of EXITMETA (C54/L.2;
F3(j)/G.8(g)). **FTL branch
(C22; audit G.8):** `compileStub` (`ftl/FTLOSRExitCompiler.cpp:222`) — same
treatment: parameterized ExitValue stream in EXITMETA, dedup over it,
interpreter covers materialization.
(Rejected: A.)
**Exit-site dispatch (C37/I.3):** in-tree sites lazily wire a generation
thunk (`dfg/DFGOSRExit.cpp:284-295`); AOT links the failure branch
statically to its dedup'd ramp; site identity = exit-index imm32;
parameter-block base class-R; thunks/`repatchJump`/`osrExitIndex` out.

**AOT-D11 — exceptions/unwinding/stack traces (r2).** *Exceptional* OSR exits
are taken by the unwinder, not a guard (`dfg/DFGOSRExitBase.h:59`);
handler lookup keys on CallSiteIndex (`interpreter/Interpreter.cpp:695`).
EXITMETA serializes codeOrigins pool, handler table, PC→CodeOrigin map; **v1
routes exceptional exits through the C++ recovery interpreter**;
`genericUnwind`/`StackVisitor` grow AOT-frame readers (NEW, §4.1; charter 3).

### 1.4 Post-exit policy (function-granular fallback)

After an exit the activation continues in LLInt; the *next call* is policy.
Per AOT CodeBlock (writable side table): `aotExitCounts[exitIndex]`;
`aotDemotionScore` (saturating, decayed); the validity bit (§3.4).

**Policy (AOT-D3):**

0. **Lattice descent (AOT-D0)** first: a selected-variant exit DESELECTS to
   LLInt-resident deferred-install (entry uninstall; slots poisoned; counter
   re-armed — C54/L.2); sound mid-activation (C16). LLInt re-profiling IS
   the re-selection evidence (C54); one re-trip, then sticky generic. Only
   generic-body exits feed demotion.
1. **Precondition-class exits** (watchpoint fired after load): clear the
   validity bit, **demote permanently** — the assumption is dead (§3.4).
2. **Speculation-class exits** (wrong type/structure, overflow): increment site
   counter + demotion score. **Re-enter Tier A on next call** while
   `aotDemotionScore < D`. One dominant site (`aotExitCounts[i] > S`) =
   per-call-polymorphic: demote permanently. Diffuse rare exits: keep
   re-entering — 0.1%-exit code still wins. D/S/decay are `Options` (initial
   D=8, S=128, halve per GC epoch — CI-tuned defaults, O.1; never a guess).
3. **Demotion is per-function, recorded, reportable** (function ⇒ site ⇒
   reason) for diagnostics and the optional CI feed (B.8); other functions keep
   their bindings.

Rationale: the only lever is "use the AOT code or don't". (Rejected: A.)

### 1.5 Dynamic code

`eval` / `new Function` / dynamic `import()` of unshipped sources: parsed and
run on Tier L; never Tier A — C1 holds.
Callees of dynamic code keep Tier-A bindings; `eval` inside an AOT function
= today's DFG `op_call_eval` slow path.

## 2. The offline compiler

### 2.1 Tool = full-fat jsc; input = the bundle

The AOT tool is full-fat `jsc` (all tiers) plus one mode: **`--aot-compile`** —
input the BUNDLE, output the artifact (§2.4). No record step (C2b); the tool
may be huge.

**Whole-program static analysis** (NEW, tool-only) runs over the bundle's
bytecode + module graph before any DFG plan ⇒ a per-site **static-fact pack**
(function identity = source URL + content hash + bytecode offset):

1. **Constructor shape inference.** `this.x=…; this.y=…` sequences statically
   determine shape and inline capacity;
   `op_create_this`/`op_new_object` carry a static `inlineCapacity` operand
   (`bytecode/BytecodeList.rb:541-558`; fork emits 0 at
   `bytecompiler/BytecodeGenerator.cpp:3225` — a vacant slot the tool fills).
   Output: recipes (§3.2) + structure-set facts for constructor-body `this`
   and proven `new C()` sites.
2. **Object/array literal shapes.** Syntactic; literal-derived heap data already
   serializes (`runtime/CachedTypes.cpp:1297`). Element kinds proven from visible
   writes for non-escaping arrays; escaping ⇒ conservative kinds.
3. **Module-graph constant + callee propagation — the CHA analog.** Bundle
   closed ⇒ once-assigned exports are constants; callee-unique sites provable
   at scale (Hopc, survey §4). Output:
   concrete callees (inlining, direct calls) + constants. **C17 (G.3):**
   module-scope direct `eval` poisons once-assignment proofs; the guarding
   `SymbolTable` watchpoints, never allocated jitless
   (`runtime/SymbolTable.h:238,320`), are forced ON. Each fact:
   scope-variable PRECOND entry or guarded CheckCell/CheckIsConstant whose
   comparand passed C40 identity at bind (J.1 — never the bare table-loaded
   value); post-bind reassignment fires-or-exits, never stale. **C47 (K.2):**
   edges resolving OUTSIDE the bundle (addons, `node:`/`bun:`,
   externals) are BOTTOM: never foldable, never a comparand;
   generic-but-direct.
4. **Universal watchpoint assumptions** — watchpoints, not profiles (all
   per-JSGlobalObject, hence per-realm — C19): `isHavingABadTime()`
   (`runtime/JSGlobalObject.h:1202`), arraySpecies (`:550`), saneChain (`:557`),
   intact builtin prototypes (ONLY via named sets or C40 facts — J.1). Tool
   assumes; load validates (§3.3).

Where analysis proves nothing, the pipeline emits **generic-but-direct** code:
every value-consuming DFG node has an `UntypedUse` lowering
(`dfg/DFGSpeculativeJIT.cpp:2925`; survey §1.9) — direct machine code, reached
via the AOT-D16 floor (§2.2). No profile is ever required (AOT-I12).

### 2.2 Seeding mechanics: where facts enter

Live profiles enter graph construction in three funnels: `getPrediction` reads
`ValueProfile` slots (`dfg/DFGByteCodeParser.cpp:983`); `getArrayMode` reads
`ArrayProfile` (`:1079`); inlining/access via Status objects (`:1463`).

**AOT-D4 (revised d5): three injection channels** (E.1). The
tool constructs the linked CodeBlock, then seeds before `DFG::Plan`:

1. **Slot injection**: type facts → ValueProfiles, element kinds →
   ArrayProfiles, monomorphic facts → LLInt metadata ICs (LLInt Status
   readers are MONOMORPHIC, `bytecode/CallLinkStatus.cpp:228`).
2. **Static-fact provider** (C9; scoped C61/N.1): set-valued facts cannot
   enter via slots; a tool-only provider returns synthesized Status objects
   at a CLOSED funnel list — CallLink (`dfg/DFGByteCodeParser.cpp:1463,1529`),
   GetBy (`:7483`), PutBy (`:8950`); every other Status kind is
   channel-1-only — §2.6(5). **C61:** a synthesized variant's
   `ObjectPropertyConditionSet` MUST come from the in-tree
   `generateConditionsFor*` generators
   (`bytecode/ObjectPropertyConditionSet.h:167-183`) run on the AOT-D18
   stand-in heap (computeFor parity, `bytecode/PutByStatus.cpp:111-117`);
   generator failure ⇒ no variant
   (AOT-D16 floor); empty set only where computeFor emits one. Sets flow
   into PRECOND (C28/C40) via parser `check()`
   (`dfg/DFGByteCodeParser.cpp:5460`); §3.3.3 revalidates live, fails closed.
3. **Prediction floor (AOT-D16, split C43/J.4).** Fact-less sites are NOT
   free: SpecNone ⇒ `ForceOSRExit` (`dfg/DFGByteCodeParser.cpp:1052`); empty
   ArrayProfiles ⇒ `Array::Unprofiled` ⇒ ForceExit (`dfg/DFGArrayMode.cpp:72`).
   (a) VALUES: a StaticFactAgent at the **FuzzerAgent override** (`:1001`)
   returns the fact or `SpecBytecodeTop`. (b) ARRAY modes (no agent seam):
   fact-less sites channel-1 SATURATED so `fromObserved` yields generic,
   never Unprofiled; merge-only (OR) updates preserve the seed (J.4). HONEST
   COST: Top propagates — generic regions broader than their fact-less sites
   (§2.6/§5.1).

Structure facts resolve against the tool VM's heap (§3.2/AOT-I6). (Rejected: A.)

### 2.3 Backend retarget: the LinkBuffer seam

Everything upstream of `LinkBuffer` is position-independent by construction:
B3→Air emits unresolved link records (`b3/air/AirGenerate.h:46`); bytes gain
addresses only in `LinkBuffer::copyCompactAndLinkCode`
(`assembler/LinkBuffer.cpp:290`). **The object emitter is an alternative
LinkBuffer** (`AOTObjectLinkBuffer`, NEW): same input, but (a) copies into
file-section byte vectors, (b) intra-function branches resolve normally, (c)
every *external* reference joins the relocation table.

**AOT-D12 — pointer constants never become machine immediates (corrected r2;
C3/A).** FTL `weakPointer` returns a plain B3 Int64 constant
(`ftl/FTLLowerDFGToB3.cpp:28143-28162`) ⇒ anonymous immediates (only
branches/calls/labels get link records). So the tool's DFG/FTL **lowering**
(MODIFIED, tool-only) routes every class-M/V pointer (§3.1) through a
**data-table load** (one per site per AOT-D5; §5.1.5) — making "B3/Air REUSED
UNCHANGED" *true*. **AOT-I5 (C11)**: no B3/assembler constant from
cell/`StructureID` classes outside the table-load path — full statement §7. **C21:** FTL `operation*`/thunk callees are anonymous B3
IntPtr constants (`ftl/FTLOutput.h:416`) — AOT lowering GOT-routes them at
`FTLOutput::operation`; AOT-I5 covers `OperationPtrTag`/`JSEntryPtrTag`.
(Rejected: A.)

**AOT-D18 (C23) — tool-side stand-in cells (NEW, tool-only; G.9).**
The tool never executes the bundle, yet freeze/inlining/Status machinery
dereferences live cells. A stand-in factory synthesizes FunctionExecutables
from Unlinked ones in a synthetic module scope; CallVariants built BY
EXECUTABLE; stand-ins enter RELOC/RECIPES as paths/ExecutableIDs, never
addresses (AOT-I9).

**Reuse ledger (per component).** REUSED UNCHANGED: parser, bytecompiler,
CachedBytecode; DFG phases, prediction propagation, B3, Air (C2 — true *because
of* AOT-D12); `LinkBuffer` (tool-internal); `Desired*`/`DFGCommonData` as
serialization *schema* (NEW serializers); `CallLinkInfo` (data-IC mode, §3.1 row
10); LLInt (less the C42 slow paths), GC, runtime/, WTF, bmalloc (jitless
base + AOT_RUNTIME deltas).
MODIFIED (full decorations O.5): `DFGByteCodeParser` (tool-only;
AOT-D16+C61/AOT-D4.2); DFG/FTL lowering (tool-only; AOT-D12);
`Graph::freeze` (AOT-D13); `OSRExit::compileExit` (AOT-D2); FTL
`compileStub` (C22); `FTLOutput::operation` (C21);
JITOperations/DFGOperations/Repatch (AOT_RUNTIME op set; `repatch*` →
data-IC writes; tier-up ops dropped; nm-audited AOT-I2);
`linkOSRExits` + FTL OSRExitHandle dispatch (tool-only; C37/I.3); LLInt
`replace`/`loop_osr` slow paths (C42/J.3); FTL `lazySlowPath` (C35/I.1
eager tool-time stubs, `ftl/FTLLazySlowPath.cpp:57`); `ENABLE(AOT_RUNTIME)`
decoupling (§4.1; piece list D; incl. C17 audit). NEW: static analyzer
+ fact injection + lattice pruner + stand-in cell factory (AOT-D18) (tool-only);
`AOTObjectLinkBuffer` (tool); structure recipes, precondition table,
loader/binder, selector, weak-table sweep (runtime); C++ exit-recovery
interpreter + unwinder/StackVisitor AOT-frame readers (runtime, AOT-D11).
`InlineCacheCompiler`: NOT in product; tool-only (AOT-D9). MathIC DISABLED
tool-side (C29/H.5; non-repatching ops `dfg/DFGSpeculativeJIT.cpp:5097`).

### 2.4 The artifact

One file (or linked blob, §3.5) per app; sections:

1. **HEADER** — engine build hash + format version + target triple + **ISA
   feature baseline (C58/M.1: tool never host-detects — pinned per-target;
   loader: baseline ⊆ host CPUID)** + option-flavor hash (§6) + page-hash
   tree (C33). Validation per `GenericCacheEntry::isUpToDate`
   (`runtime/CachedTypes.cpp:2586`);
   mismatch ⇒ ignored wholesale, Tier L; integrity is AOT-D19/C33.
2. **BYTECODE** — verbatim CachedBytecode payload (level 0) + **SOURCE** — the
   bundle text backing it (AOT-D15; may reference an app file by content hash).
3. **CODE** — compiled functions (incl. AOT-D0 variants) + dedup'd exit ramps +
   shared thunk bodies. Immutable; mapped PROT_READ|PROT_EXEC or linked (§3.5).
4. **EXITMETA** — per-function `OSRExit` descriptors, `Operands<ValueRecovery>`
   (DFG) + parameterized `ExitValue`/materialization (FTL, C22) streams,
   `SpeculationRecovery` undo records + recoveryIndex (DFG only — C41),
   inline-call-frame trees, codeOrigins pool, exception-handler table,
   PC→CodeOrigin map (`dfg/DFGCommonData.h:120-135`; AOT-D11), ramp index,
   parameter-block layouts (AOT-D2).
5. **RELOC** — (code offset, width/kind, target class §3.1, target id).
6. **PRECOND** — per-function serialized `DesiredWatchpoints` catalog entries
   (`ObjectPropertyCondition`s recipe-relative, global-property descriptors,
   view-detach sets, **scope-variable entries — C17**, **well-known per-global
   sets — C28, extended C59/M.2: full `LinkerIR::Type` list
   (`dfg/DFGJITCode.h:89-118`) + varInjection/varReadOnly + E1/E2**) +
   **C40 per-kind identity
   descriptors for frozen cells (J.1)**. **Closed rule (C28/H.6, amended
   C40):** every entry MUST serialize to a catalog descriptor; inexpressible ⇒
   re-emit guarded (C40-checked comparand) or decline — NEVER drop.
7. **RECIPES** — structure-recipe pool + atom table (dedup'd) + global/builtin path
   table + frozen-value constant pool (§3.2/§3.3).
8. **STATICMETA** — function identity map (§2.1) + IC seeds + variant
   precondition rows incl. **layout signatures** (C15; one per load-time recipe
   slot too, C27) + symbolic-slot layouts (AOT-D0) + demotion-report
   schema; CI feed (B.8) extends, never expands, contents (C2b).

### 2.5 Selection policy (AOT-D14): compile everything reachable

Default = **compile-everything-reachable** (no profiles ⇒ no hotness
oracle).
Size-driven demotions (bytecode-only, reason in STATICMETA): (a) per-function
cap (64 KB post-B3); (b) whole-artifact cap (§4.4); (c) statically
unreachable: no Tier-A code — bytecode + source still ship (AOT-D15); (d) **lattice pruning
(AOT-D0)**: one extra variant only where a load-bearing axis is silent AND the
function is a hot candidate (loop-bearing/length floor); V≤2 default. FTL when
any seeded speculation or proven callee exists, else DFG-grade. (Rejected: A.)

**AOT-D20 (C38) — builtins ship Tier-A (hot subset).** The builtin corpus
(`s_JSCCombinedCode`, 152 KB, survey §3.3) is not bundle code; unhandled ⇒
LLInt forever. (C59/M.2 re-audit pre-freeze.) Fixed at engine-build time: precompile a frozen hot
subset (iteration/%TypedArray%/Promise/async shells — a reviewed list, not a
training run), shipped as a flavor-hash-keyed resident section binding via
§3.3. +0.50 MB §4.1 line, CI-gated; cold remainder = §5.1(6). (I.4.)

### 2.6 What static seeding loses vs live profiles (honest)

(1) **Value-range speculation**: static analysis rarely proves int-only ranges.
(2) **Polymorphic-site bias**: analysis sometimes knows the set, never the
frequency order. (3) **Hot/cold weights**: inlining runs on proven-callee + size
heuristics. (4) **Top propagation** (AOT-D16): fact-less sites widen generic
regions transitively. (5) **C61 channel-1-only kinds** (InBy/DeleteBy/
InstanceOf/brand): monomorphic-or-generic.
All PERF deltas, never correctness (AOT-I3). The
AOT-D0 overlay recovers the monomorphic-at-runtime slice of (1)-(2); CI
feed (B.8) restores the rest — never required.

## 3. Linking reality

### 3.1 Relocation taxonomy → mechanism

The survey's catalog (§2) with binding mechanisms. Classes: **R** (link-time
symbol), **M** (load-time materialization into a per-function data table — the
`BaselineJITData` discipline, `jit/BaselineJITCode.h:124`),
**V** (revalidate-or-reject at load — the `DesiredWatchpoints::reallyAdd`
shape, `dfg/DFGDesiredWatchpoints.h:208`). AOT-D0 symbolic slots are class-M slots
written at selection instead of load.

| Baked constant | Mechanism |
|---|---|
| 1. Structure IDs (32-bit, `runtime/StructureID.h:38`) | **M+V**: RECIPE resolved at load ⇒ live `StructureID` in the data table; no immediates (§3.2); guards + allocation sites read slots (`dfg/DFGJITCompiler.h:261`). AOT-D0 symbolic slots: selector-written, **GUARD-POSITION-ONLY (C16/G.2)** — value/store consumers use load-time recipe slots; recovery streams never read rebindable slots. |
| 2. Frozen cell pointers (`Graph::freeze`, `dfg/DFGGraph.cpp:1634`) | **M+V**, per the **AOT-D13 pathability lattice** (A): pathable ⇒ table entry + **C40 per-kind identity precondition** (J.1/K.8; bind-checked BEFORE subscription — a pre-bind-patched intrinsic never binds); unpathable-droppable (closure `JSFunction`s) ⇒ per-realm executable-cell guard (C26/H.2/K.8; `dfg/DFGByteCodeParser.cpp:1571`; cross-realm EXITS) or decline; unpathable at emitter ⇒ tool-time error (AOT-I5/C5). Checked at freeze; perf §5.1.4. Weak entries guard-position-only; weak-death §3.4/AOT-I11. |
| 3. JSGlobalObject/VM pointers (`ftl/FTLLowerDFGToB3.cpp:2838`) | **M**: one well-known PER-REALM data-table slot (`offsetOfGlobalObject` analog; C19). |
| 4. `operation*` / thunks / LLInt trampolines (~736 symbols, ~0.36 MB) | **R**: GOT-style indirection. LINKED: static-link relocations; BUNDLE: loader fills the image GOT once. FTL sites routed at lowering (C21) — DFG link records don't cover them. |
| 5. Atoms / `UniquedStringImpl*` (`DFGCommonData::m_dfgIdentifiers`) | **M**: atom table interned at load (`runtime/CachedTypes.cpp:826`); code loads pointers from it. |
| 6. VM singletons (small-strings, scratch buffers; `dfg/DFGSpeculativeJIT.cpp:2947`) | **M**: via data table; scratch buffers = per-VM allocations bound at load. |
| 7. IC data (`PropertyInlineCache`/`InlineCacheHandler`; no `StructureStubInfo` in this tree — C30/H.3) | **M**: handler-dispatch (data) ICs only — the `useHandlerICInFTL` form (`runtime/OptionsList.h:643`), emitted for DFG bodies too; state in per-function tables; handler publish = data write (`bytecode/PropertyInlineCache.cpp:1149`); self-access handlers pass the E.2 audit; never repatched (§5.1.3). |
| 8. Jump replacements / invalidation (`dfg/DFGCommonData.h:133`) | **M/V → validity bit**: §3.4. |
| 9. Wasm unlinked calls | out of scope (N4); mechanism (R) already production (`wasm/WasmCalleeGroup.cpp:323,435`). |
| 10. JS-to-JS call sites (r2; E.2/D) | **M+R**: all AOT call sites are data-IC calls (`UseDataIC::Yes`, `bytecode/CallLinkInfo.h:615`); `CallLinkInfo` state in the data table; direct calls = data writes. Callee entrypoints: AOT ⇒ class-R; LLInt ⇒ prologue at bind. Poly/megamorphic: precompiled shared virtual-call thunks, E.2-audited per immediate (C12); `setStub`/repatch compiled out; C++ backstop (E.2). |
| 11. Switch jump tables (absolute side-table addresses — `bytecode/JumpTable.h:44`; C31/H.4) | **M/R**: per-function case→offset arrays; BUNDLE rebases into the data table; LINKED `.data.rel.ro` + native relocations; `AOTObjectLinkBuffer` captures finalize-time table writes; string tables key on row-5 atoms. |
| 12. Per-VM MUTABLE state via `AbsoluteAddress` — barrier threshold (`jit/AssemblyHelpers.h:2088`), fencing/stack-limit/traps (I.2 list) (C36) | **M**: GC-phase-mutable ⇒ never snapshotted; offset loads off the row-3 VM pointer (the fork's VMLite shape). Validation-only sites disabled tool-side. |

**AOT-D5: no load-time patching of code pages beyond standard dynamic
relocation; all VM-dependent values via data tables.** CODE sharable, signing/W^X
trivial (§3.5), load O(tables) not O(code). (Rejected: A.)

### 3.2 Structure recipes (deterministic shape replay)

A **recipe** deterministically describes how a Structure comes to exist: *base*
(a built-in/global prototype or recipe-ref, as a path), *indexing type*, *flags*,
*inline capacity*, *TypeInfo/ClassInfo discriminator*, *ordered
property/transition list* — every `Structure::create` input layout depends on
(`runtime/Structure.h:236`; C27). Load-time resolution walks
`Structure::addPropertyTransition` from the base, creating-or-finding each
step; the `StructureID` fills the table slot. Structure identity in JSC *is*
the transition path: replaying VMs get behaviorally identical structures; IDs
differ per VM — table-loaded, never baked (row 1).

Obligations (AOT-I6, mechanized C18): every path step an own DATA property of
a non-exotic ordinary object, `getDirect`-read — never `[[Get]]` (no user traps
mid-bind); accessor/Proxy/exotic ⇒ per-function fail-closed, reason recorded;
same for bases and §3.3.2; cycles = tool-time error. Startup-user-code
structures base on the §2.1 inferred shape; divergence ⇒ fail closed (§3.4).
**AOT-D6:** recipes, not heap snapshots, in v1 (A; opt-in later).

### 3.3 Load-time sequence and precondition validation

Per artifact: validate HEADER (fail ⇒ ignore); map sections; intern atoms.
**Realms (C19; G.5+H.1):** CODE/EXITMETA/RECIPES + exit parameter
blocks (realm-invariant, C25) are shared; ALL binding state keyed per
(artifact function × `JSGlobalObject`) (G.5/K.8); second realms bind
independently (dynamic
sources Tier L, §1.5). Then per function × realm (lazy, first link — §4.3):

1. Resolve recipes (memoized per realm); verify the resolved structure's
   ACTUAL layout (offsets, inline-ness, butterfly sizes) vs its STATICMETA
   layout signature (C27/H.7); mismatch ⇒ fail-closed.
2. Materialize data table (global paths, frozen-value paths — C40
   identity-checked against their PRECOND descriptors, atoms, singletons).
2b. **Inlinee CodeBlock slots (AOT-D17/C13; E.3+H.1).** Inlinee LLInt
   CodeBlocks may not exist at bind (need a live closure,
   `runtime/ScriptExecutable.h:115`). Bind fills existing; the rest carry an
   UNMATERIALIZED tag — such exits route through the recovery interpreter:
   materialize, fill the PER-REALM slot (store-release; C25), resume. Entries
   strong GC refs. Mid-exit materialization failure = fatal-resource
   controlled crash (C46/K.1); never UB.
3. **Precondition validation** = `reallyAdd` offline-shaped: each PRECOND
   entry re-checked against the live heap before subscribing
   (`dfg/DFGDesiredWatchpoints.h:208`; `dfg/DFGPlan.cpp:587`); an entry whose
   watchpoint set does not exist in this flavor **fails CLOSED** (C17).
4. All pass ⇒ subscribe watchpoints (fire ⇒ §3.4), set validity bit,
   install entrypoint (§1.2; lattice functions defer install, C20). Any failure ⇒
   LLInt, reason recorded. **Never UB; partial binds impossible** (AOT-I4): bit
   set last, cleared first.

### 3.4 Invalidation: the validity bit

Runtime invalidation rewrites code at `m_jumpReplacements` sites
(`dfg/DFGCommonData.h:133`). Immutable AOT code embeds there a load+branch of
the per-function validity bit. **Fire (C53/L.1) ⇒ clear bit (store-release,
§6) AND uninstall every subscribed (function × realm) entry before returning
to JS** (rationale L.1; in-tree leg: jettison's `installCode`,
`bytecode/CodeBlock.cpp:2803`). In-flight activations exit at
their next check (~1 load+branch per point); both writes are data (AOT-I2).

**Weak-entry death (AOT-I11; full record D).** Cell death fires no watchpoint
(`bytecode/CodeBlock.cpp:1478`); a dead weak entry would let
address/`StructureID` reuse PASS a stale guard — type confusion. Spec:
per-image weak-table sweep each GC cycle — dead entry ⇒ clear owning validity
bit AND poison the slot (ditto weak `StructureID`s/transition pairs). Sound
for guard-position consumers (C16); conservative scanning keeps in-flight
values alive.

Combined with §1.4: precondition-class invalidation is permanent demotion; the
bit is never re-set without full §3.3 re-validation (lazy first-link only) —
**AOT-D7**: no runtime re-validation loop (A).
**C55/L.3 — lifecycle:** debugger attach (`debugger/Debugger.cpp:600` →
`deleteAllCode`; breakpoints REQUIRE demotion) and
`ScriptExecutable::clearCode` (`runtime/ScriptExecutable.cpp:165`) ⇒ C53
sweep; the binding table strong-refs both CodeBlocks.
AOT-D7 carve-out: lifecycle demotions re-bind lazily via §3.3; attached
debugger = jitless speed.

### 3.5 W^X / iOS: two delivery modes

- **Mode LINKED (true AOT):** `AOTObjectLinkBuffer` emits a real object file
  (ELF/Mach-O): CODE in `.text`, class-R as native relocations, tables in
  `.rodata`/`.data`; the platform linker links it into the app. Code **signed
  with the app**, no JIT entitlement, no runtime executable mapping — the iOS
  mode. arm64e (C48/K.3): slot signing at bind, return-PC signing at
  recovery, gate-map entries, `@AUTH` relocations.
- **Mode BUNDLE (scoped — AOT-D19/C24/G.10/C33/C39):** artifact beside the
  app; loader verifies INTEGRITY (platform signature or app-key-signed
  page-hash tree, §4.3; G.10); CODE then PROT_READ|PROT_EXEC (never writable; app-key via
  anonymous copy, C39). Desktop/server; excluded on hardened macOS; iOS =
  LINKED only. Unverifiable ⇒ Tier L.

- **Mode BUNDLE-EMBEDDED (C45/J.6)** — the product mapping: `bun build
  --compile` embeds the artifact in the signed/verity-covered runtime or
  compiled-app executable. macOS incl. hardened ⇒ kernel-verified map, shared
  — ONLY if the blob is inside the code-directory-hashed range (re-sign after
  embed; loader verifies, else app-key/Tier L — C52/K.7); verity/IMA Linux ⇒
  same; generic Linux/Windows ⇒ C39 app-key copy; matrix + composed gates J.6.

Both share all sections and §3.1-3.4; only CODE execute permission and class-R
resolution differ. AOT-I7 equivalence is observational, above the loader;
deployability asymmetry recorded (AOT-D19).

## 4. Size budget

Ground truth: survey §3 (nm/readelf on `WebKitBuild/Release/bin/jsc`, -O2
x86-64; `.text` 23.62 MB). Caveats: -O2 not -Os; arm64 ~10-15% larger; KEEP
buckets include `ENABLE(JIT)` code a jitless compile removes (overcount in our
favor).

### 4.1 The table (from survey §3.2)

| Subsystem (`.text` bucket) | MB | Ships? |
|---|---:|---|
| JSC runtime + heap + parser + bytecompiler | 9.49 | **KEEP** (parser required: `eval`/`new Function`, C1) |
| DFG | 3.37 | DROP (offline only); keep EXITMETA + unwind readers (~0.10 est.; AOT-D11) |
| Wasm (incl. IPInt) | 2.05 | DROP compilers; **KEEP IPInt 0.10 + non-compiler Wasm runtime ≈ 0.50** (measured, Annex C). Wasm-off flavor: −0.60 (N4) |
| B3 + Air | 1.72 | DROP |
| ICU code | 1.11 | **KEEP** (ICU stance below) |
| FTL | 0.99 | DROP |
| Baseline JIT + IC compiler + `operation*` slow paths | 0.94 | SPLIT: drop compilers (~0.58); **KEEP `operation*` ≈ 0.36** (MODIFIED per ledger) |
| WTF + bmalloc + libpas (out-of-line) | 0.90 | **KEEP** |
| Assembler + disassembler | 0.73 | DROP (AOT-I2) |
| Yarr | 0.56 | **KEEP interpreter ≈ 0.28**, drop YarrJIT |
| Inspector + profiling | 0.51 | **KEEP** — Bun ships `--inspect` (C50/K.5); min flavor −0.51 |
| LLInt slow paths + asm blob | 0.33 + 0.38 | **KEEP** (Tier L) |
| jsc shell + misc + libstdc++ | ~1.0 | ~0.3 shell DROP; ~0.7 KEEP |
| NEW runtime (loader/binder, recipes, recovery interp, selector, weak sweep) | — | **KEEP, +0.30 allocation** (CI-gated §7) |
| `ENABLE(AOT_RUNTIME)` retained DFG data structures | — | **KEEP, +0.25 allocation** (C51/K.6 floor; CI bucketed) |
| Builtin Tier-A artifact (AOT-D20 hot subset) | — | **KEEP, +0.50 allocation** (CI-gated; list shrinks first — I.4) |

**KEEP sum ≈ 15.81 MB** -O2, SPLIT (C57/L.5): **13.27 factor-eligible text**
+ **1.43 flag-immune/post-factor** (L.5 split) + **ICU 1.11** (own factor
~0.9) + ~0.7 non-ICU `.rodata` +
eh_frame/data **3.15 measured** (KEEP-scaled ≈ 2.1, L.5) ⇒ **≈ 18.6 MB raw
at -O2**.

### 4.2 Path to <10 MB and the verdict

Shipping config differs subtractively (-Os/LTO/`--gc-sections`/strip,
backends out, `#if !ENABLE(JIT)`, dev trim). Precedent: jitless mini-mode
JSC, V8-lite.

**Verdict (C14/C44/C50/C57/C60): gate AT RISK — only the optimistic corner
passes unremediated.** Gate per SHIPPING flavor (Bun's incl. Inspector —
C50/K.5). AOT-I10 gates the WHOLE binary; whole-binary band
(C60/M.3): **13.27f + 1.00 ICU + 1.43 flag-immune +
0.70 rodata + 2.1g unwind/data** (f bridge, g unwind factors —
**unmeasured**; g∈[f,1]). f=0.42 ⇒ 9.6-10.8; f=0.55 ⇒ **11.6-12.5,
gap 1.6-2.5 MB**; f=0.63 ⇒ 12.8-13.6 (M.3). Feasible
only ex-ICU (AOT-D8) + measured bridge + C60 levers. **C56/L.4:
the AOT-I10 milestone measurement is a DESIGN-FREEZE PRECONDITION** (jitless
-Os/LTO build, `ENABLE(AOT_RUNTIME)` ON — C51; re-bucketed incl.
non-text). Remediation re-sized to the gap (C60 amends L.4),
Bun-flavor-valid: (1) **`.eh_frame` trim FIRST**
(`-fno-asynchronous-unwind-tables` on non-unwinder TUs; JSC is
`-fno-exceptions`, AOT-D11 unwinds EXITMETA) — SCALED (C62/N.2):
≈1.7g MB (0.95-1.7 over g∈[f,1]); `.data.rel.ro` (relocations) survives;
(2) dev-surface cuts 0.3-0.8; (3) Yarr/parser -Os; (4)
builtin list −0.50. Mid f=0.55: levers sum ≈2.0-2.2 vs gap ≤2.5 ⇒
§4-re-opens is the EXPECTED branch absent favorable measurement (C62).
Still >10 MB ⇒ gate FAILS and §4 re-opens — not a table edit.

**ICU (AOT-D8):** budget excludes the 30.73 MB `icudt75_dat` (survey §3.3):
**Bun ships ICU regardless — zero marginal bytes.** Standalone fallback:
small-icu/Intl-off. (Rejected: A.)

### 4.3 Memory + startup budget

- **Startup:** HEADER check O(1). Binding lazy per function (§3.3): recipe
  walks + PRECOND re-checks (`dfg/DFGPlan.cpp:602`) +
  subscription. Budget: **≤5 µs/function amortized, ≤10 ms / 2,000-function
  reference app**; atom interning the only eager bulk cost. CI-measured (§7).
- **Integrity (BUNDLE; C33/C39/I.5):** HEADER carries a page-granular hash
  tree, verified at BIND time per function-covering region. TOCTOU (K.8):
  direct exec-mapping ONLY where the
  kernel re-verifies page-in (platform signature; fs-verity/dm-verity);
  app-key fallback copies verified regions to anonymous memory before
  PROT_EXEC, else Tier L. CI
  gates verified-MB/ms + copy RSS + the composed app-key first-N-calls bound
  (C45/J.6).
- **Memory:** CODE mmapped/linked, shared, never copied — LINKED +
  kernel-verified BUNDLE only (C39; app-key pays copy RSS = bound CODE bytes).
  Per-function writable state (per realm, C19) **≤ 256 B median**.
  EXITMETA/RECIPES read-only, on demand. Private-dirty delta vs jitless
  baseline **< 1 MB, reference app** (same scope).

### 4.4 Artifact size budget (C14)

The 10 MB budget covers the runtime; the ARTIFACT is per-app payload — in
LINKED mode it is app `.text`/`.rodata` bytes. Pre-freeze protocol: measure
the **expansion factor** per bytecode byte on the reference app, forced
DFG/FTL (plan 5-10× code + comparable exit metadata until measured; overlay
adds 1.15-1.4×). Defaults (C32/H.8): whole-artifact cap
(§2.5b) = **2× the projected ARCH-S baseline** (≈10-20× source bytes;
reference bundle ⇒ ~10-12 MB ceiling, §5.4), absolute per-app override; demotion order = worst code-per-bytecode-byte,
overlay variants first; CI publishes artifact size for BOTH modes (AOT-I10
gate).

## 5. Performance (honest)

### 5.1 vs JIT-full JSC

Expect **a real, workload-dependent gap**, from seven losses:

1. **No runtime recompilation.** The JIT recompiles on better profiles after
   deopt; we select-then-demote (§1.4); phase-changing code lands on LLInt.
2. **No value profiles at compile time (C2b).** The §2.6 losses. Mitigations:
   watchpoints + proven shapes/callees cover the structured core; the AOT-D0
   overlay recovers monomorphic-at-runtime silent functions; the rest is
   generic-but-direct. Wrong static assumptions are exits, never wrong answers.
3. **IC story (AOT-D9 r2 + C30/H.3; C2/A):** handler bodies are
   runtime-`LinkBuffer`-generated (`bytecode/InlineCacheCompiler.cpp:1482-1770`).
   **v1 ships self-access-only
   handler ICs** (§3.1 row 7); poly/proto-chain/getter-setter take the
   `operation*` slow path. v2 (B.7): artifact-resident handlers.
4. **Less cell-identity inlining** (AOT-D13): unpathable closure-instance
   callees inline under per-realm executable-cell guards (C26) or not at all
   (§3.1 row 2).
5. **Invalidation checks + table loads**: ~1 load+branch per invalidation
   point (§3.4), one table load per pointer-constant site (AOT-D12); low
   single-digit % on check-dense code, noise elsewhere.
6. **Cold builtins stay LLInt** (AOT-D20 ships only the budgeted hot subset);
   full JSC JITs every warm builtin.
7. **RegExp is interpreter-class** (AOT-D21/N.3): dynamic AND (v1) literal
   regexps run the Yarr interpreter — a regression vs YarrJIT'd Bun on
   regex-hot servers. v2 charters a precompiled literal-regex pool via the
   §2.3 emitter (YarrJIT is LinkBuffer-finalized, `yarr/YarrJIT.cpp:7205`).

Net (publish, then measure, §7): shape-/callee-provable code at
**FTL-minus-small-to-moderate**; silent-but-monomorphic hot code at
selected-variant speed (post C20 warm-up), else generic-direct;
phase-changing/megamorphic **between LLInt and baseline**. Whole-app: tracks
bundle provability (Hopc, survey §4).

### 5.2 vs interpreters (XS/QuickJS class) and jitless JSC

Jitless LLInt floor ~2-6x off baseline+; XS/QuickJS same ballpark cold; no
interpreter reaches DFG/FTL hot. Thesis: interpreter-class deployability,
JIT-class hot loops — RegExp excepted (§5.1(7); AOT-D19/K.8).

### 5.3 Exit storms

Worst case: a hot function exits per-call on a wrong static assumption; §1.4
converges it to permanent demotion = jitless speed (CI-verified §7) — today's
iOS-JSC speed, never incorrect (K.8).

### 5.4 Prior-art honesty (C1 check)

Static Hermes makes types load-bearing (annotated/checked subset; classic
Hermes drops `with`/some `eval` forms); Porffor accepts test262 gaps. We refuse
both trades (C1): our test262 score is by construction the jitless score —
every construct has Tier L; the refusal's cost is §5.1. Closest kin: Hopc
(annotation-free AOT JS + embedded `eval` interpreter, ~1.5-2x of JITs, survey
§4) — our static mode is Hopc's bet on JSC's backend, plus watchpoints and a
tested deopt tier. **Footprint row:** Hermes ~2-3 MB + compact bytecode; XS
sub-MB + ROM snapshot; us 9.6-13.6 MB whole-binary pre-remediation
(§4.2/C60) + a per-app
artifact projected 10-15 MB-class at default policy (§4.4, C32). Size-first
embedders should take Hermes/XS — that trade IS the design.

## 6. Threads interaction (stated, not solved)

Obligations for the joint threads design (E.4/K.8): (1)
`useJSThreads` flavor-pinned (AOT-I8). (2) Validity-bit clears + C53 entry
uninstalls: store-release under watchpoint-fire safepoint/STW discipline;
checks wherever DFG would plant a jump replacement, no fewer. (3) Binding under `reallyAdd`'s
world-state guarantee; N mutators ⇒ safepoint participant. (4) Demotion
counters racy-by-design; the validity bit is the only synchronized control.
(5) Variant (de)selection = entry publish after slot binds, discipline (2);
slots write-once between deselections (C16). (6) Compose vs
`docs/threads/SPEC-{jit,ungil}.md` (checked, O.2): no jump replacements ⇒
jit I2/I8 (STW-only code patching) vacuous; CheckTraps polls keep their
trailing InvalidationPoint validity check ⇒ jit I21 holds under
`usePollingTraps`; Class-A fire (jit §5.6/I10) = (2). No ungil conflict.

## 7. Invariants and verification charter

### Invariants (binding; violations are bugs)

- **AOT-I1 (semantics):** Every ECMAScript program executes with full
  semantics; any function, at any moment, can run correctly on Tier L. AOT
  changes performance only.
- **AOT-I2 (no runtime codegen):** The product never allocates executable
  memory or writes executable pages (`jit/ExecutableAllocator.h:74`). Enforced:
  CI `nm` audit — no kept symbol references the C35/C37 forbidden list
  (InlineCacheCompiler/LinkBuffer/ExecutableAllocator/lazy-slow-path compile
  + exit-gen thunks).
- **AOT-I3 (exit soundness):** Every Tier-A speculation is guarded; every guard
  failure lands in LLInt with fully reconstructed state, per the recorded
  recovery stream (DFG `Operands<ValueRecovery>` / FTL ExitValue, C22).
- **AOT-I4 (bind atomicity):** A function is either fully bound (recipes,
  inlinee slots filled-or-tagged, preconditions validated, watchpoints
  subscribed, bit set — in that order) or pure LLInt. No partial state executes;
  an UNMATERIALIZED slot is never dereferenced by ramp code.
- **AOT-I5 (no silent bakes; decidable; r4):** No B3/assembler constant from
  a `JSCell*`/`Structure*`/`StructureID`/`OperationPtrTag` pointer (C21)
  outside the table-load/GOT path; enforced at constant *construction* — the
  `FrozenValue`/`constInt64`/`TrustedImmPtr` choke points, structure-ID
  wrappers (C11), `FTLOutput::operation` — not at emission (AOT-D12). Extends
  to tool-VM C++ heap pointers (C29) and `AbsoluteAddress` immediates (C36):
  construction outside the class-R whitelist = hard error (§3.1 row 12; I.2).
- **AOT-I6 (recipe determinism):** Recipe resolution is deterministic,
  side-effect-free, runs no user JS; unresolvable ⇒ per-function fail-closed.
- **AOT-I7 (mode equivalence):** LINKED and BUNDLE observationally identical
  above the loader (deployability/security differ — AOT-D19).
- **AOT-I8 (flavor pinning):** Artifact flavor hash must match the runtime,
  HEADER ISA baseline ⊆ host features (C58); else ignored wholesale.
- **AOT-I9 (artifact determinism):** Same bundle + same tool build (+ identical
  optional feed) ⇒ byte-identical artifact. PRODUCED, not just detected (C34):
  serializers iterate §2.1-identity/bytecode order ONLY — never pointer
  order; plan scheduling deterministic (sorted merge); charter 1 detects.
- **AOT-I10 (size gate):** Product runtime binary (ex-ICU-data, AOT-D8) < 10 MB,
  CI-enforced per flavor; `ENABLE(AOT_RUNTIME)` retained code is a tracked bucket.
- **AOT-I11 (weak-death soundness):** A dead weak data-table entry can never
  satisfy a guard: the GC-cycle sweep clears the owning validity bit and poisons
  the slot before mutators resume (§3.4).
- **AOT-I12 (no profile required):** The tool produces a valid artifact from any
  bundle with zero profile input; CI builds reference artifacts profile-free (C2b).
- **AOT-I13 (selection soundness; r4):** a variant entry publishes only after
  every symbolic slot it reads is bound and its precondition row (incl. layout
  signature C15) validated; an UNBOUND slot is never reachable from a published
  entry; slots guard-position-only (C16), always guarded; (de)selection/
  rebinding are data writes only (AOT-I2).

### Verification charter

1. **Determinism (AOT-I9):** CI builds every artifact twice, byte-compares —
   catches address/iteration-order leaks.
2. **Oracle differential vs JIT mode:** same bundle, product runtime +
   artifact vs full-JIT jsc with `forceOSRExitToLLInt` (§1.3) +
   exit-fuzzing; observable semantics match,
   incl. `toString` text equality (AOT-D15). Includes the exit-storm suite
   (every speculation class; AOT-I3 + §1.4 convergence).
3. **Precondition-violation matrix:** per PRECOND category, break it (i)
   pre-load ⇒ never binds, (ii) post-load ⇒ fires, bit clears, in-flight
   exits, next call LLInt; both modes (AOT-I7). Plus rows (FULL TEXT I.6+J.1,
   BINDING): AOT-I11 weak-death/recycled-`StructureID`; AOT-D11 exceptions
   through AOT frames; C17/C18/C19+C25/C26/C27/C28/C40.
4. **test262:** full suite on the product runtime with artifacts present;
   score equals the jitless score (C1/AOT-I1).
5. **Recovery-interpreter equivalence:** every dedup'd ramp (DFG and FTL
   streams, C22) replayed against the C++ recovery interpreter on captured exit
   states; identical frames — incl. a SpeculativeAdd-undo exit (C41).
6. **Size CI gate (AOT-I10):** per-flavor `size`/`nm` bucketing per survey
   §3.2; regression budget per bucket; hard fail at 10 MB.
7. **Budgets (§4.3-4.4):** binding-latency histogram, private-dirty delta,
   artifact bytes — per CI run, reference app.
8. **Flavor matrix (AOT-I8):** only matching artifact × runtime pairs bind;
   others run Tier L, reason logged.
9. **Selector matrix (AOT-I13):** drive LLInt profiles to (i) match ⇒ variant
   executes; (ii) mismatch/poly ⇒ generic; (iii) post-selection shape change ⇒
   descent, single rebind, demotion convergence; (iv) same-shape-different-
   LAYOUT ⇒ decline (C15); (v) deselect-mid-activation ⇒ resumed frame exits at
   next guard (C16); (vi) two realms both hot ⇒ BOTH
   reach selection (C42) — vs the item-2 oracle.
10. **Rounds 8-10 rows** (FULL TEXT K.9+L.7+M.5, BINDING there):
   C46/C47/C48/C49/C52/C53/C54/C55/C58/C59.
11. **Round-11 rows** (FULL TEXT N.1, BINDING): C61 proto-setter
   interposition pre/post-bind vs JIT oracle; proto-chain fuzz over every
   C61 Status kind.

— end.
