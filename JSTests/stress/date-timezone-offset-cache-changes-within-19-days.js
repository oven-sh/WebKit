//@ skip if $hostOS == "playstation"
//@ runDefault

// DateCache::DSTCache (runtime/JSDateMath.cpp) caches the local time offset as intervals. It used to take two
// instants at most 19 days apart with the same offset to have that offset in between, and to find the instant where
// the offset changes by bisecting between two cached intervals with different offsets. A zone can change its offset
// more than once within 19 days, so both went wrong:
// - A change and a change back (America/Recife was on DST from 2000-10-08 to 10-15 only) was merged over: the short
//   period got the offset around it, for the instants the cache had not seen before.
// - A bisection probe between two changes to different offsets (America/Cambridge_Bay 2000-10-29 and 11-05,
//   America/Asuncion 2024-10-06 and 10-15) matched neither interval. It was filed under the later one, so the instants
//   from the probe up to that interval got its offset or its DST flag, and debug builds failed
//   ASSERT(m_after->offset == offset).
// The cache now asks ICU's transition table where a run of one offset ends.
//
// Each scenario primes the cache with two instants and then checks a window around the transitions, every four
// hours, against Intl.DateTimeFormat, which asks ICU directly and shares no state with Date. So the expectations
// follow the tzdata in use: where a zone does not have the close transitions the scenario just passes.

// One language for both Date.prototype.toString() and the Intl oracle below.
$vm.setUserPreferredLanguages(["en-US"]);

const msPerMinute = 60 * 1000;
const msPerHour = 60 * msPerMinute;
const msPerDay = 24 * msPerHour;
const step = 4 * msPerHour;

function fail(message)
{
    throw new Error(message);
}

function iso(time)
{
    return new Date(time).toISOString();
}

// Minutes east of UTC at `time` in `timeZone` according to Intl ("GMT-05:00" is -300, "GMT+00:40" is 40, "GMT" is 0).
function makeOffsetOracle(timeZone)
{
    const format = new Intl.DateTimeFormat("en-US", { timeZone, timeZoneName: "longOffset", hour: "numeric" });
    return (time) => {
        const name = format.formatToParts(time).find((part) => part.type === "timeZoneName").value;
        if (name === "GMT")
            return 0;
        const match = /^GMT([+-])(\d{1,2}):?(\d{2})?(?::(\d{2}))?$/.exec(name) || fail(`${timeZone}: unexpected offset name ${name}`);
        return (match[1] === "-" ? -1 : 1) * (Number(match[2]) * 60 + Number(match[3] || 0) + Number(match[4] || 0) / 60);
    };
}

// The zone's long name at `time` according to Intl, "Paraguay Summer Time" or "Paraguay Standard Time". For a zone
// that kept one name over the years this is what Date.prototype.toString() shows in parentheses, and the
// Standard / Summer distinction in it is the cached DST flag.
function makeNameOracle(timeZone)
{
    const format = new Intl.DateTimeFormat(undefined, { timeZone, timeZoneName: "long", hour: "numeric" });
    return (time) => format.formatToParts(time).find((part) => part.type === "timeZoneName").value;
}

function nameInDateString(date)
{
    const string = date.toString();
    const match = /\(([^)]*)\)$/.exec(string) || fail(`no zone name in ${string}`);
    return match[1];
}

// A small deterministic shuffle, so that the sweep does not only walk forward (which the cache is best at).
function shuffled(array, seed)
{
    const result = array.slice();
    let state = seed >>> 0;
    for (let i = result.length - 1; i > 0; --i) {
        state = (Math.imul(state, 1103515245) + 12345) >>> 0;
        const j = state % (i + 1);
        [result[i], result[j]] = [result[j], result[i]];
    }
    return result;
}

// One Date for all the UTC-to-local checks: the cost of a zone change grows with the number of Dates in the heap.
const date = new Date;

function checkWindow(scenario, label, order)
{
    const { timeZone, offsetAt, nameAt } = scenario;
    for (const time of order) {
        date.setTime(time);
        const expectedOffset = offsetAt.get(time);

        const actualOffset = -date.getTimezoneOffset();
        if (actualOffset !== expectedOffset)
            fail(`${timeZone} ${label}: offset of ${iso(time)} is ${actualOffset}, expected ${expectedOffset}`);

        if (nameAt) {
            const actualName = nameInDateString(date);
            const expectedName = nameAt.get(time);
            if (actualName !== expectedName)
                fail(`${timeZone} ${label}: ${iso(time)} is in "${actualName}", expected "${expectedName}"`);
        }

        // The local time to UTC direction has a cache of its own with the same logic. Where the offset is steady for
        // four hours either side, the local time of `time` exists exactly once, so building a Date from its fields
        // has to give `time` back.
        if (offsetAt.get(time - step) !== expectedOffset || offsetAt.get(time + step) !== expectedOffset)
            continue;
        date.setTime(time + expectedOffset * msPerMinute);
        const fromFields = new Date(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate(), date.getUTCHours(), date.getUTCMinutes()).getTime();
        if (fromFields !== time)
            fail(`${timeZone} ${label}: ${date.toISOString().slice(0, 16)} local time is ${iso(fromFields)}, expected ${iso(time)}`);
    }
}

