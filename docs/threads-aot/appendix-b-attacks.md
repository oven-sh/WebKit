# Appendix B. Attacks

The interleavings by which earlier drafts of these documents were broken, and some that the rules withstand. Not normative. It is kept because it is the reason behind many rules: whoever wants to relax a rule finds here what the rule is there to stop, and each attack is a test (12, section 12.2, "Directed tests").

**How to read it.** Each entry names hazards of appendix A, tells the interleaving step by step, says why the text *of the draft that was attacked* did not close it, and proposes a rule. Quotations of the documents in this appendix are from those drafts, not from the present text, and section numbers may have moved. The proposals were taken up, sometimes in another form: **the present text of the documents decides**, and appendix A names the rule that answers each hazard. Entry numbers (`P4`, `F6`, `N2`) are local to a part.

## B.1 Compiled objects and the compiler (reports R1, R2)

Most severe first.

### N1. A coroutine of a thread that has ended is resumed on another thread

1. Thread T2 runs `new Thread(() => { void worker(); })` with `async function worker() { await shared; use(); }`. `fn` returns `undefined`.
2. "A pending `await` does not keep a thread alive" (02 2.6). T2 completes. Its view is unmapped, its context and allocators are freed, its ID is released (08 8.14; 09 9.2, step 14).
3. The main thread settles `shared`. The reaction's home has ended, so "the job goes to the main thread's inbox" (09 9.7; 02 2.6).
4. The main thread enters T2's `SavedFrame`. By 09 9.6 the frame may hold an allocator's address, a table base of a per-thread cache, or an address computed from `instanceGPR`. All of them point into unmapped or freed memory.

Second way, an ABA on the ID: `SavedFrame::m_thread` and `Generator::m_thread` are 16 bits, without the "generation" that 09 9.7 compares, and IDs are reused (06 6.2 calls that harmless). A new thread that gets T2's ID passes "home is me" and the test of `StepGenerator`, and resumes T2's frame.

G1 states an ownership that J1's fallback and the reuse of IDs both break. 04 4.9 and 07 7.3.3 rest the immutability of `AOTInstance_fixedFields` on G1.

