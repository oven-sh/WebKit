//@ requireOptions("--useTemporal=1", "--useDollarVM=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

// tzdata 2026d made these zones again and CLDR 49 treats them as canonical (as CLDR 45 and earlier did), but they should stay aliases.
// https://github.com/tc39/ecma402/issues/1103
const links = [
    ["EST5EDT", "America/New_York", 240],
    ["CST6CDT", "America/Chicago", 300],
    ["MST7MDT", "America/Denver", 360],
    ["PST8PDT", "America/Los_Angeles", 420],
    // Not part of that issue: tzdata before 2024b has these as zones too, and they are links from 2024b on.
    ["CET", "Europe/Brussels", -120],
    ["EET", "Europe/Athens", -180],
    ["MET", "Europe/Brussels", -120],
    ["WET", "Europe/Lisbon", -60],
];

const supported = Intl.supportedValuesOf("timeZone");
const times = [Date.UTC(1850, 0, 15), Date.UTC(1925, 6, 15), Date.UTC(1955, 9, 15), Date.UTC(2025, 0, 15), Date.UTC(2025, 6, 15)];

for (const [link, primary] of links) {
    shouldBe(new Intl.DateTimeFormat("en", { timeZone: link }).resolvedOptions().timeZone, link);
    shouldBe(new Intl.DateTimeFormat("en", { timeZone: link.toLowerCase() }).resolvedOptions().timeZone, link);
    shouldBe(supported.includes(link), false);
    shouldBe(supported.includes(primary), true);

    // Offsets and display names should match the primary zone's, including for old dates.
    for (const time of times) {
        for (const timeZoneName of ["short", "long", "longGeneric", "longOffset"]) {
            const options = { year: "numeric", month: "numeric", day: "numeric", hour: "numeric", minute: "numeric", second: "numeric", timeZoneName };
            shouldBe(new Intl.DateTimeFormat("en", { ...options, timeZone: link }).format(time), new Intl.DateTimeFormat("en", { ...options, timeZone: primary }).format(time));
        }
        const zoned = new Temporal.ZonedDateTime(BigInt(time) * 1000000n, link);
        shouldBe(zoned.timeZoneId, link);
        shouldBe(zoned.offset, zoned.withTimeZone(primary).offset);
        shouldBe(zoned.equals(zoned.withTimeZone(primary)), true);
    }
}

// A time-zone change takes effect the next time the VM is entered, so each check runs in its own task.
function checkHost(index) {
    if (index === links.length)
        return;
    const [link, primary, offset] = links[index];
    $vm.setHostTimeZone(link);
    setTimeout(() => {
        shouldBe(new Intl.DateTimeFormat().resolvedOptions().timeZone, primary);
        shouldBe(new Date(Date.UTC(2025, 6, 1)).getTimezoneOffset(), offset);
        checkHost(index + 1);
    }, 0);
}
checkHost(0);
