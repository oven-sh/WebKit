import * as b from "bun-builtins-table";

const internalFields = "InternalFields";
const collectionHeaps = ["CollectionStorage", "CollectionEntries"];
const hostSettings = "ExternalState";

const undeclared = b.otherwiseBy(b.generic);

const internalSlotGetter = {
  getter: b.any,
  capability: "heap",
  throws: true,
  uses: { this: b.inspected },
  implementation: b.generic,
} satisfies b.GetterField;

const requiresNew = {
  returns: b.never,
  capability: "heap",
  throws: true,
  implementation: b.hostCall,
} satisfies b.FunctionField;

const exposedThroughProxy = (slot: string) => [b.everything, b.storedIn(b.resultSlot(slot))];

const iteratorResult = b.layoutOf("IteratorResult").fresh;
const hostIteratorResult = b.ordinaryObjectOf("IteratorResult").fresh;
const advances = [b.readsContents(), b.writesContents()];
const advancesAndExposesContents = [...advances, b.retained];

const iteratorColumns = {
  hasConstructor: false,
  slots: { iterated: b.state(internalFields) },
} satisfies b.ClassOptions;

const lockedIteratorColumns = {
  ...iteratorColumns,
  extends: "Iterator",
  lockedMembers: ["prototype"],
} satisfies b.ClassOptions;

