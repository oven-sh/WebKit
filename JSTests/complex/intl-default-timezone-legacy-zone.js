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

// The tz database has EST5EDT, CST6CDT, MST7MDT and PST8PDT as zones of their own before 2024b and from 2026d on, and
// ICU then holds them canonical. In between, each is a link to a city zone. Here they stay aliases of those city zones
// with every tz data (tc39/ecma402#1103). Upstream has no zone for a canonical ID with no '/': the host zone is then
// UTC, and the name is a RangeError.
// ICU_TIMEZONE_FILES_DIR=icu-tz-2024a gives ICU the time zone types of tz 2024a, whatever data it was built with.
// Asia/Choibalsan is a primary identifier only with those types: this shows that ICU read them.
shouldBe(Intl.supportedValuesOf(`timeZone`).includes(`Asia/Choibalsan`), true);

const winter = new Date(2026, 0, 15, 9);
const summer = new Date(2026, 6, 15, 9);
shouldBe(winter.getTimezoneOffset(), 300);
shouldBe(summer.getTimezoneOffset(), 240);
shouldBe(winter.toISOString(), `2026-01-15T14:00:00.000Z`);
shouldBe(Intl.DateTimeFormat().resolvedOptions().timeZone, `America/New_York`);
shouldBe(Temporal.Now.timeZoneId(), `America/New_York`);
shouldBe(winter.toString(), `Thu Jan 15 2026 09:00:00 GMT-0500 (Eastern Standard Time)`);

const longName = zone => new Intl.DateTimeFormat(`en-US`, { timeZone: zone, timeZoneName: `long` }).formatToParts(0).find(part => part.type === `timeZoneName`).value;
// The hours east of UTC, one millisecond before and at each of the two changes of a year.
const sunday = (year, month, nth) => 1 + (7 - new Date(Date.UTC(year, month, 1)).getUTCDay()) % 7 + 7 * (nth - 1);
const hoursEast = (zone, time) => new Temporal.ZonedDateTime(BigInt(time) * 1000000n, zone).offsetNanoseconds / 3.6e12;
const usRule = (zone, year, standard) => {
    const start = Date.UTC(year, 2, sunday(year, 2, 2), 2 - standard);
    const end = Date.UTC(year, 10, sunday(year, 10, 1), 1 - standard);
    return `${zone} ${year}: ${[start - 1, start, end - 1, end].map(time => hoursEast(zone, time))}`;
};
const zones = [
    [`CST6CDT`, `America/Chicago`, `Central Standard Time`, -6],
    [`EST5EDT`, `America/New_York`, `Eastern Standard Time`, -5],
    [`MST7MDT`, `America/Denver`, `Mountain Standard Time`, -7],
    [`PST8PDT`, `America/Los_Angeles`, `Pacific Standard Time`, -8],
];
for (const [zone, city, name, standard] of zones) {
    // An alias keeps its name.
    shouldBe(new Intl.DateTimeFormat(`en`, { timeZone: zone }).resolvedOptions().timeZone, zone);
    shouldBe(new Intl.DateTimeFormat(`en`, { timeZone: zone.toLowerCase() }).resolvedOptions().timeZone, zone);
    shouldBe(new Temporal.ZonedDateTime(0n, zone).timeZoneId, zone);
    shouldBe(Temporal.Now.zonedDateTimeISO(zone).timeZoneId, zone);
    shouldBe(new Temporal.ZonedDateTime(0n, zone).equals(new Temporal.ZonedDateTime(0n, city)), true);
    // ICU is asked for the city zone. For the zone of its own it has no name but "GMT-05:00".
    shouldBe(longName(zone), name);
    // The name spells the US rule: daylight saving time from 02:00 on the second Sunday of March to 02:00 on the first
    // Sunday of November. From tz 2026d on the tz database has the name on that rule whatever the city zone does, so
    // the alias is right only while the city zone has the rule too. If this fails, a city zone left it: see A10 of
    // JSTests/BUN-TEST-DIFFERENCES.md.
    for (let year = 2026; year <= 2037; ++year)
        shouldBe(usRule(zone, year, standard), `${zone} ${year}: ${[standard, standard + 1, standard + 1, standard]}`);
}
shouldBe(new Date(Date.UTC(2026, 6, 15, 12)).toLocaleTimeString(`en-US`, { timeZone: `PST8PDT`, hour12: false }), `05:00:00`);
shouldBe(JSON.stringify(Intl.supportedValuesOf(`timeZone`).filter(zone => !zone.includes(`/`))), `["UTC"]`);

// With these time zone types ICU also holds two IDs canonical that are not time zones: "Factory", the tz database's
// zone for a clock that is not set, and the "SystemV/" zones, which the tz database does not have.
for (const zone of [`Factory`, `SystemV/EST5EDT`])
    shouldThrowRangeError(() => new Intl.DateTimeFormat(`en`, { timeZone: zone }));
