//@ requireOptions("--useDecimal=1")
// new Decimal(number) has the digits of the text of the number, and Number(decimal) is the number of the text of the decimal.

const bits = new BigUint64Array(1);
const doubles = new Float64Array(bits.buffer);
let state = 0x9E3779B97F4A7C15n;
const random = () => state = BigInt.asUintN(64, state * 6364136223846793005n + 1442695040888963407n);

function check(number) {
    const decimal = new Decimal(number);
    const expected = new Decimal(String(number)).toString();
    if (decimal.toString() !== expected)
        throw new Error(`${number}: expected ${expected}, got ${decimal}`);
    if (!Object.is(Number(decimal), number + 0))
        throw new Error(`${number}: got back ${Number(decimal)}`);
}

function checkText(text) {
    if (!Object.is(Number(new Decimal(text)), Number(text)))
        throw new Error(`${text}: expected ${Number(text)}, got ${Number(new Decimal(text))}`);
}

// Every exponent, with the least and the greatest significands.
for (let exponent = 0n; exponent < 2048n; ++exponent) {
    for (const significand of [0n, 1n, 2n, (1n << 51n), (1n << 52n) - 1n]) {
        bits[0] = exponent << 52n | significand;
        check(doubles[0]);
        check(-doubles[0]);
    }
}
for (let exponent = -330; exponent <= 310; ++exponent) {
    for (const digits of ["1", "5", "9007199254740991", "9007199254740993", "9999999999999999999", "18446744073709551615", "18446744073709551616", "2470328229206232720882843964341106", "1797693134862315807937289714053034"]) {
        check(Number(`${digits}e${exponent}`));
        checkText(`${digits}e${exponent}`);
        checkText(`-${digits}e${exponent}`);
    }
}
for (const text of ["0", "-0", "1e6144", "-1e6144", "1e-6176", "-1e-6176", "NaN", "Infinity", "-Infinity"])
    checkText(text);
for (let i = 0; i < testLoopCount; ++i) {
    bits[0] = random();
    check(doubles[0]);
    bits[0] >>= 12n;
    check(doubles[0]);
    check(Number(random() >> 40n) / 100);
    checkText(`${random() >> (random() >> 58n)}e${Number(random() >> 54n) % 700 - 350}`);
}
