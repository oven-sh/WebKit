//@ requireOptions("--useDecimal=1", "--useDecimalToJSON=1")
// Not in the proposal, where toJSON() throws: the exact value, as a String.

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

const values = [
    "NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5", "0.1", "1e-7", "1e34", "-1e6144", "1e-6176", "-9.999999999999999999999999999999999e6144",
    "9999999999999999999999999999999999", "1.234567890123456789012345678901234", "-1.234567890123456789012345678901234e-6143",
].map(text => new Decimal(text));
const exact = value => value.toExponential();

for (const value of values) {
    shouldBe(value.toJSON(), value.toString());
    shouldBe(value.toJSON("key"), value.toString());
    shouldBe(JSON.stringify(value), JSON.stringify(value.toString()));
    // It is read back exactly.
    for (const wrapped of [{ a: value }, [value], { a: { b: [0, value] } }]) {
        for (const space of [undefined, 2]) {
            const read = JSON.parse(JSON.stringify(wrapped, null, space), (key, value) => typeof value === "string" ? new Decimal(value) : value);
            const found = Array.isArray(read) ? read[0] : read.a.b ? read.a.b[1] : read.a;
            shouldBe(exact(found), exact(value));
        }
    }
}
shouldBe(JSON.stringify({ price: new Decimal("19.90"), list: [new Decimal("-0"), new Decimal("1e34")] }), '{"price":"19.9","list":["-0","1e34"]}');
// A replacer gets the String, and the Decimal as the property of its holder.
JSON.stringify({ a: new Decimal("1.5") }, function (key, value) {
    if (key === "a") {
        shouldBe(value, "1.5");
        shouldBe(this[key] instanceof Decimal, true);
    }
    return value;
});
for (const receiver of [undefined, null, 1, "1", {}, Decimal.prototype, new Proxy(new Decimal("1"), {})])
    shouldThrow(() => Decimal.prototype.toJSON.call(receiver), TypeError);
shouldThrow(() => JSON.stringify({ toJSON: Decimal.prototype.toJSON }), TypeError);
shouldBe(Decimal.prototype.toJSON.length, 0);

// The many ways into the fast paths of JSON.stringify().
for (let i = 0; i < testLoopCount / 10; ++i) {
    shouldBe(JSON.stringify({ n: i, d: values[5] }), `{"n":${i},"d":"1"}`);
    shouldBe(JSON.stringify([i, values[6], "x"]), `[${i},"-1.5","x"]`);
}
