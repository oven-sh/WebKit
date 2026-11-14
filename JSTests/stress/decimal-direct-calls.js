//@ requireOptions("--useDecimal=1")
// The methods with a Decimal or a count as their one argument, called directly by optimized code: each with operands that are none behind
// the compiled call, in a loop where nothing changes, and next to each other.

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
}

const methods = ["add", "subtract", "multiply", "divide", "remainder", "compare", "equals", "notEquals", "lessThan", "lessThanOrEqual", "greaterThan", "greaterThanOrEqual"];
const texts = ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5", "19.99", "0.1", "18446744073709551615", "1e19", "12345678901234567890123456789", "1e-6176", "-9e6144", "9e6144"];
const values = texts.map(text => new Decimal(text));
const text = value => value instanceof Decimal ? "d" + value.toExponential() : typeof value + (Object.is(value, -0) ? "-0" : String(value));
const others = [1, "1", 1n, true, undefined, null, Symbol(), {}, [], new Date(0), Object.create(Decimal.prototype), new Proxy(new Decimal("1"), {}), { __proto__: new Decimal("1") }, () => 1, new Map];

function throwsTypeError(f, what) {
    let error;
    try {
        f();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`${what}: expected a TypeError, got ${String(error)}`);
}

for (const method of methods) {
    // Never compiled together with the call.
    const expected = new Map;
    for (const x of values) {
        for (const y of values)
            expected.set(x.toExponential() + method + y.toExponential(), text(Decimal.prototype[method].apply(x, [y])));
    }

    const call = Function("x", "y", `return x.${method}(y)`);
    noInline(call);
    for (let i = 0; i < testLoopCount; ++i) {
        const [x, y] = [values[i % values.length], values[(i * 7 + (i >> 4)) % values.length]];
        shouldBe(text(call(x, y)), expected.get(x.toExponential() + method + y.toExponential()), `${x} ${method} ${y}`);
    }
    for (const other of others) {
        throwsTypeError(() => call(values[5], other), method + " with another operand");
        if (other !== null && other !== undefined)
            throwsTypeError(() => Decimal.prototype[method].call(other, values[5]), method + " of another receiver");
    }
    throwsTypeError(() => call(values[5]), method + " without an operand");

    // The operand is checked in each turn where it can be something else, and nothing is read of it before.
    const loop = Function("x", "y", "n", `let r = 0; for (let i = 0; i < n; ++i) { if (i > 5) r = x.${method}(y); } return r;`);
    noInline(loop);
    for (let i = 0; i < 200; ++i)
        loop(values[5], values[6], 100);
    for (const other of [...others, 0, 4.5, -1, 2 ** 40]) {
        shouldBe(loop(values[5], other, 5), 0, method + " that is not reached");
        throwsTypeError(() => loop(values[5], other, 100), method + " that is reached");
    }

    // A check in front, as a program has.
    const guarded = Function("x", "y", "n", `let r = 0; for (let i = 0; i < n; ++i) { if (y instanceof Decimal) r = x.${method}(y); else ++r; } return r;`);
    noInline(guarded);
    for (let i = 0; i < 200; ++i) {
        guarded(values[5], values[6], 50);
        for (const other of others)
            {
            // Those that are only instanceof Decimal pass the check of the program, and must not pass the one of the call.
            if (other instanceof Decimal)
                throwsTypeError(() => guarded(values[5], other, 3), method + " behind instanceof");
            else
                guarded(values[5], other, 3);
        }
    }
}

{
    const distinct = (x, y) => [x.add(y), x.add(y), x.subtract(y), x.subtract(y), x.multiply(y), x.round(1), x.round(1), x.abs(), x.abs()];
    noInline(distinct);
    for (let i = 0; i < testLoopCount / 10; ++i)
        shouldBe(new Set(distinct(values[6], values[7])).size, 9, "distinct objects");
}

{
    const round = (x, n) => x.round(n);
    noInline(round);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(round(values[7], i % 3).toString(), ["20", "20", "19.99"][i % 3], "round");
        shouldBe(round(values[9], -19 + (i & 1)).toString(), i & 1 ? "18000000000000000000" : "20000000000000000000", "round above 1");
    }
    for (const digits of ["1", 1n, {}, undefined, null])
        throwsTypeError(() => round(values[7], digits), "round to something else");
    shouldBe(round(values[7], 2 ** 40).toString(), "19.99", "many digits");
    shouldBe(round(values[7], -(2 ** 31)).toString(), "0", "the least count");
    shouldBe(round(values[7], -0).toString(), "20", "-0");
    let error;
    try {
        round(values[7], 1.5);
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof RangeError, true, "half a digit");
    const withMode = x => x.round(1, "ceil");
    noInline(withMode);
    for (let i = 0; i < testLoopCount / 10; ++i)
        shouldBe(withMode(values[7]).toString(), "20", "a mode");
}

