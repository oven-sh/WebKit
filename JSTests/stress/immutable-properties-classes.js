// The classes with write hooks of their own that support immutable properties: arrays, functions, errors, RegExp objects, String
// objects and unmapped arguments objects. Their lazily materialized properties still appear afterwards.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error((message ? message + ": " : "") + "expected " + errorType.name + " but got " + String(error));
}

function snapshot(object) {
    return JSON.stringify(Reflect.ownKeys(object).map(key => {
        let d = Object.getOwnPropertyDescriptor(object, key);
        return [String(key), "value" in d ? (typeof d.value === "object" || typeof d.value === "function" ? typeof d.value : String(d.value)) : "accessor", d.writable, d.enumerable, d.configurable];
    })) + " length:" + String(object.length);
}

// Arrays. (Every storage kind against every mutator is in immutable-properties-array-storage.js and -array-other-storage.js.)
{
    let array = $vm.makePropertiesImmutable([1, 2, 3]);
    shouldThrow(() => array.push(4), TypeError);
    shouldThrow(() => array.pop(), TypeError);
    shouldThrow(() => { "use strict"; array[0] = 9; }, TypeError);
    shouldThrow(() => { "use strict"; array.length = 0; }, TypeError);
    shouldBe(Reflect.defineProperty(array, "length", { value: 3 }), true);
    shouldBe(Reflect.defineProperty(array, 0, { value: 1 }), true);
    shouldBe(Reflect.defineProperty(array, 0, { value: 2 }), false);
    shouldBe(Reflect.deleteProperty(array, 7), true);
    shouldBe(Reflect.deleteProperty(array, 0), false);
    shouldBe(array.map(x => x * 2).join(), "2,4,6");
    shouldBe([...array].join(), "1,2,3");
    shouldBe($vm.hasImmutableProperties(array.slice()), false);
}

// Functions: name, length and prototype are materialized lazily and still appear.
{
    function declared(a, b) { }
    let arrow = (a) => { };
    let bound = declared.bind(null, 1);
    let native = Math.max;
    class Klass { static s() { } }
    for (let fn of [declared, arrow, bound, native, Klass])
        $vm.makePropertiesImmutable(fn);

    shouldBe(declared.name, "declared");
    shouldBe(declared.length, 2);
    shouldBe(typeof declared.prototype, "object");
    shouldBe(declared.prototype.constructor, declared);
    shouldBe(new declared() instanceof declared, true);
    shouldBe(arrow.name, "arrow");
    shouldBe(arrow.length, 1);
    shouldBe(bound.name, "bound declared");
    shouldBe(bound.length, 1);
    shouldBe(native.name, "max");
    shouldBe(native.length, 2);
    shouldBe(Klass.name, "Klass");
    shouldBe(Object.getOwnPropertyNames(declared).sort().join(), "length,name,prototype");

    for (let fn of [declared, arrow, bound, native, Klass]) {
        shouldThrow(() => { "use strict"; fn.extra = 1; }, TypeError);
        shouldThrow(() => { "use strict"; fn.name = "x"; }, TypeError);
        shouldBe(Reflect.defineProperty(fn, "name", { value: "x" }), false);
        shouldBe(Reflect.defineProperty(fn, "name", { value: fn.name }), true);
        shouldBe(Reflect.deleteProperty(fn, "name"), false);
        shouldBe(Reflect.deleteProperty(fn, "missing"), true);
        shouldBe(Reflect.setPrototypeOf(fn, null), false);
    }
    shouldThrow(() => { "use strict"; declared.prototype = {}; }, TypeError);
    shouldBe(declared.prototype.constructor, declared);
    // The prototype object itself was left alone.
    declared.prototype.method = function () { return 1; };
    shouldBe(new declared().method(), 1);
}

