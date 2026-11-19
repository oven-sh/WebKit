//@ requireOptions("--useDecimal=1", "--useDecimalStructuredClone=1")
// An extension: the draft has no structured clone, and neither has HTML for a Decimal.

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

// The same value, also where a Number or a shorter text would lose digits or the sign.
for (const value of values) {
    const clone = structuredClone(value);
    shouldBe(clone !== value, true);
    shouldBe(Object.getPrototypeOf(clone), Decimal.prototype);
    shouldBe(exact(clone), exact(value));
    shouldBe(Object.is(Number(clone), Number(value)), true);
    for (const nested of [structuredClone({ a: [value] }).a[0], [...structuredClone(new Map([[value, 1]])).keys()][0], structuredClone(new Map([[1, value]])).get(1), [...structuredClone(new Set([value]))][0]])
        shouldBe(exact(nested), exact(value));
}
{
    // Identity within the graph is kept, and positions of other objects in it are not disturbed.
    const x = new Decimal("1.5"), y = new Decimal("1.5"), o = {};
    const big = Object(1n), date = new Date(5);
    const clone = structuredClone([x, o, x, y, big, o, date, x, big, { x }]);
    shouldBe(clone[0] === clone[2] && clone[0] === clone[7] && clone[0] === clone[9].x, true);
    shouldBe(clone[0] !== clone[3], true);
    shouldBe(clone[1] === clone[5] && clone[4] === clone[8], true);
    shouldBe(clone[6].getTime(), 5);
    shouldBe(exact(clone[7]), "1.5e0");

    // A cycle through other objects.
    const cycle = { x };
    cycle.self = cycle;
    const cloned = structuredClone(cycle);
    shouldBe(cloned.self === cloned, true);
    shouldBe(exact(cloned.x), "1.5e0");

    // As with every other object that has a value: no own properties, no subclass.
    class Derived extends Decimal { }
    const derived = new Derived("2");
    derived.own = 1;
    shouldBe(Object.getPrototypeOf(structuredClone(derived)), Decimal.prototype);
    shouldBe(Object.hasOwn(structuredClone(derived), "own"), false);

    // Nothing that can be replaced is called, and the clone is of the realm of the function.
    const { toString, toExponential } = Decimal.prototype;
    Decimal.prototype.toString = Decimal.prototype.toExponential = Decimal.prototype.toJSON = () => { throw new Error("called"); };
    const D = Decimal;
    globalThis.Decimal = null;
    const hidden = structuredClone(x);
    globalThis.Decimal = D;
    Object.assign(Decimal.prototype, { toString, toExponential });
    shouldBe(exact(hidden), "1.5e0");
    const other = createGlobalObject();
    shouldBe(Object.getPrototypeOf(other.structuredClone(x)), other.Decimal.prototype);
    shouldBe(Object.getPrototypeOf(structuredClone(new other.Decimal("1"))), Decimal.prototype);

    // Not Decimals.
    shouldThrow(() => structuredClone(Decimal), TypeError);
    shouldThrow(() => structuredClone(Decimal.prototype), TypeError);
    shouldThrow(() => structuredClone(new Proxy(x, {})), TypeError);
    for (let i = 0; i < 1000; ++i)
        structuredClone([x, y]);
    gc();
}
