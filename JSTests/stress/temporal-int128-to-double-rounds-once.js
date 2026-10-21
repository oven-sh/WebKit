//@ requireOptions("--useTemporal=1")

// A count of nanoseconds or microseconds of 2^64 or more becomes the nearest
// Number, rounded once. Where the standard library has no __int128 (Windows),
// WTF::Int128 is WTF::Int128Impl, whose conversion to double rounded each 64-bit
// half and then their sum. int128impl-to-floating-point-rounds-once.js tests
// that conversion on every platform. This file can only fail where WTF::Int128
// is WTF::Int128Impl.

function shouldBe(actual, expected, message) {
    if (!Object.is(actual, expected))
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldThrowRangeError(func, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError))
        throw new Error(`${message}: expected a RangeError but got ${error}`);
}

const nsPerSecond = 1000000000n;
const epoch = Temporal.Instant.fromEpochNanoseconds(0n);
const nsMaxInstant = 8640000000000000000000n;

function durationOf(ns) {
    return Temporal.Duration.from({ seconds: Number(ns / nsPerSecond), nanoseconds: Number(ns % nsPerSecond) });
}

function check(ns) {
    const nearest = Number(ns);
    const duration = durationOf(ns);
    shouldBe(duration.total("nanoseconds"), nearest, `total of ${ns}`);
    shouldBe(duration.negated().total("nanoseconds"), -nearest, `total of -${ns}`);
    shouldBe(duration.round({ largestUnit: "nanoseconds" }).nanoseconds, nearest, `round of ${ns}`);
    shouldBe(duration.negated().round({ largestUnit: "nanoseconds" }).nanoseconds, -nearest, `round of -${ns}`);

    // Two nanosecond fields that are exact as Numbers, and whose sum is ns.
    const low = ns % 2n ** 30n;
    const high = ns - low;
    shouldBe(Temporal.Duration.from({ nanoseconds: Number(high) }).add({ nanoseconds: Number(low) }).nanoseconds, nearest, `add to ${ns}`);
    shouldBe(Temporal.Duration.from({ nanoseconds: -Number(high) }).subtract({ nanoseconds: Number(low) }).nanoseconds, -nearest, `subtract to -${ns}`);

    if (ns <= nsMaxInstant) {
        const instant = Temporal.Instant.fromEpochNanoseconds(ns);
        shouldBe(epoch.until(instant, { largestUnit: "nanoseconds" }).nanoseconds, nearest, `until ${ns}`);
        shouldBe(epoch.since(instant, { largestUnit: "nanoseconds" }).nanoseconds, -nearest, `since ${ns}`);
        const zoned = new Temporal.ZonedDateTime(ns, "UTC");
        shouldBe(new Temporal.ZonedDateTime(0n, "UTC").until(zoned, { largestUnit: "nanoseconds" }).nanoseconds, nearest, `zoned until ${ns}`);
        shouldBe(Temporal.PlainDateTime.from("1970-01-01").until(zoned.toPlainDateTime(), { largestUnit: "nanoseconds" }).nanoseconds, nearest, `plain until ${ns}`);

        // The same digits as microseconds, with 999 nanoseconds below them.
        const microseconds = durationOf(ns * 1000n + 999n).round({ largestUnit: "microseconds" });
        shouldBe(microseconds.microseconds, nearest, `microseconds of ${ns}`);
        shouldBe(microseconds.nanoseconds, 999, `nanoseconds below ${ns} microseconds`);
    }
}

// From 2^64 to 2^65 a Number is a multiple of 4096, so an odd multiple of 2048 is a tie.
const base = 2n ** 64n + 2n ** 63n;
shouldBe(Number(base + 2049n), 27670116110564331520, "above the tie");
check(base + 2047n); // Below the tie: down.
check(base + 2048n); // The tie: to the even neighbour, down.
check(base + 2049n); // Above the tie: up. The low 64 bits alone round down to the tie.
check(base + 6143n); // Below the next tie: down. The low 64 bits alone round up to the tie.
check(base + 6144n); // The tie: to the even neighbour, up.
check(base + 6145n);
check(2n ** 64n + 2n ** 53n + 2049n); // The smallest count that the two roundings got wrong.
check(2n ** 64n + 1n);
check(2n ** 65n - 1n); // Up to the next power of two.
check(2n ** 72n + 2n ** 19n + 1n);
check(nsMaxInstant - 1n);
check(nsMaxInstant);

// test262 built-ins/Temporal/Duration/prototype/round/precision-exact-in-balance-time-duration.js
shouldBe(new Temporal.Duration(0, 0, 0, 0, 0, 0, 8692288669465520, 0, 373761).round({ largestUnit: "microseconds" }).microseconds, 8692288669465520373761, "test262");

// A Duration holds less than 2^53 seconds. A Number near that many nanoseconds is a multiple of
// 2^30, so 2^29 below the limit is the tie between the largest valid field and the limit.
const limit = 2n ** 53n * nsPerSecond;
const largest = limit - 2n ** 29n - 1n;
shouldBe(Number(largest), Number(limit - 2n ** 30n), "largest nanoseconds field");
check(largest);
check(largest - 255n);
check(largest - 256n);
shouldThrowRangeError(() => durationOf(largest + 1n).round({ largestUnit: "nanoseconds" }), "the tie below the limit");
shouldThrowRangeError(() => durationOf(limit - 1n).round({ largestUnit: "nanoseconds" }), "one below the limit");

// The same in microseconds, where a Number near the limit is a multiple of 2^20.
const limitInMicroseconds = 2n ** 53n * 1000000n;
for (const below of [1n, 511n, 512n]) {
    const microseconds = limitInMicroseconds - 2n ** 19n - below;
    shouldBe(durationOf(microseconds * 1000n).round({ largestUnit: "microseconds" }).microseconds, Number(microseconds), `${microseconds} microseconds`);
}
shouldThrowRangeError(() => durationOf((limitInMicroseconds - 2n ** 19n) * 1000n).round({ largestUnit: "microseconds" }), "the microseconds tie below the limit");

// A fixed sequence of counts of 65 to 83 bits (xorshift64).
let state = 0x9E3779B97F4A7C15n;
function random64() {
    state ^= BigInt.asUintN(64, state << 13n);
    state ^= state >> 7n;
    state ^= BigInt.asUintN(64, state << 17n);
    return state;
}
for (let i = 0; i < testLoopCount; ++i) {
    const bits = BigInt(64 + i % 19);
    const ns = 1n << bits | (random64() << 64n | random64()) & (1n << bits) - 1n;
    check(ns < largest ? ns : largest - (random64() >> 4n));
}
