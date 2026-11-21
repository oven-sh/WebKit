//@ skip if $hostOS == "playstation"
//@ runDefault

// getTimeZoneTransition() gives the next or the previous change of a zone's UTC offset. ICU also has a transition where
// only the rule or the abbreviation of a zone changes, and the search goes on past each of those
// (getTimeZoneTransition in TimeZoneICUBridge.cpp).

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

function expectOffsetChange(label, transition)
{
    if (transition === null)
        throw new Error(`${label}: no transition`);
    if (transition.offsetNanoseconds === transition.subtract({ nanoseconds: 1 }).offsetNanoseconds)
        throw new Error(`${label}: the offset is the same before ${transition}`);
}

// The four cases of test262's intl402/Temporal/ZonedDateTime/prototype/getTimeZoneTransition/
// rule-change-without-offset-transition.js. Europe/London went from summer time to British Standard Time on 1968-10-27
// and kept its offset. America/Anchorage changed its abbreviation on 1967-04-01 and kept its offset.
const londonPrevious = new Temporal.ZonedDateTime(0n, "Europe/London").getTimeZoneTransition("previous");
expectOffsetChange("London, previous", londonPrevious);
expect("London, previous", londonPrevious.epochNanoseconds, -59004000000000000n); // 1968-02-18T03:00:00+01:00

const londonNext = new Temporal.ZonedDateTime(-39488400000000000n, "Europe/London").getTimeZoneTransition("next"); // from 1968-10-01
expectOffsetChange("London, next", londonNext);
expect("London, next", londonNext.epochNanoseconds, 57722400000000000n); // 1971-10-31T02:00:00+00:00

const anchoragePrevious = new Temporal.ZonedDateTime(-84290400000000000n, "America/Anchorage").getTimeZoneTransition("previous"); // from 1967-05-01
expectOffsetChange("Anchorage, previous", anchoragePrevious);
expect("Anchorage, previous", anchoragePrevious.epochNanoseconds, -765378000000000000n); // 1945-09-30T01:00:00-10:00

const anchorageNext = new Temporal.ZonedDateTime(-94658400000000000n, "America/Anchorage").getTimeZoneTransition("next"); // from 1967-01-01
expectOffsetChange("Anchorage, next", anchorageNext);
expect("Anchorage, next", anchorageNext.epochNanoseconds, -21470400000000000n); // 1969-04-27T03:00:00-09:00

// A zone without a change has no transition, and neither has an offset.
for (const zone of ["UTC", "Etc/GMT+5", "+05:30"]) {
    const zoned = new Temporal.ZonedDateTime(0n, zone);
    expect(`${zone}, next`, zoned.getTimeZoneTransition("next"), null);
    expect(`${zone}, previous`, zoned.getTimeZoneTransition("previous"), null);
}

// From 1900 to 2040 and back: the search forward and the search backward find the same transitions, each of them a
// change of the offset, and an instant that is a transition is not its own next or previous one.
const start = BigInt(Date.UTC(1900, 0, 1)) * 1000000n;
const end = BigInt(Date.UTC(2040, 0, 1)) * 1000000n;
const fewest = {
    "America/New_York": 200,
    "America/Anchorage": 100,
    "Europe/London": 200,
    "Europe/Dublin": 200,
    "Africa/Casablanca": 50,
    "Australia/Lord_Howe": 100,
    "Pacific/Apia": 10,
    "Asia/Kolkata": 3,
    "US/Pacific": 150,
};
for (const [zone, count] of Object.entries(fewest)) {
    const forward = [];
    for (let zoned = new Temporal.ZonedDateTime(start, zone); ;) {
        zoned = zoned.getTimeZoneTransition("next");
        if (zoned === null || zoned.epochNanoseconds >= end)
            break;
        expectOffsetChange(`${zone}, forward`, zoned);
        if (forward.length && zoned.epochNanoseconds <= forward[forward.length - 1])
            throw new Error(`${zone}: the transition after ${forward[forward.length - 1]} is ${zoned.epochNanoseconds}`);
        forward.push(zoned.epochNanoseconds);
    }
    const backward = [];
    for (let zoned = new Temporal.ZonedDateTime(end, zone); ;) {
        zoned = zoned.getTimeZoneTransition("previous");
        if (zoned === null || zoned.epochNanoseconds <= start)
            break;
        expectOffsetChange(`${zone}, backward`, zoned);
        backward.push(zoned.epochNanoseconds);
    }
    expect(`${zone}: transitions backward`, backward.reverse().join(), forward.join());
    if (forward.length < count)
        throw new Error(`${zone}: only ${forward.length} transitions`);
}

// Where the rule of a zone has no end, so have its transitions.
const later = new Temporal.ZonedDateTime(BigInt(Date.UTC(2300, 0, 1)) * 1000000n, "America/New_York");
expect("New York in 2300, next", later.getTimeZoneTransition("next").toString(), "2300-03-11T03:00:00-04:00[America/New_York]");
expect("New York in 2300, previous", later.getTimeZoneTransition("previous").toString(), "2299-11-05T01:00:00-05:00[America/New_York]");
// Before the first one there is none.
expect("Kolkata in 1800, previous", new Temporal.ZonedDateTime(BigInt(Date.UTC(1800, 0, 1)) * 1000000n, "Asia/Kolkata").getTimeZoneTransition("previous"), null);
