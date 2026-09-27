//@ requireOptions("--useTemporal=1")

// islamic-civil / islamic-tbla / islamic-umalqura and persian in years below 1. ICU4C's
// IslamicCivilCalendar::civilLeapYear() and PersianCalendar::isLeapYear() take a truncating `%` of a
// negative dividend there and call every such year leap, while ICU places the days themselves
// correctly. The bridge must report year and month lengths that agree with where the days are
// (and with icu4x): daysInYear is the distance to the next new year, the month lengths add up to
// it, inLeapYear matches it, day 30 of a 29-day final month constrains or rejects instead of
// aliasing the next year's day 1, and year/month arithmetic regulates the day against the real
// month length.

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${expected}, got ${actual}`);
}

function shouldThrow(func, errorType, label) {
    try {
        func();
    } catch (error) {
        if (error instanceof errorType)
            return;
        throw new Error(`${label}: expected ${errorType.name}, got ${error.constructor.name}`);
    }
    throw new Error(`${label}: expected ${errorType.name}, but no exception was thrown`);
}

function fields(date) {
    return `${date.year}-${date.monthCode}-${date.day}`;
}

// icu4x reference: (14 + 11 * year) mod 30 < 11 (TabularAlgorithm::year) and
// (25 * year + 11) mod 33 < 8 (calendrical_calculations persian::is_leap_year), floored.
function tabularLeapYear(calendar, year) {
    const mod = (a, n) => ((a % n) + n) % n;
    if (calendar === "persian")
        return mod(25 * year + 11, 33) < 8;
    return mod(14 + 11 * year, 30) < 11;
}

const hijriCalendars = ["islamic-civil", "islamic-tbla", "islamic-umalqura"];

for (const calendar of [...hijriCalendars, "persian"]) {
    const isPersian = calendar === "persian";
    const commonYear = isPersian ? 365 : 354;
    for (let year = -70; year <= 5; ++year) {
        const label = `${calendar} ${year}`;
        const first = Temporal.PlainDate.from({ year, month: 1, day: 1, calendar });
        const next = Temporal.PlainDate.from({ year: year + 1, month: 1, day: 1, calendar });
        const leap = tabularLeapYear(calendar, year);
        const span = first.until(next, { largestUnit: "days" }).days;

        shouldBe(first.inLeapYear, leap, `${label} inLeapYear`);
        shouldBe(first.daysInYear, commonYear + leap, `${label} daysInYear`);
        shouldBe(span, first.daysInYear, `${label} days until next 1/1`);
        shouldBe(first.monthsInYear, 12, `${label} monthsInYear`);

        let sum = 0;
        for (let month = 1; month <= 12; ++month) {
            const monthStart = Temporal.PlainDate.from({ year, month, day: 1, calendar });
            const nextMonthStart = month < 12 ? Temporal.PlainDate.from({ year, month: month + 1, day: 1, calendar }) : next;
            shouldBe(monthStart.until(nextMonthStart, { largestUnit: "days" }).days, monthStart.daysInMonth, `${label} M${month} daysInMonth vs next month start`);
            sum += monthStart.daysInMonth;
        }
        shouldBe(sum, first.daysInYear, `${label} sum of daysInMonth`);

        const lastMonth = Temporal.PlainDate.from({ year, month: 12, day: 1, calendar });
        shouldBe(lastMonth.daysInMonth, 29 + leap, `${label} M12 daysInMonth`);
        shouldBe(Temporal.PlainYearMonth.from({ year, month: 12, calendar }).daysInMonth, 29 + leap, `${label} PlainYearMonth M12 daysInMonth`);
        shouldBe(Temporal.PlainDateTime.from({ year, month: 12, day: 1, calendar }).daysInYear, commonYear + leap, `${label} PlainDateTime daysInYear`);
        shouldBe(Temporal.ZonedDateTime.from({ year, month: 12, day: 1, calendar, timeZone: "UTC" }).inLeapYear, leap, `${label} ZonedDateTime inLeapYear`);

        // The year's last day is day 354/355 (365/366), and the day after it is the next year's day 1.
        const lastDay = lastMonth.with({ day: 30 });
        shouldBe(lastDay.day, 29 + leap, `${label} with({day: 30}) constrains`);
        shouldBe(lastDay.dayOfYear, commonYear + leap, `${label} last dayOfYear`);
        shouldBe(fields(lastDay.add({ days: 1 })), `${year + 1}-M01-1`, `${label} day after the last day`);

        const constrained = Temporal.PlainDate.from({ year, month: 12, day: 30, calendar });
        shouldBe(constrained.day, 29 + leap, `${label} from day 30 constrains`);
        shouldBe(constrained.equals(next), false, `${label} 12-30 is not the next year's 1-1`);
        if (leap)
            shouldBe(Temporal.PlainDate.from({ year, monthCode: "M12", day: 30, calendar }, { overflow: "reject" }).day, 30, `${label} reject accepts day 30`);
        else
            shouldThrow(() => Temporal.PlainDate.from({ year, monthCode: "M12", day: 30, calendar }, { overflow: "reject" }), RangeError, `${label} reject day 30`);
    }
}

