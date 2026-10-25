//@ requireOptions("--useTemporal=1")
// Time zone offset lookups must stay exact while a per-zone cache of offset periods is filled, evicted and revisited:
// long sweeps that cross every transition of a zone in their range, DST gaps and folds with every disambiguation on
// both sides of the transition, rotation through more than 16 zones, and UTC-equivalent zones. Offsets that depend on
// the tzdata version are checked against Intl.DateTimeFormat, which reads ICU directly rather than through the Temporal
// cache.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(`${msg}: expected ${expected} but got ${actual}`);
}

function shouldThrow(fn, errorType, msg) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof errorType))
            throw new Error(`${msg}: expected ${errorType.name} but got ${e}`);
    }
    if (!threw)
        throw new Error(`${msg}: expected ${errorType.name} to be thrown`);
}

const offsetFormats = new Map();
function intlOffsetNanoseconds(timeZone, epochNanoseconds) {
    if (!offsetFormats.has(timeZone))
        offsetFormats.set(timeZone, new Intl.DateTimeFormat("en-US", { timeZone, timeZoneName: "longOffset" }));
    const epochMilliseconds = Number(epochNanoseconds / 1000000n) - (epochNanoseconds % 1000000n < 0n ? 1 : 0);
    const name = offsetFormats.get(timeZone).formatToParts(epochMilliseconds).find(part => part.type === "timeZoneName").value;
    const [, sign, hours, minutes, seconds] = /^GMT(?:([+-])(\d\d):(\d\d)(?::(\d\d))?)?$/.exec(name);
    if (!sign)
        return 0;
    return (sign === "-" ? -1 : 1) * ((Number(hours) * 60 + Number(minutes)) * 60 + Number(seconds ?? 0)) * 1e9;
}

// The local time and the offset are consistent with each other, with the epoch nanoseconds, and with ICU.
function checkZonedDateTime(zdt, plainDateTime, msg) {
    shouldBe(zdt.toPlainDateTime().toString(), plainDateTime.toString(), `${msg} local time`);
    shouldBe(zdt.offsetNanoseconds, intlOffsetNanoseconds(zdt.timeZoneId, zdt.epochNanoseconds), `${msg} offset`);
}

function offsetsFromPlainDateTimes(timeZone, firstYear, lastYear, step, check) {
    const offsets = [];
    for (let year = firstYear; step > 0 ? year <= lastYear : year >= lastYear; year += step) {
        for (const month of step > 0 ? [1, 4, 7, 10] : [10, 7, 4, 1]) {
            const plainDateTime = new Temporal.PlainDateTime(year, month, 15, 12);
            const zdt = plainDateTime.toZonedDateTime(timeZone);
            if (check)
                checkZonedDateTime(zdt, plainDateTime, `${plainDateTime} in ${timeZone}`);
            offsets.push(`${zdt.epochNanoseconds} ${zdt.offsetNanoseconds}`);
        }
    }
    return step > 0 ? offsets : offsets.reverse();
}
noInline(offsetsFromPlainDateTimes);

function offsetsFromInstants(timeZone, firstYear, lastYear, check) {
    const offsets = [];
    for (let year = firstYear; year <= lastYear; year++) {
        for (const month of [3, 11]) {
            const epochNanoseconds = BigInt(Date.UTC(year, month - 1, 8, 7)) * 1000000n;
            const offsetNanoseconds = new Temporal.Instant(epochNanoseconds).toZonedDateTimeISO(timeZone).offsetNanoseconds;
            if (check)
                shouldBe(offsetNanoseconds, intlOffsetNanoseconds(timeZone, epochNanoseconds), `${epochNanoseconds} in ${timeZone} offset`);
            offsets.push(offsetNanoseconds);
        }
    }
    return offsets;
}
noInline(offsetsFromInstants);

// Each sweep crosses every transition of its zone in the range. A zone caches at most 256 periods and New York's past
// has about 210, so only its sweep goes on to 2250, to force evictions.
const sweeps = [
    ["America/New_York", 1800, 2250],
    ["Australia/Lord_Howe", 1800, 2020],
    ["Asia/Kathmandu", 1800, 2020],
];

