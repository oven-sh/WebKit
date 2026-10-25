//@ requireOptions("--useTemporal=1")
// Temporal.PlainDate.prototype.since/until for every date largestUnit: month ends, leap days, sign symmetry and the
// representable limits; plus PlainDate.compare/equals with PlainDateTime, ZonedDateTime, strings and property bags.

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

function until(a, b, largestUnit) { return a.until(b, { largestUnit }).toString(); }
noInline(until);
function since(a, b, largestUnit) { return a.since(b, { largestUnit }).toString(); }
noInline(since);

// Columns: from, to, until with largestUnit years/months/weeks/days, then since with the same units.
const cases = [
    ["2021-01-31", "2021-02-28", "P28D", "P28D", "P4W", "P28D", "-P28D", "-P28D", "-P4W", "-P28D"],
    ["2020-01-31", "2020-02-29", "P29D", "P29D", "P4W1D", "P29D", "-P29D", "-P29D", "-P4W1D", "-P29D"],
    ["2020-01-31", "2020-03-01", "P1M1D", "P1M1D", "P4W2D", "P30D", "-P1M1D", "-P1M1D", "-P4W2D", "-P30D"],
    ["2021-03-31", "2021-04-30", "P30D", "P30D", "P4W2D", "P30D", "-P30D", "-P30D", "-P4W2D", "-P30D"],
    ["2021-03-31", "2021-05-01", "P1M1D", "P1M1D", "P4W3D", "P31D", "-P1M1D", "-P1M1D", "-P4W3D", "-P31D"],
    ["2021-05-31", "2021-06-30", "P30D", "P30D", "P4W2D", "P30D", "-P30D", "-P30D", "-P4W2D", "-P30D"],
    ["2021-01-31", "2021-03-31", "P2M", "P2M", "P8W3D", "P59D", "-P2M", "-P2M", "-P8W3D", "-P59D"],
    ["2021-08-31", "2022-02-28", "P5M28D", "P5M28D", "P25W6D", "P181D", "-P5M28D", "-P5M28D", "-P25W6D", "-P181D"],
    ["2019-12-31", "2020-02-29", "P1M29D", "P1M29D", "P8W4D", "P60D", "-P1M29D", "-P1M29D", "-P8W4D", "-P60D"],
    ["2020-02-29", "2021-02-28", "P11M30D", "P11M30D", "P52W1D", "P365D", "-P11M30D", "-P11M30D", "-P52W1D", "-P365D"],
    ["2020-02-29", "2021-03-01", "P1Y1D", "P12M1D", "P52W2D", "P366D", "-P1Y1D", "-P12M1D", "-P52W2D", "-P366D"],
    ["2020-02-29", "2024-02-29", "P4Y", "P48M", "P208W5D", "P1461D", "-P4Y", "-P48M", "-P208W5D", "-P1461D"],
    ["2020-02-29", "2024-02-28", "P3Y11M30D", "P47M30D", "P208W4D", "P1460D", "-P3Y11M30D", "-P47M30D", "-P208W4D", "-P1460D"],
    ["1900-02-28", "2000-02-29", "P100Y1D", "P1200M1D", "P5217W6D", "P36525D", "-P100Y1D", "-P1200M1D", "-P5217W6D", "-P36525D"],
    ["-000001-02-28", "0000-02-29", "P1Y1D", "P12M1D", "P52W2D", "P366D", "-P1Y1D", "-P12M1D", "-P52W2D", "-P366D"],
    ["2000-01-01", "2000-01-01", "PT0S", "PT0S", "PT0S", "PT0S", "PT0S", "PT0S", "PT0S", "PT0S"],
    ["1999-12-31", "2000-01-01", "P1D", "P1D", "P1D", "P1D", "-P1D", "-P1D", "-P1D", "-P1D"],
    ["2021-12-31", "2022-01-01", "P1D", "P1D", "P1D", "P1D", "-P1D", "-P1D", "-P1D", "-P1D"],
    // The representable limits, in both directions.
    ["-271821-04-19", "+275760-09-13", "P547581Y4M25D", "P6570976M25D", "P28571428W5D", "P200000001D", "-P547581Y4M25D", "-P6570976M25D", "-P28571428W5D", "-P200000001D"],
    ["+275760-09-13", "-271821-04-19", "-P547581Y4M24D", "-P6570976M24D", "-P28571428W5D", "-P200000001D", "P547581Y4M24D", "P6570976M24D", "P28571428W5D", "P200000001D"],
    ["-271821-04-19", "-271821-04-20", "P1D", "P1D", "P1D", "P1D", "-P1D", "-P1D", "-P1D", "-P1D"],
    ["+275760-09-12", "+275760-09-13", "P1D", "P1D", "P1D", "P1D", "-P1D", "-P1D", "-P1D", "-P1D"],
];
const units = ["years", "months", "weeks", "days"];
const parsed = cases.map(([from, to, ...expected]) => [Temporal.PlainDate.from(from), Temporal.PlainDate.from(to), from, to, expected]);

