function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// The IANA names stop at Etc/GMT+12. An offset past that is an offset time zone.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), 780);
shouldBe(summer.getTimezoneOffset(), 780);
shouldBe(winter.toISOString(), `2026-01-15T22:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `-13:00`);
shouldBe(Temporal.Now.timeZoneId(), `-13:00`);
