# Appendix A. Inventory of hazards, and index of rules

Every place that the survey (`research/`) found to assume one running thread, with the rule of the design that covers it. 954 rows.

**Method.** Each report of the survey ends with lists of "hazards with several mutators". For each hazard, and for each item in a report's body that is marked as memory unsafe, a reader who wanted to break the design looked for the rule that covers it, read the cited source lines, and thought in interleavings, on x86-64 and on ARM64. What the first draft did not close is recorded in [appendix B](appendix-b-attacks.md); the documents were then changed, and the rows below name the rules of the present text.
The inventory is to be made again, by the same method, when the compiler has moved (13, section 13.4a).

**Columns.** *ID*: report and number in the report. *Where*: the main `path:line` (conventions of the README). *Severity*: as in the report (each report defines its letters). *Rule*, *Section*: where the design answers. *Status*: `Covered`: a rule closes it. `Partly`: a rule addresses it and work remains that a work package of 13 names. "by class" in the Rule column: the member is named in no document, and a general rule (K1, D1, Z1, Z2) decides it beyond doubt.

| Part | Covered | Partly | GAP | Contradiction |
| --- | ---: | ---: | ---: | ---: |
| A.1 Compiled objects and the compiler (reports R1, R2) | 203 | 0 | 0 | 0 |
| A.2 Run-time state and control (reports R4, R6) | 230 | 3 | 0 | 0 |
| A.3 The heap and the flow analysis (reports R5, R3) | 155 | 0 | 0 | 0 |
| A.4 Ordinary objects, the interpreter, native code (reports R7, R8) | 363 | 0 | 0 | 0 |
| **Total** | **951** | **3** | **0** | **0** |

## A.1 Compiled objects and the compiler (reports R1, R2)

### R1, hazards H1 to H16

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R1 H1 | Test and use of an optional slot are two loads. A marker is taken for a value | `aot/codegen/AOTGuardEmitter.cpp:131` | JS, CRASH, perhaps MEM | C1, C3, CO2 | 04 4.5, 4.6.1; 05 5.4 | Covered |
| R1 H2-1 | A whole-word store of a remembered order word erases a flag set in between | `aot/codegen/AOTSlotAccessEmitter.cpp:790` | JS | CO5, CO8 | 05 5.6 f, i | Covered |
| R1 H2-2 | Two threads create different fields: one key is never listed | `aot/stubs/AOTStubsFieldCreation.cpp:95` | JS | CO5 | 05 5.6 f | Covered |
| R1 H2-3 | An order word that does not fit the layout's table indexes out of bounds | `aot/object/AOTObjectKeyOrder.cpp:52` | MEM | K1; lookups under the table's lock | 07 7.4, 7.7; 05 5.6 g; 04 4.10 | Covered |
| R1 H3-1 | Hoisted guards are never tested again, hoisted reads never see a store | `aot/analysis/AOTGuardFacts.cpp:90` | JS to MEM | `synchronizes`, C5, M10 | 04 4.4, 4.5, 4.12 | Covered |
| R1 H3-2 | Extension pointer and granule bits are loaded once in front of a loop | `aot/codegen/AOTSlotEmitter.cpp:369` | MEM | CO6, S1 | 05 5.5; 04 4.6.5 | Covered |
| R1 H3-3 | `stateOfNewObjectBefore` is sound only while the object is unpublished | `aot/codegen/AOTSlotAccessEmitter.cpp:896` | - | C2, unpublished | 04 4.3 | Covered |
| R1 H3-4 | Flow facts that encode an order in time; the state before is zero | `aot/ir/schema/flow-records.def:3` | CRASH, JS | O1, O2, O3 | 04 4.7 | Covered |
| R1 H4-1 | Granules move in place while another thread computes a popcount address | `aot/object/AOTObjectStorage.cpp:300` | MEM | CO6 | 05 5.5 | Covered |
| R1 H4-2 | Two appends lose a granule bit: two fields alias one word | `aot/stubs/AOTStubHelpersValues.cpp:152` | MEM | CO6 | 05 5.5 | Covered |
| R1 H4-3 | Replacement loses stores. An untested `PutSlot` indexes with bits that lack its granule | `aot/object/AOTObjectStorage.cpp:199` | JS, MEM | CO6 | 05 5.5 | Covered |
| R1 H4-4 | An untested `PutSlot` writes to the page of zeros | `aot/codegen/AOTSlotAccessEmitter.cpp:599` | CRASH | CO6, `EnsureExtension` | 05 5.5; 04 4.6.5 | Covered |
| R1 H4-5 | ARM64: the hidden word is seen before the Extension's contents | `aot/codegen/AOTPrimitiveEmitter.cpp:154` | MEM | CO6 (release CAS), F2 | 05 5.5; 04 4.10 | Covered |
| R1 H5-1 | `AllocationSiteRecord`: allocator visible before the header | `aot/object/AOTAllocationSites.cpp:151` | MEM | K1 | 07 7.4 | Covered |
| R1 H5-2 | `PrototypeTransition` torn: an object gets a Structure of another layout | `aot/object/AOTStructureTable.cpp:565` | MEM | K1 | 07 7.4; 05 5.7 | Covered |
| R1 H5-3 | `KeyOrderRun`, `KeyOrderStepCache`, `KeyOrderTransition` torn: wrong order word | `aot/stubs/AOTStubsFieldCreation.cpp:245` | MEM | K1 | 07 7.4 | Covered |
| R1 H5-4 | `keyOrderTransitions` pointer and mask are two words; the old table is released | `aot/object/AOTKeyOrderTable.cpp:766` | MEM | K1 | 07 7.4; 08 8.9 | Covered |
| R1 H5-5 | Key list, enumerator, prototype place, `CachedField`, plan entries: key of one, value of another | `aot/object/AOTRuntimeLayoutTable.h:94` | JS, MEM | K1 | 07 7.4 | Covered |
| R1 H5-6 | `setListedFields`: pointer, size, tag stored apart | `aot/object/AOTCompiledStructure.h:157` | MEM | one-pointer record | 05 5.13 | Covered |
| R1 H5-7 | Tables zeroed at each collection, phase unknown | `aot/runtime/AOTHeapCacheRegistry.h:48` | not sure | while stopped | 08 8.12 | Covered |
| R1 H6-1 | The stub publishes both StructureIDs before the value of an added property | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:110` | CRASH | CO9 | 05 5.6 e | Covered |
| R1 H6-2 | The side object is an ordinary object: growth, transitions, dictionary without a lock | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:187` | MEM | L3, L6, N1 to N3, SW5 | 05 5.3, 5.6; 06 6.4; 08 8.6 | Covered |
| R1 H6-3 | Nothing ties hidden word, butterfly and index to the StructureID check | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:80` | MEM | CO9, F3, N1 | 05 5.2; 06 6.4 | Covered |
| R1 H7-1 | `setStructure` rewrites the flags byte: the mark of the cell is lost | `runtime/JSCellInlines.h:259` | JS to MEM | CO5, CO15 | 05 5.2; 04 4.5, 4.8 | Covered |
| R1 H7-2 | `FinishBirth` clears a mark that another thread has just set | `aot/codegen/AOTAllocationEmitter.cpp:200` | JS to MEM | CO15 | 05 5.2; 04 4.8 | Covered |
| R1 H7-3 | `IsPristine` lost: methods of the layout's prototype on another prototype | `aot/codegen/AOTAccessEmitterDefinitions.cpp:125` | JS | CO5, CO8 | 05 5.7 | Covered |
| R1 H7-4 | New StructureID with old flags is visible | `aot/codegen/AOTGuardEmitter.cpp:181` | JS | flags first | 05 5.7 | Covered |
| R1 H8-1 | Slim closure reified in place: C++ tests the type, then reads the word | `aot/object/AOTSlimClosure.cpp:178` | MEM | C1, `loadedWord()` | 05 5.8 | Covered |
| R1 H8-2 | `ClassPrototype` becomes a `JSFinalObject`: private words become property slots | `aot/object/AOTClassPrototype.cpp:48` | MEM | one safepoint operation for conversion and members; CO10 | 05 5.10; 08 8.6 | Covered |
| R1 H8-3 | Lazy class object, lazy `prototype`, `name`, `length`, rare data | `aot/object/AOTClosure.h:21` | MEM, JS | Z1, Z2, D1 | 07 7.6; 06 6.10 | Covered |
| R1 H9-1 | Two threads both see "not made" and both make: two identities, release assertions | `aot/object/AOTDeferredClosures.cpp:78` | JS, CRASH | Z1 with restart, Z2, C3 | 07 7.6; 05 5.4, 5.13 | Covered |
| R1 H9-2 | `ensureExtraProperties` twice: one side object is overwritten | `aot/object/AOTObjectExtraProperties.cpp:644` | JS | CO7 | 05 5.6 a | Covered |
| R1 H9-3 | `makeFlattenedObject` zeroes the room under a reader that has chosen it | `aot/object/AOTDeferredClosures.cpp:43` | JS, CRASH | Confined only | 04 4.8 | Covered |
| R1 H9-4 | Hash maps, vectors, arenas, plan caches behind lazy creation | `aot/object/AOTObjectModel.h:24` | MEM | K1, table locks | 07 7.4, 7.7 | Covered |
| R1 H10 | ARM64: a new cell's words are not ordered before the store that publishes it | `aot/codegen/AOTPrimitiveEmitter.cpp:154` | MEM | F1, F1', F2, M6 | 04 4.10 | Covered |
| R1 H11-1 | One `LocalAllocator` per record, bumped without atomics | `aot/object/AOTAllocationSiteRecord.h:17` | MEM | per-thread allocators | 08 8.8; 07 7.4 | Covered |
| R1 H11-2 | One free-cell word per environment layout: two activations take one cell | `aot/codegen/AOTAllocationEmitter.cpp:238` | MEM | private statics | 04 4.8; 07 7.3.2 | Covered |
| R1 H12-1 | Fields, order word and mark of a copy are no snapshot | `aot/object/AOTObjectAssign.cpp:136` | JS | re-read of two words; generic copy without a lock | 05 5.12 | Covered |
| R1 H12-2 | The copy gets bit 28 and the source's side object | `aot/stubs/AOTStubsObjectCopy.cpp:175` | JS | re-read of two words | 05 5.12 | Covered |
| R1 H12-3 | The source's Extension is copied while granules are inserted | `aot/object/AOTObjectAssign.cpp:74` | MEM | CO6 | 05 5.5, 5.12 | Covered |
| R1 H12-4 | 16-byte loads of a live source | `aot/stubs/AOTStubsObjectCopy.cpp:38` | none | slot inside 8 bytes | 05 5.12 | Covered |
| R1 H13-1 | Order word "out of line" paired with a Structure without a list | `aot/object/AOTObjectKeyOrder.cpp:303` | CRASH | CO5 | 05 5.6 g | Covered |
| R1 H13-2 | Each `setStructure(variantOf(current))` is a read-modify-write of the StructureID | `aot/object/AOTStructureTable.cpp:495` | JS, perhaps MEM | CO5 | 05 5.7 | Covered |
| R1 H14 | Slots do not tear. `OptionalDouble` tests the upper half with a second load | `aot/codegen/AOTSlotEmitter.cpp:475` | fine | `TryGetSlot` | 04 4.6.1 | Covered |
| R1 H15 | Reads by native code that write | `aot/object/AOTObjectStorage.cpp:108` | - | Z1, K1; one atomic `or` | 05 5.13, 5.10 | Covered |
| R1 H16 | Deliberate aborts that a race can reach | `aot/runtime/AOTFieldOperations.cpp:77` | CRASH | CO10 | 05 5.6 | Covered |

### R1, section 5: each mutation

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R1 5.1-1 | Between put and clear the property exists twice | `aot/object/AOTObjectConversion.cpp:43` | harmless | CO4 | 05 5.6 a | Covered |
| R1 5.1-2 | A reader that passed its test loads the absent marker | `aot/object/AOTObjectConversion.cpp:43` | H1 | C1, `TryGetSlot` | 04 4.6.1 | Covered |
| R1 5.1-3 | A fast writer refills a slot whose property has moved out | `aot/object/AOTObjectConversion.cpp:43` | JS | CO3, in stubs too | 04 4.6.2; 05 5.4 | Covered |
| R1 5.1-4 | Slot and mark are written in different orders on different paths | `aot/object/AOTObjectStorage.cpp:163` | H1 | CO15 | 05 5.2; 04 4.6.2 | Covered |
| R1 5.1-5 | The way back into the slot | `aot/object/AOTObjectConversion.cpp:64` | - | CO3, CO4, CO8, N3 | 05 5.6 b | Covered |
| R1 5.2 | `delete`: a listed key with an empty slot; `readListedField` may return the empty value | `aot/object/AOTObjectConversion.cpp:108` | JS | CO3, CO16 | 05 5.6 c | Covered |
| R1 5.3 | Redefinition has two halves; a late fast store hides the accessor | `aot/object/AOTObjectDefinition.cpp:122` | JS | CO3, one section | 05 5.6 a | Covered |
| R1 5.4 | `freeze` against a fast store; read-modify-write of the order word | `aot/object/AOTObjectIntegrity.cpp:63` | JS | M9, CO5 | 05 5.6 d | Covered |
| R1 5.5 | Adding an undeclared property in a stub | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:216` | H6 | CO9 | 05 5.6 e | Covered |
| R1 5.6 | Prototype transition replayed in line and in a stub, on published objects too | `aot/stubs/AOTStubsBoxedCalls.cpp:494` | JS | chain lock, CO5 | 05 5.7 | Covered |
| R1 5.7 | Nine kinds of writer of the order word; the pair with the StructureID | `aot/object/AOTObject.h:153` | H2, H13 | CO5, K1 | 05 5.6 f, g | Covered |
| R1 5.8 | Construction: 16-byte header copy, conditional `mutatorFence`, `FinishBirth` | `aot/codegen/AOTAllocationEmitter.cpp:656` | H10 | CO1, F1, F4 | 05 5.6 i; 04 4.10 | Covered |