// Dates revisited between sweeps; each one is checked against ICU and against its first result.
const revisits = [];
for (const timeZone of ["America/New_York", "Australia/Lord_Howe", "Asia/Kathmandu"]) {
    for (const input of ["1801-01-15T12:00", "1883-11-18T11:00", "1918-03-31T03:30", "1945-08-14T19:00", "1969-07-20T20:17", "2000-02-29T23:59:59.999999999", "2020-12-31T12:00"])
        revisits.push([timeZone, input]);
}
const firstRevisitResults = new Map();

// Long-settled offsets. Columns: time zone, local time, toString(), epochNanoseconds.
const settled = [
    ["America/New_York", "1918-03-31T03:30", "1918-03-31T03:30:00-04:00[America/New_York]", -1633278600000000000n],
    ["America/New_York", "1945-08-14T19:00", "1945-08-14T19:00:00-04:00[America/New_York]", -769395600000000000n],
    ["America/New_York", "2000-02-29T23:59:59.999999999", "2000-02-29T23:59:59.999999999-05:00[America/New_York]", 951886799999999999n],
    ["Australia/Lord_Howe", "1969-07-20T20:17", "1969-07-20T20:17:00+10:00[Australia/Lord_Howe]", -14218980000000000n],
    ["Australia/Lord_Howe", "2000-02-29T23:59:59.999999999", "2000-02-29T23:59:59.999999999+11:00[Australia/Lord_Howe]", 951829199999999999n],
    ["Asia/Kathmandu", "1945-08-14T19:00", "1945-08-14T19:00:00+05:30[Asia/Kathmandu]", -769429800000000000n],
    ["Asia/Kathmandu", "2000-02-29T23:59:59.999999999", "2000-02-29T23:59:59.999999999+05:45[Asia/Kathmandu]", 951848099999999999n],
];

function checkRevisits() {
    for (const [timeZone, input] of revisits) {
        const plainDateTime = Temporal.PlainDateTime.from(input);
        const zdt = plainDateTime.toZonedDateTime(timeZone);
        const back = new Temporal.ZonedDateTime(zdt.epochNanoseconds, timeZone);
        const result = `${zdt} ${zdt.epochNanoseconds} ${zdt.offset} ${back}`;
        const key = `${input} in ${timeZone}`;
        if (firstRevisitResults.has(key)) {
            shouldBe(result, firstRevisitResults.get(key), key);
            continue;
        }
        checkZonedDateTime(zdt, plainDateTime, key);
        shouldBe(back.toString(), zdt.toString(), `${key} from epochNanoseconds`);
        firstRevisitResults.set(key, result);
    }
    for (const [timeZone, input, string, epochNanoseconds] of settled) {
        const zdt = Temporal.PlainDateTime.from(input).toZonedDateTime(timeZone);
        shouldBe(zdt.toString(), string, `${input} in ${timeZone}`);
        shouldBe(zdt.epochNanoseconds, epochNanoseconds, `${input} in ${timeZone} epochNanoseconds`);
        shouldBe(new Temporal.ZonedDateTime(epochNanoseconds, timeZone).toString(), string, `${input} in ${timeZone} from epochNanoseconds`);
    }
}

// Forward sweep, backward sweep and an Instant-based sweep, then revisits of dates that may have been evicted. The
// first round's sweeps are checked against ICU; every later sweep over the same range must give the same results.
const firstSweepResults = new Map();
for (let round = 0; round < 3; round++) {
    for (const [timeZone, firstYear, lastYear] of sweeps) {
        const forward = offsetsFromPlainDateTimes(timeZone, firstYear, lastYear, 1, !round).join();
        const instants = offsetsFromInstants(timeZone, firstYear, lastYear, !round).join();
        if (!round)
            firstSweepResults.set(timeZone, { forward, instants });
        const first = firstSweepResults.get(timeZone);
        shouldBe(forward, first.forward, `${timeZone} forward sweep`);
        checkRevisits();
        shouldBe(offsetsFromPlainDateTimes(timeZone, lastYear, firstYear, -1, false).join(), first.forward, `${timeZone} backward sweep`);
        checkRevisits();
        shouldBe(instants, first.instants, `${timeZone} instant sweep`);
        shouldBe(offsetsFromInstants(timeZone, firstYear, lastYear, false).join(), first.instants, `${timeZone} repeated instant sweep`);
        checkRevisits();
    }
}

