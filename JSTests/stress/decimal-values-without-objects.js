//@ requireOptions("--useDecimal=1")
// Optimized code computes with the values, and makes an object where one can be seen: also after it is left.

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
}

const texts = ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5", "19.99", "18446744073709551615", "-18446744073709551616", "1234567890123456789012345678901234", "1e-6176", "-9e6144"];
const values = texts.map(text => new Decimal(text));
const text = value => value.toExponential() + (value.equals(0m) ? 1m.divide(value).toString() : "");
function viaCall(method, receiver, ...rest) { return method === undefined ? -1.5m : Decimal.prototype[method].call(receiver, ...rest); }
noInline(viaCall);
noDFG(viaCall);

// An exit while the result exists only as a value: one object, whatever number of names it has.
for (const [source, method] of [["a.add(b)", "add"], ["a.multiply(b)", "multiply"], ["a.divide(b)", "divide"], ["a.negate()", "negate"], ["a.abs()", "abs"], ["a.mantissa()", "mantissa"], ["-1.5m"]]) {
    const f = new Function("a", "b", "n", `const x = ${source}, y = x, z = x.subtract(1m); if (n + 1 === 2) return x.isNaN(); return [x, y, z];`);
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i) {
        const a = values[i % values.length], b = values[(i * 7) % values.length];
        shouldBe(f(a, b, 1), viaCall(method, a, b).isNaN(), source);
    }
    for (const a of values) {
        // The sum overflows. A check of a type would be at the top of the function.
        const [x, y, z] = f(a, values[7], 2147483647);
        shouldBe(x, y, source);
        shouldBe(Object.getPrototypeOf(x), Decimal.prototype, source);
        shouldBe(text(x), text(viaCall(method, a, values[7])), source);
        shouldBe(text(z), text(viaCall("subtract", x, 1m)), source);
    }
}

// It escapes on one path, into an object that may escape itself, or is given properties.
{
    let escaped;
    function onOnePath(a, b, escapes) {
        const x = a.add(b);
        const holder = { x, literal: -1.5m };
        if (escapes)
            escaped = holder;
        return holder.x.lessThan(holder.literal);
    }
    noInline(onOnePath);
    function withProperty(a, escapes) {
        const x = a.negate();
        x.property = 1;
        return escapes ? x : x.property;
    }
    noInline(withProperty);
    for (let i = 0; i < testLoopCount; ++i) {
        const a = values[i % values.length], escapes = !(i % 100);
        shouldBe(onOnePath(a, values[6], escapes), viaCall("lessThan", viaCall("add", a, values[6]), values[6]), "on one path");
        if (escapes) {
            shouldBe(text(escaped.x), text(viaCall("add", a, values[6])), "escaped");
            shouldBe(escaped.literal.toString(), "-1.5", "escaped");
            shouldBe(withProperty(a, true).property, 1, "property");
            shouldBe(text(withProperty(a, true)), text(viaCall("negate", a)), "property");
        }
        shouldBe(withProperty(a, false), 1, "property");
    }
}

// Two results of the same operands are two objects, in a row and in a loop.
{
    function twice(a, b) { return [a.add(b), a.add(b), 1m, 1m, new Decimal(1), new Decimal(1)]; }
    noInline(twice);
    function inLoop(a, b) {
        const results = [];
        for (let i = 0; i < 3; ++i)
            results.push(a.add(b), 2m);
        return results;
    }
    noInline(inLoop);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(new Set(twice(values[5], values[6])).size, 6, "twice");
        shouldBe(new Set(inLoop(values[5], values[6])).size, 6, "in a loop");
    }
}

// Across loops, calls, collections and handlers.
{
    function collect(i) { if (!(i % 500)) edenGC(); }
    noInline(collect);
    function sum(a, count, o) {
        let total = 0m;
        const step = a.multiply(2m);
        for (let i = 0; i < count; ++i) {
            const next = total.add(step);
            collect(i);
            try {
                total = o.fails === i ? next.add(undefined) : next;
            } catch {
                total = next.add(step);
            }
        }
        return total;
    }
    noInline(sum);
    for (let i = 0; i < testLoopCount / 10; ++i)
        shouldBe(sum(0.05m, 20, { fails: -1 }).toString(), "2", "sum");
    shouldBe(sum(0.05m, 20, { fails: 10 }).toString(), "2.1", "sum");
    shouldBe(sum(0.05m, testLoopCount * 10, { fails: 5, other: 1 }).toString(), String(testLoopCount + 0.1), "sum");
}

