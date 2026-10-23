# 6. Arrays, ordinary objects, collections, strings, typed arrays

Everything in a compiled program that is not an object with a layout: arrays, `JSFinalObject`s (dictionaries made by `NewDictionary`, extra-properties objects, untyped `JSON.parse` results, what native and interpreted code make), function objects' properties, `Map` and `Set`, strings, typed arrays, RegExp.
The present state, instruction by instruction: `research/R7-ordinary-objects-interpreter.md` (`R7 2.4` and so on).

This is where oven-sh/WebKit#249 spent most of its effort (`research/R9-prior-threads-design.md`, section 5.1, problems P1 to P14). The problems are the object model's and stay. What is new is that only three kinds of code touch these objects (emitters and stubs that are generated per image, the interpreter's handlers for compiled processes, and C++), that all three are compiled for
threads or not at all, and that a thread holds a pointer to storage only inside a window.

## 6.1 The five problems

1. **Growth is copy, then swing.** A store by another thread into the old copy vanishes. 02 forbids that even for programs without races: `a[0] = x` and `a.push(y)` do not conflict.
2. **The meaning of storage is in another word.** Element kind and StructureID are in the cell, contents behind a pointer. A reader can pair an old meaning with new contents.
3. **Old storage must stay readable**, and what it points to must stay alive, as long as somebody can read it.
4. **Slots are reused** after a delete, and can change kind (data, accessor).
5. **Structures have mutable state of their own** that several objects share.

## 6.2 The storage word

In mt builds `sizeof(IndexingHeader)` is 16, and every butterfly has the header, whether or not it has elements.

```
            b-16                                                      b-8                b-4               b
 .. out-of-line properties | storage word                            | publicLength (32) | vectorLength (32) | element 0 ..
                             bits  0..31  elementsStartOffset
                             bits 32..39  kind      None Int32 Double Contiguous ArrayStorage SlowPutArrayStorage Undecided Restricted
                             bits 40..47  state     Confined Owned Shared Retired ReadOnly      (numbered from 1)
                             bits 48..63  thread    (Owned and Confined)
```

- Out-of-line property `i` lies one word lower than today: `offsetInButterfly()` (`runtime/PropertyOffset.h`) subtracts 3 instead of 2. A compile-time constant.
- A butterfly with elements is 8 bytes larger than today. A butterfly that has only out-of-line properties has no header at all today (`Butterfly::totalSize(.., hasIndexingHeader, ..)`): it is 16 bytes larger.
- An array that is born as one cell (`aot/runtime/AOTArrayAllocationProfile.h:59-64`) is cell (16), storage word (8), lengths (8), elements.
- A frame array (`aot/runtime/AOTFrameArrays.h:18-38`; `sizeOfFrameArray`, `aot/stubs/AOTStubsArrays.h:67-69`) has the word too, state `Confined`: the six operations that may be given one read it like any storage.
- The word of `elementsStartOffset` (`runtime/IndexingHeader.h:117-120`), which today exists only for Structures with `hasElementsStartOffset`, is the low half. It is 0 in storage that was never `Confined`.
- Copy-on-write storage is always a `JSCellButterfly` in the heap, or its twin in the image. It has the word, state `ReadOnly`. The image writer emits it.
- The states are numbered from 1. A state byte of 0 means that some place made a butterfly and did not write the word (SW13). `storageForWriting`, the stub of 6.3.2 and the collector's visitor end the process on it (release assertion). So a forgotten place is found at once, and is not read as a state that permits something.
- The upper 32 bits are the **storage tag**. The private part of a thread's view holds `ownedStorageTag[kind]` = `kind | Owned << 8 | thread << 16`, one word per kind, constant for the thread.

| State | Meaning | Who may write elements, lengths and out-of-line slots | Who may replace or rearrange the storage |
| --- | --- | --- | --- |
| `Confined` | the allocation site is proven Confined (04, section 4.2.3) | the one thread that can reach it, without a test | the same, by today's code, in place if it likes |
| `Owned(t)` | made by `t`. No other thread has written it. **Other threads may be reading it** | `t`. Another thread first makes it `Shared` | `t`, by retiring it; a safepoint operation |
| `Shared` | more than one thread has written it | every thread | a safepoint operation |
| `Retired` | it has been replaced, or will be within a few instructions | nobody | nobody. It dies at a later collection |
| `ReadOnly` | copy-on-write, frozen in place | nobody | copy-on-write: whoever writes first gives the array a copy. Frozen: nobody writes; the array gets other storage once, if the frozen form has to change (6.3.8) |

### Invariants

They bind storage that is not `Confined`, unless they say otherwise.

| # | Invariant |
| --- | --- |
| **SW1** | `kind` is written once, before the storage is published. A change of kind makes new storage. (`Confined` storage converts in place. It stores its kind byte after the last converted slot and a store-store fence, and before the Structure.) |
| **SW2** | `vectorLength` is written once, before the storage is published, and is all that the allocation's size class holds. **Every slot below it holds a hole when the storage is published.** There is no growth in place, and no room is given back in place |
| **SW3'** | `publicLength <= vectorLength` at all times. In `Owned` storage the slots from `publicLength` up hold holes, but for the moment inside an operation of the owner. In `Shared` storage such a slot holds a hole **or a value that some thread stored there** |
| **SW4** | Every slot below `vectorLength` holds at all times a value of the storage's kind or that kind's hole |
| **SW5** | State changes: `Owned(t) -> Shared` by any thread, `Owned(t) -> Retired` by `t`, both by compare-and-swap on the whole storage word; `Shared -> Retired` and `Owned(t) -> Retired` in a safepoint operation, by a plain store: at a stop no thread is between a test of the word and the store that the test allows. Nothing else. `Confined`, `ReadOnly`, `Retired` are final |
| **SW6** | The header words are never overwritten by anything else, and no header is written over former elements or properties. This includes `ArrayStorage`: `Butterfly::shift` and `unshift` (`runtime/ButterflyInlines.h:285-316`), which move properties, header and `ArrayStorage` fields inside one allocation, are used only on `Confined` storage. Elsewhere `m_indexBias` is 0 for the whole life of the storage |
| **SW7** | The pointer in the cell is replaced by a release store, after the new storage is complete. Then the publishing thread runs the owner barrier on the cell, before its next safepoint |
| **SW8** | Replaced storage is garbage-collected memory, never freed eagerly. `realloc` of large butterflies (`runtime/ButterflyInlines.h:237-246`, `runtime/JSArray.cpp:68`) only for `Confined` |
| **SW9** (= S1) | A thread uses a loaded storage pointer only inside the window in which it loaded it |
| **SW10** | The collector visits all `vectorLength` slots of Contiguous storage, not only `publicLength` (`runtime/JSObjectVisitInlines.h:103`). In `Shared` storage the length is **raised** only by a compare-and-swap loop that computes the maximum |
| **SW11** | The collector's visitor loads StructureID, `maxOffset`, pointer, then StructureID and `maxOffset` again, as today (`runtime/JSObjectVisitInlines.h:126-200, 347-380`), then the storage word. It goes on only if the word's kind is the kind of the Structure's indexing type (`Restricted`: the Structure is restricted), and the state is `ReadOnly` exactly if that type is copy-on-write. Otherwise it gives up (`didRace`), and the cell is visited again at the next stop. The pre-capacity is the word's low half |
| **SW12** | The first storage of an object, and every replacement together with which the Structure changes (kind, copy-on-write, out-of-line capacity), is installed **under the object lock, between the nuke of the StructureID and the store of the new one**. A dictionary keeps its StructureID when its out-of-line capacity grows: there the bracket is the nuke and the same ID again, and **`maxOffset` is stored inside it**, after the pointer (`runtime/JSObjectInlines.h:402-407`). That is what the visitor's second compare of `maxOffset` notices. A replacement that leaves the Structure alone (a longer or shorter vector) needs neither |
| **SW13** | Whoever allocates storage writes its state: `Owned(current thread)`; `Shared` if it replaces `Shared` storage (the one exception: `Thread.restrict`, 6.4) or if a closure allocates it (6.3.11); `ReadOnly` for a `JSCellButterfly`; `Confined` only (a) in code of a site that is proven Confined and none of whose arrays reaches an operand that also admits a Shareable site, and (b) in C++, for the successor of `Confined` storage of the same object |

**Why old storage is safe to read (problem 3).** By SW8 the memory lives until a collection. A collection needs every thread at a safepoint. By SW9 no thread holds the pointer there. What the old storage points to may then die with it. Inside the window the values it holds were reachable when the window began, and nothing is freed before the window ends.

**Why no store is lost (problem 1).** A thread stores only into storage whose state it has seen as `Owned(itself)`, `Shared` or `Confined`, in the same window.
- `Owned(t)`: only `t` stores. `t` copies and retires between two of its own operations. A thread that wants to become a writer must win a compare-and-swap on the same word that `t` retires by.
- `Shared`: replaced only while every thread is at a safepoint, so no store is between its test and its write.

**Why meaning and contents agree (problem 2).** Kind and bounds come from the storage that was loaded.

**Why a value above the length is harmless.** Two writers of `Shared` storage can leave one: `push` reads length 10, `a.length = 0` clears ten slots and stores 0, `push` stores its element at 10. By SW10 the value is kept alive. It can reappear when the length is raised past it: a value that some thread did store at that index (02, M3 and section 2.10.5).

**Why readers need no fence on ARM64.** Each slot is initialized before the storage is published (SW2), so the address dependency on the loaded pointer orders every later load behind the initialization. What a reader can miss is a later store: it then sees the hole, or the older value.

**The marker.** SW12 gives the marker exact pairs of Structure and storage, as the nuke protocol does today. SW11 is the second line of defence, and the first for `Confined` storage, whose conversion in place shows (old Structure, new kind byte) for a moment.

**Thread IDs are reused**, after the quarantine of 09, T1. Storage words are not rewritten when a thread ends. Harmless: the release and the allocation of an ID pass through `VM::m_threadTableLock` (09, section 9.2), so all stores of the old bearer happen before the first test of the new one, and the new bearer is then the only thread that passes the owner test, which is all that `Owned` claims.

