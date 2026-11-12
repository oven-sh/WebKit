import * as b from "bun-builtins-table";

const self = b.operand("this");
const element = b.slot("this", "element");
const elementOrUndefined = b.union(element, b.undefined);
const callbackResult = b.callbackResult("callback");
const newArrayOf = (elements: b.TableType) => b.instance("Array").exactly.holding({ element: elements }).fresh;
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;
const methodsWrittenTwice = new Set(["every", "filter", "find", "findIndex", "forEach", "map", "reduce", "some"]);
const textOnList = (name: string) => (methodsWrittenTwice.has(name) ? `${name}OfList` : name);

interface Receiver {
  type: b.TableType;
  element: b.PlaceLike;
  uses: readonly b.UseSpec[];
}

const arrayReceiver = (visited: b.TableType, ...aspects: readonly b.PristineAspect[]): Receiver => ({
  type: b.instance("Array").refusing(b.null).pristine(...aspects),
  element: visited,
  uses: [b.readsContents()],
});

const arraySkippingHoles = arrayReceiver(element, "Storage");
const arrayReadingHoles = arrayReceiver(elementOrUndefined, "Storage");
const arrayWithDefaultSpecies = arrayReceiver(element, "Storage", "Names");
const arrayLike: Receiver = {
  type: b.object.loose.refusing(b.null),
  element: b.ownValues("this"),
  uses: [b.runs("OwnValues", "PrototypeChain", "ToPrimitive", b.named("length"))],
};

interface Outcome {
  returns?: b.TableType;
  result: b.CallbackResultUse;
  resultTo?: b.PlaceLike;
  receiverUses?: readonly b.UseSpec[];
}

const traversalOverload = (name: string, receiver: Receiver, outcome: Outcome) =>
  ({
    ...operandCodeEffects,
    this: receiver.type,
    args: { callback: b.callable, thisArg: b.any.optional },
    returns: outcome.returns,
    uses: {
      this: [...receiver.uses, ...(outcome.receiverUses ?? [])],
      callback: [
        b.called({
          afterwards: "TouchesOnlyWhatRowNames",
          times: "PerElement",
          this: b.operand("thisArg"),
          args: [receiver.element, b.length, self],
          result: outcome.result,
          resultTo: outcome.resultTo,
        }),
      ],
      thisArg: b.inspected,
    },
    implementation: b.compiled("ArrayPrototype", receiver === arrayLike ? name : textOnList(name)),
  }) satisfies b.FunctionField;

const traversalMethod = (name: string, array: Receiver, onArray: Outcome, onArrayLike: Outcome = onArray) =>
  b.method(`Array.prototype.${name}`, [
    traversalOverload(name, array, onArray),
    traversalOverload(name, arrayLike, onArrayLike),
  ]);

const booleanAnswer: Outcome = { returns: b.bool, result: "Tested" };
const foundElement: Outcome = { returns: elementOrUndefined, result: "Tested" };
const foundOwnValue: Outcome = { returns: b.any, result: "Tested", receiverUses: [b.contentsStoredIn(b.result)] };
const foundArrayIndex: Outcome = { returns: b.int(-1, b.checked(2 ** 32 - 1, "UINT32_MAX")), result: "Tested" };
const foundIndex: Outcome = { returns: b.int(-1, Number.MAX_SAFE_INTEGER), result: "Tested" };

const arrayFromSpecies = b.union(b.instance("Array").dense, b.object).freshUnlessSpecies;
const arrayFromSpeciesWithHoles = b.union(b.instance("Array"), b.object).freshUnlessSpecies;
const elementOfResult = b.resultSlot("element");

const speciesResult = (result: b.CallbackResultUse, ...receiverUses: readonly b.UseSpec[]): Outcome => ({
  returns: result === "Escapes" ? b.object : result === "Stored" ? arrayFromSpeciesWithHoles : arrayFromSpecies,
  result,
  resultTo: result === "Stored" ? elementOfResult : undefined,
  receiverUses: [b.runs("Species"), ...receiverUses],
});

