// Date.prototype.set{FullYear,Month,UTCFullYear,UTCMonth,Year} build their result with
// MakeDay(year, month, date), which range-checks year + floor(month / 12) and never year or month
// alone. So a year or month that is out of range by itself is fine as long as the combination
// (and the day offset) lands within the time value range, and the setters have to agree with
// Date.UTC / the Date constructor given the same fields.
// https://tc39.es/ecma262/#sec-makeday

function shouldBe(actual, expected) {
    if (Number.isNaN(expected) ? !Number.isNaN(actual) : actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const startOfYear275760 = 8639977881600000; // The last year with representable time values.
const minTimeValue = -8.64e15; // -271821-04-20T00:00:00Z
shouldBe(Date.UTC(275760, 0, 1), startOfYear275760);
shouldBe(Date.UTC(-271821, 3, 20), minTimeValue);

// UTC setters give what Date.UTC gives for the same fields.
shouldBe(new Date(0).setUTCFullYear(275761, -12, 1), startOfYear275760);
shouldBe(new Date(0).setUTCFullYear(275761, -12), startOfYear275760);
shouldBe(new Date(0).setUTCFullYear(300000, -300000, 1), Date.UTC(275000, 0, 1));
shouldBe(new Date(0).setUTCFullYear(-300000, 12 * 302000), Date.UTC(2000, 0, 1));
shouldBe(new Date(NaN).setUTCFullYear(-271821, 3600000), Date.UTC(28179, 0, 1));
shouldBe(new Date(Date.UTC(-1, 0, 1)).setUTCMonth(12 * 275761), startOfYear275760);
shouldBe(new Date(Date.UTC(-271821, 3, 20)).setUTCMonth(3 + 12 * (275760 + 271821)), Date.UTC(275760, 3, 20));
shouldBe(new Date(startOfYear275760).setUTCMonth(3 - 12 * (275760 + 271821), 20), minTimeValue);

// The day offset counts too: the start of 275761 is out of range, 200 days before it is not.
shouldBe(new Date(0).setUTCFullYear(275761, 0, -200), Date.UTC(275761, 0, -200));
shouldBe(new Date(0).setUTCFullYear(275761, 0, -200), Date.UTC(275760, 0, 166));
shouldBe(new Date(0).setUTCFullYear(-271822, 0, 500), Date.UTC(-271822, 0, 500));
shouldBe(new Date(0).setUTCFullYear(-271822, 0, 500), Date.UTC(-271821, 0, 135));

// That holds to the millisecond when |this| has a milliseconds part and the day count times
// msPerDay is beyond 2^53: the day count goes into MakeDay, it is not pre-multiplied into the time.
shouldBe(new Date(946688523001).setUTCFullYear(300000, 0, -108842264), Date.UTC(300000, 0, -108842264, 1, 2, 3, 1));
shouldBe(new Date(946688523001).setUTCFullYear(300000, 0, -108842264), 946688523001);
shouldBe(new Date(8.64e15 - 999).setUTCDate(-199000000), Date.UTC(275760, 8, -199000000, 23, 59, 59, 1));
shouldBe(new Date(8.64e15 - 999).setUTCDate(-199000000), -8553601036800999);

// Local time setters give what the Date constructor gives in the same time zone.
shouldBe(new Date(2000, 0, 1).setFullYear(300000, -300000, 1), new Date(275000, 0, 1).getTime());
shouldBe(new Date(2000, 0, 1).setFullYear(275761, -12), new Date(275760, 0, 1).getTime());
shouldBe(new Date(-1000, 0, 1).setMonth(12 * 276000), new Date(275000, 0, 1).getTime());
shouldBe(new Date(-1000, 0, 1).setMonth(12 * 276000 + 5, 6), new Date(275000, 5, 6).getTime());
shouldBe(new Date(2000, 0, 1, 1, 2, 3, 457).setFullYear(300000, 0, -108842264), new Date(300000, 0, -108842264, 1, 2, 3, 457).getTime());

// The result is still NaN when the combined year is really out of range. In particular a year or
// month count beyond int32 must not wrap around.
shouldBe(new Date(0).setUTCFullYear(275761, 0, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(275760, 12), NaN);
shouldBe(new Date(0).setUTCFullYear(-271822, 0, 1), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 275760), NaN);
shouldBe(new Date(0).setUTCFullYear(2 ** 31 + 2000), NaN);
shouldBe(new Date(0).setUTCFullYear(-(2 ** 31) - 1), NaN);
shouldBe(new Date(0).setUTCFullYear(2 ** 32 + 2000), NaN);
shouldBe(new Date(0).setUTCFullYear(2000, 12 * 2 ** 32), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 2 ** 31), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 2 ** 32), NaN);
shouldBe(new Date(0).setUTCMonth(-12 * 2 ** 32), NaN);
shouldBe(new Date(0).setFullYear(2 ** 32 + 2000), NaN);
shouldBe(new Date(0).setMonth(12 * 2 ** 32), NaN);
shouldBe(new Date(0).setYear(2 ** 31 + 100), NaN);
shouldBe(new Date(0).setYear(2 ** 32 + 2000), NaN);
shouldBe(new Date(0).setYear(275761), NaN);
shouldBe(new Date(0).setUTCFullYear(1e300, 0, -1e300), NaN);
shouldBe(new Date(0).setUTCFullYear(500000, -12 * (500000 - 2000)), Date.UTC(2000, 0, 1));
// A month count around 2^56 cannot be split into years and a month exactly. MakeDay answers NaN
// for this one rather than carrying a wrapped year into the day arithmetic.
shouldBe(new Date(0).setUTCFullYear(-9007197107257351, 108086391056891980, 784353026671), NaN);
shouldBe(Date.UTC(-9007197107257351, 108086391056891980, 784353026671), NaN);

// ... and when any argument is not finite, even if their sum would be.
shouldBe(new Date(0).setUTCFullYear(Infinity, -Infinity), NaN);
shouldBe(new Date(0).setUTCFullYear(-Infinity, Infinity, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(2000, Infinity, -Infinity), NaN);
shouldBe(new Date(0).setUTCMonth(NaN, 1), NaN);
shouldBe(new Date(0).setFullYear(Infinity, -Infinity), NaN);

// The Date holds the value the setter returned.
{
    let date = new Date(0);
    shouldBe(date.setUTCFullYear(275761, -12, 1), startOfYear275760);
    shouldBe(date.getTime(), startOfYear275760);
    shouldBe(date.getUTCFullYear(), 275760);
    shouldBe(date.getUTCMonth(), 0);
    shouldBe(date.getUTCDate(), 1);

    shouldBe(date.setUTCMonth(9), NaN); // October 275760 is past the last time value.
    shouldBe(date.getTime(), NaN);
}
