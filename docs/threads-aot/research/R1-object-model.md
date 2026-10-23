# R1. The object model of AOT-compiled programs, and every way a compiled object, closure or environment is mutated at run time

Read at engine head 5c2923e566cb and Bun head 8bb8d41bffe. Nothing was built or run: every statement below is from reading source.

Paths are relative to `Source/JavaScriptCore/` unless they start with `bun:`, which
stands for `<bun>/`.

Words used below, as the source uses them:

- **layout**: the compiler's description of one kind of object (`LayoutData`, read-only, in the image).
- **shape**: the number of a layout record at run time. An object carries it negated in its shape word.
- **slot**: the bytes of one field, in the cell ("inline") or in the Extension ("out of line").
- **marker**: a bit pattern in a slot that stands for `absent`, `undefined` or `null` instead of a value of the slot's kind.
- **mark of the cell**: `TypeInfoPerCellBit`, bit 7 of the flags byte of the JSCell header. Not a GC mark.
- **extra properties**: an ordinary `JSFinalObject` that holds whatever the slots of a compiled object cannot.
- **stub**: shared machine code in the image that compiled code calls with its own convention. No C++ frame, no lock.
- **operation**: a C++ function (`operationAOT...`) called from compiled code or a stub.
- **Instance**: per realm (`JSGlobalObject`), zeroed pages that compiled code addresses through a pinned register. **State**: the C++ side of one Instance. **VMState**: per VM.

---

## 1. Layout of an `AOT::Object`, and how "empty" is encoded

### 1.1 The first 16 bytes

```
offset 0   uint32  StructureID
offset 4   uint8   indexingTypeAndMisc
offset 5   uint8   JSType            (CompiledObjectType)
offset 6   uint8   inline type flags (bit 7 = mark of the cell, bit 6 = IsPristine)
offset 7   uint8   cellState         (0xFF = the cell lies in a machine frame)
offset 8   uint32  order word
offset 12  uint32  shape word  = 0 - shape
offset 16  fields
```

- `aot/object/AOTCellFormat.h:26` `offsetOfShapeWord = 12`, `:51-52` `offsetOfOrderWord = 8`, `offsetOfFirstField = 16`, `:57` `shapeWordOf(shape) = 0 - shape`.
- `aot/object/AOTObject.h:337` `static_assert(sizeof(Object) == offsetOfOrderWord)`: the C++ class is only the JSCell header. There is no butterfly pointer. The two words lie where `JSObject` has it.
- Header byte positions: `aot/object/AOTObjectWords.h:28` (`structureIDOffset()==0`, `indexingTypeAndMiscOffset()==4`, `sizeof(JSCell)==8`), `aot/object/AOTAllocationSites.cpp:117` (`typeInfoFlagsOffset() == 6`), `heap/ConservativeRoots.cpp:300` (`typeInfoTypeOffset() == 5 && cellStateOffset() == 7`).
- `runtime/JSTypeInfo.h:48-49`: `IsPristine = 1 << 6`, `TypeInfoPerCellBit = 1 << 7` ("will only be set on the cell itself and will not be set on the Structure").
- For a compiled object `IsPristine` means the opposite of what the name suggests: it is set where the object's prototype is **not** the one its layout gives. `aot/object/AOTObjectStorage.cpp:33` `flags = parameters.hasOtherPrototype ? StructureFlags | IsPristine : StructureFlags`; the guard `HasLayoutPrototype` passes when the bit is clear (`aot/codegen/AOTGuardEmitter.cpp:181-182`, `:259-260`).
- `aot/object/AOTCellFormat.h:55` `frameCellState = UINT8_MAX`: "At JSCell::cellStateOffset() of a cell that lies in a frame. No barrier takes it for the state of a marked cell."

**Shape word.** Bit 31 (`compiledObjectShapeBit`) is set in every shape word (`AOTCellFormat.h:137`). No other kind of cell has a word at offset 12 that passes a shape test (`canPassShapeTest()` `:38-41`, static assertions `:138-140`, run-time list in `aot/object/AOTCellFormat.cpp`). So compiled code may run a shape test on any cell without a type test first.

**Order word** (`AOTCellFormat.h:60-81`):

| bits | meaning |
| --- | --- |
| 0..14 | number of the key order inside the layout. `0x7fff` (`orderStoredOutOfLine`) = the list is in the Structure's rare data |
| 15..26 | count: how many entries of that order the object has so far (max 4095) |
| 27 | `orderWordHasFieldOutsideSlot`: "A property that the layout has a field for lies among the extra properties" |
| 28 | `orderWordHasExtraProperties` |
| 29 | `orderWordIsFrozen` |
| 30 | `orderWordIsSealed` |
| 31 | `orderWordIsNotExtensible` |

`orderAndCountMask` = bits 0..26. `orderCountAndShapeMask` masks the 64-bit word at offset 8 down to order, count and shape.

I found no code that ever clears bit 27 or bit 28 (grep over `object/ runtime/ stubs/ codegen/`). For bit 28 the source says so: `aot/object/AOTObjectExtraProperties.cpp:674` "An extra-properties object that became empty stays, and bit 28 stays set: compiled code tests the bit at a member call on behalf of the member read before it."

### 1.2 Slot representations

Enumeration: `aot/ir/schema/runtime-records.def:1-4`. Traits: `aot/object/AOTSlotRepresentation.h:182-229`. Marker constants: `:55-79`.
Every slot is aligned to its own width: `aot/place/AOTPlacementSlotCheck.cpp:254-255` (`if (slot.offset % slot.width()) fail(SlotAlignment)`). Widths are 1, 2, 4 or 8 (`aot/object/AOTSlotAccess.cpp:29-31`).

| Representation | Bytes | Holds a reference | absent | undefined | null |
| --- | ---: | --- | --- | --- | --- |
| `Uint8` `Int8` | 1 | no | none | none | none |
| `Boolean` | 1 | no | none | none | none |
| `OptionalBoolean` | 1 | no | `0` (`ValueEmpty`) | `10` | `2`; false/true are the low byte of the boxed value (6, 7) |
| `Uint16` `Int16` | 2 | no | none | none | none |
| `Int32` `Uint32` `Float` | 4 | no | none | none | none |
| `OptionalInt32` | 4 | no | `INT32_MIN+1` | `INT32_MIN+2` | `INT32_MIN+3`; values are `>= INT32_MIN+5` (`minInlineInt32`) |
| `Cell32` | 4 | narrow | `0` | none | none |
| `OptionalCell32` | 4 | narrow | `0` | `10` | `2` |
| `LiteralString32` | 4 | narrow | `0` | `10` | `2`; `14 + 4n` = literal string number n; `>= minCell32` = a string cell |
| `DeferredClosure` | 4 | narrow | `1` (`absentDeferredClosure`) | (`10`) | (`2`); `0` = "not made yet" (`uncreatedDeferredClosure`) |
| `Int64` | 8 | no | none | none | none |
| `OptionalInt64` | 8 | no | `0x4002<<48` | `0x4000<<48` | `0x4001<<48`; values are within ±(2^62-1) |
| `Double` | 8 | no | none | none | none |
| `OptionalDouble` | 8 | no | `0x7ffc<<48` | `0x7ffa<<48` | `0x7ffb<<48` (three quiet NaNs; real NaN is purified before every store) |
| `Cell` | 8 | wide | `0` | none | none |
| `JSValue` | 8 | wide | `0` (`ValueEmpty`) | `10` | `2` |
| `LiteralString` | 8 | wide | `0` | `10` | `2` |

A narrow reference is `(address - cageBase) >> 3`. `heap/MarkedBlockCage.h`: `shift = 3`, `minIndex = 1 << 21`, so no cell lies in the first 16 MB of the cage and every word below `minCell32` is free for markers (`AOTSlotAccess.cpp:49-51`). `DeferredClosure` has traits `withAbsent(1)` only, but `tryWriteSlot` and `readSlot` also accept 2 and 10 there (`AOTSlotAccess.cpp:169-178`, `:279-282`).

So: **"empty" is always a sentinel in the slot itself, and only representations with `hasAbsent` have one.** `canLeaveSlot(representation) == traitsOf(representation).hasAbsent` (`AOTSlotRepresentation.h:233`). The order word does not say which slot is filled. It says which fields are *listed as keys* and, with bit 27, that some field lies outside its slot.

### 1.3 How a fast-path reader knows that a raw `Double` / `Int32` / `Boolean` slot is filled

There are five mechanisms. Which one an access uses is decided at build time.

**(a) The slot has no absent state because flow proved it never needs one.** `aot/place/AOTSlotRepresentations.cpp:140-166`:

```cpp
case LayoutKind::Record: case LayoutKind::Instance: case LayoutKind::Container: case LayoutKind::BuiltinResult:
    return !isAlwaysFilled
        || field.isCreatedByStore
        || !field.flow.stability.containsAll(everyStability)   // IsNeverDeleted, IsNeverRedefined, IsNeverStoredWithOtherType
        || field.members.mayReachUnknownJavaScript;
```

Only when all four are false does a `number` field get plain `Double`/`Int32`. The reader then does one load with no test beyond the shape. The run time enforces the proof by dying: `aot/object/AOTObjectConversion.cpp:42` "A slot has no empty marker, and a layout no word for extra properties, only where flow proves that nothing needs them", `:47` and `:112` `RELEASE_ASSERT(field.canLeaveSlot())`. Until its first store such a slot holds zero as a placeholder (`aot/object/AOTObjectStorage.cpp:123` "A write-once slot that has no absent state ... holds a placeholder until its only store").

**(b) The sentinel is tested.** Guard `IsSlotFilled` (`aot/codegen/AOTGuardEmitter.cpp:131-165`) emits `SlotEmitter::holdsValue` (slot != absent), `holdsDeclaredType` (slot is no marker at all) or `isFilled`. `holdsDeclaredType` (`aot/codegen/AOTSlotEmitter.cpp:475-499`): `OptionalInt32`: `load32 >= minInlineInt32`; `OptionalDouble`: a **32-bit load of the upper half** compared signed with `undefinedDouble >> 32`; others by `isNoMarker`. The value load that follows (`loadWithoutMarkers`, `:122-138`) is a **second, separate load** in the IR. `GetSlotOrUndefined` folds the test into the read of one load (`loadSlotOrUndefined`, `:397-424`).

**(c) The mark of the cell is tested instead of the slot.** `FieldData::isFilledAtBirth`, `holdsRawValueFromBirth` (`aot/object/AOTLayoutData.h:70-78`):
"A number or a boolean among those fields: whoever stores the marker for undefined or null sets the mark of the cell."
Guards `HasBirthFieldsFilled` and (normally) `HasNoFieldOutsideSlot` are one byte load at offset 6 and a bit test (`AOTGuardEmitter.cpp:174-186`, `:254-261`). One test covers all "fields of birth" of the object. While the mark is clear: every `isFilledAtBirth` field is in its slot, every `holdsRawValueFromBirth` slot holds a raw number/boolean and no marker, and bit 27 is clear (`Object::tryFinishBirth`, `hasEveryFieldOfBirth`, `AOTObjectConversion.cpp:88-106`).

**(d) No test at all, the run time crashes on violation.** `FieldData::keepsStateOfBirth`: "Compiled code reads the field without the test of the cell" (`AOTLayoutData.h:77-78`); defined as occupancy known, never deleted, stays in slot (`aot/ir/AOTLayout.cpp:145-148`). Enforced by `willLeaveStateOfBirth` → `crashOnLostStateOfBirth` (`aot/object/AOTObjectInternal.h:72-76`, `AOTObjectStorage.cpp:142-148`).

**(e) Out-of-line fields that may exist without storage.** `fieldMayExistWithoutStorage`: a field listed in the key order whose granule is not held reads as `undefined` (`AOTObjectStorage.cpp:116-118`, `:152-157`). "Absent bytes do not say that the field is absent: only the key order says whether the object lists it" (`AOTSlotEmitter.cpp:441`), and that case calls an operation.

---

## 2. The Extension

### 2.1 What it is

`aot/object/AOTCellFormat.h:83`: "An Extension is a word of bits and, in ascending order, the granules whose bits are set. The offset of a slot counts every granule, held or not."

