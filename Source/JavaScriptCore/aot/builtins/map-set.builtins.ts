import * as b from "bun-builtins-table";

const entries = "CollectionEntries";
const storage = "CollectionStorage";
const size = b.int(0, 2 ** 31 - 1);
const self = b.operand("this");
const element = b.slot("this", "element");
const key = b.slot("this", "key");
const value = b.slot("this", "value");

// Each value that the operand yields is an object whose elements 0 and 1 become a key and its value.
const storedAsEntries = [
  b.runsTransitively("Iterate", "OwnValues", "PrototypeChain"),
  b.reachableValuesStoredIn(b.resultSlot("key")),
  b.reachableValuesStoredIn(b.resultSlot("value")),
];

// A Map refuses an entry that is no object, and a WeakSet a value that is none.
const arrayOfObjects = b.instance("Array").holding({ element: b.object });

const requiresNew = {
  returns: b.never,
  capability: "none",
  throws: true,
  implementation: b.hostCall,
} satisfies b.FunctionField;

const iteratorOver = (iteratorClass: string, inlineBuiltin: string, kind: "Keys" | "Values" | "Entries") =>
  ({
    returns: b.instance(iteratorClass).fresh,
    capability: "heap",
    uses: { this: [b.storedIn(b.resultSlot("iterated"))] },
    reads: [storage],
    writes: [storage],
    iterates: { operand: "this", kind },
    implementation: b.inline(inlineBuiltin, { parameters: [kind] }),
  }) satisfies b.FunctionField;

const lookup = (inlineBuiltin: string, otherwise?: typeof b.hostCall) =>
  ({
    args: { key: b.any },
    returns: b.bool,
    capability: "heap",
    uses: { this: [b.readsContents()], key: b.inspected },
    implementation: b.inline(inlineBuiltin, { otherwise }),
  }) satisfies b.FunctionField;

const removal = (implementation: b.ImplementationSpec) =>
  ({
    args: { key: b.any },
    returns: b.bool,
    capability: "heap",
    uses: { this: [b.readsContents(), b.writesContents()], key: b.inspected },
    implementation,
  }) satisfies b.FunctionField;

const clearing = (inlineBuiltin: string) =>
  ({
    capability: "heap",
    throws: b.propagated,
    uses: { this: [b.writesContents()] },
    implementation: b.inline(inlineBuiltin),
  }) satisfies b.FunctionField;

