//@ requireOptions("--useTemporal=1")
// ISO 8601 / RFC 9557 serialization of Temporal types: extended years, fractionalSecondDigits, smallestUnit with
// roundingMode, offsets with sub-minute components (rounded to minutes in strings, exact in .offset), and the
// offset, timeZoneName and calendarName options.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(`${msg}: expected ${expected} but got ${actual}`);
}

function shouldThrow(fn, errorType, msg) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof errorType))
            throw new Error(`${msg}: expected ${errorType.name} but got ${e}`);
    }
    if (!threw)
        throw new Error(`${msg}: expected ${errorType.name} to be thrown`);
}

const iterations = testLoopCount;

// Extended years. Columns: year, month, day, PlainDate, PlainDateTime (01:02:03.004005006), PlainYearMonth,
// ZonedDateTime at noon UTC, Instant at noon UTC.
const years = [
    [0, 6, 7, "0000-06-07", "0000-06-07T01:02:03.004005006", "0000-06", "0000-06-07T12:00:00+00:00[UTC]", "0000-06-07T12:00:00Z"],
    [-1, 6, 7, "-000001-06-07", "-000001-06-07T01:02:03.004005006", "-000001-06", "-000001-06-07T12:00:00+00:00[UTC]", "-000001-06-07T12:00:00Z"],
    [1, 6, 7, "0001-06-07", "0001-06-07T01:02:03.004005006", "0001-06", "0001-06-07T12:00:00+00:00[UTC]", "0001-06-07T12:00:00Z"],
    [999, 6, 7, "0999-06-07", "0999-06-07T01:02:03.004005006", "0999-06", "0999-06-07T12:00:00+00:00[UTC]", "0999-06-07T12:00:00Z"],
    [-999, 6, 7, "-000999-06-07", "-000999-06-07T01:02:03.004005006", "-000999-06", "-000999-06-07T12:00:00+00:00[UTC]", "-000999-06-07T12:00:00Z"],
    [9999, 6, 7, "9999-06-07", "9999-06-07T01:02:03.004005006", "9999-06", "9999-06-07T12:00:00+00:00[UTC]", "9999-06-07T12:00:00Z"],
    [10000, 6, 7, "+010000-06-07", "+010000-06-07T01:02:03.004005006", "+010000-06", "+010000-06-07T12:00:00+00:00[UTC]", "+010000-06-07T12:00:00Z"],
    [-10000, 6, 7, "-010000-06-07", "-010000-06-07T01:02:03.004005006", "-010000-06", "-010000-06-07T12:00:00+00:00[UTC]", "-010000-06-07T12:00:00Z"],
    [-271821, 4, 20, "-271821-04-20", "-271821-04-20T01:02:03.004005006", "-271821-04", "-271821-04-20T12:00:00+00:00[UTC]", "-271821-04-20T12:00:00Z"],
    [275760, 9, 12, "+275760-09-12", "+275760-09-12T01:02:03.004005006", "+275760-09", "+275760-09-12T12:00:00+00:00[UTC]", "+275760-09-12T12:00:00Z"],
];

function formatDate(date) { return date.toString(); }
noInline(formatDate);
function formatDateTime(dateTime) { return dateTime.toString(); }
noInline(formatDateTime);

