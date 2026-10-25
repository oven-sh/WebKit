//@ requireOptions("--useTemporal=1")
// Exercises the DFG/FTL intrinsics for Temporal.ZonedDateTime.compare, Temporal.PlainTime.compare and their
// prototypes' equals: hot loops, the Int128 limits, time zone aliases, calendars, converted or rejected
// arguments (OSR exits), wrong receivers, subclasses and polymorphic call sites.

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

const z1 = new Temporal.ZonedDateTime(-1n, "UTC");
const z2 = new Temporal.ZonedDateTime(0n, "UTC");
const z2NewYork = new Temporal.ZonedDateTime(0n, "America/New_York");
const z2Japanese = new Temporal.ZonedDateTime(0n, "UTC", "japanese");
const zMin = new Temporal.ZonedDateTime(-8640000000000000000000n, "UTC");
const zMax = new Temporal.ZonedDateTime(8640000000000000000000n, "UTC");
// Only the low word differs, and its top bit is set in one of them.
const zLowA = new Temporal.ZonedDateTime(0x7fffffffffffffffn, "UTC");
const zLowB = new Temporal.ZonedDateTime(0x8000000000000000n, "UTC");
const kolkata = new Temporal.ZonedDateTime(10n, "Asia/Kolkata");
const calcutta = new Temporal.ZonedDateTime(10n, "Asia/Calcutta");
const offset = new Temporal.ZonedDateTime(10n, "+05:30");
const etcUTC = new Temporal.ZonedDateTime(0n, "Etc/UTC");

function compareZoned(x, y) { return Temporal.ZonedDateTime.compare(x, y); }
noInline(compareZoned);
function equalsZoned(x, y) { return x.equals(y); }
noInline(equalsZoned);

for (let i = 0; i < iterations; i++) {
    shouldBe(compareZoned(z1, z2), -1, "compare(z1, z2)");
    shouldBe(compareZoned(z2, z1), 1, "compare(z2, z1)");
    shouldBe(compareZoned(z2, z2NewYork), 0, "compare ignores the time zone");
    shouldBe(compareZoned(z2, z2Japanese), 0, "compare ignores the calendar");
    shouldBe(compareZoned(zMin, zMax), -1, "compare(min, max)");
    shouldBe(compareZoned(zMax, zMin), 1, "compare(max, min)");
    shouldBe(compareZoned(zLowA, zLowB), -1, "compare(low words)");
    shouldBe(compareZoned(zLowB, zLowA), 1, "compare(low words) reversed");
    shouldBe(equalsZoned(z2, new Temporal.ZonedDateTime(0n, "UTC")), true, "equals(same)");
    shouldBe(equalsZoned(z1, z2), false, "equals(z1, z2)");
    shouldBe(equalsZoned(z2, z2NewYork), false, "equals with another time zone");
    shouldBe(equalsZoned(z2, z2Japanese), false, "equals with another calendar");
    shouldBe(equalsZoned(kolkata, calcutta), true, "equals with a time zone alias");
    shouldBe(equalsZoned(kolkata, offset), false, "equals with an offset time zone");
    shouldBe(equalsZoned(z2, etcUTC), true, "UTC and Etc/UTC");
    shouldBe(equalsZoned(zMax, zMax), true, "equals(max, max)");
}

// Arguments that ToTemporalZonedDateTime converts, or rejects.
shouldBe(compareZoned("1970-01-01T00:00:00+00:00[UTC]", z1), 1, "compare(string, z1)");
shouldBe(equalsZoned(z2, "1970-01-01T00:00:00+00:00[UTC]"), true, "equals(z2, string)");
shouldBe(equalsZoned(z2, { year: 1970, month: 1, day: 1, timeZone: "UTC" }), true, "equals(z2, bag)");
shouldThrow(() => compareZoned(z1, Temporal.Instant.fromEpochNanoseconds(0n)), TypeError, "compare(z1, Instant)");
shouldThrow(() => compareZoned(z1, 42), TypeError, "compare(z1, 42)");
shouldThrow(() => compareZoned(undefined, z1), TypeError, "compare(undefined, z1)");
shouldBe(equalsZoned(z2.toInstant(), z2), true, "Instant receiver with a ZonedDateTime argument");
shouldThrow(() => Temporal.ZonedDateTime.prototype.equals.call(z2.toInstant(), z2), TypeError, "equals on a wrong receiver");
shouldThrow(() => Temporal.ZonedDateTime.compare(z1), TypeError, "compare with one argument");

