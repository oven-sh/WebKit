// setFullYear, setMonth, setDate, their UTC forms and setYear build the new time value with
// MakeDate(MakeDay(year, month, date), TimeWithinDay(t)). MakeDay range-checks
// year + floor(month / 12), never the year or the month count alone, and the result is clipped
// only at the end. So a year or a month count that is out of range alone is fine when the
// combination is a date, and a setter gives what Date.UTC and the Date constructor give for the
// same fields.
// https://tc39.es/ecma262/#sec-makeday

function shouldBe(actual, expected) {
    if (Number.isNaN(expected) ? !Number.isNaN(actual) : actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const startOfYear275760 = 8639977881600000; // The last year that has time values.
const minTimeValue = -8.64e15; // -271821-04-20T00:00:00Z
const maxTimeValue = 8.64e15; // +275760-09-13T00:00:00Z
const startOfYear0 = -62167219200000; // Date.UTC(0, 0, 1) is 1900.
shouldBe(Date.UTC(275760, 0, 1), startOfYear275760);
shouldBe(Date.UTC(-271821, 3, 20), minTimeValue);
shouldBe(Date.UTC(275760, 8, 13), maxTimeValue);

// A year that is out of range alone, and a month count that brings it back.
shouldBe(new Date(0).setUTCFullYear(275761, -12, 1), startOfYear275760);
shouldBe(new Date(0).setUTCFullYear(275761, -12), startOfYear275760);
shouldBe(new Date(0).setUTCFullYear(300000, -300000, 1), Date.UTC(275000, 0, 1));
shouldBe(new Date(0).setUTCFullYear(-300000, 12 * 302000), Date.UTC(2000, 0, 1));
shouldBe(new Date(NaN).setUTCFullYear(-271821, 3600000), Date.UTC(28179, 0, 1));
shouldBe(new Date(2000, 0, 1).setFullYear(275761, -12, 1), new Date(275760, 0, 1).getTime());
shouldBe(new Date(2000, 0, 1).setFullYear(275761, -12), new Date(275760, 0, 1).getTime());
shouldBe(new Date(2000, 0, 1).setFullYear(300000, -300000, 1), new Date(275000, 0, 1).getTime());

// A month count that spans more years than a year can be: 275760 * 12 is 3309120.
shouldBe(new Date(Date.UTC(-1, 0, 1)).setUTCMonth(12 * 275761), startOfYear275760);
shouldBe(new Date(Date.UTC(-271821, 3, 20)).setUTCMonth(3 + 12 * (275760 + 271821)), Date.UTC(275760, 3, 20));
shouldBe(new Date(startOfYear275760).setUTCMonth(3 - 12 * (275760 + 271821), 20), minTimeValue);
shouldBe(new Date(startOfYear0).setUTCMonth(3309120), startOfYear275760);
shouldBe(new Date(startOfYear0).setUTCMonth(3309121), Date.UTC(275760, 1, 1));
shouldBe(new Date(startOfYear0).setUTCMonth(3309121, 15), Date.UTC(275760, 1, 15));
shouldBe(new Date(Date.UTC(4000, 0, 1)).setUTCMonth(-3309120), Date.UTC(-271760, 0, 1));
shouldBe(new Date(Date.UTC(4000, 0, 1)).setUTCMonth(-3309121), Date.UTC(-271761, 11, 1));
shouldBe(new Date(-1000, 0, 1).setMonth(12 * 276000), new Date(275000, 0, 1).getTime());
shouldBe(new Date(-1000, 0, 1).setMonth(12 * 276000 + 5, 6), new Date(275000, 5, 6).getTime());

// oven-sh/bun#46342, hit through date-fns differenceInMonths: 3472364 months from -187631-09-27.
{
    let date = new Date(0);
    date.setUTCFullYear(-187631, 8, 27);
    shouldBe(date.setUTCMonth(3472364), Date.UTC(101732, 8, 27));
    shouldBe(date.toISOString(), "+101732-09-27T00:00:00.000Z");
    shouldBe(new Date(Date.UTC(-270000, 8, 27)).setUTCMonth(280000 * 12 + 8), Date.UTC(10000, 8, 27));
    shouldBe(new Date(Date.UTC(270000, 8, 27)).setUTCMonth(-280000 * 12 + 8), Date.UTC(-10000, 8, 27));
    shouldBe(new Date(Date.UTC(2000, 0, 1)).setUTCFullYear(-270000, 6500000), Date.UTC(271666, 8, 1));
    date = new Date(2000, 0, 1);
    date.setFullYear(-187631, 8, 27);
    shouldBe(date.setMonth(3472364), new Date(101732, 8, 27).getTime());
}

// The month count is split with a floor division: every month from 2 years back to 2 years on.
for (let month = -25; month <= 25; ++month) {
    let year = 2000 + Math.floor(month / 12);
    let monthInYear = month - 12 * Math.floor(month / 12);
    let expected = new Date(Date.UTC(year, 0, 15, 1, 2, 3, 4)).setUTCMonth(monthInYear);
    shouldBe(new Date(Date.UTC(2000, 5, 15, 1, 2, 3, 4)).setUTCMonth(month), expected);
    shouldBe(new Date(Date.UTC(1999, 5, 15, 1, 2, 3, 4)).setUTCFullYear(2000, month), expected);
    shouldBe(Date.UTC(2000, month, 15, 1, 2, 3, 4), expected);
    let date = new Date(expected);
    shouldBe(date.getUTCFullYear(), year);
    shouldBe(date.getUTCMonth(), monthInYear);
    shouldBe(date.getUTCDate(), 15);
}
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(-13), Date.UTC(1998, 11, 15));
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(-12), Date.UTC(1999, 0, 15));
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(-1), Date.UTC(1999, 11, 15));
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(0), 947894400000);
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(11), 976838400000);
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(12), Date.UTC(2001, 0, 15));
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(13), 982195200000);
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(-1), 945216000000);
shouldBe(new Date(Date.UTC(2000, 5, 15)).setUTCMonth(-13), 913680000000);