// DST gaps and folds with each disambiguation, just before, inside and just after each transition.
// Columns: compatible, earlier, later, reject (null = RangeError).
const disambiguationCases = [
    ["America/New_York", "2021-03-14T01:59:59.999999999", "2021-03-14T01:59:59.999999999-05:00[America/New_York] 1615705199999999999", "2021-03-14T01:59:59.999999999-05:00[America/New_York] 1615705199999999999", "2021-03-14T01:59:59.999999999-05:00[America/New_York] 1615705199999999999", "2021-03-14T01:59:59.999999999-05:00[America/New_York] 1615705199999999999"],
    ["America/New_York", "2021-03-14T02:00", "2021-03-14T03:00:00-04:00[America/New_York] 1615705200000000000", "2021-03-14T01:00:00-05:00[America/New_York] 1615701600000000000", "2021-03-14T03:00:00-04:00[America/New_York] 1615705200000000000", null],
    ["America/New_York", "2021-03-14T02:30", "2021-03-14T03:30:00-04:00[America/New_York] 1615707000000000000", "2021-03-14T01:30:00-05:00[America/New_York] 1615703400000000000", "2021-03-14T03:30:00-04:00[America/New_York] 1615707000000000000", null],
    ["America/New_York", "2021-03-14T03:00", "2021-03-14T03:00:00-04:00[America/New_York] 1615705200000000000", "2021-03-14T03:00:00-04:00[America/New_York] 1615705200000000000", "2021-03-14T03:00:00-04:00[America/New_York] 1615705200000000000", "2021-03-14T03:00:00-04:00[America/New_York] 1615705200000000000"],
    ["America/New_York", "2021-11-07T00:59:59.999999999", "2021-11-07T00:59:59.999999999-04:00[America/New_York] 1636261199999999999", "2021-11-07T00:59:59.999999999-04:00[America/New_York] 1636261199999999999", "2021-11-07T00:59:59.999999999-04:00[America/New_York] 1636261199999999999", "2021-11-07T00:59:59.999999999-04:00[America/New_York] 1636261199999999999"],
    ["America/New_York", "2021-11-07T01:00", "2021-11-07T01:00:00-04:00[America/New_York] 1636261200000000000", "2021-11-07T01:00:00-04:00[America/New_York] 1636261200000000000", "2021-11-07T01:00:00-05:00[America/New_York] 1636264800000000000", null],
    ["America/New_York", "2021-11-07T01:30", "2021-11-07T01:30:00-04:00[America/New_York] 1636263000000000000", "2021-11-07T01:30:00-04:00[America/New_York] 1636263000000000000", "2021-11-07T01:30:00-05:00[America/New_York] 1636266600000000000", null],
    ["America/New_York", "2021-11-07T02:00", "2021-11-07T02:00:00-05:00[America/New_York] 1636268400000000000", "2021-11-07T02:00:00-05:00[America/New_York] 1636268400000000000", "2021-11-07T02:00:00-05:00[America/New_York] 1636268400000000000", "2021-11-07T02:00:00-05:00[America/New_York] 1636268400000000000"],
    ["Australia/Lord_Howe", "2021-10-03T01:59:59.999999999", "2021-10-03T01:59:59.999999999+10:30[Australia/Lord_Howe] 1633188599999999999", "2021-10-03T01:59:59.999999999+10:30[Australia/Lord_Howe] 1633188599999999999", "2021-10-03T01:59:59.999999999+10:30[Australia/Lord_Howe] 1633188599999999999", "2021-10-03T01:59:59.999999999+10:30[Australia/Lord_Howe] 1633188599999999999"],
    ["Australia/Lord_Howe", "2021-10-03T02:00", "2021-10-03T02:30:00+11:00[Australia/Lord_Howe] 1633188600000000000", "2021-10-03T01:30:00+10:30[Australia/Lord_Howe] 1633186800000000000", "2021-10-03T02:30:00+11:00[Australia/Lord_Howe] 1633188600000000000", null],
    ["Australia/Lord_Howe", "2021-10-03T02:15", "2021-10-03T02:45:00+11:00[Australia/Lord_Howe] 1633189500000000000", "2021-10-03T01:45:00+10:30[Australia/Lord_Howe] 1633187700000000000", "2021-10-03T02:45:00+11:00[Australia/Lord_Howe] 1633189500000000000", null],
    ["Australia/Lord_Howe", "2021-10-03T02:30", "2021-10-03T02:30:00+11:00[Australia/Lord_Howe] 1633188600000000000", "2021-10-03T02:30:00+11:00[Australia/Lord_Howe] 1633188600000000000", "2021-10-03T02:30:00+11:00[Australia/Lord_Howe] 1633188600000000000", "2021-10-03T02:30:00+11:00[Australia/Lord_Howe] 1633188600000000000"],
    ["Australia/Lord_Howe", "2021-04-04T01:29:59.999999999", "2021-04-04T01:29:59.999999999+11:00[Australia/Lord_Howe] 1617460199999999999", "2021-04-04T01:29:59.999999999+11:00[Australia/Lord_Howe] 1617460199999999999", "2021-04-04T01:29:59.999999999+11:00[Australia/Lord_Howe] 1617460199999999999", "2021-04-04T01:29:59.999999999+11:00[Australia/Lord_Howe] 1617460199999999999"],
    ["Australia/Lord_Howe", "2021-04-04T01:30", "2021-04-04T01:30:00+11:00[Australia/Lord_Howe] 1617460200000000000", "2021-04-04T01:30:00+11:00[Australia/Lord_Howe] 1617460200000000000", "2021-04-04T01:30:00+10:30[Australia/Lord_Howe] 1617462000000000000", null],
    ["Australia/Lord_Howe", "2021-04-04T01:45", "2021-04-04T01:45:00+11:00[Australia/Lord_Howe] 1617461100000000000", "2021-04-04T01:45:00+11:00[Australia/Lord_Howe] 1617461100000000000", "2021-04-04T01:45:00+10:30[Australia/Lord_Howe] 1617462900000000000", null],
    ["Australia/Lord_Howe", "2021-04-04T02:00", "2021-04-04T02:00:00+10:30[Australia/Lord_Howe] 1617463800000000000", "2021-04-04T02:00:00+10:30[Australia/Lord_Howe] 1617463800000000000", "2021-04-04T02:00:00+10:30[Australia/Lord_Howe] 1617463800000000000", "2021-04-04T02:00:00+10:30[Australia/Lord_Howe] 1617463800000000000"],
    ["Asia/Kathmandu", "1985-12-31T23:59:59.999999999", "1985-12-31T23:59:59.999999999+05:30[Asia/Kathmandu] 504901799999999999", "1985-12-31T23:59:59.999999999+05:30[Asia/Kathmandu] 504901799999999999", "1985-12-31T23:59:59.999999999+05:30[Asia/Kathmandu] 504901799999999999", "1985-12-31T23:59:59.999999999+05:30[Asia/Kathmandu] 504901799999999999"],
    ["Asia/Kathmandu", "1986-01-01T00:00", "1986-01-01T00:15:00+05:45[Asia/Kathmandu] 504901800000000000", "1985-12-31T23:45:00+05:30[Asia/Kathmandu] 504900900000000000", "1986-01-01T00:15:00+05:45[Asia/Kathmandu] 504901800000000000", null],
    ["Asia/Kathmandu", "1986-01-01T00:10", "1986-01-01T00:25:00+05:45[Asia/Kathmandu] 504902400000000000", "1985-12-31T23:55:00+05:30[Asia/Kathmandu] 504901500000000000", "1986-01-01T00:25:00+05:45[Asia/Kathmandu] 504902400000000000", null],
    ["Asia/Kathmandu", "1986-01-01T00:15", "1986-01-01T00:15:00+05:45[Asia/Kathmandu] 504901800000000000", "1986-01-01T00:15:00+05:45[Asia/Kathmandu] 504901800000000000", "1986-01-01T00:15:00+05:45[Asia/Kathmandu] 504901800000000000", "1986-01-01T00:15:00+05:45[Asia/Kathmandu] 504901800000000000"],
    ["Europe/London", "2021-03-28T01:30", "2021-03-28T02:30:00+01:00[Europe/London] 1616895000000000000", "2021-03-28T00:30:00+00:00[Europe/London] 1616891400000000000", "2021-03-28T02:30:00+01:00[Europe/London] 1616895000000000000", null],
    ["Europe/London", "2021-10-31T01:30", "2021-10-31T01:30:00+01:00[Europe/London] 1635640200000000000", "2021-10-31T01:30:00+01:00[Europe/London] 1635640200000000000", "2021-10-31T01:30:00+00:00[Europe/London] 1635643800000000000", null],
];

