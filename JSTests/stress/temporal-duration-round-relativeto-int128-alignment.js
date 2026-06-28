//@ requireOptions("--useTemporal=1")

// TemporalCore::splitTimeDuration and roundTime used to return std::pair
// values holding an Int128. Microsoft's STL compiles std::pair under
// #pragma pack(push, 8), which caps member alignment at 8 and under-aligns
// the Int128 member, so the 16-byte-aligned loads the code generator emits
// for it raised a general protection fault. On Windows, every
// Temporal.Duration.prototype.round call with a relativeTo option crashed.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected "${expected}" but got "${actual}"`);
}

shouldBe(Temporal.Duration.from({ months: 1 }).round({ largestUnit: "month", relativeTo: "2023-01-01" }).toString(), "P1M");
shouldBe(Temporal.Duration.from({ months: 17, days: 14 }).round({ largestUnit: "month", relativeTo: "2023-01-01" }).toString(), "P17M14D");
shouldBe(Temporal.Duration.from({ days: 40 }).round({ largestUnit: "month", relativeTo: "2023-01-01" }).toString(), "P1M9D");
shouldBe(Temporal.Duration.from({ days: 40 }).round({ smallestUnit: "month", relativeTo: "2023-01-01" }).toString(), "P1M");
shouldBe(Temporal.Duration.from({ hours: 36, minutes: 30 }).round({ largestUnit: "day", relativeTo: "2023-01-01" }).toString(), "P1DT12H30M");

// roundTime (ISOArithmetic.cpp) is the other std::pair<.., Int128> site,
// exercised by any wall-clock-time rounding.
shouldBe(Temporal.PlainDateTime.from("2023-01-01T12:34:56.789").round({ smallestUnit: "minute" }).toString(), "2023-01-01T12:35:00");
