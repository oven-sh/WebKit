// Converting a string of binary, octal, hex or base-4/32 digits to a Number must give the double nearest to
// the exact value (ties to even), like a decimal string does. This covers parseInt with a power-of-two radix,
// Number() / ToNumber on "0x" / "0o" / "0b" strings, and numeric literals the lexer reads.
// Number(BigInt) is the reference: BigInt parses the digits exactly and its ToNumber rounds once.

function shouldBe(actual, expected, message) {
    if (!Object.is(actual, expected))
        throw new Error("FAIL " + message + ": expected " + expected + " but got " + actual);
}

const prefix = { 2: "0b", 8: "0o", 16: "0x" };

function expected(digits, radix) {
    if (radix in prefix)
        return Number(BigInt(prefix[radix] + digits));
    let value = 0n;
    for (let c of digits)
        value = value * BigInt(radix) + BigInt(parseInt(c, radix));
    return Number(value);
}

function check(digits, radix) {
    let want = expected(digits, radix);
    shouldBe(parseInt(digits, radix), want, "parseInt(\"" + digits + "\", " + radix + ")");
    shouldBe(parseInt("-" + digits, radix), -want, "parseInt(\"-" + digits + "\", " + radix + ")");
    shouldBe(parseInt(" \u3000" + digits + "\u{1F600}", radix), want, "parseInt 16-bit string, radix " + radix);
    if (!(radix in prefix))
        return;
    let literal = prefix[radix] + digits;
    shouldBe(Number(literal), want, "Number(\"" + literal + "\")");
    shouldBe(+(literal + "\u3000"), want, "ToNumber 16-bit \"" + literal + "\"");
    shouldBe(literal - 0, want, "\"" + literal + "\" - 0");
    shouldBe(eval(literal), want, "literal " + literal);
    shouldBe(eval(literal.toUpperCase()), want, "literal " + literal.toUpperCase());
    shouldBe(new Function("return " + literal + ";")(), want, "new Function literal " + literal);
    shouldBe(eval("'\u3000', " + literal), want, "literal in 16-bit source " + literal);
    let separated = prefix[radix] + digits.replace(/(.)(?=.)/g, "$1_");
    shouldBe(eval(separated), want, "literal " + separated);
    if (radix == 16)
        shouldBe(parseInt(literal), want, "parseInt(\"" + literal + "\")");
    if (radix == 8)
        shouldBe(eval("0" + digits), want, "legacy octal literal 0" + digits);
}

// The values from the original report. Each was one ulp too high.
shouldBe(parseInt("97b89d834f82bbef", 16), 10932661282742122496, "reported hex");
shouldBe(Number("0x97b89d834f82bbef"), 10932661282742122496, "reported Number hex");
shouldBe(eval("0x97b89d834f82bbef"), 10932661282742122496, "reported hex literal");
shouldBe(parseInt("1700774500551360345014", 8), 17311718268872018432, "reported octal");
shouldBe(parseInt("1kqg9a599t95", 32), 59479501168768296, "reported base 32");

// Short values that come out one ulp off when the digits are accumulated in a double, because two of the
// additions round. 2^55 + 2^53 + 5 is 45035996273704968, not ...960.
check("10100000000000000000000000000000000000000000000000000101", 2);
check("2200000000000000000000000011", 4);
check("3500000000000000005", 8);
check("130000000000009", 16);
check("16000000000000a", 16);
check("1i000000000r", 32);
shouldBe(Number("0b10100000000000000000000000000000000000000000000000000101"), 45035996273704968, "2^55 + 2^53 + 5");
shouldBe(eval("0x16000000000000a"), 99079191802150928, "0x16000000000000a");

// 2^53 + 1 is the first integer a double cannot hold. It is a tie and rounds down to even; 2^53 + 3 rounds up.
check("20000000000001", 16);
check("20000000000003", 16);
check("100000000000000000000000000000000000000000000000000001", 2);
check("100000000000000000000000000000000000000000000000000011", 2);
check("400000000000000001", 8);
check("80000000002", 32);
// A tie broken by a non-zero digit far to the right.
check("20000000000001000000000001", 16);
check("2000000000000100000000000", 16);
// Rounding up carries into the next power of two.
check("3fffffffffffffff", 16);
check("ffffffffffffffff", 16);
check("1777777777777777777777", 8);
check("vvvvvvvvvvvvv", 32);
check("3333333333333333333333333333333", 4);
// Leading zeros do not count towards the precision.
check("000000000000000000000097b89d834f82bbef", 16);
check("0000000000000000000000000000000000000000000000000000001700774500551360345014", 8);

// The largest double, the first value that rounds to Infinity (a tie with an odd significand below), and the
// value just under that tie.
let maxDigits = "fffffffffffff8" + "0".repeat(242);
shouldBe(expected(maxDigits, 16), Number.MAX_VALUE, "reference sanity");
check(maxDigits, 16);
check("fffffffffffffc" + "0".repeat(242), 16);
shouldBe(parseInt("fffffffffffffc" + "0".repeat(242), 16), Infinity, "tie above Number.MAX_VALUE");
check("fffffffffffffb" + "f".repeat(242), 16);
shouldBe(parseInt("fffffffffffffb" + "f".repeat(242), 16), Number.MAX_VALUE, "just under the tie");
check("1" + "0".repeat(256), 16);
shouldBe(Number("0x1" + "0".repeat(256)), Infinity, "2^1024");
check("1" + "0".repeat(1024), 2);
check("1" + "0".repeat(5000), 16);
check("1" + "7".repeat(400), 8);

// A deterministic sweep of 54- to 70-bit values in every power-of-two radix.
let state = 12345;
function random32() {
    state = (state + 0x6D2B79F5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return (t ^ (t >>> 14)) >>> 0;
}
for (let i = 0; i < 2000; ++i) {
    let bits = 54 + (i % 17);
    let value = 0n;
    for (let b = 0; b < bits; b += 32)
        value = (value << 32n) | BigInt(random32());
    value &= (1n << BigInt(bits)) - 1n;
    value |= 1n << BigInt(bits - 1);
    for (let radix of [2, 4, 8, 16, 32]) {
        let digits = value.toString(radix);
        let want = Number(value);
        shouldBe(parseInt(digits, radix), want, "sweep parseInt(\"" + digits + "\", " + radix + ")");
        if (radix in prefix) {
            shouldBe(Number(prefix[radix] + digits), want, "sweep Number(\"" + prefix[radix] + digits + "\")");
            if (i % 50 == 0)
                shouldBe(eval(prefix[radix] + digits), want, "sweep literal " + prefix[radix] + digits);
        }
    }
}

// parseInt through the DFG, which has its own call to the same parser.
function parseHex(s) { return parseInt(s, 16); }
noInline(parseHex);
function parseRadix(s, r) { return parseInt(s, r); }
noInline(parseRadix);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(parseHex("97b89d834f82bbef"), 10932661282742122496, "DFG parseInt hex");
    shouldBe(parseRadix("1700774500551360345014", 8), 17311718268872018432, "DFG parseInt octal");
    shouldBe(parseRadix("zz", 36), 1295, "DFG parseInt radix 36");
}

// Radixes that are not a power of two may approximate above 2^53 (the spec allows it), but stay exact below.
shouldBe(parseInt("2gosa7pa2gv", 36), 2 ** 53 - 1, "radix 36 below 2^53");
shouldBe(parseInt("-2gosa7pa2gv", 36), -(2 ** 53 - 1), "radix 36 negative");
