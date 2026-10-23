# R2. The AOT IR, its proofs, its effect model, and every optimization that assumes "no other code runs between these two points"

All engine paths are relative to `JSC/` = `Source/JavaScriptCore/` (head 5c2923e566cb).
Bun paths are relative to `BUN/` = `<bun>/`.
Everything below was read in the sources. What I did not read is listed in section 13.

Contents

1. The one-paragraph answer
2. IR forms, stages, schedules (question 9)
3. Proofs (question 1)
4. The effect model (question 2)
5. Passes that remove, move, merge or reorder memory accesses and tests (question 3)
6. Which broken facts are only stale values, which break memory safety (question 4)
7. The generic version and the entry checks (question 5)
8. Polls, safepoints, stack checks (question 6)
9. What B3 and Air are told, fences, Atomics (question 7)
10. How to add an operation, a proof, a heap, an effect flag, a pass, an option, a diagnostic (question 8)
11. Hazards with several mutators
12. What helps
13. Not found / not sure

---

## 1. The one-paragraph answer

The IR has no notion of another thread. "Something can undo it" is computed only from the effect sets of the operations
of *this* function that stand between a test and its use: a proof that "holds for a stretch" depends on the memory locations
that its test declares as read, and it ends at the first operation whose declared or summarised *writes* overlap those locations
(and may be about the same object), or that suspends. Between two operations with no such write, nothing is assumed to happen.
The same effect sets drive redundant read elimination, hoisting, carrying variables around loops (with delayed stores),
barrier removal, and they are translated one to one into B3 heap ranges, so B3 repeats the same reasoning (load CSE, store-to-load
forwarding, LICM, loop versioning with clamped bounds). A poll (`LoopHint`) is not a point where anything of the program changes.
Three design properties limit the damage: the shape word of a compiled object never changes, every slot is one aligned word that
carries its own "absent / undefined / null" markers, and type tests are made on SSA values that were already loaded, not on memory.
The dangerous part is ordinary JSC storage reached from compiled code (butterflies: `ArrayStorage`, `InBounds`, `Pristine`, dense
arrays), the per-cell "birth" flag that vouches for raw slot values, out-of-line slot storage, and the order-dependent results of
flow (`StoredBeforeEveryRead`, `AtMostOnce`).

---

## 2. IR forms, stages, schedules (question 9)

### 2.1 The four forms

`aot/ir/AOTSchema.def:4`

```
enum IRForm { | engine AsRead Prepared Selected Placed }
```

Each operation row gives a range of forms (from its `family X First..Last` header or its own `First..Last`), stored as a bit set
(`aot/ir/AOTIRForm.h:15-30`), asked with `isLegalIn(opcode, form)` (`aot/ir/AOTOpcode.h:100-101`).

| Form | Reached when | What it may contain |
| --- | --- | --- |
| `AsRead` | what the reader decodes from Bun's bytes | the language level operations of `operations.def`, `language-operations.def`, `intrinsic-operations.def`: `PropGet`, `Call`, `Check`, `Convert`, `IteratorOpen`, `Await` .. |
| `Prepared` | behind `ApplyFlowResults` (`producing(Prepared, ..)`, `aot/pipeline/AOTPipeline.cpp:144`; `advanceTo(IRForm::Prepared, ..)`, `aot/pipeline/AOTPipelineStages.cpp:308`) | plus `prepared-operations.def` and `typed-data-operations.def`: guards (`ShapeTest`, `IsSlotFilled` ..), slot accesses (`GetSlot`, `PutSlot` ..), element accesses, `CallBuiltin`, saved frames, string builders |
| `Selected` | behind the function pass `Select` (`AOTPipeline.cpp:245`, `AOTPipelineStages.cpp:411/437`) | plus `selected-operations.def`: fused accesses (`PropGetChain`, `CallMethod`), `PutSlots`, `CallInline`/`TryCallInline`, `TestAndBranch`, operator forms |
| `Placed` | behind `NumberAndEncode` (`AOTPipeline.cpp:181`, `AOTPipelineStages.cpp:461`) | plus `placed-operations.def`: `Box`, `Unbox`, `IntegerToNumber`, `NumberToInteger`, `WidenInteger`, `NarrowInteger`, `Decode`, `Compress`; payload fields marked `from Placed` (`fieldAccess`, `siteCache`, `guardBit`, `orderWord`, `allocation` ..) are filled |

An operation that ends before `Placed` has to name the pass that removes it (`removedBy =`; generator check
`aot/ir/generator/check-entries.ts:330`). Only operations legal in `Placed` get a code generator case
(`aot/ir/generator/emit-cpp-dispatch.ts:18-30`).

### 2.2 The order of the stages

`Pipeline::runAllStages()`, `aot/pipeline/AOTPipelineStages.cpp:487-528`. P = program pass, S = function schedule.

| Stage | Steps in order | Lines |
| --- | --- | --- |
| Read | P `AllocateTaggedLayouts`, P `CheckTypesAtEntry`, `markEnforcedFields`, S `Type`, P `NarrowFieldTypes`, P `NameCreatedSymbols`, verify(AsRead) | 287-299 |
| Flow | P `GuardTaintedPositions`, P `AnalyzeFlow`, P `ApplyFlowResults` (-> Prepared), collect remaining checks and callees of sites, P `CheckLockedBuiltinWrites` | 301-314 |
| Optimize | P `PromoteEnvironmentsToVariables`, P `RemoveUnreadExports`, P `DeferDefinitions`, P `SplitBehindEntryChecks`, S `Prepare`, P `NarrowConstantVariables`, P `DeferInitializers`, P `PromoteAggregateParameters`, P `SplitFreshResults`, P `SplitCoroutines`, P `CallModules`, P `CreateEntrypoints`, S `Resolve`, S `Optimize`, P `ReadCapturesFromEnvironments`, P `RemoveUnreadFields`, P `RemoveUnusedResults`, P `RemoveUnusedParameters`, P `FlattenInnerObjects`, P `SplitFreshResults`, P `SkipAnsweredEntryChecks`, P `RemoveUnusedResults`, P `MarkResultsUsedAsKeys`, P `EmbedConstantCollections`, P `RemoveUnreachableFunctions`, P `ChooseCallingConventions`, P `DeferClosuresInFields`, P `AllocateInFrames`, P `AllocateInnerObjects`, P `ReuseEnvironments` | 316-366 |
| Slots | P `NumberLiteralStrings`, P `PlanSlots` | 368-378 |
| Select | S `Select` (-> Selected), observe access sites, P `OutlineColdOperations`, P `InlineHeads`, P `RemoveUnreachableFunctions`, P `ElideStackChecks`, P `CollectRegExpLiterals` | 380-449 |
| Placement | P `NumberAndEncode` (-> Placed), P `PlaceSiteCaches`, P `CheckPlacement` (when verifying) | 451-465 |
| Representation | S `Represent`, verify(Placed) | 467-472 |
| Code | last effect summaries, heat, then `GenerateCode` outside this file | 507-525 |

`BuildCallGraph`, `ComputeEffectSummaries`, `EstimateHeat`, `SummarizeForInlining` are program passes that produce a
`ProgramResult` on demand (`aot/pipeline/AOTPass.h:63-79`); each pass row lists the results it needs and those it invalidates.

### 2.3 The table of passes

`rows`, `aot/pipeline/AOTPipeline.cpp:136-248`: 48 program passes (137-184) and 60 function passes (186-247). A row gives: id,
`Required`/`Optional`, first and last form, inputs, outputs, invalidated results, for a function pass the invalidated analyses
(`nothing`, `values`, `everything`), and modifiers:

- `addingOnly({opcodes}, ..)`: the edited functions "do no more" than before, but for the declared bounds of the listed opcodes
  (`AOTPipeline.cpp:90-96`); this is how effect summaries survive a program pass. Example 170:
  `addingOnly({ Opcode::MaterializeEnvironment, Opcode::EmptyArgumentList }, programPass(AllocateInFrames, ..))`.
- `takingAway(..)`: the edited functions do less (`RemoveUnreadFields`, 157).
- `readingOtherBodies`, `derivingFunctions`, `countingOwnChanges`, `verifiedAfterNextPass`.

`Optional` passes can be switched off per build or per function (`m_build.isEnabled(id, FunctionID())`,
`BisectionSwitches.disabledPasses`, `aot/ir/schema/compile-call.def:134-137`).

### 2.4 The seven function schedules

`enum class FunctionSchedule : uint8_t { Type, Simplify, Prepare, Resolve, Optimize, Select, Represent }` (`aot/pipeline/AOTPass.h:81`).
Descriptors at `AOTPipeline.cpp:519-527` (mode, which functions, form):

| Schedule | Mode | Functions | Form |
| --- | --- | --- | --- |
| Type | InPlace | All | AsRead |
| Simplify | InPlace | OneAtATime (only included by others, or run by a program pass on a body it changed) | Prepared |
| Prepare | OnCopies | All | Prepared |
| Resolve | InPlace | All | Prepared |
| Optimize | OnCopies | Reached | Prepared |
| Select | InPlace | All | Prepared |
| Represent | InPlace | All | Placed |

Steps, in order (`[` .. `]` is a fixpoint):

**Type** (250-254): `TypeValues`, `CheckInt32Promises`, `CheckRegExpLiterals`.

**Simplify** (256-274): `[ [ FoldConstants, SimplifyControlFlow, RemoveTrivialParameters, PropagateBranchConditions,
HoistTestsIntoPredecessors, ThreadJumps, EliminateDeadCode ] InferSwitches, EliminateRedundantReads, RetypeValues ]`,
`MergeDuplicateBlocks`, `RetypeValues`, `CompactBlocks`.

**Prepare** (276-290): `SimplifyCasts`, `InsertObjectCoercibleChecks`, `PlaceAllocationsAtTheirUse`, `AddGenericVersion`,
`ExpandProvenIterations`, `ResolveBuiltins`, `BindGlobalNames`, `SimplifyBuiltinCalls`, `ExpandData`, `InsertObjectCoercibleChecks`,
`EliminateTailRecursion`, `RemoveRedundantHandlers`, include Simplify.

**Resolve** (293-295): `ResolveAccesses`. (Comment 292: "InlineCalls pastes a body as it stands when the Optimize schedule begins.")

**Optimize** (296-360):
`[ ResolveBuiltins, BindGlobalNames, SimplifyBuiltinCalls, CallThroughMethodTable, ResolveCalls, ResolveAccesses, SplitUnions,
PlaceTypeTests, ExpandIteration, ExpandOwnKeys, SelectTypedResults, ResolveAwaitedCalls, FuseArrayChains, InlineCalls,
PlaceAllocationsAtTheirUse, FuseGeneratorLoops, SimplifyCasts, RetypeValues, InsertObjectCoercibleChecks, RemoveRedundantHandlers,
Simplify (without compacting), SinkAllocations ]`,
`ExpandIteration`, `SelectTypedResults`, `ResolveBuiltins`, Simplify, `RestartInGenericVersion`, `StartGeneratorsAtFirstStep`,
`SeparateUnlikelyLoopPaths`, `ConvertCountersToIntegers`, `SimplifyBuiltinCalls`, `ReduceIntegerDivisions`, `FoldConstantRegExps`,
`ShareRegExpLiterals`, `FuseConcatenations`, `CreateStringBuilders`, Simplify,
`[ SinkAllocations, Simplify ]` (limit `MaxSinkingRounds`),
`PlaceTypeTests` (behind rounds), `ResolveCalls`, `ResolveAccesses`, `ExpandIteration`, Simplify, `RestartInGenericVersion`,
`RemoveDeadCountedLoops`, `HoistLoopInvariantReads`, `EliminateRedundantReads`, `EliminateGuards`, `TestNarrowerTypesFirst`,
`SinkConstants`, `PlanArrayCapacities`, `PlanCollectionCapacities`, `ExpandLongArrayLiterals`, `RetypeValues`, `CompactBlocks`.

**Select** (363-366): `FinishBirths`, `Select`.

**Represent** (368-371): `ChooseRepresentations`, `RemoveRedundantBarriers`.

---

## 3. Proofs (question 1)

### 3.1 Where they are encoded

Schema, `aot/ir/AOTSchema.def:19-28`:

```
enum ProofLifetime { | engine Forever Stretch }
enum Proof (lasts: ProofLifetime, readsOnlySubject: Bool) {
    | engine None(Forever, false) TestedType(Forever, false) Truthiness(Forever, false) Equality(Forever, false) IntegerBound(Forever, false)
    ExactFunction(Forever, false) CaseValue(Forever, false) NotNullish(Forever, false)
    Shape(Stretch, true) SlotFilled(Stretch, true) NoExtraProperties(Stretch, true) NoFieldOutsideSlot(Stretch, true) BirthFieldsFilled(Stretch, true) NotFrozen(Stretch, true) OwnInstance(Stretch, true)
    Pristine(Stretch, true) ArrayStorage(Stretch, true) InBounds(Stretch, true) NotThenable(Stretch, false) MemberUnchanged(Stretch, false)
    PrototypeLinkUnchanged(Stretch, true) EvaluatedOnce(Stretch, false) KeyOrder(Stretch, true)
    UnmodifiedGenerator(Stretch, false) EnumeratorCurrent(Stretch, false) PlainData(Stretch, false)
    SourceOpen(Stretch, false) TextMatches(Stretch, false)
}
```

Operation columns (`AOTSchema.def:119-121`): `proves : Proof` (what the test establishes), `relies : Proofs` (what a user needs),
`guarded : Operand` (the operand the reliance is about). Generated accessors: `provesOf(opcode)`, `reliesOf(opcode)`,
`guardedOf(opcode)`, `lastsOf(proof)`, `readsOnlySubjectOf(proof)`; helpers `provesForever()` / `provesForStretch()`
(`aot/ir/AOTOpcode.h:103-104`).

C++: `aot/ir/AOTEdgeResults.{h,cpp}` (what an edge of a branch hands to its target), `aot/analysis/AOTGuardAvailability.{h,cpp}`,
`AOTGuardFacts.cpp`, `AOTGuardMeet.cpp`, `AOTFailedTests.cpp` (facts and where they hold), `aot/ir/AOTVerifierGuards.cpp` (checks them),
`aot/passes/AOTTypeTest*.cpp`, `AOTPlaceTypeTests*.cpp` (place tests and compute stretches).

### 3.2 How a proof becomes a value

A test is an operation with result `natural(boolean)` (generator check `check-entries.ts:325`). `Branch`, `TestAndBranch`, `Switch`,
`SwitchString`, `SwitchOnShape` have targets that take proofs: `terminator Branch (condition) -> taken(proof), notTaken(proof)`
(`aot/ir/schema/operations.def:154`). In IR text `Branch v1 -> taken b1(| 0), notTaken b4()`: behind the bar stand proof entries, each
of which becomes one more parameter of the target block.

`edgeResultOfProofEntry()`, `aot/ir/AOTEdgeResults.cpp:80-98`, decides what the parameter is:

```cpp
    if (provesForStretch(result.proof)) {
        if (proofEntry || result.side != EdgeSide::Taken)
            return std::nullopt;
        result.kind = EdgeResultKind::Guard;
        ...
    }
    ...
    result.kind = EdgeResultKind::NarrowedOperand;
    result.subject = test.operand(proofEntry);
```

