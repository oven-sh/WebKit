// An access is warmed up on an ordinary object, then inlined where its base is a constant that has the same prototype but keeps
// the property outside of its Structure. The access has never executed on that constant.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

let calls = 0;
let inherited = {
    nothing: key => ({ }),
    value: key => ({ [key]: { value: "inherited", writable: true } }),
    getter: key => ({ [key]: { get() { calls++; return "inherited"; } } }),
    setter: key => ({ [key]: { set(value) { calls++; } } }),
};

function test(message, base, parent, key, kind, body, expected) {
    let prototype = Object.create(parent, { marker: { value: 1 }, ...inherited[kind](key) });
    Object.setPrototypeOf(base, prototype);
    for (let i = 0; i < 200; i++)
        base["k" + i] = i;
    let prepare = (0, eval)(`(function (base, ordinary, count) {
        // The DFG does not watch a structure that has been a dictionary. Caching the read of an inherited property flattens it.
        let readMarker = object => object.marker;
        for (let i = 0; i < 100; i++)
            readMarker(base);

        let result;
        let access = (object, value) => { ${body} };
        for (let i = 0; i < count; i++)
            access(ordinary, 1);
        let accessBase = really => {
            if (really)
                access(base, 1);
        };
        for (let i = 0; i < count; i++)
            accessBase(false);
        return () => {
            try {
                accessBase(true);
            } catch (error) {
                result = error.constructor.name;
            }
            return result;
        };
    }) // ${message}`);
    noDFG(prepare); // Its loops would inline what they call, which then would not be compiled on its own.
    let run = prepare(base, Object.create(prototype), testLoopCount);
    calls = 0;
    shouldBe(run(), expected, message);
    shouldBe(calls, 0, message + ", calls of the inherited accessor");
}

let functionPrototype = Function.prototype;
test("function.prototype", function (a, b, c) { }, functionPrototype, "prototype", "nothing", "result = typeof object.prototype;", "object");
test("function.length", function (a, b, c) { }, functionPrototype, "length", "nothing", "result = object.length;", 3);
test("function.name", function named() { }, functionPrototype, "name", "value", "result = object.name;", "named");
test("function.length and a getter", function (a, b, c) { }, functionPrototype, "length", "getter", "result = object.length;", 3);
test("function.length and a setter", function (a, b, c) { }, functionPrototype, "length", "setter", "'use strict'; object.length = value;", "TypeError");
test("bound function.name", (function target() { }).bind(null), functionPrototype, "name", "nothing", "result = object.name;", "bound target");
test("regExp.lastIndex", /a/g, RegExp.prototype, "lastIndex", "nothing", "result = object.lastIndex;", 0);
test("regExp.lastIndex and a setter", /a/g, RegExp.prototype, "lastIndex", "setter", "object.lastIndex = 7; result = object.lastIndex;", 7);
test("array.length", [1, 2, 3], Object.prototype, "length", "nothing", "result = object.length;", 3);
test("array.length and a getter", [1, 2, 3], Object.prototype, "length", "getter", "result = object.length;", 3);
test("array.length and a setter", [1, 2, 3], Object.prototype, "length", "setter", "object.length = value; result = object.length;", 1);
test("string object.length", new String("abc"), Object.prototype, "length", "nothing", "result = object.length;", 3);
test("arguments.length", (function () { return arguments; })(1, 2), Object.prototype, "length", "nothing", "result = object.length;", 2);
