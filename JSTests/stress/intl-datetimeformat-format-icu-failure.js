//@ requireOptions("--useTemporal=1")

// When ICU fails to compute a date in the calendar of a formatter, formatToParts() and the range methods throw a
// TypeError. format() has to throw it too: it must not return the text that ICU made before it failed (" ", "/", ":").
// Which dates fail depends on ICU: chinese and dangi far from the present since ICU 77 (ICU-23043), and two days of
// the hebrew year 193151 before ICU 79 (ICU-23007). So every expectation here comes from formatToParts() at run time.
//
// Date.prototype.toLocaleString(), toLocaleDateString() and toLocaleTimeString() are `! FormatDateTime()` in ECMA-402.
// They return a string for every time value.

function fail(label, message)
{
    throw new Error(`${label}: ${message}`);
}

function outcome(fn)
{
    try {
        return { text: fn() };
    } catch (error) {
        return { error };
    }
}

function describe(result)
{
    return result.error ? `${result.error.constructor.name}: ${result.error.message}` : JSON.stringify(result.text);
}

function join(parts)
{
    return parts.map(part => part.value).join("");
}

// ICU either computes the date or it does not: the only error a valid time value can give is the TypeError.
function referenceOf(label, fn)
{
    const reference = outcome(fn);
    if (reference.error && !(reference.error instanceof TypeError))
        fail(label, `formatToParts() threw ${describe(reference)}`);
    return reference;
}

// The same text, or the same TypeError.
function shouldAgree(label, fn, reference)
{
    const result = outcome(fn);
    if (describe(result) !== describe(reference))
        fail(label, `expected ${describe(reference)}, got ${describe(result)}`);
}

// A text when the reference is a text, and a TypeError when the reference is one. The range methods have their own
// message, and a Temporal.ZonedDateTime prints the name of its time zone.
function shouldFailTogether(label, fn, reference)
{
    const result = outcome(fn);
    const ok = reference.error ? result.error instanceof TypeError : typeof result.text === "string";
    if (!ok)
        fail(label, `formatToParts() gives ${describe(reference)}, got ${describe(result)}`);
}

function shouldBeString(label, fn)
{
    const result = outcome(fn);
    if (typeof result.text !== "string")
        fail(label, `expected a string, got ${describe(result)}`);
}

const msPerDay = 86400000;
const hebrewDay = 5914526860800000; // +189393-11-23T00:00:00Z
const candidates = [
    ["hebrew", "UTC", hebrewDay - msPerDay],
    ["hebrew", "UTC", hebrewDay],
    ["hebrew", "UTC", hebrewDay + msPerDay],
    ["hebrew", "UTC", hebrewDay + 2 * msPerDay],
    ["hebrew", "UTC", Date.UTC(2024, 0, 15)],
    ["chinese", "UTC", 2208924950400000], // +071968-01-18T00:00:00Z
    ["chinese", "UTC", 8.64e15],
    ["chinese", "UTC", Date.UTC(-50000, 0, 3)],
    ["chinese", "Europe/Moscow", 3363150585600000], // The date of ICU's own test for ICU-23043.
    ["chinese", "UTC", 0],
    ["dangi", "UTC", 2208924950400000],
    ["dangi", "UTC", -8.64e15],
    ["dangi", "UTC", Date.UTC(2024, 0, 15)],
    ["gregory", "UTC", 8.64e15],
];

const shapes = [
    { year: "numeric", month: "short", day: "numeric" },
    // Longer than the buffer that the first call of ICU gets.
    { dateStyle: "full", timeStyle: "full" },
    // No date field in the pattern: ICU computes every field of the calendar before it prints one.
    { hour: "numeric", minute: "numeric" },
    { weekday: "long" },
];

function temporalValues(calendar, timeZone, instant)
{
    const zoned = instant.toZonedDateTimeISO(timeZone);
    const values = [
        ["Instant", instant],
        ["PlainDateTime", zoned.toPlainDateTime()],
        ["PlainDate", zoned.toPlainDate()],
    ];
    // A Temporal.PlainYearMonth and a Temporal.PlainMonthDay format only in their own calendar. Not every calendar
    // takes every ISO date here, and that is not what this test is about.
    const ofCalendar = [
        ["PlainDate of the calendar", () => new Temporal.PlainDate(zoned.year, zoned.month, zoned.day, calendar)],
        ["PlainYearMonth", () => new Temporal.PlainYearMonth(zoned.year, zoned.month, calendar, zoned.day)],
        ["PlainMonthDay", () => new Temporal.PlainMonthDay(zoned.month, zoned.day, calendar, zoned.year)],
    ];
    for (const [name, create] of ofCalendar) {
        const created = outcome(create);
        if (!created.error)
            values.push([name, created.text]);
    }
    return values;
}

