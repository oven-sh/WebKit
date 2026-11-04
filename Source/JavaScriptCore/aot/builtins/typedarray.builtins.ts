import * as b from "bun-builtins-table";

const elements = "TypedArrayElements";
const storage = "TypedArrayStorage";

const maximumBufferLength = b.checked(2 ** 32, "MAX_ARRAY_BUFFER_SIZE");
const bufferLength = b.int(0, maximumBufferLength);
// An element takes eight bytes at most.
const lengthOfAnyElementSize = b.int(0, 2 ** 29);
const indexOrNotFound = b.int(-1, maximumBufferLength);
const element = b.union(b.f64, b.bigint);
const elementOrUndefined = b.union(element, b.undefined);
const self = b.operand("this");
const typedArray = b.instance("TypedArray");
const typedArrayWithDefaultSpecies = typedArray.pristine("Names");
const anyArrayBuffer = b.union(b.instance("ArrayBuffer"), b.instance("SharedArrayBuffer"));
const relativeIndex = b.integer.loose;
const range = { start: relativeIndex.optional, end: relativeIndex.optional };

const readsElements = b.readsContents();
const changesElements = [readsElements, b.writesContents("element")];
const convertedToElement = b.runs("ToPrimitive");
const arrayLikeProtocols = ["OwnValues", "PrototypeChain", "ToPrimitive", b.named("length")] as const;
const convertedArrayLike = b.runsTransitively(...arrayLikeProtocols);
const convertedItems = b.runsTransitively("Iterate", ...arrayLikeProtocols);
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;

const requiresNew = {
  returns: b.never,
  capability: "none",
  throws: true,
  implementation: b.hostCall,
} satisfies b.FunctionField;

const construction = [
  {
    args: { source: typedArray },
    returns: b.sameClass.fresh,
    capability: "heap",
    throws: true,
    uses: { source: [readsElements] },
    implementation: b.hostCall,
  },
  {
    args: { buffer: anyArrayBuffer, byteOffset: b.index.loose.optional, length: b.index.loose.optional },
    returns: b.sameClass.fresh,
    capability: "heap",
    throws: true,
    readsStateBeforeConversions: true,
    uses: { buffer: [b.retained] },
    reads: [storage],
    implementation: b.hostCall,
  },
  {
    ...operandCodeEffects,
    args: { source: b.object },
    returns: b.sameClass.fresh,
    uses: { source: [convertedItems] },
    implementation: b.hostCall,
  },
  {
    args: { length: b.integer.loose.optional.admitting(b.union(lengthOfAnyElementSize, b.undefined)) },
    returns: b.sameClass.fresh,
    capability: "heap",
    throws: b.propagated,
    implementation: b.inline("TypedArrayWithLength", { otherwise: b.hostCall }),
  },
] satisfies b.FunctionField[];

const constructedByReceiver = b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", constructs: true, args: [b.length], result: "Returned" });

const mapperCall = (item: b.PlaceLike) =>
  b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "PerElement", this: b.operand("thisArg"), args: [item, b.length], result: "Escapes" });

const storageGetter = (inlineBuiltin: string) =>
  ({
    getter: bufferLength,
    capability: "none",
    uses: { this: b.inspected },
    reads: [storage],
    implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
  }) satisfies b.GetterField;

