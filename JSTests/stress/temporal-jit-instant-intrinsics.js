//@ requireOptions("--useTemporal=1")
// Exercises the DFG/FTL intrinsics for Temporal.Instant.compare, Temporal.Instant.prototype.equals,
// and the epochMilliseconds getters of Instant and ZonedDateTime: hot loops, OSR exits on
// non-Instant arguments, wrong receivers and polymorphic call sites.

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

const a = Temporal.Instant.fromEpochMilliseconds(1000);
const b = Temporal.Instant.fromEpochMilliseconds(2000);
const a2 = new Temporal.Instant(1000000000n);
// Same millisecond, different nanoseconds: compare must see the nanoseconds.
const n1 = new Temporal.Instant(1000000001n);
const negative = new Temporal.Instant(-1n);
const max = new Temporal.Instant(8640000000000000000000n);
const min = new Temporal.Instant(-8640000000000000000000n);

function compare(x, y) { return Temporal.Instant.compare(x, y); }
noInline(compare);
function equals(x, y) { return x.equals(y); }
noInline(equals);
function epochMs(x) { return x.epochMilliseconds; }
noInline(epochMs);

for (let i = 0; i < iterations; i++) {
    shouldBe(compare(a, b), -1, "compare(a, b)");
    shouldBe(compare(b, a), 1, "compare(b, a)");
    shouldBe(compare(a, a2), 0, "compare(a, a2)");
    shouldBe(compare(a2, n1), -1, "compare(a2, n1)");
    shouldBe(compare(min, max), -1, "compare(min, max)");
    shouldBe(compare(max, max), 0, "compare(max, max)");
    shouldBe(compare(negative, a), -1, "compare(negative, a)");
    shouldBe(equals(a, a2), true, "equals(a, a2)");
    shouldBe(equals(a, b), false, "equals(a, b)");
    shouldBe(equals(a2, n1), false, "equals(a2, n1)");
    shouldBe(epochMs(a), 1000, "epochMs(a)");
    shouldBe(epochMs(n1), 1000, "epochMs(n1)");
    // floor, not truncation, for negative values.
    shouldBe(epochMs(negative), -1, "epochMs(negative)");
    shouldBe(epochMs(max), 8640000000000000, "epochMs(max)");
    shouldBe(epochMs(min), -8640000000000000, "epochMs(min)");
}

// The Int128 crosses 64-bit word boundaries: low words compare unsigned, high words signed, and the
// millisecond floor takes a slow path outside int64 nanoseconds.
{
    const boundaries = [
        -8640000000000000000000n, -18446744073709551617n, -18446744073709551616n, -9223372036854775809n,
        -9223372036854775808n, -9223372036854775807n, -1000001n, -1000000n, -999999n, -1n, 0n, 1n, 999999n,
        1000000n, 9223372036854775806n, 9223372036854775807n, 9223372036854775808n, 9223372036854775809n,
        18446744073709551615n, 18446744073709551616n, 8640000000000000000000n,
    ];
    const instants = boundaries.map(ns => new Temporal.Instant(ns));
    const floorDiv = ns => Number(ns >= 0n ? ns / 1000000n : -((-ns + 999999n) / 1000000n));
    for (let i = 0; i < 2000; i++) {
        for (let j = 0; j < instants.length; j++) {
            shouldBe(epochMs(instants[j]), floorDiv(boundaries[j]), "epochMs(boundary " + boundaries[j] + ")");
            const k = (j + i) % instants.length;
            const expected = boundaries[j] < boundaries[k] ? -1 : boundaries[j] > boundaries[k] ? 1 : 0;
            shouldBe(compare(instants[j], instants[k]), expected, "compare(boundary " + boundaries[j] + ", " + boundaries[k] + ")");
            shouldBe(equals(instants[j], instants[k]), expected === 0, "equals(boundary " + boundaries[j] + ", " + boundaries[k] + ")");
        }
    }
}