for (let i = 0; i < iterations; i++) {
    for (const [year, month, day, date, dateTime, yearMonth, zoned, instant] of years) {
        shouldBe(formatDate(new Temporal.PlainDate(year, month, day)), date, `PlainDate ${year}`);
        shouldBe(formatDateTime(new Temporal.PlainDateTime(year, month, day, 1, 2, 3, 4, 5, 6)), dateTime, `PlainDateTime ${year}`);
    }
}
for (const [year, month, day, date, dateTime, yearMonth, zoned, instant] of years) {
    shouldBe(new Temporal.PlainYearMonth(year, month).toString(), yearMonth, `PlainYearMonth ${year}`);
    const zdt = new Temporal.PlainDateTime(year, month, day, 12).toZonedDateTime("UTC");
    shouldBe(zdt.toString(), zoned, `ZonedDateTime ${year}`);
    shouldBe(zdt.toInstant().toString(), instant, `Instant ${year}`);
    shouldBe(Temporal.PlainDate.from(date).toString(), date, `PlainDate round trip ${year}`);
    shouldBe(Temporal.Instant.from(instant).toString(), instant, `Instant round trip ${year}`);
}
shouldBe(new Temporal.PlainDate(-271821, 4, 19).toString(), "-271821-04-19", "PlainDate minimum");
shouldBe(new Temporal.PlainDate(275760, 9, 13).toString(), "+275760-09-13", "PlainDate maximum");
shouldBe(new Temporal.PlainDateTime(-271821, 4, 19, 0, 0, 0, 0, 0, 1).toString(), "-271821-04-19T00:00:00.000000001", "PlainDateTime minimum");
shouldBe(new Temporal.PlainDateTime(275760, 9, 13, 23, 59, 59, 999, 999, 999).toString(), "+275760-09-13T23:59:59.999999999", "PlainDateTime maximum");
shouldBe(new Temporal.Instant(-8640000000000000000000n).toString(), "-271821-04-20T00:00:00Z", "Instant minimum");
shouldBe(new Temporal.Instant(8640000000000000000000n).toString(), "+275760-09-13T00:00:00Z", "Instant maximum");

const pdt = new Temporal.PlainDateTime(2021, 3, 14, 1, 2, 3, 456, 789, 12);
const time = new Temporal.PlainTime(23, 59, 59, 999, 999, 999);
const instant = new Temporal.Instant(1615705199999999999n);
const zdt = instant.toZonedDateTimeISO("America/New_York");

// fractionalSecondDigits. Columns: digits, PlainDateTime, PlainTime, Instant, ZonedDateTime.
const digits = [
    ["auto", "2021-03-14T01:02:03.456789012", "23:59:59.999999999", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]"],
    [0, "2021-03-14T01:02:03", "23:59:59", "2021-03-14T06:59:59Z", "2021-03-14T01:59:59-05:00[America/New_York]"],
    [1, "2021-03-14T01:02:03.4", "23:59:59.9", "2021-03-14T06:59:59.9Z", "2021-03-14T01:59:59.9-05:00[America/New_York]"],
    [2, "2021-03-14T01:02:03.45", "23:59:59.99", "2021-03-14T06:59:59.99Z", "2021-03-14T01:59:59.99-05:00[America/New_York]"],
    [3, "2021-03-14T01:02:03.456", "23:59:59.999", "2021-03-14T06:59:59.999Z", "2021-03-14T01:59:59.999-05:00[America/New_York]"],
    [4, "2021-03-14T01:02:03.4567", "23:59:59.9999", "2021-03-14T06:59:59.9999Z", "2021-03-14T01:59:59.9999-05:00[America/New_York]"],
    [5, "2021-03-14T01:02:03.45678", "23:59:59.99999", "2021-03-14T06:59:59.99999Z", "2021-03-14T01:59:59.99999-05:00[America/New_York]"],
    [6, "2021-03-14T01:02:03.456789", "23:59:59.999999", "2021-03-14T06:59:59.999999Z", "2021-03-14T01:59:59.999999-05:00[America/New_York]"],
    [7, "2021-03-14T01:02:03.4567890", "23:59:59.9999999", "2021-03-14T06:59:59.9999999Z", "2021-03-14T01:59:59.9999999-05:00[America/New_York]"],
    [8, "2021-03-14T01:02:03.45678901", "23:59:59.99999999", "2021-03-14T06:59:59.99999999Z", "2021-03-14T01:59:59.99999999-05:00[America/New_York]"],
    [9, "2021-03-14T01:02:03.456789012", "23:59:59.999999999", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]"],
];

function formatWith(value, options) { return value.toString(options); }
noInline(formatWith);

for (let i = 0; i < iterations; i++) {
    const [fractionalSecondDigits, dateTime, plainTime, instantString, zoned] = digits[i % digits.length];
    const options = { fractionalSecondDigits };
    shouldBe(formatWith(pdt, options), dateTime, `PlainDateTime digits ${fractionalSecondDigits}`);
    shouldBe(formatWith(time, options), plainTime, `PlainTime digits ${fractionalSecondDigits}`);
    shouldBe(formatWith(instant, options), instantString, `Instant digits ${fractionalSecondDigits}`);
    shouldBe(formatWith(zdt, options), zoned, `ZonedDateTime digits ${fractionalSecondDigits}`);
}

