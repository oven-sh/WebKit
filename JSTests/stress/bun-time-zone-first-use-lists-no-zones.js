//@ skip if $hostOS == "playstation"
//@ requireOptions("--useDollarVM=1")
//@ runDefault

// A program that names a time zone asks ICU about that zone and no other. The host's zone, which the first local-time
// read of a Date names, and a primary identifier as ICU spells it need nothing else. An alias, another case and a name
// that is no zone need the list of ICU's zone names. Only Intl.supportedValuesOf("timeZone") asks ICU about every one
// of those names.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

function expectRangeError(label, make)
{
    try {
        make();
    } catch (error) {
        if (error instanceof RangeError)
            return;
        throw new Error(`${label}: expected a RangeError, got ${error}`);
    }
    throw new Error(`${label}: expected a RangeError, got no error`);
}

// [has listed ICU's zone names, has asked ICU about every one of them]
function listed()
{
    return `${$vm.hasListedICUTimeZoneNames()}/${$vm.hasListedPrimaryTimeZones()}`;
}

// What `ask` does asks ICU about no name.
function expectNoQuestion(label, ask)
{
    const before = $vm.questionsAboutICUTimeZones();
    ask();
    expect(`${label}: questions to ICU`, $vm.questionsAboutICUTimeZones() - before, 0);
}

function check(name, identifier, offsetInJanuary)
{
    const january = BigInt(Date.UTC(2024, 0, 15, 12)) * 1000000n;
    const zoned = new Temporal.ZonedDateTime(january, name);
    expect(`${name}: timeZoneId`, zoned.timeZoneId, identifier);
    expect(`${name}: offset`, zoned.offsetNanoseconds / 60e9, offsetInJanuary);
    expect(`${name}: resolvedOptions`, new Intl.DateTimeFormat("en", { timeZone: name }).resolvedOptions().timeZone, identifier);
}

// The test runner makes US/Pacific the host's zone.
const date = new Date(2024, 0, 15, 12);
expect("local hour", date.getHours(), 12);
expect("offset in January", date.getTimezoneOffset(), 480);
expect("offset in July", new Date(2024, 6, 15, 12).getTimezoneOffset(), 420);
expect("local time to UTC", new Date(2024, 0, 15, 12).getTime(), Date.UTC(2024, 0, 15, 20));
expect("a local time that a transition skips", new Date(2024, 2, 10, 2, 30).getTime(), Date.UTC(2024, 2, 10, 10, 30));
expect("name of the zone", date.toString(), "Mon Jan 15 2024 12:00:00 GMT-0800 (Pacific Standard Time)");
expect("after local-time reads", listed(), "false/false");

expect("default zone of Intl", new Intl.DateTimeFormat().resolvedOptions().timeZone, "America/Los_Angeles");
expect("default zone of Temporal", Temporal.Now.timeZoneId(), "America/Los_Angeles");
expect("local hour of Temporal", Temporal.Now.zonedDateTimeISO().offsetNanoseconds % 3600e9, 0);
expect("after the default zone", listed(), "false/false");

check("UTC", "UTC", 0);
check("America/New_York", "America/New_York", -300);
check("Europe/Berlin", "Europe/Berlin", 60);
check("Asia/Tokyo", "Asia/Tokyo", 540);
check("Australia/Sydney", "Australia/Sydney", 660);
check("Etc/GMT+5", "Etc/GMT+5", -300);
expect("transition of a named zone", new Temporal.ZonedDateTime(0n, "America/New_York").getTimeZoneTransition("next").epochMilliseconds, Date.UTC(1970, 3, 26, 7));
expect("after primary identifiers", listed(), "false/false");
if (!$vm.questionsAboutICUTimeZones())
    throw new Error("the zones were named without a question to ICU");
// A zone that the process has named is found without ICU, and so is the host's zone.
expectNoQuestion("zones named before", () => {
    check("UTC", "UTC", 0);
    check("America/New_York", "America/New_York", -300);
    check("Etc/GMT+5", "Etc/GMT+5", -300);
    expect("local hour, again", new Date(2024, 0, 15, 12).getHours(), 12);
    expect("default zone of Temporal, again", Temporal.Now.timeZoneId(), "America/Los_Angeles");
});

// An alias has the canonical identifier of its primary. ICU said what that identifier gives when the primary was named,
// and is asked that once: the alias takes ICU's canonical identifier of it three times and its primary once.
const questionsBeforeAlias = $vm.questionsAboutICUTimeZones();
check("US/Eastern", "US/Eastern", -300);
if ($vm.questionsAboutICUTimeZones() - questionsBeforeAlias > 4)
    throw new Error(`an alias of a zone named before took ${$vm.questionsAboutICUTimeZones() - questionsBeforeAlias} questions to ICU`);
