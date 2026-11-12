//@ requireOptions("--useDecimal=1")
// round() and scale10() with a count that the compiler does not know.

function outcome(f, ...args) {
    try {
        const value = f(...args);
        return value.toExponential() + (value.equals(0m) ? 1m.divide(value).toString() : "");
    } catch (error) {
        return `${error.name}: ${error.message}`;
    }
}
noInline(outcome);

const values = ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5", "2.5", "19.995", "-0.045", "1e-19", "1e-20", "1500", "-250", "18446744073709551615", "-1.8446744073709551615", "18446744073709551616",
    "1234567890123456789012345678901234", "-1.234567890123456789012345678901234", "1e-6176", "15e-6176", "-9e6144", "15e6110", "15e6111", "25e6112", "9999999999999999999999999999999999e6111"].map(text => new Decimal(text));
const integers = [0, 1, 2, 3, 18, 19, 20, 21, 34, -1, -2, -3, -19, -20, 6110, 6111, 6112, 6144, 6145, 6175, 6176, 6177, -6110, -6111, -6112, -6113, -6144, -6145, -6176, -6177, 8190, -8190, -8191, -8192, -8193, 12287, -12287, 12320, -12320, 16384, -16384, 2147483647, -2147483648];
const others = [-0, 0.5, 1e10, -1e10, 1e300, NaN, Infinity, -Infinity, undefined, null, "2", 2n, true, { valueOf() { throw new Error("It is not called"); } }];

for (const call of ["round(n)", "round(n, undefined)", `round(n, "ceil")`, `round(n, "floor")`, `round(n, "trunc")`, `round(n, "halfExpand")`, `round(n, "halfEven")`, `round(n, "other")`, "scale10(n)", `scale10(n, "floor")`]) {
    const reference = new Function("x", "n", `return x.${call}; // reference`);
    noDFG(reference);
    // Integers only, then anything in the code that was compiled for those, and anything from the start.
    for (const phases of [[integers, [...integers, ...others]], [[...others, ...integers]]]) {
        const f = new Function("x", "n", `return x.${call}; // ${phases.length}`);
        noInline(f);
        for (const counts of phases) {
            const cases = values.flatMap(x => counts.map(n => [x, n, outcome(reference, x, n)]));
            for (let i = 0; i < testLoopCount + cases.length; ++i) {
                const [x, n, expected] = cases[i % cases.length];
                const actual = outcome(f, x, n);
                if (actual !== expected)
                    throw new Error(`${x.toExponential()}.${call} with ${typeof n === "object" ? "an object" : n}: expected ${expected}, got ${actual}`);
            }
        }
    }
}

// The count is looked at also if nothing uses the result.
for (const method of ["round", "scale10"]) {
    const f = new Function("x", "n", `x.${method}(n); return x;`);
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        f(values[5], i & 3);
    for (let i = 0; i < testLoopCount; ++i) {
        for (const [n, name] of [[0.5, "RangeError"], ["1", "TypeError"], [1, "1e"]]) {
            if (!outcome(f, values[5], n).startsWith(name))
                throw new Error(`${method}(${n}) without a use`);
        }
    }
}
