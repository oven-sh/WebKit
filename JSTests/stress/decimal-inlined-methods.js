//@ requireOptions("--useDecimal=1")
// The methods that optimized code has inline or calls directly, in every tier, for every kind of value and receiver.

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
}

function shouldThrow(f, type, what) {
    let error;
    try {
        f();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof type))
        throw new Error(`${what}: expected a ${type.name}, got ${String(error)}`);
}

// Around each power of 10 and of 2, where the number of digits changes, and where the coefficient no longer fits in 64 bits.
const cases = [["NaN", true, false, NaN], ["Infinity", false, false, Infinity], ["-Infinity", false, false, Infinity], ["0", false, true, -Infinity], ["-0", false, true, -Infinity]];
for (let i = 0; i < 34; ++i) {
    const power = 10n ** BigInt(i);
    for (const coefficient of [power, power + 1n, power * 9n + 1n, power * 10n - 1n]) {
        for (const exponent of [0, -1, 1, -6176, 6111, -33, 100]) {
            const text = `${coefficient}e${exponent}`;
            if (coefficient < 10n ** 34n)
                cases.push([text, false, true, String(coefficient).length - 1 + exponent], ["-" + text, false, true, String(coefficient).length - 1 + exponent]);
        }
    }
}
for (const bits of [1n, 31n, 32n, 33n, 63n, 64n, 65n, 112n]) {
    for (const coefficient of [2n ** bits - 1n, 2n ** bits, 2n ** bits + 1n])
        cases.push([String(coefficient), false, true, String(coefficient).length - 1]);
}

const functions = {
    isNaN: x => x.isNaN(),
    isFinite: x => x.isFinite(),
    exponent: x => x.exponent(),
    mantissa: x => x.mantissa(),
    abs: x => x.abs(),
    negate: x => x.negate(),
    toString: x => x.toString(),
    toExponential: x => x.toExponential(),
};
for (const name in functions)
    noInline(functions[name]);

for (let n = 0; n < testLoopCount / 100; ++n) {
    for (const [text, isNaN, isFinite, exponent] of cases.slice(n % 7, n ? n % 7 + 40 : undefined)) {
        const x = new Decimal(text);
        shouldBe(functions.isNaN(x), isNaN, text + " isNaN");
        shouldBe(functions.isFinite(x), isFinite, text + " isFinite");
        shouldBe(functions.exponent(x), exponent, text + " exponent");
        shouldBe(functions.abs(x).toString(), isNaN ? "NaN" : functions.toString(x).replace(/^-/, ""), text + " abs");
        shouldBe(functions.negate(functions.negate(x)).toExponential(), functions.toExponential(x), text + " negate");
        shouldBe(new Decimal(functions.toString(x)).toExponential(), functions.toExponential(x), text + " toString");
        if (isFinite && exponent !== -Infinity)
            shouldBe(functions.mantissa(x).scale10(exponent).toExponential(), x.toExponential(), text + " mantissa");
    }
}

// All of them once more, now that the functions are compiled.
for (const [text, isNaN, isFinite, exponent] of cases) {
    const x = new Decimal(text);
    shouldBe(functions.isNaN(x), isNaN, text + " isNaN");
    shouldBe(functions.isFinite(x), isFinite, text + " isFinite");
    shouldBe(functions.exponent(x), exponent, text + " exponent");
}

// A new object each time, also where a loop has nothing that changes.
{
    const distinct = (x, count) => {
        const set = new Set;
        for (let i = 0; i < count; ++i) {
            set.add(x.abs());
            set.add(x.negate());
            set.add(x.mantissa());
        }
        return set.size;
    };
    noInline(distinct);
    const x = new Decimal("-12.5");
    for (let i = 0; i < 200; ++i)
        shouldBe(distinct(x, 20), 60, "distinct objects");
}

// Other receivers behind the compiled call, which exits and then throws.
{
    class Money extends Decimal { }
    for (const name in functions) {
        shouldBe(functions[name](new Money("1.5")).toString(), functions[name](new Decimal("1.5")).toString(), name + " of a subclass");
        for (const receiver of [Object.create(Decimal.prototype), Decimal.prototype, { __proto__: new Decimal("1") }, new Proxy(new Decimal("1"), {})])
            shouldThrow(() => functions[name](receiver), TypeError, name + " of another object");
        for (const receiver of [1, "1", 1n, true, Symbol(), undefined, null])
            shouldThrow(() => Decimal.prototype[name].call(receiver), TypeError, name + " of a primitive");
        // They work again afterwards.
        for (let i = 0; i < testLoopCount / 10; ++i)
            functions[name](new Decimal("1.5"));
        shouldBe(functions.exponent(new Decimal("123")), 2, "afterwards");
    }
}