```
Extension:  uint64 granuleBits | granule | granule | ... | zeroed room up to the capacity
granule = 8 bytes; at most 64 granules; slot offset o  ->  granule number o/8 - 1
address of slot o = ext + 8 + popcount(granuleBits & (bit(o) - 1)) * 8 + o % 8        (AOTObject.h:346-350)
```

- It is **not a JSCell**: raw auxiliary memory from `vm.heap.cagedAuxiliarySpace` (`AOTObjectStorage.cpp:276-286`), marked with `markAuxiliary` by the owner's visit.
- Capacities `{1, 3, 5, 9, 13, 19, 27, 39, 64}` granules, cell sizes in multiples of 16 bytes (`AOTCellFormat.h:96-115`). "Every granule starts as zero, and so does the room for more" (`AOTObject.h:341`).
- Only representations whose absent marker is zero go out of line, else the field is a `JSValue` there (`AOTSlotRepresentations.cpp:121-125`, `AOTSlotEmitter.cpp:363`).

**Hidden word.** One inline slot of the object (`ExtensionLayoutData::extension`, copied to `CompiledStructure::m_extensionSlot`). `Cell` (8 bytes, raw pointer, 0 = none) with wide references, else `Cell32`. The narrow word is `(ext - 8 MB - cageBase) >> 3`, and 0 = none (`AOTObjectStorage.cpp:218`, `aot/object/AOTObjectVisiting.h:71-80`). At `cageBase + 8 MB` lies a readable page of zeros (`AOTCellFormat.h:90-91`, `heap/MarkedBlockCage.cpp:63-68`), so with narrow references "no Extension" decodes to an Extension that holds no granule and compiled code needs no null test (`AOTSlotEmitter.cpp:254-267`: the null test is emitted only for the wide form).

### 2.2 Which fields, which objects, when

- A field may be out of line only if (`AOTSlotRepresentations.cpp:210-230`) the layout can have an Extension, no store is "without allocation", it is not stored at most once, not immutable, not (required and never undefined), and so on. In short: optional fields that many objects never get.
- No Extension for layouts that may lie in a frame or be flattened into another (`:82-85`).
- An object gets one: at allocation if an initial field is out of line (`AllocationEmitter::storeFieldsInExtension`, `aot/codegen/AOTAllocationEmitter.cpp:588-615`); at a `PutSlotOrAdd` that needs storage (`AOTSlotAccessEmitter.cpp:632-633`); in the by-name store stub (`aot/stubs/AOTStubsKnownFieldStores.cpp:274-318`); in `Object::store` → `ensureStorageFor` (`AOTObjectStorage.cpp:158`, `:174-180`). The operation also reserves `granulesFilledByConstructor()` at once (`aot/runtime/AOTFieldOperations.cpp:62-66`).

### 2.3 How it grows

`Object::ensureExtensionGranules` (`AOTObjectStorage.cpp:182-207`), mirrored in machine code by `appendGranule` (`aot/stubs/AOTStubHelpersValues.cpp:152-170`), `generateAllocateExtension` and `growExtensionAndReturn` (`aot/stubs/AOTStubsAllocation.cpp:310-361`, `:392-520`):

1. **Append in place** (`Extension::tryAppend`, `:288-298`). New granules all lie above every held one and fit the capacity: one plain 64-bit store `m_granuleBits = bitsAfter`. In the stub: `load64; or64; store64`, not atomic. "It is zero since the Extension was created, so the collector may see the bits at any time" (`AOTObject.h:355`).
2. **Insert in place** (`Extension::tryInsert`, `:300-318`). New granule belongs between held ones and there is room: the granules behind it are **moved up one by one in the same memory**, the gap is zeroed, and last `m_granuleBits` is stored. Allowed "Only while no thread of the collector visits cells" (`AOTObject.h:357`): the condition is `!vm.heap.mutatorShouldBeFenced()` (`:185`), in the stub a byte test of `instance.mutatorShouldBeFenced` (`AOTStubsAllocation.cpp:433`). The justification is single-mutator: `AOTObjectStorage.cpp:182` "Marking begins and ends while the mutator is stopped, so it does neither before the caller has returned."
3. **Replace** (`:199-206`). Allocate a new Extension with `held | missing`, copy each held granule, then `setExtension`: `storeStoreFence`, store the hidden word (one 32- or 64-bit store), `vm.writeBarrier(this)`. `:188` "a granule that belongs between others goes into a copy, and the old Extension stays as it is". The old one stays readable until a collection frees it; nothing forwards stores made to it.

### 2.4 What compiled code does

Read (`SlotEmitter::loadOutOfLine`, `AOTSlotEmitter.cpp:361-376`): load hidden word (32/64 bit) → compute `ext` → `load64 granuleBits` → test the bit (`holdsGranules`, `:322-328`) → `load64 granuleBits` again in the IR → popcount or subtraction chain → indexed load (`addressInExtension`, `:330-344`). Missing granule = the absent constant. B3 may merge the two loads of the bits, and may hoist: `:369` "B3 may load the extension once in front of a loop and let the cell die".

Write: `PutSlot` computes the same address and stores, with **no test** where placement proved it: `aot/codegen/AOTSlotAccessEmitter.cpp:599` "Placement puts a slot out of line only where every PutSlot finds it filled, so the Extension is there and holds its granule, unless the object lists the field without storage." `PutSlotOrAdd` first runs `ensureExtensionHolds` (test, else stub `allocateExtension(size)` or operation, `AOTSlotEmitter.cpp:269-288`).

Stubs: `computeFieldAddress(..., isOutOfLine = true)` (`aot/stubs/AOTStubsFieldReads.cpp:92-128`): one `load64` of the bits, shift so that the granule's bit is the sign, popcount.

---

## 3. The side object for extra properties

- **What**: an ordinary `JSFinalObject` made from `instance.realm.emptyExtraPropertiesStructure` (inline capacity 2: `aot/runtime/AOTInstance.h:195-196`). It has a butterfly, Structure transitions, may become a dictionary, and holds indexed properties too (`Object::getOwnPropertySlotByIndex`, `aot/object/AOTObjectMethodTable.cpp:74-82`). It is nobody's prototype (`AOTObjectInternal.cpp:128`).
- **Where the reference lives** (`cellInPlaceOfExtraProperties`, `AOTObjectExtraProperties.cpp:32-43`): a second hidden inline slot (`LayoutData::slotOfExtraProperties()`, `Cell32` or `Cell`), meaningful only while bit 28 of the order word is set. A layout has that slot only if some allocation class may add, redefine or store another type (`AOTSlotRepresentations.cpp:286-293`). Where the layout has none, "an object that gets some gets a Structure of its own, which holds them" (`aot/object/AOTCompiledStructure.h:107-112`, `RareData::heldExtraProperties`).
- **The same word may hold something else**: an `InternalFieldTuple` of "pending stars" (recorded `export *` re-exports). `Object::extraPropertiesSlow()` (`:624-630`) replaces it by a real object at first demand, from a `const` method. `makePendingStars` (`:288-310`): "Nothing of the object changes but the cell in the hidden word". `sourceOfPendingBinding` decrements a counter in the tuple on each read (`:606-610`).
- **Creation** `Object::setExtraProperties` (`:644-654`), in this order:
  1. `writeExtraPropertiesWord` (32- or 64-bit store)
  2. `vm.mutatorFence()`
  3. `orderWord |= orderWordHasExtraProperties` (plain 32-bit read-modify-write)
  4. `vm.mutatorFence()`
  5. `JSCell::setStructure(vm, withExtraProperties)` (see 4.3: three separate stores)
  6. `vm.writeBarrier(this, extra)`

  The stub does the same without a call: `createExtraProperties` (`aot/stubs/AOTStubsPropertyExtraProperties.cpp:170-185`): allocate and initialize the side object with the value already in inline slot 0, `fenceAfterAllocation`, store hidden word, `or32` into the order word, `store32` the new StructureID.
- **Order of lookups** (`Object::getOwnPropertySlot`, `AOTObjectMethodTable.cpp:35-72`): (1) is the name a field of the layout (`fieldNamed`); if so read the slot, and a non-empty value answers; (2) pending stars for a `Get`; (3) the extra properties. So **the slot wins** over an extra property of the same name. `findFieldProperty` (`AOTObjectInternal.cpp:68-83`) is the same rule for writers.
- **Caching by name**: a `CompiledStructure` variant can *name* the Structure of the side object (`extraPropertiesStructureID()`, `AOTCompiledStructure.h:94-105`). Then the outer StructureID alone implies at which offset of the side object a name lies, and stubs read `object → hidden word → (butterfly) → word` after one StructureID comparison (`readExtraPropertyAndReturn`, `AOTStubsPropertyExtraProperties.cpp:80-85`; probes `:252-271`). The "Unnamed" variant says nothing and is not cached.

---

## 4. Do the shape word and the StructureID ever change?

### 4.1 Shape word: never

- `aot/analysis/AOTGuardFacts.cpp:48-49`: "A compiled object keeps its shape, and with it its instance, for life." `proofsThatOutlastWrites { Proof::Shape, Proof::OwnInstance, Proof::SourceOpen }`.
- `aot/codegen/AOTSlotAccessEmitter.cpp:1043`: "The shape word of an object never changes, so the two words together change when the order word does."
- Every store to offset 12 that I found initializes a new cell: `AOTObjectStorage.cpp:51`, `AOTObjectAssign.cpp:144,153`, `AOTObjectBuilder.cpp:51`, `AOTAllocationSites.cpp:246`, `AOTAllocationEmitter.cpp:130`, `AOTCellAllocationEmitter.cpp:189-229`, `AOTStubsObjectCopy.cpp:178`, `aot/runtime/AOTInterpreterSlowPaths.cpp:549`.
- In the schema `Shape` is listed with lifetime `Stretch` (`aot/ir/AOTSchema.def:23`) and many operations declare `writes(ShapeWords)`; the analysis overrides that with the set above.
- PR text: "A compiled object stays one."

### 4.2 StructureID: yes, often. What never changes is the layout behind it

Every Structure an object can have is a `StructureTable::Variant` of the same shape (`aot/object/AOTStructureTable.cpp:45-133`): key = (shape, prototype, hasOtherPrototype, mayBePrototype, extra properties None/Unnamed/Named(Structure), heldExtraProperties, out-of-line key order). `variantOf(current)` copies the shape (`:495-511`), so `CompiledStructure::m_layout`, `m_extensionSlot`, `m_extraPropertiesSlot` are the same in all of them.

| Event | New Structure? | Where |
| --- | --- | --- |
| First extra property | yes | `setExtraProperties`, stub `createExtraProperties` |
| Side object changes its own Structure (add, delete, attribute change, dictionary, flatten) | yes if the old one was Named, or a Named one exists | `didChangeExtraProperties` (`AOTObjectExtraProperties.cpp:675-689`), `ensureStructureForExtraProperties` (`:691-707`), stubs `addToExtraProperties`, `addExtraPropertyInNewStorage` |
| `setPrototypeOf`, `__proto__:` in a literal, `new` with another prototype | yes | `setPrototypeKeepingShape`, `setPrototypeAtCreation` (`aot/object/AOTObjectIntegrity.cpp:121-139`); inline replay, see 5.6 |
| Object becomes somebody's prototype | yes (`mayBePrototype`) | `PropertyWalk::didBecomePrototype` (`aot/object/AOTPropertyWalk.cpp:447-454`) |
| Key order gets no number (limit hit) or gets one again | yes | `Object::setKeyOrder` (`aot/object/AOTObjectKeyOrder.cpp:303-318`) |
| freeze / seal / preventExtensions | **no** (order word only), except through the side object's own change | `Object::setIntegrity` (`AOTObjectIntegrity.cpp:63-82`) |
| Field created, deleted, stored, with a numbered key order | **no** | order word, slot, mark of the cell |
| Class field initialisation in a constructor | **no** | same |
| `this` in a derived constructor | never in place: **another object is made** | below |

**`this` allocation.** `new.target` selects the layout (`resolveThisPrototype`, `aot/object/AOTThisAllocation.cpp:85-111`; `createThis` `:135-164`). Where a base constructor has allocated inline and the prototype turns out to be that of a class below, `ClassTable::instanceWithPrototype` (`:281-320`) creates a **new** `Object` of the requested layout and copies the listed fields; the allocated cell is dropped. `:280` "The prototype object of another class never leaves the object with the allocated shape". Compiled code takes the returned cell (`AllocationEmitter::storePrototype`, `AOTAllocationEmitter.cpp:617-654`: "The operation returns the object to use").