// OSR exit: arguments that ToTemporalInstant converts.
shouldBe(compare("1970-01-01T00:00:01Z", b), -1, "compare(string, b)");
shouldBe(compare(a, "1970-01-01T00:00:01Z"), 0, "compare(a, string)");
const zdt = a.toZonedDateTimeISO("UTC");
shouldBe(compare(zdt, b), -1, "compare(zdt, b)");
shouldBe(equals(a, zdt), true, "equals(a, zdt)");
shouldBe(equals(a, "1970-01-01T00:00:01Z"), true, "equals(a, string)");

// OSR exit: arguments ToTemporalInstant rejects.
shouldThrow(() => compare({}, b), RangeError, "compare({}, b)");
shouldThrow(() => compare(a, 42), TypeError, "compare(a, 42)");
shouldThrow(() => compare(a, undefined), TypeError, "compare(a, undefined)");
shouldThrow(() => compare(a, Temporal.PlainDate.from("2020-01-01")), RangeError, "compare(a, PlainDate)");
shouldThrow(() => equals(a, null), TypeError, "equals(a, null)");
shouldThrow(() => equals(a, Object.create(Temporal.Instant.prototype)), TypeError, "equals(a, fake)");
shouldThrow(() => Temporal.Instant.compare(a), TypeError, "compare with one argument");
shouldThrow(() => a.equals(), TypeError, "equals with no argument");

// Wrong receivers for equals and epochMilliseconds.
const fake = Object.create(Temporal.Instant.prototype);
shouldThrow(() => equals(fake, a), TypeError, "fake.equals(a)");
shouldThrow(() => epochMs(fake), TypeError, "fake.epochMilliseconds");
shouldThrow(() => Temporal.Instant.prototype.equals.call(zdt, a), TypeError, "equals.call(zdt, a)");
shouldBe(epochMs(zdt), 1000, "epochMs(zdt)");
shouldBe(epochMs({ epochMilliseconds: 7 }), 7, "epochMs(plain object)");

// Order of evaluation: argument conversion happens on the first argument first.
{
    const log = [];
    const one = { toString() { log.push("one"); return "1970-01-01T00:00:00Z"; } };
    const two = { toString() { log.push("two"); return "1970-01-01T00:00:01Z"; } };
    shouldBe(compare(one, two), -1, "compare(object, object)");
    shouldBe(log.join(), "one,two", "first argument converted first");
}

// Polymorphic call sites: Instants, ZonedDateTimes and subclasses through the same code.
class MyInstant extends Temporal.Instant { }
const sub = new MyInstant(5000000000n);
const values = [a, b, zdt, sub, n1, b.toZonedDateTimeISO("Asia/Tokyo")];
const expectedMs = [1000, 2000, 1000, 5000, 1000, 2000];
function polyMs(x) { return x.epochMilliseconds; }
noInline(polyMs);
function polyCompare(x, y) { return Temporal.Instant.compare(x, y); }
noInline(polyCompare);
for (let i = 0; i < iterations; i++) {
    const j = i % values.length;
    shouldBe(polyMs(values[j]), expectedMs[j], "polyMs");
    shouldBe(polyCompare(values[j], b), Math.sign(expectedMs[j] - 2000), "polyCompare");
    shouldBe(equals(sub, values[j]), expectedMs[j] === 5000, "subclass equals");
}

// A getter shadowed on the instance or redefined on a subclass must be honored.
class Shadow extends Temporal.Instant {
    get epochMilliseconds() { return -42; }
}
const shadow = new Shadow(0n);
function shadowMs(x) { return x.epochMilliseconds; }
noInline(shadowMs);
for (let i = 0; i < iterations; i++) {
    shouldBe(shadowMs(a), 1000, "shadowMs(a)");
    if (i > iterations / 2)
        shouldBe(shadowMs(shadow), -42, "shadowMs(shadow)");
}

// Replacing Temporal.Instant.compare after it got hot must call the replacement.
function callCompare(x, y) { return Temporal.Instant.compare(x, y); }
noInline(callCompare);
for (let i = 0; i < iterations; i++)
    shouldBe(callCompare(a, b), -1, "callCompare before replacement");
