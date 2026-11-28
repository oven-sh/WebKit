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

// The attributes of every property.
{
    const checkFunction = (f, length, name) => {
        for (const [key, value] of [["length", length], ["name", name]]) {
            const descriptor = Object.getOwnPropertyDescriptor(f, key);
            shouldBe(descriptor.value, value, name + key);
            shouldBe(JSON.stringify([descriptor.writable, descriptor.enumerable, descriptor.configurable]), "[false,false,true]", name + key);
        }
    };
    checkFunction(Decimal, 1, "Decimal");
    for (const key of Reflect.ownKeys(Decimal.prototype)) {
        const descriptor = Object.getOwnPropertyDescriptor(Decimal.prototype, key);
        shouldBe(JSON.stringify([descriptor.writable, descriptor.enumerable, descriptor.configurable]), "[true,false,true]", key);
        shouldBe(typeof descriptor.value, "function", key);
        if (key !== "constructor") {
            checkFunction(descriptor.value, descriptor.value.length, key);
            shouldBe(Object.hasOwn(descriptor.value, "prototype"), false, key);
        }
    }
}

const makers = [["abs"], ["negate"], ["mantissa"], ["add", "x"], ["subtract", "x"], ["multiply", "x"], ["divide", "x"], ["remainder", "x"], ["round", 1], ["scale10", 1]];

// Results and errors are of the realm of the method.
{
    const other = createGlobalObject();
    for (const [name, argument] of makers) {
        const local = d("1.5"), foreign = new other.Decimal("1.5");
        for (const [receiver, operand] of [[local, foreign], [foreign, local], [local, local], [foreign, foreign]]) {
            for (const realm of [globalThis, other]) {
                const result = realm.Decimal.prototype[name].call(receiver, argument === "x" ? operand : argument);
                shouldBe(Object.getPrototypeOf(result), realm.Decimal.prototype, name);
            }
        }
    }
    for (const name of Object.getOwnPropertyNames(Decimal.prototype)) {
        if (name === "constructor")
            continue;
        shouldThrow(() => other.Decimal.prototype[name].call(1), other.TypeError);
    }
    shouldThrow(() => other.Decimal.prototype.round.call(d("1"), 0.5), other.RangeError);
    shouldThrow(() => other.Decimal.prototype.scale10.call(d("1"), 0.5), other.RangeError);
    shouldThrow(() => other.Decimal.prototype.toFixed.call(d("1"), { digits: -1 }), other.RangeError);
    shouldThrow(() => other.Decimal.prototype.add.call(d("1"), 1), other.TypeError);
    shouldThrow(() => other.BigInt(d("1.5")), other.RangeError);
}

// The operand is checked whatever the receiver is.
for (const name of ["add", "subtract", "multiply", "divide", "remainder", "compare", "equals", "notEquals", "lessThan", "lessThanOrEqual", "greaterThan", "greaterThanOrEqual"]) {
    for (const receiver of ["NaN", "Infinity", "-Infinity", "0", "-0", "1"]) {
        for (const operand of [undefined, null, 1, NaN, "1", 1n, {}, Decimal.prototype, new Proxy(d("1"), {}), Object.create(d("1"))])
            shouldThrow(() => d(receiver)[name](operand), TypeError);
    }
}

// Results are new objects, also where they have the value of an operand.
for (const text of ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5"]) {
    const x = d(text);
    for (const [name, argument] of makers) {
        for (const operand of argument === "x" ? [x, d("0"), d("-0"), d("1"), d("Infinity"), d("NaN")] : [argument]) {
            const result = x[name](operand);
            shouldBe(result !== x && result !== operand, true, `${text} ${name}`);
            shouldBe(result !== x[name](operand), true, `${text} ${name}`);
        }
    }
    shouldBe(x.toExponential(), d(text).toExponential());
}
