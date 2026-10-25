//@ requireOptions("--useTemporal=1")
// Exercises the DFG/FTL inlining of the Temporal.Duration getters years through milliseconds: hot loops,
// negative and zero fields, wrong receivers, subclasses, shadowing getters and polymorphic call sites.

function shouldBe(actual, expected, msg) {
    if (!Object.is(actual, expected))
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
const names = ["years", "months", "weeks", "days", "hours", "minutes", "seconds", "milliseconds", "microseconds", "nanoseconds"];
const positive = new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
const negative = positive.negated();
const zero = new Temporal.Duration();
const large = new Temporal.Duration(0, 0, 0, 0, 0, 0, Number.MAX_SAFE_INTEGER, 0, 0, 0);
const largeMs = Temporal.Duration.from({ milliseconds: 9007199254740991 });

const getters = names.map(name => {
    const f = new Function("d", `return d.${name};`);
    noInline(f);
    return f;
});
function sum(d) { return d.hours + d.minutes + d.seconds; }
noInline(sum);

for (let i = 0; i < iterations; i++) {
    for (let j = 0; j < names.length; j++) {
        shouldBe(getters[j](positive), j + 1, `positive.${names[j]}`);
        shouldBe(getters[j](negative), -(j + 1), `negative.${names[j]}`);
        // Zero fields are +0, never -0, including on a negated zero duration.
        shouldBe(getters[j](zero), 0, `zero.${names[j]}`);
        shouldBe(getters[j](zero.negated()), 0, `zero.negated().${names[j]}`);
    }
    shouldBe(getters[6](large), Number.MAX_SAFE_INTEGER, "large.seconds");
    shouldBe(getters[7](largeMs), 9007199254740991, "largeMs.milliseconds");
    shouldBe(sum(positive), 18, "sum(positive)");
}

// Wrong receivers.
const fake = Object.create(Temporal.Duration.prototype);
for (let j = 0; j < names.length; j++) {
    shouldThrow(() => getters[j](fake), TypeError, `fake.${names[j]}`);
    shouldBe(getters[j]({ [names[j]]: "own" }), "own", `plain object ${names[j]}`);
    shouldThrow(() => Object.getOwnPropertyDescriptor(Temporal.Duration.prototype, names[j]).get.call(Temporal.Instant.fromEpochMilliseconds(0)), TypeError, `${names[j]} on an Instant`);
}

// Subclasses, shadowing and polymorphism.
class MyDuration extends Temporal.Duration { }
class Shadow extends Temporal.Duration {
    get hours() { return "shadowed"; }
}
const values = [positive, new MyDuration(0, 0, 0, 0, 7), new Shadow(0, 0, 0, 0, 7), negative, { hours: 3 }];
const expectedHours = [5, 7, "shadowed", -5, 3];
function polyHours(d) { return d.hours; }
noInline(polyHours);
for (let i = 0; i < iterations; i++) {
    const j = i % values.length;
    shouldBe(polyHours(values[j]), expectedHours[j], "polyHours");
}

// Redefining the getter on the prototype after it got hot.
function protoHours(d) { return d.hours; }
noInline(protoHours);
for (let i = 0; i < iterations; i++)
    shouldBe(protoHours(positive), 5, "protoHours before redefinition");
const descriptor = Object.getOwnPropertyDescriptor(Temporal.Duration.prototype, "hours");
Object.defineProperty(Temporal.Duration.prototype, "hours", { get() { return "redefined"; }, configurable: true });
shouldBe(protoHours(positive), "redefined", "protoHours after redefinition");
Object.defineProperty(Temporal.Duration.prototype, "hours", descriptor);
shouldBe(protoHours(positive), 5, "protoHours after restore");

// The getters' results used where an int32 is expected, then a value that does not fit an int32.
{
    const table = ["a", "b", "c", "d"];
    const durations = [0, 1, 2, 3].map(days => Temporal.Duration.from({ days }));
    function indexed(d) { return table[d.days] + (d.days | 0); }
    noInline(indexed);
    for (let i = 0; i < iterations; i++)
        shouldBe(indexed(durations[i & 3]), table[i & 3] + (i & 3), "indexed");
    shouldBe(indexed(Temporal.Duration.from({ days: 2 ** 31 })), undefined + -2147483648, "indexed beyond int32");
    function sumDays(d) {
        let s = 0;
        for (let i = 0; i < 4; i++)
            s += d.days;
        return s;
    }
    noInline(sumDays);
    for (let i = 0; i < iterations; i++)
        shouldBe(sumDays(durations[i & 3]), 4 * (i & 3), "sumDays");
    shouldBe(sumDays(Temporal.Duration.from({ days: 2 ** 33 })), 4 * 2 ** 33, "sumDays beyond int32");
    shouldBe(sumDays(Temporal.Duration.from({ days: -(2 ** 31) })), -4 * 2 ** 31, "sumDays at int32 min");
}

// The same getter on an instance whose own property shadows it.
{
    const instance = new Temporal.Duration(0, 0, 0, 0, 9);
    function ownHours(d) { return d.hours; }
    noInline(ownHours);
    for (let i = 0; i < iterations; i++)
        shouldBe(ownHours(instance), 9, "ownHours before shadowing");
    Object.defineProperty(instance, "hours", { value: "own" });
    shouldBe(ownHours(instance), "own", "ownHours after shadowing");
}

// Milliseconds beyond 2^53, where the int64 to double conversion rounds.
{
    const huge = [Temporal.Duration.from({ milliseconds: 9007199254740991e3 }), Temporal.Duration.from({ milliseconds: -9007199254740991e3 }), Temporal.Duration.from({ milliseconds: 2 ** 53 + 1 })];
    const expected = huge.map(d => Object.getOwnPropertyDescriptor(Temporal.Duration.prototype, "milliseconds").get.call(d));
    function hugeMs(d) { return d.milliseconds; }
    noInline(hugeMs);
    for (let i = 0; i < iterations; i++) {
        for (let j = 0; j < huge.length; j++)
            shouldBe(hugeMs(huge[j]), expected[j], "hugeMs " + j);
    }
}

// A loop-invariant operand that is only a Duration on the path that uses it.
{
    const d = new Temporal.Duration(0, 0, 0, 0, 3, 4, 5);
    function guarded(x, n) {
        let s = 0;
        for (let i = 0; i < n; i++) {
            if (x instanceof Temporal.Duration)
                s += x.hours + x.minutes + x.seconds;
        }
        return s;
    }
    noInline(guarded);
    for (let i = 0; i < 2000; i++) {
        shouldBe(guarded(d, 10), 120, "guarded(d)");
        shouldBe(guarded(42, 10), 0, "guarded(42)");
        shouldBe(guarded({ hours: 1, minutes: 1, seconds: 1 }, 10), 0, "guarded(bag)");
    }
}