- **Forever**: the parameter is the tested operand again, as a new SSA value with the narrowed type (on either edge). Nothing can end
  it because it is a statement about a *value* that this frame already holds, not about memory.
- **Stretch**: the parameter is a `Guard` (only on the taken edge), an opaque value that is `engine functionLocal`
  (`AOTSchema.def:96`), has representation `Void` (`OperandKind Guard(Void)`, line 33) and costs no code. A user names it in its
  `guard?: Guard/inspects` operand. `AllGuards` unites several (`prepared-operations.def:31`). An allocation can itself be named as
  a guard (`GuardSource::Allocation`, `AOTGuardAvailability.h:27`).

Example (`aot/tests/passes/eliminate-guards.ir:13-24`):

```
    v1 = ShapeTest v0 layouts=[L0]
    Branch v1 -> taken b1(| 0), notTaken b4()
  b1(v2):                       # v2: guard
    ...
    v5 = GetSlot v0, v4 key="x" receivers=[L0] level=Expected
```

### 3.3 Four sources that can stand for a proof

1. **A test** on the way (guard).
2. **An allocation** in this function. `AOTGuardFacts.cpp:33-43`: a new layout instance proves
   `Shape, OwnInstance, SlotFilled, NoExtraProperties, NoFieldOutsideSlot, NotFrozen, KeyOrder` (+ `BirthFieldsFilled` for a literal that
   lists every birth field, 281-306); a new array `Pristine, ArrayStorage`; another new built-in instance `Pristine`.
3. **The type** of the subject (`isProvenByType()`, `AOTGuardFacts.cpp:511-528`): `Shape` when the type has only layouts, is not
   nullish and the layout set is complete; `NotThenable`; `Pristine` from `ClassPart::provenPristine`; `ArrayStorage` from
   `contentsAreProven && arrayStorage`. These come from flow and need no run-time test at all.
4. **A flow record** of the field, the layout or the class. `proofsDecidedByFlow` (`AOTGuardFacts.cpp:51-63`): `SlotFilled,
   NoExtraProperties, NoFieldOutsideSlot, BirthFieldsFilled, NotFrozen, OwnInstance, KeyOrder, MemberUnchanged,
   PrototypeLinkUnchanged, EvaluatedOnce`. "A flow record of the field, the layout or the class can stand for these, so nobody can tell
   whether an access takes them from its guard." `proofsNeededFromGuard()` (530-557) therefore never *demands* these from a guard
   (except `SlotFilled` with `holdsDeclaredType`); the pass that created the access decided from `FieldFlow.occupancy`,
   `FieldFlow.stability`, `LayoutFlow.impossibleMutations`, `ClassFlow` (`aot/ir/schema/flow-records.def:22-49`).

### 3.4 The list

"Reads" is the declared effect of the establishing test and is exactly the dependency set (3.5). Files: `op` = `operations.def`,
`pre` = `prepared-operations.def`, `int` = `intrinsic-operations.def`, `td` = `typed-data-operations.def`, all in `aot/ir/schema/`.

| Proof | Lasts | Asserts | Established by (reads) | Relied on by |
| --- | --- | --- | --- | --- |
| `TestedType` | forever | the value has the tested type | `TypeTest` (none) op:133; `Check` becomes `TypeTest` + branch in `AddGenericVersion` | the narrowed value's type |
| `Truthiness` | forever | the value is truthy / falsy | `ToBoolean` (none) op:112 | narrowing |
| `Equality` | forever | operand equals the other | `Eq` op:109, `StrictEq` op:110 | narrowing |
| `IntegerBound` | forever | integer relation | `Less`, `LessEq`, `Greater`, `GreaterEq` op:105-108 | integer ranges |
| `ExactFunction` | forever | the value is a closure of that function / that built-in | `IsClosureOf` (none) pre:27, `IsFunctionOfBuiltin` (none) pre:97 | direct calls |
| `CaseValue` | forever | the subject is the case's value | `Switch`, `SwitchString` op:155-156 | narrowing |
| `NotNullish` | forever | not undefined or null | no row has `proves = NotNullish`; it is a *requirement* of access plans (`aot/analysis/AOTPropertyAccess.cpp:555`, `aot/passes/AOTMethodDispatch.cpp:49,186`) | member calls |
| `Shape` | stretch in the schema, **in fact for life** (3.6) | the object's layout is in the set | `ShapeTest` (`ShapeWords[layouts]`) pre:11; `SwitchOnShape` pre:158; allocation; type | every slot operation |
| `OwnInstance` | as `Shape` | the object belongs to this `Instance` (realm) | `BelongsToInstance` (`ShapeWords`) pre:18; allocation | method dispatch |
| `SlotFilled` | stretch | the slot holds a value (with flag `holdsDeclaredType`: a value of the field's type, neither undefined nor null) | `IsSlotFilled` (`ObjectFields[key]`) pre:12; allocation that lists the field; `BirthFieldsFilled` (see below); flow `occupancy` | `GetSlot`, `GetSlotGetter`, `GetFlattenedSlot`, `GetDeferredClosureSlot` pre:34-39 |
| `NoExtraProperties` | stretch | no property in the side object | `HasNoExtraProperties` (`NamedProperties`) pre:13 -> bit `orderWordHasExtraProperties` of the order word (`aot/codegen/AOTGuardEmitter.cpp:167-171`) | reads that answer `undefined`, serializers |
| `NoFieldOutsideSlot` | stretch | no declared field has left its slot | `HasNoFieldOutsideSlot` (`NamedProperties`) pre:14 -> order word bit or the flags byte (`AOTGuardEmitter.cpp:173-186, 263-275`) | `GetSlotOrUndefined`, stores |
| `BirthFieldsFilled` | stretch | every field that each constructor fills is filled, and raw numbers/booleans among them are raw | `HasBirthFieldsFilled` (`NamedProperties`) pre:15 -> `TypeInfoPerCellBit` clear in `JSCell::typeInfoFlags` (`AOTGuardEmitter.cpp:177-178, 254-261`) | stands for `SlotFilled` (`provesForUser`, `AOTGuardFacts.cpp:709`; `isAnsweredByTestOfCell`, 643-655) |
| `NotFrozen` | stretch | not frozen/sealed/non-extensible | `IsNotFrozen` (`ShapeWords, OrderWords`) pre:16 -> `orderWordIntegrityBits` (`AOTGuardEmitter.cpp:282-290`); `OrderWords` is dropped from the dependencies (`AOTGuardFacts.cpp:739-740`) | `PutSlot`, `PutSlotOrAdd`, `PutSlots` |
| `Pristine` | stretch | an array/Map/Set/Promise/RegExp.. is as the engine makes it (aspects `Storage Names AsCreated Dense NamesKeptBelow`, `types.def:26`) | `IsPristine` (`NamedProperties, PrototypeLinks`) pre:19; `IsPristineArray` (+`ArrayStorage`) int:37; allocation; type | element operations pre:58-62, `RegExpMatchLiteral` |
| `ArrayStorage` | stretch | the array's indexing type is Int32 / Double / Contiguous (flag `isWritable`: not copy-on-write) | `HasArrayStorage` (`ArrayStorage`) pre:20; allocation; type | `GetArrayElement`, `PutArrayElement`, `GetArrayLength`, `HasArrayElement`, `AppendElements`, `IsElementPresent` |
| `InBounds` | stretch | position < length (or: an element is present; or: inside a typed array view) | `IsInBounds` (`ArrayStorage`) pre:21; `IsElementPresent` (`ArrayStorage, Elements[Array]`) pre:22; `IsInsideView` (`TypedArrayStorage`) pre:23 | `GetArrayElement`/`PutArrayElement` with `isInBounds`/`isPresent`, `Get/PutTypedArrayElement` with `isInsideView` |
| `NotThenable` | stretch, no subject | resolving with the value finds no `then` | `IsNotThenable` (`NamedProperties, ObjectFields, PrototypeLinks`) pre:24; type | `FulfillFreshPromise` |
| `MemberUnchanged` | stretch, no subject | listed class members / a built-in location are as defined | `AreMembersUnchanged` (`MemberGuards[conditions]`) pre:25 -> one guard bit in the statics of the Instance (`AOTGuardEmitter.cpp:361-364, 405-412`); `IsBuiltinLocationUnchanged` (`NamedProperties`) pre:96 | resolved calls and member reads |
| `PrototypeLinkUnchanged` | stretch | the object still has its layout's prototype | `HasLayoutPrototype` (`PrototypeLinks`) pre:17 -> flag `IsPristine` of the flags byte | inherited member lookups |
| `EvaluatedOnce` | stretch, no subject | a class was evaluated once | no row; `MemberConditionKind::ClassEvaluatedOnce` inside `AreMembersUnchanged` (pre:7-8) | |
| `KeyOrder` | stretch | the order word names this key order and count | `HasKeyOrder` (`OrderWords`) pre:26 | expanded `Object.keys`, `for-in`, stringify |
| `UnmodifiedGenerator` | stretch, no subject | generator and its prototypes untouched | `IsUnmodifiedGenerator` int:18, `IsUnmodifiedAsyncGenerator` pre:119 (`NamedProperties, PrototypeLinks`) | fused generator steps |
| `EnumeratorCurrent` | stretch, no subject | a `for-in` enumerator still matches the object | `IsEnumeratorCurrent` (`ShapeWords, OrderWords, NamedProperties`) pre:28 | `EnumeratorGetByVal` |
| `PlainData` | stretch, no subject | plain data object | `IsPlainDataObject` (`ShapeWords, NamedProperties, PrototypeLinks`) td:34 | `GetOwnDataProperty`, `CloneAs` |
| `SourceOpen` | outlasts writes (3.6) | the typed JSON parser's source is open | `JSONSourceOpen` td:7 | `JSONSourceStart/Close/Abandon` |
| `TextMatches` | stretch, no subject | text at the cursor matches | `JSONStartsWith` (`ParserState`) td:13 | `JSONAdvance` |

### 3.5 How "something can undo it" is computed

**Step 1: dependencies.** `locationsReadBy(test)`, `aot/analysis/AOTGuardFacts.cpp:732-742`: the declared `reads(..)` of the row, collected by
`ReadCollector` (67-100). `readsAllHeaps`/`unknownCode` means every heap. Bookkeeping heaps count only if nothing else is read
("A test that reads nothing else depends on its bookkeeping reads", 90). For a proof that an allocation gives:
`locationsBehindAllocationProof()` (232-253): `SlotFilled`, `NotFrozen` -> `ShapeWords`; `KeyOrder` -> `ShapeWords, OrderWords`;
`NoExtraProperties`, `NoFieldOutsideSlot`, `BirthFieldsFilled` -> `NamedProperties`; `Pristine` -> `PrototypeLinks, NamedProperties`;
`ArrayStorage` -> `ArrayStorage`.

**Step 2: subject.** `dependenciesOfGuardOf()`, `AOTGuardAvailability.cpp:426-437`: if `readsOnlySubject` is true for the proof and no
other operand may be a reference (`testReadsOnlySubject`, `AOTGuardFacts.cpp:1046-1059`) the fact is about one object
(`identityOf(operand 0)`), else it has no subject and any overlapping write ends it.

**Step 3: each writer.** The decisive predicate, `AOTGuardAvailability.cpp:43-46`:

```cpp
static bool mayEndProofOver(const Effects& effects, const LocationMasks& dependedOn)
{
    return effects.maySuspend || effects.writes.overlaps(dependedOn);
}
```

`KillQuery::writtenOf()` (147-176) refines it on one edge (normal or handler) of the writer:
- `maySuspend` -> `WriteExtent::Everywhere`;
- no overlap of write masks -> nothing;
- `leavesSlotsAsTested()` -> nothing: a store whose writes are within `PutSlotOrAdd`'s bound and that stores a value that cannot be
  `absent` (and, for `holdsDeclaredType`, cannot be undefined/null) does not end `SlotFilled` (`AOTGuardFacts.cpp:368-382`);
- a written location in a heap that is not `perObject` -> everywhere;
- else `InSomeObjects`.

An exact store (`memoryAccessOf()` says `Write`) is compared location by location with `overlapOf()` (`KillQuery::mayWrite`, 179-191).

**Step 4: which objects.** `KillQuery::mayEndProofAbout()` (193-215). No subject -> ends. Otherwise:
- `mayEndProofJudgedByDefinitions` (230-256): "An object from outside is confined nowhere, and two values from outside may name one
  object wherever their types allow it." If the subject `comesFromOutside()` (entry parameter, result of a read or call,
  `AOTGuardFacts.cpp:144-180`) and the writer `mayWriteBeyondOperands` -> ends. Else compare with each operand that the writer writes
  through: same object -> ends; types that cannot be the same object (`mayBeSameObject`) -> skip; operand from outside -> ends.
- `mayEndProofJudgedByObjects` (258-271): uses `EscapeAnalysis` (`mayAlias`, `pointsTo`). A subject that is **confined** (allocated here,
  has not escaped before the writer, does not escape at the writer, is not contained in an operand; 292-347) is touched only through
  operands that point to it.

`mayWriteBeyondOperands` comes from `effectsBeyondOperands()` (`aot/ir/AOTEffects.cpp:187-200`): for a call of known compiled functions
it is the callees' `elsewhere` summary plus the memory of parameters that no operand maps to (154-177); for an operation whose
`confinementOf()` is `WritesOnlyNamedOperands` the per-object heaps are dropped (179-185); anything that `mayRunOtherCode()` keeps all its effects.

**So calls do not clobber everything.** A call clobbers what the callee's summary says; an unresolved call clobbers
`summaries.unknownCode()` (4.4), which is still less than everything.

**Step 5: dataflow.** `GuardAvailability` numbers (guard, fact) components, groups them in kill classes by (locations, subject), collects kills
per block and solves forward availability (`AOTGuardAvailability.h:141-336`). States `Unknown / Unavailable / Available` (64-73). It may give up
on a work limit (`gaveUp()`), then answers `Unknown`. Between a test and its branch a writer is looked for separately
(`mayEndBetweenTestAndBranch`, cpp:1201).

**Step 6: enforcement.** `FunctionVerifier::verifyGuards()` (`aot/ir/AOTVerifierGuards.cpp:20-71`) walks every operation with a non-empty
`relies`, computes `proofsNeededFromGuard`, and fails with `InternalErrorKind::GuardNotAvailable` where the guard's state is `Unavailable`.
So a change of the effect model is checked mechanically by every test run that verifies.

`PlaceTypeTests` uses the same machinery through `EndingsAtWriter` (`aot/passes/AOTTypeTestEndings.cpp:23-77`): `condition.lastsForever()`
-> never ends; else `mayWriteDependenciesOf` -> `leavesSlotsAsTested` -> `GuardWriter::mayEnd`.

### 3.6 Three "stretch" proofs that no write ends

`aot/analysis/AOTGuardFacts.cpp:48-49`:

```cpp
// A compiled object keeps its shape, and with it its instance, for life.
static constexpr ProofSet proofsThatOutlastWrites { Proof::Shape, Proof::OwnInstance, Proof::SourceOpen };
```

