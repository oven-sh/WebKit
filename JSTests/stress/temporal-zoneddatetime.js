//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrow(op, errorConstructor) {
    try {
        op();
    } catch (e) {
        if (!(e instanceof errorConstructor))
            throw new Error(`threw ${e}, expected ${errorConstructor.name}`);
        return;
    }
    throw new Error(`expected to throw ${errorConstructor.name}`);
}

// Namespace.
shouldBe(typeof Temporal.ZonedDateTime, "function");
shouldBe(Temporal.ZonedDateTime.length, 2);
shouldBe(Temporal.ZonedDateTime.name, "ZonedDateTime");
shouldBe(Temporal.ZonedDateTime.prototype[Symbol.toStringTag], "Temporal.ZonedDateTime");

// Constructor must be called with new.
shouldThrow(() => Temporal.ZonedDateTime(0n, "UTC"), TypeError);

// Basic construction at Unix epoch in UTC.
{
    let z = new Temporal.ZonedDateTime(0n, "UTC");
    shouldBe(z.epochNanoseconds, 0n);
    shouldBe(z.epochMilliseconds, 0);
    shouldBe(z.timeZoneId, "UTC");
    shouldBe(z.calendarId, "iso8601");
    shouldBe(z.offset, "+00:00");
    shouldBe(z.offsetNanoseconds, 0);
    shouldBe(z.year, 1970);
    shouldBe(z.month, 1);
    shouldBe(z.monthCode, "M01");
    shouldBe(z.day, 1);
    shouldBe(z.hour, 0);
    shouldBe(z.minute, 0);
    shouldBe(z.second, 0);
    shouldBe(z.millisecond, 0);
    shouldBe(z.microsecond, 0);
    shouldBe(z.nanosecond, 0);
    shouldBe(z.dayOfWeek, 4); // Thursday
    shouldBe(z.dayOfYear, 1);
    shouldBe(z.daysInWeek, 7);
    shouldBe(z.daysInMonth, 31);
    shouldBe(z.daysInYear, 365);
    shouldBe(z.monthsInYear, 12);
    shouldBe(z.inLeapYear, false);
    shouldBe(z.toString(), "1970-01-01T00:00:00+00:00[UTC]");
    shouldBe(z.toJSON(), "1970-01-01T00:00:00+00:00[UTC]");
}

// valueOf must throw.
shouldThrow(() => +new Temporal.ZonedDateTime(0n, "UTC"), TypeError);

// Offset time zone.
{
    let z = new Temporal.ZonedDateTime(0n, "+05:30");
    shouldBe(z.timeZoneId, "+05:30");
    shouldBe(z.offset, "+05:30");
    shouldBe(z.offsetNanoseconds, 5 * 3600e9 + 30 * 60e9);
    shouldBe(z.hour, 5);
    shouldBe(z.minute, 30);
    shouldBe(z.toString(), "1970-01-01T05:30:00+05:30[+05:30]");
}

// Named time zone (IANA).
{
    let z = new Temporal.ZonedDateTime(1700000000000000000n, "America/New_York");
    shouldBe(z.timeZoneId, "America/New_York");
    shouldBe(z.offset, "-05:00");
    shouldBe(z.year, 2023);
    shouldBe(z.month, 11);
    shouldBe(z.day, 14);
    shouldBe(z.hour, 17);
    shouldBe(z.minute, 13);
    shouldBe(z.second, 20);
}

// Conversions.
{
    let z = new Temporal.ZonedDateTime(0n, "UTC");
    shouldBe(z.toInstant().epochNanoseconds, 0n);
    shouldBe(z.toPlainDate().toString(), "1970-01-01");
    shouldBe(z.toPlainTime().toString(), "00:00:00");
    shouldBe(z.toPlainDateTime().toString(), "1970-01-01T00:00:00");
}

// withTimeZone preserves the instant.
{
    let z = new Temporal.ZonedDateTime(0n, "UTC").withTimeZone("+01:00");
    shouldBe(z.epochNanoseconds, 0n);
    shouldBe(z.timeZoneId, "+01:00");
    shouldBe(z.hour, 1);
}

// compare.
shouldBe(Temporal.ZonedDateTime.compare(
    new Temporal.ZonedDateTime(0n, "UTC"),
    new Temporal.ZonedDateTime(1n, "UTC")), -1);
