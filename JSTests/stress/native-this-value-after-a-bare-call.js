// A native that is called with no receiver must not be handed the caller's scope
// as |this|.
//
// The call's this register holds the resolved scope (FunctionCallResolveNode),
// because `with (object) { f() }` has to reach f as object. Every other scope
// kind is normalized by the callee: a JS function does it with op_to_this, a C++
// native with JSValue::toThis. A native that reads callFrame->thisValue() raw
// works on the caller's lexical environment instead, so it reads, calls and
// writes the caller's own bindings.

function shouldThrowTypeError(name, f) {
    let error;
    try {
        f();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error(name + " did not throw");
    if (!(error instanceof TypeError))
        throw new Error(name + " threw " + error + ", expected a TypeError");
}

// The natives this test was written for, each with the binding it used to reach.
// The inner closure holds both names, so they live in one heap scope.
function regExpNatives() {
    shouldThrowTypeError("RegExp.prototype.test", function () {
        const test = RegExp.prototype.test;
        const exec = function () { throw new Error("the caller's scope was used as |this|"); };
        const keep = function () { return [test, exec]; };
        keep;
        return test("subject");
    });

    shouldThrowTypeError("RegExp.prototype[Symbol.match]", function () {
        const match = RegExp.prototype[Symbol.match];
        const exec = function () { throw new Error("the caller's scope was used as |this|"); };
        const flags = { toString: function () { throw new Error("the caller's scope was used as |this|"); } };
        const keep = function () { return [match, exec, flags]; };
        keep;
        return match("subject");
    });

    shouldThrowTypeError("RegExp.prototype[Symbol.matchAll]", function () {
        const matchAll = RegExp.prototype[Symbol.matchAll];
        const flags = { toString: function () { throw new Error("the caller's scope was used as |this|"); } };
        const constructor = { [Symbol.species]: function () { throw new Error("the caller's scope was used as |this|"); } };
        const keep = function () { return [matchAll, flags, constructor]; };
        keep;
        return matchAll("subject");
    });

    shouldThrowTypeError("RegExp.prototype[Symbol.replace]", function () {
        const replace = RegExp.prototype[Symbol.replace];
        const exec = function () { throw new Error("the caller's scope was used as |this|"); };
        const flags = { toString: function () { throw new Error("the caller's scope was used as |this|"); } };
        const keep = function () { return [replace, exec, flags]; };
        keep;
        return replace("subject", "replacement");
    });

    shouldThrowTypeError("RegExp.prototype[Symbol.search]", function () {
        const search = RegExp.prototype[Symbol.search];
        const exec = function () { throw new Error("the caller's scope was used as |this|"); };
        const keep = function () { return [search, exec]; };
        keep;
        return search("subject");
    });

    shouldThrowTypeError("RegExp.prototype[Symbol.split]", function () {
        const split = RegExp.prototype[Symbol.split];
        const flags = { toString: function () { throw new Error("the caller's scope was used as |this|"); } };
        const constructor = { [Symbol.species]: function () { throw new Error("the caller's scope was used as |this|"); } };
        const keep = function () { return [split, flags, constructor]; };
        keep;
        return split("subject");
    });
}

function iteratorNatives() {
    shouldThrowTypeError("Iterator.prototype.toArray", function () {
        const toArray = Iterator.prototype.toArray;
        const next = function () { throw new Error("the caller's scope was used as |this|"); };
        const keep = function () { return [toArray, next]; };
        keep;
        return toArray();
    });

    shouldThrowTypeError("Iterator.prototype.forEach", function () {
        const forEach = Iterator.prototype.forEach;
        const next = function () { throw new Error("the caller's scope was used as |this|"); };
        const keep = function () { return [forEach, next]; };
        keep;
        return forEach(function () { });
    });
}

// A custom setter has the same receiver. This one wrote a property of the name
// it is the setter for, which is a binding of the caller.
function nativeSetter() {
    const set = Object.getOwnPropertyDescriptor(Iterator.prototype, "constructor").set;
    let constructor = "the caller's binding";
    const keep = function () { return [set, constructor]; };
    keep;
    let error;
    try {
        set("written by the native");
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error("the Iterator.prototype.constructor setter: expected a TypeError, got " + error);
    if (constructor !== "the caller's binding")
        throw new Error("the Iterator.prototype.constructor setter wrote the caller's binding");
}

// A binding that is still in its temporal dead zone holds the empty JSValue. A
// native that reads it off the scope dereferences null.
function deadZoneBinding() {
    const test = RegExp.prototype.test;
    const keep = function () { return [test, exec]; };
    keep;
    let error;
    try {
        test("subject");
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error("a binding in its dead zone: expected a TypeError, got " + error);
    const exec = function () { };
}

for (let i = 0; i < testLoopCount; i++) {
    regExpNatives();
    iteratorNatives();
    nativeSetter();
    deadZoneBinding();
}

// What the scope in the this register is for: `with` still reaches its object,
// and every other bare call keeps the ordinary receiver.
function withStatement() {
    const object = {
        sloppy: function () { return this; },
        strict: function () { "use strict"; return this; },
    };
    with (object) {
        if (sloppy() !== object)
            throw new Error("a sloppy callee lost the with object");
        if (strict() !== object)
            throw new Error("a strict callee lost the with object");
        if (eval("sloppy()") !== object)
            throw new Error("a callee reached through eval lost the with object");
    }

    const regExp = /subject/;
    const holder = { test: RegExp.prototype.test, exec: regExp.exec.bind(regExp) };
    with (holder) {
        if (test("subject") !== true)
            throw new Error("a native lost the with object");
    }
}
withStatement();

function bareCall() {
    const sloppy = function () { return this; };
    const strict = function () { "use strict"; return this; };
    const keep = function () { return [sloppy, strict]; };
    keep;
    if (sloppy() !== globalThis)
        throw new Error("a sloppy callee did not get the global object");
    if (strict() !== undefined)
        throw new Error("a strict callee did not get undefined");
}
bareCall();

// Every native that is reachable from a prototype, swept: a native that is added
// later and forgets to normalize its receiver fails here. Every binding such a
// native might look up is declared, and each one reports that it was reached.
// They are all `let`, so a write by the native lands instead of being rejected
// as an assignment to a constant.
let reached = [];
function probe(nativeArgument) {
    const native = nativeArgument;
    const record = function (name) { reached.push(name); };
    let exec = function () { record("read exec"); return null; };
    let next = function () { record("read next"); return { done: true }; };
    let callback = function () { record("read callback"); };
    let flags = { toString: function () { record("read flags"); return ""; } };
    let source = { toString: function () { record("read source"); return "a"; } };
    let lastIndex = { valueOf: function () { record("read lastIndex"); return 0; } };
    let length = { valueOf: function () { record("read length"); return 0; } };
    let size = { valueOf: function () { record("read size"); return 0; } };
    let index = { valueOf: function () { record("read index"); return 0; } };
    let value = { valueOf: function () { record("read value"); return 0; } };
    let name = { toString: function () { record("read name"); return "n"; } };
    let raw = { valueOf: function () { record("read raw"); return 0; } };
    let prototype = { marker: true };
    let constructor = {
        [Symbol.species]: function () {
            record("read constructor");
            return { lastIndex: 0, exec: function () { return null; } };
        },
    };
    const names = ["exec", "next", "callback", "flags", "source", "lastIndex", "length", "size", "index", "value", "name", "raw", "prototype", "constructor"];
    const before = [exec, next, callback, flags, source, lastIndex, length, size, index, value, name, raw, prototype, constructor];
    const keep = function () {
        return [native, exec, next, callback, flags, source, lastIndex, length, size, index, value, name, raw, prototype, constructor];
    };

    try {
        native(callback, "replacement");
    } catch (e) { }
    try {
        native("subject", "replacement");
    } catch (e) { }

    const after = [exec, next, callback, flags, source, lastIndex, length, size, index, value, name, raw, prototype, constructor];
    for (let i = 0; i < before.length; i++) {
        if (before[i] !== after[i])
            record("wrote " + names[i]);
    }
    keep;
}

function sweep() {
    const prototypes = [];
    for (const name of Object.getOwnPropertyNames(globalThis)) {
        let value;
        try {
            value = globalThis[name];
        } catch (e) {
            continue;
        }
        if (typeof value === "function" && value.prototype)
            prototypes.push([name + ".prototype", value.prototype]);
    }
    prototypes.push(["TypedArray.prototype", Object.getPrototypeOf(Int8Array.prototype)]);
    prototypes.push(["ArrayIterator.prototype", Object.getPrototypeOf([][Symbol.iterator]())]);
    prototypes.push(["StringIterator.prototype", Object.getPrototypeOf(""[Symbol.iterator]())]);
    prototypes.push(["MapIterator.prototype", Object.getPrototypeOf(new Map()[Symbol.iterator]())]);
    prototypes.push(["RegExpStringIterator.prototype", Object.getPrototypeOf("a".matchAll(/a/g))]);

    const seen = new Set();
    const found = [];
    for (const [prototypeName, prototype] of prototypes) {
        if (!prototype || seen.has(prototype))
            continue;
        seen.add(prototype);
        for (const key of Reflect.ownKeys(prototype)) {
            let descriptor;
            try {
                descriptor = Object.getOwnPropertyDescriptor(prototype, key);
            } catch (e) {
                continue;
            }
            if (!descriptor)
                continue;
            const natives = [];
            if (typeof descriptor.value === "function" && key !== "constructor")
                natives.push(["", descriptor.value]);
            if (typeof descriptor.get === "function")
                natives.push([" getter", descriptor.get]);
            if (typeof descriptor.set === "function")
                natives.push([" setter", descriptor.set]);
            for (const [kind, native] of natives) {
                reached = [];
                try {
                    probe(native);
                } catch (e) {
                    continue;
                }
                if (reached.length)
                    found.push(prototypeName + "." + String(key) + kind + " reached " + reached.join(", "));
            }
        }
    }
    if (found.length)
        throw new Error("a native worked on the caller's scope:\n" + found.join("\n"));
}
sweep();
