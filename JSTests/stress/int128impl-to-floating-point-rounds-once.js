//@ requireOptions("--useDollarVM=1")

// WTF::UInt128Impl and WTF::Int128Impl convert to the nearest double or float,
// with a tie going to the even one: the value is rounded once. They used to
// round each 64-bit half and then the sum of the results. The classes are
// WTF::UInt128 and WTF::Int128 where the standard library has no __int128
// (Windows). $vm.int128ImplToNumber runs their conversions on every platform.

function shouldBe(actual, expected, message) {
    if (!Object.is(actual, expected))
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

const unsignedToDouble = (high, low) => $vm.int128ImplToNumber(high, low, false, false);
const unsignedToFloat = (high, low) => $vm.int128ImplToNumber(high, low, false, true);
const signedToDouble = (high, low) => $vm.int128ImplToNumber(high, low, true, false);
const signedToFloat = (high, low) => $vm.int128ImplToNumber(high, low, true, true);

// The Number nearest to value when `digits` significant bits are kept, ties to even. Integer arithmetic only.
function nearest(value, digits) {
    if (value < 0n)
        return -nearest(-value, digits);
    const bits = value ? value.toString(2).length : 0;
    if (bits <= digits)
        return Number(value);
    const shift = BigInt(bits - digits);
    let kept = value >> shift;
    const dropped = value & ((1n << shift) - 1n);
    const half = 1n << (shift - 1n);
    if (dropped > half || (dropped === half && (kept & 1n)))
        kept++;
    return Number(kept << shift);
}

// value is from -2^127 to 2^128 - 1.
function check(value) {
    const high = BigInt.asUintN(64, value >> 64n);
    const low = BigInt.asUintN(64, value);
    const asDouble = nearest(value, 53);
    shouldBe(asDouble, Number(value), `nearest(${value}, 53)`);
    const asFloat = Math.fround(nearest(value, 24));
    if (value >= 0n) {
        shouldBe(unsignedToDouble(high, low), asDouble, `double(UInt128Impl(${value}))`);
        shouldBe(unsignedToFloat(high, low), asFloat, `float(UInt128Impl(${value}))`);
    }
    if (value < 2n ** 127n) {
        shouldBe(signedToDouble(high, low), asDouble, `double(Int128Impl(${value}))`);
        shouldBe(signedToFloat(high, low), asFloat, `float(Int128Impl(${value}))`);
    }
}

function checkBothSigns(value) {
    check(value);
    check(-value);
}

// From 2^64 to 2^65 a double is a multiple of 4096, so 2049 above one is above a tie.
shouldBe(unsignedToDouble(1n, 0x8000000000000801n), 27670116110564331520, "2^64 + 2^63 + 2049");
shouldBe(signedToDouble(-2n, 0x7ffffffffffff7ffn), -27670116110564331520, "-(2^64 + 2^63 + 2049)");
checkBothSigns(2n ** 64n + 2n ** 63n + 2049n);
checkBothSigns(2n ** 64n + 2n ** 53n + 2049n); // The smallest value that the three roundings got wrong.
checkBothSigns((2n ** 53n + 1n) * 2n ** 64n + 1n); // The high half alone is a tie.
checkBothSigns(2n ** 53n * 10n ** 9n - 2n ** 29n - 1n); // The largest nanoseconds of a Temporal.Duration.

// The limits.
check(0n);
check(2n ** 64n - 1n);
check(2n ** 64n);
check(2n ** 128n - 1n);
check(2n ** 127n - 1n);
check(-(2n ** 127n));

// For every length: just below, on and just above a tie, with the kept part even and odd.
for (let bits = 2; bits <= 128; bits++) {
    for (const digits of [24, 53]) {
        if (bits <= digits + 1)
            continue;
        const unit = 1n << BigInt(bits - digits);
        const half = unit >> 1n;
        const smallest = 1n << BigInt(digits - 1);
        for (const kept of [smallest, smallest + 1n, 2n * smallest - 2n, 2n * smallest - 1n]) {
            for (const dropped of [half - 1n, half, half + 1n, 1n, unit - 1n]) {
                const value = kept * unit + dropped;
                check(value);
                if (value <= 2n ** 127n)
                    check(-value);
            }
        }
    }
}

// A fixed sequence of values of every length (xorshift64).
let state = 88172645463325252n;
function random64() {
    state ^= BigInt.asUintN(64, state << 13n);
    state ^= state >> 7n;
    state ^= BigInt.asUintN(64, state << 17n);
    return state;
}
for (let i = 0; i < testLoopCount; i++) {
    const value = ((random64() << 64n) | random64()) >> BigInt(i % 128);
    check(value);
    if (value <= 2n ** 127n)
        check(-value);
}