shouldBe(Temporal.ZonedDateTime.compare(
    new Temporal.ZonedDateTime(1n, "UTC"),
    new Temporal.ZonedDateTime(0n, "UTC")), 1);
shouldBe(Temporal.ZonedDateTime.compare(
    new Temporal.ZonedDateTime(0n, "UTC"),
    new Temporal.ZonedDateTime(0n, "+05:00")), 0);

// equals includes time zone identity.
shouldBe(new Temporal.ZonedDateTime(0n, "UTC").equals(new Temporal.ZonedDateTime(0n, "UTC")), true);
shouldBe(new Temporal.ZonedDateTime(0n, "UTC").equals(new Temporal.ZonedDateTime(0n, "+01:00")), false);

// Invalid inputs.
shouldThrow(() => new Temporal.ZonedDateTime(0n, "Not/AZone"), RangeError);
shouldThrow(() => new Temporal.ZonedDateTime(0n, 42), TypeError);

// Temporal.Now additions.
shouldBe(typeof Temporal.Now.zonedDateTimeISO, "function");
shouldBe(typeof Temporal.Now.plainDateTimeISO, "function");
shouldBe(typeof Temporal.Now.plainDateISO, "function");
shouldBe(typeof Temporal.Now.plainTimeISO, "function");
shouldBe(Temporal.Now.zonedDateTimeISO("UTC") instanceof Temporal.ZonedDateTime, true);
shouldBe(Temporal.Now.plainDateISO("UTC") instanceof Temporal.PlainDate, true);
shouldBe(Temporal.Now.plainTimeISO("UTC") instanceof Temporal.PlainTime, true);
shouldBe(Temporal.Now.plainDateTimeISO("UTC") instanceof Temporal.PlainDateTime, true);

// Instant interop.
{
    let z = new Temporal.ZonedDateTime(12345n, "UTC");
    shouldBe(Temporal.Instant.from(z).epochNanoseconds, 12345n);
}

// --- from() with property bags + DST disambiguation --------------------

{
    // Normal wall time.
    let z = Temporal.ZonedDateTime.from({ year: 2024, month: 3, day: 15, hour: 12, timeZone: "America/New_York" });
    shouldBe(z.toString(), "2024-03-15T12:00:00-04:00[America/New_York]");
}

{
    // DST spring-forward gap: 2024-03-10 02:30 doesn't exist in NY.
    // "compatible" (default) picks the later interpretation.
    let z = Temporal.ZonedDateTime.from({ year: 2024, month: 3, day: 10, hour: 2, minute: 30, timeZone: "America/New_York" });
    shouldBe(z.hour, 3);
    shouldBe(z.offset, "-04:00");
    // "reject" throws.
    shouldThrow(() => Temporal.ZonedDateTime.from(
        { year: 2024, month: 3, day: 10, hour: 2, minute: 30, timeZone: "America/New_York" },
        { disambiguation: "reject" }), RangeError);
}

{
    // DST fall-back overlap: 2024-11-03 01:30 occurs twice in NY.
    // "compatible"/"earlier" -> -04:00, "later" -> -05:00.
    let a = Temporal.ZonedDateTime.from({ year: 2024, month: 11, day: 3, hour: 1, minute: 30, timeZone: "America/New_York" });
    shouldBe(a.offset, "-04:00");
    let b = Temporal.ZonedDateTime.from(
        { year: 2024, month: 11, day: 3, hour: 1, minute: 30, timeZone: "America/New_York" },
        { disambiguation: "later" });
    shouldBe(b.offset, "-05:00");
    shouldThrow(() => Temporal.ZonedDateTime.from(
        { year: 2024, month: 11, day: 3, hour: 1, minute: 30, timeZone: "America/New_York" },
        { disambiguation: "reject" }), RangeError);
}

{
    // offset option: "reject" when offset doesn't match the time zone.
    shouldThrow(() => Temporal.ZonedDateTime.from(
        { year: 2024, month: 3, day: 15, hour: 12, offset: "+09:00", timeZone: "America/New_York" },
        { offset: "reject" }), RangeError);
    // "use" takes the provided offset literally.
    let z = Temporal.ZonedDateTime.from(
        { year: 2024, month: 3, day: 15, hour: 12, offset: "+00:00", timeZone: "America/New_York" },
        { offset: "use" });
    shouldBe(z.epochNanoseconds, BigInt(Date.UTC(2024, 2, 15, 12)) * 1000000n);
}

