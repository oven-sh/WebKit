import * as b from "bun-builtins-table";

type ArgumentTypes = Readonly<Record<string, b.TableType>>;
type ArgumentUses = Readonly<Record<string, readonly b.UseSpec[]>>;

const internalFields = "InternalFields";
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;

const item = b.iterated("this");
const iteratorProtocol = b.runs("Iterate", b.calls("next", "return"));
const underlyingIterator = b.storedIn(b.resultSlot("iterated"));
const newHelper = b.instance("IteratorHelper").fresh;
const accumulator = b.union(b.operand("initialValue"), item, b.callbackResult("reducer"));
const updatesState = [b.readsContents(), b.writesContents()];

const promiseOfIteratorResult = b
  .instance("Promise")
  .holding({ value: b.ordinaryObjectOf("IteratorResult"), reason: b.any }).fresh;
const rejectedPromise = b.instance("Promise").holding({ value: b.never, reason: b.instance("TypeError").unaliased }).fresh;

const itemCallback = (times: b.CallbackTimes, result: b.CallbackResultUse) => [
  b.called({ times, args: [item, b.length], result }),
];

const overridableAccessor = (name: string, holds: b.TableType, capability: b.Capability) =>
  ({
    this: b.any,
    getter: holds,
    capability,
    uses: { this: b.inspected },
    implementation: b.generic,
    setter: {
      this: b.object,
      value: b.any,
      capability: "js",
      throws: true,
      uses: {
        this: [b.definesName([name], b.operand("value")), b.storedByKey(b.keysNamed(name), b.operand("value"))],
        value: b.inspected,
      },
      implementation: b.generic,
    },
  }) satisfies b.GetterField;

const zipFunction = (name: string) =>
  ({
    ...operandCodeEffects,
    args: { iterables: b.object, options: b.object.optional },
    returns: newHelper,
    uses: { iterables: [b.everything], options: [b.everything] },
    implementation: b.compiled("JSIteratorConstructor", name),
  }) satisfies b.FunctionField;

const helperMethod = (name: string, args: ArgumentTypes, uses: ArgumentUses) =>
  ({
    ...operandCodeEffects,
    this: b.object,
    args,
    returns: newHelper,
    uses: { this: [iteratorProtocol, underlyingIterator], ...uses },
    implementation: b.compiled("JSIteratorPrototype", name),
  }) satisfies b.FunctionField;

const consumingMethod = (
  implementation: b.ImplementationSpec,
  returns: b.TableType,
  args: ArgumentTypes = {},
  uses: ArgumentUses = {},
) =>
  ({
    ...operandCodeEffects,
    this: b.object,
    args,
    returns,
    uses: { this: [iteratorProtocol], ...uses },
    implementation,
  }) satisfies b.FunctionField;

const testingMethod = (name: string, returns: b.TableType) =>
  consumingMethod(
    b.compiled("JSIteratorPrototype", name),
    returns,
    { predicate: b.any },
    { predicate: itemCallback("PerElement", "Tested") },
  );

// What is sent goes into the body of the generator, which does with it what its own code says. A generator nobody knows holds it like native code.
const resumeMethod = (name: string, parameter: string, ...sent: readonly b.UseSpec[]) =>
  ({
    args: { [parameter]: b.any },
    returns: b.layoutOf("IteratorResult").fresh,
    capability: "js",
    throws: true,
    uses: { this: updatesState, [parameter]: [b.retained, ...sent] },
    implementation: b.compiled("GeneratorPrototype", name),
  }) satisfies b.FunctionField;

