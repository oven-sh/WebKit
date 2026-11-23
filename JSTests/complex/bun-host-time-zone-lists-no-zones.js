// TZ=Europe/Berlin. The first local-time read of a Date names the host's zone. ICU is asked about that one zone: Bun
// does not list ICU's zone names for it, and so asks about none of them.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${actual}, expected ${expected}`);
}

function shouldListNoZones() {
    shouldBe($vm.hasListedICUTimeZoneNames(), false);
    shouldBe($vm.hasListedPrimaryTimeZones(), false);
}

const january = new Date(2024, 0, 15, 12);
const july = new Date(2024, 6, 15, 12);
shouldBe(january.getHours(), 12);
shouldListNoZones();
shouldBe(january.getTimezoneOffset(), -60);
shouldBe(july.getTimezoneOffset(), -120);
shouldBe(january.getTime(), Date.UTC(2024, 0, 15, 11));
// 02:30 on 31 March 2024 is a local time that the change to summer time skips.
shouldBe(new Date(2024, 2, 31, 2, 30).getTime(), Date.UTC(2024, 2, 31, 1, 30));
// 02:30 on 27 October 2024 comes twice, and is the first of the two.
shouldBe(new Date(2024, 9, 27, 2, 30).getTime(), Date.UTC(2024, 9, 27, 0, 30));
shouldBe(Date.parse("2024-07-15T12:00:00"), Date.UTC(2024, 6, 15, 10));
shouldBe(january.toString(), "Mon Jan 15 2024 12:00:00 GMT+0100 (Central European Standard Time)");
shouldBe(july.toString(), "Mon Jul 15 2024 12:00:00 GMT+0200 (Central European Summer Time)");
shouldListNoZones();

shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, "Europe/Berlin");
shouldBe(january.toLocaleString("en-US", { timeZoneName: "short" }), "1/15/2024, 12:00:00 PM GMT+1");
shouldBe(Temporal.Now.timeZoneId(), "Europe/Berlin");
shouldBe(Temporal.Now.zonedDateTimeISO().timeZoneId, "Europe/Berlin");
shouldListNoZones();