**`Confined` beside the other states.** Code for a subject that is proven Confined makes no test and is right whatever the word says, since nobody else can reach the object. C++ and generic stubs look at the state and take today's path for `Confined`, the paths of 6.3 otherwise. Tested code that meets `Confined` storage (through C++, or after the allocating thread has ended) may write it: `AcquireStorage` answers `writable`. SW13 (a) keeps that off hot paths. Validating builds compare the thread field.

## 6.3 Arrays

"May be shared" as in 04. For a Confined or unpublished array every sequence of R7 2.4 stays. An array always has storage.

Notation: `a` the cell, `b` a loaded storage pointer, `w` its storage word, `me` the running thread, `hole` the hole of the kind. The column "Owned" means `Owned(me)`.

### 6.3.1 Reader

```
rd1  b = load64(a+8)                       // LoadStorage. Once. All later loads depend on b
rd2  cmp8  [b-12], kind   ; ne -> generic  // HasStorageKind. Not emitted where flow proves the kind
rd3  cmp32 i, [b-8]       ; ae -> miss     // IsBelowLength
rd4  v = load64(b+8i)                      // always 64 bits: the low halves of a hole and of the integer 0 are equal
rd5  v == hole            ; eq -> miss     // TryGetElement. Never dropped, whatever `Dense` says
```

`a.length`: rd1, `load32(b-8)` (`GetStorageLength`). For an array whose sites reach the operand use `Restricts` (04, section 4.2.3) rd2 stands between the two, and `GetStorageLength` relies on `StorageKind`: the length of a restricted array is refused like an element (6.4). Memory safety does not need the compare, since the length lies at `b-8` in every kind.

The same number of instructions as today's unhoisted read. The kind byte comes from the storage instead of the cell.

The array intrinsics of the built-ins written in JavaScript emit the same sequences, each in one operation (04, section 4.6.7): `HasContiguousIndex`, `GetContiguousIndex`, `GetDoubleIndex` are rd1 to rd5; `TryPutByValDirect`, `TryPutIndex` are wr1 to wr4 (6.3.2) and then the bounds test on the storage that may be written; `ArrayStorageKind` answers from the storage word of one loaded pointer and proves nothing for the access that follows. `PutContiguousIndex`, `PutDoubleIndex`, `GrowNewArray`, `TruncateNewArray` are for unpublished arrays only.

### 6.3.2 Writer: the right to write

```
wr1  b = load64(a+8)                                                      // LoadStorage
wr2  cmp32 [b-12], view.ownedStorageTag[kind]  ; eq -> column Owned       // MayWriteStorage [Owned]. Proves OwnedStorage, which implies WritableStorage
wr3  cmp32 [b-12], kind | Shared << 8          ; eq -> column Shared      // MayWriteStorage [Shared], on the failing edge of wr2. Proves WritableStorage
wr4  AcquireStorage(a, b, kind) -> writable(b'), miss                     // on the failing edge of wr3. writable proves WritableStorage and StorageKind of b'
```

The 32-bit compare replaces today's compare of the whole indexing byte. It tests kind, writability and owner at once.

`AcquireStorage` is a terminator of the IR (04, section 4.6.3): `reads writes(ArrayStorage)`, not `collects`. It yields a **new** `Storage` value. What was proven about the old one (`InsideStorage`, `BelowLength`) is not proven about the new one: the bounds tests come after it. Stub `acquireStorageForWriting(a, b, kind)`, C++ `object->storageForWriting(vm)`. `kind` is the kind that the caller's code is made for, or the value `any`, for the stores of 6.4, which do not look at the elements:

```
again:
  w = load64(b-16)
  kind != any, kind(w) != kind    -> miss
  Owned(me), Shared, Confined     -> writable(b)
  Owned(other)                    -> if CAS(b-16, w, w with state Shared and thread 0): writable(b)      // now a writer
                                     else again
  Retired                         -> up to 64 times { pause; b' = load64(a+8); if b' != b: b = b'; again }
                                     -> miss
  ReadOnly                        -> miss
```

The path behind `writable` uses the column Shared of 6.3.3: the edge does not prove `OwnedStorage`. The atomic forms of that column are right for every state.

`miss` leads to an operation that is declared `collects` and `waits` (04, S5: it may run a safepoint operation). It makes copy-on-write storage writable (6.3.8), converts (6.3.6), refuses (a frozen array, whose storage it leaves alone: 6.3.8; `Restricted`: 6.4), or, after `Retired`, calls `Thread::yield()` and starts again from wr1. C++ that holds an object lock yields and tries again in place.

**The wait on `Retired` is short.** The owner copies first and retires afterwards (6.3.5), so between the retirement and the publication lie a handful of instructions without an allocation or a call, inside one window. The retiring thread takes no lock there: it holds none (6.3.5), or it took the object lock before (6.3.6, 6.4), and then no waiter can hold that lock. So no cycle is possible. The only delay is an owner that the OS takes off its core exactly there; hence the yield.

### 6.3.3 Stores

| Operation | Owned | Shared |
| --- | --- | --- |
| `a[i] = v`, `i < publicLength` | `cmp32 i, [b-8]`; `store64(b+8i, v)`; barrier | the same |
| `a[i] = v`, `publicLength <= i < vectorLength` | `cmp32 i, [b-4]`; `store64(b+8i, v)`; `store32(b-8, i+1)` | `cmp32 i, [b-4]`; `store64(b+8i, v)`; `raise(i+1)` |
| `a[i] = v`, `i >= vectorLength` | growth (6.3.5), or `ArrayStorage` if sparse | 6.3.7 |
| `a.push(v)` | `len = load32(b-8)`; `len < load32(b-4)` else grow; `store64(b+8len, v)`; `store32(b-8, len+1)` | the same with `raise(len+1)` |
| `a.pop()` | `len`; 0: `undefined`; `v = load64(b+8(len-1))`; hole: miss; `store32(b-8, len-1)`; `store64(b+8(len-1), hole)` | `len`; `v`; hole: miss; `cas32(b-8, len, len-1)` else again; `store64(.., hole)` |
| `a.length = n`, `n < len` | `store32(b-8, n)`; holes from `n` to `len-1`. C++ may give room back: 6.3.5 | `store32(b-8, n)`; holes from `n` to `len-1` |
| `a.length = n`, `len < n <= vectorLength` | `store32(b-8, n)` | `raise(n)` |
| `a.shift()`, short | move each element with 64-bit accesses; `store32(b-8, len-1)`; hole in the last | C++, rule A4 |

```
raise(n):  do { len = load32(b-8); if (len >= n) break; } while (!cas32(b-8, len, n))        // RaiseStorageLength. Release order
```

| In the table | Operation of the IR (04, section 4.6.3) | Relies on |
| --- | --- | --- |
| `cmp32 i, [b-8]`; `cmp32 i, [b-4]` | `IsBelowLength`; `IsInsideStorage` | |
| `load32(b-8)` | `GetStorageLength` | |
| `store64(b+8i, v)`; barrier | `PutElement`, which names the array beside its storage and runs the owner barrier | `StorageKind`, `InsideStorage`, `WritableStorage` |
| `store32(b-8, n)` that raises the length (column Owned only) | `SetStorageLength [raises]` | `OwnedStorage` |
| `store32(b-8, n)` that lowers it | `SetStorageLength` | `WritableStorage` |
| `raise(n)` | `RaiseStorageLength` | `WritableStorage` |
| `cas32(b-8, len, len-1)` | terminator `TryLowerStorageLength` -> `lowered`, `changed` | `WritableStorage` |

- **One order in all emitters** (today two, R7 2.4): growing is element first, then length; shrinking is length first, then hole. So in `Owned` storage no reader on x86-64 sees a hole below the length that the program did not put there. On ARM64 it may; rd5 is there.
- All stores of elements are 64 bits wide. The 32-bit store of the low half (`aot/codegen/AOTElementAccessEmitterStores.cpp:195-206`) is right only while no other thread can turn the slot into something else.
- A store of a cell is a release store on ARM64 (04, F1).
- In `Owned` storage only the owner writes the lengths, so its plain stores lose nothing.
- Lowering the length of `Shared` storage is a plain store (in `pop` a compare-and-swap, so that two `pop`s by themselves take two elements). It races with every other writer of `length`: 02, section 2.10.5.
- Two threads that `push` at once may take the same index and lose one element. They race on `length`. Neither leaves a hole.
- An owner that has passed wr2 finishes its operation with plain stores, also if the storage becomes `Shared` in the middle. Its store of the length can then undo another thread's `raise`. That is a race on `length` like the others, and memory safe by SW3' and the first half of SW10.

### 6.3.4 What a racing pair can leave behind

| Pair on `Shared` storage | End state | Rule |
| --- | --- | --- |
| `a[5] = p` against `a[9] = q`, both above the length | length 10: the maximum | SW10 |
| `push` against `push` | one or two elements, length to match | |
| `push` against `pop`, `push` against `length = n` | the pushed value below or above the length, or wiped by the other's hole | SW3', SW10 |
| `pop` against `pop`, nothing else | two elements taken | compare-and-swap |
| `pop` against `pop` followed by `push` | the length can come back to the value that the first `pop` read, so its compare-and-swap succeeds: one element is returned twice, and the pushed one is wiped. All three race on `length` | 02, section 2.10.5 |
| any of them against a reader | a value that was stored at that index, or a hole | SW4 |

No pair leaves the allocation: every index is tested against `[b-8]` or `[b-4]` of the `b` that is written through, and `vectorLength` is constant (SW2).

### 6.3.5 Growth by the owner

Stub `growVector` (`aot/stubs/AOTStubHelpersStorage.cpp:202-242`); C++ `AOT::growFullVector`, `JSObject::ensureLengthSlow`.

