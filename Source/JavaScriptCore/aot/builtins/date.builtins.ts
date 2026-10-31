import * as b from "bun-builtins-table";

type Zone = "Local" | "UTC";

const clock = "ExternalState";
const hostTimeZone = "ExternalState";
const dateCache = "RuntimeState";
const cachedCalendarFields = "InternalFields";
const membersAddedWhenRead = "ExternalState";

const component = b.f64.loose;
const optionalComponent = component.optional;
const timeOfDay = {
  hours: optionalComponent,
  minutes: optionalComponent,
  seconds: optionalComponent,
  ms: optionalComponent,
};
const newDate = b.instance("Date").fresh;
const readsTime = b.readsContents("time");
const writesTime = b.writesContents("time");

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

const timeZoneReads = (zone: Zone) => (zone === "Local" ? [hostTimeZone] : []);

const timeValue = {
  returns: b.f64,
  capability: "none",
  uses: { this: [readsTime] },
  implementation: b.inline("DateGetTime"),
} satisfies b.FunctionField;

const milliseconds = {
  returns: b.f64,
  capability: "none",
  uses: { this: [readsTime] },
  implementation: b.inline("DateGetMilliseconds"),
} satisfies b.FunctionField;

const calendarField = (field: string, zone: Zone) =>
  ({
    returns: b.f64,
    capability: "none",
    uses: { this: [readsTime] },
    reads: [...timeZoneReads(zone), dateCache],
    writes: [cachedCalendarFields, dateCache],
    implementation: b.inline("DateGetField", { parameters: [field, zone], otherwise: b.hostCall }),
  }) satisfies b.FunctionField;

