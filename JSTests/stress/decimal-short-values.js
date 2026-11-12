//@ requireOptions("--useDecimal=1")
// What optimized code computes without a call, at the limits of that, against BigInt.

const parts = [];
const add = (coefficient, exponent) => parts.push([coefficient, exponent], [-coefficient, exponent]);
for (let power = 0n; power <= 19n; ++power) {
    for (const bits of [62n, 63n, 64n]) {
        const limit = (1n << bits) / 10n ** power;
        for (const step of [-1n, 0n, 1n]) {
            if (limit + step > 0n)
                add(limit + step, -Number(power % 3n));
        }
    }
    add(1n, Number(power));
    add(3n, -Number(power));
    add(5n * 10n ** power + 5n, -Number(power) - 1);
    add(10n ** power - 1n || 7n, -2);
}
for (const coefficient of [2n, 4n, 5n, 15n, 25n, 35n, 45n, 125n, 251n, 249n, 995n, 4294967295n, 4294967296n, 4294967297n, (1n << 64n) + 1n])
    add(coefficient, -1), add(coefficient, 0), add(coefficient, -20), add(coefficient, 20);
const values = parts.map(([coefficient, exponent]) => new Decimal(`${coefficient}e${exponent}`));

// The text is rounded as the result of an operation is.
const exact = (coefficient, exponent) => new Decimal(`${coefficient}e${exponent}`).toExponential();
function aligned([x, xExponent], [y, yExponent]) {
    const unit = Math.min(xExponent, yExponent);
    return [x * 10n ** BigInt(xExponent - unit), y * 10n ** BigInt(yExponent - unit), unit];
}
const model = {
    add(a, b) { const [x, y, unit] = aligned(a, b); return exact(x + y, unit); },
    subtract(a, b) { const [x, y, unit] = aligned(a, b); return exact(x - y, unit); },
    multiply(a, b) { return exact(a[0] * b[0], a[1] + b[1]); },
    compare(a, b) { const [x, y] = aligned(a, b); return (x > y) - (x < y); },
    lessThan(a, b) { const [x, y] = aligned(a, b); return x < y; },
    lessThanOrEqual(a, b) { const [x, y] = aligned(a, b); return x <= y; },
    greaterThan(a, b) { const [x, y] = aligned(a, b); return x > y; },
    greaterThanOrEqual(a, b) { const [x, y] = aligned(a, b); return x >= y; },
};
const text = value => value instanceof Decimal ? value.toExponential() : value;
function compile(...source) {
    const f = new Function(...source);
    noInline(f);
    for (let i = 0; i < testLoopCount * 2; ++i)
        f(values[i % values.length], values[(i * 7) % values.length]);
    return f;
}
function viaCall(method, a, b) { return Decimal.prototype[method].call(a, b); }
noInline(viaCall);
noDFG(viaCall);

// 0, what is not finite, and the ends of the exponents.
const special = ["0", "-0", "NaN", "Infinity", "-Infinity", "1", "-1", "9e6144", "-9e6144", "1e6144", "4611686018427387903e6126", "4611686018427387903e6111", "7e6111", "7e6112", "7e6129", "7e6130",
    "1e-6176", "-1e-6176", "3e-6158", "3e-6157", "3e-3088", "3e-3089", "7e3055", "7e3056", "18446744073709551615e3063", "1e18", "1e19", "1e-18", "1e-19"].map(value => new Decimal(value));
const withSign = value => value instanceof Decimal ? value.toExponential() : String(value);

const stride = Math.max(1, Math.floor(values.length * values.length / (testLoopCount * 4)));
for (const method in model) {
    const f = compile("a", "b", `return a.${method}(b);`);
    const check = (i, j) => {
        const actual = text(f(values[i], values[j])), expected = model[method](parts[i], parts[j]);
        if (actual !== expected)
            throw new Error(`${values[i]}.${method}(${values[j]}): expected ${expected}, got ${actual}`);
    };
    for (let pair = 0; pair < values.length * values.length; pair += stride)
        check(pair % values.length, Math.floor(pair / values.length));
    for (let i = 0; i < values.length; ++i) {
        for (const distance of [0, 1, 2, 3])
            check(i, (i + distance) % values.length);
    }
    for (const a of special) {
        for (const b of special) {
            if (withSign(f(a, b)) !== withSign(viaCall(method, a, b)))
                throw new Error(`${withSign(a)}.${method}(${withSign(b)}): got ${withSign(f(a, b))}`);
        }
    }
}

function round([coefficient, exponent], digits, mode) {
    if (exponent >= -digits)
        return exact(coefficient, exponent);
    const divisor = 10n ** BigInt(-digits - exponent), isNegative = coefficient < 0n, magnitude = isNegative ? -coefficient : coefficient;
    const quotient = magnitude / divisor, twice = magnitude % divisor * 2n;
    const up = twice && { ceil: !isNegative, floor: isNegative, trunc: false, halfExpand: twice >= divisor, halfEven: twice > divisor || (twice === divisor && quotient % 2n === 1n) }[mode];
    return exact((quotient + (up ? 1n : 0n)) * (isNegative ? -1n : 1n), -digits);
}
function roundViaCall(value, digits, mode) { return Decimal.prototype.round.call(value, digits, mode); }
noInline(roundViaCall);
noDFG(roundViaCall);
for (const mode of ["ceil", "floor", "trunc", "halfExpand", "halfEven"]) {
    const counts = [-21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 19, 20];
    const f = compile("a", `return [${counts.map(digits => `a.round(${digits}, "${mode}")`)}];`);
    values.forEach((value, i) => {
        f(value).forEach((result, j) => {
            const expected = round(parts[i], counts[j], mode);
            if (result.toExponential() !== expected)
                throw new Error(`${value}.round(${counts[j]}, ${mode}): expected ${expected}, got ${result.toExponential()}`);
        });
    });

    const extremes = [0, -1, 6175, 6176, 6177, -6111, -6112, -6130, -6144, -6145, 2147483647, -2147483648];
    const g = compile("a", `return [${extremes.map(digits => `a.round(${digits}, "${mode}")`)}];`);
    for (const value of special) {
        g(value).forEach((result, j) => {
            if (withSign(result) !== withSign(roundViaCall(value, extremes[j], mode)))
                throw new Error(`${withSign(value)}.round(${extremes[j]}, ${mode}): got ${withSign(result)}`);
        });
    }
}
