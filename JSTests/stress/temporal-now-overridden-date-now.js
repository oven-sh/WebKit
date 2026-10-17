//@ requireOptions("--useTemporal=1")

// JSGlobalObject::overridenDateNow is the clock an embedder's setSystemTime() (bun:test) installs.
// Date.now(), new Date(), Date() and Intl read it through jsDateNow(); every Temporal.Now function
// has to report the same instant. $vm.overrideDateNow(ms) writes the field of this global object.

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${String(expected)}, got ${String(actual)}`);
}
function shouldThrow(fn, errorConstructor, label) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (!caught) throw new Error(`${label}: expected ${errorConstructor.name}, got no throw`);
    if (!(caught instanceof errorConstructor))
        throw new Error(`${label}: expected ${errorConstructor.name}, got ${caught.constructor.name}: ${caught.message}`);
}

const realBefore = Date.now();

function checkUTC(ms, utcDateTime) {
    $vm.overrideDateNow(ms);
    const expectedNs = BigInt(ms) * 1000000n;
    shouldBe(Date.now(), ms, `Date.now() at ${ms}`);

    shouldBe(Temporal.Now.instant().toString(), `${utcDateTime}Z`, `instant() at ${ms}`);
    shouldBe(Temporal.Now.instant().epochMilliseconds, ms, `instant().epochMilliseconds at ${ms}`);
    shouldBe(Temporal.Now.instant().epochNanoseconds, expectedNs, `instant().epochNanoseconds at ${ms}`);

    shouldBe(Temporal.Now.zonedDateTimeISO("UTC").toString(), `${utcDateTime}+00:00[UTC]`, `zonedDateTimeISO("UTC") at ${ms}`);
    shouldBe(Temporal.Now.zonedDateTimeISO().epochNanoseconds, expectedNs, `zonedDateTimeISO().epochNanoseconds at ${ms}`);
    shouldBe(Temporal.Now.zonedDateTimeISO().timeZoneId, Temporal.Now.timeZoneId(), `zonedDateTimeISO().timeZoneId at ${ms}`);

    shouldBe(Temporal.Now.plainDateTimeISO("UTC").toString(), utcDateTime, `plainDateTimeISO("UTC") at ${ms}`);
    shouldBe(Temporal.Now.plainDateISO("UTC").toString(), utcDateTime.slice(0, utcDateTime.indexOf("T")), `plainDateISO("UTC") at ${ms}`);
    shouldBe(Temporal.Now.plainTimeISO("UTC").toString(), utcDateTime.slice(utcDateTime.indexOf("T") + 1), `plainTimeISO("UTC") at ${ms}`);
}

// Without a time zone argument systemDateTime() takes the DateCache fast path. It has to agree
// with the explicit system zone (getISODateTimeFor) and with what the (equally overridden) Date
// reports for the instant in that zone, whatever the host zone is.
function checkSystemZone(ms) {
    $vm.overrideDateNow(ms);
    const systemZone = Temporal.Now.timeZoneId();
    shouldBe(Temporal.Now.plainDateTimeISO().equals(Temporal.Now.plainDateTimeISO(systemZone)), true, `plainDateTimeISO() matches the explicit system zone at ${ms}`);
    shouldBe(Temporal.Now.plainDateISO().equals(Temporal.Now.plainDateISO(systemZone)), true, `plainDateISO() matches the explicit system zone at ${ms}`);
    shouldBe(Temporal.Now.plainTimeISO().equals(Temporal.Now.plainTimeISO(systemZone)), true, `plainTimeISO() matches the explicit system zone at ${ms}`);
    shouldBe(Temporal.Now.zonedDateTimeISO().toPlainDateTime().equals(Temporal.Now.plainDateTimeISO()), true, `zonedDateTimeISO() matches plainDateTimeISO() at ${ms}`);

    const date = new Date();
    shouldBe(date.getTime(), ms, `new Date() at ${ms}`);
    const dateTimeOfDate = Temporal.PlainDateTime.from({
        year: date.getFullYear(), month: date.getMonth() + 1, day: date.getDate(),
        hour: date.getHours(), minute: date.getMinutes(), second: date.getSeconds(), millisecond: date.getMilliseconds(),
    });
    shouldBe(Temporal.Now.plainDateTimeISO().equals(dateTimeOfDate), true, `plainDateTimeISO() matches new Date() at ${ms}`);
}

checkUTC(819331200123, "1995-12-19T00:00:00.123");
checkUTC(0, "1970-01-01T00:00:00");
checkUTC(-1, "1969-12-31T23:59:59.999");
checkUTC(-315619200000, "1960-01-01T00:00:00");
// The ends of the Date range are the ends of the Temporal range.
checkUTC(8.64e15, "+275760-09-13T00:00:00");
checkUTC(-8.64e15, "-271821-04-20T00:00:00");

checkSystemZone(819331200123);
checkSystemZone(-1);
checkSystemZone(-315619200000);
checkSystemZone(1720000000000);

// The override is TimeClip'd exactly like `new Date()`: truncated to whole milliseconds...
for (const fractional of [819331200000.75, 0.5, -0.5, -1.25]) {
    $vm.overrideDateNow(fractional);
    const clipped = new Date().getTime();
    shouldBe(clipped, Math.trunc(fractional), `new Date() at ${fractional}`);
    shouldBe(Temporal.Now.instant().epochNanoseconds, BigInt(clipped) * 1000000n, `instant() at ${fractional}`);
    shouldBe(Temporal.Now.zonedDateTimeISO("UTC").epochNanoseconds, BigInt(clipped) * 1000000n, `zonedDateTimeISO("UTC") at ${fractional}`);
    shouldBe(Temporal.Now.plainDateTimeISO("UTC").millisecond, new Date().getUTCMilliseconds(), `plainDateTimeISO("UTC").millisecond at ${fractional}`);
}

// ...and a value that makes `new Date()` an Invalid Date has no Temporal equivalent.
for (const outOfRange of [8.64e15 + 1, -8.64e15 - 1, 1e300, Infinity, -Infinity]) {
    $vm.overrideDateNow(outOfRange);
    shouldBe(Date.now(), outOfRange, `Date.now() at ${outOfRange}`);
    shouldBe(Number.isNaN(new Date().getTime()), true, `new Date() at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.instant(), RangeError, `instant() at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.zonedDateTimeISO(), RangeError, `zonedDateTimeISO() at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.zonedDateTimeISO("UTC"), RangeError, `zonedDateTimeISO("UTC") at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.plainDateTimeISO(), RangeError, `plainDateTimeISO() at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.plainDateTimeISO("UTC"), RangeError, `plainDateTimeISO("UTC") at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.plainDateISO(), RangeError, `plainDateISO() at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.plainTimeISO(), RangeError, `plainTimeISO() at ${outOfRange}`);
    // The time zone argument is still validated first, and timeZoneId() does not read the clock.
    shouldThrow(() => Temporal.Now.plainDateISO(42), TypeError, `plainDateISO(42) at ${outOfRange}`);
    shouldThrow(() => Temporal.Now.zonedDateTimeISO("bogus"), RangeError, `zonedDateTimeISO("bogus") at ${outOfRange}`);
    shouldBe(typeof Temporal.Now.timeZoneId(), "string", `timeZoneId() at ${outOfRange}`);
}

