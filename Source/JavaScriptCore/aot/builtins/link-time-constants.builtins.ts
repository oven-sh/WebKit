import * as b from "bun-builtins-table";

type Contents = Readonly<Record<string, b.TableType>>;

const promiseState = "PromiseState";
const typedArrayStorage = "TypedArrayStorage";
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;
const realmPromise = b.builtin("Promise");
const newPromiseHolding = (contents: Contents) => b.instance("Promise").holding(contents).fresh;
// No primitive is a thenable, so nothing rejects the promise.
const fulfilledWithPrimitive = newPromiseHolding({ value: b.operand("value"), reason: b.never });
const otherConstructor = b.otherwiseConstructs(0, b.hostCall);

const keptByIterator = b.storedIn(b.resultSlot("iterated"));

const iteratorCreation = (className: string, kept: Readonly<Record<string, b.TableType>>) =>
  ({
    args: kept,
    returns: b.instance(className).fresh,
    capability: "heap",
    uses: Object.fromEntries(Object.keys(kept).map(name => [name, [keptByIterator]])),
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const typedArrayClasses = [
  "Int8Array",
  "Uint8Array",
  "Uint8ClampedArray",
  "Int16Array",
  "Uint16Array",
  "Int32Array",
  "Uint32Array",
  "Float16Array",
  "Float32Array",
  "Float64Array",
  "BigInt64Array",
  "BigUint64Array",
] as const;

// The view is made without a call of the constructor, which only says of which class it is.
const copyOfItems = (constructor: b.TableType, className: string) =>
  ({
    args: { constructor, items: b.any },
    returns: b.union(b.undefined, b.instance(className)).fresh,
    capability: "heap",
    throws: true,
    uses: { constructor: b.inspected, items: [b.readsContents()] },
    // The items may be a view.
    reads: [typedArrayStorage, "TypedArrayElements"],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

// Only the library names these functions, and it passes the operands they take without a test.
export default b.declarations(
  b.linkTimeConstants({
    promiseResolve: [
      {
        ...operandCodeEffects,
        args: { constructor: realmPromise, value: b.instance("Promise") },
        returns: b.instance("Promise"),
        uses: {
          constructor: b.inspected,
          value: [b.runs("Thenable", b.named("constructor")), b.membersOnlyInspected, b.retained, b.returned],
        },
        reads: [promiseState],
        writes: [promiseState],
        implementation: b.hostCall,
      },
      {
        args: { constructor: realmPromise, value: b.primitiveValue },
        returns: fulfilledWithPrimitive,
        capability: "heap",
        uses: { constructor: b.inspected, value: b.inspected },
        reads: [promiseState],
        writes: [promiseState],
        implementation: b.hostCall,
      },
      {
        args: { constructor: realmPromise, value: b.any },
        returns: newPromiseHolding({ value: b.operand("value") }),
        capability: "js",
        runsOnlyOperandCode: true,
        uses: { constructor: b.inspected, value: [b.runs("Thenable")] },
        reads: [promiseState],
        writes: [promiseState],
        implementation: b.hostCall,
      },
      otherConstructor,
    ],
    promiseReject: [
      {
        args: { constructor: realmPromise, reason: b.any },
        returns: newPromiseHolding({ value: b.never }),
        capability: "heap",
        uses: { constructor: b.inspected, reason: [b.storedIn(b.resultSlot("reason"))] },
        writes: [promiseState],
        implementation: b.hostCall,
      },
      otherConstructor,
    ],
    newResolvedPromise: [
      {
        args: { value: b.primitiveValue },
        returns: fulfilledWithPrimitive,
        capability: "heap",
        uses: { value: b.inspected },
        reads: [promiseState],
        writes: [promiseState],
        implementation: b.hostCall,
      },
      {
        args: { value: b.any },
        returns: newPromiseHolding({ value: b.operand("value") }),
        capability: "js",
        runsOnlyOperandCode: true,
        uses: { value: [b.runs("Thenable")] },
        reads: [promiseState],
        writes: [promiseState],
        implementation: b.hostCall,
      },
    ],
    newRejectedPromise: {
      args: { reason: b.any },
      returns: newPromiseHolding({ value: b.never }),
      capability: "heap",
      uses: { reason: [b.storedIn(b.resultSlot("reason"))] },
      writes: [promiseState],
      implementation: b.hostCall,
    },

    toIntegerOrInfinity: {
      ...operandCodeEffects,
      args: { value: b.integer.loose },
      returns: b.f64,
      implementation: b.hostCall,
    },
    instanceOf: {
      ...operandCodeEffects,
      args: { value: b.any, prototype: b.any },
      returns: b.bool,
      uses: { value: [b.runs("PrototypeChain")], prototype: b.inspected },
      implementation: b.hostCall,
    },
    makeTypeError: {
      ...operandCodeEffects,
      args: { message: b.string.loose.optional },
      returns: b.instance("TypeError").unaliased,
      implementation: b.hostCall,
    },

    isArray: {
      args: { value: b.any },
      returns: b.bool,
      capability: "heap",
      throws: true,
      uses: { value: b.inspected },
      implementation: b.inline("ArrayIsArray", { otherwise: b.hostCall }),
    },
    arrayFromFastWithoutMapFn: {
      ...operandCodeEffects,
      args: { constructor: b.any, items: b.any },
      returns: b.union(b.undefined, b.instance("Array").holding({ element: b.union(b.iterated("items"), b.undefined) })).fresh,
      uses: { constructor: b.inspected, items: [b.runs("Iterate"), b.contentsStoredIn(b.resultSlot("element"))] },
      implementation: b.hostCall,
    },
    typedArrayFromFast: [
      ...typedArrayClasses.map(name => copyOfItems(b.builtin(name), name)),
      copyOfItems(b.any, "TypedArray"),
    ],
    typedArrayLength: {
      args: { view: b.any },
      returns: b.length,
      capability: "heap",
      throws: true,
      uses: { view: b.inspected },
      reads: [typedArrayStorage],
      implementation: b.hostCall,
    },
    isTypedArrayOutOfBounds: {
      args: { view: b.instance("TypedArray") },
      returns: b.bool,
      capability: "heap",
      uses: { view: b.inspected },
      reads: [typedArrayStorage],
      implementation: b.hostCall,
    },

    // Each step of the helper resumes the generator without asking whether it is one.
    iteratorHelperCreate: iteratorCreation("IteratorHelper", { generator: b.instance("Generator"), underlyingIterator: b.any }),
    wrapForValidIteratorCreate: iteratorCreation("WrapForValidIterator", { iterator: b.any, nextMethod: b.any }),
    asyncFromSyncIteratorCreate: {
      ...operandCodeEffects,
      args: { iterator: b.any },
      returns: b.instance("AsyncFromSyncIterator").fresh,
      uses: { iterator: [keptByIterator, b.readByKey(b.keysNamed("next"), b.resultSlot("iterated"))] },
      implementation: b.hostCall,
    },
  }),
);