```
gr1  new = allocate; all element slots = hole of the kind planned for; new.vectorLength = what the size class holds       // may be a safepoint
gr2  b = load64(a+8); w = load64(b-16)
    require tag(w) == ownedStorageTag[kind], vectorLength(b) < what is wanted <= vectorLength(new),
            out-of-line capacity of the Structure == the one that new was made for
    else start again from wr1
gr3  copy out-of-line slots, publicLength, elements below publicLength; new.word = w
gr4  CAS(b-16, w, w with state Retired)             // fails: the storage has become Shared. Drop new, go to 6.3.7
gr5  store-release(a+8, new)                        (SW7)
gr6  owner barrier on a
```

No safepoint between gr2 and gr5.

- **The copy is exact.** From gr2 to gr4 the word is `Owned(me)`, or gr4 fails. While it is `Owned(me)` nobody but this thread may store, and this thread is copying.
- In the stub gr1 is the inline pop from the thread's own free list, which is no safepoint. If it needs the slow path, or if the collector will not visit the array again (`:210-211`), the stub leaves for C++.
- A copy of more than 1 MB polls every 64 KB (08, section 8.5). After a poll that parked it starts again at gr2.
- The Structure does not change, so there is no lock and no nuke (SW12).
- **Two growers** cannot both succeed: one compare-and-swap.
- **The hole is that of `kind(w)`**, which gr2 compares with the kind that gr1 filled for. The stub takes it from the tag that it tests, not from the indexing byte of the cell (`loadHoleOf`, `aot/stubs/AOTStubHelpersStorage.cpp:190-200`).
- **Shrinking.** `JSArray::setLength` gives room back when more than 64 elements and more than half of the array are dropped (`runtime/JSArray.cpp:1454-1459`; `reallocateAndShrinkButterfly`, `runtime/JSObject.cpp:4195-4216`). For storage that is not `Confined` that is this same sequence, by the owner, with the second requirement of gr2 turned round: `new length <= vectorLength(new) < vectorLength(b)`. gr3 copies the elements below the new length, and `new.publicLength` is the new length. `vectorLength(new)` is what the size class holds (SW2). The out-of-line capacity stays.
- **Storage that is not `Owned(me)` is never replaced by a shorter one**, and if gr4 fails the shorter storage is dropped: the length is lowered in place (6.3.3), the slots get holes, and the room is given back at the next replacement.
- So `a[i] = x` with `i` below the new length is not lost against `a.length = n` (02, section 2.10.5). Its writer has made the storage `Shared` before gr4, which then fails, or it finds `Retired` and stores into the successor.

### 6.3.6 Conversion between kinds by the owner (R7 2.5)

| Conversion | Today | Storage that is not `Confined` |
| --- | --- | --- |
| Undecided to any | in place | new storage |
| Int32 to Contiguous | in place, contents untouched | **new storage** (SW1). A copy of 8 bytes per element, once in the life of an array |
| Int32 to Double, Double to Contiguous | **in place**, every slot rewritten | new storage |
| Contiguous to Double, any to ArrayStorage, ArrayStorage to Contiguous | new storage already | the same procedure |

```
cv1  prepare: S = StructureID(a); S' = the Structure with the new indexing type; new = allocate, holes        // may be safepoints
cv2  b = load64(a+8); w = load64(b-16); require Owned(me), the kind planned from, room in new; else start again
cv3  fill new from b in the new kind; new.word = Owned(me) | new kind
cv4  lock a
cv5  require StructureID(a) == S and load64(a+8) == b; else unlock, start again
cv6  CAS(b-16, w, w with state Retired)             // fails: unlock, 6.3.7
cv7  nuke StructureID; store-release(a+8, new); store-release StructureID = S' (indexing byte and flags before it)
cv8  unlock; owner barrier
```

No safepoint between cv2 and cv8. (Waiting for an object lock is none: 05, L5.)

Byte 4 of the cell holds the indexing type **and the two bits of the object lock**. cv7, cl4, cw3, mf3 and fs3 change it while the lock is held, and a waiter sets the parked bit at any moment. So the indexing bits are changed by a compare-and-swap loop that keeps the lock bits (`runtime/JSCellInlines.h:273-282`), never by a store of the byte: that would lose the parked bit, and with it the wake-up.

The indexing byte and the Structure of the cell are hints for C++ dispatch. Compiled code and stubs do not read them for arrays that may be shared. C++: 6.3.12.

**Double arrays stay.** #249 never solved this ("Int32 to Double never happens", 2.9 % of cycles, navier-stokes 82 instructions per iteration against 31). Where flow proves the contents (`ArrayStorageKinds`, `ProvenContents`) no conversion can happen at all, and rd2 is not emitted. The state test on stores and the hole test on loads stay.

### 6.3.7 Replacing storage that is `Shared`, or that another thread owns

Storage in state `Owned(other)` is first made `Shared` (`AcquireStorage`, which changes nothing if the state is `Owned(me)`: the owner has 6.3.5 and 6.3.6). Then, for growth, conversion, freezing, and growth of the out-of-line capacity alike:

```
rp1  prepare, at a safepoint, holding no leaf lock and no StorageRef:
      read b, w, kind, vectorLength, S = StructureID(a)
      new = allocate for what is wanted, holes; new.word = Shared | new kind; S' if the Structure changes
rp2  ok = heap.runSafepointOperation(closure)
rp3  !ok: start again from the generic dispatch

closure (08, section 8.6):
cl1  b = load64(a+8); w = load64(b-16)
    validate: state(w) == Shared; kind(w), vectorLength(b) and StructureID(a) are those of rp1
    anything differs -> return false
cl2  copy out-of-line slots, publicLength, elements below publicLength (converted); the slots above stay holes
cl3  store64(b-16, w with state Retired)
cl4  if S':  nuke; store-release(a+8, new); StructureID = S'.      else:  store-release(a+8, new)
cl5  owner barrier on a, on the mark stack of the thread that runs the closure
    return true
```

- The closure validates **everything it was prepared from**. Two requests for one object can run in one stop: one thread grows the vector, another the out-of-line capacity, and both live in one butterfly. The second finds another `vectorLength` or StructureID and returns false. Freezing and `Thread.restrict` change the StructureID too. The validation is by contents, not by the pointer's identity, so storage that was freed and allocated again at the same address while the requester waited cannot pass for the old one.
- It allocates nothing, names no "current thread" (the new word is `Shared`), and may be run by another JavaScript thread than its requester.
- It runs beside markers: nuke, fences and barrier stay (08, section 8.6, point 4).
- `new` is rooted by the requester's parked stack.
- Values above `publicLength` are not copied: at a stop no `push` is under way, so they are leftovers.

