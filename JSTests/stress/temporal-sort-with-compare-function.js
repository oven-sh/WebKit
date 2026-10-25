//@ requireOptions("--useTemporal=1")
// Array.prototype.sort and toSorted with a Temporal compare function as the comparator must give the same
// order (stable) and the same exceptions as calling it from a JS comparator.

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

function shouldBeSameOrder(actual, expected, msg) {
    shouldBe(actual.length, expected.length, `${msg} length`);
    for (let i = 0; i < actual.length; i++)
        shouldBe(actual[i], expected[i], `${msg} [${i}]`);
}

function epochMillisecondsList(count) {
    const list = [];
    let seed = 12345;
    for (let i = 0; i < count; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        list.push((seed - 1073741824) * 1e6 + (i % 5) * 1e3);
    }
    // Ties, so that stability is visible.
    for (let i = 0; i < count; i += 7)
        list[i] = list[0];
    return list;
}

function checkSort(values, compare, msg) {
    const viaFunction = [...values].sort(compare);
    const viaWrapper = [...values].sort((a, b) => compare(a, b));
    shouldBeSameOrder(viaFunction, viaWrapper, `${msg} sort`);
    shouldBeSameOrder(values.toSorted(compare), viaWrapper, `${msg} toSorted`);
    shouldBeSameOrder([...viaWrapper].sort(compare), viaWrapper, `${msg} sorted input`);
    shouldBeSameOrder([...viaWrapper].reverse().sort(compare), [...viaWrapper].reverse().sort((a, b) => compare(a, b)), `${msg} reversed input`);
    for (let i = 1; i < viaFunction.length; i++) {
        if (compare(viaFunction[i - 1], viaFunction[i]) > 0)
            throw new Error(`${msg}: not sorted at ${i}`);
    }
}

const instants = epochMillisecondsList(500).map((ms) => Temporal.Instant.fromEpochMilliseconds(ms));
instants.push(Temporal.Instant.fromEpochNanoseconds(-8640000000000000000000n), Temporal.Instant.fromEpochNanoseconds(8640000000000000000000n), Temporal.Instant.fromEpochNanoseconds(-1n), Temporal.Instant.fromEpochNanoseconds(0n));
const zoned = instants.map((instant, i) => instant.toZonedDateTimeISO(i & 1 ? "America/New_York" : "+05:30"));
const dates = zoned.map((z, i) => z.toPlainDate().withCalendar(i % 3 ? "iso8601" : "japanese"));
const dateTimes = zoned.map((z) => z.toPlainDateTime());
const times = zoned.map((z) => z.toPlainTime());
const yearMonths = dates.map((d) => Temporal.PlainYearMonth.from({ year: d.year < 1 ? 1 : d.year, month: d.withCalendar("iso8601").month }));
const durations = epochMillisecondsList(300).map((ms, i) => (i % 5 ? Temporal.Duration.from({ days: i % 4, hours: Math.abs(ms % 1000), seconds: Math.abs(ms % 7), microseconds: i }) : Temporal.Duration.from({ hours: -Math.abs(ms % 1000), nanoseconds: -i })));
durations.push(Temporal.Duration.from({ hours: -5 }), Temporal.Duration.from({ days: 1 }), Temporal.Duration.from({ hours: 24 }), Temporal.Duration.from({ nanoseconds: Number.MAX_SAFE_INTEGER }));

checkSort(instants, Temporal.Instant.compare, "Instant");
checkSort(zoned, Temporal.ZonedDateTime.compare, "ZonedDateTime");
checkSort(dates, Temporal.PlainDate.compare, "PlainDate");
checkSort(dateTimes, Temporal.PlainDateTime.compare, "PlainDateTime");
checkSort(times, Temporal.PlainTime.compare, "PlainTime");
checkSort(yearMonths, Temporal.PlainYearMonth.compare, "PlainYearMonth");
checkSort(durations, Temporal.Duration.compare, "Duration");

// Durations at the IsValidDuration limits.
checkSort([
    Temporal.Duration.from({ seconds: Number.MAX_SAFE_INTEGER, nanoseconds: 999999999 }),
    Temporal.Duration.from({ seconds: -Number.MAX_SAFE_INTEGER, nanoseconds: -999999999 }),
    Temporal.Duration.from({ days: 104249991374, seconds: 27391, nanoseconds: 999999999 }),
    Temporal.Duration.from({ days: -104249991374 }),
    Temporal.Duration.from({ nanoseconds: 1 }),
    Temporal.Duration.from({ nanoseconds: -1 }),
    new Temporal.Duration(),
], Temporal.Duration.compare, "Duration limits");

// A day and 24 hours are equal without relativeTo, so the stable order keeps them as they were.
{
    const day = Temporal.Duration.from({ days: 1 });
    const hours = Temporal.Duration.from({ hours: 24 });
    const sorted = [hours, Temporal.Duration.from({ hours: 25 }), day].sort(Temporal.Duration.compare);
    shouldBe(sorted[0], hours, "24 hours first");
    shouldBe(sorted[1], day, "then a day");
}