const hostStep = (uses: readonly b.UseSpec[], reads: readonly b.Heap[] = []) =>
  ({
    returns: hostIteratorResult,
    capability: "heap",
    throws: true,
    uses: { this: uses },
    reads,
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const libraryStep = (file: string, name: string, returns: b.TableType) =>
  ({
    returns,
    capability: "js",
    throws: true,
    uses: { this: advancesAndExposesContents },
    implementation: b.compiled(file, name),
  }) satisfies b.FunctionField;

const fromText = {
  args: { parts: b.any.rest },
  returns: b.any,
  capability: "js",
  throws: true,
  uses: { parts: [b.runs("ToPrimitive"), b.evaluatedAsText] },
  implementation: b.generic,
} satisfies b.FunctionField;

const functionKind = (name: string) => [
  ...b.class(`%${name}%`, {
    extends: "Function",
    isNeverPristine: true,
    lockedMembers: ["prototype"],
    closedToOtherNames: ["prototype"],
    usesFeature: "Interpreter",
    nativeSymbols: [`call${name}Constructor`, `construct${name}Constructor`],
    call: fromText,
    construct: fromText,
  }),
  ...b.extend(`%${name}%.prototype`, { usesFeature: [] }),
];

const { localeList, optionBag } = b;

const collatorOptions = [
  "usage",
  "localeMatcher",
  "collation",
  "numeric",
  "caseFirst",
  "sensitivity",
  "ignorePunctuation",
];

const digitOptions = [
  "minimumIntegerDigits",
  "minimumFractionDigits",
  "maximumFractionDigits",
  "minimumSignificantDigits",
  "maximumSignificantDigits",
  "roundingIncrement",
  "roundingMode",
  "roundingPriority",
  "trailingZeroDisplay",
];

const durationUnits = [
  "years",
  "months",
  "weeks",
  "days",
  "hours",
  "minutes",
  "seconds",
  "milliseconds",
  "microseconds",
  "nanoseconds",
];

const durationFormatOptions = [
  "localeMatcher",
  "numberingSystem",
  "style",
  ...durationUnits.flatMap(unit => [unit, `${unit}Display`]),
  "fractionalDigits",
];

const localeOptions = [
  "language",
  "script",
  "region",
  "variants",
  "calendar",
  "collation",
  "firstDayOfWeek",
  "hourCycle",
  "caseFirst",
  "numeric",
  "numberingSystem",
];

const supportedLocalesOf = {
  args: { locales: b.any.optional, options: b.any.optional },
  returns: b.instance("Array").holding({ element: b.string }).fresh,
  capability: "js",
  throws: true,
  runsOnlyOperandCode: true,
  uses: { locales: localeList, options: optionBag(["localeMatcher"]) },
  implementation: b.hostCall,
} satisfies b.FunctionField;

const intlServiceCreation = (name: string, optionNames: readonly string[]) =>
  ({
    args: { locales: b.any.optional, options: b.any.optional },
    returns: b.instance(`Intl.${name}`).fresh,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: { locales: localeList, options: optionBag(optionNames) },
    reads: [hostSettings],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const intlService = (
  name: string,
  tablePrefix: string,
  usesFeature: readonly b.FeatureUseSpec[],
  optionNames: readonly string[],
  call: b.FunctionField = requiresNew,
) =>
  b.class(`Intl.${name}`, {
    usesFeature,
    nativeSymbols: [`callIntl${name}`, `constructIntl${name}`],
    constructorTable: b.staticTable(`runtime/Intl${name}Constructor.cpp`, `${tablePrefix}ConstructorTable`),
    prototypeTable: b.staticTable(`runtime/Intl${name}Prototype.cpp`, `${tablePrefix}PrototypeTable`),
    call,
    construct: intlServiceCreation(name, optionNames),
    static: { supportedLocalesOf },
  });

const temporalClass = (name: string, prototypeTable: string, proto: b.ClassOptions["proto"] = {}) =>
  b.class(`Temporal.${name}`, {
    usesFeature: "Temporal",
    nativeSymbols: [`callTemporal${name}`, `constructTemporal${name}`],
    constructorTable: b.staticTable(`runtime/Temporal${name}Constructor.cpp`, `temporal${name}ConstructorTable`),
    prototypeTable: b.staticTable(`runtime/Temporal${name}Prototype.cpp`, prototypeTable),
    proto,
  });

const webAssemblyTable = (file: string, table: string) => b.staticTable(`wasm/js/${file}.cpp`, table);

const webAssemblyClass = (name: string, options: b.ClassOptions = {}) =>
  b.class(`WebAssembly.${name}`, {
    nativeSymbols: [`callJSWebAssembly${name}`, `constructJSWebAssembly${name}`],
    ...options,
  });

export default b.declarations(
  b.class("Proxy", {
    jsType: "ProxyObjectType",
    hasPrototype: false,
    lockedMembers: ["constructor"],
    mutability: "AlwaysLocked",
    slots: { target: b.state(internalFields), handler: b.state(internalFields) },
    call: requiresNew,
    construct: {
      args: { target: b.object, handler: b.object },
      returns: b.instance("Proxy"),
      capability: "js",
      runsOnlyOperandCode: true,
      uses: { target: exposedThroughProxy("target"), handler: exposedThroughProxy("handler") },
      implementation: b.hostCall,
    },
    static: {
      revocable: {
        args: { target: b.object, handler: b.object },
        returns: b.object.fresh,
        capability: "js",
        runsOnlyOperandCode: true,
        uses: { target: [b.everything], handler: [b.everything] },
        implementation: b.hostCall,
      },
    },
  }),

  b.class("%ArrayIterator%", {
    ...lockedIteratorColumns,
    proto: { next: libraryStep("ArrayIteratorPrototype", "next", iteratorResult) },
  }),
  b.class("%MapIterator%", {
    ...lockedIteratorColumns,
    proto: { next: hostStep(advancesAndExposesContents, collectionHeaps) },
  }),
  b.class("%SetIterator%", {
    ...lockedIteratorColumns,
    proto: { next: hostStep(advancesAndExposesContents, collectionHeaps) },
  }),
  b.class("%StringIterator%", {
    ...lockedIteratorColumns,
    proto: { next: hostStep(advances) },
  }),
  b.class("%RegExpStringIterator%", {
    ...lockedIteratorColumns,
    proto: {
      // The regular expression that it steps has been in native memory since matchAll, where its exec is read and called like anything that native code holds.
      next: {
        returns: hostIteratorResult,
        capability: "js",
        throws: true,
        uses: { this: advances },
        implementation: b.hostCall,
      },
    },
  }),
  b.class("%IteratorHelper%", {
    ...lockedIteratorColumns,
    prototypeTable: b.staticTable("runtime/JSIteratorHelperPrototype.cpp", "jsIteratorHelperPrototypeTable"),
    proto: {
      next: libraryStep("JSIteratorHelperPrototype", "next", iteratorResult),
      return: libraryStep("JSIteratorHelperPrototype", "return", iteratorResult),
    },
  }),
  b.class("%WrapForValidIterator%", {
    ...lockedIteratorColumns,
    proto: {
      next: libraryStep("WrapForValidIteratorPrototype", "next", b.any),
      return: libraryStep("WrapForValidIteratorPrototype", "return", b.any),
    },
  }),
  b.class("%AsyncIterator%", {
    ...iteratorColumns,
    isNeverPristine: true,
    lockedMembers: ["prototype"],
    proto: {
      "@@asyncIterator": {
        this: b.any,
        returns: b.operand("this"),
        capability: "none",
        uses: { this: b.inspected },
        implementation: b.inline("ReturnThis"),
      },
      "@@asyncDispose": {
        this: b.any,
        returns: b.instance("Promise").holding({ value: b.undefined, reason: b.any }).fresh,
        capability: "js",
        throws: true,
        uses: { this: [b.runs(b.calls("return"))] },
        implementation: b.compiled("AsyncIteratorPrototype", "asyncDispose"),
      },
    },
  }),
  b.class("%AsyncFromSyncIterator%", {
    ...lockedIteratorColumns,
    proto: { next: undeclared, return: undeclared, throw: undeclared },
  }),
  b.class("%IntlSegments%", {
    hasConstructor: false,
    prototypeTable: b.staticTable("runtime/IntlSegmentsPrototype.cpp", "segmentsPrototypeTable"),
    proto: { "@@iterator": undeclared },
  }),
  b.class("%IntlSegmentIterator%", {
    ...iteratorColumns,
    extends: "Iterator",
    prototypeTable: b.staticTable("runtime/IntlSegmentIteratorPrototype.cpp", "segmentIteratorPrototypeTable"),
    proto: { next: hostStep(advances) },
  }),

  functionKind("AsyncFunction"),
  functionKind("GeneratorFunction"),
  functionKind("AsyncGeneratorFunction"),

  intlService(
    "Collator",
    "collator",
    ["IntlCollation"],
    collatorOptions,
    intlServiceCreation("Collator", collatorOptions),
  ),
  intlService("DisplayNames", "displayNames", ["IntlDisplayNames"], [
    "localeMatcher",
    "style",
    "type",
    "fallback",
    "languageDisplay",
  ]),
  intlService("DurationFormat", "durationFormat", ["IntlNumberFormat", "IntlUnits"], durationFormatOptions),
  intlService("ListFormat", "listFormat", ["IntlOther"], ["localeMatcher", "type", "style"]),
  intlService("PluralRules", "pluralRules", ["IntlOther", "IntlPluralRules", "IntlNumberFormat"], [
    "localeMatcher",
    "type",
    "notation",
    "compactDisplay",
    ...digitOptions,
  ]),
  intlService("RelativeTimeFormat", "relativeTimeFormat", ["IntlOther", "IntlPluralRules", "IntlNumberFormat"], [
    "localeMatcher",
    "numberingSystem",
    "style",
    "numeric",
  ]),
  intlService("Segmenter", "segmenter", ["IntlSegmenter"], ["localeMatcher", "granularity"]),
  b.class("Intl.Locale", {
    usesFeature: ["IntlOther", "IntlLocale"],
    nativeSymbols: ["callIntlLocale", "constructIntlLocale"],
    prototypeTable: b.staticTable("runtime/IntlLocalePrototype.cpp", "localePrototypeTable"),
    call: requiresNew,
    construct: {
      args: { tag: b.union(b.string, b.object), options: b.any.optional },
      returns: b.instance("Intl.Locale").fresh,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { tag: [b.readAsLocale, b.runs("ToPrimitive")], options: optionBag(localeOptions) },
      implementation: b.hostCall,
    },
  }),
  b.extend("Intl.Locale.prototype.getCollations", { usesFeature: "IntlCollation" }),

  b.namespace("Temporal", {}, { table: b.staticTable("runtime/TemporalObject.cpp", "temporalObjectTable") }),
  b.namespace(
    "Temporal.Now",
    {},
    { usesFeature: "Temporal", table: b.staticTable("runtime/TemporalNow.cpp", "temporalNowTable") },
  ),
  temporalClass("Duration", "durationPrototypeTable", {
    toLocaleString: {
      args: { locales: b.any.optional, options: b.any.optional },
      returns: b.string,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { this: b.inspected, locales: localeList, options: optionBag(durationFormatOptions) },
      reads: [hostSettings],
      usesFeature: ["IntlNumberFormat", "IntlUnits"],
      implementation: b.hostCall,
    },
  }),
  temporalClass("Instant", "instantPrototypeTable"),
  temporalClass("PlainDate", "plainDatePrototypeTable"),
  temporalClass("PlainDateTime", "plainDateTimePrototypeTable"),
  temporalClass("PlainMonthDay", "plainMonthDayPrototypeTable"),
  temporalClass("PlainTime", "plainTimePrototypeTable"),
  temporalClass("PlainYearMonth", "plainYearMonthPrototypeTable"),
  temporalClass("ZonedDateTime", "zonedDateTimePrototypeTable"),

  b.namespace(
    "WebAssembly",
    {
      JSTag: {
        getter: b.any,
        capability: "heap",
        throws: true,
        implementation: b.generic,
        nativeSymbols: ["webAssemblyGetterJSTag"],
      },
      compileStreaming: undeclared,
      instantiateStreaming: undeclared,
      promising: undeclared,
    },
    { usesFeature: "WebAssembly", table: webAssemblyTable("JSWebAssembly", "webAssemblyTable") },
  ),
  b.extend("WebAssembly.compileStreaming", {
    usesFeature: "WebAssembly",
    nativeSymbols: ["webAssemblyCompileStreamingFunc"],
  }),
  b.extend("WebAssembly.instantiateStreaming", {
    usesFeature: "WebAssembly",
    nativeSymbols: ["webAssemblyInstantiateStreamingFunc"],
  }),
  b.extend("WebAssembly.promising", { nativeSymbols: ["webAssemblyPromisingFunc"] }),
  webAssemblyClass("Exception", {
    prototypeTable: webAssemblyTable("WebAssemblyExceptionPrototype", "prototypeTableWebAssemblyException"),
  }),
  webAssemblyClass("Global", {
    prototypeTable: webAssemblyTable("WebAssemblyGlobalPrototype", "prototypeGlobalWebAssemblyGlobal"),
    proto: {
      value: {
        ...internalSlotGetter,
        setter: {
          value: b.any,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: b.inspected, value: [b.retained, b.runs("ToPrimitive")] },
          implementation: b.generic,
        },
      },
    },
  }),
  webAssemblyClass("Instance", { proto: { exports: internalSlotGetter } }),
  webAssemblyClass("Memory", {
    prototypeTable: webAssemblyTable("WebAssemblyMemoryPrototype", "prototypeTableWebAssemblyMemory"),
    proto: { toFixedLengthBuffer: undeclared, toResizableBuffer: undeclared },
  }),
  webAssemblyClass("Module", {
    constructorTable: webAssemblyTable("WebAssemblyModuleConstructor", "constructorTableWebAssemblyModule"),
  }),
  webAssemblyClass("Suspending", {
    nativeSymbols: ["callWebAssemblySuspending", "constructWebAssemblySuspending"],
  }),
  webAssemblyClass("Table", {
    prototypeTable: webAssemblyTable("WebAssemblyTablePrototype", "prototypeTableWebAssemblyTable"),
  }),
  webAssemblyClass("Tag"),
  webAssemblyClass("CompileError", { extends: "Error" }),
  webAssemblyClass("LinkError", { extends: "Error" }),
  webAssemblyClass("RuntimeError", { extends: "Error" }),
  webAssemblyClass("SuspendError", { extends: "Error" }),

  b.extend("console.takeHeapSnapshot", { nativeSymbols: ["consoleProtoFuncTakeHeapSnapshot"] }),
);