interface Fold {
  receiver: Receiver;
  initialValue: b.TableType;
  accumulator: b.TableType;
  result: b.CallbackResultUse;
  receiverUses?: readonly b.UseSpec[];
}

const folds: readonly Fold[] = [
  {
    receiver: arraySkippingHoles,
    initialValue: b.notPassed,
    accumulator: b.union(element, callbackResult),
    result: "Returned",
  },
  {
    receiver: arraySkippingHoles,
    initialValue: b.any,
    accumulator: b.union(b.operand("initialValue"), callbackResult),
    result: "Returned",
  },
  {
    receiver: arrayLike,
    initialValue: b.notPassed,
    accumulator: b.any,
    result: "Escapes",
    receiverUses: [b.contentsStoredIn(b.nativeMemory)],
  },
  {
    receiver: arrayLike,
    initialValue: b.any,
    accumulator: b.union(b.operand("initialValue"), callbackResult),
    result: "Returned",
  },
];

const foldOverload = (name: string, fold: Fold) =>
  ({
    ...operandCodeEffects,
    this: fold.receiver.type,
    args: { callback: b.callable, initialValue: fold.initialValue },
    returns: fold.accumulator,
    uses: {
      this: [...fold.receiver.uses, ...(fold.receiverUses ?? [])],
      callback: [
        b.called({
          afterwards: "TouchesOnlyWhatRowNames",
          times: "PerElement",
          this: b.undefined,
          args: [fold.accumulator, fold.receiver.element, b.length, self],
          result: fold.result,
        }),
      ],
      initialValue: b.inspected,
    },
    implementation: b.compiled("ArrayPrototype", fold.receiver === arrayLike ? name : textOnList(name)),
  }) satisfies b.FunctionField;

const foldMethod = (name: string) => b.method(`Array.prototype.${name}`, folds.map(fold => foldOverload(name, fold)));

const comparatorCall = (compared: b.PlaceLike) =>
  b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Repeatedly", this: b.undefined, args: [compared, compared], result: "Escapes" });
const stringComparison = b.runsOnContents("ToPrimitive");
const movesOwnValues = [b.movesElements, b.storedByKey(b.indexKeys, b.ownValues("this")), b.deletedByKey(b.indexKeys)];

interface SortOutcome {
  returns: b.TableType;
  receiverUses: readonly b.UseSpec[];
}

interface SortOutcomeOnArrayLike extends SortOutcome {
  receiver: b.TableType;
}

const wrapperOfReceiver = b.union(...(["Boolean", "Number", "String", "Symbol", "BigInt"] as const).map(name => b.instance(name))).fresh;

const sortMethod = (name: string, onArray: SortOutcome, onArrayLike: readonly SortOutcomeOnArrayLike[], columns: b.EntryColumns = {}) =>
  b.method(`Array.prototype.${name}`, [
    {
      ...operandCodeEffects,
      ...columns,
      this: arraySkippingHoles.type,
      args: { comparator: b.callable },
      returns: onArray.returns,
      uses: { this: onArray.receiverUses, comparator: [comparatorCall(element)] },
      implementation: b.compiled("prelude/array-sort", name),
    },
    {
      ...operandCodeEffects,
      this: arraySkippingHoles.type,
      args: { comparator: b.undefined.optional },
      returns: onArray.returns,
      uses: { this: [...onArray.receiverUses, stringComparison] },
      implementation: b.hostCall,
    },
    ...onArrayLike.map(outcome => ({
      ...operandCodeEffects,
      this: outcome.receiver,
      args: { comparator: b.callable.optional },
      returns: outcome.returns,
      uses: {
        this: [...arrayLike.uses, stringComparison, ...outcome.receiverUses],
        comparator: [comparatorCall(arrayLike.element)],
      },
      implementation: b.hostCall,
    })),
  ]);

const itemsProtocols = b.runs("Iterate", "OwnValues", "PrototypeChain", "ToPrimitive", b.named("length"));
const builtinIterable = b.union(
  b.string,
  ...["Set", "Map", "ArrayIterator", "SetIterator", "MapIterator", "StringIterator"].map(name => b.instance(name).pristine("Names")),
);