An array that several threads write grows a logarithmic number of times. At 0.04 ms per running thread and stop (#249's measurement) an array that reaches a million elements costs about 20 stops.

### 6.3.8 Copy-on-write, frozen in place

Writers of `ReadOnly` storage leave `AcquireStorage` on `miss`. `convertFromCopyOnWrite`:

```
cw1  prepare: S = StructureID(a); S' = the writable Structure; new = allocate, holes                  // may be safepoints
     b = load64(a+8); require ReadOnly and room in new, else start again; copy from b; new.word = Owned(me) | kind
cw2  lock a; require StructureID(a) == S and load64(a+8) == b, else unlock, drop new, start again on what is there
cw3  nuke; store-release(a+8, new); StructureID = S'
cw4  unlock; owner barrier
```

No safepoint between the load of `b` and cw4.

The read-only storage is not retired: other arrays of the same literal point at it. Two writers: the lock decides. The loser starts again on the winner's storage, which it finds `Owned(other)`.

A named property is never added to `ReadOnly` storage (a `JSCellButterfly` has no room in front): the array is made writable first.

`tryFreezeArrayInPlace` is a replacement whose new storage is a `JSCellButterfly` in state `ReadOnly` and whose Structure changes: 6.3.6 for the owner, 6.3.7 otherwise. An array that is copy-on-write already keeps its storage and changes the Structure under the lock.

**A frozen array is never thawed.** For compiled code and stubs the only thing that says "frozen" is the state `ReadOnly`. So from the moment it is frozen an array has, for every other thread, a frozen Structure, and storage that no compiled store can pass: `ReadOnly`, or of kind `ArrayStorage`.

`moveFrozenElementsToSparseMap` (`runtime/JSObjectFrozenElements.cpp:17-47`) gives the array the ordinary frozen form. Today it goes back to the copy-on-write Structure, makes the array writable, converts it, and freezes it again: four published steps. In mt builds it is **one replacement**:

```
mf1  prepare: S = StructureID(a)
              S' = the Structure in which today's four steps end, found by running the transitions on Structures, not on the array
              new = allocate, kind ArrayStorage; map = a sparse map                                    // may be safepoints
     b = load64(a+8); require ReadOnly, else start again
     enter each element of b into map, read-only and not configurable; new.word = Owned(me) | ArrayStorage
mf2  lock a; require StructureID(a) == S and load64(a+8) == b, else unlock, drop new and map, start again on what is there
mf3  nuke; store-release(a+8, new); StructureID = S'
mf4  unlock; owner barrier; invalidations (07, section 7.5)
```

No safepoint between the load of `b` and mf4. The source is `ReadOnly`, so no stop is needed, and it is not retired. Nothing undoes the move. Two movers: the lock decides, and the loser finds nothing left to do.

| Who asks for the move today | mt builds |
| --- | --- |
| the array becomes a prototype (`runtime/StructureCreateInlines.h:69-72`) | moves, before the prototype-chain lock is taken (05, section 5.7): the elements of a prototype must be able to intercept |
| the array takes a private name (`aot/runtime/AOTPrivateNameOperations.cpp:179-180`) | moves: a `JSCellButterfly` has no room in front |
| `Thread.restrict` (6.4) | moves |
| `JSArray::defineOwnProperty` (`runtime/JSArray.cpp:228-229`), `JSArray::pop` (`:1491-1492`), `JSObject::putInlineSlow` (`runtime/JSObject.cpp:621-622`), `setPrototypeWithCycleCheck` (`:2015-2016`) | **does not move.** The attempt is refused, or it does not touch the elements. The generic algorithm asks the array for the attributes of its own properties and for its extensibility, and the form that is frozen in place answers both: read-only and not configurable; not extensible |

### 6.3.9 First storage of an object

An object other than an array can have none. The first out-of-line property, the first element (`o[0] = 1`, `createInitialIndexedStorage`, `runtime/JSObject.cpp:1068`), and the butterfly that a typed array gets with its `ArrayBuffer` (6.8) all come with a change of the Structure, or are paired with it:

```
fs1  prepare: S; S'; new = allocate, holes; new.word = Owned(me)
fs2  lock o; require StructureID(o) == S and load64(o+8) == 0, else unlock, start again
fs3  nuke; store-release(o+8, new); store the value; StructureID = S'
fs4  unlock; owner barrier
```

### 6.3.10 Moving the start of the elements (R7 2.7)

`tryMoveElementsStartForward` writes a new header over what were elements, in the same allocation. A thread that holds the old pointer then reads lengths as a value: type confusion. It breaks SW6.

- `Confined` storage: as today.
- Otherwise not used. `shift` and `unshift` move the elements, in any kind.

This is a known cliff for a long queue that is shared between threads and drained with `shift()`: linear time per call. Two ways out, neither in the first version: encode the header words so that each is a non-cell value whatever a stale reader makes of it (bias `vectorLength` by 2^29, so that the word reads as a number); or a `Queue` class in `bun:threads`.

### 6.3.11 ArrayStorage, sparse maps, `havingABadTime`

- Every access to the elements is C++ and takes the object lock, reads included (a rehash of the sparse map frees the table under a reader). Compiled readers leave at rd2.
- The storage follows 6.2 like any other. Named properties in front of it are read without a lock (N1), and every compiled access begins with three loads through `b`, so its header and its properties never move (SW6). `shiftCountWithArrayStorage` and `unshiftCountWithArrayStorage` (`runtime/JSArray.cpp:1834-1841, 1998-2008`) move elements inside `m_vector`, or replace the storage.
- A writer passes the state test under the lock. Replacement: 6.3.6, with the lock also where the Structure stays, or 6.3.7.
- The visitor takes the cell lock for this kind, as today. By L1 it never finds it held by a parked thread.
- `haveABadTime(vm)` can happen only in realms of `node:vm` and `ShadowRealm`: the built-in prototypes of the main realm are locked.
- `haveABadTime(vm, interceptor)` (`runtime/JSGlobalObject.cpp:3139-3174`) is made for the main realm of a compiled process. It happens once per object that begins to intercept indexed accesses: a prototype that gets an indexed accessor, a chain that gets a `Proxy` (`runtime/JSObject.cpp:1065, 1988-1991`). `Object.defineProperty(MyList.prototype, 0, { set })` is enough.
- Both walk the heap and convert arrays of any thread (`switchToSlowPutArrayStorage`). Each is **one safepoint operation, walk and conversions together**, and the one kind of closure that allocates (08, section 8.6, point 8). Per array it does cl2 to cl5, also where today only the Structure changes (`ArrayStorage` to `SlowPutArrayStorage`: SW1). It takes a source in any state: it retires `Owned` and `Shared`, and leaves `ReadOnly` and `Confined` as they are. The new storage is `Shared`; the successor of `Confined` storage is `Confined` (SW13 (b)). An array that is frozen in place is passed over, as today (`runtime/JSObject.cpp:1905-1908`): nothing is stored into it.
- The caller holds nothing. Where a change of a prototype is the cause, the operation runs after the prototype-chain lock and the object lock are released (6.4; 05, section 5.7).

### 6.3.12 C++

About 140 places in the run time decode lanes from `indexingType()` and `butterfly()` (#249's count). The rule for each:

| # | Rule |
| --- | --- |
| **A1** | Load the butterfly once per window (`StorageRef<Butterfly> b = object->storage()`), take the kind from `b->storageKind()`, the bounds from `b`. Never `indexingType()` of the cell followed by `butterfly()`. `Restricted` is a kind that no fast path accepts, also one that reads nothing but the length |
| **A2** | A store is preceded, in the same window, by `b = object->storageForWriting(vm)`, which is wr2 to wr4. Changes of the length follow the column of 6.3.3 that the state names |
| **A3** | After anything that may be a safepoint (allocation, a call of JavaScript, a lock that may block) load again, and test again. An exception check is none (08, section 8.4). A loop that calls compiled code once per element (`sort` with a comparator, the C++ forms of `forEach`) calls `heap.stopIfNecessary()` once per 1,024 calls (08, section 8.5), which is one |
| **A4** | A function that must see the array whole (`reverse`, `splice`, `fill`, `copyWithin`, `shift`, `indexOf` with vector instructions) works on the loaded storage with 64-bit accesses and bounds from that storage. It may see other threads' stores half way; 02 allows that. It never trusts a length that it read earlier against a storage that it loaded later. **An algorithm whose memory safety depends on reading the same value twice works on a private copy**: `sort` copies out, sorts, and stores back inside the bounds of a freshly loaded storage |
| **A5** | Fast paths that cannot be written so are compiled out in mt builds and the generic path is taken |
| **A6** | A pointer to replaceable storage is held in `StorageRef<T>`; one that must outlive a safepoint, in `RevalidatedRef` (08, S1'). `park()`, `releaseAccess()` and `runSafepointOperation()` assert in debug and validating builds that no `StorageRef` is live |

`Butterfly::contiguous()`, `contiguousDouble()`, `contiguousInt32()` assert the kind in debug builds.

A known offender against A3: `runtime/ArrayPrototype.cpp:1612-1668` loads both butterflies, lengths and indexing types, allocates, then copies with the old values.

### 6.3.13 Heuristics

`ArrayAllocationProfile`, "last array", `ArrayWords` counters, `placeThatConverts` (a parameter passed through memory): private part of the view (07). `lastArraySize` (`runtime/JSObject.cpp:78`): one plain word; a lost update costs a guess.

## 6.4 Named properties of ordinary objects

Inline slots need nothing: they lie in the cell and never move. Out-of-line slots lie in the butterfly and follow 6.2: reads through one loaded pointer, stores after the state test (kind is not compared: mask it off).

### Adding a property (R7 3.2, 3.3)

Under the object lock (05, section 5.3), unless the object is unpublished (compiled code knows; C++ builders of new objects use `putDirectUnpublished`).

```
prepare  S = StructureID; newStructure (6.5)                                                           // may be safepoints
         no storage yet, and one is needed:                6.3.9
         storage is ReadOnly:                              6.3.8 first, then start again
         capacity grows:                                   new = allocate, holes                       // may be a safepoint
                                                           b = load64(o+8); w = load64(b-16)           // no safepoint from here to unlock
           w is Owned(me):                                 copy everything from b (as gr3)
           w is Shared or Owned(other):                    forget b. The whole mutation is the closure of 6.3.7, with steps 2 and 3 inside cl4
lock     require StructureID == S, and load64(o+8) == b if b was copied; else unlock and start again
1        capacity grows:  CAS(b-16, w, w with state Retired), else unlock and start again; nuke; store-release(o+8, new)
                          dictionary: maxOffset (SW12)
2        out-of-line slot:  b = storageForWriting (wr2 to wr4, kind `any`)
         store the value at its offset (release for a cell on ARM64)
3        store-release StructureID; flags and type bytes before it if they change
unlock
4        owner barrier; invalidations (07, section 7.5)
```

A thread that holds an object lock does not park (L1), so a replacement that needs a stop is decided in `prepare`, before the lock and before the first store that a lock-free reader can see (05, L6). For the side object of a compiled object the mutation that becomes the closure is the whole mutation of the outer object (05, section 5.6).

The owner of the storage may grow the **elements** of the same butterfly at the same time, without the lock (6.3.5). The storage word arbitrates: the adder's step 2 either makes the storage `Shared` first, and the owner's gr4 fails, or it finds `Retired`, waits the few instructions, and goes on in the successor.

**Rule N1 (readers).** load-acquire StructureID; find the offset (per-thread cache, or the Structure's table); load the butterfly; load the slot. All in one window. What the reader may pair:

| StructureID | Storage | Result |
| --- | --- | --- |
| old | new | fine: the out-of-line capacity only grows, offsets keep their meaning (N2, N2', N3). A shorter successor (6.3.5) is shorter in its elements only. Flattening makes the capacity smaller while no reader is in a window |
| new | new | fine by release and acquire |
| new | old | impossible: the storage is published before the StructureID |

**A nuked StructureID.** The nuke during a replacement is unconditional (today: only on x86 or while marking), and it happens on objects that other threads read. Stubs compare IDs, miss, and go the slow way. In C++ `JSCell::structureID()` and `structure()` return the **decontaminated** ID in mt builds (one `and`): by the table an old StructureID with either storage is a legal pair, so a reader needs no slow path. Only a writer that plans a transition, and the collector's visitor, look at the raw word.

### Deleting; changing the kind of a property

| # | Rule |
| --- | --- |
| **N2 quarantine** (dictionaries) | The offset of a property deleted from a dictionary is not given out again before the end of the next collection. `PropertyTable` keeps two lists, quarantined and free. Tables with a non-empty quarantine are registered in a list of the heap; the end of a collection, with all threads parked, walks it and moves one list into the other. Before the table is edited, the slot of a *data* property gets `undefined` (release). The slot of an accessor or custom-value property gets the VM's immortal `emptyGetterSetter`, whose getter is null and whose setter is the null setter: a reader with an old answer reads `undefined`, and a writer does what a store to an accessor without a setter does. So a reader with an old StructureID or an old table entry gets the old value or `undefined`, never a later property's value (02, M7), and a slot that has held a `GetterSetter` holds one until its offset leaves quarantine |
| **N2' no reuse** (all other Structures) | An offset is a function of the Structure: deletion is a cached transition (`runtime/Structure.cpp:960-977`), and the offset that a later addition takes is part of the Structure it leads to (`:577-593`). No list can delay its reuse. In mt builds an addition transition out of a Structure that is not a dictionary **never takes a deleted offset**: `nextOffset()` (`runtime/PropertyTable.h:484-490`) answers `offsetForPropertyNumber(size() + number of deleted offsets)`. The slot gets `undefined` or `emptyGetterSetter` as in N2, and stays a hole for the life of the object, or until the object is flattened in a safepoint operation |
| **N3 fresh offset** | A redefinition that changes what the slot holds (data to accessor, accessor to data, custom) takes a **new offset**. The old slot keeps its last value and is treated as deleted (N2 or N2'). So a reader that has learnt "data at offset k" never loads a `GetterSetter` as a value. (Open defect O17-1 of #249.) The key's position in the table's order does not change |

An object that adds and deletes in turn grows by a slot per round, until the chain reaches its maximal length and the object becomes a dictionary, where N2 recycles.

### Dictionaries (R7 3.5)

The Structure belongs to one object and is edited in place.

- Writers: object lock, then `Structure::m_lock`.
- **Storage first.** Today the table entry is inserted before the butterfly grows (`runtime/StructureInlines.h:437-443`), so a lock-free reader can take a new offset to the old, smaller storage. In mt builds: grow (nuke, pointer, `maxOffset`, the same ID again: SW12), store the value, then insert the entry (release). Where the capacity stays, `maxOffset` is stored after the value and before the entry. The reader's side is PT8.
- Readers are lock-free over the table protocol of 6.5.
- **Flattening** (`flattenDictionaryStructure`) renumbers offsets and moves values under one StructureID. It is a safepoint operation:

```
fl1  prepare, at a safepoint, as rp1:
      read S = StructureID(o), n = the number of properties, hence the out-of-line capacity afterwards; b, w, kind, vectorLength
      capacity gets smaller, and o keeps storage (it has elements or is an array, or properties stay out of line):
        new = allocate for that capacity and vectorLength, holes
        new.word = kind | Owned(requester) if w is that; Confined if w is (SW13 (b)); else Shared
fl2  ok = heap.runSafepointOperation(closure);  !ok: start again, or do without

closure:
fl3  validate: StructureID(o) == S; S is still a dictionary with n properties; state, thread, kind and vectorLength of the storage are those of fl1
     anything differs -> return false
fl4  cell lock, Structure::m_lock, nuke                                   // against the marker, as today (runtime/Structure.cpp:1799-1825)
fl5  renumber the table; move the values: into the cell, and into new, or into b if the capacity stays
     empty the table's two lists of offsets, quarantined and free, and take the table off the heap's list (N2)
fl6  capacity gets smaller: copy publicLength and the elements below it into new; retire b if it is Owned or Shared;
                            store-release(o+8, new), or 0 if o keeps no storage
     maxOffset; the Structure is no dictionary any more; StructureID = S; unlock both
fl7  owner barrier on o, on the mark stack of the thread that runs the closure
     return true
```

- The closure allocates nothing and opens no `DeferGC` scope (08, section 8.6, points 7 and 8). The smaller butterfly, which `shiftButterflyAfterFlattening` (`runtime/JSObject.cpp:4464-4494`) makes in the middle today, is `new`: its size follows from `n`, which fl3 validates.
- `ReadOnly` storage has no properties in front, so its capacity is 0 before and after, and it stays.
- **The quarantine may be dropped.** It protects a reader that took an offset from the table before a deletion. N1 keeps StructureID, offset and slot in one window, and at a stop no window is open. No cache holds such an offset: only uncacheable dictionaries have deleted anything and are renumbered, and those are never filed in a cache. If the table stayed on the heap's list, the end of the next collection would hand out offsets that are in use again.
- Afterwards the Structure is an ordinary one, and N2' holds for it.
- Callers that flatten as an optimization (filing in property caches, enumerator creation) do so at most once per object, and do without if the closure returns false.

### Lazy static properties (R7 3.7)

A read of a `HasStaticPropertyTable` object puts the property into the object: an add. The function, accessor or constant is made outside the lock (that may allocate, pull a lazy class, or run a `PropertyCallback`). Under the lock the offset is looked up again, and the loser drops what it made. `AOT::RuntimeHooks::didCreateStaticProperty` is called by the winner only. Members that the image names are made before the first thread starts (`Instance::prepareForThreads()`, 07).

`vm.allowLazyMaterializationOfImmutablePropertiesCount`, a per-VM counter used as a dynamic scope, moves to `ThreadContext`: while one thread is inside such a scope other threads' writes to immutable properties must still be refused. `m_deletePropertyMode` likewise.

### `Atomics` on a property

Every `Atomics` operation on a property of an ordinary object, reading forms included, holds the object lock from the lookup to the store (04, section 4.6.6, last row of "One protocol per slot"). No inline form reaches such a property, so the lock is taken by all of them, and each is one step against the others (02, section 2.5). Under the lock: find the offset; load the slot; compute; `storageForWriting` for a slot out of line; store (sequentially consistent). A `store` that has to create the property is the addition above, whose `prepare` comes before the lock. A plain access to the same property takes no lock: the program has a race then, with the outcomes of any store against a read.

### Changing the prototype

As 05, section 5.7, for every object, with a layout or without: a chain passes through both sorts, so one lock covers both. `JSObject::setPrototypeWithCycleCheck` and `setPrototypeDirect` (`runtime/JSObject.cpp:1956-2062`):

```
prepare  the Structures that the change and didBecomePrototype need
         the new prototype is an array that is frozen in place: 6.3.8, mf1 to mf4
lock     VM::prototypeChainLock
1        the cycle walk; didBecomePrototype on the new prototype (under that object's lock)
2        lock o; the new StructureID, or the poly-proto slot (release); unlock o
unlock   VM::prototypeChainLock
3        owner barrier; invalidations (07, section 7.5)
4        holding nothing: haveABadTime(vm, o) or switchToSlowPutArrayStorage, if the new chain intercepts indexed accesses (6.3.11; 6.3.6, 6.3.7)
```

Of two racing `setPrototypeOf(a, b)` and `setPrototypeOf(b, a)` exactly one succeeds, and no chain gets a cycle (02, M8). Both locks are leaf locks (08, H15), and step 4 may need a stop: hence its place. Until it is done an indexed store into a hole can pass by a setter of the new chain. Such a store reads the link that step 2 writes, so the program has a race there.

### `Thread.restrict` on an array or an ordinary object

Fast paths test nothing that could refuse a thread (05, section 5.11, for objects with a layout). So a restricted object is one on which **every fast path misses**. `Thread.restrict(o)` is a safepoint operation, prepared as rp1 and validated as cl1 (kind, `vectorLength`, StructureID, and the state that was read):

1. `o` gets a fresh Structure that is restricted: it names the owner, lies in no transition tree, prohibits caching, and has the type flags that send every generic operation through its class's hooks. It keeps the properties, their attributes and the extensibility of the Structure that it replaces.
2. Its storage, if any, is replaced by a copy whose kind byte is `Restricted`, a value that no emitted compare names, and whose state is `Owned(owner)`. The owner's ID is captured by value (08, section 8.6, point 5). The elements' real kind is the Structure's indexing type.

No state is changed beforehand: at the stop nobody is between a test and a store. What the closure does with the source:

| State of the source | The closure |
| --- | --- |
| no storage | changes the Structure only |
| `Owned(t)`, whoever `t` is; `Shared` | copies, retires the source (SW5) |
| `Confined` | copies, leaves the source as it is. Nobody else can hold it |
| `ReadOnly`, copy-on-write | copies, leaves the source as it is: other arrays of the literal point at it. The restricted Structure has the writable indexing type |
| `ReadOnly`, frozen in place | does not occur: `Thread.restrict` first gives the array the ordinary frozen form (6.3.8, mf1 to mf4), whose storage is `ArrayStorage`, and prepares from that |
| `Retired` | does not occur: between a retirement and the publication of the successor lies no safepoint |

After the stop a named access misses on the StructureID, an indexed read and a read of `length` at rd2, an indexed store at wr2 to wr4 (the kind differs). The generic path tests the owner first and throws `ConcurrentAccessError` elsewhere. The owner pays the generic path too; it grows and converts its storage by 6.3.5 and 6.3.6, without a stop, and the successors keep the kind byte `Restricted`. A site that reaches the operand use `Restricts` never counts as having a proven kind, so rd2 is emitted for it, also in front of `a.length` (6.3.1).

**The restricted Structure is a carrier of a thread's ID** (09, T1): its visitor replaces a quarantined owner by `NoThread`. After the owner has ended the object is usable by nobody, and never by a later thread with the same ID (02, section 2.7). The thread field of the storage word is not rewritten. It decides nothing here: every access passes the test against the Structure's owner first.

## 6.5 Structures

A Structure is shared by all objects of a shape, so the object lock does not cover it.

| State (R7 3.4) | Rule |
| --- | --- |
| Transition table | `Structure::m_lock` for writers **and for mutator readers**: the `..Concurrently` variants that exist for compiler threads (`runtime/StructureInlines.h:579-583`). In front of it the per-thread property caches answer repeated adds. Look up, miss, create, insert becomes: create outside, lock, look up again, insert or take the winner's |
| Property table, **stealing** (`takePropertyTableOrCloneIfPinned`, `runtime/Structure.cpp:1257-1270`; used for additions and for deletions, `:1027`) | **A table that has been reachable from a Structure in the transition tree is never edited again, unless that Structure is a dictionary.** The table belongs to the Structure, not to the object in hand: other, published objects have the same Structure, and their readers take no lock (`Structure::get`, `runtime/Structure.h:1036-1051`; the x86-64 stubs probe the table of any Structure that has one, also up the prototype chain). So the table is **cloned**, under the source's `m_lock`, never stolen. (Open defect I-2 of #249 was here.) A builder that wants to avoid n clones makes the whole run at once (`PropertyRunAddition`, `createWithPropertyAdditions`), with one table for the last Structure |
| Materialization of a table | built aside; then, under `m_lock`, look again and install or drop |
| `PropertyTable` contents | below |
| `m_bitField`, `m_seenProperties`, `m_maxOffset`, `m_propertyHash`, `m_outOfLineTypeFlags` and other words edited in place | atomic read-modify-write (`fetch_or`, `fetch_and`); words that stubs read are read once |
| `m_blob` | one 64-bit store: other threads copy it into new cells |
| `tryMarkPristine` at run time (`runtime/Structure.cpp:745-797`) | walks the transition tree under each node's `m_lock`; per Structure the two rows above. A cell made from the old blob stays unmarked: conservative |
| `m_previousOrRareData`, `allocateRareData` | create outside, publish by compare-and-swap (release); readers acquire |
| `StructureRareData` caches (enumerator, own keys, special properties) | each a single pointer to an immutable record, published by compare-and-swap. Records that own watchpoints: WP2 |
| `m_immutablePropertyNames` | WP3 |
| `m_polyProtoWatchpoint` (`Box<>`) | set once by compare-and-swap of the raw pointer; the loser drops its box |
| Watchpoint sets | below |
| `m_cachedPrototypeChain` | single pointer, compare-and-swap. The caller returns what it installed or found, and does not load again |
| `vm.propertyRuns`, `PropertyAdditionCache` | table lock; the per-Instance `literals.propertyRuns[n]` words are in the private part of the view |
| `StructureCache` | its lock, **for the mutator's lookup too**: `createEmptyStructure` reads `m_structures` without it today ("only the main thread can get here", `runtime/StructureCache.cpp:45-56`) |

**Cost of cloning.** One copy per *new* Structure, so quadratic in the length of a chain that is built for the first time, up to `s_maxTransitionLength` = 128, or 512 for `put_by_id` (`runtime/Structure.h:219-220`). Repeated shapes hit the transition table and clone nothing. A Structure keeps its clone until a collection drops unpinned tables.

### `PropertyTable` with lock-free readers

x86-64 stubs probe the table directly (`findInPropertyTable`, `aot/stubs/AOTStubsPropertyRead.cpp:116-161`; up the chain, `:372-461`). On ARM64 leaf operations do the same walk in C++. `rehash` replaces vector, size and mask one after the other and frees the old vector at once.

| # | Rule |
| --- | --- |
| **PT1** | The index vector is one allocation that begins with its own size, mask and number of used entries. The table holds one pointer to it. A reader loads the pointer once (acquire) |
| **PT2** | An entry is written completely, then its index slot is stored (release), then the number of used entries (release) |
| **PT3** | A removed entry's key is overwritten by the deleted marker; its offset is quarantined (N2) |
| **PT4** | A replaced vector is freed when every thread has passed a safepoint (08, section 8.9). Markers read it only under `Structure::m_lock` (08, H5) |
| **PT5** | Writers hold `Structure::m_lock` |
| **PT6** | Offset and attributes of an entry are one aligned 64-bit word, read by one load and replaced by one store. Compact entries are one word already. In a wide entry (`runtime/Structure.h:184-186`) `m_offset` and `m_attributes` share the second word. So no reader pairs the attribute `Accessor` with the offset of a data slot |
| **PT7** | A traversal (`forEachProperty`) takes `Structure::m_lock` (`forEachPropertyConcurrently` exists), or reads the pointer, then the count in the vector's header (acquire), and visits only entries below it |
| **PT8** | A lock-free reader loads the index slot with acquire order (04, F3) before it loads the butterfly. For a dictionary the StructureID does not change on an add, so N1's acquire orders nothing there |

`replaceOwnValueAndReturn` (`:463-510`), which searches and then **stores**, makes the state test of 6.2 before an out-of-line store.

### Watchpoint sets

With the JIT off nobody's code depends on them. State words are single words, as today. `InlineWatchpointSet::inflate` is a compare-and-swap. `add`, `remove` and `fireAll` hold the set's lock, a leaf lock (08, H15). What the run time's own caches do about changes: 07, section 7.5.

About 80 C++ fast paths test `isStillValid()` and then act (R7 10.6). In the main realm the watched built-ins are locked, and most sets cannot fire.

| # | Rule |
| --- | --- |
| **WP1** | A C++ fast path may rest on a watchpoint set only if a firing that comes after the test leaves it memory safe by this document. Sets that guard a **shape** (`arrayBufferDetach`, `havingABadTime`, sane chain) do not qualify: there the path makes the test of this document itself (storage kind, `LoadView`). W-3.10 lists the sites, each with one of: cannot fire (locked), harmless (a hook is skipped once), replaced |
| **WP2** | A watchpoint is unlinked under its set's lock. A record that owns watchpoints (`m_cachedPropertyNameEnumeratorWatchpoints`, the special property cache's `Bag`) is handed to `freeAfterSafepoint` after they are unlinked |
| **WP3** | `m_immutablePropertyNames` is a member of `StructureRareData` itself, set once, before the Structure is installed in any cell. It does not live in a record that can be replaced: losing it would unlock a built-in |
| **WP4** | Watchpoints that a realm installs at first need are installed when the realm is made: `tryInstallSpeciesWatchpoint` (`runtime/JSGlobalObject.cpp:3553-3647`), which asserts that it runs once and may flatten a prototype, and `tryInstallPropertyDescriptorFastPathWatchpoint` (`:3866-3945`), which `aot/runtime/AOTDefinitionOperations.cpp:265` calls on any thread and which appends to plain vectors. In a `node:vm` realm they follow 07, Z2 |

## 6.6 `Map` and `Set`

Storage is always an `AOT::CompactTable` (R7 5.1): one cell with header, index, key column, value column. Entries are appended, deletion empties the key in place, nothing moves inside a live table. That is nearly a table with lock-free readers already. What breaks it: the obsolete table is **reused in place as a forwarding record** before the owner's pointer is switched, `removeAllEntries` wipes in place, two appenders race, and fast
paths never look at `m_status`.

| # | Rule |
| --- | --- |
| **MS1** | Every operation that changes a table that may be shared (`set`, `add`, `delete`, `clear`, growth, the first table, the move of an in-image table to the heap) holds the **owner's** object lock (the `JSMap` or `JSSet` cell). Strings are resolved and hashed before |
| **MS2** | Append: value, then key (release), then the index slot or tag (release), then `m_used` (release), then `m_alive`. A reader **through the index** depends on the slot it loaded. A reader **by position** (iterators, `forEach`, the collector) loads `m_used` (acquire), then the key (acquire), then the value. Neither sees a key without its value |
| **MS3** | Overwriting the value of an existing key: one store (release for a cell) |
| **MS4** | Delete: the value is left, the key is overwritten by 0 (release), `m_alive` decremented. A reader that has matched the key and then loads the value gets the last value: it is ordered before the delete. The visitor visits all entries below `m_used`, so the value stays alive until the next rehash |
| **MS5** | A table that has been replaced or cleared is **never written again**, with one exception: a field of its own in the header, `m_successor` (8 more bytes in mt builds), stored once (release), and `m_status`, stored after it. The list of deleted entries that iterators need lies in the successor (`m_deletedInPredecessor`, allocated with it). The header of an in-image table is a heap cell (`aot/runtime/AOTCompactTableCreation.cpp:29-38`), so the same holds for it |
| **MS6** | `clear` always installs a new, empty table. `removeAllEntries` in place only for Confined maps |
| **MS7** | `m_storage` of the owner is switched by a release store **before** the old table is marked. A reader loads `m_storage` once and works on that table inside one window |
| **MS8** | `get`, `has`, `size` take no lock |
| **MS9** | Iteration holds `(table, entry)`. `next` reads `m_status` (acquire); `Replaced` or `Cleared`: follow `m_successor`. The flag `wasWalked` is set by an atomic `or`. An iterator object that two threads step is itself a location: its fields are updated under its own object lock |
| **MS10** | An operation that reads the whole table at once (`tryCopy`, one `memcpy` of index and columns, `aot/runtime/AOTCompactTable.cpp:364-383`; `new Map(map)`, serialization, `groupBy`) holds the owner's lock |

**What is promised.** Writers are serialized by MS1. `get`, `has`, `set`, `add`, `delete`, `clear` are linearizable. A `get` that finds the key is ordered at its load of the index slot; one that does not, at the load that ended its probe; a reader on an old table is ordered before the switch. An entry is never reused in a live table, so `get(k)` beside `delete(k)` and `set(k, v2)` returns the old value, absent, or `v2`.
`size` and iteration read `m_alive` and `m_used`, which a `set` stores after the index slot. They may lag behind `has` and `get` by operations that are still in progress, and are exact with respect to operations that happen before them (02, section 2.10.5).

The old table is a GC cell and is traced as long as something points to it, so the window rule is not needed for memory safety here. It is kept for uniformity: a table pointer is a `window` value.

Stubs: `MapSet`, `SetAdd`, `emitDelete`, `GrowCompactTable` get `lockObject` / `unlockObject` around them for maps that may be shared. `GrowCompactTable`'s condition "No marker reads a table meanwhile" (`aot/stubs/AOTStubsCollections.cpp:621-622`) no longer licenses anything: the stub follows MS5 and MS7 always.

`CompactStorage::setIterationEntry` ("the iteration entry" in one per-VM word, `aot/runtime/AOTCompactStorage.cpp:91-99`): `ThreadContext`.

**Cost.** `set` and `delete` on a map that may be shared: two atomic instructions more. `get` and `has`: nothing. Maps are classified per allocation site, so a map that never leaves its thread pays nothing.

### `WeakMap`, `WeakSet`

`WeakMapImpl`: a malloc'ed buffer of pairs of key and value.

| # | Rule |
| --- | --- |
| **MS11** | The buffer begins with its capacity (one pointer, as PT1). Writers hold the object lock. A replaced buffer is freed after a safepoint of every thread; the visitor reads it under the cell lock (08, H5). The shrink and the pruning of dead keys at the end of a collection run with all threads parked |
| **MS12** | Add: value, then key (release); today it is key, then value (`runtime/WeakMapImpl.h:429-430`). Delete: the key becomes the deleted marker (release), and the value is **left** until the next rehash; today `makeDeleted` clears it (`:147-150`). A lookup takes no lock: it loads the key (acquire), then the value. So `get` never returns the empty value |

## 6.7 Strings

Immutable to programs, mutable inside (R7 6).

| State | Rule |
| --- | --- |
| **Opening a string** (`StringEmitter::viewOf*`, `sourceOfRope`, `JSString::view`, `unsafeView`, `GCOwnedDataScope`, natives) | Whoever opens a string loads `m_fiber` (of the string, or of a substring's base) **once**, and takes characters, length and width from that one `StringImpl*`. Atomization can replace an impl by an equal one of the other width at any moment. Today `sourceOfRope` (`aot/codegen/AOTStringEmitter.cpp:190-199`) takes the characters from a stub and the width from a second load: the stub `ropeCharacters` returns characters and flags as a pair |
| **Rope resolution** (`resolveRopeWithFunction`, `runtime/JSString.cpp:329-376`) | Load `m_fiber` **once**; derive `isRope`, `is8Bit`, `isSubstring`, fiber 0 from the loaded word. Each fiber that the walk visits (`resolveRopeInternalNoSubstring`, `resolveRopeSlowCase`) is classified from one load of *its* `m_fiber`: a third thread may resolve it meanwhile. Resolve into a new buffer. Publish by **compare-and-swap** of `m_fiber` from the loaded word (release). The loser derefs its buffer and uses the winner's. Fibers 1 and 2 are never cleared, but once a winner has published they are no longer marked: **from the load of the rope word to the end of the copy there is no safepoint.** `reportExtraMemoryAllocated`, which is one, is called by the winner, after the compare-and-swap. The same for `convertToNonRope` in `equalResolvingRopes` and `resolveSubstringBySharing` |
| **`swapToAtomString`** | compare-and-swap of the word. The old impl is parked until no thread can hold it: the existing list (`appendPossiblyAccessedStringFromConcurrentThreadsOrGCOwnedDataScope`) becomes per thread, merged at a stop. An entry is dropped in one place only: at the end of a collection, by the conductor, if its `JSString` was found on **no thread's** stack (08, section 8.12). `clearConcurrentRetainedDataIfPossible` (`heap/Heap.cpp:1406-1434`, called by the incremental sweeper's timer, `heap/IncrementalSweeper.cpp:77`), which clears the list when "no JavaScript is executing", is empty in mt builds (08, H16). Compiled code that keeps characters, length and width of an opened string across calls relies on exactly this |
| `markAsAtom()`: read-modify-write of the flags byte | atomic `or` |
| `StringImpl::m_refCount` | atomic already. `deref` gets release order, and the thread that frees an acquire fence. The shortcut "when this is the only reference nobody else can be racing to add one" (`wtf:text/StringImpl.h:1218-1229`: no `fetch_sub`, the count stays 1) is taken **only if `!isAtom()`**, one test of a word that is loaded anyway. The atom table holds a pointer that is not counted, and a lookup makes a reference out of it |
| `StringImpl::m_hashAndFlags` | `std::atomic<unsigned>`, `fetch_or` / `fetch_and`. `setHash` racing with `setIsAtom` loses the atom bit today |
| **Atom table** | one table per VM, **sharded**: 64 shards by hash, a lock each. Names that the build saw are atoms in the image, found without any table (`AtomStringTable::s_readOnlyAtomLookup`), so only names made at run time take a lock. Life cycle below |
| `vm.lastAtomizedIdentifier*`, `lastCachedString`, `NumericStrings`, `KeyAtomStringCache`, `StringSplitCache`, `StringReplaceCache`, `immutableButterflyToStringCache`, `JSONCache` | per thread (07, section 7.4). `instance.realm.smallIntStrings`, `intStrings` point at the thread's own |
| `stringSplitIndice`, `m_stringSearcherTables` (a search fills the tables and trusts them, `wtf:text/AdaptiveStringSearcher.h:397-404`), `stringRecursionCheck*` | scratch of the running thread (07, section 7.2) |
| Single-character strings, `emptyString` | made at VM creation, immutable |
| String constants of the program (`instance.stringCell(n)`) | shared region, published by compare-and-swap (07, section 7.6). Two cells for one constant would be invisible, but one saves memory |
| `constants.nextStringHeader` (bump pointer moved by a stub) | private part of the view. A header is named by a shared cell and outlives its thread: chunks of one process-wide reservation, never freed (09, T5) |
| String builders | the builder belongs to an activation. The pool (`instance.stringBuilder.freeStorage`) is in the private part of the view. A builder that is live across an `await` stays with its coroutine, which stays on its thread |
| `JSONWords` (the open source of a typed `JSON.parse`) | private part of the view |

**Life cycle of an atom.** (#249: "refcount 0 is final".)

1. `deref` of an atom is always `fetch_sub(release)`.
2. The thread that reaches 0 takes the shard's lock and removes the bucket **only if it still holds this pointer**. (`remove` looks up by hash and pointer and asserts that it found something, `wtf:text/AtomStringImpl.cpp:555-569`: the assertion goes.) Then it frees.
3. A lookup, under the shard's lock, takes a reference only if the count is not 0 (`tryRef`, a compare-and-swap loop). If it is 0 it makes a new atom and **overwrites the dead atom's bucket** with it.
4. The table is the VM's, whatever thread runs: every JavaScript thread's `Thread::atomStringTable()` points at it, and so does that of the VM's collector, marker and sweeper threads. Destructors of cells that hold atoms (`Symbol`, `PropertyTable`, unlinked executables) run there too.

`FlatString` values (characters, length, width in registers) stay legal across calls and safepoints: a `StringImpl`'s characters are immutable, and the impl is kept alive as above, through the `JSString*` that the same frame holds.

## 6.8 Typed arrays and ArrayBuffers

Elements race as bytes of a SharedArrayBuffer do today, for every buffer. The problem is the triple `{vector, length, mode}` of the view, the life of the bytes, and the buffer's own state (R7 7.3).

| # | Rule |
| --- | --- |
| **TA1** | Memory that a view's `m_vector` has pointed to stays mapped, and is not given to anybody else, until every thread has passed a safepoint **and no pin is left** (TA9). `ArrayBufferContents` that are detached or transferred away go to the deferred-free list (08, section 8.9). The release of a *range* of a live reservation (a shrinking `resize`) is a deferred closure that takes the buffer's lock and releases only what lies above the buffer's **current** length: the buffer may have grown again |
| **TA2** | A writer that shrinks or detaches stores the **length first** (0 or the smaller one, release), then the vector. A reader loads the **vector first** (acquire), then the length. So a reader never pairs a new, smaller memory with an old, larger length |
| **TA3** | A writer that grows commits memory, zeroes `[old length, new length)` explicitly, then stores the length (release). The base of a resizable or growable buffer never moves (it is reserved at its maximum, as today) |
| **TA4** | `transfer` = copy, then detach the source. A grow of a wasm memory detaches the old buffer: TA1, TA2 |
| **TA5** | **The vector of a view that may be shared never moves.** `slowDownAndWasteMemory()` copies the elements of a small ("fast") typed array out of the GC heap when `.buffer` is first read, and loses stores made meanwhile (open defect O17-2 of #249). In mt builds `FastTypedArray` mode is used only by allocation sites that flow proves Confined. Every other view, and every view that C++ makes, is born with malloc'ed memory (`OversizeTypedArray`), which `slowDownAndWasteMemory` adopts without a copy |
| **TA6** | The lazily made `ArrayBuffer` of a view, and the butterfly that names it, are installed under the **view's** object lock, after a second look (6.3.9) |
| **TA7** | `ArrayBuffer` is `ThreadSafeRefCounted`. `ArrayBuffer::m_lock`, a leaf lock (08, H15), covers: `resize`, `transfer`, detach; `pin` and `unpin` against them; the list of incoming references; `m_wrapper` (a member of the **buffer**: two views of one buffer are two object locks); and the reading of `{data, length}` by a view under construction **together with** its registration in that list, so that no detach falls between the two |
| **TA8** | C++ that works on `span()` takes vector and length by TA2, once per window (open defect O17-5). A loop that calls JavaScript between its accesses (`runtime/JSGenericTypedArrayViewPrototypeFunctions.h:240-247` caches `typedVector()` and tests `isDetached()` each turn) holds a `RevalidatedRef` (08, S1'): test and use lie in one window |
| **TA9** | A span that is used while heap access is released (a blocking system call: 08, H1), or that is handed to another OS thread, is **pinned** for that time (08, H11): a thread without heap access counts as having passed a safepoint, so the safepoints alone do not protect it. `pin` and `transfer`, `resize`, detach exclude each other under the buffer's lock (TA7). Contents with a pin are not put on the deferred list: the three do their work on the views' words and leave the old bytes to the last `unpin`. **The last `unpin`** of contents that were detached, transferred away or shrunk meanwhile does not free them. It hands them to `freeAfterSafepoint` (08, H11): another thread may have loaded vector and length before the change and still be in that window (TA1 asks for both conditions). A caller that has no heap access, or that is no JavaScript thread (Bun's work pool), appends to `Heap::m_deferredFreesOfOthers` (08, section 8.9). In the table of built-ins `blocks: true` with an operand of a buffer type makes the emitter pin |
| **TA10** | "Somebody may write these bytes while I look" is true of every buffer whose view is not Confined. What today is done only for `isShared()` is done for all of them: `sort` takes its snapshot (`runtime/JSGenericTypedArrayViewInlines.h:941-963, 1253`; `std::sort`'s unguarded loops and the radix scatter leave the range if elements change under them). An algorithm in two passes (measure, then write: the length of the UTF-16 form of UTF-8 bytes; "is all ASCII", then a copy) bounds its second pass by the size that was **allocated**, not by what the first pass measured |

In compiled code: `LoadView` (04, section 4.6.4). `length`, `byteLength`, `byteOffset` of length-tracking views read the buffer's length word (acquire).

The mode changes only from `OversizeTypedArray` to `WastefulTypedArray`, with vector and length unchanged, so a stale mode is harmless.

**Cost of TA5.** A `malloc`, a `free` and a finalizer where a bump allocation was, for every small view that is not proven Confined. That includes `Buffer`: Bun has no pool (`allocBufferUnsafe` is `JSUint8Array::createUninitialized`, `bun:src/jsc/bindings/JSBuffer.cpp:283-289, 545-550`; `poolSize` is only a property). To be measured at stage 3. The lever, if it is needed: a slab per thread for small oversize vectors, whose chunks the views' finalizers give back.

`Atomics.wait` has one waiter record per VM today: per thread.

## 6.9 RegExp

| State (R7 9) | Rule |
| --- | --- |
| `RegExp` cell: compiled forms, made lazily (also `BytecodePattern::m_bytecodeBehindFlatProgram`) | made under the object lock (upstream takes it already, against compiler threads), after a second look. Readers (`hasCodeFor`, `matchInlineOnce`, stub `matchOnFlatProgram`) take no lock: **`m_state` is stored last (release)** (today before `m_regExpBytecode`, `runtime/RegExp.cpp:574-598`), and a reader loads it first (acquire) |
| `RegExp::deleteCode`, `reset()` | guarded today by "the VM is idle" (08, H16). The compiled forms go to `freeAfterSafepoint`. A matcher holds them inside one window. One that parks at its poll first records its cell in `ThreadContext::m_executingRegExp`, and a form whose cell some thread has recorded stays on the list |
| Code in the image | read-only, re-entrant. Its scratch (backtrack stack, match offsets `regExpMatchOutput`, one per VM) is per thread |
| **`RegExp::m_ovector`**, a member of the shared cell | does not exist in mt builds. `ovectorSpan(thread)` returns the thread's buffer, grown to `offsetVectorSize()` before the match. Its users take it from there: `createRegExpMatchesArray`, `replaceUsingRegExpSearch`, `StringReplaceCache::set`, `subpatternIdForGroupName` (which uses a value from it as an index, `runtime/RegExp.h:188`), Bun's `URLPatternComponent.cpp` |
| The flat matcher's and the bytecode interpreter's scratch, `m_regExpAllocator` | per thread. `m_regExpAllocatorLock`, held for a whole interpreted match today, is gone |
| `RegExpGlobalData`, `RegExpCachedResult` (six words), `RegExpSubstringGlobalAtomCache` | per thread, reached through the private part of the view. `RegExp.$1` is per thread (02) |
| `RegExpObject::m_regExpAndFlags` against `RegExp.prototype.compile` | **one operation loads the word once**, and uses that cell for the match and for building the result. Today the stubs load `regExp` again after the match: `m_numSubpatterns` of a new cell would bound a read of the match output of the old one |
| `lastIndex` | one location, one word. A match reads it once and writes it once |
| Literals shared by `ShareRegExpLiterals` | 04, section 4.8 |
| `RegExpCache` | **all** members under `m_lock`. Today it guards the weak map only: `addToStrongCache` and the ring of parsed patterns run without it (`runtime/RegExpCache.cpp:86-92, 135-138`), and the index wraps by a test for equality |
| `RegExpLiterals` | 07, sections 7.6, 7.7 |
| A match that runs long | polls every 2^16 backtracks and can park (08, section 8.5) |

## 6.10 Other cells with state of their own

**Default rule D1.** State of several words inside a cell that any thread can reach: changes, and reads that traverse it, hold the cell's object lock. Allocate outside, check again inside, never run JavaScript inside.

**"No JavaScript is running" is not a fact that an mt build can ask for** (08, H16). `entryScope` is per thread (07, section 7.2), and a null `entryScope` says nothing about other threads. Tests of that kind are compiled out. The consumers in the code of this document and of 10:

| Consumer | Where | mt |
| --- | --- | --- |
| `clearConcurrentRetainedDataIfPossible` | `heap/Heap.cpp:1406-1434` | empty; 6.7 |
| `RegExp::deleteCode`, pruning of the RegExp cache | `runtime/RegExp.cpp` | `freeAfterSafepoint`; 6.9 |
| `VM::whenIdle`, `m_didPopListeners` | `runtime/VM.cpp:1175-1183, 2178-2183` | the callback runs as a safepoint operation |
| `deleteAllCode`, `shrinkFootprintNow` | `runtime/VM.cpp:1202-1253` | `main`, and refused while `VM::m_threads` has more than one member (10, section 10.2) |

| Cell | Disposition |
| --- | --- |
| `JSFunction`: rare data | made aside, published by compare-and-swap of the tagged word |
| `JSFunction`: lazy `prototype`, `name`, `length` | reification is an add (6.4): the value is made outside, the offset looked up again under the lock. The flags of `FunctionRareData` (`hasReifiedLength` and so on: bit-fields in one byte, `runtime/FunctionRareData.h:177-180`) are set by an atomic `or` **after** the put; today before it |
| `FunctionRareData`: allocation profiles | Structure and prototype are one immutable record, published by compare-and-swap; `clear` swaps in null. `ObjectAllocationProfile::m_allocator` (`bytecode/ObjectAllocationProfile.h:75`), a `LocalAllocator` of the filling thread in a shared cell, is compiled out: no path without the JIT reads it |
| `FunctionRareData::setCompiledAllocation` (8, 16 and 32 bits in three stores, `runtime/FunctionRareData.h:77-87`; `createThis` pairs them on a hot path) | the 24-bit constructor number and the StructureID are one 64-bit word: one store, one load |
| `JSBoundFunction` | immutable, plus lazy words that any thread computes alike |
| `DirectArguments`, `ScopedArguments`, `ClonedArguments` | override storage is filled, then published by compare-and-swap; the "overrode" flags are stored (release) **after** the properties are put; a word that is tested is used from the same load |
| `ErrorInstance`, `Exception`: lazy stack | materialization can run JavaScript: Bun's hook (`bun:src/jsc/bindings/ZigGlobalObject.cpp:695-696`) calls the user's `Error.prepareStackTrace` (`bun:src/jsc/bindings/FormatStackTraceForJS.cpp:155`). So it follows 07, Z2, with a claim byte of its own (not a bit-field shared with other flags): `NotMaterialized`, `Claimed(thread)`, `Done`. The claimant takes the trace out under the object lock, releases the lock, runs the hook, puts the properties (each an add), and stores `Done` (release). `Claimed(other)`: wait as Z2 says (heap access released; 09, W1). `Claimed(itself)`, a `prepareStackTrace` that reads `stack` of the same error: `undefined`, as today |
| `DateInstance` | three words, each read and written whole, also in line by compiled code (`aot/builtins/AOTDateBuiltinEmitter.cpp:16-25, 58-68`). No memory hazard. `setTime` racing with `getHours` can leave an old breakdown cached until the next `setTime` (02, section 2.10.5). The VM's `DateCache` is per thread |
| `ProxyObject` | target and handler are single words; revoke is one store. The trap cache is an array of members of the cell: one 64-bit word per trap, `(handler StructureID, offset)`, read and written whole |
| Array, Map, Set, String iterators, iterator helpers | object lock around a step |
| `JSPromiseCombinatorsGlobalContext`, `JSPromiseCombinatorsContext` | D1: the resolve-element functions can be called by any thread |
| `JSWeakObjectRef`, `JSFinalizationRegistry` | 08, section 8.11 |
| `Intl.*` objects | members that change after construction under the object lock; ICU handles used only through calls that are `const`, else cloned per use |
| `IntlCache`; the realm's cached `localeCompare` collator and number format (three words each, key before value, `runtime/JSGlobalObject.cpp:3464-3506`); the five default formatters | **per thread** (07, section 7.4): an ICU object is opened or cloned per thread. `localeCompare` and `toLocale*String` are `any` and take no serial lock (11, section 11.6) |
| `Symbol` registry, `symbolImplToSymbolMap` and other `WeakGCMap`s and `WeakGCSet`s | table lock, for lookups too |
| `JSSegmentedVariableObject` (global variables): `variableAt` goes through the spine of a `SegmentedVector`, which `addVariables` reallocates (`runtime/JSSegmentedVariableObject.h:60-70`) | the spine is one block with its length inside, replaced by a release store and freed after a safepoint (as PT1, PT4). Variables first, then the symbol table entry (`addSymbolTableEntry` does it the other way round, `runtime/JSGlobalObject.cpp:2598-2613`). Writers hold the declaration lock (10, section 10.3) |
| `SymbolTable`: rare data, `m_localToEntry`, `m_arguments` | D1 |
| `JSGlobalObject` lazy properties | 07, section 7.6 |
| Module records, namespace objects | 09, section 9.9 |
| Generators, promises | 09, sections 9.6, 9.7 |

**A change of time zone or default locale** (`process.env.TZ = ..`) sets a bit in `m_entryScopeServicesRawBits` today; the next thread that *enters* resets "the" `DateCache` and walks `dateInstanceSpace` (`runtime/VM.cpp:2127-2164`). In mt builds the request bumps `VM::m_environmentEpoch` (07, section 7.5). A thread compares it where it compares the cache epoch (07, E2), and resets its own `DateCache` and `IntlCache`. The walk that marks `DateInstance`s stale is one safepoint operation, run by the thread that made the change.

## 6.11 Members of `VM` and `JSGlobalObject` behind the built-ins

`research/R7-ordinary-objects-interpreter.md`, section 10, goes through both classes member by member. Where each kind is settled:

| Kind of member | Where |
| --- | --- |
| state and scratch of the running thread (`stringSplitIndice`, `m_stringSearcherTables`, `m_syncResumeCallCache`, `stringRecursionCheck*` ..) | 07, section 7.2 |
| caches (`NumericStrings`, `lastCachedString`, `immutableButterflyToStringCache`, `IntlCache`, the random number states ..) | 07, section 7.4 |
| set before the second thread starts (the embedder's hooks, `m_name`, `m_evalEnabled` ..); plain counters and flags that become atomics | 07, section 7.2 |
| tables that hold truth (`m_executablesOfNamelessHostFunctions`, `m_impurePropertyWatchpointSets`, `m_signposts` ..) | 07, section 7.7 |
| lazily made things | 07, section 7.6 |
| what only interpreted code needs | 10, section 10.3 |
| `s_nextHashForSymbol` (`wtf:text/SymbolImpl.cpp:96-102`), `lastArraySize` | stay plain: a lost update costs a hash collision or a guess |

The appendix has the row for each member.
