//@ skip if $hostOS == "playstation"

// The local Date setters, the multi-argument Date constructor and Date.parse of a string without a UTC offset
// compute TimeClip(UTC(local time value)). For a Date within one UTC offset of either end of the time value range
// that local time value is itself outside the range, and UTC() has to use the zone's offset at that date for it
// (https://tc39.es/ecma262/#sec-utc-t, "The algorithm must not limit t to the time value range").

function shouldBe(actual, expected, message) {
    if (!Object.is(actual, expected))
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

const minTimeValue = -8.64e15; // -271821-04-20T00:00:00.000Z
const maxTimeValue = 8.64e15; // +275760-09-13T00:00:00.000Z
const msPerHour = 3600 * 1000;
const msPerDay = 24 * msPerHour;

const timeValuesNearTheLimits = [
    minTimeValue,
    minTimeValue + 1,
    minTimeValue + msPerHour,
    minTimeValue + msPerDay - 1,
    minTimeValue + msPerDay,
    maxTimeValue - msPerDay,
    maxTimeValue - msPerDay + 1,
    maxTimeValue - 5 * msPerHour,
    maxTimeValue - 1,
    maxTimeValue,
];

// Rebuilding a valid Date from its own local fields, and setting a local field to the value it already has,
// are identity operations on the time value.
function testRoundTrips(timeZone) {
    for (const timeValue of timeValuesNearTheLimits) {
        const date = new Date(timeValue);
        const fields = [date.getFullYear(), date.getMonth(), date.getDate(), date.getHours(), date.getMinutes(), date.getSeconds(), date.getMilliseconds()];
        shouldBe(new Date(...fields).getTime(), timeValue, `${timeZone}: new Date(${fields})`);

        shouldBe(new Date(timeValue).setMilliseconds(date.getMilliseconds()), timeValue, `${timeZone}: new Date(${timeValue}).setMilliseconds(${date.getMilliseconds()})`);
        shouldBe(new Date(timeValue).setSeconds(date.getSeconds()), timeValue, `${timeZone}: new Date(${timeValue}).setSeconds(${date.getSeconds()})`);
        shouldBe(new Date(timeValue).setMinutes(date.getMinutes()), timeValue, `${timeZone}: new Date(${timeValue}).setMinutes(${date.getMinutes()})`);
        shouldBe(new Date(timeValue).setHours(date.getHours()), timeValue, `${timeZone}: new Date(${timeValue}).setHours(${date.getHours()})`);
        shouldBe(new Date(timeValue).setDate(date.getDate()), timeValue, `${timeZone}: new Date(${timeValue}).setDate(${date.getDate()})`);
        shouldBe(new Date(timeValue).setMonth(date.getMonth()), timeValue, `${timeZone}: new Date(${timeValue}).setMonth(${date.getMonth()})`);
        shouldBe(new Date(timeValue).setFullYear(date.getFullYear()), timeValue, `${timeZone}: new Date(${timeValue}).setFullYear(${date.getFullYear()})`);
        shouldBe(new Date(timeValue).setFullYear(...fields.slice(0, 3)), timeValue, `${timeZone}: new Date(${timeValue}).setFullYear(${fields.slice(0, 3)})`);
        shouldBe(new Date(timeValue).setHours(...fields.slice(3)), timeValue, `${timeZone}: new Date(${timeValue}).setHours(${fields.slice(3)})`);

        // One millisecond further out is not a time value any more.
        if (timeValue === minTimeValue) {
            shouldBe(new Date(timeValue).setMilliseconds(date.getMilliseconds() - 1), NaN, `${timeZone}: new Date(${timeValue}).setMilliseconds(${date.getMilliseconds() - 1})`);
            fields[6] -= 1;
            shouldBe(new Date(...fields).getTime(), NaN, `${timeZone}: new Date(${fields})`);
        }
        if (timeValue === maxTimeValue) {
            shouldBe(new Date(timeValue).setMilliseconds(date.getMilliseconds() + 1), NaN, `${timeZone}: new Date(${timeValue}).setMilliseconds(${date.getMilliseconds() + 1})`);
            fields[6] += 1;
            shouldBe(new Date(...fields).getTime(), NaN, `${timeZone}: new Date(${fields})`);
        }
    }

    // A local time more than a day beyond either end is NaN whatever the offset is.
    shouldBe(new Date(275760, 8, 14, 0, 0, 0, 1).getTime(), NaN, `${timeZone}: new Date(275760, 8, 14, 0, 0, 0, 1)`);
    shouldBe(new Date(-271821, 3, 18, 23, 59, 59, 999).getTime(), NaN, `${timeZone}: new Date(-271821, 3, 18, 23, 59, 59, 999)`);
    shouldBe(new Date(1970, 0, 1, 0, 0, 0, 1e300).getTime(), NaN, `${timeZone}: new Date(1970, 0, 1, 0, 0, 0, 1e300)`);
    shouldBe(new Date(1970, 0, 1, 0, 0, 0, -1e300).getTime(), NaN, `${timeZone}: new Date(1970, 0, 1, 0, 0, 0, -1e300)`);
    shouldBe(new Date(0).setMilliseconds(2 ** 70), NaN, `${timeZone}: new Date(0).setMilliseconds(2 ** 70)`);

    // With an explicit UTC offset in the string there is no local time involved, only TimeClip of the result.
    shouldBe(Date.parse("-271821-04-19T20:00:00-04:00"), minTimeValue, `${timeZone}: Date.parse("-271821-04-19T20:00:00-04:00")`);
    shouldBe(Date.parse("+275760-09-13T05:00:00+05:00"), maxTimeValue, `${timeZone}: Date.parse("+275760-09-13T05:00:00+05:00")`);
    shouldBe(Date.parse("+275760-09-13T05:00:00.001+05:00"), NaN, `${timeZone}: Date.parse("+275760-09-13T05:00:00.001+05:00")`);
}

// America/New_York is at its local mean time, -4:56:02, before 1883-11-18, so the minimum time value reads as
// -271821-04-19T19:03:58 local time there, and that local time is below the time value range.
function testWestAtTheMinimum(timeZone) {
    shouldBe(new Date(minTimeValue).getTimezoneOffset(), 296, `${timeZone}: new Date(minTimeValue).getTimezoneOffset()`);
    shouldBe(new Date(-271821, 3, 19, 19, 3, 58).getTime(), minTimeValue, `${timeZone}: new Date(-271821, 3, 19, 19, 3, 58)`);
    shouldBe(new Date(-271821, 3, 19, 19, 3, 57, 999).getTime(), NaN, `${timeZone}: new Date(-271821, 3, 19, 19, 3, 57, 999)`);
    shouldBe(new Date(-271821, 3, 19, 20, 3, 58).getTime(), minTimeValue + msPerHour, `${timeZone}: new Date(-271821, 3, 19, 20, 3, 58)`);
    shouldBe(new Date(minTimeValue + msPerHour).setHours(19), minTimeValue, `${timeZone}: new Date(minTimeValue + msPerHour).setHours(19)`);
    shouldBe(new Date(minTimeValue + msPerHour).setHours(19, 3, 57, 999), NaN, `${timeZone}: new Date(minTimeValue + msPerHour).setHours(19, 3, 57, 999)`);
    shouldBe(Date.parse("-271821-04-19T19:03:58"), minTimeValue, `${timeZone}: Date.parse("-271821-04-19T19:03:58")`);
    shouldBe(Date.parse("-271821-04-19T19:03:57.999"), NaN, `${timeZone}: Date.parse("-271821-04-19T19:03:57.999")`);
}

// East of Greenwich the maximum time value has a local time past +275760-09-13T00:00:00.
function testEastAtTheMaximum(timeZone) {
    const offset = -new Date(maxTimeValue).getTimezoneOffset() * 60 * 1000;
    if (!(offset > 0))
        throw new Error(`${timeZone}: expected a positive UTC offset at the maximum time value but got ${offset}`);
    shouldBe(new Date(275760, 8, 13, 0, 0, 0, offset).getTime(), maxTimeValue, `${timeZone}: new Date(275760, 8, 13, 0, 0, 0, ${offset})`);
    shouldBe(new Date(275760, 8, 13, 0, 0, 0, offset + 1).getTime(), NaN, `${timeZone}: new Date(275760, 8, 13, 0, 0, 0, ${offset + 1})`);
    shouldBe(new Date(275760, 8, 13, 0, 0, 0, offset - 1).getTime(), maxTimeValue - 1, `${timeZone}: new Date(275760, 8, 13, 0, 0, 0, ${offset - 1})`);
    shouldBe(new Date(maxTimeValue - 5 * msPerHour).setHours(offset / msPerHour), maxTimeValue, `${timeZone}: new Date(maxTimeValue - 5 * msPerHour).setHours(${offset / msPerHour})`);
    shouldBe(new Date(maxTimeValue - 5 * msPerHour).setHours(offset / msPerHour, 0, 0, 1), NaN, `${timeZone}: new Date(maxTimeValue - 5 * msPerHour).setHours(${offset / msPerHour}, 0, 0, 1)`);
}

// The zones to test, each with its getTimezoneOffset() at the epoch to tell that the zone change took effect.
// A zone west of Greenwich at its first (local mean time) offset has the local time of the minimum time value below
// the range, a zone east of it today has the local time of the maximum above it. Asia/Manila (-15:56:08 until 1844,
// +8 today) has both.
const timeZones = {
    "America/New_York": 300,
    "America/Los_Angeles": 480,
    "America/St_Johns": 210,
    "Pacific/Honolulu": 600,
    "Europe/London": -60,
    "Europe/Paris": -60,
    "Asia/Kolkata": -330,
    "Asia/Tokyo": -540,
    "Australia/Lord_Howe": -600,
    "Pacific/Kiritimati": 640,
    "Pacific/Apia": 660,
    "Asia/Manila": -480,
    "UTC": 0,
};

const steps = [];
for (const timeZone in timeZones)
    steps.push([timeZone, testRoundTrips]);
steps.push(["America/New_York", testWestAtTheMinimum]);
for (const timeZone of ["Asia/Tokyo", "Pacific/Apia", "Pacific/Kiritimati"])
    steps.push([timeZone, testEastAtTheMaximum]);

function runNextStep() {
    if (!steps.length)
        return;
    const [timeZone, test] = steps.shift();
    if (!$vm.setHostTimeZone(timeZone))
        throw new Error(`failed to set the host time zone to ${timeZone}`);
    // A zone change is only picked up on VM entry, so run the checks from a fresh turn.
    setTimeout(() => {
        shouldBe(new Date(0).getTimezoneOffset(), timeZones[timeZone], `${timeZone}: new Date(0).getTimezoneOffset()`);
        test(timeZone);
        runNextStep();
    }, 0);
}
runNextStep();