function makeScenario(timeZone, firstTransition, lastTransition, checkNames)
{
    const first = Date.parse(firstTransition);
    const last = Date.parse(lastTransition);
    const offsetOracle = makeOffsetOracle(timeZone);
    const nameOracle = checkNames ? makeNameOracle(timeZone) : null;

    // The instants to check: from two days before the first transition to 20 days past the last.
    const sweep = [];
    for (let time = first - 2 * msPerDay; time <= last + 20 * msPerDay; time += step)
        sweep.push(time);
    // A walk of one instant per day from 40 days before the first transition to 20 days past the last, the shape of a
    // calendar loop. Every day of the walk is on the sweep's grid.
    const daily = [];
    for (let time = first - 40 * msPerDay; time <= last + 20 * msPerDay; time += msPerDay)
        daily.push(time);
    const offsetAt = new Map;
    for (let time = daily[0] - step; time <= sweep[sweep.length - 1] + step; time += step)
        offsetAt.set(time, offsetOracle(time));
    const nameAt = nameOracle ? new Map([...daily, ...sweep].map((time) => [time, nameOracle(time)])) : null;

    // The pairs of instants to prime the cache with: the first one from 19 days before the first transition up to
    // the last transition, every other day at another hour, the second one a minute to 17 days after it. So the 19 day
    // window past the first one holds both transitions, and the second one comes at the offsets from either side.
    const primings = [];
    for (let time = first - 19 * msPerDay - msPerMinute; time <= last; time += 49 * msPerHour) {
        for (const distance of [msPerMinute, 6 * msPerDay, 12 * msPerDay, 17 * msPerDay])
            primings.push([time, time + distance]);
    }

    return { timeZone, sweep, daily, offsetAt, nameAt, offsetOracle, primings };
}

const scenarios = [
    // -05 DST (CDT) until 2000-10-29T07:00Z, then -05 (EST) until 2000-11-05T05:00Z, then -06 (CST).
    makeScenario("America/Cambridge_Bay", "2000-10-29T07:00Z", "2000-11-05T05:00Z", false),
    // -04 until 2024-10-06T04:00Z, then -03 DST until 2024-10-15T03:00Z, then -03 as the standard time (tzdata 2025a).
    // The offset does not tell the last two apart, the name in Date.prototype.toString() does.
    makeScenario("America/Asuncion", "2024-10-06T04:00Z", "2024-10-15T03:00Z", true),
    // +01 (CET) until 1944-04-03T01:00Z, then +02 DST (CEST) until 1944-04-12T22:00Z, then +03 (MSK).
    makeScenario("Europe/Simferopol", "1944-04-03T01:00Z", "1944-04-12T22:00Z", false),
    // +02 DST (CEST) until 1944-10-02T01:00Z, then +01 (CET) until 1944-10-12T23:00Z, then +03 (MSK).
    makeScenario("Europe/Riga", "1944-10-02T01:00Z", "1944-10-12T23:00Z", false),
    // -01 until 1976-04-14T01:00Z, then +00 until 1976-05-01T00:00Z, then +01 DST.
    makeScenario("Africa/El_Aaiun", "1976-04-14T01:00Z", "1976-05-01T00:00Z", false),

    // A change and a change back within 19 days.
    // -03 until 2000-10-08T03:00Z, then -02 DST until 2000-10-15T02:00Z, then -03 again.
    makeScenario("America/Recife", "2000-10-08T03:00Z", "2000-10-15T02:00Z", false),
    // -03 until 2000-10-08T03:00Z, then -02 DST until 2000-10-22T02:00Z, then -03 again.
    makeScenario("America/Fortaleza", "2000-10-08T03:00Z", "2000-10-22T02:00Z", false),
    // -03 until 2004-06-01T03:00Z, then -04 until 2004-06-13T04:00Z, then -03 again.
    makeScenario("America/Argentina/Tucuman", "2004-06-01T03:00Z", "2004-06-13T04:00Z", false),
    // +02 DST until 1943-04-17T00:00Z, then +01 until 1943-04-25T01:00Z, then +02 DST again.
    makeScenario("Africa/Tunis", "1943-04-17T00:00Z", "1943-04-25T01:00Z", false),
    // +02 until 2040-10-20T00:00Z, then +03 DST until 2040-10-26T23:00Z, then +02 again (a predicted Ramadan break).
    makeScenario("Asia/Gaza", "2040-10-20T00:00Z", "2040-10-26T23:00Z", false),
];

const steps = [];
for (const scenario of scenarios) {
    // One pass over the window in order, one daily walk up to it, and one shuffled, on a fresh cache each, without
    // any priming.
    steps.push([scenario, "forward", null, scenario.sweep]);
    steps.push([scenario, "daily", null, scenario.daily]);
    steps.push([scenario, "shuffled", null, shuffled(scenario.sweep, 1)]);
    scenario.primings.forEach(([first, second], index) => {
        steps.push([scenario, `after ${iso(first)} and ${iso(second)}`, [first, second], shuffled(scenario.sweep, index + 2)]);
    });
}

function runNextStep()
{
    if (!steps.length)
        return;
    const [scenario, label, priming, order] = steps.shift();
    // Setting the host time zone, even to the same zone, empties the offset caches, as of the next VM entry.
    if (!$vm.setHostTimeZone(scenario.timeZone))
        fail(`failed to set the host time zone to ${scenario.timeZone}`);
    setTimeout(() => {
        if (priming) {
            for (const time of priming) {
                date.setTime(time);
                const actualOffset = -date.getTimezoneOffset();
                const expectedOffset = scenario.offsetOracle(time);
                if (actualOffset !== expectedOffset)
                    fail(`${scenario.timeZone} ${label}: offset of ${iso(time)} is ${actualOffset}, expected ${expectedOffset}`);
            }
        }
        checkWindow(scenario, label, order);
        runNextStep();
    }, 0);
}
runNextStep();
