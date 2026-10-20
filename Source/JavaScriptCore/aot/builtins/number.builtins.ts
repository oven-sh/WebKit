import * as b from "bun-builtins-table";

const numeric = b.f64.loose.allowBigInt.optional;
const radix = b.integer.loose.optional;
const digitCount = b.integer.loose.optional;

const numberObject = b.instance("Number").or(b.builtin("Number.prototype"));
const booleanObject = b.instance("Boolean").or(b.builtin("Boolean.prototype"));
const bigintObject = b.instance("BigInt");

const wrappedValue = (primitive: b.TableType, wrapper: b.TableType) =>
  [
    {
      this: primitive,
      returns: primitive,
      capability: "none",
      implementation: b.inline("ReturnThis"),
    },
    {
      this: wrapper,
      returns: primitive,
      capability: "heap",
      uses: { this: b.inspected },
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const numberTest = (inlineBuiltin: string) =>
  ({
    args: { value: b.any },
    returns: b.bool,
    capability: "none",
    uses: { value: b.inspected },
    deterministic: true,
    implementation: b.inline(inlineBuiltin),
  }) satisfies b.FunctionField;

const conversionToNumber = {
  returns: b.f64,
  capability: "heap",
  throws: true,
  implementation: b.inline("CallNumberConstructor", { otherwise: b.hostCall }),
} satisfies b.FunctionField;

const inlineToString = {
  this: b.f64,
  returns: b.string,
  capability: "heap",
  implementation: b.inline("NumberToString", { otherwise: b.hostCall }),
} satisfies b.FunctionField;

const hostToString = (receiver: b.TableType) =>
  ({
    this: receiver,
    args: { radix },
    returns: b.string,
    capability: "heap",
    throws: true,
    uses: { this: b.inspected },
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const withDigitCount = (parameter: string) =>
  ({
    this: b.f64.or(numberObject),
    args: { [parameter]: digitCount },
    returns: b.string,
    capability: "heap",
    throws: true,
    uses: { this: b.inspected },
    usesFeature: "NumberPrecisionMethods",
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const truncatedToBits = {
  args: { bits: b.index.loose, bigint: b.bigint.loose },
  returns: b.bigint,
  capability: "heap",
  throws: true,
  implementation: b.hostCall,
} satisfies b.FunctionField;

export default b.declarations(
  b.class("Number", {
    isPrimitiveWrapper: true,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    constructorCallee: "HostFunction",
    types: [b.viewType("Number")],
    constructorTable: b.staticTable("runtime/NumberConstructor.cpp", "numberConstructorTable"),
    prototypeTable: b.staticTable("runtime/NumberPrototype.cpp", "numberPrototypeTable"),
    call: [
      { ...conversionToNumber, args: { value: b.string }, uses: { value: [b.readAsCharacters] } },
      { ...conversionToNumber, args: { value: numeric } },
    ],
    construct: {
      args: { value: numeric },
      returns: b.instance("Number").fresh,
      capability: "heap",
      implementation: b.hostCall,
    },
    static: {
      EPSILON: b.constant(b.checked(Number.EPSILON, "std::numeric_limits<double>::epsilon()")),
      MAX_SAFE_INTEGER: b.constant(b.checked(Number.MAX_SAFE_INTEGER, "9007199254740991.0")),
      MAX_VALUE: b.constant(b.checked(Number.MAX_VALUE, "std::numeric_limits<double>::max()")),
      MIN_SAFE_INTEGER: b.constant(b.checked(Number.MIN_SAFE_INTEGER, "-9007199254740991.0")),
      MIN_VALUE: b.constant(b.checked(Number.MIN_VALUE, "5e-324")),
      NaN: b.constant(b.checked(Number.NaN, "PNaN")),
      NEGATIVE_INFINITY: b.constant(b.checked(Number.NEGATIVE_INFINITY, "-std::numeric_limits<double>::infinity()")),
      POSITIVE_INFINITY: b.constant(b.checked(Number.POSITIVE_INFINITY, "std::numeric_limits<double>::infinity()")),
      isFinite: numberTest("NumberIsFinite"),
      isInteger: numberTest("NumberIsInteger"),
      isNaN: numberTest("NumberIsNaN"),
      isSafeInteger: numberTest("NumberIsSafeInteger"),
    },
    proto: {
      toExponential: withDigitCount("fractionDigits"),
      toFixed: withDigitCount("fractionDigits"),
      toPrecision: withDigitCount("precision"),
      toString: [
        { ...inlineToString, args: { radix: b.undefined.optional }, appendForm: "NumberToStringAppend" },
        { ...inlineToString, args: { radix: b.integer.loose }, throws: true },
        hostToString(numberObject),
      ],
      valueOf: wrappedValue(b.f64, numberObject),
    },
  }),
  b.same("Number.parseFloat", "parseFloat"),
  b.same("Number.parseInt", "parseInt"),

  b.class("BigInt", {
    isPrimitiveWrapper: true,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    types: [b.viewType("BigInt")],
    constructorTable: b.staticTable("runtime/BigIntConstructor.cpp", "bigIntConstructorTable"),
    prototypeTable: b.staticTable("runtime/BigIntPrototype.cpp", "bigIntPrototypeTable"),
    call: {
      args: { value: b.primitiveValue.loose },
      returns: b.bigint,
      capability: "heap",
      throws: true,
      implementation: b.hostCall,
    },
    construct: {
      returns: b.never,
      capability: "heap",
      throws: true,
      implementation: b.hostCall,
    },
    static: {
      asIntN: truncatedToBits,
      asUintN: truncatedToBits,
    },
    proto: {
      toString: hostToString(b.bigint.or(bigintObject)),
      valueOf: wrappedValue(b.bigint, bigintObject),
    },
  }),

  b.class("Boolean", {
    isPrimitiveWrapper: true,
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    constructorCallee: "HostFunction",
    types: [b.viewType("Boolean")],
    prototypeTable: b.staticTable("runtime/BooleanPrototype.cpp", "booleanPrototypeTable"),
    call: {
      args: { value: b.bool.loose },
      returns: b.bool,
      capability: "none",
      uses: { value: b.inspected },
      implementation: b.hostCall,
    },
    construct: {
      args: { value: b.any.optional },
      returns: b.instance("Boolean").fresh,
      capability: "heap",
      uses: { value: b.inspected },
      implementation: b.hostCall,
    },
    proto: {
      toString: {
        this: b.bool.or(booleanObject),
        returns: b.string,
        capability: "heap",
        uses: { this: b.inspected },
        implementation: b.hostCall,
      },
      valueOf: wrappedValue(b.bool, booleanObject),
    },
  }),

  // core-js 2 counts U+180E as white space, finds that the engine does not, and replaces both.
  b.replacedByPackages("Number.parseFloat", "Number.parseInt"),
);
