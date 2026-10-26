import * as b from "bun-builtins-table";

const receiver = b.string.loose.refusing(b.null);
const position = b.integer.loose.optional;
const characters = [b.readAsCharacters];
const maximumLength = 2 ** 31 - 1;
const propertyLookup = ["ObjectFields", "NamedProperties", "ShapeWords", "PrototypeLinks"];

const { localeList } = b;

const collatorOptions = b.optionBag([
  "usage",
  "localeMatcher",
  "collation",
  "numeric",
  "caseFirst",
  "sensitivity",
  "ignorePunctuation",
]);

const stringValue = [
  {
    this: b.string,
    returns: b.string,
    capability: "none",
    implementation: b.inline("ReturnThis"),
  },
  {
    this: b.instance("String"),
    returns: b.string,
    capability: "none",
    uses: { this: b.inspected },
    implementation: b.hostCall,
  },
] satisfies b.FunctionField[];

const indexedRead = (inlineBuiltin: string, parameter: string, returns: b.TableType) =>
  ({
    this: receiver,
    args: { [parameter]: position },
    returns,
    capability: "heap",
    throws: b.propagated,
    deterministic: true,
    uses: { this: characters },
    implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const range = (inlineBuiltin: string, secondParameter: string) =>
  ({
    this: receiver,
    args: { start: position, [secondParameter]: position },
    returns: b.string,
    capability: "heap",
    throws: b.propagated,
    deterministic: true,
    uses: { this: characters },
    substringOf: "this",
    implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const substringTest = (inlineBuiltin: string, positionParameter: string, columns: b.EntryColumns = {}) =>
  [
    {
      this: receiver,
      args: { searchString: b.object, [positionParameter]: b.any.optional },
      returns: b.bool,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: {
        this: characters,
        searchString: [b.runs(b.named("@@match"), "ToPrimitive"), b.membersOnlyInspected],
        [positionParameter]: [b.runs("ToPrimitive")],
      },
      reads: propertyLookup,
      implementation: b.hostCall,
    },
    {
      ...columns,
      this: receiver,
      args: { searchString: b.string.loose, [positionParameter]: position },
      returns: b.bool,
      capability: "heap",
      throws: b.propagated,
      deterministic: true,
      uses: { this: characters, searchString: characters },
      implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
    },
  ] satisfies b.FunctionField[];

const indexSearch = (inlineBuiltin: string, startingPosition: b.TableType) =>
  ({
    this: receiver,
    args: { searchString: b.string.loose, position: startingPosition },
    returns: b.int(-1, maximumLength),
    capability: "heap",
    throws: b.propagated,
    deterministic: true,
    uses: { this: characters, searchString: characters },
    implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

// A text of a length that every string may have is never refused.
const padded = (inlineBuiltin: string, nativeSymbol: string) =>
  [
    {
      this: receiver,
      args: { maxLength: b.int(-(2 ** 31), 2 ** 30), fillString: b.string.optional },
      returns: b.string,
      capability: "heap",
      throws: b.propagated,
      isSpecialCase: true,
      uses: { fillString: characters },
      implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
    },
    {
      this: receiver,
      args: { maxLength: b.integer.loose, fillString: b.string.optional },
      returns: b.string,
      capability: "heap",
      throws: true,
      deterministic: true,
      uses: { fillString: characters },
      usesFeature: "StringPadding",
      nativeSymbols: [nativeSymbol],
      implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
    },
    {
      this: receiver,
      args: { maxLength: b.integer.loose, fillString: b.any },
      returns: b.string,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { fillString: [b.runs("ToPrimitive")] },
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const converted = (implementation: b.ImplementationSpec) =>
  ({
    this: receiver,
    returns: b.string,
    capability: "heap",
    throws: b.propagated,
    deterministic: true,
    implementation,
  }) satisfies b.FunctionField;

const trimmed = (inlineBuiltin: string) =>
  ({
    this: receiver,
    returns: b.string,
    capability: "heap",
    throws: b.propagated,
    deterministic: true,
    uses: { this: characters },
    substringOf: "this",
    implementation: b.inline(inlineBuiltin),
  }) satisfies b.FunctionField;

const localeCase = (nativeSymbol: string) =>
  [
    {
      this: receiver,
      args: { locales: b.hostLocaleOrWellFormedTag },
      returns: b.string,
      capability: "heap",
      throws: b.propagated,
      isSpecialCase: true,
      uses: { this: characters },
      reads: ["ExternalState"],
      implementation: b.hostCall,
    },
    {
      this: receiver,
      args: { locales: b.any.optional },
      returns: b.string,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { locales: localeList },
      reads: ["ExternalState"],
      usesFeature: "StringLocaleCase",
      nativeSymbols: [nativeSymbol],
      implementation: b.hostCall,
    },
  ] satisfies b.FunctionField[];

const htmlElement = {
  mutability: "ConstantUnlessWritten",
  this: receiver,
  returns: b.string,
  capability: "heap",
  throws: true,
  uses: { this: characters },
  usesFeature: "StringHTML",
  implementation: b.hostCall,
} satisfies b.FunctionField;

const htmlElementWithAttribute = (attribute: string) =>
  ({
    ...htmlElement,
    args: { [attribute]: b.string.loose },
    uses: { this: characters, [attribute]: characters },
  }) satisfies b.FunctionField;

export default b.declarations(
  b.class("String", {
    isPrimitiveWrapper: true,
    definesCharactersOnNewInstances: true,
    constructorCallee: "HostFunction",
    lockedMembers: ["constructor", "prototype"],
    mutability: "AlwaysLocked",
    types: [b.viewType("String")],
    constructorTable: b.staticTable("runtime/StringConstructor.cpp", "stringConstructorTable"),
    prototypeTable: b.staticTable("runtime/StringPrototype.cpp", "stringPrototypeTable"),
    call: [
      {
        args: { value: b.primitiveValue.optional },
        returns: b.string,
        capability: "heap",
        throws: b.propagated,
        appendForm: "CallStringConstructorAppend",
        implementation: b.inline("CallStringConstructor", { otherwise: b.hostCall }),
      },
      {
        args: { value: b.string.loose },
        returns: b.string,
        capability: "heap",
        throws: b.propagated,
        implementation: b.inline("CallStringConstructor", { otherwise: b.hostCall }),
      },
    ],
    construct: {
      args: { value: b.string.loose.optional },
      returns: b.instance("String").fresh,
      capability: "heap",
      throws: b.propagated,
      implementation: b.hostCall,
    },
    static: {
      fromCharCode: {
        args: { codeUnits: b.f64.loose.rest },
        returns: b.string,
        capability: "heap",
        throws: b.propagated,
        deterministic: true,
        implementation: b.inline("StringFromCharCode", { max: 1, otherwise: b.hostCall }),
      },
      fromCodePoint: {
        args: { codePoints: b.any.rest },
        returns: b.string,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { codePoints: [b.runs("ToPrimitive")] },
        implementation: b.inline("StringFromCodePoint", { max: 1, otherwise: b.hostCall }),
      },
      raw: {
        args: { template: b.object.loose.refusing(b.null), substitutions: b.any.rest },
        returns: b.string,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: {
          template: [b.runsTransitively("OwnValues", "ToPrimitive", b.named("raw", "length"))],
          substitutions: [b.runs("ToPrimitive")],
        },
        implementation: b.hostCall,
      },
    },
    instance: {
      length: {
        this: b.string,
        getter: b.int(0, maximumLength),
        capability: "none",
        uses: { this: characters },
        implementation: b.inline("StringLength"),
      },
    },
    proto: {
      at: {
        ...indexedRead("StringAt", "index", b.union(b.string, b.undefined)),
        mutability: "ConstantUnlessWritten",
        substringOf: "this",
      },
      charAt: { ...indexedRead("StringCharAt", "pos", b.string), substringOf: "this" },
      charCodeAt: { ...indexedRead("StringCharCodeAt", "pos", b.f64), privateAlias: true },
      codePointAt: indexedRead("StringCodePointAt", "pos", b.union(b.int(0, 0x10ffff), b.undefined)),
      concat: {
        this: receiver,
        args: { strings: b.string.loose.rest },
        returns: b.string,
        capability: "heap",
        throws: true,
        deterministic: true,
        implementation: b.inline("StringConcat", { max: 2, otherwise: b.hostCall }),
      },
      endsWith: substringTest("StringEndsWith", "endPosition", { privateAlias: true }),
      includes: substringTest("StringIncludes", "position"),
      startsWith: substringTest("StringStartsWith", "position", {
        length: 1,
        boxedCallStub: "StringStartsWith",
        nativeSymbols: ["stringProtoFuncStartsWith"],
      }),
      indexOf: {
        ...indexSearch("StringIndexOf", position),
        length: 1,
        boxedCallStub: "StringIndexOf",
        nativeSymbols: ["stringProtoFuncIndexOf"],
      },
      lastIndexOf: {
        ...indexSearch("StringLastIndexOf", b.f64.loose.optional),
        usesFeature: "StringLastIndexOf",
        nativeSymbols: ["stringProtoFuncLastIndexOf"],
      },
      isWellFormed: {
        this: receiver,
        returns: b.bool,
        capability: "heap",
        throws: b.propagated,
        deterministic: true,
        uses: { this: characters },
        usesFeature: "StringWellFormed",
        nativeSymbols: ["stringProtoFuncIsWellFormed"],
        implementation: b.inline("StringIsWellFormed", { otherwise: b.hostCall }),
      },
      localeCompare: [
        {
          this: receiver,
          args: { that: b.string.loose, locales: b.hostLocaleOrWellFormedTag, options: b.undefined.optional },
          returns: b.int(-1, 1),
          capability: "heap",
          throws: b.propagated,
          isSpecialCase: true,
          uses: { this: characters, that: characters },
          reads: ["ExternalState"],
          implementation: b.inline("StringLocaleCompare", { otherwise: b.hostCall }),
        },
        {
          this: receiver,
          args: { that: b.string.loose, locales: b.any.optional, options: b.any.optional },
          returns: b.int(-1, 1),
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: characters, that: characters, locales: localeList, options: collatorOptions },
          reads: ["ExternalState"],
          usesFeature: "IntlCollation",
          nativeSymbols: ["stringProtoFuncLocaleCompare"],
          implementation: b.inline("StringLocaleCompare", { otherwise: b.hostCall }),
        },
      ],
      normalize: [
        {
          this: receiver,
          args: { form: b.union(b.undefined, b.enumeration(["NFC", "NFD", "NFKC", "NFKD"])).optional },
          returns: b.string,
          capability: "heap",
          throws: b.propagated,
          isSpecialCase: true,
          uses: { form: characters },
          implementation: b.inline("StringNormalize", { otherwise: b.hostCall }),
        },
        {
          this: receiver,
          args: { form: b.string.loose.optional },
          returns: b.string,
          capability: "heap",
          throws: true,
          deterministic: true,
          uses: { form: characters },
          usesFeature: "StringNormalize",
          nativeSymbols: ["stringProtoFuncNormalize"],
          implementation: b.inline("StringNormalize", { otherwise: b.hostCall }),
        },
      ],
      padEnd: padded("StringPadEnd", "stringProtoFuncPadEnd"),
      padStart: padded("StringPadStart", "stringProtoFuncPadStart"),
      repeat: {
        this: receiver,
        args: { count: b.integer.loose },
        returns: b.string,
        capability: "heap",
        throws: true,
        deterministic: true,
        usesFeature: "StringRepeat",
        nativeSymbols: ["stringProtoFuncRepeat"],
        implementation: b.inline("StringRepeat", { otherwise: b.hostCall }),
      },
      slice: {
        ...range("StringSlice", "end"),
        appendForm: "StringSliceAppend",
        length: 2,
        boxedCallStub: "StringSlice",
        nativeSymbols: ["stringProtoFuncSlice"],
      },
      substr: { ...range("StringSubstr", "length"), privateAlias: true },
      substring: { ...range("StringSubstring", "end"), appendForm: "StringSubstringAppend" },
      toLocaleLowerCase: localeCase("stringProtoFuncToLocaleLowerCase"),
      toLocaleUpperCase: localeCase("stringProtoFuncToLocaleUpperCase"),
      toLowerCase: {
        ...converted(b.inline("StringToLowerCase")),
        length: 0,
        boxedCallStub: "StringToLowerCase",
        nativeSymbols: ["stringProtoFuncToLowerCase"],
      },
      toUpperCase: converted(b.inline("StringToUpperCase")),
      toWellFormed: {
        ...converted(b.inline("StringToWellFormed", { otherwise: b.hostCall })),
        usesFeature: "StringWellFormed",
        nativeSymbols: ["stringProtoFuncToWellFormed"],
      },
      trim: {
        ...trimmed("StringTrim"),
        length: 0,
        boxedCallStub: "StringTrim",
        nativeSymbols: ["stringProtoFuncTrim"],
      },
      trimEnd: trimmed("StringTrimEnd"),
      trimStart: trimmed("StringTrimStart"),
      anchor: htmlElementWithAttribute("name"),
      big: htmlElement,
      blink: htmlElement,
      bold: htmlElement,
      fixed: htmlElement,
      fontcolor: htmlElementWithAttribute("color"),
      fontsize: htmlElementWithAttribute("size"),
      italics: htmlElement,
      link: htmlElementWithAttribute("url"),
      small: htmlElement,
      strike: htmlElement,
      sub: htmlElement,
      sup: htmlElement,
      toString: stringValue,
      valueOf: stringValue,
      "@@iterator": {
        this: receiver,
        returns: b.instance("StringIterator").fresh,
        capability: "heap",
        uses: { this: [b.storedIn(b.resultSlot("iterated"))] },
        iterates: { operand: "this", kind: "Values" },
        implementation: b.hostCall,
      },
    },
  }),
  b.same("String.prototype.trimLeft", "String.prototype.trimStart"),
  b.same("String.prototype.trimRight", "String.prototype.trimEnd"),

  // core-js 2 counts U+180E as white space, finds that the engine does not, and replaces all five.
  b.replacedByPackages(
    "String.prototype.trim",
    "String.prototype.trimEnd",
    "String.prototype.trimLeft",
    "String.prototype.trimRight",
    "String.prototype.trimStart",
  ),
);
