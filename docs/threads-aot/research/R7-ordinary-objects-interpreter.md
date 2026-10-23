# R7. Ordinary objects, arrays, collections, strings, typed arrays, RegExp, the interpreter, and the engine's unlocked caches in an AOT executable

Engine tree: `<engine>/` at 5c2923e566cb. Unless a path starts with `Source/` or `src/`, it is relative to
`Source/JavaScriptCore/`. `WTF/…` means `Source/WTF/wtf/…`. `bun:` means `<bun>/`.
"Diff" means `git diff 8deea350d4e1 HEAD`. "Mutator" means an OS thread that runs JavaScript.

Sections 7 to 10 were researched separately
under the same brief, each reading its own sources; the author spot-checked them (see 13.3).

Contents
1. Which values are still ordinary JSC objects
2. Arrays
3. Ordinary objects: butterflies, Structures, dictionaries, locks that exist
4. What AOT changes in Structure / JSObject / JSCell
5. Map, Set, WeakMap, WeakSet
6. Strings
7. Typed arrays, ArrayBuffer, SharedArrayBuffer, Atomics
8. The interpreter under AOT
9. RegExp at run time
10. Per-VM, per-realm and per-object caches that C++ uses without locks
11. Hazards with several mutators (sections 1 to 6; 7 to 10 carry their own lists)
12. What helps
13. Not found / not sure

---------------------------------------------------------------------------------------------------------------------------------------

## 1. Which values in a compiled program are still ordinary JSC objects

### 1.1 The cell kinds that AOT adds (everything else is unchanged JSC)

| JSType (`runtime/JSType.h`) | C++ class | Butterfly word at offset 8? |
| --- | --- | --- |
| `CompiledEnvironmentType` (:76), below `ObjectType`, "a cell whose header bytes are shape bytes is never an object" (`runtime/JSCell.h:318-319`) | `AOT::Environment : JSCell` (`aot/object/AOTEnvironment.h:36`) | no; header bytes 4 and 6 are shape bytes, so `cellLock()`, `indexingTypeAndMisc()`, `inlineTypeFlags()` assert it is not one (`runtime/JSCell.h:152-176`) |
| `CompiledClosureType` (:83) = `JSFunctionType + 1` | `AOT::Closure : JSFunction` (`aot/object/AOTClosure.h:21`) | **yes**: it is a JSFunction, an ordinary object with Structure + butterfly for its named properties |
| `CompiledObjectType` (:160) | `AOT::Object : JSObject` (`aot/object/AOTObject.h:113`) | no (order word + shape word lie there) |
| `SlimClosureType` (:161) | `AOT::SlimClosure : JSObject` (`aot/object/AOTSlimClosure.h:31`) | no |
| `SlimClosureWithPropertiesType` (:162) | `AOT::ReifiedSlimClosure : JSObjectWithButterfly` (`aot/object/AOTSlimClosure.h:112`) | yes |

`objectTypesWithoutButterfly { WebAssemblyGCObjectType, SlimClosureType }` (`runtime/JSType.h:205-206`) is the range for which
`JSObject::butterfly()` answers null (`runtime/JSObject.h:462`). Further AOT cells that are plain `JSCell`s or ordinary objects:
`AOT::CompactTable : JSCell` (storage of every Map and Set, section 5), `AOT::StringBuilderStorage : JSCell` (6.7),
`AOT::ClassPrototype : JSNonFinalObject` (`aot/object/AOTClassPrototype.h:16`), `AOT::ModuleNamespace : JSNonFinalObject`
(`aot/runtime/AOTModuleNamespace.h:24`), `AOT::Generator : JSNonFinalObject` (`aot/runtime/AOTGenerator.h:26`),
`AOT::CompiledStructure : Structure` (`aot/object/AOTCompiledStructure.h:26`).

### 1.2 Inventory

| Value in the program | What it is at run time | Evidence |
| --- | --- | --- |
| Array literal, `new Array(n)`, `Array.of`, rest parameter, spread result, `slice`/`concat`/`map` result | ordinary `JSArray` with one of the realm's *original array Structures*, butterfly with `IndexingHeader`. New: cell and first storage are **one allocation** (2.2). May also lie in the caller's frame ("frame array", 1.5) or have copy-on-write storage in the image (2.8) | IR `NewArray`, `CreateRest` (`aot/ir/schema/operations.def:50`, `prepared-operations.def:75`); `aot/runtime/AOTArrayAllocationProfile.h:59-64` |
| Object literal whose names are known | `AOT::Object` with a layout (`NewObject`, `operations.def:47`); bun: `src/aot/lower/object/literal.rs:415-418` (`literal_layout` → `begin_object`) | not ordinary (R1) |
| Object literal under a **dictionary type** (`Record<string,T>`, index signature without members), or an empty literal typed so | ordinary `JSFinalObject` made by `NewDictionary` | bun: `src/aot/lower/object/literal.rs:36-53, 402-407`; `src/aot/lower/layouts/dictionaries.rs:23` "The keys of `Record<string, Item>` are data: no name is known at build time, so a layout would have no field"; engine: `aot/runtime/AOTDictionaryOperations.cpp:22-30` (`constructEmptyObject(vm, globalObject->objectStructureForObjectConstructor())`) |
| Object literal stored as `F.prototype = { … }` | also `NewDictionary` (ordinary object) | bun: `literal.rs:394, 407` (`is_stored_as_prototype_object`) |
| `{ __proto__: null }` (empty) | `Object.create(null)` built-in call → ordinary object | bun: `literal.rs:383-388` |
| Literal that begins with a spread `{ ...a, x }` | `CopyObject`: "The copy makes the object, with the layout of the source where that is a record. Every later member is defined by name" | bun: `src/aot/lower/object.rs:79-91` |
| Computed key on a compiled object, `delete`, redefinition, a store a slot can't hold | the compiled object stays; the property moves to its **extra properties object, an ordinary `JSFinalObject`** (`constructEmptyObject(vm, …emptyExtraPropertiesStructure)`) | `aot/object/AOTObjectExtraProperties.cpp:632-655`; "The elements of a compiled object are those of its extra properties" (`aot/stubs/AOTStubsElements.cpp:124`) |
| `JSON.parse` result | where an expected type gives a layout: `AOT::Object` built by a plan (`ObjectBuilder::tryBuild`); otherwise ordinary `JSFinalObject` (`createWithButterflyCopyingInlineStorage`) and `JSArray` | `aot/runtime/AOTJSONValueParser.cpp:196, 292, 304, 310` |
| Objects made by native code (Bun APIs, `Object.fromEntries`, `Object.create`, `structuredClone`, iterator results of native iterators, `Intl.*`, Date, Promise, Error …) | ordinary JSC cells of their own classes | unchanged engine code |
| `arguments` | unmapped: `ClonedArguments`; mapped (sloppy): `DirectArguments` whose slots the compiled function reads and writes through `GetMappedArgument`/`SetMappedArgument` | `aot/runtime/AOTArgumentsOperations.cpp:86-205`; `operations.def:58-61` |
| Class prototype object | `AOT::ClassPrototype`, a `JSNonFinalObject` that overrides every method-table hook and answers from the image's class member table until it is touched; then `createMembers` / `convertToOrdinaryObject` make it an ordinary object in place (`JSCell::setStructure` allows that one change of ClassInfo: `runtime/JSCellInlines.h:262-263`) | `aot/object/AOTClassPrototype.h:16-52` |
| Function objects | `AOT::Closure` (JSFunction subclass, ordinary named properties), or `SlimClosure` (no butterfly) which is *reified* into `ReifiedSlimClosure` when somebody needs properties (`CompiledCallee::reifyProperties`, called for instance from `didBecomePrototype`, `runtime/StructureCreateInlines.h` diff) | R6 covers calls |
| Module namespace object | `AOT::ModuleNamespace` (method-table object) | `aot/runtime/AOTModuleNamespace.h:24` |
| Errors | ordinary `ErrorInstance` (`NewRuntimeError`, `NewSuppressedError`, `new Error` built-in) with lazy stack materialisation (section 10) | `operations.def:56-57` |
| Map / Set | ordinary `JSMap`/`JSSet` cells, but `m_storage` is always an `AOT::CompactTable` in a VM with compiled code (section 5) | `aot/runtime/AOTCompactTable.cpp:53-59` |
| RegExp literal | ordinary `RegExpObject` + `RegExp` cell (section 9) | `operations.def:54` |
| Template object | ordinary frozen `JSArray` made once per site (`NewTemplateObject … boundTo = Site`, with a `staticsOffset`) | `operations.def:55` |
| Generators, promises | `AOT::Generator` / `JSPromise` (R-coroutines) | `prepared-operations.def:112-113` |
| Everything made by interpreted code (`eval`, `new Function`, `node:vm`) | ordinary JSC objects only: `op_new_object` → `JSFinalObject`, `JSFunction` with `FunctionExecutable`, `JSLexicalEnvironment` … (section 8) | |

So: **yes, the compiler gives some object literals an ordinary `JSFinalObject`**, on purpose, whenever the object is used as a dictionary or
as a hand-written prototype object.

### 1.3 "Dictionary" has two meanings

1. AOT's IR word: `NewDictionary () -> natural(object)` (`aot/ir/schema/operations.def:48`) = an *ordinary empty JSFinalObject* whose members are
   all defined and read **by name** through the per-VM property tables or, if "No table has the Structure and the name: mostly because they meet
   for the first time, as where an object serves as a dictionary. The objects themselves answer, and nothing is filed"
   (`aot/stubs/AOTStubsKeyedAccess.cpp:263`), by machine code that probes the Structure's `PropertyTable` directly (3.6).
2. JSC's own *dictionary mode* of a Structure (`isDictionary()`, cacheable / uncacheable), which such an object enters after enough additions
   (`shouldDoCacheableDictionaryTransitionForAdd`, `runtime/Structure.cpp:871-877`) or a delete. Then the Structure belongs to that one object
   and is mutated in place (3.4).

New helpers for such objects: **property runs** (`runtime/PropertyRun.h`): a list of names interned per VM
(`vm.propertyRuns.intern`, `PropertyRunTable::m_runs`, `m_runsByLastName`, plain `HashSet`/`HashMap`, no lock, `PropertyRun.h:119-144`), one Structure transition for the whole
run (`TransitionKind::PropertyRunAddition`), a per-VM weak `PropertyAdditionCache` (`PropertyRun.h:59-117`), and per-Instance lazily filled
`instance->runtime.literals.propertyRuns[number]` (`aot/runtime/AOTObjectLiteralOperations.cpp:22-35`: check null, build, store).
`Structure::createForOneObject` makes an unshared Structure ("no Structure is looked for, and none is remembered for the next", `runtime/Structure.h:314-317`).

### 1.4 "Compact table"

`AOT::CompactTable` (`aot/runtime/AOTCompactTable.h:35`) is the hash table behind **every** `Map` and `Set` of a VM with compiled code:
"A VM with compiled code gives every Map and Set this storage, and add() never declines a key" (`AOTCompactTable.cpp:55`). One GC cell from
`vm.cellSpace()`: 24-byte header, an index (tags / 16-bit / 32-bit slots), a key column and a value column of 32-bit ("narrow", caged references) or
64-bit words. A table may also be a thin cell over a read-only `TableInImage` (`NewSetFromData`/`NewMapFromData`). `AOT::CompactStorage`
(`aot/runtime/AOTCompactStorage.h:20-45`) is the C API through which `runtime/JSOrderedHashTable.h` reaches it. Details in section 5.

### 1.5 "Frame array"

"Compiled code keeps an array that nothing writes to and nobody keeps in its frame" (`aot/runtime/AOTFrameArrays.h:18`). It is a real `JSArray` header +
`IndexingHeader` + elements laid out in the **native stack frame** of the caller (`sizeOfFrameArray`, `aot/stubs/AOTStubsArrays.h:67-69`), recognised by
`cellState() == frameCellState && type() == ArrayType` (`AOTFrameArrays.h:19-22`). Only six operations may be handed one (`mayBeGivenFrameArray`, :25-38);
anything that would keep it copies it (`copyFrameArrayToHeap`, `aot/runtime/AOTArrayOperations.cpp:80-88`; `heapValueOf`, `AOTFrameArrays.h:42-47`).
By construction it never escapes to another thread as long as the escape analysis (`aot/analysis/AOTFrameArrayParameters.h`,
`aot/passes/AOTFrameAllocationPointers.h`) treats "hand to another thread" as an escape, which today can only happen through a store or a call it already models.

---------------------------------------------------------------------------------------------------------------------------------------

## 2. Arrays

### 2.1 Where the words live

```
JSArray cell            0  StructureID (32)   4 indexingTypeAndMisc (8)  5 JSType (8)  6 inline flags (8: IsPristine = 0x40)  7 cellState (8)
                        8  Butterfly*  ────────────┐
butterfly allocation    [elementsStartOffset word]? [out-of-line properties …] [IndexingHeader: publicLength(32) vectorLength(32)] │ element 0, 1, …
                                                                                                                                  ^ Butterfly* points here
```

* `publicLength` and `vectorLength` are in the `IndexingHeader` directly in front of element 0 (`runtime/IndexingHeader.h:49-50, 126-132`). **So one load of
  the butterfly pointer gives a consistent pair (bounds, elements)**: whoever uses one loaded pointer for the bounds test and for the access can't run off
  that allocation, whatever other threads do to the array, provided the allocation is still alive and its header is not overwritten (see 2.6 and 2.7 for the two
  places where that proviso fails).
* The **kind of the elements (Int32 / Double / Contiguous / ArrayStorage, CopyOnWrite bit) is not in the butterfly**. It is in byte 4 of the cell (and in the
  Structure). Kind and contents are therefore two separate memory locations and **can disagree** for a second observer (2.5).
* Byte 4 also holds the two bits of the cell lock (`IndexingTypeLockIsHeld = 0x40`, `IndexingTypeLockHasParked = 0x80`, `runtime/IndexingType.h:97-98`).
  Compiled stores compare the *whole* byte with the expected indexing type: "A lock bit that another thread holds for a moment sends the access the slow way"
  (`aot/codegen/AOTElementAccessEmitter.cpp:169-174, 198-201`). So holding `cellLock()` already diverts every inline/stub *writer* (and the readers that use
  `hasWritableShape`) to C++. Readers that mask with `IndexingShapeMask` / `arrayStorageTestMask` ignore the lock bits.

### 2.2 Indexing types in an AOT process

* All of JSC's indexing types still exist. What changes is the policy: `VM::arrayStoragePolicy()` is `KeepsDenseArraysDense` when `hasCompiledCode()`
  (`runtime/VM.h:520`; `runtime/ArrayConventions.h:108-137`). Effects (all in the diff of `runtime/JSArray.cpp`, `JSObject.cpp`):
  stores far beyond the vector stay dense if `index/2 <= vectorLength` and dense enough (`ArrayConventions.h:118-125`); bulk copies stay dense up to
  `MAX_STORAGE_VECTOR_LENGTH` (:127-132); `shift`/`unshift`/`splice` of long arrays no longer convert to ArrayStorage but **move the start of the elements**
  (2.7); an array that did get ArrayStorage **comes back** to Contiguous when it is dense again (`returnsFromArrayStorageWhenDense`, `JSArray.cpp:574-575` →
  `JSObject::tryConvertArrayStorageToContiguous`, `JSObject.cpp:3622-3662`: new butterfly, then under `cellLock()`: nuke, set butterfly, set Structure).
* Compiled code and stubs have fast paths for exactly three "forms": `ArrayStorage { Other Int32 Double Contiguous }` (`aot/ir/schema/types.def:25`). Undecided is treated
  as "every position absent" by readers (`aot/stubs/AOTStubsArrayReads.cpp:61`) and sent to C++ by writers. ArrayStorage / SlowPut always go to C++.
* **Array born as one cell**: "An array on the heap is born as one cell: the cell of the array, the indexing header and the elements, one behind the other"
  (`aot/runtime/AOTArrayAllocationProfile.h:59-64`; `tryCreateArrayWithStorage`, `aot/runtime/AOTArrayOperations.cpp:145-168`; stubs `ArrayLiteral`, `NewEmptyArray`, `ArraySlice`).
  The butterfly pointer then points *into the array's own cell*. `isStorageInOwnCell()` (`runtime/JSObjectVisitInlines.h:38-48`): "Such storage is no allocation of its own: nobody
  marks it, and behind its vector there is no room that a size class left over." When the array grows, the storage moves to `auxiliarySpace` and the tail of the cell is dead weight.
* **The pristine bit** (`IsPristine = 1 << 6` of the inline flags, `runtime/JSTypeInfo.h:48`; copied into byte 6 of every cell): for an array it means *both* "no own name shadows
  anything on the locked prototype chain, prototype is the realm's `Array.prototype`" *and* "elements are plain: no indexed accessors, no SlowPut, not having a bad time"
  (`Structure::setPristineBits`, `runtime/Structure.cpp:654-665`: "Compiled code asks the cell of an array one bit for its names and its elements"; full list of what it implies:
  `JSArray::validatePristine`, `runtime/JSArray.cpp:1295-1327`). Because `Array.prototype` and `Object.prototype` have immutable indexed properties in the main realm,
  "Nothing stands under an index on the prototypes of the array, so an element that is not there reads as undefined" (`aot/stubs/AOTStubsArrayReads.cpp:37`).
  Test in line: one byte load + bit test (`ElementAccessEmitter::isPristineArray`, `aot/codegen/AOTElementAccessEmitter.cpp:112-118`).
* **Aspects that no bit records**: "Nothing in an object says that it stays as created, or that it has no holes. Only flow proves these."
  (`aot/builtins/AOTBuiltinPristine.h:32-33`: `aspectsWithoutTest { AsCreated, Dense }`). `Dense` is a fact of the *type* (`ClassPart.provenPristine`, `arrayStorage`,
  `contentsAreProven`, `types.def:33-36`) proven by the whole-program flow analysis over all writers.
* **Arrays frozen in place**: `Object.freeze(array)` with the dense policy (`runtime/ObjectConstructor.cpp:1325`) calls `JSObject::tryFreezeArrayInPlace`
  (`runtime/JSObjectFrozenElements.cpp:49-76`): the elements are copied once into a `JSCellButterfly` (copy-on-write storage), the Structure becomes
  `originalArrayStructure(CopyOnWrite…)` + `TransitionKind::FreezeElementsInPlace` (which also sets `hasImmutableProperties`). Readers keep their fast paths (shape bits are unchanged), writers
  fail the "writable shape" test because of the CopyOnWrite bit. Leaving that state (`moveFrozenElementsToSparseMap`, :17-47) happens on `defineOwnProperty`, `pop`, becoming a prototype.

### 2.3 Proofs: the check and the access can be far apart

The IR separates tests from accesses. `HasArrayStorage … proves = ArrayStorage`, `IsInBounds … proves = InBounds`, `IsElementPresent … proves = InBounds`, `IsPristine … proves = Pristine`
(`aot/ir/schema/prepared-operations.def:19-22`); `GetArrayElement`/`PutArrayElement`/`GetArrayLength`/`AppendElements` say `relies = Pristine, ArrayStorage, InBounds` and carry the flags
`isInBounds isPresent readsHoleAsNaN` and the payload `arrayStorage` (:58-62). All three proofs are `Stretch` proofs (`aot/ir/AOTSchema.def:19-28`):
`Pristine(Stretch, true) ArrayStorage(Stretch, true) InBounds(Stretch, true)`. A stretch ends only at an operation **of the same function** whose declared effects may write what the test read:
`EndingsAtWriter::endingOf` (`aot/passes/AOTTypeTestEndings.cpp:31-47`): `if (!mayWriteDependenciesOf(condition, program, m_effects)) return ProofEnding::DoesNotEnd;`.
The abstract heaps are `ArrayStorage`, `Elements[Array]` (`AOTSchema.def:66-67`). Nothing models a write by another thread.

What code generation does with a proof (`aot/codegen/AOTElementAccessEmitter.cpp`):
* known storage + `isInBounds`/`isPresent`: `readInsideLength` (:512-536) = `load butterfly; load element` with **no kind test and no bounds test** (they exist only under `--aot-validate`).
* `Dense`: no hole test (:270-275, :464-469); a dense Int32 array read as a number loads only the low 32 bits (:474-476); a dense Int32 store of an int32 **stores only the low 32 bits**
  (`replaceElement`, `AOTElementAccessEmitterStores.cpp:195-206`: "Inside the length of an array of int32 without holes every element is a boxed int32, and all of them have the same high half").
* B3 may additionally CSE/hoist the loads of the butterfly pointer, `publicLength`, the indexing byte across anything that does not write the matching B3 abstract heap (`aot/codegen/AOTHeaps.*`).

### 2.4 The exact sequences

Registers are omitted. "slow" = call of the named C++ operation. All loads and stores are plain (no acquire/release, no atomics).

**`a[i]` read, stub `ArrayGetAtIndex`** (`aot/stubs/AOTStubsArrayReads.cpp:38-100`), used outside "fast" code shapes and behind every inline arm:
1. `shape = load8(a+4) & IndexingShapeMask` (:54-55)
2. `b = load64(a+8)` (:56)
3. Contiguous or Int32: `len = load32(b-8)`; if `i >= len` (64-bit unsigned) absent; `v = load64(b+8i)`; if `v == 0` absent; return v (:63-72)
4. Double: same with `loadDouble`; NaN = absent; result boxed as int32 if exact, else as double (:74-89)
5. Undecided: absent. Other shapes: slow `operationAOTGetByVal`.
6. absent: if `i > MAX_ARRAY_INDEX` slow, else `undefined` (no prototype walk: the caller has established Pristine).

**`a[i]` read in line** (`readWithAnyStorage`, `AOTElementAccessEmitter.cpp:571-621`): `shape = load8(a+4) & mask` (:589), `b = load64(a+8)` (:590), branch on shape, `load32(b-8)` compare, `load64(b+8i)`, hole test unless Dense.
With known storage: steps without the shape test (`readWithKnownStorage`, :538-569); with `isInBounds`: only `load64(a+8)`, `load(b+8i)` (:512-536).

**`a.length`**: `load64(a+8)`, `load32(b-8)` (`arrayLength`, :223-226). Relies on Pristine/ArrayStorage so that `publicLength` is the length (for ArrayStorage the same word is the length too).

**`a[i] = v` stub `ArrayPutAtIndex`** (`aot/stubs/AOTStubsArrayStores.cpp:25-77, 128-141`):
1. `kind = load8(a+4) & IndexingShapeAndWritabilityMask` (includes CopyOnWrite) (:35-36); not one of the three → slow.
2. `b = load64(a+8)`; `vl = load32(b-4)`; `i >= vl` → slow (:41-45)
3. value must fit the kind (Int32: int32; Double: number, not NaN), else slow
4. **store element** `store64(b+8i)` (:57 / :69)
5. `len = load32(b-8)`; if `i >= len`: `store32(b-8, i+1)` (:72-76). (A store beyond the length but inside the vector leaves holes between; fine because slots between length and vector end are holes.)
6. element barrier (`emitElementBarrierAndReturn`, `AOTStubsArrayPush.cpp:45-63`).

**`a[i] = v` in line** (`storeInVector`, `AOTElementAccessEmitterStores.cpp:161-193`): whole byte 4 compared with the form's indexing type (:263 / :287-293), `b`, `len`; inside the length: store. Beyond the length and below
`vectorLength`: **`setPublicLength(i+1)` first (:177), the element store after it (:181 / :186)**, the opposite order of the stub.

**`a.push(v)` stub `ArrayPush`** (`aot/stubs/AOTStubsArrayPush.cpp:75-136`):
1. `kind = load8(a+4) & IndexingShapeAndWritabilityMask` (:87-88)
2. `b = load64(a+8)`; `len = load32(b-8)`; if `len >= load32(b-4)` → grow (:90-92)
3. fit test by kind; `store64/storeDouble(b+8*len)` (:99, :111, :121)
4. `store32(b-8, len+1)` (:114-115, :122-123); barrier for cells; returns the boxed new length.
5. grow: `growFullVector` (2.6), then **jump back to step 1** (:128-130: "No program can observe the capacity, so the value may still turn out not to fit the kind").

**`a.push(v)` in line**, two emitters with two orders:
* built-in call `pushOne` (`aot/codegen/AOTArrayStorageBuiltinEmitter.cpp:151-193`): `b`, `len`, `len < vl`, byte 4 compare, (cells: array must be beyond the barrier threshold else stub), **store element (:181), then length (:187)**.
* IR `AppendElement` (array literals with spreads, comprehension-like code) `appendWithMode` (`AOTElementAccessEmitterStores.cpp:359-371`): **length first (:365), then element (:366)**.
* `pushSeveral` (:218-261): `makeRoom` may call `operationAOT…ArrayGrow` and reload the butterfly; stores all elements, then the length.
* `AppendElements` stub (`AOTStubsArrayPush.cpp:180-329`): for a Contiguous target "The collector visits the elements below the length, which are holes until they are copied": **length first (:238, :258)**, then the copy loop; for Int32/Double targets copy first, length last (:286, :297, :306), and on meeting a hole it un-writes what it wrote (:308-320).

**`a.pop()` in line** (`aot/codegen/AOTArrayBuiltinEmitter.cpp:198-221`): `b`, `len`; writable-shape test; `len == 0` → undefined; `v = load(b+8(len-1))`; hole → miss; **store hole (:213), then `publicLength = len-1` (:214)**. C++ `JSArray::pop` has the same order (`runtime/JSArray.cpp:1514-1545`).

**`a.shift()` in line** (:146-196): for `len <= maxLengthForElementwiseMove (32)`: read element 0, move each element down one by one with plain load/store, store a hole in the last, set the length.

**`a.length = n` in line** (`generateArraySetLength`, `AOTArrayStorageBuiltinEmitter.cpp:79-139`): writable shape; growing within the vector: just `store32(b-8, n)`; shrinking by at most `MaxInlineClearedElements`: store holes from `n` to `len` (:115-122), **then** the length (:125). Otherwise `putById(length)`.
C++ `JSArray::setLength` (`runtime/JSArray.cpp:1432-1472`): if more than 64 and more than half is dropped: `reallocateAndShrinkButterfly` = **new, shorter butterfly**, `storeStoreFence`, publish pointer (`JSObject.cpp:4195-4216`); else clear then `setPublicLength`.

**Holes**: empty JSValue (0) in Int32/Contiguous, PNaN in Double. "Whatever lies between the length and the end of the vector is a hole" (`AOTStubsArrayPush.cpp:308`) is an invariant that all fast paths rely on.

**Generic `o[k]`** when the receiver is not known to be an array: stubs `GetByVal*`, `PutByVal*` (`aot/stubs/AOTStubsElements.cpp`, `AOTStubsKeyedAccess.cpp`) test the JSType / indexing byte, then do the same three-form sequences, typed arrays, strings, `arguments`, compact "tables that a literal with numbers for keys has made" (`AOTStubsElements.cpp:176`), and the extra-properties object of a compiled object (:124). `PutByVal` at `index == publicLength == vectorLength` grows in the stub (:496-500).

### 2.5 Conversions between kinds

| Conversion | Where | In place? | Order of writes |
| --- | --- | --- | --- |
| Undecided → Int32/Double/Contiguous | `JSObject.cpp:1210-1258` | in place | fill, then `setStructure` |
| Int32 → Double | `convertInt32ToDouble`, `JSObject.cpp:1311-1336` | **in place** | every slot of the vector rewritten from boxed int32 to raw double (top down), *then* `setStructure` (no fence between) |
| Int32 → Contiguous | `convertInt32ToContiguous`, :1338-1348 | in place, contents untouched | `setStructure` only |
| Double → Contiguous | `convertDoubleToContiguous`, :1381-1406 | **in place** | every slot rewritten from raw double to boxed double / empty, `storeStoreFence`, `setStructure` |
| **Contiguous → Double** (new with AOT) | `convertStorageOfNumbers`, `aot/runtime/AOTArrayOperations.cpp:193-226`, reached from IR `ConvertToDoubleStorage` (`AOTElementAccessEmitterStores.cpp:413-471`) | **new butterfly** | fill new butterfly, `nukeStructureAndSetButterfly`, `setStructure(originalArrayStructure(ArrayWithDouble))` |
| any → ArrayStorage | `convert*ToArrayStorage`, :1350-1374 … | new butterfly | fill, nuke + set butterfly, `setStructure` |
| ArrayStorage → Contiguous (new) | `tryConvertArrayStorageToContiguous`, :3622-3662 | new butterfly | under `cellLock()`: nuke + set butterfly, `setStructure` |
| CopyOnWrite → writable | `convertFromCopyOnWrite`, :1635-1672 | new butterfly (copy) | memcpy, fence, nuke + set butterfly, `setStructure` |
| writable → CopyOnWrite frozen (new) | `tryFreezeArrayInPlace` (2.2) | new `JSCellButterfly` | nuke + set butterfly, `setStructure` |

`JSCell::setStructure` itself is four separate writes (`runtime/JSCellInlines.h:259-284`): `m_structureID` (32 bit), `m_flags` (byte 6, carries IsPristine), `m_type` (byte 5), then a CAS loop on byte 4
(indexing type, preserving lock bits). So for a short time a cell shows the new StructureID with the old indexing byte and the old pristine bit.

`nukeStructureAndSetButterfly` does the nuke and the fences **only if `isX86() || vm.heap.mutatorShouldBeFenced()`** (`runtime/JSObjectInlines.h:122-133`); on ARM64 outside a concurrent marking phase it is a bare pointer store.

What a second mutator can observe (type confusion table):

| Observer did | Converter did meanwhile | Observer gets |
| --- | --- | --- |
| read byte 4 = Int32 (or holds an `ArrayStorage=Int32` proof), then loads `b[i]` as a JSValue | Int32 → Double in place | **raw double bits taken as a JSValue**. A small double (denormal, e.g. 5e-324·k) has zero top 16 bits = "cell pointer". Forged pointer → memory unsafe |
| read byte 4 = Double, then `storeDouble(b[i])` | Double → Contiguous in place | **raw double bits stored into a Contiguous array**; every later reader (and the collector's `appendValuesHidden`) takes them for a JSValue → memory unsafe |
| read byte 4 = Double, then `loadDouble(b[i])` | Double → Contiguous | boxed bits read as a double: wrong number, or a NaN pattern. The stubs treat NaN as a hole. A read that `readsHoleAsNaN` or is `Dense` passes an *impure* NaN on; whether every later boxing purifies it was not checked (13) |
| dense Int32 proof, `store32` of the low half | Int32 → Contiguous, then another thread stores a cell pointer in that slot | **low half of a pointer replaced** → wild pointer, memory unsafe |
| read byte 4 = Contiguous, stores a cell | Contiguous → Double (new butterfly) | store lands in the *old* butterfly: lost update only (old butterfly is still a valid allocation of JSValues) |
| read byte 4 = X, loaded old `b` | any conversion with a new butterfly | consistent old view (kind and contents of the old butterfly agree) as long as the observer read byte 4 *before* the converter's `setStructure` and `b` *before* the pointer store. The opposite interleaving (new `b`, old kind) is possible: byte 4 is written after the butterfly. E.g. Contiguous → ArrayStorage: reader with kind Contiguous and the **new ArrayStorage butterfly**: `b-8` is ArrayStorage's `publicLength`, `b+8i` reads `ArrayStorage` header fields (`m_sparseMap`, `m_indexBias`, `m_numValuesInVector`) as JSValues → memory unsafe |

The readers in 2.4 load byte 4 **before** the butterfly pointer (stub :54 then :56; inline :589 then :590, but B3 may reorder independent loads, and ARM64 may too).

### 2.6 Growth: who copies, when is the pointer published, what happens to the old storage

**Stub `growVector`** (`aot/stubs/AOTStubHelpersStorage.cpp:202-242`), used by `ArrayPush`, `AppendElements`, `PutByVal*`:
1. bail to C++ if `length > maxLengthForElementwiseCopy (64)`; if the array needs a barrier ("Only the array leads to the new vector, so an array that the collector will not visit again is left to C++", :211-212); unless the StructureID is one of the three original array Structures of the Instance (:153-161: "The butterfly of such an array holds the indexing header and the vector and nothing else, and the vector is the array's own").
2. allocate from `instance.allocation.auxiliarySpace`'s allocator for `nextLength` (inline free-list pop, `allocateCell`).
3. write `publicLength`, `vectorLength` of the new storage (:224-225); copy elements from the last to the first with plain 64-bit transfers (:227-232); fill the rest with the hole of the kind, **re-reading byte 4 for the kind** (:190-200, :235-240).
4. `storePtr(newStorage, a+8)` (:241). **No fence in front of the publication** (x86: not needed; ARM64: a second mutator, or a concurrent marker, could see the pointer before the contents. The barrier test in step 1 is what keeps the marker out, nothing keeps a mutator out).
5. The old storage is simply dropped: it is GC memory (auxiliary space or the tail of the array's own cell) and dies at a later collection.

**C++** `AOT::growFullVector` (`aot/runtime/AOTArrayOperations.cpp:313-343`) and `JSObject::ensureLengthSlow` (`runtime/JSObject.cpp:4127-4193`):
* room left in the size class: fill holes, `storeStoreFence`, `setVectorLength` **in place** (vector length of a live butterfly grows; never shrinks in place).
* else `Butterfly::reallocArrayRightIfPossible` (`runtime/ButterflyInlines.h:224-254`):
  * normal case: allocate, `memcpy`, fill holes, `setVectorLength`, `storeStoreFence`, publish (`JSObject.cpp:4183-4186`; the AOT copy uses `setButterfly`, which fences only on x86 or when fenced: `JSObjectInlines.h:110-120`). Old butterfly left to the GC.
  * **large arrays**: `canRealloc = !propertyCapacity && !vm.heap.mutatorShouldBeFenced() && isPreciseAllocation()` → `reallocatePreciseAllocationNonVirtual` (:237-246): "We can eagerly destroy butterfly backed by PreciseAllocation if (1) concurrent collector is not active and (2) the butterfly does not contain any property storage." **The old butterfly's memory is `realloc`ed, i.e. possibly freed at once.** A second mutator that has loaded the old pointer reads or writes freed memory.
* `tryGrowAndShiftButterflyRight` (`runtime/JSArray.cpp:50-…`, unshift) has the same `canReallocInPlace` test.

Lost updates: a store or push by thread B into the old butterfly after thread A copied that slot is lost; two pushes race on `publicLength` (both read `len`, both store at `len`, both write `len+1`): one element lost, no memory problem. Two growers: both publish, one wins, the loser's pushes are lost.

### 2.7 Moving the start of the elements (new with AOT; replaces ArrayStorage for queues)

`runtime/JSObjectElementsStartOffset.cpp`. A Structure bit `hasElementsStartOffset` (bit 31, `runtime/Structure.h:932`; `TransitionKind::AllocateElementsStartOffset`) says that a 64-bit count of free slots lies in front of the indexing header
(`IndexingHeader::elementsStartOffset`, `runtime/IndexingHeader.h:117-120`; `Butterfly::preCapacity`, `ButterflyInlines.h:46-55`).
`tryMoveElementsStartForward(count)` (`:73-95`), used by `shift()`/`splice(0,n)` on arrays longer than `shiftThreshold` (`JSArray.cpp:1615-1621, 1649-1655, 1890-1898, 1935-1943`):

```
Locker locker { cellLock() };
newButterfly = oldBase + count slots                      // same allocation
newButterfly->setPublicLength(publicLength - count);      // these three words overwrite what were
newButterfly->setVectorLength(vectorLength - count);      // elements [count-1] and [count-2]
newButterfly->indexingHeader()->elementsStartOffset(0) = numberOfFreeSlots + count;   // of the old view
WTF::storeStoreFence();
butterflyRef().set(vm, this, newButterfly);
```

`tryMoveElementsStartBackward` (:97-131) is the mirror image (writes a header and holes in front). The cell lock is for the collector. A second mutator that still holds the **old** butterfly pointer and reads `old[count-1]`
gets `publicLength | vectorLength << 32` as a JSValue (with a vector shorter than 65536 that has zero tag bits: a "cell pointer"), and `old[count-2]` is the free-slot count → **type confusion / wild pointer**. Its bounds check against `old[-1]` (the old header, which is still intact unless a later move overwrites it with holes/garbage) passes.
Stub growth is kept away from such arrays because their StructureID is not an original one (2.6 step 1). Inline element reads/writes are *not* kept away: shape bits are unchanged.

### 2.8 Copy-on-write arrays

* Array literals of constants: stub `ArrayOfData` (`aot/stubs/AOTStubsArrayLiterals.cpp:261-302`) allocates only the 16-byte `JSArray` cell and stores header + storage pointer with one `storePair64` (:290). For Int32/Double/Values the storage is
  **read-only memory of the image** ("Copy-on-write storage without references may lie in read-only memory of the process. No cell stands in front of it, and the collector has nothing to do with it", `runtime/JSCellButterfly.h:168-171`, `isOutsideHeap`; process globals `s_startOfStorageOutsideHeap/s_sizeOfStorageOutsideHeap` set once, `JSCellButterfly.cpp:66-75`). For `SharedValues` the storage is a `JSCellButterfly` made once per site and kept in a word of the Instance's statics (the stub goes slow while that word is 0, :275-277).
* Writers: every inline/stub writer tests the CopyOnWrite bit (`IndexingShapeAndWritabilityMask` or whole-byte compare) and leaves for C++, which calls `convertFromCopyOnWrite` (2.5). "The guard accepts copy-on-write storage, which only the operation may replace" (`AOTElementAccessEmitterStores.cpp:262`).
* For a second mutator: reader with old (shared, immutable) butterfly sees the old contents: harmless. Two threads both converting: both copy, both publish; one copy and the writes that went into it are lost.

### 2.9 Per-site and per-Instance array heuristics written at run time

* `ArrayAllocationProfile` (8 bytes per allocation site in the Instance: count, capacity, shape, largest lengths; `aot/runtime/AOTArrayAllocationProfile.h:25-50`), bumped by the stub `NewEmptyArrayAtSite` (`reads(AllocatorState, RuntimeState) | writes(…)`, `aot/stubs/AOTStubsArrayFamilies.def`), reviewed by `reviewArrayAllocationProfile`; next to it a word with the **last array allocated at the site** (`ArrayWords::distanceFromProfileToLastArray`, `sizeOfLastArrayWord`).
* `ArrayWords` in the Instance (`aot/stubs/AOTStubsArrayWords.h:14-27`): `elementsLookedAtInVain`, `placeThatConverts`, `shortYoungArraysConverted[64]`; `placeThatConverts` is a **parameter passed through memory** from inline code (`AOTElementAccessEmitterStores.cpp:461`) to `convertStorageOfNumbers` (`AOTArrayOperations.cpp:199-200`).
  Races: heuristics only (wrong counter credited).

### 2.10 `havingABadTime`

Stubs test it as "the Instance's header word for new Contiguous arrays is zero" (`branchIfHavingABadTime`, `aot/stubs/AOTStubsArrayLiterals.cpp:57-60`; also `offsetOfNewCopyOnWriteArrayHeader`: "A realm whose arrays are not pristine has no header for such a cell", :287-289). In the main realm indexed accessors on `Array.prototype`/`Object.prototype` are refused (immutable indexed properties), so it can only happen in `node:vm`/ShadowRealm realms. `JSGlobalObject::haveABadTime` walks the heap and converts every array (section 10).

---------------------------------------------------------------------------------------------------------------------------------------

## 3. Ordinary objects

### 3.1 Layout and the collector's protocol

Inline slots behind the cell, out-of-line slots growing downward from the butterfly pointer (offset `firstOutOfLineOffset`). The number of valid out-of-line slots is **not** in the butterfly: it is `Structure::maxOffset()/outOfLineCapacity()`.
So (StructureID, butterfly) is a two-word state, and for dictionaries (StructureID, butterfly, `structure->m_maxOffset`) a three-word state.
The protocol that exists serves one reader, the concurrent marker (`runtime/JSObjectVisitInlines.h:126-200`):

```
mutator:    object->structure = nuke(object->structure); object->butterfly = newButterfly; [structure->m_offset = newMaxOffset;] object->structure = newStructure
collector:  ReadStructureEarly ReadButterfly ReadStructureLate   (and maxOffset early/late); on nuked or mismatch: didRace, revisit later
```
"with all fencing necessary to ensure that this executes as if under sequential consistency". The mutator side is `nukeStructureAndSetButterfly` (`runtime/JSObjectInlines.h:122-133`), active only on x86 or while `mutatorShouldBeFenced()`.
With `visitor.mutatorsAreStopped()` the collector reads plainly (:115-124).

### 3.2 Adding a property in C++ (`JSObject::putDirectInternal`, `runtime/JSObjectInlines.h:561-717`)

Non-dictionary, transition exists or is made:
1. `newStructure = addPropertyTransitionToExistingStructure(...)` (no lock on the mutator path, `StructureInlines.h:554-577`) or `addNewPropertyTransition` (3.3).
2. if `outOfLineCapacity` differs: `newButterfly = allocateMoreOutOfLineStorage` = `Butterfly::createOrGrowPropertyStorage` (allocate, `memcpy` old properties + indexing header + elements, zero the new slots; `ButterflyInlines.h:158-178`), then `nukeStructureAndSetButterfly(vm, structureID, newButterfly)` (:641).
3. `putDirectOffset(vm, offset, value)` (:650): value **before** the Structure.
4. `setStructureForAddedProperty` → `setStructure(vm, newStructure)` (:651, :512-520).
5. `didAddPropertyWithTransition` (:522-534): prototype objects invalidate chain caches (`vm.invalidateStructureChainIntegrity(Add, this, uid)`), compiled class objects tell `AOT::PropertyWalk::didAddPropertyToClosure`.
The old butterfly is left to the GC.

Note that growing the *property* storage of an object that also has *elements* copies the elements too: an array with named properties gets a new butterfly when its 5th, 9th … name is added.

### 3.3 Adding a property in a stub

`growOutOfLineStorage` (`aot/stubs/AOTStubHelpersStorage.cpp:105-139`), reached from the cached "add" answers of the property tables: allocate from the Instance's auxiliary allocator, copy/clear pairs, store the added value into the new storage, then always (no test of `mutatorShouldBeFenced`):
```
or32(nukedStructureIDBit, object+0); storeFence; storePtr(storage, object+8); storeFence; store32(newStructureID, object+0)      (:134-138)
```
It stores only the StructureID, not flags/type/indexing byte: "An inline cache replays a transition by storing the StructureID: the flags of the cell stay as they are" (`runtime/Structure.cpp:769`). That is why a transition that would change cell flags (lose Pristine) is "uncacheable": `isCacheableTransitionFrom` requires `hasSameCellFlagsAs` (`runtime/Structure.h` diff; `JSObject::setStructureForAddedPropertySlow`, `JSObject.cpp:4691-4696`).
It refuses objects with indexed storage (validation :34; the callers test the indexing byte).

### 3.4 Structure's own mutable state, and the locks that exist

`ConcurrentJSLock` is a real 1-byte `WTF::Lock` in every build (`runtime/ConcurrentJSLock.h:34-37`). `Structure::m_lock` (`runtime/Structure.h:1129`) exists **to protect against the compiler threads and the collector, not against a second mutator**: the mutator takes it when it *writes*, and reads without it.

| State | Written by | Lock taken by the writer | Mutator-side readers |
| --- | --- | --- | --- |
| `m_transitionTable` (single-slot or `HashMap` of weak Structures) | `add`/`addAlias` in `addNewPropertyTransition` etc. | `GCSafeConcurrentJSLocker(structure->m_lock)` (`Structure.cpp:882-897, 942-944, 1041, 1080, 1190, 1356, 1475, 1505, 2514`) | `addPropertyTransitionToExistingStructure` (`StructureInlines.h:573-577`), `nonPropertyTransition` (`Structure.cpp:1277`), `tryMarkPristine`'s `forEach` (:774): **no lock**. The `…Concurrently` variant locks (`StructureInlines.h:579-583`) |
| `m_propertyTableUnsafe` (a GC cell `PropertyTable` with a malloc'ed index vector) | materialised lazily (`materializePropertyTable`, `Structure.cpp:545-…`, takes the lock of the Structure it finds a table in and its own at :569); **stolen** by the next transition: `takePropertyTableOrCloneIfPinned` (:1257-1270) sets the old Structure's pointer to null under `m_lock` and hands the same table to the new Structure, which then adds to it; cleared by the collector ("During GC, it may be set to 0 by another thread", `Structure.h:1158`) | writer holds `m_lock` | `Structure::get(vm, name)` → `ensurePropertyTableIfNotEmpty` (`Structure.h:1036-1043`) → `table->get(uid)`: **no lock**. Stubs probe it too (3.6) |
| `PropertyTable` contents (`m_indexVector`, `m_indexSize`, `m_indexMask`, `m_keyCount`, `m_deletedCount`; `runtime/PropertyTable.h:292-296`) | `add`/`remove`/`rehash` (:548-582: new vector allocated, entries reinserted, **`destroyIndexVector(oldIndexVector)` frees the old one at once**) | the owning Structure's `m_lock` (`Structure::add` `StructureInlines.h:~426`, `remove`, `attributeChange`) | none |
| `m_maxOffset`, `m_transitionOffset`, `m_bitField`, `m_blob`, `m_outOfLineTypeFlags`, `m_seenProperties`, `m_propertyHash` | dictionary adds/removes, `setContainsReadOnlyProperties`, `setPristineBits`, `didTransition…`, `setHasBeenFlattenedBefore` … (non-atomic read-modify-write of a 32-bit bit field) | some under `m_lock`, many not | plain loads, also from stubs (`Structure::bitFieldOffset()`, `seenPropertiesOffset()`) |
| `m_previousOrRareData` | `allocateRareData` (`Structure.cpp:1892-1899`: create, `storeStoreFence`, store); new: `dropTransitionChain`/`restoreTransitionChain` (`Structure.h:318-319`), `setPreviousID` under the target's lock (`Structure.cpp:890-893`) | none for `allocateRareData` (check-then-act `if (!hasRareData()) allocateRareData(vm)`, e.g. `StructureInlines.h` `cacheSpecialProperty`, `Structure.cpp:1909`) | plain |
| `StructureRareData` caches (enumerator, own keys, special properties, replacement watchpoint sets, `m_immutablePropertyNames`) | lazily | `m_replacementWatchpointSets` under `m_lock` (`Structure.cpp:1911`, `StructureInlines.h:211`); others none | section 10 |
| `m_transitionWatchpointSet` and other `InlineWatchpointSet`s | `didTransitionFromThisStructure`, `fireStructureTransitionWatchpoint` | none (thin/fat encoded word; inflate is check-then-act) | JIT is off; AOT has its own watchers (`aot/runtime/AOTWatchpoints.*`, R-caches) |
| `m_cachedPrototypeChain` | `prototypeChain()` lazily | none | plain |
| `StructureCache` (per realm, prototype → Structure for `Object.create`, `createThis`) | `emptyObjectStructureForPrototype` … | **has a `Lock`** (`runtime/StructureCache.h:60-68`, `StructureCache.cpp:36, 78, 89`) | same lock |
| `vm.propertyRuns` (`PropertyRunTable`, `PropertyAdditionCache`) (new) | `intern`, `add`, pruned at GC end | none | `findCachedPropertyAddition` (`Structure.cpp:836-851`) |

Check-then-act in `addPropertyTransition` (`Structure.cpp:853-860`): look up, miss, create, insert. Two mutators adding the same name to objects of the same Structure both miss and both insert. `StructureTransitionTable::add` with an existing key replaces/asserts; the two objects end up with *different* Structures for the same shape: legal for JSC's semantics (slower), but `ASSERT(!addPropertyTransitionToExistingStructure(...))` (:866) shows the assumption. The table-stealing step is the dangerous one: thread A steals S's table for S→S1 and adds `x` to it while thread B, adding `y` to another object of S, has already fetched the same table pointer (`propertyTableOrNull()` at :1260 is read *before* the lock at :1264) → both Structures own and mutate one table → corrupt offsets → out-of-bounds property slots (memory unsafe).

`cellLock()` (2 bits in byte 4 of any cell but an Environment): taken by the mutator where the *shape of the butterfly changes without a Structure change that the nuke protocol covers*: `flattenDictionaryStructure` when it shrinks (`Structure.cpp:1818-1820`), ArrayStorage shift/unshift, the new element-start moves (2.7), `tryConvertArrayStorageToContiguous`, `CompactTable::becomeObsolete/removeAllEntries` and `visitFields` (section 5), `StringBuilderStorage::reset`, `DirectArguments`/`JSSegmentedVariableObject`/`SparseArrayValueMap`/`JSFunction` rare data etc. The partner is always a collector or compiler thread.

### 3.5 Dictionary mode

* Entering: `toCacheableDictionaryTransition` / `toUncacheableDictionaryTransition` make a fresh Structure with a *pinned* copy of the table (`Structure.cpp:1198-1224`, pin at :1207). From then on add / remove / attribute change **mutate that Structure in place**: `addOrReplacePropertyWithoutTransition`, called from the dictionary branch of `putDirectInternal` (`JSObjectInlines.h:577-599`): inside the callback, under `m_lock`: if capacity grows: allocate butterfly, `nukeStructureAndSetButterfly`, `structure->setMaxOffset`, fence, `setStructureIDDirectly(structureID)`; then, outside the lock, `putDirectOffset`.
* A reader (C++ `getOwnPropertySlot`, or the stub in 3.6) does: load StructureID, look the name up in the table → offset, load butterfly, load slot. With a concurrent `delete` + `add`, uncacheable dictionaries **reuse offsets** (`PropertyTable` keeps deleted offsets), so a stale offset reads another property's value: wrong value only, *if* the offset is within the capacity of the butterfly that the reader loads. It need not be: in `Structure::addOrReplacePropertyWithoutTransition` (`runtime/StructureInlines.h:415-448`) the order under `m_lock` is `table->addAfterFind(...)` **first** (:437), then `func(locker, newOffset, newMaxOffset)` (:443), and it is `func` that allocates and publishes the larger butterfly. A lock-free reader can therefore find the new entry, take its out-of-line offset, and load the **old, smaller butterfly** → read (or, in `replaceOwnValueAndReturn`, write) in front of the allocation → memory unsafe. The first `table->find` of the adder itself (:422) also runs before the lock is taken.
* `flattenDictionaryStructure` (`Structure.cpp:1784-1881`): under `m_lock` (+ `cellLock` when shrinking): nuke the object's StructureID, **renumber all offsets and move the values**, zero the rest, maybe shrink or drop the butterfly (`shiftButterflyAfterFlattening`, `setButterfly(nullptr)`), restore the StructureID. Any concurrent reader with a cached offset reads a different property, or, after `setButterfly(nullptr)`, dereferences null − offset.
  Who flattens: prototype chain caching (`JSObject::flattenDictionaryObject`), `for-in` enumerator creation, AOT property cache filing (not re-read here).

### 3.6 Machine code that reads `Structure` and `PropertyTable` (x86-64 only, `#if CPU(X86_64)`)

`findInPropertyTable` (`aot/stubs/AOTStubsPropertyRead.cpp:116-161`):
1. `seen = load64(structure + seenPropertiesOffset)`; `(seen & uid) != uid` → absent (a bloom filter kept in the Structure) (:125-127)
2. `table = load64(structure + propertyTableUnsafeOffset)`; null → unknown (:129-130)
3. `vector = load64(table + offsetOfIndexVector)`; not compact → unknown (:131-132)
4. `size = load32(table + offsetOfIndexSize)` (:133)
5. up to 4 probes: `entryIndex = load8(vector + (hash & (size-1)))`, 0 → absent; `entry = load64(vector + size + 8*(entryIndex-1))`; compare name in the low 48 bits (:143-155)
6. `answerByPropertyTableAndReturn` (:164-198): offset from the entry; inline slot `load64(object + inlineStorage + 8*offset)` or `b = load64(object+8); load64(b - …)`.
`answerFromObjectsAndReturn` (:372-461) repeats that up the prototype chain via `Structure::prototypeOffset()`, tests `s_staticPropertiesReifiedBits`, handles compiled objects (field records, extra-properties object) and class prototypes.
`replaceOwnValueAndReturn` (:463-510) does the same search and then **stores** the value at the found offset if the Structure's bit field has none of `hasImmutableProperties | hasListedImmutableProperties | isWatchingReplacement`.
`countMissOfPair` (:512-527) bumps an 8-bit counter in `instance.propertyCaches.missesOfPairs` and sets `missIsDueForFiling`.

Vector and size are two loads from a table that `rehash` replaces non-atomically and frees eagerly: with a second mutator this is use-after-free and out-of-bounds (size from the new table, vector from the old).

### 3.7 Static (lazy) properties

`HasStaticPropertyTable` objects put a property into the property table at its first read (`Lookup.cpp`, `reifyStaticProperty`), i.e. **a read mutates the object** (Structure transition or dictionary add). In an AOT realm that runs under `vm.allowLazyMaterializationOfImmutablePropertiesCount` (a per-VM counter used as a dynamic scope: `JSObjectInlines.h:386, 573-576`; `FixedNamesOfBuiltin::willWrite`, `aot/builtins/AOTBuiltinTableRealm.cpp:872-875`). While one thread is inside such a scope, *every* thread's writes to immutable properties are let through.

---------------------------------------------------------------------------------------------------------------------------------------

## 4. What is different in Structure / JSObject / JSCell with AOT

1. **Immutable properties by listed names.** The fork's `main` already had `hasImmutableProperties` (all properties; bit 10, `Structure.h:915`). New: `hasListedImmutableProperties` (bit 11, :916) + `ImmutablePropertyNames` (`runtime/ImmutablePropertyNames.h:17-84`), a ref-counted, **immutable after construction** (`const` members :80-83) set: explicit names, `Part::{NumericKeys, Prototype, OtherStrings}`, open strings, and `OutsideNames` (virtual; AOT's `FixedNamesOfBuiltin` answers from the read-only built-ins table, `aot/builtins/AOTBuiltinTableRealm.cpp:861-925`). It hangs off `StructureRareData`'s special property cache (`StructureRareData::setImmutablePropertyNames`).
   Every writer asks `structure->isImmutableProperty(uid)` (`Structure.h:413-418`): `putDirectInternal` (`JSObjectInlines.h:573`), `putDirectWithoutTransition` (:386), `JSArray::put/defineOwnProperty/setLength/pop`, delete, defineOwnProperty, setPrototype (`hasImmutablePrototype`, :432). "Where the answer is no, the owner of the listed names is told of the write" (`willWrite`, → `WritesToBuiltinMembers`/`BuiltinLocationWatch`, a byte per watched member in the Instance: `WatchedLocationState { NotArmed, Unchanged, Changed }`, `aot/runtime/AOTBuiltinLocationWatch.h:36-38`).
   Applied at realm start-up and when a lazy class is created (`aot/runtime/AOTRealmStartup.cpp:142, 329, 432`; `didCreateClass(…LazyClassStructure…)` :480). `refusalAlwaysThrows()` makes the refusal a TypeError in sloppy code.
   Classes with write hooks of their own must declare `WriteHooksHonorImmutableProperties` (`JSTypeInfo.h:64`).
2. **Pristine bits.** Three places: out-of-line flag `HasPristineNames` (`JSTypeInfo.h:67`), Structure bit `hasPristineElements` (bit 12, `Structure.h:917`), inline flag `IsPristine` (copied to cells). Set on existing Structures *in place* by `Structure::tryMarkPristine` (`Structure.cpp:745-797`): walks the transition tree from the Structure, computes the bits that every reachable cacheable transition keeps, `vm.setMayHaveUncacheableTransitions()`, then `setPristineBits` on all of them (writes `m_blob`, `m_outOfLineTypeFlags`, bit field). Called at realm start-up (`AOTRealmStartup.cpp:459-463`) **and lazily at run time** (`aot/builtins/AOTBuiltinOperationSupport.cpp:56`, `aot/runtime/AOTTypedArrayOperations.cpp:42`). Cells allocated *before* the mark keep a clear bit (conservative). Inherited/cleared per transition: `pristineBitsAfterTransition` (:706-743): lost on `ChangePrototype`, `AddIndexedAccessors`, SlowPut, and on adding a name that "mayShadowPrototypeChain" (symbols, `then`, `toJSON`, any name found on the locked chain; :680-696) or a canonical numeric string (elements aspect).
   The bit of a *cell* is cleared by `setStructure` (`m_flags = mergeInlineTypeFlags(...)`, `JSCellInlines.h:271`), i.e. **after** the StructureID store.
3. **`StructureVariant::Compiled` / `isCompiledStructure()`** (`Structure.h:940-945`): `AOT::CompiledStructure` describes compiled objects and environments. Generic C++ entry points branch on it: `JSObject::getDirect` (`JSObject.h:394-397, 406-409`), `putDirectInternal` (`JSObjectInlines.h:683-686`: `vm.hasCompiledCode() && (isCompiledStructure() || type() == SlimClosureType)` → `putDirectToCompiledObject`), private fields/brands (:958-1160), `didBecomePrototype`, `StructureRareData::cacheSpecialProperty`.
4. **New TransitionKinds** 19-22: `MakeListedPropertiesImmutable`, `FreezeElementsInPlace`, `PropertyRunAddition`, `AllocateElementsStartOffset` (`runtime/StructureTransitionTable.h`), `addAlias` (one transition-table key leading to a Structure whose own transition is another).
5. **Dropping transition chains**: `dropTransitionChain`/`restoreTransitionChain`/`isPinnedPropertyAdditionFrom` (`Structure.h:318-327`, `Structure.cpp:799-826`): a Structure at the end of an unshared chain of ≥ 4 additions can pin its table and forget `previousID`; `tryPutDirectByExistingTransition` restores it (`JSObjectInlines.h:536-545`). More in-place mutation of shared Structures.
6. **Collector fields in Structure**: `m_markingStamp`, `m_cellVisitKind` (`Structure.h` diff) for the inline visits of `SlotVisitor::drainStack` (R-heap).
7. **`JSObject::indexingShouldBeSparse`**, `ensureArrayStorage` refusal for immutable indexed properties (`JSArray.cpp:208-213`), `putDirectBuiltinFunction(…, BuiltinCode, …)` (built-in JS functions may be compiled code), `JSC_REGISTERED_HOST_FUNCTION`.
8. `VM::m_mayHaveUncacheableTransitions` (`VM.h:522-524`): a sticky per-VM bool, set once.

---------------------------------------------------------------------------------------------------------------------------------------

## 5. Map, Set, WeakMap, WeakSet

### 5.1 Layout

`JSMap`/`JSSet` = `JSOrderedHashTable<Traits>`: one field `WriteBarrier<JSCell> m_storage` (was `WriteBarrier<Storage>`; `runtime/JSOrderedHashTable.h` diff), null until the first insertion. `Helper::isCompact(storage)` = `storage.type() != JSCellButterflyType` (`runtime/JSOrderedHashTableHelper.h:653`). With `vm.hasCompiledCode()` the first `add` makes a CompactTable (`JSOrderedHashTable.h:109-114, 188-194`), so upstream's `JSCellButterfly`-based table is never created there; every method branches (`:66, 96, 121, 142, 160, 172, 258, 291`).

`CompactTable` (`aot/runtime/AOTCompactTable.h:492-501`):
```
 0 JSCell   8 m_used(32)  12 m_alive(32)  16 m_capacity(32)  20 m_indexBits(8)  21 m_keysShiftAndFlags(8: keysAreWide 0x80, keysAreInImage 0x40, wasWalked 0x20, shift)  22 m_valueWidth(8)  23 m_status(8)
24 index:  capacity<=8: 8 tag bytes, entry number = byte position;  <=254: uint16 slots (8-bit tag | 8-bit entry);  larger: uint32 slots; free = all ones
   keys:   m_capacity × 4 bytes (narrow: caged 32-bit cell reference, or undefined/null/true/false; 0 = deleted) or × 8 bytes
   values: m_capacity × 0 / 4 / 8 bytes
```
Entries are appended in insertion order; deletion empties key and value in place and leaves the index slot; nothing is ever moved inside a live table.
An obsolete table is **re-used in place as a forwarding record** (`becomeObsolete`, `AOTCompactTable.cpp:392-412`): under `cellLock()`: the list of deleted entry numbers is written over the index area, `m_status = Replaced|Cleared`, the 8 bytes at `m_used/m_alive` become the **successor pointer**, `m_capacity` becomes the number of deleted entries.

### 5.2 Fast paths

Inline code only normalises the key, computes nothing else, and calls stubs (`aot/builtins/AOTHashTableBuiltinEmitter.cpp:128-165`); `size` is inline: `load m_storage; null → 0; load32 m_alive` (:118-126). WeakMap/WeakSet lookups may be fully inline (`probeWeakTable`, `aot/builtins/AOTMapSetBuiltinEmitter.cpp:80-133`). Iteration `MapNext`/`SetNext` is inline + front-end stub.

**get / has** (`MapGet`, `MapHas`, `SetHas`; `aot/stubs/AOTStubsHashTables.cpp:465-575`, `AOTStubsCollections.cpp:268-366`):
1. string key: `impl = load64(key+8)`; rope → C++; `hash = load32(impl+flags) >> 8`; 0 → leaf call `operationAOTHashStringImpl` (writes the hash into the StringImpl) (`AOTStubsHashTables.cpp:521-540`). Other keys: `rapidHashMix64(bits) & hashMask` (:571-572); doubles and BigInts → C++.
2. `table = load64(owner + m_storage)`; null → absent (:505-510)
3. test `m_keysShiftAndFlags` for in-image/wide (:471)
4. probe the index with `m_indexBits`, `m_used`, compare `keys[entry]` (identity, then for strings impl identity, hash, characters)
5. value: `values = keys + capacity*width` (`emitLoadValuesOfCompactTable`), load, decode narrow → `cageBase + word`.
The stub **never looks at `m_status`**: the owner's `m_storage` is by construction a live table.

**set / add** (`MapSet`, `SetAdd`; `emitAppendToCompactTable`, `AOTStubsCollections.cpp:210-247`): search as above with `findsInsertionPoint`; found → overwrite the value column in place (+ barrier). Absent:
`entry = load32(m_used)`; `entry >= m_capacity` → `GrowCompactTable`; width checks ("Nothing is stored before the last exit"); **store key; store index slot/tag; store value; `fenceAfterAllocation` (ARM64: `dmb ishst` only if `instance.mutatorShouldBeFenced`; x86: nothing); `m_used = entry+1`; `m_alive += 1` (read-modify-write in memory)**; barrier on the table ("A marker visits the entries below the count that it reads", :241). C++ twin: `append` / `tryAddQuickly` (`AOTCompactTable.cpp:310-321, 493-552`).

**delete** (`emitDelete`, :393-415): search; clear key, clear value, `m_alive -= 1`. The index keeps pointing at the dead entry.

**growth / rehash**: stub `GrowCompactTable` (:570-810) for small string-keyed tables without deletions, only while "No marker reads a table meanwhile" (`mutatorShouldBeFenced == 0`, :621-622): allocate the larger cell by a precomputed "growth plan", copy index/keys/values, append the new key, `larger.m_used = larger.m_alive = count+1`, then (:789-792) `old.m_status = Replaced` (store8), `old.{m_used,m_alive} = larger` (store64), `old.m_capacity = 0`, and **last** `owner.m_storage = larger`; no fence anywhere in that sequence.
C++ `CompactTable::add` (`AOTCompactTable.cpp:554-633`): `replacement = tryCopyWithWidths(...)` (compacts deleted entries, may widen columns), then `replaceWith`: **`table->becomeObsolete(vm, Replaced, replacement)` first, `setStorage(vm, owner, replacement)` second** (:570-574). So there is a window in which `owner.m_storage` still names a table whose header words already mean something else.
`clear` (:709-737): three cases: `removeAllEntries()` in place (under `cellLock`: `m_used = m_alive = 0`, index wiped) if nobody has walked the table; delete every entry in place; or fresh table + `becomeObsolete(Cleared, owner)` + `setStorage`.
`tryMoveToHeap` (`AOTCompactTableCreation.cpp:65-…`): first write to an in-image table copies it to a heap table.

**iteration**: an iterator (or an inlined `for…of`) holds `(table, entry)`. `CompactTable::next` (`AOTCompactTable.cpp:739-768`): Live: scan forward for a non-empty key, **set `wasWalked`** (non-atomic `|=` on byte 21, also in the stub: load8/or/store8, `AOTStubsCollections.cpp:886-890`); Replaced: subtract the deleted entries before `entry`, follow the successor; Cleared: restart at 0 in the owner's current table. `JSMapIterator::nextWithAdvanceInCompactStorage` (`runtime/JSMapIterator.h` diff) updates the iterator's internal fields `entry`, `storage`.
`CompactStorage::setIterationEntry/iterationEntry` keep "the iteration entry" in **one per-VM word** (`vm.aotState().runtimeTable->setCompactTableIterationEntry`, `aot/runtime/AOTCompactStorage.cpp:91-99`); upstream kept it inside the storage. It is the hand-over between `@mapIterationNext`-style intrinsics of interpreted/native callers.

Collector: `visitFields` (`AOTCompactTable.h:528-542`) takes `cellLock()`, reads status; live: `used = m_used; loadLoadFence; visitEntries(0, used)`.

### 5.3 WeakMap / WeakSet

Unchanged upstream `WeakMapImpl` (`runtime/WeakMapImpl.h:500-503`): `m_buffer` (**malloc'ed**, or an inline/empty buffer), `m_capacity`, `m_keyCount`, `m_deleteCount`. Stub/inline lookup (`emitLookupInWeakTable`, `AOTStubsCollections.cpp:285-327`): `buffer = load64(m_buffer)`, `mask = load32(m_capacity) - 1`, linear probe `load64(buffer + 16*(hash&mask))` until equal or 0, then the value. `set`/`delete` are C++.
`rehash` (`runtime/WeakMapImplInlines.h:130-172`): `makeAndSetNewBuffer(capacity)` stores `m_buffer` then `m_capacity` (`WeakMapImpl.h:470-486`), reinserts, **`WeakMapBufferType::destroy(oldBuffer)` frees at once**. It also runs at the end of a collection (`reconcileWeakReferencesAtGCEnd` → shrink).

### 5.4 What a second mutator could observe (summary; severity in section 11)

* Reader vs `append`: on x86 fine (entry becomes visible in the index before `m_used` grows, and tag/short probes compare `entry >= used`… only the Tag kind compares with `used`, `AOTCompactTable.h:328`; slot kinds accept any entry number the slot names). A reader can see the index slot before the key store on ARM64, or the key before the value: it then reads key 0 (= deleted, no match) or **value 0 = empty JSValue** returned from `map.get` → an empty value escapes into compiled code.
* Two appenders: both read `m_used = n`, both write entry n, both insert index slots → one key lost but its index slot stays, `m_alive` off by one (`m_alive` is what `size` returns). No out-of-bounds.
* Reader or writer vs `becomeObsolete`/`removeAllEntries`: header words change meaning under the reader: `m_used` = low half of a pointer, `m_capacity` = small count (moves `offsetOfValues()`), index = list of entry numbers. Entry numbers read from a clobbered index are bounded only by the slot format (8, 20, 28 bits), not by the capacity → **out-of-bounds read of keys/values, narrow words decoded into cell pointers** → memory unsafe. A *writer* that appends to an obsolete table overwrites the successor pointer (`m_used` store) → memory unsafe.
* WeakMap: buffer/capacity torn pair → out-of-bounds; freed buffer → use-after-free.

---------------------------------------------------------------------------------------------------------------------------------------

## 6. Strings

### 6.1 JSString and ropes

`JSString`: 16 bytes: cell header + `m_fiber` (`mutable uintptr_t`, `runtime/JSString.h:345`) = `StringImpl*`, or for a rope fiber 0 with flag bits (`isRopeInPointer`, `is8BitInPointer`, `isSubstringInPointer`). `JSRopeString` adds 16 bytes of `CompactFibers`: length (32), fiber1 (48), fiber2 (48) (`JSString.h:806`; a substring rope keeps its base in fiber1 and the offset in fiber2, :679-684).

**Resolution mutates the cell in place** (`JSRopeString::resolveRopeWithFunction`, `runtime/JSString.cpp:329-376`; `convertToNonRope`, `runtime/JSStringInlines.h:382-398`):
1. read `isSubstring()`, `is8Bit()` (both from `m_fiber`), `length()`; allocate a `StringImpl`; `resolveToBuffer(fiber0(), fiber1(), fiber2(), …)` (`JSString.cpp:245-249`; `fiber0()` **re-reads `m_fiber`**, `JSString.h:747-750`); sub-ropes are walked, not resolved.
2. `WTF::storeStoreFence()`
3. **one 64-bit store**: `new (&uninitializedValueInternal()) String(WTF::move(string))` over `m_fiber` ("JSString's String initialization must be done in one pointer move")
4. "We do not clear the trailing fibers and length information (fiber1 and fiber2) because we could be reading the length concurrently."
5. if the impl is an atom: fence, `markAsAtom()` = `m_flags |= TypeInfoPerCellBit` (plain read-modify-write of byte 6, `JSCellInlines.h:362-371`)
6. `notifyNeedsDestruction()` (sets a bit for the block in its directory; documented as lock-free safe: `heap/BlockDirectory.h:125, 256`)
7. `vm.heap.reportExtraMemoryAllocated`.

It **is** safe against the concurrent marker and compiler threads: they read `fiberConcurrently()` once and decide from that one word (`JSString::visitFields`, `runtime/JSStringVisitInlines.h`; old code in the diff of `JSString.cpp:107-140`); stale fiber1/fiber2 stay valid cell pointers because they are never cleared (they keep garbage alive until the string dies… they are not visited once the word is no rope).
It is **not** safe against a second resolver: step 1 re-reads `m_fiber` several times after the caller's `isRope()` test. If thread A finishes step 3 in between, thread B's `fiber0()` is `StringImpl* & stringMask` taken for a `JSString*`, `is8Bit()`/`isSubstring()` are bits of a heap address → type confusion. If both get through step 1, both store: the loser's `StringImpl` reference is overwritten without `deref` (leak; anyone who already read it keeps a valid pointer, because it is never freed).

New with the fork/AOT: `JSString::equalResolvingRopes` / `equalWithoutResolvingRopes` (`JSString.cpp:95-222`) compare ropes piecewise and then *share*: `convertToNonRope(String { *impl })` gives a rope the impl of the equal flat string (:115-121); `resolveSubstringBySharing` (`JSStringInlines.h:400-408`). So even `===` can mutate both operands.

**Atomisation swaps the impl of a flat string**: `JSString::swapToAtomString` (`JSString.h:927-947`): fence; `valueInternal().swap(target)` (one word); fence; `markAsAtom()`; `vm.heap.appendPossiblyAccessedStringFromConcurrentThreadsOrGCOwnedDataScope(this, WTF::move(target))`: the old impl is parked in a per-heap `SegmentedVector` (`heap/Heap.h:732, 1027`, no lock) until a GC finds the JSString on no stack (`Heap.cpp:2348`) or no JS runs (`Heap.cpp:1414-1433`). The comment added by this branch: "Code compiled ahead of time keeps the characters, length and width of a flat string it holds in a live JSString* across calls, and relies on the old StringImpl staying alive as described above."
The new impl can differ in width: "Atomizing the base can replace its StringImpl with one of the other width, so a substring's own 8-bit flag may be stale" (`aot/codegen/AOTStringEmitter.cpp:197`).

### 6.2 How compiled code reads a string

`StringEmitter::viewOfString` (`aot/codegen/AOTStringEmitter.cpp:202-218`): `fiber = load64(s+8)`; flat: `characters = load64(impl+8)`, `length = load32(impl+4)`, `flags = load32(impl+16)`: all from the **one impl pointer** loaded, hence consistent. Rope: `length = load32(s+16)`; stub `RopeCharacters` (resolves or, for a substring, computes base characters + offset), **then a second, separate load of the flat cell's impl for the flags** (:190-199).
`FlatString` is an opaque, function-local IR type (`AOTSchema.def:96`): once a string is "opened", `(characters, length, is8Bit)` live in registers/spill slots across calls (`m_openedStrings`, :260). "The value word is not immutable here: the cell may have been a rope before the caller's test, and B3 hoists an immutable load above that test" (:176). Character loads are marked immutable (:271-272).
IR effects: `ResolvesRopes = reads(StringStorage) writes(StringStorage, RuntimeState) throws collects` (`AOTSchema.def:87`).

### 6.3 Concatenation, substring, conversions (stubs)

* `Concatenate(2|3)` (`aot/stubs/AOTStubsStringConcatenation.cpp`, `makeRopeOfTwoValuesAndReturn`): allocates a `JSRopeString` from the Instance's allocator and fills it: touches only the new cell and reads lengths/flags of the operands (from one load of each operand's value word). Empty operands return the other. Overflow and non-strings → C++.
* `Substring` (`AOTStubsStringSubstrings.cpp`): single characters from `instance.singleCharacterStrings` (= `vm.smallStrings.singleCharacterStrings()`, filled at VM creation; `aot/runtime/AOTInstance.cpp:127`), else a substring rope over a flat base (`makeSubstringOfFlatString`); a rope base is flattened first (`flattenSubject` → `operationAOTFlatten`).
* `IntegerToString` and primitive-to-string (`AOTStubsStringConversions.cpp:51-97`, `AOTStubsPrimitiveFastPaths.cpp:80-110`) **read `vm.numericStrings` from machine code**: small ints: `load64(smallIntStrings + 16*n + offsetOfJSString)`; others: `entry = intStrings + 24*(hash(n) & (NumericStrings::cacheSize - 1))` (cacheSize = 1024, `runtime/NumericStrings.h:41`); `load32(entry.key) == n`, then `load64(entry.jsString)`; null → C++ (`operationAOT…`, which fills it: `vm.numericStrings.setJSString/addJSString`, `aot/runtime/AOTStringOperations.cpp:144-178`). Key and JSString are two words written separately.
* Literal comparisons, `LiteralString` representation: a value of a union of literal strings travels as a small "literal string word"; `toLiteralString` probes "the literal string cache of the instance" (`findLiteralStringWord`, `AOTStubsStrings.h:109-110`; `ProgramStrings::m_literalStringCacheKeys`, an array of `RefPtr<StringImpl>`, `aot/runtime/AOTProgramStrings.h:90`), filled by `literalStringWordFor`/`findLiteralString` at run time.

### 6.4 StringImpl

* **Refcount**: `std::atomic<uint32_t> m_refCount` (`WTF/text/StringImpl.h:162`); `ref()` = relaxed `fetch_add`, skipped for static strings ("A static string may be in memory that nobody can write", :1207-1211); `deref()` (:1214-1229): relaxed load; static → return; "When this is the only reference nobody else can be racing to add one, so skip the atomic read-modify-write"; else relaxed `fetch_sub`; last → `StringImpl::destroy`. Atomic, but **relaxed on the release path** (no release/acquire pair between the last use by one thread and the free by another).
* **Hash and flags**: `mutable unsigned m_hashAndFlags` (:172), plain. `setHash` (:1184-…) is `m_hashAndFlags |= hash << 8`; `setIsAtom` (:1174-1181) is `|=`/`&= ~`; also `s_hashFlagDidReportCost`. Two threads hashing the same string write the same bits: benign. **`setHash` racing with `setIsAtom`** is a lost update: the atom bit can vanish (string is in the table but `isAtom()` is false → on destruction it is not removed → dangling table entry) or reappear after removal.
* **Static strings in the image**: identifiers and literals are `StaticStringImpl`-shaped records in read-only memory (`hashAndFlagsOfStaticString`, `refCountOfStaticString`, `StringImpl.h:372-378`): hash precomputed, refcount never written. With a position-independent image there are "lazy string headers" in the Instance instead (`lazyStrings.headers`, `LazyStringHeader`, `aot/stubs/AOTStubsPropertyRead.cpp:212-214, 385`; not studied).
  Headers made at run time over characters of the image: `ProgramStrings::tryCreateHeader` (`aot/runtime/AOTProgramStrings.cpp:140-150`): **bump pointer `constants.nextStringHeader` in the Instance, no atomics**.
* Substring sharing: `StringImpl::createSubstringSharingImpl` refs the base impl (atomic). `JSString`s share impls freely.

### 6.5 Atom tables

* WTF: one `AtomStringTable` per `Thread` (`WTF/Threading.h:264-273, 441-442`; `Threading.cpp:270`). `AtomStringImpl::add/lookUp/remove` use `Thread::currentSingleton().atomStringTable()` (`WTF/text/AtomStringImpl.cpp:75-78`). `AtomStringTableLocker` is an **empty class** unless `USE(WEB_THREAD)` (:49-72).
* JSC: a VM has `m_atomStringTable` (`runtime/VM.h:675, 711`): for `VMType::Default` the creating thread's own table, else a new one (`VM.cpp:279`). `JSLock::didAcquireLock` points the current thread at the VM's table (`runtime/JSLock.cpp:125-127`) and `willReleaseLock` restores (`:328-330`). So "one thread at a time in a VM" is exactly what makes the table safe. **With several threads in one VM all of them would point at one unlocked `HashSet`.**
* `AtomStringImpl::remove` runs from `StringImpl::destroy` on whatever thread drops the last reference and looks in *that thread's current table*: `RELEASE_ASSERT(wasRemoved, "The string being removed is an atom in the string table of an other thread!")` (`AtomStringImpl.cpp:561-569`). Collector/sweeper threads that destroy `JSString`s are a known special case upstream (sweeping happens on the mutator or with the VM's table installed; not re-read).
* **Image atoms** (diff): "Static atoms in read-only memory are in no table, and every thread has the same ones. A table that has no atom with the characters asks this function for one before it makes its own" (`WTF/text/AtomStringTable.h` diff). `AtomStringTable::s_readOnlyAtomLookup` is a process-global `std::atomic` function pointer, set once by `ProgramIdentifiers` (`aot/runtime/AOTProgramIdentifiers.cpp:77`) to `ProgramIdentifiers::findAtom(StringView, hash)`, a pure lookup in the image. Every `add` path first does `table.find`, then `lookUp(text, hash)`, and only then `table.add` (`findAtomBeforeAdding`, `AtomStringImpl.cpp:91-108, 113-118` and the four `addSlowCase`s). `setReadOnlyAtomLookup` de-atomises earlier duplicates **in the calling thread's table only** (`AtomStringTable.cpp` diff).
  Consequence: for every name the build saw, atom identity is process-wide and needs no table and no lock. Only names created at run time (computed keys, `JSON.parse` keys not in the image, `eval` text) go into the mutable table.
* Symbols/private names of the program: `ProgramSymbols` (open-addressed array in `ProgramIdentifiers`, filled lazily by `uid(number)`; has `tryGetIdentifierConcurrently`; header includes `<wtf/Lock.h>`; `aot/runtime/AOTProgramIdentifiers.h:31-80`).
* `vm.lastAtomizedIdentifierStringImpl/…AtomStringImpl` one-entry cache in `JSString::toIdentifier` (`JSString.h:958-…`): two `Ref`s written one after the other (section 10).

### 6.6 Lazily created constant cells (per Instance)

`ProgramStrings::constant(number)` (`aot/runtime/AOTProgramStrings.cpp:169-186`): read the Instance's cell word; 0 → `createString`, `setConstant` under `m_state.caches->lock()` (a lock shared with the collector's visit of remembered slots). `keyCell`, `literalString…`, `bigIntConstant`, `functionNameAt`: check, create, `setImmortalCellAt` (`immortalCells->add(cell)`; plain store; :242-250, 342-358). Compiled code reads these words in line and calls the operation on 0. A race makes two cells: invisible for strings (no identity); for `Symbol` key cells `Symbol::create(vm, SymbolImpl&)` goes through `vm.symbolImplToSymbolMap` (WeakGCMap, unlocked; section 10).

### 6.7 String builders

The compiler turns `s += …` loops, template literals, `join`, `JSON.stringify` into an opaque IR value `StringBuilder` (`AOTSchema.def:94`, heap `StringBuilderContents … perObject`).
* `StackStringBuilder` (`aot/runtime/AOTStringBuilderStorage.h:182-200`): 128+16 bytes **in the compiled frame**, cursor in a register. Thread-private by construction.
* `StringBuilderStorage` (`:26-178`): a GC cell (own IsoSubspace `vm.aotStringBuilderStorageSpace`) with a `RefPtr<StringImpl> m_buffer`, raw `m_characters`, `m_position`, `m_limit`, segments (long strings appended by reference), last result. Inline code appends by writing at `m_characters + m_position` after a *reservation* (`reserve/tryReserve/commit`; `maxStringBuilderReservation = 4096`, per-token sizes for JSON in `aot/runtime/AOTStringBuilderReservation.h:14-20`). `toString()` may *share* the buffer with the result (`m_sharedLength`, `wouldOverwriteSharedCharacters()` tests `m_buffer->hasOneRef()`, :138).
  A builder belongs to one activation; it is not reachable from JS. (Whether one can be live across an `await`/`yield` and so migrate between threads with its coroutine was not checked.)
* **The pool is per Instance and unlocked**: `instance.stringBuilder.freeStorage` (`aot/runtime/AOTStringBuilderWords.h:16-19`), a singly linked list through `m_nextFree`. Pop in line: `head = load(pool); store(pool, load(head.nextFree)); store(head.nextFree, 0)` (`aot/codegen/AOTStringBuilderEmitter.cpp:241-253`); C++ `acquire`/`returnToPool` (`AOTStringBuilderStorage.cpp:89-126`). "Every collection clears the word." Two threads popping at once get **the same builder** → both write one buffer with independent cursors and limits → heap overflow is prevented only by each one's own `m_limit` check against a shared `m_position`… memory unsafe once one of them replaces the buffer.
* `StringKeyCache` (`aot/runtime/AOTPropertyCachesStringKeys.h`): `tryFinishWithCachedKey`, `tryFindKeyByParts` look up a per-Instance cache of short key strings so that `map.get(a + ":" + b)` allocates nothing (belongs to the property-cache report).

### 6.8 JSON

Per-Instance `JSONWords` (`aot/runtime/AOTJSONWords.h:28-47`): `sourceStart`, `sourceEnd`, `sourceOwnerStackPointer`, `runtime`, `sourceEncoding`, `canEndSourceInline`, `valueParserFailed`: "the open source" of typed `JSON.parse`, with a stack discipline keyed by **stack pointer comparison** (`didUnwindTo`). IR heap `ParserState`. One open source per Instance: two threads parsing at once overwrite each other's `sourceStart/End` → out-of-bounds reads of text. `runtime/JSONObject.cpp` (+1,042) fast stringify uses per-call state only (not fully read); `vm.jsonAtomStringCache` in section 10.


---------------------------------------------------------------------------------------------------------------------------------------

## 7. Typed arrays, ArrayBuffer, SharedArrayBuffer, DataView, Atomics

Paths are relative to `Source/JavaScriptCore/` unless they start with `WTF/`, `bmalloc/` (same `Source/`) or `bun:` (`<bun>/`). "View" means any `JSArrayBufferView` (typed array or DataView).

Short version. Compiled code and stubs read a view with three or four plain, unordered loads (`m_mode`, `m_length`, `m_vector`, then the element). Nothing ties them together except "no other code ran in between". In hot functions B3 hoists the first three out of loops, across calls whose effect summary does not write `TypedArrayStorage`, and across safepoints. The C++ side (detach, transfer, `.buffer`, view creation, refcounts, `Atomics.wait`) is written for one mutator per VM and takes `cellLock()` only against the concurrent collector. Fixed-length views of a `SharedArrayBuffer` are the one case that is already safe: their three words never change after construction.

### 7.1 Layout and modes

`JSArrayBufferView : JSNonFinalObject` (`runtime/JSArrayBufferView.h:182`). Fields in declaration order (`:375-378`):

```cpp
VectorPtr m_vector;          // CagedBarrierPtr<Gigacage::Primitive, void>  (:193)
size_t m_length;             // elements (bytes for a DataView)
size_t m_byteOffset { 0 };
TypedArrayMode m_mode;       // uint8_t
```

They follow the 8-byte `JSCell` header and the 8-byte butterfly word, so 16 / 24 / 32 / 40 (derived, not read from a static_assert; `aot/stubs/AOTStubsAllocation.cpp:560` asserts only `offsetOfByteOffset() == offsetOfLength() + 8`, and `aot/runtime/AOTTypedArrayOperations.cpp:38` asserts `sizeof(View) == sizeof(JSArrayBufferView)`). All four are plain fields. None is `std::atomic`.

Modes (`JSArrayBufferView.h:60-98`) and bit masks (`:100-109`):

| Mode | Bits | Who owns the vector |
| --- | --- | --- |
| `FastTypedArray` | `0001'0000` | GC: an auxiliary cell of `vm.primitiveGigacageAuxiliarySpace()`, length <= `fastSizeLimit` = 1000 elements (`:192`, `JSArrayBufferView.cpp:68-78`). No butterfly |
| `OversizeTypedArray` | `0011'0000` | `Gigacage::tryMalloc`, freed by a heap finalizer when the view dies (`JSArrayBufferView.cpp:94-106`, `:183`, `:228-238`) |
| `WastefulTypedArray` | `0101'1000` | An `ArrayBuffer`. "The view does not own the vector" (`:78-83`) |
| `GrowableShared[AutoLength]WastefulTypedArray` | `0101'101x` | growable `SharedArrayBuffer` |
| `ResizableNonShared[AutoLength]WastefulTypedArray` | `0101'110x` | resizable `ArrayBuffer` |
| `DataViewMode` and its four resizable/growable variants | `1000'1xxx` | An `ArrayBuffer`, held in `JSDataView::m_buffer` |

`isResizableOrGrowableSharedMode = 0b110`, `resizabilityAndAutoLengthMask = 0b111`, `isHavingArrayBufferMode = 0b1000`.

- Where the `ArrayBuffer*` lives. Wasteful typed arrays: in the butterfly's `IndexingHeader`, `u.typedArray.buffer`, a union with `publicLength`/`vectorLength` (`runtime/IndexingHeader.h:48`, `:69-70`, `:125-135`), read by `existingBufferInButterfly()` = `butterfly()->indexingHeader()->arrayBuffer()` (`JSArrayBufferView.h:383-387`). DataView: `ArrayBuffer* m_buffer`, a raw pointer (`runtime/JSDataView.h:83`, `:108`). Neither is a `RefPtr`. What keeps the buffer alive is the incoming-reference mechanism of 7.4.
- Detached means `hasArrayBuffer() && !hasVector()` (`:269`): vector null, and `detachFromArrayBuffer()` also zeroes length and byteOffset.
- C++ trusts the raw fields when `(mode & 0b111) <= 0b010`, that is fixed length, or growable-shared with an explicit length (`canUseArrayBufferViewRawFieldsDirectly`, `:141-160`). Everything else recomputes from `buffer->byteLength(seq_cst)` through `IdempotentArrayBufferByteLengthGetter` (`:287-342`, `runtime/ArrayBuffer.h:416-433`).
- Gigacage. `GIGACAGE_ENABLED` is 0 when `BUSE(MIMALLOC)` (`bmalloc/bmalloc/BPlatform.h:486-492`). AOT code never cages or untags a vector: no hit for `caged`, `cageConditionally`, `untagArrayPtr` in `aot/codegen`, `aot/builtins`, `aot/stubs`. The vector word is used as loaded.

### 7.2 What compiled code and stubs do

#### 7.2.1 How an access becomes a typed array access

`planClassAccess` (`aot/analysis/AOTPropertyAccessElements.cpp:414-435`) picks `ElementAccessForm::TypedArray` when the receiver's built-in class, or a parent of it, has a `typedArrayType` (`BuiltinTable::typedArrayClassOf`, `aot/builtins/AOTBuiltinTable.cpp:429-436`). `ResolveAccesses` rewrites `ElemGet`/`ElemSet` into `GetTypedArrayElement` / `PutTypedArrayElement` (`aot/passes/AOTResolveAccessesElements.cpp:68-96`). If the class is only expected, a `TypeTest` stands in front (`plan.testsReceiverClass`, `aot/passes/AOTArrayElementTests.cpp:196-197`). Each `XArray` class declares `jsType: XArrayType` (`aot/builtins/generator/lib.ts:1134-1148`), and the cell test kinds are `CellTypeIs` / `InheritsClassInfo` / ... (`aot/codegen/AOTTypeTestEmitterCells.cpp:160-193`). `Proof::TestedType` lasts `Forever` (`aot/ir/AOTSchema.def:21`), which stays true with threads: the `JSType` byte of a view never changes.

The operations (`aot/ir/schema/prepared-operations.def`):

```
:23  IsInsideView (view, position, typedArrayType) [keyIsIntegral] -> boolean ; reads(TypedArrayStorage) ; proves = InBounds
:66  GetTypedArrayLength (view, typedArrayType) -> int[0, 17179869184] ; reads(TypedArrayStorage)
:67  GetTypedArrayElement (view, position, guard?, ...) [keyIsIntegral isInsideView] ; Allocates reads(TypedArrayStorage, TypedArrayElements) ; relies = InBounds ; guarded = view
:68  PutTypedArrayElement (view, position, value, guard?, ...) [keyIsIntegral isInsideView] ; reads(TypedArrayStorage) writes(TypedArrayElements) ; relies = InBounds ; guarded = view
:103 RequireAttachedTypedArray (view) ; reads(TypedArrayStorage) throws ; runtime = ThrowIfTypedArrayIsDetached
```

#### 7.2.2 `ta[i]` read, typed code (`aot/codegen/AOTElementAccessEmitterTypedArrays.cpp`)

Inline for Int8..Uint32, Uint8Clamped, Float32, Float64. Not for Float16, BigInt64, BigUint64 (`hasInlineElementAccess`, `aot/builtins/AOTTypedArrayKinds.h:16-37`): those read through the generic `getByVal` stub (`:112-118`, `readOutOfLine`, `aot/codegen/AOTElementAccessEmitter.cpp:630-634`).

Unguarded form (`:127-142`), in emission order:

1. index from the position operand; not an integer goes to `outside`.
2. `load8 view->m_mode`; any of `0b111` set goes to `otherwise`, the `getByVal` stub (`typedArrayElementsOf`, `:51-55`).
3. `load64 view->m_length`, then `load64 view->m_vector`. Both loads are emitted before the bounds branch (`:54`).
4. `index <u length`, else `outside`, which yields `undefined` (`:133`, `:137-138`).
5. element load at `vector + (index << logElementSize)` in heap `TypedArrayProperties`, then `ensureStillAliveHere(view)` (`:69-76`).

There is no test of the vector against null and no detach test. The only statement about detaching is the comment on the guard: "A view of a detached buffer has the length 0." (`aot/codegen/AOTElementEmitterLiterals.cpp:64`). So safety against detach rests on reading a length and a vector that belong together.

Guarded form. `choosePresenceTest` returns `PresenceTest::InsideView` for every typed array access outside rare blocks (`aot/passes/AOTArrayElementPresence.cpp:211-221`). Then a separate `IsInsideView` value does steps 1, 2, 4 and branches (`AOTElementEmitterLiterals.cpp:65-74`); the failing edge goes to generic code. The access has `isInsideView` set and does only (`:58-67`, `:119-125`):

```cpp
TypedArrayElements elements { view, loadField<Integer>(view, JSArrayBufferView_length), loadField<Pointer>(view, JSArrayBufferView_vector) };
if (generator().validatesOrChecksProofs()) { ...checkPrecondition(mode has no 0b111 bit); checkPrecondition(below(index, elements.length)); }
```

In a normal build the length load is dead. The access is `load m_vector; load element`, with no check of its own, at any distance behind the test that the guard analysis allows.

#### 7.2.3 `ta[i] = v` (`:145-196`)

- Inline (`storesInline`, `:157`) for the same types when the value is unboxed, and not (clamped and a double). Same sequence as the read, out of bounds is a no-op (`:183-187`), guarded form at `:162-167`. Otherwise `operationAOTTypedArrayPut`, which is `view->setIndex()` in C++ (`:192`, `aot/runtime/AOTTypedArrayOperations.cpp:310-329`).
- BigInt64/BigUint64 (`:171-180`): `length = lengthOf(view)`, which for a non-fixed view is a leaf call to `operationAOTTypedArrayLength` = `view->length()` (`:38-47`, `aot/runtime/AOTArrayAllocationProfile.cpp:98-101`). Then the bounds branch, then `load m_vector`, then `store64`. This is the one inline store that also runs on resizable and growable views.
- Element stores are plain `store8/16/32/64`, `storeFloat`, `storeDouble` through `SlotEmitter::store` (`:97-98`).

#### 7.2.4 `length`, `byteLength`, `byteOffset`

| Where | Sequence |
| --- | --- |
| Inline built-ins `TypedArrayLength/ByteLength/ByteOffset` (`aot/ir/schema/inline-builtins.def:23`, `reads(TypedArrayStorage)`) | `load8 m_mode & 0b110`; nonzero is a miss, which calls the host getter; else `load64 m_length` (shifted for byteLength) or `load64 m_byteOffset` (`aot/builtins/AOTArrayBufferViewBuiltinEmitter.cpp:30-33`, `:72-93`) |
| `GetTypedArrayLength` (made by `for..of` expansion, `aot/passes/AOTExpandIterationIndexed.cpp:133-149`, `:257-258`) | mode `& 0b111`, `m_length`, else leaf call (`AOTElementAccessEmitterTypedArrays.cpp:38-47`, `:102-105`) |
| Access by name through the caches, `Slot::Kind::TypedArrayLength` | `mode & 0b111`, `load64 m_length`, above INT32_MAX goes uncached (`aot/stubs/AOTStubsPropertyReadHandlers.cpp:347-355`). Filled when the chain walk finds the native length getter, or byteLength for 1-byte elements (`aot/runtime/AOTPropertyCachesReadAnswers.cpp:110-122`, `:156-157`) |
| Boxed call stub of the getter | JSType range test, `mode & 0b111`, `load64 m_length` (`aot/stubs/AOTStubsBoxedCalls.cpp:248-257`) |
| `DataView.prototype.byteLength` / `byteOffset` | host call / generic (`aot/builtins/buffer.builtins.ts:272-273`) |

`RequireAttachedTypedArray` is the only emitted null test of a vector: `hasFixedLength && m_vector != 0`, else `operationAOTThrowIfTypedArrayIsDetached` (`aot/codegen/AOTExpandedIterationEmitter.cpp:80-94`).

#### 7.2.5 DataView get/set, `Buffer` readX/writeX, Atomics: one shared helper

`findTypedArrayElement(view, index, otherwise, outside)` (`AOTArrayBufferViewBuiltinEmitter.cpp:50-65`):

```cpp
Condition isFixed  = hasFixedLength(view);                                    // load8 m_mode & 0b110
Condition isInside = below(index, select(isFixed, lengthOf(view), integer(0))); // load64 m_length
branchUnless(isInside, otherwise);
return vectorOf(view);                                                        // load64 m_vector, after the branch
```

- DataView (`aot/builtins/AOTBufferBuiltinEmitter.cpp:35-71`): `findBytes` tests `offset + size - 1 <u m_length` (`:102-108`), then one load or store of the whole width at `vector + offset`, with a `byteSwap` patchpoint for big endian (`loadBytes` `:143-185`, `storeBytes` `:187-216`). Float16 is never inline (`aot/select/AOTBuiltinFormRulesNumbers.cpp:109-112`). Every miss (not fixed length, out of range, detached since length is 0) calls the host function, which throws what the standard says.
- `Buffer.prototype.readInt32LE` and friends: `generateBufferAccess` (`:282-308`), the same `findBytes` + `loadBytes`/`storeBytes`. Bun's table generates the entries from `JSBufferAccessors.def` with `b.inline("BufferAccess", ...)` and `this: b.instance("Buffer")` (`bun:src/codegen/builtins/collect-registrations.ts:179-203`).
- `buf[i]` on a `Buffer`: Bun declares `b.class("Buffer", { extends: "Uint8Array", jsType: "Uint8ArrayType", isNeverPristine: true, pristineTest: "InstanceStructure", ... })` (`bun:src/codegen/builtins/host/buffer.builtins.ts:232-239`), so `typedArrayClassOf` gives `TypeUint8` and 7.2.2/7.2.3 apply.

#### 7.2.6 Untyped code: shared stubs (`aot/stubs/AOTStubsElements.cpp`)

- `checkTypedArrayAccess` (`:95-106`): `load8 JSType`, range test against `FirstTypedArrayType`; `test8 m_mode, 0b110`, nonzero is slow; sign-extend the key; `load m_length`; `index >=u length` is absent/slow; `load m_vector`. Then a chain of JSType compares picks the load (`:259-313`) or store (`:450-489`). BigInt and Float16 arrays fall to the slow path.
- `GetByValExpecting` / `PutByValExpecting` for Int32Array and Uint8Array (`:636-646`): is cell, `JSType ==`, `mode & 0b110`, `index >=u [m_length]` (compare with memory), `load m_vector`, access. A miss jumps to the general stub.
- Order in every stub: type, mode, length, vector, element. All plain loads.

#### 7.2.7 Hoisting and CSE: which heaps, which proof, what ends it

IR heaps: `heap TypedArrayStorage observable ordered` and `heap TypedArrayElements observable ordered` (`aot/ir/AOTSchema.def:68-69`). Neither is `perObject`: "views of one ArrayBuffer are different objects that share its bytes and its length" (`aot/ir/AOTAbstractHeap.cpp:83`). B3 heaps nested below them (`aot/codegen/AOTHeaps.cpp:43-49`): `JSArrayBufferView_byteOffset/_length/_mode/_vector`, `ArrayBuffer_data`, `Butterfly_arrayBuffer` under `TypedArrayStorage`; `TypedArrayProperties` under `TypedArrayElements`. An element store therefore never clobbers a vector, length or mode load. `ArrayBuffer_shared` and `JSArrayBuffer_impl` are declared immutable (`aot/codegen/AOTHeaps.h:48-49`), with the reason "A buffer is made shared before it gets its wrapper, and a shared buffer is never detached." (`aot/codegen/AOTTypeTestEmitterCells.cpp:194`).

At the IR level:

- The proof is `Proof::InBounds(Stretch, readsOnlySubject = true)` (`AOTSchema.def:24`). A `GuardFact` depends on the locations its test reads (`aot/analysis/AOTGuardAvailability.h:38-48`, `locationsReadBy`), here all of `TypedArrayStorage`. The user must name the same position and the test must be an `IsInsideView` (`provesInBoundsForUser`, `aot/analysis/AOTGuardFacts.cpp:630-641`).
- What ends it (`KillQuery::writtenOf`, `aot/analysis/AOTGuardAvailability.cpp:152-173`): an operation that may suspend, or whose `effects.writes` overlap. Since the heap is not per object, any such write ends every view proof (`WriteExtent::Everywhere`, `:167-170`). Nothing else ends it: allocation, collection, a loop poll, a call with a narrower summary do not.
- Who writes `TypedArrayStorage` according to the table: `%TypedArray%.prototype.buffer` (`writes: [storage]`, `aot/builtins/typedarray.builtins.ts:261-269`, because of 7.3.1), `subarray` (`viewOfRange`, `:198`), `ArrayBuffer.prototype.resize/transfer/transferToFixedLength/slice` and `SharedArrayBuffer.prototype.grow` through `uses: { this: changesState }` on classes with `stateHeap: storage` (`aot/builtins/buffer.builtins.ts:14`, `:63-92`, `:197`, `:230`), and anything that runs unknown code.
- `EliminateRedundantReads` covers only `ModuleVariables, EnvironmentFields, ObjectFields` (`aot/passes/AOTEliminateRedundantReads.h:16`). `HoistLoopInvariantReads` covers only `ModuleVariables, EnvironmentFields` (`aot/passes/AOTHoistLoopInvariantReads.cpp:27`). So no IR pass merges or hoists `GetTypedArrayLength` or element reads themselves. `EliminateGuards` (not read) works on the tests.

At the B3 level, for functions that go through B3/Air (`aot/codegen/AOTB3Policy.cpp`): `hoistsLoopInvariantValues = true` (`:114`), `redundancyElimination = MemoryNumbering` (`:125`), and `loops.rotates / foldsComparisonsWithBounds / versions / clampsLoopBounds / hoistsOnlyFromLikelyPaths / hoistsImmutableLoadsPastWrites = true` (`:144-153`).

- LICM moves a load to the pre-header unless `effects.reads.overlaps(data[loop].writes)` (`b3/B3HoistLoopInvariantValues.cpp:298-299`).
- The writes of a call are exactly its IR effects: `applyEffects` does `appendHeaps(effects.writes, writes)` and then `decoratePatchpointWrite` (`aot/codegen/AOTRuntimeCallEffects.cpp:72-118`).
- A collection adds only `collectorState, weakTableStorage, weakTableEntries` (`AOTHeaps.cpp:441-447`).
- Consequence: in `for (...) s += ta[i]` or `ta[i] = f(i)` with an `f` whose summary does not write `TypedArrayStorage`, the loads of `m_mode`, `m_length`, `m_vector` are loop invariant for B3 and may be done once in front of the loop. `b3/B3LoopRangeChecks.h:28-47` plans to decide `index <u limit` comparisons for an invariant limit and to clamp the loop bound (I read the policy and the headers, not the transformation).

In the direct generator (most functions): no phases, but `tryReuseLoad` aliases a load to an earlier identical one while `memoryEpoch` is unchanged (`aot/codegen/AOTDirectCodeGenerator.cpp:2825-2846`). The epoch advances at every store, fence and patchpoint (`:1696`, `:1699`, `:1706`).

Safepoints:

- `LoopHint` is `UsesRuntime terminates collects` (`aot/ir/schema/operations.def:150`). The poll patchpoint gets those effects, with the comment "Handling a trap stops the thread for the collector or ends it. It looks at no object of the program." (`aot/codegen/AOTHintEmitter.cpp:165-172`). A stop at a poll therefore invalidates no hoisted vector or length.
- Some loops have no poll at all: `generateLoopHint` skips it when `endsWithinLengthOfData(block)` (`:144-149`), that is when an increasing counter with a constant step is bounded by `GetTypedArrayLength` / inline `TypedArrayLength` / array or string length, and every non-rare operation in the loop "always completes" (`:61-141`).

### 7.3 C++ state changes, and what a concurrent `ta[i]` can see

#### 7.3.1 `slowDownAndWasteMemory()` (Fast/Oversize to Wasteful), `runtime/JSArrayBufferView.cpp:271-334`

It is reached from `possiblySharedBuffer()` whenever the mode is Fast or Oversize (`runtime/JSArrayBufferViewInlinesLight.h:54-84`): `.buffer`, `subarray`, `possiblySharedImpl()`, and every native API that wants an `ArrayBuffer`. AOT operations reach it too (`AOTTypedArrayOperations.cpp:302`, `:360`, `:377`). Order of events:

1. `ASSERT(!hasArrayBuffer())` (debug only), `DeferGCForAWhile`.
2. Fast: `ArrayBuffer::tryCreate(span())`, a malloc plus a copy of the bytes (`:299`). Oversize: `ArrayBuffer::createAdopted(span())`, same address, the buffer now owns it (`:308`).
3. `setButterfly(vm, Butterfly::createOrGrowArrayRight(...))`, outside any lock (`:320-322`).
4. Under `cellLock()`: `indexingHeader()->setArrayBuffer(buffer)`; `m_vector.setWithoutBarrier(buffer->data())`; `WTF::storeStoreFence()`; `m_mode = WastefulTypedArray` (`:324-330`).
5. `heap->addReference(this, buffer, ...)` (`:331`).

The lock and the fence serve the collector. `JSGenericTypedArrayView::visitChildrenImpl` takes `cellLock()` to read mode, vector and byte size as a unit, and marks the vector as auxiliary only for `FastTypedArray` (`runtime/JSGenericTypedArrayViewInlines.h:756-800`). `JSArrayBufferView::visitChildrenImpl` does `hasArrayBuffer()`, `loadLoadFence()`, `possiblySharedBuffer()`, `addOpaqueRoot(buffer)` (`JSArrayBufferView.cpp:204-217`).

A second mutator:

- Reader or writer of a Fast view that loaded the old vector (or has it hoisted): it reads stale bytes, and its stores after the copy in step 2 are lost for good. Wrong value only, as long as the old auxiliary cell stays allocated. After step 4 the view no longer marks it, and `ensureStillAliveHere(view)` keeps the view, not the old vector. It survives only if the collector finds the raw pointer in that thread's registers or stack. `ConservativeRoots` does accept interior pointers into a block's cell ("We could point into the middle of an object", `heap/ConservativeRoots.cpp:211-213`). Whether every mutator's registers and compiled frames are scanned is the heap report's question.
- Length is unchanged, so there is no length/vector mismatch here. The fast-path mode test (`0b110` / `0b111`) gives the same answer before and after.
- Two threads in `slowDownAndWasteMemory()` on one view: both pass step 1. Oversize: two `ArrayBuffer`s adopt the same malloc block, a double free. Fast: two buffers, and the loser's stays referenced from the incoming list while views made from it alias nothing. Step 3 races with any butterfly use. Memory safety.

#### 7.3.2 Detach and transfer

`ArrayBuffer::transferTo` (`runtime/ArrayBuffer.cpp:498-522`): shared buffers are shared, not detached (`:507-510`); non-detachable ones (`m_pinCount` or locked) are copied (`:512-517`); else `result = m_contents.detach(); notifyDetaching(vm);`. `ArrayBuffer::detach` (`:525-529`) is `auto unused = m_contents.detach(); notifyDetaching(vm);`.

1. `ArrayBufferContents::detach()` moves all seven fields out by `swap` (`ArrayBuffer.h:194-211`). `ArrayBuffer::m_contents.m_data` becomes null first.
2. `notifyDetaching` (`:531-539`) walks `incomingReferenceAt(i)`, and for each `JSArrayBufferView` calls `detachFromArrayBuffer()` (`JSArrayBufferView.cpp:260-269`): under `cellLock()`, `m_length = 0; m_byteOffset = 0; m_vector.clear();`, then `realm()->notifyArrayBufferDetaching()`, which fires `m_arrayBufferDetachWatchpointSet` once (`runtime/JSGlobalObjectInlines.h:723-728`, `runtime/JSGlobalObject.cpp:2625-2628`). Then the buffer's own `m_detachingWatchpointSet.fireAll`.
3. Nothing in `aot/` reads either watchpoint set (no hit for `arrayBufferDetachWatchpointSet` or `detachingWatchpointSet` outside tests). They matter only to the DFG (`dfg/DFGGraph.h:1033`).

When the bytes are freed: `~ArrayBufferContents` runs `m_destructor` (`ArrayBuffer.h:157-163`).

- `detach()`: at the closing brace, right after the views were cleared.
- `ArrayBuffer.prototype.transfer(sameLength)`: not freed. The same address becomes the new buffer's store (`runtime/JSArrayBufferPrototype.cpp:350-359`).
- `transfer(otherLength)` and `transferToFixedLength` of a resizable buffer: copied into a new buffer, and `dummyContents` frees the old block at return (`:380-395`).
- `postMessage` / `structuredClone` transfer: whenever the receiver drops it (Bun side, not read).

There is no grace period, epoch or handshake anywhere.

A concurrent `ta[i]` (mode, length, vector, element):

| Interleaving | Result |
| --- | --- |
| length and vector both old, free happens before the element access | use after free, read or write. Memory safety |
| length old (nonzero), vector new (null). Possible because the writer stores length first and the reader loads length first | access at address `index << log2(size)`, anywhere below the old byte length, so in `[0, 2^32)` (`MAX_ARRAY_BUFFER_SIZE` is checked as `2 ** 32` in `aot/builtins/typedarray.builtins.ts:6`). A wild access. It is not one of the planned null faults of `image/AOTFaultHandler` as far as I read. Memory safety / crash |
| guarded form, hoisted vector, or stub already past its check | as row 1, with a window as long as the loop |
| length new (0) | out of bounds: `undefined` / no-op. Correct |
| `transfer(sameLength)` | the stale view aliases the new buffer until that one dies. First a wrong value, a use after free later |

On ARM64 the reader's two loads and the writer's three stores are also unordered in hardware. `cellLock()` orders only against other lock holders.

#### 7.3.3 `resize`, `grow`, wasm refresh

- `ArrayBuffer::resize` (`ArrayBuffer.cpp:555-641`) takes no lock, and says why: "A non-shared buffer that is not a Wasm memory owns its handle outright and is reachable only from its own agent's thread, so the handle's lock has nothing to guard here." (`:563-564`). The address never moves: the whole `maxByteLength` is reserved (`tryAllocateResizableMemory`, `:394`). Growing commits pages, zero-fills, then stores `m_sizeInBytes`, a plain `size_t` (`:632-635`). Shrinking calls `OSAllocator::protect(..., false, false)` on whole wasm-size pages before it stores the smaller size (`:612-629`, `:635`).
- Compiled code never reads a resizable view inline, except the BigInt store of 7.2.3. The C++ slow path is check then act: `isDetached()`, `inBounds(i)` (reads `buffer->byteLength()` relaxed, `JSGenericTypedArrayViewInlines.h:828-843`), then `typedVector()[i]` (`:857-872`, `:880-896`). A racing shrink gives a fault on a protected page (crash, no corruption) or a read/write of bytes beyond the new length inside the last committed page (wrong value). Two racing `resize` calls corrupt `memoryHandle->size()` and the `BufferMemoryManager` accounting (`:590-629`).
- `SharedArrayBufferContents::grow` (`:660-689`) takes `m_memoryHandle->lock()`. `tryGrow` (`:691-763`) reads `m_sizeInBytes` seq_cst, commits, zero-fills, then `updateSize(newByteLength)` with a seq_cst store to `std::atomic<size_t>` (`ArrayBuffer.h:98-101`, `:128`). Grow only, the address never moves. This is already multi-agent safe.
- `ArrayBuffer::refreshAfterWasmMemoryGrow` rewrites `m_vector` of every incoming view without a lock (`ArrayBuffer.cpp:459-476`, `runtime/JSArrayBufferViewInlines.h:44-53`). That is the same hazard as detach for BoundsChecking wasm memories, whose old block is released by wasm code (not read).

#### 7.3.4 `pin` / `unpin` / `pinAndLock`

There is no `m_locked` field in this tree. It is `Checked<unsigned> m_pinCount` with `s_lockedFlag = INT32_MIN` (`ArrayBuffer.h:340-341`). `pin()` is `m_pinCount++`, `unpin()` is a read, a decrement and an `|=`, `pinAndLock()` is `|=` (`:386-406`): all non-atomic. `isDetachable()` = `!m_pinCount && !isShared()` (`:398-401`). `transferTo` tests it and then detaches with nothing held, so a `pin()` on another thread between test and detach is lost: native code that pinned the buffer then works on freed bytes. Callers: `ArrayBufferView::setDetachable` (`runtime/ArrayBufferView.cpp:97-108`), the C API (`API/JSTypedArray.cpp:250`, `:337`), `makeShared`, `makeWasmMemory`.

### 7.4 Refcounts, contents, wrapper, incoming references

- `class ArrayBuffer final : public GCIncomingRefCounted<ArrayBuffer>` (`ArrayBuffer.h:249`), which derives from `DeferrableRefCounted<T>` (`heap/GCIncomingRefCounted.h:43`), whose count is `mutable uint32_t m_refCount` with `ref()` = `m_refCount += 2` and `derefBase()` = `m_refCount -= 2; return !m_refCount` (`WTF/wtf/DeferrableRefCounted.h:46-49`, `:76-80`, `:93`). **Not atomic.** Bit 0 is the "deferred" flag, set while any GC cell refers to the buffer. Ordinary paths ref and deref all the time: `RefPtr<ArrayBuffer> buffer = view->possiblySharedBuffer()` in `isArrayBufferViewOutOfBounds` and `integerIndexedObjectLength` (`JSArrayBufferViewInlines.h:72`, `:105`), `JSDataView::viewByteLength` (`JSDataView.h:69`), `subarray` (`runtime/JSGenericTypedArrayViewPrototypeFunctions.h:2010`), `Ref<ArrayBuffer> protect(*this)` in `transferTo`. Two threads on views of one buffer lose an update: premature delete or leak.
- Thread safe: `SharedArrayBufferContents : ThreadSafeRefCounted` (`ArrayBuffer.h:59`) and the `SharedTask` destructor.
- `ArrayBufferContents` (`ArrayBuffer.h:239-246`): `m_data` (CagedPtr), `m_destructor`, `RefPtr<SharedArrayBufferContents> m_shared`, `RefPtr<BufferMemoryHandle> m_memoryHandle`, `size_t m_sizeInBytes`, `size_t m_maxByteLength`, `bool m_hasMaxByteLength`. All plain. `sizeInBytes()` goes to the atomic only when growable and shared (`:169-176`).
- Each agent today has its own `ArrayBuffer` object over one `SharedArrayBufferContents` (`shareWith`, `ArrayBuffer.cpp:257-270`). With threads in one VM, one `ArrayBuffer` and one `JSArrayBuffer` are reached by all threads, so the non-atomic parts above are shared even for a SAB.
- Incoming references: `uintptr_t m_encodedPointer` is either one tagged `JSCell*` or a `Vector<JSCell*>*` (`GCIncomingRefCounted.h:109-113`). `addIncomingReference` (`heap/GCIncomingRefCountedInlines.h:34-56`) goes none, singleton (+ `setIsDeferred(true)`), `new Vector`, `append`. No lock. It is called through `Heap::addReference` (`heap/Heap.cpp:716-724`) and `GCIncomingRefCountedSet::addReference`, which also appends to the heap-wide `m_vector` / `m_markedObjectsWithNewIncomingReferences` and adds to `m_bytes` (`heap/GCIncomingRefCountedSetInlines.h:47-64`). No lock there either. Callers: every view over a buffer and every `JSArrayBuffer` (`JSArrayBufferView.cpp:190`, `:198`, `:331`; `runtime/JSArrayBuffer.cpp:49`; `AOTTypedArrayOperations.cpp:369`). The list is pruned in `sweep()` at the end of a collection (`SetInlines.h:67-108`, `Heap.cpp:2496`), where the last removal deletes the buffer. `notifyDetaching` and `refreshAfterWasmMemoryGrow` walk it unlocked.
- View creation over a buffer is check then publish: `buffer->isDetached()` (`JSGenericTypedArrayViewInlines.h:113`), range check against `byteLength()` (`:120`), `m_vector = data() + byteOffset` in the `ConstructionContext` (`JSArrayBufferView.cpp:132`), cell allocation, and only then `finishCreation` with `addReference` (`:190`). A detach on another thread between `:132` and `:190` does not see the new view, which keeps a dangling vector and a nonzero length forever.
- `Weak<JSArrayBuffer> m_wrapper` is a public field (`ArrayBuffer.h:338`). `toJS` is get or create: `native.m_wrapper.get()`, else `JSArrayBuffer::create`, whose `finishCreation` calls `registerWrapper` (`runtime/SimpleTypedArrayController.cpp:44-58`, `JSArrayBuffer.cpp:45-51`). Bun does `domObject->m_wrapper = JSC::Weak<JSC::JSArrayBuffer>(wrapper, wrapperOwner, &world)` (`bun:src/jsc/bindings/JSDOMWrapperCache.h:115-119`). Two threads reading `ta.buffer` first get two wrappers (identity broken, `ta.buffer !== ta.buffer`) and race on the `Weak` assignment, that is on `WeakImpl` allocation and release in the heap's WeakSet.

### 7.5 Atomics

| Function | Under AOT | Source |
| --- | --- | --- |
| `load store add and or xor sub exchange compareExchange` on Int8..Uint32 arrays, all operands numbers | inline (`InlineBuiltin::Atomics`, two-way: a miss calls the host function) | `buffer.builtins.ts:148-173`, `AOTBuiltinFormRulesNumbers.cpp:114-124`, `:145-146` |
| the same on BigInt64/BigUint64 arrays | host function in `AtomicsObject.cpp` | `buffer.builtins.ts:165-171` |
| `isLockFree`, `notify`, `pause`, `wait`, `waitAsync` | host function | `buffer.builtins.ts:175-191`, `:309-343` |

Inline sequence (`aot/builtins/AOTBufferBuiltinEmitterAtomics.cpp:38-78`): convert index and operands; `findTypedArrayElement(view, index, miss, miss)` (mode `0b110`, length, vector: plain loads); address; `emitAtomicAccess`; `m_heaps.decorateFencedAccess(&m_heaps.TypedArrayProperties, access)`; `ensureStillAliveHere(view)`.

`emitAtomicAccess` (`:80-105`):

- RMW operations are B3 `AtomicXchgAdd/And/Or/Sub/Xor`, `AtomicXchg`, `AtomicStrongCAS`.
- `Load` is an ordinary B3 `Load8S/Load16S/Load` and `Store` an ordinary `Store8/16/Store`, made atomic only by the fence range. The FTL differs: it uses `atomicXchgAdd(0)` and `atomicXchg` (`ftl/FTLLowerDFGToB3.cpp:5397-5408`).
- "These are relaxed atomics by default. Use AbstractHeapRepository::decorateFencedAccess() with a non-null heap to make them seq_cst fenced." (`ftl/FTLOutput.h:383-384`).
- Lowering: a fenced store is `Xchg` on x86 and `StoreRel` on ARM64 (`b3/B3LowerToAir.cpp:1578-1590`). A fenced load is a plain `mov` with `kind.effects = true` on x86 and `LoadAcq*` on ARM64 (`:4192-4206`, `:4264-4304`). RMW picks acquire/release forms by `atomic->hasFence()` (`:3878`, `:4014-4018`).
- The direct generator refuses any function that has a fenced load or store (`AOTDirectCodeGenerator.cpp:1683-1684`, `:1692-1693`) and any opcode it does not list, which includes the B3 atomics (`:1804-1805`).
- C++ versions: `WTF::atomicExchangeAdd`, ..., `atomicCompareExchangeStrong`, `atomicLoadFullyFenced`, `atomicStoreFullyFenced` (`runtime/AtomicsObject.cpp:225-305`, `:349`).

Non-shared buffers: the RMW/load/store family works on any integer typed array, inline and in C++. There is no `isShared` test (`validateIntegerTypedArray`, `AtomicsObject.cpp:143-179`). `wait` and `waitAsync` throw unless `isShared()` (`:483-484`, `:515-516`). `notify` returns 0 for a non-shared array (`:610-611`).

What the compiler thinks an atomic orders:

- The B3 fence range is `TypedArrayProperties` only.
- The IR effects of the inline form are `Allocates reads(TypedArrayStorage, TypedArrayElements) writes(TypedArrayElements) throws` (`inline-builtins.def:42`, applied by `describeInline`, `aot/ir/AOTEffectsOfBuiltins.cpp:349-378`).
- The table entry, used while it is still a `CallBuiltin` and for all host-call forms including `wait`, `notify`, `pause`, adds `reads/writes: ["ExternalState"]` and `capability: "heap"` (`buffer.builtins.ts:9`, `:140-146`). The latter means `Allocates, UsesRuntime, ResolvesRopes` (`aot/builtins/generator/vocabulary.ts:228`).
- In no form does an Atomics call write `ObjectFields`, `EnvironmentFields`, `ModuleVariables`, `Elements`, `NamedProperties`, `ShapeWords`. So reads of ordinary objects may be kept in registers, merged (`EliminateRedundantReads`), hoisted (`HoistLoopInvariantReads`, B3 LICM) and proofs kept across `Atomics.load`, `Atomics.store` and even `Atomics.wait`. That is right while only SAB bytes are shared between agents. It is wrong as a synchronization primitive for shared objects.

`WaiterListManager`:

- Process global (`singleton()`, `runtime/WaiterListManager.cpp:105-113`). `m_waiterListsLock` guards the map from address to `WaiterList` (`:423-435`), and each list has its own `Lock lock` (`runtime/WaiterListManager.h:194`). `Waiter` and `WaiterList` are `ThreadSafeRefCounted`. The key is the raw address of the element.
- Is `Atomics.wait` allowed: `vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread()` (`AtomicsObject.cpp:454-457`). Bun installs `new WebCoreTypedArrayController(true)` for every VM, and the method returns that flag (`bun:src/jsc/bindings/BunClientData.cpp:192`, `bun:src/jsc/bindings/webcore/WebCoreTypedArrayController.cpp:62-65`). So it is allowed on the main thread too.
- `waitSyncImpl` (`WaiterListManager.cpp:116-182`): `Ref<Waiter> syncWaiter = vm.syncWaiter();`, which is **one `Waiter` per VM** (`runtime/VM.cpp:297`, `:1973-1976`, `runtime/VM.h:1556`); `findOrCreateList(ptr)`; `VMBlockingScope waitScope(vm, AtomicsWaitBlocked)`; lock the list; compare `WTF::atomicLoad(ptr)`; `list->addLast(syncWaiter)`; loop on `condition().waitUntil(list->lock, ...)`.
- The `Waiter` is an intrusive `BasicRawSentinelNode` (`WaiterListManager.h:42`). Two threads of one VM waiting at once would link the same node twice and share one `Condition`.
- `VMBlockingScope` calls `notifyVMBlocking`, which does `RELEASE_ASSERT(!std::exchange(vm.traps().m_isInBlockingScope, true))` (`runtime/VMManager.cpp:395-401`). The second waiter of a VM crashes there first.
- Termination wakes `vm.syncWaiter()` only (`runtime/VMTraps.cpp:330`, `:423`).
- GC interaction: `waitSyncImpl` neither releases heap access nor drops the JSLock. There is no `DropAllLocks`, `ReleaseHeapAccessScope` or `JSLock` use in the function, although `JSLock.h` is included. `VMBlockingScope` only counts the VM as blocked for the VMManager's stop-the-world among VMs ("Increments m_numberOfBlockedVMs so the STW trigger condition (stopped + blocked == active) can be satisfied while this VM sleeps", `runtime/VMManager.h:427-433`). That is not the collector's handshake. Today that is harmless, since the only mutator of that heap is asleep and allocates nothing.
- `waitAsync` ties the waiter to `vm.deferredWorkTimer` and to `RunLoop::currentSingleton()` of the calling thread (`WaiterListManager.cpp:58-64`, `:206-214`). The promise is resolved by a task scheduled on the VM's deferred work timer (`:284-292`, `:321-327`).

### 7.6 Ordinary accesses to shared memory

- A fixed-length view over a `SharedArrayBuffer` has mode `WastefulTypedArray` / `DataViewMode` (`JSArrayBufferView.cpp:115-116`, `:145-146`), so it takes exactly the inline paths of 7.2. No code path, inline or stub, asks whether the buffer is shared. Element accesses are plain machine loads and stores of the element width, with no fence and no atomic opcode.
- B3 treats them as ordinary memory in heap `TypedArrayProperties`. Two reads of `ta[i]` with no intervening write to that heap may be merged (MemoryNumbering, `tryReuseLoad`) and a read may be hoisted out of a loop. A spin on `ta[0]` without Atomics may never end. This already holds between workers today.
- Alignment and tearing. Typed arrays are element aligned (`verifyByteOffsetAlignment`, `JSGenericTypedArrayViewInlines.h:125`), and every inline access is one instruction of at most 8 bytes, so Float64 and the BigInt64 store do not tear in hardware on x86-64 or ARM64. DataView and `Buffer` accessors issue one unaligned 2/4/8-byte access at `vector + offset` (`AOTBufferBuiltinEmitter.cpp:148-151`, `:201-214`), which can tear when it crosses an alignment or cache line boundary. The C++ fallbacks copy byte by byte (`runtime/JSDataViewPrototype.cpp:165-176`, `:214-222`) and BigInt64 reads go through C++ `typedVector()[i]`. Tearing yields a wrong number only: the bytes are never interpreted as pointers, and doubles are purified on the way out in the stubs (`PureDouble::purify`, `AOTStubsElements.cpp:283`, `:311`). I did not check `SlotEmitter::load` for the inline Float32/Float64 path.
- Bulk copies use `memmove` / `memcpy` / `std::fill` on shared bytes (7.7).

### 7.7 Bulk operations in C++

All are host calls under AOT (`typedarray.builtins.ts:220-420`), except the AOT fast paths listed last. The pattern everywhere: validate; read the length; convert arguments (user code may run); **revalidate once**; then take `typedVector()` and loop or `memmove` with no further check. The revalidation exists because user code on the same thread may have detached or resized. Behind it the code assumes nothing can change. File: `runtime/JSGenericTypedArrayViewPrototypeFunctions.h` unless noted.

| Function | Validate / revalidate | Unchecked stretch |
| --- | --- | --- |
| `set(typedArray)` | `validateTypedArray(this)` `:295`; `integerIndexedObjectLength(source, getter)` `:302-305` | `setFromTypedArray`: `min(length, other->length())`, `validateRange`, then `memmove(target, source, byteLength)` (`JSGenericTypedArrayViewInlines.h:295-316`), or per-element loops with `getIndexQuicklyAsNativeValue` / `setIndexQuicklyToNativeValue`, which only `ASSERT(inBounds)` and reload `typedVector()` each time (`:238-262`, `:857-872`) |
| `copyWithin` | `:323`, length `:326`, recheck `:342-357` | `array = typedVector(); memmove(...)` `:359-360` |
| `fill` | `:668`, recheck `:685-690` | `memset` / `std::fill` on `underlyingVector` `:696-719` |
| `slice` | `:1851-1861`, recheck `:1896-1902` | `setFromTypedArray(..., CopyType::LeftToRight)` `:1913-1961`. The result is `createUninitialized` for non-resizable sources `:1885` |
| `subarray` | none; length and byteOffset read first `:1976-1977` | `possiblySharedBuffer()` (7.3.1) `:2010`, then `create` over the buffer (7.4) `:2021` |
| `sort()` | `validateTypedArray` `:1743`; `RELEASE_ASSERT(!isDetached())` (`Inlines.h:910`) | in place `std::sort` / `sortFloat` / `countingSort` / `radixSort` on `typedSpan()` |
| `sort(cmp)` | copies into a `Vector`, sorts there, then `isDetached()` `:1721`, `min(length(), ...)`, copy back through a fresh `typedSpan()` `:1726-1727` | the two `copyElements` calls |
| `forEach/map/filter/find*/every/some/reduce*` | per element `isDetached()` (+ `inBounds` if resizable) `:243-268`. For a non-resizable view that has a buffer, `array = typedVector()` is cached for the whole loop (`hasStableVector`, `:240-247`) | one element per check |
| `indexOf/includes/lastIndexOf/join/reverse/toReversed/with` | same family (`:366-660`, `:723-780`, `:1596-1640`, `:2032-2100`); not read line by line | |
| DataView get/set | `viewByteLength(getter)` then the range check | `dataView->vector() + byteOffset`, byte loop (`JSDataViewPrototype.cpp:156-176`, `:205-222`) |
| Atomics in C++ | `validateTypedArray`, `validateAtomicAccess` (length), convert operands, recheck `isDetached() \|\| !inBounds` (`AtomicsObject.cpp:116`, `:346`) | `typedVector() + accessIndex` `:119`, `:349`. `atomicsWaitImpl` takes `ptr` before converting the timeout, which may run user code (`:443-446`); safe only because a SAB cannot be detached |

`sort` needs its own line. The in-place algorithms are memory safe only if the data holds still, and the code knows that for shared buffers only: `if (isShared())` copies to `forShared` before `std::sort` (`Inlines.h:939-964`), and `radixSortWithCounters` takes a `snapshot` only `if (isShared())` (`:1252-1258`), then scatters with `destination[cursors[digit of source[cursor]]++] = value`, the cursors coming from histograms of an earlier pass over the same memory (`:1260-1323`). With threads, a non-shared buffer can change underneath: `std::sort`'s unguarded inner loops can run off the range (general knowledge of the algorithm, not read here), and a radix cursor can pass `length`, which is an index outside `scratch` or, after the `swap`, outside the array itself. `destination` is a `std::span`; whether this build has hardened span indexing was not checked. `countingSort` writes exactly the number of elements it counted, so it gives only wrong values.

AOT's own fast paths have the same shape, each with one check and then raw use:

- `asFixedBytes` (JSType, mode, `isDetached()`; `aot/runtime/AOTTypedArrayOperations.cpp:72-78`) followed by:
  - `memmove(target->typedVector() + position, source->typedVector(), count)` (`operationAOTTypedArraySetOfAnyValue`, `:266-281`), an operation with no `AOT_OPERATION_BEGIN`;
  - `tryEncodeInto(..., destination->typedVector(), destination->lengthRaw())` (`:253-263`, `:421-434`), where `latin1CharactersOf` may resolve a rope, hence allocate and collect, between taking the pointer and writing (`:202-206`, `:81-95`);
  - `operationAOTTypedArraySubarrayOfAnyValue` (`:284-308`).
- `operationAOTTextOfBytes` (`buffer.toString`): one check, then `charactersAreAllASCII(bytes)` and afterwards `String(bytes)` over the live bytes (`aot/builtins/AOTBufferBuiltinOperations.cpp:80-99`). A concurrent writer can put non-ASCII bytes into what is read as UTF-8/ASCII: wrong text, not unsafe, since it is built as Latin-1.
- `JSONRuntime::openUTF8(view->span())` copies first and classifies the copy (`aot/runtime/AOTJSONSource.cpp:335-345`, `:368-369`): the right order. `span()` itself is `{ vector(), byteLength() }`, two separate reads (`JSArrayBufferView.h:285`).
- Inline `Array.from(typedArray)`: mode, `count = m_length`, allocation (may collect), then `vector = m_vector` once, and a loop of `count` element loads (`AOTArrayBufferViewBuiltinEmitter.cpp:123-151`).
- `makeRangeWithoutMemory` builds a detached view by hand: `transferTo`, `indexingHeader()->setArrayBuffer(&buffer)`, `vm.heap.addReference` (`AOTTypedArrayOperations.cpp:354-371`).

### 7.8 Allocation

- `new Uint8Array(n)` with a number operand: `InlineBuiltin::TypedArrayWithLength` (`typedarray.builtins.ts:60-67`; `Allocates throws`, `inline-builtins.def:24`). The form is `Whole` (`AOTBuiltinFormRulesNumbers.cpp:154-159`, `:175-176`), emitted as a call of the shared stub `newTypedArray()` with (type, length as double) (`AOTArrayBufferViewBuiltinEmitter.cpp:95-103`). All other constructor overloads (buffer, typed array, iterable) are host calls (`typedarray.builtins.ts:35-59`).
- The stub, `generateNewTypedArray` (`aot/stubs/AOTStubsAllocation.cpp:544-604`):
  1. Fast path only for `1 <= length` and rounded byte size `<= 256` (`maxBytes`, `:558`, `:564-576`).
  2. The per-type record `instance->allocation.typedArrayAllocations[type]` = `{ allocator, header, elementSize }` (`aot/runtime/AOTInstance.h:162-167`, `:185`) must be filled (`:570`).
  3. Bytes: the `LocalAllocator` of `instance->allocation.primitiveAuxiliarySpace` (= `&vm.primitiveGigacageAuxiliarySpace()`, `aot/runtime/AOTInstance.cpp:223`) for the size step; `allocateCell`; zero-fill loop (`:577-586`).
  4. Cell: `allocateCell` from the record's allocator; then store header, butterfly = 0, `m_vector`, `m_length`, `m_byteOffset = 0`, `m_mode = FastTypedArray` (`:587-595`).
  5. `fenceAfterAllocation`: nothing on x86-64; on other CPUs a `storeFence` only while `instance->mutatorShouldBeFenced` (`aot/stubs/AOTStubHelpersAllocation.cpp:67-76`).
- Slow path: `operationAOTNewTypedArray` (`AOTTypedArrayOperations.cpp:51-69`) calls `newTypedArrayView<View>`, which lazily writes the per-instance record without a lock (`header`, then `elementSize` and `allocator` if null; also `structure->tryMarkPristine(vm)`) (`:35-49`), and then `View::create` with the `ConstructionContext` of 7.1.
- The allocators are `LocalAllocator`s reached through the `Instance`, which is per realm, so the free lists are shared by whoever runs in that realm (heap report).
- `tryCreateFewBytes` (TextEncoder fast path) allocates the same way in C++ (`:124-131`).
- `Buffer`: the engine half knows it only as a host class below `Uint8Array` (`BuiltinClasses::Buffer` in `aot/builtins/AOTBuiltinCallEmitter.cpp:187`). `Buffer.alloc`, `Buffer.from` and the rest are host calls in Bun's table (`bun:src/codegen/builtins/host/buffer.builtins.ts:245-260`). The instance structure is `globalObject->m_JSBufferSubclassStructure` (`:237`).
- `ArrayNarrowStorage` turns a local array of flags or small integers that does not escape into a typed array (`aot/passes/AOTArrayNarrowStorage.cpp:481-520`). Such views are thread local by construction.

### 7.9 What the AOT branch changed in these files (`git diff 8deea350d4e1 HEAD`)

| File | Change |
| --- | --- |
| `runtime/JSGenericTypedArrayViewConstructor.h` | 26 returns wrapped: `return callInt8Array;` becomes `return JSC_REGISTERED_HOST_FUNCTION(callInt8Array);` and so on. The macro returns the same pointer and records the entry point as removable when its feature is compiled out (`aot/builtins/AOTBuiltinTableGuards.h:60-87`). No change of behavior |
| `runtime/JSGenericTypedArrayViewConstructorInlines.h` | +7 lines: `new XArray(object)` takes the iterator path also when `AOT::PropertyWalk::isCompiledObject(object) && wouldHaveArrayStorage(object)` (`:267-269`) |
| `runtime/JSGenericTypedArrayViewPrototypeFunctions.h` | 12 sites (`forEach map filter find findIndex findLast findLastIndex every some reduce reduceRight sort`): `callData.type == CallData::Type::JS` becomes `CachedCall::supports(callData)`, and `CachedCall::create(globalObject, asObject(f), callData, n)`, so that compiled callees (`CallData::Type::Compiled`) get a cached call (`interpreter/CachedCall.h:50-58`). Validation logic untouched |
| `runtime/BufferMemoryHandle.cpp` | `m_maxFastMemoryCount` becomes `max(Options::maxNumWasmFastMemories(), 4096)` when `Wasm::CodeInImage::isInProcess()`. Wasm only |
| `runtime/AtomicsObject.cpp` | one line, the same `JSC_REGISTERED_HOST_FUNCTION` wrapping (`:89`) |
| `runtime/JSArrayBuffer.h` | +`offsetOfImpl()` for the sharing-mode type test |
| unchanged | `JSArrayBufferView.{h,cpp}`, `JSArrayBufferViewInlines*.h`, `JSGenericTypedArrayView.h`, `JSGenericTypedArrayViewInlines.h`, `ArrayBuffer.{h,cpp}`, `WaiterListManager.*`, `SimpleTypedArrayController.*`, `heap/GCIncomingRefCounted*`, `JSDataView*` |

### 7.10 Hazards with several mutators (typed arrays)

Memory safety:

1. **Detach/transfer against any reader or writer** (7.3.2). The bytes are freed synchronously, and readers hold a raw vector from a plain load, possibly hoisted out of a loop (7.2.7) that may have no poll. Use after free for reads and writes, in compiled code, stubs and every C++ bulk loop (7.7). This is the central problem of this section.
2. **Length/vector mismatch**: old length with a null vector is an access at `index * size`. Old length with a vector refreshed by a wasm grow is the same class.
3. **Proof `InBounds` and B3 invariance assume that only this thread's operations write `TypedArrayStorage`.** A stop-the-world handshake does not repair this by itself: polls and collections are modelled as not writing that heap, so a thread resumes with its hoisted vector and length.
4. **View created while its buffer is being detached** (7.4): never notified, dangling for good.
5. **`ArrayBuffer` refcount is not atomic**, and ordinary length queries on resizable views, `subarray`, `transferTo` ref and deref it: premature delete.
6. **Incoming reference list and `Heap::m_arrayBuffers`**: unlocked `Vector` appends and the singleton-to-vector switch, racing with each other and with `notifyDetaching`'s walk.
7. **`slowDownAndWasteMemory` run twice at once**: double adoption of an oversize block (double free), a butterfly set outside the lock.
8. **The old fast vector after Fast to Wasteful** is kept alive for other threads only by conservative scanning of their registers and stacks.
9. **`pin()` against `transferTo`**: non-atomic count, test then detach.
10. **`sort()` without a comparator on a non-shared buffer** that another thread writes: `std::sort` and the radix scatter go out of bounds. The snapshot is taken only for `isShared()`.
11. **`Atomics.wait` from two threads of one VM**: `RELEASE_ASSERT` in `notifyVMBlocking`. Without it, one intrusive `Waiter` node would be linked twice.
12. **Publication of a fresh view on ARM64**: no store fence unless the collector asks for one, so another thread can see the cell pointer before `m_vector` / `m_length` / `m_mode`. These are raw words left from the cell's previous life, not JSValues.
13. **`m_wrapper` (`Weak`) assignment race** when two threads ask for `.buffer`: WeakImpl bookkeeping.
14. **Two concurrent `resize` calls**, or `resize` against `transfer`, on one buffer: handle size, page protection and accounting diverge. The comment at `ArrayBuffer.cpp:563-564` states the assumption.
15. **Lazy fill of `instance->allocation.typedArrayAllocations[type]`**: `allocator` is the field the stub tests, and it is written after `header` and `elementSize`, but with plain stores. On ARM64 the stub could see `allocator` with a stale `header` or `elementSize`.

Crash but no corruption:

16. Shrinking `resize` against the C++ slow path or the inline BigInt store: a fault on a protected page.

Liveness:

17. A thread parked in `Atomics.wait` keeps heap access. With a shared heap, a collection requested by another thread waits for it, and the thread that would call `notify` may be the one waiting for that collection.
18. Termination and traps wake only `vm.syncWaiter()`.
19. Loops bounded by a typed array length have no poll. They are finite, but up to 2^32 iterations long, so any handshake waits that long.

Wrong JavaScript value only:

20. Stores lost when another thread converts Fast to Wasteful between the copy and the vector swing. This is observable in a program that has no data race on any element: thread A writes `ta[0]`, thread B reads `ta.buffer`.
21. `ta.buffer !== ta.buffer` across threads (two wrappers), and two different `ArrayBuffer`s for one view.
22. Atomics do not order ordinary object, variable or array accesses for the compiler: the fence range and the IR effects cover typed array bytes and `ExternalState` only. The hardware fence is there, the compiler barrier is not.
23. Plain element reads may be merged or hoisted (spin loops). Unaligned DataView/Buffer accesses and bulk copies may tear.
24. After `transfer(sameLength)`, a stale view writes into the new buffer.
25. `buffer.toString()` and TextEncoder fast paths classify the bytes and then read them again.
26. Validation builds only: `captureBytes` hashes non-shared bytes around a call and treats a change as the call's effect ("Another thread writes shared memory while the call runs", `aot/runtime/AOTOperandSnapshot.cpp:225-231`). That gives false alarms under threads.

What already holds:

- The JSType test is forever valid.
- Fixed-length SAB views never change mode, length or vector, and their buffer is `pinAndLock`ed (`ArrayBuffer.cpp:445-450`).
- Growable SABs are handled with a lock and an atomic size, and compiled code always leaves them to C++.
- `WaiterListManager`'s lists are properly locked.
- No code is patched and no watchpoint is consulted by compiled code for typed arrays, so nothing has to be invalidated in code on detach. The whole contract sits in four words of the view.

### 7.11 Not found / not sure

- **Interpreter.** I did not read `llint/LowLevelInterpreterAOT.asm` for `get_by_val` / `put_by_val` on typed arrays (covered by the interpreter section).
- **B3 loop passes.** I read the policy flags, the LICM condition and the headers of `B3LoopRangeChecks` / `B3LoopVersionPlan` / `B3MemoryNumbering`, not their bodies, and I inspected no generated code. "Mode, length and vector are hoisted" follows from the heap ranges and `B3HoistLoopInvariantValues.cpp:298-299`. LICM also has limits I did not evaluate: `MaxHoistedLoadsPerLoop`, register budgets, and control dependence (the vector load sits behind the mode branch).
- **`EliminateGuards`, `PlaceTypeTests` hoisting** (`aot/passes/AOTPlaceTypeTestsHoisting.cpp`), `HoistTestsIntoPredecessors`: not read. I do not know how far in front of its access an `IsInsideView` can be moved, only what ends its proof.
- **Table to heaps.** I did not trace how the generator maps `b.writesContents()` / `changesState` on `ArrayBuffer` to IR heaps. I rely on `stateHeap: storage` and the slot declarations.
- **Test for `Buffer`.** Which `CellTestKind` a `TypeTest` for `Buffer` (as opposed to `Uint8Array`) compiles to: not traced.
- **Refused functions.** I saw only `m_isRefused = true` (`AOTDirectCodeGenerator.cpp:520-524`), not the caller that presumably sends the function through B3/Air.
- **Allocator.** Whether Bun's AOT build defines `BUSE_MIMALLOC` (hence Gigacage off) and which allocator `Gigacage::free` ends in: not verified. `wtf/Gigacage.h` is not present in this worktree's `WTF/wtf`.
- **Fault handler.** What `image/AOTFaultHandler` does with a fault at `null + index` or in a protected resizable page: not read.
- **Bun-side users of views** (`postMessage` transfer, `Blob`, file and socket I/O that keep a raw pointer or `pin()` during asynchronous work, `Buffer.allocUnsafe` pooling): outside the engine half. They hold raw vectors across event-loop turns already.
- **NaN purification** in `SlotEmitter::load` for Float32/Float64 elements: not read.
- **The memory model of ECMAScript** (which accesses may tear): not checked against the standard here.

---------------------------------------------------------------------------------------------------------------------------------------

## 8. The interpreter path (LLInt in an AOT process)

Paths are relative to `Source/JavaScriptCore/` unless they start with `bun:` (= `<bun>/`) or `WTF:` (= `.../Source/WTF/wtf`). "AOT.asm" = `llint/LowLevelInterpreterAOT.asm`, "LLI.asm" = `llint/LowLevelInterpreter.asm`, "LLI64.asm" = `llint/LowLevelInterpreter64.asm`, "Slow.cpp" = `llint/LLIntSlowPaths.cpp`. "Ordinary handler" = the upstream handler of the same opcode.

Short version: the AOT handlers are a *speed* layer (25 opcodes and one trampoline). They do not remove the per-bytecode caches. Every LLInt inline cache of upstream JSC (get_by_id, put_by_id, scope ops, call link infos, private names, to_this ..) is still there, still written by slow paths with plain stores under `CodeBlock::m_lock` (a lock that the asm readers never take), and the AOT handlers add three new kinds of shared mutable state: probes of the per-VM hashed property table, `LocalAllocator*` pointers cached in metadata/plans/Instance and bumped in line, and metadata words rewritten at link time or by slow paths (`ObjectLiteralPlan`, fused `m_resolveType`).

### 8.1 What `LowLevelInterpreterAOT.asm` replaces, how, when

**When.** Once per process, in `LLInt::initialize()`, called from `JSC::initialize()` (`runtime/InitializeThreading.cpp:128`), before the opcode maps are frozen read-only.
- `llint/LLIntData.cpp:201` `llint_entry(...)` fills the trampoline entries; `:206` calls `installBytecodeInterpreter` (through `callRemovable<RemovableFeature::Interpreter,...>`; in a `--trim`med executable `:203` uses `ReturnDefault`, i.e. the interpreter may be absent).
- `llint/LLIntData.cpp:75-84`:
  ```cpp
  llint_bytecode_entry(opcodeMap, opcodeMapWide16, opcodeMapWide32);
  #if ENABLE(AOT)
      if (Options::runsCompiledImage())
          llint_aot_entry(opcodeMap, opcodeMapWide16, opcodeMapWide32);
  ```
  `Options::runsCompiledImage()` is `useAOT()` (`runtime/Options.h:153`). `useAOT` also forces `useJIT = false` (`runtime/Options.cpp:805-815`).
- `llint/LLIntData.cpp:217-220` then `permanentlyFreezePages(..., ReadOnly)`. After that the maps are immutable: **the dispatch tables themselves are not a threads problem.**
- `entry(llint_aot, ...)` is AOT.asm:1788-1825; for each opcode `setEntryAddresses` stores three addresses: narrow, wide16, wide32 (AOT.asm:1789-1793). The asm is included only `if AOT` (LLI.asm:2302-2304).
- The handlers have no embedded OpcodeID (`interpreter/Interpreter.cpp:494-495` skips the debug assertion for that reason).

**The exact list** (AOT.asm:1795-1824), all three widths each:
`op_add`, `op_call` (not on ARM64E), `op_create_lexical_environment`, `op_enumerator_get_by_val`, `op_enumerator_next`, `op_get_by_id`, `op_get_by_val`, `op_get_length`, `op_in_by_id`, `op_in_by_val`, `op_jfalse`, `op_jnstricteq`, `op_jstricteq`, `op_jtrue`, `op_mov`, `op_new_array`, `op_new_object`, `op_new_promise`, `op_nstricteq`, `op_put_by_id`, `op_resolve_scope`, `op_stricteq`, `op_to_this`, `op_typeof_is_function`, `op_typeof_is_object`; plus the narrow entry of the helper `llint_virtual_call_trampoline` (AOT.asm:1823-1824; `VM::getCTIVirtualCall()` reads that entry, `runtime/VM.cpp:1157-1159`).
Also, outside the maps: `String.prototype.charCodeAt`'s host entry is `llint_aot_char_code_at_thunk` (AOT.asm:1739-1784, installed by `runtime/VM.cpp:1002-1013`); it reads only the caller's arguments and immutable string data.

**Everything else keeps the ordinary handler.** In particular (this answers part of the question list): `get_by_id_direct`, `get_by_id_with_this`, `get_private_name`/`put_private_name`, `set/check_private_brand`, `put_by_val(_direct)`, `instanceof`, `call_ignore_result`, `construct`, `super_construct`, `tail_call`, all `*_varargs`, `call_direct_eval`, `iterator_open/next`, `async_iterator_*`, `get_from_scope`, `put_to_scope`, `create_this` (a plain `slowPathOp`, LLI.asm:2317), `enter`, `loop_hint`, `check_traps`, `catch`, `ret`, all arithmetic but `add`, `jneq_ptr`, `new_array_buffer`, `new_array_with_size`, `new_func*`, `new_reg_exp(_shared)`, `switch_*`, `profile_type`. There is no `try_get_by_id` in this tree (no match in `llint/` or `BytecodeList.rb`). "No bytecode is added" (PR text); the `BytecodeList.rb` diff only adds `asm_section` tags.
Many AOT handlers are a prefix and fall into the ordinary one: `jumpToOrdinaryHandler` (AOT.asm:170-172) is used by `put_by_id`, `create_lexical_environment`, `new_promise`, `to_this`, `resolve_scope`, `stricteq` family, `add`, `get_length`.

**Two link-time consequences under `useAOT`:** metadata tables always use 32-bit offsets and an aligned first entry (`bytecode/UnlinkedMetadataTableInlines.h:38-45`, read by `metadataImpl`, AOT.asm:83-89), and the bytecode generator emits object literals as "all values first, then `new_object`, then direct `put_by_id`s" (`bytecompiler/NodesCodegen.cpp:686-...`) so that `op_new_object` can swallow the stores.

### 8.2 The replaced handlers, step by step, with what they read and write

**How the hashed table is probed from asm** (used by get_by_id, get_by_val, in_by_*, enumerator_get_by_val, put_by_id):
1. `loadInstanceOfRealm` (AOT.asm:119-124): `CodeBlock[cfr] -> m_globalObject -> JSGlobalObject::m_aotInstance`; null (a `node:vm`/ShadowRealm realm) means slow path.
2. `findHashedSlot` (AOT.asm:136-161): `offset = ((structureID * 0x9e3779b1) >> 11) + (uid << 1)`, `andq InstanceHashedSlotOffsetMask[instance]` (:143), `addp InstanceHashedSlots[instance]` (:144). Mask first, base second; the base never moves (`aot/runtime/AOTPropertyCachesHashedTable.h:30` "it never moves"), the mask grows (`AOTPropertyCaches.cpp:144-145` republishes both words per Instance).
3. Four ways (`xorp` of 32, 96, 32 bytes). For each: compare the **key word** at +16 (`uid | SlotAccess`, `AOTPropertyCachesWords.h:27,44`), then the **StructureID** at +0 (`AOTRuntimeRecords.h` `offsetOfStructureID() == 0`). That pair is all that makes an entry valid (`HashedSlot::matches`, `AOTPropertyCachesWords.h:39`). No generation, no sequence number.
4. After a match the handler loads, with separate loads, `kind` (+4), `flags` (+5), `location` (+6), `payload` (+8, 8 bytes; or two 4-byte halves).
The writer is `HashedSlotTable::write()` -> `RememberedSlot::set` = `m_value = value` (plain 32-byte struct copy, `heap/RememberedSlot.h:125-129`, `AOTPropertyCachesHashedTable.cpp:122-132`); eviction is `erase` = `m_value = T{}` then `uid->deref()` (`:134-140`), victim chosen by `m_nextVictimWay++` (`:117`); `grow()` copies entries to the upper half and clears the lower (`:187-211`); the collector erases dead entries at the end of a collection (`:262-284`). None is atomic or ordered.

| Opcode | Steps | Reads (shared) | Writes (shared) |
| --- | --- | --- | --- |
| `get_by_id` AOT.asm:497-627 | (1) `mode == Default`? (:503) (2) `structureID == defaultMode.structureID` (:504-505) (3) load `cachedOffset` (:506): `< firstOutOfLineOffset` -> in-cell load, an empty word means "field of a compiled object is absent" -> table; `>= 1<<30` (`narrowReferenceInSiteCache`) -> 32-bit load at `cell + (offset - 1<<30)`, shift, add cage base (:535-542); `>= 1<<29` (`extraPropertyInSiteCache`) -> hidden-word offset in the high half, word index in the low half (:522-532); else butterfly load (:516-520). (4) other modes as upstream: ProtoLoad (:547-561, three loads: structureID, cachedOffset, cachedSlot), ArrayLength, Unset. (5) miss: `.probe` / `.compiledObject` -> hashed table (`getFromCompiledObject` :441-472, `getFromHashedSlot` :408-438). (6) `_llint_slow_path_get_by_id`. | `GetByIdModeMetadata` (16 bytes: `structureID`,`cachedOffset`,`cachedSlot`/`mode`/`hitCount`, `bytecode/GetByIdMetadata.h:41-100`), hashed table, `CompiledStructure` slots, `Instance.cellCageBase`, `CustomAccessorRecord` table (:417-425) | none in asm (no value profile). Slow path writes metadata, see 8.3 |
| `get_by_val` :629-646 | the ordinary body as a macro (`getByVal`, LLI64.asm:1823-1904) with a new slow tail: compiled object + atom/symbol key (`loadNameOfKey` :395-405) -> hashed table | as above | **still writes** `m_arrayProfile.m_lastSeenStructureID` (LLI64.asm:1854), `m_arrayProfileFlags` (:1843-1845) and the value profile bucket (:1827,:1896): the macro is shared |
| `in_by_id`, `in_by_val` :648-689 | object? -> `testPresenceInObject` (:475-495): table probe, kind -> present/absent, `Field` -> look at the slot | hashed table | none |
| `enumerator_next` :692-722 | ordinary macro, then for a compiled object: enumerator's `m_cachedStructureID`, `m_cachedOrderWord(Mask)` against the object's order word | enumerator cell, object order word | frame only |
| `enumerator_get_by_val` :724-745 | ordinary macro (`enumeratorGetByVal`, LLI64.asm:3791-3831), then table | as above | `m_enumeratorMetadata` RMW byte (LLI64.asm:3797-3799), value profile (AOT.asm:734, LLI64.asm:3826) |
| `put_by_id` :763-989 | (1) the site cache exactly as upstream: `m_oldStructureID` (:770-771), `m_newStructureID` (:772), `m_structureChain` walk (:774-788), then reload `m_newStructureID` and `m_offset` (:794-795), **store StructureID, then the value** (:796-798), one barrier. Replace: `m_offset` (:806). (2) compiled object: probe with `uid|Store`, kinds `Field`/`FieldCreation`; frozen bit of the order word (:837); `locateField`; `RuntimeType` from the image tables (:839-843); tag tests; check "slot not absent" then store (e.g. :868-869, :872-874, :900-902): check-then-act on the field itself | `OpPutById::Metadata` (4 words), hashed table, read-only image tables | the object; no metadata |
| `call` :1682-1698 | ordinary `callHelper` (LLI64.asm:2525-2578) with `prepareCallSiteForConstruct` (= no array profile) and `dispatchAfterCallWithoutProfile` (no value profile, AOT.asm:1670-1675). Loads `LazyCallLinkInfo::m_data` (:2544), `m_callee` (:2547), compares, `m_codeBlock` -> callee frame's CodeBlock slot (:2559-2560), `m_monomorphicCallDestination` (:2564), call | `CallSiteData`/`CallLinkInfo` (3 words read separately) | frame only. Slow paths rewrite the CallLinkInfo, 8.3 |
| virtual call trampoline :1709-1728 | callee type `CompiledClosureType` -> sign of the closure word (class?) -> `Structure::m_realm -> m_aotInstance -> stubs.closureEntrypoint`; slim closures likewise; else ordinary | closure word, Instance stub words (written once, `aot/stubs/AOTStubEntrypoints.cpp:72-75`) | none |
| `resolve_scope` :1111-1146 | `m_resolveType & 0xff == ClosureVar`? walk `m_localScopeDepth` scopes; if the upper bits hold a fusion (`bytesToSkip`, variable offset) also load the variable, store it into the *next* instruction's `m_dst` and skip `get_from_scope` (+`check_tdz`) | `m_resolveType`, `m_localScopeDepth` | frame only. The fusion is written once at link time by `fuseReadsOfClosureVariables` (`aot/runtime/AOTInterpreterSlowPaths.cpp:629-650`, called from `bytecode/CodeBlock.cpp:877-880`), before the CodeBlock is published |
| `new_object` :1149-1225 | `m_objectAllocationProfile.m_allocator` word: tag bit 1 = `ObjectLiteralPlan*`, bare tag = "not planned yet" (set at link, `CodeBlock.cpp:492-495`). No plan: `allocateCell`, header from `m_structure`. Plan: allocator, header, second word, cleared words and value offsets come from the plan; then it decodes the following `put_by_id`s itself and stores the operands | metadata word, **plan contents (mutable, see 8.3)**, `LocalAllocator` | **bumps a `LocalAllocator` in line** |
| `new_array` :1228-1341 | bad-time test on an Instance word; `ArrayAllocationProfile` as one 64-bit word (:1235), reads the last array's indexing type through the untraced pointer (:1241); two `allocateCell`s (butterfly from `Instance.allocation.auxiliarySpace`, cell from `Instance.arrayAllocator`) | profile word, Instance, global object's structure table | allocators; **RMW of the profile word** (:1331-1335: load, keep top 16 bits, or in the new array, store) |
| `create_lexical_environment` :1025-1067 | SymbolTable singleton must be thin-invalidated (:1029; private members read by offset, checked by `checkPlacesInSymbolTable`, Slow.cpp:841-844), size class -> `Instance.allocation.cellSpace` allocator | SymbolTable, Instance | allocator |
| `new_promise` :1080-1095, `add` (rope) :1538-1583 | allocator + header from Instance words | `Instance.allocation.inlineAllocations[Promise]`, `ropeStringAllocator`, `stringHeader` | allocator. `add` writes **no** arith profile on its own paths |
| `to_this` :1098-1108 | strict + non-cell: nothing to do; else ordinary | operands | none |
| `get_length` :1640-1666 | string / array length in line; else ordinary | objects | none on the fast paths |
| `mov`, `jtrue/jfalse`, `(n)stricteq`, `j(n)stricteq`, `typeof_is_*` | pure functions of operands and immutable string data | - | none (slow tails bump a counter, 8.3) |

`allocateCell` (AOT.asm:993-1011) is `FreeList::allocateWithCellSize` in asm: load `intervalStart`, compare with `intervalEnd`, maybe pop `nextInterval`, store `intervalStart += cellSize`. No atomics. The ordinary LLInt handlers never allocate in asm (all of `new_*`/`create_*` are `slowPathOp`/`llintSlowPathOp`, LLI.asm:2316-2377): **inline allocation from asm is new with the AOT handlers**, and the `LocalAllocator*` is reached through state that all threads of a VM would share (metadata word, plan, Instance).

`fenceAfterAllocation` (AOT.asm:1014-1022) and `writeBarrierOnCell` (:748-759) read `VM::heap` words via `CodeBlock::m_vm`.

### 8.3 Metadata and profile writes that remain in an AOT process

The header comment "Nobody reads a value profile here, so none is written" (AOT.asm:13) is true only for the handlers' own paths. `useLLIntICs` stays `true` (`runtime/OptionsList.h:740`), so all slow-path caching is on.

**A. Caches that are read back by handlers (a race can matter).** "Lock" = `ConcurrentJSLocker(codeBlock->m_lock)`; it orders writers and the collector, never the asm readers.

| Opcode(s) | Field(s), writer | Reader | Torn/stale read |
| --- | --- | --- | --- |
| `get_by_id`, `get_length`, `instanceof` (x2), `iterator_open`, `iterator_next` (x2), `async_iterator_open` | `GetByIdModeMetadata`. `performLLIntGetByID` Slow.cpp:1044-1064: under lock `clearToDefaultModeWithoutCache()` (mode, structureID=0, offset=0), then `structureID`, then `cachedOffset` (:1053-1054). `setupGetByIdPrototypeCache` :979-987 -> `setProtoLoadMode` (structureID, offset, then the 64-bit `cachedSlot` that also flips `mode`, `GetByIdMetadata.h:128-149`). `setArrayLengthMode()` :1064 **without lock**. `--hitCountForLLIntCaching` :1060 without lock. AOT only: `cacheFieldOfCompiledObject` Slow.cpp:1071-1087 (lock; `structureID` then `cachedOffset`, or `mode = Unset`). Cleared by watchpoints (`LLIntPrototypeLoadAdaptiveStructureWatchpoint::clearLLIntGetByIdCache`) and by the collector (`CodeBlock.cpp:1881-1914`) | AOT.asm:503-577; `performGetByIDHelper` LLI64.asm:1646-1689 | **Memory safety.** structureID is published *before* the offset: a second thread can match the new structureID and use the old/zero offset (in-cell word 0 of another shape; for a compiled object possibly a raw int/double or two packed 32-bit references read as a JSValue), or match the old one and read the new offset. In `get_by_id` the offset is also a *format tag* (`1<<29`, `1<<30`), so a torn pair changes how the word is decoded. ProtoLoad: `mode` byte overlaps `cachedSlot`: reader loads mode, structureID, offset, slot at four different times |
| `get_by_id_direct` | `m_structureID`, `m_offset`; cleared **without lock** Slow.cpp:905-906, set under lock :910-912 | LLI64.asm:1626-1630 | memory safety (same pair) |
| `get_private_name` | `m_structureID`,`m_offset`,`m_property` Slow.cpp:1483-1493 | LLI64.asm:1918-1936 | memory safety |
| `put_by_id` | clear of four words **without lock** Slow.cpp:1297-1300; set under lock :1321-1327 (old, offset, new, chain) or :1340-1342 | AOT.asm:770-808, LLI64.asm:1754-1808 | **Memory safety, writes.** e.g. old=S matched, `m_newStructureID` still 0 from the clear -> treated as a replace at the new `m_offset` (slot does not exist yet: out-of-bounds store in the butterfly); or new structure stored with a stale offset |
| `put_private_name`, `set_private_brand`, `check_private_brand` | Slow.cpp:1621-1661, :1696-1703, :1734-1735 | LLI64.asm:1943-2025 | memory safety for put/set (stores a StructureID into the cell); wrong brand answer for check |
| `resolve_scope` | `slow_path_resolve_scope` `runtime/CommonSlowPaths.cpp:1476-1485` (lock): `m_resolveType`, then the **union** word `m_globalObject`/`m_globalLexicalEnvironment` (shares storage with `m_symbolTable`, `m_constantScope`, `m_moduleImportSlot`; `BytecodeList.rb:538-556`), then the epoch (union with `m_localScopeDepth`). `notifyLexicalBindingUpdate` `CodeBlock.cpp:3682-3718` | LLI64.asm:2883-2975, AOT.asm:1112-1117 | memory safety: type says `GlobalProperty`, pointer word still holds the previous member of the union (for `UnresolvedProperty` sites it is what linking left there); returned as a scope object |
| `get_from_scope` | `tryCacheGetFromScopeGlobal` `runtime/CommonSlowPathsInlines.h:124-169`; `cacheGlobalLexicalVar` `:42-61` (stores at :57-60) writes `m_getPutInfo` **first**, `m_operand` (now a raw `JSValue*`) **last** | LLI64.asm:3013 (type byte), :2989/:2996/:3004 (`m_operand`) | **Memory safety.** `m_operand` means "property offset", "scope offset" or "address" depending on the type byte. Between the two stores another thread does `loadq [offset]` (wild read) |
| `put_to_scope` | same functions (`:63-122`); additionally `m_structureID` and `m_watchpointSet` are a union (`BytecodeList.rb:586-594`) | LLI64.asm:3075-3203 | **Memory safety, wild write** (`storeq t1, [t0]`, :3090-3091) |
| `to_this` | `m_cachedStructureID`, `m_toThisStatus`, no lock, `CommonSlowPaths.cpp:287-306` | LLI64.asm:910-917 | none: each word is used alone; a hit only skips work that is a no-op for that structure |
| call family | see below | | memory safety |
| `new_object` | allocator word: `didLinkNewObject` (link), `planObjectLiteral` (`AOTInterpreterSlowPaths.cpp:657-688`: check `== tag`, store a plain allocator, later store `plan|tag`); **plan contents are overwritten in place** by `didOutgrowCell` -> `fillPlanForFinalObject(..., *entry.plan)` = `result = plan` (`:588-607`, `:496`; comment ":602 The sites hold the address of the plan") | AOT.asm:1151-1209, Slow.cpp:1825-1840 | **Memory safety.** A reader mixes the old `allocator` (small cell) with the new `cellHeader`/`numberOfClearedWords`/`valueOffsets` (roomier cell): stores past the cell |
| `new_array`, `new_array_with_size`, `new_array_buffer`, `new_array_with_species` | `ArrayAllocationProfile` one word; asm RMW AOT.asm:1331-1335; C++ `updateLastAllocation`/`updateProfile`; collector `updateAllArrayAllocationProfilePredictions` | AOT.asm:1235-1243 | lost update only for the word itself (a hint; `bytecode/ArrayAllocationProfile.cpp:36-43` "awkwardly racy but totally sound"). But `updateProfile()` (mutator slow path via `selectIndexingType`, and GC end) reads `lastArray->getVectorLength()` and its comment says why that is single-thread only: ":50-53 vector length can be accessible only from the main thread because large butterfly can be realloced in the main thread". With several mutators the last array may belong to another thread: **stale/freed butterfly read (M, read only)** |
| `new_reg_exp_shared` | `m_cachedObject` (`RegExpObject::literalAsReceiver`, Slow.cpp:824-830) | LLI64.asm:3627-3660 | one RegExpObject per site handed to every evaluation, hence to every thread. Only for literals without `g`/`y` (`bytecompiler/NodesCodegen.cpp:190-196`), so `lastIndex` is not written; two threads creating it: last one wins, harmless. RegExp match state itself is R7's RegExp section |
| `create_this`, `create_promise`, `create_generator`, `create_async_generator`, `super_construct(_varargs)` | `m_cachedCallee` (`CommonSlowPaths.cpp:218-224`, `:246-252`; asm LLI.asm:2566-2572, LLI64.asm:3235-3241) | only a JIT would; the collector clears it (`CodeBlock.cpp:2008-2036`) | none |
| `iterator_open/next`, `async_*`, `iterator_close_check` | `seenModes` RMW (LLI64.asm:3488-3490 .., `CommonSlowPaths.cpp:805-906`) | `canUseFastIterationMode(seenModes, mode)` `CommonSlowPaths.cpp:799,974` | lost bit -> other (equivalent) path; value-safe as far as read |
| `catch` | `m_hasExecutedWithoutBuffer`, `CodeBlock::m_hasCatchThatExecutedWithoutBuffer` (`CodeBlock.cpp:2504-2509`; `useLazyCatchLiveness` default true, so no buffer is ever made without a JIT) | tier-up only | none |

**Call link infos** (`op_call` AOT, all other call ops ordinary):
- A site starts at one of three per-VM shared `CallSiteData` (`bytecode/CallLinkInfo.h:350-359`, `VM::neverExecutedCallSiteData()` .. `runtime/VM.h:1098-1100`). First call: `handleUnlinkedCall` (Slow.cpp:686-737) `setExecutedOnce` (plain store of `m_data`). Second call: `ensureSlow` (`CallLinkInfo.cpp:325-338`: `new CallSiteData`, `storeStoreFence`, `m_data = data`) and `linkFor`.
  Check-then-act: `RELEASE_ASSERT(!lazyCallLinkInfo.get())` (Slow.cpp:713). A thread that loaded the shared `m_data` just before another thread installed the site's own data arrives here and **crashes on the assertion**; if both pass it, both allocate and one `CallSiteData` leaks while a caller may still be using it.
- Linking: `setMonomorphicCallee` writes `m_callee`, `m_codeBlock`, `m_monomorphicCallDestination`, `m_mode` in that order (`CallLinkInfo.cpp:141-149`); `clearCallee`/`reset`/`setVirtualCall`/`setStub` rewrite the same three words (`:151-157`, `:351-383`, `:401-417`). The reader loads callee, then codeBlock, then destination (LLI64.asm:2547-2564). **Memory safety**: new callee + old/null destination = jump to 0 or to another function's prologue with the wrong `CodeBlock*` in the frame.
- `linkMonomorphicCall` -> `calleeCodeBlock->linkIncomingCall` pushes on `CodeBlock::m_incomingCalls`, an unlocked `SentinelLinkedList` (`CodeBlock.cpp:2623-2628`); `installCode` walks it (`ScriptExecutable.cpp:229-230`). Memory safety.
- Polymorphic: works without a JIT (`bytecode/Repatch.cpp:281-286` picks `llint_polymorphic_*_call_trampoline`), which scans `CallLinkInfo::m_stub`'s `CallSlot`s (LLI.asm:2841-2890) while `setStub` -> `clearStub` drops the previous routine (`CallLinkInfo.cpp:90-99,401-405`).
- `*_varargs` have the `DataOnlyCallLinkInfo` inline in metadata (`BytecodeList.rb:100-195`), plus `updateMaxArgumentCountIncludingThisForVarargs` (Slow.cpp:2530).
- The collector unlinks dead callees at the end of a collection (`CallLinkInfo::reconcileWeakReferencesAtGCEnd`, `CallLinkInfo.cpp:181-228`; world stopped).

**B. Pure profiles (written, never read by a handler).** All are single plain stores or non-atomic RMW of a byte/halfword/word. Race = lost sample only.
- Value profile buckets, `storeq` at a negative index of the metadata table (`valueProfile`, LLI64.asm:77-81): every ordinary `llintOpWithProfile`/`return profiled` op: `get_argument`, `to_object`, `get_prototype_of`, `get_from_arguments`, `get_internal_field`, `get_by_id_direct`, `get_private_name`, `get_from_scope`, `to_this` (:922), all calls but AOT `op_call`, `iterator_*`, `instanceof`, `get_by_val`, `enumerator_get_by_val`; and `LLINT_RETURN_PROFILED`/`RETURN_PROFILED` in every slow path (also `slow_path_get_by_id`, Slow.cpp:1106).
- `ArrayProfile::m_lastSeenStructureID` / `m_arrayProfileFlags`: `get_by_val`, `put_by_val(_direct)` (LLI64.asm:2040-2127), `get_length` ordinary (:1712), `in_by_val` slow path, `enumerator_*`, `iterator_open/next`, `arrayProfileForCall` (:2516-2522, also into the VM-shared `CallSiteData`).
- `UnaryArithProfile`/`BinaryArithProfile`: `orh` into vectors of the **UnlinkedCodeBlock** (LLI.asm:3033-3046): `inc/dec`, `to_number`, `to_numeric`, `negate`, `mul/sub/div/..`, `add` when it falls to the ordinary handler; C++ `CommonSlowPaths.cpp:403-728`.
- `jneq_ptr m_hasJumped` (LLI64.asm:2286), `m_enumeratorMetadata`, `EnumeratorMetadata |=`.
- Argument value profiles are **not** allocated without a JIT (`CodeBlock.cpp:1404`; asm tests the null storage, LLI.asm:1632-1633).
- `profile_type`/`profile_control_flow` only exist when the type/control-flow profiler is on (`VM::m_typeProfilerLog` cursor, LLI64.asm:3247-...): per-VM log cursor, memory safety if ever enabled.
- Who reads B in an AOT process: **the collector**. `isBaselineCode(InterpreterThunk)` holds, so marker threads run `updatePredictionsConcurrently` (`CodeBlock.cpp:1503-1507`) and the end phase runs `updateAllPredictions` (`:2159-2160`), which decode buckets and `m_lastSeenStructureID` and clear dead samples (`:3464-3490`, `:3516-...`). That design already tolerates one racing mutator; more mutators add nothing new.

**C. Execution counters.** `UnlinkedCodeBlock::m_llintExecuteCounter.m_counter` is bumped with a non-atomic `baddis` in every prologue (+5, LLI.asm:1507-1508), `loop_hint` (+1, LLI64.asm:481-519 via `checkSwitchToJIT` LLI.asm:1444-1450), `ret`/`tail_call` (+10, LLI.asm:1452-1458). It starts at `-thresholdForJITAfterWarmUp` (`bytecode/UnlinkedCodeBlock.cpp:86`); on crossing zero the slow path finds `!shouldJIT` and calls `dontJITAnytimeSoon()` -> `deferIndefinitely()` (Slow.cpp:448-450, 519-521, 558-561; `CodeBlock.h:637-640`). The counter lives in the *unlinked* block, so it is shared by all CodeBlocks/realms of that code. It **does have a reader**: CodeBlock aging (8.5). Race = lost increments / a spurious slow-path call: harmless.

**D. AOT-side counters and tables touched from interpreter slow paths.** `count(vm|instance, RuntimeCounterID::Interpreter*SlowPaths)` (`AOTInterpreterSlowPaths.cpp:326,414,431,690-698`; `willRunInInterpreter`, `AOTInterpreterTripwire.cpp:25`) are plain `++` on a per-VM or per-Instance array (`aot/runtime/AOTRuntimeCounters.cpp:58-67`): lost counts only. `tryGetById/…` call `PropertyCaches::findByKey`, `getAndCache`, `putAndCache`, `hasPropertyAndCache`, `caches.cache(...)`, `shouldFile` (`:165-176`, `:228-236`, `:291-311`, `:389-399`): these **write** the per-VM tables (hashed table, miss counters). `findByKey` returns a `const Slot*` *into the table* that is then read field by field (`readBySlot`, `:118-163`) and even across calls that run JS (`callGetter`, `:124`).

**E. Per-CodeBlock C++ containers mutated at run time:** `m_llintGetByIdWatchpointMap.add` (Slow.cpp:962,976; no lock), `createRareDataIfNecessary` (`CodeBlock.h:1017`), lazily made FunctionExecutables (`materializeFunctionExecutable`, `CodeBlock.cpp:908-918`: `ASSERT(!slot)`, `slot.set`, `m_numberOfUnmaterializedFunctionExecutables--`; "mutator only", `CodeBlock.h:1090`): two threads at the same `new_func_exp` make two FunctionExecutables (observable only through caches keyed by executable; counter underflow), template objects (`ScriptExecutable::createTemplateObject`, `ScriptExecutable.cpp:467-484`: unlocked map; two threads could get **different template objects for one site = wrong JS value**, plus HashMap corruption).

### 8.4 From text to running bytecode

Entry points:
- Indirect `eval`: `globalFuncEval` (`runtime/JSGlobalObjectFunctions.cpp:457`), `evalEnabled` test :496, JSON shortcut :509, `IndirectEvalExecutable::tryCreate` :517 -> `vm.codeCache()->getUnlinkedEvalCodeBlock` (`runtime/IndirectEvalExecutable.cpp:58`), `executeEval` :522.
- Direct `eval` from bytecode: `op_call_direct_eval` -> `commonCallDirectEval` (Slow.cpp:2556-2582) -> `eval()` (`interpreter/Interpreter.cpp:224`) -> `evaluateDirectEvalAt` (:127). From compiled code: 8.7.
- `new Function`: `constructFunction` (`runtime/FunctionConstructor.cpp:203` evalEnabled) -> `constructFunctionSkippingEvalEnabledCheck` :218 -> `FunctionExecutable::fromGlobalCode` :225 -> `CodeCache::getUnlinkedGlobalFunctionExecutable` (`runtime/CodeCache.cpp:272`). The body's bytecode is made at the first call.
- `node:vm`: `bun:src/jsc/bindings/NodeVMScript.cpp:212` (`getUnlinkedProgramCodeBlock`) and `:223` `JSC::evaluate` -> `Interpreter::executeProgram` (`Interpreter.cpp:1491`).

Stages and the state each touches:
1. **Parser.** `parse<>()` (`parser/Parser.h:2281`) builds a `Parser` on the C++ stack; `ParserArena`/`IdentifierArena` are members of it (thread-private). But: the constructor calls `vm.addSourceProviderCache(source.provider())` (`parser/Parser.cpp:151`) = unlocked `HashMap` `VM::sourceProviderCacheMap` (`runtime/VM.h:822`, `VM.cpp:1284-1291`), pruned/cleared by `pruneUnusedSourceProviderCaches` (:1293) and `shrinkFootprintNow` (:1267). The `SourceProviderCache` itself is shared by all parses of that provider.
2. **Identifiers.** `IdentifierArena::makeIdentifier` -> `Identifier::fromString` -> `AtomStringImpl::add`, which uses `Thread::currentSingleton().atomStringTable()` (`WTF:text/AtomStringImpl.cpp:76-79`); the locker is empty unless `USE(WEB_THREAD)` (:66-70). `runtime/Identifier.cpp:77` asserts `vm.atomStringTable() == Thread::currentSingleton().atomStringTable()`. Image atoms are looked up first through `AtomStringTable::readOnlyAtomLookup()` (:81-107, read-only). So a second JS thread must install the VM's table as its current table, and then every `add`/`remove` on it races (**memory safety**; also atom *identity* = property-key identity, so two tables are not an option). Also `uid->ref()/deref()` in the hashed table and `Identifier` copies: `StringImpl` refcounts are non-atomic.
3. **CodeCache** (`VM::m_codeCache`, `VM.h:1455`): `CodeCacheMap` = `HashMap` + `m_size/m_age` counters, **no lock** (`runtime/CodeCache.h:81-240`); `findCacheAndUpdateAge` writes even on a hit (`:131-132`). Used by indirect eval, programs, modules, `new Function` (`CodeCache.cpp:226,235,284,328`) and `updateCache` (`bytecode/UnlinkedFunctionExecutable.cpp:86`). Direct eval does not use it (`CodeCache.cpp:160-163`). Values are `Strong<>` handles (HandleSet: heap section).
4. **Bytecode generation.** `BytecodeGenerator::generate` (`CodeCache.cpp:146`, `UnlinkedFunctionExecutable.cpp:82`) allocates GC cells (UnlinkedCodeBlock, UnlinkedFunctionExecutable, SymbolTable, constants) and uses `vm.m_compactVariableMap->get(...)` (`CodeCache.cpp:145`; `CompactTDZEnvironmentMap` is `RefCounted` with an unlocked map, `parser/VariableEnvironment.h:437`). Handles un-register in destructors, i.e. also when cells are swept.
5. **`DirectEvalCodeCache`.** Key = (source `StringImpl*` contents, call site) (`bytecode/DirectEvalCodeCache.h:54-86`), max 64 entries. `set`/`clear`/`visitAggregate` take `m_lock`; **`get` does not** (`DirectEvalCodeCacheInlines.h:36-39`), and `set` tests `m_cacheMap.size()` outside (`.h:131-135`). Owner: the caller's `CodeBlock::m_rareData` for bytecode callers (`CodeBlock.h:619`), for compiled callers a per-Instance cell (8.7). Get-miss-create-set is check-then-act: two executables for one key, last wins (harmless); get during a rehash: memory safety.
6. **`UnlinkedFunctionExecutable::unlinkedCodeBlockFor`** (`UnlinkedFunctionExecutable.cpp:254-306`): test slot, generate, `m_unlinkedCodeBlockForCall.set`, `unlinkedFunctionExecutableSpaceAndSet.set.add(this)` ("FIXME GlobalGC: Need syncrhonization here", :303). `takeParentDeclaredNames()` (:79) *moves* data out of the executable: a second concurrent generation sees it gone (not read what that changes).
7. **`ScriptExecutable::prepareForExecution`** (`bytecode/CodeBlock.h:1120-1137`): `if (hasJITCodeFor(kind)) return codeBlockFor(kind)`; else `prepareForExecutionImpl` (`ScriptExecutable.cpp:405-455`): `DeferGCForAWhile`, `refusesToInterpret` (8.9), `newCodeBlockFor` with `RELEASE_ASSERT(!executable->m_codeBlock)` / `RELEASE_ASSERT(!executable->codeBlockFor(kind))` (:282, :295, :303, :315), `setupLLInt` -> `LLInt::setEntrypoint` (tripwire), `installCode`.
   Two threads first-calling one function: both pass the `hasJITCodeFor` test; the second either trips a RELEASE_ASSERT or installs a second CodeBlock over the first, which then `unlinkOrUpgradeIncomingCalls` (:229-230) while the first thread may be running in it.
8. **`installCode`** (`ScriptExecutable.cpp:138-233`) publishes in this order: `m_codeBlockForCall` (barriered store, `FunctionExecutableInlines.h:55-66`), `m_jitCodeForCall` (a `RefPtr` assignment), `m_jitCodeForCallWithArityCheck = nullptr`, `clearableCodeSet.add(this)` (IsoCellSet bit), debugger hooks. `entrypointFor` later caches `m_jitCodeForCallWithArityCheck` (`runtime/ExecutableBase.h:153-183`). Readers in asm: `virtualThunkFor` loads `m_jitCodeForCallWithArityCheck`, then `m_codeBlockForCall` (LLI.asm:2789-2797): two words, read in the opposite order of no particular protocol. With LLInt-only code the entry address is the same shared prologue for every function, so a stale entry pointer is benign; a null/stale `m_codeBlockForCall` next to a non-null entry is **memory safety**.
9. **`CodeBlock` construction.** Constructor: `m_metadata(unlinkedCodeBlock->metadata().link())` (`CodeBlock.cpp:383`), `vm.heap.codeBlockSet().add(this)` (:401). `UnlinkedMetadataTable::link()` (`UnlinkedMetadataTableInlines.h:186-231`) mutates the *shared* unlinked table: `m_is32Bit`, frees and replaces `m_rawBuffer`, `m_isLinked = true`; `unlink()` (:233-256) runs from `~MetadataTable` i.e. from a CodeBlock's destruction (sweep). Two realms/threads linking the same UnlinkedCodeBlock at once: double free / use after free of `m_rawBuffer`. `finishCreation` (`CodeBlock.cpp:415-905`): `setConstantRegisters` (:434, clones SymbolTables), function decl/expr vectors (:448-470), handlers, per-op metadata placement-new (:519-522), scope resolution against the **actual scope chain of the first caller** (`JSScope::abstractResolve`, :551,:657,:754,:786; may touch watchpoint sets for `Put`), `fuseReadsOfClosureVariables` (:877-880), template objects (:891). All of this happens before the block is published, so it is private *if* step 7 is made single-flight.
10. **`executeEval`** (`Interpreter.cpp:1996-2203`): finds the variable object, `prepareForExecution`, fires `varInjectionWatchpointSet` for sloppy eval in a function (:2054-2055), declares vars/functions by `hasOwnProperty` + `put` (`ensureBindingExists` :2094-2103: check-then-act) or `createGlobalVarBinding`, then
    ```cpp
    JSCallee* callee = globalObject->evalCallee();      // :2150  one cell per realm (JSGlobalObject.h:267, .cpp:1138)
    callee->setScope(vm, scope);                        // :2166 / :2196
    vmEntryToJavaScript...(..., callee, ...);
    callee->setScope(vm, nullptr);                      // :2168 / :2198
    ```
    with the comment ":2194 eval code only uses scope at the beginning (op_enter)". `op_enter` does `Callee[cfr] -> JSCallee::m_scope -> scope register` (LLI64.asm:864-867). **Two threads in `eval` in one realm exchange scope chains (or get null).** The eval's bytecode was linked with fixed depths/offsets for its own chain (`ClosureVar` + `m_localScopeDepth`), so this is memory safety, not just a wrong variable.
11. **Programs** (`executeProgram`): `ProgramExecutable::initializeGlobalProperties` (`runtime/ProgramExecutable.cpp:94-310`): a long check phase (prototype chain, shadowing rules, :126-228) followed by an act phase: `BatchedTransitionOptimizer` (:230), `createGlobalVarBinding`/`createGlobalFunctionBinding` (:252-274), global lexical `symbolTable->add` under the SymbolTable lock + `addVariables` (:280-293), `bumpGlobalLexicalBindingEpoch` (:307). The epoch bump (`runtime/JSGlobalObject.cpp:3989-4004`) is `++m_globalLexicalBindingEpoch`, tells the AOT run time (`didChangeGlobalLexicalBindings`) and on wrap rewrites every CodeBlock's metadata (`notifyLexicalBindingUpdate`). Readers compare the epoch in `resolve_scope` (LLI64.asm:2895-2898): a thread that passed the check just before the bump resolves to the global object once more: **wrong JS value window** (shadowed global), same as a reordering of the two scripts. After the run `useRunOnceCodeRelease` drops the program's code (`Interpreter.cpp:1718-1719`).
12. **`VM::m_builtinExecutables`**: per-VM array of lazily created `UnlinkedFunctionExecutable*` (`builtins/BuiltinExecutables.cpp:313-324`, test-then-create), cleared by `deleteAllCode`.

### 8.5 Is bytecode ever discarded in an AOT process? Yes (linked code), by the collector

- **Mini mode is off**: `VM::isInMiniMode()` = `(!useJIT && !runsCompiledImage) || forceMiniVMMode` (`runtime/VM.h:806-809`), so `useUnlinkedCodeBlockJettisoning()` is false (:811-814): `UnlinkedFunctionExecutable -> UnlinkedCodeBlock` edges are strong (`codeBlockEdgeMayBeWeak()`, `bytecode/UnlinkedFunctionExecutable.h:376-381`) and `UnlinkedCodeBlock::age` is not consulted. **Unlinked bytecode is not aged out.**
- **Linked CodeBlocks are aged out.** The executable->CodeBlock edge is weak-ish: `ScriptExecutable::visitCodeBlockEdge` (`ScriptExecutable.cpp:577-622`) marks the block only if `shouldVisitStrongly`; `shouldJettisonDueToOldAge` (`CodeBlock.cpp:1594-1679`): not already marked (a block found in a frame by the stack scan is), TTL of `InterpreterThunk` = 5 s (:1563), and with `useExecutionCountForCodeBlockAging` (default true, `OptionsList.h:88`) "if the LLInt execute counter moved since the last look, renew the lease to 3 x TTL" (:1641-1669; writes `m_previousCounter`, `m_creationTime` from a marker thread under `m_lock`). After `deferIndefinitely()` (`m_totalCount = 0; m_counter = INT32_MIN`, `bytecode/ExecutionCounter.cpp:69-74`) the asm keeps adding to `m_counter`, so `count()` (`ExecutionCounter.h:66`) still moves and aging still sees activity.
- The actual removal: end of collection, world stopped, on whichever thread conducts: `Collector::endCollectionInEachHeap` (`heap/Collector.cpp:763-784`) -> `Heap::pruneDeadReferences` (`heap/Heap.cpp:1011-1046`; ":1020 This doesn't necessarily run on this heap's mutator thread") -> `reconcileWeakReferencesAtGCEnd` (:739-802) -> `FunctionExecutable`/`GlobalExecutable::reconcile...` -> `jettisonCodeBlockEdgeIfDead` (`runtime/ScriptExecutableInlines.h:33-62`) -> `jettison(JettisonDueToOldAge)` -> `installCode(nullptr)`; then `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` (`CodeBlock.cpp:1872-...`) clears caches that name dead Structures/cells, and `CallLinkInfo::reconcile...` unlinks. `deleteUnmarkedCompiledCode` only frees stub routines (`Heap.cpp:1320-1323`).
  What protects a running block: it is marked from the frames that hold it, and `m_codeBlocks->iterateCurrentlyExecuting` (`Heap.cpp:1041-1045`). With several mutators this needs every thread's stack scanned (heap report); a thread that has read `m_codeBlockForCall` into a register/frame slot at a safepoint is covered by the conservative scan as today.
- **Explicit deletion** needs an idle VM: `Heap::deleteAllCodeBlocks` has `RELEASE_ASSERT(!vm.entryScope)` (`Heap.cpp:1180`), `VM::deleteAllCode` runs `whenIdle` (`VM.cpp:1202-1215`), `shrinkFootprintNow` returns if `entryScope` (:1230). `vm.entryScope` is one word per VM: "idle" would have to mean "no thread is in JS".
- `ScriptExecutable::clearCode` (`ScriptExecutable.cpp:77-131`) callers seen: the two above and run-once release of programs/modules.
- No `ExecutableToCodeBlockEdge` class in this tree (edges are `WriteBarrier<CodeBlock>` + the functions above). `forceCodeBlockToJettisonDueToOldAge` exists (`OptionsList.h:533`, default false). The AOT branch changes nothing in aging (diff of `CodeBlock.cpp` only adds `JSC_REMOVABLE_*` and the two link hooks).

### 8.6 The border between interpreted and compiled code

**What a compiled function is to `op_call`.** Either `AOT::Closure`, a `JSFunction` subclass whose cell type is `CompiledClosureType` (`aot/object/AOTClosure.h:21`, `runtime/JSFunction.h:174`), or a slim closure (`SlimClosureType..SlimClosureWithPropertiesType`). `getJSFunction()` tests `type == JSFunctionType` (`runtime/JSObjectInlines.h:53-58`), so both are "not a JS function" to the call slow paths and go to `handleHostCall`, where `getCallData` answers the new `CallData::Type::Compiled { entrypoint, realm }` (`runtime/CallData.h:45,59-72`; `Closure::getCallData`, `aot/object/AOTClosure.cpp:122-132`; a class constructor answers `Native callClassConstructor`).
- Linking: `linkCompiledCallee` (`bytecode/RepatchInlines.h:89-110`): seen once -> `linkMonomorphicCall(vm, owner, info, /*codeBlock*/ nullptr, callee, entrypoint)`; then polymorphic (`Repatch.cpp:190-203`) or virtual. Non-IC path: Slow.cpp:2339-2342, 2371-2374.
- The entry pointer is **not per function**: it is one adapter stub per image, `closureEntrypoint()` = `StubFamily::EnterClosure` (function-local static, `aot/stubs/AOTStubEntrypoints.cpp:39-43`), also mirrored in `Instance.stubs` for the asm trampoline. The adapter (`generateEnterClosure`, `aot/stubs/AOTStubsCallAdapters.cpp:279-310`) finds the Instance from the callee (`callee -> StructureID -> Structure::realm -> m_aotInstance`, `emitFindInstance` :97-104), makes an ordinary JSC frame, **stores null in the CodeBlock slot** (:133), saves/sets the pinned registers (instance, cage base, tags), loads the 64-bit closure word at `offsetOfClosureEntry` (:284) which holds the code address and parameter count, moves arguments from the frame to registers, calls, restores, returns. All inputs are the frame, the closure (immutable word) and Instance words: nothing here is written but the stack. (`EnterClosureForConstruct` writes `Instance.constructWords` = a small cache of two words per entry, :383-387: calls report.)

**Compiled -> interpreted.** No `vmEntry`. `generateCallEngineCallee` (`aot/stubs/AOTStubsCallEngine.cpp:106-143`): spill argument registers, build a JSC callee frame on the stack (`emitEngineCallFrame` :42-62), write **the global object into its own frame's CodeBlock slot and 0 into its callee slot** (`emitStoreCodeOwner` :64-72, because `CallLinkInfo::ownerForSlowPath` reads `callerFrame()->codeOwnerCell()`, `CallLinkInfo.h:563-573`; `globalObjectForSlowPath` accepts a JSGlobalObject owner, `CallLinkInfo.cpp:390-391`), load `Entry::CallLinkInfoForCall` and call `Entry::VirtualCallTrampoline`.
- `CallLinkInfoForCall` is **one `DataOnlyCallLinkInfo` per VM**, set to virtual mode once (`aot/runtime/AOTRuntimeTable.cpp:222-233`); in `Mode::Virtual` the slow paths do not modify it (`RepatchInlines.h:106-107,221-222`). Read-only in practice.
- The trampoline is the (AOT) virtual call trampoline: `JSFunction -> executable -> m_jitCodeForCallWithArityCheck`, `m_codeBlockForCall` -> callee frame, jump to the LLInt arity-check prologue (LLI.asm:2782-2801); on null, `llint_virtual_call` (Slow.cpp:752-767) -> `virtualForWithFunction` -> `prepareForExecution` (8.4 step 7).
- C++ run-time operations call text functions through `JSC::call` -> `Interpreter::executeCallImpl` -> `vmEntryToJavaScript` (e.g. `operationAOTApply`, `aot/runtime/AOTCallOperations.cpp:121-137`). C++ -> compiled: `callCompiledCode` (`aot/runtime/AOTCompiledCall.cpp:45-68`): `VMEntryScope`, `ProtoCallFrame.init(nullptr /*codeBlock*/, ...)`, `vmEntryToJavaScript(entrypoint)`.

**Frames.** Adapter frame: JSC header, CodeBlock = null, callee = closure. Frames of compiled functions proper have no JSC header at all; walkers classify by return address (`StackVisitor::visitWithCompiledFrames`, `AOT::CompiledFrameIterator`, per-VM `CallSiteCache` used in `aot/runtime/AOTUnwinding.cpp:235-262`: a cache written during walks; calls/stack report). `makeHostFunctionCall` now zeroes `ReturnPC` "to tell it from a frame record of ahead-of-time compiled code" (LLI64.asm:373-374).

**`topCallFrame`/`topEntryFrame` and friends are VM fields**, reached from asm through `CodeBlock::m_vm`, `JSGlobalObject::m_vm` or the callee's MarkedBlock header (`convertJSCalleeToVM`, LLI.asm:1466-1474):
- `doVMEntry` saves `VM::topCallFrame/topEntryFrame` in the `VMEntryRecord` and restores them (LLI64.asm:187-200, 296-305); native trampolines store `cfr` (LLI64.asm:2818, 2840, 2850, 2872); every LLInt slow path sets it (`LLINT_BEGIN` -> `SlowPathFrameTracer`); every AOT operation sets it to *its own C++ frame record* (`AOTOperationPrologueCallFrameTracer`, `interpreter/FrameTracers.h:168-189`; `AOT_OPERATION_BEGIN`, `aot/runtime/AOTOperationScope.h:72-85`).
- Also per VM and used by asm: `m_exception` (LLI.asm:2588, LLI64.asm:2834), `callFrameForCatch`, `targetMachinePCForThrow`, `targetInterpreterPCForThrow` (LLI64.asm:2744-2754, 2791), `encodedHostCallReturnValue` (:404; written Slow.cpp:2578), the soft stack limit and trap bits in `VM::m_threadContext` (LLI.asm:251, 1549, 2519), `m_lastStackTop` (LLI.asm:1987), the callee-save buffer of the top entry frame, `m_shadowChicken`, `m_typeProfilerLog`, `heap` barrier threshold / `mutatorShouldBeFenced`.
  **Every one of these must become per thread (or be reached through a per-thread register); LLInt has no thread register today: it re-derives `VM*` from heap cells.** This is the largest mechanical change in the interpreter.

**Exceptions across the border.** `genericUnwind` (`jit/JITExceptions.cpp:42-103`) -> `Interpreter::unwind` (`Interpreter.cpp:1436-1446`): with compiled code `visitWithCompiledFrames`; `UnwindFunctor::unwindCompiledFrames` (:1256-1273) -> `AOT::unwindCompiledFrames` (`aot/runtime/AOTUnwinding.cpp:134-185`) which copies saved registers into the VMEntryRecord buffer, and returns FoundHandler / LeftCompiledCode (walk continues in bytecode frames via `visitFrom(exit)`) / ReachedEntryFrame. It writes Instance words: `instance.runtime.exceptions.stackPointerOfHandler`, `instance.json.didUnwindTo`, `instance.coroutines.frameThatMayContinue = nullptr` (:107-113, :181): **per-Instance, would have to be per thread.** The result goes into the six `vm.target*`/`callFrameForCatch` words above; `llint_op_catch` reads them (LLI64.asm:2739-2775).

**Tripwire.** `LLInt::setEntrypoint` (`llint/LLIntEntrypoint.cpp:261-267`) calls `AOT::willRunInInterpreter` whenever the VM has compiled code or the option is on: counts by code type; with `crashIfInterpreterIsUsed` it crashes unless the provider's provenance is not `Static` (run-time text), or the code is an implicit class constructor, or a built-in function in a realm without an Instance (`aot/runtime/AOTInterpreterTripwire.cpp:22-40`). It fires when a CodeBlock is *linked*, not per run. Stateless apart from the counter.

### 8.7 Direct `eval` inside compiled functions

**Build time (Bun front end).** A call spelled `eval(...)` is lowered to `callee === %eval% ? DirectEval : ordinary call` (`bun:src/aot/lower/scope/eval.rs:32-81`). A strict eval whose text is a build-time constant naming a visible binding becomes a plain read (`find_binding_named_by_text`, :84-126).
- Every symbol visible at the call is recorded as *used and stored* by the eval (`visit_direct_eval`, `bun:src/aot/lower/scope/walk/functions.rs:224-247`), plus `this`, `new.target`, home object, `arguments`, class slots, CommonJS wrapper parameters.
- `compute_sharing` treats "seen by eval" like "captured by another function": `is_shared = is_exported || is_seen_by_eval || ...` (`bun:src/aot/lower/scope/plan/sharing.rs:48-62`). Such a binding gets a **location**: a module `Variable` or a field of a heap `Environment`. Exception: an immutable binding that is initialized before the eval is passed by value (`collect_fields_for_eval`, :376-397).
  So yes: variables are moved to heap environments at compile time because the function contains `eval`, and they are found **by a name table**, not by scanning frames.
- The table is the `EvalScope` record (`aot/ir/schema/program-records.def:48-58`): mode, flags, list of `EvalBinding {name, kind, depth, layout/field | variable | operand, function, setter}`; kinds: `Variable Constant FunctionName This NewTarget HomeObject ClassObject Arguments PrivateName PrivateMethod PrivateAccessor WithObject EvalVariables MappedArgument SimpleCatchParameter`. The IR op is `DirectEval(evalScope, text, thisValue, newTarget?, environments...)`, `unknownCode`, `flow = Unsound` (`aot/ir/schema/language-operations.def:49`). In the image it is a read-only `DirectEvalRequest` + `EvalBindingData[]` + an index sorted by identifier number (`aot/runtime/AOTOperationData.h:208-241`).

**Run time**: `operationAOTDirectEval(instance, text, requestOffset, count, operands)` (`aot/runtime/AOTDirectEvalOperations.cpp:958-992`).
1. Non-string, non-object text is returned (:963-964).
2. `buildScopeChain` (:850-869) **on every call**: copies the operands (environments, values) into a fresh `JSCellButterfly` (:817-823), then builds fresh scope cells from the global scope inwards (`addBindingsToChain` :784-815): `AOT::EvalScope` (a `JSScope` with `StrictEvalActivationType`, delegates `getOwnPropertySlot/put/deleteProperty` to an `EvalBindings` object; answers `@newTargetLocal` itself; `aot/object/AOTEvalScope.cpp:48-73`), `EvalVariableScope` (a real `JSLexicalEnvironment` with a fresh 4-slot SymbolTable: private class brand, private brand, own bindings, variables object; scope type `VarScope`/`LexicalScope`/`CatchScopeWithSimpleParameter`; :562-668), `JSWithScope` for `with` objects.
3. Reads/writes from the interpreted text: the eval's `resolve_scope`/`get_from_scope`/`put_to_scope` stay **Dynamic** for these scopes (they are not plain lexical environments with the names in a SymbolTable), so they go `JSScope::resolve` + `getPropertySlot` / `methodTable()->put` (Slow.cpp:2651-2730). `EvalBindings::find` (:402-423): `@this`, `@derivedConstructor` by private name, else `identifiers->numberOf(uid)` + binary search in the request. `read` -> `readBinding` (:100-113): module variable = `ProgramStatics::readVariable(instance, slot)`; environment slot = `readSlot(representation, env + offset)`; mapped argument = `DirectArguments` slot; operand = the copied value. Write: `tryWriteBinding` (:462-494): `tryWriteSlot` into `instance->statics() + offset` (+ `didStoreRootInStatics`) or into the Environment (+ barrier). `RELEASE_ASSERT(tryWriteBinding(...))` (:508): the compiler must have given eval-visible slots a representation that takes any value.
   So the interpreted text touches **the same words** compiled code uses, with plain loads/stores: same races as two compiled threads on one variable (objects/environments report), nothing extra.
4. TDZ: an empty value throws `createTDZError` on read (:454-457) and on write (`storeVariable` reads first, :500-503: check-then-act, value-only). `const`/function name/private names: TypeError or silent (:520-531). `this` in a derived constructor before `super()`: `undefined` operand is mapped to empty (`boundThisOrEmpty` :115-118); `super()` in the text throws (`createSuperCallInDirectEvalError`, :532-534; documented `bun:docs/bundler/aot-compatibility.mdx:87`).
5. `var`/function declarations of a **sloppy** eval: `executeEval` looks for the first `JSLexicalEnvironment` with `VarScope` (`Interpreter.cpp:2025-2038`) = the `EvalVariableScope`, whose `put` forwards unknown names to the **`EvalVariables` object**, an ordinary null-prototype object that the compiled function created on entry and keeps in a slot (`ScopeSlot::EvalVariables`, `bun:src/aot/lower/scope/enter.rs:187`; binding kind `EvalVariables`, `bun:.../eval/bindings.rs:341-346`). Compiled code in such scopes resolves free/outer names dynamically by testing those objects first (`bun:src/aot/lower/scope/dynamic.rs:43-88`). If the function has none, a throw-away object is used (`addEmptyVariableScope`, :761-764, :858-859). At module/global level the variable object is the global object. Also fires the realm's `varInjectionWatchpointSet` (`Interpreter.cpp:2054-2055`).
6. Private names: `privateNameEntryOf` -> `PrivateNameEnvironment` handed to the parser (:864-867, `caller.privateNames`); values are fresh `Symbol` cells over the image's private `SymbolImpl` (:220-225), accessor pairs, or for static fields a pair of `JSNativeStdFunction`s (:345-356).
7. **Caching**: `DirectEvalCaches`, one destructible cell per Instance (`instance.state->objects->caches->directEvalCaches`, created lazily :948-956, kept alive by `immortalCells`), maps `requestOffset -> unique_ptr<DirectEvalCodeCache>` (:885-946). Key inside = (text, `BytecodeIndex::fromBits(requestOffset)`) (:987-989). `ensureCacheOf`: unlocked `m_caches.get`, locked `add` (lock is for the collector's visit). The cached `DirectEvalExecutable` has **one `EvalCodeBlock`**, linked against the scope chain of the first call; later calls (other activations, other threads) reuse it with their own fresh chain of the same shape.

**Mutable state here**: per Instance: `directEvalStructures.{bindings,scope,variableScope}` (`ensureStructure` :706-714, test-then-create-then-`immortalCells->add`), `directEvalCaches`, `ImmortalCells::m_cells` (SegmentedVector append, `aot/runtime/AOTImmortalCells.h:38`), `objectLiteralPlans`. Per realm: `evalCallee` (8.4 step 10; compiled callers go through the same `executeEval`). Per VM: identifiers table (read-only after load, as far as read), CodeCache not used. Everything else is fresh per call.

### 8.8 Closed programs

- Set at build: `flagsForUnseenCode` (`aot/image/AOTImageBuilder.cpp:577-593`) adds `ImageFlag::AdmitsUnknownJavaScript` iff flow recorded an "unseen code" cause that admits it (or flow has no results). Meaning: `aot/image/AOTImageFormat.h:117` "Without AdmitsUnknownJavaScript the proofs in the code hold only while nothing but the code of the image runs".
- Tested once per realm: `RuntimeHooks::didCreateRealm` (`aot/runtime/AOTRuntimeHooks.cpp:80-82`) -> `refuseEvaluationUnlessAdmitted` (`aot/runtime/AOTRealmStartup.cpp:127-132`, "Every realm of the VM, with an instance or without"): `realm->setEvalEnabled(false, evaluationRefusedMessage())`.
- The `EvalError` is then thrown by the engine's existing `evalEnabled()` tests: direct eval `Interpreter.cpp:171-173`, indirect `JSGlobalObjectFunctions.cpp:496-498`, `Function` `FunctionConstructor.cpp:203-207`. `m_evalEnabled` is a plain bool written at realm creation (`JSGlobalObject.h:701,1324-1327`): read-only afterwards.
- Bun side: before engine init `refuse_code_the_compiler_did_not_see` (`bun:src/runtime/cli/startup_options.rs:467-472`); `errorIfUnseenCodeIsRefused` for `Bun.plugin` (`bun:src/jsc/bindings/BunCompiledModules.cpp:453-459`, `BunPlugin.cpp:1209`); `node:vm` contexts set their own flag (`NodeVM.cpp:1069`).
- With `--trim` a closed program may have no interpreter at all (`RemovableFeature::Interpreter`: `LLIntData.cpp:97-100`, `JSC_REMOVABLE_DEFINITION(Interpreter, ...)` on `parse`, `BytecodeGenerator::generate`, `executeEval`, `setEntrypoint`, ..).
- **For threads: in a closed program none of section 8 exists at run time** (subject to 8.9).

### 8.9 Built-in JavaScript (`builtins/*.js`)

Neither "always interpreted" nor "replaced by C++": **compiled into the image per function, on demand; text is the fallback.**
- The generator numbers every function of `JavaScriptCore_BUILTINS_SOURCES` in `BuiltinCodeIndex` order and declares it a `RuntimeFunction` (`aot/builtins/generator/collect-runtime-functions.ts:97-102,167-186`); 11 files have hand-written TypeScript replacements (`aot/builtins/handwritten/*.ts`, `aot/builtins/AOTBuiltinTable.cmake`), some functions are excluded. One was moved to C++ (`Reflect.deleteProperty`, `builtins/ReflectObject.js` diff).
- Bun's front end compiles the runtime functions that are roots: start-up functions, module-loader functions, what the language itself uses, and what the program's sources call (`bun:src/aot/load/runtime_roots.rs:25-58`). The image lists them in `compiledRuntimeFunctions {id, function, visibility}` (`aot/runtime/AOTModuleData.h:330-334`).
- At realm set-up every `JSC_BUILTIN_FUNCTION` goes through `BuiltinExecutables::codeFor(index)` (`Scripts/wkbuiltins/builtins_templates.py:91`, `builtins/BuiltinExecutables.cpp:327-361`): `hasCompiledFunction` = `vm.hasCompiledCode() && image has id` -> `{nullptr, id}`; else `linkFromText`. `JSFunction::create(vm, global, BuiltinCode, ...)` (`runtime/JSFunction.cpp:114-136`) then makes `AOT::builtinFunction(globalObject, id)` = an `AOT::Closure` (`aot/runtime/AOTBuiltinModules.cpp:86-98`) **if the realm has an Instance**, else links the text.
- Consequences:
  - In the program's own realm, a built-in that the build saw a use of is machine code. `crashIfInterpreterIsUsed` treats built-in text running in that realm as a failure (provenance `Static`, `parser/SourceProvider.h:386-391`), and Bun runs whole servers under it (PR-bun-45631.txt:355). So **a program without run-time text does not run LLInt in steady state** (that is the tested claim; "The executable has no bytecode in it", PR-bun:276).
  - An engine built-in that is *not* in the image (only run-time text can reach it) is interpreted from `s_JSCCombinedCode`: `refusesToInterpret` only refuses *host library* text (`id >= numberOfBuiltinCodes`) and only without `AdmitsLibraryText` (`AOTBuiltinModules.cpp:157-181`, `ScriptExecutable.cpp:423-429`); docs: "Everything that belongs to the language always works in a string" (`bun:docs/bundler/aot-compatibility.mdx:305`).
  - Realms without an Instance (`node:vm` contexts, ShadowRealm) have all their built-ins as text (tripwire comment, `AOTInterpreterTripwire.cpp:34`).
  - Bun's own JS modules/functions: compiled (`ModuleKind::RuntimeSource`, `evaluateBuiltinModule`) or refused with "was not compiled into this executable".
- So LLInt runs only in programs that (a) evaluate text, (b) create other realms, or (c) load uncompiled code from disk (`--external`, plugins: `imports_uncompiled_code`). In those programs interpreted code includes engine built-ins (Array/Promise/iterator helpers in JS), not only the user's strings.

### Hazards with several mutators (interpreter)

Severity: **M** = memory safety (type confusion, wild pointer, double allocation, container corruption), **C** = deterministic crash (RELEASE_ASSERT), **V** = wrong JavaScript value only, **P** = lost profile/perf only.

Execution state
1. **M** All "current thread" state is in `VM` and found via `CodeBlock::m_vm` / cell -> VM: `topCallFrame`, `topEntryFrame`, `m_exception`, `callFrameForCatch`, `target*ForThrow`, `encodedHostCallReturnValue`, soft stack limit, trap bits, `m_lastStackTop`, entry-frame callee-save buffer (8.6). LLInt has no thread register.
2. **M** `JSGlobalObject::m_evalCallee`: `setScope(scope)` / enter / `setScope(nullptr)` around every eval, direct or indirect, from bytecode or compiled code (`Interpreter.cpp:2150-2198`; read in `op_enter`). Not checked: whether `globalCallee`/module callees have the same pattern (program code uses `globalCallee` with a fixed scope, `:1500,1709`).
3. **M** Unwinding writes per-Instance words (`stackPointerOfHandler`, `frameThatMayContinue`, `json.didUnwindTo`).

Inline allocation
4. **M** `allocateCell` in asm on `LocalAllocator`s reached from shared places: `OpNewObject` metadata word, `ObjectLiteralPlan::allocator`, `Instance.allocation.{cellSpace,auxiliarySpace,inlineAllocations[]}`, `Instance.arrayAllocator`, `ropeStringAllocator`. Two threads get the same cell. If allocators become per thread, none of these cached pointers may stay where they are.

Per-bytecode caches (multi-word, writer not ordered against asm readers)
5. **M** `GetByIdModeMetadata` (structureID published before offset; offset doubles as a format tag in the AOT handler; mode byte overlaps `cachedSlot`). 7 opcodes.
6. **M** `OpPutById` four words incl. an unlocked clear; transition = store StructureID then value (also a two-step update of the *object*, visible to other threads and identical to upstream; objects report).
7. **M** `get_by_id_direct`, `get_private_name`, `put_private_name`, `set_private_brand`; **V** `check_private_brand`.
8. **M** `get_from_scope`/`put_to_scope`: `m_getPutInfo` vs `m_operand` (offset / address) vs union `m_structureID|m_watchpointSet`; wild load/store.
9. **M** `resolve_scope`: `m_resolveType` vs union pointer vs union epoch/depth. **V** epoch check-then-use against a concurrent global `let/const` declaration.
10. **M** `CallLinkInfo` three words; `m_incomingCalls` list; polymorphic stub replacement under a scanning reader; **C** `RELEASE_ASSERT(!lazyCallLinkInfo.get())` and double `ensureSlow` in `handleUnlinkedCall`.
11. **M** `ObjectLiteralPlan` overwritten in place (`didOutgrowCell`); `ObjectLiteralPlans::m_entries` Vector append/scan; `planObjectLiteral` test-then-set of the allocator word (benign by itself: both compute the same plan).
12. **M** Hashed property table probed from asm and from `findByKey` users: validity = two words, payload read afterwards; eviction/rewrite/grow/`clear` with plain stores; `const Slot*` held across JS calls. (Details: caches section.)
13. **P** `ArrayAllocationProfile` word RMW (but **M**, read only: `updateProfile()` looks into another thread's array butterfly); all value/array/arith profiles; `seenModes`, `m_hasJumped`, `m_enumeratorMetadata`, `m_cachedCallee`, `m_toThisStatus`; LLInt execute counter (its reader, aging, only becomes slightly less exact).
14. Writers outside the mutators: watchpoint fires (`clearLLIntGetByIdCache`) run on whichever thread changes the structure, i.e. **another thread's CodeBlock metadata is rewritten asynchronously** (today: same thread, between bytecodes). **M** with 5.

Making code
15. **M** Atom table (current thread's = VM's), non-atomic `StringImpl` refcounts on identifiers, sources, uids in caches.
16. **M** Unlocked per-VM containers: `CodeCacheMap` (written on hits too), `sourceProviderCacheMap` + shared `SourceProviderCache`, `CompactTDZEnvironmentMap`, `BuiltinExecutables` slots, `codeBlockSet`, IsoCellSets (`clearableCodeSet`, `unlinkedFunctionExecutableSpaceAndSet.set`).
17. **M** `DirectEvalCodeCache::get` without the lock; `DirectEvalCaches::m_caches.get` without the lock. **P** duplicate executables.
18. **M/C** `prepareForExecution` is check-then-act: RELEASE_ASSERTs in `newCodeBlockFor`, or a second `installCode` that unlinks callers of a block in use. `installCode` publishes CodeBlock and entry in separate words; `virtualThunkFor` reads both.
19. **M** `UnlinkedMetadataTable::link/unlink` mutate the shared unlinked table (`m_rawBuffer` free/replace) without a lock; `unlink` runs at sweep time.
20. **M/V** Lazy per-CodeBlock state: `materializeFunctionExecutable`, `createRareDataIfNecessary`, `m_llintGetByIdWatchpointMap`, template object map (**V**: two identities for one template site).
21. **V/M** Declaration instantiation (`executeEval` var/function hoisting, `initializeGlobalProperties`): long check phase then act phase on the global object / variable object; duplicates or missed SyntaxErrors (**V**); structure transitions of the global object under `BatchedTransitionOptimizer` (**M**, objects report).
22. **M** Per-Instance lazies in the eval path: `directEvalStructures.*`, `directEvalCaches`, `ImmortalCells::add`.

Discarding code
23. **M** Aging/jettison and cache clearing assume "world stopped = the one mutator is stopped at a safepoint and its stack is scanned". Needs all JS threads stopped and scanned. `deleteAllCode*`/`shrinkFootprint` assume `!vm.entryScope` means nobody runs JS.
24. **P** `shouldJettisonDueToOldAge` writes `m_creationTime/m_previousCounter` from markers (already concurrent).

What is *not* a hazard: opcode maps (frozen), bytecode instruction streams and constants after publication (immutable; the AOT handlers decode following instructions but never patch them), `DirectEvalRequest`/`RuntimeType`/layout tables (image, read-only), adapter stubs and `CallLinkInfoForCall` (virtual, never relinked), `m_evalEnabled`, fused `m_resolveType` (written before publication), scope chains built per direct eval (fresh cells).

### Not found / not sure

- Whether `CustomAccessorTable::records()` can move when it grows (asm reads `Instance.propertyCaches.customAccessors` then indexes it, AOT.asm:417-421). Not read.
- Lifetime of a replaced `PolymorphicCallStubRoutine` when its code is the shared LLInt trampoline: `deleteUnmarkedJettisonedStubRoutines` decides by stack scan of code ranges; I did not read whether a thread that only holds the *data* pointer (t5 in LLI.asm:2842-2850) keeps it alive.
- `UnlinkedFunctionExecutable::takeParentDeclaredNames()`: effect of a second generation after the move not read.
- `SourceProviderCache` internals (`parser/SourceProviderCache*`) not read beyond creation/`markUsed`.
- `JSScope::abstractResolve` for `Put` "prepares the variable's watchpoint set" (comment `CodeBlock.cpp:540`): the mutation itself not read.
- `AOT::RuntimeHooks::didChangeGlobalLexicalBindings` (what compiled code caches about global lexical bindings) not read.
- Whether Bun calls `shrinkFootprint`/`deleteAllCode` in a compiled executable (Bun report).
- `StackVisitor.cpp/.h` diff (309+129 lines) and `CallSiteCache` only skimmed through their users.
- Module code from disk (`ModuleProgramExecutable`, `JSModuleEnvironment::fillImportSlot`, `LazyClosureVar`) in an AOT process: paths exist (LLI64.asm:2940-2948, 3036-3043) but not followed.
- ARM64E keeps the ordinary `op_call` (AOT.asm:1681,1796); C_LOOP has none of this. Not relevant to shipped targets as far as the PR's platform table says.
- I did not verify by running anything; all statements are from reading at head 5c2923e566cb.

---------------------------------------------------------------------------------------------------------------------------------------

## 9. RegExp at run time in an AOT process

All paths are relative to `Source/JavaScriptCore/` unless they start with `WTF/`.
"Diff" means `git diff 8deea350d4e1 HEAD`. "Stub" means a routine of `aot/stubs/` that is machine code in the image.

### 9.0 The picture in one table

A match touches five kinds of memory. Only the first column is private to the calling thread today.

| Matcher | Program / code | Backtrack memory | Output vector (also *working* memory of the matcher) | Stack limit it compares with |
| --- | --- | --- | --- | --- |
| Image code, called from a stub | image, read-only (`aot/image/AOTImage.cpp:300-304`) | machine stack of the caller (`yarr/YarrJIT.cpp:1047-1073`) | **one buffer per VM** `VMState::regExpMatchOutput`, or **one per RegExp cell** `RegExp::m_ovector` for the form `Found` (`aot/stubs/AOTStubsRegExp.cpp:58-59,128-140`) | `Instance::stackMirror.m_softStackLimit`, per realm (`aot/stubs/AOTStubsRegExpFrames.h:68-74`) |
| Image code, called from C++ (`RegExp::matchWithImageCode`) | same | same | what the caller passes: almost always `regExp->ovectorSpan()`; a local `Vector<int,32>` for match-only (`runtime/RegExpInlines.h:395-397`) | `vm.softStackLimit()` (`yarr/YarrMatchingContextHolder.h:63-65`) |
| Flat program (`yarr/YarrFlatProgram*`) | malloc, owned by `BytecodePattern`, owned by the RegExp cell | 128 frames in the C++ frame of `run()`, then a malloc'ed `Vector` local to the call (`yarr/YarrFlatProgramMatcher.cpp:103-126,166`) | as above | none (bounded by `Options::maxRegExpStackSize()`, `:115`) |
| Yarr bytecode interpreter | malloc, same owner | **one pool per VM** `VM::m_regExpAllocator` under `VM::m_regExpAllocatorLock`, held for the whole match (`yarr/YarrInterpreter.cpp:2307-2338`) | as above | none (FIXME at `yarr/YarrInterpreter.cpp:2299-2300`) |
| Atom (`/abc/`) | `RegExp::m_atom` | none | as above | none; C++ path uses **per-VM** `vm.adaptiveStringSearcherTables()` (`runtime/RegExpInlines.h:185-194`) |

The decisive fact for threads: **RegExp cells are shared by construction** (one cell per literal record and flags per VM, one cell per
pattern/flags in `RegExpCache`), and the cell, or the VM, owns the buffer into which every match of it writes. Two threads that run the
same `/(\w+)=(\w+)/.exec(line)` share that buffer even though their `RegExpObject`s are different.

### 9.1 The `RegExp` cell

**Fields** (`runtime/RegExp.h:318-336`): `m_patternString`, `m_atom`, `m_state` (uint8), `m_specificPattern`, `m_flags`,
`m_constructionErrorCode`, `m_lastUseEpoch`, `m_imageRecordNumber` (uint16, diff), `m_numSubpatterns`, `m_minimumSize`,
`unique_ptr<BytecodePattern> m_regExpBytecode`, `unique_ptr<YarrCodeBlock> m_regExpJITCode`, `unique_ptr<RareData> m_rareData`,
`FixedVector<int> m_ovector`, `ThreadSafeLazyUniquePtr<BitSet<256>> m_firstCharacterBitmap`. There is no lock member: the lock is
`JSCell::cellLock()`. States (`:274-283`): `ParseError, JITCode, ByteCode, ImageCode, ImageAtom, NotCompiled`; the two `Image*` are the diff's.
The diff also exports offsets of `m_state`, `m_regExpBytecode`, `m_ovector`, `m_numSubpatterns`, `m_atom`, `m_rareData`, `m_regExpJITCode`
(`:88-102`): stubs and compiled code read these words directly, with no lock.

**Written after creation** (all else is set in `finishCreation*` before the cell is published):

| Field | Writer | Lock |
| --- | --- | --- |
| `m_ovector` *contents* | every match that has captures | none |
| `m_lastUseEpoch` | every C++ match, `noteUse` (`runtime/RegExpInlines.h:110-115,164,331`). Comment: "Only the VM's thread notes the use." Stubs never write it | none |
| `m_state`, `m_minimumSize`, `m_regExpBytecode`, `m_regExpJITCode`, `m_constructionErrorCode`, `m_atom`, `m_specificPattern`, `m_rareData` | `compile`, `compileMatchOnly`, `byteCodeCompileIfNecessary`, `deleteCode` | `cellLock()` (writers only) |
| `m_state`, `m_minimumSize`, `m_constructionErrorCode` | `reset()` after a soft error (`runtime/RegExp.h:118-123`, called at `RegExpInlines.h:174-175,341-342`, `RegExp.cpp:519-520`) | **none** |
| `m_rareData->m_cachedGroupsStructureID` | `ensureGroupsStructure` (`runtime/RegExp.cpp:334-361`): read, compare realm, build by transitions, `set` | none |
| `BytecodePattern::m_bytecodeBehindFlatProgram` | `interpretBehindFlatProgram`, at match time (`yarr/YarrInterpreter.cpp:3386-3404`) | none |

**Lazy compile protocol.** `matchInlineOnce` (`runtime/RegExpInlines.h:154-264`): (1) `s.length() - startOffset < m_minimumSize` -> miss; (2)
`noteUse`; (3) `compileIfNecessary` = `hasCodeFor()` (**unlocked** read of `m_state`, `:84-101,117-126`) else `compile()`; (4) re-read `m_state`
for `ParseError`; (5) atom search if `hasValidAtom()`; (6) dispatch on `m_state` again: `JITCode` / `ImageCode` -> `matchWithImageCode` /
else `Yarr::interpret(m_regExpBytecode.get(), ...)` (`:238-250`). So one match reads `m_state` three or four times without a lock and assumes it does not move.

`RegExp::compile` (`runtime/RegExp.cpp:554-604`), under `Locker { cellLock() }`, in this order:
1. `takeCodeFromImage(vm)` (diff, `:478-494`). If `hasValidAtom() && vm.hasCompiledCode()`: `m_state = ImageAtom`, done. Else `imageRecord()`; if the record
   has form `Code`: `regExpCache()->addToStrongCache(this)` when there was no code, `ensureRegExpJITCode()` (allocate `YarrCodeBlock`, `storeStoreFence`,
   publish pointer, `:767-775`), `setCodeFromImage(for8Bit, for16Bit)` (two `MacroAssemblerCodeRef` stores, `yarr/YarrJIT.h:325-329`),
   **then** `m_state = ImageCode`, **then** `m_minimumSize`. No fence between the code pointers and `m_state`.
2. Otherwise parse (`YarrPattern`), `updateMetadataFromPattern` (`:271-296`: rewrites `m_atom`, `m_specificPattern`, `m_numSubpatterns`; creates `RareData` once,
   behind a `storeStoreFence`, comment "The concurrent GC thread reads m_rareData in visitChildren").
3. `if (!hasCode()) { addToStrongCache(this); m_state = ByteCode; }` (`:571-575`) **before any bytecode exists**.
4. JIT branch: dead in an AOT process (`Options::useRegExpJIT() = false` with the JIT, `runtime/Options.cpp:746`).
5. `m_state = ByteCode;` (`:597`), then `m_regExpBytecode = byteCodeCompilePattern(...)` (`:598`), then on failure `m_state = ParseError`, then `m_minimumSize` (`:603`).

`compileMatchOnly` has the same shape (`:649-719`; state at `:670,693`, bytecode at `:699`). With a compiled program and no JIT it builds the capture-complete
pattern at once (`willUseInterpreter`, `:657-659`). **State is published before the pointer it promises.** A second mutator that reads `m_state == ByteCode`
between steps 3 and 5 calls `Yarr::interpret(nullptr, ...)`; the stub does the same in assembly (9.7, `matchOnFlatProgram`).

`deleteCode` (`:748-764`, under `cellLock`): `m_state = NotCompiled`, `m_minimumSize = 0`, `m_regExpJITCode->clear()` (object stays, code refs become null),
`m_regExpBytecode = nullptr` (**frees** the bytecode and the flat program). Callers: `RegExpCache::deleteAllCode` / `deleteCodeNotUsedInCurrentFullCollectionCycle`
<- `VM::deleteAllRegExpCode`, `VM::shrinkFootprintNow` (`runtime/VM.cpp:1192-1200,1254-1259`). Their safety argument is "no JS is running":
`whenIdle` tests `if (!entryScope)` (`runtime/VM.cpp:1175-1183`) and `shrinkFootprintNow` returns if `entryScope` (`:1230`). `entryScope` is one word per VM.

**Concurrent readers today.** (a) The collector: `visitChildrenImpl` reads `m_rareData` and its structure ID (`runtime/RegExp.cpp:322-330`); `estimatedSize` reads
`m_regExpBytecode`, `m_regExpJITCode`, `m_ovector.byteSize()` unlocked (`:308-320`). (b) Compiler threads through `matchConcurrently` (takes `cellLock`, `:629-647,726-741`);
the diff makes it answer `false` for `ImageCode`/`ImageAtom`, and an AOT process has no compiler threads. (c) The sampling profiler reads `VM::m_executingRegExp`
(`runtime/SamplingProfiler.cpp:634,647`), which `MatchingContextHolder` sets and clears around each C++ match (`yarr/YarrMatchingContextHolder.h:59-76`; `runtime/VM.h:963`). Stubs do not set it.

**How image code is found (diff).** By pattern text and flags, in a sorted hash table of the image; never by address.
- Records: `ImageRegExp`, 32 bytes, read-only (`aot/runtime/AOTRegExpRecord.h:25-74`): string IDs of pattern and atom, `codeFor8Bit`/`codeFor16Bit` (offsets into the regexp code range),
  `numSubpatterns`, `minimumSize`, `details`, flags, `form` in {`Code`, `Atom`, `NoCode`, `NoMetadata`}, `isLiteral`. `g` and `d` are not part of the key
  (`significantFlags`, `:26-29`), so one record serves `/x/`, `/x/g`, `/x/d`.
- `findRegExpRecord(vm, pattern, flags)` (`aot/runtime/AOTRegExpRecords.cpp:57-72`): `equal_range` over `ImageSection::RegExpHashes` by `hashOf(pattern.hash(), flags)`, then compare raw flags and text. Reads only the image, plus one counter bump on a miss.
- *Any* RegExp made at run time asks it: `RegExp::finishCreation` (`runtime/RegExp.cpp:190-198`). A hit calls `finishCreationFromImage` (`:244-268`): stores `m_imageRecordNumber = index + 1`
  (0xFFFF = "look up by pattern again", `:232-242`), `m_numSubpatterns`, `m_minimumSize`, `m_specificPattern`, `m_atom` + `m_state = ImageAtom` for form `Atom`, `RareData` from the image, and allocates `m_ovector`.
- The **code** is attached lazily, at the first C++ match (`takeCodeFromImage`). A literal matched only through its bound stub (9.7) never needs the attachment: the stub calls the code by symbol.
- A pattern without a record: with a compiled program every length is "lazy" (`maxEagerLength = 0`, `:199-214`): only `Yarr::checkSyntax`, then `m_numSubpatterns` and `m_ovector`. The `YarrPattern` is first built in `compile`.

**Flat matcher (diff).** `Yarr::byteCompile` (`yarr/YarrInterpreter.cpp:3322-3355`): when `pattern.m_analysis == Performed` (= `vm.hasCompiledCode()`, `runtime/RegExp.cpp:426-430`) it tries
`compileFlatProgram`. Three outcomes: declined -> ordinary bytecode, reason kept in `m_flatProgramDecline` (reasons: `yarr/YarrFlatProgram.h:169-179`); `runsOnlyOn8BitSubjects` -> bytecode **and** `m_flatProgramFor8BitSubjects`;
otherwise a `BytecodePattern` with **only** `m_flatProgram` and `m_source` (no `m_body`). The program is immutable after construction (`FlatProgram`, `yarr/YarrFlatProgram.h:134-167`, all `Vector`s of PODs; the matcher takes `const FlatProgram&`).
`Machine::run` (`yarr/YarrFlatProgramMatcher.cpp:151-199`) first fills `output[0 .. 2*(n+1))` with `offsetNoMatch`, keeps registers and frames in `FrameStorage` (local variable, `:166`; overflow is a local `Vector`, `:109-121`), step budget `matchLimit` in a member.
No statics, no VM: the stub comment says "Most of them run on a flat program, which needs nothing of the VM" (`aot/stubs/AOTStubsRegExp.cpp:274`). But **captures live only in `output`**: `consumeCapturedText` reads `begin = output[k]`, `end = output[k+1]`
and then `input[begin + i]` with a check only of `length - position < count` (`:701-717`); logged ops restore `output[...]` from frames (`:825`).

**Hand-over to the bytecode interpreter.** `Yarr::interpret` (`yarr/YarrInterpreter.cpp:3430-3443`): `m_flatProgram` -> `interpretFlatProgram`; else 8-bit subject and `m_flatProgramFor8BitSubjects` -> that; else `Interpreter<CharType>`.
A flat program "gives up" with `ErrorNoMemory`/`ErrorHitLimit` (`hasGivenUp`, `:3379-3384`) -> `interpretBehindFlatProgram` (`:3386-3404`): `if (!bytecode->m_body) { if (!bytecode->m_bytecodeBehindFlatProgram) { parse m_source; m_bytecodeBehindFlatProgram = ByteCompiler(...).compile(...) } }`.
That is an unlocked check-then-assign of a `unique_ptr` during a match. `Interpreter::interpret` (`:2297-2343`) takes `ConcurrentJSLocker locker(pattern->m_lock)` = `&vm.m_regExpAllocatorLock` (`runtime/RegExp.cpp:438`; `ConcurrentJSLock` is a real `WTF::Lock`, `runtime/ConcurrentJSLock.h` "using ConcurrentJSLock = Lock"),
then `startAllocator`, matches, `stopAllocator(64 KB)`. The lock covers the whole match, so interpreted matches of one VM are already serialised and the pool is safe. `VM::deleteAllRegExpCode` takes the same lock to release retained pools (`runtime/VM.cpp:1198-1199`).

### 9.2 Image-mode code: what it touches at match time

`yarr/YarrCodeGenerator.cpp` is `#define YARR_FOR_IMAGE 1` + `#include "YarrJIT.cpp"`: the same generator with a `CodeAddressing` policy (`yarr/YarrCodeAddressing.h:77-114`).
`AOT::ImageCodeAddressing` (`aot/image/AOTRegExpCodeAddressing.cpp:100-183`) turns every address into a PC-relative reference to a symbol of the regexp section: shared tables (`yarr/YarrSharedTables.h:41-46`), class tables, Latin-1 bit planes, plain data, 64-bit literals, shared routines (`aot/image/AOTRegExpCode.h:46`).
A process address in the code is a compile failure (`JITFailureReason::ProcessAddressInCode`, `aot/image/AOTRegExpCodeAddressing.cpp:72-73`). The section lies inside the image's code range (`aot/image/AOTImage.cpp:300-304`).

Arguments (5): characters, start, length, `int* output`, `MatchingContextHolder*`. Memory used:
- **Output vector.** Not write-only. `subpatternStartAddress` is `Address(m_regs.output, ...)` unless the pattern needs an internal copy in its frame (`yarr/YarrJIT.cpp:2696-2706`). The code *reads back*: match start (`getMatchStart`, `:2983-2990`), captures for back references (`loadSubPattern`, `:11797-11799`),
  the loop end test of a back reference compares `patternIndex` with the **memory word** on every turn, `NotEqual` (`:3885-3886`), and `saveParenContext`/`restoreParenContext` copy captures between output and stack (`:1231,1271`). The shared routine `ClearCaptures` is `rep stosq` into `output + offset` (`yarr/YarrSharedRoutines.cpp:357-369`).
- **`MatchingContextHolder`** (`yarr/YarrMatchingContextHolder.h:40-57`): 4 words, always on the caller's stack. The code reads `m_stackLimit` (entry frame check `yarr/YarrJIT.cpp:12133-12154`; each paren context `:1069`) and reads/writes `m_freeList` where no register is free for it (`:1058-1060,1081-1082`). A stub builds it in its match frame: `storeMatchingContext` stores the limit and `0` for the free list, nothing else (`aot/stubs/AOTStubsRegExpFrames.h:67-74`).
- **Pattern context buffer: does not exist in this tree.** `grep -r PatternContex` finds nothing. Paren contexts are carved from the machine stack (`allocateParenContext`, `yarr/YarrJIT.cpp:1047-1073`: free list first, else `sp - size`, compare with the limit, move `sp`).
- **Stack limit.** From a stub: `Instance::stackMirror.m_softStackLimit` (`aot/runtime/AOTInstance.h:104`, registered with `vm.traps()` at `AOTInstance.cpp:291`; `runtime/StackManager.h:42-64`). From C++: `vm.softStackLimit()`. Both are one value per VM/realm, i.e. the limit of *one* stack. Over the limit the code answers `JITCodeFailure` ("punt to the interpreter", `:12144-12151`, `:2800-2807`).
- **Return addresses** for backtracking are PC-relative values stored in the frame (`ImageCodeAddressing::storeReturnAddress`, `aot/image/AOTRegExpCodeAddressing.cpp:176-183`).
- Match budget: register `remainingMatchCount` (`yarr/YarrJIT.cpp:1049,12170`).

**Writes to shared data: none by the code itself** other than through the `output` pointer it is handed. I found no store in `YarrSharedRoutines.cpp` except `ClearCaptures` and push/pop. So image code is re-entrant if and only if `output` is private.

### 9.3 `RegExpCache`

One per VM (`runtime/VM.h:1008`). State (`runtime/RegExpCache.h:87-95`): `m_weakCache` (`RegExpKey -> Weak<RegExp>`, `WTF_GUARDED_BY_LOCK(m_lock)`), `m_strongCache[64]` + `m_nextEntryInStrongCache`, `m_emptyRegExp`, and the diff's `m_parsedPatterns` (lazily allocated `std::array<RegExp*,1024>`) + `m_nextParsedPattern`.
- `lookupOrCreate` (`runtime/RegExpCache.cpp:50-81`): lock, `get`, **unlock**; `create()`; (diff) `keepParsedPattern` when there is no record or it has no metadata; lock, `weakAdd`. Check-then-act across two critical sections: two threads can both create; the second `weakAdd` replaces the first entry. Two live cells for one key is harmless for JS (cells are not observable), but `finalize` does `weakRemove(m_weakCache, regExp->key(), regExp)` (`:121-127`) and I did not check what it does when the map holds the other cell.
- Only the weak map is locked. `addToStrongCache` (`:129-139`), `keepParsedPattern` (`:84-94`), `ensureEmptyRegExpSlow` (`:113-119`) and the `fill(nullptr)` of `deleteAllCode` (`:141-149`) are plain stores; `visitAggregateImpl` reads the arrays unlocked (`:173-185`) with `appendUnbarriered`. `addToStrongCache` runs under *that cell's* `cellLock`, which does not exclude another cell's compile. The index update is read-modify-write with a wrap test by equality (`m_nextEntryInStrongCache == 64`, `:136-138`): two racing increments can step over 64 and then index out of the array. `keepParsedPattern` wraps with `%` so it stays in range, but its lazy `unique_ptr` creation can be done twice (the loser's array is freed while the other thread may be storing into it).
- Diff summary: the ring of 1024 parsed patterns (rationale `runtime/RegExpCache.h:80-82`: "The weak cache forgets them at every collection"), its clearing and visiting, `NEVER_INLINE` + `JSC_REMOVABLE_DEFINITION` markers. No locking change.
- **Literal cells bypass this cache**: `RegExp::createFromImage` allocates directly unless the record has `NoMetadata` (`runtime/RegExp.cpp:371-381`). They are therefore never reached by `deleteAllCode`, and `/x/` and `new RegExp("x")` are two cells with two `m_ovector`s.

### 9.4 Legacy statics: `RegExpGlobalData` / `RegExpCachedResult`

One `RegExpGlobalData` inside each `JSGlobalObject` (`runtime/JSGlobalObject.h:554,1153-1154`): `m_cachedResult`, `m_substringGlobalAtomCache`, `m_multiline` (`runtime/RegExpGlobalData.h:63-66`). **There is no `m_ovector` here in this tree**; it moved to the RegExp cell (`performMatch` uses `regExp->ovectorSpan()`, `runtime/RegExpGlobalDataInlines.h:60-73`).
`RegExpCachedResult` (`runtime/RegExpCachedResult.h:74-83`): `MatchResult m_result` (two `size_t`), `bool m_reified`, `bool m_oneCharacterMatch`, then six `WriteBarrier`s: `m_lastInput`, `m_lastRegExp`, `m_reifiedResult`, `m_reifiedInput`, `m_reifiedLeftContext`, `m_reifiedRightContext`.

**`record`** (`runtime/RegExpGlobalDataInlines.h:35-43`), six plain stores then a barrier on the global object, in this order:
`m_lastRegExp`, `m_lastInput`, `m_result` (start, end), `m_reified = false`, `m_oneCharacterMatch`, `vm.writeBarrier(owner)`.
The stub `recordMatch` (`aot/stubs/AOTStubsRegExp.cpp:376-404`) does the same seven stores in the same order, loading start/end from the output vector *again* (`load32(matchOutputEntry(offsets, 0/1))`), then `branchIfBarrierIsNeeded` -> `writeBarrier()` stub.
Callers of `record`: `performMatch` x2 (`:55-90`, only on success), `recordMatch` (`:92-96`), `resetResultFromCache` (`:103-109`, which first **copies a cached ovector into `regExp->ovectorSpan()`**).

**Lazy reification.** `lastResult` (`runtime/RegExpCachedResult.cpp:50-96`): `if (!m_reified)`: `m_reifiedInput = m_lastInput`; null `m_lastRegExp` -> the empty RegExp; if `m_result`: for `m_oneCharacterMatch` recompute `m_result` by `reverseFind`; then **run the match again**:
`createRegExpMatchesArray(vm, globalObject, string, input, m_lastRegExp.get(), m_result.start, ignoreMatched)` (`:82`), which writes that RegExp's `m_ovector` once more; then `m_reifiedResult`, clear both contexts, `m_reified = true`, barrier. It assumes the triple (regexp, input, start) is consistent, so that the re-match succeeds: `createRegExpMatchesArray` returns `nullptr` on a miss (`runtime/RegExpMatchesArray.h:108-111`) and
`getBackref`/`getLastParen` dereference the result at once (`array->length()`, `runtime/RegExpGlobalData.cpp:60-63,80-83`). `leftContext`/`rightContext` take `jsSubstring(m_reifiedInput, 0, m_result.start)` / `(m_result.end, length - m_result.end)` (`runtime/RegExpCachedResult.cpp:109,127-128`); the range checks of `jsSubstring` are `ASSERT`s (`tryJSSubstringImpl`, `runtime/JSString.h:1134-1136`).
The collector reads `m_reified` to decide which four slots to visit (`:35-46`).

**AOT operations** record like the engine: `matchFromStart` (`aot/runtime/AOTRegExpOperations.cpp:58-66`), `findMatch` (`:114`), `operationAOTRegExpMatchLiteral` (`:211`), `operationAOTRegExpMatchLiteralWithoutObject` (`:260`), and everything that goes through `RegExpObject::exec/test/matchGlobal`, `replaceUsingRegExpSearch`, `regExpSplitFast`, `regExpSearchFast`.

**Does compiled code ever skip recording? Yes, on a whole-program proof.** Flow computes `LegacyRegExpStatics::{Unknown, NeverRead}` (`aot/valueflow/AOTValueFlowResultsBuiltins.cpp:1208-1222`: `NeverRead` when no unseen code runs, the `RegExp` constructor neither escapes nor is lost, and no member with `mutability == ChangesOnItsOwn` has a use). Consumers of `mayReadLegacyRegExpStatics()`:
- `addRegExpLiterals`: `constants.recordsMatch = legacyStaticsMayBeRead` (`aot/stubs/AOTStubPolicy.cpp:70-89`); the bound literal stub then emits no `recordMatch` for `test`/`search` (`aot/stubs/AOTStubsRegExpLiterals.cpp:218-241`).
- `findLiteralMatch`: `usesObject = flags has g|y|d || mayRead...` (`aot/select/AOTRegExpRules.cpp:104-106`). Without the object the site calls `regExpMatchLiteralWithoutObject`, "so that the match leaves nothing behind" (`aot/stubs/AOTStubsRegExp.h:16,34-35`); `finishRegExpExec(WithoutObject)` skips `advanceLastIndexAndRecordMatch` (`aot/stubs/AOTStubsRegExp.cpp:594-595`).
- `findAtomSearch`: `/abc/.test(s)` / `s.search(/abc/)` become a plain string search, the RegExp operand is dropped (`aot/select/AOTStringLiteralRules.cpp:236-264`).
- Constant folding of a match and deferring initialisers (`aot/passes/AOTFoldConstants.cpp:489-491`, `aot/passes/AOTDeferInitializers.cpp:1531`).
The generic stubs (`regExpMatch`, `finishRegExpExec(WithObject)`) and all C++ paths always record, so in a `NeverRead` program the seven stores still happen on those paths and nobody reads them.

**Diff in the two files.** `RegExpCachedResult.cpp`: only `NEVER_INLINE` + `JSC_REMOVABLE_DEFINITION(RegExp, ...)` on `lastResult`, `leftContext`, `rightContext`, `setInput`. `RegExpGlobalData.cpp`: the same markers plus `willReadLegacyStatics()` at the head of the four getters -> `AOT::willReadLegacyRegExpStatics(vm)`, which crashes in a validating run if the image carries `ProgramTables::neverReadsLegacyRegExpStatics` (`aot/runtime/AOTRuntimeValidation.cpp:148-152`; flag set at `aot/place/AOTProgramTableEncoder.cpp:146-147`; also called from `regExpConstructorInput`, `runtime/RegExpConstructor.cpp:203`). No change of layout or order.

### 9.5 `RegExpObject`, `lastIndex`, literals

**Layout**: 4 words (`static_assert` at `aot/stubs/AOTStubsRegExp.cpp:761`): header, butterfly, `uintptr_t m_regExpAndFlags`, `WriteBarrier<Unknown> m_lastIndex` (`runtime/RegExpObject.h:197-198`). Low bits: `lastIndexIsNotWritableFlag = 1`, `legacyFeaturesDisabledFlag = 2`, `sharedLiteralFlag = 4` (`:42-48`).
Writers of `m_regExpAndFlags`, both plain read-modify-write: `setRegExp` (`:66-71`, from `RegExp.prototype.compile`, `runtime/RegExpPrototype.cpp:362`) and `setLastIndexIsNotWritable` (`:115-118`, from `defineOwnProperty`, `runtime/RegExpObject.cpp:137`). `compile` stays available under AOT as a host call that `replacesContents` (`aot/builtins/regexp.builtins.ts`, entry `compile`).
`setLastIndex(uint64)` tests the flag, then `setWithoutWriteBarrier(jsNumber)` (`:78-89`).

**Compiled code and stubs**:
- `re.lastIndex` read: one `load64` (`aot/builtins/AOTRegExpBuiltinEmitter.cpp:58-62`). Write: `load64 m_regExpAndFlags`, test bit 0, `store64`, barrier if the value is a cell; not writable -> `putById` strict (`:51-56,64-82`). B3 heap: all of it is `AbstractHeap::RegExpState` (`:35-38`).
- Stubs: `loadRegExp` = load + mask (`aot/stubs/AOTStubsRegExpFrames.h:104-108`), `branchIfReadOnlyLastIndex` (`:87-90`), `resetLastIndex` = store boxed 0 (`:92-95`), `storeEndOfMatchInLastIndex` = `load32 output[1]`, box, store (`:97-102`).
  `matchFromLastIndex` (`aot/stubs/AOTStubsRegExp.cpp:247-272`) loads `lastIndex` once, requires Int32, not unicode, writable, `start <= length`; the value stays in a register, so the bound check cannot be undone by a racing store.
  Comment at `:420`: "Whoever starts at lastIndex has seen that it is writable": check at the start, store after the match.
- Each stub **re-loads `regExp` from the object after the match** (`matchAtom:363`, `advanceLastIndexAndRecordMatch:417`, `branchUnlessMatchArrayIsPlain:548-552` which then reads `m_numSubpatterns`, flags and `m_rareData` of whatever cell is there now). Compiled code does too (`numberOfGroupsOf`, `loadMatchedRanges`, `aot/builtins/AOTRegExpBuiltinEmitterPrefixes.cpp:71-94`).

**Literals in compiled code.** Three levels of laziness, three owners:
1. *RegExp cell*: one per (record, full flags) **per VM**, in `VMState::regExpLiterals` (`aot/runtime/AOTVMState.h:74`). `RegExpLiterals::regExpFor` (`aot/runtime/AOTRegExpLiterals.cpp:76-88`): lazily caches `m_records`; `m_cells.find<LiteralTranslator>`; on a miss `RegExp::createFromImage`, `m_cells.add`, `m_cellsInOrderOfCreation.append`. A `HashSet` and a `Vector`, **no lock**. Cells are immortal roots: `visitRoots` (`:55-69`) walks the vector from `m_numberOfCellsVisited`, and itself writes `m_numberOfCellsVisited`/`m_lastFullVisitVersion`; registered as `StrongRoots, GreyedByExecution` (`:71-74`).
2. *Statics slot*: per **Instance** (realm), offset placed at build time. IR: `NewRegExp (... regExpRecord, staticsOffset)` and `SharedRegExpLiteral` (`aot/ir/schema/operations.def:54`, `prepared-operations.def:95`).
   - `NewRegExp` (`aot/codegen/AOTAllocationEmitterObjects.cpp:20-37`): `load64` of the slot, call stub `newRegExpLiteral(record, flags, offset, sharedCell)`. Stub (`aot/stubs/AOTStubsRegExp.cpp:751-778`): slot null or no allocator -> operation; else allocate 32 bytes inline, store header (`Instance.allocation.inlineAllocations[RegExpObject].header`), butterfly 0, `loadRegExp(shared)` -> `m_regExpAndFlags` (flags cleared by the mask), `lastIndex = 0`, `fenceAfterAllocation`. **So every evaluation makes a new `RegExpObject`; the slot's object only serves as the carrier of the `RegExp*`.**
   - `operationAOTNewRegExpLiteral` (`aot/runtime/AOTRegExpOperations.cpp:341-353`): `regExpFor`; throw if invalid; `if (offset != none && !statics->at<JSCell*>(offset)) setSharedLiteral(RegExpObject::create(...))`; `prepareInlineAllocation`; return another new object.
   - `SharedRegExpLiteral` (`aot/builtins/AOTBuiltinCallEmitter.cpp:516-530`): `load64` slot; non-zero -> **that object is the value of the expression**; zero -> `operationAOTSharedRegExpLiteral` (`AOTRegExpOperations.cpp:355-365`, under `DeferTerminationForAWhile` + `DeferGCForAWhile`), which creates and stores unconditionally.
   - Publish order, `setSharedLiteral` (`:335-339`): object fully constructed -> `vm.mutatorFence()` -> `RememberedSlot::set` = store pointer, then `RememberedSlotSet::record` (card byte + `m_writtenCards.append`, `heap/RememberedSlot.h:47-59,125-129`). `mutatorFence` is a compiler fence on x86 and on ARM64 a `storeStoreFence` **only while `mutatorShouldBeFenced()`** (`heap/HeapInlines.h:123-134`). Readers use a plain load.
3. *Inline allocation data*: `prepareInlineAllocation` (`aot/runtime/AOTOperationScope.h:89-97`): `if (data.allocator) return; data.header = ...; data.allocator = ...localAllocator()`. Stubs test `allocator` and then load `header`.

**When is a literal shared?** `aot/passes/AOTShareRegExpLiterals.cpp`: the object must not escape and every use must be a listed built-in. `exec`/`test`/`execPrefix` only without `g`/`y` (`AdvancesIfGlobalOrSticky`, `:141-142,165-168`); `match`/`replace`/`replaceAll` also **with `g`** (`ResetsIfGlobal`, `:139-140,169-173`), justified by `:163`: "With g, these String methods store 0 first and stop at the failed match that stores 0 again."
The reasoning is per function and sequential ("A callee may evaluate the site of the literal again, and would find the two objects identical", `:108-109`). On the fast C++ paths the claim survives threads, because position is kept in locals: `replaceUsingRegExpSearch` only does `setLastIndex(0)` (`runtime/StringPrototype.cpp:366-369`, loop variables `:389-390`), `matchGlobal` likewise (`runtime/RegExpObject.cpp:198`). On the generic paths (`regExpReplaceGeneric`, `regExpMatchSlow`) `lastIndex` of the object *is* the loop variable; they are reached when `isSymbol*FastAndNonObservable` fails (9.6).
The bound literal stub relies on a type-level proof too: `singleRegExpLiteralAsCreatedOf` + `isProvenPristine` (`aot/select/AOTRegExpRules.cpp:66-69,93-96`); comment `aot/stubs/AOTStubsRegExpLiterals.cpp:38`: "The object stays as created, so lastIndex holds a number".

**Interpreted code** (`eval`, `new Function`): `op_new_reg_exp` makes an object per evaluation (`llint/LLIntSlowPaths.cpp:815-822`); `op_new_reg_exp_shared` (not part of the diff) caches one object in the op's metadata and re-validates watchpoints, `m_regExpAndFlags`, structure and `lastIndex == 0` on each use (`llint/LowLevelInterpreter64.asm:3627-3661`, `runtime/RegExpObjectInlines.h:136-145`). `LowLevelInterpreterAOT.asm` contains no regexp handler (grep: none).

### 9.6 Caches and watchpoints around RegExp

| Cache | Owner | Content, update | Cleared / visited |
| --- | --- | --- | --- |
| `StringReplaceCache` (`runtime/StringReplaceCache.h:43-72`) | `VM::stringReplaceCache` (`runtime/VM.h:694`) | 64 x {`RefPtr<AtomStringImpl> m_subject`, `RegExp*`, `JSCellButterfly* m_result`, `MatchResult`, `Vector<int> m_lastMatch`}. `get` returns `Entry*` (`StringReplaceCacheInlines.h:36-57`); `set` writes 5 fields one by one, may reset the neighbour (`:59-93`). User: `addToRegExpSearchCache` (`runtime/StringPrototypeInlines.h:815-882`): copies `entry->m_lastMatch`, `resetResultFromCache`, returns `entry->m_result` | cleared after a full GC (`heap/Heap.cpp:2339`); visited in Eden GCs (`runtime/VM.cpp:2299-2300`) |
| `StringSplitCache` (`runtime/StringSplitCache.h:45-89`) | `VM::m_stringSplitCache`, `LazyUniqueRef` (`runtime/VM.h:1022-1024`, `VM.cpp:316-318`) | 64 string + 256 regexp entries {subject, `RegExp*`, `JSCellButterfly*`, `MatchResult`}. `getForRegExp`/`setForRegExp` (`StringSplitCacheInlines.h:92-141`), used by `regExpSplitFast` (`runtime/RegExpPrototype.cpp:937-950,863`) | cleared after **every** GC (`heap/Heap.cpp:2342-2343`); not visited: an entry is valid only until the next collection |
| `RegExpSubstringGlobalAtomCache` (`runtime/RegExpSubstringGlobalAtomCache.h:44-59`) | `RegExpGlobalData`, per realm | 8 words: base, offset, length, regexp, count, matchEnd, `MatchResult`. `collectMatches` compares four, then takes three (`.cpp:59-71`), writes all eight at the end (`:162-172`). `startIndex = m_lastMatchEnd` is re-checked against the input (`input->length() >= startIndex`, `:104,116,134`) | visited (`:34-39`) |
| `AdaptiveStringSearcherTables` | `VM::m_stringSearcherTables`, lazy (`runtime/VM.h:298,1486`) | three `int` arrays that a search **populates for its pattern and then reads** (`WTF/wtf/text/AdaptiveStringSearcher.h:97-111`). Used by every C++ atom match (`runtime/RegExpInlines.h:187,351`) and by `aot/runtime/AOTStringBuiltinReplaceOperations.cpp:81,138` | - |
| Groups structure | `RegExp::RareData::m_cachedGroupsStructureID` | 9.1; one slot, keyed by realm | visited |
| Match array structures | `JSGlobalObject::m_regExpMatchesArrayStructure`, `...WithIndicesStructure` (`runtime/JSGlobalObject.h:444-445`) | set at init (`JSGlobalObject.cpp:1392-1393`), replaced by `haveABadTime` (`:2996-2998`). AOT copies the header word into `Instance.allocation.inlineAllocations[RegExpMatchesArray]` on first use (`aot/runtime/AOTRegExpOperations.cpp:150`); the stub guards with `branchIfHavingABadTime` (`aot/stubs/AOTStubsRegExp.cpp:576`) | - |
| `VMState::regExpMatchOutput` | VM | `max(image maxRegExpOutputSize, 2*(15+1))` ints, allocated in `attachImage` (`aot/runtime/AOTVMState.cpp:65`, `AOTVMState.h:49-50,75`) | freed in `releaseImage` |

The stub's atom search does not use the searcher tables: it calls the leaves `operationAOTStringFind8` / `FindCharacter8` (`aot/stubs/AOTStubsRegExp.cpp:211-239`, `aot/runtime/AOTStringBuiltinLeaves.cpp:132-135`).

**Watchpoints.** `m_regExpPrimordialPropertiesWatchpointSet`, `m_regExpPrototypeTestWatchpointSet`, `m_regExpSpeciesWatchpointSet`, `m_regExpRecompiledWatchpointSet` (`runtime/JSGlobalObject.h:546,569-570,578`), armed by adaptive watchpoints on `RegExp.prototype` (`JSGlobalObject.cpp:2370-2384`).
The C++ that AOT operations call **still reads them**: `isSymbol{Match,Search,MatchAll,Replace,Split}FastAndNonObservable` (`runtime/RegExpObjectInlines.h:37-207`) = watchpoint(s) valid, `lastIndex` is a number, structure is `regExpStructure()` (or same prototype and no custom properties). `compile` still fires `regExpRecompiledWatchpointSet` (`runtime/RegExpPrototype.cpp:360`).
Compiled code and stubs do **not** read watchpoints. What replaces them:
- The table locks the built-in: `lockedObjects: ["prototype"], lockedMembers: ["constructor"], mutability: "AlwaysLocked"` (`aot/builtins/regexp.builtins.ts`, class `RegExp`); per the PR text no name can be replaced *or added* on `RegExp.prototype` in the program's realm. So the primordial set cannot fire there; realms of `node:vm`/`ShadowRealm` are not locked (`locksBuiltins` needs `moduleHost`, `aot/runtime/AOTRealmStartup.cpp:51-54`).
- Per-object purity is a *type* fact: overloads take `pristineRegExp = b.instance("RegExp").pristine("Names")`; the emitters are selected only under that proof or behind a guard.
- Two stubs re-check at run time by one compare: `checkOriginalRegExp` = `structureID == Instance.receiverStructureID(Receiver::RegExp)` (`aot/stubs/AOTStubsRegExp.cpp:669-673`, filled at `aot/runtime/AOTInstance.cpp:254`), used by `stringReplaceRegExpMayNotMatch` and `stringMatchRegExp`.

### 9.7 AOT files and the fast paths

**`aot/runtime/`**
- `AOTRegExpRecord.h`: image formats `ImageRegExp`, `ImageRegExpHash`, `ImageRegExpCharacterSet`, `ImageRegExpNamedGroups`.
- `AOTRegExpRecords.{h,cpp}`: read-only views of the three sections + code range; `findRegExpRecord`, entry points, atom/pattern strings, named-group decoding.
- `AOTRegExpLiterals.{h,cpp}`: per-VM set of literal `RegExp` cells (9.5).
- `AOTRegExpOperations.cpp`: `operationAOTRegExp{Exec,Test,TestBoxed,Find,MatchLiteral,MatchLiteralWithoutObject,MatchFlatProgram8,ExecPrefix,Flags,Source}`, `operationAOTStringMatchPrefix`, `operationAOT{NewRegExpLiteral,SharedRegExpLiteral,NewRegExp}`.
- (`AOTStringBuiltinReplaceOperations.cpp`: `operationAOTString{Replace,ReplaceAll,ReplaceRegExpString,Split,Match,Search}RegExp`.)

**`aot/builtins/`**: `regexp.builtins.ts` (table rows: types, effects on heap `RegExpState`, inline forms); `AOTRegExpBuiltinEmitter.{h,cpp}` (B3 for 13 inline built-ins; mostly a stub or operation call); `AOTRegExpBuiltinEmitterPrefixes.cpp` (`exec`/`match` whose result is destructured: no array, reads offsets straight from `RegExp::m_ovector`); `AOTSimplifyBuiltinCallsRegExps.cpp` (`s.match(r)` -> `r.exec(s)` when `r` is never global).
**`aot/stubs/`**: `AOTStubsRegExp.h` (keys, forms); `AOTStubsRegExpFrames.h` (match frame, context, lastIndex helpers); `AOTStubsRegExp.cpp` (`regExpMatch` x4, `finishRegExpExec` x2, `stringMatchRegExp`, `stringReplaceRegExpMayNotMatch`, `newRegExpLiteral`, `recordMatch`); `AOTStubsRegExpLiterals.cpp` (`regExpMatchLiteral`: one thunk per record and form, constants baked in).
**`aot/image/`** (build time unless noted): `AOTRegExpRequests.*` (which patterns the image gets: literals, evaluated texts, flow candidates); `AOTRegExpSummary.*` (parse once per pattern: metadata, atom, groups; has its own `Lock`); `AOTRegExpCompiler.*` (drives `Yarr::generateCode`, 8- and 16-bit bodies); `AOTRegExpCodeAddressing.*` (PC-relative addressing, shared/failure routines); `AOTRegExpCode.h` (relocatable body + symbols); `AOTRegExpSection.*` (layout, dedup of bodies, routines, tables); `AOTRegExpRecordEncoder.*` (records, hashes, details, `maxOutputSize`); `AOTRegExpInProcessLoader.*` (run time only under `useRegExpImageMode`, "jsc and testRegExp", `runtime/OptionsList.h:165`).
**Elsewhere**: `aot/select/AOTRegExpRules.cpp`, `aot/codegen/AOTRegExpMatchLiteral.cpp`, `aot/passes/AOTShareRegExpLiterals.cpp`, `aot/passes/AOTFoldConstantRegExps.cpp`, `aot/ir/AOTCheckRegExpLiterals.cpp` (diagnostic for invalid literals), `aot/ir/AOTRegExpFlags.h`.

**`re.test(s)` / `re.exec(s)`, `re` a known literal** (`RegExpMatchLiteral`; `aot/codegen/AOTRegExpMatchLiteral.cpp:18-37` -> `generateRegExpMatchLiteral`, `aot/stubs/AOTStubsRegExpLiterals.cpp:276-342`):
load `StringImpl*` (rope -> out of line), characters, length; for `g`/`y`: read-only test, `lastIndex` load (Int32 >= 0); compare with the constant `minimumSize`; `emitMatch` (`:179-202`): push frame, save operands in it, `output = VM.aotState.regExpMatchOutput`, context on the stack, **direct call** of `{SymbolSpace::RegExp, record, 8|16}`; `-1` -> miss (reset `lastIndex` if used); `< -1` -> out of line;
`test`/`search`: optional `lastIndex` store and `recordMatch`, answer; `exec`/`match`: jump to `finishRegExpExec`. The RegExp *cell* is not touched at all unless `recordsMatch`. Out of line -> the generic stub or operation below.

**`re.test(s)` / `re.exec(s)`, any RegExp** (`generateRegExpExec/Test`, `aot/builtins/AOTRegExpBuiltinEmitter.cpp:100-118` -> `generateRegExpMatch`, `aot/stubs/AOTStubsRegExp.cpp:611-667`):
flat subject (substring ropes are read in place; other ropes are flattened and the stub restarts); `lastIndex` must be a number; `regExp`, flags; `g|y` -> `matchFromLastIndex`; `m_state != ImageCode` -> `matchAtom` (`m_atom` non-null, both 8-bit: leaf search, then store the two offsets) -> else `matchOnFlatProgram` (`:275-315`: `m_state == ByteCode`, 8-bit, groups <= 15 for the VM buffer, `load m_regExpBytecode`, `load m_flatProgram` or `...For8BitSubjects` **with no null test of the bytecode pointer**, bump a counter, leaf `operationAOTRegExpMatchFlatProgram8`);
`ImageCode`: `m_regExpJITCode` -> `m_ref8/m_ref16` (null -> slow), `m_minimumSize`, indirect call. Found: Boolean forms -> `advanceLastIndexAndRecordMatch`; Array -> `finishRegExpExec(WithObject)`. Anything else -> `operationAOTRegExp{Exec,Test,Find,TestBoxed}` -> `RegExpObject::exec/test` -> `RegExp::matchInline` (9.1).

**`finishRegExpExec`** (`:555-609`): needs no `d`, no rare data, groups <= `maxGroupsOfMatchArray`, not having a bad time, allocator prepared. `tryCreateMatchArray` (`:518-543`, "Nothing here calls anything, so the collector sees either the whole array or none of its parts"): allocate butterfly, **re-load the VM buffer**, store `index`/`input`/`groups`, then per group `start = output[2i]`, `length = output[2i+1] - start`, `makeSubstringOfFlatString`, which builds a substring rope (or reads `characters[start]` for length 1) **with no comparison against the subject's length** (`aot/stubs/AOTStubsStringSubstrings.cpp:23-71`). Failure of any allocator -> `operationAOTRegExpMatchLiteral(object|0, string, record, Exec)`, which reads the offsets out of `regExpMatchOutput` *after* the transition to C++ (`aot/runtime/AOTRegExpOperations.cpp:197-202`).

**Destructured `exec`/`match`** (`execBoundsInline`, `matchPrefixInline`, `AOTRegExpBuiltinEmitterPrefixes.cpp:112-169`): stub `regExpMatch(Found)` with `OffsetVectorOwner::RegExp`; back in compiled code, `numberOfGroups` and the ranges are loaded from the cell's `m_ovector` and become substrings or raw bounds.

**`s.replace(re, str)`**: stub `stringReplaceRegExpMayNotMatch` (`:682-733`): structure check, number `lastIndex`, not sticky, writable if global, reset `lastIndex` if global, run image code or atom search into the VM buffer; **miss -> return `s`**; a hit (or anything odd) -> `operationAOTStringReplaceRegExpString`, which matches again from 0 via `replaceUsingRegExpSearch` -> `performMatch(..., &ovector)` loops on `regExp->ovectorSpan()`. Function replacement / `replaceAll`: operation at once (`AOTRegExpBuiltinEmitter.cpp:136-166`).
**`s.match(re)`**: stub `stringMatchRegExp` (`:735-749`): structure check, not global -> swap operands, jump to `regExpMatch(Array)`; else `operationAOTStringMatchRegExp` -> `regExpMatchFast` -> `matchGlobal`. **`s.search(re)`**, **`s.split(re)`**: operations (`AOTRegExpBuiltinEmitter.cpp:168-195`).

### What helps (RegExp)

- Image code, shared routines, tables, records, hashes and flat programs are immutable; image code has no process addresses and no global stores (9.2). Backtracking is on the caller's stack or in call-local memory for two of the three matchers.
- There is no code patching, no tier-up, no `matchConcurrently` client, no JIT failure bookkeeping. `ImageCode` never changes meaning: even a racing `deleteCode` leaves the `YarrCodeBlock` allocated and the stub null-tests the entry point (`aot/stubs/AOTStubsRegExp.cpp:176`).
- Literal cells are immortal and never lose their state (not in `RegExpCache`), and the bound literal stubs do not read the cell.
- The interpreter pool already has a real lock; `RegExpCache::m_weakCache` too; `m_firstCharacterBitmap` is a `ThreadSafeLazyUniquePtr` (only DFG uses it, `dfg/DFGGraph.cpp:1666`).
- `lastIndex` and `m_regExpAndFlags` are single aligned words; stubs bound-check the register copy.
- `NeverRead` programs need no legacy statics at all; locked `RegExp.prototype` removes the watchpoint problem from compiled code.
- The output pointer is already a *parameter* of every matcher: giving each thread its own buffer changes `loadMatchOutput`/`loadOffsetVector`, `ovectorSpan()` callers and `loadMatchedRanges`, not the matchers.

### Hazards with several mutators (RegExp)

MS = memory safety (wild read/write, use after free, null call); JS = only a wrong JavaScript value.

1. **`VMState::regExpMatchOutput`, one per VM. MS.** Written by image code, flat programs and `matchAtom` from stubs; read back *during* the match (back references, paren contexts, match start) and *after* it by `finishRegExpExec`, `recordMatch`, `storeEndOfMatchInLastIndex`, `operationAOTRegExpMatchLiteral`. Offsets of another thread's longer subject become substring ropes or `characters[start]` loads with no range check; a back-reference loop that ends on `patternIndex != output[end]` can run off the subject.
2. **`RegExp::m_ovector`, one per shared cell. MS.** Same failure through `performMatch`, `createRegExpMatchesArray`, `findMatch`, `fillPrefixOfMatch`, `operationAOTRegExpExecPrefix`, `regExpMatch(Found)` + `loadMatchedRanges`, `replaceUsingRegExpSearch` (`int* ovector` kept across `jsSubstring` allocations and, in the callback loop, refreshed only per iteration), `resetResultFromCache`, `StringReplaceCache::set(..., regExp->ovectorSpan())`. `subpatternIdForGroupName` uses a value from the vector as an **index** into it (`runtime/RegExp.h:188`, used at `runtime/StringPrototype.cpp:442-449`). Hit by the most ordinary program: two threads, one literal.
3. **`m_state` published before `m_regExpBytecode`** (`RegExp.cpp:574/597` vs `598`; `670/693` vs `699`), readers unlocked (`hasCodeFor`, `matchInlineOnce`, stub `matchOnFlatProgram`). **MS** (null dereference; on ARM64 also `ImageCode` visible before `m_ref8/16`, which the stub survives by its null test but `matchWithImageCode` -> `execute` does not).
4. **`deleteCode` / `reset()` while another thread matches. MS** (use after free of `BytecodePattern`/`FlatProgram`). Guarded only by "VM idle" = `!entryScope`. `reset()` is unlocked and runs on the matching thread after a soft error.
5. **`BytecodePattern::m_bytecodeBehindFlatProgram`** lazy `unique_ptr` at match time. **MS** (second assignment frees the bytecode the first thread is interpreting).
6. **`RegExpLiterals::m_cells` / `m_cellsInOrderOfCreation`** unlocked hash set + vector, mutated at first evaluation of any literal (also from `resolveMatchedRegExp`). **MS** (rehash/realloc under a reader), plus duplicate cells.
7. **`RegExpCache` rings**: `m_nextEntryInStrongCache` equality wrap -> index past the array; double creation of `m_parsedPatterns`. **MS.** `lookupOrCreate` double create, `m_emptyRegExp`: JS-invisible.
8. **`RegExpCachedResult`**: 7-store `record` vs. `record` vs. `lastResult`. A torn (regexp, input, start/end) makes the re-match miss -> `nullptr` array dereferenced; `leftContext/rightContext` substring out of range; `m_oneCharacterMatch` with a regexp that has no atom (`atom().codeUnitAt(0)` behind `ASSERT`s, `RegExpCachedResult.cpp:67-73`); the reification sequence itself is a long check-then-act on `m_reified`, which the collector also reads. **MS.** Even without tearing the *meaning* ("the last match") is undefined across threads: **JS**.
9. **`AdaptiveStringSearcherTables`, per VM. MS.** A search populates the tables for its pattern and trusts them: `shift = j - charOccurrence(...)`, `index += shift`, only the upper bound is tested before `subjectPtr[index + j]` (`WTF/wtf/text/AdaptiveStringSearcher.h:397-404`); a table filled for a longer pattern gives a negative shift. Its `LazyUniqueRef` creation is also unsynchronised.
10. **`StringReplaceCache`, `StringSplitCache`. MS.** `RefPtr<AtomStringImpl>` and `Vector<int>` members assigned field by field; `get` returns a pointer into the table that the caller then copies from; a racing `set`/`entry2 = {}`/GC-end `clear()` frees `m_lastMatch`'s buffer or double-derefs the subject. A torn but live entry gives another call's result (**JS**). Split entries hold unmarked butterflies and rely on "cleared at every collection".
11. **`RegExpSubstringGlobalAtomCache`**: 8-word entry, compare-4-take-3. Wrong count or wrong `lastResult` -> **JS** for the array (start index is re-checked), but the bad `lastResult` goes into `recordMatch` and from there into hazard 8.
12. **Re-loading `regExp` from the object after the match + `RegExp.prototype.compile` on another thread. MS.** `branchUnlessMatchArrayIsPlain` takes `m_numSubpatterns` of the *new* cell and `tryCreateMatchArray` reads that many pairs from `regExpMatchOutput`, whose size only covers image records and 15 groups; `loadMatchedRanges` mixes the new cell's vector with the old match. Also `setRegExp` vs `setLastIndexIsNotWritable`: lost update of one word (**JS**: a frozen `lastIndex` becomes writable again, or the old pattern comes back).
13. **`lastIndex`**: read-match-write is not atomic; "has seen that it is writable" is check-then-act. **JS** only (value is boxed Int32 or any JSValue with a barrier; the start offset is checked in a register).
14. **Shared literal objects** (`SharedRegExpLiteral`, and `op_new_reg_exp_shared` in interpreted code): one `RegExpObject` per site per realm stands for "a new object each time" on a sequential argument. With `g` under `match`/`replace`/`replaceAll`, threads share `lastIndex`: harmless on the fast paths (only 0 is stored), **JS** (skipped or repeated matches, non-termination is conceivable) if a generic path that loops on `lastIndex` is taken.
15. **Statics slot of a literal**: check-then-create in two operations (two objects, last store wins: invisible for `NewRegExp`, and for `SharedRegExpLiteral` two evaluations may see different objects, invisible by the pass's own conditions). Publication has no fence on ARM64 unless the collector asked for one -> a reader can see the pointer before the cell's words: **MS**. `RememberedSlotSet::record` appends to a `Vector` unlocked: **MS** (general statics problem, not regexp-specific).
16. **`prepareInlineAllocation`**: `header` then `allocator`, no fence; and the `LocalAllocator` it publishes is a single-mutator free list used inline by `newRegExpLiteral`, `tryCreateMatchArray`, `makeSubstringOfFlatString` (`ropeStringAllocator`, `auxiliarySpace`). **MS**; belongs to the heap section.
17. **Stack limit of the wrong thread** (`Instance::stackMirror`, `vm.softStackLimit()`). On a thread whose stack lies below the recorded limit every framed pattern answers `JITCodeFailure` (slow, and with `crashIfInterpreterIsUsed` a crash, `RegExp.cpp:442-448`); above it the check never fires and deep paren contexts run into the guard page. Crash rather than corruption as far as I can tell; the Yarr interpreter has no stack check at all.
18. **`ensureGroupsStructure`**: read-compare-build-set of one structure ID; both threads build by `addPropertyTransition` on shared structures. The slot itself: **JS**-neutral. The transitions: see the structures section.
19. **`VM::m_executingRegExp`**: set/cleared per C++ match; with two threads the profiler may read a stale or null cell. Diagnostic only, but the pointer is not a root.
20. **`m_lastUseEpoch`, runtime counters** (`AOT::count`, `++values[i]`, `aot/runtime/AOTRuntimeCounters.cpp:58-67`; stub `add64`/load-add-store, `aot/stubs/AOTStubHelpersValues.cpp:276-288`): lost updates. Harmless, except that exact counts are a test oracle.
21. **`m_regExpAllocatorLock` held for a whole interpreted match**: safe, but one catastrophic pattern blocks every other interpreted match of the VM, and a waiter is blocked in C++ with heap access. Liveness with stop-the-world requests must be designed.
22. **Having a bad time** between `branchIfHavingABadTime` and the inline array allocation with the cached header: an array with the old structure appears after the switch. Same class as every inline array allocation.
23. **Whole-program facts proved sequentially**: `NeverRead`, `AsCreated`/pristine literals, "the object does not escape". They are flow facts over all code, so a second thread running *the same program* does not add uses; but the per-function ordering arguments of `AOTShareRegExpLiterals` and `LastIndexUse` do assume that nothing runs between two operations on the object.

### Not found / not sure

- `VM::m_regExpPatternContexBuffer`, `acquireRegExpPatternContexBuffer` and its lock: **not in this tree** (no grep hit). Paren contexts are on the stack. Likewise `RegExpGlobalData::m_ovector` does not exist; the vector is `RegExp::m_ovector`. I did not check whether that move is upstream's or the fork's (it is not in the diff; only `offsetOfOvector()` is).
- I did not read all 2,766 changed lines of `YarrJIT.cpp`. "No store to shared data" rests on: the `CodeAddressing` interface, `ImageCodeAddressing`, a grep of stores in `YarrSharedRoutines.cpp`, the failure reason `ProcessAddressInCode`, and the paths quoted. I did not verify page protection of the regexp section at run time.
- Whether `RegExpLiterals::visitRoots` and `RegExpCache::visitAggregate` can run while a mutator runs: the constraints are declared `ConstraintConcurrency::Concurrent` (`aot/runtime/AOTHeapHooks.cpp:74-97`) while a comment says '"At" runs once per stop of the mutator' (`:55`). Left to the heap section.
- `weakAdd`/`weakRemove` behaviour when two cells exist for one key: not read.
- Atom strings: `decodeRegExpNamedGroups` calls `ImageText::createAtom`, `ensureGroupsStructure` calls `Identifier::fromString`; which atom table a second thread would use, and the non-atomic `StringImpl` reference counts of `m_atom`/`m_patternString` copies (`String pattern = regExp->pattern()`, `RegExpCache.cpp:131`), were not examined here.
- Whether the first `compile` of a lazily constructed pattern can change `m_atom` from null to a string (it assigns unconditionally, `RegExp.cpp:273`; creation keeps "only pattern characters" patterns out of the lazy path, `:209`). If it can, stubs that load `m_atom` race with a `String` assignment.
- `regExpReplaceGeneric`, `regExpMatchSlow`, `regExpSplitSlow`, `RegExpStringIterator`, `collectGlobalAtomMatches`, `removeAllUsingRegExpSearch`, `replaceAllWithStringUsingRegExpSearch`: not read line by line; I assumed they reach the matcher only through `performMatch`/`RegExpObject::exec`.
- `strings.substring(...)` of `StringEmitter` (used by `matchPrefixInline`): I did not check whether it range-checks; the stub twin `makeSubstringOfFlatString` does not.
- How the Bun half calls RegExp from native code (e.g. `Bun.Glob`, router, `JSC::Yarr` users outside `RegExp`) is outside this section.

---------------------------------------------------------------------------------------------------------------------------------------

## 10. Per-VM, per-realm and per-object caches and lazily initialised state in built-in C++ code

Paths are relative to `Source/JavaScriptCore/` unless they start with `wtf/` (= `Source/WTF/wtf/`). Everything below was read in
`<engine>/` at 5c2923e566cb. "AOT hunk" means a difference against 8deea350d4e1.
Not covered here (other reports): RegExp state, `aot/runtime/AOTPropertyCaches*`, the collector and allocators, the JIT tiers.

### 10.0 Legend, and four facts that decide most rows

**Severity** — **M** memory safety (use after free, double free, out-of-bounds, type confusion, a cell seen before it is initialised);
**C** deterministic crash (`RELEASE_ASSERT`, null dereference); **I** two objects where JS must see one; **V** wrong JS value only;
**P** only a lost cache fill; **—** no hazard found.
**Rule** (suggestion) — **PT** one copy per JS thread; **LK** lock; **LF** lock-free (publish one word with CAS/release, readers acquire);
**EG** initialise eagerly before the first second thread starts; **OFF** switch the cache off in a VM with threads; **STW** do it with all JS threads stopped.

1. **`StringImpl` reference counts are atomic, the slots that hold the references are not.** `std::atomic<uint32_t> m_refCount` (wtf/text/StringImpl.h:162),
   `ref()` is `fetch_add` (:1211), `deref()` has a shortcut: "When this is the only reference nobody else can be racing to add one, so skip the atomic
   read-modify-write" (:1222-1228). So *every* cache slot of type `String` / `RefPtr<…StringImpl>` / `Ref<…>` that one thread overwrites while another copies it
   is a use after free: reader loads the pointer, writer swaps and derefs to zero and destroys, reader calls `ref()` on freed memory. Two writers on one slot deref the old
   value twice. This is the shape called "refcounted swap" below and it is always **M**.
2. **`m_hashAndFlags` is a plain `unsigned`** (wtf/text/StringImpl.h:172) updated with `|=`/`&=`: `setHash` (:1200), `setIsAtom` (:1179-1181), `cost()` sets `s_hashFlagDidReportCost` (:1153),
   `setNeverAtomize` (:371). Two of these at once on one string lose a bit. Losing the hash is harmless (recomputed), losing `isAtom` after the string was put in the atom table is not
   (the destructor then does not remove it from the table: dangling table entry, **M**).
3. **`ConcurrentJSLock` is a real lock in this tree whatever the JIT setting**: `using ConcurrentJSLock = Lock;` (runtime/ConcurrentJSLock.h:34). `JSCell::cellLock()` is real too. But the
   existing locks were placed for "one mutator against compiler threads and the collector": the mutator *writes* under the lock and *reads* without it
   (comment in runtime/StructureCache.cpp:45: "We don't need to lock here because only the main thread can get here, and only the main thread can mutate the cache").
4. **Interpreted code exists only if the image admits unknown JavaScript.** `refuseEvaluationUnlessAdmitted` disables eval in every realm of a closed program
   (aot/runtime/AOTRealmStartup.cpp:128-132). All state that only bytecode needs is collected in 10.8 and can get one coarse rule.

Three update shapes recur: (a) *null-check, create, store one word* (duplicate creation; on ARM64 also publication without a fence unless noted);
(b) *key and value in separate words of a direct-mapped entry* (torn entry: the key of one fill with the value of another); (c) *`WTF::HashMap`/`Vector` growth* (rehash or realloc frees the
storage a reader is walking: always **M**).

### 10.1 `class VM` member by member (runtime/VM.h:262-1580)

#### 10.1.1 State of "the" running thread (not caches; must become per thread by construction)

| Member (VM.h line) | Written at run time by | Remark |
|---|---|---|
| `disallowVMEntryCount` (419) | scopes; also `AllowLazyMaterializationOfImmutableProperties` in debug (VM.h:1626, 1634) | read at every entry: aot/runtime/AOTCompiledCall.cpp:63, 97; interpreter/Interpreter.cpp:1524, 1805, 1912, 2224. A scope of thread A forbids entry to thread B. PT |
| `m_exception`, `m_lastException` (421, 423) | `setException` VM.cpp:1342-1349 (two stores + `fireTrap(NeedExceptionHandling)`), `clearException` VM.h:1399-1408 | B would see, catch or clear A's exception. `lastException` is also cleared by `shrinkFootprintNow` (VM.cpp:1242). PT |
| `m_terminationException` (422) | `ensureTerminationException` VM.cpp:877-884, shape (a) | shared singleton compared by pointer (`isTerminationException` VM.h:362-366): a duplicate makes a termination look like an ordinary exception (**V**, catchable termination). EG |
| `topCallFrame`, `topEntryFrame` (431-432) | every call boundary; "sometimes treated as a pair in assembly code" (VM.h:429-430, static_assert :1582) | PT |
| `maybeReturnPC` (433), `topJSPIContext` (434) | interpreter/FrameTracers.h:134 | PT |
| `m_entryScopeServicesRawBits` (444) | one plain `OptionSet` byte and one atomic byte in one `uint16_t` (439-456); read as 16 bits (398) | plain half is RMW (`add`/`remove`, 402-405, 487-490). PT or all-atomic |
| `didEnterVM` (459) | aot/runtime/AOTCompiledCall.cpp:50, 82; saved/restored AOTBuiltinCells.cpp:269-272, AOTRuntimeValidation.cpp:448 | save/restore pattern: PT |
| `allowLazyMaterializationOfImmutablePropertiesCount` (465) | scope VM.h:1617-1641 | **see 10.1.6** |
| `m_apiLock` (469) | `JSLock::didAcquireLock` runtime/JSLock.cpp:118-160: installs the VM's atom table in the *thread* (:126), `setLastStackTop`, heap access, `RELEASE_ASSERT(!m_vm->stackPointerAtVMEntry())` (:138), `setStackPointerAtVMEntry` (:140) | the single-owner protocol itself; `currentThreadIsHoldingAPILock()` is asserted in dozens of places (e.g. VM.h:1216, VM.cpp:1596, 1315) |
| `m_threadContext` = `VMTraps` + `StackManager` (470; runtime/VMTraps.h:334-356) | soft stack limit, `Atomic<BitField> m_trapBits`, plain `m_deferTerminationCount`, `m_trapsDeferred`, `m_isInBlockingScope`… | trap bits are atomic; the counters and the stack limit are one thread's. Compiled code addresses it at `offsetOfTraps()` (VM.h:878-891). PT |
| `m_stackPointerAtVMEntry`, `m_currentSoftReservedZoneSize`, `m_stackLimit`, `m_lastStackTop` (1431-1434) | `updateStackLimits` VM.cpp:1504-1548 computes from `Thread::currentSingleton().stack()` | `sanitizeStackForVM` `RELEASE_ASSERT(stack.contains(vm.lastStackTop()))` (VM.cpp:1856): **C** at once with two threads. PT |
| `encodedHostCallReturnValue`, `newCallFrameReturnValue`, `callFrameForCatch`, `targetMachinePCForThrow`, `targetMachinePCAfterCatch`, `targetInterpreterPCForThrow`, `targetInterpreterMetadataPCForThrow`, `targetTryDepthForThrow`, `varargsLength`, `osrExit*` (950-962) | llint/LLIntSlowPaths.cpp:2333, 2365, 2494-2495, 2518-2524, 2578; bytecode/RepatchInlines.h:125, 149; read in llint/LowLevelInterpreter64.asm:404 | "return value through the VM" between a slow path and the next instructions. **M** if shared (wild PC / frame). PT |
| `m_executingRegExp` (963), `m_regExpAllocator` + lock (1009-1010), `m_regExpCache` (1008) | — | RegExp report |
| `interpreter` (994), `entryScope` (995) | runtime/VMEntryScope.cpp:41, 65 | `isEntered()`, `whenIdle` (VM.cpp:1175-1183: `if (!entryScope) { callback(); return; }`), `shrinkFootprintNow` (VM.cpp:1230) all read "no JS is running" from this one pointer. PT + a VM-wide count |
| `stringRecursionCheckFirstObject`, `stringRecursionCheckVisitedObjects` (998-999) | runtime/StringRecursionChecker.h:57-100 | cycle guard of `Array.prototype.join/toString`. Shared: B joining an array that A is joining gets `""` (**V**); the `HashSet` add/remove is shape (c) **M**. PT |
| `m_checkpointSideState` (1477) | VM.cpp:1594-1636 | JIT only. PT |
| `m_scratchBuffers` (1474-1476) | `scratchBufferForSize` VM.cpp:1932-1953 under `m_scratchBufferLock` | lock guards the list only; "You can only write to entries in the ScratchBuffer from the main thread" (VM.h:965-968); every caller gets the *same* last buffer (VM.cpp:1951). Callers with JIT off not searched |
| `m_synchronousModuleQueue` (1536) | runtime/JSModuleLoader.cpp:1458-1461 points it at a **stack frame**; appended from runtime/JSPromise.cpp:562 | B settling a loader promise appends to a `Vector` on A's stack: **M**. PT |
| `m_drainMicrotaskDelayScopeCount` (1494) | VM.cpp:2533-2548 | plain counter; PT or atomic |
| `m_didPopListeners` (1560) | `whenIdle` VM.cpp:1181; drained VM.cpp:2178-2183 | `Vector` append, shape (c) **M**; semantics "when idle" need a definition for N threads. LK |
| `m_aboutToBeNotifiedRejectedPromises` (1497) | `promiseRejected` VM.cpp:1804-1807; drained VM.cpp:1789-1802 | `Vector<Strong<>>`: **M**. PT (with the microtask queue) or LK |
| `m_currentWeakRefVersion` (1507) | `finalizeSynchronousJSExecution` VM.h:1214-1219 (`++`, asserts the API lock) | see 10.7.12 |
| `m_mightBeExecutingTaintedCode` (565) | interpreter/Interpreter.cpp:1862, runtime/CommonSlowPaths.cpp:1266; cleared VM.h:1218 | interpreter only. PT |
| `m_deletePropertyMode` (1453) | `DeletePropertyModeScope` VM.h:786-803 | **see 10.1.6** |
| `currentlyDestructingCallbackObject(+ClassInfo)` (672-673) | C API finalisers | PT / sweeper |
| `m_failNextNewCodeBlock`, `m_entryCountFromOutside`, `m_unlinkedCodeKeepers` ("the VM's thread only", 1458), `m_isDeletingAllCodeToGenerateItAgain`, `m_activeHeapAnalyzer` (1449-1465) | VM.h:913-919, 1080-1092, 1147 | plain counters/flags. LK or atomic |
| `m_hasSideData`, `m_hasTerminationRequest`, `m_executionForbidden`, `m_executionForbiddenOnTermination`, `m_isDebuggerHookInjected` (1542-1546) | VM.cpp:1307-1311, VM.h:333-345 | separate `bool`s, monotone except `m_hasTerminationRequest`; termination of *which* thread is a design question |
| `m_topExceptionScope`, `m_needExceptionCheck`, `m_nativeStackTraceOfLastThrow`, `m_throwingThread`… (1436-1443) | only `ENABLE(EXCEPTION_SCOPE_VERIFICATION)` | PT in debug builds |
| `m_crossTaskToken` (467), `m_onEachMicrotaskTick`, `m_hostObservesMicrotaskBoundaries`, `m_onComputeErrorInfo*`, `m_onAppendStackTrace`, `m_computeLineColumnWithSourcemap` (1499-1505) | embedder setters VM.h:1198-1204, 1250-1253; VM.cpp:635-651 | `WTF::Function` move-assign while another thread calls it: **M**. Set once before threads (EG) |

#### 10.1.2 Written only in the constructor (VM.cpp:267-626) — no rule needed beyond "construct before sharing"

All `WriteBarrier<Structure>` members VM.h:574-626 (stores at VM.cpp:368-435; I did not see a store to `hashMapBucketSetStructure`/`hashMapBucketMapStructure`/`webAssemblyCalleeGroupStructure` in the
constructor; they occur only in VM.h and the visit list of VM.cpp), `m_orderedHashTableDeletedValue`, `m_orderedHashTableSentinel`, `m_sortScratchSentinel` (VM.cpp:436-438), `m_sentinelStructure` and the eleven `m_fast*Sentinel`
(VM.cpp:440-454), `heapBigIntConstantOne/Zero` (VM.cpp:461-482), `propertyNames` (`new CommonIdentifiers`, VM.cpp:379), `emptyList`, `m_neverExecutedCallSiteData` and siblings (VM.cpp:302-304),
`m_typedArrayController` (VM.cpp:531), `m_bytecodeIntrinsicRegistry` (VM.cpp:977-982), `m_identifier` (`VMIdentifier::generate()`, an `AtomicObjectIdentifier`, VM.h:260), the `const Ref/UniqueRef` members.
**`smallStrings` is fully eager**: `initializeCommonStrings` creates the empty string, all 256 single-character strings and the named ones (runtime/SmallStrings.cpp:44-82); afterwards only the collector writes `m_needsToBeVisited` (:87).
`numericStrings.initializeSmallIntCache` pre-fills 0-9 only (runtime/NumberPrototype.cpp:519-528).
`m_cachedSortScratch` (VM.h:659) is touched only by DFG/FTL code (dfg/DFGSpeculativeJIT.cpp:15341-15397, ftl/FTLLowerDFGToB3.cpp:11048-11118).

#### 10.1.3 Lazily created singletons

| Member | Declaration | Creation | Lock | A racing thread sees | Sev. | Rule |
|---|---|---|---|---|---|---|
| `m_hasOwnPropertyCache`, `m_megamorphicCache`, `m_stringSplitCache`, `m_shadowChicken`, `m_heapProfiler`, `m_stringSearcherTables` (`LazyUniqueRef`), `m_watchdog` (`LazyRef`) | VM.h:1014-1024, 1483-1491 | lambdas VM.cpp:308-338; protocol wtf/LazyUniqueRef.h (wtf/LazyRef.h has the same lines): `get` tests `lazyTag` (:71), `callFunc` does `ref.m_pointer \|= initializingTag` (:121), runs the lambda, `set` stores the raw pointer (:108-112), then `RELEASE_ASSERT(!(m_pointer & lazyTag))`, `RELEASE_ASSERT(!(m_pointer & initializingTag))` (:123-124) | none; `initializingTag` is only `ASSERT`ed in `get` (:70) | both run the lambda; the loser's `set` overwrites without deleting (leak; A keeps a `T&` to an orphan: split cache). If B's `\|= initializingTag` lands after A's `set`, A's second `RELEASE_ASSERT` fires, or the stored pointer has bit 1 set | C / M | EG for the four that AOT code reaches (has-own, split, searcher tables → better PT, see 10.1.4); LF otherwise |
| `m_megamorphicCache` in particular | VM.h:1018 | ensured only under `Options::useJIT()` (VM.cpp:457-460) and from jit/AssemblyHelpers.cpp:491, 618, 712 | — | with JIT off `megamorphicCache()` stays null, so `bumpEpoch` (VM.cpp:2479-2480, 2494-2495) and `age` (heap/Heap.cpp:2331) are skipped | — | nothing |
| `m_emptyPropertyNameEnumerator`, 15 × `m_promise…Executable` | VM.h:628-643 | null check runtime/VMInlines.h:139-150; slow paths VM.cpp:1985-2112: create, `setWithoutWriteBarrier` (no fence; `ASSERT(!m_x)`) | none | duplicates (not JS-visible by themselves; whether any code compares these executables by pointer was not searched); pointer before contents on ARM64 | P / M(arm64) | EG ("Eagerly initialize constant cells since the concurrent compiler can access them" already does it for the enumerator under JIT, VM.cpp:456-460) |
| `m_fastCanConstructBoundExecutable`, `m_slowCanConstructBoundExecutable` | VM.h:662-663 | `getBoundFunction` VM.cpp:1072-1093 | none | as above | P | EG (JIT build already calls it at VM.cpp:589) |
| `m_fastRemoteFunctionExecutable`, `m_slowRemoteFunctionExecutable` | VM.h:665-666 (`Weak<>`) | `getRemoteFunction` VM.cpp:1095-1117: `slot = Weak<NativeExecutable>(result)` | none | `Weak` assignment allocates and frees a `WeakImpl`: two writers free twice / reader reads a freed impl | M | LK (ShadowRealm only) |
| `numericStrings.m_doubleCache` | runtime/NumericStrings.h:178 | `if (!m_doubleCache) initializeDoubleCache()` (:75-76; NumberPrototype.cpp:505-506, 674) | none | second `unique_ptr` assignment frees the 4096-entry table the first thread is filling | M | EG or PT |
| `m_terminationDeadlines` | VM.h:1484 | VM.cpp:1320-1321 | API lock asserted | `RefPtr` swap | M | LK |
| `m_persistentBytecodePayloads`, `m_cachedBytecodeTwo/ThreeCharacterAtoms` | VM.h:1456, 1466-1467 | VM.cpp:1168-1173, 2558-2570 | none | `unique_ptr` double init | M | 10.8 |
| `m_exceptionFuzzBuffer`, `m_perBytecodeProfiler`, `m_typeProfiler(+Log)`, `m_controlFlowProfiler`, `m_samplingProfiler`, `m_fuzzerAgent`, `jitSizeStatistics`, `m_debugState` | VM.h:1004, 1471-1490 | options at construction, or `enable*Profiler` (VM.cpp:1694-1760: plain counters) | none | tooling | — | OFF or STW |
| dynamic `IsoSubspace`s (client heap) | heap/Heap.h:1350-1366 | `GCClient::Heap::nameSlow` heap/Heap.cpp:3665-3675: `ASSERT(!m_name); Locker locker { server().m_lock }; … m_name = WTF::move(space);` | **has a lock but does not re-check after taking it** | the second thread replaces the `unique_ptr` and destroys the `IsoSubspace` (with its allocator) from which the first is allocating. The server side (heap/Heap.cpp:3514-3522) has no lock of its own and relies on the client's | M | re-check under the lock, or one client heap per thread (heap report) |

#### 10.1.4 Content caches

| Cache | Declaration | Fill / read sites | Lock | Shape | A racing thread sees | Sev. | Rule |
|---|---|---|---|---|---|---|---|
| `numericStrings` small ints 0-1023 | VM.h:681; runtime/NumericStrings.h:60-65, 175 | `lookupSmallString` fills `value` lazily (:165-171); `addJSString(int)` NumberPrototype.cpp:482-490. **Compiled stubs read `jsString` directly**: pointers copied into the instance at aot/runtime/AOTInstance.cpp:270-271, loads at aot/stubs/AOTStubsStringConversions.cpp:96-98, AOTStubsPrimitiveFastPaths.cpp:109 | none | `String` slot + one `JSString*` word | two fillers of `value`: refcounted swap; `jsString` alone is a one-word publish | M | EG (1024 strings) or LF |
| `numericStrings` int cache (1024, direct mapped) | NumericStrings.h:50-58, 176 | `add(int)` :86-97, `addJSString(int)` NumberPrototype.cpp:491-500, `setJSString` (AOT hunk) NumericStrings.h:114-121 called from aot/runtime/AOTStringOperations.cpp:153, AOTStringBuilderStorageResults.cpp:73. Stub: compare `key`, then load `jsString` (AOTStubsStringConversions.cpp:88-90) | none | (b): `key`, `value`, `jsString` written in that order. In `addJSString` the stale `jsString` of the evicted number stays in place between `entry.key = i` (:493) and `entry.jsString = …` (:499) | the stub returns the text of **another number**; `value` is a refcounted swap | V + M | PT, or one-word entries (the `JSString` alone, verified by content) |
| `numericStrings` unsigned / double caches | NumericStrings.h:67-71, 177-178 | `add(unsigned)` :99-109, `add(double)` :73-84, `addJSString(double)` NumberPrototype.cpp:503-517; users runtime/Identifier.cpp:45-55 (`Identifier::from`), JSCJSValue.cpp:381-389, JSStringJoiner.h:207-215, JSONObject.cpp:246, StringConstructor.cpp:212-219. `add` returns `const String&` **into the table** | none | (b) + reference into the entry | the caller's reference is overwritten under it | M | PT |
| (GC) | heap/Heap.cpp:2336-2338 | "Compiled code reads these tables itself. They have a fixed size, so what they keep is bounded": **not cleared** when `runsCompiledImage()`; visited as roots VM.cpp:2294 | | | | | |
| `keyAtomStringCache` | VM.h:686; runtime/KeyAtomStringCache.h:35-51 (512 × `JSString*`) | `make` KeyAtomStringCacheInlines.h:37-60; users JSStringInlines.h:659-833, aot/builtins/AOTURLBuiltinOperations.cpp:283 | none | one word per slot, verified by content on hit (:51-53) | benign except unfenced publish; slots are unrooted and depend on `clear()` at *every* GC end (heap/Heap.cpp:2341) | P / M(arm64) | LF or PT |
| `lastCachedString` | VM.h:683 | read runtime/JSString.h:1256-1258, write JSString.cpp:460 | none | one word, verified | as above (strings have no identity) | P | PT |
| `lastAtomizedIdentifierStringImpl` / `…AtomStringImpl` | VM.h:684-685 (`Ref<>` × 2) | `JSString::toIdentifier` JSString.h:958-966: compare first, assign first, compute atom, assign second, then copy second | none | pair of refcounted swaps | (1) freed impl; (2) B matches the first word A just wrote and takes the *previous* atom: property access under the wrong key | M | PT |
| `stringSplitIndice` | VM.h:693 (`Vector<unsigned>`, reserved 256 at VM.cpp:343) | scratch of `String.prototype.split` runtime/StringPrototype.cpp:1085 and RegExp split RegExpPrototype.cpp:1068 | none | shared scratch buffer, (c) | realloc under the other thread; mixed contents | M | PT |
| `m_stringSplitCache` | VM.h:1022; runtime/StringSplitCache.h:45-89 | get/set StringSplitCacheInlines.h:34-90 (2-way; `RefPtr` subject, `RefPtr` separator, raw `JSCellButterfly*`); users StringPrototype.cpp:1078, 1135, 1196; RegExpPrototype.cpp:863, 941 | none (`AssertNoGC`) | (b) + refcounted swaps; butterfly unrooted, cleared every GC (Heap.cpp:2342-2343) | the pieces of another string (**V**), freed atoms | M | PT or OFF |
| `stringReplaceCache` | VM.h:694; runtime/StringReplaceCache.h:43-72 | `get` returns `Entry*` into the table (StringReplaceCacheInlines.h:36-57), used after return at StringPrototypeInlines.h:819 ff.; `set` :59-93 does `entry2 = { }` and replaces `Vector<int> m_lastMatch` | none | multi-word entry with an owned `Vector` | entry emptied / vector freed while read | M | PT or OFF |
| `immutableButterflyToStringCache` | VM.h:695 (`HashMap<JSCellButterfly*, JSString*>`) | runtime/JSArray.cpp:1182-1194; cleared Heap.cpp:2353 | none | (c) | rehash | M | PT or OFF |
| `m_hasOwnPropertyCache` | VM.h:1014; runtime/HasOwnPropertyCache.h:39-105 (2048 × {`RefPtr impl`, `StructureID`, `bool`}) | `get` :76-85; `tryAdd` HasOwnPropertyCacheInlines.h:35-65 (`…[index] = Entry { WTF::move(impl), id, result }`); user runtime/ObjectPrototype.cpp:124-138. AOT hunk: compiled structures are answered first by `AOT::PropertyWalk::hasOwnProperty` (:112-123) | none | (b) + refcounted swap | key of one fill with `result` of another (**V**); double deref | M | PT or OFF |
| `m_jsonCache` (`JSONCache`; replaces upstream's `JSONAtomStringCache`) | VM.h:1026; runtime/JSONCache.h:53-266 | atoms: `addAtomString` JSONCacheInlines.h:83-94 (`m_impl`, `m_length`, `m_buffer` in that order); transitions `addTransition` :298-320 (`from`, then `to`); names `NameTable::insert` JSONCache.cpp:55-68 (`from`, `to`, `offset`, …, then the text in a *separate* array); strings `StringTable::insert` JSONCacheInlines.h:432-439. Readers: LiteralParser.cpp:74, aot/runtime/AOTJSONScanner.h:329, AOTJSONParseOperations.cpp:89-185, AOTJSONKeyOrderOperations.cpp:200-211, AOTJSONValueParser.cpp:43 | none | (b), several words and two arrays per entry | `match` (JSONCacheInlines.h:377-385) accepts `from` + text and hands out `to` and `offset`: a torn entry gives a Structure and an offset that do not belong to the object being built (type confusion / out-of-bounds store in a consumer that trusts them); atom slot: refcounted swap | M | PT (it is sized for "consecutive objects usually have the same shape", JSONCache.h:188-190) |
| `JSONCache::m_isParsing` | JSONCache.h:110-128, 265 | `ParsingScope` saves and restores | none | save/restore flag | A's scope end clears it while B parses; then `reconcileTransitionsAtGCEnd` (:177-185) wipes tables B relies on | V/M | PT |
| `dateCache` | VM.h:1002; runtime/JSDateMath.h:85-225 | see 10.7.8 | only the process-wide host zone has one (`timeZoneCacheLock`, JSDateMath.cpp:501, 516) | several | | M | PT |
| BigInt modulo inverse: `m_cachedBigIntDivisor`, `m_nextCachedBigIntDivisor`, `m_bigIntCachedInverse`, `m_bigIntDivisorCount`, `m_bigIntFoldFactor` | VM.h:719-723 | runtime/JSBigInt.cpp:4847-4879: the divisor pointer is published (:4871) **before** `m_bigIntFoldFactor` (:4872) and the inverse (`cachedModMakeInverse` :4874 → `resize` :4342); reader `cachedMod` :4630, `RELEASE_ASSERT(vm.m_bigIntCachedInverse.size() == n + 1)` :4661 | none | 5 words + `Vector` | divisor matches, inverse is of the previous divisor: wrong remainder, the `RELEASE_ASSERT`, or a freed buffer | M | PT or OFF |
| `m_stringSearcherTables` | VM.h:1486; wtf/text/AdaptiveStringSearcher.h:97-111 (three `int` arrays) | filled per search, read in the Boyer-Moore loops (`shift = j - charOccurrence(…)`, `index += shift`, :401-420, 511-524). Users: StringPrototype.cpp:902, 1234, 2101; StringPrototypeInlines.h:398-732; aot/runtime/AOTStringOperations.cpp:135, AOTStringBuiltinOperations.cpp:56, AOTStringBuiltinReplaceOperations.cpp:81, 138, AOTStringBuiltinLiteralOperations.cpp:142 | none | shared scratch tables that must stay constant for one search | shifts of another pattern: missed matches (**V**); a negative shift is arithmetically possible, whether the subject access is bounds-checked was not read | V, M not excluded | PT |
| `m_syncResumeCallCache` (`MicrotaskCallCache`) | VM.h:1029; interpreter/MicrotaskCall.h:83-150 | 8 `MicrotaskCall` entries, round robin `m_nextEntryIndex` (:125-130); users aot/runtime/AOTJobs.cpp:304-368, AOTAsyncResumption.cpp:84, llint/LLIntSlowPaths.cpp:2309 | none | entry rebuilt in place | entry replaced while another thread is calling through it | M | PT (the job loop's own copy already lives on its stack: runtime/MicrotaskQueue.cpp, AOT hunk `Owner::JobLoop`) |
| `m_random`, `m_heapRandom`, `m_integrityRandom` | VM.h:473-475 | tools/Integrity.cpp:109-115; heap/MarkedBlockInlines.h:317; reseeded heap/Collector.cpp:504 | none | two 64-bit words (wtf/WeakRandom.h `advance`) | repeated numbers | V (not JS-visible) | PT |
| `machineCodeBytesPerBytecodeWordForBaselineJIT`, `m_startupJITDeferralScale`, `m_numberOfActiveJITPlans` | VM.h:682, 1540, 1558 | JIT | | | | — | |

#### 10.1.5 Tables

| Table | Declaration | Mutation | Lock | Sev. | Rule |
|---|---|---|---|---|---|
| `m_atomStringTable` | VM.h:675 | 10.5 | none | M | LK/LF, the largest single item |
| `m_symbolRegistry`, `m_privateSymbolRegistry` | VM.h:676-677 | 10.5 | none | M + I | LK |
| `symbolImplToSymbolMap`, `atomStringToJSStringMap`, `wasmGCStructureMap` | VM.h:767-771 | 10.4, 10.5 | none | M + I | LK |
| `m_executablesOfNamelessHostFunctions` | VM.h:667-668 ("getHostFunction() without the JIT") | `getHostFunction` VM.cpp:1062-1068: `find`, `create()`, `set(key, Weak<>)`. "Promise.prototype.finally asks at every call" (VM.cpp:1059) | none | M (c) | LK, or EG (bounded: "as many as there are functions in C++") |
| `propertyRuns` (`PropertyRunTable` + `PropertyAdditionCache`) | VM.h:521; runtime/PropertyRun.h:59-144 (`HashSet`, `HashMap`, `HashSet<Entry>`, `FastBitVector`) | `intern` from aot/runtime/AOTObjectLiteralOperations.cpp:24-33 (also fills `instance->runtime.literals.propertyRuns[number]`, shape (a)); `additionCache().add` runtime/Structure.cpp:1655, 1744; reads Structure.cpp:830-838, 1468, 1514, 1589 | none | M (c) | LK (writes are rare after warm-up), readers need it too |
| `m_mayHaveUncacheableTransitions` | VM.h:524 | set once: aot/runtime/AOTVMState.cpp:54, Structure.cpp:793 | monotone bool | — | |
| `m_impurePropertyWatchpointSets` | VM.h:1470 | VM.cpp:1680-1692 | none | M (c) | LK (embedder) |
| `sourceProviderCacheMap`, `m_codeCache`, `m_builtinExecutables`, `m_compactVariableMap`, `m_functionHasExecutedCache` | VM.h:822, 1455, 1469, 1012, 1479 | 10.8 | none | M | 10.8 |
| `m_loopHintExecutionCounts` | VM.h:1552-1553 | VM.cpp:2227-2254 | **`m_loopHintExecutionCountLock`**; the counter it hands out is then bumped unlocked | — | |
| `m_microtaskQueues` (intrusive list), `m_defaultMicrotaskQueue` | VM.h:1447, 1555 | 10.7.11 | none | M | PT |
| `m_debuggers` | VM.h:1566 | VM.cpp:2403-2411 | none | M | STW |
| `deferredWorkTimer` | VM.h:670 | 10.7.12 | partial | M | LK |
| `m_intlCache` | VM.h:1468 | 10.7.9 | none | M | PT |
| `m_aotState` | VM.h:516; aot/runtime/AOTVMState.h:36-83 | other reports. It already has locks for `instances` (`instancesLock`), `loopPollPages`, `innerCellsWithoutMark`; `regExpMatchOutput` is one shared buffer (:75) | | | |

#### 10.1.6 Two per-VM "mode" scopes that change what *other* threads may do

* **`allowLazyMaterializationOfImmutablePropertiesCount`** (VM.h:465; scope VM.h:1613-1641: "While one of these is alive, properties can be put directly on an object whose Structure says
  hasImmutableProperties()"). It is tested on every direct put/delete that meets an immutable property: `if (structure->isImmutableProperty(uid) && !vm.allow…Count)` runtime/JSObjectInlines.h:386, 574;
  JSObject.cpp:2190, 2226; aot/builtins/AOTBuiltinTableRealm.cpp:874. The scope is opened by every lazy materialisation in this section (static tables, function `name`/`length`/`prototype`, error `stack`, arguments `callee`).
  With a shared counter, while thread A reifies anything, thread B's write to a **locked built-in** goes through. The AOT compiler's proofs rest on those objects being immutable (`lockObject`, aot/runtime/AOTRealmStartup.cpp:140-144),
  so this is type confusion in compiled code, not just a wrong value: **M**. The `++`/`--` are also plain. Rule: PT (better: pass the permission as an argument).
* **`m_deletePropertyMode`** (VM.h:1453, 781-803). `IgnoreConfigurable` is set around one `deleteProperty` in aot/object/AOTObjectConversion.cpp:79-82 and runtime/ProgramExecutable.cpp:205. During that window a `delete` by thread B
  removes non-configurable properties (**V**, and **M** where compiled code assumes the property stays). The restore writes back the *saved* value: two overlapping scopes can leave the mode on for good. Rule: PT.

#### 10.1.7 Caches are emptied in the collection epilogue

`Heap::sweepAndClearCachesInEpilogue` (heap/Heap.cpp:2318-2354) calls `pruneUnusedSourceProviderCaches`, `HasOwnPropertyCache::clear`, `jsonCache().clearStrings()/clearJSStrings()`, `numericStrings.clearOnGarbageCollection()` (not in a compiled image),
`stringReplaceCache.clear()`, `keyAtomStringCache.clear()`, `StringSplitCache::clear`, `immutableButterflyToStringCache.clear()`; `VM::reconcileWeakReferencesAtGCEnd` (VM.cpp:2263-2267) and every `WeakGCHashTable` do the same kind of work.
Each `clear()` derefs strings and overwrites entries in place. Several caches hold **unrooted** cell pointers and are only correct because they are wiped before the mutator runs again.
With per-thread copies the epilogue has to reach all copies; whether all JS threads are stopped at this point is for the heap report.
`executeEntryScopeServicesOnEntry` (VM.cpp:2127-2164) is a second wipe point: *any* thread entering the VM after a time zone or language change resets `dateCache`/`intlCache` and walks `heap.dateInstanceSpace` (VM.cpp:2134-2147).

### 10.2 `LazyProperty<>` and `LazyClassStructure`: the exact protocol

One word, `uintptr_t m_pointer` (runtime/LazyProperty.h:117), `lazyTag = 1`, `initializingTag = 2` (:114-115).

1. `initLater(lambda)` stores `lazyTag | &theFunc`, where `theFunc` is a `static constexpr` function pointer variable (runtime/LazyPropertyInlines.h:52-53).
2. `get(owner)` → `getInitializedOnMainThread` (LazyProperty.h:93-101): `if (m_pointer & lazyTag)` load the function through `m_pointer & ~3` and call it; else return the word as a pointer.
3. `callFunc` (LazyPropertyInlines.h:97-108):
   ```cpp
   if (initializer.property.m_pointer & initializingTag)
       return nullptr;                                   // "gracefully supports recursive calls"
   DeferTerminationForAWhile deferTerminationForAWhile { initializer.vm };
   initializer.property.m_pointer |= initializingTag;
   callStatelessLambda<void, Func>(initializer);         // must call init.set(value)
   RELEASE_ASSERT(!(initializer.property.m_pointer & lazyTag));
   RELEASE_ASSERT(!(initializer.property.m_pointer & initializingTag));
   return bit_cast<ElementType*>(initializer.property.m_pointer);
   ```
4. `set` → `setMayBeNull`: `m_pointer = value; RELEASE_ASSERT(!(m_pointer & lazyTag)); vm.writeBarrier(owner, value)` (:57-62). **No fence** before the store.
5. `getConcurrently()` returns null while `lazyTag` is set (LazyProperty.h:83-89). The collector skips a lazy word (`visit`, LazyPropertyInlines.h:73-77).

What a second JS thread can see:

| Interleaving | Result | Sev. |
|---|---|---|
| B calls `get` while A is between step 3's `\|=` and `set` | B returns **`nullptr`** (the recursion answer). Almost no caller checks: `linkTimeConstant` only `ASSERT(result)` (runtime/JSGlobalObjectInlines.h:698-703); `LazyClassStructure::prototype` does `get(global)->storedPrototypeObject()` (runtime/LazyClassStructureInlines.h:49-53) | C (null deref), or a null Structure handed to an allocation |
| B reads `lazyTag` set and the tag test passes before A's `\|=` | both run the initialiser: two prototypes / constructors / functions; the later `set` wins while the earlier object may already have escaped (e.g. put on the global object by `reifyStaticProperty`, runtime/Lookup.h:533-548) | I |
| B's `\|= initializingTag` (a load and a store) straddles A's `set` | the word becomes `function-address\|3` again after A published, or `pointer\|2`; A's `RELEASE_ASSERT`s fire, or readers use a pointer with bit 1 set | C / M |
| B reads the published word on ARM64 | cell contents not yet visible (no `storeStoreFence`) | M |

**`LazyClassStructure`** = `LazyProperty<JSGlobalObject, Structure> m_structure` + `WriteBarrier<JSObject> m_constructor` (runtime/LazyClassStructure.h:120-121). The initialiser calls, in order,
`setPrototype` (only records), `setStructure` — which **publishes `m_structure` at once** via `structureInit.set(structure)` (runtime/LazyClassStructure.cpp:55-70) — and then `setConstructor`, which does
`prototype->putDirectWithoutTransition(vm, constructor…)` and `m_constructor.set` (:72-87). Between the two, the class is "initialised" for every reader that tests the structure word:
`constructor(global)` is `m_structure.get(global); return m_constructor.get();` (LazyClassStructure.h:84-89) → **null constructor**, which `reifyStaticProperty` would then `putDirect` on the global object (Lookup.h:542-548); and instances can be made whose prototype has no `constructor` yet, while
`putDirectWithoutTransition` mutates the prototype's property table in place under them (**M**, objects report).
**AOT hunk**: both setters call `AOT::RuntimeHooks::didCreateClass` when `vm.hasCompiledCode()` (LazyClassStructure.cpp:66-69, 83-86). `didCreateClass` (aot/runtime/AOTRealmStartup.cpp:480-512) records the instance Structure
(`instance->didCreateInstanceStructure`), returns early on the first call ("The realm completes the prototype while it creates the constructor", :501-503) and on the second **locks** prototype and constructor (`BuiltinLocker`) and `markPristine`s the structure. So the lazy creation also
writes AOT instance tables (`builtinCells`), and a class is usable before it is locked.
`didCreateStaticProperty` (:514-536) does the same for each lazily reified static member.

**Rule.** The initialisers allocate, may run JS ("A PropertyCallback builder can enter JS", runtime/Lookup.cpp:69-70) and re-enter other lazies, so a spin-wait on `initializingTag` can deadlock between threads (A inits X needs Y, B inits Y needs X) and must still answer null for *same-thread* recursion.
Options: one recursive per-realm "lazy initialisation" lock taken only on the slow path (`lazyTag` seen), with release-store in `set` and acquire/dependent load in `get`, and `m_constructor`/prototype completed *before* the structure word is published; or EG: force all lazies when the first thread is spawned
(cost: 83 + 57 + link-time constants, counted in 10.3, plus what their initialisers allocate).

### 10.3 `JSGlobalObject` member by member (runtime/JSGlobalObject.h:232-770)

**Count of lazy members.** `LazyProperty<JSGlobalObject, …>`: 70 named declarations + 13 `m_resizableOrGrowableSharedTypedArray##name##Structure` (`FOR_EACH_TYPED_ARRAY_TYPE`, runtime/TypedArrayType.h:38-54) = **83** (4 are WebAssembly, 2 ObjC and 2 GLib are compiled out for Bun).
`LazyClassStructure`: 21 named + 11 (`FOR_EACH_LAZY_BUILTIN_TYPE`, JSGlobalObject.h:180-194) + 13 typed arrays + 12 WebAssembly (:197-209) = **57**.
`m_linkTimeConstants`: `FixedVector<LazyProperty<JSGlobalObject, JSCell>>` of `numberOfLinkTimeConstants` (:516; bytecode/LinkTimeConstant.h:171: 126 listed + the generated `JSC_FOREACH_BUILTIN_LINK_TIME_CONSTANT`), some `set` eagerly in `init` (JSGlobalObject.cpp:1494-1631, 1924-1927), the rest `initLater` (e.g. :1226, 1286, 1461-1464). JSGlobalObject.cpp has 185 lines containing `initLater`.

*LazyProperty names:* m_defaultCollator, m_defaultDateTimeFormat, m_defaultDateFormat, m_defaultTimeFormat, m_defaultNumberFormat, m_collatorStructure, m_displayNamesStructure, m_durationFormatStructure, m_listFormatStructure, m_localeStructure, m_pluralRulesStructure,
m_relativeTimeFormatStructure, m_segmentIteratorStructure, m_segmenterStructure, m_segmentsStructure, m_segmentDataObjectStructure, m_segmentDataObjectWithIsWordLikeStructure, m_intlPartObjectStructure, m_intlPartObjectWithSourceStructure, m_intlPartObjectWithUnitStructure,
m_intlPartObjectWithUnitAndSourceStructure (287-307); m_parseIntFunction, m_parseFloatFunction, m_objectProtoToStringFunction, m_arrayProtoToStringFunction, m_arrayProtoValuesFunction, m_asyncFromSyncIteratorProtoNextFunction, m_mapProtoEntriesFunction, m_setProtoValuesFunction,
m_stringProtoSymbolIteratorFunction, m_numberProtoToStringFunction, m_iteratorProtoSymbolIteratorFunction, m_throwTypeErrorArgumentsCalleeGetterSetter, m_moduleLoader (333-358); m_debuggerScopeStructure, m_withScopeStructure, m_strictEvalActivationStructure, m_moduleEnvironmentStructure,
m_callbackConstructorStructure, m_callbackFunctionStructure, m_callbackObjectStructure, m_rawJSONObjectStructure, (m_objc*, m_glib* ×4), m_ffiFunctionStructure, m_ffiCallbackStructure (375-402); m_customGetterFunctionStructure, m_customSetterFunctionStructure, m_nativeStdFunctionStructure,
m_remoteFunctionStructure, m_iteratorResultObjectStructure, m_promiseAllSettledFulfilledResultStructure, m_promiseAllSettledRejectedResultStructure, m_dataPropertyDescriptorObjectStructure, m_accessorPropertyDescriptorObjectStructure, m_promiseCapabilityObjectStructure, m_moduleRecordStructure,
m_syntheticModuleRecordStructure, m_moduleNamespaceObjectStructure, m_proxyObjectStructure, m_callableProxyObjectStructure, m_proxyRevokeStructure (450-467); m_webAssemblyModuleRecordStructure, m_webAssemblyFunctionStructure, m_webAssemblyWrapperFunctionStructure, m_webAssemblyJSTag (487-490); m_typedArrayProto, m_typedArraySuperConstructor (507-508).
*LazyClassStructure names:* m_evalError…, m_rangeError…, m_referenceError…, m_syntaxError…, m_typeError…, m_URIError…, m_aggregateError…, m_suppressedErrorStructure (269-276); m_dateTimeFormatStructure, m_numberFormatStructure (308-309); m_duration…, m_instant…, m_plainDate…, m_plainDateTime…, m_plainMonthDay…, m_plainTime…, m_plainYearMonth…, m_zonedDateTimeStructure (320-327);
m_sharedArrayBufferStructure, m_disposableStackStructure, m_asyncDisposableStackStructure (468-470); by macro: booleanObject, date, error, map, numberObject, set, weakMap, weakSet, weakObjectRef, finalizationRegistry, arrayBuffer; m_typedArray{Int8…BigUint64, DataView}; the 12 WebAssembly classes.

| Member (JSGlobalObject.h) | Run-time writers | Lock | Shape / what a racing thread sees | Sev. | Rule |
|---|---|---|---|---|---|
| all lazies above | 10.2 | none | 10.2 | C/I/M | 10.2 |
| AOT mirror `instance->realm.linkTimeConstants[]` (aot/runtime/AOTInstance.h:209) | compiled code loads the word, on zero calls `operationAOTLinkTimeConstant`, which stores it (aot/builtins/AOTRuntimeIntrinsicEmitter.cpp:46-51; AOTRuntimeIntrinsicOperations.cpp:48-56; also AOTAsyncOperations.cpp:47) | none | one word, idempotent **if** the underlying `LazyProperty` is single-valued | P | follows 10.2 |
| `m_aotInstance`, `m_debugger`, `m_microtaskRunnability`, `m_associatedContextIsFullyActive`, `m_canFastQueueMicrotask`, `m_microtaskQueue` (238-245) | `setDebugger`, `updateCanFastQueueMicrotask` (247-254), `setMicrotaskQueue` JSGlobalObject.cpp:4076-4083 (`Ref` move-assign) + AOT hook `didChangeMicrotaskQueueing` | none | refcounted swap of a non-thread-safe `RefCounted` queue | M | set before threads; queue per thread (10.7.11) |
| `WriteBarrier<>`/`WriteBarrierStructureID` members 261-285, 329-373, 392-448, 454-455, 500-504 | `init()` only, **except** the array structures below | — | | — | |
| `m_arrayStructureForIndexingShapeDuringAllocation[]`, `m_regExpMatchesArray*Structure`, `m_clonedArgumentsStructure` (409, 444-446, 395) | `fireWatchpointAndMakeAllArrayStructuresSlowPut` JSGlobalObject.cpp:2972-3013 | none | see "having a bad time" below | V/M | STW |
| `m_globalScopeExtension` (264), `m_unhandledRejectionCallback` (505), `m_name` (529), `m_consoleClient`, `m_evalEnabled` + `m_evalDisabledErrorMessage`, `m_webAssemblyEnabled`…, `m_runtimeFlags`, `m_trustedTypesEnforcement` (701-730) | embedder/debugger setters: JSGlobalObject.cpp:2615-2623, JSGlobalObjectInlines.h:118, JSGlobalObject.h:1324-1326; debugger/DebuggerEvalEnabler.h:52-66 toggles `evalEnabled` around an evaluation | none | `String` members: refcounted swap. `DebuggerEvalEnabler` is a save/restore scope on shared state | M (strings) / V | set before threads; debugger under STW |
| `m_cachedLocaleCompareCollator` + `…Locale` + `…LanguagesEpoch` (312-314); `m_cachedToLocaleStringNumberFormat` + 2 (316-318) | `cachedLocaleCompareCollator` JSGlobalObject.cpp:3464-3484 writes locale (:3480), epoch, then the cell (:3482); number format :3486-3506 | none | 3 words, key before value: B matches the new locale and gets the **old** collator (**V**); `String` swap (**M**); the cached ICU object is then used by both threads (10.7.9) | M | PT |
| `m_structureCache` (518) | runtime/StructureCache.cpp:40-82 | `m_lock` for `set`/`clear`/`emptyObjectStructureConcurrently`; **the mutator's `m_structures.get(key)` at :56 is unlocked** (comment :45) | (c) against another mutator's locked `set`; check-then-create gives two Structures for one (prototype, capacity, class) — legal but splits shapes | M | take the existing lock on reads |
| `m_symbolTableCache`, `m_resumableCodeSymbolTableClones`, `m_moduleProgramExecutables` (519-527) | bytecode/CodeBlock.cpp:1343-1363; runtime/JSModuleRecord.cpp:571 | none | `WeakGCMap`, 10.4 | M | 10.8 |
| `m_asyncContextData` (532) | field 0 of one `InternalFieldTuple` **is** "the current async context": `AsyncContextSwapScope` reads it, overwrites it and restores it (runtime/AsyncContextSwapScope.h:90-92, 117-123, 149-152); captured in `VM::throwException` (VM.cpp:1410-1411) | none | thread state kept in a realm cell: B's `AsyncLocalStorage.getStore()` returns A's store; restore order interleaves | V (security-relevant for servers) | PT |
| `m_ffiContext` (533) | `ffiContext()` JSGlobalObject.cpp:4064-4073 (fence, then `unique_ptr` assign) | none | double init frees the first | M | LK/EG |
| `m_masqueradesAsUndefined…`, `m_havingABadTime…`, `m_varInjection…`, `m_varReadOnly…`, `m_regExpRecompiled…`, `m_regExpLastIndexWritable…`, `m_arrayBufferDetachWatchpointSet` (542-548, `const Ref<WatchpointSet>`); 40 `InlineWatchpointSet` (558-597) + 2 × 13 per typed array (598-601) | 10.6 | none | 10.6 | V/M | 10.6 |
| the `std::unique_ptr<…Watchpoint>` members (604-619) | eager for Array/Promise/RegExp (JSGlobalObject.cpp:2432-2449); **lazy** for ArrayBuffer and typed arrays: `tryInstallArrayBufferSpeciesWatchpoint` (runtime/JSArrayBufferPrototypeInlines.h:46), `tryInstallTypedArraySpeciesWatchpoint` (JSGenericTypedArrayViewPrototypeFunctions.h:86) → `tryInstallSpeciesWatchpoint` JSGlobalObject.cpp:3553-3647, which starts with `RELEASE_ASSERT(!constructorWatchpoint); RELEASE_ASSERT(!speciesWatchpoint);` and may `flattenDictionaryStructure` the prototype (:3567-3568, 3586-3587) | none | callers test `state() == ClearWatchpoint` first (JSArrayBufferPrototypeInlines.h:43-47): two threads both pass → second hits the `RELEASE_ASSERT`, or `RELEASE_ASSERT(!speciesWatchpointSet.isBeingWatched())` (:3639) | C (+M: flatten) | EG or LK |
| `m_installedObjectAdaptiveStructureWatchpoints`, `…PropertyChange…`, `m_installedChainedWatchpoints` (620-622) | `install*` JSGlobalObject.cpp:3845-3864 (`Vector::append`); lazily via `tryInstallPropertyDescriptorFastPathWatchpoint` :3866-3945, called from runtime/ObjectConstructor.cpp:762, **aot/runtime/AOTDefinitionOperations.cpp:265, aot/object/AOTObjectExtraProperties.cpp:373**; also when lazy classes are created (`installMapPrototypeWatchpoint` … :3817-3843) | none | (c); `RELEASE_ASSERT(!…isBeingWatched())` :3941 | M/C | EG or LK |
| `m_weakTickets` (634) | `addWeakTicket` JSGlobalObject.cpp:4216-4226 | **`cellLock()`** + fence, `ThreadSafeWeakHashSet` | `clearWeakTickets` tests the pointer unlocked (:4229) — benign | — | |
| `m_rareData` (551; struct JSGlobalObjectInlines.h:53-58: `profileGroup`, `opaqueJSClassData` map) | `createRareDataIfNeeded` :481-486; `contextData` :507-511 returns a reference into the `HashMap` | none | double init; (c) | M | LK (C API only) |
| `m_weakRandom` (553) — **`Math.random()`** | `weakRandomNumber()` (:1371; runtime/MathObject.cpp:338); **inlined in compiled code**: load low, load high, store low, store high (aot/builtins/AOTMathBuiltinEmitter.cpp:234-249) | none | two 64-bit words, xorshift128+ (wtf/WeakRandom.h `advance`) | two threads return the same number or mix states; no unsafe value can arise (the result is masked to 53 bits) | V | PT (seed each thread) |
| `m_regExpGlobalData` (554) | RegExp report | | | | |
| `m_referencedGlobalPropertyWatchpointSets` (697-698) | JSGlobalObject.cpp:4133-4145, `#if ENABLE(DFG_JIT)` | **lock** | | — | |
| `m_globalLexicalBindingEpoch` (705) | `bumpGlobalLexicalBindingEpoch` JSGlobalObject.cpp:3989-4004: AOT hook `didChangeGlobalLexicalBindings`, plain `++`, on wrap iterates all CodeBlocks; from runtime/ProgramExecutable.cpp:307 | none | read by LLInt `resolve_scope` (llint/LowLevelInterpreter64.asm:2896-2897) | V | 10.8 / STW |
| `m_executableForCachedFunctionExecutableForFunctionConstructor` (710, `Weak<>`) | JSGlobalObject.cpp:4281-4291; read :4242 | none | `Weak` set | M | 10.8 |
| `m_customGetterFunctionSet`, `m_customSetterFunctionSet` (751-752, `WeakGCSet`; upstream's "…Map") | `ensureValue` runtime/PropertyDescriptor.cpp:65, 80 (`Object.getOwnPropertyDescriptor` on a custom accessor) | none | (c); two function objects for one accessor | M + I | LK |
| `m_signposts` (761) | JSGlobalObject.cpp:665-677 | none | (c) | M | LK (console API) |
| `m_importMap`, `m_intlLegacyConstructedSymbol`, `m_globalObjectMethodTable`, `m_vm` | construction | | | — | |

**Global variables.** `JSGlobalObject` is a `JSSegmentedVariableObject`: `SegmentedVector<WriteBarrier<Unknown>, 16> m_variables` (runtime/JSSegmentedVariableObject.h:99). `addVariables` grows it under `cellLock()` (JSSegmentedVariableObject.cpp:53-61), but
`variableAt`/`isValidScopeOffset` read it unlocked: "This is not thread-safe, since m_variables is a segmented vector, and its spine can resize with malloc/free if new variables - unrelated to the one you are accessing - are added" (.h:60-70). **M** for any reader that goes through the spine (`symbolTableGet`, runtime/JSSymbolTableObject.h:70-88, holds the *SymbolTable's* lock, not the cell lock).
`SymbolTable` has `mutable ConcurrentJSLock m_lock` (runtime/SymbolTable.h:767) and every map access takes a locker. `addSymbolTableEntry` (JSGlobalObject.cpp:2598-2613) publishes the entry under the symbol table lock and only *then* calls `addVariables` (:2608): in between, a reader finds the name with an offset that is not valid yet (`isValidScopeOffset` false → "no such property", **V**).
`addStaticGlobals` does it the other way round (variables first, :3513) and is safe in that respect. Both end with the AOT hook `invalidateStructureChainIntegrity(Add, this, uid)` (:2611-2612, 3535-3536). There is no `m_lastStaticGlobalOffset` in this tree.
The global lexical environment (`m_globalLexicalEnvironment`, :263) is the same class family. `SymbolTable` also has lazies of its own: `m_rareData` (`ensureRareData` :749-757), `m_localToEntry` (:784), `m_singleton` (`InferredValue`, :782), `m_arguments` (:780).

**Having a bad time** (`JSGlobalObject::haveABadTime`, JSGlobalObject.cpp:3023-3137) is **not** a stop-the-world operation. Order: `isHavingABadTime()` test; `DeferGC`; step 1 `fireWatchpointAndMakeAllArrayStructuresSlowPut`: clear the structure cache (+ fire `m_structureCacheClearedWatchpointSet`), overwrite the allocation structures one by one, and fire
`m_havingABadTimeWatchpointSet` **last** ("This ensures that the concurrent JIT threads will always get the SlowPut versions of the structures if isHavingABadTime() returns true", :3007-3011); step 2 `HeapIterationScope` + `objectSpace().forEachLiveCell(finder)` collects `Vector<JSObject*> foundObjects`; finally `switchToSlowPutArrayStorage` on each (:3131-3136).
It assumes nobody allocates or reshapes between the steps. A second thread that loaded an array Structure before :2991 can publish a Contiguous array after the heap walk (missed: indexed setters on the prototype are bypassed, **V**); the conversion swaps butterflies of objects other threads are indexing (**M**, objects report).
AOT hunk `haveABadTime(VM&, JSObject* interceptor)` (:3139-3174): if `Array.prototype`/`Object.prototype` are immutable and are not the interceptor, only objects with the interceptor in their chain are converted, the realm stays "good". Same walk, same assumption. Rule: STW.

### 10.4 `WeakGCMap` / `WeakGCSet` / `WeakGCHashTable`

In this tree the map is a plain `UncheckedKeyHashMap<Key, Value*>` of **raw cell pointers** (runtime/WeakGCMap.h:42-43, 106), not of `Weak<>`. `get` = `m_map.get` (:54-57); `set` = `m_map.set` + `markDirty` (:59-64); `ensureValue` = `m_map.ensure(key, functor)` under `AssertNoGC`, refilling a zombie null (:66-82).
`markDirty` links the table into a heap list if not yet on it (runtime/WeakGCHashTable.h:45-49 → `Heap::addDirtyWeakGCHashTable`, heap/Heap.cpp:3087-3090): an unlocked intrusive-list insert. At GC end `reconcileWeakReferencesAtGCEnd`: full collection `removeIf(!isMarked)`, eden collection nulls dead values in place
(runtime/WeakGCMapInlines.h:70-86). No lock anywhere; `StructureCache` wraps one in its own. Hazards: (c) on any concurrent `set`/`ensure`; `get`-then-`set` users create duplicates; a value read by B just before the reconcile pass is not kept alive by the read (no read barrier) — fine today because the mutator is the one running the epilogue or is stopped.
Instances: `VM::symbolImplToSymbolMap`, `atomStringToJSStringMap`, `wasmGCStructureMap`; `JSGlobalObject::m_symbolTableCache`, `m_resumableCodeSymbolTableClones`, `m_moduleProgramExecutables`, `m_structureCache.m_structures`, the two custom accessor sets; `PropertyAdditionCache`. Rule: one lock per table (readers too) or a concurrent table; the dirty list needs its own lock.

### 10.5 Atom strings, identifiers, symbols

* **Which table.** Every `AtomStringImpl::add/lookUp/remove` uses `Thread::currentSingleton().atomStringTable()->table()` (wtf/text/AtomStringImpl.cpp:76-79). `JSLock::didAcquireLock` points the thread at the VM's table (runtime/JSLock.cpp:126), the release path restores the thread's own (:328-331). For `VMType::Default` the VM's table *is* the creating thread's default table (VM.cpp:279).
  N JS threads in one VM must all point at one table, because identifiers are compared by pointer.
* **No lock.** `AtomStringTableLocker` is an empty class unless `USE(WEB_THREAD)` (AtomStringImpl.cpp:49-72); the call sites are already bracketed with it (:131, 402, 437, 462, 494, 520-531, 550, 564, 578, 602, 618, 635), which is a ready-made seam.
* **Add** (:109-126): AOT/Bun hunk first asks `readOnlyAtomLookup()` (atomic function pointer, wtf/text/AtomStringTable.h:52-58) via `findAtomBeforeAdding` (table `find`, then the image's read-only atoms), then `HashSet::add`. `addSlowCase` (:447-476) inserts an *existing* `StringImpl` and then `setIsAtom(true)` (non-atomic flag, fact 2).
* **Remove** runs in the destructor of the last reference, on **whatever thread drops it** (:561-569): `RELEASE_ASSERT(wasRemoved, "The string being removed is an atom in the string table of an other thread!")`. There is also the classic resurrection race: A's `deref` reaches zero and is about to remove; B's `add` finds the entry in the table and refs it. A lock alone does not fix that; the lookup must fail for a zero count (or removal must re-check under the lock).
* Races: (c) on the `HashSet` (**M**); two atoms for the same characters (**V→M**: property lookups by pointer miss; AOT tables keyed by uid disagree).
* **What helps**: atoms of the program's own identifiers are static strings in read-only memory, "in no table, and every thread has the same ones" (AtomStringTable.h:52-54); `ref`/`deref` skip static strings (StringImpl.h:1207-1209, 1218-1220). Only atoms made at run time (computed keys, `JSON.parse` keys, `split` pieces) touch the table.
* `CommonIdentifiers`/`vm.propertyNames`: built once (VM.cpp:379). `Identifier::from(int/unsigned/double)` goes through `numericStrings` (runtime/Identifier.cpp:45-55, 10.1.4) and then the atom table.
* **`SymbolImpl::nextHashForSymbol`**: `static unsigned s_nextHashForSymbol` with `+=` and `|=` (wtf/text/SymbolImpl.cpp:96-102), process-global, already racy between Workers. A lost update gives two symbols the same hash: only collisions (**P**).
* **`Symbol()`**: `Symbol::finishCreation` does `vm.symbolImplToSymbolMap.set(&uid, this)` for *every* symbol (runtime/Symbol.cpp:63-69) → (c) **M** on a hot path. `Symbol::create(vm, uid)` is `get` then create (:166-174): two `Symbol` cells for one `SymbolImpl`. `===` on two cells that are neither strings nor BigInts is `v1 == v2` (runtime/JSCJSValueInlines.h:431-438), so **I**: `Symbol.for('x') !== Symbol.for('x')`, while both name the same property.
* **`Symbol.for`**: runtime/SymbolConstructor.cpp:112 → `SymbolRegistry::symbolForKey` (wtf/text/SymbolRegistry.cpp:45-59): `m_table.add(rep.impl())`, and on a new entry creates the symbol and **overwrites the bucket through the saved iterator** (`*addResult.iterator = symbol`, :57). No lock (`SymbolRegistry.h:37-51`; "CanMakeThreadSafeCheckedPtr" is about the pointer, not the table).
  Between `add` and the overwrite the bucket holds the *key string*, which a second `symbolForKey` would `downcast<SymbolImpl>` (:49); `downcast` is checked (`RELEASE_ASSERT`, wtf/TypeCasts.h:109-122), so **C**; the `HashSet` itself is shape (c) **M**. `remove` (:67-73) runs from the `RegisteredSymbolImpl` destructor on any thread. AOT uses the registry at start-up (aot/runtime/AOTProgramIdentifiers.cpp:148, aot/builtins/AOTBuiltinTable.cpp:824). Rule: LK.
* `Symbol::m_description` lazily caches a `JSString` (Symbol.cpp:131-143): shape (a), **P**.
* `JSString`/`JSRopeString` resolution and `swapToAtomString` (JSString.h:940-947: fence, swap, fence, `markAsAtom`, and the old `String` parked in a heap list) are covered elsewhere; note only that the protocol was designed for concurrent *readers*, with one writer.

### 10.6 Watchpoint sets with the JIT off

`WatchpointSet`: `int8_t m_state`, `int8_t m_setIsNotEmpty`, an intrusive `SentinelLinkedList` (bytecode/Watchpoint.h:165-280). `InlineWatchpointSet`: one word, thin = encoded state, fat = pointer to a `WatchpointSet` (:304-506).
*Fire*: thin: `m_data = encodeState(IsInvalidated); storeStoreFence()` (:356-366). Fat: `fireAllSlow` — fence, `m_state = IsInvalidated` ("Do this first. Needed for adaptive watchpoints."), then `fireAllWatchpoints`: `DeferGCForAWhile`, `while (!m_set.isEmpty()) { w = *m_set.begin(); w.remove(); w.fire(vm, detail); }` (bytecode/Watchpoint.cpp:129-188).
*Add*: `m_set.push(watchpoint); m_setIsNotEmpty = true; m_state = IsWatched` (`ASSERT(!isCompilationThread())`, :118-127). *Inflate*: allocate, fence, store pointer (:209-215).
State reads are documented as safe from other threads (`isStillValid`, Watchpoint.h:199-204, 332-341); **list operations and inflation are mutator-only and unlocked**.

Who still uses them with the JIT off:
* *Readers* (C++ fast paths that test `isStillValid()`/`state()`): by number of tests — runtime/RegExpObjectInlines.h (14), JSGlobalObjectInlines.h (12), IteratorOperations.cpp (7), JSGlobalObject.cpp (6), JSArray.cpp (5), ArrayPrototypeInlines.h (5), JSPromiseConstructor.cpp (3), JSPromise.cpp, JSPromisePrototype.cpp, JSMicrotask.cpp, SetPrototypeInlines.h, RegExpPrototypeInlines.h, ObjectConstructor.cpp, CommonSlowPaths*.{cpp,h}, JSGenericTypedArrayView*.h, bytecode/ObjectAllocationProfileInlines.h (2 each), Structure.h/StructureInlines.h, FunctionRareData.h, **aot/runtime/AOTDefinitionOperations.cpp (2), aot/object/AOTObjectExtraProperties.cpp (2), aot/runtime/AOTJobs.cpp, AOTRuntimeValidation.cpp**.
  All are *check-then-act*: "the iterator protocol is untouched, so copy the butterfly". Single-threaded nothing can fire between the check and the end of the native function unless the function itself calls JS. With threads the fire can land anywhere in the act. Mostly **V** (an observable hook is skipped once); **M** where the act relies on a *shape* (bad time, `arrayBufferDetach`, sane chain → holes read as undefined).
* *Writers of lists*: adaptive watchpoints installed by the realm (10.3), `StructureRareData` special-property and enumerator caches (10.7.6), `FunctionRareData::createAllocationProfileClearingWatchpoint`, `ObjectAllocationProfile`, LLInt inline caches (10.8). They push onto `Structure::m_transitionWatchpointSet` and replacement sets of **shared** Structures.
  Two pushes, or a push against a fire, corrupt the list: **M**. Two inflations: the loser's fat set, with the watchpoints already added to it, is dropped: that watchpoint never fires (stale cache forever: **V/M**).
* `VariableWriteFireDetail::touch` on global variable writes (`symbolTablePutTouchWatchpointSet`, runtime/JSSymbolTableObject.h:111-117): `touch` is load / fence / store of the thin word (Watchpoint.h:378-395), a lost update keeps `IsWatched` where `IsInvalidated` was due. Nobody compiles against it with the JIT off.
Rule: state words LF (already are); list + inflation under one lock (per set is too fine: a striped or per-VM "watchpoint lock"); firing that guards shapes → STW.

### 10.7 Lazy state inside individual objects

#### 10.7.1 `ErrorInstance` and `Exception` (runtime/ErrorInstance.{h,cpp}, Exception.cpp; large AOT hunk)

Fields (ErrorInstance.h:177-200): `unique_ptr<Vector<StackFrame>> m_stackTrace`, **AOT** `AOT::ReturnAddressTrace* m_returnAddresses` (auxiliary heap memory, marked in the new `visitChildrenImpl`, ErrorInstance.cpp:156-166), `m_capturedStackTrace`, `m_lineColumn`, `String m_sourceURL`, `String m_stackString`, nine 1-bit flags in one byte (`m_errorInfoMaterialized`, `m_hasErrorInfo`, …).
With compiled code an error is born holding only return addresses (`tryRetainReturnAddresses`, :207-221, called from `finishCreation` :268-272). Stages, each on first need:
1. `materializeStackTrace` (:190-204): builds a new vector **reading `m_returnAddresses` and `m_stackTrace` without the lock**, then under `cellLock()` replaces `m_stackTrace` and nulls `m_returnAddresses`. Called from `stackTrace()` (ErrorInstance.h:125-130), `captureStackTrace` (:223-232), `computeErrorInfo` (:502-503), `materializeErrorInfoIfNeeded` (:560-561).
2. `materializeErrorInfoIfNeeded` (:555-632), reached from `getOwnPropertySlot`/`put`/`defineOwnProperty`/`deleteProperty` for `line`, `column`, `sourceURL`, `stack` (:634-652, 692-735): tests `m_errorInfoMaterialized`; Bun path sets the flag **first** (:567), calls the embedder hook with `*m_stackTrace` (:572), frees the trace under the lock (:574-579), then four `putDirect`s inside `AllowLazyMaterializationOfImmutableProperties` (:584-591). Other path: `computeErrorInfo` (:495-530), puts, flag **last** (:628).
3. GC end: `reconcileWeakReferencesAtGCEnd` (:470-493) may run `computeErrorInfo` itself when a frame (or, AOT, the instance's liveness token, :485-490) died.

The `cellLock()` protects only the collector's view (`visitChildren`, `estimatedSize`). Two JS threads reading `e.stack` of one error: both pass `if (m_returnAddresses)`; the second dereferences `m_returnAddresses` after the first nulled it (`m_returnAddresses->numberOfFramesOnTop`, :194: **C**) or copies from the `m_stackTrace` the first just destroyed (**M**);
in the Bun path B sees the flag set and falls through to the ordinary lookup before the puts → `e.stack === undefined` (**V**); in the other path both `putDirect` (structure transition race, **M**, objects report); `WTF::move(m_sourceURL)` (:588, 608) twice. The flag byte is RMW.
`Exception` has the same two-stage shape: `retainReturnAddresses`, `materializeStack` (Exception.cpp:110-119, 183-204: reads unlocked, swaps under `cellLock`), and `takeStackFrom` **shares** the error's `ReturnAddressTrace` pointer with the exception (:133-137).
Rule: errors cross threads rarely but legitimately (rejections). One lock per error for the whole materialisation (the cell lock is free for that if the hook cannot re-enter the same error), flags published last with release.

#### 10.7.2 `JSFunction`, `FunctionRareData`

* `m_executableOrRareData`: one tagged word (`rareDataTag`). `allocateRareData` (runtime/JSFunction.cpp:155-170), `allocateAndInitializeRareData` (:196-215): create the `FunctionRareData` (which stores the executable), `storeStoreFence` ("A DFG compilation thread may be trying to read the rare data"), store, barrier. Readers are safe. **Two allocators**: the caller's "has no rare data" test (`ensureRareData`, runtime/JSFunction.h:150-156; `ensureRareDataAndObjectAllocationProfile`, JSFunctionInlines.h:288-297) and the load at :157 / :198 are separate, and in release nothing re-checks the tag. If both load the untagged word, the later store overwrites the earlier. If the second loads *after* the first's store, it passes a tagged `FunctionRareData*` to `FunctionRareData::create` as the `ExecutableBase*` (:159): a rare data whose `m_executable` is a mis-tagged pointer to another rare data (**M**). Effect of the plain overwrite: the first rare data, with flags already set in it (`hasReifiedLength`…), is lost → a property is reified twice (`putDirect` on an existing name: attributes/value reset, **V**) and allocation profiles split. Rule: CAS.
* Flags `m_hasReifiedLength`, `m_hasReifiedName`, `m_hasModifiedLength…`, `m_hasModifiedName…`: four bit-fields in one byte (runtime/FunctionRareData.h:177-180), plain RMW: lost update.
* **Order flag → property.** `reifyLength` (:588-600): `rareData->setHasReifiedLength(); AllowLazy… ; putDirect(length)`. Same in `reifyName` (:632-634) and all branches of `reifyLazyBoundNameIfNeeded` (:738-763). B sees the flag, skips reification, does the ordinary lookup and finds nothing: `f.length === undefined`, `f.name` inherited from `Function.prototype` (**V**).
* **Lazy `prototype`** (`getOwnPropertySlot` :372-386, `reifyLazyPrototypeIfNeeded` :682-695): `getDirectOffset` invalid → `putDirect(prototype, constructPrototypeObject())`. Two threads: two prototype objects, one survives; objects already constructed against the other keep it (**I**: `new F() instanceof F` false).
* `FunctionRareData` contents: `m_objectAllocationProfile` (structure + prototype + allocator words), `m_allocationProfileWatchpointSet`, `m_internalFunctionAllocationProfile`, `m_boundFunctionStructureID`, `m_allocationProfileClearingWatchpoint` (`unique_ptr`) (:171-176); filled by `initializeObjectAllocationProfile` (FunctionRareData.cpp:91-134), `createInternalFunctionAllocationStructureFromBase` (.h:110-114), cleared by `clear` (.cpp:136-144) from a watchpoint fire. Multi-word profile vs. `clear`: structure of one prototype with the prototype of another (**V**; **M** if inline capacity and allocator disagree — not read).
* **AOT hunk**: `setCompiledAllocation` writes an 8-bit and a 16-bit half of the constructor number and then the StructureID (FunctionRareData.h:77-87, 181-185): three stores; reader pairs `compiledAllocationConstructor()` with `compiledAllocationStructure()`. A torn pair gives the Structure cached for *another* compiled constructor with this `new.target` → object of the wrong layout: **M**. (Consumer `AOT::ClassTable::createThis` not read.)
* AOT closures: `CompiledClosureType` functions are `JSFunction`s that count as host functions; `name`, `toString`, display name and lazy `name` come from `AOT::CompiledCallee` (JSFunction.cpp:234-237, 253-256, 308-316, 536-541, 754-759) i.e. from image data, but **reification still goes through the same rare data, flags and `putDirect`**. "Slim" closures (`isSlimClosureType`) are not `JSFunction`s (:522); see the closures report. `JSFunction::create(…, BuiltinCode, …)` asks `AOT::builtinFunction(globalObject, code.id)` first (:114-136): a per-realm table of built-in function objects (not read here).
* `NativeExecutable::m_asString` (runtime/NativeExecutable.h:91-93, 114; .cpp:123-135) and `FunctionExecutable::RareData::m_asString` (runtime/FunctionExecutable.cpp:182-193, with a fence): shape (a), strings: **P**.

#### 10.7.3 `JSBoundFunction`, `JSRemoteFunction`

`m_nameMayBeNull` (null = not computed; `nameSlow` ends in `m_nameMayBeNull.set`, runtime/JSBoundFunction.cpp:341), `m_length` (`PNaN` = not computed; `lengthSlow` :385-416), `m_canConstruct` (`TriState::Indeterminate`; `canConstructSlow` :418-434) — JSBoundFunction.h:61-109, 136-139. Each is one word/byte, computed from immutable inputs (target chain), so racing fills are idempotent: **P** (name: two equal strings).
The AOT hunk adds `AOT::CompiledCallee::length(cursor)` for slim closures (:390-395). `JSRemoteFunction::m_nameMayBeNull`/`m_length` are written once in `copyNameAndLength` during creation (runtime/JSRemoteFunction.cpp:268-279). Reified `name`/`length` properties go through 10.7.2.

#### 10.7.4 Arguments objects

| Object | Lazy state | Code | Race | Sev. |
|---|---|---|---|---|
| `DirectArguments` | `m_mappedArguments` (null = `length`/`callee`/iterator not materialised) runtime/DirectArguments.h:183; `m_modifiedArgumentsDescriptor` (GenericArgumentsImpl.h:66) | `overrideThings` DirectArguments.cpp:128-148: `RELEASE_ASSERT(!m_mappedArguments)`, three `putDirect`, allocate, **publish (:145), then zero-fill (:146-147)** | second thread: `RELEASE_ASSERT`; reader of `isMappedArgument` (.h:82) sees uninitialised bytes → treats a live argument as unmapped | C / V |
| `ScopedArguments` | `bool m_overrodeThings` ScopedArguments.h:170 | ScopedArguments.cpp:136-147: `RELEASE_ASSERT(!m_overrodeThings)`, puts, flag last | `RELEASE_ASSERT`; double `putDirect` | C |
| `ClonedArguments` | `m_callee` (cleared = specials materialised) ClonedArguments.h:100-104 | `materializeSpecials` ClonedArguments.cpp:283-303; `getOwnPropertySlot` :209-232 tests `specialsMaterialized()` and then loads `m_callee` again: `thisObject->m_callee->executable()` (:215) | null dereference between test and use; `RELEASE_ASSERT(!specialsMaterialized())` | C |
| `GenericArgumentsImpl` | `initModifiedArgumentsDescriptor` GenericArgumentsImplInlines.h:278-287: `RELEASE_ASSERT(!m_modifiedArgumentsDescriptor)` | | | C |

Compiled code creates and reads these (aot/runtime/AOTArgumentsOperations.cpp, AOTCallOperations.cpp, aot/codegen/AOTAllocationEmitterObjects.cpp, aot/object/AOTCellFormat.cpp mention them; not read). An arguments object rarely leaves its thread; rule: LK on the cell, or accept C→ make the asserts re-checks.

#### 10.7.5 Static property tables (`HasStaticPropertyTable`, runtime/Lookup.{h,cpp}, JSObject.cpp)

`getOwnStaticPropertySlot` → `setUpStaticFunctionSlot` (Lookup.cpp:54-108): `getDirectOffset` invalid and `!staticPropertiesReified()` → `reifyStaticProperty` (Lookup.h:486-583: creates the function / accessor / constant, or pulls a `LazyCellProperty` (:533-539) or `LazyClassStructure` constructor (:542-548), or runs a `PropertyCallback` that may enter JS (:551-560); each `putDirect*` inside `AllowLazyMaterializationOfImmutableProperties`) → look the offset up again, `RELEASE_ASSERT_NOT_REACHED` if still absent (:92-95).
AOT hunks: on an exception a stand-in for "a member left out" is installed (`AOT::takeStandInForMemberLeftOut`, :79-89) and every new non-accessor member is reported to `AOT::RuntimeHooks::didCreateStaticProperty` (:97-100), which records it in `builtinCells`, `locationWatch`, and **locks** it (aot/runtime/AOTRealmStartup.cpp:514-536). With `--trim`, custom values are reified too (`setUpReifiedCustomSlot`, :110-114).
`reifyAllStaticProperties` (JSObject.cpp:3146-3197; on first `delete`, JSObject.cpp:2316-2324, and others): `convertToDictionary`, loop, then `structure()->setStaticPropertiesReified(true)` — a bit in `Structure::m_bitField`, written as *clear then or* (`DEFINE_BITFIELD`, Structure.h:895-905): non-atomic RMW on a word with 25 other flags.
Races: check-then-put → **two function objects** for `Array.prototype.map` etc., the first possibly already cached by compiled code or locked by the AOT hook (**I**, and the builtin-cell tables then disagree with the object: **M** for code specialised on the cell); concurrent `putDirect` on the shared prototype (**M**).
This is *the* most exposed lazy mechanism: every first use of any built-in method goes through it, on objects that all threads share. Rule: EG for the members the image references (the compiler knows them: `BuiltinTable`), the per-realm lazy lock of 10.2 for the rest.

#### 10.7.6 `Structure` and `StructureRareData`

| State | Declaration | Writer | Lock | Race | Sev. |
|---|---|---|---|---|---|
| `m_previousOrRareData` | runtime/Structure.h:1150 | `allocateRareData` Structure.cpp:1892-1899: create, fence, store; called after an **unlocked** `if (!hasRareData())` (Structure.cpp:1909-1910, 2407-2408; StructureInlines.h:229-230; Structure.h:567) | none | two rare datas; whatever the loser already stored (replacement sets, cached enumerator, `m_maxOffset`/`m_transitionOffset`, **`m_immutablePropertyNames`**) vanishes | V → M (immutability list lost) |
| `m_cachedPrototypeChain` | Structure.h:1148 | `prototypeChain` StructureInlines.h:175-185: validate, `clearCachedPrototypeChain()`, `set`, then **re-load and return** (:184) | none | B's clear between A's set and A's return: A returns null → `canCachePropertyNameEnumerator` dereferences it (`structureChain->head()`, Structure.cpp:2432-2434) | C |
| `m_cachedPropertyNameEnumeratorAndFlag` + `m_cachedPropertyNameEnumeratorWatchpoints` | runtime/StructureRareData.h:172-173 | `setCachedPropertyNameEnumerator` StructureRareDataInlines.h:122-128: replaces the `FixedVector` of watchpoints (destroying installed ones), installs new ones, stores the word | none | vector freed while the other thread installs into it / while a fire walks the list; word itself is atomic. Read: JSPropertyNameEnumeratorInlines.h:54-59, filled :94-96. AOT hunk: compiled objects use `compiledObject->propertyNameEnumerator()` instead (JSPropertyNameEnumeratorInlines.h, first lines) and enumerators carry `m_cachedOrderWord(+Mask)`, immutable after construction | M |
| `m_cachedPropertyNames[kind]` | :174 | sentinel protocol: first call stores the sentinel `1`, second stores the butterfly after a fence (StructureRareDataInlines.h:153-162; callers runtime/ObjectConstructor.cpp:494-503, 1512-1514) | none | one word, value derived from the immutable Structure: idempotent | P |
| `m_replacementWatchpointSets`, `m_activeReplacementWatchpointSet` | :181, 188 | `ensurePropertyReplacementWatchpointSet` Structure.cpp:1901-1921 | **`Structure::m_lock`** for the map; `firePropertyReplacementWatchpointSet` decrements the 30-bit counter and flips `isWatchingReplacement` **outside** it (:1923-1935) | counter shares a word with the 2-bit `m_cachedHasDefaultToPrimitive…` (:188-189): RMW | V |
| `m_specialPropertyCache` (`toStringTag`, `toString`, `valueOf`, `toPrimitive`, `toJSON`) | :182; StructureRareDataInlines.h:45-65 | `ensureSpecialPropertyCacheSlow` StructureRareData.cpp:117-125 (fence, `unique_ptr` assign; `ASSERT(!m_specialPropertyCache)`); `cacheSpecialPropertySlow` :139-240 fills `m_missWatchpoints` (`Bag`), `m_equivalenceWatchpoint`, then `m_value` last; `clearCachedSpecialProperty` :242-257 | **none** | double init frees the cache a reader holds (`cachedSpecialProperty` :97-110); Bag/`unique_ptr` mutated by two fillers; clear against fill leaves a value without watchpoints (stale `Object.prototype.toString` result forever) | M |
| — AOT hunk | StructureRareData.cpp:161-168 (in `cacheSpecialPropertySlow`) | for `isCompiledStructure()` the value is cached only if `AOT::PropertyWalk::hasPropertyOnChain == Absent`, without watchpoints, else give up | | who invalidates it was not read | ? |
| `m_immutablePropertyNames` (AOT hunk) | StructureRareDataInlines.h:59, 173-176 | `setImmutablePropertyNames` StructureRareData.cpp:127-130 (`RefPtr` in the special property cache) | none | set when an object is locked; read on every put to a "listed immutable" structure | M if set after sharing |
| `m_polyProtoWatchpoint` (`Box<>`) | :183 | `setSharedPolyProtoWatchpoint` :108 | none | refcounted swap; poly proto is created by `ObjectAllocationProfile` | M |
| `m_previous` | :185 | `setPreviousID`/`clearPreviousID` | none | one 32-bit word; GC clears it | objects report |
| `Structure::m_bitField` | Structure.h:895-932 | flags "can be flipped on the main thread at any timing" (:914, 919): `definitelyNonThenableState`, `isWatchingReplacement`, `didWatchInternalProperties`, `staticPropertiesReified`, `mayBePrototype`, `transitionWatchpointIsLikelyToBeFired`… | none | setter = two plain stores (`&=` then `\|=`): a concurrent setter of another flag writes back a stale word and can **undo** e.g. `mayBePrototype` or resurrect a cleared flag | V → M |

#### 10.7.7 `ProxyObject` trap cache, typed arrays, `ArrayBuffer` wrapper

* `ProxyObject`: `m_handlerTrapsOffsetsCache[]`, `m_handlerStructureID`, `m_handlerPrototypeStructureID` (runtime/ProxyObject.h:143-145). Hit test compares the two IDs (:150-153), then uses the offset (ProxyObject.cpp:124). Fill writes offset, then the IDs (:178-180; AOT branch for compiled handlers stores a *field number*, :163-165); mismatch → `clearHandlerTrapsOffsetsCache` (`fill_n`, :193-195).
  A late offset store by a thread that looked the trap up under the handler's previous Structure lands after another thread validated the cache for the new one: `getDirect(staleOffset)` on the handler — wrong slot, possibly beyond its storage; in the AOT branch `layout().fields()[number]`. **M** (result is at least checked by `ensureIsCallable`). Rule: PT impossible (per object) → OFF, or pack (structureID, offset) per trap in one 64-bit word.
* `JSArrayBufferView::slowDownAndWasteMemory` (runtime/JSArrayBufferView.cpp:271-334), the lazy creation of `.buffer`: `ASSERT(!hasArrayBuffer())`; `FastTypedArray`: **copy** the elements into a new `ArrayBuffer` (:299); `OversizeTypedArray`: `ArrayBuffer::createAdopted(span())` (:308); new butterfly; then under `cellLock()` set the buffer, `m_vector`, fence, `m_mode` (:324-330). The lock is for the collector/compiler.
  Two threads: two buffers adopting one vector, each with `primitiveGigacageDestructor()` (runtime/ArrayBuffer.cpp:302-306) → double free (**M**); elements stored by B between A's copy and A's `m_vector` swing are lost (**V**), and B may keep writing into the old GC-owned vector through a pointer it loaded earlier. Compiled typed-array accesses load `m_vector` themselves (other report). Rule: LK + re-check for creation; the copy case needs STW or "fast typed arrays are never shared" (convert on publication).
* `ArrayBuffer::m_wrapper` (`Weak<JSArrayBuffer>`, runtime/ArrayBuffer.h:338): `SimpleTypedArrayController::toJS` get-then-create, `ASSERT(!native.m_wrapper)`, `Weak` assign (runtime/SimpleTypedArrayController.cpp:44-57): **I** + **M**.
* `JSCallbackObject`: `m_privateProperties` is a lazily made `unique_ptr<JSPrivatePropertyMap>` (API/JSCallbackObject.h:133); class data per realm in `JSGlobalObject::RareData::opaqueJSClassData` (10.3). C API only. `JSGlobalProxy::m_target`: one word, `setTarget` (runtime/JSGlobalProxy.cpp:49-51), embedder navigation only.

#### 10.7.8 Dates

There is **no `DateInstanceCache` and no `DateInstance::m_data`** in this tree. A `DateInstance` holds `double m_internalNumber` and two `PlainGregorianDateTime` (one 64-bit payload each, `static_assert(sizeof == 8)`, wtf/PlainGregorianDateTime.h:164-166; zero = never computed, a stale marker otherwise) (runtime/DateInstance.h:98-100).
`gregorianDateTime(cache)`: non-zero → return, else compute and store (DateInstance.h:64-76; .cpp:52-71). `setInternalNumber`: number, then two stale markers (:53-60). Compiled code does the same three stores inline (aot/builtins/AOTDateBuiltinEmitter.cpp:58-68) and reads the fields by offset (:16-25).
Each word is atomic; the *triple* is not: A `setTime`, B `getHours`: B can read the new number and store a breakdown of it *before* A's stale-marker store wipes it (harmless), or compute from the old number and store *after* the marker: the date then answers `getTime()` new and `getHours()` old until the next set (**V**, persistent). No memory hazard. Rule: accept, or version the payload.

`DateCache` (runtime/JSDateMath.h:215-224), all unlocked:
| Field | Use | Race | Sev. |
|---|---|---|---|
| `m_timeZoneCache` (`unique_ptr<OpaqueICUTimeZone>` holding a `UCalendar`) | lazily opened `timeZoneCacheSlow` JSDateMath.cpp:565-579; `calculateLocalTimeOffset` does `ucal_setMillis` and then `ucal_get`/`ucal_getTimeZoneOffsetFromLocal` on the **shared calendar** (:135-151); reset by `clearForTimeZoneChange` (:583) | set/get pairs interleave (offset of another instant); ICU object mutated concurrently; freed by a thread entering the VM (VM.cpp:2134-2136) while in use | M |
| `m_caches[2]` (`DSTCache`: 32 entries, `m_before`/`m_after` pointers, epoch) | `localTimeOffset` :225-330 | inconsistent intervals → wrong offset | V |
| `m_yearMonthDayCache` (`optional` of 4 ints) | :351-367 | torn: wrong day/month | V |
| `m_cachedDateString` + `m_cachedDateStringValue` | `parseDate` :406-468 (compare `String`, return value; assign both) | refcounted swap; string of one parse, value of another | M |
| `m_brokenDownDateCaches[2]` (8 × {double key, 8-byte value}) | JSDateMath.h:173-204 | (b) | V |
| `m_timeZoneStandardDisplayNameCache`, `m_timeZoneDSTDisplayNameCache` | `timeZoneDisplayName` :478-499 | refcounted swap | M |
| `m_cachedTimeZoneID`, `m_mayHaveCachedLocalGregorianDateTime` | `hasTimeZoneChange` (.h:92-99; Bun defines `USE_TIME_ZONE_CHANGE_NOTIFICATIONS`, wtf/PlatformUse.h:74) | | V |
Rule: PT (a `UCalendar` per thread); the time-zone-change wipe must reach every copy.

#### 10.7.9 Intl and Temporal

* **Process-global tables are already safe**: `LazyNeverDestroyed` + `std::call_once` for available locales (runtime/IntlObject.cpp:445-447, 583-585, 604-606), ICU default locale (:861-863), numbering systems (:1153-1155), calendars (:1721-1723, 1756-1758, 1781-1800), time zones (:2044-2046, 2170-2172, 2239-2240); `IntlCache::s_languagesEpoch` atomic (IntlCache.cpp:42). The `String`s inside are shared by all threads: fine for refcounts (fact 1), but they must not be atomised in place into one thread's table (`addSlowCase` turns the *given* `StringImpl` into the atom) — with one VM-wide table this stops being a per-thread question.
* **`IntlCache`** (runtime/IntlCache.h:55-116), per VM, unlocked: `m_cachedDateTimePatternGenerator` + locale (:97-109; replaced in `cacheSharedPatternGenerator` IntlCache.cpp:61-67; the returned raw `UDateTimePatternGenerator*` is then used, :71-94) → freed under the user, **M**; `m_cachedCanonicalizedLocaleIDs` `HashMap<String,String>` (IntlCache.cpp:96-112 ff.) (c) **M**;
  `m_cachedDateTimeFormatImpls` `TinyLRUCache` of 4 (`findIfCached`/`insert`, wtf/TinyLRUCache.h:148-170) **M**; it makes several `Intl.DateTimeFormat` objects **share one `IntlDateTimeFormatImpl`** and thus one `UDateFormat`. Rule: PT.
* **Realm defaults** `m_defaultCollator`, `m_defaultDateTimeFormat/DateFormat/TimeFormat`, `m_defaultNumberFormat`: one ICU-backed object per realm used by `localeCompare` (runtime/StringPrototype.cpp:1798; **aot/runtime/AOTStringBuiltinOperations.cpp:135**), `toLocale*String` (DatePrototype.cpp:998-1050, NumberPrototype.cpp:630, six Temporal prototypes). Besides 10.2, all threads then call ICU on the same object.
* **ICU calls on shared objects**: `ucol_strcoll(UTF8)` (IntlCollator.cpp:307-314), `unumf_formatDouble/Decimal` into a fresh result (IntlNumberFormat.cpp:594-618), `uplrules_select*` (IntlPluralRules.cpp:264-355), `udat_format`/`udat_formatForFields` on `m_impl->m_dateFormat` (IntlDateTimeFormat.cpp:1466, 1548, 1633). Which of these ICU entry points are safe on one object from several threads is an ICU contract that I did not read; ICU's general rule is "const use only".
  Certainly unsafe by construction: `IntlRelativeTimeFormat` formats into **member** scratch objects `m_formattedResult` and `m_cfpos` (IntlRelativeTimeFormat.h:82-83; .cpp:309-347); `IntlSegments`/`IntlSegmentIterator` move a member `UBreakIterator` (`ubrk_following`, `ubrk_next`, IntlSegments.cpp:77-84, IntlSegmentIterator.cpp:75).
* **Lazily created members** (shape (a) with `unique_ptr` → double init frees, **M**): `IntlDateTimeFormat::m_dateIntervalFormat` (.cpp:1719-1768), `IntlDateTimeFormatImpl::m_temporalFormatterCache` and its per-kind formatters (:2297-2302; `mutable` in a `const`-shared impl), `IntlNumberFormat::m_numberRangeFormatter` (:562-569), `IntlDurationFormat::m_formatterCache` (:649-655);
  `mutable String m_calendar/m_numberingSystem` filled in `resolvedOptions` (IntlDateTimeFormat.cpp:165-172, 1319-1321; IntlNumberFormat.cpp:1227; IntlRelativeTimeFormat.cpp:206; IntlDurationFormat.cpp:301) refcounted swap; `IntlLocale`'s 9 `String` + 6 `optional<String>` getters (`if (m_x.isNull()) m_x = …`, IntlLocale.cpp:475-700) same;
  `m_boundCompare`/`m_boundFormat` (`collator.compare` getter creates the bound function on first read: IntlCollatorPrototype.cpp:136, IntlNumberFormatPrototype.cpp:131, IntlDateTimeFormatPrototype.cpp:284) **I**; `IntlCollator::m_canDoASCIIUCADUCETComparison` TriState (IntlCollator.h:72-74, .cpp:435) idempotent **P**.
* Rule for Intl objects: a lock per object around every ICU use (they are slow paths), or clone the ICU object per thread (`udat_clone` is already used for Temporal formatting, IntlDateTimeFormat.cpp:2374-2544).
* **Temporal**: no file-level or function-level mutable statics found in `Temporal*.cpp`/`ISO8601.cpp` by the search in 10.9; the eight `LazyClassStructure`s and the realm default formatters are its only shared lazies.

#### 10.7.10 Modules

`AbstractModuleRecord::m_resolutionCache` (`HashMap`, runtime/AbstractModuleRecord.h:416): `tryGetCachedResolution`/`cacheResolution` .cpp:691-702, unlocked, filled during `resolveExport` (:1024-1026, 1146, 1170, 1211, 1391) — which also runs lazily from `getModuleNamespace`. `m_moduleNamespaceObject`/`m_deferredNamespaceObject`: null-check then create (:1267-1271 ff.) → **I** (`import * as ns` identity).
`m_loadedModules`: written under `cellLock()` (comment :568, `set` :580; `ensure` at :413), read unlocked (:261, 314). `m_asyncParentModules` (`Vector`), `m_pendingAsyncDependencies`, `m_cycleRoot`, `CyclicModuleRecord::m_status` (CyclicModuleRecord.h:89-99): the spec's link/evaluate state machine, plain fields.
`JSModuleLoader`: `m_moduleMap`, `m_resolutionFailures`, `m_loadedModules`, `m_prelinkedRecords` (runtime/JSModuleLoader.h:285-317): **mutations under `cellLock()`** (.cpp:911-912, 1246-1247, 1267, 1364-1365, 1371-1374), **lookups not** (:316-321, 788, 830, 1241, 1255, 1361) — the mutator/collector pattern of fact 3; find-then-add creates two registry entries for one key (module evaluated twice: **I**).
`JSModuleNamespaceObject::m_exports` is filled at creation (JSModuleNamespaceObject.h:98-100). `VM::m_moduleAsyncEvaluationCount` plain `++` (VM.h:1354; aot/runtime/AOTModuleEvaluation.cpp:399). Compiled modules have their own evaluation code (`aot/runtime/AOTModuleEvaluation.cpp`, not read). Rule: dynamic `import()` from several threads needs one loader lock held across find-or-create; static graphs are linked before threads exist.

#### 10.7.11 Microtasks and promises

`MicrotaskQueue` (runtime/MicrotaskQueue.h:228-300): `MarkedMicrotaskDeque m_queue` (= `Deque<QueuedTask>` + `m_markedBefore`), `m_toKeep`, `m_isScheduledToRun`, `m_isPerformingMicrotaskCheckpoint`; `RefCounted` (not thread-safe); linked in `VM::m_microtaskQueues`. No lock.
**Compiled stubs enqueue inline**: AOT hunk exports `offsetOfStart/End/CapacityMask/Buffer/MarkedBefore/IsScheduledToRun` (:277-284), used at aot/stubs/AOTStubsCoroutineJobs.cpp:37-95 and AOTStubsCoroutinePromises.cpp:211 (load `end`, compute slot, write the task, store `end`). Two enqueuers write one slot and lose a job, or one grows the buffer (**M**). The collector visits it (`visitAggregate`, `beginMarking`, VM.cpp:2256-2279).
Rule: PT queue (a job runs on the thread that queued it? — a semantic decision for the jobs report); then `m_aboutToBeNotifiedRejectedPromises`, `m_drainMicrotaskDelayScopeCount`, `m_syncResumeCallCache`, `m_synchronousModuleQueue` go with it. `JSPromise` state lives in internal fields (flags + reactions): a multi-word state machine per object, covered elsewhere.

#### 10.7.12 `WeakRef`, `FinalizationRegistry`, `DeferredWorkTimer`

* `JSWeakObjectRef::deref` (runtime/JSWeakObjectRef.h:47-56): `if (m_value && vm.currentWeakRefVersion() != m_lastAccessVersion) { m_lastAccessVersion = …; writeBarrier(this); } return m_value.get();`; the visitor keeps the target alive iff the versions are equal (JSWeakObjectRef.cpp:51-53). The version is bumped at the end of each synchronous job (`finalizeSynchronousJSExecution`, VM.h:1214-1219, from `drainMicrotasks` VM.cpp:1837).
  With threads, A finishing a job bumps the VM-wide version in the middle of B's job: the target B just `deref`ed may be collected before B's job ends (violates the KeepDuringJob guarantee: second `deref()` in the same job gives `undefined`: **V**; the value B holds is on its stack, so no **M**). `m_value` is loaded twice. Rule: version per thread + the visitor compares against all live threads' versions, or a per-thread keep-alive list.
* `JSFinalizationRegistry`: all four tables are accessed under `cellLock()` (runtime/JSFinalizationRegistry.cpp:74, 105, 188, 212, 229; .h:116-120): **already locked**.
* `DeferredWorkTimer` (runtime/DeferredWorkTimer.h:122-127): `m_tasks` under `m_taskLock` (.cpp:93, 245, 283, 297, 339); **`m_pendingTickets` (`HashSet<Ref<Ticket>>`) is guarded by the API lock only** (asserts :196, 223, 232, 263, 346, 352; `add` :211). (c) **M**. An embedder hook `onAddPendingWork`, if installed, takes the ticket instead of the set (:208-213). Rule: LK.

### 10.8 State that only interpreted code (eval / `new Function` / `node:vm`) needs

Reachable only when `image->admitsUnknownJavaScript()`. None has a lock for mutators unless stated.

| State | Declaration | Writers | Shape |
|---|---|---|---|
| `CodeCache::m_sourceCode` | runtime/CodeCache.h:242-269 | lookups/insertions on every compile; `clear`, `clearCodeDecodedFromPersistentPayloads` (VM.cpp:1202-1216, 1253) | map + age list, (c) |
| `BuiltinExecutables::m_unlinkedExecutables[]` | builtins/BuiltinExecutables.h:95 | `if (!m_unlinkedExecutables[index]) … = createBuiltinExecutable` (.cpp:316-322); `clear` :303; `linkFromText` :349-353 (AOT hunk: used when the image has no compiled form of a built-in, JSFunction.cpp:106-112) | (a) |
| `sourceProviderCacheMap` | VM.h:821-822 | `addSourceProviderCache` VM.cpp:1284-1291; pruned in the GC epilogue :1293-1298; cleared :1267 | (c), `RefPtr` values |
| `m_compactVariableMap`, parser arenas, `m_functionHasExecutedCache` | VM.h:1012, 1479 | parser / bytecode generator | (c) |
| `ScriptExecutable` / `FunctionExecutable`: `m_rareData` (`ensureRareDataSlow`, fence + `unique_ptr`, runtime/FunctionExecutable.cpp:170-180), `m_codeBlockForCall/Construct` (FunctionExecutable.h:83-93), `m_singleton` (:254), `m_cachedPolyProtoStructureID` (:262-268), `m_overrideLineNumber` | | `prepareForExecution`, `installCode`, `clearCode` | (a); double compile installs two CodeBlocks |
| Template objects, interpreted | `TemplateObjectMap` per executable (runtime/ScriptExecutable.h:46; members FunctionExecutable.h:319, ProgramExecutable.h:97, EvalExecutable.h:91, ModuleProgramExecutable.h:174) | `createTemplateObject` ScriptExecutable.cpp:467-484: `add` under `cellLock()`, then **uses the iterator after unlocking** (:478, 482); `ensureTemplateObjectMapImpl` :486-494 (a) | iterator invalidated by another thread's `add` (**M**); two template objects for one site (**I**) |
| `UnlinkedFunctionExecutable`: `m_unlinkedCodeBlockForCall/Construct` in a **union** with `RefPtr<Decoder> m_decoder` / cached offsets, discriminated by the bit-field `m_isCached` (bytecode/UnlinkedFunctionExecutable.h:386, 419-429); `ensureRareData` (:463-470); deferred members | | decode on first use | union re-typed in place: **M** |
| `SourceProvider`: `m_id` lazily from a `static std::atomic<SourceID>` (parser/SourceProvider.cpp:89-95; the check `if (!m_id)` is plain: two IDs possible), `m_sourceURLDirective`/`m_sourceMappingURLDirective` `String` setters (SourceProvider.h:211, 309), line starts **locked** (`m_lock`, :134-143) | | | |
| `JSGlobalObject::m_symbolTableCache`, `m_resumableCodeSymbolTableClones`, `m_moduleProgramExecutables`, `m_executableForCached…FunctionConstructor`, `m_globalLexicalBindingEpoch`, `m_varInjectionWatchpointSet` | 10.3 | | |
| LLInt metadata (inline caches, `resolve_scope` epochs). The AOT handlers "write no profiles" (PR text; llint/LowLevelInterpreterAOT.asm not read) | | | |
| `VM::varargsLength`, `newCallFrameReturnValue`, `encodedHostCallReturnValue`, `m_mightBeExecutingTaintedCode`, `m_loopHintExecutionCounts` | 10.1.1 | | |

**Template objects in compiled code**: one word per site in the instance's statics. `generateNewTemplateObject` (aot/codegen/AOTAllocationEmitterObjects.cpp:39-60): load the word; if zero call `operationAOTNewTemplateObject` (aot/runtime/AOTConstantOperations.cpp:96-120: builds and freezes `cooked` and `raw`), `mutatorFence()`, `store64`, mark the statics card. Shape (a): the object is complete before it is published, but two threads can each create one and each *use its own* → identity of the template object differs between the first calls (**I**; tagged-template caches keyed by it, e.g. `WeakMap`, see two keys). Rule: CAS and use the winner.

Suggested rule for the bucket: one VM-wide "compile lock" around parse / generate / link / install and the maps above, per-thread for the call-return scratch fields; or forbid `eval`-family calls on threads other than the first in v1.

### 10.9 Process-global state written at run time

Search: function- and file-level `static` without `const`/`constexpr` in `runtime/ interpreter/ bytecode/ parser/ builtins/ API/`, plus `thread_local`.

| State | Where | Protection | Verdict |
|---|---|---|---|
| `g_jscConfig`, `Options` | `Config::finalize()` at the end of the first VM's constructor (VM.cpp:601); setters `RELEASE_ASSERT(!g_jscConfig.isPermanentlyFrozen())` (runtime/JSCConfig.cpp:38, 44) | frozen after start | — |
| `VMIdentifier::generate()` | VM.h:259-260, VM.cpp:269 | atomic | — (there is no `VM::s_numberOfIDs`) |
| host trampolines `jitCodeForCallTrampoline` / `…Construct…` | VM.cpp:989-1034 (incl. the AOT `llint_aot_char_code_at_thunk`) | `call_once` | — |
| `SymbolImpl::s_nextHashForSymbol` | wtf/text/SymbolImpl.cpp:98 | none | P |
| `SourceProvider` id counter | parser/SourceProvider.cpp:92 | atomic | — |
| `lastArraySize` | runtime/JSObject.cpp:78, 3986-3993 (growth heuristic) | none, one `unsigned` | P |
| host time zone | runtime/JSDateMath.cpp:501-546 | `timeZoneCacheLock` | — |
| Intl tables | 10.7.9 | `call_once` | — |
| `AtomStringTable::s_readOnlyAtomLookup` | wtf/text/AtomStringTable.cpp:33-59 | atomic pointer; `setReadOnlyAtomLookup` edits the *calling thread's* table | set once at start |
| `cryptographicallyRandomNumber` | wtf/CryptographicallyRandomNumber.cpp:54-151 | `Lock m_lock` | — |
| `WTF::numberToString` family | wtf/dtoa.h:33-58 | caller-supplied `NumberToStringBuffer` | — |
| `BufferMemoryManager`, `WaiterListManager`, `VMManager`, `NativeCalleeRegistry`, `SideDataRepository`, `JSRunLoopTimer::Manager`, `BufferAccessorRegistry`, `ScriptFetchParameters` shared, `ProfilerSupport` | `LazyNeverDestroyed` singletons (runtime/*.cpp; list in the search output) | already used from several VMs/threads; internals not read | assumed — |
| `s_destructionLock`, `vmCreationShouldCrash`, `VM::canUseAssembler` static | VM.cpp:244, 265, 628 | start-up | — |
| `SuperSampler`, `OpcodeStats`, `ExceptionFuzz` counter, `SamplingProfiler` statistics, `logOutcomeImpl` (`#if FAST_STRINGIFY_LOG_USAGE`, runtime/JSONObject.cpp:912-925) | tooling | mixed | OFF |
| `StructureID` | an encoded address (`StructureID::encode/decode`, runtime/StructureID.h:37 ff.); allocation = allocating the Structure cell | heap report | |
| **Already `thread_local`** | `stringifyHints`, `reasonOfLastFailure` (runtime/JSONObject.cpp:794-796); `places[]` memo of image data (aot/runtime/AOTPrivateNameOperations.cpp:193-211: "Layouts and their fields are data of the image, so what is remembered never goes stale"); `quotedSource` (AOTErrorMessages.cpp:472); `objectListedWithoutOrder` (aot/object/AOTObjectInternal.cpp:42); validation helpers (AOTNativeCallValidation.cpp:114-117, AOTRuntimeValidation.cpp:343, AOTAssignmentPlans.cpp:111); `AssertNoGC::s_scopeReentryCount` (heap/DeferGC.cpp:32) | | good precedent; none holds a GC cell except `objectListedWithoutOrder` (a raw `const JSObject*`, scope-like use; not read) |

### 10.10 Summary

**By danger and exposure** (what an ordinary program on two threads hits first):
1. Atom string table and the `RefPtr`/`String` cache slots (10.5, 10.1.4): any computed property key, `JSON.parse`, number→string.
2. Static property table reification + `LazyProperty`/`LazyClassStructure` (10.7.5, 10.2): first use of any built-in; also feeds AOT's built-in cell tables and locking.
3. The two mode scopes (10.1.6): they silently suspend the immutability that compiled code is proved against.
4. Microtask queue with inline enqueue (10.7.11); `Symbol()` → `symbolImplToSymbolMap` (10.5); `getHostFunction` map (10.1.5); `propertyRuns` (10.1.5).
5. Scratch state used as if it were a local: `stringSplitIndice`, searcher tables, `stringRecursionCheck*`, `numericStrings` references, `regExpMatchOutput`, `m_syncResumeCallCache`.
6. Per-object lazies on objects that are naturally shared: functions (`prototype`, `name`, `length`), Structures' rare data, errors, typed-array `.buffer`, Intl objects.
7. `dateCache`/`intlCache`: shared mutable ICU objects.

**Counts.** VM: about 45 members are thread state (PT by nature), about 25 caches/tables need a rule, of which 14 are best PT (pure accelerators with direct-mapped entries), 8 need a lock (identity-bearing tables), 3 can be EG. Realm: 83 + 57 lazies + link-time constants under one protocol; 66 inline + 7 fat watchpoint sets; about 12 other mutable members.

**What helps.**
* JIT off removes the megamorphic cache, sort scratch, checkpoint side state, OSR fields, referenced-global watchpoint sets and all *code* that depends on watchpoints: a late or lost fire can no longer leave stale machine code, only a stale C++ fast-path decision.
* A closed image removes the whole of 10.8.
* Program identifiers are read-only static atoms outside any table (10.5).
* Most accelerators verify hits by content or are pure functions of immutable inputs, so "per thread" costs only memory: `JSONCache`, `NumericStrings` (≈ 4096 × 24 B + 3 × 1024 entries), `HasOwnPropertyCache` (2048 × 16 B), `StringSplitCache`, `StringReplaceCache`, `KeyAtomStringCache` (4 KB), searcher tables, `DateCache`.
  But compiled stubs take `numericStrings` table addresses from the **instance** (`realm.smallIntStrings`, `realm.intStrings`), so PT there means a per-thread word reachable from compiled code.
* Locks exist already at many of the right places (`SymbolTable::m_lock`, `Structure::m_lock`, `StructureCache::m_lock`, `cellLock()` in loader, module records, errors, registry; the empty `AtomStringTableLocker`): the work is to make the *mutator's reads* take them and to re-check after acquiring.
* Fences before publication are present wherever a compiler thread used to read (`allocateRareData`, `ensure*RareDataSlow`, `setCachedPropertyNames`, `inflateSlow`, `ffiContext`); they are missing in `LazyProperty::set`, `LazyUniqueRef::set` and the `setWithoutWriteBarrier` lazies of `VM`.

### 10.11 Not found / not sure

* **Names in the task that do not exist in this tree**: `m_sentinelSetBucket` (the ordered hash table sentinels are eager, VM.cpp:436-437), `m_fastBoundExecutable` (now `m_fast/slowCanConstructBoundExecutable`), `jsonAtomStringCache`/`JSONAtomStringCache` (now `JSONCache`), `DateInstanceCache`, `DateInstance::m_data`, `m_templateRegistry`/`TemplateRegistry`, `m_symbolTableHasher`, `m_lastStaticGlobalOffset`, `m_customGetter/SetterFunctionMap` (now `WeakGCSet`s), `VM::s_numberOfIDs`, a `VM` member `m_arityCheck…` (the name occurs only in call-link/JIT files), `VM::m_weakRandom` (it is `m_random`).
* Not read: consumers of `JSONCache::findName` results (how far `to`/`offset` are trusted), `AOT::ClassTable::createThis` (consumer of `compiledAllocationStructure`), `AOT::builtinFunction`, `builtinCells`/`BuiltinLocker` internals, `ObjectAllocationProfile::initializeProfile`, `InternalFunctionAllocationProfile`, `HeapIterationScope`, `CompactTDZEnvironmentMap`, `CodeCacheMap`, `PersistentBytecodePayloads`, `llint/LowLevelInterpreterAOT.asm`, `aot/runtime/AOTModuleEvaluation.cpp`, `AOTArgumentsOperations.cpp`, `TypeProfiler`, `ShadowChicken`, `Watchdog`, `HeapProfiler`, `FFI::FFIContext`, `BuiltinNames` (whether private-name tables grow after start-up).
* Not sure: on which thread, and with which other threads stopped, `sweepAndClearCachesInEpilogue` and the `reconcileWeakReferencesAtGCEnd` passes run (10.1.7, 10.4). Whether subject indexing in `AdaptiveStringSearcher` is bounds-checked (decides V or M). Whether any code compares the lazily made `NativeExecutable`s of 10.1.3 by pointer. Who calls `scratchBufferForSize` with the JIT off. Who invalidates the watchpoint-less special-property cache entry of compiled structures (10.7.6).
* ICU's own thread-safety guarantees per entry point were not checked against ICU sources or documentation; rows in 10.7.8/10.7.9 that depend on them say so.
* The embedder (Bun) adds its own `ClientData`, lazy structures on `Zig::GlobalObject`, `onAddPendingWork`, error-info hooks: outside this section.
* ARM64 "pointer before contents" entries are reasoning from the absence of a fence in the quoted code, not observed failures; on x86-64 those rows reduce to duplicate creation.

---------------------------------------------------------------------------------------------------------------------------------------

## 11. Hazards with several mutators (sections 1 to 6)

Sections 7, 8, 9 and 10 end with their own lists. Severity: **M** = memory safety (type confusion, wild or dangling pointer, out of bounds), **V** = only a wrong / lost / duplicated JavaScript value,
**H** = heuristic or statistics only. "Reader"/"writer" are two mutators.

### 11.1 Arrays

| # | State / assumption | Race | Sev. |
| --- | --- | --- | --- |
| A1 | Kind of elements in byte 4 of the cell, contents in the butterfly; Int32→Double and Double→Contiguous rewrite the contents **in place** (`runtime/JSObject.cpp:1311-1336, 1381-1406`) | reader or writer that decided on the old kind touches the new contents: raw double read as JSValue, raw double stored into a Contiguous vector (2.5) | **M** |
| A2 | `Stretch` proofs `ArrayStorage`, `InBounds`, `Pristine` end only at writes of the same function (`aot/passes/AOTTypeTestEndings.cpp:31-47`); guarded accesses have no test at all (`AOTElementAccessEmitter.cpp:512-536`, `…Stores.cpp:215-226`) | any other thread converts, shrinks, freezes, reallocates or de-pristines the array inside the stretch | **M** |
| A3 | B3 may keep `butterfly`, `publicLength`, `vectorLength`, byte 4 in registers, or reload one and not the other, across everything that doesn't write their abstract heap | length of butterfly 1 used with butterfly 2 (after `reallocateAndShrinkButterfly`, `JSObject.cpp:4195-4216`, or a conversion to ArrayStorage) → out of bounds. The "one pointer gives bounds and elements" property (2.1) holds only if both come from the same load | **M** |
| A4 | `Dense` (no holes) is a fact of the type, read without a hole test (`AOTElementAccessEmitter.cpp:264-275, 464-489`) | several writers expose **transient holes below the length**: inline store beyond the length and `AppendElement` write the length first (`…Stores.cpp:177, 365`), `AppendElements` too (`AOTStubsArrayPush.cpp:238, 258`); `pop`, `length = n`, `shift` store the hole before they lower the length (`AOTArrayBuiltinEmitter.cpp:170-171, 213-214`, `AOTArrayStorageBuiltinEmitter.cpp:115-125`, `JSArray.cpp:1461-1470, 1524-1525`). On ARM64 even element-then-length can be seen reversed. The reader then unboxes an empty value (0) / PNaN under a proven type | **M** (null-like cell, unchecked unboxing; at best a crash) |
| A5 | Dense Int32: 32-bit store of the low half (`…Stores.cpp:195-206`) | slot meanwhile holds a double or a cell pointer (array converted by another thread) → half-overwritten pointer | **M** |
| A6 | `reallocArrayRightIfPossible` reallocs a PreciseAllocation butterfly when the collector is idle (`runtime/ButterflyInlines.h:237-246`; same test in `JSArray.cpp:68`) | old butterfly freed while another thread reads/writes through it | **M** (use after free) |
| A7 | `tryMoveElementsStartForward/Backward` write a new indexing header over former elements of the same allocation (`runtime/JSObjectElementsStartOffset.cpp:87-93, 122-129`) | holder of the old butterfly pointer reads header words as elements, or overwrites the new header with an element store (→ forged `vectorLength`) | **M** |
| A8 | Stub `growVector` publishes the new butterfly without a fence (`aot/stubs/AOTStubHelpersStorage.cpp:241`); C++ `setButterfly`/`nukeStructureAndSetButterfly` fence only on x86 or while `mutatorShouldBeFenced()` (`runtime/JSObjectInlines.h:110-133`) | ARM64: reader sees the pointer before header/elements: stale bytes of recycled auxiliary memory as lengths and JSValues | **M** |
| A9 | `setStructure` = 4 stores (ID, flags, type, CAS on byte 4) after the butterfly store (`runtime/JSCellInlines.h:270-282`) | new butterfly + old kind (e.g. ArrayStorage butterfly read as Contiguous); StructureID test (`branchUnlessHasOriginalArrayStructure`) and byte-4 reads (`loadHoleOf`) in one stub see different states | **M** |
| A10 | A nuked StructureID (bit `nukedStructureIDBit`) is visible in the cell between nuke and restore | another mutator decodes it (`structure()`), or a stub compares/probes with it. Stubs miss and go slow; C++ dereferences a wrong pointer | **M** |
| A11 | read-modify-write of `publicLength`: push/push, push/pop, `a[len] = v` | lost or duplicated elements, `length` off | V |
| A12 | copy to a new butterfly (growth, adding the 5th named property, COW → writable, Contiguous → Double) is not atomic with respect to element stores | stores into the old butterfly after its slot was copied are lost; two converters: one copy lost | V |
| A13 | in-place grow of `vectorLength` (`JSObject.cpp:4187-4190`, `AOTArrayOperations.cpp:335-338`) | ordered by `storeStoreFence`; reader without a load fence may see the larger vectorLength and unfilled slots (ARM64). Only writers read `vectorLength`, and they only store | V |
| A14 | `tryFreezeArrayInPlace` / `moveFrozenElementsToSparseMap` | writer that passed the writable-shape test before the freeze stores into the *old* butterfly: store lost (frozen copy unaffected) | V |
| A15 | Site statics filled lazily: `SharedValues` storage, template objects (`NewTemplateObject … boundTo = Site`) | two threads create two; for template objects identity is observable | V |
| A16 | `ArrayAllocationProfile`, last-array word, `ArrayWords` incl. the parameter-in-memory `placeThatConverts` | wrong counters; the *last array* word is a cell reference read by `reviewArrayAllocationProfile` (liveness rule not studied) | H (check the last-array word) |
| A17 | `haveABadTime` (secondary realms), `tryMarkPristine` at run time (`Structure.cpp:791-796`: rewrites `m_blob`, flags, bit field of live shared Structures) | allocation in another thread copies a half-updated header; conversions of all arrays need every mutator stopped | **M** for bad time, V for the mark |
| A18 | Frame arrays and `StackStringBuilder`s lie in a native frame | safe while nothing hands a frame pointer to another thread; a future "spawn with arguments" built-in must be an escape in `AOTFrameArrayParameters` | (rule) |

### 11.2 Ordinary objects and Structures

| # | State / assumption | Race | Sev. |
| --- | --- | --- | --- |
| O1 | (StructureID, butterfly[, maxOffset]) is multi-word; capacity lives in the Structure | reader: new StructureID (offset beyond the old capacity) + old butterfly. Excluded on x86 by store order + TSO if the reader loads ID then butterfly; not on ARM64 (no load ordering in stubs; C++ writer unfenced unless the collector marks) | **M** |
| O2 | two adders on one object | each runs nuke / butterfly / StructureID; the four stores interleave so that the surviving Structure has a larger capacity than the surviving butterfly | **M** |
| O3 | same, no growth | both take the same free offset for different names; last `setStructure` wins: one property lost, the other may show the wrong value | V |
| O4 | Property table stealing (`takePropertyTableOrCloneIfPinned`, `Structure.cpp:1257-1270`; pointer read before the lock) | two transitions out of one Structure share and mutate one table → wrong offsets in a Structure | **M** |
| O5 | mutator-side lookups read `PropertyTable` without `m_lock` (`Structure.h:1036-1051`; x86-64 stub `findInPropertyTable`) while `rehash` swaps and frees the index vector (`PropertyTable.h:548-582`) | use after free; size of one vector with the address of another | **M** |
| O6 | dictionary add: table entry first, butterfly second (`StructureInlines.h:437, 443`) | lock-free reader/replacer uses an offset that the current butterfly lacks | **M** |
| O7 | `flattenDictionaryStructure` renumbers offsets, moves values, may drop the butterfly (`Structure.cpp:1822-1881`) | stale offset → other property (V); null or shifted butterfly → **M** | **M** |
| O8 | transition table read without lock by mutators (`StructureInlines.h:573-577`, `Structure.cpp:1277`, `:774`) while `add` may turn the single slot into a `HashMap` or rehash it | torn tagged pointer / freed map | **M** |
| O9 | look up, miss, create, insert (`addPropertyTransition`, `nonPropertyTransitionSlow`, `findCachedPropertyAddition`, `addAlias`) | duplicate Structures for one shape: slower caches; AOT tables keyed by StructureID stay correct per ID | V/H |
| O10 | `Structure::m_bitField`, `m_outOfLineTypeFlags`, `m_blob`: non-atomic `|=` from many setters | lost flag (`hasReadOnlyOrGetterSetterProperties…`, `didTransition`, `isWatchingReplacement`, `staticPropertiesReified`, pristine bits) → checks that stubs make on the bit field (`AOTStubsPropertyRead.cpp:412, 465, 497`) pass wrongly: a read-only or watched property is overwritten | V (semantics; can invalidate what caches assume) |
| O11 | `allocateRareData` and every `StructureRareData` cache: check-then-act | two rare datas, one lost with what was cached in it (incl. `m_immutablePropertyNames` if set through the loser!) | V, **locks of built-ins could be lost** |
| O12 | `vm.propertyRuns` (`HashSet`, `HashMap`, `FastBitVector`), `PropertyAdditionCache` | concurrent insert/rehash | **M** |
| O13 | a **read** can write: static property reification, lazy `ClassPrototype::createMembers`, `SlimClosure` reification, `Object::extraPropertiesSlow` → `makePendingStars`, `JSFunction` lazy `prototype/name/length` | two readers both materialise; other readers see the half-made object | **M**/V (see O2, section 10) |
| O14 | `vm.allowLazyMaterializationOfImmutablePropertiesCount`: per-VM counter as a dynamic scope | other threads' writes to locked built-ins pass while it is non-zero; `++/--` not atomic | V (breaks "built-ins can't be replaced", which compiled code relies on without looking) |
| O15 | `WatchedLocationState` byte, `willWrite` then write | compiled reader: test byte, use constant. Stale by one write: linearisable | V |
| O16 | `growOutOfLineStorage` stub stores only the StructureID and assumes flags/type/indexing byte stay | holds per transition; races as O2 | **M** |
| O17 | `missesOfPairs` counters, `missIsDueForFiling` | lost increments | H |

### 11.3 Map / Set / WeakMap

| # | State / assumption | Race | Sev. |
| --- | --- | --- | --- |
| C1 | `becomeObsolete` / stub growth reuse the old table's header and index **before** `owner.m_storage` is switched (`AOTCompactTable.cpp:392-412, 570-574`; `AOTStubsCollections.cpp:789-792`); fast paths never test `m_status` | reader probes a clobbered table: bogus `m_used`, `m_capacity`, index → out-of-bounds entry → garbage decoded to a cell pointer; writer appends into it and overwrites the successor pointer | **M** |
| C2 | `removeAllEntries` (clear in place) wipes index and counts (`:687-698`) assuming "nobody stands behind an entry" (`wasWalked`) | concurrent reader: absent (V). Concurrent appender: `m_used` written back from a stale value → entries with stale keys revive; iterator of another thread that has not yet set `wasWalked` | V |
| C3 | append: key, index, value, (conditional fence), `m_used`, `m_alive` | ARM64 or slot-kind index: reader finds the entry before its value → returns the **empty value**; key column read as 0 = "deleted" | **M** (empty value under a proven type) / V |
| C4 | two appenders | same entry number used twice; `m_alive += 1` in memory is not atomic; index holds two slots for one entry | V (lost key, wrong `size`) |
| C5 | widening (`tryCopyWithWidths`): narrow → wide columns is a *new* table; `m_valueWidth`, `m_keysShiftAndFlags` are read separately from the columns | only with C1 | **M** via C1 |
| C6 | `wasWalked`: load8 / or / store8 on a byte that also holds `keysAreWide`, `keysAreInImage`, shift | those other bits are constant for a live table, so a lost `wasWalked` is the only effect → `clear()` in place under a walker → walker skips/repeats | V |
| C7 | first insertion: `m_storage == null` → create → store (`JSOrderedHashTable.h:109-114`, stub `NewCompactTable`) | two tables, one lost with its entry | V |
| C8 | in-image table → heap (`tryMoveToHeap`) | as C7 | V |
| C9 | per-VM `compactTableIterationEntry` (`AOTCompactStorage.cpp:91-99`) | two threads iterating through the C++/interpreter intrinsics swap positions: entry number of one table used in another; `keyAt(entry)` is not bounds-checked beyond `RELEASE_ASSERT(isLive())` paths (not fully read) | **M**? |
| C10 | iterator objects (`JSMapIterator` internal fields `entry`, `storage`) shared between threads | two-field update torn: entry of the old table with the new table | V (entries are re-validated against `m_used` in `next`) |
| C11 | WeakMap `m_buffer`/`m_capacity` two loads; `rehash` frees the old buffer at once (`WeakMapImplInlines.h:130-172`); also at the end of a collection | use after free, out of bounds | **M** |
| C12 | hashing a key string writes `m_hashAndFlags` (leaf `operationAOTHashStringImpl`) | see S5 | |

### 11.4 Strings

| # | State / assumption | Race | Sev. |
| --- | --- | --- | --- |
| S1 | rope resolution re-reads `m_fiber` after the `isRope()` test (`JSString.cpp:338-372`, `JSString.h:747-750`) | second resolver takes a `StringImpl*` for fiber 0 / for flag bits | **M** |
| S2 | `convertToNonRope` placement-news over the word | two winners: one `StringImpl` leaked (never freed: safe for readers) | leak |
| S3 | `swapToAtomString`: old impl parked in `Heap::m_possiblyAccessedStrings…` (`SegmentedVector`, unlocked; cleared when "no JS is executing", pruned by what conservative roots find) | concurrent appends corrupt the vector (**M**); "no JS executing" and the root scan must cover every mutator or the old impl is freed under an opened `FlatString` (**M**) | **M** |
| S4 | rope view: characters from the stub, width flag from a second load of the impl (`AOTStringEmitter.cpp:190-199`) | atomisation to an impl of the other width between the two → 8-bit characters read as 16-bit: out-of-bounds read | **M** |
| S5 | `m_hashAndFlags` plain; `setHash` vs `setIsAtom` (and `didReportCost`) | lost atom bit → dangling entry in the atom table, or a non-atom treated as an atom (pointer-compared names) | **M** |
| S6 | one unlocked `AtomStringTable` per VM, installed per thread by `JSLock` | concurrent `add`/`remove`/rehash | **M** |
| S7 | `AtomStringImpl::remove` uses the *current thread's* table | last `deref` on a thread with another table: `RELEASE_ASSERT` crash | crash |
| S8 | add = find in table, find in image, add | two threads atomise the same new text → with a lock fine; without: two atoms for one text → property lookups by pointer fail | V→**M** with S6 |
| S9 | `deref()` relaxed, with a non-atomic fast path for refcount 1 | last use on thread A not ordered before the free on thread B | **M** (theoretical on ARM64) |
| S10 | `markAsAtom` / `setPerCellBit`: `m_flags |= 0x80` on byte 6 of a JSString | only that bit ever changes in a string's flags | benign |
| S11 | `tryCreateHeader` bump pointer; `m_literalStringCacheKeys` (`RefPtr` array); `immortalCells->add`; constant cell words | same header handed out twice (**M**); `RefPtr` slot overwritten by two threads (double deref, **M**); duplicate constants (V, invisible for strings) | **M** |
| S12 | string builder pool pop/push (`AOTStringBuilderEmitter.cpp:241-253`, `AOTStringBuilderStorage.cpp:89-126`) | one builder given to two activations | **M** |
| S13 | `vm.numericStrings` read by stubs: key word, then JSString word | entry replaced between the loads: string of another number | V (cells are kept alive by the cache's own visit; clearing at GC not re-read) |
| S14 | `JSONWords` open source per Instance, ownership by stack-pointer order | two parsers; stack addresses of different threads are compared | **M** |
| S15 | `equalResolvingRopes` mutates both operands of `===` | S1/S2 from a "read-only" operation | **M** via S1 |

---------------------------------------------------------------------------------------------------------------------------------------

## 12. What helps

1. **No code is patched, nothing is jettisoned, nothing is profiled for a later tier.** Every fast path is a fixed sequence that can be audited once. The list of sequences is short and centralised: `ElementAccessEmitter::{indexingMode, elementsOf, publicLength, vectorLength, setPublicLength, loadRawElement, storeRawElement}`, stubs `ArrayGetAtIndex / ArrayPutAtIndex / ArrayPush / AppendElements / growVector / growOutOfLineStorage / emitFindInHashTable / emitAppendToCompactTable / GrowCompactTable / emitLookupInWeakTable / findInPropertyTable`, `StringEmitter::viewOf*`. A new protocol is a change in about a dozen functions.
2. **The compiler sees the whole program.** It can know whether a program ever starts a thread, and emit today's sequences when it doesn't. The flow analysis already tracks allocation sites to every position, which is the raw material for "this array never reaches another thread".
3. **Bounds travel with the elements** (`IndexingHeader` in front of element 0): an access that takes length and element from one loaded butterfly pointer can't leave that allocation. Old butterflies are GC memory and a butterfly pointer in a register or stack slot keeps its allocation alive (`heap/ConservativeRoots.cpp:79-87, 162-169` handle butterfly pointers, also those just past the end), provided every mutator's stack and registers are scanned. The exception is the one `realloc` path (A6). Storage born inside the array's own cell lives as long as the array.
4. **Type-level facts are thread-agnostic.** `ClassPart.arrayStorage`, `contentsAreProven`, `Dense`, `AsCreated` come from "flow sees every writer". No interleaving adds a writer. An array whose storage kind is *proven* never converts, so A1/A5/A9 concern only arrays with tested (stretch) kinds. What threads break is (a) stretch proofs and (b) transient states of the writers' own sequences (A4), and (b) is fixable by ordering element-before-length on growth and length-before-hole on shrink.
5. **Inline writers compare the whole indexing byte**, lock bits included (`AOTElementAccessEmitter.cpp:169`): taking `cellLock()` on an array already sends every inline and stub writer to C++. That is a ready-made way to fence writers out during a conversion (readers mask the bits away and would need the same).
6. **Built-in prototypes are locked**: no indexed accessors, no shadowing, no prototype swaps on `Array.prototype`, `Object.prototype` etc. in the main realm. Holes read as `undefined` without a chain walk, `haveABadTime` can't happen there, `array.push` needs no lookup. The lists (`ImmutablePropertyNames`, built-ins table) are immutable data.
7. **The pristine bit is conservative.** No transition turns it on (`pristineBitsAfterTransition` only keeps or clears). `tryMarkPristine` marks Structures later; a cell made earlier stays unmarked until its next `setStructure` copies the flags (`TypeInfo::mergeInlineTypeFlags`, `runtime/JSTypeInfo.h:142-145`). A stale "clear" is harmless, and a stale "set" is a linearisable read of the moment before the change. Only the *stretch* that compiled code hangs on the test is the problem (A2).
8. **Locks and lock-taking readers already exist** for the compiler threads: `Structure::m_lock` (a real lock in all builds), `getConcurrently`, `forEachPropertyConcurrently`, `addPropertyTransitionToExistingStructureConcurrently`, `StructureCache::m_lock`, `cellLock()`, `SymbolTable::m_lock`. All *writers* of transition tables and property tables already hold `m_lock`. What is missing is the reader side on mutators, and deferred freeing of index vectors.
9. **The nuke protocol** gives a seqlock-like way to read (StructureID, butterfly) consistently, proven in a comment for one concurrent reader (`JSObjectVisitInlines.h:126-200`); the stub version is unconditional and fenced (`AOTStubHelpersStorage.cpp:134-138`). It does not handle two writers.
10. **CompactTable is append-only**: entries never move in a live table, deletion is an in-place tombstone, `m_used` is published after the entry (with a fence hook already at the right place), growth makes a new table and leaves a forwarding chain that iterators follow. Making the old table stay intact (separate forwarding word, status tested by fast paths or storage switched first) would make lock-free readers plausible. In-image tables are immutable.
11. **Strings**: contents immutable; `StringImpl` refcount already atomic; static strings (all names and literals of the program) are read-only, pre-hashed, never counted; image atoms have process-wide identity without any table; rope resolution publishes with one fenced word store and never clears fibers; atomisation parks the old impl. One CAS on `m_fiber` (loser drops its impl) and single-read discipline in `resolveRope` would close S1/S2.
12. **Per-activation scratch is already on the stack** in many places: frame arrays, `StackStringBuilder`, argument buffers.
13. **`--aot-validate`** re-checks every proof at its use (`StorageGuardValidator`, `checkPrecondition`): with threads it is a detector for exactly the A2/A4 class.
14. **The effect system names the shared state**: heaps `ArrayStorage`, `Elements`, `CollectionStorage`, `CollectionEntries`, `StringStorage`, `StringBuilderContents`, `ParserState`, `RegExpState`, `RuntimeState` (`AOTSchema.def:55-79`), with attributes `perObject`, `ordered`, `observable`. "Another thread may write heap H of an escaped object" can be expressed there, and `ProofLifetime` already separates what survives from what doesn't.
15. A **closed program** (`AdmitsUnknownJavaScript` clear) never runs LLInt for program text (section 8), which removes bytecode metadata, CodeCache and parser state from the problem for such programs.

---------------------------------------------------------------------------------------------------------------------------------------

## 13. Not found / not sure

### 13.1 Sections 1 to 6

* Whether B3 under `aot/codegen/AOTB3Policy.cpp` really hoists/CSEs butterfly, length and indexing-byte loads across calls and loop back edges was inferred from the abstract heaps, not seen in generated code. (Section 7.2.7 reaches the same conclusion for typed arrays, also by inference.)
* What exactly happens when a `Dense`/proven read yields the empty value or an impure NaN: `ConversionEmitter::unbox`, `boxNumber` were not read. Severity of A4/C3 is stated as M on the ground that no check exists; R1 should say what a 0 in a `Cell`/`Cell32` position does (fault page → TypeError, or cage base).
* How flow proves `Dense` (which writers it accepts, e.g. `a[a.length] = v`, `length = n` growth) was not read.
* The middle of `GrowCompactTable` (`AOTStubsCollections.cpp:672-785`), `emitSearchCompactTable`, `copyWithAbsentKey`, `tryCopyWithWidths`, `AOTCompactTableLookup.cpp`, `AOTCompactTableCreation.cpp` (beyond function heads), `AOTTableInImage.*` were skimmed only.
* `CollectionIntrinsicEmitter::generateNext` (inline iteration) was not read line by line; whether it re-validates `entry < m_used` against the same table load was taken from the stub.
* `AOTStubsElements.cpp` and `AOTStubsKeyedAccess.cpp` were read by their comments and selected functions, not completely. `AOTStubsArrayHoles.cpp`, `AOTStubsArraySlice.cpp`, `AOTStubsArrayLiterals.cpp` (except `ArrayOfData`), `AOTElementEmitterAllocation.cpp`: allocation only touches new cells; not audited.
* `ArrayAllocationProfile`'s "last array" word: who keeps that array alive, and when the word is cleared.
* ARM64: `answerFromObjectsAndReturn`/`findInPropertyTable` are `#if CPU(X86_64)`; on ARM64 the same misses go to `operationAOTGetByValGeneric` etc. (`aot/runtime/AOTPropertyCachesLeafOperations.cpp`), "leaf" C functions that read Structures without materialising (`Structure::getWithoutMaterializing`); not read.
* Lazy string headers of position-independent images (`lazyStrings.*`, `LazyStringHeader`): how and when a header is filled, and whether that is a run-time write to shared memory.
* `ProgramIdentifiers::uid()` for symbols (lazy creation, what its `Lock` guards).
* Whether a `StringBuilderStorage` can be live across `await`/`yield`.
* Where JSStrings are destroyed (sweep on which thread, with which atom table installed) in this fork.
* `runtime/JSONObject.cpp` (+1,042), `AOTJSON*` beyond `JSONWords` and the object-making lines; `AOTJSONKeyOrder*` caches.
* `JSObject.cpp`'s diff is 896 lines; only the array/storage functions quoted above were read. `Structure.cpp`'s diff (856 lines): `dropTransitionChain`, `restoreTransitionChain`, `createWithPropertyAdditions`, `addPropertyRunTransition`, `makeListedPropertiesImmutableTransition` were not read line by line.
* `SparseArrayValueMap`, ArrayStorage shift/unshift: unchanged upstream, not re-read; always C++.
* `DirectArguments` slots are read/written by compiled code only through operations (`GetMappedArgument`), as far as `AOTArgumentsOperations.cpp` shows; inline forms in `AOTArgumentsEmitter.cpp` were not read.

### 13.2 Sections 7 to 10

Each carries its own "Not found / not sure".

### 13.3 How far the helper sections were checked

The author re-read the cited lines for these claims and found them as stated: `VMState::regExpMatchOutput` is one array per VM (`aot/runtime/AOTVMState.h:75`, `.cpp:65`); `RegExp::compile` writes `m_state = ByteCode` before `m_regExpBytecode` (`runtime/RegExp.cpp:574, 597-598`); `ArrayBuffer : GCIncomingRefCounted : DeferrableRefCounted` with a plain `m_refCount += / -=` (`runtime/ArrayBuffer.h:249`, `heap/GCIncomingRefCounted.h:43`, `WTF/DeferrableRefCounted.h:48, 78`); `Atomics.wait` uses `vm.syncWaiter()` (`runtime/WaiterListManager.cpp:120`); the handler swap and the list of 25 replaced opcodes plus the virtual call trampoline (`llint/LLIntData.cpp:75-84`, `llint/LowLevelInterpreterAOT.asm:1788-1825`); the per-realm `evalCallee` with `setScope(scope)` / `setScope(nullptr)` around each eval (`interpreter/Interpreter.cpp:2150-2198`); `LazyProperty::callFunc` returning null while `initializingTag` is set and setting the tag with a plain `|=` (`runtime/LazyPropertyInlines.h:95-108`); `m_deletePropertyMode` as a per-VM scope (`runtime/VM.h:783-797, 1453`); the megamorphic cache being created only under `Options::useJIT()` (`runtime/VM.cpp:457-460`). Everything else in 7 to 10 is as the helpers reported it. One tension with the pull request text: it says the AOT handlers "write no profiles, since no JIT will read them"; section 8.3 finds that this holds for value profiles on the replaced handlers' own paths only: the ordinary handlers and all slow paths are unchanged, `useLLIntICs` stays on, and the LLInt inline caches in bytecode metadata (and the array allocation profile word) are still written.