const disambiguations = ["compatible", "earlier", "later", "reject"];
function resolve(input, timeZone, disambiguation) {
    const zdt = Temporal.ZonedDateTime.from(`${input}[${timeZone}]`, { disambiguation });
    return `${zdt.toString()} ${zdt.epochNanoseconds}`;
}
noInline(resolve);

for (let round = 0; round < 3; round++) {
    // Evict cached periods between rounds.
    const [evictionFirstYear, evictionLastYear, evictionStep] = round === 1 ? [2250, 1800, -1] : [1800, 2250, 1];
    offsetsFromPlainDateTimes("America/New_York", evictionFirstYear, evictionLastYear, evictionStep, false);
    for (const [timeZone, input, ...expected] of disambiguationCases) {
        for (let i = 0; i < disambiguations.length; i++) {
            const disambiguation = disambiguations[i];
            if (expected[i] === null)
                shouldThrow(() => resolve(input, timeZone, disambiguation), RangeError, `${input} ${timeZone} ${disambiguation}`);
            else
                shouldBe(resolve(input, timeZone, disambiguation), expected[i], `${input} ${timeZone} ${disambiguation}`);
            const plain = Temporal.PlainDateTime.from(input);
            if (expected[i] === null)
                shouldThrow(() => plain.toZonedDateTime(timeZone, { disambiguation }), RangeError, `toZonedDateTime ${input} ${timeZone} ${disambiguation}`);
            else
                shouldBe(`${plain.toZonedDateTime(timeZone, { disambiguation })} ${plain.toZonedDateTime(timeZone, { disambiguation }).epochNanoseconds}`, expected[i], `toZonedDateTime ${input} ${timeZone} ${disambiguation}`);
        }
    }
    shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[America/New_York]").getTimeZoneTransition("next").toString(), "2021-11-07T01:00:00-05:00[America/New_York]", "New York next");
    shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[America/New_York]").getTimeZoneTransition("previous").toString(), "2021-03-14T03:00:00-04:00[America/New_York]", "New York previous");
    shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[Australia/Lord_Howe]").getTimeZoneTransition("next").toString(), "2021-10-03T02:30:00+11:00[Australia/Lord_Howe]", "Lord Howe next");
    shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[Australia/Lord_Howe]").getTimeZoneTransition("previous").toString(), "2021-04-04T01:30:00+10:30[Australia/Lord_Howe]", "Lord Howe previous");
    shouldBe(Temporal.ZonedDateTime.from("2021-06-01T00:00[Asia/Kathmandu]").getTimeZoneTransition("previous").toString(), "1986-01-01T00:15:00+05:45[Asia/Kathmandu]", "Kathmandu previous");
}

