//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(`${msg}: expected ${expected} but got ${actual}`);
}

function shouldThrow(fn, errorType, msg) {
    try {
        fn();
    } catch (e) {
        if (!(e instanceof errorType))
            throw new Error(`${msg}: expected ${errorType.name} but got ${e}`);
        return;
    }
    throw new Error(`${msg}: expected ${errorType.name} to be thrown`);
}

// Intl.DateTimeFormat reads ICU directly rather than through the Temporal offset cache.
const offsetFormat = new Intl.DateTimeFormat("en-US", { timeZone: "America/New_York", timeZoneName: "longOffset" });
function intlOffsetNanoseconds(epochMilliseconds) {
    const name = offsetFormat.formatToParts(epochMilliseconds).find(part => part.type === "timeZoneName").value;
    const [, sign, hours, minutes, seconds] = /^GMT(?:([+-])(\d\d):(\d\d)(?::(\d\d))?)?$/.exec(name);
    return sign ? (sign === "-" ? -1 : 1) * ((Number(hours) * 60 + Number(minutes)) * 60 + Number(seconds ?? 0)) * 1e9 : 0;
}

// DST gaps and folds with each disambiguation.
// Each row: [input, compatible, earlier, later, reject (null: RangeError)]
const disambiguationCases = [
    ["2021-03-14T02:30[America/New_York]", "2021-03-14T03:30:00-04:00[America/New_York]", "2021-03-14T01:30:00-05:00[America/New_York]", "2021-03-14T03:30:00-04:00[America/New_York]", null],
    ["2021-11-07T01:30[America/New_York]", "2021-11-07T01:30:00-04:00[America/New_York]", "2021-11-07T01:30:00-04:00[America/New_York]", "2021-11-07T01:30:00-05:00[America/New_York]", null],
    ["2021-10-03T02:15[Australia/Lord_Howe]", "2021-10-03T02:45:00+11:00[Australia/Lord_Howe]", "2021-10-03T01:45:00+10:30[Australia/Lord_Howe]", "2021-10-03T02:45:00+11:00[Australia/Lord_Howe]", null],
    ["2021-04-04T01:45[Australia/Lord_Howe]", "2021-04-04T01:45:00+11:00[Australia/Lord_Howe]", "2021-04-04T01:45:00+11:00[Australia/Lord_Howe]", "2021-04-04T01:45:00+10:30[Australia/Lord_Howe]", null],
    ["1986-01-01T00:10[Asia/Kathmandu]", "1986-01-01T00:25:00+05:45[Asia/Kathmandu]", "1985-12-31T23:55:00+05:30[Asia/Kathmandu]", "1986-01-01T00:25:00+05:45[Asia/Kathmandu]", null],
    // A whole day skipped.
    ["2011-12-30T12:00[Pacific/Apia]", "2011-12-31T12:00:00+14:00[Pacific/Apia]", "2011-12-29T12:00:00-10:00[Pacific/Apia]", "2011-12-31T12:00:00+14:00[Pacific/Apia]", null],
    ["2021-03-14T03:00[America/New_York]", "2021-03-14T03:00:00-04:00[America/New_York]", "2021-03-14T03:00:00-04:00[America/New_York]", "2021-03-14T03:00:00-04:00[America/New_York]", "2021-03-14T03:00:00-04:00[America/New_York]"],
];

// Each row: [time zone, epoch nanoseconds of a transition, offset before it, offset from it on]
const transitions = [
    ["America/New_York", 1615705200000000000n, "-05:00", "-04:00"],
    ["Australia/Lord_Howe", 1633188600000000000n, "+10:30", "+11:00"],
];

