# 5. Objects with a layout, closures, environments, classes

The object model of compiled programs under threads. The present model, word by word and write by write, is in `research/R1-object-model.md`; section and hazard numbers (`R1 5.1`, `R1 H4`) refer to it.

Everything here concerns objects of a **Shareable** layout (04, section 4.2). For a Confined layout the compiler, and the run time **as far as it reads and writes the object itself** (cell, Extension, side object and their storage), do what they do today. The run time tells the two apart by a bit of the layout record
(`LayoutData::impossibleMutations()` contains `ReachOtherThread`): a property of the data, not a mode.

Confined is a fact about objects, not about the class: ten threads may each allocate instances of one Confined layout. Whatever several objects share follows this document, 06, section 6.5, and 07 **whatever the layout**: Structures, their variants and their caches (5.13), key orders, class and closure tables, per-layout records.

## 5.1 What is already right

| Fact | Source |
| --- | --- |
| The shape word never changes. Offset, width, representation and "is this word a reference" are functions of it | `aot/analysis/AOTGuardFacts.cpp:48`; R1 4.1 |
| A slot is 1, 2, 4 or 8 bytes, aligned to its width: each access is single-copy atomic on x86-64 and ARM64 | `aot/place/AOTPlacementSlotCheck.cpp:254-255` |
| "Empty" is a bit pattern in the slot itself | `aot/object/AOTSlotRepresentation.h:55-79` |
| What a slot may hold is a fact about all stores of the program | the verifier "checks every operand, argument and stored value against the type of its place" |
| An object never converts to another representation. The worst case costs one slot and a side object | R1, "What helps" 2 |
| The collector reads neither shape word nor order word. It goes StructureID, layout, read-only reference table, and all Structures of one object share the layout | `aot/object/AOTObject.h:472-477` |
| Markers of references decode into memory that is never allocated | `heap/MarkedBlockCage.h`: `minIndex = 1 << 21` |

So *shape test, load, use at the proven type* needs no change, and racing plain stores to a field that never leaves its slot need none either.

## 5.2 Invariants

