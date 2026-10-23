# R3. The whole-program flow analysis (`aot/valueflow/`) and what it would take to add thread reachability and thread escape

Read-only research on oven-sh/WebKit#757 (head 5c2923e566cb) and oven-sh/bun#45631 (head 8bb8d41bffe).

Path prefixes used below:

- `vf/` = `Source/JavaScriptCore/aot/valueflow/`
- `aot/` = `Source/JavaScriptCore/aot/`
- `BUN/` = `<bun>/`
- `PR-wk` / `PR-bun` = the two PR texts in ``

Everything marked **[fact]** (or not marked) was read in the source at the cited line. Everything marked **[inference]** or placed
in section 10 ("How the two new results could be added") is my conclusion, not something the code says.

---

## 0. Summary for the design document

1. The analysis is an inclusion-based (Andersen style) points-to / closure analysis with on-the-fly call graph, over *interned sets* of
   abstract members. It is flow-insensitive for the heap, SSA-plus-type-test sensitive inside a body, field sensitive by
   (abstract object, name), and context insensitive except for one narrow cloning device (functions that take a property key as a parameter).
2. **Everything that moves is a set at a node.** Object attributes ("was deleted from", "reached native code"), global flags ("unseen code
   runs"), the list of used built-ins, the escape set: all are ordinary nodes that hold sets, and rules are re-run when the part of a node that
   they watch grows. A new attribute "shared between threads" fits this shape exactly; there are two existing closures to copy
   (`runEscape` and `followWhatNativeCodeHolds`).
3. **There is already a transitive escape closure**: node 0 `escaped()` ("Whatever was handed to code that nobody compiles") with one
   statement of kind `Escape` that follows every property, getter, setter, copy source and prototype link of whatever arrives and enters every
   closure that arrives. It ends as `ObjectMutation::ReachNativeCode` / `ReachUnknownJavaScript` in `LayoutFlow.impossibleMutations`, is
   encoded into the run-time layout table and is checked by `--aot-validate`. This is the end-to-end template for a thread-escape result.
4. **Granularity is the real problem, not the solver.**
   - All instances of a class are *one* abstract object (`instanceOf(layout)`), all closures of a function are one (`closureOf(function)`),
     all promises of an async function are one, all rest arrays of a function are one.
   - Mutation results are folded to *one allocation class per layout*: "what may happen to one object of the layout may happen to every one"
     (`vf/AOTValueFlowResultsObjects.cpp:581`).
   - Captured variables are not fields of the closure object. Immutable captures are entry-parameter nodes of the function; mutable ones are
     fields of an environment layout, one node per (layout, field) "whichever object holds them" (`vf/AOTValueFlowBodies.cpp:1333`).
     No set ever holds an environment (`vf/AOTValueFlowPositions.cpp:850`).
   So "shared" will be a property of a *layout*, a *function's closures*, a *variable* or an *environment field*, never of one object.
5. The value flow of `new Thread(fn, ...args)` / `thread.join()` **is expressible in today's table** (the same words as `Promise.prototype.then`
   and `setTimeout`). What is **not** expressible: *where* a callback runs. `CallbackTimes` says only when and how often
   (`Later | Repeatedly | PerElement | Once | PerEvent`). One new column is needed, plus a place for what the callback throws.
6. Of the 23 `FlowResultKind`s, 18 are unions over all reached code and stay true with threads as long as thread entry functions are in the solve
   (for the ones that type *memory*, with the publication caveat below).
   **Initialization, StoreCounts, ConstructorFieldOrders, ExecutionCounts, ClassEvaluations** (and the "filled at birth"/"sees only finished objects"
   family that hangs on them) are *order* or *count* facts derived from one thread of control. They stay logically true (causality), but what
   compiled code does with them (read without a test) needs a happens-before edge at every publication, which a racy store on ARM64 does not give.
   **OperandSummaries** stays true but loses its main client guarantee (see 5.3).
7. Workers are explicitly *outside* the model: "no object of another VM gets here, so a worker, which runs the program once more in a VM of its
   own, is none of them" (`vf/AOTValueFlowResultsBuiltins.cpp:1184`). Nothing in `valueflow/` knows about a second thread in one VM.

---

## 1. The abstract domain

### 1.1 Members of a set (the "universe")

`vf/AOTValueFlow.h:21`: "A member of the universe: a kind of primitive, one of the two placeholders, an allocation site, the closure of a
function, a built-in, a constant key."

The kinds are `ValueFlowObjectKind` (`vf/AOTValueFlowBodies.h:35-59`) and they are numbered in this order by `numberObjects()`
(`vf/AOTValueFlowBodiesLink.cpp:777-825`):

| Index range | Kind | One member per | Source |
| --- | --- | --- | --- |
| 0..15 | `Primitive` | kind of primitive (`numberOfPrimitiveKinds = 16`; bit number of `PrimitiveSet`, includes `Absent`, `Integer`, `WholeNumber`..) | `Bodies.h:98-99` |
| 16 | `UnknownObject` (**unknown**) | 1 | `Bodies.h:101-102` |
| 17 | (**untracked**) | 1 | `Bodies.h:103-104` |
| then | `Builtin` | entry of the built-ins table (canonical entry stands for it) | `Bodies.h:105-106` |
| then | `ModuleBuiltin` | (built-in, module): `import.meta`, `module`, `require` | `Bodies.h:40-41` |
| then | `Namespace` | module | `Bodies.h:110` |
| then | `Instance`, `Prototype` | **two per layout**: `instanceOf(layout)`, `prototypeOf(layout)` | `Bodies.h:111-112` |
| then | `ResultOfBuiltin` | two per built-in (ordinal 0: what it returns; ordinal 1: "all the objects it makes to put into that one"), plus one per `hostObject` record | `Bodies.h:113-116`, `BodiesLink.cpp:782-783` |
| then | `Closure` | **function** (not creation site) | `Bodies.h:117` |
| then | `CoroutineResult` | function: "The promise of an async function, the object of a generator: one per function" | `Bodies.h:44-45,118` |
| then | `RestArray` | function | `Bodies.h:119` |
| then | `FunctionPrototype`, `ConstructedObject` | function (plain functions without a layout) | `Bodies.h:48-50,120-121` |
| then | `Site` | allocation site = (function, value, ordinal). Origins: `Operation`, `DataNode`, `BuiltinResult`, `TypedResult` | `Bodies.h:73-93` |
| last | `Key` | property key of the program (constant strings and symbols that name properties) | `Bodies.h:57-58,127` |

Details that matter for precision:

- **A call operation is an allocation site for what a built-in allocates**: `ValueFlowSiteOrigin::BuiltinResult`: "What a built-in allocates that a
  call finds as its callee: with ordinal 0 the object it returns, with ordinal 1 all the objects it makes to put into that one"
  (`Bodies.h:77-78`). So `new Map()` at two places gives two abstract Maps. Where no call operation spells the call out (native code calls it,
  or it is called through a group), the two per-built-in objects are shared by all such calls (`Bodies.h:55-56`;
  `vf/AOTValueFlowBuiltins.cpp:727-728`: "A call without objects of its own shares those of the built-in with every other such call").
- **Class instances collapse to one object per class.** The allocation stands in the base-class constructor, so the site is shared by every `new`.
  `objectAllocatedUnder(site, prototype)` maps (site, prototype object of class C) to `instanceOf(C)`
  (`vf/AOTValueFlowBodiesLink.cpp:257-274`; caller `HeapRule::allocateUnder`, `vf/AOTValueFlowRulesHeap.cpp:661-683`).
  `BodiesLink.cpp:896`: "The allocation in the constructor of a base class makes the instances of the classes below it too, and one object stands
  for all of them." For constructor functions: `AllocatedInstances::collect` uses `instanceOf(layout)` or `constructedObjectOf(constructor)`
  (`vf/AOTValueFlowRulesCalls.cpp:942`).
- Object and array *literals* are per site (`runNewObject`, `runNewArray`: `RulesHeap.cpp:685-741`).
- `TypedResult` sites of one layout share one object (`BodiesLink.cpp:804-823`).
- Strings: a constant string is a `Key` member only if the program has a property key with that text; otherwise only the kind `String`
  (`vf/AOTValueFlowBodies.cpp:863-867`). The kind covers the keys ("a set that holds the kind of strings or symbols lists no key of that kind",
  `vf/AOTValueFlowSolver.h:586`).
- Numbers are known by kind only; a separate side analysis bounds them (`ValueFlowNumbers`, `vf/AOTValueFlowResultsInternal.h:141-142`).

### 1.2 The two placeholders

`vf/AOTValueFlow.h:159-168`:

```
// Two members of the universe stand for objects that no set lists one by one.
// An object or a function that compiled code has allocated and that the analysis has stopped following.
ValueFlowObjectID untracked;
// Anything: a value from code that nobody compiles or from an unsound edge, and any object that was handed to such code.
ValueFlowObjectID unknown;
```

Each placeholder is tied to one *covered node*:

- `unknown` stands for all that node 0 `escaped()` holds (`Bodies.h:101,404-405`).
- `untracked` stands for all that node 1 `lost()` holds: "Nothing follows from being here: no function is entered and no object is opened"
  (`Bodies.h:406-407`).

Normal form: "a set that holds a placeholder lists no object that the placeholder covers" (`Solver.h:586`). The graph *retires* nodes that hold
the placeholder and flow into its covered node, so they stop listing members (`vf/AOTValueFlowSolver.cpp:189-211`,
`vf/AOTValueFlowGraph.h:83-96`). Exceptions are always listed: built-ins that let in unseen code (`m_objectsAlwaysFollowed`, `Solver.cpp:170-178`;
"The analysis follows a built-in that lets in unseen code as a value ... and never stops: a set that is given up still lists it",
`vf/AOTValueFlowRulesHost.cpp:876`) and holders of library exports.

Where the unknown object is born is an enum of nine sources, `ValueFlowUnknownSource` (`ValueFlow.h:170-187`): `UndescribedNativeResult`,
`UnseenCode`, `UncheckedConversionOfUntracked`, `DataFromOutside`, `ThrownByRunTime`, `ParameterOfEscapedFunction`, `ContentsOfEscapedObject`,
`ResultOfUnknownCallee`, `ObjectWithoutLayout`. The source is carried in the key of the constant node so reports can walk back to it
(`vf/AOTValueFlowLog.h:72-73`).

### 1.3 Nodes (positions)

Nodes that more than one body names (`ValueFlowProgramNodes`, `Bodies.h:401-462`):

| Node | Key | Line |
| --- | --- | --- |
| `escaped()` = 0, `lost()` = 1 | global | `Bodies.h:405,407` |
| `variable(VariableID)` | **module variable** (also static fields, exports, see 2.5) | `:408` |
| `field(LayoutID, index)` = `environmentField` = `privateField` | **per (layout, field)**, for environment fields, private fields, brands, hidden fields only | `:409-411` |
| `entryParameter(function, index)`; `capture(function, index)` is the same thing ("captures first") | per function | `:413-415` |
| `result(function, index)`, `thrown(function)`, `yielded`, `sent` | per function | `:416-421` |
| `ofHost(ValueFlowHostNode)` | 38 global nodes/flags (`numberOfValueFlowHostNodes = 38`) | `:329-399,422` |
| `hub(ValueFlowBehaviorID)` | one per "behavior" of native code on an operand | `:423-425` |

Nodes inside a body (`ValueFlowBodyNodeKind`, `Bodies.h:489-501`): `EntryParameter, Result, Thrown, Yielded, Sent, Caught, OperationResult,
Filter, Merge`. A body is "what is left of a function when only the nodes stay that a round needs" (`Bodies.h:503`); SSA copies are folded away.

Nodes that rules make on demand, keyed and hash-consed (`ValueFlowMadeNodeKind`, `vf/AOTValueFlowLog.h:20-36`, key `ValueFlowNodeKey` `:70-117`):

| Kind | Key | Meaning |
| --- | --- | --- |
| `Constant` | set (+ unknown source) | holds the set for ever; `add(node, set)` is an edge out of one |
| `Read`, `Written` | **(set of objects, name)** | the heap, see 2.1 |
| `ReadUnderKind`, `WrittenUnderKind` | (set, kind of name) | all names of one kind |
| `Lookup` | (set of receivers, name) | result of a made statement that walks the prototype chain / runs a protocol |
| `CallGroup` | **(set of callees, call kind, marker)**, 14 nodes | see 3.1 |
| `Hub` | behavior | |
| `Masked` | (node, mask, mask kind) | the node filtered |
| `Marked` | `ObjectMutation` (or 6 internal marks) | **"Holds the objects that have one mark"** |
| `Instance` | (node of a body, marker) | the cloning device, see 1.4 |

### 1.4 Sensitivity

- **Context sensitivity: none in general.** One node per parameter, one per result. Three small refinements:
  1. *Instanced functions* (`ValueFlowInstancedFunctions`, `Bodies.h:789-835`; found by `findParametersThatAreKeys`, `vf/AOTValueFlowAnalysis.cpp:41-323`):
     "A parameter is a key where the body stores under it, where the body calls what it has read under it, or where the body hands it on to a
     function that takes a key there." Such a function's body nodes are cloned per *constant key* passed. Limits: `mostMarkersOfCall = 8`,
     `mostInstances = 64`, `mostNodesOfBody = 1024` (`Bodies.h:825-827`). The comment at `Analysis.cpp:97` shows the target:
     `` `this.events[type].call(this, a)` `` i.e. **EventEmitter**.
  2. *Result summaries*: a function that returns one of its parameters gives each caller group its own operand back
     (`ResultSummary`, `Bodies.h:427-443`; `RulesCalls.cpp:1008-1022`).
  3. *Receiver filtering*: "In `o.m(..)` a function runs with a receiver as `this` only where the read of `m` from that receiver answers the
     function" (`RulesCalls.cpp:594`). Applies up to `MaxValueFlowMembersToldApart = 64` receivers (`aot/ir/schema/limits.def:44`).
- **Field sensitivity: yes**, by `ValueFlowName` = (kind, payload) (`ValueFlow.h:117-129`), 37 kinds (`:28-83`; one of them, `PrivateField`, has no user in `valueflow/` that I could find: private fields use the per-layout nodes of 2.2). Array elements are one name
  (`Elements`), no index sensitivity (except `RestArgument` by index and the entries-pair hidden slots `:131-136`).
- **Flow sensitivity: only inside a body and only through SSA and tests.** `Filter` nodes and operand masks carry what a type test lets through
  (`ValueFlowMaskKind`: `Plain, Checked, Declared, Expected`, `ValueFlow.h:147-157`; semantics in `ValueSetMaker::pass`,
  `vf/AOTValueSets.cpp:1462-1490`). The heap, variables and environment fields are flow-insensitive in the solve.
  Order is recovered *after* the solve by separate analyses (`AOTFlowOrder*`, `AOTFlowConstructionOrders.cpp`), see 5.
- **Reachability sensitive**: a body's statements that "leave the body" run, and its `edgesOnceReached` / `seedsOnceReached` apply, only once the
  function is reached (`ValueFlow.h:218-226`, `Bodies.h:601-609`).
- The analysis runs **once, on IR form `AsRead`** (`vf/AOTValueFlowResults.cpp:361`; pipeline row `aot/pipeline/AOTPipeline.cpp:143-145`), after
  `CheckTypesAtEntry`-style splitting: "Only a function of the program is split into the tests of its arguments, its typed body and its generic
  version in front of the analysis" (`Bodies.h:322`). Functions created later by passes map back through `findAnalyzedFunction`
  (`vf/AOTFlowResults.h:318-320`).

---

## 2. How the heap is modelled

### 2.1 The basic mechanism: `read(set, name)` / `written(set, name)`

`vf/AOTValueFlowRuleContext.h:139-141`:

```
// One node for each (set, name). What is written through a set reaches what is read under the same name through every set that shares an object with it.
ValueFlowNodeID read(ClimbingSet objects, ValueFlowName);
ValueFlowNodeID written(ClimbingSet objects, ValueFlowName);
```

So there is **no node per (object, field)**. A store `o.x = v` with `o` in set S makes/finds node `Written(S, x)` and connects `v` to it
(`HeapRule::write`, `RulesHeap.cpp:254-256`). A read through set T makes `Read(T, x)`. The *solver* links `Written(S,x) -> Read(T,x)` when
`S` meets `T`, or when the names "may be the same" (`mayBeTheSameName`, `ValueFlow.h:85-115`: e.g. a write under `UnknownKey` is found by a read
of any `Property` or `Elements`; a read of `EveryProperty` finds `Property, Elements, UnknownKey, Slot, InternalFields, SteppedValues,
ReturnedValue`). Linking is in `ValueFlowSolver::link*` (`Solver.cpp:1890-2105`), per name, with three size classes of sets
(`NodesOfName`: `ofFewObjects`, `ofManyObjects`, `ofVeryManyObjects`, `Solver.h:504-511`) and chains by member for the small ones.

Order of events for one store statement `PropSet o, v key=x` (`HeapRule::store`, `RulesHeap.cpp:596-614`):

1. `receivers = valueOf(o)` (logged as a read of the node, so the statement is due again when it moves).
2. `write(receivers, Property x, v)`:
   - `v -> Written(known(receivers), x)`;
   - if `untracked` in receivers: `v -> Written({untracked}, x)` ("What is stored through a placeholder lies in one node for each name", `:253`);
   - if a built-in object of the realm is among the receivers and native code may read that name: `v -> escaped` (`:260-293`);
   - if `unknown` in receivers: `v -> Written({unknown}, x)`, **`v -> escaped`**, and raise `ObjectNobodyKnowsIsWritten` (`:294-298`).
3. `convertStoredValue`, `noteWrittenMembers` (built-in members written).
4. `markStore`: `mark(objects, StoreAfterCreation)`, and `AddProperty` + `AddedNames` where the layout declares no such field (`:389-413`).
5. `callSettersOfReceivers` (lookup of setters through the chain, entering them through a `Set` call group).
6. `mayThrow()`.

### 2.2 Named fields of compiled objects

At allocation (`runNewObject`, `RulesHeap.cpp:685-707`): each listed value goes to `Written({site}, Property key)`; **except** fields flagged
`IsPrivate | IsBrand | IsHidden`, which go to the per-layout node `nodes().field(originLayout, originField)` (`:702-703`). So `#x` is per class, not
per object set.

### 2.3 Arrays, Maps, Sets, Promises, typed arrays, iterators

- Array literal: `Written({site}, Elements)` (`runNewArray`, `:735-741`).
- Every built-in class declares **slots** in the table (`ClassColumns.slots`, `aot/builtins/generator/vocabulary.ts:639`), e.g.
  `Map: { key: contents("Key", "CollectionEntries"), value: contents("Value", "CollectionEntries") }` (`aot/builtins/map-set.builtins.ts:106`),
  `Promise: { value: contents("Value", "PromiseState"), reason: state("PromiseState") }` (`aot/builtins/promise.builtins.ts:97`).
  A slot becomes a heap name: `BuiltinApplication::nameOf(slot)` (`vf/AOTValueFlowBuiltins.cpp:1245-1253`): slots in heap `Elements` /
  `TypedArrayElements` map to the `Elements` name; slots of `BoundFunction` map to `HiddenSlot`; all others to `{Slot, slot.index()}`.
- Promise: `promiseValue`/`promiseReason` slots (`vf/AOTValueFlowRulesPromises.cpp:17-18`). `new Promise(executor)` is special-cased so that "the
  promise and the function that resolves it are objects of the call" (`vf/AOTValueFlowBuiltins.h:86-87`;
  `applyConstructionOfPromise`, `RulesPromises.cpp:181-211`): resolve function holds `ResolvedPromises -> promise`. One *reject* function object
  is shared by all promises ("What a promise is rejected with escapes, so one function may reject them all", `:187`).
- `await v`: `lookup(values, AwaitedValue)` (`runAwait`, `RulesPromises.cpp:53-64`); the made statement `runAwaitedValue` (`:155-179`) answers non-thenables
  as themselves, the `value` slot of promises, and asks thenables. The rejection reason flows to `thrown`.
- Async function / generator: one `CoroutineResult` object per function; "One object stands for the promises or the generators that all calls of the
  function answer" (`vf/AOTValueFlowRulesCoroutines.cpp:42`). `resolvePromises(coroutine, result(function,0))`,
  `rejectPromises(coroutine, thrown(function))`.
- Iterators: names `SteppedValues`, `ReturnedValue`, `SentValues`, `OpenedBy` (`ValueFlow.h:56-61`); protocol lookups `OpenedIterators`,
  `IteratedValues`, `IteratorResults` (`:67-71`).

### 2.4 Closures and captured variables

`NewClosure` has no rule; the body reduction emits (`vf/AOTValueFlowBodies.cpp:892-897`):

```
setConstant(id, constantOf({ ValueFlowObjectKind::Closure, value.function().index(), { } }));
for (index ...) connect(captures[index], NodeName::entryParameter(value.function(), index));
```

So: (1) the closure value is the constant `closureOf(function)`; (2) every captured value flows straight into the callee's entry-parameter node,
for all closures of that function together; (3) **nothing is written "into" the closure object**, so no heap walk from a closure finds its captures.

Mutable captured variables live in environments: `NewObject` with a layout of kind `Environment`. "The fields of an environment are nodes of the
program, whichever object holds them" (`Bodies.cpp:1333`); `GetEnvironmentField`/`PutEnvironmentField` read/write `field(layout, field)` directly
(`Bodies.cpp:885-891`). "An environment and an opaque word are no values of the language, and no set stands for one" (`vf/AOTValueFlowPositions.cpp:850`).

Own properties stored on function objects (`f.x = 1`) use the ordinary `Written({closureOf(f)}, x)`.
Bound functions: `HiddenSlot` names ("A slot that no code reads out of the object, whoever holds the object: what a bound function was made of",
`ValueFlow.h:52-53`).

### 2.5 Module variables, exports, static fields, globals

- Module variable: one node `variable(VariableID)`; `GetModuleVariable`/`PutModuleVariable` (`Bodies.cpp:878-884`).
- "A variable may be a property of an object at once: what a module exports lies on its namespace object, and a static field lies on its class
  object" (`ResultsInternal.h:115`). The namespace object is a universe member per module; the rules tie it to the exported variables
  (`tieNamespaceToExports`, `vf/AOTValueFlowRulesModules.cpp:423-424`). "One object stands for the class objects of all evaluations of a class"
  (`vf/AOTValueFlowResultsVariablesOnObjects.cpp:616`).
- Globals: `globalThis` is a `Builtin` member; its declared members come from the table, everything else is `Written({GlobalThis}, name)`.
  A store into a built-in object of the realm escapes unless the table says native code does not read that name (`RulesHeap.cpp:260-293`,
  `namesReadByNativeCode`, `aot/builtins/generator/lib.ts:1158-1161`).
- **Native cells**: "Memory of native code that JavaScript reaches only through the entries that name the cell. There is one per name in a
  program" (`lib.ts:378-379`). Modelled as a `Slot` on the global object: "It has no holder, so the global object stands in"
  (`vf/AOTValueFlowBuiltins.h:56`; `Builtins.cpp:2452-2479`). E.g. `Timeout.callback`, `Server.fetch` (`BUN/src/codegen/builtins/host/native-cells.ts`).
- `NativeMemory` as a target is simply `escaped()` (`forEachTargetAt` default case, `Builtins.cpp:1655-1657`); as a source it is the undescribed value
  (`:1495-1497`).

### 2.6 Prototype chains and lookups

`PrototypeLink` is a heap name. A property read makes a `Lookup(start set, name)` statement (`HeapRule::read`, `RulesHeap.cpp:163-166`) whose rule
(`readThroughChain`, `vf/AOTValueFlowRulesHeapLookups.cpp:1170-`) computes the chain, connects every `Read(chain objects, name)`, adds `undefined` where
the lookup may find nothing, reads table members, and enters getters through a `Get` call group.

---

## 3. Calls, and what native code does

### 3.1 Call groups

`vf/AOTValueFlow.h:283-297`: "What every call of one set of callees shares. A call site has one edge per operand into it and one out of it."
A group has nodes `thisValue, newTarget, argument(0..7), otherArguments, laterArguments, result, thrown` (`numberOfGroupedArguments = 8`).
The key is (set of callees, `ValueFlowCallKind`, marker) (`Log.h:77`).

`ValueFlowCallKind` (`ValueFlow.h:263-274`): `Call, Construct, Get, Set, CallByNativeCode, ConstructByNativeCode, CallByHost`.
`CallByHost`: "A call that native code makes where no call of the program is under way, with operands that a record of the table lists. No
statement of the program leads to what is entered, so it is no seen code for that."

Order of events for `Call callee, this, args...` (`runCall`, `RulesCalls.cpp:782-835`):

1. `callees = valueOf(operand 0)`; `applyEventsOfCall` (is an evaluator / loader among them? see 4.3).
2. `thrown += unknown value (ThrownByRunTime)`.
3. Library functions that a table entry describes are *applied* as entries, not entered (`:792-796`).
4. Closures: `passOperandsToInstances` (key cloning), then `passOperands` into `group(closures, kind)`.
5. `callListedBuiltins`: "The entry of a built-in and a bound function are applied with the operands of this call" (`:40`), i.e. built-ins are
   **call-site sensitive** when the call operation lists them.
6. Placeholders among the callees: `callPlaceholder` / `callMemberNobodyKnows` / `callOperandOfDescribedCall` (`:812-824`).

The made statement `runCallGroup` (`RulesCalls.cpp:1025-1105`) runs once per key and calls `enter()` per closure (`:968-1023`):
`context.reach(function)`; connect group operands to entry parameters; connect `thrown(function) -> group.thrown`, `result -> group.result`.
If `unknown` is among the callees: `letUnknownCodeWrite`, **all operands -> `escaped`**, result and thrown get the unknown value (`:1093-1104`).

Direct calls (`CallDirect`) have no statement: edges at reduction time plus `functionsEnteredOnceReached` (`Bodies.cpp:924-948`).

**Consequence for "who called whom":** the group statement has no function ("None for a statement that a rule has made", `RuleContext.h:86-87`).
Caller identity is lost in the solve and is recovered after it by walking "what the last run of the statement got from read(), written(), lookup(),
group(), hub() and masked()" (`forEachMadeNodeAskedForBy`, `Solver.h:378-397`); see `EntriesFromFunction::collect`
(`vf/AOTValueFlowResultsFunctions.cpp:427-459`). And: "The solve does not keep which row of the table has entered which closure"
(`ResultsFunctions.cpp:286`).

### 3.2 Applying a built-in (`BuiltinApplication`, `vf/AOTValueFlowBuiltins.cpp:184-2400`)

`run()` (`:200-237`): record the use; find the dispatch tree for the mode; `select()` every overload whose parameter types the operand sets may
meet; `apply()` each. `apply(overload)` (`:702-777`), in order:

1. `capability == JS && !RunsOnlyOperandCode` -> `noteCallThatRunsOtherCode`.
2. If the overload is implemented by (or modelled by) a library function in the program: **enter that function** with the call's operands and return
   (`:717-723`, `enter`, `:1066-1085`).
3. Choose `m_resultObject` / `m_contentsObject` (site objects of the call, else the per-built-in ones).
4. `applyParameter` for receiver and each parameter (`:1114-1164`):
   - `uses == all` -> `handToUndescribedNativeCode` (`:1095-1112`): operand -> `escaped`, `letUnknownCodeWrite`, `letWhateverHasEscapedBeCalled`, result and
     thrown are undescribed values.
   - `WritesContents | MovesElements` -> `mark(StoreAfterCreation)`.
   - protocols -> connect the operand to the **hub** of that behavior (one node per distinct (protocols, transitive?, named members, called
     members), `ValueFlowBehavior`, `vf/AOTValueFlowBuiltinTables.h:26-33`); `runBehavior` (`Builtins.cpp:3212`) reads members, calls members, converts, iterates.
5. `flow.transfers` (`connect(from, to)`), `flow.callbacks` (`applyCallback`), `flow.handlerCalls`, `flow.propertyActions`.
6. Result: `valueOfType(result.type, m_resultObject)`.
7. Thrown: unknown value if `capability == JS`, unknown object if `throws`.
8. Undeclared host overload -> `letUndeclaredNativeCodeRun`.

**Default is conservative**: "an operand that may hold an object and has no uses" gets `everything`
(`aot/builtins/generator/build-uses.ts:1022-1026`, `applyWrittenUses`); an entry without a
declaration is `undeclaredSignature`: `any.rest`, `capability: "js"`, `throws: true`, `isUndeclared` (`lib.ts:891-902`).

### 3.3 Callbacks: what the table says and what flow does with it

Table vocabulary (`vocabulary.ts:170-174,305-321`):

```ts
export const callbackTimes = ["Later", "Repeatedly", "PerElement", "Once", "PerEvent"] as const;
export const callbackResultUses = ["Escapes", "Ignored", "Tested", "Returned", "Stored"] as const;
export interface CallbackSpec {
  times: CallbackTimes; constructs?: boolean; newTarget?: PlaceSpec; thisValue?: PlaceSpec;
  arguments?: readonly PlaceSpec[]; spreadArguments?: PlaceSpec; result?: CallbackResultUse; resultTo?: PlaceSpec;
}
export interface HandlerSpec { times: CallbackTimes; thisValue?: PlaceSpec; arguments?: readonly PlaceSpec[]; result?: CallbackResultUse; }
```

"Later is one call after the built-in has returned, PerEvent any number of them: once per request, message or event"
(`aot/builtins/AOTBuiltinTable.h:300-304`, `isCalledAfterReturn`).

`applyCallback` (`Builtins.cpp:2160-2188`): for each part of the callee set, `group(callees, CallByNativeCode | ConstructByNativeCode)`; `this` from the
place or `undefined`; `passArguments`; `spreadArguments -> group.otherArguments`; `useResult`; `connectThrown`.