// Rotate through more than 16 zones repeatedly. Columns: January offset, July offset, previous transition from
// 2021-01-15T12:00 local.
const rotation = [
    ["America/New_York", "-05:00", "-04:00", "2020-11-01T01:00:00-05:00[America/New_York]"],
    ["America/Chicago", "-06:00", "-05:00", "2020-11-01T01:00:00-06:00[America/Chicago]"],
    ["America/Denver", "-07:00", "-06:00", "2020-11-01T01:00:00-07:00[America/Denver]"],
    ["America/Los_Angeles", "-08:00", "-07:00", "2020-11-01T01:00:00-08:00[America/Los_Angeles]"],
    ["America/Sao_Paulo", "-03:00", "-03:00", "2019-02-16T23:00:00-03:00[America/Sao_Paulo]"],
    ["America/St_Johns", "-03:30", "-02:30", "2020-11-01T01:00:00-03:30[America/St_Johns]"],
    ["Europe/London", "+00:00", "+01:00", "2020-10-25T01:00:00+00:00[Europe/London]"],
    ["Europe/Paris", "+01:00", "+02:00", "2020-10-25T02:00:00+01:00[Europe/Paris]"],
    ["Europe/Moscow", "+03:00", "+03:00", "2014-10-26T01:00:00+03:00[Europe/Moscow]"],
    ["Asia/Tehran", "+03:30", "+04:30", "2020-09-20T23:00:00+03:30[Asia/Tehran]"],
    ["Asia/Kolkata", "+05:30", "+05:30", "1945-10-14T23:00:00+05:30[Asia/Kolkata]"],
    ["Asia/Kathmandu", "+05:45", "+05:45", "1986-01-01T00:15:00+05:45[Asia/Kathmandu]"],
    ["Asia/Shanghai", "+08:00", "+08:00", "1991-09-15T01:00:00+08:00[Asia/Shanghai]"],
    ["Asia/Tokyo", "+09:00", "+09:00", "1951-09-09T00:00:00+09:00[Asia/Tokyo]"],
    ["Australia/Adelaide", "+10:30", "+09:30", "2020-10-04T03:00:00+10:30[Australia/Adelaide]"],
    ["Australia/Lord_Howe", "+11:00", "+10:30", "2020-10-04T02:30:00+11:00[Australia/Lord_Howe]"],
    ["Pacific/Chatham", "+13:45", "+12:45", "2020-09-27T03:45:00+13:45[Pacific/Chatham]"],
    ["Pacific/Auckland", "+13:00", "+12:00", "2020-09-27T03:00:00+13:00[Pacific/Auckland]"],
    ["Pacific/Kiritimati", "+14:00", "+14:00", "1995-01-01T00:00:00+14:00[Pacific/Kiritimati]"],
    ["America/Santiago", "-03:00", "-04:00", "2020-09-06T01:00:00-03:00[America/Santiago]"],
    ["Atlantic/Azores", "-01:00", "+00:00", "2020-10-25T00:00:00-01:00[Atlantic/Azores]"],
];

