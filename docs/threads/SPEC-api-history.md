# SPEC-api-history.md — archived revisions and review-resolution logs

This file archives, VERBATIM, the full rev-4 text of `docs/threads/SPEC-api.md`
(post-review-round-3), including its rev-2/3/4 change logs and all review-round
refutation/resolution maps. It was moved here when rev 5 compressed the main spec
under the 40,000-byte size cap. Rev 5 made ZERO normative changes: every layout,
protocol step, invariant, lock rank, manifest entry, and task in rev 4 appears in
rev 5, compressed. This file is authoritative for review history and rationale;
normative rules live in `SPEC-api.md`.

---

## Archived rev 4 (full text)

# SPEC-api.md — FROZEN implementation spec (rev 4, post-review-round-3)

**Workstream:** Thread/Lock/Condition/ThreadLocal JS API, Atomics-on-properties, test corpus
**Branch:** `jarred/threads` · **Design doc of record:** `/THREAD.md` (top section)
**Status:** FROZEN. Implement-phase agents follow this spec without redesigning. Anything
ambiguous here is a spec bug; resolve it by choosing the *most conservative* reading and noting
it in the PR description — do not invent new API surface.

This spec covers **phase 1 of the THREAD.md execution plan**: the GIL'd `Thread()`
implementation that serves as the semantic oracle and makes the full test corpus runnable
(THREAD.md line 23: "ship a GIL'd `Thread()` first (semantic oracle + makes the full test
corpus runnable), then remove the GIL layer-by-layer"). Every semantic defined here is the
*final* semantic — later phases change the implementation underneath, never the observable
behavior — **except where a clause is explicitly marked "GIL-phase-only"**.

Rev 2 incorporates adversarial-review round 1. Material changes from rev 1: the lock word
protocol is replaced by delegation to `WTF::Lock` plus a separate holder-identity field
(§5.3); async lock acquisition is now fully specified (§5.5a); the Condition and
property-waiter park protocols are re-specified with explicit lost-wakeup closure (§5.4,
§5.6) and §5.9 rule (a) amended to permit them; GIL preemption is cooperative-only in
phase 1 (§5.2); `Thread.restrict` no longer requires any new Structure/TypeInfo machinery
(§5.7); thread/process lifecycle is frozen (§4.6); Strong-handle destruction points are
frozen (§5.10); the cross-workstream interface adds `currentButterflyTID` and the
attach/detach coordination note (§7); TID recycling is phase-qualified (§5.1, I17); test
ownership is narrowed (§8, §9.1).