const t1 = new Temporal.PlainTime(12, 30, 15, 1, 2, 3);
const t1b = Temporal.PlainTime.from("12:30:15.001002003");
const t2 = new Temporal.PlainTime(12, 30, 15, 1, 2, 4);
const midnight = new Temporal.PlainTime();
const last = new Temporal.PlainTime(23, 59, 59, 999, 999, 999);

function compareTimes(x, y) { return Temporal.PlainTime.compare(x, y); }
noInline(compareTimes);
function equalsTimes(x, y) { return x.equals(y); }
noInline(equalsTimes);

for (let i = 0; i < iterations; i++) {
    shouldBe(compareTimes(t1, t2), -1, "compare(t1, t2)");
    shouldBe(compareTimes(t2, t1), 1, "compare(t2, t1)");
    shouldBe(compareTimes(t1, t1b), 0, "compare(t1, t1b)");
    shouldBe(compareTimes(midnight, last), -1, "compare(midnight, last)");
    shouldBe(equalsTimes(t1, t1b), true, "equals(t1, t1b)");
    shouldBe(equalsTimes(t1, t2), false, "equals(t1, t2)");
    shouldBe(equalsTimes(last, last), true, "equals(last, last)");
}

shouldBe(compareTimes("12:30:15.001002004", t1), 1, "compare(string, t1)");
shouldBe(compareTimes(t1, new Temporal.PlainDateTime(2020, 1, 1, 12, 30, 15, 1, 2, 3)), 0, "compare(t1, PlainDateTime)");
shouldBe(compareTimes(t1, new Temporal.ZonedDateTime(45015001002003n, "UTC")), 0, "compare(t1, ZonedDateTime)");
shouldBe(equalsTimes(t1, { hour: 12, minute: 30, second: 15, millisecond: 1, microsecond: 2, nanosecond: 3 }), true, "equals(t1, bag)");
shouldThrow(() => compareTimes(t1, 42), TypeError, "compare(t1, 42)");
shouldThrow(() => compareTimes(t1, "not a time"), RangeError, "compare(t1, bad string)");
shouldThrow(() => Temporal.PlainTime.prototype.equals.call(z2, t1), TypeError, "equals on a wrong receiver");

// Subclasses have the internal slots.
class MyZoned extends Temporal.ZonedDateTime { }
class MyTime extends Temporal.PlainTime { }
for (let i = 0; i < iterations; i++) {
    shouldBe(compareZoned(new MyZoned(5n, "UTC"), z2), 1, "compare(subclass, z2)");
    shouldBe(equalsZoned(new MyZoned(0n, "UTC"), z2), true, "equals(subclass, z2)");
    shouldBe(compareTimes(new MyTime(1), midnight), 1, "compare(subclass, midnight)");
    shouldBe(equalsTimes(new MyTime(), midnight), true, "equals(subclass, midnight)");
}

// A polymorphic site: one function called with several compare functions and argument types.
function comparePolymorphic(compare, x, y) { return compare(x, y); }
noInline(comparePolymorphic);
for (let i = 0; i < iterations; i++) {
    shouldBe(comparePolymorphic(Temporal.ZonedDateTime.compare, z1, z2), -1, "polymorphic ZonedDateTime");
    shouldBe(comparePolymorphic(Temporal.PlainTime.compare, t2, t1), 1, "polymorphic PlainTime");
    shouldBe(comparePolymorphic(Temporal.Instant.compare, z2, z1), 1, "polymorphic Instant with ZonedDateTimes");
}

// A hot loop whose argument type changes after tier-up.
function compareMaybe(x, y) { return Temporal.ZonedDateTime.compare(x, y); }
noInline(compareMaybe);
for (let i = 0; i < iterations; i++)
    shouldBe(compareMaybe(z1, z2), -1, "warm-up");
for (let i = 0; i < 100; i++)
    shouldBe(compareMaybe("1970-01-01T00:00:00.000000001+00:00[UTC]", z2), 1, "after exit");
