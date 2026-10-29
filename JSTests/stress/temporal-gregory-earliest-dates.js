//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

// The gregory calendar is proleptic: it has the ISO calendar's months and days from the first date on,
// which is the day before the first time value of Date.
function check(iso, eraYear, month, day) {
    const date = Temporal.PlainDate.from(iso).withCalendar("gregory");
    shouldBe(date.era, "bce");
    shouldBe(date.eraYear, eraYear);
    shouldBe(date.month, month);
    shouldBe(date.day, day);
    shouldBe(Temporal.PlainDate.from({ calendar: "gregory", era: "bce", eraYear, month, day }).withCalendar("iso8601").toString(), iso);

    const parts = new Intl.DateTimeFormat("en", { calendar: "gregory", year: "numeric", month: "numeric", day: "numeric", era: "short" }).formatToParts(date);
    shouldBe(parts.find(part => part.type === "year").value, String(eraYear));
    shouldBe(parts.find(part => part.type === "month").value, String(month));
    shouldBe(parts.find(part => part.type === "day").value, String(day));
}

check("-271821-04-19", 271822, 4, 19);
check("-271821-04-20", 271822, 4, 20);
for (let year = 271821; year >= 271814; --year) {
    for (const month of [1, 2, 3, 12]) {
        if (year === 271821 && month < 12)
            continue;
        check(`-${year}-${String(month).padStart(2, "0")}-01`, year + 1, month, 1);
        check(`-${year}-${String(month).padStart(2, "0")}-28`, year + 1, month, 28);
    }
}
