//@ requireOptions("--useDecimal=1")
// Optimized code multiplies where a constant has a reciprocal, and rounds a quotient without all of its digits.

function viaCall(method, receiver, ...rest) { return Decimal.prototype[method].call(receiver, ...rest); }
noInline(viaCall);
noDFG(viaCall);
function shouldBe(actual, expected, what) {
    if (actual.toExponential() !== expected.toExponential())
        throw new Error(`${what}: expected ${expected.toExponential()}, got ${actual.toExponential()}`);
}

const values = ["0", "-0", "NaN", "Infinity", "-Infinity", "1", "-1", "3", "-7", "19.99", "100", "0.005", "-0.015", "2.5", "1234.56", "1e-6176", "-3e-6176", "5e-6170", "9e6144", "7e6111", "1e30",
    "18446744073709551615", "18446744073709551616", "-9223372036854775808", "9999999999999999999999999999999999", "1.234567890123456789012345678901234"].map(text => new Decimal(text));

// With a reciprocal of up to 64 bits, with a longer one, with one out of the domain, with none, and with one that only seems exact.
for (const text of ["1", "-1", "2", "-4", "5", "8", "10", "100", "1e-3", "0.5", "0.25", "-0.125", "1024", "134217728", "268435456", "7450580596923828125", "1e6144", "1e-6144", "1e-6145", "1e-6176", "2e6144", "5e-6176",
    "3", "12", "365", "99999999999999999", "9.9999999999999998e25", "0", "-0", "NaN", "Infinity", "-Infinity", "37252902984619140625"]) {
    const divisor = new Decimal(text);
    const ofLiteral = new Function("x", `return [x.divide(new Decimal("${text}")), x.divide(new Decimal("${text}")).round(2), x.divide(new Decimal("${text}")).add(x)];`);
    const ofConstant = new Function("divisor", `return x => [x.divide(divisor), x.divide(divisor).round(2), x.divide(divisor).add(x)]; // ${text}`)(divisor);
    // In another block the parser does not know what the variable holds.
    const ofVariable = new Function("constant", `return x => { const divisor = constant; if (x.isNaN()) x = x.negate(); return [x.divide(divisor), x.divide(divisor).round(2), x.divide(divisor).add(x)]; }; // ${text}`)(divisor);
    for (const f of [ofLiteral, ofConstant, ofVariable]) {
        noInline(f);
        for (let i = 0; i < testLoopCount; ++i)
            f(values[i % values.length]);
        for (const x of values) {
            const quotient = viaCall("divide", x, divisor), [alone, rounded, added] = f(x), what = `${x.toExponential()} / ${text}`;
            shouldBe(alone, quotient, what);
            shouldBe(rounded, viaCall("round", quotient, 2), what);
            shouldBe(added, viaCall("add", quotient, x), what);
        }
    }
}

const cases = [];
for (const x of values) {
    for (const y of values) {
        for (const digits of [-6112, -6111, -20, -1, 0, 2, 13, 14, 19, 20, 33, 6156, 6157, 6176, 6177])
            cases.push([x, y, digits]);
    }
}
for (const [x, y, digits] of [
    // The first rounding changes what the second one sees.
    ["-3.000000000000000009e3074", "1.000000000000000001e26", -3030], ["-1.00000000000000002e2", "4.99999999999999999e2", 19], ["-5.000000000000000001e72", "-1.000000000000000001e91", 37],
    ["-1.2e-68", "1.5625e6109", 6164], ["-3.02e-6172", "4.487919354e10", 6176], ["-8.07e-3105", "4.9e3080", 6169],
    ["8.6244175119645893e5239", "-3.58533889e-3113", -8353], ["-8e229", "-6.5506e-6145", -6392],
    // The divisor or the quotient leaves 64 bits in units of the result.
    ["2.65247120074183479e-4", "1.0000000000000002e5", 3], ["-1.000000000000000001e-60", "-5e2", 59], ["18446744073709551615", "1", 1], ["18446744073709551615", "3", 1],
    ["99999999999999", "1", 0], ["299999999999999", "3", 0], ["300000000000001", "3", 0], ["1", "8", 2], ["3", "8", 2], ["-1", "8", 2], ["1", "200", 2], ["3", "200", 2],
])
    cases.push([new Decimal(x), new Decimal(y), digits]);

for (const mode of ["ceil", "floor", "trunc", "halfExpand", "halfEven"]) {
    const alone = new Function("x", "y", "digits", `return x.divide(y).round(digits, "${mode}");`);
    const withQuotient = new Function("x", "y", "digits", `const quotient = x.divide(y); return [quotient.round(digits, "${mode}"), quotient.round(2, "${mode}"), quotient];`);
    noInline(alone);
    noInline(withQuotient);
    for (let i = 0; i < testLoopCount; ++i) {
        alone(...cases[i % cases.length]);
        withQuotient(...cases[i % cases.length]);
    }
    for (const [x, y, digits] of cases) {
        const quotient = viaCall("divide", x, y), what = `${x.toExponential()} / ${y.toExponential()} to ${digits} ${mode}`;
        const expected = viaCall("round", quotient, digits, mode);
        shouldBe(alone(x, y, digits), expected, what);
        const [rounded, toTwo, same] = withQuotient(x, y, digits);
        shouldBe(rounded, expected, what);
        shouldBe(toTwo, viaCall("round", quotient, 2, mode), what);
        shouldBe(same, quotient, what);
    }
}

// The code is left between the two, or by the second one.
{
    function between(x, y, n) {
        const quotient = x.divide(y);
        if (n + 1 !== 2)
            return quotient;
        return quotient.round(n);
    }
    function byCount(x, y, digits) { return x.divide(y).round(digits); }
    noInline(between);
    noInline(byCount);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(between(1m, 3m, 1), 0.3m, "between");
        shouldBe(byCount(2m, 3m, 1), 0.7m, "by count");
    }
    shouldBe(between(1m, 3m, 2147483647), 0.3333333333333333333333333333333333m, "between");
    shouldBe(byCount(2m, 3m, 2 ** 40), 0.6666666666666666666666666666666667m, "by count");
    let error;
    try {
        byCount(2m, 3m, 0.5);
    } catch (thrown) {
        error = thrown;
    }
    if (!(error instanceof RangeError))
        throw new Error(`by count: ${error}`);
}

// A constant object is an operand whatever its class or its properties are, and a receiver by what it has under the name.
{
    class Derived extends Decimal {
        divide() { return 42m; }
    }
    const derived = new Derived("4"), frozen = Object.freeze(new Decimal("8")), changed = new Decimal("16"), other = {};
    const f = x => [x.divide(derived), derived.divide(x), x.divide(frozen), frozen.divide(x), x.divide(changed), changed.divide(x)];
    const g = (x, fails) => fails ? x.divide(other) : x;
    noInline(f);
    noInline(g);
    const check = expected => f(2m).forEach((result, i) => shouldBe(result, expected[i], `constant ${i}`));
    for (let i = 0; i < testLoopCount; ++i) {
        check([0.5m, 42m, 0.25m, 4m, 0.125m, 8m]);
        let error;
        try {
            g(2m, !(i % 50));
        } catch (thrown) {
            error = thrown;
        }
        if (error instanceof TypeError !== !(i % 50))
            throw new Error(`no Decimal: ${error}`);
    }
    changed.divide = () => 43m;
    check([0.5m, 42m, 0.25m, 4m, 0.125m, 43m]);
}
