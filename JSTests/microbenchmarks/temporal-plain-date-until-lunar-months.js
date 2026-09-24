//@ requireOptions("--useTemporal=1")
// Regression benchmark for NonISODateUntil (CalendarICUBridge.cpp) with largestUnit months or
// years in the lunisolar calendars. The month count comes from the epoch-day span, confirmed by
// one ICU month add, so the cost of a difference does not grow with the span; probing one
// candidate month at a time made a century of chinese months cost about a second.

const cases = [];
for (const calendar of ["chinese", "dangi", "hebrew"]) {
    const one = Temporal.PlainDate.from("1924-02-05").withCalendar(calendar);
    for (const years of [1, 25, 100])
        cases.push({ one, two: one.add({ years, days: 40 }), largestUnit: "months" });
    cases.push({ one, two: one.add({ years: 100, days: 40 }), largestUnit: "years" });
}

for (const { one, two, largestUnit } of cases) {
    const forward = one.until(two, { largestUnit });
    if (!one.add(forward).equals(two))
        throw new Error(`Bad difference ${forward} from ${one} to ${two}`);
}

let months = 0;
for (let i = 0; i < 10; ++i) {
    for (const { one, two, largestUnit } of cases) {
        months += one.until(two, { largestUnit }).months;
        months += two.since(one, { largestUnit }).months;
        months += two.until(one, { largestUnit }).months;
    }
}

if (months <= 0)
    throw new Error(`Bad total: ${months}`);
