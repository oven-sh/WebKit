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

// The global, the constructor and the prototype.
{
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, "Decimal");
    shouldBe(descriptor.value, Decimal);
    shouldBe(descriptor.writable, true);
    shouldBe(descriptor.enumerable, false);
    shouldBe(descriptor.configurable, true);
    shouldBe(Decimal.name, "Decimal");
    shouldBe(Decimal.length, 1);
    shouldBe(Object.getPrototypeOf(Decimal), Function.prototype);
    shouldBe(Object.getPrototypeOf(Decimal.prototype), Object.prototype);
    shouldBe(Decimal.prototype.constructor, Decimal);
    const prototype = Object.getOwnPropertyDescriptor(Decimal, "prototype");
    shouldBe(prototype.writable || prototype.enumerable || prototype.configurable, false);
    shouldBe(typeof d("1"), "object");

    const lengths = {
        isNaN: 0, isFinite: 0, exponent: 0, mantissa: 0, abs: 0, negate: 0,
        add: 1, subtract: 1, multiply: 1, divide: 1, remainder: 1,
        compare: 1, equals: 1, notEquals: 1, lessThan: 1, lessThanOrEqual: 1, greaterThan: 1, greaterThanOrEqual: 1,
        round: 1, scale10: 1, toString: 0, toLocaleString: 0, toExponential: 0, toFixed: 0, toPrecision: 0, toJSON: 0, valueOf: 1,
    };
    shouldBe(Reflect.ownKeys(Decimal.prototype).sort().join(), ["constructor", ...Object.keys(lengths)].sort().join());
    for (const [name, length] of Object.entries(lengths)) {
        const method = Decimal.prototype[name];
        shouldBe(method.length, length, name);
        shouldBe(method.name, name);
        shouldBe(Object.getOwnPropertyDescriptor(Decimal.prototype, name).enumerable, false);
        shouldThrow(() => new method(), TypeError);
        // The prototype itself is not a Decimal.
        for (const receiver of [undefined, null, 1, "1", 1n, {}, Decimal.prototype, new Proxy(d("1"), {})])
            shouldThrow(() => method.call(receiver, d("1")), TypeError);
        if (name !== "toJSON" && name !== "valueOf" && length && name !== "round" && name !== "scale10") {
            for (const operand of [undefined, null, 1, "1", 1n, {}, Decimal.prototype])
                shouldThrow(() => d("1")[name](operand), TypeError);
        }
    }
}