// smallestUnit with roundingMode. Columns: smallestUnit, roundingMode, PlainDateTime, PlainTime, Instant, ZonedDateTime.
// Rounding the ZonedDateTime up lands in the New York DST gap and is reported after the transition.
const rounding = [
    ["minute", "trunc", "2021-03-14T01:02", "23:59", "2021-03-14T06:59Z", "2021-03-14T01:59-05:00[America/New_York]"],
    ["minute", "halfExpand", "2021-03-14T01:02", "00:00", "2021-03-14T07:00Z", "2021-03-14T03:00-04:00[America/New_York]"],
    ["minute", "ceil", "2021-03-14T01:03", "00:00", "2021-03-14T07:00Z", "2021-03-14T03:00-04:00[America/New_York]"],
    ["minute", "floor", "2021-03-14T01:02", "23:59", "2021-03-14T06:59Z", "2021-03-14T01:59-05:00[America/New_York]"],
    ["second", "trunc", "2021-03-14T01:02:03", "23:59:59", "2021-03-14T06:59:59Z", "2021-03-14T01:59:59-05:00[America/New_York]"],
    ["second", "halfExpand", "2021-03-14T01:02:03", "00:00:00", "2021-03-14T07:00:00Z", "2021-03-14T03:00:00-04:00[America/New_York]"],
    ["second", "ceil", "2021-03-14T01:02:04", "00:00:00", "2021-03-14T07:00:00Z", "2021-03-14T03:00:00-04:00[America/New_York]"],
    ["second", "floor", "2021-03-14T01:02:03", "23:59:59", "2021-03-14T06:59:59Z", "2021-03-14T01:59:59-05:00[America/New_York]"],
    ["millisecond", "trunc", "2021-03-14T01:02:03.456", "23:59:59.999", "2021-03-14T06:59:59.999Z", "2021-03-14T01:59:59.999-05:00[America/New_York]"],
    ["millisecond", "halfExpand", "2021-03-14T01:02:03.457", "00:00:00.000", "2021-03-14T07:00:00.000Z", "2021-03-14T03:00:00.000-04:00[America/New_York]"],
    ["millisecond", "ceil", "2021-03-14T01:02:03.457", "00:00:00.000", "2021-03-14T07:00:00.000Z", "2021-03-14T03:00:00.000-04:00[America/New_York]"],
    ["millisecond", "floor", "2021-03-14T01:02:03.456", "23:59:59.999", "2021-03-14T06:59:59.999Z", "2021-03-14T01:59:59.999-05:00[America/New_York]"],
    ["microsecond", "trunc", "2021-03-14T01:02:03.456789", "23:59:59.999999", "2021-03-14T06:59:59.999999Z", "2021-03-14T01:59:59.999999-05:00[America/New_York]"],
    ["microsecond", "halfExpand", "2021-03-14T01:02:03.456789", "00:00:00.000000", "2021-03-14T07:00:00.000000Z", "2021-03-14T03:00:00.000000-04:00[America/New_York]"],
    ["microsecond", "ceil", "2021-03-14T01:02:03.456790", "00:00:00.000000", "2021-03-14T07:00:00.000000Z", "2021-03-14T03:00:00.000000-04:00[America/New_York]"],
    ["microsecond", "floor", "2021-03-14T01:02:03.456789", "23:59:59.999999", "2021-03-14T06:59:59.999999Z", "2021-03-14T01:59:59.999999-05:00[America/New_York]"],
    ["nanosecond", "trunc", "2021-03-14T01:02:03.456789012", "23:59:59.999999999", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]"],
    ["nanosecond", "halfExpand", "2021-03-14T01:02:03.456789012", "23:59:59.999999999", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]"],
    ["nanosecond", "ceil", "2021-03-14T01:02:03.456789012", "23:59:59.999999999", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]"],
    ["nanosecond", "floor", "2021-03-14T01:02:03.456789012", "23:59:59.999999999", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]"],
];