Rev 3 incorporates adversarial-review round 2. Material changes from rev 2:
`Thread.restrict` pins restricted objects in uncacheable-dictionary state via
`setHasBeenFlattenedBefore(true)` — review proved the engine otherwise *flattens* the
dictionary at the first IC attempt and silently disables enforcement (§5.7.1, G25); the
restrict choke-point hook is re-homed to an integrator-applied `INTEGRATE-api.md` manifest
entry because SPEC-objectmodel contains no counterpart contract (§5.7.3, §9.2.6; I14 becomes
an integration-phase gate); the §5.6 property-wait protocol is rewritten as ONE ordered
sequence (rev 2 contained two contradictory orderings and a wake-path GIL/listLock deadlock),
with the timed-out-vs-notified arbitration done inside a single `listLock` critical section
(G27) and a specified `waitAsync` timeout mechanism (G26); §5.9 gains rule (e) (never
(re)acquire the GIL while holding any rank ≥ 1 lock — rescoped to ranks 1–3 in rev 4) and
§5.10's PropertyWaiterTable row is
rewritten to match; §5.4 step 5 now removes stale `CondWaiter`s on non-notified park returns;
§5.5a's release pump enumerates `cond.asyncWait`'s immediate release and async-held ownership
gets a ticket identity (`m_asyncHolder` + `consumed` flag, §5.3/§4.3); §4.5 gains dispatch
step 0 (`useJSThreads` off ⇒ today's code path, restoring I1); the `currentButterflyTID`
definition is **deleted** (provider is SPEC-vmstate §6.7; rev 2 created an ODR conflict) and
the §5.2 spawn path adopts the VMLite tid handshake; §5.1 states the TID-recycling
switch-over condition (SPEC-vmstate note N2); G15 is corrected (no `baseline.json`, no race
amplifier, no TSAN target exist in this tree) and I16/I19/task 13 degrade gracefully; §3/§9.2
record the master-flag unification (`useConcurrentJS` aliased to `useJSThreads` at
integration, matching SPEC-jit R6/CS1 — **the alias clause is deleted again in rev 4**; see
below and G33).

Rev 4 incorporates adversarial-review round 3. All eight distinct round-3 findings verified
real against the tree; none refuted. Material changes from rev 3: the `Thread.restrict`
excluded-receiver list gains the **species-watchpoint-protected builtin prototypes and
constructors** — review proved `tryInstallSpeciesWatchpoint` flattens dictionary
prototype/constructor structures **unguarded** by `hasBeenFlattenedBefore()`, and the
ArrayBuffer/TypedArray installs are *lazy and user-triggerable*, so restricting e.g.
`ArrayBuffer.prototype` would let any thread silently un-pin enforcement (G29; Deviation 8,
§4.1, §5.7.1; G25's "during global-object linking" wording corrected); the §5.7.3/§9.2-6
hook is now gated on `Options::useJSThreads()`, skips `PropertySlot::InternalMethodType::
VMInquiry` probes on the get path (engine-internal, exception-forbidden traffic — G31), and
`threadRestrictCheck` gains a mandatory zero-restricted-objects atomic fast path (§5.7.2/3);
the `ThreadAffinityTable` owner identity is re-keyed from the recyclable/shared `uint16_t`
TID to a `Ref<ThreadState>` compared via `WTF::Thread*` (§5.7.2 — rev 3 used exactly the
identity §5.3 disqualifies); `new Thread(fn, ...args)` now roots `fn`/`args` in
`ThreadState` Strongs with frozen §5.10 create/clear rows (rev 3 had a GC/UAF window between
spawn and first JSLock acquisition); the §5.6 `waitAsync` finite-timeout timer is re-homed
from `RunLoop::currentSingleton()` (a runloop spawned threads never drive — the promise
could never settle "timed-out") to **the shared VM's runloop** `vm.runLoop()` (G28), with a
new corpus test; sync **TA-path** `Atomics.wait` from a spawned thread is gated off in
phase 1 (throws the existing G11 `TypeError`) because `WaiterListManager::waitSync` parks
without dropping the JSLock and uses the per-VM `vm.syncWaiter()` — under the GIL the
canonical SAB wait/notify pattern between Threads would deadlock the whole VM (G30; §4.5
step 1a, I1 carve-out, I21); §5.9 rule (e) is rescoped to ranks 1–3 (as written it
contradicted §5.3's own hold protocol, which legally reacquires the GIL while holding the
rank-4 leaf); and §3/§9.2-1's `useConcurrentJS` **alias clause is deleted** — current
SPEC-objectmodel retired that name natively (`SPEC-objectmodel.md:5-6, 192-193`) and its
manifest greps that the identifier exists nowhere (`:1149-1152`), so the alias would have
violated the sibling spec's lint (the stale `SPEC-objectmodel.md:867` citation is dropped).

---

## 1. Grounding: verified facts in this tree

Every load-bearing claim below was verified by reading the named file at the named line in
this working tree. Implementers may rely on these without re-deriving them.

| # | Fact | Evidence |
|---|------|----------|
| G1 | Block handout from the shared heap server is not yet synchronized; the spots are marked. | `Source/JavaScriptCore/heap/LocalAllocator.cpp:138` ("FIXME GlobalGC: Need to synchronize here to when allocating from the BlockDirectory in the server.") and `:170-171` |
| G2 | Every cell already carries a 2-bit lock in the IndexingType byte. | `Source/JavaScriptCore/runtime/IndexingType.h:53`, `:97-98`, `:230` |
| G3 | `useHandlerICInFTL` exists and defaults off. | `Source/JavaScriptCore/runtime/OptionsList.h:638` |
| G4 | `useSharedArrayBuffer` exists and defaults off. | `Source/JavaScriptCore/runtime/OptionsList.h:680` |
| G5 | Every atomization site already threads an `AtomStringTableLocker` through; it is a real `Lock` only under `USE(WEB_THREAD)`, a no-op otherwise. | `Source/WTF/wtf/text/AtomStringImpl.cpp:40-64` |
| G6 | The atom table is **per WTF thread**, resolved via `Thread::currentSingleton().atomStringTable()`, and `JSLock` swaps the VM's table in on acquisition. | `Source/WTF/wtf/text/AtomStringImpl.cpp:68-71`, `Source/JavaScriptCore/runtime/JSLock.cpp:124`, `Source/JavaScriptCore/runtime/VM.h:623,644` |
| G7 | `JSLock` is recursive, supports cross-thread VM migration, and has `DropAllLocks` for blocking sections. | `Source/JavaScriptCore/runtime/JSLock.h:40-50, 73` |
| G8 | `VMManager` stop-the-world machinery exists with a conductor/StopReason model; current clients are the Wasm debugger **and** the memory debugger. | `Source/JavaScriptCore/runtime/VMManager.h:73, 125-132, 214-316` |
| G9 | `Atomics.*` host functions and their validation helpers live in `AtomicsObject.cpp`: `validateAtomicAccess` (`:123`), `validateIntegerTypedArray` (`:145`), host functions (`:396-634`). | `Source/JavaScriptCore/runtime/AtomicsObject.cpp` |
| G10 | Blocking/async waits are centralized in `WaiterListManager`, a process singleton keyed by raw `void*` address; `waitSync` performs the value re-check, the enqueue, **and the blocking wait all inside one `Locker` on the per-list lock** (`Locker listLocker { list->lock }` at `:127`, value check `:128-129`, `addLast` `:131`, `waitForSync(listLocker, …)` `:134` — the wait atomically releases the list lock). `waitAsync` returns a promise via the DeferredWorkTimer pattern. Results map to `vm.smallStrings.okString()/notEqualString()/timedOutString()`; `Terminated` → `vm.throwTerminationException()`. | `Source/JavaScriptCore/runtime/WaiterListManager.cpp:120-145`, `WaiterListManager.h:203-249`, `AtomicsObject.cpp:443-478` |
| G11 | Sync-blocking permission is policy-gated per thread: `vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread()`. | `Source/JavaScriptCore/runtime/AtomicsObject.cpp:459-462` |
| G12 | The `Atomics` global is installed via the `JSGlobalObject.cpp` static table: `Atomics createAtomicsProperty DontEnum|PropertyCallback` with `createAtomicsProperty` at `:428`. | `Source/JavaScriptCore/runtime/JSGlobalObject.cpp:428-431, 729` |
| G13 | `JSDestructibleObject` and `vm.destructibleObjectSpace()` exist, so destructible cells need **no** VM.h subspace member. | `Source/JavaScriptCore/runtime/JSDestructibleObject.h:34`, `Source/JavaScriptCore/runtime/VM.h:485` |
| G14 | A per-thread VM context type already exists (per-thread `VMTraps`, intrusive list node). | `Source/JavaScriptCore/runtime/VMThreadContext.h` |
| G15 | **(corrected in rev 3)** From thread-prep, the bench corpus (`JSTests/threads/bench/`) and `Tools/threads/bench-gate.sh` exist. **`Tools/threads/baseline.json` does NOT exist** (`Tools/threads/` contains only `bench-gate.sh`; the script exits 2 "baseline not found … run with --record first" without it, and `--record` writes it). **No race amplifier and no TSAN no-JIT target exist anywhere in this tree** (grep for amplifier/TSAN hooks over `Tools/`, `JSTests/threads/`, `OptionsList.h` finds only the sibling specs' own forward references; `JSTests/threads/` contains only `bench/`). §8, I16, I19, and task 13 are scoped to this reality. | `JSTests/threads/bench/harness.js:1-16`, `Tools/threads/bench-gate.sh:13-14,33,72` |
| G16 | `run-jsc-stress-tests` supports per-test `//@ requireOptions(...)` directives. | `Tools/Scripts/run-jsc-stress-tests:1029` |
| G17 | `runtime/AtomicsObject.cpp` is already in the build manifest. | `Source/JavaScriptCore/Sources.txt:765` |
| G18 | VM microtask queues are per-VM (`SentinelLinkedList<MicrotaskQueue, …> m_microtaskQueues`). | `Source/JavaScriptCore/runtime/VM.h:1253` |
| G19 | In release builds the generic named-property read path is the **ALWAYS_INLINE** `JSObject::getOwnPropertySlotImpl`/`getOwnPropertySlot` in the header; the out-of-line copy in `JSObject.cpp` exists only under `ASSERT_ENABLED` ("unique (not inlined) … to enable `Structure::validateFlags()` to do checks using function pointer comparisons"). The inline path calls the methodTable only when the `OverridesGetOwnPropertySlot` TypeInfo flag is set. | `Source/JavaScriptCore/runtime/JSObject.h:1459,1471`, `Source/JavaScriptCore/runtime/JSObject.cpp:661-669` |
| G20 | `TransitionKind` contains **no** kind that changes TypeInfo flags (the full enum is property ops, indexing-type/array-storage changes, PreventExtensions/Seal/Freeze, prototype, brand). | `Source/JavaScriptCore/runtime/StructureTransitionTable.h:44-68` |
| G21 | `JSObject::convertToUncacheableDictionary(VM&)` and `JSObject::switchToSlowPutArrayStorage(VM&)` are existing, public, exported APIs; `Structure::toUncacheableDictionaryTransition` exists. | `Source/JavaScriptCore/runtime/JSObject.h:820-821, 848`, `Source/JavaScriptCore/runtime/Structure.h:315` |
| G22 | `WTF::Lock` (adaptive, ParkingLot-based, with the contended-bit handover already solved in `unlockSlow` via `unparkOne`'s `mayHaveMoreThreads`) exposes `tryLock()`, `lock()`, `unlock()`, `isHeld()`, `isLocked()`. `WTF::Thread::uid()` is a process-unique `uint32_t`. `ParkingLot::parkConditionally` takes a validation lambda run under ParkingLot's internal queue lock; `unparkOne` reports `mayHaveMoreThreads`. | `Source/WTF/wtf/Lock.h:75-129`, `Source/WTF/wtf/Threading.h:149`, `Source/WTF/wtf/ParkingLot.h:63-112` |
| G23 | The VMTraps event set in this tree is `NeedShellTimeoutCheck, NeedTermination, NeedWatchdogCheck, NeedDebuggerBreak, NeedStopTheWorld, NeedExceptionHandling` (note: includes `NeedStopTheWorld`, handled in `VMTraps.cpp:508`). There is no registration hook for new events; adding one means editing these shared files. | `Source/JavaScriptCore/runtime/VMTraps.h:149-156`, `Source/JavaScriptCore/runtime/VMTraps.cpp:508` |
| G24 | `JSLock::didAcquireLock` `RELEASE_ASSERT(!m_vm->stackPointerAtVMEntry())` — relocking after a DropAllLocks at an arbitrary point with live JS frames is *not* the host-call-boundary pattern the engine exercises today. | `Source/JavaScriptCore/runtime/JSLock.cpp:137` |
| G25 | **(new in rev 3)** The engine *flattens* dictionary structures back into cacheable non-dictionary structures at IC/caching sites — but **every such site is guarded by `hasBeenFlattenedBefore()`** and degrades to `GiveUpOnCache` / `InvalidPrototypeChain` / early-return when the bit is set: `bytecode/Repatch.cpp:348-354` (`actionForCell`: get ICs), `:619-624` (put-unset), `:1533-1537` (delete ICs), `llint/LLIntSlowPaths.cpp:849-853`, `runtime/Operations.cpp:137-141` (`normalizePrototypeChain`), `bytecode/ObjectPropertyConditionSet.cpp:593-598`. `Structure::setHasBeenFlattenedBefore(bool)` is a **public** setter (`Structure.h:884-888` DEFINE_BITFIELD in a `public:` section, bit at `:904`) and the bit is **inherited by every subsequent transition** (`Structure.cpp:342`). The only *unguarded* flatten call sites operate on program/module/eval **scope objects** (`interpreter/Interpreter.cpp:1192, 1496, 1684` — `scope` / `variableObject` only), on prototype/constructor structures inside `JSGlobalObject::tryInstallSpeciesWatchpoint` (`runtime/JSGlobalObject.cpp:3360, 3379` — **rev 4 correction: NOT only "during global-object linking"; see G29 — two install sites are lazy and user-triggerable**), and in the fuzz-gated test hook `$vm.flattenDictionaryObject` (`tools/JSDollarVM.cpp:3664, 4527`). | named lines |
| G26 | **(new in rev 3)** `WTF::RunLoop` exposes `dispatchAfter(Seconds, Function<void()>&&)`. **(rev 4)** `dispatchAfter` schedules on the receiver runloop and fires only if some thread *drives* that runloop; `RunLoop::currentSingleton()` on a `Thread()`-spawned thread returns a loop nobody ever runs (the §5.2 thread body never calls `RunLoop::run()`/`cycle()`). | `Source/WTF/wtf/RunLoop.h:344` |
| G27 | **(new in rev 3)** `WaiterListManager::waitSyncImpl` resolves timed-out-vs-notified **inside the single `listLock` critical section** that also did the enqueue: after the wait returns, `!syncWaiter->isOnList()` ⇒ OK (a notifier dequeued us); else `findAndRemove(self)` ⇒ TimedOut. A notify and a timeout can never both claim one waiter. | `Source/JavaScriptCore/runtime/WaiterListManager.cpp:135-142` |
| G28 | **(new in rev 4)** The VM owns a runloop captured at creation: `const Ref<WTF::RunLoop> m_runLoop` / `WTF::RunLoop& runLoop() const` (`VM.h:439, 1112`). `DeferredWorkTimer` is a `JSRunLoopTimer` riding that loop (per-VM data is keyed by `RunLoop&`, `JSRunLoopTimer.cpp:45`), and the jsc shell pumps it while async work is pending via `vm.deferredWorkTimer->runRunLoop()` (`jsc.cpp:4480`, `DeferredWorkTimer.cpp:181`). So a timer armed on `vm.runLoop()` fires whenever ticket settlement is possible at all — the same liveness contract every other §5.5 ticket already relies on. `RunLoop::dispatch`/`dispatchAfter` are safe to call from any thread. | named lines |
| G29 | **(new in rev 4)** `JSGlobalObject::tryInstallSpeciesWatchpoint` (`JSGlobalObject.cpp:3345`) flattens the *prototype* structure (`:3359-3360`) and the *constructor* structure (`:3378-3379`) whenever they are dictionaries, with **no `hasBeenFlattenedBefore()` guard**, then `RELEASE_ASSERT`s non-dictionary (`:3361`). Three install sites run during `JSGlobalObject::init` before user code (Array `:2416`, Promise `:2420`, RegExp `:2433`), but **two are lazy and user-triggerable at any time**: ArrayBuffer/SharedArrayBuffer via `speciesWatchpointIsValid` (`JSArrayBufferPrototypeInlines.h:46`, reached from `ArrayBuffer.prototype.slice` et al.) and each `%TypedArray%` view via `JSGenericTypedArrayViewPrototypeFunctions.h:86`. The function also probes `constructor`/`Symbol.species` with `InternalMethodType::VMInquiry` slots followed immediately by `scope.assertNoException()` (`:3368-3369`, `:3382-3383`). | named lines |
| G30 | **(new in rev 4)** The TA sync-wait path blocks **without dropping the JSLock**: `atomicsWaitImpl` (`AtomicsObject.cpp:441-477`) calls `WaiterListManager::singleton().waitSync(vm, ptr, …)` directly inside the host call (no `DropAllLocks` anywhere on the path), and `waitSyncImpl` parks on `vm.syncWaiter()` — a **single per-VM `Waiter` object** (`WaiterListManager.cpp:121`, `VM.h:1343` `const Ref<Waiter> m_syncWaiter`, `VM.cpp:1631-1633`). Sound today (one mutator per VM); under a shared-VM GIL a parked TA waiter starves every other thread of the GIL, and any naive `DropAllLocks` fix would let two threads of the one VM corrupt the shared `Waiter`. | named lines |
| G31 | **(new in rev 4)** `PropertySlot::InternalMethodType::VMInquiry` marks engine-internal probes: "Our VM is just poking around … not allowed to do user observable actions" (`PropertySlot.h:122`); `isVMInquiry()` accessor at `PropertySlot.h:155`. Such probes run in exception-forbidden contexts (e.g. G29's `assertNoException` pairs; `disallowVMEntry` is set for VMInquiry slots, `PropertySlot.h:133-136`). | named lines |
| G32 | **(new in rev 4)** `Structure` carries `hasBeenDictionary` (bit 26, `Structure.h:907`), set on any dictionary transition and surviving flattening (consulted at `Structure.h:782`). Recorded because round 3 proposed gating restrict on it; rejected — see the round-3 notes in §2 (flattening would *also* re-enable IC caching, so a sticky C++-gate alone cannot preserve enforcement). | `Source/JavaScriptCore/runtime/Structure.h:782, 907` |
| G33 | **(new in rev 4)** Current SPEC-objectmodel uses `useJSThreads` **natively** and retires `useConcurrentJS`: "earlier revisions' `useConcurrentJS` name is retired (SPEC-jit CS1 is thereby satisfied without an alias)" (`SPEC-objectmodel.md:5-7`), rename recorded at `:192-194`, and its manifest entry 1 requires "no `useConcurrentJS` anywhere — grep lint" (`:1149-1152`). Rev 3's citation `SPEC-objectmodel.md:867` does not contain a flag (stale). | named lines |

---

## 2. Deviations from THREAD.md, and review-round notes

Where THREAD.md (or the appended 2017 blog post) asserts something the tree contradicts or
underdetermines, this spec rules as follows:

1. **VMManager users.** THREAD.md line 7 says only the wasm debugger uses stop-the-world.
   The tree has two clients (G8). Harmless; noted for accuracy.
2. **Atom table "flip" is more than a flip.** Per G5/G6 the table is per-WTF-thread and
   swapped by `JSLock` on entry. For the GIL phase this spec does **not** make the table
   process-global: all `Thread()` threads enter the *same* VM under the GIL and inherit the
   same `m_atomStringTable` via the existing JSLock swap. The process-global migration
   belongs to the shared-VM-state workstream.
3. **`Atomics.wait` keying is unsound for properties.** `WaiterListManager` keys waiter
   lists by raw slot address (G10); property slots move on butterfly reallocation. This spec
   keys property waits on the pair **(JSCell\*, UniquedStringImpl\*)** (cells never move in
   JSC; §5.6).
4. **`Atomics.wake` does not exist.** The blog uses `Atomics.wake`; this tree implements
   `Atomics.notify`. This spec extends **`notify` only**.
5. **`join()` on "the main thread".** Bun has no browser main-thread rule; blocking is gated
   per-thread via `isAtomicsWaitAllowedOnCurrentThread()` (G11). `thread.join()`,
   `lock.hold()`, and `cond.wait()` reuse exactly that gate.
6. **Butterfly-pointer tagging claims** (THREAD.md line 9) were not re-verified here; they
   are object-model-workstream territory and nothing in this spec depends on them.
7. **`asyncHold`/`asyncWait` release semantics are underdetermined in the blog.** This spec
   freezes an explicit-release form (§4.2, §4.3).
8. **`Thread.restrict` scope is narrowed from the blog's "any proxyable operation".**
   The blog (THREAD.md line 130) wants every proxyable op intercepted. For ordinary objects,
   `getPrototypeOf` is an inline structure read (`structure->storedPrototype`) with no
   hookable choke point short of new TypeInfo machinery (G19, G20), and `[[Call]]`/
   `[[Construct]]` do not route through the object property paths at all. Phase 1 freezes
   the enforced set to: **get, set, has, delete, defineProperty, ownKeys (property-name
   snapshot), setPrototypeOf, isExtensible, preventExtensions — including the ByIndex/
   indexed variants of get/set/delete**. `getPrototypeOf` (a side-effect-free read) and
   call/construct are explicitly *not* enforced in phase 1; documented limitation, revisited
   when the object-model workstream's structure machinery lands. §4.1 and I14 are scoped to
   this list. **Rev 3/4 — excluded receivers:** `Thread.restrict(o)` throws
   `TypeError` (`"cannot restrict this object"`) when `o` is a global object, a global
   proxy, a `Proxy`, an environment/scope object, **or (rev 4) a species-watchpoint-
   protected builtin prototype or constructor** — exactly the objects this tree's
   `tryInstallSpeciesWatchpoint` call sites pass as `prototype`/`constructor`:
   `Array.prototype`/`Array`, `Promise.prototype`/`Promise`, `RegExp.prototype`/`RegExp`,
   `ArrayBuffer.prototype`/`ArrayBuffer` and the SharedArrayBuffer pair (both sharing
   modes), and each `%TypedArray%` view prototype/constructor plus the `%TypedArray%`
   super prototype/constructor (G29 enumerates the install sites). Implementation:
   pointer-compare `o` against the corresponding slots of `o`'s own `JSGlobalObject`;
   slots not yet materialized cannot match (the caller could not hold a reference to an
   unmaterialized object), so lazy accessors need not be forced. Rationale: Proxy traps
   bypass the generic `JSObject` paths entirely, and the remaining excluded classes are
   exactly the receivers subject to *unguarded* dictionary flattening (scope objects:
   `Interpreter.cpp:1192/1496/1684`, G25; species prototypes/constructors:
   `JSGlobalObject.cpp:3360/3379` — and the ArrayBuffer/TypedArray installs are **lazy and
   user-triggerable**, G29, so without this exclusion `new ArrayBuffer(8).slice(0)` on any
   thread would flatten a restricted `ArrayBuffer.prototype` back to a cacheable
   non-dictionary and silently kill §5.7 enforcement). Documented phase-1 limitation.
9. **GIL preemption is cooperative-only in phase 1.** Rev 1 specified a watchdog firing a
   VMTraps trap whose handler does DropAllLocks+yield. Review verified this needs a new
   VMTraps event in shared, unowned `VMTraps.h/.cpp` (G23), and that relocking mid-frame is
   not a pattern the engine exercises (G24). Phase 1 therefore yields **only** at blocking
   primitives (which all DropAllLocks) — see §5.2. `jsThreadGILTimeSliceMs` is reserved but
   inert (default `0`); preemptive time-slicing moves to a later phase (candidate vehicle:
   the existing `NeedStopTheWorld` event + VMManager, G23/G8).

**Review-round refutations / corrections (so re-review does not re-trip):**

- *"VMTraps events are exactly NeedTermination, NeedWatchdogCheck, NeedDebuggerBreak"* —
  stale: this tree's list also has `NeedShellTimeoutCheck`, `NeedStopTheWorld`,
  `NeedExceptionHandling` (`VMTraps.h:149-156`). The reviewers' *conclusion* (no
  yield-capable event, shared-file edit required) is correct and is resolved by Deviation 9;
  the event inventory they cited is not.
- All other round-1 blocker/major findings were verified as real against the tree and are
  resolved in the bodies of §4–§9 below (each section notes what changed).

**Review-round-2 refutations / corrections (so re-review does not re-trip):**

- *"SPEC-heap.md:557-558 freezes: 'The VM-lite workstream constructs GCClient::Heap … and
  brackets thread lifetime with attach/detach; nothing else.'"* — that quote does not exist:
  `grep -n "nothing else" docs/threads/SPEC-heap.md` returns 0 matches. What SPEC-heap
  actually freezes (`SPEC-heap.md:314-315` and `:872-874`) is: "**Whichever workstream
  creates secondary execution contexts** (VM-lite or the Thread API) constructs
  `GCClient::Heap(sharedServer)` and brackets thread lifetime with `attachCurrentThread()` /
  `detachCurrentThread()`", and it assigns the blocking-primitive
  `releaseHeapAccess`/`acquireHeapAccess` obligation **to the Thread-API workstream by
  name**. In phase 1 the workstream creating secondary execution contexts is this one, so
  §5.2's "the thread body brackets `fn`" is *consistent* with SPEC-heap, not contradictory.
  Rev 2's own parenthetical mis-summarized SPEC-heap as assigning the bracketing to the
  VM-lite workstream; that wording is fixed in §5.2. No spec disagreement remains on this
  point.
- All other round-2 blocker/major findings were verified as real (against the tree and/or
  the sibling specs' actual text) and are resolved in the bodies below; load-bearing new
  evidence is in G25-G27 and the corrected G15. Cross-spec resolutions adopted, each
  matching what the sibling spec already records: `currentButterflyTID()` provider is
  SPEC-vmstate §6.7 (this spec's duplicate definition is deleted — §7); TID-recycling
  switch-over per SPEC-vmstate §6.7 note N2 (§5.1); `useConcurrentJS` aliased to
  `useJSThreads` at integration per SPEC-jit R6/CS1 (**superseded in rev 4: alias deleted —
  G33, §3, round-3 map item 8**); the VMLite spawn handshake
  per SPEC-vmstate §6.7 (§5.2).

**Review-round-3 resolution map (all eight distinct findings verified real; none refuted —
each is resolved in-text so re-review can check the fix, not re-derive the bug):**

1. *Lazy species-watchpoint flatten defeats the restrict pin* — real (G29). Resolved by
   extending the Deviation 8 excluded-receiver list (above) to the species-protected builtin
   prototypes/constructors; G25's "global-object linking" wording corrected. The round-3
   alternative of gating on `hasBeenDictionary()` (G32) was **rejected**: it would keep the
   C++ hook firing but the flatten would simultaneously re-enable IC/JIT caching of the
   flattened structure, so accesses would stop reaching the generic paths at all — a sticky
   gate guards a path that is no longer taken. The exclusion closes the only flatten route
   to a restricted object instead.
2. *`waitAsync` finite timeout armed on a never-driven runloop* — real (G26 rev-4 note).
   Resolved: the timer is armed on **`vm.runLoop()`** (G28), §5.6; new corpus test
   `atomics/property-waitasync-timeout.js`; post-GIL re-freeze obligation recorded in §5.6.
3. *TA-path `Atomics.wait` parks holding the GIL; cross-thread SAB wait/notify deadlocks* —
   real (G30). Resolved by a phase-1 gate, not a DropAllLocks bracket: §4.5 step 1a (spawned
   threads throw the existing G11-shaped `TypeError`), I1 carve-out, new invariant I21.
   A bracket alone is unsound anyway — `waitSyncImpl` parks the per-VM `vm.syncWaiter()`
   (G30), and giving each thread its own `Waiter` requires editing unowned
   `WaiterListManager`/`VM.h`; deferred to the post-GIL phase with the §5.6 re-freeze.
   The residual main-thread hazard is documented at §4.5 step 1a.
4. *`threadRestrictCheck` throws into exception-forbidden `VMInquiry` probes* — real (G31;
   G29's `assertNoException` pairs are concrete crash sites). Resolved: the hook's get-path
   skips `slot.isVMInquiry()` traffic (§5.7.3, §9.2 entry 6).
5. *`fn`/`args` unrooted between spawn and first JSLock acquisition* — real (no §5.1 slot,
   no §5.10 row covered them and §5.10 claims exhaustiveness). Resolved: `ThreadState`
   gains `fnSlot`/`argSlots` Strongs (§5.1) with frozen §5.10 rows and the `~ThreadState`
   assert extended.
6. *Affinity table keyed on recyclable/shared uint16 TID* — real (contradicted §5.3's own
   identity rule). Resolved: owner identity is a `Ref<ThreadState>` compared via
   `WTF::Thread*` (§5.7.2); the §7 TID note now states TIDs are never used for
   restrict-owner identity either.
7. *Rule §5.9(e) contradicts the §5.3 hold protocol* — real. Resolved: (e) rescoped to
   ranks 1–3 with the rank-4-leaf shape explicitly permitted and the no-cycle argument
   recorded (§5.9).
8. *`useConcurrentJS` alias contradicts current SPEC-objectmodel; cited line stale* — real
   (G33). Resolved: alias clause deleted from §3 and §9.2 entry 1; the no-`useConcurrentJS`
   grep lint is adopted here too.
9. *Restrict hook taxes flag-off dictionary objects process-wide* — real (the rev-3 hook
   text had no `Options::useJSThreads()` guard and `threadRestrictCheck` had no empty-table
   fast path). Resolved: both added (§5.7.2, §5.7.3, §9.2 entry 6).

---

## 3. Configuration surface (Options)

All new behavior is gated behind option flags. **Implementers may not edit
`runtime/OptionsList.h`** (shared hot file); these are manifest entries for
`docs/threads/INTEGRATE-api.md` (§9). Until integration lands, implementers reference them
through the names below and may keep a local patch out-of-tree.

| Option | Type | Default | Meaning |
|---|---|---|---|
| `useJSThreads` | Bool | `false` | Master switch. When false, none of `Thread`, `Lock`, `Condition`, `ThreadLocal`, `ConcurrentAccessError` are installed on the global object, and `Atomics.*` behaves byte-identically to today (Invariant I1). |
| `maxJSThreads` | Unsigned | `32766` | Max simultaneously-live `Thread`s (TID space is 2^15 per THREAD.md line 21; `0` = main, `0x7fff` = reserved `notTTLTID`, so 1…0x7ffe usable). Exceeding throws `RangeError`. |
| `jsThreadGILTimeSliceMs` | Unsigned | `0` | **Reserved, inert in phase 1** (Deviation 9). Accepted and parsed; has no effect. Later phases use it for preemptive time-slicing. |
| `jsThreadStackSizeKB` | Unsigned | `0` | Native stack size for spawned threads; `0` = WTF::Thread default. |

`Options::useJSThreads()` requires `Options::useSharedArrayBuffer()` semantics for the memory
model but **not** the flag (G4): property atomics do not require SAB to be enabled.

**Master-flag unification (rev 4, cross-spec):** `useJSThreads` is the **single** master
switch, and **no `useConcurrentJS` identifier may be introduced anywhere**. Current
SPEC-objectmodel already uses the `useJSThreads` name natively and retired `useConcurrentJS`
(`SPEC-objectmodel.md:5-7, 192-194`, G33); its integration manifest *forbids* the old
identifier with a grep lint (`:1149-1152`). Rev 3's instruction to integrate
`useConcurrentJS` as an alias is therefore **deleted** (it would have violated the sibling
lint and added dead surface; its `SPEC-objectmodel.md:867` citation was stale — G33). What
remains frozen: at integration exactly one `OptionsList.h` entry named `useJSThreads` exists
(§9.2 entry 1; deduped with SPEC-jit M1 / SPEC-objectmodel §10 entry 1; **this** spec's
description string is canonical), satisfying SPEC-jit CS1/R6 vacuously, as SPEC-objectmodel
itself records. Consequence: `--useJSThreads=0` implies all object-model gating off,
preserving I1/I19 byte-identity; the Thread API can never run with the object-model
protections compiled out.

---

## 4. Public JS API (exact, frozen)

All constructors are global properties installed `DontEnum` via the `PropertyCallback`
mechanism (G12), only when `Options::useJSThreads()`. All are real constructors: calling
without `new` throws `TypeError`. All instances are `JSDestructibleObject` subclasses (G13)
with ordinary prototypes (`Thread.prototype` etc., each with `Symbol.toStringTag`).

### 4.1 `Thread`

- **`new Thread(fn, ...args)`** — `fn` must be callable else `TypeError`
  (`"Thread constructor requires a callable argument"`). Spawns a native thread immediately.
  The new thread calls `fn(...args)` with `this === undefined`. The returned `JSThread`
  object is the *same object* observed as `Thread.current` inside the new thread.
  **GC rooting (rev 4):** `fn` and each element of `args` are rooted in the new
  `ThreadState`'s `fnSlot`/`argSlots` Strongs *by the spawning thread, under the GIL, before
  `WTF::Thread::create`* and cleared by the spawned thread under the JSLock immediately
  after the `fn(...args)` call returns or throws (§5.1, §5.10) — there is no window in
  which a GC can collect them between construction and first execution.
- **`thread.join()`** — blocks until the thread completes; returns `fn`'s return value.
  If `fn` threw, `join()` rethrows that exception value. Multiple `join()` calls (from any
  thread, any number of times, before or after completion) all observe the same result.
  `join()` from the thread itself throws `Error` (`"Thread cannot join itself"`).
  If `isAtomicsWaitAllowedOnCurrentThread()` is false (G11), throws `TypeError`.
  While blocked, the GIL is released (§5.2).
- **`thread.asyncJoin()`** — returns a `Promise` resolved with the result / rejected with the
  thrown exception. Never blocks. Implementation: §5.5 ticket (DeferredWorkTimer pattern,
  G10). Multiple calls return distinct promises with the same settlement.
- **`thread.id`** — number; the engine TID (main thread = `0`).
- **`Thread.current`** — getter; the `JSThread` for the calling thread. On the main thread
  (and any embedder thread not spawned by `new Thread`), a `JSThread` handle is created
  lazily on first access and is stable thereafter for that thread.
- **`Thread.restrict(o)`** — `o` must be an object else `TypeError`; global objects, global
  proxies, `Proxy` objects, environment/scope objects, and the species-watchpoint-protected
  builtin prototypes/constructors throw `TypeError` (Deviation 8, rev-4 exclusion list).
  Marks `o` as restricted
  to the calling thread; returns `o`. Idempotent from the owning thread; calling it from a
  *different* thread on an already-restricted object throws `ConcurrentAccessError`.
  Thereafter every operation in the **enforced set of Deviation 8** (get, set, has, delete,
  defineProperty, ownKeys, setPrototypeOf, isExtensible, preventExtensions, incl. indexed
  variants) performed by any other thread throws `ConcurrentAccessError`. `getPrototypeOf`
  and call/construct are not enforced in phase 1 (Deviation 8). Restriction also has the
  observable side effects of §5.7 (the object becomes an uncacheable dictionary with
  SlowPutArrayStorage indexing — `delete` performance characteristics, not semantics,
  change; property values, ordering, and attributes are unchanged).
- **`ConcurrentAccessError`** — global constructor, subclass of `Error` (`name` is
  `"ConcurrentAccessError"`).

Thread lifecycle states (internal, exact): `Running → Finished(result) | Failed(exception)`.
There is no detach and no cancel. Lifecycle and process-exit semantics: §4.6.

### 4.2 `Lock`

- **`new Lock()`** — non-recursive mutual-exclusion lock.
- **`lock.hold(fn)`** — `fn` callable else `TypeError`. If the calling thread already holds
  the lock (per the holder-identity field, §5.3) throws `Error` (`"Lock is not recursive"`).
  Acquires the lock: first a `tryLock` (uncontended acquisition never blocks and is always
  allowed); on failure, if blocking is disallowed (G11) throws `TypeError`, else releases
  the GIL and blocks. Then calls `fn()`, releases in a `finally`-equivalent (release happens
  even if `fn` throws), returns `fn()`'s result / rethrows its exception.
- **`lock.asyncHold(fn?)`** —
  - With `fn`: returns a `Promise`. When the lock is granted (§5.5a; possibly via the
    immediate-`tryLock` path), `fn()` is invoked on a runloop turn with the lock held; the
    promise settles with `fn`'s result/exception after the lock is released.
  - Without `fn`: returns a `Promise` that resolves, once the lock is granted, with a
    function `release`; the caller must call `release()` exactly once. Calling it twice
    throws `Error`; never calling it stalls other acquirers (documented, not detected).
  - If the calling thread currently holds the lock synchronously, throws `Error`
    (`"Lock is not recursive"`). An async-held lock (§5.3 `m_asyncHeld`) is *not* a
    recursion error from any thread — callers queue normally.
- **`lock.locked`** — getter, boolean: `m_lock.isLocked() || m_asyncHeld` (§5.3). For tests
  only; inherently racy.
- Acquisition order between sync and async acquirers is **unspecified** (barging permitted,
  §5.5a); among async tickets it is FIFO.

### 4.3 `Condition`

- **`new Condition()`**.
- **`cond.wait(lock)`** — `lock` must be a `Lock` held by the calling thread (holder-identity
  check, §5.3), else `TypeError`. Atomically (protocol in §5.4): enqueue this thread as a
  waiter, release `lock`, block (GIL released). On wakeup: reacquire `lock`, then return
  `undefined`. **Spurious wakeups are permitted**; callers must use predicate loops (tests
  must too). Blocking permission gated per G11.
- **`cond.asyncWait(lock)`** — `lock` must be a `Lock` that is either
  **(a) sync-held by the calling thread** (`m_holder` identity check, §5.3), or
  **(b) async-held** (live `m_asyncHolder` ticket, §5.3). Otherwise `TypeError`.
  For case (b) there is **no sound thread identity for "the holder"** — an async hold
  belongs to a ticket/continuation, not a thread — so the call is **accepted without
  ownership validation** (frozen as the conservative choice; rev 2's "held by the calling
  thread" precondition was unverifiable for async-held locks and is deleted). Case (b)
  *consumes* the live async hold: the ticket's `consumed` flag (§5.5) is flipped, so the
  outstanding `release` function thereafter throws the §4.2 double-release `Error`.
  Releases `lock` immediately — case (a): clear `m_holder`, unlock; case (b): under
  `m_queueLock` clear `m_asyncHolder` and `m_asyncHeld`, then unlock — and in **both** cases
  then runs the §5.5a release pump R (asyncWait's release is one of R's enumerated release
  points). Returns a `Promise`. When this waiter is notified, the waiter's ticket is
  moved to the lock's async-acquirer queue (§5.5a); when the lock is granted to it, the
  promise resolves on a runloop turn with a fresh `release` function (same contract as
  `asyncHold()` without `fn`).
- **`cond.notify()` / `cond.notifyAll()`** — wake one / all current waiters (sync and async
  uniformly, FIFO order across both kinds). Returns the number of waiters woken (number).
  May be called with or without holding any lock.

### 4.4 `ThreadLocal`

- **`new ThreadLocal()`**.
- **`threadLocal.value`** — accessor property on `ThreadLocal.prototype` (get/set). Each
  thread observes its own slot; initial value on every thread is `undefined`. Values are
  ordinary JS values (shared-heap references allowed). Storage layout: §5.8.

### 4.5 `Atomics` extended to `(object, propertyName)`

Dispatch rule for every `Atomics` function that today takes `(typedArray, index, …)` —
`load`, `store`, `add`, `sub`, `and`, `or`, `xor`, `exchange`, `compareExchange`, `wait`,
`waitAsync`, `notify`:

0. **If `!Options::useJSThreads()`: the entire function body is today's code path — steps
   1-3 below do not exist.** (Rev 3: review found rev 2's dispatch, read literally, would
   give `Atomics.load({x:1},'x')` the property path even with the flag off, violating I1 and
   §3. The flag check is the first thing each host function does; the shape in
   `AtomicsObject.cpp` is `if (!Options::useJSThreads()) { existing body, textually intact }`
   — matching how `ta-path-unchanged.js` exercises I1 with the flag off.)
1. If `arg0` is a `JSArrayBufferView` (any view, including the float types that today throw
   inside `validateIntegerTypedArray`): take the **existing path, unchanged** (Invariant I1)
   — with exactly one rev-4 carve-out, step 1a.
   1a. **TA sync-wait gate (rev 4; phase-1, GIL-phase-only).** For **`Atomics.wait`
   only** (not `waitAsync`, not `notify`), before the existing body runs: if
   `ThreadManager::isJSThreadCurrent()` (§7), throw the same `TypeError` the existing
   G11 gate throws (`"Atomics.wait cannot be called from the current thread."`,
   `AtomicsObject.cpp:460`). Rationale (G30): `WaiterListManager::waitSync` parks
   **without dropping the JSLock**, so a spawned thread waiting on a SharedArrayBuffer
   would hold the GIL forever and the would-be notifier could never run — the canonical
   SAB wait/notify pattern between `Thread`s would deadlock the whole VM. A
   `DropAllLocks` bracket is not a legal fix in this phase: `waitSyncImpl` parks the
   **per-VM** `vm.syncWaiter()` (G30), so two GIL-dropped threads of the one shared VM
   would corrupt the single `Waiter`; fixing that requires editing unowned
   `WaiterListManager`/`VM.h` and is deferred to the post-GIL re-freeze (§5.6 note).
   This gate lives inside the flag-on branch of the owned `AtomicsObject.cpp` dispatch;
   the flag-off body remains textually intact (I1). **Documented residual phase-1
   hazard:** a *main/embedder*-thread sync TA `Atomics.wait` is still permitted (it must
   be — it is today's behavior and other agents/workers can notify it), but if its only
   possible notifier is a spawned `Thread`, it deadlocks under the cooperative GIL; the
   supported cross-`Thread` blocking-wait primitive in phase 1 is **property**
   `Atomics.wait` (§5.6), which does drop the GIL. Covered by I21 and
   `atomics/ta-wait-thread-gate.js`.
2. Else if `arg0` is an object: take the **property path** below. `arg1` is converted with
   `ToPropertyKey`.
3. Else: `TypeError` (as today).

`Atomics.isLockFree` and `Atomics.pause` are unchanged.

Property-path semantics (all SeqCst; each numbered item is one atomic step per the THREAD.md
line 5 memory model):

- **`Atomics.load(o, k)`** — reads `o`'s **own** property `k`. If absent → `TypeError`
  (`"Atomics.load: object has no own property"`); if it is an accessor or `k` resolves only on
  the prototype chain → `TypeError`. Returns the value.
- **`Atomics.store(o, k, v)`** — sets own data property `k` to `v` (creating it if absent and
  `o` is extensible; `TypeError` if `k` is an accessor, non-writable, or `o` is
  non-extensible and `k` absent). Returns `v`.
- **`Atomics.exchange(o, k, v)`** — like `store` but `k` must already exist as an own data
  property; returns the previous value.
- **`Atomics.compareExchange(o, k, expected, replacement)`** — `k` must exist as own data
  property. If `SameValueZero(current, expected)`, stores `replacement`. Returns the value
  read in either case. Equality is **SameValueZero** (frozen; `===` would make CAS loops on
  NaN impossible).
- **`Atomics.add/sub/and/or/xor(o, k, v)`** — `k` must exist as own data property and its
  current value must be a JS number, and `ToNumber(v)` is applied to the operand; otherwise
  `TypeError`. For `and/or/xor` both are additionally converted with `ToInt32` and the result
  is an int32 number; for `add/sub` plain double arithmetic. Stores the result, returns the
  **old** value. (No coercion is ever applied to the *stored* value: a stored string throws.)
- **`Atomics.wait(o, k, expected, timeout?)`** — `k` must exist as own data property. If
  `!SameValueZero(current, expected)` returns `"not-equal"`. Otherwise blocks (gated per G11;
  GIL released; protocol §5.6) until notified or timed out; returns `"ok"` or `"timed-out"`.
  String results use the exact same small strings as the TA path (G10). Termination behaves
  as the TA path: `vm.throwTerminationException()`.
- **`Atomics.waitAsync(o, k, expected, timeout?)`** — same checks; returns
  `{ async: false, value: "not-equal" }` or `{ async: true, value: Promise }` exactly
  mirroring the TA `waitAsync` result shape, including `"timed-out"` settlement for finite
  timeouts (mechanism specified in §5.6 — rev 3; the TA path's timeout machinery lives
  inside `WaiterListManager`, which this spec does not extend).
- **`Atomics.notify(o, k, count?)`** — wakes up to `count` (default `Infinity`) waiters
  registered on `(o, k)`; returns the number woken. Notifying a key with no waiters returns 0
  and is valid even if `o` has no own property `k`.

Waiter identity is the pair `(cell, uid)` — see Deviation 3 and §5.6. A waiter parked via the
property path is **never** woken by a TA-path `Atomics.notify` and vice versa.

### 4.6 Thread & process lifecycle (frozen; new in rev 2)

Review found "thread completes" ambiguous with pending async work. Frozen semantics:

1. **Completion = `fn` returns or throws.** A thread's lifetime is exactly the execution of
   `fn`. After `fn` returns/throws, the thread body (still under the JSLock) drains the
   shared VM microtask queue once via the existing drain entry point (**GIL-phase-only**
   wording; post-GIL: drains *its own* per-thread queue until empty), publishes the result
   (F1), clears its owned Strongs (§5.10), releases the JSLock, and exits. The thread does
   **not** wait for tickets it registered.
2. **Tickets outlive threads.** Async tickets (§5.5) are process-owned (DeferredWorkTimer,
   G10), not thread-owned. A ticket registered by a thread that has since completed still
   settles normally on whichever thread drains it (GIL-phase relaxation of I12). A ticket
   whose settling condition never occurs (e.g. `asyncHold` on a lock whose holder never
   releases) never settles — same as a never-notified TA `waitAsync` today; not an error.
3. **Process exit / jsc shell.** Pending tickets keep the shell alive exactly the way TA
   `waitAsync` does today (DeferredWorkTimer keeps the runloop scheduled — existing
   behavior, no shell edits). The engine does **not** implicitly join running threads:
   process teardown while an unjoined thread is mid-`fn` is permitted and may terminate it
   abruptly; no invariant covers observable effects of that teardown. **Test convention
   (mandatory):** every test `join()`s or awaits `asyncJoin()` on every thread it spawns.
4. **Who drains.** GIL phase: the single shared VM queue is drained at the existing drain
   points (host call boundaries, shell runloop turns, and step 1's completion drain).
   Post-GIL: each thread drains only its own queue (see §5.5 cross-thread settlement).

---

## 5. Data structures, layouts, lock ordering, fences

### 5.1 Native thread state

```
ThreadManager (process singleton, mirrors WaiterListManager::singleton() shape)
├── Lock m_lock                          // ordering rank 1 (see §5.9)
├── HashMap<uint16_t /*tid*/, Ref<ThreadState>> m_threads
├── Deque<uint16_t> m_freeTIDs
└── uint16_t m_nextTID = 1               // 0 = main, 0x7fff reserved (notTTLTID)

ThreadState : ThreadSafeRefCounted<ThreadState>
├── uint16_t tid
├── RefPtr<WTF::Thread> nativeThread     // rev 4: ALSO set (to &Thread::currentSingleton())
│       //   for lazily-created main/embedder ThreadStates — it is the §5.7.2
│       //   restrict-owner identity, same idiom as §5.3 (compared, never dereferenced;
│       //   the RefPtr keeps the WTF::Thread alive, so no pointer reuse)
├── enum class Phase : uint8_t { Running, Finished, Failed }  (std::atomic, release-published)
├── result slot: Strong<Unknown> (created & cleared under the JSLock; §5.10)
├── Strong<Unknown> fnSlot               // rev 4: roots `fn` from spawn until called (§5.10)
├── Vector<Strong<Unknown>> argSlots     // rev 4: roots `args` likewise (§5.10)
├── Box<Lock> joinLock; Condition joinCondition          // for sync join()
├── Vector<Ref<AsyncTicket>> asyncJoiners                // §5.5
└── HashMap<uint64_t, Strong<Unknown>> threadLocals      // §5.8; owner-thread-only access
```

**TID recycling (phase-qualified — review finding resolved):** the *final* contract, frozen
to match the object-model workstream and THREAD.md line 21, is that a TID is recycled **only
at a GC safepoint after rebias** (no butterfly anywhere may still carry it).
**GIL-phase-only implementation shortcut:** because no butterfly tagging exists yet in phase
1, the GIL build recycles at join-completion. I17 is worded so its test asserts only the
phase-agnostic properties (range; no two *live* threads share a TID; the TID of a live
thread is never handed out) — it does **not** assert the recycling point.
**Switch-over condition (rev 3; closes SPEC-vmstate §6.7 note N2):** recycling moves from
join-completion to GC-safepoint-after-rebias at the moment butterfly TID tagging is enabled
(the object-model workstream's tagging landed *and* active under the unified master flag,
§3). Additional frozen constraint matching SPEC-vmstate §6.7: a TID is never recycled while
any installed `VMLite` still carries it — automatically satisfied at join-completion because
the dying thread's `VMLite::setCurrent(nullptr)` (§5.2 handshake) strictly precedes result
publication and hence join-completion.

The `JSThread` cell layout: `JSDestructibleObject` + `Ref<ThreadState>`. `subspaceFor`
returns `&vm.destructibleObjectSpace()` (G13). Same shape for `JSLockObject` (cell +
`Ref<NativeLockState>`), `JSConditionObject`, `JSThreadLocalObject` (cell + `uint64_t key`,
§5.8).

**Fence requirement F1:** `Phase` is stored with `std::memory_order_release` after the result
`Strong` is written; `join()` readers load it `acquire` before reading the result. (Redundant
under the GIL; specified so the code is already correct when the GIL is removed.)

### 5.2 GIL protocol (phase-1 only; deleted in later phases)

The GIL **is the existing `JSLock`** (G7) of the single shared VM. No new lock is introduced:

- `new Thread(fn)` spawns a `WTF::Thread`; the thread body takes `JSLockHolder lock(vm)` and
  runs `fn`. `JSLock` already migrates the atom table (G6) and stack limits per thread.
  (Coordination notes, corrected/extended in rev 3:
  - **GCClient bracketing — owned by THIS workstream's thread body.** SPEC-heap assigns
    attach/detach to "whichever workstream creates secondary execution contexts"
    (`SPEC-heap.md:314-315`, `:872-874`) and assigns the blocking-primitive
    `releaseHeapAccess`/`acquireHeapAccess` obligation to the Thread-API workstream by name.
    When `GCClient::Heap::attachCurrentThread()`/`detachCurrentThread()` land, the thread
    body brackets `fn` with them (attach immediately after JSLock acquisition, detach in the
    completion sequence before JSLock release). In the GIL phase there is exactly one VM and
    one `GCClient::Heap`, and `JSLock::didAcquireLock` already registers the machine thread
    (`JSLock.cpp:141-142`), so no calls are required for phase 1; the blocking primitives'
    `DropAllLocks` already satisfies the release-access discipline SPEC-heap requires.
  - **VMLite handshake (frozen by SPEC-vmstate §6.7; restated here because the spawn path is
    api-owned).** When SPEC-vmstate's `VMLite` is merged and enabled, the thread body writes
    the allocated TID into the new thread's `VMLite::tid` **before** calling
    `VMLite::setCurrent(&lite)` (before `fn` runs), and calls `VMLite::setCurrent(nullptr)`
    in the completion sequence before the TID can be recycled (§5.1). Without VMLite in the
    build, this is a no-op; ThreadManager remains the sole TID allocator either way (§7).)
- Every blocking primitive in §4 (`join`, contended `hold`, `cond.wait`, property
  `Atomics.wait`) wraps its park in `JSLock::DropAllLocks` (G7), exactly like blocking host
  functions today. These are the **only** yield points.
- **Cooperative-only (Deviation 9):** there is no preemption watchdog in phase 1. A thread
  in a compute loop holds the GIL until it blocks or returns. Consequence for tests: no test
  may rely on preemptive interleaving — all `races/` tests synchronize via the blocking
  primitives (they already do, since they exercise locks/atomics waits). Preemptive
  time-slicing is a later-phase work item (vehicle: `NeedStopTheWorld`/VMManager, G23/G8);
  `jsThreadGILTimeSliceMs` stays reserved for it.

**Invariant-critical:** test programs must never depend on GIL atomicity beyond what §4
promises; the corpus (§8) includes tests annotated `// GIL-INDEPENDENT` that must still pass
when the GIL is removed.

### 5.3 `NativeLockState` (backing `Lock`) — re-specified in rev 2, async-hold identity added in rev 3

Rev 1 froze a hand-rolled `(holderTid | CONTENDED_BIT)` word. Review found two soundness
holes ((a) `mainThreadTID == 0` collides with the "free" encoding; (b) the contended bit is
lost on handover, stranding the second of ≥ 2 parked waiters) plus an identity hole (all
embedder threads share TID 0). All three are eliminated by **not hand-rolling the mutex**:

```
NativeLockState : ThreadSafeRefCounted<NativeLockState>
├── WTF::Lock m_lock                        // the actual mutex; rank 4 leaf (G22).
│       // WTF::Lock's unlockSlow already restores the parked bit from
│       // ParkingLot::unparkOne's mayHaveMoreThreads — the handover bug class is
│       // delegated to proven in-tree code, not re-derived.
├── std::atomic<WTF::Thread*> m_holder      // sync holder identity; nullptr when free
│       //   or async-held. Written ONLY by the thread that just acquired m_lock
│       //   (store after acquire), cleared ONLY by that same thread before unlock.
│       //   Other threads read it solely to compare against
│       //   &WTF::Thread::currentSingleton() — never dereferenced, so no lifetime
│       //   issue. This is the same identity idiom as JSLock::m_lastOwnerThread
│       //   (JSLock.cpp:140) but pointer-valued.
├── std::atomic<bool> m_asyncHeld           // true while held on behalf of a ticket
│       //   (kept as an atomic for the racy `lock.locked` getter only)
├── Lock m_queueLock                        // rank 3 (protects the next two lines)
├── RefPtr<AsyncTicket> m_asyncHolder       // rev 3: identity of the LIVE async hold;
│       //   non-null iff m_asyncHeld. This is the ticket whose `release` capability is
│       //   outstanding; §4.3 asyncWait and §5.5a release validate/consume against it.
└── Deque<Ref<AsyncTicket>> m_asyncWaiters  // FIFO async acquirers (§5.5a)
```

Holder identity is **never** the engine TID (TIDs are 0 for main *and all embedder threads*,
and are recycled — both disqualifying for identity). The non-recursion check (§4.2),
`cond.wait`'s held-by-caller check (§4.3), and `lock.locked` all read `m_holder` /
`m_lock.isLocked()`; all are O(1), and all are correct on the main thread and on multiple
distinct embedder threads.

`hold` protocol: recursion check (`m_holder == &Thread::currentSingleton()` → throw) →
`m_lock.tryLock()`; on failure G11-gate then `DropAllLocks` + `m_lock.lock()` → store
`m_holder = current` (relaxed; the lock's own acquire fence orders it) → run `fn` → clear
`m_holder = nullptr` → `m_lock.unlock()` → async-pump check (§5.5a step R).

**F2 (replaces rev 1's):** no custom fences are required for the mutex itself (`WTF::Lock`
provides acquire/release). `m_holder` stores may be relaxed *because* they are bracketed by
the lock's own acquire/release; `m_asyncHeld` uses release on set / acquire on read.

### 5.4 `NativeConditionState` (backing `Condition`) — re-specified in rev 2, stale-waiter removal added in rev 3

```
NativeConditionState : ThreadSafeRefCounted
├── Lock queueLock                            // ordering rank 3
└── Deque<Ref<CondWaiter>> waiters            // FIFO
CondWaiter
├── enum kind { Sync, Async }
├── std::atomic<uint8_t> state                // Waiting → Notified, flipped exactly once
└── (Sync: parked via ParkingLot on &state; Async: Ref<AsyncTicket>)
```

Rev 1's protocol (enqueue under `queueLock`, release JS lock, then plain park) had a
lost-wakeup window between dropping `queueLock` and parking: a racing `notify` would dequeue
and unpark an address nobody is parked on yet. Frozen fix — the park is a
**`ParkingLot::parkConditionally`** (G22) whose validation lambda re-checks
`waiter->state == Waiting`:

`wait(lock)` exact order:
1. Verify caller holds `lock` (§5.3 identity). Under `queueLock`: append `CondWaiter`
   (state = Waiting).
2. Release the JS `Lock` (clear `m_holder`, `m_lock.unlock()`, run the async pump §5.5a-R).
3. `DropAllLocks` (GIL).
4. `ParkingLot::parkConditionally(&waiter->state, validation = [&]{ return
   waiter->state.load() == Waiting; }, beforeSleep = []{}, deadline)`.
5. On return from `parkConditionally` (rev 3 — rev 1/2 left a stale-entry hole here): take
   `queueLock` and re-check `waiter->state`. If still `Waiting` (token return, or any
   non-notified return), **remove self from `waiters`** — it must be present — and treat the
   return as spurious; if `Notified`, a concurrent `notify` already dequeued us — treat as
   notified. (Without this, a non-notified return leaves a stale `CondWaiter` that a future
   `notify()` dequeues, flips, and unparks against an empty address: that notification is
   consumed by nobody — a lost wakeup for a real waiter, I9 — and `notify()`'s woken-count
   over-reports. With an infinite deadline ParkingLot does not return spuriously in
   practice, but the removal rule is frozen now so any future timeout/termination addition
   cannot introduce the bug.) Then release `queueLock`, reacquire the GIL, reacquire the JS
   `Lock` (ordinary `hold`-style acquire, sans recursion check), return.

`notify()` exact order (per waiter dequeued FIFO under `queueLock`): set
`waiter->state = Notified` (release) **while still under `queueLock`** (rev 3 — this makes
"dequeued ⇔ state flipped" atomic w.r.t. step 5's `queueLock`-held re-check, so a waiter
can never observe itself dequeued-but-still-Waiting), **before**
`ParkingLot::unparkOne(&waiter->state)` (the unpark itself may run after `queueLock` is
released).

**F3 (replaces rev 1's):** the lost-wakeup guard is the *park-side validation*: if notify's
state-flip wins the race, validation fails and the would-be waiter never sleeps; if the park
wins, the waiter is parked before notify's unpark runs (both orderings arbitrated by
ParkingLot's internal queue lock, which is exactly what `parkConditionally`'s validation
exists for, G22). The enqueue-before-JS-lock-release ordering of steps 1–2 additionally
guarantees a notifier that acquires the lock after the waiter releases it observes the
waiter enqueued (I9's subject).

Async waiters: `notify` dequeues them under `queueLock`, then (not holding `queueLock`)
hands the ticket to the lock's async-acquirer queue (§5.5a). Never hold two rank-3 locks at
once (§5.9).

### 5.5 Async tickets

`asyncJoin`, `asyncHold`, `asyncWait`, and property `waitAsync` all use one ticket type
modeled on `WaiterListManager::waitAsync` (G10): `{ Strong<JSPromise>, VM&, registering
ThreadState*, std::atomic<uint8_t> state /* Waiting → Notified | TimedOut, §5.6 */,
std::atomic<bool> consumed /* rev 3: release-capability validity, §4.2/§4.3/§5.5a */ }`,
scheduled via DeferredWorkTimer. The `release` function handed to JS captures its ticket;
`release()` and `cond.asyncWait`'s hold-consumption both do a CAS on `consumed` — the loser
of any race observes `consumed == true` and throws the §4.2 double-release `Error`. This is
the ticket identity rev 2 lacked for async-held ownership.

- **GIL phase (GIL-phase-only relaxation, noted in I12):** all threads share the one VM
  queue; settlement runs on whichever thread drains it. Observationally allowed because the
  GIL serializes execution.
- **Post-GIL mechanism (frozen surface — review found rev 1 delegated this to a provider
  that disclaims it):** each `ThreadState` carries a thread-safe ticket inbox
  (`Lock` rank 3 + `Vector<Ref<AsyncTicket>>`) plus a runloop-wakeup hook. A settling thread
  *never* enqueues into another thread's `MicrotaskQueue` (consistent with the VM-state
  spec's invariant that a `MicrotaskQueue` is touched only by its owning thread); it appends
  to the inbox and wakes the owner's runloop; the **owning thread** drains its inbox into
  its own microtask queue. This wake+pull protocol is part of *this* spec's surface
  (`ThreadState`), not a phantom dependency.

### 5.5a Async lock acquisition (new in rev 2 — review found it unimplementable before)

`asyncHold` and `asyncWait`-reacquisition grant the lock to a *ticket*, not a parked thread;
`WTF::Lock`/ParkingLot cannot do that alone. Frozen protocol (**retry-on-grant**, no direct
handoff):

- **A (acquire):** `asyncHold` first tries `m_lock.tryLock()`. Success → under `m_queueLock`
  set `m_asyncHeld = true` and `m_asyncHolder = ticket` (rev 3), schedule the ticket's settlement (promise resolves on a runloop
  turn per I12; with-`fn` arity runs `fn` there). Failure → enqueue the ticket FIFO on
  `m_asyncWaiters` under `m_queueLock`.
- **R (release pump):** every release of `m_lock` — sync `hold` exit, async `release()`
  call, `cond.wait`'s step-2 release, and **`cond.asyncWait`'s immediate release (§4.3;
  rev 3 — review found this fourth release point missing, which would strand queued
  `asyncHold` tickets until some unrelated release)** — after unlocking, takes `m_queueLock`; if
  `m_asyncWaiters` is non-empty, schedules (DeferredWorkTimer) a **pump task** for the head
  ticket (idempotent: at most one pending pump per lock; guarded by a bool under
  `m_queueLock`).
- **P (pump task, runs on a runloop turn):** `tryLock()`. Success → under `m_queueLock`
  dequeue the head ticket, set `m_asyncHeld = true` and `m_asyncHolder = ticket` (rev 3),
  settle it. Failure (a sync acquirer
  barged in between) → clear the pump-pending bool; the barger's own release re-runs R, so
  no grant is ever lost. Starvation of async tickets under perpetual sync contention is
  possible (barging is explicitly permitted, §4.2); livelock is not — every release
  reschedules the pump.
- **Release of an async-held lock (rev 3 — ticket-validated):** `release()` first CASes its
  captured ticket's `consumed` false→true (failure ⇒ throw the §4.2 `Error`: the hold was
  already released or consumed by `cond.asyncWait`); then under `m_queueLock` asserts
  `m_asyncHolder == ticket`, clears `m_asyncHolder` and `m_asyncHeld`; unlocks `m_lock` (the
  unlock is performed by whatever thread runs `release()` — legal: `WTF::Lock` is not
  owner-checked; *our* ownership bookkeeping is `m_asyncHolder`/the ticket), then runs R.
  `cond.asyncWait`'s consumption of an async hold (§4.3 case (b)) runs the identical
  sequence, with the CAS performed on `m_asyncHolder`'s ticket.
- **`cond.asyncWait` reacquisition:** on notify, the cond waiter's ticket is enqueued via A's
  failure path (plus an immediate R-style pump schedule), so it competes FIFO with other
  async acquirers.

### 5.6 Property-waiter table (backing `Atomics.wait/waitAsync/notify` on properties) — re-specified again in rev 3

```
PropertyWaiterTable (process singleton, lives in runtime/ThreadAtomics.cpp)
├── Lock m_lock                                            // ordering rank 2
└── HashMap<std::pair<JSCell*, UniquedStringImpl*>, Ref<PropertyWaiterList>> m_lists
        // per-list: Lock listLock (rank 3) + Deque of waiters; shape mirrors
        // WaiterListManager's WaiterList (G10). Do NOT extend WaiterListManager
        // itself (not an owned file).
```

Each sync waiter carries its own `WTF::Condition` plus a `std::atomic<uint8_t> state ∈
{ Waiting, Notified, TimedOut }` that is **flipped exactly once, always under `listLock`** —
that single flip is the arbitration point between notify and timeout for both sync and async
waiters.

- Key liveness: while a list is non-empty the table holds a `Strong<JSObject>` on the cell
  and a `Ref<UniquedStringImpl>` on the uid; the entry is removed when the last waiter
  leaves (Strong lifecycle rules: §5.10). Waited-on objects are GC-protected for the
  duration of waits — frozen, documented behavior.

Rev 2's F4 stated two contradictory orderings (its lead sentence took `listLock` *after*
dropping the GIL and re-read the value under it; its value-read note took `listLock` before
`DropAllLocks` with no re-read), and its wake path reacquired the GIL while still holding
`listLock` — an ABBA deadlock against any notifier (notifier: JSLock → wants `listLock`;
waiter: `listLock` → wants JSLock). Rev 3 deletes both texts and freezes ONE protocol.
**The whole §5.6 protocol is GIL-phase-only:** its happens-before argument is the JSLock.
The post-GIL phase MUST re-freeze this section against the object-model's atomic property
reads (a later-phase deliverable, not implementer discretion).

**F4 (rev 3) — `Atomics.wait(o, k, expected, timeout)` exact order:**

1. **Under the JSLock** (host-function entry; GIL held): validate per §4.5; read
   `v = o.k`. If `!SameValueZero(v, expected)` → return `"not-equal"`. There is **no
   re-read step anywhere below** — a property re-read after `DropAllLocks` is illegal
   without the JSLock, and step 2 makes it unnecessary.
2. **Still under the JSLock:** take `PropertyWaiterTable::m_lock` (rank 2), find-or-create
   the `(cell, uid)` list, create the §5.10 Strongs if this is the first waiter, drop
   `m_lock`. Take `listLock` (rank 3), enqueue the waiter (`state = Waiting`), release
   `listLock`.
   *Lost-wakeup closure (GIL-phase argument):* every store to `o.k` and every
   `Atomics.notify(o,k)` runs under the JSLock, which this thread holds continuously from
   step 1's read through this enqueue. A racing store+notify therefore either committed
   wholly before step 1 (waiter returns `"not-equal"`) or its notify runs after the waiter
   is enqueued (the notify dequeues and signals it). No `listLock` needs to be held across
   the GIL drop.
3. `DropAllLocks` (GIL released; **no other lock held at this instant**).
4. Take `listLock`. Loop: if `state != Waiting`, exit the loop (notified). Else
   `waiter->condition.waitUntil(listLock, deadline)` — `WTF::Condition` atomically releases
   `listLock` while sleeping (the `waitForSync` pattern, G10). If `waitUntil` returned
   `false` (deadline reached), exit the loop; spurious `true` returns re-loop. Step 5's
   re-check of `state` under the still-held `listLock` arbitrates a notify that raced the
   deadline.
5. **Result decision, inside the same `listLock` critical section** (mirrors
   `waitSyncImpl`'s arbitration, G27): if `state == Notified` → result `"ok"` (the notifier
   already dequeued us). Else the deadline elapsed with `state == Waiting`:
   `findAndRemove(self)` from the deque — it must succeed, because any notifier flips
   `state` before releasing `listLock` — set `state = TimedOut`, result `"timed-out"`.
   Record `bool listNowEmpty = list is empty`. Because the flip happens exactly once under
   `listLock`, a notify and a timeout can never both claim the same waiter, and `notify`'s
   return count is exact (I10).
6. Release `listLock`. **Only now** does the `DropAllLocks` scope end and the GIL get
   reacquired (rule §5.9(e)). *Scope-nesting (frozen so implementations don't split):*
   steps 3-6 are an explicit `JSLock::DropAllLocks` scope containing a
   `Locker listLocker { list->listLock }` strictly *inside* it; the `Locker` is destroyed
   (step 6) before the `DropAllLocks` destructor runs. No RAII inversion exists.
7. **Under the JSLock again:** if `listNowEmpty` was recorded, take `m_lock` then
   `listLock` in rank order, re-check emptiness (a new waiter may have arrived), and if
   still empty remove the table entry and clear its Strongs (§5.10 — Strong ops require the
   JSLock). Check termination as the TA path does (`vm.throwTerminationException()`),
   else return the result via the same small strings as the TA path (G10).

**`notify(o, k, count)` exact order:** under the JSLock (it is a JS host call), take
`listLock`; dequeue up to `count` waiters FIFO; for each **sync** waiter: set
`state = Notified`, then `condition.notifyOne()` — all under `listLock`; for each **async**
waiter: flip its ticket `state = Notified` under `listLock`, collect it. Release `listLock`,
then settle collected tickets via §5.5 (never holding `listLock` while settling). Returns
the number of waiters whose state **this call** flipped.

**`waitAsync` tickets and their timeout (re-homed in rev 4; GIL-phase-only mechanism):**
tickets enqueue under `listLock` exactly like sync waiters (steps 1-2 run under the JSLock
as part of the `waitAsync` host call). A finite timeout arms, at registration, a
**`vm.runLoop().dispatchAfter(timeout, task)`** timer on the **shared VM's runloop** (G28;
`dispatchAfter` per G26; the in-tree TA timeout machinery lives inside `WaiterListManager`,
which we may not extend). **Rev 3 armed this on `RunLoop::currentSingleton()` of the
registering thread — wrong: a `Thread()`-spawned thread never drives its runloop (the §5.2
body is JSLockHolder → fn → completion; no `RunLoop::run()`), so a timer armed there could
never fire and the promise could never settle `"timed-out"`, violating §4.5.** The VM's
runloop is the loop `DeferredWorkTimer` already rides and the jsc shell already pumps while
async work is pending (`jsc.cpp:4480`, G28), so the timer has exactly the same liveness as
every other §5.5 ticket settlement; `RunLoop::dispatchAfter` is thread-safe to call from
the registering thread (G28). The timer task (runs on the VM runloop, *not* under any lock):
take `JSLockHolder` (rank 0, holding nothing — §5.9(e) satisfied), then `listLock`; if the
ticket's `state` is still `Waiting` → `findAndRemove` it, set `state = TimedOut`, note
`listNowEmpty`, release `listLock`, then settle the promise with `"timed-out"` via §5.5
(settlement and the §5.10 Strong/table cleanup run under the already-held JSLock, step-7
pattern). If `state` is already `Notified` → release `listLock` and do nothing (the notify's
settlement owns the ticket). Infinite timeout arms no timer (matches TA `waitAsync`). The
single-flip-under-`listLock` rule makes timeout-vs-notify races settle each ticket exactly
once. **Post-GIL re-freeze obligation (recorded, like the rest of §5.6):** once threads own
runloops/inboxes (§5.5), timeout arming re-homes to the owning thread's inbox/wakeup
machinery — and the same re-freeze must deliver the per-thread TA-wait fix that lifts the
§4.5 step-1a gate. Coverage: `atomics/property-waitasync-timeout.js` (spawned thread arms a
short finite timeout with no notifier; parent `await`s the promise via `asyncJoin` and
asserts `"timed-out"`).

### 5.7 `Thread.restrict` enforcement — redesigned in rev 2, flatten-pinned and re-homed in rev 3

Rev 1 required a structure transition to a structure with a new `ThreadRestricted`
OutOfLineTypeFlag. Review verified that is unimplementable without new TransitionKind /
Structure machinery (G20) in files owned by the object-model workstream, and that the
release-build property paths would bypass the hooks anyway (G19). Frozen redesign — **uses
only existing, public mechanisms; zero new Structure/TypeInfo machinery**:

1. **Defeat caching and JIT fast paths with existing transitions — and PIN them off
   (rev 3).** `Thread.restrict(o)` (under the GIL) calls the existing
   `o->convertToUncacheableDictionary(vm)` (G21: `JSObject.h:821`) and, if `o` has (or may
   get) indexed properties, `o->switchToSlowPutArrayStorage(vm)` (G21: `JSObject.h:848`),
   **then calls `o->structure()->setHasBeenFlattenedBefore(true)` on the final structure**
   (public setter, G25) and asserts `isUncacheableDictionary()`.
   The pin is load-bearing (round-2 blocker, verified): without it, the IC machinery's
   documented response to an uncacheable dictionary is to **flatten it back into a normal
   cacheable structure on the first cache attempt** — `Repatch.cpp:348-354` and the five
   sibling sites in G25 — including for accesses from the *owning* thread, after which
   `isUncacheableDictionary()` goes false forever, ICs install fast paths, and enforcement
   silently dies after roughly one warm access. With the bit set, **every flatten site
   reachable from generic property paths is guarded by `hasBeenFlattenedBefore()`** and
   degrades to `GiveUpOnCache` / `InvalidPrototypeChain` / early-return (G25), so the object
   remains an uncacheable dictionary permanently; the bit is inherited by any later
   transition of the structure (`Structure.cpp:342`), so it cannot be lost. Residual escapes
   are closed by the **Deviation 8 exclusion list** (§4.1): the unguarded flatten sites
   (G25 as corrected, G29) reach only (a) scope/global objects and (b) the
   species-watchpoint-protected builtin prototypes/constructors — **rev 4: (b) was the
   round-3 hole; `tryInstallSpeciesWatchpoint` flattens those with no
   `hasBeenFlattenedBefore()` guard, and the ArrayBuffer/TypedArray installs are lazy and
   user-triggerable (G29), so they are now excluded receivers** — and both classes throw
   `TypeError` from `Thread.restrict`; `$vm.flattenDictionaryObject` is a fuzz-gated test
   hook (G25) that the corpus must not call on restricted objects. No restrictable object
   can therefore ever be flattened, which is what keeps both the pin and the §5.7.2 cheap
   gate (`isUncacheableDictionary()`) permanently true. (The alternative of switching the
   gate to the sticky `hasBeenDictionary()` bit, G32, was rejected: see §2 round-3 map
   item 1 — a flatten would also re-enable IC caching, bypassing the generic-path hooks
   entirely, so a sticky C++ gate alone cannot preserve enforcement.)
   Consequences, all via existing engine behavior: no IC ever caches named accesses on a
   pinned uncacheable dictionary (now *durably* true, not transiently); JIT'd named accesses
   fall to the `operationGetById*/operationPutById*` slow paths; JIT'd and LLInt indexed
   fast paths are keyed on indexing shape, and `SlowPutArrayStorage` forces them
   out-of-line. Every access to a restricted object therefore reaches generic C++ code.
   **Test obligation (rev 3):** `api/thread-restrict.js` must contain an IC warm-up phase —
   ≥ 10^4 iterations of named get and put on the restricted object from the owning thread
   (enough to drive LLInt/Baseline IC repatch attempts past their warm-up thresholds) —
   after which cross-thread access must still throw `ConcurrentAccessError`. This catches
   the flatten regression class, which can pass in an interpreter-only run and regress
   under JIT.
2. **Affinity table (owner identity re-keyed in rev 4).** Process-singleton
   `ThreadAffinityTable` in `ThreadManager.cpp`
   (`Lock` rank 2 + `HashMap<JSCell*, Ref<ThreadState> /*owner*/>` + a
   `std::atomic<size_t> m_restrictedCount` maintained on insert/erase; entries pruned by a
   weak-handle finalizer — each insert also creates a `Weak<JSObject>` whose finalizer
   erases the entry and decrements the count). **Rev 3 stored `uint16_t ownerTid` — wrong
   by this spec's own §5.3 rule: engine TIDs are shared (0 for main *and every* embedder
   thread, §7) and recycled at join-completion (§5.1), so a recycled or shared TID would
   silently transfer restrict ownership to an unrelated thread.** Frozen identity: the
   owner is the restricting thread's `Ref<ThreadState>` (unique per thread, never recycled;
   the Ref keeps it alive for the life of the entry), and `threadRestrictCheck` compares
   `entry->owner->nativeThread.get() == &WTF::Thread::currentSingleton()` — the §5.3 idiom
   (compared, never dereferenced beyond the RefPtr-kept object; main/embedder threads get
   `nativeThread` set at lazy `ThreadState` creation, §5.1). Exported check (owned files):
   `JS_EXPORT_PRIVATE bool JSC::threadRestrictCheck(JSGlobalObject*, JSObject*)` — returns
   true / throws `ConcurrentAccessError` when the calling thread is not the owner.
   **Mandatory fast path (rev 4):** `threadRestrictCheck` first loads `m_restrictedCount`
   relaxed; zero ⇒ return true touching no lock — so programs that never call
   `Thread.restrict` pay only an atomic load even on dictionary objects with the flag on.
   Cheap gate: callers test `Options::useJSThreads() && structure->isUncacheableDictionary()`
   first (restricted ⇒ uncacheable dictionary by step 1, **and stays one forever because
   step 1 pins flattening off and the §4.1 exclusion list bars every unguarded-flatten
   receiver** — rev 4; the gate is stable), so with the flag off the hook is dead code and
   non-dictionary objects pay one predicted branch on an already-loaded structure field,
   never touching the hash map.
3. **Choke-point hook — an `INTEGRATE-api.md` manifest entry applied by the INTEGRATOR
   (re-homed in rev 3).** Round-2 review verified (and this rev re-verified by grep) that
   `docs/threads/SPEC-objectmodel.md` contains **zero** occurrences of
   `threadRestrictCheck`, `ConcurrentAccessError`, `Thread.restrict`, or any choke-point
   contract — so rev 2's "contract recorded in both manifests, applied by the object-model
   implementer" had no executor: under the frozen-spec/no-coordination execution model, a
   contract recorded only in the consumer's spec is a no-op. Frozen resolution: the hook is
   an **explicit manifest entry in `INTEGRATE-api.md`** (§9.2 entry 6) — exact diffs against
   `JSObject.h` / `JSObjectInlines.h` / `JSObject.cpp`, **applied during the single
   integration/build-fix step, after the object-model workstream's diff has landed**. The
   manifest mechanism exists precisely for shared/unowned files; applying at integration
   also dissolves the merge-conflict hazard, because the integrator edits the final merged
   generic paths and can see any successor entry point the object-model rewrite introduced.
   The hook text (one branch per generic entry point in Deviation 8's enforced set):

   > Every generic-path entry point for the operations in Deviation 8's enforced set —
   > today: `getOwnPropertySlotImpl` (`JSObject.h:1459`), `getOwnPropertySlotByIndex`
   > (`JSObject.cpp:587`), the `putInline*`/`putInlineSlow` family (`JSObjectInlines.h`),
   > `putByIndex`, `deleteProperty` + `deletePropertyByIndex`, `defineOwnProperty`,
   > `getOwnPropertyNames`, `setPrototype`(`Of`), `isExtensible`, `preventExtensions`
   > (`JSObject.cpp`) — plus any successor generic entry point present in the merged tree
   > at integration time (the integrator enumerates them) — MUST begin with:
   > `if (UNLIKELY(Options::useJSThreads() && structure->isUncacheableDictionary()) && !threadRestrictCheck(globalObject, object)) return /*op-appropriate failure*/;`
   > **On the get-path entry points (those taking a `PropertySlot&`), enforcement is
   > additionally skipped when `slot.isVMInquiry()`** (`PropertySlot.h:155`, G31): VMInquiry
   > probes are engine-internal, run in exception-forbidden contexts (e.g.
   > `tryInstallSpeciesWatchpoint`'s probes are followed immediately by
   > `scope.assertNoException()`, `JSGlobalObject.cpp:3368-3369/3382-3383`, and set
   > `disallowVMEntry`), and are not "proxyable operations" in the blog's sense — throwing
   > there is an assertion crash in debug and stale-exception VM state in release. The
   > skip is part of the frozen hook text, applied uniformly by the integrator.

   Rev 4 gating rationale: rev 3's hook had no `Options::useJSThreads()` guard, so every
   generic-path op on *any* uncacheable dictionary object (deletes, reified statics — common
   in single-threaded code) would have taken a process-singleton lock and a hash probe even
   with the flag off, a flag-off contention/timing regression against I1/I19's spirit. With
   the guard, flag-off builds evaluate one always-false option load; flag-on builds with no
   restricted objects stop at §5.7.2's relaxed count load.
   This is one branch on already-slow paths and adds nothing to the inline fast path for
   non-dictionary objects. **Until entry 6 is applied, `api/thread-restrict.js` is
   `//@ skip`ped with a comment naming §9.2 entry 6, and I14 is an integration-phase
   acceptance gate, not an implement-phase one** (restated at I14). The implement-phase
   deliverables for restrict are complete without it: the §4.1 surface, the step-1
   conversions + pin, the affinity table, the exported `threadRestrictCheck`, and the test.

`Thread.restrict` on an object with named-property ICs already compiled: the dictionary
transition fires the existing structure-transition watchpoints; stale ICs are invalidated by
existing machinery — no new safepoint logic.

### 5.8 `ThreadLocal` storage

Each `JSThreadLocalObject` cell carries a process-unique monotonically increasing `uint64_t
key` (allocated from `ThreadManager` under `m_lock`). Per-thread storage is the
`HashMap<uint64_t, Strong<Unknown>>` in that thread's `ThreadState` (§5.1; main thread's
lazily-created `ThreadState` included). `value` get/set only ever touch the *current*
thread's map — no locking, no cross-thread access, by construction. GC: values are `Strong`,
so they are roots until the thread exits or the value is overwritten. A dead `ThreadLocal`
cell leaks its slots in still-live threads until those threads exit — frozen, documented
phase-1 behavior (recorded as acceptable in I13). Strong lifecycle: §5.10.

### 5.9 Lock ordering (total order; acquiring against rank order is a bug)

```
rank 0:  GIL (JSLock)                          — outermost; dropped before any park (see exemptions)
rank 1:  ThreadManager::m_lock
rank 2:  PropertyWaiterTable::m_lock, ThreadAffinityTable lock   (never both at once)
rank 3:  NativeConditionState::queueLock, NativeLockState::m_queueLock,
         PropertyWaiterList::listLock, ThreadState inbox lock     (never two at once)
rank 4:  NativeLockState::m_lock (WTF::Lock) / ParkingLot internal  — leaf
```

Rules:
(a) never *indefinitely* block while holding any rank ≥ 1 native lock, **with two frozen
    exemptions** (rev 2 — rev 1's blanket rule contradicted the only sound wait shapes):
    (a1) `ParkingLot::parkConditionally`'s validation lambda runs under ParkingLot's
    internal queue lock (rank 4 leaf) by design (G22);
    (a2) the §5.6 step-4 wait blocks on a per-waiter `WTF::Condition` **that atomically
    releases `listLock`** while sleeping — the WaiterListManager pattern (G10/G27); the GIL
    is already dropped at that point (§5.6 step 3). No other block-while-holding is
    permitted.
(b) the GIL is always released (`DropAllLocks`) before parking/blocking, in the exact
    per-protocol orders of §5.4 and §5.6;
(c) `WaiterListManager`'s internal lock is never held while taking any of the above
    (we never call into it while holding ours, and it never calls us);
(d) never hold two rank-3 locks simultaneously (§5.4 async-handoff and §5.5a observe this);
(e) **(rev 3, rescoped in rev 4)** the GIL (rank 0) is **never acquired or reacquired while
    holding any rank 1–3 lock**. Every wake/timeout path in §5.4 and §5.6 releases all
    rank 1–3 locks *before* its `DropAllLocks` scope ends — rev 2's §5.6 wake path and
    §5.10 timeout row violated this (waiter held `listLock` wanting the JSLock; notifier
    held the JSLock wanting `listLock`) and were rewritten in rev 3. **The one permitted
    shape is the rank-4 leaf:** holding `NativeLockState::m_lock` across GIL reacquisition
    — the §5.3 contended-`hold` protocol (`DropAllLocks` + `m_lock.lock()`; the
    `DropAllLocks` destructor reacquires the GIL with `m_lock` held), §5.4 step 5's
    JS-`Lock` reacquire, and §4.3/§5.5a's granted-lock resolutions all have it. Rev 3's
    blanket "rank ≥ 1" wording outlawed the spec's own mandatory protocol; the shape is
    deadlock-free because **no GIL holder ever blocks on `m_lock` without first dropping
    the GIL** (the §5.3 protocol's `DropAllLocks` precedes `m_lock.lock()`, and `tryLock`
    never blocks), so the GIL→m_lock and m_lock→GIL edges can never both be "blocked-on"
    edges and no cycle exists. No rank-1–3 lock may ever be held at a GIL (re)acquisition,
    without exception.

### 5.10 `Strong<>` handle lifecycle (new in rev 2 — review found destruction unspecified)

`Strong` create/clear requires the API lock (HandleSet is not thread-safe). Frozen rule:
**every `Strong` named in this spec is created AND cleared only on a thread that currently
holds the JSLock**, at these exact points:

| Strong | created | cleared |
|---|---|---|
| native thread's `Strong<JSThread>` (keeps `Thread.current` alive) | in the spawning thread, under the GIL, before `WTF::Thread::create` returns the handle to the new thread | by the spawned thread itself in its completion sequence (§4.6 step 1), under the JSLock, before release |
| `ThreadState` result slot | completion sequence, under the JSLock | when the *joining* side has no further use it is simply retained until `~ThreadState`; to keep `~ThreadState` lock-free, the completion sequence stores the result **and** every `join`/ticket settlement reads it under the JSLock; the slot itself is cleared in the completion drain of the **last** `JSThread` cell finalization *or* at `ThreadManager` teardown — both run under the JSLock |
| `ThreadState::fnSlot` + `argSlots` (rev 4 — round 3 found `fn`/`args` unrooted between spawn and first execution: the parent can drop the GIL and a GC can run before the spawned thread first takes the JSLock, while the caller frame may have discarded its references — a UAF of the thread function) | in the **spawning** thread, under the GIL, before `WTF::Thread::create` (§4.1) | by the **spawned** thread, under the JSLock, immediately after the `fn(...args)` call returns or throws — before the §4.6.1 completion drain |
| `ThreadState::threadLocals` values | setter, under the GIL (owner thread) | overwrite/thread-exit completion sequence (owner thread, under JSLock); main thread's at VM teardown |
| `AsyncTicket::Strong<JSPromise>` | registration (host call, holds JSLock) | settlement (DeferredWorkTimer task, runs holding the JSLock) — identical to today's `waitAsync`; never-settled tickets are torn down by DeferredWorkTimer's existing VM-shutdown path |
| `PropertyWaiterTable` cell Strongs | first-waiter insert, §5.6 step 2 — under the JSLock, before `DropAllLocks` | empty-list table cleanup of §5.6 step 7 (sync timeout) / the `waitAsync` settlement task (notify and async timeout) — all run **under the JSLock**, taking `m_lock` + `listLock` in rank order and re-checking emptiness. The timed-out waiter's *dequeue* itself (§5.6 step 5) touches no Strong, so nothing Strong-related ever happens off the JSLock, and no lock is held across the GIL reacquisition (§5.9(e)) |

`~ThreadState` `RELEASE_ASSERT`s that the result slot, `fnSlot`, `argSlots`, and
`threadLocals` are empty — making
the "last deref on an arbitrary thread frees handles without the lock" bug class (the review
finding) structurally impossible rather than merely avoided.

---

## 6. Invariants (numbered, testable)

Each invariant has at least one test in §8 referencing its number. In test headers and the
CI grep, invariants are written **namespaced** as `API-I<n>` (e.g. `API-I6`) so sibling
specs' colliding I-numbers don't cross-match (review finding).

- **I1 (no-op when off / TA path frozen):** With `--useJSThreads=0` (default), the observable
  behavior of the entire engine is byte-identical to the base branch; with it on, every
  `Atomics.*` call whose first argument is a `JSArrayBufferView` behaves identically to
  today, including all error messages in `AtomicsObject.cpp:123-180` — **with exactly one
  rev-4 carve-out: sync `Atomics.wait` on a view from a `Thread()`-spawned thread throws
  the §4.5 step-1a `TypeError` (I21)**. No other TA-path behavior differs with the flag on,
  and nothing differs with it off.
- **I2 (result fidelity):** For any value `v` (including objects, NaN, −0),
  `new Thread(() => v).join()` returns a value `SameValue`-equal to `v` (identical reference
  for objects).
- **I3 (exception fidelity):** If the thread function throws `e`, `join()` rethrows the
  *same* `e` (reference identity for objects) and `asyncJoin()` rejects with it.
- **I4 (join idempotence):** Any number of `join`/`asyncJoin` calls from any threads agree on
  the same outcome; none hangs after completion.
- **I5 (current identity):** Inside a spawned thread, `Thread.current` is reference-equal to
  the object returned by `new Thread(...)` in the parent, and is stable across reads.
- **I6 (lock mutual exclusion):** For N threads each doing M `lock.hold(() => counter++)` on
  a shared plain object property, the final value is exactly N×M. Must hold with the main
  thread as one of the contenders (the rev-1 lock encoding failed exactly there; §5.3).
  (GIL-INDEPENDENT.)
- **I7 (lock release on throw):** `lock.hold(fn)` releases the lock when `fn` throws; a
  subsequent `hold` from another thread succeeds.
- **I8 (non-recursion):** Nested `hold` on the same lock from the same thread — including
  the main thread — throws `Error` without deadlocking and without releasing the outer hold.
- **I9 (no lost wakeup):** A `cond.wait(lock)` that enqueued before a `notify()` under the
  same lock is woken by it (modulo spurious wakeups, which only *add* returns, never lose
  them). Test shape: producer/consumer with predicate loops never hangs; also a ≥ 3-thread
  variant (two waiters, sequential notifies) covering the multi-waiter handover path.
- **I10 (property wait/notify lossless):** `Atomics.notify(o, k)` wakes a waiter that
  observed `SameValueZero(o[k], expected)` and parked, with no window in which a
  store+notify between the check and the park is lost (§5.6 F4 rev-3 protocol: the
  JSLock-held read-then-enqueue closes the window; the `listLock`-held single state flip
  arbitrates notify vs timeout). Test: ping-pong via `Atomics.wait/store/notify` on a plain
  property terminates.
- **I11 (waiter-key isolation):** Property waiters on `(o, "k")` are unaffected by
  `Atomics.notify` on a typed array, on `(o, "j")`, or on a different object with the same
  property name.
- **I12 (async settlement):** `asyncJoin`/`asyncHold`/`asyncWait`/`waitAsync` promises
  settle on a runloop turn, never synchronously inside the registering call.
  (GIL-phase-only relaxation: settlement thread is unspecified; post-GIL it is the
  registering thread via the §5.5 inbox protocol — tests assert only ordering, not thread
  identity.)
- **I13 (ThreadLocal isolation):** Writes to `threadLocal.value` on one thread are never
  observable on another; initial value is `undefined` on every thread. The §5.8 leak is
  documented, not a violation.
- **I14 (restrict):** After `Thread.restrict(o)` on thread T, every operation in the
  Deviation 8 enforced set on `o` from thread ≠ T throws `ConcurrentAccessError` — the test
  must exercise named get/set, **indexed get/set on a restricted array**, has, delete,
  defineProperty, ownKeys snapshot, setPrototypeOf, isExtensible, preventExtensions; all
  ops from T continue to work; `o`'s property values are unchanged by the restriction;
  enforcement survives the §5.7.1 IC warm-up loop (≥ 10^4 owner-thread accesses).
  (`getPrototypeOf` and call/construct intentionally untested for enforcement: Deviation 8.)
  **Phase scoping (rev 3): I14 is an integration-phase gate** — enforcement is wired by the
  integrator via §9.2 entry 6; until then `api/thread-restrict.js` is `//@ skip`ped (§5.7.3)
  and the CI coverage grep accepts the skipped file as I14's reference.
- **I15 (atomics atomicity):** N threads × M iterations of `Atomics.add(o, "x", 1)` on a
  plain object yields exactly N×M (GIL-INDEPENDENT); same for a `compareExchange` retry loop.
- **I16 (no torn object model):** Under the race amplifier **when one exists** (rev 3: no
  amplifier is in this tree — G15; until one lands, the `races/` corpus runs plain GIL'd
  execution and I16's at-scale claim is correspondingly weaker — recorded, not hidden), any
  interleaving of property adds from one thread and reads/writes from others never crashes,
  never loses a property that an `Atomics.store` happened-before-published, and `o.f === v`
  reads only values some thread actually wrote (THREAD.md line 5 semantics).
- **I17 (TID bounds):** Spawned thread ids are in `[1, 0x7ffe]`; exceeding `maxJSThreads`
  live threads throws `RangeError`; no two simultaneously-live threads share an id, and a
  live thread's id is never reissued. (The *recycling point* is deliberately not asserted:
  it is join-completion in the GIL phase and GC-safepoint-after-rebias finally — §5.1.)
- **I18 (blocking gate):** Where `isAtomicsWaitAllowedOnCurrentThread()` is false, `join()`,
  contended `hold()`, `cond.wait()`, and property `Atomics.wait` throw `TypeError`, while
  their async variants succeed (and uncontended `hold()` succeeds).
- **I19 (bench gate):** With `--useJSThreads=0`, `Tools/threads/bench-gate.sh` passes
  against a baseline **recorded by the integrator from a jsc built at the pre-workstream
  branch tip** (`bench-gate.sh --record`; rev 3: `Tools/threads/baseline.json` does not
  exist in this tree — G15 — and a baseline recorded from the implementer's own modified
  build would make the gate vacuous). **I19 is an integration-phase gate.** The
  implement-phase obligation is mechanical only: `--record` followed by the gate on the
  same build exits 0 (proves the corpus and script run).
- **I20 (lifecycle determinism):** A pending `asyncJoin` keeps the jsc shell alive until the
  thread completes and the promise settles (§4.6.3, DeferredWorkTimer behavior); a thread
  whose `fn` returns while its own `asyncHold` continuation is pending does not lose that
  settlement (§4.6.2).
- **I21 (TA sync-wait gate; rev 4, GIL-phase-only):** With the flag on, sync `Atomics.wait`
  on a `JSArrayBufferView` from a `Thread()`-spawned thread throws `TypeError`
  (§4.5 step 1a) without parking and without observable side effects; the same call on the
  main thread, and `Atomics.waitAsync`/`Atomics.notify` on views from any thread, behave
  as today. (This invariant is *deleted*, not relaxed, by the post-GIL re-freeze — §5.6.)
- **I22 (spawned-thread waitAsync timeout fires; rev 4):** A property
  `Atomics.waitAsync(o, k, v, finiteTimeout)` registered on a spawned thread with no
  matching notify settles with `"timed-out"` (§5.6 timer on `vm.runLoop()`, G28); the
  settlement is awaitable from the parent (I12's thread-of-settlement relaxation applies).

---

## 7. Public interface consumed by other workstreams

Other implement-phase agents build against these names without coordination. Signatures are
frozen; bodies live in the owned files (§9.1).

```cpp
// runtime/ThreadManager.h
namespace JSC {

class ThreadManager {
public:
    JS_EXPORT_PRIVATE static ThreadManager& singleton();
    static constexpr uint16_t mainThreadTID = 0;        // THREAD.md line 21 convention
    static constexpr uint16_t notTTLTID    = 0x7fff;    // reserved, never allocated
    static uint16_t currentTID();                        // see TID note below
    JS_EXPORT_PRIVATE static bool isJSThreadCurrent();   // true iff spawned by new Thread
    // Diagnostics / future N-mutator integration. NOTE (rev 2): the heap workstream's
    // frozen design roots stacks via GCClient::Heap clientSet() + attach/detachCurrentThread
    // (bracketed per §5.2's coordination note), NOT via this iterator. Offered, not relied on.
    void forEachThreadState(const Invocable<void(ThreadState&)> auto&);
};

// NOTE (rev 3): `currentButterflyTID()` is NOT declared or defined by this workstream.
// Rev 2's provider block here created an ODR/duplicate-symbol conflict with SPEC-vmstate
// §6.7, which declares it in VMLite.h and defines it in VMLite.cpp as
// `VMLite* lite = VMLite::currentIfExists(); return lite ? lite->tid : 0;` — and
// SPEC-objectmodel §9.1 (SPEC-objectmodel.md:757) names the VM-lite side as the provider.
// That single provider stands. ThreadManager remains the SOLE TID ALLOCATOR; the spawn
// path feeds the allocated TID into VMLite::tid before VMLite::setCurrent (§5.2 handshake),
// which is what makes the VMLite definition return the right value on spawned threads.
// (`ThreadManager::currentTID()` stays as the allocator-facing accessor; in the GIL phase
// it returns 0 on main/embedder threads — see TID note below — and it is never used for
// lock-holder identity, §5.3.)

// Thread.restrict choke-point check (§5.7); returns true if allowed, else throws
// ConcurrentAccessError and returns false. Callers gate on isUncacheableDictionary() first.
JS_EXPORT_PRIVATE bool threadRestrictCheck(JSGlobalObject*, JSObject*);

} // namespace JSC

// runtime/ThreadAtomics.h — consumed by DFG/FTL workstream if it intrinsifies property atomics
namespace JSC {
JS_EXPORT_PRIVATE JSValue atomicsLoadOnProperty(JSGlobalObject*, JSObject*, PropertyName);
JS_EXPORT_PRIVATE JSValue atomicsStoreOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue);
JS_EXPORT_PRIVATE JSValue atomicsRMWOnProperty(JSGlobalObject*, JSObject*, PropertyName, AtomicsRMWOp, JSValue operand);
JS_EXPORT_PRIVATE JSValue atomicsCompareExchangeOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue expected, JSValue replacement);
enum class AtomicsRMWOp : uint8_t { Add, Sub, And, Or, Xor, Exchange };
}

// runtime/ThreadObject.h — global installation hooks (wired via INTEGRATE manifest, §9.2)
namespace JSC {
JSValue createThreadProperty(VM&, JSObject* globalObject);
JSValue createLockProperty(VM&, JSObject* globalObject);
JSValue createConditionProperty(VM&, JSObject* globalObject);
JSValue createThreadLocalProperty(VM&, JSObject* globalObject);
JSValue createConcurrentAccessErrorProperty(VM&, JSObject* globalObject);
}
```

**TID note (frozen):** `currentTID()` returns `0` on the main thread **and, in the GIL phase
only, on all embedder threads** (they are serialized by the GIL, so sharing the logical id
is sound *for now*). Post-GIL contract, frozen so the object-model regime is sound: every
thread that enters the VM gets a real TID allocated lazily on first entry; two distinct
embedder threads never share a TID. **TIDs are never used for lock-holder identity** (§5.3
uses `WTF::Thread*`) **nor — rev 4 — for restrict-owner identity** (§5.7.2 uses
`Ref<ThreadState>`/`WTF::Thread*`; rev 3's `ownerTid` keying is deleted), which is what
makes the GIL-phase id-sharing and join-time recycling harmless: nothing keyed on a TID
outlives the thread it identifies.

Type names (frozen, to avoid the `JSC::JSLock` collision — `runtime/JSLock.h:73` owns that
name): `JSThread`, `JSLockObject`, `JSConditionObject`, `JSThreadLocalObject`.

Options names per §3. (Rev 2: there is **no** `ThreadRestricted` TypeInfo flag — §5.7.)

---

## 8. Test corpus layout — `JSTests/threads/`

**Ownership (narrowed in rev 2 — review found globs overlapping sibling specs):** this
workstream owns exactly
`JSTests/threads/harness.js`, `JSTests/threads/api/**`, `JSTests/threads/atomics/**`,
`JSTests/threads/races/**`, and `Tools/threads/run-tests.sh`.
It does **not** own: `JSTests/threads/bench/**` (bench-gate workstream, G15),
`JSTests/threads/heap-*.js` (heap workstream), `JSTests/threads/objectmodel/**`
(object-model workstream). Do not create or modify those.

```
JSTests/threads/
├── harness.js                  # shared asserts: shouldBe, shouldThrow(type, fn),
│                               #   spawnN(n, fn), withTimeout(ms, fn) — plain jsc-shell JS
├── api/
│   ├── thread-basic.js                 # API-I2, API-I4, API-I5
│   ├── thread-exception.js             # API-I3
│   ├── thread-constructor-errors.js    # callable check, no-new TypeError
│   ├── thread-id-bounds.js             # API-I17
│   ├── thread-lifecycle.js             # API-I20
│   ├── thread-restrict.js              # API-I14
│   ├── lock-basic.js                   # API-I6 (small N, incl. main thread), API-I7, API-I8
│   ├── lock-async-hold.js              # API-I12, release-function contract, barging doc'd
│   ├── condition-basic.js              # API-I9 (incl. 2-waiter handover case)
│   ├── condition-async-wait.js         # API-I12
│   ├── threadlocal-basic.js            # API-I13
│   └── blocking-gate.js                # API-I18
├── atomics/
│   ├── ta-path-unchanged.js            # API-I1
│   ├── property-load-store.js
│   ├── property-rmw.js                 # API-I15 single-thread edge cases
│   ├── property-cas-samevaluezero.js   # NaN/-0 equality matrix
│   ├── property-wait-notify.js         # API-I10 ping-pong
│   ├── property-waiter-isolation.js    # API-I11
│   ├── property-waitasync-timeout.js   # API-I22 (rev 4): spawned thread, finite timeout,
│   │                                   #   no notifier; parent awaits "timed-out"
│   ├── ta-wait-thread-gate.js          # API-I21 (rev 4): sync TA wait throws TypeError on
│   │                                   #   a spawned thread; waitAsync/notify on views OK
│   └── property-errors.js              # absent prop, accessor, proto-chain, frozen target
├── races/                      # GIL-INDEPENDENT; run under race amplifier + TSAN target
│                               #   WHEN those thread-prep deliverables exist (G15: neither
│                               #   is in this tree yet; until then these run plain — §10.13)
│   ├── counter-lock.js                 # API-I6 at scale (N=8, M=1e5; includes ≥2 parked waiters)
│   ├── counter-atomics.js              # API-I15 at scale
│   ├── transition-vs-read.js           # API-I16
│   ├── transition-vs-write.js          # API-I16
│   ├── wait-notify-storm.js            # API-I10 under contention
│   └── join-storm.js                   # API-I4
└── threads.yaml                # NOT created by this workstream — manifest entry (§9.2)
```

Conventions (frozen):
- Every test starts with `//@ requireOptions("--useJSThreads=1")` (G16), except
  `ta-path-unchanged.js` (runs twice, with and without the flag).
- Tests are self-checking and silent on success; failure = throw (jsc nonzero exit).
- Every spawned thread is `join`ed or `asyncJoin`-awaited (§4.6.3 mandatory convention).
- No test relies on preemptive GIL interleaving (§5.2 — cooperative-only); progress must
  come from the blocking primitives.
- Race tests put an upper time bound on every blocking operation (`harness.js withTimeout`)
  so a lost-wakeup bug fails fast instead of hanging CI.
- Each test's header comment lists the invariants it covers using the **`API-I<n>`**
  namespaced form; CI greps that every `API-I1`…`API-I22` is referenced by ≥ 1 test. The
  namespacing prevents cross-matching sibling specs' own I-numbers.
- `Tools/threads/run-tests.sh` (owned, new): globs
  `JSTests/threads/{api,atomics,races}/*.js` **and additionally runs**
  `JSTests/threads/heap-*.js` and `JSTests/threads/objectmodel/*.js` when present (it runs
  them; it does not own them — sibling workstreams add files, this runner picks them up).
  Honors `JSC` env var for the shell path, `--filter=`, and `--amplify`.
- **`--amplify` defined behavior (rev 3 — no amplifier exists in this tree, G15):** if an
  executable `Tools/threads/amplify.sh` exists (the frozen probe path for the thread-prep
  amplifier, whenever it lands), wrap each jsc invocation with it; otherwise print exactly
  one warning line (`run-tests.sh: amplifier not present; running plain`) and run plain.
  A missing amplifier is a no-op passthrough, never an error.

---

## 9. File ownership and integration manifest

### 9.1 Owned paths (the ONLY files the implementer of this workstream may create/edit)

```
Source/JavaScriptCore/runtime/ThreadObject.h / .cpp          # JSThread, constructors/prototypes,
                                                             #   createXXXProperty hooks, ConcurrentAccessError
Source/JavaScriptCore/runtime/ThreadManager.h / .cpp         # ThreadState, TID allocation,
                                                             #   threadRestrictCheck, ThreadAffinityTable,
                                                             #   ThreadLocal key allocator
                                                             #   (NOT currentButterflyTID — §7, rev 3)
Source/JavaScriptCore/runtime/ThreadAtomics.h / .cpp         # property-path Atomics impl + PropertyWaiterTable
Source/JavaScriptCore/runtime/ThreadLocalObject.h / .cpp     # JSThreadLocalObject
Source/JavaScriptCore/runtime/LockObject.h / .cpp            # JSLockObject + NativeLockState (§5.3, §5.5a)
Source/JavaScriptCore/runtime/ConditionObject.h / .cpp       # JSConditionObject + NativeConditionState
Source/JavaScriptCore/runtime/AtomicsObject.cpp              # dispatch split only (§4.5 step 1-3);
                                                             #   TA path must remain textually intact (I1)
JSTests/threads/harness.js
JSTests/threads/api/**
JSTests/threads/atomics/**
JSTests/threads/races/**
Tools/threads/run-tests.sh
```

(Rev 2: test ownership narrowed per §8; `JSTests/threads/heap-*.js`, `objectmodel/**`,
`bench/**` are sibling-owned.)

### 9.2 Manifest entries for `docs/threads/INTEGRATE-api.md` (shared hot files — implementers MUST NOT edit these; list them verbatim in the integration manifest instead)

1. **`Source/JavaScriptCore/runtime/OptionsList.h`** — add the four options of §3, formatted
   like `OptionsList.h:638/680`:
   ```
   v(Bool, useJSThreads, false, Normal, "enable shared-memory Thread/Lock/Condition/ThreadLocal API"_s) \
   v(Unsigned, maxJSThreads, 32766, Normal, nullptr) \
   v(Unsigned, jsThreadGILTimeSliceMs, 0, Normal, nullptr) \   // reserved, inert in phase 1 (Deviation 9)
   v(Unsigned, jsThreadStackSizeKB, 0, Normal, nullptr) \
   ```
   **Dedupe note (rev 4):** SPEC-jit M1 and SPEC-objectmodel §10 entry 1 list the same
   `useJSThreads` flag; at integration exactly ONE entry is added, with **this** description
   string (canonical). **No `useConcurrentJS` identifier is introduced anywhere** — current
   SPEC-objectmodel uses `useJSThreads` natively and its manifest greps that the old name
   does not exist (G33); rev 3's alias instruction is deleted (§3, master-flag unification).
   This spec adopts the same grep lint for the integrated tree.
2. **`Source/JavaScriptCore/runtime/JSGlobalObject.cpp`** — five static-table rows in the
   `@begin globalObjectTable` block (pattern of line 729), each guarded inside its
   `createXXXProperty` body by `Options::useJSThreads()` returning `jsUndefined()` when off:
   ```
   Thread                createThreadProperty                 DontEnum|PropertyCallback
   Lock                  createLockProperty                   DontEnum|PropertyCallback
   Condition             createConditionProperty              DontEnum|PropertyCallback
   ThreadLocal           createThreadLocalProperty            DontEnum|PropertyCallback
   ConcurrentAccessError createConcurrentAccessErrorProperty  DontEnum|PropertyCallback
   ```
   plus `#include "ThreadObject.h"`.
   *Deviation note:* returning `jsUndefined()` when disabled makes
   `globalThis.Thread === undefined` but `"Thread" in globalThis === true`. Pre-approved
   fallback: install via `JSGlobalObject::init()` `putDirect` guarded by the option.
3. *(Removed in rev 2.)* Rev 1 allocated a `ThreadRestricted` TypeInfo bit in
   `JSTypeInfo.h`. §5.7 no longer uses TypeInfo at all; no `JSTypeInfo.h` edit exists.
4. **`Source/JavaScriptCore/Sources.txt`** — add `runtime/ThreadObject.cpp`,
   `runtime/ThreadManager.cpp`, `runtime/ThreadAtomics.cpp`, `runtime/ThreadLocalObject.cpp`,
   `runtime/LockObject.cpp`, `runtime/ConditionObject.cpp` (alphabetical, near
   `Sources.txt:765`).
5. **`Source/JavaScriptCore/CMakeLists.txt`** — add the six new `.h` files to
   `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`.
6. **`Thread.restrict` choke-point hook — applied by the INTEGRATOR (rewritten in rev 3,
   hook text amended in rev 4).**
   `INTEGRATE-api.md` carries the §5.7.3 hook text — **the rev-4 text: gated on
   `Options::useJSThreads()`, and get-path entry points skip `slot.isVMInquiry()` probes
   (G31)** — plus the current entry-point list as an
   exact-diff manifest entry against `JSObject.h` / `JSObjectInlines.h` / `JSObject.cpp`,
   **applied during the integration/build-fix step, after the object-model workstream's
   diff has landed** (the integrator also covers any successor generic entry point present
   in the merged tree — §5.7.3). Rationale: `SPEC-objectmodel.md` contains no counterpart
   contract (verified by grep, §5.7.3), so rev 2's "object-model implementer applies it"
   had no executor; an `INTEGRATE-objectmodel.md` entry cannot be written by this spec
   either (that manifest is the objectmodel agent's output). This workstream supplies the
   exported `threadRestrictCheck` (§7), the restrict-time conversions + flatten pin
   (§5.7.1), and the acceptance test (`api/thread-restrict.js`, API-I14 —
   `//@ skip`ped until this entry is applied; the integrator un-skips it and it must then
   pass). No file is edited by two workstreams; no implement-phase agent edits these files
   at all.
7. **Test-runner wiring** — `JSTests/threads.yaml` (or the equivalent stanza for
   `run-javascriptcore-tests`) running `api/`, `atomics/`, `races/` with `runDefault`-family
   commands; until landed, `Tools/threads/run-tests.sh` is the runner.

No edits to `VM.h`/`VM.cpp`/`JSGlobalObject.h`/`VMTraps.h`/`VMTraps.cpp`/`Structure.h`/
`Structure.cpp`/`StructureTransitionTable.h`/`JSTypeInfo.h` are required by this design
(rev 2 verified: subspaces via `destructibleObjectSpace()` G13; singletons mirror
`WaiterListManager` G10; no preemption traps — Deviation 9; no TypeInfo/Structure machinery —
§5.7; per-thread state lives in `ThreadState`, not `VM`).

---

## 10. Ordered task list (one large implementation agent)

1. **Scaffolding:** create the six runtime file pairs from §9.1 with class skeletons,
   `ClassInfo`, prototypes, `subspaceFor` → `destructibleObjectSpace()` (G13), and the five
   `createXXXProperty` functions; keep a local (uncommitted-to-shared-files) patch for the
   §9.2 manifest items so you can build and run; copy that patch verbatim into
   `docs/threads/INTEGRATE-api.md` as you go.
2. **ThreadManager + GIL thread spawn:** `ThreadState`, TID allocation/recycling (§5.1,
   API-I17), `currentTID` (NOT `currentButterflyTID` — §7 rev 3; the §5.2 VMLite handshake
   is a no-op until VMLite is merged), `new Thread(fn, ...args)` under
   `JSLockHolder` (§5.2) **with the rev-4 `fnSlot`/`argSlots` rooting (§4.1, §5.10 — create
   before `WTF::Thread::create`, clear after the call returns/throws)**, result/exception
   capture with F1 and the §5.10 Strong points,
   completion sequence (§4.6.1), sync `join()` with `DropAllLocks` + G11 gate
   (API-I2–I5, I18).
3. **`Thread.current` + main-thread lazy state.** (No preemption watchdog — Deviation 9.)
4. **`asyncJoin`** via the §5.5 ticket pattern (API-I12, I20).
5. **`Lock`:** `NativeLockState` per §5.3 (WTF::Lock + holder field), `hold`
   (API-I6–I8), then the §5.5a async protocol: `asyncHold` both arities, release pump,
   `locked` getter.
6. **`Condition`:** §5.4 parkConditionally protocol, `wait`/`notify`/`notifyAll` (API-I9),
   then `asyncWait` reacquisition through §5.5a.
7. **`ThreadLocal`** (§5.8, API-I13).
8. **Atomics dispatch split** in `AtomicsObject.cpp` (§4.5 steps 0–3) leaving the flag-off
   TA path textually intact, **including the rev-4 step-1a spawned-thread sync-TA-wait gate
   (API-I21, `ta-wait-thread-gate.js`)**; land `ta-path-unchanged.js` first and keep it
   green (API-I1).
9. **Property atomics:** `load/store/exchange/compareExchange` with SameValueZero, then the
   RMW family (API-I15 single-threaded coverage).
10. **PropertyWaiterTable** (§5.6 F4 rev-3 protocol, including the `waitAsync` timeout
    timer **armed on `vm.runLoop()` — rev 4, G28; never `RunLoop::currentSingleton()`**):
    `wait`, `waitAsync`, `notify` on properties (API-I10, I11, I22 —
    `property-waitasync-timeout.js`).
11. **`Thread.restrict` + `ConcurrentAccessError`** (§5.7): receiver exclusions (§4.1 —
    **including the rev-4 species-protected prototypes/constructors, G29**),
    `convertToUncacheableDictionary` + `switchToSlowPutArrayStorage` +
    `setHasBeenFlattenedBefore(true)` pin (§5.7.1 — the pin is mandatory; without it
    enforcement dies at the first IC warm-up, G25), affinity table (**rev 4:
    `Ref<ThreadState>` owner identity + `m_restrictedCount` fast path, §5.7.2**),
    `threadRestrictCheck`
    export, the hook diff into `INTEGRATE-api.md` (entry 6 — **rev-4 text: flag-gated +
    VMInquiry skip**, integrator-applied), acceptance
    test with the IC warm-up loop (API-I14 — `//@ skip`ped with a comment naming entry 6;
    it becomes a hard gate at integration).
12. **Test corpus:** finish every file in §8 (api → atomics → races); wire
    `Tools/threads/run-tests.sh`; verify the `API-I` coverage grep finds API-I1–API-I22.
13. **Gates (rev 3 — degrade gracefully; G15: no amplifier, no TSAN target, no baseline
    exist in this tree):** run the race corpus via `Tools/threads/run-tests.sh` — with
    `--amplify` if `Tools/threads/amplify.sh` exists, else plain (frozen §8 fallback); run
    under the TSAN no-JIT target only if one exists in the tree you are handed. For the
    bench gate, the implement-phase step is mechanical: `bench-gate.sh --record` then the
    gate on the same build must exit 0; the authoritative API-I19 comparison (pre-change
    baseline recorded by the integrator) runs at integration (§6 I19). Fix failures; do not
    redesign — semantic questions are settled by §4/§6 of this spec.
14. **Finalize `docs/threads/INTEGRATE-api.md`** with the exact, build-tested manifest diffs
    from steps 1, 8, and 11.


---

# Rev 6 archive (moved out of SPEC-api.md for the 40KB cap; normative meaning unchanged, SPEC-api.md rev 6 keeps condensed forms with the same G/Dev/F/I numbers)

## Rev 6: full-text Grounding section (condensed in-spec to claims+cites)

## 1. Grounding (verified by reading; paths under `Source/JavaScriptCore/`; `wtf/`=`Source/WTF/wtf/`)

- G1: block handout unsynchronized ("FIXME GlobalGC") - `heap/LocalAllocator.cpp:138,170-171`.
- G2: 2-bit cell lock in IndexingType byte - `runtime/IndexingType.h:53,97-98,230`.
- G3: `useHandlerICInFTL` off - `OptionsList.h:638`. G4: `useSharedArrayBuffer` off - `:680`.
- G5: atomization threads an `AtomStringTableLocker`; real lock only under `USE(WEB_THREAD)` - `wtf/text/AtomStringImpl.cpp:40-64`.
- G6: atom table per WTF thread; JSLock swaps the VM's in - `AtomStringImpl.cpp:68-71`, `JSLock.cpp:124`, `VM.h:623,644`.
- G7: JSLock recursive, migrates threads, has DAL - `JSLock.h:40-50,73`.
- G8: `VMManager` stop-the-world; clients wasm + memory debugger - `VMManager.h:73,125-132,214-316`.
- G9: Atomics hosts/validators - `AtomicsObject.cpp:123,145,396-634`.
- G10: WLM process singleton, raw `void*` keys; `waitSync` re-check+enqueue+wait in ONE per-list `Locker` (wait releases it atomically); `waitAsync` = DWT promise; results via `vm.smallStrings`; Terminated -> `throwTerminationException()` - `WaiterListManager.cpp:120-145`, `AtomicsObject.cpp:443-478`.
- G11: sync-block gate `vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread()` - `AtomicsObject.cpp:459-462`.
- G12: `Atomics` installed via static table - `JSGlobalObject.cpp:428-431,729`.
- G13: `JSDestructibleObject` + `vm.destructibleObjectSpace()` - `JSDestructibleObject.h:34`, `VM.h:485`.
- G14: per-thread VM context type - `runtime/VMThreadContext.h`.
- G15: thread-prep delivered only `JSTests/threads/bench/` + `Tools/threads/bench-gate.sh`; no `baseline.json` (gate exits 2 sans `--record`), no race amplifier, no TSAN no-JIT target - `bench/harness.js:1-16`, `bench-gate.sh:13-14,33,72`.
- G16: `//@ requireOptions` works - `run-jsc-stress-tests:1029`. G17: `AtomicsObject.cpp` in `Sources.txt:765`. G18: microtask queues per-VM - `VM.h:1253`.
- G19: release generic named-read = ALWAYS_INLINE `getOwnPropertySlotImpl`; methodTable only under `OverridesGetOwnPropertySlot` - `JSObject.h:1459,1471`, `JSObject.cpp:661-669`.
- G20: no TypeInfo-flag-changing `TransitionKind` - `StructureTransitionTable.h:44-68`.
- G21: `convertToUncacheableDictionary`/`switchToSlowPutArrayStorage` public - `JSObject.h:820-821,848`; `toUncacheableDictionaryTransition` - `Structure.h:315`.
- G22: `WTF::Lock` adaptive/ParkingLot; `parkConditionally` validation runs under ParkingLot's internal queue lock - `wtf/Lock.h:75-129`, `Threading.h:149`, `ParkingLot.h:63-112`.
- G23: VMTraps event set fixed (incl. NeedStopTheWorld `VMTraps.cpp:508`), no registration hook - `VMTraps.h:149-156`.
- G24: `didAcquireLock` asserts `!stackPointerAtVMEntry()`; mid-frame relock unexercised - `JSLock.cpp:137`.
- G25: all generic-path IC flatten sites `hasBeenFlattenedBefore()`-guarded, degrade - `Repatch.cpp:348-354,619-624,1533-1537`, `LLIntSlowPaths.cpp:849-853`, `Operations.cpp:137-141`, `ObjectPropertyConditionSet.cpp:593-598`; setter public (`Structure.h:884-888,904`), bit inherited (`Structure.cpp:342`); unguarded sites only scope objects (`Interpreter.cpp:1192,1496,1684`), G29's, fuzz-gated `$vm.flattenDictionaryObject` (`JSDollarVM.cpp:3664,4527`).
- G26: `RunLoop::dispatchAfter` (`wtf/RunLoop.h:344`) fires only on a driven loop; spawned `Thread()`s never drive `RunLoop::currentSingleton()`.
- G27: `waitSyncImpl` arbitrates timeout-vs-notify in the one enqueueing LL section - `WaiterListManager.cpp:135-142`.
- G28: VM owns a runloop (`VM.h:439,1112`); DWT rides it (`JSRunLoopTimer.cpp:45`); jsc shell pumps while async work pends (`jsc.cpp:4480`, `DeferredWorkTimer.cpp:181`); `dispatch/dispatchAfter` thread-safe.
- G29: `tryInstallSpeciesWatchpoint` (`JSGlobalObject.cpp:3345`) flattens prototype+constructor unguarded (`:3359-3360,3378-3379`); init-time Array/Promise/RegExp (`:2416,2420,2433`); lazy user-triggerable ArrayBuffer/SAB (`JSArrayBufferPrototypeInlines.h:46`), TA views (`JSGenericTypedArrayViewPrototypeFunctions.h:86`); probes VMInquiry + `assertNoException` (`:3368-3369,3382-3383`).
- G30: TA sync-wait blocks WITHOUT dropping the JSLock, parking the per-VM single `vm.syncWaiter()` - `AtomicsObject.cpp:441-477`, `WaiterListManager.cpp:121`, `VM.h:1343`, `VM.cpp:1631-1633`.
- G31: VMInquiry = engine-internal probe, exception-forbidden (`disallowVMEntry`) - `PropertySlot.h:122,133-136,155`.
- G32: `hasBeenDictionary` (bit 26) survives flattening (`Structure.h:782,907`); gating restrict on it rejected (hist §2).
- G33: SPEC-objectmodel uses `useJSThreads`, retired `useConcurrentJS`, lints it absent - `SPEC-objectmodel.md:5-7,192-194,1149-1152`.


## Rev 6 compression record (2026-06-05)

SPEC-api.md rev 6 is rev 5 compressed under the hard 40,000-byte cap. ZERO normative
changes: every G/Dev/F/I/manifest/task number, every layout, signature, lock rank, frozen
error string, manifest entry, and ordered task survives. Compression devices (declared in
the rev-6 header legend): single-line paragraphs; backticks stripped outside fenced
blocks; articles and some inter-word spacing elided; abbreviations (CAE, DAL, DWT, WLM,
SVZ, UD, TM, TS, PWT, QL, LL, JSL, u/JSL, uJT(), GI, GPO, RL, prop, WS, implr, JT/, thr,
obj, wtr, tkt, INT, compl, dict, proto, ctor, exc); section references are bare numbers
(5.6 = old §5.6). Quoted JS error strings, the §5.7.3/9.2-6 frozen hook text, all C++
signatures, and manifest code blocks are byte-preserved. The rev-5/rev-6 full-text
Grounding section (G1-G33 with claim prose) is archived verbatim above in "Rev 6 archive";
SPEC-api.md keeps the complete G->file:line citation index. Long-form justifications
(lock-holder identity rationale, lost-wakeup arguments, deadlock-freedom argument,
$vm/flattening escape analysis) remain in this file's earlier rev-4/rev-5 archives, marked
"hist" at their use sites in the spec.

# Round 1 (rev 6 -> rev 7) adversarial-review resolutions (2026-06-05)

Twelve blocker/major findings were filed against rev 6. Every one was re-verified
against THREAD.md and the tree before action. Dispositions below; the spec carries
only the normative outcome, full arguments live here.

## R1-1 / R1-8 (duplicates): OptionsList comment after line-continuation backslash — REAL, fixed
Verified: FOR_EACH_JSC_OPTION entries (OptionsList.h:630-684) are backslash-continued;
phase-2 line splicing requires the backslash to be the last character; rev 6's
`v(Unsigned, jsThreadGILTimeSliceMs, 0, Normal, nullptr) \ // reserved...` truncates the
macro. Fix (9.2-1): the note moved into the option's description string
(`"reserved, inert in phase 1 (SPEC-api Deviation 9)"_s`); manifest preamble now states
"NOTHING after a continuation backslash".

## R1-2 / R1-9 (duplicates): static-table rows violate I1 flag-off — REAL, fixed
Verified: a @begin globalObjectTable row exists regardless of the callback's return, so
`"Thread" in globalThis`, Reflect.ownKeys, getOwnPropertyDescriptor all observably differ
from base with the flag off — contradicting I1 and breaking `'Thread' in globalThis`
polyfill guards. Rev 7 deletes the static-table option entirely and mandates exactly one
mechanism: guarded `putDirectWithoutTransition` in JSGlobalObject::init(), precedent
JSGlobalObject.cpp:1623-1624 (`if (Options::useSharedArrayBuffer()) putDirectWithoutTransition(...)`),
new G12. Flag off => no own property, byte-identical lookup behavior; I1 stands unamended.
Eager creation of five small objects at init when the flag is on is accepted (same cost
class as the SAB constructor).

## R1-3: QL (rank 3) acquired while m_lock (rank 4) held — REAL spec bug, fixed via exemption
The mandated 5.3/5.5a sequences do acquire QL while holding m_lock; rev 6's 5.9 said
"acquiring against rank order is bug" with no exemption, so assertion-based rank checking
would trip on the spec's own protocol. Swapping the rank labels was REJECTED: 5.9(e)'s one
permitted hold-across-GIL-reacq shape (contended hold / cond reacq / 5.5a) is keyed to the
rank-4 leaf; relabeling m_lock rank 3 would put a rank-3 lock across GIL reacquisition and
break (e). Rev 7 adds 5.9(f): QL may be taken while m_lock held; deadlock-freedom argument:
(i) every blocking m_lock acquisition (5.3 contended hold after DAL; 5.4-5 reacquire) is
made while holding no other lock; (ii) every m_lock acquisition made while any lock is held
is tryLock-only (5.5a A and P); (iii) QL is never held across any blocking operation —
hence no cycle is possible. 5.9's header now reads "against-rank acquisition=bug except (f)".

## R1-4: current-ThreadState lookup undefined; embedder threads collide at tid 0 — REAL, fixed
Verified: rev 6 had only the tid-keyed m_threads map; embedder threads all have
currentTID()==0 (frozen §7 note), so two embedder threads cannot coexist in a tid-keyed
map, and a tid-keyed lookup would also need TM::m_lock (rank 1) on the 5.8 "no locking"
path. Rev 7 (5.1) freezes the SOLE mechanism: a `static WTF::ThreadSpecific<RefPtr<ThreadState>>`
in ThreadManager.cpp — spawned bodies install their TS before fn; main/embedder threads
lazily create+install on first access (tid 0, nativeThread=&Thread::currentSingleton());
distinct embedder threads therefore get distinct TSs; identity comparisons are always
Ref<TS>/nativeThread, never tid. m_threads is annotated SPAWNED-only; lazy TSs are not in
it. 5.8 now names currentThreadState explicitly.

## R1-5: I18/blocking-gate.js "untestable" — FALSE-POSITIVE in its main claim, spec amended for clarity
The finding asserted no Option/$vm/API can flip the gate in the jsc shell. Wrong:
jsc.cpp:4281 parses shell arg `--can-block-is-false`; jsc.cpp:4439 then constructs
`SimpleTypedArrayController(false)` for the (sole, non-worker) VM, making
isAtomicsWaitAllowedOnCurrentThread() false (SimpleTypedArrayController.cpp:60-63). In the
GIL phase there is exactly one shared VM, so this makes every thread (main, spawned)
G11-false — exactly what I18's throw paths need; the async/uncontended assertions don't
consult G11 and pass on the same run. The finding was right that requireOptions() cannot
pass it (it is a shell arg, not a JSC option), so rev 7 freezes the wiring: run-tests.sh
appends --can-block-is-false for blocking-gate.js; the 9.2-7 yaml stanza must do the same.
New G34; one-line refutation note in §2.

## R1-6: join/completion handshake under-specified — REAL, fixed (new F5)
Rev 6's 4.6.1 indeed never signaled joinCondition nor settled asyncJoiners; joinLock was
absent from the 5.9 table; joinCondition.wait (sleeps holding joinLock) was not an (a)
exemption; asyncJoiners' guard was unstated. Rev 7 adds F5 (5.1): completion, after the F1
result publish and still u/JSL, takes joinLock, release-stores Phase, notifyAll's
joinCondition, swaps asyncJoiners out, drops joinLock, then settles the moved tickets via
5.5 scheduling; join() fast-paths on Phase acquire-load, else parks in a DAL scope under
joinLock with a while-Running predicate loop; asyncJoin checks Phase and appends under
joinLock. Lost-wakeup closure: Phase store and joiner re-check are both under joinLock.
joinLock = rank 3; joinCondition.wait = new exemption 5.9(a3) (WTF::Condition releases its
lock while sleeping, GIL already dropped); asyncJoiners guarded by joinLock. 4.6.1 and task
2 now reference F5.

## R1-7: 5.5a pump lost-grant interleaving — REAL, re-frozen
Confirmed the interleaving: P tryLock-fails against sync holder S; S unlocks and runs R,
sees pump-pending still true, schedules nothing; P then clears pump-pending and exits —
lock free, waiters queued, no pump pending, permanent lost wakeup once the GIL no longer
serializes P against S. Rev 7 re-freezes P as clear-then-test: under QL clear pump-pending
FIRST, drop QL, tryLock; on failure do nothing — whoever holds m_lock must release through
R, which now sees pump-pending false and reschedules. No pump-task busy loop: a failed P
schedules nothing itself; rescheduling happens only on a real release. P is marked GI.

## R1-10 (blocker): butterfly TID-tag TLS init/clear missing — REAL, fixed
SPEC-jit.md:288 (CS3, MANDATORY) requires initializeButterflyTIDTagForCurrentThread() after
TID assignment and before any JS, clear at detach; g_jscButterflyTIDTag zero-init is correct
only for the main thread (SPEC-jit.md:270 R5); grep confirmed rev 6 never mentioned it.
Rev 7 wires both calls into the 5.2 handshake (init MUST follow VMLite::setCurrent because
the tag derives from currentButterflyTID(), which reads the installed VMLite; clear right
after setCurrent(nullptr) in the completion sequence) and adds manifest entry 9.2-8, since
jit/ConcurrentButterflyOperations.h does not exist during api's implementation window.

## R1-11: VMLite lifecycle on spawned threads under-specified — REAL, fixed
SPEC-vmstate 6.5.1 (lines 506-520) mandates registerLite/unregisterLite with
unregister-before-destruction AND before teardown setCurrent(nullptr); R2/M_opts2 (lines
69, 630-632) force useVMLite on under useJSThreads, so the "without VMLite: no-op" branch
is dead in every integrated test run. Rev 7 (5.2) freezes allocation and order: the thread
body owns `makeUnique<VMLite>()` (VMLite is TZONE_ALLOCATED; vm back-pointer per 6.5.1);
spawn = construct -> tid write -> registerLite -> setCurrent -> tag init -> fn; completion =
unregisterLite -> setCurrent(nullptr) -> tag clear -> destroy, all before TID release
(5.1's recycling note updated to "5.2 teardown precedes TID release"). Per-thread
defaultMicrotaskQueue stays inert in vmstate Phase A, so 4.6 drains and the 5.5 post-GIL
inbox design are unaffected. Wiring is 9.2-8 (headers absent during implementation).

## R1-12: atom-table regime contradiction (Dev 2 vs vmstate M_opts2/M4) — REAL, reconciled
Confirmed M_opts2 lands `useJSThreads => useSharedAtomStringTable+useVMLite+
useStructureAllocationLock` in Options::notifyOptionsChanged and M4 skips exactly the
JSLock setCurrentAtomStringTable swap in shared mode. Rev 7's Dev 2 + §3 own the transitive
semantics: implement-phase local builds run the per-VM swap; the integrated build runs the
shared table. Soundness in both: under the GIL all atomization happens while holding the
JSL, so atom identity is JSL-serialized regardless of which table backs it; G6's migration
claim becomes vacuous (not wrong) in shared mode. The §8 corpus must pass in both regimes
(it is exercised in both: implementer locally, integrator at INT).

## Editorial (same revision)
Two rev-6 compression artifacts removed (duplicated " G25/G29;hist.)" fragment after Dev 8;
duplicated " G11-gated." in 4.3). Unused grounding entries G1/G2/G3/G9/G14/G17/G18 dropped
from the in-spec index (full text remains in "Rev 6 archive" above); G25/G29 secondary site
lists condensed to primaries (full lists above); G34 (jsc shell gate), G35 (SPEC-jit
P5/R5/CS3), G36 (SPEC-vmstate R2/6.5.1/6.7/M_opts2/M4) added. Heavy prose compression to
hold the 40KB cap (comma-space tightening outside code, shortened connectives); no
normative content removed — layouts, signatures, invariants I1-I22, lock ranks, manifests
1-8, and the task list are all intact, with F5, currentThreadState, 5.2 handshake order,
5.5a P re-freeze, 5.9(a3)/(f) added.

# Round 2 (rev 7 -> rev 8) adversarial-review resolutions — 2026-06-05

Ten findings filed against rev 7 (two overlapping citation findings treated as one). All
verified against THREAD.md and the tree before acting. Dispositions:

## R2-1: 5.5a A-path post-GIL lost grant — REAL (blocker), re-frozen
Reviewer interleaving confirmed against rev 7 text: A on tryLock failure only enqueued the
ticket on m_asyncWaiters under QL. Post-GIL (no GIL serialization): T2's tryLock fails
against holder T1; T1 unlocks and runs R, takes QL, sees m_asyncWaiters EMPTY (T2 not yet
enqueued), schedules nothing; T2 then enqueues. End state: lock free, ticket queued,
pump-pending false, no future m_lock release exists -> permanent lost grant with zero
contention. Exactly the bug class R1-7 fixed for P (clear-before-tryLock); A was never
re-analyzed, and rev 7 even prescribed the correct enqueue-then-pump-schedule shape for
cond.asyncWait reacquisition while omitting it from A itself. Rev 8 re-freezes A: on
tryLock failure, under the same QL section, enqueue FIFO AND (if pump-pending false) set
it + schedule the pump. Safety: a redundant pump tryLock-fails harmlessly (P, R1-7);
liveness: every enqueue now guarantees either a pending pump or a holder whose release-R
sees a non-empty queue. cond.asyncWait reacq bullet simplified to "via A's failure path
(incl. its pump schedule)" — semantics unchanged, the schedule now lives in A.

## R2-2: cond.asyncWait on sync-held lock double-unlocks via hold() epilogue — REAL
(blocker), re-frozen
Confirmed: the only sync-hold path is inside lock.hold(fn) (5.3 m_holder), and rev 7's
frozen hold sequence unconditionally ran clear m_holder -> m_lock.unlock() -> pump after
fn. 4.3(a) releases the lock inside fn, so fn's return performed a second
WTF::Lock::unlock() on a lock the thread no longer holds — possibly now held by an async
grantee via the 5.5a pump (G22: unlock-not-held corrupts/asserts). Rev 8 freezes the
chosen semantics (reviewer's option 1): asyncWait(lock) consumes the enclosing hold in
BOTH cases (a) and (b) (rev 7 said only (b)). 5.3 epilogue guard (frozen): after fn, if
m_holder == &Thread::currentSingleton() do clear+unlock+pump, else skip all three —
4.3(a) is the sole path that clears m_holder under fn, so the guard is precise. 4.2
documents that hold(fn) returns without the lock held in that case. Note cond.wait(lock)
is NOT affected: 5.4 step 5 reacquires hold-style (re-stores m_holder) before returning,
so the epilogue still sees m_holder == self.

## R2-3: Thread.restrict cannot enforce indexed GET (LLInt/DFG SlowPutArrayStorage loads)
— REAL (blocker), Dev 8/I14 re-frozen
Verified in-tree: LowLevelInterpreter64.asm get_by_val does
`bia t2, SlowPutArrayStorageShape - ArrayStorageShape, .opGetByValNotIndexedStorage`
(:1891) and falls through to the direct ArrayStorage::m_vector load for BOTH
ArrayStorageShape and SlowPutArrayStorageShape (only a hole -> btqz -> slow path), never
reaching getOwnPropertySlotByIndex; DFGArrayMode.cpp:117-129 likewise builds
Array::SlowPutArrayStorage AsIs load modes. SlowPut defeats PUT fast paths only;
structure dictionary-ness is irrelevant to get_by_val shape dispatch. So a foreign
thread's arr[0] read on a restricted array returns the value in every tier. Rev 7's I14
("full set w/ indexed") was unimplementable as specced. Rev 8 takes reviewer option (a):
cross-thread indexed GET moves to the documented-unenforced set alongside
getPrototypeOf/call/construct (Dev 8); enforced indexed ops = set/delete/define (all
funnel through putByIndex-slow/deletePropertyByIndex/defineOwnProperty generic entries,
which SlowPut + the 9.2-6 hook do cover); I14 and api/thread-restrict.js re-frozen to
test indexed set/delete/define and to leave the unenforced set untested. Option (b)
(evacuating indexed storage / forbidding indexed receivers) rejected: restricting an
array is a corpus requirement, and migrating elements to named dictionary props changes
observable semantics (ownKeys order, length coupling) for a perf-only feature.

## R2-4: frozen hook text used UNLIKELY() which no longer exists — REAL (major), re-frozen
grep confirms: no UNLIKELY( anywhere in WTF (only UNLIKELY_FOR_C_ASSERTIONS,
wtf/Assertions.h:411-412); JSObject.cpp/AtomicsObject.cpp use C++20 [[unlikely]] on the
if-statement (e.g. JSObject.cpp:528). Rev 7's frozen exact-diff hook could not compile,
and the attribute rewrite is not integrator-licensed under a FROZEN contract. Rev 8
re-freezes the hook as a plain three-conjunct condition with [[unlikely]] on the if:
`if (Options::useJSThreads() && structure->isUncacheableDictionary() &&
!threadRestrictCheck(globalObject, object)) [[unlikely]] return ...;`. Note the rev 7
text's grouping (UNLIKELY around first two conjuncts) carried no codegen semantics worth
preserving; the flat form short-circuits identically (uJT() first, flag-off pays one
predicted branch, I1/I19 unaffected).

## R2-5: Dev 2 "M4 skips the JSLock swap" stale vs vmstate rev 7 — REAL (major), fixed
SPEC-vmstate.md:133 ("Rev 7: the JSLock atom-table swap is KEPT in shared mode"),
:349-351 ("M4 MUST preserve it ... its only change = the §6.4.4 VMLite install/restore"),
M4 manifest at :655. SPEC-api R1-12 had reconciled against the older vmstate revision
(swap skipped) and was not refreshed when vmstate rev 7 reversed it. Rev 8 Dev 2 now
states: M_opts2 makes uJT() imply the three flags; M4 KEEPS the atom-table swap; only
change = VMLite install/restore. Shared-mode atomization stays JSL-serialized via the
swap, so the R1-12 soundness argument survives with the swap present rather than vacuous.

## R2-6: AsyncTicket raw "registering TS*" dangles post-GIL — REAL (major), re-frozen
4.6.2 freezes tickets as process-owned and thread-outliving; ThreadState is
ThreadSafeRefCounted and released at teardown; the post-GIL settle surface routes through
the registering TS's inbox. A raw TS* therefore settles through freed memory once the
registering thread exits. Rev 8: field becomes Ref<ThreadState> (TS always exists at
registration), and the dead-owner rule is frozen: TS carries bool inboxOpen guarded by
inboxLock; compl seq closes the inbox under inboxLock and drains residue to the main TS;
settlers finding inboxOpen==false append to the main TS inbox (main never exits). I12
updated to note the post-GIL settling thread is the registering thread, or main for
dead owners. Phase 1: inbox fields exist but are inert (shared-queue I12 relaxation).

## R2-7: ThreadState layout omitted members other frozen sections require — REAL (major),
fixed
(1) 5.10's Strong<JSThread> row had no backing field: added `Strong<JSThread> jsThread`
to the 5.1 struct (also the I5 identity carrier). (2) 5.5's post-GIL inbox (already named
in the 5.9 rank-3 list as "ThreadState inbox lock") had no fields: added
inboxLock/inbox/inboxOpen (see R2-6). (3) Clearing point for lazy main/embedder TSs
(which never run a compl seq): 5.10 row 1 now says created at first Thread.current
access, cleared at VM teardown alongside main threadLocals; ~ThreadState RELEASE_ASSERT
list extended with jsThread.

## R2-8: stale cross-spec line citations (G33/G35/G36, §7 ":757", 5.2 SPEC-heap cite) —
REAL (major), all re-pinned (two overlapping findings, one resolution)
Verified against current siblings: SPEC-jit.md is 277 lines — P5=211, R5=238, CS3=256
(G35's 240/270/288 were copied from the older jit revision quoted in R1-10; :240 lands on
R7, a different contract). SPEC-objectmodel.md is 417 lines — the no-useConcurrentJS
anchors are :5 (same-flag line) and :388 (manifest lint); :192-194 was the unrelated
cell-lock section and :1149-1152 past EOF; the currentButterflyTID concurrence for §7 is
:285-289 (":757" past EOF). SPEC-heap.md — ACT/DCT declared :244-245, bracketing/contract
note :274 (rev 7's ":314-315,872-874" pointed at the STW resume protocol and past EOF).
SPEC-vmstate.md — R2=:66-68 (kept), 6.5.1=:517-530, 6.7=:551-568, M_opts2=:647-649,
M4=:655 (rev 7's "630-640" covered M10-M12). §1 now also records the pinned sibling revs
(jit r5, objectmodel r5, heap r5, vmstate r7) and that anchor names govern if lines
drift — the reviewer-suggested anchor-first citation discipline.

## R2-9: SPEC-jit R6/CS1 "useConcurrentJS aliases" contradiction — REAL (major), but
NOT this WS's file; disposition recorded
SPEC-jit.md:239 (R6) and :254 (CS1) still say objectmodel's useConcurrentJS aliases
useJSThreads; SPEC-api §3 and SPEC-objectmodel.md:5/:388 agree no such option exists
(grep lint). SPEC-api cannot edit SPEC-jit; rev 8 records the disposition in §2 Notes
(ONE flag, no alias; "api §M" in jit:22/objectmodel:380 means api §3) and flags the
orchestrator to rewrite jit R6/CS1. The api-side contracts (single OptionsList entry,
grep lint) are unchanged and remain canonical per 9.2-1.

## Editorial (same revision)
~2.4KB of additions offset by non-normative compression to hold the 40KB cap: argument
parentheticals moved here (5.9(f) legality, OptionsList backslash rationale, static-table
rejection reason, G11-gate flippability refutation, "no edits required" rationale,
amplifier warning string shortened to "warn once", beforeSleep=[]{} elided from the 5.4
parkConditionally call — the G22-grounded signature still requires passing an empty
beforeSleep), grounding-index glosses trimmed (G12/G25/G34), §10 task parentheticals
deduplicated against their defining sections. No layouts, signatures, invariants,
lock ranks, manifest entries, or task-list items removed.

## R2-3 addendum
getOwnPropertySlotByIndex (JSObject.cpp:587) removed from the 5.7.3/9.2-6 hook entry
list: with indexed GET unenforced (Dev 8 rev 8), hooking only the generic indexed-get
entry would make cross-thread arr[i] throw CAE on slow-path shapes but not fast-path
ones — tier/shape-dependent behavior. Unenforced now deterministically means "never
throws". Indexed has falls in the same unenforced bucket (Dev 8's indexed enforced set
is set/delete/define only).

----

# Round 3 adversarial review - resolutions (rev 8 -> rev 9)

13 findings filed (3 blocker, 10 major; several duplicates). All verified against the tree; none were false positives. Every one resolved by revision. The spec was also re-compressed heavily to stay under the 40000-byte cap; no normative content was cut - only prose, provenance markers ("rev 8", "Round n"), and rationale already recorded here.

## R3-1 (blocker) - TID recycling deviation not in section 2
Real. THREAD.md:21 freezes "recycled at GC after rebias"; rev 8 only disclosed the join-completion shortcut inline in 5.1. Fix: new Deviation 10. While drafting it we found a worse latent bug: rev 8 recycled at *join completion*, but join can complete (joiner woken u/JSL) before the dying thread has run its VMLite teardown (which rev 9 moved outside the JSL, see R3-4) - a joiner could recycle a TID still carried by an installed VMLite, violating "Never recycle a VMLite-installed TID" and vmstate 6.7. Rev 9 therefore re-freezes the GPO recycle point to "dying thread returns its TID to m_freeTIDs as the LAST step of 5.2 teardown (post-JSL, after setCurrent(nullptr)); join completion never releases TIDs". This still satisfies SPEC-vmstate.md:545-548 ("safe - after the dying thread's setCurrent(nullptr)") - vmstate's parenthetical names the safety condition, not the trigger. Dev 10 also states explicitly that a still-live finished JSThread MAY observe .id reissued (I17 reissue point stays unasserted).

## R3-2/R3-7/R3-10 (major, filed 3x) - registerLite arity
Real. SPEC-vmstate.md:507 freezes `void registerLite(VMLite&, VM&)` (sole writer of VMLite::vm, asserts-null); :659-660 explicitly demanded "SPEC-api refreshes - 5.2 vm-ptr step = registerLite(*lite, vm)". Rev 8 carried the one-arg call. Rev 9: `VMLiteRegistry::singleton().registerLite(*lite, vm)` with vm = the single shared GIL-phase VM the body locks; the "(vm ptr per 6.5.1)" parenthetical (readable as "set lite->vm yourself", which would trip vmstate's sole-writer assert) deleted. The bad Dev-2 cite "SPEC-vmstate.md:349-351" (pointed into the 6.3 layout block) re-pointed to :336-337 (M4 note) and :464-478 (6.4.4).

## R3-3/R3-8/R3-12 (major, filed 3x) - run-tests.sh / yaml globs omit sibling corpora
Real. vmstate N6 (SPEC-vmstate.md:656-659) required threads/vmstate/*.js "next rev"; SPEC-jit.md:5 owns JSTests/threads/jit/** and was likewise never globbed; the rev-8 9.2-7 yaml stanza listed only api/atomics/races. Rev 9 section 8 run-tests.sh globs: JT/{api,atomics,races}/*.js + threads/heap-*.js + threads/{objectmodel,vmstate}/*.js + threads/jit/**/*.js when present (not owned), and the 9.2-7 stanza covers the same set. Section 8 "NOT owned" list now names vmstate/** and jit/** so the api implementer doesn't touch them.

## R3-4 (blocker) - VMLite install ordering vs vmstate 6.4.4
Real, and the most serious finding. Rev 8 installed the spawned lite u/JSL (after JSLockHolder). vmstate 6.4.4 (SPEC-vmstate.md:471-478) installs the VM's tid-0 main carrier in didAcquireLock when currentIfExists() is null, sets m_didInstallVMLite, and *restores the entry value (null) in willReleaseLock*. Trace under rev 8: first outermost acquire -> main carrier installed + flag set -> api overwrites current with its lite -> first DAL (any blocking primitive) outermost release -> willReleaseLock restores null, clobbering the api lite -> reacquire installs main carrier again -> spawned thread runs with tid 0 forever after: currentButterflyTID()==0 (false main-thread ownership once tagging is on - objectmodel E4/T1 unsoundness), jit I19 RELEASE_ASSERT (g_jscButterflyTIDTag == currentButterflyTID()<<48) fires at VM entry, vmstate I14/I18 violated. vmstate's own hunk comment says "lite installed pre-JSLockHolder" - the two frozen texts disagreed and api's was the wrong one. Rev 9 re-freezes 5.2: makeUnique -> tid -> registerLite(*lite, vm) -> setCurrent -> initializeButterflyTIDTagForCurrentThread, ALL BEFORE constructing JSLockHolder (registerLite needs no JSL; P5 "after setCurrent" and CS3 "before any JS" both still hold); teardown (unregisterLite -> setCurrent(nullptr) -> clear tag -> destroy lite -> release TID) moved AFTER the final JSL release, mirrored in 4.6.1. With this ordering didAcquireLock sees tid!=0, skips install, and its debug-assert cur->vm == m_vm holds (registerLite already set vm).

## R3-5 (major) - 5.4 step 5 "reacq GIL, reacq JS Lock" deadlock
Real. Literal order (GIL first, then m_lock.lock()) deadlocks: the waiter blocks on m_lock while holding the GIL; the m_lock holder is inside hold(fn) and needs the GIL to reach its release epilogue. It also contradicted 5.9(e), whose only permitted rank-4-leaf shape is m_lock held ACROSS GIL reacq. Rev 9 step 5: release queueLock; still inside step-3's DAL scope (GIL not held) tryLock then blocking m_lock.lock(); set m_holder; only then end the DAL scope so GIL reacq happens with m_lock held, per 5.9(e). No recursion/G11 check (caller held the lock at entry; G11 was checked by the original hold).

## R3-6 (blocker) - switchToSlowPutArrayStorage undecidable/crashing
Real. JSObject.cpp:2060-2101 switchToSlowPutArrayStorage has cases only for ArrayClass/Undecided/Int32/Double/Contiguous/ArrayStorage; default: CRASH(). Calling it on a blank-indexing plain object ({f:"hello"} - THREAD.md's canonical restrict example) crashes; calling it conditionally leaves the escape the reviewer described (owner adds o[0] post-restrict -> Contiguous -> foreign in-bounds indexed PUT takes the vector-store fast path, bypassing the Dev-8 hook). Rev 9 freezes the unconditional sequence: ensureArrayStorage(vm) first (JSObject.h:897 -> ensureArrayStorageSlow JSObject.cpp:1986-2025 handles ALL_BLANK via createInitialArrayStorage / sparse-dictionary path; returns null only for hijacksIndexingHeader structures), then switchToSlowPutArrayStorage (every reachable indexingType now cased - ArrayStorage shapes take the nonPropertyTransition SwitchToSlowPutArrayStorage arm), then convertToUncacheableDictionary, then the pin. hijacksIndexingHeader receivers become new Deviation 11 (TypeError, same as Dev 8 exclusions). SlowPut shape is sticky, so all later indexed PUTs (including owner-added o[0]) stay on the hooked generic paths in every tier (get_by_val side already relied on this, Dev 8). I14 + thread-restrict.js extended: restrict plain {}, owner o[0]=1, >=10^4 owner indexed-put warm-up, foreign indexed set must still throw CAE.

## R3-9 (major) - ticket keep-alive DWT mechanics unspecified
Real. WLM registers DWT pending work at *registration* (WaiterListManager.cpp:67: addPendingWork(WorkType::AtSomePoint, vm, promise, { }) in the waiter ctor), settles via scheduleWorkSoon (:287), cancels via :297-298; settle-time-only scheduling would let a never-satisfied ticket contribute nothing to shell liveness (I20 fail) since by 4.6.2 it never settles. Rev 9 5.5 freezes the protocol: AsyncTicket gains a DWT::Ticket member; addPendingWork(AtSomePoint, vm, promiseCell, {}) at registration u/JSL = the shell-liveness mechanism; scheduleWorkSoon at settle (task settles promise + clears Strong); never-settled tickets torn down by DWT's VM-shutdown cancelPendingWork(VM&) (DeferredWorkTimer.h:87) - api adds no hook. 4.6.2 now states explicitly: a forever-pending ticket (and a leaked asyncHold release fn) MAY keep the shell alive indefinitely - frozen, documented, identical to TA waitAsync with infinite timeout.

## R3-11 (major) - no mechanism for clearing main/embedder TS Strongs at VM teardown
Real. VM.h has no shutdown-observer API and 9.2 forbids VM.h/.cpp edits; the main TS lives in a static ThreadSpecific destroyed at OS-thread exit (typically after ~VM), so the rev-8 text forced an implementer to either trip the ~ThreadState RELEASE_ASSERT or destroy Strongs against a dead HandleSet. Rev 9 names the concrete hook (5.10): before creating any Strong in a lazy TS, ensure TS::jsThread exists, then register ONE vm.heap.addFinalizer(jsThread cell, lambda) (Heap.h:394-396 - public, JS_EXPORT_PRIVATE). The Strong pins the cell, so the lambda fires exactly at heap.lastChanceToFinalize() inside ~VM (VM.cpp:633) = "VM teardown"; it holds Ref<TS> and clears jsThread/threadLocals/result. If an embedder thread exits earlier, its ThreadSpecific destructor only drops the RefPtr; the lambda's Ref keeps the TS alive until ~VM, so ~ThreadState's assert holds and no Strong outlives its HandleSet. 9.2's "no VM.h/.cpp edits" note now records this ("5.10 hook=public Heap API").

## R3-13 (major) - Atomics dispatch ignores DFG/FTL untyped JIT operations
Real. AtomicsObject.cpp:641-737 defines JSC_DEFINE_JIT_OPERATION operationAtomicsAdd/And/CompareExchange/Exchange/Load/Or/Store/Sub/Xor, called by DFG/FTL untyped Atomics nodes; they funnel into the same shared helpers as the host functions (atomicReadModifyWrite(globalObject, vm, args, Func) overload at :182; atomicStore for store). Patching only the host entry points gives tier-dependent semantics: Atomics.add(o,"x",1) in hot code (races/counter-atomics.js tier-ups guaranteed) reaches the unmodified operation and throws TypeError. Rev 9 4.5 freezes placement: steps 0-3 live in the shared helpers so both JSC_DEFINE_HOST_FUNCTION and operationAtomics* route through; wait/waitAsync/notify have no JIT operation (host-only - verified by grep). property-rmw.js gains a tiered >=1e4-iteration Atomics.add(o,"x",1) loop under default JIT.

## Size-cap note
Rev 8 was at 39952/40000; round-3 fixes added ~3.6KB of normative text. The delta was paid for by prose compression across the whole document (abbreviations AO/WLM.cpp/DWT.h, provenance-marker removal, dedup of section-3 vs 9.2-1 and 5.7.1-test vs I14 text, terser G-index rows with secondary cites moved here). Full pre-compression wording of any clause is recoverable from this file's earlier rounds plus git history of the spec.

Secondary cites moved out of the spec's grounding index (rev 9): G25 also Structure.cpp:342, JSDollarVM.cpp:3664,4527; G29 also the %TypedArray% view/proto lazy-slot sites at JSGlobalObject.cpp:2416-2433/3345-3379 context. Dev 8 indexed-GET soundness cites: LowLevelInterpreter64.asm:1891, DFGArrayMode.cpp:117-129.

# Round 4 (rev 9 -> rev 10)

## R4-1 (blocker) - 5.5a pump-task success with empty m_asyncWaiters
Real. Reviewer interleaving verified against rev 9's frozen A/R/P: (1) W1 enqueued by A, pump1 scheduled (pending=true); (2) pump1 clears pending, drops QL; (3) before pump1's tryLock, the sync holder releases and its R sees queue non-empty + pending=false, so it sets pending and schedules pump2; (4) pump1 tryLock succeeds, dequeues W1 (queue now empty), grants; (5) W1's release() runs R with the queue empty - schedules nothing; (6) pump2 runs, clears pending, tryLock SUCCEEDS on the now-free lock, and rev 9's "dequeue head" hits an empty Deque. WTF::Deque::takeFirst asserts on empty (crash), and a defensive skip would return holding m_lock with no holder recorded - permanently stuck lock. Rev 9's "redundant pump harmless" claim was false and is deleted. Rev 10 P success arm: under QL, if m_asyncWaiters is empty, m_lock.unlock() and return - running R would be a no-op since the queue is empty (any future A-failure does its own schedPump), so the pump just gives the lock back. The full interleaving lives here; the spec carries "(reachable;interleaving:hist)".

## R4-2 (major) - claimed contradiction between api 5.2 and vmstate 6.4.4 install order: FALSE POSITIVE
The finding quotes SPEC-vmstate.md:469-471 as saying "api 5.2 installs the spawned lite UNDER the JSLock, after didAcquireLock". That text does not exist in the current tree. The frozen 6.4.4 block (SPEC-vmstate.md:460-480) says at :471-474, verbatim: "api rev 9 §5.2 (api:148): registerLite+setCurrent(lite) BEFORE the first JSLockHolder => spawned threads' didAcquireLock sees cur->vm == m_vm, installs nothing (m_didInstallVMLite false)". That is exactly api 5.2's order - the two specs were reconciled in round 3 (R3-4 above) and vmstate's text was updated to match; the reviewer appears to have quoted a pre-R3-4 snapshot. The only cosmetic issue was api's "(vmstate 6.4.4 requires...)" attribution reading as if vmstate forced the order; rev 10 rewords to "(=vmstate 6.4.4,:471-474;didAcquireLock sees cur->vm==m_vm,installs nothing)" - stating the consequence the suggested fix asked for explicitly. No semantic change. One-line refutation note added to spec section 2.

## R4-3 (blocker) - async-grant pump vs DWT one-shot ticket semantics
Real, and the sharpest finding of the round. DeferredWorkTimer::doWork (DeferredWorkTimer.cpp) does `m_pendingTickets.take(pendingTicket)` BEFORE running the scheduled task (:142 region) and silently skips any queued task whose ticket is no longer pending (:116 `if (pendingTicket == m_pendingTickets.end()) continue`). Rev 9 R/P scheduled pumps via "schedule (DWT) pump for head tkt": the first pump to run consumes the head waiter's dwtTicket, so (a) every later reschedule for that ticket is dropped - under any sync contention the asyncHold promise never settles; (b) the consumed ticket ends the promise's shell-liveness pin (I20/4.6.3 violated); (c) even a successful pump's settle targets the consumed ticket and is dropped. Rev 10 adopts the reviewer's option (b): pumps are scheduled via the head tkt's vm.runLoop().dispatch() (G28 - same RunLoop the DWT drains, so ordering vs settle tasks is preserved on that loop), schedPump factored out and shared by A-failure, R, and cond.asyncWait reacquisition; the registration dwtTicket is touched exactly once, by the single final settle via scheduleWorkSoon (5.5). Option (a) (run the pump inside the ticket's own DWT task, re-registering fresh tickets on failure) was rejected: addPendingWork needs the target cell and re-registration churns the pendingTickets set on every contended release, and a re-registration gap between take() and re-add would briefly drop the liveness pin. Re-arming after a failed pump is unchanged: the current holder's release runs R with pump-pending false and reschedules.

## R4-4 (major) - 5.4 step 1 takes NCS::queueLock while holding NLS::m_lock; 5.9(f) only exempted QL
Real. The cond.wait enqueue must happen before the lock release (F3 lost-wakeup argument, round 1), so the order stands and the exemption list was wrong, not the protocol. Rev 10 extends 5.9(f): with NLS::m_lock (rank 4) held one MAY take QL (5.3 pump, 5.5a A/P/E/release) or NCS::queueLock (5.4 step 1). Deadlock-freedom of the new edge: NCS::queueLock holders are (i) 5.4-1 enqueue (holds m_lock, takes queueLock - the exempted direction), (ii) notify/notifyAll and 5.4-5 re-check, which hold queueLock only across dequeue/flip/unparkOne and never attempt m_lock while holding it. So there is no queueLock->m_lock edge and the m_lock->queueLock edge cannot cycle. Reorder-and-re-derive (release m_lock before enqueueing) was rejected: it reopens the F3 lost-wakeup window the round-1 review closed.

## R4-5 (major) - Atomics.wait termination promised but unreachable in 5.6
Real. Rev 9's F4-4 parked indefinitely on a per-waiter WTF::Condition; the step-7 "termination check as TA" could never run for an infinite timeout. The TA path works because VMTraps pokes the per-VM syncWaiter condition on termination (VMTraps.cpp:329 and :419 - vm.syncWaiter()->condition().notifyOne()) and WLM's waitForSync loops on !vm.hasTerminationRequest() (WaiterListManager.cpp:86) returning WaitSyncResult::Terminated (:142). PWT waiters each have their OWN condition, VMTraps knows nothing about them, and 9.2 forbids editing VMTraps.*. Rev 10: F4-4 loop mirrors WLM.cpp:86's predicate (state!=Waiting || hasTerminationRequest || past deadline) but bounds each park at min(deadline, now+10ms) - a frozen 10ms poll quantum, the cheapest mechanism that needs no new registration machinery and no VMTraps edits; 10ms keeps termination latency well under the watchdog's resolution while a parked waiter costs ~100 wakeups/s, acceptable for a GPO section that gets re-frozen post-GIL anyway. Waiter enum gains Terminated (flip arbitration unchanged: exactly one flip, always under LL); step 5 maps termination to Terminated only after findAndRemove (Notified wins if both race, matching WLM's didGetDequeued-first logic); step 7 throws via throwTerminationException(). New I24 + atomics/property-wait-termination.js (--watchdog as the shell's termination trigger; skipped where the shell can't terminate a parked waiter); the quantum-wakeup-no-spurious-return half rides in property-wait-notify.js.

## R4-6 (major) - TS::result end-of-life and TM::m_threads erasure unspecified
Real on all three subpoints. (1) Rev 9's addFinalizer hook was scoped "Lazy-TS" and the 5.10 result row said "last JSThread finalization or TM teardown" with no mechanism and no defined "TM teardown" - an implementer had to invent the clearer for spawned threads, against a RELEASE_ASSERT in ~ThreadState. Rev 10 makes the hook universal: EVERY TS registers ONE vm.heap.addFinalizer(jsThread cell, lambda) at TS::jsThread creation (spawner under GIL for spawned; first-Strong for lazy); the lambda holds Ref<TS> and clears any still-set jsThread/threadLocals/result. JSL context argument: Weak-finalizer lambdas (Heap::LambdaFinalizerOwner::finalize, Heap.cpp:2663) run during Heap::finalize() on the collecting mutator thread holding the API lock, or at lastChanceToFinalize() inside ~VM - both satisfy 5.10's "cleared only on a thr holding JSL". Soundness of clearing result at spawned-cell death: join()/asyncJoin() require a live reference to the JSThread cell, so once the cell is unreachable no observer of result can exist. The "TM teardown" phrase is deleted from the table. (2) m_threads entry removal: rev 10 Dev 10/5.1 freeze ONE TM::m_lock critical section at the dying thread's last teardown step - erase m_threads entry, THEN push the TID to m_freeTIDs - so a recycled TID can never collide with a stale HashMap entry (the reviewer's silent-Ref-drop scenario). (3) Spawned compl seq still clears fnSlot/argSlots/threadLocals/jsThread itself (owner-thread, u/JSL); the finalizer is the SOLE clearer of result only.

## R4-7 (blocker) - unconditional switchToSlowPutArrayStorage crashes on already-SlowPut objects
Real. JSObject.cpp:2060-2101 switchToSlowPutArrayStorage cases ArrayClass, Undecided, Int32, Double, Contiguous, and NonArrayWithArrayStorage/ArrayWithArrayStorage; NonArrayWithSlowPutArrayStorage and ArrayWithSlowPutArrayStorage (IndexingType.h:106,113) fall to `default: CRASH()`. Rev 9's ":2060-2101 every reachable indexingType cased" was simply wrong (round-3 R3-6 fixed the blank-shape crash by prepending ensureArrayStorage but over-claimed coverage). Reachable triggers confirmed: owner re-restrict (4.1 idempotency re-runs the sequence on an object the first pass made SlowPut) and first-restrict of a post-bad-time array (Array.prototype pollution converts arrays to ArrayWithSlowPutArrayStorage; (a) ensureArrayStorage no-ops on any ArrayStorage so it does not save (b)). Rev 10 sequence: step (0) affinity-table hit short-circuits - owner gets o back (mechanizing 4.1's "idempotent from owner" so re-restrict never re-converts at all), non-owner gets CAE; (b) guarded by !hasSlowPutArrayStorage(indexingType()) - sound because after (a) the object has some ArrayStorage, and if it is already SlowPut there is nothing to do (R3-6's escape argument concerned skipping the conversion on NON-SlowPut shapes, which the guard does not do); (c) guarded by !isUncacheableDictionary() (convertToUncacheableDictionary on a dictionary would churn structures pointlessly; with step (0) this guard is belt-and-braces for first-restrict of objects that are already uncacheable dictionaries for unrelated reasons). I14/thread-restrict.js extended: owner double-restrict, restrict-after-bad-time.

## R4-8 (major) - asyncHold(fn) implicit-release protocol unspecified
Real. Rev 9 4.2 said with-fn settles "after release" but the release site was in no enumeration, had no mechanism, and collided with 4.3(b) (fn may consume the hold via cond.asyncWait, making a naive post-fn unlock a WTF::Lock double-unlock - G22 contract violation). Rev 10 adds 5.5a E, the exact analogue of the sync hold's epilogue guard: post-fn (same RL task that ran fn), CAS the ticket's consumed flag false->true; success => clear m_asyncHolder/m_asyncHeld under QL, unlock, run R; failure => 4.3(b) took the hold, skip unlock and R entirely, NOT an error (mirrors 5.3's m_holder==current guard, and the "outstanding release throws" clause of 4.3 applies only to the no-fn arity's handed-out release fn - E is the runtime's own release, so a consumed CAS is the expected cooperative outcome, not a contract breach). Either way the promise settles with fn's result/exc. E added to R's release-site enumeration; Dev 7 reworded (no-fn/asyncWait=explicit release fn, with-fn=implicit E). New I23 + lock-async-hold.js case: asyncHold(fn) whose fn calls cond.asyncWait(lock) - no Error, no double-unlock, later acquirers proceed.

## Rev 10 size-cap note
Round-4 fixes added ~3.0KB of normative text; rev 9 stood at 39986/40000. Paid for by: deleting redundant per-clause "(frozen)" tags (the header freezes the whole document), new abbreviations NLS/NCS/AT, deduplicating triplicated facts (TID-space map, I14 skip note, blocking-gate flag wiring, "TM sole TID allocator", caller-gating condition, GI-tests-post-GIL note), and moving rationale arguments verbatim into this file (R4-1 interleaving, R4-3 DWT-one-shot argument, R4-5 quantum justification, 5.9(f) legality, 5.7.1 guard reachability). No invariant, layout, signature, lock-order rule, manifest entry, or task was cut. Final: 39980/40000.

## Rev 11 — whole-design adversarial review round 1 dispositions

1. BLOCKER (x2) vmstate N8 — teardown order: ACCEPTED. Rev 10's 4.6.1/5.2 released the final JSL before unregisterLite; since join() results are published (and joiners woken) under that same JSL hold, a joiner could drive the embedder to ~VM while the dying thread sat between JSL-release and unregisterLite — tripping vmstate §6.4.4/I20's under-lock assert (or, without it, leaving a registered lite with a dangling vm readable by the M11 GC-marker iteration). Fix (rev 11): the completion sequence performs unregisterLite -> setCurrent(nullptr) -> clearButterflyTIDTagForCurrentThread() STILL UNDER the final JSL hold (legal: VMLiteRegistry::lock is leaf rank — see 5.9; it nests under everything), then releases JSL, then destroys the lite. TID "release" no longer exists (see item 2). vmstate r10 records N8 RESOLVED, verify-only at INT.

2. BLOCKER+MAJOR TID recycling contradiction: ACCEPTED; api yields. Old Dev 10 reissued TIDs at teardown during the GIL phase and deferred to "GC-after-rebias once tagging lands AND is active" — but OM tagging is gated on the same useJSThreads flag that enables spawning, so the reissue path was live exactly in the configuration OM ledger 8c forbids, and the stop-recycling predicate had no implementable definition (rebias is unowned). Rev 11 rule: NO reissue, ever, this milestone. m_freeTIDs stays declared but UNUSED (dead flag-on and flag-off); teardown's last TM::m_lock section only erases the m_threads entry; m_nextTID reaching 0x7fff (notTTLTID) => RangeError at spawn ("lifetime" cap of 32766 spawned Threads per process, matching OM §2's 2^15 space). I17 drops "never reissued while live (reissue point unasserted)" for "NEVER reissued"; thread-id-bounds.js asserts non-reuse after join instead of exercising reuse. vmstate §6.7 and OM 8c updated in lockstep. Rationale: a reissued TID t makes the new thread the apparent owner of every surviving (t,0)-tagged flat butterfly and apparent transition-TID of structures with m_transitionThreadLocalTID==t — silent I11/I15 violations the GIL merely masks.

3. MAJOR Atomics-on-properties post-GIL owner: ACCEPTED AS CHARTER. 5.6 is GPO by design (atomicity = JSL); the gap was that, unlike vmstate Phase B, the post-GIL re-freeze had no recorded charter, so removing the GIL would silently require reopening two frozen specs (this one's 4.5 semantics + OM §9.5, which lacks atomic slot CAS/RMW primitives). New Dev 12 + 5.6 note: the re-freeze (obj-model atomic slot CAS/RMW added to OM §9.5 at that point) is an UNOWNED future WS, chartered, with INTEGRATE recording orchestrator sign-off; OM carries the mirror entry (ledger 8g). Adding the primitives to OM's frozen surface NOW was considered and rejected jointly: they cannot be exercised or validated under the GIL, and freezing untestable concurrency primitives is how specs rot.

4. Composed flag-off gate: I1's "byte-identical to base" is now explicitly scoped to THIS WS's files, deferring to vmstate R3's composed bar (bench-noise + golden disasm modulo each spec's listed unconditional deltas — jit D7 repacks, vmstate R3(a)-(d)). api still adds zero unconditional deltas of its own; the reworded I1 is what the integrator can actually check on the composed tree.

5. Byte-cap edits: rev-10 review-log lines (R4-false-positive quote, vmstate line-cites in Dev 2/Dev 10) compressed to history pointers; teardown/recycling text net-shrank. No semantics changed beyond items 1-3.


## Whole-design adversarial review round 2 — resolutions (rev 11 -> rev 12)

1. **TID lifetime cap breaks thread-per-task processes (major, cross-cutting) — ACCEPTED.** Dev 10 rewritten: no-reissue is PHASE-1 only; GC-time rebias is chartered-owned (OM 8c r12/OM Task 13 own the restamp pass — dead-TID butterfly tags+structure TIDs restamped to 0 under the shared-GC stop; new api task 15 owns reissue). m_freeTIDs is no longer dead — it is reserved as the reissue pool (5.1 comment updated); the spawn-time RangeError now also consults it; I17 reworded ("reissued only by Dev-10 rebias"). Until rebias lands the cap stands and is documented; pooling remains the user-side workaround.
2. **Composed deliverable mis-scoped / perf milestone unowned (major+blocker, cross-cutting) — ACCEPTED.** §2 gains the normative composed-deliverable note (referenced by vmstate Dev 10): the five-spec fan-out delivers the GIL'd Thread() semantics milestone plus flag-gated infrastructure; GIL removal and near-baseline N-mutator performance are explicitly GATED on the enumerated charters (heap Dev 7; vmstate Dev 10 Phase B; OM Tasks 13-14; jit §4.3 revival), to be chartered by the orchestrator BEFORE GIL removal with INTEGRATE sign-off; the flag-on 1-thread budget is jit Task-13's <=5% composite gate. This is the in-spec re-scope the reviewers asked for (THREAD.md itself is not an owned path this round).
3. **Cap compliance:** rev-12 additions were paid by moving the §8 per-file test manifests VERBATIM to the new sub-cap `SPEC-api-annex.md` §T (FROZEN NORMATIVE; in-spec pointer remains) and trimming the review-log line. No other normative change; no API surface change.

## Whole-design adversarial review round 3 — resolutions (rev 12 -> rev 13)

1. **Binding charter list omitted Dev 12/OM 8g (major) — ACCEPTED.** Internal inconsistency confirmed: §2's enumeration claimed to bind all five specs for GIL removal, yet omitted the prop-Atomics re-freeze even though Dev 12 charters it in the same document. I6/I15/I10 are marked GI but their current implementations derive atomicity from the JSL (5.6 is explicitly GPO); shipping GIL removal against the old list would tear Atomics.compareExchange/add on properties (violating I15 and THREAD.md:5). §2 r13 adds "Dev 12/OM 8g (atomic slot CAS/RMW+PWT re-home+4.5-1a lift)" to the enumeration; OM annex §L 8g mirrors.
2. **Post-GIL execution-model gaps (two majors, cross-cutting) — ACCEPTED via §2 additions.** The charter list now also names: heap §3.8's per-thread-client post-GIL model (one GCClient::Heap per Thread — bridges heap's per-client machinery to api §5.2's single-VM Thread shape; heap owns the normative statement), and vmstate Dev 10 Phase B explicitly INCLUDING thread-granular STW (VMManager counts entered THREADS per VM; jit R1.c arbitration re-frozen there). Nothing in api's GIL-phase protocol changes: 5.2 bracketing, ACT/DCT placement, and TID notes stand; the GCClient bracketing note in 5.2 already anticipated per-thread attach/detach.
3. **TTL collapse disclosure (major, cross-cutting) — ACCEPTED as disclosure+trigger.** §2 now states plainly that until OM Task 14 lands, concurrent property adds on shared shapes are cell-locked (OM 8h), with jit Task-13's N-thread construction microbench as the charter trigger — the "near-baseline for well-behaved concurrent code" claim is thereby scoped at the composed-deliverable level rather than left implicit (THREAD.md is not an owned path).
4. **Perf-gate matrix (major, cross-cutting) — mirrored.** §2's gate citation now says two-config matrix incl. useSharedGCHeap=1 (normative text: jit Task 13; heap §3.7).
5. Cap compliance: paid by compressing the review-log pointer line and the rev banner; no semantic surface, invariant, or protocol change anywhere in §§3-10.

## Round-4 COMPOSED-design review — rev 14 (r4) resolutions

### r4.1 §3 prep (CONFIRMED bootstrap finding)
"Keep local patch until INT" collided with jit's pre-applied M1 in the SAME shared working
tree (duplicate useJSThreads lines), and the five specs carried three different conventions.
Now: 9.2-1 is orchestrator-PRE-APPLIED before fan-out (this spec's text canonical; jit §10
records the cross-spec rule); absent ⇒ STOP+escalate; NO local patch. Non-Options 9.2 hunks
the implementer needs for self-tests (notably 9.2-2 JSGlobalObject init, 9.2-4/5 build
wiring) build in a heap-§14-style private overlay worktree, never committed. Task 1 updated.

### r4.2 §2 composed-deliverable re-scope (finding: perf contract carried by charters)
The reviewer is right that the five specs deliver the GIL milestone and that every
steady-state N-mutator cost (shared-mode sync GC, single-MSPL allocation, post-fire locked
transitions, thread-granular STW, prop-atomics atomicity) lives in charters. Editing
THREAD.md is out of scope for this fan-out, so §2 — already declared binding on all five
specs — now states it EXPLICITLY: the fan-out lands ONLY the GIL'd Thread() milestone;
THREAD.md's N-mutator perf contract is phase-2; every listed charter must be chartered with
owner+frozen interface+budget BEFORE GIL removal; vmstate Dev-10 Phase B is a HARD
precondition and jit Task-13's integration gate validates the N-separate-VMs config only.
This is option (a) of the suggested fix, executed in the binding cross-spec section.

### r4.3 Wording reconciliation with OM r14 (blocker follow-through)
"Until OM Task 14, concurrent prop adds on shared shapes=cell-locked" was not what frozen OM
r13 said (E4 kept them lock-free). OM r14 adds L6/I37 (mutator transition-table lookups via
the m_lock Concurrently variant; property-table steal/walk/mutation under Structure::m_lock).
§2 now reads "cell-locked+structure-table-locked (OM 8h/L6/I37)" — the disclosure finally
matches the protocol implementers build. Gate citation updated to the heap Dev-7 split
({1,0} gated <=5%; {1,1} recorded).

### r4.4 Editorial (size cap)
§8 test-corpus conventions relocated verbatim to annex §T2 (FROZEN NORMATIVE); minor cite
compressions (heap §9, vmstate 6.4.4, DWT one-shot rationale → hist). No semantic change.
