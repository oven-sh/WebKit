//@ slow!
// Exercises the divide-and-conquer toString for non-power-of-two radixes against a reference
// that only ever divides by single digits, around the 14-digit threshold at which it takes over
// from the schoolbook loop and across its recursion levels. Every radix is covered so the level
// divisors, which are powers of the chunk divisor, start from a different digit count each time.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected.slice(0, 40)}... (${expected.length} chars) but got ${actual.slice(0, 40)}... (${actual.length} chars)`);
}

// Repeated division by radix^9, which stays on the single-digit division path.
function refToString(x, radix) {
    if (x === 0n)
        return "0";
    const sign = x < 0n;
    if (sign)
        x = -x;
    const chunk = BigInt(radix) ** 9n;
    const parts = [];
    while (x > 0n) {
        let part = (x % chunk).toString(radix);
        x /= chunk;
        if (x > 0n)
            part = part.padStart(9, "0");
        parts.push(part);
    }
    return (sign ? "-" : "") + parts.reverse().join("");
}

function makeOperand(digits, seed, shape) {
    const parts = new Array(digits);
    let mix = BigInt.asUintN(64, 0x9e3779b97f4a7c15n * BigInt(seed + 1));
    for (let i = 0; i < digits; i++) {
        mix = BigInt.asUintN(64, mix * 6364136223846793005n + 1442695040888963407n);
        let digit;
        switch (shape) {
        case "random":
            digit = mix;
            break;
        case "ones":
            digit = 0xffffffffffffffffn;
            break;
        case "sparse":
            digit = (i * 7 + seed) % 5 === 0 ? mix : 0n;
            break;
        case "top":
            digit = i ? 0n : 1n;
            break;
        }
        parts[i] = digit.toString(16).padStart(16, "0");
    }
    if (shape === "random" || shape === "sparse")
        parts[0] = "8" + parts[0].slice(1);
    return BigInt("0x" + parts.join(""));
}

// Each radix gets the sizes around the threshold and around the doublings of its own level
// divisors, whose digit counts differ by radix. The reference conversion is quadratic, so the
// larger sizes are sampled.
const radixes = [3, 5, 7, 10, 12, 17, 25, 36];
const shapes = ["random", "ones", "sparse", "top"];

for (const digits of [13, 14, 15, 16, 27, 28, 29, 55, 56, 57, 58, 113, 114, 115, 227, 228, 229, 455, 456, 457]) {
    for (const shape of (digits < 100 ? shapes : [shapes[digits % shapes.length]])) {
        const x = makeOperand(digits, digits, shape);
        for (const radix of (digits < 100 ? radixes : [radixes[digits % radixes.length], 10])) {
            shouldBe(x.toString(radix), refToString(x, radix), `${digits} digits ${shape} radix ${radix}`);
            if (shape === "random")
                shouldBe((-x).toString(radix), refToString(-x, radix), `-${digits} digits radix ${radix}`);
        }
    }
}

// Chunks that equal a level divisor, or sit just below or above one, take the special cases in
// the recursion. Powers of the radix land exactly on the divisors.
for (const radix of [3, 7, 10, 36]) {
    for (const exponent of [19, 20, 38, 39, 76, 77, 152, 153, 304, 305, 608, 609, 1216, 1217, 2432, 2433]) {
        const power = BigInt(radix) ** BigInt(exponent);
        for (const x of [power, power - 1n, power + 1n, 2n * power, 2n * power - 1n, power * power, power * power - 1n, power * (power - 1n)])
            shouldBe(x.toString(radix), refToString(x, radix), `${radix} ** ${exponent} neighbour`);
    }
}

// Decimal strings with long runs of zeros, which the level recursion has to fill in.
for (const exponent of [100, 1000, 10000]) {
    const power = 10n ** BigInt(exponent);
    shouldBe(power.toString(), "1" + "0".repeat(exponent), `10 ** ${exponent}`);
    shouldBe((power + 1n).toString(), "1" + "0".repeat(exponent - 1) + "1", `10 ** ${exponent} + 1`);
    shouldBe((power - 1n).toString(), "9".repeat(exponent), `10 ** ${exponent} - 1`);
    shouldBe((power * power + power).toString(), "1" + "0".repeat(exponent - 1) + "1" + "0".repeat(exponent), `10 ** ${2 * exponent} + 10 ** ${exponent}`);
}

// A large value whose decimal form is checked by parsing it back.
{
    const x = makeOperand(20000, 20000, "random");
    const string = x.toString();
    shouldBe(String(BigInt(string)), string, "20000 digit decimal round trip");
    if (BigInt(string) !== x)
        throw new Error("20000 digit decimal round trip value");
}