### 4.3 In what order `JSCell::setStructure` writes

`runtime/JSCellInlines.h:259-284`:

```cpp
m_structureID = structure->id();                                                  // 32-bit store
m_flags = TypeInfo::mergeInlineTypeFlags(structure->typeInfo().inlineTypeFlags(), m_flags);  // 8-bit read-modify-write, keeps bit 7
m_type = structure->typeInfo().type();                                            // 8-bit store
... CAS loop on m_indexingTypeAndMisc only if it differs
vm.writeBarrier(this, structure);
```

Three plain stores, no fence between them. The flags byte is rewritten from the Structure's flags merged with the *current* mark of the cell.

---

## 5. Each mutation: the exact writes, and what a concurrent fast path could see

"Fast-path reader/writer" below = compiled code that has passed its guards (shape test, perhaps a flag or order-word test) and then loads or stores at a fixed offset. No guard and access pair is atomic, and no fast path takes a lock.

### 5.1 A store that the slot cannot hold

Route: typed code tests the value's type; on failure it continues in generic code, which stores by name: stub `putKnownField` (`AOTStubsKnownFieldStores.cpp:186-391`) → `storeField` (`AOTStubsFieldStores.cpp:438-466`) → exit `cannot` → by-name stubs → operation → `Object::putWithKnownField` (`AOTObjectMethodTable.cpp:208-267`) or `storeInPresentField` (`AOTFieldOperations.cpp:56-60`): `tryStore` fails → `moveFieldToExtraProperties`.

`undefined`/`null` into a slot that has markers is **not** such a store: the marker is written in the slot, whatever the field's type says (`RuntimeLayoutTable::slotAdmits`, `aot/object/AOTRuntimeLayoutTable.h:50-61`), and `willStoreMarker` sets the mark of the cell where `holdsRawValueFromBirth` (`AOTObjectStorage.cpp:142-148`). In `Object::store` the slot is written **first**, the mark **after** (`:163`, `:167-168`). Inline compiled code does it the other way round: `markCellIfMarkerReplacesRawValue` before `writeSlot` (`AOTSlotAccessEmitter.cpp:606`, `:615`).

`Object::moveFieldToExtraProperties` (`AOTObjectConversion.cpp:43-62`):

| # | write | width |
| --- | --- | --- |
| 1 | `ensureExtraProperties`: if none yet, the sequence of section 3 | 4/8, 4 (RMW), 4+1+1 |
| 2 | `didCreateField(state, field, false)` if the field is not in the key order | order word, 4 (RMW) |
| 3 | `orderWord |= orderWordHasFieldOutsideSlot` | 4 (RMW) |
| 4 | `setPerCellBit(true)` | 1 (RMW) (`JSCellInlines.h:362-370`) |
| 5 | `putProperty(extra, name, leaving)`: ordinary `putDirect` on the side object | many words: value, perhaps new butterfly, side object's StructureID |
| 6 | `clear(field)`: the absent marker into the slot | 1/4/8 |
| 7 | `didChangeExtraProperties`: `setStructure` on the object | 4+1+1 |
| 8 | `didChangeOwnProperties`: invalidation of chain caches if the object may be a prototype | tables |

Windows:
- Between 5 and 6 the property exists twice. Slot wins, so a reader sees the old value: harmless.
- A reader that passed the mark test (or `IsSlotFilled`) before 4/6 and loads after 6 gets the **absent marker as if it were a value** (see H1).
- A fast-path writer that passed its guards before 3/6 and stores after 6 **refills the slot**. Now slot and side object both hold the name, bit 27 is set, and the slot wins: the value stored by the slow path is lost, and the stale copy stays in the side object. JavaScript-visible (lost store, which one of two racing stores may be anyway), plus a broken invariant ("While no field lies outside its slot, no extra property has the name of a field", `AOTObjectKeyOrder.cpp:188`).

The way back: `tryMoveFieldBackToSlot` (`AOTObjectConversion.cpp:64-86`): `tryStore` into the slot, then delete from the side object, then `setStructure`. Bit 27 and the mark stay set.

### 5.2 `delete`

`Object::deleteProperty` (`AOTObjectMethodTable.cpp:275-315`) → in slot: `Object::deleteField` (`AOTObjectConversion.cpp:108-121`):

1. `clear(field)`: absent marker into the slot (1/4/8).
2. `setPerCellBit(true)` if `isFilledAtBirth` (1, RMW). **After** the slot.
3. `removeFieldFromKeyOrder` → `setKeyOrder` (5.7): builds the remaining list, looks up or adds a numbered order, then order word (4, RMW), perhaps `setStructure`.
4. chain invalidation.

Between 1 and 3 the object lists a key whose slot is empty. `getOwnPropertyNames` tolerates that (`AOTObjectMethodTable.cpp:108-113` re-checks each listed field). Fast paths that list keys by `(Structure, order word)` alone (`Object::fieldsInKeyOrder`, `AOTObject.h:449-456`; JSON, for-in, `Object.keys` caches) then read an empty slot; `readListedField` (`:458-464`) falls back to `read()`, which may return an empty `JSValue`.

### 5.3 `defineProperty` / redefinition to an accessor

`Object::defineOwnProperty` → `defineFieldProperty` (`aot/object/AOTObjectDefinition.cpp:69-128`). "A property is in its slot only while it has the form that the layout gives the field". If attributes stay the layout's and the value fits: a plain `tryStore`. Otherwise (`:122-127`): `moveFieldToExtraProperties(current value, current attributes, Redefinition)` (the 8 steps above) and then `extra->defineOwnProperty(...)` on the side object, then `didChangeExtraProperties` again. Between the two halves the property is a data property in the side object with its old value.

A field whose layout form *is* an accessor holds a `GetterSetter` cell in its slot; joining the second half makes a **new** `GetterSetter` and stores it (`:103-110`). "Nothing writes the words of a GetterSetter once it is made" (`AOTSlotAccessEmitter.cpp:300`).

A fast-path reader that passed its guards before the move returns the old data value: a legal linearization. A fast-path **writer** that stores after the move refills the slot and thereby hides the accessor (slot wins): wrong JavaScript, memory-safe.

### 5.4 freeze / seal / preventExtensions

`Object::setIntegrity` (`AOTObjectIntegrity.cpp:63-82`): (1) freeze/seal the side object with the ordinary functions (many writes, Structure transition), (2) `didChangeExtraProperties` (`setStructure`), (3) `orderWord |= bits` (one 32-bit RMW), (4) invalidations. Field attributes are *computed* from the order word (`attributesOfPublicFields`, `AOTObject.h:154-160`).

Fast-path writer: `IsNotFrozen` = `load32(order word) & 0xE0000000 == 0` (`AOTGuardEmitter.cpp:282-290`), then the store. Check-then-act: a store can land after the freeze. The proof also lasts over a stretch of code (see H3). Wrong JavaScript only.

The RMW in (3) races with every other writer of the order word (H2).

### 5.5 Adding an undeclared property

C++: `Object::tryStoreExtraProperty` (`AOTObjectExtraProperties.cpp:718-753`): look up offset in the side object's Structure; replace = `putDirectOffset`; add = `willAddExtraProperty` (ends member guards of the class), `putDirect`, then `setExtraProperties` / `ensureStructureForExtraProperties` / `didChangeExtraProperties`.