for (let i = 0; i < iterations; i++) {
    const [smallestUnit, roundingMode, dateTime, plainTime, instantString, zoned] = rounding[i % rounding.length];
    // smallestUnit wins over fractionalSecondDigits.
    const options = { fractionalSecondDigits: 9, roundingMode, smallestUnit };
    shouldBe(formatWith(pdt, options), dateTime, `PlainDateTime ${smallestUnit} ${roundingMode}`);
    shouldBe(formatWith(time, options), plainTime, `PlainTime ${smallestUnit} ${roundingMode}`);
    shouldBe(formatWith(instant, options), instantString, `Instant ${smallestUnit} ${roundingMode}`);
    shouldBe(formatWith(zdt, options), zoned, `ZonedDateTime ${smallestUnit} ${roundingMode}`);
}

shouldBe(pdt.toString({ fractionalSecondDigits: 2.9 }), "2021-03-14T01:02:03.45", "truncated digits");
shouldThrow(() => pdt.toString({ smallestUnit: "hour" }), RangeError, "hour smallestUnit");
shouldThrow(() => pdt.toString({ fractionalSecondDigits: 10 }), RangeError, "10 digits");
shouldThrow(() => pdt.toString({ fractionalSecondDigits: -1 }), RangeError, "-1 digits");
shouldThrow(() => pdt.toString({ fractionalSecondDigits: "9" }), RangeError, "string digits");
shouldThrow(() => pdt.toString({ fractionalSecondDigits: Infinity }), RangeError, "Infinity digits");
shouldBe(new Temporal.PlainDateTime(275760, 9, 13).toString({ smallestUnit: "minute" }), "+275760-09-13T00:00", "maximum date-time minute");
shouldThrow(() => new Temporal.PlainDateTime(275760, 9, 13, 23, 59, 59, 999).toString({ smallestUnit: "second", roundingMode: "ceil" }), RangeError, "rounding past maximum date-time");
shouldBe(new Temporal.Instant(8640000000000000000000n).toString({ smallestUnit: "minute", roundingMode: "ceil" }), "+275760-09-13T00:00Z", "maximum instant minute");
shouldBe(new Temporal.Instant(8639999999999999999999n).toString({ smallestUnit: "second", roundingMode: "ceil" }), "+275760-09-13T00:00:00Z", "rounding up to maximum instant");

// Offsets. Columns: time zone, instant, toString(), offset, offsetNanoseconds, Instant#toString({ timeZone }),
// offset: "never", timeZoneName: "never", timeZoneName: "critical".
const offsets = [
    ["+05:30", "2021-03-14T06:59:59.999999999Z", "2021-03-14T12:29:59.999999999+05:30[+05:30]", "+05:30", 19800000000000, "2021-03-14T12:29:59.999999999+05:30", "2021-03-14T12:29:59.999999999[+05:30]", "2021-03-14T12:29:59.999999999+05:30", "2021-03-14T12:29:59.999999999+05:30[!+05:30]"],
    ["-00:01", "1900-06-01T00:00:00Z", "1900-05-31T23:59:00-00:01[-00:01]", "-00:01", -60000000000, "1900-05-31T23:59:00-00:01", "1900-05-31T23:59:00[-00:01]", "1900-05-31T23:59:00-00:01", "1900-05-31T23:59:00-00:01[!-00:01]"],
    ["+23:59", "2021-03-14T06:59:59.999999999Z", "2021-03-15T06:58:59.999999999+23:59[+23:59]", "+23:59", 86340000000000, "2021-03-15T06:58:59.999999999+23:59", "2021-03-15T06:58:59.999999999[+23:59]", "2021-03-15T06:58:59.999999999+23:59", "2021-03-15T06:58:59.999999999+23:59[!+23:59]"],
    ["-23:59", "1900-06-01T00:00:00Z", "1900-05-31T00:01:00-23:59[-23:59]", "-23:59", -86340000000000, "1900-05-31T00:01:00-23:59", "1900-05-31T00:01:00[-23:59]", "1900-05-31T00:01:00-23:59", "1900-05-31T00:01:00-23:59[!-23:59]"],
    ["+00:00", "1880-01-01T00:00:00Z", "1880-01-01T00:00:00+00:00[+00:00]", "+00:00", 0, "1880-01-01T00:00:00+00:00", "1880-01-01T00:00:00[+00:00]", "1880-01-01T00:00:00+00:00", "1880-01-01T00:00:00+00:00[!+00:00]"],
    ["America/New_York", "2021-03-14T06:59:59.999999999Z", "2021-03-14T01:59:59.999999999-05:00[America/New_York]", "-05:00", -18000000000000, "2021-03-14T01:59:59.999999999-05:00", "2021-03-14T01:59:59.999999999[America/New_York]", "2021-03-14T01:59:59.999999999-05:00", "2021-03-14T01:59:59.999999999-05:00[!America/New_York]"],
];

