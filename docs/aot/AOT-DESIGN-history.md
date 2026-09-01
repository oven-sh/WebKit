# AOT-DESIGN history + BINDING annexes

Companion to `docs/aot/AOT-DESIGN.md` (the capped spec). Annexes here carry the same
authority as the main doc. Frozen-spec conventions apply: corrections are appended, never
silently edited; decisions are relitigated only via a logged correction entry.

**Provenance (repeated):** clean-room design. Filip Pizlo's unpublished AOT-JSC design has
NOT been seen; no content here derives from it. Sources: this tree + `AOT-SURVEY.md` +
public prior art only.

## Change log

- **2026-06-10 — draft 1.** Initial AOT-DESIGN.md (39,167 bytes) + this file. Decisions
  AOT-D1..D9, invariants AOT-I1..I10 established.
- **2026-06-10 — draft 2 (round-2 adversarial review applied).** 11 external findings
  received; every tree-fact claim re-verified against this tree (Annex D); **all 11
  accepted** (two pairs merged: F3+F9 exit-ramp immediates, F5+F11 unpathable frozen
  cells), zero refuted. Decisions AOT-D10..D13 added; AOT-D2 and AOT-D9 corrected;
  AOT-I5 restated as a decidable property; AOT-I11 added; §3.1 gains row 10 (JS calls);
  size table gains AOT_RUNTIME (+0.15) and unwind-reader (0.05→0.10) allocations,
  KEEP sum 13.95→14.15 MB. Main doc 49,988 bytes (cap 50,000).
- **2026-06-10 — draft 4 (constraint C2b applied: NO TRAINING RUNS).** Drafts 1-3 were
  built on a training-run/profile-replay spine (`--aot-record` + profile pack +
  replay-by-slot-injection), violating hard constraint C2b: the PRIMARY mode must
  compile from static analysis of the bundle alone, Java-AOT style — real applications
  do not do PGO training. Correction C7. Changes: C2b added to §0; §2.1 rewritten
  (whole-program static analysis: constructor shape inference, literal shapes +
  escape-ish element kinds, module-graph constant/callee propagation as the CHA analog,
  universal watchpoint assumptions; generic-but-direct `UntypedUse` floor); §2.2
  rewritten as static-fact seeding (AOT-D4 revised — same slot-injection mechanism, new
  fact source); §2.5 selection policy added (AOT-D14: compile-everything-reachable);
  §2.6 honest static-vs-profile loss added; PROFILEMETA → STATICMETA; §1/§3/§5/§7
  de-profiled (Tier-A description, post-exit policy wording, recipe-validity argument,
  perf expectations, AOT-I9, charter item 2); AOT-I12 added (no profile required).
  Profiles confined to optional appendix-grade Annex B.8. Main doc 49,989 bytes.
- **2026-06-10 — draft 5 (round-3 adversarial review applied).** 10 external findings
  received; every tree-fact claim re-verified against this tree (Annex E). **All 10
  accepted** (two merges: G2+G5 Status-injection mechanism, G8+G9 size-budget
  arithmetic/verdict), zero refuted. Corrections C8-C14. Decisions AOT-D15 (source
  text ships), AOT-D16 (prediction floor via FuzzerAgent seam + ArrayProfile
  saturation), AOT-D17 (inlinee CodeBlock materialization protocol) added; AOT-D4
  revised (three channels; DFGByteCodeParser MODIFIED tool-only at the Status
  funnels); AOT-I5 extended to 32-bit StructureID wrappers; AOT-I2 gains the
  link-time `nm` audit; AOT-I4 gains the inlinee-slot clause; §2.4 gains SOURCE; §2.5
  "skip" clarified to never drop bytecode/source; §4.1 sum corrected
  (14.15→14.70 incl. measured 0.50 MB wasm glue; recovery interp de-duped); §4.2
  verdict softened to FEASIBLE-IFF; §4.4 artifact budget + §5.4 footprint row added;
  ledger gains DFGByteCodeParser and JITOperations/DFGOperations/Repatch rows. To
  stay under the 50,000-byte cap, "Rejected:" tails were moved to Annex A (binding)
  and several passages compressed with full text recorded in Annex E. Main doc
  49,971 bytes.

- **2026-06-10 — draft 6 (constraint C2b follow-up: architecture decision must be
  COST-MODELED, not fiat).** New main-doc §0.5 (AOT-D0): ARCH-S (static spine) vs
  ARCH-L (selection spine) modeled against each other with measured numbers (survey
  §3.5 per-function compiled sizes; §1.10 data-IC machinery; §1.11 free LLInt
  profiling); decision = HYBRID (ARCH-S spine + pruned ARCH-L overlay, V≤2 on the
  statically-silent hot-candidate subset); one-paragraph epitaphs recorded for both
  pure architectures. Overlay threaded through the design: §1.1 (Tier-A lattice
  bodies), §1.2 (selector via the `checkSwitchToJIT` slow path), §1.4 step 0
  (lattice descent precedes demotion), §2.5(d) (lattice-pruning policy), §2.4
  (CODE variants; STATICMETA precondition rows + symbolic-slot layouts), §2.6
  (overlay recovers the monomorphic-at-runtime slice of losses 1-2), §3.1 (symbolic
  slots = class-M written at selection; row 1/row 7 notes), §4.4 (overlay artifact
  factor), §5.1 (items 1-2 + net expectation re-derived), §6(5) (selection publish
  discipline). New invariant AOT-I13 (selection soundness); new charter item 9
  (selector matrix). Full cost-model derivation + selector protocol: Annex F
  (BINDING). To stay under the 50,000-byte cap, many passages were compressed
  (wording-only); every compressed passage whose content was load-bearing already
  has its full text in Annexes A/D/E (per the draft-5 precedent); the reuse-ledger
  table was converted to equivalent prose with no row dropped. Main doc 49,997
  bytes.

- **2026-06-10 — draft 7 (round-4 adversarial review applied).** 10 external findings
  received (the 10th truncated in transit; its core claims were verified from the
  main doc's own §3.5/§5.2 text). Every tree-fact claim independently re-verified
  against this tree (Annex G). **All 10 accepted** — three blockers against the
  AOT-D0 overlay (C15 layout signatures, C16 guard-position-only slots, C20
  deferred install/LLInt residency), one blocker against the §2.1.3 static spine
  (C17 useJIT-gated watchpoints), six majors (C18 path-walk contract, C19 realms,
  C21 FTL operation constants, C22 FTL exit compiler, C23 stand-in cells, C24
  BUNDLE integrity/entitlement) — zero refuted. Decisions AOT-D18 (tool-side
  stand-in cell factory), AOT-D19 (BUNDLE scoping + integrity) added; AOT-I5
  extended to OperationPtrTag constants; AOT-I7 caveated; AOT-I13 extended (layout
  signature + guard-position-only); charter items 3 and 9 gain r4 rows; charter 5
  covers both exit streams. Annex F is NOT silently edited: its r4 amendments
  (F.2.5 layout-equivalence collapse, F.2.8 warm-up cost, F.4 protocol) are
  recorded in Annex G per frozen-spec conventions. To stay under the cap, many
  passages were compressed (full text already in Annexes A/D/E/F, or newly in
  Annex G). Main doc 49,994 bytes.

- **2026-06-10 — draft 8 (round-5 adversarial review applied).** 10 external findings
  received (the 10th truncated in transit; its verifiable core — pointer-keyed
  serialization inputs vs AOT-I9 — was confirmed from the tree and the doc's own
  text). Every tree-fact claim independently re-verified against this tree
  (Annex H). **All 10 accepted, 0 refuted** — two blockers (C25 exit parameter
  blocks vs realms; C32 artifact-cap self-contradiction), eight majors (C26 D13
  realm pinning, C27 recipe layout inputs + spine layout verification, C28
  closed PRECOND rule, C29 MathIC, C30 IC re-grounding on
  PropertyInlineCache/InlineCacheHandler, C31 switch jump tables, C33 BUNDLE
  integrity startup cost, C34 AOT-I9 production mechanism). §3.1 gains row 11;
  AOT-I5 extended to tool-VM heap-object pointers; AOT-I9 gains the production
  rule; charter 3 gains four r5 rows; survey §2 row 7 corrected in place
  (bracketed, original preserved) and rows 10-11 added. Annexes E.3/G.5/F.2.6
  are NOT silently edited: their r5 amendments are recorded in Annex H.1/H.8.
  Compressions recorded in H.9. Main doc 49,992 bytes.
- **2026-06-10 — draft 9 (round-6 adversarial review applied).** 6 external
  findings; every tree-fact claim independently re-verified against this tree
  (Annex I). **5 distinct root causes accepted, 0 refuted** (findings 2+4
  merged: same `AbsoluteAddress` class — C36). Two blockers: C35 (FTL
  LazySlowPath = runtime codegen in every FTL-grade body ⇒ eager tool-time
  generation, op+thunk dropped) and C36 (per-VM mutable-state `AbsoluteAddress`
  bakes escape AOT-I5 ⇒ §3.1 row 12, AOT-I5 extended to the constructor).
  Majors: C37 (exit-site dispatch ABI specified; generation-thunk/repatchJump/
  osrExitIndex machinery compiled out; ledger row), C38 (AOT-D20: builtin
  corpus hot subset ships Tier-A, +0.50 MB §4.1 line; cold remainder = §5.1
  loss 6), C39 (C33 BUNDLE lazy-verify TOCTOU ⇒ bind-time verification;
  app-key fallback copies to anonymous memory; §4.3 memory claims re-scoped).
  Survey rows 12-13 added. Compressions recorded in I.6. Main doc 49,998
  bytes.
- **2026-06-10 — draft 10 (round-7 adversarial review applied).** 6 external
  findings; every tree-fact claim independently re-verified against this tree
  (Annex J). **All 6 accepted, 0 refuted.** One blocker: C40 (the §3.1 row-2
  "load-time identity precondition" was UNDEFINED and every sanctioned guard
  fallback was circular against the table-loaded value — a pre-bind
  monkey-patched builtin at an intrinsified site executed pristine-builtin
  behavior unguarded; now a per-kind behavioral-identity check, bind-verified
  before subscription). Majors: C41 (DFG `SpeculationRecovery` absent from
  ramp identity/EXITMETA/recovery interpreter — silent value corruption on
  dedup'd ramps), C42 (the `checkSwitchToJIT` counter lives on the SHARED
  UnlinkedCodeBlock while selection state is per (function × realm); jitless
  `replace` slow path calls `dontJITAnytimeSoon` on first trip ⇒ the C20
  failure class resurfaced per-realm; slow paths now MODIFIED, re-arm rule
  specified), C43 (AOT-D16's array floor cited the FuzzerAgent seam, which
  only covers ValueProfile predictions; array floor re-grounded on channel-1
  ArrayProfile saturation), C44 (§4.1 KEEP sum stale after C38 — 14.70
  excluded the +0.50 builtin row; §4.2 bridge applied -Os/LTO factors to
  flag-immune precompiled bytes; sums restated 15.20/17.9, bridge split
  compressible vs fixed), C45 (no delivery mode matched Bun's actual
  distribution; BUNDLE-EMBEDDED defined, per-platform matrix + composed
  app-key startup/RSS gates recorded). Charter 3 gains the patched-builtin
  rows, charter 5 the undo case, charter 9 item (vi). Compressions recorded
  in J.7.
- **2026-06-10 — draft 11 (round-8 external verification applied).** 7
  external findings; every tree-fact claim independently re-verified against
  this tree, with the size claims re-measured on the Annex C binary
  (Annex K). **All 7 accepted, 0 refuted.** C46 (exit-time inlinee-CodeBlock
  materialization failure: fatal-resource carve-out recorded — the one
  bounded exception to "any failure ⇒ LLInt"; never UB), C47 (non-bundle
  import edges — native addons, `node:`/`bun:` builtins, externals — yield
  BOTTOM facts; values never foldable nor comparands), C48 (arm64e/PAC
  mechanism enumerated: bind-time slot signing, recovery-time return-PC
  signing, gate-map discipline, ptrauth relocations; E.2 gains a PAC column),
  C49 (E.2 extended with the LLInt `*_return_location` label family,
  checkpoint trampoline, and the array-sort comparator trampoline as a
  shipped CODE body; recovery interpreter derives the return-location symbol
  — dedup key unchanged), C50 (the gated shipping flavor includes the
  Inspector; KEEP sum 15.20 → 15.81 MB with C51; the conservative bridge end
  now EXCEEDS the 10 MB gate — recorded, milestone build decides), C51
  (`ENABLE(AOT_RUNTIME)` allocation re-measured +0.15 → +0.25; F10's
  `StructureStubInfo` naming re-grounded to `PropertyInlineCache`/
  `InlineCacheHandler`), C52 (BUNDLE-EMBEDDED platform-signature rows require
  code-signature COVERAGE of the embedded blob + re-sign after embed; loader
  verifies, else app-key/Tier L). Charter gains item 10 (K.9). Compressions
  recorded in K.8. Main doc 49,997 bytes.
- **2026-06-10 — draft 12 (round-9 external verification applied).** 6
  external findings; every tree-fact claim re-verified against this tree
  (jettison's `installCode` leg, the DFG/FTL exit-ramp profile-refinement
  blocks, `Debugger::recompileAllJSFunctions`, `ScriptExecutable::clearCode`,
  and the section sizes re-measured by `readelf` on the Annex C binary).
  **All 6 accepted, 0 refuted** (Annex L). C53 (BLOCKER: watchpoint fire must
  uninstall entrypoints, not just clear the bit — the fresh-call window),
  C54 (exit-time profile refinement dropped EXPLICITLY; F3 class (j)/G.8
  item (g); G.6 re-selection re-grounded on LLInt re-profiling via
  deselect-to-LLInt-resident), C55 (debugger attach / `deleteAllCode` /
  `clearCode` ⇒ demotion sweep; binding lifetime; AOT-D7 lifecycle re-bind
  carve-out), C56 (AOT-I10 milestone measurement promoted to design-freeze
  PRECONDITION + pre-committed remediation order), C57 (§4 KEEP sum split
  factor-eligible vs flag-immune; band restated 8.0-9.7, ~10.8 conservative;
  non-text line added). Charter gains L.7 rows. Compressions in L.6.
- **2026-06-10 — draft 15 (round-12 FRESH-EYES FINALIZATION, Annex O).**
  Final round. Skeptical-implementer pass over the whole doc + annexes:
  10-line executive summary added at top (mandated); §1.4 demotion
  thresholds D/S pinned to recorded Options defaults (O.1 — were undefined
  symbols an implementer would have guessed); missing `## 5. Performance`
  section header restored. Compose check vs `docs/threads/SPEC-jit.md`
  (rev 12) and `SPEC-ungil.md` (rev 35): NO contradictions; §6 gains
  item (6) discharging jit I2/I8 (vacuous — AOT patches no code) and
  jit I21 (poll→invalidation-point adjacency preserved by construction)
  explicitly (record O.2). §4 size table re-verified against
  `WebKitBuild/Release/bin/jsc` as rebuilt 2026-06-10 08:27 (O.3): .rodata,
  eh_frame/data, icudt75_dat exact matches; .text and operation* drifted
  ~+1.4% from ongoing threads work — conclusions and the AT-RISK verdict
  unchanged. Cap-funding compressions recorded O.4-O.6 (full text
  preserved there). Main doc 49,999 bytes.

## Corrections log

- **2026-06-10 / C1 (AOT-D2):** Claim "ramp inputs are all compile-time data; `compileExit`
  REUSED" — **false**: `compileExit`/`adjustAndJumpToTarget` bake per-VM/per-exit heap
  addresses (full table Annex D/F3). Corrected to parameter-block ABI; ledger row
  REUSED→MODIFIED; dedup re-keyed post-parameterization (draft-1 key was unsound:
  wrong-function frame reconstruction). Impact: §1.3, §2.3, §2.4.
- **2026-06-10 / C2 (AOT-D9):** Claim "data ICs pre-seeded monomorphic/polymorphic" —
  unimplementable: handler bodies are runtime-`LinkBuffer`-generated
  (`bytecode/InlineCacheCompiler.cpp:1482-1770,5501,5542`). Corrected to v1
  self-access-only; perf expectations downgraded (§5.1.3). Impact: §3.1 row 7, §5.1.
- **2026-06-10 / C3 (§2.3 wrapper-hook story):** Claim "recording hook on typed wrappers
  makes immediates relocatable" — false for FTL (`weakPointer` returns a plain B3
  constant, `ftl/FTLLowerDFGToB3.cpp:28143`; anonymous immediates by
  `LinkBuffer::copyCompactAndLinkCode`). Replaced by AOT-D12 (table loads at lowering);
  AOT-I5 restated. Impact: §2.3, §7.
- **2026-06-10 / C4 (§1.2):** Claim "install with `JITType::DFGJIT` so jitType()-keyed
  logic behaves correctly" — wrong (no exit target via alternative chain; wrong GC
  policy; that logic is largely compiled out of jitless builds). Replaced by AOT-D10.
- **2026-06-10 / C5 (§3.1 row 2):** Draft-1 "hard error for unpathable frozen cells"
  made ordinary ESM apps unbuildable. Replaced by AOT-D13 lattice; path language
  extended to module-environment slots of shipped modules.
- **2026-06-10 / C6 (§2.3/§4.1 "runtime REUSED UNCHANGED (jitless config)"):** the
  product needs DFG data structures that are `#if ENABLE(DFG_JIT)` today — that build
  configuration did not exist. Added `ENABLE(AOT_RUNTIME)` ledger row + size line.
- **2026-06-10 / C7 (whole-doc; constraint-level):** Drafts 1-3 made RECORDED-profile
  training runs the design's spine ("Tier A ... against RECORDED profiles",
  `--aot-record`, profile-pack §2.1, replay §2.2, profile-staleness §5.1.2) — this
  violates hard constraint C2b (no training runs; static-primary). Not a tree-fact
  error: the seeding *mechanism* (slot-injection at the
  `getPrediction`/`getArrayMode`/Status funnels) survives unchanged; the fact *source*
  is replaced by whole-bundle static analysis (constructor/literal shape inference,
  module-graph constant + callee propagation, universal watchpoint assumptions —
  survey §1.9), with `UntypedUse` generic-but-direct lowering where nothing is proven.
  AOT-D4 revised, AOT-D14 + AOT-I12 added, AOT-D9 seeds now static-shape-fact-sourced
  and runtime-warmable via writable data-IC tables. Profile input demoted to optional
  CI-feed appendix (Annex B.8): it may only fill the same §2.2 slots, never be
  required. Impact: §0, §1.1, §1.4, §1.5, §2 (all), §3.1 rows 2/7, §3.2, §5.1-5.4,
  §7 (AOT-I9, AOT-I12, charter 2).

- **2026-06-10 / C8 (AOT-D15; round 3, blocker):** The design had NO source-text
  story: `Function.prototype.toString` (core C1 surface) was silently unsupported,
  and `CachedSourceProviderShape` encodes origin/URL/directives/positions but never
  the text (`runtime/CachedTypes.cpp:1610-1627`) — CachedBytecode links against an
  externally supplied SourceProvider whose content must match, so §1.1's "never
  re-parses" and §2.5's "skip" were unrealizable/unsound as written. Fixed: AOT-D15
  (SOURCE ships in §2.4 item 2; backs the provider, `toString`, `Error.stack`, lazy
  parse of demoted/skipped functions); §2.5(c) restated (no Tier-A code; bytecode +
  source always ship — static unreachability is undecidable under `eval`); charter
  item 2 gains toString-equality. Rejected: shipping bytecode without source (a C1
  fork).
- **2026-06-10 / C9 (AOT-D4 r3; round 3):** "Proven callees/structure sets as
  synthesized Status-object inputs ... identical formats, same parser, zero churn" —
  refuted: Status objects are computed BY the parser, not consumed by it, and the
  LLInt-side readers are strictly monomorphic (`CallLinkStatus::computeFromLLInt`
  single `lastSeenCallee`, `bytecode/CallLinkStatus.cpp:228`;
  `GetByStatus::computeFromLLInt` single structureID + non-Default bail,
  `bytecode/GetByStatus.cpp:56-96`); polymorphic Status state exists only as
  baseline-IC output, post-execution. Fixed: AOT-D4 split into three channels; a
  tool-only static-fact provider at the three Status funnel call sites
  (`dfg/DFGByteCodeParser.cpp:1463,1529,7483`); **DFGByteCodeParser relabeled
  MODIFIED (tool-only)** in the ledger. Two review findings merged (same root cause).
