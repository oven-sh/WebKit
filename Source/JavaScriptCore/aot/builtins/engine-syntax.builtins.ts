import * as b from "bun-builtins-table";

const intrinsics = (position: b.IntrinsicPosition, meaning: b.IntrinsicSpec, names: readonly string[]) =>
  names.flatMap(name => b.intrinsic(name, position, meaning));

const operation = (opcode: string, ...names: readonly string[]) =>
  intrinsics("Call", { intrinsic: "Operation", opcode }, names);

const jsTypeTests = (jsTypes: Readonly<Record<string, string>>) =>
  Object.entries(jsTypes).flatMap(([name, jsType]) =>
    b.intrinsic(name, "Call", { intrinsic: "HasJSType", first: jsType, last: jsType }),
  );

const enumerators = (
  cppEnum: string,
  members: Readonly<Record<string, readonly [value: number, enumerator: string]>>,
) =>
  Object.entries(members).flatMap(([name, [value, enumerator]]) =>
    b.intrinsic(name, "Value", { intrinsic: "Int", value, cppExpression: `${cppEnum}::${enumerator}` }),
  );

const builtinObjects = (...paths: readonly string[]) =>
  paths.flatMap(path => b.intrinsic(path, "Value", { intrinsic: "Builtin", path }));

const linkTimeConstants = (...names: readonly string[]) =>
  names.flatMap(name => b.intrinsic(name, "Call", { intrinsic: "Builtin", path: b.linkTimeConstantPath(name) }));

const symbolKeys = (...symbols: readonly string[]) =>
  symbols.flatMap(symbol => b.intrinsic(symbol, "Member", { intrinsic: "SymbolKey", symbol }));

const textEvaluators = ["Function", "%AsyncFunction%", "%GeneratorFunction%", "%AsyncGeneratorFunction%", "eval"];

// No object holds these. The interpreter calls them for text that never names them: a spread without a fast path, and `using`.
// There is text once an evaluator is called. `Function.prototype` and `x instanceof Function` evaluate none.
const functionsCalledForEvaluatedText = [
  b.functionIn("IteratorHelpers", "performIteration"),
  b.functionIn("DisposableStackPrototype", "getDisposeMethod"),
  b.functionIn("DisposableStackPrototype", "getAsyncDisposeMethod"),
].map(called => ({ function: called, when: b.whenCallRunsOtherCode }));