const listAsItsClassMakesIt = b.instance("Array").pristine("Storage", "Names");
const elementOfItemsOrUndefined = b.union(b.slot("items", "element"), b.undefined);

const mapperCall = (item: b.PlaceLike) =>
  b.called({
    afterwards: "TouchesOnlyWhatRowNames",
    times: "PerElement",
    this: b.operand("thisArg"),
    args: [item, b.length],
    result: "Stored",
    resultTo: b.resultSlot("element"),
  });

export default b.declarations(
  b.static("Object.groupBy", {
    ...operandCodeEffects,
    args: { items: b.any, callback: b.callable },
    returns: b.object.unaliased,
    uses: {
      items: [b.runs("Iterate"), b.iteratedValuesStoredIn(b.nativeMemory)],
      callback: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "PerElement", this: b.undefined, args: [b.iterated("items"), b.length], result: "Escapes" })],
    },
    implementation: b.compiled("ObjectConstructor", "groupBy"),
  }),

  b.static("Array.from", [
    {
      args: { items: b.instance("TypedArray").pristine("Names"), mapper: b.undefined.optional },
      returns: newArrayOf(b.slot("items", "element")).dense,
      capability: "heap",
      throws: true,
      uses: { this: b.inspected, items: [b.readsContents()] },
      requiresHolderAsThis: true,
      implementation: b.compiled("ArrayConstructor", "from"),
    },
    {
      ...operandCodeEffects,
      args: { items: listAsItsClassMakesIt, mapper: b.undefined.optional },
      returns: newArrayOf(elementOfItemsOrUndefined).dense,
      uses: { this: b.inspected, items: [b.readsContents()] },
      implementation: b.compiled("ArrayConstructor", "from"),
    },
    {
      ...operandCodeEffects,
      args: { items: listAsItsClassMakesIt, mapper: b.callable, thisArg: b.any.optional },
      returns: newArrayOf(b.callbackResult("mapper")).dense,
      uses: {
        this: b.inspected,
        items: [b.readsContents()],
        mapper: [mapperCall(elementOfItemsOrUndefined)],
        thisArg: b.inspected,
      },
      implementation: b.compiled("ArrayConstructor", "from"),
    },
    {
      ...operandCodeEffects,
      args: { items: builtinIterable, mapper: b.undefined.optional },
      returns: newArrayOf(b.iterated("items")).dense,
      uses: { this: b.inspected, items: [b.runs("Iterate")] },
      isSpecialCase: true,
      implementation: b.compiled("prelude/iterator-consumers", "fromIterable"),
    },
    {
      ...operandCodeEffects,
      args: { items: b.any, mapper: b.undefined.optional },
      returns: newArrayOf(b.union(b.iterated("items"), b.undefined)).dense,
      uses: { this: b.inspected, items: [itemsProtocols, b.contentsStoredIn(b.resultSlot("element"))] },
      implementation: b.compiled("ArrayConstructor", "from"),
    },
    {
      ...operandCodeEffects,
      args: { items: builtinIterable, mapper: b.callable, thisArg: b.any.optional },
      returns: newArrayOf(b.callbackResult("mapper")).dense,
      uses: {
        this: b.inspected,
        items: [b.runs("Iterate")],
        mapper: [mapperCall(b.iterated("items"))],
        thisArg: b.inspected,
      },
      isSpecialCase: true,
      implementation: b.compiled("prelude/iterator-consumers", "from"),
    },
    {
      ...operandCodeEffects,
      args: { items: b.any, mapper: b.callable, thisArg: b.any.optional },
      returns: newArrayOf(b.callbackResult("mapper")).dense,
      uses: {
        this: b.inspected,
        items: [itemsProtocols],
        mapper: [mapperCall(b.iterated("items")), mapperCall(b.ownValues("items"))],
        thisArg: b.inspected,
      },
      implementation: b.compiled("ArrayConstructor", "from"),
    },
    {
      args: { items: b.any, mapper: b.any, thisArg: b.any.optional },
      returns: b.never,
      capability: "heap",
      throws: true,
      uses: { this: b.inspected, items: b.inspected, mapper: b.inspected, thisArg: b.inspected },
      implementation: b.compiled("ArrayConstructor", "from"),
    },
    b.otherwiseBy(b.compiled("ArrayConstructor", "from")),
  ]),

  b.static("Array.fromAsync", [
    {
      ...operandCodeEffects,
      args: { asyncItems: b.any, mapper: b.any.optional, thisArg: b.any.optional },
      returns: b.instance("Promise").holding({ value: newArrayOf(b.any) }).fresh,
      uses: {
        this: b.inspected,
        asyncItems: [
          b.retained,
          b.runsTransitively(
            "Iterate",
            "OwnValues",
            "PrototypeChain",
            "ToPrimitive",
            "Thenable",
            b.calls("@@asyncIterator"),
            b.named("length"),
          ),
          b.reachableValuesStoredIn(b.nativeMemory),
        ],
        mapper: [
          b.retained,
          b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", this: b.operand("thisArg"), args: [b.any, b.length], result: "Escapes" }),
        ],
        thisArg: [b.retained],
      },
      requiresHolderAsThis: true,
      implementation: b.compiled("ArrayConstructor", "fromAsync"),
    },
    b.otherwiseBy(b.compiled("ArrayConstructor", "fromAsync")),
  ]),

  traversalMethod("every", arraySkippingHoles, booleanAnswer),
  traversalMethod("some", arraySkippingHoles, booleanAnswer),
  traversalMethod("forEach", arraySkippingHoles, { result: "Ignored" }),
  traversalMethod("find", arrayReadingHoles, foundElement, foundOwnValue),
  traversalMethod("findLast", arrayReadingHoles, foundElement, foundOwnValue),
  traversalMethod("findIndex", arrayReadingHoles, foundArrayIndex, foundIndex),
  traversalMethod("findLastIndex", arrayReadingHoles, foundArrayIndex, foundIndex),
  traversalMethod(
    "filter",
    arrayWithDefaultSpecies,
    { returns: newArrayOf(element).dense, result: "Tested" },
    speciesResult("Tested", b.contentsStoredIn(elementOfResult)),
  ),
  traversalMethod(
    "map",
    arrayWithDefaultSpecies,
    { returns: newArrayOf(callbackResult), result: "Stored", resultTo: b.resultSlot("element"), receiverUses: b.inspected },
    speciesResult("Stored"),
  ),
  traversalMethod(
    "flatMap",
    arrayWithDefaultSpecies,
    { returns: b.instance("Array").fresh.dense, result: "Flattened", resultTo: elementOfResult },
    speciesResult("Escapes"),
  ),
  b.privateAlias("Array.prototype.forEach", "Array.prototype.map"),

  foldMethod("reduce"),
  b.extend("Array.prototype.reduce", { callsFunctions: [b.functionIn("ArrayPrototype", "chainToValue")] }),
  b.extend("Array.prototype.filter", { callsFunctions: [b.functionIn("ArrayPrototype", "chainToList")] }),
  foldMethod("reduceRight"),

  sortMethod(
    "sort",
    { returns: self, receiverUses: [b.movesElements] },
    [
      { receiver: b.object, returns: self, receiverUses: movesOwnValues },
      { receiver: arrayLike.type, returns: wrapperOfReceiver, receiverUses: movesOwnValues },
    ],
    { length: 1, boxedCallStub: "ArraySort", nativeSymbols: ["arrayProtoFuncSort"] },
  ),
  sortMethod(
    "toSorted",
    { returns: newArrayOf(elementOrUndefined).dense, receiverUses: [b.readsContents()] },
    [{ receiver: arrayLike.type, returns: newArrayOf(b.any).dense, receiverUses: [b.contentsStoredIn(b.resultSlot("element"))] }],
    { usesFeature: "RareArrayMethods", nativeSymbols: ["arrayProtoFuncToSorted"] },
  ),
);
