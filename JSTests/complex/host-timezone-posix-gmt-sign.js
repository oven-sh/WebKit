function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// "GMT+3" in TZ is a POSIX value: three hours WEST of UTC. ICU also reads "GMT+3", as a custom ID three hours EAST.
// The host is set to what the C library reads.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), 180);
shouldBe(summer.getTimezoneOffset(), 180);
shouldBe(winter.toISOString(), `2026-01-15T12:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `Etc/GMT+3`);
shouldBe(Temporal.Now.timeZoneId(), `Etc/GMT+3`);
