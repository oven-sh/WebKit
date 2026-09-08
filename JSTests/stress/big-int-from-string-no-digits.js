// StringToBigInt: a sign or a radix prefix must be followed by at least one digit.
// StringIntegerLiteral ::: StrWhiteSpace_opt | StrWhiteSpace_opt StrIntegerLiteral StrWhiteSpace_opt
// SignedInteger ::: DecimalDigits | + DecimalDigits | - DecimalDigits
// Only the whitespace-only string has the value 0n. "+", "-", "0x" followed by nothing
// but whitespace do not parse, so BigInt() throws a SyntaxError and the comparison
// operators treat the string as if it were NaN.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

function shouldThrowSyntaxError(string) {
    let error = null;
    try {
        BigInt(string);
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error("BigInt(" + JSON.stringify(string) + ") should throw a SyntaxError, got " + String(error));
}

const whitespace = ["", " ", "\t", "\n", "\r\n", "\u00a0", "\u2028", "\u3000", "\ufeff", " \t\n "];

const noDigits = [];
for (const body of ["+", "-", "0x", "0X", "0b", "0B", "0o", "0O"]) {
    for (const before of whitespace) {
        for (const after of whitespace)
            noDigits.push(before + body + after);
    }
}
noDigits.push("+ 1", "- 1", "0x 1", "+-", "-+", "++1", "--1", "+0x1", "-0b1", "0x+1", "0b-1");

for (const string of noDigits)
    shouldThrowSyntaxError(string);

// The same strings on the other side of ==, !=, <, <=, >, >= go through StringToBigInt too.
// The result is undefined there, which makes == false and every relational comparison false.
function compare(bigInt, string) {
    return [bigInt == string, string == bigInt, bigInt != string, string != bigInt, bigInt < string, bigInt <= string, bigInt > string, bigInt >= string, string < bigInt, string <= bigInt, string > bigInt, string >= bigInt];
}
noInline(compare);

const expectedForNoDigits = [false, false, true, true, false, false, false, false, false, false, false, false];
for (let i = 0; i < testLoopCount; ++i) {
    const string = noDigits[i % noDigits.length];
    for (const bigInt of [0n, 1n, -1n, 2n ** 64n]) {
        const actual = compare(bigInt, string);
        for (let j = 0; j < actual.length; ++j)
            shouldBe(actual[j], expectedForNoDigits[j], "compare(" + bigInt + "n, " + JSON.stringify(string) + ")[" + j + "]");
    }
}

// Controls: whitespace alone is 0n, and a sign or prefix with digits still parses with
// whitespace on both sides.
for (const string of whitespace) {
    shouldBe(BigInt(string), 0n, "BigInt(" + JSON.stringify(string) + ")");
    shouldBe(0n == string, true, "0n == " + JSON.stringify(string));
    shouldBe(1n > string, true, "1n > " + JSON.stringify(string));
}

const withDigits = [["+0", 0n], ["-0", 0n], ["+7", 7n], ["-7", -7n], ["007", 7n], ["+007", 7n], ["0x1f", 31n], ["0B101", 5n], ["0o17", 15n], ["-12345678901234567890", -12345678901234567890n]];
for (const [body, expected] of withDigits) {
    for (const before of whitespace) {
        for (const after of whitespace) {
            const string = before + body + after;
            shouldBe(BigInt(string), expected, "BigInt(" + JSON.stringify(string) + ")");
            shouldBe(expected == string, true, JSON.stringify(string) + " == " + expected + "n");
            shouldBe(expected + 1n > string, true, expected + 1n + "n > " + JSON.stringify(string));
        }
    }
}
