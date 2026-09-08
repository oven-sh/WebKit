//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`);
}

// NonISODateUntil (CalendarICUBridge.cpp) counts the months of a lunisolar calendar (chinese,
// dangi, hebrew) from the epoch-day span and confirms the count with one ICU month add, instead of
// probing one candidate month at a time. Everything below compares a result either with the same
// difference computed another way or with a value that does not depend on ICU's data.

// An independent month count: step one month at a time from the first day of one's month to the
// first day of two's month. Single steps never reach the ICU paths that the span count relies on.
function monthsBySingleSteps(one, two) {
    const sign = Temporal.PlainDate.compare(two, one);
    let cursor = one.subtract({ days: one.day - 1 });
    const end = two.subtract({ days: two.day - 1 });
    let months = 0;
    while (!cursor.equals(end)) {
        cursor = sign > 0 ? cursor.add({ months: 1 }) : cursor.subtract({ months: 1 });
        months += sign;
        if (sign * Temporal.PlainDate.compare(cursor, end) > 0)
            throw new Error(`stepped past ${end} from ${one}`);
    }
    // The candidate that reaches two's own month surpasses only on the day.
    if (months && sign * (one.day - two.day) > 0)
        months -= sign;
    return months;
}

function checkDifference(one, two, label, { singleSteps = true } = {}) {
    for (const [from, to] of [[one, two], [two, one]]) {
        const until = from.until(to, { largestUnit: "months" });
        shouldBe(until.years, 0, `${label} ${from} until ${to} years`);
        shouldBe(until.weeks, 0, `${label} ${from} until ${to} weeks`);
        // Adding the difference back lands on the other date: the defining property of DateUntil.
        shouldBe(from.add(until).equals(to), true, `${label} ${from} + ${until} round trip`);
        // Balanced: the day remainder is less than a month.
        shouldBe(Math.abs(until.days) < 30, true, `${label} ${from} until ${to} has ${until.days} days`);
        // since swaps receiver and argument, so it counts from `to`; the two counts agree whenever
        // neither end clamps, which a day-1 endpoint guarantees.
        if (from.day === 1 || to.day === 1)
            shouldBe(to.since(from, { largestUnit: "months" }).months, until.months, `${label} ${to} since ${from} months`);
        if (singleSteps)
            shouldBe(until.months, monthsBySingleSteps(from, to), `${label} ${from} until ${to} months by single steps`);
    }
}

for (const calendar of ["chinese", "dangi"]) {
    // These took seconds to minutes with one ICU walk per candidate month. A chinese/dangi month
    // step is itself slow in ICU4C, so instead of single steps the count is pinned; both ends sit
    // mid-month, where no ICU version moves them to another month.
    const longSpans = [
        ["0001-01-25", "2034-03-05", 25146],
        ["-002000-04-20", "-000001-12-12", 24732],
        ["1000-01-01", "1800-01-10", 9894],
        ["1899-12-20", "2100-06-20", 2479],
    ];
    for (const [a, b, months] of longSpans) {
        const one = Temporal.PlainDate.from(a).withCalendar(calendar);
        const two = Temporal.PlainDate.from(b).withCalendar(calendar);
        shouldBe(one.until(two, { largestUnit: "months" }).months, months, `${calendar} ${a} until ${b} months`);
        shouldBe(two.until(one, { largestUnit: "months" }).months, -months, `${calendar} ${b} until ${a} months`);
        shouldBe(one.since(two, { largestUnit: "months" }).months, -months, `${calendar} ${a} since ${b} months`);
        checkDifference(one, two, calendar, { singleSteps: false });
    }

    const shortSpans = [
        ["2023-03-22", "2023-04-20"], // M02 into the leap M02L that follows it
        ["2023-02-20", "2026-12-22"],
        ["1970-01-31", "1970-03-01"],
        ["2024-02-10", "2029-11-30"],
    ];
    for (const [a, b] of shortSpans)
        checkDifference(Temporal.PlainDate.from(a).withCalendar(calendar), Temporal.PlainDate.from(b).withCalendar(calendar), calendar);

    // A day-30 source clamps in the 29-day months along the way, but not in the count.
    const day30 = Temporal.PlainDate.from({ calendar, year: 2020, monthCode: "M04", day: 30 });
    shouldBe(day30.day, 30, `${calendar} 2020 M04 has 30 days`);
    checkDifference(day30, day30.add({ days: 50 * 29 + 17 }), `${calendar} day 30`);
    checkDifference(day30, Temporal.PlainDate.from({ calendar, year: 2025, monthCode: "M09", day: 29 }), `${calendar} day 30 to 29`);
}

