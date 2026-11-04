function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// A POSIX value with a daylight saving rule is neither a named zone nor an offset zone, the two kinds ECMA-262 has.
// ICU does not read such a value: it names the zone that /etc/localtime is, or guesses one from the abbreviation. No
// zone is 7:15 from UTC, so that zone is never the one TZ gives, and the host zone is the standard offset of the value.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), -435);
shouldBe(summer.getTimezoneOffset(), -435);
shouldBe(winter.toISOString(), `2026-01-15T01:45:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `+07:15`);
shouldBe(Temporal.Now.timeZoneId(), `+07:15`);
