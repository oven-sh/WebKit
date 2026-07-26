//@ runDefault

// Exercise the Karatsuba, Burnikel-Ziegler and divide-and-conquer toString
// paths in JSBigInt against the unchanged power-of-two toString and BigInt
// parser. Inputs are chosen to straddle the 32/64-digit algorithm
// thresholds as well as values with long zero runs.

function assert(c, m) {
    if (!c)
        throw new Error(m);
}

let seed = 0x2b992ddfa23249d6n;
function rand64() {
    seed = (seed * 6364136223846793005n + 1442695040888963407n) & 0xffffffffffffffffn;
    return seed;
}
function randBig(words) {
    let r = 0n;
    for (let i = 0; i < words; i++)
        r = (r << 64n) | rand64();
    return r;
}

// toString(16) uses toStringBasePowerOfTwo which is untouched by this
// change, so a hex round-trip is an independent oracle.
for (const words of [1, 2, 31, 32, 33, 63, 64, 65, 100, 300, 1000, 4000]) {
    for (let i = 0; i < 4; i++) {
        const b = randBig(words);
        const s = b.toString();
        assert(BigInt(s) === b, "dec roundtrip words=" + words);
        assert((-b).toString() === "-" + s, "neg words=" + words);
        assert(BigInt("0x" + b.toString(16)) === b, "hex words=" + words);
    }
}

function parseInRadix(s, r) {
    let v = 0n;
    const R = BigInt(r);
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        v = v * R + BigInt(c < 58 ? c - 48 : c - 87);
    }
    return v;
}
for (const r of [3, 5, 7, 9, 11, 13, 23, 35, 36]) {
    for (const words of [1, 33, 64, 65, 200, 2000]) {
        const b = randBig(words);
        assert(parseInRadix(b.toString(r), r) === b, "radix " + r + " words=" + words);
    }
}

// Karatsuba multiplication: algebraic identities with operands that cross
// the threshold, including very unbalanced pairs.
for (const words of [20, 33, 64, 128, 300, 1000]) {
    for (let i = 0; i < 3; i++) {
        const a = randBig(words);
        const b = randBig(words);
        assert(a * b === b * a, "commut words=" + words);
        assert((a + b) * (a + b) === a * a + 2n * a * b + b * b, "square words=" + words);
        assert((a + 1n) * (a - 1n) === a * a - 1n, "diffsq words=" + words);
    }
    // Unbalanced: one operand above, one below the threshold.
    const big = randBig(words);
    const small = randBig(3);
    assert(big * small === small * big, "unbal words=" + words);
    assert((big * small).toString(16) === (small * big).toString(16), "unbal hex");
}

// Values with long runs of zero digits exercise the remainder-is-zero and
// normalized-span handling inside Burnikel-Ziegler.
for (const e of [1200, 1234, 2000, 5000, 20000, 80000]) {
    assert((10n ** BigInt(e)).toString() === "1" + "0".repeat(e), "pow10 " + e);
    assert((10n ** BigInt(e) - 1n).toString() === "9".repeat(e), "pow10-1 " + e);
}
for (const e of [2000, 10000, 100000, 500000]) {
    const b = 1n << BigInt(e);
    assert(BigInt(b.toString()) === b, "pow2 " + e);
}
