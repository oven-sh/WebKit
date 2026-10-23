function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// An offset that is not a whole number of hours has no IANA name. It is an offset time zone.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), -435);
shouldBe(summer.getTimezoneOffset(), -435);
shouldBe(winter.toISOString(), `2026-01-15T01:45:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `+07:15`);
shouldBe(Temporal.Now.timeZoneId(), `+07:15`);
