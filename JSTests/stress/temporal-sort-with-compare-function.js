//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(`${msg}: expected ${expected} but got ${actual}`);
}

function shouldThrow(fn, errorType, msg) {
    try {
        fn();
    } catch (e) {
        if (!(e instanceof errorType))
            throw new Error(`${msg}: expected ${errorType.name} but got ${e}`);
        return;
    }
    throw new Error(`${msg}: expected ${errorType.name} to be thrown`);
}

// Sorting with the compare function itself must give the same stable order as calling it from a JS comparator.
function checkSort(values, compare, msg) {
    const expected = [...values].sort((a, b) => compare(a, b));
    const actual = [...values].sort(compare);
    for (let i = 0; i < values.length; i++)
        shouldBe(actual[i], expected[i], `${msg} [${i}]`);
}

// Pseudo-random milliseconds with ties, so that stability is visible.
const milliseconds = [];
for (let i = 0, seed = 12345; i < 100; i++) {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    milliseconds.push(i % 7 ? (seed - 1073741824) * 1e6 + (i % 5) * 1e3 : 0);
}
const instants = milliseconds.map(ms => Temporal.Instant.fromEpochMilliseconds(ms));
instants.push(new Temporal.Instant(-8640000000000000000000n), new Temporal.Instant(8640000000000000000000n), new Temporal.Instant(-1n));
const zoned = instants.map((instant, i) => instant.toZonedDateTimeISO(i & 1 ? "America/New_York" : "+05:30"));
const dates = zoned.map((z, i) => z.toPlainDate().withCalendar(i % 3 ? "iso8601" : "japanese"));
const durations = milliseconds.map((ms, i) => Temporal.Duration.from(i % 5 ? { days: i % 4, hours: Math.abs(Math.trunc(ms / 1e6) % 1000), microseconds: i } : { hours: -Math.abs(Math.trunc(ms / 1e6) % 1000), nanoseconds: -i }));
// A day and 24 hours are equal without relativeTo.
durations.push(Temporal.Duration.from({ hours: 24 }), Temporal.Duration.from({ days: 1 }), Temporal.Duration.from({ seconds: Number.MAX_SAFE_INTEGER, nanoseconds: 999999999 }));

checkSort(instants, Temporal.Instant.compare, "Instant");
checkSort(zoned, Temporal.ZonedDateTime.compare, "ZonedDateTime");
checkSort(dates, Temporal.PlainDate.compare, "PlainDate");
checkSort(zoned.map(z => z.toPlainDateTime()), Temporal.PlainDateTime.compare, "PlainDateTime");
checkSort(zoned.map(z => z.toPlainTime()), Temporal.PlainTime.compare, "PlainTime");
checkSort(dates.map(d => d.withCalendar("iso8601").toPlainYearMonth()), Temporal.PlainYearMonth.compare, "PlainYearMonth");
checkSort(durations, Temporal.Duration.compare, "Duration");
{
    const day = Temporal.Duration.from({ days: 1 });
    const hours23 = Temporal.Duration.from({ hours: 23 });
    const hours24 = Temporal.Duration.from({ hours: 24 });
    const sorted = [hours24, day, hours23].sort(Temporal.Duration.compare);
    shouldBe(sorted[0], hours23, "23 hours first");
    shouldBe(sorted[1], hours24, "then 24 hours");
    shouldBe(sorted[2], day, "then a day");
}

// Elements without the internal slots, and Durations with years, months or weeks, take the generic path: they are
// converted, or the compare function throws.
checkSort([...instants.slice(0, 20), zoned[3], instants[5].toString()], Temporal.Instant.compare, "Instant with ZonedDateTime and string");
shouldThrow(() => [instants[0], 42].sort(Temporal.Instant.compare), TypeError, "Instant.compare with a number");
shouldThrow(() => [Temporal.Duration.from({ months: 1 }), Temporal.Duration.from({ days: 30 })].sort(Temporal.Duration.compare), RangeError, "Duration.compare with months");

// Holes and undefined never reach the comparator.
{
    const sorted = [instants[3], undefined, instants[1], , instants[2]].sort(Temporal.Instant.compare);
    const expected = [instants[1], instants[2], instants[3]].sort((a, b) => Temporal.Instant.compare(a, b));
    for (let i = 0; i < expected.length; i++)
        shouldBe(sorted[i], expected[i], `sparse [${i}]`);
    shouldBe(sorted[3], undefined, "undefined after the values");
    shouldBe(4 in sorted, false, "hole moved to the end");
}

// The DFG sorts arrays of at most 16 elements itself, calling the compare function for each pair.
function sortInstants(values) { return values.sort(Temporal.Instant.compare); }
noInline(sortInstants);
const small = instants.slice(0, 12);
const expected = [...small].sort((a, b) => Temporal.Instant.compare(a, b));
for (let i = 0; i < testLoopCount; i++)
    shouldBe(sortInstants([...small])[i % small.length], expected[i % small.length], "hot sort");