// Offsets with seconds exist only in the historic LMT and early standard times of named zones, whose exact values depend
// on the tzdata version. These rows take the offset from Intl.DateTimeFormat. In current tzdata they are New York
// -04:56:02, Kolkata +05:21:10, Monrovia -00:43:08 and Monrovia -00:44:30 (a half-minute tie, rounded away from zero);
// only the presence of some offset with seconds is asserted.
function formatOffsetNanoseconds(offsetNanoseconds) {
    const seconds = Math.abs(offsetNanoseconds) / 1e9;
    const pad = n => String(n).padStart(2, "0");
    const hoursAndMinutes = `${offsetNanoseconds < 0 ? "-" : "+"}${pad(Math.floor(seconds / 3600))}:${pad(Math.floor(seconds / 60) % 60)}`;
    return seconds % 60 ? `${hoursAndMinutes}:${pad(seconds % 60)}` : hoursAndMinutes;
}
for (const [timeZone, instantString] of [["America/New_York", "1880-01-01T00:00:00Z"], ["Asia/Kolkata", "1900-06-01T00:00:00Z"], ["Africa/Monrovia", "1900-06-01T00:00:00Z"], ["Africa/Monrovia", "1937-06-01T00:00:00Z"]]) {
    const instant = Temporal.Instant.from(instantString);
    const name = new Intl.DateTimeFormat("en-US", { timeZone, timeZoneName: "longOffset" }).formatToParts(instant.epochMilliseconds).find(part => part.type === "timeZoneName").value;
    const [, sign, hours, minutes, seconds] = /^GMT(?:([+-])(\d\d):(\d\d)(?::(\d\d))?)?$/.exec(name);
    const offsetNanoseconds = sign ? (sign === "-" ? -1 : 1) * ((Number(hours) * 60 + Number(minutes)) * 60 + Number(seconds ?? 0)) * 1e9 : 0;
    const rounded = formatOffsetNanoseconds(Math.sign(offsetNanoseconds) * Math.round(Math.abs(offsetNanoseconds) / 6e10) * 6e10);
    const local = instant.add({ nanoseconds: offsetNanoseconds }).toString().slice(0, -1);
    offsets.push([timeZone, instantString, `${local}${rounded}[${timeZone}]`, formatOffsetNanoseconds(offsetNanoseconds), offsetNanoseconds, `${local}${rounded}`, `${local}[${timeZone}]`, `${local}${rounded}`, `${local}${rounded}[!${timeZone}]`]);
}
shouldBe(offsets.some(([, , , offset]) => offset.length > "+00:00".length), true, "an offset with seconds is covered");

for (let round = 0; round < 50; round++) {
    for (const [timeZone, instantString, string, offset, offsetNanoseconds, withTimeZone, offsetNever, nameNever, nameCritical] of offsets) {
        const instant = Temporal.Instant.from(instantString);
        const zoned = instant.toZonedDateTimeISO(timeZone);
        shouldBe(zoned.toString(), string, `${timeZone} ${instantString}`);
        shouldBe(zoned.offset, offset, `${timeZone} ${instantString} offset`);
        shouldBe(zoned.offsetNanoseconds, offsetNanoseconds, `${timeZone} ${instantString} offsetNanoseconds`);
        shouldBe(instant.toString({ timeZone }), withTimeZone, `${timeZone} ${instantString} Instant timeZone`);
        shouldBe(zoned.toString({ offset: "never" }), offsetNever, `${timeZone} ${instantString} offset never`);
        shouldBe(zoned.toString({ timeZoneName: "never" }), nameNever, `${timeZone} ${instantString} timeZoneName never`);
        shouldBe(zoned.toString({ timeZoneName: "critical" }), nameCritical, `${timeZone} ${instantString} timeZoneName critical`);
        // A minute-rounded offset string still matches the exact sub-minute offset when parsed back.
        shouldBe(Temporal.ZonedDateTime.from(string).epochNanoseconds, zoned.epochNanoseconds, `${timeZone} ${instantString} round trip`);
    }
}