// More arguments than the method has, and a replaced method.
{
    const extra = x => [x.isNaN(1), x.exponent(1, 2), x.toString(undefined), x.abs(1).toString()].join();
    noInline(extra);
    for (let i = 0; i < testLoopCount / 10; ++i)
        shouldBe(extra(new Decimal("-15")), "false,1,-15,15", "extra arguments");
    shouldThrow(() => new Decimal("1").toString(1), TypeError, "options that are no object");

    const isNaN = x => x.isNaN();
    noInline(isNaN);
    for (let i = 0; i < testLoopCount / 10; ++i)
        isNaN(new Decimal("1"));
    const original = Decimal.prototype.isNaN;
    Decimal.prototype.isNaN = () => "replaced";
    shouldBe(isNaN(new Decimal("1")), "replaced", "replaced method");
    Decimal.prototype.isNaN = original;
    shouldBe(isNaN(new Decimal("NaN")), true, "restored method");
}

// The strings come from a cache. Equal values have equal strings, and others do not share one, through collections.
{
    const strings = [];
    for (let i = 0; i < 3000; ++i)
        strings.push(new Decimal(i).divide(new Decimal(8)).toString());
    for (let round = 0; round < 3; ++round) {
        for (let i = 0; i < 3000; ++i) {
            const x = new Decimal(i).divide(new Decimal(8));
            shouldBe(x.toString(), strings[i], "cached string");
            shouldBe(x.toString(), String(i / 8), "its text");
            shouldBe(Number(x.toExponential()), i / 8, "its exponential text");
        }
        round == 1 ? fullGC() : edenGC();
    }
    for (const [a, b] of [["0", "-0"], ["1", "10"], ["1", "-1"], ["1e33", "1e34"], ["18446744073709551616", "1"]]) {
        for (let i = 0; i < 10; ++i) {
            shouldBe(new Decimal(a).toString() === new Decimal(b).toString(), false, `${a} and ${b}`);
            shouldBe(new Decimal(a).toExponential() === new Decimal(b).toExponential(), false, `${a} and ${b}`);
        }
    }
}

// The options are read as before, whatever kind of object they are.
{
    const x = new Decimal("2.345");
    const format = options => x.toFixed(options);
    noInline(format);
    const log = [];
    for (let i = 0; i < 300; ++i) {
        shouldBe(format({ digits: 2 }), "2.34", "digits");
        shouldBe(format({ digits: 2, roundingMode: "halfExpand" }), "2.35", "both");
        shouldThrow(() => format({ roundingMode: "ceil" }), TypeError, "no digits");
        shouldBe(format({ __proto__: { digits: 1 } }), "2.3", "inherited");
        shouldBe(format({ __proto__: null, digits: 1 }), "2.3", "null prototype");
        shouldBe(format({ get digits() { return 2; } }), "2.34", "getter");
        shouldBe(format(Object.assign([], { digits: 2 })), "2.34", "array");
        shouldBe(format(Object.assign(() => { }, { digits: 2 })), "2.34", "function");
        shouldBe(format(new Proxy({ digits: 2 }, { })), "2.34", "proxy");
    }
    format(new Proxy({ digits: 2 }, { get(target, key) { log.push(key); return target[key]; } }));
    shouldBe(log.join(), "digits,roundingMode", "order");
    Object.prototype.roundingMode = "ceil";
    shouldBe(format({ digits: 2 }), "2.35", "from Object.prototype");
    delete Object.prototype.roundingMode;
    Object.defineProperty(Object.prototype, "roundingMode", { get() { return "floor"; }, configurable: true });
    shouldBe(new Decimal("2.349").toFixed({ digits: 2 }), "2.34", "getter of Object.prototype");
    delete Object.prototype.roundingMode;
    const dictionary = { digits: 2 };
    for (let i = 0; i < 200; ++i)
        dictionary["p" + i] = i;
    for (let i = 0; i < 100; ++i)
        delete dictionary["p" + i];
    shouldBe(format(dictionary), "2.34", "dictionary");
}

// From numbers, without text.
for (const number of [0, -0, 1, -1, 5, 10, 100, 1e6, 2 ** 31, -(2 ** 31), 2 ** 31 - 1, 2 ** 53, -(2 ** 53), 2 ** 53 + 2, 1e21, 1e22, 1.5, -1.5, 0.1, 5e-324, 1.7976931348623157e308, 1e-7, 123456789012345680, 9007199254740993, 0.30000000000000004])
    shouldBe(new Decimal(number).toExponential(), new Decimal(String(number)).toExponential(), `new Decimal(${number})`);
for (const number of [NaN, Infinity, -Infinity])
    shouldBe(new Decimal(number).toString(), String(number), `new Decimal(${number})`);
