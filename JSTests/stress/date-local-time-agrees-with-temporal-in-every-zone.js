//@ skip if $hostOS == "playstation"
//@ requireOptions("--useDollarVM=1")
//@ runDefault

function fail(zone, what, at, date, temporal)
{
    throw new Error(`${zone}: ${what} at ${at}: Date says ${date}, Temporal says ${temporal}`);
}

function checkInstant(zone, ms)
{
    const date = new Date(ms);
    const offsetOfDate = Date.UTC(date.getFullYear(), date.getMonth(), date.getDate(), date.getHours(), date.getMinutes(), date.getSeconds(), date.getMilliseconds()) - ms;
    const offsetOfTemporal = Temporal.Instant.fromEpochMilliseconds(ms).toZonedDateTimeISO(zone).offsetNanoseconds / 1e6;
    if (offsetOfDate !== offsetOfTemporal)
        fail(zone, "the offset", ms, offsetOfDate, offsetOfTemporal);
}

function checkLocal(zone, ms)
{
    const fields = new Date(ms);
    const year = fields.getUTCFullYear(), month = fields.getUTCMonth(), day = fields.getUTCDate(), hour = fields.getUTCHours(), minute = fields.getUTCMinutes();
    const byDate = new Date(year, month, day, hour, minute).getTime();
    const byTemporal = new Temporal.PlainDateTime(year, month + 1, day, hour, minute).toZonedDateTime(zone, { disambiguation: "compatible" }).epochMilliseconds;
    if (byDate !== byTemporal)
        fail(zone, "the local time", `${year}-${month + 1}-${day} ${hour}:${minute}`, byDate, byTemporal);
}

let checked = 0;

function checkZone(zone)
{
    if (Temporal.Now.timeZoneId() !== zone)
        throw new Error(`the host's zone is ${Temporal.Now.timeZoneId()}, not ${zone}`);
    for (let year = 1890; year <= 2060; year += 10) {
        for (const month of [0, 6]) {
            checkInstant(zone, Date.UTC(year, month, 15, 12));
            checkLocal(zone, Date.UTC(year, month, 15, 12));
            checked += 2;
        }
    }
    const transitions = [];
    let at = Temporal.Instant.fromEpochMilliseconds(Date.UTC(1900, 0, 1)).toZonedDateTimeISO(zone);
    const end = Date.UTC(2040, 0, 1);
    while (transitions.length < 80) {
        at = at.getTimeZoneTransition("next");
        if (!at || at.epochMilliseconds > end)
            break;
        transitions.push(at);
    }
    const month = 31 * 86400000;
    transitions.forEach((transition, index) => {
        const ms = transition.epochMilliseconds;
        if (index > 0 && ms - transitions[index - 1].epochMilliseconds < month)
            return;
        if (index + 1 < transitions.length && transitions[index + 1].epochMilliseconds - ms < month)
            return;
        const before = Temporal.Instant.fromEpochMilliseconds(ms - 1000).toZonedDateTimeISO(zone).offsetNanoseconds / 1e6;
        const after = transition.offsetNanoseconds / 1e6;
        for (const delta of [-3600000, -1000, 0, 1000, 3600000]) {
            checkInstant(zone, ms + delta);
            for (const offset of [before, after])
                checkLocal(zone, Math.floor((ms + offset + delta) / 60000) * 60000);
            checked += 3;
        }
    });
}

const zones = Intl.supportedValuesOf("timeZone");
if (zones.length < 400)
    throw new Error(`only ${zones.length} time zones`);

let next = 0;
function step()
{
    if (next === zones.length) {
        if (checked < 100000)
            throw new Error(`only ${checked} checks`);
        return;
    }
    const zone = zones[next++];
    if (!$vm.setHostTimeZone(zone))
        throw new Error(`${zone} was refused`);
    setTimeout(() => {
        checkZone(zone);
        step();
    }, 0);
}
step();
