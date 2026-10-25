//@ requireOptions("--useTemporal=1")

// ToTemporalDateTime on a PlainDate creates the date-time at midnight with ? CreateTemporalDateTime (step 2.c.iii),
// so a PlainDate whose midnight is outside ISODateTimeWithinLimits must throw RangeError. -271821-04-19 is a valid
// PlainDate (PlainDate limits are checked at noon) but its midnight is one day before the earliest instant.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`Expected ${expected} but got ${actual}`);
}

function shouldThrow(func, errorType) {
    let threw = false;
    try { func(); } catch (e) {
        threw = true;
        if (!(e instanceof errorType))
            throw new Error(`Expected ${errorType.name} but got ${e.constructor.name}: ${e.message}`);
    }
    if (!threw)
        throw new Error(`Expected ${errorType.name} but no exception was thrown`);
}

const earliestDate = Temporal.PlainDate.from("-271821-04-19");
const nextDate = Temporal.PlainDate.from("-271821-04-20");
const dateTime = new Temporal.PlainDateTime(2000, 1, 1);

shouldThrow(() => Temporal.PlainDateTime.from(earliestDate), RangeError);
// The overflow option is read before the date-time is created.
{
    let overflowRead = false;
    shouldThrow(() => Temporal.PlainDateTime.from(earliestDate, { get overflow() { overflowRead = true; return "constrain"; } }), RangeError);
    shouldBe(overflowRead, true);
}
shouldThrow(() => Temporal.PlainDateTime.compare(earliestDate, dateTime), RangeError);
shouldThrow(() => Temporal.PlainDateTime.compare(dateTime, earliestDate), RangeError);
shouldThrow(() => dateTime.equals(earliestDate), RangeError);

// until/since convert `other` (step 1) before reading options (step 3).
for (const method of ["until", "since"]) {
    let optionsRead = false;
    const options = {
        get largestUnit() {
            optionsRead = true;
            return undefined;
        }
    };
    shouldThrow(() => dateTime[method](earliestDate, options), RangeError);
    shouldBe(optionsRead, false);
}

// The next day's midnight is within the limits.
shouldBe(Temporal.PlainDateTime.from(nextDate).toString(), "-271821-04-20T00:00:00");
shouldBe(Temporal.PlainDateTime.compare(nextDate, dateTime), -1);
shouldBe(dateTime.equals(nextDate), false);
shouldBe(dateTime.since(nextDate, { largestUnit: "days" }).days, 100010957);
shouldBe(Temporal.PlainDateTime.from(new Temporal.PlainDate(275760, 9, 13)).toString(), "+275760-09-13T00:00:00");