// hebrew: 235 months are exactly 19 years. ICU 75 and later mis-add 229 or more months forward from
// Adar..Elul of a common year when the sum lands in Tishri..Adar I; the bridge routes around that.
{
    const nisan5720 = Temporal.PlainDate.from({ calendar: "hebrew", year: 5720, monthCode: "M07", day: 25 });
    shouldBe(nisan5720.inLeapYear, false, "5720 is a common year");
    const sums = [
        [229, 5739, "M01"],
        [235, 5739, "M07"],
        [463, 5757, "M12"],
        [464, 5758, "M01"],
        [470, 5758, "M07"],
        [2350, 5910, "M07"],
        [2351, 5910, "M08"],
    ];
    for (const [months, year, monthCode] of sums) {
        const sum = nisan5720.add({ months });
        shouldBe(`${sum.year} ${sum.monthCode} ${sum.day}`, `${year} ${monthCode} 25`, `5720 Nisan 25 + ${months} months`);
        shouldBe(sum.subtract({ months }).equals(nisan5720), true, `5720 Nisan 25 + ${months} months and back`);
        let stepped = nisan5720;
        for (let i = 0; i < months; ++i)
            stepped = stepped.add({ months: 1 });
        shouldBe(`${stepped.year} ${stepped.monthCode}`, `${year} ${monthCode}`, `5720 Nisan 25 + ${months} single months`);
    }

    const tishri5758 = Temporal.PlainDate.from({ calendar: "hebrew", year: 5758, monthCode: "M01", day: 14 });
    shouldBe(nisan5720.until(tishri5758, { largestUnit: "months" }).toString(), "P463M18D", "5720 Nisan 25 until 5758 Tishri 14");
    shouldBe(tishri5758.since(nisan5720, { largestUnit: "months" }).toString(), "P463M19D", "5758 Tishri 14 since 5720 Nisan 25");
    shouldBe(nisan5720.until(tishri5758, { largestUnit: "years" }).toString(), "P37Y5M18D", "5720 Nisan 25 until 5758 Tishri 14 in years");
    checkDifference(nisan5720, tishri5758, "hebrew");
    checkDifference(Temporal.PlainDate.from("1880-08-06").withCalendar("hebrew"), Temporal.PlainDate.from("2343-11-02").withCalendar("hebrew"), "hebrew");
    checkDifference(Temporal.PlainDate.from({ calendar: "hebrew", year: 5784, monthCode: "M05L", day: 30 }), Temporal.PlainDate.from({ calendar: "hebrew", year: 3784, monthCode: "M02", day: 29 }), "hebrew Adar I 30");
}

// islamic-* months are lunar too but twelve to every year; they share the month-phase tail with the
// lunisolar calendars, so whole years serve as a closed form to check it against.
for (const calendar of ["islamic-civil", "islamic-tbla", "islamic-umalqura"]) {
    const one = Temporal.PlainDate.from({ calendar, year: 1, monthCode: "M01", day: 1 });
    const two = Temporal.PlainDate.from({ calendar, year: 1601, monthCode: "M01", day: 1 });
    shouldBe(one.until(two, { largestUnit: "months" }).toString(), "P19200M", `${calendar} 1600 years in months`);
    shouldBe(two.until(one, { largestUnit: "months" }).toString(), "-P19200M", `${calendar} 1600 years in months, backwards`);
    checkDifference(Temporal.PlainDate.from("1990-03-30").withCalendar(calendar), Temporal.PlainDate.from("2077-07-07").withCalendar(calendar), calendar);
}