// Year/month arithmetic regulates the day against the real month length (pre-fix results in comments).
for (const calendar of hijriCalendars) {
    // -4 is leap (30-day Dhu al-Hijjah); -3, -7 and -2 are common.
    const leapLastDay = Temporal.PlainDate.from({ year: -4, month: 12, day: 30, calendar });
    shouldBe(fields(leapLastDay.add({ years: 1 })), "-3-M12-29", `${calendar} -4-12-30 + P1Y`); // was -2-M01-30
    shouldBe(fields(leapLastDay.subtract({ years: 3 })), "-7-M12-29", `${calendar} -4-12-30 - P3Y`); // was -6-M01-30
    shouldBe(fields(leapLastDay.add({ years: 3 })), "-1-M12-30", `${calendar} -4-12-30 + P3Y (leap to leap)`);
    shouldThrow(() => leapLastDay.add({ years: 1 }, { overflow: "reject" }), RangeError, `${calendar} -4-12-30 + P1Y reject`);
    shouldThrow(() => leapLastDay.subtract({ years: 3 }, { overflow: "reject" }), RangeError, `${calendar} -4-12-30 - P3Y reject`);
    shouldBe(fields(leapLastDay.add({ years: 3 }, { overflow: "reject" })), "-1-M12-30", `${calendar} -4-12-30 + P3Y reject (fits)`);

    const muharram30 = Temporal.PlainDate.from({ year: -7, month: 1, day: 30, calendar });
    shouldBe(fields(muharram30.add({ months: 11 })), "-7-M12-29", `${calendar} -7-01-30 + P11M`);
    shouldBe(fields(muharram30.add({ months: 23 })), "-6-M12-30", `${calendar} -7-01-30 + P23M (-6 is leap)`);
    shouldBe(fields(muharram30.add({ months: 35 })), "-5-M12-29", `${calendar} -7-01-30 + P35M`);
    shouldThrow(() => muharram30.add({ months: 11 }, { overflow: "reject" }), RangeError, `${calendar} -7-01-30 + P11M reject`);
    shouldBe(fields(muharram30.add({ months: 23 }, { overflow: "reject" })), "-6-M12-30", `${calendar} -7-01-30 + P23M reject (fits)`);
    shouldBe(fields(muharram30.add({ months: 11, days: 1 })), "-6-M01-1", `${calendar} -7-01-30 + P11M1D`);

    // until is the inverse of add.
    const nextNewYear = Temporal.PlainDate.from({ year: -5, month: 1, day: 1, calendar });
    shouldBe(muharram30.until(nextNewYear, { largestUnit: "years" }).toString(), "P1Y11M1D", `${calendar} until years`);
    shouldBe(muharram30.until(nextNewYear, { largestUnit: "months" }).toString(), "P23M1D", `${calendar} until months`);
    shouldBe(nextNewYear.until(muharram30, { largestUnit: "months" }).toString(), "-P23M1D", `${calendar} until months reversed`);
    shouldBe(muharram30.add("P1Y11M1D").equals(nextNewYear), true, `${calendar} add(until) round-trips`);

    // PlainYearMonth arithmetic takes the same path.
    shouldBe(Temporal.PlainYearMonth.from({ year: -4, month: 12, calendar }).add({ years: 1 }).daysInMonth, 29, `${calendar} PlainYearMonth -4-12 + P1Y daysInMonth`);
    shouldBe(Temporal.PlainYearMonth.from({ year: -7, month: 1, calendar }).until(Temporal.PlainYearMonth.from({ year: -5, month: 1, calendar })).toString(), "P2Y", `${calendar} PlainYearMonth until`);
}