for (let i = 0; i < iterations; i++) {
    for (const [a, b, from, to, expected] of parsed) {
        for (let u = 0; u < units.length; u++) {
            shouldBe(until(a, b, units[u]), expected[u], `${from} until ${to} ${units[u]}`);
            shouldBe(since(a, b, units[u]), expected[u + 4], `${from} since ${to} ${units[u]}`);
        }
    }
}

// With no options the default largestUnit is days, and string/bag arguments go through the same path.
shouldBe(Temporal.PlainDate.from("2020-01-31").until("2020-03-01").toString(), "P30D", "default largestUnit");
shouldBe(Temporal.PlainDate.from("2020-01-31").since({ year: 2020, month: 3, day: 1 }).toString(), "-P30D", "bag argument");
shouldBe(Temporal.PlainDate.from("-271821-04-19").until("-271821-04-20").toString(), "P1D", "lower limit default");
shouldBe(Temporal.PlainDate.from("+275760-09-13").since("+275760-09-12").toString(), "P1D", "upper limit default");
shouldBe(Temporal.PlainDate.from("2021-01-31").until("2021-03-30", { largestUnit: "months", smallestUnit: "months", roundingMode: "halfExpand" }).toString(), "P2M", "rounded until");
shouldBe(Temporal.PlainDate.from("2021-01-31").since("2021-03-30", { largestUnit: "years", smallestUnit: "weeks", roundingMode: "ceil" }).toString(), "-P1M4W", "rounded since");
shouldThrow(() => Temporal.PlainDate.from("2021-01-31").until("2021-03-30", { largestUnit: "hours" }), RangeError, "time largestUnit");
shouldThrow(() => Temporal.PlainDate.from("2021-01-31").until(Temporal.PlainDate.from("2021-03-30").withCalendar("japanese")), RangeError, "calendar mismatch");

// compare/equals with other Temporal types and property bags. Columns: argument, compare(d, arg), compare(arg, d), d.equals(arg).
const d = new Temporal.PlainDate(2021, 3, 14);
const comparisons = [
    [new Temporal.PlainDateTime(2021, 3, 14, 23, 59, 59, 999, 999, 999), 0, 0, true],
    [new Temporal.PlainDateTime(2021, 3, 15), -1, 1, false],
    [Temporal.ZonedDateTime.from("2021-03-14T03:30-04:00[America/New_York]"), 0, 0, true],
    [Temporal.ZonedDateTime.from("2021-03-14T23:30+14:00[Pacific/Kiritimati]"), 0, 0, true],
    [new Temporal.ZonedDateTime(1615705200000000000n, "Pacific/Kiritimati"), 0, 0, true],
    [new Temporal.ZonedDateTime(1615705200000000000n, "Pacific/Pago_Pago"), 1, -1, false],
    [{ year: 2021, month: 3, day: 14 }, 0, 0, true],
    [{ year: 2021, monthCode: "M03", day: 13 }, 1, -1, false],
    [{ year: 2021, month: 3, day: 14, calendar: "gregory" }, 0, 0, false],
    [{ year: 2021, month: 2, day: 31 }, 1, -1, false],
    [{ year: 2021, month: 3, day: 32 }, -1, 1, false],
    ["2021-03-14T10:00[Asia/Tokyo]", 0, 0, true],
    ["2021-03-14[u-ca=japanese]", 0, 0, false],
    [new Temporal.PlainDate(2021, 3, 14, "japanese"), 0, 0, false],
];

function compare(a, b) { return Temporal.PlainDate.compare(a, b); }
noInline(compare);
function equals(a, b) { return a.equals(b); }
noInline(equals);

for (let i = 0; i < iterations; i++) {
    for (const [arg, forward, backward, equal] of comparisons) {
        shouldBe(compare(d, arg), forward, `compare(d, ${arg})`);
        shouldBe(compare(arg, d), backward, `compare(${arg}, d)`);
        shouldBe(equals(d, arg), equal, `equals(${arg})`);
    }
}

shouldBe(Temporal.PlainDate.compare(new Temporal.PlainDate(-271821, 4, 19), new Temporal.PlainDate(275760, 9, 13)), -1, "limits compare");
shouldBe(Temporal.PlainDate.compare(new Temporal.ZonedDateTime(-8640000000000000000000n, "-01:00"), new Temporal.PlainDate(-271821, 4, 19)), 0, "lower limit ZonedDateTime");
shouldBe(Temporal.PlainDate.compare(new Temporal.ZonedDateTime(8640000000000000000000n, "+23:59"), "+275760-09-13"), 0, "upper limit ZonedDateTime");
shouldThrow(() => Temporal.PlainDate.compare(d, { year: 2021, month: 3 }), TypeError, "bag without day");
shouldThrow(() => d.equals({ year: 2021, monthCode: "M03", month: 4, day: 1 }), RangeError, "conflicting month and monthCode");
