# 4. The compiler

What changes in `aot/valueflow`, `aot/ir`, `aot/analysis`, `aot/passes`, `aot/select`, `aot/place`, `aot/represent`, `aot/codegen` and in B3's policy when an image is compiled with `AdmittedCode::Threads`.
Facts about the present compiler: `research/R2-ir-proofs-effects.md` and `research/R3-value-flow.md`. Recipes for adding an operation, a proof, a heap, an effect flag, a pass, an option: R2, section 10.

All of this is compiled only under `ENABLE(JS_THREADS)`, and in such an engine it is always on.

## 4.1 The shape of the change

The compiler has no notion of another thread. It has three things that carry the whole design:

1. **A test and its uses are tied by a value.** A proof that holds for ever is a narrowed SSA value; a proof that holds for a stretch is a `Guard` value that depends on the heaps its test declares as read (`aot/analysis/AOTGuardFacts.cpp:732`) and ends at an operation
   that suspends or whose writes overlap them (`aot/analysis/AOTGuardAvailability.cpp:43`). The verifier checks every use (`aot/ir/AOTVerifierGuards.cpp:20-71`).
2. **One set of declared effects drives everything**, from `EliminateRedundantReads` to the heap ranges that B3 sees.
3. **Every fast path has a generic path in the same function**, reached by an SSA edge.

The change has five parts:

| Part | Section |
| --- | --- |
| Flow computes who can run where and what can be shared | 4.2, 4.3 |
| Synchronization becomes an effect | 4.4 |
| Each proof gets a rule for subjects that may be shared. Proofs whose staleness is unsafe are replaced by facts about **loaded values** | 4.5, 4.6 |
| Facts of flow that encode an order in time get a side condition | 4.7 |
| A rule per pass, per B3 switch, per emitter; fences; polls | 4.8 to 4.13 |

**Principle C1.** A fact about a value that this frame holds cannot be broken by another thread. A fact about memory can. So wherever staleness would be unsafe, the design turns "test memory, then read memory" into "read memory once, then test the value".
Removing a reload is always safe; adding one never is.

## 4.2 Sharing analysis

Two results, A (which functions run where) and B (what can be shared), computed after the solve as **one least fixpoint** over one worklist. B needs A: a variable is shared if a function that runs on a thread touches it, and what it holds is shared with it. A needs nothing of B as long as every closure that native code holds is AnyThread outright (4.2.2, R5'). Any refinement that lets this depend on who can reach the closure (4.2.5) makes A need B, and then one pass of each in a fixed order gives sets that are too small: a closure that becomes AnyThread touches new variables, which hold new closures. So the step is built as a fixpoint from the start. Landing place: a new step in the DAG of `analyzeValueFlow` (`aot/valueflow/AOTValueFlowResults.cpp:403-542`), over `ValueFlowHeapIndex` / `ValueFlowHeapView`. It feeds nothing back into the solve: sharing changes how code is compiled, not what flows where.

A wrong answer is memory unsafe: Confined storage is written without a test, replaced in place and `realloc`ed, Confined layouts have packed Extensions, MainOnly variables get no release stores. So every rule below errs towards AnyThread and Shared.

### 4.2.1 What the solve has to keep

The seed set cannot be recovered afterwards ("The solve does not keep which row of the table has entered which closure", `aot/valueflow/AOTValueFlowResultsFunctions.cpp:286`). A new host node `ThreadEntries` (`ValueFlowHostNode`, `aot/valueflow/AOTValueFlowBodies.h:329-399`): in `applyCallback`, `applyHandlerCall` and `callByHost`, when the callback's record says `on: "NewThread"`, add the
closures among the callees. If the unknown (or untracked) placeholder is among the callees add every closure in `escaped` (or `lost`) and report `ThreadFunctionIsUnknown`.

A second host node `HandedToThreads` receives, at the same place: the callee closures, the values at `thisValue`, `arguments`, `spreadArguments`, what the record's `resultTo` / `thrownTo` slot can hold, and **the result object of the call that the record belongs to** (R1').

**A constructor that the solve does not see applied.** Seeds are made only where the solve applies the row. So the entry `Thread` has the flag `StartsThread`, and is treated as the evaluators are (`eventOfCalling`, `aot/valueflow/AOTValueFlowRulesHost.cpp:734-747,877-904`): it is always followed as a value, also through sets that are given up (`m_objectsAlwaysFollowed`, `aot/valueflow/AOTValueFlowSolver.cpp:170-178`). If the built-in is in `escaped` or `lost` (stored on `globalThis`, handed to unknown code, constructed through `new X(f)` where `X` holds the unknown object), or the image has `AdmitsUnknownJavaScript` (interpreted text can `import("bun:threads")` in a program that imports it), then every closure in `escaped` (and `lost`) is a thread entry, with `ThreadFunctionIsUnknown`. The solve has entered those closures with unknown operands already (`enterFromNativeCode`, `:114-152`), so the union facts about them hold.