// The override belongs to the global object it was set on, for Temporal.Now as for Date.
{
    $vm.overrideDateNow(819331200000);
    const other = $vm.createGlobalObject();
    shouldBe(Temporal.Now.instant().epochMilliseconds, 819331200000, "this realm's instant()");
    shouldBe(other.Date.now() >= realBefore, true, "the other realm's Date.now() is the real clock");
    shouldBe(other.Temporal.Now.instant().epochMilliseconds >= realBefore, true, "the other realm's instant() is the real clock");

    other.$vm.overrideDateNow(0);
    shouldBe(other.Date.now(), 0, "the other realm's Date.now() once overridden");
    shouldBe(other.Temporal.Now.instant().toString(), "1970-01-01T00:00:00Z", "the other realm's instant() once overridden");
    shouldBe(other.Temporal.Now.plainDateISO("UTC").toString(), "1970-01-01", "the other realm's plainDateISO() once overridden");
    shouldBe(Temporal.Now.instant().epochMilliseconds, 819331200000, "this realm's instant() after the other realm's override");
}

// Clearing the override (no argument, as setSystemTime() with none) goes back to the real clock.
for (const clear of [() => $vm.overrideDateNow(), () => $vm.overrideDateNow(NaN)]) {
    $vm.overrideDateNow(819331200000);
    shouldBe(Temporal.Now.instant().epochMilliseconds, 819331200000, "instant() before clearing");
    clear();
    shouldBe(Date.now() >= realBefore, true, "Date.now() after clearing");
    shouldBe(Temporal.Now.instant().epochMilliseconds >= realBefore, true, "instant() after clearing");
    shouldBe(Temporal.Now.zonedDateTimeISO().epochMilliseconds >= realBefore, true, "zonedDateTimeISO() after clearing");
    shouldBe(Temporal.Now.plainDateTimeISO("UTC").year >= new Date(realBefore).getUTCFullYear(), true, "plainDateTimeISO() after clearing");
    shouldBe(Temporal.Now.instant().epochMilliseconds <= Date.now(), true, "instant() after clearing is not ahead of Date.now()");
}
