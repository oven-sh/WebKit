//@ requireOptions("--useDecimal=1")
// Text, and JSON as in https://tc39.es/proposal-decimal/#sec-decimal.prototype.tojson.

function shouldBe(actual, expected, what = "") {
    if (!Object.is(actual, expected))
        throw new Error(`${what} bad value: ${String(actual)}, expected ${String(expected)}`);
}

const values = [
    "NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5", "0.1", "1e-7", "1e34", "-1e6144", "1e-6176", "-9.999999999999999999999999999999999e6144",
    "9999999999999999999999999999999999", "1.234567890123456789012345678901234", "-1.234567890123456789012345678901234e-6143",
].map(text => new Decimal(text));
const exact = value => value.toExponential();

// Every text form of a value is read back as that value.
for (const value of values) {
    for (const text of [value.toString(), value.toExponential(), String(value), `${value}`, [value].join(), value.toPrecision({})])
        shouldBe(exact(new Decimal(text)), exact(value), text);
    if (value.isFinite())
        shouldBe(exact(new Decimal(value.toFixed({ digits: 6176 }))), exact(value));
}

// JSON.stringify() throws wherever the Decimal is, as for a BigInt, and does not write an empty object instead.
{
    const original = Decimal.prototype.toJSON;
    const name = func => {
        try {
            return func();
        } catch (error) {
            return error.name;
        }
    };
    const x = new Decimal("1.5");
    for (const value of [x, [x], { a: x }, { a: { b: [1, x] } }, [[x]], [1, "a", x], Object.create(null, { a: { value: x, enumerable: true } }), [...Array(50).keys(), x]]) {
        for (const space of [undefined, 2, "\t"]) {
            for (const replacer of [undefined, ["a", "b"], (key, value) => value])
                shouldBe(name(() => JSON.stringify(value, replacer, space)), "TypeError");
        }
    }
    // A function of the program takes over, on the prototype or on the object.
    Decimal.prototype.toJSON = function () { return this.toString(); };
    shouldBe(JSON.stringify({ a: [x, { b: new Decimal("1e-7") }] }), '{"a":["1.5",{"b":"1e-7"}]}');
    Decimal.prototype.toJSON = function () { return JSON.rawJSON(this.toString()); };
    shouldBe(JSON.stringify([new Decimal("0.1234567890123456789012345678901234")]), "[0.1234567890123456789012345678901234]");
    Decimal.prototype.toJSON = original;
    const own = new Decimal("2");
    own.toJSON = () => "own";
    shouldBe(JSON.stringify([own]), '["own"]');
    // All digits of a number can be read.
    const read = JSON.parse("[0.1234567890123456789012345678901234, 1e400]", (key, value, context) => typeof value === "number" ? new Decimal(context.source) : value);
    shouldBe(exact(read[0]), "1.234567890123456789012345678901234e-1");
    shouldBe(exact(read[1]), "1e400");
}
