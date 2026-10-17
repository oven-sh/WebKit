//@ slow!
// Exercises the Burnikel-Ziegler and Barrett division paths around their divisor-size thresholds
// (57 and 13000 digits), with dividends of one to many divisor lengths. Each quotient and
// remainder is checked against x == q * y + r with 0 <= r < y, which relies on the multiplication
// paths but shares no division code, and the quotient of exact multiples is checked directly.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected.toString(16).slice(0, 40)}... but got ${actual.toString(16).slice(0, 40)}...`);
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
            // Only the top digit is set, with its high bit, so the divisor needs no normalization
            // shift and the dividend's top block is maximal.
            digit = i ? 0n : 0x8000000000000000n;
            break;
        case "low":
            // A small top digit forces the largest normalization shift.
            digit = i ? mix : 1n;
            break;
        }
        parts[i] = digit.toString(16).padStart(16, "0");
    }
    if (shape === "random" || shape === "sparse")
        parts[0] = "8" + parts[0].slice(1);
    return BigInt("0x" + parts.join(""));
}

function check(x, y, message) {
    const q = x / y;
    const r = x % y;
    if (r < 0n || r >= y)
        throw new Error(`${message}: remainder out of range`);
    shouldBe(q * y + r, x, `${message} identity`);
    shouldBe((-x) / (-y), q, `${message} negative operands quotient`);
    shouldBe((-x) % y, -r, `${message} negative dividend remainder`);
}

const shapes = ["random", "ones", "sparse", "top", "low"];

// Divisor sizes around the Burnikel-Ziegler threshold and its power-of-two block rounding, with
// dividends from one digit longer up to many blocks.
for (const divisorSize of [56, 57, 58, 113, 114, 115, 127, 128, 129, 228, 229, 456, 457]) {
    for (const extra of [1, 2, 57, 58, 114, 115, 500]) {
        const dividendSize = divisorSize + extra;
        for (const shape of shapes) {
            const x = makeOperand(dividendSize, dividendSize, shape);
            const y = makeOperand(divisorSize, divisorSize * 3 + 1, shapes[(shapes.indexOf(shape) + 1) % shapes.length]);
            check(x, y, `${dividendSize} / ${divisorSize} ${shape}`);
        }
    }
}

// Divisor sizes around the Barrett threshold, where the dividend is at most twice the divisor,
// exactly twice, and chunked beyond that.
for (const [divisorSize, extra, shape] of [[12999, 13000, "random"], [13000, 1, "low"], [13000, 13001, "ones"], [13001, 27000, "random"]]) {
    const dividendSize = divisorSize + extra;
    const x = makeOperand(dividendSize, dividendSize, shape);
    const y = makeOperand(divisorSize, divisorSize * 3 + 1, shapes[(shapes.indexOf(shape) + 2) % shapes.length]);
    check(x, y, `${dividendSize} / ${divisorSize} ${shape}`);
}

// Exact multiples, and the remainders 1 and y - 1, with quotients of various sizes.
for (const divisorSize of [57, 128, 13001]) {
    const y = makeOperand(divisorSize, divisorSize, "random");
    for (const quotientSize of [1, 2, 57, 300]) {
        const q = makeOperand(quotientSize, quotientSize * 7, "sparse");
        for (const r of [0n, 1n, y - 1n]) {
            const x = q * y + r;
            shouldBe(x / y, q, `${quotientSize} x ${divisorSize} + ${r === 0n ? "0" : r === 1n ? "1" : "y - 1"} quotient`);
            shouldBe(x % y, r, `${quotientSize} x ${divisorSize} + ${r === 0n ? "0" : r === 1n ? "1" : "y - 1"} remainder`);
        }
    }
}

// Powers of two as divisors and dividends.
for (const bits of [64 * 57, 64 * 1000 + 1]) {
    const p = 1n << BigInt(bits);
    const x = makeOperand(Math.ceil(bits / 64) * 2 + 3, bits, "random");
    shouldBe(x / p, x >> BigInt(bits), `${bits} bit power of two divisor`);
    shouldBe(x % p, x & (p - 1n), `${bits} bit power of two remainder`);
    shouldBe(x / (p - 1n) * (p - 1n) + x % (p - 1n), x, `${bits} bit all ones divisor`);
    shouldBe((p * p) / p, p, `${bits} bit power of two dividend`);
    shouldBe((p * p - 1n) / p, p - 1n, `${bits} bit all ones dividend`);
    shouldBe((p * p - 1n) % p, p - 1n, `${bits} bit all ones dividend remainder`);
}
