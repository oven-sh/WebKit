//@ skip if $hostOS == "playstation"
//@ runDefault

// Where Bun bundles ICU, a time zone is held as ICU's zone object (ICUTimeZone.cpp). It was held as a calendar, and
// that ICU opens no calendar without the data that tells it the likely script and region of a locale: about 18 million
// instructions and 470 page faults, in a process that may never name a locale. The offsets, the transitions and the
// names of a zone need none of that data, so a program that only asks for those does not load it.
//
// icuDataItemCount() is how many items of its data ICU has loaded. The shell has it where its ICU data is packed as
// Bun's is (jsc.cpp, BUN_ICU_ZSTD). Without it this test says nothing.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

if (typeof icuDataItemCount === "function") {
    // The host's zone, which the test runner makes US/Pacific: the first local-time read of a Date, a local time to
    // UTC, and the name of the zone.
    const date = new Date(2024, 0, 15, 12);
    expect("local hour", date.getHours(), 12);
    expect("offset in July", new Date(2024, 6, 15, 12).getTimezoneOffset(), 420);
    expect("a local time that a transition skips", new Date(2024, 2, 10, 2, 30).getTime(), Date.UTC(2024, 2, 10, 10, 30));
    expect("name of the zone", date.toString(), "Mon Jan 15 2024 12:00:00 GMT-0800 (Pacific Standard Time)");
    expect("default zone of Temporal", Temporal.Now.zonedDateTimeISO().timeZoneId, "America/Los_Angeles");

    // A zone that the program names: an offset, a local time to UTC, and both transitions.
    const zoned = new Temporal.ZonedDateTime(0n, "Europe/Berlin");
    expect("offset of a named zone", zoned.offsetNanoseconds, 3600e9);
    expect("local time in a named zone", Temporal.PlainDateTime.from("2024-07-15T12:00").toZonedDateTime("Europe/Berlin").epochMilliseconds, Date.UTC(2024, 6, 15, 10));
    expect("next transition", zoned.getTimeZoneTransition("next").epochMilliseconds, Date.UTC(1980, 3, 6, 1));
    expect("previous transition", zoned.getTimeZoneTransition("previous").epochMilliseconds, Date.UTC(1949, 9, 2, 1));

    const loaded = icuDataItemCount();
    if (!loaded)
        throw new Error("ICU gave the offsets of a zone and loaded no data");

    // To maximize a locale takes that data. If it is loaded here, nothing above loaded it.
    const locale = new Intl.Locale("en");
    const before = icuDataItemCount();
    expect("maximized locale", locale.maximize().toString(), "en-Latn-US");
    if (icuDataItemCount() === before)
        throw new Error("ICU had the data of likely subtags before a locale was maximized: something that holds a time zone opened a calendar");
}
