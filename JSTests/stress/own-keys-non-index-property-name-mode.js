// PropertyNameMode::NonIndexStrings and NonIndexStringsAndSymbols: an object's getOwnPropertyNames() adds no key for an element,
// a character or a mapped argument it holds, and every other key as before. $vm.ownNonIndexKeys(object, symbols = true,
// dontEnum = true) returns what a PropertyNameArrayBuilder in one of those modes collects.

var abort = $vm.abort;

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldBeKeys(actual, expected, message) {
    let show = keys => "[" + keys.map(String).join(", ") + "]";
    if (actual.length !== expected.length || actual.some((key, i) => key !== expected[i]))
        throw new Error(message + ": expected " + show(expected) + " but got " + show(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(message + ": expected " + errorType.name + " but got " + String(error));
}

function isIndexKey(key) {
    if (typeof key !== "string")
        return false;
    let index = key >>> 0;
    return String(index) === key && index !== 0xFFFFFFFF;
}

// The keys of [[OwnPropertyKeys]] in their order, less the index keys and what the two flags leave out.
function ownKeysLessIndexKeys(object, symbols, dontEnum) {
    return Reflect.ownKeys(object).filter(key => {
        if (isIndexKey(key))
            return false;
        if (!symbols && typeof key === "symbol")
            return false;
        return dontEnum || Object.getOwnPropertyDescriptor(object, key).enumerable;
    });
}

// `all`, when given, is what the object gives with both flags on. Every combination of the flags is compared with [[OwnPropertyKeys]].
function check(object, all, message) {
    shouldBe(Reflect.ownKeys(object).some(isIndexKey), true, message + ": has index keys");
    if (all)
        shouldBeKeys($vm.ownNonIndexKeys(object), all, message);
    for (let symbols of [true, false]) {
        for (let dontEnum of [true, false])
            shouldBeKeys($vm.ownNonIndexKeys(object, symbols, dontEnum), ownKeysLessIndexKeys(object, symbols, dontEnum), message + " symbols:" + symbols + " dontEnum:" + dontEnum);
    }
}

let symbol = Symbol("s");
let hidden = Symbol("hidden");

// Neither "4294967295" nor "01" is an array index: 2^32 - 1 is past the last one, and "01" is not canonical.
// A typed array takes "4294967295", a canonical numeric string, for an element that is out of bounds: it gets the other keys.
function decorate(object, keys = decoration) {
    for (let key of keys)
        Object.defineProperty(object, key, { value: 1, writable: true, enumerable: key !== "hiddenNamed" && key !== hidden, configurable: true });
    return object;
}
let decoration = ["named", "hiddenNamed", "4294967295", "01", symbol, hidden];
let typedArrayDecoration = decoration.filter(key => key !== "4294967295");

// Ordinary objects and arrays, in each shape of element storage.
{
    let sparse = [1, 2, 3];
    sparse[100000] = 4;
    let withHiddenElement = [1, 2, 3];
    Object.defineProperty(withHiddenElement, 1, { value: 2, enumerable: false });
    let arrays = {
        int32: [1, 2, 3],
        int32WithHole: [1, , 3],
        contiguous: [{ }, "a", null],
        double: [1.5, 2.5, 3.5],
        doubleWithHole: [1.5, , 3.5],
        sparse,
        withHiddenElement,
    };
    for (let [name, array] of Object.entries(arrays))
        check(decorate(array), ["length", ...decoration], "array " + name);

    let objects = {
        int32: { 0: 1, 1: 2 },
        contiguous: { 0: "a", 1: { } },
        double: { 0: 1.5, 1: 2.5 },
        sparse: { 0: 1, 100000: 2 },
    };
    for (let [name, object] of Object.entries(objects))
        check(decorate(object), decoration, "object " + name);

    shouldBeKeys($vm.ownNonIndexKeys([]), ["length"], "empty array");
    shouldBeKeys($vm.ownNonIndexKeys({ }), [], "empty object");
    shouldBeKeys($vm.ownNonIndexKeys([1, 2, 3], true, false), [], "array, enumerable keys");
}

// String objects: "length" stays the first key.
{
    class DerivedString extends String { }
    let strings = {
        "String object": new String("abc"),
        "Object(string)": Object("abc"),
        "String subclass": new DerivedString("abc"),
    };
    for (let [name, string] of Object.entries(strings)) {
        string[5] = "element past the characters";
        check(decorate(string), ["length", ...decoration], name);
    }
    shouldBeKeys($vm.ownNonIndexKeys(new String("abc"), true, false), [], "String object, enumerable keys");
    shouldBeKeys($vm.ownNonIndexKeys(new String("")), ["length"], "empty String object");
    shouldBe($vm.ownNonIndexKeys(String.prototype).includes("length"), true, "String.prototype has length");
    shouldBe($vm.ownNonIndexKeys(String.prototype).some(isIndexKey), false, "String.prototype");
}

// Typed arrays.
{
    let constructors = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float16Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array];
    for (let constructor of constructors) {
        check(decorate(new constructor(3), typedArrayDecoration), typedArrayDecoration, constructor.name);
        shouldBeKeys($vm.ownNonIndexKeys(new constructor(3)), [], constructor.name + " without properties");
    }
    let resizable = new Uint8Array(new ArrayBuffer(4, { maxByteLength: 16 }));
    check(decorate(resizable, typedArrayDecoration), typedArrayDecoration, "length-tracking Uint8Array");
}

// Arguments objects: a DirectArguments, a ScopedArguments (a closure captures a parameter) and a ClonedArguments.
{
    function direct(a, b) { return arguments; }
    function scoped(a, b) { (function () { return a; }); return arguments; }
    function strict(a, b) { "use strict"; return arguments; }

    for (let make of [direct, scoped, strict]) {
        check(make(1, 2, 3), ["length", "callee", Symbol.iterator], make.name + " arguments");
        check(decorate(make(1, 2, 3)), null, make.name + " arguments with properties");

        // These give the object its own length, callee and Symbol.iterator properties, and unmap an argument.
        let overridden = make(1, 2, 3);
        overridden.length = 7;
        delete overridden[0];
        Object.defineProperty(overridden, 1, { value: "b", enumerable: false });
        overridden[9] = "element past the arguments";
        check(overridden, ["length", "callee", Symbol.iterator], make.name + " arguments after overrides");

        let withoutLength = make(1, 2, 3);
        delete withoutLength.length;
        check(withoutLength, ["callee", Symbol.iterator], make.name + " arguments without length");
    }
}

// A Proxy without an ownKeys trap hands the mode to its target. So does the global proxy.
{
    let targets = {
        array: [1, 2, 3],
        "String object": new String("abc"),
        Uint8Array: new Uint8Array(3),
        arguments: (function () { return arguments; })(1, 2, 3),
        Proxy: new Proxy([1, 2, 3], { }),
    };
    for (let [name, target] of Object.entries(targets)) {
        let all = $vm.ownNonIndexKeys(target);
        check(new Proxy(target, { }), all, "Proxy of " + name);
        shouldBeKeys($vm.ownNonIndexKeys(Object.create(target)), [], "object that inherits from " + name);
    }

    // The enumerable keys of a Proxy: [[GetOwnProperty]] is asked about the keys that were collected, and about no element.
    let asked = [];
    let proxy = new Proxy(decorate([1, 2, 3]), {
        getOwnPropertyDescriptor(target, key) {
            asked.push(key);
            return Reflect.getOwnPropertyDescriptor(target, key);
        },
    });
    shouldBeKeys($vm.ownNonIndexKeys(proxy, true, false), ["named", "4294967295", "01", symbol], "Proxy with getOwnPropertyDescriptor, enumerable keys");
    shouldBeKeys(asked, ["length", ...decoration], "keys the Proxy was asked about");

    globalThis[7] = "element of the global object";
    globalThis.namedGlobal = 1;
    check(globalThis, ownKeysLessIndexKeys(globalThis, true, true), "global object");
    shouldBe($vm.ownNonIndexKeys(globalThis).includes("namedGlobal"), true, "global object has namedGlobal");
    delete globalThis[7];
}

// Only the elements an object holds are left out. A key that an ownKeys trap returns is added as it is.
{
    let log = [];
    let proxy = new Proxy([1, 2, 3], {
        ownKeys(target) {
            log.push("ownKeys");
            return ["0", "1", "2", "length", "named", symbol];
        },
    });
    shouldBeKeys($vm.ownNonIndexKeys(proxy), ["0", "1", "2", "length", "named", symbol], "Proxy with ownKeys");
    shouldBeKeys($vm.ownNonIndexKeys(proxy, false), ["0", "1", "2", "length", "named"], "Proxy with ownKeys, no symbols");
    shouldBeKeys($vm.ownNonIndexKeys(proxy, true, false), ["0", "1", "2"], "Proxy with ownKeys, enumerable keys");
    shouldBeKeys(log, ["ownKeys", "ownKeys", "ownKeys"], "ownKeys calls");

    let { proxy: revoked, revoke } = Proxy.revocable([1, 2, 3], { });
    revoke();
    shouldThrow(() => $vm.ownNonIndexKeys(revoked), TypeError, "revoked Proxy");
    shouldThrow(() => $vm.ownNonIndexKeys(1), TypeError, "not an object");
}

// The modes with index keys give what they gave.
{
    let array = decorate([1, 2, 3]);
    shouldBeKeys(Reflect.ownKeys(array), ["0", "1", "2", "length", ...decoration], "Reflect.ownKeys");
    shouldBeKeys(Object.getOwnPropertyNames(array), ["0", "1", "2", "length", "named", "hiddenNamed", "4294967295", "01"], "Object.getOwnPropertyNames");
    shouldBeKeys(Object.getOwnPropertySymbols(array), [symbol, hidden], "Object.getOwnPropertySymbols");
    shouldBeKeys(Object.keys(array), ["0", "1", "2", "named", "4294967295", "01"], "Object.keys");
    let forIn = [];
    for (let key in array)
        forIn.push(key);
    shouldBeKeys(forIn, ["0", "1", "2", "named", "4294967295", "01"], "for-in");
    shouldBeKeys(Reflect.ownKeys(new String("ab")), ["0", "1", "length"], "Reflect.ownKeys of a String object");
    shouldBeKeys(Object.keys(new Uint8Array(2)), ["0", "1"], "Object.keys of a Uint8Array");
}

// An export of a module that is named like an index is not an element: the namespace object keeps it.
(async function () {
    let namespace = await import("./resources/own-keys-non-index-module-namespace.js");
    shouldBeKeys(Reflect.ownKeys(namespace), ["0", "named", Symbol.toStringTag], "module namespace");
    shouldBeKeys($vm.ownNonIndexKeys(namespace), ["0", "named", Symbol.toStringTag], "module namespace without index keys");
    shouldBeKeys($vm.ownNonIndexKeys(namespace, false), ["0", "named"], "module namespace, no symbols");
}()).catch(abort);
