function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// A POSIX TZ value gives a zone by its offset, not by a name: "MSK-3" is three hours east of UTC all year. The C
// library reads it, ICU has no zone with that ID, and the host zone must not become UTC for that. A whole number of
// hours has an IANA name. Its sign counts west, as the sign in TZ does.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), -180);
shouldBe(summer.getTimezoneOffset(), -180);
shouldBe(winter.toISOString(), `2026-01-15T06:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `Etc/GMT-3`);
shouldBe(Temporal.Now.timeZoneId(), `Etc/GMT-3`);