const setMethod = (nativeSymbol: string, withPristineSet: b.TableType, withSetLike: b.TableType) =>
  [
    {
      mutability: "ConstantUnlessWritten",
      args: { other: b.instance("Set").pristine("Names") },
      returns: withPristineSet,
      capability: "heap",
      throws: b.propagated,
      uses: { this: [b.readsContents()], other: [b.readsContents()] },
      usesFeature: "SetMethods",
      nativeSymbols: [nativeSymbol],
      implementation: b.hostCall,
    },
    // SetPrototype.cpp getSetSizeAsInt: what is no object, or is not passed, makes each of them throw.
    {
      args: { other: b.primitiveValue },
      returns: b.never,
      capability: "heap",
      throws: true,
      uses: { this: b.inspected },
      implementation: b.hostCall,
    },
    {
      args: { other: b.object },
      returns: withSetLike,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { this: [b.everything], other: [b.everything] },
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const elementOfEitherSet = b.union(element, b.slot("other", "element"));
const newSetOfAnything = b.instance("Set").holding({ element: b.any }).fresh;
const newSetOfElements = b.instance("Set").holding({ element }).fresh;
const newSetOfEitherElements = b.instance("Set").holding({ element: elementOfEitherSet }).fresh;

export default b.declarations(
  b.class("Map", {
    jsType: "JSMapType",
    slots: { key: b.contents("Key", entries), value: b.contents("Value", entries) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    openNamesOfPrototype: ["toJSON"],
    types: [b.exactType("Map", "key", "value"), b.viewType("ReadonlyMap", "key", "value")],
    call: requiresNew,
    construct: [
      {
        args: { entries: b.null },
        returns: b.instance("Map").holding({ key: b.never, value: b.never }).fresh,
        capability: "heap",
        isSpecialCase: true,
        implementation: b.inline("NewMap"),
      },
      {
        args: { entries: b.any.admitting(arrayOfObjects) },
        returns: b.instance("Map").fresh,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: { entries: storedAsEntries },
        model: b.functionIn("models/Collections", "mapFromEntries"),
        implementation: b.inline("MapFromEntries", { otherwise: b.hostCall }),
      },
    ],
    static: {
      groupBy: {
        args: { items: b.any, callback: b.callable },
        returns: b.instance("Map").holding({ value: b.instance("Array").holding({ element: b.iterated("items") }).fresh }).fresh,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          items: [b.runs("Iterate")],
          callback: [
            b.called({
              afterwards: "TouchesOnlyWhatRowNames",
              times: "PerElement",
              args: [b.iterated("items"), b.length],
              result: "Stored",
              resultTo: b.resultSlot("key"),
            }),
          ],
        },
        implementation: b.compiled("MapConstructor", "groupBy"),
      },
    },
    proto: {
      size: {
        getter: size,
        capability: "none",
        uses: { this: [b.readsContents()] },
        privateAlias: true,
        length: 0,
        boxedCallStub: "MapSize",
        nativeSymbols: ["mapProtoFuncSize"],
        implementation: b.inline("MapSize"),
      },
      clear: { ...clearing("MapClear"), privateAlias: true },
      delete: {
        ...removal(b.inline("MapDelete")),
        throws: b.propagated,
        privateAlias: true,
        length: 1,
        boxedCallStub: "MapDelete",
        nativeSymbols: ["mapProtoFuncDelete"],
      },
      entries: { ...iteratorOver("MapIterator", "MapIterator", "Entries"), privateAlias: true },
      keys: { ...iteratorOver("MapIterator", "MapIterator", "Keys"), privateAlias: true },
      values: { ...iteratorOver("MapIterator", "MapIterator", "Values"), privateAlias: true },
      forEach: {
        args: { callback: b.callable, thisArg: b.any.optional },
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [b.readsContents()],
          callback: [
            b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "PerElement", this: b.operand("thisArg"), args: [value, key, self], result: "Ignored" }),
          ],
          thisArg: b.inspected,
        },
        privateAlias: true,
        implementation: b.compiled("MapPrototype", "forEach"),
      },
      get: {
        args: { key: b.any },
        returns: b.union(value, b.undefined),
        capability: "heap",
        throws: b.propagated,
        uses: { this: [b.readsContents()], key: b.inspected },
        writes: [storage],
        privateAlias: true,
        length: 1,
        boxedCallStub: "MapGet",
        nativeSymbols: ["mapProtoFuncGet"],
        implementation: b.inline("MapGet"),
      },
      has: {
        ...lookup("MapHas"),
        writes: [storage],
        throws: b.propagated,
        privateAlias: true,
        length: 1,
        boxedCallStub: "MapHas",
        nativeSymbols: ["mapProtoFuncHas"],
      },
      set: {
        args: { key: b.any, value: b.any },
        returns: self,
        capability: "heap",
        throws: b.propagated,
        uses: { this: [b.readsContents()], key: [b.storedIn(key)], value: [b.storedIn(value)] },
        privateAlias: true,
        length: 2,
        boxedCallStub: "MapSet",
        nativeSymbols: ["mapProtoFuncSet"],
        implementation: b.inline("MapSet"),
      },
      getOrInsert: {
        args: { key: b.any, value: b.any },
        returns: b.union(value, b.operand("value")),
        capability: "heap",
        throws: b.propagated,
        uses: { this: [b.readsContents()], key: [b.storedIn(key)], value: [b.storedIn(value)] },
        usesFeature: "MapGetOrInsert",
        nativeSymbols: ["mapProtoFuncGetOrInsert"],
        implementation: b.hostCall,
      },
      getOrInsertComputed: {
        args: { key: b.any, callback: b.callable },
        returns: b.union(value, b.callbackResult("callback")),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [b.readsContents()],
          key: [b.storedIn(key)],
          // The callback gets the key as the map keeps it: +0 for -0.
          callback: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", args: [key], result: "Stored", resultTo: value })],
        },
        usesFeature: "MapGetOrInsert",
        nativeSymbols: ["mapProtoFuncGetOrInsertComputed"],
        implementation: b.hostCall,
      },
    },
  }),
  b.same("Map.prototype.@@iterator", "Map.prototype.entries"),

  b.class("Set", {
    jsType: "JSSetType",
    slots: { element: b.contents("Element", entries) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    openNamesOfPrototype: ["toJSON"],
    types: [b.exactType("Set", "element"), b.viewType("ReadonlySet", "element")],
    call: requiresNew,
    construct: [
      {
        args: { values: b.null },
        returns: b.instance("Set").holding({ element: b.never }).fresh,
        capability: "heap",
        isSpecialCase: true,
        implementation: b.inline("NewSet"),
      },
      {
        args: { values: b.any.admitting(b.instance("Array")) },
        returns: b.instance("Set").holding({ element: b.iterated("values") }).fresh,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: { values: [b.runs("Iterate")] },
        model: b.functionIn("models/Collections", "setFromValues"),
        implementation: b.inline("SetFromIterable", { otherwise: b.hostCall }),
      },
    ],
    proto: {
      size: {
        getter: size,
        capability: "none",
        uses: { this: [b.readsContents()] },
        privateAlias: true,
        length: 0,
        boxedCallStub: "SetSize",
        nativeSymbols: ["setProtoFuncSize"],
        implementation: b.inline("SetSize"),
      },
      add: {
        args: { value: b.any },
        returns: self,
        capability: "heap",
        throws: b.propagated,
        uses: { this: [b.readsContents()], value: [b.storedIn(element)] },
        privateAlias: true,
        length: 1,
        boxedCallStub: "SetAdd",
        nativeSymbols: ["setProtoFuncAdd"],
        implementation: b.inline("SetAdd"),
      },
      clear: { ...clearing("SetClear"), privateAlias: true },
      delete: {
        ...removal(b.inline("SetDelete")),
        throws: b.propagated,
        privateAlias: true,
        length: 1,
        boxedCallStub: "SetDelete",
        nativeSymbols: ["setProtoFuncDelete"],
      },
      entries: { ...iteratorOver("SetIterator", "SetIterator", "Entries"), privateAlias: true },
      values: { ...iteratorOver("SetIterator", "SetIterator", "Values"), privateAlias: true },
      forEach: {
        args: { callback: b.callable, thisArg: b.any.optional },
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [b.readsContents()],
          callback: [
            b.called({
              afterwards: "TouchesOnlyWhatRowNames",
              times: "PerElement",
              this: b.operand("thisArg"),
              args: [element, element, self],
              result: "Ignored",
            }),
          ],
          thisArg: b.inspected,
        },
        privateAlias: true,
        implementation: b.compiled("SetPrototype", "forEach"),
      },
      has: {
        ...lookup("SetHas"),
        writes: [storage],
        throws: b.propagated,
        privateAlias: true,
        length: 1,
        boxedCallStub: "SetHas",
        nativeSymbols: ["setProtoFuncHas"],
      },
      difference: setMethod("setProtoFuncDifference", newSetOfElements, newSetOfElements),
      intersection: setMethod("setProtoFuncIntersection", newSetOfElements, newSetOfElements),
      symmetricDifference: setMethod("setProtoFuncSymmetricDifference", newSetOfEitherElements, newSetOfAnything),
      union: setMethod("setProtoFuncUnion", newSetOfEitherElements, newSetOfAnything),
      isDisjointFrom: setMethod("setProtoFuncIsDisjointFrom", b.bool, b.bool),
      isSubsetOf: setMethod("setProtoFuncIsSubsetOf", b.bool, b.bool),
      isSupersetOf: setMethod("setProtoFuncIsSupersetOf", b.bool, b.bool),
    },
  }),
  b.same("Set.prototype.keys", "Set.prototype.values"),
  b.same("Set.prototype.@@iterator", "Set.prototype.values"),
  b.privateAlias("Set.prototype.keys"),

  b.class("WeakMap", {
    jsType: "JSWeakMapType",
    slots: { key: b.contents("Key", entries), value: b.contents("Value", entries) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    types: [b.exactType("WeakMap", "key", "value")],
    call: requiresNew,
    construct: [
      {
        args: { entries: b.null },
        returns: b.instance("WeakMap").holding({ key: b.never, value: b.never }).fresh,
        capability: "heap",
        implementation: b.hostCall,
      },
      {
        args: { entries: b.any },
        returns: b.instance("WeakMap").fresh,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { entries: storedAsEntries },
        model: b.functionIn("models/Collections", "weakMapFromEntries"),
        implementation: b.hostCall,
      },
    ],
    proto: {
      delete: removal(b.hostCall),
      get: {
        args: { key: b.any },
        returns: b.union(value, b.undefined),
        capability: "heap",
        uses: { this: [b.readsContents()], key: b.inspected },
        length: 1,
        boxedCallStub: "WeakMapGet",
        nativeSymbols: ["protoFuncWeakMapGet"],
        implementation: b.inline("WeakMapGet", { otherwise: b.hostCall }),
      },
      has: {
        ...lookup("WeakMapHas", b.hostCall),
        length: 1,
        boxedCallStub: "WeakMapHas",
        nativeSymbols: ["protoFuncWeakMapHas"],
      },
      set: {
        args: { key: b.any, value: b.any },
        returns: self,
        capability: "heap",
        throws: true,
        uses: { this: [b.readsContents()], key: [b.storedIn(key)], value: [b.storedIn(value)] },
        length: 2,
        boxedCallStub: "WeakMapSet",
        nativeSymbols: ["protoFuncWeakMapSet"],
        implementation: b.hostCall,
      },
      getOrInsert: {
        args: { key: b.any, value: b.any },
        returns: b.union(value, b.operand("value")),
        capability: "heap",
        throws: true,
        uses: { this: [b.readsContents()], key: [b.storedIn(key)], value: [b.storedIn(value)] },
        usesFeature: "MapGetOrInsert",
        nativeSymbols: ["protoFuncWeakMapGetOrInsert"],
        implementation: b.hostCall,
      },
      getOrInsertComputed: {
        args: { key: b.any, callback: b.callable },
        returns: b.union(value, b.callbackResult("callback")),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [b.readsContents()],
          key: [b.storedIn(key)],
          callback: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", args: [b.operand("key")], result: "Stored", resultTo: value })],
        },
        usesFeature: "MapGetOrInsert",
        nativeSymbols: ["protoFuncWeakMapGetOrInsertComputed"],
        implementation: b.hostCall,
      },
    },
  }),

  b.class("WeakSet", {
    jsType: "JSWeakSetType",
    slots: { element: b.contents("Element", entries) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    types: [b.exactType("WeakSet", "element")],
    call: requiresNew,
    construct: [
      {
        args: { values: b.null },
        returns: b.instance("WeakSet").holding({ element: b.never }).fresh,
        capability: "heap",
        implementation: b.hostCall,
      },
      {
        args: { values: b.any.admitting(arrayOfObjects) },
        returns: b.instance("WeakSet").holding({ element: b.iterated("values") }).fresh,
        capability: "js",
        throws: b.propagated,
        runsOnlyOperandCode: true,
        uses: { values: [b.runs("Iterate")] },
        model: b.functionIn("models/Collections", "weakSetFromValues"),
        implementation: b.hostCall,
      },
    ],
    proto: {
      add: {
        args: { value: b.any },
        returns: self,
        capability: "heap",
        throws: true,
        uses: { this: [b.readsContents()], value: [b.storedIn(element)] },
        implementation: b.hostCall,
      },
      delete: removal(b.hostCall),
      has: {
        ...lookup("WeakSetHas", b.hostCall),
        length: 1,
        boxedCallStub: "WeakSetHas",
        nativeSymbols: ["protoFuncWeakSetHas"],
      },
    },
  }),

  b.class("WeakRef", {
    instanceCppClass: b.cppClass("JSWeakObjectRef"),
    slots: { target: b.state("InternalFields") },
    types: [b.exactType("WeakRef", "target")],
    usesFeature: "WeakReferences",
    nativeSymbols: ["callWeakRef", "constructWeakRef"],
    call: requiresNew,
    construct: {
      args: { target: b.any },
      returns: b.instance("WeakRef").fresh,
      capability: "heap",
      throws: true,
      uses: { target: [b.storedIn(b.resultSlot("target"))] },
      implementation: b.hostCall,
    },
    proto: {
      deref: {
        returns: b.union(b.slot("this", "target"), b.undefined),
        capability: "heap",
        uses: { this: [b.readsContents()] },
        usesFeature: "WeakReferences",
        nativeSymbols: ["protoFuncWeakRefDeref"],
        implementation: b.hostCall,
      },
    },
  }),

  b.class("FinalizationRegistry", {
    instanceCppClass: b.cppClass("JSFinalizationRegistry"),
    slots: {
      cleanup: b.state("InternalFields"),
      target: b.state("InternalFields"),
      heldValue: b.state("InternalFields"),
      unregisterToken: b.state("InternalFields"),
    },
    types: [b.exactType("FinalizationRegistry", "heldValue")],
    usesFeature: "WeakReferences",
    nativeSymbols: ["callFinalizationRegistry", "constructFinalizationRegistry"],
    call: requiresNew,
    construct: {
      args: { cleanup: b.callable },
      returns: b.instance("FinalizationRegistry").holding({ target: b.never, heldValue: b.never, unregisterToken: b.never }).fresh,
      capability: "heap",
      throws: true,
      uses: {
        cleanup: [
          b.storedIn(b.resultSlot("cleanup")),
          b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", args: [b.resultSlot("heldValue")], result: "Ignored" }),
        ],
      },
      implementation: b.hostCall,
    },
    proto: {
      register: {
        args: { target: b.any, heldValue: b.any, unregisterToken: b.any.optional },
        capability: "heap",
        throws: true,
        uses: {
          this: [b.writesContents()],
          target: [b.storedIn(b.slot("this", "target"))],
          heldValue: [b.storedIn(b.slot("this", "heldValue"))],
          unregisterToken: [b.storedIn(b.slot("this", "unregisterToken"))],
        },
        usesFeature: "WeakReferences",
        nativeSymbols: ["protoFuncFinalizationRegistryRegister"],
        implementation: b.hostCall,
      },
      unregister: {
        args: { unregisterToken: b.any },
        returns: b.bool,
        capability: "heap",
        throws: true,
        uses: { this: [b.writesContents()], unregisterToken: b.inspected },
        usesFeature: "WeakReferences",
        nativeSymbols: ["protoFuncFinalizationRegistryUnregister"],
        implementation: b.hostCall,
      },
    },
  }),

  ["Map", "Set"].flatMap(name =>
    b.static(`${name}.@@species`, {
      this: b.any,
      getter: b.operand("this"),
      capability: "none",
      uses: { this: b.inspected },
      mutability: "AlwaysLocked",
      implementation: b.hostCall,
    }),
  ),

  // `import "core-js"` replaces a proposal without a test until the proposal is finished: 3.22 to 3.30, and 3.40 to 3.41.
  b.replacedByPackages("Map.groupBy"),
  b.replacedByPackages(
    "Map.prototype.getOrInsert",
    "Map.prototype.getOrInsertComputed",
    "WeakMap.prototype.getOrInsert",
    "WeakMap.prototype.getOrInsertComputed",
  ),
);
