//@ requireOptions("--useTemporal=1")

// A time zone identifier gets its entry in the table of time zones when it is first asked for, if it is a primary
// identifier in its own spelling. Anything else has all of them added. What was asked for before must not show.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + JSON.stringify(expected) + ", got " + JSON.stringify(actual));
}

function shouldThrowRangeError(callback, message) {
    let error;
    try {
        callback();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError))
        throw new Error(message + ": expected a RangeError, got " + error);
}

function resolved(timeZone) {
    return new Intl.DateTimeFormat("en", { timeZone }).resolvedOptions().timeZone;
}

function zoned(timeZone) {
    return new Temporal.ZonedDateTime(0n, timeZone);
}

// Before the whole table exists: UTC, which it starts out with, and primary identifiers.
shouldBe(new Temporal.ZonedDateTime(0n, "UTC").timeZoneId, "UTC", "UTC");
shouldBe(resolved("UTC"), "UTC", "UTC");
// Ones that are primary identifiers to IANA and to CLDR both: which of the two ICU goes by depends on the platform.
const early = ["Europe/Berlin", "America/New_York", "Asia/Tokyo", "America/Los_Angeles", "Australia/Sydney"];
const earlyZoned = early.map(zoned);
for (const name of early) {
    shouldBe(resolved(name), name, "a primary identifier");
    shouldBe(resolved(name), name, "the same again");
}

// Each of these takes the whole table.
shouldBe(resolved("US/Pacific"), "US/Pacific", "a link keeps its own identifier");
shouldBe(resolved("europe/berlin"), "Europe/Berlin", "another case");
shouldBe(resolved("EUROPE/BERLIN"), "Europe/Berlin", "another case");
shouldBe(resolved("utc"), "UTC", "another case");
for (const name of ["", "Europe/Berli", "Europe/Berlinx", " Europe/Berlin", "Etc/Unknown", "Foo/Bar", "PST", "SystemV/PST8PDT", "Europe/Berlin".repeat(20), "a".repeat(129), "\u{1F600}"])
    shouldThrowRangeError(() => resolved(name), JSON.stringify(name.slice(0, 40)));

// The identifiers from before are the ones the table has, once each and in order.
const all = Intl.supportedValuesOf("timeZone");
shouldBe(JSON.stringify(all), JSON.stringify([...new Set(all)].sort()), "sorted, and each once");
shouldBe(all.includes("UTC"), true, "UTC is listed");
for (const name of early)
    shouldBe(all.includes(name), true, name + " is listed");
shouldBe(all.includes("US/Pacific"), false, "a link is not listed");

// A link's primary identifier is an entry from before the table, and one from after it is the same entry.
shouldBe(zoned("US/Pacific").equals(earlyZoned[3]), true, "a link equals its primary identifier");
shouldBe(zoned("US/Pacific").equals(earlyZoned[1]), false, "and no other");
early.forEach((name, i) => {
    shouldBe(zoned(name).equals(earlyZoned[i]), true, name + " is the same time zone as before");
    shouldBe(zoned(name.toLowerCase()).equals(earlyZoned[i]), true, name + " in another case too");
    shouldBe(zoned(name.toLowerCase()).timeZoneId, name, name + " in another case");
});
for (const name of all) {
    shouldBe(resolved(name), name, "a listed identifier");
    shouldBe(zoned(name).timeZoneId, name, "a listed identifier");
}