| # | Invariant |
| --- | --- |
| **CO1** | The shape word of a cell is written once, before the cell is published |
| **CO2** | The bits of a slot are at all times a value of the field's proven type in the slot's representation, or one of the representation's markers |
| **CO3** | In a published object `absent` is written into a slot only under the object lock, by an atomic exchange or compare-and-swap. No fast path overwrites an `absent` **that a mutation of 5.6 has written**: not compiled code, not a stub, not a handler of the interpreter, not an atomic operation (04, sections 4.6.2, 4.6.6). A field that no mutation can empty (first row of 04, section 4.6.2) may be created by a plain store where the layout lists no keys |
| **CO4** | A slot that holds a value is the truth about its property. A copy of the name among the extra properties is ignored while the slot is filled |
| **CO5** | The header word (StructureID, lock byte, type, flags, cell state) and the order word of a published object are changed only under the object lock. Exceptions: the cell state (collector's protocol, a byte store) and the lock bits themselves |
| **CO6** | The Extension of an object is allocated once and never replaced, moved or repacked. It is published by a compare-and-swap (release). Then the publishing thread runs the owner barrier, before its next safepoint |
| **CO7** | The word of the extra properties goes from "none", perhaps through one tuple of pending stars (5.13), to one side object, and then never changes again. In a layout without that word the same holds of the side object that the object's Structure variant holds: every later variant of the object holds the same one |
| **CO8** | The bits `HasFieldOutsideSlot` (27), `HasExtraProperties` (28), the integrity bits (29 to 31) of the order word, and `IsPristine` (= has another prototype) of the flags byte are set and never cleared |
| **CO9** | Whoever sees a StructureID of an object (by a load-acquire) sees everything that was stored into the object and its side object before that StructureID was stored |
| **CO10** | An operation that finds another state than its caller saw has lost a race and starts again. It does not abort (5.6) |
| **CO11 to CO13** | The variable of a static field is a slot: the counterparts of CO2 to CO4 (5.14) |
| **CO14** | An object is filled by words only while it is unpublished (5.12) |
| **CO15** | The mark of the cell (`TypeInfoPerCellBit`) of an object of a Shareable layout means nothing. Nobody reads it and nobody writes it after the cell's header has been copied from the allocation site's record: not compiled code, not a stub, not the run time |
| **CO16** | A walk over listed fields treats an empty slot as a `delete` that has won (5.6 c) |

CO1, CO2, CO8 hold today. The others are what this document adds.

Why CO15. One bit of the header vouches today for the contents of several slots, and `HasNoFieldOutsideSlot` is as a rule a test of that bit (`flagsTestedBy`, `aot/codegen/AOTGuardEmitter.cpp:174-186`). A bit and a slot are two words, and no order of the two writes helps a reader that tests first and loads later. So each read tests its own load (04, C3), `HasNoFieldOutsideSlot` tests bit 27 of the order word, `FinishBirths` skips the layout, `markCellIfMarkerReplacesRawValue`, `willStoreMarker`, `tryFinishBirth` and the copies of 5.12 leave the bit alone.

## 5.3 The object lock

The two lock bits of byte 4 of the header (`IndexingTypeLockIsHeld = 0x40`, `IndexingTypeLockHasParked = 0x80`, `runtime/IndexingType.h:97-98`): JSC's `JSCellLock`. In an `AOT::Object` the rest of the byte is unused.

```cpp
class ObjectLocker {            // runtime/threads/ObjectLocker.h
    ObjectLocker(VM&, JSCell*); // NoSafepointScope, then cellLock().lock()
    ~ObjectLocker();            // unlock, end of scope
};
```

| # | Rule |
| --- | --- |
| **L1** | A thread that holds an object lock does not park, does not run JavaScript, and does not wait for anything but a lock that is lower in the order of L3. `NoSafepointScope` makes `stopIfNecessary()` a no-op for this thread and defers collection as `DeferGCForAWhile` does; a debug build asserts that no poll, no call of compiled code and no blocking primitive runs inside. In particular it waits for no claim and does not conduct (08, S4) |
| **L2** | Allocation under the lock is allowed (it cannot collect), in front of the point of no return (L6). A slow path that may need much memory (a new Structure, a large side storage) allocates first, locks, checks that what it planned from is unchanged, and starts over if not |
| **L3** | Order: prototype-chain lock (5.7), then the object, then its side object, then table locks of the run time (07, section 7.7), then heap locks. Never two unrelated objects at once. All of them are leaf locks in the sense of 08, H15: the holder is in a `NoSafepointScope`, and waiters keep heap access |
| **L4** | The collector's visitors do not take the lock of an `AOT::Object`, `Closure`, `SlimClosure` or `Environment`. (They do not today.) Because of L1 a visitor that does take a cell lock, of another class, never finds it held by a parked thread |
| **L5** | A thread that waits for an object lock keeps heap access. By L1 the holder is running, so the wait is short, and a stop that is requested meanwhile completes after both have gone on to their next safepoint |
| **L6 (point of no return)** | Each mutation of 5.6 has a store behind which the object is in a state that only the end of the mutation makes whole again: as a rule the store that empties a slot. Behind it the mutation does not allocate and cannot fail, and it waits for nothing but the successor of storage that it finds `Retired`, which a running thread publishes within a few instructions (06, section 6.3.2). **Everything that can fail lies in front of it.** What can be made aside (Structures, the side object, new storage with a copy, a closure that a slot still defers) is made before the lock. The place that the property moves to is entered in front of the point of no return too, under the lock, where CO4 or CO13 hides it: the property stands in the side object before the slot is emptied. Whether storage is `Owned` by this thread is decided by the compare-and-swap that retires it, not by a look at it, and that compare-and-swap lies in front of the point of no return. If it fails, or if the preparation sees at once that storage has to be replaced which this thread does not own (06, section 6.2), the **whole mutation** runs as the closure of a safepoint operation (08, section 8.6), without the lock |

Why L6. Storage in state `Shared` is replaced only while every thread is at a safepoint (06, SW5), and a holder of an object lock does not park (L1). The side object's storage becomes `Shared` as soon as a second thread stores into an existing out-of-line extra property, which takes no lock and can happen at any moment, also between the mover's check and its retirement. A free slot cannot be kept in reserve either: the out-of-line capacity is a function of the side object's Structure, and storage of another capacity is installed together with that Structure (06, SW12). A mover that found out with the slot already empty that it cannot replace the storage could neither go on nor let go: a reader that gets the lock would find `absent` in the slot and nothing in the side object. The same holds for an allocation that fails, since nothing can be collected under the lock.

A closure of a safepoint operation validates what it was prepared from and reports failure if anything differs (08, section 8.6); the requester then starts again from the generic dispatch.

A mutation that is reached from an operation without the effect `waits` (04, S5) cannot run a safepoint operation. It returns `MustRetryAtSafepoint` to the frame that opened the scope (08, S4). The stores by name, `defineProperty`, `delete`, `freeze`, `setPrototypeOf` and `Object.assign` have the effect.

**Who takes it.** Every C++ function of `aot/object/` and `aot/runtime/` that changes anything of a published object other than the value in a filled slot. In stubs: `lockObject(cell, scratch)` = load byte 4, `or 0x40`, compare-and-swap (acquire), on failure call the operation; `unlockObject` = compare-and-swap back (release), or the operation if the parked bit is set.

**Who does not.** Reads. Stores of values into filled slots. Everything that touches only an unpublished object, or an object of a Confined layout, itself.

An uncontended lock and unlock are two atomic instructions, about 40 cycles. They are paid on paths that today run 50 to several hundred instructions.

## 5.4 Slots

### Reads

| Slot | Compiled code (04, section 4.6.1) | Generic stubs and C++ |
| --- | --- | --- |
| no markers (flow proves always filled; 04, rule O2) | one load | one load |
| markers, in line | `TryGetSlot`: one load, test the loaded word | the same (`readSlot` already decodes one loaded word) |
| markers, read as "the value or `undefined`" | `GetSlotOrUndefined` if the field cannot be moved out, else `TryGetSlotOrUndefined`: on `absent` it looks at bit 27 | the generic read below |
| an accessor in its slot | `TryGetSlotGetter` | load the word once, test it, then read the `GetterSetter` |
| a deferred closure | `TryGetDeferredClosureSlot` | `materializeDeferredClosure`, below |
| a deferred closure, read as "the value or `undefined`" | `TryGetDeferredClosureSlotOrUndefined`: on `absent`, also as the operation's answer, it looks at bit 27 | the same, then the generic read below |
| in the Extension | 5.5 | |

No reader takes a marker for a value, because no reader relies on an earlier test of the slot (04, C3).

**A generic read of a declared field that finds `absent`** has three answers to choose from. It loads the slot (acquire on ARM64), then the order word.

| Order word, field | Answer |
| --- | --- |
| bit 27 is set | the property may live among the extra properties. Take the object lock and look again, first at the slot, then at the side object |
| bit 27 is clear, the field has `fieldMayExistWithoutStorage` | the key order decides. A field that the object lists reads as `undefined` and is present, whether or not it has storage: "Absent bytes do not say that the field is absent" (`aot/codegen/AOTSlotEmitter.cpp:441`; `Object::read`, `aot/object/AOTObjectStorage.cpp:108-118`). Reading the key order: 5.6 (g) |
| else | the object has no such property |

A mover sets bit 27 before it empties the slot (5.6 a), and while bit 27 is clear no extra property has the name of a field (`aot/object/AOTObjectKeyOrder.cpp:188`). So bit 28 need not be looked at for a declared field.
Per-thread caches may answer "absent" for `(StructureID, name)` where the Structure is a variant without extra properties: by CO9 and the order of 5.6 that is the same test.

### Stores into a filled slot of a published object

| Who | A field that `canLeaveSlot()` and whose stability flow does not prove | Any other field |
| --- | --- | --- |
| compiled code | `TryPutSlot` (04, section 4.6.2) | `PutSlot`: plain store, release for a reference on ARM64 |
| stubs that store by name: `putKnownField`, `storeField`, `storeFieldByTable` (`aot/stubs/AOTStubsKnownFieldStores.cpp:186-407`, `aot/stubs/AOTStubsFieldStores.cpp:438-466`) | they test the slot's presence and dispatch on the representation at run time. Each arm ends in a compare-and-swap of its width from **the word that the presence test loaded**; on failure the stub tests again. The stub reads "Shareable" and `canLeaveSlot` from the records it has in hand | plain store, release for a reference |
| C++: `Object::tryStore`, `tryWriteSlot` | `compareExchange` from the loaded word that is not `absent` | plain (release) store |
| handlers of the interpreter | as the stubs (10, section 10.4) | |
| `Atomics`, in line and in C++ | one protocol on the slot's word: a compare-and-swap from the word that the result was computed from (04, section 4.6.6). `tryStore` is not used for a read-modify-write | the same |

That a stub has found its way through a per-thread cache changes nothing: 07, K1, speaks of probes and fills, not of the access that follows a hit.

### Deferred closures

A slot of representation `DeferredClosure` has more states than "not made" and "made": 0 "not made yet", 1 `absent`, the markers of `undefined` and `null`, a reference (`aot/object/AOTSlotRepresentation.h:55-79`; `aot/object/AOTSlotAccess.cpp:169-178`). `Object::materializeDeferredClosure` (`aot/object/AOTDeferredClosures.cpp:78-104`):

```
again:  w = load(slot)
        w is a reference, or the marker of undefined or null:  that is the value
        w == 1:   empty                                             // the caller has an edge or a branch for this
        w == 0:   v = create, outside any lock
                  compareExchange(slot, 0, encode(v)) (release); barrier;  success: v
                  failure: drop v; goto again                       // 07, Z1
```

The loser does not "use what is there" unseen: what is there may be `absent`, or a value that the program has stored. A store of a value replaces 0 like any value (0 is not `absent`). A compiled store can put 0 back ("the next read of the slot makes the closure", `aot/codegen/AOTSlotAccessEmitter.cpp:555-563`); on a published object it follows the table above.

### Widths

Compare-and-swap exists for 8, 16, 32 and 64 bits on both targets. `OptionalBoolean` is the only representation below 4 bytes that has markers; the 2-byte representations have none, so they are stored plainly. A `Double` slot is swapped as 64 raw bits; NaN is purified before, as today. A `LiteralString32` word is compared and swapped like any other: a read does not write a made cell back into the slot.

## 5.5 The Extension

Today (R1 2): a word of granule bits and the held granules packed in ascending order, address by popcount; grows by appending in place, by **moving granules in place** while the collector is not marking, or by replacement. With a second mutator each of the three is unsafe (R1 H4), and replacement also loses a store that another thread makes to a *different* field, which 02 forbids even for
programs without races.

**A Shareable layout has an unpacked Extension.**

```
Extension of a Shareable layout:   uint64 granuleBits (all granules of the layout, constant) | granule 0 | granule 1 | .. | granule n-1
address of slot at offset o   =    ext + o                                   (o counts every granule, as today)
```

- Allocated at the first need, with room for every out-of-line granule of the layout, zeroed. Zero is `absent` in every representation that may lie out of line (`aot/place/AOTSlotRepresentations.cpp:121-125`).
- Published by one compare-and-swap of the hidden word from 0 (release). The loser drops its allocation and uses the winner's. No lock.
- Then `writeBarrier(owner)`, as `setExtension` runs it today, before the publishing thread's next safepoint (CO6). The owner may be old and black and the Extension young: without the barrier the next Eden collection would not visit the owner, would not mark the Extension, and would sweep it. A collection needs the winner at a safepoint, so a loser that stores a number, and runs no barrier of its own, is covered by the winner's.
- Never replaced (CO6). So no store can be lost, no granule can move, no bit can be lost, and a `PutSlot` "that knows its granule is held" needs to know only that the Extension exists. `Object::setExtension` on an object that has an Extension is a release assertion for an unpacked layout.
- Address computation is an addition instead of a popcount chain: fewer instructions than today.
- The collector's three granule sets apply to a constant bit word. "Read once" stays trivially right.
- `ExtensionLayoutData::isUnpacked`, set by `PlanSlots` for Shareable layouts. Readers of the flag: `SlotEmitter::addressInExtension` (`aot/codegen/AOTSlotEmitter.cpp:330-344`), `computeFieldAddress` (`aot/stubs/AOTStubsFieldReads.cpp:92-128`), `Object::slotAddress` (`aot/object/AOTObject.h:346-350`), `ensureExtensionGranules`, the allocation stubs (`aot/stubs/AOTStubsAllocation.cpp:310-520`), the visitor (`aot/object/AOTObjectVisiting.h:82-150`), object copies.

**Cost.** Memory: an object of such a layout that gets one optional out-of-line field pays for all of them. `PlanSlots` therefore bounds the out-of-line part of a Shareable layout (new `LimitID::MaxUnpackedExtensionGranules`, 16): fields beyond the bound are placed in line if the cell's size class allows, else they are extra properties from the start.

**Read in compiled code:** `LoadExtension` (hidden word, 32 or 64 bits; with narrow references 0 decodes to the page of zeros, so no test), then `TryGetSlot` at `ext + o`. **Write:** the `Storage` comes from `EnsureExtension` (test, allocate, compare-and-swap, barrier), then `PutSlot` or `TryPutSlot` as in 5.4 (04, section 4.6.5). On the page of zeros a read finds `absent`; a store never takes its address from `LoadExtension`.

The address is a `window` value although the Extension is never replaced: the collector marks an Extension that it finds from a stack but does not trace it, so a pointer held without its owner must not outlive a safepoint.

**`fieldMayExistWithoutStorage`** (R1 1.3 e) needs no new case. The source decides by the key order, not by the storage: `Object::read` answers `undefined` for a listed field whose slot is missing **or holds absent bytes**, and the inline test calls an operation on absent bytes (`SlotEmitter::isFilled`, `aot/codegen/AOTSlotEmitter.cpp:436-449`). A granule holds two 4-byte slots, so "storage with absent bytes for a listed field" exists today. In an unpacked Extension it is merely more frequent. A store of `undefined` into such a field writes the marker if the Extension exists and nothing if it does not (`Object::store`, `aot/object/AOTObjectStorage.cpp:150-157`); both read the same.

## 5.6 Mutations of a published object

Each runs under the object lock from first to last step, or, by L6, as the closure of a safepoint operation. "Prepare" steps may allocate and run before the lock (L2). The order of the stores is what lock-free readers rely on.

### (a) A store that the slot cannot hold; redefinition (`moveFieldToExtraProperties`, R1 5.1, 5.3)

| Step | Write | Why in this place |
| --- | --- | --- |
| prepare | the side object if there is none; the Structure variants of the object that will be needed; the side object's next Structure; if that Structure has a larger out-of-line capacity, new storage for it with a copy of the old (06, section 6.4, "prepare"); for a redefinition the `GetterSetter`; for a `DeferredClosure` slot whose value is wanted, a closure, made aside as in 5.4. If the side object's storage has to be replaced and is not `Owned(me)`: safepoint operation | L2, L6 |
| lock, check | the object's StructureID, the side object's StructureID and its storage pointer are those that were planned from. Else unlock and start over | L2 |
| 1 | `ensureExtraProperties`: hidden word (release), bit 28 (release), StructureID of the variant with extra properties (release). **In a layout that has no hidden word for the extra properties** the side object hangs on the Structure variant (`CompiledStructure::heldExtraProperties`, `aot/object/AOTCompiledStructure.h:108-111`), and the order is: StructureID of the variant that holds it (release), then bit 28 (release) | a reader that sees bit 28 finds the side object; a reader that later finds `absent` finds the bits. After this step the side object is empty, so a thread that sees the new StructureID and a clear bit 28 answers as before |
| 2 | bit 27 | the same |
| 3 | list the field in the key order if it is not | |
| 4 | enter the property into the side object, by steps 1 to 3 of 06, section 6.4: retire the old storage by compare-and-swap if the capacity grows, nuke, publish; the value (release); the side object's StructureID (release). The property has its final attributes. Its value is the value that did not fit, or the accessor, or, where the slot's last value is wanted, the placeholder `undefined` | **the last step that can fail**, and it fails in its first store or not at all. If the retirement fails: unlock, and start over as a safepoint operation. Nobody sees the property: the slot is filled (CO4), a walk over the extra properties passes over the name of a field (`isExtraKey`, `aot/object/AOTObjectKeyOrder.cpp:28-31`), and the object's StructureID does not name the side object's new Structure yet |
| 5 | `old` = the word that an atomic exchange replaces by `absent`. In a `DeferredClosure` slot: compare-and-swap the loaded word for `absent`; if that word was 0, `old` is the closure that was prepared | **point of no return.** From here fast reads fail their test, fast stores fail their compare-and-swap (CO3) |
| 6 | where `old` is wanted: store it over the placeholder (release). One store into a slot that exists, behind `storageForWriting`, which cannot fail here | readers that found `absent` and bit 27 are waiting for the lock. Nothing is allocated |
| 7 | StructureID of the variant that names the side object's new Structure (release) | CO9 |
| 8 | invalidations (07, section 7.5) | |

The value that step 5 returns is the last value that any fast store put there: no store is lost between "read the current value" and "empty the slot".

The states after steps 1, 2 and 3 are whole: an empty side object and a set bit 27 only send readers the long way round. So a mover whose step 4 fails just unlocks. A mover that has passed step 4 does not turn back: nothing between there and the end can fail, and a hidden copy that were left behind would come to light at the next `delete` of the field.

`defineOwnProperty` has no second half: the property enters the side object with the attributes, or as the accessor, that the definition asks for. There is no moment in which a reader finds it as a data property with its old value.

### (b) The way back (`tryMoveFieldBackToSlot`)

| Step | Write | Why |
| --- | --- | --- |
| prepare | the side object's Structure without the name; the object's variant that names it | L2 |
| 1 | compare-and-swap the slot from `absent` to the value | from here the slot is the truth (CO4) |
| 2 | remove the name from the side object as a redefinition does: the old slot keeps its last value and is treated as deleted (06, N3) | a reader that holds the old StructureID, and an entry "the name lies at offset k of the side object", gets the previous value, never an `undefined` that nobody stored |
| 3 | StructureID of the variant that names the side object's new Structure (release) | CO9 |
| 4 | invalidations | |

Bits 27 and 28 stay (CO8).

### (c) `delete` (R1 5.2)

Under the lock: `exchange(slot, absent)`; remove the key from the order (one 32-bit store of the order word, or the out-of-line form, (g)); invalidations. Between the two stores the object lists a key whose slot is empty.

**Rule CO16.** Whoever walks the listed fields of an object that may be shared takes an empty slot for a `delete` that has won: it skips the key, or drops what it has built and starts again under the object lock. `getOwnPropertyNames` re-checks each listed field already; compiled key loops read with `TryGetSlot`. `Object::readListedField` (`aot/object/AOTObject.h:458-464`) falls back to `read()`, which returns the empty value; in mt builds it returns `std::optional<JSValue>`, so that its users (`Object.values` and `entries`, `aot/object/AOTObjectKeys.cpp:76`; the property walk, `aot/object/AOTPropertyWalkInlines.h:60`; `aot/builtins/AOTObjectBuiltinOperations.cpp:235`) cannot pass the empty value on.

### (d) `freeze`, `seal`, `preventExtensions` (R1 5.4)

Under the lock: the side object's own change (its lock is lower, L3; its new Structure is prepared), StructureID, then `orderWord |= bits`. Writing `Atomics` operations on a layout whose integrity can change, and every slow-path store, take the lock, so none of them lands after the call has returned (04, section 4.6.6). A fast-path store that holds an older `NotFrozen` guard may (02, M9).
The inline form of `Object.freeze` (`aot/builtins/AOTObjectBuiltinEmitter.cpp:178-193`) loads the order word, sets the bits and stores it back without the lock. It is made for unpublished objects only (CO5; 04, section 4.8).

### (e) Adding an undeclared property (R1 5.5)

Prepare as in (a): the side object's next Structure and, if the capacity grows, new storage with a copy. Under the lock, in C++ and in the stub `addExtraProperty`: steps 1 to 3 of 06, section 6.4, on the side object, that is the retirement of the old storage if there is one to retire, then the value into the side object's slot, then the side object's StructureID (release); then the object's StructureID (release). The retirement is the only step that can fail, and it is the first store: if it fails, unlock, and start over as a safepoint operation (L6). The point of no return is the side object's StructureID. Today's stub stores both IDs before the value (R1 H6).
The stub serves the case in which the side object's storage has room and passes the state test of 06, section 6.2. Everything else, growth above all, leaves for C++, where L6 decides between the lock and a safepoint operation. `addExtraPropertyInNewStorage` (`aot/stubs/AOTStubsPropertyExtraProperties.cpp:187-214`) is used for Confined layouts only.

### (f) Creating a declared field after publication (`PutSlotOrAdd`, by-name store stubs, R1 5.7, 5.8)

An out-of-line field: `EnsureExtension` first, without the lock (5.5). Under the lock: compare-and-swap the slot from `absent` to the value; then the order word. Key order caches (`KeyOrderRun`, `KeyOrderStepCache`, the transition table's front) are per thread (07); the truth (`KeyOrderTable`) has a table lock.

### (g) Key order out of line and back (`setKeyOrder`)

Under the lock, in today's order. A lock-free reader of the pair (`Object::keyOrder`, `aot/object/AOTObjectKeyOrder.cpp:38-54`) loads the order word (**acquire**: it is the first load that licenses the second, 04, F3), then the StructureID, and takes the lock if the word says "out of line" and the Structure has no list.

A numbered order at or above `layout.numberOfOrders` is a reference into `KeyOrderTable`. The order word is stored plainly. The lookup (`entriesInOrder`) takes the table's lock, which the thread that added the order has passed through, so whoever has loaded an order word finds its order (07, section 7.7).

### (h) `setPrototypeOf` (R1 5.6)

5.7.

### (i) Construction

Unpublished: all of today's inline sequences (16-byte header copy, whole-word stores of the order word, `add` to memory), but for the mark of the cell (CO15). After the point where `this` leaks the compiler treats the object as published.

### What lock-free readers of metadata may see

| Reader | Sees | Consequence |
| --- | --- | --- |
| `IsNotFrozen`, `HasNoExtraProperties`, `HasNoFieldOutsideSlot`: one load of the order word | the state before or after | a legal order of the race |
| `HasKeyOrder`: 64-bit load of order and shape word | one consistent word | |
| `HasLayoutPrototype`: flags byte | sticky bit | "Compiled code tests the flag after the read that relies on it" (`aot/object/AOTStructureTable.cpp:533`) stays right |
| by-name probe: StructureID (acquire), then per-thread cache | CO9 | |
| C++ `extraProperties()`: order word (acquire), bit 28, then the hidden word; in a layout without that word, then the StructureID and `heldExtraProperties()` of the variant (`cellInPlaceOfExtraProperties`, `aot/object/AOTObjectExtraProperties.cpp:32-38`) | what the second load reads was stored before bit 28 (step 1 of a) | never "has extra properties" with an empty word or a variant that holds nothing |

### Operations that re-check what their stub saw

`crashOnWrongOperand` (`aot/runtime/AOTFieldOperations.cpp:77-78, 93-94, 120-121, 231-232, 244-245, 262-263`) and the release assertions of `readSlot`, `moveFieldToExtraProperties`, `createClassObject`, `ClassPrototype::tryGetDeclaredMember` abort where stub and operation, or two halves of one function, disagree. With threads a legal program can get there (R1 H16).

**Rule CO10.** For a Shareable layout an operation that finds another state than its caller saw has lost a race. It starts again from the generic dispatch. It aborts only for states that no interleaving can produce.

## 5.7 Prototypes

- **One process-wide prototype-chain lock** (`VM::prototypeChainLock`) around: the cycle walk, the change of the link, `didBecomePrototype` on the new prototype. Exactly one of two racing `setPrototypeOf(a, b)`, `setPrototypeOf(b, a)` succeeds. It is the topmost leaf lock (08, section 8.15): its holder does not park, so the Structures that the change and `didBecomePrototype` need are made before, or allocated without collecting (L2).
- **The lock is for every object, with a layout or without.** A chain passes through both sorts, so `JSObject::setPrototypeWithCycleCheck` and `setPrototypeDirect` (`runtime/JSObject.cpp:1956-2062`) take it around the same three steps as `AOT::Object::setPrototype`. A link that lies in a slot (poly-proto) is stored under it too.
- **What follows from the new chain comes after both locks are released**: `haveABadTime`, which is a safepoint operation, and `switchToSlowPutArrayStorage`, which is a conversion (06, sections 6.3.6, 6.3.7, 6.3.11). A holder of a leaf lock can do neither. For the same reason an array that is frozen in place and is to become a prototype moves its elements to a sparse map (`runtime/StructureCreateInlines.h:69-72`) **before** the lock is taken; nothing undoes that move. Until the two followers are done an indexed store into a hole can pass by a setter of the new chain. It reads the link that the other thread is writing, so the program has a race there.
- Then the object lock: flags byte `|= IsPristine` first, then the StructureID of the variant (release).
- The inline replay of a prototype transition (`aot/codegen/AOTAccessEmitterDefinitions.cpp:106-130`) is used on unpublished objects only: `__proto__:` in a literal, `new` with another prototype. Its cache is per thread.
- The stub in front of `Object.setPrototypeOf` (`aot/stubs/AOTStubsBoxedCalls.cpp:494-509`) replays only for Confined layouts. Otherwise it calls C++.
- `mayBePrototype` (`PropertyWalk::didBecomePrototype`) is a change of the StructureID like any other: under the lock of the object that becomes a prototype.

Every `setStructure(variantOf(current) with one thing changed)` is a read-modify-write of the StructureID (R1 H13). Under the lock none is lost.

## 5.8 Closures

| Kind | Mutable after creation | Rule |
| --- | --- | --- |
| `AOT::Closure` | `m_entry` and the captures: never. StructureID, butterfly, rare data: as any `JSFunction` | 06 for properties, 07, section 7.6 for lazily made parts |
| `AOT::SlimClosure` | the word at offset 8 changes once, from closure word to butterfly pointer (`becomeReified`, `aot/object/AOTSlimClosure.cpp:178-193`) | below |

**Reification.** Under the object lock, in today's order: fill the butterfly; nuke the StructureID; fence; store the word (release); fence; `setStructure`. Two threads: the second finds the work done. A nuked StructureID decodes to the Structure from before (`StructureID::decode`, `runtime/StructureID.h:73-77`), so C++ that meets the closure in the middle reads a valid Structure.

**Readers load the word once and decide on the loaded value** (04, principle C1). "A pointer's upper half never looks like the upper half of a closure word" (`aot/object/AOTCellFormat.h:43-47`), so one 64-bit load tells which it is.

- Call sites: already so. A loaded closure word stays a valid code offset.
- C++: `SlimClosure::function()`, `captures()`, `captureCountsOf` must not test `type()` and then read the word (R1 H8). New accessor `loadedWord()`; everything derives from it. If it is a pointer, the function number is in the butterfly, which is complete when the pointer is visible (release, address dependency).
- The captures stay where they are in both forms.
- The collector's seqlock-like read stays.

**Frame closures, lent closures**: thread private by C2 (04).

## 5.9 Environments

A bare cell: constant header, variables in slots, no Extension, no extra properties, no order word, no lock (byte 4 holds part of the shape).

- A variable is a location like a field. Plain loads and stores; a reference stored into an environment that may be shared is a release store on ARM64.
- TDZ: the read is followed by `CheckInitialized` on the loaded value. Already a test of a value.
- Where flow proved `StoredBeforeEveryRead` or `SetAtCreation` there is no marker and no check: allowed under rule O2 of 04. That rests on the field being addressed through the closure that carries the environment. An environment that `PromoteEnvironmentsToVariables` turns into variables loses this, and its facts are asked again under O3 (04, sections 4.7, 4.8).
- `MaterializeEnvironment` copies a frame environment to the heap "in front of the first use that lets it out": the copy is unpublished.
- `ReuseEnvironments`: the free-cell word is per thread (04, section 4.8).
- Coroutine frames are environments of kind `CoroutineFrame`: 09, section 9.6.

## 5.10 Classes

Everything of a class is made at first touch (R1 8.1). Creation protocols: 07, section 7.6. What the image names through operations that must not wait is made before the second thread exists (`Instance::prepareForThreads()`; 04, S5). A class object's cell is published in `closureCells[]` after `defineClass`, so whoever finds it finds its members. Two things belong here.

**The stand-in prototype.** `AOT::ClassPrototype` answers lookups from the image's record, counts lookups **in its own fields on reads**, and is converted in place to a `JSFinalObject` on a write or after enough reads (`aot/object/AOTClassPrototype.cpp:48-124`). It is the most shared object of a program.

*No writes on reads.* No counting in mt builds. The conversion happens on the first write, on the first generic access that the record cannot answer, or never. `hasMember` caches its answer for `then` in `m_flags`, in two steps (`HasCheckedThen`, then `DeclaresThen`, `:135-138`): a second thread that reads between them would take a class that declares `then` for not thenable. Both bits are published by one atomic `or`.

*The conversion.* In the source `convertToOrdinaryObject` empties the object (`ClassDefinition::prepareObjects`, `aot/object/AOTClassCreation.cpp:327-331`) and the members are defined afterwards, one by one, each with an allocation (`defineMembers`, `:342`). A thread that runs between the two would find a prototype without its methods, though `C.prototype.extra = 1` and `c.method()` do not conflict (02, section 2.10.1). So conversion and definition are one step for everybody else:

| Step | |
| --- | --- |
| 1 claim | the class's claim word (07, Z2): one thread prepares |
| 2 prepare, outside any lock | the class object; the ordinary Structure and the chain of transitions up to the Structure that has every member; the method closures and `GetterSetter`s; out-of-line storage if the members need it; what was stored early (`m_earlyPrototypeStores`) |
| 3 safepoint operation (08, section 8.6) | the closure checks that the object is still the stand-in. It zeroes the private words, stores the members into their slots, and sets the final Structure, with the fences and the nuke that today's code has for the marker's sake. It allocates nothing |
| 4 afterwards | guards are armed, dependents are told, the cache epoch moves (07, E1); the claim is released |

Once per class. The operations that can get here have the effect `waits` (04, S5).

*Readers.* `tryGetDeclaredMember` is not free of safepoints: `findDeclaredMethod` makes the method's closure (`aot/object/AOTDeclaredMembers.cpp:81-89`) and `createDeclaredAccessor` a `GetterSetter`, and an allocation's slow path parks. So a reader takes what it needs of the stand-in (`m_classIndex`, `m_memberRow`) once, and after every call that may allocate it loads the StructureID again. If the object is no stand-in any more it returns `std::nullopt`, and the caller goes on with `JSObject::getOwnPropertySlot` on the ordinary object. `createMembers` begins with the same test and returns at once if the work is done. The release assertion `classInfo() == info()` (`:109`) goes (CO10).
Between two safepoints of a reader the object does not change its class, so a stub, which has none, needs no test beyond its compare of the StructureID.

**Forgetting a definition** (`ClassTable::forgetDefinition`, `StructureTable::forgetStructuresOf`): a class that is evaluated again. Under the class table's lock; per-thread allocation records are dropped through the cache epoch (07, section 7.5).

## 5.11 `Thread.restrict` on an object with a layout

Fast paths test nothing that could refuse a thread. So a restricted object is an object **all of whose fields have left their slots**.

- Flow: the operand use `Restricts` marks the layouts that reach `Thread.restrict` with a new `ObjectMutation::Restrict`. For such a layout every field `canLeaveSlot()` (has an `absent` marker), reads are `TryGetSlot`, stores are `TryPutSlot`. A layout that flow does not mark cannot reach the call; if one does through unknown code, the call throws `TypeError("cannot restrict this object")`.
- `Thread.restrict(o)`: prepare a side object if there is none, the side object's Structure that has every field of the layout, new storage for that Structure with a copy, and a closure for each slot that still defers one. Under the object lock, or in a safepoint operation if L6 says so: steps 1 to 3 of 5.6 (a) for all fields; then **one** step 4, which enters all the properties at once, with placeholders, under one retirement and one StructureID of the side object; then steps 5 and 6 field by field; then the Structure variant `Restricted(owner)`. So the one step that can fail lies in front of the first emptied slot.
- The variant is a carrier of a thread's ID (09, T1): its visitor replaces a quarantined owner by `NoThread`. After the owner has ended the object is usable by nobody.
- Every access now fails its fast test and arrives at the method table, where the variant's flag leads to the owner check: `ConcurrentAccessError` for another thread.
- The owner pays the generic path too, as in #249.

## 5.12 Copies of objects

`createCopyOfWords`, `copyFieldsOfSameLayout` (`aot/object/AOTObjectAssign.cpp:90-160`), the stubs `generateCopyObjectFrontEnd`, `generateCopyDataPropertiesFrontEnd` (`aot/stubs/AOTStubsObjectCopy.cpp`) copy the fields of a live source in 8- or 16-byte pieces (R1 H12).

### The source

For a source that may be shared:

1. Load the header word and the order-and-shape word (acquire). If bit 27 or 28 is set, or the lock bit: generic copy.
2. Copy the fields. Each slot is copied whole (aligned, inside an 8-byte piece).
3. Load both words again (after an acquire fence). If either differs: drop the copy, go to the generic copy.
4. The copy takes the order word loaded in step 1.

Every structural change writes one of the two words, or holds the lock bit while it is under way (5.6), so equal words mean that only values changed, and a mix of old and new *values* is what a field-by-field copy in JavaScript could see too. The copy is unpublished, so nothing else is needed. An Extension is copied the same way, as one block, into an Extension of the copy's own.

The **generic copy** is what the language says: the list of keys, then a `[[Get]]` per key. It holds no lock, since a getter may run (L1).

### The target

**Rule CO14.** An object is filled by words only while it is unpublished.

`Object::tryCopyFieldsOf` (`aot/object/AOTObjectAssign.cpp:236-299`) serves `Object.assign(target, source)` for a target of the source's shape that lists no key yet. It copies all field words with `gcSafeMemcpy`, calls `setExtension` on the target, and stores a whole order word (`copyFieldsOfSameLayout`, `:90-100`; `tryCopyWordsOfSameStructure`, `:216-234`). On a published target that would replace an Extension which another thread has just published and holds in its window (CO6), and write the source's `absent` words by plain stores (CO3).
So these functions, and the assignment plans that move words (`applyMoves`), take a parameter `Unpublished`, as `putDirectUnpublished` of 06, section 6.4. Compiled code passes it where the target is unpublished (`Object.assign({}, s)`, spread into a literal). For any other target of a Shareable layout assignment goes field by field through 5.4 and 5.6 (f).

## 5.13 Reads by native code that write

`Object::read` materializes deferred closures; `extraProperties()` replaces pending stars; `fieldNamed` fills `m_cachedFields`; `fieldsInKeyOrder` fills a cache inside the shared `CompiledStructure`; `countExtraKeys`; `sourceOfPendingBinding` decrements a counter; `ClassPrototype` counts and caches (R1 H15).
The rows that speak of a Structure, a table or a per-layout record hold for Confined layouts too: two threads that call `Object.keys`, each on an object of its own, write the same `CompiledStructure`.

| Write | Rule |
| --- | --- |
| deferred closure | compare-and-swap of the slot, with the restart of 5.4 (07, Z1) |
| flattened object that keeps its slot | exists only in Confined outer layouts (04, section 4.8) |
| pending stars: `makePendingStars`, `extraPropertiesSlow` (`aot/object/AOTObjectExtraProperties.cpp:288-310, 624-630`) | each step of CO7 is a compare-and-swap of the hidden word; the loser drops its cell and takes the winner's. The side object that replaces the tuple is complete, with storage for all its properties, before it is published, so nothing has to grow inside the `DeferGC` scope of these functions (08, S4) |
| `RuntimeLayoutTable::m_cachedFields`, `FieldMovePlanCache`, `AssignmentPlanCache`, `PropertyListCache` | per thread (07, section 7.4) |
| `CompiledStructure::setListedFields`, `setNumberOfExtraKeys` | the list is built aside and published as **one pointer** to an immutable record `{fields, size, orderAndCount}` by compare-and-swap (release). Readers load the pointer once |
| counter of pending bindings | dropped in mt builds (a heuristic) |
| counters and `m_flags` of `ClassPrototype` | 5.10 |

## 5.14 Static fields

A static field of a class with one class object lives in a **variable** in the shared statics, with a slot representation and an absent marker. The class object shows it through one shared `CustomGetterSetter` (`aot/object/AOTStaticFields.cpp:117-133`). A store that the variable cannot hold, a redefinition or a `delete` takes the property out of the variable: `moveToProperty` (`:135-147`), `didDeleteProperty` (`:163-169`). A later definition may bring it back (`canEnterVariable`, `:59-76`).

Compiled code reaches the variable without the class object: `GetModuleVariable` and `PutModuleVariable` behind a `MemberUnchanged` guard, at a type without `absent` (`aot/analysis/AOTPropertyAccessSlots.cpp:558-589`). The guard may be stale (04, section 4.5). So the variable is treated as what it is, a slot that its property can leave.

| # | Invariant |
| --- | --- |
| **CO11** | The bits of the variable are at all times a value of the field's proven type in the variable's representation, or one of the representation's markers |
| **CO12** | Once the class object is published `absent` is written into the variable only under the class object's lock, by an atomic exchange. No fast path overwrites `absent` |
| **CO13** | A variable that holds a value is the truth about the property. While it does, the class object's own property of that name is the `CustomGetterSetter` |

| Access | A variable that `canLeaveSlot()` | Any other |
| --- | --- | --- |
| read in compiled code | `TryGetVariable`: one load, test the loaded word; `empty` goes to the access by name (04, section 4.6.1) | `GetModuleVariable` |
| store in compiled code | `TryPutVariable`: compare-and-swap from a loaded word that is not `absent`; `empty` goes to the store by name (04, section 4.6.2) | `PutModuleVariable`, release for a reference |
| stubs and handlers that reach the variable after a hit in a cache (`findFieldForCache`, `:171`) | the same two forms | |
| the custom getter and setter, `readStaticField`, `ProgramStatics::writeVariable` | load once. On `absent`: take the class object's lock and look again, first at the variable, then at the class object's property. A store is a `compareExchange` from a word that is not `absent` | |
| the field's definition in the class's evaluation | a plain store while the class object is unpublished (04, section 4.3). Otherwise by name, under the lock: compare-and-swap from `absent` | |

**Leaving the variable** (`moveToProperty`, `didDeleteProperty`). The class object is an `AOT::Closure`; its properties follow 06, section 6.4, and L6 holds.

| Step | Write |
| --- | --- |
| prepare | `ensureProperties`; the class object's next Structure, in which the name is an ordinary property at a fresh offset (06, N3), or is gone; if that Structure has a larger out-of-line capacity, new storage with a copy (06, section 6.4) |
| lock, check | the StructureID and the storage pointer are those that were planned from |
| 1 | the storage: retire the old one by compare-and-swap if the capacity grows, nuke, publish. Then the value at the fresh offset: the value that did not fit, or the accessor, or a placeholder where `old` is wanted. **The last step that can fail**; if the retirement fails, unlock, and start over as a safepoint operation. Nobody sees the offset: the class object still has the Structure in which the name is the `CustomGetterSetter`, and the variable is filled (CO13) |
| 2 | `old = exchange(variable, absent)`: the point of no return |
| 3 | where `old` is wanted: store it at the fresh offset (release) |
| 4 | the class object's StructureID (release). For a `delete`: the Structure without the name |
| 5 | unlock; `didChangeMember` ends the guard (07, E1) |

Between steps 2 and 4 a reader by name still finds the custom property; its getter loads `absent` and waits for the lock (table above). `old` is the last value that any fast store put there. A fast store that comes later fails its compare-and-swap and goes by name.

**Entering the variable**: under the lock, remove the ordinary property as in 5.6 (b), after a compare-and-swap of the variable from `absent` to the value.