const original = Temporal.Instant.compare;
Temporal.Instant.compare = () => 99;
shouldBe(callCompare(a, b), 99, "callCompare after replacement");
Temporal.Instant.compare = original;

// The getter's prototype entry can be replaced.
function protoMs(x) { return x.epochMilliseconds; }
noInline(protoMs);
for (let i = 0; i < iterations; i++)
    shouldBe(protoMs(b), 2000, "protoMs before redefinition");
const descriptor = Object.getOwnPropertyDescriptor(Temporal.Instant.prototype, "epochMilliseconds");
Object.defineProperty(Temporal.Instant.prototype, "epochMilliseconds", { get() { return "redefined"; }, configurable: true });
shouldBe(protoMs(b), "redefined", "protoMs after redefinition");
Object.defineProperty(Temporal.Instant.prototype, "epochMilliseconds", descriptor);
shouldBe(protoMs(b), 2000, "protoMs after restore");

// Results feed arithmetic and comparisons in optimized code.
function maxOf(list) {
    let m = list[0];
    for (let i = 1; i < list.length; i++) {
        if (Temporal.Instant.compare(list[i], m) > 0)
            m = list[i];
    }
    return m.epochMilliseconds;
}
noInline(maxOf);
const list = [];
for (let i = 0; i < 100; i++)
    list.push(Temporal.Instant.fromEpochMilliseconds(((i * 37) % 100) * 1000 - 50000));
for (let i = 0; i < 2000; i++)
    shouldBe(maxOf(list), 49000, "maxOf");
list.push("1970-01-01T00:00:00Z");
shouldBe(maxOf(list), 49000, "maxOf with string");
const sorted = list.slice(0, 100).sort(Temporal.Instant.compare);
for (let i = 1; i < sorted.length; i++)
    shouldBe(Temporal.Instant.compare(sorted[i - 1], sorted[i]), -1, "sorted order");

// The same value in both operands.
function selfCompare(x) { return Temporal.Instant.compare(x, x) + (x.equals(x) ? 10 : 20); }
noInline(selfCompare);
for (let i = 0; i < iterations; i++)
    shouldBe(selfCompare(i & 1 ? a : max), 10, "selfCompare");

// An own property defined on one instance after the site got hot.
{
    const instance = Temporal.Instant.fromEpochMilliseconds(123);
    function ownMs(x) { return x.epochMilliseconds; }
    noInline(ownMs);
    for (let i = 0; i < iterations; i++)
        shouldBe(ownMs(instance), 123, "ownMs before shadowing");
    Object.defineProperty(instance, "epochMilliseconds", { value: "own" });
    shouldBe(ownMs(instance), "own", "ownMs after shadowing");
    shouldBe(ownMs(a), 1000, "ownMs on another instance");
}

// Objects and functions from another realm.
{
    const other = createGlobalObject();
    const otherA = other.Temporal.Instant.fromEpochMilliseconds(1000);
    const otherB = new other.Temporal.Instant(2000000000n);
    function crossRealm(x, y) {
        return Temporal.Instant.compare(x, y) * 100 + other.Temporal.Instant.compare(y, x) * 10 + (x.equals(y) ? 1 : 0) + x.epochMilliseconds;
    }
    noInline(crossRealm);
    for (let i = 0; i < iterations; i++) {
        shouldBe(crossRealm(otherA, b), -100 + 10 + 1000, "crossRealm(otherA, b)");
        shouldBe(crossRealm(a, otherB), -100 + 10 + 1000, "crossRealm(a, otherB)");
        shouldBe(crossRealm(otherA, a), 1 + 1000, "crossRealm(otherA, a)");
    }
}

