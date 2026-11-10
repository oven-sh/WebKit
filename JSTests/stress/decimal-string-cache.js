//@ requireOptions("--useDecimal=1")
// The strings of Decimals come from a cache of 1024 entries, two in a set. The blocks have more values than that, hence their counts. Those
// that differ in the exponent or in a word share sets whatever the hash is; the sign, the mode and the notation have few values, so only
// some of their pairs share one.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
}

// The exponent. These are written the same in both notations.
for (let e = -4000; e < 4000; ++e) {
    if (e > -40 && e < 40)
        continue;
    const text = `1.7e${e}`;
    shouldBe(new Decimal(text).toExponential(), text, "the exponent");
    shouldBe(new Decimal(text).toString(), text, "the exponent, in toString()");
}

// The lower word, the upper word, and both.
for (let i = 1; i < 4000; ++i) {
    shouldBe(new Decimal(String(i)).toString(), String(i), "the lower word");
    const upper = String(BigInt(i) << 64n | 1n);
    shouldBe(new Decimal(upper).toString(), upper, "the upper word");
    shouldBe(BigInt(new Decimal(upper)) >> 64n, BigInt(i), "the upper word, by value");
    shouldBe(new Decimal(upper).toFixed({ digits: 0 }), upper, "the upper word, in toFixed()");
}

// The sign, each value next to its negative, and the two zeros.
for (let i = 0; i < 4000; ++i) {
    const x = new Decimal(String(i));
    shouldBe(x.toString(), String(i), "positive");
    shouldBe(x.negate().toString(), i ? String(-i) : "-0", "negative");
    shouldBe(x.toFixed({ digits: 1 }), `${i}.0`, "positive, fixed");
    shouldBe(x.negate().toFixed({ digits: 1 }), `-${i}.0`, "negative, fixed");
}

// The notation, the count and the mode, of one value and of many.
const modes = [undefined, "ceil", "floor", "trunc", "halfExpand", "halfEven"];
const expectedInteger = { undefined: 2, ceil: 3, floor: 2, trunc: 2, halfExpand: 3, halfEven: 2 };
for (let round = 0; round < 3; ++round) {
    for (let digits = 0; digits <= 100; ++digits) {
        for (const roundingMode of modes) {
            const x = new Decimal("2.5");
            const fixed = x.toFixed({ digits, roundingMode });
            shouldBe(fixed, digits ? "2.5" + "0".repeat(digits - 1) : String(expectedInteger[roundingMode]), `toFixed ${digits} ${roundingMode}`);
            if (digits) {
                const precise = x.toPrecision({ precision: digits, roundingMode });
                shouldBe(Number(precise), digits == 1 ? expectedInteger[roundingMode] : 2.5, `toPrecision ${digits} ${roundingMode}`);
                shouldBe(precise.replace(/e.*|\D/g, "").length, digits, `the digits of toPrecision ${digits}`);
            }
            shouldBe(x.toString(), "2.5", "toString between them");
            shouldBe(x.toExponential(), "2.5e0", "toExponential between them");
        }
    }
    fullGC();
}

// The notation and the mode, of many values: some pairs share a set.
for (let i = 10; i < 8000; ++i) {
    const x = new Decimal(`${i}.341`);
    shouldBe(x.toFixed({ digits: 2, roundingMode: "floor" }), `${i}.34`, "floor");
    shouldBe(x.toFixed({ digits: 2, roundingMode: "ceil" }), `${i}.35`, "ceil");
    shouldBe(x.toFixed({ digits: 9 }), `${i}.341000000`, "fixed");
    shouldBe(x.toPrecision({ precision: 9 }).replace(/e.*|\D/g, ""), `${i}341000000`.slice(0, 9), "precision");
}
// These have no digits or mode, so they differ in the notation only.
for (let i = 1; i < 8000; ++i) {
    const x = new Decimal(i * 10 + 5);
    shouldBe(x.toString(), String(i * 10 + 5), "standard");
    shouldBe(x.toExponential().includes("e"), true, "exponential");
}

// The same with all 34 digits. The format is mixed with the lower word, and by how much that moves a value depends on the upper one, so it
// is with these that one value's texts come to share a set.
for (let i = 1; i < 6000; ++i) {
    const integer = `${1e15 + (i * 7919993777) % 8e15}${1e14 + (i * 104729001) % 8e14}`; // 31 digits.
    const x = new Decimal(`${integer}.341`);
    shouldBe(x.toFixed({ digits: 2, roundingMode: "floor" }), `${integer}.34`, "floor of 34 digits");
    shouldBe(x.toFixed({ digits: 2, roundingMode: "ceil" }), `${integer}.35`, "ceil of 34 digits");
    shouldBe(x.toFixed({ digits: 2, roundingMode: "trunc" }), `${integer}.34`, "trunc of 34 digits");
    shouldBe(x.toString(), `${integer}.341`, "standard of 34 digits");
    shouldBe(x.toExponential(), `${integer[0]}.${integer.slice(1)}341e30`, "exponential of 34 digits");
    shouldBe(x.toFixed({ digits: 25 }), `${integer}.341${"0".repeat(22)}`, "fixed of 34 digits");
    shouldBe(x.toPrecision({ precision: 25 }).replace(/e.*|\D/g, "").length, 25, "precision of 34 digits");
}

// Found again: values that take turns, in both orders, next to ones that push them out.
for (let i = 0; i < 1500; ++i) {
    const [a, b, c] = [i, i + 0.5, i + 0.25].map(x => [new Decimal(x), String(x)]);
    for (const [x, text] of [a, b, a, b, c, a, c, b, b, a])
        shouldBe(x.toString(), text, "in turns");
}

// Few enough to stay in the cache through a collection of the young, whose strings must be alive. A full one clears it.
for (let round = 0; round < 20; ++round) {
    for (let i = 0; i < 300; ++i)
        shouldBe(new Decimal(i + round / 64).toString(), String(i + round / 64), "before a collection");
    round % 5 == 4 ? fullGC() : edenGC();
    const garbage = [];
    for (let i = 0; i < 1000; ++i)
        garbage.push("x" + i + round);
    for (let i = 299; i >= 0; --i)
        shouldBe(new Decimal(i + round / 64).toString(), String(i + round / 64), "after a collection");
}

// A mode's name that is not the string of the identifier.
{
    const x = new Decimal("2.5");
    const expected = { ceil: "3", floor: "2", trunc: "2", halfExpand: "3", halfEven: "2" };
    for (const name in expected) {
        const rope = name.slice(0, 2) + name.slice(2);
        const substring = ("x" + name + "x").slice(1, -1);
        const flat = JSON.parse(JSON.stringify({ name })).name.split("").join("");
        for (const mode of [name, rope, substring, flat]) {
            shouldBe(x.round(0, mode).toString(), expected[name], `round ${name}`);
            shouldBe(x.toFixed({ digits: 0, roundingMode: mode }), expected[name], `toFixed ${name}`);
            shouldBe(x.toPrecision({ precision: 1, roundingMode: mode }), expected[name] + "e0", `toPrecision ${name}`);
        }
    }
}