{
    // persian: -3 is leap (30-day Esfand); -2, -1 and 0 are common.
    const calendar = "persian";
    const bahman30 = Temporal.PlainDate.from({ year: -1, month: 11, day: 30, calendar });
    shouldBe(fields(bahman30.add({ months: 1 })), "-1-M12-29", "persian -1-11-30 + P1M"); // threw RangeError
    shouldThrow(() => bahman30.add({ months: 1 }, { overflow: "reject" }), RangeError, "persian -1-11-30 + P1M reject");
    const leapLastDay = Temporal.PlainDate.from({ year: -3, month: 12, day: 30, calendar });
    shouldBe(fields(leapLastDay.add({ years: 2 })), "-1-M12-29", "persian -3-12-30 + P2Y");
    shouldBe(fields(leapLastDay.add({ years: 4 })), "1-M12-30", "persian -3-12-30 + P4Y (leap to leap)");
    shouldBe(fields(leapLastDay.subtract({ years: 4 })), "-7-M12-30", "persian -3-12-30 - P4Y (leap to leap)");
    shouldBe(fields(leapLastDay.subtract({ years: 1 })), "-4-M12-29", "persian -3-12-30 - P1Y");
    shouldThrow(() => leapLastDay.add({ years: 2 }, { overflow: "reject" }), RangeError, "persian -3-12-30 + P2Y reject");
    const farvardin31 = Temporal.PlainDate.from({ year: -2, month: 1, day: 31, calendar });
    shouldBe(fields(farvardin31.add({ months: 11 })), "-2-M12-29", "persian -2-01-31 + P11M");
    shouldBe(fields(farvardin31.add({ months: 23 })), "-1-M12-29", "persian -2-01-31 + P23M");
    const target = Temporal.PlainDate.from({ year: 0, month: 1, day: 1, calendar });
    shouldBe(farvardin31.until(target, { largestUnit: "years" }).toString(), "P1Y11M1D", "persian until years");
    shouldBe(farvardin31.until(target, { largestUnit: "months" }).toString(), "P23M1D", "persian until months");
    shouldBe(farvardin31.add("P1Y11M1D").equals(target), true, "persian add(until) round-trips");
}

