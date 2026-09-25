// The arguments object of a sloppy function is a DirectArguments, or a ScopedArguments when a closure captures a
// parameter. Once its "length" is overridden it is an ordinary array-like: a consumer reads "length" once and
// converts it with ToLength. It was converted with ToUint32, so -1 was 4294967295 and 2 ** 32 + 1 was 1.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

function shouldThrow(func, errorConstructor)
{
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorConstructor))
        throw new Error(`bad error: ${String(error)}`);
}

function count()
{
    return arguments.length;
}

function Count()
{
    this.count = arguments.length;
}

// A function that passes its own arguments object on. One copy overrides the length while it warms up. The other
// copy is compiled before it overrides one.

function compile(body)
{
    let func = (0, eval)(`(function (length) { ${body} })`);
    noInline(func);
    return func;
}

const forwarders = {
    "apply, direct": "if (length !== undefined) arguments.length = length; return count.apply(null, arguments);",
    "apply, scoped": "(function () { return length; }); if (length !== undefined) arguments.length = length; return count.apply(null, arguments);",
    "slice, direct": "if (length !== undefined) arguments.length = length; return Array.prototype.slice.call(arguments).length;",
    "slice, scoped": "(function () { return length; }); if (length !== undefined) arguments.length = length; return Array.prototype.slice.call(arguments).length;",
};

for (let name in forwarders) {
    let warm = compile(forwarders[name]);
    let cold = compile(forwarders[name]);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(warm(undefined, 1, 2), 3);
        shouldBe(warm(1, 1, 2), 1);
        shouldBe(cold(undefined, 1, 2), 3);
    }
    for (let forward of [cold, warm]) {
        for (let i = 0; i < 10; ++i) {
            shouldBe(forward(-1, 1, 2), 0);
            shouldBe(forward(-(2 ** 31), 1, 2), 0);
            shouldBe(forward(3.9, 1, 2), 3);
            shouldThrow(() => forward(2 ** 32, 1, 2), RangeError);
            shouldThrow(() => forward(2 ** 32 + 1, 1, 2), RangeError);
            shouldThrow(() => forward(Infinity, 1, 2), RangeError);
        }
    }
}

// An unguarded decrement in a call without arguments makes the length -1.
const decrementers = [
    compile("arguments.length--; return count.apply(null, arguments);"),
    compile("arguments.length--; return Array.prototype.slice.call(arguments).length;"),
];
for (let decrement of decrementers) {
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(decrement(1, 2), 1);
    for (let i = 0; i < 10; ++i)
        shouldBe(decrement(), 0);
}

// Every consumer, against a plain object with the same elements and the same length.

let lengthGets = 0;
let valueOfCalls = 0;

function lengthObject(value)
{
    return { valueOf() { ++valueOfCalls; return value; } };
}

const kinds = {
    direct() { return (function () { return arguments; })(1, 2); },
    scoped() { return (function (a, b) { (function () { return a + b; }); return arguments; })(1, 2); },
};

function plain()
{
    return { 0: 1, 1: 2, length: 2 };
}

const overrides = {
    assign(object, length) { object.length = length; },
    define(object, length) { Object.defineProperty(object, "length", { value: length, writable: true, configurable: true }); },
    getter(object, length) { Object.defineProperty(object, "length", { get() { ++lengthGets; return length; }, configurable: true }); },
    inherit(object, length) { delete object.length; Object.setPrototypeOf(object, { length }); },
};

// ToLength of each of these is 3 or less, so an operation that visits every index ends at once.
const smallLengths = [0, 1, 2, 3, 1.9, "3", NaN, undefined, -0.5, -1, -(2 ** 31), -(2 ** 32) - 1, -Infinity, () => lengthObject(1), () => lengthObject(-1)];
// ToUint32 of these is 0, 1, 0, 0 and 2.
const largeLengths = [2 ** 32, 2 ** 32 + 1, 2 ** 53, Infinity, () => lengthObject(2 ** 32 + 2)];