const calendarSetter = (zone: Zone, args: Readonly<Record<string, b.TableType>>) =>
  ({
    args,
    returns: b.f64,
    capability: "none",
    readsStateBeforeConversions: true,
    uses: { this: [readsTime, writesTime] },
    reads: [...timeZoneReads(zone), dateCache],
    writes: [dateCache],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

const formatted = (zone: Zone) =>
  ({
    returns: b.string,
    capability: "heap",
    uses: { this: [readsTime] },
    reads: timeZoneReads(zone),
    writes: [cachedCalendarFields],
    implementation: b.hostCall,
  }) satisfies b.FunctionField;

// Without options, and without a locale or with a well-formed one, there is nothing to refuse and nothing to ask.
const localized = [
  {
    args: { locales: b.hostLocaleOrWellFormedTag, options: b.undefined.optional },
    returns: b.string,
    capability: "heap",
    isSpecialCase: true,
    uses: { this: [readsTime] },
    reads: [hostTimeZone],
    implementation: b.hostCall,
  },
  {
    args: { locales: b.any.optional, options: b.any.optional },
    returns: b.string,
    capability: "js",
    throws: true,
    runsOnlyOperandCode: true,
    uses: {
      this: [readsTime],
      locales: b.localeList,
      options: b.optionBag(dateTimeFormatOptions),
    },
    reads: [hostTimeZone],
    usesFeature: "IntlDateTimeFormat",
    implementation: b.hostCall,
  },
] satisfies b.FunctionField[];

export default b.declarations(
  // Date.prototype.toJSON calls toISOString, and a conversion calls toString and valueOf, without operands. So does Object.prototype.toLocaleString.
  b.namedMemberCall("toISOString", []),
  b.namedMemberCall("toString", []),
  b.namedMemberCall("valueOf", []),
  b.class("Date", {
    jsType: "JSDateType",
    slots: { time: b.state("InternalFields", b.f64) },
    lockedMembers: ["constructor", "prototype"],
    // datejs: `Date.today().next().friday()`.
    openNamesOfPrototype: ["next"],
    types: [b.exactType("Date")],
    constructorTable: b.staticTable("runtime/DateConstructor.cpp", "dateConstructorTable"),
    prototypeTable: b.staticTable("runtime/DatePrototype.cpp", "datePrototypeTable"),
    call: {
      returns: b.string,
      capability: "heap",
      reads: [clock],
      writes: [clock],
      implementation: b.hostCall,
    },
    construct: [
      {
        args: { value: b.notPassed },
        returns: newDate,
        capability: "heap",
        reads: [clock],
        writes: [clock],
        implementation: b.hostCall,
      },
      {
        args: { value: b.f64, month: b.notPassed },
        returns: newDate,
        capability: "heap",
        implementation: b.hostCall,
      },
      {
        args: { value: b.string, month: b.notPassed },
        returns: newDate,
        capability: "heap",
        uses: { value: [b.readAsCharacters] },
        reads: [hostTimeZone],
        implementation: b.hostCall,
      },
      {
        args: { value: b.instance("Date"), month: b.notPassed },
        returns: newDate,
        capability: "heap",
        uses: { value: [readsTime] },
        implementation: b.hostCall,
      },
      {
        args: { value: b.primitiveValue.loose, month: b.notPassed },
        returns: newDate,
        capability: "heap",
        throws: true,
        uses: { value: [b.readAsCharacters] },
        reads: [hostTimeZone],
        implementation: b.hostCall,
      },
      {
        args: { year: component, month: component, date: optionalComponent, ...timeOfDay },
        returns: newDate,
        capability: "heap",
        reads: [hostTimeZone],
        implementation: b.hostCall,
      },
    ],
    static: {
      now: {
        returns: b.f64,
        capability: "none",
        reads: [clock],
        writes: [clock],
        implementation: b.inline("DateNow"),
      },
      parse: {
        args: { string: b.string.loose },
        returns: b.f64,
        capability: "heap",
        uses: { string: [b.readAsCharacters] },
        reads: [hostTimeZone],
        implementation: b.hostCall,
      },
      UTC: {
        args: { year: component, month: optionalComponent, date: optionalComponent, ...timeOfDay },
        returns: b.f64,
        capability: "none",
        deterministic: true,
        implementation: b.hostCall,
      },
    },
    proto: {
      getTime: timeValue,
      valueOf: timeValue,
      getMilliseconds: milliseconds,
      getUTCMilliseconds: milliseconds,
      getFullYear: calendarField("FullYear", "Local"),
      getUTCFullYear: calendarField("FullYear", "UTC"),
      getMonth: calendarField("Month", "Local"),
      getUTCMonth: calendarField("Month", "UTC"),
      getDate: calendarField("Date", "Local"),
      getUTCDate: calendarField("Date", "UTC"),
      getDay: calendarField("Day", "Local"),
      getUTCDay: calendarField("Day", "UTC"),
      getHours: calendarField("Hours", "Local"),
      getUTCHours: calendarField("Hours", "UTC"),
      getMinutes: calendarField("Minutes", "Local"),
      getUTCMinutes: calendarField("Minutes", "UTC"),
      getSeconds: calendarField("Seconds", "Local"),
      getUTCSeconds: calendarField("Seconds", "UTC"),
      getTimezoneOffset: calendarField("TimezoneOffset", "Local"),
      getYear: calendarField("Year", "Local"),

      setTime: {
        args: { time: component },
        returns: b.f64,
        capability: "none",
        uses: { this: [writesTime] },
        implementation: b.inline("DateSetTime"),
      },
      setMilliseconds: calendarSetter("Local", { ms: component }),
      setUTCMilliseconds: calendarSetter("UTC", { ms: component }),
      setSeconds: calendarSetter("Local", { sec: component, ms: optionalComponent }),
      setUTCSeconds: calendarSetter("UTC", { sec: component, ms: optionalComponent }),
      setMinutes: calendarSetter("Local", { min: component, sec: optionalComponent, ms: optionalComponent }),
      setUTCMinutes: calendarSetter("UTC", { min: component, sec: optionalComponent, ms: optionalComponent }),
      setHours: calendarSetter("Local", {
        hour: component,
        min: optionalComponent,
        sec: optionalComponent,
        ms: optionalComponent,
      }),
      setUTCHours: calendarSetter("UTC", {
        hour: component,
        min: optionalComponent,
        sec: optionalComponent,
        ms: optionalComponent,
      }),
      setDate: calendarSetter("Local", { date: component }),
      setUTCDate: calendarSetter("UTC", { date: component }),
      setMonth: calendarSetter("Local", { month: component, date: optionalComponent }),
      setUTCMonth: calendarSetter("UTC", { month: component, date: optionalComponent }),
      setFullYear: calendarSetter("Local", { year: component, month: optionalComponent, date: optionalComponent }),
      setUTCFullYear: calendarSetter("UTC", { year: component, month: optionalComponent, date: optionalComponent }),
      setYear: calendarSetter("Local", { year: component }),

      toString: formatted("Local"),
      toDateString: formatted("Local"),
      toTimeString: formatted("Local"),
      toUTCString: formatted("UTC"),
      toISOString: {
        returns: b.string,
        capability: "heap",
        throws: true,
        uses: { this: [readsTime] },
        writes: [cachedCalendarFields],
        implementation: b.inline("DateToISOString", { otherwise: b.hostCall }),
      },
      toLocaleString: localized,
      toLocaleDateString: localized,
      toLocaleTimeString: localized,
      toJSON: [
        {
          // A pristine date finds the built-in @@toPrimitive, valueOf and toISOString. The host call answers null for an invalid date.
          this: b.instance("Date").pristine("Names"),
          args: { key: b.any.optional },
          returns: b.union(b.string, b.null),
          capability: "heap",
          throws: true,
          uses: { this: [readsTime], key: b.inspected },
          reads: [membersAddedWhenRead],
          writes: [cachedCalendarFields, membersAddedWhenRead],
          implementation: b.inline("DateToISOString", { otherwise: b.hostCall }),
        },
        {
          this: b.object.loose.refusing(b.null),
          args: { key: b.any.optional },
          returns: b.any,
          capability: "js",
          throws: true,
          runsOnlyOperandCode: true,
          uses: { this: [b.runs("ToPrimitive", b.calls("toISOString"))], key: b.inspected },
          implementation: b.hostCall,
        },
      ],
      toTemporalInstant: {
        returns: b.object.fresh,
        capability: "heap",
        throws: true,
        uses: { this: [readsTime] },
        usesFeature: "Temporal",
        nativeSymbols: ["dateProtoFuncToTemporalInstant"],
        implementation: b.hostCall,
      },
      "@@toPrimitive": {
        this: b.object,
        args: { hint: b.any.optional },
        returns: b.primitiveValue,
        capability: "js",
        throws: true,
        runsOnlyOperandCode: true,
        uses: { this: [b.runs(b.calls("toString", "valueOf"))], hint: b.inspected },
        implementation: b.hostCall,
      },
    },
  }),
  b.same("Date.prototype.toGMTString", "Date.prototype.toUTCString"),

  // Tests spy on Date.now, and datejs replaces Date.parse and Date.prototype.toString. @formatjs/intl-datetimeformat/polyfill-force replaces the three methods that format for a locale.
  b.replacedByPackages(
    "Date.now",
    "Date.parse",
    "Date.prototype.toString",
    "Date.prototype.toLocaleString",
    "Date.prototype.toLocaleDateString",
    "Date.prototype.toLocaleTimeString",
  ),
);
