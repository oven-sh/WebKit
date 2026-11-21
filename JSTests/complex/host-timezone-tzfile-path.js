function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// TZ can name a tzfile by its path. The zone is the one the path names after "/zoneinfo/", with its daylight saving
// time.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), 300);
shouldBe(summer.getTimezoneOffset(), 240);
shouldBe(winter.toISOString(), `2026-01-15T14:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `America/New_York`);
shouldBe(Temporal.Now.timeZoneId(), `America/New_York`);