const iteratorOver = (kind: b.IterationKind) =>
  ({
    returns: b.instance("ArrayIterator").fresh,
    capability: "heap",
    throws: true,
    uses: { this: [b.storedIn(b.resultSlot("iterated"))] },
    reads: [storage],
    iterates: { operand: "this", kind },
    implementation: b.inline("ArrayIterator", { parameters: [kind], otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const search = (returns: b.TableType) =>
  ({
    args: { searchElement: b.any, fromIndex: relativeIndex.optional },
    returns,
    capability: "heap",
    throws: true,
    readsStateBeforeConversions: true,
    uses: { this: [readsElements], searchElement: b.inspected },
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const convertedByNativeCode: b.CallbackResultUse = "Escapes";

const traversal = (returns: b.TableType, result: b.CallbackResultUse, ...receiverUses: readonly b.UseSpec[]) =>
  ({
    args: { callback: b.callable, thisArg: b.any.optional },
    returns,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: {
      this: [readsElements, ...receiverUses],
      callback: [
        b.called({
          afterwards: "TouchesOnlyWhatRowNames",
          times: "PerElement",
          this: b.operand("thisArg"),
          args: [elementOrUndefined, bufferLength, self],
          result,
        }),
      ],
      thisArg: b.inspected,
    },
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const compiledTraversal = (name: string, returns: b.TableType, result: b.CallbackResultUse) =>
  ({ ...traversal(returns, result), implementation: b.compiled("TypedArrayPrototype", name) }) satisfies b.FunctionField;

type SpeciesLookup = "BeforeCallbacks" | "AfterCallbacks";

const copyingTraversal = (result: b.CallbackResultUse, speciesLookup: SpeciesLookup): b.FunctionField[] => {
  const intoSpeciesResult = {
    ...traversal(typedArray.freshUnlessSpecies, result, b.runs("Species")),
    writes: [elements],
  };
  if (speciesLookup === "AfterCallbacks") return [intoSpeciesResult];
  return [{ ...traversal(b.sameClass.fresh, result), this: typedArrayWithDefaultSpecies }, intoSpeciesResult];
};

const foldCall = (times: b.CallbackTimes, accumulator: b.PlaceLike) =>
  b.called({ afterwards: "TouchesOnlyWhatRowNames", times, this: b.undefined, args: [accumulator, elementOrUndefined, bufferLength, self], result: "Returned" });

const fold = (name: string) => [
  {
    ...operandCodeEffects,
    args: { callback: b.callable, initialValue: b.notPassed },
    returns: b.union(element, b.callbackResult("callback")),
    uses: { this: [readsElements], callback: [foldCall("Once", element), foldCall("PerElement", b.result)] },
    implementation: b.compiled("TypedArrayPrototype", name),
  },
  {
    ...operandCodeEffects,
    args: { callback: b.callable, initialValue: b.any },
    returns: b.union(b.operand("initialValue"), b.callbackResult("callback")),
    uses: {
      this: [readsElements],
      callback: [foldCall("Once", b.operand("initialValue")), foldCall("PerElement", b.result)],
      initialValue: b.inspected,
    },
    implementation: b.compiled("TypedArrayPrototype", name),
  },
] satisfies b.FunctionField[];

const sorting = (nativeSymbol: string, returns: b.TableType, receiverUses: readonly b.UseSpec[]) =>
  [
    {
      args: { comparator: b.undefined.optional },
      returns,
      capability: "heap",
      throws: true,
      uses: { this: receiverUses },
      usesFeature: "TypedArraySort",
      nativeSymbols: [nativeSymbol],
      implementation: b.hostCall,
    },
    {
      args: { comparator: b.callable },
      returns,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: {
        this: receiverUses,
        comparator: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Repeatedly", this: b.undefined, args: [element, element], result: convertedByNativeCode })],
      },
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const copyOfRange = {
  args: range,
  throws: true,
  readsStateBeforeConversions: true,
  implementation: b.hostCall,
} as const;

const viewOfRange = { ...copyOfRange, reads: [storage], writes: [storage] } as const;

const removableTypedArrayClass = (elementName: string, bytesPerElement: number) => [
  ...b.typedArrayClass(elementName, bytesPerElement),
  ...b.extend(`${elementName}Array`, {
    usesFeature: `${elementName}Array`,
    nativeSymbols: [`call${elementName}Array`, `construct${elementName}Array`],
  }),
];

const decodingOptions = b.dictionary(["alphabet", "lastChunkHandling"]).optional;
const encodingOptions = b.dictionary(["alphabet", "omitPadding"]).optional;

const textCodec = (nativeSymbol: string) =>
  ({
    capability: "heap",
    throws: true,
    usesFeature: "Uint8ArrayBase64",
    nativeSymbols: [nativeSymbol],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

export default b.declarations(
  b.class("%TypedArray%", {
    isNeverPristine: true,
    slots: { element: b.contents("Element", elements, element) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    types: [b.viewType("ArrayBufferView")],
    call: requiresNew,
    construct: construction,
    static: {
      "@@species": {
        this: b.any,
        getter: self,
        capability: "none",
        uses: { this: b.inspected },
        implementation: b.inline("ReturnThis"),
      },
      from: {
        this: b.callable,
        args: { items: b.any, mapper: b.callable.optional, thisArg: b.any.optional },
        returns: b.callbackResult("this"),
        ...operandCodeEffects,
        uses: {
          this: [constructedByReceiver],
          items: [convertedItems],
          mapper: [mapperCall(b.iterated("items")), mapperCall(b.ownValues("items"))],
          thisArg: b.inspected,
        },
        implementation: b.compiled("TypedArrayConstructor", "from"),
      },
      of: {
        ...operandCodeEffects,
        this: b.callable,
        args: { items: b.any.rest },
        returns: b.callbackResult("this"),
        uses: { this: [constructedByReceiver], items: [convertedToElement] },
        writes: [elements],
        implementation: b.hostCall,
      },
    },
    proto: {
      buffer: {
        getter: anyArrayBuffer,
        capability: "heap",
        throws: true,
        uses: { this: [b.retained] },
        reads: [storage],
        writes: [storage],
        implementation: b.hostCall,
      },
      byteLength: storageGetter("TypedArrayByteLength"),
      byteOffset: storageGetter("TypedArrayByteOffset"),
      length: {
        ...storageGetter("TypedArrayLength"),
        length: 0,
        boxedCallStub: "TypedArrayLength",
        nativeSymbols: ["typedArrayViewProtoGetterFuncLength"],
      },
      "@@toStringTag": {
        this: b.any,
        getter: b.union(b.string, b.undefined),
        capability: "heap",
        uses: { this: b.inspected },
        implementation: b.hostCall,
      },
      at: {
        args: { index: relativeIndex },
        returns: elementOrUndefined,
        capability: "heap",
        throws: true,
        readsStateBeforeConversions: true,
        uses: { this: [readsElements] },
        implementation: b.compiled("TypedArrayPrototype", "at"),
      },
      copyWithin: {
        args: { target: relativeIndex, start: relativeIndex, end: relativeIndex.optional },
        returns: self,
        capability: "heap",
        throws: true,
        readsStateBeforeConversions: true,
        uses: { this: changesElements },
        implementation: b.hostCall,
      },
      entries: iteratorOver("Entries"),
      keys: iteratorOver("Keys"),
      values: iteratorOver("Values"),
      every: compiledTraversal("every", b.bool, "Tested"),
      some: compiledTraversal("some", b.bool, "Tested"),
      fill: {
        ...operandCodeEffects,
        args: { value: b.any, ...range },
        returns: self,
        readsStateBeforeConversions: true,
        uses: { this: changesElements, value: [convertedToElement] },
        implementation: b.hostCall,
      },
      filter: copyingTraversal("Tested", "AfterCallbacks"),
      find: compiledTraversal("find", elementOrUndefined, "Tested"),
      findIndex: compiledTraversal("findIndex", indexOrNotFound, "Tested"),
      findLast: compiledTraversal("findLast", elementOrUndefined, "Tested"),
      findLastIndex: compiledTraversal("findLastIndex", indexOrNotFound, "Tested"),
      forEach: compiledTraversal("forEach", b.undefined, "Ignored"),
      includes: search(b.bool),
      indexOf: search(indexOrNotFound),
      lastIndexOf: search(indexOrNotFound),
      join: {
        args: { separator: b.string.loose.optional },
        returns: b.string,
        capability: "heap",
        throws: true,
        readsStateBeforeConversions: true,
        uses: { this: [readsElements], separator: [b.readAsCharacters] },
        implementation: b.hostCall,
      },
      map: copyingTraversal(convertedByNativeCode, "BeforeCallbacks"),
      reduce: fold("reduce"),
      reduceRight: fold("reduceRight"),
      reverse: {
        returns: self,
        capability: "heap",
        throws: true,
        uses: { this: changesElements },
        implementation: b.hostCall,
      },
      set: [
        {
          args: { source: typedArray, offset: relativeIndex.optional },
          capability: "heap",
          throws: true,
          readsStateBeforeConversions: true,
          uses: { this: changesElements, source: [readsElements] },
          implementation: b.inline("TypedArraySet", { otherwise: b.hostCall }),
        },
        {
          ...operandCodeEffects,
          args: { source: b.any, offset: relativeIndex.optional },
          readsStateBeforeConversions: true,
          uses: { this: changesElements, source: [convertedArrayLike] },
          implementation: b.hostCall,
        },
      ],
      slice: [
        {
          ...copyOfRange,
          this: typedArrayWithDefaultSpecies,
          returns: b.sameClass.fresh,
          capability: "heap",
          uses: { this: [readsElements] },
        },
        {
          ...copyOfRange,
          ...operandCodeEffects,
          returns: typedArray.freshUnlessSpecies,
          uses: { this: [readsElements, b.runs("Species")] },
          writes: [elements],
        },
      ],
      sort: sorting("typedArrayViewProtoFuncSort", self, changesElements),
      subarray: [
        {
          ...viewOfRange,
          this: typedArrayWithDefaultSpecies,
          returns: b.sameClass.fresh,
          rangeOf: "this",
          capability: "heap",
          uses: { this: [b.retained] },
        },
        {
          ...viewOfRange,
          ...operandCodeEffects,
          returns: typedArray.freshUnlessSpecies,
          uses: { this: [b.retained, b.runs("Species")] },
        },
      ],
      toLocaleString: {
        args: { locales: b.any.optional, options: b.any.optional },
        returns: b.string,
        capability: "js",
        throws: true,
        uses: { this: [readsElements], locales: [b.readAsLocale, b.retained], options: [b.retained] },
        usesFeature: ["ElementsToLocaleString", "IntlNumberFormat", "IntlUnits", "IntlDateTimeFormat"],
        implementation: b.compiled("TypedArrayPrototype", "toLocaleString"),
      },
      toReversed: {
        returns: b.sameElementType.fresh,
        capability: "heap",
        throws: true,
        uses: { this: [readsElements] },
        implementation: b.hostCall,
      },
      toSorted: sorting("typedArrayViewProtoFuncToSorted", b.sameElementType.fresh, [readsElements]),
      with: {
        ...operandCodeEffects,
        args: { index: relativeIndex, value: b.any },
        returns: b.sameElementType.fresh,
        readsStateBeforeConversions: true,
        uses: { this: [readsElements], value: [convertedToElement] },
        implementation: b.hostCall,
      },
    },
  }),
  b.same("%TypedArray%.prototype.@@iterator", "%TypedArray%.prototype.values"),
  b.same("%TypedArray%.prototype.toString", "Array.prototype.toString"),

  removableTypedArrayClass("Int8", 1),
  b.typedArrayClass("Uint8", 1),
  removableTypedArrayClass("Uint8Clamped", 1),
  removableTypedArrayClass("Int16", 2),
  removableTypedArrayClass("Uint16", 2),
  removableTypedArrayClass("Int32", 4),
  removableTypedArrayClass("Uint32", 4),
  removableTypedArrayClass("Float16", 2),
  removableTypedArrayClass("Float32", 4),
  removableTypedArrayClass("Float64", 8),
  removableTypedArrayClass("BigInt64", 8),
  removableTypedArrayClass("BigUint64", 8),

  b.static("Uint8Array.fromBase64", {
    ...textCodec("uint8ArrayConstructorFromBase64"),
    args: { string: b.string, options: decodingOptions },
    returns: b.instance("Uint8Array").exactly.fresh,
    uses: { string: [b.readAsCharacters], options: b.inspected },
  }),
  b.static("Uint8Array.fromHex", {
    ...textCodec("uint8ArrayConstructorFromHex"),
    args: { string: b.string },
    returns: b.instance("Uint8Array").exactly.fresh,
    uses: { string: [b.readAsCharacters] },
  }),
  b.method("Uint8Array.prototype.setFromBase64", {
    ...textCodec("uint8ArrayPrototypeSetFromBase64"),
    args: { string: b.string, options: decodingOptions },
    returns: b.ordinaryObjectOf("ReadAndWrittenCounts").fresh,
    uses: { this: changesElements, string: [b.readAsCharacters], options: b.inspected },
  }),
  b.method("Uint8Array.prototype.setFromHex", {
    ...textCodec("uint8ArrayPrototypeSetFromHex"),
    args: { string: b.string },
    returns: b.ordinaryObjectOf("ReadAndWrittenCounts").fresh,
    uses: { this: changesElements, string: [b.readAsCharacters] },
  }),
  b.method("Uint8Array.prototype.toBase64", {
    ...textCodec("uint8ArrayPrototypeToBase64"),
    args: { options: encodingOptions },
    returns: b.string,
    uses: { this: [readsElements], options: b.inspected },
  }),
  b.method("Uint8Array.prototype.toHex", {
    ...textCodec("uint8ArrayPrototypeToHex"),
    returns: b.string,
    uses: { this: [readsElements] },
  }),

  // `core-js/actual` replaces them without a test, from 3.35 to 3.37.
  b.replacedByPackages(
    "Uint8Array.fromBase64",
    "Uint8Array.fromHex",
    "Uint8Array.prototype.toBase64",
    "Uint8Array.prototype.toHex",
  ),
);