// large: the operation does not visit every index. storesLength: the operation also stores "length".
const operations = {
    "f.apply": { large: true, run(object) { return count.apply(null, object); } },
    "Function.prototype.apply.call": { large: true, run(object) { return Function.prototype.apply.call(count, null, object); } },
    "Reflect.apply": { large: true, run(object) { return Reflect.apply(count, null, object); } },
    "Reflect.construct": { large: true, run(object) { return Reflect.construct(Count, object).count; } },
    "Reflect.construct, stored": { run(object) { let construct = Reflect.construct; return construct(Count, object).count; } },
    "Math.max.apply": { large: true, run(object) { return Math.max.apply(null, object); } },
    "slice(0, 3)": { large: true, run(object) { return JSON.stringify(Array.prototype.slice.call(object, 0, 3)); } },
    "slice(-1)": { large: true, run(object) { return JSON.stringify(Array.prototype.slice.call(object, -1)); } },
    "slice()": { run(object) { return JSON.stringify(Array.prototype.slice.call(object)); } },
    "indexOf(2)": { large: true, run(object) { return Array.prototype.indexOf.call(object, 2); } },
    "indexOf(undefined)": { run(object) { return Array.prototype.indexOf.call(object, undefined); } },
    "lastIndexOf(1, 3)": { large: true, run(object) { return Array.prototype.lastIndexOf.call(object, 1, 3); } },
    "includes(2)": { large: true, run(object) { return Array.prototype.includes.call(object, 2); } },
    "includes(undefined)": { large: true, run(object) { return Array.prototype.includes.call(object, undefined); } },
    "at(-1)": { large: true, run(object) { return Array.prototype.at.call(object, -1); } },
    "with(0, 9)": { large: true, run(object) { return JSON.stringify(Array.prototype.with.call(object, 0, 9)); } },
    "toReversed()": { large: true, run(object) { return JSON.stringify(Array.prototype.toReversed.call(object)); } },
    "toSorted()": { large: true, run(object) { return JSON.stringify(Array.prototype.toSorted.call(object)); } },
    "toSpliced(0, 0)": { large: true, run(object) { return JSON.stringify(Array.prototype.toSpliced.call(object, 0, 0)); } },
    "join()": { run(object) { return Array.prototype.join.call(object); } },
    "toLocaleString()": { run(object) { return Array.prototype.toLocaleString.call(object); } },
    "flat()": { run(object) { return JSON.stringify(Array.prototype.flat.call(object)); } },
    "concat": { run(object) { object[Symbol.isConcatSpreadable] = true; return JSON.stringify([0].concat(object)); } },
    "reverse()": { run(object) { Array.prototype.reverse.call(object); return `${object[0]} ${object[1]}`; } },
    "sort()": { run(object) { object[0] = 2; object[1] = 1; Array.prototype.sort.call(object); return `${object[0]} ${object[1]}`; } },
    "fill(7)": { run(object) { Array.prototype.fill.call(object, 7); return `${object[0]} ${object[1]} ${object[2]}`; } },
    "fill(7, 0, 2)": { large: true, run(object) { Array.prototype.fill.call(object, 7, 0, 2); return `${object[0]} ${object[1]}`; } },
    "copyWithin(0, 1, 2)": { large: true, run(object) { Array.prototype.copyWithin.call(object, 0, 1, 2); return `${object[0]} ${object[1]}`; } },
    "String.raw": { run(object) { object[0] = "a"; object[1] = "b"; return String.raw({ raw: object }, 1, 2, 3); } },
    "ownKeys": { run(object) { object[0] = "a"; object[1] = "b"; return JSON.stringify(Reflect.ownKeys(new Proxy({ }, { ownKeys() { return object; } }))); } },
    "push(7)": { storesLength: true, large: true, run(object) { return `${Array.prototype.push.call(object, 7)} ${object.length} ${object[0]}`; } },
    "pop()": { storesLength: true, large: true, run(object) { return `${Array.prototype.pop.call(object)} ${object.length} ${object[1]}`; } },
    "shift()": { storesLength: true, run(object) { return `${Array.prototype.shift.call(object)} ${object.length} ${object[0]}`; } },
    "unshift()": { storesLength: true, large: true, run(object) { return `${Array.prototype.unshift.call(object)} ${object.length}`; } },
    "unshift(7)": { storesLength: true, run(object) { return `${Array.prototype.unshift.call(object, 7)} ${object.length} ${object[0]}`; } },
    "splice(0, 0)": { storesLength: true, large: true, run(object) { return `${JSON.stringify(Array.prototype.splice.call(object, 0, 0))} ${object.length}`; } },
    "splice(0, 1)": { storesLength: true, run(object) { return `${JSON.stringify(Array.prototype.splice.call(object, 0, 1))} ${object.length} ${object[0]}`; } },
};
for (let name in operations)
    noInline(operations[name].run);

