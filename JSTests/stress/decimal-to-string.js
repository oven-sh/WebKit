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

// Strings.
{
    for (const [text, string, exponential] of [
        ["0", "0", "0e+0"], ["-0", "-0", "-0e+0"], ["-12", "-12", "-1.2e1"], ["120", "120", "1.2e2"], ["0.000001", "0.000001", "1e-6"], ["0.0000001", "1e-7", "1e-7"],
        ["-0.00000012", "-1.2e-7", "-1.2e-7"], ["1e33", "1" + "0".repeat(33), "1e33"], ["1e34", "1e34", "1e34"], ["-1.5e40", "-1.5e40", "-1.5e40"],
        ["NaN", "NaN", "NaN"], ["-Infinity", "-Infinity", "-Infinity"], ["-0.5", "-0.5", "-5e-1"], ["1234.5", "1234.5", "1.2345e3"],
    ]) {
        shouldBe(d(text).toString(), string, text);
        shouldBe(d(text).toString({}), string, text);
        shouldBe(d(text).toExponential(), exponential, text);
        shouldBe(new Decimal(d(text).toString()).toString(), string);
    }
    for (const name of ["toString", "toExponential", "toFixed", "toPrecision"]) {
        for (const options of [null, 1, "1", true, 1n, Symbol()])
            shouldThrow(() => d("1")[name](options), TypeError);
    }

    for (const [text, digits, expected, roundingMode] of [
        ["1.005", 2, "1.00"], ["1.015", 2, "1.02"], ["1.005", 2, "1.01", "halfExpand"], ["-1.5", 0, "-2", "floor"], ["-1.5", 0, "-1", "ceil"], ["1", 3, "1.000"], ["0", 2, "0.00"],
        ["-0", 2, "-0.00"], ["-0", 0, "-0"], ["-0.3", 0, "-0"], ["0.001", 2, "0.00"], ["9.999", 2, "10.00"], ["120", 1, "120.0"], ["1e34", 1, "1" + "0".repeat(34) + ".0"],
        ["1e-10", 12, "0.000000000100"], ["0.5", 0, "0"], ["NaN", 2, "NaN"], ["-Infinity", 2, "-Infinity"], ["1e6144", 0, "1" + "0".repeat(6144)],
        ["1e-6176", 6176, "0." + "0".repeat(6175) + "1"], ["0.1", -0, "0"], ["9999999999999999999999999999999.999", 2, "1" + "0".repeat(31) + ".00"],
    ])
        shouldBe(d(text).toFixed({ digits, roundingMode }), expected, `${text} ${digits}`);
    shouldBe(d("1.5").toFixed(), "2");
    for (const digits of [undefined, null, "1", 1n, {}])
        shouldThrow(() => d("NaN").toFixed({ digits }), TypeError);
    for (const digits of [NaN, Infinity, -1, 0.5])
        shouldThrow(() => d("NaN").toFixed({ digits }), RangeError);
    shouldThrow(() => d("1").toFixed({ digits: 1, roundingMode: 1 }), TypeError);
    shouldThrow(() => d("1").toFixed({ digits: 1, roundingMode: "x" }), RangeError);
    shouldThrow(() => d("1").toFixed({ digits: 2 ** 31 }), RangeError);
    shouldThrow(() => d("1").toFixed({ digits: 1e300 }), RangeError);
    shouldThrow(() => d("1").toPrecision({ precision: 1e300 }), RangeError);

    const log = [];
    d("1").toFixed(new Proxy({ digits: 1 }, { get(target, key) { log.push(key); return target[key]; } }));
    d("1").toPrecision(new Proxy({ precision: 1 }, { get(target, key) { log.push(key); return target[key]; } }));
    shouldBe(log.join(), "digits,roundingMode,precision,roundingMode");

    for (const [text, precision, expected, roundingMode] of [
        ["123.456", 4, "1.235e2"], ["123.456", 2, "1.2e2"], ["-123.456", 4, "-1.234e2", "ceil"], ["-123.456", 4, "-1.235e2", "floor"], ["123.456", 6, "123.456"],
        // The sign is not a digit.
        ["-5", 2, "-5.0"], ["-15", 3, "-15.0"], ["-1.5", 4, "-1.500"], ["-15", 2, "-15"], ["1", 3, "1.00"], ["-12", 4, "-12.00"],
        // Zero has that many digits behind the point, and its sign with one digit only.
        ["0", 1, "0"], ["-0", 1, "-0"], ["0", 3, "0.000"], ["-0", 3, "0.000"],
        // Fewer digits than asked for and a fraction: decimal notation.
        ["0.000001", 2, "0.0000010"], ["0.0000001", 2, "0.00000010"], ["-1e-10", 3, "-0.000000000100"], ["0.00123", 5, "0.0012300"], ["1.5", 3, "1.50"],
        // As many: the notation of Number.prototype.toPrecision. More: an exponent.
        ["1.5e-10", 2, "1.5e-10"], ["1.55e-10", 2, "1.6e-10"], ["1.25", 3, "1.25"], ["99.99", 3, "1.00e2"], ["99.99", 2, "1.0e2"], ["1.25", 2, "1.2e0"], ["0.00125", 2, "1.2e-3"], ["1200", 4, "1200"], ["1200", 3, "1.20e3"], ["1200", 2, "1.2e3"],
        ["25", 1, "2e1"], ["25", 1, "3e1", "halfExpand"], ["9.5", 1, "1e1"], ["NaN", 1, "NaN"], ["-Infinity", 1, "-Infinity"], ["1", 40, "1." + "0".repeat(39)], ["1e34", 35, "1" + "0".repeat(34)],
    ])
        shouldBe(d(text).toPrecision({ precision, roundingMode }), expected, `${text} ${precision}`);
    // Without options the precision is 0.
    for (const text of ["123.456", "1e40", "NaN", "Infinity", "0"]) {
        shouldThrow(() => d(text).toPrecision(), RangeError);
        shouldThrow(() => d(text).toPrecision(undefined), RangeError);
    }
    // With options but no precision: decimal notation.
    shouldBe(d("-1e40").toPrecision({}), "-1" + "0".repeat(40));
    shouldBe(d("1e-10").toPrecision({ roundingMode: 1 }), "0.0000000001");
    // The rounding mode is not checked. What is not one is halfEven, and is not converted.
    for (const roundingMode of ["x", "Ceil", "", 1, null, true, 1n, Symbol(), {}, { toString() { throw new Error("toString"); } }]) {
        for (const text of ["NaN", "Infinity", "0", "1.5"])
            shouldBe(typeof d(text).toPrecision({ precision: 2, roundingMode }), "string");
        shouldBe(d("1.25").toPrecision({ precision: 2, roundingMode }), "1.2e0");
        shouldBe(d("1.35").toPrecision({ precision: 2, roundingMode }), "1.4e0");
    }
    shouldThrow(() => d("1").toPrecision({ precision: 0 }), RangeError);
    shouldThrow(() => d("1").toPrecision({ precision: NaN }), RangeError);
    shouldThrow(() => d("1").toPrecision({ precision: "1" }), TypeError);
    // Which options are read, against every error.
    for (const [method, name, values] of [
        ["toFixed", "digits", [["1", TypeError, 2], [undefined, TypeError, 2], [NaN, RangeError, 2], [-1, RangeError, 2], [1.5, RangeError, 2], [Infinity, RangeError, 2], [1, null, 2]]],
        ["toPrecision", "precision", [["1", TypeError, 1], [undefined, null, 1], [null, TypeError, 1], [NaN, RangeError, 1], [Infinity, RangeError, 1], [-Infinity, RangeError, 2], [0, RangeError, 2], [-0, RangeError, 2], [1.5, RangeError, 2], [1, null, 2]]],
    ]) {
        for (const [value, errorType, count] of values) {
            for (const text of ["1.5", "NaN", "0"]) {
                const log = [];
                const options = new Proxy({ [name]: value }, { get(target, key) { log.push(key); return target[key]; } });
                if (errorType)
                    shouldThrow(() => d(text)[method](options), errorType);
                else
                    d(text)[method](options);
                shouldBe(log.join(), [name, "roundingMode"].slice(0, count).join(), `${method} ${String(value)}`);
            }
        }
    }
    // The error of the count is before that of the mode.
    shouldThrow(() => d("1").toFixed({ digits: 1.5, roundingMode: 1 }), RangeError);
    shouldThrow(() => d("1").toFixed({ digits: "1", roundingMode: "x" }), TypeError);

    // toPrecision and toFixed write what those of Number write where both are exact and have the digits.
    for (const number of [0.5, 1.25, 123.5, 4096, -2.75]) {
        for (let count = 1; count < 12; ++count) {
            if (count >= String(Math.abs(number)).replace(/^0?\./, "").replace(".", "").replace(/^0+/, "").length)
                shouldBe(d(number).toPrecision({ precision: count, roundingMode: "halfExpand" }), number.toPrecision(count).replace("e+", "e"));
            shouldBe(d(number).toFixed({ digits: count, roundingMode: "halfExpand" }), number.toFixed(count));
        }
    }
}
