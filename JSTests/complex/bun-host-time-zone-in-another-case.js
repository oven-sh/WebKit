// TZ=america/los_angeles. ICU has no zone of that spelling and hands the name back as the host's zone. A time zone
// identifier is found whatever its case, so the host's zone is America/Los_Angeles. Bun finds it in the list of ICU's
// zone names, and asks ICU about no other name of that list.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${actual}, expected ${expected}`);
}

const january = new Date(2024, 0, 15, 12);
const july = new Date(2024, 6, 15, 12);
shouldBe(january.getTimezoneOffset(), 480);
shouldBe(july.getTimezoneOffset(), 420);
shouldBe(january.getTime(), Date.UTC(2024, 0, 15, 20));
shouldBe(january.toString(), "Mon Jan 15 2024 12:00:00 GMT-0800 (Pacific Standard Time)");
shouldBe(july.toString(), "Mon Jul 15 2024 12:00:00 GMT-0700 (Pacific Daylight Time)");
shouldBe($vm.hasListedICUTimeZoneNames(), true);
shouldBe($vm.hasListedPrimaryTimeZones(), false);

shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, "America/Los_Angeles");
shouldBe(Temporal.Now.timeZoneId(), "America/Los_Angeles");
shouldBe($vm.hasListedPrimaryTimeZones(), false);