// startOfDay and hoursInDay, including a DST day.
{
    let z = Temporal.ZonedDateTime.from({ year: 2024, month: 3, day: 10, hour: 12, timeZone: "America/New_York" });
    shouldBe(z.startOfDay().hour, 0);
    shouldBe(z.hoursInDay, 23); // spring-forward
    let w = Temporal.ZonedDateTime.from({ year: 2024, month: 11, day: 3, hour: 12, timeZone: "America/New_York" });
    shouldBe(w.hoursInDay, 25); // fall-back
    let n = new Temporal.ZonedDateTime(0n, "UTC");
    shouldBe(n.hoursInDay, 24);
}

// Instant.prototype.toZonedDateTimeISO
{
    let z = Temporal.Instant.from("2024-01-01T00:00:00Z").toZonedDateTimeISO("Asia/Tokyo");
    shouldBe(z instanceof Temporal.ZonedDateTime, true);
    shouldBe(z.toString(), "2024-01-01T09:00:00+09:00[Asia/Tokyo]");
}

// ToTemporalTimeZoneIdentifier extracts from date-time strings.
{
    shouldBe(new Temporal.ZonedDateTime(0n, "UTC").withTimeZone("2021-08-19T17:30Z").timeZoneId, "UTC");
    shouldBe(new Temporal.ZonedDateTime(0n, "UTC").withTimeZone("2021-08-19T17:30-07:00").timeZoneId, "-07:00");
    shouldBe(new Temporal.ZonedDateTime(0n, "UTC").withTimeZone("2021-08-19T17:30[Asia/Tokyo]").timeZoneId, "Asia/Tokyo");
    shouldThrow(() => new Temporal.ZonedDateTime(0n, "UTC").withTimeZone("2021-08-19T17:30"), RangeError);
    shouldThrow(() => new Temporal.ZonedDateTime(0n, "UTC").withTimeZone("2021-08-19T17:30-07:00:01"), RangeError);
}

// --- add / subtract / with / withPlainTime / withCalendar --------------

{
    let z = new Temporal.ZonedDateTime(0n, "UTC");
    shouldBe(z.add({ hours: 1 }).toString(), "1970-01-01T01:00:00+00:00[UTC]");
    shouldBe(z.add({ days: 1 }).toString(), "1970-01-02T00:00:00+00:00[UTC]");
    shouldBe(z.add({ months: 1 }).toString(), "1970-02-01T00:00:00+00:00[UTC]");
    shouldBe(z.subtract({ hours: 1 }).toString(), "1969-12-31T23:00:00+00:00[UTC]");
}

{
    // Adding 1 calendar day across a DST spring-forward keeps the same
    // wall-clock hour but elapses only 23 hours.
    let a = Temporal.ZonedDateTime.from({ year: 2024, month: 3, day: 9, hour: 12, timeZone: "America/New_York" });
    let b = a.add({ days: 1 });
    shouldBe(b.hour, 12);
    shouldBe(b.offset, "-04:00");
    shouldBe(Number((b.epochNanoseconds - a.epochNanoseconds) / 3600000000000n), 23);
}

{
    let z = new Temporal.ZonedDateTime(0n, "UTC");
    shouldBe(z.withPlainTime("15:30").toString(), "1970-01-01T15:30:00+00:00[UTC]");
    shouldBe(z.withPlainTime().hour, 0);
    shouldBe(z.withCalendar("iso8601").calendarId, "iso8601");

    let z2 = Temporal.ZonedDateTime.from({ year: 2024, month: 6, day: 15, hour: 10, timeZone: "UTC" });
    let z3 = z2.with({ hour: 20, minute: 30 });
    shouldBe(z3.hour, 20);
    shouldBe(z3.minute, 30);
    shouldBe(z3.day, 15);
}

// PlainDateTime.prototype.toZonedDateTime
{
    let z = Temporal.PlainDateTime.from("2024-03-15T12:00").toZonedDateTime("Europe/Paris");
    shouldBe(z instanceof Temporal.ZonedDateTime, true);
    shouldBe(z.offset, "+01:00");
    shouldBe(z.hour, 12);
}