### R1, section 10: words written without a call

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R1 10-1 | 8-bit slot store | `aot/codegen/AOTSlotEmitter.cpp:78` | - | CO2, CO3 | 04 4.6.2; 05 5.4 | Covered |
| R1 10-2 | 16-bit slot store (no markers) | `aot/codegen/AOTSlotEmitter.cpp:78` | - | CO2 | 04 4.6.2 | Covered |
| R1 10-3 | 32-bit slot store, `DeferredClosure` and `LiteralString32` among them | `aot/codegen/AOTSlotAccessEmitter.cpp:563` | - | CO2, CO3, C3, F1, Z1 | 04 4.6.1, 4.6.2; 05 5.4 | Covered |
| R1 10-4 | `Float` store | `aot/codegen/AOTSlotEmitter.cpp:179` | - | CO2 | 04 4.6.2 | Covered |
| R1 10-5 | `Double`, `OptionalDouble` store | `aot/codegen/AOTSlotEmitter.cpp:183` | - | CO2, CO3 | 05 5.4 | Covered |
| R1 10-6 | 64-bit integer, cell and boxed stores | `aot/codegen/AOTSlotEmitter.cpp:78` | - | CO2, CO3, F1 | 04 4.6.2, 4.10 | Covered |
| R1 10-7 | Absent marker stored in line (allocation only) | `aot/codegen/AOTSlotEmitter.cpp:197` | - | CO3, unpublished | 05 5.6 i | Covered |
| R1 10-8 | Slot in the Extension at a computed address | `aot/codegen/AOTSlotEmitter.cpp:549` | - | CO6 | 05 5.5 | Covered |
| R1 10-9 | `granuleBits` load, or, store | `aot/stubs/AOTStubHelpersValues.cpp:152` | - | CO6 | 05 5.5 | Covered |
| R1 10-10 | Granules moved in place | `aot/stubs/AOTStubsAllocation.cpp:431` | - | CO6 | 05 5.5 | Covered |
| R1 10-11 | Hidden word of the Extension | `aot/stubs/AOTStubsAllocation.cpp:350` | - | CO6 | 05 5.5 | Covered |
| R1 10-12 | Hidden word of the extra properties | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:175` | - | CO7, CO9, F3 | 05 5.6 a; 04 4.10 | Covered |
| R1 10-13 | Order word, whole | `aot/stubs/AOTStubsFieldCreation.cpp:225` | - | CO5 | 05 5.6 f, i | Covered |
| R1 10-14 | Order word, count + 1 | `aot/stubs/AOTStubsFieldCreation.cpp:95` | - | CO5 | 05 5.6 f | Covered |
| R1 10-15 | Order word, flags kept | `aot/stubs/AOTStubsFieldCreation.cpp:181` | - | CO5 | 05 5.6 f | Covered |
| R1 10-16 | Order word, bit 28 | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:182` | - | CO5, CO8 | 05 5.6 a | Covered |
| R1 10-17 | Flags byte: set the mark | `aot/codegen/AOTSlotEmitter.cpp:515` | - | CO15 | 05 5.2; 04 4.6.2 | Covered |
| R1 10-18 | Flags byte: clear the mark | `aot/codegen/AOTAllocationEmitter.cpp:200` | - | CO15 | 05 5.2; 04 4.8 | Covered |
| R1 10-19 | Flags byte: set `IsPristine` | `aot/stubs/AOTStubsBoxedCalls.cpp:505` | - | CO5, CO8 | 05 5.7 | Covered |
| R1 10-20 | StructureID of a compiled object | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:104` | - | CO5, CO9 | 05 5.6, 5.7 | Covered |
| R1 10-21 | StructureID, value and butterfly of the side object | `aot/stubs/AOTStubsPropertyExtraProperties.cpp:110` | - | L3, L6, SW5, SW7 | 05 5.6 e; 06 6.4 | Covered |
| R1 10-22 | 128-bit store of header, order and shape of a new cell | `aot/codegen/AOTCellAllocationEmitter.cpp:188` | - | CO1 | 05 5.6 i | Covered |
| R1 10-23 | Copy of a whole object's fields in 128-bit pieces | `aot/stubs/AOTStubsObjectCopy.cpp:38` | - | re-read of two words | 05 5.12 | Covered |
| R1 10-24 | `cellState` of a frame cell | `aot/codegen/AOTAllocationEmitter.cpp:304` | - | C2 | 04 4.3 | Covered |
| R1 10-25 | Environment variable | `aot/ir/schema/operations.def:42` | - | F1 | 05 5.9 | Covered |
| R1 10-26 | "Escaped" byte of an environment | `aot/codegen/AOTEnvironmentEmitter.cpp:71` | - | not retained | 04 4.8 | Covered |
| R1 10-27 | Free-cell word of a reused environment layout | `aot/codegen/AOTEnvironmentEmitter.cpp:91` | - | private statics | 04 4.8 | Covered |
| R1 10-28 | Room of a flattened object | `aot/codegen/AOTAllocationEmitter.cpp:341` | - | Confined only | 04 4.8 | Covered |
| R1 10-29 | `KeyOrderRun`, `KeyOrderStepCache` entries | `aot/stubs/AOTStubsFieldCreation.cpp:283` | - | K1 | 07 7.4 | Covered |
| R1 10-30 | `storesToGuardedFields` counter | `aot/stubs/AOTStubsKnownFieldStores.cpp:328` | - | private | 07 7.3.3, 7.8 | Covered |
| R1 10-31 | Free lists of `LocalAllocator`s | `jit/AssemblyHelpers.cpp:1070` | - | per thread | 08 8.8 | Covered |
| R1 10-32 | Barrier: cell state, mark stack, stored-cell log, cards | `aot/stubs/AOTStubsBarriers.cpp:40` | - | per thread | 08 8.2, 8.10 | Covered |

### R2, hazards H1 to H31

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R2 H1 | Bounds test, then element access with a reloaded butterfly and no comparison | `aot/codegen/AOTElementAccessEmitter.cpp:512` | M | S1, SW2, SW3 | 04 4.6.3; 06 6.2 | Covered |
| R2 H2 | Storage kind tested or proven, then a typed element access | `aot/ir/schema/prepared-operations.def:20` | M | SW1 | 04 4.6.3 | Covered |
| R2 H3 | Dense and pristine arrays: no hole test | `aot/codegen/AOTElementAccessEmitter.cpp:464` | C/M | SW4, `TryGetElement`, 64-bit load | 04 4.6.3; 06 6.3.1 | Covered |
| R2 H4 | B3 hoists butterfly, length and vector loads; versioned loops | `aot/codegen/AOTB3Policy.cpp:147` | M | C4 | 04 4.9 | Covered |
| R2 H5 | `IsInsideView`, then access; detach, resize, transfer elsewhere | `aot/ir/schema/prepared-operations.def:23` | M | TA1, TA2, TA5 | 04 4.6.4; 06 6.8 | Covered |
| R2 H6 | The birth flag vouches for raw slot values | `aot/analysis/AOTGuardFacts.cpp:643` | M, C | not used | 04 4.5 | Covered |
| R2 H7 | Read-modify-write of the flags byte that also holds `IsPristine` | `aot/codegen/AOTSlotEmitter.cpp:515` | M | CO5 | 05 5.2; 04 4.8 | Covered |
| R2 H8 | `IsSlotFilled`, then `GetSlot` | `aot/ir/schema/prepared-operations.def:12` | C, M | C1, C3 | 04 4.6.1 | Covered |
| R2 H9 | Order word read-modify-write in `PutSlotOrAdd`, `PutSlots`, `TryCopyFields`, `JSONObjectFinish` | `aot/codegen/AOTSlotAccessEmitter.cpp:646` | S, maybe M | CO5 | 04 4.6.2; 05 5.6 f | Covered |
| R2 H10 | Extension: pointer, granule bits, storage | `aot/codegen/AOTSlotAccessEmitter.cpp:599` | M | CO6 | 05 5.5 | Covered |
| R2 H11 | Stretches of `NotFrozen`, `NoExtraProperties`, `NoFieldOutsideSlot`, `KeyOrder`, `MemberUnchanged` and others | `aot/ir/AOTSchema.def:19` | V/S | end at `synchronizes`; CO11 to CO13, F1a, `TryGetSlotOrUndefined` | 04 4.5, 4.6.1; 05 5.14; 07 7.5 | Covered |
| R2 H12 | `StoredBeforeEveryRead`, `undefinedAtStartIsNeverRead`, occupancy from constructor orders | `aot/valueflow/AOTFlowOrderLocations.cpp:110` | C/M | O1, O2, O3 | 04 4.7 | Covered |
| R2 H13 | "At most once": two threads evaluate one module | `aot/valueflow/AOTValueFlowResultsExecutions.cpp:81` | S/M | MD1 to MD7 | 04 4.7; 09 9.9 | Covered |
| R2 H14 | One free-cell word per environment layout | `aot/codegen/AOTEnvironmentEmitter.cpp:74` | S | private statics | 04 4.8 | Covered |
| R2 H15 | Lazily made things: check, make, publish | `aot/codegen/AOTBindingEmitter.cpp:82` | S, M | Z1 with restart, Z2, S5 | 07 7.6; 04 4.8 | Covered |
| R2 H16 | A new object is published by a plain store | `b3/B3MergeAdjacentConstantStores.cpp:87` | M | F1, F1', F2, M6 | 04 4.10 | Covered |
| R2 H17 | Barrier removal: "no safepoint of this thread since" | `aot/represent/AOTRemoveRedundantBarriers.cpp:77` | M | S2 | 04 4.8; 08 8.10 | Covered |
| R2 H18 | Per-Instance singletons read and written in line | `aot/runtime/AOTInstance.h:104` | M | private regions | 07 7.3.2, 7.3.3 | Covered |
| R2 H19 | `lastIndex`, cached result of six words, shared RegExp literals | `aot/passes/AOTShareRegExpLiterals.cpp` | M | per thread | 06 6.9; 04 4.8 | Covered |
| R2 H20 | Rope resolution, hash and flags, atomization behind reads | `aot/ir/AOTAbstractHeap.h:44` | M | CAS of `m_fiber` | 06 6.7 | Covered |
| R2 H21 | Map and Set tables, promise state, generator state in line | `aot/ir/schema/prepared-operations.def:108` | M | MS1 to MS9, PR1 to PR7, G1 | 06 6.6; 09 9.6, 9.7 | Covered |
| R2 H22 | Redundant read elimination, hoisting, B3 numbering: a spin loop never ends | `aot/passes/AOTHoistLoopInvariantReads.cpp:149` | V (liveness) | M10, `PinLoopExitReads` | 04 4.12; 02 2.10.4 | Covered |
| R2 H23 | `delayStoresIn` invents a write | `aot/passes/AOTCarryVariablesAroundLoops.cpp:690` | S | off for shared | 04 4.8 | Covered |
| R2 H24 | `updateIntegersInMemory`, `x++` on fields | `aot/codegen/AOTIntegerUpdatesInMemory.cpp` | V | allowed | 04 4.9; 02 2.10.5 | Covered |
| R2 H25 | Synchronization operations are no barriers in the IR | `aot/ir/schema/inline-builtins.def:42` | S | `synchronizes`, C5, E2a | 04 4.4 | Covered |
| R2 H26 | Loops without a poll, entries without a check | `aot/codegen/AOTHintEmitter.cpp:102` | liveness | S3 | 04 4.11 | Covered |
| R2 H27 | One stack limit and one poll page per Instance | `runtime/StackManager.cpp:86` | M | per thread | 09 9.4, 9.5; 07 7.3.2 | Covered |
| R2 H28 | Cells converted in place by "almost any call" | `aot/codegen/AOTHeaps.cpp:427` | M | C1, CO10, safepoint operation | 05 5.8, 5.10 | Covered |
| R2 H29 | Guard bits share a byte | `aot/codegen/AOTGuardEmitter.cpp:405` | S | E1, F1a, CO11 to CO13 | 07 7.5; 05 5.14 | Covered |
| R2 H30 | Unknown code cannot write `compiledCodeHeaps`; "when" is no longer bounded by calls | `aot/ir/AOTEffectSummariesSolver.cpp:680` | V | `effectsOfOtherThreads` | 04 4.4 | Covered |
| R2 H31 | The prologue poll must not collect | `aot/runtime/AOTExceptionOperations.cpp:63` | collector | S2, `MarkRecursiveEntries` | 04 4.11; 08 8.4 | Covered |

### R2, section 5: passes

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R2 5.1-1 | `EliminateRedundantReads`: forwarding; fixed classes that no call clears | `aot/passes/AOTEliminateRedundantReads.cpp:338` | broken | `synchronizes`, O1 | 04 4.4, 4.8 | Covered |
| R2 5.1-2 | `HoistLoopInvariantReads` | `aot/passes/AOTHoistLoopInvariantReads.cpp:149` | broken | M10 | 04 4.8, 4.12 | Covered |
| R2 5.1-3 | `carryVariablesAroundLoops`, `delayStoresIn` | `aot/passes/AOTCarryVariablesAroundLoops.cpp:690` | broken, invents a write | off for shared | 04 4.8 | Covered |
| R2 5.1-4 | `EliminateGuards` | `aot/passes/AOTTypeTestElimination.cpp:1558` | broken | guard availability | 04 4.5, 4.8 | Covered |
| R2 5.1-5 | `PlaceTypeTests` and its hoister | `aot/passes/AOTPlaceTypeTestsHoisting.cpp:88` | broken | guard availability; M10 | 04 4.5, 4.12 | Covered |
| R2 5.1-6 | Passes that move or fold tests of SSA values | `aot/passes/AOTControlFlowRules.cpp:116` | not broken | C1 | 04 4.1; 02 2.10.6 | Covered |
| R2 5.1-7 | `ResolveAccesses` and the other makers of test-and-use pairs | `aot/analysis/AOTPropertyAccess.cpp` | birthplace | `mayBeShared`, C3 | 04 4.8, 4.6.1 | Covered |
| R2 5.1-8 | `SinkAllocations` | `aot/passes/AOTSinkAllocations.cpp` | confined | unpublished | 04 4.3, 4.8 | Covered |
| R2 5.1-9 | `EliminateDeadCode` | `aot/passes/AOTEliminateDeadCode.cpp:175` | confined | unpublished | 04 4.8 | Covered |
| R2 5.1-10 | Passes that move an operation where `interferesWith` allows | `aot/ir/AOTEffects.h:93` | reorders | `interferesWith`, C5 | 04 4.4 | Covered |
| R2 5.1-11 | `RemoveDeadCountedLoops` | `aot/passes/AOTRemoveDeadCountedLoops.cpp` | harmless | none needed | | Covered |
| R2 5.1-12 | `PlanArrayCapacities`, `PlanCollectionCapacities` | `aot/passes/AOTArrayCapacityPlanner.cpp` | not traced | takes a `Storage` | 04 4.8 | Covered |
| R2 5.1-13 | `RestartInGenericVersion` | `aot/passes/AOTRestartInGenericVersion.cpp:33` | harmless | unchanged | 04 4.8 | Covered |
| R2 5.1-14 | `FinishBirths` | `aot/passes/AOTFinishBirths.cpp` | broken | skips Shareable | 04 4.8 | Covered |
| R2 5.1-15 | `FoldConstants` on narrowed variables | `aot/passes/AOTFoldConstants.cpp` | see 5.3 | `absent` stays | 04 4.7, 4.8 | Covered |
| R2 5.1-16 | `ShareRegExpLiterals`, `FoldConstantRegExps` | `aot/passes/AOTShareRegExpLiterals.cpp` | shared | MainOnly or stateless | 04 4.8 | Covered |
| R2 5.1-17 | `Select` rules move members to the root | `aot/select/AOTSelectionContext.cpp:137` | as `interferesWith` | `crossesEffects`, C5 | 04 4.4, 4.8 | Covered |
| R2 5.1-18 | `RemoveRedundantBarriers` | `aot/represent/AOTRemoveRedundantBarriers.cpp:113` | holds iff | S2 | 04 4.8; 08 8.10 | Covered |
| R2 5.2-1 | `AllocateInFrames` | `aot/passes/AOTFrameAllocation.h` | confined | C2 | 04 4.3, 4.8 | Covered |
| R2 5.2-2 | `AllocateInnerObjects` | `aot/passes/AOTAllocateInnerObjects.cpp:45` | static | unchanged | 04 4.8 | Covered |
| R2 5.2-3 | `FlattenInnerObjects`, form 2 is check-then-make | `aot/passes/AOTFlattenInnerObjects.cpp` | check-then-make | Confined only | 04 4.8 | Covered |
| R2 5.2-4 | `DeferClosuresInFields` | `aot/passes/AOTDeferClosuresInFields.cpp` | check-then-make | Z1 with restart, C3 | 04 4.8, 4.6.1; 07 7.6 | Covered |
| R2 5.2-5 | `DeferInitializers` | `aot/passes/AOTDeferInitializers.cpp` | check-then-make | Z1, Z2 | 07 7.6 | Covered |
| R2 5.2-6 | `DeferDefinitions` | `aot/passes/AOTDeferDefinitions.cpp:322` | static | Z1 | 07 7.6 | Covered |
| R2 5.2-7 | `NarrowConstantVariables` | `aot/passes/AOTNarrowConstantVariables.cpp` | safe if `absent` stays | O1 | 04 4.8 | Covered |
| R2 5.2-8 | `PromoteEnvironmentsToVariables` | `aot/passes/AOTPromoteEnvironmentsToVariablesPlan.cpp:182` | needs "once" | counts; O3 | 04 4.7, 4.8 | Covered |
| R2 5.2-9 | `PromoteAggregateParameters` and other passes that move reads to callers | `aot/passes/AOTVirtualObjectParameters.cpp:333` | stale value | unchanged | 04 4.8 | Covered |
| R2 5.2-10 | `RemoveUnreadFields` | `aot/passes/AOTRemoveUnreadFields.cpp` | static | unchanged | 04 4.8 | Covered |
| R2 5.2-11 | `ReuseEnvironments` | `aot/passes/AOTReuseEnvironments.cpp` | shared word | private statics | 04 4.8 | Covered |
| R2 5.2-12 | Constants: `EmbedConstantCollections`, `NumberLiteralStrings`, `CollectRegExpLiterals` | `aot/pipeline/AOTPipelineStages.cpp:316` | static | MS1, Z1 | 06 6.6; 07 7.6 | Covered |
| R2 5.2-13 | `ElideStackChecks` | `aot/passes/AOTElideStackChecks.cpp` | per-thread limit | per thread | 09 9.4 | Covered |
| R2 5.2-14 | `CallModules` | `aot/passes/AOTCallModules.cpp` | check-then-act | MD1 to MD5 | 09 9.9 | Covered |
| R2 5.3-1 | Order-free results hold only if every thread's code and the spawn are modelled | `aot/ir/schema/passes.def:26` | holds | `ThreadEntries`, C2, R5' | 04 4.2.1, 4.2.3 | Covered |
| R2 5.3-2 | "Never written again": publication order can break it | `aot/valueflow/AOTValueFlowResultsFields.cpp:576` | H16 | O2, M6 | 04 4.7 | Covered |
| R2 5.3-3 | `ExecutionCounts`, `StoreCounts`, `ClassEvaluations` | `aot/valueflow/AOTValueFlowResultsLocations.cpp:408` | time | counts; MD3 | 04 4.7 | Covered |
| R2 5.3-4 | `Initialization` | `aot/valueflow/AOTFlowOrderOwner.cpp:496` | time | O1, O3 | 04 4.7 | Covered |
| R2 5.3-5 | `ConstructorFieldOrders`, occupancy | `aot/valueflow/AOTFlowConstructionOrders.cpp:1217` | time | O2 | 04 4.7 | Covered |
| R2 5.4 | B3 and Air repeat the reasoning of the IR; all phases assume one mutator | `aot/codegen/AOTB3Policy.cpp:114` | one mutator | C4, rules per switch | 04 4.9 | Covered |

### R2, section 6: stale value or broken memory safety

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R2 6.2-A | Arrays: test and access load the butterfly apart | `aot/codegen/AOTElementAccessEmitter.cpp:203` | unsafe | S1, SW1 to SW9 | 04 4.6.3; 06 6.2 | Covered |
| R2 6.2-B | Birth flag, and its byte | `aot/codegen/AOTSlotAccessEmitter.cpp:583` | unsafe | not used; CO5 | 04 4.5 | Covered |
| R2 6.2-C | `IsSlotFilled [holdsDeclaredType]`, then `GetSlot` | `aot/codegen/AOTSlotEmitter.cpp:122` | unsafe | `TryGetSlot` | 04 4.6.1 | Covered |
| R2 6.2-D | Out-of-line slots | `aot/codegen/AOTSlotAccessEmitter.cpp:599` | unsafe | CO6 | 05 5.5 | Covered |
| R2 6.2-E | `PutSlotOrAdd`, `PutSlots` | `aot/codegen/AOTSlotAccessEmitter.cpp:618` | S, maybe M | CO5 | 04 4.6.2 | Covered |
| R2 6.2-F | Order-dependent flow facts: a read without a check | `aot/codegen/AOTBindingEmitter.cpp:135` | unsafe | O1, O2, O3 | 04 4.7 | Covered |
| R2 6.2-G | Typed arrays | `aot/codegen/AOTHeaps.cpp:43` | unsafe | TA1 to TA8 | 04 4.6.4; 06 6.8 | Covered |
| R2 6.2-H | Cells converted in place | `aot/codegen/AOTHeaps.cpp:427` | unsafe | C1, CO10, safepoint operation | 05 5.8, 5.10 | Covered |
| R2 6.2-I | Maps, Sets, strings, RegExp state, promise state in line | `aot/builtins/AOTMapSetBuiltinEmitter.cpp` | unsafe | MS, PR, per thread | 06 6.6, 6.7, 6.9; 09 9.7 | Covered |
| R2 6.3-1 | Value facts | `aot/ir/AOTSchema.def:21` | cannot break | C1 | 04 4.5 | Covered |
| R2 6.3-2 | `Shape`, `OwnInstance` | `aot/analysis/AOTGuardFacts.cpp:48` | cannot break | CO1 | 04 4.5 | Covered |
| R2 6.3-3 | `SlotFilled`, plain | `aot/ir/schema/prepared-operations.def:34` | crash-safe, not correct | C3 | 04 4.5, 4.6.1 | Covered |
| R2 6.3-4 | `SlotFilled [holdsDeclaredType]`, `BirthFieldsFilled` | `aot/analysis/AOTGuardFacts.cpp:643` | possibly unsafe | `TryGetSlot` | 04 4.5 | Covered |
| R2 6.3-5 | `NoExtraProperties`, `NoFieldOutsideSlot` | `aot/codegen/AOTGuardEmitter.cpp:167` | JS | CO3, CO8, CO15, `TryGetSlotOrUndefined` | 04 4.5, 4.6.1 | Covered |
| R2 6.3-6 | `NotFrozen` | `aot/codegen/AOTGuardEmitter.cpp:282` | JS | M9 | 04 4.5 | Covered |
| R2 6.3-7 | `KeyOrder`, `EnumeratorCurrent` | `aot/codegen/AOTExpandedIterationEmitter.cpp:39` | treat as unsafe | own test; N1 to N3 | 04 4.5 | Covered |
| R2 6.3-8 | `PrototypeLinkUnchanged`, `MemberUnchanged`, `EvaluatedOnce` and others | `aot/codegen/AOTGuardEmitter.cpp:361` | JS stale | end at `synchronizes`; F1a, CO11 to CO13 | 04 4.5; 05 5.14 | Covered |
| R2 6.3-9 | `Pristine(Storage, Dense)`, `ArrayStorage`, `InBounds` | `aot/ir/schema/prepared-operations.def:19` | unsafe | `Storage` values | 04 4.6.3, 4.6.4 | Covered |
| R2 6.3-10 | `PlainData` | `aot/ir/schema/typed-data-operations.def:34` | as the run time | 05, 06 | 04 4.5 | Covered |
| R2 6.3-11 | `SourceOpen`, `TextMatches`: one parser state per Instance | `aot/ir/schema/typed-data-operations.def:7` | unsafe if shared | private | 04 4.5; 07 7.3.3 | Covered |

### R2, section 9: B3, Air, fences, Atomics

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R2 9.1 | The direct generator emits what the emitters wrote; it lacks atomic and fenced cases | `aot/codegen/AOTDirectCodeGenerator.cpp:6096` | - | new cases | 04 4.9 | Covered |
| R2 9.2-1 | Effect summaries reach B3 as heap ranges | `aot/codegen/AOTRuntimeCallEffects.cpp:72` | one mutator | top and fence | 04 4.4 | Covered |
| R2 9.2-2 | Immutable heaps: Instance fields, shape word, closure entry, captures | `aot/codegen/AOTHeaps.h:42` | safe if immutable | CO1, G1 to G3, T1 | 04 4.9; 09 9.2, 9.6 | Covered |
| R2 9.3-1 | `MemoryNumbering`: load merging and store-to-load forwarding | `b3/B3MemoryNumbering.cpp:362` | one mutator | C4 | 04 4.9 | Covered |
| R2 9.3-2 | `hoistLoopInvariantValues`, loads cross polls | `b3/B3HoistLoopInvariantValues.cpp:198` | one mutator | C4; a poll's writes are honoured | 04 4.9, 4.12 | Covered |
| R2 9.3-3 | `hoistsImmutableLoadsPastWrites` | `aot/codegen/AOTB3Policy.cpp:151` | safe if immutable | immutable per thread | 04 4.9 | Covered |
| R2 9.3-4 | Loop rotation, folded comparisons, versions, clamped bounds | `aot/codegen/AOTB3Policy.cpp:147` | one mutator | C4 | 04 4.9 | Covered |
| R2 9.3-5 | `sinksValues` | `b3/B3SinkValues.cpp:226` | neutral | none needed | | Covered |
| R2 9.3-6 | `mergesAdjacentConstantStores` changes order and width | `b3/B3MergeAdjacentConstantStores.cpp:82` | order, width | harmless as it is | 04 4.9 | Covered |
| R2 9.3-7 | Trusted ranges, dereferenceable bytes, inductions | `aot/codegen/AOTCodeGeneratorPublication.cpp:322` | speculative loads | C3, O2, O3, F1a | 04 4.9 | Covered |
| R2 9.3-8 | Range reduction, implied comparisons, jump threading, selects | `aot/codegen/AOTB3Policy.cpp:130` | value reasoning | C1 | 04 4.1 | Covered |
| R2 9.3-9 | Air pairs memory accesses | `aot/codegen/AOTB3Policy.cpp:245` | two accesses | not for release | 04 4.9 | Covered |
| R2 9.3-10 | Null tests become faults | `b3/air/AirFoldTrapsIntoAccesses.cpp` | - | stateless handler | 04 4.9; 09 9.5 | Covered |
| R2 9.3-11 | Client phases: `updateIntegersInMemory`, `guardNarrowReferences` | `aot/codegen/AOTFunctionCodeGenerator.cpp:174` | not atomic | not for CAS slots | 04 4.9 | Covered |
| R2 9.4-1 | No IR operation is atomic; there is no fence operation | `aot/ir/schema/inline-builtins.def:7` | - | `AtomicSlot` on the loaded word, F1 to F5 | 04 4.6.6, 4.10 | Covered |
| R2 9.4-2 | The inline row of `Atomics` loses `ExternalState`; no barrier for slots, variables, guards | `aot/ir/AOTEffectsOfBuiltins.cpp:349` | S | `synchronizes` | 04 4.4 | Covered |
| R2 9.4-3 | Plain typed array accesses are merged and hoisted | `aot/builtins/AOTBufferBuiltinEmitterAtomics.cpp:38` | allowed | M10 | 02 2.10.4; 04 4.12 | Covered |
| R2 9.4-4 | All other fences serve the collector, none another mutator | `aot/stubs/AOTStubHelpersAllocation.cpp:73` | - | F1, F3, F4 | 04 4.10; 08 8.10 | Covered |

### Problems that the reports do not list

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| N1 | A coroutine of a thread that has ended is resumed on another thread | `aot/codegen/AOTHeaps.h:42` | MEM | G1 to G3, J2, T1, T2 | 09 9.2, 9.6, 9.7; 02 2.6 | Covered |
| N2 | `AtomicSlot`: add before the overflow test, markers as operands, `absent` overwritten | `aot/object/AOTSlotRepresentation.h:55` | MEM | CO2, CO3 | 04 4.6.6; 03 3.5.2 | Covered |
| N3 | The lock word is the holder's thread ID, and the main thread's ID is 0 | - | S (no exclusion) | the holder field is the ID + 1 | 04 4.13; 09 9.13 | Covered |
| N4 | The host moves a value to the main thread: no seed of `Shared` | `aot/valueflow/AOTValueFlowRules.cpp:105` | MEM | R5' | 04 4.2.3 | Covered |
| N5 | Copies by words **into** a published object; its Extension is replaced | `aot/object/AOTObjectAssign.cpp:236` | JS, MEM | CO14, CO6 | 05 5.12 | Covered |
| N6 | M5 (coherence) against "perform a read once", "move a read" | `aot/passes/AOTEliminateRedundantReads.cpp:338` | JS | M5, reworded | 02 2.10.4 | Covered |

## A.2 Run-time state and control (reports R4, R6)

### R4, section 1.1: the regions of the Instance

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 1.1-1 | Cards of the statics: every mutator stores 1, the collector rewrites them | `aot/stubs/AOTStubsPropertyProbes.cpp:1392` | (c) | shared region; B5 | 07 7.3.2, 08 8.10 | Covered |
| R4 1.1-2 | Statics mix truth, per-thread words and profiles in one region | `aot/place/AOTStaticsLayout.cpp:383` | mixed | shared chunk and private chunk | 07 7.3.2 | Covered |
| R4 1.1-3 | One loop poll page for all threads of a realm; the fault handler compares with the pinned register | `aot/runtime/AOTRuntimeHooks.cpp:141` | (b) | private region; stop arms each page | 07 7.3.2, 08 8.4 | Covered |
| R4 1.1-4 | Prefix holds constants, headers, collection StructureIDs that an event fills later, two allocators | `aot/runtime/AOTInstance.h:72` | (a)(b)(c) | private region, copied at creation; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.3.2; 07 7.3.3, 7.6 | Covered |
| R4 1.1-5 | The struct mixes constants, caches and words of the running thread | `aot/runtime/AOTInstance.h:100` | mixed | private as a whole; V3 to V7, `ThreadState` | 07 7.3.3; 07 7.3.1 | Covered |
| R4 1.1-6 | Key order step caches: inline readers, slow-path writers, 16-byte entries | `aot/object/AOTObjectWords.h:111` | value..memory | K1 | 07 7.3.2, 7.4 | Covered |
| R4 1.1-7 | Built-in location state bytes tested in line, flipped by watchpoints | `aot/runtime/AOTBuiltinLocationWatch.cpp:52` | (c) | F3, shared byte, `arm()` under lock | 07 7.5 | Covered |
| R4 1.1-8 | Global binding records, 24 bytes, read in 2 to 3 pieces | `aot/runtime/AOTRuntimeWords.h:43` | memory | K1, K2, private region; V3 to V7, `ThreadState` | 07 7.3.2, 7.4; 07 7.3.1 | Covered |
| R4 1.1-9 | Built-in cells: lazily filled single words | `aot/runtime/AOTBuiltinCells.cpp:84` | (c) | Z1 | 07 7.6 | Covered |
| R4 1.1-10 | Built-in values: lazily filled, and zeroed again by `forgetValuesNamed` | `aot/runtime/AOTBuiltinCells.cpp:354` | (c) | Z1; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.6; 07 7.3.3, 7.6 | Covered |
| R4 1.1-11 | String cells: a stub allocates the cell and stores the word itself | `aot/stubs/AOTStubsConstants.cpp:81` | (c) | Z1, shared region | 07 7.6 | Covered |
| R4 1.1-12 | Site caches: header and payload read separately | `aot/stubs/AOTStubsPropertySiteCaches.cpp:140` | memory | K1, private region | 07 7.4 | Covered |
| R4 1.1-13 | Method table copy written under an `mprotect` window | `aot/object/AOTMemberGuards.cpp:1654` | (c) | F2, read-write alias, `ClassTable` lock | 07 7.3.2, 7.7 | Covered |

### R4, section 1.2: the struct

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 1.2-1 | `runtimeTable`, `shapeSets`, tags, `globalObject`, `cellCageBase`: constants | `aot/runtime/AOTInstance.h:100` | (b) | copied from the canonical view | 07 7.3.3 | Covered |
| R4 1.2-2 | `vm`: constant, but per-thread `VM` members are addressed through it by stubs | `aot/runtime/AOTInstance.h:103` | (b) | 7.2 accessors (C++ only); V8 | 07 7.2; 07 7.3.4 | Covered |
| R4 1.2-3 | `stackMirror`: the limit of the one thread | `runtime/StackManager.cpp:36` | (a) | one `StackManager` per thread | 09 9.4 | Covered |
| R4 1.2-4 | `barrierThreshold`, `mutatorShouldBeFenced`: copies that the collector rewrites | `aot/runtime/AOTHeapHooks.cpp:149` | (c) | written into every view while parked; T3, T4, H3, H4 | 07 7.3.3, 08 8.10; 08 8.14; 09 9.2 | Covered |
| R4 1.2-5 | `awaitMustSuspend` | `aot/runtime/AOTRuntimeHooks.cpp:116` | (a)/(c) | F13 | 07 7.5, 09 9.7 | Covered |
| R4 1.2-6 | `propertyCaches`: table bases, generations, counters, a one-byte message, two name bit sets | `aot/runtime/AOTPropertyCachesWords.h:211` | mixed | K1; V3 to V7, `ThreadState`; K4 | 07 7.4; 07 7.3.1 | Covered |
| R4 1.2-7 | `image`: pointers into the image; `programSymbols` names a table that grows | `aot/runtime/AOTInstance.h:117` | (b) | constants; 7.6 row `ProgramSymbols` | 07 7.3.3, 7.6 | Covered |
| R4 1.2-8 | `allocation`: allocators, headers, `clonedStructures` (two words per entry) | `aot/runtime/AOTInstance.h:174` | memory | private, filled per thread | 07 7.3.3, 08 8.8 | Covered |
| R4 1.2-9 | `heap`: compiled code appends to the log and the mark stack | `aot/runtime/AOTHeapWords.h:19` | (a) | `MutatorContext` | 08 8.2 | Covered |
| R4 1.2-10 | `realm`: lazily filled constants, string key tables republished on growth, queue pointer | `aot/runtime/AOTInstance.h:198` | memory | private; K1 | 07 7.3.3, 7.4 | Covered |
| R4 1.2-11 | `numbers` | `aot/runtime/AOTInstance.h:221` | (b) | constants | 07 7.3.3 | Covered |
| R4 1.2-12 | `runtime`: exception words, counters, constant tables, header bump pointer | `aot/runtime/AOTRuntimeWords.h:96` | (a)(c) | thread state; private arena; T5; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.3.3, 7.6, 09 9.5; 09 9.2; 07 7.3.3, 7.6 | Covered |
| R4 1.2-13 | `modules`: status table pointer, `evaluatingMark` | `aot/runtime/AOTModuleWords.h:25` | (d) | MD4; thread state; V3 to V7, `ThreadState`; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.3.3, 09 9.9; 07 7.3.1; 07 7.3.3, 7.6 | Covered |
| R4 1.2-14 | `objects`: allocation site records, key order and prototype caches | `aot/object/AOTObjectWords.h:123` | memory | K1; V3 to V7, `ThreadState` | 07 7.4; 07 7.3.1 | Covered |
| R4 1.2-15 | `closures`: three allocators; four StructureIDs that an event fills | `aot/object/AOTClosureTable.cpp:130` | (a)(c) | private; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.3.3; 07 7.3.3, 7.6 | Covered |
| R4 1.2-16 | `classes`: lazily made prototype cells, two small caches | `aot/object/AOTClassTable.cpp:60` | (c) | `classPrototypes[]` moves to a shared region; caches K1 | 07 7.4, 7.7 | Covered |
| R4 1.2-17 | `coroutines`: eleven hand-over words, one allocator | `aot/runtime/AOTCoroutineWords.h:22` | (a) | private; V3 to V7, `ThreadState` | 07 7.3.3, 09 9.7; 07 7.3.1 | Covered |
| R4 1.2-18 | `builtins`: `objectCreate[64]` pairs a prototype with a header | `aot/builtins/AOTBuiltinWords.h:38` | memory | private cache | 07 7.3.3 | Covered |
| R4 1.2-19 | `arrays`: heuristic counters updated by stubs | `aot/stubs/AOTStubsArrayWords.h:15` | value | private | 07 7.8 | Covered |
| R4 1.2-20 | `iteration`, `stubs`, `lazyStrings` | `aot/stubs/AOTStubWords.h:38` | (b) | constants | 07 7.3.3 | Covered |
| R4 1.2-21 | `json`: the open source of the running thread | `aot/runtime/AOTJSONWords.h:28` | (a) | private; V3 to V7, `ThreadState` | 07 7.3.3, 06 6.7; 07 7.3.1 | Covered |
| R4 1.2-22 | `stringBuilder.freeStorage`: free list popped in line | `aot/runtime/AOTStringBuilderWords.h:16` | (a) | private | 07 7.3.3, 06 6.7 | Covered |
| R4 1.2-23 | `scratch`: `misfit` and validation words | `aot/codegen/AOTCodegenScratchWords.h:27` | (a) | private | 07 7.3.3, 04 4.14 | Covered |
| R4 1.2-24 | `constructWords`: key and code word in two stores, written by a stub | `aot/stubs/AOTStubsCallAdapters.cpp:331` | memory | private cache | 07 7.3.3 | Covered |
| R4 1.2-25 | `State* state`, and `State::instance` back | `aot/runtime/AOTState.h:105` |  | V3 to V7, `ThreadState` | 07 7.3.1 | Covered |

### R4, section 1.3: the statics

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 1.3-1 | Variables: plain racy words; initialization proofs assume one thread | `aot/place/AOTStaticsLayout.cpp:236` | memory | O1, F1; shared region | 04 4.7, 4.10 | Covered |
| R4 1.3-2 | Module values: one lazily filled root word per module | `aot/place/AOTStaticsLayout.cpp:291` | (d) | MD7 | 09 9.9 | Covered |
| R4 1.3-3 | Guard words: 64 guards per word, plain read-modify-write | `aot/object/AOTMemberGuards.cpp:325` | memory | F1, E1; E5; F1a, CO11 to CO13 | 07 7.5; 05 5.14 | Partly (W-7.4) |
| R4 1.3-4 | Module object, `require`, `import.meta`: made at first use | `aot/place/AOTStaticsLayout.cpp:322` | (d) | MD7, Z1; MD3', MD3'', MD7', W1 | 07 7.6, 09 9.9; 09 9.9, 9.14 | Covered |
| R4 1.3-5 | Shared storage of copy-on-write literals, stored by `setCell` | `aot/runtime/AOTDataBuilder.cpp:516` | (c) | Z1; K3, Z1 (card) | 07 7.6; 07 7.4, 7.6 | Covered |
| R4 1.3-7 | RegExp cells: root word per literal, stored by `setCell` | `aot/runtime/AOTProgramStatics.h:41` | (c) | Z1; K3, Z1 (card) | 07 7.6; 07 7.4, 7.6 | Covered |
| R4 1.3-8 | Template objects: identity visible | `aot/codegen/AOTAllocationEmitterObjects.cpp:39` | value | Z1 in line | 07 7.6 | Covered |
| R4 1.3-9 | Array allocation profiles: read-modify-write per allocation | `aot/stubs/AOTStubsArrayHoles.cpp:401` | value | private statics | 07 7.3.2, 7.8 | Covered |
| R4 1.3-10 | Last arrays: not roots, cleared each collection | `aot/runtime/AOTArrayAllocationProfile.cpp:39` | value | private statics | 07 7.3.2 | Covered |
| R4 1.3-11 | Free environment cells: a one-element free list | `aot/codegen/AOTAllocationEmitter.cpp:238` | memory | private statics | 07 7.3.2, 04 4.8 | Covered |
| R4 1.3-12 | `initializeProgramStatics`: check, then act, on a plain `bool` | `aot/runtime/AOTProgramStatics.cpp:73` | (d) | runs before any thread exists | 07 7.6 | Covered |

### R4, sections 3 to 6: items of the body that are marked unsafe

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 3.5 | A stale `Value` slot reads `holder + offset` without a test of the holder's Structure | `aot/stubs/AOTStubsPropertySiteCaches.cpp:66` | memory | E4 (N2, N3, SW2); H12, E2; E4 by kind, N2 | 07 7.5; 08 8.3; 06 6.4 | Covered |
| R4 3.8-1 | Absence filters: 64-byte struct copy against a stub that reads three words | `aot/runtime/AOTPropertyCaches.cpp:263` | value | K1, E2 | 07 7.4, 7.5 | Covered |
| R4 3.8-2 | Absence generations copied into every Instance | `aot/runtime/AOTPropertyCaches.cpp:292` | (c) | per thread; E2 bumps them | 07 7.5 | Covered |
| R4 3.8-3 | `namesOfObjectPrototypes`, `namesChangedOnChains`: bits only ever set, tested with no generation | `aot/stubs/AOTStubsPropertyProbes.cpp:252` | value | K1 by implication; K4 | 07 7.4 | Covered |
| R4 3.8-4 | `namesOfTexts`: torn entry, uid freed under a reader | `aot/runtime/AOTPropertyCaches.cpp:68` | memory | K1 | 07 7.4 | Covered |
| R4 3.8-5 | Custom accessor records: function pointer from a recycled or half-written record | `aot/stubs/AOTStubsPropertyAccessors.cpp:107` | memory | K1 by implication ("`propertyCaches.*` point at the thread's tables") | 07 7.4 | Covered |
| R4 3.8-6 | `StringKeyCache`, `integerKeys`, `caseConversions` | `aot/runtime/AOTPropertyCachesStringKeys.cpp:28` | memory | K1 | 07 7.4 | Covered |
| R4 3.8-7 | `NumericStrings`, small strings read by stubs | `aot/runtime/AOTInstance.h:211` | (c) | K1 | 07 7.4, 06 6.7 | Covered |
| R4 3.8-8 | Fourteen small direct-mapped caches with key and value in different words | `aot/object/AOTObjectWords.h:95` | memory | K1 | 07 7.3.3, 7.4 | Covered |
| R4 5.2 | `ChainDependents`: hash maps; five kinds of owner, two of which own shared state | `aot/runtime/AOTChainDependents.cpp:95` | (d) | "per thread: no lock"; E5 | 07 7.7; 07 7.5 | Covered |
| R4 5.6 | `RememberedSlotSet::record` appends to a `Vector` without a lock | `heap/RememberedSlot.h:47` | memory | one registry per thread; B4; K3, Z1 (card) | 07 7.4, 08 8.10; 07 7.4, 7.6 | Covered |
| R4 6.2 | A global binding caches an offset from the global object's butterfly | `aot/codegen/AOTAccessEmitter.cpp:343` | memory | N1, SW7 | 06 6.4 | Covered |
| R4 6.3 | A variable proven initialized starts as 0 and is read without a test | `aot/place/AOTStaticsLayout.cpp:102` | memory | O1 | 04 4.7 | Covered |

### R4, section 5.7: words that code tests and events flip

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 F1 | Guard bits, taken to hold over a stretch of code | `aot/codegen/AOTGuardEmitter.cpp:405` | memory | F1, E1; E5; F1a, CO11 to CO13 | 07 7.5; 05 5.14 | Partly (W-7.4) |
| R4 F2 | Method table entries | `aot/codegen/AOTCallEmitterDirect.cpp:142` | (c) | F2 | 07 7.5 | Covered |
| R4 F3 | `WatchedLocationState` | `aot/builtins/AOTBuiltinCallEmitter.cpp:541` | (c) | F3 | 07 7.5 | Covered |
| R4 F4 | `GlobalBinding.constant` | `aot/runtime/AOTGlobalBindings.cpp:63` | value | K2 | 07 7.4 | Covered |
| R4 F5 | `GlobalBinding` guard word against the global object's StructureID and epoch | `aot/codegen/AOTAccessEmitter.cpp:369` | memory | private; K2 | 07 7.5 | Covered |
| R4 F6 | Site cache header | `aot/stubs/AOTStubsPropertySiteCaches.cpp:87` | memory | K1 | 07 7.5 | Covered |
| R4 F7 | Front row word, front cell, hashed entry | `aot/stubs/AOTStubsPropertyProbes.cpp:1006` | memory | K1 | 07 7.5 | Covered |
| R4 F8 | Absence generations, filters, name bit sets | `aot/stubs/AOTStubsKnownFieldStores.cpp:57` | value | K1, E2; K4 | 07 7.5; 07 7.4 | Covered |
| R4 F9 | Trap-aware stack limit as the stop request | `runtime/StackManager.cpp:36` | (a) | private, written by the requester | 07 7.5, 08 8.4 | Covered |
| R4 F10 | Protection of the loop poll page | `aot/codegen/AOTHintEmitter.cpp:40` | | private | 07 7.5, 08 8.4 | Covered |
| R4 F11 | "An OutOfMemoryError is owed": one flag | `aot/stubs/AOTStubsPolls.cpp:133` | (c) | per thread | 07 7.5, 08 8.8 | Covered |
| R4 F12 | Barrier threshold, fence flag | `aot/runtime/AOTHeapHooks.cpp:149` | (c) | every view, while parked; T3, T4, H3, H4 | 07 7.5; 08 8.14; 09 9.2 | Covered |
| R4 F13 | `awaitMustSuspend` | `aot/runtime/AOTRuntimeHooks.cpp:116` | | private | 07 7.5 | Covered |
| R4 F14 | `frameThatMayContinue == fp` and the queue is empty | `aot/runtime/AOTAsyncAwaitConditions.h:61` | memory | private queue | 07 7.5, 09 9.7 | Covered |
| R4 F15 | `m_asyncContextTrackingEnabled` | `aot/runtime/AOTAsyncAwaitConditions.h:72` | (c) | atomic store, set once | 07 7.5 | Covered |
| R4 F16 | Module status byte | `aot/codegen/AOTModuleEmitter.cpp:151` | memory | MD1, MD2, MD5 | 09 9.9 | Covered |
| R4 F17 | Absent encoding of a variable; 0 for a function not made | `aot/place/AOTStaticsLayout.cpp:102` | memory | O1; Z1, Z2; Z2 parts 1 to 5, W1, `prepareForThreads` | 04 4.7, 07 7.6; 07 7.6; 09 9.14 | Covered |
| R4 F18 | Lazily filled words, "for ever once non-zero" with four exceptions | `aot/codegen/AOTConstantEmitter.cpp:254` | value..memory | Z1, Z2; Z2 parts 1 to 5, W1, `prepareForThreads`; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.6; 09 9.14; 07 7.3.3, 7.6 | Covered |
| R4 F19 | Array headers change when the realm has a bad time | `aot/runtime/AOTInstance.cpp:375` | value..memory | private copies; safepoint operation; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.5, 08 8.6; 07 7.3.3, 7.6 | Covered |
| R4 F20 | StructureIDs published lazily, 0 then the ID | `aot/runtime/AOTInstance.cpp:365` | (c) | private copies "filled at first use"; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.5; 07 7.3.3, 7.6 | Covered |
| R4 F21 | Validation and testing words | `aot/codegen/AOTCodegenScratchWords.h:27` | not shipping | private | 04 4.14 | Covered |
| R4 F22 | Words that are thread-private by intent | `aot/runtime/AOTCoroutineWords.h:22` | memory | private in fact | 07 7.5 | Covered |
| R4 F23 | Flags in objects and Structures | `aot/object/AOTObject.h:472` | R1 | CO5, CO8 | 05 5.2 | Covered |

### R4, section 7: made at first use

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 7.1 | String constant cell: two creators; ARM64 needs release order | `aot/stubs/AOTStubsConstants.cpp:276` | (c) | Z1 | 07 7.6 | Covered |
| R4 7.2 | String header bump pointer moved by a stub: two strings in one header | `aot/stubs/AOTStubsConstants.cpp:263` | memory | private arena per thread; T5 | 07 7.6, 06 6.7; 09 9.2 | Covered |
| R4 7.3 | Key cell, literal string cell; two `Symbol` cells for one `SymbolImpl` | `aot/runtime/AOTProgramStrings.cpp:342` | value | Z1; symbol map under its lock | 07 7.6 | Covered |
| R4 7.4 | BigInt constant | `aot/runtime/AOTProgramStrings.cpp:360` | (c) | Z1 | 07 7.6 | Covered |
| R4 7.5 | `ProgramSymbols`: lock-free probes against a table that is freed; duplicate private names | `aot/runtime/AOTProgramIdentifiers.cpp:98` | memory | table lock, PT1, freed after a safepoint; rows of 7.6 and 7.7 | 07 7.6, 08 8.9; 07 7.6, 7.7 | Covered |
| R4 7.5b | Lazy string headers of a position-independent image | `aot/image/AOTImageText.cpp:101` | correct today | none needed | 07 7.1 | Covered |
| R4 7.6 | Singleton closure: identity visible; `forgetClosure` resets the word | `aot/object/AOTClosureTable.cpp:299` | value..memory | Z1; K3, Z1 (card) | 07 7.6; 07 7.4, 7.6 | Covered |
| R4 7.7 | Class objects, prototypes, usual Structures, allocation site records | `aot/object/AOTAllocationSites.cpp:121` | memory | Z2 claim word; `StructureTable` lock; K1; Z2 parts 1 to 5, W1, `prepareForThreads` | 07 7.6, 7.7, 05 5.10; 07 7.6; 09 9.14 | Covered |
| R4 7.8 | Template object | `aot/codegen/AOTAllocationEmitterObjects.cpp:39` | value | Z1 in line | 07 7.6 | Covered |
| R4 7.9 | Namespace object, module object, `require`, `import.meta` | `aot/codegen/AOTModuleEmitter.cpp:64` | (d) | MD7; MD3', MD3'', MD7', W1 | 09 9.9; 09 9.9, 9.14 | Covered |
| R4 7.10 | `RegExpLiterals` containers: mutator without a lock, collector under the registry's | `aot/runtime/AOTRegExpLiterals.cpp:76` | memory | `RegExpLiterals` lock; rows of 7.6 and 7.7 | 07 7.6, 7.7 | Covered |
| R4 7.11 | Shared RegExp literal: `lastIndex` is live state | `aot/passes/AOTShareRegExpLiterals.cpp:208` | value | pass rule | 04 4.8 | Covered |
| R4 7.12 | Key order tables: pointer and mask in two words, old pages released | `aot/object/AOTKeyOrderTable.cpp:766` | memory | front per thread; truth under `KeyOrderTable::m_lock` | 07 7.4, 7.7 | Covered |
| R4 7.13 | Free environment cell: two threads take one cell | `aot/codegen/AOTEnvironmentEmitter.cpp:74` | memory | private statics | 04 4.8 | Covered |
| R4 7.14 | `unique_ptr` members of `State` made on demand | `aot/runtime/AOTState.h:74` | (d) | per thread, or made with the realm; V3 to V7, `ThreadState` | 07 7.6; 07 7.3.1 | Covered |
| R4 7.15 | Inline allocation data `{allocator, header}` | `aot/runtime/AOTOperationScope.h:89` | (a)(c) | private | 07 7.3.3 | Covered |
| R4 7.16 | Built-in cells and host values; side containers; `forgetValuesNamed` | `aot/runtime/AOTBuiltinCells.cpp:243` | (c)(d) | Z1; `BuiltinCells` lock; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.6, 7.7; 07 7.3.3, 7.6 | Covered |
| R4 7.17 | Property runs, growth plans, link-time constants, lazily published StructureIDs | `aot/runtime/AOTInstance.h:210` | (c) | private, filled per thread; V9 (`pushed`, `pulled`), Z1 (reset) | 07 7.3.3; 07 7.3.3, 7.6 | Covered |
| R4 7.18 | Boxed doubles: none exist | | none | | | Covered |
| R4 7.19 | Copy-on-write array literal storage | `aot/runtime/AOTDataBuilder.cpp:516` | (c) | Z1; K3, Z1 (card) | 07 7.6; 07 7.4, 7.6 | Covered |
| R4 7.20 | Caches that the run time writes on Structures (`DefinitelyNonThenableState`, special properties) | `aot/runtime/AOTChainDependents.cpp:55` | R1/R7 | 06 6.5 (single words); E5 | 06 6.5; 07 7.5 | Covered |

### R4, section 10: engine members reached by offset

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 10.2-1 | `VM::m_exception` read by stubs | `aot/stubs/AOTStubsPolls.cpp:107` | (a) | the word lies in the view | 09 9.5 | Covered |
| R4 10.2-2 | `VM::topCallFrame` written by stubs in front of host calls | `aot/stubs/AOTStubsCallHost.cpp:78` | (a) | 7.2; V8 | 07 7.2; 07 7.3.4 | Covered |
| R4 10.2-3 | `VM::topEntryFrame` read by the unwind stub | `aot/stubs/AOTStubsExceptions.cpp:48` | (a) | 7.2, 9.3; V8 | 07 7.2, 09 9.5; 07 7.3.4 | Covered |
| R4 10.2-4 | `targetMachinePCForThrow`, `callFrameForCatch`, `targetMachinePCAfterCatch` | `aot/stubs/AOTStubsExceptions.cpp:81` | (a) | 7.2; V8 | 07 7.2; 07 7.3.4 | Covered |
| R4 10.2-5 | `m_stopsAtFunctionEntries` read by poll stubs | `aot/stubs/AOTStubsPolls.cpp:133` | (c) | per thread; V8 | 07 7.5 F11; 07 7.3.4 | Covered |
| R4 10.2-6 | `structureIDBase` in the adapter | `aot/stubs/AOTStubsCallAdapters.cpp:101` | (b) | the adapter reads the view from TLS | 07 7.3.4 | Covered |
| R4 10.2-7 | `regExpMatchOutput`: one buffer per VM, written by stubs | `aot/stubs/AOTStubsRegExpFrames.h:76` | (a) memory | per thread; V8 | 07 7.2, 06 6.9; 07 7.3.4 | Covered |
| R4 10.2-8 | `m_orderedHashTableDeletedValue` | `aot/stubs/AOTStubsCollections.cpp:918` | (b) | none needed | | Covered |
| R4 10.2-9 | `m_asyncContextTrackingEnabled` | `aot/runtime/AOTAsyncAwaitConditions.h:72` | (c) | F15 | 07 7.5 | Covered |
| R4 10.2-10 | `heap.m_barrierThreshold`, `m_mutatorShouldBeFenced` read by the interpreter's handlers | `llint/LowLevelInterpreterAOT.asm:751` | (c) | the heap's own words | 08 8.10 | Covered |
| R4 10.3-1 | `LocalAllocator`, `FreeList` | `aot/stubs/AOTStubHelpersAllocation.cpp:36` | (a) memory | per thread | 08 8.8 | Covered |
| R4 10.3-2 | `CompleteSubspace::m_allocatorForSizeStep[]` yields an allocator | `heap/CompleteSubspace.cpp:85` | (a) | `ThreadSubspaceAllocators` | 08 8.8 | Covered |
| R4 10.3-3 | `StoredCellLog`, mark stack, `barriersExecuted` | `aot/stubs/AOTStubsBarriers.cpp:40` | (a) | per thread | 08 8.2 | Covered |
| R4 10.3-4 | `MicrotaskQueue` read and filled in line | `aot/stubs/AOTStubsCoroutineJobs.cpp:42` | memory | one queue per thread | 09 9.7 | Covered |
| R4 10.3-5 | `JSGlobalObject`: `m_aotInstance`, RegExp cached result (seven stores), `weakRandom`, `asyncContextData` | `aot/stubs/AOTStubsRegExp.cpp:376` | memory | per thread; V3 to V7, `ThreadState` | 06 6.9, 07 7.4, 09 9.7; 07 7.3.1 | Covered |
| R4 10.3-6 | `Structure`, `PropertyTable`, butterfly, `JSString`, `JSPromise`, Map and Set storage | | R1, R5, R7 | 05, 06, 09 | | Covered |
| R4 10.3-7 | `Yarr::MatchingContextHolder` | | per thread already | | | Covered |
| R4 10.3-8 | `RuntimeTable::m_entries`; one shared `CallLinkInfo` in virtual mode | `aot/runtime/AOTRuntimeTable.cpp:222` | (b) | none needed: nothing writes it | | Covered |

### R4, section 11: hazards H-1 to H-32

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R4 H-1 | Thread-private words kept in `VM`, `Instance`, `JSGlobalObject` | `aot/runtime/AOTInstance.h:226` | memory | `ThreadContext`, view, V1, V2; V3 to V7, `ThreadState`; V8 | 07 7.2, 7.3; 07 7.3.1; 07 7.3.4 | Covered |
| R4 H-2 | Site caches: torn header and payload | `aot/runtime/AOTSiteCaches.cpp:102` | memory | K1 | 07 7.4 | Covered |
| R4 H-3 | Hashed table: three-word entries, eviction, growth | `aot/runtime/AOTPropertyCachesHashedTable.cpp:110` | memory | K1 | 07 7.4 | Covered |
| R4 H-4 | Front table: a stale row base finds another Structure's cell | `aot/runtime/AOTPropertyCachesFrontRows.cpp:206` | memory | K1; `StructureRows` per thread | 07 7.4 | Covered |
| R4 H-5 | Proofs that hold for a stretch; lost clear of a guard bit | `aot/object/AOTMemberGuards.cpp:391` | memory | F1, E1; 04 4.5; E5; F1a, CO11 to CO13 | 07 7.5; 05 5.14 | Partly (W-7.4) |
| R4 H-6 | `GlobalBinding`: guard word written before the location | `aot/runtime/AOTGlobalBindings.cpp:127` | memory | private; K2 | 07 7.4 | Covered |
| R4 H-7 | "Change, then report"; "publish, then register the dependent" | `aot/runtime/AOTPropertyCaches.cpp:559` | memory | E1 to E4; H12, E2; E4 by kind, N2; E2a; E3 | 07 7.5; 08 8.3; 06 6.4 | Covered |
| R4 H-8 | Module evaluation: inline claim, `Evaluating` read as one's own cycle | `aot/codegen/AOTModuleEmitter.cpp:151` | memory | MD1 to MD7, O1; MD3'', MD8, MD9; MD3', MD3'', MD7', W1 | 09 9.9, 04 4.7; 09 9.9; 09 9.9, 9.14 | Covered |
| R4 H-9 | String constants: publication order; bump pointer | `aot/stubs/AOTStubsConstants.cpp:263` | memory | Z1; private arena; T5 | 07 7.6; 09 9.2 | Covered |
| R4 H-10 | `ProgramSymbols` | `aot/runtime/AOTProgramIdentifiers.cpp:117` | memory | table lock, PT1, 8.9; rows of 7.6 and 7.7 | 07 7.6; 07 7.6, 7.7 | Covered |
| R4 H-11 | Singletons with identity: check, create, plain store | `aot/object/AOTClosureTable.cpp:338` | value..memory | Z1, Z2; Z2 parts 1 to 5, W1, `prepareForThreads` | 07 7.6; 09 9.14 | Covered |
| R4 H-12 | `AllocationSiteRecord` filled and forgotten as a 32-byte struct | `aot/object/AOTAllocationSites.cpp:184` | memory | K1; forgotten through the epoch | 07 7.4, 05 5.10 | Covered |
| R4 H-13 | Free lists popped in line | `aot/codegen/AOTAllocationEmitter.cpp:238` | memory | private | 07 7.3.2, 7.3.3 | Covered |
| R4 H-14 | RegExp: match offsets, legacy statics, shared `lastIndex`, `RegExpLiterals` | `aot/stubs/AOTStubsRegExp.cpp:376` | memory | per thread; pass rule; table lock; rows of 7.6 and 7.7 | 06 6.9, 04 4.8, 07 7.7; 07 7.6, 7.7 | Covered |
| R4 H-15 | Small caches with key and value in different words | `aot/object/AOTObjectWords.h:95` | memory | K1 | 07 7.4 | Covered |
| R4 H-16 | Tables that grow by replacement | `aot/object/AOTKeyOrderTable.cpp:766` | memory | per thread, or freed after a safepoint | 07 7.4, 08 8.9 | Covered |
| R4 H-17 | `CallSiteCache` shared by all stack walks | `aot/runtime/AOTCallSiteCache.cpp:121` | memory | K1 | 07 7.4 | Covered |
| R4 H-18 | C++ containers changed on slow paths without a lock between mutators | `aot/runtime/AOTState.h:43` | memory | one lock per table; K3, Z1 (card); rows of 7.6 and 7.7 | 07 7.7; 07 7.4, 7.6; 07 7.6, 7.7 | Covered |
| R4 H-19 | `StringImpl` counts taken by caches | `aot/runtime/AOTPropertyCaches.cpp:328` | memory | atomic counts; K1 | 06 6.7 | Covered |
| R4 H-20 | Cards | `aot/codegen/AOTBarrierEmitter.cpp:179` | benign | B5 | 08 8.10 | Covered |
| R4 H-21 | Stop requests and stack limits: one mirror, one limit, one flag | `runtime/StackManager.cpp:51` | memory | per thread | 09 9.4, 08 8.4 | Covered |
| R4 H-22 | Await fast paths; whose queue runs the job of a shared promise | `aot/runtime/AOTAsyncAwaitConditions.h:59` | memory | queue per thread; PR1 to PR7; J1; G2, T1, T2, J1; J2, J3, G3 | 09 9.7; 09 9.2, 9.6, 9.7; 02 2.6; 09 9.6, 9.7 | Covered |
| R4 H-23 | `JSON.parse` state keyed by a stack pointer | `aot/runtime/AOTJSONWords.h:39` | memory | private; V3 to V7, `ThreadState` | 07 7.3.3; 07 7.3.1 | Covered |
| R4 H-24 | Method table `mprotect` window | `aot/object/AOTMemberGuards.cpp:1654` | crash | alias; lock | 07 7.3.2 | Covered |
| R4 H-25 | Decisions that tie two caches together | `aot/runtime/AOTGlobalBindings.cpp:47` | value | K2: compiled out | 07 7.5 | Covered |
| R4 H-26 | False sharing | `aot/runtime/AOTPropertyCachesWords.h:211` | performance | private | 07 7.8 | Covered |
| R4 H-27 | A stub walks the `PropertyTable` of a Structure without a lock | `aot/stubs/AOTStubsPropertyRead.cpp:117` | memory | PT1 to PT5; tables are cloned, not stolen | 06 6.5 | Covered |
| R4 H-28 | Realm-wide flips: `haveABadTime`, `captureCellHeaders` | `aot/runtime/AOTInstance.cpp:375` | value..memory | safepoint operation; V9 (`pushed`, `pulled`), Z1 (reset) | 08 8.6; 07 7.3.3, 7.6 | Covered |
| R4 H-29 | "Cleared each collection" words are not roots | `aot/runtime/AOTProgramStatics.cpp:207` | safe if all stop | cleared while stopped, for every view | 08 8.12 | Covered |
| R4 H-30 | `AtMostOne`, `ProvenOnce`, "runs at most once" | `aot/place/AOTProgramTableEncoder.cpp:145` | memory | one realm, one truth; MD3''; Z2 | 04 4.7, 09 9.9 | Covered |
| R4 H-31 | The Instance is found from the callee's realm | `aot/stubs/AOTStubsCallAdapters.cpp:97` | | TLS in adapters | 07 7.3.4 | Covered |
| R4 H-32 | The interpreter's handlers are a third implementation of each protocol | `llint/LowLevelInterpreterAOT.asm:119` | | I1: `m_thread->view` | 10 10.2 | Covered |

### R6, sections 6 to 8: items of the body

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R6 6.5-1 | Both threads pass the sign test of `m_state` | `aot/stubs/AOTStubsCoroutineResumption.cpp:112` | Unsafe | G1 | 09 9.6 | Covered |
| R6 6.5-2 | One activation runs twice from two copies of the frame | `aot/stubs/AOTStubsCoroutineResumption.cpp:603` | Unsafe | G1 | 09 9.6 | Covered |
| R6 6.5-3 | Plain increments of `m_version`: odd for good | `aot/stubs/AOTStubsCoroutineResumption.cpp:547` | Unsafe | G1: one writer | 09 9.6 | Covered |
| R6 6.5-4 | Two saves into one cell | `aot/stubs/AOTStubsCoroutineSuspension.cpp:338` | Unsafe | G1 | 09 9.6 | Covered |
| R6 6.5-5 | `SuspendedAtYield` is published before the words are saved | `aot/stubs/AOTStubsCoroutineSuspension.cpp:861` | Unsafe | G1 | 09 9.6 | Covered |
| R6 6.5-6 | `Running -> Completed` is check, then act; `didThrow()` is a blind store | `aot/runtime/AOTGenerator.cpp:117` | Unsafe | G1 | 09 9.6 | Covered |
| R6 6.5-7 | Async generators: four tests, four stores | `aot/stubs/AOTStubsCoroutineSteps.cpp:159` | Unsafe | G1 at the entry points; J2, J3, G3 | 09 9.6; 09 9.6, 9.7 | Covered |
| R6 6.5-8 | Async functions have no state word: one job or one waiter refers to the frame | `aot/runtime/AOTSavedFrame.cpp:93` | Unsafe | G1 ("nothing to test"), J1; J2, J3, G3 | 09 9.6, 9.7 | Covered |
| R6 7.2-1 | resolve against resolve: two settlements | `runtime/JSPromise.cpp:184` | Value | PR1, PR3, PR6; PR3, PR4, PR6, PR9, PR10, D1 | 09 9.7 | Covered |
| R6 7.2-2 | `then` against `await`: handler in the slot, resume task in the flags | `aot/runtime/AOTAsyncResumption.cpp:152` | Unsafe | PR1, PR2 | 09 9.7 | Covered |
| R6 7.2-3 | `await` against settle: pending again, with any value as context | `aot/stubs/AOTStubsCoroutineSuspension.cpp:488` | Unsafe | PR1, PR2, PR3 | 09 9.7 | Covered |
| R6 7.2-4 | `spillInlineReaction`: reads two words, allocates, rewrites | `runtime/JSPromise.cpp:284` | Unsafe | PR5 | 09 9.7 | Covered |
| R6 7.2-5 | `markAsHandled` rewrites the whole word | `aot/stubs/AOTStubsCoroutineSuspension.cpp:149` | Value | PR4; PR3, PR4, PR6, PR9, PR10, D1 | 09 9.7 | Covered |
| R6 7.3-1 | C++ `enqueue`: `Deque::append` reallocates | `runtime/MicrotaskQueueInlines.h:66` | Unsafe | queue per thread | 09 9.7 | Covered |
| R6 7.3-2 | Stubs reserve, fill and publish a job; "nothing may be queued" in between | `aot/stubs/AOTStubsCoroutineJobs.cpp:42` | Unsafe | queue per thread | 09 9.7 | Covered |
| R6 7.4-5 | The adapter `EnterSavedFrame` drains the queue in machine code | `aot/stubs/AOTStubsCoroutineResumption.cpp:186` | Unsafe | queue per thread; J2, J3, G3 | 09 9.7; 09 9.6, 9.7 | Covered |
| R6 7.5-1 | An `await` goes on without suspending | `aot/stubs/AOTStubsCoroutineSuspension.cpp:142` | Value | F13, F14, PR3 | 07 7.5 | Covered |
| R6 7.5-2 | Hand-over without a job, through three Instance words | `aot/stubs/AOTStubsCoroutinePromises.cpp:263` | Unsafe | private words; "home is me" test in stubs; J2, J3, G3 | 09 9.7; 09 9.6, 9.7 | Covered |
| R6 7.6-1 | `didChangeMicrotaskQueueing` recomputes words of every Instance | `aot/runtime/AOTRuntimeHooks.cpp:116` | | the calling thread's view | 09 9.7 | Covered |
| R6 7.6-2 | `setOnEachMicrotaskTick` switches the fast paths off and on | `bun:src/jsc/bindings/ZigGlobalObject.cpp:512` | Value | member of `ThreadContext` | 11 11.4 | Covered |
| R6 7.6-3 | One async context slot per realm | `runtime/AsyncContextSwapScope.h:39` | Value | one tuple cell per thread | 09 9.7 | Covered |
| R6 8.1-1 | `JSLock` as what makes the mutator unique; `m_ownerThread` | `runtime/JSLock.cpp:91` | | depth count per thread | 09 9.3 | Covered |
| R6 8.1-2 | The atom table is found through the thread and has no lock | `wtf:text/AtomStringImpl.cpp:76` | Unsafe | sharded table | 06 6.7 | Covered |
| R6 8.1-3 | `setLastStackTop`, `stackPointerAtVMEntry`: one each | `runtime/JSLock.cpp:129` | Crash | `ThreadContext` | 07 7.2 | Covered |
| R6 8.1-4 | One heap access bit | `heap/Heap.h:1070` | Unsafe | `m_access` per thread | 08 8.3 | Covered |
| R6 8.1-5 | `machineThreads().addCurrentThread()` | `runtime/JSLock.cpp:142` |  | parked state scanned in place; T3, T4, H3, H4 | 08 8.13; 08 8.14; 09 9.2 | Covered |
| R6 8.1-6 | `samplingProfiler->noticeJSLockAcquisition`: one execution thread | `runtime/SamplingProfiler.cpp:1057` | | samples each thread in turn | 09 9.10 | Covered |
| R6 8.1-7 | `willReleaseLock` drains; `DropAllLocks` parks two words in the `Thread` | `runtime/JSLock.cpp:301` | | own queue; `ReleaseHeapAccessScope` | 09 9.3 | Covered |
| R6 8.2 | Bun finds its VM, loop and global object through TLS | `bun:src/jsc/VirtualMachine.rs:681` | | one Bun VM per thread | 11 11.4 | Covered |

### R6, section 11: hazards H-1 to H-29, D, E

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R6 H-1 | Soft and trap-aware stack limit of "the" thread | `aot/codegen/AOTStackCheckEmitter.cpp:32` | Unsafe | per thread | 09 9.4 | Covered |
| R6 H-2 | `thrownValue` | `aot/runtime/AOTCallSlowPaths.cpp:76` | Value, Crash | private | 09 9.5 | Covered |
| R6 H-3 | `stackPointerOfHandler` loaded into `rsp` | `aot/stubs/AOTStubsExceptions.cpp:86` | Unsafe | private | 09 9.5 | Covered |
| R6 H-4 | `m_exception`, `m_lastException`, trap bit | `runtime/VM.h:511` | Value, Crash | per thread; the word lies in the view | 09 9.5 | Covered |
| R6 H-5 | `topCallFrame`, `topEntryFrame`: callee saves written into another thread's record | `aot/stubs/AOTStubsExceptions.cpp:48` | Unsafe | 7.2; `doVMEntry` takes the context; V8 | 07 7.2, 09 9.3; 07 7.3.4 | Covered |
| R6 H-6 | `callFrameForCatch`, `targetMachinePCForThrow` and the like | `aot/stubs/AOTStubsExceptions.cpp:81` | Unsafe | 7.2; V8 | 07 7.2; 07 7.3.4 | Covered |
| R6 H-7 | `misfit`, `pendingBarrierOwner`, `stepStatus`, `encodedHostCallReturnValue`, `regExpMatchOutput` | `aot/runtime/AOTVMState.h:75` | Value | private; 7.2 | 07 7.2, 7.3.3 | Covered |
| R6 H-8 | `frameThatMayContinue` | `aot/stubs/AOTStubsCoroutineResumption.cpp:557` | Value | private | 07 7.5 F14 | Covered |
| R6 H-9 | `waiterToResume` and two more words: double resume | `aot/stubs/AOTStubsCoroutineResumption.cpp:204` | Unsafe | private; J2, J3, G3; V3 to V7, `ThreadState` | 09 9.7; 09 9.6, 9.7; 07 7.3.1 | Covered |
| R6 H-10 | `nextEntryEndsJob`, `enteredFrame`, `stepInProgress` and the like | `aot/runtime/AOTAsyncResumption.cpp:153` | Unsafe | private; V3 to V7, `ThreadState` | 09 9.7; 07 7.3.1 | Covered |
| R6 H-11 | `entryScope`, `didEnterVM`, entry counts, deferral counts | `runtime/VM.h:1447` | Crash | `ThreadContext` | 07 7.2 | Covered |
| R6 H-12 | The realm's async context slot | `runtime/AsyncContextSwapScope.h:39` | Value | per thread | 09 9.7 | Covered |
| R6 H-13 | An event is consumed by the first thread that polls | `runtime/VMTraps.cpp:468` | semantic | one trap word per thread; stop counts states | 08 8.4, 09 9.10 | Covered |
| R6 H-14 | `json.didUnwindTo(sp)` | `aot/runtime/AOTUnwinding.cpp:110` | not read | private | 07 7.3.3 | Covered |
| R6 H-15 | `CallSiteCache` | `aot/runtime/AOTCallSiteCache.cpp:85` | Unsafe | K1 | 07 7.4 | Covered |
| R6 H-16 | `NativeExecutable::indexOfBoxedCall` | | none | | | Covered |
| R6 H-17 | `constructWords`: new key with the old construct entry | `aot/stubs/AOTStubsCallAdapters.cpp:383` | Unsafe | private cache | 07 7.3.3 | Covered |
| R6 H-18 | The per-VM `CallLinkInfo` for engine callees | `aot/runtime/AOTRuntimeTable.cpp:222` | not read | none needed: virtual mode, no counter is written | | Covered |
| R6 H-19 | `prepareInlineAllocation`: header, then allocator | `aot/runtime/AOTOperationScope.h:89` | ARM64 order | private | 07 7.3.3 | Covered |
| R6 H-20 | `Generator::m_state`: load, test, store | `aot/stubs/AOTStubsCoroutineResumption.cpp:112` | Unsafe | G1 | 09 9.6 | Covered |
| R6 H-21 | State published before the frame is saved | `aot/stubs/AOTStubsCoroutineSuspension.cpp:861` | Unsafe | G1 | 09 9.6 | Covered |
| R6 H-22 | `SavedFrame`: version lock with plain increments, one writer assumed | `aot/stubs/AOTStubsCoroutineSuspension.cpp:272` | Unsafe | G1 | 09 9.6 | Covered |
| R6 H-23 | `m_promiseOrWaiter` and the byte that says how to read it | `aot/runtime/AOTSavedFrame.cpp:177` | Unsafe | G1 (writers only); PR8, G4 | 09 9.6; 09 9.6, 9.7 | Covered |
| R6 H-24 | `JSAsyncGenerator` internal fields | `aot/stubs/AOTStubsCoroutineSteps.cpp:159` | Unsafe | G1; J2, J3, G3 | 09 9.6; 09 9.6, 9.7 | Covered |
| R6 H-25 | `JSPromise`: slot and flags disagree | `runtime/JSPromise.cpp:254` | Unsafe | PR1 to PR7; PR8, G4; PR3, PR4, PR6, PR9, PR10, D1 | 09 9.7; 09 9.6, 9.7 | Covered |
| R6 H-26 | `remainingElementsCount`; the values array written without a lock | `aot/stubs/AOTStubsCoroutinePromises.cpp:242` | Value | J1; D1; PR3, PR4, PR6, PR9, PR10, D1 | 09 9.7, 06 6.10; 09 9.7 | Covered |
| R6 H-27 | `MicrotaskQueue`: two producers share a slot | `aot/stubs/AOTStubsCoroutineJobs.cpp:42` | Unsafe | queue per thread | 09 9.7 | Covered |
| R6 H-28 | `m_markedBefore` | `aot/stubs/AOTStubsCoroutineJobs.cpp:199` | Unsafe | marked while parked | 09 9.7 | Covered |
| R6 H-29 | Return addresses edited on the stack | `aot/runtime/AOTUnwinding.cpp:297` | safe given H-5 | own view, own stack | 09 9.5 | Covered |
| R6 D-1 | Await without suspension and the in-line job loop assume a quiet queue | `aot/runtime/AOTJobs.h:34` | Value | queue per thread; PR3 | 09 9.7 | Covered |
| R6 D-2 | `DeferGCForAWhile` in prologue polls: "this call does not collect" | `aot/runtime/AOTExceptionOperations.cpp:63` | Unsafe | S2, `NoSafepointScope` | 08 8.4 | Covered |
| R6 D-3 | Loops without polls | `aot/codegen/AOTHintEmitter.cpp:102` | latency | S3 | 04 4.11 | Covered |
| R6 D-4 | A `SavedFrame` restored on another thread keeps values derived from the pinned register | `aot/codegen/AOTPrimitiveEmitter.cpp:204` | Unsafe | G1, J1; G2, T1, T2, J1 | 09 9.6, 9.7; 09 9.2, 9.6, 9.7; 02 2.6 | Covered |
| R6 D-5 | Objects and closures that lie in a frame | `aot/stubs/AOTStubsCalls.h:21` | Unsafe | C2 | 04 4.3 | Covered |
| R6 E-1 | A second OS thread has no Bun `VirtualMachine`; `m_bunVM` is the creator's | `bun:src/jsc/bindings/ZigGlobalObject.cpp:1280` | | one Bun VM per thread | 11 11.4 | Covered |
| R6 E-2 | `checkIfNextTickWasCalledDuringMicrotask` uses TLS inside a drain | `bun:src/jsc/bindings/ZigGlobalObject.cpp:512` | | per thread | 11 11.4 | Covered |
| R6 E-3 | One alternate signal stack | | diagnostics | the handler runs on the faulting stack | 09 9.5 | Covered |

### Problems that the hazard lists do not contain

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| N-1 | A thread ID is reused while generators, frames and reactions still carry it | 09 9.2 step 14 | memory | G1, J1; G2, T1, T2, J1 | 09 9.6, 9.7; 09 9.2, 9.6, 9.7; 02 2.6 | Covered |
| N-2 | Tasks posted while the home thread ends; life of the `ThreadContext` under `post` | 09 9.7 | memory, lost job | J1; G2, T1, T2, J1 | 09 9.7; 09 9.2, 9.6, 9.7; 02 2.6 | Covered |
| N-3 | A thread that had no heap access during a stop runs neither `didResume()` nor E2 | 08 8.3 | memory | E2, E4; H12, E2 | 07 7.5, 08 8.3; 08 8.3; 07 7.5 | Covered |
| N-4 | Thread start and end touch allocator lists, views and registries without heap access | `heap/BlockDirectory.cpp:232` | memory | 8.14; T3, T4, H3, H4 | 08 8.14, 09 9.2; 08 8.14; 09 9.2 | Covered |
| N-5 | The completion of an async module runs the bodies of waiting parents on the completing thread | `runtime/CyclicModuleRecord.cpp:899` | memory | MD6; MD3'', MD8, MD9 | 09 9.9 | Covered |
| N-6 | The evaluation lock is held over the whole body of the entry module | 09 9.9 MD3 | deadlock | MD3, MD7; MD3', MD3'', MD7', W1 | 09 9.9; 09 9.9, 9.14 | Covered |
| N-7 | `process.exit` on a spawned thread posts to a main thread that is blocked | 09 9.11 | deadlock | 9.10, 9.11; X1 | 09 9.11 | Covered |
| N-8 | Inline synchronizing operations do not compare the epoch | 04 4.6.6, 4.13; 09 MD1 | value | E2; E2a | 07 7.5 | Covered |
| N-9 | A change of an object that does not report yet, between a fill's walk and `didBecomePrototype` | `aot/runtime/AOTPropertyCaches.cpp:505` | value | E3; E3 | 07 7.5 | Covered |
| N-10 | The modules lock stands below two locks that code under it takes | 08 8.15 | assertion | MD3, lock order; MD3', H15 | 08 8.15; 09 9.9 | Covered |
| N-11 | `VMState::instances` as the list of views | `aot/runtime/AOTChainDependents.cpp:246` | memory | 7.3.3; `VMState::views` | 07 7.3.3 | Covered |

## A.3 The heap and the flow analysis (reports R5, R3)

### R5 section 2.6: what in `Heap` and `MarkedSpace` assumes one mutator

Rows in the order of the report's table. Paths are `heap/Heap.h` unless another is given.

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 2.6-1 | `m_worldState`: one access bit, one stopped bit, one conn bit, one epilogue bit, one waiting bit | `heap/Heap.h:1075` | C, L | H1, H3, H12; `m_access`, `m_stopRequested`, `m_stopLock` | 08 8.3, 8.4, 8.10 | Covered |
| R5 2.6-2 | `m_worldIsStopped`: written by the conductor, read by assertions and `drainInParallelPassively` | `heap/Heap.h:1076` | - | H0; set by collections only | 08 8.1, 8.6 | Covered |
| R5 2.6-3 | `m_mutatorState`: plain, with a release assertion in `AllocatingScope` | `heap/Heap.h:916` | C | `MutatorContext` | 08 8.2, 8.8, 8.11 | Covered |
| R5 2.6-4 | `m_hasAllocatedSinceLastCollection`: plain, set by every slow path | `heap/Heap.h:917` | - | relaxed store, read while stopped | 08 8.8 | Covered |
| R5 2.6-5 | Bytes allocated this cycle (three counters): plain `+=`, read by the scheduler | `heap/Heap.h:943-946` | V | per thread, flushed to atomics | 08 8.2, 8.8 | Covered |
| R5 2.6-6 | `m_extraMemoryBytesSinceLastCollectionCheck`, `m_deprecatedExtraMemorySize`: plain | `heap/Heap.h:974-975` | V | per thread; relaxed atomic | 08 8.2, 8.8 | Covered |
| R5 2.6-7 | `m_extraMemorySize`: compare-and-swap loop | `heap/Heap.h:973` | ok | safe already | | Covered |
| R5 2.6-8 | `m_bytesMarkedThisCycle`: atomic | `heap/Heap.h:957` | ok | safe already | | Covered |
| R5 2.6-9 | `m_bytesAbandonedSinceLastFullCollect`: plain `+=` | `heap/Heap.h:947` | V | relaxed atomic | 08 8.8 | Covered |
| R5 2.6-10 | `m_incrementBalance`: plain double in `performIncrement` | `heap/Heap.h:959` | M | `performIncrement` off | 08 8.10 | Covered |
| R5 2.6-11 | `m_mutatorSlotVisitor`: one visitor, not reentrant | `heap/Heap.h:986` | M | `performIncrement`, `collectInMutatorThread` off | 08 8.10 | Covered |
| R5 2.6-12 | `m_mutatorMarkStack`: plain append in C++ and in stubs | `heap/Heap.h:987` | M | per thread; view's `heap` words | 08 8.2 | Covered |
| R5 2.6-13 | `m_storedCellLog`: plain cursor bump in C++ and in a stub | `heap/Heap.h:988` | M | per thread | 08 8.2 | Covered |
| R5 2.6-14 | `m_barriersExecuted`: plain `++` | `heap/Heap.h:1088` | V | per thread counter | 08 8.2 | Covered |
| R5 2.6-15 | `m_deferralDepth`: plain `++`, `--` | `heap/Heap.h:1049` | M | per thread | 08 8.2, 8.4 | Covered |
| R5 2.6-16 | `m_didDeferGCWork`: plain | `heap/Heap.h:1079` | M | per thread | 08 8.2 | Covered |
| R5 2.6-17 | `m_mutatorDidRun`, `m_mutatorExecutionVersion`: the mutator stores `true` after each resume; the conductor derives "constraints must run again" | `heap/Heap.h:1078,1081` | - | stored by the conductor in `cancelStop()` | 08 8.4 | Covered |
| R5 2.6-18 | `m_protectedValues`: plain hash set | `heap/Heap.h:977` | M | `Heap::m_lock` or a leaf lock | 08 8.11 | Covered |
| R5 2.6-19 | `m_markListSet`: plain hash set of spilled `MarkedVectorBase`s | `heap/Heap.h:978` | M | sharded per thread | 08 8.2 | Covered |
| R5 2.6-20 | `m_strongSet`: handle allocation and release, unlocked | `heap/Heap.h:990` | M | per thread, with a return list | 08 8.2 | Covered |
| R5 2.6-21 | `m_arrayBuffers`: plain vectors | `heap/Heap.h:972` | M | lock; TA7 | 08 8.11, 06 6.8 | Covered |
| R5 2.6-22 | `m_weakGCHashTables`, `m_dirtyWeakGCHashTables`: plain | `heap/Heap.h:1051-1052` | M | lock (first member named) | 08 8.11 | Covered |
| R5 2.6-23 | `m_detachedWeakBlocks`, `m_pooledWeakBlocks`, their counts: plain lists | `heap/Heap.h:1011-1014` | M | `Heap::m_weakBlockPoolLock` | 08 8.11, 8.15 | Covered |
| R5 2.6-24 | Replaced string buffers: plain append, cleared by "found on no stack" | `heap/Heap.h:1027` | M | `m_replacedStringBuffers` per thread, merged at a stop | 08 8.2, 8.12, 06 6.7 | Covered |
| R5 2.6-25 | `m_observers`, `m_gcCompletionCallbacks`: plain | `heap/Heap.h:1035-1037` | M | lock; callbacks posted to main | 08 8.11, 8.12 | Covered |
| R5 2.6-26 | `m_sweeper`, `m_stopIfNecessaryTimer`, activity callbacks: timers of the VM's run loop | `heap/Heap.h:1030-1033` | - | main loop; spawned threads post; timer compiled out | 08 8.1, 8.8, 8.11 | Covered |
| R5 2.6-27 | `m_isCollectionPrevented`, `m_auxiliaryEvacuationIsDue`: plain | `heap/Heap.h:933-934` | C, L | H13; evacuation compiled out | 08 8.1, 8.10 | Covered |
| R5 2.6-28 | Lazily made subspaces: make, fence, publish, no lock | `heap/Heap.cpp:3514-3568` | M | `Heap::m_lock`, look again | 08 8.8, 07 7.6 | Covered |
| R5 2.6-29 | `MarkedSpace::m_capacity`: plain `+=` | `heap/MarkedSpace.h:219` | V | `MarkedSpace::m_lock` | 08 8.8 | Covered |
| R5 2.6-30 | `MarkedSpace::m_blocks`: hash set and filter, plain | `heap/MarkedSpace.h:227` | M | `MarkedSpace::m_lock` | 08 8.8, 8.11 | Covered |
| R5 2.6-31 | `m_preciseAllocations`, `m_preciseAllocationSet`: plain | `heap/MarkedSpace.h:210-211` | M | `MarkedSpace::m_lock`; H8 | 08 8.8, 8.11, 8.12 | Covered |
| R5 2.6-32 | `m_newActiveWeakSets`: plain list splice | `heap/MarkedSpace.h:230` | M | `MarkedSpace::m_lock` | 08 8.8 | Covered |
| R5 2.6-33 | `m_isIterating`, `HeapIterationScope`: `stopAllocating()` without stopping anybody | `heap/MarkedSpace.cpp:506-518` | M | safepoint operation, points 4 to 7 | 08 8.6 | Covered |
| R5 2.6-34 | `VM::m_lastStackTop`, release assertion in `sanitizeStackForVM` | `runtime/VM.h:1434` | C | per thread | 08 8.2, 8.13 | Covered |
| R5 2.6-35 | `VM::m_currentWeakRefVersion`: one counter | `runtime/VM.h:1507` | V | `m_keptAlive` per thread | 08 8.11 | Covered |

### R5 section 11.1: allocation

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 A1 | Load, compare, add on a free list that two threads share: one cell handed out twice | `jit/AssemblyHelpers.cpp:1070-1124` | M | allocators per thread, reached through private words of the view | 08 8.8 | Covered |
| R5 A2 | Refill writes three words: a mixed interval | `jit/AssemblyHelpers.cpp:1126-1180` | M | the same | 08 8.8 | Covered |
| R5 A3 | `AllocationSiteRecord` read in pieces while it is rewritten | `aot/object/AOTAllocationSites.cpp:121-202` | M | K1, H12 | 07 7.4, 08 8.3 | Covered |
| R5 A4 | Lazily filled records and inline allocations; ARM64 sees `allocator` before `header` | `aot/runtime/AOTOperationScope.h:89-97` | M | K1 | 07 7.3.3, 7.4 | Covered |
| R5 A5 | `AllocatingScope` release assertion with two threads in slow paths | `heap/AllocatingScope.h` | C | per thread | 08 8.8 | Covered |
| R5 A6 | Lost updates of allocation counters | `heap/Heap.cpp:2683-2698` | V | atomic adds | 08 8.8 | Covered |
| R5 A7 | `m_blocks` corrupted; the stack scan misses a block | `heap/MarkedSpace.cpp:405-411,562-569` | M | `MarkedSpace::m_lock` | 08 8.8 | Covered |
| R5 A8 | `m_newActiveWeakSets` splice | `heap/MarkedSpace.cpp:571-577` | M | the same lock | 08 8.8 | Covered |
| R5 A9 | Registration of precise allocations | `heap/CompleteSubspace.cpp:175-183` | M | the same lock | 08 8.8 | Covered |
| R5 A10 | `reallocatePreciseAllocationNonVirtual` frees a large butterfly under a reader | `heap/CompleteSubspace.cpp:186-240` | M | SW8 | 08 8.8, 06 6.2 | Covered |
| R5 A11 | Two threads make one lazy subspace twice | `heap/Heap.cpp:3514-3568` | M | `Heap::m_lock` | 08 8.8 | Covered |
| R5 A12 | `willStartIterating()` wipes other threads' free lists | `heap/MarkedSpace.cpp:506-518` | M | safepoint operation, points 4 to 7 | 08 8.6 | Covered |
| R5 A13 | `m_numberOfLargerCells`, `m_residentBlocks`, `MarkedBlockCage::m_reserve` | `heap/CagedAlignedMemoryAllocator.cpp:178-189` | M/V | the cage's lock; relaxed atomic | 08 8.8 | Covered |
| R5 A14 | The owed `RangeError` is thrown in a thread that did not allocate | `runtime/StackManager.cpp:73-83` | V | flag per thread | 08 8.8, 09 9.10 | Covered |
| R5 A15 | Synchronous full collection from a slow path needs all threads stopped | `heap/Heap.cpp:2895-2901` | L | requester waits as `Parked`; H7; shared reserve | 08 8.8, 8.10 | Covered |
| R5 A16 | ARM64: no fence between initializing a cell and publishing it | `aot/codegen/AOTPrimitiveEmitter.cpp:154-168` | M | F1, F1', F2, F4 | 04 4.10 | Covered |
| R5 A17 | A publishing store must not move above the initializing stores | `aot/codegen/AOTRuntimeCallEffects.cpp:74` | M | B3 fence that ends initialization; F1, F1' | 04 4.9, 4.10 | Covered |

### R5 section 11.2: barriers and remembered sets

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 B1 | In-line append to the mark stack: two threads use one index | `aot/stubs/AOTStubsBarriers.cpp:40-70` | M | per thread | 08 8.2, 8.10 | Covered |
| R5 B2 | The same at the end of a segment: write past the segment | `heap/GCSegmentedArrayInlines.h:189-194` | M | per thread | 08 8.10 | Covered |
| R5 B3 | `StoredCellLog`: cursor race, `grow()` under a lock that writers skip | `heap/StoredCellLog.cpp:44-60` | M | per thread | 08 8.10 | Covered |
| R5 B4 | `RememberedSlotSet::record`: test, set, append to a `Vector` | `heap/RememberedSlot.h:47-59` | M | K3 | 07 7.4, 08 8.10 | Covered |
| R5 B5 | Card bytes stored by compiled code | `aot/codegen/AOTBarrierEmitter.cpp:179-185` | ok | idempotent; visible at the stop | 08 8.10 | Covered |
| R5 B6 | `setCellState(PossiblyGrey)` by two threads | `heap/Heap.cpp:1397-1402` | ok | designed to be racy | 08 8.10 | Covered |
| R5 B7 | Re-whitening compare-and-swap in `addToRememberedSet` during a full collection | `heap/Heap.cpp:1358-1392` | ? | H2 (cost: W-8.4) | 08 8.10 | Covered |
| R5 B8 | `m_barriersExecuted` | `heap/Heap.cpp:1356` | V | per thread | 08 8.2 | Covered |
| R5 B9 | Barrier removal and the cached threshold need cooperative stops | `aot/represent/AOTRemoveRedundantBarriers.cpp:77-90` | M | S2, S4, S5 | 04 4.11, 08 8.4 | Covered |
| R5 B10 | `pendingBarrierOwner`: one word per realm | `aot/codegen/AOTBarrierEmitter.cpp:215-273` | C | private part of the view | 04 4.14 | Covered |
| R5 B11 | `didChangeBarrierThreshold` must reach every per-thread copy | `aot/runtime/AOTHeapHooks.cpp:149-157` | M | H3, T3 | 08 8.4, 8.14 | Covered |

### R5 section 11.3: "no marker is reading" used as "nobody is reading"

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 C1 | Granules of an Extension move in place under a reader | `aot/object/AOTObjectStorage.cpp:182-207` | M | CO6, unpacked Extension | 05 5.5, 08 8.10 | Covered |
| R5 C2 | Two appenders lose a granule bit | `aot/object/AOTObject.h:355` | V, M | constant bit word | 05 5.5 | Covered |
| R5 C3 | A store into a replaced Extension is lost | `aot/object/AOTObjectStorage.cpp:209-221` | V | CO6, with the owner barrier | 05 5.5, 08 8.10 | Covered |
| R5 C4 | A stub grows a `CompactTable` without the cell lock | `aot/stubs/AOTStubsCollections.cpp:621-622` | M | MS1, MS5, MS7 | 06 6.6 | Covered |
| R5 C5 | `becomeReified` changes the word that callers jump through | `aot/object/AOTSlimClosure.cpp:178-195` | M | one load, decide on the loaded word | 05 5.8 | Covered |
| R5 C6 | The sequence lock of a `SavedFrame` has one writer | `aot/runtime/AOTSavedFrame.h:271-316` | M | G1, G2, G3 | 09 9.6 | Covered |
| R5 C7 | ARM64: butterfly changes without nuke and fences | `runtime/JSObjectInlines.h:110-133` | M | SW7; the marker gives up on a pair that does not agree | 06 6.2, 6.3, 08 8.10 | Covered |

### R5 section 11.4: stopping, scanning, phases

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 D1 | One `hasAccessBit`; a second thread that acquires access crashes | `heap/Heap.cpp:2169-2194` | C | `m_access` per thread | 08 8.2, 8.3 | Covered |
| R5 D2 | "The conducting mutator is the stopped mutator": the others run through Begin, Fixpoint, End | `heap/Collector.cpp:834-838` | M | collector thread conducts; `requestStop()` | 08 8.4, 8.10 | Covered |
| R5 D3 | No poll for the collector in loops; prologue polls defer; polls omitted; RegExp, long C++ | `aot/codegen/AOTHintEmitter.cpp:102-150` | L | S2, S3, S3'; trap bit of the conductor (lists: W-8.6) | 04 4.11, 08 8.4, 8.5 | Covered |
| R5 D4 | Threads blocked in native code with access held | `bun:src/jsc/bindings/BunJSCEventLoop.cpp:133-141` | L | H1, H15 | 08 8.3, 8.15 | Covered |
| R5 D5 | The holder of the conn blocks, exits, or waits for a parked thread | `heap/Collector.cpp:659-676` | L | collector thread always conducts | 08 8.10 | Covered |
| R5 D6 | `m_lastStackTop` release assertions | `runtime/VM.cpp:1847-1863` | C | per thread | 08 8.13 | Covered |
| R5 D7 | `Heap::collect` asserts the VM's atom table is the thread's | `heap/Heap.cpp:1892-1915` | C | every thread points at the VM's sharded table | 06 6.7 | Covered |
| R5 D8 | Shared deferral depth: unbalanced, collection inside a `DeferGC` scope | `heap/HeapInlines.h:168-184` | M | per thread | 08 8.2 | Covered |
| R5 D9 | `DeferGC` must also mean "this thread does not park" | `heap/HeapInlines.h:180-209` | M | `NoSafepointScope`, S4, S5 | 08 8.4, 04 4.11 | Covered |
| R5 D10 | Two threads drain with one visitor | `heap/Heap.cpp:3469-3497` | M | off | 08 8.10 | Covered |
| R5 D11 | One thread runs the epilogue beside running mutators | `heap/Heap.cpp:2265-2285,2356-2382` | M | H12, H8; epilogue split | 08 8.3, 8.12 | Covered |
| R5 D12 | `mutatorWaitingBit` and `compareAndPark` as a condition variable for one waiter | `heap/Heap.cpp:2308-2316` | L | waits specified | 08 8.3, 8.6, 8.10 | Covered |
| R5 D13 | Suspension by signal of a registered thread that is not parked | `heap/MachineStackMarker.cpp:159-216` | M | every thread in `m_threads` is scanned; not in `MachineThreads` | 08 8.13 | Covered |
| R5 D14 | `preventCollection()` does not nest: one flag, one held lock | `heap/Heap.cpp:3428-3457` | C/L | H13 | 08 8.10 | Covered |
| R5 D15 | `m_protectedValues`, `m_markListSet`, `m_strongSet`, `m_weakGCHashTables`, `m_observers` | `heap/Heap.h:977-1052` | M | locks, shards, return list | 08 8.2, 8.11 | Covered |
| R5 D16 | Scratch words of the Instance that hold cells and are no roots | `aot/codegen/AOTCodegenScratchWords.h:27-35` | M | private part of the view; S2 | 07 7.3.3 | Covered |

### R5 section 11.5: weak things and finalizers

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 E1 | `WeakSet::allocate`, `deallocate` per block, unsynchronized | `heap/WeakSetInlines.h:35-48` | M | the block's header lock; `m_weakBlockPoolLock` | 08 8.11 | Covered |
| R5 E2 | Destructors and `WeakHandleOwner::finalize` run on whichever thread sweeps | `heap/MarkedBlock.cpp:824` | M/V | H6 to H10 | 08 8.11, 11 11.3, 11.7 | Covered |
| R5 E3 | Another thread's `SweepingScope` suppresses this thread's collection | `heap/Heap.cpp:2965-3053` | V | per thread | 08 8.11 | Covered |
| R5 E4 | `IncrementalSweeper`, `freeBlock` from the run loop | `heap/IncrementalSweeper.cpp:95-127` | M | main loop; `MarkedSpace::m_lock` | 08 8.11 | Covered |
| R5 E5 | One `WeakRef` version for all threads: a target dies in the middle of another thread's job | `runtime/VM.h:1217` | V | `m_keptAlive` | 08 8.11, 02 2.6 | Covered |
| R5 E6 | `deref()`: test, write version, barrier | `runtime/JSWeakObjectRef.h:49-50` | ok | `m_keptAlive` | 08 8.11 | Covered |
| R5 E7 | Which thread runs cleanup callbacks; `deferredWorkTimer` expects the API lock holder | `runtime/JSFinalizationRegistry.cpp:103-172` | V | posted to the creator's inbox | 08 8.11 | Covered |
| R5 E8 | `GCIncomingRefCountedSet::addReference` | `heap/GCIncomingRefCountedSetInlines.h:47-64` | M | lock; TA7 | 08 8.11 | Covered |
| R5 E9 | Replaced string buffers need all stacks and a lock for the append | `heap/Heap.cpp:2344-2349` | M | per thread; conductor with all stacks | 08 8.12 | Covered |
| R5 E10 | Caches zeroed while stopped; no value of one crosses a `collects` operation | `aot/runtime/AOTHeapCacheRegistry.cpp:132-160` | ok | S2; for every thread's registry | 08 8.12 | Covered |
| R5 E11 | `ImmortalCells::add` | `aot/runtime/AOTImmortalCells.cpp` | ok | locked | 07 7.7 | Covered |
| R5 E12 | `VMState::instances` iterated unlocked while stopped | `aot/runtime/AOTHeapHooks.cpp:145-146` | ok | H3, T3, T4; `VMState::views` | 08 8.14, 07 7.3.3 | Covered |

### R5: items of the body without a number

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R5 1.5 | Two uses of "the" heap in the marker: `scheduleSoon()`, `drainInParallelPassively` reads one `m_worldState` | `heap/SlotVisitor.cpp:893,966` | L | first compiled out, second reworded | 08 8.1 | Covered |
| R5 3.6-3 | `IsoSubspace::m_lowerTierPreciseFreeList`, `m_remainingLowerTierPreciseCount`: unlocked | `heap/IsoSubspace.cpp:81-113` | M | `MarkedSpace::m_lock`; H6 | 08 8.8, 8.11 | Covered |
| R5 3.6-4 | The conductor rewrites every allocator's free list | `heap/LocalAllocator.cpp:102-114` | M | S2; all threads parked | 08 8.8 | Covered |
| R5 3.6-5 | Thread exit: `stopAllocatingForGood()`, `~LocalAllocator` | `heap/LocalAllocator.cpp:79-100` | C | T4 | 08 8.14 | Covered |
| R5 3.6-6 | C++ allocation goes to the VM's one allocator set | `heap/HeapInlines.h:242-245` | M | `VM::clientHeap` is an accessor | 08 8.8 | Covered |
| R5 5.4 | Deferred root regions are visited by markers while mutators run | `heap/RememberedSlot.h:237-270` | M | H4 | 08 8.14 | Covered |
| R5 6.6 | Unmarked inner cells lose their header at the end of marking | `aot/runtime/AOTHeapHooks.cpp:198-207` | M | all threads parked | 04 4.8 | Covered |
| R5 7.4 | Scanning N stacks: registration, parked state, one conductor | `heap/MachineStackMarker.cpp:227-244` | M | `m_parkedState`, valid until `acquireAccess()` returns | 08 8.3, 8.13 | Covered |
| R5 8.4 | Four facts that forbid suspension at an arbitrary instruction | `heap/SlotVisitor.cpp:188-198` | M | S2, no signals | 08 8.4 | Covered |
| R5 9.3 | `sweepSynchronously`, stealing, shutdown sweep on the calling thread | `heap/Heap.cpp:1437-1457` | M | H6, H7 | 08 8.11 | Covered |
| R5 9.5 | Butterfly evacuation looks at one stack | `heap/Heap.cpp:1570-1575` | M | compiled out | 08 8.1 | Covered |
| R5 13-12 | Operations declared not to collect that can block | `aot/runtime/AOTConstantOperations.cpp:71-79` | L | S4, S5, `prepareForThreads()` | 08 8.4, 04 4.11 | Covered |

### R3 section 11: hazards H1 to H15

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R3 H1 | A reader that holds a reference sees all stores that preceded its publication | `aot/valueflow/AOTValueFlowResultsOrder.cpp:228-254` | M | M6, F1, F1', F2, O2, O3 | 04 4.7, 4.10 | Covered |
| R3 H2 | "Stored at most once" used as "constant after the store" | `aot/valueflow/AOTValueFlowResultsLocations.cpp:408-466` | V, M | O1 | 04 4.7 | Covered |
| R3 H3 | "At most once" rests on one evaluation per module | `aot/valueflow/AOTValueFlowResultsExecutions.cpp:81` | M | MD1 to MD7, Z2 | 09 9.9, 07 7.6 | Covered |
| R3 H4 | Stretch proofs end only at this thread's effects | `aot/ir/AOTSchema.def` | M | C1, `TryGetSlot`, `Storage`, CO3 | 04 4.5, 4.6 | Covered |
| R3 H5 | Declared effects are all that happens between two operations; a tested value is loaded again | `aot/ir/AOTEffects.h:64-160` | V, M | C1, `synchronizes`, `windowStorage` | 04 4.1, 4.4, 4.8, 4.9 | Covered |
| R3 H6 | `DoesNotEscape`, borrowed parameters, frame arrays: the callee runs on this thread | `aot/valueflow/AOTValueFlowResultsEscape.cpp:695-739` | M | C2, `args` retained | 04 4.3, 03 3.5.2 | Covered |
| R3 H7 | `ReuseEnvironments`' free cell and other per-site state in statics | `aot/passes/AOTReuseEnvironments.cpp:214` | M | private statics, K1, Z1 | 04 4.8, 07 7.3.2 | Covered |
| R3 H8 | `ProgramInstances::AtMostOne`; per-thread state must not lie in the Instance | `aot/valueflow/AOTValueFlowResultsBuiltins.cpp:1184-1206` | note | view | 07 7.3 | Covered |
| R3 H9 | Reads of native containers write | `aot/builtins/map-set.builtins.ts:191-198` | M | SW1 to SW9, MS1 to MS9, rope by compare-and-swap | 06 6.2, 6.6, 6.7 | Covered |
| R3 H10 | Native cells are one per program | `aot/builtins/generator/lib.ts:378-379` | soundness | R3, R4 | 04 4.2.3 | Covered |
| R3 H11 | Native code on another thread reads and writes escaped objects | `aot/valueflow/AOTValueFlowBodies.h:340-353` | M | `threads` column; R5'; N-j | 11 11.1, 11.2, 04 4.2.3 | Covered |
| R3 H12 | A main-only async function continues on the settling thread | `aot/valueflow/AOTValueFlowRulesPromises.cpp:155-179` | V, M | J1 to J3, G2 | 09 9.6, 9.7, 02 2.6 | Covered |
| R3 H13 | Two threads add properties: slot, order word, side object | `aot/place/AOTKeyOrders.cpp` | M | object lock, 5.6 (e), (f) | 05 5.6 | Covered |
| R3 H14 | Validation state is racy | `aot/object/AOTObjectInternal.cpp:42` | tooling | private state, atomic exchange | 04 4.14 | Covered |
| R3 H15 | One `escaped` set, one `lost` set: a precision cliff | `aot/valueflow/AOTValueFlow.h:159-168` | precision | Shareable is cheap | 04 4.2.5 | Covered |

### R3 section 5: results of flow that are not plain union facts

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R3 5-U | Union facts (kinds 5, 8 to 13, 15, 21, 22) hold only if every function a thread runs is in the solve | `aot/valueflow/AOTValueFlowResults.cpp:403-542` | M | `on: "NewThread"`, `ThreadEntries`, `StartsThread` | 04 4.2.1, 03 3.5.1 | Covered |
| R3 5-1 | Kinds 1 to 3: "the slot holds a member of the proven set" needs publication | `aot/valueflow/AOTValueFlowPositions.cpp:743-878` | M | M6, F1, CO2 | 04 4.10, 05 5.2 | Covered |
| R3 5-4 | "Symbol created once" rests on a count | `aot/valueflow/AOTValueFlowResultsSymbols.cpp:49` | V | counts hold; Z1 | 04 4.7, 07 7.6 | Covered |
| R3 5-6 | Proven contents of containers that are not safe to mutate at once | `aot/valueflow/AOTValueFlowPositions.cpp:474` | M | 06 | 06 6.3, 6.6 | Covered |
| R3 5-7 | A change of array kind between test and access | `aot/valueflow/AOTValueFlowPositions.cpp:478-479` | M | SW1, `StorageKind` | 04 4.6.3 | Covered |
| R3 5-14 | `MemberMutability::Fixed` needs `isThereFromTheStart` | `aot/valueflow/AOTValueFlowResultsClasses.cpp:755` | M | O2, Z2 | 04 4.7, 07 7.6 | Covered |
| R3 5-16 | A class definition runs at most once | `aot/valueflow/AOTValueFlowResultsClasses.cpp:719` | V | counts hold; Z2 | 04 4.7, 07 7.6 | Covered |
| R3 5-17 | `StoreCount::AtMostOnce`, `isUnchangedDuringActivation` | `aot/ir/AOTMemoryLocationAccesses.cpp:132-152` | V, M | O1, O2 | 04 4.7 | Covered |
| R3 5-18 | `Initialization`: reads without a test | `aot/valueflow/AOTFlowOrderOwner.cpp:597` | M | O1, O2 | 04 4.7 | Covered |
| R3 5-19 | Constructor field orders hold up to the leak | `aot/valueflow/AOTFlowConstructionOrders.cpp:38` | M | O2 | 04 4.7 | Covered |
| R3 5-20 | `OperandSummaries`: the callee to which an object is lent runs on the same thread | `aot/valueflow/AOTFlowSinkEquations.h:17-63` | M | C2 | 04 4.3 | Covered |
| R3 5-23 | A function runs at most once | `aot/valueflow/AOTValueFlowResultsExecutions.cpp:94-133` | M | MD3; protocols of 7.6 | 04 4.7, 07 7.6 | Covered |
| R3 5.1 | `FlowModuleOrdering::CompletesBefore` needs "evaluated" published with release | `aot/valueflow/AOTFlowOrder.h:69-73` | M | O1 (e), MD1, MD5 | 04 4.7, 09 9.9 | Covered |

### R3 sections 7.5, 9.3 and 10: conditions for the two new results

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R3 7.5 | Not expressible: where a callback runs, where its exception goes, effects "from now on", blocking | `aot/builtins/generator/vocabulary.ts:305-321` | M | `on`, `thrownTo`, `synchronizes`, `blocks` | 03 3.5.1, 3.5.2 | Covered |
| R3 7.5-3 | An escaped or lost `Thread` constructor must count as a start | `aot/valueflow/AOTValueFlowRulesHost.cpp:877-904` | M | `StartsThread`, always followed | 04 4.2.1 | Covered |
| R3 9.3 | Both results need a check at run time | `aot/valueflow/AOTFlowApplyCasts.cpp:525-531` | tooling | `ValidateThread`, hidden field, edge check | 04 4.14 | Covered |
| R3 10.1-1 | Incomplete callee sets in thread code | `aot/valueflow/AOTValueFlowResultsCalls.cpp:75` | M | first row of the edge table | 04 4.2.2 | Covered |
| R3 10.1-2 | Host entries: on which thread does native code call an escaped closure | `aot/valueflow/AOTValueFlowResultsFunctions.cpp:884` | M | closures in `escaped` are AnyThread; `CallByHost` row | 04 4.2.2 | Covered |
| R3 10.1-3 | `Later`, `PerEvent` callbacks and coroutine resumption | `aot/valueflow/AOTValueFlowBuiltins.cpp:2160-2188` | M | J1 to J3; `fires: "Caller"`, N-j | 04 4.2.2, 09 9.7, 11 11.2 | Covered |
| R3 10.1-4 | Module bodies evaluated off the main thread | `aot/valueflow/AOTFlowOrder.h:20-26` | M | module row | 04 4.2.2 | Covered |
| R3 10.1-5 | Typed body, generic version, entrances | `aot/valueflow/AOTValueFlowRules.cpp:429-474` | M | follow the edges | 04 4.2.2 | Covered |
| R3 10.1-6 | Functions that passes derive after flow | `aot/valueflow/AOTFlowResults.h:318-320` | M | inherit; deferred code takes the join of its readers | 04 4.2.2 | Covered |
| R3 10.1-7 | One body of a library function for all callers | `aot/valueflow/AOTValueFlowRulesCalls.cpp:636` | precision | AnyThread | 04 4.2.2 | Covered |
| R3 10.2-R1 | Hand-over: what a `NewThread` record passes | `aot/valueflow/AOTValueFlowBuiltins.cpp:2160-2188` | M | R1, R1' | 04 4.2.1, 4.2.3 | Covered |
| R3 10.2-R2 | Captures and environments of shared closures | `aot/valueflow/AOTValueFlowBodies.cpp:892-897,1333` | M | R2 | 04 4.2.3 | Covered |
| R3 10.2-R3 | Everything written under a shared object, hidden slots included | `aot/valueflow/AOTValueFlowRules.cpp:105-148` | M | R3 | 04 4.2.3 | Covered |
| R3 10.2-R4 | Locations without an owner activation | `aot/valueflow/AOTValueFlowResultsLocations.cpp:487-501` | M | R3, R4 | 04 4.2.3 | Covered |
| R3 10.2-R5 | Placeholders: native code carries an escaped object to another thread | `aot/valueflow/AOTValueFlowBuiltins.cpp:1655-1657` | M | R5, R5' | 04 4.2.3 | Covered |
| R3 10.2-R6 | An object allocated off the main thread that enters `Shared` by no rule is local |  | M | R6; Confined "while the allocating thread lives" | 04 4.2.3, 4.14 | Covered |
| R3 10.2-order | (a) depends only on the call graph, so: solve, (a), (b) |  | M | one least fixpoint | 04 4.2 | Covered |

## A.4 Ordinary objects, the interpreter, native code (reports R7, R8)

### R7 section 11.1: arrays

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 A1 | Kind in the cell, contents in the butterfly; Int32 to Double and Double to Contiguous rewrite in place | `runtime/JSObject.cpp:1311-1336` | M | SW1, SW4 | 06 6.2, 6.3 | Covered |
| R7 A2 | Stretch proofs `ArrayStorage`, `InBounds`, `Pristine` end only at this function's writes; guarded accesses have no test | `aot/passes/AOTTypeTestEndings.cpp:31-47` | M | S1, SW9, `Storage` values | 04 4.5, 4.6.3 | Covered |
| R7 A3 | B3 keeps butterfly, lengths, byte 4 in registers or reloads one of them: length of one butterfly with another | `runtime/JSObject.cpp:4195-4216` | M | SW9, C4 | 04 4.9, 06 6.2 | Covered |
| R7 A4 | `Dense` reads have no hole test; writers expose holes below the length for a moment | `aot/codegen/AOTElementAccessEmitter.cpp:264-275` | M | SW4, `TryGetElement` | 06 6.3, 04 4.6.3 | Covered |
| R7 A5 | Dense Int32: 32-bit store of the low half into a slot that meanwhile holds a pointer | `aot/codegen/AOTElementAccessEmitterStores.cpp:195-206` | M | SW1, 64-bit stores | 06 6.3 | Covered |
| R7 A6 | `realloc` of a large butterfly frees it under a reader | `runtime/ButterflyInlines.h:237-246` | M | SW8 | 06 6.2 | Covered |
| R7 A7 | Moving the start of the elements writes a header over former elements | `runtime/JSObjectElementsStartOffset.cpp:87-93` | M | SW6 | 06 6.2, 6.3.10, 6.3.11 | Covered |
| R7 A8 | New butterfly published without a fence (ARM64) | `aot/stubs/AOTStubHelpersStorage.cpp:241` | M | SW7, F2 | 06 6.2, 04 4.10 | Covered |
| R7 A9 | `setStructure` is four stores after the butterfly store: new butterfly with old kind | `runtime/JSCellInlines.h:270-282` | M | SW1, SW11, SW12 | 06 6.2, 6.3.6; 08 8.10 | Covered |
| R7 A10 | A nuked StructureID is visible to another mutator, which decodes it | `runtime/JSObjectInlines.h:110-133` | M | N1 (decontaminated ID) | 06 6.4 | Covered |
| R7 A11 | Read-modify-write of `publicLength`: push/push, push/pop, `a[len] = v` | `aot/codegen/AOTArrayBuiltinEmitter.cpp:170-171` | V | SW3', SW10 | 06 6.2, 6.3.3, 6.3.4; 02 2.10.5 | Covered |
| R7 A12 | Copy to a new butterfly is not atomic with element stores: lost stores, two converters | `aot/stubs/AOTStubHelpersStorage.cpp:202-242` | V | SW5, SW12, SW13; closures validate | 06 6.3.5 to 6.3.9; 08 8.6 | Covered |
| R7 A13 | `vectorLength` grows in place; a reader without a fence sees unfilled slots (ARM64) | `runtime/JSObject.cpp:4187-4190` | V | SW2 (constant, holes before publication) | 06 6.2 | Covered |
| R7 A14 | Freeze in place against a writer that has passed the test | `runtime/JSObject.cpp:2998-3023` | V | SW5, `ReadOnly` | 06 6.3 | Covered |
| R7 A15 | Site statics filled lazily: shared values, template objects (identity) | `aot/codegen/AOTAllocationEmitterObjects.cpp:39-60` | V | Z1 | 07 7.6 | Covered |
| R7 A16 | `ArrayAllocationProfile`, last array, `ArrayWords`, `placeThatConverts` | `aot/runtime/AOTArrayAllocationProfile.h:59-64` | H | K1, private statics | 06 6.3, 07 7.3.2 | Covered |
| R7 A17 | `haveABadTime`; `tryMarkPristine` rewrites `m_blob`, flags, bit field of live Structures | `runtime/Structure.cpp:791-796` | M / V | safepoint operation; `m_blob` one store; `tryMarkPristine` under `m_lock` | 06 6.3.11, 6.5 | Covered |
| R7 A18 | Frame arrays and stack string builders lie in a native frame | `aot/runtime/AOTFrameArrays.h:18-38` | rule | C2, `args` retained | 04 4.8, 03 3.5.2 | Covered |

### R7 section 11.2: ordinary objects and Structures

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 O1 | New StructureID with the old, smaller butterfly | `runtime/JSObjectInlines.h:561-717` | M | N1, SW7, F3 | 06 6.4 | Covered |
| R7 O2 | Two adders interleave nuke, butterfly, StructureID | `runtime/JSObjectInlines.h:561-717` | M | object lock, SW12, SW13 | 06 6.3.7, 6.3.9, 6.4 | Covered |
| R7 O3 | Two adders take one free offset | the same | V | object lock | 06 6.4 | Covered |
| R7 O4 | Property table stolen by two transitions out of one Structure | `runtime/Structure.cpp:1257-1270` | M | a table in the transition tree is never edited again | 06 6.5 | Covered |
| R7 O5 | Lock-free lookups while `rehash` swaps and frees the index vector | `runtime/PropertyTable.h:548-582` | M | PT1 to PT8 | 06 6.5 | Covered |
| R7 O6 | Dictionary add: table entry first, butterfly second | `runtime/StructureInlines.h:437-443` | M | "storage first", PT8 | 06 6.4, 6.5 | Covered |
| R7 O7 | `flattenDictionaryStructure` renumbers, moves, may drop the butterfly | `runtime/Structure.cpp:1822-1881` | M | safepoint operation | 06 6.4, 08 8.6 | Covered |
| R7 O8 | Transition table read without the lock while `add` rehashes | `runtime/StructureInlines.h:573-577` | M | `m_lock` for mutator readers | 06 6.5 | Covered |
| R7 O9 | Look up, miss, create, insert: duplicate Structures | `runtime/Structure.cpp:853-860` | V/H | look again under the lock | 06 6.5 | Covered |
| R7 O10 | `m_bitField`, `m_outOfLineTypeFlags`, `m_blob`: plain `\|=` | `runtime/Structure.h:895-932` | V | atomic read-modify-write | 06 6.5 | Covered |
| R7 O11 | `allocateRareData` and the rare data's caches: check, then act | `runtime/Structure.cpp:1892-1899` | V | compare-and-swap | 06 6.5 | Covered |
| R7 O12 | `vm.propertyRuns`, `PropertyAdditionCache` | `runtime/PropertyRun.h:59-144` | M | table lock | 06 6.5 | Covered |
| R7 O13 | A read can write: static properties, lazy members, function properties | `runtime/Lookup.cpp:54-108` | M/V | object lock, D1, Z1, Z2 | 06 6.4, 6.10, 05 5.13 | Covered |
| R7 O14 | `allowLazyMaterializationOfImmutablePropertiesCount` as a scope for all threads | `runtime/VM.h:465` | V | per thread | 06 6.4, 07 7.2 | Covered |
| R7 O15 | `WatchedLocationState` byte: `willWrite`, then write | `aot/runtime/AOTBuiltinLocationWatch.h:36-38` | V | word F3 of 07 | 07 7.5 | Covered |
| R7 O16 | Stub `growOutOfLineStorage` stores only the StructureID | `aot/stubs/AOTStubHelpersStorage.cpp:134-138` | M | object lock in the stub | 06 6.4 | Covered |
| R7 O17 | `missesOfPairs`, `missIsDueForFiling` | `aot/stubs/AOTStubsPropertyRead.cpp:512-527` | H | private | 07 7.3.3 | Covered |

### R7 section 11.3: Map, Set, WeakMap

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 C1 | The obsolete table is reused as a forwarding record before the owner's pointer is switched | `aot/runtime/AOTCompactTable.cpp:392-412` | M | MS5, MS7 | 06 6.6 | Covered |
| R7 C2 | `removeAllEntries` wipes in place | `aot/runtime/AOTCompactTable.cpp:687-698` | V | MS6 | 06 6.6 | Covered |
| R7 C3 | Append order: a reader finds the entry before its value: empty value | `aot/stubs/AOTStubsCollections.cpp:210-247` | M / V | MS2 | 06 6.6 | Covered |
| R7 C4 | Two appenders | the same | V | MS1 | 06 6.6 | Covered |
| R7 C5 | Widening: widths read apart from the columns | `aot/runtime/AOTCompactTable.cpp:554-633` | M via C1 | MS5, MS7 | 06 6.6 | Covered |
| R7 C6 | `wasWalked`: load, or, store on a shared byte | `aot/stubs/AOTStubsCollections.cpp:886-890` | V | MS9 | 06 6.6 | Covered |
| R7 C7 | First insertion: null, create, store | `runtime/JSOrderedHashTable.h:109-114` | V | MS1 | 06 6.6 | Covered |
| R7 C8 | In-image table moves to the heap | `aot/runtime/AOTCompactTableCreation.cpp:65-78` | V | MS1, MS5 | 06 6.6 | Covered |
| R7 C9 | One per-VM word for "the iteration entry" | `aot/runtime/AOTCompactStorage.cpp:91-99` | M? | per thread | 06 6.6, 07 7.2 | Covered |
| R7 C10 | Iterator object shared: two fields torn | `runtime/JSMapIterator.h` | V | MS9 | 06 6.6 | Covered |
| R7 C11 | WeakMap: buffer and capacity are two loads; `rehash` frees at once | `runtime/WeakMapImplInlines.h:130-172` | M | MS11, MS12 | 06 6.6 | Covered |
| R7 C12 | Hashing a key string writes `m_hashAndFlags` | `wtf:text/StringImpl.h:1184-1200` | see S5 | atomic | 06 6.7 | Covered |

### R7 section 11.4: strings

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 S1 | Rope resolution reads `m_fiber` again after the test | `runtime/JSString.cpp:338-372` | M | one load per fiber, compare-and-swap, no safepoint before the end of the copy | 06 6.7 | Covered |
| R7 S2 | Two winners: one impl leaks | `runtime/JSString.h:747-750` | leak | compare-and-swap | 06 6.7 | Covered |
| R7 S3 | `swapToAtomString`: list of parked impls unlocked; cleared when "no JS is executing" | `heap/Heap.cpp:1406-1434` | M | per-thread list, all stacks, H16 | 06 6.7; 08 8.12 | Covered |
| R7 S4 | Rope view: characters from the stub, width from a second load of the impl | `aot/codegen/AOTStringEmitter.cpp:190-199` | M | "opening a string": one load | 06 6.7 | Covered |
| R7 S5 | `m_hashAndFlags` plain: `setHash` against `setIsAtom` | `wtf:text/StringImpl.h:172` | M | atomic | 06 6.7 | Covered |
| R7 S6 | One unlocked atom table per VM | `wtf:text/AtomStringImpl.cpp:49-79` | M | 64 shards | 06 6.7 | Covered |
| R7 S7 | `remove` uses the current thread's table | `wtf:text/AtomStringImpl.cpp:561-569` | crash | life cycle of an atom, point 4 | 06 6.7 | Covered |
| R7 S8 | find, find in image, add | `wtf:text/AtomStringImpl.cpp:109-126` | V to M | shard lock | 06 6.7 | Covered |
| R7 S9 | `deref()` relaxed, with a non-atomic path for count 1 | `wtf:text/StringImpl.h:1214-1229` | M | shortcut only if `!isAtom()` | 06 6.7 | Covered |
| R7 S10 | `markAsAtom`: `\|=` on the flags byte | `runtime/JSString.h:940-947` | benign | atomic `or` | 06 6.7 | Covered |
| R7 S11 | Bump pointer of string headers; literal string cache; constant cells | `aot/runtime/AOTInstance.h:209` | M | private arena, K1, Z1 | 06 6.7, 07 7.6 | Covered |
| R7 S12 | String builder pool | `aot/codegen/AOTStringBuilderEmitter.cpp:241-253` | M | private | 06 6.7 | Covered |
| R7 S13 | `numericStrings` read by stubs: key, then string | `aot/stubs/AOTStubsStringConversions.cpp:88-98` | V | K1 | 06 6.7, 07 7.4 | Covered |
| R7 S14 | `JSONWords`: ownership by order of stack addresses | `aot/runtime/AOTJSONScanner.h:329` | M | private | 06 6.7 | Covered |
| R7 S15 | `equalResolvingRopes` mutates both operands | `runtime/JSString.cpp:329-376` | M via S1 | as S1 | 06 6.7 | Covered |

### R7 section 7.10: typed arrays

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 7.10-1 | Detach or transfer frees the bytes under a reader or writer | `runtime/ArrayBuffer.cpp:302-306` | M | TA1, `LoadView`, TA8 | 06 6.8, 04 4.6.4 | Covered |
| R7 7.10-2 | Old length with a null or refreshed vector | `runtime/JSArrayBufferView.cpp:271-334` | M | TA2, TA3 | 06 6.8 | Covered |
| R7 7.10-3 | `InBounds` and B3 invariance assume only this thread writes `TypedArrayStorage` | `aot/codegen/AOTB3Policy.cpp` | M | `window` values | 04 4.6.4, 4.9 | Covered |
| R7 7.10-4 | A view made while its buffer is being detached is never notified | `runtime/ArrayBuffer.cpp:445-450` | M | TA7 | 06 6.8 | Covered |
| R7 7.10-5 | `ArrayBuffer` reference count is plain | `heap/GCIncomingRefCounted.h:43` | M | TA7 | 06 6.8 | Covered |
| R7 7.10-6 | Incoming reference list, `Heap::m_arrayBuffers`: unlocked vectors | `runtime/ArrayBuffer.h:249` | M | TA7, heap lock | 06 6.8, 08 8.11 | Covered |
| R7 7.10-7 | `slowDownAndWasteMemory` twice at once: double adoption | `runtime/JSArrayBufferView.cpp:299-330` | M | TA5, TA6 | 06 6.8 | Covered |
| R7 7.10-8 | The old fast vector lives only by conservative scanning | the same | M | TA5 | 06 6.8 | Covered |
| R7 7.10-9 | `pin()` against `transferTo`: plain count, test, then detach | `runtime/ArrayBuffer.h:338` | M | TA7, TA9, H11 | 06 6.8; 08 8.9 | Covered |
| R7 7.10-10 | `sort()` without comparator on a buffer that another thread writes | `runtime/JSGenericTypedArrayViewInlines.h:941-963` | M | TA10 | 06 6.8 | Covered |
| R7 7.10-11 | `Atomics.wait` from two threads: one waiter record per VM | `runtime/WaiterListManager.cpp:120` | M | per thread | 06 6.8 | Covered |
| R7 7.10-12 | A fresh view is published without a fence (ARM64) | `runtime/JSArrayBufferView.cpp` | M | F1 | 04 4.10 | Covered |
| R7 7.10-13 | `m_wrapper` (`Weak`) assigned by two threads | `runtime/SimpleTypedArrayController.cpp:44-57` | M | TA7 | 06 6.8 | Covered |
| R7 7.10-14 | Two `resize`, or `resize` against `transfer` | `runtime/ArrayBuffer.cpp:563-564` | M | TA7 | 06 6.8 | Covered |
| R7 7.10-15 | Lazy fill of `typedArrayAllocations[type]` | `aot/runtime/AOTInstance.h` | M | private, filled per thread | 07 7.3.3 | Covered |
| R7 7.10-16 | Shrinking `resize`: fault on a protected page | `runtime/ArrayBuffer.cpp:563` | C | TA1 (deferred closure) | 06 6.8 | Covered |
| R7 7.10-17 | A thread in `Atomics.wait` keeps heap access | `runtime/WaiterListManager.cpp:120` | liveness | H1 | 08 8.3 | Covered |
| R7 7.10-18 | Termination wakes only `vm.syncWaiter()` | the same | liveness | per-thread trap word | 09 9.10, 9.13 | Covered |
| R7 7.10-19 | Loops bounded by a typed array length have no poll | `aot/codegen/AOTHintEmitter.cpp:102-150` | liveness | S3 | 04 4.11 | Covered |
| R7 7.10-20 | Stores lost between copy and swing in Fast to Wasteful | `runtime/JSArrayBufferView.cpp:299` | V | TA5 | 06 6.8 | Covered |
| R7 7.10-21 | Two wrappers, two `ArrayBuffer`s for one view | the same | V | TA6, TA7 | 06 6.8 | Covered |
| R7 7.10-22 | Atomics do not order ordinary accesses for the compiler | `aot/ir/AOTSchema.def` | V | `synchronizes` | 04 4.4, 03 3.5.2 | Covered |
| R7 7.10-23 | Plain reads merged or hoisted; unaligned accesses tear |  | V | M10, `PinLoopExitReads` | 02 2.10.6, 04 4.12 | Covered |
| R7 7.10-24 | After `transfer` a stale view writes into the new buffer | `runtime/ArrayBuffer.cpp` | V | TA4 | 06 6.8 | Covered |
| R7 7.10-25 | `toString`, TextEncoder paths classify the bytes, then read them again | bun natives | V | TA10, N-f | 06 6.8; 11 11.2 | Covered |
| R7 7.10-26 | Validating builds: `captureBytes` takes a change for the call's effect | `aot/runtime/AOTOperandSnapshot.cpp:225-231` | false alarm | photographs treat every buffer that is not Confined as shared | 04 4.14 | Covered |

### R7 section 8: the interpreter

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 8-1 | All state of "the" thread is in `VM`, found through `CodeBlock::m_vm` or cell to VM | `llint/LowLevelInterpreter.asm:1466-1490` | M | I1, I4 | 10 10.2 | Covered |
| R7 8-2 | `m_evalCallee`: `setScope`, enter, `setScope(nullptr)` | `interpreter/Interpreter.cpp:2150-2198` | M | one callee per thread | 10 10.3 | Covered |
| R7 8-3 | Unwinding writes per-Instance words | `aot/runtime/AOTInstance.h` | M | private part of the view | 09 9.5 | Covered |
| R7 8-4 | Inline allocation on allocators reached from shared places | `llint/LowLevelInterpreterAOT.asm:1151-1154` | M | allocators of `m_thread`, I1 | 10 10.4, 08 8.8 | Covered |
| R7 8-5 | `GetByIdModeMetadata`: several words, no order | `llint/LowLevelInterpreter64.asm` | M | I1 | 10 10.2 | Covered |
| R7 8-6 | `OpPutById`: four words; transition stores ID, then value | the same | M | I1; transition on the slow path | 10 10.2, 10.4 | Covered |
| R7 8-7 | `get_by_id_direct`, private names, brands | the same | M / V | I1 | 10 10.2 | Covered |
| R7 8-8 | `get_from_scope`, `put_to_scope`: info, operand, union | the same | M | I1, K2 | 10 10.4 | Covered |
| R7 8-9 | `resolve_scope`: type, union; epoch check, then use | `llint/LowLevelInterpreter64.asm:2896-2897` | M / V | I1, declaration lock | 10 10.3, 10.4 | Covered |
| R7 8-10 | `CallLinkInfo`, incoming calls, polymorphic stubs | `bytecode/RepatchInlines.h:125-149` | M / C | I1 | 10 10.2 | Covered |
| R7 8-11 | `ObjectLiteralPlan` overwritten in place; plan vector | `aot/runtime/AOTInterpreterSlowPaths.cpp` | M | I1 | 10 10.2 | Covered |
| R7 8-12 | Hashed property table probed from assembly | `llint/LowLevelInterpreterAOT.asm:417-421` | M | K1 | 07 7.4, 10 10.4 | Covered |
| R7 8-13 | Profiles, counters; `updateProfile()` looks into another thread's butterfly | `bytecode/ArrayAllocationProfile.cpp` | P (M read) | I1, A1; stores compiled out | 10 10.3 | Covered |
| R7 8-14 | A watchpoint fired by one thread rewrites another's metadata | `bytecode/CodeBlock.cpp` | M | E2, E4 | 10 10.2 | Covered |
| R7 8-15 | Atom table, reference counts on identifiers | `wtf:text/AtomStringImpl.cpp` | M | shards | 06 6.7 | Covered |
| R7 8-16 | `CodeCacheMap`, source provider caches, TDZ map, `BuiltinExecutables`, cell sets | `runtime/CodeCache.h:242-269` | M | one lock each, Z1 | 10 10.3 | Covered |
| R7 8-17 | `DirectEvalCodeCache::get` without the lock | `bytecode/DirectEvalCodeCache.h` | M / P | per thread | 10 10.3 | Covered |
| R7 8-18 | `prepareForExecution` is check, then act; `installCode` publishes block and entry apart | `runtime/ScriptExecutable.cpp:198-211` | M / C | I1, I2 | 10 10.2 | Covered |
| R7 8-19 | `UnlinkedMetadataTable::link` / `unlink` mutate the shared table | `bytecode/UnlinkedMetadataTable.h` | M | each link copies | 10 10.3 | Covered |
| R7 8-20 | Lazy state per CodeBlock; template object map | `runtime/ScriptExecutable.cpp:467-494` | M / V | I1, Z1 | 10 10.3 | Covered |
| R7 8-21 | Declaration instantiation: long check phase, then act phase | `runtime/ProgramExecutable.cpp:205-307` | V / M | `m_declarationLock`, parking (H15) | 10 10.3; 08 8.15 | Covered |
| R7 8-22 | Per-Instance lazies of the eval path | `aot/runtime/AOTInstance.h` | M | per thread; `ImmortalCells` is locked | 10 10.3, 07 7.6 | Covered |
| R7 8-23 | Aging, jettison, `deleteAllCode`, `shrinkFootprint` assume one stopped mutator, or `!vm.entryScope` | `runtime/VM.cpp:1175-1253` | M | all threads parked; H16 | 10 10.2; 08 8.12 | Covered |
| R7 8-24 | `shouldJettisonDueToOldAge` writes from markers | `bytecode/CodeBlock.cpp` | P | as today | 10 10.2 | Covered |

### R7 section 9: RegExp

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 9-1 | `regExpMatchOutput`, one per VM | `aot/runtime/AOTVMState.h:75` | MS | per thread | 06 6.9, 07 7.2 | Covered |
| R7 9-2 | `RegExp::m_ovector`, one per shared cell | `runtime/RegExp.h:188` | MS | `m_ovector` does not exist | 06 6.9 | Covered |
| R7 9-3 | `m_state` published before `m_regExpBytecode`; readers take no lock | `runtime/RegExp.cpp:574-598` | MS | `m_state` last (release), first (acquire) | 06 6.9 | Covered |
| R7 9-4 | `deleteCode`, `reset()` while another thread matches; guarded by "VM idle" | `runtime/RegExp.cpp` | MS | H16, `freeAfterSafepoint` | 06 6.9; 08 8.12 | Covered |
| R7 9-5 | `m_bytecodeBehindFlatProgram` made lazily at match time | `yarr/YarrInterpreter.h` | MS | object lock, look again | 06 6.9 | Covered |
| R7 9-6 | `RegExpLiterals::m_cells` unlocked | `aot/runtime/AOTRegExpLiterals.h` | MS | table lock | 07 7.6, 7.7 | Covered |
| R7 9-7 | `RegExpCache` rings: index wraps by equality; `m_parsedPatterns` made twice | `runtime/RegExpCache.cpp:86-92, 135-138` | MS | all members under `m_lock` | 06 6.9 | Covered |
| R7 9-8 | `RegExpCachedResult`: seven stores | `runtime/RegExpCachedResult.cpp:67-73` | MS | per thread | 06 6.9, 02 | Covered |
| R7 9-9 | `AdaptiveStringSearcherTables`, per VM | `wtf:text/AdaptiveStringSearcher.h:397-404` | MS | scratch of the thread | 06 6.7; 07 7.2 | Covered |
| R7 9-10 | `StringReplaceCache`, `StringSplitCache` | `runtime/StringReplaceCacheInlines.h:36-93` | MS | K1 | 07 7.4 | Covered |
| R7 9-11 | `RegExpSubstringGlobalAtomCache`: eight words | `runtime/RegExpGlobalData.h` | JS | per thread with `RegExpGlobalData` | 06 6.9 | Covered |
| R7 9-12 | `regExp` loaded again from the object after the match, against `RegExp.prototype.compile` | `aot/stubs/AOTStubsRegExp.cpp` | MS | the word is loaded once per operation | 06 6.9 | Covered |
| R7 9-13 | `lastIndex`: read, match, write | `runtime/RegExpObject.h` | JS | one location | 06 6.9, 02 2.10.5 | Covered |
| R7 9-14 | Shared literal objects stand for "a new object each time" | `aot/passes/AOTShareRegExpLiterals.cpp` | JS | `ShareRegExpLiterals` | 04 4.8 | Covered |
| R7 9-15 | Statics slot of a literal: check, then create; no fence; `RememberedSlotSet::record` | `aot/runtime/AOTRegExpLiterals.cpp` | MS | Z1, F1, B4 | 07 7.6, 08 8.10 | Covered |
| R7 9-16 | `prepareInlineAllocation`: header, then allocator; one free list | `aot/stubs/AOTStubsRegExp.cpp` | MS | per-thread allocators and records | 08 8.8 | Covered |
| R7 9-17 | Stack limit of the wrong thread | `runtime/RegExp.cpp:442-448` | crash | per-thread `StackManager` | 09 9.4 | Covered |
| R7 9-18 | `ensureGroupsStructure`: read, compare, build, set | `runtime/RegExp.cpp` | JS | 06 6.5 for the transitions | 06 6.5 | Covered |
| R7 9-19 | `VM::m_executingRegExp` | `runtime/VM.h:963` | diagnostic | by class | 07 7.2 | Covered |
| R7 9-20 | `m_lastUseEpoch`, run-time counters | `aot/runtime/AOTRuntimeCounters.cpp:58-67` | lost updates | private | 07 7.8 | Covered |
| R7 9-21 | `m_regExpAllocatorLock` held for a whole interpreted match | `runtime/VM.h:1009-1010` | liveness | the interpreter's scratch is per thread | 06 6.9 | Covered |
| R7 9-22 | Bad time between the test and the inline allocation | `aot/stubs/AOTStubsRegExp.cpp` | V | safepoint operation, window | 06 6.3 | Covered |
| R7 9-23 | Facts proven sequentially: `NeverRead`, `LastIndexUse`, literal sharing | `aot/passes/AOTShareRegExpLiterals.cpp` | JS | O1, `ShareRegExpLiterals` | 04 4.7, 4.8 | Covered |

### R7 section 10.1: `class VM`

#### 10.1.1 State of "the" running thread

07 section 7.2 moves these "member by member" by reference to this table. A row is Covered by that reference where the report's own suggestion is "one per thread".

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.1.1-1 | `disallowVMEntryCount`: a scope of one thread forbids entry to another | `runtime/VM.h:419` | V | per thread | 07 7.2 | Covered |
| R7 10.1.1-2 | `m_exception`, `m_lastException` | `runtime/VM.h:421-423` | M | per thread, word in the view | 07 7.2, 09 9.5 | Covered |
| R7 10.1.1-3 | `m_terminationException` made lazily, compared by pointer | `runtime/VM.cpp:877-884` | V | lazy singletons of `VM` | 07 7.6, 09 9.10 | Covered |
| R7 10.1.1-4 | `topCallFrame`, `topEntryFrame`, a pair in assembly | `runtime/VM.h:431-432` | M | per thread; `doVMEntry` takes the context | 07 7.2, 09 9.3 | Covered |
| R7 10.1.1-5 | `maybeReturnPC`, `topJSPIContext` | `runtime/VM.h:433-434` | M | by class | 07 7.2 | Covered |
| R7 10.1.1-6 | `m_entryScopeServicesRawBits`: a plain and an atomic byte in one word; requests meant for every thread | `runtime/VM.h:439-456` | V | `m_environmentEpoch` | 06 6.10; 07 7.5 | Covered |
| R7 10.1.1-7 | `didEnterVM`: save and restore | `runtime/VM.h:459` | V | by class | 07 7.2 | Covered |
| R7 10.1.1-8 | `allowLazyMaterializationOfImmutablePropertiesCount` | `runtime/VM.h:465` | M | per thread | 06 6.4, 07 7.2 | Covered |
| R7 10.1.1-9 | `m_apiLock`: the single-owner protocol | `runtime/JSLock.cpp:118-160` | C | depth per thread | 09 9.3 | Covered |
| R7 10.1.1-10 | `VMTraps`, `StackManager` | `runtime/VMTraps.h:334-356` | M | per thread | 07 7.2, 08 8.4 | Covered |
| R7 10.1.1-11 | Stack pointer at entry, limits, `m_lastStackTop` | `runtime/VM.cpp:1504-1548, 1856` | C | per thread | 07 7.2, 09 9.4, 08 8.13 | Covered |
| R7 10.1.1-12 | `encodedHostCallReturnValue`, catch and throw targets, `varargsLength` | `llint/LowLevelInterpreter64.asm:403-404` | M | per thread; I4 | 07 7.2; 10 10.2 | Covered |
| R7 10.1.1-13 | `m_executingRegExp`, `m_regExpAllocator`, `m_regExpCache` | `runtime/VM.h:963, 1008-1010` |  | see R7 9 | 06 6.9 | Covered |
| R7 10.1.1-14 | `entryScope` read as "no JavaScript is running" | `runtime/VM.cpp:1175-1183, 1230` | M | H16 | 08 8.12; 07 7.2 | Covered |
| R7 10.1.1-15 | `stringRecursionCheck*`: a `HashSet` and a first object | `runtime/StringRecursionChecker.h:57-100` | M | by class ("in the middle of") | 07 7.2 | Covered |
| R7 10.1.1-16 | `m_checkpointSideState` | `runtime/VM.cpp:1594-1636` | JIT only | not reached |  | Covered |
| R7 10.1.1-17 | `m_scratchBuffers`: every caller gets the same last buffer | `runtime/VM.cpp:1932-1953` | M | per thread | 07 7.2 | Covered |
| R7 10.1.1-18 | `m_synchronousModuleQueue` points at a stack frame | `runtime/JSModuleLoader.cpp:1458-1461` | M | by class; MD3 | 07 7.2, 09 9.9 | Covered |
| R7 10.1.1-19 | `m_drainMicrotaskDelayScopeCount` | `runtime/VM.cpp:2533-2548` | V | with the queue | 09 9.7 | Covered |
| R7 10.1.1-20 | `m_didPopListeners`, `whenIdle` | `runtime/VM.cpp:1181, 2178-2183` | M | H16: safepoint operation | 08 8.12; 06 6.10 | Covered |
| R7 10.1.1-21 | `VM::m_aboutToBeNotifiedRejectedPromises` | `runtime/VM.cpp:1789-1807` | M | with the queue | 09 9.7 | Covered |
| R7 10.1.1-22 | `m_currentWeakRefVersion` | `runtime/VM.h:1214-1219` | V | per thread | 08 8.11 | Covered |
| R7 10.1.1-23 | `m_mightBeExecutingTaintedCode` | `runtime/VM.h:565` | V | by class | 07 7.2 | Covered |
| R7 10.1.1-24 | `m_deletePropertyMode` | `runtime/VM.h:786-803, 1453` | M | by class | 07 7.2 | Covered |
| R7 10.1.1-25 | `currentlyDestructingCallbackObject` | `runtime/VM.h:672-673` | C API | by class | 07 7.2 | Covered |
| R7 10.1.1-26 | `m_failNextNewCodeBlock`, `m_entryCountFromOutside`, `m_unlinkedCodeKeepers`, `m_isDeletingAllCodeToGenerateItAgain`, `m_activeHeapAnalyzer` | `runtime/VM.h:1449-1465` | V | atomics | 07 7.2 | Covered |
| R7 10.1.1-27 | `m_hasTerminationRequest`, `m_executionForbidden` and siblings | `runtime/VM.h:1542-1546` | V | termination is fanned out | 09 9.10 | Covered |
| R7 10.1.1-28 | Exception scope verification state | `runtime/VM.h:1436-1443` | debug | per thread | 07 7.2, 09 9.5 | Covered |
| R7 10.1.1-29 | Embedder hooks (`WTF::Function`) assigned while another thread calls them | `runtime/VM.h:1499-1505` | M | set before the second thread starts | 07 7.2; 11 11.4 | Covered |

#### 10.1.3 Lazily created singletons

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.1.3-1 | `LazyUniqueRef`, `LazyRef` members: both run the lambda, tag bits collide | `wtf:LazyUniqueRef.h:70-124` | C / M | caches per thread; others under `Heap::m_lock` or eager | 07 7.4, 7.6 | Covered |
| R7 10.1.3-2 | `m_megamorphicCache` | `runtime/VM.cpp:457-460` | none | stays null without the JIT |  | Covered |
| R7 10.1.3-3 | `m_emptyPropertyNameEnumerator`, 15 promise executables | `runtime/VM.cpp:1985-2112` | P / M (ARM64) | lazy singletons of `VM` | 07 7.6 | Covered |
| R7 10.1.3-4 | Bound function executables | `runtime/VM.cpp:1072-1093` | P | the same | 07 7.6 | Covered |
| R7 10.1.3-5 | Remote function executables (`Weak<>`) | `runtime/VM.cpp:1095-1117` | M | `ShadowRealm` is `main` | 11 11.6 | Covered |
| R7 10.1.3-6 | `numericStrings.m_doubleCache` | `runtime/NumericStrings.h:75-76` | M | K1 | 07 7.4 | Covered |
| R7 10.1.3-7 | `m_terminationDeadlines` | `runtime/VM.cpp:1320-1321` | M | watchdog per thread | 09 9.10 | Covered |
| R7 10.1.3-8 | `m_persistentBytecodePayloads`, cached two and three character atoms | `runtime/VM.cpp:1168-1173, 2558-2570` | M | table lock | 07 7.7; 10 10.3 | Covered |
| R7 10.1.3-9 | Profilers, fuzzers, statistics | `runtime/VM.cpp:1694-1760` | tools | safepoint operation, or off | 08 8.6, 09 9.10 | Covered |
| R7 10.1.3-10 | Dynamic `IsoSubspace`s: locked, not re-checked | `heap/Heap.cpp:3665-3675` | M | look again; client heap per thread | 08 8.2, 8.8 | Covered |

#### 10.1.4 Content caches

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.1.4-1 | `numericStrings`, small ints, read by stubs | `runtime/NumericStrings.h:60-65` | M | K1; view words | 07 7.4, 06 6.7 | Covered |
| R7 10.1.4-2 | `numericStrings`, int cache: key, value, string | `runtime/NumberPrototype.cpp:491-500` | V + M | K1 | 07 7.4 | Covered |
| R7 10.1.4-3 | `numericStrings`, unsigned and double: a reference into the table is returned | `runtime/NumericStrings.h:73-109` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-4 | `keyAtomStringCache` | `runtime/KeyAtomStringCacheInlines.h:37-60` | P / M (ARM64) | K1 | 07 7.4 | Covered |
| R7 10.1.4-5 | `lastCachedString` | `runtime/JSString.cpp:460` | P | K1 | 07 7.4; 06 6.7 | Covered |
| R7 10.1.4-6 | `lastAtomizedIdentifier*`: two counted references | `runtime/JSString.h:958-966` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-7 | `stringSplitIndice`: shared scratch vector | `runtime/StringPrototype.cpp:1085` | M | scratch of the thread | 07 7.2; 06 6.7 | Covered |
| R7 10.1.4-8 | `m_stringSplitCache` | `runtime/StringSplitCacheInlines.h:34-90` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-9 | `stringReplaceCache` | `runtime/StringReplaceCacheInlines.h:36-93` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-10 | `immutableButterflyToStringCache` (`HashMap`) | `runtime/JSArray.cpp:1182-1194` | M | K1 | 07 7.4; 06 6.7 | Covered |
| R7 10.1.4-11 | `m_hasOwnPropertyCache` | `runtime/HasOwnPropertyCacheInlines.h:35-65` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-12 | `JSONCache`: atoms, transitions, names, strings | `runtime/JSONCacheInlines.h:83-94, 298-320` | M | K1 ("JSON atom cache") | 07 7.4 | Covered |
| R7 10.1.4-13 | `JSONCache::m_isParsing`: save and restore | `runtime/JSONCache.h:110-128` | V/M | with the cache | 07 7.4 | Covered |
| R7 10.1.4-14 | `dateCache` | `runtime/JSDateMath.h:85-225` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-15 | BigInt divisor and inverse: five words and a vector | `runtime/JSBigInt.cpp:4847-4879` | M | K1 | 07 7.4 | Covered |
| R7 10.1.4-16 | `m_stringSearcherTables` | `wtf:text/AdaptiveStringSearcher.h:97-111` | V, M not excluded | scratch of the thread | 07 7.2; 06 6.7 | Covered |
| R7 10.1.4-17 | `m_syncResumeCallCache`: entries rebuilt in place | `interpreter/MicrotaskCall.h:83-150` | M | scratch of the thread | 07 7.2 | Covered |
| R7 10.1.4-18 | `m_random`, `m_heapRandom`, `m_integrityRandom` | `runtime/VM.h:473-475` | V | K1 | 07 7.4 | Covered |

#### 10.1.5 Tables, 10.1.6 mode scopes, 10.1.7 the epilogue

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.1.5-1 | `m_atomStringTable` | `runtime/VM.h:675` | M | shards; life cycle of an atom | 06 6.7 | Covered |
| R7 10.1.5-2 | Symbol registries | `wtf:text/SymbolRegistry.cpp:45-73` | M + I | table lock | 06 6.10 | Covered |
| R7 10.1.5-3 | `symbolImplToSymbolMap`, `atomStringToJSStringMap`, `wasmGCStructureMap` | `runtime/VM.h:767-771` | M + I | table lock | 06 6.10, 08 8.11 | Covered |
| R7 10.1.5-4 | `m_executablesOfNamelessHostFunctions`: find, create, set | `runtime/VM.cpp:1062-1068` | M | table lock | 07 7.7 | Covered |
| R7 10.1.5-5 | `propertyRuns` | `runtime/PropertyRun.h:59-144` | M | table lock | 06 6.5 | Covered |
| R7 10.1.5-6 | `m_impurePropertyWatchpointSets` | `runtime/VM.cpp:1680-1692` | M | table lock | 07 7.7 | Covered |
| R7 10.1.5-7 | Source provider caches, code cache, built-in executables, variable map | `runtime/VM.h:822, 1455, 1469` | M | one lock each | 10 10.3 | Covered |
| R7 10.1.5-8 | `m_microtaskQueues`, default queue | `runtime/VM.h:1447, 1555` | M | one queue per thread | 09 9.7 | Covered |
| R7 10.1.5-9 | `m_debuggers` | `runtime/VM.cpp:2403-2411` | M | no inspector in the first version | 03 3.8, 09 9.10 | Covered |
| R7 10.1.5-10 | `deferredWorkTimer` | `runtime/VM.h:670` | M | one per thread | 08 8.11 | Covered |
| R7 10.1.5-11 | `m_intlCache` | `runtime/VM.h:1468` | M | per thread | 06 6.10; 07 7.4 | Covered |
| R7 10.1.6-1 | `allowLazyMaterialization..` lets other threads write locked built-ins | `runtime/JSObjectInlines.h:386, 574` | M | per thread | 06 6.4 | Covered |
| R7 10.1.6-2 | `m_deletePropertyMode`: `IgnoreConfigurable` for all threads | `aot/object/AOTObjectConversion.cpp:79-82` | V, M | by class | 07 7.2 | Covered |
| R7 10.1.7-1 | Caches with unrooted cells are wiped in the epilogue; every copy must be reached | `heap/Heap.cpp:2318-2354` | M | `didResume()`, conductor | 08 8.12 | Covered |
| R7 10.1.7-2 | `executeEntryScopeServicesOnEntry`: any entering thread resets date and Intl caches and walks the heap | `runtime/VM.cpp:2127-2164` | M | `m_environmentEpoch`; one safepoint operation | 06 6.10; 07 7.5 | Covered |

### R7 section 10.2: `LazyProperty`, `LazyClassStructure`

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.2-1 | A second thread gets `nullptr` while the first initializes | `runtime/LazyPropertyInlines.h:95-108` | C | Z2, W1 | 07 7.6; 09 9.14 | Covered |
| R7 10.2-2 | Both threads run the initializer | the same | I | Z2 (claim) | 07 7.6 | Covered |
| R7 10.2-3 | `\|= initializingTag` straddles `set` | the same | C / M | Z2 (compare-and-swap) | 07 7.6 | Covered |
| R7 10.2-4 | Published word without a fence (ARM64) | `runtime/LazyPropertyInlines.h:57-62` | M | Z2 (release) | 07 7.6 | Covered |
| R7 10.2-5 | `LazyClassStructure` publishes the structure word before the constructor exists and before the class is locked | `runtime/LazyClassStructure.cpp:55-87` | C / M | structure word published last | 07 7.6 | Covered |

### R7 section 10.3: `JSGlobalObject`

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.3-1 | 83 + 57 lazies and the link-time constants | `runtime/JSGlobalObject.h:232-770` | C/I/M | Z2, W1 | 07 7.6 | Covered |
| R7 10.3-2 | Mirror `realm.linkTimeConstants[]` | `aot/runtime/AOTInstance.h:209` | P | private | 07 7.3.3 | Covered |
| R7 10.3-3 | `m_microtaskQueue` (`Ref` move-assign), `m_debugger`, runnability flags | `runtime/JSGlobalObject.cpp:4076-4083` | M | accessor, queue per thread | 09 9.7 | Covered |
| R7 10.3-4 | Array allocation structures overwritten one by one | `runtime/JSGlobalObject.cpp:2972-3013` | V/M | safepoint operation | 06 6.3 | Covered |
| R7 10.3-5 | Embedder and debugger setters: `String` members, `DebuggerEvalEnabler` | `runtime/JSGlobalObject.cpp:2615-2623` | M / V | set before the second thread starts | 07 7.2 | Covered |
| R7 10.3-6 | Cached `localeCompare` collator and number format: three words each | `runtime/JSGlobalObject.cpp:3464-3506` | M | per thread | 06 6.10; 07 7.4 | Covered |
| R7 10.3-7 | `m_structureCache`: the mutator's `get` takes no lock | `runtime/StructureCache.cpp:45-56` | M | the lock, for lookups too | 06 6.5 | Covered |
| R7 10.3-8 | `m_symbolTableCache`, clones for resumable code, module executables | `bytecode/CodeBlock.cpp:1343-1363` | M | table lock ("other `WeakGCMap`s") | 06 6.10 | Covered |
| R7 10.3-9 | `m_asyncContextData`: thread state in a cell of the realm | `runtime/AsyncContextSwapScope.h:90-152` | V | one tuple per thread | 09 9.7 | Covered |
| R7 10.3-10 | `m_ffiContext` | `runtime/JSGlobalObject.cpp:4064-4073` | M | `bun:ffi` is `main` | 11 11.9 | Covered |
| R7 10.3-11 | Watchpoint sets of the realm | `runtime/JSGlobalObject.h:542-601` | V/M | WP1 to WP4 | 06 6.5 | Covered |
| R7 10.3-12 | Species watchpoints installed lazily: `RELEASE_ASSERT`, may flatten the prototype | `runtime/JSGlobalObject.cpp:3553-3647` | C (+M) | WP4 | 06 6.5 | Covered |
| R7 10.3-13 | `m_installed*Watchpoints` vectors, appended from operations of the run time | `runtime/JSGlobalObject.cpp:3845-3945` | M/C | WP4 | 06 6.5 | Covered |
| R7 10.3-14 | `m_rareData`, `opaqueJSClassData` | `runtime/JSGlobalObjectInlines.h:481-511` | M | table lock | 07 7.7 | Covered |
| R7 10.3-15 | `m_weakRandom`, inlined in compiled code | `aot/builtins/AOTMathBuiltinEmitter.cpp:234-249` | V | per thread | 07 7.4, 11 11.6 | Covered |
| R7 10.3-16 | `m_globalLexicalBindingEpoch` | `runtime/JSGlobalObject.cpp:3989-4004` | V | atomic, under the declaration lock | 10 10.4 | Covered |
| R7 10.3-17 | `m_executableForCachedFunctionExecutableForFunctionConstructor` (`Weak<>`) | `runtime/JSGlobalObject.cpp:4281-4291` | M | table lock, or Z1 | 07 7.7 | Covered |
| R7 10.3-18 | Custom getter and setter function sets | `runtime/PropertyDescriptor.cpp:65-80` | M + I | table lock | 06 6.10 | Covered |
| R7 10.3-19 | `m_signposts` | `runtime/JSGlobalObject.cpp:665-677` | M | table lock | 07 7.7 | Covered |
| R7 10.3-20 | Global variables: the spine of the segmented vector is reallocated under readers | `runtime/JSSegmentedVariableObject.h:60-70` | M | spine as PT1, PT4 | 06 6.10 | Covered |
| R7 10.3-21 | `addSymbolTableEntry` publishes the entry before the variable exists | `runtime/JSGlobalObject.cpp:2598-2613` | V | variables first | 06 6.10; 10 10.3 | Covered |
| R7 10.3-22 | Lazies of `SymbolTable`: rare data, `m_localToEntry`, `m_singleton`, `m_arguments` | `runtime/SymbolTable.h:749-784` | M | D1 | 06 6.10 | Covered |
| R7 10.3-23 | `haveABadTime` is not a stop of the world | `runtime/JSGlobalObject.cpp:3023-3174` | V / M | safepoint operation | 06 6.3, 08 8.6 | Covered |

### R7 sections 10.4 to 10.6

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.4-1 | `WeakGCMap`, `WeakGCSet`: raw pointers in a plain hash map, no lock | `runtime/WeakGCMap.h:42-82` | M | the owning table's lock | 06 6.10, 08 8.11 | Covered |
| R7 10.4-2 | The heap's list of dirty weak tables | `heap/Heap.cpp:3087-3090` | M | heap lock | 08 8.11 | Covered |
| R7 10.5-1 | Which atom table a thread uses | `runtime/JSLock.cpp:126` | M | the VM's | 06 6.7, 09 9.3 | Covered |
| R7 10.5-2 | `addSlowCase` inserts, then `setIsAtom(true)` | `wtf:text/AtomStringImpl.cpp:447-476` | M | shard lock, atomic flags | 06 6.7 | Covered |
| R7 10.5-3 | `remove` on whatever thread drops the last reference; resurrection | `wtf:text/AtomStringImpl.cpp:561-569` | M | life cycle of an atom | 06 6.7 | Covered |
| R7 10.5-4 | `s_nextHashForSymbol` | `wtf:text/SymbolImpl.cpp:96-102` | P | none needed |  | Covered |
| R7 10.5-5 | `Symbol()` writes `symbolImplToSymbolMap`; `create` is get, then create | `runtime/Symbol.cpp:63-69, 166-174` | M, I | Z1 under the map's lock | 07 7.6 | Covered |
| R7 10.5-6 | `Symbol.for`: add, then overwrite the bucket | `wtf:text/SymbolRegistry.cpp:45-59` | C, M | table lock | 06 6.10 | Covered |
| R7 10.5-7 | `Symbol::m_description` | `runtime/Symbol.cpp:131-143` | P | Z1 by class | 07 7.6 | Covered |
| R7 10.6-1 | C++ fast paths test `isStillValid()`, then act | `runtime/RegExpObjectInlines.h`, `runtime/JSArray.cpp` | V, M for shapes | WP1 (list of sites: W-3.10) | 06 6.5 | Covered |
| R7 10.6-2 | List operations and inflation are unlocked | `bytecode/Watchpoint.cpp:118-215` | M | compare-and-swap, the set's lock | 06 6.5 | Covered |
| R7 10.6-3 | `touch` on writes of global variables loses an update | `bytecode/Watchpoint.h:378-395` | none without the JIT | none needed |  | Covered |

### R7 section 10.7: lazy state inside objects

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.7.1-1 | `materializeStackTrace` reads without the lock, then swaps | `runtime/ErrorInstance.cpp:190-204` | C / M | Z2 with a claim byte, W1 | 06 6.10; 07 7.6 | Covered |
| R7 10.7.1-2 | `materializeErrorInfoIfNeeded`: flag first, hook, four puts | `runtime/ErrorInstance.cpp:555-632` | V / M | Z2 with a claim byte, W1 | 06 6.10; 07 7.6 | Covered |
| R7 10.7.1-3 | `Exception`: the same two stages; shares the trace pointer | `runtime/Exception.cpp:110-204` | M | D1 | 06 6.10 | Covered |
| R7 10.7.2-1 | Two allocators of a function's rare data; a tagged word taken for an executable | `runtime/JSFunction.cpp:155-215` | M | compare-and-swap | 06 6.10 | Covered |
| R7 10.7.2-2 | Four flag bit-fields in one byte | `runtime/FunctionRareData.h:177-180` | V | atomic `or` | 06 6.10 | Covered |
| R7 10.7.2-3 | Flag set before the property is put: `f.length === undefined` | `runtime/JSFunction.cpp:588-634` | V | flag after the put | 06 6.10 | Covered |
| R7 10.7.2-4 | Lazy `prototype`: two objects | `runtime/JSFunction.cpp:372-386, 682-695` | I | add under the object lock, look again | 06 6.4, 6.10 | Covered |
| R7 10.7.2-5 | Allocation profiles of `FunctionRareData` against `clear` | `runtime/FunctionRareData.cpp:91-144` | V, M not read | one immutable record; `m_allocator` compiled out | 06 6.10 | Covered |
| R7 10.7.2-6 | `setCompiledAllocation`: three stores, the reader pairs two | `runtime/FunctionRareData.h:77-87` | M | one 64-bit word | 06 6.10 | Covered |
| R7 10.7.2-7 | `m_asString` of executables | `runtime/NativeExecutable.cpp:123-135` | P | Z1 by class | 07 7.6 | Covered |
| R7 10.7.3-1 | `JSBoundFunction` lazy words | `runtime/JSBoundFunction.cpp:341-434` | P | idempotent | 06 6.10 | Covered |
| R7 10.7.4-1 | `DirectArguments::overrideThings`: publish, then zero-fill | `runtime/DirectArguments.cpp:128-148` | C / V | compare-and-swap of complete storage | 06 6.10 | Covered |
| R7 10.7.4-2 | `ScopedArguments::m_overrodeThings` | `runtime/ScopedArguments.cpp:136-147` | C | flag after the puts | 06 6.10 | Covered |
| R7 10.7.4-3 | `ClonedArguments`: test, then load `m_callee` again | `runtime/ClonedArguments.cpp:209-232` | C | D1 | 06 6.10 | Covered |
| R7 10.7.4-4 | `initModifiedArgumentsDescriptor` | `runtime/GenericArgumentsImplInlines.h:278-287` | C | compare-and-swap | 06 6.10 | Covered |
| R7 10.7.5-1 | Static property tables: check, then put; two function objects | `runtime/Lookup.cpp:54-108` | I, M | add under the object lock, D1 | 06 6.4 | Covered |
| R7 10.7.5-2 | `reifyAllStaticProperties`: dictionary, loop, then a bit of `m_bitField` | `runtime/JSObject.cpp:3146-3197` | M | object lock, atomic bits | 06 6.4, 6.5 | Covered |
| R7 10.7.6-1 | `m_previousOrRareData` | `runtime/Structure.cpp:1892-1899` | V to M | compare-and-swap | 06 6.5 | Covered |
| R7 10.7.6-2 | `m_cachedPrototypeChain`: clear, set, load again | `runtime/StructureInlines.h:175-185` | C | single pointer, compare-and-swap | 06 6.5 | Covered |
| R7 10.7.6-3 | Cached enumerator and its vector of installed watchpoints | `runtime/StructureRareDataInlines.h:122-128` | M | WP2 | 06 6.5 | Covered |
| R7 10.7.6-4 | `m_cachedPropertyNames` | `runtime/StructureRareDataInlines.h:153-162` | P | the same | 06 6.5 | Covered |
| R7 10.7.6-5 | Replacement watchpoint sets: counter and flag outside the lock | `runtime/Structure.cpp:1901-1935` | V | atomic read-modify-write | 06 6.5 | Covered |
| R7 10.7.6-6 | `m_specialPropertyCache`: double init, `Bag`, clear against fill | `runtime/StructureRareData.cpp:117-257` | M | WP2 | 06 6.5 | Covered |
| R7 10.7.6-7 | Special property cached for a compiled Structure without watchpoints | `runtime/StructureRareData.cpp:161-168` | ? | E5 | 07 7.5 | Covered |
| R7 10.7.6-8 | `m_immutablePropertyNames` lives in that cache | `runtime/StructureRareData.cpp:127-130` | M | WP3 | 06 6.5 | Covered |
| R7 10.7.6-9 | `m_polyProtoWatchpoint` (`Box<>`) | `runtime/StructureRareData.h:183` | M | compare-and-swap of the raw pointer | 06 6.5 | Covered |
| R7 10.7.6-10 | `m_previous` | `runtime/StructureRareData.h:185` |  | one word | 06 6.5 | Covered |
| R7 10.7.6-11 | `m_bitField` setters are two plain stores | `runtime/Structure.h:895-932` | V to M | atomic | 06 6.5 | Covered |
| R7 10.7.7-1 | `ProxyObject` trap cache: offset, then two IDs | `runtime/ProxyObject.cpp:124-195` | M | one 64-bit word per trap | 06 6.10 | Covered |
| R7 10.7.7-2 | `slowDownAndWasteMemory` | `runtime/JSArrayBufferView.cpp:271-334` | M | TA5, TA6 | 06 6.8 | Covered |
| R7 10.7.7-3 | `ArrayBuffer::m_wrapper` | `runtime/SimpleTypedArrayController.cpp:44-57` | I + M | TA7 | 06 6.8 | Covered |
| R7 10.7.7-4 | `JSCallbackObject` private properties | `API/JSCallbackObject.h:133` | C API | D1 | 06 6.10 | Covered |
| R7 10.7.8-1 | `DateInstance`: number and two cached words; compiled code writes all three in line | `aot/builtins/AOTDateBuiltinEmitter.cpp:16-68` | V | three whole words; the stale breakdown is accepted | 06 6.10 | Covered |
| R7 10.7.8-2 | `DateCache::m_timeZoneCache`: one `UCalendar`, set then get | `runtime/JSDateMath.cpp:135-151, 565-583` | M | K1 | 07 7.4 | Covered |
| R7 10.7.8-3 | `DateCache`: DST cache, year-month-day, broken-down dates, zone ID | `runtime/JSDateMath.cpp:225-367` | V | K1 | 07 7.4 | Covered |
| R7 10.7.8-4 | `DateCache`: cached date string, display names | `runtime/JSDateMath.cpp:406-499` | M | K1 | 07 7.4 | Covered |
| R7 10.7.8-5 | The wipe after a change of time zone must reach every copy | `runtime/VM.cpp:2134-2147` | V | `m_environmentEpoch` | 06 6.10; 07 7.5 | Covered |
| R7 10.7.9-1 | `IntlCache`: pattern generator, locale map, LRU of shared formatter impls | `runtime/IntlCache.cpp:61-112` | M | per thread | 06 6.10; 07 7.4 | Covered |
| R7 10.7.9-2 | Default collator and formats of the realm, used by `localeCompare`, `toLocale*String` | `runtime/StringPrototype.cpp:1798` | M | per thread | 06 6.10; 11 11.6 | Covered |
| R7 10.7.9-3 | ICU calls on shared objects; member scratch; break iterators | `runtime/IntlRelativeTimeFormat.cpp:309-347` | M | object lock, `const` calls or clone | 06 6.10 | Covered |
| R7 10.7.9-4 | Lazily made members (`unique_ptr`, `String`) | `runtime/IntlDateTimeFormat.cpp:1719-1768` | M | D1 | 06 6.10 | Covered |
| R7 10.7.9-5 | `m_boundCompare`, `m_boundFormat` | `runtime/IntlCollatorPrototype.cpp:136` | I | D1 | 06 6.10 | Covered |
| R7 10.7.10-1 | `m_resolutionCache`, namespace objects, `m_loadedModules`, the state machine | `runtime/AbstractModuleRecord.cpp:691-702, 1267-1271` | M, I | MD3, MD7 | 09 9.9 | Covered |
| R7 10.7.10-2 | `JSModuleLoader` maps: writes locked, lookups not | `runtime/JSModuleLoader.cpp:316-321, 911-912` | M, I | MD3, MD7 | 09 9.9 | Covered |
| R7 10.7.11-1 | `MicrotaskQueue` with inline enqueue | `aot/stubs/AOTStubsCoroutineJobs.cpp:37-95` | M | one queue per thread | 09 9.7 | Covered |
| R7 10.7.12-1 | `JSWeakObjectRef::deref` against a VM-wide version | `runtime/JSWeakObjectRef.h:47-56` | V | (thread, version) | 08 8.11 | Covered |
| R7 10.7.12-2 | `DeferredWorkTimer::m_pendingTickets` guarded by the API lock | `runtime/DeferredWorkTimer.cpp:196-263` | M | one timer per thread | 08 8.11 | Covered |

### R7 sections 10.8 and 10.9

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R7 10.8-1 | `CodeCache::m_sourceCode` | `runtime/CodeCache.h:242-269` | M | one lock | 10 10.3 | Covered |
| R7 10.8-2 | `BuiltinExecutables::m_unlinkedExecutables[]` | `builtins/BuiltinExecutables.cpp:303-353` | M | lock, Z1 | 10 10.3 | Covered |
| R7 10.8-3 | `sourceProviderCacheMap` | `runtime/VM.cpp:1284-1298` | M | one lock | 10 10.3 | Covered |
| R7 10.8-4 | `m_compactVariableMap`, parser arenas | `runtime/VM.h:1012` | M | lock; parser state is the caller's | 10 10.3 | Covered |
| R7 10.8-5 | `ScriptExecutable`, `FunctionExecutable`: rare data, code block slots, `m_singleton`, poly proto ID | `runtime/FunctionExecutable.cpp:170-180` | M | I2 | 10 10.2 | Covered |
| R7 10.8-6 | Template objects of interpreted code: iterator used after unlock; two objects | `runtime/ScriptExecutable.cpp:467-494` | M, I | Z1 in the unlinked block | 10 10.3 | Covered |
| R7 10.8-7 | `UnlinkedFunctionExecutable`: union of code blocks and decoder, retyped in place | `bytecode/UnlinkedFunctionExecutable.h:386-429` | M | decode under the object lock; `m_isCached` release and acquire | 10 10.3 | Covered |
| R7 10.8-8 | `SourceProvider`: `m_id`, directive strings | `parser/SourceProvider.cpp:89-95` | V, M | under the code cache's lock | 10 10.3; 07 7.7 | Covered |
| R7 10.8-9 | LLInt metadata | `llint/LowLevelInterpreter64.asm` | M | I1 | 10 10.2 | Covered |
| R7 10.8-10 | Template objects in compiled code | `aot/runtime/AOTConstantOperations.cpp:96-120` | I | Z1 in line | 07 7.6 | Covered |
| R7 10.9-1 | `lastArraySize` | `runtime/JSObject.cpp:78, 3986-3993` | P | none needed |  | Covered |
| R7 10.9-2 | `objectListedWithoutOrder`: a `thread_local` raw cell pointer | `aot/object/AOTObjectInternal.cpp:42` | not read | thread-local already |  | Covered |

### R8: Bun's half

#### H-A. Thread-local lookups that stand for "the VM"

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R8 A1 | `VirtualMachine::get()`, `bun_vm()` with an empty slot: `unwrap_unchecked` | `bun:src/jsc/VirtualMachine.rs:696, 988-998` | MS | one `VirtualMachine` per thread | 11 11.4 | Covered |
| R8 A2 | The slot filled with the owner's pointer: `&mut` on two threads | `bun:src/jsc/VirtualMachine.rs:968-1040` | MS | the same | 11 11.4 | Covered |
| R8 A3 | `RUNTIME_STATE` | `bun:src/runtime/jsc_hooks.rs:113-118` | MS | set in `threadDidStart` | 11 11.4 | Covered |
| R8 A4 | `CACHED_GLOBAL_OBJECT`, `defaultGlobalObject()` | `bun:src/jsc/VirtualMachine.rs:697-734` | MS | set in `threadDidStart` | 11 11.4 | Covered |
| R8 A5 | `uws::Loop::get()`: a loop that nobody runs | `bun:src/uws_sys/Loop.rs:213-215` | Hang, MS | a loop per thread | 11 11.4, 09 9.8 | Covered |
| R8 A6 | WTF atom table of the thread | `wtf:text/AtomStringImpl.cpp:76-79` | JS, MS | the VM's table | 06 6.7 | Covered |
| R8 A7 | `Output::SOURCE`, console lock counts, time logs | `bun:src/bun_core/output.rs:220` | JS | `configure_named_thread` in `threadDidStart` | 11 11.4 | Covered |
| R8 A8 | `StackCheck` bound | `bun:src/jsc/VirtualMachine.rs:3473` | MS | `configure_thread()` | 11 11.4 | Covered |
| R8 A9 | `IS_MAIN_THREAD_VM`, `MOCKED_TIME_NS`, `PRNG` | `bun:src/jsc/VirtualMachine.rs:765-766` | JS | per thread | 11 11.4 | Covered |

#### H-B. Multi-word updates and check-then-act

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R8 B1 | `entered_event_loop_count` | `bun:src/jsc/event_loop.rs:313-333` | JS | per Bun VM | 11 11.4 | Covered |
| R8 B2 | The tick ring of `process.nextTick` | `bun:src/js/internal/process/next_tick_queue.ts:31-74` | JS | `$threadLocal`; the native cell per Bun VM (BV1) | 11 11.4, 11.5 | Covered |
| R8 B3 | `$asyncContext`, field 0 | `bun:src/js/node/async_hooks.ts:97-116` | JS, security | one tuple per thread | 09 9.7 | Covered |
| R8 B4 | `isTrackingEnabled`, `isSetUp` | `bun:src/js/node/async_hooks.ts:103-113` | JS | atomic flag of the VM; per thread | 11 11.5 | Covered |
| R8 B5 | Task queues of the loop | `bun:src/jsc/event_loop.rs:48-119` | MS | per Bun VM | 11 11.4 | Covered |
| R8 B6 | `timer::All` heap | `bun:src/runtime/jsc_hooks.rs:64-111` | MS | per Bun VM | 11 11.4 | Covered |
| R8 B7 | `m_aboutToBeNotifiedRejectedPromises` | `bun:src/jsc/bindings/ZigGlobalObject.cpp:1339` | MS | per Bun VM | 11 11.4, 09 9.7 | Covered |
| R8 B8 | 153 lazy members of `Zig::GlobalObject` | `bun:src/jsc/bindings/ZigGlobalObject.h:443-613` | MS | Z2, W1 | 11 11.4; 07 7.6 | Covered |
| R8 B9 | `subspaceForImplSlow`, `subspaceForClass` | `bun:src/jsc/bindings/BunClientData.cpp:221` | MS | server under `Heap::m_lock`, client per thread | 11 11.4, 08 8.8 | Covered |
| R8 B10 | `StrongRootBlock` list, cursor, bitset | `bun:src/jsc/Strong.rs:184-226` | MS | one list per thread, counted by the affine blocks (H10) | 11 11.4; 08 8.11 | Covered |
| R8 B11 | `RefCount`, `CellRefCounted`: plain counts | `bun:src/ptr/ref_count.rs:139-207` | MS | N-k | 11 11.2, 11.3 | Covered |
| R8 B12 | Payload in the cell with `mutates` | `bun:src/codegen/bindgen-emit.ts:831, 1890-1898` | MS | N-k | 11 11.2, 11.3 | Covered |
| R8 B13 | `JsCell`, `Cell`, `RefCell` in wrapped structs | `bun:src/ptr/js_cell.rs:1-36` | MS | N-k | 11 11.2, 11.3 | Covered |
| R8 B14 | `transpiler.log` swap, resolver caches, `ref_strings`, source maps | `bun:src/jsc/web_worker.rs:329-340` | MS | per Bun VM | 11 11.4 | Covered |
| R8 B15 | `sourceProviderMap`, plugins, custom extensions, `requireMap` | `bun:src/jsc/bindings/ZigGlobalObject.h` | MS | BV1; member by member, `static_assert` | 11 11.4 | Covered |
| R8 B16 | `m_pendingNapiModules` and its count | `bun:src/jsc/bindings/napi.cpp:790-800` | MS | `process.dlopen` is `main` | 11 11.4, 11.10 | Covered |
| R8 B17 | `body_value_pool`, request-context pools | `bun:src/runtime/server/mod.rs:3517-3535` | MS | per Bun VM, thread-local | 11 11.4 | Covered |
| R8 B18 | `process.env`, `chdir`, `umask`, `TZ` and the heap walk | `bun:src/jsc/bindings/JSEnvironmentVariableMap.cpp:51` | JS, MS | `main`, `serial`; `m_environmentEpoch`; safepoint operation | 11 11.4; 06 6.10 | Covered |
| R8 B19 | `vm.entryScope` test in `drainMicrotasks` | `bun:src/jsc/bindings/ZigGlobalObject.cpp:3514-3515` | JS | both per thread | 11 11.4 | Covered |
| R8 B20 | `process.exit()` on another thread; `is_shutting_down` | `bun:src/jsc/Strong.rs:184-226` | MS | X1 | 09 9.11; 11 11.4 | Covered |
| R8 B21 | `REFUSED_CHANGES` and others under a mutex |  | safe |  |  | Covered |

#### H-C, H-D, H-E

| ID | Hazard | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| R8 C1 | A borrowed buffer or string stays as it is for the length of a call | `bun:src/codegen/bindgen-lib-internal.ts:329, 2416` | MS | TA8 to TA10, H11, N-d, N-g | 06 6.8; 08 8.9; 11 11.2 | Covered |
| R8 C2 | The table's effects describe all that changes during a call | `aot/builtins/generator/vocabulary.ts` | engine | `synchronizes`, 4.5 | 04 4.4, 4.5 | Covered |
| R8 C3 | `directEntry` takes operands untested; a proof that is not for ever can be undone before the call | `aot/builtins/generator/vocabulary.ts:407-408` | MS | N-l | 11 11.2 | Covered |
| R8 C4 | `customGetter` takes its receiver untested | `bun:src/codegen/builtins/collect-bindgen.ts:672` | fine | class is permanent | 11 11.3 | Covered |
| R8 C5 | `CallbackSpec.times` has no "concurrently" | `aot/builtins/generator/vocabulary.ts:305-314` | MS | `on: "NewThread"`, `args` retained | 03 3.5.1, 3.5.2 | Covered |
| R8 C6 | `AtMostOne` makes module variables shared words | `aot/valueflow` |  | shared locations | 04 4.2.3 | Covered |
| R8 C7 | `host_reads` of an entry module | `bun:src/aot/load` | none |  |  | Covered |
| R8 D1 | Destructors and finalizers run on the sweeping thread | `bun:src/jsc/bindings/napi.h:461-467` | MS | H6 to H10 | 08 8.11; 11 11.3, 11.7 | Covered |
| R8 D2 | `drop_in_place` of a payload in the cell cannot be posted | `bun:src/codegen/bindgen-emit.ts:1894-1898` | MS | H6, H7, H10 | 08 8.11; 11 11.7 | Covered |
| R8 D3 | `close_and_wait` counts tickets, not threads | `bun:src/jsc/VmHandle.rs:1-33` | MS | keep-alive on the main loop; native join before `~VM` | 09 9.11, 11 11.4 | Covered |
| R8 D4 | Worker with threads: `terminate()` is one trap | `bun:src/jsc/web_worker.rs` | MS | fanned out, then joined | 09 9.10, 9.11 | Covered |
| R8 D5 | The cage's last-reserve handler calls `WebWorker__endForExhaustedHeap(Bun__getVM())` on whichever thread | `bun:src/jsc/bindings/ZigGlobalObject.cpp:347` | MS | the handler uses its `VM&` | 11 11.4; 08 8.8 | Covered |
| R8 D6 | Crash and fault handlers look up "the" VM | `aot/image/AOTFaultHandler.cpp:155, 203` | engine | stateless, per context | 09 9.5 | Covered |
| R8 E1 | `CompileOptions` is fixed before flow | `bun:src/aot/engine.rs:125-166` | design | decided at the load stage | 03 3.3 | Covered |
| R8 E2 | An open program cannot prove "no Thread" | `bun:src/aot/load/runtime_roots.rs:60-83` | speed | does not widen | 03 3.3 | Covered |
| R8 E3 | Trim drops what nothing reaches | `bun:src/trim/features/engine.def` | JS | the row says `called` | 03 3.5.2 | Covered |
| R8 E4 | `check_template`: the flavour is "part of that identity for free" | `bun:src/runtime/cli/compile_command/build.rs:252-313` | none | `flavour` byte, layout hash, facts from the template | 03 3.1, 3.3 | Covered |

#### Problems that the hazard lists do not contain

| ID | Problem | Where | Severity | Rule | Section | Status |
| --- | --- | --- | --- | --- | --- | --- |
| NEW-1 | The quarantine of deleted offsets cannot work for Structures that are not dictionaries | `runtime/Structure.cpp:950-978, 577-593` | M | N2, N2', N3 | 06 6.4 | Covered |
| NEW-2 | Objects adopted by the main thread outlive the Bun `VirtualMachine` they point into | `bun:src/runtime/timer/WTFTimer.rs:45` | M | H10, `Orphan` | 08 8.11; 11 11.3, 11.4 | Covered |
| NEW-3 | `size` and iteration are not linearizable with `has` | `aot/stubs/AOTStubsCollections.cpp:210-247` | V | MS2, MS8; what is promised | 06 6.6; 02 2.10.5 | Covered |
| NEW-4 | Claims per lazy word deadlock between threads | `runtime/LazyPropertyInlines.h:95-108` | Hang | Z2, W1 | 07 7.6; 09 9.14 | Covered |
| NEW-5 | Locks that are in no order: `m_nativeSerialLock`, `m_declarationLock`, the locks of 10.3 |  | Hang | H15 | 08 8.15; 10 10.3; 11 11.1 | Covered |
| NEW-6 | `bun-aot` must see an import that only `bun-aot-mt` knows | `bun:src/resolve_builtins/HardcodedModule.rs` | build | the name is known to all flavours | 03 3.3, 3.4 | Covered |
| NEW-7 | `bun-aot-mt` is also a complete Bun with a JIT | `bun:src/jsc/bindings/ZigGlobalObject.cpp:377` | M | P3 | 03 3.1, 3.3, 3.7; 10 10.7 | Covered |
| NEW-8 | The spin on `Retired` has no bound and no yield |  | liveness | copy before retirement; yield | 06 6.3.2, 6.3.5; 08 8.15 | Covered |
| NEW-9 | A deferred decommit of a shrunk buffer runs after the buffer has grown again | `runtime/ArrayBuffer.cpp:563` | M | TA1, TA3 | 06 6.8 | Covered |
| NEW-10 | Members of the shared `Zig::GlobalObject` that name one Bun VM | `bun:src/jsc/bindings/ZigGlobalObject.h:111, 364-365, 443` | M | BV1 | 11 11.4 | Covered |

## A.5 Index of rules

Where each rule is stated. Numbers in parentheses are sections.

| Family | Subject | Document | Rules (section) |
| --- | --- | --- | --- |
| A | C++ on arrays | 06 | A1 (6.3.12), A2 (6.3.12), A3 (6.3.12), A4 (6.3.12), A5 (6.3.12), A6 (6.3.12) |
| BV | Bun's `VirtualMachine` | 11 | BV1 (11.4) |
| C | principles of the compiler | 04 | C1 (4.1), C2 (4.3), C3 (4.6.1), C4 (4.9), C5 (4.4) |
| CO | compiled objects | 05 | CO1 (5.2), CO2 (5.2), CO3 (5.2), CO4 (5.2), CO5 (5.2), CO6 (5.2), CO7 (5.2), CO8 (5.2), CO9 (5.2), CO10 (5.2), CO11 (5.2), CO12 (5.14), CO13 (5.14), CO14 (5.2), CO15 (5.2), CO16 (5.2) |
| D | default for other cells | 06 | D1 (6.10) |
| E | cache epoch | 07 | E1 (7.5), E2 (7.5), E2a (7.5), E3 (7.5), E4 (7.5), E5 (7.5) |
| F | fences | 04 | F1 (4.10), F1' (4.10), F2 (4.10), F3 (4.10), F4 (4.10), F5 (4.10) |
| F | fences | 07 | F1a (7.5) |
| G | coroutines | 09 | G1 (9.6), G2 (9.6), G3 (9.6), G4 (9.6) |
| H | heap | 08 | H0 (8.1), H1 (8.3), H2 (8.10), H3 (8.4), H4 (8.14), H5 (8.9), H6 (8.11), H7 (8.11), H8 (8.11), H9 (8.11), H10 (8.11), H11 (8.9), H12 (8.3), H13 (8.10), H14 (8.10), H15 (8.15), H16 (8.12) |
| I | interpreter | 10 | I1 (10.2), I2 (10.2), I3 (10.2), I4 (10.2) |
| J | jobs | 09 | J1 (9.7), J2 (9.7), J3 (9.7) |
| K | caches | 07 | K1 (7.4), K2 (7.4), K3 (7.4), K4 (7.4) |
| L | locks on objects / differences from #249 | 05 | L1 (5.3), L2 (5.3), L3 (5.3), L4 (5.3), L5 (5.3), L6 (5.3) |
| M | memory model | 02 | M1 (2.10.3), M2 (2.10.4), M3 (2.10.4), M4 (2.10.4), M5 (2.10.4), M6 (2.10.4), M7 (2.10.4), M8 (2.10.4), M9 (2.10.4), M10 (2.10.4), M11 (2.10.4) |
| MD | modules | 09 | MD1 (9.9), MD2 (9.9), MD3' (9.9), MD3'' (9.9), MD4 (9.9), MD5 (9.9), MD6 (9.9), MD7 (9.9), MD7' (9.9), MD8 (9.9), MD9 (9.9), MD10 (9.9) |
| MS | `Map` and `Set` | 06 | MS1 (6.6), MS2 (6.6), MS3 (6.6), MS4 (6.6), MS5 (6.6), MS6 (6.6), MS7 (6.6), MS8 (6.6), MS9 (6.6), MS10 (6.6), MS11 (6.6), MS12 (6.6) |
| N | named properties | 06 | N1 (6.4), N2 (6.4), N2' (6.4), N3 (6.4) |
| N-a .. | what a native must do to be `any` | 11 | N-a (11.2), N-b (11.2), N-c (11.2), N-d (11.2), N-e (11.2), N-f (11.2), N-g (11.2), N-h (11.2), N-i (11.2), N-j (11.2), N-k (11.2), N-l (11.2) |
| O | order facts | 04 | O1 (4.7), O2 (4.7), O3 (4.7) |
| P | packaging | 03 | P1 (3.1), P2 (3.1), P3 (3.1) |
| PR | promises | 09 | PR1 (9.7), PR2 (9.7), PR3 (9.7), PR4 (9.7), PR5 (9.7), PR6 (9.7), PR7 (9.7), PR8 (9.7), PR9 (9.7), PR10 (9.7) |
| PT | `PropertyTable` | 06 | PT1 (6.5), PT2 (6.5), PT3 (6.5), PT4 (6.5), PT5 (6.5), PT6 (6.5), PT7 (6.5), PT8 (6.5) |
| R | sharing analysis | 04 | R1 (4.2.3), R1' (4.2.3), R2 (4.2.3), R3 (4.2.3), R4 (4.2.3), R5 (4.2.3), R5' (4.2.3), R6 (4.2.3) |
| S | safepoints | 04 | S1 (4.11), S2 (4.11), S3 (4.11), S3' (4.11), S5 (4.11) |
| S | safepoints | 08 | S1' (8.9), S4 (8.4) |
| SW | storage word | 06 | SW1 (6.2), SW2 (6.2), SW3' (6.2), SW4 (6.2), SW5 (6.2), SW6 (6.2), SW7 (6.2), SW8 (6.2), SW9 (6.2), SW10 (6.2), SW11 (6.2), SW12 (6.2), SW13 (6.2) |
| T | life of a thread | 09 | T1 (9.2), T2 (9.2), T5 (9.2) |
| T | life of a thread | 08 | T3 (8.14), T4 (8.14) |
| TA | typed arrays | 06 | TA1 (6.8), TA2 (6.8), TA3 (6.8), TA4 (6.8), TA5 (6.8), TA6 (6.8), TA7 (6.8), TA8 (6.8), TA9 (6.8), TA10 (6.8) |
| V | views of the Instance | 07 | V1 (7.3.1), V2 (7.3.1), V3 (7.3.1), V4 (7.3.1), V5 (7.3.1), V6 (7.3.1), V7 (7.3.1), V8 (7.3.4), V9 (7.3.3) |
| W | waiting | 09 | W1 (9.14) |
| WP | watchpoint sets | 06 | WP1 (6.5), WP2 (6.5), WP3 (6.5), WP4 (6.5) |
| X | exit | 09 | X1 (9.11) |
| Z | things made at first use | 07 | Z1 (7.6), Z2 (7.6) |
