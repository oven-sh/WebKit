// TZ=asia/calcutta. ICU has no zone of that spelling and hands the name back as the host's zone. A time zone
// identifier is found whatever its case, and keeps the spelling that ICU's list of zone names has. Where Asia/Calcutta
// is an alias of Asia/Kolkata it stays Asia/Calcutta, as an alias that a program names does. ICU is asked about no
// other name of its list.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${actual}, expected ${expected}`);
}

const january = new Date(2024, 0, 15, 12);
shouldBe(january.getTimezoneOffset(), -330);
shouldBe(new Date(2024, 6, 15, 12).getTimezoneOffset(), -330);
shouldBe(january.getTime(), Date.UTC(2024, 0, 15, 6, 30));
shouldBe(january.toString(), "Mon Jan 15 2024 12:00:00 GMT+0530 (India Standard Time)");
shouldBe($vm.hasListedICUTimeZoneNames(), true);
shouldBe($vm.hasListedPrimaryTimeZones(), false);

shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, "Asia/Calcutta");
shouldBe(Temporal.Now.timeZoneId(), "Asia/Calcutta");
shouldBe(Temporal.Now.zonedDateTimeISO().offsetNanoseconds, 19800e9);
shouldBe($vm.hasListedPrimaryTimeZones(), false);
