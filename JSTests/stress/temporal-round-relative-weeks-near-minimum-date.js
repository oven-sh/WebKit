//@ requireOptions("--useTemporal=1")

// NudgeToCalendarUnit computes two window edges with CalendarDateAdd from the origin date. When the
// origin is late and the destination is the minimum PlainDate, the far edge of a "weeks" window lands
// exactly on -271821-04-19 (epoch day -100000001), which ISODateWithinLimits accepts. A stale guard
// compared |epoch days| against 1e8 and rejected that edge, so since()/round() threw for every
// rounding mode while the until() mirror (edges near 2016) worked.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrow(ctor, fn, message) {
    try {
        fn();
    } catch (e) {
        if (!(e instanceof ctor))
            throw new Error(`${message}: expected ${ctor.name}, got ${e}`);
        return;
    }
    throw new Error(`${message}: expected ${ctor.name} but no exception was thrown`);
}

const min = Temporal.PlainDate.from("-271821-04-19");
const nearMin = Temporal.PlainDate.from("-271821-04-20");
const max = Temporal.PlainDate.from("+275760-09-13");
const later = Temporal.PlainDate.from("2016-02-29");

// 2016-02-29 is epoch day 16860, -271821-04-20 is epoch day -100000000: 100016860 days = 14288122 weeks + 6 days.
const expectedWeeks = {
    trunc: "P14288122W",
    floor: "P14288122W",
    halfTrunc: "P14288123W",
    halfEven: "P14288123W",
    halfExpand: "P14288123W",
    halfCeil: "P14288123W",
    halfFloor: "P14288123W",
    ceil: "P14288123W",
    expand: "P14288123W",
};

for (const [roundingMode, expected] of Object.entries(expectedWeeks)) {
    shouldBe(later.since(nearMin, { smallestUnit: "weeks", roundingMode }).toString(), expected, `since weeks ${roundingMode}`);
    shouldBe(nearMin.until(later, { smallestUnit: "weeks", roundingMode }).toString(), expected, `until weeks ${roundingMode}`);
    // The negative direction rounds with the same magnitude for the sign-symmetric modes.
    if (roundingMode === "trunc" || roundingMode === "expand" || roundingMode.startsWith("half"))
        shouldBe(nearMin.since(later, { smallestUnit: "weeks", roundingMode }).toString(), "-" + expected, `reverse since weeks ${roundingMode}`);
}

// The exact minimum as the other operand: 100016861 days = 14288123 weeks exactly. From the minimum,
// both window edges are later dates. From 2016 the far edge is one week below the minimum, and
// CalendarDateAdd rejects it for every rounding mode: the spec computes both edges unconditionally.
shouldBe(min.until(later, { smallestUnit: "weeks" }).toString(), "P14288123W", "until weeks from the minimum date");
shouldBe(min.until(later, { smallestUnit: "weeks", roundingIncrement: 2, roundingMode: "trunc" }).toString(), "P14288122W", "until weeks increment 2");
shouldThrow(RangeError, () => later.since(min, { smallestUnit: "weeks" }), "since weeks to the minimum date: far edge is below the minimum");
shouldThrow(RangeError, () => later.since(min, { smallestUnit: "weeks", roundingIncrement: 2, roundingMode: "trunc" }), "since weeks increment 2 to the minimum date");

// days with an increment go through NudgeToDayOrTime and were never affected; keep them pinned.
shouldBe(later.since(nearMin, { smallestUnit: "days", roundingIncrement: 7 }).toString(), "P100016854D", "since days x7");

// PlainDateTime shares DifferenceISODateTime + RoundRelativeDuration.
{
    const laterDateTime = later.toPlainDateTime("12:00");
    const nearMinDateTime = nearMin.toPlainDateTime("12:00");
    shouldBe(laterDateTime.since(nearMinDateTime, { smallestUnit: "weeks" }).toString(), "P14288122W", "PlainDateTime since weeks trunc");
    shouldBe(laterDateTime.since(nearMinDateTime, { smallestUnit: "weeks", roundingMode: "expand" }).toString(), "P14288123W", "PlainDateTime since weeks expand");
}

// Duration.round/total with relativeTo take the same path with a negative duration.
{
    const negative = Temporal.Duration.from({ days: -100016860 });
    shouldBe(negative.round({ relativeTo: later, smallestUnit: "weeks" }).toString(), "-P14288123W", "Duration.round weeks halfExpand");
    shouldBe(negative.round({ relativeTo: later, smallestUnit: "weeks", roundingMode: "trunc" }).toString(), "-P14288122W", "Duration.round weeks trunc");
    shouldBe(negative.total({ relativeTo: later, unit: "weeks" }), -100016860 / 7, "Duration.total weeks");
}

// The mirror at the maximum: the far edge of until() lands exactly on +275760-09-13.
{
    const early = Temporal.PlainDate.from("-002016-03-03");
    const nearMax = Temporal.PlainDate.from("+275760-09-12");
    shouldBe(early.until(nearMax, { largestUnit: "days" }).days, 101455794, "days near max"); // 14493684 weeks + 6 days
    shouldBe(early.until(nearMax, { smallestUnit: "weeks" }).toString(), "P14493684W", "until weeks near max trunc");
    shouldBe(early.until(nearMax, { smallestUnit: "weeks", roundingMode: "expand" }).toString(), "P14493685W", "until weeks near max expand");
    shouldBe(nearMax.since(early, { smallestUnit: "weeks" }).toString(), "P14493684W", "since weeks near max trunc");
    shouldBe(nearMax.since(early, { smallestUnit: "weeks", roundingMode: "expand" }).toString(), "P14493685W", "since weeks near max expand");
}

// A window edge that is really outside the representable range still throws.
shouldThrow(RangeError, () => later.since(nearMin, { smallestUnit: "months", roundingIncrement: 1e9 }), "month window edge beyond the minimum");
shouldThrow(RangeError, () => Temporal.Duration.from({ weeks: -14288124 }).round({ relativeTo: later, smallestUnit: "weeks", roundingIncrement: 3 }), "week window edge beyond the minimum");