// Errors: line, column, sourceURL and stack are materialized lazily and still appear.
{
    let error = $vm.makePropertiesImmutable(new RangeError("message"));
    shouldBe(typeof error.stack, "string");
    shouldBe(typeof error.line, "number");
    shouldBe(error.message, "message");
    shouldThrow(() => { "use strict"; error.message = "changed"; }, TypeError);
    shouldThrow(() => { "use strict"; error.stack = "changed"; }, TypeError);
    shouldThrow(() => { "use strict"; error.extra = 1; }, TypeError);
    shouldBe(Reflect.defineProperty(error, "stack", { value: "changed" }), false);
    shouldBe(Reflect.deleteProperty(error, "stack"), false);
    shouldBe(Reflect.deleteProperty(error, "message"), false);
    shouldBe(error.message, "message");
    shouldBe(typeof error.stack, "string");
}

// RegExp objects: lastIndex becomes non-writable (compiled code tests the object's own flag), so matching that has to update
// it fails as it does for a frozen RegExp; matching that does not need to update it works.
{
    let plain = $vm.makePropertiesImmutable(/b/);
    shouldBe(plain.test("abc"), true);
    shouldBe("abc".replace(plain, "X"), "aXc");
    shouldBe(Object.getOwnPropertyDescriptor(plain, "lastIndex").writable, false);
    shouldThrow(() => { "use strict"; plain.lastIndex = 2; }, TypeError);
    shouldThrow(() => { "use strict"; plain.extra = 1; }, TypeError);
    shouldThrow(() => plain.compile("c"), TypeError);
    shouldBe(plain.source, "b");

    let global = $vm.makePropertiesImmutable(/b/g);
    shouldThrow(() => global.exec("abcb"), TypeError);
    shouldBe(global.lastIndex, 0);
}

// String objects.
{
    let string = $vm.makePropertiesImmutable(new String("ab"));
    shouldBe(string.length, 2);
    shouldBe(string[1], "b");
    shouldThrow(() => { "use strict"; string.extra = 1; }, TypeError);
    shouldThrow(() => { "use strict"; string[5] = "c"; }, TypeError);
    shouldBe(Reflect.defineProperty(string, 5, { value: "c" }), false);
    shouldBe(Reflect.defineProperty(string, 0, { value: "a" }), true);
    shouldBe(Reflect.deleteProperty(string, 0), false);
    shouldBe(Reflect.deleteProperty(string, 5), true);
    shouldBe(String(string), "ab");
}

// Unmapped (strict-mode) arguments objects: callee, Symbol.iterator and length still appear.
{
    let args = (function () { "use strict"; return arguments; })(1, 2, 3);
    $vm.makePropertiesImmutable(args);
    shouldBe(args.length, 3);
    shouldBe([...args].join(), "1,2,3");
    shouldBe(typeof args[Symbol.iterator], "function");
    shouldThrow(() => args.callee, TypeError);
    shouldThrow(() => { "use strict"; args[0] = 9; }, TypeError);
    shouldThrow(() => { "use strict"; args.length = 0; }, TypeError);
    shouldThrow(() => { "use strict"; args.extra = 1; }, TypeError);
    shouldBe(Reflect.deleteProperty(args, 0), false);
    shouldBe(args[0], 1);
}

// Built-in prototypes and constructors: static properties are reified lazily and still appear.
{
    for (let object of [Array.prototype, Array, Math, JSON, Promise.prototype, Promise, RegExp.prototype, Map.prototype, Object])
        $vm.makePropertiesImmutable(object);
    shouldBe([3, 1, 2].toSorted().join(), "1,2,3");
    shouldBe(typeof Array.prototype.findLast, "function");
    shouldBe(Math.hypot(3, 4), 5);
    shouldBe(JSON.stringify([1]), "[1]");
    shouldBe(typeof Promise.allSettled, "function");
    shouldBe(Object.getOwnPropertyNames(Math).includes("fround"), true);
    shouldThrow(() => { "use strict"; Array.prototype.push = function () { }; }, TypeError);
    shouldThrow(() => { "use strict"; Array.prototype[0] = 1; }, TypeError);
    shouldThrow(() => { "use strict"; Math.max = function () { }; }, TypeError);
    shouldThrow(() => { "use strict"; Promise.prototype.then = function () { }; }, TypeError);
    shouldBe(Reflect.defineProperty(Array.prototype, Symbol.iterator, { value: function () { } }), false);
    shouldBe([1, 2, 3].map(x => x + 1).join(), "2,3,4");
    shouldBe([...new Map([[1, 2]])].join(), "1,2");
    let array = [];
    array.push(1);
    array[5] = 2;
    shouldBe(array.length, 6);
    shouldBe(array[3], undefined);
}