// Elements of another type are converted by the compare function.
checkSort([...instants.slice(0, 20), zoned[3], instants[5].toString()], Temporal.Instant.compare, "Instant with ZonedDateTime and string");
checkSort([...dates.slice(0, 20), dateTimes[4], "2020-01-01"], Temporal.PlainDate.compare, "PlainDate with PlainDateTime and string");
checkSort([...times.slice(0, 20), dateTimes[2]], Temporal.PlainTime.compare, "PlainTime with PlainDateTime");

// Holes and undefined never reach the comparator.
{
    const sparse = [instants[3], undefined, instants[1], , instants[2]];
    const sorted = sparse.sort(Temporal.Instant.compare);
    shouldBe(sorted.length, 5, "sparse length");
    shouldBe(sorted[0], [instants[1], instants[2], instants[3]].sort((a, b) => Temporal.Instant.compare(a, b))[0], "sparse first");
    shouldBe(sorted[3], undefined, "undefined after the values");
    shouldBe(3 in sorted, true, "undefined is an own element");
    shouldBe(4 in sorted, false, "hole moved to the end");
}

// Exceptions come from the compare function exactly as before.
shouldThrow(() => [instants[0], {}].sort(Temporal.Instant.compare), RangeError, "Instant.compare with a plain object");
shouldThrow(() => [instants[0], 42].sort(Temporal.Instant.compare), TypeError, "Instant.compare with a number");
shouldThrow(() => [dates[0], dates[1], instants[0]].sort(Temporal.PlainDate.compare), TypeError, "PlainDate.compare with an Instant");
shouldThrow(() => [times[0], "not a time"].sort(Temporal.PlainTime.compare), RangeError, "PlainTime.compare with a bad string");
shouldThrow(() => [Temporal.Duration.from({ months: 1 }), Temporal.Duration.from({ days: 30 })].sort(Temporal.Duration.compare), RangeError, "Duration.compare with months");
// Equal durations compare as 0 before the calendar-unit check.
{
    const month = Temporal.Duration.from({ months: 1 });
    const sameMonth = Temporal.Duration.from({ months: 1 });
    const sorted = [month, sameMonth].sort(Temporal.Duration.compare);
    shouldBe(sorted[0], month, "equal calendar durations keep their order");
    shouldBe(sorted[1], sameMonth, "equal calendar durations keep their order");
}

// A subclass instance has the internal slots, so it sorts like any other.
{
    class MyInstant extends Temporal.Instant { }
    const values = [new MyInstant(5n), new MyInstant(-5n), instants[0]];
    checkSort(values, Temporal.Instant.compare, "subclass");
}

// Other functions with the same name are not the compare function.
{
    const fake = { compare(a, b) { return Temporal.Instant.compare(b, a); } }.compare;
    const sorted = [...instants].sort(fake);
    const expected = [...instants].sort((a, b) => Temporal.Instant.compare(b, a));
    shouldBeSameOrder(sorted, expected, "a different function named compare");
    const bound = Temporal.Instant.compare.bind(null);
    shouldBeSameOrder([...instants].sort(bound), [...instants].sort((a, b) => Temporal.Instant.compare(a, b)), "bound compare");
}

// A compare function from another realm.
{
    const other = createGlobalObject();
    const otherCompare = other.Temporal.Instant.compare;
    shouldBeSameOrder([...instants].sort(otherCompare), [...instants].sort((a, b) => Temporal.Instant.compare(a, b)), "cross-realm compare");
    const otherInstants = instants.slice(0, 50).map((i) => other.Temporal.Instant.fromEpochNanoseconds(i.epochNanoseconds));
    const sorted = otherInstants.sort(Temporal.Instant.compare);
    for (let i = 1; i < sorted.length; i++)
        shouldBe(Temporal.Instant.compare(sorted[i - 1], sorted[i]) <= 0, true, `cross-realm instants [${i}]`);
}

// Array-likes and frozen arrays behave as with any comparator.
{
    const arrayLike = { length: 3, 0: instants[2], 1: instants[0], 2: instants[1] };
    Array.prototype.sort.call(arrayLike, Temporal.Instant.compare);
    const expected = [instants[2], instants[0], instants[1]].sort((a, b) => Temporal.Instant.compare(a, b));
    for (let i = 0; i < 3; i++)
        shouldBe(arrayLike[i], expected[i], `array-like [${i}]`);
    shouldThrow(() => Object.freeze([instants[1], instants[0]]).sort(Temporal.Instant.compare), TypeError, "frozen array");
}

// Hot sort sites, including the small-array path the DFG inlines.
function sortSmall(values) { return values.sort(Temporal.Instant.compare); }
noInline(sortSmall);
function sortLarge(values) { return values.sort(Temporal.PlainDate.compare); }
noInline(sortLarge);
const small = instants.slice(0, 12);
const smallExpected = [...small].sort((a, b) => Temporal.Instant.compare(a, b));
const large = dates.slice(0, 64);
const largeExpected = [...large].sort((a, b) => Temporal.PlainDate.compare(a, b));
for (let i = 0; i < testLoopCount; i++) {
    shouldBeSameOrder(sortSmall([...small]), smallExpected, "hot small sort");
    if (!(i % 16))
        shouldBeSameOrder(sortLarge([...large]), largeExpected, "hot large sort");
}
