// TZ=Invalid/Zone. ICU hands the name back as the host's zone. It is no time zone identifier, so the host's zone is
// UTC. Bun learns that from the list of ICU's zone names, and asks ICU about none of them.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${actual}, expected ${expected}`);
}

const january = new Date(2024, 0, 15, 12);
shouldBe(january.getTimezoneOffset(), 0);
shouldBe(new Date(2024, 6, 15, 12).getTimezoneOffset(), 0);
shouldBe(january.getTime(), Date.UTC(2024, 0, 15, 12));
shouldBe(january.toString(), "Mon Jan 15 2024 12:00:00 GMT+0000 (Coordinated Universal Time)");
shouldBe($vm.hasListedICUTimeZoneNames(), true);
shouldBe($vm.hasListedPrimaryTimeZones(), false);

shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, "UTC");
shouldBe(Temporal.Now.timeZoneId(), "UTC");
shouldBe($vm.hasListedPrimaryTimeZones(), false);
