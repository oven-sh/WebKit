//@ requireOptions("--useDecimal=1")
// https://tc39.es/proposal-decimal/

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`expected ${errorType.name} from ${func}, got ${String(error)}`);
}

const d = value => new Decimal(value);

// Conversions.
{
    shouldThrow(() => d("1").valueOf(), TypeError);
    shouldThrow(() => JSON.stringify(d("1")), TypeError);
    shouldThrow(() => JSON.stringify({ a: [d("1")] }), TypeError);
    shouldThrow(() => +d("1"), TypeError);
    shouldThrow(() => d("1") + d("1"), TypeError);
    shouldThrow(() => d("1") * 2, TypeError);
    shouldThrow(() => d("1") < d("2"), TypeError);
    shouldThrow(() => d("1") == 1, TypeError);
    shouldThrow(() => Math.abs(d("1")), TypeError);
    shouldBe(`${d("1.50")}`, "1.5");
    shouldBe(String(d("1.50")), "1.5");

    for (const [text, expected] of [["1.5", 1.5], ["-0", -0], ["0", 0], ["NaN", NaN], ["-Infinity", -Infinity], ["1e400", Infinity], ["-1e-400", -0], ["0.1", 0.1],
        ["9007199254740993", 9007199254740992], ["9007199254740993.000000000000000001", 9007199254740994], ["5e-324", 5e-324]]) {
        shouldBe(Number(d(text)), expected, text);
        shouldBe(new Number(d(text)).valueOf(), expected, text);
    }
    class Derived extends Decimal { valueOf() { throw new Error("called"); } }
    shouldBe(Number(new Derived("2")), 2);
    shouldBe(BigInt(new Derived("2")), 2n);

    for (const [text, expected] of [["0", 0n], ["-0", 0n], ["-12", -12n], ["1.0", 1n], ["120", 120n], ["1e6144", 10n ** 6144n], ["9999999999999999999999999999999999", 10n ** 34n - 1n]])
        shouldBe(BigInt(d(text)) === expected, true, text);
    for (const text of ["NaN", "Infinity", "-Infinity", "0.5", "-1e-6176"])
        shouldThrow(() => BigInt(d(text)), RangeError);
    // Only the two constructors convert.
    shouldThrow(() => BigInt.asIntN(8, d("1")), TypeError);
}