- `useResult` (`:2121-2143`): `Ignored`/`Tested` nothing; `Escapes` -> `escaped`; `Returned` -> result node; `Stored` -> the target place (through
  `AwaitedValue` if the place is a promise's value slot).
- `connectThrown` (`:2145-2152`): "What a function throws that is called after the built-in has returned goes to native code": `Later`/`PerEvent` ->
  `escaped`; else -> the call's `thrown`.
- Only for a promise value slot: "A promise that is resolved with what the function answers is rejected with what the function throws" (`:2179-2186`).

**`times` is used in exactly these places** (grep `isCalledAfterReturn|BuiltinCallbackTimes::`): where thrown goes (above); `FlowIndirectCall.times` for
heat (`aot/analysis/AOTHeatGraph.cpp:59-61,366-372`) and inlining (`aot/passes/AOTSummarizeForInlining.cpp:124-126`); construction orders accept only `Once`
(`vf/AOTFlowConstructionOrders.cpp:1411`); native-call validation ("ran code during the call?", `aot/runtime/AOTNativeCallValidation.cpp:366-368`); generator checks
(`aot/builtins/generator/check.ts:559-563,613`: a callback that is called before return forces `capability: "js"`).
**In the points-to solve "now" and "later" are the same thing** (flow-insensitive).

### 3.4 The specific cases asked about

**`setTimeout(fn, ms, ...args)`** (`BUN/src/codegen/builtins/host/globals.builtins.ts:30-54,1750-1751`):

```ts
const scheduled = (times: b.CallbackTimes) => ({
  callback: [
    b.storedIn(b.nativeCell(timerCallbackCell)),
    b.called({ times, this: b.result, spreadArgs: b.operand("passed"), result: "Ignored" }),
  ],
  passed: [b.storedIn(b.nativeCell(timerArgumentsCell))],
});
const timerAfterDelay = (times, delay, capability) => ({
    args: { callback: b.callable, delay, passed: b.any.rest },
    returns: b.instance("Timeout").fresh,
    capability, throws: true, uses: scheduled(times),
    reads: externalState, writes: externalState, implementation: b.hostCall });
b.fn("setTimeout", delayedTimer("Later")), b.fn("setInterval", delayedTimer("PerEvent")),
```

`fn` does **not** escape: it is entered with exactly `passed` and the Timeout as `this`, and is kept in the global cell `Timeout.callback` from where
`%Timeout%.prototype._onTimeout` can read it (`globals.builtins.ts:1755-1777`). What it throws escapes.

**`promise.then(f, g)`**: `aot/builtins/promise.builtins.ts:25-42,215-231`, quoted in 7.3. Pristine promise: `f` is called `Later` with `slot(this, value)`, its result is
`Stored` in `resultSlot("value")`. Non-pristine receiver: reactions' results `Escape`, receiver `retained`, species run.

**`queueMicrotask(fn)`**: `called({ times: "Later", result: "Ignored" })` (`globals.builtins.ts:1807-1814`).

**`addEventListener(type, listener)`**: listener `retained` (escapes) and `called({ times: "PerEvent", this: eventTarget, args: [b.object], result: "Escapes" })`,
plus `runs(calls("handleEvent"))` (`globals.builtins.ts:144-164`).

**`new Worker(url)`** (`BUN/src/codegen/builtins/host/handwritten-classes.builtins.ts:20-25,141-151`):

```ts
constructorOnly("Worker", { extends: "EventTarget", construct: [
  { ...startedWorker, args: { url: b.string, options: b.undefined }, uses: { url: [b.loadedAsModule] } },
  { ...startedWorker, args: { url: b.any, options: b.any.optional },
    uses: { url: [b.everything, b.loadedAsModule], options: [b.everything, b.loadedAsModule] } } ] }),
```

- `loadedAsModule` = role `ModuleSpecifier`, which sets the entry flag `LoadsModuleByName` (`aot/builtins/generator/build-overloads.ts:55-57`). Calling it raises the
  event `UnseenCodeCause::LoadsModuleByName` (`eventOfCalling`, `RulesHost.cpp:734-747`), which evaluates every module that may answer to a name
  (`runModulesLoadedByName`, `:1188-1234`).
- `Worker` is listed under `seenCodeEntries` with `runsUnseenCode: false`: "A worker has a heap of its own and starts with an entry point of the
  build: the loader of its realm refuses everything else" (`BUN/.../unseen-code.builtins.ts:41-51,318`).
- The worker file is an extra **entry** of the program (`BUN/docs/bundler/aot-compatibility.mdx:659-687`), so `runHostRoots` evaluates it like the main entry. There is a
  host-read context `"Worker"` (`vocabulary.ts:221`; `BUN/.../host-reads.builtins.ts:9-10`).
- **The analysis does not separate the worker's copy of the heap from the main one**: module variables, classes and sites of a module evaluated in both are the
  same abstract things. That is sound because it is a union. `ProgramInstances` deliberately ignores workers (`ResultsBuiltins.cpp:1184`).

**`postMessage(obj)`** (global, i.e. inside a worker; `globals.builtins.ts:203-217,1609-1617`):

```ts
const cloned = [ b.readsContents(),
  b.runsTransitively("OwnKeys", "OwnValues", "PrototypeChain", b.named("name", "message", "stack", "cause")) ];
const transferList = [b.runsTransitively(b.named("transfer"), "Iterate"), b.writesContents()];
b.fn("postMessage", { capability: "js", runsOnlyOperandCode: true,
  args: { message: b.any.optional, options: b.any.optional }, throws: true,
  uses: { message: cloned, options: transferList }, reads: heapsReadByClone, writes: heapsWrittenByTransfer, implementation: b.hostCall }),
```

Structured clone = "native code walks the object transitively and may run its getters; it keeps nothing". The message is **not** retained and does not
escape. `structuredClone` is the same with `returns: b.any.fresh` (`:1817-1836`). What arrives on the other side (`onmessage`) is whatever native code passes:
`eventHandlerAttribute`: `called({ times: "PerEvent", this: eventTarget, spreadArgs: b.any, result: "Escapes" })` (`:177-194`).

**`worker.postMessage(obj)`** (method of the Worker instance): I found **no declaration**. `Worker` is declared `constructorOnly` = `hasPrototype: false`
(`handwritten-classes.builtins.ts:17-18`). **[inference]** the read of `postMessage` then answers a value nobody knows and the call goes through
`callMemberNobodyKnows` (`RulesCalls.cpp:667-676`): `letUnknownCodeWrite` and all operands -> `escaped`. `MessagePort::postMessage` appears only as a *native caller* in
`nativeCalls` lists (`BUN/.../runtime-dependencies.builtins.ts:488-489`).

**`EventEmitter`**: `node:events` is JavaScript of Bun's library, compiled with the program (function role `Library`). No table rows model `on`/`emit`; flow follows the code.
Precision comes from the instanced-function device (1.4). PR-wk:178 still lists `EventEmitter.emit` among the worst idioms.

### 3.5 Entering a function from native code

`enterFromNativeCode` (`RulesHost.cpp:114-152`): `reach(function)`; every non-capture entry parameter gets the unknown value
(`ParameterOfEscapedFunction`), unless a declared type is "assumed by every entrypoint" or the table names the function and the parameter is annotated
(`passFromNativeCode`, `:74-112`); results and thrown -> `escaped`. **Captures are skipped** (`:127`): they keep what `NewClosure` gave them.

---

## 4. Roots, reachability, open programs

### 4.1 Roots

- "Body i is function i; the last body is the host's, which is reached from the start" (`Solver.h:35`). `solve()` calls `reach(numberOfBodies - 1, {})`
  (`Solver.cpp:3620`) and makes the one `Escape` statement due (`:3621-3628`).
- The host body has four statements: `HostRoots, UsedBuiltins, UnseenCode, ModulesLoadedByName` (`describeHost`, `RulesHost.cpp:39-49`).
- `runHostRoots` (`:692-732`): for each `program.entries`: add the namespace to `ModulesOfSeenCode`, `evaluateModule(entry.module)`, apply `hostReads` (e.g. the
  default export is passed to `Bun.serve`, `then` is called: `BUN/.../host-reads.builtins.ts`). `record Entry wire { module, hostReads }` (`aot/ir/schema/program-records.def:88-89`).
- `makeHostBody`: "What the host does of its own accord: it enters the entry points and the functions it calls, with the unknown object as every operand" (`Bodies.h:633-634`).
- `runUsedBuiltins` (`:1058-1147`): whatever hangs on a used built-in comes in with it: library code behind it, `loads`, `callsFunctions`, start-up modules.

### 4.2 "A function that nothing reaches isn't compiled"

1. `context.reach(f)` logs to the thread's round log (`RuleContext.cpp:849-855`); `takeReachedFunctions` sorts and calls `ValueFlowSolver::reach`
   (`Solver.cpp:612-631,421-447`): sets `m_isReached`, remembers **one** witness (`m_reachedBy`), makes the body's leaving statements due, wakes
   statements that asked `isReached(f)`, and transitively reaches `functionsEnteredOnceReached`.
2. `computeFunctionResults` (`ResultsFunctions.cpp:863-869`): `reachability = Unreached` for a function that is not reached, unless the host calls it
   (`functionsCalledByHost`) and it is not "entered on demand".
3. `removeUnreachedBodies` (`vf/AOTFlowApplyFunctions.cpp:73-80`) removes the body; `cutCallsOfUnreachedVersions`, `dropDataOfUnreachedCode`.
4. Later, `RemoveUnreachableFunctions` works on the `CallGraph` (`aot/passes/AOTRemoveUnreachableFunctions.h`).
5. "Unsolved code": "The solve has not reached the function, and its operations are compiled all the same: the host may enter it" (`ResultsInternal.h:359-365`).
   Results treat what such code touches as unknown.

### 4.3 How a program becomes open

- Entry flags: `EvaluatesText`, `LoadsModuleByName`, `RunsUnseenCode`, `StartsInstance` (`aot/builtins/AOTBuiltinTable.h:153-170`).
- Only a *call* counts. `applyEventsOfCall` (`RulesHost.cpp:877-904`) looks at the built-in functions among the callees of a call by **seen code**, asks `eventOfCalling`, and
  `raiseEvent` -> `letUnseenCodeRun(reach)` (`:182-189`), which raises host flags by adding the unknown object to `UnseenCodeRuns`, and depending on
  `UnseenCodeReach` also `UnseenCodeHasLoader`, `UnseenCodeChangesRealm`.
- Evaluators are always followed as values, even through given-up sets (`m_objectsAlwaysFollowed`).
- What is *not* tracked is refused at run time: a closed image throws `EvalError` (PR-wk:71,146; `BUN/docs/bundler/aot-compatibility.mdx:263-274`).
- After the solve: `collectUnseenCode` (`ResultsBuiltins.cpp:690-713`): "The image admits at run time what its records name and refuses the rest, so they name what the solve has
  let in, no more and no less." `flagsForUnseenCode` (`aot/image/AOTImageBuilder.cpp:578-593`) sets `ImageFlag::AdmitsUnknownJavaScript` iff a record's cause is one of
  `EvaluatesText, ImportsUncompiledModule, RunsUnseenCode, OptionAdmitsCode` (`vf/AOTFlowUnseenCode.h:15-27`). "Without AdmitsUnknownJavaScript the proofs in the code hold only while
  nothing but the code of the image runs" (`aot/image/AOTImageFormat.h:117`).

### 4.4 What an open program does to the results

**Not** "every set gets the unknown placeholder". `runUnseenCode` (`RulesHost.cpp:1149-1186`) is headed "One more caller without types, not a second world". It:

- adds `globalThis` to `escaped`;
- if the realm may change: `letAnythingHappenTo(all host objects of the realm)`;
- hands loaded modules (or library modules only) to native code;
- uses every `EvaluatesText` built-in as `Escaped, Called`; `letHostChooseOperands`; uses `UndeclaredNativeCode`;
- exposes module objects only if `UnseenCodeChangesRealm`.

And `runEscape` then applies `letAnythingHappenTo(escaped)` (all marks, unknown value under `UnknownKey`, unknown getters/setters, unknown prototype link;
`RulesHost.cpp:211-226`) and `letUnseenCodeCreateRecords`. "Unseen code opens what it can reach. Only the option that lets any code in, at any place, opens the whole program"
(`vf/AOTFlowUnseenCode.h:35-39`). Objects that never escape keep all their proofs.

The one total switch: `build.options().admitsUnseenCode()` (only `AdmittedCode::Inspector`, `aot/ir/schema/compile-call.def:128`): "Code that comes in anywhere at run time does anything to
anything" -> `createUnknownResultTables`, no solve (`vf/AOTValueFlowResults.cpp:364-366`).

Two intermediate states are worth copying for threads:

- **Escaped but read-only**: "Code that nobody compiles stores into the objects that it was handed. Until then it only reads them" (`UnknownCodeWrites`, `Bodies.h:340-341`;
  test `aot/tests/valueflow/placeholders/escaped-object-is-read-only-until-unknown-code-writes.ir`).
- **Held, not called**: `HeldByNativeCode`: "What native code holds only through the exports of a module of the library: it keeps these and calls none of them" (`Bodies.h:352-353`).

---

## 5. The 23 `FlowResultKind`s

`aot/ir/schema/passes.def:26-30`. **A `FlowResultKind` is a switch, not a data structure**: `BuildContext::isEnabled(kind, subject)` tests a bit of
`disabledFlowResults` and a range of subject ids (`aot/support/AOTCompileOptions.cpp:106-119`); it exists for bisection and tests (`# OPTIONS: disable=flow:<FlowResultKind>`,
`aot/tests/README.md:371-375`). The data are the records of `aot/ir/schema/flow-records.def` and `FlowResultTables` / `FlowApplication`
(`vf/AOTFlowResults.h:137-192`, `vf/AOTFlowApplication.h:214-233`).

Classes used in the last column:

- **U** ("union fact"): true because *no reached code* does X, or *all* reached code does only Y. Independent of the number of threads **provided** every function that a thread can run is
  reached in the solve with the right operands.
- **P** ("publication fact"): a U fact about *values*, plus an assumption that a reader never sees the location *before* its first store. Logically true under any interleaving (the reader
  got the reference causally after the store), but on a weakly ordered machine it needs a release/acquire pair or a fence where the reference is published.
- **O** ("order/count fact"): derived from program order, dominance, "outside every cycle", "runs at most once". Needs re-examination.

| # | Kind | Meaning (producer) | Where it lands | Main consumers | Threads |
| --- | --- | --- | --- | --- | --- |
| 1 | `ProvenLayoutSets` | A position's type lists the layouts that arrive (`withinLimits`, `Positions.cpp:743`); `AllocationState::NeverAllocated` (`ResultsObjects.cpp:591`) | `FlowPositionUpdate.type/isProven`, `LayoutFlow.allocation` | everything typed: `ResolveAccesses`, `PlaceTypeTests`, `EliminateGuards`, slot representations | **U** for the set, **P** for "the slot holds a member of it" |
| 2 | `NarrowedTypes` | Master switch of `ValueFlowPositions::decide` (`Positions.cpp:853`); a `Check` that always passes becomes `AssumeType` (`ResultsTypes.cpp:1784`, `applyCheckUpdates`) | positions, `FlowBodyUpdate.proofs/claims` | all passes via types; `FlowRemovedChecks` statistic | **U** / **P** |
| 3 | `RemovedNullish` | `undefined`/`null` dropped from a type where they never arrive (`Positions.cpp:874`) | types | null-test removal, fault-based tests, raw representations | **U** / **P** (an unfilled slot reads as empty/undefined) |
| 4 | `LiteralValues` | string/symbol/RegExp literals listed in types (`Positions.cpp:747`); created symbols as constants (`ResultsSymbols.cpp:49`) | types, `FlowSymbolConstant` | `FoldConstants`, `SwitchString`, `FoldConstantRegExps` | **U**; the "symbol created once" part is **O** (rests on ExecutionCounts) |
| 5 | `ExactFunctions` | function part of a type lists the functions (`Positions.cpp:754`) | types | `ResolveCalls`, inlining, closure tests | **U** |
| 6 | `ProvenContents` | what an array/Map/Set/Promise "that the program allocates and that nobody else gets hold of" holds (`ValueFlowContainerFacts`, `ResultsInternal.h:38`; `Positions.cpp:474`) | `ClassPart.contents/contentsAreProven` | element loads without test, `ExpandProvenIterations`, `FuseArrayChains` | **U** for the set. Native containers themselves are not safe to mutate concurrently: separate problem |
| 7 | `ArrayStorageKinds` | the indexing kind of a proven array; "The kind rests on what is stored" (`Positions.cpp:478-479`) | `ClassPart.arrayStorage` | `ElementAccessEmitter`, array intrinsics | **U** as a set of kinds; but a *transition* by another thread between check and access is a memory-safety issue where more than one kind is possible |
| 8 | `PristineInstances` | instance never gets own names / other prototype / odd storage (`builtinBehaviorMutations`, `ResultsContents.cpp:62-67`) | `ClassPart.provenPristine` | overload selection `pristine(...)`, inline built-ins | **U** |
| 9 | `ConstantBuiltinLocations` | member of a built-in holder is never written (`ResultsBuiltins.cpp:756,1145`) | `BuiltinLocationFlow`, `constantMembers`, `intactHolders` | `ResolveBuiltins`, `BindGlobalNames`; the run time *locks* them | **U** (and enforced by locks at run time) |
| 10 | `CalleeSets` | complete set of functions/built-ins at a call (`ResultsCalls.cpp:113`); `CallerState::AllKnown` (`ResultsFunctions.cpp:903-909`) | `FlowCallStamp`, `FunctionFlow.callers`, capabilities `IsNeverCalledByHost`, `HasOnlyDirectCallers` | `ResolveCalls`, `CallThroughMethodTable`, `BuildCallGraph`, `ChooseCallingConventions`, `RemoveUnusedParameters` | **U** |
| 11 | `ReceiverSets` | complete set of layouts at an access (`ResultsAccesses.cpp:502`) | `FlowAccessStamp` | `ResolveAccesses`, `RemoveUnreadFields` (`aot/passes/AOTRemoveUnreadFields.cpp:775`) | **U** |
| 12 | `ClassBases` | what a class extends (`computeBase`, `ResultsClasses.cpp:299-328`) | `LayoutFlow.baseState/base/baseClass` | layouts, method table, `super` | **U** |
| 13 | `AppendedFields` | properties added after creation become fields of the layout (limit `MaxAppendedFieldsPerLayout = 96`) (`ResultsFields.cpp:512,607`) | `FlowLayoutUpdate.appendedFields` | `PlanSlots` | **U** for the list; whether the slot is filled is dynamic (order word) |
| 14 | `HolderMutations` | **what can never happen to an object of a layout**: `impossibleMutations`, `FieldStability`, `MemberMutability::Fixed`, `sitesOfObjectsThatNeverChange` (`ResultsObjects.cpp:597-611`, `ResultsFields.cpp:567-572`, `ResultsClasses.cpp:722-790`) | `LayoutFlow`, `AllocationClass`, `FieldFlow.stability`, `ClassFlow` | guards dropped (`EliminateGuards`), `FlattenInnerObjects`, `AllocateInnerObjects`, slot representations, `ExpandOwnKeys`, run-time layout table | **U**. `MemberMutability::Fixed` also needs `isThereFromTheStart` ("there from the first moment that anybody has the holder", `ResultsClasses.cpp:755`): **P** |
| 15 | `OwnNames` | complete list of keys ever added; prototype/static names complete (`ResultsClasses.cpp:792-795`, `ResultsFields.cpp:1079-1083`) | `addedKeys(+AreComplete)`, `prototypeNamesAreComplete` | `ExpandOwnKeys`, absent-name answers | **U** |
| 16 | `ClassEvaluations` | the class definition runs at most once (`ResultsClasses.cpp:719`) | `ClassFlow.evaluations` | class object/prototype as constants, statics in variables | **O** (count). True if module evaluation stays once per realm; a class defined inside a thread function is not at-most-once anyway |
| 17 | `StoreCounts` | `WriterState::AllKnown`, `StoreCount::Never/AtMostOnce`, `StoreNaming`, `isUnchangedDuringActivation` (`ResultsLocations.cpp:437-466`, `ResultsFields.cpp:565,578-582`) | `LocationFlow` | `NarrowConstantVariables`, `EliminateRedundantReads`, `DeferDefinitions`, `DeferInitializers`, `PromoteEnvironmentsToVariables`, `aot/ir/AOTMemoryLocationAccesses.cpp` (aliasing/effects) | `AllKnown`, `Never`: **U**. `AtMostOnce`: **O**+**P**: consumers treat "stored at most once" + "initialized" as *constant after the store* |
| 18 | `Initialization` | `StoredBeforeEveryRead`, `SetAtCreation`, `readsAfterInitialization`, `undefinedAtStartIsNeverRead`, dead-zone removal (`ResultsOrder.cpp:228-254`, `ResultsFields.cpp:574,989,1428,1453`, `Positions.cpp:858,878`) | `LocationFlow.initialization`, `Occupancy`, `functionsSeeingOnlyFinishedObjects` | TDZ-check removal (`aot/codegen/AOTBindingEmitter.cpp`), reads of slots without "is filled" test (`aot/analysis/AOTPropertyAccessSlots.cpp`), raw slot representations | **O**+**P**: the most thread-sensitive kind |
| 19 | `ConstructorFieldOrders` | the orders in which constructors/literals create keys (`ResultsFields.cpp:1156,1223,1423`, `vf/AOTFlowLiteralOrderResults.cpp:233`) | `LayoutFlow.constructorOrders`, `fieldCreation`, `literalKeyOrders` | `place/AOTKeyOrders.cpp`, order word, `ExpandOwnKeysOrders` | **O** but about an object *before it leaks* (thread-local by construction); **P** afterwards |
| 20 | `OperandSummaries` | what becomes of a value that arrives at an entry parameter / environment field (`computeOperandSummaries`, `vf/AOTValueFlowResultsEscape.cpp:695-739`) | `parameterSummaries`, `storedValueUses` | `EscapeAnalysis`, `AllocateInFrames`, `BorrowedParameters`, `EffectSummaries` (`aot/ir/AOTEffectSummaries.cpp:589`), `SinkAllocations` | **U** for "does not retain/return". See 5.3 |
| 21 | `ObjectUses` | who only calls / never reads / never compares: `ClosureUse::CalledOnly`, `ReadState`, `KeyEnumeration`, `closuresAreNeverCompared`, `NamespaceUse`, legacy RegExp statics (`ResultsFunctions.cpp:910,1076`, `ResultsLocations.cpp:461`, `ResultsBuiltins.cpp:1213`) | `FunctionFlow.closureUse`, `LocationFlow.reads`.. | slim/deferred closures, `RemoveUnreadFields`, `RemoveUnreadExports`, key-order elision | **U** |
| 22 | `ReachedFunctions` | `FunctionReachability::Unreached` (`ResultsFunctions.cpp:864`); also gates `ProgramInstances` and the unseen-code records (`ResultsBuiltins.cpp:705,1188`) | `FunctionFlow.reachability` | `removeUnreachedBodies` | **U** |
| 23 | `ExecutionCounts` | function runs at most once (`computeExecutionResults`, `vf/AOTValueFlowResultsExecutions.cpp:94-133`) | `FunctionFlow.executions` | heat, `PromoteEnvironmentsToVariables`, `DeferInitializers`, `EmbedConstantCollections`, `ExpandData`, site-cache placement (`aot/place/AOTSiteCachePlacement.cpp`), `hasOneClosure` | **O** (count) |

### 5.1 How the O facts are derived, and where one thread is assumed

**ExecutionCounts** (`ResultsExecutions.cpp`): seeds are module bodies that "only the evaluation of the module enters" (`:64-79`), and "A module of the image is evaluated at most once,
whatever its kind: no cache lists it, and a body that has thrown is not run again" (`:81`). Propagation: "The one operation that enters a function runs at most once if it lies outside every cycle
of a function that does. An accessor, a protocol or a callback that enters the function too is in no stamp" (`:115`), requiring `numberOfEnteringOperations == 1`,
`!functionsEnteredWithoutStamp`, `callers == AllKnown`.
A function handed to `new Thread` is entered by a native callback, hence in `functionsEnteredWithoutStamp`, hence never `AtMostOnce`. So the *count* survives threads **iff the module loader
guarantees one evaluation per realm under concurrent `import()`/`require()`**. That is a run-time obligation, not a flow one.

**StoreCount::AtMostOnce** for variables/environment fields (`findLocationsStoredAtMostOnce`, `ResultsLocations.cpp:408-432`): "The one operation that stores to a location runs at most once if it lies outside
every cycle of a function that does." For object fields: nothing stores after creation (`ResultsFields.cpp:578-582`).

**Initialization of variables and environment fields** (`AOTFlowOrder*`): input is a list of `FlowCall {caller, site, callee}` where "The caller is host() and the site none for an entry that no
operation of the program spells out" (`vf/AOTFlowOrder.h:28-35`). Core rules:
- "A function is entered only by a call that flow has linked, after the caller was entered. The entries so far say when somebody can first reach the function" (`vf/AOTFlowOrderOwner.cpp:597`).
- `FlowScopeEntry {Unknown, AfterPoint, Never}`: "The point lies in the body of the owner, or of a function that runs only as a part of the owner's activation" (`FlowOrder.h:86-99`).
- "Every run that passes the later point has passed the earlier one" (`OrderOwner.cpp:114`).
- "Seen from a caller, a store has run when the call returns, and a function that is entered during the call may be entered as soon as the call starts" (`vf/AOTFlowOrderLocations.cpp:34`).
- "What the host calls can run while any module is being evaluated" (`vf/AOTFlowOrderModules.cpp:40`).
- Which entries count as the host's: "An entry by a statement that a rule has made, any entry of unsolved code or of a function with callers that nobody knows, and the entry of a closure that a
  placeholder among the callees of a call stands for, is the host's" (`vf/AOTValueFlowResultsOrder.cpp:85-86`).

The reasoning is "a closure cannot run before the program point that first *mentions* it". With threads that stays causally true. What breaks is the silent step from "the store was executed earlier" to "the
reader's load sees it".

One more single-thread assumption: **`FlowModuleOrdering::CompletesBefore`**: "the first module has run to its end, or has thrown for good, before any code of the second one reads a variable of it"
(`FlowOrder.h:69-73`). If thread B can run a function of module M2 while thread A is still evaluating M1 that M2 imports... it cannot start M2's code before M2 was linked and evaluated, which follows M1.
Causal again, but only if "evaluated" is published with release semantics.

**Construction orders** (`vf/AOTFlowConstructionOrders.cpp:38`): "The object that a constructor makes is followed from its allocation through the constructor and through every function that takes it, up to the first
place where somebody else may get hold of it." "Until an object leaks, only the code that is followed here holds it" (`:1217`). "What leaks through another operand of a call leaks when the call is entered"
(`:195`). A callback is accepted only with `times == Once` (`:1411`). **[inference]** Handing `this` to `new Thread(...)` inside a constructor is a leak under these rules as long as the Thread entry's operand uses are not "inspected only".

### 5.2 Things that are results but have no `FlowResultKind` of their own

`ProgramInstances::AtMostOne` (`ResultsBuiltins.cpp:1184-1206`; consumers: `aot/place/AOTProgramTableEncoder.cpp:145`, `aot/stubs/AOTStubPolicy.cpp:33` `hasOneInstance`,
`aot/codegen/AOTClosureTestEmitter.cpp:38,227`, `aot/analysis/AOTPropertyAccessReceivers.cpp:93`); `functionsWithOneClosure`; `indirectCalls`; `boundFunctionUses`; `sinkOperands`; `typedResultCalls`;
`dataNodes`; `keysStoredThroughUnknownReceivers`; `keysOfSeveralSymbols`; `unseenCode`; `featureUses` / `builtinUses` (drive `--trim`); and, set later by passes, `BorrowedParameters`,
`FrameArrayParameters`, `ConstantLists`, `sitesOfWholeLiterals` (`FlowResults.h:242-254`).

### 5.3 `OperandSummaries` and the per-function escape analysis

This is a *second* whole-program system, run at the end of `applyFlowResults` (`vf/AOTFlowApply.cpp:242`):

- Nodes: "One node for each value that may hold a reference, and one for each field of an environment" (`ResultsEscape.cpp:35`).
- Lattice: `OperandUses` = flags `Inspects Retains Returns Calls CallsPerElement CallsRepeatedly ReadsContents WritesContents RetainsContents MovesElements StoresByKey DeletesByKey AssignsFrom DefinesName
  TakesDescriptor ChangesIntegrity ChangesPrototype BecomesPrototype PassesToUserCode AppliesTransitively AppliesToContents` (`aot/ir/AOTSchema.def:45-48`).
- `escapingUses = { Retains, Returns, RetainsContents, PassesToUserCode }`, `mutatingUses = {...}` (`vf/AOTFlowSinkEquations.h:17-35`).
- Equations with transforms `Same, Argument, RepeatedArgument, Leaves, IfReturned, Lifetime, Contents, Held` (`:37-49`), solved on demand in parallel (`solveSinkEquations`).
- Separate answers for frame pointers: "A pointer into a frame may be in no cell, however soon that cell dies, and native code takes cells of the heap only" (`:63`).

The per-function `EscapeAnalysis` (`aot/analysis/AOTEscapeAnalysis.h`) tracks `Allocation` and `EntryParameter` objects, `EscapeState {Unknown, Escapes, DoesNotEscape}`, `LiveInstances {Unknown, AtMostOne}`,
aliasing; it reads operand summaries at calls. `AllocateInFrames` = `aot/passes/AOTFrameAllocation*.cpp`.

Thread relevance: an object that `DoesNotEscape` its activation is thread-local for free, with one condition: **the callee to which it is lent must run on the same thread, synchronously**. A parameter summary "only inspects" is
computed from the callee body. For built-ins it comes from the table's `uses`. If a table row for `new Thread(fn, ...args)` said `passed: b.inspected`, frame allocation would hand a stack object to another thread. The row must say `Retains`.

---

## 6. The solver

### 6.1 Data structures

- **`ValueSets`** (`vf/AOTValueSets.h:25-643`): interned immutable sets; "Equal sets have equal ids" (`ValueFlow.h:19`). An id is `storedSet*2 + isComplement` (`complement(set) = id ^ 1`, `:40`), so complements are free and
  masks are usually complements. Forms: one member inline in the id (`namesOneMember`, `:426-429`); *short* sets (64 low members as a word + up to 26 high members, `:33,444`); *lists* of (place, block) entries, blocks of 4096
  members stored as present-word bitmaps, groups of 64 places. Three `InternedRuns` tables (blocks, groups, lists), 64 shards each, lock-free lookup, lock on insert (`:338-422`). Numbers have 30 bits.
  A global memo `RememberedByAll` (seqlock style, `:607-635`). `hashOfMembers` "Depends on the members alone: the same in every run and on any number of threads, which the numbers of sets are not" (`:199`).
- **`ValueSetMaker`** (`:651-792`): per-thread buffers and memo tables for unite/intersect/meet/pass. "A maker of sets takes megabytes to set up" (`ResultsInternal.h:303`).
- **`ValueFlowGraph`** (`vf/AOTValueFlowGraph.h:27`): "The nodes, the edges between them and the least solution of value(n) = what mask(n) lets through of the union of value(p) over the edges p -> n where a constant node holds its set for ever."
  Arrays by node: `Gate {mask:24, flags:8}`, `Row {low: 64 bits, set, firstArrival}`, `Links` (2 inline or a block) for successors and predecessors, `m_value`. 64 shards by `node >> 6 & 63`.
  Flags: `IsAtRest, IsRetired, IgnoresArrivals, IsCoveredNode`.
- **`ValueFlowSolver`** (`vf/AOTValueFlowSolver.h:242-1063`): made nodes (`ListThatStaysInPlace<MadeNodes>`, max 2^27), index of made nodes in 1024 lock-sharded open-addressing tables (`:763-785`), made statements, *uses* (who asks
  for which made node; unused made statements go "at rest"), readers by node (64 shards), **watches** ("The statements that wait for a node to hold more of a set. They are found by what is new in the node, member by member", `:889-932`), names and link shards (72).

### 6.2 The loop (`ValueFlowSolver::solve`, `Solver.cpp:3599-3748`)

```
addNodes; setMasks || addInitialEdges
make the non-leaving statements of every body due; reach(host body); make the Escape statement due
make the mark nodes objectsThatAreNotPlain, objectsWrittenUnderUnknownKey, objectsWithRareWrite[4]
for (;;) {                                     // a round
    runRules();                                //   turns: all due statements in parallel, each thread logs into its ValueFlowRoundLog
    merge();                                   //   number made nodes, link Read/Written nodes by name, collect edges, note readers/watchers
    m_graph.solve(moved, ...);                 //   waves of messages between 64 shards until nothing moves
    noteWhatPlaceholdersCover(); connectWhatStoppedPlaceholdersCover();
    makeReadersDue(moved) || retireNodesAmong(moved);
    if (m_saturates) saturate(moved);
    quiet? -> (Rounds::Due) one full round with every statement, then one with "general rules only", else break
}
fillNumbersThatNobodyGave(); releaseRows(); spreadMarks(); findObjectsOutsideFamilies(); findUnknownSources(report, releaseRoundState)
```

- Rules see a frozen snapshot: "What every node holds while the rules of a round run. Nothing moves in it until all of them have run" (`RuleContext.h:45`). A rule's only outputs: `connect`, `add`, `reach`, `mark`, `use`, and asking for made nodes
  (`RuleContext.h:203-213`). **Rules must be monotone and idempotent**; they are re-run whenever an input they read has moved.
- Dependency tracking is by what the rule asked: `valueOf(node)` (any move), `holdsAnything(node)` (empty -> non-empty), `valueWithin(set, node)` / `holdsAnyOf(set, node)` (only that part), `newSince(node)` (delta), `isReached(f)`, `markedAmong(objects, marks)`
  (`RuleContext.h:98-124`).
- Parallelism: `forEachIndexInParallel` over shares of <= 32 due statements, "dear" statements get a thread each (`runDueStatements`, `Solver.cpp:526-610`). Nodes are made where first asked for, under sharded locks (`makeNodesWhereAskedFor`). Graph waves: `team.run(numberOfShards...)` with per-shard
  mailboxes, falling back to one thread below 1024 messages (`runWave`, `vf/AOTValueFlowGraph.cpp:1188-1237`).
- Determinism: "but for the numbers of sets the result is the same on any number of threads" (`Bodies.h:837`); choices go by `contentOf(statement)` not by number (`Solver.h:593-594`). Tests: `TestingSwitch::DelayFlowRules` / `DelayOtherFlowRules`: "each seed is another valid
  schedule. The programs must answer the same under each" (`Solver.h:863`); `BUN/test/aot/same-program-at-any-thread-count.test.ts`.
- Fixpoint check: `Rounds::Due` adds a last round of all statements; product builds use `DueWithoutCheck`: "That round is a quarter to a third of all runs, and on no program has it added anything" (`Solver.h:287-294`). `--aot-check` word `FlowFixpoint` -> internal errors
  `FlowEndedTooEarly`, `FlowRuleWasNotDue` (`vf/AOTValueFlowResults.cpp:188-220`, `aot/ir/schema/diagnostics.def:134-135`): "the sets of that build are too small, and a set that is too small is a false proof."

### 6.3 Limits (`aot/ir/schema/limits.def:42-45`)

| Limit | Value | Effect |
| --- | --- | --- |
| `MaxValueFlowEdgesLogged` | 268,435,456 | "Once the rules have logged more edges than this, every set that still moves is given up" (`Solver.h:65-66`); `isPastEdgeBudget` |
| `MaxValueFlowSetMembers` | 16,777,216 | "A set of more layouts and callees than this is given up: its members go to `lost`, and the untracked object stands for them" (`Solver.h:61-62`). Counted in *distinct layouts/classes/callees*, not members (`hasTooManyLayoutsAndCallees`, `Solver.cpp:3240-3254`). At this value it is effectively off |
| `MaxValueFlowMembersToldApart` | 64 | "A rule that looks at the members of a value one by one takes a value with more members than this as a whole: a method call passes all its receivers to every callee" (`Solver.h:63-64`) |
| `MaxAppendedFieldsPerLayout` | 96 | |
| also | | `MaxLayoutsInType`, `MaxFunctionsInType`, `MaxLiteralsInType`, `MaxBuiltinsInType` cut what a *type* may list (`Positions.cpp:743-757`); `mostBytesOfTypesOfSets = 256 MB` (`vf/AOTValueFlowPositions.h:126`) |

Saturation (`saturate`, `Solver.cpp:3256-3287`): for a node that is given up, add edges `node -> lost` and `{untracked} -> node`. Only unmasked `Read/Written/…UnderKind/CallGroup/Lookup(Property|Elements|UnknownKey)` and body nodes may saturate (`maySaturate`, `:3219-3238`).
After the solve: "What was done through the untracked object may have been done to any object that it stands for" (`spreadMarks`, `:3301-3309`).

### 6.4 Existing propagations in the style of an escape analysis

| Mechanism | Where | Shape | Usable as a template for |
| --- | --- | --- | --- |
| **`escaped` + `runEscape`** | `vf/AOTValueFlowRules.cpp:105-148,314-348` | node holding objects; one statement; on `newSince(escaped)`: `read(arrived, EveryProperty/EveryGetter/EverySetter/CopiedFrom) -> escaped`; `PrototypeLink` (not for instances of program classes); `objectsHeldFromCreation`; `mark(arrived, ReachNativeCode)`; step iterators; **enter every closure** | the closure of "shared" over the heap |
| `lookUpThroughInstances` | `Rules.cpp:166-233` | "What lies above an instance is found through it and is not handed out with it": methods of the class of an escaped instance are *called* with the escaped instances as `this`; the prototype object itself does not escape unless unseen code runs | sharing an instance must not make its class "mutated" |
| **`HeldByNativeCode` + `followWhatNativeCodeHolds`** | `RulesHost.cpp:642-690` | the same closure into a *different* node, without entering functions: "Whoever holds an object holds what it leads to, in the same way" | a second, weaker reachability set beside `escaped`: proof that the framework supports several |
| **Marks** (`ObjectMutation`) | `RuleContext.cpp:857-875`, `Log.h:83`, `Solver.cpp:3289-3310` | `mark(objects, flags)` = edge from a constant into the node `Marked(flag)`; `markedAmong(objects, flags)` watches it | a boolean attribute per abstract object |
| Internal marks | `Log.h:84-88`: `objectsThatAreNotPlain`, `objectsWrittenUnderUnknownKey`, `objectsWithRareWrite(Accessor|Deletion|Copy|PrototypeLink)` | same, "Two marks that only rules ask after; no result reads them" | cheap pre-filters |
| Host flags | `isHostFlagRaised`/`raise`, `RulesHost.cpp:172-180` | a node that holds the unknown object "once it is so" | a global "some thread is started" flag: with it clear, *nothing* changes for programs without threads |
| `lost` / saturation | 6.3 | give-up with a placeholder | what to do with shared + untracked |
| `FunctionsOfSeenCode`, `ModulesOfSeenCode` | `Bodies.h:334-337`, `isInSeenCode` `RulesHost.cpp:759-770` | **a set of closures "that seen code calls"**, grown by `applyEventsOfCall` from each call statement whose own function is in the set, and through direct calls in `runHostRoots:697-720` | exactly the shape of "functions that may run off the main thread" computed *inside* the solve |
| Operand summaries / `EscapeAnalysis` | 5.3 | backward, per parameter | frame-local implies thread-local |
| `AllocationClass.locality: FrameLocality { Unknown FrameLocal }` | `aot/ir/schema/flow-records.def:14,64` | declared in the schema; **I found no producer and no consumer** (grep `FrameLocal` hits only the schema) | a ready-made but unused slot |

### 6.5 What adding something costs, concretely

- *A new mark*: add a flag to `flags ObjectMutation` in `flow-records.def:22-25`. It flows by itself into `marksOf`, `possibleMutationsOf`, `AllocationClass.impossibleMutations`, `LayoutFlow.impossibleMutations`, the placed layout (`aot/place/AOTLayoutTableEncoding.cpp:422`) and run-time
  `LayoutData::impossibleMutations()` (`aot/object/AOTLayoutData.h:195-235`). **`impossibleMutationBits = 10` and there are exactly 10 flags today** (`static_assert` at `AOTLayoutData.h:370`), so the bit field must grow. `m_mutationNodes` has room for 32 (`RuleContext.h:297`).
  Caveat: `letAnythingHappenTo` marks `OptionSet<ObjectMutation>::all()` (`RulesHost.cpp:220`) and `escapedObjectMutations` returns `allObjectMutations` when unseen code can run (`ResultsObjects.cpp:318-323`), so a new flag is automatically "possible" for everything that escapes to unknown JavaScript. For
  "may reach another thread" that is the right default.
- *A new global node*: add an enumerator to `ValueFlowHostNode` and bump `numberOfValueFlowHostNodes` (`Bodies.h:329-399`).
- *A new statement kind*: add to `ValueFlowStatementKind` (`ValueFlow.h:228-247`), append it in `describeHost` (`RulesHost.cpp:47-48`), dispatch it in `runRuleOnNodesNamed` (`Rules.cpp:493-552`).
- *A new call kind*: `ValueFlowCallKind` is part of the group key, so a new kind gives separate groups (separate parameter edges are still merged at the callee's entry parameters).
- *A watch-heavy node*: `holdsWhatPlaceholderCovers(node)` (`Solver.h:936-944`) special-cases `escaped`, `lost`, two marks and the rare writes because "nearly every statement that stands on an object watches them"; their watches are sharded by set. A `shared` node that many rules watch should be
  added there. If only the `Share` statement reads it, nothing is needed.
- *A new post-solve step*: add a step to the DAG in `analyzeValueFlow` (`Results.cpp:403-542`), using `ValueFlowHeapIndex` / `ValueFlowHeapView` ("The heap as the solve has left it, for questions that no rule has asked", `ResultsInternal.h:718-758`: `expand`, `reaches`, `writtenUnder`, `prototypesOf`, `chainOf`,
  `forEachNodeWrittenUnder`).

---

## 7. The table of built-ins

### 7.1 Where it is

Engine half: `aot/builtins/*.builtins.ts` (26 files, 7,967 lines), generator `aot/builtins/generator/` (19,146 lines). Host half: `BUN/src/codegen/builtins/host/*.builtins.ts` (26 files, ~9,500 lines) plus collectors that read Bun's `.classes.ts`, bindgen and static tables (`BUN/src/codegen/builtins/collect-*.ts`).
"one table of both: 6,330 entries in 271 classes" (PR-wk:125). C++ form: `aot/builtins/AOTBuiltinTable.h` (read-only arrays behind external symbols). Checked against the engine by `JSTests/stress/bun-aot-builtins-table.js`, and at run time by `--aot-validate` (section 9).

### 7.2 The TypeScript schema of an entry

An entry is a *member* with one or more *overloads* (signatures). From `generator/lib.ts:678-710` and `generator/vocabulary.ts:501-590`:

```ts
export interface SignatureOptions extends SignatureColumns {
  this?: TableType;                                   // receiver type; dispatch tests it
  args?: Readonly<Record<string, TableType>>;         // named operands, each { accepted, conversion, converted, refused, admitted?, mode, members }
  returns?: TableType | TypeByPlatform;
  resultProperties?: Readonly<Record<string, TableType>>;
  prefixProperties?: Readonly<Record<string, TableType>>;
  capability: Capability;                             // "none" | "heap" | "js"
  throws?: boolean | typeof propagated;
}
export interface SignatureColumns {
  uses?: Readonly<{ [name in OperandName]?: readonly UseSpec[] }>;   // per operand: what native code does with it
  reads?: readonly Heap[];  writes?: readonly Heap[];                // abstract heaps (AOTSchema.def:58-81)
  runsOnlyOperandCode?: boolean;  readsStateBeforeConversions?: boolean;  isSpecialCase?: boolean;  resultIsDeclaredOnly?: boolean;
  range?; substringOf?; typedResultForm?; iterates?; prototypeSource?;
  implementation?: ImplementationSpec;                // Generic | HostCall | Compiled{function} | Inline{builtin, otherwise} | CachedGetter | OwnSlot
  directEntry?: DirectEntrySpec;  model?: FunctionRef;  overloadUsesFeature?; fieldsForm?; prefixForm?; listForm?;
}
export interface EntryColumns {                       // once per entry, for all overloads
  mutability?: "ChangesOnItsOwn" | "ConstantUnlessWritten" | "AlwaysLocked";
  deterministic?; requiresHolderAsThis?; startsInstance?; privateAlias?; defaultExport?; constructable?; hasSetter?; length?;
  nativeSymbols?; customGetter?; directEntryOf{Getter,Setter,Method}?; creationSymbol?; functionObjectCode?; linkTimeConstant?;
  usesFeature?; prototypeUsesFeature?; runsUnseenCode?: boolean | ConditionSpec; hasUndeclaredMembers?; marks?: BuiltinMark[];
  loadsMachineCodeAs?; loads?: ModuleLoadSpec[]; callsFunctions?: CalledFunctionSpec[];
  appendForm?; hostAppendSymbol?; jsonTextForm?; literalPartsForm?; bodyBytesForm?; boxedCallStub?; platforms?;
}
```

The heart is `UseSpec` (`vocabulary.ts:325-358`) over `PlaceSpec` (`:281-293`) and `KeySpec` (`:298-303`):

```ts
export type PlaceSpec =
  | { place: "Operand"; operand } | { place: "Slot"; operand; slot } | { place: "IteratedValues"; operand; isSettled? } | { place: "OwnValues"; operand }
  | { place: "Result" } | { place: "ResultSlot"; slot } | { place: "ResultField"; field; holdsOnlyWhatIsStored? } | { place: "NewValue"; value: TypeSpec }
  | { place: "NativeMemory" } | { place: "Prototype"; operand } | { place: "Member"; operand; path: MemberPath } | { place: "NativeCell"; cell: string };

export type UseSpec =
  | { use: "Retains"; in: PlaceSpec } | { use: "Returns" }
  | ({ use: "Calls" } & CallbackSpec) | ({ use: "CallsMember"; path } & HandlerSpec) | ({ use: "CallsKept"; cell } & HandlerSpec)
  | { use: "RetainsMember"; path; in } | { use: "ReturnsKept"; cell } | { use: "Creates"; value: TypeSpec; in }
  | { use: "ReadsByKey"; key; to } | { use: "StoresByKey"; key; value } | { use: "DeletesByKey"; key } | { use: "TestsByKey"; key }
  | { use: "AssignsFrom"; sources } | { use: "DefinesFrom"; source } | { use: "DefinesName"; key; value }
  | { use: "DefinesGetter" | "DefinesSetter"; key; value } | { use: "ReadsGetter" | "ReadsSetter"; key; to }
  | { use: "TakesDescriptor"; key; descriptor } | { use: "TakesDescriptorMap"; descriptors } | { use: "DescribesOwnProperties" } | { use: "DescribesResult" }
  | { use: "MovesElements" } | { use: "ReadsContents"; slot? } | { use: "WritesContents"; slot? }
  | { use: "RetainsContents"; in; transitively? } | { use: "RetainsSettledValues"; in } | { use: "RetainsIteratedValues"; in }
  | { use: "ChangesIntegrity"; level } | { use: "ChangesPrototype"; to } | { use: "BecomesPrototype"; of }
  | { use: "Role"; role: OperandRole }           // EvaluatedAsText, ModuleSpecifier, ReadAsCharacters, Locale, ContentKey, ...
  | { use: "Runs"; protocols: ProtocolSpec[]; on?: "ReachableObjects" | "Contents" }   // OwnKeys OwnValues ToPrimitive Thenable Iterate Species ToJSON HasInstance PrototypeChain, named(..), calls(..)
  | { use: "Everything" };
```

Answers to "takes, returns, reads, writes, may call back, keeps":

| Question | Column |
| --- | --- |
| takes | `this`, `args` (+ conversions `.loose/.wrap`, `.optional/.nullable/.rest`, `.refusing`, `.admitting`, `.pristine`) |
| returns | `returns` with parts `Instance(.holding(slots)).fresh/.unaliased/.freshUnlessSpecies`, `Operand`, `Slot`, `IteratedValues`, `CallbackResult`, `Prototype`, `Layout`, `Builtin`, primitives |
| reads / writes | `reads` / `writes` (heaps) plus what the generator derives from `uses` (`deriveHeaps`, `build-uses.ts`) |
| may run code | `capability: "js"`; `runsOnlyOperandCode`; `Runs` protocols; `Calls*` |
| may call back | `Calls` (operand is the function), `CallsMember` (function found at a path in an options object), `CallsKept` (function an earlier call left in a native cell) |
| keeps | `Retains in <place>`: `NativeMemory` = escapes; `NativeCell(name)` = kept in a named global cell; `Slot`/`ResultSlot`/`ResultField` = kept in an object |

Lowering (`applyUse`, `generator/build-uses.ts`): `Retains*`/`Returns`/`Creates`/`ReturnsKept` become `BuiltinTransfer {from, to}`; `Calls` -> `BuiltinCallback`; `CallsMember`/`CallsKept` -> `BuiltinHandlerCall`; keyed uses -> `BuiltinPropertyAction`; each also sets bits of `OperandUses`, `Protocols`, `roles` on the
`BuiltinParameter` (`AOTBuiltinTable.h:550-633`). `struct BuiltinOverload { builtin, mode, flags, receiver, parameters, result, effects{capability, reads, writes}, flow{transfers, callbacks, propertyActions, handlerCalls}, code }` (`:672-703`).

Besides entries there are declarations for what native code does **with no call under way** (`lib.ts:1314-1487`): `hostRead`, `hostObject(cell, {prototype, fields, otherNames, internalFields})`, `hostFunction`, `hostMethodCall`, `namedMemberCall`, `nativeCalls` ("Every place where native code names one of the functions is
listed, and the generator counts them in the sources"), `startup`, `seenCodeOnly`; with `HostUseSpec = Inspected | Called{call?} | PassedTo{builtin, argument} | Kept{cell} | IsValueOf{builtin} | Member{name, uses}` (`vocabulary.ts:450-457`).

### 7.3 Three complete examples

**(1) `Array.prototype.push`** (`aot/builtins/array.builtins.ts:359-382`, with the constants from `:6-37`):

```ts
push: [
  { this: b.instance("Array").pristine("Storage"),
    args: { items: b.any.rest },
    returns: b.int(0, b.checked(2 ** 32 - 1, "UINT32_MAX")),
    capability: "heap", throws: true,
    uses: { this: [b.readsContents(), b.writesContents()], items: [b.storedIn(b.slot("this", "element"))] },
    length: 1, boxedCallStub: "ArrayPush", nativeSymbols: ["arrayProtoFuncPush"],
    implementation: b.inline("ArrayPush", { otherwise: b.hostCall }) },
  { this: b.object.loose.refusing(b.null),                                    // any array-like
    args: { items: b.any.rest },
    returns: b.length,
    capability: "js", throws: true, runsOnlyOperandCode: true, writes: ["MemberGuards"],
    uses: { this: [b.readsContents(), b.runs("OwnValues", "PrototypeChain", "ToPrimitive", b.named("length")),
                   b.storedByKey(b.indexKeys, b.operand("items")), b.storedByKey(b.keysNamed("length"), b.length)],
            items: b.inspected },
    implementation: b.hostCall },
],
```

**(2) `Promise.prototype.then`** (`aot/builtins/promise.builtins.ts:5-42,215-231`):

```ts
const pristinePromise = b.instance("Promise").pristine("Names");
const settledValue = b.slot("this", "value"), settledReason = b.slot("this", "reason");
const chainedReaction = (settled) => [ b.called({ times: "Later", args: [settled], result: "Stored", resultTo: b.resultSlot("value") }) ];
const escapingReaction = (settled) => [ b.called({ times: "Later", args: [settled], result: "Escapes" }) ];
const pristineThen = (onFulfilled, onRejected, contents) => ({
    capability: "js", throws: true, runsOnlyOperandCode: true,
    this: pristinePromise, args: { onFulfilled, onRejected },
    returns: b.instance("Promise").holding(contents).fresh,
    uses: { this: [b.readsContents(), b.writesContents()],
            onFulfilled: chainedReaction(settledValue), onRejected: chainedReaction(settledReason) },
    implementation: b.inline("PromiseThen") });
then: [
  { ...pristineThen(b.callable, b.callable, { value: b.union(b.callbackResult("onFulfilled"), b.callbackResult("onRejected")) }), privateAlias: true },
  pristineThen(b.callable, b.any.optional, { value: b.callbackResult("onFulfilled"), reason: settledReason }),
  pristineThen(b.any, b.callable, { value: b.union(settledValue, b.callbackResult("onRejected")) }),
  pristineThen(b.any.optional, b.any.optional, { value: settledValue, reason: settledReason }),
  { capability: "js", throws: true, runsOnlyOperandCode: true,
    args: { onFulfilled: b.any.optional, onRejected: b.any.optional }, returns: b.object,
    uses: { this: [b.readsContents(), b.writesContents(), b.retained, b.runs("Species")],
            onFulfilled: escapingReaction(settledValue), onRejected: escapingReaction(settledReason) },
    implementation: b.hostCall },
],
```

**(3) A Bun class method: `Blob.prototype.text` / `Response.prototype.text`** (`BUN/src/codegen/builtins/host/web-classes.builtins.ts:10-21,41-47,259-275,296,442,493-508`):

```ts
const nativeOnly = { capability: "heap", throws: true, implementation: b.hostCall } as const;
const anyCode    = { capability: "js",   throws: true, implementation: b.hostCall } as const;   // "A body may be a stream that the program made."
const settledWithAnyReason = (value) => b.instance("Promise").holding({ value, reason: b.any }).fresh;
const stateUpdate = [b.readsContents(), b.writesContents()];
const readsBlob = (settledValue) => ({ ...nativeOnly, returns: settledWithAnyReason(settledValue),
    uses: { this: stateUpdate }, reads: ["ExternalState"], writes: ["ExternalState"] });
const readsBody = (settledValue) => ({ ...anyCode,    returns: settledWithAnyReason(settledValue),
    uses: { this: stateUpdate }, reads: ["ExternalState"], writes: ["ExternalState"] });
b.method("%Blob%.prototype.text", readsBlob(b.string));        // via declareMethods("Blob", membersOfBlob)
b.method("%Response%.prototype.text", readsBody(b.string));    // via declareMethods("Response", membersOfMessage("Response"))
```

A handler found in an options object (`Bun.serve({ fetch })`): `b.memberCalled(["fetch"], calledWith(b.instance("Request").fresh))` with `times: "PerEvent"` (`BUN/.../globals.builtins.ts:627-654`); a handler kept earlier: `b.keptCalled(requestHandlerCell, {...})` (`:899`).

### 7.4 How natively implemented classes are described

`b.class(path, ClassOptions)` (`lib.ts:1040-1131`) with `ClassColumns` (`vocabulary.ts:612-652`): `extends`, `jsType` / `instanceCppClass` (how compiled code tests an instance), `slots` (named abstract contents, each with a heap and an optional `ContentsSlot` `Element|Key|Value` that maps to a TypeScript type
argument through `types: [exactType("Map", "key", "value")]`), `stateHeap`, `pristineTest`, `lockedObjects` / `lockedMembers` / `closedToOtherNames` / `openNamesOf…`, `namesDefinedOnNewInstances`, `constructorOnlyReadsPrototypeOfNewTarget`, `sharingMode: "Default" | "Shared"` (for `SharedArrayBuffer`), `hasConstructor`,
`hasPrototype`; plus `call`, `construct`, `static`, `proto`, `instance` (own data properties at a known offset: `ownSlot`), and `constructorTable` / `prototypeTable` (members are collected from JSC's static hash tables, so a member without a hand-written row still exists as an *undeclared* entry).

`Map` (`aot/builtins/map-set.builtins.ts:104-199`): slots `key`, `value` in heap `CollectionEntries`, `stateHeap: "CollectionStorage"`, `mutability: "AlwaysLocked"`, `construct` with a `model` (a TypeScript function in `aot/builtins/models/Collections.ts` that flow *enters* in place of records), methods mostly `b.inline(...)`.
Note `get` has `writes: [storage]` (`:191-198`).

`Worker`: `constructorOnly`, no prototype in the table (see 3.4). Most Bun classes come from collectors with `methodReturning(declaredText)` (`BUN/src/codegen/builtins/collect-bindgen.ts:136`) or stay `b.otherwise`.

### 7.5 An entry for `new Thread(fn, ...args)` and `thread.join()`

**[inference]** Written with today's vocabulary:

```ts
b.class("Thread", {
  instanceCppClass: b.cppClass("JSThread"),
  slots: { result: b.contents("Value", "ThreadState"), failure: b.state("ThreadState") },   // needs a new heap "ThreadState" in AOTSchema.def
  stateHeap: "ThreadState",
  lockedObjects: ["prototype"], lockedMembers: ["constructor"],
  construct: {
    args: { fn: b.callable, passed: b.any.rest },
    returns: b.instance("Thread").fresh,
    capability: "heap", throws: true,                       // "heap" is allowed: check.ts:559 exempts callbacks that are called after return
    uses: {
      fn:     [b.called({ times: "Later", spreadArgs: b.operand("passed"), result: "Stored", resultTo: b.resultSlot("result") })],
      passed: [b.storedIn(b.nativeCell("Thread.arguments"))],   // must be a Retains: see 5.3
    },
    reads: ["ExternalState"], writes: ["ExternalState"],
    implementation: b.hostCall,
  },
  proto: {
    join: { returns: b.slot("this", "result"), capability: "heap", throws: true,
            uses: { this: [b.readsContents()] }, reads: ["ThreadState"], implementation: b.hostCall },
  },
});
```

What works today with this text (checked against the rules):

- `fn` is entered through `group(callees, CallByNativeCode)`, `this = undefined`, `passed -> group.otherArguments` (`applyCallback`, `Builtins.cpp:2160-2178`). With `args: [b.operand("passed")]` instead of `spreadArgs`, positions are kept ("A rest operand among the arguments is handed on in order", `:2093`).
- The result: `useResult(Stored)` connects `group.result -> Written({site object of this `new`}, Slot result)` (`:2134-2140`, `forEachTargetAt(ResultSlot)` `:1637-1639`). `join()` reads `Slot(this, result)` (`forEachNodeAt(Slot)` `:1377-1379`). **Per `new Thread` site**, because the construction is a call operation with its own `BuiltinResult` site.
- `fn` does not escape; its parameters keep precise sets; it keeps `CallerState::AllKnown`, because the entry is recorded as a `FlowIndirectCall{caller, site, kind: BuiltinCallback, times: Later}` ("Every entry into a function whose callers are all known is a call that names the function, a call whose stamp lists it, or an indirect call out of an operation", `ResultsFunctions.cpp:884`).

What is **not expressible today**:

1. **"on another thread"**. No word in `CallbackSpec`/`HandlerSpec`/`HostCallSpec`. Needed: e.g. `on?: "CallerThread" | "NewThread"` -> a bit in `BuiltinCallback` / `BuiltinHandlerCall` / `BuiltinHostCall`.
2. **Where what `fn` throws goes.** `connectThrown` sends the thrown value of a `Later` callback to `escaped` (`:2145-2152`). Only a promise's value slot gets "rejected with what the function throws" (`:2179-2186`, hard-wired to `BuiltinSlots::PromiseValue/PromiseReason`). If `join()` rethrows, either model the
   result as a Promise slot pair, or add `thrownTo?: PlaceSpec`. As written the error object escapes (sound, imprecise) and `join`'s `throws: true` yields the unknown object.
3. **"this entry makes the program multi-threaded"**: an entry flag like `StartsInstance` (e.g. `StartsThread`) so that a global host flag can be raised only when a *call* of it is reached (as for evaluators), and so that an escaped/lost `Thread` constructor counts (`countProgramInstances` shows the pattern: uses, `hasEscaped`, `isLost`, `unseenCodeHasLoader`).
4. **Effects**: `reads`/`writes`/`capability` describe what happens *during the call*. Nothing can say "from now on, heaps X may change at any time".
5. Blocking (`join`, locks, `Atomics.wait`) has no word; `capability: "heap"` + `ExternalState` reads/writes is the closest ("The clock moves between two readings with no store in between. The write keeps each reading where it is written", `globals.builtins.ts:84`). **[inference]** `join()`/lock acquire must be declared to *write* every heap that shared
   objects live in, or redundant-read elimination will carry values of shared locations across it.

---

## 8. Precision on real programs, and cost

### 8.1 Numbers found

- TypeScript 6 compiler checking `effect`: "Of the 52,790 property accesses left in that program after optimization, 13,077 are proven, 34,946 have a test of their own and 4,767 go by name" (PR-wk:168). That is 24.8% / 66.2% / 9.0%.
- The table: 6,330 entries, 271 classes (PR-wk:125).
- Compile time of the large CLI (Claude Code's CLI with dependencies, PR-bun:407): 36.0-37.1 s wall, 426-448 CPU s, 5.4 GB peak, 16 threads. Flow 9.1 s of a 38.3 s run with phase log; "For about 10 of the 36 s most of the 16 threads have nothing to do: 3.6 s of that is in flow" (PR-wk:180-194). Goal: 10 s total on a 16-core laptop; "We're 2.6 times over it".
- Solver sizing comment: "A solve makes about two keys and two and a half nodes for each node of its input" (`Solver.cpp:151`).
- Witnesses: "It costs 20 bytes an edge: for tests and reports" (`Solver.h:67`); `isAskedHowValuesCame` "costs a walk over all that holds the unknown object and a copy of all edges" (`:69`).
- Construction orders: "The effects, the escape analysis and the use lists of every function that takes an object under construction are gigabytes in a large program" (`vf/AOTFlowConstructionOrders.cpp:814`).
- The post-solve part: "Much of a step runs on one thread" (`Results.cpp:357`); it is a DAG of ~30 named steps run on the pool (`:403-542`).

I found **no checked-in statistics** of proven/unproven positions, sizes of `escaped`/`lost`, number of nodes/edges/rounds for any real program. The counters exist: `StatisticID::FlowAllocationSites, FlowAllocationClasses, FlowProvenPositions, FlowUnprovenPositions, FlowNarrowedPositions, FlowProvenContentsSites, FlowRemovedChecks,
FlowRemainingChecks, FlowUnreachedFunctions, FlowAppendedFields, FlowSetsOutsideTypes, LayoutsOutsideFamily, FlowCallsOfUnknownCallees, FlowAccessesOfUnknownReceivers` (`aot/ir/schema/statistics.def:14-18`), `ValueFlowSolver::Counters` / `Round` (`Solver.h:247-274`), and the per-round log line under `logsPhaseTimes` (`Solver.cpp:3690-3691`).
They can be printed from a build; nobody has to guess.

### 8.2 What bounds precision (relevant to thread escape)

1. One abstract object per class / per function's closures / per async function's promises (1.1).
2. One allocation class per layout in the results (`ResultsObjects.cpp:581-613`). Literal sites with the same layout are merged at this point even though the solve kept them apart.
3. No context sensitivity: a helper like `function box(x) { return { x } }` has one site; a generic container class has one `instanceOf`.
4. Native cells are global: all timers share `Timeout.callback`/`Timeout.arguments`.
5. Anything that meets an undeclared native entry escapes (`handToUndescribedNativeCode`), and `escaped` is one set: what comes back from any undescribed native result is "any escaped object".
6. Above 64 receivers/keys a rule stops telling members apart.
7. `catch (e)`: every call adds the unknown value to `thrown` (`RulesCalls.cpp:788-790`), and thrown values of `Later` callbacks escape.

### 8.3 Cost expectations for the new results **[inference]**

A `shared` node with a `Share` statement adds: one node, one statement that re-runs when `shared` grows (delta via `newSince`), and per delta about five `Read(arrived, Every*)` made nodes, each linked against all `Written(*, name)` nodes of all names of that kind (`ReadUnderKind` machinery). That is the same work as `runEscape` does. In a program where most of
the heap is reachable from module variables that thread code touches, `shared` becomes a very large set; sets are interned bitmaps so the size itself is cheap, but each `Read(shared-delta, EveryProperty)` is a "very many objects" node, "Found only by a look at each" (`Solver.h:509-510`).
The alternative, a post-solve closure over `ValueFlowHeapIndex` (all `WrittenNode {objects, value}` by name), is a plain graph reachability over data that already exists, adds nothing to the rounds, and parallelises by stretches (`stretchesOfNodesWrittenUnder`, `ResultsInternal.h:668-685`). It is possible **only if no rule needs the answer**, i.e. if "shared" does not feed
back into points-to sets or reachability. It does not: sharing changes *how* code must be compiled, not *what* flows where.

---

## 9. Validation at run time (`--aot-validate`)

### 9.1 The machinery

- Option: `--aot-validate` (`BUN/src/runtime/cli/Arguments.rs:697`), wire flag `CheckSet::Validate` (`aot/ir/schema/compile-call.def:117`), run-time switch `vm.aotState().validatesRuntime` (`aot/runtime/AOTRuntimeValidation.h:124`).
- "With `--aot-validate`, compiled code checks every fact that it relies on at run time and aborts on a miss: proven types, flow's results, declared effects of native calls, 32-bit references, registers that a stub must preserve. `aot/tests/validation/` has a lie per producer, each of which has to be caught" (PR-wk:271).
- Failure: `crashOnValidationFailure(vm, {kind, function, value, found})`, prints `AOT validation: <Kind> f<n> v<n> ..` (`aot/tests/README.md:333`). 48 kinds, append-only (`AOTRuntimeValidation.h:25-77`).

### 9.2 How each kind of flow result is validated

| Flow result | Mechanism | Where |
| --- | --- | --- |
| proven/narrowed types, removed checks | `insertRuntimeValidation`: before every `AssumeType` a `ValidateType`; "A pass may use a proof up: it folds a test, resolves a callee or removes a read, and the cast or the stamp that stated the proof goes" so the validation op is a separate operation that survives | `vf/AOTFlowApplyCasts.cpp:532-564`; ops `aot/ir/schema/prepared-operations.def:150-154` |
| receiver sets | `ValidateType(receiver, type of listed layouts)` | `FlowApplyCasts.cpp:505-513,555-558` |
| callee sets | `ValidateCallee` -> `UnlistedCallee` | `:559-562`; `aot/codegen/AOTRuntimeValidationChecks.cpp:401-428` |
| method resolved to a direct call | `ValidateLookup` -> `LookupDiffers` | `RuntimeValidationChecks.cpp:432-460` |
| **runs at most once / stored at most once / class evaluated once** | `ValidateFirstRun(variable)`: a hidden module variable per function/variable/class; "fail unless absent, then store true" -> `SecondRun` | `FlowApply.h:59-63` (`FlowFirstTimeMarks`), `FlowApplyCasts.cpp:516-554`, `RuntimeValidationChecks.cpp:312-317` |
| initialization / TDZ removal | `checkBindingIsStored` -> `BindingReadBeforeStore`; `SlotEmpty` | `aot/codegen/AOTRuntimeValidationEmitter.cpp:379-382`; `RuntimeValidationChecks.cpp:237` |
| every value at every use | `validateOperandsOf`: `checkValue` for each operand with layouts/classes; `checkClassProofs` (`ArrayNotPristine`, `ArrayStorageKind`, `ContainerContents`) | `RuntimeValidationEmitter.cpp:39-62`; `RuntimeValidationChecks.cpp:285-310` |
| **impossible mutations** | run time: `validateMutation(vm, object, mutation)`: `if (validatesRuntime && object.layout().impossibleMutations().contains(mutation)) crash` at every slow path that deletes/redefines/adds/freezes/reparents | `aot/object/AOTObjectInternal.h:44-66`, `AOTObjectInternal.cpp:21-40` |
| **`ReachNativeCode`** | `NativeCallValidationScope` around every native call: for a function that is not in the table, every operand that is a compiled object whose layout "never reaches native code" fails | `aot/runtime/AOTNativeCallValidation.cpp:564-578` |
| table rows (`uses`, `reads`, `writes`, `throws`, `capability`, `Retains`) | the same scope photographs operands before and compares after: `NativeCallChangedOperand, NativeCallWroteHeap, NativeCallRanCode, NativeCallThrew, NativeCallCollected, NativeCallKeptOperand` | `AOTNativeCallValidation.cpp:540-760` |
| heap slots | `validateSlotTypesOfLiveObjects` after a collection -> `HeapSlotType` | `AOTRuntimeValidation.h:119-122,135-137` |
| frame allocation | `checkIsNotFrameCell` at every retaining use -> `FrameCellEscaped`; frame cells are poisoned at exits/suspensions | `RuntimeValidationEmitter.cpp:130-181,320-349`; `RuntimeValidationChecks.cpp:512-520` |
| eliminated reads | `ValidateSameValue(kept, replacement)` -> `ReadsDiffer` | `aot/analysis/AOTValueSubstitution.cpp:207-229` |
| keys never enumerated, removed fields | `failIfKeysMustNotBeListed` -> `KeysListed`, `RemovedFieldNamed` | `AOTObjectInternal.cpp:53-61` |
| unreached code | `reportUnreachable` -> `UnreachableValue` | `RuntimeValidationEmitter.cpp:397-400` |
| closed program | not validation: the run time refuses (`EvalError`) | `aot/runtime/AOTRealmStartup.cpp:130` |
| solver fixpoint | build time, `FlowFixpoint` | 6.2 |

Tests: `aot/tests/validation/*.ir` (25 files), each a program with a deliberately false fact; also `provesEverything(build)`: "A test of the validating build: whatever the types of the program say counts as proven, so the program has to end wherever a value of another type arrives" (`FlowApply.h:30-33`).

### 9.3 What a thread-escape result would need **[inference]**

Two facts have to be checkable:

- *"function f never runs off the main thread"*: at the entry of f (same place as `insertFirstRunTest`, `FlowApplyCasts.cpp:525-531`) compare the current thread's identity with the main thread's. One new IR op `ValidateThread` and one failure kind (append at the end of `ValidationFailureKind`).
- *"no object of layout L / no closure of f / variable g / environment field (L, i) is touched by two threads"*: needs an **owner word**. Options in the style of the existing checks:
  1. At *publication points* only, like `ReachNativeCode`: in the native `Thread` constructor, `join`, and any other hand-over primitive, walk the operands transitively (the validator already walks operands) and fail if a compiled object's `layout().impossibleMutations()` contains the new flag. Cheap, but it checks only direct hand-over, not "stored into an
     already shared object". For that, hook the *store*: in a validating build every store of a reference into an object whose layout *may* be shared checks that the stored object's layout may be shared too (that is the closure property itself, checked edge by edge: the same idea as `checkIsNotFrameCell(operand, retainer)`).
  2. At *every access*: a validating build adds a hidden field "allocating thread" to layouts claimed local and checks it in `validateOperandsOf`. `ValidateFirstRun` shows that hidden state per fact is acceptable in validating builds. Compiled objects have no spare header word (order word + shape word fill offsets 8..15, PR-wk:81-86), so it must be a real field or a side table keyed by cell.
  Option 1's edge check plus the entry check gives an inductive proof at run time; option 2 is the direct oracle. The "lie per producer" test style needs one `.ir` per new fact.

---

## 10. How the two new results could be added **[inference throughout]**

### 10.1 (a) "Which functions can run on a thread other than the main one"

Two designs.

**A. Inside the solve, copying `FunctionsOfSeenCode`.** New host node `FunctionsOffMainThread` (closures).
- Seed: in `applyCallback` / `applyHandlerCall` / `callByHost`, when the record says `on: NewThread`, `add(FunctionsOffMainThread, callees ∩ closures)`. If `unknown` (resp. `untracked`) is among the callees, add `valueWithin(closures, escaped)` (resp. `lost`).
- Step: like `applyEventsOfCall` (`RulesHost.cpp:881-885`): in `runCall` and in every rule that asks for a group on behalf of a statement with a function, `if (holdsAnyOf({closureOf(statement.function)}, FunctionsOffMainThread)) add(FunctionsOffMainThread, callees ∩ closures)`. Direct calls: as `runHostRoots:697-720` does with `rememberedClosuresCalledDirectlyBy`.
- Gap: entries made by *made statements* (lookups that enter getters, hubs that run protocols, groups of groups) have no `statement.function`. `isInSeenCode` has the same gap and treats it as "not seen code"; for threads the safe answer is the opposite.

**B. After the solve, on the call lists that already exist.** `computeOrderResults` already builds the full list `FlowCall {caller, site, callee}` with `host()` as the caller of everything unattributed (`vf/AOTValueFlowResultsOrder.cpp:24-140`, `FlowOrder.h:28-35`); `computeFunctionResults` builds `indirectCalls` (`Accessor, BuiltinCallback, Protocol, Host`) and `functionsEnteredByHost`; stamps give callee sets; `CallDirect` is in the bodies.
  Reachability from the thread entry functions over this graph is a BitVector worklist. What is needed from the solve is only the *seed set*, which cannot be recovered today ("The solve does not keep which row of the table has entered which closure"): so a host node (as in A) or a distinct `ValueFlowCallKind::CallOnNewThread` (separate groups, found through `firstNodesOfCallGroups()`).

Soundness conditions for either:

1. **Incomplete callee sets.** "A set that is not complete still lists what the solve has followed to the call and no placeholder covers. Whatever else the call enters has escaped or is lost" (`vf/AOTValueFlowResultsCalls.cpp:75`). So: an off-main function with an incomplete stamp makes every closure in `escaped` (and/or `lost`) off-main.
2. **Host entries.** A function in `functionsEnteredByHost` / `isCalledByHost` / `hasEscaped(closure)` is called by native code "at a time nobody knows"; on *which thread* depends on the run-time design. If native callbacks always fire on the thread that registered them, then: escaped closure is off-main iff it can be *registered* from off-main code or is reachable by off-main code. Conservative: once any thread is started, all
   escaped closures are off-main. This is likely too coarse for real programs (event handlers, `Bun.serve` handlers are `memberCalled`, not escaped, so they stay precise).
3. **`Later` / `PerEvent` callbacks and coroutine resumption.** The analysis has no notion of *which thread's* job queue runs a reaction. Example: main awaits a promise that a thread resolves. `runAwaitedValue` just connects the value slot. If the run time runs reaction jobs on the resolving thread, the rest of a *main-only* async function runs off-main, and result (a) is unsound unless every coroutine that can await a *shared* promise is
   off-main. **Design constraint:** jobs and timers must be queued to the thread that registered the reaction (or that owns the coroutine). Then rule: callee of a `Later`/`PerEvent` record runs on the thread(s) of the function that made the call.
4. **Module bodies**: a dynamic `import()`/`require()` reached from off-main code evaluates module bodies off-main (`FlowModuleLoad {function, target, isSynchronous}` lists them, `FlowOrder.h:20-26`).
5. **Generic versions, entry checks, typed bodies**: three bodies per source function at the time of the solve (`genericVersionOf`, `runEntryChecks`, `Rules.cpp:429-474`): propagate along those edges (they are `CallDirect` or `reach(genericVersion)`).
6. **Derived functions**: passes after flow create functions (`SplitCoroutines`, outlining, entrypoints, bound functions). `FlowResults::function(id)` maps them to the analyzed one; a new per-function bit in `FunctionFlow` inherits that for free.
7. **Library functions "that records describe"**: a built-in implemented by a compiled library function is *entered once for all callers* ("One function of the library runs for all calls of a built-in", `RulesCalls.cpp:636`). `Array.prototype.map`'s compiled body will be off-main as soon as any thread code calls `map`. So (a) will say "both" for most of the library. That is fine if "both" only matters for accesses to *shared* data.

Landing place: a new enum in `flow-records.def`, e.g. `enum ThreadSet { | engine Unknown MainOnly }`, a field in `record FunctionFlow`, a 24th `FlowResultKind` for bisection. `numberOfEnumerators<FlowResultKind> <= bitsPerWordOfSwitches` is asserted (`aot/support/AOTCompileOptions.cpp:42`).

### 10.2 (b) "Which objects / variables can be reached by more than one thread"

**Definition that fits the domain.** `Shared` = least set of abstract objects and locations such that:

- R1 (hand-over): everything at a place that a `NewThread` record passes (`this`, `arguments`, `spreadArguments`), the callee closures themselves, and everything the callee's `result`/`thrown` can hold, is in `Shared`.
- R2 (closures): if `closureOf(f)` is in `Shared` then every object in `capture(f, i)` is in `Shared`, and every environment layout that f (or a function nested in it) reads through a capture is a *shared location set*: all its `field(L, k)` nodes are shared locations and what they hold is in `Shared`.
- R3 (heap): if `o` in `Shared` then everything in `Written(S, n)` with `o` in S, for every name kind that a holder can read, is in `Shared`: `EveryProperty` (covers `Property, Elements, UnknownKey, Slot, InternalFields, SteppedValues, ReturnedValue`), `EveryGetter`, `EverySetter`, `CopiedFrom`, `PrototypeLink`, **plus what `runEscape` can skip but a thread cannot**: `HiddenSlot` (bound target, bound this, bound arguments), private/brand/hidden fields (per-layout nodes `field(L, i)` for
  every layout of an object in `Shared`), `SentValues`, `ResolvedPromises`/`RejectedPromises`, `objectsHeldFromCreation` (prototype object <-> constructor).
- R4 (globals): every location that is not owned by an activation is shared **as soon as off-main code can access it**: module variables, static fields, namespace objects, `globalThis` and all built-in objects of the realm, native cells, class objects and prototype objects. Precisely: variable `g` is a shared location iff some function in (a) reads or writes it (`noteOperationsOnLocations` already walks every reached function and records reads/stores per location,
  `ResultsLocations.cpp:487-501`) **or** two different threads can (several instances of the same thread function): so "accessed by any off-main function" is the criterion, not "accessed by both".
- R5 (placeholders): `unknown` in `Shared` implies `escaped ⊆ Shared`; `untracked` in `Shared` implies `lost ⊆ Shared`. And the reverse direction: if native code can carry an escaped object to another thread (any native cell or `NativeMemory` that off-main code can read back as "undescribed value"), then `escaped ⊆ Shared` once any off-main function receives the unknown object. In practice: **as soon as off-main code touches an unknown value, everything escaped is shared.**
- R6 (thread-local allocation): an object allocated by an off-main function that never enters `Shared` by R1-R5 is local to its thread, although the *function* runs on many threads.

Because R2 and R4 depend on (a) and (a) depends only on the call graph, the order is: solve -> stamps/indirect calls -> (a) -> (b). All inputs exist after the step `locations: operations` in `analyzeValueFlow`.

**What the result looks like.**

- per layout: flag in `ObjectMutation` (say `ReachOtherThread`) -> `LayoutFlow.impossibleMutations`, `AllocationClass.impossibleMutations`. For built-in class instances (arrays, Maps): per allocation *site* is possible (`sitesOfObjectsThatNeverChange` is a per-site BitVector, `FlowResults.h:157-158`; `ValueFlowContainerFacts` are per object), and types can carry it in `ClassPart` like `provenPristine`.
- per function's closures: `AllocationClass.closureFunction` records exist (`flow-records.def:62-63`).
- per variable / environment field: a field in `LocationFlow` (it already has `writers`, `stores`, `reads`, `isUnchangedDuringActivation`).

**Why per layout is the natural granularity for the consumer.** An access site knows a *layout set*, not a site. Compiled code for `o.x` with `o: {L1, L2}` can use today's sequence iff both layouts are never shared. To have "local Foo" and "shared Foo" one needs two layouts (two shape numbers). The engine already derives layouts (`LayoutFlags::IsDerived`, `AllocateTaggedLayouts`: "puts one allocation for each tag where the one allocation of a constructor stood",
`vf/AOTFlowConstructionOrders.cpp:302`) and already compiles several versions of one function (typed body / generic version), so cloning is conceivable but is new work.

**Immutable data is the big escape hatch.** For a shared object, today's code is still right for every field that is never written after publication: `StoreCount::AtMostOnce` with `SetAtCreation`, `FieldStability` all set, `impossibleMutations ⊇ {AddProperty, DeleteField, RedefineProperty, ChangePrototype, ChangeIntegrity}`; `neverChangesOnceMade(object)` (`ResultsObjects.cpp:331-338`) and `sitesOfObjectsThatNeverChange` name exactly such objects. Class prototype
objects and methods with `MemberMutability::Fixed`, locked built-ins (`lockedObjects`, `AlwaysLocked`), constant module variables (`NarrowConstantVariables`) are all in this class. So the interesting set is **Shared ∧ mutated-after-publication**, and both halves are (or would be) marks on the same abstract objects. What remains needed for them is *safe publication* (section 11, H1).

### 10.3 Expected precision **[inference]**

- Programs that never construct a `Thread`: the host flag is never raised; all results as today. (Same pattern as `UnseenCodeRuns`.)
- A worker-pool style program (`new Thread(work, sharedQueue)`): `work`, everything it calls, and every module variable those functions touch become shared locations. Since library code (Bun's JS modules, helpers) keeps state in module variables, R4 will mark a large part of the library's module state shared. Classes used on both sides (`instanceOf(C)` is one object) become shared as soon as one instance is handed over or stored in a shared variable.
- The main loss is context insensitivity: `instanceOf(C)`. A per-thread-context clone of the universe ("main" and "off-main" copies of each site, chosen by which version of the allocating function runs) would double sites only for functions in both sets; it needs function versioning by thread, which also would let main-only versions keep today's code. This is the same trick as `genericVersionOf`.

---

## 11. Hazards with several mutators

The analysis itself runs at build time, so "state written at run time" here means: **facts that flow proves and that compiled code then relies on without a test**, and run-time state that exists only because of a flow result. For each: what goes wrong, and whether it is a wrong value (V) or memory safety (M).

| # | Assumption | Where it comes from | What a second thread does | V / M |
| --- | --- | --- | --- | --- |
| H1 | **A reader that holds a reference sees all stores that preceded its publication.** `SetAtCreation`, `FilledAtBirth`, `AlwaysFilled`, `StoredBeforeEveryRead`, `readsAfterInitialization`, `undefinedAtStartIsNeverRead`, `seesOnlyFinishedObjects`, `isThereFromTheStart`, proven field types with raw representations | `Initialization`, `StoreCounts`, `ConstructorFieldOrders`, `ProvenLayoutSets` (5, 5.1) | Thread A: allocate, fill fields, store the reference into a shared slot with a plain store. Thread B on ARM64 loads the reference and then a field and can see the pre-initialization bytes (the allocator's leftovers or zero). For module variables there is not even an address dependency | **M**: a `Cell32`/raw `Double`/`Int32` slot read without test; shape word or order word not yet visible gives a failed shape test at best, type confusion at worst. On x86-64 (TSO) only compiler reordering matters |
| H2 | **"Stored at most once" = constant after the store**; `isUnchangedDuringActivation`; `NarrowConstantVariables`, `DeferInitializers`, `constantListIn` | `StoreCounts` + `ExecutionCounts` | The single store is by thread A; thread B's read is "after" only causally. As H1 | V or **M** (if the variable's representation is raw/narrow) |
| H3 | **At most once** for functions, classes, symbols, closures (`hasOneClosure`, `symbolsCreatedOnce`) rests on "A module of the image is evaluated at most once" | `ResultsExecutions.cpp:81` | Two threads `import()`/`require()` the same module concurrently; check-then-act in the loader runs the body twice | **M** possible: `PromoteEnvironmentsToVariables` turns the one activation's environment into statics; `EmbedConstantCollections`, class objects as constants, `FoldConstants` on `hasOneClosure` (identity comparisons folded): V |
| H4 | **Impossible mutations hold "for ever", possible ones hold "for a stretch of code in which nothing can undo it"** (`Proof` in `AOTSchema.def`, PR-wk:91). Flow only supplies the "for ever" half. For layouts where a mutation *is* possible, passes keep guards valid until the next operation whose *effects* may undo them | effects model + `HolderMutations` | Another thread deletes/redefines/freezes/adds between guard and access. Every check-then-act on shape, "slot filled", "no extra properties", "not frozen", key order, prototype unchanged | **M** for slot reads after a shape/filled guard (slot may have been emptied and moved to the side object); belongs to R1/R2 but **the thread-escape result is exactly what decides for which layouts the old reasoning stays valid** |
| H5 | **Effects of an operation are all that happens between two operations.** `reads`/`writes`/`capability` of table rows, `EffectSummaries`, `EliminateRedundantReads`, `HoistLoopInvariantReads`, B3 heap ranges | table + `OperandSummaries` | Any shared location changes at any time. Redundant-read elimination on shared locations is allowed by most memory models for non-atomic data (V only) **unless** the two reads are a test and a use (then it is *safer* to reuse the value). The dangerous direction is *re-loading* a value that was tested (rematerialisation) | V, **M** if a tested value is re-read |
| H6 | **`DoesNotEscape` / borrowed parameters / frame arrays**: the callee runs synchronously on this thread | `OperandSummaries`, table `uses` | A wrong or lazy row for a thread primitive (`inspected` instead of `retained`) puts a frame cell into another thread's hands | **M** (dangling stack pointer). `FrameCellEscaped` validation catches it only on the retaining use that the IR sees |
| H7 | **`ReuseEnvironments`**: "The next turn of a loop can take the cell that this turn has done with, and so can the next call of the function" (`aot/passes/AOTReuseEnvironments.cpp:214`); the free cell lies in the Instance's statics (`freeCellOffset`, `RuntimeValidationChecks.cpp:505-509`) | escape analysis | Two threads run the same function and both take the one free cell | **M**. Any per-function or per-site state in `Instance` statics has this problem: site caches, deferred closures/initializers in variables, `ValidateFirstRun` marks |
| H8 | **`ProgramInstances::AtMostOne`** lets code bake "the" instance (`hasOneInstance` stub policy, slim closure tests) | `ResultsBuiltins.cpp:1184-1206` | Threads share one realm, hence one Instance: stays true. But pinned "instance register" state that is per *thread* (stack limits, exception state, scratch) must not live in the Instance | design note |
| H9 | **Native containers** (`Map.get` declares `writes: ["CollectionStorage"]`, `map-set.builtins.ts:191-198`; arrays grow by reallocating the butterfly; ropes resolve in place: `ResolvesRopes = reads(StringStorage) writes(StringStorage, RuntimeState)`, `AOTSchema.def:87`) | table rows are honest about single-thread effects | Even *reads* of shared native objects write | **M**. Flow can tell which container *sites* are never shared (per site!), which is the precise case |
| H10 | **Native cells are one per program** | `lib.ts:378-379` | If the run time makes them per thread (timers, tick queue, async context), flow's model over-approximates (sound). If they stay per realm, they are shared roots: whatever is stored there is `Shared` (R4) | analysis soundness |
| H11 | **`escaped` objects are "read-only until unknown code writes"**; `HeldByNativeCode` is "kept and not called" | host flags | unaffected by threads as such, but native code running on *another* thread (N-API thread-safe functions, event loops per thread) now reads/writes escaped objects concurrently with compiled code | M, outside flow |
| H12 | **Coroutine resumption thread** (10.1 point 3) | no model | A main-only async function continues on the thread that resolved the promise | breaks result (a): then V/M as for any unsound "local" claim |
| H13 | **Order word / key order** proven by `ConstructorFieldOrders` up to the leak; afterwards the order word is run-time state updated on add/delete | place | two threads add properties to one object: multi-word update (slot + order word + maybe side object) | **M**, R1's area; flow says for which layouts `AddProperty` is impossible |
| H14 | **Validation state**: `ValidateFirstRun` is load-test-store on a module variable; photographs of operands (`PhotographOperand`) live per VM; `static thread_local objectListedWithoutOrder` (`AOTObjectInternal.cpp:42`) is already thread-local | `--aot-validate` | false alarms or missed alarms under races | tooling only |
| H15 | **The unknown/untracked collapse**: one `escaped` set, one `lost` set | domain | R5: a single unknown value in thread code makes all escaped objects shared; past the edge budget "every set that still moves is given up" and `lost` may hold most of the program | precision cliff, not unsoundness |

Build-time note: the solver is itself heavily multi-threaded (sharded tables, seqlocks, relaxed atomics). That has nothing to do with JavaScript threads; `BUN/test/aot/thread-sanitizer.test.ts` covers it.

---

## 12. What helps

1. **Closed world.** In a closed image (`AdmitsUnknownJavaScript` clear) the compiler has seen every function that can ever run on any thread, and the run time *refuses* the rest. A JIT engine can never have a whole-program thread-escape result.
2. **No code is patched, no profile, no deoptimisation, image read-only** (PR-wk:17-23,97,119). Flow's results are constants of the executable; there is no "invalidate when the assumption breaks" machinery to make thread-safe (no watchpoints). A fact is either proven for all interleavings or not used.
3. **Union facts are interleaving independent.** 18 of 23 kinds (types, callee/receiver sets, impossible mutations, own names, uses, reachability) quantify over *all reached code*, not over executions. Adding threads = adding roots.
4. **The escape closure, marks, host flags, per-layout result records, run-time layout flags and validation hooks already exist end to end** for "reaches native code". A thread-escape mark can ride the same rails: `flow-records.def` -> `mark()` -> `spreadMarks` -> `possibleMutationsOf` -> `AllocationClass`/`LayoutFlow` -> `LayoutData::impossibleMutations()` -> `validateMutation`.
5. **"Only a call counts" events**: programs that never reach a call of `Thread` pay nothing and lose nothing; the pattern (`eventOfCalling`, always-followed objects, `EvalError` for what was not tracked) can be reused: a `Thread` constructor reached in a way the build could not follow can *throw*.
6. **The table is the single description of native code**, with conservative defaults (`everything`, undeclared) and a run-time validator for each row. Thread primitives are a handful of rows.
7. **Built-ins are locked**: "Existing members of built-in objects can't be replaced" (PR-wk:21,145). `lockedObjects`, `AlwaysLocked`, `Object.prototype`/`Promise.prototype` closed to new names. The most shared objects of all are immutable by construction.
8. **Immutability results exist**: `neverChangesOnceMade`, `sitesOfObjectsThatNeverChange`, `StoreCount::Never/AtMostOnce`, `FieldStability`, `MemberMutability::Fixed`, `constantLists`. Shared ∧ immutable needs only safe publication.
9. **Frame-local objects** (`AllocateInFrames`, `SinkAllocations`, flattened inner objects whose holder is local) are thread-local without any new analysis.
10. **Built-in containers are per call site** in the solve and in the results (`ValueFlowContainerFacts` per object), so "this Map/array never leaves its thread" can be precise.
11. **Function versioning exists** (entry checks / typed body / generic version, instanced functions, derived layouts), so "main-only version" vs "any-thread version" is an extension of a known technique rather than a new concept.
12. **Bisection and reports**: each result kind can be disabled per subject range; `UnprovenCause` + `Witness` explain *why* something is not proven (`bun compile --explain`, `BUN/docs/bundler/aot-explain.mdx`). "Shared because of this statement" falls out of `MarkWitnesses` (`ResultsObjects.cpp:623-695`).
13. **Determinism under schedules** is already a tested property of the solver, so new rules get schedule fuzzing (`DelayFlowRules`) and the fixpoint check for free.

---

## 13. Not found / not sure

- **No trace of JS threads** in `aot/valueflow/`, the table vocabulary or the schema: no word for "thread" other than the build's own worker threads, `HostReadContext "Worker"`, `sharingMode: "Shared"` for `SharedArrayBuffer`, and the comment on workers at `ResultsBuiltins.cpp:1184`.
- **`startsInstance`**: defined (`vocabulary.ts:552-553`, flag `StartsInstance`), consumed by `countProgramInstances`, but I found **no declaration that sets it** in either checkout (grep over `aot/` and `BUN/src`, `BUN/test`). The Bun checkout is sparse, so a setter may exist elsewhere. Its comment: "Makes a second realm that runs the compiled program in the VM of its caller".
- **`FrameLocality` / `AllocationClass.locality`**: in the schema only; no producer or consumer found.
- **`Worker.prototype.postMessage`, `MessagePort.prototype.postMessage`, `BroadcastChannel`**: no hand-written rows found; what I say about `worker.postMessage(obj)` (operands escape) is inferred from `hasPrototype: false` and `callMemberNobodyKnows`, not observed in a test. There may be rows produced by the collectors (`collect-*.ts`) that I did not run.
- **How `onmessage` data is typed**: only `spreadArgs: b.any`. I did not look for a `MessageEvent.data` row.
- **Real-program statistics** of the solve (nodes, edges, rounds, size of `escaped`/`lost`, proven vs unproven positions): counters exist, no numbers are checked in or quoted apart from the PR lines cited in 8.1. The split of the 9.1 s between bodies / rounds / results is not in the PR text.
- **`merge()`, `link*`, made-node "parts" and "at rest" logic** (`Solver.cpp:698-2600`): read only in outline. My cost estimate for a `Read(shared, EveryProperty)` node (8.3) rests on header comments, not on measurement.
- **`ValueFlowHeapTables`** (`vf/AOTValueFlowHeapTables.h/.cpp`, ~1,000 lines): used through its accessors only.
- **`AOTFlowOrder*` internals** (2,300 lines): I read the headers and the comments, not the algorithms. In particular I did not verify how a `BuiltinCallback` with `times: Later` is attributed in `FlowOrderInputs.calls` (to the calling statement's function and site, or to `host()`); `ResultsOrder.cpp:85-86` suggests: to the statement if an operation of a body asked for the group directly, to the host if a made statement did. For a Thread entry the difference matters: attributed to the `new Thread(...)` site, the order analysis would conclude "the
  thread function starts after this point", which is causally right and is exactly hazard H1.
- **Whether `capability: "heap"` is acceptable for a constructor that starts a thread** with respect to other consumers of `capability` (collection points, `OperationEnteredVM` validation): not checked.
- **`Atomics.*` rows**: `aot/builtins/AOTBufferBuiltinEmitterAtomics.cpp` exists and `buffer.builtins.ts`/`typedarray.builtins.ts` declare them; I did not read how `Atomics.wait/notify` are described.
- **Consumers in `passes/`** are listed by grep (file names), not read, except `AOTReuseEnvironments.cpp` comments, `AOTFrameAllocation.h`, `AOTRemoveUnreachableFunctions.h`. The V/M grades in section 11 for pass-level hazards (H3, H5, H7) are my assessment and should be cross-checked with the reports on objects, proofs and calls (R1, R2, R6).
- **The generic version and the interpreter**: interpreted code (`eval`) in an open program runs against the same objects; flow treats it as unknown JavaScript. Whether interpreted code may start threads is a design question, not answered by anything I read.
