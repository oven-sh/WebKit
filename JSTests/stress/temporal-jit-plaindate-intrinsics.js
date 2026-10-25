//@ requireOptions("--useTemporal=1")
// Exercises the DFG/FTL intrinsics for Temporal.PlainDate.compare, Temporal.PlainDateTime.compare and
// their prototypes' equals: hot loops, OSR exits on converted or rejected arguments, wrong receivers,
// calendars, subclasses and polymorphic call sites.

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

const d1 = new Temporal.PlainDate(2020, 1, 31);
const d2 = new Temporal.PlainDate(2020, 2, 1);
const d1b = Temporal.PlainDate.from("2020-01-31");
const negative = new Temporal.PlainDate(-271821, 4, 19);
const max = new Temporal.PlainDate(275760, 9, 13);
const japanese = d1.withCalendar("japanese");

function compareDates(x, y) { return Temporal.PlainDate.compare(x, y); }
noInline(compareDates);
function equalsDates(x, y) { return x.equals(y); }
noInline(equalsDates);

for (let i = 0; i < iterations; i++) {
    shouldBe(compareDates(d1, d2), -1, "compare(d1, d2)");
    shouldBe(compareDates(d2, d1), 1, "compare(d2, d1)");
    shouldBe(compareDates(d1, d1b), 0, "compare(d1, d1b)");
    shouldBe(compareDates(negative, max), -1, "compare(negative, max)");
    // compare ignores the calendar, equals does not.
    shouldBe(compareDates(d1, japanese), 0, "compare(d1, japanese)");
    shouldBe(equalsDates(d1, d1b), true, "equals(d1, d1b)");
    shouldBe(equalsDates(d1, d2), false, "equals(d1, d2)");
    shouldBe(equalsDates(d1, japanese), false, "equals(d1, japanese)");
    shouldBe(equalsDates(japanese, japanese), true, "equals(japanese, japanese)");
}

// Arguments that ToTemporalDate converts.
shouldBe(compareDates("2020-01-31", d2), -1, "compare(string, d2)");
shouldBe(compareDates(d1, { year: 2020, month: 1, day: 31 }), 0, "compare(d1, bag)");
shouldBe(compareDates(new Temporal.PlainDateTime(2020, 2, 1, 23), d2), 0, "compare(PlainDateTime, d2)");
shouldBe(equalsDates(d1, "2020-01-31"), true, "equals(d1, string)");
shouldBe(equalsDates(d1, new Temporal.PlainDateTime(2020, 1, 31, 12)), true, "equals(d1, PlainDateTime)");

// Arguments ToTemporalDate rejects, wrong receivers and missing arguments.
shouldThrow(() => compareDates(d1, 42), TypeError, "compare(d1, 42)");
shouldThrow(() => compareDates(undefined, d1), TypeError, "compare(undefined, d1)");
shouldThrow(() => compareDates(d1, "nope"), RangeError, "compare(d1, bad string)");
shouldThrow(() => equalsDates(d1, null), TypeError, "equals(d1, null)");
shouldThrow(() => equalsDates(Object.create(Temporal.PlainDate.prototype), d1), TypeError, "fake.equals(d1)");
shouldThrow(() => Temporal.PlainDate.prototype.equals.call(new Temporal.PlainDateTime(2020, 1, 31), d1), TypeError, "equals.call(PlainDateTime, d1)");
shouldThrow(() => Temporal.PlainDate.compare(d1), TypeError, "compare with one argument");
shouldThrow(() => d1.equals(), TypeError, "equals with no argument");

// Property bags are read in order, and the first argument is converted before the second.
{
    const log = [];
    const bag = name => new Proxy({ year: 2020, month: 1, day: 31 }, {
        get(target, key) { if (typeof key === "string") log.push(name + "." + key); return target[key]; },
    });
    shouldBe(compareDates(bag("one"), bag("two")), 0, "compare(bag, bag)");
    shouldBe(log.indexOf("one.year") < log.indexOf("two.year"), true, "first argument read first");
}

// Subclasses and polymorphic call sites.
class MyDate extends Temporal.PlainDate { }
const sub = new MyDate(2020, 1, 31);
const values = [d1, d2, sub, "2020-02-01", japanese, new Temporal.PlainDateTime(2020, 1, 1)];
const expected = [0, 1, 0, 1, 0, -1];
function polyCompare(x) { return Temporal.PlainDate.compare(x, d1); }
noInline(polyCompare);
for (let i = 0; i < iterations; i++) {
    const j = i % values.length;
    shouldBe(polyCompare(values[j]), expected[j], "polyCompare");
    shouldBe(equalsDates(sub, d1), true, "subclass equals");
}