// Only the result has a range. These are the last and the first time values, and one step on.
shouldBe(new Date(0).setUTCFullYear(275760, 8, 13), maxTimeValue);
shouldBe(new Date(0).setUTCFullYear(275760, 8, 14), NaN);
shouldBe(new Date(0).setUTCFullYear(275760, 9, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(275761), NaN);
shouldBe(new Date(0).setUTCFullYear(275761, 0, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(275760), startOfYear275760);
shouldBe(new Date(0).setUTCFullYear(-271821, 3, 20), minTimeValue);
shouldBe(new Date(0).setUTCFullYear(-271821, 3, 19), NaN);
shouldBe(new Date(0).setUTCFullYear(-271821), NaN);
shouldBe(new Date(0).setUTCFullYear(-271822, 0, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(-271820), Date.UTC(-271820, 0, 1));
shouldBe(new Date(0).setUTCMonth(12 * 273790 + 8, 13), maxTimeValue);
shouldBe(new Date(0).setUTCMonth(12 * 273790 + 8, 14), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 273790 + 9), NaN);
shouldBe(new Date(0).setUTCMonth(-12 * 273791 + 3, 20), minTimeValue);
shouldBe(new Date(0).setUTCMonth(-12 * 273791 + 3, 19), NaN);
shouldBe(new Date(0).setUTCMonth(-12 * 273791 + 2), NaN);
shouldBe(new Date(0).setFullYear(275761), NaN);
shouldBe(new Date(0).setFullYear(-271822), NaN);
shouldBe(new Date(0).setYear(275761), NaN);
shouldBe(new Date(0).setYear(-271822), NaN);

// The day count is part of the same sum. The start of 275761 is out of range and 200 days before
// it is not. Date.UTC and the Date constructor give the same, and so does V8.
shouldBe(new Date(0).setUTCFullYear(275761, 0, -200), Date.UTC(275761, 0, -200));
shouldBe(new Date(0).setUTCFullYear(275761, 0, -200), Date.UTC(275760, 0, 166));
shouldBe(new Date(0).setUTCFullYear(-271822, 0, 500), Date.UTC(-271821, 0, 135));
shouldBe(new Date(0).setUTCFullYear(400000, 0, -45379000), Date.UTC(400000, 0, -45379000));
shouldBe(new Date(0).setUTCFullYear(400000, 0, -45379000), 8639867894400000);
shouldBe(new Date(2000, 0, 1).setFullYear(275761, 0, -200), new Date(275761, 0, -200).getTime());
shouldBe(new Date(Date.UTC(275760, 0, 1)).setUTCMonth(12, -200), Date.UTC(275760, 0, 166));

// The day count reaches MakeDay as it is. When it was multiplied by msPerDay and added to the
// milliseconds first, a sum beyond 2^53 lost the last millisecond.
shouldBe(new Date(8.64e15 - 999).setUTCDate(-199000000), -8553601036800999);
shouldBe(new Date(8.64e15 - 999).setUTCDate(-199000000), Date.UTC(275760, 8, -199000000, 23, 59, 59, 1));
shouldBe(new Date(946688523001).setUTCFullYear(300000, 0, -108842264), 946688523001);
shouldBe(new Date(946688523001).setUTCMonth(12 * 298000, -108842264), 946688523001);
shouldBe(new Date(2000, 0, 1, 1, 2, 3, 457).setFullYear(300000, 0, -108842264), new Date(300000, 0, -108842264, 1, 2, 3, 457).getTime());
shouldBe(new Date(2000, 0, 1, 1, 2, 3, 457).setDate(-199000000), new Date(2000, 0, -199000000, 1, 2, 3, 457).getTime());

// MakeDay splits the month count into years and a month in 64-bit integers. That is exact for
// every safe integer, up to the last one.
shouldBe(new Date(0).setUTCFullYear(2000 - 750599937895082, 2 ** 53 - 1, 1), 965088000000); // 2000-08-01
shouldBe(Date.UTC(2000 - 750599937895082, 2 ** 53 - 1, 1), 965088000000);
shouldBe(new Date(0).setUTCFullYear(2000 + 750599937895083, -(2 ** 53 - 1), 1), 959817600000); // 2000-06-01
shouldBe(Date.UTC(2000 + 750599937895083, -(2 ** 53 - 1), 1), 959817600000);
shouldBe(new Date(0).setUTCFullYear(-1000000000, 12 * 1000002000 + 1, 1), 949363200000); // 2000-02-01
// A year or a month count beyond the safe integers is NaN. A double cannot split it exactly: it
// gave a wrong date (2000-01-01 for 2 ** 56 months from this year, which is 2000-05-01) or a
// year that wrapped in int arithmetic.
shouldBe(new Date(0).setUTCFullYear(-6004799503158661, 2 ** 56, 1), NaN);
shouldBe(Date.UTC(-6004799503158661, 2 ** 56, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(-750599937893082, 2 ** 53), NaN);
shouldBe(Date.UTC(-750599937893082, 2 ** 53), NaN);
shouldBe(new Date(0).setUTCFullYear(750599937897083, -(2 ** 53)), NaN);
shouldBe(Date.UTC(750599937897083, -(2 ** 53)), NaN);
shouldBe(new Date(0).setUTCMonth(2 ** 53), NaN);
shouldBe(new Date(0).setUTCMonth(-(2 ** 53)), NaN);
shouldBe(new Date(0).setUTCFullYear(2 ** 53), NaN);
shouldBe(new Date(0).setUTCFullYear(-9007197107257351, 108086391056891980, 784353026671), NaN);
shouldBe(Date.UTC(-9007197107257351, 108086391056891980, 784353026671), NaN);

// year + floor(month / 12) has to fit an int32. The day count can pull the last such year back.
shouldBe(new Date(0).setUTCFullYear(2 ** 31 - 1, 0, -784353015467), -124334438400000);
shouldBe(Date.UTC(2 ** 31 - 1, 0, -784353015467), -124334438400000);
shouldBe(new Date(0).setUTCFullYear(2 ** 31 - 2, 12, -784353015467), -124334438400000);
shouldBe(new Date(0).setUTCFullYear(2 ** 31, 0, -784353015467), NaN);
shouldBe(new Date(0).setUTCFullYear(2 ** 31 - 1, 12, -784353015467), NaN);
shouldBe(new Date(0).setUTCFullYear(-(2 ** 31), 0, 784353015833), -86400000);
shouldBe(Date.UTC(-(2 ** 31), 0, 784353015833), -86400000);
shouldBe(new Date(0).setUTCFullYear(-(2 ** 31) - 1, 0, 784353015833), NaN);
shouldBe(new Date(0).setUTCFullYear(-(2 ** 31), -1, 784353015833), NaN);
// A year or a month count beyond int32 does not wrap into range.
shouldBe(new Date(0).setUTCFullYear(2 ** 31 + 2000), NaN);
shouldBe(new Date(0).setUTCFullYear(2 ** 32 + 2000), NaN);
shouldBe(new Date(0).setUTCFullYear(2 ** 32 + 2000, 5, 6), NaN);
shouldBe(new Date(0).setUTCFullYear(2000, 12 * 2 ** 32), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 2 ** 31), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 2 ** 32), NaN);
shouldBe(new Date(0).setUTCMonth(-12 * 2 ** 32), NaN);
shouldBe(new Date(0).setUTCMonth(12 * 2 ** 32 + 5, 6), NaN);
shouldBe(new Date(0).setFullYear(2 ** 32 + 2000), NaN);
shouldBe(new Date(0).setMonth(12 * 2 ** 32), NaN);
shouldBe(new Date(0).setYear(2 ** 31 + 100), NaN);
shouldBe(new Date(0).setYear(2 ** 32 + 2000), NaN);
shouldBe(new Date(0).setYear(2 ** 53 + 2000), NaN);
shouldBe(new Date(0).setUTCFullYear(1e300, 0, -1e300), NaN);
shouldBe(new Date(0).setUTCFullYear(500000, -12 * (500000 - 2000)), Date.UTC(2000, 0, 1));

// An argument that is not finite is NaN, also when another one would cancel it.
shouldBe(new Date(0).setUTCFullYear(Infinity, -Infinity), NaN);
shouldBe(new Date(0).setUTCFullYear(-Infinity, Infinity, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(2000, Infinity, -Infinity), NaN);
shouldBe(new Date(0).setUTCFullYear(NaN, 0, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(2000, NaN, 1), NaN);
shouldBe(new Date(0).setUTCFullYear(2000, 0, NaN), NaN);
shouldBe(new Date(0).setUTCMonth(NaN, 1), NaN);
shouldBe(new Date(0).setUTCMonth(1, NaN), NaN);
shouldBe(new Date(0).setUTCDate(Infinity), NaN);
shouldBe(new Date(0).setFullYear(Infinity, -Infinity), NaN);
shouldBe(new Date(0).setYear(NaN), NaN);
// Every argument is still converted, in order, when an earlier one made the result NaN.
{
    let log = [];
    let argument = (name, value) => ({ valueOf() { log.push(name); return value; } });
    shouldBe(new Date(0).setUTCFullYear(argument("year", NaN), argument("month", 2 ** 53), argument("day", 1)), NaN);
    shouldBe(new Date(0).setUTCMonth(argument("month", Infinity), argument("day", 1)), NaN);
    shouldBe(log.join(), "year,month,day,month,day");
}

// Each setter with each number of arguments, and what it keeps of the date it starts from.
{
    let start = Date.UTC(2001, 1, 3, 4, 5, 6, 7);
    shouldBe(new Date(start).setUTCFullYear(1950), Date.UTC(1950, 1, 3, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCFullYear(1950, 14), Date.UTC(1951, 2, 3, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCFullYear(1950, 14, 40), Date.UTC(1951, 3, 9, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCMonth(14), Date.UTC(2002, 2, 3, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCMonth(14, 40), Date.UTC(2002, 3, 9, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCMonth(-3, -5), Date.UTC(2000, 8, 25, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCDate(40), Date.UTC(2001, 2, 12, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCDate(0), Date.UTC(2001, 0, 31, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCDate(-400), Date.UTC(1999, 11, 28, 4, 5, 6, 7));
    shouldBe(new Date(start).setUTCFullYear(1950, 14, 40), -591134093993);
    shouldBe(new Date(start).setUTCMonth(-3, -5), 969854706007);
    shouldBe(new Date(start).setUTCDate(-400), 946353906007);

    let local = new Date(2001, 1, 3, 4, 5, 6, 7);
    shouldBe(new Date(local).setFullYear(1950), new Date(1950, 1, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setFullYear(1950, 14), new Date(1950, 14, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setFullYear(1950, 14, 40), new Date(1950, 14, 40, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setMonth(14), new Date(2001, 14, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setMonth(14, 40), new Date(2001, 14, 40, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setDate(40), new Date(2001, 1, 40, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setDate(-400), new Date(2001, 1, -400, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setYear(1950), new Date(1950, 1, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setYear(50), new Date(1950, 1, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setYear(0), new Date(1900, 1, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setYear(99), new Date(1999, 1, 3, 4, 5, 6, 7).getTime());
    shouldBe(new Date(local).setYear(100), new Date(2001, 1, 3, 4, 5, 6, 7).setFullYear(100));
    shouldBe(new Date(local).setYear(-1), new Date(2001, 1, 3, 4, 5, 6, 7).setFullYear(-1));
    shouldBe(new Date(local).setYear(275760), new Date(275760, 1, 3, 4, 5, 6, 7).getTime());

    // A NaN date starts from +0 for setFullYear, and stays NaN for the others.
    shouldBe(new Date(NaN).setUTCFullYear(1950, 14, 40), Date.UTC(1951, 3, 9));
    shouldBe(new Date(NaN).setUTCFullYear(1950), Date.UTC(1950, 0, 1));
    shouldBe(new Date(NaN).setUTCMonth(14, 40), NaN);
    shouldBe(new Date(NaN).setUTCDate(40), NaN);
}

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