// The realm starts "having a bad time" (a Proxy enters a prototype chain, or an indexed accessor appears on a prototype): every
// array, including Array.prototype, which has no elements, is converted to slow-put storage. That conversion is the engine's own.
{
    let empty = $vm.makePropertiesImmutable([]);
    let filled = $vm.makePropertiesImmutable([1, 2, 3]);
    let arrayLike = $vm.makePropertiesImmutable(Object.assign(function () { }, { 0: "zero" }));
    $vm.makePropertiesImmutable(Array.prototype);
    let root = {};
    let leaf = Object.create(new Proxy(Object.create(root), {}));
    root.__proto__ = leaf;
    shouldBe(Object.getPrototypeOf(root), leaf);
    Object.defineProperty(Object.create(Array.prototype), 0, { get() { return 1; } });
    $vm.haveABadTime(globalThis);
    shouldBe($vm.isHavingABadTime(globalThis), true);
    for (let target of [empty, filled, arrayLike, Array.prototype]) {
        shouldBe($vm.hasImmutableProperties(target), true);
        let before = snapshot(target);
        shouldThrow(() => { "use strict"; target[0] = 9; }, TypeError);
        shouldThrow(() => { "use strict"; target[7] = 9; }, TypeError);
        try { Array.prototype.push.call(target, 9); } catch (error) { shouldBe(error instanceof TypeError, true); }
        shouldBe(snapshot(target), before);
    }
    shouldBe([1, 2, 3].map(x => x * 2).join(), "2,4,6");
    let fresh = [];
    fresh[5] = 1;
    shouldBe(fresh.length, 6);
}

// Error.captureStackTrace() defines a stack property and tells an ErrorInstance that its own is materialized: it throws for such an
// object, and an error keeps the stack it had yet to materialize.
{
    let unread = $vm.makePropertiesImmutable(new Error("unread"));
    shouldThrow(() => Error.captureStackTrace(unread), TypeError);
    shouldBe(typeof unread.stack, "string");
    shouldBe(unread.stack.length > 0, true);
    let read = new Error("read"), stack = read.stack;
    $vm.makePropertiesImmutable(read);
    shouldThrow(() => Error.captureStackTrace(read), TypeError);
    shouldBe(read.stack, stack);
    let plain = $vm.makePropertiesImmutable({});
    shouldThrow(() => Error.captureStackTrace(plain), TypeError);
    shouldBe("stack" in plain, false);
    let ordinary = {};
    Error.captureStackTrace(ordinary);
    shouldBe(typeof ordinary.stack, "string");
}

// The one attribute that changes reaches inheritors too: an object that inherits from such a RegExp cannot assign lastIndex, as
// with a frozen RegExp. (Compiled code tests the RegExp's own lastIndex-is-writable flag before it stores lastIndex in place.)
{
    let regExp = $vm.makePropertiesImmutable(/a/);
    let child = Object.create(regExp);
    shouldThrow(() => { "use strict"; child.lastIndex = 5; }, TypeError);
    shouldBe(Object.hasOwn(child, "lastIndex"), false);
    child.other = 1;
    shouldBe(child.other, 1);
}

// A class that inherits from a supported one and brings a write hook of its own is not supported.
shouldThrow(() => $vm.makePropertiesImmutable($vm.createRuntimeArray(1, 2, 3)), TypeError);