Stub, no call (`addExtraProperty`, `AOTStubsPropertyExtraProperties.cpp:216-228`, after a cache hit on the object's StructureID):
- test bit 31 of the order word, test that no barrier is needed, test bit 28;
- `addToExtraProperties` (`:110-135`): load hidden word; `store32` side object's new StructureID; `store32` object's new StructureID; **then** `store64` the value at the cached index (inline or through the butterfly).

So the two StructureIDs are published **before** the value. A second thread that probes the read cache with the new outer StructureID loads the still-zero word: an empty `JSValue` reaches compiled code (H6). Growth of the out-of-line storage (`addExtraPropertyInNewStorage`, `:187-214`) replaces the side object's butterfly in the stub.

### 5.6 `setPrototypeOf`

C++: `Object::setPrototype` (`AOTObjectIntegrity.cpp:141-156`): same-value test, extensibility test, cycle walk, then `setPrototypeKeepingShape` (`:121-131`): `didBecomePrototype` on the new prototype (may change *that* object's Structure), `setStructure(withPrototype(...))`, invalidations, perhaps `haveABadTime`.

Inline (`AccessEmitter::findPrototypeTransition` + `storeStructureAfterPrototypeTransition`, `aot/codegen/AOTAccessEmitterDefinitions.cpp:106-130`), used for `__proto__:` in literals and for `new` with another prototype: hash `(StructureID ^ prototype)`, read the 16-byte entry `{prototype, before, after}` of `instance.objects.prototypeTransitions` with three loads, then `store32 StructureID`, then `load8 / or IsPristine / store8` on the flags byte. `AOTStructureTable.cpp:562`: "Compiled code replays a transition by storing the StructureID and setting IsPristine ... in the cell: it looks for no cycle and tells nobody who depends on the chain." The table is written by `withPrototype` with a plain struct assignment (`:565`) and zeroed at each collection (`:296`).

The same replay exists as a **stub in front of a host function** (`aot/stubs/AOTStubsBoxedCalls.cpp:494-509`; from its place and its test of `orderWordIsNotExtensible` I take it to be the fast path of `Object.setPrototypeOf`, I did not read the head of the function): load StructureID, probe the table (`branch64` on `prototype`, `branch32` on `before`), test bit 31 of the order word, `load32 after`, `store32 StructureID`, `load8 / or IsPristine / store8`. So this transition is applied without a call to objects that are already published, not only to new ones. `withPrototype` files an entry only if the object is no prototype itself and holds no extra properties in its Structure (`AOTStructureTable.cpp:564`).

A reader that relies on `HasLayoutPrototype` (flags bit 6 clear) and then uses a method resolved at build time is check-then-act. `AOTStructureTable.cpp:533`: "Compiled code tests the flag after the read that relies on it, so an object that had it keeps it": once set, `hasOtherPrototype` is sticky in every later variant.

### 5.7 Key order changes

The order word names a path in a per-layout tree of orders: orders the compiler listed (in the image) plus orders added at run time (`KeyOrderTable`, per State). An order added at run time **grows in place** while an object under construction stands at its end (`aot/object/AOTKeyOrderTable.cpp:577-583`), and "What entriesInOrder() ... return stays where it is, unchanged, for as long as the table lives" (`AOTKeyOrderTable.h:36`).

Writers of the order word:
- C++ `setOrderAndCount` (`AOTObject.h:153`): `word = (word & ~mask) | orderAndCount`, plain RMW.
- Stub `stepInsideOrderOfImage` (`aot/stubs/AOTStubsFieldCreation.cpp:59-100`): `add32 (1 << 15), [object + 8]`: a memory-operand add, not locked.
- Stub `appendFieldToKeyOrderBehind` (`:142-188`): load64 of both words, probe the hash table `instance.objects.keyOrderTransitions`, then load32 / and / or / store32.
- Stub `takeRememberedKeyOrderRun` (`:214-243`): compare the 64-bit word with a `KeyOrderRun` entry, then `store32` the remembered word **as it is** (flags zero).
- Inline: `storeOrderWord` (`AOTSlotAccessEmitter.cpp:790-793`): `store32` of a **constant or remembered whole word**, after comparing the whole word (`testLikeliestKeyOrderStep` `:956-1012`, `testRememberedKeyOrderStep` `:1029-1056`), or with no comparison at all for an object whose state since its allocation is known statically (`stateOfNewObjectBefore`, `:896-954`).
- `Object::tryDefineGettersOfLiteral` (`AOTObjectDefinition.cpp:248`): whole word from a cached plan.

Out of line: `setKeyOrder` (`AOTObjectKeyOrder.cpp:303-318`). To out-of-line: `setStructure(withOutOfLineKeyOrder)` **then** `setOrderAndCount(outOfLineOrderWord)`. Back to numbered: `setOrderAndCount(numbered)` **then** `setStructure(withNumberedKeyOrder)`. In both directions there is a window with a numbered order word and a Structure that has `HasOutOfLineKeyOrder`; the opposite pairing (order word says out of line, Structure has no list) does not occur in program order. `Object::keyOrder` (`:38-54`) reads the order word, then `compiledStructure().outOfLineKeyOrder()`, which dereferences `rareData()` (null for a Structure that `holdsListedFields()`, `AOTCompiledStructure.h:208-213`).

### 5.8 Field initialisation during construction

Allocation (`AllocationEmitter::allocateWithFields`, `AOTAllocationEmitter.cpp:656-727`):
1. cell from the free list of the allocator named by the allocation site's record (stub `allocateCell`, or inline patchpoint);
2. header + order word + shape word: **one 16-byte copy** from the `AllocationSiteRecord` (`CellAllocationEmitter::copyHeaderAndWords`, `aot/codegen/AOTCellAllocationEmitter.cpp:188-229`: `movups`/`vmovups` on x86-64, `ldp`/`stp` on ARM64). The header in the record already has the mark of the cell if `isBornUnfinished` (`objectHeaderFor`, `AOTAllocationSites.cpp:115-119`);
3. hidden words, initial fields, absent markers in the other slots, zeros in rooms (plain stores of the slot's width, or 16-byte zeroing);
4. `fenceAfterInitialization()` = `mutatorFence()`;
5. Extension for out-of-line initial fields.

`mutatorFence` (`aot/codegen/AOTPrimitiveEmitter.cpp:154-167`): on x86-64 a compiler-only fence; on ARM64 a real fence **only if `instance.mutatorShouldBeFenced`**, that is only while the collector marks concurrently. The same in stubs (`fenceAfterAllocation`, `aot/stubs/AOTStubHelpersAllocation.cpp:67-76`).

Then the constructor's stores: `PutSlot` / `PutSlotOrAdd` / `PutSlots`. In inline code "The key is listed first, while the slot still says whether the field is missing" (`AOTSlotAccessEmitter.cpp:670`): order word, then slot. In the by-name stub the slot is stored first, then the key is listed (`AOTStubsKnownFieldStores.cpp:156-172`, `:361-372`).

End of birth: pass `FinishBirths` (`aot/tests/passes/finish-births.ir:1-2`: "An object is born with the mark that the fields which every constructor fills are not all stored. The pass takes the mark off behind the store by slot that fills the last of them, where no other code has run since somebody else could reach the object. Otherwise the run time looks at the slots"). `FinishBirth` = `load8 / and ~0x80 / store8` (`AOTAllocationEmitter.cpp:200-206`); `TryFinishBirth` = test, then operation → `Object::tryFinishBirth`.

An object allocated by the run time for a constructor lists nothing: `createClassInstance` uses `orderWordOf(0, 0)` ("The run time lists no field when it allocates. The stores of the constructor create the fields and record their keys", `AOTThisAllocation.cpp:113-119`).

---

## 6. Objects in frames, objects inside other objects

Three different things. All are decided at build time.

### 6.1 `AllocateInFrames`: cells in the machine frame

- What: closures, environments, objects and some arrays "that do not escape their activation are allocated in the machine frame" (`aot/tests/passes/allocate-in-frames.ir:1`). `AllocationStorage { Heap, Frame, FrameUntilMaterialized }` (`aot/ir/schema/operations.def:1`).
- How it looks: a locked stack slot (`CellAllocationEmitter::allocateInFrame`, `AOTCellAllocationEmitter.cpp:345-348`). An **object** gets the same header as heap cells of its site (StructureID included), order word, shape word (`FrameCellEmitter::allocate`, `AOTAllocationEmitter.cpp:118-132`) and `cellState = 0xFF` (`initializeHiddenFields`, `:299-304`). An **environment** that is only ever in frames may have no header at all (`:122-123`; `aot/codegen/AOTBarrierEmitter.cpp:90` "Such a cell may have no header, so no code may read its cell state").
- References to it are **wide**: "A narrow reference counts from the base of the cage of cells" (`aot/passes/AOTFrameAllocationPointers.cpp:163`); a field that may hold a frame pointer gets a wide slot (`AOTSlotRepresentations.cpp:77-80`). `Object::isInFrame()` (`AOTObject.h:139`) exists because such an address "leads to no block of the heap and says nothing about the cage".
- GC: "The collector visits no object that lies in a frame" (`AOTStructureTable.cpp:42`). Its fields are found by the conservative scan of the stack, which also takes each 32-bit half for a narrow reference, and recognizes the header word of a frame object to keep its Structure alive (`heap/ConservativeRoots.cpp:300-312`).
- Can a reference escape? Not to the heap, not to native code, not past the activation:
  - "Native code gets no cell of a frame, not even to look at" (`aot/passes/AOTFrameAllocationPointers.h:52`).
  - Callees may *borrow* it: closure-word bits say "nobody holds argument i, or what is read through it, any more when a call through this word returns. Whoever calls may pass a closure that lies in his own frame there" (`aot/object/AOTClosureWord.h:24-26`).
  - A frame cell may hold pointers to other frame cells (`FrameAllocation::mayHoldFramePointer`, `aot/passes/AOTFrameAllocation.h`).
- When moved to the heap:
  - `FrameUntilMaterialized` environments: `MaterializeEnvironment` in front of the first use that lets it out: allocate, `memcpy` of the body, `mutatorFence` (`operationAOTMaterializeEnvironment`, `aot/runtime/AOTAllocationOperations.cpp:53-65`). From there on code uses the heap copy ("The closure of the heap takes the environment of the heap, so the environment is copied in front of it", `aot/passes/AOTFrameAllocationMaterialization.cpp:157`).
  - A lent slim closure whose callee does not borrow: the call stub copies it (`operationAOTMoveClosureToHeap`, `:102-121`: "The closure is slim and lies in a frame. Nothing but the call that is under way has it").
  - Closures "made again": a heap closure is made at each use that lets one out (`aot/passes/AOTFrameAllocationPlanner.h:30`).
  - Objects are not moved: an object that may change its form or escape is simply planned on the heap (`AOTFrameAllocationPlanner.cpp:671`).

### 6.2 `AllocateInnerObjects`: "cells that lie inside other cells"

- `aot/tests/passes/allocate-inner-objects.ir:1`: "Two objects that are born together get one cell: the operation of the inner object, which comes first, allocates it (outerLayout=), and the operation of the outer object takes it."
- Run time: one MarkedBlock cell from a special subspace (`aotOuterCellSpace`, `AOTAllocationSites.cpp:110-113`) holds the outer object at its start and, at a fixed 16-byte-aligned offset behind the outer's fields, a **complete second cell** with header, order word, shape word. `heap/MarkedBlock.h:447-449`: "An inner cell has a header, a mark bit, which is the bit of its atom, and a cell state of its own, so marking and barriers treat it as they treat any cell. It is neither allocated nor swept nor counted by itself: it is alive for as long as the cell that holds it, the outer cell, and whoever visits one of the two appends the other." Inner arrays (cell plus first storage) are possible too (`aot/passes/AOTAllocateInnerObjects.cpp:45`).
- The outer's field still holds an ordinary reference to the inner cell, stored when the inner object is allocated (`allocateBehindOuterObject`, `AOTAllocationEmitter.cpp:435-452`: "From here on the collector may meet the outer object, which has its header and no reference yet. Nobody else does before its own operation has run").
- `LayoutData::holdsInnerObjectsInPlace`: "Compiled code finds an inner object of an object of the layout by the address of the object, without a look at the field" (`AOTLayoutData.h:198`): `innerObjectOf` = `outer + constant` (`AOTSlotAccessEmitter.cpp:182-196`). This is taken only where "Nothing puts a value into the field but the operation that allocates the object" (`AOTAllocateInnerObjects.cpp:717`).
- A reference to an inner object **can escape freely**: it is a normal JS object with a normal (narrow) reference. It keeps the outer cell's memory alive. It is never moved.
- If nothing refers to the inner cell any more while the outer lives, the collector zeroes its header at the end of marking, with mutators stopped (`HeapHooks::noteInnerCellsWithoutMark`, `didFinishMarking`, `aot/runtime/AOTHeapHooks.cpp:185-207`).

### 6.3 `FlattenInnerObjects`: no inner cell at all

`aot/tests/passes/flatten-inner-objects.ir:1-4`:
"`{ c: { d: 1 } }` makes one object. Where nobody ever holds the inner object its fields become private fields of the outer layout and the field goes away. Anywhere else the field stays and its slot defers the inner object ... what only reads and stores slots borrows the outer object (GetFlattenedSlot), and whatever wants the object itself reads the slot with the operation that makes it. **TODO: flatten: the form that keeps the slot is off until every folder of test programs is right with it taken wherever it is legal**".

- Form 1 (field goes away): nothing special at run time, just more fields (`key=#"c.d"@L0`).
- Form 2 (off by default according to that TODO, code is present): the outer object has *room* (header-less slots of the inner layout) and a 4-byte `DeferredClosure` slot with `defersFlattenedObject`. Slot word 0 = "still flattened, slots are in the room"; `>= minCell32` = "was made, slots are in that cell"; 1/2/10 = absent/null/undefined.
  - Read/write through it (`readBorrowingSlot`, `AOTSlotAccessEmitter.cpp:331-378`): `load32` the word, branch, and compute a *base address for the slots*: `outer + distance` or the made cell. All later accesses of that "borrowed" value use that base.
  - Making it (`Object::makeFlattenedObject`, `aot/object/AOTDeferredClosures.cpp:43-66`): allocate the inner cell; copy the room word by word; barrier; `mutatorFence`; `store32` the reference into the slot; barrier; **zero the room**.
  - "The run time makes the object wherever it reads the field, which the operations that call it do not declare as a write" (`AOTSlotAccessEmitter.cpp:345`).
  - "The collector looks into the room whatever the slot of the field says" (`AOTAllocationEmitter.cpp:370`): the room's reference slots are in the outer layout's reference table (`aot/place/AOTLayoutTableEncoding.cpp:259-264`).
  - No reference to a not-yet-made inner object escapes; what escapes is the made cell.

---

## 7. Closures, slim closures, environments

### 7.1 `AOT::Closure` (full)

`aot/object/AOTClosure.h:21-90`. A `JSFunction` subclass: JSCell (8) + butterfly (8) + scope (8) + executableOrRareData (8) + `m_entry` at 32 + captures from 40, 8 bytes each (`AOTCellFormat.h:122-123`). Type `CompiledClosureType`, subspace `aotClosureSpace`.

- `m_entry` is a **closure word** (`aot/object/AOTClosureWord.h`): bits 0..31 code offset from the start of the image's code, 32..53 function number, 54..55 borrowed arguments, 56 ends its tail calls, 57 takes new.target, 58..61 15 minus the number of parameters, 62 always 1, 63 not callable. The same for all closures of one function; read from `instance.closures.closureWords[function]`, a table in `FunctionTables` (per VM).
- Creation order: header, butterfly 0, scope = global object, executable, captures, **then** the word, then `mutatorFence` (`ConstantEmitter::newFullClosure`, `aot/codegen/AOTConstantEmitterClosures.cpp:167-205`; operation `AOTAllocationOperations.cpp:90-99` with `storeStoreFence` around the word). The visitor skips a closure whose word is still 0 (`AOTClosure.h:95-101`).
- **Immutable after creation**: `m_entry` (no other writer found) and the captures. `GetCapture` is declared with effects `none` (`aot/ir/schema/prepared-operations.def:81`); the only stores to capture words are in the creation paths (`storeCaptures`, `AOTConstantEmitterClosures.cpp:226-234`; C++ `create`).
- **Mutable**: StructureID/flags, butterfly, `executableOrRareData` (rare data on demand), as for any `JSFunction`. Lazy properties (`prototype`, `name`, `length`, class members) are materialized on first generic access (`prepareForGenericAccess`, `ensureClassObject`, `ensurePrototypeProperty`).

### 7.2 `AOT::SlimClosure`

`aot/object/AOTSlimClosure.h:31-92`. JSCell (8) + `m_word` at 8 (the closure word, where an object has its butterfly) + captures from 16. The upper half of the word lies at offset 12 and cannot pass a shape test (`AOTClosureWord.h:100-103`). No room for properties.

**It changes its class in place.** First write of a property, `prototype` read, etc.: `SlimClosure::becomeReified` (`aot/object/AOTSlimClosure.cpp:178-193`):

```cpp
Butterfly* butterfly = ...; fill 4 (or 8) values: function index, length, name, prototype placeholder
setStructureIDDirectly(structureID().nuke());
WTF::storeStoreFence();
m_word = std::bit_cast<uint64_t>(butterfly);      // the closure word is overwritten by a pointer
WTF::storeStoreFence();
JSCell::setStructure(vm, structure);              // type becomes SlimClosureWithPropertiesType
vm.writeBarrier(this);
```

Afterwards the function number is read from the butterfly (`ReifiedSlimClosure::function()`, `AOTSlimClosure.h:141`). The captures stay where they are. The collector handles the race with a seqlock-like read (`tryVisitFields`, `:94-110`: StructureID, fence, type, word, fence, StructureID again; `didRace` on failure). A pointer's upper half never looks like the upper half of a closure word (`mayBeUpperHalfOfSlimClosureWord`, `AOTCellFormat.h:43-47`, `:140`), which is what lets a caller test "is this still a slim closure as created" on the word alone. How call sites do that is R6's topic.

### 7.3 How variables are captured

Decided in Bun's front end, `bun:src/aot/lower/captures.rs:17-21`:

```rust
enum Capture { Environment(ScopeId), Value(BindingRef) }
```

`bun:src/aot/lower/captures/needs.rs:114-120`: `can_copy = binding.is_immutable && is_initialized_before(...)`. So:
- **copied by value** into the closure's capture word when the binding is never reassigned **and** is initialized before the closure is created (and the closure is not over the capture limit);
- otherwise the closure captures a **reference to the Environment** of the binding's scope. A closure captures each environment it needs directly (a flat list, `list_captures` `:126-150`); I saw no parent link in `Environment`.
- Module-level bindings are not in environments: `Storage::Variable` (`bun:src/aot/lower/scope/plan/storage.rs:94-106`), which are slots in the Instance's statics.
- Engine passes then change this: `PromoteEnvironmentsToVariables`, `ReadCapturesFromEnvironments`, `AllocateInFrames`, `ReuseEnvironments` (`aot/ir/schema/passes.def`).

Capture words: references first (`captureCountsOf(function).references`), visited as boxed values; then raw words in the declared representation (int32 zero-extended, double bits, ...) (`AOTClosure.h:57-59`, `AOTConstantEmitterClosures.cpp:213-234`).

### 7.4 `AOT::Environment`

`aot/object/AOTEnvironment.h:36-70`. A bare `JSCell` (not an object): header (8) + variables from offset 8. Its shape is packed into the header: low byte at offset 4 (`indexingTypeAndMisc`), high byte at offset 6 (the flags byte), the page in the Structure (`AOTCellFormat.h:117-120`, `AOTEnvironment.cpp:27`, `:66-74`). One Structure per 65536 environment shapes per Instance, immortal (`EnvironmentShapes::structureFor`, `:29-40`). `JSCell::setStructure` asserts it is never called on one (`JSCellInlines.h:261`). Coroutine frames are the same class with `LayoutKind::CoroutineFrame`.

- Variables use the same slot representations as fields; no Extension, no extra properties, no order word.
- **All variable slots are mutable** by `PutEnvironmentField`: one plain store of the slot's width plus the owner barrier (`aot/ir/schema/operations.def:42-43`). The header never changes.
- **TDZ**: the `Absent` primitive. A binding whose initialization flow cannot prove (`InitializationState::Unknown`) gets a representation with an absent marker (`AOTSlotRepresentations.cpp:149-150`); `Environment::create` and inline allocation write it; the read is followed by `CheckInitialized (value, name) -> cast ; throws` (`operations.def:130`). "A location with a dead zone holds `absent` whatever else it holds" (`aot/passes/AOTNarrowFieldTypes.cpp:87`). Where flow proves `StoredBeforeEveryRead` or `SetAtCreation` there is no marker and no check.
- **Reuse** (`ReuseEnvironments`): "A heap environment goes back to its layout's free list where the function has done with it, unless a reference to it escaped" (`aot/tests/passes/reuse-environments.ir:1`). The "free list" is **one 8-byte word per layout in the Instance's statics** (`aot/place/AOTStaticsLayout.cpp:369-381`), cleared at each collection. Take: `load64; test; store64 0` (`takeReleasedCellOrAllocate`, `AOTAllocationEmitter.cpp:238-255`). Give back: `load8` of an "escaped" byte at the end of the environment, then `store64` (`EnvironmentEmitter::generateReleaseEnvironment`, `aot/codegen/AOTEnvironmentEmitter.cpp:74-94`). `MarkEnvironmentEscaped` = `store8 1` (`:65-72`).

### 7.5 Deferred closures (in fields) and other lazy cells

- `DeferClosuresInFields` (`aot/tests/passes/defer-closures-in-fields.ir:1-3`): a field whose value is always a closure of one function over nothing, over the object itself, or over one shared environment (kept in a hidden field) is born with slot word 0.
- First read: inline `load32`, `word > 1` → use it; `0` → `operationAOTMaterializeDeferredClosure` (`loadDeferringSlot`, `AOTSlotAccessEmitter.cpp:483-502`) → `Object::materializeDeferredClosure` (`AOTDeferredClosures.cpp:78-104`): re-read the word, create the closure (for an accessor field a `GetterSetter` around it), `store32`, barrier. **Check, allocate, store**, no CAS.
- `Object::read()` is `const` and materializes (`AOTObjectStorage.cpp:108-112`), so *reads by native code write*.
- Singleton closures: one cell per function in `instance.closureCells` (4-byte `RememberedSlot<uint32_t>` holding `encode(cell) - 1`, or 8-byte pointers). `ClosureTable::closure()` (`aot/object/AOTClosureTable.cpp:338-355`): `closureIfCreated`, else create and `didCreateClosureFor` (`:299-312`, takes `caches->lock()` for the store only, which protects against the collector, not against a second creator). Compiled code reads the cell inline and calls an operation on zero (`singletonClosure`, `AOTConstantEmitterClosures.cpp:82-101`).
- `DeferDefinitions`, `DeferInitializers`: module variables that stay empty until first read (`aot/passes/AOTDeferDefinitions.h:19`, `AOTDeferInitializers.h:16`). Same check-then-create shape; details belong to the modules report.

---

## 8. Classes, and who owns which table

### 8.1 When things are created: lazily, at first touch

Nothing is created at start. "Nothing loops over the image's data at start" (PR text).

- **Class object** (`Closure` of the constructor): for a class proven to be evaluated once and capturing nothing (`hasOneClassObject`, `aot/object/AOTClassTable.h:243-247`) `ClosureTable::closure()` creates it "for whoever asks first" (`AOTClosureTable.h:44`): "by a read of a variable that is born as the class, by the first allocation of an instance, by the definition of a class below" (`AOTClassTable.h:88`). `createClassObject` (`aot/object/AOTClassTable.cpp:151-171`) registers the closure in the closure table **first**, then runs `defineClass`. Otherwise the program's `NewClass` operation defines it at evaluation.
- **Definition may make nothing**: `leavesDefinitionToRecord` (`AOTClassTable.h:102-103`), `tryLeaveDefinitionToRecord` (`aot/object/AOTClassCreation.cpp:502-503` "Where the record says all that the definition does, the definition makes nothing").
- **Prototype object**: first a stand-in `AOT::ClassPrototype` (`aot/object/AOTClassPrototype.h`) with two inline words reused as `m_classIndex`, counters, flags, `m_memberRow`. It answers some lookups from the image's record and **counts lookups in its own fields on reads** (`tryGetDeclaredMember`, `AOTClassPrototype.cpp:90-124`: `++m_numberOfLookups; m_namesLookedUp |= nameBit`). On any write, or after enough reads, `createMembers` (`:63-74`) → `convertToOrdinaryObject` (`:48-61`): zero the private fields, `storeStoreFence`, `setStructure(ordinary JSFinalObject structure)`: **an in-place class change** of a shared, long-lived object.
- **Methods**: closures made on demand (`findDeclaredMethod`, `createMethodClosure`, `m_declaredMemberHolders`).
- **Instance Structures**: `StructureTable::objectStructure(shape)` creates the "usual" one at first use (`AOTStructureTable.cpp:481-488`); `usualVariantOf` may create the class prototype as a side effect (`:431-452`).
- **Allocation site records**: filled at the first slow-path allocation of the site (`fillAllocationSite`, `AOTAllocationSites.cpp:121-160`), which may create the class object.
- A class may be **forgotten** and defined again (`ClassTable::forgetDefinition`, `StructureTable::forgetStructuresOf`, `AOTAllocationSites.cpp:184-202`) "before the run that evaluates it starts again".

### 8.2 Tables filled at run time

Owner column: **I** = inside the `Instance` pages, read (and partly written) by compiled code and stubs directly; **S** = C++ object in `State::objects` (`ObjectModel`, `aot/object/AOTObjectModel.h:24-37`), one per Instance; **V** = `VMState`; **P** = process / image (read-only).

| Table | Owner | Written by | Read by | Notes |
| --- | --- | --- | --- | --- |
| `LayoutData`, `FieldData`, `ClassData`, `AllocationData`, key orders of the image, reference tables, shape sets | P | nobody | all | read-only image |
| `FunctionTables` (closure words, executables) | V | at attach | all | `aot/runtime/AOTVMState.h:73` |
| `ProgramIdentifiers`, `VMPropertyCaches`, `CallSiteCache` | V | run time | stubs, run time | other reports |
| `StructureTable::m_structures` (usual StructureID by shape) | S (arena) | `createUsual`, `forgetStructuresOf`, sweep | run time | plain `uint32_t[]` |
| `AllocationSiteRecord[]` + `distancesToAllocationSites[]` | I (`objects`) | `ensureRecord`, `fillAllocationSite`, `predictStructure`, `forgetStructuresOf`, sweep | **inline code and stubs at every allocation** | 32-byte records `{header, orderAndShapeWords, allocator*, unused}` (`aot/object/AOTAllocationSiteRecord.h:17-27`) |
| `VariantMaps` (`WeakGCMap`, `listed`, `recentByExtraProperties`, bit set) | S | `findOrCreate`, `withExtraProperties` | run time | `listed` "Written under the lock of the caches" (`AOTStructureTable.cpp:182`); the map itself is not |
| `prototypeTransitions[1024]` | I | `withPrototype` | inline code, stub | 16-byte entries; zeroed each GC |
| `keyOrderTransitions` + mask | I | `publishTransition`, `growTransitions` | stubs, `publishedTransition` | open-addressing hash; **old table is released on growth** |
| `keyOrderRuns[2048]` | I | **stub** `rememberKeyOrderRun` | stubs | 3 stores per entry |
| `KeyOrderStepCache[2048]` at a fixed Instance offset | I | **stub** `rememberKeyOrderStepAndReturn` | inline code | 64+32+32-bit stores |
| `keyLists[1024]`, `enumerators[256]` | I | run time | inline/stubs/LLInt | `{orderWord, shapeWord/structureID, cell*}` |
| `KeyOrderTable::Storage` (added orders, continuations, lists) | S | run time | run time | arenas, hash tables |
| `RuntimeLayoutTable::m_cachedFields[4096]`, `m_fieldsByName`, `m_bodiesAtBirth` | S | lookups | run time | 3-word cache entries written on **reads** |
| `ClassTable` maps (`m_classPrototypeStructures`, `m_lazyClassObjectStructures`, `m_classObjectStructures`, `m_classSidesByHolder`, `m_declaredMemberLocations`, sets ...) | S | class creation, guards | run time | `UncheckedKeyHashMap`s, no lock |
| `classes.classPrototypes[]` | I | `registerClassPrototype`, `forgetDefinition` | inline/stubs | one `JSCell*` per class |
| `placesOfPrototypeProperties[8]`, `prototypesOfClassObjects[32]`, `m_instancesOfClassesBelow[16]` | I / S | run time | stubs / run time | 2- and 3-word entries; zeroed each GC |
| `m_recentClassLookups[64]` | S | lookups | run time | the one cache here that is `std::atomic<uint64_t>`, relaxed (`AOTClassTable.cpp:77-89`) |
| member guard words, method table | I | `MemberGuards`, `ClassTable` | inline code | R6 / other report |
| `ClassStructureCache` | S | `add` | `find` | hash map keyed (class, parents) for classes evaluated repeatedly |
| `closureCells[]` | I | `didCreateClosureFor`, `forgetClosure` | inline code | 7.5 |
| Static fields | I (`statics()`) | inline code, `ProgramStatics::writeVariable` | inline code | below |
| `FieldMovePlanCache`, `AssignmentPlanCache`, `PropertyListCache`, `getterDefinitionPlans[64]` | S (`ObjectModelCaches`) | spread/assign/keys/literals | run time | own `unique_ptr`s and `Vector`s; eviction frees plans |
| `CompiledStructure::m_listedFields/m_listedSize/m_listedOrderAndCount`, `numberOfExtraKeys` | heap cell (Structure) | `setListedFields`, `setNumberOfExtraKeys` (`const` methods) | `fieldsInKeyOrder`, stubs | a cache **inside a shared Structure**, three plain stores (`AOTCompiledStructure.h:157-164`) |

**Static fields** (`aot/object/AOTStaticFields.cpp`): a static field of a class with one class object lives in a *variable* in the Instance's statics, with a slot representation and an absent marker. The class object shows it through one shared `CustomGetterSetter` (`ensureProperties`, `:117-133`). A store the variable cannot hold, or a redefinition, moves it to an ordinary property: `moveToProperty` (`:135-147`): `putDirect` on the class object, then the absent marker into the variable, then `didChangeMember` (ends guards).

**The lock.** `CacheRegistry::lock()` (`aot/runtime/AOTHeapCacheRegistry.h:48`) is taken by the mutator around a few table writes and by the collector's `removeDeadEntries`. It is a mutator-versus-collector lock. `AOTKeyOrderTable.cpp:720`: "Only the thread that runs the instance adds transitions, so it reads them as compiled code does, without the lock."

---

## 9. How the rest of JSC sees these objects; how the collector visits them

### 9.1 Method table

`AOT::Object` is a `JSObject` with `StructureFlags` `OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence` (`AOTObject.h:116`). Its Structure has an empty property table. All of `getOwnPropertySlot(ByIndex)`, `getOwnPropertyNames`, `put(ByIndex)`, `deleteProperty(ByIndex)`, `defineOwnProperty`, `preventExtensions`, `isExtensible`, `setPrototype`, `visitChildren`, `estimatedSize`, `analyzeHeap` are overridden (`:217-227`).

- `getOwnPropertySlot`: section 3. Never cacheable for extras (`slot.disableCaching()`).
- `put`: `putWithKnownField` (`AOTObjectMethodTable.cpp:208-267`): fast: field writable in slot, receiver is the object, slot non-empty, `tryStore`. Otherwise the cases of 5.1, 5.5, a chain walk for setters, `tryCreateField`.
- `getOwnPropertyNames` (`:84-145`): indexed names of the side object, then the key order (fields and "positioned" extra keys interleaved by entries `>= numberOfFields`), then the rest of the side object.
- Code that bypasses the method table is hooked: `JSObject::getDirectFromCompiledObject`, `putDirectToCompiledObject` (`runtime/JSObject.cpp:2153-2182`), private fields and brands (`runtime/JSObjectInlines.h:960`, `:996`, `:1146`), through `AOT::PropertyWalk` (`aot/object/AOTPropertyWalk.h`). JSON, structured clone, Proxy, for-in enumerators, `Object.assign`, LLInt slow paths have own branches on `CompiledObjectType`.
- The LLInt handlers for AOT read the order word directly (`llint/LowLevelInterpreterAOT.asm:702-704`, `:837`).
- Reads through these paths **write**: deferred closures, flattened objects, pending stars, `CachedField`, `setListedFields`, `ClassPrototype` counters, `didReadAbsentField`.

### 9.2 The collector

- **The visitor reads neither the shape word nor the order word.** It goes `cell → StructureID → CompiledStructure::m_layout → referencesOffset → table in the image` (`Object::visitFields`, `AOTObject.h:472-477`; `visitReferences` / `forEachReference`, `aot/object/AOTObjectVisiting.h:82-150`; the inline visit in the marking loop, `aot/runtime/AOTHeapCellVisits.h:19-31`).
- The reference table (`appendReferences`, `aot/place/AOTLayoutTableEncoding.cpp:249-299`): three counts, then offsets of wide value slots, wide cell slots, narrow slots. The extra-properties hidden word is in it like a field (only the extension word is skipped, `:255`). Rooms of flattened objects are in it. Then, if the layout has an Extension: the extension slot and three 64-bit granule sets (wide, narrow in first half, narrow in second half).
- Raw slots are simply not in the table. A slot's kind never changes during the life of the object (it is a property of the layout), so **no state of the object decides whether a word is a reference**. A wide slot is decoded as a `JSValue` (markers and literal-string words are non-cells); a narrow one is a reference iff `>= minCell32` (`AOTObjectVisiting.h:88-91`).
- Because all Structures of one object share the layout, a StructureID that changes during concurrent marking changes nothing about which words are visited. The visit marks whichever Structure it loaded; `setStructure` runs the write barrier, so the object is revisited.
- `heldExtraProperties` and the named Structure are visited from the `CompiledStructure` (`AOTCompiledStructure.cpp:151-158`).
- **Extension during concurrent marking**: the bits are "Read once: the mutator may set bits of granules that come after these, and those hold nothing that this visit could miss" (`AOTObjectVisiting.h:105`). In-place insertion is forbidden while marking (2.3). Replacement publishes a complete copy behind a `storeStoreFence` and barriers the owner.
- **Slim closures**: seqlock-like (7.2). **Closures**: skipped while the word is 0. **Environments**: shape from the header bytes, which never change.
- **Inner cells**: 6.2. **Frame cells**: never visited, conservative scan only.
- A type test on a slot's target "reads the cell that a slot points to, which races with a running mutator", so validation runs only with mutators stopped (`AOTObjectStorage.cpp:241-244`).
- Roots in tables: `RememberedSlotSet` cards; "Compiled code stores into the cards of such a set itself" (`heap/RememberedSlot.h:61`). Several caches are simply zeroed at every collection (`addClearedEachCollection`).

---

## 10. Words written by compiled code or stubs without any call

"Inline" = emitted into the function through B3 or the direct generator. "Stub" = shared machine code, still no C++ and no lock. None of these is an atomic instruction; I saw no `lock` prefix, `ldxr/stxr`, `cas` or atomic B3 opcode in the files read for this report.

| What | Instruction width | Kind | Where |
| --- | --- | --- | --- |
| Field slot, `Boolean`/`OptionalBoolean`/`Uint8`/`Int8` | 8-bit store | plain | `SlotEmitter::storeBits` `AOTSlotEmitter.cpp:78-95` |
| `Uint16`/`Int16` | 16-bit store | plain | same |
| `Int32`/`Uint32`/`OptionalInt32`/`Cell32`/`OptionalCell32`/`LiteralString32`/`DeferredClosure` | 32-bit store | plain | same; `:540-547`; `AOTSlotAccessEmitter.cpp:563`, `:571` |
| `Float` | 32-bit FP store | plain | `:179-181` |
| `Double`/`OptionalDouble` | 64-bit FP store | plain | `:183-186` |
| `Int64`/`OptionalInt64`/`Cell`/`JSValue`/`LiteralString` | 64-bit store | plain | `storeBits` |
| Absent marker into a slot | width of the slot | plain | `storeAbsent` `:197-202` |
| Slot in the Extension | as above, at a computed address | plain | `storeInExtension` `:549-553` |
| Extension `granuleBits` | 64-bit load, or, store | RMW | stub `appendGranule`; `growExtensionAndReturn` |
| Extension granules moved in place | 64-bit load/store loop | multi-word | stub `AOTStubsAllocation.cpp:431-466` |
| Hidden word of the Extension | 32- or 64-bit store | plain | stubs `:350-355`, `:507-510` |
| Hidden word of the extra properties | 32- or 64-bit store | plain | stub `AOTStubsPropertyExtraProperties.cpp:175-181` |
| Order word, whole | 32-bit store | plain (after a compare, or none) | inline `storeOrderWord`; stubs `AOTStubsFieldCreation.cpp:225` |
| Order word, count + 1 | 32-bit `add` to memory | RMW | stub `:95`, `:123` |
| Order word, keep flags | 32-bit load/and/or/store | RMW | stub `:181-185`, `:235-237` |
| Order word, set bit 28 | 32-bit `or` to memory | RMW | stub `AOTStubsPropertyExtraProperties.cpp:182` |
| Flags byte: set the mark | 8-bit load/or/store | RMW | inline `markCellIfMarkerIsStored` `AOTSlotEmitter.cpp:515-519` |
| Flags byte: clear the mark | 8-bit load/and/store | RMW | inline `generateFinishBirth` `AOTAllocationEmitter.cpp:200-206` |
| Flags byte: set `IsPristine` | 8-bit load/or/store | RMW | inline `AOTAccessEmitterDefinitions.cpp:125-130`; stub `aot/stubs/AOTStubsBoxedCalls.cpp:505-508` |
| StructureID of a compiled object | 32-bit store | plain | inline (prototype transition); stub `storeNewStructureID` `AOTStubsPropertyExtraProperties.cpp:104-108` |
| StructureID, inline/out-of-line value, butterfly of the **side object** | 32 / 64 / 64 | plain | stub `:110-135`, `:187-214` |
| Header + order + shape of a new cell | **128-bit** (`movups`/`stp`) | plain | `copyHeaderAndWords`; zeroing of bodies likewise |
| Copy of a whole object's fields | 128-bit chunks (x86-64), 2×64 (ARM64) | multi-word read of a live source | stubs `AOTStubsObjectCopy.cpp:38-57`, `:59-130`, `:137-` |
| `cellState` of a frame cell | 8-bit store | plain | `AOTAllocationEmitter.cpp:304` |
| Environment variable | width of the slot | plain | `PutEnvironmentField` |
| Environment "escaped" byte | 8-bit store | plain | `AOTEnvironmentEmitter.cpp:71` |
| Free-cell word of a reused environment layout (Instance statics) | 64-bit load, store | check-then-act | `AOTAllocationEmitter.cpp:238-255`, `AOTEnvironmentEmitter.cpp:91` |
| Room of a flattened object | per slot; zeroing 1..8 bytes | multi-word | `storeSlotsOfFlattenedObject`, `clearReferencesInRoom` `AOTAllocationEmitter.cpp:341-368` |
| `KeyOrderRun`, `KeyOrderStepCache` entries (Instance) | 64+64+32, 64+32+32 | multi-word | stubs `AOTStubsFieldCreation.cpp:245-255`, `:283-309` |
| `propertyCaches.storesToGuardedFields` counter (Instance) | 8-bit add | RMW | stub `AOTStubsKnownFieldStores.cpp:328-340` |
| Free lists of `LocalAllocator`s named by Instance / records | pointer bumps | RMW | every inline allocation (`emitAllocateFromFreeList`) |
| Write barrier: cell state, mutator mark stack, stored-cell log, cards | various | RMW | barrier stubs; GC report |

---

## Hazards with several mutators

Severity: **JS** = only a wrong or surprising JavaScript value/behaviour. **MEM** = type confusion, wild pointer, use after free, corrupted heap metadata. **CRASH** = a deliberate abort or a fault at an address that cannot be turned into corruption.

### H1. Guard, then access (every "optional" slot) : JS, CRASH for references

Test and use are separate loads (1.3 b, c), and a proof lasts for a stretch of code (H3). Thread B moves or deletes the field, or stores `undefined`, in between. Thread A then treats a marker as a value:

- `OptionalInt32`: reads `INT32_MIN+1..3` as an integer. JS.
- `OptionalDouble`: reads `0x7ffa..0x7ffc << 48` as a double. My arithmetic, not a statement of the source: these are positive quiet NaNs, and adding `DoubleEncodeOffset` (2^49) gives `0x7ffc..0x7ffe << 48`, still inside the double range of the JSValue encoding, so boxing one without purifying does not forge a cell or an int32. JS (a NaN where `undefined` was meant). It does break "every NaN in a slot is pure".
- `OptionalInt64`: reads about 2^62 as an integer, outside the "tracked integer" range that later arithmetic assumes (`MaxTrackedInteger`, `AOTSchema.def:2`). JS at least; whether range-based proofs (array bounds derived from integer ranges) can be broken by it I did not check. **Potentially MEM**, needs a look.
- `OptionalBoolean` via `loadWithoutMarkers`: `load8 & 1`. JS.
- `Cell32`/`OptionalCell32`/`DeferredClosure` decoded without a test: `cageBase + (0|1|2|10) * 8`. The first 16 KB at least are kept inaccessible (`FaultSite::reach = 16 KB`, `aot/image/AOTFaultTable.h:27`; `AOTSlotEmitter.cpp:239`), so the next load through it faults. At a registered fault site that becomes a `TypeError`; elsewhere I expect a crash (not verified). CRASH, not MEM.
- `Cell`/`JSValue` slot read as a proven cell: pointer 0, 2 or 10: fault. CRASH.
- `LiteralString32` etc.: same classes.

The order of the two writes on the mutating side differs between paths (slot first in `Object::store` and `deleteField`, mark first in inline code and `moveFieldToExtraProperties`), but no order fixes a check-then-act reader.

### H2. Lost updates on the order word : JS, some MEM

At least nine kinds of writer (5.7, 5.4, section 3) do plain RMW or whole-word stores on one 32-bit word that mixes key order, count and five flags.

- Whole-word stores of a remembered or constant value (flags = 0) after a compare can erase a flag set in between: **frozen/sealed/not-extensible lost** (JS); **bit 28 lost** while the hidden word and the Structure say "has extra properties": `extraProperties()` returns null, properties vanish (JS), the next `setExtraProperties` overwrites the hidden word (leak, JS); **bit 27 lost**: `countExtraKeys` and friends take names of fields for extra keys (JS; asserts).
- Two threads creating different fields of one object: both read count n, both write n+1 with different orders: one key is never listed (JS: `Object.keys` misses a property that reads fine), or the order number written belongs to a path that lists a field whose slot is empty.
- **Order and count from different sources.** `entriesInOrder(layout, orderOf(word)).first(countOf(word))` (`AOTObjectKeyOrder.cpp:52-53`) trusts the pair. The pair is always written together as one 32-bit store, so tearing inside the word cannot happen. But a torn *cache entry* (H5) can supply an order word of another layout: order number out of range for this layout → out-of-bounds index into `addedOrders` or a `span::first` beyond the list. **MEM** (or a hardened-span abort).

### H3. Proofs that "hold for a stretch" assume that only this thread's operations write : JS to MEM, the central issue

`Proof` lifetimes (`aot/ir/AOTSchema.def:19-28`): `SlotFilled, NoExtraProperties, NoFieldOutsideSlot, BirthFieldsFilled, NotFrozen, Pristine, ArrayStorage, InBounds, NotThenable, MemberUnchanged, PrototypeLinkUnchanged, EvaluatedOnce, KeyOrder, ...` are `Stretch`: they end where an operation *in the same function* declares a write to a heap the test read ("its proof ends where those are written", `aot/analysis/AOTGuardFacts.cpp:90`). Passes `EliminateGuards`, `EliminateRedundantReads`, `HoistLoopInvariantReads`, `HoistTestsIntoPredecessors`, `SinkAllocations` and B3's own CSE/LICM (abstract heaps per field key: `MemoryLocation::objectField(key, receivers)`) all build on declared effects. A second mutator is an undeclared writer of every heap at every instruction.

- A guard hoisted out of a loop is never re-tested; a field read hoisted out of a loop never sees another thread's store (JS: like a data race in C, a spin loop on a plain field never ends).
- The Extension pointer and granule bits may be loaded once before a loop (`AOTSlotEmitter.cpp:369`): see H4.
- `stateOfNewObjectBefore` (`AOTSlotAccessEmitter.cpp:896-954`) is sound with threads **only** while the object is unpublished; it already requires that nothing on the way `mayRunOtherCode()`, so it stays sound as long as no store on the way publishes the object. It allows only stores by slot *into the object itself*, so this one holds.
- Flow facts (`Forever` proofs, `impossibleMutations`, `AlwaysFilled`, `stores=AtMostOnce`, `isUnchangedDuringActivation`, `StoredBeforeEveryRead`) are whole-program facts about *which* operations can happen, not *when*; most survive threads. Exceptions are those that encode an **order in time**: `InitializationState::StoredBeforeEveryRead`, `Occupancy::FilledAtBirth`, `FieldCreation::OnlyAtConstruction`, `ExecutionCount::AtMostOnce`, `isUnchangedDuringActivation`. They assume that a read which comes "after" in one thread's program order comes after in time. If an object can be published to another thread before its constructor ends (it can: `this` escaping), the other thread sees the pre-state. For slots without markers that pre-state is **zero**: `Cell32`/`Cell` zero = null pointer (CRASH), numbers zero (JS).

### H4. The Extension : MEM

- **In-place insertion** (2.3 case 2) moves granules while another thread computes `popcount(bits below)` with the old or new bits. A reader or writer can hit the neighbouring granule. Granules differ in kind (raw vs wide reference vs two narrow references), so thread A may **store a raw double into a granule that the collector and other readers treat as a reference**, or read a reference slot's bits from a raw granule. Type confusion, MEM. The only gate today is "the collector is not marking".
- **Append**: `load64/or/store64` of the bits by two threads loses a granule bit: the loser's subsequent store goes to `ext + 8 + popcount*8` computed with its own view, which may be the slot the winner thinks is *its* granule (both appended "the next" granule into the same room word). Two fields of different kinds alias one word. MEM.
- **Replacement**: stores by another thread into the old Extension after the copy are lost (JS). Two concurrent replacements: one hidden-word store wins, the other's granules vanish; fields listed in the key order read as `undefined`/absent (JS), but a `PutSlot` that "knows" its granule is held (no test, 2.4) then indexes with bits that lack it: `popcount` gives the index of the *next* held granule or of the zeroed room/past the end. **MEM**.
- **No-Extension page**: a `PutSlot` without test on an object whose hidden word is (seen as) 0 would write to the shared read-only page of zeros: fault. CRASH.
- On ARM64 the new Extension's contents are published with a fence only while the collector marks (5.8), so another thread may see the hidden word before `granuleBits` and the copied granules. MEM (stale memory of a recycled auxiliary cell read as bits and references).

### H5. Multi-word caches in the Instance written without synchronization : MEM

Readers compare a key part and then use a value part, with separate loads; writers store the parts separately.

| Cache | Torn-entry consequence |
| --- | --- |
| `AllocationSiteRecord` (32 B; filled by a struct assignment, `AOTAllocationSites.cpp:151-156`; emptied by `forgetStructuresOf`, sweep) | reader tests only "the allocator has room", then copies 16 bytes. Allocator visible before header → new cell with StructureID 0 / wrong order+shape. **MEM** |
| `PrototypeTransition` | `prototype`,`before` of one entry with `after` of another → object gets a Structure of **another layout** → the collector and every by-name path use the wrong reference table. **MEM** |
| `KeyOrderRun`, `KeyOrderStepCache`, `KeyOrderTransition` | wrong order word (H2). `publishTransition` does order its stores (value, key, fence, then the tag, `AOTKeyOrderTable.cpp:757-761`) under the lock, so that one table is safe for lock-free readers on x86-64 and, given reader-side ordering, elsewhere; the two stub-written caches are not |
| `keyOrderTransitions` pointer + mask (two words) and `pages.release(previous)` | `AOTKeyOrderTable.cpp:766`: "Compiled code loads the table and the mask from the instance for every probe and holds neither across a call, so nobody reads the table that is given back." With a second thread inside the stub: new mask with old table = out of bounds; old table after release = **use after free / unmapped**. **MEM** |
| `KeyListCacheEntry`, `EnumeratorCacheEntry`, `PlaceOfPrototypeProperty`, `PrototypeOfClassObject`, `InstanceOfClassBelow`, `CachedField`, `ListedField`, `GetterDefinitionPlan` | key of one entry, value of another: wrong key list (JS), wrong property offset read with `getDirect(offset)` (**MEM**), `FieldData*` of another layout (**MEM**), wrong Structure for a new instance (**MEM**) |
| `CompiledStructure::setListedFields` (pointer, size, then tag; no fence) | `fieldsInKeyOrder` compares the tag then builds `span{fields, size}`: new tag with old pointer/size on a weakly ordered CPU, or two writers interleaved → span over the wrong list. **MEM** |
| Zeroing at each collection | only with mutators stopped? `addClearedEachCollection`: I did not check at which phase. See "Not sure" |

### H6. Publication order of a property added to the side object : CRASH/MEM

Stub `addToExtraProperties` stores both StructureIDs before the value (5.5). A reader keyed on the new outer StructureID loads 0 = empty `JSValue`. Compiled code treats a by-name result as a valid value: empty is "cell with address 0" → fault (CRASH), or it flows into a slot where 0 means *absent* (JS). Beyond that, the side object is an ordinary JSC object mutated by `putDirect`, butterfly growth and dictionary conversion with no cell lock: **all races of the JIT-engine threads work apply to it unchanged** (butterfly replaced while another thread stores through the old one; StructureID/butterfly mismatch → out-of-bounds offset). MEM.

Also: the reader's chain is `StructureID check → hidden word → butterfly → index`. Nothing ties the three later loads to the check.

### H7. Flags byte at offset 6 : JS → MEM through H1

One byte holds the mark of the cell (bit 7), `IsPristine` (bit 6) and Structure flags. Writers: inline set mark, inline clear mark (`FinishBirth`), inline set `IsPristine`, `setPerCellBit`, and `setStructure`'s merge (4.3). All are byte RMW.
- `setStructure` on thread B reads the byte, thread A sets the mark, B writes back: **mark lost**. From then on readers skip tests for a cell that has markers or moved fields: H1 permanently, for all threads.
- `FinishBirth` on the constructing thread clears a mark that another thread (which got the object early) has just set for a deleted field. Same.
- `IsPristine` lost: methods resolved for the layout's prototype are called on an object with another prototype. JS.
- The StructureID store and the flags store are separate: a window with the new Structure and old flags, visible to `HasLayoutPrototype` and to generic JSC code that reads inline flags (`OverridesGetOwnPropertySlot` etc. are equal in all variants, so that part is harmless).

### H8. In-place class changes : MEM

- **SlimClosure → ReifiedSlimClosure** overwrites the closure word with a butterfly pointer (7.2). The nuke protocol protects the collector. A mutator that has loaded the word keeps a valid code offset (fine). A mutator in C++ that has tested `type() == SlimClosureType` and then calls `function()`/`captures()` reads function bits out of a pointer → wrong `captureCountsOf` → out-of-bounds captures. Two threads reifying at once: two butterflies, double `setStructure`, the nuked StructureID read by the second as its "current" one. **MEM**.
- **ClassPrototype → JSFinalObject** (8.1): the two inline words are private counters before and property slots after. A reader in `tryGetDeclaredMember` that increments `m_numberOfLookups` after the conversion writes into inline property storage = forges part of a `JSValue`. **MEM**. These are the most shared objects of a program (every instance's prototype).
- **Lazy class object** (`ensureClassObject`, `initializeClassObject`), lazy `prototype`/`name`/`length` of closures, `FunctionRareData` allocation: ordinary JSC check-then-create on shared function objects. MEM/JS as in the JIT-engine work.

### H9. Check-then-create of lazily made cells : JS (identity), MEM in tables

`materializeDeferredClosure`, `makeFlattenedObject`, `makePendingStars`, `ensureExtraProperties`, `ClosureTable::closure`, `createClassObject`, `createClassPrototype`/`registerClassPrototype`, `objectStructure`/`createUsual`, `findOrCreate`, `EnvironmentShapes::structureFor`, `calleeOfModuleBodies`, `staticFieldAccessor`, template Structures.
- Two threads both see "not made" and both make: `o.method !== o.method` across threads, two class objects for one class (`RELEASE_ASSERT(!closures.closureIfCreated(constructor))`, `AOTClassTable.cpp:157` → CRASH), two prototype objects (`ASSERT` in `registerClassPrototype`), two "usual" Structures for a shape (objects of one layout with different StructureIDs: caches thrash, `Variant::describes` still right: JS-neutral).
- `ensureExtraProperties` twice: one side object is overwritten and its property lost. JS.
- **`makeFlattenedObject`**: copies the room, publishes the cell, zeroes the room. A second thread that has already chosen `base = outer + distance` (6.3) keeps reading and writing the room: reads zeros as values of the slot kinds (raw zero: JS; `Cell32` zero where "always filled": CRASH), and its stores are lost. Two makers: two cells, each with a copy. JS/CRASH. (This form is off by default per the TODO.)
- The C++ containers behind these (`UncheckedKeyHashMap`, `WeakGCMap`, `Vector`, `BitSet`, arenas, `FieldMovePlanCache` with `unique_ptr` eviction while another thread holds `const FieldMovePlan*`) are not thread-safe: rehash during lookup, use after free. **MEM**.

### H10. Publication of new cells on ARM64 : MEM

`mutatorFence`/`fenceAfterAllocation` are real fences only while `mutatorShouldBeFenced` (5.8). Outside marking a new object's header, shape word, markers and fields are not ordered before the store that publishes the reference. Another core can load the reference and see the previous contents of the recycled cell: stale StructureID, a stale shape word that **passes a shape test for another layout**, stale references. MEM. On x86-64 TSO orders the stores, including the 16-byte header copy relative to later stores; whether a 16-byte `movups` store can itself be seen torn does not matter there as long as publication is a later store.

The same holds for closures (captures before word before publication), Extensions, side objects made by stubs, materialized environments.

### H11. Allocators and free lists named by shared records : MEM

`AllocationSiteRecord::allocator`, `instance.allocation.*`, `slimClosureAllocators`, `InlineAllocationData::allocator` point at **one** `LocalAllocator` each; inline code bumps its free list without atomics. Two mutators on one Instance corrupt the free list and hand out the same cell twice. The record is per Instance, so per-thread allocators need either per-thread Instances/records or an indirection. (Details: GC/allocation report.)

The one-word free list of reused environments (7.4) has the same defect in small: `load; test; store 0` lets two threads take the same environment cell; release by one thread while another... the "escaped" byte is per cell and is only about references, not about threads. **MEM** (two activations share variables of different lifetimes; a stale reference slot is typed the same, so it is "only" JS-level confusion between activations of the same layout. Still, values of one activation leak into another, and `poison`/TDZ states are bypassed).

### H12. Object copies by words from a live source : JS, MEM in one case

`createCopyOfWords`, `copyFieldsOfSameLayout` (`AOTObjectAssign.cpp:90-160`), stubs `generateCopyDataPropertiesFrontEnd`, `generateCopyObjectFrontEnd`: test the source's order word and Structure, then copy fields in 8/16-byte chunks, then read the order word (again) and store it in the copy.
- Fields and order word are not a snapshot: the copy may list a key whose slot is absent, or the mark of the cell copied earlier (`copy->setPerCellBit(source.perCellBit())`) may not match markers copied later → the **copy** is permanently inconsistent → H1 for everyone who reads it.
- `generateCopyObjectFrontEnd` checks `orderWord & ~orderAndCountMask == 0` first (`AOTStubsObjectCopy.cpp:191`) and later does `load64 [source+8]; store64 [copy+8]` (`:175-176`): if the source got extra properties in between, the copy has bit 28 set, the **same side object** in its hidden word (copied with the fields) and a Structure that says "no extra properties". Two objects share one side object. JS, and invariants of `setExtraProperties` break.
- The Extension of the source is copied granule by granule while it may be inserted into (H4).
- A 16-byte `movups` load is two 8-byte halves at worst; each slot is naturally aligned within 8 bytes, so no single slot tears.

### H13. Structure variants and `setKeyOrder` : CRASH

Reader `Object::keyOrder`: order word says out of line, then `compiledStructure().outOfLineKeyOrder()` → `rareData()->...`. With the write orders of 5.7 a single writer never exposes (out-of-line word, Structure without list) in program order, but on ARM64 the two stores are unordered, and two writers (one going out of line, one `didChangeExtraProperties` computing its variant from the *old* Structure and storing it afterwards) can leave exactly that pair for good: null `rareData()`. CRASH. More generally **every `setStructure(variantOf(current) with one thing changed)` is a read-modify-write of the StructureID**: concurrent changes of prototype, extra properties, mayBePrototype, key order lose one another. A lost `mayBePrototype` or `hasOtherPrototype` silently disables invalidation of chain caches: JS, and via stale cached offsets on prototype chains possibly MEM (property-cache report).

### H14. Slots themselves : fine, with two footnotes

A single slot store or load is one naturally aligned access of 1/2/4/8 bytes: no tearing on x86-64 or ARM64. Racing plain stores to one slot give one of the values. Footnotes:
- `holdsDeclaredType` for `OptionalDouble` reads the **upper 32 bits** and the value load reads 64: two reads (H1).
- `LiteralString32`, `OptionalInt32` stores compute the word from the value with selects: single store. Fine.
- The write barrier after a reference store is per mutator state (mark stack, stored-cell log in `instance.heap`): GC report.

### H15. Native readers that write

`Object::read`, `extraProperties()`, `getOwnPropertySlot`, `fieldNamed` (fills `m_cachedFields`), `fieldsInKeyOrder` (fills the Structure's cache), `countExtraKeys` (`setNumberOfExtraKeys`), `sourceOfPendingBinding` (counter), `ClassPrototype` lookups. A design that lets "read-only" operations run without exclusion has to deal with each of them. There is also one `thread_local` already (`objectListedWithoutOrder`, `AOTObjectInternal.cpp:42`).

### H16. Deliberate aborts that a race can reach

`crashOnWrongOperand` in the field operations re-checks what the stub saw (`AOTFieldOperations.cpp:77-78`, `:93-94`, `:120-121`, `:231-232`, `:244-245`, `:262-263`): e.g. stub found the slot filled, operation finds it empty → abort. `RELEASE_ASSERT`s in `readSlot` (`AOTSlotAccess.cpp:166`, `:176`), `moveFieldToExtraProperties`, `createClassObject`. With threads these become reachable by legal programs. CRASH.

---

## What helps (compared with a JIT engine)

1. **The shape word never changes, and the slot kinds of an object never change.** Offsets, widths and "is this word a reference" are functions of a constant. There is no butterfly for declared fields, no inline/out-of-line transition of a given field, no `Int32 → Double → Contiguous` style conversion of object storage. A shape test needs no re-validation, ever (`AOTGuardFacts.cpp:48`).
2. **An object never converts to another representation.** The worst case costs one slot (marker) plus the side object. No "become dictionary", no "uncacheable".
3. **Every slot is naturally aligned and at most 8 bytes**, so each field access is single-copy atomic on both targets. Racing plain stores to declared fields with non-optional representation and proven types are already memory-safe with **zero** changes: no test, no transition, no metadata.
4. **No code is patched, no watchpoints fire into code, no OSR exit, no deoptimization, no inline caches in code.** All run-time knowledge is data: tables in the Instance/State. Making a table thread-safe (per-thread copy, seqlock, single-word entries, or a lock on the fill path) is local work. Failing a test goes to generic code in the same frame through ordinary SSA edges.
5. **Read-only image**: layouts, field records, reference tables, key orders of the compiler, shape sets, closure words, identifiers, literal strings are immutable and shared for free.
6. **The collector needs only StructureID → layout**, the same for all variants, and already tolerates a concurrently running mutator for: field stores, Structure changes, Extension append and replacement, slim-closure reification, closures under construction, inner cells. The protocols exist (`storeStoreFence` + barrier, nuke, "read the bits once"); several generalize from "one mutator + markers" to "n mutators" if in-place insertion is dropped and fences become unconditional.
7. **Markers decode to harmless things.** Narrow markers decode into the never-allocated, mostly inaccessible first 16 MB of the cage; wide markers are non-cell JSValues or null; double markers stay doubles when boxed. A lost race on an optional *reference* slot faults instead of forging a pointer.
8. **Whole-program flow knows what is shared.** It already computes escape (`FrameLocality`, `AllocateInFrames`, `impossibleMutations`, `ReachNativeCode`, `ReachUnknownJavaScript`, allocation classes). The same machinery can compute "may be reached by another thread" per allocation class and keep today's code for everything else. A closed program (`AdmitsUnknownJavaScript` clear) makes that analysis complete.
9. **Frame cells and not-yet-published objects are thread-private by construction**, provided that starting a thread or sending a value counts as a native call/escape ("Native code gets no cell of a frame").
10. **Closures are immutable where it matters**: closure word and captures are written once before publication. By-value capture is used exactly for bindings that never change. Shared mutable bindings are concentrated in `Environment` cells and module variables, each a fixed-layout array of aligned slots with a constant header.
11. **Effects are declared in one schema** (`reads(...)`/`writes(...)` per operation, abstract heaps, `Proof` lifetimes). "Another thread may write heap H here" can be expressed in that vocabulary (for example: proofs over possibly-shared objects do not outlast a synchronization operation, or are re-tested at use), and the passes already honour it.
12. **Validating builds** (`--aot-validate`, `impossibleMutations`, sabotage tests) give a ready harness to check new invariants at run time.
13. One word (`order word`) and one byte (`flags`) hold all mutable per-object metadata, adjacent to the shape word in one aligned 8-byte word at offset 8 (order+shape) and in the 8-byte header. **CAS on a 64-bit word can cover order word + shape, or StructureID + flags + type + cell state**, so an atomic protocol for slow-path transitions needs no extra space.

---

## Not found / not sure

- **Which guards each store plan requires.** `PutSlot` declares `relies = Shape, NotFrozen` only (`prepared-operations.def:41`). That a store into a slot whose property has moved out (accessor in the side object) is prevented in single-threaded code must come from `plan.requiredProofs` (`SlotFilled`, `NoFieldOutsideSlot`; hinted at `aot/passes/AOTResolveAccesses.cpp:419-421` "A store that finds its slot filled looks no further"). I did not read `aot/analysis/AOTPropertyAccess*` where plans are made.
- **`PutSlots`** (runs of stores) and `TryPutByIds`: I read the schema lines and the key-order stubs, not their emitters.
- **What happens on a fault at an unregistered site** (I assume the process dies): `aot/image/AOTFaultHandler.*` not read.
- **How much of the first 16 MB of the cage is inaccessible.** I know `FaultSite::reach = 16 KB` is asserted to be below the zero page at 8 MB, and that nothing is allocated below 16 MB. I did not read the protection calls in `heap/MarkedBlockCage.cpp` beyond `makeEmptyExtensionReadable`.
- **At which phase `addClearedEachCollection` tables are zeroed**, and whether mutators are stopped then (`aot/runtime/AOTHeapCacheRegistry.*` not read beyond the lock).
- **Whether B3 really keeps the test load and the value load separate** for a given access, or merges them. That depends on B3 CSE and on the direct code generator (`AOTDirectCodeGenerator.cpp`), which I did not read. The IR has two loads.
- **`OptionalInt64` markers and integer-range proofs** (H1): not traced.
- **Member guards, the method table, `MemberGuards.cpp` (1,762 lines), `ChainDependents`, property caches**: only touched. They are written by several mutations above (`willAddExtraProperty` → `didShadowMember`, `didChangeInstancePrototype`, `didChangeOwnProperties` → `vm.invalidateStructureChainIntegrity`).
- **`AOTObjectAssign.cpp` (1,587 lines), `AOTAssignmentPlans.*`, `AOTObjectCloneAs.cpp`, `AOTObjectBuilder.*`, `AOTObjectKeys.cpp`**: read only in part (copy by words, plan cache shape). They create new objects or write targets through `store`/`tryWriteSlot`/`applyMoves`; I saw no further kind of in-place mutation, but I did not read every function.
- **`AOTClosure.cpp` / `AOTClosureMethodTable.cpp`** beyond creation: the lazy-property protocol of full closures and class objects is described from headers and call sites.
- **Coroutine frames** (`LayoutKind::CoroutineFrame`): same class as environments, wide references; who mutates them and when is not covered here.
- **Module namespace objects** (`LayoutKind::ModuleNamespace`): "has no shape word" (`AOTShapeTestEmitter.cpp:40`). Not covered.
- **Whether form 2 of `FlattenInnerObjects` is reachable in shipped builds.** The test file's TODO says it is off; I did not find and read the switch.
- **Line numbers** were read at the cited place or checked with `grep -n`; a range may be off by a line or two at its end.
