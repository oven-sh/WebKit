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

// round().
{
    const modes = ["ceil", "floor", "trunc", "halfExpand", "halfEven"];
    for (const [text, digits, ...expected] of [
        ["1.5", 0, "2", "1", "1", "2", "2"],
        ["2.5", 0, "3", "2", "2", "3", "2"],
        ["-1.5", 0, "-1", "-2", "-1", "-2", "-2"],
        ["-2.5", 0, "-2", "-3", "-2", "-3", "-2"],
        ["1.25", 1, "1.3", "1.2", "1.2", "1.3", "1.2"],
        ["1.01", 5, "1.01", "1.01", "1.01", "1.01", "1.01"],
        ["1250", -2, "1300", "1200", "1200", "1300", "1200"],
        ["9.99", 1, "10", "9.9", "9.9", "10", "10"],
        // The mathematical value 0 has no sign.
        ["-0.3", 0, "0", "-1", "0", "0", "0"],
        ["1", -5, "100000", "0", "0", "0", "0"],
        ["1", -7000, "Infinity", "0", "0", "0", "0"],
        ["-1", -1e300, "0", "-Infinity", "0", "0", "0"],
        ["1", 1e300, "1", "1", "1", "1", "1"],
        ["NaN", 0, "NaN", "NaN", "NaN", "NaN", "NaN"],
        ["-Infinity", 0, ...Array(5).fill("-Infinity")],
        ["-0", 0, ...Array(5).fill("-0")],
    ])
        modes.forEach((mode, i) => shouldBe(d(text).round(digits, mode).toString(), expected[i], `${text} ${digits} ${mode}`));
    shouldBe(d("2.5").round(0).toString(), "2");
    shouldBe(d("2.5").round(0, undefined).toString(), "2");
    for (const digits of [undefined, null, "1", 1n, {}])
        shouldThrow(() => d("1").round(digits), TypeError);
    for (const digits of [NaN, Infinity, -Infinity, 1.5])
        shouldThrow(() => d("NaN").round(digits), RangeError);
    for (const mode of [null, 1, {}, new String("ceil")])
        shouldThrow(() => d("1").round(0, mode), TypeError);
    for (const mode of ["", "expand", "Ceil", "halfCeil", "halfTrunc"])
        shouldThrow(() => d("1").round(0, mode), RangeError);
}

// scale10().
{
    for (const [text, power, expected] of [
        ["1.5", 2, "150"], ["1.5", -2, "0.015"], ["1", 6144, "1e6144"], ["1", 6145, "Infinity"], ["-1", 1e300, "-Infinity"], ["1", -6176, "1e-6176"], ["-1", -1e300, "-0"],
        ["15", -6177, "2e-6176"], ["1e-6176", 12320, "1e6144"], ["1", NaN, "NaN"], ["1", Infinity, "Infinity"], ["-1", Infinity, "-Infinity"], ["1", -Infinity, "0"], ["-1", -Infinity, "-0"],
        ["0", NaN, "0"], ["-0", Infinity, "-0"], ["Infinity", -Infinity, "Infinity"], ["Infinity", NaN, "Infinity"], ["NaN", 1, "NaN"], ["1", -0, "1"],
    ])
        shouldBe(d(text).scale10(power).toString(), expected, `${text} ${power}`);
    for (const power of [undefined, null, "1", 1n, {}])
        shouldThrow(() => d("1").scale10(power), TypeError);
    shouldThrow(() => d("NaN").scale10(1.5), RangeError);
}

// The order of the checks of round(), which are all before the value is looked at.
shouldThrow(() => d("1").round(1.5, 1), RangeError);
shouldThrow(() => d("1").round("1", "bad"), TypeError);
for (const text of ["NaN", "Infinity", "-Infinity", "0", "-0"]) {
    shouldThrow(() => d(text).round(0, "bad"), RangeError);
    shouldThrow(() => d(text).round(0, null), TypeError);
    shouldThrow(() => d(text).round(0.5), RangeError);
    shouldThrow(() => d(text).round(), TypeError);
}