// PlainDateTime.
const t1 = new Temporal.PlainDateTime(2020, 1, 31, 23, 59, 59, 999, 999, 999);
const t2 = new Temporal.PlainDateTime(2020, 2, 1);
const t1b = Temporal.PlainDateTime.from("2020-01-31T23:59:59.999999999");
const t1n = new Temporal.PlainDateTime(2020, 1, 31, 23, 59, 59, 999, 999, 998);
function compareDateTimes(x, y) { return Temporal.PlainDateTime.compare(x, y); }
noInline(compareDateTimes);
for (let i = 0; i < iterations; i++) {
    shouldBe(compareDateTimes(t1, t2), -1, "compare(t1, t2)");
    shouldBe(compareDateTimes(t2, t1), 1, "compare(t2, t1)");
    shouldBe(compareDateTimes(t1, t1b), 0, "compare(t1, t1b)");
    shouldBe(compareDateTimes(t1n, t1), -1, "compare(t1n, t1)");
    shouldBe(equalsDates(t1, t1b), true, "PlainDateTime equals");
    shouldBe(equalsDates(t1, t1n), false, "PlainDateTime not equals");
    shouldBe(equalsDates(t1, t1.withCalendar("gregory")), false, "PlainDateTime calendar equals");
}
shouldBe(compareDateTimes(d2, t2), 0, "compare(PlainDate, t2)");
shouldThrow(() => compareDateTimes(t1, {}), TypeError, "compare(t1, {})");

// Results feed comparisons in optimized code, and sort uses compare as its comparator.
const dates = [];
for (let i = 0; i < 200; i++)
    dates.push(new Temporal.PlainDate(2000 + (i * 37) % 50, 1 + (i * 7) % 12, 1 + (i * 11) % 28));
const sorted = dates.slice().sort(Temporal.PlainDate.compare);
for (let i = 1; i < sorted.length; i++)
    shouldBe(Temporal.PlainDate.compare(sorted[i - 1], sorted[i]) <= 0, true, "sorted order");

// PlainDateTime.prototype.equals on its own monomorphic site.
function equalsDateTimes(x, y) { return x.equals(y); }
noInline(equalsDateTimes);
for (let i = 0; i < iterations; i++) {
    shouldBe(equalsDateTimes(t1, t1b), true, "equalsDateTimes(t1, t1b)");
    shouldBe(equalsDateTimes(t1, t1n), false, "equalsDateTimes(t1, t1n)");
    shouldBe(equalsDateTimes(t1, t1.withCalendar("gregory")), false, "equalsDateTimes(t1, gregory)");
}
shouldBe(equalsDateTimes(t2, "2020-02-01T00:00"), true, "equalsDateTimes(t2, string)");
shouldBe(equalsDateTimes(t2, d2), true, "equalsDateTimes(t2, PlainDate)");
shouldThrow(() => equalsDateTimes(t2, 5), TypeError, "equalsDateTimes(t2, 5)");
shouldThrow(() => Temporal.PlainDateTime.prototype.equals.call(d2, t2), TypeError, "PlainDateTime equals on a PlainDate");

// PlainDateTime.compare ignores the calendar.
function compareDateTimesAcrossCalendars(x) { return Temporal.PlainDateTime.compare(x, t1.withCalendar("hebrew")); }
noInline(compareDateTimesAcrossCalendars);
for (let i = 0; i < iterations; i++) {
    shouldBe(compareDateTimesAcrossCalendars(t1), 0, "compare(t1, hebrew t1)");
    shouldBe(compareDateTimesAcrossCalendars(t2.withCalendar("japanese")), 1, "compare(japanese t2, hebrew t1)");
}

// A loop-invariant operand that is only a PlainDate on the path that uses it: the out-of-line
// compare/equals calls must not be hoisted above the check that proves its class.
{
    function guarded(x, n) {
        let s = 0;
        for (let i = 0; i < n; i++) {
            if (x instanceof Temporal.PlainDate)
                s += Temporal.PlainDate.compare(x, d2) + (x.equals(x) ? 10 : 0);
        }
        return s;
    }
    noInline(guarded);
    for (let i = 0; i < 2000; i++) {
        shouldBe(guarded(d1, 10), 10 * (-1 + 10), "guarded(d1)");
        shouldBe(guarded(42, 10), 0, "guarded(42)");
        shouldBe(guarded("2020-01-31", 10), 0, "guarded(string)");
        shouldBe(guarded({ year: 2020, month: 1, day: 31 }, 10), 0, "guarded(bag)");
    }
}

// PlainDate and PlainDateTime from another realm.
{
    const other = createGlobalObject();
    const foreignDate = new other.Temporal.PlainDate(2020, 1, 31);
    const foreignDateTime = new other.Temporal.PlainDateTime(2020, 1, 31, 23, 59, 59, 999, 999, 999);
    function crossRealm(x, y) {
        return Temporal.PlainDate.compare(x, d2) + other.Temporal.PlainDate.compare(d2, x) * 10 + (x.equals(d1) ? 100 : 0)
            + Temporal.PlainDateTime.compare(y, t1) * 1000 + (y.equals(t1) ? 10000 : 0);
    }
    noInline(crossRealm);
    for (let i = 0; i < iterations; i++)
        shouldBe(crossRealm(foreignDate, foreignDateTime), -1 + 10 + 100 + 0 + 10000, "crossRealm");
}