const january = new Temporal.PlainDateTime(2021, 1, 15, 12);
const july = new Temporal.PlainDateTime(2021, 7, 15, 12);
for (let round = 0; round < 20; round++) {
    for (const [timeZone, januaryOffset, julyOffset, previous] of rotation) {
        const zdt = january.toZonedDateTime(timeZone);
        shouldBe(zdt.offset, januaryOffset, `${timeZone} January`);
        shouldBe(zdt.toString(), `2021-01-15T12:00:00${januaryOffset}[${timeZone}]`, `${timeZone} January string`);
        shouldBe(july.toZonedDateTime(timeZone).offset, julyOffset, `${timeZone} July`);
        shouldBe(zdt.getTimeZoneTransition("previous").toString(), previous, `${timeZone} previous`);
    }
}

// UTC-equivalent zones have no transitions and a zero offset everywhere.
const utcZones = ["UTC", "Etc/UTC", "Etc/UCT", "UCT", "Zulu", "Etc/Zulu", "Universal", "Etc/Universal", "Etc/GMT", "GMT", "Etc/GMT0", "Etc/GMT+0", "Etc/GMT-0", "Greenwich", "Etc/Greenwich"];
const utcInstants = [-8640000000000000000000n, -5331855838000000000n, 0n, 1615705200000000000n, 8640000000000000000000n];
for (let round = 0; round < 5; round++) {
    for (const timeZone of utcZones) {
        for (const epochNanoseconds of utcInstants) {
            const zdt = new Temporal.ZonedDateTime(epochNanoseconds, timeZone);
            shouldBe(zdt.offset, "+00:00", `${timeZone} offset`);
            shouldBe(zdt.offsetNanoseconds, 0, `${timeZone} offsetNanoseconds`);
            shouldBe(zdt.getTimeZoneTransition("next"), null, `${timeZone} next`);
            shouldBe(zdt.getTimeZoneTransition("previous"), null, `${timeZone} previous`);
        }
        const zdt = new Temporal.PlainDateTime(2021, 3, 14, 2, 30).toZonedDateTime(timeZone, { disambiguation: "reject" });
        shouldBe(zdt.toString(), `2021-03-14T02:30:00+00:00[${timeZone}]`, `${timeZone} string`);
        shouldBe(zdt.epochNanoseconds, 1615689000000000000n, `${timeZone} epochNanoseconds`);
        shouldBe(zdt.timeZoneId, timeZone, `${timeZone} id`);
    }
}
shouldBe(new Temporal.ZonedDateTime(-8640000000000000000000n, "UTC").toString(), "-271821-04-20T00:00:00+00:00[UTC]", "UTC min");
shouldBe(new Temporal.ZonedDateTime(8640000000000000000000n, "Etc/GMT").toString(), "+275760-09-13T00:00:00+00:00[Etc/GMT]", "Etc/GMT max");