// The same dates agree with icu4x on the ISO side (year 1 of each calendar is unaffected).
shouldBe(Temporal.PlainDate.from({ year: -7, month: 1, day: 1, calendar: "islamic-civil" }).toString(), "0614-10-14[u-ca=islamic-civil]", "islamic-civil -7-01-01");
shouldBe(Temporal.PlainDate.from({ year: -7, month: 12, day: 29, calendar: "islamic-civil" }).toString(), "0615-10-02[u-ca=islamic-civil]", "islamic-civil -7-12-29");
shouldBe(Temporal.PlainDate.from({ year: -6, month: 1, day: 1, calendar: "islamic-civil" }).toString(), "0615-10-03[u-ca=islamic-civil]", "islamic-civil -6-01-01");
shouldBe(Temporal.PlainDate.from({ year: -7, month: 1, day: 1, calendar: "islamic-tbla" }).toString(), "0614-10-13[u-ca=islamic-tbla]", "islamic-tbla -7-01-01");
shouldBe(Temporal.PlainDate.from({ year: -7, month: 1, day: 1, calendar: "islamic-umalqura" }).toString(), "0614-10-14[u-ca=islamic-umalqura]", "islamic-umalqura -7-01-01");
shouldBe(Temporal.PlainDate.from({ year: -1, month: 1, day: 1, calendar: "persian" }).toString(), "0620-03-21[u-ca=persian]", "persian -1-01-01");
shouldBe(Temporal.PlainDate.from({ year: -1, month: 12, day: 29, calendar: "persian" }).toString(), "0621-03-20[u-ca=persian]", "persian -1-12-29");
shouldBe(Temporal.PlainDate.from({ year: 0, month: 1, day: 1, calendar: "persian" }).toString(), "0621-03-21[u-ca=persian]", "persian 0-01-01");

// Years from 1 on were already right and must not move: 2, 5, 7, 10 and 13 AH are leap, 1403 AP is leap.
for (const calendar of hijriCalendars) {
    for (const [year, leap] of [[1, false], [2, true], [3, false], [5, true], [7, true], [8, false], [10, true], [13, true], [30, false]]) {
        const first = Temporal.PlainDate.from({ year, month: 1, day: 1, calendar });
        shouldBe(first.inLeapYear, leap, `${calendar} ${year} inLeapYear`);
        shouldBe(first.daysInYear, 354 + leap, `${calendar} ${year} daysInYear`);
    }
}
shouldBe(Temporal.PlainDate.from({ year: 1403, month: 12, day: 1, calendar: "persian" }).daysInMonth, 30, "persian 1403 Esfand");
shouldBe(Temporal.PlainDate.from({ year: 1404, month: 12, day: 1, calendar: "persian" }).daysInMonth, 29, "persian 1404 Esfand");
// islamic-umalqura keeps reading its 1300-1600 AH table from ICU, where it departs from the civil rule:
// 1442 is civil-leap but has a 29-day Dhu al-Hijjah, 1444 is civil-common but has a 30-day one and a 30-day Safar.
shouldBe(Temporal.PlainDate.from({ year: 1442, month: 12, day: 1, calendar: "islamic-civil" }).daysInMonth, 30, "islamic-civil 1442 Dhu al-Hijjah");
shouldBe(Temporal.PlainDate.from({ year: 1442, month: 12, day: 1, calendar: "islamic-umalqura" }).daysInMonth, 29, "islamic-umalqura 1442 Dhu al-Hijjah");
shouldThrow(() => Temporal.PlainDate.from({ year: 1444, month: 12, day: 30, calendar: "islamic-civil" }, { overflow: "reject" }), RangeError, "islamic-civil 1444-12-30 reject");
shouldBe(Temporal.PlainDate.from({ year: 1444, month: 12, day: 30, calendar: "islamic-umalqura" }, { overflow: "reject" }).toString(), "2023-07-18[u-ca=islamic-umalqura]", "islamic-umalqura 1444-12-30");
const safar30 = Temporal.PlainDate.from({ year: 1444, month: 2, day: 30, calendar: "islamic-umalqura" }, { overflow: "reject" });
shouldBe(fields(safar30.add({ months: 1 })), "1444-M03-29", "islamic-umalqura 1444-02-30 + P1M");
shouldBe(fields(safar30.add({ months: 10 })), "1444-M12-30", "islamic-umalqura 1444-02-30 + P10M");
shouldBe(fields(safar30.add({ years: 1 })), "1445-M02-30", "islamic-umalqura 1444-02-30 + P1Y");
shouldBe(safar30.until(safar30.add({ months: 10 }), { largestUnit: "months" }).toString(), "P10M", "islamic-umalqura until inside the table");
