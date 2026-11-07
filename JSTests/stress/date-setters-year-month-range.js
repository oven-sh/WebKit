// MakeDay range-checks only year + floor(month / 12), not the year or the month count alone, so a
// date setter gives what Date.UTC and the Date constructor give: https://tc39.es/ecma262/#sec-makeday

// An expected value that is NaN fails: Date.UTC and the Date constructor share MakeDay with the setters.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldBeNaN(actual) {
    if (!Number.isNaN(actual))
        throw new Error(`expected NaN but got ${actual}`);
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
shouldBeNaN(new Date(0).setUTCFullYear(275760, 8, 14));
shouldBeNaN(new Date(0).setUTCFullYear(275760, 9, 1));
shouldBeNaN(new Date(0).setUTCFullYear(275761));
shouldBeNaN(new Date(0).setUTCFullYear(275761, 0, 1));
shouldBe(new Date(0).setUTCFullYear(275760), startOfYear275760);
shouldBe(new Date(0).setUTCFullYear(-271821, 3, 20), minTimeValue);
shouldBeNaN(new Date(0).setUTCFullYear(-271821, 3, 19));
shouldBeNaN(new Date(0).setUTCFullYear(-271821));
shouldBeNaN(new Date(0).setUTCFullYear(-271822, 0, 1));
shouldBe(new Date(0).setUTCFullYear(-271820), Date.UTC(-271820, 0, 1));
shouldBe(new Date(0).setUTCMonth(12 * 273790 + 8, 13), maxTimeValue);
shouldBeNaN(new Date(0).setUTCMonth(12 * 273790 + 8, 14));
shouldBeNaN(new Date(0).setUTCMonth(12 * 273790 + 9));
shouldBe(new Date(0).setUTCMonth(-12 * 273791 + 3, 20), minTimeValue);
shouldBeNaN(new Date(0).setUTCMonth(-12 * 273791 + 3, 19));
shouldBeNaN(new Date(0).setUTCMonth(-12 * 273791 + 2));
shouldBeNaN(new Date(0).setFullYear(275761));
shouldBeNaN(new Date(0).setFullYear(-271822));
shouldBeNaN(new Date(0).setYear(275761));
shouldBeNaN(new Date(0).setYear(-271822));

// The day count is part of the same sum: 200 days before the start of 275761 is in range.
shouldBe(new Date(0).setUTCFullYear(275761, 0, -200), Date.UTC(275761, 0, -200));
shouldBe(new Date(0).setUTCFullYear(275761, 0, -200), Date.UTC(275760, 0, 166));
shouldBe(new Date(0).setUTCFullYear(-271822, 0, 500), Date.UTC(-271821, 0, 135));
shouldBe(new Date(0).setUTCFullYear(400000, 0, -45379000), Date.UTC(400000, 0, -45379000));
shouldBe(new Date(0).setUTCFullYear(400000, 0, -45379000), 8639867894400000);
shouldBe(new Date(2000, 0, 1).setFullYear(275761, 0, -200), new Date(275761, 0, -200).getTime());
shouldBe(new Date(Date.UTC(275760, 0, 1)).setUTCMonth(12, -200), Date.UTC(275760, 0, 166));

// Date.UTC can see the order of MakeDay's sum, Day(t) + dt - 1, when the hours cancel a day count beyond 2^53.
shouldBe(Date.UTC(211, 2, 9007199255695816, -216172782121281900), 0);
shouldBe(Date.UTC(211, 2, 36028797027334450, -864691128640609200), 0);

// days * msPerDay is not added to the milliseconds first: that sum rounds beyond 2^53.
shouldBe(new Date(8.64e15 - 999).setUTCDate(-199000000), -8553601036800999);
shouldBe(new Date(8.64e15 - 999).setUTCDate(-199000000), Date.UTC(275760, 8, -199000000, 23, 59, 59, 1));
shouldBe(new Date(946688523001).setUTCFullYear(300000, 0, -108842264), 946688523001);
shouldBe(new Date(946688523001).setUTCMonth(12 * 298000, -108842264), 946688523001);
shouldBe(new Date(2000, 0, 1, 1, 2, 3, 457).setFullYear(300000, 0, -108842264), new Date(300000, 0, -108842264, 1, 2, 3, 457).getTime());
shouldBe(new Date(275760, 8, 1, 1, 2, 3, 457).setDate(-199000000), new Date(275760, 8, -199000000, 1, 2, 3, 457).getTime());

