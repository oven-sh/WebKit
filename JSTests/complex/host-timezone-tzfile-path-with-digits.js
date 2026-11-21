function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// The path of a tzfile is read from TZ itself. ICU does not hand back a TZ that has a digit before its end (a tzdata
// version in a directory name), and ICU 80 hands back the name in a path and not the path (ICU-13694).
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), -540);
shouldBe(summer.getTimezoneOffset(), -540);
shouldBe(winter.toISOString(), `2026-01-15T00:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `Asia/Seoul`);
shouldBe(Temporal.Now.timeZoneId(), `Asia/Seoul`);