// The getter's result used where an int32 is expected.
{
    const small = [Temporal.Instant.fromEpochMilliseconds(0), Temporal.Instant.fromEpochMilliseconds(1), Temporal.Instant.fromEpochMilliseconds(2)];
    const table = [10, 20, 30];
    function indexed(x) { return table[x.epochMilliseconds] + (x.epochMilliseconds | 0); }
    noInline(indexed);
    for (let i = 0; i < iterations; i++)
        shouldBe(indexed(small[i % 3]), 10 * (i % 3 + 1) + i % 3, "indexed");
    shouldBe(Number.isNaN(indexed(b)), true, "indexed out of range");
}

// ZonedDateTime.prototype.epochMilliseconds on a monomorphic site, across the int64 boundary.
{
    const nanoseconds = [-8640000000000000000000n, -9223372036854775809n, -9223372036854775808n, -1000001n, -1n, 0n, 999999n, 9223372036854775807n, 9223372036854775808n, 8640000000000000000000n];
    const zoned = nanoseconds.map((ns, i) => new Temporal.ZonedDateTime(ns, i & 1 ? "UTC" : "Asia/Kathmandu"));
    const floorDiv = ns => Number(ns >= 0n ? ns / 1000000n : -((-ns + 999999n) / 1000000n));
    function zonedMs(z) { return z.epochMilliseconds; }
    noInline(zonedMs);
    for (let i = 0; i < iterations; i++) {
        const j = i % zoned.length;
        shouldBe(zonedMs(zoned[j]), floorDiv(nanoseconds[j]), "zonedMs(" + nanoseconds[j] + ")");
    }
    shouldBe(zonedMs(a), 1000, "zonedMs(Instant)");
}

// A loop-invariant operand that is only an Instant on the path that uses it: nothing may be hoisted
// above the check that proves its class.
{
    function guarded(x, n) {
        let s = 0;
        for (let i = 0; i < n; i++) {
            if (typeof x === "object" && x instanceof Temporal.Instant)
                s += x.epochMilliseconds + Temporal.Instant.compare(x, b) + (x.equals(x) ? 1 : 0);
        }
        return s;
    }
    noInline(guarded);
    for (let i = 0; i < 2000; i++) {
        shouldBe(guarded(a, 10), 10 * (1000 - 1 + 1), "guarded(a)");
        shouldBe(guarded(42, 10), 0, "guarded(42)");
        shouldBe(guarded("1970-01-01T00:00:01Z", 10), 0, "guarded(string)");
        shouldBe(guarded({ epochMilliseconds: 5 }, 10), 0, "guarded(plain object)");
    }
}

// Calls that reach the intrinsics through call, apply, Reflect.apply and bound functions.
{
    const boundCompare = Temporal.Instant.compare.bind(null, a);
    function indirect(x) {
        return Temporal.Instant.compare.call(null, x, b) + Temporal.Instant.compare.apply(null, [b, x]) * 10
            + Reflect.apply(Temporal.Instant.prototype.equals, x, [a]) * 100 + boundCompare(x) * 1000;
    }
    noInline(indirect);
    for (let i = 0; i < iterations; i++) {
        shouldBe(indirect(a), -1 + 10 + 100 + 0, "indirect(a)");
        shouldBe(indirect(b), 0 + 0 + 0 - 1000, "indirect(b)");
    }
    shouldThrow(() => indirect({}), RangeError, "indirect({})");
}

// A getter site that only ever sees Instants from another realm.
{
    const other = createGlobalObject();
    const foreign = [other.Temporal.Instant.fromEpochMilliseconds(-1), new other.Temporal.Instant(-1n), new other.Temporal.Instant(9223372036854775809n)];
    function foreignMs(x) { return x.epochMilliseconds; }
    noInline(foreignMs);
    for (let i = 0; i < iterations; i++) {
        shouldBe(foreignMs(foreign[0]), -1, "foreignMs(-1ms)");
        shouldBe(foreignMs(foreign[1]), -1, "foreignMs(-1ns)");
        shouldBe(foreignMs(foreign[2]), 9223372036854, "foreignMs(beyond int64)");
    }
    shouldBe(foreignMs(a), 1000, "foreignMs(this realm)");
}