**Proposal.** Either of:
- **G2.** "No value that was loaded from the view or computed from `instanceGPR` is live across an operation that suspends. The verifier checks it as it checks `window` values, with `maySuspend` in place of `mayCollect`." (09 9.6 names this rule as future work. With J1's fallback it is needed in the first version.)
- **G1'.** "A job that would enter a `SavedFrame` whose home thread has ended is dropped, as a pending `await` is dropped at the end of a process. `next`, `return`, `throw` on a generator whose home has ended throw `ConcurrentAccessError`."

And in both cases: "The home of a frame, a generator or a reaction is a thread serial number that is never reused (32 bits), not the 16-bit ID of the storage word."

### R2 H12 (with R1 H3-4, R2 5.2-8, 5.3-4, 6.2-F, 9.3-7). Order facts about variables rest on causality that nothing preserves

**How the source works.** The order analysis runs per scope owner. A function's entry is the first point in the owner's frames at which it is mentioned or used (`recordClosureUse`, `aot/valueflow/AOTFlowOrderOwner.cpp:383`). A reader in another module counts as entered after the owner's exit where the module order says `CompletesBefore` (`recordOutsideReader`, `:496`). A store precedes a read if it dominates that point (`precedes`, `aot/valueflow/AOTFlowOrderLocations.cpp:35`).
`FlowCall` is `{caller, site, callee}` and only refines entries (`refineEntriesByCalls`, `aot/valueflow/AOTFlowOrderOwner.cpp:598`). Nothing can ask "does this store precede that spawn site", if the site lies in another owner.

```js
// main.js                                                   // late.js
const box = { fn: null };                                    const table = new Table();      // V: StoredBeforeEveryRead, no marker
new Thread(() => { for (;;) { const f = box.fn;              exports.g = () => table.size;
                              if (f) { f(); break; } } });
box.fn = require("./late.js").g;
```

1. T2 starts and spins. The read of `box.fn` is pinned.
2. Main evaluates `late.js`: stores V, makes `g`. In the owner `late.js` the store dominates the mention of `g`, so the read of V in `g` has no check and V has no marker.
3. Main stores `box.fn = g`.
4. In T2 flow knows `box.fn` is `null` or `g`, so `g` is inlined behind `IsClosureOf`. The load of V is `[statics + c]`. The statics are published as dereferenceable (`aot/codegen/AOTPrimitiveEmitter.cpp:209`), so the load is not control dependent (`b3/B3Procedure.cpp:584`). The loop writes only what its poll declares. `hoistLoopInvariantValues` may move the load in front of the loop, where V is 0.
5. After step 3 T2 uses 0 as a proven reference: a fault at a site that is not registered. For an integer whose range excludes 0 a range that B3 trusts can be wrong.

This happens on x86-64 too. On ARM64 it needs no hoisting: the load of V has no address dependency on the load of `box.fn`, so F2 does not apply, and F3 names no such use.

**Why the text does not close it.** O1 (b), read transitively, drops the fact. But it speaks of "the spawner", and V's owner is another function. The natural implementation (a thread entry is one more use of a closure) gives (a) and (b) inside one owner for free and keeps this fact. 02 2.10.6 allows moving a read earlier, and 04 4.11 says a poll does not end remembered reads.
O2 is sound because a field is addressed through the reference that carries the causality. `PromoteEnvironmentsToVariables` removes exactly that dependency, and 04 4.8 leaves it "unchanged".

**Proposal, rule O3.** "A location that is addressed from the view (module variable, static field, field of a promoted environment) and has `threads = Shared`: an initialization fact, per location or per read (`readsAfterInitialization`), holds for a read in an AnyThread function only if, for every `Concurrent` site `s` from whose entries the reader can be reached, (i) the owner's module `CompletesBefore` the module of the function that contains `s`, or (ii) `s` lies in a frame of the owner and an initializing store dominates `s`.
Otherwise the reader's entry in that owner is `unknownScopeEntry`: the type keeps `absent`, the variable has a marker, the loaded value is tested (C1)." A fact that passes holds from the thread's first instruction, so no motion of the read can break it.

### R2 H11 (with R2 H29, 6.3-8). A stale `MemberUnchanged` guards an untested read of a static field's variable

A static field lives in a variable with an absent marker. Compiled code reads it by `GetModuleVariable` at a type without `absent` (`planVariableRead` asks for a known initialization, `aot/analysis/AOTPropertyAccessSlots.cpp:558`), under a `MemberUnchanged` condition (`finishAtStaticField`, `aot/analysis/AOTPropertyAccessStaticMembers.cpp:62`).

1. T1 passes `AreMembersUnchanged` for `C.count`.
2. T2 runs `delete C.count`, or redefines it: `didDeleteProperty` / `moveToProperty` write `absent` into the variable, then end the guard (`aot/object/AOTStaticFields.cpp:135-170`).
3. T1 loads the variable and decodes the marker as a value: `Cell32` 0 is the base of the cage, `OptionalInt32` is `INT32_MIN + 1`, outside the range that B3 trusts.

Store side: T1's `PutModuleVariable` after step 2 refills the variable. The property then exists twice (`isInVariable()` and an own property of the class object), and a deleted property comes back.

04 4.5 files `MemberUnchanged` under "the old member is used once more". 4.6.1 and 4.6.2 speak of slots of objects only. 07 7.5 leaves the guard kinds "to be confirmed". 02 2.11 lists the static field as a location to be tested.

**Proposal, rule CO11.** "The variable of a static field that `canLeaveSlot()` is a slot in the sense of CO2 to CO4. A read loads once and tests the loaded word; `absent` goes to the access by name (`TryGetVariable`, the twin of `TryGetSlot`). A store is a compare-and-swap from a word that is not `absent`. `moveToProperty`, `didDeleteProperty` and the entry into the variable run under the class object's lock; they empty the variable by `exchange(variable, absent)` and put what the exchange returned."

### N2. `AtomicSlot` breaks CO2 and CO3

1. **Overflow.** "`add` and friends on an `Int32` slot are B3 atomics with an overflow exit." `lock xadd` / `ldaddal` has changed memory when the overflow is seen. Other threads read the wrapped number, and the operation cannot take it back atomically.
2. **Results that are markers.** In an `OptionalInt32` slot values are `>= INT32_MIN + 5`. `add`, `or`, `xor` can produce `INT32_MIN + 1` (`absent`): a fast path writes `absent` (CO3).
3. **Markers as operands.** T2 deletes the field (`absent = INT32_MIN + 1`). T1's `xadd 1` makes `INT32_MIN + 2` (`undefined`): the slot is refilled without a key.
4. **`store`, `exchange`.** A fenced store and `AtomicXchg` overwrite whatever is there, also `absent`. 4.6.6 sends writing forms to the lock unless `ChangeIntegrity` is impossible. It says nothing of fields that can be deleted, redefined or moved out.
5. **`load`.** A fenced load has no test of the loaded word, though 02 2.5 asks for a `TypeError` on an absent property.
6. **Flow.** The overloads of 03 3.5.2 declare heaps only. Nothing lets the value flow into the field's type, nothing raises `AddProperty` (`store` creates), nothing declares the value `retained` (C2). A field then keeps `IsNeverStoredWithOtherType`, and the run time meets `RELEASE_ASSERT(field.canLeaveSlot())`.

**Proposal.** "Every form of `AtomicSlot` is a load, a test of the loaded word (marker: leave for the operation), the computation in registers with its range test (result outside the slot's values: leave for the operation), and a compare-and-swap of the loaded word. `load` is a load-acquire and the same test. Unconditional read-modify-write instructions are used only on representations without markers and for operations that cannot leave the representation's range (`and`, `or`, `xor`, `exchange` on `Int32`).
In the table each overload declares that it stores `value` under `key` of `object` (the counterpart of `b.readByKey`) and that `value` is `b.retained`, so that flow treats it as the store by name that it is."

### N3. The main thread cannot hold a `Lock`

04 4.13: "compare-and-swap the lock's word from 0 to the thread's ID", "store 0 (release)". 09 9.13: the word is `{ holder thread ID, waiter bit, async-held bit }`. 02 2.2 and 09 9.2: the main thread's ID is 0.

1. Main: `lock.hold(f)`. The compare-and-swap of 0 for 0 succeeds. The word stays 0.
2. T2: `lock.hold(g)`. The compare-and-swap of 0 for 2 succeeds.
3. `f` and `g` run at the same time. Main's release then stores 0 over T2's hold.

"Lock is not recursive" cannot be told on the main thread either. The claim word of 07 Z2 (`0 -> Claimed(thread)`) needs the same care.

**Proposal.** "The holder field of a lock word or a claim word is the thread's ID plus one. 0 means free."

### R2 H4 (with R2 5.4, 9.3-1, 9.3-2). `windowStorage` and `pinnedReads` cut a load off from this thread's own stores

A B3 memory value has one `HeapRange` (`b3/B3MemoryValue.h:156`). `MemoryNumbering` keys a load by address, offset and the epoch of that one heap (`b3/B3MemoryNumbering.cpp:362-375`). Heaps "outside the IR" overlap no heap of the IR. So a load that 04 4.9 and 4.12 put into one of the two new heaps is no longer ended by a store that stays in the location's own heap. On one thread:

1. `while (o.n < 10) { o.n++; if (o.n > 5) break; }`. Both reads are flagged and lie in `pinnedReads`; the store lies in `ObjectFields[n]`. The store does not advance the epoch of `pinnedReads`, and "two flagged reads in one turn may still be merged": the second read yields the value from before the store.
2. `LoadExtension` yields the page of zeros. `ensureExtensionHoldsWithoutCollecting` (`aot/codegen/AOTSlotAccessEmitter.cpp:613`) publishes an Extension in a stub or an operation. Its patchpoint declares the hidden word's own heap and does not collect, so it does not write `windowStorage`. The next `LoadExtension` is merged with the first, and the store goes to the read-only page.
3. `push` in line stores `publicLength` in a heap under `ArrayStorage`; `GetStorageLength` loads it from `windowStorage`: `a.length` after `a.push(x)` is the old one.

(What 04 4.9 left "to be verified" is in order: the hoister adds the writes of every value of a loop, polls included: `b3/B3HoistLoopInvariantValues.cpp:198`.)

**Proposal.** No new heaps. "In an mt image every operation that `collects` also declares, in the IR, `writes(ArrayStorage, TypedArrayStorage, CollectionStorage)` and the heap of the hidden words, restricted by the alias step to subjects that may be shared. `PinLoopExitReads` records the pinned locations of a loop in its `LoopHint`, which declares `writes` of them."
Then `EliminateRedundantReads`, both hoisters, `carryVariablesAroundLoops` and B3 follow from the effects, a load and this thread's stores share a heap, and the flag `isPerformedEveryTurn` is not needed.

### R1 H8-2 (with R2 H28, 6.2-H). The conversion of a class prototype's stand-in

**The window contains a safepoint.** 05 5.10: "A reader in `tryGetDeclaredMember` is C++ without a safepoint." The function calls `findDeclaredMethod` and `createDeclaredAccessor`, which allocate a closure or a `GetterSetter` (`aot/object/AOTClassPrototype.cpp:105-106`). An allocation's slow path parks (08 8.4).
1. T1 reads `m_classIndex`, enters `findDeclaredMethod`, parks in the allocator.
2. T2's safepoint operation converts the object.
3. T1 goes on: `RELEASE_ASSERT(classInfo() == info())` (`:109`) aborts, against M11.

**The converted object is empty.** `convertToOrdinaryObject` is called from `ClassDefinition::prepareObjects` (`aot/object/AOTClassCreation.cpp:327-331`). The members are defined afterwards, one by one, each with an allocation (`defineMembers`, `:342`).
1. T1: `C.prototype.extra = 1`. The conversion runs as a safepoint operation; all threads resume.
2. T2: `c.method()` by name finds an ordinary object that does not have `method` yet: `TypeError`.
3. T1 defines `method`.

The two accesses do not conflict (02 2.10.1), so M1 is broken for a program without races.

**Proposal.** "The safepoint operation is all of `createClassPart(.., PrototypeMembers)`. What it needs (the ordinary Structure and its transitions, method closures, accessors, storage) is made before, outside, under the class's claim word (Z2); the closure only converts and stores. A reader takes what it needs of the stand-in (`m_classIndex`, `m_memberRow`) once, and after every call that may allocate it tests the cell's type again and starts over at the method table if it has changed. CO10 applies to `ClassPrototype`."

### R1 H1 (with R1 H9-1, 10-3, R2 H8, H15, 5.1-7, 5.2-4, 6.3-3). Users of `SlotFilled` that `TryGetSlot` does not replace; `DeferredClosure`

`TryGetSlot` says `replaces = IsSlotFilled, GetSlot`. `GetSlotGetter`, `GetFlattenedSlot` and `GetDeferredClosureSlot` also say `relies = Shape, SlotFilled` (`aot/ir/schema/prepared-operations.def:36-39`), and 04 4.5 declares that proof "replaced".

**`GetSlotGetter`.** T1 passes `IsSlotFilled`. T2 redefines the accessor and empties the slot. T1 loads 0 and reads the getter out of a `GetterSetter` at address 0, or at the base of the cage.

**`GetDeferredClosureSlot`** (0 = not made yet, 1 = `absent`).
1. T1 passes `IsSlotFilled`, loads the word once. `loadDeferringSlot` takes every word `<= 1` to the operation (`aot/codegen/AOTSlotAccessEmitter.cpp:483-502`).
2. T2 has stored 1 in between. `materializeDeferredClosure` returns the empty value (`aot/object/AOTDeferredClosures.cpp:87-88`).
3. Compiled code compresses and decodes it as a cell.

**Z1 on such a slot.**
1. T1 loads 0, makes the closure.
2. T2, under the lock: `exchange(slot, absent)`.
3. T1's compare-and-swap from 0 fails. Z1 says "drop `v` and use what is there". What is there is `absent`, or `undefined`, `null`, or a value that the program stored.

**5.6 (a), step 4** can return 0, which is no value that step 5 can put into the side object. (A compiled store can set the word back to 0: `writeSlot`, `:563`.)

**Proposal.** "Every read of a slot that has markers, of a subject that may be shared, is a terminator with an `empty` edge: `TryGetSlot`, `TryGetSlotGetter`, `TryGetDeferredClosureSlot`. No operation relies on `SlotFilled` for such a subject; the verifier rejects it. **Z1s**: where the word has states other than "not made" and "made", the loser of the compare-and-swap starts again from the load. An operation that returns a made thing can return "empty", and its caller has an edge for that.
In 5.6 (a) and (c), if the exchange returns "not made yet", the mover makes the closure under the lock (L2) before step 5."

### N4. The host moves values to the main thread, and no rule of 4.2.3 says so

02 2.6 sends these to the main thread: uncaught exceptions of a job or timer of a spawned thread, unhandled rejections, reactions and `FinalizationRegistry` callbacks whose thread has completed.

1. T2, in a timer: `const e = new MyError(state); throw e;` and `state` stays in use on T2. Flow sends what a `Later` callback throws to `escaped`.
2. A MainOnly handler of `process.on("uncaughtException")` receives the unknown object, that is, any escaped object.
3. R5 puts `escaped` into `Shared` only if an AnyThread function **receives** the unknown object. If none does, `MyError` and what it holds enter `Shared` by none of R1 to R5, so by R6 they are "local to the thread": Confined.
4. Main reads `e.state.items` with today's code (hoisted butterfly, birth flag, packed Extension) while T2 writes it.

Closures that a fallback runs on the main thread carry Confined objects of the ended thread in the same way. That is not concurrent, but it fails the validating check `ConfinedObjectOnOtherThread`.

**Proposal, rule R7.** "`escaped` (and `lost`) is in `Shared` as soon as an AnyThread function lets anything escape **or** receives the unknown object: the host is a channel between threads. What a `Later`, `Repeatedly` or `PerEvent` callback registered by an AnyThread function throws, and the reason of a promise that an AnyThread function can reject, are in `HandedToThreads`." If N1 is closed by G1', the fallbacks for plain reactions stay, and closures registered on a Shareable promise count as handed over.

### R1 H6-2 (with R1 10-21). The side object has to grow in the middle of a critical section that cannot park

05 5.6: each mutation "runs under the object lock from first to last step". 06 6.4: storage in state `Shared` is replaced only in a safepoint operation, for which "the lock is released". L1: a holder of an object lock does not park. The side object's storage becomes `Shared` as soon as a second thread stores into an existing out-of-line extra property, which takes no lock (05 5.3).

1. T1, `moveFieldToExtraProperties`: steps 1 to 4. The slot is `absent`, bit 27 is set.
2. Step 5: the side object's out-of-line storage is full and `Shared`. T1 may not replace it under the lock, since T2 may be between its tag compare and its store.
3. If T1 releases the lock to run a safepoint operation, a reader that was waiting finds `absent` in the slot and nothing in the side object: a lost property (M8).

The same at 5.6 (e). An allocation that fails at step 5 (no collection is possible under the lock, L2) leaves the same state. "Prepare" lists the outer variants, not the side object's next Structure or its capacity.

**Proposal.** "Prepare includes the side object's next Structure and a free slot for the property. If its storage must be replaced and is not `Owned(me)`, the whole mutation, steps 1 to 7, runs as the closure of a safepoint operation. Under the lock the mover first compares the side object's StructureID and storage pointer with what it planned from, and starts over if they differ. Nothing behind step 4 allocates."

### N5. Copies by words into a published object

05 5.12 treats the source. "The copy is unpublished" holds for `createCopyOfWords`, not for `Object::tryCopyFieldsOf` (`aot/object/AOTObjectAssign.cpp:236`), which serves `Object.assign(target, source)` for a target of the same shape that lists no key yet. It copies all field words with `gcSafeMemcpy`, calls `setExtension` on the target and stores a whole order word (`copyFieldsOfSameLayout`, `:90-100`; `tryCopyWordsOfSameStructure`, `:216-234`).

1. T2: `t.opt = 1` on a shared, empty `t`. `ensureExtension` publishes E2 without a lock (05 5.5). T2 holds `ext = E2` in its window and waits for the lock to list the key.
2. T1: `Object.assign(t, s)`, under the lock by 05 5.3: `setExtension(copy of s's Extension)`. CO6 is broken.
3. T2 gets the lock, stores into E2, lists the key. The key is listed and the value is gone.

The `memcpy` also writes the source's `absent` words by plain stores (CO3), and the assignment plans (`applyMoves`) are named nowhere.

**Proposal.** "A fast path that fills an object by words is taken only if the target is unpublished (compiled code knows; C++ takes a parameter, as `putDirectUnpublished` of 06 6.4). For any other target of a Shareable layout assignment goes field by field through 5.4 and 5.6 (f). `setExtension` on an object that has an Extension is a release assertion for unpacked layouts."

### R2 H25 (with R2 5.1-10, 5.1-17). The releasing side of two edges does not synchronize

02 2.10.6: "all of 2.10.2 are full barriers for the compiler". The list of 04 4.4 has no operation that settles a promise and not `FinishModuleEvaluation`. Their rows are narrow: `ResolvePromise`, `RejectPromise`: `Allocates UsesRuntime reads(PromiseState) writes(PromiseState ..)` (`aot/ir/schema/intrinsic-operations.def:33-34`); `ReturnAsyncResult`, `PromiseAllOfElements` (`prepared-operations.def:126-127`); `FinishModuleEvaluation`: `writes(ExternalState)` (`:134`).

`Select` moves the members of a group **down** to the root over everything that does not interfere (`crossesEffects`, `aot/select/AOTSelectionContext.cpp:137-152`), and 04 4.8 admits `PutSlots` on published objects for fields of the first row of 4.6.2.

1. T1: `o.a = 1`, then a settling of `p` with 7 that has become `ResolvePromise`, then `o.b = 2`. T2 has registered `p.then(() => read(o.a))`.
2. `SlotStoreRun` fuses the two stores at the place of the second: `ResolvePromise` neither reads nor writes `ObjectFields`.
3. T1 settles `p`; T2's reaction runs and reads the old `o.a`. The program has no race: M1 is broken.

**Proposal.** Add to the table of 4.4: "`ResolvePromise`, `RejectPromise`, `ReturnAsyncResult`, `PromiseAllOfElements` and the resolving functions, unless the promise is unpublished or its site is Confined: `synchronizes`. `FinishModuleEvaluation`, `AbandonModuleEvaluation`: `synchronizes`. In general, each row of 02 2.10.2 names the IR operations and table entries of both its ends, and a test of the schema compares the two lists."

### R2 H22 (with R1 H3-1, R2 5.1-2, 5.1-5, 9.4-3). `PinLoopExitReads` does not deliver M10

1. **State tests are not reads.** `while (o.x === undefined) {}` with `x` undeclared becomes `HasNoExtraProperties` and the constant `undefined`. The test is hoisted by `PlaceTypeTests`, nothing in the loop synchronizes, and the backward slice of the condition holds no read. The loop never ends after `o.x = 1` elsewhere. The same for `"x" in o` (`IsSlotFilled`), `Object.isFrozen(o)` (`IsNotFrozen`), `HasKeyOrder`, `AreMembersUnchanged`, `HasLayoutPrototype`.
2. **Kinds of read that are not listed.** Typed array elements (a byte of an ArrayBuffer is a location, 02 2.10.1), `IsBelowLength`, inline `Map.get` / `has` / `size`, `GetInternalField`, promise state.
3. **"The slice stops at calls"** is wrong in B3. A call whose summary only reads becomes a patchpoint without writes and without a sideways exit (`applyEffects`, `aot/codegen/AOTRuntimeCallEffects.cpp:72-117`). It is not `mustExecute()`, its reads overlap nothing that the poll writes, and its block runs in every turn, so the hoister moves the **call** out of the loop (`b3/B3HoistLoopInvariantValues.cpp:257-311`). `while (!isDone(o)) {}` with a leaf `isDone` that was not inlined spins for ever.
4. **Place in the schedule.** "Before `EliminateRedundantReads` first runs", in `Optimize`. But `Simplify`, which holds that pass, is part of `Prepare` (`aot/pipeline/AOTPipeline.cpp:256-290`). `if (flag) return; while (!flag) {}` on a module variable loses the read in the loop before the new pass has run.
5. **Natural loops only.** The verifier asks for a `LoopHint` on every cycle of the graph, reducible or not.

**Proposal.** With the proposal of R2 H4 (the `LoopHint` declares what it writes): "For each cycle with a `LoopHint` and each edge that leaves it, take the backward slice of the condition **and of every guard that an operation of the slice names**. For each operation in it that reads a heap, calls with their summaries included, add the shared locations that it reads to the writes of the `LoopHint`. The pass is the first step of `Simplify` and runs again after `InlineCalls`." Guards then end at the poll through `mayEndProofOver`, and read-only calls stay in the loop.

### R1 H10 (with R2 H16). F1 outside the emitters

F1 is a rule of the compiler's document. The plan has one line for "C++ (`WTF::Atomic` orders)". No rule says which C++ stores are meant. On ARM64:

1. T1, in C++: `JSObject* n = construct(..)`, then `array->setIndexQuickly(vm, i, n)`, `object->putDirectOffset(vm, offset, n)`, `WriteBarrier<T>::set`, `gcSafeMemcpy` in `splice` / `concat` / `fill`: plain `str`.
2. T2, compiled: loads `n` from the shared holder and reads its shape word through the address dependency (F2). T1's initializing stores are not ordered before the `str`: T2 sees the bytes of the cell that lay there before, which may pass a shape test of another layout.

Stubs that store a reference into a filled slot or an existing property have the same gap. 06 says "release for a cell" for its own sequences; 05 5.4 says it for `tryStore` only.

**Proposal, rule F1c.** "In an mt build on ARM64 `WriteBarrierBase<T>::set`, `setWithoutWriteBarrier`, `JSValue` stores through `locationForOffset` and the element setters of `Butterfly` are release stores, unless the call site uses a variant named `..Unpublished`. A bulk copy of references into storage that is not unpublished is preceded by one `dmb ishst`. Every stub that stores a register that may hold a cell into a holder that may be shared uses `stlr`." (A store-store fence is enough for M6: what must be seen are stores.)

### R1 H2-3. The order word is a reference into a table of the run time

An order number at or above `layout.numberOfOrders` names `addedOrders[n]` of `KeyOrderTable::Storage`. `entriesInOrder` finds the layout's record in a hash table that grows (`ordersOf`), then `RELEASE_ASSERT(index < orders.numberOfAddedOrders)`, then `orders.addedOrders[index]->entries()` (`aot/object/AOTKeyOrderTable.cpp:301-307`). `addOrder` replaces the array (`:366-378`) and `append` may replace the order (`:381`).

1. T1, under the table lock, adds order `n`; unlocks; under the object lock stores the order word `(n, count)` by a plain 32-bit store (05 5.6 f).
2. T2, `Object.keys(o)` without a lock ("Reads" take none, 05 5.3): loads the order word and sees `n`.
3. ARM64: a plain store behind an unlock is not ordered behind it. T2 reads the old `numberOfAddedOrders`: abort. Or it reads the old array, or `length` before the entries.

On any CPU T2's lookup races with a rehash of `ordersOf`. 07 7.7 keeps "what `entriesInOrder()` returns stays where it is". That is a statement about the result, not about the lookup. F1 covers cell references only.

**Proposal.** "Lookups in `KeyOrderTable::Storage` take `KeyOrderTable::m_lock`. The span that they return may be used after the unlock. The per-thread fronts of 07 7.4 answer repeated lookups. Because reader and adder pass through one lock, an order word that a reader has loaded names an order that its lookup finds." (The alternative, a release store of every order word and a table as in PT1, costs more.)

### R1 H7-1. `HasNoFieldOutsideSlot` tests the mark that nobody maintains

04 4.6.2: the mark "is not maintained for Shareable layouts by compiled code, since nobody reads it for them". 04 4.8: it "is neither tested nor cleared". 05 5.6 (a) has no step that sets it. But 05 5.6 lists `HasNoFieldOutsideSlot` as "one load of the order word or the flags byte", and in the source the usual form **is** the mark: `flagsTestedBy` returns `TypeInfoPerCellBit` unless the test may meet an unfinished object (`aot/codegen/AOTGuardEmitter.cpp:174-186`).

- A layout that is born unfinished: the mark is never cleared, the test always fails, and every such access takes the generic path for ever.
- Any other layout: T2 moves a field out (bit 27, no mark) and releases a lock. T1 acquires it, tests the mark again, passes, and `GetSlotOrUndefined` answers `undefined` for a property that has a value. M1 is broken.

**Proposal.** "For a subject that may be shared `flagsTestedBy(HasNoFieldOutsideSlot)` is 0: the test loads the order word and tests bit 27. `planTestsOfFlags` merges only `IsPristine` for such subjects. The run time neither reads nor writes `TypeInfoPerCellBit` of an object of a Shareable layout."

### R1 5.1-3. Stubs that store by name into a filled slot

05 5.3: "stores of values into filled slots" take no lock. 05 5.4 gives the compare-and-swap for compiled code and for `Object::tryStore`. 07 K1 says that no stub which probes a cache changes. `putKnownField`, `storeField`, `storeFieldByTable` (`aot/stubs/AOTStubsKnownFieldStores.cpp:186-407`, `aot/stubs/AOTStubsFieldStores.cpp:438`) test the slot's presence and then store plainly, with the representation dispatched at run time.

1. T1's stub finds the slot filled.
2. T2: `exchange(slot, absent)`, then the accessor goes into the side object.
3. T1's stub stores. The slot wins (CO4) and the accessor is hidden. CO3 is broken by a path that no rule names.

**Proposal.** In 05 5.4: "The stubs `storeField` and `storeFieldByTable` end, per width, in a compare-and-swap from the word that their presence test loaded, if the field record says `canLeaveSlot` and the layout record says Shareable; on failure they test again. K1 speaks of probes and fills, not of the access that follows a hit."

### R1 5.2. The empty value leaves the run time during a `delete`

05 5.6 (c) names two readers that tolerate "listed key, empty slot". `readListedField` falls back to `read()`, which returns the empty `JSValue` (`aot/object/AOTObject.h:458-464`). Its users do not test for it: `Object.values` and `entries` (`aot/object/AOTObjectKeys.cpp:76`), the property walk (`aot/object/AOTPropertyWalkInlines.h:60`), a built-in operation (`aot/builtins/AOTObjectBuiltinOperations.cpp:235`).

1. T1: `Object.values(o)` takes the field list for `(Structure, order word)`.
2. T2: `delete o.a` has emptied the slot and not yet stored the order word.
3. T1 appends the empty value to the result array: a hole in a dense array, or a null cell in typed code.

**Proposal.** "A walk over listed fields treats an empty result as `delete` having won: it skips the key, or drops what it has built and starts again under the object lock. `readListedField` returns `std::optional<JSValue>` in mt builds."

### R2 6.3-5. `GetSlotOrUndefined` under a stale guard yields a value that nobody wrote

1. `o.x` holds 5. T1 passes `HasNoFieldOutsideSlot`.
2. T2: `o.x = "s"`. The slot cannot hold it: bit 27, `exchange(slot, absent)`, the string goes into the side object.
3. T1: `GetSlotOrUndefined` loads `absent` and selects `undefined` (`aot/codegen/AOTSlotEmitter.cpp:397-424`).

The location held 5, then `"s"`. 02 2.10.5 ("the old or a new value") and M3 exclude `undefined`. 04 4.5 calls this "an old answer", which it is not.

**Proposal.** "`GetSlotOrUndefined` is unchanged on the path that finds a value. On the path that finds `absent` it loads the order word (after an acquire of the slot on ARM64) and leaves for the generic path if bit 27 or 28 is set. The mover sets the bits first (5.6 a), so a reader that sees `absent` sees them." `loadDeferringSlotOrFieldOutsideIt` has this form already.

### N6. M5 contradicts 2.10.6 and the unchanged `EliminateRedundantReads`

```js
r0 = o.x;   r1 = p.x;   r2 = o.x;        // p === o at run time; another thread stores o.x = 1 between r0 and r1
```

The classes of `EliminateRedundantReads` are keyed by (location, base). A read through another base clears nothing. So `r2` is forwarded from `r0`: `r1 = 1`, then `r2 = 0`. Two reads of one location in program order see the writes backwards, which M5 forbids, while 2.10.6 allows "perform a read once for several uses" and "move a read earlier or later" without this limit. 2.10.7 counts three strengthenings of the standard's model, and M5 is not among them.

**Proposal.** Weaken M5 to what the compiler gives: "A thread sees its own last write unless another thread has written since. Reads of one location **through one reference**, in program order, do not go backwards." Or pay for it: "a read of a shared location through a base that may alias ends the remembered reads of that location."

### Minor

1. **R1 10-12, ARM64 orders that no rule names.**
   (a) Bit 28 licenses the hidden word, and the two addresses do not depend on each other. 5.6 (a), step 1, releases the hidden word and then stores bit 28 plainly; a later plain store is not ordered behind a release store. C++ readers that go `hasExtraProperties()`, then the hidden word, without the StructureID can load 0. Rule: "bit 28 is stored with release; `extraProperties()` loads the order word with acquire."
   (b) 5.6 (g): the acquire belongs on the **first** load, the order word. An acquire on the second load orders nothing here. The fallback to the lock hides the error.
   (c) 07 E2 asks for the epoch compare "at every synchronizing operation with acquire meaning". The inline forms of 04 4.13 (`LockHold`), 4.6.6 (`AtomicSlot` load) and 09 MD1 do not list it. Without it a program without races uses a stale chain entry after an inline acquire.
2. **R1 5.1-5.** 5.6 (b) has no step for the object's StructureID and none for invalidations, though removing the name changes the side object's Structure (CO9, and the `Named` variant of `aot/object/AOTCompiledStructure.h:94`). While the old `Named` ID stays, a per-thread entry "name at offset k of the side object" reads the `undefined` that N2 of 06 stores there. Add steps 6 and 7 of (a).
3. **CO7 against 5.13.** The hidden word goes from none to a tuple of pending stars to the side object (`aot/object/AOTObjectExtraProperties.cpp:288-310, 624-630`): two changes, not "once". State CO7 as "none, or pending stars, then one side object".
4. **R1 H12-1.** "Generic copy under the source's lock" (5.12, step 1) contradicts L1 where the source has an accessor: spread and `Object.assign` call getters. The generic copy is a `[[Get]]` per key without a lock.
5. **5.4 against `fieldMayExistWithoutStorage`.** "If neither bit 27 nor bit 28 is set the property is absent" is wrong for such a field: absent bytes and a listed key read as `undefined` (`Object::read`, `aot/object/AOTObjectStorage.cpp:108-118`). Because the source decides by the key order whether or not there is storage, unpacked Extensions need no new case. The sentence of 5.5 ("not held" becomes "no Extension") should go, and 5.4 needs the third outcome.
6. **R2 H3.** 4.6.3 keeps "the 32-bit load of the low half" for dense Int32 arrays and says that `Dense` never removes the hole test. The hole is 0 in all 64 bits and the integer 0 has a zero low half, so a 32-bit load cannot tell them apart. A racing `pop` is read as 0, which nobody wrote (M3). An array that may be shared takes the 64-bit load.
7. **R1 H15.** `ClassPrototype::hasMember` writes `m_flags` on a read, in two steps: `HasCheckedThen`, then `DeclaresThen` (`aot/object/AOTClassPrototype.cpp:135-138`). A second thread that reads between them takes a class that declares `then` for not thenable. 5.10 drops the counters only. Compute both bits and publish them with one atomic `or`, or drop the cache in mt builds.
8. **`PutSlot [replacesValue]` needs an edge.** "If it is `absent` go to the generic path" cannot be said by an `operation`. Give it the row that `TryGetSlot` got: `terminator TryPutSlot (..) -> stored, empty ; reads writes(ObjectFields[key])`. Say also what `memoryAccessOf` makes of it, since store-to-load forwarding is legal on the `stored` edge only.
9. **R2 9.3-6.** "Only among the initializing stores .. As today": the phase merges any plain constant stores to one base in a block (`b3/B3MergeAdjacentConstantStores.cpp:82-135`) and knows no initialization. Either argue that a merged store is harmless (each slot lies whole inside it; fenced stores are excluded) and drop the restriction, or add the restriction as work.
10. **Names of rules.** "A rule has a number and is stated once" (README). F1 to F5 are fences in 04 4.10, and F1 to F22 are words in 07 7.5. L1 to L9 are differences in 02 2.9, and L1 to L5 are lock rules in 05 5.3. K1 to K7 are risks in 01 1.8, and K1, K2 are cache rules in 07 7.4. R1 to R6 in 04 4.2.3 collide with the names of the reports. N1 to N3, A1 to A5, H0, H1 collide with hazard numbers.
    The documents also cite `analysis/..`, `codegen/..`, `object/..` as "relative to `Source/JavaScriptCore/`"; the directories lie under `aot/`.

### Checked against the source, without a finding

- **Fixed classes of `EliminateRedundantReads`.** A class is fixed only with `stores` of `Never` or `AtMostOnce` **and** `initialization` of `StoredBeforeEveryRead` or `SetAtCreation` (`aot/ir/AOTMemoryLocationAccesses.cpp:132-151`), or as a "settling" variable whose source is a check that has passed (`aot/passes/AOTEliminateRedundantReads.cpp:231-312`). Both are facts about a value that is final once seen. They survive a synchronization if the initialization fact does (O1 to O3). `isUnchangedDuringActivation` is set only for "never stored" and "stored by the allocation".
- **`PromoteAggregateParameters`** cuts the callee at the last use of the parameter and moves that prefix, in its order, into a wrapper (`aot/passes/AOTVirtualObjectParameters.cpp:333-400`). No read crosses an operation.
- **`ConstructorFieldOrders`.** Its leak is "only the code that is followed here holds it" (`aot/valueflow/AOTFlowConstructionOrders.cpp:1217`). That is the unpublished state that O2 needs.
- **`AwaitInPlace`** applies to awaits of primitives only (`aot/select/AOTPromiseRules.cpp:307-350`). No reaction edge from another thread ends there.
- **`EnumeratorGetByVal`** reads by position from ordinary objects only; compiled objects go by name (`aot/codegen/AOTExpandedIterationEmitter.cpp:39-82`).
- **Nuked StructureIDs** decode to the old Structure (`runtime/StructureID.h:73-77`). C++ that meets a slim closure in the middle of its reification reads a valid Structure.
- **Lazy class objects** run no code of the program (`scope.assertNoException()`, `aot/object/AOTClassTable.cpp:151-171`) and claim upwards along the class hierarchy only. Two claims of Z2 cannot wait for each other.
- **Compare-and-swap on 1 and 2 bytes.** `OptionalBoolean` is the only representation below 4 bytes that has markers. The 2-byte representations have none and always take the first row of 4.6.2.
- **`LiteralString32`.** A read does not write the cell back into the slot (`aot/object/AOTSlotAccess.cpp`). The word is compared and swapped like any other.

## B.2 Run-time state and control (reports R4, R6)

Most severe first. Each proposal is the text of a rule.

### P1. R6 D-4, R4 H-22, N-1, N-2: a coroutine of a thread that has ended is resumed by another thread

**Kind.** Contradiction between J1 (09, 9.7; 02, 2.6) and G1 (09, 9.6). Memory unsafe.

G1's reason: a saved frame may hold "an allocator's address, a table base of a per-thread cache, or an address computed from `instanceGPR`". J1's last sentence, and 02, send a
reaction whose home has ended to the main thread. 02 also says that a pending `await` does not keep a thread alive. 09, 9.2, step 14 and 08, 8.14 unmap the view and free the context.

**(a) The fallback.**

1. Thread T runs `async function f() { await p; use(); }` and does not await `f()`. `p` is pending and Shareable. The frame is saved with `m_thread = T`; some word of it is `view(T) + k` or a `LocalAllocator*` of T.
2. T's function returns a value that is no promise. Its loop is empty. T completes, unmaps its view, frees its allocators.
3. The main thread settles `p`. Home T has ended, so the job goes to the main thread's inbox.
4. The main thread restores the frame (`aot/stubs/AOTStubsCoroutineResumption.cpp:603`) and jumps to the resume address. The first inline allocation, or the first access through the hoisted address, uses freed or unmapped memory.

**(b) The ID is reused.** `SavedFrame::m_thread`, `Generator::m_thread` and the reaction's field are 16-bit IDs. 9.2, step 14 gives the ID back to a free list. 9.7 speaks of "its ID's generation", which no cell stores.

1. Thread 5 makes a generator `g`, stores it in a shared variable, and ends.
2. A new thread gets ID 5, reads `g`, calls `g.next()`. The test of G1 (`m_thread == my ID`) passes.
3. The frame of the dead thread runs on the new thread with the dead thread's addresses.

The same for a reaction with home 5: it is posted to the new thread 5, which resumes a frame that it did not make.

**(c) Check, then post.** The settling thread maps the ID to a `ThreadContext*` and calls `post`. Nothing keeps the context alive between the two. And no step of 9.2 closes the inbox:
a task appended after step 11 has found the loop empty, and before step 14, is never run and never forwarded. That is a lost job, and for `asyncJoin`, `asyncHold`, `waitAsync` tickets a promise that never settles.

**Proposal.**

> **G2.** A coroutine whose thread has ended is dead. `next`, `return`, `throw` on it throw `ConcurrentAccessError`. A reaction whose context is a coroutine (`SavedFrame`, `JSAsyncGenerator`, an async module's record) and whose home has ended is dropped. Only reactions whose handler is a function go to the main thread. (02, 2.6: "If the registering thread has completed, a `then` handler runs on the main thread; an `await` of that thread never continues, as at the end of a process.")
>
> **T1 (IDs).** The ID of a thread that has ended is quarantined. It returns to the free list at the end of the first full collection that began after the thread was unregistered. In that collection the visitors of `SavedFrame`, `AOT::Generator`, `JSAsyncGenerator` and `JSPromiseReaction` replace a quarantined ID by `NoThread` (0xFFFF), which equals no thread's ID. So no live cell carries an ID that is given out again.
>
> **T2 (inbox).** `VM::m_threads` maps an ID to a reference-counted `ThreadContext`. `post(id, task)` takes the reference under `m_threadsLock`. A thread leaves its loop only by `closeInbox()`: under the inbox lock, if the inbox is empty set `closed`, else go on with the loop. `post` on a closed inbox returns false, and the caller applies G2.

The alternative is to bring forward the compiler rule of 13, section 13.5 ("no value derived from the view is live across an operation that suspends"). Then (a) is safe as written and only T1 and T2 remain.

### P2. R6 6.5-8, 6.5-7, 7.4-5, 7.5-2, H-9, H-24, R4 H-22: resume jobs that are queued without asking for the home

**Kind.** GAP. Memory unsafe. G1 says of async functions "Nothing to test". J1 names four places that settle (`fulfillPromise`, `rejectPromise`, `emitFulfillPromise`, `generateReturnAsyncResult`). The source has more ways to a resume job.

**(a) `await` of a thenable: no promise is involved.**

1. Thread T: `await { then(resolve) { new Thread(() => resolve(1)); } }`. `resumeWhenSettled` (`aot/runtime/AOTJobs.cpp:58`) ends in `resolveWithInternalMicrotask` (`runtime/JSPromise.cpp:1207`), which queues `PromiseResolveThenableJobWithInternalMicrotask` with T's `SavedFrame` as context.
2. The job, on T, makes two resolving functions that hold the task and the context, and calls `then`.
3. Thread U calls `resolve(1)`: `promiseResolvingFunctionResolveWithInternalMicrotask` (`runtime/JSPromise.cpp:940`), `resolveWithInternalMicrotask`, `fulfillWithInternalMicrotask`, `globalObject->queueMicrotask(CompiledAsyncFunctionResume, .., context)`.
4. By 9.7 `queueMicrotask` is "the caller's queue". U's job loop restores T's frame on U's stack while T is alive: U bumps T's free lists, writes T's caches. Type confusion follows from two threads in one allocator.

**(b) The hand-over through a promise.** G1 argues that "a call is synchronous, so callee and waiter are on one thread". That holds for `Continuation::Waiter`. The second hand-over takes the waiter out of the *promise's* reaction: `emitHandOverToWaiterOfPromise` (`aot/stubs/AOTStubsCoroutinePromises.cpp:350`) and, in C++, `tryScheduleImmediateResume` (`aot/runtime/AOTJobs.cpp:81`), which tests only `isSavedFrameOf(instance, waiter)`. An async function of T whose promise is awaited by U hands U's frame to T's adapter. The sentence "The stubs test 'home is me'" covers the stub. Nothing covers the C++ twin or `tryFulfillTakingInlineJob`.

**(c) Contexts that are no `SavedFrame`.** `emitFulfillPromise` takes three tasks (`:183-185`). For `AsyncGeneratorBodyCallNormal` and `AsyncGeneratorYieldAwaited` the slot holds the `JSAsyncGenerator` (`emitRunGeneratorJobWhenSettled`, `aot/stubs/AOTStubsCoroutineSuspension.cpp:710`). A compare of `SavedFrame::m_thread` reads bytes of another class. J1's table files them under "other inline microtasks .. used only on Confined promises", but the stub that awaits inside an async generator gets promises of any site.

**(d) No last line of defence.** `resumeAsyncFunction` does `uncheckedDowncast<SavedFrame>(context)` and enters (`aot/runtime/AOTAsyncResumption.cpp:150-161`). The job loop of `EnterSavedFrame` tests the StructureID only.

**Proposal.**

> **J2.** The home of a job is a property of its context, not of the promise. `homeOf(task, context)`: `SavedFrame::m_thread`; `JSAsyncGenerator`'s thread field; the thread field of a `JSGenerator` that runs an interpreted async function; the owner of a module record. Every enqueue of an `InternalMicrotask` of the resuming set (`CompiledAsyncFunctionResume[AfterTurn]`, `AsyncGeneratorBodyCall*`, `AsyncGeneratorYieldAwaited`, `AsyncGeneratorAwaitReturn`, `AsyncGeneratorDriverResume`, `AsyncFunctionResume`, `AsyncModuleExecutionResume`, `PromiseResolveThenableJobWithInternalMicrotask[Fast]`, `PromiseAllResolveJob`) goes through `enqueueAtHome(task, context, ..)`, which compares and posts. `JSGlobalObject::queueMicrotask` asserts that it is not given one of these.
>
> **J3.** Every place that takes a waiter out of a promise instead of queueing a job (`emitHandOverToWaiterOfPromise`, `tryScheduleImmediateResume`, `tryFulfillTakingInlineJob`) first compares `waiter->m_thread` with the running thread and declines otherwise.
>
> **G3.** The three places that restore a frame (`StepGenerator`, `RunAsyncGeneratorStep`, `EnterSavedFrame`) compare `m_thread` with the view's ID before they copy a word. A mismatch in `EnterSavedFrame` reposts the job (release builds) and fails validation (`FrameRestoredOnOtherThread`). Two instructions per resumption. G1's row "Nothing to test" is dropped.

### P3. R4 H-1, H-23, 1.1-5, 1.1-8, 1.2-6, 1.2-13, 1.2-14, 1.2-17, 1.2-21, 1.2-25, 7.14, 10.3-5, R6 H-9, H-10: the canonical view is the main thread's view, and C++ reaches private words through it

**Kind.** GAP. Memory unsafe. 7.3.1 makes `realm->m_aotInstance` the main thread's view. V1 speaks of addresses in *shared* regions that are stored, and of operations that get the `Instance*` from compiled code. It says nothing about C++ that finds the Instance by itself. That is most of the run time:

| Way | Uses outside tests | Examples that touch private words |
| --- | --- | --- |
| `Instance::of(realm)` (`aot/runtime/AOTInstance.h:255`) | 65 | `resumeAsyncFunction`: `instance.coroutines.nextEntryEndsJob = ..` (`aot/runtime/AOTAsyncResumption.cpp:153-157`); `willEnterAsyncGeneratorBody` reads `stepInProgress`, a pointer into a stack (`aot/runtime/AOTJobs.cpp:193-197`); `Generator::resume` (`aot/runtime/AOTGenerator.cpp:111`) |
| `State::instance` (`aot/runtime/AOTState.h:105`) | 177 | `createArrayCell` allocates from `state.instance.prefix().arrayAllocator` (`aot/runtime/AOTDataBuilder.cpp:89`); `shouldFile` takes `missIsDueForFiling` (`aot/runtime/AOTPropertyCaches.cpp:669`); allocation site records (`aot/object/AOTAllocationSites.cpp:169-211`); key lists, enumerators, prototype transitions |
| `Instance& m_instance` members (the count is of all uses; among the holders are objects that `State` owns, and the cell `ModuleRecord`) | 253 | `JSONSource` (all of `json`), `CoroutineTable::visitRoots`, `Modules::evaluateByCallingBody` (`evaluatingMark`), `ProgramStrings::tryCreateHeader` (the bump pointer), `StringKeyCache` |

**Interleaving.** Thread U runs a job. `resumeAsyncFunction` sets `nextEntryEndsJob` in the *main* thread's view and enters the adapter, which reads U's view: the flag is clear, U's job loop is off (slow, harmless). The main thread's next entry of `EnterSavedFrame`, in the middle of a job, finds the flag set and starts to drain its queue inside that job (R6 H-10, Unsafe). With `createArrayCell`, U and the main thread bump one free list: two objects in one cell.

**Three more holes in V1.**

1. *Comparison.* `Instance::of(x->structure()->realm()) != &instance`, with `instance` from compiled code, is true for every object on a spawned thread: `functionInInstance` (`aot/object/AOTThisAllocation.cpp:39`), `canReadPrototypeDirectly` (`aot/runtime/AOTClassOperations.cpp:39`), `readModuleNamespaceExport` (`aot/runtime/AOTPropertyCachesLeafOperations.cpp:170`). Own objects are taken for objects of a foreign realm.
2. *Subtraction.* `RememberedSlot::set(set, value)` calls `record(this)`, which computes `this - m_tableStart` and indexes `m_cards` with an `ASSERT` only (`heap/RememberedSlot.h:47-51`). V1 allows an operation to use the caller's view "for loads and stores". `instance.stringCell(n).set(..)` through a caller's view is such a store, and writes a card far out of bounds, because the set's base is canonical. `SiteCaches::indexOf(site)` has the reverse problem: the site's address is the caller's, `m_slots` is `state.instance.siteCaches()` (`aot/runtime/AOTSiteCaches.cpp:49`).
3. *Pointers of the struct into the mapping.* "Constants .. copied from the canonical view" is right for a pointer into a shared region (`prefix.builtinCells`, `builtins.locationStates`, `builtins.hostValues`). It is wrong for one into a private region: `runtime.counters.globalBindings = instance.tableAt<GlobalBinding>(offsetOfGlobalBindings)` (`aot/runtime/AOTInstance.cpp:332-334`). Copied, every thread's C++ files its bindings in the main thread's records, while its compiled code reads its own.

**Proposal.**

> **V3.** The canonical view is no thread's view. The main thread runs on a view of its own. In the canonical view the private regions hold what "Constants" of 7.3.3 lists and nothing else, and are mapped read-only once the realm is set up: a store through it faults on the first test run.
>
> **V4.** C++ names the two things by two types. `SharedInstance&` (from `realm->m_aotInstance`, `State::shared()`) has accessors for shared regions and constants only. `Instance&` is the running thread's view: `Instance::of(realm)` returns `ThreadContext::current()->viewOf(realm)`. `State::instance` and every `Instance& m_instance` of an object that more than one thread can reach are removed. An object that keeps an `Instance&` is per thread (`SiteCaches`, `PropertyCaches`, `GlobalBindings`, `JSONRuntime`, `StringKeyCache`, `ChainDependents`, the per-thread half of `StructureTable` and `KeyOrderTable`) and hangs off a new `ThreadState`, which `instance.state` of a view points at. `ThreadState::shared` points at the realm's `State`.
>
> **V5.** Two `Instance*` are never compared. "Of this program" is `structure->realm() == instance.globalObject`.
>
> **V6.** An address in a region becomes an offset by subtracting the base of the view it came from, at the place where it enters C++. `RememberedSlotSet::record`, `SiteCaches::indexOf`, and the registrations of `CacheRegistry` take offsets, or spans of the registry's own view.
>
> **V7.** When a view is made, a pointer member that points into the mapping is rebased: into the new view if the region is private, left canonical if it is shared. The list of such members is part of the runtime facts.

### P4. R4 3.5, H-7, N-3: a thread that had no heap access during a stop runs neither `didResume()` nor E2

**Kind.** Partly. Memory unsafe. E4 rests on N2 ("not given out again before the end of the next collection") and on SW2. Both are bounded by a collection. E2 promises a flush "when it goes on after having parked". 08, 8.3 calls `didResume()` in `park()` only. `acquireAccess()` has no such call, and a stop does not wait for a thread in `NoAccess`.

The commonest state of a thread is `NoAccess`: it waits in its event loop (H1). A timer or I/O callback that wakes it is no synchronizing operation.

1. T files `(S, "a") -> Value, holder H, out of line, index k`, chain dependent. T goes to sleep in its loop.
2. U runs `delete H.a`: the slot gets `undefined`, `k` is quarantined, the epoch moves.
3. A full collection runs. T is `NoAccess`, so nobody waits for it and nothing of it is flushed. `k` becomes free.
4. U runs `Object.defineProperty(H, "b", { get })`: offset `k` now holds a `GetterSetter`. Or `H` is flattened in a safepoint operation and its out-of-line storage shrinks (`flattenDictionaryStructure` renumbers and shifts the butterfly).
5. A timer wakes T. `acquireAccess()` returns. `o.a` hits the entry: `loadFromValueSlot` returns the `GetterSetter` cell to the program as a value, or reads past the end of the new storage.

The same hole in 08, 8.12: "clearing VM caches .. each thread clears its own in `didResume()`". T keeps `HasOwnPropertyCache`, `MegamorphicCache` and string caches keyed by StructureIDs and `uid` addresses that the collection has recycled.

A free-name site with a cached *store* ("compares no Structure") writes through such an entry: it overwrites the `GetterSetter` of step 4 with any value, and other threads' `Getter` entries then dereference it.

**Proposal.**

> **H2.** `Heap::m_stopCount` is incremented by the conductor at the end of every stop, before `cancelStop`. `acquireAccess()` ends with: `if (m_stopCountSeen != heap.m_stopCount) didResume()`. `park()` does the same. So the first thing a thread does with heap access after any stop is E2 and the clearing of 8.12.
>
> **E2, first case**, reads: "whenever it gains heap access after a stop (H2)".

An alternative for the caches of the AOT run time alone: the conductor drops the chain-dependent entries of every thread whose `m_cacheEpochSeen` is behind, in the End phase, with the `removeDeadEntries` that it already runs "for every thread's registry".

### P5. R4 3.5, H-7: E4 does not hold for accessor slots, because N2 stores `undefined` over the `GetterSetter`

**Kind.** Partly. Memory unsafe (a fault at a site that is not registered ends the process: 09, 9.5; 02, M2, M11).

The handlers of `Getter`, `Setter`, `FieldOrInherited` with `inheritsGetter`, and `FieldOrInheritedSetter` load the slot and dereference it with no test of its type: `emitGetterCall` (`aot/stubs/AOTStubsPropertyReadHandlers.cpp:145-150`), then `loadPtr [getterSetter + offsetOfGetter]` (`aot/stubs/AOTStubsPropertyAccessors.cpp:37`, setter `:96`).

N2: "Before the table is edited the slot gets `undefined`". N3 keeps the old value only for a *redefinition*.

1. T has loaded the StructureID of `o` (own accessor `x`), or holds an entry for an accessor inherited from `H`.
2. U runs `delete H.x`: `H[k] = undefined`.
3. T's handler loads `0xa` and loads from `0xa + 8`.

No staleness across a synchronization is needed: for an own accessor the window is N1's row "old StructureID, new storage". C++ has the same shape: attributes from an old table entry, then `jsCast<GetterSetter*>(getDirect(offset))`.

**Proposal.**

> **N2, second sentence:** "Before the table is edited, the slot of a *data* property gets `undefined` (release). The slot of an accessor or custom-value property gets the VM's immortal `emptyGetterSetter`, whose getter is null and whose setter is the null setter, so that a reader with an old answer reads `undefined` and a writer does what a store to an accessor without a setter does."
>
> **E4** lists the kinds: "`Value`, `FarValue`, `Field*`, `ExtraProperty`: N2 and N3. `Getter`, `Setter`, `ExtraGetter`, `ExtraSetter`, `FieldOrInherited*`: a slot that has held a `GetterSetter` holds a `GetterSetter` until its offset leaves quarantine. `Transition`, `ExtraPropertyAddition`, `FieldCreation`: the handler takes the object lock and compares the StructureID again (06, 6.4; 05, 5.6 e, f). `CustomGetter`, `CustomSetter`: the record table is the thread's own. `Constant`: K2. `Absent`, lengths: no memory is touched."

### P6. R4 1.2-4, F12, R6 8.1-5, N-4: thread start and thread end do work that the collector sees, without heap access

**Kind.** GAP. Memory unsafe. 08, 8.14 orders the start as "register; make the view; `acquireAccess()`" and the end as "`releaseAccess()`; unregister; unmap the view; free the context". A stop does not wait for a thread in `NoAccess`. So all of "make the view" and all of the teardown can run inside a stop.

**(a) Allocator lists.** `BlockDirectory::stopAllocating`, `prepareForAllocation`, `resumeAllocating` walk `m_localAllocators` without `m_localAllocatorsLock` (`heap/BlockDirectory.cpp:232, 251, 298`). Only append and remove take it (`heap/LocalAllocator.cpp:53, 82`). 08, 8.8 relies on "they run while all threads are parked". A view needs allocators before compiled code runs (`slimClosureAllocators`: "They are there before any compiled code runs", `prefix.arrayAllocator`, `ropeStringAllocator`, `stringAllocator`). A starting thread appends, and an ending thread removes and frees, while the conductor walks the list.

**(b) Collector words of a new view.**

1. N reads `heap.barrierThreshold()` and `mutatorShouldBeFenced()` for its view (today `initializeCollectorWords`, `aot/runtime/AOTHeapHooks.cpp:100`).
2. The conductor begins marking and writes the new values into every *listed* view (`didChangeBarrierThreshold`, under `instancesLock`).
3. N appends its view to the list, then acquires access after the stop.
4. N runs a whole marking phase with the old threshold and without fences: barriers are missed.

**(c) The stack of an ending thread.** 08, 8.13 scans "in place" the range in `m_parkedState` of each `NoAccess` thread. After `releaseAccess()` N unregisters, returns, and the OS unmaps the stack of a detached thread under the scan. The same for N's registry, view, inbox, microtask queue and mark stack, which the conductor visits "for every thread".

**(d) What stays behind.** `LoopPollPages::pages` and the `StackManager`'s mirror list hold addresses inside the view. No step removes them. The next `setLoopPollsEnabled` calls `mprotect(PROT_NONE)` on whatever is mapped there by then.

**Proposal.**

> **T3 (start).** `ThreadContext`; register with `m_access = NoAccess`; `acquireAccess()`; then, with access: allocators, view, registry. The collector words of the view are read, and the view is appended, in one critical section of `instancesLock`.
>
> **T4 (end).** With access: drain; hooks; `stopAllocatingForGood()`; destroy the allocators; remove the poll page, the mirror, the queue, the registry and the view from their lists; merge mark stack, log, deferred frees. Then `unregister()`: under `m_threadsLock`, if `m_stopRequested`, unlock, wait as `NoAccess`, try again; else remove the context from `m_threads` and set `NoAccess` in the same critical section. After it the thread touches no cell and the collector knows nothing of it. Unmapping and freeing follow.

`requestStop()` stores the flag before it takes `m_threadsLock`, so a thread either leaves before the conductor has read the list, or sees the flag and stays until the stop is over.

### P7. R4 5.6, 1.3-5, 1.3-7, 7.6, 7.19, H-18: `RememberedSlotSet::record` on the shared sets still appends to a `Vector`

**Kind.** Partly. Memory unsafe. 07, 7.4: "`record` (a `Vector` append) is then single-threaded again". 08, B4: "the shared sets (statics, string cells) are those whose 'cards are written without record'".

`setCardsAreWrittenWithoutRecord()` only clears `m_listsEveryWrittenCard`. `record()` does not look at it: it stores the card and appends to `m_writtenCards` as long as the list is short (`heap/RememberedSlot.h:47-59`). C++ calls `record()` on the statics' set from any thread and with no lock: `ProgramStatics::writeVariable` -> `didStoreRootInStatics` (`aot/runtime/AOTProgramStatics.cpp:135`, `.h:40`), direct `eval` (`aot/runtime/AOTDirectEvalOperations.cpp:475`), `setCell` for RegExp literal cells (`aot/runtime/AOTRegExpOperations.cpp:338`) and for shared array storage (`aot/runtime/AOTDataBuilder.cpp:516`).

Two threads in `Vector::append`: both write one slot, or one writes into a buffer that the other's growth has freed.

Other sets stay shared because their tables are truth: closure cells (`ClosureTable::m_writtenRegions`), wide string cells, `StructureTable`, `KeyOrderTable`. Z1 publishes "outside any lock" by compare-and-swap and does not mention the card at all. Today's `RememberedSlot::set` does both.

**Proposal.**

> **K3.** A `RememberedSlotSet` over a shared table has no list: `record()` returns after the byte store when `m_cardsAreWrittenWithoutRecord` is set, and every shared set has it set. (The collector then reads all cards of these sets at each stop, eight at a time, as it does for the statics today.)
>
> **Z1, added:** "Where the slot is a root found by cards, the winner stores the card after the compare-and-swap and before its next safepoint."

### P8. R6 H-23, H-25: async stack traces read pending promises and other threads' frames without the promise's lock

**Kind.** Partly. Memory unsafe. PR3 gives a lock-free reader for *settled* promises. G1 gives `SavedFrame` one *writer*. Neither covers a reader on another thread.

`appendDirectAsyncCallers` (`aot/runtime/AOTAsyncStackTrace.cpp:106-143`), reached from every `Error` that captures a stack with async frames, walks from the running coroutine: `promise->flags()`, then `promise->inlineReactionContext()` (two loads, no lock), then `findSuspendedAsyncFunction(context)`, then that frame's `waiter()` or `promise()`, which `uncheckedDowncast` `m_promiseOrWaiter` by the byte `m_continuation`. `promisesSettledBy` (`:75-99`) and `JSPromise::asyncStackTraceContext` do the same.

1. U's async function returned promise `P`. T awaits `P`, so `P`'s reaction context is T's frame `W`.
2. U throws and captures a stack. It reads `P`'s flags (resume reaction), then the slot: `W`.
3. T is resumed for another reason, finishes a call, and runs `ensurePromise` on `W`: `m_promiseOrWaiter = promise`, then `m_continuation = Promise` (`aot/runtime/AOTSavedFrame.cpp:256-257`).
4. U reads `m_continuation == Waiter` (old) and `m_promiseOrWaiter` (new): a `JSPromise` used as a `SavedFrame`. It reads `m_resumeAddress` from the promise's words and looks it up in the image.

**Proposal.**

> **PR8.** A reader of a promise that is not settled takes the busy bit, copies both words, and releases it. There is no lock-free reader of a pending promise.
>
> **G4.** Fields of a `SavedFrame`, `Generator` or `JSAsyncGenerator` other than `m_thread` are read only by its thread and by the collector. A stack trace stops at a frame whose `m_thread` is not the running thread and appends one frame `at async <thread n>`.

### P9. R4 7.2, H-9, 1.2-12: memory that belongs to a thread and is published

**Kind.** Partly. Memory unsafe. 07, 7.6 and 06, 6.7 give each thread "its own arena of run-time headers". A header is the `StringImpl` of a string constant's cell, and the cell is published in a *shared* string cell by Z1 and lives as long as the program. 09, 9.2, step 14 and 08, 8.14 "unmap the view; free the context".

1. T runs a site first, makes the header in its arena, makes the cell, wins the compare-and-swap.
2. T ends. If the arena goes with the context, every other thread's `"literal"` has a dangling `StringImpl*`.

The sizing argument also changes. Today the arena has room for "at most one header per site" (`aot/runtime/AOTProgramStrings.cpp:61-68`), and only the first Instance of the process gets one. N arenas of that size are N times the memory. Smaller ones run out, which is harmless: stub and C++ compare with the end and fall back.

**Proposal.**

> **T5.** Nothing that a thread allocates outside the garbage-collected heap may be named by shared state, unless it is never freed or is reference counted. String header arenas are chunks of one process-wide reservation. A thread takes a chunk by `fetch_add`, bumps inside it, and at its end returns the unused rest of the chunk to a free list. Used headers are never freed.

### P10. R4 H-8, N-5: an async module's completion runs other modules' bodies on the completing thread

**Kind.** Partly. Memory unsafe through `MainOnly`. Rare.

MD6: "Its record belongs to the thread that started it, and its continuation runs there". The ECMAScript algorithm, which the run time uses as it is (`aot/runtime/AOTModuleEvaluation.cpp:674-722`), executes waiting *parents* inside the child's completion: `asyncExecutionFulfilled` gathers the ancestors and calls `m->execute(globalObject)` (`runtime/CyclicModuleRecord.cpp:868-899`).

1. Spawned thread T: `import("./m.js")`. `m` has a top-level `await`. It is `EvaluatingAsync`, owner T, and the lock is free.
2. Main thread: `import("./n.js")`, and `n` imports `m` statically. Under the lock `n` gets `pendingAsyncDependencies = 1` and is appended to `m`'s parents.
3. `m`'s `await` settles. The continuation runs on T (J1). `asyncExecutionFulfilled` runs **`n`'s body on T**.
4. Flow saw `n` imported from MainOnly code only (04, 4.2.2, row `import()`), so `n`'s body and what only it calls are `MainOnly`: today's sequences, no fences, no owner tests, on objects that the main thread uses at the same time.

Two more things are open. MD6 does not say that the continuation takes the evaluation lock again, while MD7 says that the containers are touched under it only. And if T completes while `m` is pending (a pending `await` keeps no thread alive), a thread that blocks in `require("./m.js")` by MD6's last sentence blocks for ever.

**Proposal.**

> **MD8.** Every step of the async half (`asyncExecutionFulfilled`, `asyncExecutionRejected`, `executeAsync`) runs under the evaluation lock. A parent in `execList` whose owner is another thread is not executed here: it is posted to its owner, which executes it under the lock.
>
> **MD9.** A module in `EvaluatingAsync` holds a keep-alive on its owner's loop.
>
> 04, 4.2.2 gets the row: "a module that has a dependency with a top-level `await` runs on the thread that first asked for *it*".

### P11. R4 H-8, 1.3-4, 7.9, N-6: the evaluation lock is held over the whole body of the entry module

**Kind.** Partly. Deadlock in the program of 01, section 1.1.

MD3 holds `m_evaluationLock` "from the start of an outermost evaluation .. to its end, while module bodies run". The entry module's body *is* the program:

```ts
const workers = files.map(file => new Thread(() => { .. }));
for (const w of workers) w.join();          // top level of the entry module: the main thread owns the lock
```

A worker needs the lock:

- for `import()` and `require()` of anything not yet evaluated;
- by MD7, for the first namespace object, module object, `require` function or `import.meta` of a module that **is** evaluated;
- for each of **Bun's own modules** at its first use: `evaluateBuiltinModule` is `m_state.modules->require(module)` (`aot/runtime/AOTBuiltinModules.cpp:48-67`), and `BuiltinCells::cell` "may load a module". Bun's natives and library code load internal modules lazily, inside calls that look harmless.

The design sees the case and answers with an `Error` in the waiter. So a native or a library call that is the first to need an internal module throws "deadlock" in a worker, depending on what the main thread happened to touch before. And only `join` chains are detected: a main thread in `cond.wait`, `Atomics.wait` or `lock.hold` at top level hangs without a word.

A program that writes `await Promise.all(workers.map(w => w.asyncJoin()))` at top level is fine (MD6). The synchronous form is the one that the documents show.

**Proposal.** Keep "one evaluation of a module", drop "one evaluation at a time".

> **MD3'.** `Modules::m_lock` is a leaf lock in the sense of 07, 7.7. No JavaScript runs under it. It protects the containers (MD7), the status bytes other than `Evaluated`, and an owner word per module.
>
> **MD3''. A claim takes everything it needs, or nothing.** `beginEvaluation(root)`, under the lock, walks the modules that are reachable from `root` and not `Evaluated`. If one of them is owned by another thread it releases the lock, waits (heap access released) until that thread's outermost evaluation has ended, and starts again, owning nothing meanwhile. Otherwise it marks all of them `Evaluating(me)`, releases the lock, and runs the single-threaded algorithm on its own set. `Evaluating(me)` is "my own cycle", as today.
>
> A thread never waits while it owns modules that it claimed in the same call, so two claims cannot wait for each other. A thread that waits inside a body, for a set that a nested `require` needs, can form a cycle with the owner of that set only if the owner waits for this thread: the wait-for graph (claims, `join`) is searched at each such wait, and the waiter gets the `Error`.
>
> **MD7'.** Namespace objects, module objects, `require`, `import.meta`: Z1 under `m_lock`.

With this the worker of the example evaluates an internal module by itself, because it is not in the set of the entry module.

### P12. N-7: `process.exit` on a spawned thread against a main thread that is blocked

**Kind.** Contradiction between 09, 9.10 and 9.11. Deadlock.

9.11: "`process.exit(code)` on any thread: posts to the main thread if called elsewhere, and blocks." 09, 9.2 and 02, 2.6: no job and no task runs inside `join`, `hold`, `wait`.

1. Main: `w.join()`.
2. `w`: `process.exit(1)`. It posts to the main inbox and blocks.
3. Main waits for `w`. `w` waits for main.

9.10 lists `process.exit` as a termination that is "fanned to each thread's trap word" and wakes blocked threads. The two rows describe different protocols.

**Proposal.**

> **X1.** `process.exit` on a spawned thread sets `VM::m_exitRequested = code` and fires the trap `NeedExit` on the main thread. Each blocking primitive checks its thread's trap word at each wake-up (9.13). On `NeedExit` the main thread leaves the primitive with the termination exception, unwinds to its loop, runs the `exit` handlers and ends the process. The caller blocks with heap access released. If the main thread does not get there within `BUN_JSC_exitGraceMs`, the caller runs `_exit(code)` itself, without handlers.

### P13. R4 5.2, 1.3-3, F1, H-5, 7.20: dependents whose subject is shared state are kept in one thread's `ChainDependents`

**Kind.** Partly. Wrong values, for as long as another thread pleases. 7.7: "`ChainDependents` | per thread: no lock". E1: the writer "drops the dependents in **its own** caches".

Two of the owners do not own a cache of the thread:

- `MemberGuardWatchpoints::watchOtherObjects` registers a **guard** under objects that are no class holders (`aot/object/AOTMemberGuards.cpp:1701-1704`). `dropChainDependent` ends the guard (`:1723-1733`). Guard bits are truth in the shared statics.
- `StructureStateOwner` resets `DefinitelyNonThenableState` and the cached special properties in the **Structure** (`aot/runtime/AOTChainDependents.cpp:55-66`). The await and fulfil stubs test that state (`emitNonThenableCheck`, `aot/stubs/AOTStubsCoroutinePromises.cpp:101-110`).

1. Thread A arms the guard `NotHidden(C, "m")`. The dependents under the objects between an instance and `C.prototype` go into A's map. The bit is set for all threads.
2. Thread B puts an object with its own `m` into that chain, or adds `m` to one of those objects. B's map has no such dependent. `didChangeClassHolder` does not apply: the object holds no class. B bumps the epoch.
3. **B itself** calls `x.m()` in compiled code. The guard bit is still set. B gets the old method after its own change: 02, M5 ("a thread always sees its own last write").
4. The bit falls when A next runs E2. If A sleeps in its loop, or is blocked for good, it never falls, and a program without races that synchronizes B with C gets the old method on C: 02, M1.

The same with `then`: A computes `NonThenable` for Structure S, B adds `then` to a prototype, and B's own `await obj` skips it.

**Proposal.**

> **E5.** A dependent whose drop changes shared state (guards, Structure state, any later owner of that kind) is registered in the realm's `SharedChainDependents`, a table of 7.7 with a lock. E1's writer drops from it precisely, after the change and before the `fetch_add`. Per-thread maps hold only dependents whose owner is a cache of that thread. `ChainDependents::addOwner` takes the kind of the owner, and a debug build asserts it.

### P14. R4 3.8-3, F8, 1.2-6: the two name bit sets are truth

**Kind.** Partly: 7.4 makes "`propertyCaches.*` words of the view point at the thread's tables", and names neither set. Wrong values for ever, also in programs without races.

`branchUnlessNameIsAbsent` (`aot/stubs/AOTStubsPropertyProbes.cpp:245-262`) answers "absent" from a flag of the Structure (`HasOnlyFieldsBelowObjectPrototype`) and a clear bit in `namesOfObjectPrototypes`. No generation and no entry is involved, so E2 ("drops every chain-dependent entry .. bumping its generations") does not reach it. The store side tests the same set (`aot/stubs/AOTStubsKnownFieldStores.cpp:57`).

1. A: `Object.prototype.tag = 1; lock.hold(..)`. `didChangeObjectPrototype` sets the bit in A's set.
2. B acquires the lock, runs E2, reads `point.tag` on an object with a layout. B's bit is clear: `undefined`. For ever.

`namesChangedOnChains` is harmless if E2 bumps **both** generations, since filters are then made anew. E2 says "its generations".

**Proposal.**

> **K4.** `namesOfObjectPrototypes` and `namesChangedOnChains` are shared, one per VM as today. They are among the "shared words" of E1: the writer sets bits by `fetch_or` after the change and before the `fetch_add`. Each view's word points at the one array. E2 bumps `absenceFilterGeneration` and `absenceFilterGenerationForStores`.

### P15. R4 H-7, N-8: inline synchronizing operations do not run E2

**Kind.** Partly. Wrong values in programs without races (02, M1).

E2 asks for the compare "at every synchronizing operation with acquire meaning". 04 emits several of them in line, and no sequence there loads the epoch: `AtomicSlot` load (4.6.6), the inline row of `Atomics` on typed arrays (4.4), the fast path of `LockHold` (4.13), MD1's load-acquire of the status byte (09, 9.9).

1. A: `C.prototype.m = f2; Atomics.store(o, "ready", 1)`.
2. B: `while (!Atomics.load(o, "ready")); x.m()`, with `x.m` by name. `AtomicSlot` is a fenced load. B's site cache still holds `f1`.

Guards are not affected: they end at `synchronizes` and are tested again on shared bits. The per-thread tables are.

MD1: a module that extends `Object.prototype` is evaluated by A. B finds `Evaluated` in line and goes on with an "absent" entry from before. The module edge of 02, 2.10.2 is an acquire.

**Proposal.**

> **E2a.** Every inline sequence that 04 declares `synchronizes` with acquire meaning ends with `cmp [view + cacheEpochSeen], [cacheEpochAddress]; jne slow`: a load of a line that is read-mostly, a compare, a branch that is never taken. `slow` is `operationAOTDidSynchronize`, which runs E2 and is declared `collects`. The view holds the address of `VM::m_cacheEpoch` as a constant.

### P16. R4 H-7, N-9: E3 misses a change of an object that does not report yet

**Kind.** Partly. A stale entry that no later E2 drops.

E1 binds writers whose object reports, that is, whose Structure `mayBePrototype()`. `PropertyCaches::cache` gets an answer that was computed earlier, and only then calls `ensureChangesAreReported` (`aot/runtime/AOTPropertyCaches.cpp:572`, `:505-514`).

1. F reads the epoch (E3), walks, and finds `p` absent on `X`. `X` does not report yet.
2. W adds `p` to `X`. No report, no epoch.
3. F marks `X` as reporting, files "absent", registers the dependent, reads the epoch: unchanged. The entry stays.
4. Nothing ever invalidates it. After any number of synchronizations F reads `undefined`: beyond what 02, 2.10.5 allows ("until the reader's next synchronization operation").

**Proposal.**

> **E3.** "A fill reads the epoch; makes every object that the answer depends on report (under their object locks); **then** checks the answer: it compares the StructureID of each of these objects with the one that the walk saw, and starts over if one differs; files; registers; reads the epoch again."

### P17. R4 F17, F18, 7.7, H-11: Z2 has no failure path, and its claims are outside the lock order

**Kind.** Partly.

| # | Hole | Consequence |
| --- | --- | --- |
| a | The winner's creation throws (a static block, a computed key, out of memory). Z2 says only "the winner creates and stores the result" | the word stays `Claimed(t)`; every later toucher waits for ever |
| b | The winner is terminated (9.10) inside the creation | the same. And after T's ID is reused the new bearer finds `Claimed(itself)`, "is in a cycle", and goes on with a half-made class: memory unsafe |
| c | "Z2 with the variable's absent encoding **and a claim marker**" (row `DeferInitializers`). Inline readers test the variable for `absent` only | if the marker lies in the variable's own slot it is decoded as a value of the variable's type |
| d | Claims appear nowhere in 08, 8.15. A claim is held while JavaScript runs | T1 owns the modules lock and touches class `C`, which T2 has claimed. T2's static block needs a module. Neither is blocked in `join`, so the detector of 9.9 does not see it |
| e | `forgetClosure`, `forgetDefinition` reset a word that Z1 fills from 0 | a creator that began before the reset publishes a closure of the old evaluation after it (ABA on 0) |

**Proposal.**

> **Z2, added.** (1) The claim word is a word of its own, in the shared statics. The published slot keeps its "not made" encoding until the winner's release store. (2) The claim is released by a scope object: if the creation ends by an exception, termination included, the word goes back to 0 and the waiters are woken. Each tries again and meets the same exception, or succeeds. (3) A waiter registers the edge "thread waits for thread" in the wait-for graph that `join` and the module claims use (P11). A cycle throws in the waiter. (4) A word that can be reset carries a generation in its claim word. The winner publishes only if the generation is the one it claimed.

### P18. R6 7.2-1, 7.2-5, H-25, H-26: details of PR1 to PR7

**Kind.** Partly. Wrong values.

**(a) PR4 against PR1.** PR1 ends with a plain `store(m_packed, new)`, with `new` computed from `old`. PR4's atomic `or` does not look at the busy bit.
T1: compare-and-swap to `old | busy`. T2: `markAsHandled()`, an `or` with `isHandled`. T1: `store(new)`. The flag is lost, and a handled rejection is reported as unhandled. *Rule:* "PR4. `isHandled` is set by a compare-and-swap loop that does not proceed while the busy bit is set. On a settled promise, where no transition can be under way, a plain atomic `or` will do."

**(b) PR6.** "claimed by compare-and-swap on the first of the two". In the source the first store is to `callee` (`runtime/JSPromise.cpp:891, 910`), which is the resolve function in one thread and the reject function in the other: two words, and both swaps succeed. With `resolve(thenable)` on T1 the promise stays pending, so T2's `reject(e)` settles a promise that is already locked in. *Rule:* "PR6. The record is one word: the `ResolvingOther` field of the **resolve** function of the pair. Both functions claim by a compare-and-swap on that word."

**(c) `isFirstResolvingFunctionCalled`.** `resolve`, `reject`, `fulfill`, `pipeFrom` test the flag, set it, and then run code that can call `then` (`runtime/JSPromise.cpp:184-215`). No rule names it. *Rule:* "PR9. The flag is claimed by `fetch_or`. Whoever finds it set returns. `fulfillPromise` and `rejectPromise` return without effect if the word they lock is not `Pending`" (today an `ASSERT`).

**(d) Which promises are Confined, at run time.** "Used only on Confined promises" is a fact about sites. C++ (`performPromiseThen`, Bun's natives) gets promises of all sites, and no bit of the cell tells them apart: flag bit 15 was the last free one. *Rule:* "PR10. C++ and every stub whose operand's type admits a Shareable site follow PR1 to PR9 and never write the inline handler form. They must read it: such a promise is Confined, so its home is the running thread."

**(e) `Promise.all`.** With J1 all `PromiseAllResolveJob`s of one call run on one thread, which covers the stub (`aot/stubs/AOTStubsCoroutinePromises.cpp:215-261`). The resolve-element *functions*, handed to a foreign thenable's `then`, can be called by any thread. The count is a plain decrement. *Rule:* a row "`JSPromiseCombinatorsGlobalContext`, `JSPromiseCombinatorsContext`: D1" in 06, 6.10.

### P19. N-10: the lock order of 08, 8.15 contradicts MD3

**Kind.** Contradiction. The debug build's rank check fails on the first program.

8.15 places the modules lock *below* `Heap::m_conductorLock` and `VM::m_threadsLock`: "A thread that holds a lock takes only locks further down". MD3: "It is the one lock of the system under which JavaScript runs". JavaScript under it calls `new Thread` (9.2, step 2: `m_threadsLock`), grows a shared array (`runSafepointOperation`: `m_conductorLock`), allocates.

No real cycle follows, since neither the conductor nor thread registration takes the modules lock. The order is written the wrong way round. Also absent from the list: the inbox lock of 9.7, the internal locks of `Lock` and `Condition` (9.13), the claim words of Z2.

**Proposal.** With P11 the lock becomes a table lock, and its place among "table locks of 07, 7.7" is right. Without P11: the modules lock moves to the top, above heap access, with the note "held across safepoints; a holder parks like any thread; the collector and safepoint closures never take it". Add "inbox lock; `Lock` and `Condition` internal locks" beside `ParkingLot`, inside `NoSafepointScope`.

### P20. R4 1.2-2, 10.2-2 to 10.2-5, 10.2-7, H-1, R6 H-5, H-6: how stubs reach the members that leave `VM`

**Kind.** Partly. 7.2 describes C++ ("inline accessors that forward to `ThreadContext::current()`"). 9.5 moves one member (`m_exception`) into the view. 9.3 treats `doVMEntry`. For the other stub sites of R4 10.2 no document says what the new address is, and the view has no word that leads to the `ThreadContext`. 01, 1.4 C says that "no instruction of compiled code or of a stub changes its addressing".

The sites: `topCallFrame` stored in front of host calls (`aot/stubs/AOTStubsCallHost.cpp:78`, `AOTStubHelpersFrames.cpp:146`, `AOTStubsConstruct.cpp:116`, `AOTStubsPropertyAccessors.cpp:142`); `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(vm)` and its inverse (`AOTStubsExceptions.cpp:48, 59`); `targetMachinePCForThrow`, `callFrameForCatch`, `targetMachinePCAfterCatch` (`:81, 204-206`); `m_stopsAtFunctionEntries` (`AOTStubsPolls.cpp:133, 152`); `regExpMatchOutput` (`AOTStubsRegExpFrames.h:79`); `topEntryFrame` in `aot/image/AOTWasmStubs.cpp:43`. `jumpToGenericUnwindTarget` builds a stand-in Instance on the stack with only `vm` filled in, for a handler that reads `[that + offsetOfVM]` and then `VM` members (`AOTStubsExceptions.cpp:67-83`).

A site that is missed reads or writes a word that no longer exists in `VM`, or the wrong thread's.

**Proposal.**

> **V8.** The view has a constant word `thread` (`ThreadContext*`) beside `vm`. Every `[instance->vm + k]` of R4 10.2 whose class is (a) becomes `[instance->thread + k']`: the same two loads. `ThreadContext` begins with the words that `doVMEntry`, the unwind stubs and the interpreter's `op_catch` touch, in one fixed layout that is a runtime fact. `VM::offsetOfTopCallFrame()` and its like do not exist in mt builds, so that a missed site does not compile. 01, 1.4 C reads "no instruction changes its addressing *of the Instance*".

### P21. R4 1.1-4, 1.1-10, 1.2-12, 1.2-13, 1.2-15, F18, F19, F20, 7.16, 7.17, H-28: words that an event pushes into the Instance, and members that 7.3.3 does not list

**Kind.** Partly. Mostly performance; F19 and host values are wrong values.

7.3.3 says of the third group: "zero. Filled at first use on this thread, from the truth". That describes a *pull*. Several words are *pushed*, once, by the thread on which an event happens, into the Instance it finds through the realm:

| Word | Pushed by | On other threads |
| --- | --- | --- |
| `prefix.collectionStructureIDs[]` (Map, Set, Buffer) | `didCreateInstanceStructure`, when the realm's lazy class Structure is made (`aot/runtime/AOTInstance.cpp:365`, called at `AOTRealmStartup.cpp:491, 545`) | stays 0 in each spawned thread's view that exists then, the pusher's own included: the inline receiver test fails for good, and no slow path fills it |
| `closures.structuresWithPrototypeProperty[4]` | `ClosureTable`, "as they come up" (`aot/object/AOTClosureTable.cpp:130`) | the same |
| array headers of the prefix, `frameArrayHeader`, copy-on-write headers | `haveABadTime`, `captureCellHeaders` | **keep the old header**: new arrays with contiguous storage in a realm that has a bad time. 7.3.3's list of words "written into every view" lacks them. 08, 8.6 expects the event "once per realm of `node:vm`", but the stubs test `branchIfHavingABadTime` for the program's own realm, so it happens there too |
| `builtins.hostValues[n] = 0` | `forgetValuesNamed` (`aot/runtime/AOTBuiltinCells.cpp:354`) | shared region, so all see it. But a filler that read the old property before the reset wins its compare-and-swap from 0 afterwards: stale for ever |

Members that 7.3.3 puts in no group: `modules.status`; in `runtime.constants` the table bases `keyCells`, `literalStrings`, `wide*`, `bigIntCells`, `cardsOfStringCells`, `startOfStringHeaders`, `sizeOfStringHeaders`; `runtime.counters.*`; `runtime.literals.propertyRuns`; `propertyCaches.handlers`, `infoOfClassPrototypes`, the two UIDs, `defaultHasInstanceFunction`; `closures.closureWords`, `executables`, `closureHeaders`; the job words and headers of `coroutines`; `allocation.*Space` (08 has them); `realm.readAndWrittenCountsStructure` and `constants.nameOfFunctionsWithoutEntry`, for which the Instance word is the only truth there is; `state`.

**Proposal.**

> **V9.** Each member of `Instance` and `InstancePrefix` carries one of five marks in its declaration, and a generator fails the build for a member without one: `constant` (copied), `rebased` (P3, V7), `thread` (set up), `pulled` (zero; some slow path of the reader fills it), `pushed` (the writer calls `forEachView`, under `instancesLock`, while parked if the word is not a monotone 0-to-value word; a new view copies it from the canonical view in the critical section in which it is listed).
>
> `haveABadTime`'s closure rewrites the headers of every view.
>
> `forgetValuesNamed` and the fillers of host values run under the `BuiltinCells` lock. A filler reads the property again under it before it publishes.

### P22. N-11: `VMState::instances` cannot simply become the list of views

**Kind.** Contradiction with the source. 7.3.3: "`VMState::instances` becomes the list of views, under its lock."

About twenty users take an element for "a realm with its `State`":

- `didChangeChainObject` iterates **without the lock**, and calls `state->chainDependents->didChange` and `didChangeClassHolder` per element (`aot/runtime/AOTChainDependents.cpp:246-253`): N times per change, on a `Vector` that starting and ending threads change.
- `visitMarkingRoots`, `visitExecutionRoots`, `finalize`, `willAnalyzeHeap` visit `instance->state->caches` per element (`aot/runtime/AOTHeapHooks.cpp:50-69, 135-163`): the realm's registry N times, `finalize` twice on one registry.
- `instances.size() == 1` is a shortcut for "the Instance": `tryFindInstance` (`aot/runtime/AOTCompiledFrameIterator.cpp:117-123`), `findInstance` (`aot/runtime/AOTStackTrace.cpp:401`).
- `m_isSharedByInstances = instances.size() > 1` restricts what may be filed (`aot/runtime/AOTPropertyCaches.cpp:102`).
- `reserveInstance` refuses a second element unless the image allows it (`aot/runtime/AOTInstance.cpp:349`). `destroyAll` destroys each element with its `State`.

**Proposal.**

> `VMState::instances` stays the list of realms (canonical views). A new `VMState::views`, under the same lock, lists `{ view, ThreadContext* }`. The writers of 7.3.3's paragraph (`didChangeBarrierThreshold`, stop marker, `pushed` words) iterate over `views`. The constraints visit `instances` for the realm's registry and `VM::m_threads` for the threads' registries. Each unlocked iteration over `instances` is listed and given the lock.

### P23. R4 1.3-3, F1, H-5: "a stale pass of a guard is harmless" is not yet shown

**Kind.** Partly, by the design's own word (07, 7.5, F1: "To be confirmed per guard kind .. in work package W-7.4").

What the check has to show, per kind: the code behind a passed test relies on nothing but (i) the receiver's shape, which is tested apart and constant, and (ii) facts of flow that do not depend on the guard. From `aot/object/AOTMemberGuards.cpp` and the emitters nothing was found against it for `Member`, `PrototypeLink`, `NotHidden` (the old method is entered with a receiver of a layout that it accepts) or `PassedByStores` (the store goes through the slot's own type test). Not read: guard *sets* ("one bit, which is set while every guard of the set is"), whose bit is derived state with an update of its own; the arming logic (R4 N-10); and whether a result type, or an inlined body, that flow derived from "the member is unchanged" is used without a test behind the guard.

**Proposal.**

> **F1a.** No fact that `trustsKnownRanges` or a representation rests on may be derived from a guard. The verifier refuses a value whose type is narrowed by a `Guard` of kind `MemberUnchanged` or `PrototypeLinkUnchanged` and that is used unboxed without a test of the value. A set's bit is cleared by `fetch_and` **before** the member bit, so that no thread sees the set hold while a member has ended.

### P24. R4 7.5, 7.10, H-10, H-14, H-18: smaller holes in the tables of 7.6 and 7.7

**Kind.** Partly.

| # | Item | Hole | Rule text |
| --- | --- | --- | --- |
| a | `ProgramSymbols` | the row cites PT1 (block with its mask). The entry is `{number, uid}`, stored as a struct (`aot/runtime/AOTProgramIdentifiers.cpp:117-130`), and the stub compares `number` and then loads `uid`. And `uidSlowCase` creates outside the lock and inserts without looking again (`:98-115`): two private names for one `#x` | "as PT1 **and PT2**: `uid` first, then `number` (release). Under the lock: look again, then create, then insert. `SymbolImpl::create` neither collects nor runs code" |
| b | `RegExpLiterals` | the collector visits `m_cellsInOrderOfCreation` under the *registry's* lock, from a constraint that is declared concurrent. The new table lock is another lock | "an append to a container that a registry visits holds the table lock and `CacheRegistry::lock()`, in that order (8.15 allows it)" |
| c | `ZeroedPages::m_ranges`, `ZeroedTableArena` (`aot/runtime/AOTState.h:43-72`) | shared bump allocators behind many slow paths. Not in 7.7 | one leaf lock each, or one arena per thread for per-thread tables, subject to P9 |
| d | `VM::m_executablesOfNamelessHostFunctions` | named in R4 H-18, in no table | table lock |
| e | `KeyOrderTable`'s "old front" in 08, 8.9 | listed as memory that is freed late, while 07, 7.4 makes the front per thread | drop it from 8.9 |

## B.3 The heap and the flow analysis (reports R5, R3)

Most severe first. "T", "T1", "T2" are JavaScript threads, "K" a marker thread, "G" the conductor.

### F1. R5 B7: the re-whitening path of `addToRememberedSet` is unsound with two mutators (M)

`heap/Heap.cpp:1358-1392`. State of a cell `C`: mark bit `m`, cell state `s`. A full collection marks; `C` survived earlier collections, so `m = 0`, `s = PossiblyBlack`.

| Step | Who | Action | `m`, `s` |
| --- | --- | --- | --- |
| 1 | T1 | `C.f = a`; fence; reads `s == PossiblyBlack`; enters `addToRememberedSet`; `isMarked(C)` is false | 0, Black |
| 2 | K | `testAndSetMarked(C)`; `s = PossiblyGrey`; pops `C`; `s = PossiblyBlack`; fence; **reads all fields of `C`** | 1, Black |
| 3 | T1 | `compareExchange(s, PossiblyBlack, DefinitelyWhite)` succeeds | 1, White |
| 4 | T2 | `C.g = X` (`X` white, held nowhere else after this); fence; reads `s == DefinitelyWhite`; `writeBarrierSlowPath` returns (`heap/Heap.cpp:2868-2879`) | 1, White |
| 5 | T1 | `isMarked(C)` is true: `s = PossiblyBlack` | 1, Black |

`C` is marked, scanned, black and on no mark stack. `C.g` names `X`, which nobody will mark once T2's register is dead. `X` is swept at the end of the cycle (cells allocated during the cycle are not exempt: `endMarking` moves the newly-allocated version on and clears the `allocated` bits, `heap/MarkedSpace.cpp:466-491`, `heap/BlockDirectory.cpp:318-338`). Use after free.

With one mutator the window of steps 3 to 5 is harmless because the only thread that could run a barrier in it is the one inside the function. The comment's argument ("we proved that the object was not marked at the time that we executed this slow path") covers T1's own store only. The same window opens when T1 and T2 both read `isMarked == false` and T2's compare-and-swap comes after K's scan.

08 8.10 leaves the row open ("to be proven or fixed"). It has to be fixed.

**Proposed rule H2.** In mt builds `addToRememberedSet` does not re-whiten: `if (!isMarked(cell)) return;`. (The source says "We could return here".) This is sound for any number of mutators, each for itself: the store precedes a full fence, which precedes the load of the mark bit that returned 0, which precedes K's atomic set of that bit in coherence order, which precedes K's fence and its reads of the fields. Cost: a store into an old object that the full collection has not reached yet takes the slow path each time, not once. (Alternative: keep the compare-and-swap, and in the correcting branch set `PossiblyGrey` and append the cell to the thread's mark stack. The later visit then sees every store that was made while the state read white.)

### F2. New: `publicLength` can go backwards, and the marker traces only up to `publicLength` (M)

06 6.3: `a[i] = v` with `publicLength <= i < vectorLength` is "element first, then `store32(b-8, i+1)` if still smaller"; `push` is `store64(b+8len)`, `store32(b-8, len+1)`; `a.length = n` in line is "holes from `n` up, then the length". All are plain, and in state `Shared` several threads run them at once. The marker visits `butterfly->publicLength()` slots (`runtime/JSObjectVisitInlines.h:103`).

1. Storage `Shared`, `publicLength = 3`, `vectorLength = 16`. T1 runs `a[5] = p`, T2 runs `a[9] = q` (`q` a cell).
2. T1 loads the length: 3. T2 loads the length: 3, stores `q` at 9, stores length 10.
3. T1 stores `p` at 5, stores length 6. The test "still smaller" was made in step 2.
4. Slot 9 holds `q` above `publicLength`: SW3 is broken for good, not "for the moment inside an operation".
5. T2 drops `q`. A collection runs. The marker visits slots 0 to 5. `q` is freed.
6. Any thread runs `a[12] = r`: slots between the old length and 12 are taken to be holes, the length becomes 13, and `a[9]` yields a dangling reference.

The same state arises from `push` (stores `len + 1` computed from an old length) against `a[9] = q`, and from `a.length = n` against a store at `i >= n` whose bounds test passed before the holes were written.

SW3 and SW4 are stated as invariants, but no protocol maintains SW3 between two writers of `Shared` storage, and 08 does not say how far the marker reads.

**Proposed rule SW10.** For Contiguous storage that is not `Confined` the marker visits all `vectorLength` slots (load-acquire of `vectorLength`, which pairs with the release store of SW2). Slots above `publicLength` are then either holes or values that are kept alive. What is left is a value that some thread did write to that index and that reappears when the length grows, which M3 allows. **And** the length is raised by a compare-and-swap loop (`max`) wherever the storage is `Shared`, so that M8 ("`length` never below the index of an element ..") holds.

### F3. R5 B11, E12, 3.6-5, 5.4: a thread that starts or ends while a stop or marking is under way (M)

08 8.14: start is "register in `VM::m_threads` (if a stop is requested, wait first); make the view; `acquireAccess()`". End is ".. `releaseAccess()`; unregister; unmap the view; free the context". Both halves run without heap access, so nothing excludes them from a stop.

**Start.**
1. T registers (no stop requested) and begins to make its view: it copies the struct's words from the canonical view, among them `barrierThreshold = blackThreshold`, `mutatorShouldBeFenced = false`.
2. G stops the world for Begin. T has no access, so the stop completes. `beginMarking()` calls `setMutatorShouldBeFenced(true)`, which writes "every view" in `VMState::instances` (07 7.3.3). T's view is not in the list yet.
3. T appends its view, waits in `acquireAccess()`, and then runs through the whole Concurrent phase with the stale words: barriers without the fence and without the tautological threshold, the in-line append "while no collector runs beside the program", the stored-cell log during marking, no `dmb` after initialization on ARM64. A store into an object that K is scanning is missed. Use after free.

**End.**
1. T calls `releaseAccess()`. G stops the world and starts "Cs", which reads T's `m_parkedState` and scans T's stack "in place" (8.13), iterates the directory's allocators without a lock (`heap/BlockDirectory.cpp:232-238`), and visits T's registry, inbox and microtask queue.
2. T unregisters, unmaps its view, frees its `ThreadContext` with the `LocalAllocator`s, returns, and the OS unmaps its stack.
3. G reads freed or unmapped memory.

**End, during the Concurrent phase.** The first visit of a full collection turns root tables into `DeferredRootRegion`s "that any marker thread takes later, possibly while the mutator runs" (`heap/RememberedSlot.h:237-270`, `heap/SlotVisitor.cpp:921-944`). With one registry per thread (07 7.4) such a region points into T's view. T ends and unmaps it; K faults.

**Proposed rule H3.** `VM::m_threadsLock` is held by the conductor from before it sets `m_stopRequested` until after `cancelStop()` (see F18). Under that lock, and only there: a thread is added to `m_threads` **together with** its finished view and registry (the collector's words are copied under the lock), and a thread is removed. So a stop sees a thread with all of its parts or not at all. **H4.** A thread's view, registry and context are not freed while a collection is between Begin and End: the ending thread hands them to the heap (`m_retiredThreads`), and the conductor frees them at the end of the End phase.

### F4. R5 D13, 7.4: the stack scan skips a thread that is in the middle of `acquireAccess()` (M)

08 8.13: "Cs" scans "for each thread that is `Parked` or `NoAccess`". 08 8.3: `acquireAccess()` first stores `HasAccess`, then loads the request, then stores `NoAccess`.

1. T blocks in a native with access released. Its frames hold the only references to some cells. G's stop completes.
2. The native returns. T stores `HasAccess`.
3. G runs "Cs", reads `T.m_access == HasAccess`, skips T.
4. T loads `m_stopRequested == true`, stores `NoAccess`, waits. Its roots were not scanned in this stop. If this is the last Fixpoint, its cells are freed.

**Proposed text for 8.13.** "Cs" scans every thread in `m_threads`, whatever its `m_access` says. `m_parkedState` is valid from the store of `NoAccess` or `Parked` until `acquireAccess()` **returns**; a thread does not pop a frame above the captured stack top before that.

Also: 09 9.3 keeps "the steps of `didAcquireLock`", one of which is `machineThreads().addCurrentThread()`. Then `tryCopyOtherThreadStacks` suspends every JavaScript thread by signal, which 8.13 excludes ("No suspend, no copy, no signal"). Say in 9.3 that a JavaScript thread is not added to `MachineThreads`.

### F5. R5 D11, A3: `acquireAccess()` does not run `didResume()` (M)

08 8.3 calls `didResume()` at the end of `park()` only. 08 8.12 moves the clearing of `HasOwnPropertyCache`, `MegamorphicCache`, `keyAtomStringCache`, `stringSplitCache`, JSON caches (`heap/Heap.cpp:2329-2342`) into `didResume()`, because the caches are per thread.

1. T waits in `join()` or in its event loop, without access.
2. A collection runs from Begin to End. Cells that only T's caches name (they are not roots; that is why the epilogue clears them) are swept.
3. T's wait ends. `acquireAccess()` finds no stop requested and returns. T never parked, so it never cleared anything.
4. T probes `keyAtomStringCache` and gets a freed `JSString*`; or its allocation site record holds the header of a Structure that `forgetStructuresOf` dropped and the collection freed (A3: the epoch is compared "when it goes on after having parked").

**Proposed text.** `Heap::m_collectionVersion` is incremented in the End phase. `MutatorContext::m_collectionVersionSeen` is compared at the end of `acquireAccess()` and of `park()`; if it differs, `didResume()` runs. Simpler: the conductor clears every thread's caches while stopped, as 8.12 already does for the caches of the `CacheRegistry`.

### F6. R5 D9, B9, 13-12: operations that may not park, and yet have to wait or to conduct (Contradiction; M or L)

08 8.4 makes every `DeferGC` and `DeferGCForAWhile` scope a `NoSafepointScope`: the thread neither parks nor releases access. That is what S2 needs, because such a scope is how the run time keeps the promise of an IR operation that is **not** declared `collects`: `CreatesConstant`, `AllocatesWithoutCollecting`, `RuntimeCreatesConstant`, `RuntimeCreatesField` (`aot/ir/AOTSchema.def:83-85`, `aot/ir/schema/runtime-operations.def:1-5`). There are about 70 such scopes in `aot/runtime` and `aot/object` and about 60 in the rest of the engine.

Other rules demand, from inside those scopes, exactly what the scope forbids:

| Reached from a scope | What the rule demands |
| --- | --- |
| IR `Builtin`: `operationAOTBuiltinCell` holds `DeferGCForAWhile` and calls `BuiltinCells::cell`, which calls `loadModuleOf()` and `find()` (`aot/runtime/AOTConstantOperations.cpp:71-79`, `aot/runtime/AOTBuiltinCells.cpp:264-273`) | MD3: wait for `m_evaluationLock` "with heap access released". Z2: a loser of a `LazyProperty` claim "waits (parked with heap access released)" |
| IR `LinkTimeConstant`, `GetClassPrototype`, `GetSuperBase`, `SingletonClosure`, `SharedRegExpLiteral` (`aot/ir/schema/runtime-operations.def:14,42,186`, `runtime-builtin-operations.def:43,114`) | Z2 claim on a class or a lazy property |
| IR `GetOwnDataProperty`, `EnumeratorGetByVal`: a read reifies a lazy static property, which is an add (06 6.4) | "If the storage is `Shared` and must be replaced .. the whole mutation runs as a safepoint operation" |
| `makePendingStars` (`aot/object/AOTObjectExtraProperties.cpp:292`); the mutations of 05 5.6 (a) and (e), which hold the outer object's lock | growth of the side object's `Shared` storage: a safepoint operation, with a scope open or **two** locks held |
| `Structure::flattenDictionaryStructure`, called as an optimization from cache fills and enumerator creation (06 6.4), some of which serve operations that are not `collects` (not verified one by one) | safepoint operation (08 8.6) |
| `Heap::freeAfterSafepoint` called by a rehash under `Structure::m_lock` (PT4, PT5) or under an object lock (`WeakMap`) | "the thread that crosses the line runs an empty safepoint operation" (08 8.9) |

Both ways out are wrong as the text stands.
- **The thread waits as the rule says** (parks, or releases access). A collection or a safepoint operation runs inside an operation that the compiler treats as no safepoint: `Storage` values are live across it (S1 broken: the storage is replaced under the caller), a barrier was removed because "no safepoint between allocation and store" (R5 5.6 b), narrow references are in registers.
- **The thread keeps access and spins.** T1 holds the claim and, being allowed to collect, parks for a stop that G has requested. T2 spins on `Claimed(T1)` inside its scope with `HasAccess`. G waits for T2, T2 for T1, T1 for G. Deadlock.

**Proposed rule S4.** A thread inside a `NoSafepointScope` never waits for a claim, for a lock above "object" in 8.15, or for `m_conductorLock`. Code that finds it would have to returns `MustRetryAtSafepoint` to the outermost frame that opened the scope. **S5.** In an mt image every IR operation whose run-time path can reach Z2, the modules lock, an add to an object that may be shared, or `flattenDictionaryStructure` is declared `collects`. The list is closed in the schema (a flag `mayWaitForOtherThreads` on `runtime` rows, which implies `collects`; a validating build fails when an operation without the flag reaches `park()`, `releaseAccess()` or `runSafepointOperation()`: `OperationParked`). For `Builtin`, `LinkTimeConstant` and `SingletonClosure`, whose cheapness matters, the alternative is to fill all cells that the image names before the first `new Thread` returns. **8.9:** crossing the threshold sets `m_didDeferGCWork`; the flush runs at the thread's next safepoint.

### F7. New, with R5 A12, 2.6-33: safepoint operations (08 8.6) are not alone with the heap, and may run where they cannot (M)

**(a) Markers are not stopped.** `m_conductorLock` is free during the Concurrent phase, so a mutator can conduct a safepoint operation while K and its helpers mark. Point 4 says a closure "does not .. take an object lock (nobody holds one: L1)". L1 binds mutators. A marker takes the cell lock of an array with ArrayStorage or with an elements start offset (`runtime/JSObjectVisitInlines.h:355-369`), of a `CompactTable`, of a sparse map (`runtime/SparseArrayValueMap.cpp:243`), and `Structure::m_lock` (`runtime/StructureInlines.h:642-644`). `flattenDictionaryStructure` today takes the cell lock and `m_lock` and nukes the StructureID **against the marker** (`runtime/Structure.cpp:1799-1825`). A closure written to the letter of point 4 shrinks a butterfly or renumbers offsets under K's hands: K marks the wrong base address or reads a slot as a value that is none.
The same holds for 8.9: deferred frees are released at the end of a stop that did not stop the markers. That is safe only because each marker reads those buffers under the lock that the replacing writer held. No rule says so.

**(b) A closure can land on the collector thread.** "Runs the closure on the calling thread" (first sentence) against "another conductor may run .. this very closure" (step 2) and "the thread that gets the lock .. runs all pending closures" (step 3), while "the collector's stop-the-world phases take the same lock". On the collector thread there is no `MutatorContext`: "it may allocate" has no allocator (`VM::clientHeap` is an accessor for the current thread), the barrier after publishing new storage has no mark stack, `Owned(me)` has no `me`. After `stopThePeriphery()` allocation is illegal for anybody.

**(c) Preconditions.** 06 6.3 allocates the new storage **before** the operation and runs "steps 2, 5, 6 as the closure"; step 2 is "check that growth is still needed". While the caller waited, another closure of the same batch may have grown the array further (the prepared storage is now **smaller** than what step 5 copies: out-of-bounds write), changed its kind (prepared for Int32, now Contiguous), made it ArrayStorage, frozen it in place, or restricted it. For the property add of 06 6.4 the check "the StructureID is the one that was planned from" is stated for the locked path only.

**Proposed text for 8.6.**
4'. A closure runs beside markers. Towards them it is a mutator: it keeps every step that today's code takes against the marker (nuke, fences, cell lock, `Structure::m_lock`, owner barrier). It takes no lock that a *mutator* can hold.
5. Closures run on a JavaScript thread. The collector thread, when it takes `m_conductorLock` and finds closures pending, releases the lock to the first waiter and queues behind it. A closure that another JavaScript thread runs uses that thread's allocators and mark stack, and must not refer to "the current thread": what it needs of its requester it captures by value.
6. A closure validates everything it was prepared from (storage pointer, state, kind, `vectorLength` against the capacity it brought, StructureID) and returns `false` if anything differs. The requester then starts again from the generic dispatch. A closure allocates nothing that its requester could have allocated before.
7. (H5) Memory handed to `freeAfterSafepoint` is read by markers only under the lock that its replacing writer held.

### F8. R5 E2, A15, D11, 9.3, 3.6-3, 2.6-31: thread-affine blocks (08 8.11) against the sweeping that the source does (Contradiction; M/V)

The promise: "Only the owner's allocator claims such a block and only the owner sweeps it, so a destructor or finalizer runs on the thread that made the object." Six paths break it.

| # | Path | Who runs the destructor |
| --- | --- | --- |
| 1 | **Lower-tier precise cells.** The first 8 instances of every iso class are precise allocations (`runtime/JSCell.h:107`; Bun passes `T::numberOfLowerTierPreciseCells`, `bun:src/jsc/bindings/BunClientData.h:413`; taken before a block is made, `heap/LocalAllocator.cpp:187`). They are destroyed by `sweepPreciseAllocations()` (`heap/MarkedSpace.cpp:279-303`, `heap/PreciseAllocation.cpp:274-287`), which 08 8.12 moves "inside the End phase, by the conductor" | the **collector thread**, which has no Bun `VirtualMachine` |
| 2 | The freed lower-tier cell goes to one free list per subspace and is revived by any thread (`heap/IsoSubspace.cpp:81-113`) | owner field of the header is stale unless rewritten; the list is unlocked |
| 3 | **Stealing.** In a compiled process every iso subspace uses `heap.cellAllocator()` (`heap/IsoSubspace.cpp:47`), so `findEmptyBlockToSteal()` crosses all of them; the thief runs `block->sweep(nullptr)` (`heap/LocalAllocator.cpp:260-275`). "Empty" means no marked cell; destructors are still owed | any thread that allocates anything caged |
| 4 | **`collectNow(Sync)`**: after the collection the requester runs `sweepSynchronously()`: `sweepBlocks()` over every directory, `shrink()` (`heap/Heap.cpp:1930-1945,1437-1449`). `tryCollectForExhaustedCage()` is this call (`:2895-2901`). So is `Bun.gc(true)` (`bun:src/jsc/bindings/bindings.cpp:5185`) | the requester, beside running mutators |
| 5 | `shouldSweepSynchronously()` in the epilogue; `sweepEagerlyInEpilogue()` (`heap/Heap.cpp:2319-2324,2375`) | whoever runs that part: 8.12 has no row |
| 6 | **`WeakHandleOwner::finalize`** belongs to the block of the **referent**, not of the owner. Bun's `Weak` with `FetchResponse` context (`bun:src/jsc/bindings/Weak.cpp:27-44`), `JSDOMWrapperCache`, N-API weak references point at ordinary objects and functions in ordinary blocks | any thread |

**Adoption.** "When a thread ends its blocks are adopted by the main thread, and with them its objects" (11 11.3). That gives the main thread not only the finalizers but the **methods**. A socket, timer or file object made on T holds pointers into T's `VirtualMachine`, event loop and arena, which `threadWillEnd` destroyed (11 11.4 lists "mimalloc arena" as per Bun VM; for a Worker the heap dies first, here the cells outlive the thread). Not verified per class; the rule must not depend on it.

**A thread that never sweeps.** A thread that blocks for ever, or computes without allocating that class and without an idle loop, never finalizes its dead objects: descriptors leak.

**Proposed rules.**
- **H6.** An iso subspace of an `owner` class has `numberOfLowerTierPreciseCells = 0` in mt builds, and its directory is not registered with the allocator's list of directories to steal from; `shrink()` frees a block of it only if the block is swept.
- **H7.** `sweepAll`, `sweepSynchronously`, the eager sweep of the epilogue and the incremental sweeper skip affine blocks of other threads. `assertNoUnswept()` excludes them. A thread that is asked to sweep (`collectNow(Sync)`, memory pressure) posts "sweep your affine blocks" to each owner's inbox.
- **H8.** A precise allocation of an `owner` class (a large cell) is destroyed by its owner: the End phase moves it to the owner's list, and the memory is freed after the owner has run the destructor.
- **H9.** A `WeakHandleOwner` is classified like a native: `any` (runs on the sweeping thread) or `owner`. For `owner`, `WeakSet::sweep` does not call `finalize` but posts `(owner, context)` to the thread recorded in the `WeakImpl`'s context. Engine owners are `any`.
- **H10.** Blocks of an ended thread get the owner ID `Orphan` (0xFFFF), which no thread's ID equals: every method refuses. The main thread sweeps orphan blocks. `threadWillEnd` runs after a last sweep of the thread's affine blocks; a class whose destructor touches the creating thread's `VirtualMachine`, loop or arena declares `finalizesOnOwnerOnly`, and its live instances are *neutered* (the native part is destroyed, the cell marked) before the Bun VM is torn down.
- **A15.** Say in 8.8 that the synchronous sweep after `tryCollectForExhaustedCage()` runs beside running mutators under the `inUse` claims. And that the reserve (`tryUseReserveOfExhaustedCage`) is shared: while T1 unwinds with its `RangeError`, T2 to Tn consume the reserve and the process ends in `logExhaustion`. Either all threads get the owed error, or M11 gets an exception.

### F9. New: `freeAfterSafepoint` and S1 do not cover a thread without access, nor C++ in general (M)

**(a) A thread without access counts as "at a safepoint".** 08 8.9 frees at the end of a stop because "every thread is at a safepoint and by S1 holds no such pointer". H1 and N-g say that inside `ReleaseHeapAccessScope` a thread "touches no cell". The bytes of an ArrayBuffer are no cell.

1. T1 calls a native marked `blocks`, for example `readSync(fd, buffer)`. The emitter releases access around the call (03 3.5.2). The system call writes through `buffer`'s vector.
2. T2 runs `buffer.buffer.transfer()`. TA4: copy, detach; TA1: the old contents go to the deferred list.
3. Any stop completes (T1 has no access). The conductor frees the contents.
4. The kernel, or T1's `memcpy`, writes into freed memory.

On one thread this cannot happen for a synchronous call. (For work handed to Bun's thread pool the hazard exists today and is met by keeping the buffer alive; whether `transfer` is refused meanwhile was not verified.)

**(b) C++.** The verifier checks S1 for IR values only. C++ has N-d and an audit. Existing code holds storage over safepoints as a matter of course:
- `runtime/ArrayPrototype.cpp:1612-1668`: both butterflies, both lengths and both indexing types are loaded, then `auxiliarySpace().allocate()` runs, then the copies use the old pointers with the old lengths and kinds.
- `runtime/JSGenericTypedArrayViewPrototypeFunctions.h:240-247`: `typedVector()` is cached across **calls of JavaScript**, and each turn tests `isDetached()` before it reads. That is safe under TA1 and TA2, but by another argument than S1: the test and the use are in one window.
- GC-allocated storage named from a parked stack stays alive through the conservative scan, so a stale butterfly costs a lost store. Memory from `freeAfterSafepoint` has no such net.

**Proposed rules.** **H11.** A native that uses the bytes of a buffer while it has no heap access holds a pin on the `ArrayBuffer` (`pin()`, TA7) for that time. Contents with a pin count above zero are not put on the deferred list: `transfer`, `resize` and detach copy and leave the old contents to the last unpin. The table's `blocks: true` with an operand of a buffer type makes the emitter pin. **S1'.** In C++ a pointer to replaceable storage is held in `StorageRef<T>` (constructor: `++thread.m_liveStorageRefs`). `park()`, `releaseAccess()` and `runSafepointOperation()` assert that the count is zero in debug and validating builds. A pointer that must outlive a safepoint is a `RevalidatedRef`, whose `get()` takes the test that makes it valid in this window.

### F10. R3 section 10: ways past the sharing analysis (04 4.2) (M)

A wrong "Confined" or "MainOnly" is memory unsafe: Confined storage is written without a test, replaced in place and `realloc`ed, Confined layouts have packed Extensions, MainOnly variables get no release stores.

#### (a) R1 misses the `Thread` object itself

`HandedToThreads` holds the callee closures, `thisValue`, the arguments, and the contents of the result slot. The spawned thread also reaches **the object that `new Thread` returns**, through `Thread.current` (02 2.2).

```js
const t = new Thread(() => { Thread.current.box.n++; });   // on T
t.box = { n: 0 };  t.box.n++;                               // on main
```

`Thread.current` is declared `returns: b.instanceOf("Thread")`, not fresh (03 3.5.2). Flow answers a result that is not fresh with the unknown object (`aot/valueflow/AOTValueFlowBuiltins.cpp:1759-1764`), and "the unknown object stands only for what has escaped". The site object of `new Thread` has not escaped. So (1) the points-to solve itself is unsound: a store through `Thread.current` never reaches a read through `t`; (2) the `Thread` site, its out-of-line storage and `box` can be Confined.

**R1'.** `HandedToThreads` also receives the result object of the call that a `NewThread` record belongs to. The constructor's row stores its result in a native cell `Thread.current`; the getter reads that cell.

The async context (02 2.6: "a new thread starts with the store that was current at `new Thread`"; 09 9.2 step 3) is a second value that crosses without being an operand. It is covered only if `$asyncContext` is one abstract location for all threads, so that R4 fires when thread code reads it. Say so in R4; the location is per thread at run time (09 9.7), and somebody will model it so.

#### (b) R5 has one direction only

R5: `escaped` is in `Shared` "if an AnyThread function **receives** the unknown object". The other direction: an AnyThread function lets an object escape, and a function on another thread receives it.

```js
function work() { const e = new Failure(); keep = e; Promise.reject(e); }        // on T; `keep` is a local of the thread
process.on("unhandledRejection", reason => { reason.count++; });                 // handler runs on main (02 2.6, 09 9.7)
```

"What a promise is rejected with escapes"; what a `Later` callback throws escapes (`connectThrown`). The handler's parameter is the unknown object. `work` receives nothing unknown. `Failure` enters `Shared` by none of R1 to R5, so R6 calls it local. Main and T mutate one object of a Confined layout. The same for uncaught exceptions posted to main (09 9.11) and for any `any` native that keeps an operand where another thread's call can read it.

**R5'.** Once `SomeThreadStarts`, `escaped` and `lost` are in `Shared`. (Weaker and still sound: .. if an AnyThread function receives the unknown object **or any object that an AnyThread function allocates or holds is in `escaped`**.) The price is precision for escaped objects, which 4.2.5 accepts.

#### (c) A callback is attributed to the thread that registers it; some natives fire it on the thread that calls them

04 4.2.2: the callee of a `PerEvent` record "runs on the thread(s) of the function that made the registering call. This is sound because reactions, timers and events fire on the registering thread". For events that is not what the natives do:

- `process.on(event, listener)`: the listener is `retained` and `calledWithEvents` at the **registration** (`bun:src/codegen/builtins/host/process.builtins.ts:60-70`). `process.emit(..)` "calls the listeners that were added", natively, synchronously, on the **caller** (`:111-120`). Its row has `capability: "js"` and no callee edge.
- `addEventListener` and `dispatchEvent` are the same pair (`bun:src/codegen/builtins/host/globals.builtins.ts:144-164,1727-1735`), also with the global object as target.

A MainOnly function registers `h`; `h` escapes; no AnyThread function "can reach the closure" (it lies in native memory, under no object of `Shared`) or registers it. Row "Closures that escaped to native code": MainOnly. T calls `process.emit("x")`. `h` runs on T.

Today's only defence is the `threads` column: `owner` refuses the firing call. But 11.4 classifies `process.on` "for signals" only, says nothing of `process.emit`, and the checklist of 11.2 for moving a native to `any` has no item about callbacks.

A second hole of the same kind: `CallByHost` records ("a call that native code makes where no call of the program is under way") have no registering call at all. Each thread has its own event loop (11.4), so whatever the loop calls of its own accord (tick and rejection processing, `beforeExit`, warnings) runs on every thread. 4.2.1 looks at `callByHost` only for `on: "NewThread"`. Which records exist was not verified.

**Proposed.** Edge table of 4.2.2, two new rows: "A call, in an AnyThread function, of an entry with `capability: "js"` that is not `runsOnlyOperandCode`: every closure in `escaped` is AnyThread" and "`CallByHost` records: AnyThread, unless the record says `on: "Main"`". 11.2 gets **N-j**: "a function that another call registered is called only on the thread that registered it; else the row says `fires: "Caller"` and names the native cell that holds the callbacks, and flow adds the edge from each caller to what the cell holds".

#### (d) A `Thread` constructor that flow does not see applied (R3 7.5-3, 5-U)

Seeds are added "in `applyCallback`, `applyHandlerCall` and `callByHost`, when the callback's record says `on: "NewThread"`": only where the solve applies the row. If the built-in `Thread` is itself in `escaped` or `lost` (stored on `globalThis`, passed to unknown code, constructed through `new X(f)` where `X` holds the unknown object), the operands go to `escaped` and no seed is made. In an open program interpreted text can also `import("bun:threads")`, since the program imports it (02 2.1 excludes only programs that do not). `f` stays MainOnly and runs on a thread; worse, a function that nothing else reaches is entered with operands the solve never passed.

**Proposed.** As for evaluators (`m_objectsAlwaysFollowed`, `eventOfCalling`): the entry gets the flag `StartsThread`; it is always followed as a value; if it is in `escaped` or `lost`, or the image has `AdmitsUnknownJavaScript`, every closure in `escaped` (and `lost`) is a thread entry, with `ThreadFunctionIsUnknown`. Or the run time refuses: `new Thread` from a caller that is not compiled code of a site in the image's list throws `TypeError`.

#### (e) R4 says "iff", R3 says "always" (R3 H10)

A native cell "has no holder, so the global object stands in": it is a `Slot` of `globalThis` (`aot/valueflow/AOTValueFlowBuiltins.h:56`). R4 ends "`globalThis` and the realm's built-in objects are shared", and R3 follows `EveryProperty`, which covers `Slot`. So every native cell's contents are in `Shared` in every program that starts a thread: each `setTimeout` callback of the main thread, its arguments, and by R2 all they capture. That is sound, and it contradicts "a native cell is a shared location iff some AnyThread function reads or writes it". `noteOperationsOnLocations` records variable operations of functions; reads of cells are made by rows. **Proposed:** R3 skips `Slot`s of `globalThis` that are native cells; R4 decides them by "an AnyThread function calls an entry that names the cell".

#### (f) Code that a pass defers runs on the thread that first needs it (R3 10.1-6)

`DeferInitializers`, `DeferDefinitions`, `DeferClosuresInFields` and the making of classes "at first touch" move work out of a module body. "Functions that passes derive after flow inherit through `FlowResults::function(id)`": the derived initializer inherits **MainOnly** from a body that only the main thread evaluates. At run time it runs at the first read (07 7.6, Z1 or Z2), which an AnyThread function may make. `ValidateThread` fails; stores inside it into MainOnly variables are plain. **Proposed:** a deferring pass sets the derived function's `threads` to the join over the functions that read the location, and does not defer if the initializer touches a MainOnly variable while a reader is AnyThread.

#### (g) `isThereFromTheStart` (R3 5-14)

O2 lists the order facts that survive; this one is missing. It holds if nobody can have the holder before its members are there. 07 7.6: "`createClassObject` registers the closure first and then runs `defineClass`". A thread that finds the class object through `closureCells[]` does not pass the claim word of Z2. **Proposed:** the class object's cell is published (Z1's compare-and-swap) after `defineClass`; until then only the claimant can find it, through its own `ThreadContext`. Not verified against `aot/object/AOTClassTable*.cpp`.

#### (h) Result A depends on result B (Contradiction)

04 4.2: "Three results, computed after the solve, in this order." A's edge table uses B ("AnyThread if an AnyThread function can reach the closure (it is in the closure of 4.2.3)"), and B's R4 and R5 use A. R3 10.2 derived the order from "(a) depends only on the call graph", which the design's own row undoes. One pass of each gives too small a set: a closure that becomes AnyThread through B touches new variables (R4), which hold new closures. **Proposed:** A and B are one least fixpoint over one worklist. Cost stays linear in the size of the heap index.

### F11. New, with R3 10.2-R6: a reaction that is sent to the main thread because its home has ended resumes a frame that holds pointers into the dead thread (Contradiction; M)

09 9.6 gives the reason for G1: a `SavedFrame` can hold "an allocator's address, a table base of a per-thread cache, or an address computed from `instanceGPR`. Resumed on another thread, the frame would bump the first thread's free list." 09 9.7 and 02 2.6: "If the home thread has ended .. the job goes to the main thread's inbox", and "a pending `await` does not keep a thread alive".

1. T runs `async function f() { await shared; use(); }` without returning its promise from the thread's function. T completes; its view is unmapped, its allocators are freed (08 8.14).
2. Main resolves `shared`. The reaction's home is `SavedFrame::m_thread == T`, ended. The job goes to main.
3. Main copies the frame onto its stack and jumps. A spilled allocator address, or `instanceGPR`-derived address, names unmapped memory.

For a plain `then` callback there is no frame, but the closure may capture objects of a Confined layout or site: they are now used by a thread that did not allocate them. That is ordered (T's end happens before the job) and therefore safe, but it contradicts the definition of Confined, and `ConfinedObjectOnOtherThread` (04 4.14) fires in a correct program. Per-thread variables and `ThreadLocal` values silently change under the callback.

**Proposed J1'.** A reaction whose context is a `SavedFrame` is never sent to another thread. If its home has ended it is dropped, as a pending `await` is dropped when a process ends. (Or: a thread with a suspended coroutine that a live promise can resume is not complete. That changes 02 2.6.) Reactions that are plain functions may go to main; the definition of Confined becomes "reachable by no other thread **while the allocating thread lives**", and validating builds re-tag at adoption.

### F12. New: destructors of per-thread `CodeBlock`s run on whichever thread sweeps (M)

10 I1 makes the linked `CodeBlock` private to a thread: "call link infos and their incoming-call lists (a caller's link and the callee's `CodeBlock` belong to one thread)". The blocks are cells in ordinary iso subspaces. A dead caller's `~CodeBlock` destroys its metadata; `~CallLinkInfoBase` does `if (isOnList()) remove()` (`bytecode/CallLinkInfoBase.h:110-113`), which unlinks from the **live callee's** incoming list; LLInt's adaptive watchpoints unlink themselves from the `WatchpointSet` of a shared Structure.

1. T1's block `A` (dead) is linked to T1's block `B` (live).
2. T2 allocates a `CodeBlock`, claims the marked block that holds `A`, sweeps it: `remove()` writes `B`'s list.
3. T1 links another call to `B` at the same time: a doubly linked list with two writers.

**Proposed I3.** The code block spaces are thread-affine in the sense of 08 8.11, with `CodeBlock::m_thread` as owner (H6, H7 of F8 apply). Or: unlinking moves into `reconcileWeakReferencesAtGCEnd`, which runs with all threads parked, and the destructor only frees. Removing a `Watchpoint` from a set takes the set's lock (06 6.5 names `fireAll` only).

### F13. New, with R5 C7, C3: publication of storage says nothing of the owner barrier, and the marker can pair an old Structure with new storage (M)

**(a)** 05 5.5 publishes an Extension "by one compare-and-swap of the hidden word from 0 (release) .. No lock", and 06 6.3 ends growth with "store-release(a+8, new)". Neither mentions `writeBarrier(owner)`. Today's `setExtension` and `AuxiliaryBarrier::set` run it, and the stub `growVector` leaves for C++ when "the collector will not visit [the array] again" (`aot/stubs/AOTStubHelpersStorage.cpp:210-211`). Without it: the owner is old and black, the Extension young; the next Eden collection does not visit the owner, does not mark the Extension, and sweeps it. A loser of the compare-and-swap that stores a number runs no barrier either.
**Proposed:** CO6 and SW7 get "then the owner barrier, before the publishing thread's next safepoint".

**(b)** 06 6.3: a conversion publishes the new storage "**then**" changes the Structure under the object lock; no nuke is named. The marker reads StructureID, butterfly, StructureID again (`runtime/JSObjectVisitInlines.h:347-380`) and accepts (old Structure, new storage). "It takes the kind from the storage word" repairs `visitElements`. It does not repair two decisions that are made **from the Structure before the butterfly is loaded**: whether to hold the cell lock (ArrayStorage) and how to find the base of the allocation (`preCapacity`, `hasElementsStartOffset`). With (Contiguous Structure, ArrayStorage storage) K reads `m_vector`, `vectorLength()`, `m_sparseMap` without the lock that every mutator access now takes.
**Proposed:** if the kind in the storage word is not the kind of the Structure's indexing type the visit gives up (`didRace`), and the cell is visited again at the next stop.

### F14. R5 A16, A17: F1 names compiled stores; the C++ run time publishes through plain stores (M on ARM64)

F1 lists "slot of a published object or environment, shared variable, element or property in storage, shared region of the Instance, any shared table of the run time". It does not list the internal fields of C++ cells: `WriteBarrierBase<T>::set`, `JSInternalFieldObjectImpl::internalField(i).set`, `InternalFieldTuple`, `JSPromiseReaction`, bound functions, iterators, the inbox. 06 and 09 name release stores protocol by protocol.

1. T1 (C++): `cell = X::create(vm)` (plain initializing stores; `mutatorFence()` is no fence while nothing marks, F4), `holder->m_field.set(vm, holder, cell)`: a plain `str`.
2. T2 loads `holder->m_field` and then `cell->structureID()`. ARM64 may show the reference before the header.

On x86-64 the same pair is a data race in C++, and the compiler may sink an initializing store below the publishing one.

**Proposed F1'.** In mt builds `WriteBarrierBase<T>::set`, `setWithoutWriteBarrier`, `AuxiliaryBarrier::set` and `WriteBarrierStructureID::set` store with `std::memory_order_release`. `setEarlyValue` and stores inside an `ObjectInitializationScope` stay plain. One `stlr` per store on ARM64, nothing on x86-64. Then no protocol has to remember it.

### F15. R5 B4: `record()` appends to a `Vector` on shared sets too (M)

08 8.10: "per-thread registries; the shared sets (statics, string cells) are those whose cards are written without record". In the source the flag does not change `record()`: it sets the card and appends to `m_writtenCards` whatever `m_cardsAreWrittenWithoutRecord` says (`heap/RememberedSlot.h:47-59,61-66`). C++ writers of shared regions call it (`RememberedSlot<T>::set`). And only two sets have the flag (`aot/runtime/AOTProgramStrings.cpp:69`, `AOTProgramStatics.cpp:196`); built-in cells and values, closure cells and class prototypes are shared regions by 07 7.3.2 without it.
**Proposed:** a set over a shared region is made with `SharedWriters`: `record()` is the byte store alone, `m_listsEveryWrittenCard` is false for ever.

### F16. R5 2.6: members without a row in 08

08 8.11 promises "the appendix has the row for each member of R5 2.6". These have no rule, or half of one.

| Member | What goes wrong | Proposed disposition |
| --- | --- | --- |
| `m_mutatorDidRun`, `m_mutatorExecutionVersion` | Set by the mutator after `stopIfNecessarySlow` and `acquireAccessSlow` (`heap/Heap.cpp:2095,2151,2189`), which 8.3 replaces. If the store is lost in the rewrite the version stands still and `GreyedByExecution` constraints ("Cs", "Msr", "At") are taken as up to date: missed roots (M) | `cancelStop()` sets `m_mutatorDidRun = true`, on the conductor |
| `m_detachedWeakBlocks`, `m_pooledWeakBlocks`, counts | `WeakSet::allocate` under one block's header lock takes from and returns to lists of the **heap** (`heap/Heap.cpp:2799-2850`): two blocks, two locks, one list (M) | leaf lock `Heap::m_weakBlockPoolLock`, below the header locks |
| `m_hasAllocatedSinceLastCollection` | Idempotent store of `true`; read while stopped. Harmless | state it: relaxed atomic |
| `m_bytesAbandonedSinceLastFullCollect`, `m_deprecatedExtraMemorySize`, `m_numberOfLargerCells` | lost updates (V) | relaxed atomic adds |
| Activity callbacks | `Heap::didAllocate` calls `m_edenActivityCallback->didAllocate()` from every thread's slow path (`heap/Heap.cpp:2694-2695`), which arms a timer of the **main** loop from another thread. Bun's timers are per `VirtualMachine` and not `Sync` (not verified how `JSRunLoopTimer` reaches them) | a spawned thread posts to the main inbox, at most once per cycle |
| `m_stopIfNecessaryTimer->scheduleSoon()`, `drainInParallelPassively` (`heap/SlotVisitor.cpp:893,966`) | 8.1 names them and gives no replacement | the first is compiled out (the collector thread conducts); the second tests "some thread has `HasAccess`" |
| `m_worldIsStopped` | Does a safepoint operation set it? `assertIsMutatorOrMutatorIsStopped()` and `visitor.mutatorsAreStopped()` read it; the latter switches the marker to unfenced visits | set by collections only; a safepoint operation has `m_mutatorsAreParked` of its own (F7 a) |
| `pruneUnusedSourceProviderCaches()`, `sweepEagerlyInEpilogue()`, `immutableButterflyToStringCache.clear()` | parts of the epilogue (`heap/Heap.cpp:2319-2352`) that the table of 8.12 does not list | one row each |

### F17. R5 2.6-20, D15: a `Strong<>` that is released on another thread than it was made on (M)

08 8.2 shards `m_strongSet` per thread. `StrongSet::deallocate(slot)` finds the set through the slot's block and pushes on **that set's** free list, unlocked (`heap/StrongSet.h:160-179`; "every other mutation must run with the JSLock held", `:45`). The design creates such handles itself: tickets of `asyncJoin`, `asyncHold`, `waitAsync` hold "the promise (rooted)", are made by the registering thread and are settled by another (09 9.13); the `ThreadStart` record is made by the parent and released by the child (09 9.2). 11 11.4 solves this for Bun's `Strong` ("dropped elsewhere it is posted home"), nothing does for the engine's.
**Proposed:** `deallocate` compares the set's thread with the current one and, if they differ, pushes the slot on a lock-free return list of the owning set, which `allocateSlow` drains.

### F18. New: deadlock on `VM::m_threadsLock` against the lock order of 08 8.15 (L)

8.15: heap access is "the outermost lock: whoever waits for anything above `object` has released it". `m_threadsLock` is above "object". `requestStop()` sets `m_stopRequested` **first** and takes `m_threadsLock` **second** (8.4); `cancelStop()` "undoes it", under the same lock.

1. T1 holds `m_threadsLock` (allocating a thread ID, 09 9.2 step 2). T2 waits for it, access released as 8.15 demands.
2. G takes `m_conductorLock`, stores `m_stopRequested = true`, blocks on `m_threadsLock`.
3. T1 unlocks. T2 gets the lock (barging is allowed), leaves its `ReleaseHeapAccessScope`: `acquireAccess()` sees the request and waits for `!m_stopRequested`, **holding the lock**.
4. G waits for T2. T2 waits for G.

The pattern "release access, take the lock, take access back" deadlocks with every lock that a conductor or a closure also takes.

**Proposed rule H12.** The conductor takes `m_threadsLock` before it stores `m_stopRequested` and keeps it until after `cancelStop()`. (T2 then either gets the lock and its access before the request exists, or does not get the lock.) Code that runs during a stop does not take it again: "under `m_threadsLock`" in 09 9.7 (`m_microtaskQueues`) and 9.10 becomes "under `m_threadsLock`, or by the conductor". No closure and no phase of the collector takes the modules lock or `prototypeChainLock`.

### F19. R5 D3 (first half): the stop marker has three writers, and handling a trap disarms the polls (L)

08 8.4 describes the request twice: `requestStop()` stores the marker "in its `StackManager` and its view's mirror", and "a stop for the collector is one more reason (`VMTraps::NeedStopForHeap`)". The source ties the two together in a way that neither description survives.

- `VMTraps::handleTraps` **takes** each bit it handles (`clearTrapWithoutCancellingThreadStop`, `runtime/VMTraps.cpp:476`) and, on leaving, calls `updateThreadStopRequestIfNeeded()` (`:484`), which calls `StackManager::cancelStop()` when no bit is left (`:425-443`): marker removed, poll page readable.
- `operationAOTPollAtPrologue` calls `handleTraps(AsyncEvents)` under `DeferGCForAWhile` (`aot/runtime/AOTExceptionOperations.cpp:60-72`). 8.4 keeps it "unchanged. The thread goes on to its next real safepoint".

As a trap bit: (1) G sets `NeedStopForHeap` in T and arms T's polls. (2) T enters a function; the prologue poll takes the bit, cannot park, leaves; no bit is left; T's polls are disarmed. (3) T enters `for (;;) { if (o.done) break; }`. Its loop poll never fires. The stop never completes.
As a direct store: (1) G arms T. (2) A watchdog or `node:vm` timeout trap is handled on T; `updateThreadStopRequestIfNeeded()` sees no bit and cancels: the same. Conversely `Heap::cancelStop()` "undoes it" while a termination request is pending, and the termination is lost until something arms the polls again. `setStopsAtFunctionEntries` (the owed `RangeError`) writes the same word (`runtime/StackManager.cpp:73-83`).

**Proposed text.** The request is a trap bit, set and cleared under `m_trapSignalingLock`, **by the conductor only**. No handler takes it: `takeTopPriorityTrap` skips it; `handleTraps` calls `heap.stopIfNecessary()` when it is set and the caller's mask contains it; `operationAOTPollAtPrologue` passes a mask without it. The marker and the poll page are written by `updateThreadStopRequestIfNeeded()` alone, whose `shouldStop` includes the bit.

Cost to note in 8.5: while a stop is pending every function entry of a thread that has not parked yet takes the C++ slow path. And on Linux x86-64 a stop costs two `mprotect` calls per thread, each with a TLB shootdown; the 0.04 ms of #249 was measured without them, and a concurrent collection has several stops.

### F20. R5 D3 (second half): `MarkRecursiveEntries` and the bounds of 08 8.5 (L)

**(a) The cut is not stable.** The pass runs "after `BuildCallGraph`, before `ComputeEffectSummaries`". Both are results that are computed again whenever a pass invalidates them; `InlineCalls` runs in the `Optimize` schedule after them and invalidates everything (`aot/pipeline/AOTPipeline.cpp:146-147,203,311`), and `InlineHeads`, `OutlineColdOperations`, `SplitCoroutines`, `CreateEntrypoints` change the graph later.
1. Cycle `f -> g -> f`; the greedy cut picks `g`.
2. `InlineCalls` pastes `g` into `f`. The cycle is `f -> f`. `g`'s prologue, and with it the poll, is gone.
3. If the pass does not run again, `fib`-like recursion in `f` has no safepoint. If it does run again and picks another set, a caller that was optimized under "`f` does not collect" is wrong, unless nothing has yet used the summary for a `window` value or a barrier.

**Proposed S3'.** The flag is a property of the final graph: the pass runs last among the passes that change calls (after `InlineHeads`), before `ElideStackChecks`, `ChooseRepresentations`, `RemoveRedundantBarriers`. Before that, every function in a non-trivial component counts as `collects`. Inlining a flagged function leaves a `LoopHint` at the pasted entry. The verifier checks S3 on the final call graph as it checks `LoopHint` on CFG cycles (`InternalErrorKind::CallCycleWithoutSafepoint`). Edges include calls through natives that call back (`indirectCalls`), tail calls, accessors.

**(b) "`NoSafepointScope`: short by construction" is not so in the source.** `ScriptExecutable::prepareForExecutionImpl` holds `DeferGCForAWhile` over parsing, bytecode generation and linking (`runtime/ScriptExecutable.cpp:405-408`): linear in the text passed to `eval` or `new Function`, tens of milliseconds for what zod or ajv generate, and with I1 once per thread. `ErrorInstance::computeErrorInfo` holds it over the embedder's source-map hook (`runtime/ErrorInstance.cpp:495-515`). `operationAOTBuiltinCell` holds it over the loading of a module (F6). **Proposed:** the list W-8.6 gets a second half, "scopes that are not short", each with the way it is split.

**(c) "Stubs: bounded by a small constant".** True of `growVector` (`maxLengthForElementwiseCopy`). The C++ growth of 06 6.3 copies the whole vector between `Retired` and the publishing store, a window in which other writers **spin** (`acquireStorageForWriting`). For an array of 10^8 elements that is 0.1 s of spinning per waiting thread and of delay for every stop. Bound it in 8.5 ("by data size"), and let the spinner fall back to `sched_yield` after some turns.

**(d)** S3 bounds cycles, not paths. A call tree without cycles, loops or allocation is exponential in its depth. Only generated code gets there; say that the bound is "the length of the longest acyclic path".

### F21. R5 D12, 2.6-1: the waits of 08 8.3 and 8.10 are named, not specified (L)

"wait until `!m_stopRequested`", "`notifyConductor()`", "wait until no thread's `m_access` is `HasAccess`", "waits as parked" (for `collectSync`, an exhausted cage, a pending closure). The Dekker pair makes the *decision* race free; a lost wake-up is in the *sleep*.
1. G reads `T.m_access == HasAccess` and decides to sleep.
2. T stores `Parked` and calls `notifyConductor()`: nobody sleeps yet.
3. G sleeps. No other thread will notify.

**Proposed text.** The conductor waits on `Heap::m_stopCondition` under `Heap::m_stopLock`, re-reading all `m_access` words under the lock before each wait; `notifyConductor()` takes the lock, then notifies. Parked threads use `ParkingLot::compareAndPark(&m_stopRequested, true)`; `cancelStop()` stores `false`, then `unparkAll`. A requester of a collection waits for its ticket on `Collector::m_threadLock`'s condition, in state `Parked`, and does not return to `HasAccess` before both its ticket is served and no stop is requested. A requester of a safepoint operation waits on a `done` flag in its queue entry as well as for the lock, and leaves the queue for the lock when `done` is set. Both locks are leaves in 8.15.

### F22. R5 D14, 2.6-27: `preventCollection()` (C/L)

`heap/Heap.cpp:3428-3444`: take `m_collectContinuouslyLock`, wait for all collections, then "a collection can only start if this thread starts it", `RELEASE_ASSERT(!m_collectionScope)`. With N mutators any other thread's slow path requests a collection in the next instant. Users: both heap snapshot builders, `Heap::deleteAllCodeBlocks` and friends (`heap/Heap.cpp:1176,1228,1628`). 08 8.6 makes "heap walks" safepoint operations and does not mention the scope. A snapshot builder allocates and runs for seconds, so it does not fit "a closure does not block".
**Proposed H13.** `PreventCollectionScope` is a count in `Heap` under `Collector::m_threadLock`. While it is above zero the collector thread serves no request; requesters that need the collection (`collectSync`, exhausted cage) wait as parked. It nests and may be held by several threads. It does not stop mutators: a user that needs them stopped opens a safepoint operation inside.

### F23. New: the collector thread has no `ThreadContext`, and the End phase runs code that needs one (C)

08 8.10: the collector thread always conducts. 07 7.2: `VM::topCallFrame`, `m_exception`, traps, deferral depth "are not data members but inline accessors that forward to `ThreadContext::current()`". Today "in a Bun process the JavaScript thread is nearly always the conductor" (R5 2.5), so this code has run on a mutator:
- `ErrorInstance::reconcileWeakReferencesAtGCEnd` calls `computeErrorInfo`, which opens `DeferGCForAWhile` (per-thread depth) and calls the embedder's hook (`runtime/ErrorInstance.cpp:470-515`).
- Bun's hook opens `DeferTerminationForAWhile`, `SuspendExceptionScope`, `DECLARE_THROW_SCOPE` (`bun:src/jsc/bindings/FormatStackTraceForJS.cpp:647-672`) and remaps positions through `bunVM(..)`, which in mt builds "reads the thread-local" (11 11.4): null.
- `clientData->reconcileWeakReferencesAtGCEnd`, `vm().reconcileWeakReferencesAtGCEnd()` (`heap/Heap.cpp:797-800`).

**Proposed H14.** The collector thread owns a `ThreadContext` of kind `Conductor`: exception, traps and deferral words, no allocators, no view, not in `m_threads`. Embedder hooks that can run in the End phase are listed, and each takes what it needs as an argument; `Bun__getVM()` on the conductor answers the main thread's VM only for hooks that are declared to tolerate all threads being parked.

### F24. New: the window of 05 5.10 contains safepoints (C)

"The conversion .. runs as a safepoint operation. A reader in `tryGetDeclaredMember` is C++ without a safepoint, so none is in the middle of it." The function calls `findDeclaredMethod`, which makes the method's closure (`closureObject`, `aot/object/AOTDeclaredMembers.cpp:81-89`), and `createDeclaredAccessor`, which allocates a `GetterSetter` (`:182-188`). Both are allocations, hence safepoints, and they lie between the read of `m_classIndex` and `RELEASE_ASSERT(classInfo() == info())` (`aot/object/AOTClassPrototype.cpp:90-109`).
1. T1 is in `findDeclaredMethod`, its allocation takes the slow path, T1 parks.
2. T2's safepoint operation converts the prototype.
3. T1 resumes and aborts on the release assertion (M11).
**Proposed:** after the two calls the function loads the StructureID again and returns `std::nullopt` if the object is no stand-in any more (CO10).

### F25. R5 D4: locks that are in neither H1 nor the order of 08 8.15 (L)

| Lock | Where | Problem |
| --- | --- | --- |
| `VM::m_nativeSerialLock` | 11 11.1 | "released around calls back into JavaScript and around blocking". Not around allocation. T1 holds it and parks at an allocation; T2 waits for it with access (H1 does not list it): the stop never completes |
| `JSGlobalObject::m_declarationLock`, `CodeCache` lock, source provider locks | 10 10.3 | position in the order not given; `executeEval`'s check phase allocates |
| inbox lock of `ThreadContext::post` | 09 9.7 | taken by a settling thread that may hold the promise's busy bit (PR7: "nothing parks"); must be a leaf below it |
| `JSLock::m_embedderLock` | 09 9.3 | an outermost lock taken without a `ThreadContext` |
| claim words (Z2), the promise's busy bit (PR1), `Retired` storage (06 6.3) | | waits that are not locks: the debug rank checker of 8.15 does not see them |

**Proposed:** each gets a line in 8.15. Rule: a lock under which a thread may reach a safepoint is above "object", and its waiters release access (with the caveat of F18); every other lock opens a `NoSafepointScope`. `m_nativeSerialLock` is of the first kind.

### F26. New: `process.exit()` from a spawned thread (L; Contradiction within 09)

09 9.11: it "posts to the main thread if called elsewhere, and blocks". 02 2.6: "no job runs inside `join`, `hold`, `wait` or `Atomics.wait`".
1. Main is in `t.join()`. 2. T calls `process.exit(1)`: posts, blocks. 3. Main never takes the task; T never ends.
09 9.10 describes the same call otherwise: "Termination of the VM (`process.exit`, ..) fanned to each thread's trap word .. Blocked threads are woken", and threads unwind; 9.11 says "they are not unwound".
Also open: the main thread then leaves through libc's `exit`, whose `atexit` handlers and static destructors run while other threads still execute compiled code and the collector thread may be in the middle of a stop. **Proposed:** `process.exit` on T sets the main thread's trap (`NeedExit`), which every blocking primitive checks (09 9.13 has the check); `exit` handlers run on main; then the caller requests a stop that is never cancelled and calls `_exit`-like termination with all threads parked or without access.

### F27. R5 E5, E6, 2.6-35: one `(thread, version)` per `WeakRef` (V)

08 8.11: the `WeakRef` "records `(thread, version)` of its last `deref`".
1. T1 calls `w.deref()` in a long job: record `(T1, 7)`. It drops the result.
2. T2 calls `w.deref()`: record `(T2, 3)`. T2's job ends: T2's version is 4.
3. A collection: `(T2, 3)` is out of date, the target is not held, it dies.
4. T1, in the same job, calls `w.deref()` again: `undefined`. 02 2.6 promised it "until the end of the current job of the thread that called it".
Two fields written by two threads can also tear, and thread IDs are reused.
**Proposed:** the standard's own device: `ThreadContext::m_keptAlive`, a vector of cells that `deref()` and the `WeakRef` constructor append to, a root of the thread, cleared where the version is bumped today. The `WeakRef` needs no field.

### F28. New: 04 4.6.3 and 06 6.3 describe two different slow paths for a store into an array (Contradiction)

04: `MayWriteStorage (storage) -> boolean ; reads(ArrayStorage)`; a failing test is "an ordinary failing test" that leaves for the generic version (4.8). `PutElement` is `writes(Elements[Array])`, not `collects`.
06: the slow branch of the tag compare is the stub `acquireStorageForWriting(a, b)`, which **writes** the storage word (compare-and-swap to `Shared`), **returns another `b`** (after `Retired`), and calls `operationAOTMakeStorageWritable`, which allocates.
An SSA value `Storage` cannot change under a test that returns a boolean; an operation declared `reads` must not write; an allocation needs `collects` or a scope (F6). If 04 is meant, the first store by a second thread throws the whole activation into the generic version, and every later activation too until some generic store has made the storage `Shared`: say so, and count it in 1.6. If 06 is meant: `AcquireStorage (array, storage) -> writable(Storage), miss ; reads writes(ArrayStorage)`, a terminator that yields a new `Storage`, with `ReadOnly` on the `miss` edge.

## B.4 Ordinary objects, the interpreter, native code (reports R7, R8)

Most severe first. T, U are JavaScript threads, `b` a loaded storage pointer.

### F1. R7 A7, SW6: the fallback to `ArrayStorage` moves headers inside one allocation

06 section 6.3 forbids `tryMoveElementsStartForward` for storage that is not `Confined`, because it breaks SW6, and sends long shared arrays to "what upstream JSC does for them: `ArrayStorage`, whose every access is C++ under the object lock". Upstream's `ArrayStorage` does the same thing. `shiftCountWithArrayStorage` (`runtime/JSArray.cpp:1834-1841`) and `unshiftCountWithArrayStorage` (`:1998-2008`) call `Butterfly::shift` / `unshift` (`runtime/ButterflyInlines.h:285-316`): a `memmove` of out-of-line properties, header and `ArrayStorage` fields by `n` slots **in the same allocation**, then `setButterfly`.

1. U runs `o.x` (N1, no lock) or `a[i]` in compiled code and has loaded `b`.
2. `a` has two out-of-line properties. T, under the object lock, runs `a.splice(0, 2)`, which ends in `shift(structure, 2)`. The words at `b-16` and `b-8` now hold what were out-of-line properties 1 and 0: JavaScript values that the program chose.
3. U executes `cmp8 [b-12], kind` and `cmp32 i, [b-8]` on those values. A double with chosen bits gives kind `Contiguous` or `Double`, state `Shared` and any length: out-of-bounds read, and through the tag compare an out-of-bounds write.

With `unshift` the old property slots are overwritten by header words and elements, so a lock-free reader of a named property loads `m_sparseMap` or `m_indexBias | m_numValuesInVector` as a value.
The object lock does not help: readers of named properties and the first three instructions of every compiled element access take no lock.

**Proposal.** SW6 gets a second sentence: "This includes `ArrayStorage`: in mt builds `Butterfly::shift` and `unshift` are used only on `Confined` storage. On other storage `shiftCount` and `unshiftCount` move the elements inside `m_vector`, or replace the storage (retire, or safepoint operation); `m_indexBias` is 0 for its whole life."

### F2. R7 A11, SW3: a racing `push` leaves a value above `publicLength`, and the collector does not look there

SW3 says that slots from `publicLength` to `vectorLength` hold holes "except for the moment inside an operation". With two writers on `Shared` storage the exception is permanent:

1. `publicLength` is 10. T begins `a.push(x)`: `len = 10`.
2. U runs `a.length = 0` (or `pop`): holes from 0 to 9 by its own loaded length, then `store32(b-8, 0)`.
3. T: `store64(b+80, x)`. (If T's `store32(b-8, 11)` comes before U's store of 0, the end state is the same.) Slot 10 holds `x`, `publicLength` is 0.
4. The visitor marks `butterfly->contiguous().data()` up to `publicLength()` only (`runtime/JSObjectVisitInlines.h:103`). `x` dies at the next collection.
5. Any thread runs `a[20] = y`: the sequence "`publicLength <= i < vectorLength`" stores the element and sets the length to 21, trusting SW3 for the slots between. `a[10]` now yields a dangling pointer.

The report rates A11 as a wrong value. Under the design's own sequences it is use after free.

**Proposal.** (1) "**SW3'**. `publicLength <= vectorLength`. A slot at or above `publicLength` holds a hole *or a value that some thread stored there*." (2) "In mt builds the visitor of storage that is not `Confined` visits all slots below `vectorLength`" (SW4 makes that safe). (3) 02 section 2.10.5, row `a.length = n` against `push`: "an element that a racing `push` stored may reappear when the length grows again".

### F3. R7 O4: stealing the property table "for an unpublished object" is unsound

06 section 6.5: the table is cloned, never stolen, "unless the transition is for an unpublished object". The table belongs to the **Structure**, not to the object. Other, published objects have the same Structure, and their readers are lock-free: C++ `Structure::get` (`runtime/Structure.h:1036-1051`), and the x86-64 stub `findInPropertyTable`, which probes the table of any Structure that has one, also up the prototype chain (`aot/stubs/AOTStubsPropertyRead.cpp:116-161, 372-461`), not only of dictionaries as 6.5 says.

1. U reads `p.y`; `p` is published and has Structure S. U loads S's table pointer.
2. T builds a new object `q` (unpublished) with Structure S and adds `y`. The transition S to S' steals the table and adds `y` at the next offset, perhaps with a rehash.
3. U finds `y` in what it takes for S's table and loads `p`'s slot at an offset that `p`'s storage does not have.

**Proposal.** Drop the exception. "A table that has been reachable from a Structure in the transition tree is never edited again, unless that Structure is a dictionary. A builder that wants to avoid n clones makes the whole run at once (`PropertyRunAddition`, `createWithPropertyAdditions`), with one table for the last Structure." Cost of cloning: one copy per *new* Structure, so quadratic in the length of a chain that is built for the first time, up to `s_maxTransitionLength` = 128, or 512 for `put_by_id` (`runtime/Structure.h:219-220`); repeated shapes hit the transition table and clone nothing. Each Structure keeps its clone until a collection drops unpinned tables.

### F4. NEW-1, N2, N3: an offset is a function of the Structure, so a list in the table cannot delay its reuse

N2 keeps "two lists, quarantined and free" in `PropertyTable`. That works for a dictionary, whose table belongs to one object. For other Structures deletion is a cached transition (`removePropertyTransitionFromExistingStructureImpl`, `runtime/Structure.cpp:960-977`), and the offset that a later addition takes is part of the Structure that the addition leads to; materialization asserts it (`:577-593`, `nextOffset == structure->transitionOffset()`).

1. Long ago some object went S1 (has `a` at offset k), `delete a` to S2, add `b` to S3. S3 says `b` is at k. The quarantine of that time is over.
2. Now U reads `o.a`: loads StructureID S1, learns "data at k".
3. T runs `delete o.a; Object.defineProperty(o, "b", { get })`. Both steps find existing transitions. No table is edited and no list is consulted. T stores a `GetterSetter` at k.
4. U loads slot k: a `GetterSetter` as the value of `a`. M3 and M7 of 02 are violated, and an internal cell escapes to the program.

N3 does not apply: `b` is a new property, not a redefinition.

**Proposal.** "**N2'**. In mt builds an addition transition out of a Structure that is not a dictionary never takes a deleted offset: `nextOffset()` ignores `m_deletedOffsets` unless the Structure is a dictionary. The hole stays for the life of the object or until it is flattened in a safepoint operation. The quarantine of N2 applies to dictionaries; tables with a non-empty quarantine are registered in a list of the heap, which the end of a collection walks."

### F5. R7 S9, S7, 10.5-3, 10.1.5-1: the shortcut in `deref` contradicts "refcount 0 is final"

06 section 6.7 keeps the shortcut: "when this is the only reference nobody else can be racing to add one". For an atom that is false, because the table holds a pointer that is not counted, and a lookup makes a reference out of it. And on the shortcut the count never becomes 0 (`wtf:text/StringImpl.h:1218-1229`: load, equal to one increment, `destroy`).

1. T holds the last reference of atom A. `deref`: loads 1 increment, skips the `fetch_sub`.
2. U looks the same text up, under the shard's lock: finds A, `tryRef` sees a count that is not 0 and succeeds.
3. T: `StringImpl::destroy(A)`, takes the shard's lock, removes A, frees it. U holds freed memory.

Two more holes in the same row.
(a) If `tryRef` fails, U "makes a new one" while the dead atom is still in the set. `remove` looks up by hash and **pointer** and ends in `RELEASE_ASSERT(wasRemoved)` (`wtf:text/AtomStringImpl.cpp:555-569`). So U must overwrite the dead atom's bucket, and T's `remove` must accept that its pointer is gone.
(b) `remove` uses the table of the **current** thread. In mt builds the collector thread always conducts and sweeps precise allocations in the End phase (08 section 8.12); cells of classes that keep the default `numberOfLowerTierPreciseCells = 8` (`runtime/JSCell.h:107`) and hold atoms (`Symbol`, `PropertyTable`, unlinked executables) are destroyed there, on a thread whose table is not the VM's: the same assertion.

**Proposal.** "The shortcut is taken only if `!isAtom()` (one test of a word that is loaded anyway). For an atom `deref` is always `fetch_sub(release)`; the thread that reaches 0 takes the shard's lock and removes the bucket only if it still holds this pointer. A lookup that finds a count of 0 replaces the bucket's pointer by its new atom. Collector, sweeper and helper threads of an mt VM install the VM's table, or the atom's shard is found from the VM that the destructor is running for."

### F6. R8 B11, B12, B13: the owner test looks at the receiver only

11 section 11.3 tests `receiver`. Natives also take instances of `owner` classes as **operands**, and many of those natives are `any`: `fetch(request)`, `new Response(blob)`, `Bun.write(file, response)`, `structuredClone(x)`, `stream.pipeTo(other)`, `console.log(socket)`, `Buffer.concat`, `postMessage(port)`.

1. T makes `const r = new Request(..)`. U calls `fetch(r)`. `fetch` has no receiver and is `any`: no test.
2. The body casts the operand to `&mut Request` or `&Request`, and touches `Cell`, `RefCell`, `RefCount` fields while T runs `r.text()`.

That is hazard B12 / B13 unchanged.

**Proposal.** "**The owner test belongs to the cast, not to the call.** Every way from a `JSValue` to the native struct of an `owner` class makes it: bindgen's typed operands (the emitter of 11.3 puts it in front of a `directEntry`; the generated wrapper in front of a host call), `T::from_js`, `jsDynamicCast<JS..*>` through one helper `toWrappedOnThisThread`. A lint fails Bun's build for a direct downcast to an `owner` class outside the helper. A native that only passes such an operand on (stores it, returns it) declares `b.inspected` and needs no test."

### F7. NEW-2, R8 B10: adopted objects outlive their `VirtualMachine`

"When a thread ends, its blocks are adopted by the main thread, and with them its objects" (11.3), after `threadWillEnd` has done "what a Worker's exit does". A Worker's objects die with its heap. A thread's objects do not: any of them may be reachable from another thread.

1. T makes a `Timeout`, a `Subprocess`, a `Blob` reader. Their structs hold raw pointers to T's `VirtualMachine`, `EventLoop` or uws loop (`bun:src/runtime/timer/WTFTimer.rs:45`; 88 such declarations in `bun:src/runtime` and `bun:src/jsc`), slots in T's list of `Strong` roots (11.4: "one list per thread, in the Bun VM"), memory from T's mimalloc arena.
2. T ends. Its Bun VM, `RuntimeState`, arena and `Strong` list are destroyed.
3. The main thread calls `timeout.refresh()`. The owner test passes, since the block is main's now. The method follows the stored pointer: use after free. The same in the destructor when main sweeps the block (`Strong::destroy` indexes a list that is gone).

**Proposal.** "Blocks of an ended thread get the owner `Nobody`. Every `owner` test fails on them (`ConcurrentAccessError("the thread that created this object has ended")`). What native structs can point into (the `VirtualMachine` struct, `RuntimeState`, the `Strong` list, the arena) is reference counted by the thread's affine blocks and freed when the last of them is empty. Their destructors run on the main thread with a flag `owner_has_ended`, under which a destructor releases memory and file descriptors and touches no loop. Classes whose instances are meant to survive their thread (results handed to `join`) are declared `any` or are copied."

### F8. R7 A9: the kind in the storage word is not all that the marker takes from the Structure

06 section 6.3: a conversion publishes the new storage, "**then**" changes the Structure; the collector "takes the kind from the storage word". The visitor derives four more things from the Structure that it read (`runtime/JSObjectVisitInlines.h:51-80`): whether the butterfly is a `JSCellButterfly` to be appended **as a cell** (`isCopyOnWrite(structure->indexingMode())`, `:59-62`), the pre-capacity (`m_indexBias` or the start offset, `:65-70`), the out-of-line capacity, and from these the base address for `markAuxiliary`.

1. `a` is copy-on-write. T wins the compare-and-swap of the pointer (6.3, "Copy-on-write"): `a+8` names ordinary auxiliary storage, and the Structure still says copy-on-write.
2. A concurrent marker loads that Structure and the new pointer, and appends `JSCellButterfly::fromButterfly(b)`: out-of-line slots and the storage word are taken for a cell header.

The same pairing for Contiguous to `ArrayStorage` gives a wrong base: the mark bit of the wrong atom is set and the real allocation is freed while in use. Today the nuke protocol excludes these pairs (`:126-200`). 6.4 keeps it for added properties; 6.3 does not mention it for conversions, copy-on-write, or freezing in place.

Two related points.
- **A safepoint operation does not stop markers.** 08 section 8.6 says that a collection and a safepoint operation "never overlap", but only the stop-the-world phases take `m_conductorLock`; in the Concurrent phase mutators run (8.10), so a closure can run beside markers. Closures that flatten, convert or replace must keep the marker's protocol.
- **SW1 against `Confined`.** `Confined` storage converts in place "by today's code", so its kind byte is rewritten. For Double to Contiguous the byte must be stored after the last slot, with a store-store fence, or the marker reads raw doubles as values.

**Proposal.** "**SW10**. Whatever the visitor needs to find and scan a storage comes from the storage word of the pointer it loaded: kind, allocation class (auxiliary, own cell, `JSCellButterfly`, outside the heap: 2 bits), pre-capacity, out-of-line capacity (16 bits of the low half, which is free for storage that is not `Confined`). From the Structure it takes only the number of out-of-line slots in use, which by N1 never exceeds the capacity of the storage loaded after it. SW1 holds for storage that is not `Confined`; `Confined` storage stores its kind byte last, behind a store-store fence. A safepoint operation may run beside markers."

### F9. R7 O2, A12: three cases of replacement that 6.3 and 6.4 do not have

**(a) First install.** An object without a butterfly has no storage word to compare-and-swap. T adds the first out-of-line property under the object lock. U runs `o[0] = 1`, which creates the first indexed storage in C++; 6.3 never says that this takes the lock. Both store `o+8`. If U's store is last, the Structure promises out-of-line capacity that the storage lacks: a read in front of the allocation.

**(b) `Owned(other)`.** The procedure of 6.4 has "retire the old storage (`Owned(me)`)" and "`Shared`: see below". A thread that adds a property to an object whose storage another thread owns is in neither case. In step 2 (store the value) the state test must run too: the owner may be growing the *elements* of the same butterfly at that moment, without the object lock (`growVector`).

**(c) Storage allocated before the stop.** "The new storage is allocated before" the safepoint operation. Named properties and elements share the butterfly. T queues "grow the vector", U queues "grow the property capacity" for the same object, and both closures run in one stop (08 section 8.6, step 3). The second closure's storage was sized from dimensions that the first has changed. "Check that growth is still needed" does not see that: the copy drops properties or overruns.

**Proposal.** "**SW11**. Null to first storage, and every replacement that changes the out-of-line capacity, happens under the object lock; the first install is a compare-and-swap of the cell's word from null. A thread that must replace storage in state `Owned(other)` first makes it `Shared`. A closure that replaces `Shared` storage reads kind, capacity and vector length again, and allocates inside the stop (allowed by 8.6, point 4) if what it brought does not fit. Storage that C++ allocates is `Owned(current thread)`, except that the successor of `Confined` storage of the same object is `Confined`."

### F10. R7 A13, SW2: slots that are initialized after publication have no order for readers (ARM64); step 3 has no state test

Growth, step 3: "room left in the size class: fill holes; store-release `vectorLength`". The slack behind `vectorLength` is not initialized when the storage is allocated (`runtime/JSObject.cpp:4175-4190` clears it at this point).

1. Owner T: fills holes in `[vl, vl')`, releases `vl'`, stores the element at `i >= vl`, then `store32(b-8, i+1)`, plain.
2. Reader U, ARM64: `b` was loaded long after the storage was published, so the address dependency on `b` orders only what preceded SW7's store. U loads `publicLength = i+1`, passes the test, loads slot `j` with `vl <= j < i`. Load to load through a branch is not ordered. U may get the bytes from before the hole fill: what the memory held in its previous life, taken for a value of the kind.

The read sequence of 6.3 has no acquire, and should not get one.

Also: step 3 comes before the compare-and-swap of step 4, which is the only test of the state. If the storage has become `Shared`, two threads can be in step 3. The slower one fills "holes" over an element that the faster one has already pushed into the new room: a lost store, without a race in the program.

**Proposal.** "SW2: all slots up to the end of the allocation's size class hold holes **before the storage is published**. Growing `vectorLength` in place is then one store, made only by a thread that has seen `Owned(itself)` in this window. `Shared` storage is born with `vectorLength` equal to what its size class holds."

### F11. R8 C1, R7 7.10-10, 7.10-25: bytes that a native borrows

TA1 keeps detached memory mapped "until every thread has passed a safepoint". A thread that has released heap access counts as past one. H1 and N-g *require* the release around blocking system calls, and N-g only forbids touching cells.

1. U: `fs.readSync(fd, buf)` on a spawned thread. The native takes `&mut [u8]` by TA8, enters `ReleaseHeapAccessScope`, calls `read(2)`.
2. T: `buf.buffer.transfer()`. The old contents go to the deferred-free list.
3. Any stop completes, since U has no access. The conductor frees the list. The kernel writes into freed memory.

Second class: algorithms that are memory safe only if the input does not change. `sort()` takes a snapshot only for `isShared()` (`runtime/JSGenericTypedArrayViewInlines.h:941-963, 1253`); `std::sort`'s unguarded loops and the radix scatter leave the range if elements change under them. Natives that measure and then write (length of the UTF-16 form of UTF-8 bytes, then the conversion into a buffer of exactly that size; "is all ASCII", then a copy) overrun their *output*. The report rates 7.10-25 as a wrong value. With "every buffer races like a SharedArrayBuffer" (6.8) it is an overflow. Rule A4 has the same defect for arrays: it lets `sort` work "on the array itself".

**Proposal.** "**TA9**. A span that is used while heap access is released, or handed to another OS thread, is pinned. `pin` and `detach / transfer / resize` exclude each other under the buffer's lock, and a pinned buffer refuses them with a `TypeError`, as today for work-pool jobs. **TA10**. In mt builds `isShared()` in the sense of "somebody may write these bytes while I look" is true for every buffer whose view is not Confined: `sort` and every algorithm whose memory safety depends on reading a value twice works on a private copy, and a second pass is bounded by the size that was allocated, not by what the first pass measured. A4: `sort` copies out, sorts, and stores back inside the bounds of a freshly loaded storage."

### F12. R8 D1, D2: lower-tier precise cells defeat the block header and the rule "the owner sweeps"

An `IsoSubspace` hands out its first cells as precise allocations (`heap/IsoSubspace.h:47-71`; default 8, `runtime/JSCell.h:107`; Bun passes `T::numberOfLowerTierPreciseCells`, `bun:src/jsc/bindings/BunClientData.cpp:221`). For a class of which a program has a handful of instances, **all** of them are precise.

- The test of 11.3, `block = receiver & ~(blockSize - 1)`, reads memory that is no block header. 11.3 says that a precise allocation "has the field in its own header", but the three instructions do not distinguish the cases (bit `PreciseAllocation::halfAlignment` of the address, as `convertJSCalleeToVM` tests, `llint/LowLevelInterpreter.asm:1466-1472`).
- 08 section 8.12: "`sweepPreciseAllocations()`: inside the End phase, by the conductor", which is the collector thread. So `drop_in_place` of a payload in the cell runs on a thread without a Bun VM (`VirtualMachine::get()` is `unwrap_unchecked`), against 8.11 and 11.7.
- The free list of lower-tier cells (`m_lowerTierPreciseFreeList`) belongs to the server subspace: a cell freed by T's object is given to U.

**Proposal.** "Subspaces of `owner` classes are created with `numberOfLowerTierPreciseCells = 0` in mt builds. (16 KB per class and thread that makes an instance.) A static assertion in `subspaceForClass` and `subspaceForImpl`. `owner` classes have a fixed cell size below the precise cutoff, so the test needs no second case."

### F13. R7 S3, 8-23, 9-4, 10.1.1-14, 10.1.1-20: "no JavaScript is running" has no meaning for N threads

`entryScope` becomes per thread (07 section 7.2). Its consumers read it as a fact about the VM:

| Consumer | Where | What it frees |
| --- | --- | --- |
| `clearConcurrentRetainedDataIfPossible` | `heap/Heap.cpp:1406-1434` | every parked `StringImpl` |
| `RegExp::deleteCode`, cache pruning | R7 9-4 | bytecode and flat programs in use by a matcher |
| `VM::whenIdle`, `deleteAllCode`, `shrinkFootprintNow` | `runtime/VM.cpp:1175-1253` | linked and unlinked code |

1. T holds a `FlatString` of `s`. U atomizes `s`; the old impl goes to U's list (6.7: "becomes per thread").
2. U returns to its event loop. Its own `entryScope` is null. The incremental sweeper's timer fires on that loop and calls `clearConcurrentRetainedDataIfPossible` (`heap/IncrementalSweeper.cpp:77`), which clears the list.
3. T reads freed characters.

6.7 repairs the pruning at the end of a collection ("looks at all stacks"), not this second way out.

**Proposal.** "**H-idle**. `VM::isIdle()` is compiled out in mt builds. Each consumer becomes either (a) a safepoint operation that also scans all stacks for what it is about to free (parked impls: only the pruning of 08 section 8.12 remains, `clearConcurrentRetainedDataIfPossible` is empty), or (b) `main` and refused while `VM::m_threads` has more than one member (`deleteAllCode`, `shrinkFootprint`). `RegExp::deleteCode` hands the compiled forms to `freeAfterSafepoint`; a matcher holds them inside one window, or across its polls only together with the `RegExp` cell on its stack, which the pruning looks for. `whenIdle` runs its callback in a safepoint operation."

### F14. R7 S4: characters and width from two loads

`StringEmitter::sourceOfRope` (`aot/codegen/AOTStringEmitter.cpp:190-199`) gets the characters from a stub and then loads the impl of the flat base **again** for the width flag. The comment there knows that atomization can change the width. It does not know that it can happen between the two.

1. T: the stub returns 8-bit characters of the base's impl I1.
2. U: `swapToAtomString(base)`, to an equal atom I2 that is 16-bit.
3. T loads the flags from I2 and reads `length` 16-bit units from an 8-bit buffer: out of bounds.

06 section 6.7 states single-load discipline only for `resolveRopeWithFunction`, and N-c only for natives.

**Proposal.** In 6.7: "Whoever opens a string loads `m_fiber` (of the string, or of a substring's base) **once** and takes characters, length and width from that one `StringImpl*`. The stub `ropeCharacters` returns characters and flags as a pair. `viewOf*`, `JSString::view`, `unsafeView`, `GCOwnedDataScope` follow the same rule." A row for `StringEmitter` in 04 section 4.8.

### F15. R7 O5, O6: `PropertyTable`: updates in place, traversals, and the acquire that ARM64 needs

PT1 to PT5 cover insertion, removal and rehash. They do not cover:

(a) **Update of an entry in place.** `updateAttributeIfExists` (`runtime/PropertyTable.h:433-440`) stores the attributes; with N3 the offset changes too. In a wide entry these are two fields (`runtime/Structure.h:184-186`: `m_offset`, `m_attributes`). A reader that pairs the new attributes (`Accessor`) with the old offset casts a data value to `GetterSetter*`.

(b) **Traversal.** `forEachProperty` runs to `usedCount`, a word of the table, over a vector that PT1 lets change. A count from the table with an older, smaller vector is out of bounds.

(c) **ARM64.** For a dictionary the StructureID does not change on an add, so N1's acquire orders nothing. "Grow, store the value, then insert the entry (release)" needs a partner: the reader's load of the butterfly does not depend on the entry it found, so it may be satisfied first, with the old, smaller storage. The stub is x86-64 only, but on ARM64 the leaf operations do the same walk in C++.

**Proposal.** "**PT6**. Offset and attributes of an entry are one aligned 64-bit word (compact entries are already one), read by one load and replaced by one store. **PT7**. The number of used entries lies in the vector's header. A traversal takes `Structure::m_lock` (`forEachPropertyConcurrently` exists), or reads pointer, then count (acquire), and visits only entries below it. **PT8**. A lock-free reader loads the index slot with acquire order (rule F3 of 04) before it loads the butterfly."

### F16. R7 C3, NEW-3: `Map` and `Set`: readers that do not come through the index

MS2 argues: "A reader finds an entry only through the index, so it never sees a key without its value." Iterators, `forEach`, the clone (`tryCopy`, `aot/runtime/AOTCompactTable.cpp:364-383`, one `memcpy` of index and columns) and the collector find entries through `m_used` and the key column.

(a) **ARM64.** Stores: key, value, index slot (release), `m_used` (plain). A plain store behind a release store may become visible before it, and key and value are not ordered with each other. An iterator that sees the new `m_used` and the key, but not the value, returns the empty value under a proven type: C3 again.

(b) **Linearizability.** `has(k)` sees a `set` at the store of the index slot; `size` sees it at the store of `m_alive`; iteration at `m_used`. One thread that calls `m.has(k)` (true) and then `m.size` (0) while the `set` is between the two stores has seen what no order of whole operations explains. 02 section 2.10.5 promises "each operation sees the table whole: it is linearizable".

(c) A clone that copies the columns with `memcpy` beside an appender can copy a key whose value it copied a moment too early.

**Proposal.** "MS2: value, then key (release), then index slot (release), then `m_used` (release), then `m_alive`. A reader by position loads `m_used` (acquire), then the key (acquire), then the value. **MS10**. Operations that read the whole table (clone, `new Map(map)`, serialization, `groupBy` results) hold the owner's lock. **MS8'**. `size` is exact only with respect to operations that happen before it." And in 02: "`size` and iteration may lag behind `has` and `get` by operations that are still in progress."

### F17. R7 C11: `WeakMap` has a rule for its buffer, none for its entries

6.6 gives `WeakMapImpl` one pointer with the capacity inside, a lock for writers, deferred free, and says "lookups stay lock-free". The entry protocol is the source's: `add` stores the key, then the value (`runtime/WeakMapImpl.h:429-430`); `makeDeleted` marks the key and **clears the value** (`:147-150`).

- Reader matches the key, writer has not stored the value yet: the empty value escapes from `get`.
- Reader matches the key, `delete` clears the value, reader loads it: the same.

**Proposal.** "Add: value, then key (release). Delete: the key becomes the deleted marker (release) and the value is left, as in MS4, until the next rehash. The reader loads the key (acquire), then the value. The pruning of dead keys at the end of a collection runs with all threads parked, as stated."

### F18. R7 8-18, 10.8-5: I1 leaves shared words on the executable

"`prepareForExecution`, `newCodeBlockFor`, `installCode`: run per thread; there is nothing to race on." `installCode` also writes, on the shared executable, `m_jitCodeForCall` / `m_jitCodeForConstruct` (`RefPtr<JITCode>`), the two arity-check entries, and membership in `clearableCodeSet` (`runtime/ScriptExecutable.cpp:198-211`).

1. T and U call one function for the first time. Both assign the `RefPtr`: the old value is dereferenced twice.
2. Later the collector ages T's block and calls `clearCode` (`:77-82`): `m_jitCodeForCall = nullptr` for everybody. U still has its block. U's next call from C++ sees that the executable has no code and runs `prepareForExecution`, which hits `RELEASE_ASSERT(!executable->codeBlockFor(kind))` in `newCodeBlockFor` (`:315`).

Also per executable and shared: `m_rareData` (`ensureRareDataSlow`), `m_cachedPolyProtoStructureID`, `m_singleton`.

On the identity of per-thread `FunctionExecutable`s for nested functions: nothing was found through which a program can tell them apart. `toString`, `name`, `length` come from the unlinked half; stack traces and `StackVisitor` take the block from the frame; `arguments.callee` and `caller` are functions; template objects move to the unlinked block. What is keyed by the executable is invisible: poly-proto Structures in `StructureCache` (`PrototypeKey{.., executable, ..}`, `runtime/StructureCache.cpp:53`), allocation profiles. The cost is memory, threads times functions, and Structures that split per thread.

**Proposal.** "In mt builds an interpreted executable's entry words are constants: the LLInt prologues, written when the executable is made and never cleared. 'Has code' is a question to the thread's vector. `installCode` and `clearCode` write the vector's slot only. `clearableCodeSet` counts blocks per executable, under its lock. `m_index` is released by the executable's destructor, that is after the stop in which every vector dropped its dead blocks."

### F19. R7 8-1, 10.1.1-12: handlers that reach the VM without a CodeBlock

I1: wherever a handler goes `CodeBlock[cfr]`, `m_vm`, it loads `m_thread` instead; "no thread-local storage is read in handlers". Several paths do not go through a CodeBlock, because they have none:

| Path | How it finds the VM | Where |
| --- | --- | --- |
| `nativeCallTrampoline`, `internalFunctionCallTrampoline` | `storep 0, CodeBlock[cfr]`; callee, scope, `JSGlobalObject::m_vm`; writes `topCallFrame`, tests `m_exception` | `llint/LowLevelInterpreter64.asm:2806-2872` |
| `llint_op_catch`, the throw trampolines, uncaught exception | `getVMFromCallFrame`: callee cell, block header, `m_vm` | `llint/LowLevelInterpreter64.asm:380, 2745, 2781, 2789` |
| `getHostCallReturnValue` | `convertJSCalleeToVM` | `llint/LowLevelInterpreter64.asm:403-404` |

A native frame's caller may be compiled code, an adapter or C++, so the caller's CodeBlock is no way out either. These paths write `topCallFrame` and read the pending exception: the state that must be the thread's.

**Proposal.** "These paths load the `ThreadContext` from thread-local storage by the sequences of 07 section 7.3.4, as an offlineasm macro `loadThreadContext`. There are about ten sites. Each is at a call boundary, where one `%fs`-relative load or `mrs` is noise." And strike the sentence "no thread-local storage is read in handlers".

### F20. NEW-10, R8 B2, B15: what on the shared `Zig::GlobalObject` names one Bun VM

11.4 changes `clientData(vm)->bunVM` and leaves the rest to "table in W-11.5". One `VirtualMachine` per thread on a shared global works only if **no route from the global to a VM, a loop or a context remains**. These routes exist and are not listed:

| Member | Where | Why it breaks |
| --- | --- | --- |
| `void* m_bunVM`, `bunVM()` | `bun:src/jsc/bindings/ZigGlobalObject.h:111, 350`; 41 uses of `->bunVM()` in the bindings; `bunVM(Zig::GlobalObject*)` returns it | Natives on U get main's `VirtualMachine*`: hazard A2 |
| `m_scriptExecutionContext`, with an `m_bunVM` and an identifier of its own | `:188, 364`; 252 uses of `scriptExecutionContext()` | `postTaskTo(identifier, ..)` finds one context, hence one loop. Messages for a `MessagePort`, a `BroadcastChannel`, a `threadsafe` FFI callback or a `Worker` object that U owns are delivered on main's loop and touch U's object |
| `Ref<Bun::GlobalEventScope> globalEventScope` | `:365` | an `EventTarget` with a plain count and listener lists, used by `addEventListener` on the global from any thread |
| `WriteBarrier<JSNextTickQueue> m_nextTickQueue` | `:443` | `drainMicrotasks` tests and drains this one cell. The ring is per thread (11.5), the flag "has ticks" in field 0 is not: U's drain finds the flag that T set, drains its own empty ring and clears it. T's tick waits for another event: a lost wake-up |
| `m_constructors` (`DOMConstructors`), `m_domStructures[]` | `:166-171, 498, 714` | check, then create, with `ASSERT(!slot)`: two Structures or constructors for one class |
| `m_processObject` and the native members of `Bun::Process` | `:342, 613` | one emitter. `process.on("exit")`, `beforeExit`, `nextTick` set-up are state of one loop |
| `m_performance`, `m_subtleCrypto` | `:717, 752` | lazily made `RefPtr` and raw pointer |
| `mockModule`, `m_builtinInternalFunctions` | `:481, 497` | plain members written at run time |

**Proposal.** In 11.4: "In mt builds `Zig::GlobalObject` has no member `m_bunVM` and no member `m_scriptExecutionContext`. Both accessors read the thread-local Bun VM, which owns a `ScriptExecutionContext` with its own identifier, so that `postTaskTo` addresses a thread. `globalEventScope`, `m_nextTickQueue`, `m_performance` move to the Bun VM, with roots visited through the per-thread registry. `m_constructors` and `m_domStructures` follow Z2. `process` is one object; its listeners run on the main thread (09 section 9.11), and registration from another thread is `main`." W-11.5 becomes a compile-time list: a `static_assert` on `sizeof(Zig::GlobalObject)` in mt builds, so that a new member breaks the build until it is classified.

### F21. NEW-4, R7 10.2-1, 10.2-5, 10.3-1, R8 B8: claims per lazy word

**Deadlock.** Z2: a loser that finds `Claimed(other)` waits; `Claimed(itself)` is a cycle. Initializers enter other lazies, and the answer "null on recursion" exists because cycles exist. If X's initializer needs Y and Y's needs X, T begins at X and U at Y: each holds one claim and waits for the other, for ever, with heap access released, so that nothing even times out. The report says so (R7 10.2, "Rule") and proposes one recursive lock per realm.

**`LazyClassStructure`.** The initializer publishes the structure word in the middle (`setStructure`, `runtime/LazyClassStructure.cpp:55-70`), before `setConstructor` (`:72-87`) and before `didCreateClass` locks the class. Z2 speaks of the winner storing "the result". Here the result is visible to every reader of the word while `m_constructor` is null and the prototype is still mutated by `putDirectWithoutTransition`.

**Proposal.** "**Z2'**. All lazy words of one realm (`LazyProperty`, `LazyClassStructure`, link-time constants, Bun's 153) share one recursive lock, `JSGlobalObject::m_lazyLock`, with an owner thread. It is taken only on the slow path (`lazyTag` seen) and waited for with heap access released. It ranks with the modules lock in 08 section 8.15, since JavaScript may run under it. The word is stored (release) when the initializer has returned; `Initializer::set` writes to a field of the initializer, which recursive calls on the owner thread read. For `LazyClassStructure` the structure word is published last, after `m_constructor` and after `didCreateClass`."

### F22. NEW-5, R7 8-21: locks that are in no order

08 section 8.15 is "the" order, and a debug build checks ranks. Not in it: `VM::m_nativeSerialLock` (11.1), `JSGlobalObject::m_declarationLock`, the locks of `CodeCache`, `sourceProviderCacheMap`, `SourceProviderCache`, `CompactTDZEnvironmentMap`, the allocator of `m_index` (10.2, 10.3), the `ArrayBuffer`'s lock (TA7), a watchpoint set's lock (6.5), the inbox lock (09 section 9.7).

The first two are held over allocation.

1. T holds `m_nativeSerialLock` inside a `serial` native and allocates. The slow path sees a stop request and parks. (Nothing says that the lock is a `NoSafepointScope`, and it cannot be one: natives run long.)
2. U calls another `serial` native and blocks on the lock **with heap access**. H1 lists "a contended `Lock`", meaning the JavaScript class.
3. The stop waits for U. U waits for T. T waits for the end of the stop.

The same for `m_declarationLock`, which is held over "a long check phase, then an act phase" that creates Structures and variables.

**Proposal.** Add to 8.15, between `VM::m_threadsLock` and the modules lock: "`m_nativeSerialLock`, `m_declarationLock`, `m_lazyLock`: locks under which a thread may park. Whoever waits for one has released heap access." Below "table locks": "code cache and parser caches, `ArrayBuffer` lock, watchpoint set lock, inbox lock: leaf locks, `NoSafepointScope`." And a general rule: "**L6**. Each lock of an mt build is of one of two kinds: *parking* (the holder may reach a safepoint; waiters release heap access) or *leaf* (the holder is in a `NoSafepointScope`; waiters keep access). Its declaration names the kind."

### F23. R7 7.10-4, -9, -13, -14, -16, -21, 10.7.7-3, NEW-9: the `ArrayBuffer`'s own state has no lock of its own

TA1 to TA8 settle the view's triple. The buffer appears in TA7 only ("atomic or under its lock").

- **NEW-9, shrink and grow again.** `resize(small)` puts "the pages from `small` to `old`" on the deferred list (TA1: "shrunk"). `resize(old)` follows at once: by TA3 it commits (nothing to do) and stores the length. At the next stop the conductor runs the deferred decommit: pages inside the live length are unmapped or zeroed.
- **-14.** Two `resize`, or `resize` against `transfer`: the source says that the handle's lock "has nothing to guard here" (`runtime/ArrayBuffer.cpp:563-564`).
- **-9.** `pin` as an atomic count does not exclude "test the count, then detach".
- **-4.** A view under construction reads `data()` and the length, then registers in the list of incoming references. A detach between the two never reaches it: dangling for good.
- **-13, -21.** TA6 publishes the wrapper "under the view's object lock". `m_wrapper` is a member of the **buffer**. Two views of one buffer are two locks.

**Proposal.** "**TA7'**. `ArrayBuffer::m_lock` (leaf) covers: `resize`, `transfer`, detach, `pin` and `unpin` against them, the list of incoming references, `m_wrapper`, and the reading of `{data, length}` by a view under construction together with its registration. **TA1'**. A deferred release of a *range* of a live reservation is a closure that takes the lock and releases only what lies above the buffer's current length. Growing zeroes `[old length, new length)` explicitly."

### F24. R7 9-2, 9-3, 9-7, 9-12: RegExp

- **9-2.** "Scratch is per thread" does not reach `RegExp::m_ovector`, which is a member of the shared cell and is handed out by `ovectorSpan()` to `createRegExpMatchesArray`, `replaceUsingRegExpSearch`, `StringReplaceCache::set`, `subpatternIdForGroupName` (which uses a value from it as an index, `runtime/RegExp.h:188`) and to Bun (`URLPatternComponent.cpp`). Two threads and one literal are enough.
- **9-3.** The object lock serializes those who compile. Readers (`hasCodeFor`, `matchInlineOnce`, stub `matchOnFlatProgram`) take no lock, and `m_state` is stored before `m_regExpBytecode` (`runtime/RegExp.cpp:574-598`).
- **9-12.** After a match the stubs load `regExp` from the object again. `RegExp.prototype.compile` on another thread swaps it. `m_numSubpatterns` of the new cell then bounds a read of this thread's match output, whose size covers the old one: out of bounds, in spite of per-thread buffers.
- **9-7.** "`RegExpCache` has a lock": `m_lock` guards the weak map. `addToStrongCache` and the ring of parsed patterns (`runtime/RegExpCache.cpp:86-92, 135-138`) run without it; the index wraps by an equality test, so two increments step over the end.

**Proposal.** In 6.9: "`RegExp::m_ovector` does not exist in mt builds; `ovectorSpan(thread)` returns the thread's buffer, grown to `offsetVectorSize()` before the match. `m_state` is stored last (release), and a reader loads it first (acquire). One operation on a `RegExpObject` loads `m_regExpAndFlags` once and uses that cell for matching and for building the result. All members of `RegExpCache` are under `m_lock`."

### F25. NEW-6: 3.3 needs in `bun-aot` what 3.4 compiles out of it

3.3, step 2: `bun-aot` loads the application, computes `imported_modules.contains(&RuntimeModule::BunThreads)` and hands the build over. 3.4, item 1: enum member, parse and alias table are "all under `#[cfg(bun_threads)]`. Without it the specifier is unknown, which gives the wanted errors". In `bun-aot` the load stage then ends in `BuiltinModuleNotFound`, and `RuntimeModule::BunThreads` does not exist. The diagnostic `ThreadsNeedCompiler`, raised by plain `bun build`, needs the name in the first flavour too.

Smaller, in the same sequence: step 2 tests only `imports_threads`, so `bun compile --threads` in `bun-aot` is not handed over. And a threaded build started from `bun` downloads `bun-aot` (136.6 MB) only to learn that it needs `bun-aot-mt`.

**Proposal.** "The **name** `bun:threads` is known to all three flavours (`HardcodedModule::BunThreads`, `RuntimeModule::BunThreads`, unconditional). What is under `#[cfg(bun_threads)]` is the implementation. Without it, resolving the name at run time or in `bun build` gives `ThreadsNeedCompiler`, and in `bun-aot`'s load stage the hand-over. Step 2: `imports_threads || options.threads`. `bun` scans the entry points' import records for the specifier before it chooses which flavour to fetch; a miss costs the second hand-over and nothing else."

### F26. NEW-7: the third flavour is also a Bun with a JIT

"`bun-aot` is at once (a) the compiler, (b) a complete JIT Bun, and (c) the template" (R8 7.1). `ENABLE(AOT)` is on "wherever the FTL is on", and the compiler needs B3 and Air, so Baseline, DFG and FTL are in `bun-aot-mt`, compiled against a 16-byte `IndexingHeader`, an out-of-line offset shifted by one word and a storage word that they neither initialize when they allocate butterflies in line nor test when they store. `JSCInitialize` sets `useJIT() = true`, and what separates the roles at run time is `useAOT() = runsCompiledCode` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:354, 377`).

Ways into role (b) in an mt binary: the user runs `bun-aot-mt run x.ts`; macros, plugins or `bun test --aot` preloads that are evaluated in the first process; `--aot-checked` for a project in which one file imports `bun:threads` and others are only checked. P1 says that one binary has one object model. The JIT tiers are a second one.

**Proposal.** "**P3**. An engine with `ENABLE(JS_THREADS)` never generates code at run time: `Options::useJIT()`, `useDFGJIT`, `useFTLJIT`, `useBBQJIT`, `useOMGJIT` are forced off in `Options::finalize`, and `VM::canUseJIT()` is `false` at compile time. A command that would run JavaScript from source in `bun-aot-mt` either hands over to `bun-aot` (3.3, step 3 covers `run`, `test` and every other command, not only `compile`), or runs it in the interpreter with the `JS_THREADS` handlers of 10.4, which are therefore needed from M0 and not from M6."

### F27. R8 E4: the template is not always the running binary

(a) 3.1: `imageStamp()` and the layout hash "differ between the flavours by themselves, because layouts differ (`IndexingHeader`, `Instance`, `VM`)". `runtimeLayoutHash()` (`aot/runtime/AOTInstance.h:352-374`) hashes sizes and offsets of the `Instance` and of image records. Neither `sizeof(IndexingHeader)` nor `offsetInButterfly` nor anything of `VM` is in it. The flavours differ only as long as the mt `Instance` happens to have another size.

(b) 07 section 7.3.4: the TLS offset is computed by "the compiler, running in the same binary". `check_template` takes a **path** (`bun:src/runtime/cli/compile_command/build.rs:252-275`) and compares revision, module stamp and descriptions. The template may be another link of the same revision: `image_form_of` exists because a hardened, position-independent twin is a legal template. Its TLS layout is its own. The mismatch is "compared at load": the build succeeds and the executable refuses to start.

**Proposal.** "The engine's `RuntimeDescription` gets an explicit byte `flavour` (0 `aot`, 1 `aot-mt`) and the runtime facts of 07 section 7.3.4 (TLS offset or key, `_tls_index`). `find_difference` names a wrong flavour in words. The compiler takes the facts **from the template's description**, not from its own process. `runtimeLayoutHash()` also hashes `sizeof(IndexingHeader)`, `offsetInButterfly(firstOutOfLineOffset)` and `ENABLE(JS_THREADS)`."

### F28. R7 A10: a nuked StructureID in C++

"A mutator that loads a nuked ID goes to the slow path and takes the lock." That describes stubs, which compare IDs and miss. In C++ the load is `cell->structure()`, at thousands of sites, and it decodes whatever is there. The nuke is now unconditional (6.4) and happens on objects that other threads read.

**Proposal.** "In mt builds `JSCell::structureID()` and `structure()` return the decontaminated ID (one `and`). By the table of N1 an old StructureID with either storage is a legal pair, so a reader needs no slow path. Only a writer that plans a transition from the ID compares the raw word under the lock."

### F29. R7 10.7.1-1, -2: materializing `error.stack` can run JavaScript

D1's row: "one thread claims, others wait on the object lock". Then the claimant holds the lock during the materialization. In Bun the hook is `computeErrorInfoWrapperToJSValue` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:695-696`), which calls the user's `Error.prepareStackTrace` (`bun:src/jsc/bindings/FormatStackTraceForJS.cpp:155`). L1 and D1 ("never run JavaScript inside") forbid that.

**Proposal.** "`ErrorInstance` follows Z2 with a claim byte: `NotMaterialized`, `Claimed(thread)`, `Done`. The claimant takes the trace out under the object lock, releases the lock, runs the hook, puts the properties (each an add of 6.4), and stores `Done` (release). A thread that finds `Claimed(other)` waits with heap access released. `Claimed(itself)`, a `prepareStackTrace` that reads `stack` of the same error, gets `undefined`, as today."

### F30. R7 10.3-12, -13, -20, -21, 10.7.2-2 to -6, 10.7.6-9, 10.7.7-1, 10.8-7: state for which D1 is no answer

| ID | Why the default rule does not do | Proposal |
| --- | --- | --- |
| 10.3-20, -21 | `variableAt` goes through the spine of a `SegmentedVector`, which `addVariables` reallocates (`runtime/JSSegmentedVariableObject.h:60-70`). Readers hold the `SymbolTable`'s lock, not the cell's. Reached from `eval`, scripts of `node:vm`, C++ `symbolTableGet` | The spine is one block with its length inside, replaced by a release store and freed after a safepoint (as PT1, PT4). Variables first, then the symbol table entry |
| 10.3-12, -13 | `tryInstallSpeciesWatchpoint` begins with `RELEASE_ASSERT`s and may flatten a prototype. `tryInstallPropertyDescriptorFastPathWatchpoint` is called from `aot/runtime/AOTDefinitionOperations.cpp:265` on any thread and appends to plain vectors | Installed eagerly when the realm is made (built-ins are locked, so they cannot fire in the main realm); in `node:vm` realms under `m_lazyLock` (F21) |
| 10.7.2-6 | `setCompiledAllocation` writes 8, 16 and 32 bits; `createThis` pairs constructor number and Structure on a hot path where a lock is too dear. A torn pair is an object of the wrong layout | Pack the 24-bit constructor number and the StructureID into one 64-bit word: one store, one load |
| 10.7.2-5 | `ObjectAllocationProfile::m_allocator` (`bytecode/ObjectAllocationProfile.h:75`) caches a `LocalAllocator` of the filling thread in a shared cell, and dangles when that thread ends. No interpreter path uses it (`create_this` is a slow path, `llint/LowLevelInterpreter.asm:2317`, against the row "inline allocation" of 10.4) | The member is compiled out in mt builds. Structure and prototype are one immutable record, published by compare-and-swap |
| 10.7.2-2, -3 | The flags of `FunctionRareData` are bit-fields, and are set **before** the `putDirect` | Atomic `or`, after the put, as 6.10 says for arguments objects |
| 10.7.7-1 | "The trap cache is per thread": the cache is an array of members of the `ProxyObject` cell. A per-thread copy would be a side table keyed by proxy | Either off in mt builds, or one 64-bit word per trap: `(handler StructureID, offset)` |
| 10.8-7 | `m_unlinkedCodeBlockForCall` shares a union with `RefPtr<Decoder>` and is told apart by the bit-field `m_isCached`. Z1 on the pointer does not cover the change of the union's type | Decode under the executable's object lock; `m_isCached` is cleared (release) after the union holds code block pointers; readers test it first (acquire) |
| 10.7.6-9 | `Box<>` swap in `StructureRareData` | Set once by compare-and-swap of the raw pointer; the loser drops its box |

### F31. R7 10.1.5-11, 10.3-6, 10.7.8-5, 10.7.9-1, -2, 10.1.1-6, 10.1.7-2, R8 B18: Intl state behind natives that are `any`; a change of time zone

11.6 makes `Intl.*` `serial`. But `String.prototype.localeCompare`, `toLocaleUpperCase`, `Number.prototype.toLocaleString`, `Date.prototype.toLocale*String` are in groups declared `any`, and they use `m_cachedLocaleCompareCollator` with its locale and epoch (three words, key before value, `runtime/JSGlobalObject.cpp:3464-3484`), the realm's default collator and formats, and `IntlCache`. An `any` native takes no serial lock, so it races with `serial` ones and with itself: a `String` slot swapped under a reader, and one `UCollator` or `UDateFormat` used by two threads.

A change of time zone (`process.env.TZ = ..`) sets a bit in `m_entryScopeServicesRawBits`. The next thread that *enters* resets "the" `DateCache` and walks `dateInstanceSpace`. With caches per thread the request must reach each thread, and the walk must be a safepoint operation run once.

**Proposal.** "`IntlCache`, the two cached formatter triples and the five default formatters are per thread (07 section 7.4): an ICU object is cloned or opened per thread. A request that concerns every thread's caches (time zone, default locale) bumps `VM::m_environmentEpoch`; a thread compares it where it compares the cache epoch (E2) and resets its own `DateCache` and `IntlCache`. The walk that marks `DateInstance`s stale is one safepoint operation, run by the thread that made the change."

### F32. R7 10.6-1, 10.3-11, 10.7.6-3, -6, -7, -8: watchpoint sets that C++ consults

6.5 sends the reader to "07, section 7.5" for "what C++ consults them for". 7.5 is about the run time's own caches and words. The roughly 80 tests of `isStillValid()` in C++ fast paths (R7 10.6) are in no document. Most are harmless: in the main realm the watched built-ins are locked and the sets cannot fire, and a late answer skips a hook once. Two classes are not. (1) Sets that guard a **shape**: `arrayBufferDetach`, `havingABadTime`, sane chain. (2) Records that own installed watchpoints (`m_cachedPropertyNameEnumeratorWatchpoints`, the special property cache's `Bag`): a record that is replaced by compare-and-swap cannot be freed while a `fireAll` on another thread walks the list that it is linked into. `m_immutablePropertyNames` lives in such a record: if the record is replaced, the lock on a built-in is lost (R7 O11).

**Proposal.** "**W1**. A C++ fast path may rest on a watchpoint set only if a late firing leaves it memory safe by 06; else it makes the test of 06 itself (storage kind, `LoadView`). W-6.x lists the 80 sites with one of: cannot fire (locked), harmless, replaced. **W2**. A watchpoint is unlinked under its set's lock; a record that owns watchpoints is freed by `freeAfterSafepoint` after they are unlinked. **W3**. `m_immutablePropertyNames` is a member of `StructureRareData` itself, set once, before the Structure is installed in any cell."

### F33. Members that no document names

The general rules decide these, but the design's own argument (01 section 1.4 A) is that a missed site is a memory-safety bug. They should be added to the lists they belong to.

| Add to | Members |
| --- | --- |
| 07 section 7.2, scratch | `stringSplitIndice`, `m_stringSearcherTables` (M: R7 9-9), `m_syncResumeCallCache` |
| 07 section 7.4, last row | `lastCachedString`, `immutableButterflyToStringCache`, `m_random`, `m_heapRandom`, `m_integrityRandom` |
| 07 section 7.7, a lock | `m_executablesOfNamelessHostFunctions` ("`Promise.prototype.finally` asks at every call"), `m_impurePropertyWatchpointSets`, `JSGlobalObject::m_rareData`, `m_signposts`, `m_executableForCachedFunctionExecutableForFunctionConstructor`, `m_persistentBytecodePayloads` and the cached two and three character atoms, `SourceProvider::m_id` and its directive strings |
| "set before the second thread starts", with an assertion in the setter | `m_onComputeErrorInfo*`, `m_onAppendStackTrace`, `m_computeLineColumnWithSourcemap`, `m_crossTaskToken`; `JSGlobalObject::m_name`, `m_evalEnabled` and its message, `m_consoleClient`, `m_runtimeFlags` |
| atomics | `m_failNextNewCodeBlock`, `m_entryCountFromOutside`, `m_unlinkedCodeKeepers`, `m_isDeletingAllCodeToGenerateItAgain`, `m_activeHeapAnalyzer` |
| 06 section 6.5 | `tryMarkPristine` at run time (`runtime/Structure.cpp:745-797`): it walks the transition tree, so under `m_lock` per node by the rule for O8, and sets `m_blob`, `m_outOfLineTypeFlags` and the bit field of each Structure. The blob is copied into new cells by other threads: one 64-bit store |
| 11 section 11.4, `threadDidStart` | `Output::configure_named_thread` (R8 A7) |
| 04 section 4.14 | `captureBytes` must treat every buffer that is not Confined as shared (R7 7.10-26) |
| 06 section 6.2 | frame arrays carry the storage word, state `Confined` (`sizeOfFrameArray`, `aot/stubs/AOTStubsArrays.h:67-69`): the six operations that may be given one run A2 on it |

### F34. NEW-8: the spin on `Retired`

`do { pause; b' = load64(a+8) } while (b' == b)`. The argument "the owner cannot park" excludes a deadlock with a stop. No cycle was found either: between the retiring compare-and-swap and the publication the owner takes no lock, allocates nothing and calls nothing, in the stub, in 6.3's C++ form and in 6.4 (where it already holds the object lock, which the spinner does not need). What is left is time. The OS may take the owner off its core in the middle of copying a large array. Then every writer of that array burns a core, and on a machine or container with one core each burns its whole quantum before the owner runs again. A spinner may hold an object lock (6.4, step 2), so waiters pile up behind it with heap access, and a stop that is requested meanwhile takes as long.

**Proposal.** "After 64 turns the spinner calls `Thread::yield()`; after 1,024 it parks in `ParkingLot` on the address `a+8`. An owner that publishes after a copy of more than 4 KB calls `unparkAll(a+8)`. Copies above 1 MB are made **before** the retirement into storage that is not yet published, and only the slots that this thread has written since are copied again after it. (No other thread had the right to write.)"

### F35. R8 B20, D5: exit and exhaustion on a spawned thread

**B20.** 09 section 9.11: `process.exit(code)` on another thread "posts to the main thread, and blocks". If the main thread is in `thread.join()`, `lock.hold` or `Atomics.wait`, which 02 allows, it never takes the task: T waits for main, main waits for T.

**D5.** `MarkedBlockCage::setLastReserveHandler([](VM&) { WebWorker__endForExhaustedHeap(Bun__getVM()); })` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:347`) runs on whichever thread exhausts the cage. On a spawned thread `Bun__getVM()` is that thread's VM, whose `worker` is null.

**Proposal.** "A request to exit sets the termination trap of every thread (09 section 9.10), which wakes blocked ones. The main thread, woken out of `join`, runs the `exit` handlers instead of going on to wait. The last-reserve handler receives the engine `VM&` and ends the VM's **main** Bun VM through its `VmHandle`."

### F36. Smaller contradictions with the sources

| Text | Source | Consequence |
| --- | --- | --- |
| 06 section 6.8: "`Buffer` in Node's style allocates from a pool with an `ArrayBuffer` already" | Bun has no pool: `allocBufferUnsafe` is `JSUint8Array::createUninitialized` (`bun:src/jsc/bindings/JSBuffer.cpp:283-289, 545-550`); `poolSize` is only a property (`:3788`) | TA5 costs a `malloc`, a `free` and a finalizer for **every** small `Buffer` that is not proven Confined. To be measured at M3, or a per-thread slab for small oversize vectors |
| 06 section 6.5: "`StructureCache`: has a lock already" | `createEmptyStructure` reads `m_structures.get(key)` without it: "only the main thread can get here" (`runtime/StructureCache.cpp:45-56`) | Rule: the read takes the lock too (R7 10.3-7) |
| 10 section 10.4: `create_this`: "inline allocation" | `slowPathOp(create_this)` (`llint/LowLevelInterpreter.asm:2317`) | Nothing to rewrite there; see F30 for the allocator word |
| 01 section 1.5, 06 section 6.2: "8 bytes per butterfly" | A butterfly without indexed storage has no header at all today (`Butterfly::totalSize(.., hasIndexingHeader, ..)`) | 16 bytes more for each object that has only out-of-line properties |
| 06 section 6.10: `DateInstance`: "recomputed per call or published as one pointer" | Compiled code reads and writes the three words in line (`aot/builtins/AOTDateBuiltinEmitter.cpp:16-25, 58-68`) | Needs a row in 04, or: accept the report's V (an old `getHours()` until the next `setTime`) and say so in 02 |
| 11 section 11.3: `cmp16 [block + offsetOfOwnerThread], thread.id ; jne refuse` on the host-call path | The receiver there is any value | The test needs "is a cell", and a value of the field in blocks that are not affine which refuses nobody; else a wrong receiver gets `ConcurrentAccessError` where the native's own `TypeError` is due |
| 11 section 11.3 (R8 C3): `directEntry` thunks "have no test" | They "take the operands untested" on the strength of the caller's proofs | Rule: a direct form may rest only on proofs that 04 section 4.5 lists as for ever (class, `TestedType`, `StorageKind` of a loaded `Storage`). Stretch proofs about a subject that may be shared are tested again inside the native |

### F37. R7 S1: rope resolution, two assumptions that are not stated

"Fibers 1 and 2 are never cleared, so they can be read afterwards." They are never cleared, but once a winner has published they are no longer **marked**: the visitor of a resolved string does not visit them. The loser may read them only inside the window in which it loaded the rope word. That holds today, because `reportExtraMemoryAllocated`, a safepoint by 08 section 8.4, comes after `convertToNonRope` (`runtime/JSString.cpp:352-358`). Nothing says that this order carries weight.

The walk also descends into fibers that are ropes themselves (`resolveRopeInternalNoSubstring`, `resolveRopeSlowCase`). Each of them can be resolved by a third thread during the walk, so the single-load rule holds for every node and not only for the root.

**Proposal.** Add to the row: "From the load of the rope word to the end of the copy there is no safepoint. Each fiber visited is classified from one load of *its* `m_fiber`. `reportExtraMemoryAllocated` is called by the winner only, after the compare-and-swap."

## B.5 Second round: each rewritten protocol attacked by somebody who had not written it

What was read: `06-ordinary-objects.md`, sections 6.2 to 6.5, against the sources, against 02 (what is promised), 04 section 4.6.3 (what the compiler can emit), 05 (locks), 07 section 7.5 (caches) and 08 sections 8.6, 8.10, 8.15.
Paths are relative to `Source/JavaScriptCore/`.

Each finding: what goes wrong, step by step; why the text does not close it; the text that would.
Severity words: **wrong result without a race** (a program without data races sees something that 02 excludes), **promise of 02**, **cannot be built as written**, **contradiction**.

The core of 6.2 and 6.3 held: no interleaving of reader, owner, second writer, grower, converter, adder of a property and marker was found that leaves the allocation, pairs a kind with foreign contents, or loses a store. What was tried is listed at the end.
All findings but the last group are about paths of the run time that replace or reinterpret storage and have **no sequence in 6.3 or 6.4 at all**.

---

### 1. An array that is frozen in place is writable for a moment, whenever somebody tries in vain to change it

**Wrong result without a race (M9, M1).**

For compiled code and stubs the only thing that says "frozen" is the state `ReadOnly` of the storage: "Compiled code and stubs do not read [the indexing byte and the Structure] for arrays that may be shared" (6.3.6).
`JSObject::moveFrozenElementsToSparseMap` (`runtime/JSObjectFrozenElements.cpp:17-47`) undoes that state in four separate, published steps:

```
m1  setStructure(frozenStructure->previousID())      // copy-on-write, not frozen
m2  convertFromCopyOnWrite()                         // 6.3.8: new storage, Owned(me), writable Structure
m3  convert..ToArrayStorage()                        // 6.3.6
m4  objectConstructorFreeze()                        // frozen again, now with a sparse map
```

It is called where a frozen array is the receiver of something that will be **refused** or that does not touch its elements:
`JSArray::defineOwnProperty` (`runtime/JSArray.cpp:228-229`), `JSArray::pop` (`:1491-1492`), `JSObject::putInlineSlow` (`runtime/JSObject.cpp:621-622`), `setPrototypeWithCycleCheck` (`:2015-2016`), `didBecomePrototype` (`runtime/StructureCreateInlines.h:69-72`), private names (`aot/runtime/AOTPrivateNameOperations.cpp:179-180`).

```
main    const f = Object.freeze([1, 2, 3]);  start T1 and T2 with f          // freeze happens before everything below
T1      Object.create(f)                     // or f.x = 1, or f.pop() in a try: none of them writes to f
T1        m1, m2: storage of f is Owned(T1), kind Int32
T2      f[0] = 9                             // sloppy mode: must do nothing
T2        wr1 b = new storage;  wr2 fails;  wr3 fails
T2        wr4 AcquireStorage: Owned(other) -> CAS to Shared -> writable
T2        store64(b, 9)
T1        m3: cv6 fails (Shared), 6.3.7: the closure copies 9 into the ArrayStorage
T1        m4
all     f[0] === 9, for ever, in a frozen array
```

Neither thread writes a location of `f` in the sense of 02, section 2.10.1, so the program has no race. M9 says that the store fails.

Why the text does not close it: 6.3.8 speaks of `tryFreezeArrayInPlace` only. Each of m1 to m4 obeys 6.3 on its own.

**Proposed text (6.3.8).**
"`moveFrozenElementsToSparseMap` is **one** replacement. Prepared aside, at a safepoint: storage of kind `ArrayStorage` whose sparse map holds the elements with the attributes of a frozen array, and the Structure that m1 to m4 end in (found by running the four transitions on Structures, not on the array). Then, as cw2 to cw4: lock; require the StructureID and the pointer that were read; nuke; store-release of the pointer; the final StructureID; unlock; owner barrier. The read-only storage is not retired. **The array never has a Structure that is not frozen, and never storage in a state other than `ReadOnly` whose kind a compiled store can pass.**"

---

### 2. `setPrototypeOf` on an object without a layout has no rule

**Promise of 02 (M8, section 2.10.5: "exactly one succeeds", never a cycle). With a cycle every later lookup that misses spins for ever.**

```
T1  Object.setPrototypeOf(a, b)                         T2  Object.setPrototypeOf(b, a)          // a, b plain JSFinalObjects
T1  walk from b: b -> Object.prototype -> null. No a
                                                        T2  walk from a: a -> Object.prototype -> null. No b
T1  setPrototypeDirect(a, b)
                                                        T2  setPrototypeDirect(b, a)
                                                        a -> b -> a
```

(`runtime/JSObject.cpp:2050-2062` is the walk, `:1956-2001` the change.)

Why the text does not close it: 6.4 and 6.5 have sequences for adding, deleting, redefining, flattening, reifying and restricting, none for the prototype. The process-wide lock was stated in 05, section 5.7, under "Compiled objects", for `AOT::Object::setPrototype`. A chain passes through both sorts of object, so a lock that one sort takes protects neither.

Two more things hang on the same function:

- **What follows the change cannot run under a leaf lock.** `setPrototypeDirect` goes on with `realm()->haveABadTime(vm, this)` (a heap walk: a safepoint operation) or `switchToSlowPutArrayStorage` (a conversion that may need 6.3.7). `VM::prototypeChainLock` and the object lock are leaf locks (08, H15): their holder cannot park.
- `didBecomePrototype` on an array that is frozen in place runs finding 1's function, which allocates and converts, inside the lock.

**Done in 05, section 5.7:** the lock is for every object; `JSObject::setPrototypeWithCycleCheck` and `setPrototypeDirect` take it around walk, change and `didBecomePrototype`; a link in a poly-proto slot is stored under it; the two followers run after both locks are released; a frozen array moves its elements before the lock is taken.

**Proposed text (6.4, new paragraph "Changing the prototype").**
"As 05, section 5.7, for every object: prototype-chain lock, cycle walk, `didBecomePrototype`, object lock, the new StructureID (or the poly-proto slot, release), unlock both; then the invalidations of 07, section 7.5; then, holding nothing, `haveABadTime` or `switchToSlowPutArrayStorage` if the new chain intercepts indexed accesses."

---

### 3. `a.length = n` can replace the storage by a shorter one, and 6.3 has no sequence for that

**Wrong result without a race (02, section 2.10.5: "`a[i] = x` (existing index) against `a.length = n` with `n > i`: not a race: both take effect. Never: a lost store").**

`JSArray::setLength` (`runtime/JSArray.cpp:1454-1459`): if more than 64 elements and more than half of the array are dropped, `reallocateAndShrinkButterfly` (`runtime/JSObject.cpp:4195-4216`) allocates a shorter butterfly, copies, and publishes it. No state test, no retirement.

```
T1 (owner)  a.length = 10                 // a has 1000 elements
T1            new = resizeArray(..): elements 0..9 copied
T2          a[0] = x                      // wr4: Owned(T1) -> Shared, store64(old, x)
T1            store(a+8, new)
all         a[0] is the old value. x is lost
```

Why the text does not close it: the table of 6.2 says that the owner replaces "by retiring it", so the function as it stands is forbidden. But the only sequence that retires without a change of the Structure is 6.3.5, and gr2 requires `vectorLength(b) < what is wanted`. 6.3.3 shows the inline form only. 6.4 (N1: "storage only grows") and 07, section 7.5 ("storage never shrinks under a reader (SW2)") read as if no shorter successor existed. It also sets `vectorLength` to the exact length, against SW2 ("all that the size class holds").

**Proposed text (6.3.5, last bullet).**
"**Shrinking** (`reallocateAndShrinkButterfly`) is the same sequence with the second requirement of gr2 turned round (`vectorLength(new) < vectorLength(b)`, `publicLength` wanted `<= vectorLength(new)`); gr3 copies the elements below the new length. In state `Shared` it is not done: the length is lowered in place (6.3.3), and the room is given back at the next replacement. The out-of-line capacity is never made smaller, so N1's table holds."

---

### 4. The length of a restricted array can be read by every thread

**Promise of 02 (section 2.7: from other threads "get .. by name and by index" throws; "an object that cannot be enforced is never accepted").**

6.4: "a restricted object is one on which **every fast path misses**": a named access on the StructureID, an indexed read at rd2, an indexed store at wr2 to wr4.
6.3.1: "`a.length`: rd1, `load32(b-8)`." There is no compare in it that could miss, and none is needed for memory safety, since the length lies at `b-8` in every kind.

```
T1  Thread.restrict(a)
T2  a.length             // LoadStorage, GetStorageLength: the number. No ConcurrentAccessError
T2  for (let i = 0; i < a.length; i++) ..    // the loop header runs; the first a[i] throws
```

**Done in 04, section 4.6.3:** `GetStorageLength` relies on `StorageKind` for an array whose sites reach the operand use `Restricts`.

**Proposed text (6.3.1).** "`a.length`: rd1, `load32(b-8)`; for a site that reaches `Restricts`, rd2 between them." And in 6.4, in the sentence on what misses: "a read of `length` at rd2".

---

### 5. `Thread.restrict` never ends on a literal, on a frozen array, on `Confined` storage

**Cannot be built as written (endless retry).**

6.4: "`Thread.restrict(o)` is a safepoint operation, prepared and validated as in 6.3.7 (storage that is `Owned` is made `Shared` first)". cl1 validates `state(w) == Shared` and returns false otherwise; rp3 starts again.

- `Thread.restrict([1, 2, 3])`: the storage of a literal is `ReadOnly`. SW5: `ReadOnly` is final. The closure returns false every time.
- The same for an array that is frozen in place.
- The same for `Confined` storage (also final). An array of a Confined site can be passed to `Thread.restrict`; 02 accepts every array.
- `AcquireStorage`, which 6.3.7 names as the way to `Shared`, answers `writable(b)` for `Owned(me)` **without changing the state**. The usual caller of `Thread.restrict` is the owner.

One more consequence of "as in 6.3.7": rp1 gives the copy the word `Shared | Restricted`. From then on the only thread that may touch the array stops the world for each growth.

**Proposed text (6.4, point 2).**
"Its storage, if any, is replaced by a copy whose kind byte is `Restricted` and whose state is `Owned(owner)`; the owner's ID is captured by value (08, section 8.6, point 5). The closure accepts every state of the source but `Retired`, and validates kind, `vectorLength` and StructureID as cl1 does. It retires a source that is `Owned` or `Shared`. It leaves a `ReadOnly` source as it is (other arrays point at it). A frozen array keeps its frozen Structure's attributes in the restricted one. No state is changed beforehand: at the stop nobody is between a test and a store."
SW5 gets: "`Owned(t) -> Retired` also in a safepoint operation."

---

### 6. `flattenDictionaryStructure` "with today's steps" is not a closure of 08, section 8.6

**Contradiction between 06 and 08; cannot be built as written.**

6.4: flattening "runs as a safepoint operation, with today's steps against the marker". 08, section 8.6, point 8: "A closure allocates nothing in the heap. .. The one exception is `haveABadTime`."
Today's steps allocate: when the out-of-line capacity gets smaller, `shiftButterflyAfterFlattening` (`runtime/JSObject.cpp:4464-4494`) makes a new butterfly by `Butterfly::createUninitialized` and copies header and elements with `memcpy`. That is a replacement of storage, and 6.4 says nothing of the states: the copy takes the old word along, whatever it is, and the old storage is not retired.
It also opens a `DeferGC` scope (`runtime/Structure.cpp:1802`), whose exit is one of the places that 08's point 7 lists as parking the conductor.
Lastly `renumberPropertyOffsets` forgets the deleted offsets, while the table stays registered in the heap's list of tables with a quarantine (N2): the end of the next collection would move offsets into the free list that are in use again.

**Proposed text (6.4, Flattening).**
"Prepared at a safepoint: the number of properties, hence the capacity afterwards; if that is smaller, new storage for it (word: `Shared`, or `Owned(requester)` if the source is). The closure validates StructureID, number of properties, state, kind and `vectorLength`; renumbers; moves the values; publishes the new storage as cl3 to cl5; empties both lists of offsets and takes the table off the heap's list. It allocates nothing and opens no `DeferGC` scope."

---

### 7. What 04, section 4.6.3, could not emit

**Cannot be built as written; closed in 04, one request left for 06.**

| Sequence of 06 | What the IR lacked | Now in 04, section 4.6.3 |
| --- | --- | --- |
| wr2, wr3: two compares with **two successors**, columns Owned and Shared of 6.3.3 | `MayWriteStorage` was one boolean that meant "`Owned(me)` or `Shared`". Behind it the plan could not know whether a plain store of the length is allowed | `MayWriteStorage [Owned]`, on its failing edge `MayWriteStorage [Shared]`, then `AcquireStorage`. New proof `OwnedStorage` (a stretch; another thread can undo it, which is harmless for the one thing it licenses) |
| `store32(b-8, ..)`, `raise(n)`, `cas32(b-8, len, len-1)` | no operation wrote the length of a `Storage` at all | `SetStorageLength [raises]`, `RaiseStorageLength`, terminator `TryLowerStorageLength` |
| `AcquireStorage`: "`kind(w)` differs -> miss" | the terminator had no kind to compare with | payload `arrayStorage`; the edge `writable` proves `StorageKind` of the new value |
| "`store64(b+8i, v)`; barrier" | `PutElement` had `storage` but not the array. The barrier is the owner's, and `value/retains` had nothing to be retained by | operand `array`; `retains(array)`; `runsOwnerBarrier`; the verifier ties `storage` to a `LoadStorage` or `AcquireStorage` of that value |

**Request to 06, section 6.3.2:** the stub is `acquireStorageForWriting(a, b, kind)`, with a value "any" for the stores of 6.4, which mask the kind off.

**The stub `growVector` reads what 6.3.6 says stubs do not read.** It takes the hole from byte 4 of the cell (`loadHoleOf`, `aot/stubs/AOTStubHelpersStorage.cpp:190-200`) and learns from the StructureID that there are no out-of-line properties (`:153-161`). The second is gr2's third requirement and is arbitrated by gr4 (whoever adds capacity to storage that it does not own makes it `Shared` first). The first is not: between gr2 and the fill nothing ties byte 4 to `w`. It cannot go wrong, because only the owner converts `Owned` storage, but 6.3.5 should say: "the hole is that of `kind(w)`".

---

### 8. `haveABadTime` does happen in the main realm

**Contradiction with the source; the frequency in 08 is wrong.**

6.3.11: "`haveABadTime` can happen only in realms of `node:vm` and `ShadowRealm`". 08, section 8.6: "once per realm".
That holds for `JSGlobalObject::haveABadTime(VM&)`. The variant with an interceptor (`runtime/JSGlobalObject.cpp:3139-3174`) is made for the main realm of a compiled process, whose built-in prototypes are locked: it walks the heap (`HeapIterationScope`), collects the objects that have the interceptor in their chain, and calls `switchToSlowPutArrayStorage` on each. It runs each time an object that is a prototype gets an indexed accessor, or a chain that intercepts (`runtime/JSObject.cpp:1065, 1988-1991`): `Object.defineProperty(MyList.prototype, 0, { set })`, `Object.setPrototypeOf(Base.prototype, new Proxy(..))`.

It is a heap walk followed by conversions of other threads' arrays, so it must be a safepoint operation like its namesake, and it allocates in the closure, so 08's point 8 has to name it.

**Proposed text (6.3.11).** "`haveABadTime(vm)` can happen only in realms of `node:vm` and `ShadowRealm`. `haveABadTime(vm, interceptor)` can happen in the main realm, once per object that begins to intercept. Both walk the heap and convert arrays of any thread: each is one safepoint operation, walk and conversions together, and allocates under the scope (08, section 8.6, point 8). The converted storage is `Shared`."

---

### Minor

1. **`maxOffset` of a dictionary (SW11, SW12).** For a dictionary the StructureID before and after a growth of the capacity is the same. What lets the visitor notice is its second compare, of `maxOffset` (`runtime/JSObjectVisitInlines.h:375-376`), and the rule that the mutator stores `maxOffset` **between the nuke and the restored ID** (`:131-134`; `runtime/JSObjectInlines.h:402-407`). SW11 lists "StructureID, pointer, StructureID again"; "Storage first" in 6.4 reorders `Structure::add` and does not say where `setMaxOffset` goes. If it went with "insert the entry", the visitor could accept the new pointer with the old capacity and mark a wrong base address. Add to SW12: "for a dictionary, `maxOffset` is stored inside the same bracket", and to SW11: "and `maxOffset` again".
2. **Byte 4 holds the indexing type and the two bits of the object lock.** cv7, cw3, fs3 store "the indexing byte .. before" the StructureID while the lock is held, and a waiter sets the parked bit by compare-and-swap at any moment. A plain byte store loses that bit, and with it the wake-up. `JSCell::setStructure` has the loop (`runtime/JSCellInlines.h:273-282`); no stub writes the byte today. One sentence in 6.3.6: "the indexing bits are changed by a compare-and-swap that keeps the lock bits".
3. **Which lock orders the reuse of a thread ID.** 6.2 says `VM::m_threadsLock`. An ID is taken from the free list under `VM::m_threadTableLock` alone (09, section 9.2, step 3; 08, section 8.14). It is quarantined under both. The argument stands with `m_threadTableLock`.
4. **Pointer or contents.** 6.3.7: "The validation is by contents, not by the pointer's identity". 08, section 8.6, point 6, lists "storage pointer" first. 06 is right: the requester holds no pointer across its wait (S1').
5. **The all-zero storage word reads `None | Confined | thread 0`** if the enumerators are numbered as they are listed. `Confined` is the one state in which C++ reallocates in place and `AcquireStorage` says `writable` to everybody. A creation site of a butterfly that is overlooked (SW13 has to reach every `Butterfly::create..`) then fails in the unsafe direction. Number the states from 1; 0 is a release assertion in `storageForWriting` and in the visitor.
6. **`pop` against `pop`: "two elements taken"** holds for two threads. With a third operation the compare-and-swap on the length has an ABA: T1 reads `len` = 10 and `v = a[9]`; T2 pops (9); T2 pushes `x` (10); T1's `cas32(10 -> 9)` succeeds, T1 returns `v`, which T2 has returned already, and wipes `x`. All three race on `length`, and 02 allows the outcome; the table of 6.3.4 should not promise more.
7. **Order inside "prepare".** cw1 and the third line of "prepare" in 6.4 name `b` before "`new` = allocate". As written `b` lives across a safepoint (against SW9 and the assertion of A6). Write: allocate first, then load `b` and `w`, then copy; no safepoint from there to the compare-and-swap, as gr1 and gr2 have it.

---

### Tried, without a finding

- Owner's growth (gr2 to gr5) against a second writer's `AcquireStorage`, in both orders of the two compare-and-swaps, x86-64 and ARM64: the store lands in storage that is copied later, or in the successor.
- Owner's growth without the lock against another thread's added property, with and without growth of the capacity; against a lock-free store into an existing out-of-line slot; against conversion, copy-on-write, freezing.
- A reader that takes the new StructureID from the adder and the pointer from the grower (three threads, ARM64): the adder has seen the new pointer before its release store, and ARMv8 is multi-copy atomic, so the reader's later load cannot return the old pointer.
- An owner with a stale `Owned` test that finishes with plain stores of the length, against `raise`: never above `vectorLength`, never a hole below the length that nobody wrote, values above the length stay marked (SW10).
- The marker against growth without a nuke, against storage in the array's own cell (`isStorageInOwnCell`), against capacity growth, first storage, copy-on-write; against sequences on unpublished arrays, which keep "whatever lies between the length and the end of the vector is a hole" (`aot/stubs/AOTStubsArrayPush.cpp:308`), so visiting `vectorLength` slots is safe there too.
- Waiting for `Retired` while holding an object lock: no retiring thread needs a lock between retirement and publication.
- Two closures for one object in one stop; a closure whose array was replaced by storage with equal contents of the header.
- Reuse of thread IDs in `Owned` words.
- N2': the source has no "delete the last property and go back to the previous Structure" (`runtime/Structure.cpp:950-1047`), so no path gives an offset out twice through the identity of a Structure. `propertyStorageSize()` already counts deleted offsets.
- N3 against enumeration by offset: `hasAnyKindOfGetterSetterProperties` is sticky and keeps such Structures off the quick path.
- A slot that gets `emptyGetterSetter` after a custom accessor: C++ dispatches on the type of the loaded cell, not on the attributes (`getOwnNonIndexPropertySlot`).
- Structures that are filled in place without being dictionaries (`putDirectWithoutTransition`; `aot/object/AOTClosure.cpp:374-408`, `aot/object/AOTClassCreation.cpp:334-376`): only on holders that are unpublished by 07, section 7.6.
- PT1 to PT8 against add, remove, rehash, traversal, the marker's clearing of unpinned tables, the sweep of a dead `PropertyTable`.
- 6.5, watchpoint sets: nothing found.

Checked: 08, sections 8.3 to 8.6, 8.9 to 8.11, 8.14, 8.15; 04, sections 4.2, 4.7, 4.11. Against the sources, and against 02, 06, 07, 09, 11.

Paths without a prefix are relative to `Source/JavaScriptCore/`.

Most severe first. "G" is the conductor, "K" a marker thread, "T", "U" JavaScript threads.

| # | Finding | Where | Kind |
| --- | --- | --- | --- |
| Q1 | The stop bit is an async event, so every `RETURN_IF_EXCEPTION` parks, or spins for ever inside a scope | 08 8.4 | memory, hang |
| Q2 | H4 keeps the view and the registry; markers also hold the owners and tables of the `ThreadState` | 08 8.14 | memory |
| Q3 | R4 counts operations that name a variable; variables are also reached through objects and from outside operations | 04 4.2.3 | wrong result without a race; memory on ARM64 |
| Q4 | The last `unpin` has no rule, and `freeAfterSafepoint` has no list for a caller without heap access | 08 8.9 | memory |
| Q5 | A conductor that allocates in a closure parks for its own stop | 08 8.6 | deadlock |
| Q6 | The job queue is a cycle that S3 does not see | 04 4.11, 08 8.4 | hang |
| Q7 | The return list of a `StrongSet` dies with its thread | 08 8.2, 8.14 | memory, narrow |
| Q8 | A thread without access may hold a leaf lock and then ask for access | 08 8.15 | deadlock |
| Q9 | H9 records "the creating thread" | 08 8.11 | contradiction with 09 T1 |
| Q10 | "No wait of ranks 2 to 4" under `m_declarationLock` | 08 8.15 | deadlock, not verified |

Nothing found in: 08 8.3 (the Dekker pair, the sleeps, H12, `lockParking` with H3 were tried with a thread that returns from a native during a stop, with two stops back to back, with a start and an end during a stop); 08 8.5; 08 8.10 (H2, H13, the table of the marker); 04 4.7 (O1 to O3 against the module claims of 09, MD3''); 04 4.11, S3' and S5 apart from Q6.

---

### Q1. 08 8.4: `NeedStopForHeap` is an async event by construction

**The source.** `AsyncEvents = AllEvents & ~NeedExceptionHandling`, `NonDebuggerAsyncEvents = AsyncEvents & ~NeedDebuggerBreak` (`runtime/VMTraps.h:183-185`). A new bit is in both. `RETURN_IF_EXCEPTION` is (`runtime/ExceptionScope.h:131-138`, `runtime/VM.cpp:1300-1305`):

```cpp
if (vm.traps().maybeNeedHandling())                                  // any bit
    if (vm.hasExceptionsAfterHandlingTraps()) return value;          // needHandling(NonDebuggerAsyncEvents) ? handleTraps(NonDebuggerAsyncEvents)
```

`handleTraps` loops `while (needHandling(mask))`, and an event that is not taken is `case NoEvent: break;`, which goes round again (`runtime/VMTraps.cpp:488-528`).

**The text.** Rule 2 of 8.4: "No handler takes it. `takeTopPriorityTrap` skips it. `handleTraps(mask)` calls `heap.stopIfNecessary()` if the bit is set and `mask` contains it." Rule 3 takes the bit out of one caller's mask.

**(a) Hang.**

1. G requests a stop and sets T's bit.
2. T is in C++ inside a `NoSafepointScope`: under an object lock or a table lock, or in an operation that holds `DeferGCForAWhile` because its IR operation is not declared `collects`. It reaches a `RETURN_IF_EXCEPTION`.
3. `maybeNeedHandling()` is true, `needHandling(NonDebuggerAsyncEvents)` is true, `handleTraps` is entered.
4. `stopIfNecessary()` returns at once (`m_noSafepointDepth`). The bit stays. `takeTopPriorityTrap` returns `NoEvent`. `while (needHandling(mask))` holds.
5. T spins with `HasAccess`. G waits for T. Every other thread is parked. The process stands still.

The same for a loop poll of JavaScript that runs under a scope that C++ further up has opened: `operationAOTPollSlow` passes `AsyncEvents`.

**(b) A safepoint where nobody expects one.** Outside a scope step 4 parks. So each of the several thousand `RETURN_IF_EXCEPTION` of the run time is a safepoint. 11, N-d lists the safepoints that a native has to reckon with as "allocation, call of JavaScript, blocking". 06, A3 and S1' are written for those. C++ that loads a butterfly, calls something that cannot allocate (`toNumber` of a primitive, a lookup), checks for an exception and goes on with the butterfly is right by N-d, and a safepoint operation replaces the storage at the check. In release builds nothing notices (`m_liveStorageRefs` is asserted in debug and validating builds only).

**Proposed text** (in place of rules 2 and 3):

> 2. `NeedStopForHeap` is not an event. It is not in `AllEvents`, hence in none of the masks, and `handleTraps` never sees it. `maybeNeedHandling()` masks it out, so that `RETURN_IF_EXCEPTION` stays on its fast path while a stop is pending, and is no safepoint.
> 3. The callers that are safepoints ask for it themselves, behind `handleTraps`: `operationAOTPollSlow`, the interpreter's `check_traps` and `loop_hint` slow paths (`llint/LLIntSlowPaths.cpp:2624`) call `heap.stopIfNecessary()`. `operationAOTPollAtPrologue` does not.
> 4. `shouldStop` in `updateThreadStopRequestIfNeeded()` is `needHandling(AsyncEvents) || hasStopForHeap()`.

### Q2. 08 8.14, H4: what a marker holds of a thread that has ended

H4 keeps "the view and the registry" until the end of the End phase, because of `DeferredRootRegion`s. A region is `{ visit, owner, beginOffset, endOffset }`, and a marker calls `rootRegion->visit(*this, rootRegion->owner, ..)` at any time of the Concurrent phase (`heap/SlotVisitor.cpp:921-944`). `owner` is the C++ object that registered the set, and the table is whatever that object points at:

- `StructureTable::visitRoots` defers `m_writtenRecords` with `owner = this` (`aot/object/AOTStructureTable.cpp:342`). These are the allocation site records. By 07, 7.3.1 and 7.4 they are per thread: the object is part of the `ThreadState`, the records lie in the thread's table arena (`aot/object/AOTStructureTable.cpp:274`), not in the view.
- The private statics (per-thread variables with cards of their own, 07, 7.3.2) need a twin of `ProgramStatics`, which defers with `owner = this` (`aot/runtime/AOTProgramStatics.cpp:173`).

T3, step 2 makes the `ThreadState`. T4 does not name it at all.

1. A full collection begins. The first visit of T's registry defers the region `{ visitRecords, T's front of StructureTable, 0, n }`.
2. T ends in the Concurrent phase. T4, step 9 retires view and registry. Step 11 drops the context, and with it (or before it) the `ThreadState`, its arena and its reserved tables.
3. K takes the region and calls through `owner`: freed memory.

**Proposed text.**

> **H4.** What is retired is the thread's **`ThreadState` with everything it owns** (registry, table arena, the reserved tables of 07, 7.4, the objects that are registered with the registry) and the view. T4, step 9: `m_retiredThreads.append(view, threadState)`. Step 11 frees both, unless step 9 took them.

### Q3. 04 4.2.3, R4: a variable is not only touched by operations that name it

R4: "a module variable, static field or namespace object is a shared location iff some AnyThread function reads or writes it (`noteOperationsOnLocations` .. already records reads and stores per location and function)".

**The source.** (1) It does not record functions. `Locations` holds five event bits and a store count per location (`aot/valueflow/AOTValueFlowResultsLocations.cpp:27-68`); the loop over functions leaves no trace of who did what. (2) "An export is a variable and a property of the namespace object at once, and a static field is a variable and a property of the class object" (`:272`). Those accesses are noted elsewhere, in `computeLocationResults`: `noteAccessesThroughObjects` (with a function, or with none), `noteStaticFields` (`staticFieldsInUnknownHands`, `staticFieldsStoredByBuiltins`, ..), `noteModules` (host reads, `require()` "reads two exports by their names"), `noteEvalScopes`. Two of the five events are `ReadOutsideOperations` and `WrittenOutsideOperations`: "evaluated text, the loader, code that nobody has solved, whoever holds a namespace object or a class object that nobody follows".

**A program without a race.**

```js
class C { static count = 0; }
const k = pick();                                  // "count", not known to the build
const t = new Thread(() => { C[k] = 5; });         // a store by key into the class object
const before = C.count;
t.join();
print(C.count);
```

1. The thread's store is a `PutByVal` on the class object. No AnyThread function has an operation that names the variable. By R4 the variable is `MainOnly`.
2. `join` synchronizes, but `effectsOfOtherThreads()` writes only variables whose `threads` is `Shared`: "MainOnly .. variables .. are untouched" (04, 4.4).
3. `EliminateRedundantReads` forwards the first `GetModuleVariable` to the second. The program prints 0. 02, M1 is broken.

The same with `Object.assign(C, ..)`, `Reflect.set`, and text passed to `eval` on the thread.

**ARM64, with a race.** A MainOnly function stores a new object into an exported variable. "Where F1 is not emitted: .. the variable is MainOnly" (04, 4.10), so the store is a plain `str`. A thread reads `ns[key]` through the run time (`readModuleNamespaceExport`, `ProgramStatics::readVariable`). It can see the reference before the object's header: 02, M6, and behind it a shape test on old bytes. O3 does not apply to the variable either ("with `threads = Shared`").

**Proposed text.**

> R4. A module variable or the variable of a static field is a shared location iff (i) an AnyThread function has an operation that names it, or (ii) it has the event `ReadOutsideOperations` or `WrittenOutsideOperations`, or (iii) an access through an object (`variablesOnObjects->accesses`) is made by an AnyThread function or by no function, or (iv) it is an export of a module whose namespace object, or a static field of a class whose class object, is in `escaped` or `lost`. The step keeps, per function, the list of locations that its operations name: `Locations` has no such index today, only event bits per location.

### Q4. 08 8.9, H11 (with 06, TA1, TA9): the last `unpin`

H11: "Contents with a pin are not put on the deferred list: `transfer`, `resize` and detach .. leave the old bytes to the last `unpin`." TA1 asks for both conditions: "until every thread has passed a safepoint **and** no pin is left". No text says what the last `unpin` does with the bytes.

**(a) It frees them.**

1. R loads `{vector, length}` of a view (`LoadView`). Its window is open.
2. P holds a pin on the buffer (a `readSync` that is about to return).
3. U runs `buffer.transfer()`. The contents have a pin, so they are not deferred.
4. P's call returns, P unpins: the last pin. The bytes are freed.
5. R, still in the window of step 1, reads or writes freed memory. No safepoint has passed.

**(b) It calls `freeAfterSafepoint`**, which "appends to the thread's `m_deferredFrees`". A span "that is handed to another OS thread (Bun's work pool)" is unpinned there: that thread has no `MutatorContext`. A JavaScript thread that unpins before it takes access back appends to its vector while G, for whom the thread counts as stopped, empties "the lists of all threads": two writers on one `Vector`.

**Proposed text.**

> **H11, added.** The last `unpin` of contents that have been detached, transferred or shrunk meanwhile hands them to `freeAfterSafepoint`.
>
> **8.9, added.** A thread appends to its own `m_deferredFrees` only while it has heap access. Any other caller (no access, no `MutatorContext`) appends to `Heap::m_deferredFreesOfOthers` under a leaf lock, which `releaseDeferredFrees()` takes too. An entry that is on a list at the end of a stop was put there by somebody who no longer uses the memory, and every JavaScript thread is at a safepoint: it may be freed.

### Q5. 08 8.6: the conductor parks for its own stop

`stopIfNecessary()`: `if (m_noSafepointDepth) return; if (heap.m_stopRequested.load(relaxed)) park();`. During a safepoint operation `m_stopRequested` is 1, and the conductor is a JavaScript thread that runs ordinary C++. The pseudo-code calls `p->closure()` with no scope open. Point 6 says of a closure: "what it does allocate cannot collect". Point 7: "does not block". Neither opens a scope.

1. T conducts `haveABadTime`. The closure converts arrays to `SlowPutArrayStorage` and makes Structures (`runtime/JSGlobalObject.cpp:2638` and below): allocations.
2. A free list runs out. `allocateSlowCase` calls `collectIfNecessaryOrDefer`, which by 8.4 is `stopIfNecessary()`.
3. `m_stopRequested` is 1: `park()`, `compareAndPark(&m_stopRequested, 1)`. T sleeps.
4. Only T would call `cancelStop()`. All threads sleep.

The same through `reportExtraMemoryAllocated` past its threshold and through the exit of a `DeferGC` scope with deferred work, both of which 8.4 lists as safepoints.

**Proposed text.** In the pseudo-code: `{ NoSafepointScope scope; for (p : batch) p->result = p->closure(); }`. Point 7: "A closure runs inside a `NoSafepointScope` of the conducting thread. So S4 holds for it: it waits for nothing but leaf locks."

### Q6. 04 4.11, S3; 08 8.4: the job queue is a cycle without a safepoint

S3: "every cycle of a control flow graph and every cycle of the call graph contains a safepoint". S3', point 3 lists the edges of the call graph: direct calls, callee stamps, tail calls, accessors, `indirectCalls`. A `Later` callback is none of them.

```js
function poll() { if (!Atomics.load(o, "done")) queueMicrotask(poll); }
```

1. `poll` has no loop and is in no cycle of the call graph: no `LoopHint`, no `EntryIsSafepoint`. It allocates nothing: the closure is a singleton, and a job is 40 bytes of a ring buffer.
2. The jobs are run by the loop in the adapter `EnterSavedFrame`, in machine code (`performMicrotaskJobWord`), or by `MicrotaskQueue::drainImpl`, a `while (!m_queue.isEmpty())` without a poll (`runtime/MicrotaskQueue.cpp:182-253`).
3. U, which is to store `done`, allocates and asks for a collection. G requests a stop. U parks.
4. T's trap-aware limit is the marker: the adapter's loop leaves for C++, and `drainImpl` goes on. Each entry of `poll` fails its stack check and calls `operationAOTPollAtPrologue`, which is no safepoint. If `AtomicSlot` is emitted in line, its only call is behind the epoch compare, which does not fire.
5. T never parks. The stop never completes. U never stores. The process hangs, in a program that has no race and that makes progress on one core under any fair scheduler.

**Proposed text.** A row in the table of 08, 8.4: "| Between two jobs | | `drainImpl` calls `heap.stopIfNecessary()` after each job. The job loop of the adapter leaves for C++ when the trap-aware limit is the marker, as today |". And in S3: "every cycle of a control flow graph, of the call graph **and of the run time's own loops that call compiled code** (the job loop; natives that call back per element: 08, section 8.5)".

### Q7. 08 8.2, 8.14: the return list of a `StrongSet`

8.2: a foreign `deallocate` "pushes the slot on a lock-free return list of the owning set". The set is found through the slot's block (`StrongBlock::blockFor(slot)->strongSet()`, `heap/StrongSet.h:127-130`). T4, step 4 merges the ending thread's `StrongSet` into the heap's, and step 11 drops the context that holds the set. A `Strong<>` is released wherever its holder dies, also on a thread without heap access or on no JavaScript thread (a ticket of 09, 9.13). So neither `m_threadsLock` nor a stop orders it against T4.

1. U loads `block->strongSet()`: T's set.
2. T runs T4: the blocks get the heap's set, the return list is drained, the context is freed.
3. U pushes on the return list of the freed set.

And nobody drains the list of the heap's merged set, since only `allocateSlow()` of the owning thread does: slots that are returned there are lost.

**Proposed text.** "The return list belongs to the **block** (one atomic head per `StrongBlock`), not to the set. Blocks survive the merge of T4. A set drains the lists of its blocks in `allocateSlow()`; the End phase drains those of the heap's set and frees blocks that are empty."

### Q8. 08 8.15: leaf locks in the hands of a thread without access

"Why a stop always ends": "By S4 no thread that is parked or without access holds a leaf lock." S4 forbids *giving up* access inside a `NoSafepointScope` (`releaseAccess()`, `becomeParked()`, `lockParking()` assert). It does not forbid *taking* a leaf lock without access, and the design does so: `notifyConductor()`, the clearing of a W1 edge after a wake-up, `post` and `wake` from a loop that waits, a loop that takes the tasks out of its inbox, `Strong<>` in natives. That is harmless while the holder runs. It is not if the holder then asks for access.

1. T has no access. It takes its inbox lock to take out the tasks, and, because tasks hold cells, calls `acquireAccess()` with the lock held.
2. A stop is in force. T sleeps in `acquireAccess()`, holding the lock.
3. G, in the End phase, posts a `FinalizationRegistry` task to T (8.11) and waits for the inbox lock.

**Proposed text.** "`acquireAccess()` asserts `!m_noSafepointDepth`, as `releaseAccess()` does: heap access changes in neither direction inside a scope. A thread without access may hold a leaf lock; it releases it before it asks for access."

### Q9. 08 8.11, H9: "records the creating thread"

H9: "`Weak<>` records the creating thread beside the context, and `WeakSet::sweep` .. posts `(owner, context)` to that thread's inbox; if it has ended, to the main thread's, with `owner_has_ended`". 09, T1: an ID is given out again after a full collection, in which the visitors of *cells* that carry IDs replace it. A `WeakImpl` is no cell and has no visitor: "Native records name a thread by a reference to its `ThreadContext`, not by its ID."

If H9 means an ID: T ends, its ID is reused by T', the referent dies, and the finalizer with a context that points into T's freed structures is posted to T', without `owner_has_ended`.

**Proposed text.** "`Weak<>` holds a reference to the creating thread's `ThreadContext` (09, T2)". Or, to save the count: "the End phase in which an ID leaves quarantine also walks the `WeakSet`s and replaces it by `NoThread`".

### Q10. 08 8.15, rank 5: `m_declarationLock`

"No JavaScript runs under it, and no wait of ranks 2 to 4." The check phase of declaration instantiation asks the global object for each name (`hasProperty`, `getOwnPropertyDescriptor`). On `JSGlobalObject` and on Bun's global object a name can be a `LazyProperty`, whose first read is Z2: a claim, rank 2. (Not verified call by call.)

1. T holds `m_declarationLock` and meets `Claimed(U)`. By the row it may not wait. It is not in a `NoSafepointScope` either, so S4 gives no way out.
2. If it waits after all, and U's initializer evaluates a script: U waits for the lock, T for the claim. The lock is not in the wait-for graph, so W1 does not see the cycle.

`prepareForThreads()` does not help in a realm of `node:vm`, which has no Instance.

**Proposed text.** "Under `m_declarationLock` a lazy word that is `Claimed(other)` makes the holder release the lock, wait (W1), and start the check phase again." Or the lock becomes a claim of rank 2, with its holder in the graph.

---

### Smaller remarks

- **08 8.14, T3 against 09, 9.10.** An OS thread that `new Thread` has made and that has not reached T3 is in no list, so the fan-out of a termination misses it, and the native join of a VM teardown then waits for its `fn`. 09, 9.10 now says that the request is stored in the `VM` under `m_threadsLock`. T3 needs the step "if the VM has a termination request, set this thread's trap" in its critical section.
- **04 4.11, S5, first row.** "Afterwards each of them finds its word filled, on every thread" is not so for `LinkTimeConstant`: `realm.linkTimeConstants[]` is a word of the view that each thread fills for itself (07, V9, `pulled`). Its slow path copies from a truth that is made, and waits for nothing. And "can be made without running the program's JavaScript" has to be known at build time, since it decides which operations are declared `waits`: 07, 7.6 now asks for a column of the table of built-ins, and lets `new Thread` throw if a promised cell cannot be made.
- **08 8.4.** `handleTraps` takes the lock of the code block set and walks all code blocks at each call (`runtime/VMTraps.cpp:459-467`). While a stop is pending every function entry of every thread that has not parked yet comes through there (8.5). The walk serves breakpoints of the JIT and can be compiled out. As it stands the lock has no rank in 8.15.
- **08 8.11, H6 against H10.** "A swept, empty block loses its owner and may be claimed by any thread's allocator" and "Orphan blocks are never allocated from". Say which holds for an orphan block that the main thread has swept empty.

Read: `07-runtime-state.md` sections 7.3, 7.5, 7.6, 7.7; `09-control.md` sections 9.2, 9.6, 9.7, 9.9, 9.10, 9.11, 9.13, 9.14; `02-language.md` sections 2.6, 2.10; with the rules of 04, 06 and 08 that they cite, and the sources at the two commits named in the README.

Severity: **M** memory safety, **L** deadlock or lost wake-up, **R** wrong result in a program without data races, **P** a promise of 02 is broken, **X** contradiction, or cannot be built as written. "T", "U" are spawned threads.

Nothing found in: 7.3.1, 7.3.2, 7.3.4 to 7.3.6 (but see 7 for addresses of claim words), 7.7, 9.6 apart from G3, 9.10, the rules PR1 to PR10, J2, J3, MD1, MD4, MD5, MD7', T2, T5.

### 1. G3 drops the owner test for a Confined coroutine, and a Confined coroutine can reach the main thread (M)

09, G3: "No test where flow proves the coroutine's site Confined." 04, section 4.2.3: Confined means reachable by no other thread "**while that thread lives**", because 09, J1 runs a plain `then` handler of an ended thread on the main thread.

```js
new Thread(() => {
  const g = gen();  g.next();                 // g: a generator of T, suspended. Its site is Confined
  shared.then(() => g.next());                // the handler captures g
});                                           // fn returns; T completes (a pending reaction keeps no thread alive)
```

1. Flow: `then` on a promise calls its handler `Later`; the handler is not stored under the promise and does not escape (`aot/builtins/promise.builtins.ts:25-42`). So the closure enters `Shared` by no rule of 04, section 4.2.3, `g` neither, and the site of `g` is Confined. The handler is an AnyThread function.
2. T completes. T4 unmaps its view and destroys its allocators (08, section 8.14).
3. The main thread settles `shared`. `post` returns false, J1 sends the handler to the main thread.
4. The handler's `StepGenerator` has no test, by G3. It copies T's frame onto the main stack and jumps. A spilled allocator address, or an address computed from `instanceGPR`, names unmapped memory: exactly what 09, section 9.6 gives as the reason for G1.

G2 ("a coroutine whose thread has ended is dead; `next` throws") is enforced by the test that G3 leaves out. 02, section 2.6 promises the `ConcurrentAccessError`.

**Proposed text, G3, last sentence.** "The test is left out only where the coroutine's site is Confined **and the function that steps it is MainOnly**. In an AnyThread function it stays: such a function can run on the main thread as a handler of a thread that has ended (J1), with that thread's Confined objects." (An AnyThread function cannot pass such an object to a MainOnly one: a function it calls is AnyThread, and a variable it stores to is shared.) 04, section 4.2.3 then cites G3 for "no frame carries such an object on".

### 2. `NeedExit` is answered "at the next poll", and some polls are no safepoints (M, L)

09, X1, step 3: "The main thread meets the trap at its next poll .. It runs the `exit` handlers there, on top of whatever it was doing."

The prologue poll of a function without `EntryIsSafepoint` is `operationAOTPollAtPrologue`, which calls `handleTraps(AsyncEvents)` under `DeferGCForAWhile` (`aot/runtime/AOTExceptionOperations.cpp:60-72`). In mt builds that scope is a `NoSafepointScope` (08, section 8.4), and the caller's IR treats the call as not collecting.

1. U calls `process.exit(1)` and fires `NeedExit` on the main thread.
2. The main thread enters a function. Its stack check fails, the prologue poll takes the bit and runs the `exit` handlers: JavaScript inside a `NoSafepointScope`.
3. The handlers allocate. No collection can run (deferred), and no stop can complete, because this thread does not park: every other thread that requests a collection or a safepoint operation waits for ever. A handler that calls `join` or takes a contended `Lock` hits the assertion of `releaseAccess()` (08, S4). Step 4 of X1 (`becomeParked()`) does too.
4. If the scope is closed first: the frames below are at a place that the compiler holds to be no safepoint. A store whose barrier a later store covers (`LaterBarrierCovers`, `aot/represent/AOTRemoveRedundantBarriers.cpp:431-510`) has run, the covering barrier never will, and a collection during the handlers frees what an old object names and a handler can still read.

08, section 8.4 says for `NeedStopForHeap` that the prologue poll's mask leaves it out. Nothing says it for `NeedExit`.

**Proposed text, X1, step 3.** "`NeedExit` is taken only where a thread may park: in `operationAOTPollSlow` (loop polls, prologues with `EntryIsSafepoint`), in `check_traps` of the interpreter, and in the blocking primitives. `operationAOTPollAtPrologue` passes a mask without it and leaves the polls armed, as for `NeedStopForHeap` (08, section 8.4)."

### 3. A shared dependent that is registered after the change it should have seen is never dropped (R, P: M5)

07, E5: guard bits and a Structure's cached state are shared, and their dependents lie in `SharedChainDependents`. "E1's writer drops from it precisely, after the change and before the `fetch_add`." 07, E3 orders a fill: walk; make the objects report; compare the StructureIDs; file; **register its dependents**; read the epoch again.

For a thread's own caches a missed drop heals itself: the owner's next E2 drops every chain-dependent entry. E2 drops nothing shared. "A guard is set at most once."

1. T arms a member guard that depends on chain object `P`. It walks (old state), marks, compares `P`'s StructureID: unchanged.
2. U changes `P` (new StructureID), takes the lock of `SharedChainDependents`, finds no dependent on `P`, releases it.
3. T registers its dependent, sets the guard bit, reads the epoch: U has not bumped it yet. T keeps the guard.
4. U: `fetch_add`.
5. The bit stays set for good. Every thread, U included, goes on calling the old method behind the guard: U does not see its own write (M5), and a thread that synchronizes with U afterwards does not either (M1).

The section on arming is left to W-7.4, but E5 states a protocol, and the order of E3 is the wrong one for it.

**Proposed text, E5.** "A shared dependent is **registered first and validated afterwards**: under the table's lock, register; after the unlock, compare the StructureID of each object that the answer depends on with the one that the walk saw; if one differs, drop the dependent and do not set (or clear) the shared word. E1's writer changes the object first and takes the table's lock afterwards. So either the writer's scan finds the dependent, or its critical section came first, and then the change is visible to the compare."

### 4. A loop that spins on an access by name never runs E2 (P: M10; X with 2.10.5)

02, M10: "A read, or a test of an object's state (whether a property exists ..) that can decide whether a loop is left is performed again in every turn .. in the loop itself or in a function that it calls." 04, section 4.12: "an access by name: nothing: it ends everything anyway."

The compiler keeps the access in the loop. The access is answered by the thread's own caches. 07, E2 lists when a thread compares the epoch: after a stop, at a synchronizing operation, "in the slow path of every poll and allocation", at a fill.

```js
while (o.ready === undefined) {}          // untyped o; `ready` is not an own property
```

1. T's first turn files `(StructureID of o, "ready") -> Absent`, chain dependent.
2. U runs `Object.getPrototypeOf(o).ready = true`. `o`'s StructureID does not change. U drops its own entries and bumps the epoch.
3. T's probe hits `Absent` in every turn. The loop has no synchronizing operation, does not allocate, and its poll never takes the slow path unless somebody requests a stop. T spins for ever.

The location is an own property of the prototype, one of 2.10.1. The same for an inherited accessor that replaces a data property, for a hiding property put lower in the chain, and for the interpreter's `get_by_id` caches in unset and prototype mode, which "are cleared by E2 only" (10, section 10.2).
2.10.5 says of this very change: "the old or the new member, until the reader's next synchronization operation". For a spinning reader that contradicts M10.

**Proposed text.** 04, section 4.12, row "an access by name": "the `LoopHint` gets the flag `comparesCacheEpoch`: its fast path ends with the compare of 07, E2a." 07, E2: add "at a loop poll whose loop's exit depends on an access by name (04, section 4.12); in the interpreter, in `loop_hint`". 02, section 2.10.5, that row: ".. until the reader's next synchronization operation, or the next turn of a loop whose exit depends on it (M10)".

### 5. The start of a job or task that another thread sent is not among the places that run E2 (R)

02, section 2.10.2 has the edges Reaction and Task; 04, C5 names their acquiring ends "the start of the job", "the start of the task". 07, E2 runs "at every synchronizing operation with acquire meaning", and 04, section 4.4 explains: "Where the operation is a call the run time does that. Where it is emitted in line .." The start of a job is neither. 09, section 9.7: "The owner's loop takes the tasks, puts each reaction job at the end of its microtask queue, and drains."

1. T holds a chain-dependent entry for `obj.m`. It has registered a reaction on `p` and runs other timers meanwhile, so it is awake and has heap access.
2. Main: `Proto.m = f2; resolve(p)`. The epoch moves; the job is posted.
3. T takes the task under the inbox lock and runs the reaction: `obj.m()` hits the old entry and calls `f1`.

The program has no data race (the store happens before the reaction). H12 does not help: no stop took place.

**Proposed text, E2.** Add: "when it takes tasks out of its inbox (09, section 9.7)". 09, section 9.7: "After it has taken tasks from the inbox the loop runs E2 of 07, once per batch."

### 6. A stale edge of the wait-for graph gives a correct program `Error("deadlock")` (R)

09, W1: a waiter stores its edge before it blocks, and "clears the edge when it wakes". A walk trusts every edge that it finds.

1. T blocks for a module claim (or a claim of 07, Z2) that U holds: edge T to U.
2. U ends the claim and wakes its waiters. T is runnable, and has not run yet.
3. U calls `t.join()`. Under `m_waitGraphLock` it stores U to T and follows: T to U. The walk is back at U.
4. U throws `Error("deadlock: ..")`. There is none: T is about to go on and will complete.

The holder of a claim lives on after the claim, unlike the target of a `join`. And the pattern is the common one: a worker needs a module that the main thread is loading, and the main thread then joins the worker (the program of 09, section 9.9).

**Proposed text, W1.** "An edge is cleared by whoever ends the wait: the thread that ends a claim, or completes, takes `m_waitGraphLock`, clears `m_waitsFor` of each of its waiters, and only then wakes them." (Or: an edge names what is waited for, claim and generation, and a walk follows it only if that claim is still held.)

### 7. The waits of MD3'' and Z2 have no condition, and a claim word has a different address in every view (L)

`join` is specified ("the state is tested again under the condition's lock, and step 16 notifies under it"). The other two waits of W1 are not.

**(a)** 09, MD3'': "a synchronous caller releases the lock, waits (W1, heap access released) until that claim has ended".
1. T, under `Modules::m_lock`, sees that U owns a module. It releases the lock.
2. U ends its claim: clears the owner words, "waiters are woken". Nobody waits yet.
3. T enters the graph and blocks. Nobody will wake it.

**(b)** 07, Z2: "a loser that finds `Claimed(other)` waits (heap access released, woken by the winner)". The claim word lies "in the shared statics". The pattern at hand is that of 09, section 9.13: "parks in `WTF::ParkingLot` on the word's address". The shared statics are mapped at another address in each view (07, section 7.3.1), and an operation gets the caller's view (V1). A loser that parks on its own view's address is not found by a winner that unparks on its own.

**Proposed text.** MD3'': "A claim is a record with a list of waiters. The caller appends itself under `m_lock`, in the critical section in which it saw the owner; the end of a claim takes the list under `m_lock` and wakes each entry. A waiter sleeps on a flag of its own entry." Z2, part 3: "A waiter parks by `ParkingLot::compareAndPark` on the **canonical** address of the claim word (V1), with the value that it read; the scope object stores the word, then calls `unparkAll` on the canonical address, always."

### 8. Two reactions of one thread on one promise can run in the wrong order (P: 2.6)

02, section 2.6: "Reactions of one promise that were registered by one thread run in the order of registration." 09, section 9.7: reactions are posted "after the busy bit is clear (PR7)"; a `then` on a settled promise queues its job at once.

1. T: `p.then(r1)`. `p` is pending: `r1` is stored in `p`.
2. Main settles `p`: takes the busy bit, takes the reactions, stores the settled word, clears the bit. It has not posted yet.
3. T: `p.then(r2)`. `p` is settled: `r2` goes to T's microtask queue.
4. T's job ends, the queue is drained: `r2` runs.
5. Main posts `r1`. T's loop takes it later.

Even if main had posted before step 3, `r1` would lie in the inbox, which the *loop* empties, while `r2` is in the microtask queue, which is drained first. "The ordering" paragraph of 9.7 looks only at reactions that are posted together.

**Proposed text.** PR7: "The jobs of reactions whose home is another thread are appended to the inboxes **before** the bit is cleared; the inbox lock and the table lock rank below the bit (08, section 8.15). The wake-ups come after." J1: "A thread that finds a promise settled, in `then` or `await`, first moves the reaction jobs of its inbox to the end of its microtask queue (one load of 'inbox is not empty' on the fast path)." Or weaken 2.6: the order holds among reactions that were registered before the settling, and among those registered after it.

### 9. By the definition of 2.10.2 every promise that crosses threads is a data race (X)

2.10.1 makes "the state of a promise" a location. `then` and `await` read and write it, a settling writes it. 2.10.2: "A data race is a pair of conflicting accesses, not both `Atomics`, that are not ordered by happens-before." The Reaction edge orders the settling with the *reaction*, not with the *registration*.

So `await p` on T against `resolve(p)` on main is a data race, and with it `asyncJoin`, `asyncHold`, `waitAsync`, and the example of 02, section 2.6. M1 then says nothing about any program that uses them, and "a program without data races" in 04, 07 and 09 loses its subject. The same for the state of a `Thread` (`join` against completion), of a `Lock`, of a module.
2.10.5 treats the pair as well defined ("the reaction runs, once"), so the intent is clear.

**Proposed text, 2.10.2.** "A data race is a pair of conflicting accesses that are not ordered by happens-before and are not both **atomic**. Atomic are: the `Atomics` operations; every access to the state of a promise (`then`, `await`, settling, the test for 'handled'); every operation of `Thread`, `Lock`, `Condition`; the state of a module. All atomic accesses to one location are totally ordered, consistently with happens-before." (Whether `Map` and `Set` operations, which 2.10.5 calls linearizable, belong here is a decision: as written `m.get` against `m.set` is a race that has only good outcomes.)

### 10. The leftovers of `prepareForThreads()` have nowhere to go (X)

07, section 7.6: "What is left (a class whose definition runs program code, a host value that `forgetValuesNamed` has reset, a built-in whose module cannot be loaded ahead) is reached by operations that are declared `waits`; the compiler knows which from the image's tables. An operation without the word that finds its cell missing after all returns `MustRetryAtSafepoint` (S4)."

**(a) The compiler cannot know two of the three.** `forgetValuesNamed(key)` runs when the host stores to a member at run time, for every value of that name (`aot/runtime/AOTBuiltinCells.cpp:354-366,553`). Whether a module "cannot be loaded ahead", whether a cell is unavailable in this realm (`m_isUnavailable`) or awaited (`m_awaitedMembers`, `:243-259,368-390`) are facts of the run.

**(b) `MustRetryAtSafepoint` has no receiver.** 08, S4: the frame that opened the scope may wait "only if its own caller may park: in compiled code, if the IR operation is declared `waits`". `Builtin` is `-> constant ; CreatesConstant` (`aot/ir/schema/operations.def:29`): an operation with one result and no second edge, across which `window` values and narrow references are live. Its stub can neither park nor go elsewhere.

**(c) The slow path writes C++ containers.** After the prefill a cell that stayed empty sends every execution of its `Builtin` to `BuiltinCells::cell`, which calls `m_awaitedMembers.removeAt`, `m_isUnavailable.ensureSizeAndSet`, then `loadModuleOf` and `find`. The `BuiltinCells` lock is a leaf (7.7); the last two run code.

**(d) Values are not cells.** `fillValue` is "the read that the program wrote: the host makes the value now, by code that may throw and that the program may observe" (`:322-350`). Prefilling "built-in cells **and values**" moves an observable read to the first `new Thread`.
Z1's third addition ("the creator reads what it made the thing from again under the lock") cannot be done for it: the source is `holder->get()`, which runs code. Without it: T reads the old value; the host, on U, stores a new one and resets the word (already 0); T publishes the old value; U reads its own member and gets the old one, for good (M5).

**Proposed text.** "`prepareForThreads()` settles each **cell** for good: filled, or recorded as unavailable. Afterwards the six operations only load: an empty word means `unavailable`, and their slow path looks nothing up and writes nothing. A cell that can come into being later (`awaitCreation`; mark `MayBeAbsent`; a module that is loaded on demand), known from the table of built-ins, is read by a form of the operation that is declared `waits`. Built-in **values** are not prefilled; their read is an operation that `throws`, `collects` and `waits` already. `BuiltinCells` has a generation that `forgetValuesNamed` increments under the lock; `fillValue` reads it before the `get` and publishes under the lock only if it has not moved." The sentence on `MustRetryAtSafepoint` goes.

### 11. MD3'' takes a set that is not always known when the claim is made (X, with an abort: M11)

"`beginEvaluation(root)`, under the lock, walks the modules that are reachable from `root` and not `Evaluated`." MD8 rests a **release assertion** on it: "a parent has the owner of its child, because a claim is closed under 'imports and is not evaluated'."

For modules of the image the import graph is data. Not for: a `require(x)` inside a body (the target is known when the statement runs); a module that is not compiled (`ImportsUncompiledModule`), whose imports are known after it has been fetched and parsed, which MD3'' places *behind* the claim ("Link, load and evaluation then work on the thread's own set").

1. T: `import("./m.mjs")`, not compiled, with a top-level `await`. Claim {m}, `EvaluatingAsync`.
2. Main: `import("./n.mjs")`, not compiled. The walk knows only `n`. Claim {n}.
3. Loading `n` finds its import of `m`. `m` is owned by T. No rule says what happens. The standard's algorithm makes `n` a pending parent of `m` (`runtime/CyclicModuleRecord.cpp`), and T's `asyncExecutionFulfilled` meets a parent with another owner: the assertion of MD8 ends the process. Without the assertion `n`'s body runs on T, which `MainOnly` forbids (04, section 4.2.2).

Whether the records of uncompiled modules go through `AOT::Modules` was not verified; CommonJS does ("`evaluateCommonJS` runs under a claim").
For `require` in a body the text does have an answer (wait inside the body, W1). Its price should be stated: two threads that enter a CommonJS cycle from its two ends get `Error("deadlock")` in one of them, where one thread sees a partial `exports`.

**Proposed text, MD3''.** "A claim **grows**. Whenever load or link finds a module that is not `Evaluated` and not in the claim: under `m_lock`, if nobody owns it, it joins the claim; if another thread owns it, the claimant waits for that claim (a synchronous caller by W1, with its own claim held; a job of `import()` by the ticket), before it links against the module. No module becomes a pending parent of a module with another owner." MD8: the assertion stays.

### 12. Thread IDs run out while none is in use; carriers of an ID that T1 does not list (P: 2.2, 2.7)

**(a)** 02, section 2.2: `RangeError` "over the limit (65,534 **live** threads)". 09, T1: an ID returns "at the end of the first full collection that began after T4". 09, section 9.2, step 3: "`RangeError` if none". A program that starts short threads one after the other and allocates little sees no full collection. After 65,534 threads `new Thread` throws, with one thread alive.
**Proposed text, step 3.** "If the free list is empty and the quarantine is not, the caller runs `collectSync(Full)` and tries again. (`new Thread` is declared `waits`.) A collection that had begun before the last T4 frees nothing of it, so it may take two."

**(b)** T1 lists the cells whose visitors rewrite a quarantined ID. Not in the list:
- the owner of a restricted object: the Structure variant `Restricted(owner)` (05, section 5.11) and the thread field of the storage word of kind `Restricted` (06, section 6.4). After the ID is given out again the new thread passes the owner test of an object that 02, section 2.7 calls "usable only by the calling thread", with "no way back";
- the creating thread of a `Weak<>` whose owner is `owner` (08, H9). 08 now says that `WeakBlock::reap` rewrites it; T1 should name it.
**Proposed text, T1.** Add both, or: "`Restricted` names its owner by a number that is never reused (the `ThreadLocal` key space of 9.12 has 64 bits)."

### 13. `Atomics.wait` on a property: the compare and the sleep (L)

09, section 9.13 gives the key of a waiter and nothing on the order of "read the property, compare, enqueue". If the read comes first: T reads the expected value; U stores and calls `notify`, which finds no waiter; T enqueues and sleeps for ever. If the read is made under the lock of the waiter lists, a leaf of rank 17 in 08, section 8.15, a property that needs the object lock (rank 9: a field outside its slot, a dictionary) is read against the order.
**Proposed text.** "`wait`: under the list's lock, enqueue. Unlock. Read the property as `Atomics.load` does. If it differs: dequeue, `"not-equal"`. Else sleep on the entry's flag, which `notify` sets under the list's lock before it wakes."
The same sentence is missing for `Condition.wait`, which holds the condition's internal lock while it releases the `Lock` (whose slow path takes the lock object's internal lock): two locks of one rank. State the order: condition, then lock.

### 14. Smaller points

| # | Where | Point |
| --- | --- | --- |
| a | 09, section 9.2, steps 14 to 16 | After `closeInbox()` the hooks of step 15 close handles, which rejects pending promises. A reaction whose home is the running thread is queued "on the thread's own queue", which nobody drains any more: a handler function is lost, where J1 promises it the main thread. Text: "after `closeInbox()` has succeeded `enqueueAtHome` and the settling paths treat the running thread as ended" |
| b | 02, section 2.6, "Listeners of `process`: registered on the main thread and run there" | 04, section 4.2.2 and 11 must agree on what `process.on` and `process.emit` do on a spawned thread: refuse (`main`). 11, section 11.4 classifies `process.on` "for signals" only (not re-read in this round) |
| c | 07, section 7.3.3, mark `constant` | `hasInstanceUID`, `prototypeUID`, `defaultHasInstanceFunction`, `infoOfClassPrototypes`, `handlers` are written when the lazily made `PropertyCaches` is constructed (`aot/runtime/AOTPropertyCaches.cpp:154,426-429,441`), which V4 makes per thread and on demand. They are not in the canonical view when a view is copied from it. Either the realm's set-up fills them, or their mark is `thread` |
| d | 09, X1, step 3 | `exit` handlers that wait for the thread that called `process.exit` (blocked for good in step 2) hang. W1 does not see it. One sentence in 02: a thread that has called `process.exit` never completes |

Attacked: `05-compiled-objects.md`, sections 5.2 to 5.6, 5.10, 5.14; `04-compiler.md`, sections 4.4, 4.6, 4.9, 4.12, 4.13. With the sources of both commits at hand, and against 02, 06, 07, 08, 09 where they tell the same protocol.

T, U are JavaScript threads. Most severe first.

| # | Finding | Kind | Severity |
| --- | --- | --- | --- |
| 1 | `GetDeferredClosureSlotOrUndefined` has no counterpart among the terminators of C3 | gap | memory safety |
| 2 | The side object's storage is retired behind the point of no return, and the retirement can fail | contradiction (L6 against 06, SW5, SW12) | lost property (02, M8), or a mutation that cannot end |
| 3 | An `Atomics` operation in line and the same operation through the call are not atomic with each other | gap | wrong result in a program without data races (02, section 2.5) |
| 4 | A layout without a hidden word keeps its side object in the Structure, which step 1 of 5.6 (a) stores after bit 28 | contradiction with the source | crash (02, M11) |
| 5 | "For a Confined layout the run time does what it does today" covers state that is not the object's | unstated limit | memory safety, if read to the letter |
| 6 | CO3 says that no fast path overwrites `absent`; the first row of 4.6.2 does | contradiction, harmless | wording |

### 1. 04 C3, 4.6.1; 05 5.4: `GetDeferredClosureSlotOrUndefined`

C3: "Every read of a slot .. that has markers, of a subject that may be shared, is a terminator." 4.5 and 4.6.1 name the operations that are replaced: `GetSlot`, `GetSlotGetter`, `GetDeferredClosureSlot`, `GetSlotOrUndefined`; `GetFlattenedSlot` is confined to Confined outer layouts. The family `SlotAccess` has one more reader (`aot/ir/schema/prepared-operations.def:40`):

```
operation GetDeferredClosureSlotOrUndefined (object, guard?, with SlotStamp) -> declared(key)
    ; Allocates reads(ObjectFields[key], NamedProperties, OrderWords) writes(ObjectFields[key]) ; relies = Shape ; runtime = MaterializeDeferredClosure
```

The plans choose it wherever a slot that defers its closure may be empty (`readOfSlotThatMayBeEmpty`, `aot/analysis/AOTPropertyAccessSlots.cpp:364`). No document names it. It `relies` on `Shape` only, so the verifier's new check (`SlotFilledOnSharedSubject`) does not see it. It has two emitters, and both have a hole.

**(a) The result of the operation is decoded untested.** `loadDeferringSlot(.., readsEmptySlotAsUndefined = true)` and `loadDeferringSlotOrFieldOutsideIt` (`aot/codegen/AOTSlotAccessEmitter.cpp:483-502, 520-538`) call `MaterializeDeferredClosure` for the word 0 and pass `compress(result)` to `decodeWordOfDeferringSlot`.

1. T loads the slot: 0, "not made yet". It calls the operation.
2. U runs `delete o.m`, or moves the field out: the slot holds 1, `absent`.
3. T's operation loses (05, section 5.4: `w == 1: empty`). By 4.6.1 "it returns null then, and the emitter tests the result and takes `empty`". That sentence is written for `TryGetDeferredClosureSlot`. An `operation` has no edge `empty`, and these two emitters test nothing.
4. T compresses a null pointer and decodes it at the type of a closure. The next use is a call through it.

**(b) `absent` is read as `undefined` under a guard that may be stale.** `mayHaveLeftDeferringSlot` (`:505-518`) picks the form without a look at bit 27 where "the read stands behind the test that no field lies outside its slot". 4.5 says of exactly that guard (`NoFieldOutsideSlot`) that it may be stale and that "`undefined` was never its value (02, M3)", and repairs `GetSlotOrUndefined` by `TryGetSlotOrUndefined`. The deferring twin is left.

**Proposal.** 4.6.1, one more row, and its name in 4.5 and in the table of 05, section 5.4:

```
terminator TryGetDeferredClosureSlotOrUndefined (object: Object/inspects, guard?: Guard/inspects, with SlotStamp)
    -> value(value: declared(key)), outside ; Allocates reads(ObjectFields[key], OrderWords) writes(ObjectFields[key]) ; relies = Shape ; guarded = object
    ; runtime = MaterializeDeferredClosure ; replaces = GetDeferredClosureSlotOrUndefined
```

"One load. A reference or a marker: `value`. 0: the operation; a null result is treated as the word 1. 1: load the order word (the load of the slot is an acquire on ARM64); bit 27 set: `outside`; else `value` receives `undefined`. The form without the look at bit 27 is used only for a field that cannot be moved out." And C3's check becomes structural: "the verifier rejects every *operation* of the family `SlotAccess` with `access = Read` whose slot has markers and whose subject may be shared, whatever it relies on."

### 2. 05 L6, 5.6 (a), (e), 5.11, 5.14: the retirement comes too late

L6: behind the point of no return the mutation "does not allocate, does not wait and cannot fail"; what it needs, "a free slot in the side object's storage" among it, "is made before, and checked under the lock in front of that store". The check of 5.6 (a) is: "the object's StructureID, the side object's StructureID and its storage pointer are those that were planned from".

A free slot cannot be made before. The out-of-line capacity is a function of the side object's Structure, and by 06, SW12, storage of another capacity is installed "between the nuke of the StructureID and the store of the new one", that is in step 5. What can be made before is the new storage, filled with a copy (06, section 6.4, `prepare`). Installing it begins with `CAS(b-16, Owned(me), Retired)` (06, section 6.4, step 1), and that can fail: a store into an **existing** out-of-line extra property takes no lock (05, section 5.3), and its state test turns `Owned(T)` into `Shared` at any moment (06, SW5).

1. The side object's storage is full and `Owned(T)`. T prepares: next Structure, new storage, copy. L6's test "not `Owned` by this thread" answers no, so T goes the way of the lock.
2. T locks `o`. StructureIDs and storage pointer are as planned. Steps 1 to 3.
3. Step 4: T exchanges the slot for `absent`. Point of no return.
4. U runs `o.extra = 1` for an extra property that exists, through its cache: tag compare, `AcquireStorage`, `Owned(T) -> Shared`, store. No lock, no conflict with T's property (02, section 2.10.1).
5. Step 5: T's compare-and-swap to `Retired` fails. The storage is `Shared`, so only a safepoint operation may replace it, and T holds an object lock (L1). T's copy lacks U's store as well.
6. T can neither go on nor let go: the state that the paragraph "Why L6" describes.

The same in 5.6 (e) if a value has been made visible before, in 5.11 (one point of no return per field), and in 5.14, step 2 ("room for the property" in the class object's storage, behind `exchange(variable, absent)`).

Smaller: in step 5 `storageForWriting` may meet `Retired`, because the owner of the side object's storage grows its **elements** without a lock (`o[7] = x` on an object with a layout; 06, section 6.3.5). That wait is bounded, but "does not wait" does not hold to the letter.

**Proposal.** CO4 offers the way out: "A copy of the name among the extra properties is ignored while the slot is filled." So everything that can fail moves in front of the exchange.

| Step of 5.6 (a) | New order |
| --- | --- |
| 1 to 3 | as now |
| 4 | put the property into the side object, by 06, section 6.4, steps 1 to 3 (retire, nuke, publish, value, StructureID). Its value is the value that did not fit, or the accessor, or, where `old` is wanted, a placeholder (`undefined`). If the retirement fails: unlock, and start over as a safepoint operation. Nobody can see the property: the slot is filled (CO4), and the object's StructureID does not name the side object's new Structure yet |
| 5 | `old = exchange(slot, absent)`: **point of no return** |
| 6 | where `old` is wanted: store it into the side object's slot. One store into a slot that exists, behind `storageForWriting`, which cannot fail |
| 7, 8 | the object's StructureID; invalidations |

L6, last sentence but one: "Whether storage is `Owned` by this thread is decided by the compare-and-swap that retires it, not by a look at it. That compare-and-swap lies in front of the point of no return." 5.14: the ordinary property is added (at its fresh offset, under a Structure that the class object does not have yet) before `exchange(variable, absent)`; while the variable is filled it is the truth (CO13).

### 3. 04 4.6.6; 05 5.4; 02 2.5: two implementations of one atomic operation

`AtomicSlot` in line is a compare-and-swap loop on the slot and takes no lock. Its `miss` edge, every site whose key or layout is not known, interpreted code and natives use `operationAOTAtomicsOnProperty`, "which takes the object lock". Nothing says how the operation **writes**. The only rule for C++ stores is 05, section 5.4: `Object::tryStore` is "`compareExchange` from the loaded word that is not `absent`" for a field that can leave its slot (the word that `tryStore` itself loads), and a "plain (release) store" for any other field.

1. `o.n` is a plain `Int32` field, 5. Flow proves `ChangeIntegrity` impossible, so writing forms are emitted in line.
2. T: `Atomics.add(o, k, 1)` with a computed `k`: the operation. It takes the lock and reads 5.
3. U: `Atomics.add(o, "n", 1)` in line: `lock xadd`, or compare-and-swap 5 for 6. U does not look at the lock.
4. T stores 6.

Two `Atomics.add`, one increment. Both accesses are `Atomics`, so the program has no data race (02, section 2.10.2), and 02, section 2.5 says: "Every operation is one atomic step."

The lock orders the operation against other holders of the lock. It does not order it against the inline form.

**Proposal.** 4.6.6, behind the table of forms: "`operationAOTAtomicsOnProperty` on a property that lives in a slot (in line or in the Extension) runs the same five steps on the slot's word, under the object lock: the lock is there for what it protects (integrity, creation, a field that has left its slot), the compare-and-swap for the inline forms, which do not take it. The word it swaps from is the word its result was computed from. `Object::tryStore` is not used for a read-modify-write." And in 05, section 5.4, row `Atomics`: "in line and in C++: 04, section 4.6.6".

### 4. 05 5.6 (a) step 1, table "What lock-free readers of metadata may see", CO7: the side object that a Structure holds

Step 1: "hidden word (release), bit 28 (release), StructureID of the variant with extra properties (release)". Reader's row: "C++ `extraProperties()`: order word (acquire), bit 28, then the hidden word | the hidden word was stored before bit 28 | never 'has extra properties' with an empty word".

Not every layout has that hidden word. Without `extraPropertiesSlot()` the side object hangs on the **Structure variant** (`CompiledStructure::heldExtraProperties`, `aot/object/AOTCompiledStructure.h:108-111`): `writeExtraPropertiesWord` returns without a store (`aot/object/AOTObjectExtraProperties.cpp:45-49`), and `cellInPlaceOfExtraProperties` reads `structure.heldExtraProperties()` (`:32-38`). For such a layout the word that bit 28 licenses is the StructureID, and step 1 stores it **after** the bit (so does the source, `setExtraProperties`, `:644-653`).

1. T, under the lock, moves a field of such an object out: bit 28 is set.
2. U, without a lock: `extraProperties()`: `hasExtraProperties()` is true. `extraPropertiesSlow()` loads the StructureID, which is still the variant without extra properties: `heldExtraProperties()` is null.
3. `cell->type()` (`:624-630`) dereferences null: a fault at a site that is not registered ends the process (09, section 9.5; 02, M11).

**Proposal.** Step 1: "For a layout that has no hidden word for the extra properties the order is: StructureID of the variant that holds the side object (release), then bit 28 (release). The reader loads the order word (acquire), then the StructureID." That is harmless to the other readers: after step 1 the side object is empty, so a thread that sees the new StructureID and a clear bit 28 answers as before the step. A row in the table of F3 (04, section 4.10): "order word (bit 28) | StructureID, for the held side object". CO7: "the word of the extra properties, or, in a layout without one, the side object that every later Structure variant of the object holds".

### 5. 05, second paragraph; 5.3 "Who does not": what "Confined" does not license

"Everything here concerns objects of a Shareable layout. For a Confined layout the run time and the compiler do what they do today." Confined is a fact about **objects**: none is reachable by another thread than the one that allocated it (04, section 4.2.3). It is not a fact about the class. Ten threads may each allocate instances of one Confined layout, and these share the usual Structure of the shape, its variants, the layout's run-time records, and the tables.

Today's run time writes such state from operations on a single object:

- `CompiledStructure::setListedFields` stores three words into the shared Structure from a **read** of an object's keys (`aot/object/AOTCompiledStructure.h:157-161`, `aot/object/AOTObjectKeys.cpp:58`); `setNumberOfExtraKeys` (`aot/object/AOTObjectKeyOrder.cpp:198`).
- Structure variants, added key orders, class tables.

1. T and U each hold an object of their own, of one Confined layout and one Structure. Both call `Object.keys`.
2. The run time sees the bit `ReachOtherThread` absent and takes today's path: T stores `m_listedOrderAndCount`, `m_listedSize`; U stores all three; T stores `m_listedFields`.
3. A third call pairs one list's size with the other's pointer: out of bounds.

5.13 has the right rule (one pointer to an immutable record, by compare-and-swap), and 07, section 7.7 gives the tables their locks. But 5.13 stands under the sentence quoted above, and the run time "tells the two apart by a bit of the layout record".

**Proposal.** Second paragraph of 05: "For a Confined layout the compiler, and the run time **as far as it reads and writes the object itself** (cell, Extension, side object and their storage), do what they do today. Whatever several objects share follows this document, 06, section 6.5, and 07 whatever the layout: Structures and their caches (5.13), key orders, class and closure tables, per-layout records." The validating build that checks `ConfinedObjectOnOtherThread` cannot see this; ThreadSanitizer on two threads that work on Confined objects of one class can (a line for 12).

### 6. 05 CO3 against 04 4.6.2, first row

CO3: "No fast path overwrites `absent`: not compiled code, not a stub .." The first row of 4.6.2 gives a plain `PutSlot` to a field for which flow proves `IsNeverDeleted`, `IsNeverRedefined`, `IsNeverStoredWithOtherType`, "then `absent` never appears after birth". It does appear **at** birth. `PutSlot` is also the *creating* store of a lazily initialized field in a layout whose keys are never enumerated (`creatingStoreFor`, `aot/analysis/AOTPropertyAccessSlots.cpp:315-318`), and `PutSlot` relies on `Shape` and `NotFrozen`, not on `SlotFilled` (`aot/ir/schema/prepared-operations.def:41`). So a plain store over `absent` on a published object is part of the design.

It is harmless: nobody ever writes `absent` into such a slot, so there is no mover whose work could be undone, and two creators race like two stores. But CO3 is cited as the reason why step 4 of 5.6 (a) is final, and an implementer who takes CO3 to the letter turns every lazy initialization into a trip through the generic version.

**Proposal.** CO3: ".. No fast path overwrites an `absent` **that a mutation of 5.6 has written**. A field that no mutation can empty (first row of 04, section 4.6.2) may be created by a plain store where the layout lists no keys."

### Sections without a finding

- **05 5.5** (Extension). Zero is `absent` in every representation that may lie out of line (`outOfLineRepresentationFor`, `aot/place/AOTSlotRepresentations.cpp:121-125`: checked). The loser's reliance on the winner's barrier holds: a collection needs the winner at a safepoint.
- **05 5.10** (stand-in prototype). Claim, preparation, one closure: no interleaving found. A claim held over a safepoint operation is allowed by 08, section 8.6, point 1.
- **05 5.14** beyond finding 2. Tried: `TryPutVariable` against leaving and entering again (the ABA is a legal order), the custom getter against step 1, lazily made custom properties (`ensureProperties` is in `prepare`).
- **05 5.6 (b), (c), (d), (f), (g)**: no finding. The lock-free step from pending stars to the side object does not collide with step 1 of (a): an object with pending stars has bit 28 and its Structure already (`aot/object/AOTObjectExtraProperties.cpp:288`).
- **04 4.4**: no finding.
- **04 4.6.2, 4.6.3, 4.6.4, 4.6.5**: no finding. 4.6.3 and 06, sections 6.3.1 to 6.3.3, tell the same sequences.
- **04 4.9**. C4 fits the source: a load whose range is wider than one heap is keyed by the state of all memory (`keyForLoad`, `b3/B3MemoryNumbering.cpp:362-375`). `mergesAdjacentConstantStores` pairs only at offsets that are multiples of 8 (`b3/B3MergeAdjacentConstantStores.cpp:161`), so a merged store is aligned. The register allocator rematerializes constants only (`b3/air/AirAllocateRegistersByGreedy.cpp:5361-5533`): the "not verified" of the last row can go. No reduction that narrows or splits a load was found; the one that exists replaces a narrow load by a part of a wide one.
- **04 4.12, 4.13**: no finding. 4.13 and 09, section 9.13, agree on both compare-and-swaps.

## B.6 Interleavings that were tried and that the rules withstand

Listed so that nobody tries them again without new evidence.

| Attack | Why it fails |
| --- | --- |
| Owner stores after its tag compare while a foreign writer makes the storage `Shared` and asks for growth | The growth is a safepoint operation. The owner's test and store are in one window, so the store is made before the copy |
| Foreign writer's compare-and-swap to `Shared` against the owner's to `Retired` | One word and monotone states. The loser sees the winner's state. No ABA: `Retired` is final, the low half of the word is constant for storage that is not `Confined`, and the address cannot be reused before the spinner's next safepoint (SW8) |
| The owner waits for the spinner | Between retirement and publication the owner takes no lock and calls nothing (F34) |
| Thread ID reused while storage still says `Owned(id)` | The release and the allocation of an ID pass through `VM::m_threadsLock`, so the old bearer's stores happen before the new bearer's first test. At no time do two threads pass the test |
| Array born as one cell: growth leaves the in-cell storage `Retired` | The old storage lives as long as the cell, which the reader holds. `isStorageInOwnCell` keeps the visitor from marking it as auxiliary memory (but see F8 for where that fact should come from) |
| Typed `number[]` with a proven kind | No writer of another kind exists in the program, whatever the interleaving. The state test on stores and the hole test on loads stay, so F2 and F10 are the only exposures |
| `push` or `pop` with a stale length | Length and bound come from one `b`; `vectorLength` never shrinks; so the access stays inside the allocation. What goes wrong is F2, not the bounds |
| Old StructureID with new storage, for named properties | Capacity only grows outside safepoint operations, and offsets keep their meaning, given F4 |
| Per-thread cache entry for a cacheable dictionary, then `flattenDictionaryStructure` | Offsets are renumbered only for uncacheable dictionaries, which are never filed |
| `get(k)` against `delete(k)`, then `set(k, v2)` | Entries are never reused in a live table. `set` appends a new entry with a slot or tag of its own. The reader returns the old value (ordered before the delete), absent (between the two), or `v2` |
| In-image tables: `m_successor`, `m_status`, `wasWalked` would be writes to read-only memory | The header of an in-image table is a heap cell that points at the image data (`aot/runtime/AOTCompactTableCreation.cpp:29-38`) |
| Narrow to wide columns under a reader | A new table. Widths are constants of a table that MS5 never rewrites |
| A value left in a deleted entry (MS4) is read while the marker skips it | The visitor visits all entries below `m_used`, dead ones too. The value is retained until the next rehash: bounded |
| Rope resolved by compare-and-swap against `swapToAtomString` on the same cell | Different expected words. Whichever is second fails and reads again |
| Substring rope whose base is atomized meanwhile | The base's old impl is parked, given F13; the width, given F14 |
| Wasm memory grown on the main thread while a spawned thread reads a view | The grow detaches the old buffer: TA1 and TA2. In signaling mode the base does not move |
| Length-tracking view against shrink | The length is read from the buffer's word in the same window; the memory stays mapped (TA1), given F23 |
| Oversize to Wasteful under a reader that holds the old mode | Vector and length do not change (TA5) |
| A closure made on one thread from that thread's `FunctionExecutable`, called on another | The callee's index selects the caller's own vector; a miss links. Identity: F18 |
| An error made on one thread, `stack` read on another, after the first has ended | `StackFrame` holds the CodeBlock strongly and uses only its immutable half. `m_thread` of such a block must not be followed: worth an assertion |
| `m_regExpAllocatorLock` held by a matcher that parks at its poll | The allocator is per thread (6.9), so the lock is gone |
| A `serial` native is re-entered through a callback | The lock is released around calls of JavaScript. What the native sees is what same-thread re-entrancy shows it today |
| Ping-pong between `bun-aot` and `bun-aot-mt` | Both compute `imports_threads` by the same code on the same input, given F25 |