function outcome(operation, object, override, length)
{
    lengthGets = 0;
    valueOfCalls = 0;
    let result;
    try {
        override(object, typeof length === "function" ? length() : length);
        result = String(operation.run(object));
    } catch (error) {
        result = error.constructor.name;
    }
    return `${result}, ${lengthGets} gets, ${valueOfCalls} valueOf calls`;
}

function describeLength(length)
{
    if (typeof length === "function")
        return `{ valueOf: ${length().valueOf()} }`;
    return typeof length === "string" ? `"${length}"` : String(length);
}

function check(actual, expected, name, kind, length, how)
{
    if (actual !== expected)
        throw new Error(`${name} on a ${kind} arguments object, length ${describeLength(length)} by ${how}: ${actual}, expected: ${expected}`);
}

function testAgainstPlainObject()
{
    for (let kind in kinds) {
        for (let how in overrides) {
            for (let name in operations) {
                let operation = operations[name];
                // Without a setter the store throws, and with an inherited length it makes an own one.
                if (operation.storesLength && (how === "getter" || how === "inherit"))
                    continue;
                for (let length of operation.large ? smallLengths.concat(largeLengths) : smallLengths) {
                    let expected = outcome(operation, plain(), overrides[how], length);
                    let actual = outcome(operation, kinds[kind](), overrides[how], length);
                    check(actual, expected, name, kind, length, how);
                }
            }
        }
    }
}

// The values themselves, for lengths where ToUint32 and ToLength differ.
function testValues()
{
    const expected = [
        ["f.apply", -1, "0"],
        ["f.apply", -(2 ** 31), "0"],
        ["f.apply", 2 ** 32, "RangeError"],
        ["f.apply", 2 ** 32 + 1, "RangeError"],
        ["f.apply", Infinity, "RangeError"],
        ["Function.prototype.apply.call", -1, "0"],
        ["Function.prototype.apply.call", 2 ** 32 + 1, "RangeError"],
        ["Reflect.apply", -1, "0"],
        ["Reflect.apply", 2 ** 32 + 1, "RangeError"],
        ["Reflect.construct", -1, "0"],
        ["Reflect.construct", 2 ** 32 + 1, "RangeError"],
        ["Reflect.construct, stored", -1, "0"],
        ["Math.max.apply", -1, "-Infinity"],
        ["Math.max.apply", 2 ** 32 + 1, "RangeError"],
        ["slice(0, 3)", -1, "[]"],
        ["slice(0, 3)", 2 ** 32 + 1, "[1,2,null]"],
        ["slice(-1)", 2 ** 32 + 1, "[null]"],
        ["slice()", -1, "[]"],
        ["indexOf(2)", -1, "-1"],
        ["indexOf(2)", 2 ** 32, "1"],
        ["indexOf(undefined)", -1, "-1"],
        ["lastIndexOf(1, 3)", -1, "-1"],
        ["lastIndexOf(1, 3)", 2 ** 32 + 1, "0"],
        ["includes(undefined)", -1, "false"],
        ["includes(undefined)", 2 ** 32, "true"],
        ["at(-1)", 2 ** 32 + 1, "undefined"],
        ["with(0, 9)", 2 ** 32 + 1, "RangeError"],
        ["toReversed()", -1, "[]"],
        ["toReversed()", 2 ** 32 + 1, "RangeError"],
        ["toSorted()", 2 ** 32 + 1, "RangeError"],
        ["toSpliced(0, 0)", -1, "[]"],
        ["toSpliced(0, 0)", 2 ** 32, "RangeError"],
        ["join()", -1, ""],
        ["toLocaleString()", -1, ""],
        ["flat()", -1, "[]"],
        ["concat", -1, "[0]"],
        ["reverse()", -1, "1 2"],
        ["sort()", -1, "2 1"],
        ["fill(7)", -1, "1 2 undefined"],
        ["fill(7, 0, 2)", 2 ** 32, "7 7"],
        ["copyWithin(0, 1, 2)", 2 ** 32, "2 2"],
        ["String.raw", -1, ""],
        ["ownKeys", -1, "[]"],
        ["push(7)", -1, "1 1 7"],
        ["push(7)", 2 ** 32 + 1, "4294967298 4294967298 1"],
        ["push(7)", 2 ** 53, "TypeError"],
        ["pop()", -1, "undefined 0 2"],
        ["pop()", 2 ** 32 + 1, "undefined 4294967296 2"],
        ["shift()", -1, "undefined 0 1"],
        ["unshift()", -1, "0 0"],
        ["unshift()", 2 ** 32 + 1, "4294967297 4294967297"],
        ["unshift(7)", -1, "1 1 7"],
        ["splice(0, 0)", -1, "[] 0"],
        ["splice(0, 0)", 2 ** 32 + 1, "[] 4294967297"],
        ["splice(0, 1)", -1, "[] 0 1"],
    ];
    for (let kind in kinds) {
        for (let how of ["assign", "define"]) {
            for (let [name, length, result] of expected) {
                let actual = outcome(operations[name], kinds[kind](), overrides[how], length);
                check(actual, `${result}, 0 gets, 0 valueOf calls`, name, kind, length, how);
            }
        }
    }
}

