import * as b from "bun-builtins-table";

const internalFields = "InternalFields";
const collectionHeaps = ["CollectionStorage", "CollectionEntries"];
const hostSettings = "ExternalState";

const undeclared = b.otherwiseBy(b.generic);

const internalSlotGetter = {
  this: b.any.refusing(b.null),
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

// Native code makes a primitive of a Date, of a wrapper, of an array or of a URL without running code of the program. What it makes depends on what that object holds.
const readByConversions = [
  hostSettings,
  internalFields,
  "Elements",
  "ArrayStorage",
  "TypedArrayElements",
  "TypedArrayStorage",
  "NamedProperties",
  "ObjectFields",
  "ShapeWords",
  "PrototypeLinks",
];

// It looks at its receiver and at nothing else. A receiver of another class makes it throw.
const answeredFromReceiver = (returns: b.TableType) =>
  ({
    this: b.any,
    returns,
    capability: "heap",
    throws: true,
    uses: { this: b.inspected },
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

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

const functionKind = (name: string, pristineTest: "Mark" | "ClosureAsCreated" = "Mark") => [
  ...b.class(`%${name}%`, {
    extends: "Function",
    isNeverPristine: true,
    pristineTest,
    lockedMembers: ["prototype"],
    closedToOtherNames: ["prototype"],
    // As for Function: the compiler follows no function made from text.
    namesDefinedOnNewInstances: [],
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

// A method of a service of Intl keeps no operand and calls none. It makes primitives of them, and answers a string or new objects and arrays that hold strings, numbers and booleans.
// What native code makes of a Date, of a wrapper or of an array depends on what that holds.
const convertedToPrimitive = [b.runs("ToPrimitive"), b.readsContents()];
const newStringArray = b.instance("Array").holding({ element: b.string }).fresh;
const newPlainData = b.plainData.fresh;
const intlMethod = (operands: Readonly<Record<string, readonly b.UseSpec[]>>, returns: b.TableType) =>
  ({
    this: b.any,
    args: Object.fromEntries(Object.keys(operands).map(name => [name, b.any])),
    returns,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: { this: b.inspected, ...operands },
    reads: readByConversions,
    implementation: b.hostCall,
  }) satisfies b.FunctionField;
const formatAndParts = (operands: Readonly<Record<string, readonly b.UseSpec[]>>) => ({
  format: intlMethod(operands, b.string),
  formatToParts: intlMethod(operands, newPlainData),
});

const intlService = (
  name: string,
  tablePrefix: string,
  usesFeature: readonly b.FeatureUseSpec[],
  optionNames: readonly string[],
  proto: NonNullable<b.ClassOptions["proto"]>,
  call: b.FunctionField = requiresNew,
) =>
  b.class(`Intl.${name}`, {
    usesFeature,
    prototypeUsesFeature: true,
    nativeSymbols: [`callIntl${name}`, `constructIntl${name}`],
    constructorTable: b.staticTable(`runtime/Intl${name}Constructor.cpp`, `${tablePrefix}ConstructorTable`),
    prototypeTable: b.staticTable(`runtime/Intl${name}Prototype.cpp`, `${tablePrefix}PrototypeTable`),
    call,
    construct: intlServiceCreation(name, optionNames),
    static: { supportedLocalesOf },
    proto: { resolvedOptions: answeredFromReceiver(newPlainData), ...proto },
  });

// TEMPORAL. An object of Temporal holds numbers and the names of a calendar and of a time zone: no reference to anything (no class in runtime/Temporal*.h has a WriteBarrier).
// So no function keeps an operand, and none calls one. A receiver of another class makes a function throw.
// An operand is a primitive, an object of Temporal, whose numbers are copied, or a bag: its members are read under these names, an inherited one and a getter too, and converted to primitives.
// A member may be a bag again (relativeTo, plainTime).
type TemporalClass = "Duration" | "Instant" | "PlainDate" | "PlainDateTime" | "PlainMonthDay" | "PlainTime" | "PlainYearMonth" | "ZonedDateTime";

const temporalUnits = ["year", "month", "week", "day", "hour", "minute", "second", "millisecond", "microsecond", "nanosecond"];
const namesReadByTemporal = [
  ...temporalUnits,
  ...durationUnits,
  "monthCode",
  "era",
  "eraYear",
  "calendar",
  "timeZone",
  "offset",
  "plainTime",
  "overflow",
  "disambiguation",
  "largestUnit",
  "smallestUnit",
  "unit",
  "roundingIncrement",
  "roundingMode",
  "relativeTo",
  "fractionalSecondDigits",
  "calendarName",
  "timeZoneName",
  "direction",
];
const readByTemporal = [...optionBag(namesReadByTemporal), b.readsContents()];
const newTemporal = (name: TemporalClass) => b.instance(`Temporal.${name}`).fresh;

const temporalGetter = (getter: b.TableType) => ({ ...internalSlotGetter, getter }) satisfies b.GetterField;

const temporalOfReceiver = answeredFromReceiver;

// `required` is the length of the function. One more operand is read where there is one: the options.
const temporalOfOperands = (required: 0 | 1 | 2, returns: b.TableType) =>
  ({
    this: b.any,
    args:
      required === 2
        ? { one: b.any, two: b.any, options: b.any.optional }
        : { item: required ? b.any : b.any.optional, options: b.any.optional },
    returns,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses:
      required === 2
        ? { this: b.inspected, one: readByTemporal, two: readByTemporal, options: readByTemporal }
        : { this: b.inspected, item: readByTemporal, options: readByTemporal },
    reads: readByConversions,
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

// The text of a value of Temporal in its usual form.
const temporalText = temporalOfReceiver(b.string);

const numberOrUndefined = b.union(b.f64, b.undefined);
const calendarDateGetters = {
  calendarId: temporalGetter(b.string),
  era: temporalGetter(b.union(b.string, b.undefined)),
  eraYear: temporalGetter(numberOrUndefined),
  year: temporalGetter(b.f64),
  month: temporalGetter(b.f64),
  monthCode: temporalGetter(b.string),
  daysInMonth: temporalGetter(b.f64),
  daysInYear: temporalGetter(b.f64),
  monthsInYear: temporalGetter(b.f64),
  inLeapYear: temporalGetter(b.bool),
};
const dayGetters = {
  day: temporalGetter(b.f64),
  dayOfWeek: temporalGetter(b.f64),
  dayOfYear: temporalGetter(b.f64),
  weekOfYear: temporalGetter(numberOrUndefined),
  yearOfWeek: temporalGetter(numberOrUndefined),
  daysInWeek: temporalGetter(b.f64),
};
const timeGetters = Object.fromEntries(
  ["hour", "minute", "second", "millisecond", "microsecond", "nanosecond"].map(name => [name, temporalGetter(b.f64)]),
);
const epochGetters = { epochMilliseconds: temporalGetter(b.f64), epochNanoseconds: temporalGetter(b.bigint) };

// The members that most classes have. Each answers a new object of the class, a duration or a primitive.
const temporalArithmetic = (name: TemporalClass) => ({
  add: temporalOfOperands(1, newTemporal(name)),
  subtract: temporalOfOperands(1, newTemporal(name)),
});
const temporalDifferences = {
  until: temporalOfOperands(1, newTemporal("Duration")),
  since: temporalOfOperands(1, newTemporal("Duration")),
};
const conversionsOfTemporal = (...names: readonly TemporalClass[]) =>
  Object.fromEntries(names.map(name => [`to${name}`, temporalOfReceiver(newTemporal(name))]));

const temporalClass = (
  name: TemporalClass,
  prototypeTable: string,
  isCompared: boolean,
  proto: NonNullable<b.ClassOptions["proto"]>,
  statics: NonNullable<b.ClassOptions["static"]> = {},
) =>
  b.class(`Temporal.${name}`, {
    usesFeature: "Temporal",
    prototypeUsesFeature: true,
    nativeSymbols: [`callTemporal${name}`, `constructTemporal${name}`],
    constructorTable: b.staticTable(`runtime/Temporal${name}Constructor.cpp`, `temporal${name}ConstructorTable`),
    prototypeTable: b.staticTable(`runtime/Temporal${name}Prototype.cpp`, prototypeTable),
    constructorOnlyReadsPrototypeOfNewTarget: true,
    call: requiresNew,
    // Each operand is converted to a number, a BigInt or a string.
    construct: {
      args: { parts: b.any.rest },
      returns: newTemporal(name),
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      uses: { parts: convertedToPrimitive },
      reads: readByConversions,
      implementation: b.hostCall,
    },
    static: {
      from: temporalOfOperands(1, newTemporal(name)),
      ...(isCompared ? { compare: temporalOfOperands(2, b.int(-1, 1)) } : {}),
      ...statics,
    },
    proto: {
      // It throws whatever it is called on: such a value is not to be compared with `<`.
      valueOf: temporalOfReceiver(b.never),
      toJSON: temporalText,
      // With options it reads them, which may run their code.
      toString: [{ ...temporalText, args: { options: b.undefined.optional } }, temporalOfOperands(0, b.string)],
      ...proto,
    },
  });

const webAssemblyTable = (file: string, table: string) => b.staticTable(`wasm/js/${file}.cpp`, table);

// The getters throw for a receiver that is no instance of their class.
const webAssemblyGetter = (getter: b.TableType, uses: readonly b.UseSpec[] = b.inspected) =>
  ({ ...internalSlotGetter, getter, uses: { this: uses } }) satisfies b.GetterField;

// A global holds a number, a BigInt or a reference: what the constructor or the setter was handed, or what the code of an instance has stored.
const globalValueCell = "WebAssembly.Global.value";
const exportsCell = "WebAssembly.Instance.exports";
const memoryBufferCell = "WebAssembly.Memory.buffer";
const tableElementCell = "WebAssembly.Table.element";
const exceptionPayloadCell = "WebAssembly.Exception.payload";
const keptInGlobal = b.storedIn(b.nativeCell(globalValueCell));
// The code of an instance reads and calls what a table holds.
const keptInTable = [b.retained, b.storedIn(b.nativeCell(tableElementCell))];
const typedArrayStorage = "TypedArrayStorage";
const addressOrCount = b.union(b.f64, b.bigint);
const answersValueOfGlobal = [b.returnedFrom(globalValueCell)];
const memoryBuffer = b.union(b.instance("ArrayBuffer"), b.instance("SharedArrayBuffer"));
// The memory keeps one wrapper until it grows, or until a wrapper of the other kind is asked for. The getter and the two methods answer that one.
const answersKeptBuffer = [
  b.createdIn(memoryBuffer.fresh, b.nativeCell(memoryBufferCell)),
  b.returnedFrom(memoryBufferCell),
];
const answersKeptBufferOfKind = {
  this: b.any.refusing(b.null),
  returns: memoryBuffer,
  capability: "heap",
  throws: true,
  uses: { this: answersKeptBuffer },
  implementation: b.generic,
} satisfies b.FunctionField;

// The code of a module does with what it imports and is handed whatever native code may do, and nothing is known of what it exports.
const instantiates = (implementation: b.ImplementationSpec, returns: b.TableType = b.any) =>
  ({
    args: { operands: b.any.rest },
    returns,
    capability: "js",
    throws: true,
    uses: {
      operands: [
        b.everything,
        b.createdIn(b.any, b.nativeCell(globalValueCell)),
        b.createdIn(b.any, b.nativeCell(tableElementCell)),
        b.createdIn(b.any, b.nativeCell(exceptionPayloadCell)),
        b.createdIn(b.object, b.nativeCell(exportsCell)),
      ],
    },
    implementation,
  }) satisfies b.FunctionField;

const webAssemblyClass = (name: string, options: b.ClassOptions = {}) =>
  b.class(`WebAssembly.${name}`, {
    nativeSymbols: [`callJSWebAssembly${name}`, `constructJSWebAssembly${name}`],
    call: requiresNew,
    ...options,
  });

// A function of the interface reads the bytes of a buffer, or the members of a descriptor under their names, and converts those to primitives. It calls nothing else.
const webAssemblyFunction = (
  args: Readonly<Record<string, b.TableType>>,
  uses: Readonly<Record<string, readonly b.UseSpec[]>>,
  returns: b.TableType,
) =>
  ({
    this: b.any,
    args,
    returns,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: { this: b.inspected, ...uses },
    reads: readByConversions,
    implementation: b.hostCall,
  }) satisfies b.FunctionField;
const webAssemblyConstruction = (
  name: string,
  args: Readonly<Record<string, b.TableType>>,
  uses: Readonly<Record<string, readonly b.UseSpec[]>>,
) => {
  const { this: _, uses: { this: __, ...operandUses }, ...signature } = webAssemblyFunction(args, uses, b.instance(`WebAssembly.${name}`).fresh);
  return { ...signature, uses: operandUses } satisfies b.FunctionField;
};
const bytesOfModule = [b.readsContents()];
const compileOptions = [b.runsTransitively("Iterate", "ToPrimitive", b.named("builtins", "importedStringConstants")), b.readsContents()];
const moduleOperands = { bytes: b.any, options: b.any.optional };
const usesOfModuleOperands = { bytes: bytesOfModule, options: compileOptions };

// As an error of the language: the message is converted to a string and `cause` is whatever the options hold.
const webAssemblyError = (name: string) => {
  const creation = {
    args: { message: b.string.loose.optional, options: b.any.optional },
    returns: b.instance(`WebAssembly.${name}`).unaliased,
    capability: "js",
    throws: b.propagated,
    runsOnlyOperandCode: true,
    uses: { options: [b.readByKey(b.keysNamed("cause"), b.nativeMemory), b.contentsStoredIn(b.nativeMemory)] },
    implementation: b.hostCall,
  } satisfies b.FunctionField;
  return webAssemblyClass(name, { extends: "Error", constructorOnlyReadsPrototypeOfNewTarget: true, call: creation, construct: creation });
};

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
    proto: { next: libraryStep("MapIteratorPrototype", "next", iteratorResult) },
  }),
  b.class("%SetIterator%", {
    ...lockedIteratorColumns,
    proto: { next: libraryStep("SetIteratorPrototype", "next", iteratorResult) },
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
    proto: {
      // IntlSegmentsPrototype.cpp intlSegmentsPrototypeFuncIterator, IntlSegments::createSegmentIterator: the iterator gets a copy of the break iterator, the buffer and the string, not the receiver.
      "@@iterator": {
        returns: b.instance("IntlSegmentIterator").fresh,
        capability: "heap",
        throws: true,
        uses: { this: b.inspected },
        implementation: b.generic,
      },
      containing: intlMethod({ index: convertedToPrimitive }, b.union(newPlainData, b.undefined)),
    },
  }),
  b.class("%IntlSegmentIterator%", {
    ...iteratorColumns,
    extends: "Iterator",
    prototypeTable: b.staticTable("runtime/IntlSegmentIteratorPrototype.cpp", "segmentIteratorPrototypeTable"),
    proto: { next: hostStep(advances) },
  }),

  functionKind("AsyncFunction", "ClosureAsCreated"),
  functionKind("GeneratorFunction"),
  functionKind("AsyncGeneratorFunction"),

  intlService(
    "Collator",
    "collator",
    ["IntlCollation"],
    collatorOptions,
    {
      // The collator makes the function at the first read and keeps it. The function holds the collator.
      compare: {
        getter: b.callable,
        capability: "heap",
        throws: true,
        uses: { this: [b.retained] },
        // A custom accessor of a static table, as `format` of the other formatters.
        implementation: b.generic,
        call: {
          args: { x: b.any.optional, y: b.any.optional },
          returns: b.f64,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: b.inspected, x: convertedToPrimitive, y: convertedToPrimitive },
          reads: readByConversions,
          implementation: b.generic,
        },
      },
    },
    intlServiceCreation("Collator", collatorOptions),
  ),
  intlService(
    "DisplayNames",
    "displayNames",
    ["IntlDisplayNames"],
    ["localeMatcher", "style", "type", "fallback", "languageDisplay"],
    { of: intlMethod({ code: convertedToPrimitive }, b.union(b.string, b.undefined)) },
  ),
  // The duration is a string, a Temporal.Duration or an object whose members under the names of the units are converted to numbers.
  intlService(
    "DurationFormat",
    "durationFormat",
    ["IntlNumberFormat", "IntlUnits"],
    durationFormatOptions,
    formatAndParts({ duration: [...optionBag(durationUnits), b.readsContents()] }),
  ),
  // What the list hands out has to be strings.
  intlService(
    "ListFormat",
    "listFormat",
    ["IntlOther"],
    ["localeMatcher", "type", "style"],
    formatAndParts({ list: [b.runs("Iterate"), b.readsContents()] }),
  ),
  intlService(
    "PluralRules",
    "pluralRules",
    ["IntlOther", "IntlNumberFormat"],
    ["localeMatcher", "type", "notation", "compactDisplay", ...digitOptions],
    {
      select: intlMethod({ number: convertedToPrimitive }, b.string),
      selectRange: intlMethod({ start: convertedToPrimitive, end: convertedToPrimitive }, b.string),
    },
  ),
  intlService(
    "RelativeTimeFormat",
    "relativeTimeFormat",
    ["IntlOther", "IntlNumberFormat"],
    ["localeMatcher", "numberingSystem", "style", "numeric"],
    formatAndParts({ value: convertedToPrimitive, unit: convertedToPrimitive }),
  ),
  // The segments hold the string that the operand is converted to.
  intlService("Segmenter", "segmenter", ["IntlSegmenter"], ["localeMatcher", "granularity"], {
    segment: intlMethod({ input: convertedToPrimitive }, b.instance("IntlSegments").fresh),
  }),
  b.class("Intl.Locale", {
    usesFeature: ["IntlOther"],
    prototypeUsesFeature: true,
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
    proto: {
      toString: {
        this: b.any,
        returns: b.union(b.string, b.undefined),
        capability: "heap",
        throws: true,
        uses: { this: b.inspected },
        implementation: b.hostCall,
      },
      ...Object.fromEntries(
        [
          "baseName",
          "calendar",
          "caseFirst",
          "collation",
          "firstDayOfWeek",
          "hourCycle",
          "numberingSystem",
          "language",
          "script",
          "region",
          "variants",
        ].map(name => [name, { ...internalSlotGetter, getter: b.union(b.string, b.undefined) }]),
      ),
      numeric: { ...internalSlotGetter, getter: b.bool },
      maximize: answeredFromReceiver(b.instance("Intl.Locale").fresh),
      minimize: answeredFromReceiver(b.instance("Intl.Locale").fresh),
      ...Object.fromEntries(
        ["getCalendars", "getCollations", "getHourCycles", "getNumberingSystems", "getTimeZones"].map(name => [
          name,
          answeredFromReceiver(b.union(newStringArray, b.undefined)),
        ]),
      ),
      getTextInfo: answeredFromReceiver(newPlainData),
      getWeekInfo: answeredFromReceiver(newPlainData),
    },
  }),
  b.extend("Intl.Locale.prototype.getCollations", { usesFeature: "IntlCollation" }),

  b.namespace("Temporal", {}, { table: b.staticTable("runtime/TemporalObject.cpp", "temporalObjectTable") }),
  b.namespace(
    "Temporal.Now",
    {
      instant: { ...temporalOfReceiver(newTemporal("Instant")), reads: [hostSettings], writes: [hostSettings] },
      timeZoneId: { ...temporalOfReceiver(b.string), reads: [hostSettings] },
      plainDateISO: { ...temporalOfOperands(0, newTemporal("PlainDate")), writes: [hostSettings] },
      plainDateTimeISO: { ...temporalOfOperands(0, newTemporal("PlainDateTime")), writes: [hostSettings] },
      plainTimeISO: { ...temporalOfOperands(0, newTemporal("PlainTime")), writes: [hostSettings] },
      zonedDateTimeISO: { ...temporalOfOperands(0, newTemporal("ZonedDateTime")), writes: [hostSettings] },
    },
    { usesFeature: "Temporal", table: b.staticTable("runtime/TemporalNow.cpp", "temporalNowTable") },
  ),
  temporalClass("Duration", "durationPrototypeTable", true, {
    ...Object.fromEntries(durationUnits.map(unit => [unit, temporalGetter(b.f64)])),
    sign: temporalGetter(b.int(-1, 1)),
    blank: temporalGetter(b.bool),
    with: temporalOfOperands(1, newTemporal("Duration")),
    negated: temporalOfReceiver(newTemporal("Duration")),
    abs: temporalOfReceiver(newTemporal("Duration")),
    ...temporalArithmetic("Duration"),
    round: temporalOfOperands(1, newTemporal("Duration")),
    total: temporalOfOperands(1, b.f64),
    toLocaleString: {
      this: b.any.refusing(b.null),
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
  temporalClass(
    "Instant",
    "instantPrototypeTable",
    true,
    {
      ...epochGetters,
      ...temporalArithmetic("Instant"),
      ...temporalDifferences,
      round: temporalOfOperands(1, newTemporal("Instant")),
      equals: temporalOfOperands(1, b.bool),
      toZonedDateTimeISO: temporalOfOperands(1, newTemporal("ZonedDateTime")),
    },
    {
      fromEpochMilliseconds: temporalOfOperands(1, newTemporal("Instant")),
      fromEpochNanoseconds: temporalOfOperands(1, newTemporal("Instant")),
    },
  ),
  temporalClass("PlainDate", "plainDatePrototypeTable", true, {
    ...calendarDateGetters,
    ...dayGetters,
    ...temporalArithmetic("PlainDate"),
    ...temporalDifferences,
    with: temporalOfOperands(1, newTemporal("PlainDate")),
    withCalendar: temporalOfOperands(1, newTemporal("PlainDate")),
    equals: temporalOfOperands(1, b.bool),
    ...conversionsOfTemporal("PlainMonthDay", "PlainYearMonth"),
    toPlainDateTime: temporalOfOperands(0, newTemporal("PlainDateTime")),
    toZonedDateTime: temporalOfOperands(1, newTemporal("ZonedDateTime")),
  }),
  temporalClass("PlainDateTime", "plainDateTimePrototypeTable", true, {
    ...calendarDateGetters,
    ...dayGetters,
    ...timeGetters,
    ...temporalArithmetic("PlainDateTime"),
    ...temporalDifferences,
    with: temporalOfOperands(1, newTemporal("PlainDateTime")),
    withCalendar: temporalOfOperands(1, newTemporal("PlainDateTime")),
    withPlainTime: temporalOfOperands(0, newTemporal("PlainDateTime")),
    round: temporalOfOperands(1, newTemporal("PlainDateTime")),
    equals: temporalOfOperands(1, b.bool),
    ...conversionsOfTemporal("PlainDate", "PlainTime"),
    toZonedDateTime: temporalOfOperands(1, newTemporal("ZonedDateTime")),
  }),
  temporalClass("PlainMonthDay", "plainMonthDayPrototypeTable", false, {
    calendarId: temporalGetter(b.string),
    monthCode: temporalGetter(b.string),
    day: temporalGetter(b.f64),
    with: temporalOfOperands(1, newTemporal("PlainMonthDay")),
    equals: temporalOfOperands(1, b.bool),
    toPlainDate: temporalOfOperands(1, newTemporal("PlainDate")),
  }),
  temporalClass("PlainTime", "plainTimePrototypeTable", true, {
    ...timeGetters,
    ...temporalArithmetic("PlainTime"),
    ...temporalDifferences,
    with: temporalOfOperands(1, newTemporal("PlainTime")),
    round: temporalOfOperands(1, newTemporal("PlainTime")),
    equals: temporalOfOperands(1, b.bool),
  }),
  temporalClass("PlainYearMonth", "plainYearMonthPrototypeTable", true, {
    ...calendarDateGetters,
    ...temporalArithmetic("PlainYearMonth"),
    ...temporalDifferences,
    with: temporalOfOperands(1, newTemporal("PlainYearMonth")),
    equals: temporalOfOperands(1, b.bool),
    toPlainDate: temporalOfOperands(1, newTemporal("PlainDate")),
  }),
  temporalClass("ZonedDateTime", "zonedDateTimePrototypeTable", true, {
    ...calendarDateGetters,
    ...dayGetters,
    ...timeGetters,
    ...epochGetters,
    timeZoneId: temporalGetter(b.string),
    offset: temporalGetter(b.string),
    offsetNanoseconds: temporalGetter(b.f64),
    hoursInDay: temporalGetter(b.f64),
    ...temporalArithmetic("ZonedDateTime"),
    ...temporalDifferences,
    with: temporalOfOperands(1, newTemporal("ZonedDateTime")),
    withCalendar: temporalOfOperands(1, newTemporal("ZonedDateTime")),
    withPlainTime: temporalOfOperands(0, newTemporal("ZonedDateTime")),
    withTimeZone: temporalOfOperands(1, newTemporal("ZonedDateTime")),
    round: temporalOfOperands(1, newTemporal("ZonedDateTime")),
    equals: temporalOfOperands(1, b.bool),
    startOfDay: temporalOfReceiver(newTemporal("ZonedDateTime")),
    getTimeZoneTransition: temporalOfOperands(1, b.union(newTemporal("ZonedDateTime"), b.strictNull)),
    ...conversionsOfTemporal("Instant", "PlainDate", "PlainDateTime", "PlainTime"),
  }),

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
      compile: webAssemblyFunction(
        moduleOperands,
        usesOfModuleOperands,
        b.instance("Promise").holding({ value: b.instance("WebAssembly.Module").fresh, reason: b.any }).fresh,
      ),
      compileStreaming: undeclared,
      // For a module its instance, for bytes both.
      instantiate: instantiates(
        b.hostCall,
        b.instance("Promise").holding({
          value: b.union(
            b.instance("WebAssembly.Instance").fresh,
            b.ordinaryObjectOf("WebAssemblyInstantiatedSource").fresh,
          ),
          reason: b.any,
        }).fresh,
      ),
      instantiateStreaming: instantiates(b.generic),
      // The operand has to be a function that an instance exports. The new function holds it.
      promising: webAssemblyFunction({ exported: b.any }, { exported: [b.retained] }, b.callable),
      validate: webAssemblyFunction(moduleOperands, usesOfModuleOperands, b.bool),
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
    // A number or a BigInt is made of a value of the payload, a reference is kept as it is.
    construct: webAssemblyConstruction(
      "Exception",
      { tag: b.any, payload: b.any, options: b.any.optional },
      {
        tag: b.inspected,
        payload: [
          b.runsTransitively("Iterate", "ToPrimitive"),
          b.readsContents(),
          b.iteratedValuesStoredIn(b.nativeMemory),
          b.iteratedValuesStoredIn(b.nativeCell(exceptionPayloadCell)),
        ],
        options: optionBag(["traceStack"]),
      },
    ),
    proto: {
      stack: webAssemblyGetter(b.union(b.string, b.undefined)),
      getArg: webAssemblyFunction(
        { tag: b.any, index: b.any },
        { this: [b.returnedFrom(exceptionPayloadCell)], tag: b.inspected, index: convertedToPrimitive },
        b.any,
      ),
      is: { ...answeredFromReceiver(b.bool), args: { tag: b.any }, uses: { this: b.inspected, tag: b.inspected } },
    },
  }),
  webAssemblyClass("Global", {
    prototypeTable: webAssemblyTable("WebAssemblyGlobalPrototype", "prototypeGlobalWebAssemblyGlobal"),
    construct: {
      args: { descriptor: b.any.optional, value: b.any.optional },
      returns: b.instance("WebAssembly.Global").fresh,
      capability: "js",
      throws: true,
      runsOnlyOperandCode: true,
      // It reads the two members of the descriptor and converts `value` to a string. A number or a BigInt is made of the value, a reference is kept as it is.
      uses: {
        descriptor: [b.runsTransitively(b.named("mutable", "value"), "ToPrimitive")],
        value: [b.retained, keptInGlobal, b.runs("ToPrimitive")],
      },
      implementation: b.hostCall,
    },
    proto: {
      value: {
        ...webAssemblyGetter(b.any, answersValueOfGlobal),
        setter: {
          this: b.any.refusing(b.null),
          value: b.any,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: b.inspected, value: [b.retained, keptInGlobal, b.runs("ToPrimitive")] },
          implementation: b.generic,
        },
      },
      valueOf: {
        this: b.any.refusing(b.null),
        returns: b.any,
        capability: "heap",
        throws: true,
        uses: { this: answersValueOfGlobal },
        implementation: b.hostCall,
      },
    },
  }),
  webAssemblyClass("Instance", {
    construct: instantiates(b.hostCall),
    proto: { exports: webAssemblyGetter(b.object, [b.returnedFrom(exportsCell)]) },
  }),
  webAssemblyClass("Memory", {
    prototypeTable: webAssemblyTable("WebAssemblyMemoryPrototype", "prototypeTableWebAssemblyMemory"),
    construct: webAssemblyConstruction(
      "Memory",
      { descriptor: b.any },
      { descriptor: optionBag(["address", "initial", "minimum", "maximum", "shared"]) },
    ),
    proto: {
      // The buffer that the memory has handed out is detached.
      grow: {
        ...webAssemblyFunction({ delta: b.any }, { delta: convertedToPrimitive }, addressOrCount),
        writes: [typedArrayStorage],
      },
      buffer: webAssemblyGetter(memoryBuffer, answersKeptBuffer),
      toFixedLengthBuffer: answersKeptBufferOfKind,
      toResizableBuffer: answersKeptBufferOfKind,
    },
  }),
  webAssemblyClass("Module", {
    constructorTable: webAssemblyTable("WebAssemblyModuleConstructor", "constructorTableWebAssemblyModule"),
    construct: webAssemblyConstruction("Module", moduleOperands, usesOfModuleOperands),
    // Each answers new arrays and objects that describe the module.
    static: {
      exports: webAssemblyFunction({ module: b.any }, { module: b.inspected }, newPlainData),
      imports: webAssemblyFunction({ module: b.any }, { module: b.inspected }, newPlainData),
      customSections: webAssemblyFunction(
        { module: b.any, name: b.any },
        { module: b.inspected, name: convertedToPrimitive },
        b.instance("Array").holding({ element: b.instance("ArrayBuffer").fresh }).fresh,
      ),
    },
  }),
  // The code of an instance that imports the new object calls the function.
  webAssemblyClass("Suspending", {
    nativeSymbols: ["callWebAssemblySuspending", "constructWebAssemblySuspending"],
    construct: {
      args: { called: b.any },
      returns: b.object.fresh,
      capability: "heap",
      throws: true,
      uses: { called: [b.retained] },
      implementation: b.hostCall,
    },
  }),
  webAssemblyClass("Table", {
    prototypeTable: webAssemblyTable("WebAssemblyTablePrototype", "prototypeTableWebAssemblyTable"),
    construct: webAssemblyConstruction(
      "Table",
      { descriptor: b.any, value: b.any.optional },
      { descriptor: optionBag(["address", "element", "initial", "minimum", "maximum"]), value: keptInTable },
    ),
    proto: {
      // A table whose addresses have 64 bits answers a BigInt.
      length: webAssemblyGetter(addressOrCount),
      get: webAssemblyFunction(
        { index: b.any },
        { this: [b.returnedFrom(tableElementCell)], index: convertedToPrimitive },
        b.any,
      ),
      set: webAssemblyFunction(
        { index: b.any, value: b.any.optional },
        { index: convertedToPrimitive, value: keptInTable },
        b.undefined,
      ),
      grow: webAssemblyFunction(
        { delta: b.any, value: b.any.optional },
        { delta: convertedToPrimitive, value: keptInTable },
        addressOrCount,
      ),
    },
  }),
  webAssemblyClass("Tag", {
    construct: webAssemblyConstruction(
      "Tag",
      { type: b.any },
      { type: [b.runsTransitively("Iterate", "ToPrimitive", b.named("parameters")), b.readsContents()] },
    ),
  }),
  webAssemblyError("CompileError"),
  webAssemblyError("LinkError"),
  webAssemblyError("RuntimeError"),
  webAssemblyError("SuspendError"),

  // The host declares the object: what the other members do is up to its ConsoleClient.
  b.fn("console.takeHeapSnapshot", b.otherwise),
  b.extend("console.takeHeapSnapshot", {
    nativeSymbols: ["consoleProtoFuncTakeHeapSnapshot"],
    usesFeature: ["Profilers"],
  }),
);
