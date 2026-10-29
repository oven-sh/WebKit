//@ requireOptions("--useTemporal=1", "--useDollarVM=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

// Links in the IANA Time Zone Database. CLDR has them as zones of their own up to 45 and from 49 on.
const links = [
    ["EST5EDT", "America/New_York", 240],
    ["CST6CDT", "America/Chicago", 300],
    ["MST7MDT", "America/Denver", 360],
    ["PST8PDT", "America/Los_Angeles", 420],
];

const supported = Intl.supportedValuesOf("timeZone");
const times = [Date.UTC(1850, 0, 15), Date.UTC(1925, 6, 15), Date.UTC(1955, 9, 15), Date.UTC(2025, 0, 15), Date.UTC(2025, 6, 15)];

for (const [link, primary] of links) {
    shouldBe(new Intl.DateTimeFormat("en", { timeZone: link }).resolvedOptions().timeZone, link);
    shouldBe(new Intl.DateTimeFormat("en", { timeZone: link.toLowerCase() }).resolvedOptions().timeZone, link);
    shouldBe(supported.includes(link), false);
    shouldBe(supported.includes(primary), true);

    // The local time and the names are the primary's, also where the United States' rules were not the city's.
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