// What the compiler computes is what the function computes.
for (const method of ["add", "subtract", "multiply", "divide", "remainder", "abs", "negate", "mantissa"]) {
    const body = texts.map((left, i) => `new Decimal("${left}").${method}(new Decimal("${texts[(i * 5 + 3) % texts.length]}"))`);
    const f = new Function(`return [${body}];`);
    noInline(f);
    const expected = values.map((left, i) => text(viaCall(method, left, values[(i * 5 + 3) % texts.length]))).join();
    for (let i = 0; i < testLoopCount / 4; ++i)
        f();
    shouldBe(f().map(text).join(), expected, method);
}
{
    function withoutOperator() { return [1.255m.round(2), 1.255m.round(2, "floor"), (-1.255m).round(2, "ceil"), 1.5m.scale10(2), 1.5m.scale10(-7000), 1m.add(2m).multiply(3m)]; }
    noInline(withoutOperator);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(withoutOperator().join(), "1.26,1.25,-1.25,150,0,9", "folded");
}

// new Decimal(value), for each type alone, for all, and for what throws, also where the result is not used.
{
    const inputs = [0, -1, 2147483647, -2147483648, 0.1, -0, 1e21, 5e-324, 1.7976931348623157e308, NaN, -Infinity, "1.50", "-0", "1" + String(Math.random() > 2), 12n, -(10n ** 40n), undefined, null, {}, Symbol(), 1m];
    const outcome = f => { try { const result = f(); return result && text(result); } catch (error) { return error.name; } };
    const expected = inputs.map(input => outcome(() => Reflect.construct(Decimal, [input])));
    for (let warm = 0; warm <= inputs.length; ++warm) {
        const used = new Function("x", `return new Decimal(x); // ${warm}`);
        const unused = new Function("x", `const d = new Decimal(x); // ${warm}`);
        const caught = new Function("x", `const before = 1m.add(new Decimal(2)); try { return new Decimal(x); } catch { return before; } // ${warm}`);
        noInline(used);
        noInline(unused);
        noInline(caught);
        for (let i = 0; i < testLoopCount / 4; ++i) {
            const input = inputs[warm < inputs.length ? warm : i % 13];
            try {
                caught(input);
                used(input);
                unused(input);
            } catch { }
        }
        inputs.forEach((input, i) => {
            const throws = /Error/.test(expected[i]);
            shouldBe(outcome(() => used(input)), expected[i], `${warm} ${i}`);
            shouldBe(outcome(() => unused(input)), throws ? expected[i] : undefined, `${warm} ${i}`);
            shouldBe(outcome(() => caught(input)), throws ? "3e0" : expected[i], `${warm} ${i}`);
        });
    }
    function constants() { return [new Decimal(0.1), new Decimal(-0), new Decimal(7), new Decimal("-0"), new Decimal(NaN)].map(text).join(); }
    function badConstant() { try { new Decimal("1x"); } catch (error) { return error.name; } }
    class Money extends Decimal { constructor(x) { super(x); } }
    function derived(x) { return new Money(x); }
    noInline(constants);
    noInline(badConstant);
    noInline(derived);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(constants(), "1e-1,0e+0Infinity,7e0,-0e+0-Infinity,NaN", "constants");
        shouldBe(badConstant(), "SyntaxError", "constant");
        shouldBe(Object.getPrototypeOf(derived(i)), Money.prototype, "derived");
        shouldBe(Object.getPrototypeOf(derived(i).add(1m)), Decimal.prototype, "derived");
    }
}

// The value is not read before it is known that there is one.
{
    function readInLoop(a, count, reads) {
        let result = 0;
        for (let i = 0; i < count; ++i) {
            if (reads)
                result += Decimal.prototype.isNaN.call(a) ? 2 : 1;
        }
        return result;
    }
    noInline(readInLoop);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(readInLoop(values[5], 5, true), 5, "read");
        shouldBe(readInLoop([{ }, "", Symbol.iterator, 1, undefined][i % 5], 5, false), 0, "not read");
    }
}

// What does not change in a loop is computed in front of it.
for (const source of ["a.add(1m)", "a.multiply(a).subtract(a)", "a.round(k)", "a.scale10(k)", "new Decimal(k)", "new Decimal(k / 4).negate()"]) {
    const f = new Function("a", "k", "count", `let x; for (let i = 0; i < count; ++i) { for (let j = 0; j < count; ++j) { if (j & 1) x = ${source}; } } return x;`);
    const once = new Function("a", "k", `return ${source};`);
    noInline(f);
    noDFG(once);
    for (let i = 0; i < testLoopCount; ++i) {
        const a = values[i % values.length], k = i % 5 - 2;
        shouldBe(text(f(a, k, 3)), text(once(a, k)), source);
        shouldBe(f(a, k, 1), undefined, source);
    }
}
