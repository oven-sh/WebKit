//@ requireOptions("--useTemporal=1")
// Bun: not upstream's. This fork has no era override for a date before the epoch of an islamic-* or the coptic calendar
// (BUN-TEST-DIFFERENCES.md, "Intl.DateTimeFormat prints ICU's own era and year"): every method of one
// Intl.DateTimeFormat prints ICU's own era and year. Upstream's override changes what format() and formatToParts()
// print, and so formatRange() of two Numbers or Dates that print as one date. It does not change formatRangeToParts(),
// formatRange() of Temporal values, or a true range.
// The checks compare the methods with each other and name no era text, so they hold for every version of ICU.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${JSON.stringify(want)}, got ${JSON.stringify(got)}`);
}

function join(parts)
{
    return parts.map(part => part.value).join("");
}

function yearOf(parts, source)
{
    const part = parts.find(part => part.type === "year" && (!source || part.source === source));
    return part && part.value;
}

const msPerHour = 3600000;
const msPerDay = 24 * msPerHour;

const calendars = ["islamic-civil", "islamic-tbla", "islamic-umalqura", "coptic"];
const locales = ["en", "sv", "ar", "ja"];
const shapes = [
    { era: "long", year: "numeric", month: "short", day: "numeric" },
    { era: "short", year: "numeric", month: "short", day: "numeric" },
    { era: "narrow", year: "numeric", month: "short", day: "numeric" },
    // No era option: the CLDR patterns of these calendars have an era field next to a year.
    { year: "numeric", month: "short", day: "numeric" },
    { dateStyle: "full" },
];
// Before the epoch of each calendar: ISO 284 for coptic, ISO 622 for islamic-*.
const isoDates = ["-000400-01-15", "0200-01-01", "0283-08-01"];

function valuesFor(isoDate, calendar)
{
    const plainDate = Temporal.PlainDate.from(isoDate);
    const epochMilliseconds = plainDate.toZonedDateTime("UTC").epochMilliseconds;
    return [
        ["Number", epochMilliseconds],
        ["Date", new Date(epochMilliseconds)],
        ["Instant", Temporal.Instant.fromEpochMilliseconds(epochMilliseconds)],
        ["PlainDate", plainDate.withCalendar(calendar)],
        ["PlainDateTime", plainDate.toPlainDateTime("12:00").withCalendar(calendar)],
        ["PlainYearMonth", plainDate.withCalendar(calendar).toPlainYearMonth()],
    ];
}

let checked = 0;
for (const calendar of calendars) {
    for (const locale of locales) {
        for (const shape of shapes) {
            const format = new Intl.DateTimeFormat(locale, { calendar, timeZone: "UTC", ...shape });
            const label = `${locale} ${calendar} ${JSON.stringify(shape)}`;

            // One value, four methods, one text.
            for (const isoDate of isoDates) {
                for (const [kind, value] of valuesFor(isoDate, calendar)) {
                    const text = format.format(value);
                    const rangeParts = format.formatRangeToParts(value, value);
                    expect(`${label} ${kind} ${isoDate} formatToParts`, join(format.formatToParts(value)), text);
                    expect(`${label} ${kind} ${isoDate} formatRange`, format.formatRange(value, value), text);
                    expect(`${label} ${kind} ${isoDate} formatRangeToParts`, join(rangeParts), text);
                    for (const part of rangeParts)
                        expect(`${label} ${kind} ${isoDate} source of the ${part.type} part`, part.source, "shared");
                    ++checked;
                }
            }

            // Each end of a true range has the year that formatToParts() gives that date.
            const start = Temporal.PlainDate.from(isoDates[0]).toZonedDateTime("UTC").epochMilliseconds;
            const end = start + 366 * msPerDay;
            const rangeParts = format.formatRangeToParts(start, end);
            expect(`${label} range, year of the start`, yearOf(rangeParts, "startRange"), yearOf(format.formatToParts(start)));
            expect(`${label} range, year of the end`, yearOf(rangeParts, "endRange"), yearOf(format.formatToParts(end)));
        }
    }
}
expect("values checked", checked, calendars.length * locales.length * shapes.length * isoDates.length * 6);

// Every instant of one day in the formatter's time zone prints the same date, on the days around the epoch too.
// A range of two such instants is that one date, in either order.
const epochs = [
    ["islamic-civil", Date.UTC(622, 6, 19)],
    ["islamic-umalqura", Date.UTC(622, 6, 19)],
    ["islamic-tbla", Date.UTC(622, 6, 18)],
    ["coptic", Date.UTC(284, 7, 29)],
];
for (const [calendar, epoch] of epochs) {
    for (const shape of shapes) {
        const format = new Intl.DateTimeFormat("en", { calendar, timeZone: "UTC", ...shape });
        const texts = new Set;
        for (let day = -2; day <= 1; ++day) {
            const midnight = epoch + day * msPerDay;
            const text = format.format(midnight);
            const label = `${calendar} ${JSON.stringify(shape)} day ${day}`;
            for (let hour = 1; hour < 24; ++hour)
                expect(`${label} hour ${hour}`, format.format(midnight + hour * msPerHour), text);
            expect(`${label} range`, format.formatRange(midnight + 3 * msPerHour, midnight + 12 * msPerHour), text);
            expect(`${label} reversed range`, format.formatRange(midnight + 12 * msPerHour, midnight + 3 * msPerHour), text);
            texts.add(text);
        }
        expect(`${calendar} ${JSON.stringify(shape)} dates of four days`, texts.size, 4);
    }
}