for (const [calendar, timeZone, timeValue] of candidates) {
    for (const locale of ["en", "zh", "he"]) {
        for (const shape of shapes) {
            const options = { calendar, timeZone, ...shape };
            const label = `${locale} ${calendar} ${timeZone} ${timeValue} ${JSON.stringify(shape)}`;
            const formatter = new Intl.DateTimeFormat(locale, options);
            const boundFormat = formatter.format;
            const date = new Date(timeValue);

            const reference = referenceOf(label, () => join(formatter.formatToParts(timeValue)));
            shouldAgree(`${label}: format(Number)`, () => formatter.format(timeValue), reference);
            shouldAgree(`${label}: format(Date)`, () => formatter.format(date), reference);
            shouldAgree(`${label}: the bound format function`, () => boundFormat(timeValue), reference);
            shouldAgree(`${label}: formatToParts(Date)`, () => join(formatter.formatToParts(date)), reference);
            shouldFailTogether(`${label}: formatRange(Number, Number)`, () => formatter.formatRange(timeValue, timeValue), reference);
            shouldFailTogether(`${label}: formatRangeToParts(Number, Number)`, () => join(formatter.formatRangeToParts(timeValue, timeValue)), reference);
            if (!reference.error) {
                shouldAgree(`${label}: formatRange(Number, Number)`, () => formatter.formatRange(timeValue, timeValue), reference);
                shouldAgree(`${label}: formatRangeToParts(Number, Number)`, () => join(formatter.formatRangeToParts(timeValue, timeValue)), reference);
                shouldAgree(`${label}: Date.prototype.toLocaleString`, () => date.toLocaleString(locale, options), reference);
            }

            shouldBeString(`${label}: Date.prototype.toLocaleString`, () => date.toLocaleString(locale, options));
            shouldBeString(`${label}: Date.prototype.toLocaleDateString`, () => date.toLocaleDateString(locale, { calendar, timeZone }));
            shouldBeString(`${label}: Date.prototype.toLocaleTimeString`, () => date.toLocaleTimeString(locale, { calendar, timeZone }));
            shouldBeString(`${label}: Array.prototype.toLocaleString`, () => [date].toLocaleString(locale, options));

            const instant = Temporal.Instant.fromEpochMilliseconds(timeValue);
            for (const [name, value] of temporalValues(calendar, timeZone, instant)) {
                // A shape can have no field for a type: format() and formatToParts() then throw the same TypeError.
                const temporalReference = referenceOf(`${label}: ${name}`, () => join(formatter.formatToParts(value)));
                shouldAgree(`${label}: format(${name})`, () => formatter.format(value), temporalReference);
                shouldFailTogether(`${label}: formatRange(${name}, ${name})`, () => formatter.formatRange(value, value), temporalReference);
                shouldFailTogether(`${label}: formatRangeToParts(${name}, ${name})`, () => join(formatter.formatRangeToParts(value, value)), temporalReference);
                // toLocaleString() makes its own formatter. With a year, a month and a day it has the fields of this one.
                if (shape === shapes[0])
                    shouldAgree(`${label}: ${name}.prototype.toLocaleString`, () => value.toLocaleString(locale, options), temporalReference);
            }
            shouldFailTogether(`${label}: ZonedDateTime.prototype.toLocaleString`, () => instant.toZonedDateTimeISO(timeZone).toLocaleString(locale, { calendar, ...shape }), reference);
        }
    }
}

// A date that ICU fails to compute leaves nothing behind in the formatter, and such a date can arrive after format()
// has run in every tier. The values of `late` are the ones that some version of ICU fails on.
function formatLateValues(calendar, ordinary, late)
{
    const formatter = new Intl.DateTimeFormat("en", { calendar, timeZone: "UTC", dateStyle: "long" });
    const format = formatter.format;
    function run(timeValue)
    {
        return describe(outcome(() => format(timeValue)));
    }
    noInline(run);

    const expected = ordinary.map(timeValue => describe(referenceOf(`${calendar} ${timeValue}`, () => join(formatter.formatToParts(timeValue)))));
    for (let i = 0; i < testLoopCount; ++i) {
        const index = i % ordinary.length;
        const result = run(ordinary[index]);
        if (result !== expected[index])
            fail(`${calendar}: warm-up`, `expected ${expected[index]}, got ${result}`);
    }
    for (const timeValue of late) {
        const reference = describe(referenceOf(`${calendar} ${timeValue}`, () => join(formatter.formatToParts(timeValue))));
        const result = run(timeValue);
        if (result !== reference)
            fail(`${calendar}: format(${timeValue}) after warm-up`, `expected ${reference}, got ${result}`);
        for (let index = 0; index < ordinary.length; ++index) {
            const after = run(ordinary[index]);
            if (after !== expected[index])
                fail(`${calendar}: format(${ordinary[index]}) after format(${timeValue})`, `expected ${expected[index]}, got ${after}`);
        }
    }
}

formatLateValues("hebrew", [hebrewDay - msPerDay, hebrewDay + 2 * msPerDay, Date.UTC(2024, 0, 15)], [hebrewDay, hebrewDay + msPerDay]);
formatLateValues("chinese", [0, Date.UTC(2024, 0, 15), Date.UTC(1969, 6, 20)], [2208924950400000, 8.64e15, Date.UTC(-50000, 0, 3)]);