function checkTransitions() {
    for (const [input, ...expected] of disambiguationCases) {
        ["compatible", "earlier", "later", "reject"].forEach((disambiguation, i) => {
            if (expected[i] === null)
                shouldThrow(() => Temporal.ZonedDateTime.from(input, { disambiguation }), RangeError, `${input} ${disambiguation}`);
            else
                shouldBe(Temporal.ZonedDateTime.from(input, { disambiguation }).toString(), expected[i], `${input} ${disambiguation}`);
        });
    }
    for (const [timeZone, epochNanoseconds, before, after] of transitions) {
        shouldBe(new Temporal.ZonedDateTime(epochNanoseconds - 1n, timeZone).offset, before, `${timeZone} before the transition`);
        shouldBe(new Temporal.ZonedDateTime(epochNanoseconds, timeZone).offset, after, `${timeZone} at the transition`);
        shouldBe(new Temporal.ZonedDateTime(epochNanoseconds + 1n, timeZone).offset, after, `${timeZone} after the transition`);
    }
}

// A zone answers its first 16 lookups from ICU, then caches offset periods and resolves local times from them.
checkTransitions();
for (const timeZone of ["America/New_York", "Australia/Lord_Howe", "Asia/Kathmandu", "Pacific/Apia"]) {
    for (let i = 0; i < 17; i++)
        new Temporal.ZonedDateTime(BigInt(i) * 86400000000000n, timeZone).offsetNanoseconds;
}
checkTransitions();

// New York has more than 256 periods between 1800 and 2250, more than a zone caches, so this sweep evicts. The backward
// sweep and the revisits of evicted dates must give the same results, and the forward sweep agrees with ICU.
function sweep(firstYear, lastYear, step) {
    const results = [];
    for (let year = firstYear; step > 0 ? year <= lastYear : year >= lastYear; year += step) {
        for (const month of step > 0 ? [1, 4, 7, 10] : [10, 7, 4, 1]) {
            const zdt = new Temporal.PlainDateTime(year, month, 15, 12).toZonedDateTime("America/New_York");
            // The 8th of March, June, September and December at 07:00 UTC; in some years that is New York's DST start.
            const instant = Temporal.Instant.fromEpochMilliseconds(Date.UTC(year, month + 1, 8, 7)).toZonedDateTimeISO("America/New_York");
            results.push(`${zdt} ${instant.offsetNanoseconds}`);
            if (step > 0) {
                shouldBe(zdt.offsetNanoseconds, intlOffsetNanoseconds(zdt.epochMilliseconds), `${zdt} offset`);
                shouldBe(instant.offsetNanoseconds, intlOffsetNanoseconds(instant.epochMilliseconds), `${instant} offset`);
            }
        }
    }
    return step > 0 ? results : results.reverse();
}
const forward = sweep(1800, 2250, 1);
sweep(2250, 1800, -1).forEach((result, i) => shouldBe(result, forward[i], `backward sweep ${i}`));
sweep(1800, 1900, 1).forEach((result, i) => shouldBe(result, forward[i], `revisited sweep ${i}`));
shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[America/New_York]").getTimeZoneTransition("next").toString(), "2021-11-07T01:00:00-05:00[America/New_York]", "New York next");
shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[Australia/Lord_Howe]").getTimeZoneTransition("previous").toString(), "2021-04-04T01:30:00+10:30[Australia/Lord_Howe]", "Lord Howe previous");

// UTC-equivalent zones have a zero offset and no transitions anywhere.
for (const timeZone of ["UTC", "Etc/UTC", "Etc/GMT", "GMT", "Etc/GMT-0", "Zulu"]) {
    for (const epochNanoseconds of [-8640000000000000000000n, 0n, 8640000000000000000000n]) {
        const zdt = new Temporal.ZonedDateTime(epochNanoseconds, timeZone);
        shouldBe(zdt.offsetNanoseconds, 0, `${timeZone} offset`);
        shouldBe(zdt.getTimeZoneTransition("next"), null, `${timeZone} next`);
        shouldBe(zdt.getTimeZoneTransition("previous"), null, `${timeZone} previous`);
    }
    shouldBe(Temporal.ZonedDateTime.from(`2021-03-14T02:30[${timeZone}]`, { disambiguation: "reject" }).epochNanoseconds, 1615689000000000000n, `${timeZone} local time`);
}