// The 64-bit split of the month count is exact, also beyond the safe integers. 12 * 2^53 is the last month
// count whose floor(month / 12) is an exact Number.
shouldBe(new Date(0).setUTCFullYear(2000 - 750599937895082, 2 ** 53 - 1, 1), 965088000000); // 2000-08-01
shouldBe(Date.UTC(2000 - 750599937895082, 2 ** 53 - 1, 1), 965088000000);
shouldBe(new Date(0).setUTCFullYear(2000 + 750599937895083, -(2 ** 53 - 1), 1), 959817600000); // 2000-06-01
shouldBe(Date.UTC(2000 + 750599937895083, -(2 ** 53 - 1), 1), 959817600000);
shouldBe(new Date(0).setUTCFullYear(-1000000000, 12 * 1000002000 + 1, 1), 949363200000); // 2000-02-01
shouldBe(new Date(0).setUTCFullYear(-750599937893082, 2 ** 53, 1), 967766400000); // 2000-09-01
shouldBe(Date.UTC(-750599937893082, 2 ** 53, 1), 967766400000);
shouldBe(new Date(0).setUTCFullYear(750599937897083, -(2 ** 53), 1), 957139200000); // 2000-05-01
shouldBe(Date.UTC(750599937897083, -(2 ** 53), 1), 957139200000);
shouldBe(new Date(0).setUTCFullYear(-6004799503158661, 2 ** 56, 1), 957139200000); // A double split gave 2000-01-01.
shouldBe(Date.UTC(-6004799503158661, 2 ** 56, 1), 957139200000);
shouldBe(new Date(0).setUTCFullYear(2000 - 2 ** 53, 12 * 2 ** 53, 1), 946684800000); // 2000-01-01
shouldBe(Date.UTC(2000 - 2 ** 53, 12 * 2 ** 53, 1), 946684800000);
shouldBe(new Date(0).setUTCFullYear(2000 + 2 ** 53, -12 * 2 ** 53, 1), 946684800000);
shouldBe(Date.UTC(2000 + 2 ** 53, -12 * 2 ** 53, 1), 946684800000);
// floor(month / 12) is the floor of the real quotient. A double quotient has no fraction from 2^52: for these month
// counts it is the next integer, and the date would be 2001-09-01.
shouldBe(new Date(0).setUTCFullYear(2000 - 2 ** 52, 12 * 2 ** 52 + 8, 1), 967766400000); // 2000-09-01
shouldBe(Date.UTC(2000 - 2 ** 52, 12 * 2 ** 52 + 8, 1), 967766400000);
shouldBe(new Date(0).setUTCFullYear(2002 + 2 ** 52, -12 * 2 ** 52 - 16, 1), 967766400000);
shouldBe(Date.UTC(2002 + 2 ** 52, -12 * 2 ** 52 - 16, 1), 967766400000);
// Beyond 12 * 2^53 ECMA-262 still defines a date: MakeDay rounds floor(month / 12) to a Number there (2000-05-01
// and 1998-09-01 for these). NaN is JSC's limit, not the specification's.
shouldBeNaN(new Date(0).setUTCFullYear(2000 - 2 ** 53, 12 * 2 ** 53 + 16, 1));
shouldBeNaN(Date.UTC(2000 - 2 ** 53, 12 * 2 ** 53 + 16, 1));
shouldBeNaN(new Date(0).setUTCFullYear(2000 + 2 ** 53, -12 * 2 ** 53 - 16, 1));
shouldBeNaN(Date.UTC(2000 + 2 ** 53, -12 * 2 ** 53 - 16, 1));
shouldBeNaN(new Date(0).setUTCMonth(2 ** 60));
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 60, -12 * 2 ** 53));