export default b.declarations(
  textEvaluators.flatMap(path => b.extend(path, { callsFunctions: functionsCalledForEvaluatedText })),
  b.excluded("IteratorHelpers", "builtinMapIterable", "builtinSetIterable", "wrappedIterator"),
  b.excluded(
    "ProxyHelpers",
    "performProxyObjectGet",
    "performProxyObjectGetByVal",
    "performProxyObjectHas",
    "performProxyObjectHasByVal",
    "performProxyObjectSetByValSloppy",
    "performProxyObjectSetByValStrict",
    "performProxyObjectSetSloppy",
    "performProxyObjectSetStrict",
  ),
  b.excluded(
    "InjectedScriptSource",
    "createArrayWithoutPrototype",
    "createInspectorInjectedScript",
    "createObjectWithoutPrototype",
  ),
  b.excluded("ArrayIteratorPrototype", "arrayIteratorNextHelper"),
  b.excluded("GeneratorPrototype", "generatorResume"),
  b.excluded("ShadowRealmPrototype", "crossRealmThrow", "evaluate", "importValue", "wrapRemoteValue"),
  b.handwritten(
    "ArrayIteratorPrototype",
    "AsyncDisposableStackPrototype",
    "DisposableStackPrototype",
    "FunctionPrototype",
    "GeneratorPrototype",
    "IteratorOperations",
    "JSIteratorHelperPrototype",
    "MapIteratorPrototype",
    "MapPrototype",
    "ObjectConstructor",
    "SetIteratorPrototype",
    "SetPrototype",
    "TypedArrayPrototype",
  ),

  b.intrinsic("alwaysInline", "Value", { intrinsic: "Directive", directive: "AlwaysInline" }),
  b.intrinsic("nakedConstructor", "Value", { intrinsic: "Directive", directive: "NakedConstructor" }),

  b.intrinsic("undefined", "Value", { intrinsic: "Undefined" }),
  b.intrinsic("arrayProtoValues", "Value", { intrinsic: "Builtin", path: "Array.prototype.values" }),
  b.intrinsic("Infinity", "Value", { intrinsic: "Double", value: Infinity }),
  b.intrinsic("MAX_SAFE_INTEGER", "Value", {
    intrinsic: "Int",
    value: Number.MAX_SAFE_INTEGER,
    cppExpression: "maxSafeInteger()",
  }),

  b.intrinsic("toNumber", "Call", { intrinsic: "Conversion", conversion: "ToNumber" }),
  b.intrinsic("toPropertyKey", "Call", { intrinsic: "Conversion", conversion: "ToPropertyKey" }),
  b.intrinsic("toString", "Call", { intrinsic: "Conversion", conversion: "ToString" }),
  b.intrinsic("throwRangeError", "Call", { intrinsic: "Throw", error: "RangeError" }),
  b.intrinsic("throwTypeError", "Call", { intrinsic: "Throw", error: "TypeError" }),
  b.intrinsic("ifAbruptCloseIterator", "Call", { intrinsic: "IfAbruptCloseIterator" }),

  symbolKeys("asyncDispose", "asyncIterator", "dispose", "iterator"),
  intrinsics("Member", { intrinsic: "PrivateName" }, ["create", "getOwnPropertyDescriptor"]),

  operation(
    "GetInternalField",
    "getAsyncDisposableStackInternalField",
    "getDisposableStackInternalField",
    "getIteratorHelperInternalField",
    "getWrapForValidIteratorInternalField",
  ),
  operation(
    "PutInternalField",
    "putAsyncDisposableStackInternalField",
    "putDisposableStackInternalField",
  ),
  operation("ArraySpeciesCreate", "newArrayWithSpecies"),
  operation("ConstructList", "constructList"),
  operation("CreatePromise", "createPromise"),
  operation("GetByValWithThis", "getByValWithThis"),
  operation("IsConstructor", "isConstructor"),
  operation("ReadsIteratorOfListUnseen", "readsIteratorOfListUnseen"),
  operation("ToObject", "toObject"),

  jsTypeTests({
    isArrayIterator: "JSArrayIteratorType",
    isAsyncDisposableStack: "AsyncDisposableStackType",
    isDisposableStack: "DisposableStackType",
    isIteratorHelper: "JSIteratorHelperType",
    isJSArray: "ArrayType",
    isMap: "JSMapType",
    isPromise: "JSPromiseType",
    isProxyObject: "ProxyObjectType",
    isRegExpObject: "RegExpObjectType",
    isSet: "JSSetType",
    isWrapForValidIterator: "JSWrapForValidIteratorType",
    isMapIterator: "JSMapIteratorType",
    isSetIterator: "JSSetIteratorType",
  }),
  b.intrinsic("isTypedArrayView", "Call", {
    intrinsic: "HasJSType",
    first: "Int8ArrayType",
    last: "BigUint64ArrayType",
  }),

  enumerators("JSAsyncDisposableStack::Field", {
    asyncDisposableStackFieldState: [0, "State"],
    asyncDisposableStackFieldCapability: [1, "Capability"],
  }),
  enumerators("JSAsyncDisposableStack::State", { AsyncDisposableStackStateDisposed: [1, "Disposed"] }),
  enumerators("JSDisposableStack::Field", {
    disposableStackFieldState: [0, "State"],
    disposableStackFieldCapability: [1, "Capability"],
  }),
  enumerators("JSDisposableStack::State", { DisposableStackStateDisposed: [1, "Disposed"] }),
  enumerators("JSIteratorHelper::Field", {
    iteratorHelperFieldGenerator: [0, "Generator"],
    iteratorHelperFieldUnderlyingIterator: [1, "UnderlyingIterator"],
  }),
  enumerators("ProxyObject::Field", { proxyFieldTarget: [0, "Target"], proxyFieldHandler: [1, "Handler"] }),
  enumerators("JSWrapForValidIterator::Field", {
    wrapForValidIteratorFieldIteratedIterator: [0, "IteratedIterator"],
    wrapForValidIteratorFieldIteratedNextMethod: [1, "IteratedNextMethod"],
  }),

  builtinObjects(
    "AsyncDisposableStack",
    "DisposableStack",
    "Iterator",
    "Map",
    "Object",
    "ReferenceError",
    "Set",
    "SuppressedError",
    "WeakMap",
    "WeakSet",
  ),

  linkTimeConstants(
    "arrayFromFastWithoutMapFn",
    "asyncFromSyncIteratorCreate",
    "instanceOf",
    "isTypedArrayOutOfBounds",
    "iteratorHelperCreate",
    "makeTypeError",
    "newRejectedPromise",
    "newResolvedPromise",
    "promiseReject",
    "promiseResolve",
    "toIntegerOrInfinity",
    "typedArrayFromFast",
    "typedArrayLength",
    "wrapForValidIteratorCreate",
  ),
  b.intrinsic("ownKeys", "Call", { intrinsic: "Builtin", path: "Reflect.ownKeys" }),
  b.intrinsic("functionPrototypeApply", "Value", { intrinsic: "Builtin", path: "Function.prototype.apply" }),
  b.intrinsic("functionPrototypeCall", "Value", { intrinsic: "Builtin", path: "Function.prototype.call" }),
  intrinsics("Call", { intrinsic: "LinkTimeConstant" }, [
    "performPromiseThen",
    "rejectPromise",
    "resolvePromise",
  ]),
  intrinsics("Value", { intrinsic: "LinkTimeConstant" }, ["promiseReturnUndefinedOnFulfilled"]),

  intrinsics("Call", { intrinsic: "LinkedFunction" }, [
    "addDisposableResource",
    "closeAllIterators",
    "createDisposableResource",
    "flatIntoArray",
    "flatIntoArrayWithCallback",
    "getAsyncDisposeMethod",
    "getDisposeMethod",
    "getIteratorFlattenable",
    "getIteratorSync",
    "getOptionsObject",
    "iteratorCloseAllNormal",
    "iteratorZip",
    "removeFirstFromList",
  ]),
  // The engine emits these two in line, and so does the front end: the call then belongs to the built-in that makes it.
  b.intrinsic("iteratorGenericClose", "Call", { intrinsic: "LinkedFunction", expansion: "IteratorGenericClose" }),
  b.intrinsic("iteratorGenericNext", "Call", { intrinsic: "LinkedFunction", expansion: "IteratorGenericNext" }),
  intrinsics("Value", { intrinsic: "LinkedFunction" }, [
    "defaultAsyncFromAsyncArrayLike",
    "defaultAsyncFromAsyncIterator",
  ]),

  operation("CollectionTable", "collectionTable"),
  operation("CreateIteratorResult", "makeIteratorResult"),
  operation("IsGeneratorSuspendedAtStart", "isGeneratorSuspendedAtStart"),
  operation("MapNext", "mapNext"),
  operation("ResultAt", "resultAt"),
  operation("ResumeGenerator", "resumeGenerator"),
  operation("SetNext", "setNext"),
  operation("StepArrayIterator", "stepArrayIterator"),
  operation("StepMapIterator", "stepMapIterator"),
  operation("StepSetIterator", "stepSetIterator"),
  operation("StepGenerator", "stepGenerator"),

  enumerators("Typed::ArrayStorage", {
    arrayStorageInt32: [1, "Int32"],
    arrayStorageDouble: [2, "Double"],
    arrayStorageContiguous: [3, "Contiguous"],
  }),
  enumerators("Typed::GeneratorStepCheck", { generatorStepCheckIsGenerator: [1, "IsGenerator"] }),
  enumerators("Typed::GeneratorStepStatus", {
    generatorStepStatusCompleted: [1, "Completed"],
    generatorStepStatusNotStepped: [2, "NotStepped"],
  }),
  enumerators("Typed::MapNextResult", {
    mapNextResultHasEntry: [0, "HasEntry"],
    mapNextResultEntry: [1, "Entry"],
    mapNextResultKey: [2, "Key"],
    mapNextResultValue: [3, "Value"],
  }),
  enumerators("Typed::ResumeMode", {
    resumeModeNext: [0, "Next"],
    resumeModeThrow: [1, "Throw"],
    resumeModeReturn: [2, "Return"],
  }),
  enumerators("Typed::SetNextResult", {
    setNextResultHasEntry: [0, "HasEntry"],
    setNextResultEntry: [1, "Entry"],
    setNextResultKey: [2, "Element"],
  }),
  enumerators("Typed::StepArrayIteratorResult", {
    stepArrayIteratorResultDone: [0, "Done"],
    stepArrayIteratorResultValue: [1, "Value"],
  }),
  enumerators("Typed::StepGeneratorResult", {
    stepGeneratorResultValue: [0, "Value"],
    stepGeneratorResultStatus: [1, "Status"],
  }),
);