// The jobs that run the body hold an async generator while a step is under way, and they do nothing else with it: whoever steps it hands it to native code no more than whoever steps a generator.
// The promise is fulfilled with an iterator result whose value is what the generator hands out. A layout has one type for each field, so no type says that here: the rule of flow for the steps of built-in iterators does.
const asyncResumeMethod = (parameter: string, ...sent: readonly b.UseSpec[]) =>
  [
    {
      args: { [parameter]: b.any },
      returns: promiseOfIteratorResult,
      capability: "js",
      uses: { this: updatesState, [parameter]: [b.retained, ...sent] },
      implementation: b.hostCall,
    },
    {
      this: b.any,
      args: { [parameter]: b.any },
      returns: rejectedPromise,
      capability: "heap",
      throws: true,
      uses: { this: b.inspected, [parameter]: b.inspected },
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const disposalCallback = (...args: readonly b.PlaceLike[]) => [
  b.retained,
  b.called({ times: "Later", args, result: "Ignored" }),
];

export default b.declarations(
  b.class("Iterator", {
    isNeverPristine: true,
    lockedMembers: ["prototype"],
    call: { returns: b.never, capability: "heap", throws: true, implementation: b.hostCall },
    // Only for a class that extends it: with itself as new.target it throws.
    construct: { ...operandCodeEffects, returns: b.instance("Iterator").fresh, implementation: b.hostCall },
    static: {
      from: {
        ...operandCodeEffects,
        args: { value: b.any },
        returns: b.object,
        uses: { value: [b.runs("Iterate", "PrototypeChain", b.calls("next")), b.retained, b.returned] },
        implementation: b.compiled("JSIteratorConstructor", "from"),
      },
      concat: {
        ...operandCodeEffects,
        args: { items: b.any.rest },
        returns: newHelper,
        uses: { items: [b.runs("Iterate"), b.retained] },
        implementation: b.compiled("JSIteratorConstructor", "concat"),
      },
      zip: zipFunction("zip"),
      zipKeyed: zipFunction("zipKeyed"),
    },
    proto: {
      constructor: overridableAccessor("constructor", b.builtin("Iterator"), "none"),
      "@@toStringTag": overridableAccessor("@@toStringTag", b.string, "heap"),
      "@@iterator": {
        this: b.any,
        returns: b.operand("this"),
        capability: "none",
        uses: { this: b.inspected },
        implementation: b.inline("ReturnThis"),
      },
      "@@dispose": {
        ...operandCodeEffects,
        this: b.any.refusing(b.null),
        uses: { this: [iteratorProtocol] },
        implementation: b.compiled("JSIteratorPrototype", "dispose"),
      },
      map: helperMethod("map", { mapper: b.any }, { mapper: itemCallback("Later", "Escapes") }),
      filter: helperMethod("filter", { predicate: b.any }, { predicate: itemCallback("Later", "Tested") }),
      flatMap: helperMethod("flatMap", { mapper: b.any }, { mapper: itemCallback("Later", "Escapes") }),
      take: helperMethod("take", { limit: b.any }, { limit: [b.runs("ToPrimitive")] }),
      drop: helperMethod("drop", { limit: b.any }, { limit: [b.runs("ToPrimitive")] }),
      chunks: helperMethod("chunks", { chunkSize: b.any }, { chunkSize: b.inspected }),
      windows: helperMethod(
        "windows",
        { windowSize: b.any, undersized: b.any.optional },
        { windowSize: b.inspected, undersized: b.inspected },
      ),
      some: testingMethod("some", b.bool),
      every: testingMethod("every", b.bool),
      find: testingMethod("find", b.union(item, b.undefined)),
      forEach: {
        ...consumingMethod(
          b.hostCall,
          b.undefined,
          { procedure: b.any },
          { procedure: itemCallback("PerElement", "Ignored") },
        ),
        model: b.functionIn("models/Iterators", "forEach"),
      },
      reduce: consumingMethod(
        b.compiled("JSIteratorPrototype", "reduce"),
        accumulator,
        { reducer: b.any, initialValue: b.any.optional },
        {
          reducer: [
            b.called({ times: "Once", args: [b.operand("initialValue"), item, b.length], result: "Returned" }),
            b.called({ times: "PerElement", args: [b.result, item, b.length], result: "Returned" }),
          ],
          initialValue: b.inspected,
        },
      ),
      toArray: {
        ...consumingMethod(b.hostCall, b.instance("Array").holding({ element: item }).fresh),
        model: b.functionIn("models/Iterators", "toArray"),
      },
      includes: consumingMethod(
        b.hostCall,
        b.bool,
        { searchElement: b.any, skippedElements: b.any.optional },
        { searchElement: b.inspected, skippedElements: b.inspected },
      ),
      join: consumingMethod(
        b.hostCall,
        b.string,
        { separator: b.any.optional },
        {
          this: [b.runsTransitively("Iterate", "ToPrimitive", b.calls("next", "return"))],
          separator: [b.runs("ToPrimitive")],
        },
      ),
    },
  }),

  b.class("%Generator%", {
    extends: "Iterator",
    instanceCppClass: b.cppClass("Generator", "AOTGenerator.h"),
    isNeverPristine: true,
    hasConstructor: false,
    lockedObjects: ["prototype"],
    slots: { element: b.contents("Element", internalFields) },
    types: [
      b.viewType("Generator", "element"),
      b.viewType("Iterator", "element"),
      b.viewType("IterableIterator", "element"),
      b.viewType("IteratorObject", "element"),
    ],
    prototypeTable: b.staticTable("runtime/GeneratorPrototype.cpp", "generatorPrototypeTable"),
    proto: {
      next: resumeMethod("next", "value"),
      return: resumeMethod("return", "value", b.storedIn(b.resultField("value"))),
      throw: resumeMethod("throw", "exception"),
    },
  }),
  b.same("%Generator%.prototype.constructor", "%GeneratorFunction%.prototype"),
  b.same("%GeneratorFunction%.prototype.prototype", "%Generator%.prototype"),

  b.class("%AsyncGenerator%", {
    extends: "AsyncIterator",
    jsType: "JSAsyncGeneratorType",
    isNeverPristine: true,
    hasConstructor: false,
    lockedObjects: ["prototype"],
    slots: { element: b.contents("Element", internalFields) },
    types: [
      b.viewType("AsyncGenerator", "element"),
      b.viewType("AsyncIterator", "element"),
      b.viewType("AsyncIterableIterator", "element"),
      b.viewType("AsyncIteratorObject", "element"),
      b.viewType("AsyncIterable", "element"),
    ],
    prototypeTable: b.staticTable("runtime/AsyncGeneratorPrototype.cpp", "asyncGeneratorPrototypeTable"),
    proto: {
      next: asyncResumeMethod("value"),
      // The value is awaited: the constructor of a promise is read and the promise gets a reaction, and the `then` of any other object is called.
      return: asyncResumeMethod("value", b.runs("Thenable", b.named("constructor")), ...updatesState),
      throw: asyncResumeMethod("exception", b.storedIn(b.resultSlot("reason"))),
    },
  }),
  b.same("%AsyncGenerator%.prototype.constructor", "%AsyncGeneratorFunction%.prototype"),
  b.same("%AsyncGeneratorFunction%.prototype.prototype", "%AsyncGenerator%.prototype"),

  b.class("DisposableStack", {
    jsType: "DisposableStackType",
    stateHeap: internalFields,
    types: [b.exactType("DisposableStack")],
    call: { returns: b.never, capability: "heap", throws: true, implementation: b.hostCall },
    construct: {
      returns: b.instance("DisposableStack").fresh,
      capability: "heap",
      implementation: b.hostCall,
    },
    proto: {
      disposed: {
        getter: b.bool,
        capability: "none",
        uses: { this: [b.readsContents()] },
        implementation: b.generic,
      },
      use: {
        ...operandCodeEffects,
        args: { value: b.any },
        returns: b.operand("value"),
        uses: { this: updatesState, value: [b.retained, b.runs(b.calls("@@dispose"))] },
        implementation: b.compiled("DisposableStackPrototype", "use"),
      },
      adopt: {
        ...operandCodeEffects,
        args: { value: b.any, onDispose: b.any },
        returns: b.operand("value"),
        uses: { this: updatesState, value: [b.retained], onDispose: disposalCallback(b.operand("value")) },
        implementation: b.compiled("DisposableStackPrototype", "adopt"),
      },
      defer: {
        ...operandCodeEffects,
        args: { onDispose: b.any },
        uses: { this: updatesState, onDispose: disposalCallback() },
        implementation: b.compiled("DisposableStackPrototype", "deferMethod"),
      },
      move: {
        returns: b.instance("DisposableStack").fresh,
        capability: "heap",
        throws: true,
        uses: { this: updatesState },
        implementation: b.compiled("DisposableStackPrototype", "move"),
      },
      dispose: {
        capability: "js",
        throws: true,
        uses: { this: updatesState },
        implementation: b.compiled("DisposableStackPrototype", "dispose"),
      },
    },
  }),
  b.same("DisposableStack.prototype.@@dispose", "DisposableStack.prototype.dispose"),

  // `import "core-js"` replaces them without a test, from 3.45 to 3.49.
  b.replacedByPackages("Iterator.prototype.chunks", "Iterator.prototype.windows"),
);
