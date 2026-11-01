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

// Ascending, across the 64-bit word boundaries of the Int128 and the rounding edges of the millisecond division.
const nanoseconds = [-8640000000000000000000n, -18446744073709551616n, -9223372036854775809n, -9223372036854775808n, -9223372036854000000n, -1000001n, -1000000n, -1n, 0n, 9223372036853999999n, 9223372036854775807n, 9223372036854775808n, 18446744073709551616n, 8640000000000000000000n];
const milliseconds = [-8640000000000000, -18446744073710, -9223372036855, -9223372036855, -9223372036854, -2, -1, -1, 0, 9223372036853, 9223372036854, 9223372036854, 18446744073709, 8640000000000000];
const instants = nanoseconds.map(ns => new Temporal.Instant(ns));
const zoned = nanoseconds.map(ns => new Temporal.ZonedDateTime(ns, "UTC"));

function compareInstant(x, y) { return Temporal.Instant.compare(x, y); }
function equalsInstant(x, y) { return x.equals(y); }
function compareZoned(x, y) { return Temporal.ZonedDateTime.compare(x, y); }
function equalsZoned(x, y) { return x.equals(y); }
function compareDate(x, y) { return Temporal.PlainDate.compare(x, y); }
function equalsDate(x, y) { return x.equals(y); }
function compareDateTime(x, y) { return Temporal.PlainDateTime.compare(x, y); }
function equalsDateTime(x, y) { return x.equals(y); }
function compareTime(x, y) { return Temporal.PlainTime.compare(x, y); }
function equalsTime(x, y) { return x.equals(y); }

// Each row: [compare, equals, ascending values, a value equal to the first in another calendar or time zone]
const types = [
    [compareInstant, equalsInstant, instants, null],
    [compareZoned, equalsZoned, zoned, new Temporal.ZonedDateTime(nanoseconds[0], "Asia/Kolkata")],
    [compareDate, equalsDate, [new Temporal.PlainDate(-271821, 4, 19), new Temporal.PlainDate(2020, 1, 31), new Temporal.PlainDate(2020, 2, 1), new Temporal.PlainDate(275760, 9, 13)], new Temporal.PlainDate(-271821, 4, 19, "gregory")],
    [compareDateTime, equalsDateTime, [new Temporal.PlainDateTime(2020, 1, 31, 23, 59, 59, 999, 999, 998), new Temporal.PlainDateTime(2020, 1, 31, 23, 59, 59, 999, 999, 999), new Temporal.PlainDateTime(2020, 2, 1)], new Temporal.PlainDateTime(2020, 1, 31, 23, 59, 59, 999, 999, 998, "japanese")],
    [compareTime, equalsTime, [new Temporal.PlainTime(), new Temporal.PlainTime(12, 30, 15, 1, 2, 3), new Temporal.PlainTime(12, 30, 15, 1, 2, 4), new Temporal.PlainTime(23, 59, 59, 999, 999, 999)], null],
];

for (const [compare, equals, values, other] of types) {
    noInline(compare);
    noInline(equals);
    const copies = values.map(value => value.constructor.from(value));
    for (let i = 0; i < testLoopCount; i++) {
        const j = i % (values.length - 1);
        shouldBe(compare(values[j], values[j + 1]), -1, `${compare.name} ${j}`);
        shouldBe(compare(values[j + 1], values[j]), 1, `${compare.name} ${j} reversed`);
        shouldBe(compare(values[j], copies[j]), 0, `${compare.name} ${j} copy`);
        shouldBe(equals(values[j], values[j + 1]), false, `${equals.name} ${j}`);
        shouldBe(equals(values[j], copies[j]), true, `${equals.name} ${j} copy`);
        // -2^64 and 2^64 differ only in the high word.
        shouldBe(equals(values[j], values[values.length - 1 - j]), j === values.length - 1 - j, `${equals.name} ${j} mirrored`);
        if (other) {
            shouldBe(compare(values[0], other), 0, `${compare.name} in another calendar or time zone`);
            shouldBe(equals(values[0], other), false, `${equals.name} in another calendar or time zone`);
        }
    }
    shouldBe(compare(values[0], values[0].toString()), 0, `${compare.name} with a string`);
    shouldBe(equals(values[0], values[0].toString()), true, `${equals.name} with a string`);
    shouldThrow(() => compare(values[0], 42), TypeError, `${compare.name} with a number`);
    shouldThrow(() => equals(Object.create(values[0].constructor.prototype), values[0]), TypeError, `${equals.name} on a wrong receiver`);
}

function instantMilliseconds(x) { return x.epochMilliseconds; }
noInline(instantMilliseconds);
function zonedMilliseconds(x) { return x.epochMilliseconds; }
noInline(zonedMilliseconds);
for (let i = 0; i < testLoopCount; i++) {
    const j = i % nanoseconds.length;
    shouldBe(instantMilliseconds(instants[j]), milliseconds[j], `Instant epochMilliseconds ${j}`);
    shouldBe(zonedMilliseconds(zoned[j]), milliseconds[j], `ZonedDateTime epochMilliseconds ${j}`);
}
{
    const instant = new Temporal.Instant(0n);
    Object.defineProperty(instant, "epochMilliseconds", { value: 7 });
    shouldBe(instantMilliseconds(instant), 7, "epochMilliseconds own property");
}
shouldThrow(() => zonedMilliseconds(Object.create(Temporal.ZonedDateTime.prototype)), TypeError, "epochMilliseconds on a wrong receiver");

// Values beyond int32 and int52 are doubles.
const names = ["years", "months", "weeks", "days", "hours", "minutes", "seconds", "milliseconds"];
const durations = [
    new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10),
    new Temporal.Duration(-1, -2, -3, -4, -5, -6, -7, -8, -9, -10),
    new Temporal.Duration(0, 0, 0, 2 ** 31),
    new Temporal.Duration(0, 0, 0, 0, 0, 0, Number.MAX_SAFE_INTEGER),
    new Temporal.Duration(0, 0, 0, 0, 0, 0, 0, -(2 ** 62)),
];
for (const name of names) {
    const get = new Function("d", `return d.${name};`);
    noInline(get);
    const getter = Object.getOwnPropertyDescriptor(Temporal.Duration.prototype, name).get;
    const expected = durations.map(d => getter.call(d));
    for (let i = 0; i < testLoopCount; i++) {
        const j = i % durations.length;
        shouldBe(get(durations[j]), expected[j], `Duration ${name} ${j}`);
    }
    shouldBe(get({ [name]: "own" }), "own", `${name} on a plain object`);
    shouldThrow(() => get(Object.create(Temporal.Duration.prototype)), TypeError, `${name} on a wrong receiver`);
}

// The operand is only a Temporal object on the path that uses it, so nothing may be hoisted above the check.
function guarded(x, n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        if (x instanceof Temporal.Instant)
            s += x.epochMilliseconds + Temporal.Instant.compare(x, x) + (x.equals(x) ? 1 : 0);
        if (x instanceof Temporal.Duration)
            s += x.hours;
    }
    return s;
}
noInline(guarded);
const epoch = new Temporal.Instant(0n);
for (let i = 0; i < testLoopCount; i++) {
    shouldBe(guarded(epoch, 4), 4, "guarded Instant");
    shouldBe(guarded(durations[0], 4), 20, "guarded Duration");
    shouldBe(guarded(i, 4), 0, "guarded number");
}

// Replacing a getter on the prototype invalidates code that inlined it.
Object.defineProperty(Temporal.Duration.prototype, "hours", { get() { return 1; } });
shouldBe(guarded(durations[0], 4), 4, "guarded Duration with hours redefined");