`locationsReadBy()` returns the empty list for them (734-735, 746-747). The back end agrees: `AOTObject_shapeWord` is declared an *immutable*
B3 heap (`aot/codegen/AOTHeaps.h:42-52`). `aot/codegen/AOTSlotAccessEmitter.cpp:1043`: "The shape word of an object never changes".

### 3.7 Monotone bits

`aot/passes/AOTResolveCallsTestSites.cpp:54-55`: "A test in front of a member call also answers for the member read before it, so what it
proves may never come back once it is lost. The bits of a class never come back either."
`monotoneObjectProofs { NotNullish, OwnInstance, PrototypeLinkUnchanged, NoExtraProperties, NoFieldOutsideSlot }`. These states are lost once and
never regained, so a test that passes *later* also vouches for *earlier*.

---

## 4. The effect model (question 2)

### 4.1 Abstract heaps

`aot/ir/AOTSchema.def:58-81`. Index kinds in brackets. Marks: `observable` (a program can see a write), `ordered` (accesses must keep their order),
`holdsValues` (JS values live there), `perObject` (a location exists once per object), `oneValuePerLocation` (read/store forwarding is legal).

| Heap | Index | Marks |
| --- | --- | --- |
| `ModuleVariables` | Variable | observable ordered holdsValues oneValuePerLocation |
| `EnvironmentFields` | Layout, Field | observable ordered holdsValues perObject oneValuePerLocation |
| `ObjectFields` | PropertyKey (+ receivers, in C++) | observable ordered holdsValues perObject oneValuePerLocation |
| `ShapeWords` | LayoutSet | observable ordered perObject |
| `OrderWords` | | observable ordered perObject |
| `MemberGuards` | Layout | ordered |
| `PrototypeLinks` | | observable ordered holdsValues perObject |
| `NamedProperties` | PropertyKey | observable ordered holdsValues perObject oneValuePerLocation |
| `Elements` | ElementBase (`Unknown Array Arguments Object`) | observable ordered holdsValues perObject |
| `ArrayStorage` | | observable ordered perObject |
| `TypedArrayStorage`, `TypedArrayElements` | | observable ordered |
| `CollectionStorage` | | ordered perObject |
| `CollectionEntries` | | observable ordered holdsValues perObject |
| `InternalFields` | | observable ordered holdsValues perObject |
| `RegExpState` | | observable ordered |
| `PromiseState` | | observable ordered holdsValues perObject |
| `StringStorage` | | ordered |
| `StringBuilderContents` | | ordered perObject |
| `ParserState` | | ordered |
| `ExternalState` | | observable ordered |
| `AllocatorState` | | (none) |
| `RuntimeState` | | ordered |
| `ValidationState` | | (none) |

Derived sets, `aot/ir/AOTAbstractHeap.h:36-45`: `bookkeepingHeaps { AllocatorState, RuntimeState }`;
`heapsWhoseWritesNobodySees = bookkeeping | { StringStorage, CollectionStorage }`; `compiledCodeHeaps { ModuleVariables, EnvironmentFields }`
(only compiled code can write them). At most 64 heaps (line 23).

Named effect sets (`AOTSchema.def:83-91`, `operations.def:17-18`, `runtime-operations.def:1-5`): `Allocates = reads(AllocatorState)
writes(AllocatorState) collects`, `AllocatesWithoutCollecting`, `UsesRuntime`, `ResolvesRopes`, `CreatesString`, `GrowsArray`, `AppendsToBuilder`,
`ReadsObjectGraph`, `DefinesProperty`, `DefinesMemberOfClass`, `Runtime*`.

### 4.2 Locations and masks

`MemoryLocation` (`aot/ir/AOTMemoryLocation.h:29-100`): heap + up to two indexes, or the entire heap. `LocationMasks` (116-258): a set of heaps plus,
for each *indexed* heap, a 64-bit Bloom-style mask. `maskOf()` (`AOTMemoryLocation.cpp:18-104`) hashes the index to one bit (`pairIntHash & 63`); a layout set
sets one bit per member, or the whole heap if it is incomplete, has members without layout, or more than 64 members; `ElementBase` has a bit per base.
`overlapOf(a, b)` (139-180) gives `None / May / Exact`: two object fields with different keys never overlap; equal keys overlap unless both name complete, disjoint
receiver sets (113-130).

`memoryAccessOf(value)` (`aot/ir/AOTMemoryLocationAccesses.cpp:88-130`) recognises an **exact** read or write: effect rule `Declared`, exactly one read or
one write of one indexed location, no throw/collect/suspend/terminate, no operand code, heap `holdsValues` and `oneValuePerLocation`; gives location, base and
stored value. These are `GetModuleVariable`, `PutModuleVariable`, `Get/PutEnvironmentField`, `GetSlot`, `GetSlotOrUndefined`, `PutSlot` and the like.

`stabilityOf()` (132-220): `LocationStability { Unknown, ChangedByOwnStores, UnchangedDuringActivation }`, from the flow record of the variable/field
(`LocationFlow.stores`, `.initialization`, `.isUnchangedDuringActivation`, `.writers`).

### 4.3 `Effects`

`aot/ir/AOTEffects.h:64-160`:

```cpp
struct Effects {
    LocationMasks reads { LocationMasks::top() };
    LocationMasks writes { LocationMasks::top() };
    ThrowEffect mayThrow { true };          // + "by rule" or only for lack of memory
    bool mayCollect : 1 { true };
    bool mayCallUnknownCode : 1 { true };
    bool maySuspend : 1 { true };
    bool mayBeTerminated : 1 { true };
```

Predicates: `isPure`, `isRemovableWhenUnused` (writes only what nobody sees, no throw by rule, not terminated, runs no other code), `isUnobservable`,
`alwaysCompletes`, `mayRunOtherCode() = maySuspend || mayCallUnknownCode`, `mayExitSideways`. Reordering test `interferesWith()` (93-103): ordered write/write,
write/read, read/write overlap; two sideways exits; a sideways exit against an ordered write; **"runs other code" against any ordered read**.
Two reads never interfere; `mayCollect` interferes with nothing (static asserts `AOTEffects.cpp:376, 391-393`).

`EffectsOnEdges { normal, handler }` (162-196): what has happened when the operation completes, and when it throws to a handler.
`EffectTraits`: the eight yes/no answers in one byte, all eight bits used (246-256).

### 4.4 The six effect rules

`enum EffectRule { Declared Operator Access Call Builtin Inline }` (`AOTSchema.def:6`); written `; <effects> by <Rule>` in a row. The row is the **bound**;
the rule may find that less happens. Dispatch: `describeEffectsInto()`, `aot/ir/AOTEffectsDescription.cpp:321-348`. It fills an
`EffectDescription { own, functions, slowPathFunctions, callsUnknownCode, .. }` (`AOTEffectsDescription.h:19-43`), resolved by `resolveEffects()`
(cpp:402-422):

```cpp
    for (FunctionID function : description.functions) {
        if (context.program.at(function).hasBody())
            result.merge(CalleeSummary(context, function)->onCall());
        else
            namesFunctionWithoutBody = true;
    }
    // While a frame is suspended its callers run on, and what they store into their own allocations is in no summary.
    if (description.own.normal.maySuspend)
        result.normal.merge(Effects::unknownCode());
    else if (description.callsUnknownCode || namesFunctionWithoutBody)
        result.normal.merge(context.summaries.unknownCode());
```

- `Declared`: the row, refined by the value (`refinedEffectsOf`, 248-267: casts that cannot fail, flat strings, positions inside the length ..), plus the code that operands
  may run by their `uses`/`runs(Protocol)` (`describeOperands`, `aot/ir/AOTEffectsOfOperands.cpp`).
- `Operator`: by operand types (`AOTEffectsOfOperators.cpp`).
- `Access`: by receiver type and access plan (`AOTEffectsOfAccesses.cpp`).
- `Call`: `describeCall()`, `AOTEffectsOfCalls.cpp:233-257`. Callees = functions the row names, else the `CalleeSet` stamp that flow left (`calleesOfCall`, 87-99). Incomplete set -> unknown code.
- `Builtin`: the table (4.6). `Inline`: the row of the inline built-in, or `asOverload` (4.6).

### 4.5 Interprocedural summaries

`aot/ir/AOTEffectSummaries.h`. One `FunctionSummary` per function (54-65):

```cpp
struct FunctionSummary {
    EffectsOnEdges elsewhere { EffectsOnEdges::unknownCode() };   // what it does to memory not reached through a parameter
    OperandMemory throughParameters;                              // union of the next
    Vector<ParameterSummary> parameters;                          // per entry parameter: OperandSummary (uses, retainedIn) + OperandMemory (reads, writes)
    UnknownCodeWitness unknownCodeWitness;
```

`onCall()` = elsewhere + throughParameters. Bodies are summarised in `AOTEffectSummariesOfBodies.cpp` (accesses are attributed to a `BaseObject { Untracked,
EntryParameter, OwnAllocation }`, header 368-376: writes to an own allocation that does not escape are in no summary), then a fixpoint over nodes
`OnReturn / OnThrow / Parameter(n) / UnknownCode` in `AOTEffectSummariesSolver.cpp`. Suspension is stripped from a summary (`createFunctionSummary`,
`AOTEffectSummaries.cpp:640-666`: a caller of an async function does not suspend). Updated incrementally (`updateEffectSummaries`). In a validating build
`checkSummaryCoversBody` compares each summary with its body (`AOTPipelineStages.cpp:145-156`).

**Unknown code is itself summarised over the whole program.** `EffectSummarizer::effectsOfUnknownCodeAlone()`, `AOTEffectSummariesSolver.cpp:680-702`:

```cpp
    Effects unknown = Effects::unknownCode();
    unknown.writes.remove(compiledCodeHeaps);
    for (.. variables ..)
        if (variable.flow.writers != WriterState::AllKnown || isStoredThroughClassObject)
            unknown.writes.add(m_program, MemoryLocation::moduleVariable(VariableID(index)));
    for (.. environment fields ..)
        if (flowOf(..).location.writers != WriterState::AllKnown) unknown.writes.add(..environmentField..);
```

plus (`updateEffectsOfUnknownCode`, 731-740) the effects of every function that unknown code may enter (`mayBeEnteredByUnknownCode`, 34-43: may be resumed, may be
called by the host, has indirect callers, or flow does not know all callers). So "unknown code" writes every object heap, but only those module variables and environment
fields that some reachable-by-unknown-code function stores.

### 4.6 Native built-ins

`aot/builtins/*.builtins.ts` (26 files) + Bun's; generator `aot/builtins/generator/`. Fields of an overload that the effect model reads
(example `aot/builtins/buffer.builtins.ts:140-170`):

- `capability: "none" | "heap" | "js"`. `ownEffectsOf()`, `aot/ir/AOTEffectsOfBuiltins.cpp:34-49`: `heap` and `js` add `Allocates + UsesRuntime + ResolvesRopes`; `js` without
  `runsOnlyOperandCode` adds unknown code (113-114).
- `reads: [heap..]`, `writes: [heap..]`: abstract heaps by name.
- `throws`, `onlyPropagatesErrors`.
- `uses: { operand: [b.readsContents(), b.writesContents("element"), b.retained, b.inspected, b.readByKey(..), b.runsTransitively("ToPrimitive") ..] }`: per operand; these become
  `OperandBehavior` (`aot/ir/AOTEffectsOfOperands.h:29-44`: uses, protocols, named members, where retained) and decide escape, aliasing and which user code runs.
- class level: `stateHeap`, `slots`, `jsType`, `mutability`, `lockedMembers`.
- `implementation: b.hostCall | b.inline(name, { parameters, otherwise })`, or a library function / model written in JavaScript, whose *compiled body's summary* is used
  (`describeJavaScriptOf`, 74-93).
- An entry that is not declared is unknown code (`overload.isUndeclared()`, 104-107).

Run-time operations called by compiled code have rows too: `runtime Name : result (params) ; effects` (`aot/ir/schema/runtime-*.def`), e.g.
`runtime PollSlow : void () ; reads(all) writes(AllocatorState, RuntimeState) terminates collects` (`runtime-operations.def:174`). At a call the code generator uses
`context.effects() ∩ effectsOfCallee` (`aot/codegen/AOTRuntimeCallEffects.cpp:17-20`).

`--aot-validate` checks declared effects of native calls at run time (PR text; `checkDeclaredEffects`, `AOTPipelineStages.cpp:319-320`, `aot/ir/AOTEffectsCheck.cpp`).

---

## 5. Passes that remove, move, merge or reorder memory accesses and tests (question 3)

Column "2nd thread" answers: would the fact be broken by another thread that writes the same object between two operations of this thread?
"confined" = the fact is about an object that no other thread can reach, so it survives.

### 5.1 IR function passes

