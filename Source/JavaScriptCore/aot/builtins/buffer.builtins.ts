import * as b from "bun-builtins-table";

type ParameterList = Readonly<Record<string, b.TableType>>;

const bytes = "TypedArrayElements";
const storage = "TypedArrayStorage";
const otherAgents = "ExternalState";

const byteCount = b.int(0, b.checked(2 ** 32, "MAX_ARRAY_BUFFER_SIZE"));
const anyArrayBuffer = b.union(b.instance("ArrayBuffer"), b.instance("SharedArrayBuffer"));
const readsState = [b.readsContents()];
const changesState = [b.readsContents(), b.writesContents()];

const requiresNew = {
  returns: b.never,
  capability: "none",
  throws: true,
  implementation: b.hostCall,
} satisfies b.FunctionField;

const speciesGetter = {
  this: b.any,
  getter: b.operand("this"),
  capability: "none",
  uses: { this: b.inspected },
  mutability: "AlwaysLocked",
  implementation: b.inline("ReturnThis"),
} satisfies b.GetterField;

const bufferConstruction = (className: string) =>
  [
    {
      args: { length: b.index.loose.optional, options: b.primitiveValue.optional },
      returns: b.instance(className).fresh,
      capability: "heap",
      throws: true,
      implementation: b.hostCall,
    },
    {
      args: { length: b.index.loose, options: b.any },
      returns: b.instance(className).fresh,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: {
        options: [b.readByKey(b.keysNamed("maxByteLength"), b.nativeMemory), b.runsTransitively("ToPrimitive")],
      },
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const bufferGetter = (holds: b.TableType, uses: readonly b.UseSpec[]) =>
  ({
    getter: holds,
    capability: "heap",
    throws: true,
    uses: { this: uses },
    implementation: b.hostCall,
  }) satisfies b.GetterField;

const bufferSlice = (className: string) =>
  ({
    args: { start: b.integer.loose.optional, end: b.integer.loose.optional },
    returns: b.instance(className).freshUnlessSpecies,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    readsStateBeforeConversions: true,
    uses: { this: [...changesState, b.runs("Species")] },
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const lengthChange = {
  args: { newLength: b.index.loose },
  capability: "heap",
  throws: true,
  uses: { this: changesState },
  implementation: b.hostCall,
} satisfies b.FunctionField;

const bufferTransfer = {
  args: { newLength: b.index.loose.optional },
  returns: b.instance("ArrayBuffer").fresh,
  capability: "heap",
  throws: true,
  uses: { this: changesState },
  implementation: b.hostCall,
} satisfies b.FunctionField;

const byteOrder: ParameterList = { littleEndian: b.bool.loose.optional };
const singleByte: ParameterList = {};
const numberToStore = b.f64.loose;
const bigIntToStore = b.bigint.loose;

const viewExtent = {
  getter: byteCount,
  capability: "heap",
  throws: true,
  uses: { this: readsState },
  usesFeature: "DataView",
} satisfies b.GetterField;

const viewRead = (elementType: string, element: b.TableType, order = byteOrder) =>
  ({
    args: { byteOffset: b.index.loose, ...order },
    returns: element,
    capability: "heap",
    throws: true,
    uses: { this: readsState },
    usesFeature: "DataView",
    implementation: b.inline("DataViewGet", { parameters: [elementType], otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const viewWrite = (elementType: string, value: b.TableType, order = byteOrder) =>
  ({
    args: { byteOffset: b.index.loose, value, ...order },
    capability: "heap",
    throws: true,
    uses: { this: [b.readsContents(), b.writesContents("bytes")] },
    usesFeature: "DataView",
    implementation: b.inline("DataViewSet", { parameters: [elementType], otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const integerArray = b.union(
  b.instance("Int8Array"),
  b.instance("Uint8Array"),
  b.instance("Int16Array"),
  b.instance("Uint16Array"),
  b.instance("Int32Array"),
  b.instance("Uint32Array"),
);
const bigIntArray = b.union(b.instance("BigInt64Array"), b.instance("BigUint64Array"));
const waitableArray = b.union(b.instance("Int32Array"), b.instance("BigInt64Array"));
const integerElement = b.int(-(2 ** 31), 2 ** 32 - 1);

const atomicAccess = {
  capability: "heap",
  throws: true,
  readsStateBeforeConversions: true,
  reads: [otherAgents],
  writes: [otherAgents],
} satisfies b.FunctionField;

function atomicOperation(
  operation: string,
  nativeSymbol: string,
  operands: readonly string[],
  integerResult: b.TableType = integerElement,
): b.FunctionField[] {
  const operandsOf = (type: b.TableType): ParameterList => Object.fromEntries(operands.map(name => [name, type]));
  const uses = { typedArray: [b.readsContents(), b.writesContents("element")] };
  return [
    {
      ...atomicAccess,
      args: { typedArray: integerArray, index: b.index.loose, ...operandsOf(b.integer.loose) },
      returns: integerResult,
      uses,
      nativeSymbols: [nativeSymbol],
      implementation: b.inline("Atomics", { parameters: [operation], otherwise: b.hostCall }),
    },
    {
      ...atomicAccess,
      args: { typedArray: bigIntArray, index: b.index.loose, ...operandsOf(b.bigint.loose) },
      returns: b.bigint,
      uses,
      implementation: b.hostCall,
    },
  ];
}

function atomicWait(nativeSymbol: string, returns: b.TableType, uses: readonly b.UseSpec[]): b.FunctionField[] {
  const timeout = b.f64.loose.optional;
  const wait = { ...atomicAccess, returns, uses: { typedArray: uses }, implementation: b.hostCall };
  return [
    {
      ...wait,
      args: { typedArray: b.instance("Int32Array"), index: b.index.loose, value: b.i32.wrap, timeout },
      nativeSymbols: [nativeSymbol],
    },
    {
      ...wait,
      args: { typedArray: b.instance("BigInt64Array"), index: b.index.loose, value: b.bigint.loose, timeout },
    },
  ];
}

export default b.declarations(
  b.class("ArrayBuffer", {
    jsType: "ArrayBufferType",
    sharingMode: "Default",
    slots: { bytes: b.state(bytes) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    types: [b.exactType("ArrayBuffer")],
    call: requiresNew,
    construct: bufferConstruction("ArrayBuffer"),
    static: {
      "@@species": speciesGetter,
      isView: {
        args: { arg: b.any },
        returns: b.bool,
        capability: "heap",
        uses: { arg: b.inspected },
        implementation: b.hostCall,
      },
    },
    proto: {
      byteLength: bufferGetter(byteCount, readsState),
      detached: bufferGetter(b.bool, readsState),
      maxByteLength: bufferGetter(byteCount, readsState),
      resizable: bufferGetter(b.bool, b.inspected),
      resize: lengthChange,
      slice: bufferSlice("ArrayBuffer"),
      transfer: bufferTransfer,
      transferToFixedLength: bufferTransfer,
    },
  }),

  b.class("SharedArrayBuffer", {
    jsType: "ArrayBufferType",
    sharingMode: "Shared",
    slots: { bytes: b.state(bytes) },
    stateHeap: storage,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    types: [b.exactType("SharedArrayBuffer")],
    call: requiresNew,
    construct: bufferConstruction("SharedArrayBuffer"),
    static: { "@@species": speciesGetter },
    proto: {
      byteLength: { ...bufferGetter(byteCount, readsState), reads: [otherAgents], writes: [otherAgents] },
      growable: bufferGetter(b.bool, b.inspected),
      maxByteLength: bufferGetter(byteCount, b.inspected),
      grow: { ...lengthChange, reads: [otherAgents], writes: [otherAgents] },
      slice: bufferSlice("SharedArrayBuffer"),
    },
  }),

  b.class("DataView", {
    jsType: "DataViewType",
    slots: { buffer: b.state(storage, anyArrayBuffer), bytes: b.state(bytes) },
    stateHeap: storage,
    lockedMembers: ["prototype"],
    types: [b.exactType("DataView", "buffer")],
    usesFeature: "DataView",
    nativeSymbols: ["callDataView", "constructDataView"],
    prototypeTable: b.staticTable("runtime/JSDataViewPrototype.cpp", "dataViewTable"),
    call: requiresNew,
    construct: {
      args: { buffer: anyArrayBuffer, byteOffset: b.index.loose.optional, byteLength: b.index.loose.optional },
      returns: b.instance("DataView").fresh,
      capability: "heap",
      throws: true,
      readsStateBeforeConversions: true,
      uses: { buffer: [b.retained, b.readsContents(), b.storedIn(b.resultSlot("buffer"))] },
      implementation: b.hostCall,
    },
    proto: {
      buffer: {
        getter: b.slot("this", "buffer"),
        capability: "heap",
        uses: { this: b.inspected },
        usesFeature: "DataView",
        implementation: b.generic,
      },
      byteLength: { ...viewExtent, nativeSymbols: ["dataViewProtoGetterByteLength"], implementation: b.hostCall },
      byteOffset: { ...viewExtent, implementation: b.generic },
      getInt8: viewRead("TypeInt8", b.int(-128, 127), singleByte),
      getUint8: viewRead("TypeUint8", b.int(0, 255), singleByte),
      getInt16: viewRead("TypeInt16", b.int(-32768, 32767)),
      getUint16: viewRead("TypeUint16", b.int(0, 65535)),
      getInt32: viewRead("TypeInt32", b.i32),
      getUint32: viewRead("TypeUint32", b.u32),
      getFloat16: viewRead("TypeFloat16", b.f64),
      getFloat32: viewRead("TypeFloat32", b.f64),
      getFloat64: viewRead("TypeFloat64", b.f64),
      getBigInt64: viewRead("TypeBigInt64", b.bigint),
      getBigUint64: viewRead("TypeBigUint64", b.bigint),
      setInt8: viewWrite("TypeInt8", numberToStore, singleByte),
      setUint8: viewWrite("TypeUint8", numberToStore, singleByte),
      setInt16: viewWrite("TypeInt16", numberToStore),
      setUint16: viewWrite("TypeUint16", numberToStore),
      setInt32: viewWrite("TypeInt32", numberToStore),
      setUint32: viewWrite("TypeUint32", numberToStore),
      setFloat16: viewWrite("TypeFloat16", numberToStore),
      setFloat32: viewWrite("TypeFloat32", numberToStore),
      setFloat64: viewWrite("TypeFloat64", numberToStore),
      setBigInt64: viewWrite("TypeBigInt64", bigIntToStore),
      setBigUint64: viewWrite("TypeBigUint64", bigIntToStore),
    },
  }),

  b.namespace(
    "Atomics",
    {
      add: atomicOperation("Add", "atomicsFuncAdd", ["value"]),
      and: atomicOperation("And", "atomicsFuncAnd", ["value"]),
      compareExchange: atomicOperation("CompareExchange", "atomicsFuncCompareExchange", [
        "expectedValue",
        "replacementValue",
      ]),
      exchange: atomicOperation("Exchange", "atomicsFuncExchange", ["value"]),
      isLockFree: {
        args: { size: b.integer.loose },
        returns: b.bool,
        capability: "heap",
        nativeSymbols: ["atomicsFuncIsLockFree"],
        implementation: b.hostCall,
      },
      load: atomicOperation("Load", "atomicsFuncLoad", []),
      notify: {
        ...atomicAccess,
        args: { typedArray: waitableArray, index: b.index.loose, count: b.integer.loose.optional },
        returns: b.u32,
        uses: { typedArray: readsState },
        nativeSymbols: ["atomicsFuncNotify"],
        implementation: b.hostCall,
      },
      or: atomicOperation("Or", "atomicsFuncOr", ["value"]),
      pause: {
        args: { iterationNumber: b.f64.optional },
        capability: "heap",
        throws: true,
        reads: [otherAgents],
        writes: [otherAgents],
        nativeSymbols: ["atomicsFuncPause"],
        implementation: b.hostCall,
      },
      store: atomicOperation("Store", "atomicsFuncStore", ["value"], b.f64),
      sub: atomicOperation("Sub", "atomicsFuncSub", ["value"]),
      wait: atomicWait("atomicsFuncWait", b.string, changesState),
      waitAsync: atomicWait("atomicsFuncWaitAsync", b.object.unaliased, [...changesState, b.retained]),
      xor: atomicOperation("Xor", "atomicsFuncXor", ["value"]),
    },
    { locksMembers: true, mutability: "AlwaysLocked", usesFeature: "Atomics" },
  ),
);
