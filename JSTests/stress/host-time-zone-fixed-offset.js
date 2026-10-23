//@ requireOptions("--useDollarVM=1", "--useTemporal=1")

// The host time zone can be a zone at a fixed offset that has no name. ICU knows such a zone as a custom ID,
// "GMT+05:30", and ECMA-262 as an offset time zone, "+05:30". Both set it, and the zone is the one they say: Date,
// Intl.DateTimeFormat and Temporal.Now agree on it. A whole number of hours has an IANA name and takes it.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function hostTimeZone() {
    const date = new Date(2026, 0, 15, 9);
    return [date.getTimezoneOffset(), date.toISOString(), new Intl.DateTimeFormat().resolvedOptions().timeZone, Temporal.Now.timeZoneId()].join(" ");
}

for (const notATimeZone of ["+5:30", "+0530", "05:30", "+05:60", "+24:00", "+05:30:00", "MSK-3"])
    shouldBe($vm.setHostTimeZone(notATimeZone), false);

const cases = [
    ["GMT+5:30", "-330 2026-01-15T03:30:00.000Z +05:30 +05:30"],
    // What resolvedOptions().timeZone reports for an offset zone sets that zone again.
    ["+05:30", "-330 2026-01-15T03:30:00.000Z +05:30 +05:30"],
    ["GMT-03:30", "210 2026-01-15T12:30:00.000Z -03:30 -03:30"],
    ["-09:30", "570 2026-01-15T18:30:00.000Z -09:30 -09:30"],
    // The sign of an Etc/GMT name counts west. The sign of a custom ID counts east.
    ["GMT+3", "-180 2026-01-15T06:00:00.000Z Etc/GMT-3 Etc/GMT-3"],
    ["+03:00", "-180 2026-01-15T06:00:00.000Z Etc/GMT-3 Etc/GMT-3"],
    ["GMT+14", "-840 2026-01-14T19:00:00.000Z Etc/GMT-14 Etc/GMT-14"],
    ["GMT-12", "720 2026-01-15T21:00:00.000Z Etc/GMT+12 Etc/GMT+12"],
    // The IANA names stop at Etc/GMT+12 and Etc/GMT-14.
    ["GMT-13", "780 2026-01-15T22:00:00.000Z -13:00 -13:00"],
    ["GMT+15", "-900 2026-01-14T18:00:00.000Z +15:00 +15:00"],
    ["-00:00", "0 2026-01-15T09:00:00.000Z UTC UTC"],
    ["Asia/Tokyo", "-540 2026-01-15T00:00:00.000Z Asia/Tokyo Asia/Tokyo"],
];

// A time zone change takes effect the next time the VM is entered, so each check runs in its own task.
function run(index) {
    if (index === cases.length)
        return;
    const [timeZone, expected] = cases[index];
    shouldBe($vm.setHostTimeZone(timeZone), true);
    setTimeout(() => {
        shouldBe(hostTimeZone(), expected);
        run(index + 1);
    }, 0);
}
run(0);
