import * as b from "bun-builtins-table";

const hostSettings = "ExternalState";

const { localeList, optionBag } = b;
const newStringArray = b.instance("Array").holding({ element: b.string }).fresh;

const dateTimeFormatOptions = [
  "localeMatcher",
  "calendar",
  "numberingSystem",
  "hour12",
  "hourCycle",
  "timeZone",
  "weekday",
  "era",
  "year",
  "month",
  "day",
  "dayPeriod",
  "hour",
  "minute",
  "second",
  "fractionalSecondDigits",
  "timeZoneName",
  "formatMatcher",
  "dateStyle",
  "timeStyle",
];

const numberFormatOptions = [
  "localeMatcher",
  "numberingSystem",
  "style",
  "currency",
  "currencyDisplay",
  "currencySign",
  "unit",
  "unitDisplay",
  "notation",
  "minimumIntegerDigits",
  "minimumFractionDigits",
  "maximumFractionDigits",
  "minimumSignificantDigits",
  "maximumSignificantDigits",
  "roundingIncrement",
  "roundingMode",
  "roundingPriority",
  "trailingZeroDisplay",
  "compactDisplay",
  "useGrouping",
  "signDisplay",
];

const temporalClassesFormattedAsDates = [
  "Instant",
  "PlainDate",
  "PlainDateTime",
  "PlainMonthDay",
  "PlainTime",
  "PlainYearMonth",
  "ZonedDateTime",
];

const supportedLocalesOf = {
  args: { locales: b.any.optional, options: b.any.optional },
  returns: newStringArray,
  capability: "js",
  throws: true,
  runsOnlyOperandCode: true,
  uses: { locales: localeList, options: optionBag(["localeMatcher"]) },
  implementation: b.hostCall,
} satisfies b.FunctionField;

const formatterCreation = (className: string, optionNames: readonly string[]) =>
  ({
    args: { locales: b.any.optional, options: b.any.optional },
    returns: b.instance(className).fresh,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: { locales: localeList, options: optionBag(optionNames) },
    reads: [hostSettings],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const boundFormat = (inlineBuiltin: string, parameter: string) =>
  ({
    getter: b.callable,
    capability: "heap",
    throws: true,
    uses: { this: [b.retained] },
    implementation: b.hostCall,
    call: [
      {
        args: { [parameter]: b.f64 },
        returns: b.string,
        capability: "heap",
        throws: true,
        uses: { this: b.inspected },
        reads: [hostSettings],
        implementation: b.inline(inlineBuiltin, { otherwise: b.hostCall }),
      },
      {
        args: { [parameter]: b.any.optional },
        returns: b.string,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { this: b.inspected, [parameter]: [b.runs("ToPrimitive")] },
        reads: [hostSettings],
        implementation: b.generic,
      },
    ],
  }) satisfies b.GetterField;

const formatterClass = (
  name: string,
  tablePrefix: string,
  usesFeature: readonly b.FeatureUseSpec[],
  optionNames: readonly string[],
  format: b.GetterField,
) => {
  const creation = formatterCreation(`Intl.${name}`, optionNames);
  return b.class(`Intl.${name}`, {
    instanceCppClass: b.cppClass(`Intl${name}`),
    types: [b.exactType(`Intl.${name}`)],
    usesFeature,
    nativeSymbols: [`callIntl${name}`, `constructIntl${name}`],
    requiresHolderAsThis: true,
    constructorTable: b.staticTable(`runtime/Intl${name}Constructor.cpp`, `${tablePrefix}ConstructorTable`),
    prototypeTable: b.staticTable(`runtime/Intl${name}Prototype.cpp`, `${tablePrefix}PrototypeTable`),
    call: [
      { ...creation, uses: { this: [b.runs("PrototypeChain")], ...creation.uses } },
      { ...creation, this: b.any, returns: b.object, uses: { this: [b.everything], ...creation.uses } },
    ],
    construct: creation,
    static: { supportedLocalesOf },
    proto: { format },
  });
};

const temporalDateToLocaleString = {
  args: { locales: b.any.optional, options: b.any.optional },
  returns: b.string,
  capability: "js",
  throws: true,
  runsOnlyOperandCode: true,
  uses: { this: b.inspected, locales: localeList, options: optionBag(dateTimeFormatOptions) },
  reads: [hostSettings],
  usesFeature: "IntlDateTimeFormat",
  implementation: b.hostCall,
} satisfies b.FunctionField;

const numberToLocaleString = (receiver: b.TableType) =>
  ({
    this: receiver,
    args: { locales: b.any.optional, options: b.any.optional },
    returns: b.string,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: { this: b.inspected, locales: localeList, options: optionBag(numberFormatOptions) },
    reads: [hostSettings],
    usesFeature: ["IntlNumberFormat", "IntlUnits"],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

export default b.declarations(
  b.namespace(
    "Intl",
    {
      getCanonicalLocales: {
        args: { locales: b.any.optional },
        returns: newStringArray,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { locales: localeList },
        usesFeature: "IntlOther",
        implementation: b.hostCall,
      },
      supportedValuesOf: {
        args: { key: b.string.loose },
        returns: newStringArray,
        capability: "heap",
        throws: true,
        usesFeature: ["IntlOther", "IntlLocale", { feature: "IntlCollation", when: b.whenOperand("key", "collation") }],
        implementation: b.hostCall,
      },
    },
    { table: b.staticTable("runtime/IntlObject.cpp", "intlObjectTable") },
  ),

  formatterClass(
    "DateTimeFormat",
    "dateTimeFormat",
    ["IntlDateTimeFormat"],
    dateTimeFormatOptions,
    boundFormat("FormatDateTime", "date"),
  ),
  formatterClass(
    "NumberFormat",
    "numberFormat",
    ["IntlNumberFormat", "IntlUnits"],
    numberFormatOptions,
    boundFormat("FormatNumber", "value"),
  ),
  b.method(
    "Number.prototype.toLocaleString",
    numberToLocaleString(b.union(b.f64, b.instance("Number"), b.builtin("Number.prototype"))),
  ),
  b.method("BigInt.prototype.toLocaleString", numberToLocaleString(b.union(b.bigint, b.instance("BigInt")))),

  temporalClassesFormattedAsDates.flatMap(name =>
    b.method(`Temporal.${name}.prototype.toLocaleString`, temporalDateToLocaleString),
  ),
);