// An operand of another realm is a Decimal, and the result is of the method's realm.
{
    const other = createGlobalObject();
    const add = (x, y) => x.add(y);
    noInline(add);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(Object.getPrototypeOf(add(values[5], new other.Decimal("2"))), Decimal.prototype, "own method");
        shouldBe(Object.getPrototypeOf(add(new other.Decimal("2"), values[5])), other.Decimal.prototype, "method of the other realm");
    }
}

// What a call changes is nothing: other objects keep their shape and their fields.
{
    class Account { constructor(b) { this.b = b; this.n = 0; } }
    const run = (a, x) => { for (let i = 0; i < 20; ++i) { a.b = a.b.add(x); a.n++; if (a.b.lessThan(x)) a.n = -1; } return a.n; };
    noInline(run);
    for (let i = 0; i < 2000; ++i) {
        const account = new Account(new Decimal("0"));
        shouldBe(run(account, new Decimal("0.01")), 20, "count");
        shouldBe(account.b.toString(), "0.2", "sum");
    }
}

// Many calls with a count, next to integers, Decimals and booleans that are used behind them: each needs its register.
{
    const x = new Decimal("19.987654321");
    for (const count of [1, 2, 3, 4, 5, 8, 12, 20]) {
        for (const argument of ["2", "n", "i & 3"]) {
            const body = `let s = 0, t = a, u = b, d = x; for (let i = 0; i < 8; ++i) { const p = i * 3, q = a + i, r = x.lessThan(x);
                ${`d = x.round(${argument}); s = s + a | 0; t = t ^ p; `.repeat(count)} u = u + q + (r ? 1 : 0) | 0; }
                return s + "," + t + "," + u + "," + d.toString() + "," + o.v + "," + x.toString();`;
            const run = Function("x", "n", "a", "b", "o", body);
            noInline(run);
            let s = 0, t = 3, u = 5;
            for (let i = 0; i < 8; ++i) {
                for (let k = 0; k < count; ++k) {
                    s = s + 3 | 0;
                    t ^= i * 3;
                }
                u = u + 3 + i | 0;
            }
            const digits = argument == "i & 3" ? 3 : 2;
            const expected = `${s},${t},${u},${["20", "20", "19.99", "19.988"][digits]},7,19.987654321`;
            for (let i = 0; i < 400; ++i)
                shouldBe(run(x, 2, 3, 5, { v: 7 }), expected, `${count} rounds of ${argument}`);
        }
    }
}

// Results that are not used are not needed, but their checks are.
{
    const unused = Function("x", "y", "x.abs(); x.add(y); x.round(2); x.compare(y); x.toString(); return 1;");
    noInline(unused);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(unused(values[5], values[6]), 1, "unused");
    throwsTypeError(() => unused(values[5], 1), "an unused result, of another operand");
    throwsTypeError(() => unused(1, values[5]), "an unused result, of another receiver");
}

// compare() as an index and as a number, where it is NaN only late.
{
    const table = ["less", "equal", "greater"];
    const at = (x, y) => table[x.compare(y) + 1];
    noInline(at);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(at(new Decimal(i % 3), values[5]), table[i % 3 < 1 ? 0 : i % 3 < 2 ? 1 : 2], "an index");
    shouldBe(at(values[0], values[5]), undefined, "an index of NaN");
    const number = (x, y) => x.compare(y);
    noInline(number);
    for (let i = 0; i < testLoopCount; ++i)
        number(values[5], values[6]);
    shouldBe(number(values[0], values[6]), NaN, "NaN late");
}

// What the direct call does not take goes to the function.
{
    const twoArguments = (x, y) => x.add(y, 1);
    const mode = (x, m) => x.round(1, m);
    noInline(twoArguments);
    noInline(mode);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(twoArguments(values[5], values[5]).toString(), "2", "an argument too many");
        shouldBe(mode(values[7], i & 1 ? "ceil" : "floor").toString(), i & 1 ? "20" : "19.9", "a mode");
    }
}

// A count that is a double. See also decimal-code-settles.js.
{
    const counts = {
        fraction: "t[i & 1]",
        minimum: "Math.min(v, 3)",
        variable: "v",
        negativeZero: "(i ? -0 : -0)",
        logarithm: "Math.log10(100)",
    };
    for (const name in counts) {
        const run = Function("x", "t", "v", `let s = 0; for (let i = 0; i < 100; ++i) s += x.round(${counts[name]}).isNaN() ? 1 : 2; return s;`);
        noInline(run);
        for (let i = 0; i < 2000; ++i)
            shouldBe(run(values[7], [2, 0, 0.5], i & 1 ? 2 : 1e10), 200, name);
    }
}

