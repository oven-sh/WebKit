import * as b from "bun-builtins-table";

type Contents = Readonly<Record<string, b.TableType>>;

const promiseState = "PromiseState";
const operandCodeEffects = { capability: "js", throws: true, runsOnlyOperandCode: true } as const;
const pristinePromise = b.instance("Promise").pristine("Names");
const settledValue = b.slot("this", "value");
const settledReason = b.slot("this", "reason");
const valueOrHandlerResult = b.union(settledValue, b.callbackResult("onRejected"));
const eitherHandlerResult = b.union(b.callbackResult("onFulfilled"), b.callbackResult("onRejected"));
const contentsUpdate = [b.readsContents(), b.writesContents()];
const newPromiseHolding = (contents: Contents) => b.instance("Promise").holding(contents).fresh;
const newArrayOf = (elements: b.TableType) => b.instance("Array").holding({ element: elements }).fresh;

const requiresNew = {
  returns: b.never,
  capability: "none",
  throws: true,
  implementation: b.hostCall,
} satisfies b.FunctionField;

const otherReceiver = b.otherwiseBy(b.hostCall);

const chainedReaction = (settled: b.TableType) => [
  b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", args: [settled], result: "Stored", resultTo: b.resultSlot("value") }),
];
const escapingReaction = (settled: b.TableType) => [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", args: [settled], result: "Escapes" })];

const pristineThen = (onFulfilled: b.TableType, onRejected: b.TableType, contents: Contents) =>
  ({
    ...operandCodeEffects,
    this: pristinePromise,
    args: { onFulfilled, onRejected },
    returns: newPromiseHolding(contents),
    uses: {
      this: contentsUpdate,
      onFulfilled: chainedReaction(settledValue),
      onRejected: chainedReaction(settledReason),
    },
    implementation: b.inline("PromiseThen"),
  }) satisfies b.FunctionField;

const pristineCatch = (onRejected: b.TableType, contents: Contents) =>
  ({
    ...operandCodeEffects,
    this: pristinePromise,
    args: { onRejected },
    returns: newPromiseHolding(contents),
    uses: { this: contentsUpdate, onRejected: chainedReaction(settledReason) },
    implementation: b.inline("PromiseCatch"),
  }) satisfies b.FunctionField;

const resolveImplementation = b.inline("PromiseResolve", { otherwise: b.hostCall });

const elementProtocols = b.runsTransitively("Iterate", "Thenable");
// The promise settles as the first element that settles does: with what a promise among the elements holds, not with the promise.
const adoptedElement = b.settled("iterable");
const settlementRecord = b.union(
  b.ordinaryObjectOf("PromiseFulfilledResult"),
  b.ordinaryObjectOf("PromiseRejectedResult"),
).fresh;

const combinator = (feature: string, contents: Contents, ...iterableUses: readonly b.UseSpec[]) =>
  [
    {
      ...operandCodeEffects,
      args: { iterable: b.any },
      returns: newPromiseHolding(contents),
      uses: { this: b.inspected, iterable: [elementProtocols, ...iterableUses] },
      requiresHolderAsThis: true,
      usesFeature: feature,
      implementation: b.hostCall,
    },
    otherReceiver,
  ] satisfies readonly b.FunctionField[];

const disposalOf = (...disposed: readonly b.TableType[]) => [
  b.retained,
  b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", args: disposed, result: "Escapes" }),
];
const stackFunction = (name: string) => b.compiled("AsyncDisposableStackPrototype", name);

export default b.declarations(
  b.layout("PromiseCapability", {
    resolve: b.callable,
    reject: b.callable,
    promise: newPromiseHolding({ value: b.any, reason: b.any }),
  }),
  b.layout("PromiseFulfilledResult", { status: b.enumeration(["fulfilled"]), value: b.any }),
  b.layout("PromiseRejectedResult", { status: b.enumeration(["rejected"]), reason: b.any }),

  b.class("Promise", {
    jsType: "JSPromiseType",
    constructorCallee: "Function",
    constructorOnlyReadsPrototypeOfNewTarget: true,
    slots: { value: b.contents("Value", promiseState), reason: b.state(promiseState) },
    lockedObjects: ["prototype"],
    lockedMembers: ["constructor"],
    types: [b.viewType("Promise", "value"), b.viewType("PromiseLike", "value")],
    constructorTable: b.staticTable("runtime/JSPromiseConstructor.cpp", "promiseConstructorTable"),
    prototypeTable: b.staticTable("runtime/JSPromisePrototype.cpp", "promisePrototypeTable"),
    call: requiresNew,
    construct: {
      args: { executor: b.callable },
      returns: newPromiseHolding({ value: b.any, reason: b.any }),
      capability: "js",
      runsOnlyOperandCode: true,
      uses: { executor: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", args: [b.callable, b.callable], result: "Ignored" })] },
      implementation: b.compiled("PromiseConstructor", "Promise"),
    },
    static: {
      "@@species": {
        this: b.any,
        getter: b.operand("this"),
        capability: "none",
        uses: { this: b.inspected },
        implementation: b.hostCall,
      },
      all: combinator("PromiseAll", { value: newArrayOf(b.settled("iterable")) }),
      allSettled: combinator(
        "PromiseAllSettled",
        { value: newArrayOf(settlementRecord) },
        b.settledValuesStoredIn(b.nativeMemory),
      ),
      any: combinator("PromiseAny", { value: adoptedElement }),
      race: combinator("PromiseRace", { value: adoptedElement }),
      reject: [
        {
          args: { reason: b.any.optional },
          returns: newPromiseHolding({ value: b.never }),
          capability: "heap",
          // The host hands the reason of a promise that nobody handles to the listeners for unhandled rejections, and Promise.any puts it among the errors of what it rejects with.
          uses: { this: b.inspected, reason: [b.storedIn(b.resultSlot("reason")), b.retained] },
          writes: [promiseState],
          requiresHolderAsThis: true,
          privateAlias: true,
          implementation: b.hostCall,
        },
        otherReceiver,
      ],
      resolve: [
        {
          args: { value: pristinePromise },
          returns: b.operand("value"),
          capability: "heap",
          uses: { this: b.inspected, value: b.inspected },
          reads: [promiseState],
          writes: [promiseState],
          requiresHolderAsThis: true,
          privateAlias: true,
          implementation: resolveImplementation,
        },
        {
          ...operandCodeEffects,
          args: { value: b.instance("Promise") },
          returns: b.instance("Promise"),
          uses: {
            this: b.inspected,
            value: [b.runs("Thenable", b.named("constructor")), b.membersOnlyInspected, b.retained, b.returned],
          },
          reads: [promiseState],
          writes: [promiseState],
          implementation: resolveImplementation,
        },
        // No primitive is a thenable, so nothing rejects the promise.
        {
          args: { value: b.primitiveValue.optional },
          returns: newPromiseHolding({ value: b.operand("value"), reason: b.never }),
          capability: "heap",
          uses: { this: b.inspected, value: b.inspected },
          reads: [promiseState],
          writes: [promiseState],
          implementation: resolveImplementation,
        },
        {
          args: { value: b.any.optional },
          returns: newPromiseHolding({ value: b.operand("value") }),
          capability: "js",
          runsOnlyOperandCode: true,
          uses: { this: b.inspected, value: [b.runs("Thenable")] },
          reads: [promiseState],
          writes: [promiseState],
          implementation: resolveImplementation,
        },
        otherReceiver,
      ],
      try: [
        {
          ...operandCodeEffects,
          args: { callback: b.any, passed: b.any.rest },
          returns: b.instance("Promise"),
          uses: {
            this: b.inspected,
            callback: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Once", spreadArgs: b.operand("passed"), result: "Escapes" })],
            passed: b.inspected,
          },
          requiresHolderAsThis: true,
          implementation: b.compiled("PromiseConstructor", "try"),
        },
        b.otherwiseBy(b.compiled("PromiseConstructor", "try")),
      ],
      withResolvers: [
        {
          returns: b.ordinaryObjectOf("PromiseCapability").fresh,
          capability: "heap",
          uses: { this: b.inspected },
          requiresHolderAsThis: true,
          implementation: b.hostCall,
        },
        otherReceiver,
      ],
    },
    proto: {
      then: [
        { ...pristineThen(b.callable, b.callable, { value: eitherHandlerResult }), privateAlias: true },
        pristineThen(b.callable, b.any.optional, { value: b.callbackResult("onFulfilled"), reason: settledReason }),
        pristineThen(b.any, b.callable, { value: valueOrHandlerResult }),
        pristineThen(b.any.optional, b.any.optional, { value: settledValue, reason: settledReason }),
        {
          ...operandCodeEffects,
          args: { onFulfilled: b.any.optional, onRejected: b.any.optional },
          returns: b.object,
          uses: {
            this: [...contentsUpdate, b.retained, b.runs("Species")],
            onFulfilled: escapingReaction(settledValue),
            onRejected: escapingReaction(settledReason),
          },
          implementation: b.hostCall,
        },
      ],
      catch: [
        pristineCatch(b.callable, { value: valueOrHandlerResult }),
        pristineCatch(b.any.optional, { value: settledValue, reason: settledReason }),
        {
          ...operandCodeEffects,
          this: b.any.refusing(b.null),
          args: { onRejected: b.any.optional },
          returns: b.any,
          uses: { this: [b.runs(b.calls("then"))], onRejected: [b.retained] },
          implementation: b.hostCall,
        },
      ],
      finally: [
        {
          ...operandCodeEffects,
          this: pristinePromise,
          args: { onFinally: b.any.optional },
          returns: newPromiseHolding({ value: settledValue, reason: settledReason }),
          uses: {
            this: contentsUpdate,
            onFinally: [b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", result: "Stored", resultTo: b.resultSlot("value") })],
          },
          implementation: b.hostCall,
        },
        {
          ...operandCodeEffects,
          this: b.object,
          args: { onFinally: b.any.optional },
          returns: b.any,
          uses: {
            this: [b.runs("Species", b.calls("then"))],
            onFinally: [b.retained, b.called({ afterwards: "TouchesOnlyWhatRowNames", times: "Later", result: "Escapes" })],
          },
          implementation: b.hostCall,
        },
      ],
    },
  }),

  b.class("AsyncDisposableStack", {
    jsType: "AsyncDisposableStackType",
    stateHeap: "InternalFields",
    types: [b.exactType("AsyncDisposableStack")],
    call: requiresNew,
    construct: {
      returns: b.instance("AsyncDisposableStack").fresh,
      capability: "heap",
      implementation: b.hostCall,
    },
    proto: {
      adopt: {
        args: { value: b.any, onDisposeAsync: b.callable },
        returns: b.operand("value"),
        capability: "heap",
        throws: true,
        uses: {
          this: contentsUpdate,
          value: [b.retained],
          onDisposeAsync: disposalOf(b.operand("value")),
        },
        implementation: stackFunction("adopt"),
      },
      defer: {
        args: { onDisposeAsync: b.callable },
        capability: "heap",
        throws: true,
        uses: { this: contentsUpdate, onDisposeAsync: disposalOf() },
        implementation: stackFunction("deferMethod"),
      },
      disposeAsync: [
        {
          returns: newPromiseHolding({ value: b.undefined, reason: b.any }),
          capability: "js",
          uses: { this: contentsUpdate },
          implementation: stackFunction("disposeAsync"),
        },
        {
          this: b.any,
          returns: newPromiseHolding({ value: b.never, reason: b.instance("TypeError").unaliased }),
          capability: "heap",
          throws: true,
          uses: { this: b.inspected },
          implementation: stackFunction("disposeAsync"),
        },
      ],
      disposed: {
        getter: b.bool,
        capability: "none",
        uses: { this: [b.readsContents()] },
        implementation: b.hostCall,
      },
      move: {
        returns: b.instance("AsyncDisposableStack").fresh,
        capability: "heap",
        throws: true,
        uses: { this: contentsUpdate },
        implementation: stackFunction("move"),
      },
      use: {
        ...operandCodeEffects,
        args: { value: b.any },
        returns: b.operand("value"),
        uses: {
          this: contentsUpdate,
          value: [b.runs(b.calls("@@asyncDispose", "@@dispose")), b.retained],
        },
        implementation: stackFunction("use"),
      },
    },
  }),
  b.same("AsyncDisposableStack.prototype.@@asyncDispose", "AsyncDisposableStack.prototype.disposeAsync"),

  // `import "core-js"` replaces it without a test, from 3.22 to 3.37.
  b.replacedByPackages("Promise.try"),
);
