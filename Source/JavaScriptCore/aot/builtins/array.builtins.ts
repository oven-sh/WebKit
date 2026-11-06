import * as b from "bun-builtins-table";

const elements = "Elements";
const storage = "ArrayStorage";

const anyArray = b.instance("Array");
const arrayWithPristineStorage = anyArray.pristine("Storage");
const pristineArray = anyArray.pristine("Storage", "Names");
const arrayLike = b.object.loose.refusing(b.null);

const self = b.operand("this");
const selfAsObject = b.union(self, b.object);
const element = b.slot("this", "element");
const elementOrUndefined = b.union(element, b.undefined);
const newArrayOf = (contents: b.TableType) => anyArray.holding({ element: contents }).fresh;
const newDenseArrayOf = (contents: b.TableType) => newArrayOf(contents).dense;
const objectFromSpecies = b.object.freshUnlessSpecies;
// ArraySpeciesCreate: a new array, or whatever object the species constructor of the receiver makes. The method defines the elements on that object and keeps them nowhere else.
const arrayFromSpecies = b.union(anyArray, b.object).freshUnlessSpecies;
// The array has a hole only where the operands that say so have one.
const denseArrayFromSpecies = b.union(anyArray.dense, b.object).freshUnlessSpecies;
const elementOfResult = b.resultSlot("element");

const maximumArrayLength = b.checked(2 ** 32 - 1, "UINT32_MAX");
const arrayLength = b.int(0, maximumArrayLength);
const foundArrayIndex = b.int(-1, maximumArrayLength);
const foundIndex = b.int(-1, Number.MAX_SAFE_INTEGER);
const relativeIndex = b.integer.loose;
const optionalRelativeIndex = relativeIndex.optional;

const nativeEffects = { capability: "heap", throws: true } as const;
const allocationEffects = { capability: "heap", throws: b.propagated } as const;
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;
const propertyStoreEffects = { ...operandCodeEffects, writes: ["MemberGuards"] } as const;

const readsArrayLike = [b.readsContents(), b.runs("OwnValues", "PrototypeChain", "ToPrimitive", b.named("length"))];
const storesLength = b.storedByKey(b.keysNamed("length"), b.length);
const movesOwnValues = [b.movesElements, b.storedByKey(b.indexKeys, b.ownValues("this")), b.deletedByKey(b.indexKeys)];
const convertsElements = b.runsOnContents("ToPrimitive");
const testsConcatSpreadable = b.runs(
  "OwnValues",
  "PrototypeChain",
  "ToPrimitive",
  b.named("@@isConcatSpreadable", "length"),
);

const rareMethod = (nativeSymbol: string) =>
  ({ usesFeature: "RareArrayMethods", nativeSymbols: [nativeSymbol] }) satisfies b.EntryColumns;

const copiedRange = { target: relativeIndex, start: relativeIndex, end: optionalRelativeIndex };
const filledRange = { value: b.any, start: optionalRelativeIndex, end: optionalRelativeIndex };
const slicedRange = { start: optionalRelativeIndex, end: optionalRelativeIndex };
const searchedRange = { searched: b.any, fromIndex: optionalRelativeIndex };
const separator = { separator: b.string.loose.optional };

const creation = [
  {
    args: { length: b.f64.admitting(arrayLength), second: b.notPassed },
    returns: newArrayOf(b.never),
    ...allocationEffects,
    implementation: b.inline("ArrayWithLength"),
  },
  {
    args: { items: b.any.rest },
    returns: newDenseArrayOf(b.operand("items")),
    ...allocationEffects,
    uses: { items: b.inspected },
    implementation: b.inline("ArrayOf"),
  },
] satisfies b.FunctionField[];

const iteratorOver = (kind: b.IterationKind) => {
  const created = {
    returns: b.instance("ArrayIterator").fresh,
    capability: "heap",
    uses: { this: [b.storedIn(b.resultSlot("iterated"))] },
    iterates: { operand: "this", kind },
  } as const;
  return [
    { ...created, this: anyArray, implementation: b.inline("ArrayIterator", { parameters: [kind] }) },
    { ...created, this: arrayLike, implementation: b.hostCall },
  ] satisfies b.FunctionField[];
};