expect("after an alias", listed(), "true/false");
expect("an alias is its primary", new Temporal.ZonedDateTime(0n, "US/Eastern").equals(new Temporal.ZonedDateTime(0n, "America/New_York")), true);
check("america/new_york", "America/New_York", -300);
check("EUROPE/BERLIN", "Europe/Berlin", 60);
check("utc", "UTC", 0);
check("Etc/UTC", "Etc/UTC", 0);
check("est5edt", "EST5EDT", -300);
for (const name of ["Invalid/Zone", "America/New_Yrok", "US:Eastern", "America/New_York\0", "UTC\0x", "SystemV/EST5", "JST", "Etc/Unknown", "GMT+5", ""]) {
    expectRangeError(`Temporal with ${JSON.stringify(name)}`, () => new Temporal.ZonedDateTime(0n, name));
    expectRangeError(`Intl with ${JSON.stringify(name)}`, () => new Intl.DateTimeFormat("en", { timeZone: name }));
}
expect("after other spellings", listed(), "true/false");

// With the list, a zone in another spelling is found without ICU once it has been named, a name that the list does
// not have is never asked about, and a name of the list that is no zone is asked about once.
expectNoQuestion("other spellings of zones named before", () => {
    check("us/eastern", "US/Eastern", -300);
    check("AMERICA/NEW_YORK", "America/New_York", -300);
    check("Est5Edt", "EST5EDT", -300);
});
expectNoQuestion("names that ICU does not list", () => {
    for (const name of ["Invalid/Zone", "America/New_Yrok", "US:Eastern", "America/New_York\0", "Mars/Olympus_Mons", "GMT+5", "", "A".repeat(200)])
        expectRangeError(`Temporal with ${JSON.stringify(name)}, again`, () => new Temporal.ZonedDateTime(0n, name));
});
expectRangeError("a name of ICU that is no zone", () => new Temporal.ZonedDateTime(0n, "MIT"));
expectRangeError("a name of ICU that is no zone", () => new Temporal.ZonedDateTime(0n, "SystemV/PST8"));
expectRangeError("a name of ICU that is no zone", () => new Temporal.ZonedDateTime(0n, "Factory"));
expectNoQuestion("names of ICU that are no zones, again", () => {
    for (const name of ["MIT", "mit", "SystemV/PST8", "systemv/pst8", "Factory", "JST", "jst", "SystemV/EST5"]) {
        for (let times = 0; times < 3; ++times)
            expectRangeError(`Temporal with ${JSON.stringify(name)}, again`, () => new Temporal.ZonedDateTime(0n, name));
    }
});
expect("after names that are no zones", listed(), "true/false");

const zones = Intl.supportedValuesOf("timeZone");
expect("after the list of every zone", listed(), "true/true");
if (zones.length < 400)
    throw new Error(`only ${zones.length} time zones`);
// The list is what upstream's table of every time zone has, which Bun builds for this comparison only. The zones that
// the program named before the list are in it once.
const table = $vm.timeZoneTable();
expect("the primary identifiers", zones.join(), table.filter(identifier => $vm.timeZoneInTable(identifier)[1] === identifier).join());
expect("the list, again", Intl.supportedValuesOf("timeZone").join(), zones.join());

// After the list, every name that ICU has is one that the process has named.
check("Asia/Tokyo", "Asia/Tokyo", 540);
check("America/Chicago", "America/Chicago", -360);
check("us/central", "US/Central", -360);
expectRangeError("a name that is no zone, after the list", () => new Temporal.ZonedDateTime(0n, "Invalid/Zone"));
expectRangeError("a name of ICU that is no zone, after the list", () => new Temporal.ZonedDateTime(0n, "JST"));
expectNoQuestion("every identifier, after the list", () => {
    for (const identifier of table) {
        const [spelling, primary] = $vm.timeZoneInTable(identifier);
        const zoned = new Temporal.ZonedDateTime(0n, identifier.toLowerCase());
        expect(`${identifier}: timeZoneId`, zoned.timeZoneId, spelling);
        expect(`${identifier}: is ${primary}`, zoned.equals(new Temporal.ZonedDateTime(0n, primary)), true);
    }
    for (const name of ["PST", "SystemV/AST4", "Etc/Unknown", "Invalid/Zone"])
        expectRangeError(`Temporal with ${JSON.stringify(name)}, after the list`, () => new Temporal.ZonedDateTime(0n, name));
});
for (const zone of zones)
    expect(`${zone}: resolvedOptions`, new Intl.DateTimeFormat("en", { timeZone: zone }).resolvedOptions().timeZone, zone);