| Pass | File (`aot/`) | What it does | Fact it relies on | 2nd thread |
| --- | --- | --- | --- | --- |
| `EliminateRedundantReads` | `passes/AOTEliminateRedundantReads.cpp`, `.h` | load-load CSE, store-to-load and allocation-to-load forwarding, with merges at joins, on `ModuleVariables`, `EnvironmentFields`, `ObjectFields` (`.h:16`) keyed by (location, base) | no operation in between writes an overlapping location of an aliasing base; a class that is **fixed** (`stabilityOf != Unknown` or a "settling" variable, 338-347) is cleared by *no* call ("No call clears the bits of a fixed class", 464) | broken for non-fixed classes of escaped objects and for variables. Fixed classes: see 5.3 |
| `HoistLoopInvariantReads` | `passes/AOTHoistLoopInvariantReads.cpp` | moves reads of `ModuleVariables`/`EnvironmentFields` from a loop header to the preheader (27, 149-170) | nothing in the loop writes the location; `mayRunOtherCode` clobbers everything (72-80) unless the location is stable | broken: `while (!flag) {}` on a module variable never sees the store |
| `SeparateUnlikelyLoopPaths` -> `carryVariablesAroundLoops`, `delayStoresOfLoopVariables` | `passes/AOTCarryVariablesAroundLoops.cpp`, called at `passes/AOTSeparateUnlikelyLoopPaths.cpp:701` | register promotion: "A loop that reads a variable of an enclosing function or of a module in every turn reads what it stored or read in the turn before" (31). The variable becomes a block parameter. `delayStoresIn()` (690-799) removes the stores in the loop and puts **one store on every leaving edge** | no operation of the loop reads or writes the location, throws or runs other code (711-717) | broken, and worse than stale: the leaving edge stores the carried value even on a path where the loop stored nothing (769: the first definition is the header parameter), i.e. it *invents a write* of an old value and can overwrite another thread's store |
| `EliminateGuards` | `passes/AOTTypeTestElimination.cpp:1558` | folds a test whose condition holds on every path; its users take the guard that holds there | guard availability (3.5) | broken for every stretch proof but `Shape`, `OwnInstance` on escaped objects |
| `PlaceTypeTests` (+ hoisting) | `passes/AOTPlaceTypeTests*.cpp`, `AOTTypeTestStretches.*`, `AOTTypeTestEndings.cpp`, `AOTPlaceTypeTestsHoisting.cpp` | "One test covers a stretch of uses of a receiver" (`tests/passes/place-type-tests.ir:1`); offers tests on the edges that enter a loop (`TypeTestHoister`, 88-110) | the stretch contains no writer that ends the proof | same |
| `HoistTestsIntoPredecessors`, `TestNarrowerTypesFirst`, `PropagateBranchConditions`, `ThreadJumps`, `MergeDuplicateBlocks`, `InferSwitches`, `SimplifyControlFlow` | `passes/AOTControlFlowRules.cpp:116,235`, `AOTPropagateBranchConditions.cpp`, `AOTThreadJumps.cpp` .. | move or fold tests of SSA values | a condition value is an SSA value; `ThreadJumps`/`SimplifyControlFlow` read effect summaries | tests of values: not broken. A *state* test that is threaded past is the guard problem again |
| `ResolveAccesses`, `ResolveCalls`, `CallThroughMethodTable`, `SplitUnions`, `ExpandIteration`, `ExpandProvenIterations`, `ExpandOwnKeys`, `FuseArrayChains`, `SelectTypedResults` | `passes/AOTResolve*.cpp`, `analysis/AOTPropertyAccess*.cpp` .. | turn by-name accesses into test + slot/element access | create the test/use pairs | they are where check-then-act pairs are born |
| `SinkAllocations` | `passes/AOTSinkAllocations*.cpp`, `AOTVirtualObject*.cpp` | scalar replacement; builds the cell only where needed; folds stores into the allocation | escape analysis: the object is not reachable by other code until materialised | confined |
| `EliminateDeadCode` | `passes/AOTEliminateDeadCode.cpp` | unused values without effects, unused block parameters, write-only objects ("A store into an object that does not escape is live once a live value may be the object", 175) | `isRemovableWhenUnused`, escape analysis | removed *reads* are harmless; removed stores are confined |
| `PlaceAllocationsAtTheirUse`, `SinkConstants`, `CreateStringBuilders`, `FuseConcatenations`, `ExpandIteration` patterns, `ResolveCalls(Lists)`, `FuseArrayChains` interleaving | files that call `interferesWith` (grep: `passes/AOTPlaceAllocationsAtTheirUse.cpp`, `AOTSinkConstants.cpp`, `AOTCreateStringBuilders.cpp`, `AOTExpandIterationPatterns.cpp`, `AOTResolveCalls.cpp`, `AOTResolveCallsLists.cpp`, `AOTArrayChainInterleaving.cpp`) | move an operation over others | `!a.interferesWith(b)`: two reads may swap; a read may move over a write of another location | reorders this thread's reads (and a read against an unrelated write). Allowed by a relaxed model, not by sequential consistency |
| `RemoveDeadCountedLoops` | `passes/AOTRemoveDeadCountedLoops.cpp` | skips a counted loop without effects | effects | harmless |
| `PlanArrayCapacities`, `PlanCollectionCapacities` | `passes/AOTArrayCapacit*.cpp` | allocate with room, `ReserveArrayCapacity`, `HasSpareArrayCapacity` | mostly own allocations | not traced for escaped arrays |
| `RestartInGenericVersion` | `passes/AOTRestartInGenericVersion.cpp` | see 7.4: runs the reads of a prefix twice | `canRunTwice`: writes only what nobody sees, runs no other code (33-37) | second run may read other values: harmless |
| `FinishBirths` | `passes/AOTFinishBirths.cpp` | clears the "unfinished" mark of a new object behind the last birth store "where no other code has run since somebody else could reach the object" (`tests/passes/finish-births.ir:1-2`) | no other code between escape and store | broken if `this` escaped to another thread in the constructor (6.2, B) |
| `FoldConstants` (+ `AOTKnownValue.cpp`) | `passes/AOTFoldConstants.cpp` | folds a value whose *type* is one constant, so a read of a variable narrowed by `NarrowConstantVariables` folds | types | see 5.3 |
| `ShareRegExpLiterals`, `FoldConstantRegExps` | `passes/AOTShareRegExpLiterals.cpp` | "A literal whose object nobody can observe becomes one shared object" | nobody observes the object (`lastIndex` ..) | the shared object is shared between threads too (11, H19) |
| `Select` rules | `select/` ; `crossesEffects()` `select/AOTSelectionContext.cpp:137-152`, `losesGuard` 154 | fuse several operations into one (`PutSlots`, `PropGetPair`, `PropGetAndTest`, `CallMethod`, `TestAndBranch`) | members may move to the root if pending effects do not interfere | as `interferesWith` |
| `RemoveRedundantBarriers` | `represent/AOTRemoveRedundantBarriers.cpp` | (a) no barrier for a store into a frame cell; (b) none for an owner allocated since the last safepoint (`YoungOwnersProblem`, 113-159; only opcodes with `allocatesAfterLastSafepoint`, 368-380; lost when the owner escapes, tests `remove-redundant-barriers/escaped-by-*.ir`); (c) an earlier store is covered by a later barrier on the same owner when nothing in between "can collect, throw, suspend or return" (`covered-by-later-store.ir:1`) | `reachesSafepoint = mayCollect || mayRunOtherCode` (77-80): the collector changes phase only at a safepoint *of this thread* | holds iff the collector still needs every mutator at a safepoint before marking state flips (11, H17) |

### 5.2 IR program passes

| Pass | File | Fact | 2nd thread |
| --- | --- | --- | --- |
| `AllocateInFrames` | `passes/AOTFrameAllocation*.cpp` | "Closures, environments and objects that do not escape their activation are allocated in the machine frame" (`tests/passes/allocate-in-frames.ir:1`); escape analysis + callee parameter summaries ("borrowed parameters") + flow's `FrameLocality` | confined, provided the thread-spawn built-in declares that it retains its operands |
| `AllocateInnerObjects` | `passes/AOTAllocateInnerObjects.cpp` | "Two objects that are born together get one cell" | static, no time assumption |
| `FlattenInnerObjects` | `passes/AOTFlattenInnerObjects.cpp` | form 1: "Where nobody ever holds the inner object its fields become private fields of the outer layout" (whole-program, static). Form 2: the slot *defers* the inner object, made on demand by the read (`MakeBorrowedObject`, `GetFlattenedSlot`); the test file says form 2 is off (`tests/passes/flatten-inner-objects.ir:4`, testing switch `FlattenUntilHeldWhereverLegal`) | form 1 safe. Form 2 is check-then-make |
| `DeferClosuresInFields` | `passes/AOTDeferClosuresInFields.cpp` | a slot holds "not made yet" (`uncreatedDeferredClosure = 0`) and `GetDeferredClosureSlot` makes the closure and writes the slot (`pre:39`, `Allocates reads writes(ObjectFields[key])`); `isNeverWriter()` says this write ends no proof (`AOTGuardAvailability.cpp:38-41`) | check-then-make: two threads make two closures, `===` differs |
| `DeferInitializers` | `passes/AOTDeferInitializers.cpp` | "A variable that a body that runs once stores once, with what nobody can tell when it was made, stays empty until its first read .. each read that finds the variable empty calls it" | check-then-make on a module variable |
| `DeferDefinitions` | `passes/AOTDeferDefinitions.cpp` | a variable stored once with a closure that captures nothing "is born as that function" | static once decided; decided from `StoreCount::AtMostOnce` (322) |
| `NarrowConstantVariables` | `passes/AOTNarrowConstantVariables.cpp` | "A variable of a module that is stored at most once, and by every store in the program with one primitive constant, gets the type of that constant; a dead zone stays in the type" | value is the constant or `absent`: safe if `absent` stays in the type where a read may precede the store |
| `PromoteEnvironmentsToVariables` | `passes/AOTPromoteEnvironmentsToVariables*.cpp` | the environment of a function that `runsAtMostOnce` (Plan:182-204, 673) becomes module variables | needs "at most once" to stay true per Instance |
| `ReadCapturesFromEnvironments`, `PromoteAggregateParameters`, `SplitFreshResults`, `RemoveUnusedResults/Parameters`, `RemoveUnreadExports` | `passes/..` | `PromoteAggregateParameters`: "A parameter that is only taken apart is replaced by what is read from it": the reads move from the callee to the caller (inputs `Flow, EffectSummaries`) | moves reads earlier: stale value only |
| `RemoveUnreadFields` | `passes/AOTRemoveUnreadFields.cpp` | `ReadState::NeverRead` from flow | static |
| `ReuseEnvironments` | `passes/AOTReuseEnvironments.cpp`, `codegen/AOTEnvironmentEmitter.cpp:74-94` | "A heap environment goes back to its layout's free list where the function has done with it, unless a reference to it escaped": one *free cell word per layout in the statics of the Instance*, plain `store64` | the fact (not escaped) is confined, the free-cell word is shared state (H14) |
| `EmbedConstantCollections`, `NumberLiteralStrings`, `CollectRegExpLiterals` | | constants | static |
| `ElideStackChecks` | `passes/AOTElideStackChecks.cpp` | see 8.3 | per-thread stacks: same reasoning, per-thread limit needed |
| `CallModules` | `passes/AOTCallModules.cpp` | a module body is called where it is first needed, `BeginModuleEvaluation` (`pre:133`) | check-then-act on the module's state (H13) |

### 5.3 "Never written again" and other whole-program facts from flow

`FlowResultKind` (`aot/ir/schema/passes.def:26-30`), 23 kinds. Two families have to be told apart.