// The method as a constructor, and one that throws through the native function.
{
    const construct = (x, y) => new x.add(y);
    const invalid = (x, y) => x.round(1, y);
    noInline(construct);
    noInline(invalid);
    for (let i = 0; i < 1000; ++i) {
        throwsTypeError(() => construct(values[5], values[6]), "construct");
        throwsTypeError(() => invalid(values[5], 1), "a mode that is no string");
    }
}

// The count of round() is checked where the call is, not where its result is used, nor in front of a loop.
{
    const unused = (x, n) => { x.round(n); return 1; };
    const inLoop = (x, n, m) => { let r = 0; for (let i = 0; i < m; ++i) { if (i > 5) x.round(n); ++r; } return r; };
    const caught = (x, n) => { let r = 1; try { r = 2; x.round(n); r = 3; } catch (e) { return r + (e instanceof TypeError ? "T" : e instanceof RangeError ? "R" : "?"); } return r; };
    noInline(unused);
    noInline(inLoop);
    noInline(caught);
    for (let i = 0; i < testLoopCount; ++i) {
        unused(values[7], i & 3);
        inLoop(values[7], i & 1 ? 2 : 1.0e10, 20);
        shouldBe(caught(values[7], 2), 3, "no exception");
    }
    for (const count of ["1", 1n, {}, undefined, null, true, Symbol()]) {
        throwsTypeError(() => unused(values[7], count), "an unused result");
        shouldBe(inLoop(values[7], count, 5), 5, "a loop that does not reach it");
        throwsTypeError(() => inLoop(values[7], count, 20), "a loop that does");
        shouldBe(caught(values[7], count), "2T", "caught");
    }
    for (const count of [0.5, NaN])
        shouldBe(caught(values[7], count), "2R", "no integer");
}

// An error looks the same in each tier, with its native frame, from a frame that is inlined and a catch in another, with values that live
// across the call.
{
    const makers = [];
    for (let i = 0; i < 20; ++i)
        makers.push(Function('n', 'return x => x.round(n)'));
    const stackOf = (round, x) => {
        const o = { a: 1, b: x };
        let s = 7;
        try {
            s = 8;
            round(x);
            return "none";
        } catch (e) {
            return s + o.a + (o.b === x ? "same" : "other") + e.stack.split("\n")[0];
        }
    };
    noInline(stackOf);
    const good = makers.map(make => make(2)), bad = makers.map(make => make("1"));
    const expected = "9same" + (() => { try { values[7].round("1"); } catch (e) { return e.stack.split("\n")[0]; } })();
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(stackOf(good[i % 20], values[7]), "none", "good");
        if (i % 50 == 49)
            shouldBe(stackOf(bad[i / 50 % 20 | 0], values[7]), expected, "bad");
    }
    gc();
}
for (const [count, expected] of [["2", "19.99"], ["0", "20"], ["-1", "20"], ["2147483647", "19.99"], ["-2147483648", "0"], ["1e10", "19.99"], ["-0", "20"]]) {
    const round = Function("x", `return x.round(${count})`);
    noInline(round);
    for (let i = 0; i < testLoopCount / 10; ++i)
        shouldBe(round(values[7]).toString(), expected, "round(" + count + ")");
}

// The same for each function whose arguments optimized code looks at itself.
for (const [source, good, bad] of [["new Decimal(n)", ["1", 2n], ["x", { }, undefined]], ["x.round(n)", [1, 1e10], [0.5, "1"]], ["x.scale10(n)", [1, Infinity], [0.5, "1"]],
    ["x.toFixed({ digits: n })", [1, 2], [-1, 0.5, "1", undefined, 1e300]], ["x.toFixed({ digits: 1, roundingMode: n })", ["ceil", undefined], ["up", 1]]]) {
    for (const value of bad) {
        const f = Function("x", "n", `try { ${source}; return "none"; } catch (e) { return e.name + e.message + e.stack.split("\\n")[0]; } // ${String(value)}`);
        noInline(f);
        const expected = f(values[7], value);
        shouldBe(expected.endsWith("@[native code]"), true, source);
        for (let i = 0; i < testLoopCount; ++i)
            shouldBe(f(values[7], good[i & 1]), "none", source);
        for (let i = 0; i < 3; ++i)
            shouldBe(f(values[7], value), expected, source);
    }
}
