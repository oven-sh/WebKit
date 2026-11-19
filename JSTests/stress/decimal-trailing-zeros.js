//@ requireOptions("--useDecimal=1")
// Optimized code leaves the zeros at the end of what it only computes with. Nothing shows that.

const P = Decimal.prototype;
function viaCall(method, receiver, ...rest) { return P[method].call(receiver, ...rest); }
noInline(viaCall);
noDFG(viaCall);
function make(value) { return new Decimal(value); }
noInline(make);
noDFG(make);

// Two representations of a value are not equal, and have two texts.
function describe(value) {
    if (!(value instanceof Decimal))
        return Object.is(value, -0) ? "-0" : String(value);
    const text = viaCall("toExponential", value);
    if (!viaCall("isNaN", value) && !viaCall("equals", value, make(text.replace("e+0", ""))))
        throw new Error(`${text} is not equal to itself`);
    return text;
}

const values = ["0.5", "-0.5", "1.5", "2", "-4", "5", "0.25", "0.75", "12.5", "8", "-125", "999.5", "0.0625", "16", "1e3", "2e-3", "5e18", "4611686018427387900", "0", "-0", "NaN", "Infinity",
    "2e6110", "5e3055", "2e3056", "5e-3088", "2e-3088", "5e-6176", "1234567890123456789012345678901230", "9223372036854775810"].map(make);
const integers = [0, 10, -10, 100, 1200, 7, 1000000000, -2147483640];

const producers = {
    "a.add(b)": (a, b) => viaCall("add", a, b),
    "a.subtract(b)": (a, b) => viaCall("subtract", a, b),
    "a.multiply(b)": (a, b) => viaCall("multiply", a, b),
    "a.divide(0.5m)": a => viaCall("divide", a, make("0.5")),
    "a.round(0)": a => viaCall("round", a, 0),
    "a.round(-1, 'ceil')": a => viaCall("round", a, -1, "ceil"),
    "new Decimal(n)": (a, b, n) => make(n),
};
const consumers = {};
for (const method of ["add", "subtract", "multiply", "divide", "remainder", "equals", "notEquals", "lessThan", "lessThanOrEqual", "greaterThan", "greaterThanOrEqual", "compare"]) {
    consumers[`x.${method}(c)`] = (x, c) => viaCall(method, x, c);
    consumers[`c.${method}(x)`] = (x, c) => viaCall(method, c, x);
    consumers[`x.${method}(x)`] = x => viaCall(method, x, x);
}
for (const method of ["abs", "negate", "mantissa", "exponent", "isNaN", "isFinite", "toString", "toExponential", "toFixed"])
    consumers[`x.${method}()`] = x => viaCall(method, x);
for (const mode of ["ceil", "floor", "trunc", "halfExpand", "halfEven"]) {
    for (const digits of [-2, 0, 1, 2])
        consumers[`x.round(${digits}, "${mode}")`] = x => viaCall("round", x, digits, mode);
}
consumers["x.round(digits)"] = x => viaCall("round", x, 1);
consumers["x.round(digits + 0.5 - 0.5)"] = x => viaCall("round", x, 1);
consumers["x.scale10(1)"] = x => viaCall("scale10", x, 1);
for (const constant of ["-2", "-0.5", "123456789012345678901234567891"]) {
    for (const method of ["add", "subtract", "multiply", "remainder", "compare"]) {
        consumers[`x.${method}(${constant}m)`] = x => viaCall(method, x, make(constant));
        consumers[`(${constant}m).${method}(x)`] = x => viaCall(method, make(constant), x);
    }
}
consumers["x.divide(c).scale10(1)"] = (x, c) => viaCall("scale10", viaCall("divide", x, c), 1);
consumers["x.remainder(c).round(1)"] = (x, c) => viaCall("round", viaCall("remainder", x, c), 1);
consumers["x.divide(c).round(1)"] = (x, c) => viaCall("round", viaCall("divide", x, c), 1);
consumers["c.divide(x).round(1)"] = (x, c) => viaCall("round", viaCall("divide", c, x), 1);

for (const [producer, produce] of Object.entries(producers)) {
    // Each consumer has a value of its own, or one that needs no zeros would decide for all.
    const names = Object.keys(consumers);
    for (let first = 0; first < names.length; first += 12) {
        const some = names.slice(first, first + 12);
        const f = new Function("a", "b", "c", "n", "digits", `return [${some.map((consumer, i) => consumer.replace(/\bx\b/, `(x = ${producer})`).replace(/^/, "(").concat(")")).join()}]; var x;`);
        noInline(f);
        const argumentsOf = i => [values[i % values.length], values[Math.floor(i / values.length) % values.length], values[i * 7 % values.length], integers[i % integers.length], 1];
        for (let i = 0; i < testLoopCount; ++i)
            f(...argumentsOf(i));
        for (let i = 0; i < values.length ** 2; i += 7) {
            const [a, b, c, n] = argumentsOf(i), x = produce(a, b, n);
            f(a, b, c, n, 1).forEach((result, j) => {
                if (describe(result) !== describe(consumers[some[j]](x, c)))
                    throw new Error(`${some[j]} of ${producer} with ${describe(a)}, ${describe(b)}, ${describe(c)}, ${n}: got ${describe(result)}`);
            });
        }
    }

    // The code is left while the value has them. The call has never run, so it is not compiled.
    const f = new Function("a", "b", "n", "overflows", "hand", `const x = ${producer}; if (overflows + 1 === 2) return x.lessThan(b); return hand(x);`);
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        f(values[i % values.length], values[i * 7 % values.length], integers[i % integers.length], 1);
    for (const [a, b, n] of [["0.5", "1.5", 100], ["12.5", "8", -10], ["999.5", "0.5", 1200]]) {
        if (describe(f(make(a), make(b), n, 2147483647, x => x)) !== describe(produce(make(a), make(b), n)))
            throw new Error(`${producer} after an exit`);
    }
}

// From one turn of a loop to the next, and where two ways join.
function sum(list) {
    let total = 0m;
    for (let i = 0; i < list.length; ++i)
        total = total.add(list[i].multiply(list[0]));
    return total;
}
noInline(sum);
function either(a, b, which) {
    const x = which ? a.add(b) : a.multiply(b);
    return x.equals(2m);
}
noInline(either);
for (let i = 0; i < testLoopCount; ++i) {
    if (describe(sum([values[0], values[2], values[3], values[15]])) !== "2.001e0" || describe(sum([values[0], values[2]])) !== "1e0")
        throw new Error("sum");
    if (!either(values[0], values[2], true) || !either(values[1], values[4], false))
        throw new Error("either");
}
