function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

function shouldThrowRangeError(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError))
        throw new Error('not a RangeError: ' + error);
}

// The tz database has EST5EDT, CST6CDT, MST7MDT and PST8PDT as zones of their own before 2024b and from 2026d on. ICU
// then holds them canonical, and a zone that ICU holds canonical is a time zone, with or without '/'. Upstream drops
// a canonical ID with no '/' other than UTC and GMT: the host zone is then UTC, and the name is a RangeError.
// ICU_TIMEZONE_FILES_DIR=icu-tz-2024a gives ICU the time zone types of tz 2024a, whatever data it was built with.
const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), 300);
shouldBe(summer.getTimezoneOffset(), 240);
shouldBe(winter.toISOString(), `2026-01-15T14:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `EST5EDT`);
shouldBe(Temporal.Now.timeZoneId(), `EST5EDT`);

const usZones = [`CST6CDT`, `EST5EDT`, `MST7MDT`, `PST8PDT`];
for (const zone of usZones) {
    shouldBe(new Intl.DateTimeFormat(`en`, { timeZone: zone }).resolvedOptions().timeZone, zone);
    shouldBe(new Intl.DateTimeFormat(`en`, { timeZone: zone.toLowerCase() }).resolvedOptions().timeZone, zone);
    shouldBe(new Temporal.ZonedDateTime(0n, zone).timeZoneId, zone);
}
shouldBe(new Date(Date.UTC(2026, 6, 15, 12)).toLocaleTimeString(`en-US`, { timeZone: `PST8PDT`, hour12: false }), `05:00:00`);
shouldBe(JSON.stringify(Intl.supportedValuesOf(`timeZone`).filter(zone => !zone.includes(`/`))), JSON.stringify([...usZones, `UTC`]));

// With these time zone types ICU also holds two IDs canonical that are not time zones: "Factory", the tz database's
// zone for a clock that is not set, and the "SystemV/" zones, which the tz database does not have.
for (const zone of [`Factory`, `SystemV/EST5EDT`])
    shouldThrowRangeError(() => new Intl.DateTimeFormat(`en`, { timeZone: zone }));