A host flag `SomeThreadStarts` is raised with the first seed. (It is always raised in practice, since the flavour was chosen by the import. A program that imports the module and never constructs a `Thread` gets today's code everywhere.)

### 4.2.2 Result A: `FunctionFlow.threads`

```
enum ThreadSet { | engine AnyThread MainOnly }          // flow-records.def; AnyThread is the unknown/top value
```

Reachability from `ThreadEntries` over the call lists that `computeOrderResults` and `computeFunctionResults` already build (`FlowCall { caller, site, callee }`, `indirectCalls`, `functionsEnteredByHost`, callee stamps, `CallDirect`). Everything reached is `AnyThread`, the rest `MainOnly`. For each AnyThread function the step also keeps the set of `new Thread` sites from whose entries it can be reached (4.7).

Edges, beyond plain calls and callbacks that run during the call (`Once`, `PerElement`):

| Edge | Rule |
| --- | --- |
| Incomplete callee set in an AnyThread function | every closure in `escaped` (and `lost`, if untracked is present) is AnyThread |
| `Later`, `Repeatedly`, `PerEvent` callbacks that are **not** retained by native memory (timers, reactions, `queueMicrotask`: the callee lies in a native cell or a slot); coroutine resumption | the callee runs on the thread(s) of the function that made the registering call. This is sound because reactions and timers fire on the registering thread (02, section 2.6; 09, J1 to J3), or on the main thread when that thread has ended |
| Closures in `escaped` or `lost` | **AnyThread**, once `SomeThreadStarts`. Native code holds them, and some natives fire what another call has registered on the thread of **their own** caller: `process.on` models the listener at the registration (`bun:src/codegen/builtins/host/process.builtins.ts:60-70`), and `process.emit` "calls the listeners that were added", natively and synchronously, with `capability: "js"` and no callee edge (`:111-120`). `addEventListener` and `dispatchEvent` are the same pair (`bun:src/codegen/builtins/host/globals.builtins.ts:144-164,1727-1735`). This row follows from R5' |
| An entry with `fires: "Caller"` (03, section 3.5.1; 11, N-j), called in an AnyThread function | every closure that the native cell named by the row can hold is AnyThread. An `any` native that calls back what another call registered either does so on the registering thread or has this column |
| `CallByHost` records ("a call that native code makes where no call of the program is under way") | AnyThread, unless the record says `on: "Main"`. Each thread has an event loop of its own (11, section 11.4), and what a loop calls of its own accord (tick and rejection processing, `beforeExit`, warnings) runs on every thread |
| `import()`, `require()`, a module body called where it is first needed (`CallModules`) | a module's body runs on the thread that claims it: a thread that asks for it, or for a module that imports it (09, MD3''). So the body, and what only it calls, is AnyThread iff some AnyThread function can ask for a module from which it is reachable. With top-level `await` nothing changes: a waiting parent has the owner of its child (09, MD8) |
| Typed body, generic version, entrances | follow the `CallDirect` / `reach(genericVersion)` edges |
| Functions that passes derive after flow | inherit through `FlowResults::function(id)`, with the exception of the next row |
| Code that a pass **defers** (`DeferInitializers`, `DeferDefinitions`, `DeferClosuresInFields`, a class that is made at first touch) | runs at the first read (07, section 7.6), on whichever thread makes it. The pass sets the derived function's `threads` to the join over the functions that read the location. It does not defer an initializer that touches a MainOnly variable if a reader is AnyThread |
| Statements without a function (lookups that enter getters, hubs that run protocols) | AnyThread. (`isInSeenCode` answers the other way for its purpose) |
| Library functions that implement a built-in | one body for all callers, so AnyThread as soon as thread code calls the built-in. Harmless: what matters is the data |

### 4.2.3 Result B: shared locations and Shareable classes

`Shared` is the least set of abstract objects and locations closed under:

| Rule | |
| --- | --- |
| R1 hand-over | everything in `HandedToThreads` |
| **R1'** the `Thread` object | the object that `new Thread` returns is handed over too: the spawned thread reaches it through `Thread.current` (02, section 2.2). For the solve itself the constructor's row stores its result in a native cell `Thread.current`, and the getter reads that cell (03, section 3.5.2). A getter that merely "returns an instance of `Thread`" would be answered with the unknown object (`aot/valueflow/AOTValueFlowBuiltins.cpp:1759-1764`), which stands only for what has escaped: a store through `Thread.current` would never reach a read through the parent's variable |
| R2 closures | `closureOf(f)` in `Shared`: every object in `capture(f, i)` is, every environment layout that `f` or a function nested in it reads through a capture is a shared location set, and what its fields hold is in `Shared` |
| R3 heap | `o` in `Shared`: everything written under `o` for each name kind a holder can read: `EveryProperty`, `EveryGetter`, `EverySetter`, `CopiedFrom`, `PrototypeLink`, and what `runEscape` may skip but a thread cannot: `HiddenSlot`, private, brand and hidden fields, `SentValues`, `ResolvedPromises`, `RejectedPromises`, `objectsHeldFromCreation`. Not followed here: the `Slot`s of `globalThis` that are native cells (R4) |
| R4 statics | a module variable (an export with its place on the namespace object included) or the variable of a static field is a shared location iff an AnyThread function, or somebody whom the solve cannot name, can reach it **by any of the ways below**. Not "both kinds of function": two threads can run one function. A **native cell** has no holder, so the global object stands in (`aot/valueflow/AOTValueFlowBuiltins.h:56`), and it is read and written by rows, not by operations: it is a shared location iff an AnyThread function calls an entry that names it. A cell that the run time keeps per thread and hands on at `new Thread` is **one** abstract location for all threads: `$asyncContext` (09, section 9.7), whose value crosses at `new Thread` without being an operand. What a shared location holds is in `Shared`. `globalThis` and the realm's built-in objects are shared |
| R5 placeholders | `unknown` in `Shared` implies `escaped` is; `untracked` implies `lost` is |
| **R5'** the host is a channel | once `SomeThreadStarts`, `escaped` and `lost` are in `Shared` |
| R6 | an object allocated by an AnyThread function that enters `Shared` by none of the rules above is local to its thread |

**The ways to a variable (R4).** "An export is a variable and a property of the namespace object at once, and a static field is a variable and a property of the class object" (`aot/valueflow/AOTValueFlowResultsLocations.cpp:272`). So an operation that names the variable is one way among several. A store `C[k] = 5` on a thread, with `k` unknown to the build, names no variable; if the variable of `C.count` counted as MainOnly, `join` would not end the main thread's remembered read of it (4.4), and on ARM64 a main-only store of a reference into an export would be no release store (F1) although a thread reads `ns[key]`.

| Way | Data of the solve | Shared if |
| --- | --- | --- |
| an operation that names the variable: `GetModuleVariable`, `PutModuleVariable` and their kin | `locationReadBy`, `forEachLocationStoredBy` (`:72-120`), which `noteOperationsOf` applies to each reached function (`:220-237`). `Locations` keeps five event bits and a store count per location and **not** who did what (`:27-68`), so the step walks the AnyThread functions with the same two helpers | the function is AnyThread |
| an access through the class object or the namespace object that the solve has followed | `variablesOnObjects->accesses`: `{ variable, kind (Read, Observation, Store), function, site }` (`noteAccessesThroughObjects`, `:273-304`) | `function` is AnyThread, or there is none |
| everything that is "no operation that the solve has followed: evaluated text, the loader, code that nobody has solved, whoever holds a namespace object or a class object that nobody follows" | the events `ReadOutsideOperations`, `WrittenOutsideOperations` (`:27-36`). They are raised by `noteEvalScopes`, `noteOperationsOfUnsolvedCode`, `noteModules` (host reads, `require()`, exports of the library that native code reads by name, `namespaceUse == Unknown`, module objects handed out) and `noteStaticFields` (`staticFieldsInUnknownHands`, `staticFieldsOfEscapedClassObjects`, deleted, redefined, stored with another type, `staticFieldsStoredByBuiltins`) (`:259-405`) | always, once `SomeThreadStarts`: the events do not say who, and whoever it is may run on any thread. An environment field that has one of the events (a binding of an eval scope) is a shared location too; other environment fields are decided by R2 |

The third row is coarse: it makes every export of Bun's own modules that native code reads by name a shared location. That costs release stores and the tests of O3, not safety.

**Why R5' has no condition.** The host is a channel between threads: an uncaught exception or an unhandled rejection of a spawned thread is handed to handlers on the main thread, and a plain `then` handler or a `FinalizationRegistry` callback whose thread has ended runs there (02, section 2.6). What a `Later`, `Repeatedly` or `PerEvent` callback throws goes to `escaped` (`connectThrown`, `aot/valueflow/AOTValueFlowBuiltins.cpp:2145-2152`), and so does what a promise is rejected with. The receiver gets the unknown object, which stands for all of `escaped`. It need not be an AnyThread function: an object that a thread lets escape arrives at a MainOnly handler. So neither "an AnyThread function receives the unknown object" nor "an AnyThread function lets something escape" is needed as a condition; R5' covers both directions, and every `any` native that keeps an operand where another thread's call can read it.

The template is `runEscape` (`aot/valueflow/AOTValueFlowRules.cpp:105-148, 314-348`), run as a post-solve closure. `lookUpThroughInstances` shows the care needed: sharing an instance must not count as mutating its class.

Results:

| Where | Field | Granularity |
| --- | --- | --- |
| `flags ObjectMutation` (`aot/ir/schema/flow-records.def:22-25`) | `ReachOtherThread`. It flows by itself into `LayoutFlow.impossibleMutations`, `AllocationClass.impossibleMutations`, the placed layout and `LayoutData::impossibleMutations()`. `impossibleMutationBits` goes from 10 to 12 (`aot/object/AOTLayoutData.h:370`) | per layout |
| `ClassPart` of a type (as `provenPristine`) and a per-site bit vector (as `sitesOfObjectsThatNeverChange`) | `isConfined` | arrays, Maps, Sets, promises and other built-in instances: **per allocation site** |
| `AllocationClass.closureFunction` records | `ReachOtherThread` | per function's closures |
| `LocationFlow` | `threads: LocationThreads { Shared MainOnly PerThread }` | per variable, per environment field |

A layout, site or closure class without `ReachOtherThread` among its possible mutations is **Confined**; any other is **Shareable**.

A site writes the state `Confined` into the storage that it allocates only if it is Confined **and** none of its arrays reaches an operand whose type also admits a Shareable site; otherwise it writes `Owned` (06, SW13). That is for speed only: tested code that meets `Confined` storage takes the slow branch of the tag compare at each store. A site that reaches an operand use `Restricts` never counts as having a proven storage kind (06, section 6.4).

**Confined** means: no object of the class is reachable by a thread other than the one that allocated it, **while that thread lives**. A plain `then` handler that runs on the main thread because its thread has ended (09, G2) may have captured Confined objects of that thread. The end of the thread happens before the job, so the use is ordered and safe. Coroutines of an ended thread are never resumed (09, G2), so no frame carries such an object on.

Two new `FlowResultKind`s, `ThreadsOfFunctions` and `SharedObjects`, so that each can be switched off with `disable=flow:X` for bisection. Switched off means: everything AnyThread, everything Shareable.

### 4.2.4 Witnesses

For each Shareable class, each shared location and each AnyThread function the fixpoint records the edge by which it first entered its set. The chain back to a seed is the witness of `--explain=threads` (03, section 3.9).

### 4.2.5 Precision, and what to do when it is not enough

All instances of a class are one abstract object. So `Job` is Shareable as soon as one `Job` is. And everything that meets undescribed native code is Shareable, every listener that native memory retains is AnyThread, and the variables that such a listener touches are shared (R5'). That is why the rest of this document makes Shareable cheap. Refinements that are possible later and are **not** part of the first version:

- **Allocation classes per site.** Give "local `Job`" and "shared `Job`" two layouts (two shape numbers). The engine derives layouts already (`LayoutFlags::IsDerived`, `AllocateTaggedLayouts`).
- **Function versions per thread set.** A main-only copy of a function that both kinds of thread run, chosen at the call from a MainOnly function: the trick of `genericVersionOf`.
- **Escape per channel.** Native memory that the table declares per thread (`owner` classes whose firing entries are `owner` too) need not be in `Shared`.

### 4.2.6 Cost

One worklist over functions, abstract objects and locations. Each enters its set once, and each edge of the call lists and of the heap index is looked at once per entry: linear in what the solve has built. Expected well under the 3.6 s in which most threads idle during flow on the largest program.

## 4.3 Unpublished objects

**Definition.** In a function, an object is *unpublished* at an operation if it was allocated in this function (or is `this` of a constructor whose caller allocated it for this call and has not retained it), and on no path from the allocation to the operation has a retaining use occurred.
This is what `EscapeAnalysis` answers today: `retentionHistory().occurredBefore(object, operation) == HasNotOccurred` (`aot/represent/AOTRemoveRedundantBarriers.cpp:404-418`), the *confined subject* of guard availability (`aot/analysis/AOTGuardAvailability.cpp:292-347`), `stateOfNewObjectBefore` (`aot/codegen/AOTSlotAccessEmitter.cpp:896-954`), and the condition of `FinishBirths`.

An unpublished object is known to one thread. Everything that today's compiler does with it stays, whatever its class.

**Rule C2.** `new Thread`, and every native that can hand a value to another thread, declares the operand `retains`. (Checked by the native call validation, which photographs operands: `NativeCallKeptOperand`.) `Atomics.store`, `exchange`, `compareExchange` on a property are stores by name: the value operand is `retains(object)`, in the table (03, section 3.5.2) and in the row of `AtomicSlot` (4.6.6). The result of `new Thread` is published by the call itself (R1'): it is never unpublished in the caller.

## 4.4 Synchronization is an effect

A new effect flag **`synchronizes`**: "other threads' writes become visible here, and this thread's writes become visible to others".

| Place | Change |
| --- | --- |
| Schema | the word `synchronizes` in effect lists; `aot/ir/generator/parse-entries.ts:127-128`, `resolve-entries.ts:238-244`, `emit-cpp-effects.ts` |
| `Effects` (`aot/ir/AOTEffects.h:64-160`) | `bool maySynchronize : 1`; in `none()`, `unknownCode()` (true), `merge`, `intersectedWith`, `isSubsetOf`, `dump`. `EffectTraits` is full (8 of 8 bits): widen to `uint16_t`. `CompactEffects::Flag` has room |
| `interferesWith()` | `maySynchronize` interferes with every read and write of a heap that `holdsValues` or is `observable`, in both directions. It is a full barrier for the compiler |
| `mayEndProofOver()` | `effects.maySuspend \|\| effects.maySynchronize \|\| writes overlap`, with the refinement below |
| `resolveEffects()` | where `maySynchronize`: merge `summaries.otherThreads()` (below) into the normal and the handler edge |
| Visitors of `forEachDeclaredEffect` | `EffectsBuilder`, `EffectBoundBuilder`, `RowWithoutPayloadBuilder`, `SingleAccessCollector`, `ReadCollector` |
| Summaries | a function that contains a synchronizing operation synchronizes. Unknown code synchronizes |
| B3 | `applyEffects()` (`aot/codegen/AOTRuntimeCallEffects.cpp:72-117`): `reads = writes = top`, `fence = true` on the patchpoint |

The widened `EffectTraits` also takes the second new flag, `mayWaitForOtherThreads`, whose word in the schema is `waits` (rule S5, 4.11). It implies `mayCollect`, passes through the same places as `maySynchronize`, and restricts no pass beyond what `mayCollect` does. It exists so that the schema holds the closed list of operations that may park for another thread's sake.

**What a synchronizing operation clobbers.** Not everything. `EffectSummarizer::effectsOfOtherThreads()`, after the model of `effectsOfUnknownCodeAlone()` (`aot/ir/AOTEffectSummariesSolver.cpp:680-702`): writes to every object heap, restricted by the alias step to **Shareable, published** subjects; writes to the module variables and environment fields whose `LocationFlow.threads` is `Shared`. MainOnly and per-thread
variables, Confined objects and unpublished objects are untouched. In `KillQuery::mayEndProofAbout()` (`aot/analysis/AOTGuardAvailability.cpp:193-215`) a subject whose type has only Confined layouts, or that is confined in the existing sense, is not ended by `maySynchronize`.

**Which operations synchronize.**

**Rule C5.** Both ends of every edge of 02, section 2.10.2, are operations that `synchronize`, or the beginning or end of an activation. The releasing end matters as much as the acquiring one: `Select` moves the members of a group down to the group's root over everything that does not interfere (`crossesEffects`, `aot/select/AOTSelectionContext.cpp:137-152`), so a store in front of an operation that settles a promise would otherwise end up behind it.

| Edge of 02 | Releasing end | Acquiring end |
| --- | --- | --- |
| Start | the `Thread` constructor | the entry of `fn`: a new activation remembers nothing |
| End | the return of `fn` into C++ | `join`; the job of `asyncJoin`'s reaction |
| Lock | the end of `LockHold` on both edges (4.13), `release` functions, the entry into `Condition.wait` | `LockHold`, `hold`, the job of `asyncHold`, the return of `wait` |
| Atomics, Wait | `AtomicSlot` (4.6.6), the inline row of `Atomics`, every entry of `Atomics` in the table | the same |
| Reaction | `ResolvePromise`, `RejectPromise` (`aot/ir/schema/intrinsic-operations.def:33-34`), `ReturnAsyncResult`, `PromiseAllOfElements` (`aot/ir/schema/prepared-operations.def:126-127`), the resolving functions and every entry of the table that settles a promise | the start of the job; `Await` and every operation that `suspends` already end everything |
| Task | every entry of the table that posts to another thread | the start of the task |
| Module | `FinishModuleEvaluation`, `AbandonModuleEvaluation` (`aot/ir/schema/prepared-operations.def:134-135`) | `BeginModuleEvaluation` and the read of a module's state in line (09, section 9.9) |

An operation that settles a promise does not synchronize if the promise is unpublished (`FulfillFreshPromise`) or its site is Confined: nobody else has a reaction on it.

| Other operations | |
| --- | --- |
| calls of unknown code, of natives without a declaration | synchronize |
| `LoopHint`, allocation, other `collects` operations | **do not synchronize.** They end only what C4 (4.9) and 4.12 say |

A test of the schema (a new `aot/tests/AOTSynchronizationEdgeTests.cpp`) holds the table above as data and fails if a row of 02, section 2.10.2, has no operation on one of its sides, or if a listed operation lacks the flag.

The inline row of `Atomics` (`aot/ir/schema/inline-builtins.def:42`) loses `ExternalState` at `Select` today (R2, section 9.4). In mt builds the row says `synchronizes`.

**Sequences in line.** A synchronizing operation with acquire meaning also brings the thread's caches up to date (07, E2). Where the operation is a call the run time does that. Where it is emitted in line (the reading forms of `AtomicSlot`, the inline row of `Atomics` on typed arrays, the fast path of `LockHold`, the read of a module's state) the sequence ends with the compare of 07, **E2a**: load `VM::m_cacheEpoch` through the address that the view holds as a constant, compare it with the view's `cacheEpochSeen`, and on a difference call `operationAOTDidSynchronize`. One load of a line that is read-mostly, a compare, a branch that is never taken.
The start of a job or task that another thread sent is neither: the thread runs E2 when it takes jobs or tasks out of its inbox, once per batch (07, E2; 09, section 9.7).
The operation is declared `collects`, so each of these four is `collects` too, and no `window` value lives across them.

## 4.5 A rule for every proof

"Subject may be shared" means: its type admits a Shareable layout or site, and it is not unpublished here. For any other subject nothing changes.

| Proof | If stale | Rule for a subject that may be shared |
| --- | --- | --- |
| `TestedType Truthiness Equality IntegerBound ExactFunction CaseValue NotNullish` | cannot be: facts about values | none |
| `Shape`, `OwnInstance` | cannot be: the word never changes | none. Declare them `Forever` in the schema, which they are in fact |
| `SlotFilled`, plain and `holdsDeclaredType` | a marker is taken for a value: a fault for references; for integers a number outside the declared range, and ranges are published to B3 as trusted (`aot/codegen/AOTCodeGeneratorPublication.cpp:322-343`): **unsafe** | **Replaced** for all its users (rule C3, 4.6.1): `GetSlot`, `GetSlotGetter`, `GetDeferredClosureSlot` become terminators that load once and test the loaded word. `GetFlattenedSlot` and `GetFlattenedSlotOrUndefined` exist only in Confined outer layouts (4.8). `IsSlotFilled` alone stays for `in` and `hasOwn`, where the answer is only an answer |
| `BirthFieldsFilled` | the same, for several slots at once | **Not used** to stand for `SlotFilled`: `isAnsweredByTestOfCell` (`aot/analysis/AOTGuardFacts.cpp:643-655`) answers no for a Shareable layout. Each read of a slot that has markers tests its own load |
| no test, `keepsStateOfBirth` | the run time aborts | allowed iff the order fact behind it survives 4.7 |
| `NoExtraProperties`, `NoFieldOutsideSlot` | an old answer for a name that had no property. But a declared field that is moved out while the guard is held leaves `absent` in its slot, and `undefined` was never its value (02, M3) | stays a stretch; ends at `synchronizes`. `HasNoFieldOutsideSlot` tests bit 27 of the order word, never the mark of the cell (05, CO15). A read that would answer `undefined` because it has loaded `absent` tests bit 27 again, behind the load: `TryGetSlotOrUndefined`, `TryGetDeferredClosureSlotOrUndefined` (4.6.1). Stores: 4.6.2 |
| `NotFrozen` | a store lands after a freeze | stays; ends at `synchronizes` (02, M9) |
| `KeyOrder` | an old key list | stays; ends at `synchronizes`. Reads of slots under it are `TryGetSlot` with the generic path as `empty` target |
| `EnumeratorCurrent` | a cached position is used on a changed object | `EnumeratorGetByVal` makes its own test in the same operation: load StructureID (acquire), compare, load. No reliance on an earlier guard |
| `PrototypeLinkUnchanged`, `MemberUnchanged`, `EvaluatedOnce`, `UnmodifiedGenerator`, `NotThenable`, `Pristine(Names, NamesKeptBelow, AsCreated)` | the old member or protocol answer is used once more | stays; ends at `synchronizes`; F1a (07, section 7.5), below |
| `MemberUnchanged` in front of the **variable of a static field** (`finishAtStaticField`, `aot/analysis/AOTPropertyAccessStaticMembers.cpp:62`; `planVariableRead`, `planVariableStore`, `aot/analysis/AOTPropertyAccessSlots.cpp:558-589`) | `delete` and redefinition empty the variable (`aot/object/AOTStaticFields.cpp:135-170`): a marker is taken for a value, a late store refills the variable: **unsafe** | the guard stays, as the answer to "which location". The access is `TryGetVariable` or `TryPutVariable` (4.6.1, 4.6.2; 05, CO11 to CO13) wherever the variable `canLeaveSlot()` |
| `Pristine(Storage, Dense)`, `ArrayStorage`, `InBounds` (arrays) | out-of-bounds access, raw double taken for a pointer: **unsafe** | **Replaced** by facts about a loaded `Storage` value (4.6.3) |
| `InBounds` (typed arrays, `IsInsideView`) | access to freed or shrunk memory: **unsafe** | **Replaced** (4.6.4) |
| `PlainData` | its users are calls of the run time | as safe as the run time (05, 06) |
| `SourceOpen`, `TextMatches` | `ParserState` is one per Instance | `JSONWords` lie in the private part of the view (07). Nothing else changes |

The sets in `aot/analysis/AOTGuardFacts.cpp` (`proofsOfNew*` 33-43, `proofsDecidedByFlow` 52-63, `monotoneObjectProofs`) keep their members. What an allocation proves holds while the object is unpublished, which is when the compiler uses it.

**Proofs decided by a flow record** (`proofsDecidedByFlow`): order-free records (`FieldStability`, `impossibleMutations`) stand as they are. Records that rest on order: 4.7.

**What a stale guard may carry.** By rule F1a (07, section 7.5) no fact that a representation, or a range published to B3 (`trustsKnownRanges`), rests on is derived from a guard that may be stale. The code behind a passed `MemberUnchanged` or `PrototypeLinkUnchanged` relies only on the receiver's shape, which is tested apart and constant, and on facts of flow that do not depend on the guard. The verifier refuses a value that was **loaded from memory** behind such a guard, whose type is narrower than what the location's own flow record gives, and that is used unboxed or at a proven layout without a test of the value. (The order in which the run time clears the bit of a guard set and the bits of its members: 07, section 7.5.)

**Direct forms of natives** (`directEntry`; 11, N-l) take their operands untested, on the strength of the caller's proofs. They may rest only on proofs that the table lists as facts about values or as for ever (`TestedType`, `Shape`, `StorageKind` of a loaded `Storage`). A stretch proof about a subject that may be shared is tested again inside the native.

## 4.6 New operations

### 4.6.1 `TryGetSlot`

**Rule C3.** Every read of a slot or a variable that has markers, of a subject that may be shared, is a terminator: one load, a test of the loaded word, an edge for the word that is no value. The check is structural: the verifier rejects every *operation* of the family `SlotAccess` with `access = Read` whose slot has markers and whose subject may be shared, whatever it relies on (`InternalErrorKind::SlotFilledOnSharedSubject`). The one exception is `GetSlotOrUndefined` on a field that cannot be moved out (below). Which rule covers each operation of the schema that reads memory: 4.6.7. No pass turns such a terminator back into a read without a test; it may only replace it by a value that was tested or stored (below).

```
family SlotAccess Prepared..Placed
terminator TryGetSlot (object: Object/inspects, guard?: Guard/inspects, with SlotStamp) [holdsDeclaredType]
    -> filled(value: declared(key)), empty ; reads(ObjectFields[key]) ; access = Read ; relies = Shape ; guarded = object ; replaces = IsSlotFilled, GetSlot
terminator TryGetSlotOrUndefined (object: Object/inspects, guard?: Guard/inspects, with SlotStamp)
    -> value(value: declared(key)), outside ; reads(ObjectFields[key], OrderWords) ; access = Read ; relies = Shape ; guarded = object ; replaces = GetSlotOrUndefined
terminator TryGetSlotGetter (object: Object/inspects, guard?: Guard/inspects, with SlotStamp)
    -> filled(getter: declared(key)), empty ; reads(ObjectFields[key]) ; access = Read ; relies = Shape ; guarded = object ; replaces = IsSlotFilled, GetSlotGetter
terminator TryGetDeferredClosureSlot (object: Object/inspects, guard?: Guard/inspects, with SlotStamp) [holdsDeclaredType]
    -> filled(value: declared(key)), empty ; Allocates reads(ObjectFields[key]) writes(ObjectFields[key]) ; access = Read ; relies = Shape ; guarded = object
    ; runtime = MaterializeDeferredClosure ; replaces = IsSlotFilled, GetDeferredClosureSlot
terminator TryGetDeferredClosureSlotOrUndefined (object: Object/inspects, guard?: Guard/inspects, with SlotStamp)
    -> value(value: declared(key)), outside ; Allocates reads(ObjectFields[key], OrderWords) writes(ObjectFields[key]) ; access = Read ; relies = Shape ; guarded = object
    ; runtime = MaterializeDeferredClosure ; replaces = GetDeferredClosureSlotOrUndefined
terminator TryGetVariable (variable: Variable)
    -> filled(value: declared(variable)), empty ; reads(ModuleVariables[variable]) ; replaces = GetModuleVariable
```

**`TryGetSlot`.** One load of the slot's width. `filled` receives the value, decoded; its type excludes `absent` (with the flag: also `undefined` and `null`). `empty` goes where the failed `IsSlotFilled` went: the generic version.

- Emitter: `SlotEmitter::holdsValue` / `holdsDeclaredType` applied to the loaded word, not to memory. For `OptionalDouble` the test is on the upper half of the loaded 64 bits (today: a separate 32-bit load, `aot/codegen/AOTSlotEmitter.cpp:475-499`).
- Same instruction count as test-then-load: load, compare, branch.
- `ResolveAccesses` and the access plans (`aot/analysis/AOTPropertyAccess*.cpp`) emit it where the receiver may be shared and the slot has an absent state (or, with the flag, any marker). Where the slot has none, `GetSlot` stays: there is nothing to test.
- `EliminateRedundantReads` may forward an earlier `filled` value, or a stored value, to a later read of the same location. Then no test is needed: the value was tested or is known.
- Out-of-line slots: 05, section 5.5.

**`TryGetSlotOrUndefined`.** `GetSlotOrUndefined` folds test and read into one load and selects `undefined` for `absent` (`aot/codegen/AOTSlotEmitter.cpp:397-424`). It stays for a field that flow proves `IsNeverRedefined` and `IsNeverStoredWithOtherType`: there `absent` means "deleted or not yet created", and `undefined` is the answer. For a field that can be moved out the terminator is used. The path that finds a value is the same. The path that finds `absent` loads the order word (the load of the slot is an acquire on ARM64, F3) and takes `outside`, to the generic version, if bit 27 is set; else `value` receives `undefined`.
The mover sets bit 27 before it empties the slot (05, section 5.6 a), so a reader that sees `absent` sees the bit. While bit 27 is clear no extra property has the name of a field (`aot/object/AOTObjectKeyOrder.cpp:188`), so bit 28 need not be looked at. `loadDeferringSlotOrFieldOutsideIt` (`aot/codegen/AOTSlotAccessEmitter.cpp:520-538`) has this form today.

**`TryGetSlotGetter`.** As `TryGetSlot`; `filled` receives the getter that was read out of the loaded `GetterSetter`. "Nothing writes the words of a GetterSetter once it is made" (`aot/codegen/AOTSlotAccessEmitter.cpp:300`).

**`TryGetDeferredClosureSlot`.** The word has four kinds of state: 0 "not made yet", 1 `absent`, the markers of `undefined` and `null`, a reference. One load. A reference (without the flag: also a marker) goes to `filled`; 1 goes to `empty`; 0 calls `operationAOTMaterializeDeferredClosure`, which publishes by compare-and-swap and starts again from the load if it loses (07, Z1). The operation can therefore come back with "no closure": another thread has emptied the slot meanwhile. It returns null then, and the emitter tests the result and takes `empty`.
Today every word `<= 1` goes to the operation and its result is decoded untested (`loadDeferringSlot`, `aot/codegen/AOTSlotAccessEmitter.cpp:483-502`; `aot/object/AOTDeferredClosures.cpp:87-88`).

**`TryGetDeferredClosureSlotOrUndefined`.** The plans choose `GetDeferredClosureSlotOrUndefined` wherever a slot that defers its closure may be empty (`readOfSlotThatMayBeEmpty`, `aot/analysis/AOTPropertyAccessSlots.cpp:364`). For a subject that may be shared they choose the terminator. One load. A reference or a marker: `value`. 0: the operation; a null result is treated as the word 1. 1: load the order word (the load of the slot is an acquire on ARM64, F3); bit 27 set: `outside`, to the generic version; else `value` receives `undefined`. The form without the look at bit 27 (`mayHaveLeftDeferringSlot`, `aot/codegen/AOTSlotAccessEmitter.cpp:505-518`) is used only for a field that cannot be moved out, never on the strength of a `NoFieldOutsideSlot` guard. Both emitters of today (`:483-502`, `:520-538`) test the operation's result before they decode it.

**`TryGetVariable`.** For the variable of a static field that `canLeaveSlot()` (4.5). `empty` goes to the access by name on the class object. Any other variable whose type keeps `absent` (a dead zone; rule O3 of 4.7) is read by `GetModuleVariable` followed by `CheckInitialized`, which is a test of the loaded value already.

### 4.6.2 Stores into slots

| The field | Store into a published object of a Shareable layout |
| --- | --- |
| cannot leave its slot, or flow proves `IsNeverDeleted`, `IsNeverRedefined`, `IsNeverStoredWithOtherType` (then nobody ever **writes** `absent` into the slot) | `PutSlot`, a plain store (release if a reference on ARM64, 4.10). The common case in typed code. The slot may hold `absent` from birth: `PutSlot` is also the creating store of a lazily initialized field in a layout whose keys are never listed (`creatingStoreFor`, `aot/analysis/AOTPropertyAccessSlots.cpp:315-318`). That is a plain store over `absent`, and harmless: there is no mover whose work it could undo, and two creators race like two stores (05, CO3) |
| can leave its slot | `TryPutSlot`: load the word; if it is `absent` take the edge `empty`, to the generic path; else **compare-and-swap** the loaded word for the new one; on failure repeat from the load. A fast path never overwrites an `absent` that a mutation may have written. So a field that was deleted, moved out or redefined under the object lock cannot be refilled by a thread that holds an old guard (05, CO3) |
| may have to be created (`PutSlotOrAdd`, `PutSlots` that list keys) | unpublished: as today. Published: the stub takes the object lock (05, section 5.3) |

```
terminator TryPutSlot (object: Object/inspects, guard?: Guard/inspects, value: Declared/retains(object), with SlotStamp)
    -> stored, empty ; reads(ObjectFields[key]) writes(ObjectFields[key]) ; access = Store ; stored = value ; relies = Shape, NotFrozen ; guarded = object
    ; runsOwnerBarrier = true ; replaces = PutSlot
terminator TryPutVariable (variable: Variable, value: Declared/retains)
    -> stored, empty ; reads(ModuleVariables[variable]) writes(ModuleVariables[variable]) ; replaces = PutModuleVariable
```

It is a terminator because an `operation` has no edge to leave by. The barrier runs on the edge `stored`. `memoryAccessOf()` (`aot/ir/AOTMemoryLocationAccesses.cpp:88-130`) does not take it for an exact write, since it reads too: to `EliminateRedundantReads` it is a writer of the location that defines nothing. (Forwarding the stored value along `stored` alone is legal and can be added later.)

`TryPutVariable` is the same for the variable of a static field that `canLeaveSlot()` (4.5; 05, CO12). `empty` goes to the store by name on the class object.

In a `DeferredClosure` slot the word 0 ("not made yet") is not `absent`: a store replaces it like a value.

B3: `AtomicStrongCAS` of width 8, 32 or 64. (`OptionalBoolean` is the only representation below 4 bytes that has markers; the 2-byte representations have none and always take the first row.) ARM64 needs LSE (`cas*`) or an `ldxr/stxr` loop; the assembler has both for WebAssembly. B3's atomics are fully fenced, so the compare-and-swap is also the release store of F1.

**Stubs** that store by name into a filled slot follow the same table: 05, section 5.4.

Storing `undefined` or `null` as markers is a store of a value, not of `absent`: it follows the first or second row like any value. The mark of the cell is neither read nor written for Shareable layouts (05, CO15): `markCellIfMarkerReplacesRawValue` is not emitted.

### 4.6.3 `Storage` values for arrays

```
opaque Storage engine functionLocal interior window        // `window`: may not be live across an operation that collects
enum Proof { .. StorageKind(Forever, false) InsideStorage(Forever, false) WritableStorage(Stretch, false) OwnedStorage(Stretch, false) BelowLength(Stretch, false) }

family StorageAccess Prepared..Placed
operation  LoadStorage       (array: Array/inspects) -> natural(Storage) ; reads(ArrayStorage)
operation  HasStorageKind    (storage: Storage/inspects, arrayStorage: ArrayStorage) -> natural(boolean) ; none ; proves = StorageKind
operation  IsInsideStorage   (storage, position: Integer/inspects) -> natural(boolean) ; none ; proves = InsideStorage             // position < vectorLength
operation  IsBelowLength     (storage, position) -> natural(boolean) ; reads(ArrayStorage) ; proves = BelowLength                  // position < publicLength
operation  MayWriteStorage   (storage, arrayStorage, state: Owned | Shared) -> natural(boolean) ; reads(ArrayStorage) ; proves = OwnedStorage | WritableStorage   // one tag compare: wr2 or wr3 of 06
terminator AcquireStorage    (array: Array/inspects, storage: Storage/inspects, arrayStorage) -> writable(storage: Storage, proof), miss ; reads(ArrayStorage) writes(ArrayStorage) ; proves = WritableStorage, StorageKind
operation  GetStorageLength  (storage, guard?: Guard) -> natural(int32) ; reads(ArrayStorage) ; relies = StorageKind if the array's sites reach `Restricts`
operation  SetStorageLength  (storage, length: Integer/inspects, guard: Guard) [raises] -> none ; writes(ArrayStorage) ; relies = WritableStorage, with the flag OwnedStorage      // store32
operation  RaiseStorageLength (storage, length, guard: Guard) -> none ; reads(ArrayStorage) writes(ArrayStorage) ; relies = WritableStorage                                     // raise(n) of 06
terminator TryLowerStorageLength (storage, expected: Integer/inspects, length, guard: Guard) -> lowered, changed ; reads(ArrayStorage) writes(ArrayStorage) ; relies = WritableStorage   // cas32, for pop
terminator TryGetElement     (storage, position, guard: Guard, arrayStorage) -> present(value), hole ; reads(Elements[Array]) ; relies = StorageKind, BelowLength
operation  PutElement        (array: Array/inspects, storage, position, value: Declared/retains(array), guard: Guard, arrayStorage) -> none ; writes(Elements[Array]) ; relies = StorageKind, InsideStorage, WritableStorage ; runsOwnerBarrier
```

`PutElement` names the array beside its storage: the barrier is the owner's, and for the escape analysis the value is retained by the array, not by a pointer that dies at the next safepoint. The verifier checks that `storage` comes from a `LoadStorage` or an `AcquireStorage` of the same `array` value.

Why each lifetime is right (invariants of 06, section 6.2):

- `StorageKind` is for ever: the kind of a storage is written once, before the storage is published.
- `InsideStorage` is for ever: the vector length of a storage is constant (06, SW2).
- `BelowLength` may be stale. That is harmless: `publicLength <= vectorLength` holds for each storage at all times (06, SW3'), so a stale answer leads to a slot inside the allocation, which holds a value of the storage's kind or a hole (SW4). `TryGetElement` tests the loaded word for the hole. **`Dense` never removes that test** for an array that may be shared.
- `WritableStorage` can be undone only by this thread's own operations on the array (declared `writes(ArrayStorage)`) or in a safepoint operation. A `Storage` value does not live across a safepoint.
- `OwnedStorage` implies `WritableStorage`. It **can** be undone by another thread, which makes the storage `Shared` at any moment. That is harmless: all it licenses is a plain store that raises the length where `Shared` storage asks for `RaiseStorageLength`. A plain store that comes late can undo another thread's raise, which is a race on `length` in the program, and leaves a value above the length, which SW3' and SW10 allow.

- `GetStorageLength` needs no kind for memory safety: the length lies at the same place in every kind. But the length of a restricted array may be read by its owner only (02, section 2.7), and the kind byte `Restricted` is the only thing a fast path can miss on (06, section 6.4). So for an array whose sites reach the operand use `Restricts` the read of `length` stands behind `HasStorageKind` like a read of an element.

Memory safety of an element access therefore rests on three facts about one loaded pointer, and on nothing that another thread can change.

**The two columns of a store.** The sequences of 06, section 6.3.3, differ in how they write the length. The plan makes `MayWriteStorage [Owned]` (wr2); on its failing edge `MayWriteStorage [Shared]` (wr3); on that one's failing edge `AcquireStorage` (wr4). The 32-bit compare holds the kind too, so a test that passes also proves `StorageKind`. Behind the first test stands the column Owned (`SetStorageLength [raises]`, and in `pop` a plain `SetStorageLength`). Behind the second, and behind the edge `writable`, stands the column Shared (`RaiseStorageLength`, `TryLowerStorageLength`), whose atomic forms are right for every state. Lowering the length by `a.length = n` is a plain `SetStorageLength` in both.

**The slow branch of a store.** `MayWriteStorage` is a pure test, a tag compare of 06, section 6.3.2. Where both fail the plan goes on with `AcquireStorage`, the stub `acquireStorageForWriting(a, b, kind)` of 06. It writes the storage word (`Owned(other)` to `Shared` by compare-and-swap), or waits out a `Retired` storage and loads the array's pointer again. So it yields a **new** `Storage` value on the edge `writable`, together with the proof. What was proven of the old value (`StorageKind`, `InsideStorage`, `BelowLength`) says nothing of the new one: the stub compares the kind of the storage it answers with against `arrayStorage`, and the plan tests the bounds again behind the edge.
It answers `writable` for `Confined` storage too (06, section 6.2).
The stub allocates nothing and contains no safepoint, so the terminator is not `collects`. `ReadOnly` storage, another kind, a `Retired` storage whose successor does not appear within the stub's turns, and every case that needs an allocation leave by `miss`, to an operation that is declared `collects` and `waits` (it may run a safepoint operation; S5), or to the generic version. So the first store of a second thread costs one compare-and-swap, not the rest of the activation in generic code.

The plans that make `HasArrayStorage`, `IsInBounds`, `IsElementPresent`, `GetArrayElement`, `PutArrayElement`, `GetArrayLength`, `AppendElements` make the operations above when the array may be shared. For a Confined or unpublished array they make what they make today. `push`, `pop`, `shift`, `length =` in line: 06, section 6.3.

Dense Int32 arrays: neither the 32-bit store of the low half (`aot/codegen/AOTElementAccessEmitterStores.cpp:195-206`) nor the 32-bit load of the low half is used. The hole is 0 in all 64 bits and the integer 0 has a zero low half, so only a 64-bit load can tell them apart.

**Verifier.** A value of an opaque type marked `window` must not be live across an operation whose effects say `mayCollect`, on the normal or the handler edge. The check has a model: narrow references (`isReachedBeforeCollectorCanRun`, `aot/represent/AOTRepresentationWebs.cpp:496-515`). New `InternalErrorKind::StorageLiveAcrossSafepoint`. This is rule S1 of the overview, made mechanical.

A pass that would extend the life of a `Storage` value over such an operation (hoisting out of a loop with a poll, merging two loads over an allocation) finds `LoadStorage` ended there instead: rule C4 (4.9).

### 4.6.4 Typed arrays

The same idea with one more word. `LoadView (view) -> natural(ViewStorage)` loads `{vector, length}` under the protocol of 06, section 6.8, as two values of `window` types; `IsInsideLoadedView (length, position)` proves `InsideStorage`; the access uses the loaded vector. Detached or shrunk memory stays mapped until every thread has passed a safepoint, so an access through a loaded pair is safe inside its window.

A call of a native that `blocks` releases heap access, and a thread without access counts as being at a safepoint. If such a native takes a buffer or a view as an operand the call emitter pins the `ArrayBuffer` around the call (08, H11; 06, TA9).

### 4.6.5 Extensions

```
operation LoadExtension   (object: Object/inspects, guard?: Guard/inspects) -> natural(Storage) ; reads(ObjectFields) ; relies = Shape
operation EnsureExtension (object: Object/inspects, guard?: Guard/inspects) -> natural(Storage) ; AllocatesWithoutCollecting reads(ObjectFields) writes(ObjectFields) ; relies = Shape ; runsOwnerBarrier = true
```

Then `TryGetSlot`, `PutSlot`, `TryPutSlot`, which take the `Storage` as one more operand, at `storage + offset`. The Extension of a Shareable layout is unpacked (05, section 5.5), so there is no test of a granule. `LoadExtension` may yield the page of zeros; a store takes its `Storage` from `EnsureExtension`, which tests the hidden word, allocates, publishes by compare-and-swap, runs the owner barrier (05, CO6), and yields the Extension that is there afterwards, its own or the winner's.
Both name the whole heap `ObjectFields`, as the load of the hidden word does today (`aot/codegen/AOTSlotEmitter.cpp:228-232`): whatever creates a field may create the Extension.

### 4.6.6 Atomic operations on slots

```
terminator AtomicSlot (operation: AtomicsOperation, object: Object/inspects, guard?: Guard/inspects, with SlotStamp, operand?: Declared/retains(object), expected?: Any/inspects)
    -> done(result: declared(key)), miss ; reads(ObjectFields[key]) writes(ObjectFields[key]) synchronizes collects ; relies = Shape ; guarded = object ; runsOwnerBarrier = true
```

Emitted for `Atomics.*(o, "k", ..)` where the receiver's layouts and the key are known, the slot is in line, and its representation is `JSValue`, `Cell32`-like, `Int32`-like or `Double`-like. `miss` goes to the call of the table's entry, `operationAOTAtomicsOnProperty`, which throws what 02, section 2.5, says. The inline form throws nothing.

An atomic operation is a fast path like any other: it keeps CO2 and CO3 of 05. So **every form is built on the loaded word**:

| Step | |
| --- | --- |
| 1 | `w` = sequentially consistent load of the slot (F5) |
| 2 | `w` is `absent`: `miss`. (The property does not exist, or it lives outside its slot.) `w` is the marker of `undefined` or `null`: a value for `load`, `store`, `exchange`, `compareExchange`; `miss` for the arithmetic forms |
| 3 | compute the new word `n` in registers. A result that the slot cannot hold (an integer outside the representation's values, which for `OptionalInt32` begin at `INT32_MIN + 5`; a number that is no integer in an integer slot): `miss`. Nothing has been written |
| 4 | compare-and-swap `w` for `n`, sequentially consistent. On failure: step 1 |
| 5 | owner barrier if `n` may be a cell; the compare of E2a (4.4); `done` receives the result, decoded from `w` (or the operand, for `store`) |

| Form | Steps |
| --- | --- |
| `load` | 1, 2, 5 |
| `store`, `exchange` | all; `n` is the operand |
| `compareExchange` | all. SameValueZero between `w` and `expected`: on an integer slot a compare of integers; on a `Double` slot a compare of bit patterns after canonicalizing zero (every NaN in a slot is the pure one); on a `JSValue` slot equal bits are equal, two words of which neither is a number, a string or a BigInt are unequal, everything else is `miss`. Unequal: 5, without a write |
| `add`, `sub` | all. Integer slot: 64-bit arithmetic, then the range test of step 3. `Double` slot: double arithmetic, NaN purified |
| `and`, `or`, `xor` | all. `ToInt32` of both in registers |

An instruction that modifies memory unconditionally (`lock xadd`, `ldaddal`, `xchg`, `swpal`, a fenced store) has changed the slot before anything can be tested: it would turn a marker into a number, a number into a marker, or refill a slot that a `delete` has emptied. It is used only where none of that can happen: the representation has no markers, the field cannot leave its slot, and the operation cannot leave the representation's values. That is `store`, `exchange`, `and`, `or`, `xor` on plain `Int32`, and `store`, `exchange` on plain `Double`.

**One protocol per slot.** The same property is reached in line, through `miss`, from a site whose key or layout is not known, from interpreted code and from natives. All of these are `Atomics`, so a program that uses nothing else on the property has no race, and each operation has to be one atomic step against all the others (02, section 2.5). A lock that only some of them take orders nothing between the two groups. So `operationAOTAtomicsOnProperty`, on a property that lives in a slot (in line or in the Extension) or in the variable of a static field, **runs the same five steps on the same word**: the word that it swaps from is the word that its result was computed from. `Object::tryStore`, which loads for itself, is not used for a read-modify-write.

| The property | What the operation in C++ does |
| --- | --- |
| in a slot or a variable; reading form | steps 1, 2, 5. No lock |
| in a slot or a variable; writing form; the layout's integrity cannot change | the five steps. No lock |
| in a slot or a variable; writing form; any other layout | the object lock, a look at the integrity bits, the five steps, unlock. The lock is there for `freeze` (05, section 5.6 d), the compare-and-swap for the forms that take no lock |
| step 2 finds `absent`; an extra property; a property of an ordinary object; a property that `store` has to create | the object lock for the whole read-modify-write. No inline form reaches such a property, and every `Atomics` operation on it comes this way |

A step 2 that finds `absent` goes on under the lock with a second look at the slot (05, section 5.4), so an operation that meets a mover waits for it.

02 promises that no atomic write lands after a `freeze` has returned. So a writing form is emitted in line only for layouts for which flow proves `ChangeIntegrity` impossible. For any other layout it is the call.

To flow an `Atomics` operation on a property is the access by name that it is: the value enters the field's type, `store` can add a property, the arithmetic forms store a number. The rows of the table say so (03, section 3.5.2), and the operand is `retains(object)` (C2).

### 4.6.7 Every operation that reads a slot, a variable, an Extension or storage

The lists are the schema's (`aot/ir/schema/*.def`): each operation, terminator and inline row whose effects read `ObjectFields`, `ModuleVariables`, `EnvironmentFields`, `ArrayStorage`, `Elements`, `TypedArrayStorage`, `TypedArrayElements`, `CollectionStorage` or `CollectionEntries`, and those that load such memory under the effect `none`. The right column says what holds **for a subject that may be shared**, or a location with `threads = Shared`; for any other nothing changes.
A test of the schema (new: `aot/tests/AOTSharedReadCoverageTests.cpp`) holds these tables as data. An operation that reads one of the heaps and stands in no row fails the build, so an operation that is added later cannot go unnamed.

**Slots and Extensions of objects with a layout.**

| Operation | Rule |
| --- | --- |
| `GetSlot`, `GetSlotGetter`, `GetDeferredClosureSlot`, `GetDeferredClosureSlotOrUndefined`, on a slot with markers | not made. `TryGetSlot`, `TryGetSlotGetter`, `TryGetDeferredClosureSlot`, `TryGetDeferredClosureSlotOrUndefined` (C3, 4.6.1) |
| `GetSlot` on a slot without markers | stays: there is nothing to test (O2, 4.7) |
| `GetSlotOrUndefined` | stays for a field that cannot be moved out; else `TryGetSlotOrUndefined` (4.6.1) |
| `GetFlattenedSlot`, `GetFlattenedSlotOrUndefined`, `MakeBorrowedObject` | exist only in Confined outer layouts (4.8) |
| `IsSlotFilled` | an answer (`in`, `hasOwn`). It proves nothing (4.5) |
| `IsNotThenable` | an answer that may be stale (4.5; 07, F1a). Where it looks at a slot named `then` it loads the word once and compares it with the markers; it decodes nothing |
| `PutSlotOrAdd`, `PutSlots`, which read slot and order word in front of their stores | unpublished: as today. Published: the stub under the object lock; `PutSlots` only for fields of the first row of 4.6.2 (4.6.2, 4.8; 05, section 5.6 f) |
| `TryPutSlot`, `AtomicSlot` | 4.6.2, 4.6.6 |
| `LoadExtension`, `EnsureExtension` | 4.6.5; the result is a `window` value (C4) |
| `FinishBirth`, `TryFinishBirth` | not made for Shareable layouts (05, CO15) |
| `TryCopyFields`, `CopyObject`, `CopyDataProperties` | the source is read between two looks at its header and order words; the target is filled by words only while it is unpublished (05, section 5.12, CO14) |
| `EnumeratorGetByVal` | makes its own test in the same operation (4.5). The loaded word is tested like `TryGetSlot`'s; an empty slot goes to the run time (05, CO16) |
| `JSONObjectFinish` | its object is unpublished |
| calls of the run time: `PrivateGet`, `PrivateHas`, `PropHasOwn`, `ForInOpen`, `ForInStep`, `GetOwnDataProperty`, `TryPutByIds`, `ReturnAsyncResult`, `CloneAs`, `JSONAppendValue`, `PrivateSet`, `PrivateDefine`, `NewClass`, `DefineMethod`, `DefineGetter`, `DefineSetter`, `DefineAccessors`, `DefineOwnProperty`, `TryDefineLiveBinding`, `TryReExportFields`, and every access by name (`unknownCode`) | the generic read and the mutations of 05, sections 5.4 and 5.6 |
| inline rows `OwnPropertyKeys`, `ObjectGetOwnPropertyDescriptor`, `ObjectHasOwn`, `ObjectPropertyIsEnumerable`, `OwnPropertyDescriptorFields`, `ObjectValues`, `ObjectEntries` | in line they read shape word, order word and a cached list of keys (per thread: 07, section 7.4), no slot (`aot/builtins/AOTOwnPropertyBuiltinEmitter.cpp:39-65, 143-154`). Where `ExpandOwnKeys` turns them into reads of slots, those are `TryGetSlot` (4.8; 05, CO16) |

**Variables, environments, captures.**

| Operation | Rule |
| --- | --- |
| `GetModuleVariable` | a read without a test needs a fact that survives O3 (4.7). A variable with a dead zone is followed by `CheckInitialized` on the loaded value. The variable of a static field that `canLeaveSlot()`: `TryGetVariable` (4.6.1) |
| `TryGetVariable`, `TryPutVariable` | 4.6.1, 4.6.2; 05, CO11 to CO13 |
| `GetEnvironmentField` | one load; `CheckInitialized` on the loaded value, or a fact under O2 (05, section 5.9) |
| `GetCapture` (`none`) | captures are never written after the closure is made (05, section 5.8); F2 |
| `IsClosureOf`, `IsFunctionOfBuiltin`, `IsConstructor` (`none`) | the closure's word is loaded once, and the answer is taken from the loaded value, whichever of its two forms it has (05, section 5.8) |
| `MaterializeEnvironment`, `ReleaseEnvironment` | a frame's environment, and a cell that was not retained: known to one thread (C2; `ReuseEnvironments`, 4.8) |
| `AreMembersUnchanged`, `IsBuiltinLocationUnchanged` | guard words: one load, may be stale (4.5; 07, section 7.5) |
| `Builtin`, `LinkTimeConstant`, `SingletonClosure`, `SharedRegExpLiteral`, `GetClassPrototype`, `GetSuperBase` | words that are settled ahead (4.8; S5, 4.11; 07, section 7.6) |
| `ValidateFirstRun` | validating builds only. The flag is set by an atomic exchange, so that two threads cannot both be first |
| `GetArgument`, `GetRestLength`, `GetMyArgumentByVal` | the running frame |
| `GetMappedArgument`, `SetMappedArgument` | calls; 06, section 6.10 |

**Storage of arrays.**

| Operation | Rule |
| --- | --- |
| `HasArrayStorage`, `IsInBounds`, `IsElementPresent`, `GetArrayLength`, `GetArrayElement`, `HasArrayElement`, `PutArrayElement`, `AppendElements` | not made: the operations of 4.6.3 on one loaded `Storage` |
| `AppendElement`, `AppendSpread`, `AppendHole` | the literal that is being built is unpublished |
| `HasSpareArrayCapacity` | an answer about a loaded `Storage` (4.8). It licenses no store: the `PutElement` behind it relies on `InsideStorage` of the same value |
| `ReserveArrayCapacity`, `ConvertToDoubleStorage` | calls, `collects`: growth and conversion of 06, sections 6.3.5 to 6.3.7 |
| `ArrayStorageKind`, `IsPristineArray` | answers. The kind comes from the storage word of one loaded pointer, not from byte 4 of the cell. They prove nothing about storage; the access that follows tests again |
| `HasContiguousIndex`, `GetContiguousIndex`, `GetDoubleIndex` | each tests kind, bounds and hole itself, in one operation (`aot/codegen/AOTArrayIntrinsicEmitter.cpp:63-108`). The emitter emits rd1 to rd5 of 06, section 6.3.1: the kind from the loaded storage |
| `TryPutByValDirect`, `TryPutIndex` | the same for stores (`:218-255`): wr1 to wr4, then the bounds test on the storage that may be written. `miss` answers false |
| `PutContiguousIndex`, `PutDoubleIndex`, `GrowNewArray`, `TruncateNewArray` | a failing test is a validation failure that ends the process (`putInBounds`, `:110-131`). The verifier admits them on an unpublished array only, which is what they are for: the result that a built-in is filling |
| `NewArrayLike`, `NewArrayWithCapacity` | the source's kind is a hint for the new array: one load of the storage word. A wrong hint costs a conversion |
| inline rows `ArrayAt`, `ArrayIndexOf`, `ArrayIncludes`, `ArrayLength`, `MathMinOfArray`, `MathMaxOfArray` | one `LoadStorage` per window; kind, length and every element from it; each loaded word is tested for the hole. A loop over the elements loads again behind its poll |
| inline rows `ArrayPush`, `ArrayPop`, `ArrayShift`, `ArraySetLength`, and the fast paths of `ArraySlice`, `ArraySplice`, `ArraySplicePrefix`, `ArrayUnshift`, `ArrayConcat`, `ArrayJoin`, `ArrayFrom`, `ArrayIterator` | the sequences of 06, section 6.3.3, made of the operations of 4.6.3; what has no sequence there is the call (06, A1 to A6) |
| `StringBuilderAppendBuiltin`, `JSONParseExtraMembers`, `JSONAddExtraMember` | calls; 06, A1 to A6 |

**Typed arrays.**

| Operation | Rule |
| --- | --- |
| `IsInsideView`, `GetTypedArrayLength`, `GetTypedArrayElement`, `PutTypedArrayElement`, `RequireAttachedTypedArray`; inline rows `TypedArrayLength`, `TypedArrayByteLength`, `TypedArrayByteOffset`, `DataViewGet`, `DataViewSet`, `BufferAccess`, `Atomics` | `LoadView` and `IsInsideLoadedView` (4.6.4; 06, section 6.8). `Atomics` also `synchronizes` (4.4) |
| `JSONSourceOpen`, `JSONDecodeBytes` | calls. A native that must read the same bytes twice works on a private copy (06, TA10) |

**Tables of Map, Set, WeakMap, WeakSet.**

| Operation | Rule |
| --- | --- |
| `CollectionTable`, `MapNext`, `SetNext`, `MapGetByKeyParts`, `HasByKeyParts`; inline rows `MapGet`, `MapHas`, `SetHas`, `MapSize`, `SetSize`, `WeakMapGet`, `WeakMapHas`, `WeakSetHas`, `MapIterator`, `SetIterator` | lock-free readers of 06, section 6.6 (MS2, MS4, MS5, MS10 to MS12). A pointer into a table is a `window` value (C4) |
| inline rows `MapSet`, `SetAdd`, `MapDelete`, `SetDelete`, `MapClear`, `SetClear` | the owner's object lock (06, MS1): the stub or the call |
| `IsCollectionTableFull`, `ReserveCollectionCapacity` | made by `PlanCollectionCapacities` for the function's own allocations, which are unpublished (4.8) |
| `GetInternalField`, `PutInternalField` | one word, loaded or stored whole. Which words of which class: 06, section 6.6 (the table pointer of a collection); 09, sections 9.6, 9.7 (generators, promises) |

What these heaps do not hold has its rule by heap: `InternalFields` and `PromiseState` in 09, sections 9.6 and 9.7; `StringStorage` in 06, section 6.7; `RegExpState` in 06, section 6.9; `ShapeWords`, `OrderWords`, `NamedProperties`, `PrototypeLinks`, `MemberGuards` in 4.5. `ParserState`, `StringBuilderContents`, `AllocatorState`, `RuntimeState`, `ValidationState` lie in the private part of the view (07, section 7.3).

## 4.7 Facts of flow that encode an order in time

`Initialization`, `StoreCounts`, `ConstructorFieldOrders`, `ExecutionCounts`, `ClassEvaluations` (R3, section 5; R2, section 5.3). Compiled code reads without a test on their strength, so a read that is not ordered after the store must be impossible.

**Counts are not affected.** "Runs at most once", "stored at most once", "evaluated once" rest on "a module of the image is evaluated at most once" (`aot/valueflow/AOTValueFlowResultsExecutions.cpp:81`), which the run time guarantees for all threads together (09, section 9.9), and on the shape of the call graph. A function that is handed to `new Thread` is entered by a native callback, hence lies in `functionsEnteredWithoutStamp` and is never "at most once" (`:115`).

**Orders are.** The order model (`aot/valueflow/AOTFlowOrder*.cpp`) runs per scope owner. A `FlowCall` is `{caller, site, callee}` without a kind (`aot/valueflow/AOTFlowOrder.h:28-35`), and calls only refine entries (`refineEntriesByCalls`, `aot/valueflow/AOTFlowOrderOwner.cpp:598`). The mark `Concurrent` is kept beside the call list: the set of `new Thread` sites, and per AnyThread function the sites from whose entries it can be reached, which result A computes anyway (4.2.2). The **concurrent region** of a site is: its entries, everything they call, and every `Later`, `Repeatedly` or `PerEvent` callback and coroutine resumption that code of the region registers.

| Rule O1 | |
| --- | --- |
| (a) | What precedes the site in the spawning function precedes everything in the concurrent region. (The start edge of 02.) |
| (b) | Nothing that follows the site in the spawner is ordered with anything in the concurrent region. |
| (c) | Nothing in a concurrent region precedes anything outside it. (`join` is ignored: conservative.) |
| (d) | Two executions of one concurrent region are not ordered with each other. So inside a region program order proves something only about locations that are Confined, per thread, or belong to an unpublished object or an activation whose environment is Confined. |
| (e) | A module's body precedes every operation that runs behind a check of that module's state, on any thread. (The module edge of 02.) `FlowModuleOrdering::CompletesBefore` stays valid. |

**For fields of objects and environments of a Shareable layout:**

| Rule O2 | An order fact about a field (`FilledAtBirth`, `FilledWithValueAtBirth`, `AlwaysFilled`, `SetAtCreation`, `StoredBeforeEveryRead`, `OnlyAtConstruction`, `seesOnlyFinishedObjects`, `isThereFromTheStart`) holds iff the store that it rests on happens **while the object is unpublished**: before the point that `ConstructorFieldOrders` calls the leak. Publication safety (02, M6; 4.10) then carries it to every thread. |
| --- | --- |

A class whose constructor lets `this` out before it has filled its fields loses the facts for the fields filled afterwards: they get markers and tests. `--explain=threads` names the leak.

`isThereFromTheStart` ("there from the first moment that anybody has the holder", `aot/valueflow/AOTValueFlowResultsClasses.cpp:755`) is the fact behind `MemberMutability::Fixed`. Its holder is a class object or a class prototype, which the run time makes. It is unpublished until its definition is complete: the cell through which other threads find it is filled after `defineClass`, and until then only the claimant finds it (07, section 7.6, Z2).

**For locations that are addressed from the view:**

| Rule O3 | A module variable, the variable of a static field, or a field of an environment that `PromoteEnvironmentsToVariables` has turned into variables, with `threads = Shared`: an initialization fact, per location (`StoredBeforeEveryRead`, `SetAtCreation`, `undefinedAtStartIsNeverRead`) or per read (`readsAfterInitialization`), holds for a read in an AnyThread function only if, for every `Concurrent` site `s` from whose entries the reader can be reached, (i) the owner's module `CompletesBefore` the module of the function that contains `s`, or (ii) `s` lies in a frame of the owner and an initializing store dominates `s`. Otherwise the reader's entry in that owner is `unknownScopeEntry`: the type keeps `absent`, the variable has a marker, and the loaded value is tested (C1, C3). |
| --- | --- |

Why it is needed beside O1. A function's entry is the first point in the owner's frames at which it is mentioned or used (`aot/valueflow/AOTFlowOrderOwner.cpp:383`), a reader in another module counts as entered after the owner's exit where the module order says `CompletesBefore` (`:496`), and a store precedes a read if it dominates that point (`aot/valueflow/AOTFlowOrderLocations.cpp:35`). A thread entry is one more use of a closure, so O1 (a) and (b) hold inside one owner without a change. What the analysis cannot ask is whether a store in one owner precedes a spawn site in another.
A closure that is made behind the store can reach a thread that is already running, by a plain store. The field of an object is then safe, because it is addressed through the reference that carries the causality (O2, F2). A variable is addressed from `instanceGPR`: the statics are published to B3 as dereferenceable (`aot/codegen/AOTPrimitiveEmitter.cpp:209`), so the load is not control dependent (`b3/B3Procedure.cpp:584`) and may be hoisted in front of the loop in which the thread waits for the closure; on ARM64 the hardware may do the same.
A fact that passes O3 holds from the thread's first instruction, so no motion of the read can break it.

Where a fact falls, the type of the read keeps `absent` (or `undefined`), the slot or variable gets a representation with markers (`aot/place/AOTSlotRepresentations.cpp:140-166`), and the read gets its `CheckInitialized`, `TryGetSlot` or `TryGetVariable`.

`LocationStability` (`aot/ir/AOTMemoryLocationAccesses.cpp:132-152`), by which reads are carried over calls and synchronizations, is fixed only for "never stored" or "stored at most once" **with** a known initialization. It stands and falls with O1 to O3.

**Consumers that need "at most once" as mutual exclusion**, not only as a count, are check-then-act at run time and get a protocol in 07, section 7.6: `DeferInitializers`, `DeferDefinitions`, `DeferClosuresInFields`, singleton closures, class objects, `NewTemplateObject`, `SharedRegExpLiteral`. The code that they defer runs on the thread that first needs it (4.2.2).

## 4.8 A rule for every pass

Only passes that change. "shared location" = a variable or environment field with `threads = Shared`, or a field or element of a subject that may be shared.

| Pass | Rule |
| --- | --- |
| `EliminateRedundantReads` | Unchanged in what it does: load-load, store-load and allocation-load forwarding are allowed between synchronizations (02, section 2.10.6). It honours `maySynchronize`, and what a `LoopHint` writes (4.12), through the effects. Its **fixed classes**, which no call clears, stay: a class is fixed only with `stores` of `Never` or `AtMostOnce` and an initialization fact (`stabilityOfFlow`, `aot/ir/AOTMemoryLocationAccesses.cpp:132-151`), or as a variable that is stored at most once and has passed its check (`aot/passes/AOTEliminateRedundantReads.cpp:231-312`). Both say that a value, once seen, is final. They hold over a synchronization as far as the initialization fact survives 4.7 |
| `HoistLoopInvariantReads` | The same, through the effects |
| `carryVariablesAroundLoops` | Allowed for shared locations as far as it only removes reads. A location that the loop's `LoopHint` writes is not carried |
| **`delayStoresIn`** (`aot/passes/AOTCarryVariablesAroundLoops.cpp:690-799`) | **Off for shared locations.** It puts a store on every edge that leaves the loop, also on paths where the loop stored nothing: an invented write |
| `EliminateGuards`, `PlaceTypeTests` and its hoister, `HoistTestsIntoPredecessors` | Through guard availability and 4.5. No change of their own |
| `ResolveAccesses`, `ResolveCalls`, `SplitUnions`, `ExpandIteration`, `ExpandProvenIterations`, `ExpandOwnKeys`, `FuseArrayChains`, `SelectTypedResults` | They make test-and-use pairs. They ask `mayBeShared(subject, at)` and make the forms of 4.6 |
| `SinkAllocations`, `EliminateDeadCode` (write-only objects), `PlaceAllocationsAtTheirUse` | Unchanged: about objects that have not escaped |
| `FinishBirths` | For a Shareable layout the mark of the cell is neither read nor written by anybody (05, CO15). The pass skips such layouts |
| `GuardEmitter::flagsTestedBy`, `planTestsOfFlags` (`aot/codegen/AOTGuardEmitter.cpp:174-252`) | Today `HasNoFieldOutsideSlot` is as a rule a test of the mark. For a subject that may be shared it answers 0 for that opcode, so the test loads the order word and tests bit 27. Only `IsPristine` is merged into tests of the flags byte |
| `AllocateInFrames`, frame arrays | Unchanged, given C2 |
| `AllocateInnerObjects` | Unchanged: static. The collector's zeroing of an unreferenced inner header happens with all threads parked |
| `FlattenInnerObjects`, form 1 | Unchanged |
| `FlattenInnerObjects`, form 2 (off today) | Only for Confined outer layouts |
| `DeferClosuresInFields` | Shareable layouts: the slot is published by compare-and-swap from "not made yet"; the loser drops its closure and starts again from the load (07, Z1). The read is `TryGetDeferredClosureSlot` or `TryGetDeferredClosureSlotOrUndefined` (4.6.1) |
| `DeferInitializers`, `DeferDefinitions` | Shared variables: Z1, or Z2 where the initializer does more than allocate. The claim of Z2 is a word of its own in the shared statics; the variable keeps its "not made" encoding until the winner's release store, so a reader in line never meets a word that it cannot decode |
| All three deferring passes | What they move out of a body runs on the thread that first needs it. The derived function's `threads` is the join over the functions that read the location (4.2.2). A pass does not defer an initializer that touches a MainOnly variable if a reader is AnyThread |
| `NarrowConstantVariables` | Unchanged; `absent` stays in the type where 4.7 says so |
| `PromoteEnvironmentsToVariables` | The count fact stands. The variables get their `threads` from the functions that touch them. A field of an environment is addressed through the closure that carries it (O2); a variable is addressed from the view. So the initialization facts of the fields are asked again of the variables, under O3 (4.7). Where one falls the pass leaves the environment as it is |
| **`ReuseEnvironments`** | The free-cell word moves to the private statics, so each thread has its own. An environment that is released was not retained, hence is known to one thread |
| `ShareRegExpLiterals` | A shared literal with `g` or `y` is shared only among MainOnly functions; in AnyThread functions each evaluation makes its object. Literals without state stay shared |
| `PromoteAggregateParameters`, `SplitFreshResults`, `RemoveUnread*` | Unchanged. `PromoteAggregateParameters` cuts the callee behind the last use of the parameter and moves that prefix, in its order, into a wrapper (`aot/passes/AOTVirtualObjectParameters.cpp:333-400`): no read crosses an operation. The others rest on order-free facts |
| `InlineCalls`, `InlineHeads` and every other pass that changes calls | They run in front of `MarkRecursiveEntries`. Until then each function of a non-trivial component of the call graph counts as `collects` (S3', 4.11), so no `window` value is placed over a call that may turn out to be a safepoint |
| `ElideStackChecks` | Unchanged; the limit is per thread |
| `CallModules` | 09, section 9.9 |
| `RestartInGenericVersion` | Unchanged: the prefix wrote nothing that anybody sees |
| `AddGenericVersion` | Unchanged. The new edges (`empty`, `outside`, `hole`, `miss`) are ordinary failing tests |
| `PlanArrayCapacities`, `PlanCollectionCapacities` | Own allocations: unchanged. `HasSpareArrayCapacity` on an array that may be shared takes a `Storage` |
| `Select` rules | `crossesEffects()` honours `maySynchronize` through `interferesWith()`. With C5 that holds for both ends of every edge, so no member of a group moves over an operation that releases. `SlotStoreRun` (`PutSlots`) only for unpublished objects or fields of the first row of 4.6.2 |
| `ChooseRepresentations` | `Storage` is a pointer register. Narrow references as today |
| `RemoveRedundantBarriers` | Unchanged. All three arguments rest on "this thread parks only at its own `collects` operations" and on "not yet retained" (08, section 8.8) |
| **New: `PinLoopExitReads`** | 4.12. First step of the schedule `Simplify` |
| **New: `MarkRecursiveEntries`** | 4.11 (S3'): runs once, behind `InlineHeads`, in front of `ElideStackChecks` |

**Emitters of built-ins** that read several words of a cell which another thread can change:

| Emitter | Rule |
| --- | --- |
| `StringEmitter` (`viewOf*`, `sourceOfRope`, `aot/codegen/AOTStringEmitter.cpp:190-199`) | Characters, length and width come from one loaded `StringImpl*`. `sourceOfRope` gets the characters from a stub today and then loads the impl of the flat base again for the width; atomization can swap the impl between the two. The stub `ropeCharacters` returns characters and flags as a pair, from one load of `m_fiber` (06, section 6.7) |
| `DateBuiltinEmitter` (`aot/builtins/AOTDateBuiltinEmitter.cpp:16-25, 58-68`) | It reads and writes the three words of a `DateInstance` in line. Unchanged: each word is read and written whole, and none is a reference or an index. What a race can leave behind: 06, section 6.10; 02, section 2.10.5 |
| Map, Set, promise, generator, RegExp emitters | 06, sections 6.6, 6.9; 09, sections 9.6, 9.7. A table pointer is a `window` value |
| `ObjectBuiltinEmitter::generateObjectFreeze` (`aot/builtins/AOTObjectBuiltinEmitter.cpp:178-193`) | It loads the order word, sets the integrity bits and stores the word back, without the lock. On a published object that would undo a key, a count or bit 27 that another thread has stored in between (05, CO5). The inline form is made for unpublished objects and Confined layouts; otherwise `Object.freeze` is the call (05, section 5.6 d) |
| Calls of natives that `block` with a buffer operand | pin (4.6.4) |
| `directEntry` calls | The call emitter puts the owner test in front of the call, for the receiver **and for each operand** of an `owner` class whose site is not Confined (11, section 11.3) |
| `Builtin`, `LinkTimeConstant`, `SingletonClosure`, `SharedRegExpLiteral`, `GetClassPrototype`, `GetSuperBase` | They are declared without `collects` and stay so: their cheapness matters. The first `new Thread` of a realm runs `Instance::prepareForThreads()`, which **settles** each cell that the image names through these operations: the cell is made, or it is recorded as unavailable in this realm (07, section 7.6). So once a second thread exists their slow paths find the cell settled: they only load, look nothing up, write nothing, and never wait for a claim (S4). A cell that can come into being later, and a class whose definition runs JavaScript of the program, are read by a form of the operation that has the effect `waits` (S5, 4.11) |

## 4.9 B3 and Air

`aot/codegen/AOTB3Policy.cpp`. The IR's effects become heap ranges, so most of the above reaches B3 by itself. One thing does not: that a safepoint ends `window` values.

**Rule C4 (what a safepoint ends).** A load that yields a `window` value of a subject that may be shared (a `Storage`, its lengths and its storage word, a view's vector and length, the hidden word of an Extension, the table of a Map or Set) is ended by every operation that `mayCollect`, **and by everything that ends it today**. The second half is what keeps the meaning of a program on one thread: the load must stay in a heap that this thread's own stores to the same word overlap.

*In the IR.* An operation whose result has a `window` type counts as reading a location that every `mayCollect` operation writes: `interferesWith()` answers yes for the pair, and a pass that merges equal operations does not merge two of them over such an operation. Stretch proofs about a `Storage` value (`WritableStorage`, `BelowLength`) die with their subject. The declared `reads` and `writes` of the operations stay what they are, so a Confined array loses nothing.

*In B3.* A memory value has one `HeapRange` (`b3/B3MemoryValue.h:156`), and `MemoryNumbering` keys a load by the epoch of that one range (`b3/B3MemoryNumbering.cpp:362-375`). A separate heap for such loads would therefore cut them off from this thread's stores: `a.length` behind `a.push(x)` would be the old length. Instead the tree of heaps gets one node and one leaf above each of four IR heaps `H` (`ArrayStorage`, `TypedArrayStorage`, `CollectionStorage`, `ObjectFields`), in `AOTHeaps::initializeEntireHeaps()` (`aot/codegen/AOTHeaps.cpp:264-268`):

```
root
  window(H)            new. The range of a load that C4 speaks of
    entire(H)          as today, with all that is nested under it
    atSafepoint(H)     new leaf. No access names it. Every patchpoint that collects writes it
```

| Access | Range | Overlaps |
| --- | --- | --- |
| a load that C4 speaks of | `window(H)` | every store under `entire(H)`, whoever's subject it is, and `atSafepoint(H)` |
| every other load and every store, of Confined, unpublished and shared subjects alike | as today: `entire(H)` or a heap under it | as today. Not `atSafepoint(H)` |
| a patchpoint whose IR effects say `mayCollect`, **loop polls included** | what it declares today, and the four leaves, through `appendHeapsWrittenByCollection()` (`aot/codegen/AOTHeaps.cpp:442-447`, called by `applyEffects()`) | the loads of the first row only |

So code on Confined and unpublished subjects is compiled as today, to the instruction. A range wider than one heap is keyed by the state of all memory (`keyForLoad`), so a load of the first row is merged with an earlier one only if nothing was written in between: correct, and coarser than need be. Giving `heapEpoch()` ranges is a later refinement.

| Switch or phase | Rule |
| --- | --- |
| `MemoryNumbering`, `hoistLoopInvariantValues`, `versionLoops`, `clampsLoopBounds`, `foldsComparisonsWithBounds` | Stay on, under C4 and 4.12 |
| `hoistLoopInvariantValues` | It ignores loop polls as trap barriers and as sideways exits (`b3/B3HoistLoopInvariantValues.cpp:75, 201`). It honours what a poll's patchpoint writes: the writes of every value of a loop are collected without exception (`:198`), and a value whose reads overlap them stays in the loop (`:298-300`). It also hoists a **call** that only reads (4.12) |
| Loop versioning with clamped bounds | For an array that may be shared the versioned loop's invariant loads (storage, length) have the range `window(ArrayStorage)`, so a loop with a poll is not versioned on them. A loop without a poll does not exist in mt images (4.11). Strip-mining, which would give the versioned form back, is a later optimization (13, section 13.5) |
| `mergesAdjacentConstantStores` | Stays on, unchanged. The phase merges plain constant stores to one base within a block, whatever they initialize (`b3/B3MergeAdjacentConstantStores.cpp:82-148`). On a published object that is harmless: each slot lies whole inside the merged store, both stores are in the program, and a fenced store, a release store, a compare-and-swap or any value that reads memory is no candidate and ends the run |
| `updateIntegersInMemory` | Allowed: `o.n += 1` is a racy read-modify-write in the program too. Not for a slot of the second row of 4.6.2 |
| `trustsKnownRanges` | Stays on. It is sound because no marker and no placeholder can reach a value with a declared range: C3 for slots and for variables of static fields, O2 and O3 for reads without a test, F1a for guards |
| `trustsKnownDereferenceableBytes` | Speculative loads from a cell are harmless. Not published for `Storage` values beyond the header. The statics are published as dereferenceable (`aot/codegen/AOTPrimitiveEmitter.cpp:209`), so a load of a variable is not control dependent (`b3/B3Procedure.cpp:584-593`) and may move in front of the test that guards it. That is why a read of a shared variable without a test needs a fact that holds from the thread's first instruction (O3) |
| Air `pairsMemoryAccesses` | A pair is two single-copy-atomic accesses. Allowed for plain accesses. Never for a release store or a fenced access |
| `AirFoldTrapsIntoAccesses` | Unchanged: the fault handler is stateless and takes its registers from the faulting context (09, section 9.5) |
| Immutable heaps | `AOTInstance_fixedFields`, `AOTInstance_vm`: still immutable *per thread*, since the pinned register does not change while a frame is live. Loads of allocator pointers declared immutable stay right for the same reason. A frame that is saved and restored keeps such values, so it is restored only on the thread that saved it, and never after that thread has ended (09, G1 to G3) |
| No phase adds a load | C1 in B3 and Air: a load of a shared location that has two uses is computed once. `lowerToAir` folds a load into an instruction only if that instruction is its one use (`canBeInternal`, `b3/B3LowerToAir.cpp:552-567`). The register allocator rematerializes constants only (`b3/air/AirAllocateRegistersByGreedy.cpp:5361-5533`). A phase that duplicates code (`rotateLoops`, `versionLoops`, tail duplication) copies the load with all its users |

**The direct code generator** (`aot/codegen/AOTDirectCodeGenerator.cpp`) emits what the emitters wrote, in order. It needs cases for what was not reachable before outside `Atomics`: fenced `Load` and `Store` of all widths, `AtomicStrongCAS`, `AtomicXchg*` of all widths.

## 4.10 Fences

x86-64: no instruction is added for plain accesses. Total store order gives M6 and every "A before B" below. The compiler-level order is kept by the B3 fence that ends initialization and by fenced stores.

ARM64:

| # | Rule | Instruction |
| --- | --- | --- |
| **F1 publication** | A store of a cell reference into a shared location (slot of a published object or environment that may be shared, shared variable, element or property in storage that may be shared, shared region of the Instance, any shared table of the run time) is a **store-release**. This binds compiled code, the direct generator and **every stub** that stores a register which may hold a cell into a holder that may be shared | `stlr` / `stlr w`. No addressing modes, so often one `add` more |
| **F1' publication by C++** | In mt builds `WriteBarrierBase<T>::set`, `setWithoutWriteBarrier`, `AuxiliaryBarrier::set` and `WriteBarrierStructureID::set` store with `std::memory_order_release`. So do the stores that go round them: `putDirectOffset` and other stores through `locationForOffset`, the element setters of `Butterfly`, `tryWriteSlot`. `setEarlyValue`, stores inside an `ObjectInitializationScope`, and call sites named `..Unpublished` stay plain. A bulk copy of references (`gcSafeMemcpy` in `splice`, `concat`, `fill`) into storage that is not unpublished is preceded by one store-store fence | one `stlr` per store, `dmb ishst` per copy; nothing on x86-64, where the release order still keeps the C++ compiler from sinking an initializing store below the publishing one |
| **F2 dependent reads** | A load through a reference that was itself loaded needs nothing: the address dependency orders it. A load at an address that is computed from `instanceGPR` depends on no reference, whatever led the code there: O3 (4.7) | |
| **F3 guard loads** | Where a load of one word licenses a load at an address that does not depend on it the first is a **load-acquire** | `ldapr` where available, else `ldar`. The uses are listed below |
| **F4 initialization** | `mutatorFence()` after initializing a cell stays as it is: a fence only while the collector marks. Other mutators are served by F1 and F1' | |
| **F5 synchronization** | sequentially consistent | `ldar`, `stlr`, LSE atomics with `al` |

Why F1 and F1' are enough for M6: a reference reaches another thread only through a shared location, or through an operation that synchronizes. A release store orders *all* earlier stores of the thread, so not only the object's own fields but everything reachable from it is visible.

A word that is no cell reference can be a reference all the same. Each has its own rule:

| Word | Names | Rule |
| --- | --- | --- |
| StructureID | a Structure | stored with release (05, CO9; 06, N1; F1') |
| order word with a number at or above `layout.numberOfOrders` | an order that was added to `KeyOrderTable` at run time | plain store. Adder and reader pass through the table's lock (07, section 7.7) |
| hidden words, storage pointers, table pointers, `m_fiber` | memory that is no cell | release, or compare-and-swap (05, CO6, CO7; 06, SW7, MS7, section 6.7) |
| word of a literal string, function number, shape | the image | read-only |

**Uses of F3.**

| First load (acquire) | Then | Where |
| --- | --- | --- |
| StructureID | a slot or a hidden word of the same object at a cached offset | 05, CO9; 06, N1 |
| a slot that holds `absent` | the order word (bit 27) | 4.6.1; 05, section 5.4 |
| order word (bit 28) | the hidden word of the extra properties; in a layout without that word the StructureID, for the side object that the variant holds | 05, section 5.6 |
| order word ("out of line") | StructureID, for the list | 05, section 5.6 g |
| a view's vector | its length | 06, TA2 |
| a module's state | the module's variables | 09, MD1 |
| `m_packed` of a promise | `m_slot` | 09, PR3 |

Where F1 is **not** emitted: the holder is Confined, unpublished, or lies in a frame; the variable is MainOnly or per thread; the stored value is no cell by its type.

The alternative, a `dmb ishst` at the first retaining use of each new object, needs a precise notion of "first" on all paths and misses stores made to an old object just before it is published. Rejected.

In B3 a release store is `Store` with a fence range restricted to "release" (`MemoryValue::setFenceRange`); Air has `StoreRel*` forms for WebAssembly. A fenced access is a barrier for all of B3's memory optimizations today. That is stronger than needed: a policy switch `optimization.releaseStoresOrderOnlyEarlierAccesses` lets later loads stay movable. Not needed for correctness.

## 4.11 Polls and safepoints

**Rule S1.** A pointer to replaceable storage of an object that may be shared (butterfly, Extension, table of a Map or Set, vector of a view) does not live across a safepoint. In the IR such a pointer is a value of a `window` type, and the verifier refuses one that is live across an operation that `collects` (4.6.3: `StorageLiveAcrossSafepoint`). In C++: 08, S1'.

**Rule S2.** A thread parks only at an operation whose effects say `collects`.

**Rule S3 (time to safepoint).** In an mt image every cycle of a control flow graph, every cycle of the call graph, and every loop of the run time that calls compiled code contains a safepoint. The last are: the job loop, since a job that queues itself again (`function poll() { if (!done()) queueMicrotask(poll); }`) is a cycle that no graph of the compiler shows, a `Later` callback being no edge of the call graph; and natives that call back once per element (08, sections 8.4, 8.5).

| Today | mt image |
| --- | --- |
| `LoopHint` on every CFG cycle (verifier); the poll is not emitted in an innermost loop that counts up to a data length (`aot/codegen/AOTHintEmitter.cpp:102-150`) | the poll is always emitted. One load on Linux x86-64, a test and a never-taken branch elsewhere |
| The prologue's stack check is also a poll, but its slow path holds `DeferGCForAWhile`: "What a caller knows about a callee that collects nothing holds for the callee's prologue too" | stays so for most functions. **New pass `MarkRecursiveEntries`** (S3'): in every non-trivial strongly connected component of the call graph it picks the functions that cut all cycles (any feedback vertex set; the greedy one is fine) and sets `FunctionFlags::EntryIsSafepoint`. Such a function's summary says `collects`, it always has a stack check (`ElideStackChecks` leaves it `Required`), and its failing path calls `operationAOTPollSlow` instead of `operationAOTPollAtPrologue`. Without this `fib(40)`, which neither loops nor allocates, would hold up every collection for seconds |
| Large frames compare with `softStackLimit`, which is not trap aware | unchanged, unless the function has `EntryIsSafepoint`: then it also tests the trap-aware word |
| `operationAOTPollSlow` handles traps | also parks for the collector and for safepoint operations. `operationAOTPollAtPrologue` leaves the request for a stop alone: it neither parks nor disarms the polls (08, section 8.4) |

**Rule S3' (the cut belongs to the final call graph).** `CallGraph` and `EffectSummaries` are results that are computed again whenever a pass invalidates them (`aot/pipeline/AOTPipeline.cpp:146-147`). `InlineCalls` runs in the schedule `Optimize` and invalidates everything (`:203,311`); `SplitCoroutines`, `CreateEntrypoints`, `OutlineColdOperations`, `InlineHeads` change the graph later. A cut that is chosen early is undone: of a cycle `f -> g -> f` with `g` marked, inlining `g` into `f` leaves `f -> f` without a poll. So:

| # | |
| --- | --- |
| 1 | `MarkRecursiveEntries` is a program pass on the form `Selected`. It runs once, behind `InlineHeads`, the last pass that adds, removes or redirects a call, and in front of `ElideStackChecks` (`aot/pipeline/AOTPipeline.cpp:177-179`), `ChooseRepresentations` and `RemoveRedundantBarriers` |
| 2 | Before it has run, `ComputeEffectSummaries` gives **every** function of a non-trivial component `collects`. So no pass ever rests on "this recursive function does not collect", and the marking only takes the effect away from functions that are not picked and do not collect otherwise. The summaries are computed again behind it |
| 3 | The edges of the graph are: direct calls, callee stamps, tail calls (`ReturnCall`), accessors, calls through natives that call back (`indirectCalls`). An unknown call counts as an edge to every function that unknown code may enter |
| 4 | The verifier checks S3 on the final graph as it checks `LoopHint` on CFG cycles: `InternalErrorKind::CallCycleWithoutSafepoint` |

S3 bounds cycles, not paths: a call tree without cycles, loops or allocation is exponential in its depth. Only generated code gets there (08, section 8.5).

**Rule S5 (operations that may wait for another thread).** A new effect word **`waits`** in the schema, bit `mayWaitForOtherThreads` of `Effects` (4.4). It implies `collects`. In an mt image it stands on every IR operation and every `runtime` row whose path through the run time can reach one of: a claim of 07, Z2; a module claim (09, MD3''); a parking lock (08, H15); `Heap::runSafepointOperation` (an add to an object that may be shared, a replacement of `Shared` storage, `flattenDictionaryStructure`); a blocking primitive. `unknownCode()` has it. The list is closed in the schema. So there are three classes of operation. One without `collects` runs to its end without parking and without giving up heap access (08, S4), which is what S1, barrier removal and narrow references in registers rest on. One with `collects` and without `waits` parks only for a stop that somebody has requested (and for the flush of 08, section 8.9): it depends on no other thread's progress. One with `waits` may stand still until another thread has finished something. To the passes `waits` says no more than `collects`. A validating build checks both borders (`OperationParked`, `OperationWaited`, 4.14).

The operations that are declared **not** to collect (`CreatesConstant`, `AllocatesWithoutCollecting`, `RuntimeCreatesConstant`, `RuntimeCreatesField`: `aot/ir/AOTSchema.def:83-85`, `aot/ir/schema/runtime-operations.def:1-5`) fall into three groups.

| Operations | What their slow path reaches today | mt image |
| --- | --- | --- |
| `Builtin`, `LinkTimeConstant`, `SingletonClosure`, `SharedRegExpLiteral`, `GetClassPrototype`, `GetSuperBase` | lazy properties of the global object, class objects, the loading of one of Bun's modules: `operationAOTBuiltinCell` holds `DeferGCForAWhile` over `loadModuleOf()` and `find()` (`aot/runtime/AOTConstantOperations.cpp:71-79`, `aot/runtime/AOTBuiltinCells.cpp:264-273`) | **settled ahead.** They keep their effects. The first `new Thread` of a realm runs `Instance::prepareForThreads()` before the OS thread is made (09, section 9.2). It settles each cell that the image names through these operations: made, or recorded as unavailable in this realm. Afterwards these operations only load: an empty word of a built-in cell means `unavailable`, and the slow path looks nothing up and writes nothing; a word of the view that each thread fills for itself (`linkTimeConstants[]`: 07, V9, `pulled`) is copied from a truth that is made, without a wait. A cell that can come into being later (a member whose creation is awaited, a row with the mark `MayBeAbsent`, a module that is loaded on demand), known from the table of built-ins (03, section 3.5.1), and a class whose definition runs the program's JavaScript, are read by a form of the operation that is declared `waits`. Built-in **values** are not made ahead: their read is the one that the program wrote, and the operation behind an empty word `throws`, `collects` and `waits`. The rule is stated in 07, section 7.6 |
| `GetOwnDataProperty`, `EnumeratorGetByVal` | a read reifies a lazy static property, which is an add (06, section 6.4) | `waits` |
| `String`, `BigInt`, `KeyConstant`, `Decode`, `PutSlotOrAdd`, `PutSlots`, `MapNext`, `SetNext` | allocation without collection, publication by compare-and-swap (07, Z1), leaf locks (05, section 5.3) | unchanged: nothing there waits |

`prepareForThreads()` costs the first `new Thread` the time to settle what a single-threaded run would have made lazily or never, internal modules of Bun included. The image's list of these cells is known at build time and is reported by `--explain=threads`.

What a `collects` operation clobbers in an mt image, beyond today: every value of a `window` type (a `Storage`, a view's vector and length, the address of an Extension, the table of a Map or Set) of a subject that may be shared, in the IR and in B3 (rule C4, 4.9), and, if it is the `LoopHint` of a loop, the locations that `PinLoopExitReads` has recorded in it (4.12). It does **not** end other guards or remembered reads of values. A thread that has parked and goes on may use stale values and stale harmless guards, exactly as if it had not parked.

## 4.12 Reads that decide a loop's exit (M10)

The guarantee is expressed in the one vocabulary that every pass and B3 already obey: **the poll of a loop declares that it writes the locations on which the loop's exit depends.**

```
operation LoopHint (engine pinned?: Locations) [| engine isVersioned] [| engine comparesCacheEpoch] -> none ; UsesRuntime terminates collects writes(pinned) ; runtime = PollSlow
```

`Locations` is a new payload kind that holds `LocationMasks` (`aot/ir/AOTMemoryLocation.h:116-258`). The alias step restricts the writes to subjects that may be shared and to variables with `threads = Shared`.

New function pass `PinLoopExitReads`:

1. For each cycle of the control flow graph (the strongly connected components that `verifyLoopMarks` walks, `aot/ir/AOTVerifierDominance.cpp:129-155`; reducible or not) and each edge that leaves it, take the backward slice of the branch **over data and control dependences** inside the cycle.
2. For each operation of the slice that reads memory, add what it reads of shared locations to `pinned` of the cycle's `LoopHint`s:

| Operation in the slice | What is added |
| --- | --- |
| a read of a value: the terminators of 4.6.1, `GetSlot`, `GetSlotOrUndefined`, `GetModuleVariable`, `TryGetVariable`, `GetEnvironmentField`, `TryGetElement`, `GetStorageLength`, `IsBelowLength`, `GetTypedArrayElement`, `GetInternalField`, the inline reads of Map, Set and promise state | its declared `reads` |
| a **test of an object's state**: `IsSlotFilled`, `HasNoExtraProperties`, `HasNoFieldOutsideSlot`, `IsNotFrozen`, `HasKeyOrder`, `HasLayoutPrototype`, `AreMembersUnchanged`, `IsPristine` and the others of the family `Guard` | `locationsReadBy(test)` (`aot/analysis/AOTGuardFacts.cpp:732-742`): what its proof depends on |
| a guard that an operation of the slice names and whose test stands outside the cycle | the same. The guard then ends at the poll, so its user in the cycle may not name it any more. The pass puts the user back into the form by name that the schema's column `replaces` gives for it; the next `ResolveAccesses` resolves it again, with a test inside the cycle. (Rare: the pass runs in front of the passes that choose guards, below.) |
| a call of compiled functions | the `reads` of the callees' summaries |
| a call of unknown code, an access by name, a call of compiled functions whose summary contains an access by name | nothing is added to `pinned`: it ends everything anyway. The cycle's `LoopHint`s get the flag `comparesCacheEpoch`. The compiler keeps such an access in the loop, but the access is answered by the thread's own caches, and a cached `Absent` or inherited answer does not change when another thread changes a prototype: the receiver's StructureID stays. With the flag the fast path of the poll ends with the compare of 07, E2a (a load, a compare, a branch that is never taken), whose slow path drops the thread's chain-dependent entries. So `while (o.ready === undefined) {}` ends after another thread has stored `ready` on a prototype of `o`. The flag only gets set, as `pinned` only grows |

3. `pinned` only grows. A later run never takes a location out, so a fold that removes the last visible trace of a test does not undo the pin.

What follows, without a further rule:

| Consumer | Effect |
| --- | --- |
| `EliminateRedundantReads`, `HoistLoopInvariantReads`, `carryVariablesAroundLoops` | the poll is a writer of the location: the read is not forwarded over the back edge, hoisted or carried. Two reads in one turn may still be merged, and a store of this thread is forwarded as ever |
| guard availability (`mayEndProofOver`), hence `EliminateGuards`, `PlaceTypeTests` and its hoister | a state test in the slice is made again in every turn. `while (o.x === undefined) {}` for a property that does not exist yet, `while (!("x" in o)) {}`, `while (!Object.isFrozen(o)) {}` end |
| B3 | the poll's patchpoint takes the effects of the IR operation (`emitPoll`, `aot/codegen/AOTHintEmitter.cpp:165-173`), so it writes the buckets of these locations. The loads stay in their own heaps, which this thread's stores overlap too |
| B3 and calls | a call whose summary only reads becomes a patchpoint without writes and without a sideways exit (`applyEffects`, `aot/codegen/AOTRuntimeCallEffects.cpp:72-117`). It need not execute, so the hoister would move the **call** out of the loop (`b3/B3HoistLoopInvariantValues.cpp:257-311`). Its reads now overlap what the poll writes, and it stays |
| Effect summaries | what a `LoopHint` writes by `pinned` is left out of its function's summary, as suspension is (`createFunctionSummary`, `aot/ir/AOTEffectSummaries.cpp:640-666`): a caller may remember a read over the call (02, section 2.10.6). If the call decides the exit of a loop of the caller, the caller's own `LoopHint` pins it |

**Place in the schedules.** `EliminateRedundantReads` is part of `Simplify`, and `Simplify` is part of `Prepare` (`aot/pipeline/AOTPipeline.cpp:256-290`), so it runs long before `Optimize`. The pass is therefore the first step of `Simplify`, which also puts it behind `InlineCalls` in each round of `Optimize`. It runs once more in front of each run of `PlaceTypeTests`, `SeparateUnlikelyLoopPaths`, `HoistLoopInvariantReads` and `EliminateGuards`, because `ResolveAccesses` makes new tests between two runs of `Simplify`. But for the rare case of the table it changes no code, only the payload of `LoopHint`s.

Cost in #249, with the same rule in its optimizing tiers: 0.8 % of cycles. Here it applies to shared locations only.

The owner of the API has never ruled on this guarantee. 13, section 13.6, lists the alternatives (no guarantee, as for SharedArrayBuffer; or bounded staleness by making a poll's slow path continue in the generic version). The pass is separable: dropping it changes no other rule.

## 4.13 `Lock.prototype.hold` and friends in line

`inline LockHold ; reads(all) writes(all) synchronizes throws collects ; family ThreadsBuiltin`. The holder field of the lock's word is the **thread's ID plus one**; 0 means free (09, section 9.13). The main thread's ID is 0, so the ID itself could not stand for "held". The view holds the word "my ID + 1" as a constant.

Fast path: compare-and-swap the lock's word from 0 to that word (acquire); the compare of E2a (4.4); call the closure (a direct call where flow gives the exact function, so that it can be inlined); on both the normal and the handler edge compare-and-swap the word back from "my ID + 1" to 0 (release), which fails if a waiter bit has been set meanwhile, and then call the operation. Contended, or held by the caller itself: `operationAOTLockHoldSlow`, which throws `Error("Lock is not recursive")` or releases heap access while it waits.

The handler edge also runs for the termination exception: a thread that is terminated releases the locks that it holds synchronously (09, section 9.10).

`Thread.current`, `thread.id`, `ThreadLocal.prototype.value` on a per-thread variable: loads from the private part of the view.

## 4.14 Validating builds

`--aot-validate` checks every fact that compiled code relies on. New checks, each with "a lie per producer" in `aot/tests/validation/`:

| Fact | Check | Failure kind |
| --- | --- | --- |
| `MainOnly` function | `ValidateThread` at the entry (where `insertFirstRunTest` puts its test, `aot/valueflow/AOTFlowApplyCasts.cpp:525-531`): the view's thread ID is 0. Also at the entry of code that a pass has deferred (4.2.2) | `RanOffMainThread` |
| `MainOnly` variable | the same test at each access | `VariableTouchedOffMainThread` |
| Confined layout | validating builds add a hidden field "allocating thread" to Confined layouts; `validateOperandsOf` compares it at each use. Validating builds never give out a thread ID twice. The ID of a thread that has ended counts as equal to every thread's, because Confined says nothing after the end of the allocating thread (4.2.3); the first use by another thread writes the user's ID over it, so that from then on the object is checked as that thread's | `ConfinedObjectOnOtherThread` |
| Confined array, Map, Set site | the storage word's state is `Confined` and, in validating builds, carries the thread ID too, with the same treatment of ended threads | the same |
| The closure property itself | at each store of a reference into a holder that may be shared: the stored object's layout or site may be shared too (the style of `checkIsNotFrameCell(operand, retainer)`) | `ConfinedObjectPublished` |
| What reaches native memory is Shareable (R5') | `NativeCallValidationScope`: an operand that a row retains, and a value that leaves a job or timer as an uncaught exception or an unhandled rejection, has a layout or site that may be shared | the same |
| Unpublished | validating builds keep a per-thread set of cells allocated since the last retaining use that the IR declares; a native call that keeps an operand against its row is caught by `NativeCallKeptOperand` already | |
| Order facts (O1 to O3) | `BindingReadBeforeStore`, `SlotEmpty` exist | |
| At most once | `ValidateFirstRun` becomes an atomic exchange | `SecondRun` |
| No storage across a safepoint, IR (S1) | static (verifier). In validating builds a parked thread also poisons its caller-saved registers | `InternalErrorKind::StorageLiveAcrossSafepoint` |
| No storage across a safepoint, C++ (08, S1') | `releaseAccess()`, `becomeParked()`, `stopIfNecessary()` find `m_liveStorageRefs == 0` | `StorageRefAcrossSafepoint` |
| An operation without `collects` neither parks nor gives up access (08, S4) | `OperationValidationScope` (`aot/runtime/AOTRuntimeValidation.cpp:435-464`), which already compares "collected" with the declaration, records `park()`, `releaseAccess()`, `becomeParked()` | `OperationParked` |
| An operation without `waits` waits for no other thread (S5) | the same scope records a wait of 09, W1, the slow branch of `lockParking()`, a blocking primitive, and `runSafepointOperation()` with a closure | `OperationWaited` |
| Every cycle of the call graph has a safepoint (S3') | static (verifier), on the final call graph | `InternalErrorKind::CallCycleWithoutSafepoint` |
| A read with an `empty` edge is used where C3 asks for it | static (verifier): an *operation* of the family `SlotAccess` with `access = Read` whose slot has markers and whose subject may be shared, whatever it relies on; the exception is `GetSlotOrUndefined` on a field that cannot be moved out | `InternalErrorKind::SlotFilledOnSharedSubject` |
| `PutContiguousIndex`, `PutDoubleIndex`, `GrowNewArray`, `TruncateNewArray` only on unpublished arrays (4.6.7) | static (verifier) | `InternalErrorKind::NewArrayIntrinsicOnPublishedArray` |
| The mark of the cell is unused (05, CO15) | validating builds set `TypeInfoPerCellBit` at random in new objects of Shareable layouts | `MarkOfSharedCellWasRead` (any change of behaviour) |
| Owner check was made | `PutElement` takes the `Storage` that `MayWriteStorage` or `AcquireStorage` has proven writable. It re-reads the storage word after the store and fails if it is `Retired` and the array's storage is another one | `StoreIntoRetiredStorage` |
| A frame is restored on its own thread (09, G3) | the test of G3 fails in `EnterSavedFrame` | `FrameRestoredOnOtherThread` |

Per-Instance validation state (`pendingBarrierOwner`, `expectedCall`, photographs of operands) lies in the private part of the view or in `ThreadContext`. A photograph of an operand treats every buffer that is not Confined as shared: `captureBytes` must not report a change that another thread made as `NativeCallChangedOperand`.

## 4.15 Testing switches

`flags TestingSwitch` (`aot/ir/schema/compile-call.def:111-130`):

| Switch | Effect | Use |
| --- | --- | --- |
| `EverythingIsShareable` | sharing analysis answers AnyThread, Shareable, Shared for everything; only "unpublished" remains | **Runs all existing programs of `bun:test/aot/programs` through every new code path on one thread.** They must print what they print today. The cheapest and widest test of this document |
| `NothingIsUnpublished` | in addition, an allocation proves nothing beyond its own operation | the same, harsher |
| `YieldAtWindows` | a call of `sched_yield` (by a random draw, seeded) between the test and the use of each protocol: after `LoadStorage`, between load and compare-and-swap, before a release store | widens race windows (12, section 12.5) |
| `FailPredictions` (exists) | tests fail at random | exercises the edges `empty`, `outside`, `hole`, `miss` and the failing branch of `MayWriteStorage` |
