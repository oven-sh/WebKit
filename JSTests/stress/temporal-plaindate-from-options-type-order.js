//@ requireOptions("--useTemporal=1")

// ToTemporalDate (https://tc39.es/proposal-temporal/#sec-temporal-totemporaldate) step 2 for a property bag:
//   2.e PrepareCalendarFields, 2.f GetOptionsObject, 2.g GetTemporalOverflowOption, 2.h CalendarDateFromFields.
// A non-object options argument is a TypeError at 2.f. It must win over every RangeError that only
// CalendarDateFromFields can raise (unknown era, date outside the representable range), and it must
// lose to the errors that PrepareCalendarFields raises while reading the bag.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrow(ctor, fn, message) {
    try {
        fn();
    } catch (e) {
        if (!(e instanceof ctor))
            throw new Error(`${message}: expected ${ctor.name}, got ${e}`);
        return;
    }
    throw new Error(`${message}: expected ${ctor.name} but no exception was thrown`);
}

const badOptions = [null, "constrain", 1, true, Symbol("options"), 1n];

// Bags whose only problem surfaces in CalendarDateFromFields.
const lateFailingBags = [
    [{ year: -271821, month: 2, day: 31 }, "ISO date below the minimum after constrain"],
    [{ year: 275760, month: 9, day: 14 }, "ISO date above the maximum"],
    [{ era: "xx", eraYear: 1, month: 1, day: 1, calendar: "gregory" }, "unknown era"],
    [{ era: "reiwa", eraYear: 1, month: 1, day: 1, calendar: "hebrew" }, "era from another calendar"],
    [{ year: 5784, monthCode: "M05L", day: 1, calendar: "gregory" }, "leap month code in a calendar without leap months"],
    [{ year: 2024, month: 13, day: 1 }, "month constrained to 12 (control: no RangeError at all)"],
];

for (const [bag, label] of lateFailingBags) {
    for (const options of badOptions)
        shouldThrow(TypeError, () => Temporal.PlainDate.from(bag, options), `${label}, options ${typeof options}`);
}

// PrepareCalendarFields errors still come first: they happen while reading the bag, before options.
shouldThrow(RangeError, () => Temporal.PlainDate.from({ year: 2024, month: 0, day: 1 }, null), "month 0 is rejected by PrepareCalendarFields");
shouldThrow(RangeError, () => Temporal.PlainDate.from({ year: Infinity, month: 1, day: 1 }, null), "infinite year is rejected by PrepareCalendarFields");
shouldThrow(TypeError, () => Temporal.PlainDate.from({ year: 2024, month: 1 }, null), "missing day is a TypeError either way");
shouldThrow(RangeError, () => Temporal.PlainDate.from({ year: 2024, month: 1, day: 1, calendar: "not-a-calendar" }, null), "calendar is resolved before fields and options");

// The bag is read exactly once, and completely, before the options TypeError.
{
    const log = [];
    const bag = new Proxy({ year: -271821, month: 2, day: 31 }, {
        get(target, key, receiver) {
            log.push(key);
            return Reflect.get(target, key, receiver);
        },
    });
    shouldThrow(TypeError, () => Temporal.PlainDate.from(bag, null), "proxy bag with null options");
    shouldBe(log.join(","), "calendar,day,month,monthCode,year", "fields read before the options TypeError");
}

// With a real options object the late RangeErrors are reported, after overflow is read once.
for (const [bag, label] of lateFailingBags.slice(0, -1)) {
    let reads = 0;
    const options = { get overflow() { reads++; return "constrain"; } };
    shouldThrow(RangeError, () => Temporal.PlainDate.from(bag, options), `${label}, valid options`);
    shouldBe(reads, 1, `${label}, overflow reads`);
}

// And the happy paths are unchanged.
shouldBe(Temporal.PlainDate.from({ year: 2024, month: 13, day: 1 }).toString(), "2024-12-01", "constrain by default");
shouldBe(Temporal.PlainDate.from({ year: 2024, month: 13, day: 1 }, undefined).toString(), "2024-12-01", "undefined options");
shouldBe(Temporal.PlainDate.from({ year: 2024, month: 13, day: 1 }, { overflow: "constrain" }).toString(), "2024-12-01", "explicit constrain");
shouldBe(Temporal.PlainDate.from({ year: 2024, month: 13, day: 1 }, () => {}).toString(), "2024-12-01", "a function is an object");
shouldThrow(RangeError, () => Temporal.PlainDate.from({ year: 2024, month: 13, day: 1 }, { overflow: "reject" }), "explicit reject");
shouldBe(Temporal.PlainDate.from({ era: "ce", eraYear: 2024, month: 2, day: 29, calendar: "gregory" }, { overflow: "reject" }).toString(), "2024-02-29[u-ca=gregory]", "era bag with options");