// One [[Get]] of "length" and one conversion for each operation.
function testLengthIsReadOnce()
{
    for (let kind in kinds) {
        for (let name in operations) {
            let operation = operations[name];
            if (operation.storesLength)
                continue;
            for (let value of operation.large ? [1, -1, 2 ** 32 + 1] : [1, -1]) {
                let actual = outcome(operation, kinds[kind](), overrides.getter, () => lengthObject(value));
                if (!actual.endsWith(", 1 gets, 1 valueOf calls"))
                    throw new Error(`${name} on a ${kind} arguments object, length ${value}: ${actual}`);
            }
        }
    }
}

// What the getter or the conversion throws is what the operation throws.
function testErrors()
{
    class LengthError extends Error { }
    const bad = [
        [LengthError, object => { Object.defineProperty(object, "length", { get() { throw new LengthError; } }); }],
        [LengthError, object => { object.length = { valueOf() { throw new LengthError; } }; }],
        [TypeError, object => { object.length = Symbol(); }],
        [TypeError, object => { object.length = 1n; }],
    ];
    for (let kind in kinds) {
        for (let name in operations) {
            for (let [errorConstructor, override] of bad) {
                let object = kinds[kind]();
                override(object);
                shouldThrow(() => operations[name].run(object), errorConstructor);
            }
        }
    }
}

// "length" is the argument count when nothing is overridden. It still is after an element is deleted or redefined,
// although a DirectArguments then says that it overrode things.
function testArgumentCount()
{
    const changes = [
        object => { },
        object => { delete object[1]; },
        object => { Object.defineProperty(object, 0, { value: 3, writable: true, enumerable: true, configurable: true }); },
    ];
    for (let kind in kinds) {
        for (let name in operations) {
            for (let change of changes) {
                let expected = plain();
                change(expected);
                let object = kinds[kind]();
                change(object);
                shouldBe(String(operations[name].run(object)), String(operations[name].run(expected)));
            }
        }
    }
}

for (let i = 0; i < 3; ++i)
    testAgainstPlainObject();
for (let i = 0; i < 10; ++i) {
    testValues();
    testLengthIsReadOnce();
    testErrors();
    testArgumentCount();
}
