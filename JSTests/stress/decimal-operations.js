//@ requireOptions("--useDecimal=1")
// https://tc39.es/proposal-decimal/

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

const d = value => new Decimal(value);
const max = "9.999999999999999999999999999999999e6144";

// Arithmetic.
{
    shouldBe(d("0.1").add(d("0.2")).toString(), "0.3");
    shouldBe(d("1").divide(d("3")).toString(), "0." + "3".repeat(34));
    shouldBe(d("2").divide(d("3")).toString(), "0." + "6".repeat(33) + "7");
    shouldBe(d("46").remainder(d("10")).toString(), "6");
    shouldBe(d("-46").remainder(d("10")).toString(), "-6");
    shouldBe(d("46").remainder(d("-10")).toString(), "6");
    shouldBe(d("-4").remainder(d("2")).toString(), "-0");
    shouldBe(d("4").remainder(d("-2")).toString(), "0");
    shouldBe(d("1e6144").remainder(d("7")).toString(), String(10n ** 6144n % 7n));
    shouldBe(d("1e6144").remainder(d("9999999999999999999999999999999967")).toString(), String(10n ** 6144n % 9999999999999999999999999999999967n));
    shouldBe(d("1e6144").add(d("1e-6176")).toString(), "1e6144");
    shouldBe(d("1e6144").subtract(d("1e-6176")).toString(), "1e6144");
    shouldBe(d(max).add(d("1e6111")).toString(), "Infinity");
    shouldBe(d(max).add(d("4.9e6110")).toString(), max);
    shouldBe(d(max).multiply(d("10")).toString(), "Infinity");
    shouldBe(d("1e-6176").divide(d("2")).toString(), "0");
    shouldBe(d("-1e-6176").divide(d("2")).toString(), "-0");
    shouldBe(d("3e-6176").divide(d("2")).toString(), "2e-6176");
    shouldBe(d("1e-6176").multiply(d("1e6144")).toExponential(), "1e-32");

    const special = ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1"];
    // Where the spec agrees with Number, which is everywhere here but the sign a Number loses on its way into a Decimal.
    for (const x of special) {
        for (const y of special) {
            for (const [name, f] of [["add", (a, b) => a + b], ["subtract", (a, b) => a - b], ["multiply", (a, b) => a * b], ["divide", (a, b) => a / b], ["remainder", (a, b) => a % b]])
                shouldBe(Number(d(x)[name](d(y))), f(Number(x), Number(y)), `${x} ${name} ${y}`);
            const [a, b] = [Number(x), Number(y)];
            for (const [name, expected] of [["equals", a == b], ["lessThan", a < b], ["lessThanOrEqual", a <= b], ["greaterThan", a > b], ["greaterThanOrEqual", a >= b],
                // False of a NaN.
                ["notEquals", a < b || a > b], ["compare", a < b ? -1 : a > b ? 1 : a == b ? 0 : NaN]])
                shouldBe(d(x)[name](d(y)), expected, `${x} ${name} ${y}`);
        }
        shouldBe(Number(d(x).abs()), Math.abs(Number(x)));
        shouldBe(Number(d(x).negate()), -Number(x));
        shouldBe(d(x).isNaN(), Number.isNaN(Number(x)));
        shouldBe(d(x).isFinite(), Number.isFinite(Number(x)));
    }
    shouldBe(d("1.20").equals(d("1.2")), true);
    shouldBe(d("1e33").compare(d("1000000000000000000000000000000001")), -1);
    shouldBe(d("-1e33").compare(d("-1000000000000000000000000000000001")), 1);
}

// exponent() and mantissa().
{
    for (const [text, exponent, mantissa] of [
        ["123.45", 2, "1.2345"], ["-0.00120", -3, "-1.2"], ["1", 0, "1"], ["1e6144", 6144, "1"], ["1e-6176", -6176, "1"], [max, 6144, "9.999999999999999999999999999999999"],
        ["NaN", NaN, "NaN"], ["Infinity", Infinity, "Infinity"], ["-Infinity", Infinity, "Infinity"], ["0", -Infinity, "0"], ["-0", -Infinity, "-0"],
    ]) {
        shouldBe(d(text).exponent(), exponent, text);
        shouldBe(d(text).mantissa().toString(), mantissa, text);
    }
}

// Many live ones across collections.
{
    const kept = [];
    for (let i = 0; i < 1000; ++i)
        kept.push(d(i + "." + i));
    gc();
    kept.forEach((value, i) => shouldBe(value.equals(d(i + "." + i)), true));
}
