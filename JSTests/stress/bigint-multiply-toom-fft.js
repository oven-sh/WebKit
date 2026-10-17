//@ slow!
// Exercises the Toom-3 and FFT multiplication paths around their size thresholds, and the
// unbalanced shapes that are chunked or padded. Products are checked against the division paths
// (p / y == x, p % y == 0), which share no code with multiplication above the schoolbook base
// case, and against a schoolbook reference built from single-digit products where that is cheap.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected.toString(16).slice(0, 40)}... but got ${actual.toString(16).slice(0, 40)}...`);
}

function refMul(a, b) {
    let result = 0n;
    let shift = 0n;
    while (b > 0n) {
        const chunk = b & 0xffffffffn;
        if (chunk)
            result += (a * chunk) << shift;
        b >>= 32n;
        shift += 32n;
    }
    return result;
}

// Deterministic operands: digit i is a linear congruential mix of the seed, built through hex
// strings so that constructing a million-digit operand stays linear.
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
        case "halves":
            // The low half of the operand is zero, so Karatsuba's and Toom's differences
            // normalize to nothing.
            digit = i < digits / 2 ? 0n : mix;
            break;
        }
        parts[i] = digit.toString(16).padStart(16, "0");
    }
    if (shape !== "ones")
        parts[0] = "8" + parts[0].slice(1);
    return BigInt("0x" + parts.join(""));
}

function check(x, y, message) {
    const p = x * y;
    shouldBe(y * x, p, `${message} commutes`);
    shouldBe(p / y, x, `${message} quotient`);
    shouldBe(p % y, 0n, `${message} remainder`);
    shouldBe((p + y - 1n) / y, x, `${message} quotient of p + y - 1`);
    shouldBe((p + y - 1n) % y, y - 1n, `${message} remainder of p + y - 1`);
    shouldBe((-x) * y, -p, `${message} sign`);
    return p;
}

const shapes = ["random", "ones", "sparse", "halves"];

// Balanced sizes across the Karatsuba, Toom-3 and FFT crossovers, with a schoolbook reference.
for (const size of [479, 480, 481, 482, 700, 1149, 1150, 1151, 1152]) {
    for (const shape of shapes) {
        const x = makeOperand(size, size, shape);
        const y = makeOperand(size, size * 3 + 1, shapes[(shapes.indexOf(shape) + 1) % shapes.length]);
        const p = check(x, y, `${size} x ${size} ${shape}`);
        if (shape === "random" && size < 600)
            shouldBe(p, refMul(x, y), `${size} x ${size} reference`);
        shouldBe((x + 1n) * (x + 1n) - x * x, 2n * x + 1n, `${size} square ${shape}`);
    }
}

// Toom-3 pads a moderately longer x rather than chunking it; the ratio 5 : 3 is the boundary.
for (const [larger, smaller] of [[799, 480], [800, 480], [801, 480], [960, 480], [961, 480], [1440, 480], [1441, 480], [2000, 1000]]) {
    for (const shape of shapes) {
        const x = makeOperand(larger, larger + smaller, shape);
        const y = makeOperand(smaller, larger * smaller, shapes[(shapes.indexOf(shape) + 2) % shapes.length]);
        check(x, y, `${larger} x ${smaller} ${shape}`);
    }
}

// FFT is chosen on the sum of the sizes once the smaller one is wide enough, and with a very
// long x it proceeds in y-sized chunks. The chunked shapes reach into the millions of bits.
for (const [larger, smaller] of [[1700, 600], [1701, 600], [1700, 599], [5000, 600], [115001, 1150], [115001, 1149]]) {
    const x = makeOperand(larger, larger, "random");
    const y = makeOperand(smaller, smaller, "ones");
    const p = x * y;
    shouldBe(y * x, p, `${larger} x ${smaller} commutes`);
    // y is 2^bits - 1, so the product is x << bits minus x.
    shouldBe(p + x, x << BigInt(smaller * 64), `${larger} x ${smaller} value`);
}

// Squaring takes its own FFT path.
for (const size of [1150, 2300, 4096, 30000]) {
    const x = makeOperand(size, size, "random");
    const square = x * x;
    shouldBe(square % x, 0n, `${size} square remainder`);
    shouldBe((x + 1n) * (x + 1n) - square, 2n * x + 1n, `${size} square identity`);
}

// Powers of two and their neighbours have sparse transforms.
for (const bits of [1 << 16, (1 << 20) + 1]) {
    const p = 1n << BigInt(bits);
    shouldBe((p + 1n) * (p - 1n), p * p - 1n, `${bits} bit power of two`);
    shouldBe((p - 1n) * (p - 1n), p * p - 2n * p + 1n, `${bits} bit all ones`);
}
