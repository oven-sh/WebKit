import * as b from "bun-builtins-table";

const self = b.operand("this");
const element = b.slot("this", "element");
const elementOrUndefined = b.union(element, b.undefined);
const callbackResult = b.callbackResult("callback");
const newArrayOf = (elements: b.TableType) => b.instance("Array").holding({ element: elements }).fresh;
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;

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
    implementation: b.compiled("ArrayPrototype", name),
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

// ArraySpeciesCreate: a new array, or whatever object the species constructor of the receiver makes. The method defines the elements on that object and keeps them nowhere else.
// filter fills the array index by index. map leaves a hole where the receiver has one, and says so.
const arrayFromSpecies = b.union(b.instance("Array").dense, b.object).freshUnlessSpecies;
const elementOfResult = b.resultSlot("element");

const speciesResult = (result: b.CallbackResultUse, ...receiverUses: readonly b.UseSpec[]): Outcome => ({
  returns: result === "Escapes" ? b.object : arrayFromSpecies,
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
    implementation: b.compiled("ArrayPrototype", name),
  }) satisfies b.FunctionField;

const foldMethod = (name: string) => b.method(`Array.prototype.${name}`, folds.map(fold => foldOverload(name, fold)));

const comparatorCall = (compared: b.PlaceLike) =>
  b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Repeatedly", this: b.undefined, args: [compared, compared], result: "Escapes" });
const stringComparison = b.runsOnContents("ToPrimitive");

interface SortOutcome {
  returns: b.TableType;
  receiverUses: readonly b.UseSpec[];
}

const sortMethod = (name: string, onArray: SortOutcome, onArrayLike: SortOutcome, columns: b.EntryColumns = {}) =>
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
    {
      ...operandCodeEffects,
      this: arrayLike.type,
      args: { comparator: b.callable.optional },
      returns: onArrayLike.returns,
      uses: {
        this: [...arrayLike.uses, stringComparison, ...onArrayLike.receiverUses],
        comparator: [comparatorCall(arrayLike.element)],
      },
      implementation: b.hostCall,
    },
  ]);

const itemsProtocols = b.runs("Iterate", "OwnValues", "PrototypeChain", "ToPrimitive", b.named("length"));
const builtinIterable = b.union(
  b.string,
  ...["Set", "Map", "SetIterator", "MapIterator", "StringIterator"].map(name => b.instance(name).pristine("Names")),
);

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
      // The groups hold the items, and nothing describes the object that holds the groups.
      items: [b.runs("Iterate"), b.iteratedValuesStoredIn(b.nativeMemory)],
      callback: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "PerElement", this: b.undefined, args: [b.iterated("items"), b.length], result: "Escapes" })],
    },
    model: b.functionIn("models/Objects", "groupBy"),
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
      implementation: b.inline("ArrayFrom", { otherwise: b.compiled("ArrayConstructor", "from") }),
    },
    // Only the array-like path of the next overload can read a missing index.
    {
      ...operandCodeEffects,
      args: { items: builtinIterable, mapper: b.undefined.optional },
      returns: newArrayOf(b.iterated("items")).dense,
      uses: { this: b.inspected, items: [b.runs("Iterate")] },
      isSpecialCase: true,
      implementation: b.compiled("ArrayConstructor", "from"),
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
    { returns: newArrayOf(callbackResult).dense, result: "Stored", resultTo: b.resultSlot("element"), receiverUses: [b.copiesHolesToResult] },
    speciesResult("Stored", b.copiesHolesToResult),
  ),
  traversalMethod(
    "flatMap",
    arrayWithDefaultSpecies,
    { returns: newArrayOf(b.any).dense, result: "Escapes" },
    speciesResult("Escapes"),
  ),
  b.privateAlias("Array.prototype.forEach", "Array.prototype.map"),

  foldMethod("reduce"),
  foldMethod("reduceRight"),

  sortMethod(
    "sort",
    { returns: self, receiverUses: [b.movesElements] },
    {
      returns: b.union(self, b.object),
      receiverUses: [b.movesElements, b.storedByKey(b.indexKeys, b.ownValues("this")), b.deletedByKey(b.indexKeys)],
    },
    { length: 1, boxedCallStub: "ArraySort", nativeSymbols: ["arrayProtoFuncSort"] },
  ),
  sortMethod(
    "toSorted",
    { returns: newArrayOf(elementOrUndefined).dense, receiverUses: [b.readsContents()] },
    { returns: newArrayOf(b.any).dense, receiverUses: [b.contentsStoredIn(b.resultSlot("element"))] },
    { usesFeature: "RareArrayMethods", nativeSymbols: ["arrayProtoFuncToSorted"] },
  ),
);