// year + floor(month / 12) has to fit an int32. The day count can pull the last such year back.
shouldBe(new Date(0).setUTCFullYear(2 ** 31 - 1, 0, -784353015467), -124334438400000);
shouldBe(Date.UTC(2 ** 31 - 1, 0, -784353015467), -124334438400000);
shouldBe(new Date(0).setUTCFullYear(2 ** 31 - 2, 12, -784353015467), -124334438400000);
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 31, 0, -784353015467));
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 31 - 1, 12, -784353015467));
shouldBe(new Date(0).setUTCFullYear(-(2 ** 31), 0, 784353015833), -86400000);
shouldBe(Date.UTC(-(2 ** 31), 0, 784353015833), -86400000);
shouldBeNaN(new Date(0).setUTCFullYear(-(2 ** 31) - 1, 0, 784353015833));
shouldBeNaN(new Date(0).setUTCFullYear(-(2 ** 31), -1, 784353015833));
// A year or a month count beyond int32 does not wrap into range.
shouldBeNaN(Date.UTC(2 ** 32 + 2000, 0, 1));
shouldBeNaN(Date.UTC(2000, 12 * 2 ** 32, 1));
shouldBeNaN(new Date(2 ** 32 + 2000, 0, 1).getTime());
shouldBeNaN(new Date(0).setUTCMonth(2 ** 53));
shouldBeNaN(new Date(0).setUTCMonth(-(2 ** 53)));
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 53));
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 31 + 2000));
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 32 + 2000));
shouldBeNaN(new Date(0).setUTCFullYear(2 ** 32 + 2000, 5, 6));
shouldBeNaN(new Date(0).setUTCFullYear(2000, 12 * 2 ** 32));
shouldBeNaN(new Date(0).setUTCMonth(12 * 2 ** 31));
shouldBeNaN(new Date(0).setUTCMonth(12 * 2 ** 32));
shouldBeNaN(new Date(0).setUTCMonth(-12 * 2 ** 32));
shouldBeNaN(new Date(0).setUTCMonth(12 * 2 ** 32 + 5, 6));
shouldBeNaN(new Date(0).setFullYear(2 ** 32 + 2000));
shouldBeNaN(new Date(0).setMonth(12 * 2 ** 32));
shouldBeNaN(new Date(0).setYear(2 ** 31 + 100));
shouldBeNaN(new Date(0).setYear(2 ** 32 + 2000));
shouldBeNaN(new Date(0).setYear(2 ** 53 + 2000));
shouldBeNaN(new Date(0).setUTCFullYear(1e300, 0, -1e300));
shouldBe(new Date(0).setUTCFullYear(500000, -12 * (500000 - 2000)), Date.UTC(2000, 0, 1));

// An argument that is not finite is NaN, also when another one would cancel it.
shouldBeNaN(new Date(0).setUTCFullYear(Infinity, -Infinity));
shouldBeNaN(new Date(0).setUTCFullYear(-Infinity, Infinity, 1));
shouldBeNaN(new Date(0).setUTCFullYear(2000, Infinity, -Infinity));
shouldBeNaN(new Date(0).setUTCFullYear(NaN, 0, 1));
shouldBeNaN(new Date(0).setUTCFullYear(2000, NaN, 1));
shouldBeNaN(new Date(0).setUTCFullYear(2000, 0, NaN));
shouldBeNaN(new Date(0).setUTCMonth(NaN, 1));
shouldBeNaN(new Date(0).setUTCMonth(1, NaN));
shouldBeNaN(new Date(0).setUTCDate(Infinity));
shouldBeNaN(new Date(0).setFullYear(Infinity, -Infinity));
shouldBeNaN(new Date(0).setYear(NaN));
// A year or a month count that is not an int32 is converted through a Number: truncated, and then split the same way.
shouldBe(new Date(0).setUTCFullYear(2000.9, 13.7, 1.5), Date.UTC(2001, 1, 1));
shouldBe(new Date(0).setUTCFullYear(-0.5, -0, 1), startOfYear0);
shouldBe(new Date(0).setUTCMonth(-7.9), Date.UTC(1969, 5, 1));
shouldBe(new Date(0).setUTCMonth("17"), Date.UTC(1971, 5, 1));
shouldBe(new Date(0).setUTCFullYear({ valueOf() { return 2000; } }, { valueOf() { return -7; } }), Date.UTC(1999, 5, 1));
shouldBe(new Date(2001, 1, 3, 4, 5, 6, 7).setFullYear(1950.5, 14.5, 40.5), new Date(1950, 14, 40, 4, 5, 6, 7).getTime());
shouldBe(new Date(2001, 1, 3, 4, 5, 6, 7).setYear(99.9), new Date(1999, 1, 3, 4, 5, 6, 7).getTime());
// Every argument is still converted, in order, when an earlier one made the result NaN.
{
    let log = [];
    let argument = (name, value) => ({ valueOf() { log.push(name); return value; } });
    shouldBeNaN(new Date(0).setUTCFullYear(argument("year", NaN), argument("month", 2 ** 53), argument("day", 1)));
    shouldBeNaN(new Date(0).setUTCMonth(argument("month", Infinity), argument("day", 1)));
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
    shouldBeNaN(new Date(NaN).setUTCMonth(14, 40));
    shouldBeNaN(new Date(NaN).setUTCDate(40));
}

// The Date holds the value the setter returned.
{
    let date = new Date(0);
    shouldBe(date.setUTCFullYear(275761, -12, 1), startOfYear275760);
    shouldBe(date.getTime(), startOfYear275760);
    shouldBe(date.getUTCFullYear(), 275760);
    shouldBe(date.getUTCMonth(), 0);
    shouldBe(date.getUTCDate(), 1);

    shouldBeNaN(date.setUTCMonth(9)); // October 275760 is past the last time value.
    shouldBeNaN(date.getTime());
}