**Order-free results** (they say "every writer in the program writes X" or "no operation in the program ever does Y"): `ProvenLayoutSets NarrowedTypes
RemovedNullish LiteralValues ExactFunctions ProvenContents ArrayStorageKinds PristineInstances ConstantBuiltinLocations CalleeSets ReceiverSets ClassBases AppendedFields
HolderMutations OwnNames OperandSummaries ObjectUses ReachedFunctions`. A set-constraint solution does not depend on interleaving, so these hold with any number of threads
**as long as the code of every thread is part of the analysed program** and the spawn primitive is modelled (operands flow into the thread function's parameters).
- Field stability: `FieldStability { IsNeverDeleted IsNeverRedefined IsNeverStoredWithOtherType }`, `LayoutFlow.impossibleMutations`
  (`flow-records.def:22-40`), computed in `aot/valueflow/AOTValueFlowResultsFields.cpp:563-581`.
- "Never written again" for an object field (576-581): "Nothing puts a value there but the operation that allocates the object" -> `stores = AtMostOnce`,
  `isUnchangedDuringActivation = true`. Interleaving cannot break it; *publication order* can (H16).

**Order-dependent results**: `StoreCounts Initialization ExecutionCounts ClassEvaluations ConstructorFieldOrders`.
- `ExecutionCount::AtMostOnce`: `aot/valueflow/AOTValueFlowResultsExecutions.cpp:81-133`. Seeds: "A module of the image is evaluated at most once, whatever its kind"; then
  "The one operation that enters a function runs at most once if it lies outside every cycle of a function that does."
- `StoreCount::AtMostOnce` for variables and environment fields: `AOTValueFlowResultsLocations.cpp:408-433`, same argument.
- `InitializationState::StoredBeforeEveryRead`: `aot/valueflow/AOTFlowOrder*.cpp`: an order of execution computed from the call graph, module order
  (`FlowModuleOrdering::CompletesBefore`, `AOTFlowOrder.h:71-84`) and points in bodies (`FlowScopeEntry`, 88-101). It removes `absent` from the type of a read, so
  the read has no dead-zone check, and makes the location "fixed" for `EliminateRedundantReads` / stable for hoisting.
- `Occupancy { FilledAtBirth FilledWithValueAtBirth AlwaysFilled }` (`flow-records.def:6`) rests on constructor orders.

These are statements about *time* in one thread of control.

### 5.4 B3 and Air

See section 9. In short: `MemoryNumbering` (load CSE + store-to-load forwarding over dominators), `hoistLoopInvariantValues` (three times), `versionLoops` with clamped bounds,
`foldComparisonsWithBounds`, `mergeAdjacentConstantStores`, client phase `updateIntegersInMemory`, Air `pairsMemoryAccesses`. All assume one mutator.

---

## 6. Stale value or broken memory safety? (question 4)

### 6.1 Why many races are only stale values

1. **Type tests are on values.** `TypeTest` has effects `none` (`operations.def:133`); "What a cell is never changes, so no effect speaks of the words that say it"
   (`aot/codegen/AOTRuntimeCallEffects.cpp:58`). `JSCell_typeInfoType`, `Structure_classInfo`, `AOTObject_shapeWord`, `AOTClosure_entry` are immutable heaps
   (`aot/codegen/AOTHeaps.h:42-52`, `AOTHeaps.cpp:120-125`). A value that was loaded once and tested stays what it was tested to be.
2. **A forwarded or hoisted read gives one SSA value.** After `EliminateRedundantReads` both uses see the same bits, so a test of the first covers the second. Removing a
   reload is *safer* than keeping it.
3. **A slot is one aligned word of 1, 2, 4 or 8 bytes with its state in band** (`aot/object/AOTSlotRepresentation.h:55-79, 182-229`): `absent`, `undefined`, `null` are bit patterns of the same word
   (`cell32Markers { 0, 10, 2 }`, `int32Markers`, `integerMarkers`, `doubleMarkers`, `boxedMarkers`). Loads and stores are single instructions
   (`aot/codegen/AOTSlotEmitter.cpp:63-95`). There is no separate "is filled" bit to get out of step with the value, except the birth flag (6.2 B).
4. **What a slot may hold is an order-free invariant.** A typed slot is only ever written with a value of its declared type or a marker: `PutSlot` takes `value: Declared`
   (`pre:41`), the verifier "checks every operand, argument and stored value against the type of its place" (PR text), and "a store that the slot can't hold costs that one slot: it reads as empty,
   and the property moves to a side object". So *shape test, then load a `Cell32`, then use it as a proven layout without a test* is safe against races: the shape cannot change, and every word that any
   thread ever puts in that slot is a reference of the declared layout set or a marker.
5. **Markers of references fault.** `absentCell32 = 0`, `nullCell32 = 2`, `undefinedCell32 = 10` are below `minCell32`; "A null 32-bit reference decodes to the base of the cell cage"
   (`aot/image/AOTFaultHandler.cpp:127-129`), memory kept without access within `FaultSite::reach`. A marker used as a base is a fault, not a wild read. But only *registered* fault sites are turned into a
   `TypeError` (`image->faultSiteAt(pc)`, 205-207); anywhere else the process dies with SIGSEGV. That is memory-safe, not correct.

### 6.2 Where memory safety breaks

**A. Arrays (ordinary `JSArray` butterflies).** The worst group. `aot/codegen/AOTElementAccessEmitter.cpp`:

- `elementsOf(array)` loads the butterfly each time an operation is emitted (203-206). The test (`IsInBounds` / `IsElementPresent`, `branchOnPresentElement` 664-672, `testLength` 443-449) and the access are two IR
  operations, each with its own butterfly load.
- `readInsideLength()` (512-536), taken when `isInBounds() || isPresent()` (540-543): loads the butterfly again and reads `butterfly[index]` **with no comparison**; the bound is only re-checked in validating builds (519-524).
- Sequence: T1 `IsInBounds(a, i)` passes; T2 `a.length = 0` / `pop` / a reallocation to a smaller butterfly; T1 reads or, for `PutArrayElement [isInBounds]`, **writes** `newButterfly[i]` -> out-of-bounds access.
- `HasArrayStorage` then `GetArrayElement arrayStorage=Int32|Double|Contiguous`: T2 converts the storage (`ConvertToDoubleStorage`, a store of a double into an Int32 array, `ArrayStorage` transition). T1 reads raw doubles as
  boxed values or the reverse -> type confusion, forged pointers.
- `isDense()` / `findsElement()` (464-469, 475-483): no hole test; with `convertRawElement(.., typeOf(value.id()))` the element is taken to have the *proven contents type* of the array. An empty value (0) read as a cell is a null-page fault;
  a stale element of the right type is fine.
- Arrays whose storage kind and contents are proven by flow (`ClassPart.contentsAreProven`, `arrayStorage`, `provenPristine`) have no run-time test at all. The kind is order-free, the *length / butterfly pair* is not.
- Even inside one operation (`readElement`, 451-456) the order is: load butterfly, load `publicLength` from it, compare, load element from the same butterfly value. That is safe if a butterfly is never shrunk in place or freed while
  a thread holds it; `publicLength` is stored in the butterfly, so length and storage are consistent for one loaded pointer. In-place truncation followed by reuse of the tail is the open question (other reports: JSC's own array code).
- B3 makes it worse: butterfly and length loads are hoisted out of loops and the per-iteration comparison is replaced by a clamped bound (9.3).

**B. The birth flag.** One bit of the cell header vouches for the *raw* contents of several slots. `AOTGuardFacts.cpp:643`: "The mark of the cell answers for the fields that every constructor fills, and for the raw value of a
number or a boolean among them." Writer (`SlotAccessEmitter::generatePutSlot` -> `markCellIfMarkerReplacesRawValue` -> `SlotEmitter::markCellIfMarkerIsStored`, `aot/codegen/AOTSlotAccessEmitter.cpp:583-606`, `AOTSlotEmitter.cpp:510-532`):
(1) load flags byte, (2) store flags | `TypeInfoPerCellBit`, (3) store the marker in the slot. Reader: (1) load flags byte, test bit clear (`generateTestOfFlags`, `AOTGuardEmitter.cpp:254-261`), (2) later load the slot
*without markers* (`loadWithoutMarkers`, `AOTSlotEmitter.cpp:122-138`).
Race: reader (1), writer (1)(2)(3), reader (2): a marker is taken for a raw value. For `OptionalInt32` the marker is `INT32_MIN+1..3`, for `OptionalInt64` `0x4000..<<48`: a number **outside the range that the type declares**, and
integer ranges of IR types are published to B3 as trusted (`publishKnownProperties`, `aot/codegen/AOTCodeGeneratorPublication.cpp:322-343`; policy `trustsKnownRanges`, `reducesWithRanges`, `foldsImpliedComparisons`), so a bounds comparison against that
value may have been folded away. For a reference slot it is a fault (6.1.5).
Also both `FinishBirth` (`aot/codegen/AOTAllocationEmitter.cpp:200-206`) and the marking are **non-atomic read-modify-writes of a byte that also holds `IsPristine`** (`HasLayoutPrototype`, `AOTGuardEmitter.cpp:181-182`) and JSC's own inline type flags:
two threads can lose each other's bit, and a lost "marker stored" / "prototype changed" bit is permanent.

**C. `IsSlotFilled [holdsDeclaredType]` then `GetSlot [holdsDeclaredType]`.** Two loads of one word unless B3 merges them. T2 deletes the property or stores `undefined` in between: same outcome as B (marker read as raw value).
Without `holdsDeclaredType`, `GetSlot` after `SlotFilled` yields a value whose type excludes `absent`; a boxed slot then yields the empty value (0), which "has no tag bits, so it passes isCell" (`AOTGuardEmitter.cpp:51`): null-page fault later, or the
empty value stored into the heap where the engine does not expect it.

**D. Out-of-line slots.** `access.slot.isOutOfLine()` -> the slot lies in an `Extension` reached through an extension word of the cell (`extensionWordOf`, `AOTSlotAccessEmitter.cpp:40-43`); "Placement puts a slot out of line only where every PutSlot finds it filled,
so the Extension is there and holds its granule" (599); `ensureExtensionHolds..`, `storeIfExtensionHolds`, granule bits (607-615, 632-640). Load pointer, test granule bit, access: a multi-word protocol with replacement of the Extension when it grows. I did not read the
run-time half (report R1).

**E. `PutSlotOrAdd` / `PutSlots`.** (`AOTSlotAccessEmitter.cpp:618-681`) "The key is listed first, while the slot still says whether the field is missing": load slot, if absent read-modify-write the **order word** (plain load, compare with a
remembered step, plain store, `storeOrderWord` 790) or call `DidCreateField`, then store the slot. Two threads adding different fields lose one order step: count and key order in the order word no longer match the filled slots. Whether that is only a wrong `Object.keys` or
an index into a table out of range depends on the readers of the order word (R1).

**F. Order-dependent flow facts.** A read whose type lost `absent` because of `StoredBeforeEveryRead` has no check (`generateGetEnvironmentField` only validates it in validating builds, `aot/codegen/AOTBindingEmitter.cpp:135-136`). If a thread reads before the store: reference -> 0 -> fault;
raw `Int32`/`Double`/`Boolean` representation without markers -> 0, a wrong but typed value; `Int64` with a declared range that excludes 0 -> out-of-range value (as B).

**G. Typed arrays.** `IsInsideView` then `Get/PutTypedArrayElement [isInsideView]` (`pre:23, 67-68`); length, mode and vector are separate words (`JSArrayBufferView_length/_mode/_vector`, `AOTHeaps.cpp:43-46`). T2 detaches, transfers or resizes the buffer between test and access -> access to
freed or shrunk memory. (This hazard exists today only through same-thread calls, which declare `writes(TypedArrayStorage)`.)

**H. Cells converted in place.** `AOTHeaps::mayConvertCells` / `appendHeapsOfConvertedCells` (`AOTHeaps.cpp:427-439`): almost any call may rewrite `JSCell_header`, `JSObject_butterfly`, `AOTSlimClosure_word` of a cell (a slim closure becoming a full one). A multi-word rewrite of a
live cell that another thread is reading (R6).

**I. Maps, Sets, strings, RegExp state, promise state** accessed in line by built-in emitters (`aot/builtins/AOTMapSetBuiltinEmitter.cpp`, `AOTHashTableBuiltinEmitter.cpp`, `aot/codegen/AOTString*.cpp`, `AOTRegExpMatchLiteral.cpp`): table pointer + capacity + entries, rope resolution (`ResolvesRopes` *writes*
`StringStorage` from what the program sees as a read), `regExpGlobalData.cachedResult` (six words, `AOTHeaps.cpp:59-64`). Multi-word, unlocked. Memory-unsafe in general.

### 6.3 Classification of each proof under a racing writer

| Proof broken between test and use | Consequence |
| --- | --- |
| `TestedType`, `Truthiness`, `Equality`, `IntegerBound`, `ExactFunction`, `CaseValue`, `NotNullish` | cannot be broken (value facts) |
| `Shape`, `OwnInstance` | cannot be broken (immutable word) |
| `SlotFilled` (plain) | reference: fault; boxed: empty value escapes; numbers with markers: `absent` marker handled as NaN pattern / odd integer. Mostly crash-safe, not correct |
| `SlotFilled [holdsDeclaredType]`, `BirthFieldsFilled` | marker as raw value: **out-of-range integer -> possible memory unsafety through folded range checks**; reference: fault |
| `NoExtraProperties`, `NoFieldOutsideSlot` | a read answers `undefined` or the old slot value though the property now lives in the side object; a `PutSlot` refills a slot whose property has moved out (two copies). JS-level wrong value |
| `NotFrozen` | a store lands in an object that was just frozen. JS-level |
| `KeyOrder`, `EnumeratorCurrent` | stale key list; `EnumeratorGetByVal` reads by a cached position: not traced, treat as unsafe |
| `PrototypeLinkUnchanged`, `MemberUnchanged`, `EvaluatedOnce`, `UnmodifiedGenerator`, `NotThenable`, `Pristine(Names)` | the old method / old protocol answer is used once more. JS-level stale (like a stale inline cache) |
| `Pristine(Storage, Dense)`, `ArrayStorage`, `InBounds` | **memory unsafe** (6.2 A, G) |
| `PlainData` | `GetOwnDataProperty`, `CloneAs` are run-time calls (`td:35-36`): as safe as the run time |
| `SourceOpen`, `TextMatches` | about `ParserState`, a per-Instance singleton (`TypedResultDeclineReason::AnotherSourceIsOpen`, `compile-call.def:33`): **memory unsafe if two threads share it** |

---

## 7. The generic version and the entry checks (question 5)

There is no deoptimization, no OSR exit, no frame reconstruction, no invalidation. PR text: "Exits are ordinary SSA edges, and no analysis may skip the generic blocks because they're rare."

### 7.1 `CheckTypesAtEntry` (program pass, AsRead; `aot/passes/AOTCheckTypesAtEntry.cpp`)

`tests/passes/check-types-at-entry.ir:1-2`: "Whoever calls a function by value passes anything, so a parameter with a type from the source, and `this` of a method, is only expected to hold that type. The function starts with a check of each, in the order of the entry
parameters, and its body goes on with what the checks prove. It stays one function: nothing is copied and nothing throws." It inserts `Check value castType=T` (`operations.def:124`, `AsRead..Prepared`) and rewires every use of the parameter to the check (cpp:146).

### 7.2 `SplitBehindEntryChecks` (program pass, Prepared; `aot/passes/AOTSplitBehindEntryChecks.cpp`)

`tests/passes/split-behind-entry-checks.ir:1-3`: "the function becomes three. Its own id holds the tests alone. A direct caller whose arguments pass every test by their types takes the tests in, and they fold there. Where all pass it calls the body, which exists once and takes
the tested types for proven. Where one fails it calls the generic version, which takes the values as they came." The record is `CreatorRecord::EntryChecks { body, fallback }` (used at `AOTRestartInGenericVersion.cpp:27-30`); roles `FunctionRole::EntryChecks`, `Source`, .. State carried: the
arguments, one for one ("The typed body and the fallback have the parameters of the function that is split, one for one", cpp:438). Calls are tail calls between ordinary functions.

### 7.3 `AddGenericVersion` (function pass, first in `Prepare` after casts; `aot/passes/AOTAddGenericVersion.cpp`, entry 1230)

`tests/passes/add-generic-version.ir:1`: "A check that fails leaves the typed code for the operation behind it in the one generic version of the function: a copy of all that can run behind a check, in which a check and an assumed type say nothing. The copy comes back where every live value
passes a test for the type it has in the typed code."

Mechanism, in order (test f0, lines 19-40):
1. `v3 = Check v2 castType=number` becomes `t = TypeTest v2 testedType=number; Branch t -> taken passed(| 0), notTaken failed(..)`.
2. `passed(proven: number)`: `v3 = Upcast proven`, jump to the rest of the typed block.
3. `failed(unchecked)` (frequency Unlikely) jumps to the **copy of the operation behind the check**. Nothing in front of the check runs again (test f1: "The copy takes their results and runs neither again").
4. State carried: the live SSA values, as block arguments (SSA repair, cpp:1166-1203). Same frame, same registers/spill slots by ordinary register allocation.
5. Way back (test f3, lines 105-140): at the end of a turn of a loop each live value is tested for its typed type; what passes enters a block in front of the typed header, "so the copy is no part of the typed loop". In modules of the run time also behind calls
   (`add-generic-version-comes-back-behind-calls-in-modules-of-the-run-time-alone.ir`).
6. In the copy flow's stamps are dropped ("What flow has stamped on an operation was read off a run in which every check had passed", 844), but proven *positions* keep their types: "Flow follows what a check turns away through the same operations, so what it has proven of a position holds for the
   generic version too" (987). A store into a typed slot of a value that is no longer proven gets a test: "The slot of an object is written without a test" (964-985) lists the operations that need it.
7. An object that would have been sunk has to be made where the copy takes over (1234).

The rest of the file (1292 ff.) re-proves positions: a position whose proof rests on a check that may fail is "taken back" (`markExpected`, 1258-1266: sets `PositionFlags::IsExpected`).

### 7.4 `RestartInGenericVersion` (function pass; `aot/passes/AOTRestartInGenericVersion.cpp`)

Header of the test: "A test of the state of an object that fails before the body has done anything that anybody can see begins the call again there, with the arguments of the call: all such tests of a function share one block, and the generic code that the body held behind them goes."
- Applies to the typed body of an `EntryChecks` split (23-31).
- `canRunTwice(effects)`: `writes.containsOnly(heapsWhoseWritesNobodySees) && !mayRunOtherCode()` (33-37); blocks all of whose predecessors qualify (39-68).
- Only tests with `provesForStretch(proof)` ("testsStateOfObject", 71-75) or type tests that fail only for undefined/null (78-84).
- The restart block: `CallDirect genericVersion(entry parameters)` + `ReturnCall` (86-101). State carried: the entry parameters only.

With threads: the prefix's reads are repeated and may differ; nothing was written; fine.

### 7.5 `SkipAnsweredEntryChecks` (program pass; same file family)

`tests/passes/skip-answered-entry-checks.ir:1-6`: a caller whose types answer some of the tests enters "a shorter entrance, which tests the second parameter alone"; one that answers all enters "an entrance without tests, which only calls the typed body". Purely static, about argument *values*.

### 7.6 Other generic paths

`PlaceTypeTests`: "a failing test enters one generic copy, which goes on behind the stretch". `SeparateUnlikelyLoopPaths`: "A generic operation on an unlikely path of a hot loop leaves for a copy of the loop .. The copy comes back at the end of the turn". `TryCallInline .. -> done(..), miss` (`selected-operations.def:72`): inline
built-in code with a miss edge to the call. Testing switch `FailPredictions` + `ShouldFailPrediction` (`pre:155`) make tests fail at random to exercise these paths.

**Consequence for threads:** every fast path already has a correct slow path in the same function that works on any object state through the run time. A threads design can add failure causes to a test (for instance "object is shared") without new exit machinery.

---

## 8. Polls, safepoints, stack checks (question 6)

### 8.1 What is polled

One word and, on Linux x86-64, one page, **both in the `Instance`** (one per realm), addressed off the pinned `instanceGPR` (r13 on x86-64, `AOTFaultHandler.cpp:152`):

- `Instance::stackMirror`, a `StackManager::Mirror` (`aot/runtime/AOTInstance.h:104`; `runtime/StackManager.h:42-60`) with `m_trapAwareSoftStackLimit` and `m_softStackLimit`. Registered with the VM's `StackManager` in `registerStackMirror()` (`aot/runtime/AOTInstance.cpp:289-300`).
- the loop poll page at `InstanceLayout::offsetOfLoopPollPage()` (negative offset; 16 KB reserved, `aot/runtime/AOTInstanceLayout.h:28,45`), listed in `vm.aotState().loopPollPages` under a lock.

Request: `StackManager::requestStop()` (`runtime/StackManager.cpp:36-49`, `CONCURRENT_SAFE`): under `m_mirrorLock` set `m_stopsAtEveryPoll`, store `stopRequestMarker()` (= `UINTPTR_MAX`, `.h:107`) in the VM's word and in every mirror, then
`AOT::RuntimeHooks::setLoopPollsEnabled(vm, true)` = `mprotect(page, PROT_NONE)` for every page (`aot/runtime/AOTRuntimeHooks.cpp:141-152`). `cancelStop()` undoes both (51-70). `setStopsAtFunctionEntries()` (73-83) sets the marker without arming the loop pages.

### 8.2 Loop polls

IR: `operation LoopHint () [| engine isVersioned] -> none ; UsesRuntime terminates collects ; runtime = PollSlow` (`operations.def:150`). The verifier demands that every cycle of the control flow graph goes through a block with a `LoopHint` (`verifyLoopMarks`, `aot/ir/AOTVerifierDominance.cpp:129-155`, error `CycleWithoutLoopMark`); `EliminateTailRecursion` adds one
to the loop it makes (`passes/AOTEliminateTailRecursion.cpp:89`).

Code: `HintEmitter::generateLoopHint()`, `aot/codegen/AOTHintEmitter.cpp:144-150`:

```cpp
    if (!endsWithinLengthOfData(value.block()))
        emitPoll(value);
```

**No poll at all** in an innermost loop that counts up by a constant to the length of an array, string or typed array and whose likely paths `alwaysCompletes()` (102-142).

- Linux x86-64 (`hasFaultingLoopPolls`, `aot/image/AOTCodeSites.h:45-50`): a 7-byte `load32 [instance + offsetOfLoopPollPage (+4 if the function has no frame)]` into the scratch register (34-45); the site is recorded as `PollSite`. While armed the load faults; `handleLoopPoll()` (`aot/image/AOTFaultHandler.cpp:149-172`) checks the address
  against r13, pushes (for a frameless function) a frame record and the return address `pc + 7`, and sets the pc to the stub `pollAtLoop(callerFrame)`.
- Elsewhere: `branchTest64(Signed, [instance + stackMirror.trapAwareSoftStackLimit])` (the marker is negative, static assert line 30) and on a late path a call of `operationAOTPollSlow` preserving caller-saved registers (47-59).

Stub `generatePollAtLoop` (`aot/stubs/AOTStubsPolls.cpp:99-120`): prologue, `callPollSlow` (all registers that an operation clobbers are preserved), test `vm.exception`, return or jump to `handleException`.

Slow path (`aot/runtime/AOTExceptionOperations.cpp:52-58`):

```cpp
JSC_DEFINE_JIT_OPERATION(operationAOTPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollSlow);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    vm.throwTerminationExceptionIfNeeded();
```

`a-loop-that-calls-nothing-is-terminated` (`BUN/test/aot/a-loop-that-calls-nothing-is-terminated.test.ts`): a Worker spins in a compiled loop without calls; `worker.terminate()` fires the VM trap -> `requestStop()` -> the loop's next poll faults (or sees the negative word) -> `handleTraps` -> termination exception -> the stub sees
`vm.exception` and unwinds. The comment in the test records the bug it guards: on the compare-and-call platforms "that call once left the register that reports an exception as it found it".

B3 view of a poll: a patchpoint with `isLoopPoll = true`, effects from the IR row, `CellConversions::None`; comment: "Handling a trap stops the thread for the collector or ends it. It looks at no object of the program." (`AOTHintEmitter.cpp:165-173`). `B3HoistLoopInvariantValues.cpp:75,201` explicitly ignores loop polls as trap
barriers and as sideways exits, so **loads are hoisted across polls**.

### 8.3 Function entries and stack overflow

`setStackCheckGenerator()`, `aot/codegen/AOTStackCheckEmitter.cpp:71-93`; run by Air where the frame is set up.

- Small frame (bytes below the frame record <= `maxBytesAllocatedBeforeStackCheck` = 4 KB, `AOTB3Policy.cpp:32`): `branchPtr(Below, sp, [instance + stackMirror.trapAwareSoftStackLimit])` (29-33). **This one comparison is both the stack check and the entry poll**: with the marker stored every comparison fails. Failing path:
  call stub `pollAtPrologue()`, which resumes at a recorded `PrologueCheck.resumeOffset` (112-119).
- Large frame: computes the new sp and compares with `stackMirror.softStackLimit` (60-69), **not trap aware**; failing path `throwStackOverflow`.
- `generatePollAtPrologue` (`AOTStubsPolls.cpp:55-97`): first compare sp with `softStackLimit` (a real overflow -> `throwStackOverflow`), else `operationAOTPollAtPrologue` (`AOTExceptionOperations.cpp:60-72`): under `DeferGCForAWhile` ("What a caller knows about a callee that collects nothing holds for the
  callee's prologue too") `handleTraps`, termination, out-of-memory request; returns the resume address.
- `pollBeforeEntering` (122-168): for stubs that enter code through a closure word (`aot/stubs/AOTStubAssembler.cpp:627`).
- `ElideStackChecks` (`aot/passes/AOTElideStackChecks.{h,cpp}`): `StackCheck { Required, CoveredByCallers, DeferredToCallee }`. A function with `CoveredByCallers` and a frame <= `maxFrameSizeWithoutStackCheck` (240 bytes), or `DeferredToCallee`, has **no check and no entry poll** (`AOTStackCheckEmitter.cpp:76-80`); bounds in
  `aot/stubs/AOTConvention.h:62-67` (at most 8 nested unchecked functions, 4096 bytes, inside the reserved zone, `AOTB3Policy.cpp:37`).
- Cycles of tail calls: "The check is the poll of a cycle of tail calls" (`aot/codegen/AOTCallEmitterTail.cpp:109-114`), or a faulting poll without frame (`pollWhereNoFrameIsSetUp`, `AOTHintEmitter.cpp:152-163`).

The limit values come from `StackManager::setStackSoftLimit()` (`runtime/StackManager.cpp:86-102`), i.e. from the VM's one notion of "the" stack of its one mutator thread, copied to all mirrors.

### 8.4 Where the collector can act on compiled code

Every operation whose effects say `collects` (allocations' slow paths, run-time calls, polls). Frame maps / conservative scan are other reports. `RemoveRedundantBarriers` and `allocatesAfterLastSafepoint` (`AOTSchema.def:135`) rest on this list. `mutatorFence()` (`aot/codegen/AOTPrimitiveEmitter.cpp:154-167`): a B3 fence on x86-64 (compiler
barrier), on ARM64 a real fence only when `instance.mutatorShouldBeFenced` is set.

---

## 9. What B3 and Air are told (question 7)

### 9.1 Two back ends

`FunctionCodeGenerator::generateB3()` (`aot/codegen/AOTFunctionCodeGenerator.cpp:205-268`) always builds a `B3::Procedure`. If the function is not worth optimizing (`isGeneratedWithoutOptimizing`, by heat) it is emitted by `AOTDirectCodeGenerator.cpp` "in one walk over the B3 values, no B3 phase and no Air"
(263-266): loads and stores come out exactly as the emitters wrote them. Otherwise `B3::generate` under `b3PolicyFor()`.

### 9.2 Heap ranges

Yes, every IR memory operation gets a B3 abstract heap derived from its IR location. `AOTHeaps` (`aot/codegen/AOTHeaps.{h,cpp}`) extends `B3::AbstractHeapRepository`:

- one B3 heap per IR heap under `root` (`initializeEntireHeaps`, cpp:264-268); under each *indexed* IR heap 64 bucket heaps, one per mask bit (270-276); under a bucket an exact heap per location (`at()`, 375-401; for `ObjectFields` a heap per key and below it one per (key, only receiver layout)).
- upstream field heaps are re-parented: `JSCell_header` -> `ShapeWords`; `Structure_prototype` -> `PrototypeLinks`; `JSObject_butterfly`, `Butterfly_publicLength`, `Butterfly_vectorLength` -> `ArrayStorage`; `JSArrayBufferView_*`, `ArrayBuffer_data` -> `TypedArrayStorage`; `TypedArrayProperties` -> `TypedArrayElements`;
  `indexedInt32/Double/ContiguousProperties` -> `Elements[Array]`; strings -> `StringStorage`; .. (36-118). Anything unclaimed goes under `RuntimeState` (320-341).
- heaps outside the IR: `collectorState, cards, argumentBuffer, frameSlots, counters` (`.h:35-40`).
- **immutable** heaps (`B3::Mutability::Immutable`): `AOTInstance_vm`, `AOTInstance_fixedFields`, `AOTClosure_entry`, `AOTCompiledStructure_flags`, `AOTObject_shapeWord`, `ArrayBuffer_shared`, `JSArrayBuffer_impl`, enumerator words, `AOTImage_tables` (`.h:42-52`), closure captures (cpp:284-287), `JSCell_typeInfoType`,
  `Structure_classInfo`, `Structure_realm`, `ClassInfo_parentClass` (120-125), enumerator fields "fixed at creation" (127-135), `singleCharacterStrings`.
- shared heaps: "A Structure transition rewrites the cell header, an out-of-line property moves the butterfly, and a bit of the order word says 'has extra properties'" (147-153).

Calls: `RuntimeCallEmitter::applyEffects()` (`aot/codegen/AOTRuntimeCallEffects.cpp:72-117`) turns IR `Effects` into patchpoint effects: `appendHeaps(effects.reads/writes)`; + converted cells; + `collectorState`, weak tables if `mayCollect`; `reads = top` and `exitsSideways` if the call may exit sideways; a pure call has
`Effects::none()` and can be merged as a "pure call" (`MemoryNumbering::replacePureCallIfRedundant`).

So interprocedural effect summaries reach B3: a call of a compiled function that is summarised to write only `ObjectFields["x"]` leaves every other load available across it.

### 9.3 The policy

`aot/codegen/AOTB3Policy.cpp`. Switches that matter here:

| Switch | Line | Phase | Assumes one mutator? |
| --- | --- | --- | --- |
| `optimization.redundancyElimination = MemoryNumbering` | 125 | `b3/B3MemoryNumbering.cpp`, run inside `reduceStrength` | yes. A load's key is (address value, offset, epoch of its heap) (`keyForLoad`, 362-375); an immutable load has epoch 0, i.e. is merged across *everything*; a `Store` enters its value under the key of the matching load = **store-to-load forwarding** (424-428). Fenced and exotic accesses are never merged and count as a write of everything (121-130, 419-423) |
| `upstream.hoistsLoopInvariantValues = true` | 114 | `b3/B3HoistLoopInvariantValues.cpp`, up to three runs (`B3Generate.cpp:259-270`) | yes |
| `loops.hoistsImmutableLoadsPastWrites`, `hoistsOnlyFromLikelyPaths` | 151-152 | same (137, 298) | immutable heaps: safe if truly immutable |
| `loops.rotates`, `foldsComparisonsWithBounds`, `versions`, `clampsLoopBounds` | 147-150 | `B3RotateLoops`, `B3FoldComparisonsWithBounds`, `B3VersionLoops` + `B3LoopVersionPlan` (`HoistPlan`, `RangeCheckPlan`, `CountedLoop`, `rewritesLoopTest()`), `B3ClampLoopBound` | yes: a fast copy of the loop with the invariant loads of its likely path hoisted to a fast pre-header and range checks replaced by a clamped loop bound (I read the headers only) |
| `optimization.sinksValues` | 138 | `b3/B3SinkValues.cpp` | sinks only values with no reads and no writes (line 226): neutral |
| `optimization.mergesAdjacentConstantStores` | 137 | `b3/B3MergeAdjacentConstantStores.cpp` | merges initialising stores into wider ones, "in no order that anybody has promised" (87): changes store order and width |
| `knownProperties.trustsKnownRanges / trustsKnownDereferenceableBytes / trustsKnownInductions` | 271-273 | ranges and "this pointer has N readable bytes" published from IR types (`AOTCodeGeneratorPublication.cpp:322-343`) | dereferenceable bytes allow speculative loads; ranges fold comparisons |
| `reducesWithRanges`, `foldsImpliedComparisons`, `narrowsIntegers`, `threadsJumps`, `convertsBranchesToSelects` | 130-136 | | value reasoning |
| Air `peepholes.pairsMemoryAccesses`, `pairsSpillAccesses` | 245-249 | ldp/stp | a pair is two single-copy-atomic accesses, not one |
| `selection.attributesTrapsToAccesses`, `peepholes.trapReach` | 267, 252 | `b3/air/AirFoldTrapsIntoAccesses.cpp` | null tests become faults |

Client phases (`AOTFunctionCodeGenerator.cpp:174-184, 217-233`): `throwWhereTrapsAreCertain`, `mergeImpliedBranchesAfterOptimization`, `narrowTruncatedPhisAfterOptimization`, `updateIntegersInMemoryAfterOptimization` (load-op-store becomes an operation on memory, **not** atomic), `materializeSlotBasesLate`,
`guardNarrowReferences` (keeps a full pointer beside a 32-bit reference across safepoints), validators.

Every switch can be disabled per build/function (`B3::PolicySwitch`, 285-290; `BisectionSwitches.disabledBackEndSwitches`).

B3 has no dead store elimination in these phases that I saw; `eliminateCommonSubexpressions` (upstream's, which also forwards stores) is *not* run under this policy (`B3Generate.cpp:219-220`).

### 9.4 Fences and atomics today

- **No IR operation is atomic and there is no fence operation in the schema.** `grep -i atomic aot/ir/schema` finds only `enum AtomicsOperation` and `inline Atomics(AtomicsOperation)` (`inline-builtins.def:7,42`).
- `Atomics.*` are table entries (`aot/builtins/buffer.builtins.ts:140-170, 299-341`). `atomicAccess = { capability: "heap", throws: true, readsStateBeforeConversions: true, reads: [otherAgents], writes: [otherAgents] }` with `otherAgents = "ExternalState"` (line 9), plus `uses: { typedArray: [readsContents(), writesContents("element")] }`.
  Integer arrays: `implementation: b.inline("Atomics", { parameters: [operation], otherwise: b.hostCall })`; BigInt arrays, `wait`, `waitAsync`, `notify`, `pause`, `isLockFree`: `b.hostCall`.
- After `Select` the call is `CallInline inlineBuiltin=Atomics`, whose effects are the inline row (`describeInline`, `aot/ir/AOTEffectsOfBuiltins.cpp:349-365`): `Allocates reads(TypedArrayStorage, TypedArrayElements) writes(TypedArrayElements) throws`. **`ExternalState` is gone at that point**, and neither form ever writes
  `ObjectFields`, `ModuleVariables`, `Elements` ..
- Code: `BufferBuiltinEmitter::generateAtomics()` (`aot/builtins/AOTBufferBuiltinEmitterAtomics.cpp:38-105`): bounds via `findTypedArrayElement(.., miss, miss)`, then B3 `atomicXchgAdd/And/Or/Sub/Xor`, `atomicXchg`, `atomicStrongCAS`; `Load` and `Store` are plain B3 loads/stores made fenced by `m_heaps.decorateFencedAccess(&TypedArrayProperties, access)` (59)
  -> `MemoryValue::setFenceRange` (`b3/B3AbstractHeapRepository.cpp:457-458`). In B3 a fenced access is a barrier for all memory optimizations; the direct generator emits `storeFence/loadFence/memoryFence` for `B3::Fence` (`AOTDirectCodeGenerator.cpp:6096-6104`).
- Consequence: at the **IR level** an `Atomics.load`, `Atomics.store`, even `Atomics.wait()` is *not* a barrier for `GetSlot`, `GetModuleVariable`, guards: `EliminateRedundantReads`, hoisting and guard availability go straight across it. Today that is right, because only bytes of a SharedArrayBuffer are shared.
- Plain typed-array accesses to a SharedArrayBuffer are ordinary loads/stores in `TypedArrayElements`; B3 may merge and hoist them (allowed for non-atomic accesses by the JS memory model).
- Other fences: `mutatorFence` (8.4); `storeFence`/`memoryFence` in stubs for allocation, storage growth, coroutine frames and barriers (`aot/stubs/AOTStubHelpersAllocation.cpp:73`, `AOTStubHelpersStorage.cpp:135-137`, `AOTStubsCoroutineSuspension.cpp:275-280`, `AOTStubsCoroutineResumption.cpp:551-553`, `AOTStubsBarriers.cpp:32,44,143`), all for the concurrent
  collector, none for other mutators.

---

## 10. How to add things (question 8)

### 10.1 The generator

`aot/ir/generator/index.ts` reads `AOTSchema.def` + the 22 included files and writes, into the build directory (nothing is checked in):
- C++ (`engineEmitters`, `aot/ir/generator/outputs.ts:52-68`): the 44 headers/`.inc` files and 14 sources listed in `aot/ir/AOTSchemaFiles.cmake` (`AOTSchemaEnums.h`, `AOTSchemaOperations.{h,cpp}`, `AOTSchemaOperationColumns.h`, `AOTSchemaEffects.h` / `EntryEffects` / `ValueEffects`, `AOTSchemaValueAccessors.inc`, reader, printer, parser, walkers,
  verifier, result rules, runtime operations, `AOTSchemaOperationDispatch.inc`, `AOTWire*.h` ..). That cmake list is kept by hand and compared by the generator (file header).
- Rust (`rustEmitters`, 70-81): `enums.rs`, `program_enums.rs`, `entries.rs`, `records.rs`, `record_words.rs`, `record_writer.rs`, `record_references.rs`, `entry_fields.rs`, `record_text.rs`, `entry_text.rs`, `builder_<schema file>.rs`, `checks.rs`, ABI. Bun: `BUN/src/aot/build.rs`, `BUN/src/aot/interface/build.rs` (export `BUN_CODEGEN_DIR`),
  `BUN/src/aot/ir/generated.rs`, `BUN/scripts/build/aot.ts`.
- A schema hash goes into the wire header (`types.def:2`, `AOTSchemaHash.h`), so both halves must be generated from the same schema.
- `| engine` in an enum/flags/operand list separates what the front end may write (before the bar) from engine-only members; `engine operation` / `engine` operand likewise. A wire entry may not use engine-only members (`check-entries.ts:235-239`).

### 10.2 A new IR operation

1. One row in the right file, under a `family Name First..Last [; default columns]` header:
   `operation Name (operand: Kind/uses, payload: PayloadKind, engine from Placed x?: Y, with Stamp) [wireFlag | engine engineFlag] -> resultRule ; effects [by Rule] ; column = value ; ..`
   - operand kinds `AOTSchema.def:30-34`, uses 45-54 (`inspects retains(result|operand) returns calls readsContents writesContents runs(Protocol) ..`; an operand with no use is an error, `check-entries.ts:248`), payload kinds 36-43, result rules line 5 (`none constant natural(T) allocation(L) declared(x) unknown cast results(..)`).
   - effects: `none`, `reads(H[index], ..)`, `writes(..)`, `throws`, `collects`, `suspends`, `terminates`, `unknownCode`, named sets.
   - columns `AOTSchema.def:113-137`.
   - Generator checks worth knowing: legal in `AsRead` needs `flow =` (365); `proves` needs `natural(boolean)` (325); ends before `Placed` needs `removedBy` (330); `access` needs `receiver`, a store needs `stored` (280-282); `runsOwnerBarrier` needs an operand retained in another (289); `allocatesInline` needs `collects` (295);
     `replaces` may not name a later form (304).
2. Code: the dispatch calls `emitter<FamilyEmitter>().generateName(value)` (`emit-cpp-dispatch.ts:7-10`). A new family needs a class in `aot/codegen/` and a line in `FOR_EACH_AOT_OPERATION_EMITTER` (`aot/codegen/AOTEmitterList.h`).
3. If it calls the run time: a `runtime Name : result (C params) ; effects` row (`runtime-*.def`), `runtime = Name` on the operation, and `operationAOTName` in `aot/runtime/`.
4. If legal in `AsRead`: a flow rule in `aot/valueflow/AOTValueFlowRules*.cpp`, and the lowering in `BUN/src/aot/lower/` through the generated builder.
5. Representation rules (`aot/represent/AOTRepresentationRules*.cpp`), code size estimate (`passes/AOTCodeSizeEstimate.cpp`), direct generator if new B3 opcodes appear.
6. Tests: `aot/tests/**/*.ir` (text form is generated; `aot/tests/README.md`).

An **inline built-in**: `inline Name(ParamEnum) ; effects | asOverload ; family XBuiltin` in `inline-builtins.def`, `generateName(const InlineCall&)` in the family's emitter (`FOR_EACH_AOT_INLINE_BUILTIN_EMITTER`), and `implementation: b.inline("Name", ..)` in a `.builtins.ts`.

### 10.3 A new Proof

1. Add `Name(Forever|Stretch, readsOnlySubject)` to `enum Proof` (`AOTSchema.def:20-28`).
2. A test row with `proves = Name ; branches = May ; interpreted = Test` (the `Guard` family header sets the last two, `pre:10`) and **`reads(..)` of exactly the locations whose write must end it**; users get `guard?: Guard/inspects`, `relies = .., Name`, `guarded = operand`.
3. C++ that switches over all proofs: `aot/ir/AOTEdgeResults.cpp:377-416`; sets in `aot/analysis/AOTGuardFacts.cpp` (`proofsOfNew*` 33-43, `proofsThatOutlastWrites` 49, `proofsDecidedByFlow` 52-63, `locationsBehindAllocationProof` 232-253, `allocationProves` 393-420, `isProvenByType` 511-528, `provesForUser` 705-730);
   `aot/passes/AOTTypeTestConditions.*`, `AOTMethodDispatch.cpp:40-90`, `AOTVirtualObjectAccess.cpp:245`; `aot/codegen/AOTRuntimeValidationChecks.cpp` (re-check in validating builds); `aot/valueflow/AOTFlowReportAccesses.cpp:143`.
4. `GuardEmitter::generateName`.

### 10.4 A new abstract heap

1. `heap Name[IndexKinds] marks` in `AOTSchema.def:58-81`.
2. Exhaustive switches: `computeMask` and `overlapOf` (`aot/ir/AOTMemoryLocation.cpp:65-96, 146-178`), `exactKeyOf` (`aot/codegen/AOTHeaps.cpp:195-229`); a named constructor in `MemoryLocation` if indexed.
3. Limits: `numberOfHeaps <= 64` (`AOTAbstractHeap.h:23`); each *indexed* heap adds 8 bytes to `LocationMasks`, twice per `Effects`: `static_assert(sizeof(Effects) <= 136)` (`AOTEffects.cpp:302`).
4. `AOTHeaps`: the entire-heap and bucket B3 heaps come by themselves; nest field heaps with `FOR_EACH_AOT_MUTABLE_FIELD_HEAP(v(name, offset, IRHeap))` (`AOTHeaps.h:54-64`) or the tables in `AOTHeaps.cpp:36-118`.
5. `Effects::unknownCode()` is `top`, so unknown code reads and writes the new heap at once; every *declared* row and every built-in entry that touches it has to say so. `bookkeepingHeaps`, `heapsWhoseWritesNobodySees`, `compiledCodeHeaps` are hand-written sets.
6. The built-ins generator has its own vocabulary of heap names (`aot/builtins/generator/vocabulary.ts`, not read).

### 10.5 A new effect flag (for instance "synchronizes with other threads")

`Effects` bit-field + `none()`, `unknownCode()`, `merge`, `intersectedWith`, `isSubsetOf`, `interferesWith`, `dump` (`AOTEffects.h:64-160`, `AOTEffects.cpp:22-49`); `EffectTraits` has no free bit (uint8_t, 8 used); `CompactEffects::Flag` has room (6 of 12 bits, `AOTEffectSummaries.h:73-107`); every visitor of
`forEachDeclaredEffect`: `EffectsBuilder`, `EffectBoundBuilder` (`AOTEffectsDeclared.h:16-59`), `RowWithoutPayloadBuilder` (`AOTEffects.cpp:74-93`), `SingleAccessCollector` (`AOTMemoryLocationAccesses.cpp:20-44`), `ReadCollector` (`AOTGuardFacts.cpp:67-100`); the generator (`parse-entries.ts:127-128`, `resolve-entries.ts:238-244`,
`emit-cpp-effects.ts`); the built-ins table; `applyEffects` for B3. The existing flag closest in meaning is `maySuspend`: it already means "other code of the program runs here" and is handled everywhere (ends every proof, `resolveEffects` merges full `unknownCode`).

### 10.6 A new pass

1. `PassID` member in `aot/ir/schema/passes.def:1-16` (ABI file: also in Rust, used in reports, `disable=pass:X`).
2. `FOR_EACH_AOT_PROGRAM_PASS` or `FOR_EACH_AOT_FUNCTION_PASS(v(Name, functionName))` in `aot/pipeline/AOTPassList.h`.
3. One row in `rows` (`AOTPipeline.cpp:136-248`; `everyPassHasOneRow()` is a static check, 534-544): necessity, forms, inputs, outputs, invalidations, `addingOnly`/`takingAway`.
4. Function pass: `PassOutcome name(FunctionPassContext&)` (`AOTPass.h:101 ff.`) and a `ScheduleStep::run(Name)` in a schedule; optionally rows in `passesLeftIdle` / `passesThatCanWait` (408-435).
   Program pass: a member of `Pipeline`, a case in `Pipeline::runProgramPass()` (`AOTPipelineSteps.cpp:653 ff.`), a `stage.run(PassID::Name)` in `AOTPipelineStages.cpp`. A new result kind: `ProgramResult` bit (`AOTPass.h:63-79`), a member of `PipelineOutput`.
5. `disabledPasses: UInt64[4]` bounds the number of passes at 256.
6. Statistic(s) in `statistics.def`, tests `aot/tests/passes/<pass>.ir` with `# RUN: pass Name`.

### 10.7 Options, limits, statistics, diagnostics

All in ABI files (generated for both sides).
- Option: a member of a flags/enum in `compile-call.def:111-130` (`CheckSet`, `ReportSet`, `TestingSwitch`, `Instrumentation`, `AdmittedCode`, `ExcludedCode`, `OutputOptions`, `TargetOptions`) or a field of `record CompileOptions abi` (139-144; fixed layout with `size` and `reserved`). Bun: `bun_options_types::aot_options::AotOptions`,
  `BUN/src/aot/options.rs`, CLI in `BUN/src/runtime/cli/compile_options.rs`, `Arguments.rs`. Options that the run time must agree with are "recorded" (`build.recordedOptions()`, `RecordedOptions`, `ImageVerdict::OptionsConflict`, `compile-call.def:24-27`); I did not trace that path.
- Engine run-time options: `runtime/OptionsList.h` ("22 new options").
- Limit: `LimitID Name(stage, protects, degradesTo, valueOnARM64, valueOnX86_64, "purpose")` in `limits.def`; use `pass.exceeds(LimitID::X, n)` / `context.limit(..)`.
- User diagnostic: `UserDiagnosticKind Name(severity, stage, code, "message with {limit} {count} {function}")` in `diagnostics.def:4 ff.`; report `build.diagnostics().add(UserDiagnostic { .kind = .., .function = .. })` (example `AOTFunctionCodeGenerator.cpp:360`). Internal errors: `InternalErrorKind` in the same file.
- Flow result kind: `FlowResultKind` (`passes.def:26-30`), switchable with `disable=flow:X` (`disabledFlowResults: UInt64`). Selection rule: `SelectionRuleID` (18-24).
- Unproven causes and decline reasons for reports: `compile-call.def:5-60`.

---

## 11. Hazards with several mutators

Severity: **M** memory safety (type confusion, wild or out-of-bounds access), **C** deterministic crash (fault in guarded memory) or engine assertion, **S** breaks JavaScript semantics beyond what a racy program may expect (lost writes that nobody raced on, invented writes, two identities), **V** stale or
reordered value only.

| # | State / assumption | Where | Kind | Sev. |
| --- | --- | --- | --- | --- |
| H1 | `IsInBounds` / `IsElementPresent` then `Get/PutArrayElement [isInBounds|isPresent]`: butterfly reloaded, no comparison | `codegen/AOTElementAccessEmitter.cpp:512-543`, stores in `AOTElementAccessEmitterStores.cpp` | check-then-act | M |
| H2 | `HasArrayStorage` / proven `arrayStorage` then typed element access | `pre:20,59-60`; emitter 503-510 | check-then-act; indexing type and butterfly contents are two words | M |
| H3 | dense / pristine arrays: no hole test, element taken at its proven type | emitter 464-489 | "holds until a write in this thread" | C/M |
| H4 | B3 hoists butterfly, length, vector loads; versioned loops with clamped bounds | 9.3 | "no write in the loop" | M |
| H5 | `IsInsideView` then typed array access; detach / resize / transfer by another thread | `pre:23,66-68`; `AOTElementAccessEmitterTypedArrays.cpp` | check-then-act, three words | M |
| H6 | birth flag vouches for raw slot values | 6.2 B | two-word protocol (flag, slot) without ordering | M (integers), C (references) |
| H7 | non-atomic RMW of `JSCell::typeInfoFlags` (`FinishBirth`, marker mark; same byte as `IsPristine`) | `AOTAllocationEmitter.cpp:200-206`, `AOTSlotEmitter.cpp:515-519` | lost update, permanent | M |
| H8 | `IsSlotFilled` then `GetSlot`; delete / redefine / store of another type by another thread | `pre:12,34` | check-then-act on one word | C, M with `holdsDeclaredType` integers |
| H9 | order word RMW in `PutSlotOrAdd`, `PutSlots`, `TryCopyFields`, `JSONObjectFinish` | `AOTSlotAccessEmitter.cpp:646-681, 790` | lost update; order word vs slots | S, maybe M (R1) |
| H10 | Extension (out-of-line slots): pointer, granule bits, storage | `AOTSlotAccessEmitter.cpp:599-640` | multi-word | M (R1) |
| H11 | `NotFrozen`, `NoExtraProperties`, `NoFieldOutsideSlot`, `KeyOrder`, `PrototypeLinkUnchanged`, `MemberUnchanged`, `NotThenable`, `UnmodifiedGenerator` stretches | 3.4 | "holds until a write in this thread" | V/S |
| H12 | `StoredBeforeEveryRead`, `undefinedAtStartIsNeverRead`, `Occupancy::AlwaysFilled/FilledAtBirth` from constructor orders | `valueflow/AOTFlowOrder*.cpp`, `flow-records.def:3,6` | order of one thread of control | C/M (6.2 F) |
| H13 | `ExecutionCount::AtMostOnce`, `StoreCount::AtMostOnce`, `ClassFlow.evaluations`; run-time side `BeginModuleEvaluation`, `ValidateFirstRun` | `AOTValueFlowResultsExecutions.cpp:81-133`, `pre:133,153` | check-then-act: two threads that `require`/import the same module at once could both run its body; all facts derived from "once" (promoted environments, constant variables, fixed read classes, deferred definitions) fall | S/M |
| H14 | one free-cell word per environment layout in the statics | `AOTEnvironmentEmitter.cpp:74-94` | unlocked pop/push: two activations get the same cell | S (same layout, so typed) |
| H15 | lazily made things: `GetDeferredClosureSlot`, `MakeBorrowedObject`, `DeferInitializers` reads, `FunctionAsZero` variables, singleton closures, `SharedRegExpLiteral`, `String`/`BigInt`/`Builtin` constants (`CreatesConstant`), `NewTemplateObject` (`boundTo = Site`), site caches | `pre:39-40,47,95`; `AOTBindingEmitter.cpp:82-92` | check-then-make-then-publish | S (two identities); M if publication is not ordered on ARM64 |
| H16 | publication of a new object: fields, shape word, header are plain stores; the reference is then stored with a plain store | all allocation emitters; `mergeAdjacentConstantStores` reorders initialising stores | no release/acquire; on ARM64 another thread can see the reference before the contents | M on weakly ordered CPUs (zeroed or recycled memory read as slots; shape word 0 fails tests, but proven types have no test) |
| H17 | barrier removal: "no safepoint of this thread since the allocation / until the covering barrier" | `represent/AOTRemoveRedundantBarriers.cpp:77-90,113-159` | holds only if collector phase changes require every mutator at a safepoint; an owner published to another thread is already treated as escaped | M (collector) |
| H18 | per-Instance singletons that compiled code reads and writes in line: `stackMirror`, poll page, allocators (`AllocatorState`), `runtime.exceptions.*` (`thrownValue`, `stackPointerOfHandler`), argument buffer, string builder state, `ParserState`, literal string cache, site caches, global bindings, `coroutines.frameThatMayContinue`, `awaitMustSuspend`, counters | `AOTHeaps.h:35-64`, `aot/runtime/AOTInstance.h`, `AOTExceptionOperations.cpp:90`, `AOTRuntimeHooks.cpp:120-122` | shared mutable state behind a pinned register | M |
| H19 | `RegExpState`: `lastIndex`, `regExpGlobalData.cachedResult` (6 words), shared RegExp literals | `AOTHeaps.cpp:55-64`, `passes/AOTShareRegExpLiterals.cpp` | multi-word | M (indexes vs string) |
| H20 | rope resolution, `StringImpl` hash and flags, atomization: writes hidden behind reads (`heapsWhoseWritesNobodySees`) | `AOTAbstractHeap.h:44`, `ResolvesRopes` | a "read" of a shared string writes three fibers | M |
| H21 | Map/Set tables, weak tables, promise state, internal fields, generator state (`MarkGeneratorRunning`, `TakeGeneratorStepStatus`) in line | `pre:108-118`, `int:54-56` | multi-word, check-then-act ("is running") | M |
| H22 | `EliminateRedundantReads`, `HoistLoopInvariantReads`, B3 `MemoryNumbering`/LICM on variables and fields | 5.1, 9.3 | stale values; a spin loop on a plain variable or field never ends | V (liveness!) |
| H23 | `delayStoresIn`: store on every leaving edge | `AOTCarryVariablesAroundLoops.cpp:690-799` | invented write of an old value | S |
| H24 | `updateIntegersInMemory`, `x++` on fields | `codegen/AOTIntegerUpdatesInMemory.cpp` | non-atomic RMW | V (ordinary lost update of a racy program) |
| H25 | synchronisation operations are not IR barriers (`Atomics.*`, `Atomics.wait`, any future lock) | 9.4 | reads and proofs cross them | breaks every correctly synchronised program: S |
| H26 | loops without any poll; functions without entry check; large frames not trap aware | 8.2, 8.3 | time to safepoint is bounded only by data length | liveness of stop-the-world |
| H27 | one stack limit per VM mirrored into each Instance; faulting poll page per Instance; fault handler reads `Image::current()` and r13 (its scratch word `s_scratchRegisterBeforeFault` is already `thread_local`, `AOTFaultHandler.cpp:74`) | `StackManager.cpp:86-102`, `AOTFaultHandler.cpp:149-172, 193-227` | per-thread stacks need per-thread limits; arming the one page of an Instance stops every thread that uses that Instance, and none can be stopped alone | M (stack overflow unchecked against the wrong limit) |
| H28 | cells converted in place by "almost any call" | `AOTHeaps.cpp:427-439` | multi-word rewrite of a live cell | M (R6) |
| H29 | guard bits in the statics (`AreMembersUnchanged`): compiled code loads a byte and tests a mask; the bits are armed at class creation and changed by run-time watchpoints (`ClassDefinition::armGuards`, `aot/object/AOTClassCreation.cpp:443-455`; `MemberGuardWatchpoints`, `aot/object/AOTMemberGuards.*`, writer not read) | `AOTGuardEmitter.cpp:405-412`, `AOTStaticsEmitter.cpp:88-97` | several bits share a byte: if writers use a plain read-modify-write, two threads can lose an update | a guard that should have failed passes for ever: S |
| H30 | `effectsOfUnknownCodeAlone`: unknown code cannot write `compiledCodeHeaps` with `WriterState::AllKnown` | `AOTEffectSummariesSolver.cpp:680-702` | true for other threads too only in the sense "some known store writes it"; *when* is no longer bounded by calls | V |
| H31 | `DeferGCForAWhile` in the prologue poll: "What a caller knows about a callee that collects nothing holds for the callee's prologue too" | `AOTExceptionOperations.cpp:63-64` | a stop request served at a prologue must not collect; a stop-the-world for another purpose at that point sees a caller that holds unrooted/unbarriered state | collector design |

---

## 12. What helps

1. **No code patching, no invalidation, no OSR, no profiles.** Every assumption is either a build-time theorem or a test in the instruction stream with an SSA edge to generic code in the same frame (section 7). Nothing has to be stopped and rewritten when an assumption dies.
2. **The shape word is immutable** (3.6) and is an immutable heap in B3. `delete`, redefinition, a store of another type cost one slot, not the shape. The central JIT-with-threads problem (structure transition racing with a cached offset) does not exist for compiled objects.
3. **One word per slot with in-band markers** (6.1.3), naturally aligned, single instruction. No torn values, no separate presence bit (except H6).
4. **What a typed slot can hold is an invariant of every store in the program**, checked by the verifier, independent of interleaving (6.1.4). Likewise all order-free flow results (5.3), because the program is closed (`ImageFlag::AdmitsUnknownJavaScript` clear -> text is refused).
5. **References below `minCell32` and null bases fault** in reserved memory (6.1.5).
6. **One funnel for "what can undo it".** Dependencies = declared reads of the test; enders = declared/summarised writes. `mayEndProofOver` / `KillQuery::writtenOf` (guards), `findClobbers`/`addEffects` (hoisting), `Writer` in `EliminateRedundantReads`, `look()` in `delayStoresIn`, `interferesWith`, `applyEffects` -> B3. All read `Effects`.
   Changing what an operation *declares* changes all of them and B3 together, and the verifier (`GuardNotAvailable`) plus validating builds (`CheckSet::Validate`, `TestingSwitch::CheckProofsAtRunTime`, `validatesOrChecksProofs()` re-checks bounds, storage, holes at each use) check the result.
7. **There is already a notion of "other code of the program runs here": `maySuspend`.** It ends every stretch proof, clobbers all reads (`resolveEffects`: "While a frame is suspended its callers run on"), forbids delayed stores. A synchronisation operation declared `suspends`-like gets correct treatment in every IR pass today.
8. **Confinement is tracked.** `comesFromOutside`, `EscapeAnalysis`, `UseEffectHistory`, `AllocationEscapeReason`, `FrameLocality`: facts about objects that have not escaped the activation survive any number of threads (`SinkAllocations`, `AllocateInFrames`, allocation proofs, young-owner barriers, which already give up at escape).
9. **A whole-program bound on what any code may write** exists (`summaries.unknownCode()`, per-function summaries, `WriterState`, `impossibleMutations`). The same machinery can compute "what code reachable from a thread entry may write"; locations outside that set keep today's optimizations even on shared objects. Example: a layout with
   `impossibleMutations ⊇ { DeleteField, RedefineProperty, StoreOfOtherType, ChangeIntegrity, AddProperty }` needs no `SlotFilled` / `NotFrozen` test at all, in any thread.
10. **Polls are IR operations with declared effects** (`LoopHint`, `PollSlow`). If rare, dangerous mutations (array storage conversion or shrink, Extension replacement, cell conversion) were done only while all other threads stand at safepoints, it would be enough to let every safepoint operation declare `writes(ArrayStorage, ..)`: proofs and B3 loads would then end at
    polls, allocation slow paths and calls, and hold between them. The set of safepoints is exactly `mayCollect || mayRunOtherCode` (`reachesSafepoint`).
11. **Monotone state bits** (3.7): several object and class states are lost once and never regained, so a stale "lost" is impossible and a stale "still holds" is equivalent to the reader having run slightly earlier.
12. **Everything can be switched off piecemeal**: `Optional` passes, each `FlowResultKind`, each selection rule, each B3/Air switch, per build or per function range (`BisectionSwitches`). A first threads build can disable `StoreCounts`, `Initialization`, `ExecutionCounts`, `EliminateRedundantReads`, `HoistLoopInvariantReads`, `SeparateUnlikelyLoopPaths`,
    `ReuseEnvironments`, `Defer*`, B3 `MemoryNumbering`/hoisting/versioning and measure. `PassNecessity::Required` marks what cannot go.
13. **The direct code generator** does no memory optimization at all (most functions of a large program).
14. **B3 already understands fences and atomics** (fenced loads/stores, `AtomicXchg*`, `AtomicStrongCAS`, `Fence`), in `MemoryNumbering`, LICM and the direct generator; `Atomics` on typed arrays exercises them.
15. **Guards are function-local and cost no code**; a guard never crosses a call boundary (`opaque Guard .. functionLocal`), so no proof about mutable state is passed between functions. Across calls only *types* travel (value facts and order-free position facts).
16. **Instance-relative addressing.** All mutable run-time words are reached through one pinned register. Giving each thread its own block for the thread-private part (stack limits, poll page, allocators, exception words, argument buffer, parser/builder state) is a change of what the register points to, not of the generated code's shape; module variables,
    guard bits and caches would need a second base or an indirection.

---

## 13. Not found / not sure

- **`NotNullish`, `EvaluatedOnce`**: no operation has them in `proves`. I saw them only as requirements/conditions; I did not trace how a plan's `NotNullish` is discharged (presumably a `TypeTest`).
- **Does a suspension end a `Shape` guard?** `writtenOf` returns `Everywhere` for `maySuspend` before it looks at locations, but I did not check whether a fact with an empty dependency list gets a kill class at all (`internKillClass`, `collectKills`).
- **`GuardAvailability::solve`, `AOTGuardMeet.cpp`, `AOTFailedTests.cpp`**: read only in outline.
- **`EliminateRedundantReads`**: read to line 470 of 1382; the clearing logic (`Writer`, `OperandWrites`, handler rows) only from its data structures and the test header.
- **`SinkAllocations`, `AllocateInFrames`, `FlattenInnerObjects`, `AllocateInnerObjects`, `DeferInitializers`, `DeferDefinitions`, `PromoteEnvironmentsToVariables`, `PlanArrayCapacities`, `FuseArrayChains`, `ExpandIteration`, `ExpandOwnKeys`, `SplitCoroutines`**: known from test headers, comments and greps, not from a full reading. In particular I did not verify what
  `PutArrayElement`, `AppendElement`, `TryPutIndex`, `HasSpareArrayCapacity`/`ReserveArrayCapacity` emit (`AOTElementAccessEmitterStores.cpp`).
- **How flow orders host callbacks** (timers, promise jobs, and what a thread entry would look like): `FlowCall` with `caller == host()` exists (`AOTFlowOrder.h:28-35`), `isBodyThatOnlyStartOfProgramEnters`; I did not read `AOTFlowOrder*.cpp`. Whether a callback handed to a native function is assumed to run only after the current module body has completed decides whether
  H12 bites for a spawn primitive modelled as "native function that calls its operand".
- **B3 loop versioning and bound clamping**: headers and the driver only (`B3VersionLoops.cpp:40-70`, `B3LoopVersionPlan.h`, `B3ClampLoopBound.h`). I did not read `B3LoopVersionPlan.cpp`, `B3FoldComparisonsWithBounds.cpp`, `B3RotateLoops.cpp`, nor how "excursions" re-validate hoisted loads.
- **Whether a folded range check can really be reached with a marker value (H6/H8 "M")**: the two halves exist (markers outside declared ranges; ranges trusted by B3; `typeHoldsWhereverComputed` filters some publications, not read). I did not construct a concrete path.
- **In-place truncation of butterflies and reuse of their tails** in JSC's own array code with `useAOT` ("Dense arrays that stay dense in a VM with compiled code", "Arrays frozen in place"): not read.
- **Run-time half of slots** (Extension growth, side object for extra properties, `DidCreateField`, `TryFinishBirth`, order tables): not read (R1).
- **Who writes the member guard bits and how** (`aot/object/AOTMemberGuards.*`, JSC watchpoints behind them): not read; H29 is a question, not a finding.
- **Air phases** (`AirFoldTrapsIntoAccesses`, pairing, `AirReuseConditionFlags`): not read; I rely on the policy names.
- **`recordedOptions` / `RecordedOptions` and `ImageFlag`**: how a compile option is stored in the image and checked at load: not traced.
- **The built-ins generator's vocabulary** (`capability`, heap names, `UseSpec`): inferred from `buffer.builtins.ts` and from the C++ that reads the table; `aot/builtins/generator/vocabulary.ts`, `lib.ts` not read.
- **How an operation in `prepared-operations.def` is marked engine-only**: the rows have no `engine` prefix; I assume entries that start after `AsRead` never reach the wire, but did not find the rule in the generator.
- **Windows / macOS polls**: only the `#if CPU(X86_64) && OS(LINUX)` split was read; everything else takes compare-and-call.
- **`VMTraps::handleTraps` and who calls `requestStop()` for the collector** (`stopTheWorld`, `Heap::stopThePeriphery` ..): not read here.
