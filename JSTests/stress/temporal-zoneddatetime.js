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