// calendarName. Columns: option, ZonedDateTime (japanese), PlainDate (japanese), PlainDate (iso8601),
// PlainDateTime (hebrew), PlainYearMonth (iso8601), PlainMonthDay (iso8601).
const zonedJapanese = Temporal.ZonedDateTime.from("2021-03-14T12:00[Asia/Tokyo][u-ca=japanese]");
const dateJapanese = Temporal.PlainDate.from("2021-03-14[u-ca=japanese]");
const dateISO = Temporal.PlainDate.from("2021-03-14");
const dateTimeHebrew = pdt.withCalendar("hebrew");
const yearMonth = Temporal.PlainYearMonth.from({ year: 2021, month: 3 });
const monthDay = Temporal.PlainMonthDay.from({ month: 3, day: 14 });
const calendarNames = [
    ["auto", "2021-03-14T12:00:00+09:00[Asia/Tokyo][u-ca=japanese]", "2021-03-14[u-ca=japanese]", "2021-03-14", "2021-03-14T01:02:03.456789012[u-ca=hebrew]", "2021-03", "03-14"],
    ["always", "2021-03-14T12:00:00+09:00[Asia/Tokyo][u-ca=japanese]", "2021-03-14[u-ca=japanese]", "2021-03-14[u-ca=iso8601]", "2021-03-14T01:02:03.456789012[u-ca=hebrew]", "2021-03-01[u-ca=iso8601]", "1972-03-14[u-ca=iso8601]"],
    ["never", "2021-03-14T12:00:00+09:00[Asia/Tokyo]", "2021-03-14", "2021-03-14", "2021-03-14T01:02:03.456789012", "2021-03", "03-14"],
    ["critical", "2021-03-14T12:00:00+09:00[Asia/Tokyo][!u-ca=japanese]", "2021-03-14[!u-ca=japanese]", "2021-03-14[!u-ca=iso8601]", "2021-03-14T01:02:03.456789012[!u-ca=hebrew]", "2021-03-01[!u-ca=iso8601]", "1972-03-14[!u-ca=iso8601]"],
];

for (let i = 0; i < iterations; i++) {
    const [calendarName, zoned, japanese, iso, hebrew, ym, md] = calendarNames[i % calendarNames.length];
    const options = { calendarName };
    shouldBe(formatWith(zonedJapanese, options), zoned, `ZonedDateTime calendarName ${calendarName}`);
    shouldBe(formatWith(dateJapanese, options), japanese, `japanese PlainDate calendarName ${calendarName}`);
    shouldBe(formatWith(dateISO, options), iso, `iso8601 PlainDate calendarName ${calendarName}`);
    shouldBe(formatWith(dateTimeHebrew, options), hebrew, `hebrew PlainDateTime calendarName ${calendarName}`);
    shouldBe(formatWith(yearMonth, options), ym, `PlainYearMonth calendarName ${calendarName}`);
    shouldBe(formatWith(monthDay, options), md, `PlainMonthDay calendarName ${calendarName}`);
}

shouldBe(zonedJapanese.toString({ calendarName: "never", timeZoneName: "never", offset: "never", fractionalSecondDigits: 3 }), "2021-03-14T12:00:00.000", "all annotations off");
shouldBe(zonedJapanese.toString({ calendarName: "critical", timeZoneName: "critical", smallestUnit: "minute" }), "2021-03-14T12:00+09:00[!Asia/Tokyo][!u-ca=japanese]", "all annotations critical");
shouldThrow(() => zonedJapanese.toString({ calendarName: "bogus" }), RangeError, "bad calendarName");
shouldThrow(() => zonedJapanese.toString({ offset: "bogus" }), RangeError, "bad offset");
shouldThrow(() => zonedJapanese.toString({ timeZoneName: "bogus" }), RangeError, "bad timeZoneName");
