import * as b from "bun-builtins-table";

const fromText = {
  args: { parts: b.any.rest },
  returns: b.any,
  capability: "js",
  throws: true,
  uses: { parts: [b.runs("ToPrimitive"), b.evaluatedAsText] },
  implementation: b.generic,
} satisfies b.FunctionField;

const lazyOwnProperty = (type: b.TableType) =>
  ({
    mutability: "AlwaysLocked",
    getter: type,
    capability: "heap",
    throws: true,
    uses: { this: b.inspected },
    reads: ["NamedProperties"],
    writes: ["ShapeWords", "NamedProperties"],
    implementation: b.generic,
  }) satisfies b.GetterField;

const poisoned = {
  getter: b.any,
  capability: "heap",
  throws: true,
  uses: { this: b.inspected },
  implementation: b.generic,
  setter: {
    value: b.any,
    capability: "none",
    throws: true,
    uses: { this: b.inspected, value: b.inspected },
    implementation: b.generic,
  },
} satisfies b.GetterField;

export default b.declarations(
  b.class("Function", {
    isNeverPristine: true,
    lockedMembers: ["prototype"],
    closedToOtherNames: ["prototype"],
    openNamesOfPrototype: ["demethodize", "unThis"],
    types: [b.viewType("Function"), b.viewType("CallableFunction"), b.viewType("NewableFunction")],
    usesFeature: "Interpreter",
    nativeSymbols: ["callFunctionConstructor", "constructWithFunctionConstructor"],
    call: fromText,
    construct: fromText,
    instance: {
      length: lazyOwnProperty(b.f64),
      name: lazyOwnProperty(b.string),
    },
    proto: {
      arguments: poisoned,
      caller: poisoned,
      apply: {
        this: b.any,
        args: { thisArg: b.any.optional, argumentList: b.any.optional },
        returns: b.callbackResult("this"),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [
            b.called({
              times: "Once",
              this: b.operand("thisArg"),
              spreadArgs: b.ownValues("argumentList"),
              result: "Returned",
            }),
          ],
          thisArg: b.inspected,
          argumentList: [b.runs("OwnValues", "ToPrimitive", b.named("length"))],
        },
        implementation: b.inline("FunctionApply", { otherwise: b.compiled("FunctionPrototype", "apply") }),
      },
      call: {
        this: b.callable,
        args: { thisArg: b.any.optional, passed: b.any.rest },
        returns: b.callbackResult("this"),
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [
            b.called({ times: "Once", this: b.operand("thisArg"), args: [b.operand("passed")], result: "Returned" }),
          ],
          thisArg: b.inspected,
          passed: b.inspected,
        },
        implementation: b.compiled("FunctionPrototype", "call"),
      },
      bind: {
        this: b.callable,
        args: { thisArg: b.any.optional, bound: b.any.rest },
        returns: b.instance("BoundFunction").fresh,
        prototypeSource: "this",
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          this: [b.storedIn(b.resultSlot("target")), b.runs(b.named("name", "length")), b.membersOnlyInspected],
          thisArg: [b.storedIn(b.resultSlot("boundThis"))],
          bound: [b.storedIn(b.resultSlot("boundArguments"))],
        },
        implementation: b.inline("FunctionBind", { max: 4, otherwise: b.hostCall }),
        length: 1,
        boxedCallStub: "FunctionBind",
        nativeSymbols: ["functionProtoFuncBind"],
      },
      toString: {
        mutability: "ConstantUnlessWritten",
        this: b.callable,
        returns: b.string,
        capability: "heap",
        throws: true,
        uses: { this: b.inspected },
        implementation: b.hostCall,
      },
      "@@hasInstance": [
        {
          this: b.instance("BoundFunction"),
          args: { value: b.any },
          returns: b.bool,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: [b.runsTransitively("HasInstance")], value: [b.retained, b.runs("PrototypeChain")] },
          reads: ["InternalFields", "ShapeWords", "NamedProperties", "PrototypeLinks"],
          implementation: b.hostCall,
        },
        {
          args: { value: b.any },
          returns: b.bool,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: [b.runs(b.named("prototype")), b.membersOnlyInspected], value: [b.runs("PrototypeChain")] },
          reads: ["ShapeWords", "NamedProperties", "PrototypeLinks"],
          implementation: b.hostCall,
        },
      ],
    },
  }),

  b.fn("Function.prototype", {
    usesFeature: [],
    args: { operands: b.any.rest },
    returns: b.undefined,
    capability: "none",
    uses: { operands: b.inspected },
    implementation: b.generic,
  }),

  b.class("%BoundFunction%", {
    extends: "Function",
    instanceCppClass: b.cppClass("JSBoundFunction"),
    hasClosureInstances: true,
    hasConstructor: false,
    hasPrototype: false,
    slots: {
      target: b.state("InternalFields", b.callable),
      boundThis: b.state("InternalFields"),
      boundArguments: b.state("InternalFields"),
    },
  }),
);