const searchInArrayLike = (found: b.TableType) =>
  ({
    this: arrayLike,
    args: searchedRange,
    returns: found,
    ...operandCodeEffects,
    readsStateBeforeConversions: true,
    uses: { this: readsArrayLike, searched: b.inspected },
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const searchFromStart = (name: string, inlineBuiltin: string, nativeSymbol: string, foundInArray: b.TableType, found: b.TableType) =>
  [
    {
      this: arrayWithPristineStorage,
      args: { searched: b.f64, fromIndex: optionalRelativeIndex },
      returns: foundInArray,
      ...allocationEffects,
      readsStateBeforeConversions: true,
      uses: { this: [b.readsContents()] },
      isSpecialCase: true,
      length: 1,
      boxedCallStub: inlineBuiltin,
      nativeSymbols: [nativeSymbol],
      implementation: b.inline(inlineBuiltin, { max: 1, otherwise: b.hostCall }),
    },
    {
      this: arrayWithPristineStorage,
      args: searchedRange,
      returns: foundInArray,
      ...allocationEffects,
      readsStateBeforeConversions: true,
      uses: { this: [b.readsContents()], searched: b.inspected },
      implementation: b.inline(inlineBuiltin, { max: 2, otherwise: b.compiled("prelude/array-search", name, { stubConvention: true }) }),
    },
    searchInArrayLike(found),
  ] satisfies b.FunctionField[];

const spreadOperands = ["first", "second", "third"];

const concatenationOf = (spreadCount: number) => {
  const spread = spreadOperands.slice(0, spreadCount);
  return {
    this: pristineArray,
    args: { ...Object.fromEntries(spread.map(name => [name, pristineArray])), items: b.any.rest },
    returns: newDenseArrayOf(b.union(element, ...spread.map(name => b.slot(name, "element")), b.operand("items"))),
    ...operandCodeEffects,
    uses: {
      this: [b.readsContents(), b.copiesHolesToResult],
      ...Object.fromEntries(spread.map(name => [name, [b.readsContents(), b.copiesHolesToResult]])),
      items: [testsConcatSpreadable, b.readByKey(b.indexKeys, b.resultSlot("element")), b.copiesHolesToResult],
    },
    implementation: b.inline("ArrayConcat", { otherwise: b.hostCall }),
  } satisfies b.FunctionField;
};

export default b.declarations(
  b.class("Array", {
    jsType: "ArrayType",
    slots: { element: b.contents("Element", elements) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    types: [
      b.exactType("Array", "element"),
      b.viewType("ReadonlyArray", "element"),
      b.viewType("ArrayLike", "element"),
      b.viewType("ConcatArray", "element"),
      b.viewType("Iterable", "element"),
    ],
    constructorTable: b.staticTable("runtime/ArrayConstructor.cpp", "arrayConstructorTable"),
    call: creation,
    construct: creation,
    static: {
      "@@species": {
        this: b.any,
        getter: self,
        capability: "none",
        uses: { this: b.inspected },
        mutability: "AlwaysLocked",
        implementation: b.inline("ReturnThis"),
      },
      isArray: {
        args: { value: b.any },
        returns: b.bool,
        answersWhetherInstance: { value: b.instance("Array") },
        ...nativeEffects,
        uses: { value: b.inspected },
        length: 1,
        boxedCallStub: "ArrayIsArray",
        nativeSymbols: ["arrayConstructorIsArray"],
        implementation: b.inline("ArrayIsArray", { otherwise: b.hostCall }),
      },
      of: [
        {
          args: { items: b.any.rest },
          returns: newDenseArrayOf(b.operand("items")),
          ...allocationEffects,
          uses: { this: b.inspected, items: b.inspected },
          requiresHolderAsThis: true,
          implementation: b.inline("ArrayOf"),
        },
        b.otherwiseConstructs("this"),
      ],
    },
    instance: {
      length: {
        getter: arrayLength,
        capability: "none",
        uses: { this: b.inspected },
        reads: [storage],
        implementation: b.inline("ArrayLength"),
        setter: {
          value: b.f64.loose,
          ...nativeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.movesElements], value: [b.becomesLengthOfReceiver] },
          implementation: b.inline("ArraySetLength"),
        },
      },
    },
    proto: {
      at: [
        {
          this: arrayWithPristineStorage,
          args: { index: relativeIndex },
          returns: elementOrUndefined,
          ...allocationEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents()] },
          implementation: b.inline("ArrayAt", { otherwise: b.compiled("ArrayPrototype", "at") }),
        },
        {
          this: arrayLike,
          args: { index: relativeIndex },
          returns: b.any,
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [...readsArrayLike, b.contentsStoredIn(b.result)] },
          implementation: b.compiled("ArrayPrototype", "at"),
        },
      ],
      copyWithin: [
        {
          this: arrayWithPristineStorage,
          args: copiedRange,
          returns: self,
          ...nativeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.movesElements] },
          ...rareMethod("arrayProtoFuncCopyWithin"),
          implementation: b.hostCall,
        },
        {
          this: arrayLike,
          args: copiedRange,
          returns: selfAsObject,
          ...propertyStoreEffects,
          readsStateBeforeConversions: true,
          uses: { this: [...readsArrayLike, ...movesOwnValues] },
          implementation: b.hostCall,
        },
      ],
      entries: iteratorOver("Entries"),
      keys: iteratorOver("Keys"),
      values: iteratorOver("Values"),
      fill: [
        {
          this: arrayWithPristineStorage,
          args: filledRange,
          returns: self,
          ...nativeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents(), b.writesContents()], value: [b.storedIn(element)] },
          implementation: b.hostCall,
        },
        {
          this: arrayLike,
          args: filledRange,
          returns: selfAsObject,
          ...propertyStoreEffects,
          readsStateBeforeConversions: true,
          uses: { this: [...readsArrayLike, b.storedByKey(b.indexKeys, b.operand("value"))], value: b.inspected },
          implementation: b.hostCall,
        },
      ],
      includes: searchFromStart("includes", "ArrayIncludes", "arrayProtoFuncIncludes", b.bool, b.bool),
      indexOf: searchFromStart("indexOf", "ArrayIndexOf", "arrayProtoFuncIndexOf", foundArrayIndex, foundIndex),
      lastIndexOf: [
        {
          this: arrayWithPristineStorage,
          args: searchedRange,
          returns: foundArrayIndex,
          ...allocationEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents()], searched: b.inspected },
          ...rareMethod("arrayProtoFuncLastIndexOf"),
          implementation: b.hostCall,
        },
        searchInArrayLike(foundIndex),
      ],
      join: [
        {
          this: arrayWithPristineStorage,
          args: separator,
          returns: b.string,
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents(), convertsElements] },
          implementation: b.inline("ArrayJoin", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          args: separator,
          returns: b.string,
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [...readsArrayLike, convertsElements] },
          implementation: b.hostCall,
        },
      ],
      toLocaleString: {
        this: arrayLike,
        args: { locales: b.any.optional, options: b.any.optional },
        returns: b.string,
        ...operandCodeEffects,
        uses: {
          this: [...readsArrayLike, b.runsOnContents("ToPrimitive", b.calls("toLocaleString"))],
          locales: [b.readAsLocale, b.retained],
          options: [b.retained],
        },
        usesFeature: ["ElementsToLocaleString", "IntlNumberFormat", "IntlUnits", "IntlDateTimeFormat"],
        nativeSymbols: ["arrayProtoFuncToLocaleString"],
        implementation: b.hostCall,
      },
      pop: [
        {
          this: arrayWithPristineStorage,
          returns: elementOrUndefined,
          ...nativeEffects,
          uses: { this: [b.movesElements] },
          implementation: b.inline("ArrayPop", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          returns: b.any,
          ...propertyStoreEffects,
          uses: {
            this: [
              ...readsArrayLike,
              b.contentsStoredIn(b.result),
              b.movesElements,
              b.deletedByKey(b.indexKeys),
              storesLength,
            ],
          },
          implementation: b.hostCall,
        },
      ],
      shift: [
        {
          this: arrayWithPristineStorage,
          returns: elementOrUndefined,
          ...nativeEffects,
          uses: { this: [b.movesElements] },
          implementation: b.inline("ArrayShift", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          returns: b.any,
          ...propertyStoreEffects,
          uses: { this: [...readsArrayLike, b.contentsStoredIn(b.result), ...movesOwnValues, storesLength] },
          implementation: b.hostCall,
        },
      ],
      push: [
        {
          this: arrayWithPristineStorage,
          args: { items: b.any.rest },
          returns: arrayLength,
          ...nativeEffects,
          uses: { this: [b.readsContents(), b.writesContents()], items: [b.storedIn(element)] },
          length: 1,
          boxedCallStub: "ArrayPush",
          nativeSymbols: ["arrayProtoFuncPush"],
          implementation: b.inline("ArrayPush", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          args: { items: b.any.rest },
          returns: b.length,
          ...propertyStoreEffects,
          uses: {
            this: [...readsArrayLike, b.storedByKey(b.indexKeys, b.operand("items")), storesLength],
            items: b.inspected,
          },
          implementation: b.hostCall,
        },
      ],
      unshift: [
        {
          this: arrayWithPristineStorage,
          args: { items: b.any.rest },
          returns: arrayLength,
          ...nativeEffects,
          uses: { this: [b.movesElements], items: [b.storedIn(element)] },
          implementation: b.inline("ArrayUnshift", { max: 1, otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          args: { items: b.any.rest },
          returns: b.length,
          ...propertyStoreEffects,
          uses: {
            this: [...readsArrayLike, ...movesOwnValues, b.storedByKey(b.indexKeys, b.operand("items")), storesLength],
            items: b.inspected,
          },
          implementation: b.hostCall,
        },
      ],
      reverse: [
        {
          this: arrayWithPristineStorage,
          returns: self,
          ...nativeEffects,
          uses: { this: [b.movesElements] },
          implementation: b.hostCall,
        },
        {
          this: arrayLike,
          returns: selfAsObject,
          ...propertyStoreEffects,
          uses: { this: [...readsArrayLike, ...movesOwnValues] },
          implementation: b.hostCall,
        },
      ],
      toReversed: [
        {
          this: arrayWithPristineStorage,
          returns: newDenseArrayOf(elementOrUndefined),
          ...allocationEffects,
          uses: { this: [b.readsContents()] },
          ...rareMethod("arrayProtoFuncToReversed"),
          implementation: b.inline("ArrayToReversed", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          returns: newDenseArrayOf(b.any),
          ...operandCodeEffects,
          uses: { this: [...readsArrayLike, b.contentsStoredIn(b.resultSlot("element"))] },
          implementation: b.hostCall,
        },
      ],
      toSpliced: [
        {
          this: arrayWithPristineStorage,
          args: { start: optionalRelativeIndex, skipCount: optionalRelativeIndex, items: b.any.rest },
          returns: newDenseArrayOf(b.union(elementOrUndefined, b.operand("items"))),
          ...nativeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents()], items: b.inspected },
          ...rareMethod("arrayProtoFuncToSpliced"),
          implementation: b.inline("ArrayToSpliced", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          args: { start: optionalRelativeIndex, skipCount: optionalRelativeIndex, items: b.any.rest },
          returns: newDenseArrayOf(b.any),
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: {
            this: [...readsArrayLike, b.contentsStoredIn(b.resultSlot("element"))],
            items: [b.storedIn(b.resultSlot("element"))],
          },
          implementation: b.hostCall,
        },
      ],
      with: [
        {
          this: arrayWithPristineStorage,
          args: { index: relativeIndex, value: b.any },
          returns: newDenseArrayOf(b.union(elementOrUndefined, b.operand("value"))),
          ...nativeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents()], value: b.inspected },
          ...rareMethod("arrayProtoFuncWith"),
          implementation: b.inline("ArrayWith", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          args: { index: relativeIndex, value: b.any },
          returns: newDenseArrayOf(b.any),
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: {
            this: [...readsArrayLike, b.contentsStoredIn(b.resultSlot("element"))],
            value: [b.storedIn(b.resultSlot("element"))],
          },
          implementation: b.hostCall,
        },
      ],
      concat: [
        concatenationOf(3),
        concatenationOf(2),
        concatenationOf(1),
        concatenationOf(0),
        {
          this: arrayLike,
          args: { items: b.any.rest },
          returns: denseArrayFromSpecies,
          ...operandCodeEffects,
          uses: {
            this: [b.runs("Species"), testsConcatSpreadable, b.storedIn(elementOfResult), b.contentsStoredIn(elementOfResult), b.copiesHolesToResult],
            items: [testsConcatSpreadable, b.storedIn(elementOfResult), b.contentsStoredIn(elementOfResult), b.copiesHolesToResult],
          },
          implementation: b.hostCall,
        },
      ],
      flat: [
        {
          this: pristineArray,
          args: { depth: optionalRelativeIndex },
          returns: newDenseArrayOf(b.any),
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [...readsArrayLike, b.reachableValuesStoredIn(b.nativeMemory)] },
          ...rareMethod("arrayProtoFuncFlat"),
          model: b.functionIn("models/Arrays", "flat"),
          implementation: b.hostCall,
        },
        {
          this: arrayLike,
          args: { depth: optionalRelativeIndex },
          returns: objectFromSpecies,
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.runs("Species"), ...readsArrayLike, b.reachableValuesStoredIn(b.nativeMemory)] },
          implementation: b.hostCall,
        },
      ],
      slice: [
        {
          this: pristineArray,
          args: slicedRange,
          returns: newDenseArrayOf(element),
          ...allocationEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.readsContents(), b.copiesHolesToResult] },
          length: 2,
          boxedCallStub: "ArraySlice",
          nativeSymbols: ["arrayProtoFuncSlice"],
          implementation: b.inline("ArraySlice", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          args: slicedRange,
          returns: denseArrayFromSpecies,
          ...operandCodeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.runs("Species"), ...readsArrayLike, b.contentsStoredIn(elementOfResult), b.copiesHolesToResult] },
          implementation: b.hostCall,
        },
      ],
      splice: [
        {
          this: pristineArray,
          args: { start: optionalRelativeIndex, deleteCount: optionalRelativeIndex, items: b.any.rest },
          returns: newDenseArrayOf(element),
          ...nativeEffects,
          readsStateBeforeConversions: true,
          uses: { this: [b.movesElements, b.copiesHolesToResult], items: [b.storedIn(element)] },
          implementation: b.inline("ArraySplice", { otherwise: b.hostCall }),
          prefixForm: "ArraySplicePrefix",
        },
        {
          this: arrayLike,
          args: { start: optionalRelativeIndex, deleteCount: optionalRelativeIndex, items: b.any.rest },
          returns: denseArrayFromSpecies,
          ...propertyStoreEffects,
          readsStateBeforeConversions: true,
          uses: {
            this: [
              b.runs("Species"),
              ...readsArrayLike,
              b.contentsStoredIn(elementOfResult),
              b.copiesHolesToResult,
              ...movesOwnValues,
              b.storedByKey(b.indexKeys, b.operand("items")),
              storesLength,
            ],
            items: b.inspected,
          },
          implementation: b.hostCall,
        },
      ],
      toString: [
        {
          this: pristineArray,
          returns: b.string,
          ...operandCodeEffects,
          uses: { this: [b.readsContents(), convertsElements] },
          implementation: b.inline("ArrayJoin", { otherwise: b.hostCall }),
        },
        {
          this: arrayLike,
          returns: b.any,
          ...operandCodeEffects,
          uses: { this: [...readsArrayLike, convertsElements, b.runs(b.calls("join"), b.named("@@toStringTag")), b.returned] },
          implementation: b.hostCall,
        },
      ],
    },
  }),
  // Array.prototype.toString calls what it finds under `join` without operands.
  b.namedMemberCall("join", []),
  // ArrayPrototype.cpp finishCreation: an object without a prototype that holds `true` under the name of each method that `with` does not see.
  b.static("Array.prototype.@@unscopables", b.property(b.any)),
  b.same("Array.prototype.@@iterator", "Array.prototype.values"),
  b.privateAlias(
    "Array.prototype.entries",
    "Array.prototype.includes",
    "Array.prototype.indexOf",
    "Array.prototype.keys",
    "Array.prototype.pop",
    "Array.prototype.shift",
    "Array.prototype.values",
  ),

  b.class("%Arguments%", {
    hasConstructor: false,
    hasPrototype: false,
    slots: { element: b.contents("Element", elements) },
  }),
);