- **2026-06-10 / C10 (AOT-D16; round 3, blocker):** "Sites with no fact ... compile
  via the generic paths — existing cold-code behavior" — refuted: `getPrediction()`
  emits `ForceOSRExit` whenever the profile reads SpecNone
  (`dfg/DFGByteCodeParser.cpp:1052-1063`), and empty ArrayProfiles yield
  `Array::Unprofiled` (`dfg/DFGArrayMode.cpp:72`) treated as ForceExit for normal
  array ops (`dfg/DFGFixupPhase.cpp:5211-5220`) — under AOT-D14 most Tier-A functions
  would deterministically exit at their first unproven value site and §1.4 would
  demote them, gutting the static-primary mode. Fixed: AOT-D16 — the tool installs a
  StaticFactAgent at the existing FuzzerAgent prediction-override seam
  (`dfg/DFGByteCodeParser.cpp:1001-1003`, already `& SpecBytecodeTop`-masked)
  supplying facts or `SpecBytecodeTop`, plus ArrayProfile saturation to generic
  observed modes for fact-less array sites. Honest cost recorded (§2.6 item 4, §5.1
  net): Top predictions propagate (tree's own comment at `:1025-1028`), widening
  generic regions. Parser stays UNCHANGED for this channel (the seam exists).
- **2026-06-10 / C11 (AOT-I5; round 3):** The decidable choke-point list
  (`FrozenValue`/`constInt64`/`TrustedImmPtr`) missed the highest-frequency baked
  class: 32-bit StructureID immediates via `branchWeakStructure`
  `TrustedImm32(structure->id().bits())` (`dfg/DFGJITCompiler.h:261-265`) and FTL
  `weakStructureID` `constInt32(...)` (`ftl/FTLLowerDFGToB3.cpp:28154-28157`; also
  `:19392,19570,23617`) — per-process IDs exactly as unsound to bake as pointers, and
  invisible at the named choke points (an ID int32 is indistinguishable from any
  int32). Fixed: AOT-I5 extended to the structure-ID wrapper constructors; AOT-mode
  lowering routes IDs through the data table at those wrappers. Wording correction to
  AOT-I5/AOT-D12, not a new mechanism.
- **2026-06-10 / C12 (§3.1 row 10; round 3):** "Virtual-call thunk bodies precompiled
  in the shared-thunk section" had no per-immediate audit; `virtualThunkFor` bakes
  the per-VM `vm.getCTIInternalFunctionTrampolineFor(kind).taggedPtr()`
  (`jit/ThunkGenerators.cpp:303-305`), which in JIT mode is itself a runtime jitStub.
  Fixed: every shipped thunk body gets an Annex-D-style immediate audit (Annex E.2);
  the InternalFunction trampoline becomes class-R against the LLInt labels the
  jitless fallback already resolves to (`runtime/VM.cpp:1523-1535`); `Options`
  branches flavor-pinned (AOT-I8); emission via `AOTObjectLinkBuffer` over the
  existing generators.
- **2026-06-10 / C13 (AOT-D17; round 3):** OSR exit through inlined frames
  dereferences inlinee LLInt-CodeBlock-derived parameter-block slots (Annex D F3
  e,g), but the bind sequence never materialized inlinee CodeBlocks, and eager
  creation can be impossible: `ScriptExecutable::newCodeBlockFor` requires
  `(kind, JSFunction*, JSScope*)` (`runtime/ScriptExecutable.h:115`) and the inlinee's
  closure may not exist at caller bind. Unfilled slot ⇒ ramp dereferences null — UB
  violating AOT-I3. Fixed: AOT-D17 (bind fills existing; UNMATERIALIZED tag routes
  the exit through the C++ recovery interpreter which materializes from the
  reconstructed frame's callee/scope and fills the slot); AOT-I4 extended; explicit
  strong-GC-reference rule for CodeBlock-typed table entries. Full protocol: Annex
  E.3.
- **2026-06-10 / C14 (§4, round 3):** Size-budget corrections, merged: (a) "<10 MB is
  met" overclaimed an unmeasured ~40-45% -Os/LTO/compile-out reduction — softened to
  FEASIBLE-IFF (survey's binding form) with the unmeasured 9.49 MB bucket named as
  residual risk and a first-milestone `ENABLE(JIT)=OFF` -Os measurement gate; (b) the
  KEEP parenthetical summed to 14.20, not the stated 14.15; (c) wasm glue was
  unaccounted — measured now at ≈0.50 MB (nm over non-compiler `JSC::Wasm::` text;
  Annex C) and added as a KEEP line (sum 14.70, raw ≈17.4 MB); (d) the C++ recovery
  interpreter was double-counted (DFG row + NEW-runtime row) — de-duped to the
  NEW-runtime line; (e) per-app artifact size was unbudgeted despite LINKED mode
  making it app-binary bytes — §4.4 added (expansion-factor measurement protocol,
  2× source-bytes default cap, demotion order, CI publication) and §5.4 gains the
  Hermes/XS footprint row. Per Annex C's rule, these are corrections-log entries, not
  quiet table edits.

- **2026-06-10 / C15 (AOT-D0/AOT-I13; round 4, blocker):** "SHAPE axis collapses
  under symbolic slots (one mono-shape body serves any observed structure — the IC
  insight)" imported the IC insight incorrectly. IC handler stubs work because
  `InlineCacheHandler` carries the LAYOUT as runtime data
  (`offsetOfOffset`/`offsetOfNewStructureID`/`offsetOfNewSize`/`offsetOfOldSize`,
  `bytecode/InlineCacheHandler.h:130-143`) and bodies are selected per access
  shape. DFG/FTL bodies COMPILE the layout in: `compileGetByOffset` emits a fixed
  address displacement (`dfg/DFGSpeculativeJIT.cpp:15696-15724`,
  `offsetRelativeToBase`), and inline-vs-out-of-line is a compile-time branch
  selecting different base registers (`ftl/FTLLowerDFGToB3.cpp:13590,13708`). The
  AOT-I13 guard checks live-structure == bound-slot, which PASSES by construction
  for the very structure the selector chose — a same-shape-different-layout
  structure (insertion order, inline capacity) would read/write wrong memory with
  no exit. "Binding is a hint, the guard is the truth" was FALSE for layout. Fixed:
  the tool records each variant's FULL **layout signature** (per-site property
  offset, inline/out-of-line, indexing type, transition old/new pair + butterfly
  sizes, representation assumptions) in the STATICMETA precondition row; the
  selector validates the observed structure's actual layout against the signature
  before binding — structureID match alone is insufficient; mismatch ⇒ decline.
  Consequence re-derived (amends Annex F.2.5, recorded here, not silently edited):
  one symbolic body serves only LAYOUT-COMPATIBLE structures, not "any observed
  structure"; the SHAPE axis collapses per layout-equivalence class. The hybrid
  V≤2 projection stands (layout-compatible monomorphic is the common case), but
  the claim is now an honest one and charter 9 gains row (iv). Rejected
  alternative: slot-loaded indexed addressing as the universal lowering (a NEW
  conservative addressing form — costs a load+add on every access; would need a
  ledger row; reconsider only if measured decline rates are high).
- **2026-06-10 / C16 (AOT-D0/§3.1 row 1/AOT-I13; round 4, blocker):** Deselection
  poisons/rebinds symbolic slots while variant activations may still be on the
  stack (recursion: inner activation exits and deselects; outer resumes past its
  guards). F.4(4)'s "a racing stale read is caught by the guard" is true only for
  guard-position reads. The design itself created value-position consumers:
  (a) transition puts store the newStructureID slot value into a live object
  header (the `InlineCacheHandler` `m_newStructureID` analog); (b) allocation
  sites consume the structure in store position (`dfg/DFGSpeculativeJIT.cpp:446`
  `TrustedImmPtr(structure)` stored into the new cell, routed through table slots
  by §3.1 row 1); (c) exit-recovery streams may materialize constants from slots.
  A resumed outer frame executing such a consumer after poisoning writes the
  sentinel (or a rebound, layout-incompatible structure) into a live heap cell —
  corruption with no guard on the path. The §3.1 row-2 precedent (weak entries
  guard-position-only) showed the design already knew the distinction. Fixed
  (option a of the review's two): selector-written symbolic slots are
  **GUARD-POSITION-ONLY** — any value/store-position use of a speculation
  constant forces that fact to be compiled in or taken from a load-time recipe
  slot (never rebound/poisoned), making the variant ineligible for symbolic
  treatment of that axis; enforced at the AOT-I5 choke points (the same
  construction-time classification distinguishes guard vs value consumers);
  ValueRecovery/ExitValue streams never source constants from rebindable slots.
  This also retroactively grounds AOT-I11's poison argument: §3.4 now states
  poisoning is sound only for guard-position consumers and C16 makes that true
  for every poisonable slot class by construction. Charter 9 gains row (v)
  (recursion-driven deselect-mid-activation). Rejected alternative: deferred
  deselection (epoch/return-barrier so poison waits for activations to drain) —
  heavier machinery duplicating what jettisoned-code lifetime does today; revisit
  if guard-position-only proves too restrictive for transition-put variants.
- **2026-06-10 / C17 (§2.1.3/§2.4/§3.3; round 4, blocker):** The jitless product
  never creates the scope-variable watchpoint sets guarding §2.1.3's
  constant/callee folds: `SymbolTableEntry::isWatchable` requires
  `Options::useJIT()` (`runtime/SymbolTable.h:238`) and the header states
  "watchpointSet() returns nullptr if JIT is disabled" (`:320-321`); the live
  engine's fold at `dfg/DFGByteCodeParser.cpp:5588-5593`
  (`freeze(moduleEnvironment)` + `tryGetConstantClosureVar`) is sound only
  because of those sets. As written, either a legal post-bind export reassignment
  (`export let f = impl; ... f = patched`, incl. via in-module direct eval) read
  a STALE constant — a semantics break — or all §2.1.3 facts died in product.
  PRECOND also had no entry class for module-environment/scope-variable
  watchpoints. Fixed: (1) `ENABLE(AOT_RUNTIME)` ledger gains a binding audit item
  — enumerate every `Options::useJIT()`/`ENABLE(JIT)`-gated
  watchpoint-creation/maintenance site AOT preconditions depend on and force them
  ON in the AOT flavor (`SymbolTable.h:238` is the proven instance); (2) PRECOND
  catalog gains a scope-variable/module-environment watchpoint entry class; every
  §2.1.3 fact is either backed by such a serialized, bind-subscribed entry or
  compiled as guarded CheckCell/CheckIsConstant; (3) analyzer rule: direct eval
  in a module's scope poisons once-assignment proofs for that module's bindings;
  (4) bind-time rule: a PRECOND entry whose referenced set does not exist fails
  CLOSED, never vacuously passes; (5) charter 3 gains the reassigned-export row.
- **2026-06-10 / C18 (AOT-I6/§3.2; round 4):** AOT-I6 asserted side-effect-free
  resolution with no mechanism: a naive `[[Get]]` (or `[[GetOwnProperty]]` on a
  Proxy) during recipe-base/global-path resolution runs user traps mid-bind,
  under the §6(3) world-state guarantee. Fixed with a normative loader contract:
  every path step must be an own DATA property of a non-exotic ordinary object,
  read via direct structure/`getDirect` inspection; accessor/Proxy/exotic at any
  step ⇒ per-function fail-closed with reason recorded; same rule for recipe
  bases and §3.3 step-2 frozen-value path materialization. Charter 3 gains the
  path-step-exotica row. Divergence detection is now defined trap-free: the walk
  inspects structures, never invokes MOP operations on exotica.
- **2026-06-10 / C19 (§3.3 et al.; round 4):** Multiple realms were never
  addressed (zero grep hits): §3.3's image-wide recipe memoization and §3.1 row
  3's single globalObject slot broke per-realm structure identity and per-global
  watchpoint validation (`isHavingABadTime`/species/saneChain are
  per-JSGlobalObject — `runtime/JSGlobalObject.h:550,557,1202`); cross-realm
  prototype leakage is exactly the bug class fixed at engine level by commit
  51cc3feb7298. Fixed: CODE/EXITMETA/RECIPES are realm-agnostic and shared; ALL
  binding state (data tables, recipe memoization, precondition
  validation/subscription, validity bits, selection state) is keyed per (artifact
  function × JSGlobalObject) — the BaselineJITData per-CodeBlock discipline whose
  offsetOfGlobalObject the design had already adopted without its instantiation
  semantics. Decision recorded: a second instantiation of shipped modules binds
  independently per realm (not v1 fail-closed — independent binding is the same
  code path, keyed differently); ShadowRealm.evaluate / vm-context source strings
  are dynamic code on Tier L (§1.5). Charter 3 gains the two-realm differential.
- **2026-06-10 / C20 (AOT-D0/§1.2/§3.3.4; round 4, blocker):** As specified, the
  overlay selector could NEVER fire: §3.3 binding runs at first link (before
  first call) and installed the generic body, but every selector input and its
  trigger are LLInt-execution-only — `checkSwitchToJIT` ticks
  `UnlinkedCodeBlock::m_llintExecuteCounter` exclusively from the LLInt
  loop/epilogue macros (`llint/LowLevelInterpreter.asm:1830-1836`, invoked at
  `:2946,:3032,:3063`); valueProfile stores (`llint/LowLevelInterpreter64.asm:
  77-94`), GetByIdModeMetadata structureIDs (`llint/LLIntSlowPaths.cpp:1100-1117`)
  and `DataOnlyCallLinkInfo` lastSeenCallee are all LLInt-path writes; a DFG/FTL
  generic body writes none of them and (not speculating on silent axes) never
  exits there. Every shipped variant was dead bytes. Fixed: lattice functions
  validate preconditions and prepare both bodies at bind but DEFER entrypoint
  install, staying LLInt-resident until the counter trips; the selector then
  publishes the variant (facts match) or installs the generic body (decline);
  never-trip functions stay LLInt — acceptable because the threshold bounds the
  cost and a never-hot function did not need the variant. The overlay's REAL
  runtime cost — the LLInt warm-up window (threshold × per-call LLInt slowdown)
  — is now accounted in the §0.5 cost model (amends Annex F.2: new F.2.8 row,
  recorded here): for a counter threshold T and LLInt-vs-generic gap g, overlay
  functions pay ≈ T·g extra interpreted work once per process per realm;
  spine-only functions are unaffected (install at bind). The pure-ARCH-L epitaph
  is restated: it pays this window on EVERY function (not "runs generic until
  counters trip" — the same confusion). Rejected alternative: install generic at
  bind + add lightweight profiling to generic bodies — NEW mechanism with
  per-call cost on every generic body; the deferred-install scheme reuses the
  existing free profiling instead.
- **2026-06-10 / C21 (AOT-D12/§2.3/§3.1 row 4/AOT-I5; round 4):** "Class-R
  targets already flow through call link records, captured natively" was true
  for DFG only; every FTL `vmCall`/operation callee is a plain B3 IntPtr constant
  — `FTLOutput::operation` returns
  `constIntPtr(tagCFunctionPtr<void*, OperationPtrTag>(function))`
  (`ftl/FTLOutput.h:416-417`) — folded and emitted as an anonymous immediate with
  no link record (the C3 weakPointer failure mode, for the design's default
  tier). Fixed: AOT-D12 extends to class-R in FTL at the single
  `FTLOutput::operation` choke point (image-GOT data load or relocation-bearing
  patchpoint in AOT-mode lowering); AOT-I5's construction-time enforcement
  extends to `OperationPtrTag`/`JSEntryPtrTag`-tagged pointer constants
  (including `CodePtr<OperationPtrTag>` overload and lazy-slow-path callees);
  §2.3/§3.1 row 4 restated ("DFG link records captured natively; FTL R-class
  routed at lowering"); ledger row added.
- **2026-06-10 / C22 (AOT-D2/AOT-D11/EXITMETA; round 4):** The exit story was
  exclusively DFG-shaped while §2.5 makes FTL the default tier: FTL exits are
  lazily compiled by `ftl/FTLOSRExitCompiler.cpp` `compileStub` (`:222,936,940`)
  over a different representation — `ExitValue` kinds
  (`ftl/FTLExitValue.h:53-59`: Argument/Constant/InJSStack*/
  MaterializeNewObject) + `ExitTimeObjectMaterialization` — with its own baked
  per-VM/per-exit immediates, none audited by Annex D F3. Fixed (option a — an
  FTL branch; normalization to `Operands<ValueRecovery>` was rejected because
  ExitTimeObjectMaterialization graphs have no lossless ValueRecovery encoding):
  EXITMETA carries parameterized ExitValue/materialization streams; `compileStub`
  gets the same parameter-block treatment and a per-immediate audit (Annex G.8);
  dedup keys over the parameterized ExitValue stream; the C++ recovery
  interpreter covers object materialization (`operationMaterializeObjectInOSR`
  et al. as class-R operations); charter 5 replays both stream kinds; ledger
  gains the `ftl/FTLOSRExitCompiler.cpp` MODIFIED row; AOT-I3 names both streams.
- **2026-06-10 / C23 (AOT-D18; round 4):** Tool-time materialization of
  callee/executable cells was an unstated NEW mechanism the spine's headline win
  (inlining/direct calls) depends on: under C2b no module evaluation runs in the
  tool, so no JSFunction closures/module environments exist, yet CallLinkStatus
  variants are `CallVariant` cells, DFG inlining needs the inlinee's
  FunctionExecutable, and `Graph::freeze`/`DesiredWeakReferences` need real cells
  at plan time; `FunctionExecutable` creation needs a scope
  (`runtime/ScriptExecutable.h:115` — the constraint AOT-D17 acknowledged on the
  product side only). Fixed: AOT-D18 — a tool-side stand-in factory synthesizes
  FunctionExecutables from the bundle's UnlinkedFunctionExecutables against a
  synthetic module scope; CallVariants are built BY EXECUTABLE (aligned with
  AOT-D13's ExecutableID weakening, so every emitted callee identity is pathable
  by construction); stand-in identities map to AOT-D13 paths/ExecutableIDs in
  RELOC/RECIPES, never tool-heap addresses, so AOT-I9 byte-determinism holds
  (identities derive from the §2.1 function-identity map). NEW ledger row.
- **2026-06-10 / C24 (AOT-D19/§3.5/§5.2/AOT-I7; round 4):** BUNDLE mode had no
  entitlement or integrity story; the §5.2 "no JIT entitlement" thesis only holds
  for LINKED. Mapping a data file PROT_EXEC under macOS hardened runtime requires
  the allow-unsigned-executable-memory entitlement class; on iOS it is
  impossible; and the HEADER check validates compatibility, not integrity — an
  unauthenticated artifact mapped executable is arbitrary code execution for any
  writer of the path. Fixed: AOT-D19 — BUNDLE is scoped to desktop/server-class
  platforms; the hardened-macOS entitlement requirement is recorded; iOS =
  LINKED only; the loader verifies artifact integrity (platform code-signature
  on the artifact file, or an app-key-signed hash embedded at tool time) before
  any executable mapping; unverifiable ⇒ Tier L. AOT-I7 caveated: observational
  equivalence above the loader stands; deployability/security properties differ
  and are recorded. §5.2's thesis is scoped to LINKED. (Finding received
  truncated; its verifiable core — §3.5/§5.2 text, platform PROT_EXEC rules —
  was confirmed and accepted; no refuted remainder.)

- **2026-06-10 / C25 (AOT-D2/AOT-D17 × C19; round 5; BLOCKER):** The §1.3
  parameter block was specified as "one image `.rodata` symbol ... carrying
  {resume slot filled at bind, data-table base, ..., counter slot}" while C19/G.5
  keys ALL binding state per (function × JSGlobalObject) — and the block appeared
  in neither G.5 list. Both could not hold: a single bind-filled image symbol
  means realm B's bind overwrites realm A's resume CodeBlock/table base, so a
  realm-A exit reifies frames against realm-B LLInt state (AOT-I3 violation,
  type-confusion path: wrong metadata layout, wrong global); a per-realm block
  cannot be a link-time immediate. Also internally inconsistent: a bind-written
  block is not `.rodata`. Fixed (main §1.3): the parameter block is
  REALM-INVARIANT only (true `.rodata`: exit-descriptor index, inline-frame tree
  refs, ramp-shape constants); every realm-dependent input (resume LLInt
  CodeBlock + metadata/instruction pointers, data-table base, counter slot,
  AOT-D17 inlinee slots) is reached at exit time through the exiting frame's
  per-realm AOT CodeBlock — recoverable from the frame's codeBlock slot, exactly
  how `compileExit` finds state today — into that realm's data table. Annex E.3's
  "idempotent: racing exits compute identical values" claim was FALSE across
  realms (inlinee LLInt CodeBlocks are per-realm cells); amended to PER-REALM-slot
  idempotency (H.1). G.5's shared list gains "exit parameter blocks
  (realm-invariant)" (H.1). Charter 3 gains the two-realm-exit row.
- **2026-06-10 / C26 (AOT-D13 × C19; round 5):** The D13 weakened closure-callee
  guard ("ExecutableID + structure check") did not pin the callee's REALM.
  Today's guard is realm-sound by cell identity: `emitFunctionChecks`
  (`dfg/DFGByteCodeParser.cpp:1571-1594`) freezes the `FunctionExecutable*`,
  which is per-instantiation hence per-realm. ExecutableID (§2.1 identity:
  URL + hash + offset) is realm-AGNOSTIC: under C19 two realms instantiate the
  same bundle into distinct executables with the SAME ID, so a realm-B closure at
  a realm-A inlined site would pass an ID compare and run the inlined body with
  realm-A bindings (per-codeOrigin globalObject baked at tool time routes through
  the binding realm's row-3 slot: sloppy-`this`, allocation-site structures,
  species/instanceof prototypes, per-global watchpoints all realm-A) — silent
  semantics divergence, the cross-realm leakage class 51cc3feb7298 fixed at
  engine level for binding STATE but not for this guard. Fixed (main §3.1 row 2):
  at bind, resolve the ExecutableID to THIS realm's FunctionExecutable (via the
  realm's module-instantiation records) into a per-realm table slot; the guard is
  CELL-equality against that slot (the GetExecutable+CheckIsConstant shape,
  table-loaded per AOT-D12) — never a realm-agnostic ID or shape compare;
  unresolvable-at-bind ⇒ decline the inlining (generic call). Charter 3 row.
- **2026-06-10 / C27 (§3.2/§3.3 × C15; round 5):** C15's lesson (bodies BAKE
  layout; structureID equality against a slot the binder itself wrote proves
  nothing) was applied to the SELECTOR only, not the spine. Two gaps: (1) the
  recipe schema omitted layout-determining `Structure::create` inputs — inline
  capacity (decides inline vs out-of-line placement; the §2.1.1 create_this
  capacity-0 vacancy makes tool-inferred capacities load-bearing) and the
  TypeInfo/ClassInfo discriminator (`runtime/Structure.h:236`); a loader
  resolving a different capacity than the tool assumed yields transition-path
  properties with different inline-ness than the body baked, and the §3.1 row-1
  guard passes by construction ⇒ silent wrong reads/writes on the DEFAULT path.
  The "creating-or-FINDING" clause compounds it (found structures are only
  layout-equal if creation is fully recipe-determined). (2) §3.3 had no load-time
  layout check. Fixed: schema extended (main §3.2); STATICMETA carries one layout
  signature per load-time recipe slot; §3.3 step 1 verifies the RESOLVED
  structure's actual layout against it — mismatch ⇒ per-function fail-closed
  (bind-refusal instead of UB regardless of schema completeness). Charter 3 row
  (perturbed loader capacity/ClassInfo resolution).
- **2026-06-10 / C28 (§2.4 PRECOND; round 5):** The PRECOND catalog was
  open-ended by example with NO closed rule for DesiredWatchpoints entries the
  tool cannot express. Several speculation classes register ONLY as raw
  per-global set pointers via `addLazily(WatchpointSet&)`
  (`dfg/DFGDesiredWatchpoints.h:248-249`) with check-elision semantics — the
  proven instance: `Graph::isWatchingMasqueradesAsUndefinedWatchpointSet`
  (`dfg/DFGGraph.h:946-949`, set at `runtime/JSGlobalObject.h:517,1195`) lets the
  DFG OMIT the masquerades branch from typeof/undefined-equality; the watchpoint
  IS the guard. Masquerades was in neither the §2.4 catalog nor the §2.1.4 list,
  and document.all-class objects are reachable through the JSC C API masquerades
  flag Bun exposes: silently dropping the entry ships speculation with no inline
  check and no subscription ⇒ wrong `typeof` answers, an AOT-I1 violation (not an
  exit). Same gap class: this fork's E1/E2 threads elisions
  (`DFGDesiredWatchpoints.h:284-285`). Fixed: catalog gains the well-known
  per-global-set descriptor class (enumerated, incl. masquerades + threads sets);
  binding closed rule — every DesiredWatchpoints/DesiredGlobalProperties entry
  MUST serialize to a catalog descriptor; inexpressible ⇒ re-emit guarded or
  decline, never drop (C17's fail-closed covered bind-time missing sets;
  this covers tool-time inexpressible entries). Charter 3 masquerader row.
- **2026-06-10 / C29 (ledger/AOT-I5; round 5):** MathICs were absent from every
  per-immediate audit and the reuse ledger. DFG/FTL untyped arith
  (ValueAdd/ArithSub/ArithMul/ArithNegate) bakes `TrustedImmPtr(mathIC)` — a
  tool-VM C++ heap pointer NOT covered by AOT-I5's classes
  (`dfg/DFGSpeculativeJIT.cpp:4891,5068,5742`; FTL
  `ftl/FTLLowerDFGToB3.cpp:2926-3116`) — and the `*Optimize` operations
  regenerate code at runtime (`JITMathIC::generateOutOfLine`,
  `jit/JITMathIC.h:127` — LinkBuffer codegen, AOT-I2 violation). Doubly relevant:
  AOT-D16's generic-but-direct floor routes fact-less arith exactly here. Fixed
  (ledger): tool-side lowering disables MathIC generation and emits the existing
  non-repatching operation path (`dfg/DFGSpeculativeJIT.cpp:5097`
  `nonRepatchingFunction`; FTL equivalent); the `*Optimize` family is dropped
  from the kept AOT_RUNTIME operation set; AOT-I5 extended to tool-VM heap-object
  pointers (`MathIC*`, `Box<MathICGenerationState>`-class) so choke-point
  enforcement is complete. Perf note: shipped untyped-arith sites pay the
  operation call where a JIT would inline an IC — counted under §5.1(2)'s
  generic-but-direct cost. Survey §2 gains row 11.
- **2026-06-10 / C30 (§3.1 row 7 / AOT-D9 grounding; round 5):** The IC story was
  grounded in a type that does not exist in this tree: `StructureStubInfo`
  survives only as a stale comment (`runtime/RaceAmplifier.h:77`), and the survey
  row-7 claim "`useDataIC` exists for upper tiers in OptionsList" is FALSE here —
  the only relevant option is `useHandlerICInFTL`
  (`runtime/OptionsList.h:643`, default false). The real subsystem is
  `PropertyInlineCache`/`InlineCacheHandler`; the data-dispatch form (load
  handler, indirect call through `InlineCacheHandler::offsetOfCallTarget`,
  `jit/JITInlineCacheGenerator.cpp:105-106,124`) exists only for FTL sites; DFG
  sites instantiate `RepatchingPropertyInlineCache` (`:94-97`) = runtime code
  repatching, unusable under AOT-I2 — so §2.5's "DFG-grade otherwise" bodies had
  NO in-tree property-IC emission path. Fixed: row 7 + AOT-D9 re-grounded in the
  real names; ledger decision — tool-only DFG-grade lowering emits the FTL
  handler-dispatch IC form (extending `useHandlerICInFTL`'s form to DFG-tier
  emission, MODIFIED); shipped self-access handler bodies pass the E.2-style
  per-immediate audit; survey row 7 corrected in place (bracketed; original
  text preserved in the brackets' description).
- **2026-06-10 / C31 (§3.1/§2.4/AOTObjectLinkBuffer; round 5):** Switch jump
  tables were missing from the relocation catalog and the artifact format.
  DFG `op_switch_*` dispatches through heap-side tables of ABSOLUTE code
  addresses (`dfg/DFGJITCode.h:312-313`; `CodeLocationLabel<JSSwitchPtrTag>`
  entries, `bytecode/JumpTable.h:44,66-68`, populated at LinkBuffer
  finalization) — not instruction-byte immediates, so they escaped the survey §2
  framing and every taxonomy row; RELOC covers in-CODE references only. BUNDLE
  had nothing to rebuild the tables from; LINKED needed data-section absolute
  relocations the AOTObjectLinkBuffer spec did not cover. Fixed: §3.1 row 11 —
  per-function (case → function-relative offset) arrays in the artifact; BUNDLE
  rebases into the per-function data table; LINKED emits `.data.rel.ro` +
  native absolute relocations; `AOTObjectLinkBuffer` captures the finalize-time
  jump-table writes; StringJumpTable keys resolve through the row-5 atom table.
  Survey §2 gains row 10 + a framing caveat.
- **2026-06-10 / C32 (§4.4/§5.4 vs AOT-D14; round 5; BLOCKER):** The default
  whole-artifact cap ("2× bundle source bytes") contradicted the design's own
  projections (§0.5 / Annex F.2.6: ARCH-S ≈ 5.2 MB code + comparable EXITMETA
  for the 1.2 MB bundle ≈ 8-9× source) by ~4-5×; under its own numbers the
  default demotion pass would strip Tier-A code from most reachable functions,
  contradicting AOT-D14 and §5.1/§5.2. Annex F.2.6's hybrid sentence ("inside
  the §4.4 cap (2× source bytes)") was arithmetically false — it conflated "2×
  the ARCH-S baseline" with "2× source bytes" (amendment recorded in H.8; F is
  not silently edited). The projections are the credible side (the §3.5-derived
  per-function sizes re-verified this round: 23.72 MB text sum, DFG 3.37 MB,
  736 `operation*` syms / 0.362 MB, icudt75 30.73 MB). Fixed: default cap
  restated as 2× the projected ARCH-S baseline (≈10-20× source bytes; reference
  bundle ⇒ ~10-12 MB ceiling), absolute per-app override; §5.4 footprint row now
  carries the honest 10-15 MB-class per-app artifact figure for app-size
  comparisons (vs Hermes ~2-3 MB engine + compact bytecode). AOT-D14 and the
  §5.x claims stand unchanged against the corrected cap.
- **2026-06-10 / C33 (AOT-D19/§4.3; round 5):** BUNDLE integrity verification
  (mandated by AOT-D19 BEFORE any executable mapping) was absent from the §4.3
  startup budget, which counted only the O(1) HEADER check + lazy binding.
  Whole-artifact hashing is O(artifact bytes) — tens of ms for the 10-25 MB-class
  artifacts §4.4 projects, dwarfing the 10 ms binding budget, in the mode whose
  product metric is cold start. Fixed (recorded decision, artifact-format
  change): HEADER carries a page-granular hash tree; the loader verifies per
  region on first executable map (lazy); the signature covers the tree root.
  §4.3 gains the integrity line; CI gates verified-MB/ms (charter 7 scope).
- **2026-06-10 / C34 (AOT-I9; round 5; received truncated):** AOT-I9 had a
  DETECTION mechanism (charter 1 double-build byte-compare) but no PRODUCTION
  mechanism: the tool is full-fat jsc — concurrent DFG plans plus pointer-keyed
  hash containers feeding PRECOND/RELOC serialization vary iteration order with
  ASLR run-to-run, so the invariant would fail routinely. (Finding truncated in
  transit; verifiable core — DesiredWatchpoints et al. are pointer-keyed; plans
  run concurrently — confirmed from the tree; accepted.) Fixed: AOT-I9 gains the
  normative production rule — serializers iterate §2.1-identity/bytecode order
  ONLY, never pointer-keyed container order (sort-before-serialize at each
  serializer); plan scheduling deterministic (fixed worker count with sorted
  merge, or single-threaded); stand-in creation order already deterministic
  (G.9). Charter 1 remains the regression detector.
- **2026-06-10 / C35 (AOT-I2/§2.3 ledger; round 6, blocker):** FTL
  `lazySlowPath()` patchpoints (~28 real sites: object/activation/butterfly
  allocation slow cases — `ftl/FTLLowerDFGToB3.cpp:9983,10060,10435,11935,`
  `19916-19925,21232-21239,23856,24882`) are RUNTIME codegen baked into every
  FTL-grade body and were absent from the design: each emits a patchable jump to
  `lazySlowPathGenerationThunkGenerator` (`:25352`); first hit calls
  `operationCompileFTLLazySlowPath` (`ftl/FTLOperations.cpp:958`) →
  `LazySlowPath::generate` (`ftl/FTLLazySlowPath.cpp:57-72`) — CCallHelpers +
  LinkBuffer + FINALIZE_CODE at runtime, violating AOT-I2 and crashing (no
  executable allocator) the first time a common slow case is reached in a
  shipped FTL body. The reuse ledger's FTL rows (C21/C22/C29) did not cover it;
  zero doc mentions. Fixed: tool-side FTL lowering runs each generator EAGERLY
  at compile time — the generator lambdas are constructed at compile time over a
  register snapshot (`:25361-25375`); only `->run()` is deferred today — stub
  emitted into CODE, patchable jump resolved at tool time, stub immediates under
  the E.2/G.8 per-immediate audit (Annex I.1). `operationCompileFTLLazySlowPath`
  + the generation thunk leave the kept op/thunk set; AOT-I2 nm-audit symbol
  list extended; survey §2 row 13. Ledger row added.
- **2026-06-10 / C36 (AOT-I5/§3.1; round 6, blocker; two findings merged —
  same root cause):** AOT-I5's "decidable" choke points
  (FrozenValue/constInt64/TrustedImmPtr/`FTLOutput::operation`) provably cannot
  see the `AbsoluteAddress` constructor, which wraps a raw `void*` and bakes
  TOOL-VM addresses of per-VM MUTABLE state into DFG/AssemblyHelpers main-body
  fast paths: write-barrier threshold (`jit/AssemblyHelpers.h:2088,2095`, every
  barriered store), mutator fencing (`:2159`), soft stack limit (`:203` region —
  every prologue check, non-VMLite arm), trap bits
  (`dfg/DFGSpeculativeJIT.cpp:2581` — the product's only termination/safepoint
  poll in Tier-A code under polling traps). Failure mode is SILENT CORRUPTION,
  not exit-to-LLInt: a garbage threshold read SKIPS write barriers; a garbage
  limit compare misses stack overflow; a garbage trap read misses termination —
  violating the charter's degrade-to-LLInt-never-UB requirement. FTL was only
  incidentally covered (`m_out.absolute()` → constIntPtr). Verification nuance
  recorded: the `&vm().didEnterVM` sites (`dfg/DFGSpeculativeJIT.cpp:2348,2388`)
  are `Options::validateDFGClobberize()`-gated validation code — disabled
  tool-side, not table-routed; CountExecution likewise. Fixed: §3.1 row 12
  (class M: per-VM mutable fast-path state — GC-phase-mutable, so reached as
  OFFSET LOADS off the row-3 table-loaded VM pointer, never snapshotted as
  values, never absolute); AOT-I5 extended to the `AbsoluteAddress` constructor
  (and any raw-address `TrustedImm64`) in tool-mode lowering — non-whitelisted
  address ⇒ hard error; per-immediate audit Annex I.2; survey §2 row 12. This is
  the instruction-operand symmetric of H.4's finalize-time rule.
- **2026-06-10 / C37 (AOT-D2; round 6):** The guard-site → ramp DISPATCH layer
  was unspecified — the only remaining runtime-patching path in the exit story.
  In-tree: `DFGJITCompiler::linkOSRExits` emits per-site
  `move(TrustedImm32(i), GPRInfo::numberTagRegister)` + `patchableJump()`
  (32-bit: `store32(TrustedImm32(i), &vm().osrExitIndex)` — itself a baked
  per-VM address) linked to a GENERATION thunk; first hit lazily compiles the
  ramp and `MacroAssembler::repatchJump`s the site
  (`dfg/DFGJITCompiler.cpp:104-126`; `dfg/DFGOSRExit.cpp:284-295`, incl. this
  fork's gilOff `osrExitJumpDestination` variant); FTL analogous via
  OSRExitHandle. All forbidden/meaningless in the product; with dedup'd ramps,
  per-site identity delivery is load-bearing for AOT-I3. Fixed: AOT-D2 gains the
  exit-site ABI (Annex I.3): failure branch statically linked by
  AOTObjectLinkBuffer to the dedup'd ramp; site identity = exit index
  materialized in a register at the site (the in-tree imm32-in-register
  convention); parameter-block array base realm-invariant, reached
  RIP-relative/class-R. Generation thunks, `repatchJump`,
  `codeLocationForRepatch`, `osrExitIndex` compiled out. Ledger row
  (`linkOSRExits` + FTL OSRExitHandle dispatch MODIFIED, tool-only).
- **2026-06-10 / C38 (AOT-D20; round 6):** The shipped JS builtins were outside
  the AOT universe — permanently LLInt — and the §5.1/§5.4 honesty sections
  omitted it: `s_JSCCombinedCode` = 152 KB builtin JS text compiled at runtime
  (survey §3.3); AOT-D14 is scoped to THE BUNDLE, so
  `Array.prototype.forEach`/`map`/`sort`, %TypedArray%, Promise/async/iterator
  shells would interpret forever around FTL-grade user callbacks — full JSC
  JITs both; Hermes/XS builtins are native C++, so builtin-heavy code could
  LOSE to the engines §5.4 claims to beat on hot loops. DFG intrinsics cover
  only the intrinsified subset at user call sites. Fixed (recorded decision
  AOT-D20, full record I.4): the builtin corpus is fixed at tool-build time —
  precompile a HOT SUBSET (iteration/Promise/iterator shells; selection list
  frozen per engine build, not per app) with the same pipeline, shipped as a
  runtime-resident flavor-hash-keyed artifact section binding via §3.3;
  budgeted as a §4.1 line (+0.50 MB allocation, CI-gated, measured before
  freeze); the cold remainder stays LLInt and is recorded as §5.1 loss (6).
- **2026-06-10 / C39 (AOT-D19/C33 amendment; round 6):** C33's lazy page-hash
  verification is TOCTOU-unsound for file-backed executable mappings on the
  app-key FALLBACK path (generic Linux — Bun's main deployment target): clean
  file-backed PROT_EXEC pages evict under memory pressure and RE-FAULT FROM THE
  FILE, so an attacker with artifact-path write (exactly the G.10 threat model)
  modifies the file post-verification and forces eviction — kernel re-reads
  attacker bytes into an executing mapping. MAP_PRIVATE does not help for
  never-written pages. Platform code signing is immune (kernel re-verifies on
  page-in). Also "first executable map" named no implementable trigger. Fixed
  (C33 amended, not edited — frozen-spec): trigger = BIND time, per
  function-covering region (matches §3.3 lazy binding); the app-key fallback
  COPIES verified regions into anonymous memory before mprotect(PROT_EXEC) —
  RSS = bound CODE bytes, CI-tracked; §4.3's "shared, never copied" and the
  <1 MB private-dirty budget are RE-SCOPED to LINKED mode and
  kernel-verified BUNDLE (platform signing, fs-verity/dm-verity); plain-Linux
  app-key BUNDLE pays the copy or runs Tier L per deployment policy.
- **2026-06-10 / C40 (AOT-D13/§3.1 row 2; round 7; BLOCKER):** The pathable
  frozen-cell "load-time identity precondition" was never defined, and every
  guard form the docs did specify was CIRCULAR: C17 and the C28/H.6 fallback
  compile "guarded CheckCell/CheckIsConstant against the table-loaded value" —
  but the table slot is filled by the C18 path walk against the live heap, and
  the walk validates PATHABILITY, not behavioral identity. Concrete unsound
  walk (legal full-ES, ubiquitous polyfill pattern): Tier-L startup code runs
  `Math.pow = (x,y) => { effects(); return orig(x,y); }` BEFORE the lazy
  first-call bind (§3.3) of a hot function whose `Math.pow` site the tool
  devirtualized against the TOOL VM's builtin cell (AOT-D18 stand-ins cover
  bundle functions only; tool-VM builtins are live cells) and inlined as an
  intrinsic. At bind the Math→pow walk succeeds (ordinary own data property),
  the slot binds the PATCHED function, the CheckIsConstant guard is
  tautologically true, the post-bind equivalence watchpoint watches the
  patched value — and the body runs the intrinsic; `effects()` never runs. No
  exit, no demotion: unguarded wrong answers, violating AOT-I1/AOT-I3 and hard
  constraint C1. Same circularity for cell-valued AI-folded loads (e.g.
  `export const pow = Math.pow` through the §2.1.3 module-constant channel)
  and for any §2.1.4 "intact builtin prototypes" assumption not backed by a
  named per-global watchpoint set (badTime/species/saneChain/masquerades cover
  only their protocols; this tree has no general builtin-method set). C26 did
  not cover it (explicitly scoped to UNPATHABLE bundle closures); C18 only
  bars trap execution during the walk; the charter-3 matrix had no
  pre-bind-patched-builtin row. Fixed (full text J.1): the precondition is now
  a PER-KIND BEHAVIORAL IDENTITY serialized in PRECOND at tool time and
  bind-compared against the path-resolved cell BEFORE any watchpoint
  subscription — (a) bundle function ⇒ ExecutableID (C26 generalized to
  pathable cells); (b) host/native function ⇒ JSFunction whose
  NativeExecutable native pointer equals a recorded class-R symbol, plus the
  intrinsic enum the compiler consumed; (c) engine-builtin JS function ⇒
  builtin-corpus executable index (AOT-D20 flavor-hash identity); (d)
  primitive JSValue ⇒ serialized value equality; (e) any other cell kind ⇒
  fail closed. Mismatch ⇒ per-function fail-closed (Tier L), reason recorded.
  C17's "guarded CheckCell/CheckIsConstant" and C28/H.6's fallback wording are
  AMENDED (recorded, not edited — frozen-spec): the guarded form's comparand
  must itself have passed C40 identity at bind. Charter 3 gains both rows:
  pre-bind patch ⇒ never binds the intrinsified body; post-bind patch ⇒
  equivalence watchpoint fires, bit clears.
- **2026-06-10 / C41 (AOT-D2; round 7):** DFG exits carry a second
  correctness-bearing input the design never mentioned: `SpeculationRecovery`
  — register-state UNDO records (reverse in-place speculative arithmetic,
  e.g. SpeculativeAdd; BooleanSpeculationCheck restoration) passed separately
  to `OSRExit::compileExit` (`dfg/DFGOSRExit.cpp:391`) from
  `DFGJITCode::m_speculationRecovery` (`dfg/DFGJITCode.h:311`) via the exit's
  recoveryIndex, and run BEFORE value recovery. As specced, (a) two exits with
  identical ValueRecovery streams but different undo records dedup'd to one
  ramp ⇒ one skips/misapplies the undo — silent wrong values, AOT-I3 violated
  with no guard firing; (b) the C++ recovery interpreter could not apply the
  undo at all. Fixed (full text J.2): ramp identity = (parameterized
  recovery stream, SpeculationRecovery record-or-none); EXITMETA serializes
  the records; the recovery interpreter applies the undo to the captured
  register file before replaying recoveries; charter 5 gains a
  SpeculativeAdd-undo case. FTL unaffected: B3 exits carry no
  SpeculationRecovery (stated explicitly in §1.3).
- **2026-06-10 / C42 (C19/C20/G.6; round 7):** The overlay trigger is
  per-UnlinkedCodeBlock while selection state is per (function × realm):
  `checkSwitchToJIT` ticks `UnlinkedCodeBlock::m_llintExecuteCounter`
  (`llint/LowLevelInterpreter.asm:1830-1836`;
  `bytecode/UnlinkedCodeBlock.h:536,544`), shared per-VM across realms and
  across all closures of one source via the CodeCache — the very payload
  shipped as level-0 CachedBytecode. And the jitless `replace` slow path
  calls `codeBlock->dontJITAnytimeSoon()`
  (`llint/LLIntSlowPaths.cpp:617-637`), permanently deferring the SHARED
  counter on first trip; G.6 said re-arm happens "only if the generic body is
  uninstalled, which v1 does not do". Net: realm A's trip consumed the
  counter; realm B's lattice copy could never trip — the C20 failure class,
  per-realm, and the slow paths were nowhere in the MODIFIED ledger. Fixed
  (full text J.3): LLInt `replace`/`loop_osr` slow paths are MODIFIED runtime
  components under `ENABLE(AOT_RUNTIME)` — they dispatch to the selector for
  the EXECUTING CodeBlock's (function × realm) entry, never call
  `dontJITAnytimeSoon` for lattice functions, and unconditionally re-arm the
  counter to threshold after each trip. The counter is a shared TRIGGER, not
  selection state; spurious cross-realm trips are benign (selector no-ops for
  already-selected realms). Ledger row added; charter 9 gains (vi): two-realm
  lattice function — both realms must reach selection.
- **2026-06-10 / C43 (AOT-D16; round 7):** The doc attributed the array-site
  prediction floor to the FuzzerAgent override — but that hook exists only in
  `getPredictionWithoutOSRExit` (`dfg/DFGByteCodeParser.cpp:1001-1004`) and
  covers ValueProfile predictions exclusively; `getArrayMode` (`:1079-1110`)
  reads the ArrayProfile directly, and observed==0 yields
  `ArrayMode(Array::Unprofiled)` (`dfg/DFGArrayMode.cpp:71-72`) ⇒ ForceOSRExit
  downstream — gutting Tier-A coverage past every fact-less indexed access.
  The floor as cited DID NOT EXIST for array sites. Fixed (full text J.4):
  AOT-D16 split — (a) value predictions: StaticFactAgent via the FuzzerAgent
  seam, as written; (b) array modes: channel-1 ArrayProfile SLOT SATURATION —
  the tool pre-writes a conservative saturated `m_observedArrayModes` pattern
  plus outOfBounds/mayStoreToHole/mayInterceptIndexedAccesses bits per
  Action, so `fromObserved` lands on generic/SelectUsingPredictions, never
  Unprofiled; profile updates are merge-only (`observeArrayMode` =
  atomicExchangeOr, `computeUpdatedPrediction` ORs — `bytecode/
  ArrayProfile.h:276,316-317`), so the seed survives any tool-VM execution.
- **2026-06-10 / C44 (§4.1/§4.2; round 7):** Internal arithmetic of the
  central C3 table went stale after C38: the stated "KEEP sum ≈ 14.70 MB" is
  exactly the sum of the KEEP rows WITHOUT the round-6 "+0.50 builtin
  Tier-A artifact" line (9.49+0.10+0.60+1.11+0.36+0.90+0.28+0.71+0.70+0.30
  +0.15 = 14.70); round 6 added the line without reconciling (the round-4
  reconciliation predates it). Worse, §4.2 applied the extrapolated ~45%
  -Os/LTO/gc-sections reduction to the WHOLE keep set — but the builtin
  artifact section is precompiled DFG/FTL machine code in a resident section;
  compiler flags cannot shrink it. Fixed: KEEP sum = 15.20 MB (14.70
  compressible C++ text + 0.50 flag-immune artifact section), raw ≈ 17.9 MB;
  the §4.2 bridge restated as (compressible 14.70 × factor) + fixed sections
  ⇒ ≈ 8.6 MB midpoint, ~9.7 MB conservative — the <10 MB margin is
  essentially consumed at the conservative end; verdict stays FEASIBLE-IFF
  with zero slack, AOT-I10 gate unchanged. (Round-7 re-verification also
  reproduced the underlying nm bucketing on the current build: DFG 3.37, FTL
  0.99, B3 1.72, Yarr 0.56; `operation*` = 718 syms ≈ 0.36 MB, extern "C",
  inside the kept bucket; icudt75_dat 30.73 MB decimal — survey numbers
  stand.)
- **2026-06-10 / C45 (AOT-D19/§3.5; round 7):** The two delivery modes
  composed into no story for the flagship product's own distribution: Bun
  ships a prebuilt runtime, and `bun build --compile` produces single-file
  executables by APPENDING a blob — no platform-linker step exists, so
  LINKED-as-specified has no mapping onto Bun's app-build flow; hardened
  macOS excludes BUNDLE; and C39 itself names generic Linux — the primary
  target — as the app-key FALLBACK, whose costs (private RSS = all bound
  CODE bytes; hash+copy startup ≈ 10 ms+ for a 10 MB-class artifact at
  ~1 GB/s SHA-256) were recorded separately but never composed into a
  first-N-calls bound. Fixed (full text J.6): BUNDLE-EMBEDDED defined — the
  artifact embedded inside the runtime/app executable itself, file-backed by
  a binary that IS platform-signed on macOS and verity/IMA-coverable on
  managed Linux ⇒ kernel-verified class where available; per-platform matrix
  (macOS/Linux/Windows × {kernel-verified map, app-key copy, Tier L})
  recorded; §4.3 gains a composed app-key startup (hash+copy) + RSS CI gate
  alongside the verified-MB/ms gate.
- **2026-06-10 / C46 (E.3/AOT-I3; round 8):** The E.3 recovery-interpreter
  protocol ran failable CodeBlock creation
  (`ScriptExecutable::newCodeBlockFor`, `runtime/ScriptExecutable.h:115`)
  mid-OSR-exit with no failure contract — an undefined state transition on
  the deopt spine, unreachable in stock JSC (DFG inlines only executed
  CodeBlocks). Fixed (K.1): no-throw GC-deferred region; failure = fatal
  resource exhaustion, a controlled crash — the design's single recorded
  carve-out from "any failure ⇒ LLInt"; alternatives (handler-only bind
  restriction; bind-time reservation) rejected with rationale.
- **2026-06-10 / C47 (§2.1.3; round 8):** "Bundle closed ⇒ once-assigned
  exports are constants" had no rule for import edges resolving OUTSIDE the
  bundle (native `.node` addons, `node:`/`bun:` builtins, externals); an
  analyzer could fold a host export via a C17 watchpoint. Fixed (K.2):
  non-bundle edges yield BOTTOM facts — value never foldable, never a
  comparand; sites generic-but-direct.
- **2026-06-10 / C48 (§3.5/E.2/I.3; round 8):** arm64e was one clause despite
  LINKED being the flagship iOS mode. Fixed (K.3): bind-time slot signing,
  recovery-time return-PC signing (ptrauth intrinsics, frame-address
  modifier), gate-map discipline, `@AUTH`-class emitter relocations; E.2
  gains a PAC column (recorded amendment).
- **2026-06-10 / C49 (E.2/E.3/AOT-D2; round 8):** The E.2 audit omitted the
  LLInt return-location family every inlined-frame exit resolves through
  (`callerReturnPC`, `dfg/DFGOSRExitCompilerCommon.cpp:148-183`;
  JIT-generated thunks `llint/LLIntThunks.cpp:781-801`, absent jitless), and
  EXITMETA never said who resolves the per-opcode symbol. Fixed (K.4): label
  family = class-R rows; array-sort comparator trampoline ships as an
  audited CODE body; recovery interpreter derives the symbol from the
  caller's resume instruction — dedup key unchanged.
- **2026-06-10 / C50 (§4.1/§4.2/AOT-I10; round 8):** The KEEP sum modeled an
  inspector-off flavor the product does not ship (Bun ships `--inspect`);
  per-flavor gating made the table's verdict non-binding on the real flavor.
  Fixed (K.5): Inspector row KEEP for the gated Bun flavor; KEEP sum 15.81 MB
  (with C51); conservative band end ~10.1 MB now exceeds the gate — recorded
  honestly; min flavor budgeted separately.
- **2026-06-10 / C51 (Annex D F10/§4.1; round 8):** The +0.15 MB
  `ENABLE(AOT_RUNTIME)` allocation was unmeasured; re-measurement (K.6) shows
  a ≥0.20 MiB retained-shape floor (CallLinkInfo family 0.217 +
  PropertyInlineCache/InlineCacheHandler 0.144 raw). Raised to +0.25;
  milestone build must compile `ENABLE(AOT_RUNTIME)` ON (a plain
  `ENABLE(JIT)=OFF` build undercounts); F10's `StructureStubInfo` naming
  re-grounded per C30.
- **2026-06-10 / C52 (J.6/§3.5; round 8):** J.6's macOS kernel-verification
  chain omitted its precondition: bytes appended after `LC_CODE_SIGNATURE`
  are not hashed — the kernel never verifies their page-ins; mapping them
  executable is §3.5's own forbidden case. Fixed (K.7): platform-signature
  rows REQUIRE blob coverage by the code directory's hashed range + re-sign
  after embed; loader verifies coverage, else app-key/Tier L (fails closed);
  negative charter test K.9(e).
- **2026-06-10 / C53 (§3.4/§6(2)/§1.4; round 9; BLOCKER):** §3.4 specified
  fire ⇒ clear bit only; entry uninstall existed only as §1.4 EXIT-driven
  demotion. But validity checks sit where DFG plants jump replacements —
  effectful/call sites, never function entry — so a FRESH call to a
  not-on-stack function after an unrelated fire would enter Tier-A at the
  prologue and consume a folded watchpointed fact (saneChain hole-skip,
  folded property load) before any check: silent wrong values (AOT-I1/I3
  violation), not a deopt. In-tree soundness is two-legged: (a)
  `CodeBlock::jettison` synchronously reinstalls `alternative()`
  (`bytecode/CodeBlock.cpp:2803`, by the "Jettison can happen during GC"
  comment) so fresh calls never enter invalidated code; (b) jump
  replacements catch in-flight frames. The design replicated only (b).
  Fixed (L.1): fire handler clears bit AND uninstalls every subscribed
  (function × realm) entrypoint before returning to JS; §6(2) extended;
  charter row L.7(a).
- **2026-06-10 / C54 (AOT-D2/F3/G.8/G.6/§1.4 step 0; round 9):** The
  ramp-compile-time profile-refinement block — `dfg/DFGOSRExit.cpp:444-560`
  bakes a `baselineCodeBlockFor(exit.m_codeOriginForExitProfile)`-derived
  `ArrayProfile*` and `MethodOfGettingAValueProfile` bucket addresses
  (`:535`); FTL twin `ftl/FTLOSRExitCompiler.cpp:408-435`
  (`TrustedImmPtr(arrayProfile)` `:412`, `emitReportValue` `:434`) — was
  absent from the F3 and G.8 audits, and these are PER-REALM baseline-
  CodeBlock addresses, incompatible with C25 realm-invariant parameter
  blocks. Worse, G.6's single-re-selection clause cited "the exit site's
  own metadata" as evidence — a channel the design never serialized, while
  deselect-to-generic froze LLInt profiles at first-trip state: internally
  inconsistent. Fixed (L.2): DROP exit-time profile writeback in v1 (F3
  class (j), G.8 item (g) — recorded here, D/G not edited);
  `m_jsValueSource`/`m_valueProfile` explicitly EXCLUDED from EXITMETA;
  dedup key unchanged. G.6 amended: deselection returns the function to
  LLInt-RESIDENT deferred-install state (entry uninstall, counter re-armed)
  — LLInt metadata re-profiling IS the re-selection evidence; one re-trip,
  then sticky generic. Costs a second C20 warm-up window; buys a real,
  realm-correct evidence channel through machinery that already ships.
- **2026-06-10 / C55 (AOT-D7/AOT-D10/§3.4; round 9):** Debugger attach and
  code-lifecycle paths were never walked: `Debugger::recompileAllJSFunctions`
  (`debugger/Debugger.cpp:600`) calls `deleteAllCode` — breakpoints work by
  DESTROYING optimized code (Tier-A bodies contain no `op_debug`, so a
  breakpoint in a still-bound function would silently never fire, in the
  flavor that ships the Inspector per C50); `ScriptExecutable::clearCode`
  (`runtime/ScriptExecutable.cpp:165`; memory-pressure note `:508`) clears
  installed code under AOT-D10's "permanent" LLInt CodeBlock; and AOT-D7
  made any such demotion process-permanent (one `--inspect` attach would
  strand the app jitless forever, uncosted). Fixed (L.3): lifecycle events
  ⇒ C53 demotion sweep; binding table strong-refs both CodeBlocks; AOT-D7
  amended with the lifecycle re-bind carve-out (lazy §3.3 first-link path,
  same code); attached-debugger = jitless speed, recorded cost; charter
  rows L.7(b).
- **2026-06-10 / C56 (§4.2/AOT-I10; round 9):** The conservative bridge end
  exceeded the gate (C50) while the recorded fallback levers were foreclosed
  for the gated flavor: Inspector KEEP is C50-bound, the retained bucket is
  a C51 measured FLOOR, and the builtin-list shrink (−0.50) is flag-immune.
  A single unmeasured number — the -Os/LTO factor — decided pass/fail with
  no remediation path, discovered only at the milestone. Fixed (L.4): the
  milestone measurement is now a DESIGN-FREEZE PRECONDITION (cheap relative
  to everything it gates; the tree has the build infra), plus a
  pre-committed, Bun-flavor-valid remediation order.
- **2026-06-10 / C57 (§4.1/§4.2; round 9):** The bridge factor was applied
  uniformly to "15.31 compressible C++ text", but that sum contains bytes
  the factor cannot touch: the offlineasm LLInt blob (0.38, generated asm),
  the post-factor +0.30/+0.25 allocations (budget outputs multiplied as
  inputs), and ICU's already-size-tuned 1.11. And "~2 MB eh_frame/data"
  undercounts the measured -O2 full-binary total: 2.28 (`.eh_frame`) + 0.32
  (`.eh_frame_hdr`) + 0.47 (`.data.rel.ro`) + 0.08 (`.data`) = 3.15 MB
  (readelf, Annex C binary), with no stated KEEP-scaling assumption — and
  the class-R GOT (§3.1 row 4) adds `.data.rel.ro` the budget never lined.
  Fixed (L.5): KEEP split factor-eligible 13.27 / flag-immune+post-factor
  1.43 / ICU 1.11 at its own ~0.9 factor; band restated 8.0-9.7 optimistic,
  ~10.8 conservative (direction unchanged, magnitude honest); non-text KEEP
  line ≈ 2.1 MB with the scaling assumption recorded. Reinforces C56.

## Annex A — Decision index (one-line, for review navigation)

| ID | §(main) | Decision | Rejected |
|---|---|---|---|
| AOT-D0 | 0.5 | Architecture = HYBRID: ARCH-S static spine (whole-bundle analysis seeds DFG/FTL, one body default) + pruned ARCH-L overlay (V≤2 symbolic-slot variants only on statically-silent hot candidates; LLInt `checkSwitchToJIT` counter repointed compile→select; binding = data writes, `publishHandlerChainHead` discipline; deopt descends variant→generic→§1.4) — earned by the §0.5 cost model | pure ARCH-S (forfeits free LLInt profiling signal; 2-5× generic gap on silent-hot code permanent); pure ARCH-L (V≥3 lattice floor everywhere ⇒ 25-30 MB-class app `.text` in LINKED mode, per-variant EXITMETA multiplies the measured 61%-ramp share; cold-start unpredictability; inlining/direct-call wins unrecoverable at runtime); per-site-only data ICs with no function variants (cannot recover unboxing/regalloc specialization — representation is compiled in) |
| AOT-D1 | 1.2 | No OSR *entry* in v1; function-granular Tier-A entry only | catch/loop entrypoints via `m_catchEntrypoints` (deferred, data already in EXITMETA) |
| AOT-D2 | 1.3 | Exit ramps precompiled offline (dedup by recovery stream) + C++ recovery interpreter as backstop/oracle | interpreter-only exits; generic parameterized asm ramp |
| AOT-D3 | 1.4 | Post-exit policy: precondition-class ⇒ permanent demotion; speculation-class ⇒ counter-bounded re-entry | per-site code patching (violates AOT-I2); runtime re-validation timers |
| AOT-D4 | 2.2 | Profile replay by slot-injection into the linked CodeBlock before `DFG::Plan` | parallel profile-source API threaded through DFG phases |
| AOT-D5 | 3.1 | No load-time code patching beyond native relocations; all VM-dependent values via data tables (BaselineJITData discipline) | patch-immediates-at-load (breaks sharing, signing, W^X) |
| AOT-D6 | 3.2 | Structure recipes (deterministic transition-path replay), not build-time heap snapshots, in v1 | XS/Graal-style image heap (imports global side-effect-confined-startup restriction; may layer on later as opt-in) |
| AOT-D7 | 3.4 | No runtime re-validation loop; validity bit set only by the load-time bind pass | periodic re-bind of demoted functions (complexity without field evidence) |
| AOT-D8 | 4.2 | Size budget excludes ICU data (Bun ships ICU regardless); committed fallback: small-icu / Intl-off flavor | counting full 30.73 MB `icudt75_dat` in-budget |
| AOT-D9 | 5.1 | **(corrected, C2)** v1 self-access-only data ICs pre-seeded from profile; all other access forms ⇒ generic slow path; LLInt metadata ICs stay self-updating | runtime IC stub generation (is a JIT; violates AOT-I2); v1 artifact-resident AccessCase handlers (deferred, Annex B.7) |
| AOT-D10 | 1.2 | Binding object model: two CodeBlocks (permanent LLInt CodeBlock = exit target/profiling owner; thin `JITType::AOTJIT` CodeBlock wrapping image sections); old-age jettison disabled; demote = clear bit + uninstall, never free image memory | `JITType::DFGJIT` on a sole CodeBlock (no exit target, wrong GC policy) |
| AOT-D11 | 1.3 | Exceptions: EXITMETA carries codeOrigins pool + handler table + PC→CodeOrigin map; exceptional exits via C++ recovery interpreter in v1; unwinder/StackVisitor get AOT-frame support | refusing to AOT functions with exceptional exits (pass-through frames need the metadata anyway) |
| AOT-D12 | 2.3 | Class-M/V pointers never become B3/assembler constants: lowered as data-table loads; AOT-I5 enforced at constant construction | relocatable-constant B3/Air value kind (relabels B3+Air MODIFIED; more work) |
| AOT-D13 | 3.1 | Frozen-cell pathability lattice: pathable (incl. module-env slots of shipped modules) ⇒ table+precondition; unpathable-droppable ⇒ weaken to ExecutableID+structure guard or decline; emitter-reaching unpathable ⇒ hard error | tool-time hard error for all unpathable cells (unbuildable apps, invites ad-hoc weakening) |
| AOT-D14 | 2.5 | Selection policy: compile-everything-reachable (Java-style); size-driven demotions to bytecode-only (per-function 64 KB cap, whole-artifact cap §4.4, unreachable gets no Tier-A code but bytecode+source still ship per AOT-D15/C8) | hotness thresholds (no profile, C2b); compile-on-demand at load (a JIT, AOT-I2) |
| AOT-D15 | 1.1/2.4 | Bundle SOURCE TEXT ships (SOURCE section or content-hash-referenced app file): backs the CachedBytecode SourceProvider, `Function.prototype.toString`, `Error.stack` positions, lazy parse of demoted/skipped functions | shipping bytecode without source (silent `toString` break — a C1 fork; unrealizable "never re-parses" claim) |
| AOT-D16 | 2.2 | Prediction floor: StaticFactAgent at the existing FuzzerAgent override seam (`dfg/DFGByteCodeParser.cpp:1001-1003`) supplies facts or `SpecBytecodeTop`; fact-less array sites saturated to generic observed modes (`Array::Generic`); Top-propagation cost recorded §2.6(4) | leaving profiles empty (ForceOSRExit guts Tier A — C10); modifying getPrediction/FixupPhase directly (seam already exists; would relabel parser rows) |
| AOT-D17 | 3.3 | Inlinee CodeBlock slots: bind fills existing CodeBlocks; UNMATERIALIZED tag ⇒ exit routes via C++ recovery interpreter, which materializes from the reconstructed frame's callee/scope and fills the slot (store-release); CodeBlock table entries strong GC refs | eager creation at bind (impossible: closure/scope may not exist, `runtime/ScriptExecutable.h:115`); bind-refusal for functions with unmaterializable inlinees (rejects most of the bundle under AOT-D14) |

| AOT-D18 | 2.3 | Tool-side stand-in cell factory: FunctionExecutables synthesized from UnlinkedFunctionExecutables against a synthetic module scope; CallVariants by executable (ExecutableID-aligned, AOT-D13); stand-ins enter RELOC/RECIPES as paths/IDs, never addresses (AOT-I9) | executing the bundle in the tool to materialize real closures (violates C2b determinism and runs user code at build); inlining only when a live closure exists (guts the CHA-analog wins) |
| AOT-D19 | 3.5 | BUNDLE mode scoped: desktop/server-class platforms only; loader verifies artifact integrity (platform code-signature or app-key-signed hash) before PROT_EXEC mapping; iOS = LINKED only; §5.2 no-entitlement thesis is LINKED's | unauthenticated PROT_EXEC mapping (arbitrary code execution via artifact overwrite); claiming no-entitlement deployability for BUNDLE on hardened macOS (false — it needs the very entitlement class the thesis avoids) |

(AOT-D2 also corrected — see Corrections C1; its Annex-A row in draft 1 stands as
superseded by the C1 text. AOT-D4 revised by C7: same slot-injection mechanism, fact
source now the §2.1 static analyzer, not a recorded profile pack. AOT-D9's IC seeds are
static-shape-fact-sourced and runtime-warmable by data writes per C7.)

## Annex B — Deferred items (chartered, not designed)

1. **OSR entry / catch entrypoints** (AOT-D1): EXITMETA already serializes
   `DFGCommonData::m_catchEntrypoints` (`dfg/DFGCommonData.h:130`); v2 may add loop-header
   entry thunks for long-running-loop workloads.
2. **Heap-snapshot mode** (AOT-D6): opt-in XS-style startup snapshot for embedders that
   accept side-effect-confined startup; composes with recipes (snapshot resolves
   category-1/2 constants eagerly; recipes remain the portable path).
3. **Wasm AOT** (main-doc N4): BBQ/OMG output through `AOTObjectLinkBuffer`; the
   relocation list already exists (`wasm/WasmBBQPlan.cpp:94`,
   `wasm/WasmCalleeGroup.cpp:323,435`).
4. **Threads joint design** (main §6): fence/safepoint contract for validity-bit clears;
   bind-as-safepoint-participant under N mutators; flavor matrix already pinned by
   AOT-I8.
5. **arm64e detail:** PAC conventions for data-table function pointers in BUNDLE mode
   (LINKED mode is handled by the platform toolchain).
6. **Profile-feed merge semantics** (multi-run union/intersection policy per profile
   kind, for the OPTIONAL B.8 feed only) — tool-side, does not affect the artifact
   format version.
7. **Artifact-resident IC handlers (v2 of AOT-D9):** offline tool compiles
   statically-predicted (or B.8-fed) AccessCase chains through the object emitter as
   artifact-resident handler bodies indexed by the IC table, with full class-M/V
   treatment of their baked structure IDs/offsets. Re-promotes
   polymorphic/proto-chain/getter sites from slow-path to handler speed without runtime
   codegen.
8. **Optional CI-profile feed (APPENDIX-GRADE per C2b; the ONLY sanctioned profile
   input).** An app's existing CI/test run may execute under full-fat `jsc` with a
   `--aot-profile-out` flag dumping ValueProfile lattices, array profiles, callee sets,
   and prior-deployment demotion reports (§1.4.3) keyed by §2.1 function identity. The
   tool consumes the feed by filling exactly the same §2.2 slots the static analyzer
   fills — facts merge as upgrades (static fact ∪ observed lattice; observed
   polymorphic-site bias orders guard chains; hot/cold weights tune inlining). Restores
   the §2.6 losses (1)-(3). HARD RULES: the artifact must build identically-validly
   with the feed absent (AOT-I12); the feed can never *remove* a static fact's guard or
   widen the artifact's required sections; determinism per AOT-I9 includes the feed
   bytes when present. This is an enhancement appendix, never the spine — no training
   runs are required of any user, ever.

9. **B.9 — Data-table arithmetic fast paths (v2; added r5/C29).** v1 routes
   untyped arithmetic to the non-repatching operations (H.5). A v2 may specify a
   per-site arithmetic profile slot in the writable data table plus shipped
   non-regenerating fast-path bodies keyed on observed int/double kinds — the
   handler-IC discipline (data publish, no codegen) applied to MathIC's problem.
   Requires its own per-immediate audit before any body ships (E.2 rule).

10. **B.10 — Multi-baseline ("fat") artifacts (v2; added r10/C58).** v1 pins
    ONE ISA baseline per artifact (M.1); hosts below it run Tier L wholesale.
    A v2 may ship one CODE section per baseline (e.g. x86_64 baseline +
    AVX2-class), HEADER carrying per-section feature-bit sets; the loader
    picks the widest section whose baseline ⊆ host CPUID. Sections are
    otherwise identical artifacts (same BYTECODE/EXITMETA/RECIPES); size cost
    is CODE-section multiplication, counted against the §4.4 cap.

## Annex D — Round-2 adversarial review record (2026-06-10, BINDING)

External review delivered 11 findings. Each tree-fact claim was independently
re-verified against this tree before acceptance. Disposition: **11/11 accepted, 0
refuted**; F3+F9 merged (same root cause: baked per-VM/per-exit immediates in the exit
path), F5+F11 merged (same root cause: unpathable frozen-cell identities).

| # | Finding (short) | Verified tree facts | Resolution |
|---|---|---|---|
| F1 | Dead weak table entries have no invalidation hook ⇒ guard false-positive UB after address/StructureID reuse | `shouldJettisonDueToWeakReference` `bytecode/CodeBlock.cpp:1478`; `m_transitions`/`m_weakReferences`/`m_weakStructureReferences` `dfg/DFGCommonData.h:124-126` | §3.4 weak-table sweep; new invariant AOT-I11; charter item 3 row |
| F2 | Exceptions/unwinding/stack traces unaddressed (blocker: C1 surface) | `dfg/DFGOSRExitBase.h:59,63`; `interpreter/Interpreter.cpp:695-701`; `CodeBlock::handlerForIndex` `bytecode/CodeBlock.cpp:2324`; `dfg/DFGCommonData.h:120,121,131` | AOT-D11; EXITMETA extended; budget line bumped 0.05→0.10 |
| F3+F9 | Exit-ramp "all compile-time data" false; dedup key unsound | `handleExitCounts` bakes `&exit.m_count`, `CodeBlock*`, `operationTriggerReoptimizationNow` (`dfg/DFGOSRExitCompilerCommon.cpp:42-113`); `metadataTable()`/`instructionsRawPointer()` `:387-391,:495-500`; callee cells `:403`; `didTryToEnterInLoop` `:89`; `baselineAlternative()`+per-frame barriers `:426-450`; `&currentInstruction` `:461ff`; `TrustedImmPtr(&vm)/(&exit)` `dfg/DFGOSRExit.cpp:965-980` (with fork-specific per-lite publication nearby) | AOT-D2 corrected (Corrections C1): parameter-block ABI, `handleExitCounts` dropped, dedup post-parameterization, ledger MODIFIED |
| F4 | `JITType::DFGJIT` on sole CodeBlock breaks exit targeting + GC policy | `isBaselineCode` assert `dfg/DFGOSRExitCompilerCommon.cpp:459-463`; per-tier `timeToLive` `bytecode/CodeBlock.cpp:1485-1516`; `#if ENABLE(DFG_JIT)` gating `dfg/DFGOSRExitCompilerCommon.cpp:29` | AOT-D10 (Corrections C4) |
| F5+F11 | Unpathable frozen cells: hard error unbuildable / module-scope identity cliff | `Graph::freeze` `dfg/DFGGraph.cpp:1634` (arbitrary profile-observed cells incl. closure callees); `frozenPointer(defaultHasInstanceFunction)` `ftl/FTLLowerDFGToB3.cpp:~18608` | AOT-D13 lattice (Corrections C5); module-env-slot paths; perf note §5.1.4 |
| F6 | FTL pointer constants unrecoverable at LinkBuffer seam (blocker) | `weakPointer` returns plain B3 constant `ftl/FTLLowerDFGToB3.cpp:28143-28162`; only branches/calls/labels have link records by `assembler/LinkBuffer.cpp:290` | AOT-D12 table-loads-at-lowering (Corrections C3); AOT-I5 restated |
| F7 | JS-to-JS call linking entirely missing | `setStub` `bytecode/CallLinkInfo.h:241`; `UseDataIC::Yes` RELEASE_ASSERT `:615`; `virtualThunkFor*` `jit/ThunkGenerators.h:59-61` | §3.1 row 10 added; thunks precompiled in shared-thunk section; C++ slow-path backstop |
| F8 | "Pre-seeded polymorphic data ICs" has no executable-handler mechanism | 12 `LinkBuffer patchBuffer(..., Profile::InlineCache)` sites `bytecode/InlineCacheCompiler.cpp:1482-1770,5501,5542`; `useHandlerICInFTL` `runtime/OptionsList.h:643` | AOT-D9 corrected to self-access-only v1 (Corrections C2); v2 = Annex B.7 |
| F10 | Product runtime build config does not exist ("REUSED UNCHANGED (jitless)" mislabel) | `#if ENABLE(DFG_JIT)`: `dfg/DFGCommonData.h:28`, `dfg/DFGOSRExit.h:28`, `bytecode/CodeBlock.h:46,55,514-542,614,634,1086` | `ENABLE(AOT_RUNTIME)` ledger row + 0.15 MB allocation (Corrections C6) |

**F3 per-immediate table (full text for the main doc's compressed citation):** baked
immediates the parameter block replaces — (a) `&exit.m_count` AbsoluteAddress add32;
(b) `jit.codeBlock()` / `jit.baselineCodeBlock()` TrustedImmPtr moves; (c) baked call to
`operationTriggerReoptimizationNow` + `exitCountThresholdForReoptimization*` immediates
(whole `handleExitCounts` dropped — product has no reoptimizer);
(d) `ownerExecutable()->addressOfDidTryToEnterInLoop()` AbsoluteAddress per inline
frame; (e) `metadataTable()`/`instructionsRawPointer()`/`baselineJITData()` for callers
in `reifyInlinedCallFrames`; (f) `inlineCallFrame->calleeConstant()` storeCell;
(g) `baselineAlternative()` + per-inline-frame `baselineCodeBlock` write-barrier
pointers in `adjustAndJumpToTarget`; (h) `&currentInstruction` and exit-target
`metadataTable()`/`instructionsRawPointer()` in the exitToLLInt tail; (i)
`TrustedImmPtr(&vm)`/`TrustedImmPtr(&exit)`/scratch-buffer pointers around
`operationMaterializeOSRExitSideState`. Replacement classes: writable side-table slot
(a); per-function data-table slots bound §3.3 (b,e,g,h); dropped (c,d — reoptimization
machinery); constant-pool/data-table refs (f); parameter-block fields (i, exit
descriptor + VM reached via runtime calling convention).

**F10 piece list (`ENABLE(AOT_RUNTIME)` retained-and-decoupled set):** CommonData-shaped
EXITMETA reader types (inline-call-frame set, codeOrigins pool, PC→CodeOrigin map,
handler table); OSRExit descriptor + ValueRecovery/`Operands<ValueRecovery>`;
CodeBlockJettisoningWatchpoint-equivalent validity-bit watchpoint plumbing; data-IC
`StructureStubInfo` (self-access subset) and `CallLinkInfo` data paths; AOT JITCode
class + alternative-chain support; exit-count side tables. Compiled OUT: all of
dfg/ftl/b3/jit compilers, MacroAssembler+LinkBuffer, ExecutableAllocator pools,
InlineCacheCompiler, `setStub`/repatch, thunk generators. CI bucketing per AOT-I10
counts the retained set as its own bucket.

## Annex C — Measurement provenance for §4 (size budget)

**Round-3 additions (C14):** wasm-glue KEEP line = 0.50 MB, measured 2026-06-10 on the
same binary: `nm --print-size -C` text symbols matching `JSC::Wasm::` minus
compiler-family symbols (BBQ/OMG/B3/Air/JIT/compile/tierup/OSREntry/LLIntPlan/IPInt
generators) = 0.50 MB of 1.95 MB total Wasm text — module parsing/validation,
instance/JS-API objects required for IPInt to be usable. The §4.4 artifact-budget
numbers (5-10× expansion, 2× source-bytes cap) are planning ALLOCATIONS, not
measurements, until the charter-7 reference-app measurement exists; revising them
requires a corrections-log entry.

All numbers in main-doc §4.1 are transcribed from `AOT-SURVEY.md` §3.2-§3.4, measured on
`WebKitBuild/Release/bin/jsc` (RelWithDebInfo, -O2, x86-64 Linux, static, all tiers ON):
`.text` 23.62 MB / `.rodata` 31.37 MB (readelf), 49,898 text symbols bucketed via nm.
Known biases, binding on any future re-measure: (a) -O2 not -Os; (b) x86-64 (arm64 text
~10-15% larger); (c) KEEP buckets include `#if ENABLE(JIT)`-conditional code a jitless
compile removes (overcount in budget's favor); (d) the measured binary links all four
MacroAssembler backends — a shipping build links one or zero. The two NEW-code
allocations (loader/binder+recipes+recovery-interpreter ≈ 0.30 MB; EXITMETA readers
≈ 0.05 MB) are budget *allocations*, not measurements — CI gate AOT-I10/charter item 6
turns them into measurements once code exists; exceeding an allocation requires a
corrections-log entry, not a quiet table edit.

## Annex E — Round-3 adversarial review record (2026-06-10, BINDING)

External review delivered 10 findings. Each tree-fact claim was independently
re-verified against this tree before acceptance. Disposition: **10/10 accepted, 0
refuted**; G2+G5 merged (same root cause: no Status-object injection point; LLInt
slots monomorphic), G8+G9 merged (same root cause: §4 verdict/arithmetic never
reconciled). No finding was a false positive; the closest call was G6 (AOT-I5
wording), accepted as a wording correction because the named choke points genuinely
cannot see a StructureID int32.

| # | Finding (short) | Verified tree facts | Resolution |
|---|---|---|---|
| G1 | No source-text story: `toString`/SourceProvider/lazy-parse unsupported; §2.5 "skip" unsound (blocker) | `CachedSourceProviderShape::encode` writes origin/URLs/directives/positions, never text (`runtime/CachedTypes.cpp:1610-1627`); zero grep hits for toString/SourceProvider in all three docs | AOT-D15 + SOURCE section; §2.5(c) restated; charter toString oracle (C8) |
| G2+G5 | Set-valued static facts cannot enter via slot injection; "synthesized Status inputs" have no injection point; LLInt Status readers monomorphic | `CallLinkStatus::computeFromLLInt` single `lastSeenCallee` (`bytecode/CallLinkStatus.cpp:228`); `GetByStatus::computeFromLLInt` single structureID + non-Default bail w/ FIXME (`bytecode/GetByStatus.cpp:56-96`); Status objects constructed BY the parser at `dfg/DFGByteCodeParser.cpp:1463,1529,7483` — no pass-in parameter | AOT-D4 r3 three channels; tool-only static-fact provider; parser MODIFIED (tool-only) ledger row (C9) |
| G3 | Inlinee LLInt CodeBlocks may not exist at exit time; bind never materializes them; `newCodeBlockFor` needs JSFunction/JSScope that may not exist (UB at exit) | `ScriptExecutable.h:115` `newCodeBlockFor(CodeSpecializationKind, JSFunction*, JSScope*)`; Annex D F3(e,g) slots dereferenced by reified frames | AOT-D17 protocol (§3.3 step 2b); AOT-I4 extended; strong-ref rule (C13); full protocol E.3 |
| G4 | Empty profiles do NOT compile generic: getPrediction ⇒ ForceOSRExit on SpecNone; Array::Unprofiled ⇒ ForceExit — static-primary mode gutted (blocker) | `dfg/DFGByteCodeParser.cpp:1052-1063` (`addToGraph(ForceOSRExit)`, "Give up on executing this code"); `dfg/DFGArrayMode.cpp:71-72` (observed==0 ⇒ Unprofiled); `dfg/DFGFixupPhase.cpp:5211-5220` comment; FuzzerAgent seam `:1001-1003` (`& SpecBytecodeTop`); Top-propagation comment `:1025-1028` | AOT-D16 prediction floor via FuzzerAgent seam + ArrayProfile saturation; §2.6(4)/§5.1 re-derived (C10) |
| G6 | AOT-I5 choke list misses 32-bit StructureID immediates (branchWeakStructure TrustedImm32, FTL weakStructureID constInt32) | `dfg/DFGJITCompiler.h:261-265` (`TrustedImm32(structure->id().bits())` on JSVALUE64); `ftl/FTLLowerDFGToB3.cpp:28154-28157` (`constInt32(structure->id().bits())`); also `:19392,19570,23617` | AOT-I5 extended to the wrapper constructors (C11) |
| G7 | Shared-thunk section has no immediate audit; `virtualThunkFor` bakes per-VM trampoline pointer | `jit/ThunkGenerators.cpp:303-305` (`vm.getCTIInternalFunctionTrampolineFor(kind).taggedPtr()` as TrustedImmPtr); jitless fallback to `llint_internal_function_{call,construct}_trampoline` (`runtime/VM.cpp:1523-1535`) | Annex E.2 thunk audit; row 10 updated (C12) |
| G8+G9 | "<10 MB is met" rests on unmeasured multipliers; KEEP sum off by 0.05; wasm glue unaccounted (~0.5-0.8 MB); recovery interpreter double-counted | Sum re-added = 14.20 ≠ 14.15; non-compiler `JSC::Wasm::` text measured 0.50 MB (of 1.95 total); recovery interp appeared in both the DFG row and the NEW-runtime row; survey's own language is "feasible iff" (`AOT-SURVEY.md` §3.4) | Verdict ⇒ FEASIBLE-IFF + measurement gate; glue line; de-dup; KEEP sum 14.70, raw ≈17.4 MB (C14 a-d) |
| G10 | Per-app artifact size unbudgeted; LINKED mode makes it app-binary bytes on iOS; §5.4 silent on footprint vs Hermes/XS | AOT-D14 compiles everything reachable at DFG/FTL grade; §3.5 LINKED links CODE/EXITMETA/RECIPES into app `.text`/`.rodata`; no expansion-factor or cap default existed | §4.4 artifact budget + §5.4 footprint row (C14 e) |
| (G-ops) | Kept `operation*` surface entangled with dropped JIT machinery; whole files `#if ENABLE(JIT)`; repatch* pulls InlineCacheCompiler/LinkBuffer back in; tier-up ops cannot ship | `jit/JITOperations.cpp:31` (`#if ENABLE(JIT)` wraps file); `dfg/DFGOperations.cpp:112-113`; `repatchGetBy`/`repatchGetBySlowPathCall` calls at `jit/JITOperations.cpp:376,438,452,466,486,491,513,608,681,765` | Ledger row JITOperations/DFGOperations/Repatch MODIFIED; AOT-I2 link-time `nm` audit (part of C14/C9 round; main §2.3 + AOT-I2) |

### E.1 — AOT-D4 round-3 full record (three injection channels)

Channel 1 (slot injection) writes only what the slots can hold: ValueProfile lattices,
ArrayProfile observed-mode masks, LLInt metadata IC fields — all genuinely writable
without execution, but monomorphic by construction on the Status-feeding fields (one
`lastSeenCallee`; one `defaultMode.structureID`, and `GetByStatus::computeFromLLInt`
returns NoInformation for any non-Default mode — the proto-load FIXME at
`bytecode/GetByStatus.cpp:67-69` documents the ceiling). Channel 2 (static-fact
provider) is the only way set-valued facts reach the DFG: a tool-only interface
consulted immediately before/instead of `CallLinkStatus::computeFor` /
`GetByStatus::computeFor` at `dfg/DFGByteCodeParser.cpp:1463,1529,7483`, returning
fully-formed Status objects whose variants reference tool-VM structures (subject to
AOT-D13 pathability and §3.2 recipes). This is a parser modification — tool-only,
~3 call sites, compiled out of the product — and the ledger says so; the rejected
alternatives were (a) fabricating baseline `StructureStubInfo`/`AccessCase` state
without execution (no API exists; InlineCacheCompiler state is produced by the IC
machinery during real execution) and (b) writing LLInt metadata only (silently
degrades all set-valued facts to monomorphic — an undisclosed §2.6-class loss).
Channel 3 (AOT-D16) exists because empty profiles do not mean "generic": they mean
ForceOSRExit (G4). The StaticFactAgent implements the existing `FuzzerAgent`
interface (`VM::fuzzerAgent`, override point `dfg/DFGByteCodeParser.cpp:1001-1003`,
result masked `& SpecBytecodeTop` by the existing code); for ArrayProfiles the tool
saturates fact-less sites' observed-mode masks at injection time so
`ArrayMode::fromObserved` selects generic handling rather than `Array::Unprofiled`
(⇒ ForceExit). Recorded honest cost: per the tree's own comment
(`dfg/DFGByteCodeParser.cpp:1025-1028`), Top predictions propagate transitively, so
the generic-but-direct floor covers regions, not just sites; §5.1's published
expectation accounts for this.

### E.2 — Shared-thunk per-immediate audit (C12; BINDING for §3.1 row 10 / §2.4 CODE item 3)

Thunks shipped in the shared-thunk section, each emitted offline by
`AOTObjectLinkBuffer` over the existing generators (tool-only), with every baked
immediate classified:

| Thunk | Baked immediates today | Product classification |
|---|---|---|
| `virtualThunkFor` regular/tail/construct (`jit/ThunkGenerators.cpp:217-333`) | `vm.getCTIInternalFunctionTrampolineFor(kind).taggedPtr()` TrustedImmPtr (~`:303-305`); `operationVirtualCall` TrustedImmPtr; frame-extent TrustedImm32s | InternalFunction trampoline ⇒ class-R against `llint_internal_function_{call,construct}_trampoline` (the jitless resolution, `runtime/VM.cpp:1523-1535`); `operationVirtualCall` ⇒ class-R operation symbol; frame-extent immediates ⇒ true compile-time constants (ship as-is) |
| Arity-fixup thunk | offsets/constants only + slow-path operation pointer | operation pointer ⇒ class-R; rest compile-time |
| Exception-throw thunks (`getCTIThrowExceptionFromCallSlowPath` family) | per-VM jitStub targets in JIT mode; LLInt label fallbacks exist (`runtime/VM.cpp` same pattern as `:1523-1535`) | class-R against the LLInt labels |
| LLInt OSR-exit trampoline wrappers (`llint/LLIntThunks.cpp:759-779`) | LinkBuffer-generated wrappers around offlineasm labels | NOT shipped as generated wrappers: ramps link class-R directly against the underlying offlineasm labels (already link symbols) |
| `Options`-conditional branches inside any thunk (e.g. `useJSThreads()`) | Options loads/branches | flavor-pinned at tool time (AOT-I8); branch compiled in or out per artifact flavor |

Rule (binding): a thunk body may ship only after its immediate audit lands in this
table; an unaudited thunk in CODE is an AOT-I5-class violation.

### E.3 — AOT-D17 inlinee-CodeBlock protocol (C13; full text)

Problem: reified inline frames (Annex D F3 e,g) need the inlinee's LLInt
`metadataTable()`/`instructionsRawPointer()`/CodeBlock pointer; under AOT-D14 the
inlinee may never have been called in the product VM, so no CodeBlock exists; and
`ScriptExecutable::newCodeBlockFor(kind, JSFunction*, JSScope*)`
(`runtime/ScriptExecutable.h:115`) cannot run at caller-bind time when the inlinee
closure has not been allocated (e.g. an inlined inner function created inside the
caller). Protocol: (1) at bind (§3.3 step 2b), for each transitively inlined frame,
if the inlinee's LLInt CodeBlock already exists, its slots are filled; otherwise the
slot group is set to the UNMATERIALIZED tag (a single distinguished value, never a
valid pointer). (2) Ramp contract: before dereferencing any inlinee slot group, the
ramp tests the tag; tagged ⇒ tail-call into the C++ recovery interpreter for the
whole exit (no partial asm/C++ frame reconstruction). This costs one test per inlined
frame per exit on the ramp path — exits are already off the fast path. (3) The
recovery interpreter, running in C++ with the VM available, recovers the inline
frame's callee `JSFunction` (from `calleeConstant()` or the recorded callee recovery
— the same source `reifyInlinedCallFrames` uses) and its scope from the materialized
frame state, runs `prepareForExecution`-equivalent machinery to create the LLInt
CodeBlock, fills the parameter-block slots with a store-release (idempotent: racing
exits compute identical values; under threads the §6.3 binding/world-state rules
apply), then completes the reconstruction. (4) GC: CodeBlock-typed data-table entries
are STRONG references — visited by the image's owner exactly as a DFG CodeBlock
strongly visits inlinee baseline CodeBlocks today; a bound function's exit targets
can therefore never be collected while the binding is valid. (5) AOT-I4 ordering:
slot fill-or-tag happens before precondition validation; the validity bit is still
set last.

### E.4 — Threads obligations, expanded (§6 compression overflow)

(1) `useJSThreads` checks: branches on `Options` values are ordinary loads the
offline compiler emits like any other; the HEADER option-flavor hash pins the
configuration, and the flavor matrix (charter 8) verifies cross-flavor refusal. Two
flavors (threads-on / threads-off), never runtime-dynamic — Options are fixed at VM
boot. (2) Validity-bit clears: cleared with store-release; observed via the same
safepoint/STW discipline that makes watchpoint fires sound against concurrent
executors. Bit-check placement must satisfy the same "no unbounded watchpoint-blind
execution" window the threads design demands of JIT code: checks at every point the
DFG would have planted a jump replacement, no fewer. The threads design owes the
fence/safepoint contract; this design owes the check-placement guarantee. (3)
Load-time binding (§3.3) must run under the same world-state guarantee as
`Plan::reallyAdd` (VM lock / STW moment today); with N mutators, binding becomes a
safepoint participant — chartered to the joint design (Annex B.4). (4) Demotion
counters are racy-by-design (saturating, advisory); no correctness hangs on them; the
validity bit is the only synchronized control.

## Annex F — AOT-D0 architecture cost model, full record (2026-06-10, BINDING)

Main-doc §0.5 is the capped summary; this annex is the full derivation. All
measured inputs are from `AOT-SURVEY.md` §3.5 (per-function compiled-code sizes,
`JSC_logJIT=1` over 8 JetStream3/Octane workloads on the existing
`WebKitBuild/Release/bin/jsc`, x86-64; typescript = 1.2 MB source, ~1,800 defined
functions, 568 baseline / 637 DFG / 220 FTL compilations observed), §1.10
(data-IC machinery), §1.11 (LLInt profiling inventory).

### F.1 The two candidates, stated fully

**ARCH-S (static spine).** Whole-program bundle analysis (constructor/literal shape
inference, module-graph constant + callee propagation — the CHA analog) seeds
DFG/FTL speculation; generic-but-direct (`UntypedUse`) code where analysis is
silent. One compiled body per function. Runtime never re-decides anything; the only
runtime control is the §1.4 demotion policy. Honest weakness modeled: the history
of JS engines says profiling beats static analysis at exactly the sites that matter
(value ranges, polymorphic bias) — under ARCH-S those sites are generic FOREVER.

**ARCH-L (selection spine).** The runtime remains a profiler. Everything in survey
§1.11 ships and ticks in the jitless product configuration at zero marginal cost:
`valueProfile` stores (`llint/LowLevelInterpreter64.asm:77-82`), ArrayProfile
relaxed-atomic updates (`bytecode/ArrayProfile.h:215,225-229`), LLInt get_by_id
metadata ICs that record the observed hot structureID as data
(`bytecode/GetByIdMetadata.h:98-125`, written by `LLIntSlowPaths.cpp:1111` and
`setupGetByIdPrototypeCache` `:966`), `DataOnlyCallLinkInfo` callee observation
(`bytecode/CallLinkInfo.h:568`), and the `checkSwitchToJIT` execute counter
(`llint/LowLevelInterpreter.asm:1830-1836`) whose triggered slow path simply
declines under `useJIT=0` today. The artifact ships a per-function SPECULATION
LATTICE: a generic body plus shape-specialized variants compiled against SYMBOLIC
speculation constants resolved through patchable constant pools. Runtime tier-up is
SELECTION + LINKING: the counter trips, a selector reads the LLInt profiles, binds
the observed facts into the chosen variant's pool slots (data writes), publishes
the entry pointer. Never codegen, W^X-clean. Deopt descends the lattice before
falling to LLInt. The in-tree proof that this mechanism class works at production
quality is the handler-IC subsystem: `InlineCacheHandler` records carry every
speculation constant as data read by shared stub bodies
(`bytecode/InlineCacheHandler.h:61,130-143,171-189`); installing/upgrading = one
published data write (`publishHandlerChainHead`,
`bytecode/PropertyInlineCache.cpp:1149,1210`); stub bodies are generated per access
SHAPE, not per site (`createPreCompiled`, `bytecode/InlineCacheCompiler.cpp:7475`,
`createPreCompiledICJITStubRoutine` `:7553-7623`). Static analysis's job in ARCH-L
is lattice PRUNING (bounding variant count), not proof.

### F.2 Cost-model inputs (measured)

1. **Bytes per variant, code:** FTL mean 2,682 B / median 1,536 B (n=338); DFG mean
   2,915 B / median 1,168 B (n=906); density 6.5-8 B per bytecode instruction is
   the stable predictor across all 8 workloads (range 4.2-29). Variant size is
   therefore predictable offline from bytecode length. Baseline-shaped code is the
   WRONG generic fallback at 14.6 B/bc (~2× DFG density) — generic bodies must be
   DFG-pipeline output (survey §3.5 consequence 3; consistent with AOT-D14's
   "DFG-grade otherwise").
2. **Bytes per variant, exit metadata:** lazily-compiled exit ramps (only exits
   actually HIT) already total 61% of FTL text bytes (555 KB vs 906 KB) at ~1.2 KB
   median. Precompiled per-site ramps multiply with VARIANT COUNT as well as site
   count; EXITMETA (descriptors + recovery streams) likewise. Metadata, not code,
   is the marginal-variant killer; AOT-D2's dedup + C++ recovery interpreter are
   load-bearing for ANY architecture, and doubly so for lattices.
3. **Guard cycles:** identical between architectures, by construction. AOT-D12
   already bans pointer/StructureID machine immediates product-wide, so every
   speculation guard is a data-table load + cmp + predicted branch in BOTH spines
   (~1-3 cycles over an immediate-cmp; §5.1.5 already budgets this). A symbolic
   slot bound at selection time is byte-identical code to a recipe slot bound at
   load time. Guards therefore do NOT discriminate; this neutralizes the classic
   anti-ARCH-L argument ("indirection tax") for THIS design specifically.
4. **Link/patch cost:** one store-release per pool slot + one entry-pointer publish
   per selection event — the `publishHandlerChainHead` discipline. Nanoseconds per
   function vs milliseconds for a JIT compile; amortized over the counter
   threshold, noise. Not a discriminator.
5. **Variant counts.** The decisive structural fact: SYMBOLIC SLOTS COLLAPSE THE
   SHAPE AXIS. One "monomorphic at sites s1..sk" variant serves ANY observed
   structure assignment by binding k slot values — exactly why IC stub bodies are
   per-shape, not per-structure. What does NOT collapse is the REPRESENTATION axis:
   int32-vs-double unboxing, array element kinds, cell-vs-other — these change
   register allocation and instruction selection, i.e. are compiled into the body.
   k independent representation axes ⇒ up to 2^k distinct bodies. Unpruned lattices
   are therefore intractable for nontrivial functions (FTL-hot functions routinely
   carry dozens of speculation sites). Pruning: static facts pin most axes (Hopc
   evidence: most property access in real apps is statically provable — survey §4);
   the residual is chain-pruned to V=1 (fact-covered), V=2 (generic + one all-mono
   symbolic variant), V=3 (rare int/double arithmetic split, tool flag).
6. **Artifact projections** (typescript-scale, compile-everything per AOT-D14;
   DFG-mean density):
   - ARCH-S: ~1,800 × 2.9 KB ≈ 5.2 MB code + comparable EXITMETA ⇒ the §4.4
     baseline (1×). (Hot-set-only measured floor: 3.4 MB code + 0.5 MB ramps.)
   - ARCH-L unpruned at the V=3 FLOOR (real lattices deeper): ≥3× code ≈ 16 MB,
     plus per-variant EXITMETA on a base where ramps already measure 61% of text ⇒
     25-30 MB-class artifact. In LINKED mode (§3.5, the iOS flagship) that is app
     `.text`/`.rodata` — rejected on bytes alone, before any startup argument.
   - Hybrid (V=2 confined to the statically-silent hot-candidate subset, estimated
     20-40% of functions): 1.15-1.4× ARCH-S; inside the §4.4 cap (2× source bytes)
     with the overlay counted in the expansion-factor measurement.
7. **Speedup expectations.** Where static facts exist, selection adds ~nothing (the
   same speculation compiles either way). Where analysis is silent, ARCH-S ships
   generic-direct; the speculative-vs-generic gap on property/arith-dense code is
   the 2-5× class (public bracket: jitless LLInt is ~2-6× off baseline+, survey §4;
   speculative upper tiers sit further above). This gap IS what profiling has
   historically bought over static guessing — §2.6 losses (1) value ranges and (2)
   polymorphic bias. The overlay recovers the monomorphic-at-runtime slice of that
   gap for 20-40% of functions at +0.15-0.4× artifact bytes. Whole-app expectation:
   hybrid ≈ ARCH-S on fully-provable bundles; up to tens of percent better on
   framework-heavy bundles where analysis is silent but runtime behavior is
   monomorphic. Phase-changing/megamorphic code is identical in all three
   (descends/demotes; §5.3).

### F.3 Decision and earned hybrid

Bytes and speedup are the only discriminators (F.2.3/F.2.4 neutralize the rest).
Pure ARCH-L loses on bytes (F.2.6) and forfeits the bundle-closure wins — inlining
and direct-call formation are compile-time decisions; no selector can make them at
runtime. Pure ARCH-S forfeits a free signal (F.1) against the documented history
that profiling beats static guessing exactly where ARCH-S is weakest. The hybrid
takes ARCH-S as spine (facts primary, closure wins kept) and ARCH-L's mechanism at
bounded scope (V≤2 on the silent-hot subset, symbolic slots = the same class-M
table slots written later). Both pure forms receive epitaphs in §0.5; relitigation
requires a corrections-log entry.

### F.4 Selector protocol (normative detail behind §1.2)

(1) Tool marks lattice functions in STATICMETA with: variant list, per-variant
precondition row (which profile slots must read monomorphic / which arith sites
must be int-only etc.), symbolic-slot layout. (2) Bind installs the generic body
and leaves variant slots UNBOUND (poisoned sentinel, same family as AOT-I11
poison). (3) `checkSwitchToJIT` slow path (today's decline under `useJIT=0`)
becomes the selector entry: under the same world-state guarantee as binding (§3.3 /
§6.3), it reads ValueProfile buckets, ArrayProfile masks, GetByIdModeMetadata
structureIDs, DataOnlyCallLinkInfo callees for the sites named in the precondition
row; any mismatch/polymorphism ⇒ sticky decline with decay (counter re-arms at a
larger threshold). (4) On match: write each slot (store-release), then publish the
variant entry pointer. Guards still check every slot (AOT-I13: binding is a hint,
the guard is the truth) — a racing stale read is caught by the guard and exits,
never UB. (5) Exit from a selected variant: deselect (entry ⇒ generic body), poison
slots; ONE re-selection against a different stable shape permitted; thereafter
generic-body §1.4 rules. Validity-bit clears (§3.4) dominate selection state:
invalid ⇒ both bodies dead. (6) GC: symbolic slots holding cells follow the §3.1
row-2 weak/strong rules; the weak-table sweep covers selector-written entries
identically. (7) Threads: publication discipline identical to validity-bit
discipline (§6.2/§6.5); slots write-once between deselections.

### F.5 Measurement obligations added by AOT-D0

Charter 9 (selector matrix) is the functional gate. Size gates: the §4.4
expansion-factor measurement reports spine and overlay bytes separately; the
overlay's projected 1.15-1.4× is an allocation in the Annex-C sense — exceeding it
demotes overlay variants first (worst code-per-bytecode-byte order) and requires a
corrections-log entry, not a quiet re-budget.

## Annex G — Round-4 adversarial review record (2026-06-10, BINDING)

External review delivered 10 findings (the 10th truncated in transit). Each
tree-fact claim was independently re-verified against this tree before
acceptance. Disposition: **10/10 accepted, 0 refuted.** Corrections C15-C24;
decisions AOT-D18/D19; AOT-I5/I7/I13 amended. Annex F is not silently edited:
its amendments (F.2.5 layout-equivalence, new F.2.8 warm-up row, F.4 protocol
changes) are recorded in G.6 below per frozen-spec conventions.

| # | Finding (short) | Verified tree facts | Resolution |
|---|---|---|---|
| H1 | Symbolic-variant structureID guard does not validate LAYOUT — silent type confusion (blocker) | `InlineCacheHandler` layout-as-data offsets (`bytecode/InlineCacheHandler.h:130-143`); `compileGetByOffset` fixed displacement (`dfg/DFGSpeculativeJIT.cpp:15696-15724`); compile-time inline-ness branch (`ftl/FTLLowerDFGToB3.cpp:13590,13708`); `GetByIdModeMetadataDefault {structureID, cachedOffset}` (`bytecode/GetByIdMetadata.h:98-125`) | C15: layout signatures in STATICMETA; selector validates layout before bind; F.2.5 amended (G.6); AOT-I13 extended; charter 9(iv) |
| H2 | Deselection poisons/rebinds slots under in-flight activations; value-position consumers read poison (blocker) | Allocation-site store-position structure (`dfg/DFGSpeculativeJIT.cpp:440-450` `TrustedImmPtr(structure)` stored into the new cell); `offsetOfNewStructureID` transition-put analog (`InlineCacheHandler.h:136`); §3.1 row-2 guard-position precedent in the doc itself | C16: selector-written slots GUARD-POSITION-ONLY; value/store-position facts via load-time recipe slots or compiled in; AOT-I5-choke-point enforced; recovery streams never source rebindable slots; charter 9(v) |
| H3 | Jitless product never allocates the SymbolTable watchpoint sets guarding §2.1.3 constants (blocker) | `isWatchable()` requires `Options::useJIT()` (`runtime/SymbolTable.h:238`); "watchpointSet() returns nullptr if JIT is disabled" (`:320-321`); fold site `dfg/DFGByteCodeParser.cpp:5588-5593` | C17: AOT-flavor forces the sets ON (ledger audit item); PRECOND scope-variable entry class; eval-poisoning rule; fail-CLOSED on missing sets; charter 3 reassigned-export row |
| H4 | Recipe/global-path resolution can execute user JS (Proxy traps/getters) at bind | AOT-I6 asserted, no read mechanism specified; §3.2/§3.3 resolve against the live post-startup heap | C18: normative path walk (own data property, non-exotic ordinary object, getDirect-style inspection; exotica ⇒ fail-closed); charter 3 path-step-exotica row |
| H5 | Realms never addressed: image-wide memoization + single globalObject slot break per-realm identity | zero realm mentions (grep); per-global watchpoints (`runtime/JSGlobalObject.h:550,557,1202`); engine-level cross-realm fix precedent 51cc3feb7298 | C19: binding state keyed per (function × JSGlobalObject); shared CODE/EXITMETA/RECIPES; per-realm recipe memoization; ShadowRealm/vm sources = Tier L; charter 3 two-realm row |
| H6 | Selector can never fire: bind installs generic body but profiles/counter are LLInt-only (blocker) | `checkSwitchToJIT` macro ticks `m_llintExecuteCounter` only in LLInt (`llint/LowLevelInterpreter.asm:1830-1836`, uses at `:2946,:3032,:3063`); LLInt-only profile stores (`llint/LowLevelInterpreter64.asm:77-94`, `llint/LLIntSlowPaths.cpp:1100-1117`) | C20: deferred entry install — lattice functions stay LLInt-resident until trip; selector publishes variant-or-generic; warm-up window costed (G.6/F.2.8); epitaph restated |
| H7 | FTL operation calls are anonymous B3 constants; class-R "captured natively" false for the default tier | `FTLOutput::operation` = `constIntPtr(tagCFunctionPtr<OperationPtrTag>(...))` (`ftl/FTLOutput.h:416-417`); C3 precedent for weakPointer | C21: FTLOutput::operation choke point ⇒ image GOT; AOT-I5 extended to OperationPtrTag/JSEntryPtrTag; §2.3/§3.1 row 4 restated; ledger row |
| H8 | FTL has its own exit compiler/representation; AOT-D2/EXITMETA/audit were DFG-only | `compileStub` (`ftl/FTLOSRExitCompiler.cpp:222,936,940`); ExitValue kinds (`ftl/FTLExitValue.h:53-59`); ExitTimeObjectMaterialization | C22: FTL branch — ExitValue streams in EXITMETA, compileStub per-immediate audit (G.8), dedup over parameterized stream, recovery-interpreter materialization coverage, ledger row, AOT-I3/charter 5 updated |
| H9 | Tool-time callee/executable cell materialization is an unstated NEW mechanism | `newCodeBlockFor(kind, JSFunction*, JSScope*)` (`runtime/ScriptExecutable.h:115`); freeze/CallVariant need live cells; tool never evaluates modules (C2b) | C23/AOT-D18: stand-in factory (G.9); NEW ledger row; AOT-I9 determinism via §2.1 identities |
| H10 | BUNDLE mode: no entitlement/integrity story; §5.2 thesis LINKED-only (received truncated; core verified from main-doc §3.5/§5.2 text + platform PROT_EXEC rules) | §3.5 mapped PROT_EXEC from a data file; HEADER = compatibility check only | C24/AOT-D19: integrity verification before mapping; platform scoping (iOS = LINKED); AOT-I7 caveat; §5.2 scoped (G.10) |

### G.2 — C16 slot-consumption rule (full text)

Classification happens at tool time, at the AOT-I5 choke points (the only places
a Structure/cell constant can enter lowering): each routed constant is tagged
GUARD (compared, never stored/propagated into the heap or into recovery) or
VALUE (stored into object headers, newly allocated cells, returned, or
referenced by a ValueRecovery/ExitValue stream). A symbolic (selector-written,
rebindable, poisonable) slot may carry GUARD-tagged consumers only. A variant
whose silent axis requires a VALUE-position constant either (a) takes that
constant from a load-time recipe slot — bound once at §3.3 bind, never rebound,
never poisoned by deselection (weak-death still clears the validity bit, which
dominates) — or (b) is not generated for that axis (pruned; the generic body
serves). Transition-put variants therefore cannot symbolically bind their NEW
structure: it must be recipe-derived (the transition target is computable at
tool time from the bound old structure's recipe + the transition step) or the
variant is ineligible. Exit-recovery constants always reference the constant
pool or load-time slots, never rebindable ones (re-asserted from AOT-D2's
"recovery cell constants → constant pool (M)").

### G.3 — C17 watchpoint-gating audit (full text)

Ledger audit item (binding, part of the ENABLE(AOT_RUNTIME) work): grep-grade
enumeration of every site where watchpoint-set creation or maintenance is gated
on `Options::useJIT()`, `Options::useDFGJIT()`, or `ENABLE(JIT)`/
`ENABLE(DFG_JIT)`; for each, decide KEEP-ON (an AOT precondition class depends
on it — the proven instance is `SymbolTable::isWatchable`,
`runtime/SymbolTable.h:238`) or LEAVE-OFF (pure JIT bookkeeping) and record the
decision in the audit table before implementation freeze. Memory note: forcing
scope-variable sets ON re-adds FatEntry allocations the jitless build avoids —
counted against the §4.3 private-dirty budget. The §2.1.3 emission rule: a
constant/callee fact compiles as an unguarded fold ONLY when a PRECOND
scope-variable entry exists for the binding (set exists, is watchable, and is
still-valid at bind, per §3.3.3 fail-closed); otherwise the tool emits the
guarded form (CheckCell/CheckIsConstant against the table-loaded value) or drops
the fact. Direct eval anywhere in a module's scope chain marks all bindings of
that module as non-once-assigned regardless of syntactic assignment counts.

### G.5 — C19 realm keying (full rule)

Shared, realm-agnostic, immutable: CODE, EXITMETA, RELOC, RECIPES (the recipe
DESCRIPTIONS — their resolution products are per-realm), BYTECODE/SOURCE,
STATICMETA. Per (artifact function × JSGlobalObject): data tables (incl. the
row-3 globalObject slot and all recipe-product StructureIDs), recipe-resolution
memoization, precondition validation + watchpoint subscriptions (universal
assumptions like havingABadTime/species/saneChain are per-global sets — each
realm validates and subscribes its own), validity bits, exit/demotion counters,
selection state and symbolic-slot bindings (a shape hot in realm 1 says nothing
about realm 2). Lifecycle: binding state is owned by the per-realm linked
CodeBlock (the BaselineJITData discipline — `jit/BaselineJITCode.h:124-137`,
per-CodeBlock so shared code is realm-correct) and dies with it. A second
instantiation of shipped modules under another global binds independently
through the identical §3.3 sequence; ShadowRealm.evaluate / `node:vm` context
SOURCE STRINGS are dynamic code on Tier L (§1.5). Cross-realm calls into a bound
function execute the callee realm's binding; no binding state is ever read
across realms. Charter 3's two-realm differential drives both directions
(realm-2 first-bind, and realm-1 invalidation while realm-2 stays valid).

### G.6 — C20 deferred-install selector protocol + Annex F amendments

Amended F.4 protocol: (2') bind validates preconditions and prepares BOTH bodies
but does NOT install an entrypoint for lattice functions — the CodeBlock keeps
its LLInt entry; spine-only functions install at bind as before. (3') the
`checkSwitchToJIT` slow path IS the selection point (it only fires while
LLInt-resident, which the deferred install guarantees); on decline it installs
the generic body — the function leaves LLInt either way at the trip. Functions
that never reach threshold remain LLInt permanently: bounded cost (they were
never hot), and no timer fallback in v1 (a timer would add a clock dependency
for code that by definition does not matter). After deselection (F.4(5)) the
entry returns to the GENERIC body (not LLInt — its facts were already validated
at bind); the single re-selection permission re-arms the LLInt counter only if
the generic body is uninstalled, which v1 does not do — re-selection therefore
reads the LLInt profiles as they stood at first trip plus any LLInt execution
since; acceptable because re-selection requires a NEW stable shape, observed via
the exit site's own metadata. New F.2.8 cost row: overlay warm-up = threshold T
× per-call LLInt-vs-generic gap, paid once per (function × realm × process);
with T in the low thousands (today's `thresholdForJITAfterWarmUp` family) and
the 2-6× LLInt factor, this is milliseconds-class per function and amortizes
iff the function is genuinely hot — exactly the population the lattice pruner
selects for. F.2.5 amendment (C15): "one mono-shape body serves ANY observed
structure assignment" is corrected to "any LAYOUT-COMPATIBLE structure
assignment"; the selector's decline rate on layout mismatch is a new measured
input for the §4.4 expansion-factor protocol (high decline ⇒ variants are dead
bytes ⇒ prune harder or emit per-layout-class variants under the same V cap).

### G.8 — C22 FTL exit-compiler per-immediate audit (binding skeleton)

`ftl/FTLOSRExitCompiler.cpp` `compileStub` baked immediates and their product
classification (Annex D F3 style): (a) `&exit`/exit-descriptor pointer and
`jitCode->osrExit` indexing ⇒ parameter-block fields (exit-descriptor index into
EXITMETA); (b) VM scratch/materialization buffer pointers
(`vm.osrExitJumpDestination`, scratch buffers around materialization) ⇒
per-function data-table slots bound at §3.3; (c) `ExitValueConstant` payloads ⇒
constant pool (M), per C16 never rebindable slots; (d)
ExitTimeObjectMaterialization graphs ⇒ serialized in EXITMETA; product replays
them via the C++ recovery interpreter calling `operationMaterializeObjectInOSR`-
class operations (class-R); (e) `Options::poisonDeadOSRExitVariables` and
similar option branches ⇒ flavor-pinned (AOT-I8); (f) codeBlock/baseline
pointers in the shared tail ⇒ same treatment as Annex D F3 (b,e,g,h) — the tail
(`adjustAndJumpToTarget`) is shared and already audited. Completion of the
per-line audit is an implementation-freeze gate, same rule as Annex E.2 thunks:
an unaudited FTL ramp in CODE is an AOT-I5-class violation.

### G.9 — AOT-D18 stand-in factory (full record)

Inputs: the bundle's UnlinkedFunctionExecutables (already produced by the
CachedBytecode pipeline) + the §2.1 module graph. The factory creates, per
module, a synthetic JSModuleEnvironment-shaped scope chain (structure-only; no
user evaluation) and FunctionExecutables bound to it, sufficient for
`newCodeBlockFor`-class parsing and DFG inlining of any bundle function. Status
synthesis (AOT-D4.2) builds `CallVariant`s from these stand-in executables —
variant-by-executable, never by closure, which matches AOT-D13's product-side
weakening (closure-identity inlining is already declined/weakened, so the tool
never needs a real closure). `Graph::freeze` on a stand-in records a
RELOC/RECIPES reference by ExecutableID (= §2.1 function identity) or path;
AOT-I5's choke points reject any stand-in whose identity is not so expressible
(cannot happen by construction for bundle functions; guards against future
leakage). Determinism (AOT-I9): stand-in creation order is the module-graph
topological order; no tool-heap address ever reaches the artifact.

### G.10 — AOT-D19 BUNDLE integrity (full record)

Threat model: the artifact path is writable by something other than the app
installer (user-writable app dirs, compromised updater). HEADER validation
(`isUpToDate`-style) is a compatibility check with zero authentication. Rule:
the loader maps CODE executable only after integrity verification — preferred:
the artifact file is covered by platform code-signing (macOS: signed as an app
resource with library validation; Linux: dm-verity/IMA where deployed);
fallback: the tool embeds a signature over HEADER+CODE+EXITMETA+RELOC keyed to
the app (public key compiled into the product binary), verified at load.
Verification failure ⇒ artifact ignored wholesale, Tier L, reason logged
(fail-safe, same path as HEADER mismatch). Platform scoping recorded: iOS
forbids PROT_EXEC data mappings entirely ⇒ LINKED only; hardened-runtime macOS
BUNDLE requires the unsigned-executable-memory entitlement class — permitted
but it forfeits the §5.2 no-entitlement thesis, which is therefore stated as
LINKED's property. AOT-I7 stands as observational equivalence above the loader;
this asymmetry is below it.

### G.full — full text of round-4 main-doc compressions

Passages compressed in draft 7 whose full pre-compression text is not already in
Annexes A/D/E/F are the round-4 additions themselves; their normative full forms
are G.2/G.3/G.6/G.8/G.9/G.10 above and the C15-C24 corrections-log entries.
Wording-only trims elsewhere (§1.1, §1.4, §2.2, §3.1 rows, §4, §5, §6, charter)
dropped no normative content: every dropped cite/clause appears verbatim in the
corresponding Annex A/D/E/F entry or corrections-log record of the round that
introduced it.


## Annex H — Round-5 adversarial review record (2026-06-10, BINDING)

External review delivered 10 findings (the 10th truncated in transit; its
verifiable core was confirmed from the tree). Each tree-fact claim was
independently re-verified against this tree before acceptance — all checked
cites held (notably: `StructureStubInfo` exists only in
`runtime/RaceAmplifier.h:77`; no `useDataIC` option, only `useHandlerICInFTL`
at `runtime/OptionsList.h:643`; `TrustedImmPtr(mathIC)` at
`dfg/DFGSpeculativeJIT.cpp:5068`; jump tables at `dfg/DFGJITCode.h:312-313`;
masquerades set at `dfg/DFGGraph.h:946-949`; `emitFunctionChecks` at
`dfg/DFGByteCodeParser.cpp:1571-1594`; `Structure::create` inline-capacity
parameter at `runtime/Structure.h:236`). Disposition: **10/10 accepted, 0
refuted.** Corrections C25-C34; AOT-I5/I9 amended; AOT-D19 amended (C33);
survey corrected (row 7) and extended (rows 10-11).

| # | Finding (short) | Resolution |
|---|---|---|
| I1 | Exit parameter blocks never re-keyed for realms: C19 vs AOT-D2 unreconciled — wrong-realm LLInt resume (BLOCKER) | C25; §1.3 re-specified (realm-invariant block; realm state via exiting frame's per-realm AOT CodeBlock); E.3 + G.5 amended (H.1); charter 3 row |
| I2 | D13 ExecutableID guard realm-agnostic: same-source other-realm closure runs inline with wrong-realm semantics | C26; §3.1 row 2 realm rule (per-realm cell equality; decline if unresolvable); charter 3 row |
| I3 | Recipe schema omits inline capacity/TypeInfo/ClassInfo; load-time recipe slots get no layout verification | C27; §3.2 schema extended; §3.3 step-1 layout-signature check; STATICMETA per-recipe signatures; charter 3 row |
| I4 | PRECOND catalog open-ended; masquerades (and threads-set) check-elision speculation could ship unguarded+unsubscribed | C28; well-known per-global-set descriptor class; closed serialize-or-decline rule; charter 3 row |
| I5 | MathIC baked heap pointers + runtime regeneration, absent from audits/ledger | C29; MathIC disabled tool-side (non-repatching ops); `*Optimize` ops dropped; AOT-I5 extended; survey row 11 |
| I6 | IC story named a nonexistent type; data-IC form is FTL-only here — DFG-grade bodies had no emission path | C30; row 7/AOT-D9 re-grounded in PropertyInlineCache/InlineCacheHandler; DFG-grade bodies emit the handler form (tool-only, MODIFIED); survey row 7 corrected |
| I7 | Switch jump tables (side-table absolute code addresses) missing from catalog + artifact format | C31; §3.1 row 11; AOTObjectLinkBuffer captures table writes; survey row 10 |
| I8 | Artifact cap (2× source) contradicts own 5-10× projections by ~5×; F.2.6 "inside the cap" false (BLOCKER) | C32; cap restated (2× ARCH-S baseline ≈ 10-20× source); §5.4 honest per-app figure; F.2.6 amended (H.8) |
| I9 | BUNDLE integrity verification absent from startup budget; O(artifact) eager hash dwarfs 10 ms | C33; page-granular hash tree in HEADER, verify-on-first-map; CI MB/ms gate |
| I10 | AOT-I9 detected but not produced; pointer-hashed containers feed serialization (truncated; core verified) | C34; production rule in AOT-I9 (identity-ordered serialization, deterministic scheduling) |

### H.1 — C25 amendments to Annex E.3 and G.5 (frozen-spec: recorded, not edited)

**E.3(3) amendment.** "fills the parameter-block slots with a store-release
(idempotent: racing exits compute identical values)" is corrected to: fills the
**per-realm data-table slot group** owned by the exiting frame's AOT CodeBlock
with a store-release. Idempotency holds PER REALM SLOT only: racing exits *from
the same realm* compute identical values; exits from different realms write
different (per-realm) slots and never race each other. Inlinee CodeBlock
pointers are per-realm cells; a shared bind-written block would make the
original claim false. The ramp's UNMATERIALIZED-tag test (E.3(2)) reads the
per-realm slot group, reached via the frame's codeBlock — not a link-time
address.

**G.5 amendment.** The shared/realm-agnostic list gains: **exit parameter
blocks** — legal precisely because C25 strips them to realm-invariant content
(exit-descriptor index, inline-frame tree refs, ramp-shape constants). Anything
realm-dependent formerly listed as block content (resume slot, data-table base,
counter slot) lives in the per-realm data table, already on G.5's per-realm
side. Rationale for the access path: the exiting frame's codeBlock slot
identifies the realm's AOT CodeBlock (the AOT-D10 thin CodeBlock), which owns
the realm's table — the same way `compileExit` locates per-CodeBlock state
today; no realm-dependent link-time constant exists anywhere on the exit path.

### H.2 — C26 realm-pinned weakened guard (full text)

Bind-time: resolve ExecutableID → this realm's `FunctionExecutable` via the
realm's module-instantiation records (the §3.3 sequence already walks them for
recipes/paths); write the cell into a per-realm table slot; the emitted guard
compares the callee's executable (GetExecutable) against the table-loaded cell.
This restores exactly the realm-soundness property of today's frozen-cell check
(`emitFunctionChecks`: FunctionExecutables are per-instantiation, hence
per-realm) without requiring an unpathable closure cell. "Structure check"
language is dropped from the guard description: a type/shape check cannot
distinguish realms. Where a realm cannot resolve the ID to a unique live
executable at bind (e.g. the defining module not yet instantiated in that
realm), the inlining is declined for that realm (generic call) — never a
weaker compare. G.5's "cross-realm calls execute the callee realm's binding"
is unaffected: it governs non-inlined calls; inlining bypasses the callee's
binding, which is exactly why the guard must pin the realm.

### H.3 — C30 IC re-grounding (full text)

In-tree reality: `PropertyInlineCache` (abstract) with two concrete forms —
`HandlerPropertyInlineCache` (data-dispatch: fast path loads the handler from
the cache and calls through `InlineCacheHandler::offsetOfCallTarget`,
`jit/JITInlineCacheGenerator.cpp:102-107`; used by FTL sites under
`useHandlerICInFTL`, comment at `:124`) and `RepatchingPropertyInlineCache`
(`:94-97`, runtime code repatching — unusable under AOT-I2). Handler chains are
published by data write (`publishHandlerChainHead`,
`bytecode/PropertyInlineCache.cpp:1149`); precompiled shared handler bodies
exist (`createPreCompiled`, cited in F.1). AOT mapping: ALL Tier-A property-IC
sites — FTL-grade and DFG-grade — emit the handler-dispatch form; the tool's
DFG lowering is MODIFIED (tool-only) to instantiate handler ICs where it would
create repatching ICs; per-function IC tables live in the per-realm writable
data table (C19); v1 ships the self-access handler subset (AOT-D9 unchanged in
scope), each shipped handler body passing the E.2 per-immediate audit.
Anything beyond self-access takes the operation slow path (§5.1.3). The
survey's row-7 `useDataIC` claim is corrected in the survey itself.

### H.4 — C31 jump-table mechanism (full text)

Serialized form: per function, per table, an array of (case value →
function-relative code offset) plus a default offset; string tables reference
atom-table indices (the `UnlinkedStringJumpTable` offset-table shape is the
natural source — the unlinked tables already ship in BYTECODE; only the
CTI-address vectors are JIT-side). Bind (BUNDLE): loader materializes absolute
entries = mapped CODE base + function offset + entry offset into the
per-function data table; the dispatch sequence indexes that table (today's
emission already loads the table base from heap state, so no code-shape change).
LINKED: the emitter places materialized tables in `.data.rel.ro` with native
absolute relocations against the function's `.text` symbol. AOTObjectLinkBuffer
rule (amends §2.3's spec): LinkBuffer's jump-table finalization writes
(populating `CodeLocationLabel` vectors) are intercepted and redirected to the
serialized form; a finalize-time absolute code address reaching any data
structure other than this serializer is an AOT-I5-class violation (closing the
"catalog complete" premise for table-side addresses generally, not just
switches).

### H.5 — C29 MathIC record (full text)

The MathIC family (`JITAddIC`/`JITSubIC`/`JITMulIC`/`JITNegIC`,
`jitCode()->common.addJIT*IC`, `dfg/DFGSpeculativeJIT.cpp:4891`) bakes the IC
object's address (`TrustedImmPtr(mathIC)`, `:5068,5742`) and reaches
`operationValue*Optimize`-class operations whose runtime behavior is
`JITMathIC::generateOutOfLine` (`jit/JITMathIC.h:127`) — LinkBuffer codegen over
the live inline region. FTL identical
(`ftl/FTLLowerDFGToB3.cpp:2926-3116`). Decision (AOT-D9-style honest
downgrade): tool lowering takes the existing non-repatching branch
unconditionally (`nonRepatchingFunction`, `dfg/DFGSpeculativeJIT.cpp:5097`); no
MathIC object exists in the artifact or product; `*Optimize` operations leave
the kept set (they are tier-up/regeneration ops, same class as the dropped
`setStub` path in E.2). Cost: untyped-arith sites (already the AOT-D16 floor
population) pay an operation call where the JIT inlines a fast path — recorded
under §5.1(2). A future v2 may specify a data-table arithmetic profile + shipped
non-regenerating fast path; chartered in Annex B (B.9).

### H.6 — C28 closed PRECOND rule (full text)

Tool-time rule (AOT-I-class, enforced where `DesiredWatchpoints` is serialized):
the serializer walks every entry the plan registered through any `addLazily`
overload (`dfg/DFGDesiredWatchpoints.h:248-259`) plus `DesiredGlobalProperties`;
each must map to one of the catalog's descriptor classes
(ObjectPropertyCondition / global-property / view-detach / scope-variable /
well-known per-global set). The well-known-set class is a closed enumeration:
masqueradesAsUndefined, havingABadTime, arraySpecies, saneChain,
arrayIteratorProtocol, numberToString, plus this fork's structure
transitionThreadLocal/writeThreadLocal (E1/E2) sets; each binds per realm
(C19) and subscribes like the §2.1.4 assumptions. An entry that maps to no
class is a TOOL-TIME hard stop for that function: re-emit the speculation in
guarded form if the parser/lowering supports it, else decline the function —
dropping the entry is the one forbidden outcome, because watchpoint-carried
speculation has NO inline check to fall back on (check-elision is the point of
the mechanism). New raw-set registrations added to the engine later fail loudly
(unknown class ⇒ decline), never silently.

### H.7 — C27 spine layout-signature check (full text)

STATICMETA records, per load-time recipe slot, the same signature C15 defined
for selector slots: per-access-site offset + inline/out-of-line placement,
indexing type, and for transition sites the (old, new) pair's offsets +
butterfly size delta — as the TOOL VM resolved them when the body was compiled.
§3.3 step 1, after resolving a recipe (creating-or-finding), recomputes the
signature from the LIVE structure and byte-compares. Pass ⇒ slot written;
fail ⇒ the owning function fails closed (reason recorded), exactly the §3.3
discipline. This converts any divergence in loader-side structure construction
(capacity inference drift, ClassInfo mismatch, future schema gaps) from silent
type confusion into a bind refusal. The schema extension (capacity,
TypeInfo/ClassInfo) makes refusals rare; the signature check makes them safe.

### H.8 — C32 amendment to Annex F.2.6 (frozen-spec: recorded, not edited)

F.2.6's hybrid bullet ends "inside the §4.4 cap (2× source bytes) with the
overlay counted in the expansion-factor measurement." That sentence is FALSE as
written (≈1.15-1.4 × the ARCH-S baseline ≈ 10-13× source bytes, ~5× over a
2×-source cap) and is amended to: "inside the §4.4 cap (2× the projected ARCH-S
baseline) with the overlay counted in the expansion-factor measurement." The
cost model's measured inputs and the decision are unaffected — the error was in
the cap's denominator, not the projections (re-verified this round: text sum
23.72 MB, DFG 3.37 MB, 736 `operation*` symbols / 0.362 MB, icudt75 30.73 MB).
F.5's allocation discipline now reads against the corrected cap; the hot-set
floor datum (3.4 MB code + 0.5 MB ramps) stands as the supporting measurement
for any future opt-in hot-set-only policy (which would be a new decision, not
AOT-D14).

### H.9 — round-5 main-doc compressions (full-text record)

To fit C25-C34 under the cap, draft 8 compressed: §0.5 (ARCH-L/decision/epitaph
wording; table cells), §1.2 (AOT-D10 cite list; overlay-selection paragraph —
normative full text remains G.6), §1.3 (intro cites; FTL-branch sentence — full
text G.8), §1.4 (intro + step 0 wording — full rule G.2), AOT-D15 (full record
C8), §2.1-§2.2 (cite trims; C17 sentence — full audit G.3), §2.3 (AOT-D18 —
full record G.9; ledger wording), §3.1 rows 1/3/10 (full texts G.2/E.2/D), §3.2
obligations (full mechanization C18/G), §3.3 step 2b (full protocol E.3 + H.1),
§3.4 weak-death (full record AOT-I11/D), §3.5 BUNDLE bullet (full record
G.10 + C33), §4.2 verdict, §5.1-§5.2 wording, §6 (full text E.4), charter items
2/3/9 (full matrices G + this annex). Every compressed clause's normative
content lives in the cited annex or corrections entry; no invariant, decision,
or file:line grounding was dropped — cite-list trims kept at least one anchor
line per claim.

## Annex I — Round-6 adversarial review record (2026-06-10, BINDING)

External review delivered 6 findings (2 blockers, 4 major). Every tree-fact
claim was independently re-verified against this tree before acceptance —
all checked cites held (37 `lazySlowPath(` occurrences in
`ftl/FTLLowerDFGToB3.cpp`, ~28 real emission sites; `LazySlowPath::generate`
LinkBuffer/FINALIZE_CODE at `ftl/FTLLazySlowPath.cpp:57-72`;
`operationCompileFTLLazySlowPath` at `ftl/FTLOperations.cpp:958`;
`AbsoluteAddress(vm.heap.addressOfBarrierThreshold())` at
`jit/AssemblyHelpers.h:2088,2095`, fencing `:2159`, soft-stack-limit fallback
arm in the `:195-205` region; trap bits at `dfg/DFGSpeculativeJIT.cpp:2581`;
exit-site stubs at `dfg/DFGJITCompiler.cpp:104-126`; `repatchJump` +
per-lite `osrExitJumpDestination` at `dfg/DFGOSRExit.cpp:284-295`;
`s_JSCCombinedCode` per survey §3.3). Disposition: **6 findings, 5 distinct
root causes accepted, 0 refuted** — findings 2 and 4 (both: `AbsoluteAddress`
escapes AOT-I5) are the SAME class at the same constructor and are merged
into C36; the merge is recorded, not a refutation. One per-cite nuance
recorded in C36: the `didEnterVM` sites are
`Options::validateDFGClobberize()`-gated validation code (disabled tool-side,
not table-routed) — the finding's class is unaffected.

| # | Finding (short) | Resolution |
|---|---|---|
| J1 | FTL LazySlowPath = runtime codegen in every FTL-grade body; absent from design (BLOCKER, AOT-I2) | C35; eager tool-time generation into CODE; op+thunk dropped from kept set; nm audit + gate row; survey row 13; I.1 |
| J2+J4 | `AbsoluteAddress` bakes per-VM mutable-state tool addresses in main bodies; outside every AOT-I5 choke point; silent-corruption class (BLOCKER) | C36; §3.1 row 12 (offset loads off row-3 VM pointer); AOT-I5 extended to the constructor; survey row 12; I.2 |
| J3 | Guard-site → ramp dispatch unspecified (generation thunk + repatchJump + per-VM osrExitIndex) | C37; AOT-D2 exit-site ABI; ledger row; I.3 |
| J5 | Shipped JS builtins permanently LLInt; §5.1/§5.4 honesty broken | C38; AOT-D20 (hot-subset builtin artifact + recorded loss 6); §4.1 line; I.4 |
| J6 | C33 lazy hash verify TOCTOU-unsound on file-backed exec mappings (app-key fallback) | C39; bind-time verify; anonymous-copy fallback; §4.3 claims re-scoped; I.5 |

### I.1 — C35 LazySlowPath eager-generation rule (full text)

Binding rule: in `--aot-compile` mode, FTL lowering does not emit the
`lazySlowPathGenerationThunkGenerator` jump (`ftl/FTLLowerDFGToB3.cpp:25352`).
For each `lazySlowPath()` patchpoint the tool invokes the site's generator
(`LazySlowPath::Generator`) at compile time — sound because the generator
lambda and its register/usedRegisters/callSiteIndex snapshot are fully
determined at compile time (`:25361-25375`); runtime contributes nothing but
the trigger — and emits the resulting stub into CODE adjacent to the body,
with the patchable jump resolved at tool time to the stub and the stub's
done/exception edges linked to the body's labels. Every immediate in each
generated stub passes the E.2/G.8-style per-immediate audit (the stubs call
`operation*` ⇒ class R via the C21 GOT route; cell/structure constants ⇒
AOT-I5 choke points apply — the generators run under the same MODIFIED
tool-mode assembler). Alternative (b) — lowering these sites to ordinary
eager slow-path calls — is RESERVED as the per-site fallback where a
generator resists offline emission; either way no lazy path survives.
Dropped from the kept set: `operationCompileFTLLazySlowPath`,
`lazySlowPathGenerationThunkGenerator`, `FTL::LazySlowPath::generate`.
AOT-I2 CI nm audit symbol list extends accordingly (charter 6 scope).
Implementation-freeze gate row (mirrors G.8): enumerate all `lazySlowPath()`
sites at freeze; an unaudited generated stub in CODE is an AOT-I5-class
violation per E.2's closed rule.

### I.2 — C36 main-path per-immediate audit: `AbsoluteAddress` bakes (BINDING)

Enumerated per-VM/per-heap raw-address instruction operands in main-body
DFG/AssemblyHelpers emission, with dispositions (class M' = offset load off
the row-3 per-VM table slot; GC-phase-mutable values are NEVER snapshotted
as table values):

| Bake | Site | Disposition |
|---|---|---|
| `vm.heap.addressOfBarrierThreshold()` | `jit/AssemblyHelpers.h:2088,2095` (`barrierBranch`, every barriered store) | M' — load VM ptr from table, `branch32` against `Heap::offsetOfBarrierThreshold` analog; mutable during concurrent GC |
| `vm.heap.addressOfMutatorShouldBeFenced()` | `jit/AssemblyHelpers.h:2159` | M' — same discipline |
| `vm.addressOfSoftStackLimit()` | `jit/AssemblyHelpers.h` `:195-205` region, non-VMLite arm (every prologue stack check) | M' — this fork's VMLite arm already does the offset-load shape (`loadVMLite` + `offsetOfSoftStackLimit`); AOT uses that shape unconditionally |
| `vm().traps().trapBitsAddress()` | `dfg/DFGSpeculativeJIT.cpp:2581` (loop back-edge trap poll; the product's only Tier-A termination/safepoint hook under polling traps) | M' — offset load; polling traps are the AOT product's trap mechanism (no signal-patching) |
| `&vm().didEnterVM` | `dfg/DFGSpeculativeJIT.cpp:2348,2388` | validateDFGClobberize-only: DISABLED tool-side |
| CountExecution / execution counters | `dfg/DFGSpeculativeJIT64.cpp` (countExecution) | profiling/diagnostic: DISABLED tool-side |

Enforcement (AOT-I5 extension, normative): in tool-mode lowering, construction
of `AbsoluteAddress` (or a `TrustedImm64`/`TrustedImmPtr` from a raw
non-cell address) over any address not on a link-time-symbol whitelist is a
HARD ERROR — symmetric to H.4's finalize-time rule: a tool-VM absolute
address reaching any instruction operand is an AOT-I5-class violation. The
whitelist is the class-R symbol set only. Any future engine merge adding an
`AbsoluteAddress` fast-path check now fails at tool time instead of shipping
a silent bake. Survey §2 row 12 records the class.

### I.3 — C37 AOT exit-site ABI (full text)

Per guard site: (1) the failure branch is emitted as a normal branch whose
target the tool resolves at AOTObjectLinkBuffer time to the function's
dedup'd ramp for that recovery stream — intra-image, no runtime patching;
(2) site identity is materialized immediately before the jump as an imm32
exit index in a designated scratch register (the in-tree JSVALUE64
convention: `move(TrustedImm32(i), GPRInfo::numberTagRegister)`,
`dfg/DFGJITCompiler.cpp:104-117`; the ramp's first act today is re-deriving
state from that index — same contract); (3) the ramp indexes the
realm-invariant parameter-block array (AOT-D2/C25) whose base is reached
RIP-relative (LINKED) or via image GOT (BUNDLE) — class R, never a baked
data address (H.4). Compiled out of the product and the tool's AOT emission:
the OSR-exit generation thunks, `MacroAssembler::repatchJump` of exit sites,
`exit.codeLocationForRepatch()`, the per-lite `osrExitJumpDestination`
publish (`dfg/DFGOSRExit.cpp:284-295`), and the 32-bit
`store32(TrustedImm32(i), &vm().osrExitIndex)` form (a per-VM baked address —
C36's class; 64-bit register convention used on all AOT targets). FTL: the
OSRExitHandle lazy-compile + repatch path is replaced identically — ramps
are precompiled from the parameterized ExitValue stream (C22), sites
statically linked. Ledger: `DFGJITCompiler::linkOSRExits` + FTL
OSRExitHandle dispatch — MODIFIED (tool-only). Audit obligation: these two
emitters join the Annex D F3 per-immediate audit scope (every immediate in
the site stub is the exit index imm32 or a link-resolved target; anything
else is a violation).

### I.4 — C38 AOT-D20 builtin corpus (full record)

Decision: the JS-implemented builtin corpus (`s_JSCCombinedCode`, 152 KB
text, survey §3.3 — kept as text and compiled at runtime in stock JSC) is a
FIXED input known at engine-build time. The AOT engine build precompiles a
designated HOT SUBSET once with the same §2 pipeline (static facts from the
builtins' own code + the universal watchpoint assumptions; builtin-internal
structures get recipes/preconditions like any other) and ships the result as
a runtime-resident artifact section keyed by the same flavor hash (AOT-I8) —
binding/validation identical to app functions (§3.3); a builtin whose
preconditions fail binds nowhere and runs LLInt, correct by AOT-I1. Subset
selection is frozen per engine build (iteration shells: forEach/map/filter/
reduce/sort comparators' shells, %TypedArray% shells; Promise/async/iterator
machinery), NOT per app — no training runs (C2b holds; the subset list is a
reviewed source artifact). Budget: +0.50 MB allocation line in §4.1,
CI-gated (AOT-I10 bucket); measured before implementation freeze — if the
hot subset cannot fit 0.50 MB at default policy, the list shrinks (recorded),
never the budget. The cold remainder stays LLInt permanently and is recorded
as §5.1 loss (6); §5.4's comparison now carries the caveat explicitly.
Rejected: full-corpus precompilation (projected 1.5-3 MB at §4.4 expansion
factors — fails C3 headroom); status quo (permanent-LLInt builtins — breaks
§5.1/§5.4 honesty and loses builtin-heavy workloads to native-builtin
engines).

### I.5 — C39 BUNDLE integrity, amended mechanism (full text)

Threat model unchanged (G.10). Soundness hole closed: user-space hashing of
a file-backed mapping does not authenticate later execution — eviction +
re-fault re-reads the (attacker-writable) file into an already-PROT_EXEC
mapping; MAP_PRIVATE only protects written pages. Amended mechanism:

1. Verification trigger: BIND time, per function-covering region (the §3.3
   lazy-binding unit) — implementable, unlike "first executable map";
   pre-bind regions are PROT_NONE.
2. Kernel-verified deployments — macOS platform code signing; Linux with
   fs-verity/dm-verity/IMA covering the artifact — map
   PROT_READ|PROT_EXEC directly: the kernel re-verifies on page-in;
   "shared, never copied" holds.
3. App-key fallback (plain Linux, user-writable paths): after hash-tree
   verification the loader COPIES each verified region into anonymous
   memory, then mprotect(PROT_EXEC) (never writable thereafter — W^X
   sequence, not simultaneity). Cost: private RSS = bound CODE bytes,
   CI-tracked (charter 7); the §4.3 "<1 MB private-dirty" budget and the
   "never copied" claim are re-scoped to modes 2/LINKED. mlock-pinning was
   considered and rejected (pins RSS without removing the copy's simplicity;
   RLIMIT_MEMLOCK fragility).
4. Deployment policy may instead decline: unverifiable-and-uncopyable ⇒
   Tier L (existing AOT-D19 fail-safe).

### I.6 — round-6 main-doc compressions (full-text record)

To fit C35-C39 under the cap, draft 9 compressed: §0 non-goals wording; §0.5
(intro, ARCH-S/ARCH-L/decision wording); §1.2 (overlay-selection paragraph —
normative full text remains G.6; AOT-D10 cite list); §1.3 (intro; AOT-D2
paragraph — per-immediate table remains Annex D/F3, FTL branch G.8; the C37
exit-site paragraph is a summary of I.3); §1.4 (intro + step 0 — full rule
G.2); §1.5; §2.1 items 1/3 (full audit G.3); §2.2 channels 2/3 (full record
E.1); §2.3 (AOT-D12/C21 wording; AOT-D18 — full record G.9; ledger MathIC
sentence — full record H.5; the C35 ledger entry is a summary of I.1); §2.5;
§2.6; §3.1 intro + rows 1/2/4/6/7/10/11/12 (full texts G.2, A+H.2, E.2/D, H.4,
I.2); §3.2 obligations (full mechanization C18); §3.3 (realms — full rule
G.5+H.1; steps 1/2b/3 — full texts H.7, E.3+H.1, G.3); §3.4 weak-death (full
record Annex D); §3.5 (BUNDLE — full record G.10+C33+I.5); §4.0 caveats; §4.3
(integrity/memory bullets are summaries of I.5); §4.4 (C32 full text H.8);
§5.1-§5.4 wording; §6 (full text E.4); §7 (AOT-I13 wording; charter items
2/3/9). No invariant, decision, or file:line grounding was dropped; every
compressed clause's normative content lives in the cited annex or corrections
entry; cite-list trims kept at least one anchor line per claim.

Charter item 3, pre-compression full matrix (the main doc now cites this
text): per PRECOND category, break it (i) before load ⇒ never binds, (ii)
after load ⇒ fires, bit clears, in-flight activations exit, next call LLInt;
both modes (AOT-I7). Plus weak-death (AOT-I11): referent collected ⇒ sweep
clears bit + poisons slot, guards fail — incl. recycled-`StructureID`/reused
address. Exceptions (AOT-D11): throw through AOT frames (incl. inlined) ⇒
correct handler + `Error.stack`. Plus (r4): path-step exotica (C18:
Proxy/getter on a path step ⇒ never binds, NO trap runs); reassigned export
(C17: post-bind reassignment incl. via in-module eval ⇒ fires or was guarded,
never reads stale); two-realm differential (C19: per-realm structures +
per-global watchpoint state, no cross-realm leakage). Plus (r5): two-realm
exits resume each in its OWN realm's LLInt CodeBlock (C25); cross-realm
closure at a D13-weakened inlined site exits, never runs inline (C26);
perturbed loader capacity/ClassInfo ⇒ fail-closed, never wrong reads (C27);
C-API masquerader post-bind ⇒ fires + `typeof` correct, pre-bind ⇒ fails
closed (C28). Round 6 adds (binding, charter scope): the I.1 lazy-slow-path
freeze gate (every `lazySlowPath()` site enumerated; unaudited stub in CODE =
violation), the I.2 `AbsoluteAddress` tool-mode hard-error check (a CI tool
build must FAIL on a synthetic `AbsoluteAddress` bake), and the I.5 integrity
matrix (tampered artifact post-verification + forced eviction ⇒ no attacker
bytes execute in app-key mode — copy semantics make this structural; tampered
pre-bind ⇒ region declines, Tier L).

## Annex J — Round-7 adversarial review record (2026-06-10, BINDING)

6 external findings. Every tree-fact claim independently re-verified against
this tree before acceptance. Disposition table:

| # | Finding | Tree verification | Disposition |
|---|---|---|---|
| 1 | Pathable frozen-cell "load-time identity precondition" undefined; sanctioned guards circular; pre-bind patched builtin runs intrinsic unguarded | Verified: term appears once (§3.1 row 2), defined nowhere; C17 (§2.1.3) and C28/H.6 fallback both compare against the table-loaded value the C18 walk binds; charter 3 had no patched-builtin row; no general builtin-method watchpoint set exists in this tree | **ACCEPTED (blocker) — C40/J.1** |
| 2 | DFG `SpeculationRecovery` absent from ramp identity / EXITMETA / recovery interpreter | Verified: `OSRExit::compileExit(..., SpeculationRecovery*, ...)` `dfg/DFGOSRExit.cpp:391`; `FixedVector<SpeculationRecovery> m_speculationRecovery` `dfg/DFGJITCode.h:311`; `grep SpeculationRecovery docs/aot/*.md` = 0 hits pre-round | **ACCEPTED — C41/J.2** |
| 3 | Overlay trigger per-UnlinkedCodeBlock (shared) vs per-(function × realm) selection state; jitless replace path `dontJITAnytimeSoon`; re-arm unspecified; slow paths not in MODIFIED ledger | Verified: `llint/LowLevelInterpreter.asm:1830-1836` ticks `UnlinkedCodeBlock::m_llintExecuteCounter` (`bytecode/UnlinkedCodeBlock.h:536,544`); `llint/LLIntSlowPaths.cpp:617-637` jitless branch calls `dontJITAnytimeSoon()` | **ACCEPTED — C42/J.3** |
| 4 | AOT-D16 array floor cites FuzzerAgent, but the hook covers only ValueProfile predictions; `getArrayMode` unhooked; observed==0 ⇒ Unprofiled ⇒ ForceExit | Verified: hook only in `getPredictionWithoutOSRExit` (`dfg/DFGByteCodeParser.cpp:1001-1004`); `getArrayMode` (`:1079-1110`) reads profile directly; `dfg/DFGArrayMode.cpp:71-72` `case 0: return ArrayMode(Array::Unprofiled)`; merges are OR-only (`bytecode/ArrayProfile.h:276,316-317`) | **ACCEPTED — C43/J.4** |
| 5 | §4.1 KEEP sum excludes the C38 +0.50 row; §4.2 applies -Os/LTO factor to flag-immune artifact bytes | Verified by re-adding the table rows (=14.70 without the builtin row; 15.20 with) and by the round-6 change-log/I.4 sequence; nm re-bucketing on the current build reproduces survey §3.2 | **ACCEPTED — C44 (corrections-log entry; §4.1/§4.2 edited in place per Annex C rule)** |
| 6 | No delivery mode maps onto Bun's actual distribution (prebuilt runtime, blob-append `--compile`, hardened macOS); app-key costs never composed | Verified against §3.5/AOT-D19/C39 text (I.5 names generic Linux as the fallback case) and §4.3's separate, uncomposed gates | **ACCEPTED — C45/J.6** |

0 refuted. Main-doc compressions to fit under the cap: J.7.

### J.1 — C40 identity precondition (full text; amends §3.1 row 2, C17, C28/H.6)

Every PRECOND frozen-cell entry (pathable lattice tier) carries, serialized at
tool time, an IDENTITY descriptor; at bind, the C18 path walk resolves the
cell, then the loader compares the resolved cell against the descriptor
BEFORE materializing the table slot or subscribing equivalence watchpoints:

1. **Bundle function** ⇒ ExecutableID (source URL + content hash + bytecode
   offset, §2.1 identity). Bind verifies the resolved `JSFunction`'s
   `FunctionExecutable` IS this realm's executable for that ID — C26's
   mechanism, generalized from unpathable closures to pathable cells.
2. **Host/native function** ⇒ resolved cell must be a `JSFunction` whose
   `NativeExecutable` native pointer equals a named class-R symbol recorded by
   the tool, PLUS the `Intrinsic` enum value the compiler consumed at the
   site. Expressible today: operation/host-function pointers are already
   class-R (§3.1 row 4); the tool records the symbol, the loader resolves it
   through the same GOT and pointer-compares.
3. **Engine-builtin JS function** ⇒ builtin-corpus executable index (AOT-D20's
   flavor-hash-keyed identity): the resolved function's executable must be
   the runtime's builtin executable at that index.
4. **Primitive JSValue** ⇒ serialized value equality (incl. NaN bit pattern).
5. **Any other cell kind** ⇒ fail closed at tool time (do not emit the
   freeze) or, if reached at bind, per-function fail-closed.

Mismatch at bind ⇒ per-function fail-closed (Tier L), reason recorded
(demotion-report schema). Post-bind behavior unchanged: equivalence
watchpoints subscribe AFTER identity passes, so a later patch fires and
clears the validity bit. Consequences: a pre-bind `Math.pow` polyfill fails
clause 2 (patched closure's NativeExecutable differs — or it is a JS function
where a native one was recorded) ⇒ the function with the intrinsified body
never binds; the §2.1.3 re-export `export const pow = Math.pow` captured at
module eval binds only if the captured value passes the recorded identity.
Amendments (frozen-spec, recorded here, prior texts unedited): C17's "guarded
CheckCell/CheckIsConstant" and C28/H.6's "guarded form against the
table-loaded value" both now require the comparand slot to have passed C40
identity; a guard against an identity-unverified table value is the forbidden
circular form. §2.1.4 "intact builtin prototypes" assumptions are valid ONLY
when backed by a named per-global watchpoint set (the C28 closed enumeration)
or by C40-checked frozen-cell facts; neither ⇒ the assumption may not be
compiled in. Charter 3 rows added: (i) pre-bind patched builtin at an
intrinsified/devirtualized/folded site ⇒ function never binds (or, where the
fact was compiled as a guard, the guard exits) — the intrinsic NEVER executes
for the patched callee; (ii) post-bind patch ⇒ watchpoint fires, bit clears,
in-flight activations exit at next check.

### J.2 — C41 SpeculationRecovery (full text; amends AOT-D2, §2.4 item 4, charter 5)

DFG `SpeculationRecovery` records describe register-state UNDO performed
in-place by speculative fast paths before their checks (e.g. SpeculativeAdd:
subtract back the addend; BooleanSpeculationCheck: restore the xored tag).
`OSRExit::compileExit` (`dfg/DFGOSRExit.cpp:391`) receives the record (from
`DFGJITCode::m_speculationRecovery`, `dfg/DFGJITCode.h:311`, via the exit's
recoveryIndex) and emits the undo BEFORE value recovery. Amendments:
(a) AOT-D2 ramp identity = (pointer-parameterized ValueRecovery/ExitValue
stream, SpeculationRecovery record-or-none) — dedup keys on the PAIR; two
exits sharing a stream but differing in undo records get distinct ramps.
(b) EXITMETA (§2.4 item 4) serializes the `m_speculationRecovery` vector and
each exit descriptor's recoveryIndex.
(c) The C++ recovery interpreter applies the undo to the CAPTURED register
file first, then replays recoveries — required for it to remain the
universal backstop and the charter-5 oracle.
(d) Charter 5 gains a case whose exit state requires a SpeculativeAdd undo;
ramp output and interpreter output must agree on the un-done register.
(e) FTL is unaffected: B3-generated exits carry no SpeculationRecovery; the
FTL (C22) stream needs no undo leg — stated so a future reviewer does not
re-open it.

### J.3 — C42 shared-trigger selection protocol (full text; amends G.6, §2.3 ledger, charter 9)

Facts: the `checkSwitchToJIT` counter is on the UnlinkedCodeBlock
(`llint/LowLevelInterpreter.asm:1830-1836`;
`bytecode/UnlinkedCodeBlock.h:536,544`), shared per-VM across (i) realms and
(ii) all closures/CodeBlocks of one source, via the CodeCache — i.e. the
level-0 CachedBytecode payload the design ships. Selection state (C19/G.5) is
per (artifact function × JSGlobalObject). Jitless `llint_replace` calls
`dontJITAnytimeSoon()` (`llint/LLIntSlowPaths.cpp:617-637`), deferring the
shared counter essentially forever on first trip. Protocol (normative):

1. The LLInt `replace` and `loop_osr` slow paths are **MODIFIED runtime
   components** under `ENABLE(AOT_RUNTIME)` (ledger row added — previously
   LLInt was listed REUSED UNCHANGED; the delta is these two slow paths
   only). For a lattice function they dispatch to the selector with the
   EXECUTING CodeBlock's (function × realm) entry; they NEVER call
   `dontJITAnytimeSoon` for lattice functions. Non-lattice functions keep
   today's jitless behavior.
2. **Re-arm rule** (supersedes G.6's "re-armed only if the generic body is
   uninstalled"): after EVERY trip the slow path unconditionally re-arms
   `m_llintExecuteCounter` to the selection threshold. The counter is a
   shared TRIGGER, not selection state; re-arming costs one store.
3. Spurious trips are benign by construction: a trip for a (function × realm)
   already selected/declined is a selector no-op (sticky-with-decay state per
   G.6); other realms'/closures' LLInt-resident CodeBlocks can still trip
   because of rule 2. `loop_osr` trips select but do not OSR-enter (AOT-D1).
4. Charter 9 gains (vi): two realms, same lattice function, both driven hot —
   BOTH realms must reach selection (variant or generic per their own
   profiles); a never-selecting second realm is the C20/C42 regression.

### J.4 — C43 array-mode floor mechanism (full text; splits AOT-D16)

AOT-D16(a) — value predictions: unchanged; StaticFactAgent at the FuzzerAgent
seam (`dfg/DFGByteCodeParser.cpp:1001-1004`) returns the static fact or
`SpecBytecodeTop`; SpecNone never reaches `ForceOSRExit` from a fact-less
value site.
AOT-D16(b) — array modes: the agent seam does NOT cover `getArrayMode`
(`:1079-1110` reads the ArrayProfile directly; observed==0 ⇒
`ArrayMode(Array::Unprofiled)`, `dfg/DFGArrayMode.cpp:71-72` ⇒ ForceExit). The
floor is instead channel 1 (slot injection): for every fact-less array-profiled
site the tool pre-writes into the linked CodeBlock's ArrayProfile a SATURATED
conservative state — `m_observedArrayModes` = the union of all ArrayModes bits
relevant to the site's `Array::Action` (per-Action pattern frozen at
implementation; Write sites include the store-conversion shapes), plus
`outOfBounds`, `mayStoreToHole`, and `mayInterceptIndexedAccesses` — so
`fromObserved` selects `Array::Generic`/`SelectUsingPredictions` with
`makeSafe`, never Unprofiled, and downstream code is generic-but-direct, not
ForceExit. Soundness of the seed: profile updates are merge-only
(`observeArrayMode` = `atomicExchangeOr`; `computeUpdatedPrediction` ORs in
bits — `bytecode/ArrayProfile.h:276,316-317`), so tool-VM activity can only
add bits, and added bits only widen an already-generic mode. Sites WITH
static facts get exact bits (channel 1 as before). Alternative recorded and
rejected: a tool-only hook inside `getArrayMode` under the "~3 sites
DFGByteCodeParser MODIFIED" allowance — workable, but slot saturation needs
zero parser changes and exercises the same code path live profiles do.

### J.5 — C44 budget reconciliation (record)

Full reasoning in the corrections-log entry (C44). §4.1/§4.2 edited in place
per the Annex C measurement rule (arithmetic reconciliation, not a
re-measurement): KEEP sum 14.70 → 15.20 MB (label "compressible C++ text +
artifact section"), raw 17.4 → 17.9 MB; §4.2 bridge = (14.70 × ~0.45-0.55
retained) + 0.50 fixed ⇒ ≈ 6.6-8.6 MB band, ~9.7 MB at the conservative end
of the extrapolation — within <10 MB ONLY if the -Os/LTO bridge delivers;
restated as such. Any future AOT-D20 list growth adds to the FIXED term and
must be netted against compressible reductions (I.4's "list shrinks before
budget grows" now has a stated arithmetic home).

### J.6 — C45 BUNDLE-EMBEDDED + platform matrix (full text; amends AOT-D19/§3.5/§4.3)

**Mode BUNDLE-EMBEDDED** (a BUNDLE sub-mode, not a third loader): the
artifact is embedded INSIDE the executing binary — the prebuilt `bun` runtime
(engine-builtin AOT-D20 section) or a `bun build --compile` single-file app
(appended blob, today's mechanism). Properties: the backing file is the app
binary itself, which (a) on macOS is platform-code-signed ⇒ kernel re-verifies
page-in ⇒ kernel-verified class: direct PROT_READ|PROT_EXEC map, shared,
never copied — and because the SIGNED BINARY carries the bytes, hardened-
runtime macOS is served without the unsigned-executable-memory entitlement
(the C24 exclusion applied to a SEPARATE unsigned file); (b) on Linux is
verity/IMA-coverable exactly when deployments already cover the executable —
managed fleets get the kernel-verified class; (c) generic Linux: app-key
hash+copy per C39, scoped to BOUND regions only (lazy §3.3 ⇒ pay-as-you-bind).
Per-platform matrix (normative defaults):

| Platform | Mode | Integrity class | RSS/startup consequence |
|---|---|---|---|
| macOS (incl. hardened) | BUNDLE-EMBEDDED | platform signature (kernel) | mapped, shared; §4.3 budgets as-is |
| Linux w/ fs-verity/dm-verity/IMA | BUNDLE-EMBEDDED or BUNDLE | kernel | mapped, shared; §4.3 as-is |
| generic Linux | BUNDLE-EMBEDDED | app-key (C39) | copy RSS = bound CODE; composed startup gate below |
| Windows | BUNDLE-EMBEDDED | app-key (C39) until a kernel-verified path is qualified | same as generic Linux |
| iOS / app-store | LINKED (unchanged) | app signature | per §3.5 |

LINKED remains the mode for embedders with native link steps; Bun's flows
never require one. §4.3 amendment: the CI budget table gains a COMPOSED
app-key row — first-N-calls latency = bind cost + hash+copy of the regions
those N functions bind (reference app, N=2,000: hash+copy of bound code at
~1 GB/s SHA-256 budgeted ≤ 12 ms ON TOP of the ≤ 10 ms bind budget; gate
both) — and an RSS row (app-key copy bytes = bound CODE bytes, already
CI-tracked per C39, now reported per-platform-mode). §5.4's footprint row
implicitly assumed "mapped, shared"; on app-key platforms the honest
comparison includes the copy RSS — recorded here so the embedder-facing
claim is not overstated.

### J.7 — round-7 main-doc compressions (full-text record)

To fit C40-C45 under the cap, draft 10 compressed wording (meaning-preserving;
where a clause was dropped, its full text is already recorded in a BINDING
annex, cited inline at the compressed site): header conventions; §0 non-goals
N1; §0.5 (intro, ARCH-L paragraph, all four cost-table cells, decision
paragraph, epitaphs — full cost-model text remains Annex F); §1.1 (Tier-A
sentence, CachedBytecode sentence, AOT-D15 tail); §1.2 (AOT-D1 tail; C15
layout-mismatch clause); §1.3 (intro cites merged; AOT-D2 parameter-block
input list; C37 in-tree wiring description — full ABI I.3); §1.5; §2.1 (tool
intro; constructor-inference plumbing sentence; literal element-kind clause;
C17 sentence — full rule G.3; UntypedUse closing); §2.2 (channel-1
parenthetical); §2.3 (AOT-I5 restatement — full text §7 + C11; C21
parenthetical; AOT-D18 wording; ledger rows C35/C42/JITOperations/AOT_RUNTIME;
relocation-table clause); §2.4 (HEADER validation tail; PRECOND enumeration —
full closed rule H.6 as amended by C40/J.1; artifact intro); §2.5 ("Java-style";
skip-clause tail — executability/`toString` guarantee unchanged, AOT-D15
governs; AOT-D20 rejection tail — full record I.4); §2.6 (B.8 clause); §3.1
(intro framing note; row 1, row 2 C26 wording — full text H.2; rows 4, 5, 7,
11, 12 cell wording); §3.2 (resolution paragraph; AOT-D6 parenthetical — full
rationale Annex A); §3.3 (intro realm sentence; step-1 C15-on-spine phrasing —
C27/H.7 unchanged; step-2b cite); §3.4 (cost sentence folded; weak-death
wording — full record D); §3.5 (BUNDLE bullet — full record G.10/I.5); §4.2
(precedent clause); §4.3 (eager-hashing parenthetical — rationale unchanged in
C33/I.5; composed-gate phrasing); §4.4 (LINKED-cost clause; protocol phrasing);
§5.1 (item-3 "honest downgrade" tag; net-paragraph term); §5.2; §5.3 (retry
window phrasing); AOT-I5 trailing clause (the silent-corruption framing — full
text I.2); AOT-I9 ("pointer-keyed container order" → "pointer order"); charter
3 (matrix row phrasing — full text I.6 + J.1); charter 9 item (vi) phrasing.
No invariant, decision, mechanism, number, or file:line grounding was removed;
the only deliberate semantic additions this round are the C40-C45 deltas.

## Annex K — Round-8 external verification record (2026-06-10, BINDING)

7 external findings. Every tree-fact claim independently re-verified against
this tree before acceptance (re-measurements re-run on
`WebKitBuild/Release/bin/jsc`, the Annex C binary). Disposition table:

| # | Finding | Tree verification | Disposition |
|---|---|---|---|
| 1 | E.3 recovery interpreter has no failure contract for inlinee-CodeBlock materialization (OOM/throw mid-OSR-exit); "any failure ⇒ LLInt" undefined on the deopt spine | Verified: `ScriptExecutable::newCodeBlockFor` declared `runtime/ScriptExecutable.h:115` — heap-allocating, failable; grep for OOM/materialization-failure text in docs/aot/ = 0 hits; stock JSC never reaches this state (DFG inlines only executed CodeBlocks) so no in-tree precedent exists | **ACCEPTED — C46/K.1** |
| 2 | §2.1.3 "bundle closed" has no rule for import edges leaving the bundle (native `.node` addons, `node:`/`bun:` builtins, externals) — an analyzer could fold a host export via C17 | Verified: grep native-module/external-import treatment in docs/aot/ = 0 hits; §1.5 covers dynamically LOADED code only | **ACCEPTED — C47/K.2** |
| 3 | arm64e/PAC is one clause ("PAC per platform convention", §3.5) though LINKED is the flagship iOS mode; binder/recovery-interpreter/emitter PAC obligations unstated | Verified: zero arm64e/ptrauth mechanism text in history; `LowLevelInterpreter64.asm:575-577` shows the ARM64E `JSCConfigGateMapOffset` gate dance; return-PC tagging today lives in JIT-emitted thunk glue (`llint/LLIntThunks.cpp`) | **ACCEPTED — C48/K.3** |
| 4 | E.2 audit omits the LLInt return-location label family the design routes every inlined-frame exit through; EXITMETA never says who resolves the per-opcode return-location symbol | Verified: `callerReturnPC` (`dfg/DFGOSRExitCompilerCommon.cpp:148-183`) selects per-opcode/width `LLInt::returnLocationThunk(...)` + `checkpointOSRExitFromInlinedCallTrampolineThunk` (:153-154) + `arraySortComparatorReturnTrampolineThunk` (:160-161, discards return value, re-dispatches caller's `op_call`); thunks JIT-generated `llint/LLIntThunks.cpp:781,791,801` inside `#if ENABLE(JIT)` (:388) — absent jitless | **ACCEPTED — C49/K.4** |
| 5 | Size budget excludes Inspector (+0.51) though Bun ships `--inspect` in release; AOT-I10 gates per flavor ⇒ the shipping flavor busts the conservative bridge | Verified: §4.1 Inspector row "DROP in min build"; AOT-I10 "CI-enforced per flavor"; grep inspector in history = 0 hits — never reconciled | **ACCEPTED — C50/K.5** |
| 6 | +0.15 MB `ENABLE(AOT_RUNTIME)` allocation unmeasured; two constituents alone measure ~0.37 MiB raw; F10 names `StructureStubInfo`, which C30 established does not exist here | Re-measured (K.6): CallLinkInfo family 0.217 MiB + PropertyInlineCache/InlineCacheHandler 0.144 MiB = 0.361 raw; retained-shape split ~0.20 MiB before readers/JITCode/plumbing; `bytecode/PropertyInlineCache.h` exists, no StructureStubInfo file | **ACCEPTED — C51/K.6** |
| 7 | J.6 macOS BUNDLE-EMBEDDED kernel-verification claim omits the code-signature-coverage requirement: bytes appended after LC_CODE_SIGNATURE are not hashed ⇒ unauthenticated exec mapping (§3.5's own forbidden case) | Verified against J.6 text ("appended blob, today's mechanism" + "platform-code-signed ⇒ kernel re-verifies page-in" with no coverage precondition); Mach-O code-directory semantics are public-spec facts | **ACCEPTED — C52/K.7** |

7/7 accepted, 0 refuted. Main-doc compressions to fit: K.8. Charter additions: K.9.

### K.1 — C46 exit-time inlinee-materialization failure contract (amends E.3; scopes AOT-I3/AOT-I4)

Rule (binding): the recovery interpreter performs inlinee-CodeBlock
materialization (E.3 step 3) inside a no-throw, GC-deferred region; if
allocation or linking fails there, the condition is FATAL RESOURCE
EXHAUSTION — a controlled crash with diagnostic (function, inline depth,
allocation site), never UB and never a corrupted resume. This is the design's
single recorded carve-out from "any failure ⇒ LLInt": at that point the
Tier-A activation is mid-deconstruction and every sound resume target —
including a catch handler inside the very inlinee being materialized —
requires the CodeBlock that just failed. Precedent: JSC already crashes on
OOM in unrecoverable critical sections. Rejected alternatives (rationale,
recorded): (b) "decline bind when a handler lives in an unmaterialized
inlinee" is INSUFFICIENT — the normal (non-exception) resume path needs the
inlinee CodeBlock too, so the restriction would not remove the failure path,
only narrow it; (c) "reserve the allocation at bind" degenerates to eager
materialization, which is impossible in general — the inlinee closure may
not exist at bind time (E.3's founding constraint). Eager materialization
where the closure DOES exist at bind remains the default
quality-of-implementation behavior (it already is: E.3 step 1 fills existing
CodeBlocks), shrinking the carve-out's reachable surface to
closure-not-yet-created inlinees. AOT-I3's "never UB" stands unweakened;
"always LLInt" is now "always LLInt, or the C46 controlled-crash carve-out
under exit-time allocation failure". Charter row: K.9(a).

### K.2 — C47 bundle-boundary rule (amends §2.1.3; interacts C17/G.3, C40/J.1, AOT-D16/C43)

Rule (binding, closing the analyzer's spec): a module-graph edge whose
resolved target is NOT bundle-internal — native addons (`.node`),
`node:`/`bun:` builtin specifiers resolved to host implementations at
runtime, build-time-declared externals — yields BOTTOM static facts. The
importing module's LOCAL binding may still carry a C17 once-assignment
watchpoint (that is a fact about the binding, checkable at bind), but the
binding's VALUE is never foldable, never an inlining callee, and never a
`CheckCell`/`CheckIsConstant` comparand: no C40-expressible identity exists
at tool time for host-produced cells (C40 clause-2 native-function symbols
identify ENGINE natives by `NativeExecutable` symbol — they convey no value
facts about host-module exports and do not apply here). Call sites and
property sites through such bindings compile generic-but-direct (the
AOT-D16/C43 floor). This makes the previously-implicit soundness chain (no
tool-known value ⇒ no fold; C40 clause 5 fail-closed) an explicit emission
rule the analyzer implementer cannot miss. Charter row: K.9(b).

### K.3 — C48 arm64e/PAC obligations (amends §3.5 LINKED, AOT-D2/AOT-D11, I.3; extends E.2 — recorded, E.2 not edited)

LINKED is the iOS mode; jitless LLInt already ships on arm64e in production,
so feasibility is not in question — these are the concrete obligations the
NEW/MODIFIED components carry there:

1. **Bind-time slot signing.** Class-M/R/GOT entries holding code pointers
   (`JSEntryPtrTag`/`OperationPtrTag` classes) are PAC-signed values on
   arm64e; the loader/binder signs each slot at bind time per the tag's
   platform convention (data writes — no entitlement involved). The §3.3
   binding sequence and the C37/I.3 exit-site ABI inherit this as a per-slot
   step; symbolic (selector-written) slots sign at selection time under the
   same rule.
2. **Recovery-interpreter return-PC signing.** Reconstructed caller return
   PCs (the K.4/C49 family) are signed against the frame being built —
   modifier = runtime frame address — via ptrauth intrinsics at recovery
   time. A static parameter block CANNOT carry pre-signed PCs (the modifier
   does not exist until the frame does); today the equivalent signing lives
   in JIT-emitted ramp/thunk glue (`llint/LLIntThunks.cpp`), which the
   product lacks. The C++ recovery interpreter is the signer.
3. **Gate discipline.** Entries from LLInt/runtime into AOT CODE and back
   use the existing `g_config` gate-map discipline
   (`LowLevelInterpreter64.asm:575-577`, `JSCConfigGateMapOffset`); AOT entry
   points are gate-map participants, not raw branch targets.
4. **Emitter relocations.** `AOTObjectLinkBuffer` in LINKED mode emits
   ptrauth-aware relocation kinds (Mach-O `@AUTH` / `ARM64E_AUTH`-class) for
   every signed pointer materialized from `.rodata`/`.data.rel.ro` (jump
   tables C31, parameter-block code references, GOT initializers).
5. **E.2 PAC column (recorded amendment).** The E.2 audit table gains a PAC
   column: each thunk row records the signing discipline of every baked or
   linked pointer; the E.2 closed rule now reads "a thunk body may ship only
   after its immediate audit INCLUDING its PAC column lands". The K.4 rows
   below are born with that column.

Charter row: K.9(c).

### K.4 — C49 E.2 extension: return-location label family (amends E.2 — recorded; amends E.3/AOT-D2 dedup story)

New audit rows (E.2 format: thunk | baked immediates today | product
classification | PAC):

| Thunk | Today | Product | PAC |
|---|---|---|---|
| `*_return_location` family — per-opcode × width offlineasm labels for `op_call`, `op_call_ignore_result`, `op_iterator_open`, `op_iterator_next`, `op_construct`, `op_super_construct`, `op_call_varargs`, `op_construct_varargs`, ... (`LLInt::returnLocationThunk`, `llint/LLIntThunks.cpp:801`, JIT-generated wrappers, `#if ENABLE(JIT)`) | wrapper = LinkBuffer-generated jump to the offlineasm label | NOT shipped as wrappers: the labels are exported link symbols, class-R (same treatment as the E.2 OSR-exit trampoline row) | return-PC values signed at frame reconstruction per K.3(2) |
| `checkpointOSRExitFromInlinedCallTrampoline` (`llint/LLIntThunks.cpp:781`) | wrapper around offlineasm label | class-R direct to the label | K.3(2) |
| `arraySortComparatorReturnTrampoline` (`llint/LLIntThunks.cpp:791`; selected at `dfg/DFGOSRExitCompilerCommon.cpp:160-161`) | REAL CODE, not a label jump: discards the comparator return value and re-dispatches the caller's `op_call` | ships as an audited CODE body in the shared-thunk section, per-immediate audited like any E.2 row | body's own embedded pointers audited; produced return PC signed per K.3(2) |

Resolution rule (binding, closes the EXITMETA gap): the per-exit
return-location symbol is NOT carried in the parameter block and is NOT part
of the ramp dedup key. The recovery interpreter (which v1 routes all
inlined-frame and exceptional exits through — AOT-D11) derives it at exit
time from the caller's resume CodeBlock instruction stream — opcode + width
of the call instruction at the resume index — mirroring `callerReturnPC`'s
switch (`dfg/DFGOSRExitCompilerCommon.cpp:148-183`). Dedup key therefore
remains exactly the AOT-D2/C41 pair (parameterized stream, undo record).
Charter row: K.9(d).

### K.5 — C50 gated-flavor naming: inspector ships (amends §4.1/§4.2; scopes AOT-I10)

Rule (binding): the AOT-I10 < 10 MB gate applies to every SHIPPING flavor,
and the product (Bun) ships `bun --inspect` in release builds — so the
gated Bun flavor INCLUDES the Inspector bucket. §4.1's Inspector row flips
to KEEP (+0.51 MB compressible) for that flavor; a separately-budgeted
inspector-off MIN flavor remains (its band = the Bun-flavor band − 0.51 ×
factor). Consequence, composed with C51: KEEP sum 15.20 → 15.81 MB
(compressible 14.70 → 15.31; artifact 0.50 unchanged); §4.2 band ≈ 6.9-8.9 MB
optimistic, ~10.1 MB at the conservative factor — the CONSERVATIVE END NOW
EXCEEDS THE GATE. Recorded honestly rather than re-tabled: the -Os/LTO
bridge must beat the conservative factor, or the inspector/retained buckets
must shrink, or the gate fails at the AOT-I10 milestone build — which is a
design-gate failure, not a table edit (§4.2's existing rule).

### K.6 — C51 `ENABLE(AOT_RUNTIME)` allocation re-measured: +0.15 → +0.25 (amends Annex D F10 — recorded, D not edited; amends §4.1, Annex C)

Measurement (2026-06-10, same binary and method class as Annex C:
`nm --print-size -C` text symbols, `WebKitBuild/Release/bin/jsc`, -O2):

- `CallLinkInfo`/`OptimizingCallLinkInfo`/`DataOnlyCallLinkInfo`/
  `BaselineUnlinkedCallLinkInfo`/`DirectCallLinkInfo` family: **0.217 MiB**.
- `PropertyInlineCache` + `InlineCacheHandler`: **0.144 MiB**
  (`InlineCacheCompiler` measured separately at 0.084 MiB — tool-only,
  excluded).
- Retained-shape estimate: dropping symbols matching the compiled-out forms
  (`emit*`/`Repatching*`/`setStub`/`generate*`/JIT/Snippet) leaves
  **0.160 MiB** of the two families; + `OSRExit`-named 0.022 + `ValueRecovery`
  0.016 ⇒ **≥ 0.20 MiB measured floor** BEFORE the EXITMETA reader types, AOT
  JITCode class, validity-bit watchpoint plumbing, and exit-count tables F10
  also lists.

Allocation restated to **+0.25 MB** (measured floor + headroom); per the
Annex C rule this remains an allocation finalized at the AOT-I10 milestone.
Two binding riders: (1) the milestone build must compile with
`ENABLE(AOT_RUNTIME)` ON — a plain `ENABLE(JIT)=OFF` build UNDERCOUNTS this
bucket because it compiles `CallLinkInfo`/`PropertyInlineCache` out entirely;
(2) naming re-grounded: Annex D F10's "data-IC `StructureStubInfo`
(self-access subset)" reads as "data-IC `PropertyInlineCache`/
`InlineCacheHandler` (self-access subset)" per C30/H.3 — this tree has no
`StructureStubInfo` (recorded here; D is not edited).

### K.7 — C52 BUNDLE-EMBEDDED signature-coverage requirement (amends J.6, §3.5, §4.3)

Rule (binding): every platform-signature row of the J.6 matrix (macOS
foremost) REQUIRES the embedded artifact bytes to lie INSIDE the range the
platform signature actually covers — for Mach-O, within the code directory's
hashed range. Bytes appended after `LC_CODE_SIGNATURE` are NOT hashed: the
kernel does not verify their page-ins, and mapping them PROT_EXEC is exactly
the unauthenticated-executable-mapping §3.5 forbids. Mechanism: `bun build
--compile` embeds the artifact in a real segment/section and RE-SIGNS the
binary after embedding (Bun's tooling already rewrites the binary; ad-hoc or
developer-ID signing both satisfy coverage). The loader VERIFIES coverage —
blob offset+length within the code directory's hashed range — before
classifying the mapping kernel-verified; uncovered ⇒ degrade to the C39
app-key copy path or Tier L, never the direct exec map. A naive
"append + don't re-sign" build therefore fails CLOSED (app-key/Tier L), not
open. The hardened-runtime claim (no JIT entitlement needed for file-backed
signed exec mappings) is conditioned on the same coverage precondition.
Charter row: K.9(e), a negative test.

### K.8 — round-8 main-doc compressions (full-text record)

To fit C46-C52 under the cap, draft 11 compressed the following passages
(meaning-preserving; originals verbatim here):

- **§5.2 first sentence block, was:** "Versus the product's floor (jitless
  LLInt, ~2-6x off baseline+): compiled functions get DFG/FTL-grade code.
  Versus XS/QuickJS: same ballpark cold; no interpreter reaches DFG/FTL
  output on hot code. Thesis: interpreter-class deployability (LINKED needs
  no JIT entitlement; BUNDLE scoped per AOT-D19), JIT-class hot loops."
- **§6 body, was:** "Obligations for the joint threads design (full text
  Annex E.4): (1) `useJSThreads` flavor-pinned via the HEADER hash (AOT-I8).
  (2) Validity-bit clears are cross-thread events: store-release under the
  safepoint/STW discipline of watchpoint fires; checks at every point DFG
  would plant a jump replacement, no fewer. (3) Binding runs under
  `reallyAdd`'s world-state guarantee; N mutators ⇒ safepoint participant.
  (4) Demotion counters racy-by-design; the validity bit is the only
  synchronized control. (5) Variant (de)selection = entry publish after slot
  binds, discipline (2); slots write-once between deselections (C16)."
- **§4.3 Integrity bullet TOCTOU clause, was:** "TOCTOU (C39): file-backed
  PROT_EXEC pages re-fault from an attacker-writable file after a user-space
  check — direct exec-mapping only where the kernel re-verifies page-in
  (platform signing; fs-verity/dm-verity); the app-key fallback COPIES
  verified regions to anonymous memory before mprotect(PROT_EXEC), else
  Tier L."
- **§1.3 AOT-D2 realm clause, was:** "ramps load a per-site **parameter
  block** — true `.rodata`, REALM-INVARIANT only (C25/H.1); ALL
  realm-dependent inputs (resume CodeBlock/metadata, table base, counter,
  inlinee slots) are reached through the exiting frame's per-realm AOT
  CodeBlock — a link-time immediate cannot be realm-dependent (C19)."
- **§3.1 row 2 identity parenthetical, was:** "(J.1; bind-checked BEFORE
  subscription: bundle fn ⇒ ExecutableID; native fn ⇒ class-R
  `NativeExecutable` ptr + intrinsic; builtin ⇒ corpus index; primitive ⇒
  value; else fail-closed — a pre-bind `Math.pow` polyfill never binds an
  intrinsified body)."
- **§3.1 row 2 C26 parenthetical, was:** "(C26/H.2: cell-equality vs THIS
  realm's executable from ExecutableID — `GetExecutable`+`CheckIsConstant`,
  `dfg/DFGByteCodeParser.cpp:1571`; cross-realm EXITS; unresolvable ⇒ generic
  call)" — full rule unchanged in H.2.
- **§3.1 row 12 left cell tail, was:** "; outside every typed choke point;
  missed = SILENT corruption (C36/I.2)" — framing unchanged in I.2.
- **§3.1 row 10 tail, was:** "; `Options` branches flavor-pinned (AOT-I8)" —
  rule unchanged in E.2's Options row.
- **§1.2 C15 clause, was:** "; layout mismatch = silent type confusion" —
  rationale unchanged in C15/H.7.
- **§2.3 AOT-D12 clause, was:** "that folds and emits as an **anonymous
  immediate** (only branches/calls/labels have link records)".
- **§2.3 AOT-I5 restatement, was:** "**AOT-I5 (restated decidable, C11)**
  bans any B3/assembler constant from `JSCell*`/`Structure*`/`StructureID`
  outside the table-load path, enforced at *construction* (choke points +
  structure-ID wrappers, `dfg/DFGJITCompiler.h:261`,
  `ftl/FTLLowerDFGToB3.cpp:28154`) — not at the emitter (undecidable)." —
  authoritative statement is §7 AOT-I5, unchanged.
- **§7 AOT-I5 trailing clause, was:** "Extends to tool-VM C++ heap-object
  pointers (`MathIC*`-class, C29) and (C36) to `AbsoluteAddress`/raw-address
  immediates: tool-mode construction over any address outside the class-R
  whitelist is a hard error (§3.1 row 12; audit I.2)."
- **§3.3 realm-state enumeration, was:** "ALL binding state (tables, recipe
  memoization, subscriptions, validity + selection bits) keyed per (artifact
  function × `JSGlobalObject`)" — full enumeration unchanged in G.5.
- **§4.4 protocol phrase, was:** "Protocol before freeze: compile the
  reference app under full-fat `jsc`, forced DFG/FTL ⇒ the **expansion
  factor** per bytecode byte".
- **§5.1 item-3 clause, was:** "⇒ pre-seeded polymorphic ICs
  unimplementable." — conclusion unchanged in C2/A.
- **§5.3 tail, was:** "never \"slower than no AOT\" beyond the retry window —
  never incorrect." (retry-window bound unchanged, §1.4).
- **Charter-3 C40 row, was:** "pre-bind-patched builtin at an intrinsified
  site ⇒ never binds / guard exits — the intrinsic NEVER runs; post-bind
  patch ⇒ fires (C40)." — full text J.1.

No invariant, decision, mechanism, number, or file:line grounding was
removed; the only semantic additions this round are the C46-C52 deltas.

### K.9 — round-8 charter additions (BINDING; main-doc charter item 10)

(a) **C46:** drive an OSR exit through an UNMATERIALIZED inlinee slot under
simulated allocation failure (both the plain-resume and
handler-inside-the-inlinee shapes); verify the recorded behavior — controlled
crash with diagnostic, never UB, never a corrupted resume.
(b) **C47:** a bundle importing a native/builtin module binds and runs
correctly with the host export swapped between runs — no stale fold, no
identity guard against a host-produced cell; sites observed
generic-but-direct.
(c) **C48:** arm64e product runtime: exit + inlined-frame reconstruction
tests pass with PAC enforcement live (signed slot loads, signed reconstructed
return PCs, gate-map entries); a corrupted signed slot faults, never
executes.
(d) **C49:** inlined-frame exits resuming through `op_iterator_next` and
through the array-sort comparator re-dispatch execute correctly on the
product runtime (label-family resolution + the shipped trampoline body).
(e) **C52 (negative):** an artifact blob appended OUTSIDE the
code-signature-covered range never maps executable — loader classifies it
app-key/Tier L; a tampered covered blob fails kernel page-in verification.

## Annex L — Round-9 external verification record (2026-06-10, BINDING)

6 external findings, all tree-fact claims independently re-verified against
this tree before acceptance. **All 6 accepted, 0 refuted.**

| # | Finding (short) | Verified tree facts | Resolution |
|---|---|---|---|
| L-1 | BLOCKER: fresh-call window after watchpoint fire — bit cleared but entry installed; validity checks (jump-replacement positions) need not dominate a fresh call's first folded-fact use | `CodeBlock::jettison` reinstalls: `ownerExecutable()->installCode(vm, alternative(), ...)` `bytecode/CodeBlock.cpp:2803` (the in-tree fresh-call leg the design dropped); DFG invalidation points are effectful/call-site placed, never prologue | **C53/L.1**; §3.4+§6(2) amended; charter L.7(a) |
| L-2 | Re-selection evidence channel does not exist: G.6 cites "exit site's own metadata" but AOT-D2 ramps perform no profile writes and deselect-to-generic freezes LLInt profiles | `dfg/DFGOSRExit.cpp:444-560` refinement block; `:535` `MethodOfGettingAValueProfile`; grep over both docs: zero hits for `MethodOfGettingAValueProfile`/`m_codeOriginForExitProfile`/refine | **C54/L.2**; G.6 amended (deselect ⇒ LLInt-resident) |
| L-3 | Debugger attach + `deleteAllCode`/`clearCode` unwalked: bound breakpoints silently never hit; clearCode vs AOT-D10 permanence; AOT-D7 makes lifecycle demotion process-permanent | `debugger/Debugger.cpp:600` `m_vm.deleteAllCode(PreventCollectionAndDeleteAllCode)` inside `recompileAllJSFunctions`; `runtime/ScriptExecutable.cpp:165` `clearCode`, `:508` memory-pressure comment | **C55/L.3**; AOT-D7 carve-out; charter L.7(b) |
| L-4 | F3/G.8 audits omit the profile-refinement immediate family (per-realm `ArrayProfile*`/value-profile bucket addresses) | `ftl/FTLOSRExitCompiler.cpp:412` `TrustedImmPtr(arrayProfile)`, `:434` `emitReportValue`; DFG twin above; F3 classes (a)-(i) and G.8 (a)-(f) contain neither | **C54/L.2**: F3 class (j), G.8 item (g) — recorded here, D/G not edited per frozen-spec |
| L-5 | <10 MB gate: conservative projection fails while all recorded fallback levers are foreclosed for the gated flavor (C50 inspector, C51 floor, flag-immune builtin list) | K.5/K.6 text as recorded; §4.2 "~10.1 conservative — now EXCEEDS the gate" | **C56/L.4**: milestone ⇒ design-freeze PRECONDITION + remediation order |
| L-6 | 15.31 MB "compressible" base includes flag-immune bytes (asm blob, post-factor allocations, ICU); eh_frame/data undercounted with no scaling assumption | `readelf -SW` Annex C binary: `.text` 23.95, `.eh_frame` 2.28, `.eh_frame_hdr` 0.32, `.data.rel.ro` 0.47, `.data` 0.08 MB | **C57/L.5**: KEEP split + band restated + non-text line |

### L.1 — C53 fire-time entry uninstall (full text; amends §3.4, §6(2), §1.4.1)

Rule (binding): the watchpoint-fire handler must, BEFORE returning to JS,
(1) clear the validity bit (store-release) AND (2) uninstall the entrypoint
of every subscribed (function × realm) binding. Both are data writes
(AOT-I2-clean). The reverse walk uses the §3.3 step-4 subscription objects —
each subscription (the CodeBlockJettisoningWatchpoint analog) carries its
owning binding, exactly the in-tree shape. This replicates BOTH in-tree
soundness legs: entry uninstall = the `jettison`→`installCode` leg (fresh
calls take the LLInt prologue); validity checks at jump-replacement
positions = the in-flight leg (resumed/running frames exit at their next
check). §1.4.1's exit-driven demotion remains as the POLICY layer; C53 is
the SAFETY layer and runs first, unconditionally. Threads (§6(2)): both
writes happen under the same watchpoint-fire safepoint/STW discipline; an
entry publish is store-release, an uninstall likewise; a racing call
observes either the old entry (and exits at its first check only if it
cannot consume a folded fact before one — guaranteed by uninstall-before-
return-to-JS within the firing thread's critical section, and by the STW
discipline for cross-thread fires) or the LLInt prologue. Rejected
alternative: a validity-bit load+branch in every AOT prologue — sound but
pays a per-call cost on every bound function forever to cover an event
(precondition death) that demotes permanently anyway; uninstall is one-time
work at fire, on the fire path, proportional to subscribers. Recorded as
the chosen trade; revisit only if fire-path latency (subscriber-list walk
under STW) measures pathological (charter L.7(a) measures it).

### L.2 — C54 profile-writeback drop + re-selection re-grounding (full text; amends AOT-D2, §2.4 item 4, F3, G.8, G.6, §1.4 step 0)

(1) F3 gains class (j), G.8 gains item (g): the profile-refinement
immediates — `dfg/DFGOSRExit.cpp:444-560` (ramp-compile-time
`codeBlock->getArrayProfile(...)` pointer; `MethodOfGettingAValueProfile::
emitReportValue` bucket addresses, `:535`) and `ftl/FTLOSRExitCompiler.cpp:
408-435` (same pair). Classification: per-realm baseline-CodeBlock metadata
addresses — C29/AOT-I5-class if baked offline, C25-violating if put in
parameter blocks. (2) Decision: **DROP exit-time profile writeback in v1.**
AOT ramps perform NO profile writes; `exit.m_jsValueSource` /
`exit.m_valueProfile` / `m_codeOriginForExitProfile` are EXCLUDED from
EXITMETA item 4 (explicitly, so the omission is a decision, not a gap);
the AOT-D2 dedup key — (pointer-parameterized stream, undo record) — is
unchanged; the C++ recovery interpreter likewise performs no refinement
(remains the testing oracle for frame reconstruction only). Rationale: the
only in-design consumer of exit-observed values was G.6's re-selection
clause; serializing the channel would have added per-exit realm-resolved
descriptors and changed ramp identity for a signal LLInt already produces.
(3) G.6 amendment (supersedes its deselect-to-generic sentence): F.4(5)
deselection now returns the function to the C20 LLInt-RESIDENT
deferred-install state — entry uninstalled, symbolic slots poisoned, the
shared trigger re-armed (C42 dispatch is per (function × realm) and never
`dontJITAnytimeSoon`, so re-trip is already legal). LLInt metadata
profiling re-observes the failing site within one execution; the selector's
re-trip reads FRESH profiles — the re-selection evidence channel is now a
shipped mechanism, not a phantom. One re-trip permitted (G.6's single
re-selection unchanged in count); on second deselection the selector
installs the GENERIC body sticky (no further LLInt residence; generic's
facts were bind-validated). Cost recorded: a second C20 warm-up window
(threshold × LLInt-vs-generic gap) on the deselect path — paid only by
functions that selected wrongly once, bounded by the single-re-trip rule.
§1.4 step 0 restated accordingly; demotion still counts generic-body exits
only. Charter L.7(c) exercises the evidence source end-to-end.

### L.3 — C55 code-lifecycle + debugger demotion (full text; amends AOT-D7, AOT-D10, §3.4)

Events: (i) debugger attach / breakpoint toggling —
`Debugger::recompileAllJSFunctions` (`debugger/Debugger.cpp:600`) calls
`VM::deleteAllCode(PreventCollectionAndDeleteAllCode)`: in-tree, breakpoints
work by destroying optimized code so `op_debug` runs in LLInt; Tier-A
bodies contain no `op_debug` and no debugger check. (ii)
`ScriptExecutable::clearCode` (`runtime/ScriptExecutable.cpp:165`;
triggered under memory pressure per the `:508` comment) — clears installed
code during normal jitless operation. Rules (binding): (1) Both events ⇒ a
**demotion sweep** over affected bindings using the C53 machinery verbatim
(clear validity bits store-release + uninstall entries; in-flight frames
exit at their next check) — debugger attach sweeps ALL bindings;
clearCode sweeps the executable's bindings. A breakpoint set in a bound
function therefore hits on the NEXT CALL (it runs LLInt), at the price of
demotion — the in-tree price exactly. (2) Lifetime: the per-realm binding
table holds STRONG refs to both CodeBlocks (LLInt + AOT); the validity
bit's home (the AOT CodeBlock side table) and EXITMETA targets survive any
`clearCode` of the executable's installed-code slot; AOT-D10's "permanent"
is thus scoped to the binding table's ownership, and the jettison/clearCode
paths see the binding as demote-then-release. (3) AOT-D7 amendment —
lifecycle carve-out: a demotion caused by debugger attach or `clearCode`
(NOT by precondition-class watchpoint death — those facts are dead and stay
dead) MAY re-bind lazily through the SAME §3.3 first-link path after the
debugger detaches / the executable re-materializes; AOT-D7's "v1-run only
at load" reads as "only the §3.3 path, which is lazy per function" — no new
machinery, no runtime re-validation loop (re-bind = full re-validation by
construction). (4) Recorded cost: while a debugger is attached the app runs
at jitless speed (§5 row); detach restores bindings lazily. Charter L.7(b).

### L.4 — C56 design-freeze precondition + remediation order (full text; amends §4.2, AOT-I10)

Rule (binding): the AOT-I10 milestone measurement — a jitless
-Os/LTO/`--gc-sections`/strip build of THIS tree, `ENABLE(AOT_RUNTIME)`
floor approximated by NOT compiling out `CallLinkInfo`/`PropertyInlineCache`
(per C51 rider 1), re-bucketed per survey §3.2 incl. a non-text
(`.eh_frame`/`.data*`/`.rodata`) line — is a **DESIGN-FREEZE
PRECONDITION**: implementation work beyond the measurement itself does not
start until the measured Bun-flavor projection is < 10 MB. Rationale: one
unmeasured number (the -Os/LTO factor) decides a hard constraint; the
measurement is cheap (existing build infra; days, not weeks) relative to
everything it gates. Remediation order if the measured factor lands
conservative (gap ≈ 0.8-1.5 MB per L.5), restricted to levers VALID for the
gated Bun flavor (C50 forecloses inspector-off; C51 is a floor; wasm-off is
invalid — Bun ships WebAssembly): (1) 9.49-bucket sub-cuts: dev/dump/
verbose/disassembler-adjacent surface, `dataLog` machinery, options
introspection — measured candidates, est. 0.3-0.8 MB at -Os; (2) Yarr +
parser -Os deltas beyond the uniform factor (both are cold-startup paths;
per-TU -Os pragmas); (3) AOT-D20 builtin-artifact list shrink, up to −0.50
(flag-immune budget, still real bytes); (4) `.eh_frame` reduction:
`-fno-asynchronous-unwind-tables` on TUs outside the unwinder's AOT-frame
walk (AOT-D11 readers need tables only for KEEP frames that unwind). If the
ordered levers still leave > 10 MB: the gate FAILS and the design re-opens
at §4 — recorded as the honest end state, not deferred to a milestone
surprise. AOT-I10 text extended accordingly.

### L.5 — C57 KEEP split + restated band (full text; amends §4.1, §4.2)

Split of the former "15.31 MB compressible C++ text": **factor-eligible
C++ text = 13.27 MB** (15.31 − 0.38 offlineasm LLInt blob − 0.30 NEW
allocation − 0.25 retained allocation − 1.11 ICU); **flag-immune +
post-factor = 1.43 MB** (asm blob 0.38 + builtin artifact 0.50 + NEW 0.30 +
retained 0.25 — the allocations are post-factor budget OUTPUTS and were
being multiplied as inputs); **ICU code = 1.11 MB at its own factor ~0.9**
(already size-tuned library code; limited -Os headroom). Restated band,
same endpoint factors as before (optimistic f≈0.42, mid f≈0.55,
conservative f≈0.63 on eligible text): 13.27f + 1.00 + 1.43 ⇒ **≈ 8.0
optimistic / ≈ 9.7 mid / ≈ 10.8 MB conservative** — vs the former 6.9 /
8.9 / 10.1: both ends move UP, the conservative gate violation widens to
≈ 0.8-1 MB (feeds L.4's remediation sizing). Non-text KEEP line (new):
measured -O2 full binary (readelf, Annex C binary): `.eh_frame` 2.28 +
`.eh_frame_hdr` 0.32 + `.data.rel.ro` 0.47 + `.data` 0.08 = **3.15 MB**;
KEEP share ≈ 3.15 × (15.81/23.95 text ratio) ≈ **2.1 MB**, ASSUMPTION:
unwind/data bytes scale with retained text (stated so the milestone can
falsify it); the class-R image GOT (§3.1 row 4, ~736 slots ≈ 6 KB) +
per-function data tables live inside the +0.30 NEW allocation. The §4.2
band remains a TEXT(+artifact) band; the milestone comparison is
whole-binary per bucket incl. the non-text line — apples-to-apples per
L.4. §4.1 KEEP-sum paragraph and §4.2 verdict restated in the main doc;
raw -O2 total corrected ≈ 18.5 → ≈ 19.7 MB (15.81 + 0.7 non-ICU `.rodata`
+ 3.15 measured, KEEP-scaled ⇒ ≈ 18.6; full-binary unscaled bound 19.7 —
the scaled figure is the budget line, the bound is the falsifier).

### L.6 — round-9 main-doc compressions (full-text record)

To fit C53-C57 under the cap, draft 12 compressed: **§1.4 step 0, was:**
"0. **Lattice descent (AOT-D0)** first: an exit from a selected variant
DESELECTS to generic (entry write; slots poisoned) — sound mid-activation
because slots are guard-position-only (C16: resumed frames re-check before
use). One re-selection against a different stable shape permitted; then
generic is \"Tier A\" below. Only generic-body exits feed demotion." (replaced
by the C54 wording, normative full text L.2). **§3.4 first paragraph, was:**
"Runtime invalidation rewrites code at `m_jumpReplacements` sites
(`dfg/DFGCommonData.h:133`). Immutable AOT code instead embeds there a
load+branch of the per-function validity bit; watchpoint fire ⇒ clear bit
(store-release, §6) ⇒ next check funnels into the OSR-exit path; in-flight
activations exit at their next check (~1 load+branch per point)." (extended
with the C53 sentence; normative full text L.1). **§4.1 KEEP-sum and §4.2
verdict paragraphs**: prior text preserved verbatim inside K.5 and the
pre-C57 main doc; superseded numbers recorded in L.5. Additionally,
cite-shortening and "full text/full record X" → "(X)" pointer trims (no
semantic change; normative full text unmoved in the named annexes) were
applied in: §0 non-goals; §0.5 intro; §1.1 (AOT-D15 head); §1.2 (AOT-D10
head + decoupling sentence, overlay-selection head + cites); §1.3 (AOT-D2
head + C19 cite, exit-site dispatch, intro cite ranges); §1.5; §2.1 item 3
(C17 wording — rule unchanged, G.3 normative); §2.2 (AOT-D4 head, channels
1 and 3 — J.4 normative for (b)); §2.3 (AOT-D12 para, AOT-D18 head, ledger
MathIC/lazySlowPath entries); §2.5 (AOT-D20 tail); §3.1 (intro class
definitions; rows 1, 2, 6, 7, 10, 11, 12 cite lists); §3.3 (realms head,
step 2b — E.3 normative, step 4 cross-ref); §3.4 (weak-death para, AOT-D7
sentence); §3.5 (both BUNDLE modes + closing para — G.10/J.6/K.7
normative); §4.1 (allocation-row parentheticals, KEEP-sum per L.5); §4.2
(first para, verdict per L.5); §4.3 (startup + integrity cites); §4.4 head;
§5.1 item 3 head; §5.4 footprint number (6.9-10.1 → 8.0-10.8 per C57); §6
head + item (2) (C53 addition); §7 (AOT-I5 wording, charter items 2, 3,
10). Any disputed wording resolves to this annex + L.1-L.5 and the named
normative annexes.

### L.7 — round-9 charter additions (BINDING; main-doc charter item 10 extension)

(a) **C53:** bind F under a saneChain/ObjectPropertyCondition watchpoint;
with F NOT on stack, fire the watchpoint from unrelated code; the next call
to F executes LLInt (entry uninstalled) — assert no folded fact is consumed
(differential vs oracle on a heap arranged so the stale fold yields a wrong
value, not a crash). Also: measure fire-path latency vs subscriber count
(L.1's revisit trigger). (b) **C55:** set a breakpoint in a bound function
⇒ hit on next call; `clearCode`-then-call ⇒ LLInt, then (post
re-materialization) lazy re-bind succeeds; precondition-dead functions do
NOT re-bind after detach; step through an exit-reconstructed inlined frame
⇒ correct pause locations. (c) **C54:** drive a selected variant to
deselect; verify the function re-enters LLInt-resident state, the counter
re-arms, LLInt profiles re-observe the NEW shape, and the single re-trip
selects it (vs the item-2 oracle); verify EXITMETA contains no value/array
profile descriptors (schema assert).

## Annex M — Round-10 external verification record (2026-06-10, BINDING)

4 external findings (1 informational), all tree-fact claims independently
re-verified against this tree before acceptance. **3 accepted, 0 refuted,
1 recorded as verification evidence (M.6).**

| # | Finding (short) | Verified tree facts | Resolution |
|---|---|---|---|
| M-1 | BLOCKER: tool is full-fat jsc, so codegen paths that condition on RUNTIME-DETECTED host CPU features bake the build machine's ISA into shipped code; HEADER validates build hash/format/triple/flavor only — a triple does not encode microarch features. SIGILL on lesser deployment CPUs = UB-class failure with no degrade path; docs never mention CPU features | `assembler/MacroAssemblerX86_64.cpp:508` + `assembler/MacroAssemblerARM64.cpp:615` `collectCPUFeatures()`; `b3/B3LowerMacrosAfterOptimizations.cpp:97,118,139` `supportsFloatingPointRounding()` gates Ceil/Floor/FTrunc lowering; `MacroAssemblerX86_64.h:391-411` LZCNT/BMI1, `:1257-1316` AVX paths; `MacroAssemblerARM64.cpp:660-725` JSCVT via HWCAP/sysctl/IsProcessorFeaturePresent; grep over docs/aot: zero hits for cpuid/JSCVT/AVX/collectCPUFeatures | **ACCEPTED (blocker) — C58/M.1**; §2.4 item 1 + AOT-I8 amended; charter M.5(a) |
| M-2 | C28/H.6 closed well-known-set enumeration materially incomplete vs the tree: set/map iteration, string toString/valueOf coercion, structureCacheCleared, string-symbol/RegExp/Promise protocol sets, and varInjection elision all register through the serialized funnels; fail-safe holds (unknown class ⇒ decline) but the decline blast radius guts Tier-A coverage for ubiquitous speculation classes and the AOT-D20 builtin shells themselves | Full per-global `LinkerIR::Type` watchpoint-set list `dfg/DFGJITCode.h:89-118` (set/map iterator, stringToString/valueOf, structureCacheCleared, 5 string-symbol sets, regExpPrimordialProperties, promiseThen/Species, 3 chain-is-sane); `dfg/DFGGraph.h:970-1003` isWatching* funnels; `dfg/DFGByteCodeParser.cpp:9754` `addLazily(varInjectionWatchpointSet())`, `:6206-6211` consumption elides var-injection checks (+ varReadOnly `:6212`); `:11431` map-iterator addLazily | **ACCEPTED (major) — C59/M.2**; §2.4 item 6 amended; charter M.5(b); AOT-D20 re-audit obligation |
| M-3 | BLOCKER: §4.2 feasibility verdict compares a TEXT-only band against the whole-binary AOT-I10 gate; the design's own non-text KEEP line (~0.7 rodata + ~2.1 KEEP-scaled unwind/data = ~2.8 MB, §4.1/L.5) is omitted from the verdict arithmetic, understating the gap (~1 MB claimed vs 1.6-2.5 MB mid-case) and mis-sizing L.4's remediation order (sized to 0.8-1.5 MB) | L.5's own text: "The §4.2 band remains a TEXT(+artifact) band; the milestone comparison is whole-binary"; AOT-I10 (§7) gates "Product runtime binary"; §4.1 KEEP table carries the 0.7 + 3.15-measured/2.1-scaled non-text line the §4.2 formula (13.27f + 1.0 + 1.43) excludes; readelf re-verified 2.28+0.32+0.47+0.08=3.15 MB; `-fno-exceptions` confirmed `Source/cmake/WebKitCompilerFlags.cmake:201` (makes the `.eh_frame` lever load-bearing: AOT-D11 unwinds via EXITMETA, not `.eh_frame`) | **ACCEPTED (blocker) — C60/M.3**; §4.2 verdict restated whole-binary; L.4 remediation re-sized + reordered (amends L.4 — recorded, L.4 not edited) |
| M-4 | Verification summary: table numbers, exclusion list, ICU, wasm-jitless, startup bounds, artifact format, prior-art honesty re-checked, all pass | Reporter's own spot-checks (text 23.95 MiB; DFG 3.37/B3+Air 1.72/FTL 0.99/Yarr 0.56; 718 `operation*` syms 0.360 MB flat-named; icudt75 30.73 MB; LLIntSlowPaths.cpp:408-539 `#if ENABLE(JIT)` clean; canUseWasm `runtime/Options.cpp:1674` JIT-free; JSToWasm.cpp:548/WasmIPIntPlan.cpp:127 !useJIT fallbacks) | **RECORDED (informational) — M.6**; no edits |

### M.1 — C58 ISA-baseline pinning (full text; amends §2.4 item 1, AOT-I8)

Problem: the offline tool is full-fat jsc (§2.1) running on a build host.
MacroAssembler and B3 lowering decisions condition on host-detected CPU
features (`collectCPUFeatures()` — `assembler/MacroAssemblerX86_64.cpp:508`,
`assembler/MacroAssemblerARM64.cpp:615`; consumed at e.g.
`MacroAssemblerX86_64.h:4577-4690`, AVX paths `:1257+`, LZCNT/BMI1 `:391-411`,
JSCVT `MacroAssemblerARM64.cpp:660-725`,
`b3/B3LowerMacrosAfterOptimizations.cpp:97,118,139`
`supportsFloatingPointRounding()`). Unmodified, the tool bakes the BUILD
HOST's ISA into shipped CODE; the §2.4 HEADER (build hash + format version +
target triple + flavor hash) cannot catch it — a triple does not encode
microarchitectural features and JSC Options carry none. Executing such an
artifact on a lesser CPU is SIGILL/UB with no degrade-to-LLInt path,
violating §3.3 "Never UB" and the AOT-I4 discipline. Decision (binding):

1. **The tool NEVER host-detects.** Tool initialization forces the
   MacroAssembler/B3 feature-state variables (the `s_*CheckState` family and
   x86 feature flags) to a **pinned per-target ISA baseline** declared in the
   target spec (e.g. x86_64-v2-class: no AVX/BMI unless the baseline says so;
   arm64: baseline ARMv8.0, JSCVT off unless pinned). `collectCPUFeatures()`
   is compiled out of the tool's emission path; any feature-conditional
   emission query outside the pinned baseline is an AOT-I5-class tool-time
   hard error (same choke-point discipline: enforced where the feature state
   is READ, not where instructions are emitted).
2. **The baseline is serialized into HEADER** as an explicit feature-bit set
   (not merely folded into the flavor hash — the loader must be able to
   compare bits against the host, not just equality-check a hash; the bit
   set additionally feeds the flavor hash so unrelated-baseline artifacts
   also fail the existing AOT-I8 check first).
3. **Loader CPUID-superset check at HEADER validation:** the product runtime
   computes host features once (the same `collectCPUFeatures()` machinery —
   it is detection-only, ~no bytes) and requires artifact-baseline ⊆ host
   features. Any missing bit ⇒ artifact ignored WHOLESALE, Tier L, reason
   logged (same path as a flavor-hash mismatch; AOT-I8 extended).
4. Charter row M.5(a): an artifact carrying a feature bit the host lacks
   never binds.

Multi-baseline "fat" artifacts (one CODE section per baseline, loader picks
the widest satisfiable) are chartered as future work in Annex B (B.10), not
v1. Rejected alternative: lowest-common-denominator only (no pinning knob) —
costs measurable arithmetic perf (e.g. rounding via call per
B3LowerMacrosAfterOptimizations fallback) on hosts that DO have the
features, and embedders like Bun know their deployment floor.

### M.2 — C59 closed-enumeration extension to the full LinkerIR catalog (full text; amends C28/H.6 as already amended by C40/J.1; amends §2.4 item 6)

H.6's well-known-set class enumerated 6 sets + E1/E2. The tree's own
data-driven catalog is larger: `LinkerIR::Type` (`dfg/DFGJITCode.h:89-118`)
enumerates per-global watchpoint sets for array/set/map iterator protocols,
numberToString, structureCacheCleared, string toString/valueOf coercion,
five string-symbol protocol sets (match/search/replace/split/toPrimitive),
RegExp primordial properties, promiseThen, array/promise species, and three
prototype-chain-is-sane sets — each registered through the exact
`isWatchingGlobalObjectWatchpoint` funnels the serializer walks
(`dfg/DFGGraph.h:940-1003`). Additionally `varInjectionWatchpointSet` /
`varReadOnlyWatchpointSet` register via raw `addLazily`
(`dfg/DFGByteCodeParser.cpp:9754`; consumed at `:6206-6215` to ELIDE
var-injection/read-only checks for sloppy direct-eval scope resolution).
H.6 as written fails SAFE (unknown class ⇒ tool-time decline, never drop) —
soundness was never at risk — but the decline blast radius silently demotes
every function speculating on string coercion, Set/Map iteration, RegExp
protocol fast paths, or eliding var-injection checks: a large fraction of
real bundles, and the AOT-D20 builtin shells themselves (Map/Set iteration
helpers watch exactly these sets). That undercuts §0.5 coverage assumptions
and §4.4 projections. Decision (binding):

1. The closed enumeration is extended to: **the full per-global
   `LinkerIR::Type` watchpoint-set list as of this tree's
   `dfg/DFGJITCode.h:89-118`** (badTime, masquerades, arrayBufferDetach —
   already covered as the view-detach class, array/set/map iterator
   protocols, numberToString, structureCacheCleared, stringToString,
   stringValueOf, the five string-symbol sets, regExpPrimordialProperties,
   promiseThen, arraySpecies, promiseSpecies, the three chain-is-sane sets)
   **plus `varInjectionWatchpointSet` and `varReadOnlyWatchpointSet`**, plus
   this fork's E1/E2 threads sets. Each binds per realm (C19), subscribes
   like the §2.1.4 assumptions, and gets a PRECOND descriptor kind.
2. The closure DISCIPLINE is unchanged (entry mapping to no class ⇒ re-emit
   guarded per C40 or decline, NEVER drop; new engine registrations fail
   loudly). Only the catalog grew.
3. **AOT-D20 re-audit obligation:** the builtin hot-subset list (I.4/C38) is
   re-audited against the extended catalog before the list freezes; any
   shell whose required set is still outside the catalog is removed from
   the list rather than shipped-but-declining.
4. Charter rows M.5(b): (i) sloppy direct eval injects a var post-bind ⇒
   varInjection set fires ⇒ bit clears + entry uninstall (C53 path) before
   control returns to JS, differential proves no stale folded scope read;
   (ii) `String.prototype.toString` patched post-bind at a
   coercion-speculating site ⇒ fires + demotes; pre-bind patch ⇒ never
   binds.

### M.3 — C60 whole-binary verdict restatement + remediation re-sizing (full text; amends §4.2; amends C56/L.4 remediation order — recorded, L.4 not edited)

Defect (accepted): AOT-I10 gates the PRODUCT BINARY (text + rodata +
eh_frame + data, ex-ICU-data), but the §4.2 verdict compared the gate
against the TEXT(+flag-immune+ICU-code) band 13.27f + 1.0 + 1.43, omitting
the design's own §4.1/L.5 non-text KEEP line (~0.7 MB non-ICU `.rodata` +
3.15 MB measured `.eh_frame`/`.eh_frame_hdr`/`.data.rel.ro`/`.data`,
KEEP-scaled ≈ 2.1 MB). L.5 recorded the band/gate units mismatch without
correcting the verdict; L.4's remediation order was sized to the resulting
understated gap (0.8-1.5 MB). This is not a re-litigation of
AOT-D8/C56/C57 — those stand; the verdict sentence and gap sizing were
internally inconsistent with them. Corrected band (whole-binary units,
g = unwind/data scaling factor, ASSUMPTION g∈[f,1] until the C56
milestone measures it):

    13.27×f + 1.00 ICU-code + 1.43 flag-immune + 0.70 rodata + 2.1×g

- optimistic f=0.42: 9.6 (g=f) - 10.8 (g=1) MB — only the best corner passes
- mid f=0.55: **11.6 - 12.5 MB — gap 1.6-2.5 MB** (was claimed "mid passes")
- conservative f=0.63: 12.8 - 13.6 MB — gap 2.8-3.6 MB (was claimed ~1 MB)

Consequence: the C56 design-freeze precondition is UNCHANGED (it would have
caught this at the milestone), but the recorded headline moves from
"FEASIBLE-IFF" to **"gate at risk: only the optimistic corner passes
unremediated"**, and the L.4 remediation order is re-sized and REORDERED to
the corrected gap. The only single lever sized to a 1.6-2.5 MB mid-case gap
is the `.eh_frame` trim, so it is promoted from L.4 position (4) to
position (1): JSC builds `-fno-exceptions`
(`Source/cmake/WebKitCompilerFlags.cmake:201`) and the AOT-D11 product
unwinder walks EXITMETA descriptors, NOT `.eh_frame` — asynchronous unwind
tables on non-unwinder TUs serve only debuggers/profilers of the C++
runtime itself, so `-fno-asynchronous-unwind-tables` (keeping tables on the
loader/binder + crash-reporting TUs) recovers up to ~2 MB of the 2.1
KEEP-scaled unwind bytes. Remaining order unchanged in content: (2)
9.49-bucket dev-surface cuts (0.3-0.8 MB), (3) Yarr/parser per-TU -Os
deltas, (4) AOT-D20 builtin list shrink up to −0.50. If the ordered levers
still leave > 10 MB at the milestone: the gate FAILS and §4 re-opens — the
same honest end state L.4 recorded. §4.2 restated in the main doc in
whole-binary units; the old text-band sentence is superseded (full old text
preserved in L.5/L.6).

### M.4 — round-10 main-doc compressions (full-text record)

To fit C58-C60 under the cap, draft 13 compressed (full text recoverable
here and in the cited normative annexes; L.6 conventions). Wording-only,
no semantic change: **§4.2** verdict paragraph REPLACED wholesale per M.3
(old text in L.5/L.6); lead-in "The shipping config differs only
subtractively (-Os, LTO + `--gc-sections` + strip, backends removed,
`#if !ENABLE(JIT)`, dev surface trimmed). Precedent: ..." tightened;
ICU-stance "Fallback for a standalone claim: small-icu or Intl-off flavor"
→ "Standalone fallback: small-icu/Intl-off". **§2.4** item 6
well-known-set parenthetical now cites the LinkerIR catalog instead of
enumerating six sets inline (extended list = M.2 item 1, normative; the
`dfg/DFGDesiredWatchpoints.h:248,284` cite moved here); item 8 "The
optional CI feed (B.8) extends, never expands required contents" → "CI
feed (B.8) extends, never expands, contents". **§4.1** KEEP-sum
flag-immune parenthetical "(asm 0.38 + artifact 0.50 + NEW 0.30 + retained
0.25)" → "(L.5 split)" (figures normative in L.5). **§0** C2b "The PRIMARY
mode compiles from STATIC analysis of the shipped BUNDLE alone" → "PRIMARY
mode = STATIC analysis of the shipped BUNDLE alone". **§1.1** "No runtime
codegen of any kind" → "No runtime codegen". **§1.5** "parsed and run on
Tier L as jitless JSC today" → "parsed and run on Tier L". **§2.5**
AOT-D14 head "Default = compile-everything-reachable: every function in
the bundle's static call graph gets Tier-A code (no profiles ⇒ no hotness
oracle)" → "Default = compile-everything-reachable (no profiles ⇒ no
hotness oracle)" — the dropped clause is the definition of "reachable",
restated here as NORMATIVE: every function in the bundle's static call
graph gets Tier-A code; AOT-D20 head "The JS builtin corpus
(`s_JSCCombinedCode`, 152 KB text, survey §3.3) is not bundle code;
unhandled it is LLInt forever" → "The builtin corpus (... 152 KB ...) is
not bundle code; unhandled ⇒ LLInt forever". **§2.6** tail "never
correctness risks (AOT-I3) ... the optional CI feed (B.8) restores" →
"never correctness ... CI feed (B.8) restores". **§4.3** startup budget
"≤ 5 µs/function amortized, ≤ 10 ms for a 2,000-function reference app;
atom interning is the only eager bulk cost" → "≤5 µs/function amortized,
≤10 ms / 2,000-function reference app; atom interning the only eager bulk
cost". **§5.4** footprint row restated per C60 ("us ~8.0-10.8 MB
ex-ICU-data (§4.2, unproven)" → "us 9.6-13.6 MB whole-binary
pre-remediation (§4.2/C60)"). **§7** AOT-I8 sentence shortened (full rule
= original AOT-I8 text + C58/M.1 item 3); charter-10 round-10 rows
abbreviated (full text = M.5). Any disputed wording resolves to this
annex.

### M.5 — round-10 charter additions (BINDING; main-doc charter item 10 extension)

(a) **C58:** build an artifact against a baseline with one feature bit the
test host lacks (synthesized HEADER acceptable) ⇒ loader ignores it
wholesale, Tier L, reason logged; nothing binds; differential passes on
Tier L. Conversely baseline ⊆ host ⇒ binds. CI matrix includes one
sub-baseline emulated host per target. (b) **C59:** (i) sloppy direct eval
injects a shadowing var post-bind ⇒ varInjection fires ⇒ bit clears +
entry uninstall before return to JS; oracle differential on a heap where a
stale folded global/scope read yields a WRONG VALUE, not a crash; (ii)
`String.prototype.toString` patched post-bind at a coercion-speculating
bound site ⇒ set fires + demotes; same patch pre-bind ⇒ function never
binds; (iii) Map/Set iterator protocol perturbed post-bind under an
AOT-D20 precompiled builtin shell ⇒ shell demotes to LLInt builtin, no
stale protocol assumption consumed.

### M.6 — round-10 verification-evidence record (informational)

External reporter independently re-derived: `.text` 23.95 MiB; nm buckets
DFG 3.37 / B3+Air 1.72 / FTL 0.99 / Yarr 0.56 MB; 718 flat-named extern-C
`operation*` symbols = 0.360 MB (confirming no double-count against the
dropped `JSC::DFG::` bucket); `icudt75_dat` 30.73 MB (AOT-D8);
`llint/LLIntSlowPaths.cpp` + `runtime/CommonSlowPaths.cpp` contain zero
`DFG::` references; tier-up sits under `#if ENABLE(JIT)` with non-JIT
`#else` branches (`LLIntSlowPaths.cpp:408-539`) — consistent with C42;
wasm-under-jitless KEEP: `canUseWasm()` (`runtime/Options.cpp:1674`) has no
JIT requirement, `wasm/js/JSToWasm.cpp:548` + `wasm/WasmIPIntPlan.cpp:127`
carry explicit `!Options::useJIT()` fallbacks; §4.3 startup gates, AOT-I8/
AOT-I9 artifact format, iOS LINKED/C48/C52 story, §5.4 prior-art honesty
all re-checked and pass. Minor count drift vs H.8 (718 syms/0.360 MB vs
736/0.362) is measurement-method noise, below budget granularity. No
action; recorded so silence is not read as unchecked.

## Annex N — Round-11 external verification record (2026-06-10, BINDING)

Four external findings (1 blocker, 3 major). Each tree-fact claim was
independently re-verified against this tree before acceptance. Disposition:
**4/4 accepted, 0 refuted**. Main doc: draft 14; new rules C61-C63 + decision
AOT-D21; compressions N.4.

| # | Finding (short) | Verified tree facts | Resolution |
|---|---|---|---|
| N-1 | BLOCKER: channel-2 synthesized Status objects have no ObjectPropertyConditionSet emission rule; an empty set passes the parser vacuously, registers nothing in DesiredWatchpoints, and a runtime-installed prototype setter is silently bypassed — UNGUARDED wrong result (violates AOT-I1/AOT-I3); channel 1 is sound because computeFromLLInt re-derives sets through the in-tree generators | `PutByStatus::computeFromLLInt` builds the transition variant ONLY after `generateConditionsForPropertySetterMissConcurrently` succeeds, invalid ⇒ NoInformation (`bytecode/PutByStatus.cpp:111-120`); GetBy proto-loads same family (`bytecode/GetByStatus.cpp:521` `generateConditionsForPrototypePropertyHitConcurrently`); the parser merely consumes: `check(variant.conditionSet())` iterates — empty set passes vacuously (`dfg/DFGByteCodeParser.cpp:5460,5527,6362`); generator family declared `bytecode/ObjectPropertyConditionSet.h:167-183`; grep for conditionSet/PutByStatus/generateConditions across docs/aot/ pre-round: zero hits; E.1 channel-2 text constrained only pathability (cells), not condition sufficiency | **ACCEPTED (blocker) — C61/N.1**; §2.2 channel 2 rewritten; §2.6(5); charter item 11 |
| N-2 | AOT-D4 funnel list never states WHICH Status kinds channel 2 may feed; "~3 sites" is get/call-only while §2.1.1 constructor-shape facts are put-side — the legal funnel set and its soundness/disclosure obligations are indeterminable | Funnels verified: CallLink `dfg/DFGByteCodeParser.cpp:1463,1529`; GetBy `:7483`; PutBy `op_put_by_id` `:8950` (more PutBy/InBy/DeleteBy/InstanceOf computeFor sites exist: `:8799,:9035,:10357,:8301,:11042` — the closed list deliberately excludes them) | **ACCEPTED (major) — folded into C61/N.1** (closed funnel list + §2.6(5) disclosure) |
| N-3 | §4.2 remediation lever 1 quoted the UNSCALED whole-binary .eh_frame measurement (~2 MB) against a SCALED gap — bytes already removed by -Os/LTO/--gc-sections cannot be trimmed again; lever is ≈1.7g MB (≈0.95 at mid g≈0.55), and .data.rel.ro inside the 2.1g line is untouchable; "only gap-sized lever" overstated headroom ~2x at the band's own central f | readelf re-verified: .eh_frame 0x2476e0 = 2.28 MiB, .eh_frame_hdr 0.32, .data.rel.ro 0.47, .data 0.08 ⇒ 3.15 measured (matches M-3 row); eh_frame(+hdr) share = 2.60/3.15 = 0.825; trimmable ≈ 0.825 × 2.1g ≈ 1.7g MiB; mid g=0.55 ⇒ ≈0.95 MB, not 2 | **ACCEPTED (major) — C62/N.2**; §4.2 lever restated scaled; M.3 amended (recorded here, M.3 not edited) |
| N-4 | RegExp silently demoted to Yarr-interpreter-class forever; absent from §5.1 losses, §5.2 thesis, §5.4 footprint — no recorded decision despite regex being hot-path for Bun's server market and the fix fitting the design's own emitter seam | Only Yarr matches in AOT-DESIGN.md pre-round: §4.1 size row + §4.2 -Os lever; YarrJIT output is LinkBuffer-finalized (`yarr/YarrJIT.cpp:32` include; `:7205,7212` addLinkTask finalize), i.e. the same seam AOTObjectLinkBuffer exploits (§2.3); regexp literals statically visible at bytecode level | **ACCEPTED (major) — AOT-D21 + C63/N.3**; §5.1(7); §5.2 thesis scoped |

### N.1 — C61 channel-2 condition-set emission rule + closed funnel list (full text; amends AOT-D4/E.1, §2.2, §2.6; charter item 11)

(1) **Closed funnel list.** Channel 2 (the tool-only static-fact provider)
may synthesize exactly three Status kinds, at exactly these consumption
funnels: `CallLinkStatus` (`dfg/DFGByteCodeParser.cpp:1463,1529`),
`GetByStatus` (`:7483`), `PutByStatus` (`op_put_by_id`, `:8950`). Every
other Status kind (`InByStatus`, `DeleteByStatus`, `InstanceOfStatus`,
`CheckPrivateBrandStatus`, and any kind added later) is **channel-1-only**:
seedable through LLInt metadata where slots exist, otherwise the AOT-D16
generic floor. Recorded §2.6(5) loss: set-valued facts for the excluded
kinds degrade to monomorphic-or-generic — the disclosure E.1 demanded of
alternative (b), now scoped to the kinds where we actually take that trade.
PutBy is IN the list because §2.1.1 constructor-shape facts are put-side;
the other `PutByStatus::computeFor` parser sites (`:8799,:10673,:11042`,
by-val/enumerator forms) are NOT provider-fed in v1 — channel-1/generic.

(2) **Condition-set emission rule.** A channel-2 synthesized variant is
legal ONLY if its `ObjectPropertyConditionSet` was produced by the in-tree
condition generators (`generateConditionsForPropertySetterMiss[Concurrently]`
/ `...PropertyMiss[Concurrently]` / `...PrototypePropertyHit[Concurrently]`
family, `bytecode/ObjectPropertyConditionSet.h:167-183`) executed against
the recipe-built stand-in heap (AOT-D18) — the exact step in-tree Status
computation never skips (`bytecode/PutByStatus.cpp:111-120`,
`bytecode/GetByStatus.cpp:521`). Generator failure (invalid set) ⇒ the
variant is NOT emitted; the site compiles generic-but-direct (AOT-D16
floor). An EMPTY conditionSet is legal only where in-tree computeFor would
produce one (pure self-access on the guarded structure; direct puts).

(3) **PRECOND flow.** Every synthesized variant's conditionSet reaches the
parser's `check(variant.conditionSet())` walk
(`dfg/DFGByteCodeParser.cpp:5460,5527,6362`), lands in DesiredWatchpoints,
and serializes under the C28/C40 closed rule into PRECOND; §3.3 step 3
revalidates each condition against the LIVE heap at bind — so a wrong
analyzer prototype-chain model fails CLOSED (function never binds), and a
post-bind interposition fires the adaptive watchpoint (§3.4). This restores
the channel-1 symmetry the finding identified: channel 1 is sound because
computeFromLLInt runs the generators; channel 2 is now sound for the same
reason, by rule rather than by accident.

(4) **Charter (main-doc item 11).** (i) `Object.defineProperty(C.prototype,
'x', {set})` at module-eval time over an inferred constructor shape ⇒ the
function never binds (absence condition fails §3.3.3) or the emitted guard
exits — the setter is ALWAYS invoked; differential vs JIT oracle. (ii) Same
patch post-bind ⇒ watchpoint fires, bit clears, in-flight exits. (iii) Fuzz
lens mutating prototype chains under every C61-synthesizable Status kind.

Rationale: this is the C47/K.2 finding-shape — an implicit soundness chain
(Status computation = condition generation) became explicit when the design
replaced the computation. It met the M-1 blocker standard: the one found
path where "every failure degrades to LLInt" did not hold.

### N.2 — C62 scaled remediation lever (full text; amends §4.2 lever 1; amends C60/M.3 — recorded, M.3 not edited)

Defect (accepted): M.3 correctly scaled the 3.15 MiB measured unwind/data
line by g into the band (2.1g) but quoted lever (1) UNSCALED ("~2 MB, only
gap-sized lever") from the whole-binary .eh_frame measurement. Bytes that
-Os/LTO/--gc-sections already removed with their dropped functions cannot
be trimmed a second time. Corrected sizing: trimmable = eh_frame(+hdr)
share of the surviving line ≈ (2.60/3.15) × 2.1g ≈ **1.7g MiB** — 0.95 MB
at mid g≈f=0.55, up to 1.7 MB at g=1. `.data.rel.ro` (0.47 measured;
relocations) and any retained unwinder-TU tables survive the lever.
Consequence at mid f=0.55: levers (1)-(4) sum ≈ 0.95 + 0.55 + small + 0.50
≈ 2.0-2.2 MB against a gap of up to 2.5 MB — **the §4-re-opens branch is
the EXPECTED outcome unless the C56 milestone measurement lands favorable**.
This is an internal-arithmetic amendment only: the C56 freeze precondition,
the C60 band, and the escape hatch all stand unchanged.

### N.3 — C63/AOT-D21 RegExp execution tier (full text; new decision; amends §5.1, §5.2)

Defect (accepted): the product ships only the Yarr interpreter (§4.1 KEEP
0.28, YarrJIT dropped — correct under AOT-I2), but no honesty section
disclosed the consequence and no decision declined the fixable case.
Frozen-spec conventions require the feature or a recorded decision; neither
existed.

**AOT-D21 (decision):** v1 DECLINES AOT compilation of bundle-literal
regexps; ALL regexp execution (dynamic and literal) runs the Yarr
interpreter. Recorded as §5.1 loss (7) and scoped out of the §5.2
"JIT-class hot loops" thesis. Rationale for declining in v1: (a) the
artifact CODE/RELOC/table machinery is chartered around DFG/FTL function
bodies; a regex pool adds a second code-section producer (YarrJIT over
`AOTObjectLinkBuffer` — feasible: YarrJIT output is LinkBuffer-finalized,
`yarr/YarrJIT.cpp:7205-7212`, position-independent at the same seam §2.3
exploits) plus its own per-immediate audit (E.2-class) and per-arch matrix
— estimated +1 section kind, +0.1-0.3 MB artifact for literal-heavy
bundles, and a NEW audit surface, against a v1 schedule already carrying
C56 at-risk size work; (b) correctness is unaffected (AOT-I1: interpreter
is full-semantics); the loss is perf-only and demotion-free. **v2 charter:**
precompiled literal-regex pool as artifact CODE — literals are statically
visible at bytecode level, compiled per flavor/ISA baseline (C58), bound by
class-M/R table discipline like the AOT-D20 builtin artifact; dynamic
regexps stay interpreter-class forever (C1 analog of §1.5). An
embedded/server developer comparing vs YarrJIT'd Bun today must read
§5.1(7) as a real regression until v2 lands.

### N.4 — round-11 main-doc compressions (full-text record)

To fit C61-C63 under the cap, draft 14 compressed (wording-only; binding
content unchanged; originals recoverable here). Substantive originals:

- §0.5 epitaphs: "*Pure ARCH-S* — refuses a profiling signal the product
  pays for anyway; the 2-5× generic gap on silent-hot code becomes
  permanent. *Pure ARCH-L* — V≥3 floor everywhere (25-30 MB-class app
  `.text`); universal LLInt warm-up (C20); ..." (now "refuses free
  profiling" etc.; same decisions).
- AOT-D10: "old-age jettison disabled ... demotion = clear bit + uninstall
  entry, **never** free image memory; weak-reference jettison (`:1478`)
  replaced by the §3.4 sweep. Needed DFG data structures decouple ...
  compilers/assembler/allocator stay out."
- §1.3: refinement-block parenthetical "(— per-realm profile addresses)";
  FTL branch "audited + MODIFIED, dedup over that stream"; AOT-D12's
  in-place AOT-I5 enforcement cites (`dfg/DFGJITCompiler.h:261`,
  `ftl/FTLLowerDFGToB3.cpp:28154`) — now only in §7 AOT-I5, unchanged
  there.
- §1.4 intro "(per-call bad luck vs 'the statics are wrong')"; step 0
  "slots guard-position-only (C16) ... (ramps write NO profiles — C54) ...
  = 'Tier A' below" (rule text intact via C16/C54 cites).
- §2.5 AOT-D20 motivation clause "— iteration/Promise shells interpreting
  around FTL-grade callbacks" and rejection detail "(full corpus 1.5-3 MB;
  status quo loses builtin-heavy code)" — both remain in I.4.
- §3.4 C53: rationale clause "checks (jump-replacement positions) need not
  dominate a fresh call's first folded-fact use" → "(rationale L.1)" (full
  text L.1). C55: "Tier-A has no `op_debug`", "(not precondition-class)",
  "(recorded cost)" — full text L.3.
- §3.5 BUNDLE: "— unauthenticated executable mapping = arbitrary code
  execution" → "(G.10)" (full text G.10).
- §3.1 row 12: fencing/stack-limit/traps cites (`jit/AssemblyHelpers.h:2159,
  :195`, `dfg/DFGSpeculativeJIT.cpp:2581`) → "(I.2 list)" (full cites I.2).
- §7 AOT-I2 symbol list → "C35/C37 forbidden list" (full list I.1/I.3);
  charter items 3 and 10 row prose → pointers (full rows I.6+J.1, K.9, L.7,
  M.5); charter 2 "violated"; charter 9 "no poison consumption".
- Minor connective trims throughout (vs/against, out/compiled out,
  "(addons, `node:`/`bun:`, externals)").

— end Annex N.

## Annex O — Round-12 fresh-eyes finalization record (2026-06-10, BINDING)

Reviewer stance: no stake in rounds 1-11; read AOT-DESIGN.md + annexes
end-to-end as a skeptical implementer ("could I start building the offline
compiler and the product runtime, or where would I guess?").

### O.1 — Implementer gaps found and fixed (main doc edited directly)

1. **§1.4 thresholds D/S were free variables.** The post-exit policy said
   "while `aotDemotionScore < D`" and "`aotExitCounts[i] > S`" with no
   values anywhere in doc or annexes — the first thing a runtime implementer
   would have had to invent. FIXED: D, S and the decay schedule are JSC
   `Options` (so CI/embedders tune without a spec change), with RECORDED
   initial defaults: **D=8** (a function gets eight diffuse-exit strikes
   before permanent demotion — same order as the in-tree reoptimization
   retry ladder's early rungs), **S=128** (a single site absorbing >128
   exits is per-call-polymorphic at any plausible call volume), **decay =
   halve `aotDemotionScore` per full GC cycle** (cheap, already a global
   epoch; mirrors `CodeBlock` exit-count cool-down intent). These are
   declared starting points subject to the §7 charter-7 CI budgets, not
   measured truths — but they are now recorded decisions, not guesses.
2. **Missing `## 5. Performance (honest)` header.** Draft 14 jumped from
   §4.4 directly to `### 5.1` (a casualty of an earlier compression);
   §5.x cross-references resolved but the section had no title. FIXED.
3. **No executive summary.** Added at top (10 content lines): execution
   model, reuse, size verdict, honest costs. Normative text unchanged —
   the summary cites §0.5/§2.3/§4.2/§2.6/§5.1 and adds no new claims.
4. Buildability verdict otherwise: the offline-compiler side is
   implementable from §2 + Annexes D-N (seams are file:line-pinned:
   FuzzerAgent override `dfg/DFGByteCodeParser.cpp:1001`, Status funnels
   `:1463/:7483/:8950`, `AOTObjectLinkBuffer` at the `LinkBuffer` seam,
   AOT-D12 lowering rule, AOT-D18 stand-in factory). The product-runtime
   side is implementable from §1/§3 + AOT-I1-I13 (binding order AOT-I4 is
   explicit; selector protocol §1.2 + AOT-I13; sweep AOT-I11). Remaining
   open numerics are all DECLARED as measure-gates (f/g factors C60,
   artifact expansion factor §4.4, builtin list I.4) rather than silent
   unknowns. No further undefined symbols found.

### O.2 — Compose check vs docs/threads/SPEC-{jit,ungil}.md (§6)

Checked AOT §6 against SPEC-jit rev 12 (+annex) and SPEC-ungil rev 35.
**No contradiction found.** Three points made explicit as §6 item (6):

- **jit I2/I8 (machine code modified only world-stopped):** AOT's product
  has NO jump replacements and NO repatching — invalidation = validity-bit
  data write + entry uninstall (§3.4/C53). I2/I8 hold vacuously; AOT is
  strictly WEAKER than what SPEC-jit already permits. The store-release +
  Class-A-fire-under-STW discipline in §6(2) matches jit §5.6/I10 exactly.
- **jit I21 (flag on: polling traps only; every DFG/FTL poll immediately
  followed by an invalidation point, so parked mutators resume into the
  patched exit, never across invalidated code):** AOT bodies are produced
  by the same DFG/FTL pipeline, which plants InvalidationPoints after
  CheckTraps; AOT lowers each InvalidationPoint to the §3.4 validity
  check. The adjacency therefore survives offline compilation by
  construction, and a parked-then-resumed mutator hits the validity check
  before any code whose preconditions died. `usePollingTraps` forced
  (jit M2b) composes with the jitless product trivially (LLInt already
  polls; AOT code keeps its compiled CheckTraps polls).
- **SPEC-ungil:** its stop protocol (§A.3), entry tokens (§F) and U0
  config gates are tier-agnostic; AOT's Tier A behaves as "DFG/FTL code
  that is never recompiled" — every ungil obligation on JIT'd code is an
  obligation on jettison/patching/IC-publish machinery AOT does not ship.
  The AOT-I8 flavor hash must cover `useJSThreads` AND `useThreadGIL`
  (both change codegen-relevant invariants); §6(1) already pins the former
  and the option-flavor hash (§2.4 HEADER) covers the full Options set.
  N5 stands: the joint design (selector/binder as stop participants,
  jit-R1-style conductor for C53 sweeps) remains chartered, not solved.

### O.3 — Size-table re-verification (2026-06-10, post-survey binary)

`WebKitBuild/Release/bin/jsc` was rebuilt (threads work) AFTER the survey
measurements. Re-measured this round (`size -A`, `nm -S`):

| Quantity | Survey/§4 value | Re-measured | Verdict |
|---|---|---|---|
| `.text` | 23.62 MiB | 25,116,946 B = 23.95 MiB | +1.4% drift (threads work), in noise |
| `.rodata` | 31.37 MiB | 32,891,744 B = 31.37 MiB | exact |
| `icudt75_dat` | 30.73 MB | 30,729,184 B | exact |
| eh_frame(+hdr)+data(.rel.ro) | 3.15 MiB | 3.14 MiB | exact |
| `operation*` text symbols | ~736 / ~0.36 MB | 801 / 0.39 MB | drift, same class |

Conclusions unchanged: KEEP sum, the C57/C60 band arithmetic, and the
AT-RISK verdict all carry; the +0.33 MiB `.text` drift is inside the
"overcount in our favor" caveat (jitless compile removes the very
`ENABLE(JIT)` code the threads branch is currently growing). The C56
design-freeze measurement (jitless -Os/LTO build) remains the binding
number; this row is provenance only.

### O.4 — Full text displaced by round-12 compressions (BINDING here)

- §0 Non-goals, draft-14 wording: "N1 — not startup-only; the point is
  shipping *optimized machine code*, no JIT. N2 — not closed-world
  (GraalVM): the bundle is analyzed whole, but `eval`/dynamic loading stay
  legal — on Tier L. N3 — not 100% AOT coverage: dynamic/declined code
  runs LLInt forever. N4 — not a wasm design (product wasm = IPInt +
  runtime, §4). N5 — threads/AOT joint design chartered, not solved (§6)."
- §1.5: "Functions *called by* dynamic code keep their Tier-A bindings;
  `eval` inside an AOT function is handled as DFG does today
  (`op_call_eval` slow paths)."
- §2.1(3): proven-callee claim long form: "'this site only sees F' is
  provable for large fractions of real code (Hopc, survey §4)".
- §2.2 C61: "— the step computeFor never skips" (now "computeFor parity").
- §2.5 AOT-D20 tail: "(I.4; rejected alternatives there.)".
- §3.2 AOT-D6: "(snapshot startup-semantics tax, A; opt-in later)".
- §5.2/§5.3: wording-only tightenings; claims identical.
- §7 charter 3/10/11: row lists compressed to correction-ID pointers; the
  full row texts remain BINDING at I.6+J.1, K.9+L.7+M.5, and N.1.

### O.5 — §2.3 reuse-ledger MODIFIED row, draft-14 full decorations

"`DFGByteCodeParser` (tool-only — AOT-D16 seam + C61 closed funnel list
AOT-D4.2); DFG/FTL lowering (tool-only — AOT-D12); `Graph::freeze`
(AOT-D13); `OSRExit::compileExit` (parameter blocks — AOT-D2); FTL
`compileStub` (ExitValue stream — C22); FTL `Output::operation`
(GOT-routed — C21); `jit/JITOperations.cpp`/`dfg/DFGOperations.cpp`/
`jit/Repatch.cpp` (AOT_RUNTIME op set; `repatch*` → data-IC writes;
tier-up ops dropped; nm-audited AOT-I2); `DFGJITCompiler::linkOSRExits` +
FTL OSRExitHandle dispatch (tool-only — C37/I.3); LLInt
`replace`/`loop_osr` slow paths (selector dispatch + re-arm — C42/J.3);
FTL `lazySlowPath` patchpoints (C35/I.1: eager tool-time stubs into CODE,
E.2-audited, `ftl/FTLLazySlowPath.cpp:57`; compile op + thunk dropped);
`ENABLE(AOT_RUNTIME)` decoupling (sized §4.1; piece list D; incl. C17
watchpoint-gating audit)." The main-doc row now carries the same entries
with pointer-form decorations; semantics unchanged.

### O.6 — Round-12 spot-check of load-bearing cites (all pass)

`jit/ExecutableAllocator.cpp:160` (RELEASE_ASSERT(!Options::useJIT())),
`llint/LLIntEntrypoint.cpp:60`, `dfg/DFGOSRExitCompilerCommon.cpp:459-463`
(`forceOSRExitToLLInt` leg), `dfg/DFGByteCodeParser.cpp:1052`
(getPrediction), `llint/LowLevelInterpreter.asm:1830` (checkSwitchToJIT),
`runtime/StructureID.h:38`, `runtime/OptionsList.h:643`
(useHandlerICInFTL) — all verified against the tree this round.

— end Annex O.
